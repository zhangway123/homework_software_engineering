//Trade Service
#pragma once

#include "common/Result.h"
#include "common/Clock.h"
#include "store/Store.h"
#include <vector>

struct TradeService {
    // 时钟由 main 统一创建并注入，测试时可换成 FakeClock 验证超时（REQ-014）
    explicit TradeService(Store& store, Clock& clock) : store(store), clock(clock) {}

    // 买家预约商品：扣减余量并生成预约记录（REQ-001 / REQ-019）
    Result<long long> reserve(long long buyerId, long long productId, int amount = 1);

    // 买家取消预约，恢复余量（REQ-012）
    Result<long long> cancelReservation(long long buyerId, long long reservationId);

    // 卖家确认预约，可同时调整价格（REQ-014）
    Result<long long> confirmReservation(
        long long sellerId,
        long long reservationId,
        int changedPrice = -1
    );

    // 买家付款，货款进入平台托管（REQ-014 / REQ-015）
    Result<long long> buy(long long buyerId, long long reservationId);

    // 平台当前托管的货款总额（REQ-015）
    double escrowBalance();

    // 只读查询，供 UI 展示（返回副本，按 id 升序）
    std::vector<Reservation> listReservations();
    std::vector<Order> listOrders();

    // 卖家发货，订单进入已发货状态（REQ-015）
    Status ship(long long sellerId, long long orderId);

    // 买家确认收货，托管货款转给卖家（REQ-015）
    Status confirmReceipt(long long buyerId, long long orderId);

    // 扫描并处理全部超时：
    //   预约 24 小时卖家未确认 -> 自动取消并释放余量（REQ-012）
    //   确认后 2 小时买家未付款 -> 自动取消并释放余量（REQ-014）
    //   付款后 7 天卖家未发货   -> 交易关闭、托管货款退还买家（REQ-015）
    //   发货后 7 天买家未确认收货 -> 自动打款给卖家（REQ-015）
    // 返回本次发生状态变更的记录数
    int sweepTimeouts();

private:
    Store& store;
    Clock& clock;
};
