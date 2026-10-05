#ifndef CONVERSATION_TYPES_H
#define CONVERSATION_TYPES_H

#include "assistant_types.h"

#include <cstdint>
#include <string>

namespace conversation {

enum class InputSource {
    Voice,
    Text,
};

enum class EventType {
    ModeChanged,
    UserMessage,
    IntentResult,
    ToolResult,
    ReplyDelta,
    ReplyFinal,
    Error,
};

struct ConversationRequest {
    std::string text;
    InputSource source{InputSource::Voice};
    std::string session_id{"default"};
    bool enable_tts{true};
    uint64_t turn_id{0};
    int64_t submitted_at_ms{0};
    std::string request_id;
};

struct ConversationEvent {
    EventType type{EventType::Error};
    InputSource source{InputSource::Voice};
    uint64_t turn_id{0};
    std::string request_id;
    std::string text;
    std::string mode;
    std::string intent;
    bool enable_tts{true};
    bool is_final{false};
    // Unix 毫秒时间戳；为 0 时由运行时或控制网关在发送前补齐。
    uint64_t timestamp_ms{0};
    // 性能指标只在 ReplyFinal 中完整填写，负值表示该阶段未执行。
    int64_t intent_latency_ms{-1};
    int prompt_tokens{-1};
    int output_tokens{-1};
    int64_t llm_ttft_ms{-1};
    int64_t llm_prompt_decode_ms{-1};
    int64_t llm_decode_ms{-1};
    double llm_tokens_per_s{0.0};
    bool llm_truncated{false};
    bool has_llm_metrics{false};
};

const char* toString(InputSource source);
const char* toString(EventType type);

}  // namespace conversation

#endif  // CONVERSATION_TYPES_H
