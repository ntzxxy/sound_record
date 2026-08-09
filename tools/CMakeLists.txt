cmake_minimum_required(VERSION 3.17)

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)

if(TARGET llm)
    add_executable(assistant_context_demo assistant_context_demo.cpp)

    target_link_libraries(assistant_context_demo PRIVATE assistant llm)

    target_compile_features(assistant_context_demo PRIVATE cxx_std_17)

    target_compile_options(assistant_context_demo PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
else()
    message(STATUS ">>>> BUILD_LLM=OFF, skip assistant_context_demo <<<<")
endif()
