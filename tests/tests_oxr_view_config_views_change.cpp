// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief #1488 acceptance: XR_EXT_view_configuration_views_change.
 *
 * TWO halves, because neither alone can cover the issue's six cases.
 *
 * 1. UNIT (the bulk). oxr_views_change.c is compiled straight into this binary
 *    (the tests_rig_composer pattern) and driven with an INJECTED now_ns. That
 *    is the whole reason the state machine was extracted into its own
 *    translation unit: the throttle is then testable with no sleeping, no wall
 *    clock, no compositor and no GPU, and the kill switches arrive as plain
 *    parameters rather than as process-wide DEBUG_GET_ONCE_* caches that can
 *    only be set once per process.
 *
 *    Why not drive the real fire site instead: oxr_session_frame_end() returns
 *    at its `xc == NULL` early-out on a headless session, so the update site is
 *    unreachable from a headless harness; and the runtime library's exports are
 *    whitelisted in libopenxr.def / libopenxr.version / exported_symbols.list,
 *    so there is no dlsym seam either.
 *
 * 2. INTEGRATION (registration + the pre-change gate). Loads the BUILT runtime
 *    library via xrNegotiateLoaderRuntimeInterface exactly like
 *    tests_oxr_view_space.cpp, and proves end-to-end that the extension is
 *    advertised at specVersion 1, that an instance can enable it, and that
 *    before any change has happened an EXT-enabled instance and a plain one
 *    enumerate identical views. SKIPs (never fails) when no display processor
 *    is registered on the box.
 *
 * Issue cases -> test cases:
 *   1 EXT not enabled  -> "case 1"     6 DXR_VIEWS_CHANGE_LIVE=0 -> "case 6"
 *   2 recommended moves -> "case 2"    3 count invariant         -> "case 3"
 *   4 throttle/coalesce -> "case 4"    5 no change, no doorbell  -> "case 5"
 */

#include "catch_amalgamated.hpp"

#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "oxr_views_change.h"
}

#if defined(DXR_RUNTIME_LIB_PATH)
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#define XR_USE_PLATFORM_WIN32
#else
#include <dlfcn.h>
#endif
#include <openxr/openxr_loader_negotiation.h>
#endif

namespace {

constexpr uint32_t kViewCount = 2;
constexpr uint64_t kSec = OXR_VIEWS_CHANGE_MIN_PERIOD_NS;

//! A frozen xrCreateInstance-time snapshot, the way oxr_system_fill_in() leaves it.
struct Frozen
{
	XrViewConfigurationView v[XRT_MAX_VIEWS];

	Frozen()
	{
		std::memset(v, 0, sizeof(v));
		for (uint32_t i = 0; i < XRT_MAX_VIEWS; i++) {
			v[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
			v[i].recommendedImageRectWidth = 800;
			v[i].recommendedImageRectHeight = 600;
			v[i].maxImageRectWidth = 4096;
			v[i].maxImageRectHeight = 4096;
			v[i].recommendedSwapchainSampleCount = 1;
			v[i].maxSwapchainSampleCount = 4;
		}
	}
};

//! RAII around the state machine so every case starts from a clean lock.
struct VC
{
	struct oxr_views_change vc;
	Frozen frozen;

	VC()
	{
		REQUIRE(oxr_views_change_init(&vc) >= 0);
		// Seed every slot, not just kViewCount, so a read at a LARGER
		// count than the update used has well-defined content to prove
		// select() copies the shadow verbatim rather than inventing.
		oxr_views_change_seed(&vc, frozen.v, XRT_MAX_VIEWS);
	}
	~VC()
	{
		oxr_views_change_fini(&vc);
	}

	struct oxr_views_change_stats stats = {};

	//! Feed dims at @p now_ns; returns "the caller should ring the doorbell".
	bool
	feed(uint32_t w, uint32_t h, uint64_t now_ns, bool ext_enabled = true)
	{
		return oxr_views_change_update(&vc, frozen.v, kViewCount, w, h, now_ns, ext_enabled, &stats);
	}

