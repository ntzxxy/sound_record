find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

add_executable(qt_hmi main.cpp)

set_target_properties(qt_hmi PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin
)

target_link_libraries(qt_hmi PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    conversation
    llm
)

target_compile_features(qt_hmi PRIVATE cxx_std_17)

target_compile_options(qt_hmi PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
