#!/usr/bin/env bash
# Interactive, audio-free benchmark for the Qt text HMI on Jetson.
# It keeps the HMI interactive while persisting per-turn LLM metrics and
# 1 Hz device/process resource samples in one self-contained session folder.
set -euo pipefail

if [[ $# -gt 3 ]]; then
    echo "Usage: $0 [qt_hmi_binary] [model.gguf] [benchmark_root]" >&2
    exit 2
fi

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
project_root=$(cd "$script_dir/.." && pwd)
binary=${1:-"$project_root/build-jetson-qt-gpu/bin/qt_hmi"}
model=${2:-"$project_root/models/gemma-4-E4B-it-Q4_0.gguf"}
benchmark_root=${3:-"$project_root/runtime/benchmarks"}

[[ $binary = /* ]] || binary="$PWD/$binary"
[[ $model = /* ]] || model="$PWD/$model"

if [[ ! -x $binary ]]; then
    echo "Qt HMI binary is not executable: $binary" >&2
    exit 1
fi
if [[ ! -f $model ]]; then
    echo "GGUF model does not exist: $model" >&2
    exit 1
fi

timestamp=$(date +%Y%m%d_%H%M%S)
session_dir="$benchmark_root/qt_llm_$timestamp"
runtime_dir="$session_dir/runtime"
xdg_runtime_dir="$project_root/runtime/xdg"
mkdir -p "$session_dir/raw" "$runtime_dir" "$xdg_runtime_dir"
source "$script_dir/jetson_qt_hmi_env.sh"
prepare_jetson_qt_hmi_env "$xdg_runtime_dir"

cache_file="$(dirname "$(dirname "$binary")")/CMakeCache.txt"
{
    echo "timestamp=$(date --iso-8601=seconds)"
    echo "git_head=$(git -C "$project_root" rev-parse HEAD)"
    echo "binary=$binary"
    echo "model=$model"
    echo "model_sha256=$(sha256sum "$model" | awk '{print $1}')"
    echo "display=$DISPLAY"
    echo "xauthority=$XAUTHORITY"
    echo "dbus_session_bus_address=$DBUS_SESSION_BUS_ADDRESS"
    echo "qt_im_module=$QT_IM_MODULE"
    echo "cuda_home=$CUDA_HOME"
    echo "nvcc=$(command -v nvcc || true)"
    nvcc -V 2>&1 || true
    if [[ -f $cache_file ]]; then
        grep -E '^(GGML_CUDA|CMAKE_CUDA_COMPILER|CMAKE_CUDA_ARCHITECTURES):' "$cache_file" || true
    fi
    uname -a
} > "$session_dir/environment.txt"

tegrastats_pid=""
if command -v tegrastats >/dev/null 2>&1; then
    tegrastats --interval 1000 > "$session_dir/raw/tegrastats.log" 2>&1 &
    tegrastats_pid=$!
else
    echo "tegrastats is unavailable" > "$session_dir/raw/tegrastats.log"
fi

cleanup() {
    if [[ -n $tegrastats_pid ]] && kill -0 "$tegrastats_pid" 2>/dev/null; then
        kill "$tegrastats_pid" 2>/dev/null || true
        wait "$tegrastats_pid" 2>/dev/null || true
    fi
    if [[ -n ${sampler_pid:-} ]] && kill -0 "$sampler_pid" 2>/dev/null; then
        kill "$sampler_pid" 2>/dev/null || true
        wait "$sampler_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

echo "Benchmark session: $session_dir"
echo "Close the Qt window when all interactive turns are complete."

"$binary" "$model" \
    --state-dir "$runtime_dir" \
    --benchmark-dir "$session_dir" \
    > "$session_dir/raw/qt_hmi.log" 2>&1 &
app_pid=$!

{
    echo "timestamp,pid,pcpu,pmem,rss_kb,vsz_kb,nlwp,elapsed"
    while kill -0 "$app_pid" 2>/dev/null; do
        stats=$(ps -p "$app_pid" -o %cpu= -o %mem= -o rss= -o vsz= -o nlwp= -o etime= | xargs || true)
        if [[ -n $stats ]]; then
            echo "$(date --iso-8601=seconds),$app_pid,${stats// /,}"
        fi
        sleep 1
    done
} > "$session_dir/raw/process_samples.csv" &
sampler_pid=$!

if wait "$app_pid"; then
    app_status=0
else
    app_status=$?
fi
echo "qt_hmi_exit_code=$app_status" > "$session_dir/summary.txt"
echo "Saved: $session_dir/turn_metrics.csv"
echo "Saved: $session_dir/raw/process_samples.csv"
echo "Saved: $session_dir/raw/tegrastats.log"
exit "$app_status"
