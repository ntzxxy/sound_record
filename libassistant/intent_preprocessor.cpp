#include "intent_preprocessor.h"

#include "llm.h"

#include <iostream>
#include <string>

namespace assistant {
namespace {

// 粗粒度路由已经在 RequestRouter 中完成。这里的模型只负责校验候选并抽取
// 对应字段，不再重复八分类。GENERAL_CHAT 是防止粗路由误命中业务的安全拒绝。
const char kSafeRejectPrompt[] =
    "你是JSON抽取器。没有提供可靠的业务候选，不要分类或猜测。"
    "只输出{\"intent\":\"GENERAL_CHAT\",\"reply\":\"\"}，不要解释或markdown。";

const char kDeviceControlPrompt[] =
    "前级候选为设备控制。你只校验并抽取字段，不要在业务类型之间分类。只输出JSON。\n"
    "明确要求执行设备操作时输出{\"intent\":\"DEVICE_CONTROL\",\"device_command\":{\"room\":\"\",\"device\":\"\",\"action\":\"TURN_ON|TURN_OFF|SET_TEMPERATURE|SET_MODE\",\"value\":null,\"mode\":\"\"}}。"
    "空调制冷/制热/除湿/送风用SET_MODE，mode分别填COOL/HEAT/DRY/FAN；同句温度填数字value。只填原文信息，不能输出device_id。"
    "灯光颜色、亮度等未支持动作不得改写成温度设置，应输出GENERAL_CHAT并说明暂不支持。\n"
    "控制意图明确但缺必需字段时输出{\"intent\":\"CLARIFY\",\"device_command\":{\"room\":\"\",\"device\":\"\",\"action\":\"\",\"value\":null,\"mode\":\"\"},\"missing_slots\":[\"字段名\"],\"clarification_question\":\"一个简短问题\"}。\n"
    "若只是描述、询问方法、否定执行或并非控制请求，输出{\"intent\":\"GENERAL_CHAT\",\"reply\":\"简短安全回答\"}。不得声称设备已经执行。";

const char kDeviceFaultPrompt[] =
    "前级候选为设备故障。你只校验并抽取字段，不要在业务类型之间分类。只输出JSON。\n"
    "用户在反馈真实设备异常时输出{\"intent\":\"DEVICE_FAULT\",\"device_event\":{\"room\":\"\",\"device\":\"\",\"event_type\":\"FAULT\",\"description\":\"\"}}，字段只能来自原文，room可空。\n"
    "缺少设备时输出{\"intent\":\"CLARIFY\",\"clarification_question\":\"请说明哪个设备出现了问题。\"}；知识问答或非故障反馈输出{\"intent\":\"GENERAL_CHAT\",\"reply\":\"\"}。";

// 明确的记忆写入使用独立提示词，减少无关业务说明带来的预填充和生成开销。
const char kExplicitMemoryWritePrompt[] =
    "前级候选为记忆写入。你只抽取字段，不要在业务类型之间分类。只输出JSON。\n"
    "有完整事实时只能输出：{\"intent\":\"MEMORY_WRITE\",\"memory\":{\"category\":\"USER_PREFERENCE|DEVICE_PREFERENCE|HABIT|ROUTINE|OBJECT_LOCATION\",\"subject\":\"\",\"attribute\":\"\",\"value\":\"\"}}。\n"
    "只记录用户原话中明确给出的事实，不能猜测或补充。睡眠、作息等稳定习惯用HABIT；物品所在位置用OBJECT_LOCATION；一般喜好用USER_PREFERENCE。\n"
    "缺少可保存的具体内容时输出：{\"intent\":\"CLARIFY\",\"clarification_question\":\"请说明要记住的具体内容。\"}。";

const char kMemoryQueryPrompt[] =
    "前级候选为记忆查询。你只抽取检索字段，不要在业务类型之间分类。只输出JSON。\n"
    "输出{\"intent\":\"MEMORY_QUERY\",\"memory_query\":{\"subject\":\"\",\"attribute\":\"\",\"condition\":\"\",\"scope\":\"\"}}，只填原文信息。"
    "无法确定要查询什么时输出{\"intent\":\"CLARIFY\",\"clarification_question\":\"请说明你想查询哪条记忆。\"}。";

const char kRecordQueryPrompt[] =
    "前级候选为历史记录查询。你只抽取记录类型，不要在业务类型之间分类。只输出JSON。\n"
    "输出{\"intent\":\"RECORD_QUERY\",\"record_query\":{\"type\":\"ALL|DEVICE_FAULT|USER_PREFERENCE|OBJECT_LOCATION\"}}。"
    "无法确定查询范围时type填ALL。";

const char kWeatherQueryPrompt[] =
    "前级候选为天气查询。你只抽取字段，不要在业务类型之间分类。只输出JSON。\n"
    "输出{\"intent\":\"WEATHER_QUERY\",\"weather_query\":{\"city\":\"\",\"start_date\":\"\",\"end_date\":\"\",\"days\":0}}。"
    "只填原文明确给出的地点和日期；日期格式YYYY-MM-DD，未给出日期则留空，不要自行计算今天。"
    "缺少地点时输出{\"intent\":\"CLARIFY\",\"clarification_question\":\"请告诉我需要查询天气的地点。\"}。";

std::string makeSystemPrompt(const std::string& semantic_hint) {
    if (semantic_hint == "device_control" || semantic_hint == "complex_device_control") {
        return kDeviceControlPrompt;
    }
    if (semantic_hint == "device_fault_report") return kDeviceFaultPrompt;
    if (semantic_hint == "explicit_memory_write") return kExplicitMemoryWritePrompt;
    if (semantic_hint == "memory_query") return kMemoryQueryPrompt;
    if (semantic_hint == "record_query") return kRecordQueryPrompt;
    if (semantic_hint == "weather_query") return kWeatherQueryPrompt;
    return kSafeRejectPrompt;
}

int maxTokensForHint(const std::string& semantic_hint) {
    if (semantic_hint == "record_query") return 48;
    if (semantic_hint == "memory_query") return 80;
    if (semantic_hint == "weather_query") return 96;
    if (semantic_hint == "explicit_memory_write") return 96;
    if (semantic_hint == "device_control" || semantic_hint == "complex_device_control" ||
        semantic_hint == "device_fault_report") {
        return 112;
    }
    return 48;
}

}  // namespace

bool IntentPreprocessor::initialize() {
    return true;
}

IntentResult IntentPreprocessor::analyze(const std::string& user_input,
                                         const std::string& semantic_hint) {
    IntentResult result;
    if (user_input.empty()) {
        result.intent = IntentType::GeneralChat;
        result.json_valid = true;
        return result;
    }

    char output[4096];
    llm_once_params_t params;
    params.max_tokens = maxTokensForHint(semantic_hint);
    params.temperature = 0.0f;
    int latency_ms = 0;
    const std::string system_prompt = makeSystemPrompt(semantic_hint);
    std::cout << "[IntentPrompt] hint=" << semantic_hint
              << " bytes=" << system_prompt.size()
              << " max_tokens=" << params.max_tokens << std::endl;
    const int ret = llm_generate_once(system_prompt.c_str(), user_input.c_str(),
                                      &params, output, sizeof(output), &latency_ms);
    result.intent_latency_ms = latency_ms;
    result.raw_json = output;
    std::cout << "[IntentRawJSON] " << output << std::endl;
    std::cout << "[METRIC] intent_latency_ms=" << latency_ms << std::endl;

    if (ret < 0) {
        result.intent = IntentType::Clarify;
        result.clarification_question = "我刚才没有理解清楚，请再说一遍。";
        return result;
    }

    std::string error;
    if (!parser_.parse(output, &result, &error)) {
        result.intent = IntentType::Clarify;
        result.raw_json = output;
        result.intent_latency_ms = latency_ms;
        result.clarification_question = "我刚才没有理解清楚，请再说一遍。";
        std::cerr << "[IntentParser] error=" << error << std::endl;
        return result;
    }
    result.intent_latency_ms = latency_ms;
    return result;
}

}  // namespace assistant
