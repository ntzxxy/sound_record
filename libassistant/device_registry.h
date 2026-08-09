#ifndef DEVICE_REGISTRY_H
#define DEVICE_REGISTRY_H

#include "assistant_types.h"

#include <optional>

namespace assistant {

class DeviceRegistry {
public:
    std::optional<ResolvedDeviceCommand> resolve(const DeviceCommand& command) const;
};

}  // namespace assistant

#endif  // DEVICE_REGISTRY_H
