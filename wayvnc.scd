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

If the Wayland session consists of  multiple outputs, only one will be captured.
By default this will be the first one, but can be specified by the _-o_ command
line argument. The argument accepts the short name such as _eDP-1_ or _DP-4_.
Running wayvnc in verbose mode (_-v_) will display the names of all outputs on
startup.

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

# ENVIRONMENT

The following environment variables have an effect on wayvnc:

_WAYLAND_DISPLAY_
	Specifies the name of the Wayland display that the compositor to which
	wayvnc shall bind is running on.

_XDG_CONFIG_HOME_
	Specifies the location of configuration files.

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
