# libllm —— LLM 推理封装模块
# 依赖 llama.cpp，仅在 BUILD_LLM=ON 时编译

add_library(llm STATIC llm.cpp chat_agent.cpp)

target_include_directories(llm PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/llama.cpp/include
    ${CMAKE_SOURCE_DIR}/third_party/llama.cpp/ggml/include
    ${CMAKE_SOURCE_DIR}/third_party/llama.cpp/ggml/src
)

target_link_libraries(llm PUBLIC
    llama
)
