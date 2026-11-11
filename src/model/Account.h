#pragma once
#include <string>
#include "Enums.h"

struct Account{
    long long id;
    Role role;
    std::string userName;
    std::string password;
    double balance;

    static Account create(long long myId,Role myRole,std::string myName,std::string myPassword){
        Account a;
        a.id = myId;
        a.role = myRole;
        a.userName = myName;
        a.password = myPassword;
        a.balance = 0;   // 新账户余额从 0 开始，避免未初始化值参与交易计算
        return a;
    }


    bool hasAbility(Role r){
        return (role & r) != 0;
    }
};