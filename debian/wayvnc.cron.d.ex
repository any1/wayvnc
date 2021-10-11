#
# Regular cron jobs for the wayvnc package
#
0 4	* * *	root	[ -x /usr/bin/wayvnc_maintenance ] && /usr/bin/wayvnc_maintenance
