/*
 * Copyright (c) 2012 Arvin Schnell <arvin.schnell@gmail.com>
 * Copyright (c) 2012 Rob Clark <rob@ti.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/* Based on a egl cube test app originally written by Arvin Schnell */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>

#include "esUtil.h"
#include "cubetex.h"


/*******************/
#define DRM_IOCTL_MODE_ATOMIC_PAGE_FLIP	DRM_IOWR(0xBC, struct drm_mode_crtc_atomic_page_flip)

struct drm_mode_crtc_atomic_page_flip {
	uint32_t crtc_id;
	uint32_t flags;
	uint64_t user_data;
	uint32_t reserved;
	uint32_t count_props;
	uint64_t props_ptr;  /* ptr to array of drm_mode_obj_set_property */
};

#define VOID2U64(x) ((uint64_t)(unsigned long)(x))
/*******************/

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))


static struct {
	EGLDisplay display;
	EGLConfig config;
	EGLContext context;
	EGLSurface surface;
	GLuint program;
	GLint modelviewmatrix, modelviewprojectionmatrix, normalmatrix;
    GLint in_position, in_normal, in_color;
	GLuint vbo;
	GLuint positionsoffset, colorsoffset, normalsoffset;
} gl;

static struct {
	EGLSurface surface;
	GLuint program;
	GLuint vbo;
	GLuint positionsoffset;
	GLuint texture;
	GLint texture_location;
	GLint in_position;
	GLint modelviewmatrix;
} frame_gl;

static struct {
	struct gbm_device *dev;
	struct gbm_surface *surface, *surface2;
} gbm;

static struct {
	int fd;
	drmModeModeInfo *mode;
	uint32_t crtc_id, plane_id;
	uint32_t connector_id;
	uint32_t crtc_fb_prop, plane_fb_prop, plane_x_prop, plane_y_prop;
} drm;

struct drm_fb {
	struct gbm_bo *bo;
	uint32_t fb_id;
};

static int init_drm(void)
{
	static const char *modules[] = {
			"i915", "radeon", "nouveau", "vmwgfx", "omapdrm", "exynos"
	};
	drmModeRes *resources;
	drmModeConnector *connector = NULL;
	drmModeEncoder *encoder = NULL;
	drmModePlaneRes *plane_resources;
	drmModeObjectProperties *props;
	int i, area;

	for (i = 0; i < ARRAY_SIZE(modules); i++) {
		printf("trying to load module %s...", modules[i]);
		drm.fd = drmOpen(modules[i], NULL);
		if (drm.fd < 0) {
			printf("failed.\n");
		} else {
			printf("success.\n");
			break;
		}
	}

	if (drm.fd < 0) {
		printf("could not open drm device\n");
		return -1;
	}

	resources = drmModeGetResources(drm.fd);
	if (!resources) {
		printf("drmModeGetResources failed: %s\n", strerror(errno));
		return -1;
	}

	/* find a connected connector: */
	for (i = 0; i < resources->count_connectors; i++) {
		connector = drmModeGetConnector(drm.fd, resources->connectors[i]);
		if (connector->connection == DRM_MODE_CONNECTED) {
			/* it's connected, let's use this! */
			break;
		}
		drmModeFreeConnector(connector);
		connector = NULL;
	}

	if (!connector) {
		/* we could be fancy and listen for hotplug events and wait for
		 * a connector..
		 */
		printf("no connected connector!\n");
		return -1;
	}

	/* find highest resolution mode: */
	for (i = 0, area = 0; i < connector->count_modes; i++) {
		drmModeModeInfo *current_mode = &connector->modes[i];
		int current_area = current_mode->hdisplay * current_mode->vdisplay;
		if (current_area > area) {
			drm.mode = current_mode;
			area = current_area;
		}
	}

	if (!drm.mode) {
		printf("could not find mode!\n");
		return -1;
	}

	/* find encoder: */
	for (i = 0; i < resources->count_encoders; i++) {
		encoder = drmModeGetEncoder(drm.fd, resources->encoders[i]);
		if (encoder->encoder_id == connector->encoder_id)
			break;
		drmModeFreeEncoder(encoder);
		encoder = NULL;
	}

	if (!encoder) {
		printf("no encoder!\n");
		return -1;
	}

	drm.crtc_id = encoder->crtc_id;
	drm.connector_id = connector->connector_id;

	plane_resources = drmModeGetPlaneResources(drm.fd);
	for (i = 0; i < plane_resources->count_planes; i++) {
		drmModePlane *plane =
				drmModeGetPlane(drm.fd, plane_resources->planes[i]);
		drm.plane_id = plane->plane_id;
		break;
	}

	props = drmModeObjectGetProperties(drm.fd,
			drm.crtc_id, DRM_MODE_OBJECT_CRTC);
	if (props) {
		drmModePropertyPtr prop;
		int i;
		for (i = 0; i < props->count_props; i++) {
			prop = drmModeGetProperty(drm.fd, props->props[i]);
			if (!strcmp(prop->name, "fb")) {
				drm.crtc_fb_prop = prop->prop_id;
			}
			drmModeFreeProperty(prop);
		}
		drmModeFreeObjectProperties(props);
	}

	props = drmModeObjectGetProperties(drm.fd,
			drm.plane_id, DRM_MODE_OBJECT_PLANE);
	if (props) {
		drmModePropertyPtr prop;
		int i;
		for (i = 0; i < props->count_props; i++) {
			prop = drmModeGetProperty(drm.fd, props->props[i]);
			if (!strcmp(prop->name, "fb")) {
				drm.plane_fb_prop = prop->prop_id;
			} else if (!strcmp(prop->name, "crtc_x")) {
				drm.plane_x_prop = prop->prop_id;
			} else if (!strcmp(prop->name, "crtc_y")) {
				drm.plane_y_prop = prop->prop_id;
			}
			drmModeFreeProperty(prop);
		}
		drmModeFreeObjectProperties(props);
	}

	return 0;
}

