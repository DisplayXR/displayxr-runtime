// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  DisplayXR spatial shell — launches service, launches apps, monitors.
 *
 * Usage:
 *   displayxr-shell.exe [--pose x,y,z,w,h] app1.exe [--pose x,y,z,w,h] app2.exe ...
 *
 * - Auto-starts displayxr-service --shell if not running
 * - Launches each app with DISPLAYXR_WORKSPACE_SESSION=1 and XR_RUNTIME_JSON set
 * - Optionally assigns per-app window pose via --pose x,y,z,width_m,height_m
 * - Monitors client connect/disconnect until Ctrl+C
 *
 * @ingroup ipc
 */

#include "shell_app_scan.h"
#include "shell_openxr.h"
#include "shell_chrome.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <math.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#include <tlhelp32.h> // For process enumeration (service PID lookup)
#include <shellapi.h> // For Shell_NotifyIcon (system tray); PrivateExtractIconsA (Browse PE-icon extract)
#include <commdlg.h>  // For GetOpenFileNameA (Phase 5.14 Browse tile)

#include "displayxr_shell_resource.h" // IDI_DISPLAYXR_SHELL — embedded tray/PE icon

// PNG encoding for the Browse-for-app PE icon → tile-icon conversion. Defined
// here (not in shell_app_scan.c) because main.c owns the Browse flow and this
// keeps the dependency in one TU. STB_IMAGE_WRITE_IMPLEMENTATION must be
// defined in exactly one source file per binary; the displayxr-service binary
// has its own definition in comp_d3d11_service.cpp.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#else
#include <unistd.h>
#endif


#define P(...) fprintf(stdout, __VA_ARGS__)
#define PE(...) fprintf(stderr, __VA_ARGS__)

#ifdef _WIN32
#define HOTKEY_TOGGLE 1
#define HOTKEY_LAUNCH 2
#define HOTKEY_CAPTURE 3
#define WM_TRAYICON (WM_USER + 1)
// Phase 2.K B-merged: only Exit. There's no longer a separate "deactivate"
// path — Ctrl+Space and tray Exit both kill all apps and shut down the shell.
// The orchestrator's keyboard hook respawns the shell on the next Ctrl+Space.
#define TRAY_CMD_EXIT 2002
#define POLL_INTERVAL_MS 500
#endif

#define MAX_APPS 8
#define MAX_CAPTURES 24

static volatile int g_running = 1;
#ifdef _WIN32
static bool g_shell_active = false;
static bool g_service_managed = false; // When true, skip Ctrl+Space hotkey (service owns it)
static bool g_launcher_visible = false; // Phase 5.7: spatial launcher panel toggle (Ctrl+L)
static HWND g_msg_hwnd = NULL;
#endif

// Phase 2.I: file-scope OpenXR state. Set in main() after shell_openxr_init
// succeeds. Helpers throughout the shell dispatch through this pointer; once
// non-NULL, the corresponding ipc_call_workspace_* / launcher_* / system_*
// sites have been migrated to the public extension surface.
static struct shell_openxr_state *g_xr = NULL;

// Phase 2.C: chrome render module. Created after g_xr in main(); owns one
// chrome swapchain per workspace client (filtered by PID — the shell's own
// session does not get chrome). NULL until the shell is fully initialized.
static struct shell_chrome *g_chrome = NULL;

// Cap on locally-buffered enumerate. The current wire IPC_MAX_CLIENTS is
// 8 server-side; 16 keeps headroom for a future raise without touching
// the shell.
#define SHELL_MAX_CLIENTS 16

// Drain the workspace client id list from the runtime. Returns the number
// of ids written; 0 on error or empty.
static uint32_t
shell_enumerate_clients(XrWorkspaceClientId *out_ids, uint32_t cap)
{
	if (g_xr == NULL || out_ids == NULL || cap == 0) {
		return 0;
	}
	uint32_t count = 0;
	if (g_xr->enumerate_clients(g_xr->session, cap, &count, out_ids) != XR_SUCCESS) {
		return 0;
	}
	return count;
}

// Fetch metadata for one client. Returns true on success; @p info is
// initialized with the required type + next NULL.
static bool
shell_get_client_info(XrWorkspaceClientId id, XrWorkspaceClientInfoEXT *info)
{
	if (g_xr == NULL || info == NULL) {
		return false;
	}
	memset(info, 0, sizeof(*info));
	info->type = XR_TYPE_WORKSPACE_CLIENT_INFO_EXT;
	return g_xr->get_client_info(g_xr->session, id, info) == XR_SUCCESS;
}

// ---------- Phase 2.G: layout presets owned by the controller ----------
//
// The pose math here is identical to what the runtime owned in
// comp_d3d11_service.cpp before Phase 2.G. The runtime no longer intercepts
// Ctrl+1..3; those keys flow through xrEnumerateWorkspaceInputEventsEXT and
// the controller dispatches them. Per-client poses are pushed via
// xrSetWorkspaceClientWindowPoseEXT — the only public layout primitive.
//
// Display dimensions are pulled from XR_EXT_display_info during shell init
// (see shell_openxr.cpp). Layout-preset math reads them through g_xr so we
// scale poses to the actual physical display rather than guessing.
//
// Carousel radius scales with display width so the ring fits any panel:
// 0.18 of display width gives a sensible carousel on both LP-3D
// (~0.06 m radius on 0.344 m) and the larger 0.700 m fallback.
#define SHELL_PI_F 3.14159265358979323846f

// Forward declarations: the Phase 2.K animation framework + carousel state
// machine come before the static layout helpers but reference some of them.
static void
shell_compute_carousel_pose(int n, int idx, float angle_offset,
                            XrPosef *out_pose,
                            float *out_w, float *out_h);

// Phase 2.K: focused-client id seen on the most-recent FOCUS_CHANGED event
// from the runtime. 0 = no focused client. File-scope so the carousel state
// machine can read it without depending on drain-internal storage.
static int s_focused_client_id = 0;

// ---------- Phase 2.K: shell-side animation framework ----------
//
// Replicates the deleted runtime slot_animate_to / slot_animate_tick — but
// controller-side now, per the "controllers own all motion policy" North
// Star (memory: feedback_controllers_own_motion). Each entry tracks a
// per-client interpolation from a snapshot of the current pose to a target
// pose+size over a configurable duration with an ease-out cubic curve. A
// FRAME_TICK from the runtime (or the main-loop fallback timer) drives the
// shell_slot_anim_tick() pass that lerps and re-pushes set_pose.

#define SHELL_ANIM_DURATION_NS (300ULL * 1000000ULL) // 300 ms default
#define SHELL_ANIM_MAX 16                            // ≥ SHELL_MAX_CLIENTS

struct shell_slot_anim
{
	bool                 active;
	XrWorkspaceClientId  id;
	XrPosef              start_pose;
	XrPosef              target_pose;
	float                start_w;
	float                start_h;
	float                target_w;
	float                target_h;
	uint64_t             start_ns;
	uint64_t             duration_ns;
};

static struct shell_slot_anim s_anims[SHELL_ANIM_MAX];

