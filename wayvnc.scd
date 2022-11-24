wayvnc(1)

# NAME

wayvnc - A VNC server for wlroots based Wayland compositors.

# SYNOPSIS

*wayvnc* [options] [address [port]]

# OPTIONS

*-C, --config=<path>*
	Select a config file.

*-g,--gpu*
	Enable features that require GPU.

*-o, --output=<name>*
	Select output to capture.

*-k, --keyboard=<layout>[-variant]*
	Select keyboard layout. The variant can be appended if needed.

*-s, --seat=<name>*
	Select seat by name.

*-S, --socket=<path>*
	Set wayvnc control socket path. Default: $XDG_RUNTIME_DIR/wayvncctl
	or /tmp/wayvncctl-$UID

*-r, --render-cursor*
	Enable overlay cursor rendering.

*-f, --max-fps=<fps>*
	Set the rate limit (default 30).

*-p, --show-performance*
	Show performance counters.

*-u, --unix-socket*
	Create a UNIX domain socket instead of TCP, treating the address as a
	path.

*-d, --disable-input*
	Disable all remote input. This allows using wayvnc without compositor
	support of virtual mouse / keyboard protocols.

*-V, --version*
	Show version info.

*-v,--verbose*
	Be more verbose. Same as setting `--log-level=info`.

*-L,--log-level*
	Set log level. The levels are: error, warning, info, debug, trace and
	quiet.

*-h, --help*
	Get help.

# DESCRIPTION

This is a VNC server for wlroots based Wayland compositors. It attaches to a
running Wayland session, creates virtual input devices and exposes a single
display via the RFB protocol. The Wayland session may be a headless one, so it
is also possible to run wayvnc without a physical display attached.

## MULTIPLE OUTPUTS

If the Wayland session consists of multiple outputs, only one will be captured.
By default this will be the first one, but can be specified by the _-o_ command
line argument. The argument accepts the short name such as _eDP-1_ or _DP-4_.
Running wayvnc in verbose mode (_-v_) will display the names of all outputs on
startup, or you can query them at runtime via the *wayvncctl get-outputs*
command.

You can also change which output is being captured on the fly via the *wayvncctl
set-output* command.

# CONFIGURATION

wayvnc searches for a config file in the location
	~/$XDG_CONFIG_HOME/wayvnc/config
or if $XDG_CONFIG_HOME is not set
	~/.config/wayvnc/config

## SYNTAX

The configuration file is composed of key-value pairs separated with an *equal*
sign. Whitespace around either the key or the value is insignificant and is not
considered to be part of the key or the value.

## KEYWORDS

*address*
	The address to which the server shall bind, e.g. 0.0.0.0 or localhost.

*certificate_file*
	The path to the certificate file for encryption. Only applicable when
	*enable_auth*=true.

*enable_auth*
	Enable authentication and encryption. Setting this value to *true*
	requires also setting *certificate_file*, *private_key_file*,
	*username* and *password*.

*password*
	Choose a password for authentication.

*port*
	The port to which the server shall bind. Default is 5900.

*private_key_file*
	The path to the private key file for encryption. Only applicable when
	*enable_auth*=true.

*username*
	Choose a username for authentication.

*xkb_layout*
	The keyboard layout to use for key code lookup.

	Default: _XKB_DEFAULT_LAYOUT_ or system default.

*xkb_model*
	The keyboard model by which to interpret keycodes and LEDs.

	Default: "pc105"

*xkb_options*
	A comma separated list of options, through which the user specifies
	non-layout related preferences such as which key is the Compose key. 

	Default: _XKB_DEFAULT_OPTIONS_ or system default.

*xkb_rules*
	The rules file describes how to interpret the values of the model,
	layout, variant and options fields.

	Default: _XKB_DEFAULT_RULES_ or system default.

*xkb_variant*
	The keyboard variant to use for keycode lookup.

	Default: _XKB_DEFAULT_VARIANT_ or system default.

## EXAMPLE

```
address=0.0.0.0
enable_auth=true
username=luser
password=p455w0rd
private_key_file=/path/to/key.pem
certificate_file=/path/to/cert.pem
```

# WAYVNCCTL CONTROL SOCKET

To facilitate runtime interaction and control, wayvnc opens a unix domain socket
at *$XDG_RUNTIME_DIR*/wayvncctl (or a fallback of /tmp/wayvncctl-*$UID*). A
client can connect and exchange json-formatted IPC messages to query and control
the running wayvnc instance.

## IPC COMMANDS

_HELP_

The *help* command, when issued without any parameters, lists the names of all
available commands.

If an optional *command* parameter refers to one of those commands by name, the
response data will be a detailed description of that command and its parameters.

_VERSION_

The *version* command queries the running wayvnc instance for its version
information. Much like the _-V_ option, the response data will contain the
version numbers of wayvnc, as well as the versions of the neatvnc and aml
components.

_EVENT-RECEIVE_

The *event-receive* command registers for asynchronous server events. See the
_EVENTS_ section below for details on the event message format, and the _IPC
EVENTS_ section below for a description of all possible server events.

Event registration registers for all available server events and is scoped to
the current connection only. If a client disconnects and reconnects, it must
re-register for events.

_SET-OUTPUT_

For multi-output wayland displays, this command switches which output is
actively captured by wayvnc. This operates in 2 different modes, depending on
which parameters are supplied:

*cycle=next|prev*
	Cycle to the next/prev output in the output list, wrapping back to the
	first/last if the end of the list is reached.

