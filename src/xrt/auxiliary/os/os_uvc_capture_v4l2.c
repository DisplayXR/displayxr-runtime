// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  V4L2 capture backend for the UVC stereo camera source (desktop
 *         Linux) — see os_uvc_capture.h. The Linux twin of
 *         os_uvc_capture_mf.cpp, same contract, same names where they map.
 *
 * Privacy-relevant facts, in order of the calls:
 *  - enumerate: sysfs + udev's database only (os_uvc_v4l2_enumerate_at). No
 *    /dev/video node is opened to list devices — the analogue of
 *    MFEnumDeviceSources, and the reason enumeration can run on every rescan
 *    without waking (or wedging) a camera that belongs to someone else.
 *  - list_modes: open the node, VIDIOC_QUERYCAP + ENUM_FMT / ENUM_FRAMESIZES /
 *    ENUM_FRAMEINTERVALS, close. No format is set, no buffer requested, no
 *    stream started (no frames, no camera light). Only ever called for a
 *    device the user's config matched.
 *  - open: S_FMT + S_PARM, 4 mmap'd buffers, STREAMON; close: STREAMOFF,
 *    munmap, close. The service calls these only while a stream is started.
 *  - the decoder report touches no camera (it only tries to dlopen
 *    libturbojpeg).
 *
 * Formats: NV12 > YUYV > MJPEG at the requested size and the rate closest to
 * the requested one (Windows' order). NV12 / YUYV buffers are handed to the
 * source as they are (U_STEREO_UVC_PIXEL_NV12 / _YUY2, planes pointing into
 * the mmap'd buffer, which stays dequeued until the next read / close). MJPEG
 * is decoded on the CPU into NV12 by os_uvc_mjpeg.h — libturbojpeg (dlopen'd,
 * with DCT-domain downscaling towards the output size the source asks for),
 * else the built-in stb_image decoder (slow; one WARN at open). There is no
 * hardware MJPEG decode on Linux (yet): DXR_STEREO_CAMERA_UVC_DECODER=hardware
 * refuses to open an MJPEG mode instead of falling back, as on Windows.
 *
 * Latest-wins, without a reader thread: read() polls the node, then drains
 * every buffer the driver has completed, keeps the newest and requeues the
 * rest (counted as superseded). Everything runs on the caller's thread (the
 * service's camera thread).
 *
 * Unplug: uvcvideo wakes poll with POLLERR / POLLHUP and fails DQBUF with
 * ENODEV; read() then returns U_STEREO_UVC_READ_ERROR (one WARN) and the
 * manager ends the stream, as on Windows when the reader reports a failure.
 *
 * `file:<path>` ids (`displayxr-cli camera uvc-decode-bench`, tests): a
 * concatenated MJPEG stream file read through the same decoder + read path,
 * unpaced; the end of the file is a READ_ERROR (end of stream).
 *
 * @ingroup aux_os
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // realpath, dladdr
#endif

#include "os/os_uvc_capture.h"
#include "os/os_uvc_capture_v4l2.h"
#include "os/os_uvc_mjpeg.h"
#include "os/os_time.h"
#include "util/u_logging.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <linux/videodev2.h>

#define V4L2_BUFFERS 4
#define FILE_PREFIX "file:"
#define MAX_FORMATS 256
#define FILE_MAX_BYTES (512u * 1024u * 1024u)
#define FILE_MAX_FRAMES 100000u


/*
 *
 * Small helpers.
 *
 */

static void
appendf(char *buf, size_t cap, const char *fmt, ...)
{
	size_t len = strnlen(buf, cap);
	if (len + 1 >= cap) {
		return;
	}
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf + len, cap - len, fmt, ap);
	va_end(ap);
}

//! Read a small text file (first line, trailing whitespace trimmed). False if absent.
static bool
read_line(const char *path, char *out, size_t cap)
{
	out[0] = '\0';
	FILE *f = fopen(path, "re");
	if (f == NULL) {
		return false;
	}
	bool ok = fgets(out, (int)cap, f) != NULL;
	fclose(f);
	if (!ok) {
		out[0] = '\0';
		return false;
	}
	size_t n = strlen(out);
	while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ' || out[n - 1] == '\t')) {
		out[--n] = '\0';
	}
	return true;
}

static char *
read_whole(const char *path, size_t max, size_t *out_len)
{
	*out_len = 0;
	FILE *f = fopen(path, "rbe");
	if (f == NULL) {
		return NULL;
	}
	size_t cap = 4096, n = 0;
	char *buf = (char *)malloc(cap + 1);
	while (buf != NULL) {
		if (n == cap) {
			if (cap >= max) {
				break;
			}
			cap = cap * 2 > max ? max : cap * 2;
			char *nb = (char *)realloc(buf, cap + 1);
			if (nb == NULL) {
				free(buf);
				buf = NULL;
				break;
			}
			buf = nb;
		}
		size_t r = fread(buf + n, 1, cap - n, f);
		if (r == 0) {
			break;
		}
		n += r;
	}
	fclose(f);
	if (buf != NULL) {
		buf[n] = '\0';
		*out_len = n;
	}
	return buf;
}

static void
fourcc_str(uint32_t f, char out[5])
{
	for (int i = 0; i < 4; i++) {
		char c = (char)((f >> (8 * i)) & 0xff);
		out[i] = (c >= 32 && c < 127) ? c : '?';
	}
	out[4] = '\0';
}

