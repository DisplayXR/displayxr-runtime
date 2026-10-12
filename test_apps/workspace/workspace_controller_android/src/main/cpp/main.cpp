// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Minimal Android workspace controller (#1006, ADR-036 D4).
 *
 * What it does, and nothing more:
 *  1. xrCreateInstance with XR_DXR_spatial_workspace → the runtime sees a
 *     CONTROLLER claim and verifies the package (manifest meta-data + the
 *     runtime's signing certificate). The manifest forces IPC, and the slot
 *     broker routes a verified controller to the MAIN-process service; the
 *     runtime's IPC client publishes this NativeActivity's fullscreen surface,
 *     which becomes the shared output in overlay mode.
 *  2. xrCreateSession with no graphics binding (controllers never render),
 *     then xrActivateSpatialWorkspaceDXR → the overlay mode turns on.
 *  3. Twice a second: enumerate the workspace's clients and place them side by
 *     side across the panel (xrSetWorkspaceClientWindowPoseDXR).
 *
 * Test knobs:
 *  - `adb shell setprop debug.dxr.test_controller.active 0` deactivates the
 *    overlay (per-window resumes), `1` (or unset) re-activates it.
 *  - `adb shell setprop debug.dxr.test_controller.focus N` focuses the N-th
 *    placed client (1-based; 0 or unset = no focus). The focused client owns
 * the shared display processor's 2D/3D mode (#967 D-5).
 *  - `adb shell setprop debug.dxr.test_controller.mode N` requests rendering
 *    mode N as the workspace's display-mode authority (0 = 2D, 1 = 3D on Leia),
 *    which drives the shared display processor's lens (#967 D-5).
 */

#include <android/log.h>
#include <android_native_app_glue.h>
#include <sys/system_properties.h>

#include <jni.h>

#define XR_USE_PLATFORM_ANDROID
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_DXR_display_info.h>
#include <openxr/XR_DXR_spatial_workspace.h>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#define TAG "dxr-ws-controller"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace {

struct Controller
{
	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId system = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	bool active = false;
	float disp_w_m = 0.0f;
	float disp_h_m = 0.0f;

	PFN_xrActivateSpatialWorkspaceDXR activate = nullptr;
	PFN_xrDeactivateSpatialWorkspaceDXR deactivate = nullptr;
	PFN_xrEnumerateWorkspaceClientsDXR enumerate = nullptr;
	PFN_xrGetWorkspaceClientInfoDXR info = nullptr;
	PFN_xrSetWorkspaceClientWindowPoseDXR set_pose = nullptr;
        PFN_xrSetWorkspaceFocusedClientDXR set_focus = nullptr;
        PFN_xrRequestDisplayRenderingModeDXR request_mode = nullptr;
        int focus_applied = -1;
        int mode_applied = -1;

        std::vector<XrWorkspaceClientId> placed;
};

bool g_window_ready = false;

void
handle_cmd(struct android_app *app, int32_t cmd)
{
	(void)app;
	switch (cmd) {
	case APP_CMD_INIT_WINDOW: g_window_ready = true; break;
	case APP_CMD_TERM_WINDOW: g_window_ready = false; break;
	default: break;
	}
}

int64_t
now_ms()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int want_focus() {
  char v[PROP_VALUE_MAX] = {};
  if (__system_property_get("debug.dxr.test_controller.focus", v) > 0) {
    return atoi(v);
  }
  return 0;
}

bool
want_active()
{
	char v[PROP_VALUE_MAX] = {};
	if (__system_property_get("debug.dxr.test_controller.active", v) > 0 && v[0] == '0') {
		return false;
	}
	return true;
}

template <typename T>
bool
get_proc(XrInstance inst, const char *name, T *out)
{
	XrResult r = xrGetInstanceProcAddr(inst, name, reinterpret_cast<PFN_xrVoidFunction *>(out));
	if (r != XR_SUCCESS || *out == nullptr) {
		LOGE("xrGetInstanceProcAddr(%s) failed: %d", name, (int)r);
		return false;
	}
	return true;
}

bool
init(struct android_app *app, Controller &c)
{
	PFN_xrInitializeLoaderKHR init_loader = nullptr;
	if (!get_proc(XR_NULL_HANDLE, "xrInitializeLoaderKHR", &init_loader)) {
		return false;
	}
	XrLoaderInitInfoAndroidKHR li = {XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
	li.applicationVM = app->activity->vm;
	li.applicationContext = app->activity->clazz;
	if (init_loader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&li)) != XR_SUCCESS) {
		LOGE("xrInitializeLoaderKHR failed");
		return false;
	}

	const char *exts[] = {
	    XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
	    XR_DXR_SPATIAL_WORKSPACE_EXTENSION_NAME,
	    XR_DXR_DISPLAY_INFO_EXTENSION_NAME,
	};
	XrInstanceCreateInfoAndroidKHR ai = {XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
	ai.applicationVM = app->activity->vm;
	ai.applicationActivity = app->activity->clazz;
	XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
	ci.next = &ai;
	std::strncpy(ci.applicationInfo.applicationName, "workspace_controller_android",
	             XR_MAX_APPLICATION_NAME_SIZE - 1);
	ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	ci.enabledExtensionCount = sizeof(exts) / sizeof(exts[0]);
	ci.enabledExtensionNames = exts;
	XrResult r = xrCreateInstance(&ci, &c.instance);
	if (r != XR_SUCCESS) {
		LOGE("xrCreateInstance failed: %d", (int)r);
		return false;
	}

	XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if (xrGetSystem(c.instance, &sgi, &c.system) != XR_SUCCESS) {
		LOGE("xrGetSystem failed");
		return false;
	}
	XrDisplayInfoDXR di = {XR_TYPE_DISPLAY_INFO_DXR};
	XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
	sp.next = &di;
	if (xrGetSystemProperties(c.instance, c.system, &sp) == XR_SUCCESS) {
		c.disp_w_m = di.displaySizeMeters.width;
		c.disp_h_m = di.displaySizeMeters.height;
	}
	if (c.disp_w_m <= 0.0f || c.disp_h_m <= 0.0f) {
		c.disp_w_m = 0.17f;
		c.disp_h_m = 0.27f;
	}
	LOGI("display %.3f x %.3f m", (double)c.disp_w_m, (double)c.disp_h_m);

        if (!get_proc(c.instance, "xrActivateSpatialWorkspaceDXR",
                      &c.activate) ||
            !get_proc(c.instance, "xrDeactivateSpatialWorkspaceDXR",
                      &c.deactivate) ||
            !get_proc(c.instance, "xrEnumerateWorkspaceClientsDXR",
                      &c.enumerate) ||
            !get_proc(c.instance, "xrGetWorkspaceClientInfoDXR", &c.info) ||
            !get_proc(c.instance, "xrSetWorkspaceClientWindowPoseDXR",
                      &c.set_pose) ||
            !get_proc(c.instance, "xrSetWorkspaceFocusedClientDXR",
                      &c.set_focus) ||
            !get_proc(c.instance, "xrRequestDisplayRenderingModeDXR",
                      &c.request_mode)) {
          return false;
        }

        // A controller session carries no graphics binding (XR_DXR_spatial_workspace).
	XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
	sci.systemId = c.system;
	r = xrCreateSession(c.instance, &sci, &c.session);
	if (r != XR_SUCCESS) {
		LOGE("xrCreateSession (controller, no graphics binding) failed: %d", (int)r);
		return false;
	}
	LOGI("controller session created");
	return true;
}

void
set_active(Controller &c, bool on)
{
	if (on == c.active) {
		return;
	}
	XrResult r = on ? c.activate(c.session) : c.deactivate(c.session);
	LOGW("%s -> %d", on ? "xrActivateSpatialWorkspaceDXR" : "xrDeactivateSpatialWorkspaceDXR", (int)r);
	if (r == XR_SUCCESS) {
		c.active = on;
		c.placed.clear();
	}
}

// Lay every OpenXR client out side by side across the panel, each a column of
// the panel's width, 90% of its height (display-centric metres, +y up).
void place_clients(Controller &c, struct android_app *app) {
  // XR_DXR_display_info reports the panel in the vendor's natural orientation;
  // our fullscreen window shows how it is held right now (the NP02J is
  // portrait-native), so lay out across whichever edge is horizontal.
  // The window is gone while a client's Activity sits on top of ours, so
  // remember the last orientation we saw.
  static int held_portrait = -1;
  if (app->window != nullptr) {
    const int32_t ww = ANativeWindow_getWidth(app->window);
    const int32_t wh = ANativeWindow_getHeight(app->window);
    if (ww > 0 && wh > 0) {
      held_portrait = wh > ww ? 1 : 0;
    }
  }
  float pw = c.disp_w_m, ph = c.disp_h_m;
  if (held_portrait >= 0 && ((held_portrait == 1) != (ph > pw))) {
    const float t = pw;
    pw = ph;
    ph = t;
  }
  uint32_t n = 0;
  if (c.enumerate(c.session, 0, &n, nullptr) != XR_SUCCESS) {
    return;
  }
  std::vector<XrWorkspaceClientId> ids(n);
  if (n > 0 && c.enumerate(c.session, n, &n, ids.data()) != XR_SUCCESS) {
    return;
  }
  std::vector<XrWorkspaceClientId> apps;
  for (uint32_t i = 0; i < n; i++) {
    XrWorkspaceClientInfoDXR ci = {XR_TYPE_WORKSPACE_CLIENT_INFO_DXR};
    if (c.info(c.session, ids[i], &ci) == XR_SUCCESS &&
        ci.clientType == XR_WORKSPACE_CLIENT_TYPE_OPENXR_3D_DXR) {
      apps.push_back(ids[i]);
    }
  }
  if (apps == c.placed) {
    return;
  }
  const uint32_t count = (uint32_t)apps.size();
  const float col_w = pw / (float)(count > 0 ? count : 1);
  bool all_ok = true;
  for (uint32_t i = 0; i < count; i++) {
    XrPosef pose = {};
    pose.orientation.w = 1.0f;
    pose.position.x = -pw * 0.5f + col_w * ((float)i + 0.5f);
    pose.position.y = 0.0f;
    pose.position.z = 0.0f;
    XrResult r =
        c.set_pose(c.session, apps[i], &pose, col_w * 0.96f, ph * 0.9f);
    LOGW("place client %u (%u of %u) at x=%.3f w=%.3f h=%.3f -> %d", apps[i],
         i + 1, count, (double)pose.position.x, (double)(col_w * 0.96f),
         (double)(ph * 0.9f), (int)r);
    all_ok = all_ok && r == XR_SUCCESS;
  }
  if (!all_ok) {
    return; // a client not ready to be placed yet: retry next tick
  }
  c.placed = apps;
  c.focus_applied = -1; // re-apply focus against the new layout
}

void apply_mode(Controller &c) {
  char v[PROP_VALUE_MAX] = {};
  if (__system_property_get("debug.dxr.test_controller.mode", v) <= 0 ||
      v[0] < '0' || v[0] > '9') {
    return;
  }
  const int want = atoi(v);
  if (want == c.mode_applied) {
    return;
  }
  XrResult r = c.request_mode(c.session, (uint32_t)want);
  LOGW("request rendering mode %d (workspace display-mode authority) -> %d",
       want, (int)r);
  c.mode_applied = want;
}

void apply_focus(Controller &c) {
  const int want = want_focus();
  if (want == c.focus_applied) {
    return;
  }
  XrWorkspaceClientId id = XR_NULL_WORKSPACE_CLIENT_ID;
  if (want >= 1 && (size_t)want <= c.placed.size()) {
    id = c.placed[(size_t)want - 1];
  }
  XrResult r = c.set_focus(c.session, id);
  LOGW("focus client #%d (id %u) -> %d", want, id, (int)r);
  c.focus_applied = want;
}

void
poll_xr_events(Controller &c)
{
	XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
	while (xrPollEvent(c.instance, &ev) == XR_SUCCESS) {
		ev = {XR_TYPE_EVENT_DATA_BUFFER};
	}
}

} // namespace

