#include "request_router.h"

#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace assistant {
namespace {

bool isBlank(const std::string& input) {
    for (unsigned char c : input) {
        if (!std::isspace(c)) return false;
    }
    return true;
}

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

void eraseAll(std::string* text, const std::string& needle) {
    if (!text || needle.empty()) return;
    std::size_t pos = 0;
    while ((pos = text->find(needle, pos)) != std::string::npos) {
        text->erase(pos, needle.size());
    }
}

std::string trimPunctuation(std::string text) {
    const std::vector<std::string> punctuation = {
        " ", "\t", "，", "。", "！", "？", "、", "：", ":", "；", ";",
    };
    bool changed = true;
    while (changed && !text.empty()) {
        changed = false;
        for (const auto& token : punctuation) {
            if (text.rfind(token, 0) == 0) {
                text.erase(0, token.size());
                changed = true;
            }
            if (text.size() >= token.size() &&
                text.compare(text.size() - token.size(), token.size(), token) == 0) {
                text.erase(text.size() - token.size());
                changed = true;
            }
        }
    }
    return text;
}

std::optional<int> chineseDigit(const std::string& text, std::size_t* used) {
    struct Digit { const char* word; int value; };
    static const Digit kDigits[] = {
        {"零", 0}, {"一", 1}, {"二", 2}, {"两", 2}, {"三", 3}, {"四", 4},
        {"五", 5}, {"六", 6}, {"七", 7}, {"八", 8}, {"九", 9},
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

std::optional<double> extractNumber(const std::string& text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) continue;
        char* end = nullptr;
        const double value = std::strtod(text.c_str() + i, &end);
        if (end != text.c_str() + i) return value;
    }

    const std::string ten = "十";
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text.compare(i, ten.size(), ten) == 0) {
            std::size_t used = 0;
            const auto ones = chineseDigit(text.substr(i + ten.size()), &used);
            return 10 + (ones ? *ones : 0);
        }
        std::size_t used = 0;
        const auto tens = chineseDigit(text.substr(i), &used);
        if (!tens) continue;
        const std::size_t next = i + used;
        if (text.compare(next, ten.size(), ten) == 0) {
            std::size_t ones_used = 0;
            const auto ones = chineseDigit(text.substr(next + ten.size()), &ones_used);
            return *tens * 10 + (ones ? *ones : 0);
        }
    }
    return std::nullopt;
}

// 这里只维护语言别名；设备是否真实可执行仍以 DeviceRegistry 为准。
struct EntityAlias {
    const char* text;
    const char* canonical;
};

bool extractEntity(const std::string& text,
                   const EntityAlias* aliases,
                   std::size_t alias_count,
                   std::string* out) {
    if (!out) return false;
    for (std::size_t i = 0; i < alias_count; ++i) {
        if (contains(text, aliases[i].text)) {
            *out = aliases[i].canonical;
            return true;
        }
    }
    return false;
}

std::optional<DeviceCommand> parseDeviceCommand(const std::string& text) {
    DeviceCommand command;
    static constexpr EntityAlias kRooms[] = {
        {"卧室", "卧室"}, {"客厅", "客厅"}, {"厨房", "厨房"},
        {"卫生间", "卫生间"}, {"厕所", "卫生间"}, {"阳台", "阳台"},
        {"书房", "书房"}, {"餐厅", "餐厅"}, {"儿童房", "儿童房"},
        {"玄关", "玄关"},
    };
    static constexpr EntityAlias kDevices[] = {
        {"空气净化器", "空气净化器"}, {"扫地机器人", "扫地机器人"},
        {"加湿器", "加湿器"}, {"热水器", "热水器"}, {"洗衣机", "洗衣机"},
        {"空调", "空调"}, {"风扇", "风扇"}, {"窗帘", "窗帘"},
        {"电视", "电视"}, {"冰箱", "冰箱"}, {"插座", "插座"},
        {"门锁", "门锁"}, {"音箱", "音箱"}, {"灯", "灯"},
    };
    extractEntity(text, kRooms, sizeof(kRooms) / sizeof(kRooms[0]), &command.room);
    extractEntity(text, kDevices, sizeof(kDevices) / sizeof(kDevices[0]), &command.device);

    // “模式”仍按设备控制处理，再由校验层明确告知当前不支持。
    const bool asks_power_on = contains(text, "打开") || contains(text, "开启") ||
                               contains(text, "启动") || contains(text, "开机");
    const bool asks_power_off = contains(text, "关闭") || contains(text, "关掉") ||
                                contains(text, "关机");
    if (contains(text, "制热") || contains(text, "制冷") ||
        contains(text, "除湿") || contains(text, "送风")) {
        command.action = "SET_MODE";
    } else if (asks_power_on && !asks_power_off) {
        command.action = "TURN_ON";
    } else if (asks_power_off && !asks_power_on) {
        command.action = "TURN_OFF";
    }
    else if (contains(text, "设置") || contains(text, "设为") || contains(text, "设成") ||
             contains(text, "调到") || contains(text, "调成") || contains(text, "切换")) {
        command.action = "SET_TEMPERATURE";
    }

    command.value = extractNumber(text);
    if (command.value && command.action.empty()) command.action = "SET_TEMPERATURE";
    if (command.room.empty() && command.device.empty() && command.action.empty() && !command.value) {
        return std::nullopt;
    }
    return command;
}

