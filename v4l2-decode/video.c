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

#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <endian.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

#include <linux/videodev2.h>

#include "video.h"
#include "common.h"

#define DBG_TAG "   vid"

#define CASE(ENUM) case ENUM: return #ENUM;

static const char *fourcc_to_string(uint32_t fourcc)
{
	static __thread char s[4];
	uint32_t fmt = htole32(fourcc);

	memcpy(s, &fmt, 4);

	return s;
}

static const char *buf_type_to_string(enum v4l2_buf_type type)
{
	switch (type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		return "OUTPUT";
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		return "CAPTURE";
	default:
		return "??";
	}
}

static const char *v4l2_field_to_string(enum v4l2_field field)
{
	switch (field) {
	CASE(V4L2_FIELD_ANY)
	CASE(V4L2_FIELD_NONE)
	CASE(V4L2_FIELD_TOP)
	CASE(V4L2_FIELD_BOTTOM)
	CASE(V4L2_FIELD_INTERLACED)
	CASE(V4L2_FIELD_SEQ_TB)
	CASE(V4L2_FIELD_SEQ_BT)
	CASE(V4L2_FIELD_ALTERNATE)
	CASE(V4L2_FIELD_INTERLACED_TB)
	CASE(V4L2_FIELD_INTERLACED_BT)
	default: return "unknown";
	}
}

static const char *v4l2_colorspace_to_string(enum v4l2_colorspace cspace)
{
	switch (cspace) {
	CASE(V4L2_COLORSPACE_SMPTE170M)
	CASE(V4L2_COLORSPACE_SMPTE240M)
	CASE(V4L2_COLORSPACE_REC709)
	CASE(V4L2_COLORSPACE_BT878)
	CASE(V4L2_COLORSPACE_470_SYSTEM_M)
	CASE(V4L2_COLORSPACE_470_SYSTEM_BG)
	CASE(V4L2_COLORSPACE_JPEG)
	CASE(V4L2_COLORSPACE_SRGB)
	default: return "unknown";
	}
}

#undef CASE

static void list_formats(struct instance *i, enum v4l2_buf_type type)
{
	struct v4l2_fmtdesc fdesc;
	struct v4l2_frmsizeenum frmsize;

	dbg("%s formats:", buf_type_to_string(type));

	memzero(fdesc);
	fdesc.type = type;

	while (!ioctl(i->video.fd, VIDIOC_ENUM_FMT, &fdesc)) {
		dbg("  %s", fdesc.description);

		memzero(frmsize);
		frmsize.pixel_format = fdesc.pixelformat;

		while (!ioctl(i->video.fd, VIDIOC_ENUM_FRAMESIZES, &frmsize)) {
			switch (frmsize.type) {
			case V4L2_FRMSIZE_TYPE_DISCRETE:
				dbg("    %dx%d",
				    frmsize.discrete.width,
				    frmsize.discrete.height);
				break;
			case V4L2_FRMSIZE_TYPE_STEPWISE:
			case V4L2_FRMSIZE_TYPE_CONTINUOUS:
				dbg("    %dx%d to %dx%d, step %+d%+d",
				    frmsize.stepwise.min_width,
				    frmsize.stepwise.min_height,
				    frmsize.stepwise.max_width,
				    frmsize.stepwise.max_height,
				    frmsize.stepwise.step_width,
				    frmsize.stepwise.step_height);
				break;
			}

			if (frmsize.type != V4L2_FRMSIZE_TYPE_DISCRETE)
				break;

			frmsize.index++;
		}

		fdesc.index++;
	}
}

