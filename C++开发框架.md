# 二手交易系统 · C++ 开发框架

> 依据 `requirements.md` 与 `UML建模分析.md` 整理，目标是一个**能直接开工的 C++ 控制台原型骨架**。
> 技术约定：C++17、单进程、无第三方库、数据放内存（重启清空）。

---

## 1. 框架怎么搭

```
┌──────────────────────────────┐
│  UI 层  ui/ConsoleUI         │  只负责菜单、输入输出、打印错误
├──────────────────────────────┤
│  服务层 service/*Service     │  业务规则、权限、状态流转、加锁、通知、审计
├──────────────────────────────┤
│  存储层 Store                │  所有实体的 map + 一把互斥锁
├──────────────────────────────┤
│  模型层 model/               │  实体 struct + 枚举，纯数据
└──────────────────────────────┘
   common/：Result、Clock、Id、密码哈希、脱敏
   scheduler/：定时扫描超时（24h / 2h / 7d / 30d）
```

五条硬规则：

1. **UI 不碰 Store**，只调用 Service，不自己判断权限（服务端鉴权，REQ-017）。
2. **Service 是唯一改状态的地方**；每个方法固定套路：校验参数 → 查角色 → 加锁 → 查实体 → 校验状态/版本 → 改数据 → 发通知/写审计 → 返回 `Result`。
3. **实体是纯数据**，只放少量自身判断（如 `canReserve()`），不放跨对象业务。
4. **跨对象操作在同一把锁里完成**（扣库存 + 建预约必须原子，REQ-019）。
5. **时间一律用 `Clock`**，不要直接 `chrono::now()`，否则超时没法测。

---

## 2. 目录结构

```
secondhand/
├── CMakeLists.txt
├── src/
│   ├── main.cpp                 # 组装依赖、启动调度线程、进入 UI
│   ├── common/
│   │   ├── Result.h             # Result<T> / Status / 错误码
│   │   ├── Clock.h              # Clock / SystemClock / FakeClock
│   │   ├── Hash.h               # 密码哈希、手机号/姓名脱敏
│   │   └── Role.h               # 角色位标志与 hasRole()
│   ├── model/
│   │   ├── Enums.h              # 所有枚举
│   │   ├── Account.h  Address.h
│   │   ├── Product.h  Category.h  ReviewApplication.h
│   │   ├── Reservation.h  Order.h  EscrowRecord.h  ReturnRequest.h
│   │   ├── Conversation.h  Message.h  Notification.h  ComplaintTicket.h
│   │   └── AuditLog.h  Rating.h  ViewRecord.h  ScoreConfig.h
│   ├── repository/
│   │   └── Store.h              # 内存存储（相当于仓储层）
│   ├── service/
│   │   ├── AuthService.h/.cpp
│   │   ├── ProductService.h/.cpp
│   │   ├── TradeService.h/.cpp       # 预约 + 订单 + 托管
│   │   ├── ReturnService.h/.cpp
│   │   ├── SearchService.h/.cpp
│   │   ├── MessageService.h/.cpp
│   │   ├── ModerationService.h/.cpp  # 举报工单、管理员查私信
│   │   ├── NotificationService.h/.cpp
│   │   └── AuditService.h/.cpp
│   ├── scheduler/
│   │   └── Scheduler.h/.cpp
│   └── ui/
│       └── ConsoleUI.h/.cpp
└── tests/
    └── test_main.cpp
```

> 内存存储只是"仓储层的一种实现"。以后换 MySQL，只需替换 `Store`，Service 的方法签名不变。

---

## 3. 类应该有什么

### 3.1 实体类（model/）

