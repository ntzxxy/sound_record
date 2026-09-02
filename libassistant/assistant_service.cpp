#include "assistant_service.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <vector>

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

std::string makeUnsupportedDeviceReply(const DeviceCommand& command) {
    return "当前没有找到" + command.room + command.device + "，无法执行。";
}

std::string makeInvalidDeviceCommandReply(const ResolvedDeviceCommand& command,
                                          const std::string& error) {
    if (error == "light_temperature_unsupported") {
        return command.room + command.device + "不支持温度设置。";
    }
    if (error == "invalid_temperature") {
        return "空调温度只支持设置在16到30度之间。";
    }
    return "这个设备指令暂时不支持。";
}

bool containsText(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

bool isCancelText(const std::string& text) {
    return containsText(text, "取消") || containsText(text, "算了") ||
           containsText(text, "不用了") || containsText(text, "先不弄") ||
           containsText(text, "停止");
}

bool isPreferenceMemoryText(const std::string& text) {
    return containsText(text, "我喜欢") || containsText(text, "我习惯") ||
           containsText(text, "以后默认") || containsText(text, "记住我") ||
           containsText(text, "偏好");
}

bool hasDeviceWord(const std::string& text) {
    return containsText(text, "空调") || containsText(text, "灯");
}

bool hasControlVerb(const std::string& text) {
    return containsText(text, "打开") || containsText(text, "开启") ||
           containsText(text, "关闭") || containsText(text, "关掉") ||
           containsText(text, "设置") || containsText(text, "设为") ||
           containsText(text, "调到") || containsText(text, "调成");
}

bool looksLikeDeviceControlText(const std::string& text) {
    return hasDeviceWord(text) && hasControlVerb(text) && !isPreferenceMemoryText(text);
}

bool hasAnyDeviceSlot(const DeviceCommand& command) {
    return !command.room.empty() || !command.device.empty() ||
           !command.action.empty() || command.value.has_value();
}

bool hasSlot(const std::vector<std::string>& slots, const std::string& slot) {
    for (const auto& item : slots) {
        if (item == slot) return true;
    }
    return false;
}

std::vector<std::string> missingDeviceSlots(const DeviceCommand& command) {
    std::vector<std::string> missing;
    if (command.room.empty()) missing.push_back("room");
    if (command.device.empty()) missing.push_back("device");
    if (command.action.empty()) missing.push_back("action");
    if (command.action == "SET_TEMPERATURE" && !command.value) {
        missing.push_back("value");
    }
    return missing;
}

DeviceCommand mergeDeviceCommand(const DeviceCommand& base,
                                 const DeviceCommand& patch) {
    DeviceCommand merged = base;
    if (!patch.room.empty()) merged.room = patch.room;
    if (!patch.device.empty()) merged.device = patch.device;
    if (!patch.action.empty()) merged.action = patch.action;
    if (patch.value) merged.value = patch.value;
    return merged;
}

std::optional<double> extractFirstNumber(const std::string& text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) continue;
        char* end = nullptr;
        const double value = std::strtod(text.c_str() + i, &end);
        if (end != text.c_str() + i) return value;
    }
    return std::nullopt;
}

std::optional<int> chineseDigit(const std::string& text, std::size_t* used) {
    struct Digit {
        const char* word;
        int value;
    };
    static const Digit kDigits[] = {
        {"零", 0}, {"一", 1}, {"二", 2}, {"两", 2}, {"三", 3}, {"四", 4},
        {"五", 5}, {"六", 6}, {"七", 7}, {"八", 8}, {"九", 9}
    };
    for (const auto& digit : kDigits) {
        const std::string word = digit.word;
        if (text.compare(0, word.size(), word) == 0) {
            if (used) *used = word.size();
            return digit.value;
        }
    }
    return std::nullopt;
}

std::optional<double> extractChineseNumber(const std::string& text) {
    const std::string ten = "十";
    for (std::size_t i = 0; i < text.size(); ++i) {
        std::size_t used = 0;
        if (text.compare(i, ten.size(), ten) == 0) {
            std::size_t ones_used = 0;
            int value = 10;
            if (auto ones = chineseDigit(text.substr(i + ten.size()), &ones_used)) {
                value += *ones;
            }
            return value;
        }

        auto tens = chineseDigit(text.substr(i), &used);
        if (!tens) continue;
        std::size_t pos = i + used;
        if (text.compare(pos, ten.size(), ten) == 0) {
            int value = *tens * 10;
            pos += ten.size();
            std::size_t ones_used = 0;
            if (auto ones = chineseDigit(text.substr(pos), &ones_used)) {
                value += *ones;
            }
            return value;
        }
    }
    return std::nullopt;
}

