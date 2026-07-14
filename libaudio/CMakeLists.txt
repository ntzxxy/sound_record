# 1. 先设置路径（重要：必须在 add_library 之前）
set(LIBRARY_OUTPUT_PATH ${PROJECT_BINARY_DIR}/lib)

# 2. 再生成库
add_library(audio_lib STATIC audio.cpp ringbuffer.cpp audio_player.cpp audio_player_class.cpp)
target_include_directories(audio_lib PRIVATE ${PROJECT_SOURCE_DIR}/libnet)

# 3. 最后改名
set_target_properties(audio_lib PROPERTIES OUTPUT_NAME "audio")