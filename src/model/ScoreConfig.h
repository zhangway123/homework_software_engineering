// ScoreConfig：综合排序权重（REQ-011“权重为可配置项”）
//
// 待实现模型，先以注释确定字段与约束；实现时再写成 struct。

// 字段
//   double matchWeight  = 0.5;   // 匹配度
//   double ratingWeight = 0.2;   // 近期评价
//   double viewWeight   = 0.1;   // 浏览量
//   double salesWeight  = 0.2;   // 销量

// 约束与约定
//   - 存一条即可（Store 里放 ScoreConfig 成员，不必做成 id -> 配置 的表）。
//   - 四个权重之和为 1；读取时若数据损坏（负数 / 全为 0），回退到 0.5/0.2/0.1/0.2。
//   - 管理端可修改权重属于可选扩展，本期只要 SearchService 从配置读取，不写死常量。
