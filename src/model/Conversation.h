// ============================================================================
// Conversation：某个商品下、一对买卖双方之间的消息线程（REQ-005 / REQ-006）

#pragma once
#include "common/Clock.h"

struct Conversation {
    long long id;
    long long productId;   // 会话围绕的商品
    long long buyerId;     // 买家 Account::id
    long long sellerId;    // 卖家 Account::id，由商品推导，不能由前端传入
    Clock::TimePoint createdAt{};
//       // 可选：最后一条消息时间。想做“最近会话排前面”就加；
//       // 不加的话 listConversations 只能按 id / createdAt 排序。
    Clock::TimePoint lastMessageAt{};

    static Conversation create(long long myId, long long myProductId,
                                long long myBuyerId, long long mySellerId,
                                Clock::TimePoint now)
    {
        Conversation c;
        c.id = myId;
        c.productId = myProductId;
        c.buyerId = myBuyerId;
        c.sellerId = mySellerId;
        c.createdAt = now;
        c.lastMessageAt = now;
        return c;                            
    }
};
//
// 唯一性（很重要）：
//   - 同一组 (productId, buyerId, sellerId) 只能存在一条会话（REQ-006 + REQ-021）。
//   - 判重不要靠遍历 conversations，而是在 MessageService::openConversation 里
//     用 Store::conversationIndex 查；整个过程必须在 store.mutexLock 内，
//     否则两个并发“打开会话”会各建一条。
//   - 重复打开应幂等返回已有 id，而不是报错。
//
// 其他约定：
//   - 不要在 Conversation 里内嵌 std::vector<Message>：消息统一放
//     Store::messages，避免同一条消息被拷贝两份、并发写时不一致。
//   - 管理员不是会话参与方，不建“管理员会话”子类；管理员只读查看私信
//     由“举报工单 + 审计日志”授权（REQ-005），属于监管服务，不属于会话本身。
// ============================================================================