	//! What xrEnumerateViewConfigurationViews would answer.
	const XrViewConfigurationView *
	read(bool ext_enabled, bool live_enabled, XrViewConfigurationView *scratch, uint32_t count = kViewCount)
	{
		return oxr_views_change_select(&vc, frozen.v, count, ext_enabled, live_enabled, scratch);
	}
};

//! The fields the extension forbids moving, plus the two it allows.
void
require_immutable_fields_match(const XrViewConfigurationView &got, const XrViewConfigurationView &frozen)
{
	REQUIRE(got.maxImageRectWidth == frozen.maxImageRectWidth);
	REQUIRE(got.maxImageRectHeight == frozen.maxImageRectHeight);
	REQUIRE(got.recommendedSwapchainSampleCount == frozen.recommendedSwapchainSampleCount);
	REQUIRE(got.maxSwapchainSampleCount == frozen.maxSwapchainSampleCount);
}

} // namespace


TEST_CASE("case 1: EXT not enabled - enumerate is frozen across a size change (#1488)", "[oxr][views_change]")
{
	VC t;
	XrViewConfigurationView scratch[XRT_MAX_VIEWS];

	// Baseline sample, then two real changes - fed with ext_enabled=true so
	// the shadow really does hold new values; the READ is what case 1 gates.
	t.feed(800, 600, 1 * kSec);
	t.feed(1280, 720, 2 * kSec);
	t.feed(1920, 1080, 4 * kSec);

	const XrViewConfigurationView *a = t.read(/*ext*/ false, /*live*/ true, scratch);
	REQUIRE(a == t.frozen.v); // the frozen array itself, not a copy of it

	// "Identical buffer contents ... for the lifetime of the instance": two
	// reads across the change are byte-equal to the snapshot.
	for (uint32_t i = 0; i < kViewCount; i++) {
		REQUIRE(a[i].recommendedImageRectWidth == 800);
		REQUIRE(a[i].recommendedImageRectHeight == 600);
		require_immutable_fields_match(a[i], t.frozen.v[i]);
	}
	const XrViewConfigurationView *b = t.read(false, true, scratch);
	REQUIRE(std::memcmp(a, b, sizeof(XrViewConfigurationView) * kViewCount) == 0);
}

TEST_CASE("case 2: EXT enabled - only recommendedImageRect* moves (#1488)", "[oxr][views_change]")
{
	VC t;
	XrViewConfigurationView scratch[XRT_MAX_VIEWS];

	t.feed(800, 600, 1 * kSec); // baseline, no change yet
	REQUIRE(t.read(true, true, scratch) == t.frozen.v);

	REQUIRE(t.feed(1280, 720, 3 * kSec)); // a real change -> doorbell

	const XrViewConfigurationView *got = t.read(true, true, scratch);
	REQUIRE(got == scratch);
	for (uint32_t i = 0; i < kViewCount; i++) {
		REQUIRE(got[i].recommendedImageRectWidth == 1280);
		REQUIRE(got[i].recommendedImageRectHeight == 720);
		// The whole point: max* and BOTH sample counts are untouched.
		require_immutable_fields_match(got[i], t.frozen.v[i]);
		REQUIRE(got[i].type == XR_TYPE_VIEW_CONFIGURATION_VIEW);
	}
}

TEST_CASE("case 3: the count is a parameter, never a baked-in view_count (#1488)", "[oxr][views_change]")
{
	VC t;
	t.feed(800, 600, 1 * kSec);
	REQUIRE(t.feed(1600, 900, 3 * kSec));

	// select() fills exactly `count` entries and touches nothing past them,
	// so the caller - which passes the count of the REQUESTED view
	// configuration type - is what decides the count, not this module.
	for (uint32_t count = 1; count <= 4; count++) {
		XrViewConfigurationView scratch[XRT_MAX_VIEWS];
		std::memset(scratch, 0xAB, sizeof(scratch));

		const XrViewConfigurationView *got = t.read(true, true, scratch, count);
		REQUIRE(got == scratch);
		for (uint32_t i = 0; i < count; i++) {
			// The update above ran with view_count == kViewCount, so
			// only those entries carry the new dims; the rest still
			// hold what oxr_views_change_seed() put there. Reading a
			// larger count must NOT invent values - it copies exactly
			// what the shadow holds.
			const bool fed = i < kViewCount;
			REQUIRE(got[i].recommendedImageRectWidth == (fed ? 1600u : 800u));
			REQUIRE(got[i].recommendedImageRectHeight == (fed ? 900u : 600u));
			require_immutable_fields_match(got[i], t.frozen.v[i]);
		}
		// Entry `count` was never written: the poison survives.
		if (count < XRT_MAX_VIEWS) {
			XrViewConfigurationView poison;
			std::memset(&poison, 0xAB, sizeof(poison));
			REQUIRE(std::memcmp(&scratch[count], &poison, sizeof(poison)) == 0);
		}
	}
}

TEST_CASE("case 4: 1 Hz throttle coalesces, and the last value wins (#1488)", "[oxr][views_change]")
{
	VC t;
	XrViewConfigurationView scratch[XRT_MAX_VIEWS];

	uint64_t now = 10 * kSec;
	t.feed(800, 600, now); // baseline

	// A drag-resize: 30 distinct sizes at 16 ms, i.e. ~0.5 s of burst, all
	// well inside one throttle window after the first doorbell.
	int doorbells = 0;
	uint32_t w = 800;
	for (int i = 0; i < 30; i++) {
		now += 16 * 1000 * 1000ULL;
		w += 4;
		if (t.feed(w, 600, now)) {
			doorbells++;
		}
	}
	// Exactly one: the first change re-arms immediately (last_push_ns == 0),
	// the other 29 are suppressed.
	REQUIRE(doorbells == 1);

	// ...but the ENUMERATE answer was never throttled - it already carries
	// the coalesced final value, which is what the app reads when it wakes.
	const XrViewConfigurationView *got = t.read(true, true, scratch);
	REQUIRE(got[0].recommendedImageRectWidth == w);

	// The deferred doorbell still fires, without any further change, on the
	// first frame end after the window closes: pending_push survived.
	REQUIRE(t.feed(w, 600, now + 10 * 1000 * 1000ULL) == false); // still inside 1 s
	REQUIRE(t.feed(w, 600, now + kSec) == true);                 // window closed -> deferred fire
	// 30 edges, 2 doorbells: 28 were folded into the deferred one. This is the
	// `suppressed=` field the soak line reports.
	REQUIRE(t.stats.edges == 30);
	REQUIRE(t.stats.emitted == 2);
	REQUIRE(t.stats.suppressed == 28);
	// And it does not fire twice for the same suppressed change.
	REQUIRE(t.feed(w, 600, now + 3 * kSec) == false);
}

TEST_CASE("case 5: a no-change frame produces no doorbell at all (#1488)", "[oxr][views_change]")
{
	VC t;
	XrViewConfigurationView scratch[XRT_MAX_VIEWS];

	// Even with the throttle wide open (each call a full second apart), a
	// frame whose dims equal the last ones must ring nothing. This is the
	// Design-7 / LOVR-hazard guarantee: a consumer that recreates swapchains
	// unconditionally on the event can never be triggered for nothing.
	for (int i = 1; i <= 10; i++) {
		REQUIRE(t.feed(800, 600, (uint64_t)i * 10 * kSec) == false);
	}
	// And no shadow was written, so the read stays on the frozen array.
	REQUIRE(t.read(true, true, scratch) == t.frozen.v);

	// The first sample after a real change also baselines without firing
	// twice: one change, one doorbell.
	REQUIRE(t.feed(1024, 768, 200 * kSec) == true);
	REQUIRE(t.feed(1024, 768, 300 * kSec) == false);
}

TEST_CASE("case 6: DXR_VIEWS_CHANGE_LIVE=0 restores case 1 with the EXT enabled (#1488)", "[oxr][views_change]")
{
	VC t;
	XrViewConfigurationView scratch[XRT_MAX_VIEWS];

	t.feed(800, 600, 1 * kSec);
	REQUIRE(t.feed(1280, 720, 3 * kSec));

	// Live values off: the frozen snapshot, byte for byte, even though the
	// shadow holds 1280x720.
	const XrViewConfigurationView *got = t.read(/*ext*/ true, /*live*/ false, scratch);
	REQUIRE(got == t.frozen.v);
	REQUIRE(got[0].recommendedImageRectWidth == 800);
	REQUIRE(got[0].recommendedImageRectHeight == 600);

	// The fire site ANDs the same switch into ext_enabled, so live=0 also
	// silences the doorbell - a doorbell whose enumerate answer cannot move
	// is exactly the hazard this work exists to prevent.
	VC t2;
	t2.feed(800, 600, 1 * kSec, /*ext_enabled*/ false);
	REQUIRE(t2.feed(1280, 720, 3 * kSec, /*ext_enabled*/ false) == false);
	REQUIRE(t2.read(true, true, scratch) == t2.frozen.v); // nothing was written either
}


TEST_CASE("case 7: the EVENT kill switch is the CALLER's, and never freezes live values (#1488)", "[oxr][views_change]")
{
	VC t;
	XrViewConfigurationView scratch[XRT_MAX_VIEWS];

	// DXR_VIEWS_CHANGE_EVENT must NOT be folded into ext_enabled. update()
	// reports the doorbell unconditionally; the fire site decides whether to
	// emit it. So with EVENT off (simulated by the caller simply ignoring the
	// return value) the shadow still moves and the next enumerate still
	// answers with the new size.
	REQUIRE(t.feed(1280, 720, 3 * kSec) == true); // caller decides what to do with this

	const XrViewConfigurationView *got = t.read(/*ext*/ true, /*live*/ true, scratch);
	REQUIRE(got == scratch);
	REQUIRE(got[0].recommendedImageRectWidth == 1280);
	REQUIRE(got[0].recommendedImageRectHeight == 720);

	// The doorbell was authorised and CONSUMED even though the caller did not
	// emit it: no backlog accumulates, so flipping EVENT back on cannot make
	// suppressed changes fire in a burst.
	REQUIRE(t.feed(1280, 720, 30 * kSec) == false);

	// The churn counters the soak line prints. emitted is 1-based and advances
	// once per authorised doorbell - that is what a run counts events/min from.
	REQUIRE(t.stats.edges == 1);
	REQUIRE(t.stats.emitted == 1);
	REQUIRE(t.stats.suppressed == 0);
	REQUIRE(t.feed(1600, 900, 60 * kSec) == true);
	REQUIRE(t.stats.edges == 2);
	REQUIRE(t.stats.emitted == 2);
	REQUIRE(t.stats.suppressed == 0);
	REQUIRE(t.stats.last_w == 1600);
	REQUIRE(t.stats.last_h == 900);

	// LIVE off IS legitimately folded into ext_enabled, and that one does
	// freeze both halves: no write, no doorbell.
	VC t2;
	REQUIRE(t2.feed(1280, 720, 3 * kSec, /*ext_enabled*/ false) == false);
	REQUIRE(t2.read(true, true, scratch) == t2.frozen.v);
}

TEST_CASE("case 8: the FIRST frame can already be a change (#1488)", "[oxr][views_change]")
{
	// The edge detector is seeded from the frozen snapshot, not from the
	// first sample. The reference value the app holds IS the snapshot, so a
	// first frame that already differs from it is a change the app must hear
	// about - otherwise enumerate stays stale until the NEXT change, which
	// may never come.
	SECTION("first frame equal to the snapshot rings nothing")
	{
		VC t;
		XrViewConfigurationView scratch[XRT_MAX_VIEWS];
		// The VC fixture seeds at the Frozen defaults, 800x600.
		REQUIRE(t.feed(800, 600, 5 * kSec) == false);
		REQUIRE(t.read(true, true, scratch) == t.frozen.v);
	}

	SECTION("first frame differing from the snapshot rings, and live moves")
	{
		VC t;
		XrViewConfigurationView scratch[XRT_MAX_VIEWS];
		REQUIRE(t.feed(900, 600, 5 * kSec) == true);

		const XrViewConfigurationView *got = t.read(true, true, scratch);
		REQUIRE(got == scratch);
		REQUIRE(got[0].recommendedImageRectWidth == 900);
		REQUIRE(got[0].recommendedImageRectHeight == 600);
		require_immutable_fields_match(got[0], t.frozen.v[0]);
	}
}


TEST_CASE("case 9: recommended* is clamped to max*, and the ceiling ends the edges (#1488)", "[oxr][views_change]")
{
	VC t;
	XrViewConfigurationView scratch[XRT_MAX_VIEWS];

	// The frozen fixture caps at 4096; narrow it so the clamp is reachable.
	for (uint32_t i = 0; i < XRT_MAX_VIEWS; i++) {
		t.frozen.v[i].maxImageRectWidth = 1920;
		t.frozen.v[i].maxImageRectHeight = 1080;
	}

	// The compositor getters return window/canvas x view_scale with NO
	// ceiling, so an oversized window (DPI virtualisation, a multi-monitor
	// span) can hand us dims above max. Publishing those would put
	// recommended > max, which the spec forbids.
	REQUIRE(t.feed(2500, 1400, 5 * kSec) == true);

	const XrViewConfigurationView *got = t.read(true, true, scratch);
	REQUIRE(got == scratch);
	for (uint32_t i = 0; i < kViewCount; i++) {
		REQUIRE(got[i].recommendedImageRectWidth == 1920);
		REQUIRE(got[i].recommendedImageRectHeight == 1080);
		REQUIRE(got[i].recommendedImageRectWidth <= got[i].maxImageRectWidth);
		REQUIRE(got[i].recommendedImageRectHeight <= got[i].maxImageRectHeight);
		// The ceiling itself is untouched, as always.
		REQUIRE(got[i].maxImageRectWidth == 1920);
		REQUIRE(got[i].maxImageRectHeight == 1080);
	}

	// Growing FURTHER past the ceiling is not a change: edge detection runs on
	// the clamped value, so a drag that keeps enlarging an already-oversized
	// window rings nothing and adds no edges.
	REQUIRE(t.feed(2600, 1400, 60 * kSec) == false);
	REQUIRE(t.feed(4000, 3000, 120 * kSec) == false);
	REQUIRE(t.stats.edges == 1);
	REQUIRE(t.stats.emitted == 1);

	// Coming back under the ceiling is a change again.
	REQUIRE(t.feed(1280, 720, 180 * kSec) == true);
	REQUIRE(t.read(true, true, scratch)[0].recommendedImageRectWidth == 1280);
}


/*
 *
 * Integration half: registration + the pre-change gate, through the public API.
 *
 */

#if defined(DXR_RUNTIME_LIB_PATH)
namespace {

struct Runtime
{
	PFN_xrGetInstanceProcAddr gipa = nullptr;
	XrInstance instance = XR_NULL_HANDLE;