// 小型声明式语法同时检查动作、房间和设备，用于区分完整命令、缺参命令和普通对话。
enum class DeviceCommandMatch {
    NoMatch,
    PartialMatch,
    FullMatch,
    ComplexCandidate,
};

struct LocalDeviceControlMatch {
    DeviceCommandMatch status{DeviceCommandMatch::NoMatch};
    DeviceCommand command;
};

bool hasControlVerb(const std::string& text) {
    return contains(text, "打开") || contains(text, "开启") || contains(text, "开机") ||
           contains(text, "关闭") || contains(text, "关掉") || contains(text, "关机") ||
           contains(text, "设置") || contains(text, "设为") ||
           contains(text, "设成") || contains(text, "调到") || contains(text, "调成") || contains(text, "切换") ||
           contains(text, "启动");
}

bool isInformationalControlQuestion(const std::string& text) {
    return contains(text, "怎么") || contains(text, "如何") ||
           contains(text, "为什么") || contains(text, "是什么");
}

LocalDeviceControlMatch matchLocalDeviceControl(const std::string& text) {
    LocalDeviceControlMatch match;
    // 单独的数字不是操作，至少需要动作词以及房间或设备中的一个锚点。
    if (!hasControlVerb(text) || isInformationalControlQuestion(text)) return match;

    const auto parsed = parseDeviceCommand(text);
    if (!parsed || parsed->action.empty()) return match;
    match.command = *parsed;
    const bool has_anchor = !match.command.room.empty() || !match.command.device.empty();
    if (!has_anchor) return match;

    if (match.command.action == "SET_TEMPERATURE") {
        // “调成暖光”等非数值设置交给结构化提取，不能误走温度缺参追问。
        if (match.command.device == "灯" && !match.command.value) {
            match.status = DeviceCommandMatch::ComplexCandidate;
            return match;
        }
        // 已识别出操作类型但槽位不足时，才允许进入后续追问状态。
        if (!match.command.value || match.command.room.empty() || match.command.device.empty()) {
            match.status = DeviceCommandMatch::PartialMatch;
            return match;
        }
        match.status = DeviceCommandMatch::FullMatch;
        return match;
    }

    if (match.command.room.empty() || match.command.device.empty()) {
        match.status = DeviceCommandMatch::PartialMatch;
        return match;
    }
    match.status = DeviceCommandMatch::FullMatch;
    return match;
}

bool hasExplicitMemoryWrite(const std::string& text) {
    // 长期记忆必须有明确保存意图，普通的第一人称陈述仍按对话处理。
    return contains(text, "记住") || contains(text, "保存到记忆") ||
           contains(text, "加入记忆") || contains(text, "保存为偏好");
}

std::optional<MemoryItem> parseObjectLocationWrite(const std::string& text) {
    if (!hasExplicitMemoryWrite(text)) return std::nullopt;
    const std::size_t marker = text.find("放在");
    if (marker == std::string::npos) return std::nullopt;

    std::string subject = text.substr(0, marker);
    for (const char* noise : {"请记住", "记住", "我的", "我", "，", "。"}) eraseAll(&subject, noise);
    subject = trimPunctuation(subject);
    std::string value = trimPunctuation(text.substr(marker + std::string("放在").size()));
    if (subject.empty() || value.empty()) return std::nullopt;
    return MemoryItem{"OBJECT_LOCATION", subject, "位置", value, 0, "", "", "", "", 100};
}

