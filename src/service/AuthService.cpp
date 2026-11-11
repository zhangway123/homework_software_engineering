#include "AuthService.h"



Result<long long> AuthService::registerUser(std::string name,std::string password){

    std::lock_guard<std::mutex> lock(store.mutexLock);
    
    if (store.usernameIndex.count(name)) 
        return Result<long long>::fail(Err::InvalidArg, "username taken");
    long long id = store.newId();
    store.accounts[id] = Account::create(id, Buyer, name, password);
    store.usernameIndex[name] = id;
    return Result<long long>::good(id);


}

Result<long long> AuthService::login(std::string name,std::string password){
    
    // 登录失败统一提示，不区分“用户不存在”和“密码错误”（REQ-027）
    if (store.usernameIndex.count(name) == 0)
        return Result<long long>::fail(Err::Unauthorized, "invalid username or password");
    long long id = store.usernameIndex[name];
    if (store.accounts[id].password != password)
        return Result<long long>::fail(Err::Unauthorized, "invalid username or password");
    return Result<long long>::good(id);

}

Result<long long> AuthService::createAccount(std::string name,std::string password,Role role){

    std::lock_guard<std::mutex> lock(store.mutexLock);

    if (store.usernameIndex.count(name))
        return Result<long long>::fail(Err::InvalidArg, "username taken");
    long long id = store.newId();
    store.accounts[id] = Account::create(id, role, name, password);
    store.usernameIndex[name] = id;
    return Result<long long>::good(id);

}

Result<Account> AuthService::getAccount(long long id){

    std::lock_guard<std::mutex> lock(store.mutexLock);

    auto it = store.accounts.find(id);
    if (it == store.accounts.end())
        return Result<Account>::fail(Err::NotFound, "invalid account id");
    return Result<Account>::good(it->second);

}

Status AuthService::recharge(long long accountId, double amount) {

    std::lock_guard<std::mutex> lock(store.mutexLock);

    auto it = store.accounts.find(accountId);
    if (it == store.accounts.end())
        return Status::fail(Err::Unauthorized, "invalid account id");
    if (amount <= 0)
        return Status::fail(Err::InvalidArg, "amount must be positive");

    it->second.balance += amount;
    return Status::good();

}