//Authentication Service
#pragma once
#include "common/Result.h"
#include "common/Clock.h"
#include <string>
#include "model/Account.h"
#include "store/Store.h"




struct AuthService{
    explicit AuthService(Store& store, Clock& clock):store(store),clock(clock){}
    
    Result<long long> registerUser(std::string name,std::string password);

    Result<long long> login(std::string name,std::string password);

    // 系统预置 / 后台建号（REQ-003）：UI 测试页用它创建卖家、管理员账号
    Result<long long> createAccount(std::string name,std::string password,Role role);

    // 按 id 取账户快照，UI 用来展示当前登录用户及其角色
    Result<Account> getAccount(long long id);

    // 账户充值：增加可用余额（买家付款前需要先有余额）
    Status recharge(long long accountId, double amount);

private:
    Store& store;
    Clock& clock;
};