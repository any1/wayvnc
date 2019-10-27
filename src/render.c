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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <wayland-client.h>
#include <libdrm/drm_fourcc.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "logging.h"
#include "render.h"
#include "dmabuf.h"

#define MAYBE_UNUSED __attribute__((unused))

#define XSTR(s) STR(s)
#define STR(s) #s

#define X_GL_EARLY_EXTENSIONS \
	X(PFNEGLGETPLATFORMDISPLAYEXTPROC, eglGetPlatformDisplayEXT) \
	X(PFNEGLDEBUGMESSAGECONTROLKHRPROC, eglDebugMessageControlKHR) \
	X(PFNGLDEBUGMESSAGECALLBACKKHRPROC, glDebugMessageCallbackKHR) \

#define X_GL_LATE_EXTENSIONS \
	X(PFNEGLCREATEIMAGEKHRPROC, eglCreateImageKHR) \
	X(PFNEGLDESTROYIMAGEKHRPROC, eglDestroyImageKHR) \
	X(PFNGLEGLIMAGETARGETTEXTURE2DOESPROC, glEGLImageTargetTexture2DOES) \

#define X_GL_EXTENSIONS \
	X_GL_EARLY_EXTENSIONS \
	X_GL_LATE_EXTENSIONS \

#define X(type, name) type name;
	X_GL_EXTENSIONS
#undef X

int gl_format_from_fourcc(GLenum* result, uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		*result = GL_BGRA_EXT;
		return 0;
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		*result = GL_RGBA;
		return 0;
	}

	return -1;
}

static inline void* gl_load_single_extension(const char* name)
{
	void* ext = eglGetProcAddress(name);
	if (!ext)
		log_debug("GL: Failed to load procedure: %s\n", name);

	return ext;
}

static int gl_load_early_extensions(void)
{
#define X(type, name) \
	name = gl_load_single_extension(XSTR(name)); \
	if (!name) \
		return -1;

	X_GL_EARLY_EXTENSIONS
#undef X

	return 0;
}

static int gl_load_late_extensions(void)
{
#define X(type, name) \
	name = gl_load_single_extension(XSTR(name)); \
	if (!name) \
		return -1;

	X_GL_LATE_EXTENSIONS
#undef X

	return 0;
}

MAYBE_UNUSED
static void egl_log(EGLenum error, const char* command, EGLint msg_type,
		    EGLLabelKHR thread, EGLLabelKHR obj, const char *msg)
{
	(void)error;
	(void)msg_type;
	(void)thread;
	(void)obj;

	log_debug("EGL: %s: %s\n", command, msg);
}

MAYBE_UNUSED
static void gles2_log(GLenum src, GLenum type, GLuint id, GLenum severity,
		      GLsizei len, const GLchar *msg, const void *user)
{
	(void)src;
	(void)type;
	(void)id;
	(void)severity;
	(void)len;
	(void)user;

	log_debug("GLES2: %s\n", msg);
}

static void gl_debug_init()
{
#ifndef NDEBUG
	static const EGLAttrib debug_attribs[] = {
		EGL_DEBUG_MSG_CRITICAL_KHR, EGL_TRUE,
		EGL_DEBUG_MSG_ERROR_KHR, EGL_TRUE,
		EGL_DEBUG_MSG_WARN_KHR, EGL_TRUE,
		EGL_DEBUG_MSG_INFO_KHR, EGL_TRUE,
		EGL_NONE,
	};
	eglDebugMessageControlKHR(egl_log, debug_attribs);

	glEnable(GL_DEBUG_OUTPUT_KHR);
	glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_KHR);
	glDebugMessageCallbackKHR(gles2_log, NULL);
#endif
}

static int gl_load_shader(GLuint* dst, const char* source, GLenum type)
{
	GLuint shader = glCreateShader(type);

	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);

	if (glGetError() != GL_NO_ERROR) {
		glDeleteShader(shader);
		return -1;
	}

	*dst = shader;
	return 0;
}

