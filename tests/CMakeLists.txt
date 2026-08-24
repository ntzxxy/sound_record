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

add_executable(test_conversation_runtime
    test_conversation_runtime.cpp
    runtime_llm_stub.cpp
    chat_agent_stub.cpp
)

target_link_libraries(test_conversation_runtime PRIVATE conversation)

target_compile_features(test_conversation_runtime PRIVATE cxx_std_17)

target_compile_options(test_conversation_runtime PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_executable(test_control_gateway
    test_control_gateway.cpp
    runtime_llm_stub.cpp
    chat_agent_stub.cpp
    ${PROJECT_SOURCE_DIR}/server/control_gateway.cpp
)

target_include_directories(test_control_gateway PRIVATE
    ${PROJECT_SOURCE_DIR}/server
)

target_link_libraries(test_control_gateway PRIVATE conversation pthread)

target_compile_features(test_control_gateway PRIVATE cxx_std_17)

target_compile_options(test_control_gateway PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
