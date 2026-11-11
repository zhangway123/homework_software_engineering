// ============================================================================
// MessageService 实现说明（按注释顺序填代码；现在不写实现）

#include "MessageService.h"
#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
//
// 匿名 namespace 里的常量与小工具：
namespace{
    constexpr std::chrono::seconds kMessageRetention{30 * 24 * 60 * 60};  // REQ-006
    constexpr std::size_t kMaxContentLength = 2000;

    //   std::string conversationKey(long long productId, long long buyerId, long long sellerId)
    //       -> std::to_string(productId) + ":" + std::to_string(buyerId) + ":"
    //          + std::to_string(sellerId)
    std::string conversationKey(long long productId, long long buyerId, long long sellerId){
        return std::to_string(productId) + ":" + std::to_string(buyerId) + ":" + std::to_string(sellerId);

    }
    //   bool isParticipant(const Conversation& c, long long accountId)
    //       -> c.buyerId == accountId || c.sellerId == accountId
    bool isParticipant(const Conversation& c, long long accountId){
        return c.buyerId == accountId || c.sellerId == accountId;
    }

    //   bool looksLikeImageUrl(const std::string& s)
    //       -> 非空、长度 <= kMaxContentLength、后缀（忽略大小写）在
    //          .png/.jpg/.jpeg/.gif/.webp 内
    
}


// 通用约定：
//   - 每个 public 方法第一行都加 std::lock_guard<std::mutex> lock(store.mutexLock);
//   - store.newId() 只能在持锁时调用；
//   - 查表用 find，不用 operator[]；
//   - 先鉴权、再校验内容，避免向无关用户泄露“会话是否存在”；
//   - 时间一律 clock.now()，不要直接取 system_clock。
// ============================================================================

// ----------------------------------------------------------------------------
// Result<long long> MessageService::openConversation(long long buyerId, long long productId)
//
// 1) 加锁。
// 2) store.accounts.find(buyerId)：不存在 -> fail(Unauthorized, "invalid account id")。
// 3) 校验买家能力：account.hasAbility(Buyer)；不满足 -> fail(Forbidden, "No Accessibility!")。
//    （管理员与买卖角色互斥，不能靠 Admin 身份绕过；一个账号可同时是 Buyer|Seller。）
// 4) store.products.find(productId)：不存在 -> fail(NotFound, "invalid product id")。
// 5) 商品必须处于 ProductStatus::Published：买家看不到草稿/待审核/已下架商品。
//    已有会话在商品下架后仍应可读，但不再允许新建 -> fail(InvalidState, "...")。
// 6) sellerId = product.sellerId；若 sellerId == buyerId -> fail(InvalidArg, "cannot talk to yourself")。
//    卖家身份只从商品推导，绝不从请求体接收。
// 7) key = conversationKey(productId, buyerId, sellerId)；
//    在 store.conversationIndex 里查：
//      命中 -> return good(已有 conversationId)（幂等，不新建）。
//    这一步和下面的写入都在同一把锁内，保证并发打开只产生一条会话。
// 8) id = store.newId()；
//    Conversation c = Conversation::create(id, productId, buyerId, sellerId, clock.now());
//    store.conversations[id] = c；store.conversationIndex[key] = id；
//    return good(id)。
// ----------------------------------------------------------------------------
Result<long long> MessageService::openConversation(long long buyerId, long long productId){
    std::lock_guard<std::mutex> lock(store.mutexLock);

    auto buyerIt = store.accounts.find(buyerId);
    if(buyerIt == store.accounts.end()){
        return Result<long long>::fail(Err::Unauthorized, "invalid account id");
    }

    // 先鉴权再查商品：否则无买家能力的账号能从错误码差异里探测商品是否存在
    if(!buyerIt->second.hasAbility(Buyer)){
        return Result<long long>::fail(Err::Forbidden, "No Accessibility!");
    }

    auto productIt = store.products.find(productId);
    if(productIt == store.products.end()){
        return Result<long long>::fail(Err::NotFound, "invalid product id");
    }

    const Commodity& p = productIt->second;

    if(!(p.status == ProductStatus::Published)){
        return Result<long long>::fail(Err::InvalidState, "status fault!");
    }

    if(buyerId == p.sellerId){
        return Result<long long>::fail(Err::InvalidArg, "cannot talk to yourself!");
    }

    std::string key = conversationKey(productId,buyerId,p.sellerId);

    auto keyIt = store.conversationIndex.find(key);

    if(keyIt == store.conversationIndex.end()){
        long long conversationId = store.newId();
        Conversation conversation = Conversation::create(
            conversationId,
            productId,
            buyerId,
            p.sellerId,
            clock.now()
        );

        store.conversations[conversationId] = conversation;
        store.conversationIndex[key] = conversationId;

        return Result<long long>::good(conversationId);
    }
    else{
        return Result<long long>::good(keyIt->second);
    }




}



