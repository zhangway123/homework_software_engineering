#pragma once
#include <string>
#include "Enums.h"
#include "common/Clock.h"
#include "Order.h"

// 退货申请（REQ-016）：
// 买家发起 -> 管理员审核 -> 买家在 7 天内寄回并提供快递单号
// -> 卖家确认收到 -> 平台把托管货款退还给买家
struct ReturnRequest {
    long long id;
    long long orderId;
    long long buyerId;
    long long sellerId;

    std::string reason;        // 买家申请退货的理由
    std::string reviewReason;  // 管理员审核意见（驳回时必填）
    std::string trackingNo;    // 买家寄回商品的快递单号

    ReturnStatus status;

    Clock::TimePoint applyAt{}, approveAt{}, deliveredAt{}, receivedAt{};

    static ReturnRequest create(long long myId, const Order& order, const std::string& myReason) {
        ReturnRequest r;
        r.id = myId;
        r.orderId = order.id;
        r.buyerId = order.buyerId;
        r.sellerId = order.sellerId;
        r.reason = myReason;
        r.status = ReturnStatus::Pending;

        // 时间字段（applyAt / approveAt / deliveredAt / receivedAt）由服务在状态流转时填写
        return r;
    }
};
