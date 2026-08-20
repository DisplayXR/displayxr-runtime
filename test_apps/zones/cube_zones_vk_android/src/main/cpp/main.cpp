// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
//
// cube_zones_vk_android — the Android XR_DXR_display_zones test app.
//
// A clone of cube_handle_vk_android (same class: _handle via
// XR_DXR_android_surface_binding; same API: Vulkan; same NativeActivity shell)
// with the zones chain added, existing to answer ONE question on hardware:
//
//     is the PER-ZONE WEAVE PHASE right on Android?
//
// It submits a zones frame every frame:
//   * one 3D zone (id 1) at x = canvas_w/3, y = canvas_h/4, size w/2 x h/2 —
//     deliberately offset on BOTH axes and NOT at the window origin, because a
//     zone at 0,0 cannot distinguish "the offset is applied" from "the offset
//     is ignored";
//   * one Local2D zone: a full-width bar-ruler band across the top.
// Wish mode is AUTO (the runtime derives the hardware wish from the zone rect,
// ADR-027 Decision 1); VALIDATE is on by default.
//
// THE PHASE CHAIN TO CHECK, END TO END. Each stage prints one WARN/INFO on
// change (never per frame), and the three numbers must agree:
//
//   app     [zones] frame layout: canvas=WxH | 3D zone id=1 rect=ZX,ZY WxH
//   runtime ZONES IPC: first zone-scoped locate, rect=(ZX,ZY WxH) ...
//   runtime [per-session] #568 render layer = ZONE_3D id=1 rect=ZX,ZY WxH
//   runtime HW_GEO: ... canvas=ZX,ZY WxH subrect=1 (#53 zone phase)
//   plugin  HW_DBG_CNSDK: window screen rect WX,WY ...          (#150/#1033)
//   plugin  HW_DBG_CNSDK: weave band ZX,ZY WxH screen-pos SX,SY (#53)
//
//   INVARIANT:  SX == WX + ZX  and  SY == WY + ZY
//
// i.e. the interlace phase is referenced to the zone's origin ON THE PANEL =
// window origin + zone offset. Fullscreen WX,WY is 0,0 so the window half is
// invisible; run it FREEFORM at an offset to exercise both halves:
//
//   adb shell settings put global enable_freeform_support 1   # once
//   adb root
//   PKG=com.displayxr.cube_zones_vk_android
//   adb shell "am start -n $PKG/.MainActivity --windowingMode 5;
//              am start -n $PKG/.MainActivity --windowingMode 5;
//              while :; do p=$(pidof $PKG); [ -n "$p" ] && { kill -STOP $p; break; }; done"
//   TID=<task id from: adb shell dumpsys activity activities | grep $PKG>
//   adb shell am task resize $TID 300 500 1300 2000
//   adb shell "kill -CONT $(pidof $PKG)"
//
// (The SIGSTOP staging is the same trick scripts/android-sidebyside.sh uses:
// it makes the resize provably precede surface creation, so the OpenXR session
// is built against the FINAL surface — see #1040.)
//
// KNOWN RUNTIME LIMIT, deliberately not exercised here: the Android
// out-of-process per-session render path takes the FIRST projection / ZONE_3D
// layer of the frame and ignores the rest (comp_multi_system.c,
// render_session_to_own_target), so exactly ONE 3D zone is composited on
// Android however many the app submits — even though the runtime advertises
// maxZones3D = 32. Hence one 3D zone + one Local2D here, which is also the
// shape displayxr-demo-avatar ships.

#include <android/log.h>
#include <android_native_app_glue.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
// openxr_platform.h references VkInstance / VkDevice / VkFormat under
// XR_USE_GRAPHICS_API_VULKAN but does NOT include <vulkan/vulkan.h>
// itself — the consumer must do that first.
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
// XR_DXR_display_info: display dimensions + the rendering-mode enumeration /
// switching entry points. Drives the adaptive tiled-atlas multiview port (#499).
#include <openxr/XR_DXR_display_info.h>
// XR_DXR_view_rig: the runtime owns the Kooima/off-axis projection. The app
// chains a display-centric XrDisplayRigDXR on xrLocateViews and consumes the
// render-ready XrView{pose, fov} — no app-side projection or per-orientation
// compensation (device rotation is the weaver/DP's job, per the CNSDK model).
#include <openxr/XR_DXR_view_rig.h>

// XR_DXR_display_zones (ADR-027) + XR_DXR_local_3d_zone: this app's reason to
// exist. One 3D zone (deliberately NOT at the window origin) plus one Local2D
// zone, so a per-zone weave-PHASE error on Android is visible as a doubled /
// shifted cube inside the band while the rest of the window is untouched.
#include <openxr/XR_DXR_local_3d_zone.h>
#include <openxr/XR_DXR_display_zones.h>

// XR_DXR_android_surface_binding (#1037, ADR-036 D2/D6): this app owns its
// Surface and hands it to the runtime, instead of letting the runtime spawn a
// SurfaceView of its own. The runtime-spawned view has no ViewParent, so it
// crashes the app the moment the task lands in a freeform / split-screen
// container — which is precisely the multi-app case this test app exists for.
#if __has_include(<openxr/XR_DXR_android_surface_binding.h>)
#define CUBE_HAVE_ANDROID_SURFACE_BINDING 1
#include <openxr/XR_DXR_android_surface_binding.h>
#endif

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <jni.h>
#include <sys/system_properties.h>
#include <unistd.h>

// SPIR-V headers generated by the spirv_shaders() CMake helper at build
// time. They expose `shaders_cube_vert` / `shaders_cube_frag` as
// `unsigned int [...]` arrays (host-endian SPIR-V words).
#include "shaders/cube.vert.h"
#include "shaders/cube.frag.h"

// The Wood_Crate textured cube + floor grid (ported from cube_handle_vk_win).
#include <android/asset_manager.h>
#include "crate_scene.h"
#include "hud_font.h"

#define LOG_TAG "cube_zones_vk_android"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Hardware-bring-up verbose debug. Gated on XRT_DEBUG_ANDROID_VERBOSE
// passed from build.gradle's debug variant. Compiled out in release.
// Tag "HW_DBG_APP:" greppable separation from runtime-side HW_DBG_CNSDK
// / HW_DBG_DP logs.
#ifdef XRT_DEBUG_ANDROID_VERBOSE
#define DXR_HW_DBG(...)       __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "HW_DBG_APP: " __VA_ARGS__)
#define DXR_HW_DBG_ONCE(...)  do {                                                                 \
		static bool _logged = false;                                                                \
		if (!_logged) {                                                                             \
			__android_log_print(ANDROID_LOG_INFO, LOG_TAG, "HW_DBG_APP[once]: " __VA_ARGS__);       \
			_logged = true;                                                                         \
		}                                                                                           \
	} while (0)
#else
#define DXR_HW_DBG(...)       ((void)0)
#define DXR_HW_DBG_ONCE(...)  ((void)0)
#endif

namespace {

const char *
xr_result_str(XrResult r)
{
	// Just the codes we expect to see; anything else falls through to a
	// numeric format in the caller.
	switch (r) {
	case XR_SUCCESS:                            return "XR_SUCCESS";
	case XR_ERROR_RUNTIME_FAILURE:              return "XR_ERROR_RUNTIME_FAILURE";
	case XR_ERROR_RUNTIME_UNAVAILABLE:          return "XR_ERROR_RUNTIME_UNAVAILABLE";
	case XR_ERROR_INSTANCE_LOST:                return "XR_ERROR_INSTANCE_LOST";
	case XR_ERROR_INITIALIZATION_FAILED:        return "XR_ERROR_INITIALIZATION_FAILED";
	case XR_ERROR_API_VERSION_UNSUPPORTED:      return "XR_ERROR_API_VERSION_UNSUPPORTED";
	case XR_ERROR_EXTENSION_NOT_PRESENT:        return "XR_ERROR_EXTENSION_NOT_PRESENT";
	case XR_ERROR_API_LAYER_NOT_PRESENT:        return "XR_ERROR_API_LAYER_NOT_PRESENT";
	case XR_ERROR_OUT_OF_MEMORY:                return "XR_ERROR_OUT_OF_MEMORY";
	case XR_ERROR_FUNCTION_UNSUPPORTED:         return "XR_ERROR_FUNCTION_UNSUPPORTED";
	case XR_ERROR_VALIDATION_FAILURE:           return "XR_ERROR_VALIDATION_FAILURE";
	default:                                    return nullptr;
	}
}

void
log_xr_result(const char *what, XrResult r)
{
	const char *name = xr_result_str(r);
	if (name != nullptr) {
		LOGI("%s -> %s", what, name);
	} else {
		LOGI("%s -> XrResult(%d)", what, (int)r);
	}
}

XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system_id = XR_NULL_SYSTEM_ID;
XrVersion g_required_vk_version = XR_MAKE_VERSION(1, 1, 0);

VkInstance g_vk_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_vk_phys_device = VK_NULL_HANDLE;
VkDevice g_vk_device = VK_NULL_HANDLE;
VkQueue g_vk_queue = VK_NULL_HANDLE;
uint32_t g_vk_queue_family = UINT32_MAX;

XrSession g_session = XR_NULL_HANDLE;
XrSessionState g_session_state = XR_SESSION_STATE_UNKNOWN;
bool g_session_running = false;
bool g_exit_requested = false;

// Reference space for the projection layer. STAGE if available, else LOCAL.
XrSpace g_app_space = XR_NULL_HANDLE;

// ─── Adaptive tiled-atlas multiview model (#499) ──────────────────────────
// DisplayXR exposes a tiled-atlas multiview model exactly like the Windows /
// macOS cubes: the runtime advertises up to `g_max_view_count` views (the max
// across every rendering mode) via xrEnumerateViewConfigurationViews, and the
// app renders the *active* mode's `view_count` views into TILES of a SINGLE
// atlas swapchain. The projection layer submits N projection views that all
// reference that one swapchain, each with a per-tile imageRect.
//
// The previous build hard-coded stereo (kViewCount = 2, one swapchain per
// view) and bailed in create_swapchains() the moment the runtime reported a
// different count — sim_display advertises 4 (max across its 2D / Anaglyph /
// Cropped-SBS / Squeezed-SBS / Quad modes), so create failed, g_app_space
// stayed NULL, xrLocateViews busy-looped, and nothing was ever presented.
// That was the Android black screen. This file now mirrors cube_handle_vk_win.
constexpr uint32_t kMaxViews = 8;

// One advertised rendering mode (mirror of XrDisplayRenderingModeInfoDXR).
struct RenderingModeInfo
{
	uint32_t view_count{2};
	uint32_t tile_columns{2};
	uint32_t tile_rows{1};
	float view_scale_x{0.5f};
	float view_scale_y{1.0f};
	bool hardware_3d{true};
	bool requestable{true};
	char name[64]{"Stereo"};
};
RenderingModeInfo g_modes[kMaxViews] = {};
uint32_t g_mode_count = 0;            // 0 → no XR_DXR_display_info; default stereo
std::atomic<uint32_t> g_current_mode{0};
uint32_t g_max_view_count = 2;        // xrEnumerateViewConfigurationViews → locate capacity
uint32_t g_display_px_w = 0;          // native panel pixels (XR_DXR_display_info)
uint32_t g_display_px_h = 0;
bool g_has_display_info = false;
bool g_has_view_rig = false;          // XR_DXR_view_rig: runtime-owned Kooima projection

#ifdef CUBE_HAVE_ANDROID_SURFACE_BINDING
// XR_DXR_android_surface_binding (#1037). Advertised → we hand the runtime our
// OWN ANativeWindow at xrCreateSession and republish it across the surface's
// destroy/recreate cycle, so no runtime-spawned SurfaceView is ever created
// (it has no ViewParent and dies in freeform).
bool g_has_surface_binding = false;
PFN_xrSetAndroidSurfaceDXR g_pfnSetAndroidSurface = nullptr;
PFN_xrSetAndroidWindowGeometryDXR g_pfnSetAndroidWindowGeometry = nullptr;
// The window this app currently owns; the android_main thread publishes it and
// the frame loop consumes it, hence the atomic.
std::atomic<ANativeWindow *> g_app_window{nullptr};

// Live window geometry, sampled on the UI thread by MainActivity's Choreographer
// callback (Android reports a pure window MOVE to nobody else) and consumed once
// per frame by the render loop. Packed into one seq-guarded snapshot so the
// reader never mixes halves of two samples.
struct WindowRectSample
{
	int32_t x = 0, y = 0;
	int32_t w = 0, h = 0;
	int32_t panel_w = 0, panel_h = 0;
	int32_t display_id = 0;
};
std::atomic<uint64_t> g_win_rect_seq{0};   // bumped by the UI thread on any change
WindowRectSample g_win_rect;                // guarded by the seq above (single writer)
#endif

// Display-centric rig defaults (match cube_handle_vk_win). virtualDisplayHeight
// in app units — 0.24 = 4x the 0.06 m cube; all factors at their neutral 1.0.
constexpr float kRigVirtualDisplayHeight = 0.24f;

// Drag-orbit camera: single-finger drag orbits the viewer around the scene.
// Stored as yaw/pitch (radians); the rig pose is built from these each frame.
// Default 0 → identity rig (viewer square-on to the virtual display plane).
std::atomic<float> g_cam_yaw{0.0f};
std::atomic<float> g_cam_pitch{0.0f};

// True while a finger is down — pauses the cube's auto-spin so a drag-orbit
// isn't fighting a moving cube (and so releasing doesn't read as a "jump" as
// the spin resumes). Set from the touch handler (UI thread), read by the
// render loop. g_spin_angle accumulates only when not touching, so the spin
// resumes smoothly from where it paused (no discontinuity).
std::atomic<bool> g_touching{false};
float g_spin_angle = 0.0f;  // render-thread only

// The crate cube + grid scene, and the APK asset manager it loads textures
// from (captured in android_main from app->activity->assetManager).
CrateScene g_scene;

// Crisp antialiased HUD text (stb_truetype atlas). Falls back to the legacy
// bitmap glyphs if init fails (g_hud_font.ready == false).
HudFont g_hud_font;
AAssetManager *g_asset_manager = nullptr;

// XR_DXR_display_info entry points (resolved after the session exists).
PFN_xrEnumerateDisplayRenderingModesDXR g_pfnEnumModes = nullptr;
PFN_xrRequestDisplayRenderingModeDXR g_pfnRequestMode = nullptr;
// #522 test hook: select MANAGED/MANUAL via `setprop debug.dxr.eyemode N`.
PFN_xrRequestEyeTrackingModeDXR g_pfnRequestEyeMode = nullptr;

// Single tiled-atlas swapchain shared by all views. Each view writes into its
// tile via a viewport/scissor offset inside one render pass; xrEndFrame submits
// N projection views that all point at this swapchain with per-tile imageRects.
struct AtlasSwapchain
{
	XrSwapchain swapchain{XR_NULL_HANDLE};
	uint32_t width{0};
	uint32_t height{0};
	// Image arrays sized at xrEnumerateSwapchainImages time. Capped at 8
	// (typical OpenXR runtime budget); more is logged + clamped.
	XrSwapchainImageVulkanKHR images[8]{};
	VkImageView image_views[8]{};
	VkFramebuffer framebuffers[8]{};
	uint32_t image_count{0};
	// One depth target sized to the whole atlas (tiles don't overlap, so a
	// single LOAD_OP_CLEAR depth shared across all tiles is correct).
	VkImage depth_image{VK_NULL_HANDLE};
	VkDeviceMemory depth_mem{VK_NULL_HANDLE};
	VkImageView depth_view{VK_NULL_HANDLE};
};
AtlasSwapchain g_atlas;

// ─── XR_DXR_display_zones state (ADR-027) ─────────────────────────────────
// The 3D zone rect is recomputed every frame from the CURRENTLY HELD canvas
// (the app must not cache it across a rotation), as fractions of the canvas:
//   3D zone   : x = W/3, y = H/4, size W/2 x H/2   (offset on BOTH axes)
//   Local2D   : full-width band across the top, height H/8
// The x-offset of W/3 is deliberately not a round multiple of anything the
// lenticular pitch divides, so a dropped or double-counted zone offset in the
// weave phase shows up as a doubled edge instead of averaging out.
bool g_has_display_zones = false;  //!< XR_DXR_display_zones advertised + enabled
bool g_has_local_3d_zone = false;  //!< XR_DXR_local_3d_zone advertised + enabled
bool g_zones_active = false;       //!< caps queried OK + Local2D swapchain up
PFN_xrGetDisplayZoneCapabilitiesDXR g_pfnGetZoneCaps = nullptr;

// Flat Local2D swapchain (one image set, filled from a staging buffer each
// frame). Small and static — the point is that SOMETHING opaque sits in the 2D
// band, so "the 2D region is transparent" can't be mistaken for a zone bug.
struct Local2DSwapchain
{
	XrSwapchain swapchain{XR_NULL_HANDLE};
	uint32_t width{0};
	uint32_t height{0};
	XrSwapchainImageVulkanKHR images[8]{};
	uint32_t image_count{0};
	VkBuffer staging{VK_NULL_HANDLE};
	VkDeviceMemory staging_mem{VK_NULL_HANDLE};
};
Local2DSwapchain g_local2d;

//! The zone rects for the frame being built, in client-window pixels.
struct ZoneLayout
{
	int32_t canvas_w, canvas_h;
	int32_t z3d_x, z3d_y, z3d_w, z3d_h;
	int32_t l2d_x, l2d_y, l2d_w, l2d_h;
};

// Forward decls used across the rendering-mode helpers.
const RenderingModeInfo &active_mode();
uint32_t active_view_count();
// Forward decls used by the zones block (defined further down).
uint32_t find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags props);
void compute_zone_layout(struct ZoneLayout *zl);
void current_canvas_dims(uint32_t *out_w, uint32_t *out_h);

// ─── on-device UI / eye-tracking readout state ────────────────────────────
// g_bg_enabled: background color on/off. Default OFF → black clear in both
//   eyes, so the weave/3D is judged against a clean black field. Toggled
//   on-device by TAPPING the screen (handle_input) and also exposed to the
//   Kotlin side via JNI (nativeSetBackgroundEnabled).
// g_eye_*: latest tracked head-center position (meters, display-relative),
//   stored each frame from the located view poses. Drives the on-screen
//   eye-position marker and is read by the Kotlin overlay (nativeGetEye).
std::atomic<bool> g_bg_enabled{false};
std::atomic<float> g_eye_x{0.0f};
std::atomic<float> g_eye_y{0.0f};
std::atomic<float> g_eye_z{0.0f};

VkFormat g_swapchain_format = VK_FORMAT_UNDEFINED;
VkCommandPool g_app_cmd_pool = VK_NULL_HANDLE;

// Graphics pipeline state for the triangle renderer.
VkRenderPass g_render_pass = VK_NULL_HANDLE;
VkPipelineLayout g_pipeline_layout = VK_NULL_HANDLE;
VkPipeline g_pipeline = VK_NULL_HANDLE;
VkShaderModule g_vs_module = VK_NULL_HANDLE;
VkShaderModule g_fs_module = VK_NULL_HANDLE;

// Cube geometry in a vertex buffer (position + per-face color). Baking it in
// the shader tripped Adreno's pipeline linker, so it lives in real GPU memory.
VkBuffer g_cube_vbuf = VK_NULL_HANDLE;
VkDeviceMemory g_cube_vbuf_mem = VK_NULL_HANDLE;
struct CubeVertex { float pos[3]; float color[3]; };
constexpr uint32_t kCubeVertexCount = 36;