static int
xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do {
		r = ioctl(fd, req, arg);
	} while (r < 0 && errno == EINTR);
	return r;
}

static bool
is_mjpeg(uint32_t fourcc)
{
	return fourcc == V4L2_PIX_FMT_MJPEG || fourcc == V4L2_PIX_FMT_JPEG;
}


/*
 *
 * Pure helpers (os_uvc_capture_v4l2.h).
 *
 */

void
os_uvc_v4l2_friendly_name(const char *card, const char *product, char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return;
	}
	const bool has_card = card != NULL && card[0] != '\0';
	const bool has_product = product != NULL && product[0] != '\0';
	if (has_card && (!has_product || strstr(card, product) != NULL)) {
		snprintf(out, cap, "%s", card);
	} else if (has_product && has_card) {
		snprintf(out, cap, "%s (%s)", product, card);
	} else if (has_product) {
		snprintf(out, cap, "%s", product);
	} else {
		out[0] = '\0';
	}
}

int
os_uvc_v4l2_udev_capture(const char *record)
{
	static const char key[] = "E:ID_V4L_CAPABILITIES=";
	if (record == NULL) {
		return -1;
	}
	for (const char *line = record; line != NULL && *line != '\0';) {
		const char *nl = strchr(line, '\n');
		const size_t n = nl != NULL ? (size_t)(nl - line) : strlen(line);
		if (n >= sizeof(key) - 1 && strncmp(line, key, sizeof(key) - 1) == 0) {
			const char *v = line + sizeof(key) - 1;
			const size_t vn = n - (sizeof(key) - 1);
			static const char cap[] = ":capture:";
			for (size_t i = 0; i + sizeof(cap) - 1 <= vn; i++) {
				if (strncmp(v + i, cap, sizeof(cap) - 1) == 0) {
					return 1;
				}
			}
			return 0;
		}
		line = nl != NULL ? nl + 1 : NULL;
	}
	return -1;
}

int
os_uvc_v4l2_fourcc_rank(uint32_t fourcc)
{
	if (fourcc == V4L2_PIX_FMT_NV12) {
		return 3;
	}
	if (fourcc == V4L2_PIX_FMT_YUYV) {
		return 2;
	}
	if (is_mjpeg(fourcc)) {
		return 1;
	}
	return 0;
}

bool
os_uvc_v4l2_pick_format(const struct os_uvc_v4l2_format *formats,
                        uint32_t count,
                        const struct u_stereo_uvc_mode *want,
                        bool allow_mjpeg,
                        struct os_uvc_v4l2_format *out)
{
	bool found = false;
	struct os_uvc_v4l2_format best = {0};
	int best_rank = 0;
	for (uint32_t i = 0; i < count; i++) {
		const struct os_uvc_v4l2_format *m = &formats[i];
		const int rank = os_uvc_v4l2_fourcc_rank(m->fourcc);
		if (rank == 0 || (!allow_mjpeg && is_mjpeg(m->fourcc)) || m->width != want->width ||
		    m->height != want->height) {
			continue;
		}
		bool better;
		if (!found) {
			better = true;
		} else if (want->fps > 0.0f) {
			float d = m->fps > want->fps ? m->fps - want->fps : want->fps - m->fps;
			float bd = best.fps > want->fps ? best.fps - want->fps : want->fps - best.fps;
			better = d < bd - 0.01f || (d <= bd + 0.01f && rank > best_rank);
		} else {
			better = m->fps > best.fps + 0.01f || (m->fps >= best.fps - 0.01f && rank > best_rank);
		}
		if (better) {
			best = *m;
			best_rank = rank;
			found = true;
		}
	}
	if (found && out != NULL) {
		*out = best;
	}
	return found;
}

uint32_t
os_uvc_v4l2_modes_from_formats(const struct os_uvc_v4l2_format *formats,
                               uint32_t count,
                               struct u_stereo_uvc_mode *out,
                               uint32_t cap)
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < count && n < cap; i++) {
		const struct os_uvc_v4l2_format *f = &formats[i];
		if (os_uvc_v4l2_fourcc_rank(f->fourcc) == 0) {
			continue;
		}
		bool dup = false;
		for (uint32_t k = 0; k < n && !dup; k++) {
			dup = out[k].width == f->width && out[k].height == f->height && out[k].fps == f->fps;
		}
		if (!dup && out != NULL) {
			out[n].width = f->width;
			out[n].height = f->height;
			out[n].fps = f->fps;
			n++;
		}
	}
	return n;
}

//! End (one past EOI) of the JPEG starting at @p s (an SOI), or 0.
static size_t
jpeg_end(const uint8_t *p, size_t len, size_t s)
{
	size_t i = s + 2;
	// Marker segments up to and including SOS.
	for (;;) {
		if (i + 1 >= len || p[i] != 0xFF) {
			return 0;
		}
		while (i < len && p[i] == 0xFF) {
			i++;
		}
		if (i >= len) {
			return 0;
		}
		const uint8_t m = p[i++];
		if (m == 0x01 || (m >= 0xD0 && m <= 0xD8)) {
			continue;
		}
		if (m == 0xD9) {
			return i; // EOI with no scan: an empty image, still a frame boundary
		}
		if (i + 2 > len) {
			return 0;
		}
		const size_t seg = ((size_t)p[i] << 8) | p[i + 1];
		if (seg < 2 || i + seg > len) {
			return 0;
		}
		i += seg;
		if (m == 0xDA) {
			break;
		}
	}
	// Entropy-coded data (and any further scans): the first FF D9 not stuffed.
	while (i + 1 < len) {
		if (p[i] == 0xFF && p[i + 1] == 0xD9) {
			return i + 2;
		}
		i++;
	}
	return 0;
}

