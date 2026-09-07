#include "validators.h"

namespace assistant {

bool DeviceCommandValidator::validate(const ResolvedDeviceCommand& command, std::string* error) const {
    if (!command.valid || command.device_id.empty()) {
        if (error) *error = "device_not_found";
        return false;
    }
    if (command.room.empty() || command.device.empty() || command.action.empty()) {
        if (error) *error = "missing_device_slot";
        return false;
    }
    if (command.action != "TURN_ON" && command.action != "TURN_OFF" &&
        command.action != "SET_TEMPERATURE") {
        if (error) *error = "unsupported_action";
        return false;
    }
    const bool is_light = command.device == "灯" ||
                          command.device_id == "bedroom_light" ||
                          command.device_id == "living_room_light";
    if (is_light && command.action == "SET_TEMPERATURE") {
        if (error) *error = "light_temperature_unsupported";
        return false;
    }
    if (command.action == "SET_TEMPERATURE") {
        if (!command.value || *command.value < 16.0 || *command.value > 30.0) {
            if (error) *error = "invalid_temperature";
            return false;
        }
    }
    return true;
}

bool MemoryItemValidator::validate(const MemoryItem& item, std::string* error) const {
    if (item.category != "USER_PREFERENCE" && item.category != "OBJECT_LOCATION" &&
        item.category != "HABIT" && item.category != "ROUTINE" &&
        item.category != "DEVICE_PREFERENCE") {
        if (error) *error = "invalid_memory_category";
        return false;
    }
    if (item.subject.empty() || item.attribute.empty() || item.value.empty()) {
        if (error) *error = "missing_memory_slot";
        return false;
    }
    if (item.confidence < 0 || item.confidence > 100) {
        if (error) *error = "invalid_memory_confidence";
        return false;
    }
    return true;
}

bool DeviceEventValidator::validate(const DeviceEvent& event, std::string* error) const {
    if (event.device.empty()) {
        if (error) *error = "missing_device";
        return false;
    }
    if (event.description.empty()) {
        if (error) *error = "missing_fault_description";
        return false;
    }
    if (event.event_type.empty()) {
        if (error) *error = "missing_event_type";
        return false;
    }
    return true;
}

}  // namespace assistant