// Result<long long> MessageService::send(long long senderId, long long conversationId,
//                                        MessageKind kind, const std::string& content)
//
// 1) 加锁。
// 2) store.conversations.find(conversationId)：不存在 -> fail(NotFound, "invalid conversation id")。
// 3) isParticipant(conversation, senderId)：不满足 -> fail(Forbidden, "not a participant")。
//    先做这一步，再校验内容，避免非参与方探测会话是否存在。
// 4) 内容校验：
//    - content 为空 -> fail(InvalidArg, "empty content")。
//    - kind == MessageKind::Text：
//        * 去掉首尾空白后不能为空；
//        * content.size() <= kMaxContentLength。
//    - kind == MessageKind::Image：
//        * looksLikeImageUrl(content) 为真，否则 fail(InvalidArg, "invalid image url")。
//        * 只存地址，不读文件、不存二进制。
//    - 其他 kind -> fail(InvalidArg, "invalid message kind")（防御性）。
// 5) id = store.newId()；
//    Message m = Message::create(id, conversationId, senderId, kind, content, clock.now());
//    store.messages[id] = m；
//    若 Conversation 有 lastMessageAt，则同步更新为 m.sentAt；
//    可选：把 id 追加到 Store::messageIdsByConversation[conversationId]。
// 6) return good(id)。
// 7) 通知对方（可选）：当前仓库没有 Notification 模块；若以后加，
//    应该在锁外再发通知，避免持锁调用另一个服务造成死锁。
// ----------------------------------------------------------------------------

Result<long long> MessageService::send(long long senderId, long long conversationId,
                                           MessageKind kind, const std::string& content)
{
    std::lock_guard<std::mutex> lock(store.mutexLock);
    auto conversationIt = store.conversations.find(conversationId);
    if(conversationIt == store.conversations.end()){
        return Result<long long>::fail(Err::NotFound, "invalid conversation id");
    }
    Conversation& c = conversationIt->second;
    if(!isParticipant(c, senderId)){
        return Result<long long>::fail(Err::Forbidden, "not a participant");
    }

    if(content.empty()){
        return Result<long long>::fail(Err::InvalidArg, "empty content");
    }

    if(kind == MessageKind::Text){
        if (content.length() > kMaxContentLength){
            return Result<long long>::fail(Err::InvalidArg, "over max content length!");
        }
    }
    else if(kind == MessageKind::Image){
        // TODO(REQ-006)：图片消息暂未开放，先整体拒绝，别把任意字符串当图片地址入库；
        // 之后补上 looksLikeImageUrl 再放开。
        return Result<long long>::fail(Err::InvalidArg, "image message not supported yet");
    }
    else{
        // 防御性：非法 kind 不能入库
        return Result<long long>::fail(Err::InvalidArg, "invalid message kind");
    }
    //create mes
    long long id = store.newId();
    Message m = Message::create(id, conversationId, senderId, kind, content, clock.now());


    store.messages[id] = m;

    c.lastMessageAt = m.sentAt;

    //把 id 追加到 Store::messageIdsByConversation[conversationId]。
    store.messageIdsByConversation[conversationId].push_back(id);

    return Result<long long>::good(id);
}






// ----------------------------------------------------------------------------
// Result<std::vector<Message>> MessageService::list(long long accountId, long long conversationId)
//
// 1) 加锁。
// 2) 查会话：不存在 -> fail(NotFound, "invalid conversation id")。
// 3) isParticipant 校验：不满足 -> fail(Forbidden, "not a participant")。
// 4) 遍历 store.messages（或只遍历 messageIdsByConversation[conversationId]）：
//    - 只收 conversationId 相同的消息（拷贝进 vector<Message>）；
//    - 防御性过滤 now - sentAt > kMessageRetention 的消息，即使 sweeper 还没执行。
// 5) 排序：先 sentAt 升序；时间相同（FakeClock 下常见）再按 id 升序。
// 6) return good(std::move(out))。
// 注意：只读接口不要顺手改 read 标记；要做未读状态就单独 markRead。
// ----------------------------------------------------------------------------
Result<std::vector<Message>> MessageService::list(long long accountId, long long conversationId){
    std::lock_guard<std::mutex> lock(store.mutexLock);
    auto conversationIt = store.conversations.find(conversationId);
    if(conversationIt == store.conversations.end()){
        return Result<std::vector<Message>>::fail(Err::NotFound, "invalid conversation id");
    }
    Conversation& c = conversationIt->second;
    if(!isParticipant(c, accountId)){
        return Result<std::vector<Message>>::fail(Err::Forbidden, "not a participant");
    }

    std::vector<Message> out;
    auto idsIt = store.messageIdsByConversation.find(conversationId);
    if (idsIt != store.messageIdsByConversation.end()) {
        const auto now = clock.now();
        out.reserve(idsIt->second.size());
        for (long long id : idsIt->second) {
            auto msgIt = store.messages.find(id);
            if (msgIt == store.messages.end()) continue;          // sweeper 已删
            const Message& m = msgIt->second;
            if (m.conversationId != conversationId) continue;      // 防御
            if (now - m.sentAt > kMessageRetention) continue;      // sweeper 还没跑
            out.push_back(m);
        }
    }
    std::sort(out.begin(), out.end(), [](const Message& a, const Message& b) {
        if (a.sentAt != b.sentAt) return a.sentAt < b.sentAt;
        return a.id < b.id;
    });
    return Result<std::vector<Message>>::good(std::move(out));



}





