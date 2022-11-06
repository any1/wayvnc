wayvncctl(1)

# NAME

wayvncctl - A command line control lient for wayvnc(1)

# SYNOPSIS

*wayvncctl* [options] [command [--parameter value ...]]

# OPTIONS

*-S, --socket=<path>*
	Set wayvnc control socket path. Default: $XDG_RUNTIME_DIR/wayvncctl
	or /tmp/wayvncctl-$UID

*-j, --json*
	Produce json output to stdout.

*-V, --version*
	Show version info.

*-v,--verbose*
	Be more verbose.

*-h, --help*
	Get help about the wayvncctl command itself (lists these options). Does
	not connect to the wayvncctl control socket.

# DESCRIPTION

*wayvnc(1)* allows runtime interaction via a unix socket json-ipc mechanism.
This command line utility provides easy interaction with those commands.

For a full list of currently supported commands, see
*wayvnc(1)* section _IPC COMMANDS_, or run the
*wayvncctl help* command.

Running *wayvncctl help* contacts the server over the control socket and returns
a list of the available commands.

Running *wayvncctl command-name --help* returns a description of the server-side
command and its available parameters.

# EXAMPLES

Query the server for all available IPC command names:

```
$ wayvncctl help
Allowed commands:
  - help
  - version
  - set-output

Run 'wayvncctl command-name --help' for command-specific details.
```

Get help on the "set-output" IPC command:

```
$ wayvncctl set-output --help
Usage: wayvncctl [options] set-output [params]

Switch the actively captured output

Parameters:
  --switch-to=...
    The specific output name to capture

  --cycle=...
    Either "next" or "prev"

Run 'wayvncctl --help' for allowed options
```

Cycle to the next active output:

```
$ wayvncctl set-output --cycle=next
```

Get json-formatted version information:

```
$ wayvncctl --json version
{"wayvnc":"v0.5.0","neatvnc":"v0.5.1","aml":"v0.2.2"}
```

# ENVIRONMENT

The following environment variables have an effect on wayvncctl:

_XDG_RUNTIME_DIR_
	Specifies the default location for the wayvncctl control socket.

# SEE ALSO

*wayvnc(1)*
