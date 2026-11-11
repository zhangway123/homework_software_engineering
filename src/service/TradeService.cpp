//Trade Service
#include "TradeService.h"

#include <algorithm>
#include <chrono>
#include <mutex>

namespace {
// 卖家确认后买家须在 2 小时内付款，超时自动取消（REQ-014）
constexpr std::chrono::seconds kPaymentTimeout{2 * 60 * 60};
// 预约后 24 小时卖家未确认，自动取消（REQ-012）
constexpr std::chrono::seconds kConfirmTimeout{24 * 60 * 60};
// 付款后 7 天卖家未发货，交易无效并退款（REQ-015）
constexpr std::chrono::seconds kShipTimeout{7 * 24 * 60 * 60};
// 发货后 7 天买家未确认收货，自动打款给卖家（REQ-015）
constexpr std::chrono::seconds kReceiptTimeout{7 * 24 * 60 * 60};

// 预约占用的库存回到商品；商品已被删除时忽略
void releaseStock(Store& store, const Reservation& r) {
    auto productIt = store.products.find(r.commodityId);
    if (productIt != store.products.end())
        productIt->second.remaining += r.amount;
}

// 订单对应的托管记录；同一订单只会有一条
EscrowRecord* findEscrow(Store& store, long long orderId) {
    for (auto& entry : store.escrows) {
        if (entry.second.orderId == orderId)
            return &entry.second;
    }
    return nullptr;
}

// 同一买家对同一商品只能有一条进行中的预约（REQ-001 / REQ-019）
bool hasActiveReservation(const Store& store, long long buyerId, long long productId) {
    for (const auto& entry : store.reservations) {
        const Reservation& r = entry.second;
        if (r.purchaserId == buyerId && r.commodityId == productId &&
            (r.status == ReservationStatus::Pending ||
             r.status == ReservationStatus::Confirmed))
            return true;
    }
    return false;
}
}

Result<long long> TradeService::reserve(long long buyerId, long long productId, int amount) {
    std::lock_guard<std::mutex> lock(store.mutexLock);

    if (amount <= 0)
        return Result<long long>::fail(Err::InvalidArg, "amount must be positive");

    auto accountIt = store.accounts.find(buyerId);
    if (accountIt == store.accounts.end())
        return Result<long long>::fail(Err::Unauthorized, "invalid account id");
    if (!accountIt->second.hasAbility(Buyer))
        return Result<long long>::fail(Err::Forbidden, "No Accessibility!");

    auto productIt = store.products.find(productId);
    if (productIt == store.products.end())
        return Result<long long>::fail(Err::NotFound, "invalid product id");

    // 下面必须改 Store 里的商品本体，不能改副本
    Commodity& product = productIt->second;

    if (product.sellerId == buyerId)
        return Result<long long>::fail(Err::Forbidden, "cannot reserve your own product");
    if (!product.can_reserve() || product.remaining < amount)
        return Result<long long>::fail(Err::OutOfStock, "Can Not Reserve!");

    if (hasActiveReservation(store, buyerId, productId))
        return Result<long long>::fail(Err::DuplicateReservation, "product already reserved");

    product.remaining -= amount;

    long long reservationId = store.newId();
    Reservation r = Reservation::create(reservationId, product, accountIt->second, amount);
    r.createdAt = clock.now();

    store.reservations[reservationId] = r;
    return Result<long long>::good(reservationId);
}

Result<long long> TradeService::cancelReservation(long long buyerId, long long reservationId) {
    std::lock_guard<std::mutex> lock(store.mutexLock);

    auto accountIt = store.accounts.find(buyerId);
    if (accountIt == store.accounts.end())
        return Result<long long>::fail(Err::Unauthorized, "invalid account id");
    if (!accountIt->second.hasAbility(Buyer))
        return Result<long long>::fail(Err::Forbidden, "No Accessibility!");

    auto reservationIt = store.reservations.find(reservationId);
    if (reservationIt == store.reservations.end())
        return Result<long long>::fail(Err::NotFound, "invalid reservation id");

    Reservation& r = reservationIt->second;

    // 只能取消自己的预约（REQ-017：服务端独立鉴权）
    if (r.purchaserId != buyerId)
        return Result<long long>::fail(Err::Forbidden, "not your reservation");
    // 卖家确认后买家不可再取消（REQ-012）
    if (r.status != ReservationStatus::Pending)
        return Result<long long>::fail(Err::InvalidState, "only pending reservation can be cancelled");

    releaseStock(store, r);
    r.status = ReservationStatus::Cancelled;

    return Result<long long>::good(reservationId);
}

