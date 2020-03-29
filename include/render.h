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
	GLuint tex;
};

struct renderer {
	EGLDisplay display;
	EGLContext context;

	const struct output* output;

	GLint read_format;
	GLint read_type;

	struct renderer_fbo frame_fbo[2];
	int frame_index;

	struct renderer_fbo damage_fbo;

	struct {
		GLuint program;
		GLint u_tex0;
		GLint u_proj;
	} frame_shader;

	struct {
		GLuint program;
		GLint u_tex0;
		GLint u_tex1;
		GLint u_width;
		GLint u_height;
	} damage_shader;
};

int renderer_init(struct renderer* self, const struct output* output,
                  enum renderer_input_type input_type);
void renderer_destroy(struct renderer* self);

int render_dmabuf(struct renderer* self, struct dmabuf_frame* frame);
int render_framebuffer(struct renderer* self, const void* addr, uint32_t format,
                       uint32_t width, uint32_t height, uint32_t stride);

/* Copy a horizontal stripe from the GL frame into a pixel buffer */
void renderer_read_frame(struct renderer* self, void* dst, uint32_t y,
                         uint32_t height);

void renderer_read_damage(struct renderer* self, void* dst, uint32_t y,
                          uint32_t height);

void renderer_swap_textures(struct renderer* self);

void render_damage(struct renderer* self);