static uint64_t
shell_now_ns(void)
{
#ifdef _WIN32
	// QueryPerformanceCounter gives the highest-resolution monotonic source
	// on Windows; convert ticks to ns. Cached frequency keeps the call cheap.
	static LARGE_INTEGER s_freq = {0};
	if (s_freq.QuadPart == 0) {
		QueryPerformanceFrequency(&s_freq);
	}
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	// (ticks * 1e9) / freq, in 64-bit safe order.
	return (uint64_t)((double)t.QuadPart * 1e9 / (double)s_freq.QuadPart);
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

static float
shell_ease_out_cubic(float t)
{
	if (t <= 0.0f) return 0.0f;
	if (t >= 1.0f) return 1.0f;
	float f = 1.0f - t;
	return 1.0f - f * f * f;
}

static void
shell_lerp_pose(const XrPosef *a, const XrPosef *b, float t, XrPosef *out)
{
	out->position.x = a->position.x + (b->position.x - a->position.x) * t;
	out->position.y = a->position.y + (b->position.y - a->position.y) * t;
	out->position.z = a->position.z + (b->position.z - a->position.z) * t;
	// nlerp + normalize is plenty for the small angles + short durations
	// our presets traverse (carousel auto-rotation excluded — that path
	// drives set_pose directly without seeding the animation). Pick the
	// short-arc by flipping b if dot < 0.
	float dot = a->orientation.x * b->orientation.x + a->orientation.y * b->orientation.y +
	            a->orientation.z * b->orientation.z + a->orientation.w * b->orientation.w;
	float bx = b->orientation.x, by = b->orientation.y, bz = b->orientation.z, bw = b->orientation.w;
	if (dot < 0.0f) {
		bx = -bx; by = -by; bz = -bz; bw = -bw;
	}
	float qx = a->orientation.x + (bx - a->orientation.x) * t;
	float qy = a->orientation.y + (by - a->orientation.y) * t;
	float qz = a->orientation.z + (bz - a->orientation.z) * t;
	float qw = a->orientation.w + (bw - a->orientation.w) * t;
	float n = sqrtf(qx * qx + qy * qy + qz * qz + qw * qw);
	if (n > 0.0f) {
		float inv = 1.0f / n;
		qx *= inv; qy *= inv; qz *= inv; qw *= inv;
	} else {
		qx = 0.0f; qy = 0.0f; qz = 0.0f; qw = 1.0f;
	}
	out->orientation.x = qx;
	out->orientation.y = qy;
	out->orientation.z = qz;
	out->orientation.w = qw;
}

// Find the animation slot for `id`; returns NULL if not present.
static struct shell_slot_anim *
shell_slot_anim_find(XrWorkspaceClientId id)
{
	for (int i = 0; i < SHELL_ANIM_MAX; i++) {
		if (s_anims[i].active && s_anims[i].id == id) {
			return &s_anims[i];
		}
	}
	return NULL;
}

// Seed an animation toward `target_pose` / `target_w` × `target_h` over
// `duration_ns`. Snapshots the current pose via xrGetWorkspaceClientWindowPoseEXT
// to use as the start of the interpolation. Reuses an existing entry if one
// is already animating this client so preset switches mid-animation pick up
// from the current interpolated pose.
//
// Returns true if the seed succeeded (the runtime knew the slot well enough
// to read its pose). Returns false if get_pose failed (typical connect-time
// race: the IPC client_id is enumerable but its xrt_compositor isn't bound
// yet) — caller should retry on the next poll iteration. We deliberately do
// NOT fall back to "target=target=animation no-op" because every set_pose
// during such an animation also fails XR_ERROR_VALIDATION, and 300 ms can
// elapse before binding completes — leaving the cube at the runtime's
// entry-animation final pose.
static bool
shell_slot_anim_seed(XrWorkspaceClientId id,
                     const XrPosef *target_pose,
                     float target_w,
                     float target_h,
                     uint64_t duration_ns)
{
	if (g_xr == NULL || id == XR_NULL_WORKSPACE_CLIENT_ID || target_pose == NULL) {
		return false;
	}

	XrPosef cur_pose;
	float cur_w = target_w, cur_h = target_h;
	if (g_xr->get_pose == NULL) {
		// get_pose unavailable (older runtime) — no-glide fallback: the
		// caller can still see "true" and the carousel/preset will just
		// snap on the first set_pose tick.
		cur_pose = *target_pose;
	} else {
		XrResult gr = g_xr->get_pose(g_xr->session, id, &cur_pose, &cur_w, &cur_h);
		if (gr != XR_SUCCESS) {
			// Slot not yet bound. Caller retries next tick.
			return false;
		}
	}

	struct shell_slot_anim *a = shell_slot_anim_find(id);
	if (a == NULL) {
		for (int i = 0; i < SHELL_ANIM_MAX; i++) {
			if (!s_anims[i].active) {
				a = &s_anims[i];
				break;
			}
		}
	}
	if (a == NULL) {
		// Should not happen with SHELL_ANIM_MAX ≥ SHELL_MAX_CLIENTS.
		PE("shell_slot_anim_seed: no free animation slot for client %u\n", id);
		return false;
	}

	a->active = true;
	a->id = id;
	a->start_pose = cur_pose;
	a->target_pose = *target_pose;
	a->start_w = cur_w;
	a->start_h = cur_h;
	a->target_w = target_w;
	a->target_h = target_h;
	a->start_ns = shell_now_ns();
	a->duration_ns = duration_ns;
	return true;
}

// Tick every active animation: interpolate, push set_pose, mark inactive on
// completion. Returns the number of animations still active after the tick
// (so the caller can pick a tight or relaxed poll cadence).
static int
shell_slot_anim_tick(void)
{
	if (g_xr == NULL || g_xr->set_pose == NULL) {
		return 0;
	}
	uint64_t now = shell_now_ns();
	int still_active = 0;
	for (int i = 0; i < SHELL_ANIM_MAX; i++) {
		struct shell_slot_anim *a = &s_anims[i];
		if (!a->active) continue;
		if (a->duration_ns == 0) {
			// "Drive" mode — push target immediately, deactivate.
			g_xr->set_pose(g_xr->session, a->id, &a->target_pose, a->target_w, a->target_h);
			a->active = false;
			continue;
		}
		uint64_t elapsed = now - a->start_ns;
		float t = (float)((double)elapsed / (double)a->duration_ns);
		bool done = false;
		if (t >= 1.0f) {
			t = 1.0f;
			done = true;
		}
		float eased = shell_ease_out_cubic(t);
		XrPosef p;
		shell_lerp_pose(&a->start_pose, &a->target_pose, eased, &p);
		float w = a->start_w + (a->target_w - a->start_w) * eased;
		float h = a->start_h + (a->target_h - a->start_h) * eased;
		g_xr->set_pose(g_xr->session, a->id, &p, w, h);
		if (done) {
			a->active = false;
		} else {
			still_active++;
		}
	}
	return still_active;
}

static int
shell_slot_anim_active_count(void)
{
	int n = 0;
	for (int i = 0; i < SHELL_ANIM_MAX; i++) {
		if (s_anims[i].active) n++;
	}
	return n;
}

// Read out the target window dims for an in-flight slot animation. Returns
// true if an animation for `id` is currently active and writes target_w/h.
// Used by the chrome create path so the very first chrome layout is sized
// for the animation's destination rather than the mid-glide current dims —
// otherwise the chrome stays at stale (often display-sized) dims because
// `shell_chrome_on_window_resized` was a no-op when the preset apply ran
// before the chrome existed.
static bool
shell_slot_anim_get_target(XrWorkspaceClientId id, float *target_w, float *target_h)
{
	for (int i = 0; i < SHELL_ANIM_MAX; i++) {
		if (s_anims[i].active && s_anims[i].id == id) {
			if (target_w) *target_w = s_anims[i].target_w;
			if (target_h) *target_h = s_anims[i].target_h;
			return true;
		}
	}
	return false;
}

// Cancel any pending animation for `id`. Used when a client disconnects so we
// don't keep pushing set_pose on a dead slot.
static void
shell_slot_anim_cancel(XrWorkspaceClientId id)
{
	for (int i = 0; i < SHELL_ANIM_MAX; i++) {
		if (s_anims[i].active && s_anims[i].id == id) {
			s_anims[i].active = false;
		}
	}
}


// ---------- Phase 2.K: shell-side carousel state machine ----------
//
// Replicates the deleted runtime carousel — auto-rotation, drag-to-rotate,
// scroll-radius, TAB-snap, momentum — controller-side. Drives per-client
// poses every tick by calling shell_compute_carousel_pose with the current
// `angle_offset` and `radius_scale`.
//
// Inputs (consumed by shell_drain_input_events):
//  - POINTER LMB down on a title bar → start drag at cursor X
//  - MOTION (LMB held) → angle_offset += dx * sensitivity
//  - POINTER LMB up → end drag, latch momentum from recent dx samples
//  - SCROLL → radius_scale ×= (1 + delta * 0.05) clamped to [0.5, 1.5]
//  - KEY VK_TAB (no Ctrl) → snap focused client to front, animate over
//    300 ms, pause auto-rotation 5 s.
//
// Per-frame work happens in shell_carousel_tick() — called from the main
// loop only when `s_active_preset == carousel`. Auto-rotation runs at
// ~10°/s; momentum decays exponentially with a half-life of ~0.5 s.

#define SHELL_CAROUSEL_AUTO_RATE_RPS         (10.0f * SHELL_PI_F / 180.0f)  // ~10°/s
#define SHELL_CAROUSEL_DRAG_SENSITIVITY_RPP  (2.0f * SHELL_PI_F / 1000.0f)  // 1000 px = full turn
#define SHELL_CAROUSEL_MOMENTUM_DECAY_PER_S  2.0f                          // angular vel halves in ~0.35 s
#define SHELL_CAROUSEL_RADIUS_MIN            0.5f
#define SHELL_CAROUSEL_RADIUS_MAX            1.5f
#define SHELL_CAROUSEL_SNAP_PAUSE_NS         (5ULL * 1000000000ULL)
#define SHELL_CAROUSEL_ANGLE_ANIM_NS         SHELL_ANIM_DURATION_NS

static const char *s_active_preset = NULL; // last preset applied, NULL = none
static bool        s_pointer_capture_active = false;

struct shell_carousel_state
{
	float    angle_offset;        // current ring rotation (rad)
	float    angular_velocity;    // rad/sec, drives momentum
	float    radius_scale;        // multiplier on shell_compute_carousel_pose's base radius
	bool     dragging;
	int32_t  drag_last_cursor_x;
	uint64_t last_tick_ns;
	uint64_t pause_until_ns;       // auto-rotation paused until this ns
	uint64_t drag_last_dx_ns;      // for momentum sampling
	float    drag_last_dx_per_s;   // last computed angular velocity sample

	// TAB-snap interpolation of angle_offset itself.
	bool     angle_anim_active;
	float    angle_anim_start;
	float    angle_anim_target;
	uint64_t angle_anim_start_ns;
	uint64_t angle_anim_duration_ns;
};
static struct shell_carousel_state s_car = {
    .angle_offset = 0.0f,
    .angular_velocity = 0.0f,
    .radius_scale = 1.0f,
    .dragging = false,
    .drag_last_cursor_x = 0,
    .last_tick_ns = 0,
    .pause_until_ns = 0,
    .drag_last_dx_ns = 0,
    .drag_last_dx_per_s = 0.0f,
    .angle_anim_active = false,
    .angle_anim_start = 0.0f,
    .angle_anim_target = 0.0f,
    .angle_anim_start_ns = 0,
    .angle_anim_duration_ns = 0,
};

// Wrap angle to (-π, π] so accumulated drift doesn't lose precision.
static float
shell_wrap_angle(float a)
{
	while (a >  SHELL_PI_F) a -= 2.0f * SHELL_PI_F;
	while (a <= -SHELL_PI_F) a += 2.0f * SHELL_PI_F;
	return a;
}

// Push a fresh per-client pose for every connected app client based on the
// current carousel state. Called every tick while carousel is active. Does
// NOT go through the slot animation framework — carousel positions are
// computed directly (controller is the source of truth for the angle).
static void
shell_carousel_redrive_poses(void)
{
	if (g_xr == NULL || g_xr->set_pose == NULL) return;
	XrWorkspaceClientId raw_ids[SHELL_MAX_CLIENTS];
	uint32_t raw_n = shell_enumerate_clients(raw_ids, SHELL_MAX_CLIENTS);
	if (raw_n == 0) return;

#ifdef _WIN32
	uint64_t self_pid = (uint64_t)GetCurrentProcessId();
#else
	uint64_t self_pid = (uint64_t)getpid();
#endif
	XrWorkspaceClientId ids[SHELL_MAX_CLIENTS];
	uint32_t n = 0;
	for (uint32_t i = 0; i < raw_n; i++) {
		XrWorkspaceClientInfoEXT cinfo;
		if (!shell_get_client_info(raw_ids[i], &cinfo)) continue;
		if (cinfo.pid == self_pid) continue;
		ids[n++] = raw_ids[i];
	}
	if (n == 0) return;

	float disp_w = (g_xr != NULL) ? g_xr->display_width_m : 0.700f;
	for (uint32_t i = 0; i < n; i++) {
		XrPosef pose;
		float w, h;
		shell_compute_carousel_pose((int)n, (int)i, s_car.angle_offset, &pose, &w, &h);
		// Apply radius scaling. shell_compute_carousel_pose plants x = sin(theta)*radius
		// and z = (cos(theta)-1)*zmax*0.5, where radius = disp_w * 0.18. Scale x and z
		// uniformly by radius_scale to grow / shrink the ring without touching the angle.
		pose.position.x *= s_car.radius_scale;
		pose.position.z *= s_car.radius_scale;
		// Suppress unused-warning on disp_w when carousel pose math doesn't need it;
		// kept here for symmetry with grid/immersive paths.
		(void)disp_w;
		g_xr->set_pose(g_xr->session, ids[i], &pose, w, h);
	}
}

// Called every poll iteration while carousel is the active preset. Interpolates
// any pending angle-snap, advances auto-rotation / momentum, then redrives
// poses for the connected clients. Cheap when nothing is in motion.
static void
shell_carousel_tick(void)
{
	if (s_active_preset == NULL || strcmp(s_active_preset, "carousel") != 0) return;
	if (g_xr == NULL || g_xr->set_pose == NULL) return;

	// Defer to the slot-animation framework while any client is still
	// gliding into its initial carousel slot (entry transition from grid /
	// immersive). Once those settle, the carousel takes over the poses.
	if (shell_slot_anim_active_count() > 0) {
		return;
	}

	uint64_t now = shell_now_ns();
	if (s_car.last_tick_ns == 0) {
		s_car.last_tick_ns = now;
		shell_carousel_redrive_poses();
		return;
	}
	float dt = (float)((double)(now - s_car.last_tick_ns) / 1e9);
	if (dt > 0.1f) dt = 0.1f; // clamp huge gaps after sleep
	s_car.last_tick_ns = now;

	if (s_car.angle_anim_active) {
		// TAB-snap interpolation of angle_offset.
		uint64_t elapsed = now - s_car.angle_anim_start_ns;
		float t = (float)((double)elapsed / (double)s_car.angle_anim_duration_ns);
		bool done = false;
		if (t >= 1.0f) { t = 1.0f; done = true; }
		float eased = shell_ease_out_cubic(t);
		s_car.angle_offset =
		    s_car.angle_anim_start + (s_car.angle_anim_target - s_car.angle_anim_start) * eased;
		if (done) s_car.angle_anim_active = false;
	} else if (s_car.dragging) {
		// angle_offset is set incrementally by MOTION handling in the drain;
		// nothing to advance here. Suppress auto-rotation.
	} else if (now < s_car.pause_until_ns) {
		// auto-rotation paused (TAB-snap recently fired); momentum still decays.
	} else {
		// Auto-rotation OR momentum — if momentum is significant, prefer it;
		// otherwise apply ambient auto-rotation.
		if (fabsf(s_car.angular_velocity) > 0.05f) {
			s_car.angle_offset += s_car.angular_velocity * dt;
		} else {
			s_car.angle_offset += SHELL_CAROUSEL_AUTO_RATE_RPS * dt;
		}
	}

	// Decay momentum exponentially regardless of rotation source.
	if (!s_car.dragging) {
		float decay = expf(-SHELL_CAROUSEL_MOMENTUM_DECAY_PER_S * dt);
		s_car.angular_velocity *= decay;
		if (fabsf(s_car.angular_velocity) < 0.01f) s_car.angular_velocity = 0.0f;
	}

	s_car.angle_offset = shell_wrap_angle(s_car.angle_offset);
	shell_carousel_redrive_poses();
}

// Find the (PID-filtered) index of `id` in the same enumeration order
// shell_compute_carousel_pose uses. Returns -1 on miss.
static int
shell_carousel_index_of(XrWorkspaceClientId id)
{
	if (g_xr == NULL) return -1;
	XrWorkspaceClientId raw_ids[SHELL_MAX_CLIENTS];
	uint32_t raw_n = shell_enumerate_clients(raw_ids, SHELL_MAX_CLIENTS);
#ifdef _WIN32
	uint64_t self_pid = (uint64_t)GetCurrentProcessId();
#else
	uint64_t self_pid = (uint64_t)getpid();
#endif
	int idx = 0;
	for (uint32_t i = 0; i < raw_n; i++) {
		XrWorkspaceClientInfoEXT cinfo;
		if (!shell_get_client_info(raw_ids[i], &cinfo)) continue;
		if (cinfo.pid == self_pid) continue;
		if (raw_ids[i] == id) return idx;
		idx++;
	}
	return -1;
}

// Compute the angle_offset that puts client `idx` at the front (angle 0).
// shell_compute_carousel_pose places client idx at base_angle = 2π * idx/n
// plus angle_offset, so target_offset = -base_angle.
static float
shell_carousel_target_offset_for(int idx, int n)
{
	if (n <= 0) return 0.0f;
	return shell_wrap_angle(-(2.0f * SHELL_PI_F / (float)n) * (float)idx);
}

// Snap the focused client to the front. Animates angle_offset over 300 ms
// and pauses auto-rotation for 5 s afterward (matches pre-Phase-2.G runtime
// behaviour).
static void
shell_carousel_tab_snap(void)
{
	if (s_focused_client_id == 0) return;
	int idx = shell_carousel_index_of((XrWorkspaceClientId)s_focused_client_id);
	if (idx < 0) return;
	XrWorkspaceClientId raw_ids[SHELL_MAX_CLIENTS];
	uint32_t raw_n = shell_enumerate_clients(raw_ids, SHELL_MAX_CLIENTS);
#ifdef _WIN32
	uint64_t self_pid = (uint64_t)GetCurrentProcessId();
#else
	uint64_t self_pid = (uint64_t)getpid();
#endif
	int n = 0;
	for (uint32_t i = 0; i < raw_n; i++) {
		XrWorkspaceClientInfoEXT cinfo;
		if (!shell_get_client_info(raw_ids[i], &cinfo)) continue;
		if (cinfo.pid == self_pid) continue;
		n++;
	}
	if (n <= 0) return;

	float target = shell_carousel_target_offset_for(idx, n);
	// Pick the short-arc direction so we don't sweep all the way around.
	float delta = shell_wrap_angle(target - s_car.angle_offset);
	target = s_car.angle_offset + delta;

	s_car.angle_anim_active = true;
	s_car.angle_anim_start = s_car.angle_offset;
	s_car.angle_anim_target = target;
	s_car.angle_anim_start_ns = shell_now_ns();
	s_car.angle_anim_duration_ns = SHELL_CAROUSEL_ANGLE_ANIM_NS;
	s_car.angular_velocity = 0.0f;
	s_car.pause_until_ns = shell_now_ns() + SHELL_CAROUSEL_SNAP_PAUSE_NS;
}

// Pointer capture toggling. Carousel needs MOTION events to flow even when
// the cursor leaves the workspace window (drag continues), so flip on entry
// to carousel and off on exit. Idempotent.
static void
shell_set_pointer_capture(bool want)
{
	if (g_xr == NULL || want == s_pointer_capture_active) return;
	if (want) {
		if (g_xr->enable_pointer_capture != NULL) {
			g_xr->enable_pointer_capture(g_xr->session, 1u); // LMB
			s_pointer_capture_active = true;
		}
	} else {
		if (g_xr->disable_pointer_capture != NULL) {
			g_xr->disable_pointer_capture(g_xr->session);
			s_pointer_capture_active = false;
		}
	}
}


// Adaptive grid: ceil(sqrt(N)) columns, fills row-first. 90% display, 10%
// padding per cell. All windows at Z=0. Mirrors compute_grid_layout().
static void
shell_compute_grid_pose(int n, int idx,
                        float *out_x, float *out_y, float *out_z,
                        float *out_w, float *out_h)
{
	float disp_w = (g_xr != NULL) ? g_xr->display_width_m : 0.700f;
	float disp_h = (g_xr != NULL) ? g_xr->display_height_m : 0.394f;
	if (n <= 0) n = 1;
	int cols = (int)ceilf(sqrtf((float)n));
	int rows = (int)ceilf((float)n / (float)cols);
	int col = idx % cols;
	int row = idx / cols;
	float cell_w = disp_w * 0.90f / (float)cols;
	float cell_h = disp_h * 0.90f / (float)rows;
	*out_w = cell_w * 0.90f;
	*out_h = cell_h * 0.90f;
	*out_x = ((float)col - ((float)(cols - 1)) / 2.0f) * cell_w;
	*out_y = (((float)(rows - 1)) / 2.0f - (float)row) * cell_h;
	*out_z = 0.0f;
}

// Grid tangent to a convex paraboloid: Z curves toward viewer at the edges,
// each window tilts to face the surface normal. Curvature is tuned so the
// corners sit ~+0.015 m forward of the display plane.
static void
shell_compute_immersive_pose(int n, int idx, XrPosef *out_pose,
                             float *out_w, float *out_h)
{
	float disp_w = (g_xr != NULL) ? g_xr->display_width_m : 0.700f;
	float disp_h = (g_xr != NULL) ? g_xr->display_height_m : 0.394f;
	float x, y, z;
	shell_compute_grid_pose(n, idx, &x, &y, &z, out_w, out_h);

	float max_r_sq = (disp_w / 2) * (disp_w / 2) +
	                 (disp_h / 2) * (disp_h / 2);
	float curvature = 0.015f / max_r_sq;
	float r_sq = x * x + y * y;
	z = curvature * r_sq;

	float dzdx = 2.0f * curvature * x;
	float dzdy = 2.0f * curvature * y;
	float yaw = -atanf(dzdx);
	float pitch = atanf(dzdy);
	float cy = cosf(yaw / 2), sy = sinf(yaw / 2);
	float cp = cosf(pitch / 2), sp = sinf(pitch / 2);

	out_pose->position.x = x;
	out_pose->position.y = y;
	out_pose->position.z = z;
	out_pose->orientation.x = sp * cy;
	out_pose->orientation.y = cp * sy;
	out_pose->orientation.z = -sp * sy;
	out_pose->orientation.w = cp * cy;
}

// Static carousel: windows arranged on a 360° ring, front at Z=0 and back at
// -zmax. Phase 2.G ships a snap (no per-frame rotation). The runtime used to
// auto-rotate; the controller can add that back as a follow-up by ticking the
// angle each frame and re-pushing poses.
static void
shell_compute_carousel_pose(int n, int idx, float angle_offset,
                            XrPosef *out_pose,
                            float *out_w, float *out_h)
{
	float disp_w = (g_xr != NULL) ? g_xr->display_width_m : 0.700f;
	float disp_h = (g_xr != NULL) ? g_xr->display_height_m : 0.394f;
	float zmax = (disp_w > disp_h ? disp_w : disp_h) / 5.0f;
	float radius = disp_w * 0.18f;
	float base_w = disp_w * 0.40f;
	float base_h = disp_h * 0.40f;
	if (n <= 0) n = 1;

	float base_angle = (2.0f * SHELL_PI_F / (float)n) * (float)idx;
	float world_angle = base_angle + angle_offset;

	out_pose->position.x = sinf(world_angle) * radius;
	float raw_depth = cosf(world_angle); // +1 front, -1 back
	out_pose->position.z = (raw_depth - 1.0f) * zmax * 0.5f;
	out_pose->position.y = 0.0f;

	float depth_t = (raw_depth + 1.0f) / 2.0f;
	float scale = 0.70f + 0.30f * depth_t;
	*out_w = base_w * scale;
	*out_h = base_h * scale;

	out_pose->orientation.x = 0.0f;
	out_pose->orientation.y = 0.0f;
	out_pose->orientation.z = 0.0f;
	out_pose->orientation.w = 1.0f;
}

// Apply a named preset to all currently-connected workspace clients.
// Returns true if at least one pose was pushed. Unknown names are ignored.
//
// xrEnumerateWorkspaceClientsEXT includes the controller's own session in
// the list (as a sentinel — the runtime tracks every connected session).
// Layout presets need to skip it: the controller has no positionable
// window, and including it in the count throws off grid math (e.g. 2 real
// cubes + the controller would be laid out as 3 cells in a 2×2 grid,
// pushing the cubes into corners).
static bool
shell_apply_preset(const char *name)
{
	if (g_xr == NULL || name == NULL) {
		return false;
	}
	XrWorkspaceClientId raw_ids[SHELL_MAX_CLIENTS];
	uint32_t raw_n = shell_enumerate_clients(raw_ids, SHELL_MAX_CLIENTS);
	if (raw_n == 0) {
		P("Layout '%s': no clients yet\n", name);
		return false;
	}

	// Filter out the controller's own session by PID match.
#ifdef _WIN32
	uint64_t self_pid = (uint64_t)GetCurrentProcessId();
#else
	uint64_t self_pid = (uint64_t)getpid();
#endif
	XrWorkspaceClientId ids[SHELL_MAX_CLIENTS];
	uint32_t n = 0;
	for (uint32_t i = 0; i < raw_n; i++) {
		XrWorkspaceClientInfoEXT cinfo;
		if (!shell_get_client_info(raw_ids[i], &cinfo)) {
			continue;
		}
		if (cinfo.pid == self_pid) {
			continue; // skip self
		}
		ids[n++] = raw_ids[i];
	}
	if (n == 0) {
		P("Layout '%s': no app clients yet\n", name);
		return false;
	}

	bool is_grid      = (strcmp(name, "grid") == 0);
	bool is_immersive = (strcmp(name, "immersive") == 0);
	bool is_carousel  = (strcmp(name, "carousel") == 0);
	if (!is_grid && !is_immersive && !is_carousel) {
		return false;
	}

	// Phase 2.K: track the active preset so the per-tick carousel state
	// machine knows when to take over and toggle pointer capture as we
	// enter / leave carousel. Reset carousel state on (re-)entry so a
	// fresh angle ramp starts from the static snap position.
	s_active_preset = is_grid ? "grid" : (is_immersive ? "immersive" : "carousel");
	if (is_carousel) {
		s_car.angle_offset = 0.0f;
		s_car.angular_velocity = 0.0f;
		s_car.dragging = false;
		s_car.angle_anim_active = false;
		s_car.last_tick_ns = 0;
		s_car.pause_until_ns = 0;
		shell_set_pointer_capture(true);
	} else {
		shell_set_pointer_capture(false);
	}

	uint32_t seeded = 0;
	for (uint32_t i = 0; i < n; i++) {
		XrPosef pose;
		pose.orientation.x = 0.0f;
		pose.orientation.y = 0.0f;
		pose.orientation.z = 0.0f;
		pose.orientation.w = 1.0f;
		pose.position.x = 0.0f;
		pose.position.y = 0.0f;
		pose.position.z = 0.0f;
		float w = 0.0f, h = 0.0f;

		if (is_grid) {
			float x, y, z;
			shell_compute_grid_pose((int)n, (int)i,
			                        &x, &y, &z, &w, &h);
			pose.position.x = x;
			pose.position.y = y;
			pose.position.z = z;
		} else if (is_immersive) {
			shell_compute_immersive_pose((int)n, (int)i,
			                             &pose, &w, &h);
		} else { // carousel
			shell_compute_carousel_pose((int)n, (int)i, 0.0f,
			                            &pose, &w, &h);
		}

		// Phase 2.K: instead of snapping via set_pose, seed an animation
		// from the current pose to the new one. The main-loop tick drives
		// per-frame interpolation until the animation completes (~300 ms).
		// Seed returns false during the connect-time race (slot not yet
		// bound — get_pose fails); the caller retries on the next poll.
		if (shell_slot_anim_seed(ids[i], &pose, w, h, SHELL_ANIM_DURATION_NS)) {
			seeded++;
		}

		// Phase 2.C: re-push chrome layout for the slot's target size so
		// the controller-rendered chrome scales with the window. One
		// IPC RPC per preset apply (not per tick) — animation set_pose
		// path stays uncontended.
		if (g_chrome != NULL) {
			shell_chrome_on_window_resized(g_chrome, ids[i], w, h);
		}
	}

	if (seeded > 0 && seeded < n) {
		P("Layout '%s': partial seed %u/%u (slot binding still in flight)\n",
		  name, seeded, n);
	} else if (seeded > 0) {
		P("Layout '%s' (%u windows) — gliding over %llu ms\n",
		  name, n, (unsigned long long)(SHELL_ANIM_DURATION_NS / 1000000ULL));
	}

	// Return true only when every client seeded — that's the signal the
	// auto-tile-on-connect retry path watches for to clear its pending flag.
	return seeded == n && n > 0;
}

// Drain pending workspace input events; dispatch Ctrl+1..3 to layout presets.
// The modifier bit layout follows XR_EXT_spatial_workspace's MVP policy:
// bit 0 SHIFT, bit 1 CTRL, bit 2 ALT.
//
// Phase 2.K: also consumes the new MOTION / FRAME_TICK / FOCUS_CHANGED
// variants. Commit 4 silently absorbs them (FRAME_TICK is read so the count
// is consumed; FOCUS_CHANGED is logged but not yet acted on); commit 5 wires
// MOTION + SCROLL into the carousel state machine.

// Phase 2.K: latest workspace state read from the drain. The carousel state
// machine reads s_focused_client_id (forward-declared at file top) to drive
// TAB-snap.

static void
shell_drain_input_events(void)
{
	if (g_xr == NULL || g_xr->enumerate_input_events == NULL) {
		return;
	}
	XrWorkspaceInputEventEXT evs[16];
	uint32_t cnt = 0;
	if (g_xr->enumerate_input_events(g_xr->session, 16, &cnt, evs) != XR_SUCCESS) {
		return;
	}
	bool carousel_active = (s_active_preset != NULL && strcmp(s_active_preset, "carousel") == 0);
	for (uint32_t i = 0; i < cnt; i++) {
		const XrWorkspaceInputEventEXT *e = &evs[i];
		switch (e->eventType) {
		case XR_WORKSPACE_INPUT_EVENT_KEY_EXT: {
			if (!e->key.isDown) break;
			bool ctrl = (e->key.modifiers & 0x2u) != 0;
			if (ctrl) {
				switch (e->key.vkCode) {
				case '1': shell_apply_preset("grid"); break;
				case '2': shell_apply_preset("immersive"); break;
				case '3': shell_apply_preset("carousel"); break;
				// '4' reserved for a future preset slot.
				default: break;
				}
			} else if (e->key.vkCode == 0x09 /*VK_TAB*/ && carousel_active) {
				// Phase 2.K: TAB while carousel is active snaps the
				// focused client to the front and pauses auto-rotation.
				shell_carousel_tab_snap();
			}
			break;
		}
		case XR_WORKSPACE_INPUT_EVENT_FOCUS_CHANGED_EXT:
			s_focused_client_id = (int)e->focusChanged.currentClientId;
			break;
		case XR_WORKSPACE_INPUT_EVENT_POINTER_EXT: {
			// Phase 2.C C4: dispatch chrome-region clicks. The runtime
			// fills chromeRegionId on POINTER events when the cursor
			// hits the controller-submitted chrome quad inside one of
			// the regions push_layout populated. Acts on LMB-down only.
			//
			// In-runtime chrome (still drawing until C5) ALSO fires the
			// runtime-side close/min/max handlers via the legacy
			// in_close_btn etc. fields — this is additive, not exclusive.
			// Once C5 deletes the in-runtime path, the controller-driven
			// dispatch here becomes the sole chrome interactivity owner.
			if (e->pointer.button == 1 /* LMB */ && e->pointer.isDown &&
			    e->pointer.chromeRegionId != 0 &&
			    e->pointer.hitClientId != 0) {
				switch (e->pointer.chromeRegionId) {
				case SHELL_CHROME_REGION_CLOSE:
					if (g_xr->request_client_exit != NULL) {
						(void)g_xr->request_client_exit(g_xr->session,
						                                e->pointer.hitClientId);
					}
					break;
				case SHELL_CHROME_REGION_MAX:
					if (g_xr->request_client_fullscreen != NULL) {
						(void)g_xr->request_client_fullscreen(g_xr->session,
						                                       e->pointer.hitClientId,
						                                       XR_TRUE);
					}
					break;
				case SHELL_CHROME_REGION_MIN:
				case SHELL_CHROME_REGION_GRIP:
				default:
					// Min: no shell-side handler yet (parity with the
					// in-runtime button which is also a no-op visually).
					// Grip: drag-to-move lives in the carousel handler
					// below for now; a future commit can move it here.
					break;
				}
			}

			// Carousel only: LMB on a TITLE_BAR begins drag; LMB up ends
			// drag and latches momentum from the most recent angular
			// velocity sample. Other buttons + non-carousel are ignored —
			// the runtime still routes clicks through the focus path.
			if (!carousel_active) break;
			if (e->pointer.button != 1) break; // LMB only
			if (e->pointer.isDown) {
				if (e->pointer.hitRegion == XR_WORKSPACE_HIT_REGION_TITLE_BAR_EXT) {
					s_car.dragging = true;
					s_car.drag_last_cursor_x = e->pointer.cursorX;
					s_car.drag_last_dx_ns = shell_now_ns();
					s_car.drag_last_dx_per_s = 0.0f;
					s_car.angular_velocity = 0.0f;
					s_car.angle_anim_active = false;
				}
			} else {
				if (s_car.dragging) {
					// Latch the most recent dx-per-second as momentum.
					s_car.angular_velocity = s_car.drag_last_dx_per_s;
					s_car.dragging = false;
				}
			}
			break;
		}
		case XR_WORKSPACE_INPUT_EVENT_POINTER_HOVER_EXT: {
			// Phase 2.C C3.C-4: hovered-slot transitions from the runtime.
			// Emitted whenever the per-frame hit-test sees the cursor enter
			// or leave a window slot — regardless of pointer-capture state,
			// so this works in grid + immersive modes where MOTION events
			// don't fire. currentClientId == 0 means "cursor off all slots."
			//
			// Phase 2.C spec_version 9: also fires on chromeRegionId
			// transitions WITHIN the same slot, so the chrome can drive
			// per-button hover lighten without continuous pointer capture.
			if (g_chrome != NULL) {
				shell_chrome_set_hover(g_chrome,
				                       e->pointerHover.currentClientId,
				                       (uint32_t)e->pointerHover.currentChromeRegionId);
			}
			break;
		}
		case XR_WORKSPACE_INPUT_EVENT_WINDOW_POSE_CHANGED_EXT: {
			// Phase 2.C C5 follow-up: runtime resized or repositioned a
			// window (edge drag, fullscreen toggle, etc.). Re-push chrome
			// layout so the pill tracks the new window top edge instead
			// of staying at the create-time dimensions.
			if (g_chrome != NULL && e->windowPoseChanged.clientId != 0) {
				shell_chrome_on_window_resized(g_chrome,
				                                e->windowPoseChanged.clientId,
				                                e->windowPoseChanged.widthMeters,
				                                e->windowPoseChanged.heightMeters);
			}
			break;
		}
		case XR_WORKSPACE_INPUT_EVENT_POINTER_MOTION_EXT: {
			if (!carousel_active || !s_car.dragging) break;
			// Accumulate angle from cursor delta; sample angular velocity
			// in rad/sec for the post-release momentum.
			int32_t dx = e->pointerMotion.cursorX - s_car.drag_last_cursor_x;
			s_car.drag_last_cursor_x = e->pointerMotion.cursorX;
			float drot = (float)dx * SHELL_CAROUSEL_DRAG_SENSITIVITY_RPP;
			s_car.angle_offset = shell_wrap_angle(s_car.angle_offset + drot);

			uint64_t now = shell_now_ns();
			uint64_t dt_ns = now - s_car.drag_last_dx_ns;
			if (dt_ns > 1000000ULL) { // > 1 ms — avoid divide-by-near-zero
				float dt = (float)((double)dt_ns / 1e9);
				s_car.drag_last_dx_per_s = drot / dt;
				s_car.drag_last_dx_ns = now;
			}
			break;
		}
		case XR_WORKSPACE_INPUT_EVENT_SCROLL_EXT: {
			if (!carousel_active) break;
			// deltaY > 0 = scroll up; expand ring. deltaY < 0 = scroll down;
			// shrink ring. Multiplicative so symmetric in log space.
			float factor = 1.0f + e->scroll.deltaY * 0.05f;
			s_car.radius_scale *= factor;
			if (s_car.radius_scale < SHELL_CAROUSEL_RADIUS_MIN)
				s_car.radius_scale = SHELL_CAROUSEL_RADIUS_MIN;
			if (s_car.radius_scale > SHELL_CAROUSEL_RADIUS_MAX)
				s_car.radius_scale = SHELL_CAROUSEL_RADIUS_MAX;
			break;
		}
		case XR_WORKSPACE_INPUT_EVENT_FRAME_TICK_EXT:
			// Tick is paced by the main-loop poll cadence (variable 16/500
			// ms) — FRAME_TICK is consumed here so the count is drained
			// but does not redundantly drive the tick.
			break;
		default:
			break;
		}
	}
}

static void
signal_handler(int sig)
{
	(void)sig;
	g_running = 0;
}

struct app_entry
{
	const char *exe_path;
	bool has_pose;
	float px, py, pz;       // position in meters from display center
	float width_m, height_m; // window physical size in meters
#ifdef _WIN32
	HANDLE process;
	DWORD pid;
#endif
	bool pose_applied;
};

struct capture_entry
{
	uint64_t hwnd;
	char name[128];
	uint32_t client_id; // filled after IPC call
	bool added;
};

#define MAX_SAVED_WINDOWS 32

struct saved_window
{
	char app_name[128];
	float x, y, z;
	float width_m, height_m;
};

struct shell_config
{
	struct saved_window windows[MAX_SAVED_WINDOWS];
	int window_count;
};

static void
get_config_path(char *buf, size_t buf_size)
{
#ifdef _WIN32
	const char *appdata = getenv("LOCALAPPDATA");
	if (appdata) {
		snprintf(buf, buf_size, "%s\\DisplayXR\\shell_layout.json", appdata);
	} else {
		snprintf(buf, buf_size, "shell_layout.json");
	}
#else
	const char *home = getenv("HOME");
	if (home) {
		snprintf(buf, buf_size, "%s/.displayxr/shell_layout.json", home);
	} else {
		snprintf(buf, buf_size, "shell_layout.json");
	}
#endif
}

static void
shell_config_load(struct shell_config *cfg)
{
	cfg->window_count = 0;

	char path[512];
	get_config_path(path, sizeof(path));

	FILE *f = fopen(path, "rb");
	if (!f) return;

	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len <= 0 || len > 64 * 1024) { fclose(f); return; }

	char *data = (char *)malloc(len + 1);
	fread(data, 1, len, f);
	data[len] = '\0';
	fclose(f);

	cJSON *root = cJSON_Parse(data);
	free(data);
	if (!root) return;

	cJSON *windows = cJSON_GetObjectItemCaseSensitive(root, "windows");
	if (cJSON_IsObject(windows)) {
		cJSON *entry = NULL;
		cJSON_ArrayForEach(entry, windows)
		{
			if (cfg->window_count >= MAX_SAVED_WINDOWS) break;
			struct saved_window *sw = &cfg->windows[cfg->window_count];
			snprintf(sw->app_name, sizeof(sw->app_name), "%s", entry->string);

			cJSON *jx = cJSON_GetObjectItemCaseSensitive(entry, "x");
			cJSON *jy = cJSON_GetObjectItemCaseSensitive(entry, "y");
			cJSON *jz = cJSON_GetObjectItemCaseSensitive(entry, "z");
			cJSON *jw = cJSON_GetObjectItemCaseSensitive(entry, "w");
			cJSON *jh = cJSON_GetObjectItemCaseSensitive(entry, "h");
			if (cJSON_IsNumber(jx)) sw->x = (float)jx->valuedouble;
			if (cJSON_IsNumber(jy)) sw->y = (float)jy->valuedouble;
			if (cJSON_IsNumber(jz)) sw->z = (float)jz->valuedouble;
			if (cJSON_IsNumber(jw)) sw->width_m = (float)jw->valuedouble;
			if (cJSON_IsNumber(jh)) sw->height_m = (float)jh->valuedouble;
			cfg->window_count++;
		}
	}
	cJSON_Delete(root);
	P("Loaded %d saved window poses from %s\n", cfg->window_count, path);
}