// In-frame HUD: a persistently-mapped host-visible vertex buffer of CubeVertex
// quads, rebuilt each frame and drawn with an identity MVP (screen-space NDC).
// Android widgets can't overlay the runtime's display window, so the HUD lives
// inside the rendered frame.
VkBuffer g_hud_vbuf = VK_NULL_HANDLE;
VkDeviceMemory g_hud_vbuf_mem = VK_NULL_HANDLE;
void *g_hud_mapped = nullptr;
constexpr uint32_t kHudMaxVerts = 8192;

// Authoritative 4-way display rotation, pushed from MainActivity (Surface
// rotation: 0/1/2/3 = ROTATION_0/90/180/270). Configuration.orientation only
// distinguishes portrait/landscape, so it can't tell the two portraits (0 vs
// 180) apart — that ambiguity made the HUD rotation jump between builds. The
// framebuffer rotates with the device, so the HUD is counter-rotated by this
// to stay upright in every orientation.
std::atomic<int> g_display_rotation{0};

// Set when xrCreateInstance fails with RUNTIME_UNAVAILABLE (the DisplayXR
// runtime isn't reachable — usually because it's in Android's "stopped" state
// and this device's OEM blocks waking it). MainActivity polls this to show the
// user a "launch DisplayXR first" message.
std::atomic<bool> g_runtime_unavailable{false};

// Frame counter for throttled logging — log heartbeat every 60 frames.
uint64_t g_frame_count = 0;

// Wire the Khronos OpenXR loader's Android-specific init. Required on
// Android because the loader needs the JavaVM + Activity context to
// discover the runtime APK via the OpenXRRuntimeService intent.
bool
initialize_loader(struct android_app *app)
{
	PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = nullptr;
	XrResult res = xrGetInstanceProcAddr(
	    XR_NULL_HANDLE, "xrInitializeLoaderKHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&xrInitializeLoaderKHR));
	if (res != XR_SUCCESS || xrInitializeLoaderKHR == nullptr) {
		LOGE("xrGetInstanceProcAddr(xrInitializeLoaderKHR) failed (%d)", (int)res);
		return false;
	}

	XrLoaderInitInfoAndroidKHR loader_init = {};
	loader_init.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
	loader_init.applicationVM = app->activity->vm;
	loader_init.applicationContext = app->activity->clazz;
	res = xrInitializeLoaderKHR(
	    reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&loader_init));
	log_xr_result("xrInitializeLoaderKHR", res);
	return res == XR_SUCCESS;
}

bool
create_instance(struct android_app *app)
{
	// Reset per attempt so the flag reflects THIS attempt's result, not a
	// stale failure from a previous launch (which made the dialog re-appear
	// after the runtime was already started).
	g_runtime_unavailable.store(false, std::memory_order_relaxed);

	// XR_DXR_display_info is what turns this into a real DisplayXR *extension*
	// app: it unlocks the display-pixel dimensions + the rendering-mode
	// enumeration / switching entry points the tiled-atlas multiview port
	// needs (#499). Enable it only when the runtime advertises it so a runtime
	// built without it still brings up (degrading to a default stereo atlas).
	g_has_display_info = false;
	g_has_view_rig = false;
	{
		uint32_t ext_count = 0;
		if (xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr) == XR_SUCCESS &&
		    ext_count > 0) {
			XrExtensionProperties props_buf[128] = {};
			if (ext_count > 128) {
				ext_count = 128;
			}
			for (uint32_t i = 0; i < ext_count; ++i) {
				props_buf[i].type = XR_TYPE_EXTENSION_PROPERTIES;
			}
			if (xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, props_buf) ==
			    XR_SUCCESS) {
				for (uint32_t i = 0; i < ext_count; ++i) {
					if (std::strcmp(props_buf[i].extensionName,
					                XR_DXR_DISPLAY_INFO_EXTENSION_NAME) == 0) {
						g_has_display_info = true;
					} else if (std::strcmp(props_buf[i].extensionName,
					                       XR_DXR_VIEW_RIG_EXTENSION_NAME) == 0) {
						g_has_view_rig = true;
					} else if (std::strcmp(props_buf[i].extensionName,
					                       XR_DXR_DISPLAY_ZONES_EXTENSION_NAME) == 0) {
						g_has_display_zones = true;
					} else if (std::strcmp(props_buf[i].extensionName,
					                       XR_DXR_LOCAL_3D_ZONE_EXTENSION_NAME) == 0) {
						g_has_local_3d_zone = true;
#ifdef CUBE_HAVE_ANDROID_SURFACE_BINDING
					} else if (std::strcmp(props_buf[i].extensionName,
					                       XR_DXR_ANDROID_SURFACE_BINDING_EXTENSION_NAME) ==
					           0) {
						g_has_surface_binding = true;
#endif
					}
				}
			}
		}
		LOGI("XR_DXR_display_info advertised: %s; XR_DXR_view_rig advertised: %s",
		     g_has_display_info ? "yes" : "no", g_has_view_rig ? "yes" : "no");
		// XR_DXR_display_zones REQUIRES both local_3d_zone (>= v3) and view_rig
		// (>= v2) — enabling zones without them is a spec violation, so treat a
		// missing dependency as "no zones" rather than half-enabling.
		if (g_has_display_zones && !(g_has_local_3d_zone && g_has_view_rig)) {
			LOGW("XR_DXR_display_zones advertised but a dependency is missing "
			     "(local_3d_zone=%d view_rig=%d) — zones disabled",
			     (int)g_has_local_3d_zone, (int)g_has_view_rig);
			g_has_display_zones = false;
		}
		LOGI("XR_DXR_display_zones advertised: %s; XR_DXR_local_3d_zone advertised: %s",
		     g_has_display_zones ? "yes" : "no", g_has_local_3d_zone ? "yes" : "no");
	}

	const char *extensions[8] = {
	    XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
	    // _enable2 lets the runtime help create our VkInstance/VkDevice and is
	    // the modern replacement for _enable.
	    XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
	};
	uint32_t extension_count = 2;
	if (g_has_display_info) {
		extensions[extension_count++] = XR_DXR_DISPLAY_INFO_EXTENSION_NAME;
	}
	if (g_has_view_rig) {
		extensions[extension_count++] = XR_DXR_VIEW_RIG_EXTENSION_NAME;
	}
	if (g_has_display_zones) {
		extensions[extension_count++] = XR_DXR_LOCAL_3D_ZONE_EXTENSION_NAME;
		extensions[extension_count++] = XR_DXR_DISPLAY_ZONES_EXTENSION_NAME;
	}
#ifdef CUBE_HAVE_ANDROID_SURFACE_BINDING
	if (g_has_surface_binding) {
		extensions[extension_count++] = XR_DXR_ANDROID_SURFACE_BINDING_EXTENSION_NAME;
	}
	LOGI("XR_DXR_android_surface_binding advertised: %s", g_has_surface_binding ? "yes" : "no");
#endif

	XrInstanceCreateInfoAndroidKHR android_info = {};
	android_info.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
	android_info.applicationVM = app->activity->vm;
	android_info.applicationActivity = app->activity->clazz;

	XrInstanceCreateInfo create_info = {};
	create_info.type = XR_TYPE_INSTANCE_CREATE_INFO;
	create_info.next = &android_info;
	std::strncpy(create_info.applicationInfo.applicationName,
	             "cube_zones_vk_android",
	             XR_MAX_APPLICATION_NAME_SIZE - 1);
	create_info.applicationInfo.applicationVersion = 1;
	std::strncpy(create_info.applicationInfo.engineName, "displayxr",
	             XR_MAX_ENGINE_NAME_SIZE - 1);
	create_info.applicationInfo.engineVersion = 1;
	create_info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	create_info.enabledExtensionCount = extension_count;
	create_info.enabledExtensionNames = extensions;

	// Retry on RUNTIME_UNAVAILABLE: on a cold launch the DisplayXR runtime may
	// still be waking from Android's "stopped" state (MainActivity kicks it via
	// FLAG_INCLUDE_STOPPED_PACKAGES). A few hundred ms lets the broker become
	// discoverable on cooperative devices. NOTE: on aggressive OEMs (e.g. the
	// nubia NP02J / ZTE DefendManager+AutoLaunch) the app-initiated wake is
	// BLOCKED ("RelatedStart BlockResult = true") and these retries can't help —
	// there the user must launch the DisplayXR app once, or enable auto-launch
	// for it in device settings.
	XrResult res = XR_ERROR_RUNTIME_UNAVAILABLE;
	for (int attempt = 0; attempt < 5; ++attempt) {
		res = xrCreateInstance(&create_info, &g_instance);
		if (res != XR_ERROR_RUNTIME_UNAVAILABLE) {
			break;
		}
		LOGW("xrCreateInstance: runtime unavailable (attempt %d/5); waiting for "
		     "the DisplayXR runtime broker (launch the DisplayXR app once if this "
		     "persists — OEM auto-start may be blocking it)…",
		     attempt + 1);
		usleep(400 * 1000);  // 400 ms
	}
	log_xr_result("xrCreateInstance", res);
	if (res != XR_SUCCESS) {
		if (res == XR_ERROR_RUNTIME_UNAVAILABLE) {
			g_runtime_unavailable.store(true, std::memory_order_relaxed);
		}
		return false;
	}

	/* Stable sentinel for CI emulator smoke test (build-android.yml).
	 * Tag intentionally distinctive so a single grep over logcat
	 * confirms the full broker → runtime → plug-in chain reached
	 * xrCreateInstance success. Do NOT rename without updating the
	 * CI workflow. */
	LOGI("ANDROID_POC_SENTINEL xrCreateInstance=XR_SUCCESS");

	XrInstanceProperties props = {};
	props.type = XR_TYPE_INSTANCE_PROPERTIES;
	res = xrGetInstanceProperties(g_instance, &props);
	if (res == XR_SUCCESS) {
		LOGI("Runtime: \"%s\" v%u.%u.%u",
		     props.runtimeName,
		     XR_VERSION_MAJOR(props.runtimeVersion),
		     XR_VERSION_MINOR(props.runtimeVersion),
		     XR_VERSION_PATCH(props.runtimeVersion));
	} else {
		LOGW("xrGetInstanceProperties failed (%d)", (int)res);
	}
	return true;
}

