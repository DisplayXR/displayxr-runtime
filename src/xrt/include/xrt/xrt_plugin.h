// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vendor plug-in negotiation interface.
 *
 * Defines the C ABI the DisplayXR runtime DLL uses to discover and
 * negotiate with vendor plug-in DLLs at `xrCreateInstance` time. The
 * runtime enumerates `HKLM\Software\DisplayXR\DisplayProcessors\*`
 * (Windows) — or, on POSIX, a JSON manifest directory under
 * `~/Library/Application Support/DisplayXR/DisplayProcessors/` (macOS)
 * or `${XDG_DATA_HOME:-~/.local/share}/DisplayXR/DisplayProcessors/`
 * (Linux) — loads each plug-in's DLL, resolves the single exported
 * entry point `xrtPluginNegotiate`, and asks it to identify itself.
 *
 * The plug-in returns an @ref xrt_plugin_iface vtable. The runtime
 * calls `probe()` to ask the plug-in whether it claims the current
 * system (e.g. "is a Leia SR display present?"). The first plug-in
 * whose probe succeeds wins; subsequent plug-ins are skipped.
 *
 * Logging, debug-variable tracking, metrics, and the limited-unique-id
 * generator are **NOT** plumbed through this iface — plug-ins reach
 * them by linking the runtime's `aux_imp.lib` import library. See
 * `docs/adr/ADR-019-vendor-plugin-aux-boundary.md`.
 *
 * The DP vtable returned by `create_dp_<api>` is unchanged from today —
 * the per-graphics-API factory typedefs in
 * `xrt_display_processor_<api>.h` are the canonical signatures and the
 * plug-in iface simply hands one back per supported API.
 *
 * @author David Fattal
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_display_processor.h"
#include "xrt/xrt_display_processor_d3d11.h"
#include "xrt/xrt_display_processor_d3d12.h"
#include "xrt/xrt_display_processor_gl.h"
#include "xrt/xrt_display_processor_metal.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Forward declaration of the host's device interface (full def in
 * `xrt/xrt_device.h`). The plug-in iface only handles `xrt_device *`
 * by pointer, so a forward decl is sufficient at this layer.
 */
struct xrt_device;


/*!
 * Vendor-neutral physical-display info populated by
 * `xrt_plugin_iface::get_display_info`. Lets the runtime drop direct
 * vendor calls (`leiasr_*`, `sim_display_get_display_info`) from its
 * own translation units, satisfying ADR-019/plan goal §2.1 ("runtime
 * DLL has zero vendor identifiers in its link line").
 *
 * Forward-compat: the runtime sets `struct_size` to its own
 * `sizeof(struct xrt_plugin_display_info)` before calling
 * `get_display_info`; plug-ins MUST NOT write past that offset.
 * Field additions append at the end with no API version bump.
 */
struct xrt_plugin_display_info
{
	/*! `sizeof(struct xrt_plugin_display_info)` at the runtime's
	 *  compile time. The plug-in clamps its writes to this offset. */
	uint32_t struct_size;
	uint32_t reserved_0;

	/*! Physical display dimensions in meters. */
	float display_width_m;
	float display_height_m;

	/*! Nominal viewer position relative to display center, in meters.
	 *  Drives Kooima projection defaults when the app has no
	 *  external head tracking. */
	float nominal_viewer_x_m;
	float nominal_viewer_y_m;
	float nominal_viewer_z_m;

	/*! Native panel resolution in pixels. */
	uint32_t display_pixel_width;
	uint32_t display_pixel_height;

	/*! Vendor-recommended per-view scaling. 1.0 means "render at the
	 *  native panel resolution per view"; <1.0 means downscale. The
	 *  compositor reads this into `xrt_system_compositor_info::recommended_view_scale_*`. */
	float recommended_view_scale_x;
	float recommended_view_scale_y;

	/*! Display top-left in virtual-screen coordinates (Windows-style).
	 *  Used to position workspace windows over the 3D panel. Both
	 *  fields 0 means "no preference" / "display origin is the
	 *  desktop origin" — the sim_display path picks this. */
	int32_t display_screen_left;
	int32_t display_screen_top;

