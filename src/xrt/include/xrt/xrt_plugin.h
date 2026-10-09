// Copyright 2026, The DisplayXR Project
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
#include <stddef.h>
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
	 *  compositor reads this into `xrt_system_compositor_info::recommended_view_scale_*`.
	 *
	 *  **The rule:** `xrt_rendering_mode::view_scale_x/y` is the single source
	 *  of truth for view, tile and atlas sizing; this scalar is only a baseline
	 *  hint, so set it to 0 (the runtime then derives it from the mode table)
	 *  or to a value DERIVED FROM THE SAME NUMBERS as the active 3D mode's
	 *  scale — never to a second, independently computed figure.
	 *
	 *  Why it matters: the mode table sizes per-mode view dims, the worst-case
	 *  atlas and the compositor's tile grid, while this scalar sizes
	 *  `XrViewConfigurationView.recommended*`. If they disagree, an app is sized
	 *  per view from one number and tiled from the other, and a scalar larger
	 *  than the mode's scale declares an atlas too small to hold the real tiles.
	 *  The runtime also overwrites this scalar from the mode table on every
	 *  rendering-mode change, so a disagreeing value does not even survive. */
	float recommended_view_scale_x;
	float recommended_view_scale_y;

	/*! Display top-left in virtual-screen coordinates (Windows-style).
	 *  Used to position workspace windows over the 3D panel. Both
	 *  fields 0 means "no preference" / "display origin is the
	 *  desktop origin" — the sim_display path picks this. */
	int32_t display_screen_left;
	int32_t display_screen_top;

	/*! Eye-tracking mode bits supported by this display, as
	 *  understood by `XR_DXR_display_info`. Bit 0 = MANAGED, bit 1 =
	 *  MANUAL; 0 = no eye tracking at all. A typical hardware DP is
	 *  MANAGED-only (bit 0); sim_display declares 0 — its positions
	 *  are nominal, not tracked (`SIM_DISPLAY_FAKE_TRACKING=1`
	 *  re-enables MANUAL for hardware-free testing). Consistency rule
	 *  (#441): non-zero iff at least one rendering mode sets
	 *  XRT_RENDERING_MODE_FLAG_HAS_TRACKING in `mode_flags`. */
	uint32_t supported_eye_tracking_modes;

	/*! Default eye-tracking mode for sessions that don't override.
	 *  0 = MANAGED, 1 = MANUAL. */
	uint32_t default_eye_tracking_mode;

	/*! Panel refresh rate in milli-Hz (e.g. 60000 = 60 Hz). Integer so no
	 *  float crosses the ABI. 0 = unknown → the runtime keeps its own
	 *  default (the null compositor's frame pacer falls back to 20 FPS).
	 *  Drives `xrWaitFrame` pacing on the null-compositor (Android OOP)
	 *  path, where the DP plug-in owns present/weave and there is no
	 *  swapchain compositor to source vblank from. Append-only (struct_size
	 *  guards older plug-ins that don't write it). */
	uint32_t refresh_mhz;
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
 * NOT read past that offset.
 *
 * **Growth caveat (multi-screen M1):** `probe_displays` receives these as an
 * ARRAY, and a plug-in indexes it with its own compile-time `sizeof`, so
 * appending a field changes the array stride under every existing plug-in —
 * that is a layout break (an XRT_PLUGIN_API_VERSION_CURRENT bump), not the
 * free append `struct_size` suggests. New per-monitor facts go in a separate
 * struct passed by pointer instead (see @ref xrt_display_physical).
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
 * Physical facts about one monitor that @ref xrt_display_descriptor does not
 * carry (multi-screen M1), handed by pointer — never as an array — to
 * `xrt_plugin_iface::get_display_info_for_monitor`, so it can grow by
 * appending under `struct_size` with no ABI bump.
 *
 * The runtime sets `struct_size` to its own `sizeof`; a plug-in MUST NOT
 * read past it.
 */
struct xrt_display_physical
{
	/*! `sizeof(struct xrt_display_physical)` at the runtime's compile time. */
	uint32_t struct_size;

	/*! Reserved for alignment. Must be 0. */
	uint32_t reserved_0;

	/*! Physical size from EDID (detailed timing, else the basic block),
	 *  millimetres; 0 = unknown. */
	uint32_t physical_width_mm;
	uint32_t physical_height_mm;

	/*! The connector's device (native) mode; may differ from the
	 *  descriptor's pixel size under a scaled desktop. 0 = unknown. */
	uint32_t native_pixel_width;
	uint32_t native_pixel_height;
};

/*!
 * The screen a display processor is created for (multi-screen M2, ADR-047
 * D2): which monitor, where it sits on the desktop, and its physical facts.
 * Handed by pointer to `xrt_plugin_iface::create_dp_vk_for_screen` so the DP
 * can describe THAT screen (its own panel size, pixel size and desktop
 * origin) instead of the plug-in's one process-wide panel.
 *
 * The runtime sets `struct_size` to its own `sizeof`; a plug-in MUST NOT
 * read past it. Grows by appending (never passed as an array).
 */
struct xrt_screen_binding
{
	/*! `sizeof(struct xrt_screen_binding)` at the runtime's compile time. */
	uint32_t struct_size;

	/*! Reserved for alignment. Must be 0. */
	uint32_t reserved_0;

	/*! Registry monitor id (`xrt_display_descriptor::monitor_id`). */
	uint64_t monitor_id;

	/*! The monitor's desktop rect, in the space the OS places windows in
	 *  (X11: the root window). */
	int32_t desktop_left;
	int32_t desktop_top;
	uint32_t desktop_width;
	uint32_t desktop_height;

	/*! The connector's device (native) mode; 0 = unknown. Equal to the
	 *  desktop size exactly when window pixels reach the panel unresampled. */
	uint32_t native_pixel_width;
	uint32_t native_pixel_height;

	/*! EDID physical size, millimetres; 0 = unknown. */
	uint32_t physical_width_mm;
	uint32_t physical_height_mm;

	/*! Desktop compositor scale for this output; 0 = unknown. */
	float desktop_scale;

	/*! Reserved for alignment. Must be 0. */
	uint32_t reserved_1;

	/*! Vendor display identity for the plug-in's own SDK (e.g. the LeiaSR
	 *  `displayId` of SR-P1/P2); 0 = none. Opaque to the runtime. */
	uint64_t display_id;

	/*! Vendor serial from the winning claim ("" = n/a). */
	char serial[64];