// Query the runtime's XrSystemId for a Vulkan handheld/HMD form factor
// and ask what Vulkan API version the runtime needs. Logs everything to
// logcat — this is the last step we can do without actually creating a
// VkInstance. B13c picks up here and uses these values to create one.
bool
query_system_and_graphics_reqs()
{
	if (g_instance == XR_NULL_HANDLE) {
		return false;
	}

	// Try HMD first (runtime default — see u_system.c), then fall back
	// to handheld for tablet-style displays.
	XrSystemGetInfo sys_info = {};
	sys_info.type = XR_TYPE_SYSTEM_GET_INFO;
	sys_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrResult res = xrGetSystem(g_instance, &sys_info, &g_system_id);
	log_xr_result("xrGetSystem(HEAD_MOUNTED_DISPLAY)", res);
	if (res != XR_SUCCESS) {
		sys_info.formFactor = XR_FORM_FACTOR_HANDHELD_DISPLAY;
		res = xrGetSystem(g_instance, &sys_info, &g_system_id);
		log_xr_result("xrGetSystem(HANDHELD_DISPLAY)", res);
		if (res != XR_SUCCESS) {
			return false;
		}
	}

	XrSystemProperties sys_props = {};
	sys_props.type = XR_TYPE_SYSTEM_PROPERTIES;
	res = xrGetSystemProperties(g_instance, g_system_id, &sys_props);
	if (res == XR_SUCCESS) {
		LOGI("System: \"%s\" vendor=0x%08x maxSwapchain=%ux%u maxLayers=%u",
		     sys_props.systemName, sys_props.vendorId,
		     sys_props.graphicsProperties.maxSwapchainImageWidth,
		     sys_props.graphicsProperties.maxSwapchainImageHeight,
		     sys_props.graphicsProperties.maxLayerCount);
	} else {
		LOGW("xrGetSystemProperties failed (%d)", (int)res);
	}

	// Resolve the extension entry point — it lives in libopenxr_loader.so
	// but the loader exposes it only after the corresponding extension
	// is enabled on the instance (which we did in create_instance).
	PFN_xrGetVulkanGraphicsRequirements2KHR get_reqs = nullptr;
	res = xrGetInstanceProcAddr(
	    g_instance, "xrGetVulkanGraphicsRequirements2KHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&get_reqs));
	if (res != XR_SUCCESS || get_reqs == nullptr) {
		LOGE("xrGetInstanceProcAddr(xrGetVulkanGraphicsRequirements2KHR) failed (%d)",
		     (int)res);
		return false;
	}

	XrGraphicsRequirementsVulkanKHR reqs = {};
	reqs.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR;
	res = get_reqs(g_instance, g_system_id, &reqs);
	log_xr_result("xrGetVulkanGraphicsRequirements2KHR", res);
	if (res != XR_SUCCESS) {
		return false;
	}

	LOGI("Vulkan API: min=%u.%u.%u max=%u.%u.%u",
	     XR_VERSION_MAJOR(reqs.minApiVersionSupported),
	     XR_VERSION_MINOR(reqs.minApiVersionSupported),
	     XR_VERSION_PATCH(reqs.minApiVersionSupported),
	     XR_VERSION_MAJOR(reqs.maxApiVersionSupported),
	     XR_VERSION_MINOR(reqs.maxApiVersionSupported),
	     XR_VERSION_PATCH(reqs.maxApiVersionSupported));

	// Save the runtime's minimum for VkApplicationInfo::apiVersion. Going
	// higher is also allowed (up to max), but min is the safest choice
	// that the runtime promises to accept.
	g_required_vk_version = reqs.minApiVersionSupported;
	return true;
}

// Create a VkInstance via the runtime's xrCreateVulkanInstanceKHR — that
// path lets the runtime inject any platform-specific extensions it needs
// (e.g. swapchain-image-import KHRs) on top of our base
// VkInstanceCreateInfo. No app-side extensions needed at this stage.
bool
create_vulkan_instance()
{
	PFN_xrCreateVulkanInstanceKHR xr_create_vk_instance = nullptr;
	XrResult res = xrGetInstanceProcAddr(
	    g_instance, "xrCreateVulkanInstanceKHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&xr_create_vk_instance));
	if (res != XR_SUCCESS || xr_create_vk_instance == nullptr) {
		LOGE("xrGetInstanceProcAddr(xrCreateVulkanInstanceKHR) failed (%d)", (int)res);
		return false;
	}

	VkApplicationInfo app_info = {};
	app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app_info.pApplicationName = "cube_zones_vk_android";
	app_info.applicationVersion = 1;
	app_info.pEngineName = "displayxr";
	app_info.engineVersion = 1;
	app_info.apiVersion = VK_MAKE_VERSION(
	    XR_VERSION_MAJOR(g_required_vk_version),
	    XR_VERSION_MINOR(g_required_vk_version),
	    0);

	VkInstanceCreateInfo vk_ci = {};
	vk_ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	vk_ci.pApplicationInfo = &app_info;

	XrVulkanInstanceCreateInfoKHR xr_ci = {};
	xr_ci.type = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR;
	xr_ci.systemId = g_system_id;
	xr_ci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
	xr_ci.vulkanCreateInfo = &vk_ci;
	xr_ci.vulkanAllocator = nullptr;

	VkResult vk_result = VK_SUCCESS;
	res = xr_create_vk_instance(g_instance, &xr_ci, &g_vk_instance, &vk_result);
	log_xr_result("xrCreateVulkanInstanceKHR", res);
	if (res != XR_SUCCESS || vk_result != VK_SUCCESS) {
		LOGE("xrCreateVulkanInstanceKHR vk_result=%d", (int)vk_result);
		return false;
	}
	return true;
}

// Ask the runtime which physical device backs our XrSystemId. On Android
// this is typically the one and only GPU, but the API surface is the
// same on multi-GPU desktop platforms.
bool
pick_physical_device()
{
	PFN_xrGetVulkanGraphicsDevice2KHR xr_get_phys = nullptr;
	XrResult res = xrGetInstanceProcAddr(
	    g_instance, "xrGetVulkanGraphicsDevice2KHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&xr_get_phys));
	if (res != XR_SUCCESS || xr_get_phys == nullptr) {
		LOGE("xrGetInstanceProcAddr(xrGetVulkanGraphicsDevice2KHR) failed (%d)", (int)res);
		return false;
	}

	XrVulkanGraphicsDeviceGetInfoKHR info = {};
	info.type = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR;
	info.systemId = g_system_id;
	info.vulkanInstance = g_vk_instance;

	res = xr_get_phys(g_instance, &info, &g_vk_phys_device);
	log_xr_result("xrGetVulkanGraphicsDevice2KHR", res);
	return res == XR_SUCCESS;
}

// Create a VkDevice via xrCreateVulkanDeviceKHR — same pattern as the
// instance, the runtime injects whatever device extensions it needs on
// top of our base info. We supply one graphics queue.
bool
create_vulkan_device()
{
	// Find a queue family with graphics support. On Android there's
	// usually exactly one, but the proper enumeration is cheap.
	uint32_t qf_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(g_vk_phys_device, &qf_count, nullptr);
	if (qf_count == 0) {
		LOGE("No Vulkan queue families");
		return false;
	}
	VkQueueFamilyProperties qf_props[16] = {};
	const uint32_t qf_cap = sizeof(qf_props) / sizeof(qf_props[0]);
	if (qf_count > qf_cap) {
		qf_count = qf_cap;
	}
	vkGetPhysicalDeviceQueueFamilyProperties(g_vk_phys_device, &qf_count, qf_props);

	g_vk_queue_family = UINT32_MAX;
	for (uint32_t i = 0; i < qf_count; ++i) {
		if (qf_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			g_vk_queue_family = i;
			break;
		}
	}
	if (g_vk_queue_family == UINT32_MAX) {
		LOGE("No graphics-capable Vulkan queue family");
		return false;
	}

	const float priority = 1.0f;
	VkDeviceQueueCreateInfo qci = {};
	qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	qci.queueFamilyIndex = g_vk_queue_family;
	qci.queueCount = 1;
	qci.pQueuePriorities = &priority;

	VkPhysicalDeviceFeatures features = {};

	VkDeviceCreateInfo dci = {};
	dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.pEnabledFeatures = &features;

	PFN_xrCreateVulkanDeviceKHR xr_create_vk_device = nullptr;
	XrResult res = xrGetInstanceProcAddr(
	    g_instance, "xrCreateVulkanDeviceKHR",
	    reinterpret_cast<PFN_xrVoidFunction *>(&xr_create_vk_device));
	if (res != XR_SUCCESS || xr_create_vk_device == nullptr) {
		LOGE("xrGetInstanceProcAddr(xrCreateVulkanDeviceKHR) failed (%d)", (int)res);
		return false;
	}

	XrVulkanDeviceCreateInfoKHR xr_ci = {};
	xr_ci.type = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR;
	xr_ci.systemId = g_system_id;
	xr_ci.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
	xr_ci.vulkanPhysicalDevice = g_vk_phys_device;
	xr_ci.vulkanCreateInfo = &dci;
	xr_ci.vulkanAllocator = nullptr;

	VkResult vk_result = VK_SUCCESS;
	res = xr_create_vk_device(g_instance, &xr_ci, &g_vk_device, &vk_result);
	log_xr_result("xrCreateVulkanDeviceKHR", res);
	if (res != XR_SUCCESS || vk_result != VK_SUCCESS) {
		LOGE("xrCreateVulkanDeviceKHR vk_result=%d", (int)vk_result);
		return false;
	}

	vkGetDeviceQueue(g_vk_device, g_vk_queue_family, 0, &g_vk_queue);
	LOGI("Vulkan device ready: queue_family=%u queue=%p", g_vk_queue_family, g_vk_queue);
	return true;
}

bool
create_session()
{
	// XR_KHR_vulkan_enable2 reuses XrGraphicsBindingVulkanKHR — there's
	// no `2` suffix on the binding struct, only on the create/get fns.
	XrGraphicsBindingVulkanKHR binding = {};
	binding.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
	binding.instance = g_vk_instance;
	binding.physicalDevice = g_vk_phys_device;
	binding.device = g_vk_device;
	binding.queueFamilyIndex = g_vk_queue_family;
	binding.queueIndex = 0;

	XrSessionCreateInfo ci = {};
	ci.type = XR_TYPE_SESSION_CREATE_INFO;
	ci.next = &binding;
	ci.systemId = g_system_id;

#ifdef CUBE_HAVE_ANDROID_SURFACE_BINDING
	// Hand the runtime OUR window (#1037). NativeActivity gives it to us
	// directly as android_app::window — no SurfaceView of our own to build, and
	// no runtime-spawned one to crash in freeform. The runtime takes its own
	// ANativeWindow reference; we keep owning the Surface.
	XrAndroidSurfaceBindingCreateInfoDXR surface_binding = {};
	if (g_has_surface_binding) {
		ANativeWindow *win = g_app_window.load(std::memory_order_acquire);
		surface_binding.type = XR_TYPE_ANDROID_SURFACE_BINDING_CREATE_INFO_DXR;
		surface_binding.next = &binding;
		surface_binding.nativeWindow = win;
		// Seed only; MainActivity's Choreographer poll takes over immediately
		// via xrSetAndroidWindowGeometryDXR.
		surface_binding.screenOffsetX = g_win_rect.x;
		surface_binding.screenOffsetY = g_win_rect.y;
		surface_binding.transparentBackgroundEnabled = XR_FALSE;
		ci.next = &surface_binding;
		LOGI("xrCreateSession: chaining XR_DXR_android_surface_binding (window=%p offset=%d,%d)",
		     (void *)win, (int)g_win_rect.x, (int)g_win_rect.y);
	}
#endif

	XrResult res = xrCreateSession(g_instance, &ci, &g_session);
	log_xr_result("xrCreateSession", res);

#ifdef CUBE_HAVE_ANDROID_SURFACE_BINDING
	if (res == XR_SUCCESS && g_has_surface_binding) {
		xrGetInstanceProcAddr(g_instance, "xrSetAndroidSurfaceDXR",
		                      (PFN_xrVoidFunction *)&g_pfnSetAndroidSurface);
		xrGetInstanceProcAddr(g_instance, "xrSetAndroidWindowGeometryDXR",
		                      (PFN_xrVoidFunction *)&g_pfnSetAndroidWindowGeometry);
		LOGI("surface-binding entry points: set_surface=%p set_geometry=%p",
		     (void *)g_pfnSetAndroidSurface, (void *)g_pfnSetAndroidWindowGeometry);
	}
#endif
	return res == XR_SUCCESS;
}

#ifdef CUBE_HAVE_ANDROID_SURFACE_BINDING
// Republish (win != nullptr) or drop (nullptr) our Surface. Called from the
// android_main thread on APP_CMD_INIT_WINDOW / APP_CMD_TERM_WINDOW, which is
// exactly where native_app_glue learns of surfaceCreated/surfaceDestroyed.
void
publish_app_surface(ANativeWindow *win)
{
	g_app_window.store(win, std::memory_order_release);
	if (g_pfnSetAndroidSurface == nullptr || g_session == XR_NULL_HANDLE) {
		return;
	}
	XrAndroidSurfaceBindingCreateInfoDXR b = {};
	b.type = XR_TYPE_ANDROID_SURFACE_BINDING_CREATE_INFO_DXR;
	b.nativeWindow = win;
	b.screenOffsetX = g_win_rect.x;
	b.screenOffsetY = g_win_rect.y;
	XrResult res = g_pfnSetAndroidSurface(g_session, win != nullptr ? &b : nullptr);
	LOGI("xrSetAndroidSurfaceDXR(%p) -> %d", (void *)win, (int)res);
}

// Push the latest UI-thread window-rect sample to the runtime, once per frame
// and only when it actually changed. The runtime needs it for BOTH the vendor
// weave phase and the per-window Kooima canvas — and a pure window MOVE is
// reported to nobody but this app (#1033/#1034, ADR-036 D6).
void
push_window_geometry()
{
	if (g_pfnSetAndroidWindowGeometry == nullptr || g_session == XR_NULL_HANDLE) {
		return;
	}
	static uint64_t last_seq = 0;
	uint64_t seq = g_win_rect_seq.load(std::memory_order_acquire);
	if (seq == last_seq || seq == 0) {
		return;
	}
	last_seq = seq;
	WindowRectSample r = g_win_rect;
	if (r.w <= 0 || r.h <= 0) {
		return;
	}
	XrAndroidWindowGeometryDXR g = {};
	g.type = XR_TYPE_ANDROID_WINDOW_GEOMETRY_DXR;
	g.windowRect = XrRect2Di{{r.x, r.y}, {r.w, r.h}};
	g.panelExtent = XrExtent2Di{r.panel_w, r.panel_h};
	g.displayId = r.display_id;
	XrResult res = g_pfnSetAndroidWindowGeometry(g_session, &g);
	// Lifecycle only (the rect actually changed), never every frame.
	LOGI("window screen rect: %d,%d %dx%d panel %dx%d display %d -> %d", r.x, r.y, r.w, r.h, r.panel_w,
	     r.panel_h, r.display_id, (int)res);
}
#endif

// ── Active rendering-mode accessors ───────────────────────────────────────
// active_mode() returns the live mode; with no XR_DXR_display_info it returns
// a sane default-stereo (2 views, 2×1 tiles, 0.5×1.0 scale) so the atlas path
// still works against a runtime that doesn't advertise rendering modes.
const RenderingModeInfo &
active_mode()
{
	static const RenderingModeInfo kDefaultStereo = {};
	if (g_mode_count == 0) {
		return kDefaultStereo;
	}
	uint32_t idx = g_current_mode.load(std::memory_order_relaxed);
	if (idx >= g_mode_count) {
		idx = 0;
	}
	return g_modes[idx];
}

uint32_t
active_view_count()
{
	uint32_t vc = active_mode().view_count;
	if (vc < 1) {
		vc = 1;
	}
	if (vc > kMaxViews) {
		vc = kMaxViews;
	}
	return vc;
}

// The client-window canvas in pixels for the CURRENTLY HELD orientation. Panel
// long/short edges are fixed; g_display_px_* is the STARTUP orientation, so
// derive orientation-independent long/short first and let the live rotation
// decide which is width. This app is fullscreen, so window == panel; a freeform
// window reports its own size via nativeSetWindowRect (used below when present).
void
current_canvas_dims(uint32_t *out_w, uint32_t *out_h)
{
	uint32_t big = g_display_px_w >= g_display_px_h ? g_display_px_w : g_display_px_h;
	uint32_t small = g_display_px_w >= g_display_px_h ? g_display_px_h : g_display_px_w;
	int rot = g_display_rotation.load(std::memory_order_relaxed);
	bool portrait = (rot == 1 || rot == 3);
	uint32_t w = portrait ? small : big;
	uint32_t h = portrait ? big : small;

#ifdef CUBE_HAVE_ANDROID_SURFACE_BINDING
	// AUTHORITATIVE: this app owns its Surface (XR_DXR_android_surface_binding),
	// so the window's pixel size is a direct query — no orientation guessing.
	// The rotation heuristic above is only a pre-surface fallback, and it is
	// device-dependent (on the NP02J natural orientation is PORTRAIT, so
	// ROTATION_0 is portrait and the usual `rot==1||rot==3` test is inverted).
	// Getting this wrong makes the app emit a zone rect that overflows its own
	// window — which then reads as a runtime/plug-in phase bug when it is not.
	ANativeWindow *win = g_app_window.load(std::memory_order_acquire);
	if (win != nullptr) {
		int32_t ww = ANativeWindow_getWidth(win);
		int32_t wh = ANativeWindow_getHeight(win);
		if (ww > 0 && wh > 0) {
			w = (uint32_t)ww;
			h = (uint32_t)wh;
		}
	}
	// In freeform / split-screen the UI thread also samples the window rect
	// (Android reports a pure MOVE to nobody else); prefer it when present, it
	// is the same size in the same space.
	uint64_t seq = g_win_rect_seq.load(std::memory_order_acquire);
	if (seq != 0) {
		WindowRectSample sample = g_win_rect;
		if (g_win_rect_seq.load(std::memory_order_acquire) == seq && sample.w > 0 && sample.h > 0) {
			w = (uint32_t)sample.w;
			h = (uint32_t)sample.h;
		}
	}
#endif
	*out_w = w;
	*out_h = h;
}

// The frame's zone layout, in client-window pixels. Recomputed every frame from
// the live canvas — NEVER cached and never re-derived from a previous zone rect
// (runtime#570: an app that re-derives its zone from a zone-sized "canvas"
// shrinks it geometrically each frame until the woven band collapses).
void
compute_zone_layout(ZoneLayout *zl)
{
	uint32_t cw = 0, ch = 0;
	current_canvas_dims(&cw, &ch);
	zl->canvas_w = (int32_t)cw;
	zl->canvas_h = (int32_t)ch;

	// 3D zone: offset on BOTH axes so a dropped x- OR y-offset in the weave
	// phase is distinguishable. Even-aligned (an odd offset would fold a
	// half-pixel into the interlace and muddy the read).
	zl->z3d_w = ((int32_t)cw / 2) & ~1;
	zl->z3d_h = ((int32_t)ch / 2) & ~1;
	zl->z3d_x = ((int32_t)cw / 3) & ~1;
	zl->z3d_y = ((int32_t)ch / 4) & ~1;
	if (zl->z3d_x + zl->z3d_w > (int32_t)cw) {
		zl->z3d_x = (int32_t)cw - zl->z3d_w;
	}
	if (zl->z3d_y + zl->z3d_h > (int32_t)ch) {
		zl->z3d_y = (int32_t)ch - zl->z3d_h;
	}

	// Local2D band across the top — pure 2D content in a zones frame (the
	// implicit-mask-from-Local2D-rects rule is OFF here, ADR-027).
	zl->l2d_x = 0;
	zl->l2d_y = 0;
	zl->l2d_w = (int32_t)cw;
	zl->l2d_h = ((int32_t)ch / 8) & ~1;
}

// Per-tile render size + tile grid for the active mode, sized for the CURRENTLY
// HELD orientation (#518). Each eye renders at current_display × view_scale — so
// landscape gives e.g. 1920×1200 and portrait 1200×1920 (the device's per-eye tile
// in that orientation), not a fixed startup-orientation size. The swapchain/atlas is
// allocated worst-case across orientations (create_swapchains), so the per-frame tile
// is a sub-rect of it; render_w/render_h drive both the render viewport and the
// submitted subImage.imageRect, so the weave reads the correct per-orientation tile.
void
active_tile_dims(uint32_t *render_w, uint32_t *render_h, uint32_t *cols, uint32_t *rows)
{
	const RenderingModeInfo &m = active_mode();
	uint32_t c = m.tile_columns ? m.tile_columns : 1;
	uint32_t r = m.tile_rows ? m.tile_rows : 1;

	// Panel long/short edges are fixed; the held orientation (g_display_rotation
	// 1/3 = portrait) decides which is width vs height. g_display_px is the startup
	// orientation, so derive orientation-independent long/short first.
	uint32_t disp_w = 0, disp_h = 0;
	current_canvas_dims(&disp_w, &disp_h);
	if (disp_w == 0 || disp_h == 0) { // no display info yet — fall back to atlas tiles
		disp_w = g_atlas.width / c;
		disp_h = g_atlas.height / r;
	}

	// ZONES: the tile the app renders is the ZONE's canvas, not the window's —
	// the zone rect IS the canvas (ADR-027 Decision 3). Scale by the mode's
	// view_scale exactly as the full-canvas path does, so the DP sees the same
	// tile-aspect ↔ canvas-aspect relationship it does for a plain projection.
	if (g_zones_active) {
		ZoneLayout zl = {};
		compute_zone_layout(&zl);
		if (zl.z3d_w > 0 && zl.z3d_h > 0) {
			disp_w = (uint32_t)zl.z3d_w;
			disp_h = (uint32_t)zl.z3d_h;
		}
	}

	uint32_t rw = (uint32_t)((double)disp_w * m.view_scale_x);
	uint32_t rh = (uint32_t)((double)disp_h * m.view_scale_y);
	if (rw == 0)
		rw = disp_w;
	if (rh == 0)
		rh = disp_h;
	// Never exceed the atlas tile capacity (tiles must not overlap / overflow).
	uint32_t max_tw = g_atlas.width / c;
	uint32_t max_th = g_atlas.height / r;
	if (rw > max_tw)
		rw = max_tw;
	if (rh > max_th)
		rh = max_th;
	*render_w = rw;
	*render_h = rh;
	*cols = c;
	*rows = r;
}

// Query display pixel dimensions (XR_DXR_display_info) + the runtime's
// rendering modes, and the max view count the runtime advertises. Must run
// AFTER create_session() (mode enumeration is session-scoped) and BEFORE
// create_swapchains() (atlas sizing needs the display dims + mode tile layout).
bool
query_display_info_and_modes()
{
	// Max view count = the device's advertised PRIMARY_STEREO view count (the
	// max across all rendering modes). xrLocateViews REQUIRES capacity >= this
	// value or it returns XR_ERROR_SIZE_INSUFFICIENT — the exact gate the old
	// hard-coded `2` tripped on sim_display (which reports 4).
	uint32_t vc = 0;
	XrResult res = xrEnumerateViewConfigurationViews(
	    g_instance, g_system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &vc, nullptr);
	if (res == XR_SUCCESS && vc > 0) {
		g_max_view_count = vc > kMaxViews ? kMaxViews : vc;
	}
	LOGI("Runtime advertises %u views (max across modes)", g_max_view_count);

	// Native panel pixels — chain XrDisplayInfoDXR onto the system-properties
	// query (there is no standalone xrGetDisplayInfoEXT).
	if (g_has_display_info) {
		XrDisplayInfoDXR di = {XR_TYPE_DISPLAY_INFO_DXR};
		XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
		sp.next = &di;
		if (xrGetSystemProperties(g_instance, g_system_id, &sp) == XR_SUCCESS) {
			g_display_px_w = di.displayPixelWidth;
			g_display_px_h = di.displayPixelHeight;
			LOGI("Display: %ux%u px, %.3fx%.3f m", g_display_px_w, g_display_px_h,
			     di.displaySizeMeters.width, di.displaySizeMeters.height);
		}

		xrGetInstanceProcAddr(g_instance, "xrEnumerateDisplayRenderingModesDXR",
		                      reinterpret_cast<PFN_xrVoidFunction *>(&g_pfnEnumModes));
		xrGetInstanceProcAddr(g_instance, "xrRequestDisplayRenderingModeDXR",
		                      reinterpret_cast<PFN_xrVoidFunction *>(&g_pfnRequestMode));
		xrGetInstanceProcAddr(g_instance, "xrRequestEyeTrackingModeDXR",
		                      reinterpret_cast<PFN_xrVoidFunction *>(&g_pfnRequestEyeMode));
	}

	// Enumerate rendering modes (view counts, tile layouts, scales). The
	// runtime reports which mode is active via isActive; we adopt it.
	if (g_pfnEnumModes != nullptr) {
		uint32_t mc = 0;
		if (g_pfnEnumModes(g_session, 0, &mc, nullptr) == XR_SUCCESS && mc > 0) {
			if (mc > kMaxViews) {
				mc = kMaxViews;
			}
			XrDisplayRenderingModeInfoDXR modes[kMaxViews] = {};
			for (uint32_t i = 0; i < mc; ++i) {
				modes[i].type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_DXR;
			}
			if (g_pfnEnumModes(g_session, mc, &mc, modes) == XR_SUCCESS) {
				g_mode_count = mc;
				LOGI("Rendering modes (%u):", mc);
				for (uint32_t i = 0; i < mc; ++i) {
					g_modes[i].view_count = modes[i].viewCount ? modes[i].viewCount : 1;
					g_modes[i].tile_columns = modes[i].tileColumns ? modes[i].tileColumns : 1;
					g_modes[i].tile_rows = modes[i].tileRows ? modes[i].tileRows : 1;
					g_modes[i].view_scale_x = modes[i].viewScaleX > 0.0f ? modes[i].viewScaleX : 1.0f;
					g_modes[i].view_scale_y = modes[i].viewScaleY > 0.0f ? modes[i].viewScaleY : 1.0f;
					g_modes[i].hardware_3d = modes[i].hardwareDisplay3D != 0;
					g_modes[i].requestable = modes[i].isRequestable != 0;
					std::strncpy(g_modes[i].name, modes[i].modeName, sizeof(g_modes[i].name) - 1);
					g_modes[i].name[sizeof(g_modes[i].name) - 1] = '\0';
					LOGI("  [%u] %s views=%u tiles=%ux%u scale=%.2fx%.2f 3D=%d active=%d req=%d",
					     modes[i].modeIndex, g_modes[i].name, g_modes[i].view_count,
					     g_modes[i].tile_columns, g_modes[i].tile_rows, g_modes[i].view_scale_x,
					     g_modes[i].view_scale_y, (int)g_modes[i].hardware_3d, (int)modes[i].isActive,
					     (int)g_modes[i].requestable);
					if (modes[i].isActive) {
						g_current_mode.store(modes[i].modeIndex, std::memory_order_relaxed);
					}
				}
			}
		}
	}
	if (g_mode_count == 0) {
		LOGW("No rendering modes enumerated — defaulting to a single 2-view SBS atlas");
	}
	return true;
}

// Create the SINGLE tiled-atlas swapchain. Each frame the app renders the
// active mode's views into tiles of this one image and submits N projection
// views that reference it with per-tile imageRects (mirrors the Windows /
// macOS cubes). Sized to the largest atlas any mode can produce full-screen —
// for sim_display / Leia SR that collapses to the panel resolution.
bool
create_swapchains()
{
	// Per-view config: only needed here for the sample count + the no-display-
	// info fallback dims. Don't gate on the count anymore.
	XrViewConfigurationView view_config = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
	{
		uint32_t got = 0;
		XrViewConfigurationView buf[kMaxViews] = {};
		for (uint32_t i = 0; i < kMaxViews; ++i) {
			buf[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
		}
		uint32_t cap = g_max_view_count > kMaxViews ? kMaxViews : g_max_view_count;
		XrResult vres = xrEnumerateViewConfigurationViews(
		    g_instance, g_system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, cap, &got, buf);
		if (vres == XR_SUCCESS && got > 0) {
			view_config = buf[0];
		}
	}

	// Pick a swapchain format the runtime supports. Prefer 8-bit linear
	// RGBA / BGRA — these line up with what the runtime's vk_native
	// compositor and the DP expect. SRGB variants kicked out so we don't
	// trip a gamma double-correction.
	uint32_t format_count = 0;
	XrResult res = xrEnumerateSwapchainFormats(g_session, 0, &format_count, nullptr);
	if (res != XR_SUCCESS || format_count == 0) {
		log_xr_result("xrEnumerateSwapchainFormats(count)", res);
		return false;
	}
	int64_t formats[64] = {};
	if (format_count > 64) {
		format_count = 64;
	}
	res = xrEnumerateSwapchainFormats(g_session, format_count, &format_count, formats);
	if (res != XR_SUCCESS) {
		log_xr_result("xrEnumerateSwapchainFormats(fill)", res);
		return false;
	}
	const int64_t preferred[] = {
	    VK_FORMAT_B8G8R8A8_UNORM,
	    VK_FORMAT_R8G8B8A8_UNORM,
	};
	for (int64_t pref : preferred) {
		for (uint32_t i = 0; i < format_count && g_swapchain_format == VK_FORMAT_UNDEFINED; ++i) {
			if (formats[i] == pref) {
				g_swapchain_format = (VkFormat)pref;
			}
		}
		if (g_swapchain_format != VK_FORMAT_UNDEFINED) {
			break;
		}
	}
	if (g_swapchain_format == VK_FORMAT_UNDEFINED) {
		LOGE("Runtime didn't advertise a UNORM swapchain format; first supported = 0x%llx",
		     (long long)formats[0]);
		g_swapchain_format = (VkFormat)formats[0];
	}
	LOGI("Chose swapchain format: 0x%x", (uint32_t)g_swapchain_format);

	// Atlas dims = worst case over all modes AND BOTH ORIENTATIONS (#518). The
	// swapchain is never recreated on device rotation, so it must hold either
	// orientation's largest tile layout; each frame renders a per-orientation
	// sub-rect of it (active_tile_dims). For each mode the atlas is sized for
	// (long×short) and (short×long); the global max per dim is taken.
	uint32_t atlas_w = 0, atlas_h = 0;
	if (g_display_px_w > 0 && g_display_px_h > 0) {
		uint32_t big = g_display_px_w >= g_display_px_h ? g_display_px_w : g_display_px_h;
		uint32_t small = g_display_px_w >= g_display_px_h ? g_display_px_h : g_display_px_w;
		atlas_w = big;
		atlas_h = big; // square lower bound — either orientation's long edge can be width or height
		for (uint32_t i = 0; i < g_mode_count; ++i) {
			const uint32_t dims[2][2] = {{big, small}, {small, big}}; // landscape, portrait
			for (uint32_t o = 0; o < 2; ++o) {
				uint32_t aw = (uint32_t)((double)g_modes[i].tile_columns *
				                         g_modes[i].view_scale_x * (double)dims[o][0]);
				uint32_t ah = (uint32_t)((double)g_modes[i].tile_rows *
				                         g_modes[i].view_scale_y * (double)dims[o][1]);
				if (aw > atlas_w) {
					atlas_w = aw;
				}
				if (ah > atlas_h) {
					atlas_h = ah;
				}
			}
		}
	} else {
		// No display info: fall back to the recommended per-view rect laid out
		// across the default mode's tile grid.
		const RenderingModeInfo &m = active_mode();
		atlas_w = view_config.recommendedImageRectWidth * m.tile_columns;
		atlas_h = view_config.recommendedImageRectHeight * m.tile_rows;
	}
	if (atlas_w == 0 || atlas_h == 0) {
		LOGE("Atlas sizing failed (%ux%u); aborting swapchain create", atlas_w, atlas_h);
		return false;
	}

	g_atlas.width = atlas_w;
	g_atlas.height = atlas_h;

	XrSwapchainCreateInfo ci = {};
	ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
	ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	ci.format = g_swapchain_format;
	ci.sampleCount = view_config.recommendedSwapchainSampleCount ? view_config.recommendedSwapchainSampleCount : 1;
	ci.width = atlas_w;
	ci.height = atlas_h;
	ci.faceCount = 1;
	ci.arraySize = 1;
	ci.mipCount = 1;

	res = xrCreateSwapchain(g_session, &ci, &g_atlas.swapchain);
	if (res != XR_SUCCESS) {
		log_xr_result("xrCreateSwapchain", res);
		return false;
	}

	uint32_t img_count = 0;
	res = xrEnumerateSwapchainImages(g_atlas.swapchain, 0, &img_count, nullptr);
	if (res != XR_SUCCESS) {
		log_xr_result("xrEnumerateSwapchainImages(count)", res);
		return false;
	}
	if (img_count > 8) {
		LOGW("Swapchain advertises %u images; capping at 8", img_count);
		img_count = 8;
	}
	for (uint32_t j = 0; j < img_count; ++j) {
		g_atlas.images[j].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
	}
	res = xrEnumerateSwapchainImages(g_atlas.swapchain, img_count, &img_count,
	                                 reinterpret_cast<XrSwapchainImageBaseHeader *>(g_atlas.images));
	if (res != XR_SUCCESS) {
		log_xr_result("xrEnumerateSwapchainImages(fill)", res);
		return false;
	}
	g_atlas.image_count = img_count;
	LOGI("Atlas swapchain: %ux%u, %u images (active mode '%s', %u views)", atlas_w, atlas_h,
	     img_count, active_mode().name, active_view_count());
	return true;
}

// ─── XR_DXR_display_zones: caps + the Local2D band ────────────────────────

//! A fixed-size flat swapchain for the 2D band. Deliberately small (the runtime
//! scale-blits it into the band rect post-weave) and filled from a staging
//! buffer so no extra render pass / pipeline is needed.
constexpr uint32_t kLocal2DWidth = 512;
constexpr uint32_t kLocal2DHeight = 64;

//! High-contrast vertical bar ruler. Bars every 32 texels with a distinct
//! leftmost bar, so the human eye can tell at a glance whether the 2D band was
//! placed at the window origin (bar 0 flush left) or somewhere else.
static void
fill_local2d_pattern(uint8_t *px, uint32_t w, uint32_t h)
{
	for (uint32_t y = 0; y < h; ++y) {
		for (uint32_t x = 0; x < w; ++x) {
			uint8_t *p = px + (size_t)(y * w + x) * 4;
			const bool bar = (x % 32u) < 12u;
			const bool first = (x < 12u);
			p[0] = first ? 255 : (bar ? 40 : 12);   // R
			p[1] = first ? 80 : (bar ? 200 : 16);   // G
			p[2] = first ? 40 : (bar ? 235 : 28);   // B
			p[3] = 255;                              // opaque: this is 2D content
		}
	}
}

bool
create_local2d_swapchain()
{
	XrSwapchainCreateInfo ci = {};
	ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
	ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
	ci.format = g_swapchain_format;
	ci.sampleCount = 1;
	ci.width = kLocal2DWidth;
	ci.height = kLocal2DHeight;
	ci.faceCount = 1;
	ci.arraySize = 1;
	ci.mipCount = 1;
	XrResult res = xrCreateSwapchain(g_session, &ci, &g_local2d.swapchain);
	if (res != XR_SUCCESS) {
		log_xr_result("xrCreateSwapchain(local2d)", res);
		return false;
	}
	g_local2d.width = kLocal2DWidth;
	g_local2d.height = kLocal2DHeight;

	uint32_t n = 0;
	if (xrEnumerateSwapchainImages(g_local2d.swapchain, 0, &n, nullptr) != XR_SUCCESS || n == 0) {
		return false;
	}
	if (n > 8) {
		n = 8;
	}
	for (uint32_t i = 0; i < n; ++i) {
		g_local2d.images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
	}
	if (xrEnumerateSwapchainImages(g_local2d.swapchain, n, &n,
	                               reinterpret_cast<XrSwapchainImageBaseHeader *>(g_local2d.images)) !=
	    XR_SUCCESS) {
		return false;
	}
	g_local2d.image_count = n;

	// Host-visible staging buffer, filled once with the pattern.
	const VkDeviceSize bytes = (VkDeviceSize)kLocal2DWidth * kLocal2DHeight * 4;
	VkBufferCreateInfo bci = {};
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size = bytes;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(g_vk_device, &bci, nullptr, &g_local2d.staging) != VK_SUCCESS) {
		return false;
	}
	VkMemoryRequirements req = {};
	vkGetBufferMemoryRequirements(g_vk_device, g_local2d.staging, &req);
	VkMemoryAllocateInfo mai = {};
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = find_memory_type(req.memoryTypeBits,
	                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
	                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (mai.memoryTypeIndex == UINT32_MAX ||
	    vkAllocateMemory(g_vk_device, &mai, nullptr, &g_local2d.staging_mem) != VK_SUCCESS) {
		return false;
	}
	vkBindBufferMemory(g_vk_device, g_local2d.staging, g_local2d.staging_mem, 0);
	void *mapped = nullptr;
	if (vkMapMemory(g_vk_device, g_local2d.staging_mem, 0, bytes, 0, &mapped) != VK_SUCCESS) {
		return false;
	}
	fill_local2d_pattern(static_cast<uint8_t *>(mapped), kLocal2DWidth, kLocal2DHeight);
	vkUnmapMemory(g_vk_device, g_local2d.staging_mem);

	LOGI("[zones] Local2D swapchain: %ux%u, %u images", kLocal2DWidth, kLocal2DHeight, n);
	return true;
}

//! Copy the staged pattern into Local2D image @p idx. The runtime hands these
//! out in GENERAL (AHardwareBuffer-imported), so bracket the copy accordingly.
static bool
upload_local2d_image(uint32_t idx)
{
	VkCommandBufferAllocateInfo ai = {};
	ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	ai.commandPool = g_app_cmd_pool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(g_vk_device, &ai, &cmd) != VK_SUCCESS) {
		return false;
	}
	VkCommandBufferBeginInfo bi = {};
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &bi);

	VkImage img = g_local2d.images[idx].image;
	VkImageMemoryBarrier to_dst = {};
	to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	to_dst.srcAccessMask = 0;
	to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	to_dst.image = img;
	to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
	                     nullptr, 0, nullptr, 1, &to_dst);

	VkBufferImageCopy copy = {};
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.imageExtent = {kLocal2DWidth, kLocal2DHeight, 1};
	vkCmdCopyBufferToImage(cmd, g_local2d.staging, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

	VkImageMemoryBarrier to_gen = to_dst;
	to_gen.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	to_gen.dstAccessMask = 0;
	to_gen.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	to_gen.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
	                     0, nullptr, 0, nullptr, 1, &to_gen);

	vkEndCommandBuffer(cmd);
	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	VkResult r = vkQueueSubmit(g_vk_queue, 1, &si, VK_NULL_HANDLE);
	if (r == VK_SUCCESS) {
		vkQueueWaitIdle(g_vk_queue);
	}
	vkFreeCommandBuffers(g_vk_device, g_app_cmd_pool, 1, &cmd);
	return r == VK_SUCCESS;
}

//! Query zone capabilities and stand up the Local2D band. Called once, after
//! the session exists. Failure degrades to the plain full-canvas projection
//! path (identical to cube_handle_vk_android) rather than aborting.
bool
activate_zones()
{
	if (!g_has_display_zones) {
		LOGW("[zones] XR_DXR_display_zones not enabled — running the full-canvas path");
		return false;
	}
	if (xrGetInstanceProcAddr(g_instance, "xrGetDisplayZoneCapabilitiesDXR",
	                          reinterpret_cast<PFN_xrVoidFunction *>(&g_pfnGetZoneCaps)) != XR_SUCCESS ||
	    g_pfnGetZoneCaps == nullptr) {
		LOGW("[zones] xrGetDisplayZoneCapabilitiesDXR unresolvable — zones disabled");
		return false;
	}
	XrDisplayZoneCapabilitiesDXR caps = {XR_TYPE_DISPLAY_ZONE_CAPABILITIES_DXR};
	XrResult r = g_pfnGetZoneCaps(g_session, &caps);
	if (XR_FAILED(r) || !caps.supported || caps.maxZones3D < 1) {
		LOGW("[zones] capabilities: rc=0x%x supported=%d maxZones3D=%u — zones disabled",
		     (unsigned)r, (int)caps.supported, caps.maxZones3D);
		return false;
	}
	LOGI("[zones] capabilities: supported=1 maxZones3D=%u", caps.maxZones3D);

	if (!create_local2d_swapchain()) {
		LOGW("[zones] Local2D swapchain creation failed — the 2D band will be empty");
	}

	g_zones_active = true;
	ZoneLayout zl = {};
	compute_zone_layout(&zl);
	LOGI("[zones] ACTIVE canvas=%dx%d px | 3D zone id=1 rect=%d,%d %dx%d | Local2D rect=%d,%d %dx%d",
	     zl.canvas_w, zl.canvas_h, zl.z3d_x, zl.z3d_y, zl.z3d_w, zl.z3d_h, zl.l2d_x, zl.l2d_y,
	     zl.l2d_w, zl.l2d_h);
	return true;
}

void
destroy_local2d_swapchain()
{
	if (g_local2d.swapchain != XR_NULL_HANDLE) {
		xrDestroySwapchain(g_local2d.swapchain);
		g_local2d.swapchain = XR_NULL_HANDLE;
	}
	if (g_local2d.staging != VK_NULL_HANDLE) {
		vkDestroyBuffer(g_vk_device, g_local2d.staging, nullptr);
		g_local2d.staging = VK_NULL_HANDLE;
	}
	if (g_local2d.staging_mem != VK_NULL_HANDLE) {
		vkFreeMemory(g_vk_device, g_local2d.staging_mem, nullptr);
		g_local2d.staging_mem = VK_NULL_HANDLE;
	}
	g_local2d.image_count = 0;
}

// One reference space for the app — projection layer poses are expressed
// relative to this. STAGE if the runtime supports it, else LOCAL.
bool
create_reference_space()
{
	XrReferenceSpaceCreateInfo ci = {};
	ci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
	ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	ci.poseInReferenceSpace.orientation = {0, 0, 0, 1};
	ci.poseInReferenceSpace.position = {0, 0, 0};
	XrResult res = xrCreateReferenceSpace(g_session, &ci, &g_app_space);
	log_xr_result("xrCreateReferenceSpace(LOCAL)", res);
	return res == XR_SUCCESS;
}

// Initialize the crate cube + grid scene (pipelines, geometry, textures). Runs
// after the render pass exists (the scene pipelines compile against it).
bool
create_scene()
{
	return crate_scene_init(g_scene, g_vk_device, g_vk_phys_device, g_vk_queue, g_vk_queue_family,
	                        g_render_pass, g_asset_manager);
}

// Standalone command pool for the test app's per-frame cmd buffers.
// Created lazily once the session + swapchains exist.
bool
create_cmd_pool()
{
	VkCommandPoolCreateInfo ci = {};
	ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	ci.queueFamilyIndex = g_vk_queue_family;
	VkResult res = vkCreateCommandPool(g_vk_device, &ci, nullptr, &g_app_cmd_pool);
	if (res != VK_SUCCESS) {
		LOGE("vkCreateCommandPool failed: %d", res);
		return false;
	}
	return true;
}

void
destroy_swapchains()
{
	if (g_atlas.swapchain != XR_NULL_HANDLE) {
		xrDestroySwapchain(g_atlas.swapchain);
		g_atlas.swapchain = XR_NULL_HANDLE;
	}
	destroy_local2d_swapchain();
	g_atlas.image_count = 0;
	g_atlas.width = 0;
	g_atlas.height = 0;
	g_swapchain_format = VK_FORMAT_UNDEFINED;
}

void
destroy_reference_space()
{
	if (g_app_space != XR_NULL_HANDLE) {
		xrDestroySpace(g_app_space);
		g_app_space = XR_NULL_HANDLE;
	}
}

void
destroy_cmd_pool()
{
	if (g_app_cmd_pool != VK_NULL_HANDLE && g_vk_device != VK_NULL_HANDLE) {
		vkDestroyCommandPool(g_vk_device, g_app_cmd_pool, nullptr);
		g_app_cmd_pool = VK_NULL_HANDLE;
	}
}

// Record + submit a clear-to-color cmd buffer on the swapchain image
// for view `view_idx` at swapchain index `image_idx`. Distinct colors
// per view so a one-eye-covered hardware test can verify left/right
// (see displayxr-leia-plugin docs/cnsdk-android-calibration.md § "Tile-to-eye
// mapping"). Layout-aware: arrives from xrAcquireSwapchainImage in
// ─── matrix helpers ──────────────────────────────────────────────────
//
// Column-major 4x4 matrix so push constants land in GLSL "mat4" layout
// without per-element transposition.

struct Mat4 { float m[16]; };

inline Mat4
mat4_identity()
{
	Mat4 r{};
	r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
	return r;
}

inline Mat4
mat4_mul(const Mat4 &a, const Mat4 &b)
{
	Mat4 r{};
	for (int col = 0; col < 4; ++col) {
		for (int row = 0; row < 4; ++row) {
			float s = 0.0f;
			for (int k = 0; k < 4; ++k) {
				s += a.m[k * 4 + row] * b.m[col * 4 + k];
			}
			r.m[col * 4 + row] = s;
		}
	}
	return r;
}

// Build the view matrix that transforms world-space points into the
// eye's local frame. OpenXR's XrPosef is world-from-eye, so the view
// matrix is inverse(world-from-eye). For a rigid transform that's
// transpose(R) + (-transpose(R) * t).
Mat4
view_matrix_from_pose(const XrPosef &pose)
{
	const float x = pose.orientation.x, y = pose.orientation.y;
	const float z = pose.orientation.z, w = pose.orientation.w;
	const float xx = x * x, yy = y * y, zz = z * z;
	const float xy = x * y, xz = x * z, yz = y * z;
	const float wx = w * x, wy = w * y, wz = w * z;

	// R is world-from-eye rotation. View matrix uses R^T.
	const float r00 = 1.0f - 2.0f * (yy + zz);
	const float r01 = 2.0f * (xy + wz);
	const float r02 = 2.0f * (xz - wy);
	const float r10 = 2.0f * (xy - wz);
	const float r11 = 1.0f - 2.0f * (xx + zz);
	const float r12 = 2.0f * (yz + wx);
	const float r20 = 2.0f * (xz + wy);
	const float r21 = 2.0f * (yz - wx);
	const float r22 = 1.0f - 2.0f * (xx + yy);

	// Translation in eye-space: -R^T * t
	const float tx = -(r00 * pose.position.x + r01 * pose.position.y + r02 * pose.position.z);
	const float ty = -(r10 * pose.position.x + r11 * pose.position.y + r12 * pose.position.z);
	const float tz = -(r20 * pose.position.x + r21 * pose.position.y + r22 * pose.position.z);

	// Column-major: col c, row r → m[c*4+r]
	Mat4 v{};
	v.m[0]  = r00; v.m[1]  = r10; v.m[2]  = r20; v.m[3]  = 0.0f;
	v.m[4]  = r01; v.m[5]  = r11; v.m[6]  = r21; v.m[7]  = 0.0f;
	v.m[8]  = r02; v.m[9]  = r12; v.m[10] = r22; v.m[11] = 0.0f;
	v.m[12] = tx;  v.m[13] = ty;  v.m[14] = tz;  v.m[15] = 1.0f;
	return v;
}

// Build an asymmetric perspective projection from OpenXR FOV (radians,
// signed: angleLeft < 0, angleRight > 0). Vulkan clip space: x ∈ [-1,1],
// y ∈ [-1,1] points DOWN, z ∈ [0,1], depth grows away from camera (we
// look along -Z in eye space). near < far, both positive.
Mat4
projection_matrix_from_fov(const XrFovf &fov, float aspect_w_over_h, float near_z, float far_z)
{
	const float tan_l = std::tan(fov.angleLeft);
	const float tan_r = std::tan(fov.angleRight);
	const float tan_d = std::tan(fov.angleDown);
	const float tan_u = std::tan(fov.angleUp);
	const float tan_w = tan_r - tan_l;
	const float tan_h = tan_u - tan_d;

	Mat4 p{};
	p.m[0]  = 2.0f / tan_w;
	p.m[5]  = 2.0f / tan_h;
	p.m[8]  = (tan_r + tan_l) / tan_w;
	p.m[9]  = (tan_u + tan_d) / tan_h;
	p.m[10] = -far_z / (far_z - near_z);
	p.m[11] = -1.0f;
	p.m[14] = -(far_z * near_z) / (far_z - near_z);

	// Aspect correction: the runtime reports a (near-)square per-eye FOV,
	// but each eye image is 16:10 (2560x1600), so an uncorrected projection
	// stretches geometry horizontally. Derive the horizontal scale from the
	// vertical one and the real viewport aspect, keeping the vertical FOV.
	// Applied identically to both eyes, so the stereo (IPD in the view
	// matrix) is preserved — only the per-eye shape is un-stretched.
	if (aspect_w_over_h > 0.0f) {
		p.m[0] = (2.0f / tan_h) / aspect_w_over_h;
		p.m[8] = 0.0f;  // symmetric Leia FOV → no horizontal skew
	}

	// Vulkan flips Y in clip space vs OpenGL — negate the Y-related
	// terms so triangles aren't upside-down.
	p.m[5]  = -p.m[5];
	p.m[9]  = -p.m[9];
	return p;
}

// Build the display-rig pose from the orbit camera (yaw/pitch). The rig pose
// is the virtual-display-plane pose in the locate space; orbiting rotates the
// viewer about the scene. Position stays at the origin (the runtime places the
// eyes relative to this plane).
XrPosef
build_rig_pose()
{
	const float yaw = g_cam_yaw.load(std::memory_order_relaxed);
	const float pitch = g_cam_pitch.load(std::memory_order_relaxed);
	// Quaternion from yaw (about Y) then pitch (about X): q = qYaw * qPitch.
	const float cy = std::cos(yaw * 0.5f), sy = std::sin(yaw * 0.5f);
	const float cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
	XrPosef pose{};
	pose.orientation.w = cy * cp;
	pose.orientation.x = cy * sp;
	pose.orientation.y = sy * cp;
	pose.orientation.z = -sy * sp;
	pose.position = {0.0f, 0.0f, 0.0f};
	return pose;
}

// Display-local eye distance for the ZDP-anchored clip: z of (rigPose^-1 *
// eyeWorld). Degenerates to pose.position.z at identity rig pose. Mirrors
// RigLocalEyeZ() in cube_handle_vk_win.
float
rig_local_eye_z(const XrPosef &rig, const XrVector3f &eye_world)
{
	const float dx = eye_world.x - rig.position.x;
	const float dy = eye_world.y - rig.position.y;
	const float dz = eye_world.z - rig.position.z;
	const float qx = -rig.orientation.x, qy = -rig.orientation.y;
	const float qz = -rig.orientation.z, qw = rig.orientation.w;
	const float cx = qy * dz - qz * dy + qw * dx;
	const float cy = qz * dx - qx * dz + qw * dy;
	return dz + 2.0f * (qx * cy - qy * cx);
}

// Read a float system property (live-tunable knob), e.g.
//   adb shell setprop debug.dxr.aspect_mult 1.5
// Returns default_val if unset/unparseable. Used to tune the SBS aspect
// multiplier on the device without rebuilding.
static float
get_prop_float(const char *name, float default_val)
{
	char buf[PROP_VALUE_MAX] = {0};
	int n = __system_property_get(name, buf);
	if (n <= 0) {
		return default_val;
	}
	char *end = nullptr;
	float v = strtof(buf, &end);
	return (end != buf) ? v : default_val;
}

// Model matrix for the spinning cube: scale to ~0.08 app units (a sensible
// fraction of the 0.24 virtual-display height the rig anchors to), rotate about
// Y (and slower about X). Sits at the rig origin (the virtual display plane).
// With the runtime view-rig driving the projection, the cube no longer needs an
// app-side forward push — the rig anchors depth to the display plane.
Mat4
cube_model_matrix(float angle)
{
	const float cy = std::cos(angle), sy = std::sin(angle);
	const float ax = angle * 0.5f;
	const float cx = std::cos(ax), sx = std::sin(ax);

	const float kCubeScale = 0.08f;
	Mat4 s = mat4_identity();
	s.m[0] = kCubeScale; s.m[5] = kCubeScale; s.m[10] = kCubeScale;

	Mat4 ry = mat4_identity();  // rotate about Y
	ry.m[0] = cy;  ry.m[2] = -sy;
	ry.m[8] = sy;  ry.m[10] = cy;

	Mat4 rx = mat4_identity();  // rotate about X
	rx.m[5] = cx;  rx.m[6] = sx;
	rx.m[9] = -sx; rx.m[10] = cx;

	// column-major (M·v): scale first, then rotate.
	return mat4_mul(ry, mat4_mul(rx, s));
}

// ─── render pass + pipeline ───────────────────────────────────────────

bool
create_render_pass()
{
	VkAttachmentDescription color = {};
	color.format = g_swapchain_format;
	color.samples = VK_SAMPLE_COUNT_1_BIT;
	color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	// xrAcquireSwapchainImage delivers in COLOR_ATTACHMENT_OPTIMAL per
	// OpenXR spec; xrReleaseSwapchainImage expects it to stay there.
	color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkAttachmentReference color_ref = {};
	color_ref.attachment = 0;
	color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	// Depth attachment for the cube. Cleared each frame, not stored (only
	// needed within the render pass for occlusion).
	VkAttachmentDescription depth = {};
	depth.format = VK_FORMAT_D32_SFLOAT;
	depth.samples = VK_SAMPLE_COUNT_1_BIT;
	depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkAttachmentReference depth_ref = {};
	depth_ref.attachment = 1;
	depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass = {};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &color_ref;
	subpass.pDepthStencilAttachment = &depth_ref;

	VkAttachmentDescription attachments[2] = {color, depth};
	VkRenderPassCreateInfo rpci = {};
	rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	rpci.attachmentCount = 2;
	rpci.pAttachments = attachments;
	rpci.subpassCount = 1;
	rpci.pSubpasses = &subpass;

	VkResult res = vkCreateRenderPass(g_vk_device, &rpci, nullptr, &g_render_pass);
	if (res != VK_SUCCESS) {
		LOGE("vkCreateRenderPass failed: %d", res);
		return false;
	}
	DXR_HW_DBG("render pass created (format=0x%x)", (uint32_t)g_swapchain_format);
	return true;
}

bool
create_pipeline()
{
	// Shader modules. The spirv_shaders() CMake helper generates
	// unsigned-int arrays with idiomatic identifier names from the
	// source path: "shaders/triangle.vert" → "shaders_triangle_vert".
	VkShaderModuleCreateInfo vs_ci = {};
	vs_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	vs_ci.codeSize = sizeof(shaders_cube_vert);
	vs_ci.pCode = shaders_cube_vert;
	if (vkCreateShaderModule(g_vk_device, &vs_ci, nullptr, &g_vs_module) != VK_SUCCESS) {
		LOGE("vkCreateShaderModule(vert) failed");
		return false;
	}

	VkShaderModuleCreateInfo fs_ci = {};
	fs_ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	fs_ci.codeSize = sizeof(shaders_cube_frag);
	fs_ci.pCode = shaders_cube_frag;
	if (vkCreateShaderModule(g_vk_device, &fs_ci, nullptr, &g_fs_module) != VK_SUCCESS) {
		LOGE("vkCreateShaderModule(frag) failed");
		return false;
	}

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = g_vs_module;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = g_fs_module;
	stages[1].pName = "main";

	VkPushConstantRange pc_range = {};
	pc_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	pc_range.offset = 0;
	pc_range.size = sizeof(Mat4);

	VkPipelineLayoutCreateInfo pli = {};
	pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pli.pushConstantRangeCount = 1;
	pli.pPushConstantRanges = &pc_range;
	if (vkCreatePipelineLayout(g_vk_device, &pli, nullptr, &g_pipeline_layout) != VK_SUCCESS) {
		LOGE("vkCreatePipelineLayout failed");
		return false;
	}

	// Vertex input: one interleaved buffer of CubeVertex {vec3 pos; vec3 color;}.
	VkVertexInputBindingDescription vbind = {};
	vbind.binding = 0;
	vbind.stride = sizeof(CubeVertex);
	vbind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	VkVertexInputAttributeDescription vattrs[2] = {};
	vattrs[0].location = 0;  // in_pos
	vattrs[0].binding = 0;
	vattrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
	vattrs[0].offset = offsetof(CubeVertex, pos);
	vattrs[1].location = 1;  // in_color
	vattrs[1].binding = 0;
	vattrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
	vattrs[1].offset = offsetof(CubeVertex, color);

	VkPipelineVertexInputStateCreateInfo vi = {};
	vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = &vbind;
	vi.vertexAttributeDescriptionCount = 2;
	vi.pVertexAttributeDescriptions = vattrs;

	VkPipelineInputAssemblyStateCreateInfo ia = {};
	ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	VkPipelineViewportStateCreateInfo vp = {};
	vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	vp.viewportCount = 1;
	vp.scissorCount = 1;

	VkPipelineRasterizationStateCreateInfo rs = {};
	rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	// No cull — the triangle is drawn world-facing but we may approach
	// it from either side as the head moves; better to keep both sides
	// visible than to debug a missing-triangle problem later.
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
	rs.lineWidth = 1.0f;

	VkPipelineMultisampleStateCreateInfo ms = {};
	ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	VkPipelineColorBlendAttachmentState cba = {};
	cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	// HUD-only pipeline → alpha blend via a DYNAMIC blend constant, so the HUD
	// can draw a translucent panel (low constant alpha) and crisp opaque text
	// (alpha 1) in two passes. CubeVertex carries no alpha, so the per-draw
	// blend constant supplies it (vkCmdSetBlendConstants before each pass).
	cba.blendEnable = VK_TRUE;
	cba.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
	cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
	cba.colorBlendOp = VK_BLEND_OP_ADD;
	cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
	cba.alphaBlendOp = VK_BLEND_OP_ADD;
	VkPipelineColorBlendStateCreateInfo cb = {};
	cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	cb.attachmentCount = 1;
	cb.pAttachments = &cba;

	// Depth test so the spinning cube's near faces occlude its far faces
	// (cube renders correctly regardless of triangle winding — we cull none).
	VkPipelineDepthStencilStateCreateInfo ds = {};
	ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	ds.depthTestEnable = VK_TRUE;
	ds.depthWriteEnable = VK_TRUE;
	ds.depthCompareOp = VK_COMPARE_OP_LESS;

	const VkDynamicState dyn_states[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
	                                      VK_DYNAMIC_STATE_BLEND_CONSTANTS};
	VkPipelineDynamicStateCreateInfo dyn = {};
	dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dyn.dynamicStateCount = 3;
	dyn.pDynamicStates = dyn_states;

	VkGraphicsPipelineCreateInfo gpi = {};
	gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	gpi.stageCount = 2;
	gpi.pStages = stages;
	gpi.pVertexInputState = &vi;
	gpi.pInputAssemblyState = &ia;
	gpi.pViewportState = &vp;
	gpi.pRasterizationState = &rs;
	gpi.pMultisampleState = &ms;
	gpi.pDepthStencilState = &ds;
	gpi.pColorBlendState = &cb;
	gpi.pDynamicState = &dyn;
	gpi.layout = g_pipeline_layout;
	gpi.renderPass = g_render_pass;
	gpi.subpass = 0;

	VkResult pres = vkCreateGraphicsPipelines(g_vk_device, VK_NULL_HANDLE, 1, &gpi, nullptr,
	                                          &g_pipeline);
	if (pres != VK_SUCCESS) {
		LOGE("vkCreateGraphicsPipelines failed: %d", pres);
		return false;
	}
	DXR_HW_DBG("graphics pipeline created");
	return true;
}

// Pick a memory type satisfying `type_bits` with the requested properties.
uint32_t
find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags props)
{
	VkPhysicalDeviceMemoryProperties mp = {};
	vkGetPhysicalDeviceMemoryProperties(g_vk_phys_device, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
		if ((type_bits & (1u << i)) &&
		    (mp.memoryTypes[i].propertyFlags & props) == props) {
			return i;
		}
	}
	return UINT32_MAX;
}

