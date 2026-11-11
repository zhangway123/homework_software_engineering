#pragma once
#include <string>
#include "Enums.h"
#include "common/Clock.h"

struct Commodity {
    long long id;
    long long sellerId;
    std::string title, description, category;
    double price;
    int remaining;
    ProductStatus status;

    // 最后一次保存时间（草稿自最后一次保存起保留 30 天，REQ-007）
    Clock::TimePoint savedAt{};

    static Commodity create(long long myId,long long mySellerId){
        Commodity c;
        c.id = myId;
        c.sellerId = mySellerId;
        c.status = ProductStatus::Draft;
        return c;
    }

    void modify
    (
        std::string myName,
        std::string myDescription,
        std::string myCategory,
        double myPrice,
        int myRemaining
    )
    {
        title = myName;
        description = myDescription;
        category = myCategory;
        price = myPrice;
        remaining = myRemaining;
    }

    bool can_reserve() const {
        return remaining > 0 && (status == ProductStatus::Published);
    }

    // TODO(REQ-011)：综合排序还缺两个数据来源，实现 SearchService 前先定口径：
    //   1) 发布时间：同分时需要按发布时间倒序。建议加 Clock::TimePoint publishedAt{}，
    //      在 ProductService::review 审核通过（status 置 Published）时写入 clock.now()；
    //      addProduct 只建草稿，不要在这里填。
    //   2) 近 30 天销量：二选一，不要两处同时算——
    //      要么由 Order 统计（Completed 且 receivedAt 在 30 天窗口内，需先让交易流程把 receivedAt 落库），
    //      要么加 int sales30d 在确认收货时累加。
    //   售罄不新增状态：Published 且 remaining == 0 即为售罄，仍可被搜索（REQ-013）。

};