static void
shell_config_save(const struct shell_config *cfg)
{
	if (cfg->window_count == 0) return;

	char path[512];
	get_config_path(path, sizeof(path));

	// Ensure directory exists
#ifdef _WIN32
	{
		char dir[512];
		snprintf(dir, sizeof(dir), "%s", path);
		char *last = strrchr(dir, '\\');
		if (last) { *last = '\0'; CreateDirectoryA(dir, NULL); }
	}
#endif

	cJSON *root = cJSON_CreateObject();
	cJSON_AddNumberToObject(root, "version", 1);
	cJSON *windows = cJSON_AddObjectToObject(root, "windows");

	for (int i = 0; i < cfg->window_count; i++) {
		const struct saved_window *sw = &cfg->windows[i];
		cJSON *entry = cJSON_AddObjectToObject(windows, sw->app_name);
		cJSON_AddNumberToObject(entry, "x", sw->x);
		cJSON_AddNumberToObject(entry, "y", sw->y);
		cJSON_AddNumberToObject(entry, "z", sw->z);
		cJSON_AddNumberToObject(entry, "w", sw->width_m);
		cJSON_AddNumberToObject(entry, "h", sw->height_m);
	}

	char *json_str = cJSON_Print(root);
	cJSON_Delete(root);

	FILE *f = fopen(path, "wb");
	if (f) {
		fwrite(json_str, 1, strlen(json_str), f);
		fclose(f);
	}
	free(json_str);
}

static struct saved_window *
shell_config_find(struct shell_config *cfg, const char *app_name)
{
	for (int i = 0; i < cfg->window_count; i++) {
		if (strcmp(cfg->windows[i].app_name, app_name) == 0) {
			return &cfg->windows[i];
		}
	}
	return NULL;
}

static void
shell_config_update(struct shell_config *cfg, const char *app_name,
                    float x, float y, float z, float w, float h)
{
	struct saved_window *sw = shell_config_find(cfg, app_name);
	if (!sw) {
		if (cfg->window_count >= MAX_SAVED_WINDOWS) return;
		sw = &cfg->windows[cfg->window_count++];
		snprintf(sw->app_name, sizeof(sw->app_name), "%s", app_name);
	}
	sw->x = x; sw->y = y; sw->z = z;
	sw->width_m = w; sw->height_m = h;
}

// --- 4C.9 / 5.5: Registered apps config ---

#define MAX_REGISTERED_APPS 32

struct registered_app
{
	char name[128];
	char exe_path[MAX_PATH];
	char manifest_path[MAX_PATH];  // .displayxr.json path (so launcher remove can delete it); "" for legacy entries
	char type[8];          // "3d", "2d", or "" (unknown)
	char category[32];     // manifest "category", default "app"
	char description[256]; // manifest "description"
	char display_mode[16]; // manifest "display_mode", default "auto"
	char icon_path[MAX_PATH];     // resolved 2D icon (absolute) or ""
	char icon_3d_path[MAX_PATH];  // resolved 3D icon (absolute) or ""
	char icon_3d_layout[8];       // "sbs-lr"|"sbs-rl"|"tb"|"bt"|""
};

static struct registered_app g_registered_apps[MAX_REGISTERED_APPS];
static int g_registered_app_count = 0;

static void
get_registered_apps_path(char *buf, size_t buf_size)
{
#ifdef _WIN32
	const char *appdata = getenv("LOCALAPPDATA");
	if (appdata) {
		snprintf(buf, buf_size, "%s\\DisplayXR\\registered_apps.json", appdata);
	} else {
		snprintf(buf, buf_size, "registered_apps.json");
	}
#else
	const char *home = getenv("HOME");
	if (home) {
		snprintf(buf, buf_size, "%s/.displayxr/registered_apps.json", home);
	} else {
		snprintf(buf, buf_size, "registered_apps.json");
	}
#endif
}

// Case-insensitive exe_path comparison. Normalizes forward slashes to
// backslashes before comparing so "test_apps/x/build/x.exe" matches
// "test_apps\x\build\x.exe".
static bool
exe_path_equal(const char *a, const char *b)
{
	if (a == NULL || b == NULL) return a == b;
	while (*a && *b) {
		int ca = (unsigned char)*a;
		int cb = (unsigned char)*b;
		if (ca == '/') ca = '\\';
		if (cb == '/') cb = '\\';
		if (ca >= 'A' && ca <= 'Z') ca += 32;
		if (cb >= 'A' && cb <= 'Z') cb += 32;
		if (ca != cb) return false;
		a++; b++;
	}
	return *a == '\0' && *b == '\0';
}

// Phase 2.C C3.C-3a: resolve a registered_app's icon_path for a given OS
// PID. Used by the chrome lazy-create path to pass the per-client app
// icon to shell_chrome_on_client_connected. Returns "" if no icon is
// resolvable. Best-effort; not fatal.
//
// Resolution order:
//   1. Open the PID, get exe path via QueryFullProcessImageName.
//   2. Match exe path against g_registered_apps; if found AND the app
//      has a sidecar icon_path, return that.
//   3. Else fall back to extracting the Win32 PE icon (.ico embedded in
//      the exe) into a per-exe cached PNG under %TEMP% and return
//      that path.
//
// The fallback path picks up app icons even for unregistered apps
// (test_apps without sidecar manifests) and matches the icon Windows
// itself uses in Explorer.
#ifdef _WIN32
static bool extract_pe_icon_to_png(const char *exe, const char *out_png); // fwd decl
static void sanitize_basename(const char *src, char *dst, size_t dst_size); // fwd decl

static const char *
shell_resolve_icon_for_pid(unsigned long pid)
{
	static char cached_path[MAX_PATH];
	cached_path[0] = '\0';

	if (pid == 0) return "";
	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
	if (h == NULL) return "";
	char exe[MAX_PATH] = {0};
	DWORD n = (DWORD)sizeof(exe);
	BOOL ok = QueryFullProcessImageNameA(h, 0, exe, &n);
	CloseHandle(h);
	if (!ok || n == 0) return "";

	// 1. Sidecar icon from registered_app, if available.
	for (int i = 0; i < g_registered_app_count; i++) {
		if (exe_path_equal(g_registered_apps[i].exe_path, exe) &&
		    g_registered_apps[i].icon_path[0] != '\0') {
			snprintf(cached_path, sizeof(cached_path), "%s",
			         g_registered_apps[i].icon_path);
			return cached_path;
		}
	}

	// 2. Fallback: extract the PE icon to a cached PNG under %TEMP%.
	// Cache by sanitized exe path so we don't re-extract on every call.
	const char *tmp = getenv("TEMP");
	if (tmp == NULL || !*tmp) return "";
	char base[MAX_PATH];
	const char *fname = strrchr(exe, '\\');
	fname = fname ? fname + 1 : exe;
	sanitize_basename(fname, base, sizeof(base));
	snprintf(cached_path, sizeof(cached_path),
	         "%s\\displayxr_app_icon_%s.png", tmp, base);
	if (GetFileAttributesA(cached_path) == INVALID_FILE_ATTRIBUTES) {
		if (!extract_pe_icon_to_png(exe, cached_path)) {
			cached_path[0] = '\0';
			return "";
		}
	}
	return cached_path;
}
#else
static const char *
shell_resolve_icon_for_pid(unsigned long pid) { (void)pid; return ""; }
#endif

// Phase 2.C C3.C-3b: resolve the per-app title text the shell_chrome pill
// renders between the icon and the grip dots. Mirrors shell_resolve_icon_
// for_pid: prefer the registered_app sidecar's friendly `name` (e.g.
// "Cube D3D11 (Handle)") when the PID's exe matches a registered app;
// otherwise fall back to the OpenXR application name (cinfo.name, e.g.
// "SRCubeOpenXRExt"). Returns "" when neither is resolvable.
//
// Note: cinfo.name does NOT carry the shell-side " (N)" duplicate-instance
// suffix — that suffix is appended only in the launcher's print path
// (main.c near line 3411), not in the runtime payload — so no suffix
// stripping is needed here.
#ifdef _WIN32
static const char *
shell_resolve_title_for_pid(unsigned long pid, const char *fallback_name)
{
	static char cached_title[128];
	cached_title[0] = '\0';

	if (pid != 0) {
		HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
		if (h != NULL) {
			char exe[MAX_PATH] = {0};
			DWORD n = (DWORD)sizeof(exe);
			BOOL ok = QueryFullProcessImageNameA(h, 0, exe, &n);
			CloseHandle(h);
			if (ok && n > 0) {
				for (int i = 0; i < g_registered_app_count; i++) {
					if (exe_path_equal(g_registered_apps[i].exe_path, exe) &&
					    g_registered_apps[i].name[0] != '\0') {
						snprintf(cached_title, sizeof(cached_title), "%s",
						         g_registered_apps[i].name);
						return cached_title;
					}
				}
			}
		}
	}

	if (fallback_name != NULL && fallback_name[0] != '\0') {
		snprintf(cached_title, sizeof(cached_title), "%s", fallback_name);
		return cached_title;
	}
	return "";
}
#else
static const char *
shell_resolve_title_for_pid(unsigned long pid, const char *fallback_name)
{
	(void)pid;
	return (fallback_name != NULL) ? fallback_name : "";
}
#endif

// Read a string field from a cJSON object into a fixed buffer. If the field
// is missing or not a string, dst is left untouched.
static void
json_copy_str(char *dst, size_t dst_size, const cJSON *obj, const char *key)
{
	const cJSON *j = cJSON_GetObjectItemCaseSensitive(obj, key);
	if (cJSON_IsString(j) && j->valuestring != NULL) {
		snprintf(dst, dst_size, "%s", j->valuestring);
	}
}

static void
registered_app_zero(struct registered_app *app)
{
	memset(app, 0, sizeof(*app));
}