Result<long long> TradeService::confirmReservation(
    long long sellerId,
    long long reservationId,
    int changedPrice
) {
    std::lock_guard<std::mutex> lock(store.mutexLock);

    auto accountIt = store.accounts.find(sellerId);
    if (accountIt == store.accounts.end())
        return Result<long long>::fail(Err::Unauthorized, "invalid account id");
    if (!accountIt->second.hasAbility(Seller))
        return Result<long long>::fail(Err::Forbidden, "No Accessibility!");

    auto reservationIt = store.reservations.find(reservationId);
    if (reservationIt == store.reservations.end())
        return Result<long long>::fail(Err::NotFound, "invalid reservation id");

    Reservation& r = reservationIt->second;

    // 只能确认自己商品的预约
    if (r.sellerId != sellerId)
        return Result<long long>::fail(Err::Forbidden, "not your reservation");
    if (r.status != ReservationStatus::Pending)
        return Result<long long>::fail(Err::InvalidState, "invalid reservation status");

    // 前端校验不可信，服务端仍需独立校验（REQ-017）
    if (changedPrice != -1) {
        if (changedPrice <= 0)
            return Result<long long>::fail(Err::InvalidArg, "changed price must be positive");
        r.commodityPrice = changedPrice;
    }

    r.status = ReservationStatus::Confirmed;
    r.confirmedAt = clock.now();

    return Result<long long>::good(reservationId);
}

Result<long long> TradeService::buy(long long buyerId, long long reservationId) {
    std::lock_guard<std::mutex> lock(store.mutexLock);

    auto accountIt = store.accounts.find(buyerId);
    if (accountIt == store.accounts.end())
        return Result<long long>::fail(Err::Unauthorized, "invalid account id");
    if (!accountIt->second.hasAbility(Buyer))
        return Result<long long>::fail(Err::Forbidden, "No Accessibility!");

    auto reservationIt = store.reservations.find(reservationId);
    if (reservationIt == store.reservations.end())
        return Result<long long>::fail(Err::NotFound, "invalid reservation id");

    Reservation& r = reservationIt->second;

    if (r.purchaserId != buyerId)
        return Result<long long>::fail(Err::Forbidden, "not your reservation");
    if (r.status != ReservationStatus::Confirmed)
        return Result<long long>::fail(Err::InvalidState, "reservation not confirmed yet");

    // 确认后 2 小时未付款：自动取消并释放余量（REQ-014）
    const Clock::TimePoint now = clock.now();
    if (r.confirmedAt != Clock::TimePoint{} && now - r.confirmedAt > kPaymentTimeout) {
        releaseStock(store, r);
        r.status = ReservationStatus::Cancelled;
        return Result<long long>::fail(Err::Expired, "payment timeout, reservation cancelled");
    }

    Account& buyer = accountIt->second;
    double totalPrice = r.commodityPrice * r.amount;

    if (buyer.balance < totalPrice)
        return Result<long long>::fail(Err::InvalidState, "insufficient balance");

    buyer.balance -= totalPrice;

    long long orderId = store.newId();
    store.orders[orderId] = Order::create(orderId, r, buyer);

    Order& order = store.orders[orderId];
    // 货款由平台托管，确认收货后再转给卖家（REQ-015）
    order.status = OrderStatus::Paid;
    order.paidAt = now;

    // 货款进入平台托管：生成托管记录，卖家此时还不能拿到钱（REQ-015）
    long long escrowId = store.newId();
    EscrowRecord escrow = EscrowRecord::create(escrowId, order);
    escrow.heldAt = now;
    store.escrows[escrowId] = escrow;

    // 预约对应的库存已在 reserve 时扣减，这里只推进状态
    r.status = ReservationStatus::Completed;

    return Result<long long>::good(orderId);
}

Status TradeService::ship(long long sellerId, long long orderId) {
    std::lock_guard<std::mutex> lock(store.mutexLock);

    auto accountIt = store.accounts.find(sellerId);
    if (accountIt == store.accounts.end())
        return Status::fail(Err::Unauthorized, "invalid account id");
    if (!accountIt->second.hasAbility(Seller))
        return Status::fail(Err::Forbidden, "No Accessibility!");

    auto orderIt = store.orders.find(orderId);
    if (orderIt == store.orders.end())
        return Status::fail(Err::NotFound, "invalid order id");

    Order& order = orderIt->second;

    // 只能给自己的订单发货（REQ-017：服务端独立鉴权）
    if (order.sellerId != sellerId)
        return Status::fail(Err::Forbidden, "not your order");
    if (order.status != OrderStatus::Paid)
        return Status::fail(Err::InvalidState, "only paid order can be shipped");

    order.status = OrderStatus::Shipped;
    order.shippedAt = clock.now();

    return Status::good();
}