static char* read_file(const char* path)
{
	FILE* stream = fopen(path, "r");
	if (!stream)
		return NULL;

	size_t size = 4096;
	size_t rsize = 0;

	char* contents = malloc(size);
	if (!contents)
		goto alloc_failure;

	while (1) {
		rsize += fread(contents + rsize, 1, size - rsize, stream);
		if (rsize < size)
			break;

		size *= 2;
		contents = realloc(contents, size);
		if (!contents)
			goto read_failure;
	}

	if (ferror(stream))
		goto read_failure;

	if (rsize == size) {
		contents = realloc(contents, size + 1);
		if (!contents)
			goto read_failure;
	}

	contents[rsize] = '\0';

	fclose(stream);
	return contents;

read_failure:
	free(contents);
alloc_failure:
	fclose(stream);
	return NULL;
}

static int gl_load_shader_from_file(GLuint* dst, const char* path, GLenum type)
{
	char* source = read_file(path);
	if (!source)
		return -1;

	int rc = gl_load_shader(dst, source, type);

	free(source);
	return rc;
}

static int gl_compile_shader_program(GLuint* dst, const char* vertex_path,
				     const char* fragment_path)
{
	int rc = -1;
	GLuint vertex, fragment;

	if (gl_load_shader_from_file(&vertex, vertex_path, GL_VERTEX_SHADER) < 0)
		return -1;

	if (gl_load_shader_from_file(&fragment, fragment_path,
				     GL_FRAGMENT_SHADER) < 0)
		goto fragment_failure;

	GLuint program = glCreateProgram();

	glAttachShader(program, vertex);
	glAttachShader(program, fragment);

	glBindAttribLocation(program, 0, "pos");
	glBindAttribLocation(program, 1, "texture");

	glLinkProgram(program);

	glDeleteShader(vertex);
	glDeleteShader(fragment);

	if (glGetError() != GL_NO_ERROR) {
		glDeleteProgram(program);
		goto program_failure;
	}

	*dst = program;
	rc = 0;
program_failure:
	glDeleteShader(fragment);
fragment_failure:
	glDeleteShader(vertex);
	return rc;
}

void gl_clear(void)
{
	glClearColor(0.0, 0.0, 0.0, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);
}

void gl_render(void)
{
	static const GLfloat s_vertices[4][2] = {
		{ -1.0, 1.0 },
		{ 1.0, 1.0 },
		{ -1.0, -1.0 },
		{ 1.0, -1.0 },
	};

	static const GLfloat s_positions[4][2] = {
		{ 0, 0 },
		{ 1, 0 },
		{ 0, 1 },
		{ 1, 1 },
	};

	gl_clear();

	glVertexAttribPointer (0, 2, GL_FLOAT, GL_FALSE, 0, s_vertices);
	glVertexAttribPointer (1, 2, GL_FLOAT, GL_FALSE, 0, s_positions);

	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
}

void renderer_destroy(struct renderer* self)
{
	glDeleteProgram(self->dmabuf_shader_program);
	glDeleteProgram(self->texture_shader_program);
	eglMakeCurrent(self->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
		       EGL_NO_CONTEXT);
	eglDestroySurface(self->display, self->surface);
	eglDestroyContext(self->display, self->context);
	eglTerminate(self->display);
}

int renderer_init(struct renderer* self, uint32_t width, uint32_t height)
{
	if (!eglBindAPI(EGL_OPENGL_ES_API))
		return -1;

	if (gl_load_early_extensions() < 0)
		return -1;

	gl_debug_init();

	self->display =
		eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA,
					 EGL_DEFAULT_DISPLAY, NULL);
	if (!self->display)
		return -1;

	if (!eglInitialize(self->display, NULL, NULL))
		return -1;

	static const EGLint cfg_attr[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_ALPHA_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_RED_SIZE, 8,
		EGL_NONE
	};

	EGLConfig cfg;
	EGLint cfg_count;

	if (!eglChooseConfig(self->display, cfg_attr, &cfg, 1, &cfg_count))
		return -1;

	static const EGLint ctx_attr[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};

	self->context = eglCreateContext(self->display, cfg, EGL_NO_CONTEXT,
					 ctx_attr);
	if (!self->context)
		return -1;

	EGLint surf_attr[] = {
		EGL_WIDTH, width,
		EGL_HEIGHT, height,
		EGL_NONE
	};

	self->surface = eglCreatePbufferSurface(self->display, cfg, surf_attr);
	if (!self->surface)
		goto surface_failure;

	if (!eglMakeCurrent(self->display, self->surface, self->surface,
			    self->context))
		goto make_current_failure;

	log_debug("%s\n", glGetString(GL_VERSION));

	if (gl_load_late_extensions() < 0)
		goto late_extension_failure;

	if (gl_compile_shader_program(&self->dmabuf_shader_program,
				      "shaders/dmabuf-vertex.glsl",
				      "shaders/dmabuf-fragment.glsl") < 0)
		goto shader_failure;

	if (gl_compile_shader_program(&self->texture_shader_program,
				      "shaders/texture-vertex.glsl",
				      "shaders/texture-fragment.glsl") < 0)
		goto shader_failure;

	self->width = width;
	self->height = height;
	glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &self->read_format);
	glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &self->read_type);

	glViewport(0, 0, width, height);
	gl_clear();

	return 0;

