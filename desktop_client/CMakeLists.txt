cmake_minimum_required(VERSION 3.17)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Network)

add_executable(desktop_client main.cpp)

set_target_properties(desktop_client PROPERTIES
    AUTOMOC ON
    AUTOUIC ON
    AUTORCC ON
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin
)

target_link_libraries(desktop_client PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Network
)

target_compile_features(desktop_client PRIVATE cxx_std_17)

target_compile_options(desktop_client PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
