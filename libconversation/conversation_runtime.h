#ifndef CONVERSATION_RUNTIME_H
#define CONVERSATION_RUNTIME_H

#include "conversation_types.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace assistant {
class AssistantService;
}

namespace conversation {

class ConversationRuntime {
public:
    using EventCallback = std::function<void(const ConversationEvent&)>;

    ConversationRuntime(std::string memory_path, std::string event_log_path = "");
    ~ConversationRuntime();

    ConversationRuntime(const ConversationRuntime&) = delete;
    ConversationRuntime& operator=(const ConversationRuntime&) = delete;

    bool initialize();
    bool start();
    void stop();

    uint64_t submit(ConversationRequest request);
    void resetConversation();
    void setEventCallback(EventCallback callback);

private:
    struct Task {
        ConversationRequest request;
        bool reset{false};
    };

    void workerLoop();
    void emit(ConversationEvent event) const;
    static void agentCallback(const char* text, int is_final);

    std::unique_ptr<assistant::AssistantService> assistant_service_;
    std::queue<Task> tasks_;
    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> next_request_id_{1};

    mutable std::mutex callback_mutex_;
    EventCallback callback_;

    static thread_local ConversationRuntime* callback_runtime_;
    static thread_local const ConversationRequest* callback_request_;
};

}  // namespace conversation

#endif  // CONVERSATION_RUNTIME_H
