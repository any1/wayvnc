wayvncctl(1)

# NAME

wayvncctl - A control client for wayvnc(1)

# SYNOPSIS

*wayvncctl* [options] [command [--parameter value ...]]

# OPTIONS

*-S, --socket=<path>*
	Set wayvnc control socket path. Default: $XDG_RUNTIME_DIR/wayvncctl
	or /tmp/wayvncctl-$UID

*-V, --version*
	Show version info.

*-v,--verbose*
	Be more verbose.

*-h, --help*
	Get help.

# DESCRIPTION

*wayvnc(1)* allows runtime interaction via a unix socket json-ipc mechanism.
This command line utility provides easy interaction with those commands.

For a full list of currently supported commands, see
*wayvnc(1)* section _IPC COMMANDS_, or use the
*wayvncctl help* command.

# EXAMPLES

Query the server for all available command names:

```
$ wayvncctl help
{
    "commands": [
        "help",
        "version",
        "set-output",
	...
    ]
}
```

Get help on the "help" command:

```
$ wayvncctl help --command help
{
    "help": {
        "description": "List all commands, or show usage of a specific command",
        "params": [
            {
                "command": "The command to show (optional)"
            }
        ]
    }
}
```

Cycle to the next active output:

```
$ wayvncctl set-output --cycle=forward
```

# ENVIRONMENT

The following environment variables have an effect on wayvncctl:

_XDG_RUNTIME_DIR_
	Specifies the default location for the wayvncctl control socket.

# SEE ALSO

*wayvnc(1)*