static int init_gbm(void)
{
	gbm.dev = gbm_create_device(drm.fd);

	gbm.surface = gbm_surface_create(gbm.dev,
			drm.mode->hdisplay, drm.mode->vdisplay,
			GBM_FORMAT_XRGB8888,
			GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	if (!gbm.surface) {
		printf("failed to create gbm surface\n");
		return -1;
	}

	gbm.surface2 = gbm_surface_create(gbm.dev,
			(drm.mode->hdisplay / 2) - 40,
			(drm.mode->vdisplay / 2) - 40,
			GBM_FORMAT_XRGB8888,
			GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	if (!gbm.surface2) {
		printf("failed to create gbm surface\n");
		return -1;
	}

	return 0;
}

static unsigned int make_program(const char* vertex_shader_source,
		const char* fragment_shader_source)
{
	GLint ret;
	GLuint program;
	GLuint vertex_shader, fragment_shader;
	vertex_shader = glCreateShader(GL_VERTEX_SHADER);

	glShaderSource(vertex_shader, 1, &vertex_shader_source, NULL);
	glCompileShader(vertex_shader);

	glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &ret);
	if (!ret) {
		char *log;

		printf("vertex shader compilation failed!:\n");
		glGetShaderiv(vertex_shader, GL_INFO_LOG_LENGTH, &ret);
		if (ret > 1) {
			log = malloc(ret);
			glGetShaderInfoLog(vertex_shader, ret, NULL, log);
			printf("%s", log);
		}

		return -1;
	}

	fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);

	glShaderSource(fragment_shader, 1, &fragment_shader_source, NULL);
	glCompileShader(fragment_shader);

	glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &ret);
	if (!ret) {
		char *log;

		printf("fragment shader compilation failed!:\n");
		glGetShaderiv(fragment_shader, GL_INFO_LOG_LENGTH, &ret);

		if (ret > 1) {
			log = malloc(ret);
			glGetShaderInfoLog(fragment_shader, ret, NULL, log);
			printf("%s", log);
		}

		return 0;
	}

	program = glCreateProgram();

	glAttachShader(program, vertex_shader);
	glAttachShader(program, fragment_shader);

	glLinkProgram(program);

	glGetProgramiv(program, GL_LINK_STATUS, &ret);
	if (!ret) {
		char *log;

		printf("program linking failed!:\n");
		glGetProgramiv(program, GL_INFO_LOG_LENGTH, &ret);

		if (ret > 1) {
			log = malloc(ret);
			glGetProgramInfoLog(program, ret, NULL, log);
			printf("%s", log);
		}

		return 0;
	}

	return program;
}

