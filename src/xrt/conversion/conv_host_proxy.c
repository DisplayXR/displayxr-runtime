// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The satellite host proxy (see conv_host_proxy.h).
 *
 * Locks: p->m guards everything below except the rings / scratch / host
 * output mappings (worker only) and the channel's send side (p->send_mtx,
 * leaf). Neither is held across a wait for the host, and on_complete /
 * on_event are called with no proxy lock held.
 *
 * Epoch: bumped under p->m the moment a host loss is seen and BEFORE any
 * pending request, queued or in-flight command is failed (conv_host_iface.h).
 *
 * @ingroup aux_conversion
 */

#include "conv_host_proxy.h"
#include "conv_host_wire.h"

#include "os/os_process.h"
#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_git_tag.h"
#include "util/u_logging.h"
#include "util/u_shm_ring.h"

#include "xrt/xrt_config_os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#include "xrt/xrt_windows.h"
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif


/*
 *
 * Limits and timings.
 *
 */

#define PX_MAX_IMAGES 256u   //!< registered image slots per incarnation
#define PX_MAX_PENDING 32u   //!< control requests in flight
#define PX_MAX_STREAMS 64u   //!< provider streams per incarnation
#define PX_MAX_OUTPUTS 128u  //!< host outputs per incarnation (sets x roles)
#define PX_MAX_SYNCS 32u     //!< host output fences per incarnation
#define PX_IDX_BITS 16u      //!< image index = epoch << 16 | slot
#define PX_SCRATCH_DEFAULT (64ull * 1024ull * 1024ull)
#define PX_ALIGN 4096ull

#define PX_REQUEST_TIMEOUT_MS 5000u    //!< ordinary control requests
#define PX_ACCEPT_TIMEOUT_MS 10000u    //!< the host connecting its channel (a start costs ~4 s on a slow box)
#define PX_SLICE_MS 50u                //!< worker / supervisor wait slice
#define PX_FENCE_TIMEOUT_MS 2000u      //!< CPU fence observe per result
#define PX_MAINTENANCE_POLL_NS (1000ull * 1000000ull)

static uint64_t
now_ns(void)
{
	return os_monotonic_get_ns();
}


/*
 *
 * Shared memory (the lane: rings + scratch; host CPU outputs).
 *
 */

struct px_shm
{
	uint64_t handle; //!< native (HANDLE / fd); OS_NATIVE_HANDLE_INVALID = none
	uint8_t *ptr;
	uint64_t bytes;
};

static void
px_shm_close(struct px_shm *s)
{
#ifdef XRT_OS_WINDOWS
	if (s->ptr != NULL) {
		UnmapViewOfFile(s->ptr);
	}
#else
	if (s->ptr != NULL) {
		munmap(s->ptr, (size_t)s->bytes);
	}
#endif
	if (s->handle != OS_NATIVE_HANDLE_INVALID && s->handle != 0) {
		os_native_handle_close(s->handle);
	}
	s->handle = OS_NATIVE_HANDLE_INVALID;
	s->ptr = NULL;
	s->bytes = 0;
}

static bool
px_shm_create(struct px_shm *s, uint64_t bytes)
{
	s->handle = OS_NATIVE_HANDLE_INVALID;
	s->ptr = NULL;
	s->bytes = bytes;
#ifdef XRT_OS_WINDOWS
	HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, (DWORD)(bytes >> 32),
	                              (DWORD)(bytes & 0xFFFFFFFFu), NULL);
	if (h == NULL) {
		return false;
	}
	void *p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)bytes);
	if (p == NULL) {
		CloseHandle(h);
		return false;
	}
	s->handle = (uint64_t)(uintptr_t)h;
	s->ptr = (uint8_t *)p;
#else
	char name[64];
	(void)snprintf(name, sizeof(name), "/dxr-conv-%ld-%llu", os_process_current_pid(),
	               (unsigned long long)now_ns());
	int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		return false;
	}
	(void)shm_unlink(name);
	(void)fcntl(fd, F_SETFD, FD_CLOEXEC);
	if (ftruncate(fd, (off_t)bytes) != 0) {
		close(fd);
		return false;
	}
	void *p = mmap(NULL, (size_t)bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		close(fd);
		return false;
	}
	s->handle = (uint64_t)fd;
	s->ptr = (uint8_t *)p;
#endif
	return true;
}

//! Map a received section (takes ownership of @p handle). @p bytes 0 = the whole section.
static bool
px_shm_map(struct px_shm *s, uint64_t handle, uint64_t bytes)
{
	s->handle = handle;
	s->ptr = NULL;
	s->bytes = bytes;
#ifdef XRT_OS_WINDOWS
	void *p = MapViewOfFile((HANDLE)(uintptr_t)handle, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)bytes);
	if (p == NULL) {
		px_shm_close(s);
		return false;
	}
	if (bytes == 0) {
		MEMORY_BASIC_INFORMATION mbi;
		if (VirtualQuery(p, &mbi, sizeof(mbi)) != 0) {
			s->bytes = (uint64_t)mbi.RegionSize;
		}
	}
	s->ptr = (uint8_t *)p;
#else
	if (bytes == 0) {
		struct stat sb;
		if (fstat((int)handle, &sb) != 0 || sb.st_size <= 0) {
			px_shm_close(s);
			return false;
		}
		s->bytes = (uint64_t)sb.st_size;
	}
	void *p = mmap(NULL, (size_t)s->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, (int)handle, 0);
	if (p == MAP_FAILED) {
		px_shm_close(s);
		return false;
	}
	s->ptr = (uint8_t *)p;
#endif
	return true;
}


/*
 *
 * State.
 *
 */

struct px_pending
{
	bool used;
	bool done;
	uint32_t request_id;
	uint32_t type; //!< request type (the reply is type | REPLY_BIT)
	int32_t result;
	uint32_t reply_bytes;
	uint8_t reply[CONV_HOST_WIRE_MAX_PAYLOAD];
	uint64_t handle; //!< a pulled handle the reply carried (job blob); OS_NATIVE_HANDLE_INVALID = none
};

struct px_image
{
	bool used;
	struct xrt_conv_image img; //!< the service's view (cpu_ptr / handle are the service's)
	bool pushed;               //!< REGISTER_IMAGE accepted by this incarnation
	uint64_t scratch_off;      //!< CPU: place in the lane scratch
	uint64_t scratch_len;
};

struct px_output
{
	bool used;
	uint64_t stream;
	uint32_t index; //!< host-chosen
	uint32_t role;  //!< CONV_HOST_WIRE_OUT_*
	uint32_t request_id;
	struct xrt_conv_image img;
	uint64_t shm_offset;
	struct px_shm shm;   //!< CPU: the host's section, mapped
	void *gpu_ctx;       //!< GPU: opened by gpu->open_output
	uint64_t gpu_handle; //!< pulled, not yet opened (worker opens it)
};

struct px_sync
{
	bool used;
	uint32_t index;
	uint32_t kind;
	uint64_t handle; //!< pulled, not yet opened
	void *fence;     //!< gpu->open_fence
};

struct px_stream
{
	bool used;
	uint64_t id; //!< service id (cmd->stream)
	uint32_t cap_id;
	uint32_t out_w, out_h;      //!< extent the current output set was allocated for (0 = none)
	uint32_t out_request_id;    //!< the ALLOC_OUTPUTS whose set is current
	bool desc_cached;
	uint32_t desc_w, desc_h;
	struct xrt_conv_output_desc desc;
};

struct proxy
{
	struct conv_host base;
	struct conv_host_proxy_config cfg;
	char provider_id[CONV_HOST_WIRE_TAG_MAX];
	char provider_binary[1024];
	char host_exe[1024];
	char log_path[1024];
	struct conv_host_proxy_gpu_ops gpu;
	bool have_gpu;

	struct os_mutex m;
	struct os_cond cond;      //!< state changes (replies, running, loss, queue)
	struct os_mutex send_mtx; //!< leaf: one sender on the channel

	/* Supervisor. */
	struct conv_supervisor sup;
	struct os_thread sup_thread;
	bool sup_started;
	bool stop;          //!< destroy requested
	bool breach;        //!< watchdog breach to report
	bool chan_broken;   //!< reader saw the channel break
	bool hello_ok;      //!< accepted hello, bring-up pending
	uint32_t hello_caps;
	bool ever_lost;     //!< a LOST was raised (RESTORED owed at the next bring-up)
	bool restart_owed;  //!< an abnormal loss: respawn after the backoff even with no command queued
	bool want_describe; //!< no cached describe: spawn once to describe (conv_host_proxy_request_describe)
	bool luid_restart;  //!< the adapter LUID changed under a live host: respawn it on the new one
	bool luid_kill_sent; //!< ... and the supervisor thread already ended it (exit 20)
	uint64_t spawned_luid; //!< --adapter-luid the current / last host was given
	uint64_t hello_luid;   //!< the adapter the host says it opened
	bool status_known;
	struct xrt_conv_provider_status provider_status;
	uint32_t raised_terminal; //!< supervisor FAILED already reported (by its failed_at_ns)
	uint64_t raised_terminal_at;
	uint64_t next_maintenance_ns;

	/* The current incarnation. */
	uint32_t epoch;
	bool running; //!< bring-up done: describe cached, lane open
	struct os_process *proc;
	struct os_channel *ch;
	struct os_thread reader;
	bool reader_started;
	bool reader_stop;
	long host_pid;

	/* [C] caches (survive incarnations; replaced by each bring-up). */
	bool described;
	uint64_t generation; //!< bumped at every bring-up and STATE_CHANGED(0)
	uint32_t cap_count;
	struct xrt_conv_capability caps[XRT_CONV_MAX_CAPABILITIES];
	struct xrt_conv_state states[XRT_CONV_MAX_CAPABILITIES];
	bool redescribe;
	struct conv_host_heartbeat hb_host; //!< the host's last heartbeat (diagnostics)

	/* Requests. */
	uint32_t next_request_id;
	struct px_pending pending[PX_MAX_PENDING];

	/* Tables (per incarnation; cleared on loss). */
	struct px_image images[PX_MAX_IMAGES];
	struct px_stream streams[PX_MAX_STREAMS];
	struct px_output outputs[PX_MAX_OUTPUTS];
	struct px_sync syncs[PX_MAX_SYNCS];
	uint64_t next_stream_id;

	/* Lane (worker only once running). */
	struct px_shm lane;
	struct u_shm_ring submit_ring, complete_ring;
	struct os_wake wake_to_host, wake_from_host;
	bool wakes_open;
	uint64_t scratch_base, scratch_bytes;
	uint64_t seq;

	/* Command queue + worker. */
	struct conv_host_cmd *head, *tail;
	uint32_t queued;
	struct os_thread worker;
	bool worker_started;
	uint32_t open_streams;
	bool in_flight;
	uint32_t cur_op, cur_cap;
	uint64_t cur_since_ns;
	uint64_t calls_total;

	/* The job in flight (JOB_PROGRESS routing). */
	uint64_t next_job_id;
	uint64_t cur_job_id;
	const struct xrt_conv_job_control *cur_job_ctl;
};

static inline struct proxy *
px(struct conv_host *h)
{
	return (struct proxy *)h;
}


/*
 *
 * Channel.
 *
 */

/*!
 * Send one frame. @p handles are THIS process's handles: on Windows they are
 * duplicated into the host and their host values written into the frame; on
 * POSIX they ride as SCM_RIGHTS. Borrowed either way.
 */
static bool
px_send(struct proxy *p,
        uint32_t type,
        uint32_t request_id,
        const void *payload,
        uint32_t bytes,
        const uint64_t *handles,
        uint32_t handle_count)
{
	if (bytes > CONV_HOST_WIRE_MAX_PAYLOAD || handle_count > CONV_HOST_WIRE_MAX_HANDLES) {
		return false;
	}
	uint8_t buf[sizeof(struct conv_host_wire_frame) + CONV_HOST_WIRE_MAX_PAYLOAD];
	struct conv_host_wire_frame f;
	memset(&f, 0, sizeof(f));
	f.magic = CONV_HOST_WIRE_MAGIC;
	f.type = type;
	f.payload_bytes = bytes;
	f.request_id = request_id;
	f.handle_count = handle_count;

	os_mutex_lock(&p->m);
	struct os_channel *ch = p->ch;
	struct os_process *proc = p->proc;
	os_mutex_unlock(&p->m);
	if (ch == NULL || proc == NULL) {
		return false;
	}
	os_mutex_lock(&p->send_mtx);
#ifdef XRT_OS_WINDOWS
	for (uint32_t i = 0; i < handle_count; i++) {
		uint64_t remote = 0;
		if (os_process_dup_to_child(proc, handles[i], &remote) != 0) {
			os_mutex_unlock(&p->send_mtx);
			return false;
		}
		f.handles[i] = remote;
	}
	const uint64_t *fds = NULL;
	const uint32_t nfds = 0;
#else
	const uint64_t *fds = handles;
	const uint32_t nfds = handle_count;
#endif
	memcpy(buf, &f, sizeof(f));
	if (bytes > 0) {
		memcpy(buf + sizeof(f), payload, bytes);
	}
	const int r = os_channel_send(ch, buf, sizeof(f) + bytes, fds, nfds);
	os_mutex_unlock(&p->send_mtx);
	return r == 0;
}

