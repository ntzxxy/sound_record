#ifndef CONTROL_GATEWAY_H
#define CONTROL_GATEWAY_H

#include "conversation_runtime.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace server {

class LocalControlGateway {
public:
    explicit LocalControlGateway(conversation::ConversationRuntime& runtime,
                                 int port = 18081);
    ~LocalControlGateway();

    LocalControlGateway(const LocalControlGateway&) = delete;
    LocalControlGateway& operator=(const LocalControlGateway&) = delete;

    bool start();
    void stop();
    void broadcast(const conversation::ConversationEvent& event);
    void broadcastStatus(const std::string& status, const std::string& message = "");

private:
    struct Client;

    void acceptLoop();
    void clientLoop(const std::shared_ptr<Client>& client);
    void handleLine(const std::shared_ptr<Client>& client, const std::string& line);
    void removeClient(int fd);
    std::string statusJson() const;

    conversation::ConversationRuntime& runtime_;
    int port_{18081};
    int listen_fd_{-1};
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::mutex clients_mutex_;
    std::vector<std::shared_ptr<Client>> clients_;
    std::vector<std::thread> client_threads_;

    // These values are derived entirely from existing server-side events; the
    // board protocol does not need a new status frame for the desktop UI.
    mutable std::mutex status_mutex_;
    std::string board_status_{"unknown"};
    std::string board_message_;
    std::string input_mode_{"text"};
    std::string last_intent_{"unknown"};
    uint64_t last_turn_id_{0};
    uint64_t last_event_timestamp_ms_{0};
};

}  // namespace server

#endif  // CONTROL_GATEWAY_H
