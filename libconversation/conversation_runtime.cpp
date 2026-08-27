#include "conversation_runtime.h"

#include "assistant_service.h"
#include "chat_agent.h"

#include <chrono>
#include <iostream>
#include <utility>

namespace conversation {

thread_local ConversationRuntime* ConversationRuntime::callback_runtime_ = nullptr;
thread_local const ConversationRequest* ConversationRuntime::callback_request_ = nullptr;

const char* toString(InputSource source) {
    return source == InputSource::Voice ? "voice" : "text";
}

const char* toString(EventType type) {
    switch (type) {
        case EventType::ModeChanged: return "mode_changed";
        case EventType::UserMessage: return "user_message";
        case EventType::IntentResult: return "intent_result";
        case EventType::ReplyDelta: return "reply_delta";
        case EventType::ReplyFinal: return "reply_final";
        case EventType::Error: return "error";
    }
    return "error";
}

ConversationRuntime::ConversationRuntime(std::string memory_path,
                                         std::string event_log_path)
    : assistant_service_(std::make_unique<assistant::AssistantService>(
          std::move(memory_path), std::move(event_log_path))) {}

ConversationRuntime::~ConversationRuntime() {
    stop();
}

bool ConversationRuntime::initialize() {
    return assistant_service_ && assistant_service_->initialize();
}

bool ConversationRuntime::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return true;
    worker_ = std::thread(&ConversationRuntime::workerLoop, this);
    return true;
}

void ConversationRuntime::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;
    queue_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

uint64_t ConversationRuntime::submit(ConversationRequest request) {
    if (request.text.empty() || !running_) return 0;
    const uint64_t task_id = next_request_id_.fetch_add(1);
    if (request.request_id.empty()) {
        request.request_id = std::to_string(task_id);
    }

    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        tasks_.push({std::move(request), false});
    }
    queue_cv_.notify_one();
    return task_id;
}

void ConversationRuntime::resetConversation() {
    if (!running_) return;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        tasks_.push({{}, true});
    }
    queue_cv_.notify_one();
}

void ConversationRuntime::setEventCallback(EventCallback callback) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callback_ = std::move(callback);
}

std::vector<assistant::MemoryItem> ConversationRuntime::memorySnapshot() const {
    return assistant_service_ ? assistant_service_->memorySnapshot()
                              : std::vector<assistant::MemoryItem>{};
}

std::vector<assistant::DeviceEvent> ConversationRuntime::eventSnapshot() const {
    return assistant_service_ ? assistant_service_->eventSnapshot()
                              : std::vector<assistant::DeviceEvent>{};
}

bool ConversationRuntime::deleteMemoryRecord(const assistant::MemoryItem& item) {
    return assistant_service_ && assistant_service_->deleteMemoryRecord(item);
}

bool ConversationRuntime::deleteDeviceFaultRecord(const assistant::DeviceEvent& event) {
    return assistant_service_ && assistant_service_->deleteDeviceFaultRecord(event);
}

void ConversationRuntime::emit(ConversationEvent event) const {
    if (event.timestamp_ms == 0) {
        event.timestamp_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }
    EventCallback callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback = callback_;
    }
    if (callback) callback(event);
}

void ConversationRuntime::workerLoop() {
    while (running_) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] { return !tasks_.empty() || !running_; });
            if (!running_ && tasks_.empty()) break;
            task = std::move(tasks_.front());
            tasks_.pop();
        }

        if (task.reset) {
            agent_reset();
            continue;
        }

        const ConversationRequest& request = task.request;
        emit({EventType::ModeChanged, request.source, request.turn_id,
              request.request_id, "", toString(request.source), "",
              request.enable_tts, false});
        emit({EventType::UserMessage, request.source, request.turn_id,
              request.request_id, request.text, "", "", request.enable_tts, false});

        assistant::ServiceResult result = assistant_service_->process(request.text);
        emit({EventType::IntentResult, request.source, request.turn_id,
              request.request_id, "", "", assistant::toString(result.task_type),
              request.enable_tts, false});

        if (!result.call_llm) {
            if (!result.fixed_reply.empty()) {
                emit({EventType::ReplyDelta, request.source, request.turn_id,
                      request.request_id, result.fixed_reply, "", "",
                      request.enable_tts, false});
            }
            emit({EventType::ReplyFinal, request.source, request.turn_id,
                  request.request_id, "", "", "", request.enable_tts, true});
            continue;
        }

        callback_runtime_ = this;
        callback_request_ = &request;
        const int ret = agent_chat_with_context(request.text.c_str(),
                                                result.runtime_context.c_str(),
                                                &ConversationRuntime::agentCallback);
        callback_request_ = nullptr;
        callback_runtime_ = nullptr;
        if (ret != 0) {
            emit({EventType::Error, request.source, request.turn_id,
                  request.request_id, "对话模型处理失败", "", "",
                  request.enable_tts, true});
        }
    }
}

void ConversationRuntime::agentCallback(const char* text, int is_final) {
    ConversationRuntime* runtime = callback_runtime_;
    const ConversationRequest* request = callback_request_;
    if (!runtime || !request) return;

    if (text && text[0]) {
        runtime->emit({EventType::ReplyDelta, request->source, request->turn_id,
                       request->request_id, text, "", "", request->enable_tts, false});
    }
    if (is_final) {
        runtime->emit({EventType::ReplyFinal, request->source, request->turn_id,
                       request->request_id, "", "", "", request->enable_tts, true});
    }
}

}  // namespace conversation
