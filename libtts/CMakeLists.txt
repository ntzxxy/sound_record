# libtts —— TTS 语音合成封装模块
# 依赖 sherpa-onnx，仅在 BUILD_ASR=ON 时编译（共用 sherpa-onnx）

add_library(tts STATIC tts.cpp)

target_include_directories(tts PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx
)

target_link_libraries(tts PUBLIC
    sherpa-onnx-core
)
