#!/bin/bash

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"

valgrind --leak-check=full \
	--show-leak-kinds=all \
	--suppressions=$SCRIPT_DIR/valgrind.supp \
	$@
