#ifndef ASSISTANT_VALIDATORS_H
#define ASSISTANT_VALIDATORS_H

#include "assistant_types.h"

#include <string>

namespace assistant {

class DeviceCommandValidator {
public:
    bool validate(const ResolvedDeviceCommand& command, std::string* error) const;
};

class MemoryItemValidator {
public:
    bool validate(const MemoryItem& item, std::string* error) const;
};

class DeviceEventValidator {
public:
    bool validate(const DeviceEvent& event, std::string* error) const;
};

}  // namespace assistant

#endif  // ASSISTANT_VALIDATORS_H
