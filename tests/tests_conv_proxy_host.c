// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A scripted conversion host for tests_conv_proxy (#2034): speaks the
 *         real service <-> host wire (conv_host_wire.h) over the real channel,
 *         rings and shm, with one built-in CPU capability instead of a
 *         provider DLL. Not the shipping displayxr-conversion-host (#2033).
 *
 * Standard arguments (conv_host_wire.h CONV_HOST_ARG_*) plus:
 *   --fake-warm-ms <ms>            warm duration (default 20)
 *   --fake-describe-ms <ms>        delay every DESCRIBE reply (a slow bring-up)
 *   --fake-crash-after <n>         exit(3) when the n-th convert arrives (never completes it)
 *   --fake-crash-once <file>       ... only when <file> does not exist (it is created first)
 *   --fake-hang-after <n>          stop serving converts at the n-th (the watchdog's case)
 *   --fake-exit-at-start <code>    exit with <code> before saying hello (10 = ABI reject, ...)
 *
 * The capability: cap_id 4, VIEWS_MONO, stream lane, VIEWS output, CPU memory,
 * RGBA8 in; views out = 2 x w by h RGBA8 (ROW): every view is the input with
 * byte 2 (blue) set to 0x40 + view.
 */

#include "os/os_process.h"
#include "conversion/conv_host_wire.h"
#include "util/u_git_tag.h"
#include "util/u_shm_ring.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
static void
sleep_ms(unsigned ms)
{
	Sleep(ms);
}
static uint64_t
mono_ms(void)
{
	return (uint64_t)GetTickCount64();
}
#else
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static void
sleep_ms(unsigned ms)
{
	struct timespec ts = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
	nanosleep(&ts, NULL);
}
static uint64_t
mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
#endif

#define CAP_ID 4u
#define MAX_IMG 64u
#define MAX_OUT 16u

static struct os_channel *g_ch;
static char g_provider[64];
static unsigned g_warm_ms = 20;
static unsigned g_describe_ms = 0;
static unsigned g_crash_after = 0;
static const char *g_crash_once = NULL;
static unsigned g_hang_after = 0;
static uint32_t g_state = XRT_CONV_STATE_INSTALLED;
static uint64_t g_luid = 0;

struct img
{
	bool used;
	uint64_t shm_offset;
	uint64_t bytes;
	uint32_t w, h, stride;
};
static struct img g_img[MAX_IMG];

struct out
{
	bool used;
	uint64_t stream;
	uint32_t index;
	uint8_t *ptr;
	uint64_t bytes;
	uint32_t w, h, stride;
};
static struct out g_out[MAX_OUT];
static uint32_t g_next_out = 1;

static uint8_t *g_lane;
static uint64_t g_lane_bytes;
static struct u_shm_ring g_sub, g_com;
static struct os_wake g_wake_in, g_wake_out;
static volatile int g_lane_up;
static unsigned g_converts;

/* --- shm --- */

static uint8_t *
shm_new(uint64_t bytes, uint64_t *out_handle)
{
#ifdef _WIN32
	HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, (DWORD)(bytes >> 32),
	                              (DWORD)(bytes & 0xFFFFFFFFu), NULL);
	if (h == NULL) {
		return NULL;
	}
	void *p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)bytes);
	*out_handle = (uint64_t)(uintptr_t)h;
	return (uint8_t *)p;
#else
	char name[64];
	snprintf(name, sizeof(name), "/dxr-test-host-%ld-%llu", (long)getpid(), (unsigned long long)mono_ms());
	int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		return NULL;
	}
	shm_unlink(name);
	if (ftruncate(fd, (off_t)bytes) != 0) {
		close(fd);
		return NULL;
	}
	void *p = mmap(NULL, (size_t)bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	*out_handle = (uint64_t)fd;
	return p == MAP_FAILED ? NULL : (uint8_t *)p;
#endif
}

