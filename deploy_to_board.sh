#!/bin/sh
# 自动部署到 i.MX6ULL 板子（使用 SCP，因为板子 Dropbear 不支持 SFTP）
# 用法: sh deploy_to_board.sh [board_ip]
#       默认 IP 为 192.168.0.15

BOARD_IP=${1:-192.168.0.15}
BOARD_USER=root
BOARD_PASS=${BOARD_PASS:-1}
BOARD_PATH=/home/root/workspace/sound_record
SRC_DIR="$(cd "$(dirname "$0")/cmake-build-imx-board/bin" && pwd)"

echo "[deploy] Target: ${BOARD_USER}@${BOARD_IP}:${BOARD_PATH}"
echo "[deploy] Source: ${SRC_DIR}"

# 创建临时 askpass 脚本
ASKPASS_SCRIPT=$(mktemp)
echo '#!/bin/sh' > "$ASKPASS_SCRIPT"
echo "echo ${BOARD_PASS}" >> "$ASKPASS_SCRIPT"
chmod +x "$ASKPASS_SCRIPT"

# 使用 setsid + SCP 传输（绕过 sshpass 依赖）
for FILE in out run.sh mic_in_config.sh mic_in_config_usb.sh; do
    if [ -f "${SRC_DIR}/${FILE}" ]; then
        echo "[deploy] Uploading ${FILE}..."
        SSH_ASKPASS="$ASKPASS_SCRIPT" DISPLAY=dummy setsid scp \
            -o StrictHostKeyChecking=no \
            "${SRC_DIR}/${FILE}" \
            "${BOARD_USER}@${BOARD_IP}:${BOARD_PATH}/" 2>/dev/null
        if [ $? -eq 0 ]; then
            echo "[deploy]   OK"
        else
            echo "[deploy]   FAILED"
        fi
    fi
done

rm -f "$ASKPASS_SCRIPT"
echo "[deploy] Done."