static struct px_pending *
pending_find_locked(struct proxy *p, uint32_t request_id)
{
	for (uint32_t i = 0; i < PX_MAX_PENDING; i++) {
		if (p->pending[i].used && p->pending[i].request_id == request_id) {
			return &p->pending[i];
		}
	}
	return NULL;
}

/*!
 * Send a request and wait for its reply (copied into @p reply, at most
 * @p reply_size). Returns the reply's result field (offset 8 of every reply
 * payload), XRT_ERROR_IPC_FAILURE when the host went away (the epoch moved) or
 * the channel failed, XRT_TIMEOUT after @p timeout_ms (0 = wait for ever,
 * still ended by a loss). @p cancel, polled every slice, sends CANCEL once.
 * Never call it on the reader thread.
 */
static xrt_result_t
px_request(struct proxy *p,
           uint32_t type,
           const void *payload,
           uint32_t bytes,
           const uint64_t *handles,
           uint32_t handle_count,
           void *reply,
           uint32_t reply_size,
           uint32_t timeout_ms,
           bool (*cancel)(void *),
           void *cancel_ctx,
           uint32_t *out_request_id,
           uint64_t cancel_job_id,
           uint64_t *out_handle)
{
	if (out_handle != NULL) {
		*out_handle = OS_NATIVE_HANDLE_INVALID;
	}
	os_mutex_lock(&p->m);
	if (p->proc == NULL || p->ch == NULL) {
		os_mutex_unlock(&p->m);
		return XRT_ERROR_IPC_FAILURE;
	}
	struct px_pending *pd = NULL;
	for (uint32_t i = 0; i < PX_MAX_PENDING; i++) {
		if (!p->pending[i].used) {
			pd = &p->pending[i];
			break;
		}
	}
	if (pd == NULL) {
		os_mutex_unlock(&p->m);
		return XRT_ERROR_IPC_FAILURE;
	}
	const uint32_t epoch = p->epoch;
	if (++p->next_request_id == 0) {
		p->next_request_id = 1;
	}
	const uint32_t rid = p->next_request_id;
	memset(pd, 0, sizeof(*pd));
	pd->used = true;
	pd->request_id = rid;
	pd->type = type;
	pd->handle = OS_NATIVE_HANDLE_INVALID;
	if (out_request_id != NULL) {
		*out_request_id = rid;
	}
	os_mutex_unlock(&p->m);

	if (!px_send(p, type, rid, payload, bytes, handles, handle_count)) {
		os_mutex_lock(&p->m);
		pd->used = false;
		p->chan_broken = p->epoch == epoch ? true : p->chan_broken;
		os_cond_broadcast(&p->cond);
		os_mutex_unlock(&p->m);
		return XRT_ERROR_IPC_FAILURE;
	}

	const uint64_t deadline = timeout_ms != 0 ? now_ns() + (uint64_t)timeout_ms * 1000000ull : 0;
	bool cancel_sent = false;
	xrt_result_t r = XRT_TIMEOUT;
	os_mutex_lock(&p->m);
	for (;;) {
		if (pd->done) {
			r = (xrt_result_t)pd->result;
			if (out_handle != NULL) {
				*out_handle = pd->handle;
				pd->handle = OS_NATIVE_HANDLE_INVALID;
			}
			if (reply != NULL && reply_size > 0) {
				const uint32_t n = pd->reply_bytes < reply_size ? pd->reply_bytes : reply_size;
				memset(reply, 0, reply_size);
				memcpy(reply, pd->reply, n);
			}
			break;
		}
		if (p->epoch != epoch) {
			r = XRT_ERROR_IPC_FAILURE;
			break;
		}
		if (deadline != 0 && now_ns() >= deadline) {
			r = XRT_TIMEOUT;
			break;
		}
		if (cancel != NULL && !cancel_sent) {
			os_mutex_unlock(&p->m);
			const bool c = cancel(cancel_ctx);
			if (c && cancel_job_id != 0) {
				struct conv_host_wire_job_ref jr;
				conv_host_wire_init(&jr, sizeof(jr));
				jr.job_id = cancel_job_id;
				(void)px_send(p, CONV_HOST_MSG_JOB_CANCEL, 0, &jr, sizeof(jr), NULL, 0);
				cancel_sent = true;
			} else if (c) {
				struct conv_host_wire_cancel cw;
				conv_host_wire_init(&cw, sizeof(cw));
				cw.request_id = rid;
				(void)px_send(p, CONV_HOST_MSG_CANCEL, 0, &cw, sizeof(cw), NULL, 0);
				cancel_sent = true;
			}
			os_mutex_lock(&p->m);
			continue;
		}
		os_cond_wait_timeout_ns(&p->cond, &p->m, (uint64_t)PX_SLICE_MS * 1000000ull);
	}
	if (pd->handle != OS_NATIVE_HANDLE_INVALID) {
		os_native_handle_close(pd->handle); // nobody asked for it
		pd->handle = OS_NATIVE_HANDLE_INVALID;
	}
	pd->used = false;
	os_mutex_unlock(&p->m);
	return r;
}

//! Read the result field of a reply payload (offset 8, every reply type).
static int32_t
reply_result(const uint8_t *payload, uint32_t bytes)
{
	int32_t r = (int32_t)XRT_ERROR_IPC_FAILURE;
	if (bytes >= 12) {
		memcpy(&r, payload + 8, sizeof(r));
	}
	return r;
}


/*
 *
 * Reader thread (one per incarnation): replies + the host's unsolicited messages.
 *
 */

//! Pull a handle the host announced (Windows: its value; POSIX: the fd that arrived).
static uint64_t
pull_handle(struct proxy *p, const struct conv_host_wire_frame *f, const uint64_t *fds, uint32_t nfds, uint32_t i)
{
#ifdef XRT_OS_WINDOWS
	(void)fds;
	(void)nfds;
	if (i >= f->handle_count || p->proc == NULL) {
		return OS_NATIVE_HANDLE_INVALID;
	}
	uint64_t local = OS_NATIVE_HANDLE_INVALID;
	if (os_process_dup_from_child(p->proc, f->handles[i], true, &local) != 0) {
		return OS_NATIVE_HANDLE_INVALID;
	}
	return local;
#else
	(void)p;
	(void)f;
	return i < nfds ? fds[i] : OS_NATIVE_HANDLE_INVALID;
#endif
}

static void
handle_hello(struct proxy *p, const uint8_t *payload, uint32_t bytes)
{
	struct conv_host_wire_hello h;
	struct conv_host_wire_hello_ack ack;
	conv_host_wire_init(&ack, sizeof(ack));
	conv_host_wire_set_str(ack.git_tag, sizeof(ack.git_tag), u_git_tag);
	uint32_t reject = CONV_HOST_REJECT_NONE;
	if (!conv_host_wire_receive(&h, sizeof(h), payload, bytes)) {
		reject = CONV_HOST_REJECT_FAMILY;
	} else if (strncmp(h.git_tag, u_git_tag, sizeof(h.git_tag)) != 0) {
		reject = CONV_HOST_REJECT_GIT_TAG;
	} else if (strncmp(h.provider_id, p->provider_id, sizeof(h.provider_id)) != 0) {
		reject = CONV_HOST_REJECT_PROVIDER;
	} else if (p->proc != NULL && (long)h.host_pid != os_process_pid(p->proc)) {
		reject = CONV_HOST_REJECT_PEER;
	}
	ack.accepted = reject == CONV_HOST_REJECT_NONE ? 1u : 0u;
	ack.reject_reason = reject;
	(void)px_send(p, CONV_HOST_MSG_HELLO_ACK, 0, &ack, sizeof(ack), NULL, 0);
	if (reject != CONV_HOST_REJECT_NONE) {
		U_LOG_W("[conv:%s] host hello rejected (reason %u; host tag '%.64s', ours '%s')", p->provider_id, reject,
		        h.git_tag, u_git_tag);
	}
	os_mutex_lock(&p->m);
	conv_supervisor_hello(&p->sup, now_ns(), reject == CONV_HOST_REJECT_NONE);
	if (reject == CONV_HOST_REJECT_NONE) {
		p->hello_ok = true;
		p->hello_caps = h.capability_count;
		p->hello_luid = h.adapter_luid;
		if (p->spawned_luid != 0 && h.adapter_luid != 0 && h.adapter_luid != p->spawned_luid) {
			U_LOG_W("[conv:%s] host opened adapter %llx, not the %llx it was given", p->provider_id,
			        (unsigned long long)h.adapter_luid, (unsigned long long)p->spawned_luid);
		}
	}
	os_cond_broadcast(&p->cond);
	os_mutex_unlock(&p->m);
}

static void
handle_output_image(struct proxy *p,
                    const struct conv_host_wire_frame *f,
                    const uint8_t *payload,
                    uint32_t bytes,
                    const uint64_t *fds,
                    uint32_t nfds)
{
	struct conv_host_wire_output_image oi;
	if (!conv_host_wire_receive(&oi, sizeof(oi), payload, bytes) || oi.role >= CONV_HOST_WIRE_OUT_COUNT) {
		return;
	}
	const uint64_t h = pull_handle(p, f, fds, nfds, 0);
	struct px_output o;
	memset(&o, 0, sizeof(o));
	o.used = true;
	o.stream = oi.stream;
	o.index = oi.index;
	o.role = oi.role;
	o.request_id = f->request_id;
	o.shm.handle = OS_NATIVE_HANDLE_INVALID;
	o.gpu_handle = OS_NATIVE_HANDLE_INVALID;
	xrt_conv_wire_unpack_image(&oi.image, &o.img, NULL, &o.shm_offset);
	if (o.img.memory_kind == XRT_CONV_MEM_CPU) {
		if (h == OS_NATIVE_HANDLE_INVALID || !px_shm_map(&o.shm, h, 0) ||
		    o.shm_offset + o.img.byte_size > o.shm.bytes) {
			px_shm_close(&o.shm);
			U_LOG_W("[conv:%s] host output %u: unusable CPU section", p->provider_id, oi.index);
			return;
		}
	} else {
		o.gpu_handle = h; // opened by the worker (gpu ops run there)
	}
	os_mutex_lock(&p->m);
	struct px_output *slot = NULL;
	for (uint32_t i = 0; i < PX_MAX_OUTPUTS && slot == NULL; i++) {
		if (!p->outputs[i].used) {
			slot = &p->outputs[i];
		}
	}
	if (slot != NULL) {
		*slot = o;
	}
	os_mutex_unlock(&p->m);
	if (slot == NULL) {
		px_shm_close(&o.shm);
		os_native_handle_close(o.gpu_handle);
	}
}

static void
handle_sync(struct proxy *p,
            const struct conv_host_wire_frame *f,
            const uint8_t *payload,
            uint32_t bytes,
            const uint64_t *fds,
            uint32_t nfds)
{
	struct conv_host_wire_sync s;
	if (!conv_host_wire_receive(&s, sizeof(s), payload, bytes)) {
		return;
	}
	const uint64_t h = pull_handle(p, f, fds, nfds, 0);
	os_mutex_lock(&p->m);
	struct px_sync *slot = NULL;
	for (uint32_t i = 0; i < PX_MAX_SYNCS && slot == NULL; i++) {
		if (!p->syncs[i].used) {
			slot = &p->syncs[i];
		}
	}
	if (slot != NULL) {
		memset(slot, 0, sizeof(*slot));
		slot->used = true;
		slot->index = s.index;
		slot->kind = s.kind;
		slot->handle = h;
	}
	os_mutex_unlock(&p->m);
	if (slot == NULL) {
		os_native_handle_close(h);
	}
}

