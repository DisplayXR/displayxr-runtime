// Copyright 2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Native OpenGL compositor — direct GL rendering, no interop.
 *
 * Creates GL texture swapchains, renders atlas output, presents
 * to window. Supports Windows (WGL), Android (EGL), macOS (CGL).
 *
 * @author David Fattal
 * @ingroup comp_gl
 */

#include "comp_gl_compositor.h"
#ifdef _WIN32
#include "d3d11/comp_d3d11_window.h"
#endif

#include "util/comp_layer_accum.h"
#ifdef XRT_OS_WINDOWS
#include "util/comp_display_refresh_win.h"
// #918 Phase 3 — the shared `weave placement:` line, plus the canonical reason
// tokens. Header-only from comp_xbridge: this compositor has NO output-device
// split (ADR-037 §5 — OpenGL exposes no device-selection API at all), so it
// never links the transport, it only names itself the way the split paths do.
#include "d3d/d3d_weave_placement.h"
#include "comp_split_gate.h"
#endif

#include "xrt/xrt_device.h"
#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_display_processor_gl.h"
#include "xrt/xrt_handles.h"
#include "xrt/xrt_config_build.h"
#include "xrt/xrt_config_os.h"
#include "xrt/xrt_limits.h"
#include "xrt/xrt_system.h"

#include "os/os_threading.h"

#include "util/u_logging.h"
#include "util/u_setting.h"
#include "util/u_weave_scope.h"
#include "util/u_misc.h"
#include "util/u_tiling.h"
#include "util/u_canvas.h"
#include "util/u_capture_intent.h"
#include "util/u_repaint_gate.h"
#include "util/u_image_capture.h"
#include "util/u_time.h"
#include "util/u_hud.h"
#include <displayxr_mcp/mcp_capture.h>

// STB_IMAGE_WRITE_STATIC scopes the stbi_write_* symbols to this TU so
// we can safely implement stb in multiple compositors that link into
// the same binary (metal, gl, d3d11, d3d11_service).
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "os/os_time.h"
#include <sys/stat.h>
#ifndef XRT_OS_WINDOWS
#include <unistd.h>
#endif
#include "os/os_threading.h"
#include "math/m_api.h"

#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty_interface.h"
#endif

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>

#ifdef XRT_OS_WINDOWS
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif


// GL function loading via GLAD (cross-platform)
#ifdef XRT_OS_WINDOWS
#include "ogl/ogl_api.h"
#include "ogl/wgl_api.h"
#include <d3d11.h>
#include <dxgi1_3.h>     // CreateDXGIFactory2, IDXGISwapChain1, CreateSwapChainForComposition
#include <dcomp.h>       // DirectComposition (transparent GL present path)
#include <dwmapi.h>      // DwmFlush (late-weave pacing — no DXGI stats on GL)
#pragma comment(lib, "dwmapi.lib")
#include <d3dcompiler.h> // D3DCompile (inline blit shader for the DComp present path)
// ADR-037 §2 render-adapter resolver — the shared one (#1161). The GL interop
// devices consult it rather than growing a second placement rule (#1159).
#include "d3d/d3d_render_adapter.hpp"
// GUID for ID3D11Texture2D (needed for OpenSharedResource in C)
static const IID IID_ID3D11Texture2D_local = {
    0x6f15aaf2, 0xd208, 0x4e89,
    {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};
#elif defined(XRT_OS_ANDROID)
#include "ogl/ogl_api.h"
#include "ogl/egl_api.h"
#elif defined(__APPLE__)
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include "comp_gl_window_macos.h"
#endif

/*
 * WGL_NV_DX_interop2 function types (loaded dynamically via wglGetProcAddress)
 */
#ifdef XRT_OS_WINDOWS
typedef BOOL(WINAPI *PFN_wglDXSetResourceShareHandleNV)(void *dxObject, HANDLE shareHandle);
typedef HANDLE(WINAPI *PFN_wglDXOpenDeviceNV)(void *dxDevice);
typedef BOOL(WINAPI *PFN_wglDXCloseDeviceNV)(HANDLE hDevice);
typedef HANDLE(WINAPI *PFN_wglDXRegisterObjectNV)(HANDLE hDevice, void *dxObject,
                                                   GLuint name, GLenum type, GLenum access);
typedef BOOL(WINAPI *PFN_wglDXUnregisterObjectNV)(HANDLE hDevice, HANDLE hObject);
typedef BOOL(WINAPI *PFN_wglDXLockObjectsNV)(HANDLE hDevice, GLint count, HANDLE *hObjects);
typedef BOOL(WINAPI *PFN_wglDXUnlockObjectsNV)(HANDLE hDevice, GLint count, HANDLE *hObjects);

#define WGL_ACCESS_READ_WRITE_NV 0x0001
#endif


/*
 *
 * Constants
 *
 */

#define GL_SWAPCHAIN_MAX_IMAGES 8
#ifndef GL_MAX_LAYERS
#define GL_MAX_LAYERS 16
#endif

// Default window dimensions
#define GL_DEFAULT_WIDTH 2560
#define GL_DEFAULT_HEIGHT 1440


/*
 *
 * GL swapchain
 *
 */

struct comp_gl_swapchain
{
	//! Must be first — state tracker casts to xrt_swapchain_gl to read images[].
	struct xrt_swapchain_gl base;

	GLuint textures[GL_SWAPCHAIN_MAX_IMAGES];
	uint32_t image_count;
	struct xrt_swapchain_create_info info;

	//! GL texture target: GL_TEXTURE_2D for single-layer swapchains,
	//! GL_TEXTURE_2D_ARRAY for layered (arraySize>1) swapchains.
	GLenum target;

	int32_t acquired_index;
	int32_t waited_index;
	uint32_t last_released_index;
};

static inline struct comp_gl_swapchain *
gl_swapchain(struct xrt_swapchain *xsc)
{
	return (struct comp_gl_swapchain *)xsc;
}


/*
 *
 * GLSL shaders (embedded)
 *
 */

static const char *VS_FULLSCREEN_QUAD =
    "#version 330 core\n"
    "out vec2 v_uv;\n"
    "uniform float u_flip_y;\n" // 0.0 = normal, 1.0 = flip Y (for IOSurface)
    "void main() {\n"
    "    float x = float((gl_VertexID & 1) << 2);\n"
    "    float y = float((gl_VertexID & 2) << 1);\n"
    "    float uv_y = y * 0.5;\n"
    "    v_uv = vec2(x * 0.5, mix(uv_y, 1.0 - uv_y, u_flip_y));\n"
    "    gl_Position = vec4(x - 1.0, y - 1.0, 0.0, 1.0);\n"
    "}\n";

//! Fragment shader: blit single texture to screen.
static const char *FS_BLIT =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D u_texture;\n"
    "uniform vec4 u_src_rect;\n" // x, y, w, h in normalized coords
    "void main() {\n"
    "    vec2 uv = u_src_rect.xy + v_uv * u_src_rect.zw;\n"
    "    fragColor = texture(u_texture, uv);\n"
    "}\n";

//! Fragment shader: blit one layer of an ARRAY texture to the atlas. Under
//! single-pass-instanced the app submits ONE arraySize>1 (GL_TEXTURE_2D_ARRAY)
//! swapchain with per-view imageArrayIndex 0 (left) / 1 (right); u_layer selects
//! the slice. This is the GL analog of the D3D12 #656 / D3D11 array fix — a plain
//! sampler2D always views layer 0 (flat output).
static const char *FS_BLIT_ARRAY =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2DArray u_texture;\n"
    "uniform vec4 u_src_rect;\n" // x, y, w, h in normalized coords
    "uniform float u_layer;\n"   // array slice (imageArrayIndex)
    "void main() {\n"
    "    vec2 uv = u_src_rect.xy + v_uv * u_src_rect.zw;\n"
    "    fragColor = texture(u_texture, vec3(uv, u_layer));\n"
    "}\n";

//! Vertex shader: positioned quad for window-space layers.
//! Takes uniform position/size in NDC.
//! v_uv.y is flipped: top of NDC rect samples UV.y=0 (top of texture),
//! bottom samples UV.y=1. WS-layer source textures land in GL via
//! glTexSubImage2D in top-down memory order (CG/D2D bitmap layout, row 0 =
//! top of image), so UV.y=0 is the top. Without the flip the HUD renders
//! upside-down on the atlas (then sim_display passes that inversion straight
//! through to screen).
static const char *VS_WINDOW_SPACE =
    "#version 330 core\n"
    "out vec2 v_uv;\n"
    "uniform vec4 u_rect;\n" // x, y, w, h in NDC [-1,1]
    "void main() {\n"
    "    float x = float((gl_VertexID & 1) << 1) - 1.0;\n" // -1 or 1
    "    float y = float((gl_VertexID & 2)) - 1.0;\n"       // -1 or 1
    "    v_uv = vec2((x + 1.0) * 0.5, (1.0 - y) * 0.5);\n"
    "    gl_Position = vec4(u_rect.x + (x * 0.5 + 0.5) * u_rect.z,\n"
    "                       u_rect.y + (y * 0.5 + 0.5) * u_rect.w,\n"
    "                       0.0, 1.0);\n"
    "}\n";

//! Fragment shader: textured quad with alpha.
static const char *FS_TEXTURED =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D u_texture;\n"
    "uniform vec4 u_src_rect;\n" // x, y, w, h in texture coords
    "void main() {\n"
    "    vec2 uv = u_src_rect.xy + v_uv * u_src_rect.zw;\n"
    "    fragColor = texture(u_texture, uv);\n"
    "}\n";

//! Fragment shader: masked 2D-over-3D composite (#439 Phase 3 GL leg), by
//! u_composite_mode:
//! 0 (LERP): final = M*weave + (1-M)*twod (explicit authored mask).
//! 1 (ALPHA_OVER, #491): final = twod + (1-twod.a)*weave — premultiplied
//!   "over" of the 2D atop the weave, so translucent 2D reveals the 3D scene
//!   (the IMPLICIT Local2D mask; mask unused).
//! 2 (ZONES, ADR-027/#801): final = twod + (1-twod.a)*(M*weave) — M gates
//!   only the WEAVE by zone geometry (binary zone raster, or the #803 opt-in
//!   feather ramp); the 2D composites on top by its own premultiplied alpha.
//! All three sources are window-resolution textures in the same GL
//! framebuffer orientation, 1:1.
static const char *FS_MASKED_COMPOSITE =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D u_twod;\n"
    "uniform sampler2D u_mask;\n"
    "uniform sampler2D u_weave;\n"
    "uniform int u_composite_mode;\n"
    "void main() {\n"
    "    vec4 twod  = texture(u_twod,  v_uv);\n"
    "    vec4 weave = texture(u_weave, v_uv);\n"
    "    if (u_composite_mode == 1) {\n"
    "        fragColor = twod + (1.0 - twod.a) * weave;\n"
    "        return;\n"
    "    }\n"
    "    float M = clamp(texture(u_mask, v_uv).r, 0.0, 1.0);\n"
    "    if (u_composite_mode == 2) {\n"
    "        fragColor = twod + (1.0 - twod.a) * (M * weave);\n"
    "        return;\n"
    "    }\n"
    "    fragColor = M * weave + (1.0 - M) * twod;\n"
    "}\n";

/*
 *
 * GL compositor structure
 *
 */

// #439 Phase 3 — authored zone mask (XR_DXR_local_3d_zone), GL R8 texture.
// In-process handle apps author it on the same GL context the composite
// samples from, frame-serialized, so a single texture (no staged copy) is
// coherent. Tier-3 acquire_rt is unimplemented on GL.
struct comp_gl_zone_mask
{
	GLuint tex;  //!< R8 mask, M in [0,1] (1 = 3D / keep weave).
	GLuint fbo;  //!< Framebuffer over tex for the Tier-1/2 scissored clears.
	uint32_t w, h;
	bool submitted; //!< True once submitted at least once (else invisible).
};

struct comp_gl_compositor
{
	//! Must be first — implements xrt_compositor_native.
	struct xrt_compositor_native base;

	// --- GL resources ---
	GLuint program_blit;      //!< Shader for blitting eye to atlas texture
	GLuint program_blit_array; //!< Blit shader for LAYERED (array) swapchains (sampler2DArray)
	GLuint program_window_space; //!< Window-space layer (positioned quad)
	GLuint vao_empty;         //!< Empty VAO for vertex-shader-generated fullscreen quad
	GLuint fbo;               //!< Framebuffer for rendering into atlas texture
	GLuint atlas_texture;    //!< Atlas texture (tile_columns * view_width x tile_rows * view_height)
	uint32_t atlas_tex_width;  //!< Atlas texture width (fixed at init)
	uint32_t atlas_tex_height; //!< Atlas texture height (fixed at init)
	uint32_t view_width;
	uint32_t view_height;
	uint32_t tile_columns;    //!< Tile columns in atlas layout (default 2 for stereo)
	uint32_t tile_rows;       //!< Tile rows in atlas layout (default 1 for stereo)

	// --- Layer accumulation ---
	struct comp_layer_accum layer_accum;

	/*!
	 * #868 / #875: serialises the frame path against the repaint replay.
	 *
	 * GL needs this for a reason the other backends do not have: a WGL context
	 * can be current on at most ONE thread at a time. The frame path makes
	 * c->hglrc current and releases it before returning, so between app frames
	 * the context is current nowhere — that gap is where a repaint claims it.
	 * The lock is what guarantees the two never overlap.
	 */
	struct os_mutex mutex;

	//! #868: the repaint loop. See gl_repaint_thread.
	//! os_* primitives, not std::, because this struct is U_TYPED_CALLOC'd —
	//! calloc runs no constructors, so a std::mutex/std::thread member here
	//! would be undefined behaviour.
	struct os_thread_helper repaint_thread;

	/*!
	 * #868: what the repaint thread needs to replay the last app frame's weave
	 * WITHOUT re-reading app-owned state. Published by layer_commit; see the
	 * #875 deposit half in gl_composite_local_2d.
	 */
	struct
	{
		int enabled;                 //!< DXR_WEAVE_REPAINT=0 disables.
		int force;                   //!< DXR_WEAVE_REPAINT_FORCE=1 correctness probe.
		bool armed;                  //!< False on zero-copy: the atlas IS the app's texture.
		bool app_frame_in_progress;  //!< Set by layer_begin, cleared by layer_commit.
		uint64_t last_app_frame_ns;  //!< Quiet-gate key. Never touched by a repaint.
		struct u_repaint_gate gate;  //!< #1257 interval-aware quiet gate.
		struct u_app_partition partition; //!< #1257 slot partition: xrWaitFrame throttle state.

		//! The 2D-under backdrop the last app frame DEPOSITED. Reused, never
		//! re-flattened — the flatten samples the app's Local2D textures.
		GLuint backdrop_tex;
		uint32_t backdrop_w, backdrop_h;

		//! The atlas + geometry the last app frame wove from (compositor-owned).
		GLuint atlas_tex;
		uint32_t present_w, present_h;
		float last_dt;

		uint64_t count, ticks;       //!< Diagnostics.
	} repaint;

	// --- Platform context ---
#ifdef XRT_OS_WINDOWS
	HWND hwnd;
	HDC hdc;
	HGLRC hglrc;        //!< Compositor's own GL context
	HGLRC app_hglrc;    //!< App's GL context (shared textures)
	HDC app_hdc;        //!< App's device context (for restoring after compositor work)
	struct comp_d3d11_window *own_window; //!< Self-owned window (hosted mode)
	bool owns_window;
#elif defined(XRT_OS_ANDROID)
	void *egl_display;   //!< EGLDisplay
	void *egl_context;   //!< EGLContext
	void *egl_surface;   //!< EGLSurface
#elif defined(__APPLE__)
	struct comp_gl_window_macos *macos_window;  //!< macOS window helper
	bool owns_window;
	bool has_shared_iosurface;
	GLuint iosurface_gl_texture;    //!< GL texture backed by shared IOSurface
	//! Dedicated FBO with iosurface_gl_texture attached. MUST be distinct from
	//! c->fbo: the DP crop helpers temporarily rebind c->fbo's attachment to the
	//! atlas texture, so reusing c->fbo as the present target would clobber where
	//! the weave/composite lands (also required so gl_composite_local_2d can lerp
	//! into the IOSurface target while its internal crop uses c->fbo). Windows
	//! parity: shared_present_fbo.
	GLuint iosurface_present_fbo;
	uint32_t iosurface_width;
	uint32_t iosurface_height;
#endif

	// --- Shared texture (D3D11 interop via WGL_NV_DX_interop2, Windows only) ---
#ifdef XRT_OS_WINDOWS
	bool has_shared_texture;
	ID3D11Device *dx_device;
	ID3D11DeviceContext *dx_context;
	ID3D11Texture2D *dx_shared_texture;
	//! CPU staging for the GL→D3D readback bridge (see gl_shared_readback_upload).
	//! The shared-texture present path weaves into a plain GL render texture, then
	//! glReadPixels the woven region and UpdateSubresource it into the app's
	//! shared D3D texture — the WGL_NV_DX_interop2 write-BACK into the shared
	//! surface is unreliable on this stack. Grown on demand, freed at destroy.
	uint8_t *shared_readback_cpu;
	size_t shared_readback_cap;
	GLuint shared_gl_texture;      //!< Plain GL render texture for the shared-texture weave
	//! Dedicated FBO with shared_gl_texture attached. MUST be distinct from
	//! c->fbo: the DP crop helpers temporarily rebind c->fbo's attachment to the
	//! atlas texture, so reusing c->fbo as the present target would clobber where
	//! the weave/composite lands (and is also required so gl_composite_local_2d
	//! can lerp into the shared target while its internal crop uses c->fbo).
	GLuint shared_present_fbo;
	uint32_t shared_width;
	uint32_t shared_height;

	// WGL_NV_DX_interop2 function pointers (used by the DComp transit path)
	PFN_wglDXOpenDeviceNV pfn_wglDXOpenDeviceNV;
	PFN_wglDXCloseDeviceNV pfn_wglDXCloseDeviceNV;
	PFN_wglDXRegisterObjectNV pfn_wglDXRegisterObjectNV;
	PFN_wglDXUnregisterObjectNV pfn_wglDXUnregisterObjectNV;
	PFN_wglDXLockObjectsNV pfn_wglDXLockObjectsNV;
	PFN_wglDXUnlockObjectsNV pfn_wglDXUnlockObjectsNV;
#endif

	// --- Display processor ---
	struct xrt_display_processor_gl *display_processor;
	GLuint dp_crop_fbo;            //!< FBO for cropping atlas to content dims before DP
	GLuint dp_input_texture;       //!< Intermediate texture at content dims for DP input
	uint32_t dp_input_width;       //!< Current dp_input_texture width (0 = not allocated)
	uint32_t dp_input_height;      //!< Current dp_input_texture height

	// --- Eye tracking cache ---
	//! Cached eye positions from layer_commit (where SR weaver has fresh data).
	//! Returned by get_predicted_eye_positions for xrLocateViews (called before layer_commit).
	struct xrt_eye_positions cached_eye_pos;
	bool have_cached_eye_pos;

	// --- State ---
	bool hardware_display_3d;  //!< True when in 3D mode, false = 2D passthrough

	//! Per-frame effective CONTENT layout (#542): the atlas grid actually
	//! painted and handed to the DP this frame — submission-derived,
	//! decoupled from hardware_display_3d (which only drives the DP weave,
	//! HUD, and V-key paths). c->tile_columns/view_width stay the MODE
	//! layout. eff_views == 0 until the first layer commit computes it.
	uint32_t eff_views;
	uint32_t eff_cols;
	uint32_t eff_rows;
	uint32_t eff_tile_w;
	uint32_t eff_tile_h;

	uint64_t last_frame_ns;
	struct u_hud *hud;          //!< HUD overlay (shared u_hud system)
	GLuint hud_texture;         //!< GL texture for HUD pixel upload
	float smoothed_frame_time_ms; //!< Smoothed frame time for HUD FPS display
	struct xrt_device *xdev;
	struct xrt_system_devices *xsysd;
	bool sys_info_set;
	struct xrt_system_compositor_info sys_info;
	uint32_t last_3d_mode_index;       //!< Last 3D mode index (for V-key toggle restore)
	bool legacy_app_tile_scaling;      //!< True if app is legacy (gates 1/2/3 key mode selection)

	// --- #439 Phase 3 — Local2D / zone-mask consumer (full net-new GL leg) ---
	//! Active authored zone mask (XR_DXR_local_3d_zone). Set by zone_mask_submit
	//! (sticky, last-submit-wins), cleared on that mask's destroy. NOT owned.
	struct comp_gl_zone_mask *active_zone_mask;
	//! True if this frame's accumulator carried any XRT_LAYER_LOCAL_2D layer
	//! (set once at the top of layer_commit). Drives the effective-canvas
	//! supersede + the composite's have_local_2d branch.
	bool local_2d_last_frame;
	//! XR_DXR_display_zones (ADR-027): true when the current frame's
	//! accumulator carries XRT_LAYER_ZONE_3D layers (a "zones frame"). In a
	//! zones frame the canvas output rect, the sticky submitted mask, and
	//! the implicit-mask-from-Local2D rule are all inert; the effective
	//! canvas is the full client window; the wish drives the DP publish
	//! ONLY — the post-weave composite gates the weave by the BINARY zone
	//! raster (or the #803 opt-in feather raster), never by an explicit
	//! wish (#801: the wish is hardware-only). Set in the same per-frame
	//! scan as local_2d_last_frame.
	bool zones_frame;
	//! Explicit per-frame wish (XrDisplayZonesFrameEndInfoDXR.wishMask) set
	//! via comp_gl_compositor_zones_set_frame_wish before commit; NULL =
	//! auto-derive from the zone rects. Not owned — zone_mask_destroy
	//! clears any dangling reference.
	struct comp_gl_zone_mask *frame_wish;
	//! Tier-1 fallback edge state: request_display_mode(true) fired once
	//! on the zones rising edge; never forces 2D on the falling edge.
	//! P4: only taken for legacy DPs (caps.supported == 0) — a zone-capable
	//! DP gets the per-frame wish publish instead (gl_sync_zone_mask_to_dp).
	bool zones_mode_requested;
	//! #224 / ADR-027 hardware-DP zone leg (P4): cached get_local_zone_caps
	//! result. 0 = not queried yet, 1 = supported, 2 = legacy DP.
	int zone_dp_state;
	//! DP zone caps when zone_dp_state == 1.
	struct xrt_dp_local_zone_caps zone_dp_caps;
	//! Published-content generation: bumped on zone_mask_submit, on an
	//! auto-wish re-raster whose rect set / dims actually changed, and on
	//! an explicit-frame-wish source change — NOT per frame.
	uint64_t zone_publish_seq;
	//! True while this client's mask is published to the DP — drives the
	//! clear-on-deactivate edge.
	bool zone_published;
	//! This frame's resolved wish texture + dims, set by
	//! gl_composite_local_2d in zones frames (the explicit frame-wish
	//! authoring texture — same-context, no staging, matching the GL
	//! zone_mask_submit contract — or the auto raster) and reset at the top
	//! of layer_commit. The publish runs same-context after the composite,
	//! so GL command ordering makes the content visible to the DP's calls.
	GLuint zone_publish_tex;
	uint32_t zone_publish_w, zone_publish_h;
	//! Seq-bump caches: last explicit wish pointer actually published, and
	//! the auto raster's rect set (dims via zone_publish_w/h persisting).
	struct comp_gl_zone_mask *zone_frame_wish_last;
	struct xrt_rect zone_wish_rects[XRT_MAX_LAYERS];
	uint32_t zone_wish_rect_count;
	//! Masked-composite program (FS_MASKED_COMPOSITE + VS_FULLSCREEN_QUAD).
	GLuint program_masked_composite;
	//! Post-weave composite scratch. weave_tex receives the DP weave (the DP
	//! is redirected to it when the consumer is active); local2d_scratch holds
	//! the flattened Local2D layers; implicit_mask is the R8 mask rasterized
	//! from the layer rects. Each has its own FBO. Lazily (re)allocated at the
	//! window region dims; the composite lerps them into the window.
	GLuint weave_tex, weave_fbo;
	GLuint local2d_scratch_tex, local2d_scratch_fbo;
	GLuint implicit_mask_tex, implicit_mask_fbo;
	uint32_t composite_scratch_w, composite_scratch_h; // weave + local2d dims
	//! Zones COMPOSITE mask with per-zone opt-in feather
	//! (XrDisplayZoneFeatherDXR, #800/#803). Allocated only when a frame's
	//! zones request feather — the published wish must stay binary (the
	//! implicit_mask raster above), so a feathered composite needs its own
	//! texture. All-hard frames sample the binary raster for the composite.
	//! Re-rastered every feathered zones frame (VK-style).
	GLuint feather_mask_tex, feather_mask_fbo;
	uint32_t feather_mask_w, feather_mask_h;
	//! #491 part 3 — the flattened 2D-UNDER backdrop (Local2D layers before the
	//! projection in list order), handed to the DP via set_background_2d so it
	//! composites `backdrop over captured-desktop` under the 3D weave. Own FBO.
	GLuint backdrop_scratch_tex, backdrop_scratch_fbo;
	uint32_t backdrop_scratch_w, backdrop_scratch_h;
	uint32_t implicit_mask_w, implicit_mask_h;
	uint32_t implicit_rect_count;
	struct xrt_rect implicit_rects[XRT_MAX_LAYERS];

	//! MCP capture_frame request box (serviced at end of layer_commit).
	struct mcp_capture_request mcp_capture;

	//! Per-frame capture intent. See u_capture_intent.h.
	struct u_capture_intent capture_intent;

	// --- Transparent-background opt-in plumbing ---
	bool transparent_background;

	// --- Transparent-background present path (Windows: DComp + WGL_NV_DX_interop2) ---
	// See the big comment block above gl_setup_dcomp_present() for the architecture
	// and why it composes two independently-proven halves instead of the reverted
	// (PR #3b) direct-interop-into-flip-model-backbuffer approach.
#ifdef XRT_OS_WINDOWS
	bool dcomp_active;                  //!< True once the DComp present path is wired up.
	ID3D11Device *dcomp_dx_device;      //!< Dedicated D3D11 device for the present bridge.
	ID3D11DeviceContext *dcomp_dx_context;
	IDXGISwapChain1 *dcomp_swapchain;   //!< Flip-model composition swapchain (PREMULTIPLIED).
	IDCompositionDevice *dcomp_device;
	IDCompositionTarget *dcomp_target;
	IDCompositionVisual *dcomp_visual;
	HANDLE dcomp_dx_interop_device;     //!< wglDXOpenDeviceNV(dcomp_dx_device).
	// Off-screen transit texture: GL weaves into it (proven interop path), then a
	// D3D11 fullscreen-quad blit copies it to the swapchain back buffer (RTV write).
	ID3D11Texture2D *dcomp_transit_tex;
	ID3D11ShaderResourceView *dcomp_transit_srv;
	GLuint dcomp_transit_gl_tex;        //!< GL view of dcomp_transit_tex.
	GLuint dcomp_transit_fbo;           //!< FBO bound to dcomp_transit_gl_tex.
	HANDLE dcomp_transit_iop;           //!< wglDXRegisterObjectNV handle for the transit tex.
	// D3D11 fullscreen-triangle blit pipeline.
	ID3D11VertexShader *dcomp_vs;
	ID3D11PixelShader *dcomp_ps;
	ID3D11SamplerState *dcomp_samp;
	uint32_t dcomp_present_w, dcomp_present_h;

	// P0.3 (#573) — no-interop readback fallback. When WGL_NV_DX_interop2 is
	// unavailable (near-extinct hardware), GL can't share a texture with D3D11, so
	// the weave goes to the default framebuffer (the opaque path) and per frame we
	// glReadPixels it → upload to this DYNAMIC (CPU-write) D3D11 texture → reuse the
	// same blit + DComp Present. Lets chroma-key be deleted everywhere. Shares the
	// dcomp_dx_device/swapchain/target/blit pipeline with the interop path above.
	bool dcomp_readback_active;          //!< True once the readback present path is wired up.
	// Dedicated RGBA GL weave target — NOT FBO 0. The window's default framebuffer
	// has no usable alpha (glReadPixels returns A=1), which would turn the α=0
	// see-through holes into opaque black; weaving into a real RGBA texture
	// preserves the premultiplied alpha the DComp present needs.
	GLuint dcomp_readback_gl_tex;        //!< RGBA8 GL texture the DP weaves into.
	GLuint dcomp_readback_gl_fbo;        //!< FBO around dcomp_readback_gl_tex.
	ID3D11Texture2D *dcomp_readback_tex; //!< DYNAMIC RGBA source uploaded each frame.
	ID3D11ShaderResourceView *dcomp_readback_srv;
	uint8_t *dcomp_readback_cpu;         //!< glReadPixels CPU target (w*h*4 bytes).

	// --- Interop-device adapter placement (ADR-037 §5, #1159) ---
	//! Panel origin in OS virtual-screen coordinates, kept so the ADR-037 §2
	//! resolver can be called from anywhere in this file. Only
	//! `DXR_D3D_FORCE_GPU=scanout` actually reads it.
	int32_t panel_screen_left, panel_screen_top;
	//! LUID of the adapter the compositor's GL context runs on, when the driver
	//! reports one. See gl_query_context_luid().
	LUID gl_context_luid;
	bool gl_context_luid_valid;
#endif
};

static inline struct comp_gl_compositor *
gl_comp(struct xrt_compositor *xc)
{
	return (struct comp_gl_compositor *)xc;
}


#ifdef XRT_OS_WINDOWS
/*
 *
 * Interop-device adapter placement (ADR-037 §5, #1159).
 *
 * This compositor creates D3D11 devices in two places — the transparency
 * (DComp) present bridge and the device that opens the app's shared texture.
 * Both used to pass a NULL adapter to D3D11CreateDevice, i.e. "whatever DXGI
 * hands back first", which is a placement decision made by nobody: on a hybrid
 * box that adapter need not be the one the GL context lives on, and a
 * cross-adapter WGL_NV_DX_interop share is a per-frame copy through the OS.
 *
 * What GL can and cannot have. ADR-037 §5 puts OpenGL in the "OS, advisory" row
 * for a hard reason: there is no GL analogue of `suggested_d3d_luid` or
 * `select_physical_device`, so the runtime CANNOT place the GL context. The
 * driver does, steered only by the per-exe `UserGpuPreferences` pin and the
 * `NvOptimusEnablement` export. So the goal here is the reachable one: put the
 * interop devices on *whatever adapter the GL context already landed on*, and
 * say so in the log — including when we could not tell.
 *
 * How the GL context's adapter is determined, best evidence first:
 *   1. `GL_EXT_memory_object_win32` → `glGetUnsignedBytevEXT(GL_DEVICE_LUID_EXT)`.
 *      This is the driver reporting its own D3D LUID through a Khronos
 *      extension; it is exact, not a guess. Present on current NVIDIA, Intel
 *      and AMD Windows drivers.
 *   2. `GL_VENDOR` → PCI vendor id, matched against the DXGI adapters. Only
 *      trusted when it names exactly one adapter (the common iGPU+dGPU split is
 *      two different vendors, so it usually does). Heuristic.
 *   3. `GL_RENDERER` compared against the DXGI adapter descriptions, to
 *      separate two adapters from the same vendor. Heuristic.
 *   4. No evidence at all → the ADR-037 §2 resolver's answer. Deliberate and
 *      logged as a fallback — still strictly better than NULL, because the
 *      ranking and the `DXR_D3D_FORCE_GPU` override channel both apply.
 *
 */

//! One resolved interop-device adapter, plus the reason it was chosen.
struct gl_interop_adapter
{
	//! AddRef'd, or NULL when nothing could be resolved (caller passes NULL to
	//! D3D11CreateDevice and keeps today's behaviour).
	IDXGIAdapter *adapter;
	LUID luid;
	WCHAR description[128];
	//! Short static string naming the rule that decided. Never NULL.
	const char *provenance;
	//! True only when the GL context's LUID is KNOWN and equals `luid`. False
	//! also means "unknown" — never read it as "known to differ".
	bool confirmed_same_as_gl;
};

/*!
 * The LUID of the adapter the *current* GL context runs on, as reported by the
 * driver via GL_EXT_memory_object_win32. Returns false when the extension is
 * absent or the query errors — that is a legitimate outcome, not a failure.
 */
static bool
gl_query_context_luid(LUID *out_luid)
{
	if (out_luid == NULL) {
		return false;
	}
	if (!GLAD_GL_EXT_memory_object_win32 || glad_glGetUnsignedBytevEXT == NULL) {
		return false;
	}

	// Drain any pre-existing error so the one we check below is ours. Bounded:
	// with no current context glGetError() is not required to ever return
	// GL_NO_ERROR, and this must not become a hang at session create.
	for (int drain = 0; drain < 16 && glGetError() != GL_NO_ERROR; drain++) {
	}

	GLubyte bytes[GL_LUID_SIZE_EXT] = {0};
	glGetUnsignedBytevEXT(GL_DEVICE_LUID_EXT, bytes);
	if (glGetError() != GL_NO_ERROR) {
		return false;
	}

	static_assert(sizeof(LUID) == GL_LUID_SIZE_EXT, "GL_DEVICE_LUID_EXT is not LUID-sized");
	memcpy(out_luid, bytes, sizeof(LUID));
	return true;
}

//! The PCI vendor id GL_VENDOR implies, or 0 when it names no adapter vendor.
static UINT
gl_vendor_id_from_gl_strings(void)
{
	const char *vendor = glGetString != NULL ? (const char *)glGetString(GL_VENDOR) : NULL;
	if (vendor == NULL) {
		return 0;
	}
	if (strstr(vendor, "NVIDIA") != NULL || strstr(vendor, "nvidia") != NULL) {
		return 0x10DE;
	}
	if (strstr(vendor, "Intel") != NULL || strstr(vendor, "INTEL") != NULL) {
		return 0x8086;
	}
	if (strstr(vendor, "AMD") != NULL || strstr(vendor, "ATI") != NULL ||
	    strstr(vendor, "Advanced Micro") != NULL) {
		return 0x1002;
	}
	return 0;
}

//! Case-insensitive "does GL_RENDERER mention this DXGI description?", ignoring
//! the `(R)` / `(TM)` noise DXGI puts in descriptions and GL usually does not.
static bool
gl_renderer_mentions(const WCHAR *dxgi_description)
{
	const char *renderer = glGetString != NULL ? (const char *)glGetString(GL_RENDERER) : NULL;
	if (renderer == NULL || dxgi_description == NULL) {
		return false;
	}

	char needle[128] = {0};
	size_t n = 0;
	for (size_t i = 0; dxgi_description[i] != L'\0' && n + 1 < sizeof(needle); i++) {
		WCHAR wc = dxgi_description[i];
		// Drop the parens and anything non-ASCII (the (R) / (TM) marks DXGI
		// carries and GL_RENDERER usually does not).
		if (wc == L'(' || wc == L')' || wc > 0x7F) {
			continue;
		}
		if (wc == L'R' && i > 0 && dxgi_description[i - 1] == L'(') {
			continue;
		}
		needle[n++] = (char)tolower((unsigned char)wc);
	}
	if (n == 0) {
		return false;
	}

	char hay[512] = {0};
	size_t h = 0;
	for (size_t i = 0; renderer[i] != '\0' && h + 1 < sizeof(hay); i++) {
		char ch = renderer[i];
		if (ch == '(' || ch == ')') {
			continue;
		}
		if (ch == 'R' && i > 0 && renderer[i - 1] == '(') {
			continue;
		}
		hay[h++] = (char)tolower((unsigned char)ch);
	}

	// Both sides normalise to "intel uhd graphics 770" / "nvidia geforce rtx
	// 3080", so a plain substring test is enough — GL_RENDERER's extra tail
	// ("/PCIe/SSE2") does not defeat it.
	return strstr(hay, needle) != NULL;
}

/*!
 * Resolve the adapter one of this compositor's D3D11 interop devices should be
 * created on. @p purpose names the device in the log ("transparency present
 * bridge", "shared-texture upload"). Never returns an adapter the caller must
 * use: a NULL `adapter` means "no better answer than DXGI's default".
 */
static struct gl_interop_adapter
gl_resolve_interop_adapter(struct comp_gl_compositor *c, const char *purpose)
{
	struct gl_interop_adapter out = {};
	out.provenance = "unresolved";

	IDXGIFactory1 *factory = NULL;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory)) || factory == NULL) {
		U_LOG_W(
		    "ADR-037 §5 GL interop device (%s): no DXGI factory — falling back to the DXGI "
		    "default adapter (#1159)",
		    purpose);
		return out;
	}

	// Pass 1 — the driver's own answer. Exact when it is available.
	if (c->gl_context_luid_valid) {
		IDXGIAdapter1 *a = NULL;
		for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
			DXGI_ADAPTER_DESC1 d = {};
			if (SUCCEEDED(a->GetDesc1(&d)) && d.AdapterLuid.HighPart == c->gl_context_luid.HighPart &&
			    d.AdapterLuid.LowPart == c->gl_context_luid.LowPart) {
				out.adapter = a; // keep the reference
				out.luid = d.AdapterLuid;
				wcsncpy_s(out.description, d.Description, _TRUNCATE);
				out.provenance = "GL context LUID (GL_EXT_memory_object_win32)";
				out.confirmed_same_as_gl = true;
				factory->Release();
				return out;
			}
			a->Release();
		}
	}

	// Pass 2/3 — vendor id, then the renderer string to split a vendor tie.
	const UINT want_vendor = gl_vendor_id_from_gl_strings();
	if (want_vendor != 0) {
		IDXGIAdapter1 *vendor_hit = NULL;
		DXGI_ADAPTER_DESC1 vendor_desc = {};
		int vendor_count = 0;
		IDXGIAdapter1 *renderer_hit = NULL;
		DXGI_ADAPTER_DESC1 renderer_desc = {};
		int renderer_count = 0;

		IDXGIAdapter1 *a = NULL;
		for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
			DXGI_ADAPTER_DESC1 d = {};
			if (FAILED(a->GetDesc1(&d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 ||
			    d.VendorId != want_vendor) {
				a->Release();
				continue;
			}
			vendor_count++;
			if (vendor_hit == NULL) {
				a->AddRef();
				vendor_hit = a;
				vendor_desc = d;
			}
			if (gl_renderer_mentions(d.Description)) {
				renderer_count++;
				if (renderer_hit == NULL) {
					a->AddRef();
					renderer_hit = a;
					renderer_desc = d;
				}
			}
			a->Release();
		}

		// A renderer-string match is only trusted when it is unambiguous, and so
		// is the vendor match. Two same-vendor adapters that GL_RENDERER cannot
		// separate is exactly the case where guessing would be worse than saying
		// "unknown" and taking the §2 answer.
		if (renderer_count == 1) {
			out.adapter = renderer_hit;
			out.luid = renderer_desc.AdapterLuid;
			wcsncpy_s(out.description, renderer_desc.Description, _TRUNCATE);
			out.provenance = "GL_RENDERER description match (heuristic)";
			if (vendor_hit != NULL) {
				vendor_hit->Release();
			}
			factory->Release();
			return out;
		}
		if (renderer_hit != NULL) {
			renderer_hit->Release();
		}
		if (vendor_count == 1) {
			out.adapter = vendor_hit;
			out.luid = vendor_desc.AdapterLuid;
			wcsncpy_s(out.description, vendor_desc.Description, _TRUNCATE);
			out.provenance = "GL_VENDOR PCI vendor-id match (heuristic)";
			factory->Release();
			return out;
		}
		if (vendor_hit != NULL) {
			vendor_hit->Release();
		}
	}

	factory->Release();

	/*
	 * Pass 4 — no GL evidence. Take the ADR-037 §2 answer rather than NULL.
	 * It is not "the GL adapter"; it is a deliberate, ranked, overridable
	 * choice instead of DXGI enumeration order, and the log says which it is.
	 */
	const uint32_t panel_w =
	    (c->xdev != NULL && c->xdev->hmd != NULL) ? (uint32_t)c->xdev->hmd->screens[0].w_pixels : 0;
	const uint32_t panel_h =
	    (c->xdev != NULL && c->xdev->hmd != NULL) ? (uint32_t)c->xdev->hmd->screens[0].h_pixels : 0;
	xrt::auxiliary::d3d::RenderAdapterChoice choice = xrt::auxiliary::d3d::getRenderAdapter(
	    c->panel_screen_left, c->panel_screen_top, panel_w, panel_h, D3D_FEATURE_LEVEL_11_0, U_LOGGING_INFO);
	if (choice.adapter != nullptr) {
		DXGI_ADAPTER_DESC d = {};
		if (SUCCEEDED(choice.adapter->GetDesc(&d))) {
			out.adapter = choice.adapter.detach();
			out.luid = d.AdapterLuid;
			wcsncpy_s(out.description, d.Description, _TRUNCATE);
			out.provenance = choice.from_env ? "ADR-037 §2 fallback, env-forced (GL adapter unknown)"
			                                 : "ADR-037 §2 fallback (GL adapter unknown)";
		}
	}
	return out;
}

