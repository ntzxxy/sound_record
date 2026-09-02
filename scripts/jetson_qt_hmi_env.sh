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
