#include "llm.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::vector<std::string> g_chat_prompts;
std::vector<std::string> g_appended_contexts;
int g_reply_index = 0;

int copyFormatted(const std::string& text, char* buf, int buf_size) {
    if (!buf || buf_size <= 0) return static_cast<int>(text.size());
    const int copy_size = std::min(static_cast<int>(text.size()), buf_size - 1);
    if (copy_size > 0) std::memcpy(buf, text.data(), static_cast<std::size_t>(copy_size));
    buf[copy_size] = '\0';
    return static_cast<int>(text.size());
}

}  // namespace

namespace chat_agent_test {

void resetMock() {
    g_chat_prompts.clear();
    g_appended_contexts.clear();
    g_reply_index = 0;
}

const std::vector<std::string>& chatPrompts() { return g_chat_prompts; }
const std::vector<std::string>& appendedContexts() { return g_appended_contexts; }

}  // namespace chat_agent_test

extern "C" int llm_init(const char*) { return 0; }
extern "C" void llm_destroy(void) {}
extern "C" void llm_reset_context(void) {}
extern "C" int llm_get_context_size(void) { return 0; }

extern "C" int llm_append_text(const char* text) {
    g_appended_contexts.emplace_back(text ? text : "");
    return 0;
}

extern "C" int llm_format_prompt(const char* system_prompt, const char* user_message,
                                  char* buf, int buf_size) {
    const std::string formatted = std::string(system_prompt ? system_prompt : "") +
                                  "\nUSER:" + (user_message ? user_message : "");
    return copyFormatted(formatted, buf, buf_size);
}

extern "C" int llm_format_messages(const llm_chat_message_t* messages, int message_count,
                                    int, char* buf, int buf_size) {
    std::string formatted;
    for (int i = 0; i < message_count; ++i) {
        formatted += messages[i].role ? messages[i].role : "";
        formatted += ':';
        formatted += messages[i].content ? messages[i].content : "";
        formatted += '\n';
    }
    return copyFormatted(formatted, buf, buf_size);
}

extern "C" int llm_chat(const char* prompt, llm_callback_t callback) {
    g_chat_prompts.emplace_back(prompt ? prompt : "");
    const std::string reply = "reply-" + std::to_string(++g_reply_index);
    if (callback) {
        callback(reply.c_str(), 0);
        callback("", 1);
    }
    return 0;
}

extern "C" int llm_generate_once(const char*, const char*, const llm_once_params_t*,
                                  char*, int, int*) {
    return -1;
}