static void
handle_frame(struct proxy *p,
             const struct conv_host_wire_frame *f,
             const uint8_t *payload,
             const uint64_t *fds,
             uint32_t nfds)
{
	const uint32_t bytes = f->payload_bytes;
	if ((f->type & CONV_HOST_MSG_REPLY_BIT) != 0) {
		const bool has_handle = f->handle_count > 0 || nfds > 0;
		const uint64_t h = has_handle ? pull_handle(p, f, fds, nfds, 0) : OS_NATIVE_HANDLE_INVALID;
		bool kept = false;
		os_mutex_lock(&p->m);
		struct px_pending *pd = pending_find_locked(p, f->request_id);
		if (pd != NULL && (pd->type | CONV_HOST_MSG_REPLY_BIT) == f->type) {
			pd->reply_bytes = bytes;
			memcpy(pd->reply, payload, bytes);
			pd->result = reply_result(payload, bytes);
			pd->handle = h;
			kept = true;
			pd->done = true;
			os_cond_broadcast(&p->cond);
		}
		os_mutex_unlock(&p->m);
		if (!kept && h != OS_NATIVE_HANDLE_INVALID) {
			os_native_handle_close(h);
		}
		for (uint32_t i = 1; i < nfds; i++) {
			os_native_handle_close(fds[i]);
		}
		return;
	}
	switch (f->type) {
	case CONV_HOST_MSG_HELLO: handle_hello(p, payload, bytes); break;
	case CONV_HOST_MSG_LOG: {
		struct conv_host_wire_log lg;
		if (conv_host_wire_receive(&lg, sizeof(lg), payload, bytes)) {
			lg.text[sizeof(lg.text) - 1] = '\0';
			U_LOG((enum u_logging_level)(lg.level <= U_LOGGING_ERROR ? lg.level : U_LOGGING_ERROR),
			      "[conv:%s] %s", p->provider_id, lg.text);
		}
		break;
	}
	case CONV_HOST_MSG_HEARTBEAT: {
		struct conv_host_wire_heartbeat hb;
		if (conv_host_wire_receive(&hb, sizeof(hb), payload, bytes)) {
			os_mutex_lock(&p->m);
			conv_supervisor_heartbeat(&p->sup, now_ns());
			p->hb_host.in_flight = hb.in_flight;
			p->hb_host.queued = hb.queued;
			p->hb_host.calls_total = hb.calls_total;
			p->hb_host.exec_threads = hb.exec_threads;
			os_mutex_unlock(&p->m);
		}
		break;
	}
	case CONV_HOST_MSG_STATE_CHANGED: {
		struct conv_host_wire_state_changed sc;
		if (conv_host_wire_receive(&sc, sizeof(sc), payload, bytes)) {
			os_mutex_lock(&p->m);
			if (sc.cap_id == 0) {
				p->redescribe = true; // the supervisor thread re-DESCRIBEs
				os_cond_broadcast(&p->cond);
			} else {
				for (uint32_t i = 0; i < p->cap_count; i++) {
					if (p->caps[i].cap_id == sc.cap_id) {
						conv_host_wire_unpack_state(&sc.state, &p->states[i]);
					}
				}
			}
			os_mutex_unlock(&p->m);
		}
		break;
	}
	case CONV_HOST_MSG_OUTPUT_IMAGE: handle_output_image(p, f, payload, bytes, fds, nfds); return;
	case CONV_HOST_MSG_SYNC: handle_sync(p, f, payload, bytes, fds, nfds); return;
	case CONV_HOST_MSG_JOB_PROGRESS: {
		struct conv_host_wire_job_progress jp;
		if (conv_host_wire_receive(&jp, sizeof(jp), payload, bytes)) {
			jp.stage[sizeof(jp.stage) - 1] = '\0';
			os_mutex_lock(&p->m);
			const struct xrt_conv_job_control *ctl = jp.job_id == p->cur_job_id ? p->cur_job_ctl : NULL;
			os_mutex_unlock(&p->m);
			// The worker is blocked in this job's request: ctl outlives the call.
			if (ctl != NULL && ctl->progress != NULL) {
				ctl->progress(ctl->ctx, jp.fraction, jp.stage);
			}
		}
		break;
	}
	default: break; // anything newer: ignored
	}
	for (uint32_t i = 0; i < nfds; i++) {
		os_native_handle_close(fds[i]);
	}
}

static void *
reader_main(void *ptr)
{
	struct proxy *p = (struct proxy *)ptr;
	uint8_t payload[CONV_HOST_WIRE_MAX_PAYLOAD];
	for (;;) {
		os_mutex_lock(&p->m);
		const bool stop = p->reader_stop;
		struct os_channel *ch = p->ch;
		os_mutex_unlock(&p->m);
		if (stop || ch == NULL) {
			break;
		}
		struct conv_host_wire_frame f;
		uint64_t fds[CONV_HOST_WIRE_MAX_HANDLES];
		uint32_t nfds = CONV_HOST_WIRE_MAX_HANDLES;
		int r = os_channel_recv(ch, &f, sizeof(f), fds, &nfds, 100);
		if (r == 0) {
			continue;
		}
		bool ok = r == 1 && conv_host_wire_frame_valid(&f);
		if (ok && f.payload_bytes > 0) {
			uint32_t none = 0;
			ok = os_channel_recv(ch, payload, f.payload_bytes, NULL, &none, 1000) == 1;
		}
		if (!ok) {
			for (uint32_t i = 0; r == 1 && i < nfds; i++) {
				os_native_handle_close(fds[i]);
			}
			os_mutex_lock(&p->m);
			if (!p->reader_stop) {
				p->chan_broken = true; // closed, desynced or a malformed frame: the peer is gone or hostile
			}
			os_cond_broadcast(&p->cond);
			os_mutex_unlock(&p->m);
			break;
		}
		handle_frame(p, &f, payload, fds, nfds);
	}
	return NULL;
}


/*
 *
 * Bring-up and loss (supervisor thread).
 *
 */

static void
lane_close(struct proxy *p)
{
	if (p->wakes_open) {
		os_wake_destroy(&p->wake_to_host);
		os_wake_destroy(&p->wake_from_host);
		p->wakes_open = false;
	}
	px_shm_close(&p->lane);
	memset(&p->submit_ring, 0, sizeof(p->submit_ring));
	memset(&p->complete_ring, 0, sizeof(p->complete_ring));
}

static bool
lane_open(struct proxy *p)
{
	const uint32_t sub_bytes = (uint32_t)sizeof(struct conv_host_wire_submit_record);
	const uint32_t com_bytes = (uint32_t)sizeof(struct conv_host_wire_complete_record);
	const size_t sub_ring = u_shm_ring_bytes(sub_bytes, CONV_HOST_WIRE_RING_CAPACITY);
	const size_t com_ring = u_shm_ring_bytes(com_bytes, CONV_HOST_WIRE_RING_CAPACITY);
	if (sub_ring == 0 || com_ring == 0) {
		return false;
	}
	const uint64_t scratch_at = ((uint64_t)sub_ring + com_ring + PX_ALIGN - 1) & ~(PX_ALIGN - 1);
	const uint64_t scratch = p->cfg.scratch_bytes != 0 ? p->cfg.scratch_bytes : PX_SCRATCH_DEFAULT;
	if (!px_shm_create(&p->lane, scratch_at + scratch)) {
		U_LOG_W("[conv:%s] could not create the lane section (%llu bytes)", p->provider_id,
		        (unsigned long long)(scratch_at + scratch));
		return false;
	}
	if (!u_shm_ring_init(&p->submit_ring, p->lane.ptr, sub_ring, sub_bytes, CONV_HOST_WIRE_RING_CAPACITY) ||
	    !u_shm_ring_init(&p->complete_ring, p->lane.ptr + sub_ring, com_ring, com_bytes,
	                     CONV_HOST_WIRE_RING_CAPACITY)) {
		lane_close(p);
		return false;
	}
	p->scratch_base = scratch_at;
	p->scratch_bytes = scratch;
	uint64_t remote_to = OS_NATIVE_HANDLE_INVALID, remote_from = OS_NATIVE_HANDLE_INVALID;
	if (os_wake_create_pair(&p->wake_to_host, &remote_to) != 0) {
		lane_close(p);
		return false;
	}
	if (os_wake_create_pair(&p->wake_from_host, &remote_from) != 0) {
		os_wake_destroy(&p->wake_to_host);
#ifndef XRT_OS_WINDOWS
		os_native_handle_close(remote_to);
#endif
		lane_close(p);
		return false;
	}
	p->wakes_open = true;

	struct conv_host_wire_lane_open lo;
	conv_host_wire_init(&lo, sizeof(lo));
	lo.lane = 0;
	lo.lane_kind = CONV_HOST_LANE_STREAM;
	lo.ring_capacity = CONV_HOST_WIRE_RING_CAPACITY;
	lo.submit_record_bytes = sub_bytes;
	lo.complete_record_bytes = com_bytes;
	lo.shm_bytes = p->lane.bytes;
	lo.submit_ring_offset = 0;
	lo.complete_ring_offset = sub_ring;
	const uint64_t hs[3] = {p->lane.handle, remote_to, remote_from};
	struct conv_host_wire_status st;
	const xrt_result_t r = px_request(p, CONV_HOST_MSG_LANE_OPEN, &lo, sizeof(lo), hs, 3, &st, sizeof(st),
	                                  PX_REQUEST_TIMEOUT_MS, NULL, NULL, NULL, 0, NULL);
#ifndef XRT_OS_WINDOWS
	// POSIX: the host has its own copies now (SCM_RIGHTS); Windows: the remote
	// end IS our local event (os_wake_create_pair), nothing extra to close.
	os_native_handle_close(remote_to);
	os_native_handle_close(remote_from);
#endif
	if (r != XRT_SUCCESS) {
		lane_close(p);
		return false;
	}
	p->seq = 0;
	return true;
}

//! DESCRIBE every capability and GET_STATE each (no lock held).
static bool
describe_all(struct proxy *p, uint32_t count)
{
	if (count > XRT_CONV_MAX_CAPABILITIES) {
		count = XRT_CONV_MAX_CAPABILITIES;
	}
	struct xrt_conv_capability caps[XRT_CONV_MAX_CAPABILITIES];
	struct xrt_conv_state states[XRT_CONV_MAX_CAPABILITIES];
	memset(states, 0, sizeof(states));
	uint32_t n = 0;
	for (uint32_t i = 0; i < count; i++) {
		struct conv_host_wire_index req;
		conv_host_wire_init(&req, sizeof(req));
		req.index = i;
		struct conv_host_wire_describe_reply rep;
		if (px_request(p, CONV_HOST_MSG_DESCRIBE, &req, sizeof(req), NULL, 0, &rep, sizeof(rep),
		               PX_REQUEST_TIMEOUT_MS, NULL, NULL, NULL, 0, NULL) != XRT_SUCCESS) {
			return false;
		}
		if (i == 0 && rep.count < count) {
			count = rep.count;
		}
		conv_host_wire_unpack_capability(&rep.capability, &caps[n]);
		states[n].struct_size = (uint32_t)sizeof(states[n]);
		n++;
	}
	for (uint32_t i = 0; i < n; i++) {
		struct conv_host_wire_cap_request req;
		conv_host_wire_init(&req, sizeof(req));
		req.cap_id = caps[i].cap_id;
		struct conv_host_wire_state_reply rep;
		if (px_request(p, CONV_HOST_MSG_GET_STATE, &req, sizeof(req), NULL, 0, &rep, sizeof(rep),
		               PX_REQUEST_TIMEOUT_MS, NULL, NULL, NULL, 0, NULL) == XRT_SUCCESS) {
			conv_host_wire_unpack_state(&rep.state, &states[i]);
		} else {
			states[i].state = XRT_CONV_STATE_INSTALLED;
		}
	}
	os_mutex_lock(&p->m);
	memcpy(p->caps, caps, sizeof(caps[0]) * n);
	memcpy(p->states, states, sizeof(states[0]) * n);
	p->cap_count = n;
	p->described = true;
	p->generation++;
	os_mutex_unlock(&p->m);
	return true;
}

//! After an accepted hello: describe, open the lane, go RUNNING (supervisor thread).
//! GET_PROVIDER_STATUS: the FALLBACK flag, platform state and hint, cached for the registry.
static void
fetch_provider_status(struct proxy *p)
{
	struct conv_host_wire_index q;
	conv_host_wire_init(&q, sizeof(q));
	struct conv_host_wire_provider_status rep;
	const xrt_result_t r = px_request(p, CONV_HOST_MSG_GET_PROVIDER_STATUS, &q, sizeof(q), NULL, 0, &rep,
	                                  sizeof(rep), PX_REQUEST_TIMEOUT_MS, NULL, NULL, NULL, 0, NULL);
	os_mutex_lock(&p->m);
	if (r == XRT_SUCCESS && rep.reported != 0) {
		memset(&p->provider_status, 0, sizeof(p->provider_status));
		p->provider_status.struct_size = (uint32_t)sizeof(p->provider_status);
		p->provider_status.state = rep.state;
		p->provider_status.flags = rep.flags;
		conv_host_wire_set_str(p->provider_status.hint, sizeof(p->provider_status.hint), rep.hint);
		p->status_known = true;
	} else if (r == XRT_SUCCESS) {
		p->status_known = false; // the provider reports none
	}
	os_mutex_unlock(&p->m);
}

static void
bring_up(struct proxy *p, uint32_t caps)
{
	fetch_provider_status(p); // before the describe: the core reads both at the generation bump
	if (!describe_all(p, caps) || !lane_open(p)) {
		os_mutex_lock(&p->m);
		conv_supervisor_channel_broken(&p->sup, now_ns());
		os_mutex_unlock(&p->m);
		return;
	}
	os_mutex_lock(&p->m);
	p->running = true;
	p->restart_owed = false;
	p->want_describe = false;
	const bool restored = p->ever_lost;
	const uint32_t epoch = p->epoch;
	os_cond_broadcast(&p->cond);
	os_mutex_unlock(&p->m);
	U_LOG_W("[conv:%s] host pid %ld up (epoch %u, %u capabilities)", p->provider_id, p->host_pid, epoch,
	        p->cap_count);
	if (restored) {
		conv_host_raise_event(&p->base, CONV_HOST_EV_RESTORED, 0);
	}
}

