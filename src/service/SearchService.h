// SearchService（REQ-011 商品查询与排序 / REQ-013 售罄商品可搜索）
#pragma once

#include <string>
#include <vector>

#include "common/Clock.h"
#include "common/Result.h"
#include "store/Store.h"

// 查询条件（DTO，不持久化）
struct SearchQuery {
    std::string keyword;                  // 可空；对 title / description 做忽略大小写的包含匹配
    std::string category;                 // 可空；精确匹配
    bool hasPriceRange = false;
    double minPrice = 0;                  // 闭区间下界
    double maxPrice = 0;                  // 闭区间上界；要求 min <= max
};

// 一条搜索结果（DTO，只返回副本）
struct SearchItem {
    long long productId = 0;
    std::string title;
    std::string category;
    double price = 0;
    int remaining = 0;
    bool soldOut = false;                 // Published 且 remaining == 0
    int sales30d = 0;                     // 近 30 天已完成订单件数
    double matchScore = 0;                // 0~100
    double salesScore = 0;                // 0~100，按候选集最大值归一化
    double totalScore = 0;                // 0.8 * matchScore + 0.2 * salesScore
};

struct SearchService {
    explicit SearchService(Store& store, Clock& clock) : store(store), clock(clock) {}

    // 当前阶段只用匹配度与近 30 天销量排序：
    //   匹配度：关键词命中标题记 100，只命中描述记 60，都没命中则不进结果；
    //           关键词为空时统一记 100（相当于只按销量排序）。
    //   销量：近 30 天 OrderStatus::Completed 的订单件数（Order::amount 求和），
    //         按候选商品中的最大值归一化到 0~100。
    //   权重：匹配度 0.8、销量 0.2。
    // 只返回 Published 商品（含售罄），按总分降序；同分按近 30 天销量降序、商品 id 升序。
    Result<std::vector<SearchItem>> search(const SearchQuery& query);

private:
    Store& store;
    Clock& clock;
};
