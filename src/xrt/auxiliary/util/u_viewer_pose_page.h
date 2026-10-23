// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The viewer pose page: one shared-memory page of timestamped
 *         viewer-tracking samples, written by ONE producer and read by any
 *         number of processes (ADR-054 N8; tracking-provider-discovery.md
 *         §4a "Out-of-process hosting").
 *
 * The page is how a viewer-tracking provider that runs in its own host
 * process (`displayxr-tracking-host`, supervised by the service) delivers its
 * samples: the provider still pushes `struct xrt_tracking_viewer_sample`
 * into an `xrt_tracking_sink` (the ABI is unchanged); in the host the sink is
 * @ref u_vpp_write_sample. Readers — the service's view path and any
 * in-process runtime on the same desktop — copy the new samples into their
 * own `u_eye_history` ring and sample it at their target time exactly as
 * before.
 *
 * This file is PURE: it lays the page out in memory the caller provides
 * (os/os_shm.h maps it) and includes no OS header. It never blocks, never
 * allocates and takes no lock.
 *
 * ## Layout (version @ref U_VPP_VERSION; byte offsets are the wire format)
 *
 * | Offset | Block | Written by |
 * |---|---|---|
 * | 0    | geometry: magic, version, sizes, owner pid + creation time | the owner, once |
 * | 64   | `write_count` (samples published), `heartbeat_ns` | the producer |
 * | 128  | owner status (seqlock): supervisor state, restarts, generation, host pid, provider id | the owner |
 * | 320  | producer status (seqlock): producer pid, generation, provider status, tracks, source, camera key | the
 * producer | | 768  | `demand_ns`: the last time a reader needed a viewer | any reader | | 1024 | @ref U_VPP_RING_SIZE
 * slots of `{seq, sample}` | the producer |
 *
 * The **owner** creates the page (the service); the **producer** writes the
 * samples (the host process, or the service itself when it hosts the provider
 * in-process). A slot holding sample number `k` carries `seq = 2k + 2` once
 * written and `2k + 1` while being written (a seqlock per slot); `write_count`
 * is published after the slot, so every sample below it is complete unless
 * the producer has lapped it since. A reader re-checks the slot's `seq`
 * after copying: a changed or odd value is a torn read (retried), a larger
 * even value a lapped sample (dropped, counted).
 *
 * The producer is UNTRUSTED (it runs third-party code): the reader validates
 * the geometry once (@ref u_vpp_attach_check) and bounds every index it
 * reads; a nonsense `write_count` only costs it samples.
 *
 * Liveness is the caller's business, from what the page reports: the owner's
 * and the producer's pids (os_process_is_alive) and the producer's heartbeat
 * (@ref U_VPP_HEARTBEAT_STALE_NS). A stale page means "not tracking".
 *
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_tracking_plugin.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! "DVPP" read as a little-endian u32.
#define U_VPP_MAGIC 0x50505644u
//! Layout version; a reader accepts exactly this one.
#define U_VPP_VERSION 1u
//! Samples the page keeps (a quarter second at 60 Hz — the reader drains far more often).
#define U_VPP_RING_SIZE 16u
//! Bytes before the first slot.
#define U_VPP_HEADER_BYTES 1024u
//! Total bytes a page needs (header + slots, rounded to a 4 KiB page).
#define U_VPP_PAGE_BYTES 8192u
//! Size of the provider id / source / camera key strings in the page (NUL included).
#define U_VPP_ID_MAX 64u
#define U_VPP_TEXT_MAX 128u

//! Producer heartbeat age past which the producer counts as gone (it beats every ~100 ms).
#define U_VPP_HEARTBEAT_STALE_NS ((int64_t)1500 * 1000 * 1000)
//! A reader's `demand_ns` younger than this keeps the provider wanted.
#define U_VPP_DEMAND_FRESH_NS ((int64_t)5000 * 1000 * 1000)

//! What the owner (the service's supervisor) says the hosting is doing.
enum u_vpp_state
{
	U_VPP_STATE_NONE = 0,     //!< the page exists, no provider decided (or torn down)
	U_VPP_STATE_IDLE = 1,     //!< a provider is decided, not started yet (starts on the first viewer request)
	U_VPP_STATE_STARTING = 2, //!< the host is being spawned / the provider started
	U_VPP_STATE_RUNNING = 3,  //!< the producer is up
	U_VPP_STATE_BACKOFF = 4,  //!< the host exited abnormally; it is restarted after a backoff
	U_VPP_STATE_FAILED = 5,   //!< crash loop / unusable: not restarted until the service restarts
	U_VPP_STATE_STOPPED = 6,  //!< the owner shut down
};

//! Stable lower-case name of @p state ("none", "idle", "starting", "running", "backoff", "failed", "stopped").
const char *
u_vpp_state_str(uint32_t state);

/*!
 * The owner's status block, as copied out by @ref u_vpp_read_owner.
 */