static void
clear_tables_locked(struct proxy *p, struct px_output *dead_outputs, struct px_sync *dead_syncs)
{
	memset(p->images, 0, sizeof(p->images));
	memset(p->streams, 0, sizeof(p->streams));
	memcpy(dead_outputs, p->outputs, sizeof(p->outputs));
	memcpy(dead_syncs, p->syncs, sizeof(p->syncs));
	memset(p->outputs, 0, sizeof(p->outputs));
	memset(p->syncs, 0, sizeof(p->syncs));
	p->open_streams = 0;
}

static void
release_outputs(struct proxy *p, struct px_output *outs, uint32_t n, struct px_sync *syncs, uint32_t ns)
{
	for (uint32_t i = 0; i < n; i++) {
		if (!outs[i].used) {
			continue;
		}
		px_shm_close(&outs[i].shm);
		if (outs[i].gpu_ctx != NULL && p->have_gpu) {
			p->gpu.close_output(p->gpu.ud, outs[i].gpu_ctx);
		}
		if (outs[i].gpu_handle != OS_NATIVE_HANDLE_INVALID && outs[i].gpu_handle != 0) {
			os_native_handle_close(outs[i].gpu_handle);
		}
	}
	for (uint32_t i = 0; i < ns; i++) {
		if (!syncs[i].used) {
			continue;
		}
		if (syncs[i].fence != NULL && p->have_gpu) {
			p->gpu.close_fence(p->gpu.ud, syncs[i].fence);
		}
		if (syncs[i].handle != OS_NATIVE_HANDLE_INVALID && syncs[i].handle != 0) {
			os_native_handle_close(syncs[i].handle);
		}
	}
}

/*!
 * The host process is gone (exit code @p code): bump the epoch FIRST, then fail
 * everything that waited on it, tear the incarnation down and tell the core.
 * Supervisor thread, no lock held.
 */
static void
on_host_gone(struct proxy *p, int32_t code)
{
	struct px_output *outs = (struct px_output *)calloc(PX_MAX_OUTPUTS, sizeof(*outs));
	struct px_sync *syncs = (struct px_sync *)calloc(PX_MAX_SYNCS, sizeof(*syncs));

	os_mutex_lock(&p->m);
	p->epoch++;
	const bool was_running = p->running;
	p->running = false;
	p->hello_ok = false;
	p->chan_broken = false;
	for (uint32_t i = 0; i < PX_MAX_PENDING; i++) {
		if (p->pending[i].used && !p->pending[i].done) {
			p->pending[i].done = true;
			p->pending[i].result = (int32_t)XRT_ERROR_IPC_FAILURE;
			p->pending[i].reply_bytes = 0;
		}
	}
	p->reader_stop = true;
	if (outs != NULL && syncs != NULL) {
		clear_tables_locked(p, outs, syncs);
	} else {
		memset(p->images, 0, sizeof(p->images));
		memset(p->streams, 0, sizeof(p->streams));
		p->open_streams = 0;
	}
	// Queued commands never reached the host: they are casualties too.
	struct conv_host_cmd *q = p->head;
	p->head = p->tail = NULL;
	p->queued = 0;
	const bool requested = p->stop;
	os_cond_broadcast(&p->cond);
	os_mutex_unlock(&p->m);

	if (p->reader_started) {
		os_thread_join(&p->reader);
		os_thread_destroy(&p->reader);
		p->reader_started = false;
	}
	os_mutex_lock(&p->m);
	struct os_channel *ch = p->ch;
	struct os_process *proc = p->proc;
	p->ch = NULL;
	p->proc = NULL;
	p->host_pid = 0;
	p->reader_stop = false;
	os_mutex_unlock(&p->m);
	os_mutex_lock(&p->send_mtx); // no sender may still hold the channel
	os_mutex_unlock(&p->send_mtx);
	os_channel_destroy(&ch);
	os_process_destroy(&proc);

	// The worker may still be inside a convert on the lane: it leaves on the
	// epoch change before it touches the lane again; wait for that.
	os_mutex_lock(&p->m);
	while (p->in_flight && p->cur_op == CONV_OP_CONVERT) {
		os_cond_wait_timeout_ns(&p->cond, &p->m, (uint64_t)PX_SLICE_MS * 1000000ull);
	}
	os_mutex_unlock(&p->m);
	lane_close(p);
	if (outs != NULL && syncs != NULL) {
		release_outputs(p, outs, PX_MAX_OUTPUTS, syncs, PX_MAX_SYNCS);
	}
	free(outs);
	free(syncs);

	while (q != NULL) {
		struct conv_host_cmd *next = q->next_;
		q->next_ = NULL;
		q->result = XRT_ERROR_IPC_FAILURE;
		q->end_ns = now_ns();
		q->on_complete(q);
		q = next;
	}

	if (!requested) {
		U_LOG_W("[conv:%s] host exited (code %d)%s", p->provider_id, (int)code,
		        was_running ? "" : " before it was up");
		os_mutex_lock(&p->m);
		p->ever_lost = true;
		// The core keeps the provider's tasks WARMING(restarting) and re-warms on
		// RESTORED: the respawn is owed even with nothing queued right now.
		p->restart_owed = (code != 0 && code != CONV_HOST_EXIT_UPDATE) || p->luid_restart;
		p->luid_restart = false;
		p->luid_kill_sent = false;
		os_mutex_unlock(&p->m);
		conv_host_raise_event(&p->base, CONV_HOST_EV_LOST, code);
	}
}


/*
 *
 * Supervisor thread.
 *
 */

/*! Start the host. false: not running; *out_exit_code says how it ended
 *  (CONV_SUP_EXIT_SPAWN_FAILED when it never started, else the exit code of
 *  a host that quit before connecting its channel, e.g. 10 = ABI reject). */
static bool
do_spawn(struct proxy *p, int32_t *out_exit_code)
{
	*out_exit_code = CONV_SUP_EXIT_SPAWN_FAILED;
	char hint[96];
	(void)snprintf(hint, sizeof(hint), CONV_HOST_WIRE_CHANNEL_FMT, p->provider_id,
	               (unsigned long)os_process_current_pid());
	struct os_channel *ch = NULL;
	char token[256];
	uint64_t child_end = OS_NATIVE_HANDLE_INVALID;
	if (os_channel_create_for_child(hint, &ch, token, sizeof(token), &child_end) != 0) {
		U_LOG_W("[conv:%s] could not create the host channel", p->provider_id);
		return false;
	}
	char parent[32], luid[32];
	(void)snprintf(parent, sizeof(parent), "%ld", os_process_current_pid());
	os_mutex_lock(&p->m);
	p->spawned_luid = p->cfg.adapter_luid;
	(void)snprintf(luid, sizeof(luid), "%llx", (unsigned long long)p->spawned_luid);
	os_mutex_unlock(&p->m);
	const char *args[16 + 8];
	uint32_t n = 0;
	args[n++] = CONV_HOST_ARG_PROVIDER;
	args[n++] = p->provider_id;
	args[n++] = CONV_HOST_ARG_BINARY;
	args[n++] = p->provider_binary;
	args[n++] = CONV_HOST_ARG_CHANNEL;
	args[n++] = token;
	args[n++] = CONV_HOST_ARG_PARENT;
	args[n++] = parent;
	args[n++] = CONV_HOST_ARG_ADAPTER_LUID;
	args[n++] = luid;
	for (uint32_t i = 0; i < p->cfg.extra_arg_count && n < 24; i++) {
		args[n++] = p->cfg.extra_args[i];
	}
	struct os_process_spawn_info si;
	memset(&si, 0, sizeof(si));
	si.exe_path = p->host_exe;
	si.args = args;
	si.arg_count = n;
	si.kill_on_parent_exit = true;
	si.channel_child_handle = child_end;
	si.log_path = p->log_path[0] != '\0' ? p->log_path : NULL;
	struct os_process *proc = NULL;
	if (os_process_spawn(&si, &proc) != 0) {
		U_LOG_W("[conv:%s] could not start %s", p->provider_id, p->host_exe);
		os_native_handle_close(child_end);
		os_channel_destroy(&ch);
		return false;
	}
	if (os_channel_accept_child(ch, proc, child_end, PX_ACCEPT_TIMEOUT_MS) != 0) {
		int32_t code = 0;
		if (os_process_wait(proc, 0, &code) == 1) {
			U_LOG_W("[conv:%s] host exited (code %d) before connecting its channel", p->provider_id, (int)code);
		} else {
			U_LOG_W("[conv:%s] host pid %ld never connected its channel", p->provider_id,
			        os_process_pid(proc));
			(void)os_process_terminate(proc, CONV_HOST_EXIT_WATCHDOG);
			(void)os_process_wait(proc, 2000, &code);
		}
		*out_exit_code = code;
		os_process_destroy(&proc);
		os_channel_destroy(&ch);
		return false;
	}
	os_mutex_lock(&p->m);
	p->proc = proc;
	p->ch = ch;
	p->host_pid = os_process_pid(proc);
	p->reader_stop = false;
	os_mutex_unlock(&p->m);
	if (os_thread_init(&p->reader) == 0 && os_thread_start(&p->reader, reader_main, p) == 0) {
		p->reader_started = true;
	}
	return p->reader_started;
}

static bool
demand_locked(const struct proxy *p)
{
	return p->queued > 0 || p->in_flight || p->open_streams > 0 || p->restart_owed ||
	       (!p->described && (p->cfg.spawn_for_describe || p->want_describe));
}

//! Report a terminal supervisor state once (FAILED / crash loop).
static void
report_terminal(struct proxy *p)
{
	os_mutex_lock(&p->m);
	struct conv_supervisor_status st;
	conv_supervisor_get_status(&p->sup, &st);
	const bool fresh = st.state == CONV_SUP_FAILED && p->sup.failed_at_ns != p->raised_terminal_at;
	if (fresh) {
		p->raised_terminal_at = p->sup.failed_at_ns;
	}
	const uint32_t reason = st.failure_reason;
	os_mutex_unlock(&p->m);
	if (!fresh) {
		return;
	}
	U_LOG_W("[conv:%s] host FAILED (%s); no respawn until a configuration change", p->provider_id,
	        st.detail != NULL ? st.detail : "?");
	if (reason == XRT_CONV_FAIL_CRASH_LOOP) {
		conv_host_raise_event(&p->base, CONV_HOST_EV_CRASH_LOOP, 0);
	} else {
		conv_host_raise_event(&p->base, CONV_HOST_EV_FAILED, (int32_t)reason);
	}
}

