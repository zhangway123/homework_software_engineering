// ViewRecord：商品详情页的一次浏览（REQ-011 浏览量来源）
//
// 待实现模型，先以注释确定字段与约束；实现时再写成 struct。

// 字段
//   long long id;
//   long long productId;
//   long long viewerId;       // 浏览者账号 id
//   Clock::TimePoint at{};    // clock.now()

// 约束与约定
//   - 同一 (productId, viewerId) 24 小时内只记一条，去重由 SearchService::recordView 负责。
//   - 统计口径：近 30 天 UV，即去重后的 viewerId 数量。
//   - 只记录 Published 商品的浏览；草稿/审核中/下架/隐藏商品不记录。
//   - 写入权限只认会话里的 accountId，不能由前端传 viewerId。
