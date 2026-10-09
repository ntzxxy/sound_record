#!/usr/bin/env bash
# Shared Jetson desktop/CUDA environment for the Qt HMI entry points.
# This file is intended to be sourced, not executed directly.

prepare_jetson_qt_hmi_env() {
    local runtime_dir=${1:?"runtime directory is required"}
    local login_user
    local login_uid

    login_user=$(id -un)
    login_uid=$(id -u)

    export DISPLAY="${DISPLAY:-:0}"
    export XAUTHORITY="${XAUTHORITY:-/home/${login_user}/.Xauthority}"

    # A process started from SSH does not inherit GNOME's session bus.  Fcitx5's
    # Qt6 frontend requires this address to communicate with the desktop IME.
    export DBUS_SESSION_BUS_ADDRESS="${QT_HMI_DBUS_SESSION_BUS_ADDRESS:-${DBUS_SESSION_BUS_ADDRESS:-unix:path=/run/user/${login_uid}/bus}}"
    export QT_IM_MODULE="${QT_HMI_QT_IM_MODULE:-fcitx}"
    export GTK_IM_MODULE="${QT_HMI_GTK_IM_MODULE:-fcitx}"
    export XMODIFIERS="${QT_HMI_XMODIFIERS:-@im=fcitx}"

    export CUDA_HOME="${CUDA_HOME:-/usr/local/cuda}"
    if [[ ! -d $CUDA_HOME ]]; then
        echo "[jetson-qt-env] CUDA_HOME does not exist: $CUDA_HOME" >&2
        return 1
    fi
    export PATH="$CUDA_HOME/bin:$PATH"
    export LD_LIBRARY_PATH="$CUDA_HOME/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

    export XDG_RUNTIME_DIR="$runtime_dir"
    mkdir -p "$XDG_RUNTIME_DIR"
    chmod 700 "$XDG_RUNTIME_DIR"
}

# Keep the semantic-router package and its ONNX Runtime dependency identical
# for the normal launcher and the benchmark launcher.  A missing package is a
# configuration error for the Jetson demo: silently falling back to the NONE
# router would make an acceptance run look valid while testing another path.
prepare_semantic_router_env() {
    local project_root=${1:?'project root is required'}
    local build_root=${2:?'build root is required'}
    local router_dir
    local onnxruntime_root
    local required_file

    if [[ -n ${SOUND_RECORD_SEMANTIC_ROUTER_MODEL_DIR:-} ]]; then
        router_dir=$SOUND_RECORD_SEMANTIC_ROUTER_MODEL_DIR
    elif [[ -d "$project_root/models/semantic_router/public_benchmark_v1" ]]; then
        router_dir="$project_root/models/semantic_router/public_benchmark_v1"
    else
        # Compatibility with the local training workspace; Jetson deployment
        # uses models/semantic_router/public_benchmark_v1 by default.
        router_dir="$project_root/runtime/semantic_router_phase2/phase3/public_model_package"
    fi
    for required_file in model.onnx router_config.tsv vocab.txt tfidf.tsv prototypes.tsv; do
        if [[ ! -f "$router_dir/$required_file" ]]; then
            echo "[jetson-qt-env] Semantic router file is missing: $router_dir/$required_file" >&2
            echo "[jetson-qt-env] Set SOUND_RECORD_SEMANTIC_ROUTER_MODEL_DIR to the deployed package." >&2
            return 1
        fi
    done
    # tfidf.tsv is intentionally empty when sparse_weight=0.  The other files
    # always carry runtime data and must not be empty.
    for required_file in model.onnx router_config.tsv vocab.txt prototypes.tsv; do
        if [[ ! -s "$router_dir/$required_file" ]]; then
            echo "[jetson-qt-env] Semantic router file is empty: $router_dir/$required_file" >&2
            return 1
        fi
    done
    export SOUND_RECORD_SEMANTIC_ROUTER_MODEL_DIR="$router_dir"

    onnxruntime_root=${ONNXRUNTIME_ROOT:-"$build_root/_deps/onnxruntime-src"}
    if [[ -f "$onnxruntime_root/lib/libonnxruntime.so" ]]; then
        export LD_LIBRARY_PATH="$onnxruntime_root/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    fi

    echo "[jetson-qt-env] Semantic router package: $SOUND_RECORD_SEMANTIC_ROUTER_MODEL_DIR"
}

verify_semantic_router_linkage() {
    local binary=${1:?'executable path is required'}
    local linkage

    if ! command -v ldd >/dev/null 2>&1; then
        echo "[jetson-qt-env] ldd is unavailable; cannot verify ONNX Runtime linkage." >&2
        return 1
    fi
    linkage=$(ldd "$binary" 2>&1 || true)
    if [[ $linkage != *libonnxruntime.so* ]]; then
        echo "[jetson-qt-env] Executable was built without ONNX Runtime: $binary" >&2
        echo "[jetson-qt-env] Reconfigure with -DONNXRUNTIME_ROOT=<onnxruntime distribution>." >&2
        return 1
    fi
    if [[ $linkage == *libonnxruntime.so*'not found'* ]]; then
        echo "[jetson-qt-env] libonnxruntime.so is linked but cannot be loaded: $binary" >&2
        return 1
    fi
}
