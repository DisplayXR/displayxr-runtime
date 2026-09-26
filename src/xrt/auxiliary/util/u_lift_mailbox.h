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

	//! publish → publish interval, for the effective conversion rate.
	uint64_t last_publish_ns;
	uint64_t interval_ema_ns; //!< alpha = 1/8; 0 until two results exist
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
u_lift_mailbox_commit_submit(
    struct u_lift_mailbox *mb, int32_t slot, int64_t source_time, uint64_t now_ns, uint32_t width, uint32_t height);

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

//! Effective conversion rate in results per second (0 until two results).
float
u_lift_mailbox_rate_hz(const struct u_lift_mailbox *mb);


/*
 *
 * Cross-stream scheduling (XrLiftPriorityDXR).
 *
 * The module converts one frame at a time, so concurrent streams share it. The
 * lift thread asks for a PLAN each round and converts the planned streams in
 * order:
 *
 *  - every HIGH stream with a pending frame;
 *  - ONE NORMAL stream with a pending frame, round-robin;
 *  - every LOW stream with a pending frame, only on every
 *    U_LIFT_LOW_EVERY_N-th round;
 *  - PAUSED streams never.
 *
 * A round is counted only when something (non-paused) was pending, so an idle
 * service does not "use up" LOW rounds.
 *
 */

enum u_lift_priority
{
	U_LIFT_PRIORITY_PAUSED = 0,
	U_LIFT_PRIORITY_LOW = 1,
	U_LIFT_PRIORITY_NORMAL = 2,
	U_LIFT_PRIORITY_HIGH = 3,
};

//! LOW streams convert on every Nth round.
#define U_LIFT_LOW_EVERY_N 4

struct u_lift_sched
{
	uint64_t round;
	uint64_t normal_rr_last; //!< id of the NORMAL stream served last
};

//! One stream as the scheduler sees it. Entries must be in ascending id order.
struct u_lift_sched_entry
{
	uint64_t id;
	uint32_t priority; //!< enum u_lift_priority
	bool pending;
};

void
u_lift_sched_init(struct u_lift_sched *s);

/*!
 * Plan one round: write up to @p max stream ids to convert, in order, to
 * @p out_ids and return how many. May return 0 with a pending LOW stream (not
 * its round) — call again.
 */
uint32_t
u_lift_sched_plan(
    struct u_lift_sched *s, const struct u_lift_sched_entry *entries, uint32_t count, uint64_t *out_ids, uint32_t max);


/*
 *
 * Snapshot size cap (service policy, ADR-042).
 *
 * A lift-flagged weave rect is snapshotted at DEVICE pixels, and the module
 * synthesizes its views at INPUT resolution — a fullscreen player on an 8K
 * panel is a 7680x4319 input and a 15360-wide SBS per frame. The result is
 * stretched back into the rect's current position anyway, so the service
 * downsamples the snapshot before the DP. An app's explicit
 * xrSubmitLiftFrameDXR frame is NOT capped: its size is the app's choice.
 *
 */

//! Default cap on the snapshot's long edge (DXR_LIFT_MAX_INPUT_EDGE unset).
#define U_LIFT_MAX_INPUT_EDGE_DEFAULT 1920u

//! Smallest cap honoured; lower non-zero values clamp to it.
#define U_LIFT_MAX_INPUT_EDGE_MIN 256u

/*!
 * Parse a DXR_LIFT_MAX_INPUT_EDGE value. NULL, empty or non-numeric = the
 * default; "0" = no cap (returns 0); 1..255 clamp to
 * U_LIFT_MAX_INPUT_EDGE_MIN.
 */
uint32_t
u_lift_max_input_edge_parse(const char *value);

/*!
 * Scale @p w x @p h so the long edge is at most @p cap, keeping the aspect.
 * The long edge becomes @p cap rounded DOWN to even; the short edge is scaled
 * by the same factor and rounded to the NEAREST even value, minimum 2. When
 * @p cap is 0 or the long edge already fits, the dims pass through unchanged
 * and the function returns false; true = the dims were reduced.
 */
bool
u_lift_cap_dims(uint32_t w, uint32_t h, uint32_t cap, uint32_t *out_w, uint32_t *out_h);