	template <typename T>
	T
	fn(const char *name)
	{
		PFN_xrVoidFunction f = nullptr;
		if (XR_FAILED(gipa(instance, name, &f))) {
			return nullptr;
		}
		return reinterpret_cast<T>(f);
	}
	~Runtime()
	{
		if (instance != XR_NULL_HANDLE) {
			auto destroy = fn<PFN_xrDestroyInstance>("xrDestroyInstance");
			if (destroy != nullptr) {
				destroy(instance);
			}
		}
	}
};

bool
load_gipa(Runtime &rt)
{
	const char *path = DXR_RUNTIME_LIB_PATH;
	PFN_xrNegotiateLoaderRuntimeInterface negotiate = nullptr;
#if defined(_WIN32)
	HMODULE mod = LoadLibraryA(path);
	if (mod == nullptr) {
		return false;
	}
	negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
	    GetProcAddress(mod, "xrNegotiateLoaderRuntimeInterface"));
#else
	void *mod = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (mod == nullptr) {
		return false;
	}
	negotiate =
	    reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(dlsym(mod, "xrNegotiateLoaderRuntimeInterface"));
#endif
	if (negotiate == nullptr) {
		return false;
	}

	XrNegotiateLoaderInfo li = {};
	li.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
	li.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
	li.structSize = sizeof(li);
	li.minInterfaceVersion = 1;
	li.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
	li.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
	li.maxApiVersion = XR_MAKE_VERSION(1, 1, 999);

