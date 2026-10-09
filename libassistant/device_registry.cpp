#include "device_registry.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace assistant {
namespace {

std::vector<std::string> actionsForDevice(const std::string& device) {
    if (device == "空调") {
        return {"TURN_ON", "TURN_OFF", "SET_TEMPERATURE", "SET_MODE"};
    }
    return {"TURN_ON", "TURN_OFF"};
}

std::vector<RegisteredDevice> defaultDevices() {
    return {
        {"living_room_ac", "客厅", "空调", actionsForDevice("空调"), "simulation"},
        {"bedroom_ac", "卧室", "空调", actionsForDevice("空调"), "simulation"},
        {"living_room_light", "客厅", "灯", actionsForDevice("灯"), "simulation"},
        {"bedroom_light", "卧室", "灯", actionsForDevice("灯"), "simulation"},
    };
}

std::string joinActions(const std::vector<std::string>& actions) {
    std::ostringstream out;
    for (std::size_t i = 0; i < actions.size(); ++i) {
        if (i > 0) out << ',';
        out << actions[i];
    }
    return out.str();
}

std::vector<std::string> splitActions(const std::string& value) {
    std::vector<std::string> actions;
    std::size_t start = 0;
    while (start <= value.size()) {
        std::size_t comma = value.find(',', start);
        if (comma == std::string::npos) comma = value.size();
        const std::string action = value.substr(start, comma - start);
        if (!action.empty()) actions.push_back(action);
        if (comma == value.size()) break;
        start = comma + 1;
    }
    return actions;
}

bool safeField(const std::string& value) {
    return !value.empty() && value.find_first_of("\t\r\n") == std::string::npos;
}

}  // namespace

DeviceRegistry::DeviceRegistry(std::string storage_path)
    : storage_path_(std::move(storage_path)), devices_(defaultDevices()) {}

bool DeviceRegistry::load() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (storage_path_.empty()) return true;
    if (!std::filesystem::exists(storage_path_)) return true;

    std::ifstream in(storage_path_);
    if (!in.is_open()) return false;
    std::vector<RegisteredDevice> loaded;
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> fields;
        std::size_t start = 0;
        while (start <= line.size()) {
            std::size_t tab = line.find('\t', start);
            if (tab == std::string::npos) tab = line.size();
            fields.push_back(line.substr(start, tab - start));
            if (tab == line.size()) break;
            start = tab + 1;
        }
        if (fields.size() < 5 || !safeField(fields[0]) || !safeField(fields[1]) ||
            !safeField(fields[2])) {
            continue;
        }
        RegisteredDevice item;
        item.device_id = fields[0];
        item.room = fields[1];
        item.device = fields[2];
        item.supported_actions = splitActions(fields[3]);
        item.transport = fields[4].empty() ? "simulation" : fields[4];
        if (!item.supported_actions.empty()) loaded.push_back(std::move(item));
    }
    devices_ = std::move(loaded);
    return true;
}

std::optional<ResolvedDeviceCommand> DeviceRegistry::resolve(const DeviceCommand& command) const {
    std::lock_guard<std::mutex> lock(mutex_);
    ResolvedDeviceCommand resolved;
    resolved.room = command.room;
    resolved.device = command.device;
    resolved.action = command.action;
    resolved.value = command.value;
    resolved.mode = command.mode;

    for (const auto& item : devices_) {
        if (item.room != command.room || item.device != command.device) continue;
        resolved.device_id = item.device_id;
        resolved.supported_actions = item.supported_actions;
        resolved.valid = true;
        return resolved;
    }
    return std::nullopt;
}

std::vector<RegisteredDevice> DeviceRegistry::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return devices_;
}

bool DeviceRegistry::add(const std::string& room, const std::string& device,
                         std::string* device_id, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!safeField(room) || !safeField(device)) {
        if (error) *error = "房间和设备名称不能为空，也不能包含制表符或换行。";
        return false;
    }
    for (const auto& item : devices_) {
        if (item.room == room && item.device == device) {
            if (error) *error = "该房间中已经注册了同名设备。";
            return false;
        }
    }

    int suffix = 1;
    std::string generated_id;
    do {
        generated_id = "custom_device_" + std::to_string(suffix++);
    } while (std::any_of(devices_.begin(), devices_.end(), [&](const auto& item) {
        return item.device_id == generated_id;
    }));

    RegisteredDevice item;
    item.device_id = generated_id;
    item.room = room;
    item.device = device;
    item.supported_actions = actionsForDevice(device);
    item.transport = "simulation";
    devices_.push_back(item);
    if (!saveUnlocked()) {
        devices_.pop_back();
        if (error) *error = "设备注册表保存失败。";
        return false;
    }
    if (device_id) *device_id = generated_id;
    return true;
}

bool DeviceRegistry::remove(const std::string& device_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = std::find_if(devices_.begin(), devices_.end(), [&](const auto& item) {
        return item.device_id == device_id;
    });
    if (it == devices_.end()) return false;
    const RegisteredDevice removed = *it;
    devices_.erase(it);
    if (!saveUnlocked()) {
        devices_.push_back(removed);
        return false;
    }
    return true;
}

bool DeviceRegistry::saveUnlocked() const {
    if (storage_path_.empty()) return true;
    const std::filesystem::path target(storage_path_);
    if (!target.parent_path().empty()) {
        std::error_code directory_error;
        std::filesystem::create_directories(target.parent_path(), directory_error);
        if (directory_error) return false;
    }
    const std::filesystem::path temporary = target.string() + ".tmp";
    std::ofstream out(temporary, std::ios::trunc);
    if (!out.is_open()) return false;
    for (const auto& item : devices_) {
        out << item.device_id << '\t' << item.room << '\t' << item.device << '\t'
            << joinActions(item.supported_actions) << '\t' << item.transport << '\n';
    }
    out.close();
    if (!out) return false;
    std::error_code rename_error;
    std::filesystem::rename(temporary, target, rename_error);
    if (rename_error) {
        std::error_code remove_error;
        std::filesystem::remove(target, remove_error);
        rename_error.clear();
        std::filesystem::rename(temporary, target, rename_error);
    }
    return !rename_error;
}

}  // namespace assistant
