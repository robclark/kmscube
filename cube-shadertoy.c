/*
 * Copyright © 2020 Google, Inc.
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

#define _GNU_SOURCE

#include <assert.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <GLES3/gl3.h>

#include "common.h"
#include "esUtil.h"

static struct {
	struct egl egl;

	const struct gbm *gbm;

	/* Shadertoy rendering (to FBO): */
	GLuint stoy_program;
	GLuint stoy_fbo, stoy_fbotex;
	GLint stoy_time_loc;
	GLuint stoy_vbo;

	/* Cube rendering (textures from FBO): */
	GLfloat aspect;
	GLuint program;
	/* uniform handles: */
	GLint modelviewmatrix, modelviewprojectionmatrix, normalmatrix;
	GLint texture;
	GLuint vbo;
	GLuint positionsoffset, texcoordsoffset, normalsoffset;
	GLuint tex[2];
} gl;

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

static const GLfloat vTexCoords[] = {
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

static const char *cube_vs =
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

static const char *cube_fs =
	"precision mediump float;           \n"
	"                                   \n"
	"uniform sampler2D uTex;            \n"
	"                                   \n"
	"varying vec4 vVaryingColor;        \n"
	"varying vec2 vTexCoord;            \n"
	"                                   \n"
	"void main()                        \n"
	"{                                  \n"
	"    gl_FragColor = vVaryingColor * texture2D(uTex, vTexCoord);\n"
	"}                                  \n";

static const char *shadertoy_vs =
	"attribute vec3 position;           \n"
	"void main()                        \n"
	"{                                  \n"
	"    gl_Position = vec4(position, 1.0);\n"
	"}                                  \n";

static const char *shadertoy_fs_tmpl =
	"precision mediump float;                                                             \n"
	"uniform vec3      iResolution;           // viewport resolution (in pixels)          \n"
	"uniform float     iGlobalTime;           // shader playback time (in seconds)        \n"
	"uniform vec4      iMouse;                // mouse pixel coords                       \n"
	"uniform vec4      iDate;                 // (year, month, day, time in seconds)      \n"
	"uniform float     iSampleRate;           // sound sample rate (i.e., 44100)          \n"
	"uniform vec3      iChannelResolution[4]; // channel resolution (in pixels)           \n"
	"uniform float     iChannelTime[4];       // channel playback time (in sec)           \n"
	"uniform float     iTime;                                                             \n"
	"                                                                                     \n"
	"%s                                                                                   \n"
	"                                                                                     \n"
	"void main()                                                                          \n"
	"{                                                                                    \n"
	"    mainImage(gl_FragColor, gl_FragCoord.xy);                                        \n"
	"}                                                                                    \n";


static const uint32_t texw = 512, texh = 512;

static int load_shader(const char *file)
{
	struct stat statbuf;
	char *frag;
	int fd, ret;

	/* load src file: */
	fd = open(file, 0);
	if (fd < 0) {
		err(fd, "could not open '%s'", file);
	}

	ret = fstat(fd, &statbuf);
	if (ret < 0) {
		err(ret, "could not stat '%s'", file);
	}

	const char *text =
		mmap(NULL, statbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

	asprintf(&frag, shadertoy_fs_tmpl, text);

	return create_program(shadertoy_vs, frag);
}

static int init_shadertoy(const char *file)
{
	int ret = load_shader(file);
	gl.stoy_program = ret;

	glBindAttribLocation(gl.program, 0, "position");

	ret = link_program(gl.stoy_program);

	glUseProgram(gl.stoy_program);
	gl.stoy_time_loc = glGetUniformLocation(gl.stoy_program, "iTime");

	/* we can set iResolution a single time, it doesn't change: */
	GLint resolution_location = glGetUniformLocation(gl.stoy_program, "iResolution");
	glUniform3f(resolution_location, texw, texh, 0);

	glGenFramebuffers(1, &gl.stoy_fbo);
	glGenTextures(1, &gl.stoy_fbotex);
	glBindFramebuffer(GL_FRAMEBUFFER, gl.stoy_fbo);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, gl.stoy_fbotex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, texw, texh, 0, GL_RGB, GL_UNSIGNED_BYTE, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
		gl.stoy_fbotex, 0);

	const GLfloat vertices[] = {
		-1.0f, -1.0f, 0.0f,
		 1.0f, -1.0f, 0.0f,
		-1.0f,  1.0f, 0.0f,
		 1.0f,  1.0f, 0.0f,
	};
	glGenBuffers(1, &gl.stoy_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, gl.stoy_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), 0, GL_STATIC_DRAW);
	glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(vertices), &vertices[0]);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (const GLvoid *)(intptr_t)0);

	return 0;
}

static void draw_shadertoy(unsigned i)
{
	GLenum mrt_bufs[] = {GL_COLOR_ATTACHMENT0};

	glBindFramebuffer(GL_FRAMEBUFFER, gl.stoy_fbo);
	glViewport(0, 0, texw, texh);

	glUseProgram(gl.stoy_program);
	glUniform1f(gl.stoy_time_loc, (float)i / 60.0f);

	glBindBuffer(GL_ARRAY_BUFFER, gl.stoy_vbo);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (const GLvoid *)(intptr_t)0);
	glEnableVertexAttribArray(0);

	glDrawBuffers(1, mrt_bufs);

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	glDisableVertexAttribArray(0);

	/* switch back to back buffer: */
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void draw_cube_shadertoy(unsigned i)
{
	ESMatrix modelview;

	draw_shadertoy(i);

	glViewport(0, 0, gl.gbm->width, gl.gbm->height);
	glEnable(GL_CULL_FACE);

	/* clear the color buffer */
	glClearColor(0.5, 0.5, 0.5, 1.0);
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(gl.program);

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

	glBindBuffer(GL_ARRAY_BUFFER, gl.vbo);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (const GLvoid *)(intptr_t)gl.positionsoffset);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, (const GLvoid *)(intptr_t)gl.normalsoffset);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, (const GLvoid *)(intptr_t)gl.texcoordsoffset);
	glEnableVertexAttribArray(2);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, gl.stoy_fbotex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_R, GL_REPEAT);
	glUniform1i(gl.texture, 0); /* '0' refers to texture unit 0. */

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 4, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 8, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 12, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 16, 4);
	glDrawArrays(GL_TRIANGLE_STRIP, 20, 4);

	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(2);
}

const struct egl * init_cube_shadertoy(const struct gbm *gbm, const char *file, int samples)
{
	int ret;

	ret = init_egl(&gl.egl, gbm, samples);
	if (ret)
		return NULL;

	gl.aspect = (GLfloat)(gbm->height) / (GLfloat)(gbm->width);
	gl.gbm = gbm;

	ret = create_program(cube_vs, cube_fs);
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
	gl.texture   = glGetUniformLocation(gl.program, "uTex");

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
	glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, (const GLvoid *)(intptr_t)gl.normalsoffset);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, (const GLvoid *)(intptr_t)gl.texcoordsoffset);

	ret = init_shadertoy(file);
	if (ret) {
		printf("failed to initialize\n");
		return NULL;
	}

	gl.egl.draw = draw_cube_shadertoy;

	return &gl.egl;
}
