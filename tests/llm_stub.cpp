#include "llm.h"

extern "C" int llm_generate_once(const char*,
                                 const char*,
                                 const llm_once_params_t*,
                                 char* output,
                                 int output_size,
                                 int* latency_ms) {
    if (latency_ms) *latency_ms = 0;
    if (output && output_size > 0) output[0] = '\0';
    return -1;
}
