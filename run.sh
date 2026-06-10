#!/bin/sh
# 一键启动：配置声卡 + 运行程序
# 用法: ./run.sh              (默认 USB 麦克风)
#       ./run.sh wm8960       (板载咪头)
#       ./run.sh usb          (USB 麦克风，显式指定)

cd "$(dirname "$0")"

# 确保可执行权限（CLion SFTP 可能不保留权限）
chmod +x ./out ./run.sh

# 设备类型：默认 USB，可通过第一个参数切换(可选 usb\wm9860)
DEVICE=${1:-usb}

if [ "$DEVICE" = "wm8960" ]; then
    echo '[run.sh] Configuring on-board mic (wm8960)...'
    sh ./mic_in_config.sh
else
    echo '[run.sh] Configuring USB microphone...'
    sh ./mic_in_config_usb.sh
fi

echo '[run.sh] Starting out with device: '"$DEVICE"
./out "$DEVICE"