static uint8_t *
shm_map(uint64_t handle, uint64_t bytes)
{
#ifdef _WIN32
	return (uint8_t *)MapViewOfFile((HANDLE)(uintptr_t)handle, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)bytes);
#else
	void *p = mmap(NULL, (size_t)bytes, PROT_READ | PROT_WRITE, MAP_SHARED, (int)handle, 0);
	return p == MAP_FAILED ? NULL : (uint8_t *)p;
#endif
}

/* --- channel --- */

static int
send_frame(uint32_t type, uint32_t request_id, const void *payload, uint32_t bytes, const uint64_t *handles, uint32_t nh)
{
	uint8_t buf[sizeof(struct conv_host_wire_frame) + CONV_HOST_WIRE_MAX_PAYLOAD];
	struct conv_host_wire_frame f;
	memset(&f, 0, sizeof(f));
	f.magic = CONV_HOST_WIRE_MAGIC;
	f.type = type;
	f.payload_bytes = bytes;
	f.request_id = request_id;
	f.handle_count = nh;
#ifdef _WIN32
	for (uint32_t i = 0; i < nh; i++) {
		f.handles[i] = handles[i]; // our values: the service pulls them
	}
	const uint64_t *fds = NULL;
	uint32_t nfds = 0;
#else
	const uint64_t *fds = handles;
	uint32_t nfds = nh;
#endif
	memcpy(buf, &f, sizeof(f));
	memcpy(buf + sizeof(f), payload, bytes);
	return os_channel_send(g_ch, buf, sizeof(f) + bytes, fds, nfds);
}

static void
reply_status(uint32_t type, uint32_t rid, int32_t result)
{
	struct conv_host_wire_status st;
	conv_host_wire_init(&st, sizeof(st));
	st.result = result;
	send_frame(type | CONV_HOST_MSG_REPLY_BIT, rid, &st, sizeof(st), NULL, 0);
}

static uint64_t
frame_handle(const struct conv_host_wire_frame *f, const uint64_t *fds, uint32_t nfds, uint32_t i)
{
#ifdef _WIN32
	(void)fds;
	(void)nfds;
	return i < f->handle_count ? f->handles[i] : OS_NATIVE_HANDLE_INVALID;
#else
	(void)f;
	return i < nfds ? fds[i] : OS_NATIVE_HANDLE_INVALID;
#endif
}

/* --- the capability --- */

static void
fill_cap(struct xrt_conv_capability *c)
{
	memset(c, 0, sizeof(*c));
	c->struct_size = (uint32_t)sizeof(*c);
	c->cap_id = CAP_ID;
	c->task = XRT_CONV_TASK_VIEWS_MONO;
	c->lanes = XRT_CONV_LANE_STREAM;
	c->input_kinds = XRT_CONV_INPUT_MONO;
	c->outputs = XRT_CONV_OUT_VIEWS;
	c->flags = XRT_CONV_CAP_EXPLICIT_VIEWPOINTS;
	c->import_memory_kinds = XRT_CONV_MEM_CPU;
	c->input_formats = 1u << XRT_CONV_FMT_RGBA8_UNORM;
	c->min_views = 2;
	c->max_views = 2;
	c->max_streams = 4;
	c->max_concurrent = 1;
	c->licence_class = XRT_CONV_LICENCE_PERMISSIVE;
	c->commercial_ok = 1;
	snprintf(c->display_name, sizeof(c->display_name), "scripted test host");
	c->est_latency_ms = 2;
	c->est_warmup_ms = 20;
}

/* --- the ring worker --- */