| 类 | 关键字段 | 说明 |
|---|---|---|
| `Account` 账号 | id、username、phone、salt、pwdHash、roleMask、failCount、lockUntil、balance | 一个账号可同时是买家+卖家；管理员与二者互斥；5 次输错锁 15 分钟 |
| `Address` 收货地址 | id、accountId、receiver、phone、region、detail | 输出时脱敏 |
| `Product` 商品 | id、sellerId、title、description、category、price、totalStock、remaining、status、version、lastSavedAt、publishedAt、sales30d | `remaining` 是并发扣减核心；`version` 是乐观锁 |
| `Category` 分类 | id、name、avgRating | 无评价时取分类均值 |
| `ReviewApplication` 审核申请 | id、productId、sellerId、type（新发布/修改）、snapshot、oldSnapshot、status、rejectReason、submitAt、reviewerId | 修改审核期间用 `oldSnapshot` 回滚 |
| `Reservation` 预约 | id、productId、buyerId、orderId、price、status、createdAt、confirmedAt、version | 独立对象，有超时 |
| `Order` 订单 | id、reservationId、productId、buyerId、sellerId、amount、status、payDeadline、paidAt、shippedAt、receivedAt、trackingNo | 卖家确认时创建，付款后进入托管 |
| `EscrowRecord` 托管记录 | id、orderId、amount、status、heldAt、releasedAt、refundedAt | 资金状态与订单状态分开 |
| `ReturnRequest` 退货申请 | id、orderId、buyerId、reason、status、trackingNo、applyAt、approveAt、shipDeadline、deliveredAt、receivedAt | 同一订单同时最多一条进行中 |
| `Conversation` 会话 | id、productId、buyerId、sellerId | 按（商品、买家、卖家）唯一 |
| `Message` 消息 | id、conversationId、senderId、kind（文字/图片）、content、sentAt | 保留 30 天 |
| `Notification` 通知 | id、accountId、type、content、read、createdAt | 与聊天消息区分 |
| `ComplaintTicket` 举报工单 | id、reporterId、productId、reason、status、handlerId | 管理员查私信的授权凭证 |
| `AuditLog` 审计日志 | id、actorId、action、targetType、targetId、result、at | 所有敏感操作都要写 |
| `Rating` 评价 | id、orderId、productId、buyerId、score、content、at | 排序数据来源（可后补） |
| `ViewRecord` 浏览记录 | id、productId、viewerId、at | 24 小时去重（可后补） |
| `ScoreConfig` 排序权重 | match=0.5、rating=0.2、views=0.1、sales=0.2 | 可配置 |

### 3.2 枚举（model/Enums.h）

```cpp
enum class Role : unsigned { Buyer = 1, Seller = 2, Admin = 4 };

enum class ProductStatus     { Draft, PendingReview, Published, SoldOut, OffShelf, Hidden, Rejected };
enum class ReviewStatus      { Pending, Approved, Rejected };
enum class ReservationStatus { Pending, Confirmed, Paid, Completed, Cancelled };
enum class OrderStatus       { PendingPayment, Paid, Shipped, Completed, Closed };
enum class EscrowStatus      { Held, Released, Refunded };
enum class ReturnStatus      { Pending, Approved, Shipped, Delivered, Received, Refunded, Rejected, Closed };
enum class MessageKind       { Text, Image };
```

### 3.3 服务类（service/）

| 服务 | 关键方法 | 对应需求 |
|---|---|---|
| `AuthService` | `registerUser`、`login`、`logout`、`requireRole` | 003 / 004 / 027 |
| `ProductService` | `saveDraft`、`openDraft`、`submitForReview`、`review`、`editPublished`、`takeDown`、`hide`、`restore`、`sweepDrafts` | 007~010 / 025 / 026 |
| `TradeService` | `reserve`、`cancelReservation`、`confirmReservation`、`pay`、`ship`、`confirmReceipt`、`sweepTimeouts` | 001 / 012 / 014 / 015 / 019 |
| `ReturnService` | `apply`、`review`、`submitTracking`、`confirmDelivered`、`confirmReceived`、`sweepTimeouts` | 016 |
| `SearchService` | `search`、`recordView` | 011 / 013 |
| `MessageService` | `openConversation`、`send`、`list` | 005 / 006 |
| `ModerationService` | `createTicket`、`handleTicket`、`viewConversationWithTicket` | 005 |
| `NotificationService` | `send`、`list`、`markRead` | 多处 |
| `AuditService` | `log`、`list` | 017 |

> 服务再拆细（审核服务、订单服务、托管服务）也可以，但**原型阶段 `TradeService` 一个就够**，状态流转集中反而好写。

### 3.4 UI 类

