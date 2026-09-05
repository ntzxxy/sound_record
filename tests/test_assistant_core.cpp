#include "context_builder.h"
#include "assistant_service.h"
#include "device_registry.h"
#include "event_log.h"
#include "intent_json_parser.h"
#include "memory_store.h"
#include "request_router.h"
#include "validators.h"
#include "weather_service.h"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>

namespace {

using assistant::ContextBuilder;
using assistant::AssistantService;
using assistant::DeviceCommandValidator;
using assistant::DeviceRegistry;
using assistant::DeviceEvent;
using assistant::DeviceEventValidator;
using assistant::EventLog;
using assistant::IntentJsonParser;
using assistant::IntentResult;
using assistant::IntentType;
using assistant::MemoryItem;
using assistant::MemoryItemValidator;
using assistant::MemoryDeleteRequest;
using assistant::MemoryQuery;
using assistant::MemoryStore;
using assistant::LocalRouteStatus;
using assistant::RequestRouter;
using assistant::ServiceResult;
using assistant::WeatherQuery;
using assistant::WeatherQueryValidator;
using assistant::NormalizedWeatherQuery;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "[TestAssistantCore] check failed: " #condition \
                      << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            std::abort(); \
        } \
    } while (0)

const char* kTestMemoryPath = "/tmp/assistant_core_test_memory_v2.tsv";
const char* kTestEventPath = "/tmp/assistant_core_test_device_events.tsv";

void resetTestFile() {
    std::remove(kTestMemoryPath);
    std::remove("/tmp/assistant_core_test_memory_v2.tsv.tmp");
    std::remove(kTestEventPath);
    std::remove("/tmp/assistant_core_test_device_events.tsv.tmp");
}

MemoryItem makeItem(const std::string& category,
                    const std::string& subject,
                    const std::string& attribute,
                    const std::string& value,
                    int64_t updated_at) {
    MemoryItem item;
    item.category = category;
    item.subject = subject;
    item.attribute = attribute;
    item.value = value;
    item.updated_at = updated_at;
    return item;
}

DeviceEvent makeEvent(const std::string& room,
                      const std::string& device,
                      const std::string& event_type,
                      const std::string& description,
                      int64_t timestamp) {
    DeviceEvent event;
    event.room = room;
    event.device = device;
    event.event_type = event_type;
    event.description = description;
    event.timestamp = timestamp;
    return event;
}

assistant::DeviceCommand makeCommand(const std::string& room,
                                     const std::string& device,
                                     const std::string& action,
                                     std::optional<double> value = std::nullopt) {
    assistant::DeviceCommand command;
    command.room = room;
    command.device = device;
    command.action = action;
    command.value = value;
    return command;
}

int countTriple(const std::vector<MemoryItem>& items,
                const std::string& category,
                const std::string& subject,
                const std::string& attribute) {
    int count = 0;
    for (const auto& item : items) {
        if (item.category == category &&
            item.subject == subject &&
            item.attribute == attribute) {
            ++count;
        }
    }
    return count;
}

}  // namespace

