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
# - vncdo for client testing is in the $PATH
#   (pip install vncdotool)

set -e

GREEN='\033[0;32m'
RED='\033[0;31m'
RESET='\033[0m'

print_ok()
{
	printf "  ${GREEN}Ok${RESET}\n";
}

print_fail()
{
	printf "${RED}Fail${RESET}: %s\n" "$*" >&2
}

catch_crash()
{
	# LeakSanitizer does not work under ptrace and fails on exit
	ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0" \
	exec gdb -batch -ex "handle SIGTERM nostop noprint pass" \
		-ex run -ex "thread apply all bt full" --args $@
}

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
VNCDO=${VNCDO:-$(which vncdo)}
echo "Found: $WAYVNC $WAYVNCCTL $SWAY $SWAYMSG $VNCDO"

check_tool() {
	if ! "$@"; then
		print_fail "Could not run $*"
		exit 1
	fi
}

check_tool $WAYVNC --version
check_tool $SWAY --version
check_tool $VNCDO --version
IFS=" .-" read -r _ _ SWAYMAJOR SWAYMINOR _ < <($SWAY --version)

export XDG_CONFIG_HOME=$INTEGRATION_ROOT/xdg_config
export XDG_RUNTIME_DIR=/tmp/wayvnc-integration-$$

test_setup() {
	[[ -d "$XDG_RUNTIME_DIR" ]] && rm -rf "$XDG_RUNTIME_DIR"
	mkdir -p "$XDG_RUNTIME_DIR"
	echo "=============================================="
	echo "$*"
	echo "=============================================="
}

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
	until last=$(eval "$*" 2>&1); do
		if ! timeout_check; then
			print_fail "Timeout waiting for $*"
			printf "%s\n" "$last" >&2
			return 1
		fi
	done
	[[ -z $last ]] || printf "%s\n" "$last"
}

SWAY_ENV=$XDG_RUNTIME_DIR/sway.env
SWAY_PID=
start_sway() {
	echo "Starting sway..."
	SWAY_LOG=$XDG_RUNTIME_DIR/sway.log
	WLR_BACKENDS=headless \
	WLR_RENDERER=pixman \
	WLR_LIBINPUT_NO_DEVICES=1 \
	$SWAY &>"$SWAY_LOG" &
	SWAY_PID=$!
	wait_until [[ -f "$SWAY_ENV" ]] >/dev/null
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
	rm -f "$SWAY_ENV" || true
}

WAYVNC_PID=
WAYVNC_ADDRESS=127.0.0.1
WAYVNC_PORT=5999
start_wayvnc() {
	echo "Starting wayvnc..."
	WAYVNC_LOG=$XDG_RUNTIME_DIR/wayvnc.log
	catch_crash $WAYVNC "$@" -L debug "$WAYVNC_ADDRESS" "$WAYVNC_PORT" &>$WAYVNC_LOG &
	WAYVNC_PID=$!
	# Wait for the VNC listening port
	echo "  Started $WAYVNC_PID"
	wait_until nc -z -w1 $WAYVNC_ADDRESS $WAYVNC_PORT
	echo "  Listening on $WAYVNC_ADDRESS:$WAYVNC_PORT"
	# Wait for the control socket
	wait_until [[ -S "$XDG_RUNTIME_DIR/wayvncctl" ]] >/dev/null
	echo "  Control socket ready"
}

stop_wayvnc() {
	[[ -z $WAYVNC_PID ]] && return 0
	echo "Stopping wayvnc ($WAYVNC_PID)"
	pkill -TERM -P "$WAYVNC_PID"
	wait "$WAYVNC_PID" || true
	unset WAYVNC_PID
}

