cmake_minimum_required(VERSION 3.17)

# 服务端需要 C++17，和顶层项目及 libassistant 保持一致
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置生成的可执行二进制文件输出路径（输出到 build/bin 下）
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)

# 添加流式接收端可执行程序
add_executable(stream_receiver stream_receiver.cpp)

# 复用板子端 net.h 的 StreamHeader_t 帧协议定义
target_include_directories(stream_receiver PRIVATE
    ${PROJECT_SOURCE_DIR}/libnet
    ${PROJECT_SOURCE_DIR}/libasr
    ${PROJECT_SOURCE_DIR}/libllm
    ${PROJECT_SOURCE_DIR}/libassistant
    ${PROJECT_SOURCE_DIR}/libtts
    ${PROJECT_SOURCE_DIR}/libaudio
)

# 链接 ASR、LLM、TTS、AudioPlayer 模块
target_link_libraries(stream_receiver PRIVATE asr assistant llm tts audio_lib asound pthread)

target_compile_options(stream_receiver PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

# 构建提示信息
message(STATUS ">>>> 服务端 C++ 流式解析验证程序已就绪 <<<<")
