wayvncctl(1)

# NAME

wayvncctl - A command line control client for wayvnc(1)

# SYNOPSIS

*wayvncctl* [options] [command [--parameter value ...]]

# OPTIONS

*-S, --socket=<path>*
	Set wayvnc control socket path. Default: $XDG_RUNTIME_DIR/wayvncctl
	or /tmp/wayvncctl-$UID

*-w, --wait*
	Wait for wayvnc to start up if it's not already running. Default: Exit
	immediately with an error if wayvnc is not running.

*-r,--reconnect*
	If disconnected while waiting for events, wait for wayvnc to restart and
	re-register for events. Default: Exit when wayvnc exits.

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

# ASYNCHRONOUS EVENTS

While *wayvncctl* normally terminates after sending one request and receiving
the corresponding reply, the *event-receive* command acts differently. Instead
of exiting immediately, *wayvncctl* waits for any events from the server,
printing each to stdout as they arrive. This mode of operation will block until
either it receives a signal to terminate, or until the wayvnc server terminates.

In _--json_ mode, each event is printed on one line, with a newline character at
the end, for ease in scripting:

```
$ wayvncctl --json event-receive
{"method":"client-connected","params":{"id":"0x10ef670","hostname":null,"username":null,"connection_count":1}}
{"method":"client-disconnected","params":{"id":"0x10ef670","hostname":null,"username":null,"connection_count":0}}
```

The default human-readible output is a multi-line yaml-like format, with two
newline characters between each event:

```
$ wayvncctl event-receive

client-connected:
  id: 0x10ef670
  hostname: 192.168.1.18
  connection_count: 1

client-disconnected:
  id: 0x10ef670
  hostname: 192.168.1.18
  connection_count: 0

```

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

A script that takes an action for each client connect and disconnect event:

```
#!/bin/bash

connection_count_now() {
    echo "Total clients: $count"
}

while IFS= read -r EVT; do
    case "$(jq -r '.method' <<<"$EVT")" in
        client-*onnected)
            count=$(jq -r '.params.connection_count' <<<"$EVT")
            connection_count_now "$count"
            ;;
    esac
done < <(wayvncctl --wait --reconnect --json event-receive)
```

# ENVIRONMENT

The following environment variables have an effect on wayvncctl:

_XDG_RUNTIME_DIR_
	Specifies the default location for the wayvncctl control socket.

# SEE ALSO

*wayvnc(1)*
