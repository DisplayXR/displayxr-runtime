// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  SPSC fixed-record ring in shared memory (see u_shm_ring.h).
 * @ingroup aux_util
 */

#include "util/u_shm_ring.h"

#include <string.h>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif


/*
 *
 * Atomics on shared memory. Plain u32 words (the layout is a wire format, so
 * no C11 _Atomic types); compiler intrinsics only, no OS header (MSVC has no
 * usable <stdatomic.h> in C, see CLAUDE.md).
 *
 */

#if defined(_MSC_VER) && !defined(__clang__)

// Interlocked operations are full barriers on every MSVC target.
static inline uint32_t
ring_load_acquire(volatile uint32_t *p)
{
	return (uint32_t)_InterlockedOr((volatile long *)p, 0);
}

static inline void
ring_store_release(volatile uint32_t *p, uint32_t v)
{
	(void)_InterlockedExchange((volatile long *)p, (long)v);
}

static inline uint32_t
ring_load_seq_cst(volatile uint32_t *p)
{
	return (uint32_t)_InterlockedOr((volatile long *)p, 0);
}

static inline void
ring_store_seq_cst(volatile uint32_t *p, uint32_t v)
{
	(void)_InterlockedExchange((volatile long *)p, (long)v);
}

static inline void
ring_fence(void)
{
	volatile long dummy = 0;
	(void)_InterlockedOr(&dummy, 0);
}

#elif defined(__GNUC__) || defined(__clang__)

