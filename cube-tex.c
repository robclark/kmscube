/*
 * Copyright (c) 2017 Rob Clark <rclark@redhat.com>
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

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "common.h"
#include "esUtil.h"

static struct {
	struct egl egl;

	GLfloat aspect;
	enum mode mode;
	const struct gbm *gbm;

	GLuint program;
	/* uniform handles: */
	GLint modelviewmatrix, modelviewprojectionmatrix, normalmatrix;
	GLint texture, textureuv;
	GLuint vbo;
	GLuint positionsoffset, texcoordsoffset, normalsoffset;
	GLuint tex[2];
} gl;

const struct egl *egl = &gl.egl;

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

static const uint32_t texw = 512, texh = 512;

WEAK uint64_t
gbm_bo_get_modifier(struct gbm_bo *bo);

static int get_fd_rgba(uint32_t *pstride, uint64_t *modifier)
{
	struct gbm_bo *bo;
	void *map_data = NULL;
	uint32_t stride;
	extern const uint32_t raw_512x512_rgba[];
	uint8_t *map, *src = (uint8_t *)raw_512x512_rgba;
	int fd;

	/* NOTE: do not actually use GBM_BO_USE_WRITE since that gets us a dumb buffer: */
	bo = gbm_bo_create(gl.gbm->dev, texw, texh, GBM_FORMAT_ABGR8888, GBM_BO_USE_LINEAR);

	map = gbm_bo_map(bo, 0, 0, texw, texh, GBM_BO_TRANSFER_WRITE, &stride, &map_data);

	for (uint32_t i = 0; i < texh; i++) {
		memcpy(&map[stride * i], &src[texw * 4 * i], texw * 4);
	}

	gbm_bo_unmap(bo, map_data);

	fd = gbm_bo_get_fd(bo);

	if (gbm_bo_get_modifier)
		*modifier = gbm_bo_get_modifier(bo);
	else
		*modifier = DRM_FORMAT_MOD_LINEAR;

	/* we have the fd now, no longer need the bo: */
	gbm_bo_destroy(bo);

	*pstride = stride;

	return fd;
}

static int get_fd_y(uint32_t *pstride, uint64_t *modifier)
{
	struct gbm_bo *bo;
	void *map_data = NULL;
	uint32_t stride;
	extern const uint32_t raw_512x512_nv12[];
	uint8_t *map, *src = (uint8_t *)raw_512x512_nv12;
	int fd;

	/* NOTE: do not actually use GBM_BO_USE_WRITE since that gets us a dumb buffer: */
	bo = gbm_bo_create(gl.gbm->dev, texw, texh, GBM_FORMAT_R8, GBM_BO_USE_LINEAR);

	map = gbm_bo_map(bo, 0, 0, texw, texh, GBM_BO_TRANSFER_WRITE, &stride, &map_data);

	for (uint32_t i = 0; i < texh; i++) {
		memcpy(&map[stride * i], &src[texw * i], texw);
	}

	gbm_bo_unmap(bo, map_data);

	fd = gbm_bo_get_fd(bo);

	if (gbm_bo_get_modifier)
		*modifier = gbm_bo_get_modifier(bo);
	else
		*modifier = DRM_FORMAT_MOD_LINEAR;

	/* we have the fd now, no longer need the bo: */
	gbm_bo_destroy(bo);

	*pstride = stride;

	return fd;
}

static int get_fd_uv(uint32_t *pstride, uint64_t *modifier)
{
	struct gbm_bo *bo;
	void *map_data = NULL;
	uint32_t stride;
	extern const uint32_t raw_512x512_nv12[];
	uint8_t *map, *src = &((uint8_t *)raw_512x512_nv12)[texw * texh];
	int fd;

	/* NOTE: do not actually use GBM_BO_USE_WRITE since that gets us a dumb buffer: */
	bo = gbm_bo_create(gl.gbm->dev, texw/2, texh/2, GBM_FORMAT_GR88, GBM_BO_USE_LINEAR);

	map = gbm_bo_map(bo, 0, 0, texw/2, texh/2, GBM_BO_TRANSFER_WRITE, &stride, &map_data);

	for (uint32_t i = 0; i < texh/2; i++) {
		memcpy(&map[stride * i], &src[texw * i], texw);
	}

	gbm_bo_unmap(bo, map_data);

	fd = gbm_bo_get_fd(bo);

	if (gbm_bo_get_modifier)
		*modifier = gbm_bo_get_modifier(bo);
	else
		*modifier = DRM_FORMAT_MOD_LINEAR;

	/* we have the fd now, no longer need the bo: */
	gbm_bo_destroy(bo);

	*pstride = stride;

	return fd;
}

