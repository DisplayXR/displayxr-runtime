// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Conversion job lane (ADR-053 decision 3; roadmap
 *         conversion-providers.md §2.7): one-shot conversions on their own
 *         worker per provider host, with progress, cooperative cancel and
 *         result staging (conv_stage).
 *
 *  - Scope: per connection; jobs die with it. ≤ 16 queued per connection,
 *    then XRT_ERROR_CLIENT_LIMIT_REACHED.
 *  - Queue: one per host; INTERACTIVE before NORMAL before BACKGROUND, FIFO
 *    within a priority.
 *  - Worker: one job thread per host, separate from the stream dispatcher,
 *    so a splat job can no longer stall a video stream (G8). Whether the
 *    provider runs a job beside streams is still its declaration
 *    (LANES_CONCURRENT); selection routes a job away from a provider that
 *    would block live streams when another candidate exists.
 *  - Readiness: a job never fails because its task is not READY: it is
 *    WAITING_FOR_PROVIDER and warms it.
 *  - Progress: the provider callback; events rate-limited to 2 Hz and 5 %
 *    steps (state changes always go out).
 *  - Cancel: a flag the provider polls through is_cancelled (≤ 250 ms by
 *    contract); a queued or waiting job cancels at once.
 *  - Inputs: decoded pixels only; CPU images are copied at submit, GPU images
 *    must already be runtime-owned snapshots. Never a file path.
 *  - Results: chunked blobs (no 256 MiB latch) or a caller-opened file.
 *
 * @ingroup aux_conversion
 */

#pragma once

#include "xrt/xrt_conversion_plugin.h"

#include "conv_stage.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! @name Job status — values equal XrLiftJobStatusDXR (spec 5) @{ */
#define CONV_JOB_QUEUED 1u
#define CONV_JOB_WAITING_FOR_PROVIDER 2u
#define CONV_JOB_RUNNING 3u
#define CONV_JOB_SUCCEEDED 4u
#define CONV_JOB_FAILED 5u
#define CONV_JOB_CANCELLED 6u
/*! @} */

/*! @name Job priority — values equal XrLiftJobPriorityDXR @{ */
#define CONV_JOB_PRIORITY_BACKGROUND 1u
#define CONV_JOB_PRIORITY_NORMAL 2u
#define CONV_JOB_PRIORITY_INTERACTIVE 3u
/*! @} */

//! Queued (not yet finished) jobs per connection.
#define CONV_JOB_MAX_QUEUED_PER_CLIENT 16u
//! Progress events: at most every 500 ms ...
#define CONV_JOB_PROGRESS_MIN_INTERVAL_NS (500ull * 1000ull * 1000ull)
//! ... and only for steps of at least 5 %.
#define CONV_JOB_PROGRESS_MIN_STEP 0.05f

struct conv_job_create_info
{
	uint32_t task;     //!< must offer XRT_CONV_LANE_JOB
	uint32_t priority; //!< CONV_JOB_PRIORITY_*; 0 → NORMAL
	uint32_t input_kind;
	uint32_t requested_outputs; //!< XRT_CONV_OUT_*; 0 = the task's primary
	uint32_t requested_blob_format;
	uint32_t image_count;
	uint32_t image_role[XRT_CONV_MAX_FRAME_IMAGES];
	struct xrt_conv_image images[XRT_CONV_MAX_FRAME_IMAGES];
	struct xrt_conv_params params;
	/*! Optional caller-opened file for the PRIMARY output (the service has
	 *  checked it is a disk file with write access). */
	conv_stage_file_write_fn file_write;
	void *file_ctx;
	uint64_t max_result_bytes; //!< in-memory limit per output; 0 = none

	/* ADR-057 §4 job inputs, copied to xrt_conv_job_in. params.stereo.baseline_m
	 * is validated at submit (finite, 0 <= b <= XRT_CONV_MAX_BASELINE_M; 0 =
	 * unknown, never defaulted). */
	uint32_t job_flags;           //!< XRT_CONV_JOB_*
	uint32_t max_input_long_side; //!< px; 0 = the provider's own
};

struct conv_job_status
{
	uint64_t job_id;
	uint32_t status;         //!< CONV_JOB_*
	uint32_t failure_reason; //!< XRT_CONV_FAIL_* / CONV_REASON_*
	float progress;          //!< [0,1]; < 0 = unknown
	uint64_t elapsed_ns;
	char stage[32];
	uint32_t output_count;
};

#ifdef __cplusplus
}
#endif