*switch-to=output-name*
	Switch to a specific output by name.

_DISCONNECT_CLIENT_

The *disconnect-client* command disconnects a single VNC client.

Parameters:

*id*
	Required: The ID of the client to disconnect.  This ID can be found from the
	_GET-CLIENTS_ command or receipt of a _CLIENT-CONNECTED_ event.

_GET-CLIENTS_

The *get-clients* command retrieves a list of all VNC clients currently
connected to wayvnc.

_GET-OUTPUTS_

The *get-outputs* command retrieves a list of all outputs known to wayvnc and
whether or not each one is currently being captured.

## IPC EVENTS

_CLIENT-CONNECTED_

The *client-connected* event is sent when a new VNC client connects to wayvnc.

Parameters:

*id=...*
	A unique identifier for this client.

*connection_count=...*
	The total number of connected VNC clients including this one.

*hostname=...*
	The hostname or IP of this client. May be null.

*username=...*
	The username used to authenticate this client. May be null.

_CLIENT-DISCONNECTED_

The *client-disconnected* event is sent when a VNC cliwnt disconnects from
wayvnc.

Parameters:

*id=...*
	A unique identifier for this client.

*connection_count=...*
	The total number of connected VNC clients not including this one.

*hostname=...*
	The hostname or IP of this client. May be null.

*username=...*
	The username used to authenticate this client. May be null.

## IPC MESSAGE FORMAT

The *wayvncctl(1)* command line utility will construct properly-formatted json
ipc messages, but any client will work. The client initiates the connection and
sends one or more request objects, each of which will receive a corresponding
response object.

*Note* This message format is unstable and may change substantially over the
next few releases.

_REQUEST_

The general form of a json-ipc request message
is:

```
{
	"method": "command-name",
	"id": 123,
	"params": {
		"key1": "value1",
		"key2": "value2",
	}
}
```

The *method* is the name of the command to be executed. Use the *help* method to
query a list of all valid method names.

The *id* field is optional and may be any valid json number or string. If
provided, the response to this request will contain the identical id value,
which the client may use to coordinate multiple requests and responses.

The *params* object supplies optional parameters on a per-method basis, and may
be omitted if empty.

_RESPONSE_

```
{
	"id": 123,
	"code": 0,
	"data": {
		...
	}
}
```

If the request had an id, the response will have an identical value for *id*.

The numerical *code* provides an indication of how the request was handled. A
value of *0* always signifies success. Any other value means failure, and varies
depending on the method in question.

The *data* object contains method-specific return data. This may be structured
data in response to a query, a simple error string in the case of a failed
request, or it may be omitted entirely if the error code alone is sufficient.

_EVENTS_

Events are aaynchronous messages sent from a server to all registered clients.
The message format is identical to a _REQUEST_, but without an "id" field, and a
client must not send a response.

Example event message:

```
{
	"method": "event-name",
	"params": {
		"key1": "value1",
		"key2": "value2",
	}
}
```

In order to receive any events, a client must first register to receive them by
sending a _event-receive_ request IPC. Once the success response has been sent
by the server, the client must expect that asynchronous event messages may be
sent by the server at any time, even between a request and the associated
response.

# ENVIRONMENT

The following environment variables have an effect on wayvnc:

_WAYLAND_DISPLAY_
	Specifies the name of the Wayland display that the compositor to which
	wayvnc shall bind is running on.

_XDG_CONFIG_HOME_
	Specifies the location of configuration files.

_XDG_RUNTIME_DIR_
	Specifies the default location for the wayvncctl control socket.

# FAQ

*Wayvnc complains that a protocol is not supported*

	The error might look like this:
	```
	wl_registry@2: error 0: invalid version for global zxdg_output_manager_v1 (4): have 2, wanted 3
	ERROR: ../src/main.c: 388: Screencopy protocol not supported by compositor. Exiting. Refer to FAQ section in man page.
	ERROR: ../src/main.c: 1024: Failed to initialise wayland
	```

	This means that your wayland compositor does not implement the
	screencopy protocol and wayvnc won't work with it. Screencopy is
	implemented by wlroots based compositors such as Sway and Wayfire.

*How can I run wayvnc in headless mode/over an SSH session?*

	Set the environment variables _WLR_BACKENDS_=headless and
	_WLR_LIBINPUT_NO_DEVICES_=1 before starting the compositor, then run
	wayvnc as normal.

*How can I pass my mod-key from Sway to the remote desktop session?*

	Create an almost empty mode in your sway config. Example:
	```
	mode passthrough {
		bindsym $mod+Pause mode default
	}
	bindsym $mod+Pause mode passthrough
	```
	This makes it so that when you press $mod+Pause, all keybindings, except
	the one to switch back, are disabled.

*Not all symbols show up when I'm typing. What can I do to fix this?*

	Try setting the keyboard layout in wayvnc to the one that most closely
	matches the keyboard layout that you're using on the client side. An
	exact layout isn't needed, just one that has all the symbols that you
	use.

*How do I enable the Compose key?*

	Set "xkb_options=compose:menu" in the config file. Any key that is not
	otherwise used will work. There just needs to be some key for wayvnc to
	match against.

# AUTHORS

Maintained by Andri Yngvason <andri@yngvason.is>. Up-to-date sources can be
found at https://github.com/any1/wayvnc and bugs reports or patches can be
submitted to GitHub's issue tracker.

# SEE ALSO

*wayvncctl(1)*
