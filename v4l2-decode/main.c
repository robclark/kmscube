/*
 * V4L2 Codec decoding example application
 * Kamil Debski <k.debski@samsung.com>
 *
 * Main file of the application
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
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <linux/input.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <signal.h>
#include <poll.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>

#include "common.h"
#include "video.h"

#define DBG_TAG "  main"

int debug_level = 2;

#define DRM_ALIGN(val, align)	((val + (align - 1)) & ~(align - 1))

#define av_err(errnum, fmt, ...) \
	err(fmt ": %s", ##__VA_ARGS__, av_err2str(errnum))

/* This is the size of the buffer for the compressed stream.
 * It limits the maximum compressed frame size. */
#define STREAM_BUUFER_SIZE	(1024 * 1024)

static void stream_close(struct instance *i);

static const int event_type[] = {
	V4L2_EVENT_EOS,
	V4L2_EVENT_SOURCE_CHANGE,
};

static int
subscribe_events(struct instance *i)
{
	const int n_events = sizeof(event_type) / sizeof(event_type[0]);
	int idx;

	for (idx = 0; idx < n_events; idx++) {
		if (video_subscribe_event(i, event_type[idx]))
			return -1;
	}

	return 0;
}

static EGLImage create_egl_image(struct egl *egl, struct instance *i, int fd)
{
	uint32_t stride = DRM_ALIGN(i->width, 128);
	uint32_t y_scanlines = DRM_ALIGN(i->height, 32);

	const EGLint attr[] = {
		EGL_WIDTH, i->width,
		EGL_HEIGHT, i->height,
		EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_NV12,
		EGL_DMA_BUF_PLANE0_FD_EXT, fd,
		EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
		EGL_DMA_BUF_PLANE0_PITCH_EXT, stride,
		EGL_DMA_BUF_PLANE1_FD_EXT, fd,
		EGL_DMA_BUF_PLANE1_OFFSET_EXT, stride * y_scanlines,
		EGL_DMA_BUF_PLANE1_PITCH_EXT, stride,
		EGL_NONE
	};
	EGLImage img;

	img = egl->eglCreateImageKHR(egl->display, EGL_NO_CONTEXT,
			EGL_LINUX_DMA_BUF_EXT, NULL, attr);
	assert(img);

	return img;
}

static int
restart_capture(struct egl *egl, struct instance *i)
{
	struct video *vid = &i->video;
	unsigned int n;
	int ret;

	/* Stop capture and release buffers */
	if (vid->cap_buf_cnt > 0 && video_stop_capture(i))
		return -1;

	/* Setup capture queue with new parameters */
	if (video_setup_capture(i, 4, i->width, i->height))
		return -1;

	for (n = 0; n < vid->cap_buf_cnt; n++) {
		int fd;

		if (video_export_capture(i, n, &fd))
			return -1;

		dbg("exported capture buffer index:%u, fd:%d", n, fd);

		i->eglimg[n] = create_egl_image(egl, i, fd);
		if (!i->eglimg[n])
			return -1;
	}

	/* Queue all capture buffers */
	for (n = 0; n < vid->cap_buf_cnt; n++) {
		if (video_queue_buf_cap(i, n))
			return -1;
	}

	if (video_stream(i, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			 VIDIOC_STREAMON))
		return -1;

	return 0;
}

static int
handle_video_event(struct instance *i)
{
	struct v4l2_event event;

	if (video_dequeue_event(i, &event))
		return -1;

	switch (event.type) {
	case V4L2_EVENT_EOS:
		info("EOS reached");
		break;
	case V4L2_EVENT_SOURCE_CHANGE:
		/* flush capture queue, we will reconfigure it when flush
		 * done event is received */
		video_flush(i, V4L2_DEC_QCOM_CMD_FLUSH_CAPTURE);
		break;
	default:
		dbg("unknown event type occurred %x", event.type);
		break;
	}

	return 0;
}

static void
cleanup(struct instance *i)
{
	stream_close(i);

	if (i->sigfd != 1)
		close(i->sigfd);
	if (i->video.fd)
		video_close(i);
}

