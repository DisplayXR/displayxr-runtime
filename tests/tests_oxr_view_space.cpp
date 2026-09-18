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

/*!
 * #1499: is the plug-in's default rendering mode a >2-view one for THIS process?
 *
 * The mode floor is only observable on a device whose active mode a stereo
 * session cannot fill, and the only such device is sim-display in its Quad
 * mode. That is chosen by `SIM_DISPLAY_OUTPUT=quad` at plug-in load, so it has
 * to be a process-level fact - hence the separate ctest registrations in
 * tests/CMakeLists.txt (`tests_oxr_view_space_mode_floor` /
 * `_mode_pinned`). Under the plain registration these arms have nothing to
 * observe and say so.
 */
bool
sim_quad_requested()
{
	const char *v = std::getenv("SIM_DISPLAY_OUTPUT");
	return v != nullptr && std::strcmp(v, "quad") == 0;
}

//! #1499: is SIM_DISPLAY_FORCE_MODE armed (the device PINS its mode)?
bool
sim_mode_pinned()
{
	const char *v = std::getenv("SIM_DISPLAY_FORCE_MODE");
	return v != nullptr && v[0] != '\0' && std::strcmp(v, "-1") != 0;
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

		uint32_t count = 0;
		REQUIRE(XR_SUCCEEDED(pfnLocateViews(session, &li, &vs, 0, &count, nullptr)));
		REQUIRE(count >= 1);
		std::vector<XrView> out(count, {XR_TYPE_VIEW});
		REQUIRE(XR_SUCCEEDED(pfnLocateViews(session, &li, &vs, count, &count, out.data())));
		const XrViewStateFlags need = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
		REQUIRE((vs.viewStateFlags & need) == need);
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
 * #1502 + #1486: the centroid is over the REPORTED array, not the eye set.
 *
 * Under PRIMARY_MULTIVIEW_DXR the session reports the device max (4 on
 * sim_display) while the active rendering mode may be narrower; xrLocateViews
 * fills the surplus slots by duplicating view 0. An app - and the CTS, which
 * loops over every enumerated view configuration - averages what it was handed,
 * so VIEW must be the mean of THAT array, duplicates included.
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
		CHECK(v.size() == device_max);
	}

	const XrPosef T_local_view = rt.locate(rt.view, rt.local, t);
	const XrVector3f c = rt.centroid(v);
	INFO("VIEW in LOCAL: " << pstr(T_local_view));
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
		CHECK(rt.config_view_count(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR) == device_max);
		// A valid core type the system does NOT advertise is still refused.
		CHECK(rt.config_view_count(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO) == 0);
	}

	SECTION("a MULTIVIEW session locates the device max and refuses PRIMARY_STEREO")
	{
		const XrTime t = rt.now();

		uint32_t n = 0;
		CHECK(rt.locate_count(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR, t, &n) == XR_SUCCESS);
		CHECK(n == device_max);

		// Same session, the OTHER advertised configuration: valid enum, wrong
		// session - UNSUPPORTED, not VALIDATION_FAILURE.
		CHECK(rt.locate_count(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, t, nullptr) ==
		      XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED);

		// And the full locate really writes that many views.
		std::vector<XrView> v = rt.views(rt.local, t, nullptr, nullptr);
		CHECK(v.size() == device_max);
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
 * which mode sim-display comes up in is decided by `SIM_DISPLAY_OUTPUT` when
 * the plug-in is loaded, and `SIM_DISPLAY_FORCE_MODE` is read through
 * DEBUG_GET_ONCE_NUM_OPTION, so one binary cannot host both the floored and the
 * pinned case. tests/CMakeLists.txt registers this file twice more:
 * `tests_oxr_view_space_mode_floor` (SIM_DISPLAY_OUTPUT=quad, tag
 * `[mode_floor]`) and `tests_oxr_view_space_mode_pinned`
 * (SIM_DISPLAY_FORCE_MODE=4, tag `[mode_pinned]`).
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
		    "this arm needs SIM_DISPLAY_OUTPUT=quad, an UNpinned device and DXR_MODE_FLOOR on "
		    "- see tests_oxr_view_space_mode_floor in tests/CMakeLists.txt");
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

	SECTION("after xrBeginSession the active mode is one this session can fill")
	{
		XrDisplayRenderingModeInfoDXR active{};
		REQUIRE(rt.active_mode(&active));
		INFO("active mode " << active.modeIndex << " '" << active.modeName << "' viewCount "
		                    << active.viewCount);
		// THE assertion: a 2-view session is not left sitting in Quad.
		CHECK(active.viewCount <= 2);
		// ...and it kept 3D rather than falling back to the 2D mode, because
		// sim-display has 2-view 3D modes to fall back to.
		CHECK(active.hardwareDisplay3D == XR_TRUE);
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
		// previousModeIndex is the mode the floor took us OUT of - Quad, the
		// mode SIM_DISPLAY_OUTPUT=quad started us in.
		CHECK(changed_from == 4u);

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
		    "this arm needs SIM_DISPLAY_OUTPUT=quad, an UNpinned device and DXR_MODE_FLOOR on "
		    "- see tests_oxr_view_space_mode_floor in tests/CMakeLists.txt");
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
		    "this arm needs SIM_DISPLAY_FORCE_MODE=4 - see tests_oxr_view_space_mode_pinned "
		    "in tests/CMakeLists.txt");
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
	CHECK(active.modeIndex == 4u); // SIM_DISPLAY_FORCE_MODE=4, Quad
	CHECK(active.viewCount > 2);

	/*
	 * ...and the pin exempts the DENIAL too, not just the floor. This session
	 * is SITTING in mode 4, so denying a request for mode 4 would be refusing
	 * it permission to ask for the mode it is already in - and the device, not
	 * the runtime, is the authority on a pinned mode. The first cut of the
	 * denial gate got this wrong.
	 */
	rt.drain_events();
	auto request = rt.fn<PFN_xrRequestDisplayRenderingModeDXR>("xrRequestDisplayRenderingModeDXR");
	CHECK(request(rt.session, 4u) == XR_SUCCESS);
	for (const XrEventDataBuffer &ev : rt.drain_events()) {
		CHECK(ev.type != XR_TYPE_EVENT_DATA_DISPLAY_MODE_REQUEST_DENIED_DXR);
	}
	// The device still holds the mode, which is the whole point of the pin.
	REQUIRE(rt.active_mode(&active));
	CHECK(active.modeIndex == 4u);

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
		    "this arm needs SIM_DISPLAY_OUTPUT=quad, an UNpinned device and DXR_MODE_FLOOR on "
		    "- see tests_oxr_view_space_mode_floor in tests/CMakeLists.txt");
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
	REQUIRE(unfillable >= 0); // Quad
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
		    "this arm needs DXR_MODE_FLOOR=0 AND SIM_DISPLAY_OUTPUT=quad - see "
		    "tests_oxr_view_space_mode_floor_off in tests/CMakeLists.txt");
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
