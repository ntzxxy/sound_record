#include "assistant_types.h"

#include <sstream>

namespace assistant {

const char* toString(IntentType type) {
    switch (type) {
        case IntentType::GeneralChat: return "GENERAL_CHAT";
        case IntentType::DeviceControl: return "DEVICE_CONTROL";
        case IntentType::DeviceFault: return "DEVICE_FAULT";
        case IntentType::MemoryWrite: return "MEMORY_WRITE";
        case IntentType::MemoryQuery: return "MEMORY_QUERY";
        case IntentType::MemoryDelete: return "MEMORY_DELETE";
        case IntentType::RecordQuery: return "RECORD_QUERY";
        case IntentType::WeatherQuery: return "WEATHER_QUERY";
        case IntentType::Clarify: return "CLARIFY";
    }
    return "CLARIFY";
}

std::optional<IntentType> intentTypeFromString(const std::string& value) {
    if (value == "GENERAL_CHAT") return IntentType::GeneralChat;
    if (value == "DEVICE_CONTROL") return IntentType::DeviceControl;
    if (value == "DEVICE_FAULT") return IntentType::DeviceFault;
    if (value == "MEMORY_WRITE") return IntentType::MemoryWrite;
    if (value == "MEMORY_QUERY") return IntentType::MemoryQuery;
    if (value == "MEMORY_DELETE") return IntentType::MemoryDelete;
    if (value == "RECORD_QUERY") return IntentType::RecordQuery;
    if (value == "WEATHER_QUERY") return IntentType::WeatherQuery;
    if (value == "CLARIFY") return IntentType::Clarify;
    return std::nullopt;
}

const char* toString(RecordType type) {
    switch (type) {
        case RecordType::All: return "ALL";
        case RecordType::DeviceFault: return "DEVICE_FAULT";
        case RecordType::UserPreference: return "USER_PREFERENCE";
        case RecordType::ObjectLocation: return "OBJECT_LOCATION";
    }
    return "ALL";
}

std::optional<RecordType> recordTypeFromString(const std::string& value) {
    if (value == "ALL") return RecordType::All;
    if (value == "DEVICE_FAULT") return RecordType::DeviceFault;
    if (value == "USER_PREFERENCE") return RecordType::UserPreference;
    if (value == "OBJECT_LOCATION") return RecordType::ObjectLocation;
    return std::nullopt;
}

std::string formatDeviceCommand(const ResolvedDeviceCommand& command) {
    std::ostringstream oss;
    oss << "device_id=" << command.device_id << '\n'
        << "room=" << command.room << '\n'
        << "device=" << command.device << '\n'
        << "action=" << command.action << '\n'
        << "value=";
    if (command.value) {
        oss << *command.value;
    } else {
        oss << "<none>";
    }
    oss << '\n' << "mode=simulation";
    return oss.str();
}

std::string formatDeviceEvent(const DeviceEvent& event) {
    std::ostringstream oss;
    oss << "room=" << (event.room.empty() ? "<unknown>" : event.room) << '\n'
        << "device=" << (event.device.empty() ? "<unknown>" : event.device) << '\n'
        << "event_type=" << event.event_type << '\n'
        << "description=" << event.description << '\n'
        << "timestamp=" << event.timestamp;
    return oss.str();
}

std::string formatMemoryItem(const MemoryItem& item) {
    std::ostringstream oss;
    oss << "category=" << item.category << '\n'
        << "subject=" << item.subject << '\n'
        << "attribute=" << item.attribute << '\n'
        << "value=" << item.value << '\n'
        << "updated_at=" << item.updated_at;
    return oss.str();
}

}  // namespace assistant
