// Copyright 2025, Leia Inc.
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

#include "xrt/xrt_device.h"
#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_display_processor_gl.h"
#include "xrt/xrt_handles.h"
#include "xrt/xrt_config_build.h"
#include "xrt/xrt_config_os.h"
#include "xrt/xrt_limits.h"
#include "xrt/xrt_system.h"

#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_tiling.h"
#include "util/u_canvas.h"
#include "util/u_capture_intent.h"
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
#include <d3dcompiler.h> // D3DCompile (inline blit shader for the DComp present path)
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

//! Fragment shader: masked 2D-over-3D composite (#439 Phase 3 GL leg).
//! Hard-mask path: final = M*weave + (1-M)*twod (explicit authored mask).
//! #491 alpha-over path (u_alpha_over): final = twod + (1-twod.a)*weave —
//! premultiplied "over" of the 2D atop the weave, so translucent 2D reveals the
//! 3D scene (used for the IMPLICIT Local2D mask). All three sources are
//! window-resolution textures in the same GL framebuffer orientation, 1:1.
static const char *FS_MASKED_COMPOSITE =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D u_twod;\n"
    "uniform sampler2D u_mask;\n"
    "uniform sampler2D u_weave;\n"
    "uniform int u_alpha_over;\n"
    "void main() {\n"
    "    vec4 twod  = texture(u_twod,  v_uv);\n"
    "    vec4 weave = texture(u_weave, v_uv);\n"
    "    if (u_alpha_over != 0) {\n"
    "        fragColor = twod + (1.0 - twod.a) * weave;\n"
    "        return;\n"
    "    }\n"
    "    float M = clamp(texture(u_mask, v_uv).r, 0.0, 1.0);\n"
    "    fragColor = M * weave + (1.0 - M) * twod;\n"
    "}\n";

/*
 *
 * GL compositor structure
 *
 */

// #439 Phase 3 — authored zone mask (XR_EXT_local_3d_zone), GL R8 texture.
// In-process handle apps author it on the same GL context the composite
// samples from, frame-serialized, so a single texture (no staged copy) is
// coherent. Tier-3 acquire_rt is unimplemented on GL (chroma-key-only DP).
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

	//! Canvas output rect for shared-texture apps.
	struct u_canvas_rect canvas;

	//! 2D surround texture handle (Spec v6).
	struct u_surround_2d_handle surround_2d;

	// --- #439 Phase 3 — Local2D / zone-mask consumer (full net-new GL leg) ---
	//! Active authored zone mask (XR_EXT_local_3d_zone). Set by zone_mask_submit
	//! (sticky, last-submit-wins), cleared on that mask's destroy. NOT owned.
	struct comp_gl_zone_mask *active_zone_mask;
	//! True if this frame's accumulator carried any XRT_LAYER_LOCAL_2D layer
	//! (set once at the top of layer_commit). Drives the effective-canvas
	//! supersede + the composite's have_local_2d branch.
	bool local_2d_last_frame;
	//! XR_EXT_display_zones (ADR-027): true when the current frame's
	//! accumulator carries XRT_LAYER_ZONE_3D layers (a "zones frame"). In a
	//! zones frame the canvas output rect, the sticky submitted mask, and
	//! the implicit-mask-from-Local2D rule are all inert; the effective
	//! canvas is the full client window; the wish drives the post-weave
	//! visual lerp. Set in the same per-frame scan as local_2d_last_frame.
	bool zones_frame;
	//! Explicit per-frame wish (XrDisplayZonesFrameEndInfoEXT.wishMask) set
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
#endif
};

static inline struct comp_gl_compositor *
gl_comp(struct xrt_compositor *xc)
{
	return (struct comp_gl_compositor *)xc;
}


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
	// 1. Dedicated D3D11 device for the present bridge.
	HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
	                               D3D11_SDK_VERSION, &c->dcomp_dx_device, NULL,
	                               &c->dcomp_dx_context);
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

	// Ensure compositor's GL context is current for texture creation
