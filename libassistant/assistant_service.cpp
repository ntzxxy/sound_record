#include "assistant_service.h"

#include <ctime>
#include <filesystem>
#include <iostream>

namespace assistant {
namespace {

constexpr const char* kLogPrefix = "[AssistantContext]";

void printSnapshot(const std::vector<MemoryItem>& items) {
    std::cout << "[MemorySnapshot]" << std::endl;
    for (std::size_t i = 0; i < items.size(); ++i) {
        std::cout << (i + 1) << ". " << items[i].category
                  << " | " << items[i].subject
                  << " | " << items[i].attribute
                  << " | " << items[i].value << std::endl;
    }
}

std::string makeDeviceReply(const ResolvedDeviceCommand& command) {
    std::string action_text = "控制";
    if (command.action == "TURN_ON") {
        action_text = "打开";
    } else if (command.action == "TURN_OFF") {
        action_text = "关闭";
    } else if (command.action == "SET_TEMPERATURE") {
        action_text = "设置";
    }

    if (command.action == "SET_TEMPERATURE" && command.value) {
        return "已识别到把" + command.room + command.device + "调到" +
               std::to_string(static_cast<int>(*command.value)) +
               "度的指令，当前为模拟控制模式。";
    }
    return "已识别到" + action_text + command.room + command.device +
           "的指令，当前为模拟控制模式。";
}

std::string defaultClarification() {
    return "这个指令还不够明确，请说明房间、设备和动作。";
}

std::string noMemoryContext() {
    return "【相关系统记忆】\n"
           "- 系统中没有找到与该问题相关的已存储信息。不得猜测或虚构。\n";
}

std::string defaultEventLogPath(const std::string& memory_path) {
    const std::filesystem::path path(memory_path);
    const std::filesystem::path dir = path.parent_path();
    if (dir.empty()) return "./runtime/device_fault_events.tsv";
    return (dir / "device_fault_events.tsv").string();
}

std::string makeDeviceFaultContext(const DeviceEvent& event) {
    return "【设备异常记录】\n"
           "- 已记录用户反馈的设备异常："
           + (event.room.empty() ? "" : event.room)
           + event.device + "，" + event.description + "。\n"
           "- 这只是异常日志记录，不代表设备已经检查或修复。请自然回复用户，并避免声称故障已解决。\n";
}

}  // namespace

AssistantService::AssistantService(const std::string& memory_path,
                                   const std::string& event_log_path)
    : memory_store_(memory_path),
      event_log_(event_log_path.empty() ? defaultEventLogPath(memory_path) : event_log_path) {}

bool AssistantService::initialize() {
    const bool memory_ok = memory_store_.load();
    const bool event_ok = event_log_.load();
    const bool intent_ok = intent_preprocessor_.initialize();
    std::cout << kLogPrefix << " initialize memory_load="
              << (memory_ok ? "OK" : "FAIL")
              << " event_log_load=" << (event_ok ? "OK" : "FAIL")
              << " intent_preprocessor=" << (intent_ok ? "OK" : "FAIL")
              << std::endl;
    return memory_ok && event_ok && intent_ok;
}

ServiceResult AssistantService::process(const std::string& user_input) {
    IntentResult intent = intent_preprocessor_.analyze(user_input);
    ServiceResult result;
    result.task_type = intent.intent;

    std::cout << "[TaskClass] " << toString(result.task_type) << std::endl;

    switch (intent.intent) {
        case IntentType::GeneralChat: {
            result.call_llm = true;
            break;
        }

        case IntentType::DeviceControl: {
            if (!intent.device_command) {
                result.task_type = IntentType::Clarify;
                result.call_llm = false;
                result.fixed_reply = defaultClarification();
                std::cout << "[TaskClass] " << toString(result.task_type) << std::endl;
                break;
            }

            auto resolved = device_registry_.resolve(*intent.device_command);
            std::string error;
            if (!resolved || !device_validator_.validate(*resolved, &error)) {
                result.task_type = IntentType::Clarify;
                result.call_llm = false;
                result.fixed_reply = defaultClarification();
                std::cout << "[DeviceCommandInvalid] error="
                          << (resolved ? error : "device_not_found") << std::endl;
                break;
            }

            result.call_llm = false;
            result.device_command = resolved;
            result.fixed_reply = makeDeviceReply(*resolved);
            std::cout << "[DeviceCommand]" << std::endl
                      << formatDeviceCommand(*resolved) << std::endl;
            break;
        }

        case IntentType::DeviceFault: {
            if (!intent.device_event) {
                result.task_type = IntentType::GeneralChat;
                result.call_llm = true;
                break;
            }

            DeviceEvent event = *intent.device_event;
            event.timestamp = static_cast<int64_t>(std::time(nullptr));
            std::string error;
            if (!event_validator_.validate(event, &error)) {
                result.task_type = IntentType::GeneralChat;
                result.call_llm = true;
                std::cout << "[DeviceFaultInvalid] error=" << error << std::endl;
                break;
            }

            if (!event_log_.append(event)) {
                std::cerr << kLogPrefix << " device_event_save=FAIL" << std::endl;
            }
            result.call_llm = true;
            result.device_event = event;
            result.runtime_context = makeDeviceFaultContext(event);
            std::cout << "[DeviceFault]" << std::endl
                      << formatDeviceEvent(event) << std::endl;
            std::cout << "[InjectedContext]" << std::endl
                      << result.runtime_context;
            break;
        }

        case IntentType::MemoryWrite: {
            if (!intent.memory) {
                result.task_type = IntentType::Clarify;
                result.call_llm = false;
                result.fixed_reply = "这条信息还不够明确，请说明要记住的内容。";
                break;
            }

            MemoryItem item = *intent.memory;
            item.updated_at = static_cast<int64_t>(std::time(nullptr));
            std::string error;
            if (!memory_validator_.validate(item, &error)) {
                result.task_type = IntentType::Clarify;
                result.call_llm = false;
                result.fixed_reply = "这条信息还不够明确，请说明要记住的内容。";
                std::cout << "[MemoryWriteInvalid] error=" << error << std::endl;
                break;
            }

            memory_store_.upsert(item);
            if (!memory_store_.save()) {
                std::cerr << kLogPrefix << " memory_save=FAIL" << std::endl;
            }
            result.call_llm = false;
            result.fixed_reply = "好的，我记住了。";
            result.stored_memory = item;
            std::cout << "[MemoryWrite]" << std::endl
                      << formatMemoryItem(item) << std::endl;
            printSnapshot(memory_store_.snapshot());
            break;
        }

        case IntentType::MemoryQuery: {
            if (!intent.memory_query) {
                result.task_type = IntentType::Clarify;
                result.call_llm = false;
                result.fixed_reply = "请说明你想查询哪条记忆。";
                break;
            }

            result.call_llm = true;
            result.runtime_context =
                context_builder_.buildMemoryContext(*intent.memory_query, memory_store_);
            if (result.runtime_context.empty()) {
                result.runtime_context = noMemoryContext();
            }
            std::cout << "[InjectedContext]" << std::endl
                      << result.runtime_context;
            break;
        }

        case IntentType::Clarify: {
            result.call_llm = false;
            result.fixed_reply = intent.clarification_question.empty()
                                     ? defaultClarification()
                                     : intent.clarification_question;
            break;
        }
    }

    return result;
}

std::vector<MemoryItem> AssistantService::memorySnapshot() const {
    return memory_store_.snapshot();
}

std::vector<DeviceEvent> AssistantService::eventSnapshot() const {
    return event_log_.snapshot();
}

}  // namespace assistant
