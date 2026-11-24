// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief #1370 acceptance: xrLocateViews honours XrViewLocateInfo::space.
 *
 * Drives the BUILT runtime library through the public OpenXR API - loaded
 * directly via xrNegotiateLoaderRuntimeInterface, no Khronos loader, no
 * manifest - on a headless session (XR_MND_headless), so it needs no GPU, no
 * window and no panel. It does need a registered display processor (the
 * sim-display plug-in is enough), exactly like `displayxr-cli selftest`; when
 * xrCreateInstance cannot find one the suite SKIPs rather than fails, so the
 * pre-registration ctest pass stays green and the registered pass (CI runs it
 * right after `selftest`) is the real gate.
 *
 * Session classes reachable headlessly, in-process:
 *   - legacy runtime-window (no rig chained): the qwerty synthesis path;
 *   - XR_DXR_view_rig chained (camera rig): the rig path.
 * The IPC route and the external-window RAW class need a service / an HWND and
 * are covered by the hardware eyeball list on the issue.
 *
 * What is pinned (all within 1 mm / 0.1 deg):
 *   1. base-invariance - for base in {LOCAL, STAGE, VIEW}: every XrView
 *      orientation equals xrLocateSpace(VIEW, base).orientation, and the
 *      eye-centroid offset from the VIEW origin, expressed in the VIEW frame,
 *      is the SAME vector in every base. (VIEW is the display plane; the eyes
 *      sit in front of it by design, so the offset is non-zero but must not
 *      depend on the base - that is the whole bug.)
 *   2. rig round trip - a camera rig posed at P in LOCAL yields the same views
 *      as the rig posed at T_stage_local o P in STAGE, shifted by exactly
 *      T_stage_local = xrLocateSpace(LOCAL, STAGE). No constant is hard-coded:
 *      the LOCAL offset is READ from the runtime, never assumed.
 *   3. displayPlanePose is locate-space: with a rig chained it equals the
 *      chained rig pose in that base; without one it equals
 *      xrLocateSpace(VIEW, base) (the plane is the head device pose).
 *
 * #1486 (same harness, same SKIP rule) additionally pins the primary
 * view-configuration list:
 *   4. with XR_DXR_display_info enabled, xrEnumerateViewConfigurations reports
 *      PRIMARY_STEREO first and PRIMARY_MULTIVIEW_DXR second; STEREO reports
 *      exactly 2 views while MULTIVIEW reports the device max across rendering
 *      modes (read from xrEnumerateDisplayRenderingModesDXR, never hard-coded);
 *   5. a session begun on MULTIVIEW locates that many views and REFUSES a
 *      PRIMARY_STEREO locate; a session begun on PRIMARY_STEREO locates exactly
 *      2 even on a 4-view device;
 *   6. without the extension only one configuration is advertised and the
 *      MULTIVIEW enum is not a valid value at all.
 *
 *   7. under DXR_VIEW_CONFIG_LEGACY=1 the pre-#1486 mapping comes back: ONE
 *      configuration, PRIMARY_STEREO, reporting the device MAX.
 *
 * Arm 7 runs in its OWN PROCESS and that is not cosmetic: the kill switch is
 * read through DEBUG_GET_ONCE_BOOL_OPTION, which caches the first read for the
 * life of the process, so one binary cannot host both mappings. CMake therefore
 * registers this file TWICE - the plain registration, and
 * `tests_oxr_view_space_legacy`, which sets DXR_VIEW_CONFIG_LEGACY=1 and passes
 * the `[view_config_legacy]` tag filter. The two guards below (@ref
 * legacy_switch_set) make each arm SKIP in the wrong environment, so the pairing
 * is enforced by the SOURCE too and not only by the ctest arguments - running
 * the bare binary with the variable exported cannot silently fail the #1486
 * arms.
 */

#include "catch_amalgamated.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#define XR_USE_PLATFORM_WIN32
#else
#include <dlfcn.h>
#include <time.h>
#define XR_USE_TIMESPEC
#endif

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/XR_DXR_view_rig.h>
#include <openxr/XR_DXR_display_info.h>

#ifndef DXR_RUNTIME_LIB_PATH
#error "DXR_RUNTIME_LIB_PATH must name the built runtime library"
#endif