int main() {
    resetTestFile();

    {
        IntentJsonParser parser;
        IntentResult result;
        std::string error;
        const std::string json =
            "{\"intent\":\"DEVICE_CONTROL\",\"device_command\":{\"room\":\"卧室\","
            "\"device\":\"空调\",\"action\":\"TURN_ON\",\"value\":null},"
            "\"device_event\":null,\"memory\":null,\"memory_query\":null,"
            "\"clarification_question\":\"\"}";
        CHECK(parser.parse(json, &result, &error));
        CHECK(result.intent == IntentType::DeviceControl);
        CHECK(result.device_command);
        CHECK(result.device_command->room == "卧室");
        CHECK(result.device_command->device == "空调");
        CHECK(result.device_command->action == "TURN_ON");
        CHECK(!result.device_command->value);
    }

    {
        IntentJsonParser parser;
        IntentResult result;
        std::string error;
        const std::string json =
            "{\"intent\":\"WEATHER_QUERY\",\"device_command\":null,"
            "\"device_event\":null,\"memory\":null,\"memory_query\":null,"
            "\"memory_delete\":null,\"weather_query\":{\"city\":\"北京\","
            "\"start_date\":\"2026-08-01\",\"end_date\":\"2026-08-03\",\"days\":3},"
            "\"missing_slots\":[],\"clarification_question\":\"\"}";
        CHECK(parser.parse(json, &result, &error));
        CHECK(result.intent == IntentType::WeatherQuery);
        CHECK(result.weather_query);
        CHECK(result.weather_query->city == "北京");
        CHECK(result.weather_query->start_date == "2026-08-01");
        CHECK(result.weather_query->end_date == "2026-08-03");
        CHECK(result.weather_query->days == 3);
    }

    {
        WeatherQuery request;
        request.city = "北京";
        NormalizedWeatherQuery normalized;
        std::string error;
        CHECK(WeatherQueryValidator::normalize(request, &normalized, &error));
        CHECK(normalized.start_date == WeatherQueryValidator::localToday());
        CHECK(normalized.end_date == normalized.start_date);
        CHECK(normalized.days == 1);

        request.start_date = "2026-02-30";
        CHECK(!WeatherQueryValidator::normalize(request, &normalized, &error));
        CHECK(error == "invalid_date");
    }

    {
        IntentJsonParser parser;
        IntentResult result;
        std::string error;
        const std::string json =
            "{\"intent\":\"DEVICE_FAULT\",\"device_command\":null,"
            "\"device_event\":{\"room\":\"客厅\",\"device\":\"空调\","
            "\"event_type\":\"FAULT\",\"description\":\"不制冷并且有异响\"},"
            "\"memory\":null,\"memory_query\":null,\"clarification_question\":\"\"}";
        CHECK(parser.parse(json, &result, &error));
        CHECK(result.intent == IntentType::DeviceFault);
        CHECK(result.device_event);
        CHECK(result.device_event->room == "客厅");
        CHECK(result.device_event->device == "空调");
        CHECK(result.device_event->event_type == "FAULT");
        CHECK(result.device_event->description == "不制冷并且有异响");
    }

    {
        IntentJsonParser parser;
        IntentResult result;
        std::string error;
        const std::string wrapped =
            "说明文字\n{\"intent\":\"MEMORY_WRITE\",\"device_command\":null,"
            "\"device_event\":null,"
            "\"memory\":{\"category\":\"USER_PREFERENCE\",\"subject\":\"睡眠照明\","
            "\"attribute\":\"偏好\",\"value\":\"关闭主灯\",\"condition\":\"睡觉时\","
            "\"context\":\"睡眠环境\",\"time\":\"\",\"scope\":\"卧室\",\"confidence\":95},\"memory_query\":null,"
            "\"clarification_question\":\"\",\"reply\":\"好的，睡眠时会避免主灯。\"}\n尾部文字";
        CHECK(parser.parse(wrapped, &result, &error));
        CHECK(result.intent == IntentType::MemoryWrite);
        CHECK(result.memory);
        CHECK(result.memory->category == "USER_PREFERENCE");
        CHECK(result.memory->subject == "睡眠照明");
        CHECK(result.memory->attribute == "偏好");
        CHECK(result.memory->value == "关闭主灯");
        CHECK(result.memory->condition == "睡觉时");
        CHECK(result.memory->scope == "卧室");
        CHECK(result.memory->confidence == 95);
        CHECK(result.response_text == "好的，睡眠时会避免主灯。");
    }

    {
        IntentJsonParser parser;
        IntentResult result;
        std::string error;
        const std::string json =
            "{\"intent\":\"MEMORY_DELETE\",\"device_command\":null,"
            "\"device_event\":null,\"memory\":null,\"memory_query\":null,"
            "\"memory_delete\":{\"category\":\"OBJECT_LOCATION\","
            "\"subject\":\"护照\",\"delete_all\":false},"
            "\"missing_slots\":[],\"clarification_question\":\"\"}";
        CHECK(parser.parse(json, &result, &error));
        CHECK(result.intent == IntentType::MemoryDelete);
        CHECK(result.memory_delete);
        CHECK(result.memory_delete->category == "OBJECT_LOCATION");
        CHECK(result.memory_delete->subject == "护照");
        CHECK(!result.memory_delete->delete_all);
    }

    {
        IntentJsonParser parser;
        IntentResult result;
        std::string error;
        const std::string json =
            "{\"intent\":\"CLARIFY\",\"device_command\":{\"room\":\"\","
            "\"device\":\"空调\",\"action\":\"TURN_ON\",\"value\":null},"
            "\"device_event\":null,\"memory\":null,\"memory_query\":null,"
            "\"missing_slots\":[\"room\"],"
            "\"clarification_question\":\"请问要打开哪个房间的空调？\"}";
        CHECK(parser.parse(json, &result, &error));
        CHECK(result.intent == IntentType::Clarify);
        CHECK(result.device_command);
        CHECK(result.device_command->device == "空调");
        CHECK(result.device_command->action == "TURN_ON");
        CHECK(result.missing_slots.size() == 1);
        CHECK(result.missing_slots[0] == "room");
    }

    {
        IntentJsonParser parser;
        IntentResult result;
        std::string error;
        CHECK(!parser.parse("not json", &result, &error));
        CHECK(error == "no_json_object");
    }

    {
        DeviceRegistry registry;
        DeviceCommandValidator validator;
        assistant::DeviceCommand command;
        command.room = "卧室";
        command.device = "空调";
        command.action = "SET_TEMPERATURE";
        command.value = 26.0;
        auto resolved = registry.resolve(command);
        CHECK(resolved);
        CHECK(resolved->device_id == "bedroom_ac");
        std::string error;
        CHECK(validator.validate(*resolved, &error));
    }

    {
        DeviceRegistry registry;
        DeviceCommandValidator validator;
        assistant::DeviceCommand command;
        command.room = "客厅";
        command.device = "灯";
        command.action = "SET_TEMPERATURE";
        command.value = 26.0;
        auto resolved = registry.resolve(command);
        CHECK(resolved);
        std::string error;
        CHECK(!validator.validate(*resolved, &error));
        CHECK(error == "light_temperature_unsupported");
    }

    {
        AssistantService service(kTestMemoryPath, kTestEventPath);
        IntentResult first;
        first.intent = IntentType::Clarify;
        first.device_command = makeCommand("", "空调", "TURN_ON");
        first.missing_slots = {"room"};
        first.clarification_question = "请问要打开哪个房间的空调？";

        ServiceResult clarify = service.processAnalyzed("帮我把空调打开", first);
        CHECK(clarify.task_type == IntentType::Clarify);
        CHECK(!clarify.call_llm);
        CHECK(clarify.fixed_reply == "请问要控制哪个房间的空调？");

        IntentResult second;
        second.intent = IntentType::GeneralChat;
        ServiceResult completed = service.processAnalyzed("卧室", second);
        CHECK(completed.task_type == IntentType::DeviceControl);
        CHECK(!completed.call_llm);
        CHECK(completed.device_command);
        CHECK(completed.device_command->device_id == "bedroom_ac");
        CHECK(completed.fixed_reply.find("打开卧室空调") != std::string::npos);
    }

    {
        AssistantService service(kTestMemoryPath, kTestEventPath);
        IntentResult first;
        first.intent = IntentType::Clarify;
        first.device_command = makeCommand("卧室", "", "TURN_ON");
        first.missing_slots = {"device"};
        first.clarification_question = "请问要打开卧室的哪个设备？";

        ServiceResult clarify = service.processAnalyzed("帮我打开卧室的", first);
        CHECK(clarify.task_type == IntentType::Clarify);
        CHECK(!clarify.call_llm);

        IntentResult second;
        second.intent = IntentType::GeneralChat;
        ServiceResult completed = service.processAnalyzed("空调", second);
        CHECK(completed.task_type == IntentType::DeviceControl);
        CHECK(!completed.call_llm);
        CHECK(completed.device_command);
        CHECK(completed.device_command->device_id == "bedroom_ac");
    }

    {
        AssistantService service(kTestMemoryPath, kTestEventPath);
        IntentResult first;
        first.intent = IntentType::Clarify;
        first.device_command = makeCommand("卧室", "空调", "SET_TEMPERATURE");
        first.missing_slots = {"value"};
        first.clarification_question = "请问要设置到多少度？";

        ServiceResult clarify = service.processAnalyzed("把卧室空调调到", first);
        CHECK(clarify.task_type == IntentType::Clarify);
        CHECK(!clarify.call_llm);

        IntentResult second;
        second.intent = IntentType::GeneralChat;
        ServiceResult completed = service.processAnalyzed("26度", second);
        CHECK(completed.task_type == IntentType::DeviceControl);
        CHECK(!completed.call_llm);
        CHECK(completed.device_command);
        CHECK(completed.device_command->device_id == "bedroom_ac");
        CHECK(completed.device_command->value);
        CHECK(static_cast<int>(*completed.device_command->value) == 26);
    }

    resetTestFile();

    {
        AssistantService service(kTestMemoryPath, kTestEventPath);
        IntentResult wrong;
        wrong.intent = IntentType::MemoryWrite;
        wrong.memory = makeItem("USER_PREFERENCE", "卧室灯温度", "偏好", "26度", 0);

        ServiceResult rejected = service.processAnalyzed("将卧室灯设置为26度", wrong);
        CHECK(rejected.task_type == IntentType::DeviceControl);
        CHECK(!rejected.call_llm);
        CHECK(!rejected.stored_memory);
        CHECK(rejected.fixed_reply.find("不支持温度设置") != std::string::npos);
        CHECK(service.memorySnapshot().empty());

        ServiceResult rejected_again = service.processAnalyzed("将卧室灯设置为二十六度", wrong);
        CHECK(rejected_again.task_type == IntentType::DeviceControl);
        CHECK(!rejected_again.call_llm);
        CHECK(!rejected_again.stored_memory);
        CHECK(rejected_again.fixed_reply.find("不支持温度设置") != std::string::npos);
        CHECK(service.memorySnapshot().empty());
    }

    resetTestFile();

    {
        AssistantService service(kTestMemoryPath, kTestEventPath);
        IntentResult general;
        general.intent = IntentType::GeneralChat;

        ServiceResult kitchen = service.processAnalyzed("打开厨房空调", general);
        CHECK(kitchen.task_type == IntentType::DeviceControl);
        CHECK(!kitchen.call_llm);
        CHECK(!kitchen.device_command);
        CHECK(kitchen.fixed_reply.find("当前没有找到厨房空调") != std::string::npos);

        ServiceResult bathroom = service.processAnalyzed("打开卫生间灯", general);
        CHECK(bathroom.task_type == IntentType::DeviceControl);
        CHECK(!bathroom.call_llm);
        CHECK(!bathroom.device_command);
        CHECK(bathroom.fixed_reply.find("当前没有找到卫生间灯") != std::string::npos);
    }

    {
        AssistantService service(kTestMemoryPath, kTestEventPath);
        IntentResult first;
        first.intent = IntentType::Clarify;
        first.device_command = makeCommand("卧室", "", "TURN_ON");
        first.missing_slots = {"device"};
        first.clarification_question = "请问要打开卧室的哪个设备？";

        ServiceResult clarify = service.processAnalyzed("帮我打开卧室的", first);
        CHECK(clarify.task_type == IntentType::Clarify);
        CHECK(!clarify.call_llm);

        IntentResult cancel;
        cancel.intent = IntentType::GeneralChat;
        ServiceResult canceled = service.processAnalyzed("取消", cancel);
        CHECK(canceled.task_type == IntentType::Clarify);
        CHECK(!canceled.call_llm);
        CHECK(canceled.fixed_reply == "好的，已取消。");

        ServiceResult later = service.processAnalyzed("空调", cancel);
        CHECK(later.task_type == IntentType::GeneralChat);
        CHECK(later.call_llm);
        CHECK(!later.device_command);
    }

    resetTestFile();
    {
        RequestRouter router;
        CHECK(router.analyze("你好，今天心情怎么样？").status == LocalRouteStatus::Chat);
        CHECK(router.analyze("请记住我不喜欢太刺眼的光").status ==
              LocalRouteStatus::SemanticFallback);
        CHECK(router.analyze("晚上看书的时候，灯不要太亮。").semantic_hint ==
              "implicit_preference_or_routine");
        CHECK(router.analyze("我喜欢今天的天气。").status == LocalRouteStatus::Chat);
        CHECK(router.analyze("我正在准备睡前阅读，我有一盏阅读灯，位置在书桌旁。").semantic_hint ==
              "implicit_object_location");
        CHECK(router.analyze("我通常在晚上十一点阅读半小时。").semantic_hint ==
              "implicit_preference_or_routine");
        CHECK(router.analyze("阅读时我不喜欢太刺眼的光。").semantic_hint ==
              "implicit_preference_or_routine");
        const auto preference_query = router.analyze("我喜欢什么样的灯光？");
        CHECK(preference_query.status == LocalRouteStatus::FastPath);
        CHECK(preference_query.intent.intent == IntentType::MemoryQuery);
        CHECK(preference_query.intent.memory_query);
        CHECK(preference_query.intent.memory_query->attribute == "偏好");
        CHECK(router.analyze("你还记得我不喜欢哪种光吗？").status ==
              LocalRouteStatus::FastPath);
        CHECK(router.analyze("我之前一般什么时候阅读？").semantic_hint == "memory_recall");
        CHECK(router.analyze("室内温度一般设置多少比较舒服？").status ==
              LocalRouteStatus::Chat);
        CHECK(router.analyze("打开").status == LocalRouteStatus::Chat);
        const auto partial_control = router.analyze("打开空调");
        CHECK(partial_control.status == LocalRouteStatus::FastPath);
        CHECK(partial_control.matched_rule == "device_control_partial_match");
        const auto full_control = router.analyze("打开客厅的灯");
        CHECK(full_control.status == LocalRouteStatus::FastPath);
        CHECK(full_control.matched_rule == "device_control_full_match");
        const auto complex_control = router.analyze("把客厅的灯设置成暖光");
        CHECK(complex_control.status == LocalRouteStatus::SemanticFallback);
        CHECK(complex_control.semantic_hint == "complex_device_control");
        CHECK(router.analyze("嗯，帮我想一个睡前放松的办法。").status ==
              LocalRouteStatus::Chat);
        const auto explicit_memory = router.analyze("请记住，我睡觉时喜欢把空调设为二十五度。");
        CHECK(explicit_memory.status == LocalRouteStatus::SemanticFallback);
        CHECK(explicit_memory.semantic_hint == "explicit_memory_write");
        CHECK(router.analyze("空调怎么好像不工作了？").semantic_hint == "device_fault_report");
        CHECK(router.analyze("我准备阅读半小时，请给我一个简短建议。").status ==
              LocalRouteStatus::Chat);
        const auto reading_context = router.analyze("如果我说开始阅读，你理解为我想做什么？");
        CHECK(reading_context.status == LocalRouteStatus::Chat);
        CHECK(reading_context.intent.memory_context_query);
        CHECK(reading_context.intent.memory_context_query->subject == "阅读");

        AssistantService service(kTestMemoryPath, kTestEventPath);

        // A business-shaped request that the structured model cannot resolve
        // must fail closed instead of starting a second chat-model turn.
        ServiceResult semantic_rejection = service.process("请记住我不喜欢太刺眼的光");
        CHECK(semantic_rejection.task_type == IntentType::Clarify);
        CHECK(!semantic_rejection.call_llm);
        CHECK(!semantic_rejection.fixed_reply.empty());

        // Fast-path commands must not need the LLM router before validation.
        ServiceResult opened = service.process("打开客厅的灯");
        CHECK(opened.task_type == IntentType::DeviceControl);
        CHECK(!opened.call_llm);
        CHECK(opened.intent_latency_ms == 0);
        CHECK(opened.device_command);

        ServiceResult invalid = service.process("把客厅灯设置为二十度");
        CHECK(invalid.task_type == IntentType::DeviceControl);
        CHECK(!invalid.call_llm);
        CHECK(invalid.fixed_reply.find("不支持温度设置") != std::string::npos);

        ServiceResult first = service.process("打开空调");
        CHECK(first.task_type == IntentType::Clarify);
        CHECK(!first.call_llm);
        ServiceResult completed = service.process("客厅的");
        CHECK(completed.task_type == IntentType::DeviceControl);
        CHECK(!completed.call_llm);
        CHECK(completed.device_command);
        CHECK(completed.device_command->device_id == "living_room_ac");

        ServiceResult fault = service.process("请记录，客厅空调不制冷");
        CHECK(fault.task_type == IntentType::DeviceFault);
        CHECK(!fault.call_llm);
        CHECK(fault.device_event);

        ServiceResult stored = service.process("请记住，我的钥匙放在客厅鞋柜第二层");
        CHECK(stored.task_type == IntentType::MemoryWrite);
        CHECK(!stored.call_llm);
        CHECK(stored.stored_memory);
        CHECK(stored.stored_memory->subject == "钥匙");

        ServiceResult found = service.process("我的钥匙放在哪里");
        CHECK(found.task_type == IntentType::MemoryQuery);
        CHECK(!found.call_llm);
        CHECK(found.fixed_reply.find("客厅鞋柜第二层") != std::string::npos);

        IntentResult memory_aware_chat;
        memory_aware_chat.intent = IntentType::GeneralChat;
        memory_aware_chat.memory_context_query = MemoryQuery{"钥匙", "位置", "", ""};
        ServiceResult contextual = service.processAnalyzed("钥匙在哪儿？", memory_aware_chat);
        CHECK(contextual.call_llm);
        CHECK(contextual.runtime_context.find("客厅鞋柜第二层") != std::string::npos);

        IntentResult reply_with_memory;
        reply_with_memory.intent = IntentType::MemoryWrite;
        reply_with_memory.memory = makeItem("USER_PREFERENCE", "阅读照明", "偏好",
                                            "不喜欢太刺眼的光", 0);
        reply_with_memory.response_text = "已记住，阅读时我会优先给出柔和照明建议。";
        ServiceResult stored_reply = service.processAnalyzed("阅读时别太刺眼", reply_with_memory);
        CHECK(!stored_reply.call_llm);
        CHECK(stored_reply.fixed_reply == reply_with_memory.response_text);

        ServiceResult recalled_preference = service.process("你还记得我不喜欢哪种光吗？");
        CHECK(recalled_preference.task_type == IntentType::MemoryQuery);
        CHECK(!recalled_preference.call_llm);
        CHECK(recalled_preference.fixed_reply.find("不喜欢太刺眼的光") != std::string::npos);

        ServiceResult records = service.process("查看设备故障记录");
        CHECK(records.task_type == IntentType::RecordQuery);
        CHECK(!records.call_llm);
        CHECK(records.fixed_reply.find("客厅空调") != std::string::npos);
    }

    resetTestFile();

    {
        AssistantService service(kTestMemoryPath, kTestEventPath);
        IntentResult write;
        write.intent = IntentType::MemoryWrite;
        write.memory = makeItem("OBJECT_LOCATION", "护照", "位置", "书桌第二个抽屉", 0);
        ServiceResult stored = service.processAnalyzed("护照放在书桌第二个抽屉", write);
        CHECK(!stored.call_llm);
        CHECK(stored.stored_memory);
        CHECK(service.memorySnapshot().size() == 1);

        IntentResult del;
        del.intent = IntentType::MemoryDelete;
        MemoryDeleteRequest request;
        request.category = "OBJECT_LOCATION";
        request.subject = "护照";
        del.memory_delete = request;

        ServiceResult removed = service.processAnalyzed("忘掉护照的位置", del);
        CHECK(removed.task_type == IntentType::MemoryDelete);
        CHECK(!removed.call_llm);
        CHECK(removed.deleted_memory);
        CHECK(removed.fixed_reply.find("已删除") != std::string::npos);
        CHECK(service.memorySnapshot().empty());
    }

    resetTestFile();

    {
        AssistantService service(kTestMemoryPath, kTestEventPath);
        IntentResult write;
        write.intent = IntentType::MemoryWrite;
        write.memory = makeItem("USER_PREFERENCE", "卧室灯温度", "偏好", "26度", 0);
        ServiceResult stored = service.processAnalyzed("记住我卧室灯温度偏好是26度", write);
        CHECK(stored.stored_memory);
        CHECK(service.memorySnapshot().size() == 1);

        IntentResult misclassified;
        misclassified.intent = IntentType::MemoryWrite;
        misclassified.memory = makeItem("USER_PREFERENCE", "记忆清除", "状态", "已清除", 0);
        ServiceResult removed = service.processAnalyzed("清除卧室灯温度记忆", misclassified);
        CHECK(removed.task_type == IntentType::MemoryDelete);
        CHECK(!removed.call_llm);
        CHECK(!removed.stored_memory);
        CHECK(service.memorySnapshot().empty());
    }

    {
        DeviceEventValidator validator;
        std::string error;
        CHECK(validator.validate(
            makeEvent("", "空调", "FAULT", "好像不工作了", 100),
            &error));
        CHECK(!validator.validate(
            makeEvent("客厅", "", "FAULT", "好像不工作了", 100),
            &error));
        CHECK(error == "missing_device");
    }

    {
        MemoryItemValidator validator;
        std::string error;
        CHECK(validator.validate(
            makeItem("OBJECT_LOCATION", "雨伞", "位置", "玄关柜最下层", 100),
            &error));
        CHECK(!validator.validate(
            makeItem("CONFIRMED_FACT", "天气", "状态", "热", 100),
            &error));
        CHECK(error == "invalid_memory_category");
    }

    {
        EventLog log(kTestEventPath);
        CHECK(log.load());
        CHECK(log.append(makeEvent("客厅", "空调", "FAULT", "不制冷\t有异响\n需要检查", 200)));

        EventLog restored(kTestEventPath);
        CHECK(restored.load());
        auto snapshot = restored.snapshot();
        CHECK(snapshot.size() == 1);
        CHECK(snapshot[0].room == "客厅");
        CHECK(snapshot[0].device == "空调");
        CHECK(snapshot[0].event_type == "FAULT");
        CHECK(snapshot[0].description == "不制冷\t有异响\n需要检查");
        CHECK(snapshot[0].timestamp == 200);
        CHECK(restored.removeExact(snapshot[0]));
        CHECK(restored.save());
        CHECK(restored.snapshot().empty());
    }

    resetTestFile();

    {
        AssistantService service(kTestMemoryPath, kTestEventPath);
        IntentResult fault;
        fault.intent = IntentType::DeviceFault;
        fault.device_event = makeEvent("卧室", "空调", "FAULT", "不制冷", 0);
        ServiceResult stored_fault = service.processAnalyzed("卧室空调不制冷", fault);
        CHECK(stored_fault.task_type == IntentType::DeviceFault);
        CHECK(stored_fault.call_llm);

        ServiceResult fault_with_control_word =
            service.processAnalyzed("请记录，卧室空调设置异常", fault);
        CHECK(fault_with_control_word.task_type == IntentType::DeviceFault);

        IntentResult preference;
        preference.intent = IntentType::MemoryWrite;
        preference.memory = makeItem("USER_PREFERENCE", "空调温度", "偏好", "26度", 0);
        CHECK(service.processAnalyzed("记住我喜欢26度", preference).stored_memory);

        ServiceResult fault_records = service.process("查询设备故障记录");
        CHECK(fault_records.task_type == IntentType::RecordQuery);
        CHECK(!fault_records.call_llm);
        CHECK(fault_records.fixed_reply.find("卧室空调") != std::string::npos);

        ServiceResult preference_records = service.process("我有哪些偏好");
        CHECK(preference_records.task_type == IntentType::RecordQuery);
        CHECK(!preference_records.call_llm);
        CHECK(preference_records.fixed_reply.find("空调温度") != std::string::npos);
    }

    resetTestFile();

    {
        MemoryStore store(kTestMemoryPath);
        CHECK(store.load());
        store.upsert(makeItem("USER_PREFERENCE", "空调温度", "偏好", "26度", 100));
        store.upsert(makeItem("USER_PREFERENCE", "空调温度", "偏好", "25度", 101));
        auto snapshot = store.snapshot();
        CHECK(snapshot.size() == 1);
        CHECK(countTriple(snapshot, "USER_PREFERENCE", "空调温度", "偏好") == 1);
        CHECK(snapshot[0].value == "25度");
        CHECK(store.removeExact(snapshot[0]));
        CHECK(store.save());
        CHECK(store.snapshot().empty());
    }

    resetTestFile();

    {
        MemoryStore store(kTestMemoryPath);
        CHECK(store.load());
        store.upsert(makeItem("OBJECT_LOCATION", "雨伞", "位置", "玄关柜最下层", 100));
        CHECK(store.save());

        MemoryStore restored(kTestMemoryPath);
        CHECK(restored.load());
        auto snapshot = restored.snapshot();
        CHECK(snapshot.size() == 1);
        CHECK(snapshot[0].category == "OBJECT_LOCATION");
        CHECK(snapshot[0].subject == "雨伞");
        CHECK(snapshot[0].attribute == "位置");
        CHECK(snapshot[0].value == "玄关柜最下层");
    }

    resetTestFile();

    {
        MemoryStore store(kTestMemoryPath);
        CHECK(store.load());
        store.upsert(makeItem("OBJECT_LOCATION", "雨伞", "位置", "玄关柜最下层", 100));
        store.upsert(makeItem("USER_PREFERENCE", "空调温度", "偏好", "26度", 101));
        store.upsert(makeItem("OBJECT_LOCATION", "备用钥匙", "位置", "电视柜左边抽屉", 102));
        ContextBuilder builder;
        MemoryQuery query;
        query.subject = "雨伞";
        query.attribute = "位置";
        std::string context = builder.buildMemoryContext(query, store, 1, 120);
        CHECK(context.find("玄关柜最下层") != std::string::npos);
        CHECK(context.find("电视柜左边抽屉") == std::string::npos);
        CHECK(context.size() <= 120);
    }

    resetTestFile();

    {
        // A global lighting preference and a reading-specific one must coexist;
        // the scene-specific item should win when the query carries a scene.
        MemoryStore store(kTestMemoryPath);
        CHECK(store.load());
        MemoryItem global = makeItem("USER_PREFERENCE", "灯光", "偏好", "喜欢暖光", 100);
        global.scope = "全屋";
        MemoryItem reading = makeItem("USER_PREFERENCE", "灯光", "偏好", "不喜欢太刺眼", 101);
        reading.condition = "阅读时";
        reading.context = "阅读环境";
        reading.confidence = 95;
        store.upsert(global);
        store.upsert(reading);
        MemoryQuery reading_query{"阅读", "偏好", "阅读", ""};
        const auto matches = store.selectRelevant(reading_query, 2);
        CHECK(matches.size() == 2);
        CHECK(matches.front().condition == "阅读时");
        CHECK(matches.front().value == "不喜欢太刺眼");
        CHECK(store.save());

        MemoryStore restored(kTestMemoryPath);
        CHECK(restored.load());
        const auto snapshot = restored.snapshot();
        CHECK(snapshot.size() == 2);
        const auto restored_matches = restored.selectRelevant(reading_query, 1);
        CHECK(restored_matches.size() == 1);
        CHECK(restored_matches.front().condition == "阅读时");
        ContextBuilder builder;
        const std::string context = builder.buildMemoryContext(reading_query, restored);
        CHECK(context.find("不喜欢太刺眼") != std::string::npos);
        CHECK(context.find("条件：阅读时") != std::string::npos);
    }

    resetTestFile();
    std::cout << "[TestAssistantCore] all tests passed" << std::endl;
    return 0;
}
