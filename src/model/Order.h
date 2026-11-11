#pragma once
#include "Reservation.h"
#include "Enums.h"
#include "common/Clock.h"

struct Order{
    int id;
    int reservationId;
    int buyerId;
    int sellerId;
    int commodityId;

    double commodityPrice;
    int amount;

    double totalPrice;

    OrderStatus status;

    Clock::TimePoint payDeadline{}, paidAt{}, shippedAt{}, receivedAt{};

    static Order create(long long orderId,Reservation res,Account buyer){
        Order o;
        o.id = orderId;
        o.reservationId = res.id;
        o.buyerId = buyer.id;
        o.sellerId = res.sellerId;
        o.commodityId = res.commodityId;

        o.commodityPrice = res.commodityPrice;
        o.amount = res.amount;
        o.totalPrice = o.commodityPrice * o.amount;

        o.status = OrderStatus::PendingPayment;

        // 时间字段（payDeadline / paidAt / shippedAt / receivedAt）按约定暂不填写
        return o;
    }



};