static int init_gl(void)
{
	EGLint major, minor, n;

	static const GLfloat frame_vertices[] = {
			-0.5, -0.5, 1.0,
			0.5, -0.5, 1.0,
			-0.5, -0.45, 1.0,
			-0.5, -0.45, 1.0,
			0.5, -0.5, 1.0,
			0.5, -0.45, 1.0,
			0.5, -0.45, 1.0,
			0.5, 0.45, 1.0,
			0.45, 0.45, 1.0,
			0.45, 0.45, 1.0,
			0.45, -0.45, 1.0,
			0.5, -0.45, 1.0,
			0.5, 0.45, 1.0,
			0.5, 0.5, 1.0,
			-0.5, 0.5, 1.0,
			-0.5, 0.5, 1.0,
			-0.5, 0.45, 1.0,
			0.5, 0.45, 1.0,
			-0.5, 0.45, 1.0,
			-0.5, -0.45, 1.0,
			-0.45, -0.45, 1.0,
			-0.5, 0.45, 1.0,
			-0.45, -0.45, 1.0,
			-0.45, 0.45, 1.0
	};

	static const GLfloat vVertices[] = {
			// front
			-1.0f, -1.0f, +1.0f, // point blue
			+1.0f, -1.0f, +1.0f, // point magenta
			-1.0f, +1.0f, +1.0f, // point cyan
			+1.0f, +1.0f, +1.0f, // point white
			// back
			+1.0f, -1.0f, -1.0f, // point red
			-1.0f, -1.0f, -1.0f, // point black
			+1.0f, +1.0f, -1.0f, // point yellow
			-1.0f, +1.0f, -1.0f, // point green
			// right
			+1.0f, -1.0f, +1.0f, // point magenta
			+1.0f, -1.0f, -1.0f, // point red
			+1.0f, +1.0f, +1.0f, // point white
			+1.0f, +1.0f, -1.0f, // point yellow
			// left
			-1.0f, -1.0f, -1.0f, // point black
			-1.0f, -1.0f, +1.0f, // point blue
			-1.0f, +1.0f, -1.0f, // point green
			-1.0f, +1.0f, +1.0f, // point cyan
			// top
			-1.0f, +1.0f, +1.0f, // point cyan
			+1.0f, +1.0f, +1.0f, // point white
			-1.0f, +1.0f, -1.0f, // point green
			+1.0f, +1.0f, -1.0f, // point yellow
			// bottom
			-1.0f, -1.0f, -1.0f, // point black
			+1.0f, -1.0f, -1.0f, // point red
			-1.0f, -1.0f, +1.0f, // point blue
			+1.0f, -1.0f, +1.0f  // point magenta
	};

	static const GLfloat vColors[] = {
			// front
			0.0f,  0.0f,  1.0f, // blue
			1.0f,  0.0f,  1.0f, // magenta
			0.0f,  1.0f,  1.0f, // cyan
			1.0f,  1.0f,  1.0f, // white
			// back
			1.0f,  0.0f,  0.0f, // red
			0.0f,  0.0f,  0.0f, // black
			1.0f,  1.0f,  0.0f, // yellow
			0.0f,  1.0f,  0.0f, // green
			// right
			1.0f,  0.0f,  1.0f, // magenta
			1.0f,  0.0f,  0.0f, // red
			1.0f,  1.0f,  1.0f, // white
			1.0f,  1.0f,  0.0f, // yellow
			// left
			0.0f,  0.0f,  0.0f, // black
			0.0f,  0.0f,  1.0f, // blue
			0.0f,  1.0f,  0.0f, // green
			0.0f,  1.0f,  1.0f, // cyan
			// top
			0.0f,  1.0f,  1.0f, // cyan
			1.0f,  1.0f,  1.0f, // white
			0.0f,  1.0f,  0.0f, // green
			1.0f,  1.0f,  0.0f, // yellow
			// bottom
			0.0f,  0.0f,  0.0f, // black
			1.0f,  0.0f,  0.0f, // red
			0.0f,  0.0f,  1.0f, // blue
			1.0f,  0.0f,  1.0f  // magenta
	};

	static const GLfloat vNormals[] = {
			// front
			+0.0f, +0.0f, +1.0f, // forward
			+0.0f, +0.0f, +1.0f, // forward
			+0.0f, +0.0f, +1.0f, // forward
			+0.0f, +0.0f, +1.0f, // forward
			// back
			+0.0f, +0.0f, -1.0f, // backbard
			+0.0f, +0.0f, -1.0f, // backbard
			+0.0f, +0.0f, -1.0f, // backbard
			+0.0f, +0.0f, -1.0f, // backbard
			// right
			+1.0f, +0.0f, +0.0f, // right
			+1.0f, +0.0f, +0.0f, // right
			+1.0f, +0.0f, +0.0f, // right
			+1.0f, +0.0f, +0.0f, // right
			// left
			-1.0f, +0.0f, +0.0f, // left
			-1.0f, +0.0f, +0.0f, // left
			-1.0f, +0.0f, +0.0f, // left
			-1.0f, +0.0f, +0.0f, // left
			// top
			+0.0f, +1.0f, +0.0f, // up
			+0.0f, +1.0f, +0.0f, // up
			+0.0f, +1.0f, +0.0f, // up
			+0.0f, +1.0f, +0.0f, // up
			// bottom
			+0.0f, -1.0f, +0.0f, // down
			+0.0f, -1.0f, +0.0f, // down
			+0.0f, -1.0f, +0.0f, // down
			+0.0f, -1.0f, +0.0f  // down
	};

	static const EGLint context_attribs[] = {
			EGL_CONTEXT_CLIENT_VERSION, 2,
			EGL_NONE
	};

	static const EGLint config_attribs[] = {
			EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
			EGL_RED_SIZE, 1,
			EGL_GREEN_SIZE, 1,
			EGL_BLUE_SIZE, 1,
			EGL_ALPHA_SIZE, 0,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
			EGL_NONE
	};

	static const char *vertex_shader_source =
			"uniform mat4 modelviewMatrix;      \n"
			"uniform mat4 modelviewprojectionMatrix;\n"
			"uniform mat3 normalMatrix;         \n"
			"                                   \n"
			"attribute vec4 in_position;        \n"
			"attribute vec3 in_normal;          \n"
			"attribute vec4 in_color;           \n"
			"\n"
			"vec4 lightSource = vec4(2.0, 2.0, 20.0, 0.0);\n"
			"                                   \n"
			"varying vec4 vVaryingColor;        \n"
			"                                   \n"
			"void main()                        \n"
			"{                                  \n"
			"    gl_Position = modelviewprojectionMatrix * in_position;\n"
			"    vec3 vEyeNormal = normalMatrix * in_normal;\n"
			"    vec4 vPosition4 = modelviewMatrix * in_position;\n"
			"    vec3 vPosition3 = vPosition4.xyz / vPosition4.w;\n"
			"    vec3 vLightDir = normalize(lightSource.xyz - vPosition3);\n"
			"    float diff = max(0.0, dot(vEyeNormal, vLightDir));\n"
			"    vVaryingColor = vec4(diff * in_color.rgb, 1.0);\n"
			"}                                  \n";

	static const char *fragment_shader_source =
			"precision mediump float;           \n"
			"                                   \n"
			"varying vec4 vVaryingColor;        \n"
			"                                   \n"
			"void main()                        \n"
			"{                                  \n"
			"    gl_FragColor = vVaryingColor;  \n"
			"}                                  \n";

	static const char* crate_vertex_shader_source =
			"attribute vec4 in_position;        \n"
			"                                   \n"
			"uniform mat4 modelviewMatrix;      \n"
			"                                   \n"
			"varying vec2 TextureCoord;         \n"
			"                                   \n"
			"const vec2 xlate_bias = vec2(0.5, 0.5);\n"
			"                                   \n"
			"void main()                        \n"
			"{                                  \n"
			"    TextureCoord = in_position.xy + xlate_bias;\n"
			"    gl_Position = modelviewMatrix * in_position;\n"
			"}                                  \n";
	static const char* crate_fragment_shader_source =
			"precision mediump float;           \n"
			"                                   \n"
			"uniform sampler2D CrateTexture;    \n"
			"varying vec2 TextureCoord;         \n"
			"const vec3 color_bias = vec3(0.62, 0.8, 0.25);\n"
			"                                   \n"
			"void main()                        \n"
			"{                                  \n"
			"    vec4 texel = texture2D(CrateTexture, TextureCoord);\n"
			"    gl_FragColor = vec4(texel.rgb * color_bias, 1.0);\n"
			"}                                  \n";
	GLenum texture_format = cube_texture.bytes_per_pixel == 3 ? GL_RGB : GL_RGBA;

	gl.display = eglGetDisplay(gbm.dev);

	if (!eglInitialize(gl.display, &major, &minor)) {
		printf("failed to initialize\n");
		return -1;
	}

	printf("Using display %p with EGL version %d.%d\n",
			gl.display, major, minor);

	printf("EGL Version \"%s\"\n", eglQueryString(gl.display, EGL_VERSION));
	printf("EGL Vendor \"%s\"\n", eglQueryString(gl.display, EGL_VENDOR));
	printf("EGL Extensions \"%s\"\n", eglQueryString(gl.display, EGL_EXTENSIONS));

	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		printf("failed to bind api EGL_OPENGL_ES_API\n");
		return -1;
	}

	if (!eglChooseConfig(gl.display, config_attribs, &gl.config, 1, &n) || n != 1) {
		printf("failed to choose config: %d\n", n);
		return -1;
	}

	gl.context = eglCreateContext(gl.display, gl.config,
			EGL_NO_CONTEXT, context_attribs);
	if (gl.context == NULL) {
		printf("failed to create context\n");
		return -1;
	}

	gl.surface = eglCreateWindowSurface(gl.display, gl.config, gbm.surface, NULL);
	if (gl.surface == EGL_NO_SURFACE) {
		printf("failed to create egl surface\n");
		return -1;
	}

	frame_gl.surface = eglCreateWindowSurface(gl.display, gl.config, gbm.surface2, NULL);
	if (frame_gl.surface == EGL_NO_SURFACE) {
		printf("failed to create egl surface\n");
		return -1;
	}

	/* connect the context to the surface */
	eglMakeCurrent(gl.display, gl.surface, gl.surface, gl.context);

	/* Set up the main program for rendering the color cube */
	gl.program = make_program(vertex_shader_source, fragment_shader_source);
	if (gl.program == 0) {
		printf("Failed to create cube program\n");
		return -1;
	}
	gl.in_position = glGetAttribLocation(gl.program, "in_position");
	gl.in_normal = glGetAttribLocation(gl.program, "in_normal");
	gl.in_color = glGetAttribLocation(gl.program, "in_color");
	gl.modelviewmatrix = glGetUniformLocation(gl.program, "modelviewMatrix");
	gl.modelviewprojectionmatrix = glGetUniformLocation(gl.program, "modelviewprojectionMatrix");
	gl.normalmatrix = glGetUniformLocation(gl.program, "normalMatrix");

	/* Set up the program for rendering the textured frame */
	frame_gl.program = make_program(crate_vertex_shader_source, crate_fragment_shader_source);
	if (frame_gl.program == 0) {
		printf("Failed to create frame program\n");
		return -1;
	}
	frame_gl.in_position = glGetAttribLocation(frame_gl.program, "in_position");
	frame_gl.texture_location = glGetUniformLocation(frame_gl.program, "CrateTexture");
	frame_gl.modelviewmatrix = glGetUniformLocation(frame_gl.program, "modelviewMatrix");

	/* Set up the VBO for the color cube */
	gl.positionsoffset = 0;
	gl.colorsoffset = sizeof(vVertices);
	gl.normalsoffset = sizeof(vVertices) + sizeof(vColors);
	glGenBuffers(1, &gl.vbo);
	glBindBuffer(GL_ARRAY_BUFFER, gl.vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vVertices) + sizeof(vColors) + sizeof(vNormals), 0, GL_STATIC_DRAW);
	glBufferSubData(GL_ARRAY_BUFFER, gl.positionsoffset, sizeof(vVertices), &vVertices[0]);
	glBufferSubData(GL_ARRAY_BUFFER, gl.colorsoffset, sizeof(vColors), &vColors[0]);
	glBufferSubData(GL_ARRAY_BUFFER, gl.normalsoffset, sizeof(vNormals), &vNormals[0]);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	/* Set up the texture and VBO for the frame */
	glGenTextures(1, &frame_gl.texture);
	glBindTexture(GL_TEXTURE_2D, frame_gl.texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, texture_format, cube_texture.width,
			cube_texture.height, 0, texture_format, GL_UNSIGNED_BYTE,
			cube_texture.pixel_data);

	frame_gl.positionsoffset = 0;
	glGenBuffers(1, &frame_gl.vbo);
	glBindBuffer(GL_ARRAY_BUFFER, frame_gl.vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(frame_vertices), &frame_vertices[0], GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	glViewport(0, 0, drm.mode->hdisplay, drm.mode->vdisplay);
	glEnable(GL_CULL_FACE);

	return 0;
}

