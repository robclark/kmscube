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
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"

#include <drm_fourcc.h>

#include <gst/gst.h>
#include <gst/gstmemory.h>
#include <gst/gstpad.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsink.h>
#include <gst/video/gstvideometa.h>

GST_DEBUG_CATEGORY_EXTERN(kmscube_debug);
#define GST_CAT_DEFAULT kmscube_debug

struct decoder {
	GMainLoop          *loop;
	GstElement         *pipeline;
	GstElement         *sink;
	pthread_t           gst_thread;

	uint32_t            format;
	GstVideoInfo        info;

	const struct gbm   *gbm;
	const struct egl   *egl;
	unsigned            frame;

	EGLImage            last_frame;
	GstSample          *last_samp;
};

static GstPadProbeReturn
pad_probe(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
	struct decoder *dec = user_data;
	GstEvent *event = GST_PAD_PROBE_INFO_EVENT(info);
	GstCaps *caps;

	(void)pad;

	if (GST_EVENT_TYPE(event) != GST_EVENT_CAPS)
		return GST_PAD_PROBE_OK;

	gst_event_parse_caps(event, &caps);

	if (!caps) {
		GST_ERROR("caps event without caps");
		return GST_PAD_PROBE_OK;
	}

	if (!gst_video_info_from_caps(&dec->info, caps)) {
		GST_ERROR("caps event with invalid video caps");
		return GST_PAD_PROBE_OK;
	}

	switch (dec->info.finfo->format) {
	case GST_VIDEO_FORMAT_I420:
		dec->format = DRM_FORMAT_YUV420;
		break;
	case GST_VIDEO_FORMAT_NV12:
		dec->format = DRM_FORMAT_NV12;
		break;
	case GST_VIDEO_FORMAT_YUY2:
		dec->format = DRM_FORMAT_YUYV;
		break;
	default:
		GST_ERROR("unknown format\n");
		return GST_PAD_PROBE_OK;
	}

	GST_DEBUG("got: %ux%u@%4.4s\n", dec->info.width, dec->info.height,
			(char *)&dec->format);

	return GST_PAD_PROBE_OK;
}

static void *
gst_thread_func(void *args)
{
	struct decoder *dec = args;
	g_main_loop_run(dec->loop);
	return NULL;
}

static void
element_added_cb(GstBin *bin, GstElement *element, gpointer user_data)
{
	(void)user_data;
	(void)bin;

	printf("added: %s\n", GST_OBJECT_NAME(element));

	// XXX is there a better way to do this, like match class name?
	if (strstr(GST_OBJECT_NAME(element), "v4l2video0dec") == GST_OBJECT_NAME(element)) {
		/* yes, "capture" rather than "output" because v4l2 is bonkers */
		gst_util_set_object_arg(G_OBJECT(element), "capture-io-mode", "dmabuf");
	}
}

struct decoder *
video_init(const struct egl *egl, const struct gbm *gbm, const char *filename)
{
	struct decoder *dec;
	GstElement *src, *decodebin;

	dec = calloc(1, sizeof(*dec));
	dec->loop = g_main_loop_new(NULL, FALSE);
	dec->gbm = gbm;
	dec->egl = egl;

	/* Setup pipeline: */
	static const char *pipeline =
		"filesrc name=\"src\" ! decodebin name=\"decode\" ! video/x-raw ! appsink sync=false name=\"sink\"";
	dec->pipeline = gst_parse_launch(pipeline, NULL);

	dec->sink = gst_bin_get_by_name(GST_BIN(dec->pipeline), "sink");

	src = gst_bin_get_by_name(GST_BIN(dec->pipeline), "src");
	g_object_set(G_OBJECT(src), "location", filename, NULL);
	gst_object_unref(src);

	/* if we don't limit max-buffers then we can let the decoder outrun
	 * vsync and quickly chew up 100's of MB of buffers:
	 */
	g_object_set(G_OBJECT(dec->sink), "max-buffers", 2, NULL);

	gst_pad_add_probe(gst_element_get_static_pad(dec->sink, "sink"),
			GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
			pad_probe, dec, NULL);

	/* hack to make sure we get dmabuf's from v4l2video0dec.. */
	decodebin = gst_bin_get_by_name(GST_BIN(dec->pipeline), "decode");
	g_signal_connect(decodebin, "element-added", G_CALLBACK(element_added_cb), dec);

	/* let 'er rip! */
	gst_element_set_state(dec->pipeline, GST_STATE_PLAYING);

	pthread_create(&dec->gst_thread, NULL, gst_thread_func, dec);

	return dec;
}

static void
set_last_frame(struct decoder *dec, EGLImage frame, GstSample *samp)
{
	if (dec->last_frame)
		dec->egl->eglDestroyImageKHR(dec->egl->display, dec->last_frame);
	dec->last_frame = frame;
	if (dec->last_samp)
		gst_sample_unref(dec->last_samp);
	dec->last_samp = samp;
}

// TODO this could probably be a helper re-used by cube-tex:
static int
buf_to_fd(const struct gbm *gbm, int size, void *ptr)
{
	struct gbm_bo *bo;
	void *map, *map_data = NULL;
	uint32_t stride;
	int fd;

	/* NOTE: do not actually use GBM_BO_USE_WRITE since that gets us a dumb buffer: */
	bo = gbm_bo_create(gbm->dev, size, 1, GBM_FORMAT_R8, GBM_BO_USE_LINEAR);

	map = gbm_bo_map(bo, 0, 0, size, 1, GBM_BO_TRANSFER_WRITE, &stride, &map_data);

	memcpy(map, ptr, size);

	gbm_bo_unmap(bo, map_data);

	fd = gbm_bo_get_fd(bo);

	/* we have the fd now, no longer need the bo: */
	gbm_bo_destroy(bo);

	return fd;
}

