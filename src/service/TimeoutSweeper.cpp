//Timeout Sweeper
#include "TimeoutSweeper.h"

#include "MessageService.h"
#include "ProductService.h"
#include "ReturnService.h"
#include "TradeService.h"

#include <iostream>

TimeoutSweeper::TimeoutSweeper(
    ProductService& products,
    TradeService& trades,
    ReturnService& returns,
    MessageService& messages,
    std::chrono::milliseconds interval
)
    : products(products), trades(trades), returns(returns),
      messages(messages), interval(interval) {
    worker = std::thread(&TimeoutSweeper::loop, this);
}

TimeoutSweeper::~TimeoutSweeper() {
    stop();
}

void TimeoutSweeper::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
    }
    cv.notify_all();
    if (worker.joinable())
        worker.join();
}

long long TimeoutSweeper::sweepCount() const {
    std::lock_guard<std::mutex> lock(mutex);
    return sweeps;
}

long long TimeoutSweeper::changeCount() const {
    std::lock_guard<std::mutex> lock(mutex);
    return changes;
}

void TimeoutSweeper::loop() {
    std::unique_lock<std::mutex> lock(mutex);

    // 启动先扫一次，之后按间隔等待；用条件变量而不是 sleep，
    // 这样 stop() 能立刻打断等待，进程退出时不必干等一个周期
    while (!stopping) {
        lock.unlock();
        sweepOnce();
        lock.lock();

        cv.wait_for(lock, interval, [this] { return stopping; });
    }
}

void TimeoutSweeper::sweepOnce() {
    // 四个 Service 各自对同一个 Store 加锁，逐个调用即可，不并发写
    const int productChanges = products.sweepTimeouts();
    const int tradeChanges   = trades.sweepTimeouts();
    const int returnChanges  = returns.sweepTimeouts();
    const int messageChanges = messages.sweepTimeouts();
    const int total          = productChanges + tradeChanges + returnChanges + messageChanges;

    {
        std::lock_guard<std::mutex> lock(mutex);
        ++sweeps;
        changes += total;
    }

    if (total > 0) {
        // 一次 << 输出，避免服务器线程的日志插进来把整行撕开
        std::cout << "[sweeper] closed " << total << " timed-out record(s): "
                  << "products=" << productChanges
                  << " trades=" << tradeChanges
                  << " returns=" << returnChanges
                  << " messages=" << messageChanges << "\n";
    }
}
