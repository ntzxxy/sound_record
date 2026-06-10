#!/bin/sh
# USB 麦克风初始化脚本（C-Media 等 USB Audio 设备）
# 用法: sh mic_in_config_usb.sh [card_number]
#       默认操作 card 1（USB 声卡），可传参数覆盖

CARD=${1:-1}

echo "[mic_usb] Configuring USB mic on card ${CARD}..."

# 列出可用控制器
amixer -c ${CARD} scontrols

# 打开 Mic 捕获、设置音量
amixer -c ${CARD} sset 'Mic' 80% cap 2>/dev/null || \
amixer -c ${CARD} sset 'Capture' 80% cap 2>/dev/null || \
echo "[mic_usb] Warning: no Mic/Capture control found on card ${CARD}"

# 如果存在 Auto Gain Control，关闭它（避免自动增益影响 ASR）
amixer -c ${CARD} sset 'Auto Gain Control' off 2>/dev/null

echo "[mic_usb] USB mic config done."
