#include "control_gateway.h"

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

namespace server {
namespace {

uint64_t nowUnixMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string jsonEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char c : value) {
        switch (c) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped.push_back(c); break;
        }
    }
    return escaped;
}

bool findJsonString(const std::string& json, const std::string& key, std::string* value) {
    const std::string marker = "\"" + key + "\"";
    std::size_t pos = json.find(marker);
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos + marker.size());
    if (pos == std::string::npos) return false;
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return false;
    ++pos;

    std::string parsed;
    bool escaped = false;
    for (; pos < json.size(); ++pos) {
        const char c = json[pos];
        if (escaped) {
            switch (c) {
                case 'n': parsed.push_back('\n'); break;
                case 'r': parsed.push_back('\r'); break;
                case 't': parsed.push_back('\t'); break;
                default: parsed.push_back(c); break;
            }
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            *value = std::move(parsed);
            return true;
        }
        parsed.push_back(c);
    }
    return false;
}

bool findJsonBool(const std::string& json, const std::string& key, bool default_value) {
    const std::string marker = "\"" + key + "\"";
    const std::size_t key_pos = json.find(marker);
    if (key_pos == std::string::npos) return default_value;
    const std::size_t colon = json.find(':', key_pos + marker.size());
    if (colon == std::string::npos) return default_value;
    const std::size_t value_pos = json.find_first_not_of(" \t", colon + 1);
    if (value_pos == std::string::npos) return default_value;
    return json.compare(value_pos, 4, "true") == 0;
}

std::string eventJson(const conversation::ConversationEvent& event) {
    std::ostringstream out;
    out << "{\"type\":\"" << conversation::toString(event.type) << "\""
        << ",\"request_id\":\"" << jsonEscape(event.request_id) << "\""
        << ",\"turn_id\":" << event.turn_id
        << ",\"source\":\"" << conversation::toString(event.source) << "\""
        << ",\"text\":\"" << jsonEscape(event.text) << "\""
        << ",\"mode\":\"" << jsonEscape(event.mode) << "\""
        << ",\"intent\":\"" << jsonEscape(event.intent) << "\""
        << ",\"enable_tts\":" << (event.enable_tts ? "true" : "false")
        << ",\"final\":" << (event.is_final ? "true" : "false")
        << ",\"timestamp_ms\":"
        << (event.timestamp_ms == 0 ? nowUnixMs() : event.timestamp_ms) << "}";
    return out.str();
}

}  // namespace

struct LocalControlGateway::Client {
    explicit Client(int socket_fd) : fd(socket_fd) {}

    int fd{-1};
    std::mutex write_mutex;

    bool sendLine(const std::string& line) {
        std::lock_guard<std::mutex> lock(write_mutex);
        std::string payload = line + "\n";
        const char* data = payload.data();
        std::size_t remaining = payload.size();
        while (remaining > 0) {
            const ssize_t sent = send(fd, data, remaining, MSG_NOSIGNAL);
            if (sent <= 0) return false;
            data += sent;
            remaining -= static_cast<std::size_t>(sent);
        }
        return true;
    }
};

LocalControlGateway::LocalControlGateway(conversation::ConversationRuntime& runtime,
                                         int port)
    : runtime_(runtime), port_(port) {}

LocalControlGateway::~LocalControlGateway() {
    stop();
}

bool LocalControlGateway::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return true;

    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        running_ = false;
        return false;
    }

    int reuse = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port_));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
        listen(listen_fd_, 4) < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        running_ = false;
        return false;
    }

    accept_thread_ = std::thread(&LocalControlGateway::acceptLoop, this);
    std::cout << "[ControlGateway] listening on 127.0.0.1:" << port_ << std::endl;
    return true;
}

void LocalControlGateway::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) return;
    if (listen_fd_ >= 0) {
        shutdown(listen_fd_, SHUT_RDWR);
        close(listen_fd_);
        listen_fd_ = -1;
    }
    if (accept_thread_.joinable()) accept_thread_.join();

    std::vector<std::shared_ptr<Client>> clients;
    std::vector<std::thread> client_threads;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients.swap(clients_);
        client_threads.swap(client_threads_);
    }
    for (const auto& client : clients) {
        shutdown(client->fd, SHUT_RDWR);
    }
    for (auto& client_thread : client_threads) {
        if (client_thread.joinable()) client_thread.join();
    }
}

void LocalControlGateway::acceptLoop() {
    while (running_) {
        sockaddr_in address{};
        socklen_t length = sizeof(address);
        const int client_fd = accept(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            if (running_) std::cerr << "[ControlGateway] accept failed" << std::endl;
            continue;
        }

        auto client = std::make_shared<Client>(client_fd);
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            clients_.push_back(client);
            client_threads_.emplace_back(&LocalControlGateway::clientLoop, this, client);
        }
    }
}

