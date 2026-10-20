// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Vendor-generic display status snapshot (ADR-051 D1).
 *
 * The ONE model every status renderer reads: `displayxr-cli status`, the
 * Dashboard, the MCP surface and any later web page. Its JSON shape is
 * `docs/roadmap/display-dashboard.md` §3 (`u_status_snapshot_to_cjson`), its
 * text shape is §7 (`u_status_snapshot_format_text`), and its runtime-derived
 * warnings are §4 (`u_status_warnings_derive`).
 *
 * Plain data, fixed size, NO pointers: phase 2 serves it from the service over
 * the DIAG IPC path by value (in pieces — see the size note below).
 *
 * Who fills what, by phase:
 *  - phase 1: the headless builder (`target_status_snapshot_build_headless`) —
 *    runtime, plugins, screens (identity, desktop, native, physical, roles,
 *    claim, layout, eye-tracking caps); `source = HEADLESS`, no clients,
 *    generation 0, every screen `eye_tracking.state = NO_DP`, no `mode`, no `dps`.
 *  - phase 2: the service — generation counters, clients[], per-screen mode,
 *    live tracking state and the bound DPs, workspace.
 *  - phase 3: the plug-in slot `get_screen_status` — the per-screen vendor cell.
 *
 * Size: tens of KB (see `tests_status_snapshot`), so the service does not send
 * it as one reply: it crosses as @ref xrt_status_head + one
 * @ref xrt_status_screen per `system_get_status_snapshot(screen_index)` reply
 * + one @ref xrt_status_client per `system_get_client_segments(client_id)`
 * reply — each a fixed-size struct by value, the mechanism
 * `system_enumerate_displays` uses for `xrt_screen_list` (see
 * `u_status_snapshot_get_head` / `u_status_snapshot_set_head`).
 *
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_screen.h"
#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_input_selection.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Snapshot schema; bumped only on an incompatible change (keys are only ever added).
#define XRT_STATUS_SCHEMA 1

//! Registered plug-ins carried in a snapshot.
#define XRT_STATUS_MAX_PLUGINS 8
//! Screens carried in a snapshot (= the screen-list max).
#define XRT_STATUS_MAX_SCREENS XRT_SCREEN_LIST_MAX
//! Clients carried in a snapshot (= IPC_MAX_CLIENTS, which xrt/ cannot include).
#define XRT_STATUS_MAX_CLIENTS 32
//! Warnings per screen and at system level.
#define XRT_STATUS_MAX_WARNINGS 8
//! Display processors bound to one screen.
#define XRT_STATUS_MAX_SCREEN_DPS 8
//! Segments per client (= XRT_MAX_SEGMENTS).
#define XRT_STATUS_MAX_SEGMENTS XRT_MAX_SEGMENTS
//! Size of a warning code, incl. the NUL.
#define XRT_STATUS_CODE_MAX 32
//! Size of a warning text, incl. the NUL.
#define XRT_STATUS_TEXT_MAX 128
//! Size of short identity strings (ids, names, serials), incl. the NUL.
#define XRT_STATUS_NAME_MAX 64
//! Conversion providers carried in a snapshot (ADR-053; roadmap §6.1 "<= 8").
#define XRT_STATUS_MAX_CONV_PROVIDERS 8
//! Conversion task rows carried in a snapshot (provider x capability, plus composed rows; "<= 32").
#define XRT_STATUS_MAX_CONV_TASKS 32
//! Ceiling for ONE conversion row reply over DIAG (the row + its generation + the result code).
#define XRT_STATUS_CONV_ROW_REPLY_MAX 1024
//! "No provider" in @ref xrt_status_conv_task::provider.
#define XRT_STATUS_CONV_NO_PROVIDER 0xFFFFFFFFu
//! "Not in the selection order" in @ref xrt_status_conv_task::order (dropped by the filter).
#define XRT_STATUS_CONV_NOT_ORDERED 0xFFFFFFFFu

/*!
 * Bits for @ref xrt_status_claim::apis — the graphics APIs whose DP factory the
 * winning claim offers on that screen.
 * @{
 */
#define XRT_STATUS_API_BIT_D3D11 (1u << 0)
#define XRT_STATUS_API_BIT_D3D12 (1u << 1)
#define XRT_STATUS_API_BIT_VK (1u << 2)
#define XRT_STATUS_API_BIT_GL (1u << 3)
#define XRT_STATUS_API_BIT_METAL (1u << 4)
/*! @} */

//! Where the snapshot was built (JSON "service" / "headless").
enum xrt_status_source
{
	XRT_STATUS_SOURCE_SERVICE = 0,  //!< Assembled in the running service (live).
	XRT_STATUS_SOURCE_HEADLESS = 1, //!< Assembled in-process: "what a process starting now would get".
};