// Upload the cube's 36 vertices (6 faces × 2 triangles, per-face color) into
// a host-visible vertex buffer. Half-extent 0.12 m → ~24 cm cube.
bool
create_cube_vertex_buffer()
{
	const float S = 0.22f;  // half-extent → ~44 cm cube (bigger = easier to see)
	const float R[3] = {0.90f, 0.20f, 0.20f};  // front  red
	const float G[3] = {0.20f, 0.80f, 0.30f};  // back   green
	const float B[3] = {0.25f, 0.45f, 0.95f};  // left   blue
	const float Y[3] = {0.95f, 0.80f, 0.20f};  // right  yellow
	const float M[3] = {0.85f, 0.35f, 0.85f};  // top    magenta
	const float C[3] = {0.25f, 0.80f, 0.85f};  // bottom cyan
#define V(x, y, z, c) {{x, y, z}, {c[0], c[1], c[2]}}
	const CubeVertex verts[kCubeVertexCount] = {
	    // front (z+)
	    V(-S, -S, S, R), V(S, -S, S, R), V(S, S, S, R),
	    V(-S, -S, S, R), V(S, S, S, R), V(-S, S, S, R),
	    // back (z-)
	    V(S, -S, -S, G), V(-S, -S, -S, G), V(-S, S, -S, G),
	    V(S, -S, -S, G), V(-S, S, -S, G), V(S, S, -S, G),
	    // left (x-)
	    V(-S, -S, -S, B), V(-S, -S, S, B), V(-S, S, S, B),
	    V(-S, -S, -S, B), V(-S, S, S, B), V(-S, S, -S, B),
	    // right (x+)
	    V(S, -S, S, Y), V(S, -S, -S, Y), V(S, S, -S, Y),
	    V(S, -S, S, Y), V(S, S, -S, Y), V(S, S, S, Y),
	    // top (y+)
	    V(-S, S, S, M), V(S, S, S, M), V(S, S, -S, M),
	    V(-S, S, S, M), V(S, S, -S, M), V(-S, S, -S, M),
	    // bottom (y-)
	    V(-S, -S, -S, C), V(S, -S, -S, C), V(S, -S, S, C),
	    V(-S, -S, -S, C), V(S, -S, S, C), V(-S, -S, S, C),
	};
#undef V

	const VkDeviceSize size = sizeof(verts);
	VkBufferCreateInfo bci = {};
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size = size;
	bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(g_vk_device, &bci, nullptr, &g_cube_vbuf) != VK_SUCCESS) {
		LOGE("create cube vertex buffer failed");
		return false;
	}

	VkMemoryRequirements req = {};
	vkGetBufferMemoryRequirements(g_vk_device, g_cube_vbuf, &req);
	VkMemoryAllocateInfo mai = {};
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = find_memory_type(
	    req.memoryTypeBits,
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (mai.memoryTypeIndex == UINT32_MAX ||
	    vkAllocateMemory(g_vk_device, &mai, nullptr, &g_cube_vbuf_mem) != VK_SUCCESS) {
		LOGE("alloc cube vertex buffer memory failed");
		return false;
	}
	vkBindBufferMemory(g_vk_device, g_cube_vbuf, g_cube_vbuf_mem, 0);

	void *mapped = nullptr;
	if (vkMapMemory(g_vk_device, g_cube_vbuf_mem, 0, size, 0, &mapped) != VK_SUCCESS) {
		LOGE("map cube vertex buffer failed");
		return false;
	}
	memcpy(mapped, verts, (size_t)size);
	vkUnmapMemory(g_vk_device, g_cube_vbuf_mem);
	DXR_HW_DBG("cube vertex buffer uploaded (%u verts)", kCubeVertexCount);
	return true;
}