static void *
sup_main(void *ptr)
{
	struct proxy *p = (struct proxy *)ptr;
	for (;;) {
		const uint64_t t = now_ns();
		os_mutex_lock(&p->m);
		if (p->stop && !conv_supervisor_host_alive(&p->sup) && p->proc == NULL) {
			os_mutex_unlock(&p->m);
			break;
		}
		conv_supervisor_set_demand(&p->sup, t, demand_locked(p));
		if (p->breach) {
			p->breach = false;
			conv_supervisor_watchdog_breach(&p->sup, t);
		}
		if (p->chan_broken) {
			p->chan_broken = false;
			conv_supervisor_channel_broken(&p->sup, t);
		}
		if (p->luid_restart && !p->luid_kill_sent && p->proc != NULL) {
			// Closed for update: no backoff, no crash accounting; respawned on
			// demand (restart_owed) with the new --adapter-luid.
			p->luid_kill_sent = true;
			(void)os_process_terminate(p->proc, CONV_HOST_EXIT_UPDATE);
		}
		const bool hello = p->hello_ok && !p->running;
		const uint32_t hello_caps = p->hello_caps;
		const bool redescribe = p->redescribe && p->running;
		if (redescribe) {
			p->redescribe = false;
		}
		const bool poll_mutex = t >= p->next_maintenance_ns;
		os_mutex_unlock(&p->m);

#ifdef XRT_OS_WINDOWS
		if (poll_mutex) {
			char name[160];
			(void)snprintf(name, sizeof(name), "Global\\DisplayXR-ConvMaintenance-%s", p->provider_id);
			const bool held = os_named_mutex_held(name);
			os_mutex_lock(&p->m);
			conv_supervisor_set_maintenance(&p->sup, held);
			p->next_maintenance_ns = t + PX_MAINTENANCE_POLL_NS;
			os_mutex_unlock(&p->m);
		}
#else
		(void)poll_mutex;
#endif
		if (hello) {
			os_mutex_lock(&p->m);
			p->hello_ok = false;
			os_mutex_unlock(&p->m);
			bring_up(p, hello_caps);
			continue;
		}
		if (redescribe) {
			uint32_t count = 0;
			os_mutex_lock(&p->m);
			count = p->cap_count;
			os_mutex_unlock(&p->m);
			// The count may have changed: ask for one more than we know.
			(void)describe_all(p, count + 1 > XRT_CONV_MAX_CAPABILITIES ? count : count + 1);
		}

		os_mutex_lock(&p->m);
		uint64_t next = UINT64_MAX;
		const enum conv_supervisor_action a = conv_supervisor_tick(&p->sup, now_ns(), &next);
		const int32_t kill_code = conv_supervisor_kill_code(&p->sup);
		struct os_process *proc = p->proc;
		os_mutex_unlock(&p->m);

		switch (a) {
		case CONV_SUP_ACTION_SPAWN: {
			int32_t code = CONV_SUP_EXIT_SPAWN_FAILED;
			const bool ok = do_spawn(p, &code);
			const bool started = ok || code != CONV_SUP_EXIT_SPAWN_FAILED;
			os_mutex_lock(&p->m);
			conv_supervisor_spawned(&p->sup, now_ns(), started);
			os_mutex_unlock(&p->m);
			if (!ok) {
				on_host_gone(p, code);
				if (started) {
					os_mutex_lock(&p->m);
					conv_supervisor_exited(&p->sup, now_ns(), code);
					os_mutex_unlock(&p->m);
				}
				report_terminal(p);
			}
			continue;
		}
		case CONV_SUP_ACTION_SHUTDOWN: {
			struct conv_host_wire_shutdown sd;
			conv_host_wire_init(&sd, sizeof(sd));
			sd.orderly = 1;
			(void)px_send(p, CONV_HOST_MSG_SHUTDOWN, 0, &sd, sizeof(sd), NULL, 0);
			break;
		}
		case CONV_SUP_ACTION_TERMINATE:
			if (proc != NULL) {
				U_LOG_W("[conv:%s] killing host pid %ld (exit %d)", p->provider_id, os_process_pid(proc),
				        (int)kill_code);
				(void)os_process_terminate(proc, kill_code);
			}
			break;
		default: break;
		}

		// Wait: for the process when there is one, else on the condition.
		uint64_t wait_ns = (uint64_t)PX_SLICE_MS * 1000000ull;
		const uint64_t t2 = now_ns();
		if (next != UINT64_MAX && next > t2 && next - t2 < wait_ns) {
			wait_ns = next - t2;
		}
		if (proc != NULL) {
			int32_t code = 0;
			const int w = os_process_wait(proc, (uint32_t)(wait_ns / 1000000ull) + 1u, &code);
			if (w == 1) {
				on_host_gone(p, code);
				os_mutex_lock(&p->m);
				conv_supervisor_exited(&p->sup, now_ns(), code);
				os_mutex_unlock(&p->m);
				report_terminal(p);
			}
		} else {
			os_mutex_lock(&p->m);
			if (!p->stop && !p->hello_ok && !p->breach && !p->chan_broken) {
				os_cond_wait_timeout_ns(&p->cond, &p->m, wait_ns);
			}
			os_mutex_unlock(&p->m);
		}
	}
	return NULL;
}


/*
 *
 * Command worker.
 *
 */

/*! Wait until the host is RUNNING (true) or the command cannot be served
 *  (false: proxy stopping, or the supervisor gave up). p->m held. */
static bool
wait_running_locked(struct proxy *p)
{
	for (;;) {
		if (p->stop) {
			return false;
		}
		if (p->running) {
			return true;
		}
		struct conv_supervisor_status st;
		conv_supervisor_get_status(&p->sup, &st);
		if (st.state == CONV_SUP_FAILED || st.state == CONV_SUP_STOPPED) {
			return false;
		}
		os_cond_wait_timeout_ns(&p->cond, &p->m, (uint64_t)PX_SLICE_MS * 1000000ull);
	}
}

static struct px_stream *
stream_find_locked(struct proxy *p, uint64_t id)
{
	for (uint32_t i = 0; i < PX_MAX_STREAMS; i++) {
		if (p->streams[i].used && p->streams[i].id == id) {
			return &p->streams[i];
		}
	}
	return NULL;
}

static bool
cmd_is_cancelled(void *ctx)
{
	const struct conv_host_cmd *c = (const struct conv_host_cmd *)ctx;
	return c->is_cancelled != NULL && c->is_cancelled(c->cancel_ctx);
}

static xrt_result_t
exec_cap_request(struct proxy *p, uint32_t type, struct conv_host_cmd *c)
{
	struct conv_host_wire_cap_request req;
	conv_host_wire_init(&req, sizeof(req));
	req.cap_id = c->cap_id;
	struct conv_host_wire_status st;
	const bool warm = type == CONV_HOST_MSG_WARM;
	xrt_result_t r = px_request(p, type, &req, sizeof(req), NULL, 0, &st, sizeof(st), 0,
	                            warm && c->is_cancelled != NULL ? cmd_is_cancelled : NULL, c, NULL, 0, NULL);
	// Refresh the cached state: the core reads it right after the warm.
	struct conv_host_wire_state_reply sr;
	if (r != XRT_ERROR_IPC_FAILURE &&
	    px_request(p, CONV_HOST_MSG_GET_STATE, &req, sizeof(req), NULL, 0, &sr, sizeof(sr), PX_REQUEST_TIMEOUT_MS,
	               NULL, NULL, NULL, 0, NULL) == XRT_SUCCESS) {
		os_mutex_lock(&p->m);
		for (uint32_t i = 0; i < p->cap_count; i++) {
			if (p->caps[i].cap_id == c->cap_id) {
				conv_host_wire_unpack_state(&sr.state, &p->states[i]);
			}
		}
		os_mutex_unlock(&p->m);
	}
	return r;
}

static xrt_result_t
exec_stream_create(struct proxy *p, struct conv_host_cmd *c)
{
	const struct xrt_conv_stream_info *si = c->stream_info;
	if (si == NULL) {
		return XRT_ERROR_IPC_FAILURE;
	}
	os_mutex_lock(&p->m);
	const uint64_t id = ++p->next_stream_id;
	struct px_stream *s = NULL;
	for (uint32_t i = 0; i < PX_MAX_STREAMS && s == NULL; i++) {
		if (!p->streams[i].used) {
			s = &p->streams[i];
		}
	}
	if (s == NULL) {
		os_mutex_unlock(&p->m);
		return XRT_ERROR_ALLOCATION;
	}
	memset(s, 0, sizeof(*s));
	s->used = true;
	s->id = id;
	s->cap_id = c->cap_id;
	os_mutex_unlock(&p->m);

	struct conv_host_wire_stream_open so;
	conv_host_wire_init(&so, sizeof(so));
	so.cap_id = c->cap_id;
	so.stream = id;
	so.input_kind = si->input_kind;
	so.outputs = si->outputs;
	so.content_hint = si->content_hint;
	so.view_count = si->view_count;
	so.views_layout = si->views_layout;
	so.max_input_width = si->max_input_width;
	so.max_input_height = si->max_input_height;
	struct conv_host_wire_status st;
	const xrt_result_t r = px_request(p, CONV_HOST_MSG_STREAM_OPEN, &so, sizeof(so), NULL, 0, &st, sizeof(st),
	                                  PX_REQUEST_TIMEOUT_MS, NULL, NULL, NULL, 0, NULL);
	os_mutex_lock(&p->m);
	s = stream_find_locked(p, id);
	if (r == XRT_SUCCESS && s != NULL) {
		c->stream = id;
		p->open_streams++;
	} else if (s != NULL) {
		s->used = false;
	}
	os_mutex_unlock(&p->m);
	return r;
}

static xrt_result_t
exec_stream_destroy(struct proxy *p, struct conv_host_cmd *c)
{
	struct conv_host_wire_stream_ref sr;
	conv_host_wire_init(&sr, sizeof(sr));
	sr.stream = c->stream;
	struct conv_host_wire_status st;
	const xrt_result_t r = px_request(p, CONV_HOST_MSG_STREAM_CLOSE, &sr, sizeof(sr), NULL, 0, &st, sizeof(st),
	                                  PX_REQUEST_TIMEOUT_MS, NULL, NULL, NULL, 0, NULL);
	struct px_output dead[PX_MAX_OUTPUTS];
	uint32_t nd = 0;
	os_mutex_lock(&p->m);
	struct px_stream *s = stream_find_locked(p, c->stream);
	if (s != NULL) {
		s->used = false;
		if (p->open_streams > 0) {
			p->open_streams--;
		}
	}
	for (uint32_t i = 0; i < PX_MAX_OUTPUTS; i++) {
		if (p->outputs[i].used && p->outputs[i].stream == c->stream) {
			dead[nd++] = p->outputs[i];
			p->outputs[i].used = false;
		}
	}
	os_mutex_unlock(&p->m);
	release_outputs(p, dead, nd, NULL, 0);
	return r;
}

//! The image slot of @p img (by import_key), or -1.
static int32_t
image_slot_locked(struct proxy *p, const struct xrt_conv_image *img)
{
	for (uint32_t i = 0; i < PX_MAX_IMAGES; i++) {
		if (p->images[i].used && p->images[i].img.import_key == img->import_key &&
		    p->images[i].img.memory_kind == img->memory_kind) {
			return (int32_t)i;
		}
	}
	return -1;
}

//! First-fit scratch range (worker; p->m held).
static bool
scratch_alloc_locked(struct proxy *p, uint64_t len, uint64_t *out_off)
{
	len = (len + 255u) & ~255ull;
	uint64_t at = 0;
	for (;;) {
		bool moved = false;
		for (uint32_t i = 0; i < PX_MAX_IMAGES; i++) {
			const struct px_image *im = &p->images[i];
			if (!im->used || im->scratch_len == 0) {
				continue;
			}
			if (at < im->scratch_off + im->scratch_len && im->scratch_off < at + len) {
				at = (im->scratch_off + im->scratch_len + 255u) & ~255ull;
				moved = true;
			}
		}
		if (!moved) {
			break;
		}
	}
	if (at + len > p->scratch_bytes) {
		return false;
	}
	*out_off = at;
	return true;
}

/*! Make slot @p si known to this incarnation; CPU: refresh its scratch copy
 *  from @p img (the caller's pointer). */
static bool
push_slot(struct proxy *p, int32_t si, const struct xrt_conv_image *img, uint32_t role)
{
	os_mutex_lock(&p->m);
	struct px_image *im = &p->images[si];
	const bool cpu = img->memory_kind == XRT_CONV_MEM_CPU;
	bool need_push = !im->pushed;
	if (cpu && need_push && im->scratch_len == 0) {
		uint64_t off = 0;
		if (img->byte_size == 0 || !scratch_alloc_locked(p, img->byte_size, &off)) {
			os_mutex_unlock(&p->m);
			U_LOG_W("[conv:%s] input image does not fit the lane scratch (%llu bytes)", p->provider_id,
			        (unsigned long long)img->byte_size);
			return false;
		}
		im->scratch_off = off;
		im->scratch_len = img->byte_size;
	}
	const uint64_t scratch_off = im->scratch_off;
	const struct xrt_conv_image svc = im->img;
	os_mutex_unlock(&p->m);

	if (cpu) {
		if (svc.cpu_ptr == NULL || p->lane.ptr == NULL) {
			return false;
		}
		memcpy(p->lane.ptr + p->scratch_base + scratch_off, img->cpu_ptr != NULL ? img->cpu_ptr : svc.cpu_ptr,
		       (size_t)svc.byte_size);
	}
	if (!need_push) {
		return true;
	}
	if (!cpu && !p->have_gpu) {
		return false;
	}
	struct conv_host_wire_register_image ri;
	conv_host_wire_init(&ri, sizeof(ri));
	ri.index = (uint32_t)si;
	xrt_conv_wire_pack_image(&svc, role, cpu ? p->scratch_base + scratch_off : 0, &ri.image);
	uint64_t hs[2];
	uint32_t nh = 0;
	if (!cpu) {
		hs[nh++] = svc.handle;
		if (svc.sync.kind != XRT_CONV_SYNC_NONE && svc.sync.kind != XRT_CONV_SYNC_KEYED_MUTEX) {
			hs[nh++] = svc.sync.handle;
		}
	}
	struct conv_host_wire_status st;
	const xrt_result_t r = px_request(p, CONV_HOST_MSG_REGISTER_IMAGE, &ri, sizeof(ri), hs, nh, &st, sizeof(st),
	                                  PX_REQUEST_TIMEOUT_MS, NULL, NULL, NULL, 0, NULL);
	if (r != XRT_SUCCESS) {
		return false;
	}
	os_mutex_lock(&p->m);
	if (p->images[si].used && p->images[si].img.import_key == svc.import_key) {
		p->images[si].pushed = true;
	}
	os_mutex_unlock(&p->m);
	return true;
}

//! A registered input (found by import_key) pushed to this incarnation.
static bool
push_input(struct proxy *p, const struct xrt_conv_image *img, uint32_t role, uint32_t *out_slot)
{
	os_mutex_lock(&p->m);
	const int32_t si = image_slot_locked(p, img);
	os_mutex_unlock(&p->m);
	if (si < 0) {
		return false;
	}
	*out_slot = (uint32_t)si;
	return push_slot(p, si, img, role);
}

