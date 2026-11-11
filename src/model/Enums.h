#pragma once

enum Role : unsigned { Buyer = 1, Seller = 2, Admin = 4 };

enum class ProductStatus { Draft, PendingReview,Published,OffShelf,Hidden };

enum class ReservationStatus {Pending, Confirmed, Completed, Cancelled};

enum class ReviewStatus      { Pending, Approved, Rejected };

// 审核类型：新发布 / 已发布商品修改（REQ-008、REQ-009）
enum class ReviewType        { Create, Modify };

enum class OrderStatus       { PendingPayment, Paid, Shipped, Completed, Closed ,Returned};

enum class EscrowStatus      { Held, Released, Refunded };

enum class ReturnStatus      { Pending, Approved, Shipped, Delivered, Received, Refunded, Rejected, Closed };

// TODO(REQ-006 消息)：在下面新增消息类型枚举，Message::kind 用它：
enum class MessageKind       { Text, Image };
// Text 的 content 存正文；Image 的 content 只存图片地址/相对路径，不存二进制。
// 只需要这两个值，不要复用 OrderStatus 之类的交易枚举。