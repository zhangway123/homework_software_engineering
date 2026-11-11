// SearchService：REQ-011 商品查询与排序（当前阶段只用匹配度 + 近 30 天销量）
#include "SearchService.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr double kMatchWeight = 0.8;
constexpr double kSalesWeight = 0.2;
constexpr std::chrono::hours kSalesWindow{24 * 30};

std::string trim(const std::string& s) {
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(begin, end - begin);
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

Result<std::vector<SearchItem>> SearchService::search(const SearchQuery& query) {
    std::lock_guard<std::mutex> lock(store.mutexLock);

    if (query.hasPriceRange && (query.minPrice < 0 || query.maxPrice < query.minPrice)) {
        return Result<std::vector<SearchItem>>::fail(Err::InvalidArg, "invalid price range");
    }

    const auto now = clock.now();
    const std::string keyword = toLower(trim(query.keyword));
    const std::string category = trim(query.category);

    auto countSales = [&](long long productId) {
        long long total = 0;
        for (const auto& entry : store.orders) {
            const Order& order = entry.second;
            if (static_cast<long long>(order.commodityId) != productId) continue;
            if (order.status != OrderStatus::Completed) continue;
            const auto age = now - order.receivedAt;
            if (age < std::chrono::seconds::zero() || age > kSalesWindow) continue;
            total += order.amount > 0 ? order.amount : 1;
        }
        return static_cast<int>(total);
    };

    std::vector<SearchItem> items;
    for (const auto& entry : store.products) {
        const Commodity& product = entry.second;
        if (product.status != ProductStatus::Published) continue;
        if (!category.empty() && product.category != category) continue;
        if (query.hasPriceRange &&
            (product.price < query.minPrice || product.price > query.maxPrice)) {
            continue;
        }

        double matchScore = 100.0;
        if (!keyword.empty()) {
            if (contains(toLower(product.title), keyword)) {
                matchScore = 100.0;
            } else if (contains(toLower(product.description), keyword)) {
                matchScore = 60.0;
            } else {
                continue;
            }
        }

        SearchItem item;
        item.productId = product.id;
        item.title = product.title;
        item.category = product.category;
        item.price = product.price;
        item.remaining = product.remaining;
        item.soldOut = product.remaining == 0;
        item.matchScore = matchScore;
        item.sales30d = countSales(product.id);
        items.push_back(std::move(item));
    }

    int maxSales = 0;
    for (const SearchItem& item : items) {
        maxSales = std::max(maxSales, item.sales30d);
    }

    for (SearchItem& item : items) {
        item.salesScore = maxSales > 0 ? 100.0 * item.sales30d / maxSales : 0.0;
        item.totalScore = kMatchWeight * item.matchScore + kSalesWeight * item.salesScore;
    }

    std::sort(items.begin(), items.end(), [](const SearchItem& a, const SearchItem& b) {
        if (a.totalScore != b.totalScore) return a.totalScore > b.totalScore;
        if (a.sales30d != b.sales30d) return a.sales30d > b.sales30d;
        return a.productId < b.productId;
    });

    return Result<std::vector<SearchItem>>::good(std::move(items));
}
