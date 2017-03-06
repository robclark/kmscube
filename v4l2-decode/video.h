/*
 * V4L2 Codec decoding example application
 * Kamil Debski <k.debski@samsung.com>
 *
 *
 * Copyright 2012 Samsung Electronics Co., Ltd.
 * Copyright (c) 2015 Linaro Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#ifndef INCLUDE_VIDEO_H
#define INCLUDE_VIDEO_H

#include <stdint.h>
#include <linux/videodev2.h>

#define V4L2_DEC_QCOM_CMD_FLUSH_CAPTURE		0
#define V4L2_DEC_QCOM_CMD_FLUSH			100

struct instance;

/* Open the video decoder device */
int video_open(struct instance *i, char *name);

/* Close the video decoder devices */
void video_close(struct instance *i);

/* Subscribe to an event on the video device */
int video_subscribe_event(struct instance *i, int event_type);

/* Setup the OUTPUT queue. The size determines the size for the stream
 * buffer. This is the maximum size a single compressed frame can have.
 * The count is the number of the stream buffers to allocate. */
int video_setup_output(struct instance *i, unsigned long codec,
		       unsigned int size, int count);

/* Setup the CAPTURE queue. */
int video_setup_capture(struct instance *i, int num_buffers, int w, int h);

/* Stop OUTPUT queue and release buffers */
int video_stop_output(struct instance *i);

/* Stop CAPTURE queue and release buffers */
int video_stop_capture(struct instance *i);

/* Queue OUTPUT buffer */
int video_queue_buf_out(struct instance *i, int n, int length,
			uint32_t flags, struct timeval ts);

/* Queue CAPTURE buffer */
int video_queue_buf_cap(struct instance *i, int n);

/* Control MFC streaming */
int video_stream(struct instance *i, enum v4l2_buf_type type, int status);

/* Flush a queue */
int video_flush(struct instance *i, uint32_t flags);

int video_export_capture(struct instance *i, unsigned int idx, int *fd);
int video_create_buf(struct instance *i, unsigned int width,
		     unsigned int height, unsigned int *index);

/* Dequeue a buffer, the structure *buf is used to return the parameters of the
 * dequeued buffer. */
int video_dequeue_output(struct instance *i, int *n);
int video_dequeue_capture(struct instance *i, int *n, int *finished,
			  unsigned int *bytesused, struct timeval *ts);

/* Dequeue a pending event */
int video_dequeue_event(struct instance *i, struct v4l2_event *ev);

int video_set_control(struct instance *i);
int video_set_dpb(struct instance *i, unsigned int format);

#endif /* INCLUDE_VIDEO_H */

