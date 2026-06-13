cmake_minimum_required(VERSION 3.5)
project(IMX_AI_ASSISTANT C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 流传输开关（默认 ON），在顶层定义确保 libaudio/libnet/src 全部生效
option(STREAMING_MODE "Enable real-time TCP streaming mode" ON)
if(STREAMING_MODE)
    add_definitions(-DSTREAMING_MODE)
    message(STATUS ">>>> 流式传输模式已启用 (STREAMING_MODE = ON) <<<<")
else()
    message(STATUS ">>>> 传统本地文件保存模式 (STREAMING_MODE = OFF) <<<<")
endif()

# 添加子目录，CMake 会按顺序进入这两个目录执行里面的 CMakeLists.txt
add_subdirectory(libaudio)
add_subdirectory(libnet)
add_subdirectory(src)

# PC 端 server 只在使用 x86 原生编译器时才构建，arm 交叉编译时自动跳过
option(BUILD_SERVER "Build the PC-side stream receiver" ON)
if(BUILD_SERVER AND CMAKE_CROSSCOMPILING)
    message(STATUS ">>>> 交叉编译环境，自动跳过 stream_receiver（仅限 PC 端使用）<<<<")
    set(BUILD_SERVER OFF)
endif()
if(BUILD_SERVER)
    add_subdirectory(server)
endif()