#ifdef XRT_OS_WINDOWS
	HDC prev_hdc = wglGetCurrentDC();
	HGLRC prev_hglrc = wglGetCurrentContext();
	wglMakeCurrent(c->hdc, c->hglrc);
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

	// Create GL textures
	GLenum internal_format = xrt_format_to_gl_internal(info->format);
	glGenTextures(image_count, sc->textures);

	for (uint32_t i = 0; i < image_count; i++) {
		glBindTexture(GL_TEXTURE_2D, sc->textures[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, internal_format,
		             info->width, info->height, 0,
		             GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

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
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SRGB_DECODE_EXT, GL_SKIP_DECODE_EXT);
		}

		// Store GL texture name in the swapchain_gl images array
		// (this is what the state tracker reads via xrt_swapchain_gl)
		sc->base.images[i] = sc->textures[i];
	}
	glBindTexture(GL_TEXTURE_2D, 0);

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
			U_LOG_I("Window closed - signaling session exit");
			return XRT_ERROR_IPC_FAILURE;
		}
	}
#endif

	int64_t now_ns = (int64_t)os_monotonic_get_ns();
	int64_t period_ns = (int64_t)(1000000000.0 / 60.0); // 60 Hz

	static int64_t frame_id = 0;
	*out_frame_id = ++frame_id;
	*out_wake_time_ns = now_ns;
	*out_predicted_gpu_time_ns = now_ns + period_ns / 2;
	*out_predicted_display_time_ns = now_ns + period_ns;
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
 * Local-2D layer (XR_EXT_local_3d_zone v3, #439 Phase 3) — accumulate only;
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
 * 3D display zone layer (XR_EXT_display_zones, ADR-027) — multi-swapchain
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

	// Pass (possibly cropped) texture to DP.
	// XR_EXT_display_zones: in a zones frame the output rect is inert —
	// pass no canvas (0s) so the DP weaves the full client window.
	const bool use_canvas = c->canvas.valid && !c->zones_frame;
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
	    use_canvas ? c->canvas.x : 0,
	    use_canvas ? c->canvas.y : 0,
	    use_canvas ? c->canvas.w : 0,
	    use_canvas ? c->canvas.h : 0);
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

// XR_EXT_display_zones (ADR-027) — (re)rasterize the AUTO wish: union of the
// frame's zone rects with an INWARD stepped ring feather. M=0 outside the
// zones; inside each zone M ramps 0->1 over the first 16 px from the edge
// (ascending-value insets — max semantics: overlapping feathers can never
// dim a core; small zones clamp the inset so the center still reaches 1).
// Feathering inward keeps the visual lerp fading zone content toward
// TRANSPARENT at the edge (never toward the weave of empty atlas, which DPs
// may report opaque black).
// Reuses the implicit-mask R8 texture (the implicit rule is inert in zones
// frames) and re-rasters every zones frame, VK-style — a handful of scissored
// clears — while invalidating the implicit rect cache so a later legacy frame
// re-rasters. Rects are window px (top-left); flip Y for the bottom-left GL
// framebuffer. Returns the mask texture, or 0 on failure.
#define ZONE_WISH_FEATHER_STEPS 8
#define ZONE_WISH_FEATHER_STEP_PX 2
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
	for (int32_t s = 1; s <= ZONE_WISH_FEATHER_STEPS; s++) {
		const float v = (float)s / (float)ZONE_WISH_FEATHER_STEPS;
		glClearColor(v, 0.0f, 0.0f, 0.0f);
		for (uint32_t i = 0; i < rect_count; i++) {
			// Small zones clamp the inset so the center still reaches 1.
			int32_t min_ext = rects[i].extent.w < rects[i].extent.h ? rects[i].extent.w
			                                                        : rects[i].extent.h;
			int32_t max_inset = (min_ext - 1) / 2;
			if (max_inset < 0) {
				max_inset = 0;
			}
			int32_t inset = s * ZONE_WISH_FEATHER_STEP_PX;
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
			// Flip Y: window top-left → GL bottom-left framebuffer.
			int32_t gl_y = (int32_t)h - bottom;
			glScissor(left, gl_y, right - left, bottom - top);
			glClear(GL_COLOR_BUFFER_BIT);
		}
	}
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	static bool wish_logged = false;
	if (!wish_logged) {
		wish_logged = true;
		U_LOG_W("GL zone wish mask (auto): %ux%u, %u zone rect(s), %u-px feather", w, h, rect_count,
		        ZONE_WISH_FEATHER_STEPS * ZONE_WISH_FEATHER_STEP_PX);
	}
	return c->implicit_mask_tex;
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
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		dp_tex = c->dp_input_texture;
	}

	// XR_EXT_display_zones: in a zones frame the output rect is inert —
	// pass no canvas (0s) so the DP weaves the full client window.
	const bool use_canvas = c->canvas.valid && !c->zones_frame;
	glBindFramebuffer(GL_FRAMEBUFFER, target_fbo);
	glViewport(0, 0, output_w, output_h);
	xrt_display_processor_gl_process_atlas(c->display_processor, dp_tex, eff_tile_w, eff_tile_h,
	                                       eff_cols, eff_rows, GL_RGBA8, output_w, output_h,
	                                       use_canvas ? c->canvas.x : 0, use_canvas ? c->canvas.y : 0,
	                                       use_canvas ? c->canvas.w : 0, use_canvas ? c->canvas.h : 0);
}