// Append a scanned app into the in-memory registry. The scanner already
// dedups by exe_path, so we just append. Caller is responsible for clearing
// g_registered_apps[] before the scan run.
static void
append_scanned_app(const struct shell_scanned_app *s)
{
	if (g_registered_app_count >= MAX_REGISTERED_APPS) {
		PE("registry full — dropping scanned app '%s'\n", s->name);
		return;
	}
	struct registered_app *app = &g_registered_apps[g_registered_app_count++];
	registered_app_zero(app);
	snprintf(app->name, sizeof(app->name), "%s", s->name);
	snprintf(app->exe_path, sizeof(app->exe_path), "%s", s->exe_path);
	snprintf(app->manifest_path, sizeof(app->manifest_path), "%s", s->manifest_path);
	snprintf(app->type, sizeof(app->type), "%s", s->type);
	snprintf(app->category, sizeof(app->category), "%s", s->category);
	snprintf(app->description, sizeof(app->description), "%s", s->description);
	snprintf(app->display_mode, sizeof(app->display_mode), "%s", s->display_mode);
	snprintf(app->icon_path, sizeof(app->icon_path), "%s", s->icon_path);
	snprintf(app->icon_3d_path, sizeof(app->icon_3d_path), "%s", s->icon_3d_path);
	snprintf(app->icon_3d_layout, sizeof(app->icon_3d_layout), "%s", s->icon_3d_layout);
}

// Forward declarations — these are called from registered_apps_load but
// defined later in this file.
static void
registered_apps_save(void);
#ifdef _WIN32
static void
get_exe_dir(char *buf, size_t buf_size);
#endif

// Rebuild the in-memory registry from the manifest scanner. Manifests
// (sidecar + registered) are now the sole source of truth — registered_apps.json
// is just a debug snapshot of the last scan, not a separate input. See
// docs/specs/displayxr-app-manifest.md §11.
static void
registered_apps_load(void)
{
	g_registered_app_count = 0;

	char path[512];
	get_registered_apps_path(path, sizeof(path));

#ifdef _WIN32
	{
		// Make sure the parent directory exists before we try to write the
		// debug snapshot.
		char dir[512];
		snprintf(dir, sizeof(dir), "%s", path);
		char *last = strrchr(dir, '\\');
		if (last) { *last = '\0'; CreateDirectoryA(dir, NULL); }
	}

	{
		char exe_dir[MAX_PATH] = {0};
		get_exe_dir(exe_dir, sizeof(exe_dir));
		size_t edl = strlen(exe_dir);
		if (edl > 0 && (exe_dir[edl - 1] == '\\' || exe_dir[edl - 1] == '/')) {
			exe_dir[edl - 1] = '\0';
		}

		struct shell_scanned_app scanned[MAX_REGISTERED_APPS];
		int n = shell_scan_apps(exe_dir, scanned, MAX_REGISTERED_APPS);
		for (int i = 0; i < n; i++) {
			append_scanned_app(&scanned[i]);
		}
	}
#endif

	P("Registry: %d app(s) after scan.\n", g_registered_app_count);
	for (int i = 0; i < g_registered_app_count; i++) {
		P("  %s  (%s)\n", g_registered_apps[i].name, g_registered_apps[i].exe_path);
	}

	// Persist a snapshot of the current scan for debugging / inspection.
	// Not a source of truth — the next load() rebuilds entirely from manifests.
	registered_apps_save();
}

static void
registered_apps_save(void)
{
	char path[512];
	get_registered_apps_path(path, sizeof(path));

	cJSON *arr = cJSON_CreateArray();
	for (int i = 0; i < g_registered_app_count; i++) {
		cJSON *obj = cJSON_CreateObject();
		const struct registered_app *app = &g_registered_apps[i];
		cJSON_AddStringToObject(obj, "name", app->name);
		cJSON_AddStringToObject(obj, "exe_path", app->exe_path);
		cJSON_AddStringToObject(obj, "type", app->type);
		if (app->manifest_path[0])  cJSON_AddStringToObject(obj, "manifest_path", app->manifest_path);
		if (app->category[0])       cJSON_AddStringToObject(obj, "category", app->category);
		if (app->description[0])    cJSON_AddStringToObject(obj, "description", app->description);
		if (app->display_mode[0])   cJSON_AddStringToObject(obj, "display_mode", app->display_mode);
		if (app->icon_path[0])      cJSON_AddStringToObject(obj, "icon_path", app->icon_path);
		if (app->icon_3d_path[0])   cJSON_AddStringToObject(obj, "icon_3d_path", app->icon_3d_path);
		if (app->icon_3d_layout[0]) cJSON_AddStringToObject(obj, "icon_3d_layout", app->icon_3d_layout);
		cJSON_AddItemToArray(arr, obj);
	}
	char *json_str = cJSON_Print(arr);
	cJSON_Delete(arr);

	FILE *f = fopen(path, "wb");
	if (f) {
		fwrite(json_str, 1, strlen(json_str), f);
		fclose(f);
	}
	free(json_str);
}

#ifdef _WIN32
/*!
 * Get the directory containing the shell executable.
 * Returns path with trailing backslash, e.g. "C:\..._package\bin\"
 */
static void
get_exe_dir(char *buf, size_t buf_size)
{
	GetModuleFileNameA(NULL, buf, (DWORD)buf_size);
	char *last_sep = strrchr(buf, '\\');
	if (last_sep == NULL) {
		last_sep = strrchr(buf, '/');
	}
	if (last_sep != NULL) {
		last_sep[1] = '\0';
	}
}



/*!
 * Resolve the XR_RUNTIME_JSON manifest path.
 * Tries (in order):
 * 1. _package/DisplayXR_win64.json (installed manifest, relative to bin/)
 * 2. build/Release/openxr_displayxr-dev.json (dev build, relative to CWD)
 */
static bool
get_runtime_json_path(char *buf, size_t buf_size)
{
	// Try dev build manifest relative to CWD first (correct DLL path)
	char cwd[MAX_PATH];
	GetCurrentDirectoryA(MAX_PATH, cwd);
	snprintf(buf, buf_size, "%s\\build\\Release\\openxr_displayxr-dev.json", cwd);
	if (GetFileAttributesA(buf) != INVALID_FILE_ATTRIBUTES) {
		return true;
	}

	// Try installed manifest: _package/DisplayXR_win64.json
	char exe_dir[MAX_PATH];
	get_exe_dir(exe_dir, sizeof(exe_dir));

	// exe_dir = "..._package/bin/" → go up to "_package/"
	size_t len = strlen(exe_dir);
	if (len >= 4) {
		char *tail = exe_dir + len - 4;
		if ((tail[0] == 'b' || tail[0] == 'B') &&
		    (tail[1] == 'i' || tail[1] == 'I') &&
		    (tail[2] == 'n' || tail[2] == 'N') &&
		    (tail[3] == '\\' || tail[3] == '/')) {
			tail[0] = '\0';
		}
	}

	snprintf(buf, buf_size, "%sDisplayXR_win64.json", exe_dir);
	if (GetFileAttributesA(buf) != INVALID_FILE_ATTRIBUTES) {
		return true;
	}

	PE("Warning: no runtime manifest found, apps may fail to connect\n");
	return false;
}

// Write everything before the last path separator of @p path into @p out.
// If there's no separator, @p out is set to "." (current dir). Used to derive
// lpCurrentDirectory for CreateProcessA so launched apps land with their
// install dir as CWD, matching Explorer-launched behavior.
static void
dirname_of(const char *path, char *out, size_t out_size)
{
	if (out_size == 0) return;
	out[0] = '\0';
	if (path == NULL || !*path) {
		snprintf(out, out_size, ".");
		return;
	}

	const char *last_bs = strrchr(path, '\\');
	const char *last_fs = strrchr(path, '/');
	const char *last = last_bs > last_fs ? last_bs : last_fs;
	if (last == NULL) {
		snprintf(out, out_size, ".");
		return;
	}

	size_t len = (size_t)(last - path);
	if (len >= out_size) len = out_size - 1;
	memcpy(out, path, len);
	out[len] = '\0';
}

// Ensure the Windows Per-App Graphics Settings registry has a "high
// performance" preference for @p exe_path, matching what Settings → System →
// Display → Graphics writes. DXGI honours this at process start, which puts
// the launched app on the same adapter as the (dGPU-pinned) service. Without
// it, third-party apps that don't carry NvOptimusEnablement land on the iGPU
// on hybrid laptops, and the IPC shared swapchain texture renders black on
// the service side because shared textures can't bridge two adapters.
//
// Idempotent. Only writes if the entry is missing or empty so a user-set
// "Power saving" preference is respected.
//
// Reference: docs.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-1-6-improvements
static void
ensure_app_gpu_pref_high(const char *exe_path)
{
	const char *kSubkey = "Software\\Microsoft\\DirectX\\UserGpuPreferences";
	HKEY hkey = NULL;
	DWORD disposition = 0;
	LSTATUS rc = RegCreateKeyExA(HKEY_CURRENT_USER, kSubkey, 0, NULL,
	                             REG_OPTION_NON_VOLATILE, KEY_READ | KEY_WRITE,
	                             NULL, &hkey, &disposition);
	if (rc != ERROR_SUCCESS || hkey == NULL) {
		PE("ensure_app_gpu_pref_high: RegCreateKeyEx failed (%ld) for %s\n",
		   (long)rc, exe_path);
		return;
	}

	// Read existing value first — respect any pre-existing preference, even
	// "power saving". Only write when missing/empty.
	char existing[256] = {0};
	DWORD existing_size = sizeof(existing);
	DWORD existing_type = 0;
	rc = RegQueryValueExA(hkey, exe_path, NULL, &existing_type,
	                      (LPBYTE)existing, &existing_size);
	bool already_set = (rc == ERROR_SUCCESS && existing_type == REG_SZ &&
	                    existing_size > 1);
	if (!already_set) {
		const char *value = "GpuPreference=2;"; // 2 = high-performance / dGPU
		rc = RegSetValueExA(hkey, exe_path, 0, REG_SZ, (const BYTE *)value,
		                    (DWORD)strlen(value) + 1);
		if (rc == ERROR_SUCCESS) {
			P("Registered high-perf GPU preference for %s\n", exe_path);
		} else {
			PE("ensure_app_gpu_pref_high: RegSetValueEx failed (%ld) for %s\n",
			   (long)rc, exe_path);
		}
	}

	RegCloseKey(hkey);
}

// Find a top-level main window owned by the given PID. Used to apply alpha=0
// to Unity's HWND after launch — Unity must think its window is visible (so
// its render loop runs) but the user shouldn't see it.
struct find_pid_hwnd_ctx {
	DWORD pid;
	HWND hwnd;
};

static BOOL CALLBACK
find_pid_hwnd_proc(HWND hwnd, LPARAM lparam)
{
	struct find_pid_hwnd_ctx *c = (struct find_pid_hwnd_ctx *)lparam;
	DWORD wpid = 0;
	GetWindowThreadProcessId(hwnd, &wpid);
	if (wpid != c->pid)
		return TRUE;
	if (GetWindow(hwnd, GW_OWNER) != NULL)
		return TRUE; // skip child/owned windows
	if (!IsWindowVisible(hwnd))
		return TRUE;
	RECT rc;
	if (!GetClientRect(hwnd, &rc))
		return TRUE;
	if ((rc.right - rc.left) < 100)
		return TRUE; // skip tiny helper windows
	c->hwnd = hwnd;
	return FALSE;
}

static HWND
wait_for_main_hwnd(DWORD pid, int timeout_ms)
{
	struct find_pid_hwnd_ctx ctx = {0};
	ctx.pid = pid;
	int elapsed = 0;
	while (elapsed < timeout_ms) {
		ctx.hwnd = NULL;
		EnumWindows(find_pid_hwnd_proc, (LPARAM)&ctx);
		if (ctx.hwnd != NULL)
			return ctx.hwnd;
		Sleep(50);
		elapsed += 50;
	}
	return NULL;
}

/*!
 * Launch an app with DISPLAYXR_WORKSPACE_SESSION=1 and XR_RUNTIME_JSON set.
 */
static bool
launch_app(struct app_entry *app, const char *runtime_json)
{
	// Build environment block: inherit current env + add our vars
	// Use SetEnvironmentVariable before CreateProcess with NULL env
	// (simpler than building a full env block)
	if (runtime_json != NULL) {
		SetEnvironmentVariableA("XR_RUNTIME_JSON", runtime_json);
	}
	SetEnvironmentVariableA("DISPLAYXR_WORKSPACE_SESSION", "1");

	// Resolve to absolute path (relative paths fail with CreateProcessA)
	char abs_path[MAX_PATH];
	if (_fullpath(abs_path, app->exe_path, MAX_PATH) == NULL) {
		PE("Failed to resolve path: %s\n", app->exe_path);
		return false;
	}

	// Force the launched app onto the same adapter as the dGPU-pinned
	// service so the IPC shared swapchain texture works on hybrid laptops.
	// See ensure_app_gpu_pref_high for why this matters.
	ensure_app_gpu_pref_high(abs_path);

	// CWD = exe's install dir (matches Explorer behavior). DisplayXRClient.dll
	// drops displayxr.log relative to CWD; without this it would land
	// wherever the shell happened to be launched from.
	char exe_dir[MAX_PATH];
	dirname_of(abs_path, exe_dir, sizeof(exe_dir));

	// Quote the exe path in case of spaces
	char cmd[MAX_PATH + 16];
	snprintf(cmd, sizeof(cmd), "\"%s\"", abs_path);

	// Phase 6.1 (#140): start the app with its window hidden by default. In
	// shell mode the app's HWND on the 3D display disrupts the SR weaver and
	// causes a multi-second stretch artifact. The app's content is captured
	// via shared handles into the multi-compositor atlas, so its own window
	// doesn't need to be visible.
	//
	// Exception — Unity: Unity's render loop is gated on Win32 visibility
	// regardless of Application.runInBackground; with SW_HIDE Unity stops
	// calling xrEndFrame and the shell tile gets no content. Detect Unity by
	// the presence of UnityPlayer.dll next to the exe (ships with every Unity
	// standalone build) and show those windows un-activated instead so they
	// keep rendering while the shell stays in the foreground.
	char unity_marker[MAX_PATH];
	snprintf(unity_marker, sizeof(unity_marker), "%s\\UnityPlayer.dll", exe_dir);
	BOOL is_unity = (GetFileAttributesA(unity_marker) != INVALID_FILE_ATTRIBUTES);

	STARTUPINFOA si = {0};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = is_unity ? SW_SHOWNOACTIVATE : SW_HIDE;
	PROCESS_INFORMATION pi = {0};

	BOOL ok = CreateProcessA(
	    NULL, cmd, NULL, NULL, FALSE,
	    CREATE_NEW_CONSOLE,  // Each app gets its own console
	    NULL,                // Inherit our (modified) environment
	    exe_dir, &si, &pi);

	// Clean up env vars so they don't leak to future CreateProcess calls
	// (though it doesn't matter since we set them every time)

	if (ok) {
		app->process = pi.hProcess;
		app->pid = pi.dwProcessId;
		P("Launched: %s (PID %lu)\n", app->exe_path, pi.dwProcessId);
		CloseHandle(pi.hThread);

		// Unity needs its window to stay "visible" to keep rendering, but
		// the user shouldn't see it on the desktop. Mark the main HWND as a
		// layered window with alpha=0 once Unity has created it.
		if (is_unity) {
			HWND hwnd = wait_for_main_hwnd(pi.dwProcessId, 5000);
			if (hwnd != NULL) {
				LONG_PTR ex = GetWindowLongPtrA(hwnd, GWL_EXSTYLE);
				SetWindowLongPtrA(hwnd, GWL_EXSTYLE,
				                  ex | WS_EX_LAYERED | WS_EX_TRANSPARENT);
				SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
				P("Unity main HWND %p marked alpha=0 (kept rendering, hidden from view)\n",
				  (void *)hwnd);
			} else {
				PE("Unity main HWND not found within 5s for PID %lu — window will stay visible\n",
				   pi.dwProcessId);
			}
		}
		return true;
	} else {
		PE("Failed to launch %s: error %lu\n", app->exe_path, GetLastError());
		return false;
	}
}
#endif // _WIN32

static void
print_usage(void)
{
	P("Usage: displayxr-shell [options] [--pose x,y,z,w,h] app1.exe ...\n");
	P("\n");
	P("Options:\n");
	P("  --pose x,y,z,w,h       Set window pose for the next app argument\n");
	P("                          x,y,z = position (meters from display center)\n");
	P("                          w,h = window width and height (meters)\n");
	P("  --capture-hwnd <hwnd>   Capture a 2D window by its HWND (decimal or 0x hex)\n");
	P("                          Can be specified multiple times\n");
	P("  --help                  Show this help\n");
	P("\n");
	P("If no apps are specified, runs in monitor-only mode.\n");
	P("The service (displayxr-service --shell) is auto-started if needed.\n");
}

static int
parse_args(int argc, char *argv[], struct app_entry *apps, int *app_count,
           struct capture_entry *captures, int *capture_count)
{
	*app_count = 0;
	*capture_count = 0;
	bool next_has_pose = false;
	float px = 0, py = 0, pz = 0, pw = 0, ph = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			print_usage();
			return -1;
		}
		if (strcmp(argv[i], "--service-managed") == 0) {
			g_service_managed = true;
			continue;
		}
		if (strcmp(argv[i], "--pose") == 0) {
			if (i + 1 >= argc) {
				PE("Error: --pose requires argument x,y,z,w,h\n");
				return -1;
			}
			i++;
			int n = sscanf(argv[i], "%f,%f,%f,%f,%f", &px, &py, &pz, &pw, &ph);
			if (n < 5) {
				PE("Error: --pose needs 5 comma-separated values (x,y,z,w,h), got %d\n", n);
				return -1;
			}
			next_has_pose = true;
			continue;
		}
		if (strcmp(argv[i], "--capture-hwnd") == 0) {
			if (i + 1 >= argc) {
				PE("Error: --capture-hwnd requires HWND value\n");
				return -1;
			}
			i++;
			if (*capture_count >= MAX_CAPTURES) {
				PE("Warning: max %d captures, ignoring %s\n", MAX_CAPTURES, argv[i]);
				continue;
			}
			struct capture_entry *ce = &captures[*capture_count];
			memset(ce, 0, sizeof(*ce));
			ce->hwnd = (uint64_t)strtoull(argv[i], NULL, 0); // supports decimal and 0x hex
#ifdef _WIN32
			{
				char title[128] = "Captured Window";
				GetWindowTextA((HWND)(uintptr_t)ce->hwnd, title, sizeof(title));
				snprintf(ce->name, sizeof(ce->name), "%s", title);
			}
#else
			snprintf(ce->name, sizeof(ce->name), "Capture HWND %llu", (unsigned long long)ce->hwnd);
#endif
			(*capture_count)++;
			continue;
		}
		if (strcmp(argv[i], "--") == 0) {
			continue;
		}

		// This is an app path
		if (*app_count >= MAX_APPS) {
			PE("Warning: max %d apps, ignoring %s\n", MAX_APPS, argv[i]);
			continue;
		}
		struct app_entry *a = &apps[*app_count];
		memset(a, 0, sizeof(*a));
		a->exe_path = argv[i];
		if (next_has_pose) {
			a->has_pose = true;
			a->px = px;
			a->py = py;
			a->pz = pz;
			a->width_m = pw;
			a->height_m = ph;
			next_has_pose = false;
		}
		(*app_count)++;
	}
	return 0;
}

