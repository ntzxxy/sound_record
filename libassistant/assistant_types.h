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
    RecordQuery,
    WeatherQuery,
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
    // Conditions distinguish a global preference from one that applies only
    // during a scene such as reading or sleeping.
    std::string condition;
    std::string context;
    std::string time;
    std::string scope;
    int confidence{100};
};

struct MemoryQuery {
    std::string subject;
    std::string attribute;
    std::string condition;
    std::string scope;
};

struct MemoryDeleteRequest {
    std::string category;
    std::string subject;
    bool delete_all{false};
};

// 记录中心的分类。设备故障是可重复发生的历史事件，不能与会覆盖更新的
// 用户记忆混为同一条记录，因此仅在查询和界面展示时统一处理。
enum class RecordType {
    All,
    DeviceFault,
    UserPreference,
    ObjectLocation
};

struct RecordQuery {
    RecordType type{RecordType::All};
};

// 天气请求只保存语义层已明确给出的参数。日期为空时由服务端读取本机
// 系统时钟补齐，不能由 LLM 猜测“今天”的具体日期。
struct WeatherQuery {
    std::string city;
    std::string start_date;
    std::string end_date;
    int days{0};
};

struct IntentResult {
    IntentType intent{IntentType::Clarify};
    std::optional<DeviceCommand> device_command;
    std::optional<DeviceEvent> device_event;
    std::optional<MemoryItem> memory;
    std::optional<MemoryQuery> memory_query;
    std::optional<MemoryDeleteRequest> memory_delete;
    std::optional<RecordQuery> record_query;
    std::optional<WeatherQuery> weather_query;
    // Local-only selectors for injecting memory into a normal chat turn.
    std::optional<MemoryQuery> memory_context_query;
    bool include_recent_memory_context{false};
    std::vector<std::string> missing_slots;
    std::string clarification_question;
    // Optional user-facing text from a validated structured result.
    std::string response_text;
    std::string raw_json;
    int intent_latency_ms{0};
    bool json_valid{false};
    // True only when a deterministic local parser produced this decision.  It
    // lets the executor keep a fixed reply on the fast path instead of
    // needlessly asking the chat model to phrase a confirmation.
    bool local_route{false};
};

enum class LocalRouteStatus {
    // Plain language: skip intent extraction and call the chat model once.
    Chat,
    // Business-like but not deterministic: ask Gemma for structured intent.
    SemanticFallback,
    // Deterministically parsed and validated locally.
    FastPath
};

struct RequestAnalysis {
    LocalRouteStatus status{LocalRouteStatus::Chat};
    IntentResult intent;
    std::string matched_rule;
    // Constant, locally derived candidate type.  It is intentionally not an
    // extracted fact and must never be persisted or executed by itself.
    std::string semantic_hint;
};

struct ServiceResult {
    IntentType task_type{IntentType::GeneralChat};
    // Carried from IntentResult for UI/benchmark observability. It does not
    // participate in routing or alter the selected task.
    int intent_latency_ms{0};
    bool call_llm{true};
    std::string runtime_context;
    std::string fixed_reply;
    std::string tool_name;
    std::string tool_result_json;
    std::optional<ResolvedDeviceCommand> device_command;
    std::optional<DeviceEvent> device_event;
    std::optional<MemoryItem> stored_memory;
    std::optional<MemoryDeleteRequest> deleted_memory;
};

const char* toString(IntentType type);
std::optional<IntentType> intentTypeFromString(const std::string& value);
const char* toString(RecordType type);
std::optional<RecordType> recordTypeFromString(const std::string& value);
std::string formatDeviceCommand(const ResolvedDeviceCommand& command);
std::string formatDeviceEvent(const DeviceEvent& event);
std::string formatMemoryItem(const MemoryItem& item);

}  // namespace assistant

#endif  // ASSISTANT_TYPES_H
