#include "control_gateway.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "[TestControlGateway] check failed: " #condition \
                      << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            std::abort(); \
        } \
    } while (0)

std::string receiveUntil(int fd, const std::string& marker) {
    std::string received;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(fd, &read_fds);
        timeval timeout{0, 100000};
        const int ready = select(fd + 1, &read_fds, nullptr, nullptr, &timeout);
        if (ready <= 0) continue;
        char buffer[1024];
        const ssize_t size = recv(fd, buffer, sizeof(buffer), 0);
        if (size <= 0) break;
        received.append(buffer, static_cast<std::size_t>(size));
        if (received.find(marker) != std::string::npos) break;
    }
    return received;
}

}  // namespace

int main() {
    const char* memory_path = "/tmp/control_gateway_test_memory.tsv";
    std::remove(memory_path);
    std::remove("/tmp/device_fault_events.tsv");

    conversation::ConversationRuntime runtime(memory_path);
    CHECK(runtime.initialize());
    server::LocalControlGateway gateway(runtime, 18081);
    runtime.setEventCallback([&](const conversation::ConversationEvent& event) {
        gateway.broadcast(event);
    });
    CHECK(runtime.start());
    CHECK(gateway.start());

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(18081);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);

    const char request[] =
        "{\"type\":\"submit_text\",\"request_id\":\"gateway-1\","
        "\"text\":\"测试文字输入\",\"enable_tts\":false}\n";
    CHECK(send(fd, request, sizeof(request) - 1, MSG_NOSIGNAL) ==
          static_cast<ssize_t>(sizeof(request) - 1));

    const std::string response = receiveUntil(fd, "reply_final");
    CHECK(response.find("\"type\":\"accepted\"") != std::string::npos);
    CHECK(response.find("\"type\":\"user_message\"") != std::string::npos);
    CHECK(response.find("\"source\":\"text\"") != std::string::npos);
    CHECK(response.find("\"type\":\"reply_final\"") != std::string::npos);

    close(fd);
    gateway.stop();
    runtime.stop();
    std::cout << "[TestControlGateway] all tests passed" << std::endl;
    return 0;
}