/*!
 * Log which adapter an interop device actually landed on, with LUID and
 * provenance (the rule PR #1023 established), and shout when it is not the GL
 * context's adapter. A cross-adapter WGL_NV_DX_interop share is a real
 * performance cliff, and before #1159 it was completely invisible.
 */
static void
gl_log_interop_placement(struct comp_gl_compositor *c, const char *purpose, const struct gl_interop_adapter *chosen)
{
	if (chosen->adapter == NULL) {
		U_LOG_W(
		    "ADR-037 §5 GL interop device (%s): adapter UNRESOLVED — created on the DXGI default "
		    "(#1159)",
		    purpose);
		return;
	}

	U_LOG_W("ADR-037 §5 GL interop device (%s): '%ls' LUID=%08lx:%08lx (%s) (#1159)", purpose, chosen->description,
	        (unsigned long)chosen->luid.HighPart, (unsigned long)chosen->luid.LowPart, chosen->provenance);

	if (c->gl_context_luid_valid && !chosen->confirmed_same_as_gl) {
		U_LOG_W(
		    "ADR-037 §5: GL interop device (%s) is on LUID=%08lx:%08lx but the GL context runs on "
		    "LUID=%08lx:%08lx — CROSS-ADAPTER interop; every share/copy crosses the bus, every frame "
		    "(#1159)",
		    purpose, (unsigned long)chosen->luid.HighPart, (unsigned long)chosen->luid.LowPart,
		    (unsigned long)c->gl_context_luid.HighPart, (unsigned long)c->gl_context_luid.LowPart);
	}
}

/*!
 * Once per GL session: what the GL context landed on, what ADR-037 §2 would
 * have picked, and the standing limitation that the runtime cannot make those
 * two agree. Without this line a GL session's placement reads like an
 * unexplained fallback instead of a documented one (ADR-037 §5).
 */
static void
gl_log_placement_advisory_once(struct comp_gl_compositor *c, int32_t screen_left, int32_t screen_top)
{
	c->panel_screen_left = screen_left;
	c->panel_screen_top = screen_top;
	c->gl_context_luid_valid = gl_query_context_luid(&c->gl_context_luid);

	const uint32_t panel_w =
	    (c->xdev != NULL && c->xdev->hmd != NULL) ? (uint32_t)c->xdev->hmd->screens[0].w_pixels : 0;
	const uint32_t panel_h =
	    (c->xdev != NULL && c->xdev->hmd != NULL) ? (uint32_t)c->xdev->hmd->screens[0].h_pixels : 0;
	xrt::auxiliary::d3d::RenderAdapterChoice render = xrt::auxiliary::d3d::getRenderAdapter(
	    screen_left, screen_top, panel_w, panel_h, D3D_FEATURE_LEVEL_11_0, U_LOGGING_INFO);

	DXGI_ADAPTER_DESC rdesc = {};
	bool have_render = render.adapter != nullptr && SUCCEEDED(render.adapter->GetDesc(&rdesc));

	if (c->gl_context_luid_valid) {
		const bool same = have_render && rdesc.AdapterLuid.HighPart == c->gl_context_luid.HighPart &&
		                  rdesc.AdapterLuid.LowPart == c->gl_context_luid.LowPart;
		U_LOG_W(
		    "ADR-037 §5 GL placement is OS-ADVISORY: OpenGL exposes no adapter-selection API, so the "
		    "driver (+ the per-exe UserGpuPreferences pin) decides. GL context is on LUID=%08lx:%08lx "
		    "(%s); ADR-037 §2 would pick '%ls' LUID=%08lx:%08lx (%s) — %s. The runtime follows the GL "
		    "context with its interop devices; it cannot move the context. (#1159)",
		    (unsigned long)c->gl_context_luid.HighPart, (unsigned long)c->gl_context_luid.LowPart,
		    glGetString != NULL ? (const char *)glGetString(GL_RENDERER) : "unknown renderer",
		    have_render ? rdesc.Description : L"<unavailable>",
		    have_render ? (unsigned long)rdesc.AdapterLuid.HighPart : 0UL,
		    have_render ? (unsigned long)rdesc.AdapterLuid.LowPart : 0UL,
		    have_render ? render.provenance : "unresolved",
		    same ? "MATCH" : (have_render ? "DIVERGES" : "unverified"));
	} else {
		U_LOG_W(
		    "ADR-037 §5 GL placement is OS-ADVISORY: OpenGL exposes no adapter-selection API, so the "
		    "driver (+ the per-exe UserGpuPreferences pin) decides. This driver does not report "
		    "GL_DEVICE_LUID_EXT (no GL_EXT_memory_object_win32), so the GL context's adapter is NOT "
		    "reliably knowable here — interop devices fall back to the GL_VENDOR/GL_RENDERER match, "
		    "then to ADR-037 §2 ('%ls', %s). Renderer: %s. (#1159)",
		    have_render ? rdesc.Description : L"<unavailable>", have_render ? render.provenance : "unresolved",
		    glGetString != NULL ? (const char *)glGetString(GL_RENDERER) : "unknown renderer");
	}
}

//! One (create device on `adapter`, open the app's shared texture) attempt.
//! Leaves nothing behind on failure.
static bool
gl_try_shared_texture_device(struct comp_gl_compositor *c, IDXGIAdapter *adapter, void *shared_texture_handle)
{
	ID3D11Device *dev = NULL;
	ID3D11DeviceContext *ctx = NULL;
	HRESULT hr = D3D11CreateDevice(adapter, adapter != NULL ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
	                               NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx);
	if (FAILED(hr) || dev == NULL) {
		if (ctx != NULL) {
			ctx->Release();
		}
		if (dev != NULL) {
			dev->Release();
		}
		return false;
	}

	ID3D11Texture2D *tex = NULL;
	hr = dev->OpenSharedResource((HANDLE)shared_texture_handle, __uuidof(ID3D11Texture2D), (void **)&tex);
	if (FAILED(hr) || tex == NULL) {
		ctx->Release();
		dev->Release();
		return false;
	}

	c->dx_device = dev;
	c->dx_context = ctx;
	c->dx_shared_texture = tex;
	return true;
}

/*!
 * Create the D3D11 device that opens and uploads the app's shared texture, on a
 * deliberately chosen adapter (#1159), and open the texture with it.
 *
 * `OpenSharedResource` is itself the placement oracle here: a D3D11 shared
 * handle can only be opened by a device on the **same adapter as the device
 * that created it**. So "the right adapter" for this device is not a
 * preference, it is a hard requirement set by the app — and success is proof.
 * The preferred adapter (the GL context's, per gl_resolve_interop_adapter) is
 * tried first because that is the one the rest of the compositor works on;
 * every other hardware adapter is then tried in turn, so a mismatch degrades to
 * a loud cross-adapter warning instead of a dead session. Before #1159 this was
 * a single NULL-adapter attempt: right by luck, or fatal.
 */
static bool
gl_create_shared_texture_device(struct comp_gl_compositor *c, void *shared_texture_handle)
{
	struct gl_interop_adapter placed = gl_resolve_interop_adapter(c, "shared-texture upload");
	gl_log_interop_placement(c, "shared-texture upload", &placed);

	bool tried_placed = false;
	if (placed.adapter != NULL) {
		bool ok = gl_try_shared_texture_device(c, placed.adapter, shared_texture_handle);
		placed.adapter->Release();
		placed.adapter = NULL;
		tried_placed = true;
		if (ok) {
			return true;
		}
		U_LOG_W(
		    "GL shared texture: could not be opened on the resolved adapter '%ls' LUID=%08lx:%08lx — "
		    "the app created it on a DIFFERENT adapter; searching (#1159)",
		    placed.description, (unsigned long)placed.luid.HighPart, (unsigned long)placed.luid.LowPart);
	}

	IDXGIFactory1 *factory = NULL;
	if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory)) && factory != NULL) {
		IDXGIAdapter1 *a = NULL;
		for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
			DXGI_ADAPTER_DESC1 d = {};
			if (FAILED(a->GetDesc1(&d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
				a->Release();
				continue;
			}
			if (tried_placed && d.AdapterLuid.HighPart == placed.luid.HighPart &&
			    d.AdapterLuid.LowPart == placed.luid.LowPart) {
				a->Release(); // already tried above
				continue;
			}
			bool ok = gl_try_shared_texture_device(c, a, shared_texture_handle);
			a->Release();
			if (ok) {
				factory->Release();
				/*
				 * Loud on purpose. The upload itself still works (it goes
				 * through system memory), but the app's present surface and
				 * the GL context are on different GPUs, so the frame crosses
				 * the bus once more than it should — and that is a placement
				 * fact about the app, not about us.
				 */
				U_LOG_W(
				    "ADR-037 §5: GL shared-texture device landed on '%ls' LUID=%08lx:%08lx, "
				    "which is NOT the GL context's adapter — CROSS-ADAPTER handoff; the app "
				    "created its present surface on another GPU (#1159)",
				    d.Description, (unsigned long)d.AdapterLuid.HighPart,
				    (unsigned long)d.AdapterLuid.LowPart);
				return true;
			}
		}
		factory->Release();
	}

	// Last resort: exactly the pre-#1159 call, so nothing that used to work stops.
	if (gl_try_shared_texture_device(c, NULL, shared_texture_handle)) {
		U_LOG_W(
		    "GL shared texture: opened on the DXGI default adapter after the resolved and enumerated "
		    "adapters all failed (#1159)");
		return true;
	}

	return false;
}
#endif // XRT_OS_WINDOWS


/*
 *
 * Transparent-background present path (Windows: DComp + WGL_NV_DX_interop2).
 *
 * Mirrors what the D3D11/D3D12/VK native compositors do for transparent desktop
 * composition: route the compositor's output through a flip-model DXGI swapchain
 * created with DXGI_ALPHA_MODE_PREMULTIPLIED and bound to the app's HWND via
 * DirectComposition, so DWM blends per-pixel alpha (alpha-0 pixels show the
 * desktop through the window) without any chroma-key trick.
 *
 * GL is special: it can't render to a DXGI swapchain back buffer directly. A
 * prior attempt (PR #3b, reverted in commit 670d0158d) registered the
 * flip-model back buffers themselves as GL FBOs via wglDXRegisterObjectNV and
 * had the DP weave straight into them — on the dev hardware (RTX 3080 + Win11)
 * those GL writes never became visible. That is the known WGL_NV_DX_interop2 ->
 * *flip-model* swapchain back-buffer incompatibility. The revert confirmed the
 * useful split: D3D11 RTV writes (ClearRenderTargetView) to the back buffer DID
 * present; only GL-interop writes (and CopyResource) into it failed.
 *
 * So this path composes the two halves that ARE proven on this hardware:
 *   1. GL weaves into an OFF-SCREEN interop transit texture — the exact path the
 *      runtime already ships for _texture apps (register an off-screen
 *      ID3D11Texture2D as a GL FBO, lock / render / unlock).
 *   2. A D3D11 fullscreen-triangle shader blit (an RTV write — proven) samples
 *      that transit texture and draws it into the flip-model DComp back buffer.
 *   3. swapchain->Present + dcomp_device->Commit.
 * No GL write to a flip-model back buffer; no CopyResource.
 *
 * Gated on (transparent_background && hwnd != NULL && !owns_window): the app's
 * HWND must carry WS_EX_NOREDIRECTIONBITMAP (set by the app, e.g.
 * cube_handle_gl_win), and runtime-hosted GL windows are out of scope for now.
 * Any setup failure (no WGL_NV_DX_interop2, no DComp, etc.) leaves dcomp_active
 * false and the compositor falls back to the opaque SwapBuffers path.
 */

#ifdef XRT_OS_WINDOWS
// Inline HLSL for the transit-texture -> back-buffer blit. Fullscreen triangle
// from SV_VertexID; V is flipped because the GL transit texture is bottom-up.
static const char *kDcompBlitHLSL =
    "Texture2D gTex : register(t0);\n"
    "SamplerState gSamp : register(s0);\n"
    "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
    "VSOut vs_main(uint vid : SV_VertexID) {\n"
    "  VSOut o;\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2);\n"      // (0,0),(2,0),(0,2)
    "  o.pos = float4(p * float2(2,-2) + float2(-1,1), 0, 1);\n"
    "  o.uv  = float2(p.x, 1.0 - p.y);\n"                  // flip V (GL bottom-up)
    "  return o;\n"
    "}\n"
    "float4 ps_main(VSOut i) : SV_TARGET {\n"
    "  return gTex.Sample(gSamp, i.uv);\n"                 // already premultiplied
    "}\n";

// Tear down the DComp present path. Safe to call when !dcomp_active.
static void
gl_destroy_dcomp_present(struct comp_gl_compositor *c)
{
	if (c->dcomp_transit_iop != NULL && c->pfn_wglDXUnregisterObjectNV != NULL &&
	    c->dcomp_dx_interop_device != NULL) {
		c->pfn_wglDXUnregisterObjectNV(c->dcomp_dx_interop_device, c->dcomp_transit_iop);
		c->dcomp_transit_iop = NULL;
	}
	if (c->dcomp_transit_fbo != 0) {
		glDeleteFramebuffers(1, &c->dcomp_transit_fbo);
		c->dcomp_transit_fbo = 0;
	}
	if (c->dcomp_transit_gl_tex != 0) {
		glDeleteTextures(1, &c->dcomp_transit_gl_tex);
		c->dcomp_transit_gl_tex = 0;
	}
	if (c->dcomp_dx_interop_device != NULL && c->pfn_wglDXCloseDeviceNV != NULL) {
		c->pfn_wglDXCloseDeviceNV(c->dcomp_dx_interop_device);
		c->dcomp_dx_interop_device = NULL;
	}
	// P0.3 (#573) — readback fallback resources.
	if (c->dcomp_readback_gl_fbo != 0) { glDeleteFramebuffers(1, &c->dcomp_readback_gl_fbo); c->dcomp_readback_gl_fbo = 0; }
	if (c->dcomp_readback_gl_tex != 0) { glDeleteTextures(1, &c->dcomp_readback_gl_tex);     c->dcomp_readback_gl_tex = 0; }
	if (c->dcomp_readback_srv) { c->dcomp_readback_srv->Release(); c->dcomp_readback_srv = NULL; }
	if (c->dcomp_readback_tex) { c->dcomp_readback_tex->Release(); c->dcomp_readback_tex = NULL; }
	if (c->dcomp_readback_cpu) { free(c->dcomp_readback_cpu);      c->dcomp_readback_cpu = NULL; }
	if (c->dcomp_samp)         { c->dcomp_samp->Release();         c->dcomp_samp = NULL; }
	if (c->dcomp_ps)           { c->dcomp_ps->Release();           c->dcomp_ps = NULL; }
	if (c->dcomp_vs)           { c->dcomp_vs->Release();           c->dcomp_vs = NULL; }
	if (c->dcomp_transit_srv)  { c->dcomp_transit_srv->Release();  c->dcomp_transit_srv = NULL; }
	if (c->dcomp_transit_tex)  { c->dcomp_transit_tex->Release();  c->dcomp_transit_tex = NULL; }
	if (c->dcomp_visual)       { c->dcomp_visual->Release();       c->dcomp_visual = NULL; }
	if (c->dcomp_target)       { c->dcomp_target->Release();       c->dcomp_target = NULL; }
	if (c->dcomp_device)       { c->dcomp_device->Release();       c->dcomp_device = NULL; }
	if (c->dcomp_swapchain)    { c->dcomp_swapchain->Release();    c->dcomp_swapchain = NULL; }
	if (c->dcomp_dx_context)   { c->dcomp_dx_context->Release();   c->dcomp_dx_context = NULL; }
	if (c->dcomp_dx_device)    { c->dcomp_dx_device->Release();    c->dcomp_dx_device = NULL; }
	c->dcomp_active = false;
	c->dcomp_readback_active = false;
}

// Shared D3D11 / DComp / blit setup used by BOTH the interop and the no-interop
// readback present paths (#573 P0.3): a dedicated D3D11 device, a flip-model
// PREMULTIPLIED composition swapchain bound to the HWND via DirectComposition, and
// the fullscreen-triangle blit pipeline. The source texture and per-frame upload
// differ between the two paths and are set up by the callers. Returns false (left
// torn down) on any failure.
static bool
gl_setup_dcomp_common(struct comp_gl_compositor *c, HWND hwnd, uint32_t w, uint32_t h)
{
	// 1. Dedicated D3D11 device for the present bridge, on a DELIBERATELY chosen
	//    adapter (#1159). This device is handed to wglDXOpenDeviceNV by the
	//    interop path, so it has to be the GL context's adapter or the share is
	//    cross-adapter — and on the readback fallback it is the device that
	//    receives every uploaded frame. Passing NULL here used to let DXGI
	//    enumeration order decide. An unresolved adapter (NULL) still works:
	//    that is exactly today's behaviour, and it is logged as such.
	struct gl_interop_adapter placed = gl_resolve_interop_adapter(c, "transparency present bridge");
	gl_log_interop_placement(c, "transparency present bridge", &placed);
	// D3D_DRIVER_TYPE_UNKNOWN is mandatory when an adapter is supplied.
	HRESULT hr = D3D11CreateDevice(
	    placed.adapter, placed.adapter != NULL ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL,
	    0, D3D11_SDK_VERSION, &c->dcomp_dx_device, NULL, &c->dcomp_dx_context);
	if (placed.adapter != NULL) {
		placed.adapter->Release();
		placed.adapter = NULL;
	}
	if (FAILED(hr) || c->dcomp_dx_device == NULL) {
		U_LOG_W("Transparent GL: D3D11CreateDevice failed: 0x%08x — staying opaque", (unsigned)hr);
		gl_destroy_dcomp_present(c);
		return false;
	}

	// 2. Flip-model composition swapchain (PREMULTIPLIED alpha).
	IDXGIFactory2 *factory = NULL;
	hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), (void **)&factory);
	if (FAILED(hr) || factory == NULL) {
		U_LOG_W("Transparent GL: CreateDXGIFactory2 failed: 0x%08x", (unsigned)hr);
		gl_destroy_dcomp_present(c);
		return false;
	}
	DXGI_SWAP_CHAIN_DESC1 scd = {};
	scd.Width = w;
	scd.Height = h;
	scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	scd.SampleDesc.Count = 1;
	scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	scd.BufferCount = 2;
	scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	scd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
	hr = factory->CreateSwapChainForComposition(c->dcomp_dx_device, &scd, NULL,
	                                            &c->dcomp_swapchain);
	factory->Release();
	if (FAILED(hr) || c->dcomp_swapchain == NULL) {
		U_LOG_W("Transparent GL: CreateSwapChainForComposition failed: 0x%08x", (unsigned)hr);
		gl_destroy_dcomp_present(c);
		return false;
	}

	// 3. Bind the swapchain to the HWND through DirectComposition.
	hr = DCompositionCreateDevice2(NULL, __uuidof(IDCompositionDevice),
	                               (void **)&c->dcomp_device);
	if (SUCCEEDED(hr)) hr = c->dcomp_device->CreateTargetForHwnd(hwnd, TRUE, &c->dcomp_target);
	if (SUCCEEDED(hr)) hr = c->dcomp_device->CreateVisual(&c->dcomp_visual);
	if (SUCCEEDED(hr)) hr = c->dcomp_visual->SetContent(c->dcomp_swapchain);
	if (SUCCEEDED(hr)) hr = c->dcomp_target->SetRoot(c->dcomp_visual);
	if (SUCCEEDED(hr)) hr = c->dcomp_device->Commit();
	if (FAILED(hr)) {
		U_LOG_W("Transparent GL: DirectComposition bind failed: 0x%08x", (unsigned)hr);
		gl_destroy_dcomp_present(c);
		return false;
	}

	// 4. Compile the fullscreen-triangle blit pipeline.
	ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
	hr = D3DCompile(kDcompBlitHLSL, strlen(kDcompBlitHLSL), "dcomp_blit", NULL, NULL,
	                "vs_main", "vs_5_0", 0, 0, &vsb, &err);
	if (FAILED(hr)) {
		U_LOG_W("Transparent GL: blit VS compile failed: 0x%08x %s", (unsigned)hr,
		        err ? (const char *)err->GetBufferPointer() : "");
		if (err) err->Release();
		gl_destroy_dcomp_present(c);
		return false;
	}
	if (err) { err->Release(); err = NULL; }
	hr = D3DCompile(kDcompBlitHLSL, strlen(kDcompBlitHLSL), "dcomp_blit", NULL, NULL,
	                "ps_main", "ps_5_0", 0, 0, &psb, &err);
	if (FAILED(hr)) {
		U_LOG_W("Transparent GL: blit PS compile failed: 0x%08x %s", (unsigned)hr,
		        err ? (const char *)err->GetBufferPointer() : "");
		if (err) err->Release();
		vsb->Release();
		gl_destroy_dcomp_present(c);
		return false;
	}
	if (err) { err->Release(); err = NULL; }
	hr = c->dcomp_dx_device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(),
	                                            NULL, &c->dcomp_vs);
	if (SUCCEEDED(hr))
		hr = c->dcomp_dx_device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(),
		                                          NULL, &c->dcomp_ps);
	vsb->Release();
	psb->Release();
	if (FAILED(hr)) {
		U_LOG_W("Transparent GL: blit shader create failed: 0x%08x", (unsigned)hr);
		gl_destroy_dcomp_present(c);
		return false;
	}
	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	hr = c->dcomp_dx_device->CreateSamplerState(&sd, &c->dcomp_samp);
	if (FAILED(hr)) {
		U_LOG_W("Transparent GL: blit sampler failed: 0x%08x", (unsigned)hr);
		gl_destroy_dcomp_present(c);
		return false;
	}

	c->dcomp_present_w = w;
	c->dcomp_present_h = h;
	return true;
}

