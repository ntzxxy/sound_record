# 1. 设置头文件路径
include_directories(${PROJECT_SOURCE_DIR}/libaudio)

# 2. 先设置输出路径（重要：必须在 add_executable 之前）
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)

# 3. 再生成程序
add_executable(assistant_app main.c)

# 4. 最后链接
target_link_libraries(assistant_app audio_lib asound pthread)