// ─── in-frame HUD ─────────────────────────────────────────────────────────
// 3x5 bitmap font. Each glyph is 5 rows; the low 3 bits of each row are the
// pixels (bit2 = left column, bit0 = right column). Only the characters the
// HUD needs are defined; unknown chars render blank.
static void
hud_glyph(char ch, uint8_t rows[5])
{
	auto set = [&](uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint8_t e) {
		rows[0] = a; rows[1] = b; rows[2] = c; rows[3] = d; rows[4] = e;
	};
	switch (ch) {
	case '0': set(0b111,0b101,0b101,0b101,0b111); break;
	case '1': set(0b010,0b110,0b010,0b010,0b111); break;
	case '2': set(0b111,0b001,0b111,0b100,0b111); break;
	case '3': set(0b111,0b001,0b111,0b001,0b111); break;
	case '4': set(0b101,0b101,0b111,0b001,0b001); break;
	case '5': set(0b111,0b100,0b111,0b001,0b111); break;
	case '6': set(0b111,0b100,0b111,0b101,0b111); break;
	case '7': set(0b111,0b001,0b010,0b010,0b010); break;
	case '8': set(0b111,0b101,0b111,0b101,0b111); break;
	case '9': set(0b111,0b101,0b111,0b001,0b111); break;
	case '-': set(0b000,0b000,0b111,0b000,0b000); break;
	case '+': set(0b000,0b010,0b111,0b010,0b000); break;
	case '.': set(0b000,0b000,0b000,0b000,0b010); break;
	case ':': set(0b000,0b010,0b000,0b010,0b000); break;
	case 'B': set(0b110,0b101,0b110,0b101,0b110); break;
	case 'G': set(0b011,0b100,0b101,0b101,0b011); break;
	case 'E': set(0b111,0b100,0b110,0b100,0b111); break;
	case 'X': set(0b101,0b101,0b010,0b101,0b101); break;
	case 'Y': set(0b101,0b101,0b010,0b010,0b010); break;
	case 'Z': set(0b111,0b001,0b010,0b100,0b111); break;
	default:  set(0,0,0,0,0); break;
	}
}

static uint32_t
hud_quad(CubeVertex *v, uint32_t n, float x0, float y0, float x1, float y1,
         float z, const float c[3])
{
	if (n + 6 > kHudMaxVerts) {
		return n;
	}
	const CubeVertex a = {{x0, y0, z}, {c[0], c[1], c[2]}};
	const CubeVertex b = {{x1, y0, z}, {c[0], c[1], c[2]}};
	const CubeVertex d = {{x1, y1, z}, {c[0], c[1], c[2]}};
	const CubeVertex e = {{x0, y1, z}, {c[0], c[1], c[2]}};
	v[n++] = a; v[n++] = b; v[n++] = d;
	v[n++] = a; v[n++] = d; v[n++] = e;
	return n;
}

// Draw a string at (ox, oy) top-left in NDC. px/py = glyph pixel size.
static uint32_t
hud_text(CubeVertex *v, uint32_t n, const char *s, float ox, float oy,
         float px, float py, float z, const float c[3])
{
	float x = ox;
	for (; *s != '\0'; ++s) {
		if (*s != ' ') {
			uint8_t g[5];
			hud_glyph(*s, g);
			for (int r = 0; r < 5; ++r) {
				for (int col = 0; col < 3; ++col) {
					if (g[r] & (1 << (2 - col))) {
						const float qx = x + col * px;
						const float qy = oy + r * py;
						n = hud_quad(v, n, qx, qy, qx + px * 0.85f,
						             qy + py * 0.85f, z, c);
					}
				}
			}
		}
		x += 4.0f * px;  // 3-wide glyph + 1 column gap
	}
	return n;
}

// Format a float as a signed 2-decimal string ("-0.03"). No locale, no libm.
static void
hud_ftoa(float val, char *buf, size_t cap)
{
	const int neg = val < 0.0f;
	float a = neg ? -val : val;
	int whole = (int)a;
	int frac = (int)((a - (float)whole) * 100.0f + 0.5f);
	if (frac >= 100) { whole += 1; frac -= 100; }
	snprintf(buf, cap, "%s%d.%02d", neg ? "-" : "+", whole, frac);
}

// Vertex count of the translucent panel (drawn first, at low blend-constant
// alpha); the remaining verts are the opaque text. Set by build_hud().
static uint32_t g_hud_panel_n = 0;

// A filled axis-aligned rect (two triangles) in HUD NDC.
static uint32_t
hud_rect(CubeVertex *v, uint32_t n, float x0, float y0, float x1, float y1, float z, const float c[3])
{
	return hud_quad(v, n, x0, y0, x1, y1, z, c);
}