	/*! Eye-tracking mode bits supported by this display, as
	 *  understood by `XR_EXT_display_info`. Bit 0 = MANAGED, bit 1 =
	 *  MANUAL. Leia is MANAGED-only (bit 0); sim_display is
	 *  MANUAL-only (bit 1). */
	uint32_t supported_eye_tracking_modes;

	/*! Default eye-tracking mode for sessions that don't override.
	 *  0 = MANAGED, 1 = MANUAL. */
	uint32_t default_eye_tracking_mode;
};


/*
 *
 * Per-display vendor claims (issue #69 / ADR-015).
 *
 */

/*!
 * Vendor-neutral descriptor for one connected monitor, handed to the
 * plug-in's `xrt_plugin_iface::probe_displays`. Built by the runtime from
 * the vendor-neutral EDID enumerator (`os_display_edid`). The plug-in
 * echoes back `monitor_id` for the monitors it claims.
 *
 * Forward-compat: the runtime sets `struct_size` to its own
 * `sizeof(struct xrt_display_descriptor)` before the call; plug-ins MUST
 * NOT read past that offset. Field additions append at the end with no
 * API version bump.
 */
struct xrt_display_descriptor
{
	/*! `sizeof(struct xrt_display_descriptor)` at the runtime's compile
	 *  time. The plug-in clamps its reads to this offset. */
	uint32_t struct_size;

	/*! Reserved for alignment. Must be 0. */
	uint32_t reserved_0;

	/*! Runtime-assigned monitor identifier, stable for this boot.
	 *  Derived from stable EDID identity (manufacturer/product/screen
	 *  position), not the transient HMONITOR. The plug-in echoes it back
	 *  in the matching @ref xrt_display_claim. */
	uint64_t monitor_id;

	/*! EDID bytes 8-9 (raw, as stored in EDID) — mirrors
	 *  `os_display_edid_monitor::manufacturer_id`. */
	uint16_t edid_manufacturer;

	/*! EDID bytes 10-11 (raw, as stored in EDID) — mirrors
	 *  `os_display_edid_monitor::product_id`. */
	uint16_t edid_product;

	/*! Monitor width/height in pixels (current mode). */
	uint32_t pixel_width;
	uint32_t pixel_height;

	/*! Current refresh rate in milli-Hz (e.g. 60000 = 60 Hz). Integer
	 *  so no float crosses the ABI. */
	uint32_t refresh_mhz;

	/*! Monitor top-left in virtual-screen coordinates. */
	int32_t screen_left;
	int32_t screen_top;

	/*! Bit 0 = primary monitor. Other bits reserved (must be 0). */
	uint32_t flags;
};

/*!
 * How sure a plug-in is that a monitor is its hardware. The runtime
 * resolves competing claims for the same monitor by highest confidence
 * (ties broken by registration ProbeOrder).
 */
enum xrt_display_claim_confidence
{
	XRT_DISPLAY_CLAIM_FALLBACK = 10,  //!< sim_display: "anything unclaimed".
	XRT_DISPLAY_CLAIM_EDID = 50,      //!< matched my EDID table.
	XRT_DISPLAY_CLAIM_VERIFIED = 100, //!< EDID + SDK/service/serial handshake.
};

/*!
 * Per-API bits for @ref xrt_display_claim::supported_apis. A set bit means
 * the plug-in's matching `create_dp_<api>` factory works for that monitor.
 * @{
 */
#define XRT_DP_API_BIT_VK (1u << 0)
#define XRT_DP_API_BIT_D3D11 (1u << 1)
#define XRT_DP_API_BIT_D3D12 (1u << 2)
#define XRT_DP_API_BIT_GL (1u << 3)
#define XRT_DP_API_BIT_METAL (1u << 4)
/*! @} */

/*!
 * One monitor a plug-in claims, returned from
 * `xrt_plugin_iface::probe_displays`. Fixed layout (no `struct_size`): the
 * plug-in fills a runtime-provided array and returns a count, so growth is
 * via @ref xrt_display_descriptor (input) rather than this output struct.
 */
struct xrt_display_claim
{
	/*! Echoes the @ref xrt_display_descriptor::monitor_id being claimed. */
	uint64_t monitor_id;

	/*! @ref xrt_display_claim_confidence. */
	uint32_t confidence;

