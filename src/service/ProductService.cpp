//Product Service
#include "ProductService.h"

#include <algorithm>
#include <chrono>
#include <mutex>

namespace {
// 草稿自最后一次保存起保留 30 天（REQ-007）
constexpr std::chrono::seconds kDraftRetention{30 * 24 * 60 * 60};

// 只把可编辑字段拷进商品，id / sellerId / status 由服务自己维护
void applyEditableFields(Commodity& target, const Commodity& source) {
    target.title       = source.title;
    target.description = source.description;
    target.category    = source.category;
    target.price       = source.price;
    target.remaining   = source.remaining;
}
}

Result<long long> ProductService::addProduct(long long accountId) {
    // 读账户表和写商品表都要在锁内完成，且不能用 operator[]（会悄悄插入默认账户）
    std::lock_guard<std::mutex> lock(store.mutexLock);

    auto accountIt = store.accounts.find(accountId);
    if (accountIt == store.accounts.end())
        return Result<long long>::fail(Err::Unauthorized, "invalid account id");

    if (!accountIt->second.hasAbility(Seller))
        return Result<long long>::fail(Err::Forbidden,"No Accessibility!");

    long long id = store.newId();
    store.products[id] = Commodity::create(id, accountId);
    store.products[id].savedAt = clock.now();   // 草稿 30 天保留期的起点（REQ-007）
    return Result<long long>::good(id);
}

Status ProductService::saveDraft(
    long long sellerId,
    long long productId,
    const std::string& title,
    const std::string& description,
    const std::string& category,
    double price,
    int remaining
) {
    std::lock_guard<std::mutex> lock(store.mutexLock);

    auto accountIt = store.accounts.find(sellerId);
    if (accountIt == store.accounts.end())
        return Status::fail(Err::Unauthorized, "invalid account id");
    if (!accountIt->second.hasAbility(Seller))
        return Status::fail(Err::Forbidden, "No Accessibility!");

    auto productIt = store.products.find(productId);
    if (productIt == store.products.end())
        return Status::fail(Err::NotFound, "invalid product id");
    if (productIt->second.sellerId != sellerId)
        return Status::fail(Err::Forbidden, "not your product");
    // 只有草稿与已下架商品可编辑；已发布商品须走修改审核（REQ-009），不能直接改
    if (productIt->second.status != ProductStatus::Draft &&
        productIt->second.status != ProductStatus::OffShelf)
        return Status::fail(Err::InvalidState, "only draft or offshelf can be edited");
    if (price < 0 || remaining < 0)
        return Status::fail(Err::InvalidArg, "price and remaining must be non-negative");

    productIt->second.modify(title, description, category, price, remaining);
    // 每次保存都重新计时（REQ-007：自最后一次保存起保留 30 天）
    productIt->second.savedAt = clock.now();
    return Status::good();
}

Result<long long> ProductService::submitForReview(long long sellerId, long long productId) {
    std::lock_guard<std::mutex> lock(store.mutexLock);

    auto accountIt = store.accounts.find(sellerId);
    if (accountIt == store.accounts.end())
        return Result<long long>::fail(Err::Unauthorized, "invalid account id");
    if (!accountIt->second.hasAbility(Seller))
        return Result<long long>::fail(Err::Forbidden, "No Accessibility!");

    auto productIt = store.products.find(productId);
    if (productIt == store.products.end())
        return Result<long long>::fail(Err::NotFound, "invalid product id");
    if (productIt->second.sellerId != sellerId)
        return Result<long long>::fail(Err::Forbidden, "not your product");
    // 草稿可首次提交；已下架商品可编辑后重新提交审核（REQ-025）
    if (productIt->second.status != ProductStatus::Draft &&
        productIt->second.status != ProductStatus::OffShelf)
        return Result<long long>::fail(Err::InvalidState,
                                       "only draft or offshelf can be submitted");

    long long reviewId = store.newId();
    ReviewApplication ra;
    ra.id = reviewId;
    ra.productId = productId;
    ra.sellerId = sellerId;
    ra.reviewerId = 0;
    ra.status = ReviewStatus::Pending;
    ra.type = ReviewType::Create;
    ra.snapshot = productIt->second;      // 待审内容
    ra.pastSnapshot = productIt->second;  // 新发布没有旧版本，占位

    store.reviewApps[reviewId] = ra;
    productIt->second.status = ProductStatus::PendingReview;  // 审核期间不可见（REQ-009）

    return Result<long long>::good(reviewId);
}

