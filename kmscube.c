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

#include <assert.h>
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
#include <drm/drm_fourcc.h>

#define GL_GLEXT_PROTOTYPES 1
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "esUtil.h"


#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

enum {
	RGBA,          /* single-plane RGBA */
	NV12_2IMG,     /* NV12, handled as two textures and converted to RGB in shader */
	NV12_1IMG,     /* NV12, imported as planar YUV eglimg */
} mode = RGBA;

static struct {
	EGLDisplay display;
	EGLConfig config;
	EGLContext context;
	EGLSurface surface;
	GLuint program;
	/* uniform handles: */
	GLint modelviewmatrix, modelviewprojectionmatrix, normalmatrix;
	GLint texture, textureuv;
	GLuint vbo;
	GLuint positionsoffset, texcoordsoffset, normalsoffset;
	GLuint tex[2];

	PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
	PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
} gl;

static struct {
	struct gbm_device *dev;
	struct gbm_surface *surface;
} gbm;

static struct {
	int fd;
	drmModeModeInfo *mode;
	uint32_t crtc_id;
	uint32_t connector_id;
} drm;

struct drm_fb {
	struct gbm_bo *bo;
	uint32_t fb_id;
};

static uint32_t find_crtc_for_encoder(const drmModeRes *resources,
				      const drmModeEncoder *encoder) {
	int i;

	for (i = 0; i < resources->count_crtcs; i++) {
		/* possible_crtcs is a bitmask as described here:
		 * https://dvdhrm.wordpress.com/2012/09/13/linux-drm-mode-setting-api
		 */
		const uint32_t crtc_mask = 1 << i;
		const uint32_t crtc_id = resources->crtcs[i];
		if (encoder->possible_crtcs & crtc_mask) {
			return crtc_id;
		}
	}

	/* no match found */
	return -1;
}

static uint32_t find_crtc_for_connector(const drmModeRes *resources,
					const drmModeConnector *connector) {
	int i;

	for (i = 0; i < connector->count_encoders; i++) {
		const uint32_t encoder_id = connector->encoders[i];
		drmModeEncoder *encoder = drmModeGetEncoder(drm.fd, encoder_id);

		if (encoder) {
			const uint32_t crtc_id = find_crtc_for_encoder(resources, encoder);

			drmModeFreeEncoder(encoder);
			if (crtc_id != 0) {
				return crtc_id;
			}
		}
	}

	/* no match found */
	return -1;
}