static void draw_box(int atomic, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	/* connect the context to the surface */
	eglMakeCurrent(gl.display, gl.surface, gl.surface, gl.context);

	glViewport(0, 0, drm.mode->hdisplay, drm.mode->vdisplay);

	/* clear the color buffer */
	if (atomic)
		glClearColor(0.25, 0.25, 0.25, 1.0);
	else
		glClearColor(0.5, 0.15, 0.15, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);

	/* Set up just a modelview matrix for the frame */
	ESMatrix modelview;
	esMatrixLoadIdentity(&modelview);
	float hcenter = drm.mode->hdisplay / 2.0;
	float pixel_width = 2.0 / drm.mode->hdisplay;
	float xlatex = (x - hcenter) * pixel_width + 0.5;
	float vcenter = drm.mode->vdisplay / 2.0;
	float pixel_height = 2.0 / drm.mode->vdisplay;
	float xlatey = (vcenter - y) * pixel_height - 0.5;
	esTranslate(&modelview, xlatex, xlatey, 0.0);

	/* Load the program and draw the frame */
	glBindBuffer(GL_ARRAY_BUFFER, frame_gl.vbo);
	glEnableVertexAttribArray(frame_gl.in_position);
	glVertexAttribPointer(frame_gl.in_position, 3, GL_FLOAT, GL_FALSE, 0, (const GLvoid*)frame_gl.positionsoffset);
	glUseProgram(frame_gl.program);
	glUniformMatrix4fv(frame_gl.modelviewmatrix, 1, GL_FALSE, &modelview.m[0][0]);
	glDrawArrays(GL_TRIANGLES, 0, 24);

	eglSwapBuffers(gl.display, gl.surface);
}

