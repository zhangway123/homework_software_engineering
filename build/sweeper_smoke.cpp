// 临时冒烟测试：后台 TimeoutSweeper 线程
#include "service/MessageService.h"
#include "service/ProductService.h"
#include "service/ReturnService.h"
#include "service/TimeoutSweeper.h"
#include "service/TradeService.h"

#include <chrono>
#include <iostream>
#include <thread>

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
    MessageService messages(store, clock);

    store.accounts[1] = Account::create(1, Buyer, "buyer", "p");
    store.accounts[2] = Account::create(2, Seller, "seller", "p");
    store.accounts[1].balance = 1000;

    Commodity product = Commodity::create(10, 2);
    product.price = 100;
    product.remaining = 5;
    product.status = ProductStatus::Published;
    store.products[10] = product;

    // 一条消息，用来验证 sweeper 也会清理 30 天前的私信
    Commodity talk = Commodity::create(11, 2);
    talk.status = ProductStatus::Published;
    store.products[11] = talk;
    long long convId = messages.openConversation(1, 11).value;
    CHECK(messages.send(1, convId, MessageKind::Text, "hi").ok());

    // 先把时钟拨到预约已超时、消息已过 30 天保留期，再启动线程：
    // FakeClock 不做同步，运行中再改指针会有数据竞争，测试里避开
    long long rid = trades.reserve(1, 10, 1).value;
    clock.advance(std::chrono::hours(24 * 31));

    {
        TimeoutSweeper sweeper(products, trades, returns, messages, std::chrono::milliseconds(10));

        // 等后台线程跑过至少两轮
        for (int i = 0; i < 400 && sweeper.sweepCount() < 2; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(sweeper.sweepCount() >= 2);

        sweeper.stop();   // join 之后才读 Store，避免和后台线程并发

        CHECK(sweeper.changeCount() >= 2);      // 预约取消 + 消息过期
        CHECK(store.reservations[rid].status == ReservationStatus::Cancelled);
        CHECK(store.products[10].remaining == 5);
        CHECK(store.messages.empty());          // 30 天前的消息已被 sweeper 清掉
        CHECK(store.messageIdsByConversation.empty());
    }

    // stop() 要能立刻打断 30 秒的等待，不能让退出卡一个周期
    {
        TimeoutSweeper sweeper(products, trades, returns, messages);   // 默认 30s
        const auto start = std::chrono::steady_clock::now();
        sweeper.stop();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        CHECK(elapsed < 1000);

        sweeper.stop();   // 可重复调用
    }

    // 超时记录只处理一次，重复清扫不会重复扣/退
    {
        TimeoutSweeper sweeper(products, trades, returns, messages, std::chrono::milliseconds(10));
        for (int i = 0; i < 400 && sweeper.sweepCount() < 5; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        sweeper.stop();
        CHECK(store.products[10].remaining == 5);
        CHECK(store.reservations[rid].status == ReservationStatus::Cancelled);
    }

    if (failures == 0) std::cout << "all sweeper checks passed\n";
    return failures == 0 ? 0 : 1;
}
