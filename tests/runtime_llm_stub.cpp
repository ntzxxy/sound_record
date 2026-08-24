#include "llm.h"

#include <cstring>

extern "C" int llm_generate_once(const char*,
                                 const char*,
                                 const llm_once_params_t*,
                                 char* output,
                                 int output_size,
                                 int* latency_ms) {
    const char* json =
        "{\"intent\":\"GENERAL_CHAT\",\"device_command\":null,"
        "\"device_event\":null,\"memory\":null,\"memory_query\":null,"
        "\"memory_delete\":null,\"missing_slots\":[],"
        "\"clarification_question\":\"\"}";
    if (latency_ms) *latency_ms = 0;
    if (!output || output_size <= static_cast<int>(std::strlen(json))) return -1;
    std::strcpy(output, json);
    return 0;
}
