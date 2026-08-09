#ifndef ASSISTANT_SERVICE_H
#define ASSISTANT_SERVICE_H

#include "context_builder.h"
#include "device_registry.h"
#include "event_log.h"
#include "intent_preprocessor.h"
#include "memory_store.h"
#include "validators.h"

#include <string>
#include <vector>

namespace assistant {

class AssistantService {
public:
    explicit AssistantService(const std::string& memory_path,
                              const std::string& event_log_path = "");

    bool initialize();
    ServiceResult process(const std::string& user_input);
    std::vector<MemoryItem> memorySnapshot() const;
    std::vector<DeviceEvent> eventSnapshot() const;

private:
    IntentPreprocessor intent_preprocessor_;
    DeviceRegistry device_registry_;
    DeviceCommandValidator device_validator_;
    MemoryItemValidator memory_validator_;
    DeviceEventValidator event_validator_;
    MemoryStore memory_store_;
    EventLog event_log_;
    ContextBuilder context_builder_;
};

}  // namespace assistant

#endif  // ASSISTANT_SERVICE_H
