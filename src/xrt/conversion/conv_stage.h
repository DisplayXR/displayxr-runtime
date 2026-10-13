// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Job result staging (ADR-053 decision 3; roadmap
 *         conversion-providers.md §2.7, §2.8): the byte sinks a provider's
 *         job_execute writes into, kept as chunked blobs or streamed into a
 *         caller-opened file handle.
 *
 * Retires the v3 two-call blob latch and its 256 MiB cap (G8): an output is a
 * list of fixed-size chunks with no transport limit, read back at any offset
 * (xrReadLiftJobResultDXR's chunked idiom). A caller that opened a file
 * (XrLiftJobFileOutputDXR — a handle, never a path; the service checks it is
 * a disk file with write access before use) gets the bytes written through
 * a write callback instead, and nothing is kept in memory. Delivery for the
 * conv_job_fetch reply: ≤ 1 MiB inline, else a shm section, or FILE.
 *
 * Thread-safety: the provider writes from the job worker while readers may
 * fetch only after the job finished; the stage itself takes no lock.
 *
 * @ingroup aux_conversion
 */

#pragma once

#include "xrt/xrt_conversion_plugin.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Outputs per job (primary + auxiliaries).
#define CONV_STAGE_MAX_OUTPUTS 4u
//! Chunk size of an in-memory output.
#define CONV_STAGE_CHUNK_BYTES (1024u * 1024u)
//! Inline delivery limit of conv_job_fetch.
#define CONV_STAGE_INLINE_MAX (1024u * 1024u)

/*! Write @p count bytes to the caller-opened file; false = stop (the sink
 *  then reports failure to the provider). */
typedef bool (*conv_stage_file_write_fn)(void *ctx, const void *bytes, uint64_t count);

struct conv_stage_output
{
	uint32_t role;        //!< XRT_CONV_OUT_* this output carries
	uint32_t blob_format; //!< XRT_CONV_BLOB_* announced by begin()
	uint64_t size_hint;
	uint64_t size;      //!< bytes received
	uint64_t max_bytes; //!< the stage's limit, copied at add_output
	bool begun;
	bool failed; //!< allocation / file write failure / over the limit

	/* In memory. */
	uint8_t **chunks;
	uint32_t chunk_count, chunk_cap;

	/* Or a caller-opened file. */
	conv_stage_file_write_fn file_write;
	void *file_ctx;
};

struct conv_stage
{
	struct conv_stage_output outs[CONV_STAGE_MAX_OUTPUTS];
	uint32_t count;
	uint64_t max_bytes; //!< per output, in memory; 0 = no limit
	struct xrt_conv_blob_sink sinks[CONV_STAGE_MAX_OUTPUTS];
};

void
conv_stage_init(struct conv_stage *st, uint64_t max_bytes);

//! Free every chunk.
void
conv_stage_fini(struct conv_stage *st);

/*!
 * Add one output for @p role (XRT_CONV_OUT_*); with @p file_write the bytes
 * go to the caller's file instead of memory. Returns the output index or -1.
 */
int32_t
conv_stage_add_output(struct conv_stage *st, uint32_t role, conv_stage_file_write_fn file_write, void *file_ctx);

/*!
 * Discard everything received so far and make every output writable again
 * (same roles, same sinks): a job re-run from its input after its host died.
 * false when an output streams to a caller's file and already wrote bytes
 * (those cannot be taken back).
 */
bool
conv_stage_reset(struct conv_stage *st);

//! The provider-facing sinks (one per output), valid while @p st lives.
struct xrt_conv_blob_sink *
conv_stage_sinks(struct conv_stage *st, uint32_t *out_count);

//! Output index of @p role, or -1.
int32_t
conv_stage_find(const struct conv_stage *st, uint32_t role);

//! Bytes received on output @p index (0 for an unknown index).
uint64_t
conv_stage_size(const struct conv_stage *st, uint32_t index);

/*! Copy up to @p cap bytes of output @p index from @p offset; returns the
 *  bytes copied (0 past the end, for a file output, or an unknown index). */
uint64_t
conv_stage_read(const struct conv_stage *st, uint32_t index, uint64_t offset, void *buf, uint64_t cap);

/*! conv_job_fetch delivery for output @p index (XRT_CONV_WIRE_DELIVERY_*
 *  values: 0 none, 1 inline, 2 shm, 3 file). */
uint32_t
conv_stage_delivery(const struct conv_stage *st, uint32_t index);

#ifdef __cplusplus
}
#endif
