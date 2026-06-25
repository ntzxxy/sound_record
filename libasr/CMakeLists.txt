# libasr —— ASR 引擎封装模块
# 依赖 sherpa-onnx-core，仅在 BUILD_ASR=ON 时编译

add_library(asr STATIC asr.cpp)

target_include_directories(asr PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx
    ${CMAKE_BINARY_DIR}/_deps/onnxruntime-src/include
)

target_link_libraries(asr PUBLIC
    sherpa-onnx-core
)