std::optional<MemoryQuery> parseExactLocationQuery(const std::string& text) {
    const std::size_t marker = text.find("在哪里");
    if (marker == std::string::npos) return std::nullopt;
    std::string subject = text.substr(0, marker);
    for (const char* noise : {"我的", "我", "请问", "请", "放"}) eraseAll(&subject, noise);
    subject = trimPunctuation(subject);
    if (subject.empty()) return std::nullopt;
    return MemoryQuery{subject, "位置", "", "", ""};
}

// 偏好查询句式较稳定，优先本地处理，避免不必要的结构化模型调用。
std::optional<MemoryQuery> parsePreferenceQuery(const std::string& text) {
    // 仅有主题词和疑问词不足以读取记忆，还需要“记得”等明确回忆语义。
    const bool recalls_saved_memory =
        contains(text, "还记得") || contains(text, "之前记录") ||
        contains(text, "已保存") || contains(text, "保存过") ||
        contains(text, "记忆里") || contains(text, "我之前说过");
    const bool asks_preference = contains(text, "喜欢") || contains(text, "不喜欢") ||
                                 contains(text, "偏好");
    const bool lighting_topic = contains(text, "灯光") || contains(text, "照明") ||
                                contains(text, "光");
    if (!recalls_saved_memory || !asks_preference || !lighting_topic) {
        return std::nullopt;
    }

    MemoryQuery query;
    query.attribute = "偏好";
    if (contains(text, "阅读")) query.subject = "阅读";
    else if (contains(text, "灯") || contains(text, "光")) query.subject = "灯光";
    return query;
}

std::optional<MemoryQuery> parseMemoryQueryCandidate(const std::string& text) {
    const bool asks_for_fact = contains(text, "什么") || contains(text, "哪个") ||
                               contains(text, "叫什么") || contains(text, "哪种") ||
                               contains(text, "哪里") || contains(text, "怎么做") ||
                               contains(text, "写在前面") || contains(text, "先拼");
    const bool refers_to_user_history = contains(text, "我的") || contains(text, "我家") ||
                                        contains(text, "我给") || contains(text, "我通常") ||
                                        contains(text, "我习惯") || contains(text, "按我的") ||
                                        contains(text, "之前记录") || contains(text, "已保存") ||
                                        contains(text, "想不起来");
    if (!asks_for_fact || !refers_to_user_history) return std::nullopt;

    MemoryQuery query;
    query.query_text = text;
    if (contains(text, "叫") || contains(text, "名字") || contains(text, "昵称")) {
        query.attribute = "名称";
    } else if (contains(text, "前面") || contains(text, "顺序") ||
               contains(text, "习惯") || contains(text, "先拼")) {
        query.attribute = "习惯";
    } else if (contains(text, "位置") || contains(text, "哪里")) {
        query.attribute = "位置";
    }
    return query;
}

bool isQueryVerb(const std::string& text) {
    return contains(text, "查看") || contains(text, "查询") || contains(text, "列出") ||
           contains(text, "有哪些") || contains(text, "有哪") || contains(text, "找出");
}

std::optional<RecordQuery> parseRecordQuery(const std::string& text) {
    if (!isQueryVerb(text)) return std::nullopt;
    if (contains(text, "故障") || contains(text, "异常")) return RecordQuery{RecordType::DeviceFault};
    if (contains(text, "偏好") || contains(text, "习惯")) return RecordQuery{RecordType::UserPreference};
    if (contains(text, "位置") || contains(text, "物品")) return RecordQuery{RecordType::ObjectLocation};
    if (contains(text, "记忆")) return RecordQuery{RecordType::All};
    return std::nullopt;
}

std::optional<DeviceEvent> parseFaultRecord(const std::string& text) {
    if (!(contains(text, "记录") || contains(text, "报告"))) return std::nullopt;
    const bool has_fault = contains(text, "不制冷") || contains(text, "异响") ||
                           contains(text, "闪烁") || contains(text, "漏水") ||
                           contains(text, "故障") || contains(text, "异常") ||
                           contains(text, "不亮") || contains(text, "没反应") ||
                           contains(text, "失灵") || contains(text, "卡住") ||
                           contains(text, "不摆动") || contains(text, "嗡嗡") ||
                           contains(text, "不工作") || contains(text, "坏了");
    if (!has_fault) return std::nullopt;
    const auto command = parseDeviceCommand(text);
    if (!command || command->device.empty()) return std::nullopt;
    DeviceEvent event;
    event.room = command->room;
    event.device = command->device;
    event.event_type = "FAULT";
    event.description = text;
    for (const char* noise : {"请记录", "记录", "报告", "，", "。"}) eraseAll(&event.description, noise);
    event.description = trimPunctuation(event.description);
    return event.description.empty() ? std::nullopt : std::optional<DeviceEvent>(event);
}