WAYVNCCTL_PID=
WAYVNCCTL_LOG=$XDG_RUNTIME_DIR/wayvncctl.log
WAYVNCCTL_EVENTS=$XDG_RUNTIME_DIR/wayvncctl.events
WAYVNCCTL_EVENTS_CONSUMED=$XDG_RUNTIME_DIR/wayvncctl.events.consumed
start_wayvncctl_events() {
	echo 0 >"$WAYVNCCTL_EVENTS_CONSUMED"
	$WAYVNCCTL --verbose --wait --reconnect --json event-receive >"$WAYVNCCTL_EVENTS" 2>"$WAYVNCCTL_LOG" &
	WAYVNCCTL_PID=$!
}

stop_wayvncctl_events() {
	[[ -z $WAYVNCCTL_PID ]] && return 0
	echo "Stopping wayvncctl event recorder ($WAYVNCCTL_PID)"
	kill "$WAYVNCCTL_PID"
	rm -f "$WAYVNCCTL_EVENTS" "$WAYVNCCTL_EVENTS_CONSUMED" || true
	unset WAYVNCCTL_PID
}

# Verifies the events recorded since the last successful verification
verify_events() {
	local expected=("$@")
	local consumed
	consumed=$(cat "$WAYVNCCTL_EVENTS_CONSUMED")
	echo "Verifying recorded events"
	local name i=0
	while IFS= read -r EVT; do
		name=$(jq -r '.method' <<<"$EVT")
		ex=${expected[$((i++))]}
		echo "  Event: $name=~$ex"
		[[ $name == "$ex" ]] || return 1
	done < <(tail -n +$((consumed + 1)) "$WAYVNCCTL_EVENTS")
	if [[ $i -lt ${#expected[@]} ]]; then
		while [[ $i -lt ${#expected[@]} ]]; do
			print_fail "  Missing: ${expected[$((i++))]}"
		done
		return 1
	fi
	echo $((consumed + i)) >"$WAYVNCCTL_EVENTS_CONSUMED"
	print_ok
}

cleanup() {
	result=$?
	set +e
	stop_lingering_client
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
	[[ -d "$XDG_RUNTIME_DIR" ]] && rm -rf "$XDG_RUNTIME_DIR"
}
trap cleanup EXIT

test_version_ipc() {
	echo "Checking version command"
	local version
	version=$($WAYVNCCTL --json version)
	[[ -n $version ]]
	echo "  version IPC returned data"
	print_ok
}

sway_active_outputs() {
	$SWAYMSG -t get_outputs | jq 'map(select(.active == true))'
}

test_output_list_ipc() {
	local expected_capture=${1:-HEADLESS-1}
	echo "Checking output-list command"
	local sway_json wayvnc_json
	sway_json=$(sway_active_outputs)
	wayvnc_json=$($WAYVNCCTL --json output-list)
	local sway_list wayvnc_list
	sway_list=$(jq -r '.[].name' <<<"$sway_json" | sort -u)
	wayvnc_list=$(jq -r '.[].name' <<<"$wayvnc_json" | sort -u)
	[[ "$sway_list" == "$wayvnc_list" ]]
	echo "  output-list IPC matches \`swaymsg -t get_outputs\`"
	wayvnc_capturing=$(jq -r '.[] | select(.captured == true).name' <<<"$wayvnc_json")
	echo "  Capturing: $wayvnc_capturing=~$expected_capture"
	[[ $wayvnc_capturing == "$expected_capture" ]]
	print_ok
}

verify_wayvnc_exited() {
	wait_until ! kill -0 $WAYVNC_PID >/dev/null
	unset WAYVNC_PID
}

verify_wayvnc_exited_normally() {
	verify_wayvnc_exited
	# The exit status is swallowed by gdb, so check its report instead
	grep -q "exited normally" "$WAYVNC_LOG"
}

test_exit_ipc() {
	echo "Checking wayvnc-exit command"
	$WAYVNCCTL wayvnc-exit &>/dev/null
	verify_wayvnc_exited
	echo "  wayvnc is shutdown"
	print_ok
}

client() {
	VNCDO_LOG=$XDG_RUNTIME_DIR/vncdo.log
	$VNCDO -v --server=$WAYVNC_ADDRESS::$WAYVNC_PORT "$@" &>>$VNCDO_LOG
}

LINGERING_CLIENT_PID=
start_lingering_client() {
	echo "Connecting a lingering client"
	VNCDO_LOG=$XDG_RUNTIME_DIR/vncdo.log
	$VNCDO -v --server=$WAYVNC_ADDRESS::$WAYVNC_PORT pause 60 &>>$VNCDO_LOG &
	LINGERING_CLIENT_PID=$!
	wait_until verify_events \
		client-connected
}

stop_lingering_client() {
	[[ -z $LINGERING_CLIENT_PID ]] && return 0
	echo "Stopping lingering client ($LINGERING_CLIENT_PID)"
	kill "$LINGERING_CLIENT_PID" 2>/dev/null || true
	unset LINGERING_CLIENT_PID
}

test_client_connect() {
	echo "Connecting to send ctrl+t"
	client key ctrl-t
	echo "  Looking for the result..."
	[[ -f $XDG_RUNTIME_DIR/test.txt ]]
	print_ok
}

output_count() {
	sway_active_outputs | jq 'length'
}

sway_output_create() {
	local initial_count
	initial_count=$(output_count)
	echo "Creating new output"
	$SWAYMSG create_output &>/dev/null
	# shellcheck disable=SC2016
	wait_until [[ '$(output_count)' -gt "$initial_count" ]]
	echo "  $(sway_active_outputs | jq -r '.[-1].name')"
	print_ok
}

sway_output_is_gone() {
	local output=$1
	$SWAYMSG -t get_outputs | jq -e "all(.name != \"$output\")"
}

sway_output_destroy() {
	local output=$1
	echo "Removing output $output"
	$SWAYMSG output "$output" unplug >/dev/null
	wait_until sway_output_is_gone "$output" >/dev/null
	print_ok
}

smoke_test() {
	local extra_test=$1
	shift
	test_setup "smoke test${*:+ ($*)}"
	start_sway
	start_wayvncctl_events
	start_wayvnc "$@"
	test_version_ipc
	wait_until verify_events \
		wayvnc-startup
	$extra_test
	test_client_connect
	wait_until verify_events \
		client-connected \
		client-disconnected
	test_exit_ipc
	wait_until verify_events \
		wayvnc-shutdown
	stop_wayvncctl_events
	stop_sway
}

multioutput_test() {
	test_setup "multioutput test"
	start_sway
	sway_output_create
	start_wayvncctl_events

	# Ensure outout selection commandline works
	start_wayvnc -o HEADLESS-1
	wait_until verify_events \
		wayvnc-startup
	test_output_list_ipc HEADLESS-1

	# Test outout-cycle
	$WAYVNCCTL output-cycle
	wait_until verify_events \
		capture-changed
	test_output_list_ipc HEADLESS-2

	# Test outout-cycle wraps
	$WAYVNCCTL output-cycle
	wait_until verify_events \
		capture-changed
	test_output_list_ipc HEADLESS-1

	# Add a new output, then switch to it
	sway_output_create
	wait_until test_output_list_ipc HEADLESS-1
	$WAYVNCCTL output-set HEADLESS-3
	wait_until verify_events \
		output-added \
		capture-changed
	test_output_list_ipc HEADLESS-3

	if [[ $SWAYMAJOR -le 1 && $SWAYMINOR -lt 8 ]]; then
		echo "Warning: sway-1.8 or later is needed for complete testing"
		return 0
	fi
	# Remove the output, and make sure we fallback properly
	start_lingering_client
	sway_output_destroy HEADLESS-3
	wait_until verify_events \
		capture-changed \
		output-removed
	wait_until test_output_list_ipc HEADLESS-1

	# Remove the remaining outputs, and make sure we exit normally
	sway_output_destroy HEADLESS-2
	sway_output_destroy HEADLESS-1
	echo "Checking that wayvnc exits normally"
	verify_wayvnc_exited_normally
	echo "  wayvnc exited normally"
	print_ok
	wait_until verify_events \
		output-removed \
		output-removed \
		wayvnc-shutdown
	stop_lingering_client
	stop_sway
	stop_wayvncctl_events
}

test_output_list_empty() {
	echo "Checking output-list is empty when detached"
	local wayvnc_json
	wayvnc_json=$($WAYVNCCTL --json output-list)
	[[ $(jq 'length' <<<"$wayvnc_json") -eq 0 ]]
	echo "  output-list is empty"
	print_ok
}

test_attach_ipc() {
	local display=$1
	echo "Attaching to $display"
	$WAYVNCCTL attach "$display" &>/dev/null
	echo "  attach IPC succeeded"
	print_ok
}

test_detach_ipc() {
	echo "Checking detach command"
	$WAYVNCCTL detach &>/dev/null
	echo "  detach IPC succeeded"
	print_ok
}

detached_test() {
	test_setup "detached test"
	start_wayvncctl_events
	start_wayvnc -D
	test_version_ipc
	wait_until verify_events \
		wayvnc-startup
	test_output_list_empty

	start_sway
	test_attach_ipc "$WAYLAND_DISPLAY"
	wait_until test_output_list_ipc HEADLESS-1

	test_detach_ipc
	wait_until verify_events \
		capture-changed \
		detached
	test_output_list_empty

	# Reattach, then remove the last output, and make sure we detach
	test_attach_ipc "$WAYLAND_DISPLAY"
	wait_until test_output_list_ipc HEADLESS-1
	wait_until verify_events \
		capture-changed
	start_lingering_client
	sway_output_destroy HEADLESS-1
	wait_until verify_events \
		output-removed \
		detached
	test_output_list_empty
	stop_lingering_client
	wait_until verify_events \
		client-disconnected

	test_exit_ipc
	wait_until verify_events \
		wayvnc-shutdown
	stop_wayvncctl_events
	stop_sway
}

WAYVNC_CONFIG=$XDG_RUNTIME_DIR/wayvnc.config
verify_wayvnc_fails_with() {
	local expected_error=$1
	shift
	echo "Starting wayvnc, expecting failure..."
	WAYVNC_LOG=$XDG_RUNTIME_DIR/wayvnc.log
	local status=0
	timeout 5 $WAYVNC -D -C "$WAYVNC_CONFIG" -L debug \
		"$WAYVNC_ADDRESS" "$WAYVNC_PORT" &>"$WAYVNC_LOG" || status=$?
	echo "  Exit status: $status=~1"
	[[ $status -eq 1 ]]
	echo "  Error: $expected_error"
	grep -qF "$expected_error" "$WAYVNC_LOG"
	print_ok
}

bad_config_syntax_test() {
	test_setup "bad config syntax test"
	cat >"$WAYVNC_CONFIG" <<-EOF
		address=127.0.0.1

		this line has no delimiter
	EOF
	verify_wayvnc_fails_with "Failed to load config. Error on line 3"
}

bad_config_rsa_key_path_test() {
	test_setup "bad config RSA key path test"
	cat >"$WAYVNC_CONFIG" <<-EOF
		enable_auth=true
		password=secret
		rsa_private_key_file=$XDG_RUNTIME_DIR/nonexistent/rsa_key.pem
	EOF
	verify_wayvnc_fails_with "Failed to load RSA credentials"
}

bad_config_tls_paths_test() {
	test_setup "bad config TLS paths test"
	cat >"$WAYVNC_CONFIG" <<-EOF
		enable_auth=true
		password=secret
		private_key_file=$XDG_RUNTIME_DIR/nonexistent/key.pem
		certificate_file=$XDG_RUNTIME_DIR/nonexistent/cert.pem
	EOF
	verify_wayvnc_fails_with "Failed to enable TLS authentication"
}

smoke_test test_output_list_ipc
smoke_test true --desktop
multioutput_test
detached_test
bad_config_syntax_test
bad_config_rsa_key_path_test
bad_config_tls_paths_test