static void draw_cube(uint32_t i, uint32_t w, uint32_t h)
{
	ESMatrix modelview;

	/* connect the context to the surface */
	eglMakeCurrent(gl.display, frame_gl.surface, frame_gl.surface, gl.context);

	glViewport(0, 0, w, h);

	/* clear the color buffer */
	glClearColor(0.5, 0.5, 0.5, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);

	esMatrixLoadIdentity(&modelview);
	esTranslate(&modelview, 0.0f, 0.0f, -8.0f);
	esRotate(&modelview, 45.0f + (0.25f * i), 1.0f, 0.0f, 0.0f);
	esRotate(&modelview, 45.0f - (0.5f * i), 0.0f, 1.0f, 0.0f);
	esRotate(&modelview, 10.0f + (0.15f * i), 0.0f, 0.0f, 1.0f);

	GLfloat aspect = (GLfloat)w / (GLfloat)h;

	ESMatrix projection;
	esMatrixLoadIdentity(&projection);
	esFrustum(&projection, -2.0f, +2.0f, -1.0f * aspect, +1.0f * aspect, 6.0f, 10.0f);

	ESMatrix modelviewprojection;
	esMatrixLoadIdentity(&modelviewprojection);
	esMatrixMultiply(&modelviewprojection, &modelview, &projection);

	float normal[9];
	normal[0] = modelview.m[0][0];
	normal[1] = modelview.m[0][1];
	normal[2] = modelview.m[0][2];
	normal[3] = modelview.m[1][0];
	normal[4] = modelview.m[1][1];
	normal[5] = modelview.m[1][2];
	normal[6] = modelview.m[2][0];
	normal[7] = modelview.m[2][1];
	normal[8] = modelview.m[2][2];

	/* Load the program and draw the color cube */
	glBindBuffer(GL_ARRAY_BUFFER, gl.vbo);
	glVertexAttribPointer(gl.in_position, 3, GL_FLOAT, GL_FALSE, 0, (const GLvoid*)gl.positionsoffset);
	glEnableVertexAttribArray(gl.in_position);
	glVertexAttribPointer(gl.in_normal, 3, GL_FLOAT, GL_FALSE, 0, (const GLvoid*)gl.normalsoffset);
	glEnableVertexAttribArray(gl.in_normal);
	glVertexAttribPointer(gl.in_color, 3, GL_FLOAT, GL_FALSE, 0, (const GLvoid*)gl.colorsoffset);
	glEnableVertexAttribArray(gl.in_color);
	glUseProgram(gl.program);
	glUniformMatrix4fv(gl.modelviewmatrix, 1, GL_FALSE, &modelview.m[0][0]);
	glUniformMatrix4fv(gl.modelviewprojectionmatrix, 1, GL_FALSE, &modelviewprojection.m[0][0]);
	glUniformMatrix3fv(gl.normalmatrix, 1, GL_FALSE, normal);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 4, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 8, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 12, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 16, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 20, 4);

	glBindBuffer(GL_ARRAY_BUFFER, 0);

	eglSwapBuffers(gl.display, frame_gl.surface);
}