MemoryDeleteRequest parseDelete(const std::string& text) {
    MemoryDeleteRequest request;
    request.delete_all = (contains(text, "全部") || contains(text, "所有") || contains(text, "清空")) &&
                         contains(text, "记忆");
    if (request.delete_all) return request;
    request.category = contains(text, "位置") || contains(text, "在哪") || contains(text, "放在")
        ? "OBJECT_LOCATION" : "";
    request.subject = text;
    for (const char* noise : {"请", "帮我", "把", "删除", "清除", "清空", "忘掉", "不要记住", "记忆", "位置", "的", "。", "，"}) {
        eraseAll(&request.subject, noise);
    }
    request.subject = trimPunctuation(request.subject);
    return request;
}

bool isAmbiguousBulkControl(const std::string& text) {
    return contains(text, "全部") || contains(text, "所有") || contains(text, "除了") ||
           contains(text, "以及") || contains(text, "和");
}

std::optional<MemoryQuery> chatMemoryContextQuery(const std::string& text) {
    if (contains(text, "阅读") || contains(text, "看书")) {
        return MemoryQuery{"阅读", "", "阅读", "", ""};
    }
    if (contains(text, "灯光") || contains(text, "照明") || contains(text, "刺眼")) {
        return MemoryQuery{"", "偏好", "", "", ""};
    }
    if (contains(text, "偏好") || contains(text, "习惯")) {
        return MemoryQuery{"", "偏好", "", "", ""};
    }
    return std::nullopt;
}

bool refersToEarlierConversation(const std::string& text) {
    return contains(text, "刚才") || contains(text, "前面") || contains(text, "之前") ||
           contains(text, "上面");
}

RequestAnalysis fastResult(IntentResult intent, const char* rule) {
    intent.local_route = true;
    intent.json_valid = true;
    RequestAnalysis analysis;
    analysis.status = LocalRouteStatus::FastPath;
    analysis.intent = std::move(intent);
    analysis.matched_rule = rule;
    return analysis;
}

RequestAnalysis semanticFallback(const SemanticRouteResult& route,
                                 const char* rule,
                                 const char* hint) {
    RequestAnalysis analysis;
    analysis.status = LocalRouteStatus::SemanticFallback;
    analysis.semantic_route = route;
    analysis.matched_rule = rule;
    analysis.semantic_hint = hint;
    return analysis;
}

bool isExecutionSuppressed(const std::string& text) {
    return (contains(text, "不要") && hasControlVerb(text)) ||
           contains(text, "不要执行") || contains(text, "不执行") ||
           contains(text, "先别") || contains(text, "别执行") ||
           contains(text, "保持当前状态") || contains(text, "保持原样") ||
           contains(text, "只是台词") || contains(text, "原样复述");
}

}  // namespace

RequestRouter::RequestRouter(std::shared_ptr<const SemanticIntentRouter> semantic_router)
    : semantic_router_(std::move(semantic_router)) {}

