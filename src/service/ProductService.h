//Product Service
#pragma once

#include <string>
#include <vector>
#include "common/Result.h"
#include "common/Clock.h"
#include "store/Store.h"

struct ProductService {

    explicit ProductService(Store& store, Clock& clock):store(store),clock(clock){}

    // 卖家创建商品草稿（REQ-004 / REQ-007）
    Result<long long> addProduct(long long accountId);

    // 卖家编辑自己的草稿（REQ-007）
    Status saveDraft(
        long long sellerId,
        long long productId,
        const std::string& title,
        const std::string& description,
        const std::string& category,
        double price,
        int remaining
    );

    // 草稿提交审核，返回审核申请 id（REQ-008）
    Result<long long> submitForReview(long long sellerId, long long productId);

    // 管理员审核（REQ-008 / REQ-009 / REQ-026）
    Status review(
        long long adminId,
        long long reviewApplicationId,
        bool approval,
        const std::string& reason
    );


    // 下架商品功能（卖家只能下架自己的商品，管理员可下架任意商品，REQ-010）
    Status removeFromShelf(long long operatorId,long long productId);

    // 清理超过 30 天未保存的草稿（REQ-007），返回本次删除的草稿数
    int sweepTimeouts();

    // 只读查询，供 UI 展示（返回副本，调用方不持有 Store 内部引用）
    
    std::vector<ReviewApplication> listReviews();

    std::vector<Commodity> listProducts();

private:
    Store& store;
    Clock& clock;
};