static int init_drm(void)
{
	static const char *modules[] = {
			"i915", "radeon", "nouveau", "vmwgfx", "omapdrm", "exynos", "msm", "tegra", "virtio_gpu"
	};
	drmModeRes *resources;
	drmModeConnector *connector = NULL;
	drmModeEncoder *encoder = NULL;
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

	/* find prefered mode or the highest resolution mode: */
	for (i = 0, area = 0; i < connector->count_modes; i++) {
		drmModeModeInfo *current_mode = &connector->modes[i];

		if (current_mode->type & DRM_MODE_TYPE_PREFERRED) {
			drm.mode = current_mode;
		}

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

	if (encoder) {
		drm.crtc_id = encoder->crtc_id;
	} else {
		uint32_t crtc_id = find_crtc_for_connector(resources, connector);
		if (crtc_id == 0) {
			printf("no crtc found!\n");
			return -1;
		}

		drm.crtc_id = crtc_id;
	}

	drm.connector_id = connector->connector_id;

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

	return 0;
}

static int init_gl(void)
{
	EGLint major, minor, n;
	GLuint vertex_shader, fragment_shader;
	GLint ret;

	static const GLfloat vVertices[] = {
			// front
			-1.0f, -1.0f, +1.0f,
			+1.0f, -1.0f, +1.0f,
			-1.0f, +1.0f, +1.0f,
			+1.0f, +1.0f, +1.0f,
			// back
			+1.0f, -1.0f, -1.0f,
			-1.0f, -1.0f, -1.0f,
			+1.0f, +1.0f, -1.0f,
			-1.0f, +1.0f, -1.0f,
			// right
			+1.0f, -1.0f, +1.0f,
			+1.0f, -1.0f, -1.0f,
			+1.0f, +1.0f, +1.0f,
			+1.0f, +1.0f, -1.0f,
			// left
			-1.0f, -1.0f, -1.0f,
			-1.0f, -1.0f, +1.0f,
			-1.0f, +1.0f, -1.0f,
			-1.0f, +1.0f, +1.0f,
			// top
			-1.0f, +1.0f, +1.0f,
			+1.0f, +1.0f, +1.0f,
			-1.0f, +1.0f, -1.0f,
			+1.0f, +1.0f, -1.0f,
			// bottom
			-1.0f, -1.0f, -1.0f,
			+1.0f, -1.0f, -1.0f,
			-1.0f, -1.0f, +1.0f,
			+1.0f, -1.0f, +1.0f,
	};

	GLfloat vTexCoords[] = {
			//front
			1.0f, 1.0f,
			0.0f, 1.0f,
			1.0f, 0.0f,
			0.0f, 0.0f,
			//back
			1.0f, 1.0f,
			0.0f, 1.0f,
			1.0f, 0.0f,
			0.0f, 0.0f,
			//right
			1.0f, 1.0f,
			0.0f, 1.0f,
			1.0f, 0.0f,
			0.0f, 0.0f,
			//left
			1.0f, 1.0f,
			0.0f, 1.0f,
			1.0f, 0.0f,
			0.0f, 0.0f,
			//top
			1.0f, 1.0f,
			0.0f, 1.0f,
			1.0f, 0.0f,
			0.0f, 0.0f,
			//bottom
			1.0f, 0.0f,
			0.0f, 0.0f,
			1.0f, 1.0f,
			0.0f, 1.0f,
	};

	static const GLfloat vNormals[] = {
			// front
			+0.0f, +0.0f, +1.0f, // forward
			+0.0f, +0.0f, +1.0f, // forward
			+0.0f, +0.0f, +1.0f, // forward
			+0.0f, +0.0f, +1.0f, // forward
			// back
			+0.0f, +0.0f, -1.0f, // backward
			+0.0f, +0.0f, -1.0f, // backward
			+0.0f, +0.0f, -1.0f, // backward
			+0.0f, +0.0f, -1.0f, // backward
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
			"attribute vec2 in_TexCoord;        \n"
			"                                   \n"
			"vec4 lightSource = vec4(2.0, 2.0, 20.0, 0.0);\n"
			"                                   \n"
			"varying vec4 vVaryingColor;        \n"
			"varying vec2 vTexCoord;            \n"
			"                                   \n"
			"void main()                        \n"
			"{                                  \n"
			"    gl_Position = modelviewprojectionMatrix * in_position;\n"
			"    vec3 vEyeNormal = normalMatrix * in_normal;\n"
			"    vec4 vPosition4 = modelviewMatrix * in_position;\n"
			"    vec3 vPosition3 = vPosition4.xyz / vPosition4.w;\n"
			"    vec3 vLightDir = normalize(lightSource.xyz - vPosition3);\n"
			"    float diff = max(0.0, dot(vEyeNormal, vLightDir));\n"
			"    vVaryingColor = vec4(diff * vec3(1.0, 1.0, 1.0), 1.0);\n"
			"    vTexCoord = in_TexCoord; \n"
			"}                            \n";

	static const char *fragment_shader_source_1img =
			"#extension GL_OES_EGL_image_external : enable\n"
			"precision mediump float;           \n"
			"                                   \n"
			"uniform samplerExternalOES uTex;   \n"
			"                                   \n"
			"varying vec4 vVaryingColor;        \n"
			"varying vec2 vTexCoord;            \n"
			"                                   \n"
			"void main()                        \n"
			"{                                  \n"
			"    gl_FragColor = vVaryingColor * texture2D(uTex, vTexCoord);\n"
			"}                                  \n";

	static const char *fragment_shader_source_2img =
			"#extension GL_OES_EGL_image_external : enable  \n"
			"precision mediump float;                       \n"
			"                                               \n"
			"uniform samplerExternalOES uTexY;              \n"
			"uniform samplerExternalOES uTexUV;             \n"
			"                                               \n"
			"varying vec4 vVaryingColor;                    \n"
			"varying vec2 vTexCoord;                        \n"
			"                                               \n"
			"mat4 csc = mat4(1.0,  0.0,    1.402, -0.701,   \n"
			"                1.0, -0.344, -0.714,  0.529,   \n"
			"                1.0,  1.772,  0.0,   -0.886,   \n"
			"                0.0,  0.0,    0.0,    0.0);    \n"
			"                                               \n"
			"void main()                                    \n"
			"{                                              \n"
			"    vec4 yuv;                                  \n"
			"    yuv.x  = texture2D(uTexY,  vTexCoord).x;   \n"
			"    yuv.yz = texture2D(uTexUV, vTexCoord).xy;  \n"
			"    yuv.w  = 1.0;                              \n"
			"    gl_FragColor = vVaryingColor * (yuv * csc);\n"
			"}                                              \n";

	const char *fragment_shader_source = (mode == NV12_2IMG) ?
			fragment_shader_source_2img : fragment_shader_source_1img;

	gl.display = eglGetDisplay(gbm.dev);

	if (!eglInitialize(gl.display, &major, &minor)) {
		printf("failed to initialize\n");
		return -1;
	}


#define get_proc(name) do { \
		gl.name = (void *)eglGetProcAddress(#name); \
		assert(gl.name); \
	} while (0)

	get_proc(eglCreateImageKHR);
	get_proc(eglDestroyImageKHR);
	get_proc(glEGLImageTargetTexture2DOES);

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

	/* connect the context to the surface */
	eglMakeCurrent(gl.display, gl.surface, gl.surface, gl.context);

	printf("GL Extensions: \"%s\"\n", glGetString(GL_EXTENSIONS));

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

		return -1;
	}

	gl.program = glCreateProgram();

	glAttachShader(gl.program, vertex_shader);
	glAttachShader(gl.program, fragment_shader);

	glBindAttribLocation(gl.program, 0, "in_position");
	glBindAttribLocation(gl.program, 1, "in_normal");
	glBindAttribLocation(gl.program, 2, "in_color");

	glLinkProgram(gl.program);

	glGetProgramiv(gl.program, GL_LINK_STATUS, &ret);
	if (!ret) {
		char *log;

		printf("program linking failed!:\n");
		glGetProgramiv(gl.program, GL_INFO_LOG_LENGTH, &ret);

		if (ret > 1) {
			log = malloc(ret);
			glGetProgramInfoLog(gl.program, ret, NULL, log);
			printf("%s", log);
		}

		return -1;
	}

	glUseProgram(gl.program);

	gl.modelviewmatrix = glGetUniformLocation(gl.program, "modelviewMatrix");
	gl.modelviewprojectionmatrix = glGetUniformLocation(gl.program, "modelviewprojectionMatrix");
	gl.normalmatrix = glGetUniformLocation(gl.program, "normalMatrix");
	if (mode == NV12_2IMG) {
		gl.texture   = glGetUniformLocation(gl.program, "uTexY");
		gl.textureuv = glGetUniformLocation(gl.program, "uTexUV");
	} else {
		gl.texture   = glGetUniformLocation(gl.program, "uTex");
	}

	glViewport(0, 0, drm.mode->hdisplay, drm.mode->vdisplay);
	glEnable(GL_CULL_FACE);

	gl.positionsoffset = 0;
	gl.texcoordsoffset = sizeof(vVertices);
	gl.normalsoffset = sizeof(vVertices) + sizeof(vTexCoords);

	glGenBuffers(1, &gl.vbo);
	glBindBuffer(GL_ARRAY_BUFFER, gl.vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vVertices) + sizeof(vTexCoords) + sizeof(vNormals), 0, GL_STATIC_DRAW);
	glBufferSubData(GL_ARRAY_BUFFER, gl.positionsoffset, sizeof(vVertices), &vVertices[0]);
	glBufferSubData(GL_ARRAY_BUFFER, gl.texcoordsoffset, sizeof(vTexCoords), &vTexCoords[0]);
	glBufferSubData(GL_ARRAY_BUFFER, gl.normalsoffset, sizeof(vNormals), &vNormals[0]);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (const GLvoid *)(intptr_t)gl.positionsoffset);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, (const GLvoid *)(intptr_t)gl.normalsoffset);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, (const GLvoid *)(intptr_t)gl.texcoordsoffset);
	glEnableVertexAttribArray(2);

	return 0;
}

