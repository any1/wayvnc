#!/usr/bin/env bash
#
# Integration test for wayvnc
#
# For now, this doesn't do much, but does check that some basic functionality isn't DOA
#
# Prerequisites:
# - wayvnc and wayvncctl are built in ../build/, or in the $PATH
#   - Override by setting $WAYVNC and $WAYVNCCTL
# - sway and swaymsg are in the $PATH
#   - Override by setting $SWAY and $SWAYMSG
# - jq for parsing json output is in the $PATH
# -lsof for TCP port checking
#

set -e

INTEGRATION_ROOT=$(realpath "$(dirname "$0")")
REPO_ROOT=$(realpath "$INTEGRATION_ROOT/../..")
WAYVNC_BUILD_DIR=${WAYVNC_BUILD_DIR:-$(realpath "$REPO_ROOT/build")}
if [[ -d $WAYVNC_BUILD_DIR ]]; then
    export PATH=$WAYVNC_BUILD_DIR:$PATH
fi
echo "Looking for required binaries..." 
WAYVNC=${WAYVNC:-$(which wayvnc)}
WAYVNCCTL=${WAYVNCCTL:-$(which wayvncctl)}
SWAY=${SWAY:-$(which sway)}
SWAYMSG=${SWAYMSG:-$(which swaymsg)}
echo "Found: $WAYVNC $WAYVNCCTL $SWAY $SWAYMSG"
$WAYVNC --version
$SWAY --version

export XDG_CONFIG_HOME=$INTEGRATION_ROOT/xdg_config
export XDG_RUNTIME_DIR=/tmp/wayvnc-integration-$$
mkdir -p "$XDG_RUNTIME_DIR"

TIMEOUT_COUNTER=0
TIMEOUT_MAXCOUNT=1
TIMEOUT_DELAY=0.1
timeout_init() {
    TIMEOUT_COUNTER=0
    TIMEOUT_MAXCOUNT=${1:-5}
    TIMEOUT_DELAY=${2:-0.1}
}

timeout_check() {
    if [[ $(( TIMEOUT_COUNTER++ )) -gt $TIMEOUT_MAXCOUNT ]]; then
        echo "Timeout"
        return 1
    fi
    sleep "$TIMEOUT_DELAY"
}

SWAY_ENV=$XDG_RUNTIME_DIR/sway.env
SWAY_PID=
start_sway() {
    echo "Starting sway..."
    SWAY_LOG=$XDG_RUNTIME_DIR/sway.log
    WLR_BACKENDS=headless \
    WLR_LIBINPUT_NO_DEVICES=1 \
    $SWAY &>"$SWAY_LOG" &
    SWAY_PID=$!
    timeout_init 10
    while [[ ! -f $SWAY_ENV ]]; do
        timeout_check
    done
    grep -e ^WAYLAND_DISPLAY -e ^SWAYSOCK $SWAY_ENV >$SWAY_ENV.trim
    . $SWAY_ENV.trim
    export WAYLAND_DISPLAY SWAYSOCK
    echo "  sway is running: $SWAYSOCK"
}

stop_sway() {
    [[ -z $SWAY_PID ]] && return 0
    echo "Stopping sway ($SWAY_PID)"
    kill "$SWAY_PID"
    unset SWAY_PID WAYLAND_DISPLAY SWAYSOCK
}

WAYVNC_PID=
WAYVNC_ADDRESS=localhost
WAYVNC_PORT=5999
start_wayvnc() {
    echo "Starting wayvnc..."
    WAYVNC_LOG=$XDG_RUNTIME_DIR/wayvnc.log
    $WAYVNC "$WAYVNC_ADDRESS" "$WAYVNC_PORT" &>$WAYVNC_LOG &
    WAYVNC_PID=$!
    # Wait for the VNC listening port
    echo "  Started $WAYVNC_PID"
    timeout_init
    while ! lsof -a -p$WAYVNC_PID -iTCP@$WAYVNC_ADDRESS:$WAYVNC_PORT -sTCP:LISTEN &>/dev/null; do
        timeout_check
    done
    echo "  Listening on $WAYVNC_ADDRESS:$WAYVNC_PORT"
    # Wait for the control socket
    timeout_init
    while [[ ! -S $XDG_RUNTIME_DIR/wayvncctl ]]; do
        timeout_check
    done
    echo "  Control socket ready"
}

stop_wayvnc() {
    [[ -z $WAYVNC_PID ]] && return 0
    echo "Stopping wayvnc ($WAYVNC_PID)"
    kill "$WAYVNC_PID"
    unset WAYVNC_PID
}

cleanup() {
    result=$?
    set +e
    stop_wayvnc
    stop_sway
    if [[ $result != 0 ]]; then
        echo SWAY LOG
        echo --------
        cat $SWAY_LOG
        echo
        echo WAYVNC_LOG
        echo ----------
        cat $WAYVNC_LOG
        exit
    fi
    rm -rf $XDG_RUNTIME_DIR
}
trap cleanup EXIT

test_version_ipc() {
    echo "Checking version command"
    local version
    version=$($WAYVNCCTL --json version)
    [[ -n $version ]]
    echo "  version IPC returned data"
    echo "ok"
}

test_output_list_ipc() {
    echo "Checking output-list command"
    local sway_json wayvnc_json
    sway_json=$($SWAYMSG -t get_outputs)
    wayvnc_json=$($WAYVNCCTL --json output-list)
    local sway_list wayvnc_list
    sway_list=$(jq -r '.[].name' <<<"$sway_json" | sort -u)
    wayvnc_list=$(jq -r '.[].name' <<<"$wayvnc_json" | sort -u)
    [[ "$sway_list" == "$wayvnc_list" ]]
    echo "  output-list IPC matches \`swaymsg -t get_outputs\`"
    echo "ok"
}

test_exit_ipc() {
    echo "Cbecking wayvnc-exit command"
    # Ignore errors because killing the socket races vs receiving
    # a return message: https://github.com/any1/wayvnc/issues/233
    $WAYVNCCTL wayvnc-exit &>/dev/null || true
    timeout_init
    while kill -0 $WAYVNC_PID &>/dev/null; do
        timeout_check
    done
    echo "  wayvnc is shutdown"
    unset WAYVNC_PID
    echo "ok"
}

smoke_test() {
    start_sway
    start_wayvnc
    test_version_ipc
    test_output_list_ipc
    test_exit_ipc
    stop_sway
}

smoke_test