// Build the HUD geometry into the mapped buffer. Returns the total vertex count;
// g_hud_panel_n holds the leading panel-vertex count for the two-pass draw.
// Layout: a slick rounded translucent panel in the TOP-LEFT, with the tracked
// face position (display-space, the face-dot frame) as three stacked lines.
static uint32_t
build_hud(CubeVertex *v)
{
	uint32_t n = 0;

	// When the stb_truetype font is up, the HUD is drawn entirely by hud_font
	// (a smooth SDF panel sized snugly to the text + crisp glyphs) — this
	// legacy bitmap path (panel + 3x5 glyphs) is only the no-font fallback.
	if (g_hud_font.ready) {
		g_hud_panel_n = 0;
		return 0;
	}

	// ── translucent rounded panel (drawn first; alpha comes from the blend
	// constant). The HUD MVP v-flips Y, so y≈-0.80 is the TOP edge; the panel
	// hangs down from there into the top-left corner. Rounded corners are the
	// classic two-overlapping-rects approximation (a horizontal band + a
	// vertical band leaves the four corners chamfered/rounded).
	const float panel[3] = {0.05f, 0.06f, 0.12f};  // dark slate
	const float x0 = -0.975f, y0 = -0.800f;        // top-left
	const float x1 = -0.545f, y1 = -0.520f;        // bottom-right
	const float r = 0.028f;                         // corner radius
	const float zp = 0.050f;
	n = hud_rect(v, n, x0,     y0 + r, x1,     y1 - r, zp, panel);  // full width
	n = hud_rect(v, n, x0 + r, y0,     x1 - r, y1,     zp, panel);  // full height
	g_hud_panel_n = n;

	// ── face-position readout. When the stb_truetype font is up it draws the
	// text (crisp, separate pipeline) — see the HUD draw block. Only fall back to
	// the legacy bitmap glyphs here if the font failed to initialize.
	if (!g_hud_font.ready) {
		const float px = 0.012f, py = 0.015f;
		const float zt = 0.045f;  // slightly nearer than the panel → drawn on top
		const float white[3] = {0.92f, 0.96f, 1.0f};
		char b[16], line[24];
		hud_ftoa(g_eye_x.load(std::memory_order_relaxed), b, sizeof b);
		snprintf(line, sizeof line, "X %s", b);
		n = hud_text(v, n, line, x0 + 0.035f, -0.775f, px, py, zt, white);
		hud_ftoa(g_eye_y.load(std::memory_order_relaxed), b, sizeof b);
		snprintf(line, sizeof line, "Y %s", b);
		n = hud_text(v, n, line, x0 + 0.035f, -0.695f, px, py, zt, white);
		hud_ftoa(g_eye_z.load(std::memory_order_relaxed), b, sizeof b);
		snprintf(line, sizeof line, "Z %s", b);
		n = hud_text(v, n, line, x0 + 0.035f, -0.615f, px, py, zt, white);
	}
	return n;
}

static bool
create_hud_buffer()
{
	const VkDeviceSize size = sizeof(CubeVertex) * kHudMaxVerts;
	VkBufferCreateInfo bci = {};
	bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bci.size = size;
	bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
	bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(g_vk_device, &bci, nullptr, &g_hud_vbuf) != VK_SUCCESS) {
		LOGE("create HUD vertex buffer failed");
		return false;
	}
	VkMemoryRequirements req = {};
	vkGetBufferMemoryRequirements(g_vk_device, g_hud_vbuf, &req);
	VkMemoryAllocateInfo mai = {};
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = find_memory_type(
	    req.memoryTypeBits,
	    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (mai.memoryTypeIndex == UINT32_MAX ||
	    vkAllocateMemory(g_vk_device, &mai, nullptr, &g_hud_vbuf_mem) != VK_SUCCESS) {
		LOGE("alloc HUD vertex buffer memory failed");
		return false;
	}
	vkBindBufferMemory(g_vk_device, g_hud_vbuf, g_hud_vbuf_mem, 0);
	// Persistently mapped (host-coherent): rebuilt each frame in record_atlas.
	if (vkMapMemory(g_vk_device, g_hud_vbuf_mem, 0, size, 0, &g_hud_mapped) != VK_SUCCESS) {
		LOGE("map HUD vertex buffer failed");
		return false;
	}
	DXR_HW_DBG("HUD vertex buffer created (%u verts max)", kHudMaxVerts);
	return true;
}

// Create the atlas depth target (D32_SFLOAT, full atlas dimensions). One depth
// image is shared by every tile — tiles don't overlap, so a single per-frame
// LOAD_OP_CLEAR is correct.
bool
create_atlas_depth()
{
	VkImageCreateInfo ici = {};
	ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = VK_FORMAT_D32_SFLOAT;
	ici.extent = {g_atlas.width, g_atlas.height, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (vkCreateImage(g_vk_device, &ici, nullptr, &g_atlas.depth_image) != VK_SUCCESS) {
		LOGE("create atlas depth image failed");
		return false;
	}

	VkMemoryRequirements req = {};
	vkGetImageMemoryRequirements(g_vk_device, g_atlas.depth_image, &req);
	VkMemoryAllocateInfo mai = {};
	mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	mai.allocationSize = req.size;
	mai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (mai.memoryTypeIndex == UINT32_MAX ||
	    vkAllocateMemory(g_vk_device, &mai, nullptr, &g_atlas.depth_mem) != VK_SUCCESS) {
		LOGE("alloc atlas depth memory failed");
		return false;
	}
	vkBindImageMemory(g_vk_device, g_atlas.depth_image, g_atlas.depth_mem, 0);

	VkImageViewCreateInfo dvci = {};
	dvci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	dvci.image = g_atlas.depth_image;
	dvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	dvci.format = VK_FORMAT_D32_SFLOAT;
	dvci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
	if (vkCreateImageView(g_vk_device, &dvci, nullptr, &g_atlas.depth_view) != VK_SUCCESS) {
		LOGE("create atlas depth view failed");
		return false;
	}
	return true;
}

// One framebuffer per atlas swapchain image (color = atlas image, depth =
// shared atlas depth). The single render pass spans the whole atlas; per-tile
// rendering is done with viewport/scissor.
bool
create_view_framebuffers()
{
	if (!create_atlas_depth()) {
		return false;
	}
	for (uint32_t i = 0; i < g_atlas.image_count; ++i) {
		VkImageViewCreateInfo ivci = {};
		ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		ivci.image = g_atlas.images[i].image;
		ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		ivci.format = g_swapchain_format;
		ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		if (vkCreateImageView(g_vk_device, &ivci, nullptr, &g_atlas.image_views[i]) != VK_SUCCESS) {
			LOGE("vkCreateImageView atlas img=%u failed", i);
			return false;
		}

		VkImageView fb_attachments[2] = {g_atlas.image_views[i], g_atlas.depth_view};
		VkFramebufferCreateInfo fbi = {};
		fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		fbi.renderPass = g_render_pass;
		fbi.attachmentCount = 2;
		fbi.pAttachments = fb_attachments;
		fbi.width = g_atlas.width;
		fbi.height = g_atlas.height;
		fbi.layers = 1;
		if (vkCreateFramebuffer(g_vk_device, &fbi, nullptr, &g_atlas.framebuffers[i]) != VK_SUCCESS) {
			LOGE("vkCreateFramebuffer atlas img=%u failed", i);
			return false;
		}
	}
	return true;
}

void
destroy_view_framebuffers()
{
	for (uint32_t i = 0; i < 8; ++i) {
		if (g_atlas.framebuffers[i] != VK_NULL_HANDLE) {
			vkDestroyFramebuffer(g_vk_device, g_atlas.framebuffers[i], nullptr);
			g_atlas.framebuffers[i] = VK_NULL_HANDLE;
		}
		if (g_atlas.image_views[i] != VK_NULL_HANDLE) {
			vkDestroyImageView(g_vk_device, g_atlas.image_views[i], nullptr);
			g_atlas.image_views[i] = VK_NULL_HANDLE;
		}
	}
	if (g_atlas.depth_view != VK_NULL_HANDLE) {
		vkDestroyImageView(g_vk_device, g_atlas.depth_view, nullptr);
		g_atlas.depth_view = VK_NULL_HANDLE;
	}
	if (g_atlas.depth_image != VK_NULL_HANDLE) {
		vkDestroyImage(g_vk_device, g_atlas.depth_image, nullptr);
		g_atlas.depth_image = VK_NULL_HANDLE;
	}
	if (g_atlas.depth_mem != VK_NULL_HANDLE) {
		vkFreeMemory(g_vk_device, g_atlas.depth_mem, nullptr);
		g_atlas.depth_mem = VK_NULL_HANDLE;
	}
}

void
destroy_pipeline()
{
	if (g_pipeline != VK_NULL_HANDLE) {
		vkDestroyPipeline(g_vk_device, g_pipeline, nullptr);
		g_pipeline = VK_NULL_HANDLE;
	}
	if (g_pipeline_layout != VK_NULL_HANDLE) {
		vkDestroyPipelineLayout(g_vk_device, g_pipeline_layout, nullptr);
		g_pipeline_layout = VK_NULL_HANDLE;
	}
	if (g_fs_module != VK_NULL_HANDLE) {
		vkDestroyShaderModule(g_vk_device, g_fs_module, nullptr);
		g_fs_module = VK_NULL_HANDLE;
	}
	if (g_vs_module != VK_NULL_HANDLE) {
		vkDestroyShaderModule(g_vk_device, g_vs_module, nullptr);
		g_vs_module = VK_NULL_HANDLE;
	}
}

void
destroy_render_pass()
{
	if (g_render_pass != VK_NULL_HANDLE) {
		vkDestroyRenderPass(g_vk_device, g_render_pass, nullptr);
		g_render_pass = VK_NULL_HANDLE;
	}
}

// ─── per-frame draw ───────────────────────────────────────────────────
//
// Render the active mode's `view_count` views into TILES of the single atlas
// image, in ONE render pass, then submit once. Mirrors RenderScene() in
// cube_handle_vk_win: LOAD_OP_CLEAR blacks the whole atlas, then each view
// draws into its tile via a viewport/scissor offset. The HUD is drawn per-tile
// at zero disparity (same screen-plane NDC in every tile). `render_w/h` are the
// per-tile dims; `cols/rows` the atlas grid; `swap_lr` is the ghost-diagnosis
// eye-swap knob.
bool
record_atlas(uint32_t image_idx, const XrView *views, uint32_t view_count, uint32_t render_w,
             uint32_t render_h, uint32_t cols, uint32_t rows, bool swap_lr, const XrPosef &rig_pose)
{
	(void)rows;
	VkCommandBufferAllocateInfo ai = {};
	ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	ai.commandPool = g_app_cmd_pool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 1;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	if (vkAllocateCommandBuffers(g_vk_device, &ai, &cmd) != VK_SUCCESS) {
		return false;
	}

	VkCommandBufferBeginInfo bi = {};
	bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(cmd, &bi);

	// LOAD_OP_CLEAR fills the WHOLE atlas (every tile) once at render-pass
	// begin. Dark blue, matching cube_handle_vk_win's scene background.
	VkClearValue clears[2] = {};
	clears[0].color.float32[0] = 0.05f;
	clears[0].color.float32[1] = 0.05f;
	clears[0].color.float32[2] = 0.25f;
	clears[0].color.float32[3] = 1.0f;
	clears[1].depthStencil.depth = 1.0f;

	VkRenderPassBeginInfo rpbi = {};
	rpbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	rpbi.renderPass = g_render_pass;
	rpbi.framebuffer = g_atlas.framebuffers[image_idx];
	rpbi.renderArea.offset = {0, 0};
	rpbi.renderArea.extent = {g_atlas.width, g_atlas.height};
	rpbi.clearValueCount = 2;
	rpbi.pClearValues = clears;
	vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

	// CANONICAL render (CNSDK model): the app draws its source views in the
	// natural orientation and applies NO per-orientation compensation. Device
	// rotation is the weaver/DP's job (the Leia interlacer rotates the weave
	// pattern; the DisplayXR vk_native compositor + DP do the same). The old
	// app-side aspect_mult / yscale / HUD-rotation knobs were the wrong layer
	// and are gone. Projection comes from the runtime via XR_DXR_view_rig.
	const float rig_vh = kRigVirtualDisplayHeight;  // scaleFactor = 1.0

	// Scene model matrices (tile-independent — only view/proj vary per tile).
	// Cube: unit cube → 0.06 m, spun about Y, base resting on the grid (centre
	// lifted to y = 0.03). Grid: scaled 0.05 and lifted so its y=-1 line plane
	// sits at the floor (y = 0). Column-major (M·v). Mirrors RenderScene() in
	// cube_handle_vk_win.
	// Accumulated spin angle — advances only when not being touched, so the
	// cube holds still during a drag-orbit and resumes the spin smoothly.
	if (!g_touching.load(std::memory_order_relaxed)) {
		g_spin_angle += 0.02f;
	}
	const float angle = g_spin_angle;
	const float ca = std::cos(angle), sa = std::sin(angle);
	Mat4 cube_rot = mat4_identity();
	cube_rot.m[0] = ca;  cube_rot.m[2] = -sa;
	cube_rot.m[8] = sa;  cube_rot.m[10] = ca;
	const float kCubeSize = 0.06f;
	Mat4 cube_scale = mat4_identity();
	cube_scale.m[0] = cube_scale.m[5] = cube_scale.m[10] = kCubeSize;
	Mat4 cube_trans = mat4_identity();
	cube_trans.m[13] = kCubeSize * 0.5f;  // base on the grid floor
	const Mat4 cube_model = mat4_mul(cube_trans, mat4_mul(cube_scale, cube_rot));
	const Mat4 cube_model_normals = mat4_mul(cube_scale, cube_rot);  // for lighting

	const float kGridScale = 0.05f;
	Mat4 grid_scale = mat4_identity();
	grid_scale.m[0] = grid_scale.m[5] = grid_scale.m[10] = kGridScale;
	Mat4 grid_trans = mat4_identity();
	grid_trans.m[13] = kGridScale;  // lift the y=-1 plane to y=0
	const Mat4 grid_world = mat4_mul(grid_trans, grid_scale);
	const float grid_color[4] = {0.3f, 0.3f, 0.35f, 1.0f};

	// Build the HUD geometry ONCE (identical in every tile) — rebuilding it
	// into the persistently-mapped buffer mid-pass would be a write-while-read
	// hazard against an already-recorded tile draw. Drawn upright in tile-NDC.
	uint32_t hud_n = 0;
	const Mat4 hud_mvp = mat4_identity();
	const bool hud_ok = (g_hud_vbuf != VK_NULL_HANDLE && g_hud_mapped != nullptr);
	if (hud_ok) {
		hud_n = build_hud(reinterpret_cast<CubeVertex *>(g_hud_mapped));
	}

	// Build the crisp stb_truetype text once per frame (drawn per tile below,
	// on top of the translucent panel). The panel top-left is (-0.975,-0.800).
	uint32_t hud_text_n = 0;
	float hud_bbox[4] = {0, 0, 0, 0};  // tight text bounds (NDC) → snug panel
	if (g_hud_font.ready) {
		char xb[16], yb[16], zb[16], block[80];
		hud_ftoa(g_eye_x.load(std::memory_order_relaxed), xb, sizeof xb);
		hud_ftoa(g_eye_y.load(std::memory_order_relaxed), yb, sizeof yb);
		hud_ftoa(g_eye_z.load(std::memory_order_relaxed), zb, sizeof zb);
		snprintf(block, sizeof block, "X %s\nY %s\nZ %s", xb, yb, zb);
		hud_text_n = hud_font_build(g_hud_font, block, -0.945f, -0.775f, 0.0013f, 0.085f, hud_bbox);
	}

	for (uint32_t i = 0; i < view_count; ++i) {
		// Place this view in its atlas tile (column-major within the grid).
		const uint32_t tile_x = (cols > 0) ? (i % cols) : 0;
		const uint32_t tile_y = (cols > 0) ? (i / cols) : 0;
		const float vp_x = (float)(tile_x * render_w);
		const float vp_y = (float)(tile_y * render_h);

		VkViewport vp = {};
		vp.x = vp_x;
		vp.y = vp_y;
		vp.width = (float)render_w;
		vp.height = (float)render_h;
		vp.minDepth = 0.0f;
		vp.maxDepth = 1.0f;
		vkCmdSetViewport(cmd, 0, 1, &vp);

		VkRect2D scissor = {};
		scissor.offset = {(int32_t)vp_x, (int32_t)vp_y};
		scissor.extent = {render_w, render_h};
		vkCmdSetScissor(cmd, 0, 1, &scissor);

		// Render eye `src` into tile `i` (src != i only under the swap knob).
		const uint32_t src = swap_lr ? (view_count - 1 - i) : i;
		const XrView &view = views[src];

		// Consume the runtime's render-ready XrView{pose, fov}: the rig already
		// baked the off-axis (Kooima) FOV, so pass it straight through (aspect
		// = -1 → no app-side aspect override / skew). Only the clip policy stays
		// app-side (FOV is clip-independent): ZDP-anchored near/far about the
		// virtual display plane — near = ez - vH, far = ez + 1000·vH, where ez
		// is the rig-local z of this eye.
		const Mat4 view_mat = view_matrix_from_pose(view.pose);
		const float ez = rig_local_eye_z(rig_pose, view.pose.position);
		float near_z = (ez - rig_vh > 0.001f) ? (ez - rig_vh) : 0.001f;
		float far_z = ez + 1000.0f * rig_vh;
		const Mat4 proj_mat = projection_matrix_from_fov(view.fov, -1.0f, near_z, far_z);
		const Mat4 view_proj = mat4_mul(proj_mat, view_mat);

		// Floor grid, then the textured crate cube (each binds its own
		// pipeline). Depth test resolves occlusion; the grid is behind/below.
		const Mat4 grid_mvp = mat4_mul(view_proj, grid_world);
		crate_scene_draw_grid(g_scene, cmd, grid_mvp.m, grid_color);
		const Mat4 cube_mvp = mat4_mul(view_proj, cube_model);
		crate_scene_draw_cube(g_scene, cmd, cube_mvp.m, cube_model_normals.m);

		// In-frame HUD over the scene in this tile (identity MVP → screen-space
		// NDC; zero disparity so it sits at the screen plane). Uses the app's
		// simple pos+color pipeline, re-bound here after the scene pipelines.
		if (hud_ok && hud_n > 0) {
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipeline);
			vkCmdPushConstants(cmd, g_pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
			                   sizeof(hud_mvp), &hud_mvp);
			VkDeviceSize hud_off = 0;
			vkCmdBindVertexBuffers(cmd, 0, 1, &g_hud_vbuf, &hud_off);
			// Pass 1: translucent panel (blend-constant alpha < 1).
			if (g_hud_panel_n > 0) {
				const float panel_alpha[4] = {0.0f, 0.0f, 0.0f, 0.62f};
				vkCmdSetBlendConstants(cmd, panel_alpha);
				vkCmdDraw(cmd, g_hud_panel_n, 1, 0, 0);
			}
			// Pass 2 (fallback only): legacy bitmap text on top, when the
			// stb_truetype font isn't available.
			if (!g_hud_font.ready && hud_n > g_hud_panel_n) {
				const float text_alpha[4] = {0.0f, 0.0f, 0.0f, 1.0f};
				vkCmdSetBlendConstants(cmd, text_alpha);
				vkCmdDraw(cmd, hud_n - g_hud_panel_n, 1, g_hud_panel_n, 0);
			}
		}

		// Smooth SDF panel sized snugly to the text, then crisp glyphs on top.
		if (g_hud_font.ready && hud_text_n > 0) {
			const float padx = 0.030f, pady = 0.024f;  // margin around the text
			const float pcol[4] = {0.05f, 0.06f, 0.12f, 0.72f};
			hud_font_draw_panel(g_hud_font, cmd, hud_mvp.m, hud_bbox[0] - padx, hud_bbox[1] - pady,
			                    hud_bbox[2] + padx, hud_bbox[3] + pady, 0.022f, 0.0045f, pcol);
			const float text_col[4] = {0.92f, 0.96f, 1.0f, 1.0f};
			hud_font_draw(g_hud_font, cmd, hud_mvp.m, text_col, hud_text_n);
		}
	}

	vkCmdEndRenderPass(cmd);
	vkEndCommandBuffer(cmd);

	VkSubmitInfo si = {};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cmd;
	VkResult res = vkQueueSubmit(g_vk_queue, 1, &si, VK_NULL_HANDLE);
	if (res == VK_SUCCESS) {
		// Host stall — acceptable for this test app; xrEndFrame needs the
		// atlas image ready and there's no per-frame fence wired yet.
		vkQueueWaitIdle(g_vk_queue);
	}
	vkFreeCommandBuffers(g_vk_device, g_app_cmd_pool, 1, &cmd);

	DXR_HW_DBG_ONCE("record_atlas: first successful tiled-atlas submit");
	return res == VK_SUCCESS;
}

