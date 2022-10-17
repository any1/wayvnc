# wayvnc

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

## Installing
```
# Arch Linux
pacman -S wayvnc

# FreeBSD
pkg install wayvnc

# Fedora
dnf install wayvnc

# Debian (unstable / testing)
apt install wayvnc

# openSUSE Tumbleweed
zypper install wayvnc

# Void Linux
xbps-install wayvnc
```

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

#### For Fedora 31
```
dnf install -y meson gcc ninja-build pkg-config egl-wayland egl-wayland-devel \
	mesa-libEGL-devel mesa-libEGL libwayland-egl libglvnd-devel \
	libglvnd-core-devel libglvnd mesa-libGLES-devel mesa-libGLES \
	libxkbcommon-devel libxkbcommon libwayland-client libwayland \
	wayland-devel gnutls-devel jansson-devel       
```

#### For Debian (unstable / testing)
```
apt build-dep wayvnc
```
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

meson build
ninja -C build
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
You'll need a private X509 key and a certificate. A self-signed key with a
certificate can be generated like so:
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