// #439 Phase 3 — the post-weave masked composite. Runs INSTEAD of the plain
// gl_crop_and_process_dp present when an explicit submitted mask or Local2D
// layers are present. Flattens the 2D, resolves the mask (explicit tex or
// implicit raster), redirects the DP weave into weave_tex, then lerps
// M*weave + (1-M)*twod into target_fbo (the window). Returns false → caller
// falls through to the plain present.
static bool
gl_composite_local_2d(struct comp_gl_compositor *c, GLuint atlas_tex, GLuint target_fbo, uint32_t output_w,
                      uint32_t output_h)
{
	// XR_EXT_display_zones: a zones frame ALWAYS runs the composite (the
	// feathered wish edge lerps the weave toward the 2D flatten even with
	// zero Local2D layers); the sticky mask + implicit-mask rules are inert.
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

	// Resolve the mask texture. Zones frame (XR_EXT_display_zones): the
	// WISH — the explicit frame wish (same-context GL authoring texture,
	// referenced-at-frame-end = consume current authored state, no submit
	// required — mirroring zone_mask_submit's no-staging contract) or the
	// auto ring-feathered raster from the zone rects. Legacy: explicit
	// submitted mask wins; else implicit.
	GLuint mask_tex = 0;
	if (zones_frame) {
		if (c->frame_wish != NULL && c->frame_wish->tex != 0) {
			mask_tex = c->frame_wish->tex;

			// P4 publish source + seq: the explicit wish (the live
			// authoring texture — same-context, matching the GL
			// no-staging contract). Bump the generation on a source
			// change (pointer flip; GL masks carry no author
			// generation, so a same-pointer re-author keeps its seq).
			c->zone_publish_tex = mask_tex;
			c->zone_publish_w = c->frame_wish->w;
			c->zone_publish_h = c->frame_wish->h;
			if (c->zone_frame_wish_last != c->frame_wish) {
				c->zone_frame_wish_last = c->frame_wish;
				c->zone_publish_seq++;
			}
		} else {
			struct xrt_rect zone_rects[XRT_MAX_LAYERS];
			uint32_t zone_rect_count = 0;
			for (uint32_t i = 0; i < c->layer_accum.layer_count && zone_rect_count < XRT_MAX_LAYERS;
			     i++) {
				if (c->layer_accum.layers[i].data.type != XRT_LAYER_ZONE_3D) {
					continue;
				}
				zone_rects[zone_rect_count++] = c->layer_accum.layers[i].data.zone_3d.rect;
			}
			mask_tex = gl_update_zone_wish_mask(c, zone_rects, zone_rect_count, output_w, output_h);
			if (mask_tex != 0) {
				// P4 publish source + seq: the auto raster — bump the
				// generation only when the rect set / dims actually
				// changed (or the source flipped explicit -> auto).
				bool wish_dirty = c->zone_frame_wish_last != NULL ||
				                  c->zone_wish_rect_count != zone_rect_count ||
				                  c->zone_publish_w != output_w || c->zone_publish_h != output_h;
				for (uint32_t i = 0; !wish_dirty && i < zone_rect_count; i++) {
					if (memcmp(&c->zone_wish_rects[i], &zone_rects[i], sizeof(zone_rects[i])) !=
					    0) {
						wish_dirty = true;
					}
				}
				if (wish_dirty) {
					c->zone_frame_wish_last = NULL;
					memcpy(c->zone_wish_rects, zone_rects,
					       sizeof(zone_rects[0]) * zone_rect_count);
					c->zone_wish_rect_count = zone_rect_count;
					c->zone_publish_seq++;
				}
				c->zone_publish_tex = mask_tex;
				c->zone_publish_w = output_w;
				c->zone_publish_h = output_h;
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

	// Flatten the Local2D layers (the twod source). With no Local2D layers (a
	// pure explicit-mask frame) the scratch stays transparent → the masked
	// region shows the desktop, matching the VK leg.
	// Zones frame: flatten ALL Local2D layers (no under/over split — 2D-under
	// is reserved in v1); with zero Local2D layers the clear-only scratch
	// fades the feather to transparent.
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
	{
		uint32_t bd_w = 0, bd_h = 0;
		GLuint bd_tex = gl_flatten_backdrop_2d(c, output_w, output_h, &bd_w, &bd_h);
		xrt_display_processor_gl_set_background_2d(c->display_processor, bd_tex, bd_w, bd_h);
	}

	// Redirect the DP weave into weave_tex.
	gl_dp_weave_to_fbo(c, atlas_tex, c->weave_fbo, output_w, output_h);

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
	// XR_EXT_display_zones: zones frames are ALWAYS the hard M-lerp
	// (final = M·weave + (1−M)·flatten(2D-over)) — composition follows zone
	// geometry + the wish, never the #491 alpha-over rule.
	const bool alpha_over = !zones_frame && have_local_2d && !have_explicit;
	glUniform1i(glGetUniformLocation(c->program_masked_composite, "u_alpha_over"), alpha_over ? 1 : 0);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glActiveTexture(GL_TEXTURE0);

	static bool composite_logged = false;
	if (!composite_logged) {
		U_LOG_W("GL Local2D composite: %ux%u region, %s mask, twod=%s (#491 alpha_over=%d)", output_w,
		        output_h, zones_frame ? "zone wish" : (have_explicit ? "explicit" : "implicit"),
		        have_local_2d ? "local2d layers" : "(empty)", alpha_over);
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
// (xrRequestDisplayModeEXT), under which this layout keeps following the
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

static xrt_result_t
gl_compositor_layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
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
	// XR_EXT_display_zones: the zones-frame flag is resolved in the same
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

	// XR_EXT_display_zones hardware leg (P4). Zone-capable DP: the per-frame
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
				// XR_EXT_display_zones: a zones frame spans the full
				// client window (the output rect is inert) — view dims
				// follow the window branch below.
				if (c->canvas.valid && !c->zones_frame) {
					u_tiling_compute_canvas_view(mode, c->canvas.w, c->canvas.h,
					                             &c->view_width, &c->view_height);
				}
#if defined(XRT_OS_WINDOWS) || defined(__APPLE__)
				else if (!c->owns_window || c->zones_frame) {
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
	// XR_EXT_display_zones (ADR-027): a zones frame composes N placed zone
	// layers into the window-spanning atlas — the unzoned area must weave to
	// nothing (transparent) so the feathered wish edge blends toward the
	// desktop.
	glClearColor(0.0f, 0.0f, 0.0f, (c->transparent_background || c->zones_frame) ? 0.0f : 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	// XR_EXT_display_zones: zone rects are client-window px and the tile
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

	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		struct comp_layer *layer = &c->layer_accum.layers[i];

		// XR_EXT_display_zones: zone layers blit through the same pass at
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
				// XR_EXT_display_zones: scale the zone rect (client-
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

			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, gsc->textures[img_idx]);
			glUniform1i(loc_tex, 0);
			glUniform4f(loc_rect, nr.x, nr.y, nr.w, nr.h);

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

			// XR_EXT_display_zones: restore the REPLACE blit state for
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
			if (c->canvas.valid && !c->zones_frame && c->canvas.w > 0 && c->canvas.h > 0) {
				dp_w = c->canvas.w;
				dp_h = c->canvas.h;
			} else if (c->zones_frame) {
				// XR_EXT_display_zones: the zone PLACEMENT (layer_commit)
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
			if (!gl_composite_local_2d(c, atlas_for_present, c->shared_present_fbo, dp_w, dp_h)) {
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
			if (c->canvas.valid && !c->zones_frame && c->canvas.w > 0 && c->canvas.h > 0) {
				dp_w = c->canvas.w;
				dp_h = c->canvas.h;
			} else if (c->zones_frame) {
				// XR_EXT_display_zones: the zone PLACEMENT (layer_commit)
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
			if (!gl_composite_local_2d(c, atlas_for_present, c->iosurface_present_fbo, dp_w, dp_h)) {
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
			if (!gl_composite_local_2d(c, atlas_for_present, (GLuint)present_target_fbo, present_w,
			                           present_h)) {
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


/*
 *
 * Compositor destroy
 *
 */

static void
gl_compositor_destroy(struct xrt_compositor *xc)
{
	struct comp_gl_compositor *c = gl_comp(xc);

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

void
comp_gl_compositor_set_output_rect(struct xrt_compositor *xc,
                                    int32_t x, int32_t y,
                                    uint32_t w, uint32_t h)
{
	struct comp_gl_compositor *c = gl_comp(xc);
	c->canvas.valid = true;
	c->canvas.x = x;
	c->canvas.y = y;
	c->canvas.w = w;
	c->canvas.h = h;
}

void
comp_gl_compositor_set_surround_2d(struct xrt_compositor *xc,
                                    void *shared_handle,
                                    uint32_t w, uint32_t h)
{
	struct comp_gl_compositor *c = gl_comp(xc);
	if (shared_handle == nullptr) {
		c->surround_2d = {};
		U_LOG_I("GL surround 2D cleared");
		return;
	}
	c->surround_2d.valid = true;
	c->surround_2d.shared_handle = shared_handle;
	c->surround_2d.w = w;
	c->surround_2d.h = h;
	// Phase (post-Phase-C) TODO: WGL_NV_DX_interop2 share / EGL image
	// import for the per-frame surround blit pass.
	U_LOG_I("GL surround 2D registered: handle=%p %ux%u (open + blit pending)",
	        shared_handle, w, h);
}

/*
 *
 * #439 Phase 3 — XR_EXT_local_3d_zone authored-mask API (GL leg).
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
	// XR_EXT_display_zones: never leave a dangling frame-wish reference.
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

	// Per-frame reference (XR_EXT_display_zones): oxr sets this on every
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
	// XR_EXT_display_zones: a zones frame supersedes the canvas (the output
	// rect is inert) — the metrics already describe the window.
	if (!c->zones_frame) {
		u_canvas_apply_to_metrics(out_metrics, &c->canvas);
	}
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
	// XR_EXT_display_zones: a zones frame supersedes the canvas (the output
	// rect is inert) — the metrics already describe the window.
	if (!c->zones_frame) {
		u_canvas_apply_to_metrics(out_metrics, &c->canvas);
	}
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

	// Set up the GL→D3D shared-texture present path if a handle was provided.
	// NOTE: this does NOT use WGL_NV_DX_interop2 to weave directly into the
	// shared surface. GL interop write-BACK into the app's shared D3D texture
	// is unreliable on this stack (the GL render target fills, but the bytes
	// never reach the D3D resource — owned or opened, RGBA or BGRA). Instead we
	// weave into a plain GL render texture and bridge the result into the
	// shared surface with glReadPixels → UpdateSubresource (gl_shared_readback_upload),
	// mirroring the runtime's no-interop DComp readback present path.
	if (shared_texture_handle != NULL) {
		// D3D11 device used only to open + upload the app's shared texture.
		HRESULT hr = D3D11CreateDevice(
		    NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
		    NULL, 0, D3D11_SDK_VERSION,
		    &c->dx_device, NULL, &c->dx_context);
		if (FAILED(hr)) {
			U_LOG_E("Failed to create D3D11 device for GL shared texture: 0x%08x", hr);
			free(c);
			return XRT_ERROR_OPENGL;
		}

		// Open the app's shared D3D11 texture (its present surface).
		hr = c->dx_device->OpenSharedResource(
		    (HANDLE)shared_texture_handle,
		    __uuidof(ID3D11Texture2D), (void **)&c->dx_shared_texture);
		if (FAILED(hr)) {
			U_LOG_E("Failed to open shared D3D11 texture: 0x%08x", hr);
			c->dx_context->Release();
			c->dx_device->Release();
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
		// Shared IOSurface mode: offscreen GL context, render into IOSurface
		xrt_result_t xret = comp_gl_window_macos_create_offscreen(
		    gl_context, &c->macos_window);
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

	U_LOG_W("Native OpenGL compositor created: %ux%u", width, height);

	return XRT_SUCCESS;
}