static void
drm_fb_destroy_callback(struct gbm_bo *bo, void *data)
{
	struct drm_fb *fb = data;
	struct gbm_device *gbm = gbm_bo_get_device(bo);

	if (fb->fb_id)
		drmModeRmFB(drm.fd, fb->fb_id);

	free(fb);
}

static struct drm_fb * drm_fb_get_from_bo(struct gbm_bo *bo)
{
	struct drm_fb *fb = gbm_bo_get_user_data(bo);
	uint32_t width, height, stride, handle;
	int ret;

	if (fb)
		return fb;

	fb = calloc(1, sizeof *fb);
	fb->bo = bo;

	width = gbm_bo_get_width(bo);
	height = gbm_bo_get_height(bo);
	stride = gbm_bo_get_stride(bo);
	handle = gbm_bo_get_handle(bo).u32;

	ret = drmModeAddFB(drm.fd, width, height, 24, 32, stride, handle, &fb->fb_id);
	if (ret) {
		printf("failed to create fb: %s\n", strerror(errno));
		free(fb);
		return NULL;
	}

	gbm_bo_set_user_data(bo, fb, drm_fb_destroy_callback);

	return fb;
}

static void page_flip_handler(int fd, unsigned int frame,
		  unsigned int sec, unsigned int usec, void *data)
{
	int *waiting_for_flip = data;
	*waiting_for_flip = 0;
}