	/*! OS output name (X11: RandR output, else the DRM connector); "" = unknown. */
	char device_name[64];
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

/*!
 * Generic state of the vendor platform a plug-in drives (ADR-045), reported
 * through `xrt_plugin_iface::get_platform_state`. Vendor-neutral by design:
 * the runtime acts only on these values and shows the plug-in's hint string
 * verbatim; it never learns what the platform is.
 *
 * Values are stable ABI (appended only).
 */
enum xrt_plugin_platform_state
{
	//! Not reported: slot absent (older plug-in), call returned false, or a
	//! value this runtime does not know. Treated as "carry on as before".
	XRT_PLUGIN_PLATFORM_STATE_UNKNOWN = 0,
	//! Platform installed, running, and its display is attached.
	XRT_PLUGIN_PLATFORM_STATE_READY = 1,
	//! The vendor platform runtime is not installed on this machine.
	XRT_PLUGIN_PLATFORM_STATE_PLATFORM_ABSENT = 2,
	//! Installed, but its service/daemon is not running (yet).
	XRT_PLUGIN_PLATFORM_STATE_PLATFORM_NOT_RUNNING = 3,
	//! Platform present and running, but none of its displays is attached.
	XRT_PLUGIN_PLATFORM_STATE_NO_DISPLAY = 4,
	//! Platform present but unusable by this plug-in (version too old/new,
	//! unsupported OS or GPU, ...). The hint says what.
	XRT_PLUGIN_PLATFORM_STATE_INCOMPATIBLE = 5,
};

/*!
 * Bits for @ref xrt_plugin_platform_status::flags.
 * @{
 */
/*!
 * This plug-in is a FALLBACK (e.g. the vendor-neutral simulation display):
 * it claims any system so the runtime always has a display processor. The
 * runtime adopts a better plug-in on re-probe only while the active one
 * carries this bit — the "no live swap" rule (ADR-045 D3). Vendor plug-ins
 * MUST NOT set it.
 */
#define XRT_PLUGIN_PLATFORM_FLAG_FALLBACK (1u << 0)
/*! @} */

/*! Size of @ref xrt_plugin_platform_status::hint, including the NUL. */
#define XRT_PLUGIN_PLATFORM_HINT_MAX 128

/*!
 * Out-param of `xrt_plugin_iface::get_platform_state`. The runtime sets
 * @ref struct_size and zero-fills the rest before the call; the plug-in
 * MUST NOT write past `struct_size`. Grows by consuming @ref reserved
 * (append-only; no ABI bump).
 */
struct xrt_plugin_platform_status
{
	/*! `sizeof(struct xrt_plugin_platform_status)` as the RUNTIME knows it. */
	uint32_t struct_size;

	/*! @ref xrt_plugin_platform_state value. */
	uint32_t state;

	/*! Bitmask of `XRT_PLUGIN_PLATFORM_FLAG_*`. */
	uint32_t flags;

	/*! Reserved for alignment. Must be 0. */
	uint32_t reserved_0;

	/*!
	 * Short, user-facing, vendor-written hint, UTF-8, NUL-terminated,
	 * truncated to fit — e.g. what to install or plug in. Empty when there
	 * is nothing to say (typically READY). Shown verbatim by the runtime's
	 * diagnostics (cli, tray); never parsed.
	 */
	char hint[XRT_PLUGIN_PLATFORM_HINT_MAX];

