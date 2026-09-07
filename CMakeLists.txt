cmake_minimum_required(VERSION 3.17)
project(IMX_AI_ASSISTANT C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
    set(_default_assistant_runtime OFF)
    set(_default_board_client OFF)
    set(_default_desktop_client ON)
else()
    set(_default_assistant_runtime ON)
    set(_default_board_client ON)
    set(_default_desktop_client OFF)
endif()

# 运行时依赖 Linux 网络、音频、推理库；Windows 上仅构建 Qt 客户端时不应被带入。
option(BUILD_ASSISTANT_RUNTIME "Build the assistant runtime and its dependencies" ${_default_assistant_runtime})

# 视觉模块与语音运行时解耦：开发期可以单独验证摄像头，后续也可复用到 Jetson。
option(BUILD_VISION "Build the OpenCV camera capture and vision foundation" ON)

# VLM 依赖 llama.cpp 的实验性 mtmd 库。默认关闭，避免普通 YOLO 预览额外加载 Gemma。
option(BUILD_VLM "Build the Gemma multimodal scene-analysis worker" OFF)

# 本地视觉工作台的语音输入测试：复用现有 sherpa-onnx ASR，但不启用
# 助手运行时、TCP 服务或 TTS，方便在 WSL/Jetson 上独立验证语音问图。
option(BUILD_VISION_VOICE_TEST "Build local microphone input for the vision workbench" OFF)

# 流传输开关（默认 ON），在顶层定义确保 libaudio/libnet/src 全部生效
option(STREAMING_MODE "Enable real-time TCP streaming mode" ON)
if(STREAMING_MODE)
    add_definitions(-DSTREAMING_MODE)
    message(STATUS ">>>> 流式传输模式已启用 (STREAMING_MODE = ON) <<<<")
else()
    message(STATUS ">>>> 传统本地文件保存模式 (STREAMING_MODE = OFF) <<<<")
endif()

# 开发板客户端依赖 ALSA、Linux Socket 等系统接口。桌面 Qt 客户端在 Windows
# 上构建时应关闭该选项，避免默认 all 目标误编译板端程序。
option(BUILD_BOARD_CLIENT "Build the Linux development-board client" ${_default_board_client})
if(BUILD_ASSISTANT_RUNTIME AND BUILD_BOARD_CLIENT)
    add_subdirectory(libaudio)
    add_subdirectory(libnet)
    add_subdirectory(src)
endif()

if(BUILD_ASSISTANT_RUNTIME)
    add_subdirectory(libassistant)
    add_subdirectory(libconversation)
    add_subdirectory(libpipeline)
else()
    message(STATUS ">>>> 未构建助手运行时；可独立构建 Qt 桌面控制客户端 <<<<")
endif()

# ASR 引擎仅在 PC/Jetson（非 arm 交叉编译）时构建
option(BUILD_ASR "Build sherpa-onnx ASR engine" ON)
if(BUILD_ASR AND CMAKE_CROSSCOMPILING)
    message(STATUS ">>>> 交叉编译环境，跳过 sherpa-onnx（仅限 PC 端使用）<<<<")
    set(BUILD_ASR OFF)
endif()
if((BUILD_ASSISTANT_RUNTIME OR (BUILD_VISION AND BUILD_VISION_VOICE_TEST)) AND BUILD_ASR)
    # sherpa-onnx v1.13.3 的外层 CMake 有子目录 add_subdirectory(sherpa-onnx)
    # 先把 CMAKE_CURRENT_SOURCE_DIR 指向正确位置才能找到 sherpa-onnx/ 子目录
    set(saved_CMAKE_CURRENT_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
    add_subdirectory(third_party/sherpa-onnx)
    add_subdirectory(libasr)
    if(BUILD_ASSISTANT_RUNTIME)
        add_subdirectory(libtts)
    endif()
endif()

# LLM 引擎仅在 PC/Jetson 时构建
option(BUILD_LLM "Build llama.cpp LLM engine" ON)
if(BUILD_LLM AND CMAKE_CROSSCOMPILING)
    message(STATUS ">>>> 交叉编译环境，跳过 llama.cpp（仅限 PC 端使用）<<<<")
    set(BUILD_LLM OFF)
endif()
if((BUILD_ASSISTANT_RUNTIME AND BUILD_LLM) OR (BUILD_VISION AND BUILD_VLM))
    if(BUILD_VLM)
        # 只构建可被 libvision 链接的 mtmd 库，不构建 llama.cpp 的工具集。
        set(LLAMA_BUILD_MTMD ON CACHE BOOL "llama: build multimodal library" FORCE)
        set(LLAMA_BUILD_COMMON OFF CACHE BOOL "llama: build common library" FORCE)
        set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "llama: build tools" FORCE)
    endif()
    add_subdirectory(third_party/llama.cpp)
endif()
if(BUILD_ASSISTANT_RUNTIME AND BUILD_LLM)
    add_subdirectory(libllm)
endif()

if(BUILD_VISION AND NOT CMAKE_CROSSCOMPILING)
    add_subdirectory(libvision)
endif()

# PC 端 server 只在使用 x86 原生编译器时才构建，arm 交叉编译时自动跳过
option(BUILD_SERVER "Build the PC-side stream receiver" ON)
if(BUILD_SERVER AND CMAKE_CROSSCOMPILING)
    message(STATUS ">>>> 交叉编译环境，自动跳过 stream_receiver（仅限 PC 端使用）<<<<")
    set(BUILD_SERVER OFF)
endif()
if(BUILD_ASSISTANT_RUNTIME AND BUILD_SERVER)
    add_subdirectory(server)
endif()

# P0 Jetson runtime: all voice modules are in one process and communicate
# through an in-memory bounded queue. server/libnet remain available as the
# preserved legacy AIV1 path, but are not used by this target.
option(BUILD_COCKPIT_RUNTIME "Build the single-process local cockpit runtime" ON)
if(BUILD_COCKPIT_RUNTIME AND BUILD_ASSISTANT_RUNTIME AND TARGET asr AND TARGET llm AND
   TARGET tts AND TARGET audio_lib AND TARGET net_lib)
    add_subdirectory(apps/cockpit_runtime)
endif()

# Board-side Qt HMI. Unlike desktop_client, this target calls
# ConversationRuntime directly and therefore does not depend on the legacy
# TCP control service.
option(BUILD_QT_HMI "Build the in-process Qt home HMI" OFF)
if(BUILD_QT_HMI AND BUILD_ASSISTANT_RUNTIME AND TARGET conversation AND TARGET llm)
    add_subdirectory(apps/qt_hmi)
endif()

option(BUILD_ASSISTANT_TOOLS "Build assistant context demo and tests" ON)
if(BUILD_ASSISTANT_TOOLS AND NOT CMAKE_CROSSCOMPILING AND (BUILD_ASSISTANT_RUNTIME OR BUILD_VISION))
    add_subdirectory(tools)
endif()

if(BUILD_ASSISTANT_RUNTIME AND BUILD_ASSISTANT_TOOLS AND NOT CMAKE_CROSSCOMPILING)
    add_subdirectory(tests)
endif()

option(BUILD_DESKTOP_CLIENT "Build the Qt desktop control client" ${_default_desktop_client})
if(BUILD_DESKTOP_CLIENT AND NOT CMAKE_CROSSCOMPILING)
    add_subdirectory(desktop_client)
endif()