int main(int argc, char *argv[])
{
	fd_set fds;
	drmEventContext evctx = {
			.version = DRM_EVENT_CONTEXT_VERSION,
			.page_flip_handler = page_flip_handler,
	};
	struct gbm_bo *bo, *bo2;
	struct drm_fb *fb, *fb2;
	uint32_t i = 0;
	int ret;
	uint32_t x, y, w, h;
	int32_t x_inc, y_inc;
	int atomic = 0;

	if ((argc >= 2) && !strcmp(argv[1], "--atomic"))
		atomic = 1;

	ret = init_drm();
	if (ret) {
		printf("failed to initialize DRM\n");
		return ret;
	}

	x = 64;
	y = 64;
	w = (drm.mode->hdisplay / 2) - 40;
	h = (drm.mode->vdisplay / 2) - 40;
	x_inc = 1;
	y_inc = 2;

	FD_ZERO(&fds);
	FD_SET(0, &fds);
	FD_SET(drm.fd, &fds);

	ret = init_gbm();
	if (ret) {
		printf("failed to initialize GBM\n");
		return ret;
	}

	ret = init_gl();
	if (ret) {
		printf("failed to initialize EGL\n");
		return ret;
	}

	/* clear the color buffer */
	glClearColor(0.5, 0.5, 0.5, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);
	eglSwapBuffers(gl.display, gl.surface);
	bo = gbm_surface_lock_front_buffer(gbm.surface);
	fb = drm_fb_get_from_bo(bo);

	eglMakeCurrent(gl.display, frame_gl.surface, frame_gl.surface, gl.context);
	glClearColor(0.5, 0.5, 0.5, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);
	eglSwapBuffers(gl.display, frame_gl.surface);
	bo2 = gbm_surface_lock_front_buffer(gbm.surface2);
	fb2 = drm_fb_get_from_bo(bo2);

	/* set mode: */
	ret = drmModeSetCrtc(drm.fd, drm.crtc_id, fb->fb_id, 0, 0,
			&drm.connector_id, 1, drm.mode);
	if (ret) {
		printf("failed to set mode: %s\n", strerror(errno));
		return ret;
	}

	ret = drmModeSetPlane(drm.fd, drm.plane_id, drm.crtc_id, fb2->fb_id,
			0, x, y, w, h, 0, 0, w<<16, h<<16);
	if (ret) {
		printf("failed set plane: %s\n", strerror(errno));
		return -1;
	}

again:
	i = 0;
	printf("running %s\n", atomic ? "atomic" : "non-atomic");
	while (i < 3600) {
		struct gbm_bo *next_bo, *next_bo2;
		int waiting_for_flip = 1;

		if (((x + x_inc + w + 20) >= drm.mode->hdisplay) ||
				((x + x_inc - 20) <= 0))
			x_inc = -x_inc;

		if (((y + y_inc + h + 20) >= drm.mode->vdisplay) ||
				((y + y_inc - 20) <= 0))
			y_inc = -y_inc;

		x += x_inc;
		y += y_inc;

		draw_box(atomic, x-20, y-20, w+40, h+40);
		next_bo = gbm_surface_lock_front_buffer(gbm.surface);
		fb = drm_fb_get_from_bo(next_bo);

		draw_cube(i++, w, h);
		next_bo2 = gbm_surface_lock_front_buffer(gbm.surface2);
		fb2 = drm_fb_get_from_bo(next_bo2);

		if (atomic) {
			struct drm_mode_obj_set_property props[] = { {
					.value = fb->fb_id,
					.prop_id = drm.crtc_fb_prop,
					.obj_id  = drm.crtc_id,
					.obj_type = DRM_MODE_OBJECT_CRTC,
			}, {
					.value = fb2->fb_id,
					.prop_id = drm.plane_fb_prop,
					.obj_id  = drm.plane_id,
					.obj_type = DRM_MODE_OBJECT_PLANE,
			}, {
					.value = x,
					.prop_id = drm.plane_x_prop,
					.obj_id  = drm.plane_id,
					.obj_type = DRM_MODE_OBJECT_PLANE,
			}, {
					.value = y,
					.prop_id = drm.plane_y_prop,
					.obj_id  = drm.plane_id,
					.obj_type = DRM_MODE_OBJECT_PLANE,
			} };
			struct drm_mode_crtc_atomic_page_flip req = {
					.crtc_id = drm.crtc_id,
					.flags = DRM_MODE_PAGE_FLIP_EVENT,
					.user_data = VOID2U64(&waiting_for_flip),
					.count_props = ARRAY_SIZE(props),
					.props_ptr = VOID2U64(props),
			};
			ret = drmIoctl(drm.fd, DRM_IOCTL_MODE_ATOMIC_PAGE_FLIP, &req);
			if (ret) {
				printf("failed to queue atomic page flip: %s\n", strerror(errno));
				return -1;
			}
		} else {
			ret = drmModePageFlip(drm.fd, drm.crtc_id, fb->fb_id,
					DRM_MODE_PAGE_FLIP_EVENT, &waiting_for_flip);
			if (ret) {
				printf("failed to queue page flip: %s\n", strerror(errno));
				return -1;
			}

			ret = drmModeSetPlane(drm.fd, drm.plane_id, drm.crtc_id, fb2->fb_id,
					0, x, y, w, h, 0, 0, w<<16, h<<16);
			if (ret) {
				printf("failed to queue plane flip: %s\n", strerror(errno));
				return -1;
			}
		}

		while (waiting_for_flip) {
			ret = select(drm.fd + 1, &fds, NULL, NULL, NULL);
			if (ret < 0) {
				printf("select err: %s\n", strerror(errno));
				return ret;
			} else if (ret == 0) {
				printf("select timeout!\n");
				return -1;
			} else if (FD_ISSET(0, &fds)) {
				printf("user interrupted!\n");
				break;
			}
			drmHandleEvent(drm.fd, &evctx);
		}

		/* release last buffer to render on again: */
		gbm_surface_release_buffer(gbm.surface, bo);
		bo = next_bo;

		gbm_surface_release_buffer(gbm.surface2, bo2);
		bo2 = next_bo2;
	}
	atomic = !atomic;
	usleep(100000);
	goto again;
}
