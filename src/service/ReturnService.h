//Return Service
#pragma once

#include <string>
#include <vector>
#include "common/Result.h"
#include "common/Clock.h"
#include "store/Store.h"
#include "model/ReturnRequest.h"


struct ReturnService {
    explicit ReturnService(Store& store, Clock& clock):store(store),clock(clock){}

    // 买家对已付款的订单发起退货申请（REQ-016）
    Result<long long> apply(long long buyerId, long long orderId, const std::string& reason = "");

    // 管理员审核退货申请，approval=false 时驳回并记录理由（REQ-016）
    Status review(
        long long adminId,
        long long returnRequestId,
        bool approval = true,
        const std::string& reason = ""
    );

    // 买家在审核通过后 7 天内寄回商品并提供快递单号（REQ-016）
    Status confirmDelivered(
        long long buyerId,
        long long returnRequestId,
        const std::string& trackingNo = ""
    );

    // 卖家确认收到退货，平台把托管货款退还给买家并结束交易（REQ-016）
    Status confirmReceived(long long sellerId, long long returnRequestId);

    // 扫描并关闭审核通过后 7 天内未寄回的退货申请（REQ-016），
    // 返回本次发生状态变更的记录数
    int sweepTimeouts();

    // 只读查询，供 UI 展示（返回副本，按 id 升序）
    std::vector<ReturnRequest> listReturns();

private:
    Store& store;
    Clock& clock;
};
