// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The viewer pose page (see u_viewer_pose_page.h).
 * @ingroup aux_util
 */

#include "util/u_viewer_pose_page.h"

#include <string.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif


/*
 *
 * Atomics on shared memory. Plain fixed-width words (the layout is a wire
 * format, so no C11 _Atomic types); compiler intrinsics only, no OS header
 * (MSVC has no usable <stdatomic.h> in C, CLAUDE.md). Every access the
 * seqlocks depend on is sequentially consistent: the page is written at
 * sensor rate, the cost is nothing, and the reasoning stays simple.
 *
 */

#if defined(_MSC_VER) && !defined(__clang__)

static inline void
fence(void)
{
	volatile long dummy = 0;
	(void)_InterlockedOr(&dummy, 0);
}

// A plain aligned 64-bit read between two full fences, never an interlocked
// read-modify-write: readers map the page READ-ONLY, and a locked RMW on a
// read-only view is an access violation.
static inline uint64_t
load64(const volatile uint64_t *p)
{
	fence();
	const uint64_t v = *p;
	fence();
	return v;
}

static inline void
store64(volatile uint64_t *p, uint64_t v)
{
	(void)_InterlockedExchange64((volatile __int64 *)p, (__int64)v);
}

#elif defined(__GNUC__) || defined(__clang__)

static inline uint64_t
load64(const volatile uint64_t *p)
{
	return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}

static inline void
store64(volatile uint64_t *p, uint64_t v)
{
	__atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}

static inline void
fence(void)
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

#else
#error "u_viewer_pose_page: compiler not supported"
#endif


/*
 *
 * Layout.
 *
 */

#define OFF_MAGIC 0u
#define OFF_VERSION 4u
#define OFF_HEADER_SIZE 8u
#define OFF_RING_SIZE 12u
#define OFF_SLOT_STRIDE 16u
#define OFF_SAMPLE_SIZE 20u
#define OFF_TOTAL_SIZE 24u
#define OFF_OWNER_PID 32u
#define OFF_OWNER_CREATED 40u

#define OFF_WRITE_COUNT 64u
#define OFF_HEARTBEAT 72u

#define OFF_OWNER_SEQ 128u
#define OFF_OWNER_STATE 136u
#define OFF_OWNER_RESTARTS 140u
#define OFF_OWNER_GENERATION 144u
#define OFF_OWNER_IN_PROCESS 148u
#define OFF_OWNER_HOST_PID 152u
#define OFF_OWNER_PROVIDER 160u // .. 224

#define OFF_PROD_SEQ 320u
#define OFF_PROD_PID 328u
#define OFF_PROD_GENERATION 336u
#define OFF_PROD_STATUS 340u
#define OFF_PROD_TRACKS 344u
#define OFF_PROD_PROVIDER 352u // .. 416
#define OFF_PROD_SOURCE 416u   // .. 544
#define OFF_PROD_CAMERA 544u   // .. 672

#define OFF_DEMAND 768u

#define SLOT_SEQ_BYTES 8u
#define SAMPLE_BYTES ((uint32_t)sizeof(struct xrt_tracking_viewer_sample))
#define SLOT_STRIDE (((SLOT_SEQ_BYTES + SAMPLE_BYTES) + 63u) & ~63u)

#if defined(__cplusplus)
#define VPP_ASSERT(c, m) static_assert(c, m)
#else
#define VPP_ASSERT(c, m) _Static_assert(c, m)
#endif
VPP_ASSERT(OFF_OWNER_PROVIDER + U_VPP_ID_MAX <= OFF_PROD_SEQ, "owner block overflows");
VPP_ASSERT(OFF_PROD_CAMERA + U_VPP_TEXT_MAX <= OFF_DEMAND, "producer block overflows");
VPP_ASSERT(U_VPP_HEADER_BYTES + U_VPP_RING_SIZE * SLOT_STRIDE <= U_VPP_PAGE_BYTES, "ring does not fit the page");

