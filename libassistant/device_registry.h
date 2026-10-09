#ifndef DEVICE_REGISTRY_H
#define DEVICE_REGISTRY_H

#include "assistant_types.h"

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace assistant {

class DeviceRegistry {
public:
    explicit DeviceRegistry(std::string storage_path = "");

    bool load();
    std::optional<ResolvedDeviceCommand> resolve(const DeviceCommand& command) const;
    std::vector<RegisteredDevice> snapshot() const;
    bool add(const std::string& room, const std::string& device,
             std::string* device_id, std::string* error);
    bool remove(const std::string& device_id);

private:
    bool saveUnlocked() const;

    std::string storage_path_;
    std::vector<RegisteredDevice> devices_;
    mutable std::mutex mutex_;
};

}  // namespace assistant

#endif  // DEVICE_REGISTRY_H
