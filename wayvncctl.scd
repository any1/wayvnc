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

This command is largely self-documenting:

- Running *wayvncctl --help* lists all supported IPC commands.
- Running *wayvncctl command-name --help* returns a description of the given
  command and its available parameters.
- Running *wayvncctl event-receive --help* includes a list of all supported event
  names.
- Running *wayvncctl event-receive --show=event-name* returns a
  description of the given event and expected data fields.

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

## SPECIAL LOCAL EVENT TYPES

Especially useful when using _--wait_ or _--reconnect_ mode, wayvncctl will
generate 2 additional events not documented in *wayvnc(1)*:

*wayvnc-startup*
	Sent when a successful wayvnc control connection is established and
	event registration has succeeded, both upon initial startup and on
	subsequent registrations with _--reconnect_.

	No paramerers.

*wayvnc-shutdown*
	Sent when the wayvnc control connection is dropped, usually due to
	wayvnc exiting.

	No paramerers.

# EXAMPLES

Get help on the "output-set" IPC command:

```
$ wayvncctl output-set --help
Usage: wayvncctl [options] output-set <output-name> [params]
...
```

Cycle to the next active output:

```
$ wayvncctl output-cycle
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
    echo "Total clients: $1"
}

while IFS= read -r EVT; do
    case "$(jq -r '.method' <<<"$EVT")" in
        client-*onnected)
            count=$(jq -r '.params.connection_count' <<<"$EVT")
            connection_count_now "$count"
            ;;
        wayvnc-shutdown)
            connection_count_now 0
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
