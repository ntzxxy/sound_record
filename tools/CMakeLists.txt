cmake_minimum_required(VERSION 3.17)

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)

if(TARGET assistant)
    add_executable(weather_query_demo weather_query_demo.cpp)
    target_link_libraries(weather_query_demo PRIVATE assistant)
    target_compile_features(weather_query_demo PRIVATE cxx_std_17)
    target_compile_options(weather_query_demo PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

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

if(TARGET vision)
    add_executable(camera_preview camera_preview.cpp)
    target_link_libraries(camera_preview PRIVATE vision)
    target_compile_features(camera_preview PRIVATE cxx_std_17)
    target_compile_options(camera_preview PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

if(TARGET vision AND TARGET onnxruntime)
    add_executable(yolo_preview yolo_preview.cpp)
    target_link_libraries(yolo_preview PRIVATE vision)
    if(TARGET asr)
        target_sources(yolo_preview PRIVATE microphone_asr.cpp)
        target_link_libraries(yolo_preview PRIVATE asr asound pthread)
        target_compile_definitions(yolo_preview PRIVATE VISION_HAS_LOCAL_ASR=1)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(PULSE_SIMPLE QUIET IMPORTED_TARGET libpulse-simple)
        endif()
        if(TARGET PkgConfig::PULSE_SIMPLE)
            target_link_libraries(yolo_preview PRIVATE PkgConfig::PULSE_SIMPLE)
            target_compile_definitions(yolo_preview PRIVATE VISION_HAS_PULSE_AUDIO=1)
        endif()
    endif()
    target_compile_features(yolo_preview PRIVATE cxx_std_17)
    target_compile_options(yolo_preview PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()

if(TARGET vision AND TARGET mtmd)
    add_executable(vision_chat_demo vision_chat_demo.cpp)
    target_link_libraries(vision_chat_demo PRIVATE vision)
    target_compile_features(vision_chat_demo PRIVATE cxx_std_17)
    target_compile_options(vision_chat_demo PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )
endif()
