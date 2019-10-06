#pragma once

#include <GLES2/gl2.h>
#include <EGL/egl.h>

struct dmabuf_frame;

struct renderer {
	EGLDisplay display;
	EGLSurface surface;
	EGLContext context;
	GLuint shader_program;
	uint32_t width;
	uint32_t height;
	GLint read_format;
	GLint read_type;
};

int renderer_init(struct renderer* self, uint32_t width, uint32_t height);
void renderer_destroy(struct renderer* self);

int render_dmabuf_frame(struct renderer* self, struct dmabuf_frame* frame);

/* Copy a horizontal stripe from the GL frame into a pixel buffer */
void render_copy_pixels(struct renderer* self, void* dst, uint32_t y,
                        uint32_t height);