	/*! Bitmask of @ref XRT_DP_API_BIT_ values — which `create_dp_<api>`
	 *  factories work for this monitor. */
	uint32_t supported_apis;

	/*! Vendor device serial (e.g. Leia FPC) tying this monitor to a
	 *  specific camera/calibration unit; empty string if not applicable. */
	char serial[64];
};


/*
 *
 * API versioning.
 *
 */

/*!
 * Plug-in ABI version (major). Bumped on a non-additive layout change in the
 * structs declared in this header **OR** in the display-processor vtables
 * (`xrt_display_processor` + the per-API vtables/factory contracts in
 * `xrt_display_processor_<api>.h`), which are part of this ABI even though they
 * live in their own headers and are handed back by `create_dp_<api>`.
 *
 * As of major **v2** (ADR-020 rules 1–3), the DP vtables ALSO carry a
 * `struct_size` header — exactly like the structs in this file. So a
 * pure-additive change to a DP vtable (a method appended at the END, covered by
 * `struct_size`) is NOT a version bump: a newer runtime treats slots past an
 * older plug-in's `struct_size` as absent, and an older runtime ignores slots
 * it doesn't know about. Reordering, removing, or signature-changing a slot —
 * or inserting anywhere but the end — IS a major bump. The compile-time
 * tripwires at the end of each `xrt_display_processor*.h` fail the build if a
 * vtable's layout changes without updating the asserts, forcing a conscious
 * version bump. See ADR-020 for the full policy.
 *
 * Compatibility rule (ADR-020 rule 2/3): same major == compatible; a different
 * major is **rejected by the loader** (`target_plugin_loader.c`) — it must not
 * call through a mismatched vtable. Numbers grow forward.
 *
 * v1 → v2 history: v1 DP vtables had no `struct_size` and were read at fixed
 * offsets, so any layout change silently broke older plug-ins (the
 * standalone-VK weave regression). v2 is the one-time break that introduces the
 * `struct_size` header on the DP vtables and turns the loader's version *log*
 * into an enforced *reject*. ABI-v1 plug-ins (≤ leia v1.0.5) are rejected and
 * must rebuild against v2 headers.
 */
#define XRT_PLUGIN_API_VERSION_1 1
#define XRT_PLUGIN_API_VERSION_2 2

/*!
 * The version the runtime / plug-in is built against at compile time.
 * Plug-in DLLs returning a different value from `xrtPluginNegotiate`'s
 * `*out_plugin_api_version` are rejected by the runtime (ADR-020 rule 3) with a
 * logged error, and the loader falls back to the next plug-in / sim_display.
 */
#define XRT_PLUGIN_API_VERSION_CURRENT XRT_PLUGIN_API_VERSION_2

/*!
 * The single exported symbol every plug-in DLL must provide. C linkage,
 * no name mangling. Spelled here as a literal so the runtime's
 * `GetProcAddress` / `dlsym` call doesn't drift from the plug-in side.
 */
#define XRT_PLUGIN_ENTRYPOINT_NAME "xrtPluginNegotiate"

/*!
 * Linkage decoration plug-ins should put on their `xrtPluginNegotiate`
 * definition. Resolves to `__declspec(dllexport)` on Windows and the
 * `default` visibility attribute everywhere else. The runtime does NOT
 * use this — it builds a static library out of the plug-in entry point
 * spelled as an ordinary function; the macro only matters in plug-in
 * builds.
 *
 * Usage in a plug-in TU:
 * @code
 *   XRT_PLUGIN_EXPORT xrt_result_t
 *   xrtPluginNegotiate(uint32_t runtime_api_version,
 *                      const struct xrt_plugin_host_iface *host,
 *                      struct xrt_plugin_iface **out_iface,
 *                      uint32_t *out_plugin_api_version)
 *   { ... }
 * @endcode
 */
#if defined(_WIN32) || defined(__CYGWIN__)
#define XRT_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define XRT_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define XRT_PLUGIN_EXPORT
#endif


/*
 *
 * Opaque types.
 *
 */

/*!
 * Opaque per-plug-in instance handle. The plug-in defines the concrete
 * layout; the runtime treats it as a `void *` keyed off `probe()`'s
 * out-param and passes it back to every subsequent vtable call.
 */
struct xrt_plugin_instance;