// No-interop readback present path (#573 P0.3). For near-extinct hardware/drivers
// without WGL_NV_DX_interop2: GL can't share a texture with D3D11, so the weave
// stays on the default framebuffer and each frame we glReadPixels it → upload to a
// DYNAMIC D3D11 texture → reuse the same DComp blit. Slow (a full CPU round-trip),
// but it closes the last see-through gap so chroma-key can be deleted everywhere.
static bool
gl_setup_dcomp_readback_present(struct comp_gl_compositor *c, HWND hwnd, uint32_t w, uint32_t h)
{
	if (w == 0 || h == 0) {
		return false;
	}
	if (!gl_setup_dcomp_common(c, hwnd, w, h)) {
		return false;
	}

	// DYNAMIC (CPU-write) source texture + SRV. Uploaded from glReadPixels each
	// frame; row 0 = GL bottom row, matching the interop transit's orientation so
	// the existing blit shader's V-flip is correct.
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DYNAMIC;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	HRESULT hr = c->dcomp_dx_device->CreateTexture2D(&td, NULL, &c->dcomp_readback_tex);
	if (FAILED(hr)) {
		U_LOG_W("Transparent GL: readback CreateTexture2D failed: 0x%08x", (unsigned)hr);
		gl_destroy_dcomp_present(c);
		return false;
	}
	hr = c->dcomp_dx_device->CreateShaderResourceView(c->dcomp_readback_tex, NULL,
	                                                  &c->dcomp_readback_srv);
	if (FAILED(hr)) {
		U_LOG_W("Transparent GL: readback SRV failed: 0x%08x", (unsigned)hr);
		gl_destroy_dcomp_present(c);
		return false;
	}
	c->dcomp_readback_cpu = (uint8_t *)malloc((size_t)w * h * 4);
	if (c->dcomp_readback_cpu == NULL) {
		U_LOG_W("Transparent GL: readback CPU buffer alloc failed (%ux%u)", w, h);
		gl_destroy_dcomp_present(c);
		return false;
	}

	// Dedicated RGBA8 GL weave target + FBO (alpha-preserving, unlike FBO 0).
	glGenTextures(1, &c->dcomp_readback_gl_tex);
	glBindTexture(GL_TEXTURE_2D, c->dcomp_readback_gl_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
	glGenFramebuffers(1, &c->dcomp_readback_gl_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, c->dcomp_readback_gl_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                       c->dcomp_readback_gl_tex, 0);
	GLenum fbst = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (fbst != GL_FRAMEBUFFER_COMPLETE) {
		U_LOG_W("Transparent GL: readback FBO incomplete: 0x%x", (unsigned)fbst);
		gl_destroy_dcomp_present(c);
		return false;
	}

	c->dcomp_readback_active = true;
	U_LOG_W("Transparent GL: NO-INTEROP readback present path active (%ux%u) — glReadPixels → "
	        "D3D11 dynamic upload → DComp blit (slow fallback for hardware without "
	        "WGL_NV_DX_interop2)", w, h);
	return true;
}

// Initialize the DComp + WGL_NV_DX_interop2 present path. Returns false on any
// failure (caller stays on the opaque SwapBuffers path); always leaves the
// struct in a consistent torn-down state on false. When WGL_NV_DX_interop2 is
// unavailable (or DISPLAYXR_GL_FORCE_READBACK is set), falls back to the no-interop
// glReadPixels readback path (#573 P0.3) instead of staying opaque.
static bool
gl_setup_dcomp_present(struct comp_gl_compositor *c, HWND hwnd, uint32_t w, uint32_t h)
{
	if (w == 0 || h == 0) {
		return false;
	}

	// Debug/verification knob: force the no-interop readback path even on hardware
	// that has WGL_NV_DX_interop2, so the fallback can be exercised on dev GPUs.
	const char *force_readback = getenv("DISPLAYXR_GL_FORCE_READBACK");
	if (force_readback != NULL && *force_readback != '\0' && *force_readback != '0') {
		U_LOG_W("Transparent GL: DISPLAYXR_GL_FORCE_READBACK set — using no-interop readback path");
		return gl_setup_dcomp_readback_present(c, hwnd, w, h);
	}

	// 1. WGL_NV_DX_interop2 entry points (may already be loaded by the
	//    shared-texture path; load defensively).
	if (c->pfn_wglDXOpenDeviceNV == NULL) {
		c->pfn_wglDXOpenDeviceNV = (PFN_wglDXOpenDeviceNV)wglGetProcAddress("wglDXOpenDeviceNV");
		c->pfn_wglDXCloseDeviceNV = (PFN_wglDXCloseDeviceNV)wglGetProcAddress("wglDXCloseDeviceNV");
		c->pfn_wglDXRegisterObjectNV = (PFN_wglDXRegisterObjectNV)wglGetProcAddress("wglDXRegisterObjectNV");
		c->pfn_wglDXUnregisterObjectNV = (PFN_wglDXUnregisterObjectNV)wglGetProcAddress("wglDXUnregisterObjectNV");
		c->pfn_wglDXLockObjectsNV = (PFN_wglDXLockObjectsNV)wglGetProcAddress("wglDXLockObjectsNV");
		c->pfn_wglDXUnlockObjectsNV = (PFN_wglDXUnlockObjectsNV)wglGetProcAddress("wglDXUnlockObjectsNV");
	}
	if (c->pfn_wglDXOpenDeviceNV == NULL || c->pfn_wglDXRegisterObjectNV == NULL ||
	    c->pfn_wglDXLockObjectsNV == NULL || c->pfn_wglDXUnlockObjectsNV == NULL ||
	    c->pfn_wglDXCloseDeviceNV == NULL || c->pfn_wglDXUnregisterObjectNV == NULL) {
		U_LOG_W("Transparent GL: WGL_NV_DX_interop2 unavailable on this GPU/driver — "
		        "using no-interop readback present path");
		return gl_setup_dcomp_readback_present(c, hwnd, w, h);
	}

	// 2. Dedicated D3D11 device + swapchain + DComp bind + blit pipeline (shared).
	if (!gl_setup_dcomp_common(c, hwnd, w, h)) {
		return false;
	}

	// 3. Open the D3D11 device for GL interop.
	c->dcomp_dx_interop_device = c->pfn_wglDXOpenDeviceNV(c->dcomp_dx_device);
	if (c->dcomp_dx_interop_device == NULL) {
		U_LOG_W("Transparent GL: wglDXOpenDeviceNV failed: %lu — staying opaque", GetLastError());
		gl_destroy_dcomp_present(c);
		return false;
	}

	// 4. Off-screen transit texture (RT + SRV) and its GL interop view + FBO.
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	HRESULT hr = c->dcomp_dx_device->CreateTexture2D(&td, NULL, &c->dcomp_transit_tex);
	if (FAILED(hr)) {
		U_LOG_W("Transparent GL: transit CreateTexture2D failed: 0x%08x", (unsigned)hr);
		gl_destroy_dcomp_present(c);
		return false;
	}
	hr = c->dcomp_dx_device->CreateShaderResourceView(c->dcomp_transit_tex, NULL,
	                                                  &c->dcomp_transit_srv);
	if (FAILED(hr)) {
		U_LOG_W("Transparent GL: transit SRV failed: 0x%08x", (unsigned)hr);
		gl_destroy_dcomp_present(c);
		return false;
	}
	glGenTextures(1, &c->dcomp_transit_gl_tex);
	c->dcomp_transit_iop = c->pfn_wglDXRegisterObjectNV(
	    c->dcomp_dx_interop_device, c->dcomp_transit_tex, c->dcomp_transit_gl_tex,
	    GL_TEXTURE_2D, WGL_ACCESS_READ_WRITE_NV);
	if (c->dcomp_transit_iop == NULL) {
		U_LOG_W("Transparent GL: wglDXRegisterObjectNV(transit) failed: %lu", GetLastError());
		gl_destroy_dcomp_present(c);
		return false;
	}
	// Build the FBO around the transit GL texture (lock while attaching).
	glGenFramebuffers(1, &c->dcomp_transit_fbo);
	c->pfn_wglDXLockObjectsNV(c->dcomp_dx_interop_device, 1, &c->dcomp_transit_iop);
	glBindFramebuffer(GL_FRAMEBUFFER, c->dcomp_transit_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                       c->dcomp_transit_gl_tex, 0);
	GLenum fbst = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	c->pfn_wglDXUnlockObjectsNV(c->dcomp_dx_interop_device, 1, &c->dcomp_transit_iop);
	if (fbst != GL_FRAMEBUFFER_COMPLETE) {
		U_LOG_W("Transparent GL: transit FBO incomplete: 0x%x", (unsigned)fbst);
		gl_destroy_dcomp_present(c);
		return false;
	}

	c->dcomp_active = true;
	U_LOG_W("Transparent GL: DComp present path active (%ux%u, FLIP_DISCARD + PREMULTIPLIED + "
	        "off-screen interop transit + D3D11 blit)", w, h);
	return true;
}

// Shared D3D11 blit-and-present: draw @p srv into the current DComp back buffer via
// the fullscreen-triangle pipeline, then Present + Commit. Used by both the interop
// (transit SRV) and readback (dynamic SRV) present paths.
static void
gl_dcomp_blit_srv_present(struct comp_gl_compositor *c, ID3D11ShaderResourceView *srv)
{
	ID3D11Texture2D *bb = NULL;
	HRESULT hr = c->dcomp_swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
	if (FAILED(hr) || bb == NULL) {
		U_LOG_W("Transparent GL: GetBuffer failed: 0x%08x", (unsigned)hr);
		return;
	}
	ID3D11RenderTargetView *rtv = NULL;
	hr = c->dcomp_dx_device->CreateRenderTargetView(bb, NULL, &rtv);
	bb->Release();
	if (FAILED(hr) || rtv == NULL) {
		U_LOG_W("Transparent GL: back-buffer RTV failed: 0x%08x", (unsigned)hr);
		return;
	}

	ID3D11DeviceContext *ctx = c->dcomp_dx_context;
	D3D11_VIEWPORT vp = {};
	vp.Width = (float)c->dcomp_present_w;
	vp.Height = (float)c->dcomp_present_h;
	vp.MaxDepth = 1.0f;
	ctx->OMSetRenderTargets(1, &rtv, NULL);
	ctx->RSSetViewports(1, &vp);
	ctx->IASetInputLayout(NULL);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ctx->VSSetShader(c->dcomp_vs, NULL, 0);
	ctx->PSSetShader(c->dcomp_ps, NULL, 0);
	ctx->PSSetShaderResources(0, 1, &srv);
	ctx->PSSetSamplers(0, 1, &c->dcomp_samp);
	ctx->Draw(3, 0);
	// Unbind the SRV so the next frame's interop lock isn't held by the context.
	ID3D11ShaderResourceView *nullsrv = NULL;
	ctx->PSSetShaderResources(0, 1, &nullsrv);
	ctx->OMSetRenderTargets(0, NULL, NULL);
	ctx->Flush();
	rtv->Release();

	hr = c->dcomp_swapchain->Present(1, 0);
	if (FAILED(hr)) {
		U_LOG_W("Transparent GL: swapchain Present failed: 0x%08x", (unsigned)hr);
	}
	c->dcomp_device->Commit();
}

// Per-frame (interop path): blit the (already GL-woven) transit texture into the
// current DComp back buffer, then Present + Commit.
static void
gl_dcomp_present_frame(struct comp_gl_compositor *c)
{
	gl_dcomp_blit_srv_present(c, c->dcomp_transit_srv);
}

// Per-frame (no-interop readback path): read the woven default framebuffer back to
// the CPU, upload into the DYNAMIC D3D11 texture, then blit + Present + Commit.
static void
gl_dcomp_readback_present_frame(struct comp_gl_compositor *c)
{
	const uint32_t w = c->dcomp_present_w, h = c->dcomp_present_h;
	if (c->dcomp_readback_cpu == NULL || c->dcomp_readback_tex == NULL) {
		return;
	}

	// 1. Read the dedicated RGBA weave FBO (bottom-up, alpha preserved — unlike
	//    FBO 0). Bind it explicitly as the read framebuffer so we don't depend on
	//    whatever was last bound.
	glBindFramebuffer(GL_READ_FRAMEBUFFER, c->dcomp_readback_gl_fbo);
	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, c->dcomp_readback_cpu);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);

	// 2. Upload to the DYNAMIC texture (respect the driver's row pitch). Row 0 =
	//    glReadPixels bottom row = the interop transit's orientation, so the blit
	//    shader's V-flip yields the correct image.
	D3D11_MAPPED_SUBRESOURCE map = {};
	HRESULT hr = c->dcomp_dx_context->Map(c->dcomp_readback_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &map);
	if (FAILED(hr)) {
		U_LOG_W("Transparent GL: readback Map failed: 0x%08x", (unsigned)hr);
		return;
	}
	const uint8_t *src = c->dcomp_readback_cpu;
	uint8_t *dst = (uint8_t *)map.pData;
	const size_t row_bytes = (size_t)w * 4;
	for (uint32_t y = 0; y < h; ++y) {
		memcpy(dst + (size_t)y * map.RowPitch, src + (size_t)y * row_bytes, row_bytes);
	}
	c->dcomp_dx_context->Unmap(c->dcomp_readback_tex, 0);

	// 3. Blit + Present through DComp.
	gl_dcomp_blit_srv_present(c, c->dcomp_readback_srv);

	// Log the per-frame readback cost once (it's the whole reason this path is the
	// last-resort fallback).
	static int logged = 0;
	if (!logged) {
		logged = 1;
		U_LOG_W("Transparent GL: readback present active — full %ux%u glReadPixels + CPU upload "
		        "per frame (no-interop fallback cost)", w, h);
	}
}
#endif // XRT_OS_WINDOWS



/*
 *
 * GL helpers
 *
 */

static GLuint
compile_shader(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);

	GLint ok = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log_buf[512];
		glGetShaderInfoLog(shader, sizeof(log_buf), NULL, log_buf);
		U_LOG_E("Shader compile error: %s", log_buf);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static GLuint
create_program(const char *vs_src, const char *fs_src)
{
	GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
	GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
	if (!vs || !fs) {
		if (vs) glDeleteShader(vs);
		if (fs) glDeleteShader(fs);
		return 0;
	}

	GLuint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint ok = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log_buf[512];
		glGetProgramInfoLog(prog, sizeof(log_buf), NULL, log_buf);
		U_LOG_E("Program link error: %s", log_buf);
		glDeleteProgram(prog);
		return 0;
	}
	return prog;
}

static GLenum
xrt_format_to_gl_internal(int64_t fmt)
{
	// GL internal format enums
	switch (fmt) {
	case 0x8058: return GL_RGBA8;          // GL_RGBA8
	case 0x8C43: return GL_SRGB8_ALPHA8;   // GL_SRGB8_ALPHA8
	case 0x881A: return GL_RGBA16F;         // GL_RGBA16F
	case 0x8814: return GL_RGBA32F;         // GL_RGBA32F
	default:     return GL_RGBA8;
	}
}

// GL_EXT_texture_sRGB_decode — not in our GLAD spec, so define the enums.
#ifndef GL_TEXTURE_SRGB_DECODE_EXT
#define GL_TEXTURE_SRGB_DECODE_EXT 0x8A48
#endif
#ifndef GL_SKIP_DECODE_EXT
#define GL_SKIP_DECODE_EXT 0x8A4A
#endif

// True if GL_EXT_texture_sRGB_decode is present (cached; needs a current context).
static bool
gl_has_srgb_decode_ext(void)
{
	static int cached = -1;
	if (cached >= 0) {
		return cached != 0;
	}
	cached = 0;
	GLint n = 0;
	glGetIntegerv(GL_NUM_EXTENSIONS, &n);
	for (GLint i = 0; i < n; i++) {
		const GLubyte *e = glGetStringi(GL_EXTENSIONS, (GLuint)i);
		if (e != NULL && strcmp((const char *)e, "GL_EXT_texture_sRGB_decode") == 0) {
			cached = 1;
			break;
		}
	}
	return cached != 0;
}


/*
 *
 * Swapchain functions
 *
 */

static void
gl_swapchain_destroy(struct xrt_swapchain *xsc)
{
	struct comp_gl_swapchain *sc = gl_swapchain(xsc);
	if (sc->image_count > 0) {
		glDeleteTextures(sc->image_count, sc->textures);
	}
	free(sc);
}

static xrt_result_t
gl_swapchain_acquire_image(struct xrt_swapchain *xsc, uint32_t *out_index)
{
	struct comp_gl_swapchain *sc = gl_swapchain(xsc);
	uint32_t next = (sc->last_released_index + 1) % sc->image_count;
	sc->acquired_index = (int32_t)next;
	*out_index = next;
	return XRT_SUCCESS;
}

static xrt_result_t
gl_swapchain_wait_image(struct xrt_swapchain *xsc, int64_t timeout_ns, uint32_t index)
{
	struct comp_gl_swapchain *sc = gl_swapchain(xsc);
	sc->waited_index = (int32_t)index;
	return XRT_SUCCESS;
}

static xrt_result_t
gl_swapchain_release_image(struct xrt_swapchain *xsc, uint32_t index)
{
	struct comp_gl_swapchain *sc = gl_swapchain(xsc);
	sc->last_released_index = index;
	sc->acquired_index = -1;
	sc->waited_index = -1;
	return XRT_SUCCESS;
}

static xrt_result_t
gl_swapchain_barrier_image(struct xrt_swapchain *xsc,
                           enum xrt_barrier_direction direction,
                           uint32_t index)
{
	(void)xsc;
	(void)direction;
	(void)index;
	return XRT_SUCCESS;
}

static xrt_result_t
gl_swapchain_inc_image_use(struct xrt_swapchain *xsc, uint32_t index)
{
	(void)xsc;
	(void)index;
	return XRT_SUCCESS;
}

static xrt_result_t
gl_swapchain_dec_image_use(struct xrt_swapchain *xsc, uint32_t index)
{
	(void)xsc;
	(void)index;
	return XRT_SUCCESS;
}


/*
 *
 * Compositor functions
 *
 */

static xrt_result_t
gl_compositor_get_swapchain_create_properties(struct xrt_compositor *xc,
                                               const struct xrt_swapchain_create_info *info,
                                               struct xrt_swapchain_create_properties *xsccp)
{
	xsccp->image_count = 3;
	xsccp->extra_bits = (enum xrt_swapchain_usage_bits)0;
	return XRT_SUCCESS;
}

static xrt_result_t
gl_compositor_create_swapchain(struct xrt_compositor *xc,
                                const struct xrt_swapchain_create_info *info,
                                struct xrt_swapchain **out_xsc)
{
	struct comp_gl_compositor *c = gl_comp(xc);

	/*
	 * #885 root cause: this claim of the compositor context was UNLOCKED and
	 * UNCHECKED. A WGL context can be current on one thread at a time, and the
	 * #868 repaint thread claims c->hglrc every tick — zones apps create their
	 * zone/strip swapchains seconds into the session, so on a coin-flip of
	 * launches a create landed while the repaint held the context, the
	 * wglMakeCurrent below failed SILENTLY, and the swapchain textures were
	 * created on whatever context was current on the app thread — invisible to
	 * the compositor forever (that zone weaves black from the first frame).
	 * Serialize with the repaint via c->mutex (the repaint holds it across its
	 * claim..release) and check the claim.
	 */
	os_mutex_lock(&c->mutex);

	// Ensure compositor's GL context is current for texture creation
#ifdef XRT_OS_WINDOWS
	HDC prev_hdc = wglGetCurrentDC();
	HGLRC prev_hglrc = wglGetCurrentContext();
	if (!wglMakeCurrent(c->hdc, c->hglrc)) {
		os_mutex_unlock(&c->mutex);
		U_LOG_W("create_swapchain: wglMakeCurrent(compositor ctx) FAILED (err=%lu) — "
		        "refusing to create swapchain textures on the wrong context",
		        (unsigned long)GetLastError());
		return XRT_ERROR_ALLOCATION;
	}
#elif defined(__APPLE__)
	CGLContextObj prev_cgl_ctx = CGLGetCurrentContext();
	comp_gl_window_macos_make_current(c->macos_window);
#endif

	uint32_t image_count = 3;
	if (image_count > GL_SWAPCHAIN_MAX_IMAGES) {
		image_count = GL_SWAPCHAIN_MAX_IMAGES;
	}

	struct comp_gl_swapchain *sc = U_TYPED_CALLOC(struct comp_gl_swapchain);
	sc->image_count = image_count;
	sc->info = *info;
	sc->acquired_index = -1;
	sc->waited_index = -1;
	sc->last_released_index = 0;

	// Create GL textures. Layered (arraySize>1) swapchains allocate a
	// GL_TEXTURE_2D_ARRAY with `array_size` slices — under single-pass-instanced
	// the app renders the two eyes into slices 0/1 of one array texture and
	// submits them as two projection views with imageArrayIndex 0/1. Non-layered
	// swapchains keep the GL_TEXTURE_2D path unchanged.
	GLenum internal_format = xrt_format_to_gl_internal(info->format);
	uint32_t array_size = info->array_size > 0 ? info->array_size : 1;
	sc->target = array_size > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
	glGenTextures(image_count, sc->textures);

