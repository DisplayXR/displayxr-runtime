// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Splat facts as JSON, and the convenience camera-block writer
 *         (ADR-057 §3).
 *
 * The runtime reports facts and suggestions; it never writes a content file's
 * camera block. This is the CONVENIENCE writer a tool may call on the author's
 * behalf (`displayxr-cli conv run --camera-block auto`, `conv camera-block`):
 * it turns facts + suggestions into a SOG `meta.json` `camera` block (v2:
 * convention, rig, rest, intrinsics, stereo.baseline_m only when metric,
 * metric, focus, dxr) marked `"source": "auto"`, and it NEVER overwrites a
 * block it did not write: an existing `camera` whose `source` is not `"auto"`
 * is the author's, and the file is left untouched.
 *
 * Facts JSON is the shape of contract v1's `X-DXR-Result` (ADR-057 §6), so a
 * remote endpoint's header, `conv run --facts` and this writer share one
 * vocabulary.
 *
 * SOG archives are rewritten entry by entry: every other entry keeps its
 * compressed bytes; `meta.json` is written stored. A `meta.json` that is
 * itself deflated is refused (writers of the contract store it).
 *
 * @ingroup aux_conversion
 */

#pragma once

#include "xrt/xrt_conversion_plugin.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cJSON cJSON;

//! Spark refuses a larger meta.json; the writer never produces one.
#define CONV_SOG_META_MAX_BYTES 65536u

/*!
 * The facts of a splat job as X-DXR-Result-shaped JSON (caller frees with
 * cJSON_Delete). @p sr may be NULL (no ADR-057 result: splat_info only).
 * Unknown values are omitted, never invented.
 */
cJSON *
conv_splat_facts_to_json(const struct xrt_conv_splat_info *si, const struct xrt_conv_splat_result *sr);

/*!
 * A v2 `camera` block, `"source": "auto"`, from facts JSON. The rig ("camera")
 * and the identity rest pose are the auto writer's defaults, which an author
 * overrides. `stereo.baseline_m` and `metric` appear only for a metric result;
 * `focus` only when the facts carry a suggestion. NULL (with @p err) when the
 * facts lack the intrinsics a block needs.
 */
cJSON *
conv_camera_block_from_facts(const cJSON *facts, char *err, size_t err_size);

enum conv_camera_block_result
{
	CONV_CAMERA_BLOCK_WRITTEN = 0,   //!< there was no block; ours was added
	CONV_CAMERA_BLOCK_REPLACED_AUTO, //!< an earlier "source": "auto" block was replaced
	CONV_CAMERA_BLOCK_KEPT_AUTHORED, //!< an authored block exists: nothing was written
	CONV_CAMERA_BLOCK_ERROR,
};

const char *
conv_camera_block_result_str(enum conv_camera_block_result r);

/*!
 * @p meta_json with @p block as its `camera` key (inserted right after `count`,
 * other keys in order), into a malloc'd *@p out (NULL when KEPT_AUTHORED or
 * ERROR).
 */
enum conv_camera_block_result
conv_camera_block_apply_meta(const char *meta_json, const cJSON *block, char **out, char *err, size_t err_size);

/*!
 * Apply @p block to the SOG at @p path: a bundled `.sog` (zip), an unbundled
 * `meta.json`, or a directory holding one. Atomic: written to a temp file and
 * renamed over the original.
 */
enum conv_camera_block_result
conv_camera_block_write(const char *path, const cJSON *block, char *err, size_t err_size);

/*! Read `meta.json` out of @p path (any of the three forms); malloc'd, NUL-terminated. */
char *
conv_sog_read_meta(const char *path, char *err, size_t err_size);

#ifdef __cplusplus
}
#endif
