//Timeout Sweeper
#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

struct ProductService;
struct TradeService;
struct ReturnService;
struct MessageService;

// 超时清扫器：后台线程按固定间隔调用四个 Service 的 sweepTimeouts()，
// 让「自动取消 / 自动退款 / 自动打款 / 草稿到期删除 / 消息满 30 天清理」
// 不依赖用户操作触发。
//
// 生命周期：构造即启动，析构时停止并 join。服务对象的寿命必须长于它，
// 因此在 main 里应把它声明在四个 Service 之后。
struct TimeoutSweeper {
    TimeoutSweeper(
        ProductService& products,
        TradeService& trades,
        ReturnService& returns,
        MessageService& messages,
        std::chrono::milliseconds interval = std::chrono::seconds(30)
    );

    ~TimeoutSweeper();

    TimeoutSweeper(const TimeoutSweeper&) = delete;
    TimeoutSweeper& operator=(const TimeoutSweeper&) = delete;

    // 停止后台线程并等待退出；析构函数也会调用，可重复调用
    void stop();

    // 已完成的清扫轮数与累计变更记录数（线程安全，供日志与测试断言）
    long long sweepCount() const;
    long long changeCount() const;

private:
    void loop();
    void sweepOnce();

    ProductService& products;
    TradeService& trades;
    ReturnService& returns;
    MessageService& messages;
    std::chrono::milliseconds interval;

    std::thread worker;
    mutable std::mutex mutex;
    std::condition_variable cv;
    bool stopping = false;
    long long sweeps = 0;
    long long changes = 0;
};
