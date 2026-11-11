#pragma once
#include <chrono>

class Clock {
public:
    using TimePoint = std::chrono::system_clock::time_point;
    virtual ~Clock() = default;
    virtual TimePoint now() const = 0;
};

class SystemClock : public Clock {
public:
    TimePoint now() const override { return std::chrono::system_clock::now(); }
};

class FakeClock : public Clock {      // 测试超时用
public:
    explicit FakeClock(TimePoint t) : t_(t) {}
    TimePoint now() const override { return t_; }
    void advance(std::chrono::seconds d) { t_ += d; }
private:
    TimePoint t_;
};