uint32_t
os_uvc_v4l2_mjpeg_split_stream(const uint8_t *data, size_t len, size_t *offsets, size_t *lengths, uint32_t cap)
{
	uint32_t n = 0;
	size_t i = 0;
	while (data != NULL && i + 4 <= len) {
		if (data[i] != 0xFF || data[i + 1] != 0xD8) {
			i++;
			continue;
		}
		const size_t e = jpeg_end(data, len, i);
		if (e == 0) {
			break; // truncated last frame
		}
		if (n < cap && offsets != NULL && lengths != NULL) {
			offsets[n] = i;
			lengths[n] = e - i;
		}
		n++;
		i = e;
	}
	return n;
}

//! Walk up from @p start (a resolved sysfs path) to the USB device holding idVendor / idProduct.
static bool
usb_ids(const char *start, uint16_t *vid, uint16_t *pid, char *product, size_t product_cap)
{
	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s", start);
	for (int up = 0; up < 4; up++) {
		char path[PATH_MAX + 32], v[16], p[16];
		snprintf(path, sizeof(path), "%s/idVendor", dir);
		if (read_line(path, v, sizeof(v))) {
			snprintf(path, sizeof(path), "%s/idProduct", dir);
			if (!read_line(path, p, sizeof(p))) {
				return false;
			}
			char *e1 = NULL, *e2 = NULL;
			unsigned long lv = strtoul(v, &e1, 16), lp = strtoul(p, &e2, 16);
			if (e1 == v || e2 == p || lv > 0xffff || lp > 0xffff) {
				return false;
			}
			*vid = (uint16_t)lv;
			*pid = (uint16_t)lp;
			snprintf(path, sizeof(path), "%s/product", dir);
			read_line(path, product, product_cap);
			return true;
		}
		char *slash = strrchr(dir, '/');
		if (slash == NULL || slash == dir) {
			return false;
		}
		*slash = '\0';
	}
	return false;
}

struct node
{
	unsigned idx;
	struct u_stereo_uvc_device dev;
};

static int
node_cmp(const void *a, const void *b)
{
	const struct node *x = (const struct node *)a, *y = (const struct node *)b;
	if (x->dev.has_vid_pid != y->dev.has_vid_pid) {
		return x->dev.has_vid_pid ? -1 : 1; // USB first
	}
	return x->idx < y->idx ? -1 : x->idx > y->idx ? 1 : 0;
}

uint32_t
os_uvc_v4l2_enumerate_at(
    const char *sysfs_dir, const char *udev_dir, const char *dev_dir, struct u_stereo_uvc_device *out, uint32_t cap)
{
	DIR *d = opendir(sysfs_dir);
	if (d == NULL) {
		return 0;
	}
	enum
	{
		MAX_NODES = 64
	};
	struct node *nodes = (struct node *)calloc(MAX_NODES, sizeof(*nodes));
	uint32_t nn = 0;
	struct dirent *ent;
	while (nodes != NULL && (ent = readdir(d)) != NULL && nn < MAX_NODES) {
		unsigned idx = 0;
		char tail = 0;
		if (sscanf(ent->d_name, "video%u%c", &idx, &tail) != 1) {
			continue;
		}
		char path[PATH_MAX + 64], line[160];
		// Capture node? udev's record (v4l_id ran at plug), else the sysfs index.
		int capture = -1;
		snprintf(path, sizeof(path), "%s/%s/dev", sysfs_dir, ent->d_name);
		unsigned maj = 0, min = 0;
		if (read_line(path, line, sizeof(line)) && sscanf(line, "%u:%u", &maj, &min) == 2 && udev_dir != NULL) {
			snprintf(path, sizeof(path), "%s/c%u:%u", udev_dir, maj, min);
			size_t rl = 0;
			char *rec = read_whole(path, 64 * 1024, &rl);
			if (rec != NULL) {
				capture = os_uvc_v4l2_udev_capture(rec);
				free(rec);
			}
		}
		if (capture < 0) {
			snprintf(path, sizeof(path), "%s/%s/index", sysfs_dir, ent->d_name);
			capture = (!read_line(path, line, sizeof(line)) || strcmp(line, "0") == 0) ? 1 : 0;
		}
		if (capture == 0) {
			continue;
		}
		struct node *n = &nodes[nn];
		memset(n, 0, sizeof(*n));
		n->idx = idx;
		char card[128] = {0}, product[128] = {0};
		snprintf(path, sizeof(path), "%s/%s/name", sysfs_dir, ent->d_name);
		read_line(path, card, sizeof(card));
		snprintf(path, sizeof(path), "%s/%s/device", sysfs_dir, ent->d_name);
		char real[PATH_MAX];
		if (realpath(path, real) != NULL) {
			n->dev.has_vid_pid = usb_ids(real, &n->dev.vid, &n->dev.pid, product, sizeof(product));
		}
		os_uvc_v4l2_friendly_name(card, product, n->dev.name, sizeof(n->dev.name));
		snprintf(n->dev.id, sizeof(n->dev.id), "%.200s/video%u", dev_dir, idx);
		nn++;
	}
	closedir(d);
	if (nodes == NULL) {
		return 0;
	}
	qsort(nodes, nn, sizeof(*nodes), node_cmp);
	uint32_t count = 0;
	for (uint32_t i = 0; i < nn && count < cap && out != NULL; i++) {
		out[count++] = nodes[i].dev;
	}
	free(nodes);
	return count;
}