static void
try_apply_poses(struct app_entry *apps, int app_count,
                uint32_t *prev_ids, uint32_t prev_count)
{
	// Get current client list
	XrWorkspaceClientId client_ids[SHELL_MAX_CLIENTS];
	uint32_t client_count = shell_enumerate_clients(client_ids, SHELL_MAX_CLIENTS);
	if (client_count == 0) {
		return;
	}

	// Find new clients (IDs not in prev_ids)
	for (uint32_t i = 0; i < client_count; i++) {
		uint32_t id = client_ids[i];
		bool is_new = true;
		for (uint32_t j = 0; j < prev_count; j++) {
			if (prev_ids[j] == id) {
				is_new = false;
				break;
			}
		}
		if (!is_new) {
			continue;
		}

		// Find the first app that hasn't had its pose applied yet
		for (int a = 0; a < app_count; a++) {
			if (apps[a].has_pose && !apps[a].pose_applied) {
				XrPosef xrpose;
				xrpose.orientation.x = 0.0f;
				xrpose.orientation.y = 0.0f;
				xrpose.orientation.z = 0.0f;
				xrpose.orientation.w = 1.0f;
				xrpose.position.x = apps[a].px;
				xrpose.position.y = apps[a].py;
				xrpose.position.z = apps[a].pz;
				XrResult r = g_xr->set_pose(
				    g_xr->session, id, &xrpose,
				    apps[a].width_m, apps[a].height_m);
				if (r == XR_SUCCESS) {
					P("Applied pose to client %u: pos=(%.3f,%.3f,%.3f) size=%.3fx%.3f\n",
					  id, apps[a].px, apps[a].py, apps[a].pz,
					  apps[a].width_m, apps[a].height_m);
				}
				apps[a].pose_applied = true;
				break;
			}
		}
	}
}

static void
print_clients(uint32_t *prev_ids, uint32_t *prev_count)
{
	XrWorkspaceClientId client_ids[SHELL_MAX_CLIENTS];
	uint32_t client_count = shell_enumerate_clients(client_ids, SHELL_MAX_CLIENTS);
	if (client_count == 0 && *prev_count == 0) {
		return;
	}

	// Detect changes
	bool changed = (client_count != *prev_count);
	if (!changed) {
		for (uint32_t i = 0; i < client_count; i++) {
			if (client_ids[i] != prev_ids[i]) {
				changed = true;
				break;
			}
		}
	}

	if (!changed) {
		return;
	}

	// Print current state
	P("\n--- %u client(s) connected ---\n", client_count);
	for (uint32_t i = 0; i < client_count; i++) {
		XrWorkspaceClientInfoEXT cinfo;
		if (!shell_get_client_info(client_ids[i], &cinfo)) {
			P("  [%u] (failed to get info)\n", (unsigned)client_ids[i]);
			continue;
		}
		P("  [%u] %s (PID %llu)\n", (unsigned)client_ids[i], cinfo.name,
		  (unsigned long long)cinfo.pid);
	}

	// Update previous state
	*prev_count = client_count;
	for (uint32_t i = 0; i < client_count; i++) {
		prev_ids[i] = client_ids[i];
	}
}

#ifdef _WIN32
/*
 * Window enumeration for auto-adoption of desktop windows.
 */

/*!
 * Find the PID of displayxr-service.exe (to exclude its windows from adoption).
 */
static DWORD
find_service_pid(void)
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) return 0;

	PROCESSENTRY32 pe = {0};
	pe.dwSize = sizeof(pe);
	DWORD pid = 0;
	if (Process32First(snap, &pe)) {
		do {
			if (_stricmp(pe.szExeFile, "displayxr-service.exe") == 0) {
				pid = pe.th32ProcessID;
				break;
			}
		} while (Process32Next(snap, &pe));
	}
	CloseHandle(snap);
	return pid;
}

struct enum_ctx
{
	HWND found[MAX_CAPTURES];
	int count;
	DWORD shell_pid;   // our own process
	DWORD service_pid; // displayxr-service process
};

static BOOL CALLBACK
enum_windows_cb(HWND hwnd, LPARAM lParam)
{
	struct enum_ctx *ctx = (struct enum_ctx *)lParam;
	if (ctx->count >= MAX_CAPTURES) {
		return FALSE;
	}

	if (!IsWindowVisible(hwnd)) {
		return TRUE;
	}

	// Skip owned popups
	if (GetWindow(hwnd, GW_OWNER) != NULL) {
		return TRUE;
	}

	// Skip our own process and service process windows
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == ctx->shell_pid || pid == ctx->service_pid) {
		return TRUE;
	}

	// Skip known system window classes
	char class_name[256] = {0};
	GetClassNameA(hwnd, class_name, sizeof(class_name));
	if (strcmp(class_name, "Shell_TrayWnd") == 0 ||
	    strcmp(class_name, "Progman") == 0 ||
	    strcmp(class_name, "WorkerW") == 0 ||
	    strcmp(class_name, "Button") == 0 ||
	    strcmp(class_name, "NotifyIconOverflowWindow") == 0 ||
	    strcmp(class_name, "Windows.UI.Core.CoreWindow") == 0 ||
	    strcmp(class_name, "Shell_SecondaryTrayWnd") == 0 ||
	    strcmp(class_name, "XamlExplorerHostIslandWindow") == 0 ||
	    strcmp(class_name, "TopLevelWindowForOverflowXamlIsland") == 0) {
		return TRUE;
	}

	// Skip tiny windows (tooltips, helpers, hidden overlays)
	RECT rect;
	GetClientRect(hwnd, &rect);
	if ((rect.right - rect.left) < 100 || (rect.bottom - rect.top) < 100) {
		return TRUE;
	}

	// Skip windows with WS_EX_TOOLWINDOW (already hidden from taskbar)
	LONG_PTR exstyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
	if (exstyle & WS_EX_TOOLWINDOW) {
		return TRUE;
	}

	ctx->found[ctx->count++] = hwnd;
	return TRUE;
}

/*!
 * Enumerate visible desktop windows and adopt new ones as capture clients.
 * Skips windows already tracked in the captures array and IPC clients.
 */
static void
enumerate_and_adopt_windows(struct capture_entry *captures,
                            int *capture_count,
                            DWORD service_pid)
{
	// Enumerate visible top-level windows
	struct enum_ctx ctx = {0};
	ctx.shell_pid = GetCurrentProcessId();
	ctx.service_pid = service_pid;
	EnumWindows(enum_windows_cb, (LPARAM)&ctx);

	// Get current OpenXR client PIDs to skip already-attached 3D apps.
	DWORD ipc_pids[SHELL_MAX_CLIENTS] = {0};
	int ipc_pid_count = 0;
	{
		XrWorkspaceClientId client_ids[SHELL_MAX_CLIENTS];
		uint32_t client_count = shell_enumerate_clients(client_ids, SHELL_MAX_CLIENTS);
		for (uint32_t c = 0; c < client_count; c++) {
			if (client_ids[c] == XR_NULL_WORKSPACE_CLIENT_ID) continue;
			XrWorkspaceClientInfoEXT cinfo;
			if (shell_get_client_info(client_ids[c], &cinfo)) {
				ipc_pids[ipc_pid_count++] = (DWORD)cinfo.pid;
				if (ipc_pid_count >= SHELL_MAX_CLIENTS) break;
			}
		}
	}

	for (int i = 0; i < ctx.count; i++) {
		HWND hwnd = ctx.found[i];
		uint64_t hwnd_val = (uint64_t)(uintptr_t)hwnd;

		// Skip if already tracked
		bool already_tracked = false;
		for (int j = 0; j < *capture_count; j++) {
			if (captures[j].hwnd == hwnd_val && captures[j].added) {
				already_tracked = true;
				break;
			}
		}
		if (already_tracked) continue;

		// Skip if this HWND belongs to an IPC client process
		DWORD hwnd_pid = 0;
		GetWindowThreadProcessId(hwnd, &hwnd_pid);
		bool is_ipc = false;
		for (int j = 0; j < ipc_pid_count; j++) {
			if (ipc_pids[j] == hwnd_pid) {
				is_ipc = true;
				break;
			}
		}
		if (is_ipc) continue;

		// Adopt this window
		if (*capture_count >= MAX_CAPTURES) {
			P("  Auto-adopt: max captures (%d) reached\n", MAX_CAPTURES);
			break;
		}

		struct capture_entry *ce = &captures[*capture_count];
		memset(ce, 0, sizeof(*ce));
		ce->hwnd = hwnd_val;
		{
			char title[128] = "Captured Window";
			GetWindowTextA(hwnd, title, sizeof(title));
			snprintf(ce->name, sizeof(ce->name), "%s", title);
		}

		uint32_t cid = 0;
		XrResult r = g_xr->add_capture(g_xr->session, ce->hwnd, NULL, &cid);
		if (r == XR_SUCCESS) {
			ce->client_id = cid;
			ce->added = true;
			(*capture_count)++;
			P("  Auto-adopt: HWND=0x%llx '%s' → client_id=%u\n",
			  (unsigned long long)ce->hwnd, ce->name, cid);
		} else {
			P("  Auto-adopt: failed for HWND=0x%llx '%s'\n",
			  (unsigned long long)ce->hwnd, ce->name);
		}
	}
}

/*!
 * Check for closed capture windows and remove their capture clients.
 */
static void
cleanup_closed_captures(struct capture_entry *captures,
                        int *capture_count)
{
	for (int i = 0; i < *capture_count; i++) {
		if (!captures[i].added) continue;
		if (!IsWindow((HWND)(uintptr_t)captures[i].hwnd)) {
			P("  Window closed: '%s' (client_id=%u)\n", captures[i].name, captures[i].client_id);
			(void)g_xr->remove_capture(g_xr->session, captures[i].client_id);
			// Shift remaining entries
			for (int j = i; j < *capture_count - 1; j++) {
				captures[j] = captures[j + 1];
			}
			(*capture_count)--;
			i--; // Re-check this index
		}
	}
}

// --- 5.6: Running-app set (for launcher "running" tag) ---

#define SHELL_RUNNING_NAMES_MAX 32

struct shell_running_set
{
	int count;
	char names[SHELL_RUNNING_NAMES_MAX][128];
};

/*!
 * Snapshot the set of application_name strings for every workspace client
 * currently connected. Used by the launcher to highlight tiles whose app is
 * already running. Cheap enough to call every time the launcher opens; not
 * intended for per-frame use.
 *
 * On error, returns an empty set — the launcher should degrade to "no running
 * apps" rather than failing to open.
 */
static void
shell_get_running_app_set(struct shell_running_set *out)
{
	out->count = 0;

	XrWorkspaceClientId client_ids[SHELL_MAX_CLIENTS];
	uint32_t client_count = shell_enumerate_clients(client_ids, SHELL_MAX_CLIENTS);
	if (client_count == 0) {
		return;
	}

	for (uint32_t c = 0; c < client_count; c++) {
		if (client_ids[c] == XR_NULL_WORKSPACE_CLIENT_ID) continue;
		if (out->count >= SHELL_RUNNING_NAMES_MAX) break;

		XrWorkspaceClientInfoEXT cinfo;
		if (!shell_get_client_info(client_ids[c], &cinfo)) continue;
		if (cinfo.name[0] == '\0') continue;

		// Deduplicate — two instances of the same app are one tile in the
		// launcher (for highlight purposes).
		bool already = false;
		for (int i = 0; i < out->count; i++) {
			if (strcmp(out->names[i], cinfo.name) == 0) {
				already = true;
				break;
			}
		}
		if (already) continue;

		snprintf(out->names[out->count], sizeof(out->names[0]), "%s",
		         cinfo.name);
		out->count++;
	}
}

/*!
 * True if @p app_name matches any currently-running client in @p set. The
 * comparison is case-sensitive and expects the application_name as passed to
 * xrCreateInstance.
 */
static bool
shell_running_set_contains(const struct shell_running_set *set, const char *app_name)
{
	if (set == NULL || app_name == NULL || !*app_name) return false;
	for (int i = 0; i < set->count; i++) {
		if (strcmp(set->names[i], app_name) == 0) return true;
	}
	return false;
}

/*!
 * Phase 5.11: case-insensitive normalized exe-path equality. Treats
 * forward slashes as backslashes so registry-stored paths match what
 * QueryFullProcessImageNameW returns.
 */
static bool
shell_exe_paths_equal(const char *a, const char *b)
{
	if (a == NULL || b == NULL) return a == b;
	while (*a && *b) {
		int ca = (unsigned char)*a;
		int cb = (unsigned char)*b;
		if (ca == '/') ca = '\\';
		if (cb == '/') cb = '\\';
		if (ca >= 'A' && ca <= 'Z') ca += 32;
		if (cb >= 'A' && cb <= 'Z') cb += 32;
		if (ca != cb) return false;
		a++; b++;
	}
	return *a == '\0' && *b == '\0';
}

#ifdef _WIN32
/*!
 * Phase 5.11: resolve a PID to its absolute exe path via
 * QueryFullProcessImageNameW. Writes a UTF-8 path into @p out, returns true
 * on success.
 */
static bool
shell_pid_to_exe_path(DWORD pid, char *out, size_t out_size)
{
	if (pid == 0 || out == NULL || out_size == 0) return false;

	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (h == NULL) return false;

	wchar_t wpath[MAX_PATH] = {0};
	DWORD wlen = MAX_PATH;
	BOOL ok = QueryFullProcessImageNameW(h, 0, wpath, &wlen);
	CloseHandle(h);
	if (!ok || wlen == 0) return false;

	int n = WideCharToMultiByte(CP_UTF8, 0, wpath, (int)wlen, out, (int)out_size - 1, NULL, NULL);
	if (n <= 0) return false;
	out[n] = '\0';
	return true;
}
#endif

/*!
 * Phase 5.11: build a bitmask of registered apps that have at least one
 * matching IPC client currently connected. Bit @c i set means
 * g_registered_apps[i].exe_path equals (case-insensitive, slash-normalized)
 * the exe path of some IPC client's PID.
 *
 * Returns 0 on Windows if no clients are connected; non-Windows always 0.
 */
static uint64_t
shell_compute_running_tile_mask(void)
{
#ifdef _WIN32
	uint64_t mask = 0;

	XrWorkspaceClientId client_ids[SHELL_MAX_CLIENTS];
	uint32_t client_count = shell_enumerate_clients(client_ids, SHELL_MAX_CLIENTS);
	if (client_count == 0) {
		return 0;
	}

	// Collect each connected client's exe path once.
	char client_exes[SHELL_MAX_CLIENTS][MAX_PATH];
	int client_exe_count = 0;
	for (uint32_t c = 0; c < client_count; c++) {
		if (client_ids[c] == XR_NULL_WORKSPACE_CLIENT_ID) continue;
		XrWorkspaceClientInfoEXT cinfo;
		if (!shell_get_client_info(client_ids[c], &cinfo)) continue;
		if (cinfo.pid == 0) continue;
		if (shell_pid_to_exe_path((DWORD)cinfo.pid, client_exes[client_exe_count],
		                          sizeof(client_exes[0]))) {
			client_exe_count++;
			if (client_exe_count >= SHELL_MAX_CLIENTS) break;
		}
	}

	// Match each registered app against the connected exe set.
	int cap = g_registered_app_count;
	if (cap > 64) cap = 64; // mask is uint64_t
	for (int i = 0; i < cap; i++) {
		const char *reg_exe = g_registered_apps[i].exe_path;
		for (int j = 0; j < client_exe_count; j++) {
			if (shell_exe_paths_equal(reg_exe, client_exes[j])) {
				mask |= (1ULL << i);
				break;
			}
		}
	}

	return mask;
#else
	return 0;
#endif
}

/*!
 * Phase 5.8: ship the current g_registered_apps[] array to the service so
 * the spatial launcher panel can render its tile grid. Uses a clear+add
 * pattern (one IPC call per app) so each message stays under IPC_BUF_SIZE.
 * The shell must re-call this whenever the registry changes (browse-for-app,
 * remove, refresh).
 */
static void
shell_push_registered_apps_to_service(void)
{
	XrResult r = g_xr->clear_launcher(g_xr->session);
	if (r != XR_SUCCESS) {
		PE("xrClearLauncherAppsEXT failed: %d\n", r);
		return;
	}

	int pushed = 0;
	int cap = g_registered_app_count;
	if (cap > XR_LAUNCHER_MAX_APPS_EXT) {
		cap = XR_LAUNCHER_MAX_APPS_EXT;
	}
	for (int i = 0; i < cap; i++) {
		const struct registered_app *src = &g_registered_apps[i];

		XrLauncherAppInfoEXT info;
		memset(&info, 0, sizeof(info));
		info.type = XR_TYPE_LAUNCHER_APP_INFO_EXT;
		snprintf(info.name, sizeof(info.name), "%s", src->name);
		snprintf(info.iconPath, sizeof(info.iconPath), "%s", src->icon_path);
		snprintf(info.appType, sizeof(info.appType), "%s", src->type);
		snprintf(info.iconPath3D, sizeof(info.iconPath3D), "%s", src->icon_3d_path);
		snprintf(info.iconLayout3D, sizeof(info.iconLayout3D), "%s", src->icon_3d_layout);
		info.appIndex = i;

		r = g_xr->add_launcher_app(g_xr->session, &info);
		if (r != XR_SUCCESS) {
			PE("xrAddLauncherAppEXT[%d] failed: %d\n", i, r);
			return;
		}
		pushed++;
	}
	P("Pushed %d app(s) to launcher.\n", pushed);
}

#ifdef _WIN32

// Get the per-user registered-mode discovery dir, e.g.
// "C:\Users\foo\AppData\Local\DisplayXR\apps". Creates intermediate dirs.
// Returns true on success.
static bool
get_registered_apps_dir(char *out, size_t out_size)
{
	const char *local_appdata = getenv("LOCALAPPDATA");
	if (local_appdata == NULL || !*local_appdata) return false;

	char base[MAX_PATH];
	snprintf(base, sizeof(base), "%s\\DisplayXR", local_appdata);
	CreateDirectoryA(base, NULL);

	snprintf(out, out_size, "%s\\apps", base);
	CreateDirectoryA(out, NULL);
	return true;
}

// Replace path separators and any non-portable characters with '_'. Used to
// derive a manifest filename from an exe basename.
static void
sanitize_basename(const char *src, char *dst, size_t dst_size)
{
	size_t i = 0;
	for (; src[i] && i + 1 < dst_size; i++) {
		char c = src[i];
		bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
		dst[i] = ok ? c : '_';
	}
	dst[i] = '\0';
}

