#!/usr/bin/env bash
set -euo pipefail

workspace_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
runtime_lib_dir="${workspace_dir}/third_party/onnxruntime/lib"
binary_path="${workspace_dir}/cmake-build-vision/bin/yolo_preview"

if [[ ! -x "${binary_path}" ]]; then
    echo "未找到 ${binary_path}。请先执行 cmake --build cmake-build-vision --target yolo_preview -j4。" >&2
    exit 1
fi

cuda_library_paths="$(find "${workspace_dir}/.venv-vision/lib/python3.10/site-packages/nvidia" \
    -type d -name lib -printf '%p:' 2>/dev/null || true)"
if [[ -z "${cuda_library_paths}" ]]; then
    echo "未找到 .venv-vision 中的 CUDA 运行时，请先部署 YOLO 开发环境。" >&2
    exit 1
fi

export LD_LIBRARY_PATH="${runtime_lib_dir}:${cuda_library_paths}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
cd "${workspace_dir}"
exec "${binary_path}" "$@"