//! Where a screen's per-screen DP preference came from (JSON lower-case; null for NONE).
//! Values equal `enum u_setting_source`.
enum xrt_status_pref_source
{
	XRT_STATUS_PREF_SOURCE_NONE = 0,    //!< No preference for this screen.
	XRT_STATUS_PREF_SOURCE_ENV = 1,     //!< `DXR_PREFERRED_PLUGIN_PER_SCREEN`.
	XRT_STATUS_PREF_SOURCE_USER = 2,    //!< The per-user settings file (what the dashboard writes).
	XRT_STATUS_PREF_SOURCE_MACHINE = 3, //!< HKLM (admin).
};

//! When a change of a screen's DP takes effect (JSON "live" / "next-session").
enum xrt_status_apply
{
	//! A segment DP of the service path: recreated on the service's re-probe.
	XRT_STATUS_APPLY_LIVE = 0,
	//! The primary screen (and every in-process app): at the next session / service start.
	XRT_STATUS_APPLY_NEXT_SESSION = 1,
};

//! Live tracking state of a screen (JSON upper-case).
enum xrt_status_tracking
{
	XRT_STATUS_TRACKING_TRACKING = 0,     //!< A DP bound to the screen reports a tracked viewer.
	XRT_STATUS_TRACKING_NOT_TRACKING = 1, //!< A DP is bound, nobody is tracked.
	XRT_STATUS_TRACKING_NO_DP = 2,        //!< No DP is bound to the screen (always, headless).
	XRT_STATUS_TRACKING_UNKNOWN = 3,      //!< A DP is bound but did not answer.
};

//! Who presents a client's frames (JSON upper-case).
enum xrt_status_presenter
{
	XRT_STATUS_PRESENTER_APP_HWND = 0,       //!< The service presents into the app's own window.
	XRT_STATUS_PRESENTER_CLIENT_TEXTURE = 1, //!< The client presents a shared texture itself.
	XRT_STATUS_PRESENTER_NONE = 2,           //!< Nothing presents for it right now.
	XRT_STATUS_PRESENTER_SERVICE_WINDOW = 3, //!< Hosted: the service presents into its own window.
	XRT_STATUS_PRESENTER_SELF = 4,           //!< A weave present-owner: it weaves and presents itself.
};

//! Who holds the panel lease, from a client's point of view (JSON lower-case, §3).
enum xrt_status_lease
{
	XRT_STATUS_LEASE_CONTROLLER = 0, //!< A workspace controller holds it.
	XRT_STATUS_LEASE_SLOT = 1,       //!< This client's slot holds it (default policy).
	XRT_STATUS_LEASE_NONE = 2,       //!< Nobody / not this client.
};

//! Warning severity (JSON lower-case, §4). Only WARN and CRITICAL light a badge.
enum xrt_status_level
{
	XRT_STATUS_LEVEL_INFO = 0,
	XRT_STATUS_LEVEL_WARN = 1,
	XRT_STATUS_LEVEL_CRITICAL = 2,
};

//! Vendor tracker state of a screen (phase 3 plug-in slot; JSON upper-case).
enum xrt_status_vendor_tracker
{
	XRT_STATUS_VENDOR_TRACKER_UNKNOWN = 0,     //!< Not reported.
	XRT_STATUS_VENDOR_TRACKER_NONE = 1,        //!< The screen has no tracker.
	XRT_STATUS_VENDOR_TRACKER_OFF = 2,         //!< Present, not running.
	XRT_STATUS_VENDOR_TRACKER_STARTING = 3,    //!< Starting up.
	XRT_STATUS_VENDOR_TRACKER_RUNNING = 4,     //!< Running.
	XRT_STATUS_VENDOR_TRACKER_DOWN = 5,        //!< Expected to run and is not.
	XRT_STATUS_VENDOR_TRACKER_UNSUPPORTED = 6, //!< The platform cannot track this screen.
};

//! Vendor lens state of a screen (phase 3; JSON "2D" / "3D" / "UNKNOWN").
enum xrt_status_vendor_lens
{
	XRT_STATUS_VENDOR_LENS_UNKNOWN = 0,
	XRT_STATUS_VENDOR_LENS_2D = 1,
	XRT_STATUS_VENDOR_LENS_3D = 2,
};

//! Where a screen's millimetres came from (JSON lower-case).
enum xrt_status_mm_source
{
	XRT_STATUS_MM_SOURCE_NONE = 0,   //!< Unknown — the 0 m trap.
	XRT_STATUS_MM_SOURCE_EDID = 1,   //!< The monitor's EDID.
	XRT_STATUS_MM_SOURCE_PLUGIN = 2, //!< The claiming plug-in's display info.
};

//! Graphics API of a bound DP (JSON lower-case).
enum xrt_status_dp_api
{
	XRT_STATUS_DP_API_D3D11 = 0,
	XRT_STATUS_DP_API_D3D12 = 1,
	XRT_STATUS_DP_API_VK = 2,
	XRT_STATUS_DP_API_GL = 3,
	XRT_STATUS_DP_API_METAL = 4,
};

