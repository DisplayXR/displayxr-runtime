// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift (ADR-042): the per-stream bookkeeping between the
 *         submitting thread, the conversion thread and the consumers.
 *
 * A lift stream moves frames through three hands:
 *
 *  - PRODUCER — an IPC thread (xrSubmitLiftFrameDXR, or a lift-flagged weave
 *    rect inside xrWeaveSubmitDXR). It snapshots the caller's pixels into an
 *    INPUT SLOT and must never wait for the model.
 *  - WORKER — the one lift thread. It takes the newest pending input, runs the
 *    vendor module (synchronous, tens of ms or seconds), and copies the module's
 *    output — valid only until the module's next call — into an OUTPUT SLOT.
 *  - CONSUMERS — the weave (every frame, "latest result, whatever it is") and
 *    xrAcquireLiftResultDXR ("latest result newer than the one I already have").
 *    A consumer PINS the slot it reads so the worker can never overwrite it
 *    mid-copy.
 *
 * Two input slots make the mailbox LATEST-WINS without ever blocking the
 * producer: at most one slot is CONVERTING, so the other one is always
 * writable; a pending frame the producer overwrites (or that a newer commit
 * supersedes) is DROPPED and counted — never queued. Two output slots (the
 * ring) let the worker write the next result while a consumer reads the latest;
 * the worker waits only while a stale slot is still pinned, which lasts one
 * GPU-copy issue.
 *
 * This file is the pure state machine — no locks, no GPU, no clock. The caller
 * serializes every call under its own stream mutex and passes timestamps in, so
 * the whole contract (slot choice, drop accounting, frame ids, newer-than
 * acquire, pin exclusion, latency stats) is pinned host-side by
 * tests/tests_lift_mailbox.cpp. The D3D11 service (d3d11_lift.cpp) is the one
 * user today.
 *
 * @ingroup aux_util
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Input slots per stream. Two is the minimum for a never-blocking producer.
#define U_LIFT_INPUT_SLOTS 2
//! Output ring slots per stream (the module's output is only valid until its
//! next call, so every result is copied into one of these).
#define U_LIFT_RING_SIZE 2

enum u_lift_in_state
{
	U_LIFT_IN_FREE = 0,
	U_LIFT_IN_WRITING,    //!< producer is snapshotting into it
	U_LIFT_IN_PENDING,    //!< complete, waiting for the worker
	U_LIFT_IN_CONVERTING, //!< the worker owns it
};

enum u_lift_out_state
{
	U_LIFT_OUT_EMPTY = 0,
	U_LIFT_OUT_WRITING, //!< the worker is copying a result into it
	U_LIFT_OUT_READY,   //!< holds a complete result
};

//! Everything the runtime knows about one frame's trip through a stream.
struct u_lift_frame_meta
{
	uint64_t frame_id;         //!< per stream, monotonic from 1 (0 = none)
	int64_t source_time;       //!< caller's timestamp, echoed verbatim
	uint64_t submit_ns;        //!< producer commit time (runtime clock)
	uint64_t convert_start_ns; //!< worker took it
	uint64_t done_ns;          //!< result published
	uint32_t width, height;    //!< input extent
};

struct u_lift_mailbox
{
	enum u_lift_in_state in_state[U_LIFT_INPUT_SLOTS];
	struct u_lift_frame_meta in_meta[U_LIFT_INPUT_SLOTS];

	enum u_lift_out_state out_state[U_LIFT_RING_SIZE];
	struct u_lift_frame_meta out_meta[U_LIFT_RING_SIZE];
	uint32_t out_pins[U_LIFT_RING_SIZE];
	//! Ring slot holding the newest result, -1 = none yet.
	int32_t latest;

	//! Last frame id handed out; the next commit gets this + 1.
	uint64_t last_frame_id;
	//! Newest frame id a "newer-than" acquire has returned.
	uint64_t last_acquired_frame_id;

	//! Counters (monotonic).
	uint64_t submitted; //!< committed frames
	uint64_t dropped;   //!< pending frames superseded before the worker took them
	uint64_t converted; //!< results published
	uint64_t failed;    //!< conversions the worker abandoned

	//! submit → published latency, ns.
	uint64_t lat_last_ns;
	uint64_t lat_min_ns;
	uint64_t lat_max_ns;
	uint64_t lat_ema_ns; //!< exponential moving average, alpha = 1/8
};

//! Reset @p mb to "no frames, no results".
void
u_lift_mailbox_init(struct u_lift_mailbox *mb);

/*
 * Producer.
 */

/*!
 * Pick an input slot to snapshot into and mark it WRITING. Prefers a FREE slot;
 * otherwise overwrites the PENDING one (that frame is dropped and counted).
 * Fails only if no slot is FREE or PENDING — impossible with one producer per
 * stream, reported rather than asserted because the wire is untrusted.
 */
bool
u_lift_mailbox_begin_submit(struct u_lift_mailbox *mb, int32_t *out_slot);

/*!
 * The snapshot into @p slot is complete: assign the next frame id, mark it
 * PENDING, and drop any OLDER pending frame (latest wins). Returns the frame id.
 */
uint64_t
u_lift_mailbox_commit_submit(struct u_lift_mailbox *mb,
                             int32_t slot,
                             int64_t source_time,
                             uint64_t now_ns,
                             uint32_t width,
                             uint32_t height);

//! The snapshot into @p slot failed: return it to FREE (no frame id consumed).
void
u_lift_mailbox_abort_submit(struct u_lift_mailbox *mb, int32_t slot);

/*
 * Worker.
 */

//! True when an input is waiting for the worker.
bool
u_lift_mailbox_has_pending(const struct u_lift_mailbox *mb);

/*!
 * Take the newest PENDING input (-> CONVERTING), stamping @p now_ns as its
 * conversion start. False when nothing is pending.
 */
bool
u_lift_mailbox_take_pending(struct u_lift_mailbox *mb,
                            uint64_t now_ns,
                            int32_t *out_slot,
                            struct u_lift_frame_meta *out_meta);

//! The worker is done reading input @p slot (-> FREE).
void
u_lift_mailbox_finish_input(struct u_lift_mailbox *mb, int32_t slot);

/*!
 * Pick an output slot to write: never the latest result, never a pinned slot,
 * never one already being written. False = wait (a consumer still holds the
 * only candidate).
 */
bool
u_lift_mailbox_begin_output(struct u_lift_mailbox *mb, int32_t *out_slot);

//! Result copied into @p slot: it becomes the latest; latency stats update.
void
u_lift_mailbox_publish_output(struct u_lift_mailbox *mb,
                              int32_t slot,
                              const struct u_lift_frame_meta *meta,
                              uint64_t now_ns);

//! The worker gave up on @p slot's result (-> EMPTY; failed++).
void
u_lift_mailbox_abort_output(struct u_lift_mailbox *mb, int32_t slot);

/*
 * Consumers.
 */

/*!
 * Pin the latest result for reading. With @p only_newer, succeed only when it
 * is newer than the last result a newer-than pin returned (and record it as
 * returned); without, always return the latest (the weave's case). False when
 * no result qualifies. Every successful pin must be matched by one
 * @ref u_lift_mailbox_unpin.
 */
bool
u_lift_mailbox_pin_latest(struct u_lift_mailbox *mb,
                          bool only_newer,
                          int32_t *out_slot,
                          struct u_lift_frame_meta *out_meta);

void
u_lift_mailbox_unpin(struct u_lift_mailbox *mb, int32_t slot);

//! True once any result has been published (and not since aborted).
bool
u_lift_mailbox_has_result(const struct u_lift_mailbox *mb);


#ifdef __cplusplus
}
#endif