namespace {

constexpr float kPosTolM = 0.001f;    // 1 mm

/*!
 * Multi-screen M3: PRIMARY_MULTIVIEW_DXR reports one view set per window
 * segment the system can ever weave — the device max on a box that cannot split
 * a window (one DP-backed screen, a service session, non-Linux), the device max
 * x 2 on desktop Linux with two DP-backed screens.
 *
 * EXACT everywhere: this binary runs with DXR_SEGMENTS=0 (set below, before any
 * instance exists), which keeps the pre-M3 count on every platform. Deriving
 * the Linux x2 from the box instead is not exact — a screen counts only when its
 * plug-in's VK factory passed the vk_bundle ABI check (#1243), which an
 * installed plug-in next to a dev runtime does not, and xrEnumerateDisplaysDXR
 * cannot see that. The x2 arithmetic itself is pinned by tests_oxr_segment_views.
 */
bool
is_multiview_count(size_t n, uint32_t device_max)
{
	return n == device_max;
}

[[maybe_unused]] const bool kSegmentsOff = [] {
// Before main(): the runtime reads it at every system fill-in.
#ifdef _WIN32
	_putenv_s("DXR_SEGMENTS", "0");
#else
	setenv("DXR_SEGMENTS", "0", 1);
#endif
	return true;
}();

/*
 * ADR-066 C1: sim-display's default 3D mode is "Same as 2D" (one view), so
 * the ENUMERATED slots' max view count (1) no longer equals the located count
 * (the envelope's max, 2) - the app-visible worst-case statement that closes
 * that gap is stage 3. These arms read "the device max" off the enumerate, so
 * they pin the 3D mode to anaglyph (2 views = the envelope max) unless an arm's
 * own environment chose one (the quad arms set SIM_DISPLAY_OUTPUT themselves).
 */
[[maybe_unused]] const bool kSim3dModeAnaglyph = [] {
#ifdef _WIN32
	if (getenv("SIM_DISPLAY_OUTPUT") == nullptr && getenv("DXR_DP_SIM_DISPLAY_OUTPUT") == nullptr) {
		_putenv_s("SIM_DISPLAY_OUTPUT", "anaglyph");
	}
#else
	if (getenv("DXR_DP_SIM_DISPLAY_OUTPUT") == nullptr) {
		setenv("SIM_DISPLAY_OUTPUT", "anaglyph", 0);
	}
#endif
	return true;
}();
constexpr float kAngTolDeg = 0.1f;    // 0.1 degree

/*!
 * #1486: is the kill switch armed for THIS process?
 *
 * The runtime reads DXR_VIEW_CONFIG_LEGACY through DEBUG_GET_ONCE_BOOL_OPTION
 * (u_debug.h), whose truth test is "set and not 0/off/false/no". Mirrored here
 * loosely - a bare "is it set and not literally 0" is enough to decide which
 * arms may run, and the arms themselves then assert the mapping.
 */
bool
legacy_switch_set()
{
	const char *v = std::getenv("DXR_VIEW_CONFIG_LEGACY");
	return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
}

//! Loose truth test for a boolean setting in this process's environment.
bool
env_on(const char *name)
{
	const char *v = std::getenv(name);
	return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0 && std::strcmp(v, "off") != 0 &&
	       std::strcmp(v, "false") != 0 && std::strcmp(v, "no") != 0;
}

/*!
 * #1499 / ADR-065: does sim-display come up IN its 4-view kind for THIS process?
 *
 * The mode floor is only observable on a device whose active mode a stereo
 * session cannot fill, and the only such device is sim-display with its quad
 * kind active. Under ADR-065 that takes two settings: `SIM_DISPLAY_OUTPUT=quad`
 * makes mode 1 the 4-view Quad kind, and `SIM_DISPLAY_START_3D=1` starts the
 * device in mode 1 (sim-display starts in 2D otherwise). Both are read at
 * plug-in load, so this has to be a process-level fact - hence the separate
 * ctest registrations in tests/CMakeLists.txt (`tests_oxr_view_space_mode_floor`
 * / `_mode_pinned` / `_mode_floor_off`). Under the plain registration these
 * arms have nothing to observe and say so.
 */
bool
sim_quad_requested()
{
	const char *v = std::getenv("SIM_DISPLAY_OUTPUT");
	return v != nullptr && std::strcmp(v, "quad") == 0 && env_on("SIM_DISPLAY_START_3D");
}

/*!
 * Was THIS process explicitly configured for a mode-floor arm? tests/CMakeLists.txt
 * sets DXR_TEST_ARM=<tag> on the dedicated ctest registrations. In such a
 * process a precondition mismatch is a mis-set environment and must FAIL - a
 * vacuous SUCCEED would hide it; in every other process the arm still says
 * "not mine" and succeeds.
 */
bool
configured_arm(const char *tag)
{
	const char *v = std::getenv("DXR_TEST_ARM");
	return v != nullptr && std::strcmp(v, tag) == 0;
}

//! #1499: is SIM_DISPLAY_FORCE_MODE armed (the device PINS its mode)? Under
//! ADR-065 only 0 and 1 pin; any other value is ignored by the device.
bool
sim_mode_pinned()
{
	const char *v = std::getenv("SIM_DISPLAY_FORCE_MODE");
	return v != nullptr && (std::strcmp(v, "0") == 0 || std::strcmp(v, "1") == 0);
}

/*!
 * #1499: is the DXR_MODE_FLOOR kill switch OFF for this process?
 *
 * Every arm below asserts the feature is ON, so each states its own
 * precondition rather than trusting the ctest arguments - running the bare
 * binary with the switch disabled must not look like a regression. The
 * runtime reads it through DEBUG_GET_ONCE_BOOL_OPTION, whose truth test is
 * "set and not 0/off/false/no"; mirrored loosely, as for the #1486 switch.
 */
bool
mode_floor_disabled()
{
	const char *v = std::getenv("DXR_MODE_FLOOR");
	if (v == nullptr || v[0] == '\0') {
		return false;
	}
	return std::strcmp(v, "0") == 0 || std::strcmp(v, "off") == 0 || std::strcmp(v, "false") == 0 ||
	       std::strcmp(v, "no") == 0;
}

/*
 *
 * Minimal pose math (float, matches the runtime's own conventions).
 *
 */

XrQuaternionf
qmul(const XrQuaternionf &a, const XrQuaternionf &b)
{
	return {
	    a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
	    a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
	    a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
	    a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
	};
}

XrQuaternionf
qconj(const XrQuaternionf &q)
{
	return {-q.x, -q.y, -q.z, q.w};
}

XrVector3f
qrot(const XrQuaternionf &q, const XrVector3f &v)
{
	XrQuaternionf p = {v.x, v.y, v.z, 0.0f};
	XrQuaternionf r = qmul(qmul(q, p), qconj(q));
	return {r.x, r.y, r.z};
}

//! out = a o b (apply b, then a).
XrPosef
pmul(const XrPosef &a, const XrPosef &b)
{
	XrVector3f p = qrot(a.orientation, b.position);
	return {qmul(a.orientation, b.orientation),
	        {p.x + a.position.x, p.y + a.position.y, p.z + a.position.z}};
}

XrPosef
pinv(const XrPosef &p)
{
	XrQuaternionf qi = qconj(p.orientation);
	XrVector3f t = qrot(qi, p.position);
	return {qi, {-t.x, -t.y, -t.z}};
}

float
vdist(const XrVector3f &a, const XrVector3f &b)
{
	float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

//! Angle between two orientations in degrees (sign-agnostic).
float
qangle_deg(const XrQuaternionf &a, const XrQuaternionf &b)
{
	float d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
	if (d > 1.0f) {
		d = 1.0f;
	}
	return 2.0f * std::acos(d) * 57.29577951f;
}

XrQuaternionf
qyaw(float rad)
{
	return {0.0f, std::sin(rad * 0.5f), 0.0f, std::cos(rad * 0.5f)};
}

std::string
pstr(const XrPosef &p)
{
	char buf[160];
	std::snprintf(buf, sizeof(buf), "pos=(%.4f,%.4f,%.4f) ori=(%.4f,%.4f,%.4f,%.4f)", p.position.x, p.position.y,
	              p.position.z, p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w);
	return buf;
}

/*
 *
 * Runtime bring-up.
 *
 */

struct Runtime
{
	PFN_xrGetInstanceProcAddr gipa = nullptr;
	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId system = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	XrSpace local = XR_NULL_HANDLE;
	XrSpace stage = XR_NULL_HANDLE;
	XrSpace view = XR_NULL_HANDLE;
	bool have_view_rig = false;
	bool have_display_info = false;
	bool have_hand_tracking = false; //!< #2189: XR_EXT_hand_tracking enabled
	//! XrViewActivityStateDXR::activeViewCount of the last views() call (0 = not reported).
	uint32_t last_active = 0;

	//! #1486: the view configuration this session was begun with. Every locate
	//! below names it, because a session locates in exactly one configuration.
	XrViewConfigurationType view_config = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

	PFN_xrLocateViews pfnLocateViews = nullptr;
	PFN_xrLocateSpace pfnLocateSpace = nullptr;

	template <typename T>
	T
	fn(const char *name)
	{
		PFN_xrVoidFunction f = nullptr;
		XrResult r = gipa(instance, name, &f);
		if (XR_FAILED(r) || f == nullptr) {
			FAIL("xrGetInstanceProcAddr(" << name << ") failed: " << r);
		}
		return reinterpret_cast<T>(f);
	}

	XrTime
	now()
	{
#if defined(_WIN32)
		auto convert = fn<PFN_xrConvertWin32PerformanceCounterToTimeKHR>("xrConvertWin32PerformanceCounterToTimeKHR");
		LARGE_INTEGER qpc;
		QueryPerformanceCounter(&qpc);
		XrTime t = 0;
		REQUIRE(XR_SUCCEEDED(convert(instance, &qpc, &t)));
		return t;
#else
		auto convert = fn<PFN_xrConvertTimespecTimeToTimeKHR>("xrConvertTimespecTimeToTimeKHR");
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		XrTime t = 0;
		REQUIRE(XR_SUCCEEDED(convert(instance, &ts, &t)));
		return t;
#endif
	}

	XrPosef
	locate(XrSpace space, XrSpace base, XrTime t)
	{
		XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
		REQUIRE(XR_SUCCEEDED(pfnLocateSpace(space, base, t, &loc)));
		const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
		REQUIRE((loc.locationFlags & need) == need);
		return loc.pose;
	}

	//! Locate the views in `base`; optionally chain a rig on the request and
	//! always chain the raw-result block so displayPlanePose is reported.
	std::vector<XrView>
	views(XrSpace base, XrTime t, const void *rig, XrViewDisplayRawDXR *raw)
	{
		XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
		li.viewConfigurationType = view_config;
		li.displayTime = t;
		li.space = base;
		li.next = rig;

		XrViewState vs = {XR_TYPE_VIEW_STATE};
		if (raw != nullptr) {
			*raw = {XR_TYPE_VIEW_DISPLAY_RAW_DXR};
			vs.next = raw;
		}
		XrViewActivityStateDXR act = {XR_TYPE_VIEW_ACTIVITY_STATE_DXR};
		if (have_display_info) {
			act.next = const_cast<void *>(vs.next);
			vs.next = &act;
		}

		uint32_t count = 0;
		REQUIRE(XR_SUCCEEDED(pfnLocateViews(session, &li, &vs, 0, &count, nullptr)));
		REQUIRE(count >= 1);
		std::vector<XrView> out(count, {XR_TYPE_VIEW});
		REQUIRE(XR_SUCCEEDED(pfnLocateViews(session, &li, &vs, count, &count, out.data())));
		const XrViewStateFlags need = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
		REQUIRE((vs.viewStateFlags & need) == need);
		last_active = have_display_info ? act.activeViewCount : 0;
		return out;
	}

	/*
	 *
	 * #1486 helpers.
	 *
	 */

	//! The two-call COUNT leg of xrLocateViews under an arbitrary view
	//! configuration. Returns the result instead of REQUIRE-ing success, so the
	//! negative arms can pin the exact error.
	XrResult
	locate_count(XrViewConfigurationType type, XrTime t, uint32_t *out_count)
	{
		// Value-initialised rather than the file's `{XR_TYPE_...}` idiom purely
		// so this addition adds no -Wmissing-field-initializers noise.
		XrViewLocateInfo li{};
		li.type = XR_TYPE_VIEW_LOCATE_INFO;
		li.viewConfigurationType = type;
		li.displayTime = t;
		li.space = local;

		XrViewState vs{};
		vs.type = XR_TYPE_VIEW_STATE;
		uint32_t count = 0;
		XrResult r = pfnLocateViews(session, &li, &vs, 0, &count, nullptr);
		if (out_count != nullptr) {
			*out_count = count;
		}
		return r;
	}

	std::vector<XrViewConfigurationType>
	view_configs()
	{
		auto pfn = fn<PFN_xrEnumerateViewConfigurations>("xrEnumerateViewConfigurations");
		uint32_t n = 0;
		REQUIRE(XR_SUCCEEDED(pfn(instance, system, 0, &n, nullptr)));
		std::vector<XrViewConfigurationType> out(n, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO);
		REQUIRE(XR_SUCCEEDED(pfn(instance, system, n, &n, out.data())));
		out.resize(n);
		return out;
	}

	//! Views that @p type reports, or 0 when the system does not advertise it.
	uint32_t
	config_view_count(XrViewConfigurationType type)
	{
		auto pfn = fn<PFN_xrEnumerateViewConfigurationViews>("xrEnumerateViewConfigurationViews");
		uint32_t n = 0;
		if (XR_FAILED(pfn(instance, system, type, 0, &n, nullptr))) {
			return 0;
		}
		return n;
	}

	//! The device's rendering-mode table as this session sees it. Empty when
	//! XR_DXR_display_info is not enabled on the instance.
	std::vector<XrDisplayRenderingModeInfoDXR>
	rendering_modes()
	{
		if (!have_display_info) {
			return {};
		}
		auto pfn = fn<PFN_xrEnumerateDisplayRenderingModesDXR>("xrEnumerateDisplayRenderingModesDXR");
		uint32_t n = 0;
		if (XR_FAILED(pfn(session, 0, &n, nullptr)) || n == 0) {
			return {};
		}
		XrDisplayRenderingModeInfoDXR proto{};
		proto.type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_DXR;
		std::vector<XrDisplayRenderingModeInfoDXR> modes(n, proto);
		if (XR_FAILED(pfn(session, n, &n, modes.data()))) {
			return {};
		}
		modes.resize(n);
		return modes;
	}

	//! The device's MAX view count across rendering modes, READ from the
	//! runtime (XR_DXR_display_info) rather than assumed. 0 when unavailable.
	uint32_t
	device_max_view_count()
	{
		uint32_t max = 0;
		for (const XrDisplayRenderingModeInfoDXR &m : rendering_modes()) {
			if (m.viewCount > max) {
				max = m.viewCount;
			}
		}
		return max;
	}

	/*!
	 * #1499: the ACTIVE mode, read back through the public API
	 * (`isActive`) rather than from any runtime-internal state. Returns
	 * false when nothing claims to be active.
	 */
	bool
	active_mode(XrDisplayRenderingModeInfoDXR *out)
	{
		for (const XrDisplayRenderingModeInfoDXR &m : rendering_modes()) {
			if (m.isActive == XR_TRUE) {
				if (out != nullptr) {
					*out = m;
				}
				return true;
			}
		}
		return false;
	}

	//! #1499: index of the first mode with more than @p max_views views, or -1.
	int32_t
	first_unfillable_mode(uint32_t max_views)
	{
		for (const XrDisplayRenderingModeInfoDXR &m : rendering_modes()) {
			if (m.viewCount > max_views) {
				return (int32_t)m.modeIndex;
			}
		}
		return -1;
	}

	/*!
	 * #1499: drain the event queue, keeping every event of interest. The
	 * runtime pushes onto an INSTANCE queue, so this is the only way to
	 * observe the mode change the floor performs during xrBeginSession.
	 */
	std::vector<XrEventDataBuffer>
	drain_events()
	{
		auto pfn = fn<PFN_xrPollEvent>("xrPollEvent");
		std::vector<XrEventDataBuffer> out;
		for (int i = 0; i < 64; i++) {
			XrEventDataBuffer ev{};
			ev.type = XR_TYPE_EVENT_DATA_BUFFER;
			XrResult r = pfn(instance, &ev);
			if (r != XR_SUCCESS) {
				break;
			}
			out.push_back(ev);
		}
		return out;
	}

	XrVector3f
	centroid(const std::vector<XrView> &v)
	{
		XrVector3f c = {0, 0, 0};
		for (const XrView &x : v) {
			c.x += x.pose.position.x;
			c.y += x.pose.position.y;
			c.z += x.pose.position.z;
		}
		const float inv = 1.0f / (float)v.size();
		return {c.x * inv, c.y * inv, c.z * inv};
	}
};

bool
load_gipa(Runtime &rt)
{
	const char *path = DXR_RUNTIME_LIB_PATH;
	PFN_xrNegotiateLoaderRuntimeInterface negotiate = nullptr;
#if defined(_WIN32)
	// The runtime library's siblings (cjson / pthread / SDL2 in the build
	// tree) resolve from ITS directory, not the test's - and the search
	// flags only honour a fully qualified path with BACKslashes (a forward-
	// slash path fails with ERROR_MOD_NOT_FOUND, verified).
	std::string winpath = path;
	for (char &c : winpath) {
		if (c == '/') {
			c = '\\';
		}
	}
	path = winpath.c_str();
	HMODULE mod = LoadLibraryExA(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	if (mod == nullptr) {
		WARN("LoadLibrary(" << path << ") failed: " << GetLastError());
		return false;
	}
	negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
	    GetProcAddress(mod, "xrNegotiateLoaderRuntimeInterface"));
#else
	void *mod = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (mod == nullptr) {
		WARN("dlopen(" << path << ") failed: " << dlerror());
		return false;
	}
	negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(dlsym(mod, "xrNegotiateLoaderRuntimeInterface"));
#endif
	REQUIRE(negotiate != nullptr);

	XrNegotiateLoaderInfo li = {};
	li.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
	li.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
	li.structSize = sizeof(li);
	li.minInterfaceVersion = 1;
	li.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
	li.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
	li.maxApiVersion = XR_CURRENT_API_VERSION;

	XrNegotiateRuntimeRequest req = {};
	req.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
	req.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
	req.structSize = sizeof(req);

	REQUIRE(XR_SUCCEEDED(negotiate(&li, &req)));
	REQUIRE(req.getInstanceProcAddr != nullptr);
	rt.gipa = req.getInstanceProcAddr;
	return true;
}

//! #1486: what the caller wants the instance/session to look like. The defaults
//! reproduce the original #1370 bring-up exactly.
struct BringUp
{
	//! Enable XR_DXR_display_info on the instance (gates PRIMARY_MULTIVIEW_DXR).
	bool display_info = false;
	//! The primaryViewConfigurationType handed to xrBeginSession.
	XrViewConfigurationType begin = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	//! #2189: enable XR_EXT_hand_tracking when the runtime advertises it.
	bool hand_tracking = false;
	//! ADR-034 Amendment 8: enable XR_LOGITECH_mx_ink_stylus_interaction
	//! (REQUIRED to be advertised when set).
	bool mx_ink = false;
};

//! Returns false (after SKIP-ing the test) when the runtime cannot come up
//! headlessly - no display processor registered on this box.
bool
bring_up(Runtime &rt, const BringUp &opt = BringUp{})
{
	if (!load_gipa(rt)) {
		SKIP("runtime library not loadable: " DXR_RUNTIME_LIB_PATH);
		return false;
	}

	auto enumExt = rt.fn<PFN_xrEnumerateInstanceExtensionProperties>("xrEnumerateInstanceExtensionProperties");
	uint32_t n = 0;
	REQUIRE(XR_SUCCEEDED(enumExt(nullptr, 0, &n, nullptr)));
	std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
	REQUIRE(XR_SUCCEEDED(enumExt(nullptr, n, &n, props.data())));
	auto has = [&](const char *name) {
		for (const auto &p : props) {
			if (std::strcmp(p.extensionName, name) == 0) {
				return true;
			}
		}
		return false;
	};
	REQUIRE(has(XR_MND_HEADLESS_EXTENSION_NAME));
	rt.have_view_rig = has(XR_DXR_VIEW_RIG_EXTENSION_NAME);
	rt.have_display_info = opt.display_info && has(XR_DXR_DISPLAY_INFO_EXTENSION_NAME);

	std::vector<const char *> exts = {XR_MND_HEADLESS_EXTENSION_NAME};
#if defined(_WIN32)
	REQUIRE(has(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME));
	exts.push_back(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
#else
	REQUIRE(has(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME));
	exts.push_back(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME);
#endif
	if (rt.have_view_rig) {
		exts.push_back(XR_DXR_VIEW_RIG_EXTENSION_NAME);
	}
	if (rt.have_display_info) {
		exts.push_back(XR_DXR_DISPLAY_INFO_EXTENSION_NAME);
	}
	if (opt.mx_ink) {
		REQUIRE(has(XR_LOGITECH_MX_INK_STYLUS_INTERACTION_EXTENSION_NAME));
		exts.push_back(XR_LOGITECH_MX_INK_STYLUS_INTERACTION_EXTENSION_NAME);
	}
	rt.have_hand_tracking = opt.hand_tracking && has(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
	if (rt.have_hand_tracking) {
		exts.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
	}

	XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
	std::snprintf(ici.applicationInfo.applicationName, sizeof(ici.applicationInfo.applicationName),
	              "tests_oxr_view_space");
	ici.applicationInfo.applicationVersion = 1;
	std::snprintf(ici.applicationInfo.engineName, sizeof(ici.applicationInfo.engineName), "catch2");
	ici.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	ici.enabledExtensionCount = (uint32_t)exts.size();
	ici.enabledExtensionNames = exts.data();

	auto createInstance = rt.fn<PFN_xrCreateInstance>("xrCreateInstance");
	XrResult r = createInstance(&ici, &rt.instance);
	if (XR_FAILED(r)) {
		// No display processor registered (the Windows registry-only discovery,
		// #1201's dims proof, ...) - the environment, not the runtime under test.
		SKIP("xrCreateInstance failed (" << r << ") - no display processor registered? "
		                                  "Register the sim-display plug-in (scripts/register_dev_plugin.bat) "
		                                  "to run this suite.");
		return false;
	}

	XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	auto getSystem = rt.fn<PFN_xrGetSystem>("xrGetSystem");
	REQUIRE(XR_SUCCEEDED(getSystem(rt.instance, &sgi, &rt.system)));

	XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
	sci.systemId = rt.system;
	auto createSession = rt.fn<PFN_xrCreateSession>("xrCreateSession");
	REQUIRE(XR_SUCCEEDED(createSession(rt.instance, &sci, &rt.session)));

	XrSessionBeginInfo sbi = {XR_TYPE_SESSION_BEGIN_INFO};
	sbi.primaryViewConfigurationType = opt.begin;
	rt.view_config = opt.begin;
	auto beginSession = rt.fn<PFN_xrBeginSession>("xrBeginSession");
	REQUIRE(XR_SUCCEEDED(beginSession(rt.session, &sbi)));

	auto createSpace = rt.fn<PFN_xrCreateReferenceSpace>("xrCreateReferenceSpace");
	XrReferenceSpaceCreateInfo rci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	rci.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
	rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	REQUIRE(XR_SUCCEEDED(createSpace(rt.session, &rci, &rt.local)));
	rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
	REQUIRE(XR_SUCCEEDED(createSpace(rt.session, &rci, &rt.stage)));
	rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	REQUIRE(XR_SUCCEEDED(createSpace(rt.session, &rci, &rt.view)));

	rt.pfnLocateViews = rt.fn<PFN_xrLocateViews>("xrLocateViews");
	rt.pfnLocateSpace = rt.fn<PFN_xrLocateSpace>("xrLocateSpace");
	return true;
}

void
tear_down(Runtime &rt)
{
	if (rt.instance != XR_NULL_HANDLE) {
		auto destroyInstance = rt.fn<PFN_xrDestroyInstance>("xrDestroyInstance");
		destroyInstance(rt.instance);
		rt.instance = XR_NULL_HANDLE;
	}
}

struct Base
{
	const char *name;
	XrSpace space;
};

} // namespace

TEST_CASE("xrLocateViews honours the base space (#1370)", "[oxr][view_space]")
{
	Runtime rt;
	if (!bring_up(rt)) {
		return;
	}

	const XrTime t = rt.now();
	const Base bases[] = {{"LOCAL", rt.local}, {"STAGE", rt.stage}, {"VIEW", rt.view}};

	SECTION("legacy path: eye centroid is base-invariant relative to VIEW")
	{
		bool have_ref = false;
		XrVector3f ref_offset = {0, 0, 0};
		bool have_plane_ref = false;
		XrVector3f ref_plane_offset = {0, 0, 0};
		for (const Base &b : bases) {
			INFO("base = " << b.name);
			XrViewDisplayRawDXR raw = {XR_TYPE_VIEW_DISPLAY_RAW_DXR};
			std::vector<XrView> v = rt.views(b.space, t, nullptr, rt.have_view_rig ? &raw : nullptr);
			const XrPosef T_base_view = rt.locate(rt.view, b.space, t);
			INFO("VIEW in base: " << pstr(T_base_view));

			// Every view carries the head orientation in this base.
			for (size_t i = 0; i < v.size(); i++) {
				INFO("view " << i << ": " << pstr(v[i].pose));
				CHECK(qangle_deg(v[i].pose.orientation, T_base_view.orientation) < kAngTolDeg);
			}

			// The centroid, re-expressed in the VIEW frame, is the same
			// vector whatever the base.
			XrVector3f c = rt.centroid(v);
			XrVector3f d = {c.x - T_base_view.position.x, c.y - T_base_view.position.y, c.z - T_base_view.position.z};
			XrVector3f off = qrot(qconj(T_base_view.orientation), d);
			INFO("centroid offset in VIEW frame = (" << off.x << "," << off.y << "," << off.z << ")");
			if (!have_ref) {
				ref_offset = off;
				have_ref = true;
			} else {
				CHECK(vdist(off, ref_offset) < kPosTolM);
			}

			// The display plane reported by the raw channel is the head
			// device pose (legacy: the plane IS the head) - in this base.
			// #1502 moved VIEW off the plane onto the eye centroid, so the
			// plane is no longer AT the VIEW origin; what #1370 pins is that
			// it is reported in the LOCATE space, i.e. that its offset from
			// VIEW, read in the VIEW frame, does not depend on the base.
			if (rt.have_view_rig) {
				INFO("displayPlanePose: " << pstr(raw.displayPlanePose));
				CHECK(qangle_deg(raw.displayPlanePose.orientation, T_base_view.orientation) < kAngTolDeg);
				XrVector3f pd = {raw.displayPlanePose.position.x - T_base_view.position.x,
				                 raw.displayPlanePose.position.y - T_base_view.position.y,
				                 raw.displayPlanePose.position.z - T_base_view.position.z};
				XrVector3f poff = qrot(qconj(T_base_view.orientation), pd);
				INFO("plane offset in VIEW frame = (" << poff.x << "," << poff.y << "," << poff.z << ")");
				if (!have_plane_ref) {
					ref_plane_offset = poff;
					have_plane_ref = true;
				} else {
					CHECK(vdist(poff, ref_plane_offset) < kPosTolM);
				}
			}
		}
	}

	SECTION("chained camera rig: LOCAL and STAGE round-trip by exactly the LOCAL offset")
	{
		if (!rt.have_view_rig) {
			SKIP("XR_DXR_view_rig not advertised");
		}

		// T_stage_local: LOCAL's pose in STAGE, READ from the runtime.
		const XrPosef T_stage_local = rt.locate(rt.local, rt.stage, t);
		INFO("LOCAL in STAGE: " << pstr(T_stage_local));

		// An off-origin, yawed camera so the orientation leg is exercised too.
		XrCameraRigDXR rig = {XR_TYPE_CAMERA_RIG_DXR};
		rig.pose = {qyaw(0.35f), {0.25f, -0.10f, 0.40f}};
		rig.ipdFactor = 1.0f;
		rig.parallaxFactor = 1.0f;
		rig.convergenceDiopters = 0.5f;
		rig.verticalFov = 0.8f;
		rig.metersToVirtual = 1.0f;

		XrViewDisplayRawDXR rawL = {XR_TYPE_VIEW_DISPLAY_RAW_DXR};
		std::vector<XrView> vL = rt.views(rt.local, t, &rig, &rawL);

		// The same physical camera, expressed in STAGE.
		XrCameraRigDXR rigS = rig;
		rigS.pose = pmul(T_stage_local, rig.pose);
		XrViewDisplayRawDXR rawS = {XR_TYPE_VIEW_DISPLAY_RAW_DXR};
		std::vector<XrView> vS = rt.views(rt.stage, t, &rigS, &rawS);

		REQUIRE(vL.size() == vS.size());
		for (size_t i = 0; i < vL.size(); i++) {
			const XrPosef expect = pmul(T_stage_local, vL[i].pose);
			INFO("view " << i << " LOCAL: " << pstr(vL[i].pose) << "\n  STAGE: " << pstr(vS[i].pose)
			             << "\n  expect: " << pstr(expect));
			CHECK(vdist(vS[i].pose.position, expect.position) < kPosTolM);
			CHECK(qangle_deg(vS[i].pose.orientation, expect.orientation) < kAngTolDeg);
			// A camera rig's views carry the camera orientation.
			CHECK(qangle_deg(vL[i].pose.orientation, rig.pose.orientation) < kAngTolDeg);
		}

		// displayPlanePose is the chained rig pose, in the locate space.
		INFO("displayPlanePose LOCAL: " << pstr(rawL.displayPlanePose) << " STAGE: " << pstr(rawS.displayPlanePose));
		CHECK(vdist(rawL.displayPlanePose.position, rig.pose.position) < kPosTolM);
		CHECK(qangle_deg(rawL.displayPlanePose.orientation, rig.pose.orientation) < kAngTolDeg);
		CHECK(vdist(rawS.displayPlanePose.position, rigS.pose.position) < kPosTolM);
		CHECK(qangle_deg(rawS.displayPlanePose.orientation, rigS.pose.orientation) < kAngTolDeg);

		// And the same rig posed in VIEW: the views sit where the LOCAL ones
		// do once VIEW's pose in LOCAL is composed in (three bases, one rig).
		const XrPosef T_local_view = rt.locate(rt.view, rt.local, t);
		XrCameraRigDXR rigV = rig;
		rigV.pose = pmul(pinv(T_local_view), rig.pose);
		std::vector<XrView> vV = rt.views(rt.view, t, &rigV, nullptr);
		REQUIRE(vV.size() == vL.size());
		for (size_t i = 0; i < vV.size(); i++) {
			const XrPosef expect = pmul(pinv(T_local_view), vL[i].pose);
			INFO("view " << i << " VIEW: " << pstr(vV[i].pose) << "\n  expect: " << pstr(expect));
			CHECK(vdist(vV[i].pose.position, expect.position) < kPosTolM);
			CHECK(qangle_deg(vV[i].pose.orientation, expect.orientation) < kAngTolDeg);
		}
	}

	tear_down(rt);
}

/*
 * #1502: VIEW is the centroid of the located view origins.
 *
 * The OpenXR spec defines XR_REFERENCE_SPACE_TYPE_VIEW as the view origin, or
 * the centroid of the view origins when there is more than one - which is what
 * CTS 1.1.63's xrLocateSpace_xrLocateViews asserts at test_xrLocateSpace.cpp:330.
 * DisplayXR's head device is the display PLANE, and the eyes sit off it by the
 * nominal viewer position minus the window-centre offset (ADR-012), so before
 * #1502 the two disagreed by exactly that vector - measured on the win box as
 * (0, 0.1025, 0) once the CTS harness's DPI artefact (#1506) was removed.
 *
 * SCOPE, stated plainly because it decides what a green run here proves.
 * Headless in-process the published offset is structurally ZERO: there is no
 * window (so no window-centre term) and no DP instance reporting eyes (QTRACE
 * shows `eyes=0`), so xrLocateViews takes the UNTRACKED nominal pair, whose y
 * is deliberately 0 - "nominal_y must NOT leak into the eye", oxr_session.c -
 * and whose z cancels against nominal_z. The deviation #1502 fixes therefore
 * only EXISTS in a DP-backed, windowed session.
 *
 * So these arms pin the PLUMBING, not the magnitude: that the offset is applied
 * on both legs (target and base), that VIEW-in-X and X-in-VIEW stay inverses,
 * that VIEW-in-VIEW is the identity, that VIEW keeps the head orientation, and
 * that a chained rig does not drag VIEW along. They would catch a one-leg
 * application, a wrong composition order, or a rig leaking into VIEW. The
 * MAGNITUDE leg - centroid == VIEW with a non-zero offset - is the CTS run on
 * hardware (`xrLocateSpace_xrLocateViews`, which must stop reporting
 * `(0, 0.1025, 0) == Approx((0, 0, 0))`).
 */
TEST_CASE("VIEW is the centroid of the located views (#1502)", "[oxr][view_space][view_centroid]")
{
	Runtime rt;
	if (!bring_up(rt)) {
		return;
	}

	const XrTime t = rt.now();
	const Base bases[] = {{"LOCAL", rt.local}, {"STAGE", rt.stage}, {"VIEW", rt.view}};

	SECTION("VIEW equals the eye centroid in every base")
	{
		for (const Base &b : bases) {
			INFO("base = " << b.name);
			// No rig chained: the default/legacy locate, which is the only
			// one that publishes the offset (a rig is the app's OWN camera).
			std::vector<XrView> v = rt.views(b.space, t, nullptr, nullptr);
			const XrPosef T_base_view = rt.locate(rt.view, b.space, t);
			const XrVector3f c = rt.centroid(v);
			INFO("VIEW in base: " << pstr(T_base_view));
			INFO("centroid = (" << c.x << "," << c.y << "," << c.z << ")");
			CHECK(vdist(c, T_base_view.position) < kPosTolM);
			// VIEW keeps the head orientation - the offset is a translation.
			for (size_t i = 0; i < v.size(); i++) {
				INFO("view " << i << ": " << pstr(v[i].pose));
				CHECK(qangle_deg(v[i].pose.orientation, T_base_view.orientation) < kAngTolDeg);
			}
		}
	}

	SECTION("locating VIEW in itself is the identity, and the relation inverts")
	{
		(void)rt.views(rt.local, t, nullptr, nullptr);

		const XrPosef T_view_view = rt.locate(rt.view, rt.view, t);
		INFO("VIEW in VIEW: " << pstr(T_view_view));
		CHECK(vdist(T_view_view.position, XrVector3f{0, 0, 0}) < kPosTolM);

		const XrPosef T_local_view = rt.locate(rt.view, rt.local, t);
		const XrPosef T_view_local = rt.locate(rt.local, rt.view, t);
		const XrPosef round = pmul(T_local_view, T_view_local);
		INFO("VIEW in LOCAL: " << pstr(T_local_view) << "\n  LOCAL in VIEW: " << pstr(T_view_local)
		                       << "\n  round trip: " << pstr(round));
		CHECK(vdist(round.position, XrVector3f{0, 0, 0}) < kPosTolM);
		CHECK(qangle_deg(round.orientation, XrQuaternionf{0, 0, 0, 1}) < kAngTolDeg);
	}

	SECTION("a chained rig places the app's camera, not VIEW")
	{
		if (!rt.have_view_rig) {
			SKIP("XR_DXR_view_rig not advertised");
		}

		// Baseline: the default locate publishes the viewer's centroid.
		(void)rt.views(rt.local, t, nullptr, nullptr);
		const XrPosef before = rt.locate(rt.view, rt.local, t);

		// A rig half a metre away. Its views are nowhere near the viewer, and
		// VIEW must NOT follow them - otherwise VIEW would flip-flop between
		// an app's rig frames and its plain ones.
		XrCameraRigDXR rig = {XR_TYPE_CAMERA_RIG_DXR};
		rig.pose = {qyaw(0.35f), {0.5f, 0.25f, -0.75f}};
		rig.ipdFactor = 1.0f;
		rig.parallaxFactor = 1.0f;
		rig.convergenceDiopters = 0.5f;
		rig.verticalFov = 0.8f;
		rig.metersToVirtual = 1.0f;
		(void)rt.views(rt.local, t, &rig, nullptr);

		const XrPosef after = rt.locate(rt.view, rt.local, t);
		INFO("VIEW before rig: " << pstr(before) << "\n  after rig: " << pstr(after));
		CHECK(vdist(before.position, after.position) < kPosTolM);
		CHECK(qangle_deg(before.orientation, after.orientation) < kAngTolDeg);
	}

	tear_down(rt);
}

/*
 * #1502 + #1486 + multi-screen M3: the centroid is over the ACTIVE views.
 *
 * Under PRIMARY_MULTIVIEW_DXR the session reports the device max (times the
 * view-set capacity, M3) while the active rendering mode may be narrower;
 * xrLocateViews fills the surplus slots by duplicating view 0. Those aliases
 * carry no content and would drag the centroid toward view 0 (more so now that
 * the tail can be 3x the active set), so VIEW is the mean of the ACTIVE views
 * — (L+R)/2 for a stereo mode, exactly as a PRIMARY_STEREO session gets.
 *
 * This arm only has teeth where the device max exceeds the active mode's view
 * count; on a box whose sim-display tops out at 2 (Quad not enabled) it is the
 * stereo arm again. Reading the max from the runtime rather than assuming 4 is
 * what keeps it honest either way.
 */
TEST_CASE("VIEW tracks the reported view array under MULTIVIEW (#1502)", "[oxr][view_space][view_centroid]")
{
	if (legacy_switch_set()) {
		// SUCCEED, not SKIP: build-windows.yml reads any "SKIPPED:" from this
		// binary as "the headless runtime did not come up" (#1370).
		WARN("DXR_VIEW_CONFIG_LEGACY is armed - PRIMARY_MULTIVIEW_DXR is not advertised");
		SUCCEED("legacy process; nothing to pin here");
		return;
	}

	Runtime rt;
	BringUp opt;
	opt.display_info = true;
	opt.begin = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR;
	if (!bring_up(rt, opt)) {
		return;
	}
	if (!rt.have_display_info) {
		SKIP("XR_DXR_display_info not advertised");
		tear_down(rt);
		return;
	}

	const XrTime t = rt.now();
	std::vector<XrView> v = rt.views(rt.local, t, nullptr, nullptr);
	const uint32_t device_max = rt.device_max_view_count();
	INFO("reported views = " << v.size() << ", device max = " << device_max);
	if (device_max != 0) {
		CHECK(is_multiview_count(v.size(), device_max));
	}

	const XrPosef T_local_view = rt.locate(rt.view, rt.local, t);
	REQUIRE(rt.last_active > 0);
	REQUIRE(rt.last_active <= v.size());
	const std::vector<XrView> active(v.begin(), v.begin() + rt.last_active);
	const XrVector3f c = rt.centroid(active);
	INFO("VIEW in LOCAL: " << pstr(T_local_view) << ", active views " << rt.last_active);
	INFO("centroid = (" << c.x << "," << c.y << "," << c.z << ")");
	CHECK(vdist(c, T_local_view.position) < kPosTolM);

	tear_down(rt);
}

TEST_CASE("XR_DXR_display_info advertises PRIMARY_MULTIVIEW_DXR (#1486)", "[oxr][view_space][view_config]")
{
	if (legacy_switch_set()) {
		// SUCCEED, not SKIP: build-windows.yml reads any "SKIPPED:" from this
		// binary as "the headless runtime did not come up" (#1370).
		WARN("DXR_VIEW_CONFIG_LEGACY is armed - this arm pins the NEW mapping");
		SUCCEED("legacy process; nothing to pin here");
		return;
	}

	Runtime rt;
	BringUp opt;
	opt.display_info = true;
	opt.begin = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR;
	if (!bring_up(rt, opt)) {
		return;
	}
	if (!rt.have_display_info) {
		SKIP("XR_DXR_display_info not advertised by this build");
	}

	const uint32_t device_max = rt.device_max_view_count();
	REQUIRE(device_max >= 2); // A 1-view device advertises PRIMARY_MONO instead.
	INFO("device max view count across rendering modes = " << device_max);

	SECTION("the list is PRIMARY_STEREO first, PRIMARY_MULTIVIEW_DXR second")
	{
		const std::vector<XrViewConfigurationType> cfgs = rt.view_configs();
		REQUIRE(cfgs.size() == 2);
		CHECK(cfgs[0] == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
		CHECK(cfgs[1] == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR);
	}

	SECTION("each type reports its OWN view count")
	{
		// PRIMARY_STEREO means exactly 2, whatever the device can drive.
		CHECK(rt.config_view_count(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) == 2);
		CHECK(is_multiview_count(rt.config_view_count(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR),
		                         device_max));
		// A valid core type the system does NOT advertise is still refused.
		CHECK(rt.config_view_count(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO) == 0);
	}

	SECTION("a MULTIVIEW session locates the device max and refuses PRIMARY_STEREO")
	{
		const XrTime t = rt.now();

		uint32_t n = 0;
		CHECK(rt.locate_count(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR, t, &n) == XR_SUCCESS);
		CHECK(is_multiview_count(n, device_max));

		// Same session, the OTHER advertised configuration: valid enum, wrong
		// session - UNSUPPORTED, not VALIDATION_FAILURE.
		CHECK(rt.locate_count(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, t, nullptr) ==
		      XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED);

		// And the full locate really writes that many views.
		std::vector<XrView> v = rt.views(rt.local, t, nullptr, nullptr);
		CHECK(is_multiview_count(v.size(), device_max));
	}

	tear_down(rt);
}

TEST_CASE("PRIMARY_STEREO reports exactly 2 views on a wider device (#1486)", "[oxr][view_space][view_config]")
{
	if (legacy_switch_set()) {
		// SUCCEED, not SKIP: build-windows.yml reads any "SKIPPED:" from this
		// binary as "the headless runtime did not come up" (#1370).
		WARN("DXR_VIEW_CONFIG_LEGACY is armed - this arm pins the NEW mapping");
		SUCCEED("legacy process; nothing to pin here");
		return;
	}

	Runtime rt;
	BringUp opt;
	opt.display_info = true;
	opt.begin = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	if (!bring_up(rt, opt)) {
		return;
	}
	if (!rt.have_display_info) {
		SKIP("XR_DXR_display_info not advertised by this build");
	}

	const uint32_t device_max = rt.device_max_view_count();
	INFO("device max view count across rendering modes = " << device_max);
	if (device_max <= 2) {
		WARN("device max is " << device_max
		                      << " - this arm only PROVES the tightening on a "
		                         "device with a >2-view rendering mode (sim_display's Quad)");
	}

	const XrTime t = rt.now();
	uint32_t n = 0;
	REQUIRE(rt.locate_count(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, t, &n) == XR_SUCCESS);
	CHECK(n == 2);

	std::vector<XrView> v = rt.views(rt.local, t, nullptr, nullptr);
	CHECK(v.size() == 2);

	// The opt-in type is advertised but this session did not begin with it.
	CHECK(rt.locate_count(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR, t, nullptr) ==
	      XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED);

	tear_down(rt);
}

TEST_CASE("without XR_DXR_display_info the MULTIVIEW type does not exist (#1486)", "[oxr][view_space][view_config]")
{
	if (legacy_switch_set()) {
		// SUCCEED, not SKIP: build-windows.yml reads any "SKIPPED:" from this
		// binary as "the headless runtime did not come up" (#1370).
		WARN("DXR_VIEW_CONFIG_LEGACY is armed - this arm pins the NEW mapping");
		SUCCEED("legacy process; nothing to pin here");
		return;
	}

	Runtime rt;
	if (!bring_up(rt)) { // no XR_DXR_display_info, begun on PRIMARY_STEREO
		return;
	}

	SECTION("exactly one view configuration is advertised")
	{
		const std::vector<XrViewConfigurationType> cfgs = rt.view_configs();
		REQUIRE(cfgs.size() == 1);
		CHECK(cfgs[0] == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
		CHECK(rt.config_view_count(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR) == 0);
	}

	SECTION("the enum is not a valid VALUE, so it fails validation")
	{
		/*
		 * XR_ERROR_VALIDATION_FAILURE, not
		 * XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED: an extension enum
		 * whose extension is not enabled is not a legal value at all (the
		 * whitelist in oxr_verify_view_config_type).
		 *
		 * This is asserted through xrLocateViews rather than xrBeginSession
		 * because a HEADLESS session must IGNORE primaryViewConfigurationType
		 * per XR_MND_headless - xrBeginSession does not validate it there, and
		 * a graphics-bound session needs a GPU and a window, which this suite
		 * deliberately does not have. The graphics-bound xrBeginSession leg is
		 * on the hardware eyeball list.
		 */
		CHECK(rt.locate_count(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR, rt.now(), nullptr) ==
		      XR_ERROR_VALIDATION_FAILURE);
	}

	tear_down(rt);
}

/*
 * #1486 kill switch. SEPARATE PROCESS by construction — see the file header —
 * registered by tests/CMakeLists.txt as `tests_oxr_view_space_legacy` with
 * DXR_VIEW_CONFIG_LEGACY=1 in its environment and this tag as the filter.
 */
TEST_CASE("DXR_VIEW_CONFIG_LEGACY restores the pre-#1486 mapping", "[oxr][view_space][view_config_legacy]")
{
	if (!legacy_switch_set()) {
		// Deliberately NOT a Catch2 SKIP: build-windows.yml treats any
		// "SKIPPED:" from this binary (with sim-display registered) as "the
		// headless runtime did not come up" (#1370). Under the plain
		// registration this case simply has nothing to do.
		WARN(
		    "DXR_VIEW_CONFIG_LEGACY is not set - this case only runs under the "
		    "tests_oxr_view_space_legacy ctest, which arms it in a process of its own "
		    "(the runtime caches the read)");
		SUCCEED("not the legacy process; nothing to pin here");
		return;
	}

	Runtime rt;
	BringUp opt;
	// The extension IS enabled: the whole point is that the kill switch
	// suppresses PRIMARY_MULTIVIEW_DXR even then.
	opt.display_info = true;
	opt.begin = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	if (!bring_up(rt, opt)) {
		return;
	}
	if (!rt.have_display_info) {
		SKIP("XR_DXR_display_info not advertised by this build");
	}

	const uint32_t device_max = rt.device_max_view_count();
	REQUIRE(device_max >= 2);
	INFO("device max view count across rendering modes = " << device_max);
	if (device_max <= 2) {
		WARN("device max is " << device_max
		                      << " - the legacy mapping is only DISTINGUISHABLE from the "
		                         "new one on a device with a >2-view rendering mode "
		                         "(sim_display's Quad)");
	}

	// One entry, PRIMARY_STEREO, DESPITE the extension being enabled.
	const std::vector<XrViewConfigurationType> cfgs = rt.view_configs();
	REQUIRE(cfgs.size() == 1);
	CHECK(cfgs[0] == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);

	// ...and it reports the DEVICE MAX, which is exactly the non-conformance
	// the switch exists to preserve for already-shipped apps.
	CHECK(rt.config_view_count(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) == device_max);
	CHECK(rt.config_view_count(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR) == 0);

	const XrTime t = rt.now();
	uint32_t n = 0;
	REQUIRE(rt.locate_count(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, t, &n) == XR_SUCCESS);
	CHECK(n == device_max);

	std::vector<XrView> v = rt.views(rt.local, t, nullptr, nullptr);
	CHECK(v.size() == device_max);

	// The opt-in type is a valid ENUM (the extension is enabled) but this
	// system does not advertise it, so the locate is refused as UNSUPPORTED
	// rather than as a validation failure.
	CHECK(rt.locate_count(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR, t, nullptr) ==
	      XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED);

	tear_down(rt);
}

/*
 * #1499 — the mode floor, end to end through the public API.
 *
 * SEPARATE PROCESSES by construction, like the #1486 kill-switch arm above:
 * sim-display's 3D kind (`SIM_DISPLAY_OUTPUT`), its start-up mode
 * (`SIM_DISPLAY_START_3D`) and its pin (`SIM_DISPLAY_FORCE_MODE`) are all read
 * once, when the plug-in creates its device, so one binary cannot host both the
 * floored and the pinned case. tests/CMakeLists.txt registers this file three
 * more times: `tests_oxr_view_space_mode_floor` (OUTPUT=quad START_3D=1, tag
 * `[mode_floor]`), `tests_oxr_view_space_mode_pinned` (OUTPUT=quad
 * FORCE_MODE=1, tag `[mode_pinned]`) and `_mode_floor_off`. Each pins
 * DXR_PLUGIN_EXCLUSIVE=sim-display so a box that also has a vendor plug-in
 * registered still runs them against sim-display.
 *
 * ADR-065: the device serves {2D, Quad}. A stereo session therefore has no
 * 2-view 3D mode to fall back to and is floored all the way to 2D - the
 * user-visible cost of choosing a 4-view kind, and the reason it is called an
 * inspection kind.
 *
 * Under the plain registration these cases have no >2-view active mode to
 * observe, so they SUCCEED with a WARN — deliberately not a Catch2 SKIP, for
 * the same reason as the #1486 arms (build-windows.yml reads any "SKIPPED:"
 * from this binary as "the headless runtime did not come up").
 */
TEST_CASE("a PRIMARY_STEREO session is floored out of a >2-view mode (#1499)", "[oxr][view_space][mode_floor]")
{
	if (legacy_switch_set() || mode_floor_disabled() || !sim_quad_requested() || sim_mode_pinned()) {
		WARN(
		    "this arm needs SIM_DISPLAY_OUTPUT=quad SIM_DISPLAY_START_3D=1, an UNpinned device and "
		    "DXR_MODE_FLOOR on - see tests_oxr_view_space_mode_floor in tests/CMakeLists.txt");
		if (configured_arm("mode_floor")) {
			FAIL(
			    "this process is configured for [mode_floor] (DXR_TEST_ARM) but its "
			    "preconditions do not hold - a mis-set environment, not a pass");
		}
		SUCCEED("not the mode-floor process; nothing to pin here");
		return;
	}

	Runtime rt;
	BringUp opt;
	opt.display_info = true;
	opt.begin = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	if (!bring_up(rt, opt)) {
		return;
	}
	if (!rt.have_display_info) {
		SKIP("XR_DXR_display_info not advertised by this build");
	}

	const uint32_t device_max = rt.device_max_view_count();
	REQUIRE(device_max > 2); // SIM_DISPLAY_OUTPUT=quad must have taken.
	INFO("device max view count across rendering modes = " << device_max);

	SECTION("the device serves exactly two modes: 2D and the quad kind (ADR-065)")
	{
		const std::vector<XrDisplayRenderingModeInfoDXR> modes = rt.rendering_modes();
		REQUIRE(modes.size() == 2);
		CHECK(modes[0].modeIndex == 0);
		CHECK(modes[0].viewCount == 1);
		CHECK(modes[0].hardwareDisplay3D == XR_FALSE);
		CHECK(modes[1].modeIndex == 1);
		CHECK(modes[1].viewCount == device_max);
		CHECK(modes[1].hardwareDisplay3D == XR_TRUE);
	}

	SECTION("after xrBeginSession the active mode is one this session can fill")
	{
		XrDisplayRenderingModeInfoDXR active{};
		REQUIRE(rt.active_mode(&active));
		INFO("active mode " << active.modeIndex << " '" << active.modeName << "' viewCount "
		                    << active.viewCount);
		// THE assertion: a 2-view session is not left sitting in Quad.
		CHECK(active.viewCount <= 2);
		// ...and with only {2D, Quad} there is no 2-view 3D mode to keep it in
		// 3D: it is floored to the 2D mode (ADR-065). Before the two-mode
		// contract it fell back to Anaglyph and stayed 3D.
		CHECK(active.modeIndex == 0u);
		CHECK(active.hardwareDisplay3D == XR_FALSE);
	}

	SECTION("the app is TOLD exactly once, and told the truth")
	{
		// Apps enumerate the modes before xrBeginSession, so the floor has to
		// announce itself; the event is queued during xrBeginSession and this
		// is the first poll.
		uint32_t mode_changes = 0;
		uint32_t changed_from = 0;
		uint32_t changed_to = 0;
		for (const XrEventDataBuffer &ev : rt.drain_events()) {
			if (ev.type == XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_DXR) {
				const auto *rm = reinterpret_cast<const XrEventDataRenderingModeChangedDXR *>(&ev);
				mode_changes++;
				changed_from = rm->previousModeIndex;
				changed_to = rm->currentModeIndex;
				INFO("rendering mode changed " << rm->previousModeIndex << " -> "
				                               << rm->currentModeIndex);
			}
		}
		// EXACTLY one: a second event would mean the floor ran twice, or that
		// something else moved the mode behind it.
		REQUIRE(mode_changes == 1);
		// previousModeIndex is the mode the floor took us OUT of - Quad, mode 1,
		// which OUTPUT=quad START_3D=1 started us in; the floor's target is 2D.
		CHECK(changed_from == 1u);
		CHECK(changed_to == 0u);

		XrDisplayRenderingModeInfoDXR active{};
		REQUIRE(rt.active_mode(&active));
		// The event and the enumerator agree - the whole point of pushing it.
		CHECK(changed_to == active.modeIndex);
		CHECK(changed_to != changed_from);
	}

	tear_down(rt);
}

TEST_CASE("a PRIMARY_MULTIVIEW_DXR session keeps the >2-view mode (#1499)", "[oxr][view_space][mode_floor]")
{
	if (legacy_switch_set() || mode_floor_disabled() || !sim_quad_requested() || sim_mode_pinned()) {
		WARN(
		    "this arm needs SIM_DISPLAY_OUTPUT=quad SIM_DISPLAY_START_3D=1, an UNpinned device and "
		    "DXR_MODE_FLOOR on - see tests_oxr_view_space_mode_floor in tests/CMakeLists.txt");
		if (configured_arm("mode_floor")) {
			FAIL(
			    "this process is configured for [mode_floor] (DXR_TEST_ARM) but its "
			    "preconditions do not hold - a mis-set environment, not a pass");
		}
		SUCCEED("not the mode-floor process; nothing to pin here");
		return;
	}

	Runtime rt;
	BringUp opt;
	opt.display_info = true;
	opt.begin = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR;
	if (!bring_up(rt, opt)) {
		return;
	}
	if (!rt.have_display_info) {
		SKIP("XR_DXR_display_info not advertised by this build");
	}

	const uint32_t device_max = rt.device_max_view_count();
	REQUIRE(device_max > 2);

	// The invariant: an app that opted into the device's full width is not
	// narrowed. If this ever fails, #1499 took capability away from exactly
	// the apps #1486 added it for.
	XrDisplayRenderingModeInfoDXR active{};
	REQUIRE(rt.active_mode(&active));
	INFO("active mode " << active.modeIndex << " '" << active.modeName << "' viewCount " << active.viewCount);
	CHECK(active.viewCount == device_max);

	// ...and no floor event was pushed.
	for (const XrEventDataBuffer &ev : rt.drain_events()) {
		CHECK(ev.type != XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_DXR);
	}

	tear_down(rt);
}

TEST_CASE("a device that PINS its mode outranks the floor (#1499)", "[oxr][view_space][mode_pinned]")
{
	if (legacy_switch_set() || mode_floor_disabled() || !sim_mode_pinned()) {
		WARN(
		    "this arm needs SIM_DISPLAY_OUTPUT=quad SIM_DISPLAY_FORCE_MODE=1 - see "
		    "tests_oxr_view_space_mode_pinned in tests/CMakeLists.txt");
		if (configured_arm("mode_pinned")) {
			FAIL(
			    "this process is configured for [mode_pinned] (DXR_TEST_ARM) but its "
			    "preconditions do not hold - a mis-set environment, not a pass");
		}
		SUCCEED("not the pinned process; nothing to pin here");
		return;
	}

	Runtime rt;
	BringUp opt;
	opt.display_info = true;
	opt.begin = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	if (!bring_up(rt, opt)) {
		return;
	}
	if (!rt.have_display_info) {
		SKIP("XR_DXR_display_info not advertised by this build");
	}

	/*
	 * The dev pin exists to hold a mode against every later request, which is
	 * what keeps the N-view under-submit path testable at all. So the floor is
	 * NOT applied, the session stays in Quad, the under-submit clamp stands -
	 * and xrBeginSession logged the UNFILLABLE warning naming #1499. (The log
	 * line itself is not asserted here; the observable fact is that the mode
	 * did not move.)
	 */
	XrDisplayRenderingModeInfoDXR active{};
	REQUIRE(rt.active_mode(&active));
	INFO("active mode " << active.modeIndex << " '" << active.modeName << "' viewCount " << active.viewCount);
	CHECK(active.modeIndex == 1u); // OUTPUT=quad FORCE_MODE=1: the Quad kind, pinned
	CHECK(active.viewCount > 2);

	/*
	 * ...and the pin exempts the DENIAL too, not just the floor. This session
	 * is SITTING in mode 1, so denying a request for mode 1 would be refusing
	 * it permission to ask for the mode it is already in - and the device, not
	 * the runtime, is the authority on a pinned mode. The first cut of the
	 * denial gate got this wrong.
	 */
	rt.drain_events();
	auto request = rt.fn<PFN_xrRequestDisplayRenderingModeDXR>("xrRequestDisplayRenderingModeDXR");
	CHECK(request(rt.session, 1u) == XR_SUCCESS);
	for (const XrEventDataBuffer &ev : rt.drain_events()) {
		CHECK(ev.type != XR_TYPE_EVENT_DATA_DISPLAY_MODE_REQUEST_DENIED_DXR);
	}
	// The device still holds the mode, which is the whole point of the pin.
	REQUIRE(rt.active_mode(&active));
	CHECK(active.modeIndex == 1u);

	tear_down(rt);
}

/*
 * #1499 F6: an ORCHESTRATOR is exempt from the painter's rule.
 *
 * This arm looks like it should assert the DENIAL, and originally did - which
 * was a bug in the test, not in the runtime. `is_bridge_relay` is set for ANY
 * session with XR_DXR_display_info + XR_MND_headless (oxr_session.c), so every
 * session this headless suite can create is a relay by construction: no
 * compositor, nothing submitted, its own view count measuring the wrong thing.
 * Denying it would be measuring the relay to protect pixels that belong to a
 * different session entirely.
 *
 * So what is pinned here is the exemption, and the end-to-end denial of a
 * genuine PAINTER is on the graphics-bound list (it needs a compositor, which
 * this suite deliberately does not have). The rule itself -
 * oxr_mode_fillable_by() - is pinned in tests_oxr_mode_fillable_rule.cpp.
 */
TEST_CASE("an orchestrator session is exempt from the fillability rule (#1499)", "[oxr][view_space][mode_floor]")
{
	if (legacy_switch_set() || mode_floor_disabled() || !sim_quad_requested() || sim_mode_pinned()) {
		WARN(
		    "this arm needs SIM_DISPLAY_OUTPUT=quad SIM_DISPLAY_START_3D=1, an UNpinned device and "
		    "DXR_MODE_FLOOR on - see tests_oxr_view_space_mode_floor in tests/CMakeLists.txt");
		if (configured_arm("mode_floor")) {
			FAIL(
			    "this process is configured for [mode_floor] (DXR_TEST_ARM) but its "
			    "preconditions do not hold - a mis-set environment, not a pass");
		}
		SUCCEED("not the mode-floor process; nothing to pin here");
		return;
	}

	Runtime rt;
	BringUp opt;
	opt.display_info = true;
	opt.begin = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	if (!bring_up(rt, opt)) {
		return;
	}
	if (!rt.have_display_info) {
		SKIP("XR_DXR_display_info not advertised by this build");
	}

	const int32_t unfillable = rt.first_unfillable_mode(2);
	REQUIRE(unfillable == 1); // Quad, the device's mode 1 (ADR-065)
	INFO("requesting mode " << unfillable);

	// The floor DID fire at xrBeginSession (the floor has no orchestrator
	// exemption - it only moves the display somewhere the mode is coherent,
	// and a relay has no stake in that). Drain it so what follows can only be
	// the answer to OUR request.
	rt.drain_events();

	auto request = rt.fn<PFN_xrRequestDisplayRenderingModeDXR>("xrRequestDisplayRenderingModeDXR");
	CHECK(request(rt.session, (uint32_t)unfillable) == XR_SUCCESS);

	bool saw_denial = false;
	for (const XrEventDataBuffer &ev : rt.drain_events()) {
		if (ev.type == XR_TYPE_EVENT_DATA_DISPLAY_MODE_REQUEST_DENIED_DXR) {
			saw_denial = true;
		}
	}
	// NOT denied: this session orchestrates, it does not paint.
	CHECK_FALSE(saw_denial);

	// ...and the request really took, which is what "exempt" has to mean.
	XrDisplayRenderingModeInfoDXR active{};
	REQUIRE(rt.active_mode(&active));
	INFO("active mode " << active.modeIndex << " '" << active.modeName << "' viewCount " << active.viewCount);
	CHECK(active.modeIndex == (uint32_t)unfillable);

	// A MULTIVIEW session is equally undenied - for the other reason (it can
	// fill the mode). Both paths reach XR_SUCCESS; only the rationale differs.
	tear_down(rt);

	Runtime wide;
	BringUp wopt;
	wopt.display_info = true;
	wopt.begin = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR;
	if (!bring_up(wide, wopt)) {
		return;
	}
	wide.drain_events();
	auto wrequest = wide.fn<PFN_xrRequestDisplayRenderingModeDXR>("xrRequestDisplayRenderingModeDXR");
	CHECK(wrequest(wide.session, (uint32_t)unfillable) == XR_SUCCESS);
	for (const XrEventDataBuffer &ev : wide.drain_events()) {
		CHECK(ev.type != XR_TYPE_EVENT_DATA_DISPLAY_MODE_REQUEST_DENIED_DXR);
	}
	XrDisplayRenderingModeInfoDXR wactive{};
	REQUIRE(wide.active_mode(&wactive));
	CHECK(wactive.modeIndex == (uint32_t)unfillable);

	tear_down(wide);
}

TEST_CASE("DXR_MODE_FLOOR=0 restores the pre-#1499 behaviour", "[oxr][view_space][mode_floor_off]")
{
	if (legacy_switch_set() || !mode_floor_disabled() || !sim_quad_requested() || sim_mode_pinned()) {
		// SUCCEED, not SKIP: build-windows.yml reads any "SKIPPED:" from this
		// binary as "the headless runtime did not come up" (#1370).
		WARN(
		    "this arm needs DXR_MODE_FLOOR=0 AND SIM_DISPLAY_OUTPUT=quad SIM_DISPLAY_START_3D=1 - see "
		    "tests_oxr_view_space_mode_floor_off in tests/CMakeLists.txt");
		if (configured_arm("mode_floor_off")) {
			FAIL(
			    "this process is configured for [mode_floor_off] (DXR_TEST_ARM) but its "
			    "preconditions do not hold - a mis-set environment, not a pass");
		}
		SUCCEED("not the kill-switch process; nothing to pin here");
		return;
	}

	Runtime rt;
	BringUp opt;
	opt.display_info = true;
	opt.begin = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	if (!bring_up(rt, opt)) {
		return;
	}
	if (!rt.have_display_info) {
		SKIP("XR_DXR_display_info not advertised by this build");
	}

	const uint32_t device_max = rt.device_max_view_count();
	REQUIRE(device_max > 2); // SIM_DISPLAY_OUTPUT=quad must have taken.

	SECTION("the floor does not fire: a 2-view session is left sitting in Quad")
	{
		XrDisplayRenderingModeInfoDXR active{};
		REQUIRE(rt.active_mode(&active));
		INFO("active mode " << active.modeIndex << " '" << active.modeName << "' viewCount "
		                    << active.viewCount);
		// The pre-#1499 state, deliberately restored: the app will paint the
		// first two tiles and the rest stay at the clear colour.
		CHECK(active.viewCount == device_max);

		// ...and nothing was announced, because nothing moved.
		for (const XrEventDataBuffer &ev : rt.drain_events()) {
			CHECK(ev.type != XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_DXR);
		}
	}

	SECTION("the denial does not fire either: an unfillable request is honoured")
	{
		// NOTE: headless, so this session is also orchestrator-exempt (see the
		// exemption arm above) and would not be denied with the switch ON
		// either. What this pins is the SWITCH-OFF half of the pair - the
		// request path reaching XR_SUCCESS and the mode actually moving - not
		// the denial's absence on its own.
		const int32_t unfillable = rt.first_unfillable_mode(2);
		REQUIRE(unfillable >= 0);
		rt.drain_events();

		auto request = rt.fn<PFN_xrRequestDisplayRenderingModeDXR>("xrRequestDisplayRenderingModeDXR");
		CHECK(request(rt.session, (uint32_t)unfillable) == XR_SUCCESS);

		for (const XrEventDataBuffer &ev : rt.drain_events()) {
			CHECK(ev.type != XR_TYPE_EVENT_DATA_DISPLAY_MODE_REQUEST_DENIED_DXR);
		}

		// Both halves are off together, so the request really took.
		XrDisplayRenderingModeInfoDXR active{};
		REQUIRE(rt.active_mode(&active));
		CHECK(active.modeIndex == (uint32_t)unfillable);
	}

	tear_down(rt);
}

TEST_CASE("the system reports tracking when a tracked role device exists (#1631)", "[oxr][view_space][tracking_props]")
{
	// #1631 WIRING arm. The RULE (head flags OR the display processor's
	// eye-tracking capability OR any tracked role device) is pinned on the
	// host in tests/tests_oxr_tracking_properties_rule.cpp; what is pinned
	// HERE is that oxr_system_get_properties() actually feeds it the role
	// devices, which is the half that used to be missing — it read the head
	// xdev and nothing else, and in a display session the head is the display
	// driver's, so both flags came back false.
	//
	// Why this session is a TRUE case with no eye tracker and no hardware:
	// the sim-display builder always adds the qwerty keyboard/mouse input
	// devices (`t_builder_add_qwerty_input` in target_builder_sim_display.c),
	// and the qwerty controllers set both supported.orientation_tracking and
	// supported.position_tracking. Hence the DXR_TEST_HAVE_QWERTY gate — in a
	// build without that driver there is no role device, nothing else in a
	// bare sim-display system tracks, and FALSE is the correct answer.
	Runtime rt;
	if (!bring_up(rt)) {
		return;
	}

	auto getProps = rt.fn<PFN_xrGetSystemProperties>("xrGetSystemProperties");
	XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
	REQUIRE(XR_SUCCEEDED(getProps(rt.instance, rt.system, &props)));

	INFO("orientationTracking=" << props.trackingProperties.orientationTracking
	                            << " positionTracking=" << props.trackingProperties.positionTracking);

#if DXR_TEST_HAVE_QWERTY
	// The three CTS files that used to skip outright — test_SpaceOffsets.cpp,
	// test_InteractiveThrow.cpp, test_ActionPoses.cpp — gate on exactly this.
	CHECK(props.trackingProperties.orientationTracking == XR_TRUE);
	CHECK(props.trackingProperties.positionTracking == XR_TRUE);
#else
	// SUCCEED, not SKIP: build-windows.yml reads any "SKIPPED:" from this
	// binary as "the headless runtime did not come up" (#1370).
	WARN("built without the qwerty driver - no tracked role device, so FALSE is correct here");
	SUCCEED("no role devices in this build; the rule itself is pinned on the host");
#endif

	tear_down(rt);
}

TEST_CASE("the CTS configuration (no input providers) still reports tracking (#1631)",
          "[oxr][view_space][tracking_props_noproviders]")
{
	// #1631 REGRESSION arm, and the one that caught the real bug. The first
	// cut of the fix read the dynamic roles through GET_XDEV_BY_ROLE, i.e.
	// out of oxr_system::dynamic_roles_cache, which is only refreshed at
	// xrSyncActions — so the answer silently depended on whether an
	// input-provider plug-in was loaded (a provider populates the STATIC
	// hand-tracking roles, which ARE read live). With DXR_INPUT_PROVIDERS=0,
	// which scripts/run_cts.ps1:431 ALWAYS sets so the hands stay on qwerty,
	// nothing static held a role and the flags collapsed back to XR_FALSE —
	// exactly the configuration the CTS runs in, so the three files kept
	// skipping while the plain arm above passed.
	//
	// The variable is read through CRT getenv at the process's FIRST instance
	// create and the scan is one-shot (`g_input_load_attempted`), so this arm
	// needs its OWN process — hence tests_oxr_view_space_no_input_providers
	// in tests/CMakeLists.txt, the same pattern as the legacy / mode_floor
	// arms above.
	const char *no_providers = std::getenv("DXR_INPUT_PROVIDERS");
	if (no_providers == nullptr || std::strcmp(no_providers, "0") != 0) {
		// SUCCEED, not SKIP: build-windows.yml reads any "SKIPPED:" from this
		// binary as "the headless runtime did not come up" (#1370).
		WARN("this arm needs DXR_INPUT_PROVIDERS=0 - see tests_oxr_view_space_no_input_providers "
		     "in tests/CMakeLists.txt");
		SUCCEED("not the no-provider process; the plain [tracking_props] arm covers the other configuration");
		return;
	}

	Runtime rt;
	if (!bring_up(rt)) {
		return;
	}

	auto getProps = rt.fn<PFN_xrGetSystemProperties>("xrGetSystemProperties");
	XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
	REQUIRE(XR_SUCCEEDED(getProps(rt.instance, rt.system, &props)));

	INFO("orientationTracking=" << props.trackingProperties.orientationTracking
	                            << " positionTracking=" << props.trackingProperties.positionTracking);

#if DXR_TEST_HAVE_QWERTY
	// Deliberately asked BEFORE any xrSyncActions, like the CTS does at
	// test_SpaceOffsets.cpp:125 - that timing is the whole point.
	CHECK(props.trackingProperties.orientationTracking == XR_TRUE);
	CHECK(props.trackingProperties.positionTracking == XR_TRUE);
#else
	WARN("built without the qwerty driver - no tracked role device, so FALSE is correct here");
	SUCCEED("no role devices in this build; the rule itself is pinned on the host");
#endif

	tear_down(rt);
}

TEST_CASE("rig-scoped hand joints: a locate BEFORE the first view locate equals one after it (#2189)",
          "[oxr][view_space][rig_hands]")
{
	// XR_DXR_view_rig spec 4: joints chained with a rig come back in the rig
	// frame. Before the session's first xrLocateViews there is no recorded
	// canvas, so the in-process runtime resolves the session's own canvas
	// through the same helper the views use — the result must be what the
	// joints read once a view locate has recorded it. Needs a hand-tracking
	// provider: the sim-input plug-in, in its own process
	// (tests_oxr_view_space_rig_hands: a generated manifest +
	// DXR_INPUT_SIM_INPUT_ENABLE=1; the provider scan is one-shot).
	const char *enable = std::getenv("DXR_INPUT_SIM_INPUT_ENABLE");
	if (enable == nullptr || std::strcmp(enable, "1") != 0) {
		// SUCCEED, not SKIP: build-windows.yml reads any "SKIPPED:" from this
		// binary as "the headless runtime did not come up" (#1370).
		WARN(
		    "this arm needs the sim-input provider - see tests_oxr_view_space_rig_hands in "
		    "tests/CMakeLists.txt");
		SUCCEED("not the rig-hands process");
		return;
	}

	Runtime rt;
	BringUp opt;
	opt.hand_tracking = true;
	if (!bring_up(rt, opt)) {
		return;
	}
	if (!rt.have_view_rig || !rt.have_hand_tracking) {
		WARN("XR_DXR_view_rig / XR_EXT_hand_tracking not advertised (no hand-tracking provider resident)");
		SUCCEED("nothing to locate");
		tear_down(rt);
		return;
	}

	auto createTracker = rt.fn<PFN_xrCreateHandTrackerEXT>("xrCreateHandTrackerEXT");
	auto locateJoints = rt.fn<PFN_xrLocateHandJointsEXT>("xrLocateHandJointsEXT");
	XrHandTrackerCreateInfoEXT hci = {XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
	hci.hand = XR_HAND_LEFT_EXT;
	hci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
	XrHandTrackerEXT tracker = XR_NULL_HANDLE;
	REQUIRE(XR_SUCCEEDED(createTracker(rt.session, &hci, &tracker)));

	// A scaled, posed display rig: a missed rig shows up as base-space metres.
	XrDisplayRigDXR rig = {XR_TYPE_DISPLAY_RIG_DXR};
	rig.pose = {{0, 0, 0, 1}, {0.5f, 1.0f, -2.0f}};
	rig.virtualDisplayHeight = 3.0f;
	rig.ipdFactor = 1.0f;
	rig.parallaxFactor = 1.0f;
	rig.perspectiveFactor = 1.0f;

	const XrTime t = rt.now();
	auto joints = [&](const void *chain, XrHandJointLocationEXT *out, XrBool32 *active) {
		XrHandJointsLocateInfoEXT li = {XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
		li.next = chain;
		li.baseSpace = rt.local;
		li.time = t;
		XrHandJointLocationsEXT locs = {XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
		locs.jointCount = XR_HAND_JOINT_COUNT_EXT;
		locs.jointLocations = out;
		REQUIRE(XR_SUCCEEDED(locateJoints(tracker, &li, &locs)));
		*active = locs.isActive;
	};

	XrHandJointLocationEXT base[XR_HAND_JOINT_COUNT_EXT] = {};
	XrHandJointLocationEXT before[XR_HAND_JOINT_COUNT_EXT] = {};
	XrHandJointLocationEXT after[XR_HAND_JOINT_COUNT_EXT] = {};
	XrBool32 a0 = XR_FALSE, a1 = XR_FALSE, a2 = XR_FALSE;
	joints(nullptr, base, &a0);
	joints(&rig, before, &a1); // no view locate yet on this session
	if (!a0 || !a1) {
		WARN("the hand is not active (provider present but not tracking) - nothing to compare");
		SUCCEED("inactive hand");
		tear_down(rt);
		return;
	}
	(void)rt.views(rt.local, t, &rig, nullptr); // records the canvas
	joints(&rig, after, &a2);
	REQUIRE(a2);

	const XrVector3f &pb = base[XR_HAND_JOINT_PALM_EXT].pose.position;
	const XrVector3f &p1 = before[XR_HAND_JOINT_PALM_EXT].pose.position;
	INFO("base (" << pb.x << "," << pb.y << "," << pb.z << ") before-views (" << p1.x << "," << p1.y << "," << p1.z
	              << ")");
	// The rig applied before any view locate (the review's case): not base-space metres.
	CHECK(std::fabs(p1.z - pb.z) > 0.1f);
	for (uint32_t i = 0; i < XR_HAND_JOINT_COUNT_EXT; i++) {
		CAPTURE(i);
		CHECK(before[i].pose.position.x == Catch::Approx(after[i].pose.position.x).margin(1e-4));
		CHECK(before[i].pose.position.y == Catch::Approx(after[i].pose.position.y).margin(1e-4));
		CHECK(before[i].pose.position.z == Catch::Approx(after[i].pose.position.z).margin(1e-4));
		CHECK(before[i].radius == Catch::Approx(after[i].radius).margin(1e-6));
	}

	tear_down(rt);
}

namespace {

//! Rotate @p v by the inverse of unit quaternion @p q.
XrVector3f
quat_inv_rotate(const XrQuaternionf &q, const XrVector3f &v)
{
	// v' = v + 2w(u x v) + 2 u x (u x v), with u = the conjugate's vector part.
	const float ux = -q.x, uy = -q.y, uz = -q.z, w = q.w;
	const float cx = uy * v.z - uz * v.y, cy = uz * v.x - ux * v.z, cz = ux * v.y - uy * v.x;
	const float ccx = uy * cz - uz * cy, ccy = uz * cx - ux * cz, ccz = ux * cy - uy * cx;
	return {v.x + 2.0f * (w * cx + ccx), v.y + 2.0f * (w * cy + ccy), v.z + 2.0f * (w * cz + ccz)};
}

} // namespace

TEST_CASE("sim-input hands sit in front of the physical display plane (#2191)", "[oxr][view_space][rig_hands]")
{
	// #2191: the scripted hands must land where a default display rig shows
	// them — about HAND_DISTANCE_M (0.25) in front of the glass, ±12 cm either
	// side — measured END TO END: wrists located in LOCAL through
	// xrLocateHandJointsEXT, re-expressed in the frame of the physical display
	// plane xrLocateViews reports in LOCAL (XrViewDisplayRawDXR::
	// displayPlanePose, nothing chained). Same process as the arm above.
	const char *enable = std::getenv("DXR_INPUT_SIM_INPUT_ENABLE");
	if (enable == nullptr || std::strcmp(enable, "1") != 0) {
		WARN(
		    "this arm needs the sim-input provider - see tests_oxr_view_space_rig_hands in "
		    "tests/CMakeLists.txt");
		SUCCEED("not the rig-hands process");
		return;
	}

	Runtime rt;
	BringUp opt;
	opt.hand_tracking = true;
	if (!bring_up(rt, opt)) {
		return;
	}
	if (!rt.have_view_rig || !rt.have_hand_tracking) {
		WARN("XR_DXR_view_rig / XR_EXT_hand_tracking not advertised (no hand-tracking provider resident)");
		SUCCEED("nothing to locate");
		tear_down(rt);
		return;
	}

	const XrTime t = rt.now();
	XrViewDisplayRawDXR raw = {XR_TYPE_VIEW_DISPLAY_RAW_DXR};
	(void)rt.views(rt.local, t, nullptr, &raw);
	const XrPosef plane = raw.displayPlanePose;
	std::printf("#2191 display plane in LOCAL: (%.3f, %.3f, %.3f) q(%.3f, %.3f, %.3f, %.3f)\n", plane.position.x,
	            plane.position.y, plane.position.z, plane.orientation.x, plane.orientation.y, plane.orientation.z,
	            plane.orientation.w);

	auto createTracker = rt.fn<PFN_xrCreateHandTrackerEXT>("xrCreateHandTrackerEXT");
	auto destroyTracker = rt.fn<PFN_xrDestroyHandTrackerEXT>("xrDestroyHandTrackerEXT");
	auto locateJoints = rt.fn<PFN_xrLocateHandJointsEXT>("xrLocateHandJointsEXT");

	const XrHandEXT hands[2] = {XR_HAND_LEFT_EXT, XR_HAND_RIGHT_EXT};
	for (XrHandEXT hand : hands) {
		XrHandTrackerCreateInfoEXT hci = {XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
		hci.hand = hand;
		hci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
		XrHandTrackerEXT tracker = XR_NULL_HANDLE;
		REQUIRE(XR_SUCCEEDED(createTracker(rt.session, &hci, &tracker)));

		XrHandJointLocationEXT locs[XR_HAND_JOINT_COUNT_EXT] = {};
		XrHandJointsLocateInfoEXT li = {XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
		li.baseSpace = rt.local;
		li.time = t;
		XrHandJointLocationsEXT out = {XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
		out.jointCount = XR_HAND_JOINT_COUNT_EXT;
		out.jointLocations = locs;
		REQUIRE(XR_SUCCEEDED(locateJoints(tracker, &li, &out)));
		REQUIRE(out.isActive);

		const XrVector3f w = locs[XR_HAND_JOINT_WRIST_EXT].pose.position;
		const XrVector3f d = quat_inv_rotate(
		    plane.orientation, {w.x - plane.position.x, w.y - plane.position.y, w.z - plane.position.z});
		const bool left = hand == XR_HAND_LEFT_EXT;
		std::printf("#2191 %s wrist LOCAL (%.3f, %.3f, %.3f) -> display-plane (%.3f, %.3f, %.3f)\n",
		            left ? "left " : "right", w.x, w.y, w.z, d.x, d.y, d.z);
		CAPTURE(left, w.x, w.y, w.z, d.x, d.y, d.z);
		// In front of the glass, about HAND_DISTANCE_M out (the circle and the
		// wrist-to-root offset of the simulated hand both fit in ±10 cm).
		CHECK(d.z > 0.15f);
		CHECK(d.z < 0.35f);
		// Left hand left of centre, right hand right of it, within the panel.
		CHECK((left ? -d.x : d.x) > 0.02f);
		CHECK(std::fabs(d.x) < 0.25f);
		// Near panel-centre height, not a stage height away.
		CHECK(std::fabs(d.y) < 0.15f);

		destroyTracker(tracker);
	}

	tear_down(rt);
}

namespace {

/*!
 * One instant on the host's monotonic clock, re-expressible as an XrTime in
 * ANY instance (each instance has its own XrTime offset). The sim-input script
 * is a pure function of the monotonic timestamp, so two instances asked about
 * the same Instant see the same scripted hands — which is what lets the
 * session classes below be compared to the millimetre.
 */
struct Instant
{
#if defined(_WIN32)
	LARGE_INTEGER qpc;
#else
	struct timespec ts;
#endif
};

/*!
 * An Instant 10 s in the FUTURE. Each instance's XrTime is offset from the
 * moment it was created, so an instant taken before a later instance exists
 * would map to a negative (invalid) XrTime there. The hand script is a pure
 * function of the timestamp, so a predicted time is as good as a present one.
 */
Instant
instant_ahead()
{
	Instant i{};
#if defined(_WIN32)
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&i.qpc);
	i.qpc.QuadPart += f.QuadPart * 10;
#else
	clock_gettime(CLOCK_MONOTONIC, &i.ts);
	i.ts.tv_sec += 10;
#endif
	return i;
}

XrTime
instant_to_xr(Runtime &rt, const Instant &i)
{
	XrTime t = 0;
#if defined(_WIN32)
	auto convert =
	    rt.fn<PFN_xrConvertWin32PerformanceCounterToTimeKHR>("xrConvertWin32PerformanceCounterToTimeKHR");
	REQUIRE(XR_SUCCEEDED(convert(rt.instance, &i.qpc, &t)));
#else
	auto convert = rt.fn<PFN_xrConvertTimespecTimeToTimeKHR>("xrConvertTimespecTimeToTimeKHR");
	REQUIRE(XR_SUCCEEDED(convert(rt.instance, &i.ts, &t)));
#endif
	return t;
}

//! Wrist + palm of both hands, in the display-plane frame.
struct PlaneHands
{
	XrVector3f wrist[2];
	XrVector3f palm[2];
	//! The palm joint's radius: the RIG_LOCAL canvas scale (ADR-034
	//! Amendment 7) scales it with the canvas; 1:1 for a session with no
	//! canvas of its own.
	float palm_radius[2];
};

/*!
 * Locate both hands' wrist + palm in @p base and re-express them in the frame
 * of the display plane xrLocateViews reports in that same base
 * (XrViewDisplayRawDXR::displayPlanePose, nothing chained). Returns false when
 * a hand is not active.
 */
bool
measure_plane_hands(Runtime &rt, XrSpace base, XrTime t, PlaneHands *out, XrPosef *out_plane)
{
	XrViewDisplayRawDXR raw = {XR_TYPE_VIEW_DISPLAY_RAW_DXR};
	(void)rt.views(base, t, nullptr, &raw);
	const XrPosef plane = raw.displayPlanePose;
	const XrPosef inv_plane = pinv(plane);
	if (out_plane != nullptr) {
		*out_plane = plane;
	}

	auto createTracker = rt.fn<PFN_xrCreateHandTrackerEXT>("xrCreateHandTrackerEXT");
	auto destroyTracker = rt.fn<PFN_xrDestroyHandTrackerEXT>("xrDestroyHandTrackerEXT");
	auto locateJoints = rt.fn<PFN_xrLocateHandJointsEXT>("xrLocateHandJointsEXT");

	bool all_active = true;
	for (int h = 0; h < 2; h++) {
		XrHandTrackerCreateInfoEXT hci = {XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
		hci.hand = h == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
		hci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
		XrHandTrackerEXT tracker = XR_NULL_HANDLE;
		REQUIRE(XR_SUCCEEDED(createTracker(rt.session, &hci, &tracker)));

		XrHandJointLocationEXT locs[XR_HAND_JOINT_COUNT_EXT] = {};
		XrHandJointsLocateInfoEXT li = {XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
		li.baseSpace = base;
		li.time = t;
		XrHandJointLocationsEXT jl = {XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
		jl.jointCount = XR_HAND_JOINT_COUNT_EXT;
		jl.jointLocations = locs;
		REQUIRE(XR_SUCCEEDED(locateJoints(tracker, &li, &jl)));
		all_active = all_active && jl.isActive;

		out->wrist[h] = pmul(inv_plane, locs[XR_HAND_JOINT_WRIST_EXT].pose).position;
		out->palm[h] = pmul(inv_plane, locs[XR_HAND_JOINT_PALM_EXT].pose).position;
		out->palm_radius[h] = locs[XR_HAND_JOINT_PALM_EXT].radius;
		destroyTracker(tracker);
	}
	return all_active;
}

//! One session class's measurement of the same Instant, in LOCAL and STAGE.
struct ClassResult
{
	bool ran = false;
	PlaneHands local{};
	PlaneHands stage{};
};

ClassResult
measure_session_class(const char *name, bool display_info, const Instant &at)
{
	ClassResult r;
	Runtime rt;
	BringUp opt;
	opt.hand_tracking = true;
	opt.display_info = display_info;
	if (!bring_up(rt, opt)) {
		return r;
	}
	if (!rt.have_view_rig || !rt.have_hand_tracking || (display_info && !rt.have_display_info)) {
		WARN(name << ": XR_DXR_view_rig / XR_EXT_hand_tracking / XR_DXR_display_info not advertised");
		tear_down(rt);
		return r;
	}
	const XrTime t = instant_to_xr(rt, at);
	XrPosef plane_local{}, plane_stage{};
	const bool a0 = measure_plane_hands(rt, rt.local, t, &r.local, &plane_local);
	const bool a1 = measure_plane_hands(rt, rt.stage, t, &r.stage, &plane_stage);
	REQUIRE(a0);
	REQUIRE(a1);
	for (int h = 0; h < 2; h++) {
		std::printf(
		    "RIG_LOCAL %-12s %s wrist plane-frame (%.3f, %.3f, %.3f) palm (%.3f, %.3f, %.3f) | plane in "
		    "LOCAL %s\n",
		    name, h == 0 ? "L" : "R", r.local.wrist[h].x, r.local.wrist[h].y, r.local.wrist[h].z,
		    r.local.palm[h].x, r.local.palm[h].y, r.local.palm[h].z, pstr(plane_local).c_str());
	}
	r.ran = true;
	tear_down(rt);
	return r;
}

void
check_in_front_of_plane(const char *name, const PlaneHands &p)
{
	for (int h = 0; h < 2; h++) {
		const bool left = h == 0;
		const XrVector3f d = p.wrist[h];
		CAPTURE(name, left, d.x, d.y, d.z);
		// In front of the glass, about HAND_DISTANCE_M (0.25) out.
		CHECK(d.z > 0.15f);
		CHECK(d.z < 0.35f);
		// Left hand left of centre, right hand right of it, on the panel.
		CHECK((left ? -d.x : d.x) > 0.01f);
		CHECK(std::fabs(d.x) < 0.25f);
		// Near panel-centre height — not a standing height, not a viewer offset.
		CHECK(std::fabs(d.y) < 0.15f);
	}
}

void
check_same_hands(const char *what, const PlaneHands &a, const PlaneHands &b, float tol_m)
{
	for (int h = 0; h < 2; h++) {
		CAPTURE(what, h);
		CHECK(vdist(a.wrist[h], b.wrist[h]) < tol_m);
		CHECK(vdist(a.palm[h], b.palm[h]) < tol_m);
		CHECK(a.palm_radius[h] == Catch::Approx(b.palm_radius[h]).margin(1e-6));
	}
}

} // namespace

TEST_CASE("RIG_LOCAL hands sit on the display plane the SESSION sees, in every session class",
          "[oxr][view_space][rig_hands]")
{
	// RIG_LOCAL contract (input-provider-discovery.md §4b.4): a joint the
	// provider publishes at display-plane point L is located at
	// displayPlanePose o L — in whatever base the app locates in, and for every
	// session class. Field evidence (2026-10-18): an external-window session
	// (modelviewer, `_handle`) on a no-qwerty Linux build had the palms 1.47 m
	// below and 0.54 m in front of where they belong, because the builder
	// anchored RIG_LOCAL at the head DEVICE pose — the display plane only for
	// runtime-window sessions.
	//
	// Headless classes reachable in-process:
	//   - runtime-window (no XR_DXR_display_info): plane = head device pose;
	//   - bridge relay (XR_DXR_display_info + headless): plane = identity at the
	//     head's tracking origin — the SAME plane rule an external-window
	//     (`_handle` / `_texture`) session gets (oxr_session.c, the
	//     `!has_external_window && !is_bridge_relay` gate).
	// Both measure the same monotonic Instant, so their plane-frame hands must
	// agree to the millimetre, not just be "roughly in front".
	const char *enable = std::getenv("DXR_INPUT_SIM_INPUT_ENABLE");
	if (enable == nullptr || std::strcmp(enable, "1") != 0) {
		WARN(
		    "this arm needs the sim-input provider - see tests_oxr_view_space_rig_hands in "
		    "tests/CMakeLists.txt");
		SUCCEED("not the rig-hands process");
		return;
	}

	const Instant at = instant_ahead();
	const ClassResult hosted = measure_session_class("runtime-win", false, at);
	const ClassResult relay = measure_session_class("bridge-relay", true, at);
	if (!hosted.ran || !relay.ran) {
		SUCCEED("a session class could not come up headlessly - nothing to compare");
		return;
	}

	// Each class: the hands are in front of ITS display plane.
	check_in_front_of_plane("runtime-win", hosted.local);
	check_in_front_of_plane("bridge-relay", relay.local);
	// Each class: the plane-frame hands do not depend on the base space.
	check_same_hands("runtime-win LOCAL vs STAGE", hosted.local, hosted.stage, kPosTolM);
	check_same_hands("bridge-relay LOCAL vs STAGE", relay.local, relay.stage, kPosTolM);
	// The classes agree: RIG_LOCAL is the display plane, not the head device pose.
	check_same_hands("runtime-win vs bridge-relay", hosted.local, relay.local, 0.002f);
	// Neither class owns a canvas, so the canvas-relative rule (ADR-034
	// Amendment 7) leaves both at the physical display's scale: the palm
	// radius is the provider's, unscaled, in both (compared above) and
	// non-zero. The canvas-owning (external-window) class cannot come up
	// headlessly; its placement — L at the canvas centre + s L, radii and
	// velocities times s — is pinned in tests_view_rig_point
	// ("RIG_LOCAL is canvas-relative: placement").
	for (int h = 0; h < 2; h++) {
		CAPTURE(h);
		CHECK(hosted.local.palm_radius[h] > 0.0f);
	}
}

TEST_CASE("RIG_LOCAL hands follow the display plane a chained XR_DXR_view_rig puts in LOCAL",
          "[oxr][view_space][rig_hands]")
{
	// Field report (cube_handle_metal_macos --hands --actions, 2026-10-25): an
	// external-window app that chains a display rig at the LOCAL origin — the
	// shape every engine plug-in and cube app has — located its sim-input
	// wrists and grips at (+-0.03, -1.56, 0.25) in LOCAL: on the PHYSICAL plane
	// (the head's tracking origin, LOCAL's 1.6 m stage height below), not on
	// the plane its views and displayPlanePose are in. Once a view locate
	// chains a rig, the display plane the session sees is where the rig puts
	// it, and RIG_LOCAL must land there (input-provider-discovery.md §4b.4).
	//
	// The external-window class cannot come up headlessly; the runtime-window
	// class carries the same rule, and a rig posed AWAY from the physical
	// plane makes a miss unmistakable (before the fix the hands stayed at the
	// physical plane, ~0.6 m from where the rig shows them). A canvas-owning
	// session puts the plane at its CANVAS centre through the rig and scales
	// L by the canvas (ADR-034 Amendment 7) — the same plane for a
	// full-display canvas; the windowed case is pinned in tests_view_rig_point.
	const char *enable = std::getenv("DXR_INPUT_SIM_INPUT_ENABLE");
	if (enable == nullptr || std::strcmp(enable, "1") != 0) {
		WARN(
		    "this arm needs the sim-input provider - see tests_oxr_view_space_rig_hands in "
		    "tests/CMakeLists.txt");
		SUCCEED("not the rig-hands process");
		return;
	}

	Runtime rt;
	BringUp opt;
	opt.hand_tracking = true;
	if (!bring_up(rt, opt)) {
		return;
	}
	if (!rt.have_view_rig || !rt.have_hand_tracking) {
		WARN("XR_DXR_view_rig / XR_EXT_hand_tracking not advertised (no hand-tracking provider resident)");
		SUCCEED("nothing to locate");
		tear_down(rt);
		return;
	}

	const XrTime t = rt.now();

	// Unscaled (virtualDisplayHeight = the canvas height), posed and yawed.
	XrViewDisplayRawDXR raw0 = {XR_TYPE_VIEW_DISPLAY_RAW_DXR};
	(void)rt.views(rt.local, t, nullptr, &raw0);
	const float canvas_h = raw0.canvasSizeMeters.height;
	REQUIRE(canvas_h > 0.0f);
	XrDisplayRigDXR rig = {XR_TYPE_DISPLAY_RIG_DXR};
	rig.pose = {qyaw(0.4f), {0.4f, 0.3f, -0.35f}};
	rig.virtualDisplayHeight = canvas_h;
	rig.ipdFactor = 1.0f;
	rig.parallaxFactor = 1.0f;
	rig.perspectiveFactor = 1.0f;

	// Hands with no rig: the physical plane's frame (the #2191 contract).
	PlaneHands phys{};
	XrPosef phys_plane{};
	REQUIRE(measure_plane_hands(rt, rt.local, t, &phys, &phys_plane));

	// A view locate chaining the rig: displayPlanePose is the rig pose.
	XrViewDisplayRawDXR raw = {XR_TYPE_VIEW_DISPLAY_RAW_DXR};
	(void)rt.views(rt.local, t, &rig, &raw);
	const XrPosef plane = raw.displayPlanePose;
	INFO("rig plane in LOCAL: " << pstr(plane) << "  physical plane in LOCAL: " << pstr(phys_plane));
	CHECK(vdist(plane.position, rig.pose.position) < kPosTolM);

	auto createTracker = rt.fn<PFN_xrCreateHandTrackerEXT>("xrCreateHandTrackerEXT");
	auto destroyTracker = rt.fn<PFN_xrDestroyHandTrackerEXT>("xrDestroyHandTrackerEXT");
	auto locateJoints = rt.fn<PFN_xrLocateHandJointsEXT>("xrLocateHandJointsEXT");
	for (int h = 0; h < 2; h++) {
		XrHandTrackerCreateInfoEXT hci = {XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
		hci.hand = h == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
		hci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
		XrHandTrackerEXT tracker = XR_NULL_HANDLE;
		REQUIRE(XR_SUCCEEDED(createTracker(rt.session, &hci, &tracker)));

		auto locate = [&](const void *chain, XrHandJointLocationEXT *out) {
			XrHandJointsLocateInfoEXT li = {XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
			li.next = chain;
			li.baseSpace = rt.local;
			li.time = t;
			XrHandJointLocationsEXT jl = {XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
			jl.jointCount = XR_HAND_JOINT_COUNT_EXT;
			jl.jointLocations = out;
			REQUIRE(XR_SUCCEEDED(locateJoints(tracker, &li, &jl)));
			REQUIRE(jl.isActive);
		};
		XrHandJointLocationEXT base[XR_HAND_JOINT_COUNT_EXT] = {};
		XrHandJointLocationEXT scoped[XR_HAND_JOINT_COUNT_EXT] = {};
		locate(nullptr, base);
		locate(&rig, scoped);

		const XrPosef w = base[XR_HAND_JOINT_WRIST_EXT].pose;
		const XrVector3f in_rig = pmul(pinv(plane), w).position;
		CAPTURE(h, w.position.x, w.position.y, w.position.z, in_rig.x, in_rig.y, in_rig.z);
		// The same display-plane point as with no rig, now in the rig's frame.
		CHECK(vdist(in_rig, phys.wrist[h]) < kPosTolM);
		// An unscaled rig: base-space joints ARE the spec-4 rig-frame joints,
		// which must not move either (they undo the plane RIG_LOCAL landed on).
		for (uint32_t j = 0; j < XR_HAND_JOINT_COUNT_EXT; j++) {
			CAPTURE(j);
			CHECK(vdist(base[j].pose.position, scoped[j].pose.position) < kPosTolM);
			CHECK(qangle_deg(base[j].pose.orientation, scoped[j].pose.orientation) < kAngTolDeg);
		}
		destroyTracker(tracker);
	}

	// A view locate that chains nothing again: back on the physical plane.
	PlaneHands again{};
	XrPosef again_plane{};
	REQUIRE(measure_plane_hands(rt, rt.local, t, &again, &again_plane));
	CHECK(vdist(again_plane.position, phys_plane.position) < kPosTolM);
	check_same_hands("no rig, after a rigged locate", again, phys, kPosTolM);

	tear_down(rt);
}

TEST_CASE("sim-input HANDS=right PINCH=hold: one hand, one sustained pinch (#2191)",
          "[oxr][view_space][rig_hands_hold]")
{
	// The one-hand drag a gesture consumer could never exercise under the curl
	// wave: tests_oxr_view_space_rig_hands_hold runs this process with
	// DXR_INPUT_SIM_INPUT_HANDS=right + DXR_INPUT_SIM_INPUT_PINCH=hold.
	const char *hands = std::getenv("DXR_INPUT_SIM_INPUT_HANDS");
	const char *pinch = std::getenv("DXR_INPUT_SIM_INPUT_PINCH");
	if (hands == nullptr || std::strcmp(hands, "right") != 0 || pinch == nullptr ||
	    std::strcmp(pinch, "hold") != 0) {
		WARN("this arm needs HANDS=right PINCH=hold - see tests_oxr_view_space_rig_hands_hold");
		SUCCEED("not the rig-hands-hold process");
		return;
	}

	Runtime rt;
	BringUp opt;
	opt.hand_tracking = true;
	if (!bring_up(rt, opt)) {
		return;
	}
	if (!rt.have_hand_tracking) {
		WARN("XR_EXT_hand_tracking not advertised (no hand-tracking provider resident)");
		SUCCEED("nothing to locate");
		tear_down(rt);
		return;
	}

	auto createTracker = rt.fn<PFN_xrCreateHandTrackerEXT>("xrCreateHandTrackerEXT");
	auto destroyTracker = rt.fn<PFN_xrDestroyHandTrackerEXT>("xrDestroyHandTrackerEXT");
	auto locateJoints = rt.fn<PFN_xrLocateHandJointsEXT>("xrLocateHandJointsEXT");

	auto locate = [&](XrHandTrackerEXT tracker, XrTime t, XrHandJointLocationEXT *out) {
		XrHandJointsLocateInfoEXT li = {XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
		li.baseSpace = rt.local;
		li.time = t;
		XrHandJointLocationsEXT locs = {XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
		locs.jointCount = XR_HAND_JOINT_COUNT_EXT;
		locs.jointLocations = out;
		REQUIRE(XR_SUCCEEDED(locateJoints(tracker, &li, &locs)));
		return locs.isActive;
	};

	XrHandTrackerEXT trackers[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
	for (int i = 0; i < 2; i++) {
		XrHandTrackerCreateInfoEXT hci = {XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
		hci.hand = i == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
		hci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
		REQUIRE(XR_SUCCEEDED(createTracker(rt.session, &hci, &trackers[i])));
	}

	const XrTime t0 = rt.now();
	float max_gap = 0.0f, min_x = 1.0f, max_x = -1.0f;
	for (int k = 0; k < 40; k++) {
		const XrTime t = t0 + (XrTime)k * 100000000; // 100 ms steps across one 4 s revolution
		XrHandJointLocationEXT j[XR_HAND_JOINT_COUNT_EXT] = {};
		CHECK_FALSE(locate(trackers[0], t, j)); // HANDS=right: the left hand never tracks
		REQUIRE(locate(trackers[1], t, j));
		const XrVector3f a = j[XR_HAND_JOINT_THUMB_TIP_EXT].pose.position;
		const XrVector3f b = j[XR_HAND_JOINT_INDEX_TIP_EXT].pose.position;
		const float gap =
		    std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
		max_gap = std::fmax(max_gap, gap);
		min_x = std::fmin(min_x, j[XR_HAND_JOINT_WRIST_EXT].pose.position.x);
		max_x = std::fmax(max_x, j[XR_HAND_JOINT_WRIST_EXT].pose.position.x);
	}
	std::printf("#2191 HANDS=right PINCH=hold: max thumb-index gap %.4f m, wrist x sweep %.3f..%.3f m\n", max_gap,
	            min_x, max_x);
	CHECK(max_gap < 0.015f);      // pinched the whole revolution
	// ...while the root moves: a drag. The circle's PROJECTED diameter is 0.20 x
	// the display height (capped at 8 cm); the real one shrinks by the
	// perspective factor (0.35/0.60 at 25 cm in front of a 0.6 m viewer,
	// sim_input_hand_radius), so > 1 cm on any panel taller than 0.10 m.
	CHECK(max_x - min_x > 0.01f);
	CHECK(min_x > 0.0f);          // on the right of the display centre

	destroyTracker(trackers[0]);
	destroyTracker(trackers[1]);
	tear_down(rt);
}

namespace {

//! The viewer-tracking arms run only in their own process (the tracking scan is one-shot).
bool
viewer_tracking_process()
{
	const char *enable = std::getenv("DXR_TRACKING_SIM_TRACKING_ENABLE");
	return enable != nullptr && std::strcmp(enable, "1") == 0;
}

void
sleep_ms(int ms)
{
#if defined(_WIN32)
	Sleep((DWORD)ms);
#else
	struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
	nanosleep(&ts, nullptr);
#endif
}

} // namespace

TEST_CASE("sim_tracking's scripted sweep drives xrLocateViews through sim_display (ADR-054 N6 Part 2)",
          "[oxr][view_space][viewer_tracking]")
{
	// sim_display does not track and declares EXTERNAL_OK, so the trunk serves
	// its screen from the best enabled viewer-tracking provider: sim_tracking
	// (generated manifest + DXR_TRACKING_SIM_TRACKING_ENABLE=1, SCRIPT=sweep,
	// PERIOD_MS=2000, AMPLITUDE_M=0.1 — tests_oxr_view_space_viewer_tracking).
	// The head starts at the nominal viewer and is +AMPLITUDE to the right a
	// quarter period later; the raw display-space eyes and the views follow.
	if (!viewer_tracking_process()) {
		WARN("this arm needs the sim-tracking provider - see tests_oxr_view_space_viewer_tracking");
		SUCCEED("not the viewer-tracking process");
		return;
	}
	Runtime rt;
	if (!bring_up(rt)) {
		return;
	}
	if (!rt.have_view_rig) {
		WARN("XR_DXR_view_rig not advertised: no raw eye channel to read");
		SUCCEED("nothing to read");
		tear_down(rt);
		return;
	}

	auto raw_centroid_x = [&](XrViewDisplayRawDXR &raw, std::vector<XrView> &views) {
		views = rt.views(rt.view, rt.now(), nullptr, &raw);
		REQUIRE(raw.eyeCountOutput >= 2);
		float cx = 0.0f;
		for (uint32_t i = 0; i < raw.eyeCountOutput; i++) {
			cx += raw.rawEyes[i].x;
		}
		return cx / (float)raw.eyeCountOutput;
	};

	XrViewDisplayRawDXR raw0 = {XR_TYPE_VIEW_DISPLAY_RAW_DXR};
	std::vector<XrView> v0;
	const float x0 = raw_centroid_x(raw0, v0); // starts the provider (on demand) at phase 0
	sleep_ms(500);                             // a quarter of PERIOD_MS=2000
	XrViewDisplayRawDXR raw1 = {XR_TYPE_VIEW_DISPLAY_RAW_DXR};
	std::vector<XrView> v1;
	const float x1 = raw_centroid_x(raw1, v1);

	const float vx0 = rt.centroid(v0).x;
	const float vx1 = rt.centroid(v1).x;
	std::printf(
	    "N6 viewer tracking: raw eye centroid x %.4f -> %.4f m (tracking %d/%d), view centroid x %.4f -> "
	    "%.4f\n",
	    x0, x1, (int)raw0.isTracking, (int)raw1.isTracking, vx0, vx1);
	CHECK(raw1.isTracking == XR_TRUE);
	CHECK(std::fabs(x0) < 0.03f); // phase 0: at the nominal viewer (sim's nominal x is 0)
	CHECK(x1 - x0 > 0.06f);       // a quarter period later: ~+0.1 m to the right
	CHECK(x1 - x0 < 0.12f);
	CHECK(raw1.rawEyes[1].x - raw1.rawEyes[0].x == Catch::Approx(0.063f).margin(0.002f)); // IPD_MM default
	CHECK(vx1 > vx0); // the views follow the same way

	tear_down(rt);
}

TEST_CASE("the composed advertisement: sim_display + a usable provider advertises MANAGED|MANUAL (ADR-054 N6 Part 2)",
          "[oxr][view_space][viewer_tracking]")
{
	if (!viewer_tracking_process()) {
		WARN("this arm needs the sim-tracking provider - see tests_oxr_view_space_viewer_tracking");
		SUCCEED("not the viewer-tracking process");
		return;
	}
	Runtime rt;
	BringUp opt;
	opt.display_info = true;
	if (!bring_up(rt, opt)) {
		return;
	}
	if (!rt.have_display_info) {
		WARN("XR_DXR_display_info not advertised");
		SUCCEED("nothing to read");
		tear_down(rt);
		return;
	}
	XrEyeTrackingModeCapabilitiesDXR caps = {XR_TYPE_EYE_TRACKING_MODE_CAPABILITIES_DXR};
	XrSystemProperties props = {XR_TYPE_SYSTEM_PROPERTIES};
	props.next = &caps;
	auto getProps = rt.fn<PFN_xrGetSystemProperties>("xrGetSystemProperties");
	REQUIRE(XR_SUCCEEDED(getProps(rt.instance, rt.system, &props)));
	std::printf("N6 composed advertisement: supportedModes=0x%x defaultMode=%d\n", (unsigned)caps.supportedModes,
	            (int)caps.defaultMode);
	// sim_display alone advertises 0 (honest); the trunk composes MANAGED|MANUAL.
	CHECK(caps.supportedModes == (XrEyeTrackingModeCapabilityFlagsDXR)3);
	CHECK((int)caps.defaultMode == 0); // MANAGED
	tear_down(rt);
}

TEST_CASE("a fixed-viewer locate asks for no viewer: the provider is not started (XR_DXR_display_info v25)",
          "[oxr][view_space][viewer_tracking][fixed_viewer]")
{
	// XrViewLocateFixedViewerDXR: a locate that chains it is framed for the
	// nominal viewer and posts no viewer-tracking demand. sim_tracking's sweep
	// (PERIOD_MS=2000, AMPLITUDE_M=0.1) starts at phase 0 when the provider
	// starts, so the time the provider started is readable off the eyes: had
	// any fixed locate below started it, the first normal locate a quarter
	// period later would already see the head ~+AMPLITUDE to the right.
	if (!viewer_tracking_process()) {
		WARN("this arm needs the sim-tracking provider - see tests_oxr_view_space_viewer_tracking");
		SUCCEED("not the viewer-tracking process");
		return;
	}
	Runtime rt;
	BringUp opt;
	opt.display_info = true;
	if (!bring_up(rt, opt)) {
		return;
	}
	if (!rt.have_display_info || !rt.have_view_rig) {
		WARN("XR_DXR_display_info or XR_DXR_view_rig not advertised: nothing to read");
		SUCCEED("nothing to read");
		tear_down(rt);
		return;
	}

	XrViewLocateFixedViewerDXR fixed = {XR_TYPE_VIEW_LOCATE_FIXED_VIEWER_DXR};
	fixed.fixedViewer = XR_TRUE;

	auto raw_centroid_x = [&](const void *next, XrViewDisplayRawDXR &raw) {
		(void)rt.views(rt.view, rt.now(), next, &raw);
		REQUIRE(raw.eyeCountOutput >= 2);
		float cx = 0.0f;
		for (uint32_t i = 0; i < raw.eyeCountOutput; i++) {
			cx += raw.rawEyes[i].x;
		}
		return cx / (float)raw.eyeCountOutput;
	};

	// A quarter period of fixed-viewer locates: the nominal viewer every time.
	for (int i = 0; i < 10; i++) {
		XrViewDisplayRawDXR raw = {XR_TYPE_VIEW_DISPLAY_RAW_DXR};
		const float x = raw_centroid_x(&fixed, raw);
		CHECK(raw.isTracking == XR_FALSE);
		CHECK(std::fabs(x) < 1e-4f); // sim's nominal x is 0
		sleep_ms(50);
	}

	// The first locate that asks for a viewer starts the provider: phase 0.
	XrViewDisplayRawDXR first = {XR_TYPE_VIEW_DISPLAY_RAW_DXR};
	const float x_first = raw_centroid_x(nullptr, first);
	sleep_ms(500); // a quarter of PERIOD_MS=2000
	XrViewDisplayRawDXR quarter = {XR_TYPE_VIEW_DISPLAY_RAW_DXR};
	const float x_quarter = raw_centroid_x(nullptr, quarter);
	std::printf("fixed viewer: first normal locate x %.4f, a quarter period later %.4f (tracking %d)\n", x_first,
	            x_quarter, (int)quarter.isTracking);
	CHECK(std::fabs(x_first) < 0.03f); // not already a quarter period in: no fixed locate started it
	CHECK(quarter.isTracking == XR_TRUE);
	CHECK(x_quarter - x_first > 0.06f);

	// With the provider running for other locates, a fixed one still gets the
	// nominal viewer, untracked.
	XrViewDisplayRawDXR again = {XR_TYPE_VIEW_DISPLAY_RAW_DXR};
	const float x_again = raw_centroid_x(&fixed, again);
	CHECK(again.isTracking == XR_FALSE);
	CHECK(std::fabs(x_again) < 1e-4f);

	tear_down(rt);
}


/*
 * ADR-034 Amendment 8: the stylus profile is wired end to end. The extension
 * is advertised, and with it enabled every input / output path of
 * /interaction_profiles/logitech/mx_ink_stylus_logitech binds on both hands
 * (floats for the forces, booleans for the clicks / double taps / dock, the
 * three poses, haptics). Without the extension the profile is refused, as the
 * spec requires of a vendor profile.
 */
namespace {

XrResult
suggest_mx_ink(Runtime &rt, bool expect_ok)
{
	auto stringToPath = rt.fn<PFN_xrStringToPath>("xrStringToPath");
	auto createActionSet = rt.fn<PFN_xrCreateActionSet>("xrCreateActionSet");
	auto createAction = rt.fn<PFN_xrCreateAction>("xrCreateAction");
	auto suggest = rt.fn<PFN_xrSuggestInteractionProfileBindings>("xrSuggestInteractionProfileBindings");

	auto path = [&](const char *str) {
		XrPath out = XR_NULL_PATH;
		REQUIRE(XR_SUCCEEDED(stringToPath(rt.instance, str, &out)));
		return out;
	};

	XrActionSetCreateInfo asci = {XR_TYPE_ACTION_SET_CREATE_INFO};
	std::snprintf(asci.actionSetName, sizeof(asci.actionSetName), "stylus%d", expect_ok ? 1 : 0);
	std::snprintf(asci.localizedActionSetName, sizeof(asci.localizedActionSetName), "Stylus");
	XrActionSet set = XR_NULL_HANDLE;
	REQUIRE(XR_SUCCEEDED(createActionSet(rt.instance, &asci, &set)));

	const XrPath hands[2] = {path("/user/hand/left"), path("/user/hand/right")};
	auto action = [&](const char *name, XrActionType type) {
		XrActionCreateInfo aci = {XR_TYPE_ACTION_CREATE_INFO};
		std::snprintf(aci.actionName, sizeof(aci.actionName), "%s", name);
		std::snprintf(aci.localizedActionName, sizeof(aci.localizedActionName), "%s", name);
		aci.actionType = type;
		aci.countSubactionPaths = 2;
		aci.subactionPaths = hands;
		XrAction a = XR_NULL_HANDLE;
		REQUIRE(XR_SUCCEEDED(createAction(set, &aci, &a)));
		return a;
	};

	struct Binding
	{
		const char *component;
		XrAction action;
	};
	const Binding components[] = {
	    {"/input/tip_logitech/force", action("tip_force", XR_ACTION_TYPE_FLOAT_INPUT)},
	    {"/input/cluster_middle_logitech/force", action("middle_force", XR_ACTION_TYPE_FLOAT_INPUT)},
	    {"/input/cluster_front_logitech/click", action("front_click", XR_ACTION_TYPE_BOOLEAN_INPUT)},
	    {"/input/cluster_front_logitech/double_tap_logitech", action("front_dtap", XR_ACTION_TYPE_BOOLEAN_INPUT)},
	    {"/input/cluster_back_logitech/click", action("back_click", XR_ACTION_TYPE_BOOLEAN_INPUT)},
	    {"/input/cluster_back_logitech/double_tap_logitech", action("back_dtap", XR_ACTION_TYPE_BOOLEAN_INPUT)},
	    {"/input/dock_logitech/docked_logitech", action("docked", XR_ACTION_TYPE_BOOLEAN_INPUT)},
	    {"/input/system/click", action("system", XR_ACTION_TYPE_BOOLEAN_INPUT)},
	    {"/input/grip/pose", action("grip", XR_ACTION_TYPE_POSE_INPUT)},
	    {"/input/aim/pose", action("aim", XR_ACTION_TYPE_POSE_INPUT)},
	    {"/input/tip_logitech/pose", action("tip", XR_ACTION_TYPE_POSE_INPUT)},
	    {"/output/haptic", action("haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT)},
	};

	std::vector<XrActionSuggestedBinding> b;
	for (const char *hand : {"/user/hand/left", "/user/hand/right"}) {
		for (const Binding &c : components) {
			const std::string full = std::string(hand) + c.component;
			b.push_back({c.action, path(full.c_str())});
		}
	}
	XrInteractionProfileSuggestedBinding sb = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
	sb.interactionProfile = path("/interaction_profiles/logitech/mx_ink_stylus_logitech");
	sb.countSuggestedBindings = (uint32_t)b.size();
	sb.suggestedBindings = b.data();
	return suggest(rt.instance, &sb);
}

} // namespace

TEST_CASE("XR_LOGITECH_mx_ink_stylus_interaction: every profile path binds (ADR-034 Amendment 8)",
          "[oxr][view_space][stylus_profile]")
{
	Runtime rt;
	BringUp opt;
	opt.mx_ink = true;
	if (!bring_up(rt, opt)) {
		return;
	}
	CHECK(suggest_mx_ink(rt, true) == XR_SUCCESS);
	tear_down(rt);
}

TEST_CASE("the stylus profile is refused without its extension", "[oxr][view_space][stylus_profile]")
{
	Runtime rt;
	if (!bring_up(rt)) {
		return;
	}
	CHECK(suggest_mx_ink(rt, false) == XR_ERROR_PATH_UNSUPPORTED);
	tear_down(rt);
}

namespace {

//! @p i on the runtime's monotonic clock (the clock the sim-stylus script reads).
int64_t
instant_mono_ns(const Instant &i)
{
#if defined(_WIN32)
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	return (int64_t)((double)i.qpc.QuadPart * (1e9 / (double)f.QuadPart));
#else
	return (int64_t)i.ts.tv_sec * 1000000000LL + (int64_t)i.ts.tv_nsec;
#endif
}

Instant
instant_add_ms(Instant i, int64_t ms)
{
#if defined(_WIN32)
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	i.qpc.QuadPart += f.QuadPart * ms / 1000;
#else
	int64_t ns = (int64_t)i.ts.tv_nsec + ms * 1000000LL;
	i.ts.tv_sec += (time_t)(ns / 1000000000LL);
	i.ts.tv_nsec = (long)(ns % 1000000000LL);
#endif
	return i;
}

/*!
 * An instant ahead at which the default sim-stylus script (MODE=draw,
 * PERIOD_S=6) has its pen ON the glass, mid-stroke: lap phase in
 * [0.15, 0.70] of the 0.85 contact window, well clear of its ends.
 */
Instant
instant_pen_on_glass()
{
	Instant i = instant_ahead();
	for (int k = 0; k < 32; k++) {
		const double t = (double)instant_mono_ns(i) * 1e-9;
		const double u = std::fmod(t, 6.0) / 6.0;
		if (u > 0.15 && u < 0.70) {
			return i;
		}
		i = instant_add_ms(i, 250);
	}
	return i;
}

//! The stylus tip pose action, attached, with one action space per hand.
struct StylusTip
{
	XrActionSet set = XR_NULL_HANDLE;
	XrAction tip = XR_NULL_HANDLE;
	XrSpace space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
};

void
stylus_tip_setup(Runtime &rt, StylusTip &st)
{
	auto stringToPath = rt.fn<PFN_xrStringToPath>("xrStringToPath");
	auto createActionSet = rt.fn<PFN_xrCreateActionSet>("xrCreateActionSet");
	auto createAction = rt.fn<PFN_xrCreateAction>("xrCreateAction");
	auto suggest = rt.fn<PFN_xrSuggestInteractionProfileBindings>("xrSuggestInteractionProfileBindings");
	auto attach = rt.fn<PFN_xrAttachSessionActionSets>("xrAttachSessionActionSets");
	auto createActionSpace = rt.fn<PFN_xrCreateActionSpace>("xrCreateActionSpace");

	auto path = [&](const char *str) {
		XrPath out = XR_NULL_PATH;
		REQUIRE(XR_SUCCEEDED(stringToPath(rt.instance, str, &out)));
		return out;
	};
	const XrPath hands[2] = {path("/user/hand/left"), path("/user/hand/right")};

	XrActionSetCreateInfo asci = {XR_TYPE_ACTION_SET_CREATE_INFO};
	std::snprintf(asci.actionSetName, sizeof(asci.actionSetName), "stylus_glass");
	std::snprintf(asci.localizedActionSetName, sizeof(asci.localizedActionSetName), "Stylus glass");
	REQUIRE(XR_SUCCEEDED(createActionSet(rt.instance, &asci, &st.set)));

	XrActionCreateInfo aci = {XR_TYPE_ACTION_CREATE_INFO};
	std::snprintf(aci.actionName, sizeof(aci.actionName), "tip");
	std::snprintf(aci.localizedActionName, sizeof(aci.localizedActionName), "Tip");
	aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
	aci.countSubactionPaths = 2;
	aci.subactionPaths = hands;
	REQUIRE(XR_SUCCEEDED(createAction(st.set, &aci, &st.tip)));

	const XrActionSuggestedBinding b[2] = {
	    {st.tip, path("/user/hand/left/input/tip_logitech/pose")},
	    {st.tip, path("/user/hand/right/input/tip_logitech/pose")},
	};
	XrInteractionProfileSuggestedBinding sb = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
	sb.interactionProfile = path("/interaction_profiles/logitech/mx_ink_stylus_logitech");
	sb.countSuggestedBindings = 2;
	sb.suggestedBindings = b;
	REQUIRE(XR_SUCCEEDED(suggest(rt.instance, &sb)));

	XrSessionActionSetsAttachInfo ai = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	ai.countActionSets = 1;
	ai.actionSets = &st.set;
	REQUIRE(XR_SUCCEEDED(attach(rt.session, &ai)));

	for (int h = 0; h < 2; h++) {
		XrActionSpaceCreateInfo si = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
		si.action = st.tip;
		si.subactionPath = hands[h];
		si.poseInActionSpace = {{0, 0, 0, 1}, {0, 0, 0}};
		REQUIRE(XR_SUCCEEDED(createActionSpace(rt.session, &si, &st.space[h])));
	}
}

void
stylus_tip_sync(Runtime &rt, StylusTip &st)
{
	auto sync = rt.fn<PFN_xrSyncActions>("xrSyncActions");
	XrActiveActionSet active = {st.set, XR_NULL_PATH};
	XrActionsSyncInfo si = {XR_TYPE_ACTIONS_SYNC_INFO};
	si.countActiveActionSets = 1;
	si.activeActionSets = &active;
	REQUIRE(XR_SUCCEEDED(sync(rt.session, &si)));
}

//! The tip in @p base, from whichever hand the stylus is on. False = no pen.
bool
stylus_tip_locate(Runtime &rt, StylusTip &st, XrSpace base, XrTime t, XrPosef *out)
{
	for (int h = 1; h >= 0; h--) { // the dominant (right) hand first
		XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
		REQUIRE(XR_SUCCEEDED(rt.pfnLocateSpace(st.space[h], base, t, &loc)));
		const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT;
		if ((loc.locationFlags & need) == need) {
			*out = loc.pose;
			return true;
		}
	}
	return false;
}

/*!
 * Where @p p (in the views' base) projects in view @p v: normalised device
 * coordinates of the view's own asymmetric frustum, and the depth in front of
 * the eye (> 0 = in front).
 */
void
project_into_view(const XrView &v, const XrVector3f &p, float *out_ndc_x, float *out_ndc_y, float *out_depth)
{
	const XrPosef at = {{0, 0, 0, 1}, p};
	const XrVector3f e = pmul(pinv(v.pose), at).position;
	*out_depth = -e.z;
	const float d = *out_depth != 0.0f ? *out_depth : 1e-9f;
	const float tx = e.x / d;
	const float ty = e.y / d;
	const float tl = std::tan(v.fov.angleLeft), tr = std::tan(v.fov.angleRight);
	const float td = std::tan(v.fov.angleDown), tu = std::tan(v.fov.angleUp);
	*out_ndc_x = (2.0f * tx - (tr + tl)) / (tr - tl);
	*out_ndc_y = (2.0f * ty - (tu + td)) / (tu - td);
}

//! A pen tip on the glass is at zero parallax: the same NDC in both views.
void
check_tip_on_glass(const std::string &what, const std::vector<XrView> &views, const XrPosef &tip)
{
	REQUIRE(views.size() >= 2);
	float x0, y0, z0, x1, y1, z1;
	project_into_view(views[0], tip.position, &x0, &y0, &z0);
	project_into_view(views[1], tip.position, &x1, &y1, &z1);
	std::printf("stylus %-20s tip (%.3f, %.3f, %.3f)  eye0 depth %.3f ndc (%.4f, %.4f)  eye1 depth %.3f ndc "
	            "(%.4f, %.4f)\n",
	            what.c_str(), tip.position.x, tip.position.y, tip.position.z, z0, x0, y0, z1, x1, y1);
	CAPTURE(what, tip.position.x, tip.position.y, tip.position.z, z0, x0, y0, z1, x1, y1);
	// In front of both eyes, well past the near plane: not in the eye plane.
	CHECK(z0 > 0.05f);
	CHECK(z1 > 0.05f);
	// Zero parallax: the two eyes see the tip at the same pixel.
	CHECK(std::fabs(x0 - x1) < 0.01f);
	CHECK(std::fabs(y0 - y1) < 0.01f);
	// And on the part of the glass the views frame (the figure-eight is
	// inside the panel and these sessions' canvas is the whole panel).
	CHECK(std::fabs(x0) < 1.0f);
	CHECK(std::fabs(y0) < 1.0f);
}

} // namespace

TEST_CASE("RIG_LOCAL_PHYSICAL: a pen on the glass is at zero parallax in the session's own views",
          "[oxr][view_space][stylus_glass]")
{
	// ADR-034 Amendment 8 / input-provider-discovery.md section 4b.4a: a
	// glass-registered pose L on the glass (z = 0) lands where the session's
	// VIEWS put that point of the glass, so both eyes see it at the same pixel,
	// whatever base the app locates in. Field report (cube_hosted_d3d11_win
	// --stylus, STAGE): a runtime-window session with nothing chained put the
	// tip in the EYE plane. Such a session's views come from the runtime's
	// fly-camera rig (#595, CAMERA mode by default): the head device pose is
	// the camera, and the glass is the convergence plane in front of it, not
	// rig(t) o L. The oracle is the views' own projection, not any
	// runtime-reported plane.
	const char *enable = std::getenv("DXR_INPUT_SIM_STYLUS_ENABLE");
	if (enable == nullptr || std::strcmp(enable, "1") != 0) {
		WARN("this arm needs the sim-stylus provider - see tests_oxr_view_space_stylus in tests/CMakeLists.txt");
		SUCCEED("not the stylus process");
		return;
	}

	Runtime rt;
	BringUp opt;
	opt.mx_ink = true;
	if (!bring_up(rt, opt)) {
		return;
	}
	StylusTip st;
	stylus_tip_setup(rt, st);
	stylus_tip_sync(rt, st);

	const Instant at = instant_pen_on_glass();
	const XrTime t = instant_to_xr(rt, at);

	const Base bases[] = {{"LOCAL", rt.local}, {"STAGE", rt.stage}};
	bool any = false;
	for (const Base &b : bases) {
		// Views first (the frame order every app uses): the locate records
		// the glass the pen is registered on.
		const std::vector<XrView> views = rt.views(b.space, t, nullptr, nullptr);
		XrPosef tip{};
		if (!stylus_tip_locate(rt, st, b.space, t, &tip)) {
			continue;
		}
		any = true;
		check_tip_on_glass(std::string("no rig, ") + b.name, views, tip);
	}
	if (!any) {
		WARN("no stylus device bound (Windows discovery is registry-only: register the sim-stylus plug-in)");
		SUCCEED("no pen to locate");
		tear_down(rt);
		return;
	}

	if (rt.have_view_rig) {
		// A chained display rig posed away from the physical plane and scaled
		// (virtual height 1.5 x the canvas): on the glass of THOSE views.
		XrViewDisplayRawDXR raw0 = {XR_TYPE_VIEW_DISPLAY_RAW_DXR};
		(void)rt.views(rt.local, t, nullptr, &raw0);
		REQUIRE(raw0.canvasSizeMeters.height > 0.0f);
		XrDisplayRigDXR rig = {XR_TYPE_DISPLAY_RIG_DXR};
		rig.pose = {qyaw(0.3f), {0.2f, 0.1f, -0.4f}};
		rig.virtualDisplayHeight = 1.5f * raw0.canvasSizeMeters.height;
		rig.ipdFactor = 1.0f;
		rig.parallaxFactor = 1.0f;
		rig.perspectiveFactor = 1.0f;
		const std::vector<XrView> views = rt.views(rt.local, t, &rig, nullptr);
		XrPosef tip{};
		REQUIRE(stylus_tip_locate(rt, st, rt.local, t, &tip));
		check_tip_on_glass("display rig, LOCAL", views, tip);

		// A chained camera rig: the glass is its convergence plane.
		XrCameraRigDXR cam = {XR_TYPE_CAMERA_RIG_DXR};
		cam.pose = {qyaw(-0.2f), {-0.1f, 0.2f, 0.3f}};
		cam.ipdFactor = 1.0f;
		cam.parallaxFactor = 1.0f;
		cam.convergenceDiopters = 1.0f / 0.8f;
		cam.verticalFov = 0.6f;
		cam.metersToVirtual = 1.0f;
		const std::vector<XrView> cviews = rt.views(rt.local, t, &cam, nullptr);
		REQUIRE(stylus_tip_locate(rt, st, rt.local, t, &tip));
		check_tip_on_glass("camera rig, LOCAL", cviews, tip);
	}

	tear_down(rt);
}


/*
 * ADR-066 C1 / XR_DXR_display_info v27: a live 3D-mode switch, end to end in a
 * real in-process session. The driver's default 3D mode (sim-display's OUTPUT,
 * read through the host as DXR_DP_SIM_DISPLAY_OUTPUT, which outranks the legacy
 * alias this binary sets) changes under the running session; within the 1 s
 * poll the enumerate answers the new slot 1, one
 * XrEventDataRenderingModeTableChangedDXR arrives with the unchanged index, and
 * no XrEventDataRenderingModeChangedDXR with an unchanged index is ever sent.
 */
namespace {

void
set_sim_output(const char *v)
{
#ifdef _WIN32
	_putenv_s("DXR_DP_SIM_DISPLAY_OUTPUT", v != nullptr ? v : "");
#else
	if (v != nullptr) {
		setenv("DXR_DP_SIM_DISPLAY_OUTPUT", v, 1);
	} else {
		unsetenv("DXR_DP_SIM_DISPLAY_OUTPUT");
	}
#endif
}

} // namespace

TEST_CASE("ADR-066 C1: a live 3D-mode switch sends the table-changed event, never a same-index mode change",
          "[oxr][view_space][mode_table_live]")
{
	if (legacy_switch_set()) {
		WARN("DXR_VIEW_CONFIG_LEGACY is armed - nothing to pin here");
		SUCCEED("legacy process");
		return;
	}
	Runtime rt;
	BringUp opt;
	opt.display_info = true;
	opt.begin = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR;
	if (!bring_up(rt, opt)) {
		return;
	}
	if (!rt.have_display_info) {
		WARN("XR_DXR_display_info not advertised");
		tear_down(rt);
		return;
	}
	const std::vector<XrDisplayRenderingModeInfoDXR> before = rt.rendering_modes();
	REQUIRE(before.size() == 2);
	if (std::string(before[1].modeName) != "Anaglyph") {
		// Not sim-display's list (a vendor plug-in won the head): nothing to switch.
		WARN("slot 1 is '" << before[1].modeName << "', not sim-display's Anaglyph - arm inert");
		SUCCEED("not sim-display");
		tear_down(rt);
		return;
	}
	uint32_t active_before = 0;
	for (const auto &m : before) {
		if (m.isActive == XR_TRUE) {
			active_before = m.modeIndex;
		}
	}
	(void)rt.drain_events();

	set_sim_output("sbs");
	uint32_t table_events = 0;
	bool same_index_mode_event = false;
	uint32_t reported_active = 0xFFFFFFFFu;
	uint64_t reported_flags = 0xFFu;
	for (int i = 0; i < 60 && table_events == 0; i++) { // up to 3 s
		for (const XrEventDataBuffer &ev : rt.drain_events()) {
			if (ev.type == XR_TYPE_EVENT_DATA_RENDERING_MODE_TABLE_CHANGED_DXR) {
				const auto *t = reinterpret_cast<const XrEventDataRenderingModeTableChangedDXR *>(&ev);
				table_events++;
				reported_active = t->activeModeIndex;
				reported_flags = (uint64_t)t->flags;
			} else if (ev.type == XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_DXR) {
				const auto *m = reinterpret_cast<const XrEventDataRenderingModeChangedDXR *>(&ev);
				same_index_mode_event |= m->previousModeIndex == m->currentModeIndex;
			}
		}
		if (table_events == 0) {
			sleep_ms(50);
		}
	}
	const std::vector<XrDisplayRenderingModeInfoDXR> after = rt.rendering_modes();
	set_sim_output(nullptr);

	CHECK(table_events == 1);
	CHECK(reported_active == active_before);
	CHECK(reported_flags == 0); // C1 never asks for recreation
	CHECK_FALSE(same_index_mode_event);
	REQUIRE(after.size() == 2);
	CHECK(std::string(after[1].modeName) == "Cropped SBS");
	CHECK(after[1].tileColumns == 1);
	CHECK(after[1].tileRows == 2);
	CHECK(after[0].isActive == before[0].isActive); // the index did not move

	// Coalesced: nothing more within the second that followed.
	bool more = false;
	for (const XrEventDataBuffer &ev : rt.drain_events()) {
		more |= ev.type == XR_TYPE_EVENT_DATA_RENDERING_MODE_TABLE_CHANGED_DXR;
	}
	CHECK_FALSE(more);

	tear_down(rt);
}
