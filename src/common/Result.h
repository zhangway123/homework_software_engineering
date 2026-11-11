#pragma once
#include <string>
#include <utility>

enum class Err {
    Ok = 0, InvalidArg, Unauthorized, Forbidden, NotFound,
    InvalidState, VersionConflict, OutOfStock,
    DuplicateReservation, Locked, LimitExceeded, Expired
};

//这是 非静态数据成员声明 + 默认成员初始化器（default member initializer, NSDMI）。
//意思是：声明一个名为 value、类型为 T 的成员，并规定"如果构造时没人为它提供初值，就用 {} 来初始化它"。

//{} 是花括号初始化（列表初始化），对空的 {} 来说，效果基本等价于值初始化（value-initialization），即 T{}。

template <class T>
struct Result {
    Err code = Err::Ok;
    std::string msg;
    T value{};
    bool ok() const { return code == Err::Ok; }
    static Result good(T v) { return {Err::Ok, "", std::move(v)}; }
    static Result fail(Err c, std::string m) { return {c, std::move(m), {}}; }
};

struct Status {                       // 无返回值的版本
    Err code = Err::Ok;
    std::string msg;
    bool ok() const { return code == Err::Ok; }
    static Status good() { return {}; }
    static Status fail(Err c, std::string m) { return {c, std::move(m)}; }
};