int video_open(struct instance *i, char *name)
{
	struct v4l2_capability cap;

	i->video.fd = open(name, O_RDWR, 0);
	if (i->video.fd < 0) {
		err("Failed to open video decoder: %s", name);
		return -1;
	}

	memzero(cap);
	if (ioctl(i->video.fd, VIDIOC_QUERYCAP, &cap) < 0) {
		err("Failed to verify capabilities: %m");
		return -1;
	}

	dbg("caps (%s): driver=\"%s\" bus_info=\"%s\" card=\"%s\" "
	    "version=%u.%u.%u", name, cap.driver, cap.bus_info, cap.card,
	    (cap.version >> 16) & 0xff,
	    (cap.version >> 8) & 0xff,
	    cap.version & 0xff);

	dbg("  [%c] V4L2_CAP_VIDEO_CAPTURE",
	    cap.capabilities & V4L2_CAP_VIDEO_CAPTURE ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_VIDEO_CAPTURE_MPLANE",
	    cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_VIDEO_OUTPUT",
	    cap.capabilities & V4L2_CAP_VIDEO_OUTPUT ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_VIDEO_OUTPUT_MPLANE",
	    cap.capabilities & V4L2_CAP_VIDEO_OUTPUT_MPLANE ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_VIDEO_M2M",
	    cap.capabilities & V4L2_CAP_VIDEO_M2M ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_VIDEO_M2M_MPLANE",
	    cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_VIDEO_OVERLAY",
	    cap.capabilities & V4L2_CAP_VIDEO_OVERLAY ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_VBI_CAPTURE",
	    cap.capabilities & V4L2_CAP_VBI_CAPTURE ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_VBI_OUTPUT",
	    cap.capabilities & V4L2_CAP_VBI_OUTPUT ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_SLICED_VBI_CAPTURE",
	    cap.capabilities & V4L2_CAP_SLICED_VBI_CAPTURE ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_SLICED_VBI_OUTPUT",
	    cap.capabilities & V4L2_CAP_SLICED_VBI_OUTPUT ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_RDS_CAPTURE",
	    cap.capabilities & V4L2_CAP_RDS_CAPTURE ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_VIDEO_OUTPUT_OVERLAY",
	    cap.capabilities & V4L2_CAP_VIDEO_OUTPUT_OVERLAY ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_HW_FREQ_SEEK",
	    cap.capabilities & V4L2_CAP_HW_FREQ_SEEK ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_RDS_OUTPUT",
	    cap.capabilities & V4L2_CAP_RDS_OUTPUT ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_TUNER",
	    cap.capabilities & V4L2_CAP_TUNER ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_AUDIO",
	    cap.capabilities & V4L2_CAP_AUDIO ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_RADIO",
	    cap.capabilities & V4L2_CAP_RADIO ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_MODULATOR",
	    cap.capabilities & V4L2_CAP_MODULATOR ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_SDR_CAPTURE",
	    cap.capabilities & V4L2_CAP_SDR_CAPTURE ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_EXT_PIX_FORMAT",
	    cap.capabilities & V4L2_CAP_EXT_PIX_FORMAT ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_READWRITE",
	    cap.capabilities & V4L2_CAP_READWRITE ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_ASYNCIO",
	    cap.capabilities & V4L2_CAP_ASYNCIO ? '*' : ' ');
	dbg("  [%c] V4L2_CAP_STREAMING",
	    cap.capabilities & V4L2_CAP_STREAMING ? '*' : ' ');

	if (!(cap.capabilities & V4L2_CAP_STREAMING) ||
	    !(cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE)) {
		err("Insufficient capabilities for video device (is %s correct?)",
		    name);
		return -1;
	}

	list_formats(i, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	list_formats(i, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

        return 0;
}

void video_close(struct instance *i)
{
	close(i->video.fd);
}

int video_set_control(struct instance *i)
{
	return 0;
}

int video_set_dpb(struct instance *i, unsigned int format)
{
	return 0;
}

static int video_count_capture_queued_bufs(struct video *vid)
{
	int cap_queued = 0;

	for (int idx = 0; idx < vid->cap_buf_cnt; idx++) {
		if (vid->cap_buf_flag[idx])
			cap_queued++;
	}

	return cap_queued;
}

static int video_count_output_queued_bufs(struct video *vid)
{
	int out_queued = 0;

	for (int idx = 0; idx < vid->out_buf_cnt; idx++) {
		if (vid->out_buf_flag[idx])
			out_queued++;
	}

	return out_queued;
}

int video_queue_buf_out(struct instance *i, int n, int length,
			uint32_t flags, struct timeval timestamp)
{
	struct video *vid = &i->video;
	enum v4l2_buf_type type;
	struct v4l2_buffer buf;
	struct v4l2_plane planes[2];

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

	if (n >= vid->out_buf_cnt) {
		err("tried to queue a non existing %s buffer",
		    buf_type_to_string(type));
		return -1;
	}

	memzero(buf);
	memset(planes, 0, sizeof(planes));
	buf.type = type;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = n;
	buf.length = 1;
	buf.m.planes = planes;

	buf.m.planes[0].length = vid->out_buf_size;
	buf.m.planes[0].bytesused = length;
	buf.m.planes[0].data_offset = 0;

	buf.flags = flags;
	buf.timestamp = timestamp;

	if (ioctl(vid->fd, VIDIOC_QBUF, &buf) < 0) {
		err("failed to queue %s buffer (index=%d): %m",
		    buf_type_to_string(buf.type), buf.index);
		return -1;
	}

	dbg("%s: queued buffer %d (flags:%08x, bytesused:%d, ts: %ld.%lu), "
	    "%d/%d queued", buf_type_to_string(buf.type), buf.index, buf.flags,
	    buf.m.planes[0].bytesused, buf.timestamp.tv_sec,
	    buf.timestamp.tv_usec, video_count_output_queued_bufs(vid),
	    vid->out_buf_cnt);

	return 0;
}

int video_queue_buf_cap(struct instance *i, int n)
{
	struct video *vid = &i->video;
	enum v4l2_buf_type type;
	struct v4l2_buffer buf;
	struct v4l2_plane planes[2];

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

	if (n >= vid->cap_buf_cnt) {
		err("tried to queue a non existing %s buffer",
		    buf_type_to_string(type));
		return -1;
	}

	memzero(buf);
	memset(planes, 0, sizeof(planes));

	buf.type = type;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.index = n;
	buf.length = 1;
	buf.m.planes = planes;
	buf.m.planes[0].bytesused = 1;
	buf.m.planes[0].data_offset = 0;
	buf.m.planes[0].length = vid->cap_buf_size[0];

	if (ioctl(vid->fd, VIDIOC_QBUF, &buf) < 0) {
		err("failed to queue %s buffer (index=%d): %m",
		    buf_type_to_string(buf.type), buf.index);
		return -1;
	}

	vid->cap_buf_flag[n] = 1;

	dbg("%s: queued buffer %d, %d/%d queued", buf_type_to_string(buf.type),
	    buf.index, video_count_capture_queued_bufs(vid), vid->cap_buf_cnt);

	return 0;
}

static int video_dequeue_buf(struct instance *i, struct v4l2_buffer *buf)
{
	struct video *vid = &i->video;
	int ret;

	ret = ioctl(vid->fd, VIDIOC_DQBUF, buf);
	if (ret < 0) {
		err("failed to dequeue buffer on %s queue: %m",
		    buf_type_to_string(buf->type));
		return -errno;
	}

	switch (buf->type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		dbg("%s: dequeued buffer %d, %d/%d queued",
		    buf_type_to_string(buf->type), buf->index,
		    video_count_output_queued_bufs(vid), vid->out_buf_cnt);
		break;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		vid->cap_buf_flag[buf->index] = 0;
		dbg("%s: dequeued buffer %d (flags:%08x, bytesused:%d, "
		    "ts: %ld.%lu), %d/%d queued", buf_type_to_string(buf->type),
		    buf->index, buf->flags, buf->m.planes[0].bytesused,
		    buf->timestamp.tv_sec, buf->timestamp.tv_usec,
		    video_count_capture_queued_bufs(vid), vid->cap_buf_cnt);
		break;
	}

	return 0;
}

int video_dequeue_output(struct instance *i, int *n)
{
	struct v4l2_buffer buf;
	struct v4l2_plane planes[OUT_PLANES];
	int ret;

	memzero(buf);
	buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.m.planes = planes;
	buf.length = OUT_PLANES;

	ret = video_dequeue_buf(i, &buf);
	if (ret < 0)
		return ret;

	*n = buf.index;

	return 0;
}

int video_dequeue_capture(struct instance *i, int *n, int *finished,
			  unsigned int *bytesused, struct timeval *ts)
{
	struct v4l2_buffer buf;
	struct v4l2_plane planes[CAP_PLANES];

	memzero(buf);
	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	buf.memory = V4L2_MEMORY_MMAP;
	buf.m.planes = planes;
	buf.length = 1;

	if (video_dequeue_buf(i, &buf))
		return -1;

	*finished = 0;

	if (buf.flags & V4L2_BUF_FLAG_LAST)
		*finished = 1;

	*bytesused = buf.m.planes[0].bytesused;
	*n = buf.index;

	if (ts)
		*ts = buf.timestamp;

	return 0;
}

int video_stream(struct instance *i, enum v4l2_buf_type type, int status)
{
	struct video *vid = &i->video;
	int ret;

	ret = ioctl(vid->fd, status, &type);
	if (ret) {
		err("failed to stream on %s queue (status=%d)",
		    buf_type_to_string(type), status);
		return -1;
	}

	dbg("%s: stream %s", buf_type_to_string(type),
	    status == VIDIOC_STREAMON ? "ON" :
	    status == VIDIOC_STREAMOFF ? "OFF" : "??");

	return 0;
}

int video_flush(struct instance *i, uint32_t flags)
{
	struct video *vid = &i->video;
	struct v4l2_decoder_cmd dec;

	memzero(dec);
	dec.flags = flags;
	dec.cmd = V4L2_DEC_QCOM_CMD_FLUSH;
	if (ioctl(vid->fd, VIDIOC_DECODER_CMD, &dec) < 0) {
		err("failed to flush: %m");
		return -1;
	}

	return 0;
}

int video_export_capture(struct instance *i, unsigned int idx, int *fd)
{
	struct video *vid = &i->video;
	struct v4l2_exportbuffer eb;
	int ret;

	memzero(eb);
	eb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	eb.index = idx;
	eb.plane = 0;
	eb.flags = 0;

	ret = ioctl(vid->fd, VIDIOC_EXPBUF, &eb);
	if (ret) {
		err("failed to export capture buffer");
		return -1;
	}

	*fd = eb.fd;

	return 0;
}

int video_create_buf(struct instance *i, unsigned int width,
		     unsigned int height, unsigned int *index)
{
	struct video *vid = &i->video;
	struct v4l2_create_buffers b;
	int ret;

	memzero(b);
	b.count = 1;
	b.memory = V4L2_MEMORY_MMAP;
	b.format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.format.fmt.pix_mp.width = width;
	b.format.fmt.pix_mp.height = height;
	b.format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;

	ret = ioctl(vid->fd, VIDIOC_CREATE_BUFS, &b);
	if (ret) {
		err("Failed to create bufs index:%u (%s)", b.index,
			strerror(errno));
		return -1;
	}

	*index = b.index;

	info("create_bufs: index %u, count %u", b.index, b.count);

	return 0;
}

int video_setup_capture(struct instance *i, int num_buffers, int w, int h)
{
	struct video *vid = &i->video;
	enum v4l2_buf_type type;
	struct v4l2_format fmt;
	struct v4l2_requestbuffers reqbuf;
	unsigned int n;
	int ret;

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

	memzero(fmt);
	fmt.type = type;
	fmt.fmt.pix_mp.height = h;
	fmt.fmt.pix_mp.width = w;
	fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;

	if (ioctl(vid->fd, VIDIOC_S_FMT, &fmt) < 0) {
		err("failed to set %s format (%dx%d)",
		    buf_type_to_string(fmt.type), w, h);
		return -1;
	}

	memzero(reqbuf);
	reqbuf.count = num_buffers;
	reqbuf.type = type;
	reqbuf.memory = V4L2_MEMORY_MMAP;

	if (ioctl(vid->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
		err("failed to request %s buffers: %m",
		    buf_type_to_string(type));
		return -1;
	}

	info("%s: requested %d buffers, got %d", buf_type_to_string(type),
	    num_buffers, reqbuf.count);

	vid->cap_buf_cnt = reqbuf.count;

	if (ioctl(vid->fd, VIDIOC_G_FMT, &fmt) < 0) {
		err("failed to get %s format", buf_type_to_string(type));
		return -1;
	}

	dbg("  %dx%d fmt=%s (%d planes) field=%s cspace=%s flags=%08x",
	    fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
	    fourcc_to_string(fmt.fmt.pix_mp.pixelformat),
	    fmt.fmt.pix_mp.num_planes,
	    v4l2_field_to_string(fmt.fmt.pix_mp.field),
	    v4l2_colorspace_to_string(fmt.fmt.pix_mp.colorspace),
	    fmt.fmt.pix_mp.flags);

	for (n = 0; n < fmt.fmt.pix_mp.num_planes; n++)
		dbg("    plane %d: size=%d stride=%d scanlines=%d", n,
		    fmt.fmt.pix_mp.plane_fmt[n].sizeimage,
		    fmt.fmt.pix_mp.plane_fmt[n].bytesperline,
		    fmt.fmt.pix_mp.plane_fmt[n].reserved[0]);

	vid->cap_buf_format = fmt.fmt.pix_mp.pixelformat;
	vid->cap_w = fmt.fmt.pix_mp.width;
	vid->cap_h = fmt.fmt.pix_mp.height;
	vid->cap_buf_stride[0] = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
	vid->cap_buf_size[0] = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;

	for (n = 0; n < vid->cap_buf_cnt; n++) {
		struct v4l2_plane planes[CAP_PLANES];
		struct v4l2_buffer buf;

		memzero(buf);
		memset(planes, 0, sizeof(planes));
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = n;
		buf.m.planes = planes;
		buf.length = CAP_PLANES;

		ret = ioctl(vid->fd, VIDIOC_QUERYBUF, &buf);
		if (ret != 0) {
			err("CAPTURE: QUERYBUF failed (%s)", strerror(errno));
			return -1;
		}

		vid->cap_buf_off[n][0] = buf.m.planes[0].m.mem_offset;
#if 0
		vid->cap_buf_addr[n][0] = mmap(NULL, buf.m.planes[0].length,
					       PROT_READ | PROT_WRITE,
					       MAP_SHARED,
					       vid->fd,
					       buf.m.planes[0].m.mem_offset);

		if (vid->cap_buf_addr[n][0] == MAP_FAILED) {
			err("CAPTURE: Failed to MMAP buffer");
			return -1;
		}
#endif
		vid->cap_buf_size[0] = buf.m.planes[0].length;
	}

	dbg("%s: succesfully mmapped %d buffers", buf_type_to_string(type),
	    vid->cap_buf_cnt);

	return 0;
}

int video_stop_capture(struct instance *i)
{
	struct video *vid = &i->video;
	enum v4l2_buf_type type;
	struct v4l2_requestbuffers reqbuf;
	unsigned int n;

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

	if (video_stream(i, type, VIDIOC_STREAMOFF))
		return -1;

#if 1
	for (n = 0; n < vid->cap_buf_cnt; n++) {
		close(i->v4l_dmabuf_fd[n]);
	}
#endif
	vid->cap_buf_cnt = 0;

	memzero(reqbuf);
	reqbuf.memory = V4L2_MEMORY_MMAP;
	reqbuf.type = type;

	if (ioctl(vid->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
		err("REQBUFS with count=0 on %s queue failed: %m",
		    buf_type_to_string(type));
		return -1;
	}

	return 0;
}

int video_setup_output(struct instance *i, unsigned long codec,
		       unsigned int size, int count)
{
	struct video *vid = &i->video;
	struct v4l2_plane planes[OUT_PLANES];
	struct v4l2_requestbuffers reqbuf;
	enum v4l2_buf_type type;
	struct v4l2_format fmt;
	struct v4l2_buffer buf;
	unsigned int n;
	int ret;

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

	memzero(fmt);
	fmt.type = type;
	fmt.fmt.pix_mp.width = i->width;
	fmt.fmt.pix_mp.height = i->height;
	fmt.fmt.pix_mp.pixelformat = codec;

	if (ioctl(vid->fd, VIDIOC_S_FMT, &fmt) < 0) {
		err("failed to set %s format: %m", buf_type_to_string(type));
		return -1;
	}

	dbg("%s: setup buffer size=%u (requested=%u)", buf_type_to_string(type),
	    fmt.fmt.pix_mp.plane_fmt[0].sizeimage, size);

	vid->out_buf_size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;

	memzero(reqbuf);
	reqbuf.count = count;
	reqbuf.type = type;
	reqbuf.memory = V4L2_MEMORY_MMAP;

	if (ioctl(vid->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
		err("failed to request %s buffers: %m",
		    buf_type_to_string(type));
		return -1;
	}

	vid->out_buf_cnt = reqbuf.count;

	info("OUTPUT: Number of buffers is %u (requested %u)",
	     vid->out_buf_cnt, count);

	for (n = 0; n < vid->out_buf_cnt; n++) {
		memzero(buf);
		memset(planes, 0, sizeof(planes));
		buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = n;
		buf.m.planes = planes;
		buf.length = OUT_PLANES;

		ret = ioctl(vid->fd, VIDIOC_QUERYBUF, &buf);
		if (ret != 0) {
			err("OUTPUT: QUERYBUF failed (%s)", strerror(errno));
			return -1;
		}

		vid->out_buf_off[n] = buf.m.planes[0].m.mem_offset;
		vid->out_buf_size = buf.m.planes[0].length;

		vid->out_buf_addr[n] = mmap(NULL, buf.m.planes[0].length,
					    PROT_READ | PROT_WRITE, MAP_SHARED,
					    vid->fd,
					    buf.m.planes[0].m.mem_offset);

		if (vid->out_buf_addr[n] == MAP_FAILED) {
			err("OUTPUT: Failed to MMAP buffer");
			return -1;
		}

		vid->out_buf_flag[n] = 0;
	}

	info("OUTPUT: querybuf sizeimage %u", vid->out_buf_size);

	return 0;
}

int video_stop_output(struct instance *i)
{
	struct video *vid = &i->video;
	enum v4l2_buf_type type;
	struct v4l2_requestbuffers reqbuf;
	unsigned int n;
	int ret;

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;

	if (video_stream(i, type, VIDIOC_STREAMOFF))
		return -1;

	for (n = 0; n < vid->out_buf_cnt; n++) {
		ret = munmap(vid->out_buf_addr[n], vid->out_buf_size);
		if (ret)
			err("failed to unmap %s buffer: %m",
			    buf_type_to_string(type));
	}

	vid->out_buf_cnt = 0;

	memzero(reqbuf);
	reqbuf.memory = V4L2_MEMORY_MMAP;
	reqbuf.type = type;

	if (ioctl(vid->fd, VIDIOC_REQBUFS, &reqbuf) < 0) {
		err("REQBUFS with count=0 on %s queue failed: %m",
		    buf_type_to_string(type));
		return -1;
	}

	return 0;
}

int video_subscribe_event(struct instance *i, int event_type)
{
	struct v4l2_event_subscription sub;

	memset(&sub, 0, sizeof(sub));
	sub.type = event_type;

	if (ioctl(i->video.fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) {
		err("failed to subscribe to event type %u: %m", sub.type);
		return -1;
	}

	return 0;
}

int video_dequeue_event(struct instance *i, struct v4l2_event *ev)
{
	struct video *vid = &i->video;

	memset(ev, 0, sizeof (*ev));

	if (ioctl(vid->fd, VIDIOC_DQEVENT, ev) < 0) {
		err("failed to dequeue event: %m");
		return -1;
	}

	return 0;
}