static int
save_frame(struct instance *i, const void *buf, unsigned int size)
{
	mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
	char filename[64];
	int fd;
	int ret;
	static unsigned int frame_num = 0;

	if (!i->save_frames)
		return 0;

	if (!i->save_path)
		ret = sprintf(filename, "/mnt/frame%04d.nv12", frame_num);
	else
		ret = sprintf(filename, "%s/frame%04d.nv12", i->save_path,
			      frame_num);
	if (ret < 0) {
		err("sprintf fail (%s)", strerror(errno));
		return -1;
	}

	dbg("create file %s", filename);

	fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_SYNC, mode);
	if (fd < 0) {
		err("cannot open file (%s)", strerror(errno));
		return -1;
	}

	ret = write(fd, buf, size);
	if (ret < 0) {
		err("cannot write to file (%s)", strerror(errno));
		return -1;
	}

	close(fd);

	frame_num++;

	return 0;
}

static int
parse_frame(struct instance *i, AVPacket *pkt)
{
	int ret;

	if (!i->bsf_data_pending) {
		ret = av_read_frame(i->avctx, pkt);
		if (ret < 0)
			return ret;

		if (pkt->stream_index != i->stream->index) {
			av_packet_unref(pkt);
			return AVERROR(EAGAIN);
		}

		if (i->bsf) {
			ret = av_bsf_send_packet(i->bsf, pkt);
			if (ret < 0)
				return ret;

			i->bsf_data_pending = 1;
		}
	}

	if (i->bsf) {
		ret = av_bsf_receive_packet(i->bsf, pkt);
		if (ret == AVERROR(EAGAIN))
			i->bsf_data_pending = 0;

		if (ret < 0)
			return ret;
	}

	return 0;
}

static void
finish(struct instance *i)
{
	pthread_mutex_lock(&i->lock);
	i->finish = 1;
	pthread_cond_signal(&i->cond);
	pthread_mutex_unlock(&i->lock);
}

static int
send_eos(struct instance *i, int buf_index)
{
	struct video *vid = &i->video;
	struct timeval tv;

	tv.tv_sec = 0;
	tv.tv_usec = 0;

	if (video_queue_buf_out(i, buf_index, 1, V4L2_BUF_FLAG_LAST, tv) < 0)
		return -1;

	vid->out_buf_flag[buf_index] = 1;

	return 0;
}

static int
send_pkt(struct instance *i, int buf_index, AVPacket *pkt)
{
	struct video *vid = &i->video;
	struct timeval tv;
	uint32_t flags;

	memcpy(vid->out_buf_addr[buf_index], pkt->data, pkt->size);
	flags = 0;

	if (pkt->pts != AV_NOPTS_VALUE) {
		AVRational vid_timebase;
		AVRational v4l_timebase = { 1, 1000000000 };
		int64_t v4l_pts;

		if (i->bsf)
			vid_timebase = i->bsf->time_base_out;
		else
			vid_timebase = i->stream->time_base;

		v4l_pts = av_rescale_q(pkt->pts, vid_timebase, v4l_timebase);
		tv.tv_sec = v4l_pts / 1000000;
		tv.tv_usec = v4l_pts % 1000000;
		i->last_pts = pkt->pts;

		dbg("input ts: %08ld.%08lu (pts: %08ld, dts: %08ld, v4l_pts: %08ld, num:%d, den:%d) %s %s, dur: %ld",
			tv.tv_sec, tv.tv_usec, pkt->pts, pkt->dts, v4l_pts,
			vid_timebase.num, vid_timebase.den,
			pkt->flags & AV_PKT_FLAG_KEY ? "keyframe" : "",
			pkt->flags & AV_PKT_FLAG_CORRUPT ? "corrupted" : "",
			pkt->duration);
	} else {
		tv.tv_sec = 0;
		tv.tv_usec = 0;
	}

	if (video_queue_buf_out(i, buf_index, pkt->size, flags, tv) < 0)
		return -1;

	vid->out_buf_flag[buf_index] = 1;

	return 0;
}

static int
get_buffer_unlocked(struct instance *i)
{
	struct video *vid = &i->video;

	for (int n = 0; n < vid->out_buf_cnt; n++) {
		if (!vid->out_buf_flag[n])
			return n;
	}

	return -1;
}

static double get_video_clock(struct instance *i)
{
	double delta;

	delta = (av_gettime() - i->video_current_pts_time) / 1000000.0;

	return i->video_current_pts + delta;
}

/* This threads is responsible for parsing the stream and
 * feeding video decoder with consecutive frames to decode */
