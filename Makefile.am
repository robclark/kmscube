#
#  Copyright (c) 2012 Rob Clark <rob@ti.com>
#
#  Permission is hereby granted, free of charge, to any person obtaining a
#  copy of this software and associated documentation files (the "Software"),
#  to deal in the Software without restriction, including without limitation
#  the rights to use, copy, modify, merge, publish, distribute, sublicense,
#  and/or sell copies of the Software, and to permit persons to whom the
#  Software is furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice (including the next
#  paragraph) shall be included in all copies or substantial portions of the
#  Software.
#
#  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
#  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
#  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
#  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
#  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
#  SOFTWARE.
#

bin_PROGRAMS = kmscube

kmscube_LDADD = \
	$(DRM_LIBS) \
	$(GBM_LIBS) \
	$(EGL_LIBS) \
	$(GLES2_LIBS) \
	-lm

kmscube_CFLAGS = \
	-O0 -g \
	-Wall -Wextra \
	-std=c99 \
	$(DRM_CFLAGS) \
	$(GBM_CFLAGS) \
	$(EGL_CFLAGS) \
	$(GLES2_CFLAGS)

kmscube_SOURCES = \
	common.c \
	common.h \
	cube-smooth.c \
	cube-tex.c \
	drm-atomic.c \
	drm-common.c \
	drm-common.h \
	drm-legacy.c \
	esTransform.c \
	esUtil.h \
	frame-512x512-NV12.c \
	frame-512x512-RGBA.c \
	kmscube.c

if ENABLE_GST
kmscube_LDADD += $(GST_LIBS)
kmscube_CFLAGS += $(GST_CFLAGS)
kmscube_SOURCES += cube-video.c gst-decoder.c
endif
