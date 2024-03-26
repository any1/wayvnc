#!/bin/sh

. /etc/default/keyboard

export XDG_RUNTIME_DIR=/tmp/wayvnc
mkdir -p "$XDG_RUNTIME_DIR"

export XKB_DEFAULT_MODEL="$XKBMODEL"
export XKB_DEFAULT_LAYOUT="$XKBLAYOUT"

SELF_PID=$$

{
	while ! wayvncctl --socket=/tmp/wayvnc/wayvncctl.sock version >/dev/null 2>&1; do
		sleep 0.1
	done
	systemd-notify --ready --pid=$SELF_PID
} &

wayvnc --render-cursor \
	--detached \
	--gpu \
	--config /etc/wayvnc/config \
	--socket /tmp/wayvnc/wayvncctl.sock
