//Return Service
#include "ReturnService.h"

#include <algorithm>
#include <chrono>
#include <mutex>

namespace {
// 管理员审核通过后买家须在 7 天内寄回，否则视为交易仍然达成（REQ-016）
constexpr std::chrono::seconds kReturnShipTimeout{7 * 24 * 60 * 60};

// 退货流程中尚未结束的状态
bool isActive(ReturnStatus s) {
    return s == ReturnStatus::Pending ||
           s == ReturnStatus::Approved ||
           s == ReturnStatus::Shipped ||
           s == ReturnStatus::Delivered;
}

// 审核通过后是否已超过寄回期限（REQ-016）；status 为 Approved 时 approveAt 必然已填
bool shipBackExpired(const ReturnRequest& r, Clock::TimePoint now) {
    return now - r.approveAt > kReturnShipTimeout;
}

// 订单对应的托管记录；同一订单只会有一条
EscrowRecord* findEscrow(Store& store, long long orderId) {
    for (auto& entry : store.escrows) {
        if (entry.second.orderId == orderId)
            return &entry.second;
    }
    return nullptr;
}
}

Result<long long> ReturnService::apply(
    long long buyerId,
    long long orderId,
    const std::string& reason
) {
    std::lock_guard<std::mutex> lock(store.mutexLock);

    auto accountIt = store.accounts.find(buyerId);
    if (accountIt == store.accounts.end())
        return Result<long long>::fail(Err::Unauthorized, "invalid account id");
    if (!accountIt->second.hasAbility(Buyer))
        return Result<long long>::fail(Err::Forbidden, "No Accessibility!");

    auto orderIt = store.orders.find(orderId);
    if (orderIt == store.orders.end())
        return Result<long long>::fail(Err::NotFound, "invalid order id");

    const Order& order = orderIt->second;

    // 只能对自己的订单发起退货（REQ-017：服务端独立鉴权）
    if (order.buyerId != buyerId)
        return Result<long long>::fail(Err::Forbidden, "not your order");
    // 前置条件：存在已付款的交易（REQ-016）；未付款与已关闭的订单不能退货
    if (order.status != OrderStatus::Paid &&
        order.status != OrderStatus::Shipped &&
        order.status != OrderStatus::Completed)
        return Result<long long>::fail(Err::InvalidState, "order is not in a returnable state");

    // 同一订单只能有一条进行中的退货申请
    for (const auto& entry : store.returnRequests) {
        if (entry.second.orderId == orderId && isActive(entry.second.status))
            return Result<long long>::fail(Err::InvalidState, "return request already exists");
    }

    long long returnId = store.newId();
    ReturnRequest r = ReturnRequest::create(returnId, order, reason);
    r.applyAt = clock.now();

    store.returnRequests[returnId] = r;
    return Result<long long>::good(returnId);
}

Status ReturnService::review(
    long long adminId,
    long long returnRequestId,
    bool approval,
    const std::string& reason
) {
    std::lock_guard<std::mutex> lock(store.mutexLock);

    auto adminIt = store.accounts.find(adminId);
    if (adminIt == store.accounts.end())
        return Status::fail(Err::Unauthorized, "invalid account id");
    if (!adminIt->second.hasAbility(Admin))
        return Status::fail(Err::Forbidden, "No Accessibility!");

    auto requestIt = store.returnRequests.find(returnRequestId);
    if (requestIt == store.returnRequests.end())
        return Status::fail(Err::NotFound, "invalid return request id");

    ReturnRequest& r = requestIt->second;

    if (r.status != ReturnStatus::Pending)
        return Status::fail(Err::InvalidState, "only pending return request can be reviewed");

    // 卖家不同意退货时由管理员裁决，驳回须给出理由（REQ-016）
    if (!approval && reason.empty())
        return Status::fail(Err::InvalidArg, "reason is required when rejecting");

    r.status = approval ? ReturnStatus::Approved : ReturnStatus::Rejected;
    r.reviewReason = reason;
    r.approveAt = clock.now();

    return Status::good();
}