static EGLImage
buffer_to_image(struct decoder *dec, GstBuffer *buf)
{
	struct { int fd, offset, stride; } planes[3];
	GstVideoMeta *meta = gst_buffer_get_video_meta(buf);
	EGLImage image;
	unsigned nmems = gst_buffer_n_memory(buf);
	unsigned nplanes = (dec->format == DRM_FORMAT_YUV420) ? 3 : 2;
	unsigned i;

	if (nmems == nplanes) {
		// XXX TODO..
	} else if (nmems == 1) {
		GstMemory *mem = gst_buffer_peek_memory(buf, 0);
		int fd;

		if (dec->frame == 0) {
			printf("%s zero-copy\n", gst_is_dmabuf_memory(mem) ? "using" : "not");
		}

		if (gst_is_dmabuf_memory(mem)) {
			fd = dup(gst_dmabuf_memory_get_fd(mem));
		} else {
			GstMapInfo info;
			gst_memory_map(mem, &info, GST_MAP_READ);
			fd = buf_to_fd(dec->gbm, info.size, info.data);
			gst_memory_unmap(mem, &info);
		}

		// XXX why don't we get meta??
		if (meta) {
			for (i = 0; i < nplanes; i++) {
				planes[i].fd = fd;
				planes[i].offset = meta->offset[i];
				planes[i].stride = meta->stride[i];
			}
		} else {
			int offset = 0, stride = dec->info.width, height = dec->info.height;

			for (i = 0; i < nplanes; i++) {

				if (i == 1) {
					height /= 2;
					if (nplanes == 3)
						stride /= 2;
				}

				planes[i].fd = fd;
				planes[i].offset = offset;
				planes[i].stride = stride;

				offset += stride * height;
			}
		}
	}

	if (dec->format == DRM_FORMAT_NV12) {
		const EGLint attr[] = {
			EGL_WIDTH, dec->info.width,
			EGL_HEIGHT, dec->info.height,
			EGL_LINUX_DRM_FOURCC_EXT, dec->format,
			EGL_DMA_BUF_PLANE0_FD_EXT, planes[0].fd,
			EGL_DMA_BUF_PLANE0_OFFSET_EXT, planes[0].offset,
			EGL_DMA_BUF_PLANE0_PITCH_EXT, planes[0].stride,
			EGL_DMA_BUF_PLANE1_FD_EXT, planes[1].fd,
			EGL_DMA_BUF_PLANE1_OFFSET_EXT, planes[1].offset,
			EGL_DMA_BUF_PLANE1_PITCH_EXT, planes[1].stride,
			EGL_NONE
		};

		image = dec->egl->eglCreateImageKHR(dec->egl->display, EGL_NO_CONTEXT,
				EGL_LINUX_DMA_BUF_EXT, NULL, attr);
	} else {
		const EGLint attr[] = {
			EGL_WIDTH, dec->info.width,
			EGL_HEIGHT, dec->info.height,
			EGL_LINUX_DRM_FOURCC_EXT, dec->format,
			EGL_DMA_BUF_PLANE0_FD_EXT, planes[0].fd,
			EGL_DMA_BUF_PLANE0_OFFSET_EXT, planes[0].offset,
			EGL_DMA_BUF_PLANE0_PITCH_EXT, planes[0].stride,
			EGL_DMA_BUF_PLANE1_FD_EXT, planes[1].fd,
			EGL_DMA_BUF_PLANE1_OFFSET_EXT, planes[1].offset,
			EGL_DMA_BUF_PLANE1_PITCH_EXT, planes[1].stride,
			EGL_DMA_BUF_PLANE2_FD_EXT, planes[2].fd,
			EGL_DMA_BUF_PLANE2_OFFSET_EXT, planes[2].offset,
			EGL_DMA_BUF_PLANE2_PITCH_EXT, planes[2].stride,
			EGL_NONE
		};

		image = dec->egl->eglCreateImageKHR(dec->egl->display, EGL_NO_CONTEXT,
				EGL_LINUX_DMA_BUF_EXT, NULL, attr);
	}

	for (unsigned i = 0; i < nmems; i++)
		close(planes[i].fd);

	return image;
}

EGLImage
video_frame(struct decoder *dec)
{
	GstSample *samp;
	GstBuffer *buf;
	EGLImage   frame = NULL;

	samp = gst_app_sink_pull_sample(GST_APP_SINK(dec->sink));
	if (!samp)
		return NULL;

	buf = gst_sample_get_buffer(samp);

	// TODO inline buffer_to_image??
	frame = buffer_to_image(dec, buf);

	// TODO in the zero-copy dmabuf case it would be nice to associate
	// the eglimg w/ the buffer to avoid recreating it every frame..

	set_last_frame(dec, frame, samp);

	dec->frame++;

	return frame;
}

void video_deinit(struct decoder *dec)
{
	set_last_frame(dec, NULL, NULL);
	gst_element_set_state(dec->pipeline, GST_STATE_NULL);
	gst_object_unref(dec->sink);
	gst_object_unref(dec->pipeline);
	g_main_loop_quit(dec->loop);
	g_main_loop_unref(dec->loop);
	pthread_join(dec->gst_thread, 0);
	free(dec);
}