//! Role of a bound DP (JSON lower-case).
enum xrt_status_dp_kind
{
	XRT_STATUS_DP_KIND_PRIMARY = 0, //!< The session's own DP.
	XRT_STATUS_DP_KIND_SEGMENT = 1, //!< A per-segment DP (ADR-047 multi-screen).
};

//! Backend health of a bound DP; values equal `XRT_DP_BACKEND_STATE_*` (JSON upper-case).
enum xrt_status_dp_backend
{
	XRT_STATUS_DP_BACKEND_OK = 0,
	XRT_STATUS_DP_BACKEND_DEGRADED = 1, //!< Reconnecting to its vendor backend.
	XRT_STATUS_DP_BACKEND_STALE = 2,    //!< Cannot reconnect; being recreated.
};

//! Where a segment's eyes come from (JSON upper-case).
enum xrt_status_eye_source
{
	XRT_STATUS_EYE_SOURCE_NONE = 0,    //!< No eyes (flat, or the DP has none).
	XRT_STATUS_EYE_SOURCE_DP = 1,      //!< That screen's own DP.
	XRT_STATUS_EYE_SOURCE_PRIMARY = 2, //!< Borrowed from the session's primary DP.
};

/*!
 * One warning: `{code, level, text}` (§4). Runtime-derived codes are listed in
 * `u_status_snapshot.h`; vendor codes pass through verbatim in the vendor cell.
 */
struct xrt_status_warning
{
	char code[XRT_STATUS_CODE_MAX]; //!< Stable upper-case code, e.g. "NOT_NATIVE".
	enum xrt_status_level level;    //!< Severity.
	char text[XRT_STATUS_TEXT_MAX]; //!< One sentence that says what to do.
};

/*!
 * The vendor summary cell of one screen (ADR-051 D2). Phase 3 fills it from the
 * plug-in slot `get_screen_status`; until then @ref present is false everywhere.
 */
struct xrt_status_vendor
{
	bool present;                            //!< The claiming plug-in answered (false = "no vendor status").
	bool ready;                              //!< The vendor says the screen is usable.
	bool verified;                           //!< The vendor verified the screen's identity.
	bool calibrated;                         //!< The vendor has a calibration for it.
	enum xrt_status_vendor_tracker tracker;  //!< Tracker state.
	enum xrt_status_vendor_lens lens;        //!< Lens state.
	char model[32];                          //!< Vendor model string, shown verbatim.
	char serial[32];                         //!< Vendor serial, shown verbatim.
	struct xrt_status_warning worst_warning; //!< Worst vendor warning; empty code = none.
	char dashboard_command[160];             //!< Launch template ("{serial}" / "{monitor_id}"); "" = none.
};

/*!
 * One registered display plug-in (ADR-045 record + registration).
 */
struct xrt_status_plugin
{
	char id[XRT_STATUS_NAME_MAX];      //!< Discovery id.
	char name[128];                    //!< Registration DisplayName.
	char vendor[XRT_STATUS_NAME_MAX];  //!< Registration vendor ("" if unknown).
	char version[XRT_STATUS_NAME_MAX]; //!< Registration Version.
	char load[24];                     //!< Loader outcome, e.g. "ACTIVE", "DECLINED", "NOT_ATTEMPTED".
	uint32_t platform_state;           //!< `enum xrt_plugin_platform_state` (0 = UNKNOWN).
	char hint[128];                    //!< The plug-in's platform hint, verbatim.
	bool fallback;                     //!< A FALLBACK plug-in (sim-display).
	bool active;                       //!< The active plug-in of the process that built the snapshot.
	uint32_t probe_order;              //!< ProbeOrder (lower wins).
};

//! EDID identity of a screen.
struct xrt_status_edid
{
	char manufacturer[4]; //!< 3-letter PNP code, e.g. "AUO"; "" when unknown.
	char product[8];      //!< Product code as 4 hex digits; "" when unknown.
	uint32_t serial;      //!< EDID serial number; 0 = none.
};

//! A screen's place on the desktop.
struct xrt_status_desktop
{
	int32_t left;   //!< Desktop rect, the space the OS places windows in.
	int32_t top;    //!< See @ref left.
	uint32_t width; //!< Current mode width, pixels.
	uint32_t height;
	float scale; //!< Desktop compositor scale (e.g. 2.5); 0 = unknown.
};

//! How @ref xrt_status_native::is_native was decided.
enum xrt_status_native_basis
{
	//! The desktop rect against the native mode (Windows, macOS, X11 without
	//! a compositor that reports modes).
	XRT_STATUS_NATIVE_BASIS_DESKTOP = 0,
	//! The compositor's own facts: its current mode against its preferred
	//! mode, both device pixels (desktop Linux, Mutter DisplayConfig). The
	//! desktop rect is logical on a scaled desktop and is not consulted.
	XRT_STATUS_NATIVE_BASIS_COMPOSITOR = 1,
};

