// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pure helpers over @ref xrt_status_snapshot (ADR-051): warning
 *         derivation, JSON and text serialisation.
 *
 * No instance, no loader, no service: everything here reads only the snapshot,
 * so it is unit-testable on a synthetic one (`tests/tests_status_snapshot.cpp`).
 * The collectors live where their dependencies do — the headless builder in
 * `targets/common/target_status_snapshot.h`, the service builder (phase 2) next
 * to the IPC server.
 *
 * Runtime-derived warning codes (design §4):
 *
 * | code               | level    | scope  |
 * |--------------------|----------|--------|
 * | `NOT_NATIVE`       | critical | screen |
 * | `CLAIM_FALLBACK`   | warn     | screen |
 * | `CLAIM_EDID_ONLY`  | info     | screen |
 * | `NO_PHYSICAL_SIZE` | warn     | screen |
 * | `SEGMENT_FLAT_2D`  | warn     | screen |
 * | `TRACKER_DOWN`     | warn     | screen |
 * | `PLUGIN_NOT_READY` | critical | screen (system when the active plug-in claims none) |
 * | `SERVICE_HEADLESS` | info     | system |
 * | `DP_DEGRADED`      | warn     | screen |
 * | `DP_STALE`         | critical | screen |
 *
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_display_status.h"

#include <stddef.h>

typedef struct cJSON cJSON;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @name Warning codes (§4)
 * @{
 */
#define U_STATUS_W_NOT_NATIVE "NOT_NATIVE"
#define U_STATUS_W_CLAIM_FALLBACK "CLAIM_FALLBACK"
#define U_STATUS_W_CLAIM_EDID_ONLY "CLAIM_EDID_ONLY"
#define U_STATUS_W_NO_PHYSICAL_SIZE "NO_PHYSICAL_SIZE"
#define U_STATUS_W_SEGMENT_FLAT_2D "SEGMENT_FLAT_2D"
#define U_STATUS_W_TRACKER_DOWN "TRACKER_DOWN"
#define U_STATUS_W_PLUGIN_NOT_READY "PLUGIN_NOT_READY"
#define U_STATUS_W_SERVICE_HEADLESS "SERVICE_HEADLESS"
#define U_STATUS_W_DP_DEGRADED "DP_DEGRADED"
#define U_STATUS_W_DP_STALE "DP_STALE"
/*! @} */

//! A screen whose DP has not tracked for longer than this raises TRACKER_DOWN (§4).
#define U_STATUS_TRACKER_DOWN_MS 5000u

/*!
 * Recompute every runtime-derived warning of @p snap from its data: clears
 * `screens[*].warnings` and the system-level `warnings`, then applies the §4
 * rules. Idempotent. Never touches the vendor cell (vendor warnings pass
 * through verbatim there).
 */
void
u_status_warnings_derive(struct xrt_status_snapshot *snap);

/*!
 * Append a warning to a fixed array; false when full (the warning is dropped).
 */
bool
u_status_warning_push(struct xrt_status_warning *arr,
                      uint32_t *count,
                      uint32_t cap,
                      const char *code,
                      enum xrt_status_level level,
                      const char *text);

/*!
 * The design §3 JSON of @p snap (`schema` first). Caller owns the tree.
 */
cJSON *
u_status_snapshot_to_cjson(const struct xrt_status_snapshot *snap);

/*!
 * The design §7 text table of @p snap into @p buf (always NUL-terminated when
 * @p cap > 0; truncated, never overflowed). Returns the length the full text
 * needs, excluding the NUL — like snprintf, so a caller can size a buffer.
 */
size_t
u_status_snapshot_format_text(const struct xrt_status_snapshot *snap, char *buf, size_t cap);

/*!
 * The head piece of @p snap (everything but the screen and client rows, plus
 * the client ids) — what each `system_get_status_snapshot` reply carries.
 */
void
u_status_snapshot_get_head(const struct xrt_status_snapshot *snap, struct xrt_status_head *out);

/*!
 * Start reassembling a snapshot from its head: zeroes @p snap, copies the head
 * fields back and sets `screen_count` / `client_count` + each client row's
 * `id`. The caller then fills `screens[i]` and `clients[i]` from the per-row
 * replies (each must carry @ref xrt_status_head::generation, else refetch).
 */
void
u_status_snapshot_set_head(struct xrt_status_snapshot *snap, const struct xrt_status_head *head);

//! Both counters equal.
bool
u_status_generation_equal(const struct xrt_status_generation *a, const struct xrt_status_generation *b);

/*!
 * @name Stable enum spellings (as serialised)
 * @{
 */
const char *
u_status_source_str(enum xrt_status_source s);
const char *
u_status_tracking_str(enum xrt_status_tracking t);
const char *
u_status_presenter_str(enum xrt_status_presenter p);
const char *
u_status_lease_str(enum xrt_status_lease l);
const char *
u_status_level_str(enum xrt_status_level l);
const char *
u_status_vendor_tracker_str(enum xrt_status_vendor_tracker t);
const char *
u_status_vendor_lens_str(enum xrt_status_vendor_lens l);
const char *
u_status_mm_source_str(enum xrt_status_mm_source s);
const char *
u_status_layout_source_str(uint32_t xrt_screen_info_source);
const char *
u_status_dp_api_str(enum xrt_status_dp_api a);
const char *
u_status_dp_kind_str(enum xrt_status_dp_kind k);
const char *
u_status_dp_backend_str(enum xrt_status_dp_backend b);
const char *
u_status_eye_source_str(enum xrt_status_eye_source e);
//! `enum xrt_display_claim_confidence` value → "VERIFIED" / "EDID" / "FALLBACK" / "NONE" / "UNKNOWN".
const char *
u_status_confidence_str(uint32_t confidence);
//! `enum xrt_plugin_platform_state` value → "READY", … (same spellings as the loader).
const char *
u_status_platform_state_str(uint32_t state);
//! `enum xrt_client_class` value → "APP", … ("UNVERIFIED" when @p verified is false).
const char *
u_status_client_class_str(uint32_t client_class, bool verified);
/*! @} */

#ifdef __cplusplus
}
#endif