void
android_main(struct android_app *app)
{
	app->onAppCmd = handle_cmd;
	LOGI("android_main entered");

	// Wait for the window: the runtime's IPC client attaches its SurfaceView to
	// this Activity at connect, and that surface is the overlay's output.
	while (!g_window_ready && !app->destroyRequested) {
		int events = 0;
		struct android_poll_source *source = nullptr;
		if (ALooper_pollOnce(100, nullptr, &events, (void **)&source) >= 0 && source != nullptr) {
			source->process(app, source);
		}
	}

	Controller c;
	bool ok = !app->destroyRequested && init(app, c);
	int64_t next_tick = 0;
	while (!app->destroyRequested) {
		int events = 0;
		struct android_poll_source *source = nullptr;
		if (ALooper_pollOnce(50, nullptr, &events, (void **)&source) >= 0 && source != nullptr) {
			source->process(app, source);
		}
		if (!ok) {
			continue;
		}
		poll_xr_events(c);
		const int64_t t = now_ms();
		if (t >= next_tick) {
			next_tick = t + 500;
			set_active(c, want_active());
			if (c.active) {
                          place_clients(c, app);
                          apply_focus(c);
                          apply_mode(c);
                        }
		}
	}

	if (ok) {
		set_active(c, false);
		xrDestroySession(c.session);
		xrDestroyInstance(c.instance);
	}
	LOGI("android_main exiting");
}
