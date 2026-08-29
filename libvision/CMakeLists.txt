cmake_minimum_required(VERSION 3.17)

find_package(OpenCV REQUIRED COMPONENTS core dnn imgproc imgcodecs highgui videoio freetype)

set(ONNXRUNTIME_ROOT "${PROJECT_SOURCE_DIR}/third_party/onnxruntime" CACHE PATH
    "Root directory of the local ONNX Runtime C++ package")
set(_onnxruntime_include "${ONNXRUNTIME_ROOT}/include")
set(_onnxruntime_library "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.so")
set(_vision_sources
    camera_source.cpp
)

if(EXISTS "${_onnxruntime_include}/onnxruntime_cxx_api.h" AND EXISTS "${_onnxruntime_library}")
    list(APPEND _vision_sources
        yolo_detector.cpp
        vision_pipeline.cpp
    )
    add_library(onnxruntime SHARED IMPORTED GLOBAL)
    set_target_properties(onnxruntime PROPERTIES
        IMPORTED_LOCATION "${_onnxruntime_library}"
        INTERFACE_INCLUDE_DIRECTORIES "${_onnxruntime_include}"
    )
    set(VISION_HAS_ONNXRUNTIME ON)
else()
    set(VISION_HAS_ONNXRUNTIME OFF)
    message(WARNING "ONNX Runtime was not found at ${ONNXRUNTIME_ROOT}; yolo_preview will not be built")
endif()

if(TARGET mtmd)
    list(APPEND _vision_sources scene_analyzer.cpp)
    set(VISION_HAS_VLM ON)
else()
    set(VISION_HAS_VLM OFF)
endif()

add_library(vision STATIC
    ${_vision_sources}
)

target_include_directories(vision
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(vision
    PUBLIC
        ${OpenCV_LIBS}
)

if(VISION_HAS_ONNXRUNTIME)
    target_link_libraries(vision PUBLIC onnxruntime)
    target_compile_definitions(vision PUBLIC VISION_HAS_ONNXRUNTIME=1)
endif()

if(VISION_HAS_VLM)
    target_link_libraries(vision PUBLIC mtmd llama)
    target_include_directories(vision PUBLIC
        ${PROJECT_SOURCE_DIR}/third_party/llama.cpp/include
        ${PROJECT_SOURCE_DIR}/third_party/llama.cpp/ggml/include
        ${PROJECT_SOURCE_DIR}/third_party/llama.cpp/tools/mtmd
    )
    target_compile_definitions(vision PUBLIC VISION_HAS_VLM=1)
endif()

target_compile_features(vision PUBLIC cxx_std_17)
target_compile_options(vision PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

message(STATUS ">>>> OpenCV camera capture foundation is enabled <<<<")
