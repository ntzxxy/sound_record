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

# 5. 编译成功后自动同步/复制脚本到 bin 目录
add_custom_command(TARGET out POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${PROJECT_SOURCE_DIR}/mic_in_config.sh
        ${PROJECT_SOURCE_DIR}/mic_in_config_usb.sh
        ${PROJECT_SOURCE_DIR}/run.sh
        $<TARGET_FILE_DIR:out>
        COMMAND chmod +x $<TARGET_FILE_DIR:out>/run.sh $<TARGET_FILE_DIR:out>/mic_in_config.sh $<TARGET_FILE_DIR:out>/mic_in_config_usb.sh
        COMMENT "正在把运行脚本复制到 bin 目录并赋予权限..."
)

# 6. 手动触发一键部署到板子 (cmake --build . --target deploy)
add_custom_target(deploy
        COMMAND sh ${PROJECT_SOURCE_DIR}/deploy_to_board.sh
        DEPENDS out
)