/*
 *
 * Device queries.
 *
 */

static int
open_node(const char *id)
{
	return open(id, O_RDWR | O_NONBLOCK | O_CLOEXEC);
}

//! QUERYCAP: a streaming video capture node?
static bool
is_capture_node(int fd)
{
	struct v4l2_capability caps;
	memset(&caps, 0, sizeof(caps));
	if (xioctl(fd, VIDIOC_QUERYCAP, &caps) < 0) {
		return false;
	}
	uint32_t c = (caps.capabilities & V4L2_CAP_DEVICE_CAPS) ? caps.device_caps : caps.capabilities;
	return (c & V4L2_CAP_VIDEO_CAPTURE) && (c & V4L2_CAP_STREAMING);
}

static float
interval_fps(uint32_t num, uint32_t den)
{
	return num == 0 ? 0.0f : (float)((double)den / (double)num);
}

static uint32_t
add_rates(int fd, uint32_t fourcc, uint32_t w, uint32_t h, struct os_uvc_v4l2_format *out, uint32_t n, uint32_t cap)
{
	struct v4l2_frmivalenum iv;
	memset(&iv, 0, sizeof(iv));
	iv.pixel_format = fourcc;
	iv.width = w;
	iv.height = h;
	bool any = false;
	for (iv.index = 0; n < cap && xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &iv) == 0; iv.index++) {
		float fps;
		if (iv.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
			fps = interval_fps(iv.discrete.numerator, iv.discrete.denominator);
		} else {
			fps = interval_fps(iv.stepwise.min.numerator, iv.stepwise.min.denominator); // the fastest
		}
		out[n++] = (struct os_uvc_v4l2_format){fourcc, w, h, fps};
		any = true;
		if (iv.type != V4L2_FRMIVAL_TYPE_DISCRETE) {
			break;
		}
	}
	if (!any && n < cap) {
		out[n++] = (struct os_uvc_v4l2_format){fourcc, w, h, 0.0f};
	}
	return n;
}

//! Every usable (format, size, rate) the node offers.
static uint32_t
query_formats(int fd, struct os_uvc_v4l2_format *out, uint32_t cap)
{
	uint32_t n = 0;
	struct v4l2_fmtdesc fd_desc;
	memset(&fd_desc, 0, sizeof(fd_desc));
	fd_desc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	for (fd_desc.index = 0; n < cap && xioctl(fd, VIDIOC_ENUM_FMT, &fd_desc) == 0; fd_desc.index++) {
		const uint32_t fourcc = fd_desc.pixelformat;
		if (os_uvc_v4l2_fourcc_rank(fourcc) == 0) {
			continue;
		}
		struct v4l2_frmsizeenum fs;
		memset(&fs, 0, sizeof(fs));
		fs.pixel_format = fourcc;
		for (fs.index = 0; n < cap && xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) == 0; fs.index++) {
			if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
				n = add_rates(fd, fourcc, fs.discrete.width, fs.discrete.height, out, n, cap);
			} else {
				// Stepwise / continuous: offer the largest size only.
				n = add_rates(fd, fourcc, fs.stepwise.max_width, fs.stepwise.max_height, out, n, cap);
				break;
			}
		}
	}
	return n;
}


/*
 *
 * Decoder policy (the Windows knob, mapped).
 *
 */

enum decoder_pref
{
	DECODER_AUTO,
	DECODER_HARDWARE,
	DECODER_SOFTWARE,
};

static enum decoder_pref
read_decoder_pref(void)
{
	const char *e = getenv("DXR_STEREO_CAMERA_UVC_DECODER");
	if (e == NULL || e[0] == '\0' || strcmp(e, "auto") == 0) {
		return DECODER_AUTO;
	}
	if (strcmp(e, "hardware") == 0) {
		return DECODER_HARDWARE;
	}
	if (strcmp(e, "software") == 0) {
		return DECODER_SOFTWARE;
	}
	U_LOG_W("uvc capture: DXR_STEREO_CAMERA_UVC_DECODER=\"%s\" is not auto|hardware|software — using auto", e);
	return DECODER_AUTO;
}


/*
 *
 * The open device.
 *
 */

struct v4l2_handle
{
	char id[256];
	int fd;
	bool is_file;

	uint32_t fourcc;
	uint32_t width, height, bytesperline, sizeimage;
	float fps;
	uint32_t out_w, out_h; //!< the size the source resamples to (decode-scale target)

	struct
	{
		void *start;
		size_t length;
	} bufs[V4L2_BUFFERS];
	uint32_t nbufs;
	bool streaming;
	int held; //!< the dequeued buffer the last frame points into, -1 = none

	struct os_uvc_mjpeg *dec;

	// file: ids
	uint8_t *file;
	size_t file_len;
	size_t *offs, *lens;
	uint32_t nframes, next;

	// diagnostics
	unsigned long long frames, superseded, dropped, eio_streak;
	int64_t decode_ns;
	bool failed;
};

static void
requeue(struct v4l2_handle *h, uint32_t index)
{
	struct v4l2_buffer b;
	memset(&b, 0, sizeof(b));
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	b.memory = V4L2_MEMORY_MMAP;
	b.index = index;
	if (xioctl(h->fd, VIDIOC_QBUF, &b) < 0 && errno != ENODEV) {
		U_LOG_I("uvc capture (V4L2): QBUF %u failed: %s", index, strerror(errno));
	}
}