//! Drop slot @p si here and on the host (job inputs; worker).
static void
drop_slot(struct proxy *p, int32_t si)
{
	os_mutex_lock(&p->m);
	const bool pushed = p->images[si].pushed;
	memset(&p->images[si], 0, sizeof(p->images[si]));
	const bool running = p->running;
	os_mutex_unlock(&p->m);
	if (pushed && running) {
		struct conv_host_wire_index ui;
		conv_host_wire_init(&ui, sizeof(ui));
		ui.index = (uint32_t)si;
		struct conv_host_wire_status st;
		(void)px_request(p, CONV_HOST_MSG_UNREGISTER_IMAGE, &ui, sizeof(ui), NULL, 0, &st, sizeof(st),
		                 PX_REQUEST_TIMEOUT_MS, NULL, NULL, NULL, 0, NULL);
	}
}

static bool
job_is_cancelled(void *ctx)
{
	const struct xrt_conv_job_control *ctl = (const struct xrt_conv_job_control *)ctx;
	return ctl != NULL && ctl->is_cancelled != NULL && ctl->is_cancelled(ctl->ctx);
}

/*!
 * A job through the satellite: the inputs are registered for the job's
 * lifetime (CPU bytes in the lane scratch: the lowest-numbered open lane, the
 * only one here), JOB_SUBMIT runs to its job_done reply (JOB_CANCEL when the
 * caller's is_cancelled turns true; JOB_PROGRESS is routed to its progress),
 * and the blob comes back as a host shm section that is pulled, mapped and
 * written into the matching sink, then released on the host.
 */
static xrt_result_t
exec_job(struct proxy *p, struct conv_host_cmd *c)
{
	const struct xrt_conv_job_in *in = c->job_in;
	const struct xrt_conv_job_control *ctl = c->job_ctl;
	if (in == NULL || in->image_count > XRT_CONV_MAX_FRAME_IMAGES) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct conv_host_wire_job_submit js;
	conv_host_wire_init(&js, sizeof(js));
	int32_t slots[XRT_CONV_MAX_FRAME_IMAGES];
	uint32_t nslots = 0;
	xrt_result_t r = XRT_SUCCESS;
	for (uint32_t i = 0; i < in->image_count && r == XRT_SUCCESS; i++) {
		os_mutex_lock(&p->m);
		int32_t si = -1;
		for (uint32_t k = 0; k < PX_MAX_IMAGES && si < 0; k++) {
			if (!p->images[k].used) {
				si = (int32_t)k;
				memset(&p->images[k], 0, sizeof(p->images[k]));
				p->images[k].used = true;
				p->images[k].img = in->images[i];
			}
		}
		os_mutex_unlock(&p->m);
		if (si < 0) {
			r = XRT_ERROR_ALLOCATION;
			break;
		}
		slots[nslots++] = si;
		if (!push_slot(p, si, &in->images[i], in->image_role[i])) {
			r = XRT_ERROR_IPC_FAILURE;
			break;
		}
		js.image_idx[i] = (uint32_t)si;
		js.image_role[i] = in->image_role[i];
	}
	for (uint32_t i = in->image_count; i < XRT_CONV_MAX_FRAME_IMAGES; i++) {
		js.image_idx[i] = CONV_HOST_WIRE_NO_INDEX;
	}
	uint64_t job_id = 0;
	uint64_t blob = OS_NATIVE_HANDLE_INVALID;
	struct conv_host_wire_job_done jd;
	memset(&jd, 0, sizeof(jd));
	if (r == XRT_SUCCESS) {
		os_mutex_lock(&p->m);
		job_id = ++p->next_job_id;
		p->cur_job_id = job_id;
		p->cur_job_ctl = ctl;
		os_mutex_unlock(&p->m);
		js.job_id = job_id;
		js.cap_id = c->cap_id;
		js.input_kind = in->input_kind;
		js.image_count = in->image_count;
		js.requested_blob_format = in->requested_blob_format;
		if (c->params != NULL) {
			xrt_conv_wire_pack_params(c->params, &js.params);
		}
		r = px_request(p, CONV_HOST_MSG_JOB_SUBMIT, &js, sizeof(js), NULL, 0, &jd, sizeof(jd), 0,
		               ctl != NULL && ctl->is_cancelled != NULL ? job_is_cancelled : NULL, (void *)ctl, NULL,
		               job_id, &blob);
		os_mutex_lock(&p->m);
		p->cur_job_id = 0;
		p->cur_job_ctl = NULL;
		os_mutex_unlock(&p->m);
	}
	// The blob, into the sink that takes it.
	if (r == XRT_SUCCESS && jd.blob_bytes > 0) {
		struct px_shm m;
		if (blob == OS_NATIVE_HANDLE_INVALID || !px_shm_map(&m, blob, jd.blob_bytes)) {
			r = XRT_ERROR_IPC_FAILURE;
		} else {
			blob = OS_NATIVE_HANDLE_INVALID; // owned by m now
			bool delivered = false;
			for (uint32_t i = 0; ctl != NULL && i < ctl->sink_count && !delivered; i++) {
				struct xrt_conv_blob_sink *sk = &ctl->sinks[i];
				if (ctl->sink_count > 1 && (sk->role & jd.written) == 0) {
					continue;
				}
				delivered = true;
				if (sk->begin != NULL && !sk->begin(sk->ctx, jd.blob_format, jd.blob_bytes)) {
					r = XRT_ERROR_IPC_FAILURE;
				} else if (sk->write != NULL && !sk->write(sk->ctx, m.ptr, jd.blob_bytes)) {
					r = XRT_ERROR_IPC_FAILURE;
				}
			}
			px_shm_close(&m);
		}
	}
	if (blob != OS_NATIVE_HANDLE_INVALID) {
		os_native_handle_close(blob);
	}
	if (job_id != 0 && jd.blob_bytes > 0) {
		// The service holds its own copy now: the host closes its section.
		struct conv_host_wire_job_release jr;
		conv_host_wire_init(&jr, sizeof(jr));
		jr.job_id = job_id;
		(void)px_send(p, CONV_HOST_MSG_JOB_RELEASE, 0, &jr, sizeof(jr), NULL, 0);
	}
	if (r == XRT_SUCCESS && c->job_out != NULL) {
		c->job_out->written = jd.written;
		xrt_conv_wire_unpack_splat_info(&jd.splat_info, &c->job_out->splat_info);
		xrt_conv_wire_unpack_depth_info(&jd.depth_info, &c->job_out->depth_info);
		xrt_conv_wire_unpack_views_info(&jd.views_info, &c->job_out->views_info);
	}
	for (uint32_t i = 0; i < nslots; i++) {
		drop_slot(p, slots[i]);
	}
	return r;
}

//! A current output set for (stream, w, h): ALLOC_OUTPUTS when the extent changed.
static bool
ensure_outputs(struct proxy *p, uint64_t stream, uint32_t w, uint32_t h)
{
	os_mutex_lock(&p->m);
	struct px_stream *s = stream_find_locked(p, stream);
	const bool ok = s != NULL && s->out_w == w && s->out_h == h && s->out_request_id != 0;
	os_mutex_unlock(&p->m);
	if (ok) {
		return true;
	}
	struct conv_host_wire_alloc_outputs ao;
	conv_host_wire_init(&ao, sizeof(ao));
	ao.stream = stream;
	ao.width = w;
	ao.height = h;
	ao.set_count = 1;
	struct conv_host_wire_status st;
	uint32_t rid = 0;
	const xrt_result_t r = px_request(p, CONV_HOST_MSG_ALLOC_OUTPUTS, &ao, sizeof(ao), NULL, 0, &st, sizeof(st),
	                                  PX_REQUEST_TIMEOUT_MS, NULL, NULL, &rid, 0, NULL);
	if (r != XRT_SUCCESS) {
		return false;
	}
	// The host released the previous set after its reply: drop ours.
	struct px_output dead[PX_MAX_OUTPUTS];
	uint32_t nd = 0;
	os_mutex_lock(&p->m);
	s = stream_find_locked(p, stream);
	if (s != NULL) {
		s->out_w = w;
		s->out_h = h;
		s->out_request_id = rid;
	}
	for (uint32_t i = 0; i < PX_MAX_OUTPUTS; i++) {
		struct px_output *o = &p->outputs[i];
		if (o->used && o->stream == stream && o->request_id != rid) {
			dead[nd++] = *o;
			o->used = false;
		}
	}
	os_mutex_unlock(&p->m);
	release_outputs(p, dead, nd, NULL, 0);
	return s != NULL;
}

//! Copy one CPU host output into the runtime target (worker).
static bool
copy_cpu_output(const struct px_output *o, const struct xrt_conv_image *dst)
{
	if (dst->memory_kind != XRT_CONV_MEM_CPU || dst->cpu_ptr == NULL || o->shm.ptr == NULL) {
		return false;
	}
	const uint8_t *src = o->shm.ptr + o->shm_offset;
	uint8_t *d = (uint8_t *)dst->cpu_ptr;
	const uint32_t sstride = o->img.row_stride;
	const uint32_t dstride = dst->row_stride;
	if (sstride == 0 || dstride == 0 || sstride == dstride) {
		const uint64_t n = o->img.byte_size < dst->byte_size ? o->img.byte_size : dst->byte_size;
		memcpy(d, src, (size_t)n);
		return true;
	}
	const uint64_t srows = sstride != 0 ? o->img.byte_size / sstride : 0;
	const uint64_t drows = dst->byte_size / dstride;
	const uint64_t rows = srows < drows ? srows : drows;
	const uint32_t row = sstride < dstride ? sstride : dstride;
	for (uint64_t y = 0; y < rows; y++) {
		memcpy(d + y * dstride, src + y * sstride, row);
	}
	return true;
}

static struct px_output *
output_find_locked(struct proxy *p, uint64_t stream, uint32_t index)
{
	for (uint32_t i = 0; i < PX_MAX_OUTPUTS; i++) {
		if (p->outputs[i].used && p->outputs[i].stream == stream && p->outputs[i].index == index) {
			return &p->outputs[i];
		}
	}
	return NULL;
}

/*! Bring a completion's outputs into the runtime's targets. CPU: memcpy.
 *  GPU: observe the fence on the CPU FIRST (the fence rule), then copy. */
static bool
consume_outputs(struct proxy *p, uint64_t stream, const struct conv_host_wire_complete_record *rec, struct xrt_conv_frame_out *fo)
{
	const struct xrt_conv_image *dst[CONV_HOST_WIRE_OUT_COUNT] = {&fo->views, &fo->depth, &fo->confidence,
	                                                              &fo->disparity};
	bool fence_ok = true;
	if (rec->out_sync_idx != CONV_HOST_WIRE_NO_INDEX) {
		fence_ok = false;
		void *fence = NULL;
		uint64_t to_open = OS_NATIVE_HANDLE_INVALID;
		os_mutex_lock(&p->m);
		for (uint32_t i = 0; i < PX_MAX_SYNCS; i++) {
			struct px_sync *s = &p->syncs[i];
			if (s->used && s->index == rec->out_sync_idx) {
				if (s->fence == NULL && s->handle != OS_NATIVE_HANDLE_INVALID) {
					to_open = s->handle;
					s->handle = OS_NATIVE_HANDLE_INVALID;
				}
				fence = s->fence;
				break;
			}
		}
		os_mutex_unlock(&p->m);
		if (fence == NULL && to_open != OS_NATIVE_HANDLE_INVALID && p->have_gpu &&
		    p->gpu.open_fence(p->gpu.ud, to_open, &fence)) {
			os_mutex_lock(&p->m);
			for (uint32_t i = 0; i < PX_MAX_SYNCS; i++) {
				if (p->syncs[i].used && p->syncs[i].index == rec->out_sync_idx) {
					p->syncs[i].fence = fence;
				}
			}
			os_mutex_unlock(&p->m);
		}
		if (fence != NULL && p->have_gpu) {
			fence_ok = p->gpu.observe_fence(p->gpu.ud, fence, rec->out_fence_value, PX_FENCE_TIMEOUT_MS);
		}
		if (!fence_ok) {
			U_LOG_W("[conv:%s] output fence %u never reached %llu: result dropped", p->provider_id,
			        rec->out_sync_idx, (unsigned long long)rec->out_fence_value);
			return false;
		}
	}
	for (uint32_t role = 0; role < CONV_HOST_WIRE_OUT_COUNT; role++) {
		const uint32_t idx = rec->out_image_idx[role];
		if (idx == CONV_HOST_WIRE_NO_INDEX || dst[role]->memory_kind == 0) {
			continue;
		}
		os_mutex_lock(&p->m);
		struct px_output *o = output_find_locked(p, stream, idx);
		struct px_output copy;
		const bool found = o != NULL;
		if (found) {
			if (o->img.memory_kind != XRT_CONV_MEM_CPU && o->gpu_ctx == NULL &&
			    o->gpu_handle != OS_NATIVE_HANDLE_INVALID && p->have_gpu) {
				// Opened lazily, on the worker (the gpu ops' thread).
				uint64_t hdl = o->gpu_handle;
				o->gpu_handle = OS_NATIVE_HANDLE_INVALID;
				struct xrt_conv_image desc = o->img;
				os_mutex_unlock(&p->m);
				void *ctx = NULL;
				const bool opened = p->gpu.open_output(p->gpu.ud, hdl, &desc, &ctx);
				os_mutex_lock(&p->m);
				o = output_find_locked(p, stream, idx);
				if (o != NULL && opened) {
					o->gpu_ctx = ctx;
				} else if (opened) {
					p->gpu.close_output(p->gpu.ud, ctx);
				}
			}
			if (o != NULL) {
				copy = *o;
			}
		}
		os_mutex_unlock(&p->m);
		if (!found || o == NULL) {
			return false;
		}
		bool ok;
		if (copy.img.memory_kind == XRT_CONV_MEM_CPU) {
			ok = copy_cpu_output(&copy, dst[role]);
		} else {
			ok = p->have_gpu && copy.gpu_ctx != NULL && p->gpu.copy_output(p->gpu.ud, copy.gpu_ctx, dst[role]);
		}
		if (!ok) {
			return false;
		}
	}
	return true;
}

