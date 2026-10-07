#ifndef ASSISTANT_SERVICE_H
#define ASSISTANT_SERVICE_H

#include "context_builder.h"
#include "device_registry.h"
#include "event_log.h"
#include "intent_preprocessor.h"
#include "memory_store.h"
#include "request_router.h"
#include "validators.h"
#include "weather_service.h"

#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace assistant {

// 助手业务总入口：负责路由、校验、状态读写，并决定是否调用对话模型。
class AssistantService {
public:
    explicit AssistantService(const std::string& memory_path,
                              const std::string& event_log_path = "",
                              std::shared_ptr<const SemanticIntentRouter> semantic_router =
                                  createSemanticIntentRouterFromEnvironment());

    bool initialize();
    ServiceResult process(const std::string& user_input);
    ServiceResult processAnalyzed(const std::string& user_input,
                                  const IntentResult& analyzed_intent);
    std::vector<MemoryItem> memorySnapshot() const;
    std::vector<DeviceEvent> eventSnapshot() const;
    bool deleteMemoryRecord(const MemoryItem& item);
    bool deleteDeviceFaultRecord(const DeviceEvent& event);

private:
    IntentPreprocessor intent_preprocessor_;
    RequestRouter request_router_;
    DeviceRegistry device_registry_;
    DeviceCommandValidator device_validator_;
    MemoryItemValidator memory_validator_;
    DeviceEventValidator event_validator_;
    MemoryStore memory_store_;
    EventLog event_log_;
    ContextBuilder context_builder_;
    WeatherService weather_service_;
    // 参数不完整的设备命令只保留一轮，防止后续普通对话被误当成补充参数。
    std::optional<DeviceCommand> pending_device_command_;
    int pending_device_turns_remaining_{0};
};

}  // namespace assistant

#endif  // ASSISTANT_SERVICE_H
