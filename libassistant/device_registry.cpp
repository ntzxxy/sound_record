#include "device_registry.h"

namespace assistant {

std::optional<ResolvedDeviceCommand> DeviceRegistry::resolve(const DeviceCommand& command) const {
    ResolvedDeviceCommand resolved;
    resolved.room = command.room;
    resolved.device = command.device;
    resolved.action = command.action;
    resolved.value = command.value;

    if (command.room == "客厅" && command.device == "空调") {
        resolved.device_id = "living_room_ac";
    } else if (command.room == "卧室" && command.device == "空调") {
        resolved.device_id = "bedroom_ac";
    } else if (command.room == "客厅" && command.device == "灯") {
        resolved.device_id = "living_room_light";
    } else if (command.room == "卧室" && command.device == "灯") {
        resolved.device_id = "bedroom_light";
    } else {
        return std::nullopt;
    }
    resolved.valid = true;
    return resolved;
}

}  // namespace assistant