/*
 *
 * Iface definitions.
 *
 */

/*!
 * Host-supplied callbacks the plug-in may call. In v1 this is
 * intentionally minimal — the established channel for logging, debug-var
 * tracking, metrics, and unique-id generation is the runtime's aux
 * export surface (see ADR-019). The `reserved[]` array provides room to
 * add future host-supplied callbacks (e.g. plug-in-to-runtime eye-
 * position publish per PR #251) without bumping
 * @ref XRT_PLUGIN_API_VERSION_CURRENT.
 *
 * Forward-compat rules:
 *   - Plug-ins MUST NOT dereference any field whose offset is at or past
 *     `host->struct_size`.
 *   - The runtime MAY introduce new callbacks by repurposing reserved
 *     slots in later API versions; doing so bumps
 *     @ref XRT_PLUGIN_API_VERSION_CURRENT and grows `struct_size`.
 *
 * Lifetime: valid for the duration of the `xrtPluginNegotiate` call and
 * for the lifetime of the negotiated plug-in (i.e. until the runtime
 * calls `xrt_plugin_iface::destroy`).
 */
struct xrt_plugin_host_iface
{
	/*!
	 * `sizeof(struct xrt_plugin_host_iface)` at the runtime's compile
	 * time. Lets plug-ins built against an older header detect that the
	 * runtime is newer than they know about and refuse to read past
	 * this offset.
	 */
	uint32_t struct_size;

	/*!
	 * The API version the runtime advertises. Identical to the
	 * `runtime_api_version` parameter passed to `xrtPluginNegotiate`,
	 * duplicated here as a structural cross-check.
	 */
	uint32_t host_api_version;

	/*!
	 * Reserved space for forward-compatible host-supplied callbacks.
	 * Plug-ins MUST NOT dereference any reserved slot.
	 */
	void *reserved[14];
};

/*!
 * The plug-in's vtable. Filled in by the plug-in inside its
 * `xrtPluginNegotiate` implementation and handed back to the runtime via
 * the `out_iface` out-param. Storage is owned by the plug-in; the
 * runtime treats `*out_iface` as a read-only borrow.
 *
 * Forward-compat rules:
 *   - The runtime MUST NOT dereference any field whose offset is at or
 *     past the plug-in's reported `struct_size`.
 *   - New fields are only ever appended at the end. Reordering or
 *     redefining an existing field bumps
 *     @ref XRT_PLUGIN_API_VERSION_CURRENT.
 *
 * Lifetime: must remain valid until `destroy()` is called. After
 * `destroy()`, the runtime stops dereferencing the vtable and the
 * underlying `xrt_plugin_instance`.
 */
struct xrt_plugin_iface
{
	/*!
	 * `sizeof(struct xrt_plugin_iface)` at the plug-in's compile time.
	 * Lets the runtime detect plug-ins built against a newer header
	 * and skip reading past this offset.
	 */
	uint32_t struct_size;

	/*!
	 * Reserved for alignment. Must be 0.
	 */
	uint32_t reserved_0;

	/*!
	 * Short identifier. UTF-8. Matches the registry / manifest `<id>`
	 * subkey used at discovery (e.g. `"leia-sr"`, `"sim-display"`).
	 * Pointer storage owned by the plug-in; must remain valid for the
	 * plug-in instance's lifetime.
	 */
	const char *id;

	/*!
	 * Human-readable display name. UTF-8. Logged at probe.
	 */
	const char *display_name;

	/*!
	 * Optional publisher name. UTF-8. May be NULL.
	 */
	const char *vendor;

	/*!
	 * Optional version string. UTF-8. Logged at probe. May be NULL.
	 */
	const char *version;

	/*!
	 * Does this plug-in want to claim the current system?
	 *
	 * Cheap. May consult the vendor SDK to check for a connected display
	 * etc. Sub-millisecond budget — the runtime calls this on the
	 * `xrCreateInstance` hot path for every registered plug-in until one
	 * succeeds.
	 *
	 * On success: returns `XRT_SUCCESS` and sets `*out_inst` to a
	 * plug-in-defined handle. The runtime owns the lifetime of the
	 * returned instance and frees it via `destroy()`.
	 *
	 * On clean decline: returns `XRT_ERROR_PROBER_NOT_SUPPORTED` —
	 * meaning "no device of this type on this system." The runtime logs
	 * an info-level line and skips to the next registered plug-in.
	 *
	 * Other `XRT_ERROR_*` codes are treated as hard probe failures:
	 * logged at warning level, the plug-in is skipped.
	 */
	xrt_result_t (*probe)(struct xrt_plugin_instance **out_inst);