static void
convert(const struct conv_host_wire_submit_record *rec)
{
	struct conv_host_wire_complete_record done;
	memset(&done, 0, sizeof(done));
	done.seq = rec->seq;
	done.out_sync_idx = CONV_HOST_WIRE_NO_INDEX;
	for (uint32_t i = 0; i < CONV_HOST_WIRE_OUT_COUNT; i++) {
		done.out_image_idx[i] = CONV_HOST_WIRE_NO_INDEX;
	}
	const uint32_t si = rec->in_image_idx[0];
	struct out *o = NULL;
	for (uint32_t i = 0; i < MAX_OUT; i++) {
		if (g_out[i].used && g_out[i].stream == rec->stream) {
			o = &g_out[i];
		}
	}
	if (si >= MAX_IMG || !g_img[si].used || o == NULL || g_state != XRT_CONV_STATE_READY) {
		done.status = (int32_t)XRT_ERROR_IPC_FAILURE;
	} else {
		const struct img *in = &g_img[si];
		const uint8_t *src = g_lane + in->shm_offset;
		const uint32_t w = in->w < o->w ? in->w : o->w;
		const uint32_t h = in->h < o->h ? in->h : o->h;
		for (uint32_t v = 0; v < 2; v++) {
			for (uint32_t y = 0; y < h; y++) {
				uint8_t *d = o->ptr + (size_t)y * o->stride + (size_t)v * o->w * 4u;
				memcpy(d, src + (size_t)y * in->stride, (size_t)w * 4u);
				for (uint32_t x = 0; x < w; x++) {
					d[x * 4u + 2u] = (uint8_t)(0x40 + v);
				}
			}
		}
		done.status = 0;
		done.written = XRT_CONV_OUT_VIEWS;
		done.out_image_idx[CONV_HOST_WIRE_OUT_VIEWS] = o->index;
		done.views_info.view_count = 2;
	}
	bool wake = false;
	u_shm_ring_push(&g_com, &done, &wake);
	if (wake) {
		os_wake_signal(&g_wake_out);
	}
}

static void
ring_loop(void)
{
	struct conv_host_wire_submit_record rec;
	for (;;) {
		if (u_shm_ring_pop(&g_sub, &rec) == U_SHM_RING_OK) {
			g_converts++;
			if (g_crash_after != 0 && g_converts >= g_crash_after) {
				if (g_crash_once == NULL) {
					exit(3);
				}
				FILE *f = fopen(g_crash_once, "rb");
				if (f == NULL) {
					f = fopen(g_crash_once, "wb");
					if (f != NULL) {
						fclose(f);
					}
					exit(3);
				}
				fclose(f);
			}
			if (g_hang_after != 0 && g_converts >= g_hang_after) {
				for (;;) {
					sleep_ms(1000); // never completes: the service's watchdog kills us
				}
			}
			convert(&rec);
			continue;
		}
		if (u_shm_ring_prepare_wait(&g_sub)) {
			os_wake_wait(&g_wake_in, 100);
			u_shm_ring_finish_wait(&g_sub);
		}
	}
}

#ifdef _WIN32
static DWORD WINAPI
ring_thread(LPVOID p)
{
	(void)p;
	ring_loop();
	return 0;
}
#else
static void *
ring_thread(void *p)
{
	(void)p;
	ring_loop();
	return NULL;
}
#endif

/* --- control --- */