static const uint32_t texw = 512, texh = 512;

static int get_fd_rgba(uint32_t *pstride)
{
	struct gbm_bo *bo;
	void *map_data = NULL;
	uint32_t stride;
	extern const uint32_t raw_512x512_rgba[];
	uint8_t *map, *src = (uint8_t *)raw_512x512_rgba;
	int fd;

	/* NOTE: do not actually use GBM_BO_USE_WRITE since that gets us a dumb buffer: */
	bo = gbm_bo_create(gbm.dev, texw, texh, GBM_FORMAT_ARGB8888, GBM_BO_USE_LINEAR);

	map = gbm_bo_map(bo, 0, 0, texw, texh, GBM_BO_TRANSFER_WRITE, &stride, &map_data);

	for (int i = 0; i < texh; i++) {
		memcpy(&map[stride * i], &src[texw * 4 * i], texw * 4);
	}

	gbm_bo_unmap(bo, map_data);

	fd = gbm_bo_get_fd(bo);

	/* we have the fd now, no longer need the bo: */
	gbm_bo_destroy(bo);

	*pstride = stride;

	return fd;
}

static int get_fd_y(uint32_t *pstride)
{
	struct gbm_bo *bo;
	void *map_data = NULL;
	uint32_t stride;
	extern const uint32_t raw_512x512_nv12[];
	uint8_t *map, *src = (uint8_t *)raw_512x512_nv12;
	int fd;

	/* NOTE: do not actually use GBM_BO_USE_WRITE since that gets us a dumb buffer: */
	/* hmm, no R8/R8G8 gbm formats?? */
	bo = gbm_bo_create(gbm.dev, texw/4, texh, GBM_FORMAT_ARGB8888, GBM_BO_USE_LINEAR);

	map = gbm_bo_map(bo, 0, 0, texw/4, texh, GBM_BO_TRANSFER_WRITE, &stride, &map_data);

	for (int i = 0; i < texh; i++) {
		memcpy(&map[stride * i], &src[texw * i], texw);
	}

	gbm_bo_unmap(bo, map_data);

	fd = gbm_bo_get_fd(bo);

	/* we have the fd now, no longer need the bo: */
	gbm_bo_destroy(bo);

	*pstride = stride;

	return fd;
}

