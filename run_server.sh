#!/bin/sh
# PC 端 ASR 接收服务一键启动
# 用法: ./run_server.sh [端口号]

PORT=${1:-8080}
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/cmake-build-wsl-local"
MODEL_DIR="${SCRIPT_DIR}/models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16"
SAVE_DIR="${SCRIPT_DIR}/voice_records"

cd "${BUILD_DIR}"
mkdir -p "${SAVE_DIR}"

echo "========================================="
echo "  ASR 流式识别服务"
echo "  端口: ${PORT}"
echo "  模型: ${MODEL_DIR}"
echo "  录音: ${SAVE_DIR}"
echo "========================================="

./bin/stream_receiver "${PORT}" "${SAVE_DIR}" "${MODEL_DIR}"
