// 临时冒烟测试：三个 Service 的 sweepTimeouts
#include "service/ProductService.h"
#include "service/TradeService.h"
#include "service/ReturnService.h"

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
    ProductService products(store, clock);
    TradeService trades(store, clock);
    ReturnService returns(store, clock);

    store.accounts[1] = Account::create(1, Buyer, "buyer", "p");
    store.accounts[2] = Account::create(2, Seller, "seller", "p");
    store.accounts[3] = Account::create(3, Admin, "admin", "p");
    store.accounts[1].balance = 1000;

    // ===== ProductService：草稿 30 天保留 =====
    long long draftId = products.addProduct(2).value;
    clock.advance(std::chrono::hours(24 * 29));
    CHECK(products.saveDraft(2, draftId, "t", "d", "c", 10, 1).ok());
    clock.advance(std::chrono::hours(24 * 2));   // 距最后一次保存 2 天
    CHECK(products.sweepTimeouts() == 0);
    CHECK(store.products.count(draftId) == 1);

    long long oldDraftId = products.addProduct(2).value;
    long long publishedId = products.addProduct(2).value;
    CHECK(products.saveDraft(2, publishedId, "t", "d", "c", 100, 5).ok());
    CHECK(products.submitForReview(2, publishedId).ok());
    // 直接把已发布商品的状态摆好，避免依赖审核申请 id
    store.products[publishedId].status = ProductStatus::Published;
    store.products[publishedId].savedAt = Clock::TimePoint{};

    // draftId 最后一次保存是 29 天前（未到期）；oldDraftId 已 31 天（到期）
    store.products[draftId].savedAt = clock.now() - std::chrono::hours(24 * 29);
    store.products[oldDraftId].savedAt = clock.now() - std::chrono::hours(24 * 31);
    CHECK(products.sweepTimeouts() == 1);
    CHECK(store.products.count(oldDraftId) == 0);
    CHECK(store.products.count(draftId) == 1);
    CHECK(store.products.count(publishedId) == 1);   // 已发布的不会被清掉

    // ===== TradeService：预约 24 小时未确认 =====
    Commodity product = Commodity::create(100, 2);
    product.price = 100;
    product.remaining = 10;
    product.status = ProductStatus::Published;
    store.products[100] = product;

    long long r1 = trades.reserve(1, 100, 2).value;
    CHECK(store.products[100].remaining == 8);
    clock.advance(std::chrono::hours(25));
    CHECK(trades.sweepTimeouts() == 1);
    CHECK(store.reservations[r1].status == ReservationStatus::Cancelled);
    CHECK(store.products[100].remaining == 10);

    // ===== TradeService：确认后 2 小时未付款 =====
    long long r2 = trades.reserve(1, 100, 1).value;
    CHECK(trades.confirmReservation(2, r2).ok());
    clock.advance(std::chrono::hours(3));
    CHECK(trades.sweepTimeouts() == 1);
    CHECK(store.reservations[r2].status == ReservationStatus::Cancelled);
    CHECK(store.products[100].remaining == 10);

    // ===== TradeService：付款后 7 天未发货 -> 退款关闭 =====
    long long r3 = trades.reserve(1, 100, 1).value;
    CHECK(trades.confirmReservation(2, r3).ok());
    long long order1 = trades.buy(1, r3).value;
    CHECK(store.accounts[1].balance == 900);
    CHECK(trades.escrowBalance() == 100);
    clock.advance(std::chrono::hours(24 * 8));
    CHECK(trades.sweepTimeouts() == 1);
    CHECK(store.orders[order1].status == OrderStatus::Closed);
    CHECK(store.accounts[1].balance == 1000);
    CHECK(store.accounts[2].balance == 0);
    CHECK(trades.escrowBalance() == 0);

    // ===== TradeService：发货后 7 天未确认收货 -> 自动打款 =====
    long long r4 = trades.reserve(1, 100, 1).value;
    CHECK(trades.confirmReservation(2, r4).ok());
    long long order2 = trades.buy(1, r4).value;
    CHECK(trades.ship(2, order2).ok());
    clock.advance(std::chrono::hours(24 * 8));
    CHECK(trades.sweepTimeouts() == 1);
    CHECK(store.orders[order2].status == OrderStatus::Completed);
    CHECK(store.accounts[2].balance == 100);       // 自动打款给卖家
    CHECK(store.accounts[1].balance == 900);
    CHECK(trades.escrowBalance() == 0);

    // ===== ReturnService：审核通过后 7 天未寄回 =====
    long long r5 = trades.reserve(1, 100, 1).value;
    CHECK(trades.confirmReservation(2, r5).ok());
    long long order3 = trades.buy(1, r5).value;
    CHECK(trades.ship(2, order3).ok());
    long long rid = returns.apply(1, order3, "坏").value;
    CHECK(returns.review(3, rid).ok());
    clock.advance(std::chrono::hours(24 * 8));
    CHECK(returns.sweepTimeouts() == 1);
    CHECK(store.returnRequests[rid].status == ReturnStatus::Closed);
    CHECK(store.orders[order3].status == OrderStatus::Shipped);   // 交易维持

    if (failures == 0) std::cout << "all sweep checks passed\n";
    return failures == 0 ? 0 : 1;
}