static void *
parser_thread_func(void *args)
{
	struct instance *i = (struct instance *)args;
	AVPacket pkt;
	int buf, parse_ret, ret;
	int64_t seek_target;

	dbg("Parser thread started");

	av_init_packet(&pkt);

	while (1) {

		if (i->seek) {
			if (i->seek_flag & AVSEEK_FLAG_BACKWARD)
				seek_target = i->last_pts - 1000*10;
			else
				seek_target = i->last_pts + 1000*10;

			info("seek %s at %ld (stream idx:%d)",
				i->seek_flag & AVSEEK_FLAG_BACKWARD
					? "backward" : "forward",
				seek_target,
				i->stream->index);

			ret = av_seek_frame(i->avctx, i->stream->index,
					    seek_target, 0);
			if (ret < 0)
				err("%s: seek failed %d", __func__, ret);

/*			video_flush(i, V4L2_DEC_QCOM_CMD_FLUSH_CAPTURE);*/
			i->seek = 0;
		}

		parse_ret = parse_frame(i, &pkt);
		if (parse_ret == AVERROR(EAGAIN))
			continue;
		buf = -1;

		pthread_mutex_lock(&i->lock);
		while (!i->finish && (buf = get_buffer_unlocked(i)) < 0)
			pthread_cond_wait(&i->cond, &i->lock);
		pthread_mutex_unlock(&i->lock);

		if (buf < 0) {
			/* decoding stopped before parsing ended, abort */
			break;
		}

		if (parse_ret < 0) {
			if (parse_ret == AVERROR_EOF)
				dbg("Queue end of stream");
			else
				av_err(parse_ret, "Parsing failed");

			send_eos(i, buf);
			break;
		}

		if (send_pkt(i, buf, &pkt) < 0)
			break;

		av_packet_unref(&pkt);
	}

	av_packet_unref(&pkt);

	dbg("Parser thread finished");

	return NULL;
}

static EGLImage
handle_video_capture(struct instance *i)
{
	struct video *vid = &i->video;
	struct timeval tv;
	unsigned int bytesused;
	int ret, n, finished;
	static int in_disp = -1;

	/* capture buffer is ready */

	ret = video_dequeue_capture(i, &n, &finished,
				    &bytesused, &tv);
	if (ret < 0) {
		err("dequeue capture buffer fail");
		return NULL;
	}

	if (bytesused > 0) {
		vid->total_captured++;

		save_frame(i, (void *)vid->cap_buf_addr[n][0], bytesused);

		dbg("show buffer %06lu (idx:%02u) %08ld.%08lu",
			vid->total_captured - 1, n,
			tv.tv_sec, tv.tv_usec);

		i->video_current_pts_time = av_gettime();

		if (in_disp != -1)
			video_queue_buf_cap(i, in_disp);

		in_disp = n;
	} else if (!i->reconfigure_pending) {
		video_queue_buf_cap(i, n);
	}

	if (finished) {
		info("End of stream");
		finish(i);
	}

	return i->eglimg[n];
}

static int
handle_video_output(struct instance *i)
{
	struct video *vid = &i->video;
	int ret, n;

	ret = video_dequeue_output(i, &n);
	if (ret < 0) {
		err("dequeue output buffer fail");
		return ret;
	}

	pthread_mutex_lock(&i->lock);
	vid->out_buf_flag[n] = 0;
	pthread_cond_signal(&i->cond);
	pthread_mutex_unlock(&i->lock);

	return 0;
}

static int
handle_signal(struct instance *i)
{
	struct signalfd_siginfo siginfo;
	sigset_t sigmask;

	if (read(i->sigfd, &siginfo, sizeof (siginfo)) < 0) {
		perror("signalfd/read");
		return -1;
	}

	sigemptyset(&sigmask);
	sigaddset(&sigmask, siginfo.ssi_signo);
	sigprocmask(SIG_UNBLOCK, &sigmask, NULL);

	finish(i);

	return 0;
}

static int
setup_signal(struct instance *i)
{
	sigset_t sigmask;
	int fd;

	sigemptyset(&sigmask);
	sigaddset(&sigmask, SIGINT);
	sigaddset(&sigmask, SIGTERM);

	fd = signalfd(-1, &sigmask, SFD_CLOEXEC);
	if (fd < 0) {
		perror("signalfd");
		return -1;
	}

	sigprocmask(SIG_BLOCK, &sigmask, NULL);
	i->sigfd = fd;

	return 0;
}

