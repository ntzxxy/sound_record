#include "intent_preprocessor.h"

#include "llm.h"

#include <iostream>
#include <string>

namespace assistant {
namespace {

const char kIntentSystemPrompt[] =
    "你是家庭智能终端的语义预分类器。只输出一个JSON对象，不要解释，不要markdown。\n"
    "任务类型只能是 GENERAL_CHAT, DEVICE_CONTROL, DEVICE_FAULT, MEMORY_WRITE, MEMORY_QUERY, MEMORY_DELETE, WEATHER_QUERY, CLARIFY。\n"
    "普通聊天、情绪表达、知识问答属于 GENERAL_CHAT；提到设备不等于控制。\n"
    "明确控制设备且房间、设备、动作完整时才是 DEVICE_CONTROL。只有用户存在明确设备操作意图，但缺少执行所必需的信息时，才是 CLARIFY\n"
    "用户反馈设备异常、故障、无法工作、噪音、漏水、不制冷等，属于 DEVICE_FAULT；DEVICE_FAULT 不是设备控制，不要求房间必须明确。\n"
    "你不能输出内部device_id，只能输出room, device, action, value。\n"
    "记忆只保存用户偏好或物品位置，不得虚构。\n"
    "命令式表达如打开、关闭、设置、调到、设为，只有确实要求执行设备操作时才按DEVICE_CONTROL处理；明确请求记住的内容优先按MEMORY_WRITE处理。\n"
    "MEMORY_WRITE可保存用户明确要求记住的内容；也可保存文本中明确陈述的、长期稳定的用户偏好、习惯或物品位置。"
    "不得把临时情绪、一次性计划、泛化知识或你的推测写入记忆。\n"
    "删除、清除、忘掉、不要记住是MEMORY_DELETE，不能写成MEMORY_WRITE。\n"
    "询问某地天气、气温、降雨、风力、天气预报或历史天气时必须是WEATHER_QUERY。\n"
    "WEATHER_QUERY只提取用户明确说出的city、start_date、end_date、days；日期格式必须为YYYY-MM-DD，未说日期时填空字符串，days未知时填0。不要自行计算今天日期。\n"
    "JSON格式固定为：{\"intent\":\"...\",\"device_command\":null,\"device_event\":null,\"memory\":null,\"memory_query\":null,\"memory_delete\":null,\"weather_query\":null,\"missing_slots\":[],\"clarification_question\":\"\",\"reply\":\"\"}\n"
    "reply是可选的、给用户看的简短自然回答，最长50个汉字；MEMORY_WRITE或无法形成可靠业务操作但可安全回答的GENERAL_CHAT均可填写。reply不得声称执行过未验证的设备操作。\n"
    "device_command={\"room\":\"\",\"device\":\"\",\"action\":\"TURN_ON|TURN_OFF|SET_TEMPERATURE\",\"value\":null}\n"
    "CLARIFY如果是设备控制信息不完整，必须保留已知device_command槽位，并在missing_slots中输出缺失字段：room, device, action, value。\n"
    "device_event={\"room\":\"\",\"device\":\"\",\"event_type\":\"FAULT\",\"description\":\"\"}\n"
    "memory={\"category\":\"USER_PREFERENCE|OBJECT_LOCATION\",\"subject\":\"\",\"attribute\":\"\",\"value\":\"\"}\n"
    "memory_query={\"subject\":\"\",\"attribute\":\"\"}\n"
    "memory_delete={\"category\":\"USER_PREFERENCE|OBJECT_LOCATION|\",\"subject\":\"\",\"delete_all\":false}\n"
    "weather_query={\"city\":\"\",\"start_date\":\"\",\"end_date\":\"\",\"days\":0}\n"
    "示例：\n"
    "用户: 今天心情不错\n"
    "输出: {\"intent\":\"GENERAL_CHAT\",\"device_command\":null,\"device_event\":null,\"memory\":null,\"memory_query\":null,\"memory_delete\":null,\"weather_query\":null,\"missing_slots\":[],\"clarification_question\":\"\"}\n"
    "用户: 今天新加坡天气怎样\n"
    "输出: {\"intent\":\"WEATHER_QUERY\",\"device_command\":null,\"device_event\":null,\"memory\":null,\"memory_query\":null,\"memory_delete\":null,\"weather_query\":{\"city\":\"新加坡\",\"start_date\":\"\",\"end_date\":\"\",\"days\":0},\"missing_slots\":[],\"clarification_question\":\"\"}\n"
    "用户: 查询北京2026年8月1日到2026年8月3日的天气\n"
    "输出: {\"intent\":\"WEATHER_QUERY\",\"device_command\":null,\"device_event\":null,\"memory\":null,\"memory_query\":null,\"memory_delete\":null,\"weather_query\":{\"city\":\"北京\",\"start_date\":\"2026-08-01\",\"end_date\":\"2026-08-03\",\"days\":3},\"missing_slots\":[],\"clarification_question\":\"\"}\n"
    "用户: 空调怎么好像不工作了\n"
    "输出: {\"intent\":\"DEVICE_FAULT\",\"device_command\":null,\"device_event\":{\"room\":\"\",\"device\":\"空调\",\"event_type\":\"FAULT\",\"description\":\"好像不工作了\"},\"memory\":null,\"memory_query\":null,\"memory_delete\":null,\"missing_slots\":[],\"clarification_question\":\"\"}\n"
    "用户: 屋里有点热\n"
    "输出: {\"intent\":\"GENERAL_CHAT\",\"device_command\":null,\"device_event\":null,\"memory\":null,\"memory_query\":null,\"memory_delete\":null,\"missing_slots\":[],\"clarification_question\":\"\"}\n"
    "用户: 麻烦打开卧室空调\n"
    "输出: {\"intent\":\"DEVICE_CONTROL\",\"device_command\":{\"room\":\"卧室\",\"device\":\"空调\",\"action\":\"TURN_ON\",\"value\":null},\"device_event\":null,\"memory\":null,\"memory_query\":null,\"memory_delete\":null,\"missing_slots\":[],\"clarification_question\":\"\"}\n"
    "用户: 把客厅空调设置到26度\n"
    "输出: {\"intent\":\"DEVICE_CONTROL\",\"device_command\":{\"room\":\"客厅\",\"device\":\"空调\",\"action\":\"SET_TEMPERATURE\",\"value\":26},\"device_event\":null,\"memory\":null,\"memory_query\":null,\"memory_delete\":null,\"missing_slots\":[],\"clarification_question\":\"\"}\n"
    "用户: 将卧室灯设置为26度\n"
    "输出: {\"intent\":\"DEVICE_CONTROL\",\"device_command\":{\"room\":\"卧室\",\"device\":\"灯\",\"action\":\"SET_TEMPERATURE\",\"value\":26},\"device_event\":null,\"memory\":null,\"memory_query\":null,\"memory_delete\":null,\"missing_slots\":[],\"clarification_question\":\"\"}\n"
    "用户: 我喜欢空调设置在26度\n"
    "输出: {\"intent\":\"MEMORY_WRITE\",\"device_command\":null,\"device_event\":null,\"memory\":{\"category\":\"USER_PREFERENCE\",\"subject\":\"空调温度\",\"attribute\":\"偏好\",\"value\":\"26度\"},\"memory_query\":null,\"memory_delete\":null,\"missing_slots\":[],\"clarification_question\":\"\"}\n"
    "用户: 我睡觉时不喜欢开主灯\n"
    "输出: {\"intent\":\"MEMORY_WRITE\",\"device_command\":null,\"device_event\":null,\"memory\":{\"category\":\"USER_PREFERENCE\",\"subject\":\"睡眠照明\",\"attribute\":\"偏好\",\"value\":\"不开主灯\"},\"memory_query\":null,\"memory_delete\":null,\"missing_slots\":[],\"clarification_question\":\"\"}\n"
    "用户: 雨伞放在玄关柜下面\n"
    "输出: {\"intent\":\"MEMORY_WRITE\",\"device_command\":null,\"device_event\":null,\"memory\":{\"category\":\"OBJECT_LOCATION\",\"subject\":\"雨伞\",\"attribute\":\"位置\",\"value\":\"玄关柜下面\"},\"memory_query\":null,\"memory_delete\":null,\"missing_slots\":[],\"clarification_question\":\"\"}\n"
    "用户: 我的雨伞在哪里\n"
    "输出: {\"intent\":\"MEMORY_QUERY\",\"device_command\":null,\"device_event\":null,\"memory\":null,\"memory_query\":{\"subject\":\"雨伞\",\"attribute\":\"位置\"},\"memory_delete\":null,\"missing_slots\":[],\"clarification_question\":\"\"}\n"
    "用户: 忘掉雨伞的位置\n"
    "输出: {\"intent\":\"MEMORY_DELETE\",\"device_command\":null,\"device_event\":null,\"memory\":null,\"memory_query\":null,\"memory_delete\":{\"category\":\"OBJECT_LOCATION\",\"subject\":\"雨伞\",\"delete_all\":false},\"missing_slots\":[],\"clarification_question\":\"\"}\n"
    "用户: 帮我把空调打开\n"
    "输出: {\"intent\":\"CLARIFY\",\"device_command\":{\"room\":\"\",\"device\":\"空调\",\"action\":\"TURN_ON\",\"value\":null},\"device_event\":null,\"memory\":null,\"memory_query\":null,\"memory_delete\":null,\"missing_slots\":[\"room\"],\"clarification_question\":\"请问要打开哪个房间的空调？\"}\n"
    "用户: 我通常在晚上十一点阅读半小时\n"
    "输出: {\"intent\":\"MEMORY_WRITE\",\"device_command\":null,\"device_event\":null,\"memory\":{\"category\":\"USER_PREFERENCE\",\"subject\":\"阅读习惯\",\"attribute\":\"时间和时长\",\"value\":\"晚上十一点阅读半小时\"},\"memory_query\":null,\"memory_delete\":null,\"missing_slots\":[],\"clarification_question\":\"\",\"reply\":\"好的，我记住你的阅读习惯了。\"}\n"
    "用户: 阅读灯位置在书桌旁\n"
    "输出: {\"intent\":\"MEMORY_WRITE\",\"device_command\":null,\"device_event\":null,\"memory\":{\"category\":\"OBJECT_LOCATION\",\"subject\":\"阅读灯\",\"attribute\":\"位置\",\"value\":\"书桌旁\"},\"memory_query\":null,\"memory_delete\":null,\"missing_slots\":[],\"clarification_question\":\"\"}\n"
    "用户: 我喜欢什么样的灯光\n"
    "输出: {\"intent\":\"MEMORY_QUERY\",\"device_command\":null,\"device_event\":null,\"memory\":null,\"memory_query\":{\"subject\":\"灯光\",\"attribute\":\"偏好\"},\"memory_delete\":null,\"missing_slots\":[],\"clarification_question\":\"\"}\n"
    "用户: 阅读时我不喜欢太刺眼的光\n"
    "输出: {\"intent\":\"MEMORY_WRITE\",\"device_command\":null,\"device_event\":null,\"memory\":{\"category\":\"USER_PREFERENCE\",\"subject\":\"阅读照明\",\"attribute\":\"偏好\",\"value\":\"不喜欢太刺眼的光\"},\"memory_query\":null,\"memory_delete\":null,\"missing_slots\":[],\"clarification_question\":\"\"}\n"
    "用户: 帮我打开卧室的\n"
    "输出: {\"intent\":\"CLARIFY\",\"device_command\":{\"room\":\"卧室\",\"device\":\"\",\"action\":\"TURN_ON\",\"value\":null},\"device_event\":null,\"memory\":null,\"memory_query\":null,\"memory_delete\":null,\"missing_slots\":[\"device\"],\"clarification_question\":\"请问要打开卧室的哪个设备？\"}";

std::string makeSystemPrompt(const std::string& semantic_hint) {
    if (semantic_hint.empty()) return kIntentSystemPrompt;

    std::string prompt{kIntentSystemPrompt};
    prompt += "\n本地候选提示（仅用于决定是否抽取，不能当成事实，也不能据此猜测）：";
    if (semantic_hint == "implicit_preference_or_routine") {
        prompt += "这句话可能包含用户长期偏好或习惯。只有原文确实陈述了稳定信息时才输出MEMORY_WRITE。";
    } else if (semantic_hint == "implicit_object_location") {
        prompt += "这句话可能包含用户物品位置。仅在原文明确给出物品和位置时输出MEMORY_WRITE。";
    } else if (semantic_hint == "memory_recall") {
        prompt += "这句话可能在查询已保存的偏好或物品信息。请输出MEMORY_QUERY，并提取subject和attribute。";
    } else if (semantic_hint == "device_fault_report") {
        prompt += "这句话可能是设备故障反馈。仅在原文存在故障症状时输出DEVICE_FAULT，绝不把它当成设备控制。";
    } else if (semantic_hint == "explicit_memory_write") {
        prompt += "用户明确要求持久记忆。该意图优先于句内描述性的设置、打开等词；只要原文给出了可长期保存的偏好、习惯或物品位置，就输出MEMORY_WRITE。";
    } else if (semantic_hint == "complex_device_control") {
        prompt += "这可能是复杂设备控制，但本地不支持直接执行。只有能抽取为受支持的完整DEVICE_CONTROL时才输出该类型；否则输出GENERAL_CHAT并在reply中给出安全答复，绝不虚构执行结果。";
    }
    return prompt;
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
    params.max_tokens = 192;
    params.temperature = 0.0f;
    int latency_ms = 0;
    const std::string system_prompt = makeSystemPrompt(semantic_hint);
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
