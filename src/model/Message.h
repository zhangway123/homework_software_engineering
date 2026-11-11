// ============================================================================
// Message：会话中的单条聊天消息（REQ-006）

#pragma once
#include <string>
#include "Enums.h"          // MessageKind
#include "common/Clock.h"   // sentAt
//

struct Message {
    long long id;
    long long conversationId;
    long long senderId;        // 必须是会话的 buyerId 或 sellerId
    MessageKind kind;          // Text / Image，枚举加在 model/Enums.h
    std::string content;       // Text: 正文；Image: 图片地址/路径（不存二进制） (Image暂时不考虑)
    Clock::TimePoint sentAt{};
    bool read = false;         // 可选：UML 里的“是否已读”，要做未读红点才需要
    static Message create(long long myId, long long myConversationId,
                                long long mySenderId, MessageKind myKind,
                                std::string myContent, Clock::TimePoint now)
    {
        Message m;
        m.id = myId;
        m.conversationId = myConversationId;
        m.senderId = mySenderId;
        m.kind = myKind;
        m.content = myContent;
        m.sentAt =  now;
        return m;                           
    }
};


//
// 校验与约束（放在 MessageService::send，模型只承载数据）：
//   - content 不能为空；Text 去掉首尾空白后不能为空；长度上限建议 2000 字符。
//   - Image 的 content 只接受图片地址/相对路径，建议限制扩展名
//     .png / .jpg / .jpeg / .gif / .webp（大小写不敏感）。
//     当前 WebUI 的 HTTP 服务不支持 multipart 上传，图片要么存外链/静态目录，
//     要么另加一个文件上传接口。
//   - sentAt 必须用注入的 Clock::now() 生成，不要直接 system_clock::now()，
//     否则 FakeClock 下无法测试 30 天留存（REQ-006）。
//   - 消息留存 30 天：删除逻辑放 MessageService::sweepTimeouts，
//     不要在 Message 内部做任何时间判断。
// ============================================================================
