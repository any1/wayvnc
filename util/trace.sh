#!/bin/bash

set -e

EVENTS="sdt_wayvnc:* sdt_neatvnc:*"

delete_all_events()
{
	for e in $EVENTS; do
		sudo perf probe -d "$e" || true
	done
}

add_all_events()
{
	for e in $EVENTS; do
		sudo perf probe "$e"
	done
}

sudo perf buildid-cache -a build/wayvnc
sudo perf buildid-cache -a build/subprojects/neatvnc/libneatvnc.so

delete_all_events
add_all_events

trap "sudo chown $USER:$USER perf.data*" EXIT

sudo perf record -aR -e ${EVENTS// /,}

