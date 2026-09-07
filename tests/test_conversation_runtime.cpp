#include "conversation_runtime.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <vector>

namespace {

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "[TestConversationRuntime] check failed: " #condition \
                      << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            std::abort(); \
        } \
    } while (0)

bool hasEvent(const std::vector<conversation::ConversationEvent>& events,
              conversation::EventType type) {
    for (const auto& event : events) {
        if (event.type == type) return true;
    }
    return false;
}

bool hasReplyText(const std::vector<conversation::ConversationEvent>& events,
                  const std::string& text) {
    for (const auto& event : events) {
        if (event.type == conversation::EventType::ReplyDelta && event.text == text) return true;
    }
    return false;
}

}  // namespace

int main() {
    const char* memory_path = "/tmp/conversation_runtime_test_memory.tsv";
    std::remove(memory_path);
    std::remove("/tmp/device_fault_events.tsv");

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<conversation::ConversationEvent> events;

    conversation::ConversationRuntime runtime(memory_path);
    CHECK(runtime.initialize());
    runtime.setEventCallback([&](const conversation::ConversationEvent& event) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(event);
        }
        if (event.type == conversation::EventType::ReplyFinal) cv.notify_one();
    });
    CHECK(runtime.start());

    conversation::ConversationRequest request;
    request.text = "测试文字输入";
    request.source = conversation::InputSource::Text;
    request.request_id = "ui-test-1";
    request.enable_tts = false;
    CHECK(runtime.submit(request) != 0);

    {
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return hasEvent(events, conversation::EventType::ReplyFinal);
        }));
    }

    runtime.stop();

    std::lock_guard<std::mutex> lock(mutex);
    CHECK(hasEvent(events, conversation::EventType::ModeChanged));
    CHECK(hasEvent(events, conversation::EventType::UserMessage));
    CHECK(hasEvent(events, conversation::EventType::IntentResult));
    CHECK(hasEvent(events, conversation::EventType::ReplyDelta));
    CHECK(hasEvent(events, conversation::EventType::ReplyFinal));
    CHECK(hasReplyText(events, "stub reply"));
    CHECK(events.front().source == conversation::InputSource::Text);
    std::cout << "[TestConversationRuntime] all tests passed" << std::endl;
    return 0;
}
