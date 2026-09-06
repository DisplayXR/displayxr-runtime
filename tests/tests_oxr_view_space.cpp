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
 */

#include "catch_amalgamated.hpp"

#include <cmath>
#include <cstdio>
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

#ifndef DXR_RUNTIME_LIB_PATH
#error "DXR_RUNTIME_LIB_PATH must name the built runtime library"
#endif

namespace {

constexpr float kPosTolM = 0.001f;    // 1 mm
constexpr float kAngTolDeg = 0.1f;    // 0.1 degree

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
		li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
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

//! Returns false (after SKIP-ing the test) when the runtime cannot come up
//! headlessly - no display processor registered on this box.
bool
bring_up(Runtime &rt)
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
	sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
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
			if (rt.have_view_rig) {
				INFO("displayPlanePose: " << pstr(raw.displayPlanePose));
				CHECK(vdist(raw.displayPlanePose.position, T_base_view.position) < kPosTolM);
				CHECK(qangle_deg(raw.displayPlanePose.orientation, T_base_view.orientation) < kAngTolDeg);
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
