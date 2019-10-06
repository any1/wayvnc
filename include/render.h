#pragma once

struct dmabuf_frame;

int render_dmabuf_frame(struct renderer* self, struct dmabuf_frame* frame);

/* Copy a horizontal stripe from the GL frame into a pixel buffer */
void render_copy_pixels(struct renderer* self, void* dst, uint32_t y,
                        uint32_t height);