// Extract the largest available app icon from @p exe and write it as PNG to
// @p out_png. Uses PrivateExtractIconsA to request the largest standard size
// (256x256), then DrawIconEx onto a 32-bit DIB so PNG-compressed Vista-era
// icons render correctly. Returns true on success.
static bool
extract_pe_icon_to_png(const char *exe, const char *out_png)
{
	const int target = 256;
	HICON hicon = NULL;
	UINT extracted = PrivateExtractIconsA(exe, 0, target, target, &hicon, NULL, 1, 0);
	if (extracted == 0 || extracted == (UINT)-1 || hicon == NULL) {
		// Fall back to the system "large icon" size (32x32 on classic
		// Windows). Better than nothing.
		extracted = PrivateExtractIconsA(exe, 0, GetSystemMetrics(SM_CXICON),
		                                 GetSystemMetrics(SM_CYICON), &hicon,
		                                 NULL, 1, 0);
		if (extracted == 0 || extracted == (UINT)-1 || hicon == NULL) {
			return false;
		}
	}

	// Determine icon dimensions from the actual HICON (PrivateExtractIcons
	// may have given us the largest available size, not 256).
	ICONINFO ii = {0};
	if (!GetIconInfo(hicon, &ii)) {
		DestroyIcon(hicon);
		return false;
	}
	BITMAP bm = {0};
	int w = target, h = target;
	if (ii.hbmColor && GetObject(ii.hbmColor, sizeof(bm), &bm)) {
		w = bm.bmWidth;
		h = bm.bmHeight;
	}
	if (ii.hbmMask) DeleteObject(ii.hbmMask);
	if (ii.hbmColor) DeleteObject(ii.hbmColor);

	// Render the icon onto a 32bpp top-down DIB. DrawIconEx handles alpha
	// for both Vista PNG icons and classic AND/XOR-mask icons.
	BITMAPINFO bi = {0};
	bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;     // negative → top-down
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;

	HDC screen_dc = GetDC(NULL);
	HDC mem_dc = CreateCompatibleDC(screen_dc);
	void *bits = NULL;
	HBITMAP dib = CreateDIBSection(screen_dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
	ReleaseDC(NULL, screen_dc);

	if (dib == NULL || bits == NULL) {
		if (dib) DeleteObject(dib);
		DeleteDC(mem_dc);
		DestroyIcon(hicon);
		return false;
	}

	HGDIOBJ prev = SelectObject(mem_dc, dib);
	memset(bits, 0, (size_t)w * (size_t)h * 4);  // transparent background
	BOOL drew = DrawIconEx(mem_dc, 0, 0, hicon, w, h, 0, NULL, DI_NORMAL);
	SelectObject(mem_dc, prev);
	DeleteDC(mem_dc);
	DestroyIcon(hicon);

	if (!drew) {
		DeleteObject(dib);
		return false;
	}

	// DIB layout is BGRA; stb expects RGBA, so swap channels in place.
	uint8_t *px = (uint8_t *)bits;
	for (int i = 0; i < w * h; i++) {
		uint8_t b = px[i * 4 + 0];
		px[i * 4 + 0] = px[i * 4 + 2];
		px[i * 4 + 2] = b;
	}

	int ok = stbi_write_png(out_png, w, h, 4, bits, w * 4);
	DeleteObject(dib);
	return ok != 0;
}

/*!
 * Browse-for-app: prompt for an executable, write a registered-mode manifest
 * (`<sanitized>.displayxr.json` with `exe_path`) into
 * `%LOCALAPPDATA%\DisplayXR\apps\`, extract the embedded PE icon as a sibling
 * `<sanitized>.png`, and re-trigger discovery so the new tile shows up. See
 * docs/specs/displayxr-app-manifest.md §11.
 *
 * Called from the launcher click-poll branch when the service signals a
 * Browse-tile hit (IPC_LAUNCHER_ACTION_BROWSE). The file dialog is modal and
 * blocks the shell's message loop; on return the launcher is re-shown so the
 * user sees their new tile.
 */
static void
shell_browse_and_add_app(void)
{
	// Open the file dialog as a top-level window (hwndOwner=NULL). The
	// service granted ASFW_ANY foreground permission when it processed the
	// Browse click, so Windows lets the modal dialog activate normally.
	char exe_path[MAX_PATH] = {0};
	OPENFILENAMEA ofn = {0};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = NULL;
	ofn.lpstrFilter = "Executables (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
	ofn.lpstrFile = exe_path;
	ofn.nMaxFile = MAX_PATH;
	ofn.lpstrTitle = "Add app to DisplayXR launcher";
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

	if (!GetOpenFileNameA(&ofn)) {
		P("Browse: canceled\n");
		return;
	}

	// Derive a display name from the exe basename (strip path and .exe).
	const char *base = strrchr(exe_path, '\\');
	if (base == NULL) base = strrchr(exe_path, '/');
	base = base ? base + 1 : exe_path;
	char name[128];
	snprintf(name, sizeof(name), "%s", base);
	size_t nlen = strlen(name);
	if (nlen >= 4 && _stricmp(name + nlen - 4, ".exe") == 0) {
		name[nlen - 4] = '\0';
	}

	char sanitized[128];
	sanitize_basename(name, sanitized, sizeof(sanitized));

	char dir[MAX_PATH];
	if (!get_registered_apps_dir(dir, sizeof(dir))) {
		PE("Browse: cannot resolve %%LOCALAPPDATA%%\\DisplayXR\\apps\n");
		return;
	}

	char manifest_path[MAX_PATH];
	snprintf(manifest_path, sizeof(manifest_path), "%s\\%s.displayxr.json", dir,
	         sanitized);
	char icon_path[MAX_PATH];
	snprintf(icon_path, sizeof(icon_path), "%s\\%s.png", dir, sanitized);

	bool have_icon = extract_pe_icon_to_png(exe_path, icon_path);
	if (!have_icon) {
		P("Browse: PE icon extraction failed for %s — manifest will be iconless\n",
		  exe_path);
	}

	// Build the manifest JSON via cJSON for proper escaping of paths
	// containing backslashes, spaces, etc.
	cJSON *root = cJSON_CreateObject();
	cJSON_AddNumberToObject(root, "schema_version", 1);
	cJSON_AddStringToObject(root, "name", name);
	cJSON_AddStringToObject(root, "type", "3d");
	cJSON_AddStringToObject(root, "category", "app");
	cJSON_AddStringToObject(root, "exe_path", exe_path);
	if (have_icon) {
		// Icon path is relative to the manifest, per §2.3.
		char icon_rel[256];
		snprintf(icon_rel, sizeof(icon_rel), "%s.png", sanitized);
		cJSON_AddStringToObject(root, "icon", icon_rel);
	}

	char *json_str = cJSON_Print(root);
	cJSON_Delete(root);
	if (json_str == NULL) {
		PE("Browse: cJSON_Print failed\n");
		return;
	}

	FILE *f = fopen(manifest_path, "wb");
	if (f == NULL) {
		PE("Browse: cannot write manifest %s\n", manifest_path);
		free(json_str);
		return;
	}
	fwrite(json_str, 1, strlen(json_str), f);
	fclose(f);
	free(json_str);

	P("Browse: wrote manifest %s\n", manifest_path);

	// Re-scan + re-push so the new tile appears.
	registered_apps_load();
	shell_push_registered_apps_to_service();
}
#else
static void
shell_browse_and_add_app(void)
{
}
#endif

// --- 4C.10+4C.11: App launch from shell + auto-detect type ---

/*!
 * Launch a registered app from the shell.
 * For "3d" type: uses launch_app() with DISPLAYXR_WORKSPACE_SESSION env.
 * For "2d" type: launches without shell env, then captures via IPC.
 * For unknown type: launches as 3d, polls for IPC connect to auto-detect.
 */
static void
shell_launch_registered_app(struct registered_app *rapp,
                            const char *runtime_json,
                            struct app_entry *apps, int *app_count,
                            struct capture_entry *captures, int *capture_count)
{
	P("Launching: '%s' (%s) — %s\n", rapp->name, rapp->exe_path, rapp->type);

	if (strcmp(rapp->type, "2d") == 0) {
		// 2D app: launch without shell env, then poll for HWND and capture
		STARTUPINFOA si = {0};
		si.cb = sizeof(si);
		PROCESS_INFORMATION pi = {0};

		char abs_path[MAX_PATH];
		char cmd[MAX_PATH + 16];
		if (_fullpath(abs_path, rapp->exe_path, MAX_PATH) == NULL) {
			snprintf(abs_path, MAX_PATH, "%s", rapp->exe_path);
		}
		snprintf(cmd, sizeof(cmd), "\"%s\"", abs_path);

		// 2D apps don't share textures, but pin them to the same adapter
		// anyway for consistency with 3D apps and to keep DXGI happy.
		ensure_app_gpu_pref_high(abs_path);

		char exe_dir[MAX_PATH];
		dirname_of(abs_path, exe_dir, sizeof(exe_dir));

		// Clear shell env vars so 2D app doesn't accidentally pick them up
		SetEnvironmentVariableA("DISPLAYXR_WORKSPACE_SESSION", NULL);

		BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
		    CREATE_NEW_CONSOLE, NULL, exe_dir, &si, &pi);
		if (!ok) {
			PE("  Failed to launch 2D app: %lu\n", GetLastError());
			return;
		}
		P("  Launched 2D app PID=%lu, waiting for HWND...\n", pi.dwProcessId);
		CloseHandle(pi.hThread);

		// Poll for HWND owned by this PID (up to 5 seconds)
		HWND found_hwnd = NULL;
		for (int poll = 0; poll < 10 && found_hwnd == NULL; poll++) {
			Sleep(500);
			struct enum_ctx ectx = {0};
			ectx.shell_pid = GetCurrentProcessId();
			ectx.service_pid = 0;
			EnumWindows(enum_windows_cb, (LPARAM)&ectx);
			for (int w = 0; w < ectx.count; w++) {
				DWORD wnd_pid = 0;
				GetWindowThreadProcessId(ectx.found[w], &wnd_pid);
				if (wnd_pid == pi.dwProcessId) {
					found_hwnd = ectx.found[w];
					break;
				}
			}
		}

		if (found_hwnd != NULL && *capture_count < MAX_CAPTURES) {
			struct capture_entry *ce = &captures[*capture_count];
			memset(ce, 0, sizeof(*ce));
			ce->hwnd = (uint64_t)(uintptr_t)found_hwnd;
			GetWindowTextA(found_hwnd, ce->name, sizeof(ce->name));

			uint32_t cid = 0;
			XrResult r = g_xr->add_capture(g_xr->session, ce->hwnd, NULL, &cid);
			if (r == XR_SUCCESS) {
				ce->client_id = cid;
				ce->added = true;
				(*capture_count)++;
				P("  Captured 2D window HWND=%p '%s' → client_id=%u\n",
				  (void *)found_hwnd, ce->name, cid);
			}
		} else if (found_hwnd == NULL) {
			PE("  2D app: no visible HWND found after 5s\n");
		}
		CloseHandle(pi.hProcess);
		return;
	}

	// "3d" or unknown type: launch as shell app
	if (*app_count >= MAX_APPS) {
		PE("  Max apps (%d) reached\n", MAX_APPS);
		return;
	}
	struct app_entry *a = &apps[*app_count];
	memset(a, 0, sizeof(*a));
	a->exe_path = rapp->exe_path;
	launch_app(a, runtime_json);
	(*app_count)++;

	// 4C.11: Auto-detect type if unknown
	if (rapp->type[0] == '\0' && a->pid != 0) {
		P("  Unknown type — detecting (polling IPC for 5s)...\n");
		bool found_ipc = false;
		for (int poll = 0; poll < 10 && !found_ipc; poll++) {
			Sleep(500);
			XrWorkspaceClientId client_ids[SHELL_MAX_CLIENTS];
			uint32_t client_count = shell_enumerate_clients(client_ids, SHELL_MAX_CLIENTS);
			for (uint32_t c = 0; c < client_count; c++) {
				XrWorkspaceClientInfoEXT cinfo;
				if (shell_get_client_info(client_ids[c], &cinfo)) {
					if ((DWORD)cinfo.pid == a->pid) {
						found_ipc = true;
						break;
					}
				}
			}
		}
		if (found_ipc) {
			snprintf(rapp->type, sizeof(rapp->type), "3d");
			P("  Auto-detected: 3D app (IPC client connected)\n");
		} else {
			snprintf(rapp->type, sizeof(rapp->type), "2d");
			P("  Auto-detected: 2D app (no IPC client after 5s)\n");
			// TODO: find HWND by PID and capture as 2D
		}
		registered_apps_save();
	}
}

/*!
 * Show a simple app launcher dialog listing registered apps.
 * Returns the index of the selected app, or -1 if cancelled.
 */
static int
shell_show_launcher_dialog(void)
{
	if (g_registered_app_count == 0) {
		P("No registered apps.\n");
		return -1;
	}

	// Build a numbered list string for the MessageBox
	char list[2048] = "Select an app to launch:\n\n";
	for (int i = 0; i < g_registered_app_count; i++) {
		char line[256];
		snprintf(line, sizeof(line), "  %d. %s [%s]\n",
		         i + 1, g_registered_apps[i].name,
		         g_registered_apps[i].type[0] ? g_registered_apps[i].type : "auto");
		strncat(list, line, sizeof(list) - strlen(list) - 1);
	}
	strncat(list, "\nEnter the number in the input box below:",
	        sizeof(list) - strlen(list) - 1);

	// Use a simple input loop: cycle through apps with repeated Ctrl+L
	// For now, just launch the first app. A proper dialog is Phase 5.
	// MessageBox to show list, then launch first unlaunched app.
	// TODO(Phase 5): Replace with spatial launcher panel.

	// Find first registered app not currently running
	// For simplicity, just return the first one.
	P("%s\n", list);
	return 0; // Launch first app
}

// --- System tray icon ---

static NOTIFYICONDATAA g_nid = {0};

// Forward declaration for the tray wnd_proc.
static void tray_show_context_menu(HWND hwnd, bool shell_active);

// Phase 2.K tray-fix: dedicated wnd_proc that intercepts WM_TRAYICON directly,
// matching the service-tray pattern (service_tray_win.c). The previous design
// used the STATIC built-in class with no wnd_proc and tried to filter
// WM_TRAYICON inline in the main PeekMessage loop — which silently dropped
// the right-click events on this Windows build. A registered class + WndProc
// is the canonical Shell_NotifyIcon approach.
static LRESULT CALLBACK
shell_tray_wnd_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_TRAYICON) {
		UINT mouse_msg = LOWORD(lParam);
		// Phase 2.K B-merged: shell is always active while the process is
		// running. Left-click and right-click both surface the context menu,
		// which only contains Exit (kills apps + shell, orchestrator
		// re-spawns on next Ctrl+Space).
		if (mouse_msg == WM_LBUTTONUP || mouse_msg == WM_RBUTTONUP ||
		    mouse_msg == WM_CONTEXTMENU) {
			tray_show_context_menu(hwnd, g_shell_active);
			return 0;
		}
		return 0;
	}
	return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void
shell_register_tray_class(void)
{
	static bool s_registered = false;
	if (s_registered) return;
	WNDCLASSEXA wcex = {0};
	wcex.cbSize = sizeof(wcex);
	wcex.lpfnWndProc = shell_tray_wnd_proc;
	wcex.hInstance = GetModuleHandleA(NULL);
	wcex.lpszClassName = "DisplayXRShellTray";
	wcex.hIcon = LoadIconA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDI_DISPLAYXR_SHELL));
	if (RegisterClassExA(&wcex) != 0) {
		s_registered = true;
	}
}

static void
tray_create(HWND hwnd)
{
	g_nid.cbSize = sizeof(NOTIFYICONDATAA);
	g_nid.hWnd = hwnd;
	g_nid.uID = 1;
	g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
	g_nid.uCallbackMessage = WM_TRAYICON;
	g_nid.hIcon = LoadIconA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDI_DISPLAYXR_SHELL));
	if (g_nid.hIcon == NULL) {
		g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
	}
	strncpy(g_nid.szTip, "DisplayXR Shell (inactive)", sizeof(g_nid.szTip) - 1);
	Shell_NotifyIconA(NIM_ADD, &g_nid);
}

static void
tray_update_tooltip(bool active)
{
	strncpy(g_nid.szTip,
	        active ? "DisplayXR Shell (active)" : "DisplayXR Shell (inactive)",
	        sizeof(g_nid.szTip) - 1);
	Shell_NotifyIconA(NIM_MODIFY, &g_nid);
}

static void
tray_destroy(void)
{
	Shell_NotifyIconA(NIM_DELETE, &g_nid);
}

static void
tray_show_context_menu(HWND hwnd, bool shell_active)
{
	(void)shell_active; // Phase 2.K B-merged: only "Exit" is offered.
	HMENU menu = CreatePopupMenu();
	if (menu == NULL) {
		return;
	}
	AppendMenuA(menu, MF_STRING, TRAY_CMD_EXIT, "Exit DisplayXR Shell");

	POINT pt;
	GetCursorPos(&pt);
	SetForegroundWindow(hwnd); // Required for TrackPopupMenu to dismiss properly
	UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
	                          pt.x, pt.y, 0, hwnd, NULL);
	DestroyMenu(menu);
	PostMessage(hwnd, WM_NULL, 0, 0); // Force dismiss

	if (cmd == TRAY_CMD_EXIT) {
		g_running = 0;
	}
}

#endif // _WIN32

#ifdef _WIN32
/*
 * Phase 8: 3D capture MVP.
 *
 * Builds an output prefix under %USERPROFILE%\Pictures\DisplayXR, asks the
 * runtime to write SBS / L / R PNGs of the current pre-weave atlas, then
 * writes a JSON sidecar with the metadata returned by the runtime.
 */
static void
capture_write_sidecar(const char *json_path, const XrWorkspaceCaptureResultEXT *r)
{
	cJSON *root = cJSON_CreateObject();
	if (root == NULL) {
		return;
	}

	cJSON_AddNumberToObject(root, "schema_version", 1);
	cJSON_AddNumberToObject(root, "timestamp_ns", (double)r->timestampNs);

	cJSON *atlas = cJSON_AddObjectToObject(root, "atlas");
	cJSON_AddNumberToObject(atlas, "width", r->atlasWidth);
	cJSON_AddNumberToObject(atlas, "height", r->atlasHeight);

	cJSON *eye = cJSON_AddObjectToObject(root, "eye");
	cJSON_AddNumberToObject(eye, "width", r->eyeWidth);
	cJSON_AddNumberToObject(eye, "height", r->eyeHeight);

	cJSON *stereo = cJSON_AddObjectToObject(root, "stereo");
	cJSON_AddNumberToObject(stereo, "tile_columns", r->tileColumns);
	cJSON_AddNumberToObject(stereo, "tile_rows", r->tileRows);

	cJSON *disp = cJSON_AddObjectToObject(root, "display_m");
	cJSON_AddNumberToObject(disp, "width", r->displayWidthM);
	cJSON_AddNumberToObject(disp, "height", r->displayHeightM);

	cJSON *le = cJSON_AddArrayToObject(root, "eye_left_m");
	cJSON_AddItemToArray(le, cJSON_CreateNumber(r->eyeLeftM[0]));
	cJSON_AddItemToArray(le, cJSON_CreateNumber(r->eyeLeftM[1]));
	cJSON_AddItemToArray(le, cJSON_CreateNumber(r->eyeLeftM[2]));

	cJSON *re = cJSON_AddArrayToObject(root, "eye_right_m");
	cJSON_AddItemToArray(re, cJSON_CreateNumber(r->eyeRightM[0]));
	cJSON_AddItemToArray(re, cJSON_CreateNumber(r->eyeRightM[1]));
	cJSON_AddItemToArray(re, cJSON_CreateNumber(r->eyeRightM[2]));

	cJSON *views = cJSON_AddArrayToObject(root, "views_written");
	if (r->viewsWritten & XR_WORKSPACE_CAPTURE_FLAG_ATLAS_BIT_EXT) {
		cJSON_AddItemToArray(views, cJSON_CreateString("atlas"));
	}

	char *text = cJSON_Print(root);
	if (text != NULL) {
		FILE *f = fopen(json_path, "wb");
		if (f != NULL) {
			fwrite(text, 1, strlen(text), f);
			fclose(f);
		} else {
			PE("capture: failed to open sidecar %s\n", json_path);
		}
		free(text);
	}
	cJSON_Delete(root);
}