//! Tries before a reader gives up on a block / slot that keeps changing under it.
#define READ_TRIES 8

static inline uint8_t *
at(void *mem, uint32_t off)
{
	return (uint8_t *)mem + off;
}

static inline const uint8_t *
cat(const void *mem, uint32_t off)
{
	return (const uint8_t *)mem + off;
}

static inline volatile uint64_t *
w64(void *mem, uint32_t off)
{
	return (volatile uint64_t *)(void *)at(mem, off);
}

static inline const volatile uint64_t *
r64(const void *mem, uint32_t off)
{
	return (const volatile uint64_t *)(const void *)cat(mem, off);
}

static inline uint32_t
get_u32(const void *mem, uint32_t off)
{
	uint32_t v;
	memcpy(&v, cat(mem, off), sizeof(v));
	return v;
}

static inline void
put_u32(void *mem, uint32_t off, uint32_t v)
{
	memcpy(at(mem, off), &v, sizeof(v));
}

static inline int64_t
get_i64(const void *mem, uint32_t off)
{
	int64_t v;
	memcpy(&v, cat(mem, off), sizeof(v));
	return v;
}

static inline void
put_i64(void *mem, uint32_t off, int64_t v)
{
	memcpy(at(mem, off), &v, sizeof(v));
}

//! Copy @p src into the page as a NUL-terminated string of @p cap bytes.
static void
put_str(void *mem, uint32_t off, uint32_t cap, const char *src)
{
	char *dst = (char *)at(mem, off);
	memset(dst, 0, cap);
	if (src != NULL) {
		size_t n = strlen(src);
		if (n >= cap) {
			n = cap - 1;
		}
		memcpy(dst, src, n);
	}
}

//! Copy a page string out; always NUL-terminated (the producer is untrusted).
static void
get_str(const void *mem, uint32_t off, uint32_t cap, char *out)
{
	memcpy(out, cat(mem, off), cap);
	out[cap - 1] = '\0';
}

const char *
u_vpp_state_str(uint32_t state)
{
	switch (state) {
	case U_VPP_STATE_IDLE: return "idle";
	case U_VPP_STATE_STARTING: return "starting";
	case U_VPP_STATE_RUNNING: return "running";
	case U_VPP_STATE_BACKOFF: return "backoff";
	case U_VPP_STATE_FAILED: return "failed";
	case U_VPP_STATE_STOPPED: return "stopped";
	case U_VPP_STATE_NONE:
	default: return "none";
	}
}


/*
 *
 * Owner.
 *
 */

bool
u_vpp_init(void *mem, size_t mem_bytes, int64_t owner_pid, int64_t now_ns)
{
	if (mem == NULL || mem_bytes < U_VPP_PAGE_BYTES || ((uintptr_t)mem % 8) != 0) {
		return false;
	}
	memset(mem, 0, U_VPP_PAGE_BYTES);
	put_u32(mem, OFF_VERSION, U_VPP_VERSION);
	put_u32(mem, OFF_HEADER_SIZE, U_VPP_HEADER_BYTES);
	put_u32(mem, OFF_RING_SIZE, U_VPP_RING_SIZE);
	put_u32(mem, OFF_SLOT_STRIDE, SLOT_STRIDE);
	put_u32(mem, OFF_SAMPLE_SIZE, SAMPLE_BYTES);
	const uint64_t total = U_VPP_PAGE_BYTES;
	memcpy(at(mem, OFF_TOTAL_SIZE), &total, sizeof(total));
	put_i64(mem, OFF_OWNER_PID, owner_pid);
	put_i64(mem, OFF_OWNER_CREATED, now_ns);
	fence();
	// Magic last: an attach racing the init sees no magic and fails.
	uint32_t magic = U_VPP_MAGIC;
	memcpy(at(mem, OFF_MAGIC), &magic, sizeof(magic));
	fence();
	return true;
}