- `ConsoleUI`：持有所有 Service，负责登录 → 按角色进菜单。
- 买家菜单：搜索 / 预约 / 我的预约 / 取消 / 付款 / 确认收货 / 申请退货 / 消息。
- 卖家菜单：草稿箱 / 提交审核 / 确认预约 / 发货 / 处理退货 / 消息。
- 管理员菜单：审核商品 / 下架 / 违规隐藏 / 退货审核 / 工单查看会话 / 审计日志。
- UI 只做三件事：收输入、调服务、打印 `Result` 的 `msg`。

---

## 4. 关键代码怎么写

### 4.1 统一返回值（common/Result.h）

```cpp
#pragma once
#include <string>
#include <utility>

enum class Err {
    Ok = 0, InvalidArg, Unauthorized, Forbidden, NotFound,
    InvalidState, VersionConflict, OutOfStock,
    DuplicateReservation, Locked, LimitExceeded, Expired
};

template <class T>
struct Result {
    Err code = Err::Ok;
    std::string msg;
    T value{};
    bool ok() const { return code == Err::Ok; }
    static Result good(T v) { return {Err::Ok, "", std::move(v)}; }
    static Result fail(Err c, std::string m) { return {c, std::move(m), {}}; }
};

struct Status {                       // 无返回值的版本
    Err code = Err::Ok;
    std::string msg;
    bool ok() const { return code == Err::Ok; }
    static Status good() { return {}; }
    static Status fail(Err c, std::string m) { return {c, std::move(m)}; }
};
```

好处：不用异常，失败原因（`VersionConflict`、`OutOfStock`）直接回给 UI，满足"失败必须给出明确错误码"。

### 4.2 可注入时钟（common/Clock.h）

```cpp
#pragma once
#include <chrono>

class Clock {
public:
    using TimePoint = std::chrono::system_clock::time_point;
    virtual ~Clock() = default;
    virtual TimePoint now() const = 0;
};

class SystemClock : public Clock {
public:
    TimePoint now() const override { return std::chrono::system_clock::now(); }
};

class FakeClock : public Clock {      // 测试超时用
public:
    explicit FakeClock(TimePoint t) : t_(t) {}
    TimePoint now() const override { return t_; }
    void advance(std::chrono::seconds d) { t_ += d; }
private:
    TimePoint t_;
};
```

### 4.3 实体示例（model/Product.h）

```cpp
#pragma once
#include <string>
#include "common/Clock.h"
#include "model/Enums.h"

struct Product {
    long long id = 0;
    long long sellerId = 0;
    std::string title, description, category;
    double price = 0;
    int totalStock = 0;
    int remaining = 0;
    ProductStatus status = ProductStatus::Draft;
    int version = 1;                                  // 乐观锁 / 旧页面
    Clock::TimePoint lastSavedAt{}, publishedAt{};
    int sales30d = 0;

    bool canReserve() const {
        return status == ProductStatus::Published && remaining > 0;
    }
};
```

### 4.4 内存存储（repository/Store.h）

**一把大锁**是这个原型最重要的简化：所有服务共用一个 `Store`，锁住它就能保证"检查 + 扣减 + 写入"原子完成，作业里的 100 并发预约不会超卖。

```cpp
#pragma once
#include <mutex>
#include <string>
#include <unordered_map>
#include "model/Account.h"
#include "model/Product.h"
// ...其余实体 include

struct Store {
    std::recursive_mutex mtx;          // 一把大锁，简单且不会死锁
    long long nextId = 1;
    long long newId() { return nextId++; }   // 只在持锁时调用

    std::unordered_map<long long, Account> accounts;
    std::unordered_map<long long, Product> products;
    std::unordered_map<long long, ReviewApplication> reviewApps;
    std::unordered_map<long long, Reservation> reservations;
    std::unordered_map<long long, Order> orders;
    std::unordered_map<long long, EscrowRecord> escrows;
    std::unordered_map<long long, ReturnRequest> returns;
    std::unordered_map<long long, Conversation> conversations;
    std::unordered_map<long long, Message> messages;
    std::unordered_map<long long, Notification> notifications;
    std::unordered_map<long long, ComplaintTicket> tickets;
    std::unordered_map<long long, AuditLog> audits;
    std::unordered_map<long long, Rating> ratings;
    std::unordered_map<long long, ViewRecord> views;

    std::unordered_map<std::string, long long> usernameIndex;  // 唯一性用
    std::unordered_map<std::string, long long> phoneIndex;
};
```

### 4.5 服务方法模板（以预约为例）