//! A screen's native mode.
struct xrt_status_native
{
	uint32_t width;       //!< Native width, pixels; 0 = unknown.
	uint32_t height;      //!< Native height, pixels; 0 = unknown.
	uint32_t refresh_mhz; //!< Current refresh, milli-Hz; 0 = unknown.
	bool is_native;       //!< The screen runs its native mode (false when unknown); see @ref basis.
	//! How @ref is_native was decided (`enum xrt_status_native_basis`).
	uint32_t basis;
	//! COMPOSITOR basis: the compositor's current mode, device pixels (0 otherwise).
	uint32_t current_width;
	uint32_t current_height;
};

//! A screen's physical size.
struct xrt_status_physical
{
	uint32_t width_mm;                //!< Visible width, mm; 0 = unknown.
	uint32_t height_mm;               //!< Visible height, mm; 0 = unknown.
	enum xrt_status_mm_source source; //!< Where the numbers came from.
};

//! The three distinct "primary" notions (ADR-051 D1).
struct xrt_status_roles
{
	bool os_main;         //!< The OS desktop's primary monitor.
	bool runtime_default; //!< The runtime's SYSTEM_DEFAULT screen (where a non-spanning window lands).
	bool vendor_primary;  //!< The vendor's primary (phase 3; false until then).
};

//! The winning display claim of a screen.
struct xrt_status_claim
{
	char plugin_id[XRT_STATUS_NAME_MAX]; //!< Winning plug-in ("" = unclaimed).
	uint32_t confidence;                 //!< `enum xrt_display_claim_confidence` value (0 = none).
	char serial[XRT_STATUS_NAME_MAX];    //!< Vendor serial from the claim ("" if n/a).
	uint32_t apis;                       //!< `XRT_STATUS_API_BIT_*` factories the claim offers.
	//! @ref plugin_id won by a per-screen preference (display dashboard phase 7).
	bool forced;
	//! The per-screen preference for this screen, honoured or not ("" = none).
	char preferred_plugin[32];
	//! Where @ref preferred_plugin came from.
	enum xrt_status_pref_source preferred_source;
	//! When a change of this screen's DP takes effect.
	enum xrt_status_apply apply;
};

//! The metres + nominal viewer the segment layout uses for a screen.
struct xrt_status_layout
{
	float width_m;            //!< Physical width, metres; 0 = unknown.
	float height_m;           //!< Physical height, metres; 0 = unknown.
	float nominal_viewer_x_m; //!< Nominal viewer, screen-centre relative, metres.
	float nominal_viewer_y_m; //!< See @ref nominal_viewer_x_m.
	float nominal_viewer_z_m; //!< See @ref nominal_viewer_x_m.
	uint32_t source;          //!< `enum xrt_screen_info_source`.
};

//! Eye-tracking capability and live state of a screen.
struct xrt_status_eye_tracking
{
	uint32_t supported;             //!< Mode bits (bit 0 MANAGED, bit 1 MANUAL); 0 = none.
	uint32_t default_mode;          //!< 0 MANAGED, 1 MANUAL (meaningful iff @ref supported).
	enum xrt_status_tracking state; //!< Live state (phase 2; NO_DP headless).
	uint32_t not_tracking_ms;       //!< How long @ref state has been NOT_TRACKING (phase 2; feeds TRACKER_DOWN).
};

//! The rendering mode a screen is in (phase 2; absent headless).
struct xrt_status_mode
{
	bool valid;     //!< A DP bound to the screen reported a mode.
	uint32_t index; //!< Rendering-mode index.
	char name[32];  //!< Mode name.
	uint32_t views; //!< View count.
	bool is_3d;     //!< Hardware display in 3D.
};

//! One DP bound to a screen (phase 2).
struct xrt_status_dp
{
	uint32_t client_id;                 //!< IPC client id (0 = the service's own).
	enum xrt_status_dp_api api;         //!< Graphics API of the DP.
	enum xrt_status_dp_kind kind;       //!< Primary or segment DP.
	enum xrt_status_dp_backend backend; //!< Backend health.
};

/*!
 * One screen row — the backbone of the dashboard. Registry order, the system
 * default first.
 */
struct xrt_status_screen
{
	uint64_t id;                                 //!< Registry monitor id (never 0).
	uint32_t index;                              //!< Row index.
	char device_name[XRT_STATUS_NAME_MAX];       //!< OS device name (`\\.\DISPLAY1`, `HDMI-1`); "" = unknown.
	char friendly_name[XRT_STATUS_NAME_MAX];     //!< Human name (EDID model, else "PNP PROD").
	char key[XRT_STATUS_NAME_MAX];               //!< Stable screen key ("" = unknown).
	struct xrt_status_edid edid;                 //!< EDID identity.
	struct xrt_status_desktop desktop;           //!< Desktop placement + scale.
	struct xrt_status_native native;             //!< Native mode.
	struct xrt_status_physical physical;         //!< Physical size.
	struct xrt_status_roles roles;               //!< OS main / runtime default / vendor primary.
	struct xrt_status_claim claim;               //!< Winning claim.
	struct xrt_status_layout layout;             //!< Metres + nominal viewer.
	struct xrt_status_eye_tracking eye_tracking; //!< Caps + live state.
	struct xrt_status_mode mode;                 //!< Current rendering mode (phase 2).
	uint32_t dp_count;                           //!< Valid entries in @ref dps.
	struct xrt_status_dp dps[XRT_STATUS_MAX_SCREEN_DPS];         //!< DPs bound to the screen (phase 2).
	struct xrt_status_vendor vendor;                             //!< Vendor cell (phase 3).
	uint32_t warning_count;                                      //!< Valid entries in @ref warnings.
	struct xrt_status_warning warnings[XRT_STATUS_MAX_WARNINGS]; //!< Derived per-screen warnings (§4).
};