	for (uint32_t i = 0; i < image_count; i++) {
		glBindTexture(sc->target, sc->textures[i]);
		if (sc->target == GL_TEXTURE_2D_ARRAY) {
			glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, internal_format,
			             info->width, info->height, array_size, 0,
			             GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		} else {
			glTexImage2D(GL_TEXTURE_2D, 0, internal_format,
			             info->width, info->height, 0,
			             GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		}
		glTexParameteri(sc->target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(sc->target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(sc->target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(sc->target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		// sRGB passthrough: apps write display-referred bytes into the sRGB
		// swapchain image (typically with GL_FRAMEBUFFER_SRGB off). When the
		// compositor later samples it, a GL_SRGB8_ALPHA8 texture would auto
		// decode sRGB->linear, and since compose writes to a non-sRGB atlas
		// with no re-encode the DP receives ~2.2x-too-dark content. The Leia
		// DP expects sRGB-encoded bytes, so skip the sample-time decode and
		// pass the stored bytes through unchanged. This is correct for apps
		// that DO enable GL_FRAMEBUFFER_SRGB too (their encoded bytes also
		// pass through). Only the in-process native GL path samples these
		// textures, so this never affects app-side rendering.
		if (internal_format == GL_SRGB8_ALPHA8 && gl_has_srgb_decode_ext()) {
			glTexParameteri(sc->target, GL_TEXTURE_SRGB_DECODE_EXT, GL_SKIP_DECODE_EXT);
		}

		// Store GL texture name in the swapchain_gl images array
		// (this is what the state tracker reads via xrt_swapchain_gl)
		sc->base.images[i] = sc->textures[i];
	}
	glBindTexture(sc->target, 0);

	// Set up vtable
	sc->base.base.destroy = gl_swapchain_destroy;
	sc->base.base.acquire_image = gl_swapchain_acquire_image;
	sc->base.base.wait_image = gl_swapchain_wait_image;
	sc->base.base.release_image = gl_swapchain_release_image;
	sc->base.base.barrier_image = gl_swapchain_barrier_image;
	sc->base.base.inc_image_use = gl_swapchain_inc_image_use;
	sc->base.base.dec_image_use = gl_swapchain_dec_image_use;
	sc->base.base.image_count = image_count;
	sc->base.base.reference.count = 1;

	*out_xsc = &sc->base.base;

	U_LOG_W("Created GL swapchain: %ux%u, %u images, format 0x%x",
	         info->width, info->height, image_count, (unsigned)info->format);

	// Restore previous GL context
#ifdef XRT_OS_WINDOWS
	if (prev_hglrc != NULL) {
		wglMakeCurrent(prev_hdc, prev_hglrc);
	} else {
		wglMakeCurrent(NULL, NULL);
	}
#elif defined(__APPLE__)
	if (prev_cgl_ctx != NULL) {
		CGLSetCurrentContext(prev_cgl_ctx);
	}
#endif

	os_mutex_unlock(&c->mutex);

	return XRT_SUCCESS;
}

static xrt_result_t
gl_compositor_begin_session(struct xrt_compositor *xc, const struct xrt_begin_session_info *info)
{
	(void)xc;
	(void)info;
	return XRT_SUCCESS;
}

static xrt_result_t
gl_compositor_end_session(struct xrt_compositor *xc)
{
	(void)xc;
	return XRT_SUCCESS;
}

static xrt_result_t
gl_compositor_predict_frame(struct xrt_compositor *xc,
                             int64_t *out_frame_id,
                             int64_t *out_wake_time_ns,
                             int64_t *out_predicted_gpu_time_ns,
                             int64_t *out_predicted_display_time_ns,
                             int64_t *out_predicted_display_period_ns)
{
#ifdef XRT_OS_WINDOWS
	// Check if self-owned window was closed
	{
		struct comp_gl_compositor *c = gl_comp(xc);
		if (c->owns_window && c->own_window != NULL &&
		    !comp_d3d11_window_is_valid(c->own_window)) {
			// #999: graceful exit request, not a lost session.
			U_LOG_I("Window closed - requesting session exit");
			return XRT_ERROR_COMPOSITOR_WINDOW_CLOSED;
		}
	}
#endif

	int64_t now_ns = (int64_t)os_monotonic_get_ns();
	// Panel period, not a 60 Hz constant (a 165 Hz display is 6.06 ms).
	// Cached: EnumDisplaySettings is a syscall and this runs per frame.
	int64_t period_ns = (int64_t)(1000000000.0 / 60.0);
#ifdef XRT_OS_WINDOWS
	{
		static float cached_hz = 0.0f;
		if (cached_hz == 0.0f) {
			const float hz = comp_display_refresh_hz_win(gl_comp(xc)->hwnd);
			cached_hz = (hz > 0.0f) ? hz : 60.0f;
			U_LOG_W("Display refresh rate: %.2f Hz (frame period %.2f ms)", cached_hz,
			        1000.0 / cached_hz);
		}
		period_ns = (int64_t)(1000000000.0 / cached_hz);
	}
#endif

	static int64_t frame_id = 0;
	*out_frame_id = ++frame_id;
	*out_wake_time_ns = now_ns;
	*out_predicted_gpu_time_ns = now_ns + period_ns / 2;
	*out_predicted_display_time_ns = now_ns + period_ns;
	// #1257 partition: panel period on purpose — reporting D x period made
	// apps pace themselves on top of the throttle (double pacing); pacing
	// lives in the throttle alone.
	*out_predicted_display_period_ns = period_ns;

	return XRT_SUCCESS;
}

static xrt_result_t
gl_compositor_mark_frame(struct xrt_compositor *xc,
                          int64_t frame_id,
                          enum xrt_compositor_frame_point point,
                          int64_t when_ns)
{
	(void)xc;
	(void)frame_id;
	(void)point;
	(void)when_ns;
	return XRT_SUCCESS;
}

static xrt_result_t
gl_compositor_wait_frame(struct xrt_compositor *xc,
                          int64_t *out_frame_id,
                          int64_t *out_predicted_display_time,
                          int64_t *out_predicted_display_period)
{
	// #1257 partition: block until the app's next slot BEFORE anything else
	// — the repaint loop keeps weaving the other slots underneath this
	// sleep. Only here, never in predict_frame (wait_frame carries the
	// blocking semantic). No-op unless DXR_APP_FRAME_DIVISOR >= 2.
	{
		struct comp_gl_compositor *c = gl_comp(xc);
		int64_t period_ns = (int64_t)(1000000000.0 / 60.0);
#ifdef XRT_OS_WINDOWS
		const float hz = comp_display_refresh_hz_win(c->hwnd);
		if (hz > 1.0f) {
			period_ns = (int64_t)(1000000000.0 / hz);
		}
#endif
		// This in-process tier is UNSUPPORTED (see u_app_partition.h) —
		// the throttle refuses cleanly unless the bring-up override is set.
		u_app_partition_throttle(&c->repaint.partition, (uint64_t)period_ns,
		                         /*tier_supported=*/false);
	}

	int64_t wake, gpu_time;
	return gl_compositor_predict_frame(xc, out_frame_id, &wake, &gpu_time,
	                                    out_predicted_display_time,
	                                    out_predicted_display_period);
}

static xrt_result_t
gl_compositor_begin_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	(void)xc;
	(void)frame_id;
	return XRT_SUCCESS;
}

static xrt_result_t
gl_compositor_discard_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	(void)xc;
	(void)frame_id;
	return XRT_SUCCESS;
}


/*
 *
 * Layer functions
 *
 */

static xrt_result_t
gl_compositor_layer_begin(struct xrt_compositor *xc, const struct xrt_layer_frame_data *data)
{
	struct comp_gl_compositor *c = gl_comp(xc);

	// #868: the app's submission window opens here and closes in layer_commit.
	// The lock does not span it, so without this a repaint could win the lock
	// partway through the app filling layer_accum and replay a half-written frame.
	c->repaint.app_frame_in_progress = true;

	comp_layer_accum_begin(&c->layer_accum, data);
	return XRT_SUCCESS;
}

static xrt_result_t
gl_compositor_layer_projection(struct xrt_compositor *xc,
                                struct xrt_device *xdev,
                                struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                const struct xrt_layer_data *data)
{
	struct comp_gl_compositor *c = gl_comp(xc);
	comp_layer_accum_projection(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
gl_compositor_layer_projection_depth(struct xrt_compositor *xc,
                                      struct xrt_device *xdev,
                                      struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                      struct xrt_swapchain *d_xsc[XRT_MAX_VIEWS],
                                      const struct xrt_layer_data *data)
{
	struct comp_gl_compositor *c = gl_comp(xc);
	comp_layer_accum_projection_depth(&c->layer_accum, xsc, d_xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
gl_compositor_layer_quad(struct xrt_compositor *xc,
                          struct xrt_device *xdev,
                          struct xrt_swapchain *xsc,
                          const struct xrt_layer_data *data)
{
	struct comp_gl_compositor *c = gl_comp(xc);
	comp_layer_accum_quad(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
gl_compositor_layer_window_space(struct xrt_compositor *xc,
                                  struct xrt_device *xdev,
                                  struct xrt_swapchain *xsc,
                                  const struct xrt_layer_data *data)
{
	struct comp_gl_compositor *c = gl_comp(xc);
	comp_layer_accum_window_space(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

/*!
 * Local-2D layer (XR_DXR_local_3d_zone v3, #439 Phase 3) — accumulate only;
 * the GL consumer is a Windows follow-up leg
 * (docs/roadmap/unified-2d-3d-phase3-impl.md §7).
 */
static xrt_result_t
gl_compositor_layer_local_2d(struct xrt_compositor *xc,
                             struct xrt_device *xdev,
                             struct xrt_swapchain *xsc,
                             const struct xrt_layer_data *data)
{
	struct comp_gl_compositor *c = gl_comp(xc);
	comp_layer_accum_local_2d(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

/*!
 * 3D display zone layer (XR_DXR_display_zones, ADR-027) — multi-swapchain
 * accumulate like projection; consumed by the zones-frame branch of
 * layer_commit (zone rect scaled into the window-spanning atlas tile).
 */
static xrt_result_t
gl_compositor_layer_zone_3d(struct xrt_compositor *xc,
                            struct xrt_device *xdev,
                            struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                            const struct xrt_layer_data *data)
{
	struct comp_gl_compositor *c = gl_comp(xc);
	comp_layer_accum_zone_3d(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}


/*
 *
 * HUD overlay (shared u_hud system, cross-platform)
 *
 */

static void
gl_compositor_render_hud(struct comp_gl_compositor *c, float dt, uint32_t win_w, uint32_t win_h)
{
	if (c->hud == NULL || !u_hud_is_visible()) {
		return;
	}

	// Smooth frame time (every frame for accuracy)
	float dt_ms = dt * 1000.0f;
	if (dt_ms > 0.0f) {
		c->smoothed_frame_time_ms = c->smoothed_frame_time_ms * 0.9f + dt_ms * 0.1f;
	}
	float fps = (c->smoothed_frame_time_ms > 0.0f) ? (1000.0f / c->smoothed_frame_time_ms) : 0.0f;

	// Display dimensions from sys_info
	float disp_w_mm = 0, disp_h_mm = 0;
	float nom_x = 0, nom_y = 0, nom_z = 600.0f;
	if (c->sys_info_set) {
		disp_w_mm = c->sys_info.display_width_m * 1000.0f;
		disp_h_mm = c->sys_info.display_height_m * 1000.0f;
		nom_y = c->sys_info.nominal_viewer_y_m * 1000.0f;
		nom_z = c->sys_info.nominal_viewer_z_m * 1000.0f;
	}

	// Eye positions from display processor (fallback to nominal stereo)
	struct xrt_eye_positions eye_pos = {0};
	bool have_eyes = false;
	if (c->display_processor != NULL) {
		have_eyes = xrt_display_processor_gl_get_predicted_eye_positions(
		    c->display_processor, &eye_pos) && eye_pos.valid;
	}
	{
		static int hud_eye_log = 0;
		if (hud_eye_log < 5) {
			U_LOG_W("EYE-HUD[%d]: have=%d e0=(%.4f,%.4f,%.4f) e1=(%.4f,%.4f,%.4f)",
			        hud_eye_log, have_eyes,
			        eye_pos.eyes[0].x, eye_pos.eyes[0].y, eye_pos.eyes[0].z,
			        eye_pos.eyes[1].x, eye_pos.eyes[1].y, eye_pos.eyes[1].z);
			hud_eye_log++;
		}
	}
	if (!have_eyes) {
		eye_pos.count = 2;
		eye_pos.eyes[0] = xrt_eye_position{-0.032f, nom_y / 1000.0f, nom_z / 1000.0f};
		eye_pos.eyes[1] = xrt_eye_position{ 0.032f, nom_y / 1000.0f, nom_z / 1000.0f};
	}

	// Fill HUD data
	struct u_hud_data data = {0};
	data.device_name = (c->xdev != NULL) ? c->xdev->str : "Unknown";
	data.fps = fps;
	data.frame_time_ms = c->smoothed_frame_time_ms;
	data.mode_3d = c->hardware_display_3d;
	if (c->xdev != NULL && c->xdev->hmd != NULL) {
		uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
		if (idx < c->xdev->rendering_mode_count) {
			data.rendering_mode_name = c->xdev->rendering_modes[idx].mode_name;
		}
	}
	data.render_width = c->view_width;
	data.render_height = c->view_height;
	if (c->xdev != NULL && c->xdev->rendering_mode_count > 0) {
		u_tiling_compute_system_atlas(c->xdev->rendering_modes, c->xdev->rendering_mode_count,
		                              &data.swapchain_width, &data.swapchain_height);
	}
	data.window_width = win_w;
	data.window_height = win_h;
	data.display_width_mm = disp_w_mm;
	data.display_height_mm = disp_h_mm;
	data.nominal_x = nom_x;
	data.nominal_y = nom_y;
	data.nominal_z = nom_z;
	data.eye_count = eye_pos.count;
	for (uint32_t e = 0; e < eye_pos.count && e < 8; e++) {
		data.eyes[e].x = eye_pos.eyes[e].x * 1000.0f;
		data.eyes[e].y = eye_pos.eyes[e].y * 1000.0f;
		data.eyes[e].z = eye_pos.eyes[e].z * 1000.0f;
	}
	data.eye_tracking_active = eye_pos.is_tracking;

#ifdef XRT_BUILD_DRIVER_QWERTY
	if (c->xsysd != NULL) {
		struct xrt_pose qwerty_pose;
		if (qwerty_get_hmd_pose(c->xsysd->xdevs, c->xsysd->xdev_count, &qwerty_pose)) {
			data.vdisp_x = qwerty_pose.position.x;
			data.vdisp_y = qwerty_pose.position.y;
			data.vdisp_z = qwerty_pose.position.z;
			struct xrt_vec3 fwd_in = {0, 0, -1};
			struct xrt_vec3 fwd_out;
			math_quat_rotate_vec3(&qwerty_pose.orientation, &fwd_in, &fwd_out);
			data.forward_x = fwd_out.x;
			data.forward_y = fwd_out.y;
			data.forward_z = fwd_out.z;
		}

		struct qwerty_view_state ss;
		if (qwerty_get_view_state(c->xsysd->xdevs, c->xsysd->xdev_count, &ss)) {
			data.camera_mode = ss.camera_mode;
			data.ipd_factor = ss.ipd_factor;
			data.parallax_factor = ss.parallax_factor;
			data.inv_convergence_distance = ss.inv_convergence_distance;
			data.half_tan_vfov = ss.half_tan_vfov;
			data.m2v = ss.m2v;
			data.virtual_display_height = ss.virtual_display_height;
			data.perspective_factor = ss.perspective_factor;
			data.nominal_viewer_z = ss.nominal_viewer_z;
			data.screen_height_m = ss.screen_height_m;
		}
	}
#endif

	bool dirty = u_hud_update(c->hud, &data);

	// Lazy-create GL texture
	if (c->hud_texture == 0) {
		uint32_t hud_w = u_hud_get_width(c->hud);
		uint32_t hud_h = u_hud_get_height(c->hud);
		glGenTextures(1, &c->hud_texture);
		glBindTexture(GL_TEXTURE_2D, c->hud_texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, hud_w, hud_h, 0,
		             GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glBindTexture(GL_TEXTURE_2D, 0);
		dirty = true;
	}

	// Upload pixels if changed
	if (dirty) {
		uint32_t hud_w = u_hud_get_width(c->hud);
		uint32_t hud_h = u_hud_get_height(c->hud);
		glBindTexture(GL_TEXTURE_2D, c->hud_texture);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, hud_w, hud_h,
		                GL_RGBA, GL_UNSIGNED_BYTE, u_hud_get_pixels(c->hud));
		glBindTexture(GL_TEXTURE_2D, 0);
	}

	// Blit HUD to bottom-left of screen with alpha blending.
	// Scale down if HUD would exceed 50% of window width.
	uint32_t hud_w = u_hud_get_width(c->hud);
	uint32_t hud_h = u_hud_get_height(c->hud);
	uint32_t margin = 10;
	float scale = 1.0f;
	float max_frac = 0.5f;
	if (hud_w > (uint32_t)(win_w * max_frac)) {
		scale = (win_w * max_frac) / (float)hud_w;
	}
	uint32_t draw_w = (uint32_t)(hud_w * scale);
	uint32_t draw_h = (uint32_t)(hud_h * scale);

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glUseProgram(c->program_blit);
	glBindVertexArray(c->vao_empty);
	glViewport(margin, margin, draw_w, draw_h);

	GLint loc_rect = glGetUniformLocation(c->program_blit, "u_src_rect");
	glUniform4f(loc_rect, 0.0f, 0.0f, 1.0f, 1.0f);
	GLint loc_flip = glGetUniformLocation(c->program_blit, "u_flip_y");
	glUniform1f(loc_flip, 1.0f); // Flip Y: u_hud is top-down, GL is bottom-up

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, c->hud_texture);
	GLint loc_tex = glGetUniformLocation(c->program_blit, "u_texture");
	glUniform1i(loc_tex, 0);

	glDrawArrays(GL_TRIANGLES, 0, 3);

	glDisable(GL_BLEND);
}


/*
 *
 * Crop atlas to content dims and pass to display processor
 *
 */

// #491 part 3 — defined below near the composite; called here pre-weave.
static GLuint
gl_flatten_backdrop_2d(struct comp_gl_compositor *c, uint32_t dst_w, uint32_t dst_h, uint32_t *out_w,
                       uint32_t *out_h);

/*!
 * Crop the valid content region from the (potentially oversized) atlas texture
 * and pass it to the display processor. If the atlas exactly matches the
 * content dimensions, the atlas is passed directly (no copy).
 *
 * @param c           GL compositor
 * @param atlas_tex   Source atlas texture (may be oversized)
 * @param output_w    Output surface width (window, shared texture, IOSurface)
 * @param output_h    Output surface height
 */
static void
gl_crop_and_process_dp(struct comp_gl_compositor *c,
                       GLuint atlas_tex,
                       uint32_t output_w,
                       uint32_t output_h)
{
	// Snapshot the caller's draw FBO — the DP must weave into whatever target
	// the caller bound (window FBO 0, the shared-texture FBO, or the DComp
	// transit FBO on the transparent path), NOT an assumed FBO 0. The crop and
	// backdrop-flatten blits below bind their own FBOs, so we restore this one
	// before process_atlas.
	GLint caller_draw_fbo = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &caller_draw_fbo);

	// #491 part 3 — flatten the 2D-under layers PRE-weave and hand them to the DP
	// (it composites `backdrop over captured-desktop` under the 3D). 0 ⟹ no
	// under-layers (DP clears its backdrop). Covers the plain present, the
	// shared-texture/IOSurface paths, and the under-only fallback from the masked
	// composite. The flatten binds its own FBO, so snapshot/restore the caller's
	// draw FBO (the shared-texture path binds c->fbo before calling) so the DP
	// weaves into the intended target.
	{
		GLint prev_draw_fbo = 0;
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw_fbo);
		uint32_t bd_w = 0, bd_h = 0;
		GLuint bd_tex = gl_flatten_backdrop_2d(c, output_w, output_h, &bd_w, &bd_h);
		xrt_display_processor_gl_set_background_2d(c->display_processor, bd_tex, bd_w, bd_h);
		glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_draw_fbo);
	}

	// #542: the DP gets the frame's EFFECTIVE content layout — the grid the
	// blit passes actually painted (== the mode layout for matched
	// submissions) — not the mode layout.
	uint32_t eff_cols = c->eff_cols > 0 ? c->eff_cols : c->tile_columns;
	uint32_t eff_rows = c->eff_rows > 0 ? c->eff_rows : c->tile_rows;
	uint32_t eff_tile_w = c->eff_tile_w > 0 ? c->eff_tile_w : c->view_width;
	uint32_t eff_tile_h = c->eff_tile_h > 0 ? c->eff_tile_h : c->view_height;
	uint32_t content_w = eff_cols * eff_tile_w;
	uint32_t content_h = eff_rows * eff_tile_h;

	GLuint dp_tex = atlas_tex;

	if (content_w != c->atlas_tex_width || content_h != c->atlas_tex_height) {
		// Content is smaller than atlas — need to crop.
		// Lazily (re)create intermediate texture at content dims.
		if (c->dp_input_width != content_w || c->dp_input_height != content_h) {
			if (c->dp_input_texture != 0) {
				glDeleteTextures(1, &c->dp_input_texture);
			}
			glGenTextures(1, &c->dp_input_texture);
			glBindTexture(GL_TEXTURE_2D, c->dp_input_texture);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
			             content_w, content_h, 0,
			             GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glBindTexture(GL_TEXTURE_2D, 0);
			c->dp_input_width = content_w;
			c->dp_input_height = content_h;
			U_LOG_I("GL crop: created DP input texture %ux%u (atlas %ux%u)",
			        content_w, content_h, c->atlas_tex_width, c->atlas_tex_height);
		}

		// Blit content region from atlas into intermediate texture
		glBindFramebuffer(GL_READ_FRAMEBUFFER, c->fbo);
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                        GL_TEXTURE_2D, atlas_tex, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, c->dp_crop_fbo);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                        GL_TEXTURE_2D, c->dp_input_texture, 0);

		glBlitFramebuffer(
		    0, 0, content_w, content_h,   // src rect (content region)
		    0, 0, content_w, content_h,   // dst rect (same size)
		    GL_COLOR_BUFFER_BIT, GL_NEAREST);

		// Restore FBO state — DRAW back to the caller's target (not FBO 0), so
		// the weave below lands where the caller intended.
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)caller_draw_fbo);

		dp_tex = c->dp_input_texture;
	}

	// Late-weave (DXR_LATE_WEAVE=1): GL has no DXGI frame statistics or
	// present_wait, so pace on DWM's composition pass instead — DwmFlush
	// blocks until the next compose, putting the weave near the top of the
	// refresh interval. Coarser than the D3D/VK scanout pacing but the same
	// direction; GL is the least-used weave path (cube + legacy apps).
#ifdef XRT_OS_WINDOWS
	{
		static int late_weave = -1;
		if (late_weave < 0) {
			// Default ON (opt-out DXR_LATE_WEAVE=0), matching the
			// D3D11/D3D12/VK compositors.
			const char *e = getenv("DXR_LATE_WEAVE");
			late_weave = (e != NULL && e[0] == '0') ? 0 : 1;
		}
		if (late_weave == 1) {
			DwmFlush();
		}
	}
#endif

	// Pass (possibly cropped) texture to DP. Canvas params are always 0 — the
	// DP weaves the full client window (sub-rects are expressed as 3D zones now).
	glViewport(0, 0, output_w, output_h);
	xrt_display_processor_gl_process_atlas(
	    c->display_processor,
	    dp_tex,
	    eff_tile_w,
	    eff_tile_h,
	    eff_cols,
	    eff_rows,
	    GL_RGBA8,
	    output_w,
	    output_h,
	    0,
	    0,
	    0,
	    0);
}

#ifdef XRT_OS_WINDOWS
// GL→D3D shared-texture bridge. The runtime weaves into a WGL_NV_DX_interop2 GL
// render target, but the interop write-BACK into the D3D resource is unreliable
// on this stack (GL's view fills, the D3D surface stays empty). So mirror the
// no-interop DComp readback path: glReadPixels the woven (w×h) region of the
// currently-bound FBO and UpdateSubresource it into the app's shared texture.
// MUST be called while the interop FBO is still bound (GL owns the locked
// object). glReadPixels row 0 = GL bottom, uploaded to D3D row 0 (top) → the
// content is bottom-up in the shared texture; the app's present-blit V-flips.
static void
gl_shared_readback_upload(struct comp_gl_compositor *c, uint32_t w, uint32_t h)
{
	if (w == 0 || h == 0 || c->dx_shared_texture == NULL || c->dx_context == NULL) {
		return;
	}
	if (w > c->shared_width) w = c->shared_width;
	if (h > c->shared_height) h = c->shared_height;

	size_t need = (size_t)w * h * 4;
	if (c->shared_readback_cap < need || c->shared_readback_cpu == NULL) {
		free(c->shared_readback_cpu);
		c->shared_readback_cpu = (uint8_t *)malloc(need);
		c->shared_readback_cap = c->shared_readback_cpu != NULL ? need : 0;
	}
	if (c->shared_readback_cpu == NULL) {
		return;
	}

	glReadBuffer(GL_COLOR_ATTACHMENT0);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, c->shared_readback_cpu);

	// Upload into the top-left w×h sub-rect of the shared texture. The app reads
	// that same sub-rect (and V-flips, since the rows are GL bottom-up).
	D3D11_BOX box = {0, 0, 0, w, h, 1};
	c->dx_context->UpdateSubresource(c->dx_shared_texture, 0, &box, c->shared_readback_cpu,
	                                 (UINT)(w * 4), 0);
	// Flush so the app's separate present device sees the upload this frame.
	c->dx_context->Flush();
}
#endif


/*
 *
 * #439 Phase 3 — Local2D / masked 2D-over-3D composite (GL leg).
 *
 * Parity with the D3D11/D3D12/VK/Metal legs: final = M*weave + (1-M)*twod, a
 * hard-mask composite (the translucent redesign is #491). The GL Leia DP weaves
 * into a bound framebuffer, so when the consumer is active we redirect the
 * weave into a texture (weave_tex), flatten the Local2D layers into
 * local2d_scratch, rasterize/sample the mask, then lerp the three into the
 * window. GL framebuffers are bottom-left origin; the mask raster + the flatten
 * both flip Y from the app's top-left window pixels so they align with the
 * weave when the composite samples all three 1:1.
 *
 */

// (Re)allocate an RGBA8 color texture + its FBO at w×h (no-op if matching).
static bool
gl_ensure_color_tex_fbo(GLuint *tex, GLuint *fbo, uint32_t *cur_w, uint32_t *cur_h, uint32_t w, uint32_t h)
{
	if (*tex != 0 && *cur_w == w && *cur_h == h) {
		return true;
	}
	if (*tex != 0) {
		glDeleteTextures(1, tex);
		*tex = 0;
	}
	if (*fbo == 0) {
		glGenFramebuffers(1, fbo);
	}
	glGenTextures(1, tex);
	glBindTexture(GL_TEXTURE_2D, *tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	*cur_w = w;
	*cur_h = h;
	return true;
}

// #439 Phase 3 — (re)rasterize the IMPLICIT R8 zone mask from the frame's
// Local2D rects: M=1 (keep weave) everywhere, M=0 (show 2D) inside each rect
// (the inverse of an authored set_rects mask). GL scissor clears = the analog
// of D3D11 ClearView-rects / VK vkCmdClearAttachments. Rects are window pixels
// (top-left); flip Y for the bottom-left GL framebuffer. Dirty-checked. Returns
// the mask texture, or 0 on failure.
static GLuint
gl_update_implicit_mask(struct comp_gl_compositor *c,
                        const struct xrt_rect *rects,
                        uint32_t rect_count,
                        uint32_t w,
                        uint32_t h)
{
	if (w == 0 || h == 0 || rect_count == 0) {
		return 0;
	}

	bool dirty = c->implicit_mask_tex == 0 || c->implicit_mask_w != w || c->implicit_mask_h != h ||
	             c->implicit_rect_count != rect_count;
	for (uint32_t i = 0; !dirty && i < rect_count; i++) {
		if (memcmp(&c->implicit_rects[i], &rects[i], sizeof(rects[i])) != 0) {
			dirty = true;
		}
	}
	if (!dirty) {
		return c->implicit_mask_tex;
	}

	if (c->implicit_mask_tex == 0 || c->implicit_mask_w != w || c->implicit_mask_h != h) {
		if (c->implicit_mask_tex != 0) {
			glDeleteTextures(1, &c->implicit_mask_tex);
			c->implicit_mask_tex = 0;
		}
		if (c->implicit_mask_fbo == 0) {
			glGenFramebuffers(1, &c->implicit_mask_fbo);
		}
		glGenTextures(1, &c->implicit_mask_tex);
		glBindTexture(GL_TEXTURE_2D, c->implicit_mask_tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glBindTexture(GL_TEXTURE_2D, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, c->implicit_mask_fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->implicit_mask_tex, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		c->implicit_mask_w = w;
		c->implicit_mask_h = h;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, c->implicit_mask_fbo);
	glViewport(0, 0, w, h);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(1.0f, 0.0f, 0.0f, 0.0f); // M=1 everywhere (keep weave)
	glClear(GL_COLOR_BUFFER_BIT);

	glEnable(GL_SCISSOR_TEST);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // M=0 inside the layer rects (show 2D)
	for (uint32_t i = 0; i < rect_count; i++) {
		int32_t left = rects[i].offset.w;
		int32_t top = rects[i].offset.h;
		int32_t rw = rects[i].extent.w;
		int32_t rh = rects[i].extent.h;
		if (rw <= 0 || rh <= 0) {
			continue;
		}
		if (left < 0) {
			rw += left;
			left = 0;
		}
		if (top < 0) {
			rh += top;
			top = 0;
		}
		if (left + rw > (int32_t)w) {
			rw = (int32_t)w - left;
		}
		if (top + rh > (int32_t)h) {
			rh = (int32_t)h - top;
		}
		if (rw <= 0 || rh <= 0) {
			continue;
		}
		// Flip Y: window top-left → GL bottom-left framebuffer.
		int32_t gl_y = (int32_t)h - (top + rh);
		glScissor(left, gl_y, rw, rh);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	memcpy(c->implicit_rects, rects, sizeof(rects[0]) * rect_count);
	c->implicit_rect_count = rect_count;
	U_LOG_W("implicit zone mask: %ux%u, %u Local2D rect(s)", w, h, rect_count);
	return c->implicit_mask_tex;
}

// XR_DXR_display_zones (ADR-027) — (re)rasterize the AUTO wish: union of the
// frame's zone rects, BINARY (#800/#801 — the wish is HARDWARE-only and
// hard-edged by default; the old implicit 16px ring feather leaked cosmetic
// fractional M into the published wish and vignetted the composite at window
// edges). M=1 inside every zone rect, 0 outside. The same texture is the
// MODE_ZONES composite's weave gate and the published wish when no explicit
// wish is staged; cosmetic feather (XrDisplayZoneFeatherDXR, #803) rasters
// into its own texture and never enters this one.
// Reuses the implicit-mask R8 texture (the implicit rule is inert in zones
// frames) and re-rasters every zones frame, VK-style — a handful of scissored
// clears — while invalidating the implicit rect cache so a later legacy frame
// re-rasters. Rects are window px (top-left); flip Y for the bottom-left GL
// framebuffer. Returns the mask texture, or 0 on failure.
static GLuint
gl_update_zone_wish_mask(struct comp_gl_compositor *c,
                         const struct xrt_rect *rects,
                         uint32_t rect_count,
                         uint32_t w,
                         uint32_t h)
{
	if (w == 0 || h == 0 || rect_count == 0) {
		return 0;
	}

	// (Re)allocate — same R8 texture + FBO block as gl_update_implicit_mask.
	if (c->implicit_mask_tex == 0 || c->implicit_mask_w != w || c->implicit_mask_h != h) {
		if (c->implicit_mask_tex != 0) {
			glDeleteTextures(1, &c->implicit_mask_tex);
			c->implicit_mask_tex = 0;
		}
		if (c->implicit_mask_fbo == 0) {
			glGenFramebuffers(1, &c->implicit_mask_fbo);
		}
		glGenTextures(1, &c->implicit_mask_tex);
		glBindTexture(GL_TEXTURE_2D, c->implicit_mask_tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glBindTexture(GL_TEXTURE_2D, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, c->implicit_mask_fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->implicit_mask_tex, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		c->implicit_mask_w = w;
		c->implicit_mask_h = h;
	}

	// The wish raster replaces whatever the implicit rule cached.
	c->implicit_rect_count = 0;

	glBindFramebuffer(GL_FRAMEBUFFER, c->implicit_mask_fbo);
	glViewport(0, 0, w, h);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // no 3D wish anywhere
	glClear(GL_COLOR_BUFFER_BIT);

	glEnable(GL_SCISSOR_TEST);
	glClearColor(1.0f, 0.0f, 0.0f, 0.0f); // binary: M=1 across each zone rect
	for (uint32_t i = 0; i < rect_count; i++) {
		int32_t left = rects[i].offset.w;
		int32_t top = rects[i].offset.h;
		int32_t right = rects[i].offset.w + rects[i].extent.w;
		int32_t bottom = rects[i].offset.h + rects[i].extent.h;
		if (left < 0) {
			left = 0;
		}
		if (top < 0) {
			top = 0;
		}
		if (right > (int32_t)w) {
			right = (int32_t)w;
		}
		if (bottom > (int32_t)h) {
			bottom = (int32_t)h;
		}
		if (right <= left || bottom <= top) {
			continue;
		}
		// Flip Y: window top-left → GL bottom-left framebuffer.
		int32_t gl_y = (int32_t)h - bottom;
		glScissor(left, gl_y, right - left, bottom - top);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	static bool wish_logged = false;
	if (!wish_logged) {
		wish_logged = true;
		U_LOG_W("GL zone wish mask (auto): %ux%u, %u zone rect(s), binary", w, h, rect_count);
	}
	return c->implicit_mask_tex;
}

// XR_DXR_display_zones (#800/#803) — (re)rasterize the zones COMPOSITE mask
// with PER-ZONE opt-in feather (XrDisplayZoneFeatherDXR) into its own R8
// texture. Clear M=0, then each zone draws hard (one scissored clear at 1.0,
// feather_px[i] <= 0 — the default) or with its own inward 0->1 ramp over
// feather_px[i] window pixels (the rings idiom: ascending value WITH
// ascending inset, 2px steps, ramp-width cap 64px then wider steps; small
// zones clamp the inset so the center still reaches 1). Per zone so radii
// can differ. Only called when a frame's zones request feather — all-hard
// frames sample the binary wish raster instead, and the published wish stays
// binary regardless (cosmetics never enter the wish). Re-rasters every
// feathered zones frame (VK-style). No zone_publish_seq interaction:
// composite-only, never published. Rects are window px (top-left); flip Y
// for the bottom-left GL framebuffer. Returns the feather texture, or 0 on
// failure (caller falls back to the binary mask — hard edges, never a lost
// frame).
static GLuint
gl_update_zone_feather_mask(struct comp_gl_compositor *c,
                            const struct xrt_rect *rects,
                            const float *feather_px,
                            uint32_t rect_count,
                            uint32_t w,
                            uint32_t h)
{
	if (w == 0 || h == 0 || rect_count == 0) {
		return 0;
	}

	// (Re)allocate — same R8 texture + FBO block as the wish raster.
	if (c->feather_mask_tex == 0 || c->feather_mask_w != w || c->feather_mask_h != h) {
		if (c->feather_mask_tex != 0) {
			glDeleteTextures(1, &c->feather_mask_tex);
			c->feather_mask_tex = 0;
		}
		if (c->feather_mask_fbo == 0) {
			glGenFramebuffers(1, &c->feather_mask_fbo);
		}
		glGenTextures(1, &c->feather_mask_tex);
		glBindTexture(GL_TEXTURE_2D, c->feather_mask_tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glBindTexture(GL_TEXTURE_2D, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, c->feather_mask_fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->feather_mask_tex, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		c->feather_mask_w = w;
		c->feather_mask_h = h;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, c->feather_mask_fbo);
	glViewport(0, 0, w, h);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // M=0 outside every zone
	glClear(GL_COLOR_BUFFER_BIT);

	// Per-zone: a hard zone is one full-rect clear at 1.0 (inset 0); a
	// feathered zone ramps 0->1 over its OWN radius via the rings idiom —
	// ascending value WITH ascending inset, later (deeper, higher-value)
	// clears overwriting the inner part of earlier ones so the edge keeps
	// the low values and the core reaches 1.
	glEnable(GL_SCISSOR_TEST);
	for (uint32_t i = 0; i < rect_count; i++) {
		const float radius = feather_px[i];
		const bool feathered = radius > 0.0f;
		int32_t steps = 1;
		int32_t step_px = 0;
		if (feathered) {
			step_px = 2;
			steps = (int32_t)(radius / (float)step_px + 0.5f);
			if (steps < 1) {
				steps = 1;
			}
			if (steps > 32) { // beyond a 64px ramp, widen the step instead
				step_px = (int32_t)(radius / 32.0f + 0.5f);
				steps = 32;
			}
		}
		for (int32_t s = 1; s <= steps; s++) {
			const float v = (float)s / (float)steps; // 1.0 for the hard single step
			int32_t min_ext = rects[i].extent.w < rects[i].extent.h ? rects[i].extent.w
			                                                        : rects[i].extent.h;
			int32_t max_inset = (min_ext - 1) / 2;
			if (max_inset < 0) {
				max_inset = 0;
			}
			int32_t inset = feathered ? s * step_px : 0;
			if (inset > max_inset) {
				inset = max_inset;
			}
			int32_t left = rects[i].offset.w + inset;
			int32_t top = rects[i].offset.h + inset;
			int32_t right = rects[i].offset.w + rects[i].extent.w - inset;
			int32_t bottom = rects[i].offset.h + rects[i].extent.h - inset;
			if (left < 0) {
				left = 0;
			}
			if (top < 0) {
				top = 0;
			}
			if (right > (int32_t)w) {
				right = (int32_t)w;
			}
			if (bottom > (int32_t)h) {
				bottom = (int32_t)h;
			}
			if (right <= left || bottom <= top) {
				continue;
			}
			glClearColor(v, 0.0f, 0.0f, 0.0f);
			// Flip Y: window top-left → GL bottom-left framebuffer.
			int32_t gl_y = (int32_t)h - bottom;
			glScissor(left, gl_y, right - left, bottom - top);
			glClear(GL_COLOR_BUFFER_BIT);
		}
	}
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	static bool feather_logged = false;
	if (!feather_logged) {
		feather_logged = true;
		U_LOG_W("GL zone feather mask: %ux%u, %u zone rect(s) (composite-only, wish stays binary)", w, h,
		        rect_count);
	}
	return c->feather_mask_tex;
}

// #439 Phase 3 — draw one Local2D layer into the currently-bound flatten FBO
// (premultiplied or straight "over"). Assumes program_window_space is bound and
// the loc_* uniforms fetched by the caller. Dest rect clips to the window region;
// Y is flipped for the bottom-left GL framebuffer.
static void
gl_flatten_one_local2d_layer(struct comp_gl_compositor *c, struct comp_layer *layer, uint32_t region_w,
                             uint32_t region_h, GLint loc_rect, GLint loc_tex, GLint loc_src, bool skip_decode)
{
	struct xrt_swapchain *sc = layer->sc_array[0];
	if (sc == NULL) {
		return;
	}
	struct comp_gl_swapchain *gsc = gl_swapchain(sc);
	uint32_t img_idx = layer->data.local_2d.sub.image_index;
	if (img_idx >= gsc->image_count) {
		return;
	}
	GLuint src_tex = gsc->textures[img_idx];

	const struct xrt_rect *dr = &layer->data.local_2d.rect;
	float dx = (float)dr->offset.w;
	float dy = (float)dr->offset.h;
	float dw = (float)dr->extent.w;
	float dh = (float)dr->extent.h;
	if (dw <= 0.0f || dh <= 0.0f) {
		return;
	}

	// NDC rect for the positioned-quad VS. Window pixels are top-left origin;
	// flip Y for the bottom-left GL framebuffer: the panel's GL bottom edge is
	// region_h - (dy + dh).
	float nx = dx / (float)region_w * 2.0f - 1.0f;
	float ny = ((float)region_h - (dy + dh)) / (float)region_h * 2.0f - 1.0f;
	float nw = dw / (float)region_w * 2.0f;
	float nh = dh / (float)region_h * 2.0f;

	// App sub-rect within the swapchain image (normalized). Default full.
	struct xrt_normalized_rect nr = layer->data.local_2d.sub.norm_rect;
	if (nr.w <= 0.0f || nr.h <= 0.0f) {
		nr.x = 0.0f;
		nr.y = 0.0f;
		nr.w = 1.0f;
		nr.h = 1.0f;
	}
	// VS_WINDOW_SPACE maps the NDC top to v_uv.y=0; combined with the GL
	// bottom-left flip above, sample the source bottom-up so the panel is
	// upright. flip_y inverts that.
	float src_x = nr.x;
	float src_w = nr.w;
	float src_y, src_h;
	if (layer->data.flip_y) {
		src_y = nr.y;
		src_h = nr.h;
	} else {
		src_y = nr.y + nr.h;
		src_h = -nr.h;
	}

	bool unpremult = (layer->data.flags & XRT_LAYER_COMPOSITION_UNPREMULTIPLIED_ALPHA_BIT) != 0;
	glEnable(GL_BLEND);
	if (unpremult) {
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	} else {
		glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	}

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, src_tex);
	if (skip_decode) {
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SRGB_DECODE_EXT, GL_SKIP_DECODE_EXT);
	}
	glUniform1i(loc_tex, 0);
	glUniform4f(loc_rect, nx, ny, nw, nh);
	glUniform4f(loc_src, src_x, src_y, src_w, src_h);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// #439 Phase 3 — flatten this frame's OVER Local2D layers into local2d_scratch
// (the `twod` source). Clears transparent, draws each layer in list order (later
// = on top). #491 part 3: under-layers (before the projection in list order,
// proj_idx) are the DP backdrop and are skipped here.
static void
gl_flatten_local_2d_layers(struct comp_gl_compositor *c, uint32_t region_w, uint32_t region_h, int32_t proj_idx)
{
	glBindFramebuffer(GL_FRAMEBUFFER, c->local2d_scratch_fbo);
	glViewport(0, 0, region_w, region_h);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // transparent → desktop where uncovered (final.a=0)
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(c->program_window_space);
	glBindVertexArray(c->vao_empty);
	GLint loc_rect = glGetUniformLocation(c->program_window_space, "u_rect");
	GLint loc_tex = glGetUniformLocation(c->program_window_space, "u_texture");
	GLint loc_src = glGetUniformLocation(c->program_window_space, "u_src_rect");
	const bool skip_decode = gl_has_srgb_decode_ext();

	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		struct comp_layer *layer = &c->layer_accum.layers[i];
		if (layer->data.type != XRT_LAYER_LOCAL_2D) {
			continue;
		}
		// #491 part 3 — under-layers are the DP backdrop (handled pre-weave).
		if (proj_idx >= 0 && (int32_t)i < proj_idx) {
			continue;
		}
		gl_flatten_one_local2d_layer(c, layer, region_w, region_h, loc_rect, loc_tex, loc_src, skip_decode);
	}

	glDisable(GL_BLEND);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// #491 part 3 — flatten this frame's 2D-UNDER Local2D layers (before the
// projection in list order) into backdrop_scratch PRE-weave and return its GL
// texture name (+ region dims) so the caller hands it to the DP via
// set_background_2d (the DP composites `backdrop over captured-desktop` under the
// 3D). Returns 0 (out dims 0) when there are no under-layers.
//
// NOTE: the GL Leia DP is chroma-key-only (no WGC compose-under-bg path, see
// project_leia_transparency_model) → the backdrop has nowhere to composite, so
// the GL leg's set_background_2d is a VISUAL NO-OP today; the wiring lands so it
// works once GL transparency/compose lands (separate deferred follow-up).
static GLuint
gl_flatten_backdrop_2d(struct comp_gl_compositor *c, uint32_t dst_w, uint32_t dst_h, uint32_t *out_w,
                       uint32_t *out_h)
{
	*out_w = 0;
	*out_h = 0;
	if (!c->local_2d_last_frame) {
		return 0;
	}

	// Under = Local2D layers BEFORE the projection. No projection ⟹ no backdrop.
	int32_t proj_idx = -1;
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		enum xrt_layer_type t = c->layer_accum.layers[i].data.type;
		if (t == XRT_LAYER_PROJECTION || t == XRT_LAYER_PROJECTION_DEPTH) {
			proj_idx = (int32_t)i;
			break;
		}
	}
	if (proj_idx < 0) {
		return 0;
	}
	bool have_under = false;
	for (int32_t i = 0; i < proj_idx; i++) {
		if (c->layer_accum.layers[i].data.type == XRT_LAYER_LOCAL_2D) {
			have_under = true;
			break;
		}
	}
	if (!have_under) {
		return 0;
	}

	uint32_t region_w = dst_w;
	uint32_t region_h = dst_h;
	if (region_w == 0 || region_h == 0) {
		return 0;
	}
	if (!gl_ensure_color_tex_fbo(&c->backdrop_scratch_tex, &c->backdrop_scratch_fbo, &c->backdrop_scratch_w,
	                             &c->backdrop_scratch_h, region_w, region_h)) {
		return 0;
	}

	glBindFramebuffer(GL_FRAMEBUFFER, c->backdrop_scratch_fbo);
	glViewport(0, 0, region_w, region_h);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f); // transparent where no under-layer covers
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(c->program_window_space);
	glBindVertexArray(c->vao_empty);
	GLint loc_rect = glGetUniformLocation(c->program_window_space, "u_rect");
	GLint loc_tex = glGetUniformLocation(c->program_window_space, "u_texture");
	GLint loc_src = glGetUniformLocation(c->program_window_space, "u_src_rect");
	const bool skip_decode = gl_has_srgb_decode_ext();

	for (int32_t i = 0; i < proj_idx; i++) {
		struct comp_layer *layer = &c->layer_accum.layers[i];
		if (layer->data.type != XRT_LAYER_LOCAL_2D) {
			continue;
		}
		gl_flatten_one_local2d_layer(c, layer, region_w, region_h, loc_rect, loc_tex, loc_src, skip_decode);
	}

	glDisable(GL_BLEND);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	static bool logged = false;
	if (!logged) {
		logged = true;
		U_LOG_W("GL #491 part3: flattened 2D-under backdrop %ux%u (handed to DP set_background_2d; "
		        "GL DP is chroma-key-only → visual no-op until GL compose lands)",
		        region_w, region_h);
	}

	*out_w = region_w;
	*out_h = region_h;
	return c->backdrop_scratch_tex;
}

// Weave the (cropped) atlas into an arbitrary target FBO — same crop logic as
// gl_crop_and_process_dp, but lets the caller redirect the DP output into a
// texture FBO (the post-weave composite needs the weave in a sampleable tex).
static void
gl_dp_weave_to_fbo(struct comp_gl_compositor *c, GLuint atlas_tex, GLuint target_fbo, uint32_t output_w,
                   uint32_t output_h)
{
	// #542: same effective-layout source as gl_crop_and_process_dp.
	uint32_t eff_cols = c->eff_cols > 0 ? c->eff_cols : c->tile_columns;
	uint32_t eff_rows = c->eff_rows > 0 ? c->eff_rows : c->tile_rows;
	uint32_t eff_tile_w = c->eff_tile_w > 0 ? c->eff_tile_w : c->view_width;
	uint32_t eff_tile_h = c->eff_tile_h > 0 ? c->eff_tile_h : c->view_height;
	uint32_t content_w = eff_cols * eff_tile_w;
	uint32_t content_h = eff_rows * eff_tile_h;
	GLuint dp_tex = atlas_tex;

	if (content_w != c->atlas_tex_width || content_h != c->atlas_tex_height) {
		if (c->dp_input_width != content_w || c->dp_input_height != content_h) {
			if (c->dp_input_texture != 0) {
				glDeleteTextures(1, &c->dp_input_texture);
			}
			glGenTextures(1, &c->dp_input_texture);
			glBindTexture(GL_TEXTURE_2D, c->dp_input_texture);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, content_w, content_h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
			             NULL);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glBindTexture(GL_TEXTURE_2D, 0);
			c->dp_input_width = content_w;
			c->dp_input_height = content_h;
		}
		glBindFramebuffer(GL_READ_FRAMEBUFFER, c->fbo);
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, atlas_tex, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, c->dp_crop_fbo);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->dp_input_texture,
		                       0);
		glBlitFramebuffer(0, 0, content_w, content_h, 0, 0, content_w, content_h, GL_COLOR_BUFFER_BIT,
		                  GL_NEAREST);

		/*
		 * #885 diag (DXR_WEAVE_REPAINT_DIAG=1): the plug-in-side probes show
		 * the DP receives an EMPTY dp_input on the repaint thread from its 3rd
		 * call onward, while the app thread's stays correct. Probe THIS blit:
		 * per-thread few-shot — READ-fbo status, blit GL error, a texel of the
		 * SOURCE atlas and of the DEST dp_input at the same spot the plug-in
		 * reads black. Names whether the source is already empty on this
		 * thread or the blit drops the pixels.
		 */
		{
			static int diag = -1;
			if (diag < 0) {
				const char *e = getenv("DXR_WEAVE_REPAINT_DIAG");
				diag = (e != NULL && e[0] == '1') ? 1 : 0;
			}
			if (diag == 1) {
#ifdef XRT_OS_WINDOWS
				static DWORD s_tid[2] = {0, 0};
				static ULONGLONG s_last_ms[2] = {0, 0};
				DWORD tid = GetCurrentThreadId();
				int slot = -1;
				for (int i = 0; i < 2; i++) {
					if (s_tid[i] == tid) { slot = i; break; }
					if (s_tid[i] == 0) { s_tid[i] = tid; slot = i; break; }
				}
				ULONGLONG now_ms = GetTickCount64();
				if (slot >= 0 && now_ms - s_last_ms[slot] >= 2000) {
					s_last_ms[slot] = now_ms;
					GLenum blit_err = glGetError();
					// w/4 keeps the texel inside zone A content across modes — the
				// tile CENTRE sits on zone A right edge in 4-view tiles.
				GLint tx = (GLint)(eff_tile_w / 4), ty = (GLint)(eff_tile_h / 2);
					uint8_t src_px[4] = {0}, dst_px[4] = {0};
					// READ fbo still has the atlas attached.
					GLenum rd_status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
					glReadPixels(tx, ty, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, src_px);
					glBindFramebuffer(GL_READ_FRAMEBUFFER, c->dp_crop_fbo);
					glReadPixels(tx, ty, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, dst_px);
					glBindFramebuffer(GL_READ_FRAMEBUFFER, c->fbo);
					U_LOG_W("#885 crop diag tid=%lu atlas_tex=%u dp_input=%u rd_status=0x%x "
					        "blit_err=0x%x src(%d,%d)=(%u,%u,%u,%u) dst=(%u,%u,%u,%u)",
					        (unsigned long)tid, atlas_tex, c->dp_input_texture,
					        (unsigned)rd_status, (unsigned)blit_err, tx, ty, src_px[0],
					        src_px[1], src_px[2], src_px[3], dst_px[0], dst_px[1], dst_px[2],
					        dst_px[3]);
				}
#endif
			}
		}

		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		dp_tex = c->dp_input_texture;
	}

	// Canvas params are always 0 — the DP weaves the full client window.
	glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
	glViewport(0, 0, output_w, output_h);
	xrt_display_processor_gl_process_atlas(c->display_processor, dp_tex, eff_tile_w, eff_tile_h,
	                                       eff_cols, eff_rows, GL_RGBA8, output_w, output_h,
	                                       0, 0, 0, 0);
}

// #439 Phase 3 — the post-weave masked composite. Runs INSTEAD of the plain
// gl_crop_and_process_dp present when an explicit submitted mask or Local2D
// layers are present. Flattens the 2D, resolves the mask (explicit tex or
// implicit raster), redirects the DP weave into weave_tex, then lerps
// M*weave + (1-M)*twod into target_fbo (the window). Returns false → caller
// falls through to the plain present.
static bool
gl_composite_local_2d(struct comp_gl_compositor *c, GLuint atlas_tex, GLuint target_fbo, uint32_t output_w,
                      uint32_t output_h, bool reuse_twod)
{
	// XR_DXR_display_zones: a zones frame ALWAYS runs the composite (the
	// MODE_ZONES pass gates the weave by the binary zone raster — pixels
	// outside every zone go to the 2D flatten / transparent even with zero
	// Local2D layers); the sticky mask + implicit-mask rules are inert.
	struct comp_gl_zone_mask *mask = c->active_zone_mask;
	const bool zones_frame = c->zones_frame;
	const bool have_explicit = !zones_frame && (mask != NULL && mask->submitted && mask->tex != 0);
	const bool have_local_2d = c->local_2d_last_frame;
	if ((!zones_frame && !have_explicit && !have_local_2d) || c->program_masked_composite == 0 ||
	    output_w == 0 || output_h == 0) {
		return false;
	}

	// #491 part 3 — split Local2D by list order vs the projection: layers BEFORE
	// the projection are the 2D-under backdrop (handed to the DP pre-weave) and
	// are excluded from the overlay mask + flatten here.
	int32_t proj_idx = -1;
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		enum xrt_layer_type t = c->layer_accum.layers[i].data.type;
		if (t == XRT_LAYER_PROJECTION || t == XRT_LAYER_PROJECTION_DEPTH) {
			proj_idx = (int32_t)i;
			break;
		}
	}

	// weave_tex + local2d_scratch are both window-sized; (re)allocate them
	// together under one dims guard (composite_scratch_w/h is the canonical
	// pair). The inner ensure's throwaway dims are fine — it only runs when the
	// guard already decided a (re)alloc is needed.
	if (c->weave_tex == 0 || c->local2d_scratch_tex == 0 || c->composite_scratch_w != output_w ||
	    c->composite_scratch_h != output_h) {
		uint32_t tmp_w = 0, tmp_h = 0;
		gl_ensure_color_tex_fbo(&c->weave_tex, &c->weave_fbo, &tmp_w, &tmp_h, output_w, output_h);
		tmp_w = 0;
		tmp_h = 0;
		gl_ensure_color_tex_fbo(&c->local2d_scratch_tex, &c->local2d_scratch_fbo, &tmp_w, &tmp_h, output_w,
		                        output_h);
		c->composite_scratch_w = output_w;
		c->composite_scratch_h = output_h;
	}

	// Resolve the mask texture. Zones frame (XR_DXR_display_zones, #801):
	// ALWAYS the BINARY zone raster — per ADR-027 the wish is HARDWARE-only
	// and composition follows zone geometry + alpha, so an explicit frame
	// wish never gates blending; it routes to the DP publish only (the
	// live same-context authoring texture, referenced-at-frame-end =
	// consume current authored state, no submit required — mirroring
	// zone_mask_submit's no-staging contract). #803: when any zone
	// requests feather, the composite samples a separately-rastered
	// per-zone feather mask instead; the published wish stays binary
	// regardless (cosmetics never enter the wish). Legacy: explicit
	// submitted mask wins; else implicit.
	GLuint mask_tex = 0;
	if (zones_frame) {
		struct xrt_rect zone_rects[XRT_MAX_LAYERS];
		float zone_feathers[XRT_MAX_LAYERS];
		bool any_feather = false;
		uint32_t zone_rect_count = 0;
		for (uint32_t i = 0; i < c->layer_accum.layer_count && zone_rect_count < XRT_MAX_LAYERS; i++) {
			if (c->layer_accum.layers[i].data.type != XRT_LAYER_ZONE_3D) {
				continue;
			}
			zone_feathers[zone_rect_count] = c->layer_accum.layers[i].data.zone_3d.feather_px;
			if (zone_feathers[zone_rect_count] > 0.0f) {
				any_feather = true;
			}
			zone_rects[zone_rect_count++] = c->layer_accum.layers[i].data.zone_3d.rect;
		}
		mask_tex = gl_update_zone_wish_mask(c, zone_rects, zone_rect_count, output_w, output_h);

		if (c->frame_wish != NULL && c->frame_wish->tex != 0) {
			// P4 publish source + seq: the explicit wish (the live
			// authoring texture — same-context, matching the GL
			// no-staging contract) — PUBLISH-ONLY (#801), never the
			// composite mask. Bump the generation on a source change
			// (pointer flip; GL masks carry no author generation, so a
			// same-pointer re-author keeps its seq).
			c->zone_publish_tex = c->frame_wish->tex;
			c->zone_publish_w = c->frame_wish->w;
			c->zone_publish_h = c->frame_wish->h;
			if (c->zone_frame_wish_last != c->frame_wish) {
				c->zone_frame_wish_last = c->frame_wish;
				c->zone_publish_seq++;
			}
		} else if (mask_tex != 0) {
			// P4 publish source + seq: the auto raster — bump the
			// generation only when the rect set / dims actually
			// changed (or the source flipped explicit -> auto).
			bool wish_dirty = c->zone_frame_wish_last != NULL ||
			                  c->zone_wish_rect_count != zone_rect_count ||
			                  c->zone_publish_w != output_w || c->zone_publish_h != output_h;
			for (uint32_t i = 0; !wish_dirty && i < zone_rect_count; i++) {
				if (memcmp(&c->zone_wish_rects[i], &zone_rects[i], sizeof(zone_rects[i])) != 0) {
					wish_dirty = true;
				}
			}
			if (wish_dirty) {
				c->zone_frame_wish_last = NULL;
				memcpy(c->zone_wish_rects, zone_rects, sizeof(zone_rects[0]) * zone_rect_count);
				c->zone_wish_rect_count = zone_rect_count;
				c->zone_publish_seq++;
			}
			c->zone_publish_tex = mask_tex;
			c->zone_publish_w = output_w;
			c->zone_publish_h = output_h;
		}

		// #803 — feather is a composite-only cosmetic; raster failure
		// falls back to the binary mask (hard edges, never a lost frame).
		if (any_feather) {
			GLuint ftex = gl_update_zone_feather_mask(c, zone_rects, zone_feathers, zone_rect_count,
			                                          output_w, output_h);
			if (ftex != 0) {
				mask_tex = ftex;
			}
		}
	} else if (have_explicit) {
		mask_tex = mask->tex;
	} else {
		struct xrt_rect rects[XRT_MAX_LAYERS];
		uint32_t rect_count = 0;
		for (uint32_t i = 0; i < c->layer_accum.layer_count && rect_count < XRT_MAX_LAYERS; i++) {
			if (c->layer_accum.layers[i].data.type != XRT_LAYER_LOCAL_2D) {
				continue;
			}
			if (proj_idx >= 0 && (int32_t)i < proj_idx) {
				continue; // under-layer (backdrop) — not part of the overlay mask
			}
			rects[rect_count++] = c->layer_accum.layers[i].data.local_2d.rect;
		}
		mask_tex = gl_update_implicit_mask(c, rects, rect_count, output_w, output_h);
	}
	if (mask_tex == 0) {
		return false;
	}

	/*
	 * #885 diag (DXR_WEAVE_REPAINT_DIAG=1): name the composite's INPUTS on a
	 * repaint vs an app frame. The composite reads layer_accum + per-frame
	 * flags live, and a repaint replays between (or racing) app submissions —
	 * if any input below differs from the adjacent app frame, that input is
	 * the bug. Logs a few of each kind, then goes quiet.
	 */
	{
		static int diag = -1;
		if (diag < 0) {
			const char *e = getenv("DXR_WEAVE_REPAINT_DIAG");
			diag = (e != NULL && e[0] == '1') ? 1 : 0;
		}
		if (diag == 1) {
			static int logged_app = 0, logged_rp = 0;
			int *counter = reuse_twod ? &logged_rp : &logged_app;
			if (*counter < 6) {
				(*counter)++;
				char rects_str[256] = {0};
				size_t off = 0;
				uint32_t zc = 0;
				for (uint32_t i = 0; i < c->layer_accum.layer_count && off < sizeof(rects_str) - 40;
				     i++) {
					const struct xrt_layer_data *ld = &c->layer_accum.layers[i].data;
					if (ld->type == XRT_LAYER_ZONE_3D) {
						zc++;
						off += (size_t)snprintf(rects_str + off, sizeof(rects_str) - off,
						                        " z[%d,%d %dx%d]", ld->zone_3d.rect.offset.w,
						                        ld->zone_3d.rect.offset.h, ld->zone_3d.rect.extent.w,
						                        ld->zone_3d.rect.extent.h);
					} else if (ld->type == XRT_LAYER_LOCAL_2D) {
						off += (size_t)snprintf(rects_str + off, sizeof(rects_str) - off,
						                        " l[%d,%d %dx%d]", ld->local_2d.rect.offset.w,
						                        ld->local_2d.rect.offset.h, ld->local_2d.rect.extent.w,
						                        ld->local_2d.rect.extent.h);
					}
				}
				U_LOG_W("#885 diag %s: layers=%u zone3d=%u zones_frame=%d explicit=%d local2d=%d "
				        "mask_tex=%u in_frame=%d%s",
				        reuse_twod ? "REPAINT" : "app", c->layer_accum.layer_count, zc,
				        (int)zones_frame, (int)have_explicit, (int)have_local_2d, mask_tex,
				        (int)c->repaint.app_frame_in_progress, rects_str);
			}
		}
	}

	// Flatten the Local2D layers (the twod source). With no Local2D layers (a
	// pure explicit-mask frame) the scratch stays transparent → the masked
	// region shows the desktop, matching the VK leg.
	// Zones frame: flatten ALL Local2D layers (no under/over split — 2D-under
	// is reserved in v1); with zero Local2D layers the clear-only scratch
	// means MODE_ZONES writes M·weave over transparent — pixels outside
	// every zone present alpha 0.
	/*
	 * #875 DEPOSIT half — the only part of this composite that reads app-owned
	 * memory. Both flattens below sample the app's own Local2D swapchain
	 * textures, so they run on the APP FRAME ONLY and land in
	 * compositor-owned scratch (local2d_scratch_tex / the backdrop texture).
	 *
	 * A repaint (reuse_twod) skips them and lerps from what the last app frame
	 * deposited. Re-running them would sample textures the app has since
	 * reacquired and redrawn — the 2D-bubble flicker found on D3D11 and D3D12.
	 */
	/*
	 * #868 probe: DXR_WEAVE_REPAINT_REFLATTEN=1 makes a repaint re-run the
	 * deposit half. That VIOLATES the app-owned-state rule and must never be a
	 * default — it exists to answer one question in one run: is the black 2D
	 * zone caused by reusing the deposited flatten, or by something else in the
	 * replay? If the zone comes back with this set, the scratch is not
	 * surviving between the app frame and the repaint.
	 */
	{
		static int reflatten = -1;
		if (reflatten < 0) {
			const char *e = getenv("DXR_WEAVE_REPAINT_REFLATTEN");
			reflatten = (e != NULL && e[0] == '1') ? 1 : 0;
		}
		if (reflatten == 1) {
			reuse_twod = false;
		}
	}

	if (!reuse_twod) {
		if (have_local_2d) {
			gl_flatten_local_2d_layers(c, output_w, output_h, zones_frame ? -1 : proj_idx);
		} else {
			glBindFramebuffer(GL_FRAMEBUFFER, c->local2d_scratch_fbo);
			glViewport(0, 0, output_w, output_h);
			glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
			glClear(GL_COLOR_BUFFER_BIT);
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
		}

		// #491 part 3 — flatten the 2D-under layers PRE-weave and hand them to the DP
		// (it composites `backdrop over captured-desktop` under the 3D weave). 0 ⟹ no
		// under-layers. Must precede the weave redirect below.
		uint32_t bd_w = 0, bd_h = 0;
		GLuint bd_tex = gl_flatten_backdrop_2d(c, output_w, output_h, &bd_w, &bd_h);
		c->repaint.backdrop_tex = bd_tex;
		c->repaint.backdrop_w = bd_w;
		c->repaint.backdrop_h = bd_h;
	}

	// The DP always needs the backdrop re-handed to it: it is per-frame state
	// on the display processor, not a texture the DP retains across weaves.
	xrt_display_processor_gl_set_background_2d(c->display_processor, c->repaint.backdrop_tex,
	                                          c->repaint.backdrop_w, c->repaint.backdrop_h);

	// Redirect the DP weave into weave_tex.
	gl_dp_weave_to_fbo(c, atlas_tex, c->weave_fbo, output_w, output_h);

	/*
	 * #885 diag part 2 (same DXR_WEAVE_REPAINT_DIAG=1 gate): dump the three
	 * composite inputs of the FIRST repaint — and the first app frame, for the
	 * control pair — to %TEMP%\dxr_gl_diag_*.png. Mask is R8; the red channel
	 * is M (red = keep weave, black = show 2D).
	 */
	{
		static int diag2 = -1;
		if (diag2 < 0) {
			const char *e = getenv("DXR_WEAVE_REPAINT_DIAG");
			diag2 = (e != NULL && e[0] == '1') ? 1 : 0;
		}
		static bool dumped_rp = false, dumped_app = false;
		bool *dumped = reuse_twod ? &dumped_rp : &dumped_app;
		// Dump the app frame only AFTER the first repaint dump, so the pair is
		// adjacent in time (a frame-0 app dump catches pre-content scratch).
		if (diag2 == 1 && !*dumped && (reuse_twod || dumped_rp)) {
			*dumped = true;
			const char *tag = reuse_twod ? "repaint" : "app";
			const char *tmp = getenv("TEMP");
			GLuint dump_fbo = 0;
			glGenFramebuffers(1, &dump_fbo);
			GLuint texs[4] = {c->weave_tex, c->local2d_scratch_tex, mask_tex, atlas_tex};
			const char *names[4] = {"weave", "twod", "mask", "atlas"};
			for (int t = 0; tmp != NULL && t < 4; t++) {
				const uint32_t tw = (t == 3) ? c->atlas_tex_width : output_w;
				const uint32_t th = (t == 3) ? c->atlas_tex_height : output_h;
				if (tw == 0 || th == 0) {
					continue;
				}
				glBindFramebuffer(GL_FRAMEBUFFER, dump_fbo);
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
				                       texs[t], 0);
				if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
					continue;
				}
				uint8_t *px = (uint8_t *)malloc((size_t)tw * th * 4);
				if (px == NULL) {
					continue;
				}
				glReadPixels(0, 0, (GLsizei)tw, (GLsizei)th, GL_RGBA, GL_UNSIGNED_BYTE, px);
				// Force A=255 and flip to top-down for the PNG.
				for (size_t i = 3; i < (size_t)tw * th * 4; i += 4) {
					px[i] = 255;
				}
				char path[512];
				snprintf(path, sizeof(path), "%s/dxr_gl_diag_%s_%s.png", tmp, tag, names[t]);
				uint8_t *flipped = (uint8_t *)malloc((size_t)tw * th * 4);
				if (flipped != NULL) {
					for (uint32_t y = 0; y < th; y++) {
						memcpy(flipped + (size_t)y * tw * 4,
						       px + (size_t)(th - 1 - y) * tw * 4, (size_t)tw * 4);
					}
					stbi_write_png(path, (int)tw, (int)th, 4, flipped, (int)(tw * 4));
					free(flipped);
				}
				free(px);
			}
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			if (dump_fbo != 0) {
				glDeleteFramebuffers(1, &dump_fbo);
			}
			U_LOG_W("#885 diag: dumped %s composite inputs to %%TEMP%%\\dxr_gl_diag_%s_*.png", tag, tag);
		}
	}

	// Lerp M*weave + (1-M)*twod into the window framebuffer.
	glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
	glViewport(0, 0, output_w, output_h);
	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glUseProgram(c->program_masked_composite);
	glBindVertexArray(c->vao_empty);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, c->local2d_scratch_tex);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, mask_tex);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, c->weave_tex);
	glUniform1i(glGetUniformLocation(c->program_masked_composite, "u_twod"), 0);
	glUniform1i(glGetUniformLocation(c->program_masked_composite, "u_mask"), 1);
	glUniform1i(glGetUniformLocation(c->program_masked_composite, "u_weave"), 2);
	// #491: the implicit (auto) Local2D mask composites the 2D over the weave by
	// its own premultiplied alpha (translucent 2D reveals the 3D scene). The
	// explicit authored mask keeps the hard M-lerp.
	// XR_DXR_display_zones: MODE_ZONES (twod + (1−a)·(M·weave)) — the binary
	// zone raster (or the #803 feather ramp) gates only the WEAVE; Local2D
	// content composites on top by its own alpha (ADR-027/#801: the wish is
	// hardware-only; composition follows zone geometry + alpha). Formerly
	// the hard M-lerp, which multiplied overlays away inside zones and
	// dimmed the feathered edge.
	int composite_mode; // 0 = hard M-lerp, 1 = #491 premul over, 2 = zones
	if (zones_frame) {
		composite_mode = 2;
	} else if (have_explicit) {
		composite_mode = 0;
	} else {
		composite_mode = 1;
	}
	glUniform1i(glGetUniformLocation(c->program_masked_composite, "u_composite_mode"), composite_mode);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glActiveTexture(GL_TEXTURE0);

	static bool composite_logged = false;
	if (!composite_logged) {
		U_LOG_W("GL Local2D composite: %ux%u region, %s mask, twod=%s (mode=%d)", output_w, output_h,
		        zones_frame ? "zone" : (have_explicit ? "explicit" : "implicit"),
		        have_local_2d ? "local2d layers" : "(empty)", composite_mode);
		composite_logged = true;
	}
	return true;
}

// #224 / ADR-027 hardware-DP zone leg (P4) — one-time DP zone-capability
// probe, cached on the compositor. Returns true when the DP consumes
// published zone masks; caps are then in c->zone_dp_caps. Requires the
// compositor's GL context current (like every DP call on this leg).
static bool
gl_zone_dp_supported(struct comp_gl_compositor *c)
{
	if (c->display_processor == NULL) {
		return false;
	}
	if (c->zone_dp_state == 0) { // 0 = unqueried, 1 = supported, 2 = legacy
		struct xrt_dp_local_zone_caps caps = {};
		caps.struct_size = sizeof(caps);
		bool ok = xrt_display_processor_gl_get_local_zone_caps(c->display_processor, &caps);
		c->zone_dp_state = (ok && caps.supported != 0) ? 1 : 2;
		if (c->zone_dp_state == 1) {
			c->zone_dp_caps = caps;
			U_LOG_W("GL zone DP: local zones supported, grid %ux%u max_mask %ux%u max_hz %u "
			        "wish_fractional=%u granularity=%u",
			        caps.zone_grid_width, caps.zone_grid_height, caps.max_mask_width,
			        caps.max_mask_height, caps.max_update_hz, caps.wish_fractional,
			        caps.switch_granularity);
		}
	}
	return c->zone_dp_state == 1;
}

// Keep the DP's view of this client's zone mask in sync with the
// compositor's — the GL clone of d3d11_sync_zone_mask_to_dp. Called once per
// layer_commit while the compositor's GL context is still current (after the
// present-path composite, before the context restore), so the DP can sample
// the texture during the call with plain GL command ordering. Zones frame:
// the WISH this frame's composite resolved (the explicit authoring texture
// or the auto raster) — 0 on paths that never ran the composite; legacy
// frame: the sticky submitted mask. No resolvable source drives the
// clear-on-deactivate edge, once.
static void
gl_sync_zone_mask_to_dp(struct comp_gl_compositor *c)
{
	if (!gl_zone_dp_supported(c)) {
		return; // legacy DP — tier-1 global fallback path unchanged.
	}

	GLuint tex = 0;
	uint32_t mask_w = 0;
	uint32_t mask_h = 0;
	if (c->zones_frame) {
		tex = c->zone_publish_tex;
		mask_w = c->zone_publish_w;
		mask_h = c->zone_publish_h;
	} else {
		struct comp_gl_zone_mask *mask = c->active_zone_mask;
		if (mask != NULL && mask->submitted && mask->tex != 0) {
			tex = mask->tex;
			mask_w = mask->w;
			mask_h = mask->h;
		}
	}

	if (tex == 0) {
		if (c->zone_published) {
			xrt_display_processor_gl_clear_local_zone_mask(c->display_processor);
			c->zone_published = false;
		}
		return;
	}

#ifdef XRT_OS_WINDOWS
	// Screen-anchor the mask: client-area origin in physical screen pixels.
	// No HWND (offscreen) → nothing to anchor to; skip the publish.
	HWND wnd = c->hwnd;
	RECT r;
	POINT origin = {0, 0};
	if (wnd == NULL || !GetClientRect(wnd, &r) || r.right <= 0 || r.bottom <= 0 || !ClientToScreen(wnd, &origin)) {
		return;
	}

	bool ok = xrt_display_processor_gl_publish_local_zone_mask(c->display_processor, tex, mask_w, mask_h,
	                                                           (int32_t)origin.x, (int32_t)origin.y,
	                                                           (uint32_t)r.right, (uint32_t)r.bottom,
	                                                           c->zone_publish_seq);
	if (ok) {
		c->zone_published = true;
	}
#else
	// macOS GL: no screen-anchor helper on this path yet (Windows-first —
	// comp_gl_window_macos exposes dimensions only, not screen origin);
	// skip the publish. The clear edge above still runs.
	(void)mask_w;
	(void)mask_h;
#endif
}


/*
 *
 * MCP capture helpers
 *
 */

// Read the content region of atlas_texture (tile_columns × view_width by
// tile_rows × view_height — what actually got composited, matching what
// the compositor crops and sends to the DP), flip Y, and write @p path
// as PNG. Caller must have a current GL context.
static bool
gl_compositor_capture_atlas_to_png(struct comp_gl_compositor *c, const char *path)
{
	// #542: capture the frame's effective content region (what the passes
	// painted), falling back to the mode layout pre-first-commit.
	uint32_t cap_cols = c->eff_cols > 0 ? c->eff_cols : c->tile_columns;
	uint32_t cap_rows = c->eff_rows > 0 ? c->eff_rows : c->tile_rows;
	uint32_t cap_tile_w = c->eff_tile_w > 0 ? c->eff_tile_w : c->view_width;
	uint32_t cap_tile_h = c->eff_tile_h > 0 ? c->eff_tile_h : c->view_height;
	if (c->atlas_texture == 0 || cap_cols == 0 || cap_rows == 0 ||
	    cap_tile_w == 0 || cap_tile_h == 0) {
		return false;
	}

	uint32_t content_w = cap_cols * cap_tile_w;
	uint32_t content_h = cap_rows * cap_tile_h;
	if (content_w > c->atlas_tex_width)  content_w = c->atlas_tex_width;
	if (content_h > c->atlas_tex_height) content_h = c->atlas_tex_height;

	size_t row_pitch = (size_t)content_w * 4;
	size_t bytes = row_pitch * content_h;
	uint8_t *bottom_up = (uint8_t *)malloc(bytes);
	uint8_t *top_down = (uint8_t *)malloc(bytes);
	if (bottom_up == NULL || top_down == NULL) {
		free(bottom_up);
		free(top_down);
		return false;
	}

	// Attach atlas to a temporary read FBO; glReadPixels returns origin-
	// lower-left so we flip Y into top_down.
	GLuint prev_read_fbo = 0;
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, (GLint *)&prev_read_fbo);
	GLuint fbo = 0;
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->atlas_texture, 0);
	// Renderer renders content into FBO viewport (0, 0, content_w, content_h),
	// which in GL's lower-left origin lands at the bottom of the texture. Read
	// from y=0; the bottom_up→top_down flip below produces a top-down PNG.
	glReadPixels(0, 0, (GLsizei)content_w, (GLsizei)content_h, GL_RGBA, GL_UNSIGNED_BYTE, bottom_up);
	glFinish();
	glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read_fbo);
	glDeleteFramebuffers(1, &fbo);

	for (uint32_t y = 0; y < content_h; y++) {
		memcpy(top_down + (size_t)y * row_pitch,
		       bottom_up + (size_t)(content_h - 1 - y) * row_pitch,
		       row_pitch);
	}
	free(bottom_up);

	// Swapchain alpha is undefined for display output — force opaque so the
	// PNG doesn't render fully transparent/black (issue #425).
	u_image_force_opaque_rgba8(top_down, content_w, content_h, row_pitch);

	bool ok = stbi_write_png(path, (int)content_w, (int)content_h, 4, top_down, (int)row_pitch) != 0;
	free(top_down);
	return ok;
}