Status ReturnService::confirmDelivered(
    long long buyerId,
    long long returnRequestId,
    const std::string& trackingNo
) {
    std::lock_guard<std::mutex> lock(store.mutexLock);

    auto accountIt = store.accounts.find(buyerId);
    if (accountIt == store.accounts.end())
        return Status::fail(Err::Unauthorized, "invalid account id");
    if (!accountIt->second.hasAbility(Buyer))
        return Status::fail(Err::Forbidden, "No Accessibility!");

    auto requestIt = store.returnRequests.find(returnRequestId);
    if (requestIt == store.returnRequests.end())
        return Status::fail(Err::NotFound, "invalid return request id");

    ReturnRequest& r = requestIt->second;

    if (r.buyerId != buyerId)
        return Status::fail(Err::Forbidden, "not your return request");
    if (r.status != ReturnStatus::Approved)
        return Status::fail(Err::InvalidState, "return request not approved yet");

    // 审核通过后 7 天内未寄回：视为交易仍然达成，退货流程关闭且不退款（REQ-016）
    const Clock::TimePoint now = clock.now();
    if (shipBackExpired(r, now)) {
        r.status = ReturnStatus::Closed;
        return Status::fail(Err::Expired, "return timeout, transaction stands");
    }

    r.trackingNo = trackingNo;
    r.status = ReturnStatus::Delivered;
    r.deliveredAt = now;

    return Status::good();
}

Status ReturnService::confirmReceived(long long sellerId, long long returnRequestId) {
    std::lock_guard<std::mutex> lock(store.mutexLock);

    auto accountIt = store.accounts.find(sellerId);
    if (accountIt == store.accounts.end())
        return Status::fail(Err::Unauthorized, "invalid account id");
    if (!accountIt->second.hasAbility(Seller))
        return Status::fail(Err::Forbidden, "No Accessibility!");

    auto requestIt = store.returnRequests.find(returnRequestId);
    if (requestIt == store.returnRequests.end())
        return Status::fail(Err::NotFound, "invalid return request id");

    ReturnRequest& r = requestIt->second;

    // 只能确认自己商品那笔退货（REQ-017：服务端独立鉴权）
    if (r.sellerId != sellerId)
        return Status::fail(Err::Forbidden, "not your return request");
    if (r.status != ReturnStatus::Delivered)
        return Status::fail(Err::InvalidState, "buyer has not shipped the goods back yet");

    auto orderIt = store.orders.find(r.orderId);
    if (orderIt == store.orders.end())
        return Status::fail(Err::NotFound, "invalid order id");

    auto buyerIt = store.accounts.find(r.buyerId);
    if (buyerIt == store.accounts.end())
        return Status::fail(Err::NotFound, "buyer account not found");

    EscrowRecord* escrow = findEscrow(store, r.orderId);
    if (escrow == nullptr)
        return Status::fail(Err::NotFound, "escrow record not found");

    const Clock::TimePoint now = clock.now();
    double amount = escrow->amount;

    // 货款退还买家（REQ-016）
    if (escrow->status == EscrowStatus::Held) {
        // 常见路径：买家确认收货前退货，钱还在平台托管
        escrow->status = EscrowStatus::Refunded;
        escrow->refundedAt = now;
    }
    else if (escrow->status == EscrowStatus::Released) {
        // 买家已确认收货、货款已打给卖家：从卖家余额扣回再退给买家
        auto sellerIt = store.accounts.find(r.sellerId);
        if (sellerIt == store.accounts.end())
            return Status::fail(Err::NotFound, "seller account not found");
        if (sellerIt->second.balance < amount)
            return Status::fail(Err::InvalidState, "seller balance insufficient for refund");
        sellerIt->second.balance -= amount;
        escrow->status = EscrowStatus::Refunded;
        escrow->refundedAt = now;
    }
    else {
        return Status::fail(Err::InvalidState, "escrow already refunded");
    }

    buyerIt->second.balance += amount;

    r.status = ReturnStatus::Refunded;
    r.receivedAt = now;

    orderIt->second.status = OrderStatus::Returned;

    return Status::good();
}

int ReturnService::sweepTimeouts() {
    std::lock_guard<std::mutex> lock(store.mutexLock);

    const Clock::TimePoint now = clock.now();
    int changed = 0;

    // 审核通过后 7 天未寄回：视为交易仍然达成，不退款（REQ-016）
    for (auto& entry : store.returnRequests) {
        ReturnRequest& r = entry.second;
        if (r.status != ReturnStatus::Approved || !shipBackExpired(r, now))
            continue;
        r.status = ReturnStatus::Closed;
        ++changed;
    }

    return changed;
}

std::vector<ReturnRequest> ReturnService::listReturns() {
    std::lock_guard<std::mutex> lock(store.mutexLock);
    std::vector<ReturnRequest> out;
    out.reserve(store.returnRequests.size());
    for (const auto& entry : store.returnRequests) out.push_back(entry.second);
    std::sort(out.begin(), out.end(),
              [](const ReturnRequest& a, const ReturnRequest& b) { return a.id < b.id; });
    return out;
}
