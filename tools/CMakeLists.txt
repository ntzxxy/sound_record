cmake_minimum_required(VERSION 3.17)

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)

add_executable(weather_query_demo weather_query_demo.cpp)
target_link_libraries(weather_query_demo PRIVATE assistant)
target_compile_features(weather_query_demo PRIVATE cxx_std_17)
target_compile_options(weather_query_demo PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

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
