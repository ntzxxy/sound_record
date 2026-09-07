#include "chat_agent.h"

extern "C" int agent_init(const char*, const char*) {
    return 0;
}

extern "C" void agent_destroy(void) {}

extern "C" void agent_reset(void) {}

extern "C" int agent_chat_with_context(const char*, const char*, agent_callback_t callback) {
    if (callback) callback("stub reply", 1);
    return 0;
}

extern "C" int agent_chat(const char* message, agent_callback_t callback) {
    return agent_chat_with_context(message, nullptr, callback);
}
