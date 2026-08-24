#include "assistant_service.h"
#include "assistant_types.h"
#include "llm.h"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: assistant_context_demo <qwen-gguf-model-path> [memory-tsv-path]" << std::endl;
        return 1;
    }

    if (llm_init(argv[1]) != 0) {
        std::cerr << "[AssistantContextDemo] llm initialization failed: " << argv[1] << std::endl;
        return 1;
    }

    const std::string memory_path = argc >= 3 ? argv[2] : "./runtime/assistant_memory_v2.tsv";
    assistant::AssistantService service(memory_path);
    if (!service.initialize()) {
        std::cerr << "[AssistantContextDemo] memory initialization failed" << std::endl;
        llm_destroy();
        return 1;
    }

    std::cout << "[AssistantContextDemo] input empty line or Ctrl-D to exit" << std::endl;
    std::string line;
    while (true) {
        std::cout << "> " << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) break;

        assistant::ServiceResult result = service.process(line);
        if (!result.fixed_reply.empty()) {
            std::cout << "[Assistant] " << result.fixed_reply << std::endl;
        }
        std::cout << "[LLMRequired] " << (result.call_llm ? "true" : "false") << std::endl;
        if (result.call_llm) {
            std::cout << "[RuntimeContext]" << std::endl;
            if (result.runtime_context.empty()) {
                std::cout << "<empty>" << std::endl;
            } else {
                std::cout << result.runtime_context;
            }
        }
    }
    llm_destroy();
    return 0;
}