static inline uint32_t
ring_load_acquire(volatile uint32_t *p)
{
	return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static inline void
ring_store_release(volatile uint32_t *p, uint32_t v)
{
	__atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static inline uint32_t
ring_load_seq_cst(volatile uint32_t *p)
{
	return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}

static inline void
ring_store_seq_cst(volatile uint32_t *p, uint32_t v)
{
	__atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}

static inline void
ring_fence(void)
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

#else
#error "u_shm_ring: compiler not supported"
#endif


/*
 *
 * Layout.
 *
 */

/* Word offsets into the 192-byte header (u32 words). */
#define W_MAGIC 0u
#define W_VERSION 1u
#define W_RECORD_BYTES 2u
#define W_CAPACITY 3u
#define W_HEAD 16u    // line 1: producer
#define W_TAIL 32u    // line 2: consumer
#define W_WAITING 33u // line 2: consumer

static bool
geometry_valid(uint32_t record_bytes, uint32_t capacity)
{
	if (capacity < 2 || capacity > U_SHM_RING_MAX_CAPACITY || (capacity & (capacity - 1)) != 0) {
		return false;
	}
	if (record_bytes < 8 || record_bytes > U_SHM_RING_MAX_RECORD_BYTES || (record_bytes % 8) != 0) {
		return false;
	}
	return true;
}

size_t
u_shm_ring_bytes(uint32_t record_bytes, uint32_t capacity)
{
	if (!geometry_valid(record_bytes, capacity)) {
		return 0;
	}
	size_t n = (size_t)U_SHM_RING_HEADER_BYTES + (size_t)record_bytes * (size_t)capacity;
	return (n + 63u) & ~(size_t)63u;
}

static void
bind(struct u_shm_ring *r, void *mem, uint32_t record_bytes, uint32_t capacity)
{
	volatile uint32_t *w = (volatile uint32_t *)mem;
	memset(r, 0, sizeof(*r));
	r->head = &w[W_HEAD];
	r->tail = &w[W_TAIL];
	r->waiting = &w[W_WAITING];
	r->records = (uint8_t *)mem + U_SHM_RING_HEADER_BYTES;
	r->record_bytes = record_bytes;
	r->capacity = capacity;
	r->mask = capacity - 1;
}

bool
u_shm_ring_init(struct u_shm_ring *r, void *mem, size_t mem_bytes, uint32_t record_bytes, uint32_t capacity)
{
	size_t need = u_shm_ring_bytes(record_bytes, capacity);
	if (r == NULL || mem == NULL || need == 0 || mem_bytes < need || ((uintptr_t)mem % 8) != 0) {
		return false;
	}
	memset(mem, 0, need);
	uint32_t *w = (uint32_t *)mem;
	w[W_RECORD_BYTES] = record_bytes;
	w[W_CAPACITY] = capacity;
	w[W_VERSION] = U_SHM_RING_VERSION;
	bind(r, mem, record_bytes, capacity);
	// Magic last: an attach that races the init sees no magic and fails.
	ring_store_release((volatile uint32_t *)&w[W_MAGIC], U_SHM_RING_MAGIC);
	return true;
}

bool
u_shm_ring_attach(struct u_shm_ring *r, void *mem, size_t mem_bytes, uint32_t record_bytes, uint32_t capacity)
{
	size_t need = u_shm_ring_bytes(record_bytes, capacity);
	if (r == NULL || mem == NULL || need == 0 || mem_bytes < need || ((uintptr_t)mem % 8) != 0) {
		return false;
	}
	volatile uint32_t *w = (volatile uint32_t *)mem;
	if (ring_load_acquire(&w[W_MAGIC]) != U_SHM_RING_MAGIC || w[W_VERSION] != U_SHM_RING_VERSION ||
	    w[W_RECORD_BYTES] != record_bytes || w[W_CAPACITY] != capacity) {
		return false;
	}
	bind(r, mem, record_bytes, capacity);
	r->local_head = ring_load_acquire(r->head);
	r->local_tail = ring_load_acquire(r->tail);
	if ((uint32_t)(r->local_head - r->local_tail) > capacity) {
		return false;
	}
	return true;
}


/*
 *
 * Producer.
 *
 */

enum u_shm_ring_result
u_shm_ring_push(struct u_shm_ring *r, const void *record, bool *out_wake)
{
	if (out_wake != NULL) {
		*out_wake = false;
	}
	if (r->corrupt) {
		return U_SHM_RING_CORRUPT;
	}

	// The consumer's tail: never ahead of our head, never more than
	// capacity behind it.
	uint32_t tail = ring_load_acquire(r->tail);
	uint32_t in_flight = r->local_head - tail;
	if (in_flight > r->capacity) {
		r->corrupt = true;
		return U_SHM_RING_CORRUPT;
	}
	if (in_flight == r->capacity) {
		return U_SHM_RING_FULL;
	}

	memcpy(r->records + (size_t)(r->local_head & r->mask) * r->record_bytes, record, r->record_bytes);
	r->local_head++;
	ring_store_release(r->head, r->local_head);

	// Pairs with the fence in prepare_wait: either we see the flag, or the
	// consumer's re-check sees the head we just published.
	ring_fence();
	if (out_wake != NULL) {
		*out_wake = ring_load_seq_cst(r->waiting) != 0;
	}
	return U_SHM_RING_OK;
}


/*
 *
 * Consumer.
 *
 */

/* The producer's head, validated against our tail. */
static bool
read_head(struct u_shm_ring *r, uint32_t *out_head)
{
	uint32_t head = ring_load_acquire(r->head);
	if ((uint32_t)(head - r->local_tail) > r->capacity) {
		r->corrupt = true;
		return false;
	}
	*out_head = head;
	return true;
}

enum u_shm_ring_result
u_shm_ring_pop(struct u_shm_ring *r, void *record)
{
	if (r->corrupt) {
		return U_SHM_RING_CORRUPT;
	}
	uint32_t head;
	if (!read_head(r, &head)) {
		return U_SHM_RING_CORRUPT;
	}
	if (head == r->local_tail) {
		return U_SHM_RING_EMPTY;
	}

	memcpy(record, r->records + (size_t)(r->local_tail & r->mask) * r->record_bytes, r->record_bytes);
	r->local_tail++;
	ring_store_release(r->tail, r->local_tail);
	return U_SHM_RING_OK;
}

bool
u_shm_ring_prepare_wait(struct u_shm_ring *r)
{
	if (r->corrupt) {
		return false;
	}
	ring_store_seq_cst(r->waiting, 1);
	ring_fence();
	uint32_t head;
	if (!read_head(r, &head) || head != r->local_tail) {
		ring_store_seq_cst(r->waiting, 0);
		return false;
	}
	return true;
}

void
u_shm_ring_finish_wait(struct u_shm_ring *r)
{
	ring_store_seq_cst(r->waiting, 0);
}

uint32_t
u_shm_ring_count(struct u_shm_ring *r)
{
	uint32_t head = ring_load_acquire(r->head);
	uint32_t tail = ring_load_acquire(r->tail);
	uint32_t n = head - tail;
	return n > r->capacity ? r->capacity : n;
}