// Run the capture readback if the per-frame intent matches @p mode_filter.
// GL atlas is sample-able after either the projection-only loop or the full
// compose pass — same readback function for both modes.
static void
gl_compositor_dispatch_capture(struct comp_gl_compositor *c, uint32_t mode_filter)
{
	if (!u_capture_intent_should_capture(&c->capture_intent, mode_filter)) {
		return;
	}
	bool ok = gl_compositor_capture_atlas_to_png(c, c->capture_intent.path);
	if (ok) {
		U_LOG_I("Atlas captured (mode=%u) to %s",
		        c->capture_intent.mode, c->capture_intent.path);
	} else {
		U_LOG_W("Atlas capture failed (mode=%u path=%s)",
		        c->capture_intent.mode, c->capture_intent.path);
	}
	u_capture_intent_complete(&c->capture_intent, &c->mcp_capture, ok);
}


/*
 *
 * Layer commit — render atlas and present
 *
 */

// Per-frame effective CONTENT layout (#542) — same policy as the D3D11/D3D12
// legs: the content recipe is the ACTIVE MODE's, submissions are clamped to
// it (always-stereo apps submit identical views in a mono mode; zone layers
// carry zone-sized imageRects). The hardware weave-state never clamps
// content — divergence is the hardware-state override
// (xrRequestDisplayModeDXR), under which this layout keeps following the
// mode and the DP keeps weaving.
static void
gl_compute_effective_layout(struct comp_gl_compositor *c)
{
	uint32_t mode_cols = c->tile_columns > 0 ? c->tile_columns : 1;
	uint32_t mode_rows = c->tile_rows > 0 ? c->tile_rows : 1;
	uint32_t mode_tiles = mode_cols * mode_rows;

	uint32_t views = mode_tiles;
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		if (c->layer_accum.layers[i].data.type == XRT_LAYER_PROJECTION ||
		    c->layer_accum.layers[i].data.type == XRT_LAYER_PROJECTION_DEPTH ||
		    c->layer_accum.layers[i].data.type == XRT_LAYER_ZONE_3D) {
			views = c->layer_accum.layers[i].data.view_count;
			break;
		}
	}
	if (views == 0) {
		views = 1;
	}
	if (views > mode_tiles) {
		views = mode_tiles;
	}
	if (views > XRT_MAX_VIEWS) {
		views = XRT_MAX_VIEWS;
	}

	c->eff_views = views;
	if (views == 1) {
		c->eff_cols = 1;
		c->eff_rows = 1;
		c->eff_tile_w = mode_cols * c->view_width;
		c->eff_tile_h = mode_rows * c->view_height;
	} else {
		c->eff_cols = mode_cols;
		c->eff_rows = mode_rows;
		c->eff_tile_w = c->view_width;
		c->eff_tile_h = c->view_height;
	}
}

