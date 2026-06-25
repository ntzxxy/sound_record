#!/bin/sh
# 一键启动：配置声卡 + 运行程序
#
# ==== 用户配置区域（直接改这几个变量即可）====
MIC_TYPE="usb"           # usb 或 wm8960
ENABLE_UPLOAD=1          # 1=录音后上传到服务器, 0=仅本地保存
SERVER_IP="10.113.124.138"  # 上传目标服务器 IP
# =============================================

cd "$(dirname "$0")"
chmod +x ./out ./run.sh

# 根据 MIC_TYPE 选择 mixer 脚本
if [ "$MIC_TYPE" = "wm8960" ]; then
    echo '[run.sh] Configuring on-board mic (wm8960)...'
    sh ./mic_in_config.sh
else
    echo '[run.sh] Configuring USB microphone...'
    sh ./mic_in_config_usb.sh
fi

# 构建参数
ARGS="$MIC_TYPE"
if [ "$ENABLE_UPLOAD" = "1" ]; then
    ARGS="$ARGS --upload --server $SERVER_IP"
    echo "[run.sh] Upload enabled: ${SERVER_IP}:8080"
fi

echo '[run.sh] Starting: ./out' $ARGS
./out $ARGS
