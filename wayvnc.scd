wayvnc(1)

# NAME

wayvnc - A VNC server for wlroots based Wayland compositors.

# SYNOPSIS

*wayvnc* [options] [address [port]]

# OPTIONS

*-C, --config=<path>*
	Select a config file.

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

*-V, --version*
	Show version info.

*-h, --help*
	Get help.

# DESCRIPTION

This is a VNC server for wlroots based Wayland compositors. It attaches to a
running Wayland session, creates virtual input devices and exposes a single
display via the RFB protocol. The Wayland session may be a headless one, so it
is also possible to run wayvnc without a physical display attached.

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

# AUTHORS

Maintained by Andri Yngvason <andri@yngvason.is>. Up-to-date sources can be
found at https://github.com/any1/wayvnc and bugs reports or patches can be
submitted to GitHub's issue tracker.