/*!
 * #868 / #875: bind the present target, run the masked composite (or the plain
 * weave), and present. Shared by the app frame and the repaint replay.
 *
 * Caller MUST hold c->mutex AND have c->hglrc current on this thread. A WGL
 * context lives on one thread at a time, so the repaint claims it in the gap
 * the frame path leaves after releasing it.
 *
 * `atlas_for_present` is compositor-owned on every path a repaint can run
 * (repaints are not armed for zero-copy, where the atlas IS the app's texture).
 * The one app-owned read in the composite — the Local2D flatten — is the #875
 * deposit half and is skipped here via reuse_twod; see gl_composite_local_2d.
 */
static void
gl_window_present(struct comp_gl_compositor *c, GLuint atlas_for_present, float dt, bool is_repaint)
{
	// Normal window mode: present to screen
	// Use actual window backing dimensions
	uint32_t present_w = c->tile_columns * c->view_width;
	uint32_t present_h = c->tile_rows * c->view_height;
#ifdef XRT_OS_WINDOWS
	if (c->hwnd != NULL) {
		RECT rc;
		if (GetClientRect(c->hwnd, &rc)) {
			uint32_t ww = (uint32_t)(rc.right - rc.left);
			uint32_t wh = (uint32_t)(rc.bottom - rc.top);
			if (ww > 0 && wh > 0) {
				present_w = ww;
				present_h = wh;
			}
		}
	}
#elif defined(__APPLE__)
	comp_gl_window_macos_get_dimensions(c->macos_window, &present_w, &present_h);
#endif

	// Bind the present target. Default WGL path: FBO 0 (window default
	// framebuffer). Transparent (DComp) path: lock the off-screen interop
	// transit texture and bind its FBO — the DP weaves into it, then the
	// D3D11 blit + Present below copies it to the DComp back buffer.
#ifdef XRT_OS_WINDOWS
	if (c->dcomp_active) {
		// The transit texture is fixed-size; resize isn't supported yet
		// (deferred follow-up). Clamp present dims to the setup dims.
		if (present_w != c->dcomp_present_w || present_h != c->dcomp_present_h) {
			present_w = c->dcomp_present_w;
			present_h = c->dcomp_present_h;
		}
		c->pfn_wglDXLockObjectsNV(c->dcomp_dx_interop_device, 1, &c->dcomp_transit_iop);
		glBindFramebuffer(GL_FRAMEBUFFER, c->dcomp_transit_fbo);
	} else
#endif
	{
#ifdef XRT_OS_WINDOWS
		if (c->dcomp_readback_active) {
			// No-interop readback path: weave into a dedicated RGBA FBO (NOT
			// FBO 0, whose default framebuffer drops alpha → opaque-black
			// holes). Source texture is fixed-size, so clamp present dims.
			if (present_w != c->dcomp_present_w || present_h != c->dcomp_present_h) {
				present_w = c->dcomp_present_w;
				present_h = c->dcomp_present_h;
			}
			glBindFramebuffer(GL_FRAMEBUFFER, c->dcomp_readback_gl_fbo);
		} else
#endif
		{
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
		}
	}

	if (c->display_processor != NULL) {
		// #439 Phase 3: when this frame carries Local2D layers or an
		// active submitted mask, run the post-weave masked composite
		// (DP weaves into weave_tex, then lerp M*weave+(1-M)*twod into the
		// present target). Otherwise the DP weaves straight to it — the
		// pre-Phase-3 path, byte-identical.
		//
		// The masked composite must lerp into the FBO bound just above —
		// the off-screen DComp transit FBO on the transparent path, the
		// readback FBO on the no-interop path, or FBO 0 (the window) on the
		// opaque path. Passing a hardcoded 0 here was only accidentally
		// correct for the opaque path: on the transparent DComp path it
		// wrote the zones composite into the window default framebuffer
		// (invisible on a WS_EX_NOREDIRECTIONBITMAP window) while the DComp
		// present blitted the still-stale transit FBO — a static on-screen
		// image even as the app submitted fresh frames (#613). The plain
		// projection path (gl_crop_and_process_dp) already weaves into the
		// bound FBO, which is why a non-zones transparent GL window animated.
		GLint present_target_fbo = 0;
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &present_target_fbo);
		/*
		 * #868 diag: when the composite declines, the caller falls back to the
		 * PLAIN weave — which drops the 2D band entirely. That presents as a
		 * black 2D zone with no error logged anywhere, and it is the one thing
		 * never checked on GL. Count both outcomes per weave kind.
		 */
		const bool composited = gl_composite_local_2d(c, atlas_for_present,
		                                              (GLuint)present_target_fbo, present_w,
		                                              present_h, /*reuse_twod=*/is_repaint);
		{
			static uint64_t app_ok = 0, app_no = 0, rp_ok = 0, rp_no = 0;
			if (is_repaint) {
				composited ? rp_ok++ : rp_no++;
			} else {
				composited ? app_ok++ : app_no++;
			}
			if (((app_ok + app_no + rp_ok + rp_no) % 240) == 0) {
				U_LOG_W("#868 gl composite: app ok=%llu declined=%llu | repaint ok=%llu declined=%llu",
				        (unsigned long long)app_ok, (unsigned long long)app_no,
				        (unsigned long long)rp_ok, (unsigned long long)rp_no);
			}
		}
		if (!composited) {
			gl_crop_and_process_dp(c, atlas_for_present, present_w, present_h);
		}
	} else {
		// No display processor: simple blit
		glViewport(0, 0, present_w, present_h);
		glUseProgram(c->program_blit);
		GLint loc_rect = glGetUniformLocation(c->program_blit, "u_src_rect");
		float blit_w = (c->atlas_tex_width > 0)
		    ? (float)(c->eff_cols * c->eff_tile_w) / (float)c->atlas_tex_width : 1.0f;
		float blit_h = (c->atlas_tex_height > 0)
		    ? (float)(c->eff_rows * c->eff_tile_h) / (float)c->atlas_tex_height : 1.0f;
		glUniform4f(loc_rect, 0.0f, 0.0f, blit_w, blit_h);
		GLint loc_flip = glGetUniformLocation(c->program_blit, "u_flip_y");
		glUniform1f(loc_flip, 0.0f);

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, atlas_for_present);
		GLint loc_out_tex = glGetUniformLocation(c->program_blit, "u_texture");
		glUniform1i(loc_out_tex, 0);

		glDrawArrays(GL_TRIANGLES, 0, 3);
	}

	// HUD overlay (post-weave, before swap)
	if (c->owns_window) {
		gl_compositor_render_hud(c, dt, present_w, present_h);
	}

	// Platform-specific swap
#ifdef XRT_OS_WINDOWS
	if (c->dcomp_active) {
		// Transparent path: flush GL writes to the transit texture, release
		// the interop lock, then blit + Present through DComp.
		glFlush();
		c->pfn_wglDXUnlockObjectsNV(c->dcomp_dx_interop_device, 1, &c->dcomp_transit_iop);
		gl_dcomp_present_frame(c);
		// Restore default FBO so other paths see what they expect.
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	} else if (c->dcomp_readback_active) {
		// No-interop transparent path: the weave is in FBO 0; glReadPixels it,
		// upload to the DYNAMIC texture, then blit + Present through DComp. No
		// SwapBuffers — DComp owns the (WS_EX_NOREDIRECTIONBITMAP) window.
		glFlush();
		gl_dcomp_readback_present_frame(c);
	} else {
		SwapBuffers(c->hdc);
		if (c->owns_window && c->own_window != NULL) {
			comp_d3d11_window_signal_paint_done(c->own_window);
		}
	}
#elif defined(XRT_OS_ANDROID)
	// eglSwapBuffers(c->egl_display, c->egl_surface);
#elif defined(__APPLE__)
	comp_gl_window_macos_swap_buffers(c->macos_window);
#endif
}

/*!
 * #868 repaint loop for GL.
 *
 * Same contract as the other backends, plus one GL-specific rule: a WGL context
 * can be current on at most ONE thread at a time. The frame path makes c->hglrc
 * current and releases it before returning, so between app frames it is current
 * nowhere — this thread claims it, replays, and MUST release it again before
 * dropping the lock, or the app's next frame cannot make it current and the
 * whole compositor wedges.
 */
static void *
gl_repaint_thread(void *ptr)
{
	struct comp_gl_compositor *c = (struct comp_gl_compositor *)ptr;

	while (os_thread_helper_is_running(&c->repaint_thread)) {
		// GL keeps no cached refresh rate; query the window's (cheap, cached
		// inside the helper) and fall back to 60 Hz off-Windows / on failure.
		double hz = 60.0;
#ifdef XRT_OS_WINDOWS
		const float win_hz = comp_display_refresh_hz_win(c->hwnd);
		if (win_hz > 1.0f) {
			hz = (double)win_hz;
		}
#endif
		const uint64_t period_ns = (uint64_t)(U_TIME_1S_IN_NS / hz);

		// #1257 partition: with a known fill schedule the window segments
		// are only a few ms wide, so tick fine enough to land in them.
		os_nanosleep((int64_t)((c->repaint.partition.next_release_ns != 0) ? period_ns / 12
		                                                                   : period_ns / 4));
		if (!os_thread_helper_is_running(&c->repaint_thread)) {
			break;
		}
		c->repaint.ticks++;

		if (c->repaint.force == 1 && (c->repaint.ticks % 240) == 0) {
			U_LOG_W("#868 gl repaint: ticks=%llu count=%llu armed=%d in_frame=%d",
			        (unsigned long long)c->repaint.ticks, (unsigned long long)c->repaint.count,
			        (int)c->repaint.armed, (int)c->repaint.app_frame_in_progress);
		}

		if (!c->repaint.armed || c->repaint.app_frame_in_progress) {
			continue;
		}
		// #1257: interval-aware gate — one missed vblank when the measured
		// app cadence is stable and slow, the legacy 2-period constant
		// otherwise. See u_repaint_gate.h.
		if (c->repaint.force != 1 &&
		    !u_repaint_gate_open(&c->repaint.gate, os_monotonic_get_ns(), period_ns, &c->repaint.partition)) {
			continue;
		}

		os_mutex_lock(&c->mutex);

		if (!os_thread_helper_is_running(&c->repaint_thread) || !c->repaint.armed ||
		    c->repaint.app_frame_in_progress || c->display_processor == NULL ||
		    c->repaint.atlas_tex == 0) {
			os_mutex_unlock(&c->mutex);
			continue;
		}
		// Re-run the gate under the lock (was a bare `quiet < period` floor;
		// the #1257 adaptive window opens at half a period).
		if (c->repaint.force != 1 &&
		    !u_repaint_gate_open(&c->repaint.gate, os_monotonic_get_ns(), period_ns, &c->repaint.partition)) {
			os_mutex_unlock(&c->mutex);
			continue;
		}

#ifdef XRT_OS_WINDOWS
		if (c->hdc == NULL || c->hglrc == NULL) {
			os_mutex_unlock(&c->mutex);
			continue;
		}
		// Claim the context. Nothing else holds it: the frame path released it
		// before returning, and the lock keeps it that way for this replay.
		if (!wglMakeCurrent(c->hdc, c->hglrc)) {
			os_mutex_unlock(&c->mutex);
			continue;
		}

		/*
		 * #868 diag: what GL state does the replay INHERIT?
		 *
		 * layer_commit saves and resets DEPTH_TEST / BLEND / CULL_FACE /
		 * SCISSOR_TEST before rendering, because GL state is per-context and
		 * sticky. The repaint path never did that — it was extracted from the
		 * present half only. If any of these read as enabled here, the replay
		 * is compositing under different state than the app frame, which would
		 * explain a black 2D band that survives re-flattening (the flatten is
		 * fine; the composite that consumes it is not).
		 *
		 * Measured before changing anything: the last GL "fix" applied without
		 * this step made things worse and had to be reverted.
		 */
		{
			static int logged = 0;
			if (logged < 5) {
				logged++;
				U_LOG_W("#868 gl repaint inherits: depth=%d blend=%d cull=%d scissor=%d fbo=%d",
				        (int)glIsEnabled(GL_DEPTH_TEST), (int)glIsEnabled(GL_BLEND),
				        (int)glIsEnabled(GL_CULL_FACE), (int)glIsEnabled(GL_SCISSOR_TEST),
				        [] { GLint fb = 0; glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fb); return (int)fb; }());
			}
		}

		gl_window_present(c, c->repaint.atlas_tex, c->repaint.last_dt, /*is_repaint=*/true);

		// Release unconditionally — see the note above about wedging the app.
		wglMakeCurrent(NULL, NULL);
#elif defined(__APPLE__)
		comp_gl_window_macos_make_current(c->macos_window);
		gl_window_present(c, c->repaint.atlas_tex, c->repaint.last_dt, /*is_repaint=*/true);
#endif

		c->repaint.count++;
		u_repaint_gate_note_repaint(&c->repaint.gate, os_monotonic_get_ns());
		os_mutex_unlock(&c->mutex);

		static bool logged = false;
		if (!logged) {
			logged = true;
			U_LOG_W("#868: repainting last atlas at %.1f Hz while the app is between frames "
			        "(set DXR_WEAVE_REPAINT=0 to disable)",
			        hz);
		}
	}

	return NULL;
}

/*!
 * The frame path proper. Runs with c->mutex HELD — see the locking wrapper
 * below. Keeps its early returns, which is why the lock lives in the wrapper.
 */
static xrt_result_t
gl_compositor_layer_commit_locked(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	struct comp_gl_compositor *c = gl_comp(xc);

	// Capture-intent poll — see u_capture_intent.h. Consumed at the
	// projection-done boundary (PROJECTION_ONLY) or end of frame
	// (POST_COMPOSE).
	u_capture_intent_poll(&c->capture_intent, &c->mcp_capture);

	// Frame timing
	uint64_t now_ns = os_monotonic_get_ns();
	float dt = (c->last_frame_ns > 0) ? (float)(now_ns - c->last_frame_ns) / 1e9f : 0.016f;
	c->last_frame_ns = now_ns;

	if (c->layer_accum.layer_count == 0) {
		return XRT_SUCCESS;
	}

	// #439 Phase 3 — detect Local2D layers once per frame; drives the
	// post-weave masked composite (the GL leg's consumer path).
	// XR_DXR_display_zones: the zones-frame flag is resolved in the same
	// scan (one coherent per-frame decision).
	c->local_2d_last_frame = false;
	c->zones_frame = false;
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		if (c->layer_accum.layers[i].data.type == XRT_LAYER_LOCAL_2D) {
			c->local_2d_last_frame = true;
		} else if (c->layer_accum.layers[i].data.type == XRT_LAYER_ZONE_3D) {
			c->zones_frame = true;
		}
	}

	// Save previous GL context and switch to compositor's
#ifdef XRT_OS_WINDOWS
	HDC prev_hdc = wglGetCurrentDC();
	HGLRC prev_hglrc = wglGetCurrentContext();
	wglMakeCurrent(c->hdc, c->hglrc);
#elif defined(__APPLE__)
	CGLContextObj prev_cgl_ctx = CGLGetCurrentContext();
	comp_gl_window_macos_make_current(c->macos_window);
#endif

	// Save and set GL state — the app may have left depth test, blend, etc. on
	GLboolean prev_depth_test = glIsEnabled(GL_DEPTH_TEST);
	GLboolean prev_blend = glIsEnabled(GL_BLEND);
	GLboolean prev_cull_face = glIsEnabled(GL_CULL_FACE);
	GLboolean prev_scissor_test = glIsEnabled(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);

	// Runtime-side 2D/3D toggle from qwerty V key
#ifdef XRT_BUILD_DRIVER_QWERTY
	if (c->xsysd != NULL) {
		bool force_2d = false;
		bool toggled = qwerty_check_display_mode_toggle(c->xsysd->xdevs, c->xsysd->xdev_count, &force_2d);
		if (toggled) {
			struct xrt_device *head = c->xsysd->static_roles.head;
			if (head != NULL && head->hmd != NULL) {
				if (force_2d) {
					uint32_t cur = head->hmd->active_rendering_mode_index;
					if (cur < head->rendering_mode_count &&
					    head->rendering_modes[cur].hardware_display_3d) {
						c->last_3d_mode_index = cur;
					}
					head->hmd->active_rendering_mode_index = 0;
				} else {
					head->hmd->active_rendering_mode_index = c->last_3d_mode_index;
				}
			}
			comp_gl_compositor_request_display_mode(&c->base.base, !force_2d);
		}

		// Rendering mode change from qwerty 0/1/2/3/4 keys.
		// Legacy apps only support V toggle — skip direct mode selection.
		if (!c->legacy_app_tile_scaling) {
			int render_mode = -1;
			if (qwerty_check_rendering_mode_change(c->xsysd->xdevs, c->xsysd->xdev_count, &render_mode)) {
				struct xrt_device *head = c->xsysd->static_roles.head;
				if (head != NULL) {
					xrt_device_set_property(head, XRT_DEVICE_PROPERTY_OUTPUT_MODE, render_mode);
				}
			}
		}
	}
#endif

	// XR_DXR_display_zones hardware leg (P4). Zone-capable DP: the per-frame
	// wish publish at the end of this commit drives the per-region switch —
	// skip the global fallback. Legacy DP (no zone slots): tier-1 fallback —
	// "any zone active => request 3D" once on the rising edge, no forced 2D
	// on the falling edge. (After the context switch above — same context
	// contract as the V-key toggle's request_display_mode call.)
	if (c->zones_frame && !c->zones_mode_requested && !gl_zone_dp_supported(c)) {
		c->zones_mode_requested = true;
		comp_gl_compositor_request_display_mode(&c->base.base, true);
	} else if (!c->zones_frame) {
		c->zones_mode_requested = false;
	}

	// Reset this frame's resolved wish texture — gl_composite_local_2d sets
	// it in zones frames; a stale name from an earlier frame must never
	// publish. (zone_publish_w/h persist as the previous raster's dims for
	// the auto-wish seq dirty-check.)
	c->zone_publish_tex = 0;

	// Sync hardware_display_3d, tile layout, and per-view dimensions
	// from device's active rendering mode (MUST be before zero-copy check and blit)
	if (c->xdev != NULL && c->xdev->hmd != NULL) {
		uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
		if (idx < c->xdev->rendering_mode_count) {
			const struct xrt_rendering_mode *mode = &c->xdev->rendering_modes[idx];
			c->hardware_display_3d = mode->hardware_display_3d;
			if (mode->tile_columns > 0) {
				c->tile_columns = mode->tile_columns;
				c->tile_rows = mode->tile_rows;
			}
			// Sync view dims from active mode every frame — needed for
			// correct crop before DP and correct blit UV calculation.
			// Legacy apps: view dims are fixed at compromise scale, skip.
			if (!c->legacy_app_tile_scaling && mode->view_width_pixels > 0) {
				c->view_width = mode->view_width_pixels;
				c->view_height = mode->view_height_pixels;
#if defined(XRT_OS_WINDOWS) || defined(__APPLE__)
				if (!c->owns_window || c->zones_frame) {
					// Handle app: window may be smaller than the display,
					// so scale view dims to the actual window client area
					// (matches the D3D11/D3D12 path) — keeps the atlas content
					// region, DP input, and atlas capture at window resolution.
					uint32_t win_w = 0, win_h = 0;
#ifdef XRT_OS_WINDOWS
					RECT rc;
					if (c->hwnd != NULL && GetClientRect(c->hwnd, &rc)) {
						win_w = (uint32_t)(rc.right - rc.left);
						win_h = (uint32_t)(rc.bottom - rc.top);
					}
#else
					comp_gl_window_macos_get_dimensions(c->macos_window, &win_w, &win_h);
#endif
					if (win_w > 0 && win_h > 0) {
						u_tiling_compute_canvas_view(mode, win_w, win_h,
						                             &c->view_width, &c->view_height);
					}
				}
#endif
			}
		}
	}

	// HUD is rendered in the window-mode present path (after weave, before swap)

	// Per-frame effective CONTENT layout (#542): tile grid/dims from the
	// SUBMISSION, decoupled from the hardware weave-state. Feeds the blit
	// passes, both DP crop helpers, the atlas dump, and the capture path —
	// they must all agree on the frame's geometry.
	gl_compute_effective_layout(c);

	// Zero-copy check: can we pass the app's swapchain directly to the DP?
	bool zero_copy = false;
	GLuint zc_texture = 0;
	{
		const struct xrt_rendering_mode *mode = NULL;
		if (c->xdev != NULL && c->xdev->hmd != NULL) {
			uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
			if (idx < c->xdev->rendering_mode_count)
				mode = &c->xdev->rendering_modes[idx];
		}
		if (mode != NULL && c->layer_accum.layer_count == 1) {
			struct comp_layer *layer = &c->layer_accum.layers[0];
			if (layer->data.type == XRT_LAYER_PROJECTION ||
			    layer->data.type == XRT_LAYER_PROJECTION_DEPTH) {
				uint32_t vc = mode->view_count;
				// #542: a hardware/content divergence frame (submitted
				// views != mode views) must take the atlas path — the
				// per-view loops below would read stale proj.v[] slots,
				// and zero-copy can't re-tile a mismatched submission.
				bool same_sc = (vc > 0 && vc <= XRT_MAX_VIEWS && layer->data.view_count == vc &&
				                layer->sc_array[0] != NULL);
				for (uint32_t v = 1; v < vc && same_sc; v++) {
					if (layer->sc_array[v] != layer->sc_array[0])
						same_sc = false;
				}
				if (same_sc) {
					uint32_t img_idx = layer->data.proj.v[0].sub.image_index;
					bool same_idx = true;
					for (uint32_t v = 1; v < vc; v++) {
						if (layer->data.proj.v[v].sub.image_index != img_idx) {
							same_idx = false;
							break;
						}
					}
					bool all_array_zero = same_idx;
					for (uint32_t v = 0; v < vc && all_array_zero; v++) {
						if (layer->data.proj.v[v].sub.array_index != 0)
							all_array_zero = false;
					}
					if (all_array_zero) {
						struct comp_gl_swapchain *gsc = gl_swapchain(layer->sc_array[0]);
						int32_t rxs[XRT_MAX_VIEWS], rys[XRT_MAX_VIEWS];
						uint32_t rws[XRT_MAX_VIEWS], rhs_arr[XRT_MAX_VIEWS];
						for (uint32_t v = 0; v < vc; v++) {
							rxs[v] = layer->data.proj.v[v].sub.rect.offset.w;
							rys[v] = layer->data.proj.v[v].sub.rect.offset.h;
							rws[v] = layer->data.proj.v[v].sub.rect.extent.w;
							rhs_arr[v] = layer->data.proj.v[v].sub.rect.extent.h;
						}
						if (u_tiling_can_zero_copy(vc, rxs, rys, rws, rhs_arr,
						                           gsc->info.width, gsc->info.height, mode)) {
							zero_copy = true;
							zc_texture = gsc->textures[img_idx];
						}
					}
				}
			}
		}
	}

	// --- Step 1: Render layers into atlas texture (skip if zero-copy) ---
	if (!zero_copy) {
	glBindFramebuffer(GL_FRAMEBUFFER, c->fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                        c->atlas_texture, 0);

	glViewport(0, 0, c->tile_columns * c->view_width, c->tile_rows * c->view_height);
	// Transparent-background apps clear their views to alpha=0; the atlas must
	// preserve that so the woven output composes through the desktop. Opaque
	// apps keep the alpha=1 clear (unchanged). The per-eye blit below must be a
	// REPLACE (blend off) so the app's alpha is written verbatim rather than
	// blended over this clear.
	// XR_DXR_display_zones (ADR-027): a zones frame composes N placed zone
	// layers into the window-spanning atlas — the unzoned area must weave to
	// nothing (transparent) so the MODE_ZONES composite (and any #803
	// feather ramp) blends toward the desktop.
	glClearColor(0.0f, 0.0f, 0.0f, (c->transparent_background || c->zones_frame) ? 0.0f : 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	// XR_DXR_display_zones: zone rects are client-window px and the tile
	// spans the full window in zones frames, so the zone scale target is
	// the window client area (content dims as the headless fallback).
	uint32_t zones_target_w = 0;
	uint32_t zones_target_h = 0;
	if (c->zones_frame) {
#ifdef XRT_OS_WINDOWS
		RECT zrc;
		if (c->hwnd != NULL && GetClientRect(c->hwnd, &zrc) && zrc.right > 0 && zrc.bottom > 0) {
			zones_target_w = (uint32_t)zrc.right;
			zones_target_h = (uint32_t)zrc.bottom;
		}
#elif defined(__APPLE__)
		comp_gl_window_macos_get_dimensions(c->macos_window, &zones_target_w, &zones_target_h);
#endif
		if (zones_target_w == 0 || zones_target_h == 0) {
			zones_target_w = c->tile_columns * c->view_width;
			zones_target_h = c->tile_rows * c->view_height;
		}
	}

	glUseProgram(c->program_blit);
	glBindVertexArray(c->vao_empty);
	glDisable(GL_BLEND);

	GLint loc_tex = glGetUniformLocation(c->program_blit, "u_texture");
	GLint loc_rect = glGetUniformLocation(c->program_blit, "u_src_rect");
	// Ensure no Y-flip for atlas blit (u_flip_y may be stale from IOSurface blit)
	GLint loc_flip_atlas = glGetUniformLocation(c->program_blit, "u_flip_y");
	glUniform1f(loc_flip_atlas, 0.0f);

	// Uniform locations for the LAYERED (array) blit variant, used per-draw when
	// a swapchain has arraySize>1 (see the per-eye branch below).
	GLint loc_tex_arr = glGetUniformLocation(c->program_blit_array, "u_texture");
	GLint loc_rect_arr = glGetUniformLocation(c->program_blit_array, "u_src_rect");
	GLint loc_layer_arr = glGetUniformLocation(c->program_blit_array, "u_layer");
	GLint loc_flip_arr = glGetUniformLocation(c->program_blit_array, "u_flip_y");

	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		struct comp_layer *layer = &c->layer_accum.layers[i];

		// XR_DXR_display_zones: zone layers blit through the same pass at
		// a sub-tile viewport (alpha-over in layer-list order).
		const bool is_zone = layer->data.type == XRT_LAYER_ZONE_3D;
		if (layer->data.type != XRT_LAYER_PROJECTION &&
		    layer->data.type != XRT_LAYER_PROJECTION_DEPTH && !is_zone) {
			continue;
		}

		// CONTENT tile count from the SUBMISSION-derived effective layout
		// (#542) — no longer clamped by the hardware weave-state. Per-layer
		// view counts are still bounded by the frame's placement grid.
		uint32_t view_count = layer->data.view_count;
		if (view_count > c->eff_views) view_count = c->eff_views;
		if (view_count == 0) view_count = 1;
		for (uint32_t eye = 0; eye < view_count; eye++) {

			struct xrt_swapchain *sc = layer->sc_array[eye];
			if (sc == NULL) {
				continue;
			}

			struct comp_gl_swapchain *gsc = gl_swapchain(sc);
			uint32_t img_idx = layer->data.proj.v[eye].sub.image_index;
			if (img_idx >= gsc->image_count) {
				continue;
			}

			// Source rect from layer
			struct xrt_normalized_rect nr = layer->data.proj.v[eye].sub.norm_rect;
			if (nr.w <= 0.0f || nr.h <= 0.0f) {
				nr.x = 0.0f;
				nr.y = 0.0f;
				nr.w = 1.0f;
				nr.h = 1.0f;
			}

			// Set viewport for this eye in the atlas texture — tile-place
			// by the effective grid (#542). Mono content (views == 1)
			// gets one tile spanning the full content region; the DP
			// weaves or flattens per its own mode_3d.
			uint32_t tbx, tby, tbw, tbh; // per-view tile box
			{
				uint32_t tile_x = eye % c->eff_cols;
				uint32_t tile_y = eye / c->eff_cols;
				tbx = tile_x * c->eff_tile_w;
				tby = tile_y * c->eff_tile_h;
				tbw = c->eff_tile_w;
				tbh = c->eff_tile_h;
			}
			if (is_zone) {
				// XR_DXR_display_zones: scale the zone rect (client-
				// window px, top-left origin) into the tile box — in
				// zones frames the tile spans the full window, so
				// scale = tile/window. The atlas is a GL bottom-left
				// framebuffer, so the placement Y flips within the
				// tile (zone content stays GL-oriented, like a
				// projection tile). Premul vs straight alpha-over per
				// the UNPREMULTIPLIED flag.
				if (zones_target_w == 0 || zones_target_h == 0) {
					continue;
				}
				const struct xrt_rect *zr = &layer->data.zone_3d.rect;
				const float zsx = (float)tbw / (float)zones_target_w;
				const float zsy = (float)tbh / (float)zones_target_h;
				GLint zx = (GLint)tbx + (GLint)((float)zr->offset.w * zsx);
				GLint zy = (GLint)tby +
				           (GLint)((float)((int32_t)zones_target_h - zr->offset.h - zr->extent.h) *
				                   zsy);
				GLsizei zw = (GLsizei)((float)zr->extent.w * zsx);
				GLsizei zh = (GLsizei)((float)zr->extent.h * zsy);
				if (zw <= 0 || zh <= 0) {
					continue;
				}
				glViewport(zx, zy, zw, zh);
				glEnable(GL_BLEND);
				if ((layer->data.flags & XRT_LAYER_COMPOSITION_UNPREMULTIPLIED_ALPHA_BIT) != 0) {
					glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
					                    GL_ONE_MINUS_SRC_ALPHA);
				} else {
					glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
					                    GL_ONE_MINUS_SRC_ALPHA);
				}
			} else {
				glViewport(tbx, tby, tbw, tbh);
			}

			// Honor the projection view's array layer (imageArrayIndex).
			// Layered swapchains sample the requested slice via the
			// sampler2DArray variant; single-layer swapchains keep the
			// sampler2D path. Program is selected per-draw (arrays are rare).
			const bool layered = gsc->target == GL_TEXTURE_2D_ARRAY;
			glActiveTexture(GL_TEXTURE0);
			if (layered) {
				uint32_t array_index = layer->data.proj.v[eye].sub.array_index;
				glUseProgram(c->program_blit_array);
				glUniform1f(loc_flip_arr, 0.0f);
				glBindTexture(GL_TEXTURE_2D_ARRAY, gsc->textures[img_idx]);
				glUniform1i(loc_tex_arr, 0);
				glUniform4f(loc_rect_arr, nr.x, nr.y, nr.w, nr.h);
				glUniform1f(loc_layer_arr, (float)array_index);
			} else {
				glUseProgram(c->program_blit);
				glBindTexture(GL_TEXTURE_2D, gsc->textures[img_idx]);
				glUniform1i(loc_tex, 0);
				glUniform4f(loc_rect, nr.x, nr.y, nr.w, nr.h);
			}

			// One-shot diagnostic: log blit params for both eyes
			{
				static int blit_log = 0;
				if (blit_log < 4) {
					U_LOG_W("GL BLIT eye=%u tex=%u img=%u nr=(%.3f,%.3f,%.3f,%.3f) "
					        "vp=(%u,%u,%u,%u) view_count=%u hw3d=%d",
					        eye, gsc->textures[img_idx], img_idx,
					        nr.x, nr.y, nr.w, nr.h,
					        tbx, tby, tbw, tbh,
					        view_count, c->hardware_display_3d);
					blit_log++;
				}
			}

			// Draw fullscreen quad (3 vertices, generated in vertex shader)
			glDrawArrays(GL_TRIANGLES, 0, 3);

			// XR_DXR_display_zones: restore the REPLACE blit state for
			// the next (projection) draw.
			if (is_zone) {
				glDisable(GL_BLEND);
			}
		}
	}

	// Projection-only capture point — atlas now contains projection
	// content for every tile; window-space layers haven't been rendered
	// yet. Atlas is bound as the FBO color attachment; the capture
	// readback temporarily attaches its own FBO and reads via
	// glReadPixels (origin lower-left).
	gl_compositor_dispatch_capture(c, MCP_CAPTURE_MODE_PROJECTION_ONLY);

	// --- Step 1b: Render window-space layers (HUD overlays) ---
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		struct comp_layer *layer = &c->layer_accum.layers[i];

		if (layer->data.type != XRT_LAYER_WINDOW_SPACE) {
			continue;
		}

		const struct xrt_layer_window_space_data *ws = &layer->data.window_space;
		struct xrt_swapchain *sc = layer->sc_array[0];
		if (sc == NULL) {
			continue;
		}

		struct comp_gl_swapchain *gsc = gl_swapchain(sc);
		uint32_t img_idx = ws->sub.image_index;
		if (img_idx >= gsc->image_count) {
			continue;
		}

		// Sub-image UV rect
		struct xrt_normalized_rect nr = ws->sub.norm_rect;
		if (nr.w <= 0.0f || nr.h <= 0.0f) {
			nr.x = 0.0f;
			nr.y = 0.0f;
			nr.w = 1.0f;
			nr.h = 1.0f;
		}

		glUseProgram(c->program_window_space);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		GLint loc_ws_rect = glGetUniformLocation(c->program_window_space, "u_rect");
		GLint loc_ws_tex = glGetUniformLocation(c->program_window_space, "u_texture");
		GLint loc_ws_src = glGetUniformLocation(c->program_window_space, "u_src_rect");

		// HUD tiles align with the projection tiles: same effective grid
		// (#542), independent of the hardware weave-state.
		uint32_t effective_views = c->eff_views > 0 ? c->eff_views : 1;
		for (uint32_t eye = 0; eye < effective_views; eye++) {
			// Set viewport for this eye in the effective grid
			{
				uint32_t tile_x = eye % c->eff_cols;
				uint32_t tile_y = eye / c->eff_cols;
				glViewport(tile_x * c->eff_tile_w, tile_y * c->eff_tile_h,
				           c->eff_tile_w, c->eff_tile_h);
			}

			// Per-view disparity, graded across the view sweep (#413):
			// first view = -half, last = +half. Degenerates to the classic
			// -/+ pair for 2-view modes; a single view gets no shift.
			float half_disp = ws->disparity / 2.0f;
			float eye_shift = 0.0f;
			if (effective_views > 1) {
				float t = (float)eye / (float)(effective_views - 1);
				eye_shift = -half_disp + ws->disparity * t;
			}

			// Window-space fractional coords → NDC [-1, 1]
			float ndc_x = (ws->x + eye_shift) * 2.0f - 1.0f;
			float ndc_y = 1.0f - (ws->y + ws->height) * 2.0f; // flip Y: GL origin is bottom-left
			float ndc_w = ws->width * 2.0f;
			float ndc_h = ws->height * 2.0f;

			glUniform4f(loc_ws_rect, ndc_x, ndc_y, ndc_w, ndc_h);
			glUniform4f(loc_ws_src, nr.x, nr.y, nr.w, nr.h);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, gsc->textures[img_idx]);
			glUniform1i(loc_ws_tex, 0);

			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		}

		glDisable(GL_BLEND);
	}

	// File-triggered atlas dump for autonomous screenshot verification.
	// `touch /tmp/dxr_atlas_trigger` and the next frame writes the
	// composited content region to /tmp/dxr_atlas.png. Mirrors the
	// Windows D3D11-service screenshot trigger pattern. Runs while
	// c->fbo is still bound with atlas as the color attachment, after
	// both projection and WS-layer passes have completed.
	{
		struct stat _st;
		if (c->atlas_texture != 0 && c->eff_cols > 0 && c->eff_rows > 0 &&
		    c->eff_tile_w > 0 && c->eff_tile_h > 0 &&
		    stat("/tmp/dxr_atlas_trigger", &_st) == 0) {
			unlink("/tmp/dxr_atlas_trigger");
			// Effective content region (#542): what this frame painted.
			uint32_t cw = c->eff_cols * c->eff_tile_w;
			uint32_t ch = c->eff_rows * c->eff_tile_h;
			if (cw > c->atlas_tex_width)  cw = c->atlas_tex_width;
			if (ch > c->atlas_tex_height) ch = c->atlas_tex_height;
			size_t row_pitch = (size_t)cw * 4;
			size_t bytes = row_pitch * ch;
			uint8_t *bu = (uint8_t *)malloc(bytes);
			uint8_t *td = (uint8_t *)malloc(bytes);
			if (bu != NULL && td != NULL) {
				glReadBuffer(GL_COLOR_ATTACHMENT0);
				GLint sy = (GLint)c->atlas_tex_height - (GLint)ch;
				if (sy < 0) sy = 0;
				glReadPixels(0, sy, (GLsizei)cw, (GLsizei)ch, GL_RGBA, GL_UNSIGNED_BYTE, bu);
				glFinish();
				for (uint32_t y = 0; y < ch; y++) {
					memcpy(td + (size_t)y * row_pitch,
					       bu + (size_t)(ch - 1 - y) * row_pitch, row_pitch);
				}
				stbi_write_png("/tmp/dxr_atlas.png", (int)cw, (int)ch, 4, td, (int)row_pitch);
			}
			free(bu); free(td);
		}
	}
	} // end if (!zero_copy)

	// --- Step 2: Present atlas texture ---
	// Ensure VAO is bound for present draw calls (zero-copy skips the atlas
	// blit which normally binds it, causing GL_INVALID_OPERATION in core profile)
	glBindVertexArray(c->vao_empty);
	GLuint atlas_for_present = zero_copy ? zc_texture : c->atlas_texture;