Status ProductService::review(
    long long adminId,
    long long reviewApplicationId,
    bool approval,
    const std::string& reason
)
{
    std::lock_guard<std::mutex> lock(store.mutexLock);

    auto adminIt = store.accounts.find(adminId);
    if (adminIt == store.accounts.end())
        return Status::fail(Err::Unauthorized, "invalid account id");

    auto raIt = store.reviewApps.find(reviewApplicationId);
    if (raIt == store.reviewApps.end())
        return Status::fail(Err::Unauthorized, "invalid review application id");

    if (!adminIt->second.hasAbility(Admin))
        return Status::fail(Err::Forbidden,"No Accessibility!");

    ReviewApplication& ra = raIt->second;

    auto productIt = store.products.find(ra.productId);
    if (productIt == store.products.end())
        return Status::fail(Err::Unauthorized, "fault happen in the application");

    if (ra.status != ReviewStatus::Pending)
        return Status::fail(Err::Unauthorized, "invalid status");

    if (approval) {
        ra.status = ReviewStatus::Approved;
        ra.rejectReason.clear();
        applyEditableFields(productIt->second, ra.snapshot);   // 生效待审内容
        productIt->second.status = ProductStatus::Published;
    }
    else {
        ra.status = ReviewStatus::Rejected;
        ra.rejectReason = reason;

        if (ra.type == ReviewType::Modify) {
            // 已发布商品修改被驳回：回滚为修改前版本并恢复可见（REQ-009）
            applyEditableFields(productIt->second, ra.pastSnapshot);
            productIt->second.status = ProductStatus::Published;
        }
        else {
            // 新发布被驳回：商品保持草稿，不可见（REQ-026）
            productIt->second.status = ProductStatus::Draft;
        }
    }

    return Status::good();
}

Status ProductService::removeFromShelf(long long operatorId,long long productId){
    std::lock_guard<std::mutex> lock(store.mutexLock);

    auto accountIt = store.accounts.find(operatorId);
    if (accountIt == store.accounts.end())
        return Status::fail(Err::Unauthorized, "invalid account id");

    auto productIt = store.products.find(productId);
    if (productIt == store.products.end())
        return Status::fail(Err::NotFound, "invalid product id");

    Account& account = accountIt->second;
    Commodity& product = productIt->second;

    // 先鉴权：商品属主卖家或管理员均可下架（REQ-010），
    // 买家与卖家可以是同一账号，因此不能用 hasAbility(Buyer) 直接拒绝
    const bool isOwnerSeller = account.hasAbility(Seller) && product.sellerId == operatorId;
    const bool isAdmin = account.hasAbility(Admin);
    if (!isOwnerSeller && !isAdmin)
        return Status::fail(Err::Forbidden, "No Accessibility!");

    // 鉴权通过后再校验状态，避免向非属主泄露商品状态
    if (product.status != ProductStatus::Published)
        return Status::fail(Err::InvalidState, "only Published products can be removed");

    // 商品被预约时终止进行中的预约并归还占用余量（REQ-010）；
    // 已付款的预约已是 Completed，对应订单由管理员裁决，这里不处理
    for (auto& entry : store.reservations) {
        Reservation& r = entry.second;
        if (r.commodityId != productId)
            continue;
        if (r.status != ReservationStatus::Pending &&
            r.status != ReservationStatus::Confirmed)
            continue;
        r.status = ReservationStatus::Cancelled;
        product.remaining += r.amount;
    }

    product.status = ProductStatus::OffShelf;
    return Status::good();
}



int ProductService::sweepTimeouts() {
    std::lock_guard<std::mutex> lock(store.mutexLock);

    const Clock::TimePoint now = clock.now();
    int removed = 0;

    for (auto it = store.products.begin(); it != store.products.end();) {
        const Commodity& product = it->second;
        // 只清理草稿；审核中 / 已发布 / 已下架的商品走各自的状态机，不在此删除
        if (product.status != ProductStatus::Draft ||
            now - product.savedAt <= kDraftRetention) {
            ++it;
            continue;
        }
        it = store.products.erase(it);
        ++removed;
    }

    return removed;
}

std::vector<ReviewApplication> ProductService::listReviews() {
    std::lock_guard<std::mutex> lock(store.mutexLock);
    std::vector<ReviewApplication> out;
    out.reserve(store.reviewApps.size());
    for (const auto& entry : store.reviewApps) out.push_back(entry.second);
    std::sort(out.begin(), out.end(),
              [](const ReviewApplication& a, const ReviewApplication& b) { return a.id < b.id; });
    return out;
}


std::vector<Commodity> ProductService::listProducts() {
    std::lock_guard<std::mutex> lock(store.mutexLock);
    std::vector<Commodity> out;
    out.reserve(store.products.size());
    for (const auto& entry : store.products) out.push_back(entry.second);
    std::sort(out.begin(), out.end(),
              [](const Commodity& a, const Commodity& b) { return a.id < b.id; });
    return out;
}