void
handle_session_state(XrSessionState new_state)
{
	g_session_state = new_state;
	switch (new_state) {
	case XR_SESSION_STATE_READY: {
		XrSessionBeginInfo begin = {};
		begin.type = XR_TYPE_SESSION_BEGIN_INFO;
		begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		XrResult res = xrBeginSession(g_session, &begin);
		log_xr_result("xrBeginSession", res);
		if (res == XR_SUCCESS) {
			g_session_running = true;
		}
		break;
	}
	case XR_SESSION_STATE_STOPPING: {
		XrResult res = xrEndSession(g_session);
		log_xr_result("xrEndSession", res);
		g_session_running = false;
		break;
	}
	case XR_SESSION_STATE_EXITING:
	case XR_SESSION_STATE_LOSS_PENDING:
		g_exit_requested = true;
		break;
	default:
		break;
	}
}

void
poll_xr_events()
{
	for (;;) {
		XrEventDataBuffer ev = {};
		ev.type = XR_TYPE_EVENT_DATA_BUFFER;
		XrResult res = xrPollEvent(g_instance, &ev);
		if (res == XR_EVENT_UNAVAILABLE) {
			break;
		}
		if (res != XR_SUCCESS) {
			log_xr_result("xrPollEvent", res);
			break;
		}
		if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
			const auto *e = reinterpret_cast<const XrEventDataSessionStateChanged *>(&ev);
			if (e->session == g_session) {
				LOGI("session state -> %d", (int)e->state);
				handle_session_state(e->state);
			}
		} else if (ev.type == XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_DXR) {
			// The runtime's authoritative confirmation that the active mode
			// changed (via xrRequestDisplayRenderingModeDXR or a vendor-side
			// switch). Adopt it: drives the per-frame view count + tile layout.
			const auto *e = reinterpret_cast<const XrEventDataRenderingModeChangedDXR *>(&ev);
			if (e->session == g_session && e->currentModeIndex < g_mode_count) {
				g_current_mode.store(e->currentModeIndex, std::memory_order_relaxed);
				LOGI("rendering mode -> %u (%s): %u views, tiles %ux%u", e->currentModeIndex,
				     g_modes[e->currentModeIndex].name, g_modes[e->currentModeIndex].view_count,
				     g_modes[e->currentModeIndex].tile_columns,
				     g_modes[e->currentModeIndex].tile_rows);
			}
		} else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
			LOGW("instance loss pending — exiting");
			g_exit_requested = true;
		}
	}
}

// Request an absolute rendering-mode switch. The runtime confirms via
// XrEventDataRenderingModeChangedDXR (handled in poll_xr_events), which updates
// g_current_mode — so we don't pre-set it here.
void
request_mode(uint32_t idx)
{
	if (g_pfnRequestMode == nullptr || g_mode_count == 0 || idx >= g_mode_count) {
		return;
	}
	if (!g_modes[idx].requestable) {
		LOGW("rendering mode %u (%s) is not requestable", idx, g_modes[idx].name);
		return;
	}
	LOGI("requesting rendering mode %u (%s)", idx, g_modes[idx].name);
	XrResult res = g_pfnRequestMode(g_session, idx);
	log_xr_result("xrRequestDisplayRenderingModeDXR", res);
}

// Advance to the next requestable mode (double-tap on device).
void
cycle_mode()
{
	if (g_mode_count == 0) {
		return;
	}
	uint32_t start = g_current_mode.load(std::memory_order_relaxed);
	for (uint32_t step = 1; step <= g_mode_count; ++step) {
		uint32_t cand = (start + step) % g_mode_count;
		if (g_modes[cand].requestable) {
			request_mode(cand);
			return;
		}
	}
}

bool
render_frame()
{
#ifdef CUBE_HAVE_ANDROID_SURFACE_BINDING
	// #1037/#1033: hand the runtime this window's on-panel rect. De-duplicated
	// against the UI thread's sample sequence, so this is a couple of atomic
	// loads on an unchanged frame.
	push_window_geometry();
#endif

	// Mode switch via adb: `setprop debug.dxr.mode N` requests mode N
	// (absolute). Re-request only on change so the runtime isn't spammed.
	{
		static int last_prop_mode = -1;
		int want = (int)get_prop_float("debug.dxr.mode", -1.0f);
		if (want >= 0 && want != last_prop_mode) {
			last_prop_mode = want;
			request_mode((uint32_t)want);
		}
	}

	// #522 test hook: eye-tracking mode via adb:
	//   `setprop debug.dxr.eyemode 0` → MANAGED (vendor auto-2D on tracking loss)
	//   `setprop debug.dxr.eyemode 1` → MANUAL  (app owns 2D⇄3D; immediate isTracking)
	{
		static int last_eyemode = -1;
		int want = (int)get_prop_float("debug.dxr.eyemode", -1.0f);
		if (want >= 0 && want != last_eyemode && g_pfnRequestEyeMode != nullptr) {
			last_eyemode = want;
			XrResult r = g_pfnRequestEyeMode(g_session, (XrEyeTrackingModeDXR)want);
			log_xr_result("xrRequestEyeTrackingModeDXR", r);
		}
	}

	// Orbit camera also driveable from adb for testing / as a fallback while
	// touch-to-app routing is sorted (the runtime's display overlay currently
	// consumes touch). Degrees; sentinel -999 = unset (use the touch value).
	//   adb shell setprop debug.dxr.cam_yaw 30 ; debug.dxr.cam_pitch -15
	{
		const float yaw_deg = get_prop_float("debug.dxr.cam_yaw", -999.0f);
		const float pitch_deg = get_prop_float("debug.dxr.cam_pitch", -999.0f);
		const float kDeg2Rad = 3.14159265f / 180.0f;
		if (yaw_deg > -990.0f) {
			g_cam_yaw.store(yaw_deg * kDeg2Rad, std::memory_order_relaxed);
		}
		if (pitch_deg > -990.0f) {
			g_cam_pitch.store(pitch_deg * kDeg2Rad, std::memory_order_relaxed);
		}
	}

	XrFrameWaitInfo wait_info = {};
	wait_info.type = XR_TYPE_FRAME_WAIT_INFO;
	XrFrameState frame_state = {};
	frame_state.type = XR_TYPE_FRAME_STATE;
	XrResult res = xrWaitFrame(g_session, &wait_info, &frame_state);
	if (res != XR_SUCCESS) {
		log_xr_result("xrWaitFrame", res);
		return false;
	}

	XrFrameBeginInfo begin_info = {};
	begin_info.type = XR_TYPE_FRAME_BEGIN_INFO;
	res = xrBeginFrame(g_session, &begin_info);
	if (res != XR_SUCCESS) {
		log_xr_result("xrBeginFrame", res);
		return false;
	}

	XrCompositionLayerProjectionView projection_views[kMaxViews] = {};
	bool rendered = false;
	// Declared here (not inside the shouldRender block) because the SAME
	// XrDisplayZoneDXR instance must be chained on the projection layer at
	// xrEndFrame, below.
	ZoneLayout zl = {};
	XrDisplayZoneDXR zone3d = {XR_TYPE_DISPLAY_ZONE_DXR};
	XrDisplayRigDXR display_rig = {XR_TYPE_DISPLAY_RIG_DXR};
	// Active mode → how many views to submit + the atlas tile layout. The
	// app renders/submits the ACTIVE mode's view count; xrLocateViews still
	// needs capacity for the MAX (g_max_view_count) and returns all of them.
	const uint32_t submit_views = active_view_count();
	uint32_t render_w = 0, render_h = 0, cols = 1, rows = 1;
	active_tile_dims(&render_w, &render_h, &cols, &rows);
	if (frame_state.shouldRender) {
		XrViewState view_state = {};
		view_state.type = XR_TYPE_VIEW_STATE;
		// Request the RAW display-space eyes the DP reported (display-center
		// meters, +X right +Y up, already orientation-rotated). This is the
		// face-tracking position — the dot you'd draw mirroring the user's head —
		// independent of the orbit camera (unlike the world-space view poses).
		XrViewDisplayRawDXR view_raw = {XR_TYPE_VIEW_DISPLAY_RAW_DXR};
		if (g_has_view_rig) {
			view_state.next = &view_raw;
		}
		XrViewLocateInfo locate_info = {};
		locate_info.type = XR_TYPE_VIEW_LOCATE_INFO;
		locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locate_info.displayTime = frame_state.predictedDisplayTime;
		locate_info.space = g_app_space;

		// XR_DXR_view_rig: drive the runtime's display-centric rig so it returns
		// render-ready off-axis XrView{pose, fov} (the Kooima math is the
		// runtime's job now). The rig pose is the orbit camera; virtualDisplay-
		// Height = 0.24 app units; ipd/parallax/perspective at neutral 1.0.
		const XrPosef rig_pose = build_rig_pose();
		display_rig.pose = rig_pose;
		display_rig.virtualDisplayHeight = kRigVirtualDisplayHeight;
		display_rig.ipdFactor = 1.0f;
		display_rig.parallaxFactor = 1.0f;
		display_rig.perspectiveFactor = 1.0f;
		// XR_DXR_display_zones: the ZONE-SCOPED LOCATE. The zone struct chains
		// on XrViewLocateInfo::next with the rig hanging off ITS next, so the
		// runtime frames the Kooima to the zone rect (the rect IS the canvas)
		// and returns render-ready views for the band — not for the window.
		// The SAME instance is chained again on the projection layer below;
		// the xrEndFrame values are authoritative (ADR-027 Decision 3).
		if (g_zones_active) {
			compute_zone_layout(&zl);
			zone3d.next = g_has_view_rig ? &display_rig : nullptr;
			zone3d.zoneId = 1;
			zone3d.rect.offset = {zl.z3d_x, zl.z3d_y};
			zone3d.rect.extent = {zl.z3d_w, zl.z3d_h};
			locate_info.next = &zone3d;

			// Lifecycle breadcrumb, re-logged only when the layout CHANGES
			// (rotation / window move) — never per frame. This is the app-side
			// half of the phase evidence chain; the runtime prints
			// "[per-session] #568 render layer = ZONE_3D id=1 rect=..." and the
			// Leia plug-in prints "HW_DBG_CNSDK: weave band ... screen-pos ..."
			// whose screen-pos MUST equal window-origin + this rect's offset.
			static int32_t last[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
			const int32_t cur[8] = {zl.canvas_w, zl.canvas_h, zl.z3d_x,  zl.z3d_y,
			                        zl.z3d_w,    zl.z3d_h,    zl.l2d_w, zl.l2d_h};
			if (std::memcmp(last, cur, sizeof(cur)) != 0) {
				std::memcpy(last, cur, sizeof(cur));
				LOGI("[zones] frame layout: canvas=%dx%d | 3D zone id=1 rect=%d,%d %dx%d | "
				     "Local2D rect=%d,%d %dx%d",
				     zl.canvas_w, zl.canvas_h, zl.z3d_x, zl.z3d_y, zl.z3d_w, zl.z3d_h, zl.l2d_x,
				     zl.l2d_y, zl.l2d_w, zl.l2d_h);
			}
		} else if (g_has_view_rig) {
			locate_info.next = &display_rig;
		}

		// Locate with capacity for the MAX view count (xrLocateViews returns
		// the device's full view_count and rejects an under-capacity array
		// with XR_ERROR_SIZE_INSUFFICIENT — the exact gate the old hard-coded
		// `2` tripped on sim_display's 4).
		XrView views[kMaxViews] = {};
		for (uint32_t i = 0; i < kMaxViews; ++i) {
			views[i].type = XR_TYPE_VIEW;
		}
		uint32_t located_view_count = 0;
		res = xrLocateViews(g_session, &locate_info, &view_state, g_max_view_count,
		                    &located_view_count, views);
		if (res == XR_SUCCESS && located_view_count >= submit_views) {
#ifdef XRT_DEBUG_ANDROID_VERBOSE
			// Throttle ~1Hz: dump per-view pose + FOV for calibration checks.
			if ((g_frame_count % 60) == 0) {
				DXR_HW_DBG("located %u views (submitting %u, mode '%s', tiles %ux%u)",
				           located_view_count, submit_views, active_mode().name, cols, rows);
				DXR_HW_DBG("views[0]: pos=(%.3f, %.3f, %.3f) fov=(L=%.3f R=%.3f U=%.3f D=%.3f) rad",
				           views[0].pose.position.x, views[0].pose.position.y, views[0].pose.position.z,
				           views[0].fov.angleLeft, views[0].fov.angleRight, views[0].fov.angleUp,
				           views[0].fov.angleDown);
			}
#endif
			DXR_HW_DBG_ONCE("first xrLocateViews success");
			// L/R swap test (ghost diagnosis): render the OTHER eye into each
			// tile. Live-tunable: adb shell setprop debug.dxr.leia.swap_lr 1
			const bool swap_lr = get_prop_float("debug.dxr.leia.swap_lr", 0.0f) != 0.0f;

			// Cache the tracked head-center for the HUD readout. Use the RAW
			// display-space eyes (view_raw.rawEyes — display-center meters, the
			// face-dot frame) rather than the world-space view poses, so the
			// numbers mirror the user's actual head position and DON'T move when
			// the cube is orbited. Fall back to the view poses if the raw channel
			// wasn't filled (e.g. view-rig unsupported).
			if (g_has_view_rig && view_raw.eyeCountOutput > 0) {
				const uint32_t r1 = (view_raw.eyeCountOutput >= 2) ? 1 : 0;
				g_eye_x.store(0.5f * (view_raw.rawEyes[0].x + view_raw.rawEyes[r1].x),
				              std::memory_order_relaxed);
				g_eye_y.store(0.5f * (view_raw.rawEyes[0].y + view_raw.rawEyes[r1].y),
				              std::memory_order_relaxed);
				g_eye_z.store(0.5f * (view_raw.rawEyes[0].z + view_raw.rawEyes[r1].z),
				              std::memory_order_relaxed);
			} else {
				const uint32_t e1 = (located_view_count >= 2) ? 1 : 0;
				g_eye_x.store(0.5f * (views[0].pose.position.x + views[e1].pose.position.x),
				              std::memory_order_relaxed);
				g_eye_y.store(0.5f * (views[0].pose.position.y + views[e1].pose.position.y),
				              std::memory_order_relaxed);
				g_eye_z.store(0.5f * (views[0].pose.position.z + views[e1].pose.position.z),
				              std::memory_order_relaxed);
			}

			// SINGLE atlas swapchain: one acquire, one tiled render pass (all
			// `submit_views` tiles), one release.
			XrSwapchainImageAcquireInfo acq = {};
			acq.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
			uint32_t img_idx = 0;
			res = xrAcquireSwapchainImage(g_atlas.swapchain, &acq, &img_idx);
			if (res == XR_SUCCESS) {
				XrSwapchainImageWaitInfo wait_img = {};
				wait_img.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
				wait_img.timeout = XR_INFINITE_DURATION;
				res = xrWaitSwapchainImage(g_atlas.swapchain, &wait_img);
			}
			if (res == XR_SUCCESS) {
				record_atlas(img_idx, views, submit_views, render_w, render_h, cols, rows, swap_lr,
				             rig_pose);

				XrSwapchainImageReleaseInfo rel = {};
				rel.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
				res = xrReleaseSwapchainImage(g_atlas.swapchain, &rel);
			}

			if (res == XR_SUCCESS) {
				for (uint32_t i = 0; i < submit_views; ++i) {
					const uint32_t tile_x = (cols > 0) ? (i % cols) : 0;
					const uint32_t tile_y = (cols > 0) ? (i / cols) : 0;
					projection_views[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
					projection_views[i].pose = views[i].pose;
					projection_views[i].fov = views[i].fov;
					projection_views[i].subImage.swapchain = g_atlas.swapchain;
					projection_views[i].subImage.imageRect.offset = {(int32_t)(tile_x * render_w),
					                                                  (int32_t)(tile_y * render_h)};
					projection_views[i].subImage.imageRect.extent = {(int32_t)render_w,
					                                                  (int32_t)render_h};
					projection_views[i].subImage.imageArrayIndex = 0;
				}
				rendered = true;
			} else {
				log_xr_result("atlas acquire/wait/release", res);
			}
		} else {
			log_xr_result("xrLocateViews", res);
		}
	}

	XrCompositionLayerProjection projection_layer = {};
	projection_layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
	projection_layer.space = g_app_space;
	projection_layer.viewCount = submit_views;
	projection_layer.views = projection_views;
	// Bind this layer's views to the zone (ADR-027 Decision 3, second chain
	// point). The SAME struct instance the locate used — a locate/submit rect
	// divergence mis-frames the band for a frame.
	if (g_zones_active && rendered) {
		projection_layer.next = &zone3d;
		// The zone content carries its own alpha (the band is cleared opaque by
		// the scene, but the DP clears OUTSIDE the band transparent so the rest
		// of the window shows through).
		projection_layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	}

	// The Local2D band: pure 2D content in a zones frame (the implicit
	// mask-from-Local2D-rects rule is OFF, ADR-027). Composited post-weave by
	// the runtime into the region the DP left flat.
	XrCompositionLayerLocal2DDXR local2d_layer = {};
	local2d_layer.type = (XrStructureType)XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_DXR;
	bool have_local2d = false;
	if (g_zones_active && rendered && g_local2d.swapchain != XR_NULL_HANDLE && zl.l2d_h > 0) {
		XrSwapchainImageAcquireInfo l_acq = {};
		l_acq.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
		uint32_t l_idx = 0;
		if (xrAcquireSwapchainImage(g_local2d.swapchain, &l_acq, &l_idx) == XR_SUCCESS) {
			XrSwapchainImageWaitInfo l_wait = {};
			l_wait.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
			l_wait.timeout = XR_INFINITE_DURATION;
			if (xrWaitSwapchainImage(g_local2d.swapchain, &l_wait) == XR_SUCCESS) {
				have_local2d = upload_local2d_image(l_idx);
			}
			XrSwapchainImageReleaseInfo l_rel = {};
			l_rel.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
			xrReleaseSwapchainImage(g_local2d.swapchain, &l_rel);
		}
		if (have_local2d) {
			local2d_layer.layerFlags = 0; // opaque 2D content
			local2d_layer.subImage.swapchain = g_local2d.swapchain;
			local2d_layer.subImage.imageRect.offset = {0, 0};
			local2d_layer.subImage.imageRect.extent = {(int32_t)g_local2d.width,
			                                           (int32_t)g_local2d.height};
			local2d_layer.subImage.imageArrayIndex = 0;
			local2d_layer.rect.offset = {zl.l2d_x, zl.l2d_y};
			local2d_layer.rect.extent = {zl.l2d_w, zl.l2d_h};
		}
	}

	const XrCompositionLayerBaseHeader *layers[2] = {
	    reinterpret_cast<const XrCompositionLayerBaseHeader *>(&projection_layer),
	    reinterpret_cast<const XrCompositionLayerBaseHeader *>(&local2d_layer),
	};
	const uint32_t layer_count = rendered ? (have_local2d ? 2u : 1u) : 0u;

	XrFrameEndInfo end_info = {};
	end_info.type = XR_TYPE_FRAME_END_INFO;
	end_info.displayTime = frame_state.predictedDisplayTime;
	end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end_info.layerCount = layer_count;
	end_info.layers = layer_count > 0 ? layers : nullptr;

	// Per-frame wish reference. wishMask = NULL → AUTO: the runtime derives the
	// hardware wish from the 3D-zone rects (ADR-027 Decision 1), which is what
	// keeps the rig plane and the wish plane from drifting. VALIDATE is on by
	// default here (this is a conformance test app, not a perf one) and can be
	// A/B'd with `setprop debug.dxr.zones.validate 0`.
	XrDisplayZonesFrameEndInfoDXR zones_end = {};
	zones_end.type = (XrStructureType)XR_TYPE_DISPLAY_ZONES_FRAME_END_INFO_DXR;
	zones_end.wishMask = XR_NULL_HANDLE;
	if (g_zones_active && layer_count > 0) {
		zones_end.flags = (get_prop_float("debug.dxr.zones.validate", 1.0f) != 0.0f)
		                      ? XR_DISPLAY_ZONES_FRAME_END_VALIDATE_BIT_DXR
		                      : 0;
		end_info.next = &zones_end;
	}

	res = xrEndFrame(g_session, &end_info);
	if (res != XR_SUCCESS) {
		log_xr_result("xrEndFrame", res);
		return false;
	}

	g_frame_count++;
	if ((g_frame_count % 60) == 0) {
		LOGI("frame %llu", (unsigned long long)g_frame_count);
	}
	return true;
}

void
destroy_session()
{
	if (g_session != XR_NULL_HANDLE) {
		XrResult res = xrDestroySession(g_session);
		log_xr_result("xrDestroySession", res);
		g_session = XR_NULL_HANDLE;
	}
}

void
destroy_vulkan()
{
	if (g_vk_device != VK_NULL_HANDLE) {
		// Drain the queue before tearing down — same defensive idiom as
		// the runtime's DP destroy path (B6 audit fix).
		vkDeviceWaitIdle(g_vk_device);
		vkDestroyDevice(g_vk_device, nullptr);
		g_vk_device = VK_NULL_HANDLE;
		g_vk_queue = VK_NULL_HANDLE;
		g_vk_queue_family = UINT32_MAX;
	}
	if (g_vk_instance != VK_NULL_HANDLE) {
		vkDestroyInstance(g_vk_instance, nullptr);
		g_vk_instance = VK_NULL_HANDLE;
	}
	g_vk_phys_device = VK_NULL_HANDLE;
}

void
destroy_instance()
{
	// Tear down in reverse-creation order: cmd_pool → space → swapchains
	// → session → Vulkan → instance. Doing it any other way invalidates
	// handles still referenced by the runtime / loader. Drain GPU
	// before per-view image destruction (audit B6).
	if (g_vk_device != VK_NULL_HANDLE) {
		vkDeviceWaitIdle(g_vk_device);
	}
	destroy_view_framebuffers();
	hud_font_destroy(g_hud_font);
	crate_scene_destroy(g_scene);
	if (g_cube_vbuf != VK_NULL_HANDLE) {
		vkDestroyBuffer(g_vk_device, g_cube_vbuf, nullptr);
		g_cube_vbuf = VK_NULL_HANDLE;
	}
	if (g_cube_vbuf_mem != VK_NULL_HANDLE) {
		vkFreeMemory(g_vk_device, g_cube_vbuf_mem, nullptr);
		g_cube_vbuf_mem = VK_NULL_HANDLE;
	}
	destroy_pipeline();
	destroy_render_pass();
	destroy_cmd_pool();
	destroy_reference_space();
	destroy_swapchains();
	destroy_session();
	destroy_vulkan();
	if (g_instance != XR_NULL_HANDLE) {
		XrResult res = xrDestroyInstance(g_instance);
		log_xr_result("xrDestroyInstance", res);
		g_instance = XR_NULL_HANDLE;
		g_system_id = XR_NULL_SYSTEM_ID;
	}
}

void
handle_cmd(struct android_app *app, int32_t cmd)
{
	switch (cmd) {
	case APP_CMD_INIT_WINDOW:
		LOGI("APP_CMD_INIT_WINDOW (window=%p)", app->window);
#ifdef CUBE_HAVE_ANDROID_SURFACE_BINDING
		// Publish OUR window (#1037). Before the bring-up chain on first
		// launch (create_session chains it); after it on every resume, where
		// it goes out as xrSetAndroidSurfaceDXR so the runtime rebuilds its
		// VkSurfaceKHR and the DP resumes.
		publish_app_surface(app->window);
#endif
		// xrCreateInstance is deferred until the window exists because
		// some runtimes inspect the Activity surface state during create.
		// Idempotent — guarded by g_instance.
		if (g_instance == XR_NULL_HANDLE) {
			// Chain the bring-up steps — bail at the first failure
			// because each step depends on the previous one's outputs.
			bool brought_up =
			    create_instance(app) &&
			    query_system_and_graphics_reqs() &&
			    create_vulkan_instance() &&
			    pick_physical_device() &&
			    create_vulkan_device() &&
			    create_session() &&
			    query_display_info_and_modes() &&
			    create_swapchains() &&
			    // Zones are OPTIONAL: activate_zones() returns false on a runtime
			    // that doesn't advertise XR_DXR_display_zones and the app then runs
			    // the plain full-canvas path, so this must not gate startup.
			    (activate_zones(), true) &&
			    create_reference_space() &&
			    create_cmd_pool() &&
			    create_render_pass() &&
			    create_pipeline() &&
			    create_scene() &&
			    create_cube_vertex_buffer() &&
			    create_hud_buffer() &&
			    create_view_framebuffers();
			if (brought_up) {
				LOGI("Bring-up chain complete; awaiting session state events.");
				// Optional crisp HUD font (stb_truetype). Non-fatal: on failure
				// g_hud_font.ready stays false and the HUD falls back to the
				// legacy bitmap glyphs.
				hud_font_init(g_hud_font, g_vk_phys_device, g_vk_device, g_vk_queue,
				              g_vk_queue_family, g_render_pass, 48.0f);
			} else {
				LOGW("Bring-up chain failed; see logs above.");
			}
		}
		break;
	case APP_CMD_TERM_WINDOW:
		LOGI("APP_CMD_TERM_WINDOW");
#ifdef CUBE_HAVE_ANDROID_SURFACE_BINDING
		// Surface lost. Tell the runtime BEFORE native_app_glue frees it, so
		// nothing presents into a dead window and the DP pauses (releasing its
		// 3D-lens vote, ADR-036 D7) instead of holding the panel.
		publish_app_surface(nullptr);
#endif
		// Clear any in-progress touch: a card/recents gesture sends a DOWN to the
		// app, then the system steals it for the swipe, so the matching UP never
		// arrives — leaving g_touching stuck true, which pauses the cube auto-spin
		// forever (the scene "freezes" on resume even though frames keep
		// advancing). Resetting here releases that stuck gesture.
		g_touching.store(false, std::memory_order_relaxed);
		break;
	case APP_CMD_GAINED_FOCUS:
		LOGI("APP_CMD_GAINED_FOCUS");
		break;
	case APP_CMD_LOST_FOCUS:
		LOGI("APP_CMD_LOST_FOCUS");
		g_touching.store(false, std::memory_order_relaxed);  // release any stuck gesture
		break;
	case APP_CMD_DESTROY:
		LOGI("APP_CMD_DESTROY");
		destroy_instance();
		break;
	default:
		break;
	}
}

// On-device input (MonadoView is FLAG_NOT_FOCUSABLE, so touch lands in this
// NativeActivity input queue rather than the runtime's display window):
//   - single-finger DRAG orbits the camera (updates the display-rig yaw/pitch
//     the runtime projects through). The cube auto-spins when idle but HOLDS
//     STILL while a finger is down (g_touching), so the drag isn't fighting a
//     moving cube and releasing doesn't read as a spin "jump".
//   - DOUBLE-TAP (two quick taps with no drag) cycles the rendering mode
//     (2D / Anaglyph / Cropped-SBS / Squeezed-SBS / Quad …). Absolute switching
//     is also available via `adb shell setprop debug.dxr.mode N`.
// A drag is distinguished from a tap by total pointer travel, so the orbit
// gesture never trips the mode cycle.
// Shared touch handler driving the drag-orbit + double-tap-mode-cycle. Called
// from BOTH the NativeActivity input queue (handle_input) and the Kotlin
// dispatchTouchEvent bridge (nativeOnTouch) — on this overlay-rendered setup
// the runtime's display surface sits above the app and the NativeActivity's own
// input channel gets no touchable frame, so the Kotlin path is what actually
// delivers touch. action/x are pointer-0 values; action uses the MotionEvent
// ACTION_* codes (DOWN=0, UP=1, MOVE=2, same as AMOTION_EVENT_ACTION_*).
// time_ms is the event time in milliseconds (for double-tap timing).
void
process_touch_event(int32_t action, float x, float y, int64_t time_ms)
{
	static float last_x = 0.0f, last_y = 0.0f;
	static float moved_px = 0.0f;
	static int64_t last_up_ms = 0;

	switch (action) {
	case AMOTION_EVENT_ACTION_DOWN:
		last_x = x;
		last_y = y;
		moved_px = 0.0f;
		g_touching.store(true, std::memory_order_relaxed);  // pause auto-spin
		break;

	case AMOTION_EVENT_ACTION_MOVE: {
		const float dx = x - last_x;
		const float dy = y - last_y;
		last_x = x;
		last_y = y;
		moved_px += std::fabs(dx) + std::fabs(dy);
		// ~0.005 rad/px, sign-flipped so the scene tracks the finger (drag the
		// content, not the camera): drag right → scene rotates right; drag down
		// → scene tilts down. Pitch clamped to keep the camera off the poles.
		const float kSens = 0.005f;
		float yaw = g_cam_yaw.load(std::memory_order_relaxed) - dx * kSens;
		float pitch = g_cam_pitch.load(std::memory_order_relaxed) - dy * kSens;
		const float kPitchLimit = 1.2f;
		if (pitch > kPitchLimit) pitch = kPitchLimit;
		if (pitch < -kPitchLimit) pitch = -kPitchLimit;
		g_cam_yaw.store(yaw, std::memory_order_relaxed);
		g_cam_pitch.store(pitch, std::memory_order_relaxed);
		break;
	}

	case AMOTION_EVENT_ACTION_UP: {
		g_touching.store(false, std::memory_order_relaxed);  // resume auto-spin
		// A drag (significant travel) never counts as a tap.
		const float kTapSlopPx = 24.0f;
		if (moved_px > kTapSlopPx) {
			last_up_ms = 0;  // a drag breaks any pending double-tap
			break;
		}
		// Two taps within ~300 ms → reset the view (re-center the orbit camera),
		// like Space on the Windows app. Rendering-mode switching stays available
		// via `adb shell setprop debug.dxr.mode N`.
		const int64_t dt = time_ms - last_up_ms;
		if (last_up_ms != 0 && dt > 0 && dt < 300) {
			LOGI("double-tap → reset view (orbit re-centered)");
			g_cam_yaw.store(0.0f, std::memory_order_relaxed);
			g_cam_pitch.store(0.0f, std::memory_order_relaxed);
			last_up_ms = 0;  // reset so a triple-tap isn't two resets
		} else {
			last_up_ms = time_ms;
		}
		break;
	}

	case AMOTION_EVENT_ACTION_CANCEL:
		g_touching.store(false, std::memory_order_relaxed);  // resume auto-spin
		break;

	default:
		break;
	}
}

static int32_t
handle_input(struct android_app *app, AInputEvent *event)
{
	(void)app;
	if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) {
		return 0;
	}
	const int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
	const float x = AMotionEvent_getX(event, 0);
	const float y = AMotionEvent_getY(event, 0);
	const int64_t time_ms = AMotionEvent_getEventTime(event) / 1000000;  // ns → ms
	process_touch_event(action, x, y, time_ms);
	return 1;  // consumed
}

} // namespace

