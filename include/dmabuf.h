#pragma once

#include <stdint.h>
#include <stdbool.h>

struct zwlr_export_dmabuf_manager_v1;
struct zwlr_export_dmabuf_frame_v1;
struct wl_output;

enum dmabuf_capture_status {
        DMABUF_CAPTURE_UNSPEC = 0,
        DMABUF_CAPTURE_CANCELLED,
        DMABUF_CAPTURE_FATAL,
        DMABUF_CAPTURE_DONE
};

struct dmabuf_plane {
	int fd;
	uint32_t offset;
	uint32_t size;
	uint32_t pitch;
	uint64_t modifier;
};

struct dmabuf_frame {
	uint32_t width;
	uint32_t height;
	uint32_t format;

	uint32_t n_planes;
	struct dmabuf_plane plane[4];
};

struct dmabuf_capture {
	struct zwlr_export_dmabuf_frame_v1* zwlr_frame;
	struct dmabuf_frame frame;

        enum dmabuf_capture_status status;
        void (*on_done)(struct dmabuf_capture*);
};

struct dmabuf_capture*
dmabuf_capture_start(struct zwlr_export_dmabuf_manager_v1* manager,
		     bool overlay_cursor, struct wl_output* output,
		     void (*on_done)(struct dmabuf_capture*));
void dmabuf_capture_stop(struct dmabuf_capture* self);