static void
release_held(struct v4l2_handle *h)
{
	if (h->held >= 0) {
		requeue(h, (uint32_t)h->held);
		h->held = -1;
	}
}

static void
teardown(struct v4l2_handle *h)
{
	if (h->fd >= 0) {
		if (h->streaming) {
			enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			xioctl(h->fd, VIDIOC_STREAMOFF, &t);
			h->streaming = false;
		}
		for (uint32_t i = 0; i < h->nbufs; i++) {
			if (h->bufs[i].start != NULL && h->bufs[i].start != MAP_FAILED) {
				munmap(h->bufs[i].start, h->bufs[i].length);
			}
		}
		h->nbufs = 0;
		struct v4l2_requestbuffers rb;
		memset(&rb, 0, sizeof(rb));
		rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		rb.memory = V4L2_MEMORY_MMAP;
		rb.count = 0;
		xioctl(h->fd, VIDIOC_REQBUFS, &rb); // free the driver's buffers (fails harmlessly after unplug)
		close(h->fd);
		h->fd = -1;
	}
	os_uvc_mjpeg_destroy(&h->dec);
	free(h->file);
	free(h->offs);
	free(h->lens);
	h->file = NULL;
	h->offs = h->lens = NULL;
}

static bool
open_file(struct v4l2_handle *h, const char *path)
{
	h->file = (uint8_t *)read_whole(path, FILE_MAX_BYTES, &h->file_len);
	if (h->file == NULL || h->file_len == 0) {
		return false;
	}
	uint32_t n = os_uvc_v4l2_mjpeg_split_stream(h->file, h->file_len, NULL, NULL, 0);
	n = n > FILE_MAX_FRAMES ? FILE_MAX_FRAMES : n;
	if (n == 0) {
		return false;
	}
	h->offs = (size_t *)calloc(n, sizeof(size_t));
	h->lens = (size_t *)calloc(n, sizeof(size_t));
	if (h->offs == NULL || h->lens == NULL) {
		return false;
	}
	h->nframes = os_uvc_v4l2_mjpeg_split_stream(h->file, h->file_len, h->offs, h->lens, n);
	h->nframes = h->nframes > n ? n : h->nframes;
	return os_uvc_mjpeg_header(h->file + h->offs[0], h->lens[0], &h->width, &h->height);
}

static bool
start_device(struct v4l2_handle *h, const struct u_stereo_uvc_mode *mode, bool allow_mjpeg, char *why, size_t cap)
{
	h->fd = open_node(h->id);
	if (h->fd < 0) {
		snprintf(why, cap, "open: %s", strerror(errno));
		return false;
	}
	if (!is_capture_node(h->fd)) {
		snprintf(why, cap, "not a streaming video capture node");
		return false;
	}
	struct os_uvc_v4l2_format *formats = (struct os_uvc_v4l2_format *)calloc(MAX_FORMATS, sizeof(*formats));
	if (formats == NULL) {
		snprintf(why, cap, "out of memory");
		return false;
	}
	uint32_t nf = query_formats(h->fd, formats, MAX_FORMATS);
	struct os_uvc_v4l2_format pick;
	bool found = os_uvc_v4l2_pick_format(formats, nf, mode, allow_mjpeg, &pick);
	free(formats);
	if (!found) {
		snprintf(why, cap, "%ux%u is not offered as NV12 / YUYV%s", mode->width, mode->height,
		         allow_mjpeg ? " / MJPEG" : " (MJPEG excluded: DXR_STEREO_CAMERA_UVC_DECODER=hardware)");
		return false;
	}

	struct v4l2_format f;
	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	f.fmt.pix.width = pick.width;
	f.fmt.pix.height = pick.height;
	f.fmt.pix.pixelformat = pick.fourcc;
	f.fmt.pix.field = V4L2_FIELD_ANY;
	if (xioctl(h->fd, VIDIOC_S_FMT, &f) < 0) {
		snprintf(why, cap, "S_FMT: %s", strerror(errno));
		return false;
	}
	if (f.fmt.pix.width != pick.width || f.fmt.pix.height != pick.height || f.fmt.pix.pixelformat != pick.fourcc) {
		char a[5], b[5];
		fourcc_str(pick.fourcc, a);
		fourcc_str(f.fmt.pix.pixelformat, b);
		snprintf(why, cap, "asked %ux%u %s, the driver set %ux%u %s", pick.width, pick.height, a,
		         f.fmt.pix.width, f.fmt.pix.height, b);
		return false;
	}
	h->fourcc = pick.fourcc;
	h->width = f.fmt.pix.width;
	h->height = f.fmt.pix.height;
	h->sizeimage = f.fmt.pix.sizeimage;
	h->bytesperline = f.fmt.pix.bytesperline;
	if (h->bytesperline == 0 && !is_mjpeg(h->fourcc)) {
		h->bytesperline = h->fourcc == V4L2_PIX_FMT_YUYV ? 2 * h->width : h->width;
	}

	// Rate: best effort, as on Windows (the source measures the real one).
	const float want_fps = mode->fps > 0.0f ? mode->fps : pick.fps;
	h->fps = pick.fps;
	if (want_fps > 0.0f) {
		struct v4l2_streamparm p;
		memset(&p, 0, sizeof(p));
		p.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		p.parm.capture.timeperframe.numerator = 1000;
		p.parm.capture.timeperframe.denominator = (uint32_t)(want_fps * 1000.0f + 0.5f);
		if (xioctl(h->fd, VIDIOC_S_PARM, &p) == 0 && p.parm.capture.timeperframe.numerator != 0) {
			h->fps = interval_fps(p.parm.capture.timeperframe.numerator,
			                      p.parm.capture.timeperframe.denominator);
		}
	}

	struct v4l2_requestbuffers rb;
	memset(&rb, 0, sizeof(rb));
	rb.count = V4L2_BUFFERS;
	rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	rb.memory = V4L2_MEMORY_MMAP;
	if (xioctl(h->fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) {
		snprintf(why, cap, "REQBUFS: %s", rb.count < 2 ? "fewer than 2 buffers" : strerror(errno));
		return false;
	}
	h->nbufs = rb.count > V4L2_BUFFERS ? V4L2_BUFFERS : rb.count;
	for (uint32_t i = 0; i < h->nbufs; i++) {
		struct v4l2_buffer b;
		memset(&b, 0, sizeof(b));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		if (xioctl(h->fd, VIDIOC_QUERYBUF, &b) < 0) {
			snprintf(why, cap, "QUERYBUF: %s", strerror(errno));
			return false;
		}
		h->bufs[i].length = b.length;
		h->bufs[i].start = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, h->fd, b.m.offset);
		if (h->bufs[i].start == MAP_FAILED) {
			h->bufs[i].start = NULL;
			snprintf(why, cap, "mmap: %s", strerror(errno));
			return false;
		}
		if (xioctl(h->fd, VIDIOC_QBUF, &b) < 0) {
			snprintf(why, cap, "QBUF: %s", strerror(errno));
			return false;
		}
	}
	enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(h->fd, VIDIOC_STREAMON, &t) < 0) {
		snprintf(why, cap, "STREAMON: %s", strerror(errno));
		return false;
	}
	h->streaming = true;
	return true;
}