static xrt_result_t
exec_convert(struct proxy *p, struct conv_host_cmd *c, uint32_t epoch)
{
	const struct xrt_conv_frame_in *fin = c->frame_in;
	struct xrt_conv_frame_out *fo = c->frame_out;
	if (fin == NULL || fo == NULL || fin->image_count == 0 || fin->image_count > XRT_CONV_MAX_FRAME_IMAGES) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct conv_host_wire_submit_record rec;
	memset(&rec, 0, sizeof(rec));
	for (uint32_t i = 0; i < fin->image_count; i++) {
		uint32_t slot = 0;
		if (!push_input(p, &fin->images[i], fin->image_role[i], &slot)) {
			return XRT_ERROR_IPC_FAILURE;
		}
		rec.in_image_idx[i] = slot;
		rec.in_image_role[i] = fin->image_role[i];
	}
	for (uint32_t i = fin->image_count; i < XRT_CONV_MAX_FRAME_IMAGES; i++) {
		rec.in_image_idx[i] = CONV_HOST_WIRE_NO_INDEX;
	}
	if (!ensure_outputs(p, c->stream, fin->images[0].width, fin->images[0].height)) {
		return XRT_ERROR_IPC_FAILURE;
	}
	os_mutex_lock(&p->m);
	const bool same = p->epoch == epoch && p->running;
	rec.seq = ++p->seq;
	os_mutex_unlock(&p->m);
	if (!same) {
		return XRT_ERROR_IPC_FAILURE;
	}
	rec.stream = c->stream;
	rec.op = CONV_HOST_RING_OP_CONVERT;
	rec.image_count = fin->image_count;
	rec.in_sync_idx = CONV_HOST_WIRE_NO_INDEX; // inputs are complete at hand-over (the core's S0 drained)
	rec.submit_time_ns = now_ns();
	if (c->params != NULL) {
		xrt_conv_wire_pack_params(c->params, &rec.params);
	}
	xrt_conv_wire_pack_depth_info(&fin->depth_in_info, &rec.depth_in_info);

	bool wake = false;
	if (u_shm_ring_push(&p->submit_ring, &rec, &wake) != U_SHM_RING_OK) {
		os_mutex_lock(&p->m);
		p->chan_broken = true;
		os_cond_broadcast(&p->cond);
		os_mutex_unlock(&p->m);
		return XRT_ERROR_IPC_FAILURE;
	}
	if (wake) {
		(void)os_wake_signal(&p->wake_to_host);
	}

	// One convert in flight: the next completion is ours (older ones are stale).
	struct conv_host_wire_complete_record done;
	for (;;) {
		const enum u_shm_ring_result rr = u_shm_ring_pop(&p->complete_ring, &done);
		if (rr == U_SHM_RING_OK) {
			if (done.seq == rec.seq) {
				break;
			}
			continue;
		}
		if (rr == U_SHM_RING_CORRUPT) {
			os_mutex_lock(&p->m);
			p->chan_broken = true;
			os_cond_broadcast(&p->cond);
			os_mutex_unlock(&p->m);
			return XRT_ERROR_IPC_FAILURE;
		}
		os_mutex_lock(&p->m);
		const bool gone = p->epoch != epoch || p->stop;
		os_mutex_unlock(&p->m);
		if (gone) {
			return XRT_ERROR_IPC_FAILURE;
		}
		if (u_shm_ring_prepare_wait(&p->complete_ring)) {
			(void)os_wake_wait(&p->wake_from_host, PX_SLICE_MS);
			u_shm_ring_finish_wait(&p->complete_ring);
		}
	}
	if ((xrt_result_t)done.status != XRT_SUCCESS) {
		return (xrt_result_t)done.status;
	}
	if (!consume_outputs(p, c->stream, &done, fo)) {
		return XRT_ERROR_IPC_FAILURE;
	}
	fo->written = done.written;
	xrt_conv_wire_unpack_views_info(&done.views_info, &fo->views_info);
	xrt_conv_wire_unpack_depth_info(&done.depth_info, &fo->depth_info);
	return XRT_SUCCESS;
}

static void *
worker_main(void *ptr)
{
	struct proxy *p = (struct proxy *)ptr;
	os_mutex_lock(&p->m);
	for (;;) {
		while (p->head == NULL && !p->stop) {
			os_cond_wait(&p->cond, &p->m);
		}
		if (p->head == NULL && p->stop) {
			break;
		}
		struct conv_host_cmd *c = p->head;
		// Nothing to undo in a host that is not running: its provider streams
		// and resident models died with it. Never spawn one just to close
		// (core teardown destroys streams after the host may have gone idle).
		if (!p->running && (c->op == CONV_OP_STREAM_DESTROY || c->op == CONV_OP_EVICT)) {
			p->head = c->next_;
			if (p->head == NULL) {
				p->tail = NULL;
			}
			p->queued--;
			os_mutex_unlock(&p->m);
			c->next_ = NULL;
			c->result = XRT_SUCCESS;
			c->end_ns = now_ns();
			c->on_complete(c);
			os_mutex_lock(&p->m);
			continue;
		}
		// Wait for the host (lazy spawn: the queued command is the demand).
		if (!wait_running_locked(p)) {
			if (p->head == c) {
				p->head = c->next_;
				if (p->head == NULL) {
					p->tail = NULL;
				}
				p->queued--;
				os_mutex_unlock(&p->m);
				c->next_ = NULL;
				c->result = XRT_ERROR_IPC_FAILURE;
				c->end_ns = now_ns();
				c->on_complete(c);
				os_mutex_lock(&p->m);
			}
			continue;
		}
		if (p->head != c) {
			continue; // a loss failed the queue meanwhile
		}
		p->head = c->next_;
		if (p->head == NULL) {
			p->tail = NULL;
		}
		p->queued--;
		c->next_ = NULL;
		const uint32_t epoch = p->epoch;
		p->in_flight = true;
		p->cur_op = c->op;
		p->cur_cap = c->cap_id;
		p->cur_since_ns = c->start_ns = now_ns();
		os_mutex_unlock(&p->m);

		xrt_result_t r;
		switch (c->op) {
		case CONV_OP_WARM: r = exec_cap_request(p, CONV_HOST_MSG_WARM, c); break;
		case CONV_OP_EVICT: r = exec_cap_request(p, CONV_HOST_MSG_EVICT, c); break;
		case CONV_OP_STREAM_CREATE: r = exec_stream_create(p, c); break;
		case CONV_OP_STREAM_DESTROY: r = exec_stream_destroy(p, c); break;
		case CONV_OP_CONVERT: r = exec_convert(p, c, epoch); break;
		case CONV_OP_JOB: r = exec_job(p, c); break;
		default: r = XRT_ERROR_FEATURE_NOT_SUPPORTED; break;
		}

		os_mutex_lock(&p->m);
		if (p->epoch != epoch && r != XRT_SUCCESS) {
			r = XRT_ERROR_IPC_FAILURE; // the process it ran in is gone
		}
		p->in_flight = false;
		p->cur_op = 0;
		p->cur_since_ns = 0;
		p->calls_total++;
		os_cond_broadcast(&p->cond);
		os_mutex_unlock(&p->m);
		c->result = r;
		c->end_ns = now_ns();
		c->on_complete(c);
		os_mutex_lock(&p->m);
	}
	os_mutex_unlock(&p->m);
	return NULL;
}


/*
 *
 * conv_host_ops.
 *
 */

static xrt_result_t
px_describe_cap(struct conv_host *h, uint32_t index, struct xrt_conv_capability *out, uint32_t *out_count)
{
	struct proxy *p = px(h);
	os_mutex_lock(&p->m);
	if (!p->described) {
		os_mutex_unlock(&p->m);
		return XRT_ERROR_IPC_FAILURE; // not yet: the describe cache answers, the host is on its way
	}
	if (out_count != NULL) {
		*out_count = p->cap_count;
	}
	xrt_result_t r = XRT_SUCCESS;
	if (out != NULL) {
		if (index < p->cap_count) {
			*out = p->caps[index];
		} else {
			r = XRT_ERROR_FEATURE_NOT_SUPPORTED;
		}
	}
	os_mutex_unlock(&p->m);
	return r;
}

static uint64_t
px_get_generation(struct conv_host *h)
{
	struct proxy *p = px(h);
	os_mutex_lock(&p->m);
	const uint64_t g = p->generation;
	os_mutex_unlock(&p->m);
	return g;
}

static xrt_result_t
px_get_state(struct conv_host *h, uint32_t cap_id, struct xrt_conv_state *out)
{
	struct proxy *p = px(h);
	os_mutex_lock(&p->m);
	xrt_result_t r = XRT_ERROR_IPC_FAILURE;
	// Only the running incarnation's view counts: a dead or starting host has
	// none (the core keeps its WARMING(restarting) until RESTORED).
	if (p->running) {
		for (uint32_t i = 0; i < p->cap_count; i++) {
			if (p->caps[i].cap_id == cap_id) {
				*out = p->states[i];
				r = XRT_SUCCESS;
			}
		}
	}
	os_mutex_unlock(&p->m);
	return r;
}

static xrt_result_t
px_get_output_desc(struct conv_host *h, uint64_t stream, uint32_t w, uint32_t hgt, struct xrt_conv_output_desc *out)
{
	struct proxy *p = px(h);
	os_mutex_lock(&p->m);
	struct px_stream *s = stream_find_locked(p, stream);
	if (s != NULL && s->desc_cached && s->desc_w == w && s->desc_h == hgt) {
		*out = s->desc;
		os_mutex_unlock(&p->m);
		return XRT_SUCCESS;
	}
	const bool ok = s != NULL && p->running;
	os_mutex_unlock(&p->m);
	if (!ok) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct conv_host_wire_output_desc_request rq;
	conv_host_wire_init(&rq, sizeof(rq));
	rq.stream = stream;
	rq.width = w;
	rq.height = hgt;
	struct conv_host_wire_output_desc_reply rp;
	const xrt_result_t r = px_request(p, CONV_HOST_MSG_GET_OUTPUT_DESC, &rq, sizeof(rq), NULL, 0, &rp, sizeof(rp),
	                                  PX_REQUEST_TIMEOUT_MS, NULL, NULL, NULL, 0, NULL);
	if (r != XRT_SUCCESS) {
		return r;
	}
	memset(out, 0, sizeof(*out));
	out->struct_size = (uint32_t)sizeof(*out);
	out->views_width = rp.views_width;
	out->views_height = rp.views_height;
	out->views_format = rp.views_format;
	out->depth_width = rp.depth_width;
	out->depth_height = rp.depth_height;
	out->depth_format = rp.depth_format;
	out->confidence_width = rp.confidence_width;
	out->confidence_height = rp.confidence_height;
	out->confidence_format = rp.confidence_format;
	out->disparity_width = rp.disparity_width;
	out->disparity_height = rp.disparity_height;
	out->disparity_format = rp.disparity_format;
	os_mutex_lock(&p->m);
	s = stream_find_locked(p, stream);
	if (s != NULL) {
		s->desc = *out;
		s->desc_w = w;
		s->desc_h = hgt;
		s->desc_cached = true;
	}
	os_mutex_unlock(&p->m);
	return XRT_SUCCESS;
}

