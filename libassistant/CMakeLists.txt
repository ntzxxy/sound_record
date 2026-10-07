cmake_minimum_required(VERSION 3.17)

add_library(assistant STATIC
    assistant_types.cpp
    intent_json_parser.cpp
    intent_preprocessor.cpp
    device_registry.cpp
    validators.cpp
    event_log.cpp
    semantic_intent_router.cpp
    request_router.cpp
    memory_store.cpp
    context_builder.cpp
    weather_service.cpp
    assistant_service.cpp
)

target_include_directories(assistant PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/libllm
)

add_subdirectory(${PROJECT_SOURCE_DIR}/deps/uapi-cpp ${CMAKE_CURRENT_BINARY_DIR}/uapi-cpp)
target_link_libraries(assistant PUBLIC uapi)

target_compile_features(assistant PUBLIC cxx_std_17)

target_compile_options(assistant PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

# Model/tokenizer/prototype assets remain outside Git and are selected at
# runtime with SOUND_RECORD_SEMANTIC_ROUTER_MODEL_DIR.
set(ONNXRUNTIME_ROOT "${PROJECT_SOURCE_DIR}/third_party/onnxruntime" CACHE PATH
    "Path to an ONNX Runtime C/C++ distribution")
find_path(SOUND_RECORD_ONNXRUNTIME_INCLUDE_DIR onnxruntime_cxx_api.h
    HINTS "${ONNXRUNTIME_ROOT}" "$ENV{ONNXRUNTIME_ROOT}"
    PATH_SUFFIXES include)
find_library(SOUND_RECORD_ONNXRUNTIME_LIBRARY onnxruntime
    HINTS "${ONNXRUNTIME_ROOT}" "$ENV{ONNXRUNTIME_ROOT}"
    PATH_SUFFIXES lib lib64)
if(SOUND_RECORD_ONNXRUNTIME_INCLUDE_DIR AND SOUND_RECORD_ONNXRUNTIME_LIBRARY)
    target_compile_definitions(assistant PRIVATE SOUND_RECORD_HAS_ONNXRUNTIME=1)
    target_include_directories(assistant PRIVATE ${SOUND_RECORD_ONNXRUNTIME_INCLUDE_DIR})
    target_link_libraries(assistant PUBLIC ${SOUND_RECORD_ONNXRUNTIME_LIBRARY})
    set_property(TARGET assistant APPEND PROPERTY BUILD_RPATH
        "${ONNXRUNTIME_ROOT}/lib;${ONNXRUNTIME_ROOT}/lib64")
    message(STATUS "Semantic router: ONNX Runtime enabled")
else()
    message(STATUS "Semantic router: ONNX Runtime not found; safe NONE router will be used")
endif()
