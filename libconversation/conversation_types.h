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
    // Unix epoch milliseconds. A zero value is filled by ConversationRuntime
    // or the control gateway before the event is sent to desktop clients.
    uint64_t timestamp_ms{0};
    // Per-turn metrics are populated on ReplyFinal. A negative value means
    // that the stage was not executed (for example, a fixed rule reply).
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