static int init_tex_rgba(void)
{
	uint32_t stride;
	uint64_t modifier;
	int fd = get_fd_rgba(&stride, &modifier);
	EGLint attr[] = {
		EGL_WIDTH, texw,
		EGL_HEIGHT, texh,
		EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ABGR8888,
		EGL_DMA_BUF_PLANE0_FD_EXT, fd,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, stride,
		EGL_NONE, EGL_NONE,	/* modifier lo */
		EGL_NONE, EGL_NONE,	/* modifier hi */
		EGL_NONE
	};

	if (egl->modifiers_supported &&
	    modifier != DRM_FORMAT_MOD_INVALID) {
		unsigned size =  ARRAY_SIZE(attr);
		attr[size - 5] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
		attr[size - 4] = modifier & 0xFFFFFFFF;
		attr[size - 3] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
		attr[size - 2] = modifier >> 32;
	}
	EGLImage img;

	glGenTextures(1, gl.tex);

	img = egl->eglCreateImageKHR(egl->display, EGL_NO_CONTEXT,
			EGL_LINUX_DMA_BUF_EXT, NULL, attr);
	assert(img);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, gl.tex[0]);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	egl->glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, img);

	egl->eglDestroyImageKHR(egl->display, img);
	close(fd);

	return 0;
}

static int init_tex_nv12_2img(void)
{
	uint32_t stride_y, stride_uv;
	uint64_t modifier_y, modifier_uv;
	int fd_y = get_fd_y(&stride_y, &modifier_y);
	int fd_uv = get_fd_uv(&stride_uv, &modifier_uv);
	EGLint attr_y[] = {
		EGL_WIDTH, texw,
		EGL_HEIGHT, texh,
		EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_R8,
		EGL_DMA_BUF_PLANE0_FD_EXT, fd_y,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, stride_y,
		EGL_NONE, EGL_NONE,	/* modifier lo */
		EGL_NONE, EGL_NONE,	/* modifier hi */
		EGL_NONE
	};
	EGLint attr_uv[] = {
		EGL_WIDTH, texw/2,
		EGL_HEIGHT, texh/2,
		EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_GR88,
		EGL_DMA_BUF_PLANE0_FD_EXT, fd_uv,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, stride_uv,
		EGL_NONE, EGL_NONE,	/* modifier lo */
		EGL_NONE, EGL_NONE,	/* modifier hi */
		EGL_NONE
	};

	if (egl->modifiers_supported &&
	    modifier_y != DRM_FORMAT_MOD_INVALID &&
	    modifier_uv != DRM_FORMAT_MOD_INVALID) {
		unsigned size = ARRAY_SIZE(attr_y);
		attr_y[size - 5] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
		attr_y[size - 4] = modifier_y & 0xFFFFFFFF;
		attr_y[size - 3] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
		attr_y[size - 2] = modifier_y >> 32;

		size = ARRAY_SIZE(attr_uv);
		attr_uv[size - 5] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
		attr_uv[size - 4] = modifier_uv & 0xFFFFFFFF;
		attr_uv[size - 3] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
		attr_uv[size - 2] = modifier_uv >> 32;
	}

	EGLImage img_y, img_uv;

	glGenTextures(2, gl.tex);

	/* Y plane texture: */
	img_y = egl->eglCreateImageKHR(egl->display, EGL_NO_CONTEXT,
			EGL_LINUX_DMA_BUF_EXT, NULL, attr_y);
	assert(img_y);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, gl.tex[0]);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	egl->glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, img_y);

	egl->eglDestroyImageKHR(egl->display, img_y);
	close(fd_y);

	/* UV plane texture: */
	img_uv = egl->eglCreateImageKHR(egl->display, EGL_NO_CONTEXT,
			EGL_LINUX_DMA_BUF_EXT, NULL, attr_uv);
	assert(img_uv);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, gl.tex[1]);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	egl->glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, img_uv);

	egl->eglDestroyImageKHR(egl->display, img_uv);
	close(fd_uv);

	return 0;
}

