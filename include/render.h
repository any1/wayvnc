/*
 * Copyright (c) 2019 Andri Yngvason
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#pragma once

#include <GLES2/gl2.h>
#include <EGL/egl.h>
#include <unistd.h>
#include <wayland-client.h>

struct dmabuf_frame;
struct output;

enum renderer_input_type {
	RENDERER_INPUT_FB,
	RENDERER_INPUT_DMABUF,
};

struct renderer_fbo {
	GLuint rbo;
	GLuint fbo;
};

struct renderer {
	EGLDisplay display;
	EGLContext context;

	const struct output* output;

	GLint read_format;
	GLint read_type;

	struct renderer_fbo fbo;

	GLuint tex[2];
	int tex_index;

	struct {
		GLuint program;
		GLint u_tex;
		GLint u_proj;
	} shader;
};

int renderer_init(struct renderer* self, const struct output* output,
                  enum renderer_input_type input_type);
void renderer_destroy(struct renderer* self);

int renderer_import_dmabuf_frame(struct renderer* self, GLuint tex,
                                 struct dmabuf_frame* frame);
int renderer_import_framebuffer(struct renderer* self, GLuint tex,
                                const void* addr, uint32_t format,
                                uint32_t width, uint32_t height,
                                uint32_t stride);

/* Copy a horizontal stripe from the GL frame into a pixel buffer */
void render_copy_pixels(struct renderer* self, void* dst, uint32_t y,
                        uint32_t height);

GLuint renderer_next_tex(struct renderer* self);
GLuint renderer_last_tex(const struct renderer* self);

void render(void);