static int get_fd_uv(uint32_t *pstride)
{
	struct gbm_bo *bo;
	void *map_data = NULL;
	uint32_t stride;
	extern const uint32_t raw_512x512_nv12[];
	uint8_t *map, *src = &((uint8_t *)raw_512x512_nv12)[texw * texh];
	int fd;

	/* NOTE: do not actually use GBM_BO_USE_WRITE since that gets us a dumb buffer: */
	/* hmm, no R8/R8G8 gbm formats?? */
	bo = gbm_bo_create(gbm.dev, texw/2/2, texh/2, GBM_FORMAT_ARGB8888, GBM_BO_USE_LINEAR);

	map = gbm_bo_map(bo, 0, 0, texw/2/2, texh/2, GBM_BO_TRANSFER_WRITE, &stride, &map_data);

	for (int i = 0; i < texh/2; i++) {
		memcpy(&map[stride * i], &src[texw * i], texw);
	}

	gbm_bo_unmap(bo, map_data);

	fd = gbm_bo_get_fd(bo);

	/* we have the fd now, no longer need the bo: */
	gbm_bo_destroy(bo);

	*pstride = stride;

	return fd;
}

static int init_tex_rgba(void)
{
	uint32_t stride;
	int fd = get_fd_rgba(&stride);
	const EGLint attr[] = {
		EGL_WIDTH, texw,
		EGL_HEIGHT, texh,
		EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ABGR8888,
		EGL_DMA_BUF_PLANE0_FD_EXT, fd,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, stride,
		EGL_NONE
	};
	EGLImage img;

	glGenTextures(1, gl.tex);

	img = gl.eglCreateImageKHR(gl.display, EGL_NO_CONTEXT,
			EGL_LINUX_DMA_BUF_EXT, NULL, attr);
	assert(img);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, gl.tex[0]);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	gl.glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, img);

	gl.eglDestroyImageKHR(gl.display, img);

	return 0;
}

