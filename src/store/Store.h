#pragma once
#include "model/Enums.h"
#include "model/Commodity.h"
#include "model/EscrowRecord.h"
#include <mutex>
#include "model/Account.h"
#include "model/ReviewApplication.h"
#include "model/Reservation.h"
#include "model/Order.h"
#include "model/ReturnRequest.h"
// TODO(REQ-006)：实现会话/消息后启用下面两个 include（当前是注释，避免空模型参与编译）
#include "model/Conversation.h"
#include "model/Message.h"

#include <string>
#include <unordered_map>
#include <vector>





struct Store{
    std::mutex mutexLock;
    long long nextId = 1;
    long long newId() {return nextId ++;};

    std::unordered_map<std::string, long long> usernameIndex;  // 唯一性用



    std::unordered_map<long long, Account> accounts;
    std::unordered_map<long long, Commodity> products;
    std::unordered_map<long long, ReviewApplication> reviewApps;
    std::unordered_map<long long, Reservation> reservations;
    std::unordered_map<long long, Order> orders;
    std::unordered_map<long long, EscrowRecord> escrows;
    std::unordered_map<long long, ReturnRequest> returnRequests;

    // TODO(REQ-006)：会话与消息表
    std::unordered_map<long long, Conversation> conversations;
    std::unordered_map<long long, Message> messages;
    //
    // 建议同时加索引（不加就只能每次线性扫描）：
    //   (productId, buyerId, sellerId) -> conversationId，保证同一组合只有一条会话
    std::unordered_map<std::string, long long> conversationIndex;
    //
    // 可选：按会话缓存消息 id，list 时不必遍历全部消息
    std::unordered_map<long long, std::vector<long long>> messageIdsByConversation;
    //
    // 约定：以上所有表都受 store.mutexLock 保护，newId() 也只能持锁时调用。
    // messages 按发送时间保留 30 天，由 MessageService::sweepTimeouts 清理。

    // TODO(REQ-011)：搜索排序需要的模型与表（模型文件已按注释建好，实现时再启用）：
    //   #include "model/Rating.h"       -> std::unordered_map<long long, Rating> ratings;
    //   #include "model/ViewRecord.h"   -> std::unordered_map<long long, ViewRecord> viewRecords;
    //   #include "model/ScoreConfig.h"  -> ScoreConfig scoreConfig;   // 单条，默认 0.5/0.2/0.1/0.2
    // 可选索引（不加就线性扫描）：
    //   std::unordered_map<long long, std::vector<long long>> ratingsByProduct;
    //   std::unordered_map<std::string, long long> viewKeyIndex;  // "productId:viewerId" -> 最近一条 viewId
    // 读侧只用 find，不新增/清理的写入（24 小时去重、近 30 天统计）都在 SearchService 里做。


};