#!/bin/sh
# PC 端 ASR 接收服务一键启动
# 用法: ./run_server.sh [端口号]

PORT=${1:-8080}
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/cmake-build-wsl-local"
MODEL_DIR="${SCRIPT_DIR}/models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16"
LLM_MODEL="${SCRIPT_DIR}/models/qwen2.5-3b-instruct-q4_k_m.gguf"
TTS_MODEL="${SCRIPT_DIR}/models/vits-tts-zh"
SAVE_DIR="${SCRIPT_DIR}/voice_records"

cd "${BUILD_DIR}"
mkdir -p "${SAVE_DIR}"
mkdir -p "${SCRIPT_DIR}/voice_answers"

# ONNX Runtime 共享库路径
export LD_LIBRARY_PATH="${BUILD_DIR}/_deps/onnxruntime-src/lib:${LD_LIBRARY_PATH}"

# 关闭 llama.cpp CUDA graph 调试输出
export GGML_CUDA_GRAPH=0

echo "========================================="
echo "  ASR + LLM + TTS 流式语音对话服务"
echo "  端口: ${PORT}"
echo "  ASR模型: ${MODEL_DIR}"
echo "  LLM模型: ${LLM_MODEL}"
echo "  TTS模型: ${TTS_MODEL}"
echo "  录音: ${SAVE_DIR}"
echo "========================================="

./bin/stream_receiver "${PORT}" "${SAVE_DIR}" "${MODEL_DIR}" "${LLM_MODEL}" "${TTS_MODEL}"