```cpp
Result<long long> TradeService::reserve(long long buyerId, long long productId, int seenVersion) {
    std::lock_guard<std::recursive_mutex> lk(store_.mtx);

    // 1) 身份与角色
    auto acc = store_.accounts.find(buyerId);
    if (acc == store_.accounts.end())
        return Result<long long>::fail(Err::Unauthorized, "未登录");
    if (!hasRole(acc->second.roleMask, Role::Buyer))
        return Result<long long>::fail(Err::Forbidden, "当前账号不是买家");

    // 2) 商品存在
    auto it = store_.products.find(productId);
    if (it == store_.products.end())
        return Result<long long>::fail(Err::NotFound, "商品不存在");
    Product& p = it->second;

    // 3) 旧页面：页面带回来的 version 必须是最新的
    if (p.version != seenVersion)
        return Result<long long>::fail(Err::VersionConflict, "页面已过期，请刷新后重试");

    // 4) 业务校验
    if (p.sellerId == buyerId)
        return Result<long long>::fail(Err::Forbidden, "不能预约自己发布的商品");
    if (p.remaining == 0)
        return Result<long long>::fail(Err::OutOfStock, "商品已售罄");
    if (!p.canReserve())
        return Result<long long>::fail(Err::InvalidState, "商品当前不可预约");
    for (const auto& [id, r] : store_.reservations) {
        if (r.buyerId == buyerId && r.productId == productId &&
            (r.status == ReservationStatus::Pending || r.status == ReservationStatus::Confirmed))
            return Result<long long>::fail(Err::DuplicateReservation, "你已预约过该商品");
    }

    // 5) 改数据：扣库存 + 生成预约（同一把锁内完成）
    --p.remaining;
    if (p.remaining == 0) p.status = ProductStatus::SoldOut;
    ++p.version;

    Reservation r;
    r.id        = store_.newId();
    r.productId = productId;
    r.buyerId   = buyerId;
    r.price     = p.price;
    r.status    = ReservationStatus::Pending;
    r.createdAt = clock_.now();
    store_.reservations[r.id] = r;

    // 6) 通知 + 审计
    notif_.send(p.sellerId, "新预约", "商品《" + p.title + "》收到一个预约");
    audit_.log(buyerId, "reserve", "product", productId, "ok");
    return Result<long long>::good(r.id);
}
```

取消预约时要把库存还回去（注意状态从售罄回到已发布）：

```cpp
void TradeService::releaseStock(Product& p) {
    ++p.remaining;
    if (p.status == ProductStatus::SoldOut) p.status = ProductStatus::Published;
    ++p.version;
}
```

卖家确认 → 生成待付款订单（2 小时付款截止）：

```cpp
Status TradeService::confirmReservation(long long sellerId, long long rid) {
    std::lock_guard<std::recursive_mutex> lk(store_.mtx);
    auto it = store_.reservations.find(rid);
    if (it == store_.reservations.end()) return Status::fail(Err::NotFound, "预约不存在");
    Reservation& r = it->second;
    Product& p = store_.products.at(r.productId);

    if (p.sellerId != sellerId) return Status::fail(Err::Forbidden, "无权确认");
    if (r.status != ReservationStatus::Pending) return Status::fail(Err::InvalidState, "预约状态不允许确认");
    auto now = clock_.now();
    if (now >= r.createdAt + std::chrono::hours(24)) {
        cancelLocked(r, "卖家 24 小时未确认");
        return Status::fail(Err::Expired, "预约已超时自动取消");
    }

    r.status = ReservationStatus::Confirmed;
    r.confirmedAt = now;

    Order o;
    o.id = store_.newId();
    o.reservationId = r.id;
    o.productId = r.productId;
    o.buyerId = r.buyerId;
    o.sellerId = sellerId;
    o.amount = r.price;
    o.status = OrderStatus::PendingPayment;
    o.payDeadline = now + std::chrono::hours(2);
    store_.orders[o.id] = o;
    r.orderId = o.id;

    notif_.send(o.buyerId, "卖家已确认", "请在 2 小时内完成付款");
    return Status::good();
}
```

付款（进入平台托管）：

