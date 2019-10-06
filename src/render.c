#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#define MAYBE_UNUSED __attribute__((unused))

#ifdef NDEBUG
#define log_debug(...)
#else
#define log_debug(...) fprintf(stderr, __VA_ARGS__)
#endif

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

struct renderer {
	EGLDisplay display;
	EGLSurface surface;
	EGLContext context;
	GLuint shader_program;
};

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

static int gl_compile_shader_program(GLuint* dst)
{
	static const char vertex_src[] =
	"attribute vec2 pos;\n"
	"attribute vec2 texture;\n"
	"varying vec2 v_texture;\n"
	"void main() {\n"
	"    v_texture = vec2(texture.s, 1.0 - texture.t);\n"
	"    gl_Position = vec4(pos, 0, 1);\n"
	"}\n";

	static const char fragment_src[] =
	"#extension GL_OES_EGL_image_external: require\n\n"
	"precision mediump float;\n"
	"uniform samplerExternalOES u_tex;\n"
	"varying vec2 v_texture;\n"
	"void main() {\n"
	"    gl_FragColor = texture2D(u_tex, v_texture);\n"
	"}\n";

	int rc = -1;
	GLuint vertex, fragment;

	if (gl_load_shader(&vertex, vertex_src, GL_VERTEX_SHADER) < 0)
		return -1;

	if (gl_load_shader(&fragment, fragment_src, GL_FRAGMENT_SHADER) < 0)
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
	eglDestroySurface(self->display, self->surface);
	eglDestroyContext(self->display, self->context);
	glDeleteProgram(self->shader_program);
}

int renderer_init(struct renderer* self, int width, int height)
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
		EGL_ALPHA_SIZE, 0,
		EGL_BLUE_SIZE, 1,
		EGL_GREEN_SIZE, 1,
		EGL_RED_SIZE, 1,
		EGL_NONE
	};

	EGLConfig cfg;
	EGLint cfg_count;

	if (!eglChooseConfig(self->display, cfg_attr, cfg, 1, &cfg_count))
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

	if (gl_compile_shader_program(&self->shader_program) < 0)
		goto shader_failure;

	glUseProgram(self->shader_program);

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
