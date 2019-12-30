# wayvnc (Beta)

## Introduction
This is a VNC server for wlroots based Wayland compositors.

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

Wayvnc can be run from the build directory like so:
```
./build/wayvnc
```