//! A rectangle in window pixels.
struct xrt_status_rect
{
	int32_t x;  //!< Left.
	int32_t y;  //!< Top.
	uint32_t w; //!< Width.
	uint32_t h; //!< Height.
};

//! One segment of a client's window (phase 2).
struct xrt_status_segment
{
	uint64_t screen;                       //!< Screen id the segment lies on.
	struct xrt_status_rect canvas;         //!< Segment rect in the window's canvas, pixels.
	bool has_dp;                           //!< A DP exists for that screen right now.
	bool woven;                            //!< Woven in 3D (else a flat 2D copy).
	enum xrt_status_eye_source eye_source; //!< Where the segment's eyes come from.
};

//! A client's segment table (phase 2).
struct xrt_status_segments
{
	uint64_t generation;                                      //!< `xrt_segment_metrics::generation`.
	bool split;                                               //!< The window is woven per screen.
	uint32_t count;                                           //!< Valid entries in @ref items.
	struct xrt_status_segment items[XRT_STATUS_MAX_SEGMENTS]; //!< Left to right.
};

//! A client's session flags (phase 2).
struct xrt_status_client_flags
{
	bool active;  //!< Session active.
	bool visible; //!< Session visible.
	bool focused; //!< Session focused.
	bool overlay; //!< Overlay session.
	//! Device reads are live for this client: focused, or a sibling connection
	//! of the focused client's application (input focus follows the application).
	bool input_focused;
};

//! A client's window rect (phase 2).
struct xrt_status_window
{
	bool valid;      //!< The service knows the window.
	int32_t left;    //!< Client-area origin, desktop coordinates.
	int32_t top;     //!< See @ref left.
	uint32_t width;  //!< Client-area size, pixels.
	uint32_t height; //!< See @ref width.
};

//! View-set counts of a client (phase 2).
struct xrt_status_views
{
	uint32_t capacity; //!< Views the swapchain was sized for.
	uint32_t active;   //!< Views the current mode uses.
	uint32_t reported; //!< Views reported to the app (incl. segment view sets).
};

//! Paint / present / skip integrity triple (#1248) — never a rate without it.
struct xrt_status_integrity
{
	uint64_t paint;           //!< Frames painted.
	uint64_t present;         //!< Frames presented.
	uint64_t skip;            //!< Frames skipped.
	char weave_placement[16]; //!< Weave placement verdict, e.g. "scanout".
};

/*!
 * One IPC client (phase 2; empty headless).
 */
struct xrt_status_client
{
	uint32_t id;                           //!< IPC client id.
	int64_t pid;                           //!< Process id.
	uint32_t client_class;                 //!< `enum xrt_client_class` (verified).
	bool class_verified;                   //!< false = still "unverified".
	char name[XRT_STATUS_NAME_MAX];        //!< Application name.
	struct xrt_status_client_flags flags;  //!< Session flags.
	enum xrt_status_presenter presenter;   //!< Who presents.
	enum xrt_status_lease lease;           //!< Panel lease from this client's view.
	struct xrt_status_window window;       //!< Window rect.
	uint64_t owner_screen;                 //!< Screen owning the window handle (0 = none).
	struct xrt_status_segments segments;   //!< Segment table.
	struct xrt_status_views views;         //!< View-set counts.
	struct xrt_status_integrity integrity; //!< Integrity triple + weave placement.
};

//! Runtime identity block.
struct xrt_status_runtime
{
	char version[32];                //!< "MAJOR.MINOR.PATCH".
	char git_tag[128];               //!< Build git tag.
	uint32_t plugin_abi;             //!< XRT_PLUGIN_API_VERSION_CURRENT.
	char active_openxr_runtime[260]; //!< The OS's ActiveRuntime manifest ("" = unset / n/a).
};

//! Generation counters (phase 2; 0 headless).
struct xrt_status_generation
{
	uint64_t topology; //!< Screens + plug-ins.
	uint64_t status;   //!< Everything else.
	//! Conversion providers (ADR-053): bumps on every readiness transition and
	//! provider-table change, so `status --watch` and the dashboard update live.
	uint64_t conversion;
};