#ifdef XRT_OS_WINDOWS
	if (c->has_shared_texture) {
		// Shared-texture present: weave into the dedicated GL render target
		// (shared_present_fbo → shared_gl_texture), then bridge the result into
		// the app's shared D3D surface via gl_shared_readback_upload
		// (glReadPixels → UpdateSubresource). The WGL_NV_DX_interop2 write-back
		// is bypassed — unreliable on this stack.
		glBindFramebuffer(GL_FRAMEBUFFER, c->shared_present_fbo);

		// Clear the whole render target to transparent first: in a zones frame
		// the composite/weave only paints the window sub-rect viewport, so the
		// surround must read through to the desktop (alpha 0) rather than show
		// stale pixels from a prior frame.
		glDisable(GL_SCISSOR_TEST);
		glViewport(0, 0, c->shared_width, c->shared_height);
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);

		if (c->display_processor != NULL) {
			// DP target dims: canvas for texture apps; the full surface
			// otherwise.
			uint32_t dp_w = c->shared_width;
			uint32_t dp_h = c->shared_height;
			if (c->zones_frame) {
				// XR_DXR_display_zones: the zone PLACEMENT (layer_commit)
				// scales zone rects into the atlas tile by tile/window, so
				// the weave OUTPUT must be the window client dims too — NOT
				// the full shared surface. Mirrors the VK leg (dp_target =
				// settings.preferred = window dims) and the window-present
				// path.
				RECT zrc;
				if (c->hwnd != NULL && GetClientRect(c->hwnd, &zrc) &&
				    zrc.right > 0 && zrc.bottom > 0) {
					dp_w = (uint32_t)zrc.right;
					dp_h = (uint32_t)zrc.bottom;
				}
			}

			// Run the #439/#491/zones masked composite (transparency +
			// Local2D + zone wish) INTO the dedicated present FBO, exactly
			// like the window-present path. This is what makes the surround
			// see-through and composites the 2D zones — the plain weave is
			// opaque. Falls back to the plain weave when no composite is
			// needed (the helper returns false). Both land in
			// shared_present_fbo (distinct from the c->fbo the crop reuses).
			if (!gl_composite_local_2d(c, atlas_for_present, c->shared_present_fbo, dp_w, dp_h, /*reuse_twod=*/false)) {
				gl_crop_and_process_dp(c, atlas_for_present, dp_w, dp_h);
			}

			// Bridge the woven (dp_w×dp_h) region into the app's shared
			// texture. Re-bind the present FBO: the composite/crop bind their
			// own FBOs internally. (The zone-wish publish to the DP happens
			// once at the end of layer_commit via gl_sync_zone_mask_to_dp,
			// which consumes the zone_publish_tex this composite resolved.)
			glBindFramebuffer(GL_FRAMEBUFFER, c->shared_present_fbo);
			gl_shared_readback_upload(c, dp_w, dp_h);
		} else {
			// No display processor: simple blit.
			glViewport(0, 0, c->shared_width, c->shared_height);
			glUseProgram(c->program_blit);
			glBindVertexArray(c->vao_empty);
			GLint loc_rect = glGetUniformLocation(c->program_blit, "u_src_rect");
			float sh_w = (c->atlas_tex_width > 0)
			    ? (float)(c->eff_cols * c->eff_tile_w) / (float)c->atlas_tex_width : 1.0f;
			float sh_h = (c->atlas_tex_height > 0)
			    ? (float)(c->eff_rows * c->eff_tile_h) / (float)c->atlas_tex_height : 1.0f;
			glUniform4f(loc_rect, 0.0f, 0.0f, sh_w, sh_h);
			GLint loc_flip = glGetUniformLocation(c->program_blit, "u_flip_y");
			glUniform1f(loc_flip, 0.0f);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, atlas_for_present);
			GLint loc_out_tex = glGetUniformLocation(c->program_blit, "u_texture");
			glUniform1i(loc_out_tex, 0);
			glDrawArrays(GL_TRIANGLES, 0, 3);

			// Bridge the full surface into the app's shared texture.
			gl_shared_readback_upload(c, c->shared_width, c->shared_height);
		}

		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	} else
#endif
#ifdef __APPLE__
	if (c->has_shared_iosurface) {
		// Shared IOSurface mode: render into the IOSurface via its dedicated
		// FBO. Unlike the Windows D3D path, the GL→IOSurface write is direct
		// (the GL texture IS the IOSurface backing) — no readback bridge
		// needed. But the zones present must mirror the Windows leg otherwise:
		// dedicated FBO (the DP crop clobbers c->fbo), a transparent clear, the
		// window-dims zone weave, and the masked composite (for see-through +
		// Local2D). Content stays GL bottom-up; the app's blit must NOT flip Y.
		glBindFramebuffer(GL_FRAMEBUFFER, c->iosurface_present_fbo);

		// Clear the whole IOSurface to transparent first: in a zones frame the
		// composite/weave only paints the window sub-rect viewport, so the
		// surround must read through (alpha 0), not show stale pixels.
		glDisable(GL_SCISSOR_TEST);
		glViewport(0, 0, c->iosurface_width, c->iosurface_height);
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);

		if (c->display_processor != NULL) {
			// DP target dims: canvas for texture apps; the full surface
			// otherwise.
			uint32_t dp_w = c->iosurface_width;
			uint32_t dp_h = c->iosurface_height;
			if (c->zones_frame) {
				// XR_DXR_display_zones: the zone PLACEMENT (layer_commit)
				// scales zone rects into the atlas tile by tile/window, so
				// the weave OUTPUT must be the window client dims too — NOT
				// the full IOSurface. Parity with the Windows shared path.
				uint32_t win_w = 0, win_h = 0;
				comp_gl_window_macos_get_dimensions(c->macos_window, &win_w, &win_h);
				if (win_w > 0 && win_h > 0) {
					dp_w = win_w;
					dp_h = win_h;
				}
			}

			// Run the #439/#491/zones masked composite (transparency +
			// Local2D + zone wish) INTO the dedicated IOSurface FBO, exactly
			// like the window-present path. Falls back to the plain weave when
			// no composite is needed (the helper returns false). The zone-wish
			// publish to the DP happens once at end of layer_commit via
			// gl_sync_zone_mask_to_dp, which consumes the resolved wish.
			if (!gl_composite_local_2d(c, atlas_for_present, c->iosurface_present_fbo, dp_w, dp_h, /*reuse_twod=*/false)) {
				gl_crop_and_process_dp(c, atlas_for_present, dp_w, dp_h);
			}
		} else {
			// No display processor: simple blit, no Y-flip.
			glViewport(0, 0, c->iosurface_width, c->iosurface_height);
			glUseProgram(c->program_blit);
			glBindVertexArray(c->vao_empty);
			GLint loc_rect = glGetUniformLocation(c->program_blit, "u_src_rect");
			float used_w = (c->atlas_tex_width > 0)
			    ? (float)(c->eff_cols * c->eff_tile_w) / (float)c->atlas_tex_width : 1.0f;
			float used_h = (c->atlas_tex_height > 0)
			    ? (float)(c->eff_rows * c->eff_tile_h) / (float)c->atlas_tex_height : 1.0f;
			glUniform4f(loc_rect, 0.0f, 0.0f, used_w, used_h);
			GLint loc_flip = glGetUniformLocation(c->program_blit, "u_flip_y");
			glUniform1f(loc_flip, 0.0f);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, atlas_for_present);
			GLint loc_out_tex = glGetUniformLocation(c->program_blit, "u_texture");
			glUniform1i(loc_out_tex, 0);
			glDrawArrays(GL_TRIANGLES, 0, 3);
		}

		glFlush();

		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	} else
#endif
	{
		// #868: publish what the repaint thread replays. Armed only off the
		// zero-copy path — there the atlas IS the app's own texture, which it
		// redraws, so replaying it would weave whatever the app has since drawn.
		c->repaint.atlas_tex = atlas_for_present;
		c->repaint.armed = !zero_copy;
		// The HUD's dt is the APP's frame delta; a repaint reports the last
		// real frame's, not a repaint-to-repaint interval.
		c->repaint.last_dt = dt;

		gl_window_present(c, atlas_for_present, dt, /*is_repaint=*/false);

		/*
		 * #885 diag probe (DXR_WEAVE_REPAINT_APPTHREAD=1): run the EXACT repaint
		 * replay — same is_repaint=true path, same reuse_twod semantics — but on
		 * the APP thread, immediately after the real present. Discriminates
		 * "the replay path is broken" from "the DP is thread-affine": if this
		 * replay weaves zone B + background correctly while the repaint-thread
		 * one blacks them out, the difference is the THREAD, not the path.
		 * Diagnostic only; never a default (double weave per frame).
		 */
		{
			static int app_replay = -1;
			if (app_replay < 0) {
				const char *e = getenv("DXR_WEAVE_REPAINT_APPTHREAD");
				app_replay = (e != NULL && e[0] == '1') ? 1 : 0;
			}
			if (app_replay == 1 && !zero_copy) {
				gl_window_present(c, atlas_for_present, dt, /*is_repaint=*/true);
			}
		}

		// Only a REAL frame resets the quiet-gate.
		c->repaint.last_app_frame_ns = os_monotonic_get_ns();
		u_repaint_gate_on_app_frame(&c->repaint.gate, c->repaint.last_app_frame_ns);
	}

	// Cache eye positions AFTER process_atlas (which updates the SR weaver's
	// eye tracker). xrLocateViews calls get_predicted_eye_positions BEFORE
	// layer_commit, so it needs cached data from the previous frame.
	if (c->display_processor != NULL) {
		struct xrt_eye_positions fresh_eyes = {0};
		bool fresh_ok = xrt_display_processor_gl_get_predicted_eye_positions(
		        c->display_processor, &fresh_eyes);
		static int cache_log = 0;
		if (cache_log < 5) {
			U_LOG_W("EYE-CACHE[%d]: ok=%d valid=%d count=%d "
			        "e0=(%.4f,%.4f,%.4f) e1=(%.4f,%.4f,%.4f)",
			        cache_log, fresh_ok, fresh_eyes.valid, fresh_eyes.count,
			        fresh_eyes.eyes[0].x, fresh_eyes.eyes[0].y, fresh_eyes.eyes[0].z,
			        fresh_eyes.eyes[1].x, fresh_eyes.eyes[1].y, fresh_eyes.eyes[1].z);
			cache_log++;
		}
		if (fresh_ok && fresh_eyes.valid) {
			c->cached_eye_pos = fresh_eyes;
			c->have_cached_eye_pos = true;
		}
	}

	glBindVertexArray(0);
	glUseProgram(0);
	glBindTexture(GL_TEXTURE_2D, 0);

	// Restore previous GL state
	if (prev_depth_test) glEnable(GL_DEPTH_TEST);
	if (prev_blend) glEnable(GL_BLEND);
	if (prev_cull_face) glEnable(GL_CULL_FACE);
	if (prev_scissor_test) glEnable(GL_SCISSOR_TEST);

	// #224 / ADR-027 P4: sideband-sync this client's zone state with the DP
	// while our GL context is still current (the DP samples the texture
	// during the call) — in zones frames this publishes the WISH the
	// composite just resolved; in legacy frames the sticky submitted mask;
	// the clear edge otherwise.
	gl_sync_zone_mask_to_dp(c);

	// Post-compose capture (#210) — runs while our GL context is still
	// current so glReadPixels from atlas_texture is valid. Skipped if the
	// intent was projection-only (consumed earlier) or empty.
	gl_compositor_dispatch_capture(c, MCP_CAPTURE_MODE_POST_COMPOSE);

	// Restore previous GL context (critical for shared texture mode where
	// app has its own context and needs it back after compositor work)
#ifdef XRT_OS_WINDOWS
	if (prev_hglrc != NULL) {
		wglMakeCurrent(prev_hdc, prev_hglrc);
	} else {
		wglMakeCurrent(NULL, NULL);
	}
#elif defined(__APPLE__)
	if (prev_cgl_ctx != NULL) {
		CGLSetCurrentContext(prev_cgl_ctx);
	}
#endif

	return XRT_SUCCESS;
}

/*!
 * #868 locking wrapper. Serialises the whole frame path against the repaint
 * replay, so the GL context, the vendor weave and the present are never
 * concurrent with a repaint's.
 *
 * app_frame_in_progress is cleared here — under the lock, after the frame path
 * returns — and unconditionally, so an early return inside cannot leak it and
 * wedge the repaint loop off permanently.
 */
static xrt_result_t
gl_compositor_layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	struct comp_gl_compositor *c = gl_comp(xc);

	os_mutex_lock(&c->mutex);
	xrt_result_t xret = gl_compositor_layer_commit_locked(xc, sync_handle);
	c->repaint.app_frame_in_progress = false;
	os_mutex_unlock(&c->mutex);

	return xret;
}


/*
 *
 * Compositor destroy
 *
 */

static void
gl_compositor_destroy(struct xrt_compositor *xc)
{
	struct comp_gl_compositor *c = gl_comp(xc);

	// #868: stop the repaint loop FIRST — it holds the GL context and touches
	// the display processor, both torn down below.
	os_thread_helper_destroy(&c->repaint_thread);
	os_mutex_destroy(&c->mutex);

	mcp_capture_uninstall();
	mcp_capture_fini(&c->mcp_capture);

#ifdef XRT_OS_WINDOWS
	// Make compositor context current for GL resource cleanup
	if (c->hglrc) {
		wglMakeCurrent(c->hdc, c->hglrc);
	}
#endif

	// #224 P4: withdraw this client's zone contribution from the vendor's
	// union before the DP goes away (clear-on-teardown edge; the
	// compositor's GL context was made current above).
	if (c->zone_published && c->display_processor != NULL) {
		xrt_display_processor_gl_clear_local_zone_mask(c->display_processor);
		c->zone_published = false;
	}
	xrt_display_processor_gl_destroy(&c->display_processor);

	// Destroy HUD
	if (c->hud_texture) glDeleteTextures(1, &c->hud_texture);
	u_hud_destroy(&c->hud);

	if (c->dp_input_texture) glDeleteTextures(1, &c->dp_input_texture);
	if (c->dp_crop_fbo) glDeleteFramebuffers(1, &c->dp_crop_fbo);
	if (c->program_blit) glDeleteProgram(c->program_blit);
	if (c->program_blit_array) glDeleteProgram(c->program_blit_array);
	if (c->program_window_space) glDeleteProgram(c->program_window_space);
	if (c->program_masked_composite) glDeleteProgram(c->program_masked_composite);
	if (c->vao_empty) glDeleteVertexArrays(1, &c->vao_empty);
	if (c->fbo) glDeleteFramebuffers(1, &c->fbo);
	if (c->atlas_texture) glDeleteTextures(1, &c->atlas_texture);
	// #439 Phase 3 — Local2D composite scratch.
	if (c->weave_tex) glDeleteTextures(1, &c->weave_tex);
	if (c->weave_fbo) glDeleteFramebuffers(1, &c->weave_fbo);
	if (c->local2d_scratch_tex) glDeleteTextures(1, &c->local2d_scratch_tex);
	if (c->local2d_scratch_fbo) glDeleteFramebuffers(1, &c->local2d_scratch_fbo);
	if (c->implicit_mask_tex) glDeleteTextures(1, &c->implicit_mask_tex);
	if (c->implicit_mask_fbo) glDeleteFramebuffers(1, &c->implicit_mask_fbo);
	if (c->feather_mask_tex) glDeleteTextures(1, &c->feather_mask_tex);
	if (c->feather_mask_fbo) glDeleteFramebuffers(1, &c->feather_mask_fbo);
	// #491 part 3 — 2D-under backdrop scratch.
	if (c->backdrop_scratch_tex) glDeleteTextures(1, &c->backdrop_scratch_tex);
	if (c->backdrop_scratch_fbo) glDeleteFramebuffers(1, &c->backdrop_scratch_fbo);

#ifdef XRT_OS_WINDOWS
	// Clean up D3D11 shared-texture present resources (readback bridge; no interop).
	if (c->has_shared_texture) {
		if (c->shared_present_fbo) {
			glDeleteFramebuffers(1, &c->shared_present_fbo);
		}
		if (c->shared_gl_texture) {
			glDeleteTextures(1, &c->shared_gl_texture);
		}
		if (c->shared_readback_cpu) {
			free(c->shared_readback_cpu);
			c->shared_readback_cpu = NULL;
			c->shared_readback_cap = 0;
		}
		if (c->dx_shared_texture) {
			c->dx_shared_texture->Release();
		}
		if (c->dx_context) {
			c->dx_context->Release();
		}
		if (c->dx_device) {
			c->dx_device->Release();
		}
	}

	// Tear down the transparent DComp present path (no-op if !dcomp_active).
	// Runs while the GL context is still current (it deletes GL tex/FBO).
	gl_destroy_dcomp_present(c);

	if (c->hglrc) {
		wglMakeCurrent(NULL, NULL);
		wglDeleteContext(c->hglrc);
	}
	if (c->owns_window && c->own_window != NULL) {
		comp_d3d11_window_destroy(&c->own_window);
	} else if (c->owns_window && c->hwnd) {
		DestroyWindow(c->hwnd);
	}
#elif defined(__APPLE__)
	if (c->iosurface_present_fbo) {
		glDeleteFramebuffers(1, &c->iosurface_present_fbo);
	}
	if (c->iosurface_gl_texture) {
		glDeleteTextures(1, &c->iosurface_gl_texture);
	}
	if (c->macos_window != NULL) {
		comp_gl_window_macos_destroy(&c->macos_window);
	}
#endif

	free(c);
}


/*
 *
 * Supported formats
 *
 */

static void
gl_compositor_set_formats(struct comp_gl_compositor *c)
{
	// GL format enum values
	c->base.base.info.format_count = 4;
	c->base.base.info.formats[0] = 0x8058; // GL_RGBA8
	c->base.base.info.formats[1] = 0x8C43; // GL_SRGB8_ALPHA8
	c->base.base.info.formats[2] = 0x881A; // GL_RGBA16F
	c->base.base.info.formats[3] = 0x8814; // GL_RGBA32F
}


/*
 *
 * Platform-specific window/context creation
 *
 */

#ifdef XRT_OS_WINDOWS

//! GLAD loader: try wglGetProcAddress first, fall back to GetProcAddress on opengl32.dll.
static GLADapiproc
gl_get_proc_addr(void *userptr, const char *name)
{
	GLADapiproc ret = (GLADapiproc)wglGetProcAddress(name);
	if (ret == NULL) {
		ret = (GLADapiproc)GetProcAddress((HMODULE)userptr, name);
	}
	return ret;
}

static const wchar_t GL_WINDOW_CLASS[] = L"DisplayXRGLCompositor";

static LRESULT CALLBACK
gl_window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_CLOSE:
		return 0; // Prevent close
	default:
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}
}

static bool
gl_create_window_and_context(struct comp_gl_compositor *c,
                              void *window_handle,
                              void *app_gl_context,
                              uint32_t width,
                              uint32_t height,
                              int32_t screen_left,
                              int32_t screen_top)
{
	// Register window class
	WNDCLASSEXW wc = {0};
	wc.cbSize = sizeof(wc);
	wc.style = CS_OWNDC;
	wc.lpfnWndProc = gl_window_proc;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.lpszClassName = GL_WINDOW_CLASS;
	RegisterClassExW(&wc);

	if (window_handle != NULL) {
		c->hwnd = (HWND)window_handle;
		c->owns_window = false;
	} else {
		// Use shared window module — borderless fullscreen on Leia display,
		// dedicated thread with message pump, QWERTY input support.
		uint32_t win_w = width > 0 ? width : GL_DEFAULT_WIDTH;
		uint32_t win_h = height > 0 ? height : GL_DEFAULT_HEIGHT;
		xrt_result_t xret = comp_d3d11_window_create(
		    win_w, win_h, screen_left, screen_top, &c->own_window);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to create self-owned window for GL compositor");
			return false;
		}
		c->hwnd = (HWND)comp_d3d11_window_get_hwnd(c->own_window);
		c->owns_window = true;
	}

	if (c->hwnd == NULL) {
		U_LOG_E("Failed to get window handle");
		return false;
	}

	c->hdc = GetDC(c->hwnd);

	// Set pixel format
	PIXELFORMATDESCRIPTOR pfd = {0};
	pfd.nSize = sizeof(pfd);
	pfd.nVersion = 1;
	pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pfd.iPixelType = PFD_TYPE_RGBA;
	pfd.cColorBits = 32;
	pfd.cDepthBits = 24;
	int pf = ChoosePixelFormat(c->hdc, &pfd);
	SetPixelFormat(c->hdc, pf, &pfd);

	// Create GL context (sharing with app context if provided)
	c->app_hglrc = (HGLRC)app_gl_context;
	c->hglrc = wglCreateContext(c->hdc);
	if (c->hglrc == NULL) {
		U_LOG_E("Failed to create WGL context");
		return false;
	}

	// Share texture namespace with app context
	if (c->app_hglrc != NULL) {
		if (!wglShareLists(c->app_hglrc, c->hglrc)) {
			U_LOG_E("wglShareLists failed: %lu", GetLastError());
			return false;
		}
	}

	wglMakeCurrent(c->hdc, c->hglrc);

	// Load GL and WGL function pointers via GLAD
	HMODULE opengl_dll = LoadLibraryW(L"opengl32.dll");
	if (opengl_dll == NULL) {
		U_LOG_E("Failed to load opengl32.dll");
		return false;
	}

	int wgl_result = gladLoadWGLUserPtr(c->hdc, gl_get_proc_addr, opengl_dll);
	int gl_result = gladLoadGLUserPtr(gl_get_proc_addr, opengl_dll);

	if (wgl_result == 0 || gl_result == 0) {
		U_LOG_E("Failed to load GLAD functions: WGL=%d, GL=%d", wgl_result, gl_result);
		FreeLibrary(opengl_dll);
		return false;
	}

	U_LOG_W("GLAD loaded: GL %d.%d, renderer: %s",
	         GLAD_VERSION_MAJOR(gl_result), GLAD_VERSION_MINOR(gl_result),
	         glGetString ? (const char *)glGetString(GL_RENDERER) : "unknown");

	// #1159: latch the GL context's adapter (when the driver reports it) and
	// state, once per session, that GL placement is OS-advisory (ADR-037 §5).
	// Must be here: the context is current and GLAD is loaded, and it runs for
	// EVERY GL session, including ones that create no interop device at all —
	// the limitation is a property of the session, not of the bridge.
	gl_log_placement_advisory_once(c, screen_left, screen_top);

	return true;
}
#endif // XRT_OS_WINDOWS


/*
 *
 * GL resource initialization
 *
 */

