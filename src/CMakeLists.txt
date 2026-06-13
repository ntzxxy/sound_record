# 1. 设置头文件路径
include_directories(${PROJECT_SOURCE_DIR}/libaudio)
include_directories(${PROJECT_SOURCE_DIR}/libnet)
include_directories(.)      #在main中调用的时候去当前目录下找key

# 2. 先设置输出路径（重要：必须在 add_executable 之前）
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)

# 3. 再生成程序
add_executable(out main.c key.c led.c)

# 4. 最后链接
target_link_libraries(out audio_lib net_lib asound pthread)

# 5. 每次构建都检查并复制脚本到 bin/（不依赖 out 是否重编）
add_custom_target(deploy_scripts ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${PROJECT_SOURCE_DIR}/mic_in_config.sh
        ${EXECUTABLE_OUTPUT_PATH}/mic_in_config.sh
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${PROJECT_SOURCE_DIR}/mic_in_config_usb.sh
        ${EXECUTABLE_OUTPUT_PATH}/mic_in_config_usb.sh
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${PROJECT_SOURCE_DIR}/run.sh
        ${EXECUTABLE_OUTPUT_PATH}/run.sh
    COMMAND chmod +x ${EXECUTABLE_OUTPUT_PATH}/run.sh ${EXECUTABLE_OUTPUT_PATH}/mic_in_config.sh ${EXECUTABLE_OUTPUT_PATH}/mic_in_config_usb.sh
    COMMENT "Syncing scripts to bin/ for deployment"
    DEPENDS out
)

# 6. 手动触发部署到板子（不随每次构建自动执行，因为需要板子在线）
# 用法: cmake --build . --target deploy
add_custom_target(deploy
    COMMAND sh ${PROJECT_SOURCE_DIR}/deploy_to_board.sh
    COMMENT "Deploying to board via SCP..."
    DEPENDS out deploy_scripts
)