static int init_tex_nv12_2img(void)
{
	uint32_t stride_y, stride_uv;
	int fd_y = get_fd_y(&stride_y);
	int fd_uv = get_fd_uv(&stride_uv);
	const EGLint attr_y[] = {
		EGL_WIDTH, texw,
		EGL_HEIGHT, texh,
		EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_R8,
		EGL_DMA_BUF_PLANE0_FD_EXT, fd_y,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, stride_y,
		EGL_NONE
	};
	const EGLint attr_uv[] = {
		EGL_WIDTH, texw/2,
		EGL_HEIGHT, texh/2,
		EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_GR88,
		EGL_DMA_BUF_PLANE0_FD_EXT, fd_uv,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, stride_uv,
		EGL_NONE
	};
	EGLImage img_y, img_uv;

	glGenTextures(2, gl.tex);

	/* Y plane texture: */
	img_y = gl.eglCreateImageKHR(gl.display, EGL_NO_CONTEXT,
			EGL_LINUX_DMA_BUF_EXT, NULL, attr_y);
	assert(img_y);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, gl.tex[0]);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	gl.glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, img_y);

	gl.eglDestroyImageKHR(gl.display, img_y);

	/* UV plane texture: */
	img_uv = gl.eglCreateImageKHR(gl.display, EGL_NO_CONTEXT,
			EGL_LINUX_DMA_BUF_EXT, NULL, attr_uv);
	assert(img_uv);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, gl.tex[1]);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	gl.glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, img_uv);

	gl.eglDestroyImageKHR(gl.display, img_uv);

	return 0;
}

static int init_tex_nv12_1img(void)
{
	uint32_t stride_y, stride_uv;
	int fd_y = get_fd_y(&stride_y);
	int fd_uv = get_fd_uv(&stride_uv);
	const EGLint attr[] = {
		EGL_WIDTH, texw,
		EGL_HEIGHT, texh,
		EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_NV12,
		EGL_DMA_BUF_PLANE0_FD_EXT, fd_y,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, stride_y,
		EGL_DMA_BUF_PLANE1_FD_EXT, fd_uv,
		EGL_DMA_BUF_PLANE1_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE1_PITCH_EXT, stride_uv,
		EGL_NONE
	};
	EGLImage img;

	glGenTextures(1, gl.tex);

	img = gl.eglCreateImageKHR(gl.display, EGL_NO_CONTEXT,
			EGL_LINUX_DMA_BUF_EXT, NULL, attr);
	assert(img);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, gl.tex[0]);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	gl.glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, img);

	gl.eglDestroyImageKHR(gl.display, img);

	return 0;
}