// ----------------------------------------------------------------------------
// std::vector<Conversation> MessageService::listConversations(long long accountId)
//
// 1) 加锁。
// 2) 遍历 store.conversations，筛 buyerId == accountId || sellerId == accountId，拷贝。
// 3) 排序：按最后活跃时间倒序（有 lastMessageAt 用它，否则用 createdAt），
//    再按 id 倒序，保证顺序稳定；最近的会话排最前。
// 4) accountId 不存在时返回空列表即可（UI 层会先要求登录）。
// 说明：不返回“管理员可见的会话”，管理员查看走工单授权（REQ-005）。
// ----------------------------------------------------------------------------

std::vector<Conversation> MessageService::listConversations(long long accountId){
    std::lock_guard<std::mutex> lock(store.mutexLock);

    auto accountIt = store.accounts.find(accountId);
    if(accountIt == store.accounts.end()){
        return {};
    }


    std::vector<Conversation> Convs;
    for(const auto& conv : store.conversations){
        if(conv.second.buyerId == accountId || conv.second.sellerId == accountId){
            Convs.push_back(conv.second);
        }
    }

    // 最近活跃的排最前：有 lastMessageAt 用它，否则退回 createdAt；同一时刻按 id 倒序保证稳定
    auto activeAt = [](const Conversation& c) {
        return c.lastMessageAt != Clock::TimePoint{} ? c.lastMessageAt : c.createdAt;
    };
    std::sort(Convs.begin(), Convs.end(), [&activeAt](const Conversation& a, const Conversation& b) {
        const auto ta = activeAt(a);
        const auto tb = activeAt(b);
        if (ta != tb) return ta > tb;
        return a.id > b.id;
    });

    return Convs;
    

}




// ----------------------------------------------------------------------------
// int MessageService::sweepTimeouts()
//
// 1) 加锁；const auto now = clock.now(); int removed = 0;
// 2) 遍历 store.messages：
//      if (now - msg.sentAt > kMessageRetention) {
//          it = store.messages.erase(it);
//          ++removed;
//      } else {
//          ++it;
//      }
//    必须用 erase 返回的迭代器继续循环，不能在 erase 后 ++it。
// 3) 可选：清理由此变空的会话（没有任何消息且 createdAt 超过 30 天），
//    删除时同时 erase store.conversationIndex 中对应的 key，
//    否则索引会指向已删除的会话。若本期不做，会话表会缓慢增长，可接受。
// 4) return removed；TimeoutSweeper 会把它计入 changeCount 并输出日志。
// ----------------------------------------------------------------------------


int MessageService::sweepTimeouts(){
    std::lock_guard<std::mutex> lock(store.mutexLock);

    const auto now = clock.now(); int removed = 0;

    for(auto it = store.messages.begin();it != store.messages.end();){
        if (now - it->second.sentAt > kMessageRetention) {
            it = store.messages.erase(it);
            ++removed;
        } else {
          ++it;
        }
    }

    // 同步清掉按会话缓存里那些已不存在的 message id，否则它们会无限累积
    for(auto vit = store.messageIdsByConversation.begin();
        vit != store.messageIdsByConversation.end();){
        auto& ids = vit->second;
        ids.erase(std::remove_if(ids.begin(), ids.end(), [this](long long msgId){
            return store.messages.find(msgId) == store.messages.end();
        }), ids.end());
        if(ids.empty()){
            vit = store.messageIdsByConversation.erase(vit);
        } else {
            ++vit;
        }
    }

//   3) 可选：清理由此变空的会话（没有任何消息且 createdAt 超过 30 天），
//    删除时同时 erase store.conversationIndex 中对应的 key，
//    否则索引会指向已删除的会话。若本期不做，会话表会缓慢增长，可接受。

    return removed;

}