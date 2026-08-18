cmake_minimum_required(VERSION 3.17)

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)

add_executable(test_assistant_core
    test_assistant_core.cpp
    llm_stub.cpp
)

target_link_libraries(test_assistant_core PRIVATE assistant)

target_compile_features(test_assistant_core PRIVATE cxx_std_17)

target_compile_options(test_assistant_core PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
