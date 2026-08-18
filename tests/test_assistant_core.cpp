#include "context_builder.h"
#include "assistant_service.h"
#include "device_registry.h"
#include "event_log.h"
#include "intent_json_parser.h"
#include "memory_store.h"
#include "validators.h"

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
using assistant::MemoryQuery;
using assistant::MemoryStore;
using assistant::ServiceResult;

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
            "\"attribute\":\"偏好\",\"value\":\"关闭主灯\"},\"memory_query\":null,"
            "\"clarification_question\":\"\"}\n尾部文字";
        CHECK(parser.parse(wrapped, &result, &error));
        CHECK(result.intent == IntentType::MemoryWrite);
        CHECK(result.memory);
        CHECK(result.memory->category == "USER_PREFERENCE");
        CHECK(result.memory->subject == "睡眠照明");
        CHECK(result.memory->attribute == "偏好");
        CHECK(result.memory->value == "关闭主灯");
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
        CHECK(clarify.fixed_reply == "请问要打开哪个房间的空调？");

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
    std::cout << "[TestAssistantCore] all tests passed" << std::endl;
    return 0;
}