static xrt_result_t
px_submit(struct conv_host *h, struct conv_host_cmd *cmd)
{
	struct proxy *p = px(h);
	if (cmd == NULL || cmd->on_complete == NULL || cmd->op == CONV_OP_NONE || cmd->op > CONV_OP_JOB) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	os_mutex_lock(&p->m);
	struct conv_supervisor_status st;
	conv_supervisor_get_status(&p->sup, &st);
	if (p->stop || st.state == CONV_SUP_FAILED || st.state == CONV_SUP_STOPPED) {
		os_mutex_unlock(&p->m);
		return XRT_ERROR_IPC_FAILURE;
	}
	cmd->result = XRT_SUCCESS;
	cmd->blocked = 0;
	cmd->queued_ns = now_ns();
	cmd->start_ns = cmd->end_ns = 0;
	cmd->next_ = NULL;
	if (p->tail != NULL) {
		p->tail->next_ = cmd;
	} else {
		p->head = cmd;
	}
	p->tail = cmd;
	p->queued++;
	if (p->in_flight) {
		cmd->blocked |= CONV_CMD_BLOCKED_BY_OTHER;
	}
	os_cond_broadcast(&p->cond);
	os_mutex_unlock(&p->m);
	return XRT_SUCCESS;
}

static void
px_refresh_caps(struct conv_host *h)
{
	(void)h; // the host sizes its own exec threads from the provider's declaration
}

static bool
px_get_provider_status(struct conv_host *h, struct xrt_conv_provider_status *out)
{
	struct proxy *p = px(h);
	os_mutex_lock(&p->m);
	const bool known = p->status_known;
	if (known) {
		*out = p->provider_status;
	}
	os_mutex_unlock(&p->m);
	return known; // what the host process read from the provider at its last bring-up
}

static xrt_result_t
px_register_image(struct conv_host *h, const struct xrt_conv_image *img, uint32_t *out_index)
{
	struct proxy *p = px(h);
	if (img == NULL || out_index == NULL || img->memory_kind == 0) {
		return XRT_ERROR_ALLOCATION;
	}
	if (img->memory_kind != XRT_CONV_MEM_CPU && !p->have_gpu) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	os_mutex_lock(&p->m);
	for (uint32_t i = 0; i < PX_MAX_IMAGES; i++) {
		if (!p->images[i].used) {
			memset(&p->images[i], 0, sizeof(p->images[i]));
			p->images[i].used = true;
			p->images[i].img = *img;
			*out_index = ((p->epoch & 0xFFFFu) << PX_IDX_BITS) | i;
			os_mutex_unlock(&p->m);
			return XRT_SUCCESS; // pushed to the host at its first use as an input
		}
	}
	os_mutex_unlock(&p->m);
	return XRT_ERROR_ALLOCATION;
}

static bool
index_live_locked(struct proxy *p, uint32_t index, uint32_t *out_slot)
{
	const uint32_t slot = index & ((1u << PX_IDX_BITS) - 1u);
	if ((index >> PX_IDX_BITS) != (p->epoch & 0xFFFFu) || slot >= PX_MAX_IMAGES || !p->images[slot].used) {
		return false;
	}
	*out_slot = slot;
	return true;
}

static void
px_unregister_image(struct conv_host *h, uint32_t index)
{
	struct proxy *p = px(h);
	os_mutex_lock(&p->m);
	uint32_t slot = 0;
	if (!index_live_locked(p, index, &slot)) {
		os_mutex_unlock(&p->m);
		return; // a dead incarnation's index: nothing to drop
	}
	const bool pushed = p->images[slot].pushed;
	memset(&p->images[slot], 0, sizeof(p->images[slot]));
	const bool running = p->running;
	os_mutex_unlock(&p->m);
	if (pushed && running) {
		// Fire-and-forget would leave a reply nobody waits for; this is a
		// slot-lifetime event (rare), so a short request is fine.
		struct conv_host_wire_index ui;
		conv_host_wire_init(&ui, sizeof(ui));
		ui.index = slot;
		struct conv_host_wire_status st;
		(void)px_request(p, CONV_HOST_MSG_UNREGISTER_IMAGE, &ui, sizeof(ui), NULL, 0, &st, sizeof(st),
		                 PX_REQUEST_TIMEOUT_MS, NULL, NULL, NULL, 0, NULL);
	}
}

static bool
px_lookup_image(struct conv_host *h, uint32_t index, struct xrt_conv_image *out)
{
	struct proxy *p = px(h);
	os_mutex_lock(&p->m);
	uint32_t slot = 0;
	const bool ok = index_live_locked(p, index, &slot);
	if (ok) {
		*out = p->images[slot].img;
	}
	os_mutex_unlock(&p->m);
	return ok;
}

static void
px_heartbeat(struct conv_host *h, struct conv_host_heartbeat *out)
{
	struct proxy *p = px(h);
	memset(out, 0, sizeof(*out));
	os_mutex_lock(&p->m);
	out->in_flight = p->in_flight ? 1u : 0u;
	out->queued = p->queued;
	out->oldest_op = p->in_flight ? p->cur_op : 0u;
	out->oldest_cap_id = p->in_flight ? p->cur_cap : 0u;
	out->oldest_since_ns = p->in_flight ? p->cur_since_ns : 0u;
	out->calls_total = p->calls_total;
	out->exec_threads = p->hb_host.exec_threads;
	out->hung = false;
	os_mutex_unlock(&p->m);
}

static void
px_abandon(struct conv_host *h)
{
	// The core's watchdog (roadmap §5.3 limits): kill the host (exit 30). The
	// loss then takes the crash path. Never raises an event from here.
	struct proxy *p = px(h);
	os_mutex_lock(&p->m);
	p->breach = true;
	os_cond_broadcast(&p->cond);
	os_mutex_unlock(&p->m);
}

static uint32_t
px_get_epoch(struct conv_host *h)
{
	struct proxy *p = px(h);
	os_mutex_lock(&p->m);
	const uint32_t e = p->epoch;
	os_mutex_unlock(&p->m);
	return e;
}

static void
px_destroy(struct conv_host **h_ptr)
{
	struct proxy *p = px(*h_ptr);
	*h_ptr = NULL;
	os_mutex_lock(&p->m);
	p->stop = true;
	conv_supervisor_service_shutdown(&p->sup, now_ns());
	os_cond_broadcast(&p->cond);
	os_mutex_unlock(&p->m);
	if (p->sup_started) {
		os_thread_join(&p->sup_thread);
		os_thread_destroy(&p->sup_thread);
	}
	if (p->proc != NULL) {
		// The supervisor left with a process still there (should not happen): kill it.
		(void)os_process_terminate(p->proc, CONV_HOST_EXIT_WATCHDOG);
		int32_t code = 0;
		(void)os_process_wait(p->proc, 3000, &code);
		on_host_gone(p, code);
	}
	if (p->worker_started) {
		os_thread_join(&p->worker);
		os_thread_destroy(&p->worker);
	}
	// Anything still queued (submitted during shutdown): fail it.
	struct conv_host_cmd *q = p->head;
	p->head = p->tail = NULL;
	while (q != NULL) {
		struct conv_host_cmd *next = q->next_;
		q->next_ = NULL;
		q->result = XRT_ERROR_IPC_FAILURE;
		q->on_complete(q);
		q = next;
	}
	lane_close(p);
	if (p->have_gpu && p->gpu.destroy != NULL) {
		p->gpu.destroy(p->gpu.ud);
	}
	os_cond_destroy(&p->cond);
	os_mutex_destroy(&p->send_mtx);
	os_mutex_destroy(&p->m);
	free(p);
}

static const struct conv_host_ops k_proxy_ops = {
    .describe_cap = px_describe_cap,
    .get_generation = px_get_generation,
    .get_provider_status = px_get_provider_status,
    .get_state = px_get_state,
    .get_output_desc = px_get_output_desc,
    .submit = px_submit,
    .refresh_caps = px_refresh_caps,
    .register_image = px_register_image,
    .unregister_image = px_unregister_image,
    .lookup_image = px_lookup_image,
    .heartbeat = px_heartbeat,
    .abandon = px_abandon,
    .get_epoch = px_get_epoch,
    .destroy = px_destroy,
};


/*
 *
 * Create.
 *
 */

void
conv_host_proxy_config_defaults(struct conv_host_proxy_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->spawn_for_describe = false; // the describe cache answers; conv_host_proxy_request_describe when it cannot
}

xrt_result_t
conv_host_proxy_create(const struct conv_host_proxy_config *cfg, struct conv_host **out_host)
{
	if (cfg == NULL || out_host == NULL || cfg->provider_id == NULL || cfg->host_exe == NULL ||
	    cfg->provider_binary == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	struct proxy *p = (struct proxy *)calloc(1, sizeof(*p));
	if (p == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	p->base.ops = &k_proxy_ops;
	p->base.hosting = 2u; // CONV_HOSTING_SATELLITE
	p->cfg = *cfg;
	conv_host_wire_set_str(p->base.provider_id, sizeof(p->base.provider_id), cfg->provider_id);
	conv_host_wire_set_str(p->provider_id, sizeof(p->provider_id), cfg->provider_id);
	conv_host_wire_set_str(p->provider_binary, sizeof(p->provider_binary), cfg->provider_binary);
	conv_host_wire_set_str(p->host_exe, sizeof(p->host_exe), cfg->host_exe);
	conv_host_wire_set_str(p->log_path, sizeof(p->log_path), cfg->log_path);
	p->cfg.provider_id = p->provider_id;
	p->cfg.provider_binary = p->provider_binary;
	p->cfg.host_exe = p->host_exe;
	p->cfg.log_path = NULL;
	p->cfg.supervisor = NULL;
	if (cfg->gpu != NULL) {
		p->gpu = *cfg->gpu;
		p->have_gpu = true;
	}
	p->cfg.gpu = NULL;
	p->lane.handle = OS_NATIVE_HANDLE_INVALID;
	conv_supervisor_init(&p->sup, cfg->supervisor);
	if (os_mutex_init(&p->m) != 0 || os_mutex_init(&p->send_mtx) != 0 || os_cond_init(&p->cond) != 0) {
		free(p);
		return XRT_ERROR_THREADING_INIT_FAILURE;
	}
	if (os_thread_init(&p->worker) != 0 || os_thread_start(&p->worker, worker_main, p) != 0) {
		os_cond_destroy(&p->cond);
		os_mutex_destroy(&p->send_mtx);
		os_mutex_destroy(&p->m);
		free(p);
		return XRT_ERROR_THREADING_INIT_FAILURE;
	}
	p->worker_started = true;
	if (os_thread_init(&p->sup_thread) != 0 || os_thread_start(&p->sup_thread, sup_main, p) != 0) {
		p->have_gpu = false; // on failure the caller keeps its gpu ops
		struct conv_host *b = &p->base;
		px_destroy(&b);
		return XRT_ERROR_THREADING_INIT_FAILURE;
	}
	p->sup_started = true;
	*out_host = &p->base;
	return XRT_SUCCESS;
}

void
conv_host_proxy_request_describe(struct conv_host *h)
{
	struct proxy *p = px(h);
	os_mutex_lock(&p->m);
	if (!p->described) {
		p->want_describe = true;
		os_cond_broadcast(&p->cond);
	}
	os_mutex_unlock(&p->m);
}

void
conv_host_proxy_set_adapter(struct conv_host *h, uint64_t adapter_luid, const struct conv_host_proxy_gpu_ops *gpu)
{
	struct proxy *p = px(h);
	os_mutex_lock(&p->m);
	const bool changed = p->cfg.adapter_luid != adapter_luid;
	p->cfg.adapter_luid = adapter_luid;
	// The GPU lane opens only once the adapter is known; a lane already in use stays.
	bool adopted = false;
	if (gpu != NULL && !p->have_gpu) {
		p->gpu = *gpu;
		p->have_gpu = true;
		adopted = true;
	}
	// A live host on another adapter is restarted on this one (closed for
	// update: no crash accounting); an idle one picks it up at its next spawn.
	const bool restart = changed && p->proc != NULL && p->spawned_luid != adapter_luid;
	if (restart) {
		p->luid_restart = true; // the supervisor thread ends it (it owns the process)
		os_cond_broadcast(&p->cond);
	}
	os_mutex_unlock(&p->m);
	if (gpu != NULL && !adopted && gpu->destroy != NULL) {
		gpu->destroy(gpu->ud);
	}
	if (restart) {
		U_LOG_W("[conv:%s] service adapter is %llx: restarting the host on it", p->provider_id,
		        (unsigned long long)adapter_luid);
	}
}

void
conv_host_proxy_get_adapter(struct conv_host *h, uint64_t *out_spawned, uint64_t *out_hello)
{
	struct proxy *p = px(h);
	os_mutex_lock(&p->m);
	if (out_spawned != NULL) {
		*out_spawned = p->spawned_luid;
	}
	if (out_hello != NULL) {
		*out_hello = p->hello_luid;
	}
	os_mutex_unlock(&p->m);
}

void
conv_host_proxy_get_status(struct conv_host *h, struct conv_supervisor_status *out_status, long *out_pid)
{
	struct proxy *p = px(h);
	os_mutex_lock(&p->m);
	if (out_status != NULL) {
		conv_supervisor_get_status(&p->sup, out_status);
	}
	if (out_pid != NULL) {
		*out_pid = p->host_pid;
	}
	os_mutex_unlock(&p->m);
}