/*
 *
 * Letterbox crop (service policy, ADR-042).
 *
 * A lift-flagged weave rect often holds a film with black bars (2.39:1 in a
 * 16:9 player), sometimes with subtitles drawn in the bars. Converting the bars
 * wastes module time, and the hard black edge confuses depth. The service
 * measures, per row / column of the rect, the fraction of non-black pixels
 * (GPU reduction, read back asynchronously), and this helper turns those
 * profiles into a stable crop: the lifted input is the ACTIVE area only, and
 * the bars (subtitles included) are woven flat, identical in both eyes.
 *
 * A bar is the run of rows (columns) from an edge whose non-black fraction
 * stays below U_LIFT_LETTERBOX_PICTURE_FRAC — low enough that subtitle text in
 * a bar does not end it, high enough that picture rows do. Bars GROW only after
 * the same measurement has held for U_LIFT_LETTERBOX_SETTLE_FRAMES frames that
 * carry content (a fade or cut to black is not a letterbox), and SHRINK at once
 * when picture appears in them. A wrong crop in a dark scene costs little: the
 * cropped rows are dark and are simply woven flat.
 *
 */

//! Row / column profile length cap (buckets per axis).
#define U_LIFT_LETTERBOX_BINS_MAX 512u

//! A bucket whose non-black fraction reaches this is picture intruding into a
//! bar (the SHRINK test). Subtitle text stays below it or is caught by the
//! symmetry rule.
#define U_LIFT_LETTERBOX_PICTURE_FRAC 0.25f

//! A bar only GROWS into buckets at most this lit (truly black): a dark scene
//! edge is rarely this empty, so dark stretches do not crop picture.
#define U_LIFT_LETTERBOX_BLACK_FRAC 0.03f

//! Consecutive frames picture must intrude into a bar before it shrinks — one
//! caption frame must not un-crop; a 16:9 ad still un-crops within ~0.1 s.
#define U_LIFT_LETTERBOX_SHRINK_FRAMES 6u

/*!
 * Rows below this non-black fraction count as sparse (subtitle text, not
 * picture) when a bar is extended to match the opposite one — see
 * u_lift_letterbox_update's symmetry rule. Dense subtitles can pass
 * U_LIFT_LETTERBOX_PICTURE_FRAC; a picture edge row stays above this.
 */
#define U_LIFT_LETTERBOX_SPARSE_FRAC 0.6f

//! Frames a larger bar must hold before the crop grows into it.
#define U_LIFT_LETTERBOX_SETTLE_FRAMES 45u

//! Bars below this many pixels are ignored (no crop on that edge).
#define U_LIFT_LETTERBOX_MIN_BAR_PX 4u

//! The active area must keep at least this fraction of each dimension.
#define U_LIFT_LETTERBOX_MIN_ACTIVE_FRAC 0.3f

//! Crop of a @c w x @c h rect: bar sizes in rect pixels (0 = no bar).
struct u_lift_crop
{
	uint32_t top, bottom, left, right;
};

struct u_lift_letterbox
{
	uint32_t w, h;                //!< rect dims the state belongs to (0 = none yet)
	struct u_lift_crop committed; //!< the crop in effect
	struct u_lift_crop pending;   //!< a larger crop waiting to settle
	uint32_t pending_frames;      //!< consecutive content frames @c pending held
	struct u_lift_crop shrink;    //!< a smaller crop (picture in a bar) waiting to hold
	uint32_t shrink_frames;       //!< consecutive content frames @c shrink held
};

/*!
 * Parse DXR_LIFT_LETTERBOX: NULL / empty / anything but "0" = enabled.
 */
bool
u_lift_letterbox_parse(const char *value);

/*!
 * Feed one measurement of a @p w x @p h rect. @p rows holds @p nr per-bucket
 * non-black fractions top to bottom (bucket i covers rows [i*h/nr, (i+1)*h/nr)),
 * @p cols @p nc buckets left to right; either may be NULL/0 (that axis then
 * never crops). A change of @p w / @p h resets the state (no crop until the new
 * bars settle). Returns true when the committed crop changed.
 */
bool
u_lift_letterbox_update(struct u_lift_letterbox *lb,
                        uint32_t w,
                        uint32_t h,
                        const float *rows,
                        uint32_t nr,
                        const float *cols,
                        uint32_t nc);

//! True when @p c crops anything.
bool
u_lift_crop_active(const struct u_lift_crop *c);


#ifdef __cplusplus
}
#endif