/*
 *
 * Backend ops.
 *
 */

static uint32_t
v4l2_enumerate(void *ctx, struct u_stereo_uvc_device *out, uint32_t cap)
{
	(void)ctx;
	return os_uvc_v4l2_enumerate_at("/sys/class/video4linux", "/run/udev/data", "/dev", out, cap);
}

static uint32_t
v4l2_list_modes(void *ctx, const char *id, struct u_stereo_uvc_mode *out, uint32_t cap)
{
	(void)ctx;
	if (id == NULL) {
		return 0;
	}
	if (strncmp(id, FILE_PREFIX, strlen(FILE_PREFIX)) == 0) {
		struct v4l2_handle h;
		memset(&h, 0, sizeof(h));
		h.fd = -1;
		uint32_t n = 0;
		if (open_file(&h, id + strlen(FILE_PREFIX)) && cap > 0 && out != NULL) {
			out[0] = (struct u_stereo_uvc_mode){h.width, h.height, 30.0f};
			n = 1;
		}
		teardown(&h);
		return n;
	}
	int fd = open_node(id);
	if (fd < 0) {
		return 0;
	}
	uint32_t n = 0;
	if (is_capture_node(fd)) {
		struct os_uvc_v4l2_format *formats = (struct os_uvc_v4l2_format *)calloc(MAX_FORMATS, sizeof(*formats));
		if (formats != NULL) {
			uint32_t nf = query_formats(fd, formats, MAX_FORMATS);
			n = os_uvc_v4l2_modes_from_formats(formats, nf, out, cap);
			free(formats);
		}
	}
	close(fd);
	return n;
}

static bool
v4l2_open(
    void *ctx, const char *id, const struct u_stereo_uvc_mode *mode, uint32_t out_w, uint32_t out_h, void **out_handle)
{
	(void)ctx;
	*out_handle = NULL;
	if (id == NULL || mode == NULL) {
		return false;
	}
	struct v4l2_handle *h = (struct v4l2_handle *)calloc(1, sizeof(*h));
	if (h == NULL) {
		return false;
	}
	h->fd = -1;
	h->held = -1;
	h->out_w = out_w;
	h->out_h = out_h;
	snprintf(h->id, sizeof(h->id), "%s", id);
	const enum decoder_pref pref = read_decoder_pref();
	char why[256] = {0};
	bool ok;
	if (strncmp(id, FILE_PREFIX, strlen(FILE_PREFIX)) == 0) {
		h->is_file = true;
		h->fourcc = V4L2_PIX_FMT_MJPEG;
		ok = open_file(h, id + strlen(FILE_PREFIX));
		h->fps = 30.0f;
		if (!ok) {
			snprintf(why, sizeof(why), "not a readable MJPEG stream file");
		} else if (pref == DECODER_HARDWARE) {
			ok = false;
			snprintf(why, sizeof(why),
			         "DXR_STEREO_CAMERA_UVC_DECODER=hardware and no hardware MJPEG decoder");
		}
	} else {
		ok = start_device(h, mode, pref != DECODER_HARDWARE, why, sizeof(why));
	}
	if (ok && is_mjpeg(h->fourcc)) {
		h->dec = os_uvc_mjpeg_create(OS_UVC_MJPEG_AUTO);
		if (h->dec == NULL) {
			ok = false;
			snprintf(why, sizeof(why), "no MJPEG decoder");
		}
	}
	if (!ok) {
		U_LOG_W("uvc capture (V4L2): open %s %ux%u@%.1f failed: %s", id, mode->width, mode->height, mode->fps,
		        why);
		teardown(h);
		free(h);
		return false;
	}
	char fcc[5];
	fourcc_str(h->fourcc, fcc);
	if (h->dec != NULL) {
		char lib[256] = {0};
		const bool tj = os_uvc_mjpeg_turbojpeg_available(lib, sizeof(lib));
		U_LOG_W("uvc capture (V4L2): started %s %ux%u@%.2f %s — decoder %s%s%s%s", id, h->width, h->height,
		        h->fps, fcc, os_uvc_mjpeg_name(h->dec), tj ? " (" : "", tj ? lib : "", tj ? ")" : "");
		if (!tj) {
			U_LOG_W(
			    "uvc capture (V4L2): libturbojpeg not loadable (%s) — MJPEG is decoded by the slow "
			    "built-in "
			    "decoder; install the libturbojpeg package for full frame rates",
			    lib);
		}
	} else {
		U_LOG_W("uvc capture (V4L2): started %s %ux%u@%.2f %s — no decode (raw frames)", id, h->width,
		        h->height, h->fps, fcc);
	}
	*out_handle = h;
	return true;
}