/*!
 * Where a conversion provider runs (ADR-053 §4.4; JSON lower-case
 * "in_service" / "satellite" / "android_service").
 */
enum xrt_status_conv_hosting
{
	XRT_STATUS_CONV_HOSTING_IN_SERVICE = 0,      //!< Loaded in the service (P1; the dp-lift adapter always).
	XRT_STATUS_CONV_HOSTING_SATELLITE = 1,       //!< `displayxr-conversion-host.exe` (P2).
	XRT_STATUS_CONV_HOSTING_ANDROID_SERVICE = 2, //!< The Android `:conversion` process (P5).
};

/*!
 * One conversion provider (ADR-053, `conversion-provider-discovery.md` §7.3).
 * Crosses by value, one per `system_get_conv_provider_row(index)` reply, so it
 * stays under @ref XRT_STATUS_CONV_ROW_REPLY_MAX (asserted below).
 *
 * Provider identity is fine here: the snapshot is a diagnostics surface (CLI,
 * dashboard, MCP), never web content.
 */
struct xrt_status_conv_provider
{
	char id[XRT_STATUS_NAME_MAX];         //!< Registry / manifest id ("dp-lift" for the legacy adapter).
	char name[XRT_STATUS_NAME_MAX];       //!< DisplayName.
	char vendor[XRT_STATUS_NAME_MAX];     //!< Registration vendor ("" if unknown).
	char version[32];                     //!< Registration Version.
	char load[24];                        //!< Loader outcome: "NEGOTIATED", "ABI_MISMATCH", "BUILT_IN", ...
	char binary[256];                     //!< Binary path (truncated; "" for the built-in adapter).
	uint32_t abi;                         //!< ABI the provider reported (0 = never negotiated).
	uint32_t probe_order;                 //!< ProbeOrder (lower wins).
	enum xrt_status_conv_hosting hosting; //!< Where it runs.
	uint32_t platform_state;              //!< `enum xrt_plugin_platform_state` value (get_provider_status).
	char hint[128];                       //!< The provider's status hint (or the loader's reason), verbatim.
	bool enabled;                         //!< `Enabled` (after the per-user override).
	bool fallback;                        //!< FALLBACK provider (sorts last).
	bool hidden;                          //!< Hidden by another provider's `Supersedes`.
	bool licence_exempt;                  //!< Grandfathered (the dp-lift adapter): skips the licence gate.
	bool research_opt_in;                 //!< `AllowResearchOnly` is on (machine-wide; repeated per row).
	bool described;                       //!< Rows exist (live describe or the describe cache).
	int64_t host_pid;                     //!< Satellite host pid; 0 in-service / not running.
	uint32_t restarts;                    //!< Host restarts since service start.
	int32_t last_exit_code;               //!< Last host exit code (0 = none / orderly).
	uint64_t adapter_luid;                //!< Adapter it runs on (0 = CPU / undecided / unknown).
	char backend[32];                     //!< Backend of its first capability ("" = unknown).
	uint32_t vram_used_mb;                //!< VRAM in use (exact in satellite mode; 0 = unknown).
	uint32_t licence_class;               //!< XRT_CONV_LICENCE_* of its first capability.
	char licence_spdx[48];                //!< SPDX of its first capability.
	uint32_t cap_count;                   //!< Capabilities accepted at the last describe.
	uint32_t bad_descriptors;             //!< Capabilities rejected at the last describe (selftest exit 13).
	char descriptor_error[48];            //!< Why the first one was rejected ("" = none).
};

/*!
 * One conversion task row: a provider capability (or a composed pipeline) for
 * one task, with its readiness and where selection put it. One per
 * `system_get_conv_task_row(index)` reply.
 */
struct xrt_status_conv_task
{
	uint32_t provider;                  //!< Index into conv_providers[] (@ref XRT_STATUS_CONV_NO_PROVIDER = none).
	uint32_t task;                      //!< XRT_CONV_TASK_*.
	char task_name[24];                 //!< "views-mono", "depth-video", ...
	uint32_t cap_id;                    //!< Provider cap_id (the depth row's, for a composed row).
	bool composed;                      //!< Depth capability + the reference view-synthesis stage.
	bool selected;                      //!< Selection's choice for this task right now.
	bool cpu_staging;                   //!< A live stream on it fell back to CPU staging (service only).
	uint32_t state;                     //!< XRT_CONV_STATE_*.
	uint32_t reason;                    //!< XRT_CONV_FAIL_* / core-local reason.
	char state_name[16];                //!< "READY", "INSTALLED", ...
	char reason_name[24];               //!< "none", "crash_loop", "policy", ...
	float progress;                     //!< WARMING / DOWNLOADING, [0,1]; < 0 indeterminate.
	uint32_t order;                     //!< Rank in the task's selection order (NOT_ORDERED = dropped).
	char verdict[24];                   //!< "selected", "behind", "fell-through", "drop-licence", ...
	uint32_t lanes;                     //!< XRT_CONV_LANE_* bits.
	uint32_t min_views;                 //!< VIEWS_* limits.
	uint32_t max_views;                 //!< VIEWS_* limits.
	uint32_t licence_class;             //!< XRT_CONV_LICENCE_*.
	char backend[32];                   //!< e.g. "onnxruntime-dml".
	char model_id[XRT_STATUS_NAME_MAX]; //!< For diagnostics only.
	uint32_t typical_latency_ms;        //!< est_latency_ms, or measured (service); 0 = unknown.
	uint32_t streams;                   //!< Live streams bound (service only).
	uint32_t jobs_queued;               //!< Queued jobs (service only).
};

