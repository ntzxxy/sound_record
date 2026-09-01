add_library(cockpit_pipeline STATIC
    local_voice_pipeline.cpp
)

target_include_directories(cockpit_pipeline PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(cockpit_pipeline PUBLIC cxx_std_17)

target_compile_options(cockpit_pipeline PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
