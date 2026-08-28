cmake_minimum_required(VERSION 3.17)

add_library(assistant STATIC
    assistant_types.cpp
    intent_json_parser.cpp
    intent_preprocessor.cpp
    device_registry.cpp
    validators.cpp
    event_log.cpp
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