EGLImage video_frame(struct instance *i)
{
	struct video *vid = &i->video;
	struct pollfd pfd[3];
	short revents;
	int nfds = 0;
	int ret;
	unsigned int idx;

	pfd[nfds].fd = vid->fd;
	pfd[nfds].events = POLLOUT | POLLWRNORM | POLLPRI;
	nfds++;

	if (i->sigfd != -1) {
		pfd[nfds].fd = i->sigfd;
		pfd[nfds].events = POLLIN;
		nfds++;
	}

	while (!i->finish) {
		ret = poll(pfd, nfds, -1);
		if (ret <= 0) {
			err("poll error");
			break;
		}

		if (i->paused)
			pfd[0].events &= ~(POLLIN | POLLRDNORM);
		else
			pfd[0].events |= POLLIN | POLLRDNORM;

		for (idx = 0; idx < nfds; idx++) {
			revents = pfd[idx].revents;
			if (!revents)
				continue;

			switch (idx) {
			case 0:
				if (revents & (POLLIN | POLLRDNORM))
					return handle_video_capture(i);
				if (revents & (POLLOUT | POLLWRNORM))
					handle_video_output(i);
				if (revents & POLLPRI)
					handle_video_event(i);
				break;
			case 1:
				handle_signal(i);
				break;
			}
		}
	}

	dbg("end of stream");

	return NULL;
}

#if 0
static void *kbd_thread_func(void *args)
{
	struct instance *i = args;
	static const uint8_t forward[3] = { 27, 91, 67 };	//key ->
	static const uint8_t backward[3] = { 27, 91, 68 };	//key <-
	static struct termios oldt, newt;
	uint8_t key[3];
	int ret;

	/* tcgetattr gets the parameters of the current terminal
	 * STDIN_FILENO will tell tcgetattr that it should write the settings
	 * of stdin to oldt
	 */
	ret = tcgetattr(STDIN_FILENO, &oldt);
	if (ret) {
		err("cannot get term attr %s", strerror(errno));
		return NULL;
	}
	newt = oldt;

	/* ICANON normally takes care that one line at a time will be processed
	 * that means it will return if it sees a "\n" or an EOF or an EOL
	 */
	newt.c_lflag &= ~ICANON;
	newt.c_lflag &= ~ECHO;

	ret = tcsetattr(STDIN_FILENO, TCSANOW, &newt);
	if (ret) {
		err("cannot set new term attr %s", strerror(errno));
		return NULL;
	}

	fcntl(fileno(stdin), F_SETFL, O_NONBLOCK);

	while (!i->finish) {
		usleep(30*1000);

		ret = read(fileno(stdin), key, 3);
		if (ret < 0)
			continue;

		if (i->seek)
			continue;

		if (key[0] == forward[0] &&
		    key[1] == forward[1] &&
		    key[2] == forward[2]) {
			i->seek = 1;
			i->seek_flag = 0;
		} else if (key[0] == backward[0] &&
			   key[1] == backward[1] &&
			   key[2] == backward[2]) {
			i->seek = 1;
			i->seek_flag |= AVSEEK_FLAG_BACKWARD;
		}
	}

	tcsetattr(STDIN_FILENO, TCSANOW, &oldt);

	dbg("kbd thread finished");

	return NULL;
}
#endif

static void
stream_close(struct instance *i)
{
	i->stream = NULL;
	if (i->bsf)
		av_bsf_free(&i->bsf);
	if (i->avctx)
		avformat_close_input(&i->avctx);
}

static int
get_av_log_level(void)
{
	if (debug_level >= 5)
		return AV_LOG_TRACE;
	if (debug_level >= 4)
		return AV_LOG_DEBUG;
	if (debug_level >= 3)
		return AV_LOG_VERBOSE;
	if (debug_level >= 2)
		return AV_LOG_INFO;
	if (debug_level >= 1)
		return AV_LOG_ERROR;
	return AV_LOG_QUIET;
}

