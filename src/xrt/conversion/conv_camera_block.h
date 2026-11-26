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
#include <stdint.h>

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

/*! `meta.json` of a bundled SOG held in memory; malloc'd, NUL-terminated (NULL + @p err otherwise). */
char *
conv_sog_read_meta_mem(const uint8_t *sog, size_t size, char *err, size_t err_size);

/*!
 * Scale the `intrinsics` of facts JSON in place: fx, cx and width by @p sx;
 * fy, cy and height by @p sy (width / height rounded). The runtime downscaled
 * the caller's photo before the provider saw it, so the facts handed back are
 * re-expressed in the CALLER's pixels. 1, 1 (or a missing block) = no change.
 */
void
conv_splat_facts_scale_intrinsics(cJSON *facts, double sx, double sy);

/*!
 * Re-express a camera block's `intrinsics` in the caller's pixels, in place,
 * with EXACTLY the rule conv_splat_facts_scale_intrinsics applies to the facts
 * (fx, cx, width by @p sx; fy, cy, height by @p sy; width / height rounded), so
 * the block and the facts of one lift result agree to the same digits (#2836).
 * Only `intrinsics` changes: every other key, and the key order, is kept.
 *
 * No change (false) when @p block is not an object, it has no `intrinsics`,
 * the scale is 1, 1 (or not positive), or the intrinsics are malformed (not an
 * object, or one of the six keys present but not a finite number: logged once
 * as a WARN, the block is left exactly as authored). True when rescaled.
 */
bool
conv_camera_block_scale_intrinsics(cJSON *block, double sx, double sy);

/*!
 * The camera block a lift caller receives with a GAUSSIANS blob (XR_DXR_lift
 * spec 6): the SAME v2 object `conv run --camera-block auto` writes into a SOG,
 * built by the RUNTIME from the common facts schema whatever provider made the
 * splats, so a consumer parses it exactly like a SOG's `meta.json` camera.
 *
 * @p facts are already in the caller's pixels; @p src_scale_x / @p src_scale_y
 * are the scale they were given (conv_splat_facts_scale_intrinsics; 1, 1 when
 * the photo was not downscaled).
 *
 *  - A SOG blob that already carries an AUTHORED camera block (any `camera`
 *    whose `source` is not "auto"): that block, with only its `intrinsics`
 *    re-expressed in the caller's pixels (conv_camera_block_scale_intrinsics
 *    with @p src_scale_x / @p src_scale_y; verbatim at 1, 1). The result is a
 *    view of the file: the blob itself is never rewritten (#2836).
 *  - Else conv_camera_block_from_facts(); when the facts lack intrinsics, the
 *    same block without `intrinsics` (a viewer estimates them; never a
 *    nominal field of view).
 *  - `focus`: the provider's suggestion ("source": "auto", as the writer
 *    labels it); else, for a PLY, the runtime's own estimate from the splats
 *    (conv_splat_focus_from_ply, "source": "runtime"); else, for a SOG, the
 *    focus of its own auto block. No focus at all when none of these exist.
 *
 * @p conv_blob_format is XRT_CONV_BLOB_*. Never NULL for a PLY / SOG (the
 * caller frees with cJSON_Delete); NULL with @p err for other formats.
 */
cJSON *
conv_lift_camera_block(const cJSON *facts,
                       double src_scale_x,
                       double src_scale_y,
                       const uint8_t *blob,
                       size_t size,
                       uint32_t conv_blob_format,
                       char *err,
                       size_t err_size);

#ifdef __cplusplus
}
#endif