static bool
deliver_mjpeg(struct v4l2_handle *h, const uint8_t *data, size_t len, struct u_stereo_uvc_raw_frame *out)
{
	const int64_t t0 = os_monotonic_get_ns();
	bool ok = os_uvc_mjpeg_decode(h->dec, data, len, h->out_w, h->out_h, out);
	h->decode_ns += os_monotonic_get_ns() - t0;
	if (!ok) {
		h->dropped++;
	}
	return ok;
}

static uint32_t
file_read(struct v4l2_handle *h, struct u_stereo_uvc_raw_frame *out)
{
	while (h->next < h->nframes) {
		const uint32_t i = h->next++;
		memset(out, 0, sizeof(*out));
		if (deliver_mjpeg(h, h->file + h->offs[i], h->lens[i], out)) {
			out->time_ns = os_monotonic_get_ns();
			out->time_is_exposure = false;
			h->frames++;
			return U_STEREO_UVC_READ_OK;
		}
	}
	return U_STEREO_UVC_READ_ERROR; // end of stream
}

//! Dequeue everything completed, keep the newest. 1 = got one (@p out), 0 = none, -1 = the device failed.
static int
dequeue_newest(struct v4l2_handle *h, struct v4l2_buffer *out, int *out_errno)
{
	bool have = false;
	for (;;) {
		struct v4l2_buffer b;
		memset(&b, 0, sizeof(b));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b.memory = V4L2_MEMORY_MMAP;
		if (xioctl(h->fd, VIDIOC_DQBUF, &b) < 0) {
			const int e = errno;
			if (e == EAGAIN) {
				break;
			}
			if (e == EIO && ++h->eio_streak < 30) {
				break; // transient per the V4L2 spec; persistent EIO is a failure
			}
			*out_errno = e;
			if (have) {
				requeue(h, out->index);
			}
			return -1;
		}
		h->eio_streak = 0;
		if (b.index >= h->nbufs) {
			continue;
		}
		if (have) {
			requeue(h, out->index); // latest-wins: the older completed frame goes back
			h->superseded++;
		}
		*out = b;
		have = true;
	}
	return have ? 1 : 0;
}

//! Hand buffer @p b over as a raw frame. False = dropped (error flag, empty, short, undecodable).
static bool
deliver(struct v4l2_handle *h, const struct v4l2_buffer *b, struct u_stereo_uvc_raw_frame *out)
{
	const uint8_t *base = (const uint8_t *)h->bufs[b->index].start;
	const size_t used = b->bytesused <= h->bufs[b->index].length ? b->bytesused : h->bufs[b->index].length;
	memset(out, 0, sizeof(*out));
	bool ok = false;
	if ((b->flags & V4L2_BUF_FLAG_ERROR) == 0 && used > 0) {
		if (is_mjpeg(h->fourcc)) {
			ok = deliver_mjpeg(h, base, used, out);
			requeue(h, b->index); // decoded into our own buffer: the driver can have it back now
		} else {
			const size_t plane = (size_t)h->bytesperline * h->height;
			const size_t need = h->fourcc == V4L2_PIX_FMT_NV12 ? plane + plane / 2 : plane;
			if (used >= need) {
				out->pixel =
				    h->fourcc == V4L2_PIX_FMT_NV12 ? U_STEREO_UVC_PIXEL_NV12 : U_STEREO_UVC_PIXEL_YUY2;
				out->width = h->width;
				out->height = h->height;
				out->planes[0] = base;
				out->pitches[0] = h->bytesperline;
				if (h->fourcc == V4L2_PIX_FMT_NV12) {
					out->planes[1] = base + (size_t)h->bytesperline * h->height;
					out->pitches[1] = h->bytesperline;
				}
				h->held = (int)b->index; // stays dequeued until the next read / close
				ok = true;
			} else {
				requeue(h, b->index);
			}
		}
	} else {
		requeue(h, b->index);
	}
	if (!ok) {
		if (!is_mjpeg(h->fourcc)) {
			h->dropped++;
		}
		return false;
	}
	const uint32_t ts = b->flags & V4L2_BUF_FLAG_TIMESTAMP_MASK;
	if (ts == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC && (b->timestamp.tv_sec != 0 || b->timestamp.tv_usec != 0)) {
		// CLOCK_MONOTONIC, the os_monotonic_get_ns() domain.
		out->time_ns = (int64_t)b->timestamp.tv_sec * 1000000000ll + (int64_t)b->timestamp.tv_usec * 1000ll;
		out->time_is_exposure = (b->flags & V4L2_BUF_FLAG_TSTAMP_SRC_MASK) == V4L2_BUF_FLAG_TSTAMP_SRC_SOE;
	} else {
		out->time_ns = os_monotonic_get_ns();
		out->time_is_exposure = false;
	}
	h->frames++;
	return true;
}