	/*!
	 * Construct the plug-in's @ref xrt_device — the head/HMD-equivalent
	 * device for the runtime's prober + system-builder. Called only
	 * after a successful `probe()`.
	 *
	 * Ownership of `*out_dev` is transferred to the runtime, which
	 * destroys the device via the usual `xrt_device::destroy` vtable
	 * method.
	 */
	xrt_result_t (*create_device)(struct xrt_plugin_instance *inst,
	                              struct xrt_device **out_dev);

	/*!
	 * Per-graphics-API display-processor factories. `NULL` means the
	 * plug-in does not support that graphics API on this platform.
	 *
	 * At least one of `{create_dp_vk, create_dp_d3d11, create_dp_d3d12,
	 * create_dp_gl, create_dp_metal}` must be non-NULL — the runtime
	 * rejects a plug-in whose probe succeeds but offers no DP factory
	 * (it would have nothing the compositor can drive).
	 *
	 * Each factory's signature is owned by its corresponding header
	 * (`xrt_display_processor_<api>.h`) and is unchanged by this work.
	 *
	 * @{
	 */
	xrt_dp_factory_vk_fn_t create_dp_vk;
	xrt_dp_factory_d3d11_fn_t create_dp_d3d11;
	xrt_dp_factory_d3d12_fn_t create_dp_d3d12;
	xrt_dp_factory_gl_fn_t create_dp_gl;
	xrt_dp_factory_metal_fn_t create_dp_metal;
	/*! @} */

	/*!
	 * Free `inst` and all plug-in-owned resources hanging off it.
	 * Called by the runtime at instance teardown, or after a negotiated
	 * plug-in is superseded by a later registration. After this returns,
	 * the runtime stops dereferencing `inst` and the vtable.
	 */
	void (*destroy)(struct xrt_plugin_instance *inst);

	/*!
	 * Fill in vendor-neutral physical-display info for `xdev` (the
	 * device the plug-in returned from `create_device`). Lets the
	 * runtime populate `xrt_system_compositor_info` without calling
	 * any vendor-specific symbol directly — the headline ADR-019
	 * goal.
	 *
	 * The runtime sets `out_info->struct_size` to its own
	 * `sizeof(struct xrt_plugin_display_info)` before the call; the
	 * plug-in MUST NOT write past that offset.
	 *
	 * Returns `true` if the struct was populated, `false` if the
	 * plug-in could not produce info for this device (e.g. the
	 * vendor SDK declined). On `false`, the runtime keeps the
	 * defaults already in `xsysc->info`.
	 *
	 * Optional. NULL means "no display info available" — the runtime
	 * treats it as if the call returned `false`. Required to be
	 * non-NULL for plug-ins that ship a `create_device`
	 * implementation in v2; required already today for plug-ins
	 * loaded by a runtime built without the legacy in-proc
	 * fallback path.
	 */
	bool (*get_display_info)(struct xrt_plugin_instance *inst,
	                         struct xrt_device *xdev,
	                         struct xrt_plugin_display_info *out_info);

	/*!
	 * Bind an external pose source (typically the qwerty HMD device
	 * driving WASD/mouse camera controls) to the device returned by
	 * `create_device`. Each vendor's driver owns a private cast from
	 * `xrt_device *` back to its container struct; the iface here
	 * lets the runtime invoke that vendor-private binding without
	 * the runtime DLL knowing the vendor's struct layout.
	 *
	 * This was the regression that broke cube-in-shell rendering at
	 * v1.3.4 + iface boundary: the sim-display builder used to call
	 * `sim_display_hmd_set_pose_source` directly on the just-created
	 * head; once `create_device` started returning Leia devices via
	 * the iface, that call corrupted the head's vtable backing
	 * struct. The iface method routes the bind through the plug-in
	 * that owns the device.
	 *
	 * Passing `source = NULL` clears the binding (the device falls
	 * back to its static pose).
	 *
	 * Optional. NULL means the plug-in doesn't support external pose
	 * binding — the caller skips silently.
	 */
	void (*set_pose_source)(struct xrt_plugin_instance *inst,
	                        struct xrt_device *xdev,
	                        struct xrt_device *source);