static void
handle(const struct conv_host_wire_frame *f, const uint8_t *payload, const uint64_t *fds, uint32_t nfds)
{
	const uint32_t rid = f->request_id;
	switch (f->type) {
	case CONV_HOST_MSG_DESCRIBE: {
		if (g_describe_ms != 0) {
			sleep_ms(g_describe_ms); // a slow bring-up (a window to kill the host in)
		}
		struct conv_host_wire_describe_reply r;
		conv_host_wire_init(&r, sizeof(r));
		struct xrt_conv_capability c;
		fill_cap(&c);
		conv_host_wire_pack_capability(&c, &r.capability);
		r.count = 1;
		r.generation = 1;
		send_frame(f->type | CONV_HOST_MSG_REPLY_BIT, rid, &r, sizeof(r), NULL, 0);
		break;
	}
	case CONV_HOST_MSG_GET_STATE: {
		struct conv_host_wire_state_reply r;
		conv_host_wire_init(&r, sizeof(r));
		r.cap_id = CAP_ID;
		struct xrt_conv_state st;
		memset(&st, 0, sizeof(st));
		st.struct_size = (uint32_t)sizeof(st);
		st.state = g_state;
		st.progress = -1.0f;
		conv_host_wire_pack_state(&st, &r.state);
		send_frame(f->type | CONV_HOST_MSG_REPLY_BIT, rid, &r, sizeof(r), NULL, 0);
		break;
	}
	case CONV_HOST_MSG_GET_PROVIDER_STATUS: {
		struct conv_host_wire_provider_status r;
		conv_host_wire_init(&r, sizeof(r));
		r.reported = 1;
		r.state = 1; // XRT_PLUGIN_PLATFORM_STATE_READY
		r.flags = XRT_CONV_PROVIDER_FLAG_FALLBACK;
		conv_host_wire_set_str(r.hint, sizeof(r.hint), "scripted test host");
		send_frame(f->type | CONV_HOST_MSG_REPLY_BIT, rid, &r, sizeof(r), NULL, 0);
		break;
	}
	case CONV_HOST_MSG_WARM:
		sleep_ms(g_warm_ms);
		g_state = XRT_CONV_STATE_READY;
		reply_status(f->type, rid, 0);
		break;
	case CONV_HOST_MSG_EVICT:
		g_state = XRT_CONV_STATE_INSTALLED;
		reply_status(f->type, rid, 0);
		break;
	case CONV_HOST_MSG_STREAM_OPEN:
	case CONV_HOST_MSG_STREAM_RESET:
	case CONV_HOST_MSG_STREAM_CLOSE:
	case CONV_HOST_MSG_UNREGISTER_IMAGE:
	case CONV_HOST_MSG_REGISTER_SYNC: reply_status(f->type, rid, 0); break;
	case CONV_HOST_MSG_GET_OUTPUT_DESC: {
		struct conv_host_wire_output_desc_request q;
		conv_host_wire_receive(&q, sizeof(q), payload, f->payload_bytes);
		struct conv_host_wire_output_desc_reply r;
		conv_host_wire_init(&r, sizeof(r));
		r.views_width = q.width * 2u;
		r.views_height = q.height;
		r.views_format = XRT_CONV_FMT_RGBA8_UNORM;
		send_frame(f->type | CONV_HOST_MSG_REPLY_BIT, rid, &r, sizeof(r), NULL, 0);
		break;
	}
	case CONV_HOST_MSG_REGISTER_IMAGE: {
		struct conv_host_wire_register_image ri;
		conv_host_wire_receive(&ri, sizeof(ri), payload, f->payload_bytes);
		int32_t res = 0;
		if (ri.index >= MAX_IMG || ri.image.memory_kind != XRT_CONV_MEM_CPU ||
		    ri.image.shm_offset + ri.image.byte_size > g_lane_bytes) {
			res = (int32_t)XRT_ERROR_IPC_FAILURE;
		} else {
			struct img *im = &g_img[ri.index];
			im->used = true;
			im->shm_offset = ri.image.shm_offset;
			im->bytes = ri.image.byte_size;
			im->w = ri.image.width;
			im->h = ri.image.height;
			im->stride = ri.image.row_stride != 0 ? ri.image.row_stride : ri.image.width * 4u;
		}
		reply_status(f->type, rid, res);
		break;
	}
	case CONV_HOST_MSG_ALLOC_OUTPUTS: {
		struct conv_host_wire_alloc_outputs ao;
		conv_host_wire_receive(&ao, sizeof(ao), payload, f->payload_bytes);
		for (uint32_t i = 0; i < MAX_OUT; i++) {
			if (g_out[i].used && g_out[i].stream == ao.stream) {
				g_out[i].used = false; // released after the reply (the mapping leaks: test host)
			}
		}
		struct out *o = NULL;
		for (uint32_t i = 0; i < MAX_OUT && o == NULL; i++) {
			if (!g_out[i].used) {
				o = &g_out[i];
			}
		}
		uint64_t h = OS_NATIVE_HANDLE_INVALID;
		const uint32_t w = ao.width * 2u;
		const uint64_t bytes = (uint64_t)w * ao.height * 4u;
		uint8_t *ptr = o != NULL ? shm_new(bytes, &h) : NULL;
		if (ptr == NULL) {
			reply_status(f->type, rid, (int32_t)XRT_ERROR_ALLOCATION);
			break;
		}
		o->used = true;
		o->stream = ao.stream;
		o->index = g_next_out++;
		o->ptr = ptr;
		o->bytes = bytes;
		o->w = ao.width;
		o->h = ao.height;
		o->stride = w * 4u;
		struct conv_host_wire_output_image oi;
		conv_host_wire_init(&oi, sizeof(oi));
		oi.stream = ao.stream;
		oi.index = o->index;
		oi.role = CONV_HOST_WIRE_OUT_VIEWS;
		oi.image.memory_kind = XRT_CONV_MEM_CPU;
		oi.image.role = XRT_CONV_OUT_VIEWS;
		oi.image.width = w;
		oi.image.height = ao.height;
		oi.image.format = XRT_CONV_FMT_RGBA8_UNORM;
		oi.image.row_stride = w * 4u;
		oi.image.byte_size = bytes;
		oi.image.shm_offset = 0;
		send_frame(CONV_HOST_MSG_OUTPUT_IMAGE, rid, &oi, sizeof(oi), &h, 1);
		reply_status(f->type, rid, 0);
		break;
	}
	case CONV_HOST_MSG_LANE_OPEN: {
		struct conv_host_wire_lane_open lo;
		conv_host_wire_receive(&lo, sizeof(lo), payload, f->payload_bytes);
		const uint64_t shm = frame_handle(f, fds, nfds, 0);
		g_lane = shm_map(shm, lo.shm_bytes);
		g_lane_bytes = lo.shm_bytes;
		const size_t sub = u_shm_ring_bytes(lo.submit_record_bytes, lo.ring_capacity);
		const size_t com = u_shm_ring_bytes(lo.complete_record_bytes, lo.ring_capacity);
		int32_t res = (int32_t)XRT_ERROR_IPC_FAILURE;
		if (g_lane != NULL && lo.submit_record_bytes == sizeof(struct conv_host_wire_submit_record) &&
		    lo.complete_record_bytes == sizeof(struct conv_host_wire_complete_record) &&
		    u_shm_ring_attach(&g_sub, g_lane + lo.submit_ring_offset, sub, lo.submit_record_bytes,
		                      lo.ring_capacity) &&
		    u_shm_ring_attach(&g_com, g_lane + lo.complete_ring_offset, com, lo.complete_record_bytes,
		                      lo.ring_capacity)) {
			os_wake_from_native(&g_wake_in, frame_handle(f, fds, nfds, 1));
			os_wake_from_native(&g_wake_out, frame_handle(f, fds, nfds, 2));
			res = 0;
			if (!g_lane_up) {
				g_lane_up = 1;
#ifdef _WIN32
				CloseHandle(CreateThread(NULL, 0, ring_thread, NULL, 0, NULL));
#else
				pthread_t t;
				pthread_create(&t, NULL, ring_thread, NULL);
				pthread_detach(t);
#endif
			}
		}
		reply_status(f->type, rid, res);
		break;
	}
	case CONV_HOST_MSG_SHUTDOWN:
		reply_status(f->type, rid, 0);
		exit(0);
	default:
		if (rid != 0) {
			reply_status(f->type, rid, (int32_t)XRT_ERROR_FEATURE_NOT_SUPPORTED);
		}
		break;
	}
}

