#!/usr/bin/env bash
#
# This is free and unencumbered software released into the public domain.
#
# Anyone is free to copy, modify, publish, use, compile, sell, or
# distribute this software, either in source code form or as a compiled
# binary, for any purpose, commercial or non-commercial, and by any
# means.
#
# In jurisdictions that recognize copyright laws, the author or authors
# of this software dedicate any and all copyright interest in the
# software to the public domain. We make this dedication for the benefit
# of the public at large and to the detriment of our heirs and
# successors. We intend this dedication to be an overt act of
# relinquishment in perpetuity of all present and future rights to this
# software under copyright law.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
# OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
# ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
# OTHER DEALINGS IN THE SOFTWARE.
#
# For more information, please refer to <http://unlicense.org/>

# Integration test for wayvnc
#
# For now, this doesn't do much, but does check that some basic functionality isn't DOA
#
# Prerequisites:
# - wayvnc and wayvncctl are built in ../build/, or in the $PATH
#   - Override by setting $WAYVNC and $WAYVNCCTL or $WAYVNC_BUILD_DIR
# - sway and swaymsg are in the $PATH
#   - Override by setting $SWAY and $SWAYMSG
# - jq for parsing json output is in the $PATH
# - lsof for TCP port checking is in the $PATH

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

wait_until() {
	timeout_init 10
	until "$@" &>/dev/null; do
		timeout_check
	done
}

wait_while() {
	timeout_init 10
	while "$@" &>/dev/null; do
		timeout_check
	done
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
	wait_until [ -f "$SWAY_ENV" ]
	WAYLAND_DISPLAY=$(grep ^WAYLAND_DISPLAY= "$SWAY_ENV" | cut -d= -f2-)
	SWAYSOCK=$(grep ^SWAYSOCK= "$SWAY_ENV" | cut -d= -f2-)
	export WAYLAND_DISPLAY SWAYSOCK
	echo "  sway is managing $WAYLAND_DISPLAY at $SWAYSOCK"
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
	wait_until lsof -a -p$WAYVNC_PID -iTCP@$WAYVNC_ADDRESS:$WAYVNC_PORT \
		-sTCP:LISTEN
	echo "  Listening on $WAYVNC_ADDRESS:$WAYVNC_PORT"
	# Wait for the control socket
	wait_until [ -S "$XDG_RUNTIME_DIR/wayvncctl" ]
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
	wait_while kill -0 $WAYVNC_PID
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
