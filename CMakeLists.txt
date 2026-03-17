cmake_minimum_required(VERSION 3.5)
project(IMX_AI_ASSISTANT)

# 添加子目录，CMake 会按顺序进入这两个目录执行里面的 CMakeLists.txt
add_subdirectory(libaudio)
add_subdirectory(libnet)
add_subdirectory(src)