	/*!
	 * Report which of the supplied monitors this plug-in claims as its
	 * hardware (issue #69 / ADR-015). Turns the binary, system-level
	 * `probe()` into a per-monitor claim list so the runtime can route
	 * monitor→DP for mixed-vendor / force-sim-on-one-monitor setups.
	 *
	 * The runtime passes `display_count` vendor-neutral descriptors (built
	 * from `os_display_edid`) and a `max_claims`-sized output array. The
	 * plug-in writes one @ref xrt_display_claim per monitor it recognizes
	 * (using its own proprietary detection — EDID table match, USB
	 * handshake, etc.) and returns the number written (<= `max_claims`).
	 * Monitors the plug-in does not recognize are simply omitted.
	 *
	 * Each descriptor carries `struct_size`; the plug-in MUST NOT read past
	 * that offset. Cheap — called once at system init, not per-frame.
	 *
	 * Optional. NULL (or a plug-in whose `struct_size` predates this field)
	 * means "no per-display claims": the runtime falls back to treating a
	 * successful binary `probe()` as a single `XRT_DISPLAY_CLAIM_EDID` claim
	 * on the primary monitor (back-compat for single-display plug-ins).
	 */
	uint32_t (*probe_displays)(struct xrt_plugin_instance *inst,
	                           const struct xrt_display_descriptor *displays,
	                           uint32_t display_count,
	                           struct xrt_display_claim *out_claims,
	                           uint32_t max_claims);
};


/*
 *
 * Entry point.
 *
 */

/*!
 * Signature of the single C-ABI symbol each plug-in DLL must export as
 * @ref XRT_PLUGIN_ENTRYPOINT_NAME (`"xrtPluginNegotiate"`). Exported with
 * C linkage; no name mangling.
 *
 * @param      runtime_api_version       The @ref XRT_PLUGIN_API_VERSION_CURRENT
 *                                       (currently @ref XRT_PLUGIN_API_VERSION_2)
 *                                       the runtime speaks. Plug-ins compare
 *                                       this against the version they implement
 *                                       and may return
 *                                       `XRT_ERROR_PROBER_NOT_SUPPORTED` to
 *                                       bail cleanly if the runtime is too old
 *                                       or too new. The runtime also enforces
 *                                       the major match by rejecting any plug-in
 *                                       whose @p out_plugin_api_version differs
 *                                       (ADR-020 rule 3).
 * @param      host                      Pointer to the host's iface.
 *                                       `host->struct_size` tells the
 *                                       plug-in how much of the struct
 *                                       is defined for the host's
 *                                       version; plug-ins MUST NOT read
 *                                       past it.
 * @param[out] out_iface                 The plug-in's vtable. The
 *                                       plug-in MUST set
 *                                       `(*out_iface)->struct_size` to
 *                                       `sizeof(struct xrt_plugin_iface)`
 *                                       as known at its own compile time
 *                                       so the runtime can detect
 *                                       forward-version fields and clamp
 *                                       its reads.
 * @param[out] out_plugin_api_version    The @ref XRT_PLUGIN_API_VERSION_CURRENT
 *                                       (i.e. the major) the plug-in implements.
 *                                       Must equal the runtime's major or the
 *                                       loader rejects the plug-in.
 *
 * @return `XRT_SUCCESS` on negotiation success — the runtime proceeds to
 *         call `(*out_iface)->probe()`. `XRT_ERROR_PROBER_NOT_SUPPORTED`
 *         to decline cleanly (the runtime logs and skips). Any other
 *         `XRT_ERROR_*` is a hard failure: the runtime logs a warning
 *         and skips this plug-in.
 */
typedef xrt_result_t (*xrt_plugin_negotiate_fn_t)(uint32_t runtime_api_version,
                                                  const struct xrt_plugin_host_iface *host,
                                                  struct xrt_plugin_iface **out_iface,
                                                  uint32_t *out_plugin_api_version);


#ifdef __cplusplus
}
#endif