#ifdef __cplusplus
#define XRT_STATUS_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define XRT_STATUS_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif
// One row + its generation + a result code (with slack for alignment) must fit one DIAG reply.
XRT_STATUS_STATIC_ASSERT(sizeof(struct xrt_status_conv_provider) + sizeof(struct xrt_status_generation) + 16 <=
                             XRT_STATUS_CONV_ROW_REPLY_MAX,
                         "xrt_status_conv_provider row no longer fits one DIAG reply");
XRT_STATUS_STATIC_ASSERT(sizeof(struct xrt_status_conv_task) + sizeof(struct xrt_status_generation) + 16 <=
                             XRT_STATUS_CONV_ROW_REPLY_MAX,
                         "xrt_status_conv_task row no longer fits one DIAG reply");

//! Max model packs in @ref xrt_status_snapshot::conv_packs (ADR-053 P4).
#define XRT_STATUS_MAX_CONV_PACKS 8

/*!
 * One model pack of the runtime-owned model store (ADR-053 P4, #2108).
 * Never crosses IPC: the CLI fills it from disk in its own process
 * (`target_conv_store_fill_snapshot`), so a download in any process shows.
 */
struct xrt_status_conv_pack
{
	char pack[XRT_STATUS_NAME_MAX];     //!< Pack id.
	char provider[XRT_STATUS_NAME_MAX]; //!< Consuming provider (informational).
	char state[16];                     //!< "installed" | "downloading" | "partial" | "evicted" | "absent".
	uint32_t models;                    //!< Models in the pack.
	uint32_t models_installed;          //!< ... on disk (stamped, right size).
	uint32_t research_models;           //!< ... whose licence AND is non-commercial.
	uint64_t bytes_total;               //!< Distinct blobs of every model.
	uint64_t bytes_installed;           //!< ... on disk.
	uint64_t download_done;             //!< In-flight bytes (.part sizes).
	uint64_t download_total;            //!< ... of blobs this big.
};

//! Max presenter screens in a @ref xrt_status_workspace (= COMP_WS_MAX_SCREENS).
#define XRT_STATUS_WORKSPACE_MAX_SCREENS 8

//! One screen the workspace presents on (ADR-052).
struct xrt_status_workspace_screen
{
	uint64_t id;  //!< Screen id (matches a `screens[]` row).
	bool primary; //!< The primary presenter (the service window's screen).
	bool woven;   //!< Its presenter has a live display processor.
};

//! Workspace block (phase 2).
struct xrt_status_workspace
{
	bool enabled;                         //!< A workspace session is active.
	char controller[XRT_STATUS_NAME_MAX]; //!< Its controller ("" = none).
	//! ADR-052: the screens the workspace presents on (valid while @ref enabled).
	uint32_t screen_count;
	struct xrt_status_workspace_screen screens[XRT_STATUS_WORKSPACE_MAX_SCREENS];
};

/*!
 * The snapshot (ADR-051 D1).
 */
struct xrt_status_snapshot
{
	uint32_t schema;                                             //!< @ref XRT_STATUS_SCHEMA.
	enum xrt_status_source source;                               //!< Where it was built.
	struct xrt_status_generation generation;                     //!< Generation counters.
	struct xrt_status_runtime runtime;                           //!< Runtime identity.
	uint32_t plugin_count;                                       //!< Valid entries in @ref plugins.
	struct xrt_status_plugin plugins[XRT_STATUS_MAX_PLUGINS];    //!< Registered plug-ins, by ProbeOrder.
	uint32_t screen_count;                                       //!< Valid entries in @ref screens.
	struct xrt_status_screen screens[XRT_STATUS_MAX_SCREENS];    //!< Screens, registry order, default first.
	uint32_t client_count;                                       //!< Valid entries in @ref clients.
	struct xrt_status_client clients[XRT_STATUS_MAX_CLIENTS];    //!< IPC clients (phase 2).
	struct xrt_status_workspace workspace;                       //!< Workspace state (phase 2).
	uint32_t warning_count;                                      //!< Valid entries in @ref warnings.
	struct xrt_status_warning warnings[XRT_STATUS_MAX_WARNINGS]; //!< System-level warnings.
	//! ADR-053: conversion providers (registry + describe; live state from the service).
	uint32_t conv_provider_count;
	struct xrt_status_conv_provider conv_providers[XRT_STATUS_MAX_CONV_PROVIDERS];
	//! ADR-053: per-task rows, grouped by task, each group in selection order.
	uint32_t conv_task_count;
	struct xrt_status_conv_task conv_tasks[XRT_STATUS_MAX_CONV_TASKS];
	//! ADR-053 P4: the model store root ("" = not read) and how it was resolved.
	char conv_model_store_root[256];
	char conv_model_store_source[48];
	//! ADR-053 P4: the registered providers' model packs (read from disk by the CLI).
	uint32_t conv_pack_count;
	struct xrt_status_conv_pack conv_packs[XRT_STATUS_MAX_CONV_PACKS];
	/*!
	 * ADR-034 Amendment 5 decision 6: each attached client's LIVE input-role
	 * holder per family (and the preference that put it there), next to the
	 * stored pins the Dashboard edits. Service only (fetched one row per DIAG
	 * reply, `system_get_input_holders`); 0 headless. Diagnostics: current at
	 * fetch time, not covered by @ref generation.
	 */
	uint32_t input_holder_count;
	struct xrt_input_client_holders input_holders[XRT_STATUS_MAX_CLIENTS];
};

