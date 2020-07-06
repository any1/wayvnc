#!/bin/sh

valgrind --leak-check=full \
	--show-leak-kinds=all \
	--suppressions=util/valgrind.supp \
	$@