static int init_tex_nv12_1img(void)
{
	uint32_t stride_y, stride_uv;
	uint64_t modifier_y, modifier_uv;
	int fd_y = get_fd_y(&stride_y, &modifier_y);
	int fd_uv = get_fd_uv(&stride_uv, &modifier_uv);
	EGLint attr[] = {
		EGL_WIDTH, texw,
		EGL_HEIGHT, texh,
		EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_NV12,
		EGL_DMA_BUF_PLANE0_FD_EXT, fd_y,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, stride_y,
		EGL_DMA_BUF_PLANE1_FD_EXT, fd_uv,
		EGL_DMA_BUF_PLANE1_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE1_PITCH_EXT, stride_uv,
		EGL_NONE, EGL_NONE,	/* modifier lo */
		EGL_NONE, EGL_NONE,	/* modifier hi */
		EGL_NONE, EGL_NONE,	/* modifier lo */
		EGL_NONE, EGL_NONE,	/* modifier hi */
		EGL_NONE
	};
	EGLImage img;

	if (egl->modifiers_supported &&
	    modifier_y != DRM_FORMAT_MOD_INVALID &&
	    modifier_uv != DRM_FORMAT_MOD_INVALID) {
		unsigned size = ARRAY_SIZE(attr);
		attr[size - 9] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
		attr[size - 8] = modifier_y & 0xFFFFFFFF;
		attr[size - 7] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
		attr[size - 6] = modifier_y >> 32;
		attr[size - 5] = EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT;
		attr[size - 4] = modifier_uv & 0xFFFFFFFF;
		attr[size - 3] = EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT;
		attr[size - 2] = modifier_uv >> 32;
	}

	glGenTextures(1, gl.tex);

	img = egl->eglCreateImageKHR(egl->display, EGL_NO_CONTEXT,
			EGL_LINUX_DMA_BUF_EXT, NULL, attr);
	assert(img);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_EXTERNAL_OES, gl.tex[0]);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	egl->glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, img);

	egl->eglDestroyImageKHR(egl->display, img);
	close(fd_y);
	close(fd_uv);

	return 0;
}

static int init_tex(enum mode mode)
{
	switch (mode) {
	case RGBA:
		return init_tex_rgba();
	case NV12_2IMG:
		return init_tex_nv12_2img();
	case NV12_1IMG:
		return init_tex_nv12_1img();
	default:
		assert(!"unreachable");
		return -1;
	}
	return -1;
}

static void draw_cube_tex(unsigned i)
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

	ESMatrix projection;
	esMatrixLoadIdentity(&projection);
	esFrustum(&projection, -2.8f, +2.8f, -2.8f * gl.aspect, +2.8f * gl.aspect, 6.0f, 10.0f);

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

	if (gl.mode == NV12_2IMG)
		glUniform1i(gl.textureuv, 1);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 4, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 8, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 12, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 16, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 20, 4);
}

const struct egl * init_cube_tex(const struct gbm *gbm, enum mode mode, int samples)
{
	const char *fragment_shader_source = (mode == NV12_2IMG) ?
			fragment_shader_source_2img : fragment_shader_source_1img;
	int ret;

	ret = init_egl(&gl.egl, gbm, samples);
	if (ret)
		return NULL;

	if (egl_check(&gl.egl, eglCreateImageKHR) ||
	    egl_check(&gl.egl, glEGLImageTargetTexture2DOES) ||
	    egl_check(&gl.egl, eglDestroyImageKHR))
		return NULL;

	gl.aspect = (GLfloat)(gbm->height) / (GLfloat)(gbm->width);
	gl.mode = mode;
	gl.gbm = gbm;

	ret = create_program(vertex_shader_source, fragment_shader_source);
	if (ret < 0)
		return NULL;

	gl.program = ret;

	glBindAttribLocation(gl.program, 0, "in_position");
	glBindAttribLocation(gl.program, 1, "in_normal");
	glBindAttribLocation(gl.program, 2, "in_color");

	ret = link_program(gl.program);
	if (ret)
		return NULL;

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

	glViewport(0, 0, gbm->width, gbm->height);
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

	ret = init_tex(mode);
	if (ret) {
		printf("failed to initialize EGLImage texture\n");
		return NULL;
	}

	gl.egl.draw = draw_cube_tex;

	return &gl.egl;
}