static void
capture_frame(void)
{
	const char *home = getenv("USERPROFILE");
	if (home == NULL || home[0] == '\0') {
		home = ".";
	}

	char dir[MAX_PATH];
	snprintf(dir, sizeof(dir), "%s\\Pictures\\DisplayXR", home);

	// Best-effort: Pictures should already exist; create the DisplayXR subdir.
	char pictures[MAX_PATH];
	snprintf(pictures, sizeof(pictures), "%s\\Pictures", home);
	CreateDirectoryA(pictures, NULL);
	if (!CreateDirectoryA(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
		PE("capture: CreateDirectory(%s) failed: %lu\n", dir, GetLastError());
		return;
	}

	SYSTEMTIME st;
	GetLocalTime(&st);

	XrWorkspaceCaptureRequestEXT req = {XR_TYPE_WORKSPACE_CAPTURE_REQUEST_EXT};
	snprintf(req.pathPrefix, sizeof(req.pathPrefix),
	         "%s\\capture_%04d-%02d-%02d_%02d-%02d-%02d",
	         dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
	req.flags = XR_WORKSPACE_CAPTURE_FLAG_ATLAS_BIT_EXT;

	XrWorkspaceCaptureResultEXT result = {XR_TYPE_WORKSPACE_CAPTURE_RESULT_EXT};
	XrResult r = g_xr->capture_frame(g_xr->session, &req, &result);
	if (r != XR_SUCCESS) {
		PE("capture: xrCaptureWorkspaceFrameEXT failed: %d\n", r);
		return;
	}

	if (result.viewsWritten == 0) {
		PE("capture: runtime returned 0 views written\n");
		return;
	}

	char json_path[MAX_PATH];
	snprintf(json_path, sizeof(json_path), "%s.json", req.pathPrefix);
	capture_write_sidecar(json_path, &result);

	P("Capture saved: %s (views=0x%llx atlas=%ux%u eye=%ux%u)\n",
	  req.pathPrefix, (unsigned long long)result.viewsWritten,
	  result.atlasWidth, result.atlasHeight,
	  result.eyeWidth, result.eyeHeight);
}

// WIN32 subsystem entry point — no console window.
// Delegates to main() with the command line split into argc/argv.
int WINAPI
WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	(void)hInstance;
	(void)hPrevInstance;
	(void)lpCmdLine;
	(void)nCmdShow;
	return main(__argc, __argv);
}
#endif

int
main(int argc, char *argv[])
{
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	// Unbuffered output so messages appear immediately in redirected mode
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	// Parse arguments
	struct app_entry apps[MAX_APPS];
	int app_count = 0;
	struct capture_entry captures[MAX_CAPTURES];
	int capture_count = 0;
	if (parse_args(argc, argv, apps, &app_count, captures, &capture_count) < 0) {
		return 1;
	}

#ifdef _WIN32
	// Single-instance guard — `Local\` scopes the name to the current Windows
	// user session so it works correctly under fast-user-switching. The
	// orchestrator's keyboard hook (service_orchestrator.c) also OpenMutex's
	// this name to detect "shell already running" before spawning a duplicate
	// when Ctrl+Space is pressed and our process was launched directly via
	// command line (i.e. without going through the orchestrator's
	// launch_child path that sets s_workspace_running).
	//
	// Held for process lifetime; Windows releases on process exit, abandoned
	// state is fine for the open-only check on the orchestrator side. Don't
	// take ownership (lpName, no acquire) — we only want the name, not the
	// lock.
	static HANDLE s_singleton_mutex = NULL;
	s_singleton_mutex = CreateMutexW(NULL, FALSE, L"Local\\DisplayXR.Shell.Singleton");
	if (s_singleton_mutex != NULL && GetLastError() == ERROR_ALREADY_EXISTS) {
		PE("DisplayXR shell already running on this session — exiting.\n"
		   "    (Press Ctrl+Space or click the tray icon to toggle the existing instance.)\n");
		CloseHandle(s_singleton_mutex);
		s_singleton_mutex = NULL;
		return 0;
	}
	if (s_singleton_mutex == NULL) {
		// CreateMutex failure is non-fatal — we just lose the duplicate-launch
		// guard. Continue with a warning so dev launches behind sandbox/AV
		// quirks still work.
		PE("Warning: CreateMutex(Local\\DisplayXR.Shell.Singleton) failed (err=%lu) — "
		   "duplicate-launch guard disabled.\n",
		   (unsigned long)GetLastError());
	}
#endif

	P("DisplayXR Shell\n");
	if (app_count > 0) {
		P("Will launch %d app(s)\n", app_count);
	}
	if (capture_count > 0) {
		P("Will capture %d window(s)\n", capture_count);
	}

	// Phase 2.I C10: shell connects to the service exclusively via the public
	// OpenXR extension surface. shell_openxr_init runs the runtime DLL which
	// internally opens the IPC connection and auto-starts displayxr-service
	// if it isn't already running. The shell never manages an IPC connection
	// directly anymore.
	P("Connecting to service via OpenXR runtime...\n");
	XrResult xret = XR_SUCCESS;
	struct shell_openxr_state *xr = shell_openxr_init();
	if (xr == NULL) {
		PE("shell_openxr_init failed — workspace + launcher extensions unavailable.\n");
		return 1;
	}
	g_xr = xr;
	P("Connected to service.\n");

	// Phase 2.C: bring up the chrome render module. If init fails the shell
	// keeps running without controller-rendered chrome — the in-runtime
	// chrome (still alive until C5) provides a fallback.
	g_chrome = shell_chrome_create(g_xr);
	if (g_chrome == NULL) {
		PE("shell_chrome_create failed — controller chrome disabled\n");
	}

	// Load registered apps config (Phase 4C.9 + Phase 5.5 scanner merge)
	registered_apps_load();

	// Phase 5.8: push the merged registry to the service so the spatial
	// launcher panel can render its tile grid.
	shell_push_registered_apps_to_service();

#ifdef _WIN32
	// --- Resolve runtime JSON path (needed for app launches) ---
	char runtime_json[MAX_PATH] = {0};
	bool have_json = get_runtime_json_path(runtime_json, sizeof(runtime_json));
	DWORD service_pid = find_service_pid();

	// --- Create hidden top-level window for hotkeys + tray ---
	// Uses a dedicated `DisplayXRShellTray` class with shell_tray_wnd_proc so
	// Shell_NotifyIcon callbacks (WM_TRAYICON) get dispatched to a proper
	// WndProc — without that, the inline PeekMessage filter we used to rely
	// on silently dropped right-click events on some Windows builds.
	// WS_EX_TOOLWINDOW + zero size keeps the window invisible / out of the
	// taskbar, while still being a real top-level window so
	// SetForegroundWindow works inside the context-menu path.
	shell_register_tray_class();
	g_msg_hwnd = CreateWindowExA(WS_EX_TOOLWINDOW, "DisplayXRShellTray",
	    "DisplayXR Shell Msg", 0,
	    0, 0, 0, 0, NULL, NULL, GetModuleHandleA(NULL), NULL);
	if (g_msg_hwnd == NULL) {
		PE("Warning: failed to create tray window (hotkey/tray unavailable)\n");
	}

	// Register system-wide hotkeys. Ctrl+Space in service-managed mode exits
	// the shell so the orchestrator's keyboard hook (gated on !s_shell_running)
	// can re-spawn us on the next press.
	if (g_msg_hwnd != NULL) {
		if (!RegisterHotKey(g_msg_hwnd, HOTKEY_TOGGLE, MOD_CONTROL | MOD_NOREPEAT, VK_SPACE)) {
			PE("Warning: RegisterHotKey(Ctrl+Space) failed — hotkey unavailable\n");
		}
		if (!RegisterHotKey(g_msg_hwnd, HOTKEY_LAUNCH, MOD_CONTROL | MOD_NOREPEAT, 'L')) {
			PE("Warning: RegisterHotKey(Ctrl+L) failed — launcher hotkey unavailable\n");
		}
		if (!RegisterHotKey(g_msg_hwnd, HOTKEY_CAPTURE, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'C')) {
			PE("Warning: RegisterHotKey(Ctrl+Shift+C) failed — capture hotkey unavailable\n");
		}
	}

	// Create system tray icon
	if (g_msg_hwnd != NULL) {
		tray_create(g_msg_hwnd);
	}

	// --- Decide startup mode ---
	// If launched with apps or captures: activate immediately.
	// If launched via the service orchestrator (--service-managed): also
	// activate immediately. The orchestrator only spawns us in response to
	// Ctrl+Space being pressed (semantically "summon shell"), so starting
	// inactive-in-tray would need a second Ctrl+Space to activate — but
	// Phase 2.K B-merged: shell is always active while running. There is no
	// "in-tray waiting" state — if you launched the shell, the workspace is
	// active. Exit (Ctrl+Space, tray Exit) kills apps and the shell process;
	// the orchestrator respawns on the next Ctrl+Space.
	P("Activating shell mode...\n");
	xret = g_xr->activate(g_xr->session);
	if (xret != XR_SUCCESS) {
		PE("Warning: workspace_activate failed\n");
	}
	g_shell_active = true;
	tray_update_tooltip(true);

	// Add capture clients
	for (int i = 0; i < capture_count; i++) {
		uint32_t cid = 0;
		XrResult r = g_xr->add_capture(g_xr->session, captures[i].hwnd, NULL, &cid);
		if (r == XR_SUCCESS) {
			captures[i].client_id = cid;
			captures[i].added = true;
			P("  Capture: HWND=0x%llx '%s' → client_id=%u\n",
			  (unsigned long long)captures[i].hwnd, captures[i].name, cid);
		} else {
			PE("  Capture: failed for HWND=0x%llx '%s'\n",
			   (unsigned long long)captures[i].hwnd, captures[i].name);
		}
	}

	// Launch apps
	if (app_count > 0) {
		P("XR_RUNTIME_JSON = %s\n", have_json ? runtime_json : "(not set)");
		for (int i = 0; i < app_count; i++) {
			launch_app(&apps[i], have_json ? runtime_json : NULL);
			if (i + 1 < app_count) {
				Sleep(100);
			}
		}
	}

	// Auto-adopt is disabled for now — use --capture-hwnd for explicit 2D windows.
	// TODO(4C.6): re-enable on re-activate to auto-adopt desktop windows.
	bool auto_adopt = false;
#else
	// Non-Windows: simple activate + poll (no hotkey/tray)
	P("Activating shell mode...\n");
	xret = g_xr->activate(g_xr->session);
	if (xret != XR_SUCCESS) {
		PE("Warning: workspace_activate failed\n");
	}
	bool auto_adopt = false;
#endif

	P("Monitoring clients (Ctrl+C to exit)...\n");

	uint32_t prev_ids[SHELL_MAX_CLIENTS] = {0};
	uint32_t prev_count = 0;
	bool poses_pending = false;
	for (int i = 0; i < app_count; i++) {
		if (apps[i].has_pose) {
			poses_pending = true;
		}
	}

	int adopt_counter = 0;

	// Client ID → numbered name mapping (for persistence with duplicate app names)
	static struct { uint32_t id; char name[128]; } client_names[MAX_SAVED_WINDOWS];
	static int client_name_count = 0;

	// --- Main loop: MsgWait on Windows, Sleep on other platforms ---
	while (g_running) {
#ifdef _WIN32
		// Phase 2.K: variable cadence. While any client animation is active
		// OR the carousel preset is owning per-frame poses, we poll every
		// ~16 ms (~60 Hz) so the tick interpolates cleanly; otherwise we
		// drop back to 500 ms for ambient idle. The runtime also delivers
		// FRAME_TICK events on the input drain that the tick could pace
		// against directly, but the timer fallback keeps things simple and
		// is correct when FRAME_TICK arrives on a different drain pass.
		bool carousel_owning =
		    (s_active_preset != NULL && strcmp(s_active_preset, "carousel") == 0);
		// Animation pacing: while any per-frame animation is active we
		// need 60 Hz timer cadence regardless of the wakeup-event signal,
		// because animation lerps are CPU-driven and don't fire IPC events.
		// Idle (no animation, no carousel) → either wait forever on the
		// wakeup event (spec_version 8) or fall back to POLL_INTERVAL_MS.
		bool need_timer_pacing =
		    shell_slot_anim_active_count() > 0 || carousel_owning ||
		    (g_chrome != NULL && shell_chrome_is_animating(g_chrome));

		// spec_version 8: prefer event-driven wakeup. The runtime SetEvent's
		// the wakeup handle on every workspace input event push and on
		// hovered/focused-slot transitions, so MsgWaitForMultipleObjects
		// returns the moment something interesting happens — zero idle CPU
		// + zero hover latency. The shell_chrome_has_any chrome-poll fix
		// stays as a fallback for setups where wakeup_event_handle is NULL
		// (older runtime / unsupported platform).
		HANDLE wakeup = (HANDLE)g_xr->wakeup_event_handle;
		DWORD num_handles = (wakeup != NULL) ? 1u : 0u;
		HANDLE handles[1] = {wakeup};
		DWORD poll_ms;
		if (need_timer_pacing) {
			poll_ms = 16u;
		} else if (wakeup != NULL) {
			// Long timeout: wakeup is event-driven. Use ~1 s as a safety
			// net (in case we miss a SetEvent for some reason); not strict
			// polling.
			poll_ms = 1000u;
		} else {
			// Fallback: chrome alive → 60 Hz so hover transitions reach
			// us promptly. Else idle 500 ms.
			bool chrome_alive = (g_chrome != NULL && shell_chrome_has_any(g_chrome));
			poll_ms = chrome_alive ? 16u : (DWORD)POLL_INTERVAL_MS;
		}
		DWORD wait_result = MsgWaitForMultipleObjects(
		    num_handles, handles, FALSE, poll_ms, QS_ALLINPUT);
		// wait_result values when num_handles == 1:
		//   WAIT_OBJECT_0     → the wakeup event was signaled
		//   WAIT_OBJECT_0 + 1 → Win32 messages are pending
		//   WAIT_TIMEOUT      → poll_ms expired (animation tick, fallback poll)
		// We treat all three the same: fall through to drain + tick + Win32
		// message pump. The wakeup event is auto-reset; nothing to do here.
		(void)wait_result; // existing message-pump code below handles all paths

		// MsgWaitForMultipleObjects encodes "messages pending" as
		// WAIT_OBJECT_0 + num_handles. With num_handles=0 (fallback),
		// that's WAIT_OBJECT_0 itself; with num_handles=1 (event-driven),
		// it's WAIT_OBJECT_0 + 1. Pump messages on either condition (and
		// also on the wakeup event itself, since drain runs unconditionally
		// below — no harm in checking the queue too).
		DWORD msg_wake = WAIT_OBJECT_0 + num_handles;
		if (wait_result == msg_wake || wait_result == WAIT_OBJECT_0) {
			MSG msg;
			while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
				if (msg.message == WM_HOTKEY && msg.wParam == HOTKEY_TOGGLE) {
					// Phase 2.K B-merged: Ctrl+Space exits the shell. There
					// is no longer a separate "deactivate" state — apps die,
					// the workspace is torn down, and the orchestrator's
					// keyboard hook will respawn the shell on the next
					// Ctrl+Space. Same path for service-managed and direct
					// command-line launches.
					P("Ctrl+Space pressed — exiting shell.\n");
					g_running = 0;
					break;
				} else if (msg.message == WM_HOTKEY && msg.wParam == HOTKEY_LAUNCH) {
					// --- Ctrl+L: Toggle spatial launcher panel (Phase 5.7) ---
					// The Phase 4C MessageBox path is gone — the service-side
					// multi-compositor renders the panel in its own window when
					// launcher_visible is true. Tile grid + launch dispatch
					// come in later Phase 5 tasks.
					if (g_shell_active) {
						g_launcher_visible = !g_launcher_visible;

						// Phase 5.12: when opening, hand the service process
						// foreground-activation permission so it can pull its
						// compositor window into focus. Otherwise keys like
						// Esc stay routed to whichever app previously had
						// focus and never reach the compositor WndProc that
						// the launcher keyboard handler depends on.
						if (g_launcher_visible && service_pid != 0) {
							AllowSetForegroundWindow(service_pid);
						}

						XrResult lret = g_xr->set_launcher_visible(
						    g_xr->session, g_launcher_visible ? XR_TRUE : XR_FALSE);
						if (lret != XR_SUCCESS) {
							PE("xrSetLauncherVisibleEXT failed: %d\n", lret);
							g_launcher_visible = !g_launcher_visible; // roll back
						} else {
							P("Launcher %s\n", g_launcher_visible ? "shown" : "hidden");
						}
					}
				} else if (msg.message == WM_HOTKEY && msg.wParam == HOTKEY_CAPTURE) {
					// --- Ctrl+Shift+C: capture pre-weave SBS frame (Phase 8) ---
					if (g_shell_active) {
						capture_frame();
					} else {
						P("Capture ignored: shell not active\n");
					}
				}
				// Phase 2.K tray-fix: WM_TRAYICON is now handled by
				// shell_tray_wnd_proc (registered for the DisplayXRShellTray
				// class) via DispatchMessage below. The previous inline filter
				// silently dropped right-click events on some Windows builds.
				TranslateMessage(&msg);
				DispatchMessageA(&msg);
			}
		}
		// Fall through to poll work regardless of wait result
#else
		usleep(500000);
#endif

		// --- Poll work (only meaningful when shell is active) ---
#ifdef _WIN32
		if (!g_shell_active) {
			continue;
		}
#endif

		// Phase 2.G: drain workspace input events. Ctrl+1..3 trigger
		// layout presets; other key/pointer events are read here for
		// future use (controller-driven drag, hover routing, etc.).
		// Phase 2.K: drain also consumes FRAME_TICK / FOCUS_CHANGED /
		// MOTION variants now; the carousel state machine (commit 5)
		// reads MOTION + SCROLL.
		shell_drain_input_events();

		// Phase 2.K: drive any active animations forward. Idle when no
		// animation is active; cheap when one is.
		(void)shell_slot_anim_tick();

		// Phase 2.K: drive the carousel state machine while it's the active
		// preset. Internally early-returns when slot animations are still
		// playing (the entry transition into carousel) or when the preset
		// is something else. Cheap when idle.
		shell_carousel_tick();

		// Phase 2.C C3.C-4: chrome hover-fade tick. Lerps per-slot fade
		// alpha and re-renders the chrome SRV when alpha changes. No-op
		// when no slot has an active fade — idle == zero GPU work.
		if (g_chrome != NULL) {
			shell_chrome_tick(g_chrome);
		}

#ifdef _WIN32
		// DEBUG file-trigger: drop %TEMP%\displayxr_preset_{grid|immersive|carousel}
		// to apply that preset programmatically (bypasses keyboard focus).
		// Used for end-to-end smoke tests that can't drive Ctrl+1..3 reliably.
		{
			const char *temp_dir = getenv("TEMP");
			if (temp_dir != NULL) {
				const char *names[3] = {"grid", "immersive", "carousel"};
				char path[MAX_PATH];
				for (int i = 0; i < 3; i++) {
					snprintf(path, sizeof(path),
					         "%s\\displayxr_preset_%s", temp_dir, names[i]);
					if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
						DeleteFileA(path);
						shell_apply_preset(names[i]);
					}
				}
			}
		}
#endif

		// Apply pending poses when new clients appear
		if (poses_pending) {
			try_apply_poses(apps, app_count, prev_ids, prev_count);

			poses_pending = false;
			for (int i = 0; i < app_count; i++) {
				if (apps[i].has_pose && !apps[i].pose_applied) {
					poses_pending = true;
				}
			}
		}

		// Detect new clients and track names. Set a flag so the auto-tile
		// pass below knows whether it has work to do this tick.
		bool new_client_seen = false;
		{
			XrWorkspaceClientId client_ids[SHELL_MAX_CLIENTS];
			uint32_t client_count = shell_enumerate_clients(client_ids, SHELL_MAX_CLIENTS);

			// Phase 2.K: cancel pending animations for disconnected clients
			// so the tick stops pushing set_pose on a dead slot. Iterate the
			// previous set and bail out of any not in the current list.
			for (uint32_t p = 0; p < prev_count; p++) {
				bool still_present = false;
				for (uint32_t c = 0; c < client_count; c++) {
					if (prev_ids[p] == client_ids[c]) {
						still_present = true;
						break;
					}
				}
				if (!still_present) {
					shell_slot_anim_cancel(prev_ids[p]);
				}
			}

			for (uint32_t c = 0; c < client_count; c++) {
				bool is_new = true;
				for (uint32_t p = 0; p < prev_count; p++) {
					if (client_ids[c] == prev_ids[p]) {
						is_new = false;
						break;
					}
				}
				if (is_new && client_ids[c] != XR_NULL_WORKSPACE_CLIENT_ID) {
					XrWorkspaceClientInfoEXT cinfo;
					if (shell_get_client_info(client_ids[c], &cinfo) && cinfo.name[0] != '\0') {
						int instance = 1;
						for (int cn = 0; cn < client_name_count; cn++) {
							char existing_base[128];
							snprintf(existing_base, sizeof(existing_base), "%s", client_names[cn].name);
							char *paren = strrchr(existing_base, '(');
							if (paren && paren > existing_base && *(paren - 1) == ' ')
								*(paren - 1) = '\0';
							if (strcmp(existing_base, cinfo.name) == 0)
								instance++;
						}

						char numbered_name[128];
						if (instance > 1)
							snprintf(numbered_name, sizeof(numbered_name), "%s (%d)",
							         cinfo.name, instance);
						else
							snprintf(numbered_name, sizeof(numbered_name), "%s",
							         cinfo.name);

						if (client_name_count < MAX_SAVED_WINDOWS) {
							client_names[client_name_count].id = client_ids[c];
							snprintf(client_names[client_name_count].name, 128, "%s", numbered_name);
							client_name_count++;
						}
#ifdef _WIN32
						uint64_t self_pid_ck = (uint64_t)GetCurrentProcessId();
#else
						uint64_t self_pid_ck = (uint64_t)getpid();
#endif
						if (cinfo.pid != self_pid_ck) {
							new_client_seen = true;
						}
					}
				}
			}
		}

		print_clients(prev_ids, &prev_count);

		// Phase 2.G: when a new app client appears and the user did not
		// pin any per-app poses, auto-tile to a grid so windows occupy
		// their full slot instead of stacking at the runtime's
		// connect-time placement. Replaces the "debounced re-grid" the
		// runtime used to own.
		//
		// Race: xrEnumerateWorkspaceClientsEXT returns clients as soon as
		// they connect over IPC, but the per-client compositor slot is
		// bound a few ticks later — set_pose fails XR_ERROR_VALIDATION
		// during that window. Retry on the next tick(s) until every
		// client takes the pose.
		static bool s_auto_tile_pending = false;
		if (new_client_seen) {
			bool any_pose_specified = false;
			for (int i = 0; i < app_count; i++) {
				if (apps[i].has_pose) {
					any_pose_specified = true;
					break;
				}
			}
			if (!any_pose_specified) {
				s_auto_tile_pending = true;
			}
		}
		if (s_auto_tile_pending) {
			if (shell_apply_preset("grid")) {
				s_auto_tile_pending = false;
			}
		}

		// Phase 2.C: lazy chrome creation. The runtime cannot mint a chrome
		// swapchain for a slot that has not bound yet (Phase 2.K lesson 3),
		// so each tick we walk the client list and try to create chrome for
		// any non-self client that does not have it yet. Already-chromed
		// clients are skipped without get_pose IPC — the slot-anim's per-
		// frame set_pose RPCs need the IPC pipe quiet to keep transitions
		// smooth. Layout re-push for size changes happens in shell_apply_preset.
		if (g_chrome != NULL) {
#ifdef _WIN32
			uint64_t self_pid_chrome = (uint64_t)GetCurrentProcessId();
#else
			uint64_t self_pid_chrome = (uint64_t)getpid();
#endif
			XrWorkspaceClientId chr_ids[SHELL_MAX_CLIENTS];
			uint32_t chr_n = shell_enumerate_clients(chr_ids, SHELL_MAX_CLIENTS);
			for (uint32_t i = 0; i < chr_n; i++) {
				if (shell_chrome_has(g_chrome, chr_ids[i])) continue;

				XrWorkspaceClientInfoEXT cinfo;
				if (!shell_get_client_info(chr_ids[i], &cinfo)) continue;
				if (cinfo.pid == self_pid_chrome) continue; // skip own session

				XrPosef pose = {0};
				float w_m = 0.0f, h_m = 0.0f;
				if (g_xr->get_pose(g_xr->session, chr_ids[i], &pose, &w_m, &h_m) != XR_SUCCESS) {
					continue;
				}
				if (!(w_m > 0.0f) || !(h_m > 0.0f)) continue;
				// If an animation is in flight (e.g. the auto grid preset
				// just seeded but hasn't settled), prefer the animation's
				// target dims for the initial chrome layout. Otherwise the
				// chrome locks in the mid-glide / pre-preset dims and the
				// pill ends up sized + positioned for a different window.
				float anim_w = 0.0f, anim_h = 0.0f;
				if (shell_slot_anim_get_target(chr_ids[i], &anim_w, &anim_h) &&
				    anim_w > 0.0f && anim_h > 0.0f) {
					w_m = anim_w;
					h_m = anim_h;
				}
				const char *icon_path = shell_resolve_icon_for_pid(
				    (unsigned long)cinfo.pid);
				const char *title_text = shell_resolve_title_for_pid(
				    (unsigned long)cinfo.pid, cinfo.name);
				(void)shell_chrome_on_client_connected(
				    g_chrome, chr_ids[i], w_m, h_m, icon_path, title_text);

				// Phase 2.C spec_version 9: push the default per-client
				// visual style. Universal soft edge feather (~3 mm) on every
				// window for a subtle ambient softening; cyan-blue focus
				// halo on whichever client is focused (the runtime gates
				// the glow to the focused slot at blit time, so unfocused
				// clients see the feather only). Color matches the runtime's
				// existing launcher-hover glow for visual consistency. The
				// shell is the only authority that pushes per-client style
				// — third-party controllers can override with their own
				// theme via the same xrSetWorkspaceClientStyleEXT entry.
				if (g_xr->set_client_style != NULL) {
					XrWorkspaceClientStyleEXT style = {XR_TYPE_WORKSPACE_CLIENT_STYLE_EXT};
					style.cornerRadius = 0.05f;       // 5% of window height — matches prior runtime default
					style.edgeFeatherMeters = 0.003f; // 3 mm soft falloff
					style.focusGlowColor[0] = 0.30f;  // cyan-blue (RGB)
					style.focusGlowColor[1] = 0.55f;
					style.focusGlowColor[2] = 1.00f;
					style.focusGlowColor[3] = 1.00f;
					style.focusGlowIntensity = 0.85f;
					style.focusGlowFalloffMeters = 0.012f; // 12 mm halo extent
					(void)g_xr->set_client_style(g_xr->session, chr_ids[i], &style);
				}
			}
			// Drop chrome for any disconnected clients (diff against prev_ids).
			for (uint32_t p = 0; p < prev_count; p++) {
				bool still = false;
				for (uint32_t c = 0; c < chr_n; c++) {
					if (prev_ids[p] == chr_ids[c]) { still = true; break; }
				}
				if (!still) {
					shell_chrome_on_client_disconnected(g_chrome, prev_ids[p]);
				}
			}
		}

#ifdef _WIN32
		// Detect server-side deactivation (ESC closed the compositor window).
		// The compositor sets workspace_mode=false — sync our state.
		if (g_shell_active) {
			XrBool32 server_active_b = XR_FALSE;
			if (g_xr->get_state(g_xr->session, &server_active_b) == XR_SUCCESS) {
				bool server_active = (server_active_b == XR_TRUE);
				if (!server_active) {
					P("Shell deactivated by compositor (ESC) — returning to tray.\n");
					capture_count = 0;
					client_name_count = 0;
					g_shell_active = false;
					g_launcher_visible = false;
					tray_update_tooltip(false);
				}
			}
		}

		// Phase 5.11: refresh the running-tile mask and push to service if
		// it has changed. Cheap to compute (one IPC list call + per-PID
		// QueryFullProcessImageNameW), and we only push on diff.
		if (g_shell_active) {
			static uint64_t s_last_running_mask = (uint64_t)-1;
			uint64_t mask = shell_compute_running_tile_mask();
			if (mask != s_last_running_mask) {
				(void)g_xr->set_running_tile_mask(g_xr->session, mask);
				s_last_running_mask = mask;
			}
		}

		// Phase 5.10: poll for launcher tile clicks. The service-side
		// WM_LBUTTONDOWN handler stores a tile index when the user clicks
		// inside the launcher; we look up the registered app and dispatch
		// the launch on this side because we own the env vars + CreateProcess
		// path. The launcher panel was already hidden by the service when
		// the click landed, so we just need to sync our local state and run
		// the launch. Only poll while the launcher is actually visible to
		// keep IPC traffic minimal.
		// Poll for launcher tile clicks. Only while the launcher is visible
		// — when hidden there can't be any clicks. This is a performance
		// optimization (avoids 500ms-cadence IPC traffic when the launcher
		// isn't open), not a workaround for #144 which turned out to be a
		// stale-binary issue resolved by the Phase 6 rebuild.
		if (g_shell_active && g_launcher_visible) {
			int32_t tile_index = XR_LAUNCHER_INVALID_APPINDEX_EXT;
			if (g_xr->poll_launcher_click(g_xr->session, &tile_index) == XR_SUCCESS &&
			    tile_index != XR_LAUNCHER_INVALID_APPINDEX_EXT) {
				// Service already hid the launcher when the action registered.
				g_launcher_visible = false;

				// Phase 6.6: check refresh BEFORE remove — both use negative
				// sentinels and refresh (-300) would match the remove check
				// (<= -200) if checked second.
				if (tile_index == XR_LAUNCHER_APPINDEX_REFRESH_EXT) {
					P("Launcher: refreshing app list (before: %d apps)\n",
					  g_registered_app_count);
					registered_apps_load();
					P("Launcher: refreshed (after: %d apps)\n",
					  g_registered_app_count);
					shell_push_registered_apps_to_service();
#ifdef _WIN32
					if (service_pid != 0) {
						AllowSetForegroundWindow(service_pid);
					}
#endif
					if (g_xr->set_launcher_visible(g_xr->session, XR_TRUE) == XR_SUCCESS) {
						g_launcher_visible = true;
					}
				} else if (tile_index <= -XR_LAUNCHER_APPINDEX_REMOVE_BASE_EXT) {
					int full_idx = (int)(-(tile_index) - XR_LAUNCHER_APPINDEX_REMOVE_BASE_EXT);
					if (full_idx >= 0 && full_idx < g_registered_app_count) {
						struct registered_app *rm = &g_registered_apps[full_idx];
						P("Launcher: removing '%s' permanently\n", rm->name);

#ifdef _WIN32
						// Delete the manifest file (and sibling icon files)
						// when it's a registered-mode manifest the user owns.
						// Sidecar manifests in dev paths are developer-owned
						// and never deleted here.
						if (rm->manifest_path[0]) {
							const char *local = getenv("LOCALAPPDATA");
							const char *progdata = getenv("ProgramData");
							bool in_user_dir = (local && _strnicmp(rm->manifest_path,
							    local, strlen(local)) == 0);
							bool in_system_dir = (progdata && _strnicmp(rm->manifest_path,
							    progdata, strlen(progdata)) == 0);
							if (in_user_dir || in_system_dir) {
								if (DeleteFileA(rm->manifest_path)) {
									P("  deleted manifest: %s\n", rm->manifest_path);
								} else {
									DWORD err = GetLastError();
									PE("  could not delete manifest %s "
									   "(err=%lu) — tile will reappear next scan\n",
									   rm->manifest_path, err);
								}
								// Also delete the resolved icon files when
								// they live alongside the manifest. We
								// compare directories so we don't nuke a
								// shared icon in another location.
								char manifest_dir[MAX_PATH];
								snprintf(manifest_dir, sizeof(manifest_dir), "%s",
								         rm->manifest_path);
								char *sep = strrchr(manifest_dir, '\\');
								if (sep) *sep = '\0';
								if (rm->icon_path[0] &&
								    _strnicmp(rm->icon_path, manifest_dir,
								              strlen(manifest_dir)) == 0) {
									DeleteFileA(rm->icon_path);
								}
								if (rm->icon_3d_path[0] &&
								    _strnicmp(rm->icon_3d_path, manifest_dir,
								              strlen(manifest_dir)) == 0) {
									DeleteFileA(rm->icon_3d_path);
								}
							}
						}
#endif
						// Shift remaining entries down.
						for (int j = full_idx; j < g_registered_app_count - 1; j++) {
							g_registered_apps[j] = g_registered_apps[j + 1];
						}
						g_registered_app_count--;
						registered_apps_save();
						shell_push_registered_apps_to_service();
					}
					// Re-show launcher so user sees the updated grid.
#ifdef _WIN32
					if (service_pid != 0) {
						AllowSetForegroundWindow(service_pid);
					}
#endif
					if (g_xr->set_launcher_visible(g_xr->session, XR_TRUE) == XR_SUCCESS) {
						g_launcher_visible = true;
					}
				} else if (tile_index == XR_LAUNCHER_APPINDEX_BROWSE_EXT) {
					// Phase 5.14: Browse tile → open file dialog, add to
					// registry, re-push, re-show launcher so the user sees
					// their new tile.
					shell_browse_and_add_app();
#ifdef _WIN32
					if (service_pid != 0) {
						AllowSetForegroundWindow(service_pid);
					}
#endif
					if (g_xr->set_launcher_visible(g_xr->session, XR_TRUE) == XR_SUCCESS) {
						g_launcher_visible = true;
					}
				} else if (tile_index >= 0 && tile_index < (int32_t)g_registered_app_count) {
					struct registered_app *rapp = &g_registered_apps[tile_index];

					// Phase 5.11: if a matching client is already running,
					// focus it instead of spawning a second instance.
					bool focused_existing = false;
#ifdef _WIN32
					{
						XrWorkspaceClientId clist_ids[SHELL_MAX_CLIENTS];
						uint32_t clist_count = shell_enumerate_clients(clist_ids, SHELL_MAX_CLIENTS);
						for (uint32_t c = 0; c < clist_count && !focused_existing; c++) {
							if (clist_ids[c] == XR_NULL_WORKSPACE_CLIENT_ID) continue;
							XrWorkspaceClientInfoEXT cinfo;
							if (!shell_get_client_info(clist_ids[c], &cinfo))
								continue;
							char client_exe[MAX_PATH];
							if (!shell_pid_to_exe_path((DWORD)cinfo.pid, client_exe, sizeof(client_exe)))
								continue;
							if (shell_exe_paths_equal(client_exe, rapp->exe_path)) {
								if (g_xr->set_focused(g_xr->session, clist_ids[c]) == XR_SUCCESS) {
									P("Launcher: focused running client %u → '%s'\n",
									  (unsigned)clist_ids[c], rapp->name);
									focused_existing = true;
								}
							}
						}
					}
#endif
					if (!focused_existing) {
						P("Launcher: launching tile %d → '%s'\n",
						  (int)tile_index, rapp->name);
						shell_launch_registered_app(
						    rapp,
						    have_json ? runtime_json : NULL,
						    apps, &app_count,
						    captures, &capture_count);
					}
				} else {
					PE("Launcher click: tile %d out of range (count=%d)\n",
					   (int)tile_index, g_registered_app_count);
				}
			}
		}

		// Dynamic window tracking: detect new/closed windows every ~1 second
		if (auto_adopt) {
			adopt_counter++;
			if (adopt_counter >= 2) {
				adopt_counter = 0;
				cleanup_closed_captures(captures, &capture_count);
				enumerate_and_adopt_windows(captures, &capture_count, service_pid);
			}
		}
#endif
	}

	P("\nShell exiting.\n");

