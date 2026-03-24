cmake_minimum_required(VERSION 3.5)
project(IMX_AI_ASSISTANT C CXX) 

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 添加子目录，CMake 会按顺序进入这两个目录执行里面的 CMakeLists.txt
add_subdirectory(libaudio)
add_subdirectory(libnet)
add_subdirectory(src)