Status TradeService::confirmReceipt(long long buyerId, long long orderId) {
    std::lock_guard<std::mutex> lock(store.mutexLock);

    auto accountIt = store.accounts.find(buyerId);
    if (accountIt == store.accounts.end())
        return Status::fail(Err::Unauthorized, "invalid account id");
    if (!accountIt->second.hasAbility(Buyer))
        return Status::fail(Err::Forbidden, "No Accessibility!");

    auto orderIt = store.orders.find(orderId);
    if (orderIt == store.orders.end())
        return Status::fail(Err::NotFound, "invalid order id");

    Order& order = orderIt->second;

    // 只能确认自己的订单（REQ-017：服务端独立鉴权）
    if (order.buyerId != buyerId)
        return Status::fail(Err::Forbidden, "not your order");
    if (order.status != OrderStatus::Shipped)
        return Status::fail(Err::InvalidState, "only shipped order can be confirmed");

    // 该订单仍在托管中的货款
    auto escrowIt = std::find_if(
        store.escrows.begin(), store.escrows.end(),
        [orderId](const std::pair<const long long, EscrowRecord>& entry) {
            return entry.second.orderId == orderId && entry.second.status == EscrowStatus::Held;
        }
    );
    if (escrowIt == store.escrows.end())
        return Status::fail(Err::NotFound, "escrow record not found");

    auto sellerIt = store.accounts.find(order.sellerId);
    if (sellerIt == store.accounts.end())
        return Status::fail(Err::NotFound, "seller account not found");

    const Clock::TimePoint now = clock.now();

    // 确认收货后货款由平台托管转给卖家（REQ-015）
    sellerIt->second.balance += escrowIt->second.amount;
    escrowIt->second.status = EscrowStatus::Released;
    escrowIt->second.releasedAt = now;

    order.status = OrderStatus::Completed;
    order.receivedAt = now;

    return Status::good();
}

int TradeService::sweepTimeouts() {
    std::lock_guard<std::mutex> lock(store.mutexLock);

    const Clock::TimePoint now = clock.now();
    int changed = 0;

    // 1) 预约超时：待确认 24 小时、已确认 2 小时未付款（REQ-012 / REQ-014）
    for (auto& entry : store.reservations) {
        Reservation& r = entry.second;

        bool expired = false;
        if (r.status == ReservationStatus::Pending)
            expired = now - r.createdAt > kConfirmTimeout;
        else if (r.status == ReservationStatus::Confirmed)
            expired = now - r.confirmedAt > kPaymentTimeout;
        if (!expired)
            continue;

        releaseStock(store, r);
        r.status = ReservationStatus::Cancelled;
        ++changed;
    }

    // 2) 订单超时：卖家 7 天未发货 / 买家 7 天未确认收货（REQ-015）
    for (auto& entry : store.orders) {
        Order& order = entry.second;

        if (order.status == OrderStatus::Paid && now - order.paidAt > kShipTimeout) {
            // 卖家未按时发货：交易无效，托管货款全额退还买家，交易关闭
            EscrowRecord* escrow = findEscrow(store, order.id);
            auto buyerIt = store.accounts.find(order.buyerId);
            if (escrow == nullptr || escrow->status != EscrowStatus::Held ||
                buyerIt == store.accounts.end())
                continue;   // 数据不完整，留给人工处理，不在这里猜

            buyerIt->second.balance += escrow->amount;
            escrow->status = EscrowStatus::Refunded;
            escrow->refundedAt = now;
            order.status = OrderStatus::Closed;
            ++changed;
        }
        else if (order.status == OrderStatus::Shipped &&
                 now - order.shippedAt > kReceiptTimeout) {
            // 买家长期未确认收货：自动打款给卖家，订单视为完成
            EscrowRecord* escrow = findEscrow(store, order.id);
            auto sellerIt = store.accounts.find(order.sellerId);
            if (escrow == nullptr || escrow->status != EscrowStatus::Held ||
                sellerIt == store.accounts.end())
                continue;

            sellerIt->second.balance += escrow->amount;
            escrow->status = EscrowStatus::Released;
            escrow->releasedAt = now;
            order.status = OrderStatus::Completed;
            order.receivedAt = now;
            ++changed;
        }
    }

    return changed;
}

double TradeService::escrowBalance() {
    std::lock_guard<std::mutex> lock(store.mutexLock);
    double total = 0;
    for (const auto& entry : store.escrows) {
        if (entry.second.status == EscrowStatus::Held)
            total += entry.second.amount;
    }
    return total;
}

std::vector<Reservation> TradeService::listReservations() {
    std::lock_guard<std::mutex> lock(store.mutexLock);
    std::vector<Reservation> out;
    out.reserve(store.reservations.size());
    for (const auto& entry : store.reservations) out.push_back(entry.second);
    std::sort(out.begin(), out.end(),
              [](const Reservation& a, const Reservation& b) { return a.id < b.id; });
    return out;
}

std::vector<Order> TradeService::listOrders() {
    std::lock_guard<std::mutex> lock(store.mutexLock);
    std::vector<Order> out;
    out.reserve(store.orders.size());
    for (const auto& entry : store.orders) out.push_back(entry.second);
    std::sort(out.begin(), out.end(),
              [](const Order& a, const Order& b) { return a.id < b.id; });
    return out;
}