static uint32_t
v4l2_read(void *handle, int64_t timeout_ns, struct u_stereo_uvc_raw_frame *out)
{
	struct v4l2_handle *h = (struct v4l2_handle *)handle;
	release_held(h); // the previous frame's planes are released by contract
	if (h->is_file) {
		return file_read(h, out);
	}
	if (h->failed) {
		return U_STEREO_UVC_READ_ERROR;
	}
	const int64_t deadline = os_monotonic_get_ns() + (timeout_ns > 0 ? timeout_ns : 0);
	for (;;) {
		struct v4l2_buffer b;
		int err = 0;
		int got = dequeue_newest(h, &b, &err);
		if (got > 0 && deliver(h, &b, out)) {
			return U_STEREO_UVC_READ_OK;
		}
		bool poll_err = false;
		if (got >= 0) {
			const int64_t left = deadline - os_monotonic_get_ns();
			if (left <= 0) {
				return U_STEREO_UVC_READ_TIMEOUT;
			}
			struct pollfd p = {h->fd, POLLIN, 0};
			int r = poll(&p, 1, (int)((left + 999999) / 1000000));
			if (r < 0 && errno != EINTR) {
				err = errno;
				got = -1;
			} else if (r > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 &&
			           (p.revents & POLLIN) == 0) {
				// Nothing to dequeue and the node reports an error: unplugged or stopped.
				struct v4l2_buffer b2;
				int got2 = dequeue_newest(h, &b2, &err);
				if (got2 > 0) {
					if (deliver(h, &b2, out)) {
						return U_STEREO_UVC_READ_OK;
					}
					continue;
				}
				poll_err = true;
				got = -1;
				if (err == 0) {
					err = ENODEV;
				}
			}
		}
		if (got < 0) {
			h->failed = true;
			U_LOG_W(
			    "uvc capture (V4L2): %s stopped delivering (%s%s) after %llu frame(s); %llu superseded, "
			    "%llu "
			    "dropped",
			    h->id, strerror(err), poll_err ? ", poll error" : "", h->frames, h->superseded, h->dropped);
			return U_STEREO_UVC_READ_ERROR;
		}
	}
}

static void
v4l2_close(void *handle)
{
	struct v4l2_handle *h = (struct v4l2_handle *)handle;
	if (h == NULL) {
		return;
	}
	if (h->frames > 0) {
		if (h->dec != NULL) {
			U_LOG_W(
			    "uvc capture (V4L2): closed %s after %llu frame(s) — decoder %s, mean decode %.2f "
			    "ms/frame; "
			    "%llu superseded, %llu dropped",
			    h->id, h->frames, os_uvc_mjpeg_name(h->dec),
			    (double)h->decode_ns / (double)(h->frames + h->dropped) * 1e-6, h->superseded, h->dropped);
		} else {
			U_LOG_W("uvc capture (V4L2): closed %s after %llu frame(s); %llu superseded, %llu dropped",
			        h->id, h->frames, h->superseded, h->dropped);
		}
	}
	release_held(h);
	teardown(h);
	free(h);
}

bool
os_uvc_capture_backend(struct u_stereo_uvc_backend *out)
{
	memset(out, 0, sizeof(*out));
	out->name = "v4l2";
	out->enumerate = v4l2_enumerate;
	out->list_modes = v4l2_list_modes;
	out->open = v4l2_open;
	out->read = v4l2_read;
	out->close = v4l2_close;
	return true;
}

void
os_uvc_capture_decoder_report(char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return;
	}
	out[0] = '\0';
	const enum decoder_pref pref = read_decoder_pref();
	appendf(out, cap, "decoder policy: %s (DXR_STEREO_CAMERA_UVC_DECODER)\n",
	        pref == DECODER_SOFTWARE   ? "software"
	        : pref == DECODER_HARDWARE ? "hardware only"
	                                   : "auto");
	appendf(out, cap,
	        "V4L2 backend: NV12 / YUYV modes are used as delivered; MJPEG is decoded on the CPU (no hardware "
	        "MJPEG decode on Linux yet)\n");
	char lib[256] = {0};
	const bool tj = os_uvc_mjpeg_turbojpeg_available(lib, sizeof(lib));
	if (tj) {
		appendf(out, cap,
		        "libturbojpeg: %s (decodes to YCbCr planes, DCT-domain downscale towards the output)\n", lib);
	} else {
		appendf(out, cap, "libturbojpeg: not loadable (%s)\n", lib);
	}
	if (pref == DECODER_HARDWARE) {
		appendf(out, cap,
		        "=> MJPEG modes REFUSED (hardware only, none on Linux); NV12 / YUYV modes still open\n");
	} else if (tj) {
		appendf(out, cap, "=> CPU decode with libturbojpeg\n");
	} else {
		appendf(
		    out, cap,
		    "=> CPU decode with the built-in decoder (stb_image, full size, slow — install the libturbojpeg "
		    "package)\n");
	}
}
