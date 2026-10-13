// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Single-producer / single-consumer ring of fixed-size records in
 *         shared memory (ADR-053 D5; roadmap conversion-providers.md §3.5).
 *
 * One ring carries records in ONE direction between two processes: the
 * service -> conversion-host command ring and the host -> service completion
 * ring are two of these in one shm section (conv_host_wire_lane_open). The
 * memory comes from the caller (ipc_shmem_create / ipc_shmem_map); this file
 * never maps, allocates or signals anything, and includes no OS header.
 *
 * Layout: a 192-byte header (three 64-byte lines: immutable geometry, the
 * producer's head, the consumer's tail + its waiting flag) followed by
 * capacity x record_bytes. head and tail are free-running u32 counters; the
 * slot is counter & (capacity - 1).
 *
 * The peer is UNTRUSTED (the host runs third-party code and can scribble on
 * the shared section). Each side therefore keeps its own cursor locally and
 * only READS the peer's: a peer cursor that would put more than capacity
 * records in flight, or move backwards, makes every later call return
 * U_SHM_RING_CORRUPT, which the caller treats like a crash of the peer.
 *
 * Wake protocol (no lost wake; the OS wake signal is the caller's: an event,
 * an eventfd / socket byte, see os_process.h). The signal must LATCH: a
 * signal sent before the consumer waits still wakes it.
 *
 *   consumer                               producer
 *   --------                               --------
 *   while pop() == EMPTY:                  push(rec, &wake)
 *     if prepare_wait():                     (publishes head, full fence,
 *       wait(signal, timeout)                 reads the waiting flag)
 *       finish_wait()                      if wake: signal()
 *
 * prepare_wait() raises the waiting flag, issues a full fence and re-checks
 * the ring: if a record arrived in between it lowers the flag and returns
 * false (do not sleep). push() publishes the record, issues a full fence and
 * reports wake = true iff the flag is up. One of the two always sees the
 * other, so a record is never left behind a sleeping consumer. A spurious
 * wake (the flag still up from a consumer that already re-checked) costs one
 * empty pop.
 *
 * Only the four calls named per role may be made by that role; nothing here
 * takes a lock, and a ring is not safe for two producers or two consumers.
 *
 * @ingroup aux_util
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! "XRNG" read as a little-endian u32.
#define U_SHM_RING_MAGIC 0x474E5258u
#define U_SHM_RING_VERSION 1u

//! Bytes of the in-shm header that precedes the records.
#define U_SHM_RING_HEADER_BYTES 192u

//! Largest record (sanity bound on a peer-supplied geometry).
#define U_SHM_RING_MAX_RECORD_BYTES 65536u

//! Largest capacity (sanity bound on a peer-supplied geometry).
#define U_SHM_RING_MAX_CAPACITY 65536u

enum u_shm_ring_result
{
	U_SHM_RING_OK = 0,
	U_SHM_RING_EMPTY = 1,   //!< pop: nothing to read
	U_SHM_RING_FULL = 2,    //!< push: capacity records in flight; nothing written
	U_SHM_RING_CORRUPT = 3, //!< the peer's cursor or the header is invalid; the ring is dead
};

/*!
 * A process-local view of one ring. Plain data; zero-initialise, then
 * u_shm_ring_init (the creator) or u_shm_ring_attach (the other side).
 */
struct u_shm_ring
{
	volatile uint32_t *head; //!< in shm: written by the producer only
	volatile uint32_t *tail; //!< in shm: written by the consumer only
	volatile uint32_t *waiting; //!< in shm: written by the consumer only
	uint8_t *records;
	uint32_t record_bytes;
	uint32_t capacity;
	uint32_t mask;
	uint32_t local_head; //!< producer's own cursor (authoritative on its side)
	uint32_t local_tail; //!< consumer's own cursor (authoritative on its side)
	bool corrupt;
};

/*!
 * Bytes a ring of @p capacity records of @p record_bytes needs, header
 * included, rounded up to 64. 0 if the geometry is invalid: capacity must be
 * a power of two in [2, U_SHM_RING_MAX_CAPACITY], record_bytes a multiple of 8
 * in [8, U_SHM_RING_MAX_RECORD_BYTES].
 */
size_t
u_shm_ring_bytes(uint32_t record_bytes, uint32_t capacity);

/*!
 * Creator: lay the ring out in @p mem (@p mem_bytes >= u_shm_ring_bytes, 8-
 * aligned) and attach @p r to it. Zeroes the section. false = bad geometry.
 */
bool
u_shm_ring_init(struct u_shm_ring *r, void *mem, size_t mem_bytes, uint32_t record_bytes, uint32_t capacity);

/*!
 * Other side: attach @p r to a ring u_shm_ring_init laid out in @p mem. The
 * header must match the EXPECTED geometry exactly (both sides know the record
 * type); the cursors are adopted as found. false = mismatch / bad header.
 */
bool
u_shm_ring_attach(struct u_shm_ring *r, void *mem, size_t mem_bytes, uint32_t record_bytes, uint32_t capacity);

/*!
 * Producer: copy one record (record_bytes) in. On U_SHM_RING_OK, *out_wake
 * (may be NULL) says whether the consumer is waiting and must be signalled.
 */
enum u_shm_ring_result
u_shm_ring_push(struct u_shm_ring *r, const void *record, bool *out_wake);

//! Consumer: copy the oldest record (record_bytes) out.
enum u_shm_ring_result
u_shm_ring_pop(struct u_shm_ring *r, void *record);

/*!
 * Consumer: about to sleep. true = the ring is still empty and the waiting
 * flag is up: wait on the wake signal (with a timeout), then finish_wait().
 * false = a record arrived (or the ring is corrupt): do not sleep, pop.
 */
bool
u_shm_ring_prepare_wait(struct u_shm_ring *r);

//! Consumer: lower the waiting flag after a wait (signalled or timed out).
void
u_shm_ring_finish_wait(struct u_shm_ring *r);

/*!
 * Records in flight as seen from this side (producer: its head minus the
 * consumer's tail; consumer: the producer's head minus its tail). Advisory.
 */
uint32_t
u_shm_ring_count(struct u_shm_ring *r);

//! True once any call returned U_SHM_RING_CORRUPT.
static inline bool
u_shm_ring_is_corrupt(const struct u_shm_ring *r)
{
	return r->corrupt;
}

#ifdef __cplusplus
}
#endif
