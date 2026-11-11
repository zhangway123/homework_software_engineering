#pragma once
#include "Commodity.h"
#include "Account.h"
#include "Enums.h"
#include "common/Clock.h"

struct Reservation{
    int id;
    int sellerId;
    int purchaserId;
    int commodityId;

    double commodityPrice;
    int amount;

    Clock::TimePoint createdAt{};
    Clock::TimePoint confirmedAt{};

    ReservationStatus status;



    static Reservation create(long long reservationId,Commodity commodity,Account purchaser,int amount){
        Reservation r;
        r.id = reservationId;
        r.status = ReservationStatus::Pending;
        r.purchaserId = purchaser.id;
        r.sellerId = commodity.sellerId;
        r.commodityId = commodity.id;
        r.amount = amount;
        r.commodityPrice = commodity.price;

        return r;
    }
    
};