add_executable(cockpit_runtime
    main.cpp
    ${PROJECT_SOURCE_DIR}/src/key.c
)

set_target_properties(cockpit_runtime PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin
)

target_include_directories(cockpit_runtime PRIVATE
    ${PROJECT_SOURCE_DIR}/libaudio
    ${PROJECT_SOURCE_DIR}/libasr
    ${PROJECT_SOURCE_DIR}/libconversation
    ${PROJECT_SOURCE_DIR}/libllm
    ${PROJECT_SOURCE_DIR}/libnet
    ${PROJECT_SOURCE_DIR}/libpipeline
    ${PROJECT_SOURCE_DIR}/libtts
    ${PROJECT_SOURCE_DIR}/src
)

# net_lib stays linked only because audio_lib still contains the preserved
# legacy AIV1/TCP implementation. The cockpit_runtime code never opens it.
target_link_libraries(cockpit_runtime PRIVATE
    cockpit_pipeline
    asr
    conversation
    llm
    tts
    audio_lib
    net_lib
    asound
    pthread
)

target_compile_features(cockpit_runtime PRIVATE cxx_std_17)
target_compile_options(cockpit_runtime PRIVATE -Wall -Wextra -Wpedantic)
