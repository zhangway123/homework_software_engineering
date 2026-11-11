// 临时冒烟测试：不随项目提交，验证 ReturnService 状态流转与退款
#include "service/ReturnService.h"
#include "service/TradeService.h"

#include <cassert>
#include <iostream>

static int failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::cerr << "FAIL line " << __LINE__ << ": " #cond << "\n";   \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

int main() {
    Store store;
    FakeClock clock(Clock::TimePoint{});
    ReturnService returns(store, clock);
    TradeService trades(store, clock);

    Account buyer = Account::create(1, Buyer, "buyer", "p");
    Account seller = Account::create(2, Seller, "seller", "p");
    Account admin = Account::create(3, Admin, "admin", "p");
    buyer.balance = 1000;
    store.accounts[1] = buyer;
    store.accounts[2] = seller;
    store.accounts[3] = admin;

    // 构造一笔已付款、货款托管的订单
    Commodity product = Commodity::create(10, 2);
    product.price = 100;
    product.remaining = 5;
    product.status = ProductStatus::Published;
    store.products[10] = product;

    Reservation res = Reservation::create(11, store.products[10], store.accounts[1], 1);
    store.reservations[11] = res;

    CHECK(trades.confirmReservation(2, 11).ok());
    Result<long long> bought = trades.buy(1, 11);
    CHECK(bought.ok());
    long long orderId = bought.value;
    CHECK(store.orders[orderId].status == OrderStatus::Paid);
    CHECK(store.accounts[1].balance == 900);
    CHECK(trades.escrowBalance() == 100);

    // 卖家发货 -> 订单可退货
    CHECK(trades.ship(2, orderId).ok());

    // 非本人订单不能申请退货
    CHECK(returns.apply(2, orderId).code == Err::Forbidden);
    // 申请
    Result<long long> applied = returns.apply(1, orderId, "质量问题");
    CHECK(applied.ok());
    long long rid = applied.value;
    CHECK(store.returnRequests[rid].status == ReturnStatus::Pending);
    // 同一订单不能重复申请
    CHECK(returns.apply(1, orderId).code == Err::InvalidState);

    // 未审核不能寄回 / 不能确认收货
    CHECK(returns.confirmDelivered(1, rid).code == Err::InvalidState);
    CHECK(returns.confirmReceived(2, rid).code == Err::InvalidState);

    // 仅管理员可审核
    CHECK(returns.review(1, rid).code == Err::Forbidden);
    CHECK(returns.review(3, rid).ok());
    CHECK(store.returnRequests[rid].status == ReturnStatus::Approved);

    clock.advance(std::chrono::seconds(7 * 24 * 60 * 60 + 1));
    // 超 7 天未寄回：流程关闭、不退款
    CHECK(returns.confirmDelivered(1, rid, "SF123").code == Err::Expired);
    CHECK(store.returnRequests[rid].status == ReturnStatus::Closed);
    CHECK(store.accounts[1].balance == 900);
    CHECK(trades.escrowBalance() == 100);

    // ---- 正常路径（另一笔订单） ----
    Result<long long> applied2 = returns.apply(1, orderId, "再次申请");
    CHECK(applied2.ok());  // 上一笔已 Closed，可重新申请
    long long rid2 = applied2.value;
    CHECK(returns.review(3, rid2, false, "").code == Err::InvalidArg);  // 驳回须给理由
    CHECK(returns.review(3, rid2).ok());
    CHECK(returns.confirmDelivered(1, rid2, "SF999").ok());
    CHECK(store.returnRequests[rid2].status == ReturnStatus::Delivered);
    CHECK(store.returnRequests[rid2].trackingNo == "SF999");

    // 别的卖家不能替卖家确认
    CHECK(returns.confirmReceived(1, rid2).code == Err::Forbidden);
    CHECK(returns.confirmReceived(2, rid2).ok());
    CHECK(store.returnRequests[rid2].status == ReturnStatus::Refunded);
    CHECK(store.accounts[1].balance == 1000);          // 货款退回买家
    CHECK(store.accounts[2].balance == 0);             // 卖家没拿到钱
    CHECK(trades.escrowBalance() == 0);
    CHECK(store.orders[orderId].status == OrderStatus::Returned);

    // ---- 已确认收货后再退货：从卖家余额扣回 ----
    Store store2;
    FakeClock clock2(Clock::TimePoint{});
    ReturnService returns2(store2, clock2);
    TradeService trades2(store2, clock2);

    store2.accounts[1] = Account::create(1, Buyer, "buyer", "p");
    store2.accounts[2] = Account::create(2, Seller, "seller", "p");
    store2.accounts[3] = Account::create(3, Admin, "admin", "p");
    store2.accounts[1].balance = 1000;
    store2.products[10] = product;
    store2.reservations[11] = Reservation::create(11, store2.products[10], store2.accounts[1], 1);

    CHECK(trades2.confirmReservation(2, 11).ok());
    long long orderId2 = trades2.buy(1, 11).value;
    CHECK(trades2.ship(2, orderId2).ok());
    CHECK(trades2.confirmReceipt(1, orderId2).ok());
    CHECK(store2.accounts[2].balance == 100);          // 货款已打给卖家
    CHECK(trades2.escrowBalance() == 0);

    long long rid3 = returns2.apply(1, orderId2).value;
    CHECK(returns2.review(3, rid3).ok());
    CHECK(returns2.confirmDelivered(1, rid3, "YT1").ok());
    CHECK(returns2.confirmReceived(2, rid3).ok());
    CHECK(store2.accounts[2].balance == 0);            // 从卖家余额扣回
    CHECK(store2.accounts[1].balance == 1000);
    CHECK(store2.orders[orderId2].status == OrderStatus::Returned);

    if (failures == 0) std::cout << "all return-flow checks passed\n";
    return failures == 0 ? 0 : 1;
}