int
main(int argc, char **argv)
{
	const char *token = NULL;
	long parent = 0;
	for (int i = 1; i + 1 < argc; i += 2) {
		const char *k = argv[i], *v = argv[i + 1];
		if (strcmp(k, CONV_HOST_ARG_PROVIDER) == 0) {
			snprintf(g_provider, sizeof(g_provider), "%s", v);
		} else if (strcmp(k, CONV_HOST_ARG_CHANNEL) == 0) {
			token = v;
		} else if (strcmp(k, CONV_HOST_ARG_ADAPTER_LUID) == 0) {
			g_luid = strtoull(v, NULL, 16);
		} else if (strcmp(k, CONV_HOST_ARG_PARENT) == 0) {
			parent = strtol(v, NULL, 10);
		} else if (strcmp(k, "--fake-describe-ms") == 0) {
			g_describe_ms = (unsigned)strtoul(v, NULL, 10);
		} else if (strcmp(k, "--fake-warm-ms") == 0) {
			g_warm_ms = (unsigned)strtoul(v, NULL, 10);
		} else if (strcmp(k, "--fake-crash-after") == 0) {
			g_crash_after = (unsigned)strtoul(v, NULL, 10);
		} else if (strcmp(k, "--fake-crash-once") == 0) {
			g_crash_once = v;
		} else if (strcmp(k, "--fake-hang-after") == 0) {
			g_hang_after = (unsigned)strtoul(v, NULL, 10);
		} else if (strcmp(k, "--fake-exit-at-start") == 0) {
			return (int)strtol(v, NULL, 10);
		}
	}
	if (token == NULL || os_channel_open_child(token, &g_ch) != 0) {
		return 11;
	}
	if (parent != 0) {
		os_process_watch_parent(parent, 0);
	}
	struct conv_host_wire_hello hello;
	conv_host_wire_init(&hello, sizeof(hello));
	hello.host_pid = (uint32_t)os_process_current_pid();
	hello.provider_api_version = XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT;
	hello.capability_count = 1;
	hello.generation = 1;
	hello.adapter_luid = g_luid; // a CPU provider would say 0; echoed so the test sees what it was given
	conv_host_wire_set_str(hello.git_tag, sizeof(hello.git_tag), u_git_tag);
	conv_host_wire_set_str(hello.provider_id, sizeof(hello.provider_id), g_provider);
	if (send_frame(CONV_HOST_MSG_HELLO, 0, &hello, sizeof(hello), NULL, 0) != 0) {
		return 11;
	}
	uint64_t last_hb = mono_ms();
	uint8_t payload[CONV_HOST_WIRE_MAX_PAYLOAD];
	for (;;) {
		struct conv_host_wire_frame f;
		uint64_t fds[CONV_HOST_WIRE_MAX_HANDLES];
		uint32_t nfds = CONV_HOST_WIRE_MAX_HANDLES;
		const int r = os_channel_recv(g_ch, &f, sizeof(f), fds, &nfds, 200);
		if (mono_ms() - last_hb >= 1000) {
			last_hb = mono_ms();
			struct conv_host_wire_heartbeat hb;
			conv_host_wire_init(&hb, sizeof(hb));
			hb.exec_threads = 1;
			send_frame(CONV_HOST_MSG_HEARTBEAT, 0, &hb, sizeof(hb), NULL, 0);
		}
		if (r == 0) {
			continue;
		}
		if (r != 1 || !conv_host_wire_frame_valid(&f)) {
			return 0; // the service went away
		}
		uint32_t none = 0;
		if (f.payload_bytes > 0 && os_channel_recv(g_ch, payload, f.payload_bytes, NULL, &none, 1000) != 1) {
			return 0;
		}
		if (f.type == CONV_HOST_MSG_HELLO_ACK) {
			struct conv_host_wire_hello_ack ack;
			conv_host_wire_receive(&ack, sizeof(ack), payload, f.payload_bytes);
			if (!ack.accepted) {
				return CONV_HOST_EXIT_ABI_REJECT;
			}
			continue;
		}
		handle(&f, payload, fds, nfds);
	}
}