std::optional<double> extractNumber(const std::string& text) {
    if (auto value = extractFirstNumber(text)) return value;
    return extractChineseNumber(text);
}

std::optional<DeviceCommand> inferDeviceSlotsFromText(const std::string& text) {
    DeviceCommand command;
    if (containsText(text, "卧室")) {
        command.room = "卧室";
    } else if (containsText(text, "客厅")) {
        command.room = "客厅";
    } else if (containsText(text, "厨房")) {
        command.room = "厨房";
    } else if (containsText(text, "卫生间")) {
        command.room = "卫生间";
    } else if (containsText(text, "厕所")) {
        command.room = "卫生间";
    }

    if (containsText(text, "空调")) {
        command.device = "空调";
    } else if (containsText(text, "灯")) {
        command.device = "灯";
    }

    if (containsText(text, "打开") || containsText(text, "开启")) {
        command.action = "TURN_ON";
    } else if (containsText(text, "关闭") || containsText(text, "关掉")) {
        command.action = "TURN_OFF";
    } else if (containsText(text, "设置") || containsText(text, "设为") ||
               containsText(text, "调到") || containsText(text, "调成") ||
               containsText(text, "温度")) {
        command.action = "SET_TEMPERATURE";
    }

    command.value = extractNumber(text);
    if (command.value && command.action.empty()) {
        command.action = "SET_TEMPERATURE";
    }

    if (!hasAnyDeviceSlot(command)) return std::nullopt;
    return command;
}

DeviceCommand applyDeterministicDeviceSlots(const DeviceCommand& command,
                                            const std::optional<DeviceCommand>& inferred) {
    if (!inferred) return command;
    return mergeDeviceCommand(command, *inferred);
}

std::string makeSlotClarification(const DeviceCommand& command,
                                  const std::vector<std::string>& missing) {
    if (hasSlot(missing, "room") && !command.device.empty()) {
        return "请问要控制哪个房间的" + command.device + "？";
    }
    if (hasSlot(missing, "device") && !command.room.empty()) {
        return "请问要控制" + command.room + "的哪个设备？";
    }
    if (hasSlot(missing, "action") && !command.room.empty() && !command.device.empty()) {
        return "请问要对" + command.room + command.device + "执行什么操作？";
    }
    if (hasSlot(missing, "value") && command.action == "SET_TEMPERATURE") {
        return "请问要设置到多少度？";
    }
    return defaultClarification();
}

std::string noMemoryContext() {
    return "【相关系统记忆】\n"
           "- 系统中没有找到与该问题相关的已存储信息。不得猜测或虚构。\n";
}

bool looksLikeListQuestion(const std::string& text) {
    static const std::vector<std::string> query_words = {
        "查询", "查看", "显示", "列出", "列表", "记录", "有哪些", "有哪", "多少", "什么"
    };
    for (const auto& word : query_words) {
        if (containsText(text, word)) return true;
    }
    return false;
}

std::optional<RecordQuery> inferRecordQuery(const std::string& text) {
    const bool is_list_question = looksLikeListQuestion(text);
    const bool mentions_fault = containsText(text, "故障") || containsText(text, "异常") ||
                                containsText(text, "坏了") || containsText(text, "不制冷");
    if (mentions_fault && is_list_question) {
        return RecordQuery{RecordType::DeviceFault};
    }

    const bool mentions_preference = containsText(text, "偏好") || containsText(text, "喜欢什么") ||
                                     containsText(text, "习惯什么");
    if (mentions_preference && is_list_question) {
        return RecordQuery{RecordType::UserPreference};
    }

    const bool mentions_location = containsText(text, "物品位置") || containsText(text, "位置记录") ||
                                   containsText(text, "哪些物品") || containsText(text, "物品有哪些");
    if (mentions_location && is_list_question) {
        return RecordQuery{RecordType::ObjectLocation};
    }

    const bool mentions_all_records = containsText(text, "记忆列表") || containsText(text, "所有记忆") ||
                                      containsText(text, "全部记忆") || containsText(text, "记住了什么");
    if (mentions_all_records) {
        return RecordQuery{RecordType::All};
    }
    return std::nullopt;
}

std::string memoryCategoryName(RecordType type) {
    switch (type) {
        case RecordType::UserPreference: return "用户偏好";
        case RecordType::ObjectLocation: return "物品位置";
        default: return "系统记忆";
    }
}