void
u_vpp_write_owner(void *mem, const struct u_vpp_owner_status *st)
{
	if (mem == NULL || st == NULL) {
		return;
	}
	volatile uint64_t *seq = w64(mem, OFF_OWNER_SEQ);
	const uint64_t s = load64(seq) | 1u;
	store64(seq, s); // odd: writing
	fence();
	put_u32(mem, OFF_OWNER_STATE, st->state);
	put_u32(mem, OFF_OWNER_RESTARTS, st->restarts);
	put_u32(mem, OFF_OWNER_GENERATION, st->generation);
	put_u32(mem, OFF_OWNER_IN_PROCESS, st->in_process ? 1u : 0u);
	put_i64(mem, OFF_OWNER_HOST_PID, st->host_pid);
	put_str(mem, OFF_OWNER_PROVIDER, U_VPP_ID_MAX, st->provider_id);
	fence();
	store64(seq, s + 1u); // even: done
}


/*
 *
 * Producer.
 *
 */

void
u_vpp_producer_begin(struct u_vpp_producer *p, void *mem)
{
	if (p == NULL) {
		return;
	}
	p->mem = mem;
	p->next = mem != NULL ? load64(r64(mem, OFF_WRITE_COUNT)) : 0;
}

void
u_vpp_write_sample(struct u_vpp_producer *p, const struct xrt_tracking_viewer_sample *s)
{
	if (p == NULL || p->mem == NULL || s == NULL) {
		return;
	}
	void *mem = p->mem;
	const uint64_t k = p->next;
	const uint32_t slot = (uint32_t)(k % U_VPP_RING_SIZE);
	const uint32_t base = U_VPP_HEADER_BYTES + slot * SLOT_STRIDE;
	volatile uint64_t *seq = w64(mem, base);

	store64(seq, 2u * k + 1u); // odd: being written
	fence();
	// The sample, at most the page's size; a shorter (older) sample is zero-extended.
	uint8_t *dst = at(mem, base + SLOT_SEQ_BYTES);
	uint32_t n = s->struct_size;
	if (n > SAMPLE_BYTES || n < (uint32_t)offsetof(struct xrt_tracking_viewer_sample, left_eye)) {
		n = SAMPLE_BYTES;
	}
	memcpy(dst, s, n);
	if (n < SAMPLE_BYTES) {
		memset(dst + n, 0, SAMPLE_BYTES - n);
	}
	// The page's copy always says the page's size.
	const uint32_t page_size = SAMPLE_BYTES;
	memcpy(dst, &page_size, sizeof(page_size));
	fence();
	store64(seq, 2u * k + 2u); // even: sample k complete
	fence();
	p->next = k + 1u;
	store64(w64(mem, OFF_WRITE_COUNT), k + 1u);
}

void
u_vpp_write_producer(void *mem, const struct u_vpp_producer_status *st)
{
	if (mem == NULL || st == NULL) {
		return;
	}
	volatile uint64_t *seq = w64(mem, OFF_PROD_SEQ);
	const uint64_t s = load64(seq) | 1u;
	store64(seq, s);
	fence();
	put_i64(mem, OFF_PROD_PID, st->pid);
	put_u32(mem, OFF_PROD_GENERATION, st->generation);
	put_u32(mem, OFF_PROD_STATUS, st->provider_status);
	put_u32(mem, OFF_PROD_TRACKS, st->tracks);
	put_str(mem, OFF_PROD_PROVIDER, U_VPP_ID_MAX, st->provider_id);
	put_str(mem, OFF_PROD_SOURCE, U_VPP_TEXT_MAX, st->source);
	put_str(mem, OFF_PROD_CAMERA, U_VPP_TEXT_MAX, st->camera_key);
	fence();
	store64(seq, s + 1u);
}

void
u_vpp_heartbeat(void *mem, int64_t now_ns)
{
	if (mem != NULL) {
		store64(w64(mem, OFF_HEARTBEAT), (uint64_t)now_ns);
	}
}


/*
 *
 * Readers.
 *
 */