static int init_tex(void)
{
	switch (mode) {
	case RGBA:
		return init_tex_rgba();
	case NV12_2IMG:
		return init_tex_nv12_2img();
	case NV12_1IMG:
		return init_tex_nv12_1img();
	}
	return -1;
}

static void draw(uint32_t i)
{
	ESMatrix modelview;

	/* clear the color buffer */
	glClearColor(0.5, 0.5, 0.5, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);

	esMatrixLoadIdentity(&modelview);
	esTranslate(&modelview, 0.0f, 0.0f, -8.0f);
	esRotate(&modelview, 45.0f + (0.25f * i), 1.0f, 0.0f, 0.0f);
	esRotate(&modelview, 45.0f - (0.5f * i), 0.0f, 1.0f, 0.0f);
	esRotate(&modelview, 10.0f + (0.15f * i), 0.0f, 0.0f, 1.0f);

	GLfloat aspect = (GLfloat)(drm.mode->vdisplay) / (GLfloat)(drm.mode->hdisplay);

	ESMatrix projection;
	esMatrixLoadIdentity(&projection);
	esFrustum(&projection, -2.8f, +2.8f, -2.8f * aspect, +2.8f * aspect, 6.0f, 10.0f);

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

	glUniformMatrix4fv(gl.modelviewmatrix, 1, GL_FALSE, &modelview.m[0][0]);
	glUniformMatrix4fv(gl.modelviewprojectionmatrix, 1, GL_FALSE, &modelviewprojection.m[0][0]);
	glUniformMatrix3fv(gl.normalmatrix, 1, GL_FALSE, normal);
	glUniform1i(gl.texture, 0); /* '0' refers to texture unit 0. */

	if (mode == NV12_2IMG)
		glUniform1i(gl.textureuv, 1);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 4, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 8, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 12, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 16, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 20, 4);
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
	struct gbm_bo *bo;
	struct drm_fb *fb;
	uint32_t i = 0;
	int ret;

	if (argc > 1) {
		if (strcmp(argv[1], "RGBA") == 0) {
			mode = RGBA;
		} else if (strcmp(argv[1], "NV12_2IMG") == 0) {
			mode = NV12_2IMG;
		} else if (strcmp(argv[1], "NV12_1IMG") == 0) {
			mode = NV12_1IMG;
		}
	}

	ret = init_drm();
	if (ret) {
		printf("failed to initialize DRM\n");
		return ret;
	}

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

	ret = init_tex();
	if (ret) {
		printf("failed to initialize EGLImage texture\n");
		return ret;
	}

	/* clear the color buffer */
	glClearColor(0.5, 0.5, 0.5, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);
	eglSwapBuffers(gl.display, gl.surface);
	bo = gbm_surface_lock_front_buffer(gbm.surface);
	fb = drm_fb_get_from_bo(bo);

	/* set mode: */
	ret = drmModeSetCrtc(drm.fd, drm.crtc_id, fb->fb_id, 0, 0,
			&drm.connector_id, 1, drm.mode);
	if (ret) {
		printf("failed to set mode: %s\n", strerror(errno));
		return ret;
	}

	while (1) {
		struct gbm_bo *next_bo;
		int waiting_for_flip = 1;

		draw(i++);

		eglSwapBuffers(gl.display, gl.surface);
		next_bo = gbm_surface_lock_front_buffer(gbm.surface);
		fb = drm_fb_get_from_bo(next_bo);

		/*
		 * Here you could also update drm plane layers if you want
		 * hw composition
		 */

		ret = drmModePageFlip(drm.fd, drm.crtc_id, fb->fb_id,
				DRM_MODE_PAGE_FLIP_EVENT, &waiting_for_flip);
		if (ret) {
			printf("failed to queue page flip: %s\n", strerror(errno));
			return -1;
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
	}

	return ret;
}
