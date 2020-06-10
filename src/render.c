/*
 * Copyright (c) 2019 - 2020 Andri Yngvason
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
#include "output.h"
#include "config.h"
#include "usdt.h"

#define MAYBE_UNUSED __attribute__((unused))

#define SHADER_PATH PREFIX "/share/wayvnc/shaders"

enum {
	ATTR_INDEX_POS = 0,
	ATTR_INDEX_TEXTURE,
};

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

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

static const float transforms[][4] = {
	[WL_OUTPUT_TRANSFORM_NORMAL] = {
		1.0f, 0.0f,
		0.0f, 1.0f,
	},
	[WL_OUTPUT_TRANSFORM_90] = {
		0.0f, 1.0f,
		-1.0f, 0.0f,
	},
	[WL_OUTPUT_TRANSFORM_180] = {
		-1.0f, 0.0f,
		0.0f, -1.0f,
	},
	[WL_OUTPUT_TRANSFORM_270] = {
		0.0f, -1.0f,
		1.0f, 0.0f,
	},
	[WL_OUTPUT_TRANSFORM_FLIPPED] = {
		-1.0f, 0.0f,
		0.0f, 1.0f,
	},
	[WL_OUTPUT_TRANSFORM_FLIPPED_90] = {
		0.0f, 1.0f,
		1.0f, 0.0f,
	},
	[WL_OUTPUT_TRANSFORM_FLIPPED_180] = {
		1.0f, 0.0f,
		0.0f, -1.0f,
	},
	[WL_OUTPUT_TRANSFORM_FLIPPED_270] = {
		0.0f, -1.0f,
		-1.0f, 0.0f,
	},
};

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

static void gl_shader_log(GLuint shader)
{
	GLint len = 0;
	glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);

	char* log = malloc(len);
	assert(log);

	glGetShaderInfoLog(shader, len, &len, log);

	fwrite(log, 1, len, stderr);

	free(log);
}

static int gl_load_shader(GLuint* dst, const char* path, const char* source,
                          GLenum type)
{
	GLuint shader = glCreateShader(type);

	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);

	GLint is_compiled = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &is_compiled);

	if (!is_compiled) {
		log_error("Failed to compile shader: %s\n", path);
		gl_shader_log(shader);
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

static char* read_file_at_path(const char* prefix, const char* file)
{
	char path[256];

	snprintf(path, sizeof(path), "%s/%s", prefix, file);
	path[sizeof(path) - 1] = '\0';

	return read_file(path);
}

static int gl_load_shader_from_file(GLuint* dst, const char* file, GLenum type)
{
	char* source = read_file_at_path("shaders", file);
	if (!source)
		source = read_file_at_path(SHADER_PATH, file);

	if (!source)
		return -1;

	int rc = gl_load_shader(dst, file, source, type);

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

	glBindAttribLocation(program, ATTR_INDEX_POS, "pos");
	glBindAttribLocation(program, ATTR_INDEX_TEXTURE, "texture");

	glLinkProgram(program);

	glDeleteShader(vertex);
	glDeleteShader(fragment);

	GLint is_linked = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &is_linked);
	if (!is_linked) {
		log_error("Failed to link shaders %s and %s\n", vertex_path,
		          fragment_path);
		gl_shader_log(program);
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

void gl_draw(void)
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

	glVertexAttribPointer(ATTR_INDEX_POS, 2, GL_FLOAT, GL_FALSE, 0,
			      s_vertices);
	glVertexAttribPointer(ATTR_INDEX_TEXTURE, 2, GL_FLOAT, GL_FALSE, 0,
			      s_positions);

	glEnableVertexAttribArray(ATTR_INDEX_POS);
	glEnableVertexAttribArray(ATTR_INDEX_TEXTURE);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDisableVertexAttribArray(ATTR_INDEX_TEXTURE);
	glDisableVertexAttribArray(ATTR_INDEX_POS);
}

void renderer_swap(struct renderer* self)
{
	self->frame_index ^= 1;
}

void render_frame(struct renderer* self)
{
	renderer_swap(self);

	glBindFramebuffer(GL_FRAMEBUFFER, self->frame_fbo[self->frame_index].fbo);

	glUseProgram(self->frame_shader.program);

	glUniform1i(self->frame_shader.u_tex0, 0);

	const float* proj = transforms[self->output->transform];
	glUniformMatrix2fv(self->frame_shader.u_proj, 1, GL_FALSE, proj);

	gl_draw();

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void render_damage(struct renderer* self)
{
	glBindFramebuffer(GL_FRAMEBUFFER, self->damage_fbo.fbo);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, self->frame_fbo[self->frame_index].tex);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, self->frame_fbo[!self->frame_index].tex);

	glUseProgram(self->damage_shader.program);

	glUniform1i(self->damage_shader.u_tex0, 0);
	glUniform1i(self->damage_shader.u_tex1, 1);

	gl_draw();

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void destroy_fbo(struct renderer_fbo* fbo)
{
	glDeleteFramebuffers(1, &fbo->fbo);
	glDeleteRenderbuffers(1, &fbo->rbo);
	if (fbo->tex)
		glDeleteTextures(1, &fbo->tex);
}

int create_fbo(struct renderer_fbo* dst, GLint format, uint32_t width,
               uint32_t height)
{
	GLuint rbo = 0;
	glGenRenderbuffers(1, &rbo);
	glBindRenderbuffer(GL_RENDERBUFFER, rbo);
	glRenderbufferStorage(GL_RENDERBUFFER, format, width, height);
	glBindRenderbuffer(GL_RENDERBUFFER, 0);

	GLuint fbo = 0;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                          GL_RENDERBUFFER, rbo);
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	if (status != GL_FRAMEBUFFER_COMPLETE) {
		log_error("Framebuffer incomplete\n");
		return -1;
	}

	dst->fbo = fbo;
	dst->rbo = rbo;
	dst->tex = 0;

	return 0;
}

int create_textured_fbo(struct renderer_fbo* dst, GLint format, uint32_t width,
                        uint32_t height)
{
	GLuint tex = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, format,
	             GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glBindTexture(GL_TEXTURE_2D, 0);

	GLuint fbo = 0;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                       GL_TEXTURE_2D, tex, 0);
	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	if (status != GL_FRAMEBUFFER_COMPLETE) {
		log_error("Framebuffer incomplete\n");
		return -1;
	}

	dst->fbo = fbo;
	dst->rbo = 0;
	dst->tex = tex;

	return 0;
}

void renderer_destroy(struct renderer* self)
{
	glDeleteProgram(self->frame_shader.program);
	eglMakeCurrent(self->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
		       EGL_NO_CONTEXT);
	destroy_fbo(&self->frame_fbo[1]);
	destroy_fbo(&self->frame_fbo[0]);
	eglDestroyContext(self->display, self->context);
	eglTerminate(self->display);
}

int renderer_init(struct renderer* self, const struct output* output,
                  enum renderer_input_type input_type)
{
	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		log_error("Failed to bind EGL API\n");
		return -1;
	}

	if (gl_load_early_extensions() < 0) {
		log_error("Failed to load early GL extensions\n");
		return -1;
	}

	gl_debug_init();

	self->display =
		eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA,
					 EGL_DEFAULT_DISPLAY, NULL);
	if (!self->display) {
		log_error("Failed to get EGL display\n");
		return -1;
	}

	if (!eglInitialize(self->display, NULL, NULL)) {
		log_error("Failed to get initialize EGL\n");
		return -1;
	}

	static const EGLint cfg_attr[] = {
		EGL_SURFACE_TYPE, 0,
		EGL_ALPHA_SIZE, 8,
		EGL_BLUE_SIZE, 8,
		EGL_GREEN_SIZE, 8,
		EGL_RED_SIZE, 8,
		EGL_NONE
	};

	EGLConfig cfg;
	EGLint cfg_count;

	if (!eglChooseConfig(self->display, cfg_attr, &cfg, 1, &cfg_count)) {
		log_error("Could not choose EGL config\n");
		return -1;
	}

	static const EGLint ctx_attr[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};

	self->context = eglCreateContext(self->display, cfg, EGL_NO_CONTEXT,
					 ctx_attr);
	if (!self->context) {
		log_error("Failed to create EGL context\n");
		return -1;
	}

	if (!eglMakeCurrent(self->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
			    self->context)) {
		log_error("Failed to make EGL context current\n");
		goto make_current_failure;
	}

	log_debug("%s\n", glGetString(GL_VERSION));

	if (gl_load_late_extensions() < 0) {
		log_error("Failed to load late GL extensions\n");
		goto late_extension_failure;
	}

	uint32_t tf_width = output_get_transformed_width(output);
	uint32_t tf_height = output_get_transformed_height(output);

	if (create_textured_fbo(&self->frame_fbo[0], GL_RGBA, tf_width,
	                        tf_height) < 0) {
		log_error("Failed to create frame FBO 0\n");
		goto frame_fbo_failure_0;
	}

	if (create_textured_fbo(&self->frame_fbo[1], GL_RGBA, tf_width,
	                        tf_height) < 0) {
		log_error("Failed to create frame FBO 1\n");
		goto frame_fbo_failure_1;
	}

	if (create_fbo(&self->damage_fbo, GL_R8_EXT, tf_width, tf_height) < 0) {
		log_error("Failed to create damage FBO\n");
		goto damage_fbo_failure;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, self->frame_fbo[0].fbo);

	switch (input_type) {
	case RENDERER_INPUT_DMABUF:
		if (gl_compile_shader_program(&self->frame_shader.program,
					      "dmabuf-vertex.glsl",
					      "dmabuf-fragment.glsl") < 0)
			goto frame_shader_failure;
		break;
	case RENDERER_INPUT_FB:
		if (gl_compile_shader_program(&self->frame_shader.program,
					      "texture-vertex.glsl",
					      "texture-fragment.glsl") < 0)
			goto frame_shader_failure;
		break;
	}

	if (gl_compile_shader_program(&self->damage_shader.program,
				      "damage-vertex.glsl",
				      "damage-fragment.glsl") < 0)
		goto damage_shader_failure;

	self->frame_shader.u_tex0 =
		glGetUniformLocation(self->frame_shader.program, "u_tex0");
	self->frame_shader.u_proj =
		glGetUniformLocation(self->frame_shader.program, "u_proj");

	self->damage_shader.u_tex0 =
		glGetUniformLocation(self->damage_shader.program, "u_tex0");
	self->damage_shader.u_tex1 =
		glGetUniformLocation(self->damage_shader.program, "u_tex1");

	self->output = output;
	glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &self->read_format);
	glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &self->read_type);

	glViewport(0, 0,
	           output_get_transformed_width(output),
	           output_get_transformed_height(output));

	gl_clear();

	return 0;

damage_shader_failure:
	glDeleteShader(self->frame_shader.program);
frame_shader_failure:
	destroy_fbo(&self->damage_fbo);
damage_fbo_failure:
	destroy_fbo(&self->frame_fbo[1]);
frame_fbo_failure_1:
	destroy_fbo(&self->frame_fbo[0]);
frame_fbo_failure_0:
late_extension_failure:
make_current_failure:
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

int render_dmabuf(struct renderer* self, struct dmabuf_frame* frame)
{
	DTRACE_PROBE1(wayvnc, render_dmabuf_start, self);

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

	GLuint tex = 0;
	glGenTextures(1, &tex);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);

	glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, image);
	eglDestroyImageKHR(self->display, image);

	render_frame(self);
	glFinish();

	glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
	glDeleteTextures(1, &tex);

	DTRACE_PROBE(wayvnc, render_dmabuf_end);
	return 0;
}

int render_framebuffer(struct renderer* self, const void* addr, uint32_t format,
                       uint32_t width, uint32_t height, uint32_t stride)
{
	DTRACE_PROBE1(wayvnc, render_framebuffer_start, self);

	GLuint tex = 0;
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

	render_frame(self);

	glBindTexture(GL_TEXTURE_2D, 0);
	glDeleteTextures(1, &tex);

	DTRACE_PROBE(wayvnc, render_framebuffer_end);
	return 0;
}

void renderer_read_pixels(struct renderer* self, void* dst, uint32_t y,
                          uint32_t height)
{
	assert(y + height <= output_get_transformed_height(self->output));

	uint32_t width = output_get_transformed_width(self->output);

	GLint read_format, read_type;
	glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT, &read_format);
	glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE, &read_type);

	glFinish();

	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, y, width, height, read_format, read_type, dst);
}

void renderer_read_frame(struct renderer* self, void* dst, uint32_t y,
                         uint32_t height)
{
	DTRACE_PROBE3(wayvnc, render_read_frame_start, self, y, height);
	glBindFramebuffer(GL_FRAMEBUFFER, self->frame_fbo[self->frame_index].fbo);
	renderer_read_pixels(self, dst, y, height);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	DTRACE_PROBE(wayvnc, render_read_frame_end);
}

void renderer_read_damage(struct renderer* self, void* dst, uint32_t y,
                          uint32_t height)
{
	DTRACE_PROBE1(wayvnc, render_read_damage_start, self);
	glBindFramebuffer(GL_FRAMEBUFFER, self->damage_fbo.fbo);
	renderer_read_pixels(self, dst, y, height);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	DTRACE_PROBE(wayvnc, render_read_damage_end);
}