bool
u_vpp_attach_check(const void *mem, size_t mem_bytes)
{
	if (mem == NULL || mem_bytes < U_VPP_HEADER_BYTES || ((uintptr_t)mem % 8) != 0) {
		return false;
	}
	fence();
	struct u_vpp_geometry g;
	u_vpp_read_geometry(mem, &g);
	if (g.magic != U_VPP_MAGIC || g.version != U_VPP_VERSION || g.header_size != U_VPP_HEADER_BYTES ||
	    g.ring_size != U_VPP_RING_SIZE) {
		return false;
	}
	// The slots must hold the sample this reader copies, and the page must fit the mapping.
	if (g.sample_size < (uint32_t)offsetof(struct xrt_tracking_viewer_sample, head_position) ||
	    g.slot_stride < SLOT_SEQ_BYTES + g.sample_size || (g.slot_stride % 8u) != 0 ||
	    g.slot_stride > U_VPP_PAGE_BYTES) {
		return false;
	}
	const uint64_t need = (uint64_t)U_VPP_HEADER_BYTES + (uint64_t)g.ring_size * g.slot_stride;
	return need <= g.total_size && g.total_size <= mem_bytes;
}

void
u_vpp_read_geometry(const void *mem, struct u_vpp_geometry *out)
{
	memset(out, 0, sizeof(*out));
	if (mem == NULL) {
		return;
	}
	out->magic = get_u32(mem, OFF_MAGIC);
	out->version = get_u32(mem, OFF_VERSION);
	out->header_size = get_u32(mem, OFF_HEADER_SIZE);
	out->ring_size = get_u32(mem, OFF_RING_SIZE);
	out->slot_stride = get_u32(mem, OFF_SLOT_STRIDE);
	out->sample_size = get_u32(mem, OFF_SAMPLE_SIZE);
	memcpy(&out->total_size, cat(mem, OFF_TOTAL_SIZE), sizeof(out->total_size));
	out->owner_pid = get_i64(mem, OFF_OWNER_PID);
	out->owner_created_ns = get_i64(mem, OFF_OWNER_CREATED);
}

bool
u_vpp_read_owner(const void *mem, struct u_vpp_owner_status *out)
{
	memset(out, 0, sizeof(*out));
	if (mem == NULL) {
		return false;
	}
	const volatile uint64_t *seq = r64(mem, OFF_OWNER_SEQ);
	for (int t = 0; t < READ_TRIES; t++) {
		const uint64_t s1 = load64(seq);
		if ((s1 & 1u) != 0) {
			continue;
		}
		fence();
		struct u_vpp_owner_status st;
		memset(&st, 0, sizeof(st));
		st.state = get_u32(mem, OFF_OWNER_STATE);
		st.restarts = get_u32(mem, OFF_OWNER_RESTARTS);
		st.generation = get_u32(mem, OFF_OWNER_GENERATION);
		st.in_process = get_u32(mem, OFF_OWNER_IN_PROCESS) != 0;
		st.host_pid = get_i64(mem, OFF_OWNER_HOST_PID);
		get_str(mem, OFF_OWNER_PROVIDER, U_VPP_ID_MAX, st.provider_id);
		fence();
		if (load64(seq) == s1) {
			*out = st;
			return true;
		}
	}
	return false;
}

bool
u_vpp_read_producer(const void *mem, struct u_vpp_producer_status *out)
{
	memset(out, 0, sizeof(*out));
	if (mem == NULL) {
		return false;
	}
	const volatile uint64_t *seq = r64(mem, OFF_PROD_SEQ);
	for (int t = 0; t < READ_TRIES; t++) {
		const uint64_t s1 = load64(seq);
		if ((s1 & 1u) != 0) {
			continue;
		}
		fence();
		struct u_vpp_producer_status st;
		memset(&st, 0, sizeof(st));
		st.pid = get_i64(mem, OFF_PROD_PID);
		st.generation = get_u32(mem, OFF_PROD_GENERATION);
		st.provider_status = get_u32(mem, OFF_PROD_STATUS);
		st.tracks = get_u32(mem, OFF_PROD_TRACKS);
		get_str(mem, OFF_PROD_PROVIDER, U_VPP_ID_MAX, st.provider_id);
		get_str(mem, OFF_PROD_SOURCE, U_VPP_TEXT_MAX, st.source);
		get_str(mem, OFF_PROD_CAMERA, U_VPP_TEXT_MAX, st.camera_key);
		fence();
		if (load64(seq) == s1) {
			*out = st;
			return true;
		}
	}
	return false;
}

