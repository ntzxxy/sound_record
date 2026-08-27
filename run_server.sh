#!/bin/sh
# PC 端 ASR 接收服务一键启动
# 用法: ./run_server.sh [端口号]

PORT=${1:-8080}
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/cmake-build-wsl-local}"
MODEL_DIR="${SCRIPT_DIR}/models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16"
# 在当前 6 GB 显存设备上的实测中，此 GGML Q4_0 版首 token 更快、生成吞吐更高。
# 可通过 LLM_MODEL=/path/to/model.gguf 切换到 Google 官方 QAT 版进行质量对比。
LLM_MODEL="${LLM_MODEL:-${SCRIPT_DIR}/models/gemma-4-E4B-it-Q4_0.gguf}"
TTS_MODEL="${TTS_MODEL:-${SCRIPT_DIR}/models/christina-tts-1.5-q4}"
SAVE_DIR="${SCRIPT_DIR}/voice_records"

if [ ! -x "${BUILD_DIR}/bin/stream_receiver" ]; then
    echo "[Error] stream_receiver not found or not executable: ${BUILD_DIR}/bin/stream_receiver"
    echo "        Build it first, for example:"
    echo "        cmake --build ${BUILD_DIR} --target stream_receiver -j1"
    exit 1
fi

cd "${BUILD_DIR}"
mkdir -p "${SAVE_DIR}"
mkdir -p "${SCRIPT_DIR}/voice_answers"

# ONNX Runtime 共享库路径
export LD_LIBRARY_PATH="${BUILD_DIR}/_deps/onnxruntime-src/lib:${LD_LIBRARY_PATH}"

# 关闭 llama.cpp CUDA graph 调试输出
export GGML_CUDA_GRAPH=0

# TTS 是当前主要瓶颈；默认给 ONNX Runtime 多线程，仍可用环境变量覆盖。
export TTS_NUM_THREADS="${TTS_NUM_THREADS:-4}"
# Christina Qwen3-TTS runs through its C++/GGML CLI. On Jetson build this CLI
# with GGML_CUDA=ON and set QWEN3_TTS_BACKEND=cuda (CPU is only a fallback).
export TTS_QWEN_CLI="${TTS_QWEN_CLI:-${SCRIPT_DIR}/third_party/qwen3-tts.cpp/build/qwen3-tts-cli}"
export QWEN3_TTS_BACKEND="${QWEN3_TTS_BACKEND:-auto}"

echo "========================================="
echo "  ASR + LLM + TTS 流式语音对话服务"
echo "  端口: ${PORT}"
echo "  ASR模型: ${MODEL_DIR}"
echo "  LLM模型: ${LLM_MODEL}"
echo "  TTS模型: ${TTS_MODEL}"
echo "  TTS线程: ${TTS_NUM_THREADS}"
echo "  录音: ${SAVE_DIR}"
echo "  构建目录: ${BUILD_DIR}"
echo "========================================="

./bin/stream_receiver "${PORT}" "${SAVE_DIR}" "${MODEL_DIR}" "${LLM_MODEL}" "${TTS_MODEL}"