	XrNegotiateRuntimeRequest req = {};
	req.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
	req.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
	req.structSize = sizeof(req);

	if (XR_FAILED(negotiate(&li, &req)) || req.getInstanceProcAddr == nullptr) {
		return false;
	}
	rt.gipa = req.getInstanceProcAddr;
	return true;
}

} // namespace

TEST_CASE("integration: the extension is advertised and enabling it changes nothing yet (#1488)",
          "[oxr][views_change][integration]")
{
	Runtime probe;
	if (!load_gipa(probe)) {
		SKIP("runtime library not loadable: " DXR_RUNTIME_LIB_PATH);
		return;
	}

	// (a) advertised at specVersion 1.
	auto enumExt = probe.fn<PFN_xrEnumerateInstanceExtensionProperties>("xrEnumerateInstanceExtensionProperties");
	REQUIRE(enumExt != nullptr);
	uint32_t n = 0;
	REQUIRE(XR_SUCCEEDED(enumExt(nullptr, 0, &n, nullptr)));
	std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
	REQUIRE(XR_SUCCEEDED(enumExt(nullptr, n, &n, props.data())));

	const XrExtensionProperties *found = nullptr;
	for (const auto &p : props) {
		if (std::strcmp(p.extensionName, XR_EXT_VIEW_CONFIGURATION_VIEWS_CHANGE_EXTENSION_NAME) == 0) {
			found = &p;
		}
	}
	REQUIRE(found != nullptr);
	REQUIRE(found->extensionVersion == XR_EXT_view_configuration_views_change_SPEC_VERSION);

	// (b) an instance can enable it, and before any change has happened it
	//     enumerates exactly what a plain instance does.
	auto bring_up = [&](Runtime &rt, bool with_ext) -> bool {
		if (!load_gipa(rt)) {
			return false;
		}
		std::vector<const char *> exts;
		if (with_ext) {
			exts.push_back(XR_EXT_VIEW_CONFIGURATION_VIEWS_CHANGE_EXTENSION_NAME);
		}
		XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
		std::snprintf(ici.applicationInfo.applicationName, sizeof(ici.applicationInfo.applicationName),
		              "tests_oxr_view_config_views_change");
		ici.applicationInfo.applicationVersion = 1;
		std::snprintf(ici.applicationInfo.engineName, sizeof(ici.applicationInfo.engineName), "catch2");
		ici.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
		ici.enabledExtensionCount = (uint32_t)exts.size();
		ici.enabledExtensionNames = exts.empty() ? nullptr : exts.data();
		auto createInstance = rt.fn<PFN_xrCreateInstance>("xrCreateInstance");
		return XR_SUCCEEDED(createInstance(&ici, &rt.instance));
	};

	auto enumerate = [&](Runtime &rt, std::vector<XrViewConfigurationView> &out) -> bool {
		XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
		sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
		XrSystemId sys = XR_NULL_SYSTEM_ID;
		auto getSystem = rt.fn<PFN_xrGetSystem>("xrGetSystem");
		if (XR_FAILED(getSystem(rt.instance, &sgi, &sys))) {
			return false;
		}
		auto enumViews = rt.fn<PFN_xrEnumerateViewConfigurationViews>("xrEnumerateViewConfigurationViews");
		uint32_t c = 0;
		if (XR_FAILED(enumViews(rt.instance, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &c, nullptr))) {
			return false;
		}
		out.assign(c, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
		return XR_SUCCEEDED(
		    enumViews(rt.instance, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, c, &c, out.data()));
	};

	Runtime plain;
	if (!bring_up(plain, false)) {
		// No display processor registered - the environment, not the code.
		SKIP(
		    "xrCreateInstance failed - no display processor registered? "
		    "Register the sim-display plug-in to run this case.");
		return;
	}
	Runtime with_ext;
	REQUIRE(bring_up(with_ext, true)); // enabling the EXT must succeed

	std::vector<XrViewConfigurationView> a, b;
	REQUIRE(enumerate(plain, a));
	REQUIRE(enumerate(with_ext, b));

	REQUIRE(a.size() == b.size()); // the view COUNT never moves
	REQUIRE(!a.empty());
	for (size_t i = 0; i < a.size(); i++) {
		// No change has happened, so views_change.valid is false and the
		// EXT-enabled instance must still answer from the snapshot.
		REQUIRE(a[i].recommendedImageRectWidth == b[i].recommendedImageRectWidth);
		REQUIRE(a[i].recommendedImageRectHeight == b[i].recommendedImageRectHeight);
		REQUIRE(a[i].maxImageRectWidth == b[i].maxImageRectWidth);
		REQUIRE(a[i].maxImageRectHeight == b[i].maxImageRectHeight);
		REQUIRE(a[i].recommendedSwapchainSampleCount == b[i].recommendedSwapchainSampleCount);
		REQUIRE(a[i].maxSwapchainSampleCount == b[i].maxSwapchainSampleCount);
	}
}
#endif // DXR_RUNTIME_LIB_PATH
