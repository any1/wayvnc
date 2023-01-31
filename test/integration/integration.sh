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
# - vncdo for client testing is in the $PATH
#   (pip install vncdotool)

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
VNCDO=${VNCDO:-$(which vncdo)}
$VNCDO --version 2>/dev/null

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
		return 1
	fi
	sleep "$TIMEOUT_DELAY"
}

wait_until() {
	timeout_init 10
	local last
	until last=$("$@" 2>&1); do
		if ! timeout_check; then
			echo "Timeout waiting for $*" >&2
			printf "%s\n" "$last" >&2
			return 1
		fi
	done
	printf "%s\n" "$last"
}

wait_while() {
	timeout_init 10
	local last
	while last=$("$@" 2>&1); do
		if ! timeout_check; then
			echo "Timeout waiting for $*" >&2
			printf "%s\n" "$last" >&2
			return 1
		fi
	done
	printf "%s\n" "$last"
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
	wait_until [ -f "$SWAY_ENV" ] >/dev/null
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
WAYVNC_ADDRESS=127.0.0.1
WAYVNC_PORT=5999
start_wayvnc() {
	echo "Starting wayvnc..."
	WAYVNC_LOG=$XDG_RUNTIME_DIR/wayvnc.log
	$WAYVNC -L debug "$WAYVNC_ADDRESS" "$WAYVNC_PORT" &>$WAYVNC_LOG &
	WAYVNC_PID=$!
	# Wait for the VNC listening port
	echo "  Started $WAYVNC_PID"
	wait_until lsof -a -p$WAYVNC_PID -iTCP@$WAYVNC_ADDRESS:$WAYVNC_PORT \
		-sTCP:LISTEN >/dev/null
	echo "  Listening on $WAYVNC_ADDRESS:$WAYVNC_PORT"
	# Wait for the control socket
	wait_until [ -S "$XDG_RUNTIME_DIR/wayvncctl" ] >/dev/null
	echo "  Control socket ready"
}

stop_wayvnc() {
	[[ -z $WAYVNC_PID ]] && return 0
	echo "Stopping wayvnc ($WAYVNC_PID)"
	kill "$WAYVNC_PID"
	unset WAYVNC_PID
}

WAYVNCCTL_PID=
WAYVNCCTL_LOG=$XDG_RUNTIME_DIR/wayvncctl.log
WAYVNCCTL_EVENTS=$XDG_RUNTIME_DIR/wayvncctl.events
start_wayvncctl_events() {
	$WAYVNCCTL --verbose --wait --reconnect --json event-receive >"$WAYVNCCTL_EVENTS" 2>"$WAYVNCCTL_LOG" &
	WAYVNCCTL_PID=$!
}

stop_wayvncctl_events() {
	[[ -z $WAYVNCCTL_PID ]] && return 0
	echo "Stopping wayvncctl event recorder ($WAYVNCCTL_PID)"
	kill "$WAYVNCCTL_PID"
	unset WAYVNCCTL_PID
}

verify_events() {
	local expected=("$@")
	echo "Verifying recorded events"
	local name i=0
	while IFS= read -r EVT; do
		name=$(jq -r '.method' <<<"$EVT")
		ex=${expected[$((i++))]}
		echo "  Event: $name=~$ex"
		[[ $name == "$ex" ]] || return 1
	done <"$WAYVNCCTL_EVENTS"
	if [[ $i -lt ${#expected[@]} ]]; then
		while [[ $i -lt ${#expected[@]} ]]; do
			echo "  Missing: ${expected[$((i++))]}"
		done
		return 1
	fi
	echo "Ok"
}

cleanup() {
	result=$?
	set +e
	stop_wayvnc
	stop_sway
	stop_wayvncctl_events
	if [[ $result != 0 ]]; then
		echo
		echo SWAY LOG
		echo --------
		cat "$SWAY_LOG"
		echo
		echo WAYVNC_LOG
		echo ----------
		cat "$WAYVNC_LOG"
		echo
		echo WAYVNCCTL_LOG
		echo ----------
		cat "$WAYVNCCTL_LOG"
		echo
		echo VNCDO_LOG
		echo ----------
		cat "$VNCDO_LOG"
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
	echo "Checking wayvnc-exit command"
	$WAYVNCCTL wayvnc-exit &>/dev/null
	wait_while kill -0 $WAYVNC_PID >/dev/null
	echo "  wayvnc is shutdown"
	unset WAYVNC_PID
	echo "ok"
}

client() {
	VNCDO_LOG=$XDG_RUNTIME_DIR/vncdo.log
	$VNCDO -v --server=$WAYVNC_ADDRESS::$WAYVNC_PORT "$@" &>>$VNCDO_LOG
}

test_client_connect() {
	echo "Connecting to send ctrl+t"
	client key ctrl-t
	echo "  Looking for the result..."
	[[ -f $XDG_RUNTIME_DIR/test.txt ]]
	echo "Ok"
}

smoke_test() {
	start_sway
	start_wayvncctl_events
	start_wayvnc
	test_version_ipc
	wait_until verify_events \
		wayvnc-startup
	test_output_list_ipc
	test_client_connect
	wait_until verify_events \
		wayvnc-startup \
		client-connected \
		client-disconnected
	test_exit_ipc
	wait_until verify_events \
		wayvnc-startup \
		client-connected \
		client-disconnected \
		wayvnc-shutdown
	stop_wayvncctl_events
	stop_sway
}

smoke_test
