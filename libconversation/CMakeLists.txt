cmake_minimum_required(VERSION 3.17)

add_library(conversation STATIC
    conversation_runtime.cpp
)

target_include_directories(conversation PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/libassistant
    ${PROJECT_SOURCE_DIR}/libllm
)

target_link_libraries(conversation PUBLIC assistant)

target_compile_features(conversation PUBLIC cxx_std_17)

target_compile_options(conversation PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
