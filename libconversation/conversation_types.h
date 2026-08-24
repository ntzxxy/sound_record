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
};

const char* toString(InputSource source);
const char* toString(EventType type);

}  // namespace conversation

#endif  // CONVERSATION_TYPES_H
