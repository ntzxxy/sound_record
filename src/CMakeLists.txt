# 1. 首先生成程序（把目标创造出来，后面才能对它进行各种配置）
add_executable(out main.c key.c led.c)

# 2. 指定输出路径到 bin/（现代 CMake 的标准写法）
set_target_properties(out PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

# 3. 为创造出来的 "out" 指定头文件路径
target_include_directories(out PRIVATE
        ${PROJECT_SOURCE_DIR}/libaudio
        ${PROJECT_SOURCE_DIR}/libnet
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# 4. 链接库
target_link_libraries(out PRIVATE audio_lib net_lib asound pthread)

# 5. 运行脚本和声卡配置属于 runtime 文件。独立成 ALL target，避免只有 out 重新链接时才同步。
set(BOARD_RUNTIME_FILES
        ${PROJECT_SOURCE_DIR}/mic_in_config.sh
        ${PROJECT_SOURCE_DIR}/mic_in_config_usb.sh
        ${PROJECT_SOURCE_DIR}/run.sh
)

add_custom_target(sync_runtime_files ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PROJECT_BINARY_DIR}/bin
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${BOARD_RUNTIME_FILES}
        ${PROJECT_BINARY_DIR}/bin
        COMMAND chmod +x ${PROJECT_BINARY_DIR}/bin/run.sh ${PROJECT_BINARY_DIR}/bin/mic_in_config.sh ${PROJECT_BINARY_DIR}/bin/mic_in_config_usb.sh
        DEPENDS ${BOARD_RUNTIME_FILES}
        COMMENT "同步板端运行脚本到 bin 目录..."
)

add_dependencies(out sync_runtime_files)

# 6. 手动触发一键部署到板子 (cmake --build . --target deploy)
add_custom_target(deploy
        COMMAND sh ${PROJECT_SOURCE_DIR}/deploy_to_board.sh
        DEPENDS out
)
