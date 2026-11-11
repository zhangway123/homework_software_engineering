// Message Service（REQ-006：买卖双方沟通；REQ-005 的管理员只读查看见文末说明）
//
// 下面是待实现的完整接口，先以注释形式确定签名与语义：

#pragma once
#include <string>
#include <vector>
#include "common/Result.h"
#include "common/Clock.h"
#include "store/Store.h"




struct MessageService {
    explicit MessageService(Store& store, Clock& clock) : store(store), clock(clock) {}

//       // 买家就某商品发起（或取回）与卖家的会话。
//       // 同一 (productId, buyerId, sellerId) 幂等：已存在就返回原 id，不报错。
    Result<long long> openConversation(long long buyerId, long long productId);
//
//       // 发送一条消息。senderId 必须是会话的 buyerId 或 sellerId。
    Result<long long> send(long long senderId, long long conversationId,
                            MessageKind kind, const std::string& content);
//
//       // 会话内消息，按 sentAt 升序（同一时刻按 id）；调用者必须是参与方。
    Result<std::vector<Message>> list(long long accountId, long long conversationId);
//
//       // 当前账号参与的全部会话，最近活跃的排前面；返回副本供 UI 展示。
   std::vector<Conversation> listConversations(long long accountId);
//
//       // 删除超过 30 天的消息（REQ-006），返回删除条数；由 TimeoutSweeper 调用。
    int sweepTimeouts();
//
private:
    Store& store;
    Clock& clock;
};
//
// 设计约束：
//   - 所有方法进来先 std::lock_guard<std::mutex> lock(store.mutexLock)；
//     “查账户 + 查会话 + 写消息”必须整段持锁（REQ-021 并发一致性）。
//   - 查表一律用 find，不用 operator[]，避免悄悄插入默认对象。
//   - 返回给 UI 的对象都是副本，UI 不持有 Store 内部引用。
//   - 错误码沿用现有 Service 的语义：
//       Unauthorized = 账号/会话不存在；Forbidden = 不是参与方；
//       InvalidArg = 参数非法；InvalidState = 商品状态不允许；
//       NotFound = 明确按 id 查不到。
//
// 可选扩展（需求没有强制，建议二期再做）：
//   - Status markRead(long long accountId, long long conversationId);
//     或 list 时返回 unreadCount，用于未读红点（UML 中消息带“是否已读”）。
//   - 管理员只读查看私信不要加在 MessageService：
//     REQ-005 要求“凭举报/投诉工单”才可查看，应放到
//     ModerationService::viewConversationWithTicket(adminId, ticketId)，
//     校验管理员角色、工单处理中、工单商品与会话商品一致、双方一致，
//     只返回副本，并写 AuditLog（审计优先：写日志失败就不返回内容）。
//     当前仓库还没有 ComplaintTicket / AuditLog，因此本期不要提供
//     “管理员可列出/查看任意会话”的接口。
