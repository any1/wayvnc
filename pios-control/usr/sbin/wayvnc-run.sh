#!/bin/sh

. /etc/default/keyboard

export XDG_RUNTIME_DIR=/run/wayvnc

export XKB_DEFAULT_MODEL="$XKBMODEL"
export XKB_DEFAULT_LAYOUT="$XKBLAYOUT"

SELF_PID=$$

{
	while ! wayvncctl --socket="$XDG_RUNTIME_DIR/wayvncctl.sock" version >/dev/null 2>&1; do
		sleep 0.1
	done
	systemd-notify --ready --pid=$SELF_PID
} &

if ! raspi-config nonint is_pifive ; then
	export WAYVNC_CMA=/dev/dma_heap/linux,cma
fi

wayvnc --detached \
	--gpu \
	--config /etc/wayvnc/config \
	--socket "$XDG_RUNTIME_DIR/wayvncctl.sock"
