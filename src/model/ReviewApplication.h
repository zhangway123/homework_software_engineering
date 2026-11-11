#pragma once
#include <string>
#include "Enums.h"
#include "Commodity.h"

struct ReviewApplication{
    long long id,productId,sellerId,reviewerId;
    std::string rejectReason;
    ReviewStatus status;
    ReviewType type = ReviewType::Create;

    Commodity pastSnapshot;
    Commodity snapshot;
};