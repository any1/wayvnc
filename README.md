# wayvnc

## Introduction
This is a VNC server for wlroots based Wayland compositors. It attaches to a
running Wayland session, creates virtual input devices and exposes a single
display via the RFB protocol. The Wayland session may be a headless one, so it
is also possible to run wayvnc without a physical display attached.

## Building
### Runtime Dependencies
 * EGL
 * libuv
 * libxkbcommon
 * neatvnc
 * OpenGL ES V2.0
 * pixman

### Build Dependencies
 * GCC
 * meson
 * ninja
 * pkg-config

The easiest way to satisfy the neatvnc dependency is to clone it into the
subprojects directory:
```
mkdir subprojects
git clone https://github.com/any1/neatvnc.git subprojects/neatvnc
```

Setting the buildtype flag is not required but it is recommended as there are
significant performance gains to be had from an optimised build.
```
meson build --buildtype=release
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

:warning: Do not do this on a public network or the internet. Wayvnc does not
support any kind of encryption or password protection. A good way to protect
your VNC connection is to use SSH tunneling while listening on localhost.