```cpp
Status TradeService::pay(long long buyerId, long long orderId) {
    std::lock_guard<std::recursive_mutex> lk(store_.mtx);
    auto& o = store_.orders.at(orderId);
    if (o.buyerId != buyerId) return Status::fail(Err::Forbidden, "无权支付");
    if (o.status != OrderStatus::PendingPayment) return Status::fail(Err::InvalidState, "订单不可支付");
    auto now = clock_.now();
    if (now >= o.payDeadline) { closeUnpaid(o); return Status::fail(Err::Expired, "付款超时，订单已关闭"); }

    Account& buyer = store_.accounts.at(buyerId);
    if (buyer.balance < o.amount) return Status::fail(Err::InvalidArg, "余额不足");
    buyer.balance -= o.amount;

    EscrowRecord e;
    e.id = store_.newId(); e.orderId = o.id; e.amount = o.amount;
    e.status = EscrowStatus::Held; e.heldAt = now;
    store_.escrows[e.id] = e;

    o.status = OrderStatus::Paid;
    o.paidAt = now;
    store_.reservations.at(o.reservationId).status = ReservationStatus::Paid;

    notif_.send(o.sellerId, "买家已付款", "货款已托管，请在 7 天内发货");
    return Status::good();
}
```

### 4.6 定时任务（scheduler/Scheduler.h）

超时规则只有四条，全部放在 `sweep` 里：

```cpp
void TradeService::sweepTimeouts() {
    std::lock_guard<std::recursive_mutex> lk(store_.mtx);
    auto now = clock_.now();

    for (auto& [id, r] : store_.reservations) {
        if (r.status == ReservationStatus::Pending &&
            now >= r.createdAt + std::chrono::hours(24))
            cancelLocked(r, "24 小时卖家未确认");
    }
    for (auto& [id, o] : store_.orders) {
        if (o.status == OrderStatus::PendingPayment && now >= o.payDeadline)
            closeUnpaid(o);                                       // 2h 未付款：关单 + 还库存
        else if (o.status == OrderStatus::Paid &&
                 now >= o.paidAt + std::chrono::hours(24 * 7))
            closeAndRefund(o, "卖家 7 天未发货");                 // 退款给买家
        else if (o.status == OrderStatus::Shipped &&
                 now >= o.shippedAt + std::chrono::hours(24 * 7))
            completeAndRelease(o, "买家超期未确认，自动收货");      // 打款给卖家
    }
}

class Scheduler {
public:
    void start() {
        running_ = true;
        th_ = std::thread([this] {
            while (running_) {
                trade_.sweepTimeouts();
                product_.sweepDrafts();     // 27 天提醒、30 天删除
                ret_.sweepTimeouts();       // 审核通过 7 天未寄回
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        });
    }
    void stop() { running_ = false; if (th_.joinable()) th_.join(); }
    void tickOnce() {                 // 测试里可手动调用
        trade_.sweepTimeouts();
        product_.sweepDrafts();
        ret_.sweepTimeouts();
    }
private:
    TradeService& trade_;
    ProductService& product_;
    ReturnService& ret_;
    std::atomic<bool> running_{false};
    std::thread th_;
};
```

### 4.7 组装（main.cpp）

```cpp
int main() {
    SystemClock clock;
    Store store;

    AuditService audit(store);
    NotificationService notif(store);
    AuthService auth(store, clock, audit);
    ProductService product(store, clock, notif, audit);
    TradeService trade(store, clock, notif, audit);
    ReturnService ret(store, clock, notif, audit);
    SearchService search(store, clock);
    MessageService msg(store, clock, audit);
    ModerationService mod(store, audit);
    Scheduler scheduler(trade, product, ret);

    seedAdmin(store, auth);        // 预置管理员，不开放注册
    scheduler.start();

    ConsoleUI ui(auth, product, trade, ret, search, msg, mod, notif, audit);
    ui.run();

    scheduler.stop();
    return 0;
}
```

依赖通过构造函数注入，Service 内部只依赖 `Store&`、`Clock&` 和通知/审计，方便替换和测试。

### 4.8 构建（CMakeLists.txt）