	/*! Reserved for future fields. Plug-ins MUST leave these 0. */
	uint64_t reserved[8];
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
 *
 * v2 → v3 history (#441): `xrt_rendering_mode` gained `mode_flags`
 * (bit 0 = XRT_RENDERING_MODE_FLAG_HAS_TRACKING) + `reserved[3]` in its
 * vendor-provided section. The array is embedded by value in `xrt_device`
 * (created by the plug-in), so the element-stride change is a layout break.
 * The flags word + reserved padding exist so future per-mode capabilities are
 * new bits, not new fields — v3 is intended to be the last rendering-mode
 * layout break.
 *
 * v3 → v4 history (#573): the chroma-key transparency mechanism was deleted
 * everywhere. The `set_chroma_key` slot is removed from all five DP vtables (base
 * + d3d11/d3d12/gl/metal), shifting every slot after it — a layout break. The
 * D3D12 and GL DP vtables gain `set_transparent_background` (the chroma-key-free
 * transparency enable the other variants already had), and the extension struct
 * drops `chromaKeyColor` (XR_DXR_win32_window_binding SPEC_VERSION 7→8). True
 * transparency (alpha-capable swapchain + transparent present) is the sole path.
 *
 * v4 → v5 history (#757): `struct vk_bundle` — whose raw pointer crosses the
 * runtime → plug-in boundary via the VK DP factory — gained ABI-parity #else
 * placeholder members for every `VK_USE_PLATFORM_*`-conditional PFN slot, so
 * its layout no longer varies with feature *detection* at configure time
 * (pkg-config finding wayland-client etc.). For any config that previously
 * compiled without one of those platform macros this inserts members — a
 * layout break versus v4 binaries — but it is the LAST such break: the layout
 * is now identical across configs by construction. The Linux VK DP contract
 * also newly guarantees the dma-buf import extension set on the app device
 * (VK_EXT_external_memory_dma_buf + VK_EXT_image_drm_format_modifier,
 * desktop-background capture).
 *
 * v5 addendum (#1243, additive — NOT a bump): the v5 "identical across
 * configs" guarantee for `vk_bundle` has holes — `os_mutex` (embedded via
 * `vk_bundle_queue queues[2]`) carries `#ifndef NDEBUG` fields, and several
 * `#if defined(VK_*)` members lack the parity `#else` (they track the Vulkan
 * headers version, not our config). Until a major closes those holes, the
 * iface's appended `vk_bundle_abi_size` fingerprint lets the loader refuse a
 * mismatched pairing instead of dispatching through a skewed table.
 */
#define XRT_PLUGIN_API_VERSION_1 1
#define XRT_PLUGIN_API_VERSION_2 2
#define XRT_PLUGIN_API_VERSION_3 3
#define XRT_PLUGIN_API_VERSION_4 4
#define XRT_PLUGIN_API_VERSION_5 5

/*!
 * The version the runtime / plug-in is built against at compile time.
 * Plug-in DLLs returning a different value from `xrtPluginNegotiate`'s
 * `*out_plugin_api_version` are rejected by the runtime (ADR-020 rule 3) with a
 * logged error, and the loader falls back to the next plug-in / sim_display.
 */
#define XRT_PLUGIN_API_VERSION_CURRENT XRT_PLUGIN_API_VERSION_5

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
	 * Return the host's Android `JavaVM *` (cast to `void *`), or NULL if
	 * the host has none (non-Android runtime, or the app never supplied it
	 * via `XrInstanceCreateInfoAndroidKHR` / `XrLoaderInitInfoAndroidKHR`).
	 *
	 * Android plug-ins need the host's `JavaVM` to initialize vendor SDKs
	 * (e.g. CNSDK's `PlatformInitArgs.javaVM`). A plug-in that statically
	 * links the runtime's `aux_android` ends up with its own *private*,
	 * never-populated copy of the VM globals (hidden-visibility binds
	 * locally), so it MUST obtain the VM through this host callback rather
	 * than calling `android_globals_get_vm()` itself.
	 *
	 * Carved out of the former `reserved[]` block: `struct_size` is
	 * unchanged and the slot was previously zero-initialized, so older
	 * plug-ins (which never read it) are unaffected and no
	 * @ref XRT_PLUGIN_API_VERSION_CURRENT bump is needed. Plug-ins MUST
	 * NULL-check before calling.
	 */
	void *(*get_android_vm)(void);

	/*!
	 * Return the host's Android Activity `jobject` (as `void *`), or NULL.
	 * Companion to @ref get_android_vm; same back-compat contract. Plug-ins
	 * MUST NULL-check before calling.
	 */
	void *(*get_android_activity)(void);

	/*!
	 * Return an Android `Context` (as a jobject `void *`) whose
	 * `getClassLoader()` can resolve classes shipped in the **runtime's**
	 * APK, or NULL when the host cannot provide one.
	 *
	 * ADR-036 D2/D5, #1037: with the compositor and the vendor plug-in
	 * running IN THE APP's process, the vendor SDK's Java glue must still
	 * come from the runtime — an app must never bundle a vendor AAR
	 * (ADR-025). A vendor SDK that builds its own `DexClassLoader` takes
	 * the parent loader from the `Context` it is handed, so a Context made
	 * with `createPackageContext(<runtime pkg>,
	 * CONTEXT_INCLUDE_CODE | CONTEXT_IGNORE_SECURITY)` is all it takes for
	 * the vendor's classes to resolve out of the runtime APK. This mirrors
	 * how the runtime already hosts `org.freedesktop.monado.ipc.Client`
	 * (`loadClassFromRuntimeApk`, `ipc/android/ipc_client_android.cpp`).
	 *
	 * **Use it ONLY for class loading.** It is a class-hosting Context: it
	 * still runs under the app's uid, so `getPackageManager()` visibility
	 * and `bindService()` identity remain the app's, and it is not an
	 * Activity. Activity-typed vendor calls (orientation limiting,
	 * permission dialogs) MUST keep using @ref get_android_activity.
	 *
	 * Out-of-process (the runtime service) the "runtime package" is the
	 * calling process itself, so the host returns its own Context and the
	 * slot is a no-op that costs the plug-in nothing.
	 *
	 * The returned reference is a JNI global ref owned by the host and
	 * cached for the process lifetime; the plug-in MUST NOT delete it.
	 *
	 * Carved out of the former `reserved[]` block, exactly as
	 * @ref get_android_vm was: `struct_size` is unchanged and the slot was
	 * previously zero-initialized, so older plug-ins are unaffected and no
	 * @ref XRT_PLUGIN_API_VERSION_CURRENT bump is needed. Plug-ins MUST
	 * NULL-check before calling.
	 */
	void *(*get_android_class_host_context)(void);

	/*!
	 * Answer whether an Android package is VISIBLE to the calling process,
	 * i.e. whether `PackageManager.getPackageInfo(package_name, 0)` resolves
	 * rather than throwing `NameNotFoundException`. Returns false when it is
	 * not visible, when the host cannot answer, or on any JNI trouble.
	 *
	 * **Why this exists (runtime#1079).** Android enforces package
	 * visibility per CALLING UID. Out-of-process the vendor display
	 * processor ran in the runtime service's uid, whose manifest declares
	 * the vendor service packages in its `<queries>`. In-process
	 * (Architecture A, ADR-036 D2) the very same code runs in the APP's uid,
	 * so visibility is now the APP's manifest to declare — and the runtime
	 * cannot grant it on the app's behalf. `createPackageContext(...,
	 * CONTEXT_IGNORE_SECURITY)` (@ref get_android_class_host_context) fixes
	 * class LOADING only; it transfers no visibility.
	 *
	 * The failure this prevents is brutally undiagnosable without it: a
	 * vendor core loader queries its service package, ignores the thrown
	 * `NameNotFoundException`, and hands the resulting NULL jobject to the
	 * next JNI call — which trips CheckJNI and ABORTS the app from inside
	 * closed vendor code, with a stack that names neither the package nor
	 * the manifest. Once control is inside that loader nothing can catch it,
	 * so the only defence is to ASK FIRST: a plug-in should probe every
	 * package its SDK will bind, and on a miss fail cleanly (the loader then
	 * degrades to no-DP) while logging the exact `<package>` lines the app
	 * is missing.
	 *
	 * The probe runs against the APP's Context — which is the whole point,
	 * since that is the uid whose visibility decides the outcome.
	 *
	 * @param package_name NUL-terminated Android package name, e.g.
	 *                     `"com.example.display.config"`. Must not be NULL.
	 *
	 * Carved out of the former `reserved[]` block, exactly as
	 * @ref get_android_class_host_context was: `struct_size` is unchanged
	 * and the slot was previously zero-initialized, so older plug-ins are
	 * unaffected and no @ref XRT_PLUGIN_API_VERSION_CURRENT bump is needed.
	 * Plug-ins MUST NULL-check before calling.
	 */
	bool (*android_package_is_visible)(const char *package_name);

	/*!
	 * Reserved space for forward-compatible host-supplied callbacks.
	 * Plug-ins MUST NOT dereference any reserved slot.
	 */
	void *reserved[10];
};

/*!
 * Defined when this header carries the
 * @ref xrt_plugin_host_iface::get_android_class_host_context slot, so a plug-in
 * that must also compile against an older runtime header (which lacks the
 * field) can `#ifdef`-guard its use — the same coupled-ABI-addition pattern the
 * display-processor slots use (see `XRT_DP_VK_HAS_WINDOW_SCREEN_RECT`).
 * #1037 / ADR-036 D2.
 */
#define XRT_PLUGIN_HOST_HAS_CLASS_HOST_CONTEXT 1

/*!
 * Defined when this header carries the
 * @ref xrt_plugin_host_iface::android_package_is_visible slot, so a plug-in that
 * must also compile against an older runtime header (which lacks the field) can
 * `#ifdef`-guard its use. runtime#1079 / ADR-036 D2.
 */
#define XRT_PLUGIN_HOST_HAS_ANDROID_PACKAGE_VISIBILITY 1

/*
 *
 * Stereo camera source (ADR-043, XR_DXR_stereo_camera).
 *
 * A display's stereo camera — usually the one its eye tracker looks through —
 * produced by the plug-in from its own stack WITHOUT taking the device from
 * the tracker. The runtime service owns threads, fan-out, rectification,
 * format conversion, transport, consent and the in-use indicator; the plug-in
 * owns reading + decoding frames and the calibration of ITS ACTIVE device.
 * Graphics-API-neutral on purpose: a camera is a sensor, not a weaver, and it
 * must not share a lifetime with a display processor that is recreated on
 * presenter changes. Slots on @ref xrt_plugin_iface (appended, ADR-020).
 *
 */

//! @ref xrt_plugin_stereo_camera_info::flags — values equal the
//! XrStereoCameraFlagsDXR bits (static-asserted in the state tracker).
#define XRT_PLUGIN_STEREO_CAMERA_SHARED_WITH_EYE_TRACKING (1u << 0)
#define XRT_PLUGIN_STEREO_CAMERA_USER_FACING (1u << 1)
#define XRT_PLUGIN_STEREO_CAMERA_CALIBRATED (1u << 2)
#define XRT_PLUGIN_STEREO_CAMERA_NATIVELY_RECTIFIED (1u << 3)
#define XRT_PLUGIN_STEREO_CAMERA_MONOCHROME (1u << 4)

//! Pixel formats a plug-in delivers (values equal XrStereoCameraFormatDXR).
enum xrt_plugin_stereo_camera_format
{
	XRT_PLUGIN_STEREO_CAMERA_FORMAT_GRAY8 = 1,
	XRT_PLUGIN_STEREO_CAMERA_FORMAT_NV12 = 2,
	XRT_PLUGIN_STEREO_CAMERA_FORMAT_BGRA8 = 3,
};

//! Lens models (values equal XrStereoCameraDistortionModelDXR).
enum xrt_plugin_stereo_camera_distortion
{
	XRT_PLUGIN_STEREO_CAMERA_DISTORTION_NONE = 0,
	XRT_PLUGIN_STEREO_CAMERA_DISTORTION_RADTAN5 = 1,
	XRT_PLUGIN_STEREO_CAMERA_DISTORTION_RADTAN8 = 2,
	XRT_PLUGIN_STEREO_CAMERA_DISTORTION_KB4 = 3,
};

//! Result of @ref xrt_plugin_iface::stereo_camera_wait_frame.
enum xrt_plugin_stereo_camera_wait
{
	//! @p out holds a new frame; call stereo_camera_release_frame when done.
	XRT_PLUGIN_STEREO_CAMERA_WAIT_OK = 0,
	//! No frame within the timeout (source up; e.g. tracker warming up).
	XRT_PLUGIN_STEREO_CAMERA_WAIT_TIMEOUT = 1,
	//! The source is temporarily not delivering (tracker stopped).
	XRT_PLUGIN_STEREO_CAMERA_WAIT_SUSPENDED = 2,
	//! The source failed or went away; the runtime closes it.
	XRT_PLUGIN_STEREO_CAMERA_WAIT_ERROR = 3,
};

/*!
 * One camera, as a plug-in describes it. The runtime sets @ref struct_size to
 * its own size before the call; the plug-in must not write past it.
 */
struct xrt_plugin_stereo_camera_info
{
	uint32_t struct_size;
	//! Human-readable, UTF-8 ("Built-in 3D camera").
	char display_name[128];
	//! Stable device key (e.g. the panel serial). The SERVICE hashes it per
	//! consumer; it is never exported to clients.
	char device_identity[128];
	//! The OS's id of the physical device the frames come from, or "".
	char platform_device_hint[256];
	//! XRT_PLUGIN_STEREO_CAMERA_* bits.
	uint32_t flags;
	//! Native per-eye size.
	uint32_t eye_width;
	uint32_t eye_height;
	/*!
	 * What the source delivers, Hz — ONLY if the plug-in actually knows it
	 * (a vendor-provided value, or one it measured on an earlier open). 0 =
	 * unknown; never a placeholder guess. The service measures the real rate
	 * over the first frames after each open and reports that to apps.
	 */
	float max_frame_rate;
	//! @ref xrt_plugin_stereo_camera_format the plug-in decodes to.
	uint32_t native_format;
};

/*!
 * RAW calibration of the ACTIVE device (the one this plug-in weaves for and
 * whose tracker it reads) — never "the first calibration on the machine".
 */
struct xrt_plugin_stereo_camera_calibration
{
	uint32_t struct_size;
	//! Per eye, pixels.
	uint32_t image_width;
	uint32_t image_height;
	//! Per eye: fx fy cx cy, pixels.
	double k[2][4];
	//! @ref xrt_plugin_stereo_camera_distortion.
	uint32_t distortion_model;
	double distortion[2][8];
	double rotation_right_from_left[3][3];
	double translation_right_from_left_mm[3];
};

/*!
 * One frame handed to the runtime. Planes stay valid until
 * stereo_camera_release_frame. Always ONE side-by-side image, left eye left.
 */
struct xrt_plugin_stereo_camera_frame
{
	//! Monotonic per open, from 1.
	uint64_t sequence;
	//! os_monotonic_get_ns() domain.
	int64_t time_ns;
	//! true = sensor/exposure time; false = arrival time in the plug-in.
	bool time_is_exposure;
	//! Full SBS extent (2 * eye_width x eye_height).
	uint32_t width;
	uint32_t height;
	//! @ref xrt_plugin_stereo_camera_format.
	uint32_t format;
	const uint8_t *planes[2];
	uint32_t pitches[2];
};

//! Opaque plug-in-owned open camera.
struct xrt_plugin_stereo_camera;


/*!
 * @name Per-screen vendor status (ADR-051 D2)
 *
 * The vendor summary cell the display dashboard shows for each screen a
 * plug-in claimed, filled by @ref xrt_plugin_iface::get_screen_status. The
 * runtime shows these values and strings; it never learns what they mean for
 * a given vendor. Enum values are stable ABI (appended only).
 * @{
 */

//! @ref xrt_plugin_screen_status::tracker — the screen's eye tracker.
enum xrt_plugin_tracker_state
{
	//! The screen has no tracker (or the plug-in does not know of one).
	XRT_PLUGIN_TRACKER_STATE_NONE = 0,
	//! A tracker exists and is stopped (nobody asked for it).
	XRT_PLUGIN_TRACKER_STATE_OFF = 1,
	//! The tracker is starting (camera warming up).
	XRT_PLUGIN_TRACKER_STATE_STARTING = 2,
	//! The tracker is running and delivering.
	XRT_PLUGIN_TRACKER_STATE_RUNNING = 3,
	//! The tracker should be running and is not (lost, failed, unplugged).
	XRT_PLUGIN_TRACKER_STATE_DOWN = 4,
	//! The vendor cannot report a tracker state for this screen.
	XRT_PLUGIN_TRACKER_STATE_UNSUPPORTED = 5,
};

//! @ref xrt_plugin_screen_status::lens — the screen's optical state.
enum xrt_plugin_lens_state
{
	XRT_PLUGIN_LENS_STATE_2D = 0,
	XRT_PLUGIN_LENS_STATE_3D = 1,
	XRT_PLUGIN_LENS_STATE_UNKNOWN = 2,
};

//! @ref xrt_plugin_screen_warning::level values.
enum xrt_plugin_screen_warning_level
{
	XRT_PLUGIN_SCREEN_WARNING_LEVEL_INFO = 0,
	XRT_PLUGIN_SCREEN_WARNING_LEVEL_WARN = 1,
	XRT_PLUGIN_SCREEN_WARNING_LEVEL_CRITICAL = 2,
};

//! Current @ref xrt_plugin_screen_status::version.
#define XRT_PLUGIN_SCREEN_STATUS_VERSION 1
//! Capacity of @ref xrt_plugin_screen_status::warnings.
#define XRT_PLUGIN_SCREEN_STATUS_MAX_WARNINGS 8

//! One vendor warning about a screen, shown verbatim (never parsed).
struct xrt_plugin_screen_warning
{
	//! Stable machine code, e.g. "TRACKER_DOWN". UTF-8, NUL-terminated.
	char code[32];
	//! @ref xrt_plugin_screen_warning_level value.
	uint8_t level;
	//! One user-facing sentence. UTF-8, NUL-terminated, truncated to fit.
	char text[96];
};

/*!
 * Out-param of @ref xrt_plugin_iface::get_screen_status. The caller sets
 * @ref struct_size and zero-fills the rest before the call; the plug-in MUST
 * NOT write past `struct_size`. Grows only by appending (and bumping
 * @ref version); no ABI bump.
 */
struct xrt_plugin_screen_status
{
	//! `sizeof(struct xrt_plugin_screen_status)` as the CALLER knows it.
	uint32_t struct_size;
	//! @ref XRT_PLUGIN_SCREEN_STATUS_VERSION the plug-in filled.
	uint32_t version;
	//! Moves whenever anything below may have changed; a poller skips a screen
	//! whose counter did not move. Bumped on the vendor's own device /
	//! topology events, never by polling the hardware.
	uint64_t change_counter;
	//! The vendor platform is up for this screen.
	bool ready;
	//! The vendor positively identified this screen as its hardware.
	bool verified;
	//! A valid calibration exists for this screen (vendor-defined meaning).
	bool calibrated;
	enum xrt_plugin_tracker_state tracker;
	enum xrt_plugin_lens_state lens;
	//! Vendor model name, UTF-8, "" = unknown.
	char model[32];
	//! Vendor serial, UTF-8, "" = unknown.
	char serial[32];
	//! Valid entries in @ref warnings (<= XRT_PLUGIN_SCREEN_STATUS_MAX_WARNINGS).
	uint32_t warning_count;
	struct xrt_plugin_screen_warning warnings[XRT_PLUGIN_SCREEN_STATUS_MAX_WARNINGS];
	/*!
	 * Command line that opens the vendor's own dashboard on this screen, with
	 * `{serial}` and `{monitor_id}` placeholders the runtime substitutes, e.g.
	 * `"<exe> --page displays --display {serial}"`. "" = none. The runtime
	 * launches it on request and never parses it.
	 */
	char dashboard_command[160];
};

/*! @} */


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

	/*!
	 * `sizeof(struct vk_bundle)` as compiled into THIS plug-in — the ABI
	 * fingerprint of the raw `vk_bundle*` that @ref create_dp_vk receives
	 * (#1243). The v5 parity work (#757) made `vk_bundle` invariant across
	 * *feature-detection* configs, but the layout still varies with the
	 * build config (`os_mutex`'s `#ifndef NDEBUG` debug fields — 16 bytes,
	 * exactly two function-pointer slots) and with Vulkan-header-version
	 * gates. A Debug-config plug-in against a Release-config runtime reads
	 * `vkCreateFramebuffer` where it expects `vkCreateRenderPass` and
	 * crashes inside the driver — the cause of every broken v2.14.x
	 * Android release. The loader compares this against its own
	 * `sizeof(struct vk_bundle)` and refuses the VK DP factory on
	 * mismatch: an unwoven session plus an actionable error instead of
	 * memory corruption.
	 *
	 * Set to `(uint32_t)sizeof(struct vk_bundle)`. 0 (or a plug-in whose
	 * `struct_size` predates this field) means "unknown": the loader
	 * refuses the VK factory on Android (every pre-guard pairing there is
	 * the crash class above) and proceeds with a loud warning on desktop
	 * (working Linux pairings must not regress). Appended per ADR-020
	 * (append-only within a major; gated by @ref struct_size).
	 */
	uint32_t vk_bundle_abi_size;

	/*!
	 * `offsetof(struct vk_bundle, vkGetInstanceProcAddr)` — where the
	 * function-pointer table starts, as compiled into THIS plug-in. The
	 * second half of the #1243 fingerprint: `sizeof` alone cannot see two
	 * header-gated members changing in compensating directions (total size
	 * unchanged, table moved). Any shift of the table start — the invariant
	 * that actually breaks — is caught by this field regardless. Same
	 * gating and absent-field semantics as @ref vk_bundle_abi_size.
	 * Set to `(uint32_t)offsetof(struct vk_bundle, vkGetInstanceProcAddr)`.
	 */
	uint32_t vk_bundle_fn_table_offset;

	/*!
	 * Create a D3D11 display processor that serves ONLY the 2D→3D lift slots
	 * (ADR-042, XR_DXR_lift) — the explicit "lift-only" factory.
	 *
	 * The runtime's D3D11 service creates exactly one lift DP per process, on
	 * a dedicated device on the service adapter (@p d3d11_context has
	 * ID3D11Multithread protection on), from its lift thread, and never asks
	 * it to weave: no process_atlas, no window (@p window_handle is always
	 * NULL), no mode requests. So the DP returned here must build NO weaver
	 * and open NO tracker session — only what its conversion module needs —
	 * and must fill the lift_* slots of @ref xrt_display_processor_d3d11
	 * (XRT_DP_D3D11_HAS_LIFT). Same signature as @ref create_dp_d3d11.
	 *
	 * Why a separate factory rather than reusing the weaving DP: a conversion
	 * blocks for tens of ms (seconds for GAUSSIANS), and the weaving DP is
	 * driven on the service's one shared immediate context under the render
	 * lock and is recreated on presenter / focus changes. Running lift on it
	 * would stall every weave for the length of a conversion.
	 *
	 * Optional. NULL (or a plug-in whose `struct_size` predates this field) ⟹
	 * the runtime falls back to @ref create_dp_d3d11 with a NULL window and, if
	 * that DP carries no lift slots, destroys it at once. Appended per ADR-020
	 * (append-only within a major; gated by @ref struct_size; no
	 * XRT_PLUGIN_API_VERSION_CURRENT bump).
	 */
	xrt_dp_factory_d3d11_fn_t create_dp_d3d11_lift;

	/*!
	 * Report the plug-in's generic platform state (ADR-045): whether the
	 * vendor platform it drives is installed, running, and has its display
	 * attached — plus a short vendor-written hint for the user.
	 *
	 * **Callable before `probe()` succeeds, and without an instance.** The
	 * loader calls it right after a successful negotiation and before
	 * `probe()`, so a plug-in that is about to decline can still say why;
	 * it calls it again on every re-probe, and the runtime's diagnostics
	 * (`displayxr-cli`, the service tray) call it at any time afterwards,
	 * including while the plug-in is the active one. The answer is
	 * therefore about the plug-in's process-wide view of its platform, not
	 * about one instance.
	 *
	 * Contract:
	 *   - Cheap and non-blocking: presence checks only (a registry value, a
	 *     named object, an EDID table lookup). MUST return within the same
	 *     ~100 ms budget as `probe()` and MUST NOT wait for the vendor
	 *     platform to become ready. Thread-safe: may be called from any
	 *     thread, concurrently with the plug-in's other entry points.
	 *   - The runtime sets `out_status->struct_size` to its own
	 *     `sizeof(struct xrt_plugin_platform_status)` and zero-fills the
	 *     rest before the call; the plug-in MUST NOT write past that offset.
	 *   - Returns `true` if `out_status` was filled. `false` (or a NULL
	 *     slot, or a plug-in whose `struct_size` predates this field) means
	 *     "not reported": the runtime records
	 *     @ref XRT_PLUGIN_PLATFORM_STATE_UNKNOWN and carries on exactly as
	 *     before — the state is advisory and never gates loading.
	 *
	 * The runtime never interprets the hint; it only displays it. It does
	 * interpret the state and @ref XRT_PLUGIN_PLATFORM_FLAG_FALLBACK, and
	 * only generically (selection never re-routes away from an active
	 * non-fallback plug-in — "no live swap", ADR-045).
	 *
	 * Optional. Appended per ADR-020 (append-only within a major; gated by
	 * @ref struct_size; no XRT_PLUGIN_API_VERSION_CURRENT bump).
	 */
	bool (*get_platform_state)(struct xrt_plugin_platform_status *out_status);

	/*!
	 * @name Stereo camera source (ADR-043, XR_DXR_stereo_camera)
	 *
	 * Optional, all-or-nothing: the runtime uses the camera slots only when
	 * @ref struct_size covers @ref stereo_camera_close and every one of them
	 * is non-NULL. Appended per ADR-020 (append-only within a major; gated by
	 * @ref struct_size; no XRT_PLUGIN_API_VERSION_CURRENT bump) AFTER
	 * @ref get_platform_state (ADR-045), the last member before this block.
	 * Announced by @ref XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA.
	 *
	 * Threading: enumerate / get_calibration are cheap and callable from any
	 * thread. wait_frame / release_frame are called from ONE runtime-owned
	 * camera thread per open camera. Nothing throws across this boundary.
	 * @{
	 */

	/*!
	 * Fill up to @p capacity entries of @p out (each with struct_size preset
	 * by the runtime) and return the number of cameras. @p capacity 0 / NULL
	 * @p out = count query. Stable indices for the plug-in instance's life.
	 */
	uint32_t (*stereo_camera_enumerate)(struct xrt_plugin_instance *inst,
	                                    uint32_t capacity,
	                                    struct xrt_plugin_stereo_camera_info *out);

	/*!
	 * RAW calibration of camera @p index, for the ACTIVE device. Return
	 * XRT_ERROR_FEATURE_NOT_SUPPORTED when it cannot be resolved — the camera
	 * must then not report XRT_PLUGIN_STEREO_CAMERA_CALIBRATED.
	 */
	xrt_result_t (*stereo_camera_get_calibration)(struct xrt_plugin_instance *inst,
	                                              uint32_t index,
	                                              struct xrt_plugin_stereo_camera_calibration *out);

	/*!
	 * Open camera @p index. Doubles as a tracker keep-alive: while any camera
	 * is open, a SHARED_WITH_EYE_TRACKING source keeps its tracker running.
	 * Must never open / reconfigure / re-time the device away from the tracker.
	 */
	xrt_result_t (*stereo_camera_open)(struct xrt_plugin_instance *inst,
	                                   uint32_t index,
	                                   struct xrt_plugin_stereo_camera **out_cam);

	/*!
	 * Block up to @p timeout_ns for the next frame. Returns an @ref
	 * xrt_plugin_stereo_camera_wait. On OK the frame's planes stay valid until
	 * @ref stereo_camera_release_frame. Must not wait on a wake object shared
	 * with other readers of the vendor channel (poll it instead; ADR-043).
	 */
	uint32_t (*stereo_camera_wait_frame)(struct xrt_plugin_stereo_camera *cam,
	                                     int64_t timeout_ns,
	                                     struct xrt_plugin_stereo_camera_frame *out);

	//! Release the frame of the last OK wait_frame.
	void (*stereo_camera_release_frame)(struct xrt_plugin_stereo_camera *cam);

	//! Close; drops the keep-alive when it was the last open camera.
	void (*stereo_camera_close)(struct xrt_plugin_stereo_camera *cam);

	/*! @} */

	/*!
	 * Report the display info of ONE monitor this plug-in won in the
	 * per-monitor registry (multi-screen M1, `XR_DXR_display_info` v22
	 * `xrEnumerateDisplaysDXR`). Same out struct and the same field meanings
	 * as @ref get_display_info, but keyed by the monitor instead of the head
	 * device: physical size, nominal viewer (display-centred, metres), native
	 * pixels, recommended view scale (0 = let the runtime derive), desktop
	 * origin, eye-tracking modes supported/default for THAT monitor.
	 *
	 * Contract:
	 *   - @p display is the descriptor the runtime built for this monitor
	 *     (the one `probe_displays` saw). @p physical carries what the
	 *     descriptor does not (EDID millimetres, the connector's device
	 *     mode); it may be NULL, and its fields are 0 when unknown.
	 *   - The runtime sets `out_info->struct_size` and zero-fills the rest;
	 *     the plug-in MUST NOT write past `struct_size`.
	 *   - Cheap and non-blocking (it runs on `xrEnumerateDisplaysDXR` and in
	 *     diagnostics), callable from any thread, with @p inst NULL for a
	 *     plug-in that has no instance state.
	 *   - Return false for a monitor the plug-in cannot describe; the runtime
	 *     then derives defaults from EDID (no eye tracking, view scale 1).
	 *
	 * Optional. NULL (or a plug-in whose `struct_size` predates this field)
	 * ⟹ the runtime uses @ref get_display_info for the active plug-in's
	 * panel and EDID-derived defaults for every other monitor. Appended per
	 * ADR-020 (append-only within a major; gated by @ref struct_size; no
	 * XRT_PLUGIN_API_VERSION_CURRENT bump) AFTER the stereo camera block.
	 * Announced by @ref XRT_PLUGIN_IFACE_HAS_DISPLAY_INFO_FOR_MONITOR.
	 */
	bool (*get_display_info_for_monitor)(struct xrt_plugin_instance *inst,
	                                     const struct xrt_display_descriptor *display,
	                                     const struct xrt_display_physical *physical,
	                                     struct xrt_plugin_display_info *out_info);

	/*!
	 * Create a Vulkan display processor for ONE screen this plug-in won in
	 * the per-monitor registry (multi-screen M2, ADR-047 D2: a window that
	 * spans several screens is woven per segment by each screen's own DP).
	 *
	 * Same contract as @ref create_dp_vk (`xrt_dp_factory_vk_fn_t`), plus:
	 *   - @p inst is this plug-in's instance (NULL for a plug-in without
	 *     instance state).
	 *   - @p binding names the screen (monitor id, desktop rect, native px,
	 *     mm, serial, vendor display id); never NULL. The DP should answer
	 *     `get_display_dimensions` / `get_display_pixel_info` for THAT screen,
	 *     with `out_screen_left/top` = the binding's desktop origin.
	 *   - @p window_handle is NULL on desktop Linux: a segment DP is
	 *     windowless and gets its phase from `set_present_origin` (ADR-033).
	 *   - The compositor calls it with a canvas that is the segment (a
	 *     sub-rect of the target) and a pre-cropped atlas holding exactly that
	 *     segment's views; the DP MUST confine its output — render area,
	 *     viewport AND scissor — to the canvas (multi-screen plan risk 7), and
	 *     must not assume it is the first writer to the target this frame.
	 *   - Several instances may coexist in one process, one per screen.
	 *
	 * Optional. NULL (or a plug-in whose `struct_size` predates this field)
	 * ⟹ the plug-in's screens other than the session's primary one get a
	 * flat 2D view, never a @ref create_dp_vk DP: that DP describes the
	 * plug-in's one panel and is not bound by the canvas / not-first-writer
	 * contract above, so it could wipe the segment woven before it.
	 * Appended per ADR-020 (append-only within a major; gated by
	 * @ref struct_size; no XRT_PLUGIN_API_VERSION_CURRENT bump) after
	 * @ref get_display_info_for_monitor. Announced by
	 * @ref XRT_PLUGIN_IFACE_HAS_CREATE_DP_FOR_SCREEN.
	 */
	xrt_result_t (*create_dp_vk_for_screen)(struct xrt_plugin_instance *inst,
	                                        void *vk_bundle,
	                                        void *vk_cmd_pool,
	                                        void *window_handle,
	                                        int32_t target_format,
	                                        const struct xrt_screen_binding *binding,
	                                        struct xrt_display_processor **out_xdp);

	/*!
	 * Create a D3D11 display processor for ONE screen this plug-in won in
	 * the per-monitor registry (multi-screen M6, ADR-047 D2 on Windows): the
	 * in-process D3D11 compositor weaves a window that spans several
	 * monitors per segment, each by its own screen's DP, into the one
	 * presented back buffer.
	 *
	 * Same contract as @ref create_dp_d3d11 (`xrt_dp_factory_d3d11_fn_t`),
	 * plus everything @ref create_dp_vk_for_screen promises:
	 *   - @p binding names the screen; never NULL. Vendor DPs resolve it to
	 *     their own display identity (`binding->display_id`, else their
	 *     per-monitor claim) and bind their weaver to THAT display.
	 *   - @p window_handle: the session's real HWND for the DP of the screen
	 *     that holds the MAJORITY of the window (it keeps the vendor's drag
	 *     phase-snap), NULL for every other screen's DP, which is windowless
	 *     and gets its phase from
	 *     @ref xrt_display_processor_d3d11::set_present_origin plus the
	 *     canvas offset. A DP must weave with a NULL window.
	 *   - The compositor binds the back buffer, then calls
	 *     @ref xrt_display_processor_d3d11::process_atlas with
	 *     `canvas = the segment` and a pre-cropped atlas holding exactly that
	 *     segment's views; the DP MUST confine its output — viewport AND
	 *     scissor — to the canvas, and must not assume it is the first
	 *     writer to the back buffer this frame.
	 *   - Several instances coexist in one process, one per screen.
	 *
	 * Optional. NULL (or a plug-in whose `struct_size` predates this field)
	 * ⟹ the plug-in's screens other than the session's primary one get a
	 * flat 2D view (never a @ref create_dp_d3d11 DP, for the reason given on
	 * @ref create_dp_vk_for_screen). Appended per ADR-020 (append-only
	 * within a major; gated by @ref struct_size; no
	 * XRT_PLUGIN_API_VERSION_CURRENT bump) after
	 * @ref create_dp_vk_for_screen. Announced by
	 * @ref XRT_PLUGIN_IFACE_HAS_CREATE_DP_D3D11_FOR_SCREEN.
	 */
	xrt_result_t (*create_dp_d3d11_for_screen)(struct xrt_plugin_instance *inst,
	                                           void *d3d11_device,
	                                           void *d3d11_context,
	                                           void *window_handle,
	                                           const struct xrt_screen_binding *binding,
	                                           struct xrt_display_processor_d3d11 **out_xdp);

	/*!
	 * Create a Metal display processor for ONE screen this plug-in won in
	 * the per-monitor registry (multi-screen on macOS, ADR-047 D2): the
	 * in-process Metal compositor weaves a window that spans several
	 * displays per segment, each by its own screen's DP, into the one
	 * CAMetalLayer drawable.
	 *
	 * Same contract as @ref create_dp_metal (`xrt_dp_factory_metal_fn_t`),
	 * plus everything @ref create_dp_vk_for_screen promises. What a plug-in
	 * implements:
	 *   - @p inst is this plug-in's instance (NULL for a plug-in without
	 *     instance state).
	 *   - @p binding names the screen; never NULL. `desktop_*` is the
	 *     display's `CGDisplayBounds` in top-down POINTS; `native_pixel_*`
	 *     is its current mode in backing px; `desktop_scale` its backing
	 *     scale; `device_name` the CoreGraphics display UUID. Vendor DPs
	 *     resolve it to their own display identity (`binding->display_id`,
	 *     else their per-monitor claim) and bind their weaver to THAT
	 *     display. The DP should answer `get_display_dimensions` /
	 *     `get_display_pixel_info` for that screen.
	 *   - @p window_handle is NULL today: every per-screen DP is windowless
	 *     and gets its phase from
	 *     @ref xrt_display_processor_metal::set_present_origin (backing px
	 *     relative to that display's `CGDisplayBounds` origin, computed by
	 *     the runtime per frame) plus the canvas offset — it must not try to
	 *     derive the origin from a window. The session's own NSView stays
	 *     with the primary screen's @ref create_dp_metal DP. A DP must weave
	 *     with a NULL view.
	 *   - The compositor calls @ref xrt_display_processor_metal::process_atlas
	 *     on the shared command buffer with `canvas = the segment` (drawable
	 *     px) and a pre-cropped atlas holding exactly that segment's views.
	 *     The DP encodes its own render pass, so it MUST confine its output —
	 *     viewport AND scissor rect — to the canvas, and must not assume it
	 *     is the first writer to the target this frame: use
	 *     `MTLLoadActionLoad`, never `MTLLoadActionClear`, on the target.
	 *   - Several instances coexist in one process, one per screen.
	 *
	 * Optional. NULL (or a plug-in whose `struct_size` predates this field)
	 * ⟹ the plug-in's screens other than the session's primary one get a
	 * flat 2D view (never a @ref create_dp_metal DP, for the reason given on
	 * @ref create_dp_vk_for_screen). Appended per ADR-020 (append-only
	 * within a major; gated by @ref struct_size; no
	 * XRT_PLUGIN_API_VERSION_CURRENT bump) after
	 * @ref create_dp_d3d11_for_screen. Announced by
	 * @ref XRT_PLUGIN_IFACE_HAS_CREATE_DP_METAL_FOR_SCREEN.
	 */
	xrt_result_t (*create_dp_metal_for_screen)(struct xrt_plugin_instance *inst,
	                                           void *metal_device,
	                                           void *command_queue,
	                                           void *window_handle,
	                                           const struct xrt_screen_binding *binding,
	                                           struct xrt_display_processor_metal **out_xdp);

	/*!
	 * Create a D3D12 display processor for ONE screen this plug-in won in
	 * the per-monitor registry (multi-screen M6, ADR-047 D2 on Windows): the
	 * in-process D3D12 compositor weaves a window that spans several
	 * monitors per segment, each by its own screen's DP, into the one
	 * presented back buffer. The D3D12 twin of
	 * @ref create_dp_d3d11_for_screen.
	 *
	 * Same contract as @ref create_dp_d3d12 (`xrt_dp_factory_d3d12_fn_t`),
	 * plus everything @ref create_dp_d3d11_for_screen promises, in D3D12
	 * terms:
	 *   - @p binding names the screen; never NULL.
	 *   - @p window_handle: the session's real HWND for the majority
	 *     screen's DP, NULL for every other screen's DP, which is windowless
	 *     and gets its phase from
	 *     @ref xrt_display_processor_d3d12::set_present_origin plus the
	 *     canvas offset. A DP must weave with a NULL window.
	 *   - The compositor records every segment onto ONE command list:
	 *     @ref xrt_display_processor_d3d12::process_atlas gets
	 *     `canvas = the segment`, a pre-cropped plain 2D atlas holding
	 *     exactly that segment's views (COMMON state, its own SRV
	 *     descriptor), and the shared back buffer. A command list carries no
	 *     state a DP could inherit, so the DP itself MUST confine its output
	 *     — viewport AND scissor — to the canvas, and must not assume it is
	 *     the first writer to the back buffer this frame.
	 *   - Several instances coexist in one process, one per screen.
	 *
	 * Optional. NULL (or a plug-in whose `struct_size` predates this field)
	 * ⟹ the plug-in's screens other than the session's primary one get a
	 * flat 2D view. Appended per ADR-020 (append-only within a major; gated
	 * by @ref struct_size; no XRT_PLUGIN_API_VERSION_CURRENT bump) after
	 * @ref create_dp_metal_for_screen. Announced by
	 * @ref XRT_PLUGIN_IFACE_HAS_CREATE_DP_D3D12_FOR_SCREEN.
	 */
	xrt_result_t (*create_dp_d3d12_for_screen)(struct xrt_plugin_instance *inst,
	                                           void *d3d12_device,
	                                           void *d3d12_command_queue,
	                                           void *window_handle,
	                                           const struct xrt_screen_binding *binding,
	                                           struct xrt_display_processor_d3d12 **out_xdp);

	/*!
	 * Vendor summary for ONE screen this plug-in claimed (ADR-051 D2): fill
	 * @p out for the monitor @p monitor_id (an
	 * @ref xrt_display_descriptor::monitor_id this plug-in returned a claim
	 * for). The caller sets `out->struct_size` and zero-fills the rest; the
	 * plug-in writes nothing past it and sets `out->version`.
	 *
	 * PASSIVE: this call must not create or touch a tracker, lens, display or
	 * weaver handle, must not create a per-call vendor instance, and must
	 * return in < 20 ms. It may be polled every 2 s by the service and by
	 * diagnostic processes. Bump @ref xrt_plugin_screen_status::change_counter
	 * on the vendor's own device / topology events so a poller can skip an
	 * unchanged screen.
	 *
	 * @p inst may be NULL (a plug-in whose probe keeps no instance state).
	 * Returns XRT_SUCCESS when @p out was filled, an error for a monitor the
	 * plug-in did not claim; the runtime then shows "no vendor status".
	 *
	 * Optional. NULL (or a plug-in whose `struct_size` predates this field)
	 * ⟹ "no vendor status" — never an error. Appended per ADR-020
	 * (append-only within a major; gated by @ref struct_size; no
	 * XRT_PLUGIN_API_VERSION_CURRENT bump) after
	 * @ref create_dp_d3d12_for_screen. Announced by
	 * @ref XRT_PLUGIN_IFACE_HAS_GET_SCREEN_STATUS.
	 */
	xrt_result_t (*get_screen_status)(struct xrt_plugin_instance *inst,
	                                  uint64_t monitor_id,
	                                  struct xrt_plugin_screen_status *out);
};

/*!
 * Defined when @ref xrt_plugin_iface carries @ref
 * xrt_plugin_iface::create_dp_d3d11_lift (ADR-042), so a plug-in built against
 * an older runtime header can #ifdef-guard filling it.
 */
#define XRT_PLUGIN_IFACE_HAS_D3D11_LIFT_FACTORY 1

/*!
 * Defined when @ref xrt_plugin_iface carries @ref
 * xrt_plugin_iface::get_platform_state and this header defines
 * @ref xrt_plugin_platform_state / @ref xrt_plugin_platform_status (ADR-045),
 * so a plug-in built against an older runtime header can #ifdef-guard
 * implementing it.
 */
#define XRT_PLUGIN_HAS_PLATFORM_STATE 1

/*!
 * Defined when @ref xrt_plugin_iface carries the stereo camera slots
 * (ADR-043), so a plug-in built against an older runtime header can
 * #ifdef-guard filling them.
 */
#define XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA 1

/*!
 * Defined when @ref xrt_plugin_iface carries @ref
 * xrt_plugin_iface::get_display_info_for_monitor and this header defines
 * @ref xrt_display_physical (multi-screen M1), so a plug-in built against an older runtime header can
 * #ifdef-guard implementing it.
 */
#define XRT_PLUGIN_IFACE_HAS_DISPLAY_INFO_FOR_MONITOR 1

/*!
 * True when @p iface implements @ref
 * xrt_plugin_iface::get_display_info_for_monitor (and its struct_size covers
 * the slot).
 */
static inline bool
xrt_plugin_iface_has_display_info_for_monitor(const struct xrt_plugin_iface *iface)
{
	return iface != NULL &&
	       iface->struct_size >= offsetof(struct xrt_plugin_iface, get_display_info_for_monitor) +
	                                 sizeof(iface->get_display_info_for_monitor) &&
	       iface->get_display_info_for_monitor != NULL;
}

/*!
 * Defined when @ref xrt_plugin_iface carries @ref
 * xrt_plugin_iface::create_dp_vk_for_screen and this header defines @ref
 * xrt_screen_binding (multi-screen M2), so a plug-in built against an older
 * runtime header can #ifdef-guard implementing it.
 */
#define XRT_PLUGIN_IFACE_HAS_CREATE_DP_FOR_SCREEN 1

/*!
 * True when @p iface implements @ref xrt_plugin_iface::create_dp_vk_for_screen
 * (and its struct_size covers the slot).
 */
static inline bool
xrt_plugin_iface_has_create_dp_vk_for_screen(const struct xrt_plugin_iface *iface)
{
	return iface != NULL &&
	       iface->struct_size >= offsetof(struct xrt_plugin_iface, create_dp_vk_for_screen) +
	                                 sizeof(iface->create_dp_vk_for_screen) &&
	       iface->create_dp_vk_for_screen != NULL;
}

/*!
 * Defined when @ref xrt_plugin_iface carries @ref
 * xrt_plugin_iface::create_dp_d3d11_for_screen (multi-screen M6, Windows), so
 * a plug-in built against an older runtime header can #ifdef-guard
 * implementing it.
 */
#define XRT_PLUGIN_IFACE_HAS_CREATE_DP_D3D11_FOR_SCREEN 1

/*!
 * True when @p iface implements @ref
 * xrt_plugin_iface::create_dp_d3d11_for_screen (and its struct_size covers the
 * slot).
 */
static inline bool
xrt_plugin_iface_has_create_dp_d3d11_for_screen(const struct xrt_plugin_iface *iface)
{
	return iface != NULL &&
	       iface->struct_size >= offsetof(struct xrt_plugin_iface, create_dp_d3d11_for_screen) +
	                                 sizeof(iface->create_dp_d3d11_for_screen) &&
	       iface->create_dp_d3d11_for_screen != NULL;
}

/*!
 * Defined when @ref xrt_plugin_iface carries @ref
 * xrt_plugin_iface::create_dp_metal_for_screen (multi-screen on macOS), so a
 * plug-in built against an older runtime header can #ifdef-guard
 * implementing it (pair it with XRT_DP_METAL_HAS_PRESENT_ORIGIN).
 */
#define XRT_PLUGIN_IFACE_HAS_CREATE_DP_METAL_FOR_SCREEN 1

/*!
 * True when @p iface implements @ref
 * xrt_plugin_iface::create_dp_metal_for_screen (and its struct_size covers the
 * slot).
 */
static inline bool
xrt_plugin_iface_has_create_dp_metal_for_screen(const struct xrt_plugin_iface *iface)
{
	return iface != NULL &&
	       iface->struct_size >= offsetof(struct xrt_plugin_iface, create_dp_metal_for_screen) +
	                                 sizeof(iface->create_dp_metal_for_screen) &&
	       iface->create_dp_metal_for_screen != NULL;
}

/*!
 * Defined when @ref xrt_plugin_iface carries @ref
 * xrt_plugin_iface::create_dp_d3d12_for_screen (multi-screen M6, Windows
 * D3D12), so a plug-in built against an older runtime header can
 * #ifdef-guard implementing it.
 */
#define XRT_PLUGIN_IFACE_HAS_CREATE_DP_D3D12_FOR_SCREEN 1

/*!
 * True when @p iface implements @ref
 * xrt_plugin_iface::create_dp_d3d12_for_screen (and its struct_size covers the
 * slot).
 */
static inline bool
xrt_plugin_iface_has_create_dp_d3d12_for_screen(const struct xrt_plugin_iface *iface)
{
	return iface != NULL &&
	       iface->struct_size >= offsetof(struct xrt_plugin_iface, create_dp_d3d12_for_screen) +
	                                 sizeof(iface->create_dp_d3d12_for_screen) &&
	       iface->create_dp_d3d12_for_screen != NULL;
}

/*!
 * Defined when @ref xrt_plugin_iface carries @ref
 * xrt_plugin_iface::get_screen_status and this header defines @ref
 * xrt_plugin_screen_status (ADR-051 D2), so a plug-in built against an older
 * runtime header can #ifdef-guard implementing it.
 */
#define XRT_PLUGIN_IFACE_HAS_GET_SCREEN_STATUS 1

/*!
 * True when @p iface implements @ref xrt_plugin_iface::get_screen_status (and
 * its struct_size covers the slot).
 */
static inline bool
xrt_plugin_iface_has_get_screen_status(const struct xrt_plugin_iface *iface)
{
	return iface != NULL &&
	       iface->struct_size >=
	           offsetof(struct xrt_plugin_iface, get_screen_status) + sizeof(iface->get_screen_status) &&
	       iface->get_screen_status != NULL;
}

/*!
 * True when @p iface fills the whole stereo camera slot set (and its
 * struct_size covers it).
 */
static inline bool
xrt_plugin_iface_has_stereo_camera(const struct xrt_plugin_iface *iface)
{
	if (iface == NULL || iface->struct_size < offsetof(struct xrt_plugin_iface, stereo_camera_close) +
	                                              sizeof(iface->stereo_camera_close)) {
		return false;
	}
	return iface->stereo_camera_enumerate != NULL && iface->stereo_camera_get_calibration != NULL &&
	       iface->stereo_camera_open != NULL && iface->stereo_camera_wait_frame != NULL &&
	       iface->stereo_camera_release_frame != NULL && iface->stereo_camera_close != NULL;
}


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
