#include "chat_agent.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace chat_agent_test {
void resetMock();
const std::vector<std::string>& chatPrompts();
const std::vector<std::string>& appendedContexts();
}  // namespace chat_agent_test

namespace {

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "[TestChatAgent] check failed: " #condition \
                      << " at " << __FILE__ << ':' << __LINE__ << std::endl; \
            std::abort(); \
        } \
    } while (0)

void discardCallback(const char*, int) {}

}  // namespace

int main() {
    chat_agent_test::resetMock();
    CHECK(agent_init("mock.gguf", nullptr) == 0);

    for (int i = 1; i <= 6; ++i) {
        const std::string message = "turn-" + std::to_string(i);
        CHECK(agent_chat(message.c_str(), discardCallback) == 0);
    }

    const auto& rebuilt = chat_agent_test::appendedContexts();
    CHECK(!rebuilt.empty());
    // 第六轮前仅淘汰最旧的第一轮，重建内容保留第二至第五轮。
    CHECK(rebuilt.front().find("turn-1") == std::string::npos);
    CHECK(rebuilt.front().find("turn-2") != std::string::npos);
    CHECK(rebuilt.front().find("turn-5") != std::string::npos);

    const std::string oversized_context(3500, 'x');
    CHECK(agent_chat_with_context("查询结果怎么理解", oversized_context.c_str(),
                                  discardCallback) == 0);
    const auto& prompts = chat_agent_test::chatPrompts();
    CHECK(!prompts.empty());
    CHECK(prompts.back().find("完整原始数据已在界面展示") != std::string::npos);

    // 运行时工具数据可进入当轮 prompt，但不会进入重建后的长期历史。
    const auto& after_tool_rebuild = chat_agent_test::appendedContexts();
    CHECK(after_tool_rebuild.back().find(oversized_context) == std::string::npos);
    CHECK(after_tool_rebuild.back().find("查询结果怎么理解") != std::string::npos);

    agent_reset();
    std::cout << "[TestChatAgent] all tests passed" << std::endl;
    return 0;
}