```cmake
cmake_minimum_required(VERSION 3.16)
project(secondhand CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB_RECURSE CORE_SRC CONFIGURE_DEPENDS src/service/*.cpp src/scheduler/*.cpp)
add_library(core STATIC ${CORE_SRC})
target_include_directories(core PUBLIC src)

add_executable(app src/main.cpp src/ui/ConsoleUI.cpp)
target_link_libraries(app PRIVATE core)

enable_testing()
add_executable(tests tests/test_main.cpp)
target_link_libraries(tests PRIVATE core)
add_test(NAME all COMMAND tests)
```

没有 CMake 时，Git Bash 下：

```bash
g++ -std=c++17 -Isrc src/main.cpp src/service/*.cpp src/scheduler/*.cpp src/ui/*.cpp -o app.exe
```

---

## 5. 状态机（照着写 if / switch）

### 商品

| 当前状态 | 操作 | 新状态 |
|---|---|---|
| 草稿 / 审核驳回 | 提交审核 | 待审核 |
| 待审核 | 管理员通过 | 已发布 |
| 待审核 | 管理员驳回 | 草稿（记驳回理由） |
| 已发布 | 卖家修改 | 待审核（审核期间不可见，旧内容仍在线） |
| 待审核（修改） | 管理员驳回 | 已发布（回滚旧内容） |
| 已发布 | 余量被约满 | 售罄 |
| 售罄 | 取消/超时释放预约 | 已发布 |
| 已发布 / 售罄 | 卖家下架 | 已下架 |
| 已发布 / 售罄 | 管理员判违规 | 违规隐藏 |
| 已下架 | 卖家重新提交 | 待审核 |
| 违规隐藏 | 管理员恢复 | 已发布 |

### 预约 / 订单

| 对象 | 流转 |
|---|---|
| 预约 | 待确认 →（卖家确认）→ 已确认 →（付款）→ 已付款 → 已完成 |
| 预约 | 待确认 →（买家取消 / 24h 超时）→ 已取消（还库存） |
| 预约 | 已确认 →（2h 未付款）→ 已取消（还库存） |
| 订单 | 待付款 →（付款）→ 已付款（托管）→（发货）→ 已发货 →（确认收货 / 7d 自动）→ 已完成（打款卖家） |
| 订单 | 待付款 →（2h 超时）→ 已关闭（还库存） |
| 订单 | 已付款 →（7d 未发货）→ 已关闭（退款买家） |

### 退货

| 当前 | 操作 | 新状态 |
|---|---|---|
| 待审核 | 管理员通过 / 驳回 | 待寄回 / 已驳回 |
| 待寄回 | 买家填快递单号（7 天内） | 已寄回 |
| 待寄回 | 7 天未填 | 已关闭（交易维持） |
| 已寄回 | 管理员确认送达 | 已送达 |
| 已送达 | 卖家确认收到 | 已收货 → 退款买家 |

> 每次状态变化都要：`对象.version++`、写 `AuditLog`、给相关方发 `Notification`。

---

## 6. 并发与旧页面（REQ-019）

1. **一把锁罩住整个"检查 + 修改"**。先查后扣（查完释放锁再扣）必然超卖，这是最常见的错误。
2. **乐观锁 = version**：UI 打开商品/草稿时记下 `version`，写操作带回 `seenVersion`；不相等返回 `VersionConflict`，UI 提示"页面已过期"并刷新。
3. **重新打开同一草稿**：`saveDraft(id, content, seenVersion)` 版本旧的那个窗口保存被拒，实现"旧页面失效"；需要"接管编辑"就重新 `openDraft` 取最新版本。
4. **锁内只做内存操作**：不要打印、不要 sleep、不要做网络/文件 I/O。
5. **失败必须带错误码**：`OutOfStock`、`DuplicateReservation`、`VersionConflict`……禁止静默失败。
6. 服务方法接收 `userId`，但**权限一律在服务层查**，不依赖 UI 菜单（防止绕过前端）。

---

## 7. 新增一个功能怎么写（五步）

1. **model**：加实体字段或枚举值（新状态先加进 `Enums.h`）。
2. **Store**：需要新表就加一个 `unordered_map`。
3. **Service**：新增方法，按固定模板写：

   ```cpp
   Result<X> Service::doSomething(long long userId, ...) {
       加锁;
       查账号 / 校验角色;
       查实体 / 校验存在;
       校验状态、版本、业务前置条件;
       修改数据 + version++;
       发通知 / 写审计;
       返回 Result;
   }
   ```