static int
stream_open(struct instance *i)
{
	const AVBitStreamFilter *filter;
	AVCodecParameters *codecpar;
	int codec;
	int ret;

	av_log_set_level(get_av_log_level());

	av_register_all();
	avformat_network_init();

	ret = avformat_open_input(&i->avctx, i->url, NULL, NULL);
	if (ret < 0) {
		av_err(ret, "failed to open %s", i->url);
		goto fail;
	}

	ret = avformat_find_stream_info(i->avctx, NULL);
	if (ret < 0) {
		av_err(ret, "failed to get streams info");
		goto fail;
	}

	av_dump_format(i->avctx, -1, i->url, 0);

	ret = av_find_best_stream(i->avctx, AVMEDIA_TYPE_VIDEO, -1, -1,
				  NULL, 0);
	if (ret < 0) {
		av_err(ret, "stream does not seem to contain video");
		goto fail;
	}

	i->stream = i->avctx->streams[ret];
	codecpar = i->stream->codecpar;

	i->width = codecpar->width;
	i->height = codecpar->height;

	filter = NULL;

	switch (codecpar->codec_id) {
	case AV_CODEC_ID_H263:
		codec = V4L2_PIX_FMT_H263;
		break;
	case AV_CODEC_ID_H264:
		codec = V4L2_PIX_FMT_H264;
		filter = av_bsf_get_by_name("h264_mp4toannexb");
		break;
#if 0
	case AV_CODEC_ID_HEVC:
		codec = V4L2_PIX_FMT_HEVC;
		filter = av_bsf_get_by_name("hevc_mp4toannexb");
		break;
#endif
	case AV_CODEC_ID_MPEG2VIDEO:
		codec = V4L2_PIX_FMT_MPEG2;
		break;
	case AV_CODEC_ID_MPEG4:
		codec = V4L2_PIX_FMT_MPEG4;
		break;
#if 0
	case AV_CODEC_ID_MSMPEG4V3:
		codec = V4L2_PIX_FMT_DIVX_311;
		break;
#endif
	case AV_CODEC_ID_WMV3:
		codec = V4L2_PIX_FMT_VC1_ANNEX_G;
		break;
	case AV_CODEC_ID_VC1:
		codec = V4L2_PIX_FMT_VC1_ANNEX_L;
		break;
	case AV_CODEC_ID_VP8:
		codec = V4L2_PIX_FMT_VP8;
		break;
#if 0
	case AV_CODEC_ID_VP9:
		codec = V4L2_PIX_FMT_VP9;
		break;
#endif
	default:
		err("cannot decode %s", avcodec_get_name(codecpar->codec_id));
		goto fail;
	}

	i->fourcc = codec;

	if (filter) {
		ret = av_bsf_alloc(filter, &i->bsf);
		if (ret < 0) {
			av_err(ret, "cannot allocate bistream filter");
			goto fail;
		}

		avcodec_parameters_copy(i->bsf->par_in, codecpar);
		i->bsf->time_base_in = i->stream->time_base;

		ret = av_bsf_init(i->bsf);
		if (ret < 0) {
			av_err(ret, "failed to initialize bitstream filter");
			goto fail;
		}
	}

	return 0;

fail:
	stream_close(i);
	return -1;
}

struct instance *
video_init(struct egl *egl, const char *filename)
{
	static struct instance inst = {0};
	pthread_t parser_thread;
	pthread_t kbd_thread;
	int ret;

	inst.url = filename;

	inst.sigfd = -1;
	pthread_mutex_init(&inst.lock, 0);
	pthread_cond_init(&inst.cond, 0);

	ret = stream_open(&inst);
	if (ret)
		goto err;

	inst.video.name = "/dev/video0";

	ret = video_open(&inst, inst.video.name);
	if (ret)
		goto err;

	ret = subscribe_events(&inst);
	if (ret)
		goto err;

	ret = video_setup_output(&inst, inst.fourcc, STREAM_BUUFER_SIZE, 6);
	if (ret)
		goto err;

	ret = video_set_control(&inst);
	if (ret)
		goto err;

	ret = video_stream(&inst, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
			   VIDIOC_STREAMON);
	if (ret)
		goto err;

	ret = restart_capture(egl, &inst);
	if (ret)
		goto err;

	dbg("Launching threads");

	setup_signal(&inst);

	if (pthread_create(&parser_thread, NULL, parser_thread_func, &inst))
		goto err;

#if 0
	if (pthread_create(&kbd_thread, NULL, kbd_thread_func, &inst))
		goto err;

	main_loop(&inst);

	pthread_join(parser_thread, 0);
	pthread_join(kbd_thread, 0);
#endif

	return &inst;

#if 0
	struct video *vid = &inst.video;

	for (int n = 0; n < vid->cap_buf_cnt; n++) {
		close(inst.disp_buf[n].dbuf_fd);
		drm_dmabuf_rmfb(&inst.disp_buf[n]);
	}

	video_stop_capture(&inst);
	video_stop_output(&inst);

	cleanup(&inst);

	pthread_cond_destroy(&inst.cond);
	pthread_mutex_destroy(&inst.lock);

	info("Total frames captured %ld", inst.video.total_captured);

	return 0;
err:
	cleanup(&inst);
	return 1;
#endif

err:
	return NULL;
}

