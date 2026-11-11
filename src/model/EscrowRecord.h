#pragma once

#include "common/Clock.h"
#include "Enums.h"
#include "model/Order.h"


struct EscrowRecord {

    long long id,orderId;
    double amount;
    EscrowStatus status;
    Clock::TimePoint heldAt{};
    Clock::TimePoint releasedAt{};
    Clock::TimePoint refundedAt{};

    static EscrowRecord create(long long myId,const Order& order){
        EscrowRecord er;
        er.id = myId;
        er.orderId = order.id;
        er.amount = order.totalPrice;
        er.status = EscrowStatus::Held;
        return er;
    }
};