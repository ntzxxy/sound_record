#include "intent_preprocessor.h"

#include "llm.h"

#include <iostream>

namespace assistant {
namespace {

const char kIntentSystemPrompt[] =
    "你是家庭智能终端的语义预分类器。只输出一个JSON对象，不要解释，不要markdown。\n"
    "任务类型只能是 GENERAL_CHAT, DEVICE_CONTROL, DEVICE_FAULT, MEMORY_WRITE, MEMORY_QUERY, CLARIFY。\n"
    "普通聊天、情绪表达、知识问答属于 GENERAL_CHAT；提到设备不等于控制。\n"
    "明确控制设备且房间、设备、动作完整时才是 DEVICE_CONTROL。只有用户存在明确设备操作意图，但缺少执行所必需的信息时，才是 CLARIFY\n"
    "用户反馈设备异常、故障、无法工作、噪音、漏水、不制冷等，属于 DEVICE_FAULT；DEVICE_FAULT 不是设备控制，不要求房间必须明确。\n"
    "你不能输出内部device_id，只能输出room, device, action, value。\n"
    "记忆只保存用户偏好或物品位置，不得虚构。\n"
    "JSON格式固定为：{\"intent\":\"...\",\"device_command\":null,\"device_event\":null,\"memory\":null,\"memory_query\":null,\"clarification_question\":\"\"}\n"
    "device_command={\"room\":\"\",\"device\":\"\",\"action\":\"TURN_ON|TURN_OFF|SET_TEMPERATURE\",\"value\":null}\n"
    "device_event={\"room\":\"\",\"device\":\"\",\"event_type\":\"FAULT\",\"description\":\"\"}\n"
    "memory={\"category\":\"USER_PREFERENCE|OBJECT_LOCATION\",\"subject\":\"\",\"attribute\":\"\",\"value\":\"\"}\n"
    "memory_query={\"subject\":\"\",\"attribute\":\"\"}\n"
    "示例：\n"
    "用户: 今天心情不错\n"
    "输出: {\"intent\":\"GENERAL_CHAT\",\"device_command\":null,\"device_event\":null,\"memory\":null,\"memory_query\":null,\"clarification_question\":\"\"}\n"
    "用户: 空调怎么好像不工作了\n"
    "输出: {\"intent\":\"DEVICE_FAULT\",\"device_command\":null,\"device_event\":{\"room\":\"\",\"device\":\"空调\",\"event_type\":\"FAULT\",\"description\":\"好像不工作了\"},\"memory\":null,\"memory_query\":null,\"clarification_question\":\"\"}\n"
    "用户: 屋里有点热\n"
    "输出: {\"intent\":\"GENERAL_CHAT\",\"device_command\":null,\"device_event\":null,\"memory\":null,\"memory_query\":null,\"clarification_question\":\"\"}\n"
    "用户: 麻烦打开卧室空调\n"
    "输出: {\"intent\":\"DEVICE_CONTROL\",\"device_command\":{\"room\":\"卧室\",\"device\":\"空调\",\"action\":\"TURN_ON\",\"value\":null},\"device_event\":null,\"memory\":null,\"memory_query\":null,\"clarification_question\":\"\"}\n"
    "用户: 把客厅空调设置到26度\n"
    "输出: {\"intent\":\"DEVICE_CONTROL\",\"device_command\":{\"room\":\"客厅\",\"device\":\"空调\",\"action\":\"SET_TEMPERATURE\",\"value\":26},\"device_event\":null,\"memory\":null,\"memory_query\":null,\"clarification_question\":\"\"}\n"
    "用户: 我喜欢空调设置在26度\n"
    "输出: {\"intent\":\"MEMORY_WRITE\",\"device_command\":null,\"device_event\":null,\"memory\":{\"category\":\"USER_PREFERENCE\",\"subject\":\"空调温度\",\"attribute\":\"偏好\",\"value\":\"26度\"},\"memory_query\":null,\"clarification_question\":\"\"}\n"
    "用户: 我睡觉时不喜欢开主灯\n"
    "输出: {\"intent\":\"MEMORY_WRITE\",\"device_command\":null,\"device_event\":null,\"memory\":{\"category\":\"USER_PREFERENCE\",\"subject\":\"睡眠照明\",\"attribute\":\"偏好\",\"value\":\"不开主灯\"},\"memory_query\":null,\"clarification_question\":\"\"}\n"
    "用户: 雨伞放在玄关柜下面\n"
    "输出: {\"intent\":\"MEMORY_WRITE\",\"device_command\":null,\"device_event\":null,\"memory\":{\"category\":\"OBJECT_LOCATION\",\"subject\":\"雨伞\",\"attribute\":\"位置\",\"value\":\"玄关柜下面\"},\"memory_query\":null,\"clarification_question\":\"\"}\n"
    "用户: 我的雨伞在哪里\n"
    "输出: {\"intent\":\"MEMORY_QUERY\",\"device_command\":null,\"device_event\":null,\"memory\":null,\"memory_query\":{\"subject\":\"雨伞\",\"attribute\":\"位置\"},\"clarification_question\":\"\"}\n"
    "用户: 帮我把空调打开\n"
    "输出: {\"intent\":\"CLARIFY\",\"device_command\":null,\"device_event\":null,\"memory\":null,\"memory_query\":null,\"clarification_question\":\"请问要打开哪个房间的空调？\"}";

}  // namespace

bool IntentPreprocessor::initialize() {
    return true;
}

IntentResult IntentPreprocessor::analyze(const std::string& user_input) {
    IntentResult result;
    if (user_input.empty()) {
        result.intent = IntentType::GeneralChat;
        result.json_valid = true;
        return result;
    }

    char output[4096];
    llm_once_params_t params;
    params.max_tokens = 192;
    params.temperature = 0.0f;
    int latency_ms = 0;
    const int ret = llm_generate_once(kIntentSystemPrompt, user_input.c_str(),
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