4. **UI**：对应角色菜单加一项，打印 `Result` 的 `msg`。
5. **测试**：正常流 + 边界（库存 0、重复、超时）+ 越权 + 并发。

---

## 8. 建议开发顺序

| 阶段 | 内容 | 里程碑 |
|---|---|---|
| M1 | Result / Clock / Store / main；注册、登录、角色校验 | 能登录并区分三种角色 |
| M2 | 草稿箱 → 提交审核 → 发布 / 驳回 / 下架 / 隐藏 | 商品状态机跑通 |
| M3 | 预约 → 取消 → 卖家确认 → 付款 | 主交易链跑通 |
| M4 | 超时扫描 + 托管 → 发货 → 收货 | 24h / 2h / 7d 自动流转 |
| M5 | 退货、会话消息、举报工单、审计 | 辅助流程补齐 |
| M6 | 搜索排序、并发测试、截图写报告 | 可演示 |

---

## 9. 测试清单（最少这些）

| 测试 | 期望 |
|---|---|
| 100 个线程抢 1 件库存 | 成功 1、失败 99、余量 0，重复 10 轮 |
| 同一买家并发预约同一商品 10 次 | 只生成 1 条预约 |
| 用旧 `version` 预约 / 保存草稿 | 返回 `VersionConflict` |
| 预约后买家取消 | 余量 +1；售罄恢复为已发布 |
| `FakeClock` +24h 卖家未确认 | 预约取消、库存恢复、双方有通知 |
| 确认后 2h 未付款 | 预约取消、订单关闭、库存恢复 |
| 付款后 7 天未发货 | 订单关闭、托管退款买家 |
| 收货后 | 托管打款卖家，订单完成 |
| 买家 / 卖家 / 管理员越权调用 | 100% 返回 `Forbidden` |
| 连续输错密码 5 次 | 锁定 15 分钟，提示不区分原因 |
| 第 11 篇草稿 | 拒绝并提示删除旧草稿 |
| 草稿 27 天 / 30 天 | 提醒 / 自动删除 |

并发测试骨架：

```cpp
std::atomic<int> success{0};
std::vector<std::thread> ts;
for (int i = 0; i < 100; ++i)
    ts.emplace_back([&, i] {
        if (trade.reserve(buyerIds[i % buyerIds.size()], pid, version).ok())
            ++success;
    });
for (auto& t : ts) t.join();
assert(success == 1);
assert(store.products.at(pid).remaining == 0);
```

---

## 10. 需求与类对照

| 需求 | 主要落点 |
|---|---|
| REQ-001 / 012 / 013 / 014 | `Product`、`Reservation`、`TradeService::reserve/cancel/confirm/pay` |
| REQ-003 / 004 / 027 | `Account`、`AuthService`、角色位标志、登录锁定 |
| REQ-005 / 006 | `Conversation`、`Message`、`ComplaintTicket`、`MessageService`、`ModerationService` |
| REQ-007 / 008 / 009 / 026 | `Product` 状态机、`ReviewApplication`、`ProductService` |
| REQ-010 / 025 | `ProductService::takeDown/hide/restore`、状态-角色可见性 |
| REQ-011 | `SearchService`、`Rating`、`ViewRecord`、`ScoreConfig` |
| REQ-015 / 016 | `Order`、`EscrowRecord`、`ReturnRequest`、`ReturnService` |
| REQ-017 | Service 层鉴权、`AuditService`、DTO 脱敏（UI 输出前调用 `mask*`） |
| REQ-018 | Web / APP 共用同一套 Service/API；原型中用同一份 ConsoleUI 模拟 |
| REQ-019 | `Store.mtx` + 实体 `version` + 页面版本校验 |

---

## 11. 几条不要做的事

- 不要为买家、卖家、管理员各建一个用户子类——用一个 `Account` + `roleMask`。
- 不要在实体里查别的实体（不要 `Product::getSeller()` 去翻 Store）。
- 不要在 UI 里判断"是不是管理员"，权限判断一律在 Service。
- 不要用裸 `new/delete`；实体用值类型存 `unordered_map`。
- 不要把业务规则写成"只在前端校验"；服务层无论如何都要再校验一遍。
- 不要在锁里做耗时操作。

---

*文档位置：`software/C++开发框架.md`；实现时可从 M1 开始逐阶段填充。*