std::string makeRecordListReply(RecordType type,
                                std::vector<MemoryItem> memories,
                                std::vector<DeviceEvent> events) {
    constexpr std::size_t kMaxShownItems = 5;
    std::sort(memories.begin(), memories.end(), [](const MemoryItem& a, const MemoryItem& b) {
        return a.updated_at > b.updated_at;
    });
    std::sort(events.begin(), events.end(), [](const DeviceEvent& a, const DeviceEvent& b) {
        return a.timestamp > b.timestamp;
    });

    auto make_memory_reply = [&](RecordType category) {
        std::vector<MemoryItem> selected;
        for (const auto& item : memories) {
            if ((category == RecordType::UserPreference && item.category == "USER_PREFERENCE") ||
                (category == RecordType::ObjectLocation && item.category == "OBJECT_LOCATION")) {
                selected.push_back(item);
            }
        }
        const std::string name = memoryCategoryName(category);
        if (selected.empty()) return "目前没有已保存的" + name + "。";

        std::string reply = "目前有" + std::to_string(selected.size()) + "条" + name + "：";
        for (std::size_t i = 0; i < selected.size() && i < kMaxShownItems; ++i) {
            const auto& item = selected[i];
            reply += std::to_string(i + 1) + "." + item.subject + "：" + item.value + "；";
        }
        if (selected.size() > kMaxShownItems) reply += "其余请在记录中心查看。";
        return reply;
    };

    if (type == RecordType::DeviceFault) {
        if (events.empty()) return "目前没有已记录的设备故障。";
        std::string reply = "目前有" + std::to_string(events.size()) + "条设备故障记录：";
        for (std::size_t i = 0; i < events.size() && i < kMaxShownItems; ++i) {
            const auto& event = events[i];
            reply += std::to_string(i + 1) + "." + event.room + event.device + "：" +
                     event.description + "；";
        }
        if (events.size() > kMaxShownItems) reply += "其余请在记录中心查看。";
        return reply;
    }
    if (type == RecordType::UserPreference || type == RecordType::ObjectLocation) {
        return make_memory_reply(type);
    }

    std::size_t preference_count = 0;
    std::size_t location_count = 0;
    for (const auto& item : memories) {
        if (item.category == "USER_PREFERENCE") ++preference_count;
        if (item.category == "OBJECT_LOCATION") ++location_count;
    }
    return "当前共有" + std::to_string(events.size()) + "条设备故障、" +
           std::to_string(preference_count) + "条用户偏好和" +
           std::to_string(location_count) + "条物品位置记录，可在记录中心查看详情。";
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

bool looksLikeMemoryDeleteText(const std::string& text) {
    return containsText(text, "删除") || containsText(text, "清除") ||
           containsText(text, "清空") || containsText(text, "忘掉") ||
           containsText(text, "不要记住");
}

void eraseAll(std::string* text, const std::string& needle) {
    if (!text || needle.empty()) return;
    std::size_t pos = 0;
    while ((pos = text->find(needle, pos)) != std::string::npos) {
        text->erase(pos, needle.size());
    }
}

std::string trimAsciiSpaces(const std::string& text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }

    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string inferMemoryDeleteSubject(const std::string& text) {
    std::string subject = text;
    const std::vector<std::string> noise_words = {
        "请", "帮我", "把", "给我", "一下", "掉", "相关", "这条", "这个",
        "删除", "清除", "清空", "忘掉", "不要记住", "不用记住", "别记住",
        "我", "的", "关于", "记忆", "记录", "信息"
    };
    for (const auto& word : noise_words) {
        eraseAll(&subject, word);
    }
    return trimAsciiSpaces(subject);
}

std::string inferMemoryDeleteCategory(const std::string& text,
                                      const std::string& subject) {
    if (containsText(text, "位置") || containsText(text, "在哪") ||
        containsText(text, "放在")) {
        return "OBJECT_LOCATION";
    }
    if (containsText(text, "偏好") || containsText(text, "习惯") ||
        containsText(text, "喜欢") || containsText(text, "温度") ||
        containsText(subject, "温度")) {
        return "USER_PREFERENCE";
    }
    return "";
}

MemoryDeleteRequest inferMemoryDeleteRequest(const std::string& text) {
    MemoryDeleteRequest request;
    request.delete_all = (containsText(text, "全部") || containsText(text, "所有") ||
                          containsText(text, "清空")) &&
                         containsText(text, "记忆");
    if (!request.delete_all) {
        request.subject = inferMemoryDeleteSubject(text);
        request.category = inferMemoryDeleteCategory(text, request.subject);
    }
    return request;
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
    if (const auto record_query = inferRecordQuery(user_input)) {
        IntentResult intent;
        intent.intent = IntentType::RecordQuery;
        intent.record_query = *record_query;
        intent.json_valid = true;
        return processAnalyzed(user_input, intent);
    }
    IntentResult intent = intent_preprocessor_.analyze(user_input);
    return processAnalyzed(user_input, intent);
}

ServiceResult AssistantService::processAnalyzed(const std::string& user_input,
                                                const IntentResult& analyzed_intent) {
    IntentResult intent = analyzed_intent;
    ServiceResult result;
    result.task_type = intent.intent;
    result.intent_latency_ms = intent.intent_latency_ms;
    const std::optional<DeviceCommand> inferred_command = inferDeviceSlotsFromText(user_input);

    if (looksLikeDeviceControlText(user_input) && inferred_command) {
        intent.intent = IntentType::DeviceControl;
        intent.device_command = intent.device_command
                                    ? applyDeterministicDeviceSlots(*intent.device_command, inferred_command)
                                    : *inferred_command;
        result.task_type = IntentType::DeviceControl;
        std::cout << "[IntentCorrection] forced_device_control" << std::endl;
    } else if (looksLikeMemoryDeleteText(user_input)) {
        intent.intent = IntentType::MemoryDelete;
        if (!intent.memory_delete) {
            intent.memory_delete = inferMemoryDeleteRequest(user_input);
        }
        intent.memory.reset();
        result.task_type = IntentType::MemoryDelete;
        std::cout << "[IntentCorrection] forced_memory_delete" << std::endl;
    } else if (intent.intent == IntentType::DeviceControl && intent.device_command) {
        intent.device_command = applyDeterministicDeviceSlots(*intent.device_command, inferred_command);
    }

    std::cout << "[TaskClass] " << toString(result.task_type) << std::endl;

    if (pending_device_command_) {
        if (isCancelText(user_input)) {
            pending_device_command_.reset();
            pending_device_turns_remaining_ = 0;
            result.task_type = IntentType::Clarify;
            result.call_llm = false;
            result.fixed_reply = "好的，已取消。";
            std::cout << "[PendingDeviceCommand] canceled" << std::endl;
            return result;
        }

        std::optional<DeviceCommand> supplement = intent.device_command;
        if (!supplement || !hasAnyDeviceSlot(*supplement)) {
            supplement = inferred_command;
        }

        if (supplement && hasAnyDeviceSlot(*supplement)) {
            DeviceCommand merged = mergeDeviceCommand(*pending_device_command_, *supplement);
            std::vector<std::string> missing = missingDeviceSlots(merged);
            if (missing.empty()) {
                intent.intent = IntentType::DeviceControl;
                intent.device_command = merged;
                result.task_type = IntentType::DeviceControl;
                pending_device_command_.reset();
                pending_device_turns_remaining_ = 0;
                std::cout << "[PendingDeviceCommand] merged=complete" << std::endl;
            } else {
                pending_device_command_ = merged;
                pending_device_turns_remaining_ = 1;
                result.task_type = IntentType::Clarify;
                result.call_llm = false;
                result.fixed_reply = makeSlotClarification(merged, missing);
                std::cout << "[PendingDeviceCommand] merged=incomplete missing=";
                for (const auto& slot : missing) std::cout << slot << ' ';
                std::cout << std::endl;
                return result;
            }
        } else if (--pending_device_turns_remaining_ <= 0) {
            pending_device_command_.reset();
            pending_device_turns_remaining_ = 0;
            std::cout << "[PendingDeviceCommand] expired" << std::endl;
        }
    }

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

            std::vector<std::string> missing = missingDeviceSlots(*intent.device_command);
            if (!missing.empty()) {
                pending_device_command_ = *intent.device_command;
                pending_device_turns_remaining_ = 1;
                result.task_type = IntentType::Clarify;
                result.call_llm = false;
                result.fixed_reply = makeSlotClarification(*intent.device_command, missing);
                std::cout << "[PendingDeviceCommand] saved from incomplete control missing=";
                for (const auto& slot : missing) std::cout << slot << ' ';
                std::cout << std::endl;
                break;
            }

            auto resolved = device_registry_.resolve(*intent.device_command);
            std::string error;
            if (!resolved || !device_validator_.validate(*resolved, &error)) {
                result.task_type = IntentType::DeviceControl;
                result.call_llm = false;
                result.fixed_reply = resolved
                                         ? makeInvalidDeviceCommandReply(*resolved, error)
                                         : makeUnsupportedDeviceReply(*intent.device_command);
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

        case IntentType::MemoryDelete: {
            if (!intent.memory_delete) {
                result.call_llm = false;
                result.fixed_reply = "请说明要删除哪条记忆。";
                break;
            }

            const MemoryDeleteRequest request = *intent.memory_delete;
            if (!request.delete_all && request.subject.empty() && request.category.empty()) {
                result.call_llm = false;
                result.fixed_reply = "请说明要删除哪条记忆。";
                break;
            }

            const std::size_t removed = memory_store_.removeMatching(request);
            if (!memory_store_.save()) {
                std::cerr << kLogPrefix << " memory_save=FAIL" << std::endl;
            }
            result.call_llm = false;
            result.deleted_memory = request;
            result.fixed_reply = removed > 0 ? "好的，已删除相关记忆。" : "没有找到需要删除的记忆。";
            std::cout << "[MemoryDelete] removed=" << removed << std::endl;
            printSnapshot(memory_store_.snapshot());
            break;
        }

        case IntentType::WeatherQuery: {
            if (!intent.weather_query) {
                result.call_llm = false;
                result.fixed_reply = "请告诉我需要查询天气的地点。";
                break;
            }

            NormalizedWeatherQuery normalized;
            std::string error;
            if (!WeatherQueryValidator::normalize(*intent.weather_query, &normalized, &error)) {
                result.call_llm = false;
                if (error == "missing_city") {
                    result.fixed_reply = "请告诉我需要查询天气的地点。";
                } else if (error == "future_date_unsupported") {
                    result.fixed_reply = "当前历史天气查询不支持未来日期。";
                } else if (error == "date_range_too_long") {
                    result.fixed_reply = "单次天气查询最长支持30天。";
                } else {
                    result.fixed_reply = "日期或查询时长不合法，请按YYYY-MM-DD格式重新说明。";
                }
                std::cout << "[WeatherQuery] status=invalid error=" << error << std::endl;
                break;
            }

            result.tool_name = "weather_history";
            if (!weather_service_.queryHistory(normalized, &result.tool_result_json,
                                               &result.runtime_context, &error)) {
                result.call_llm = false;
                if (error == "location_not_found") {
                    result.fixed_reply = "没有找到这个地点，请换一个城市名称再试。";
                } else if (error == "unauthorized") {
                    result.fixed_reply = "天气服务认证失败，请检查服务端API Key。";
                } else if (error == "service_busy") {
                    result.fixed_reply = "天气服务繁忙，请稍后再试。";
                } else {
                    result.fixed_reply = "暂时无法获取天气信息，请稍后再试。";
                }
                std::cout << "[WeatherQuery] status=failed error=" << error << std::endl;
                break;
            }

            result.call_llm = true;
            std::cout << "[WeatherQuery] status=success city=" << normalized.city
                      << " start=" << normalized.start_date
                      << " end=" << normalized.end_date
                      << " days=" << normalized.days << std::endl;
            break;
        }

        case IntentType::RecordQuery: {
            if (!intent.record_query) {
                result.call_llm = false;
                result.fixed_reply = "请说明要查询设备故障、用户偏好还是物品位置。";
                break;
            }
            result.call_llm = false;
            result.fixed_reply = makeRecordListReply(intent.record_query->type,
                                                     memory_store_.snapshot(),
                                                     event_log_.snapshot());
            break;
        }

        case IntentType::Clarify: {
            if (intent.device_command && hasAnyDeviceSlot(*intent.device_command)) {
                std::vector<std::string> missing = intent.missing_slots.empty()
                                                       ? missingDeviceSlots(*intent.device_command)
                                                       : intent.missing_slots;
                if (!missing.empty()) {
                    pending_device_command_ = *intent.device_command;
                    pending_device_turns_remaining_ = 1;
                    result.call_llm = false;
                    result.fixed_reply = intent.clarification_question.empty()
                                             ? makeSlotClarification(*intent.device_command, missing)
                                             : intent.clarification_question;
                    std::cout << "[PendingDeviceCommand] saved from clarify missing=";
                    for (const auto& slot : missing) std::cout << slot << ' ';
                    std::cout << std::endl;
                    break;
                }
            }
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

bool AssistantService::deleteMemoryRecord(const MemoryItem& item) {
    return memory_store_.removeExact(item) && memory_store_.save();
}

bool AssistantService::deleteDeviceFaultRecord(const DeviceEvent& event) {
    return event_log_.removeExact(event) && event_log_.save();
}

}  // namespace assistant
