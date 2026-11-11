#pragma once
#include <string>
#include <unordered_map>
#include <vector>

struct AuthService;
struct ProductService;
struct TradeService;
struct ReturnService;
struct SearchService;
struct SearchQuery;
// TODO(REQ-006)：实现消息接口时启用下面这行前置声明：
// struct MessageService;

// 一次 HTTP 响应需要携带的内容：JSON 正文 + 可选的 Set-Cookie
struct WebReply {
    std::string json;
    std::string setCookie;  // 非空时写入 Set-Cookie 响应头
};

// 一个极简的本地 Web 界面，用于手工测试注册、商品与审核流程。
// 职责与 ConsoleUI 一致：只负责收输入、调 Service、展示 Result，
// 业务规则（用户名唯一、审核状态机等）仍由 Service 层决定。
//
// 登录后 UI 会种一个 HttpOnly 的会话 Cookie，页面上的卖家 / 管理员身份
// 全部由服务端会话推导，前端不再手填 id。
//
// 使用方式：main 里构造后调用 run()，浏览器打开 http://127.0.0.1:<port>/
struct WebUI {
    // TODO(REQ-006)：构造参数加 MessageService& messageService（放在 searchService 后），
    // 成员区同步加 MessageService& messages，main.cpp 构造 WebUI 时一并传入。
    explicit WebUI(AuthService& authService, ProductService& productService,
                   TradeService& tradeService, ReturnService& returnService,
                   SearchService& searchService, unsigned short port = 8080);

    // 启动 HTTP 服务并阻塞（按 Ctrl+C 退出）
    void run();

    // 以下方法拆出来，方便单独调试接口
    std::string registrationPage() const;                       // GET /
    std::string productPage() const;                            // GET /products
    WebReply handleRegister(const std::string& body);           // POST /api/register
    WebReply handleLogin(const std::string& body);              // POST /api/login
    WebReply handleLogout(const std::string& cookieHeader);     // POST /api/logout
    std::string registeredUsersJson() const;                    // GET /api/users

    std::string meJson(const std::string& cookieHeader);        // GET /api/me
    // 从 Cookie 头解析会话；未登录返回 0（serveOne 用它确定当前用户）
    long long sessionAccount(const std::string& cookieHeader) const;
    std::string handleCreateAccount(const std::string& body);   // POST /api/dev/account
    std::string handleRecharge(const std::string& body, long long accountId);  // POST /api/account/recharge
    std::string handleAddProduct(long long accountId);          // POST /api/products
    std::string handleSaveDraft(const std::string& body, long long accountId);
    std::string handleSubmitReview(const std::string& body, long long accountId);
    std::string handleReview(const std::string& body, long long accountId);
    std::string handleRemoveFromShelf(const std::string& body, long long accountId);
    std::string handleReserve(const std::string& body, long long accountId);
    std::string handleCancelReservation(const std::string& body, long long accountId);
    std::string handleConfirmReservation(const std::string& body, long long accountId);
    std::string handleBuy(const std::string& body, long long accountId);

    // 发货 / 确认收货（REQ-015）
    std::string handleShip(const std::string& body, long long accountId);
    std::string handleConfirmReceipt(const std::string& body, long long accountId);

    // 退货流程（REQ-016）：申请 -> 管理员审核 -> 寄回填单号 -> 卖家确认收到
    std::string handleReturnApply(const std::string& body, long long accountId);
    std::string handleReturnReview(const std::string& body, long long accountId);
    std::string handleReturnDelivered(const std::string& body, long long accountId);
    std::string handleReturnReceived(const std::string& body, long long accountId);

    // ---- 消息 / 会话（REQ-006），签名先以注释确定，实现时再落地 ----
    // POST /api/conversations
    //   body {"productId": N}；accountId 只从会话 Cookie 推导
    //   -> MessageService::openConversation(accountId, productId)
    //   返回 {"ok":true,"conversationId":N}
    // std::string handleOpenConversation(const std::string& body, long long accountId);
    //
    // POST /api/messages
    //   body {"conversationId": N, "kind": "text"|"image", "content": "..."}
    //   -> MessageService::send(accountId, conversationId, kind, content)
    //   返回 {"ok":true,"messageId":N}
    // std::string handleSendMessage(const std::string& body, long long accountId);
    //
    // GET /api/conversations
    //   -> MessageService::listConversations(accountId)
    // std::string conversationsJson(long long accountId);
    //
    // GET /api/messages?conversationId=N
    //   -> MessageService::list(accountId, conversationId)
    // std::string messagesJson(long long conversationId, long long accountId);
    //
    // 注意：serveOne 目前把 '?' 后面的 query 截掉丢弃了，GET /api/messages
    // 需要额外把 request.target 或解析出的 conversationId 传给 handler，
    // 不能继续用截掉 query 后的 path 变量。

    // 搜索（REQ-011 / REQ-013）：GET /api/search?keyword=&category=&minPrice=&maxPrice=
    //   -> SearchService::search(SearchQuery)
    //   返回 {"ok":true,"items":[{productId,title,category,price,remaining,soldOut,
    //         sales30d,matchScore,salesScore,totalScore}]}
    std::string searchJson(const SearchQuery& query);

    std::string productsJson();                                 // GET /api/products
    std::string reviewsJson();                                  // GET /api/reviews
    std::string reservationsJson();                             // GET /api/reservations
    std::string ordersJson();                                   // GET /api/orders
    std::string returnsJson();                                  // GET /api/returns
    std::string escrowJson();                                   // GET /api/escrow

private:
    struct RegisteredAccount {
        long long id;
        std::string username;
    };

    std::string createSession(long long accountId);
    void destroySession(const std::string& cookieHeader);

    AuthService& auth;
    ProductService& products;
    TradeService& trades;
    ReturnService& returns;
    SearchService& search;
    // TODO(REQ-006)：MessageService& messages;
    unsigned short port;
    // 本会话内成功注册的账号，仅用于页面右侧列表展示；不直接访问 Store
    std::vector<RegisteredAccount> registered;
    // sid -> accountId；仅内存保存，进程退出即失效
    std::unordered_map<std::string, long long> sessions;
};