#ifdef _WIN32
	// Phase 2.K B-merged: kill apps before tearing down the workspace so the
	// runtime's lazy reverse-hot-switch never fires (apps are gone before
	// xrDeactivate's "switch surviving IPC clients to standalone" branch
	// would run). EXIT_REQUEST first for graceful shutdown, then a brief
	// wait, then TerminateProcess for stragglers.
	if (g_shell_active) {
		uint64_t self_pid = (uint64_t)GetCurrentProcessId();

		// 1. Send EXIT_REQUEST via xrRequestWorkspaceClientExitEXT to every
		//    connected workspace client (filter our own controller session
		//    by PID). Apps that are still binding their slot won't be in the
		//    enumerate result yet — those get a TerminateProcess fallback
		//    below.
		if (g_xr->request_client_exit != NULL && g_xr->enumerate_clients != NULL) {
			XrWorkspaceClientId raw_ids[SHELL_MAX_CLIENTS];
			uint32_t raw_n = shell_enumerate_clients(raw_ids, SHELL_MAX_CLIENTS);
			for (uint32_t i = 0; i < raw_n; i++) {
				XrWorkspaceClientInfoEXT cinfo;
				if (!shell_get_client_info(raw_ids[i], &cinfo)) continue;
				if (cinfo.pid == self_pid) continue;
				(void)g_xr->request_client_exit(g_xr->session, raw_ids[i]);
			}
		}

		// 2. Wait briefly (up to 1 s) for the apps we launched to exit
		//    cleanly via the EXIT_REQUEST signal.
		HANDLE handles[MAX_APPS];
		DWORD count = 0;
		for (int i = 0; i < app_count; i++) {
			if (apps[i].process != NULL) handles[count++] = apps[i].process;
		}
		if (count > 0) {
			WaitForMultipleObjects(count, handles, TRUE, 1000);
		}

		// 3. Force-terminate any apps that didn't exit in time. Single-process
		//    timeout, not per-process, so a single hung app doesn't stall the
		//    whole shutdown more than 1 second.
		for (int i = 0; i < app_count; i++) {
			if (apps[i].process == NULL) continue;
			DWORD code = 0;
			if (GetExitCodeProcess(apps[i].process, &code) && code == STILL_ACTIVE) {
				P("App %s did not exit on EXIT_REQUEST; terminating.\n",
				  apps[i].exe_path != NULL ? apps[i].exe_path : "(unknown)");
				TerminateProcess(apps[i].process, 0);
			}
		}

		// 4. Now safe to deactivate the workspace. Capture clients are
		//    restored to their 2D windows by the runtime's deactivate path.
		(void)g_xr->deactivate(g_xr->session);
	}

	// Cleanup hotkey and tray
	if (g_msg_hwnd != NULL) {
		UnregisterHotKey(g_msg_hwnd, HOTKEY_TOGGLE);
		UnregisterHotKey(g_msg_hwnd, HOTKEY_LAUNCH);
		tray_destroy();
		DestroyWindow(g_msg_hwnd);
	}

	// Close process handles
	for (int i = 0; i < app_count; i++) {
		if (apps[i].process != NULL) {
			CloseHandle(apps[i].process);
		}
	}
#endif

	if (g_chrome != NULL) {
		shell_chrome_destroy(g_chrome);
		g_chrome = NULL;
	}
	shell_openxr_shutdown(xr);
	g_xr = NULL;
	return 0;
}