/*!
 * The snapshot minus its per-screen and per-client rows — the piece every
 * `system_get_status_snapshot` reply carries (ADR-051 D3, phase 2). The rows
 * cross separately, by index (screens) and by id (@ref client_ids); a reader
 * reassembles them with `u_status_snapshot_set_head` and checks that every
 * piece carries the same @ref generation.
 */
struct xrt_status_head
{
	uint32_t schema;                                             //!< @ref XRT_STATUS_SCHEMA.
	enum xrt_status_source source;                               //!< Where it was built.
	struct xrt_status_generation generation;                     //!< Generation the pieces belong to.
	struct xrt_status_runtime runtime;                           //!< Runtime identity.
	uint32_t plugin_count;                                       //!< Valid entries in @ref plugins.
	struct xrt_status_plugin plugins[XRT_STATUS_MAX_PLUGINS];    //!< Registered plug-ins.
	uint32_t screen_count;                                       //!< Screens to fetch by index.
	uint32_t client_count;                                       //!< Valid entries in @ref client_ids.
	uint32_t client_ids[XRT_STATUS_MAX_CLIENTS];                 //!< Clients, snapshot order.
	struct xrt_status_workspace workspace;                       //!< Workspace state.
	uint32_t warning_count;                                      //!< Valid entries in @ref warnings.
	struct xrt_status_warning warnings[XRT_STATUS_MAX_WARNINGS]; //!< System-level warnings.
	//! ADR-053: conversion rows to fetch BY INDEX (`system_get_conv_provider_row` /
	//! `system_get_conv_task_row`), so the head reply does not grow by ~20 KB.
	uint32_t conv_provider_count;
	uint32_t conv_task_count;
};

//! Live DPs the service reports into one build (one primary per client + its segment DPs + its own).
#define XRT_STATUS_MAX_LIVE_DPS 64

/*!
 * One display processor the service has bound right now, as the service
 * builder (`target_status_snapshot_build_service`) maps it onto a screen.
 */
struct xrt_status_live_dp
{
	//! Screen the DP weaves (0 = the system-default screen, which the builder resolves).
	uint64_t screen_id;
	struct xrt_status_dp dp;  //!< Client, API, kind, backend.
	bool answered;            //!< The DP answered the eye query.
	bool is_tracking;         //!< It reports a tracked viewer (iff @ref answered).
	uint32_t not_tracking_ms; //!< How long it has answered NOT tracking (0 when tracking).
};

/*!
 * What only the running service knows (ADR-051 D3), gathered by the IPC
 * server and merged by `target_status_snapshot_build_service` into the rows
 * the headless builder already fills. Plain data, no pointers.
 */
struct xrt_status_live
{
	struct xrt_status_generation generation;                  //!< The service's counters.
	uint32_t client_count;                                    //!< Valid entries in @ref clients.
	struct xrt_status_client clients[XRT_STATUS_MAX_CLIENTS]; //!< Fully filled client rows.
	uint32_t dp_count;                                        //!< Valid entries in @ref dps.
	struct xrt_status_live_dp dps[XRT_STATUS_MAX_LIVE_DPS];   //!< Bound DPs.
	struct xrt_status_mode mode;                              //!< The head's current rendering mode.
	struct xrt_status_workspace workspace;                    //!< Workspace state.
	//! ADR-053: the conversion core's rows as the service's conversion status
	//! source reports them (`ipc_server_set_conv_status_source`; none = 0 rows).
	uint32_t conv_provider_count;
	struct xrt_status_conv_provider conv_providers[XRT_STATUS_MAX_CONV_PROVIDERS];
	uint32_t conv_task_count;
	struct xrt_status_conv_task conv_tasks[XRT_STATUS_MAX_CONV_TASKS];
};

#ifdef __cplusplus
}
#endif