shader_failure:
late_extension_failure:
make_current_failure:
	eglDestroySurface(self->display, self->surface);
surface_failure:
	eglDestroyContext(self->display, self->context);
	return -1;
}

static inline void append_attr(EGLint* dst, int* i, EGLint name, EGLint value)
{
	dst[*i] = name;
	i[0] += 1;
	dst[*i] = value;
	i[0] += 1;
}

static void dmabuf_attr_append_planes(EGLint* dst, int* i,
				      struct dmabuf_frame* frame)
{
#define APPEND_PLANE_ATTR(n) \
	if (frame->n_planes <= n) \
		return; \
\
	append_attr(dst, i, EGL_DMA_BUF_PLANE##n##_FD_EXT, frame->plane[n].fd); \
	append_attr(dst, i, EGL_DMA_BUF_PLANE##n##_OFFSET_EXT, frame->plane[n].offset); \
	append_attr(dst, i, EGL_DMA_BUF_PLANE##n##_PITCH_EXT, frame->plane[n].pitch); \
	append_attr(dst, i, EGL_DMA_BUF_PLANE##n##_MODIFIER_LO_EXT, frame->plane[n].modifier); \
	append_attr(dst, i, EGL_DMA_BUF_PLANE##n##_MODIFIER_HI_EXT, frame->plane[n].modifier >> 32); \

	APPEND_PLANE_ATTR(0);
	APPEND_PLANE_ATTR(1);
	APPEND_PLANE_ATTR(2);
	APPEND_PLANE_ATTR(3);
#undef APPEND_PLANE_ATTR
}

int render_dmabuf_frame(struct renderer* self, struct dmabuf_frame* frame)
{
	int index = 0;
	EGLint attr[6 + 10 * 4 + 1];

	if (frame->n_planes == 0)
		return -1;

	append_attr(attr, &index, EGL_WIDTH, frame->width);
	append_attr(attr, &index, EGL_HEIGHT, frame->height);
	append_attr(attr, &index, EGL_LINUX_DRM_FOURCC_EXT, frame->format);
	dmabuf_attr_append_planes(attr, &index, frame);
	attr[index++] = EGL_NONE;

	EGLImageKHR image =
		eglCreateImageKHR(self->display, EGL_NO_CONTEXT,
				  EGL_LINUX_DMA_BUF_EXT, NULL, attr);
	if (!image)
		return -1;

	GLuint tex;
	glGenTextures(1, &tex);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
	glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image);

	glUseProgram(self->dmabuf_shader_program);
	gl_render();

	glBindTexture(GL_TEXTURE_2D, 0);
	glDeleteTextures(1, &tex);
	eglDestroyImageKHR(self->display, image);

	return 0;
}

int render_framebuffer(struct renderer* self, const void* addr, uint32_t format,
		       uint32_t width, uint32_t height, uint32_t stride)
{
	GLuint tex;
	glGenTextures(1, &tex);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, tex);

	GLenum gl_format;
	if (gl_format_from_fourcc(&gl_format, format) < 0)
		return -1;

	glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, stride / 4);
	glTexImage2D(GL_TEXTURE_2D, 0, self->read_format, width, height, 0,
		     gl_format, GL_UNSIGNED_BYTE, addr);
	glGenerateMipmap(GL_TEXTURE_2D);
	glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);

	glUseProgram(self->texture_shader_program);
	gl_render();

	glBindTexture(GL_TEXTURE_2D, 0);
	glDeleteTextures(1, &tex);

	return 0;
}

void render_copy_pixels(struct renderer* self, void* dst, uint32_t y,
			uint32_t height)
{
	assert(y + height <= self->height);

	glReadPixels(0, y, self->width, height, self->read_format,
		    self->read_type, dst);
}
