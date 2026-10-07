// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Resolve the desktop rect and stable device name of the monitor a
 *         given virtual-desktop point falls on.
 * @ingroup aux_os
 *
 * This is deliberately narrower than @ref os_display_edid.h: no SetupAPI, no
 * EDID, no registry. Just `MonitorFromPoint` + `GetMonitorInfoW` on Windows,
 * so it cannot fail the way the EDID correlation can. The runtime uses it to
 * turn the vendor plug-in's panel origin
 * (`xrt_system_compositor_info::display_screen_left/top`) into the full
 * monitor geometry that `XR_DXR_display_info` publishes to apps, so a client
 * can place its window on the 3D panel instead of the primary monitor.
 *
 * See runtime issue #1301 and docs/specs/extensions/XR_DXR_display_info.md.
 *
 * Implemented on Windows (`MonitorFromPoint`/`GetMonitorInfoW`), macOS
 * (CoreGraphics `CGDisplayBounds` + display UUID) and desktop Linux (X11
 * RandR 1.5 `XRRGetMonitors`, dlopen'd). Android and everything else get the
 * stub, which reports failure so the caller publishes "unknown".
 *
 * COORDINATE SPACE IS PER-PLATFORM, and always the one the OS places windows
 * in: physical pixels on Windows (per-monitor-v2, pinned during the query),
 * POINTS on macOS (the backing store is 2x on Retina), device pixels on X11.
 * All three are top-down with the origin at the primary/main display's
 * top-left, and all three allow negative offsets.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Size of the device-name buffer, in bytes, including the NUL.
 *
 * Windows `CCHDEVICENAME` is 32 wide chars (`\\.\DISPLAY1`), which is well
 * inside this; the headroom is for the platform identifiers that land later
 * (a macOS display UUID is 37 bytes).
 */
#define OS_DISPLAY_DEVICE_NAME_SIZE 128

/*!
 * Where a monitor's device mode (@ref os_display_desktop_info::native_width)
 * came from.
 */
enum os_display_native_source
{
	OS_DISPLAY_NATIVE_SOURCE_NONE = 0,
	//! The compositor's own current mode (Mutter DisplayConfig).
	OS_DISPLAY_NATIVE_SOURCE_COMPOSITOR,
	//! The kernel's mode list (DRM sysfs): the X11 size when it is one of
	//! the modes, else the preferred one.
	OS_DISPLAY_NATIVE_SOURCE_DRM,
};

/*!
 * Desktop geometry and identity of one monitor.
 */
struct os_display_desktop_info
{
	//! Monitor left edge in virtual-desktop coordinates. Signed: monitors
	//! left of the primary are negative.
	int32_t left;

	//! Monitor top edge in virtual-desktop coordinates. Signed: monitors
	//! above the primary are negative.
	int32_t top;

	//! Monitor width in physical pixels, in the mode it is running now.
	//! This is NOT necessarily the panel's native resolution.
	uint32_t width;

	//! Monitor height in physical pixels, in the mode it is running now.
	uint32_t height;

	//! True if this is the primary monitor (the one the desktop origin is on).
	bool is_primary;

	//! Stable OS device name, NUL-terminated UTF-8, for re-resolving this
	//! monitor after a hotplug or rearrangement. Windows: the GDI name, e.g.
	//! `\\.\DISPLAY1`. macOS: the display UUID (the CGDirectDisplayID
	//! itself is NOT stable across reboots). Linux/X11: the RandR output name,
	//! e.g. `HDMI-1`. Empty when unknown.
	char device_name[OS_DISPLAY_DEVICE_NAME_SIZE];

	/*!
	 * @name The same monitor measured in the space PLUG-IN dimensions live in
	 *
	 * These exist for exactly ONE job: comparing against the panel dimensions a
	 * display-processor plug-in reports. Comparing those against @ref width /
	 * @ref height would disagree for reasons that have nothing to do with
	 * whether the monitors match, so the comparison needs both sides in one
	 * space. Per platform, the mismatch has a different cause:
	 *
	 * - Windows: a plug-in is a DLL and inherits its host's DPI awareness, so
	 *   under a DPI-unaware host it reports VIRTUALISED dims (a 3840x2160 panel
	 *   at 250% reports 1536x864) while @ref width stays physical. These are
	 *   then the monitor as the unaware caller sees it.
	 * - macOS: @ref width is in points, but a plug-in reports BACKING PIXELS,
	 *   so on Retina the two differ by the 2x scale. These are the pixel dims.
	 * - X11: no scaling layer, so these equal @ref width / @ref height.
	 *
	 * NEVER publish these or place a window with them. Use @ref width /
	 * @ref height for anything that leaves the process.
	 * @{
	 */
	uint32_t width_in_caller_dpi;
	uint32_t height_in_caller_dpi;
	/*! @} */

	/*!
	 * @name Physical panel size as the monitor's EDID reports it, in mm
	 *
	 * The TIE-BREAKER in panel matching (see @ref
	 * os_display_desktop_info_for_panel): pixel dimensions alone cannot tell
	 * two same-resolution monitors apart, but a vendor plug-in also reports
	 * the panel's physical size and EDID gives one per monitor. Only ever
	 * used to choose between monitors that ALREADY matched on pixels —
	 * physical size on its own is weak identity (every 15.6" laptop panel is
	 * about 344x194 mm). 0 when the platform does not report it, and EDID mm
	 * values are rounded, so compare with a tolerance, never for equality.
	 * @{
	 */
	uint32_t physical_width_mm;
	uint32_t physical_height_mm;
	/*! @} */

	/*!
	 * @name The DEVICE mode behind this monitor, and its compositor scale (#1831)
	 *
	 * @ref width / @ref height are in the space the OS places windows in. On
	 * X11 under XWayland that is NOT the hardware: Mutter scales the X screen
	 * by one global integer (the ceiling of the largest monitor scale), so a
	 * 3840x2160 output at 150 % is a 5120x2880 X11 rect. These fields carry
	 * what the connector is really running, keyed by the connector name, so
	 * the panel can be identified by its hardware mode at any scale, and so
	 * "is an X11 window on this output 1:1?" becomes a comparison instead of
	 * a guess.
	 *
	 * Desktop Linux only (Mutter DisplayConfig, else DRM sysfs). 0 = unknown,
	 * which every consumer must treat as "keep current behaviour".
	 * @{
	 */
	uint32_t native_width;
	uint32_t native_height;

	//! The compositor's scale for this output (e.g. 1.5). 0 = unknown.
	double scale;

	//! Where @ref native_width / @ref native_height came from.
	enum os_display_native_source native_source;

	//! Refresh of that device mode in milli-Hz, when the source reports it
	//! (Mutter DisplayConfig does; DRM sysfs does not). 0 = unknown.
	uint32_t native_refresh_mhz;
	/*! @} */
};

/*!
 * Upper bound on monitors @ref os_display_desktop_enumerate will report.
 *
 * Generous for a desktop; a rig with more monitors than this gets the first
 * ones enumerated, which only ever degrades panel matching to the fallback.
 */
#define OS_DISPLAY_DESKTOP_MAX_MONITORS 16

/*!
 * Which rule picked the monitor, so a caller can say so out loud instead of
 * presenting a guess and a certainty identically. See @ref
 * os_display_desktop_info_for_panel for what each one means.
 */
enum os_display_desktop_rule
{
	//! Nothing resolved — no implementation, or the query failed.
	OS_DISPLAY_DESKTOP_RULE_UNRESOLVED = 0,

	//! The plug-in supplied a desktop position and we resolved the monitor
	//! under it. The strongest evidence available, and the ONLY rule the
	//! Windows path has ever used.
	OS_DISPLAY_DESKTOP_RULE_ORIGIN,

	//! No position, but exactly one monitor's mode equals the panel's native
	//! pixel size (ties broken on physical size). 1:1, so weaving is possible.
	OS_DISPLAY_DESKTOP_RULE_PIXEL_MATCH,

	//! Nothing matched — the monitor at the desktop origin. A placeable rect,
	//! not evidence that we found the panel.
	OS_DISPLAY_DESKTOP_RULE_PRIMARY_FALLBACK,

	//! No position, but exactly one monitor's CONNECTOR is running the
	//! panel's native mode (@ref os_display_desktop_info::native_width), ties
	//! broken on physical size (#1831). Identity by hardware mode, so it
	//! holds at any desktop scale — but it says nothing about whether the
	//! window-placement space is 1:1 there; @ref
	//! os_display_desktop_info::width says that.
	OS_DISPLAY_DESKTOP_RULE_CONNECTOR_MODE,
};

/*!
 * Everything a display-processor plug-in can tell us about the panel, which is
 * the whole input to monitor selection.
 */
struct os_display_panel_hint
{
	//! Panel origin in virtual-desktop coordinates; (0, 0) = "no preference"
	//! (`xrt_plugin_display_info::display_screen_left/top`).
	int32_t screen_left;
	int32_t screen_top;

	//! Native panel resolution in pixels, in the CALLER's DPI space (that is
	//! the space a plug-in reports in). 0 = unknown.
	uint32_t pixel_width;
	uint32_t pixel_height;

	//! Physical panel size in metres. 0 = unknown.
	float width_m;
	float height_m;
};

/*!
 * Outcome of monitor selection beyond the rect itself.
 */
struct os_display_panel_match
{
	//! Which rule fired.
	enum os_display_desktop_rule rule;

	//! Number of monitors the size rules had to choose between, when more
	//! than one matched. 0 or 1 = unambiguous. Callers log this once.
	uint32_t candidate_count;

	//! How many monitors the platform could enumerate at all. 0 means the
	//! platform has no enumeration, so only the origin/primary rules ran.
	uint32_t monitor_count;
};

/*!
 * Enumerate every active monitor.
 *
 * Implemented on desktop Linux (RandR 1.5). Windows, macOS and the stub return
 * 0, which makes @ref os_display_desktop_info_for_panel fall through to the
 * origin/primary rules — i.e. exactly the behaviour those platforms had before
 * size matching existed.
 *
 * @param[out] out_infos Array of at least @p max_infos entries.
 * @param max_infos Capacity of @p out_infos.
 * @return the number of monitors written, 0 on any failure.
 */
uint32_t
os_display_desktop_enumerate(struct os_display_desktop_info *out_infos, uint32_t max_infos);

/*!
 * Pick the monitor the 3D panel is on, honestly.
 *
 * A 3D panel is a SECOND monitor on essentially every real deployment, so
 * "resolve the point the plug-in gave us, else the primary" silently lands on
 * the laptop screen whenever the plug-in cannot report a position — which under
 * a Wayland compositor is always, because each output's CRTC framebuffer rect
 * starts at (0, 0) and LeiaSR's `srDisplayGetLocation()` matches panels by EDID
 * physical size alone. The pixel-size rule below is what is left when no
 * identity and no position exist.
 *
 * Rules, in order:
 *
 * 1. @ref OS_DISPLAY_DESKTOP_RULE_ORIGIN — the hint carries a non-(0,0)
 *    position. Trust it; this is the Windows path and it is unchanged.
 * 2. @ref OS_DISPLAY_DESKTOP_RULE_CONNECTOR_MODE — a monitor's connector is
 *    running the panel's native mode (desktop Linux; #1831). Scale-proof.
 * 3. @ref OS_DISPLAY_DESKTOP_RULE_PIXEL_MATCH — a monitor whose device mode is
 *    unknown has a rect equal to the panel's native pixel size.
 *    For both size rules, several candidates: pick the one whose EDID
 *    physical size is closest to the panel's, else the non-primary one, and
 *    report the ambiguity.
 * 4. @ref OS_DISPLAY_DESKTOP_RULE_PRIMARY_FALLBACK — today's behaviour.
 *
 * There is deliberately NO physical-size-only rule. A monitor that matches the
 * panel in millimetres but not in pixels is not 1:1, so it can never be
 * panel-confirmed and can never carry a phase-correct weave; and physical size
 * on its own is weak identity, so acting on it moves windows to monitors that
 * merely resemble the panel.
 *
 * @param hint What the plug-in reported. NULL is treated as all-unknown.
 * @param[out] out_info Receives the monitor geometry. Zeroed on failure.
 * @param[out] out_match Optional; receives the rule that fired and the
 *        ambiguity counts, so the caller can log or print them.
 *
 * @return true when a monitor was resolved.
 */
bool
os_display_desktop_info_for_panel(const struct os_display_panel_hint *hint,
                                  struct os_display_desktop_info *out_info,
                                  struct os_display_panel_match *out_match);

/*!
 * The policy half of @ref os_display_desktop_info_for_panel: the size rules
 * over an already-enumerated monitor list, with no I/O. Exposed so the rules
 * can be pinned by host tests.
 *
 * Runs the connector-mode rule, then the X11-size rule; never the origin or
 * primary rules, which need a point lookup.
 *
 * @return the index of the picked monitor in @p mons, or -1 when no size rule
 *         fired (the caller then falls back). @p out_match receives the rule,
 *         candidate count and @p count as the monitor count.
 */
int32_t
os_display_desktop_select_by_size(const struct os_display_desktop_info *mons,
                                  uint32_t count,
                                  const struct os_display_panel_hint *hint,
                                  struct os_display_panel_match *out_match);

/*!
 * Is a window placed in this monitor's coordinate space presented to the
 * hardware 1:1? Tri-state, never a guess (#1831):
 *
 * - 1: the rect equals the device mode — the space is device pixels.
 * - 0: both are known and differ — the compositor resamples whatever is
 *   drawn there (X11 under XWayland on an output whose scale is not the X
 *   screen's global scale).
 * - -1: the device mode is unknown.
 */
static inline int
os_display_desktop_info_is_1to1(const struct os_display_desktop_info *m)
{
	if (m == NULL || m->native_width == 0 || m->native_height == 0 || m->width == 0 || m->height == 0) {
		return -1;
	}
	return (m->width == m->native_width && m->height == m->native_height) ? 1 : 0;
}

/*!
 * Short human-readable name of a selection rule ("origin", "connector match",
 * "size match", "primary fallback"), for logs and `displayxr-cli
 * info`. Never NULL.
 */
const char *
os_display_desktop_rule_str(enum os_display_desktop_rule rule);

/*!
 * Resolve the monitor containing a virtual-desktop point.
 *
 * (0, 0) resolves to the primary monitor, which is both the "panel is at the
 * desktop origin" and the "plug-in expressed no preference" reading — they
 * want the same answer, so the caller need not distinguish them.
 *
 * A point that falls in no monitor (a gap in a ragged arrangement, or a stale
 * origin after a hotplug) resolves to the nearest monitor rather than failing,
 * so the caller always gets a placeable rect.
 *
 * @param x Point X in virtual-desktop coordinates.
 * @param y Point Y in virtual-desktop coordinates.
 * @param[out] out_info Receives the monitor geometry. Zeroed on failure.
 *
 * @return true on success. false when the platform has no implementation, or
 * when the Win32 query failed — callers should treat the geometry as unknown
 * and publish zeros rather than guessing.
 */
bool
os_display_desktop_info_at(int32_t x, int32_t y, struct os_display_desktop_info *out_info);

#ifdef __cplusplus
}
#endif