struct u_vpp_owner_status
{
	uint32_t state;      //!< enum u_vpp_state
	uint32_t restarts;   //!< abnormal host exits since the owner created the page
	uint32_t generation; //!< host spawns so far (the producer echoes the one it was spawned as)
	int64_t host_pid;    //!< the host process the owner runs now (0 = none / in-process)
	bool in_process;     //!< the owner itself hosts the provider (no host binary)
	char provider_id[U_VPP_ID_MAX];
};

/*!
 * The producer's status block, as copied out by @ref u_vpp_read_producer.
 */
struct u_vpp_producer_status
{
	int64_t pid;              //!< 0 = no producer has started
	uint32_t generation;      //!< the generation it was started as
	uint32_t provider_status; //!< enum xrt_tracking_provider_status
	uint32_t tracks;          //!< XRT_TRACKING_TRACKS_* (describe)
	char provider_id[U_VPP_ID_MAX];
	char source[U_VPP_TEXT_MAX];
	char camera_key[U_VPP_TEXT_MAX];
};

//! The geometry block, as copied out by @ref u_vpp_read_geometry.
struct u_vpp_geometry
{
	uint32_t magic;
	uint32_t version;
	uint32_t header_size;
	uint32_t ring_size;
	uint32_t slot_stride;
	uint32_t sample_size;
	uint64_t total_size;
	int64_t owner_pid;
	int64_t owner_created_ns; //!< monotonic: a re-created page has a new value
};


/*
 * Owner.
 */

/*!
 * Lay a fresh page out in @p mem (@p mem_bytes >= @ref U_VPP_PAGE_BYTES,
 * 8-aligned): zeroes it, writes the geometry, then the magic last (a reader
 * racing the init sees no magic). false = too small / misaligned.
 */
bool
u_vpp_init(void *mem, size_t mem_bytes, int64_t owner_pid, int64_t now_ns);

//! Publish the owner's status block (seqlock; owner thread only).
void
u_vpp_write_owner(void *mem, const struct u_vpp_owner_status *st);


/*
 * Producer.
 */

/*!
 * The producer's process-local cursor. Zero-init, then @ref u_vpp_producer_begin.
 */
struct u_vpp_producer
{
	void *mem;
	uint64_t next; //!< next sample number to write (continues the page's count)
};

/*!
 * Start producing into an attached page (@ref u_vpp_attach_check passed):
 * the count continues where the previous producer stopped, so readers'
 * cursors stay valid across a host restart.
 */
void
u_vpp_producer_begin(struct u_vpp_producer *p, void *mem);

/*!
 * Publish one sample (the sink's push). Copies at most the page's
 * `sample_size` bytes; a sample shorter than that is zero-extended. Any
 * thread, but ONE producing thread at a time.
 */
void
u_vpp_write_sample(struct u_vpp_producer *p, const struct xrt_tracking_viewer_sample *s);

//! Publish the producer's status block (seqlock; the producer's status thread only).
void
u_vpp_write_producer(void *mem, const struct u_vpp_producer_status *st);

//! Beat: the producer is alive at @p now_ns.
void
u_vpp_heartbeat(void *mem, int64_t now_ns);


/*
 * Readers.
 */

/*!
 * Validate a mapped page of @p mem_bytes: magic, version, header size, ring
 * size, a slot stride that holds a sample, a total that fits the mapping.
 * Call it once per mapping; every other reader call assumes it passed.
 */
bool
u_vpp_attach_check(const void *mem, size_t mem_bytes);

void
u_vpp_read_geometry(const void *mem, struct u_vpp_geometry *out);

//! Copy the owner's block; false when it kept changing (a writer stuck mid-update): @p out is then zeroed.
bool
u_vpp_read_owner(const void *mem, struct u_vpp_owner_status *out);

//! Copy the producer's block; false when it kept changing: @p out is then zeroed.
bool
u_vpp_read_producer(const void *mem, struct u_vpp_producer_status *out);

//! The producer's last heartbeat (0 = never).
int64_t
u_vpp_read_heartbeat(const void *mem);

//! Samples published so far.
uint64_t
u_vpp_read_write_count(const void *mem);

//! A reader needs a viewer now (readers map the page writable for this one word).
void
u_vpp_post_demand(void *mem, int64_t now_ns);

//! The last demand any reader posted (0 = none).
int64_t
u_vpp_read_demand(const void *mem);

//! A reader's process-local cursor. Zero-init = start from the oldest sample still on the page.
struct u_vpp_reader
{
	uint64_t next;    //!< next sample number to read
	bool started;     //!< false until the first drain (which starts at the oldest kept sample)
	uint64_t dropped; //!< samples lapped by the producer before this reader copied them
	uint64_t torn;    //!< torn reads that were retried
};

/*!
 * Copy every sample published since the last call, oldest first, into
 * @p out (at most @p max; older ones beyond that are dropped). Returns the
 * count. A sample the producer has overwritten meanwhile is dropped
 * (`r->dropped`); a torn read is retried a few times, then dropped.
 */
uint32_t
u_vpp_read_new(const void *mem, struct u_vpp_reader *r, struct xrt_tracking_viewer_sample *out, uint32_t max);

#ifdef __cplusplus
}
#endif