RequestAnalysis RequestRouter::analyze(const std::string& input) const {
    RequestAnalysis analysis;
    if (input.empty() || isBlank(input)) {
        IntentResult intent;
        intent.intent = IntentType::Clarify;
        intent.clarification_question = "请再说一遍你的需求。";
        return fastResult(std::move(intent), "empty_input");
    }

    // Hard guards are intentionally small and deterministic.  In particular,
    // a semantic DEVICE_CONTROL score can never override execution suppression.
    if (isExecutionSuppressed(input)) {
        IntentResult intent;
        intent.intent = IntentType::GeneralChat;
        return fastResult(std::move(intent), "execution_suppressed");
    }

    // A complete or safely partial command in the supported local grammar is
    // already stronger evidence than an embedding score.  Resolve it before
    // semantic routing so a conservative/OOD threshold cannot hide commands
    // that the deterministic parser and validator know how to handle.  Bulk
    // and unsupported forms still continue to the semantic fallback path.
    if (!hasExplicitMemoryWrite(input) && !isAmbiguousBulkControl(input)) {
        const LocalDeviceControlMatch match = matchLocalDeviceControl(input);
        if (match.status == DeviceCommandMatch::FullMatch ||
            match.status == DeviceCommandMatch::PartialMatch) {
            IntentResult intent;
            intent.intent = IntentType::DeviceControl;
            intent.device_command = match.command;
            return fastResult(std::move(intent),
                              match.status == DeviceCommandMatch::FullMatch
                                  ? "device_control_full_match"
                                  : "device_control_partial_match");
        }
    }

    const SemanticRouteResult route = semantic_router_
                                          ? semantic_router_->classify(input)
                                          : SemanticRouteResult{};
    analysis.semantic_route = route;

    switch (route.route) {
        case SemanticRoute::DeviceControl: {
            // The semantic result selects this parser; it does not create an
            // executable command.  Missing/complex slots go to structured extraction.
            if ((contains(input, "操作") || contains(input, "控制"))) {
                const auto command = parseDeviceCommand(input);
                if (command && !command->room.empty() && !command->device.empty() &&
                    command->action.empty()) {
                    IntentResult intent;
                    intent.intent = IntentType::DeviceControl;
                    intent.device_command = *command;
                    RequestAnalysis result = fastResult(std::move(intent), "device_control_missing_action");
                    result.semantic_route = route;
                    return result;
                }
            }
            if (isAmbiguousBulkControl(input)) {
                return semanticFallback(route, "device_control_complex", "device_control");
            }
            return semanticFallback(route, "device_control_parser_fallback", "device_control");
        }
        case SemanticRoute::MemoryWrite: {
            if (const auto location = parseObjectLocationWrite(input)) {
                IntentResult intent;
                intent.intent = IntentType::MemoryWrite;
                intent.memory = *location;
                RequestAnalysis result = fastResult(std::move(intent), "object_location_write");
                result.semantic_route = route;
                return result;
            }
            return semanticFallback(route, "memory_write_parser_fallback", "explicit_memory_write");
        }
        case SemanticRoute::MemoryQuery: {
            std::optional<MemoryQuery> query = parseExactLocationQuery(input);
            if (!query) query = parsePreferenceQuery(input);
            if (!query) query = parseMemoryQueryCandidate(input);
            if (query) {
                IntentResult intent;
                intent.intent = IntentType::MemoryQuery;
                intent.memory_query = *query;
                RequestAnalysis result = fastResult(std::move(intent), "memory_query_parser");
                result.semantic_route = route;
                return result;
            }
            return semanticFallback(route, "memory_query_parser_fallback", "memory_query");
        }
        case SemanticRoute::MemoryDelete: {
            IntentResult intent;
            intent.intent = IntentType::MemoryDelete;
            intent.memory_delete = parseDelete(input);
            RequestAnalysis result = fastResult(std::move(intent), "memory_delete_parser");
            result.semantic_route = route;
            return result;
        }
        case SemanticRoute::DeviceFault: {
            if (const auto fault = parseFaultRecord(input)) {
                IntentResult intent;
                intent.intent = IntentType::DeviceFault;
                intent.device_event = *fault;
                RequestAnalysis result = fastResult(std::move(intent), "fault_record_parser");
                result.semantic_route = route;
                return result;
            }
            return semanticFallback(route, "device_fault_parser_fallback", "device_fault_report");
        }
        case SemanticRoute::RecordQuery: {
            if (const auto query = parseRecordQuery(input)) {
                IntentResult intent;
                intent.intent = IntentType::RecordQuery;
                intent.record_query = *query;
                RequestAnalysis result = fastResult(std::move(intent), "record_query_parser");
                result.semantic_route = route;
                return result;
            }
            return semanticFallback(route, "record_query_parser_fallback", "record_query");
        }
        case SemanticRoute::WeatherQuery:
            return semanticFallback(route, "weather_query_parser_fallback", "weather_query");
        case SemanticRoute::None:
            break;
    }

    // NONE is the rejection/general-chat route.  Context lookup is ancillary;
    // it cannot turn the request into a business action.
    if (const auto query = chatMemoryContextQuery(input)) {
        analysis.intent.memory_context_query = *query;
    }
    analysis.intent.include_recent_memory_context = refersToEarlierConversation(input);
    analysis.matched_rule = "semantic_none";
    return analysis;
}

}  // namespace assistant
