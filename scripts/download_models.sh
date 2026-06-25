#!/bin/sh
# 下载 ASR 模型文件
# 模型来源：sherpa-onnx 官方预训练模型

MODEL_DIR="$(cd "$(dirname "$0")" && pwd)/models/sherpa-onnx-streaming-zipformer-small-bilingual-zh-en-2023-02-16"

if [ -d "${MODEL_DIR}" ] && [ -f "${MODEL_DIR}/encoder-epoch-99-avg-1.onnx" ]; then
    echo "[OK] 模型已存在: ${MODEL_DIR}"
    exit 0
fi

mkdir -p "${MODEL_DIR}"
cd "${MODEL_DIR}"

BASE_URL="https://github.com/k2-fsa/sherpa-onnx/releases/download/asr-models"

echo "正在下载模型..."
# 只需要这几个核心文件（不用 int8 量化版）
FILES="encoder-epoch-99-avg-1.onnx decoder-epoch-99-avg-1.onnx joiner-epoch-99-avg-1.onnx tokens.txt"

for f in ${FILES}; do
    echo "  -> ${f}"
    wget -q "${BASE_URL}/${f}" -O "${f}" || {
        echo "下载失败: ${f}"
        echo "请手动从 https://k2-fsa.github.io/sherpa/onnx/pretrained_models/index.html 下载"
        exit 1
    }
done

echo "[OK] 模型下载完成: ${MODEL_DIR}"
