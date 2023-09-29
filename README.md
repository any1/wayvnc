# wayvnc

[![Build and Unit Test](https://github.com/any1/wayvnc/actions/workflows/build.yml/badge.svg)](https://github.com/any1/wayvnc/actions/workflows/build.yml)
[![builds.sr.ht status](https://builds.sr.ht/~andri/wayvnc/commits/master.svg)](https://builds.sr.ht/~andri/wayvnc/commits/master?)
[![Packaging status](https://repology.org/badge/tiny-repos/wayvnc.svg)](https://repology.org/project/wayvnc/versions)

## Introduction
This is a VNC server for wlroots-based Wayland compositors (:no_entry: Gnome,
KDE and Weston are **not** supported). It attaches to a running Wayland session,
creates virtual input devices, and exposes a single display via the RFB
protocol. The Wayland session may be a headless one, so it is also possible
to run wayvnc without a physical display attached.

Please check the [FAQ](FAQ.md) for answers to common questions. For further
support, join the #wayvnc IRC channel on libera.chat, or ask your questions on the
GitHub [discussion forum](https://github.com/any1/wayvnc/discussions) for the
project.

## Building
### Runtime Dependencies
 * aml
 * drm
 * gbm (optional)
 * libxkbcommon
 * neatvnc
 * pam (optional)
 * pixman
 * jansson

### Build Dependencies
 * GCC
 * meson
 * ninja
 * pkg-config

#### For Arch Linux
```
pacman -S base-devel libglvnd libxkbcommon pixman gnutls jansson
```

#### For Fedora 37
```
dnf install -y meson gcc ninja-build pkg-config egl-wayland egl-wayland-devel \
	mesa-libEGL-devel mesa-libEGL libwayland-egl libglvnd-devel \
	libglvnd-core-devel libglvnd mesa-libGLES-devel mesa-libGLES \
	libxkbcommon-devel libxkbcommon libwayland-client \
	pam-devel pixman-devel libgbm-devel libdrm-devel scdoc \
	libavcodec-free-devel libavfilter-free-devel libavutil-free-devel \
	turbojpeg-devel	wayland-devel gnutls-devel jansson-devel
```

#### For Debian (unstable / testing)
```
apt build-dep wayvnc
```

#### For Ubuntu
```
apt install meson libdrm-dev libxkbcommon-dev libwlroots-dev libjansson-dev \
	libpam0g-dev libgnutls28-dev libavfilter-dev libavcodec-dev \
	libavutil-dev libturbojpeg0-dev scdoc
```

#### Additional build-time dependencies

The easiest way to satisfy the neatvnc and aml dependencies is to link to them
in the subprojects directory:
```
git clone https://github.com/any1/wayvnc.git
git clone https://github.com/any1/neatvnc.git
git clone https://github.com/any1/aml.git

mkdir wayvnc/subprojects
cd wayvnc/subprojects
ln -s ../../neatvnc .
ln -s ../../aml .
cd -

mkdir neatvnc/subprojects
cd neatvnc/subprojects
ln -s ../../aml .
cd -
```

### Configure and Build
```
meson build
ninja -C build
```

To run the unit tests:
```
meson test -C build
```

To run the [integration tests](test/integration/README.md):
```
./test/integration/integration.sh
```

## Running
Wayvnc can be run from the build directory like so:
```
./build/wayvnc
```

:radioactive: The server only accepts connections from localhost by default. To
accept connections via any interface, set the address to `0.0.0.0` like this:
```
./build/wayvnc 0.0.0.0
```

:warning: Do not do this on a public network or the internet without
user authentication enabled. The best way to protect your VNC connection is to
use SSH tunneling while listening on localhost, but users can also be
authenticated when connecting to wayvnc.

### Encryption & Authentication

#### VeNCrypt (TLS)
For TLS, you'll need a private X509 key and a certificate. A self-signed key
with a certificate can be generated like so:
```
openssl req -x509 -newkey rsa:4096 -sha256 -days 3650 -nodes \
	-keyout key.pem -out cert.pem -subj /CN=localhost \
	-addext subjectAltName=DNS:localhost,DNS:localhost,IP:127.0.0.1
```
Replace `localhost` and `127.0.0.1` in the command above with your public facing
host name and IP address, respectively, or just keep them as is if you're
testing locally.

Create a config with the authentication info and load it using the `--config`
command line option or place it at the default location
`$HOME/.config/wayvnc/config`.
```
address=0.0.0.0
enable_auth=true
username=luser
password=p455w0rd
private_key_file=/path/to/key.pem
certificate_file=/path/to/cert.pem
```

#### RSA-AES
The RSA-AES security type combines RSA with AES in EAX mode to provide secure
authentication and encryption that's resilient to eavesdropping and MITM. Its
main weakness is that the user has to verify the server's credentials on first
use. Thereafter, the client software should warn the user if the server's
credentials change. It's a Trust on First Use (TOFU) scheme as employed by SSH.

For the RSA-AES to be enabled, you need to generate an RSA key. This can be
achieved like so:
```
ssh-keygen -m pem -f ~/.config/wayvnc/rsa_key.pem -t rsa -N ""
```

You also need to tell wayvnc where this file is located, by setting setting the
`rsa_private_key_file` configuration parameter:
```
address=0.0.0.0
enable_auth=true
username=luser
password=p455w0rd
rsa_private_key_file=/path/to/rsa_key.pem
```

You may also add credentials for TLS in combination with RSA. The client will
choose.

### wayvncctl control socket

To facilitate runtime interaction and control, wayvnc opens a unix domain socket
at *$XDG_RUNTIME_DIR*/wayvncctl (or a fallback of /tmp/wayvncctl-*$UID*). A
client can connect and exchange json-formatted IPC messages to query and control
the running wayvnc instance.

Use the `wayvncctl` utility to interact with this control socket from the
command line.

See the `wayvnc(1)` manpage for an in-depth description of the IPC protocol and
the available commands, and `wayvncctl(1)` for more on the command line
interface.

There is also a handy event-loop mode that can be used to run commands when
various events occur in wayvnc. See
[examples/event-watcher](examples/event-watcher) for more details.
