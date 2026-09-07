#!/usr/bin/env bash
# One-command Jetson Qt HMI launcher.
#
# Default usage:
#   ./scripts/run_jetson_qt_hmi.sh
#
# Optional overrides:
#   QT_HMI_BINARY=... QT_HMI_MODEL=... QT_HMI_STATE_DIR=... ./scripts/run_jetson_qt_hmi.sh
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
project_root=$(cd "$script_dir/.." && pwd)

binary=${QT_HMI_BINARY:-"$project_root/build-jetson-qt-gpu/bin/qt_hmi"}
model=${QT_HMI_MODEL:-"$project_root/models/gemma-4-E4B-it-Q4_0.gguf"}
state_dir=${QT_HMI_STATE_DIR:-"$project_root/runtime/qt_hmi"}
xdg_runtime_dir=${QT_HMI_XDG_RUNTIME_DIR:-"$project_root/runtime/xdg"}

if [[ ! -x $binary ]]; then
    echo "[run_jetson_qt_hmi] Executable not found: $binary" >&2
    echo "Build it first with: cmake --build build-jetson-qt-gpu --target qt_hmi -j 2" >&2
    exit 1
fi
if [[ ! -f $model ]]; then
    echo "[run_jetson_qt_hmi] GGUF model not found: $model" >&2
    exit 1
fi

source "$script_dir/jetson_qt_hmi_env.sh"
prepare_jetson_qt_hmi_env "$xdg_runtime_dir"
mkdir -p "$state_dir"

echo "[run_jetson_qt_hmi] DISPLAY=$DISPLAY, Qt IM=$QT_IM_MODULE, CUDA_HOME=$CUDA_HOME"
echo "[run_jetson_qt_hmi] Starting: $binary"
exec "$binary" "$model" --state-dir "$state_dir"
