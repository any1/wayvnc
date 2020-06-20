#pragma once

#include <pixman.h>
#include <wayland-client.h>
#include <libdrm/drm_fourcc.h>
#include <stdbool.h>

enum wl_shm_format fourcc_to_wl_shm(uint32_t in);
bool fourcc_to_pixman_fmt(pixman_format_code_t* dst, uint32_t src);