static bool
gl_init_resources(struct comp_gl_compositor *c, uint32_t width, uint32_t height)
{
	// Initialize tile layout from active rendering mode if available
	if (c->xdev != NULL && c->xdev->hmd != NULL) {
		uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
		if (idx < c->xdev->rendering_mode_count &&
		    c->xdev->rendering_modes[idx].tile_columns > 0) {
			c->tile_columns = c->xdev->rendering_modes[idx].tile_columns;
			c->tile_rows = c->xdev->rendering_modes[idx].tile_rows;
		}
	}
	// Default to 2x1 (stereo) if not set
	if (c->tile_columns == 0) {
		c->tile_columns = 2;
		c->tile_rows = 1;
	}

	c->view_width = width / c->tile_columns;
	c->view_height = height / c->tile_rows;

	// Compile shaders
	c->program_blit = create_program(VS_FULLSCREEN_QUAD, FS_BLIT);
	c->program_blit_array = create_program(VS_FULLSCREEN_QUAD, FS_BLIT_ARRAY);
	c->program_window_space = create_program(VS_WINDOW_SPACE, FS_TEXTURED);
	// #439 Phase 3 — masked 2D-over-3D composite (flatten reuses program_window_space).
	c->program_masked_composite = create_program(VS_FULLSCREEN_QUAD, FS_MASKED_COMPOSITE);

	if (!c->program_blit || !c->program_window_space || !c->program_masked_composite) {
		U_LOG_E("Failed to compile GL compositor shaders");
		return false;
	}

	// Empty VAO for vertex-shader-generated geometry
	glGenVertexArrays(1, &c->vao_empty);

	// FBO for offscreen rendering into atlas texture
	glGenFramebuffers(1, &c->fbo);

	// FBO for cropping atlas to content dims before DP
	glGenFramebuffers(1, &c->dp_crop_fbo);

	// Atlas texture — always worst-case sized across all rendering modes.
	// Per-frame content region may be smaller; compositor crops before DP.
	uint32_t atlas_width = c->tile_columns * c->view_width;
	uint32_t atlas_height = c->tile_rows * c->view_height;
	if (c->xdev != NULL && c->xdev->rendering_mode_count > 0) {
		u_tiling_compute_system_atlas(c->xdev->rendering_modes,
		                              c->xdev->rendering_mode_count,
		                              &atlas_width, &atlas_height);
	}
	c->atlas_tex_width = atlas_width;
	c->atlas_tex_height = atlas_height;
	glGenTextures(1, &c->atlas_texture);
	glBindTexture(GL_TEXTURE_2D, c->atlas_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
	             atlas_width, atlas_height, 0,
	             GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glBindTexture(GL_TEXTURE_2D, 0);

	c->hardware_display_3d = true;

	U_LOG_W("GL compositor resources initialized: %ux%u per eye, atlas %ux%u (%u cols x %u rows)",
	         c->view_width, c->view_height, atlas_width, atlas_height, c->tile_columns, c->tile_rows);

	return true;
}


/*
 *
 * Public API
 *
 */

void
comp_gl_compositor_set_system_devices(struct xrt_compositor *xc, struct xrt_system_devices *xsysd)
{
	struct comp_gl_compositor *c = gl_comp(xc);
	c->xsysd = xsysd;
#ifdef XRT_OS_WINDOWS
	if (c->own_window != NULL) {
		comp_d3d11_window_set_system_devices(c->own_window, xsysd);
	}
#endif
}

void
comp_gl_compositor_set_sys_info(struct xrt_compositor *xc, const struct xrt_system_compositor_info *info)
{
	struct comp_gl_compositor *c = gl_comp(xc);
	c->sys_info = *info;
	c->sys_info_set = true;
	c->legacy_app_tile_scaling = info->legacy_app_tile_scaling;
	c->last_3d_mode_index = 1;

	// Legacy apps: fix view dims at the actual recommended size the app was told to render at.
	if (info->legacy_app_tile_scaling &&
	    info->legacy_view_width_pixels > 0 && info->legacy_view_height_pixels > 0) {
		c->view_width = info->legacy_view_width_pixels;
		c->view_height = info->legacy_view_height_pixels;
	}
}

/*
 *
 * #439 Phase 3 — XR_DXR_local_3d_zone authored-mask API (GL leg).
 *
 * GL R8 mask textures authored in-process on the compositor's GL context,
 * frame-serialized with the composite. Tier 1 (set_whole), Tier 2 (set_rects);
 * Tier 3 (acquire_rt) is unimplemented on GL. All entry points serialize on the
 * compositor context being current — the oxr state tracker calls them on the
 * app thread, which shares the context; a real cross-thread/process GL mask
 * would need an explicit context make-current (out of scope for the in-process
 * handle-app consumer this leg targets).
 *
 */

// (Re)allocate the R8 mask texture + FBO at w×h.
static bool
gl_zone_mask_ensure(struct comp_gl_zone_mask *m, uint32_t w, uint32_t h)
{
	if (m->tex != 0 && m->w == w && m->h == h) {
		return true;
	}
	if (m->tex != 0) {
		glDeleteTextures(1, &m->tex);
		m->tex = 0;
	}
	if (m->fbo == 0) {
		glGenFramebuffers(1, &m->fbo);
	}
	glGenTextures(1, &m->tex);
	glBindTexture(GL_TEXTURE_2D, m->tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, m->fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m->tex, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	m->w = w;
	m->h = h;
	return true;
}

extern "C" xrt_result_t
comp_gl_compositor_zone_mask_create(struct xrt_compositor *xc, uint32_t w, uint32_t h, void **out_mask)
{
	struct comp_gl_compositor *c = gl_comp(xc);
	if (out_mask == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	// 0 → runtime picks the client-window dims (the mask is window-sized).
	if (w == 0 || h == 0) {
#ifdef XRT_OS_WINDOWS
		RECT r;
		if (c->hwnd != NULL && GetClientRect(c->hwnd, &r) && r.right > 0 && r.bottom > 0) {
			w = (uint32_t)r.right;
			h = (uint32_t)r.bottom;
		}
#endif
		if (w == 0 || h == 0) {
			w = c->tile_columns * c->view_width;
			h = c->tile_rows * c->view_height;
		}
	}
	if (w == 0 || h == 0) {
		return XRT_ERROR_ALLOCATION;
	}

	struct comp_gl_zone_mask *m = U_TYPED_CALLOC(struct comp_gl_zone_mask);
	if (m == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	if (!gl_zone_mask_ensure(m, w, h)) {
		free(m);
		return XRT_ERROR_ALLOCATION;
	}
	// Default to all-3D (M=1): an unauthored-but-submitted mask = full weave.
	glBindFramebuffer(GL_FRAMEBUFFER, m->fbo);
	glViewport(0, 0, w, h);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(1.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	U_LOG_W("GL zone_mask_create: %ux%u (client-window px)", w, h);
	*out_mask = m;
	return XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_gl_compositor_zone_mask_set_whole(struct xrt_compositor *xc, void *mask_ptr, bool enable_3d)
{
	(void)xc;
	struct comp_gl_zone_mask *m = (struct comp_gl_zone_mask *)mask_ptr;
	if (m == NULL || m->fbo == 0) {
		return XRT_ERROR_ALLOCATION;
	}
	glBindFramebuffer(GL_FRAMEBUFFER, m->fbo);
	glViewport(0, 0, m->w, m->h);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(enable_3d ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_gl_compositor_zone_mask_set_rects(struct xrt_compositor *xc,
                                       void *mask_ptr,
                                       uint32_t count,
                                       const struct xrt_rect *rects)
{
	(void)xc;
	struct comp_gl_zone_mask *m = (struct comp_gl_zone_mask *)mask_ptr;
	if (m == NULL || m->fbo == 0 || (count > 0 && rects == NULL)) {
		return XRT_ERROR_ALLOCATION;
	}
	// M=0 everywhere, then M=1 inside the surviving rects (3D islands). Flip Y
	// from window top-left to the bottom-left GL framebuffer.
	glBindFramebuffer(GL_FRAMEBUFFER, m->fbo);
	glViewport(0, 0, m->w, m->h);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glEnable(GL_SCISSOR_TEST);
	glClearColor(1.0f, 0.0f, 0.0f, 0.0f);
	for (uint32_t i = 0; i < count; i++) {
		int32_t left = rects[i].offset.w;
		int32_t top = rects[i].offset.h;
		int32_t rw = rects[i].extent.w;
		int32_t rh = rects[i].extent.h;
		if (rw <= 0 || rh <= 0) {
			continue;
		}
		if (left < 0) {
			rw += left;
			left = 0;
		}
		if (top < 0) {
			rh += top;
			top = 0;
		}
		if (left + rw > (int32_t)m->w) {
			rw = (int32_t)m->w - left;
		}
		if (top + rh > (int32_t)m->h) {
			rh = (int32_t)m->h - top;
		}
		if (rw <= 0 || rh <= 0) {
			continue;
		}
		int32_t gl_y = (int32_t)m->h - (top + rh);
		glScissor(left, gl_y, rw, rh);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_gl_compositor_zone_mask_acquire_rt(
    struct xrt_compositor *xc, void *mask_ptr, void **out_texture, uint32_t *out_w, uint32_t *out_h)
{
	(void)xc;
	(void)mask_ptr;
	(void)out_texture;
	(void)out_w;
	(void)out_h;
	// Tier 3 (app-authored RT) is unimplemented on the GL leg.
	return XRT_ERROR_NOT_IMPLEMENTED;
}

extern "C" xrt_result_t
comp_gl_compositor_zone_mask_submit(struct xrt_compositor *xc, void *mask_ptr)
{
	struct comp_gl_compositor *c = gl_comp(xc);
	struct comp_gl_zone_mask *m = (struct comp_gl_zone_mask *)mask_ptr;
	if (m == NULL || m->tex == 0) {
		return XRT_ERROR_ALLOCATION;
	}
	// Same-context authoring → the composite samples m->tex directly; no
	// staged copy needed. Sticky last-submit-wins.
	m->submitted = true;
	c->active_zone_mask = m;
	c->zone_publish_seq++; // #224 P4: new content generation for the DP publish
	return XRT_SUCCESS;
}

extern "C" void
comp_gl_compositor_zone_mask_destroy(struct xrt_compositor *xc, void *mask_ptr)
{
	struct comp_gl_compositor *c = gl_comp(xc);
	struct comp_gl_zone_mask *m = (struct comp_gl_zone_mask *)mask_ptr;
	if (m == NULL) {
		return;
	}
	if (c->active_zone_mask == m) {
		c->active_zone_mask = NULL;
	}
	// XR_DXR_display_zones: never leave a dangling frame-wish reference.
	if (c->frame_wish == m) {
		c->frame_wish = NULL;
	}
	// #224 P4: drop the seq-dedup cache (pointer may be reused by a future
	// alloc) and any per-frame publish source borrowed from this mask.
	if (c->zone_frame_wish_last == m) {
		c->zone_frame_wish_last = NULL;
	}
	if (c->zone_publish_tex == m->tex) {
		c->zone_publish_tex = 0;
	}
	if (m->tex != 0) {
		glDeleteTextures(1, &m->tex);
	}
	if (m->fbo != 0) {
		glDeleteFramebuffers(1, &m->fbo);
	}
	free(m);
}

extern "C" void
comp_gl_compositor_zones_set_frame_wish(struct xrt_compositor *xc, void *mask)
{
	struct comp_gl_compositor *c = gl_comp(xc);

	// Per-frame reference (XR_DXR_display_zones): oxr sets this on every
	// zones frame before layer_commit, NULL meaning auto-derive. Consumed
	// by the commit's composite; harmlessly stale on zero-zone frames (the
	// zones branch never reads it there).
	c->frame_wish = (struct comp_gl_zone_mask *)mask;
}

extern "C" bool
comp_gl_compositor_get_recommended_view_size(struct xrt_compositor *xc, uint32_t *out_w, uint32_t *out_h)
{
	struct comp_gl_compositor *c = gl_comp(xc);
	if (out_w == NULL || out_h == NULL || c->view_width == 0 || c->view_height == 0) {
		return false;
	}
	*out_w = c->view_width;
	*out_h = c->view_height;
	return true;
}

bool
comp_gl_compositor_request_display_mode(struct xrt_compositor *xc, bool enable_3d)
{
	if (xc == NULL) {
		return false;
	}

	struct comp_gl_compositor *c = gl_comp(xc);

	if (c->display_processor != NULL) {
		return xrt_display_processor_gl_request_display_mode(c->display_processor, enable_3d);
	}

	return false;
}

void
comp_gl_compositor_set_eye_tracking_mode(struct xrt_compositor *xc, uint32_t mode)
{
	if (xc == NULL) {
		return;
	}

	struct comp_gl_compositor *c = gl_comp(xc);

	if (c->display_processor != NULL) {
		xrt_display_processor_gl_set_eye_tracking_mode(c->display_processor, mode);
	}
}

bool
comp_gl_compositor_get_predicted_eye_positions(struct xrt_compositor *xc,
                                               struct xrt_eye_positions *out_eye_pos)
{
	if (xc == NULL || out_eye_pos == NULL) {
		return false;
	}

	struct comp_gl_compositor *c = gl_comp(xc);

	if (c->display_processor == NULL) {
		return false;
	}

	return xrt_display_processor_gl_get_predicted_eye_positions(
		c->display_processor, out_eye_pos);
}

bool
comp_gl_compositor_get_window_metrics(struct xrt_compositor *xc,
                                      struct xrt_window_metrics *out_metrics)
{
	if (xc == NULL || out_metrics == NULL) {
		return false;
	}

	struct comp_gl_compositor *c = gl_comp(xc);
	memset(out_metrics, 0, sizeof(*out_metrics));

#ifdef XRT_OS_WINDOWS
	if (!c->sys_info_set || c->hwnd == NULL) {
		return false;
	}

	float disp_w_m = c->sys_info.display_width_m;
	float disp_h_m = c->sys_info.display_height_m;
	uint32_t disp_px_w = c->sys_info.display_pixel_width;
	uint32_t disp_px_h = c->sys_info.display_pixel_height;
	if (disp_px_w == 0 || disp_px_h == 0 || disp_w_m <= 0 || disp_h_m <= 0) {
		return false;
	}

	RECT rect;
	if (!GetClientRect(c->hwnd, &rect)) {
		return false;
	}
	uint32_t win_px_w = (uint32_t)(rect.right - rect.left);
	uint32_t win_px_h = (uint32_t)(rect.bottom - rect.top);
	if (win_px_w == 0 || win_px_h == 0) {
		return false;
	}

	POINT client_origin = {0, 0};
	ClientToScreen(c->hwnd, &client_origin);

	float pixel_size_x = disp_w_m / (float)disp_px_w;
	float pixel_size_y = disp_h_m / (float)disp_px_h;

	out_metrics->display_width_m = disp_w_m;
	out_metrics->display_height_m = disp_h_m;
	out_metrics->display_pixel_width = disp_px_w;
	out_metrics->display_pixel_height = disp_px_h;
	out_metrics->display_screen_left = 0;
	out_metrics->display_screen_top = 0;

	out_metrics->window_pixel_width = win_px_w;
	out_metrics->window_pixel_height = win_px_h;
	out_metrics->window_screen_left = (int32_t)client_origin.x;
	out_metrics->window_screen_top = (int32_t)client_origin.y;

	out_metrics->window_width_m = (float)win_px_w * pixel_size_x;
	out_metrics->window_height_m = (float)win_px_h * pixel_size_y;

	float win_center_px_x = (float)(client_origin.x) + (float)win_px_w / 2.0f;
	float win_center_px_y = (float)(client_origin.y) + (float)win_px_h / 2.0f;
	float disp_center_px_x = (float)disp_px_w / 2.0f;
	float disp_center_px_y = (float)disp_px_h / 2.0f;

	out_metrics->window_center_offset_x_m = (win_center_px_x - disp_center_px_x) * pixel_size_x;
	out_metrics->window_center_offset_y_m = -((win_center_px_y - disp_center_px_y) * pixel_size_y);

	out_metrics->valid = true;
	return true;
#elif defined(__APPLE__)
	if (!c->sys_info_set || c->macos_window == NULL) {
		return false;
	}

	float disp_w_m = c->sys_info.display_width_m;
	float disp_h_m = c->sys_info.display_height_m;
	uint32_t disp_px_w = c->sys_info.display_pixel_width;
	uint32_t disp_px_h = c->sys_info.display_pixel_height;
	if (disp_px_w == 0 || disp_px_h == 0 || disp_w_m <= 0 || disp_h_m <= 0) {
		return false;
	}

	uint32_t win_px_w = 0, win_px_h = 0;
	comp_gl_window_macos_get_dimensions(c->macos_window, &win_px_w, &win_px_h);
	if (win_px_w == 0 || win_px_h == 0) {
		return false;
	}

	float pixel_size_x = disp_w_m / (float)disp_px_w;
	float pixel_size_y = disp_h_m / (float)disp_px_h;

	out_metrics->display_width_m = disp_w_m;
	out_metrics->display_height_m = disp_h_m;
	out_metrics->display_pixel_width = disp_px_w;
	out_metrics->display_pixel_height = disp_px_h;
	out_metrics->display_screen_left = 0;
	out_metrics->display_screen_top = 0;

	out_metrics->window_pixel_width = win_px_w;
	out_metrics->window_pixel_height = win_px_h;

	out_metrics->window_width_m = (float)win_px_w * pixel_size_x;
	out_metrics->window_height_m = (float)win_px_h * pixel_size_y;

	// Window centre offset within the display: real on-screen position when
	// available (so window-relative 3D tracks window moves), else centred.
	// Mirrors the VK native / Metal macOS paths (#524).
	float disp_center_px_x = (float)disp_px_w / 2.0f;
	float disp_center_px_y = (float)disp_px_h / 2.0f;
	float win_center_px_x = disp_center_px_x;
	float win_center_px_y = disp_center_px_y;
	int32_t win_left = 0, win_top = 0;
	if (comp_gl_window_macos_get_screen_position(c->macos_window, &win_left, &win_top)) {
		win_center_px_x = (float)win_left + (float)win_px_w / 2.0f;
		win_center_px_y = (float)win_top + (float)win_px_h / 2.0f;
	}
	out_metrics->window_screen_left = win_left;
	out_metrics->window_screen_top = win_top;

	out_metrics->window_center_offset_x_m = (win_center_px_x - disp_center_px_x) * pixel_size_x;
	out_metrics->window_center_offset_y_m = -((win_center_px_y - disp_center_px_y) * pixel_size_y);

	out_metrics->valid = true;
	return true;
#else
	(void)c;
	return false;
#endif
}

xrt_result_t
comp_gl_compositor_create(struct xrt_device *xdev,
                          void *window_handle,
                          void *gl_context,
                          void *gl_display,
                          void *dp_factory_gl,
                          void *shared_texture_handle,
                          bool transparent_background,
                          int32_t display_screen_left,
                          int32_t display_screen_top,
                          struct xrt_compositor_native **out_xcn)
{
	struct comp_gl_compositor *c = U_TYPED_CALLOC(struct comp_gl_compositor);
	c->xdev = xdev;
	c->transparent_background = transparent_background;

	// #868: before ANY path that can reach gl_compositor_destroy — it joins the
	// repaint thread and destroys this mutex, so both must be valid even on the
	// earliest create failure.
	os_mutex_init(&c->mutex);
	os_thread_helper_init(&c->repaint_thread);

	mcp_capture_init(&c->mcp_capture);
	mcp_capture_install(&c->mcp_capture);

	// Get window dimensions
	uint32_t width = GL_DEFAULT_WIDTH;
	uint32_t height = GL_DEFAULT_HEIGHT;

	if (xdev != NULL && xdev->hmd != NULL &&
	    xdev->hmd->screens[0].w_pixels > 0) {
		width = xdev->hmd->screens[0].w_pixels;
		height = xdev->hmd->screens[0].h_pixels;
	}

	// Save caller's GL context so we can restore after init
#ifdef XRT_OS_WINDOWS
	HDC caller_hdc = wglGetCurrentDC();
	HGLRC caller_hglrc = wglGetCurrentContext();
#elif defined(__APPLE__)
	CGLContextObj caller_cgl_ctx = CGLGetCurrentContext();
#endif

	// Platform-specific context/window setup
#ifdef XRT_OS_WINDOWS
	if (!gl_create_window_and_context(c, window_handle, gl_context, width, height,
	                                  display_screen_left, display_screen_top)) {
		free(c);
		return XRT_ERROR_OPENGL;
	}

	/*
	 * #918 Phase 3 / ADR-037 §3 — THE OPENGL ANSWER, STATED.
	 *
	 * There is no output-device split for OpenGL, and per ADR-037 §5 there
	 * cannot be one on the runtime's terms: OpenGL exposes **no device-selection
	 * API**, so the runtime cannot ask WGL for a different adapter — the per-exe
	 * `UserGpuPreferences` entry is the only lever, and it is the OS's, advisory.
	 *
	 * It CAN, however, usually learn which adapter WGL picked, which is what
	 * #1159 corrected: `GL_EXT_memory_object_win32` has the driver report its own
	 * D3D LUID via `glGetUnsignedBytevEXT(GL_DEVICE_LUID_EXT)`. So `render=` is
	 * the real GL adapter whenever the driver answers, and only falls back to
	 * `UNKNOWN` when it does not — an admitted unknown, never a guess. (This is
	 * why the call moved below gl_create_window_and_context: the query needs a
	 * current context and a loaded GLAD.)
	 *
	 * Either way the split stays disengaged — knowing the adapter is not being
	 * able to choose one — so this path takes rung 2 unconditionally and says so
	 * with the same one line every other compositor emits. There is no
	 * half-engaged state to reach: nothing in this compositor consults a scanout
	 * adapter or creates a second device for the weave.
	 */
	{
		const uint32_t pw = (xdev != NULL && xdev->hmd != NULL) ? xdev->hmd->screens[0].w_pixels : 0;
		const uint32_t ph = (xdev != NULL && xdev->hmd != NULL) ? xdev->hmd->screens[0].h_pixels : 0;
		uint64_t gl_packed_luid = 0;
		if (c->gl_context_luid_valid) {
			memcpy(&gl_packed_luid, &c->gl_context_luid, sizeof(gl_packed_luid));
		}
		d3d_log_weave_placement(gl_packed_luid, display_screen_left, display_screen_top, pw, ph,
		                        /* split_active */ false, COMP_SPLIT_REASON_API_UNSUPPORTED);
	}

	// Set up the GL→D3D shared-texture present path if a handle was provided.
	// NOTE: this does NOT use WGL_NV_DX_interop2 to weave directly into the
	// shared surface. GL interop write-BACK into the app's shared D3D texture
	// is unreliable on this stack (the GL render target fills, but the bytes
	// never reach the D3D resource — owned or opened, RGBA or BGRA). Instead we
	// weave into a plain GL render texture and bridge the result into the
	// shared surface with glReadPixels → UpdateSubresource (gl_shared_readback_upload),
	// mirroring the runtime's no-interop DComp readback present path.
	if (shared_texture_handle != NULL) {
		// D3D11 device used only to open + upload the app's shared texture, on
		// a deliberately resolved adapter rather than DXGI's default (#1159).
		// Opening the shared resource is the placement oracle — see the comment
		// on gl_create_shared_texture_device().
		if (!gl_create_shared_texture_device(c, shared_texture_handle)) {
			U_LOG_E(
			    "Failed to create a D3D11 device that can open the GL shared texture on ANY "
			    "adapter");
			free(c);
			return XRT_ERROR_OPENGL;
		}

		// Get shared texture dimensions
		D3D11_TEXTURE2D_DESC desc;
		c->dx_shared_texture->GetDesc(&desc);
		c->shared_width = desc.Width;
		c->shared_height = desc.Height;

		// Plain GL render texture the runtime weaves into (no interop). The
		// woven region is read back and uploaded to dx_shared_texture each frame.
		glGenTextures(1, &c->shared_gl_texture);
		glBindTexture(GL_TEXTURE_2D, c->shared_gl_texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)c->shared_width,
		             (GLsizei)c->shared_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glBindTexture(GL_TEXTURE_2D, 0);

		// Dedicated present FBO (see shared_present_fbo): NOT c->fbo, which the
		// DP crop reuses and would clobber.
		glGenFramebuffers(1, &c->shared_present_fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, c->shared_present_fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
		                        c->shared_gl_texture, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		c->has_shared_texture = true;
		U_LOG_W("GL shared-texture present: %ux%u (glReadPixels readback bridge)",
		         c->shared_width, c->shared_height);
	}
#elif defined(__APPLE__)
	// macOS: create window/context via NSOpenGLView helper
	if (shared_texture_handle != NULL) {
		// Shared IOSurface mode: offscreen GL context, render into IOSurface.
		// Pass the app's real on-screen view (window_handle) so get_dimensions
		// reports the app's window backing — the zones weave output must match
		// the window-space zone layout, not the worst-case IOSurface (mirrors the
		// Metal compositor, which measures the app's bound view).
		xrt_result_t xret = comp_gl_window_macos_create_offscreen(
		    gl_context, window_handle, &c->macos_window);
		if (xret != XRT_SUCCESS) {
			free(c);
			return XRT_ERROR_OPENGL;
		}
		c->owns_window = false;
		comp_gl_window_macos_make_current(c->macos_window);

		// Map the IOSurface to a GL texture
		GLuint io_tex = 0;
		uint32_t io_w = 0, io_h = 0;
		xret = comp_gl_window_macos_map_iosurface(
		    c->macos_window, shared_texture_handle, &io_tex, &io_w, &io_h);
		if (xret != XRT_SUCCESS) {
			free(c);
			return XRT_ERROR_OPENGL;
		}
		c->iosurface_gl_texture = io_tex;
		c->iosurface_width = io_w;
		c->iosurface_height = io_h;

		// Dedicated present FBO (see iosurface_present_fbo): NOT c->fbo, which
		// the DP crop reuses and would clobber. The IOSurface is a
		// GL_TEXTURE_RECTANGLE.
		glGenFramebuffers(1, &c->iosurface_present_fbo);
		glBindFramebuffer(GL_FRAMEBUFFER, c->iosurface_present_fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                        GL_TEXTURE_RECTANGLE, c->iosurface_gl_texture, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		c->has_shared_iosurface = true;
	} else if (window_handle != NULL) {
		// App provided an NSView — set up external
		xrt_result_t xret = comp_gl_window_macos_setup_external(
		    window_handle, gl_context, &c->macos_window);
		if (xret != XRT_SUCCESS) {
			free(c);
			return XRT_ERROR_OPENGL;
		}
		c->owns_window = false;
		comp_gl_window_macos_make_current(c->macos_window);
	} else {
		// Create our own window
		xrt_result_t xret = comp_gl_window_macos_create(
		    width, height, gl_context, &c->macos_window);
		if (xret != XRT_SUCCESS) {
			free(c);
			return XRT_ERROR_OPENGL;
		}
		c->owns_window = true;
		comp_gl_window_macos_make_current(c->macos_window);
	}
	(void)gl_display;
#else
	(void)shared_texture_handle;
#endif

	// Scale to Retina physical pixels on macOS.
	// width/height are logical points from screens[0]; the atlas texture
	// and rendering resources must match the actual backing resolution.
#ifdef __APPLE__
	{
		float backing_scale = comp_gl_window_macos_get_backing_scale();
		width = (uint32_t)(width * backing_scale);
		height = (uint32_t)(height * backing_scale);
	}
#endif

	// Create display processor via factory.
	// For hosted apps (no external window), use the compositor's own window
	// so the SR SDK GL weaver gets a valid HWND.
	if (dp_factory_gl != NULL) {
		void *dp_window = window_handle;
#ifdef XRT_OS_WINDOWS
		if (dp_window == NULL && c->hwnd != NULL) {
			dp_window = (void *)c->hwnd;
		}
#endif
		xrt_dp_factory_gl_fn_t factory = (xrt_dp_factory_gl_fn_t)dp_factory_gl;
		xrt_result_t dp_ret = factory(dp_window, &c->display_processor);
		if (dp_ret == XRT_SUCCESS && c->display_processor != NULL) {
			U_LOG_W("GL compositor: display processor created via factory");
			// Weave scope, once — see the D3D11 leg. GL output is a window.
			(void)u_weave_scope_report(xrt_display_processor_gl_get_weave_scope(c->display_processor),
			                           "GL", /* panel_scoped */ false);
			// Forward session-level transparency (#573 — chroma-key-free;
			// mirrors the D3D11/D3D12/VK legs). client_presents=false: the DP
			// owns see-through (compose-under-bg from the atlas alpha). The GL
			// compositor's own DComp transparent present (when interop/readback
			// is up) is independent and blends the live desktop into the fully
			// transparent border. No-op on DPs without the slot (e.g.
			// sim_display, which preserves alpha natively).
			xrt_display_processor_gl_set_transparent_background(
			    c->display_processor, c->transparent_background, false);

			// #68: tell the DP whether the app self-presents only the canvas
			// (texture app) vs the runtime presenting the full target (handle).
			// has_shared_texture is Windows-only (WGL_NV_DX_interop2); on other
			// platforms GL has no shared-texture present → always false.
			bool gl_shared_texture_present = false;
#ifdef XRT_OS_WINDOWS
			gl_shared_texture_present = c->has_shared_texture;
#endif
			xrt_display_processor_gl_set_shared_texture_present(
			    c->display_processor, gl_shared_texture_present);
		} else {
			U_LOG_W("GL compositor: display processor factory returned %d, using built-in shaders", dp_ret);
			c->display_processor = NULL;
		}
	}

	// Transparent-background present path (Windows). Gated on an app-provided
	// HWND (the app must carry WS_EX_NOREDIRECTIONBITMAP); runtime-hosted GL
	// windows are out of scope for now. Any setup failure leaves dcomp_active
	// false and the compositor falls back to the opaque SwapBuffers path.
	// See gl_setup_dcomp_present() for the architecture.
#ifdef XRT_OS_WINDOWS
	if (c->transparent_background && c->hwnd != NULL && !c->owns_window) {
		uint32_t tw = c->tile_columns * c->view_width;
		uint32_t th = c->tile_rows * c->view_height;
		RECT rc;
		if (GetClientRect(c->hwnd, &rc)) {
			uint32_t ww = (uint32_t)(rc.right - rc.left);
			uint32_t wh = (uint32_t)(rc.bottom - rc.top);
			if (ww > 0 && wh > 0) {
				tw = ww;
				th = wh;
			}
		}
		if (!gl_setup_dcomp_present(c, c->hwnd, tw, th)) {
			c->dcomp_active = false; // stay opaque
		}
	}
#else
	(void)transparent_background;
#endif

	// Initialize GL resources (atlas worst-case sized, crop before DP per-frame)
	if (!gl_init_resources(c, width, height)) {
		free(c);
		return XRT_ERROR_OPENGL;
	}

	// Create HUD overlay for runtime-owned windows
	if (c->owns_window) {
		u_hud_create(&c->hud, width);
	}

	// Set up compositor interface
	struct xrt_compositor *xc = &c->base.base;
	xc->get_swapchain_create_properties = gl_compositor_get_swapchain_create_properties;
	xc->create_swapchain = gl_compositor_create_swapchain;
	xc->begin_session = gl_compositor_begin_session;
	xc->end_session = gl_compositor_end_session;
	xc->predict_frame = gl_compositor_predict_frame;
	xc->mark_frame = gl_compositor_mark_frame;
	xc->wait_frame = gl_compositor_wait_frame;
	xc->begin_frame = gl_compositor_begin_frame;
	xc->discard_frame = gl_compositor_discard_frame;
	xc->layer_begin = gl_compositor_layer_begin;
	xc->layer_projection = gl_compositor_layer_projection;
	xc->layer_projection_depth = gl_compositor_layer_projection_depth;
	xc->layer_quad = gl_compositor_layer_quad;
	xc->layer_window_space = gl_compositor_layer_window_space;
	xc->layer_local_2d = gl_compositor_layer_local_2d;
	xc->layer_zone_3d = gl_compositor_layer_zone_3d;
	xc->layer_commit = gl_compositor_layer_commit;
	xc->destroy = gl_compositor_destroy;

	// Set formats
	gl_compositor_set_formats(c);

	// Visibility/focus flags for state transitions
	xc->info.initial_visible = true;
	xc->info.initial_focused = true;

	*out_xcn = &c->base;

	// Restore caller's GL context (don't leave compositor's context current)
#ifdef XRT_OS_WINDOWS
	if (caller_hglrc != NULL) {
		wglMakeCurrent(caller_hdc, caller_hglrc);
	} else {
		wglMakeCurrent(NULL, NULL);
	}
	// Store app's context for restore in layer_commit
	c->app_hdc = (HDC)gl_display;
#elif defined(__APPLE__)
	if (caller_cgl_ctx != NULL) {
		CGLSetCurrentContext(caller_cgl_ctx);
	}
#endif

	/*
	 * #868: the repaint loop, ON by default (DXR_WEAVE_REPAINT=0 disables),
	 * matching D3D11/D3D12/VK.
	 *
	 * Started HERE, after the compositor's context has been released back to
	 * the caller just above — the loop makes c->hglrc current on its own
	 * thread, and a WGL context cannot be current on two threads at once.
	 */
	{
		/*
		 * Default ON (DXR_WEAVE_REPAINT=0 disables), matching D3D11/D3D12/VK.
		 *
		 * GL was opt-in while the #885 black band/zone stood unexplained. Root
		 * cause found + fixed (2026-08-08): swapchain creation's context claim
		 * raced this very repaint thread — see gl_compositor_create_swapchain.
		 * With the race closed, 8/8 automated launch verifications + hardware
		 * eyeball pass; the old eliminated-suspects list lives in #885.
		 */
		// #1252: settings chain (env > per-user > machine) — the Control
		// Panel's Compatibility mode turns this off. Same parse as before.
		char rp_buf[64];
		const char *e = u_setting_get_raw("DXR_WEAVE_REPAINT", rp_buf, sizeof(rp_buf), NULL);
		c->repaint.enabled = (e != NULL && e[0] == '0') ? 0 : 1;
		const char *fe = getenv("DXR_WEAVE_REPAINT_FORCE");
		c->repaint.force = (fe != NULL && fe[0] == '1') ? 1 : 0;
		if (c->repaint.force == 1) {
			U_LOG_W("#868: DXR_WEAVE_REPAINT_FORCE=1 — repainting every refresh regardless "
			        "of app rate. Correctness probe; it WILL cost frame rate.");
		}
		if (c->repaint.enabled == 1) {
			os_thread_helper_start(&c->repaint_thread, gl_repaint_thread, c);
		}
	}

	U_LOG_W("Native OpenGL compositor created: %ux%u", width, height);

	return XRT_SUCCESS;
}