// ─── JNI bridge to the Kotlin overlay (MainActivity) ──────────────────────
// Underscores in the package/class become `_1` in JNI symbol names.
extern "C" JNIEXPORT void JNICALL
Java_com_displayxr_cube_1zones_1vk_1android_MainActivity_nativeSetBackgroundEnabled(
    JNIEnv * /*env*/, jobject /*thiz*/, jboolean enabled)
{
	g_bg_enabled.store(enabled == JNI_TRUE, std::memory_order_relaxed);
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_displayxr_cube_1zones_1vk_1android_MainActivity_nativeGetEye(
    JNIEnv *env, jobject /*thiz*/)
{
	jfloatArray arr = env->NewFloatArray(3);
	if (arr == nullptr) {
		return nullptr;
	}
	const jfloat v[3] = {
	    g_eye_x.load(std::memory_order_relaxed),
	    g_eye_y.load(std::memory_order_relaxed),
	    g_eye_z.load(std::memory_order_relaxed),
	};
	env->SetFloatArrayRegion(arr, 0, 3, v);
	return arr;
}

// Authoritative device orientation, pushed from MainActivity (onCreate +
// onConfigurationChanged) since the native window buffer reports a fixed
// orientation. Drives the orientation-aware HUD rotation + projection aspect.
// Kotlin dispatchTouchEvent bridge — the reliable touch path on this overlay-
// rendered setup (the NativeActivity's own input queue gets no touchable frame
// because the runtime's display surface covers it). action = MotionEvent
// ACTION_* (DOWN=0/UP=1/MOVE=2); x/y in view pixels; eventTimeMs = getEventTime().
extern "C" JNIEXPORT void JNICALL
Java_com_displayxr_cube_1zones_1vk_1android_MainActivity_nativeOnTouch(
    JNIEnv * /*env*/, jobject /*thiz*/, jint action, jfloat x, jfloat y, jlong eventTimeMs)
{
	process_touch_event((int32_t)action, (float)x, (float)y, (int64_t)eventTimeMs);
}

extern "C" JNIEXPORT void JNICALL
Java_com_displayxr_cube_1zones_1vk_1android_MainActivity_nativeSetRotation(
    JNIEnv * /*env*/, jobject /*thiz*/, jint rotation)
{
	g_display_rotation.store(rotation & 3, std::memory_order_relaxed);
	LOGI("nativeSetRotation: %d (Surface.ROTATION_%d)", (int)rotation, (int)rotation * 90);
}

// True once xrCreateInstance has failed with RUNTIME_UNAVAILABLE — MainActivity
// polls this to prompt the user to launch DisplayXR first.
#ifdef CUBE_HAVE_ANDROID_SURFACE_BINDING
// MainActivity's Choreographer poll (#1037/#1033). Android reports a pure window
// MOVE to nobody: WindowFrames.didFrameSizeChange compares w/h only, so the move
// goes out as a oneway IWindow.moved with no layout, no invalidate and no
// callback, while SurfaceFlinger has already repositioned the layer with the OLD
// buffer. Sampling getLocationOnScreen() per frame is the only way to see it.
extern "C" JNIEXPORT void JNICALL
Java_com_displayxr_cube_1zones_1vk_1android_MainActivity_nativeSetWindowRect(
    JNIEnv * /*env*/, jobject /*thiz*/, jint x, jint y, jint w, jint h, jint panelW, jint panelH,
    jint displayId)
{
	// Single writer (the UI thread); the render thread reads after observing
	// the sequence bump, which release-orders these stores.
	g_win_rect.x = (int32_t)x;
	g_win_rect.y = (int32_t)y;
	g_win_rect.w = (int32_t)w;
	g_win_rect.h = (int32_t)h;
	g_win_rect.panel_w = (int32_t)panelW;
	g_win_rect.panel_h = (int32_t)panelH;
	g_win_rect.display_id = (int32_t)displayId;
	g_win_rect_seq.fetch_add(1, std::memory_order_release);
}
#endif

extern "C" JNIEXPORT jboolean JNICALL
Java_com_displayxr_cube_1zones_1vk_1android_MainActivity_nativeRuntimeUnavailable(
    JNIEnv * /*env*/, jobject /*thiz*/)
{
	return g_runtime_unavailable.load(std::memory_order_relaxed) ? JNI_TRUE : JNI_FALSE;
}

// True once the OpenXR instance is up (runtime reached) — lets MainActivity stop
// watching so the "runtime missing" dialog can't re-fire after a good start.
extern "C" JNIEXPORT jboolean JNICALL
Java_com_displayxr_cube_1zones_1vk_1android_MainActivity_nativeXrReady(
    JNIEnv * /*env*/, jobject /*thiz*/)
{
	return (g_instance != XR_NULL_HANDLE) ? JNI_TRUE : JNI_FALSE;
}

extern "C" void
android_main(struct android_app *app)
{
	LOGI("cube_zones_vk_android: android_main entered");
	DXR_HW_DBG("android_main: activity=%p vm=%p", (void *)app->activity->clazz,
	           (void *)app->activity->vm);
	app->onAppCmd = handle_cmd;
	app->onInputEvent = handle_input;
	// Captured for crate_scene texture loading from the APK assets.
	g_asset_manager = app->activity->assetManager;

	if (!initialize_loader(app)) {
		LOGE("OpenXR loader init failed; the loop continues but no XR calls will work");
	}

	while (true) {
		// Drain Android lifecycle events. Block only when the OpenXR
		// session isn't actively rendering yet — once we hit
		// SYNCHRONIZED+, we want to spin the frame loop and only
		// non-blocking poll Android.
		const int poll_timeout_ms = g_session_running ? 0 : 250;
		int events;
		struct android_poll_source *source;
		while (ALooper_pollAll(poll_timeout_ms, nullptr, &events, (void **)&source) >= 0) {
			if (source != nullptr) {
				source->process(app, source);
			}
			if (app->destroyRequested != 0) {
				LOGI("destroyRequested — exiting android_main");
				destroy_instance();
				return;
			}
		}

		// (Orientation comes from MainActivity via nativeSetRotation — the
		// window buffer dims aren't a reliable rotation signal.)

		// OpenXR side: pump session state events and render a frame
		// when the session is in a rendering state.
		if (g_instance != XR_NULL_HANDLE) {
			poll_xr_events();
			if (g_exit_requested) {
				LOGI("XR exit requested — exiting android_main");
				destroy_instance();
				return;
			}
			// Run the frame loop whenever the session is running (READY..STOPPING)
			// and we have a live surface. We must drive frames starting in READY:
			// a CTS-compliant runtime only advances READY->SYNCHRONIZED on the
			// app's first xrBeginFrame, so gating render on SYNCHRONIZED+ here would
			// deadlock (the session never leaves READY → black screen). render_frame
			// honors frame_state.shouldRender, so it submits no layers until the
			// runtime reports the session visible — the spec-correct bootstrap.
			//
			// The app->window guard is the #507 surface gate: on background / card /
			// split, native_app_glue clears app->window, so we skip the frame loop
			// (no wait/present on a dead surface) until the surface returns on resume
			// (APP_CMD_INIT_WINDOW). g_session_running drops on STOPPING (xrEndSession).
			if (app->window != nullptr && g_session_running) {
				render_frame();
			}
		}
	}
}
