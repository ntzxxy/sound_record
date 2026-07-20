#!/bin/sh
# 一键启动：配置声卡 + 运行程序
# 运行方式：直接 ./run.sh
#
# ==== 默认配置 ====
SERVER_IP="192.168.0.14"   # PC 端 IP 地址
MIC_TYPE="usb"
# =================

cd "$(dirname "$0")"
chmod +x ./out ./run.sh

echo '[run.sh] runtime: AIV1 full-duplex board client'

if [ "$MIC_TYPE" = "wm8960" ]; then
    echo '[run.sh] Configuring on-board mic (wm8960)...'
    sh ./mic_in_config.sh
else
    echo '[run.sh] Configuring USB microphone...'
    sh ./mic_in_config_usb.sh
fi

echo "[run.sh] 目标服务器: ${SERVER_IP}:8080"
echo "[run.sh] Starting: ./out $MIC_TYPE --server $SERVER_IP"
./out $MIC_TYPE --server "$SERVER_IP"