int64_t
u_vpp_read_heartbeat(const void *mem)
{
	return mem != NULL ? (int64_t)load64(r64(mem, OFF_HEARTBEAT)) : 0;
}

uint64_t
u_vpp_read_write_count(const void *mem)
{
	return mem != NULL ? load64(r64(mem, OFF_WRITE_COUNT)) : 0;
}

void
u_vpp_post_demand(void *mem, int64_t now_ns)
{
	if (mem != NULL) {
		store64(w64(mem, OFF_DEMAND), (uint64_t)now_ns);
	}
}

int64_t
u_vpp_read_demand(const void *mem)
{
	return mem != NULL ? (int64_t)load64(r64(mem, OFF_DEMAND)) : 0;
}

uint32_t
u_vpp_read_new(const void *mem, struct u_vpp_reader *r, struct xrt_tracking_viewer_sample *out, uint32_t max)
{
	if (mem == NULL || r == NULL || out == NULL || max == 0) {
		return 0;
	}
	const uint32_t stride = get_u32(mem, OFF_SLOT_STRIDE);
	uint32_t page_sample = get_u32(mem, OFF_SAMPLE_SIZE);
	const uint32_t copy = page_sample < SAMPLE_BYTES ? page_sample : SAMPLE_BYTES;

	const uint64_t n = load64(r64(mem, OFF_WRITE_COUNT));
	if (!r->started || n < r->next) {
		// First drain, or the count went backwards (a new page / a misbehaving
		// producer): start from the oldest sample still on the page.
		r->next = n > U_VPP_RING_SIZE ? n - U_VPP_RING_SIZE : 0;
		r->started = true;
	}
	if (n - r->next > U_VPP_RING_SIZE) {
		r->dropped += (n - r->next) - U_VPP_RING_SIZE;
		r->next = n - U_VPP_RING_SIZE;
	}
	if (n - r->next > max) {
		r->dropped += (n - r->next) - max;
		r->next = n - max;
	}

	uint32_t got = 0;
	for (uint64_t k = r->next; k < n; k++) {
		const uint32_t slot = (uint32_t)(k % U_VPP_RING_SIZE);
		const uint32_t base = U_VPP_HEADER_BYTES + slot * stride;
		const volatile uint64_t *seq = r64(mem, base);
		const uint64_t want = 2u * k + 2u;
		bool ok = false;
		bool lapped = false;
		for (int t = 0; t < READ_TRIES && !ok && !lapped; t++) {
			const uint64_t s1 = load64(seq);
			if (s1 != want) {
				// Odd at (2k+1): being written — cannot be, k is below the
				// published count, unless a NEW producer is rewriting it; past
				// it: lapped. Either way this copy of sample k is gone.
				if (s1 > want) {
					lapped = true;
				} else {
					r->torn++;
				}
				continue;
			}
			fence();
			struct xrt_tracking_viewer_sample tmp;
			memset(&tmp, 0, sizeof(tmp));
			memcpy(&tmp, cat(mem, base + SLOT_SEQ_BYTES), copy);
			fence();
			if (load64(seq) != s1) {
				r->torn++;
				continue;
			}
			tmp.struct_size = copy;
			out[got] = tmp;
			ok = true;
		}
		if (ok) {
			got++;
		} else {
			r->dropped++;
		}
	}
	r->next = n;
	return got;
}
