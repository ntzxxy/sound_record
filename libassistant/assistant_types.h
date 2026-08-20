#ifndef ASSISTANT_TYPES_H
#define ASSISTANT_TYPES_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace assistant {

enum class IntentType {
    GeneralChat,
    DeviceControl,
    DeviceFault,
    MemoryWrite,
    MemoryQuery,
    MemoryDelete,
    Clarify
};

using TaskType = IntentType;

struct DeviceCommand {
    std::string room;
    std::string device;
    std::string action;
    std::optional<double> value;
};

struct ResolvedDeviceCommand {
    std::string device_id;
    std::string room;
    std::string device;
    std::string action;
    std::optional<double> value;
    bool valid{false};
};

struct DeviceEvent {
    std::string room;
    std::string device;
    std::string event_type;
    std::string description;
    int64_t timestamp{0};
};

struct MemoryItem {
    std::string category;
    std::string subject;
    std::string attribute;
    std::string value;
    int64_t updated_at{0};
};

struct MemoryQuery {
    std::string subject;
    std::string attribute;
};

struct MemoryDeleteRequest {
    std::string category;
    std::string subject;
    bool delete_all{false};
};

struct IntentResult {
    IntentType intent{IntentType::Clarify};
    std::optional<DeviceCommand> device_command;
    std::optional<DeviceEvent> device_event;
    std::optional<MemoryItem> memory;
    std::optional<MemoryQuery> memory_query;
    std::optional<MemoryDeleteRequest> memory_delete;
    std::vector<std::string> missing_slots;
    std::string clarification_question;
    std::string raw_json;
    int intent_latency_ms{0};
    bool json_valid{false};
};

struct RequestAnalysis {
    IntentType task_type{IntentType::GeneralChat};
    std::optional<DeviceCommand> device_command;
    std::optional<MemoryItem> memory_candidate;
    std::string matched_rule;
};

struct ServiceResult {
    IntentType task_type{IntentType::GeneralChat};
    bool call_llm{true};
    std::string runtime_context;
    std::string fixed_reply;
    std::optional<ResolvedDeviceCommand> device_command;
    std::optional<DeviceEvent> device_event;
    std::optional<MemoryItem> stored_memory;
    std::optional<MemoryDeleteRequest> deleted_memory;
};

const char* toString(IntentType type);
std::optional<IntentType> intentTypeFromString(const std::string& value);
std::string formatDeviceCommand(const ResolvedDeviceCommand& command);
std::string formatDeviceEvent(const DeviceEvent& event);
std::string formatMemoryItem(const MemoryItem& item);

}  // namespace assistant

#endif  // ASSISTANT_TYPES_H
