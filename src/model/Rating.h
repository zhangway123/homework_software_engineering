// Rating：买家对已完成订单的评价（REQ-011 排序数据来源；UML UC-27）
//
// 待实现模型，先以注释确定字段与约束；实现时再写成 struct。

// 字段
//   long long id;
//   long long orderId;        // 一单一评，orderId 唯一
//   long long productId;
//   long long buyerId;
//   int score;                // 1~5 星
//   std::string content;      // 可空
//   Clock::TimePoint at{};    // 必须来自 clock.now()，供“近 30 天评价”统计

// 约束与约定
//   - 只有 OrderStatus::Completed 的订单可以评价；同一订单只能评一次。
//   - 评价写入需要新的服务方法（TradeService 或单独的 ReviewService），
//     本期 SearchService 只读不写。
//   - 需求未定义评价入口（UML 列为派生），没有数据时 SearchService 回退分类均值。
//   - 与审核（ReviewApplication）不是一回事，不要复用。