void LocalControlGateway::clientLoop(const std::shared_ptr<Client>& client) {
    std::string pending;
    char buffer[1024];
    while (running_) {
        const ssize_t received = recv(client->fd, buffer, sizeof(buffer), 0);
        if (received <= 0) break;
        pending.append(buffer, static_cast<std::size_t>(received));
        std::size_t newline = 0;
        while ((newline = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!line.empty()) handleLine(client, line);
        }
    }
    removeClient(client->fd);
    close(client->fd);
}

void LocalControlGateway::handleLine(const std::shared_ptr<Client>& client,
                                     const std::string& line) {
    std::string type;
    std::string request_id;
    if (!findJsonString(line, "type", &type)) {
        client->sendLine("{\"type\":\"error\",\"text\":\"missing request type\"}");
        return;
    }
    findJsonString(line, "request_id", &request_id);

    if (type == "submit_text") {
        std::string text;
        if (!findJsonString(line, "text", &text) || text.empty()) {
            client->sendLine("{\"type\":\"error\",\"text\":\"text is required\"}");
            return;
        }
        conversation::ConversationRequest request;
        request.text = std::move(text);
        request.source = conversation::InputSource::Text;
        request.request_id = request_id;
        request.enable_tts = findJsonBool(line, "enable_tts", false);
        const uint64_t task_id = runtime_.submit(std::move(request));
        if (task_id == 0) {
            client->sendLine("{\"type\":\"error\",\"text\":\"conversation runtime is not running\"}");
            return;
        }
        client->sendLine("{\"type\":\"accepted\",\"request_id\":\"" +
                         jsonEscape(request_id) + "\",\"task_id\":" +
                         std::to_string(task_id) + "}");
        return;
    }

    if (type == "get_status") {
        client->sendLine(statusJson());
        return;
    }

    if (type == "reset_conversation") {
        runtime_.resetConversation();
        client->sendLine("{\"type\":\"accepted\",\"request_id\":\"" +
                         jsonEscape(request_id) + "\"}");
        return;
    }

    client->sendLine("{\"type\":\"error\",\"text\":\"unsupported request type\"}");
}

std::string LocalControlGateway::statusJson() const {
    std::lock_guard<std::mutex> lock(status_mutex_);
    std::ostringstream out;
    out << "{\"type\":\"status\",\"service\":\"running\""
        << ",\"board_status\":\"" << jsonEscape(board_status_) << "\""
        << ",\"board_message\":\"" << jsonEscape(board_message_) << "\""
        << ",\"mode\":\"" << jsonEscape(input_mode_) << "\""
        << ",\"intent\":\"" << jsonEscape(last_intent_) << "\""
        << ",\"turn_id\":" << last_turn_id_
        << ",\"timestamp_ms\":" << (last_event_timestamp_ms_ == 0
                                            ? nowUnixMs()
                                            : last_event_timestamp_ms_)
        << "}";
    return out.str();
}

void LocalControlGateway::broadcast(const conversation::ConversationEvent& event) {
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        last_turn_id_ = event.turn_id;
        last_event_timestamp_ms_ = event.timestamp_ms == 0 ? nowUnixMs() : event.timestamp_ms;
        if (event.type == conversation::EventType::ModeChanged && !event.mode.empty()) {
            input_mode_ = event.mode;
        }
        if (event.type == conversation::EventType::IntentResult && !event.intent.empty()) {
            last_intent_ = event.intent;
        }
    }
    const std::string message = eventJson(event);
    std::vector<std::shared_ptr<Client>> clients;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients = clients_;
    }
    for (const auto& client : clients) client->sendLine(message);
}

void LocalControlGateway::broadcastStatus(const std::string& status,
                                          const std::string& message) {
    const uint64_t timestamp_ms = nowUnixMs();
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        board_status_ = status;
        board_message_ = message;
        last_event_timestamp_ms_ = timestamp_ms;
    }
    const std::string payload = "{\"type\":\"board_connection\",\"status\":\"" +
                                jsonEscape(status) + "\",\"text\":\"" +
                                jsonEscape(message) + "\",\"timestamp_ms\":" +
                                std::to_string(timestamp_ms) + "}";
    std::vector<std::shared_ptr<Client>> clients;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients = clients_;
    }
    for (const auto& client : clients) client->sendLine(payload);
}

void LocalControlGateway::broadcastMetric(const std::string& name, int64_t value,
                                          const std::string& unit, uint64_t turn_id,
                                          const std::string& detail) {
    const std::string payload = "{\"type\":\"metric\",\"name\":\"" +
                                jsonEscape(name) + "\",\"value\":" +
                                std::to_string(value) + ",\"unit\":\"" +
                                jsonEscape(unit) + "\",\"turn_id\":" +
                                std::to_string(turn_id) + ",\"text\":\"" +
                                jsonEscape(detail) + "\",\"timestamp_ms\":" +
                                std::to_string(nowUnixMs()) + "}";
    std::vector<std::shared_ptr<Client>> clients;
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients = clients_;
    }
    for (const auto& client : clients) client->sendLine(payload);
}

void LocalControlGateway::removeClient(int fd) {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
                                  [fd](const std::shared_ptr<Client>& client) {
                                      return client->fd == fd;
                                  }),
                   clients_.end());
}

}  // namespace server
