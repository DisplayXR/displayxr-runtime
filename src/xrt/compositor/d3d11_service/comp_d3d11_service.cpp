// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 service compositor implementation.
 * @author David Fattal
 * @ingroup comp_d3d11_service
 */

#include "comp_d3d11_service.h"
#include "d3d11_service_shaders.h"
#include "d3d11_bitmap_font.h"
#include "d3d11_capture.h"
#include "d3d11_icon_loader.h"
#include "displayxr_logo_data.h"

#include "shared/ipc_protocol.h" // struct ipc_launcher_app_list

#include "xrt/xrt_handles.h"
#include "xrt/xrt_limits.h"
#include "xrt/xrt_session.h"
#include "xrt/xrt_display_processor_d3d11.h"
#include "xrt/xrt_display_metrics.h"

#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_system.h"
#include "util/u_time.h"
#include "os/os_time.h"

#include "util/comp_layer_accum.h"

#include "comp_d3d11_window.h"

#include "math/m_api.h"
#include "math/m_vec3.h"
#include "math/m_display3d_view.h"

#include "d3d/d3d_d3d11_fence.hpp"
#include "d3d/d3d_dxgi_formats.h"

#include "util/u_hud.h"
#include "util/u_tiling.h"
#include "util/u_mcp_capture.h"

#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty_interface.h"
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>

#include <wil/com.h>
#include <wil/result.h>

#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <mutex>
#include <sddl.h>


// Bridge-relay flag: set by multi_compositor when a headless+display_info
// session connects. Read in the blit loop to use mode-native tile rects.
extern "C" bool g_bridge_relay_active;

/*
 *
 * Helpers
 *
 */

// Helper to create security attributes for AppContainer sharing
static bool
create_appcontainer_sa(SECURITY_ATTRIBUTES &sa, PSECURITY_DESCRIPTOR &sd)
{
	sa.nLength = sizeof(sa);
	sa.bInheritHandle = FALSE;

	// D: DACL
	// (A;;GA;;;AC) Allow Generic All to AppContainer (S-1-15-2-1)
	// (A;;GA;;;WD) Allow Generic All to Everyone (S-1-1-0) - for safety/debugging
	// (A;;GA;;;BA) Allow Generic All to Built-in Admins
	// (A;;GA;;;IU) Allow Generic All to Interactive User
	const wchar_t *sddl = L"D:(A;;GA;;;AC)(A;;GA;;;WD)(A;;GA;;;BA)(A;;GA;;;IU)";

	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
	        sddl, SDDL_REVISION_1, &sd, NULL)) {
		U_LOG_E("ConvertStringSecurityDescriptorToSecurityDescriptorW failed: %lu", GetLastError());
		return false;
	}

	sa.lpSecurityDescriptor = sd;
	return true;
}



/*
 *
 * Structures
 *
 */

/*!
 * Swapchain image with KeyedMutex synchronization.
 */
struct d3d11_service_image
{
	//! The imported texture
	wil::com_ptr<ID3D11Texture2D> texture;

	//! Shader resource view for compositing
	wil::com_ptr<ID3D11ShaderResourceView> srv;

	//! KeyedMutex for cross-process synchronization
	wil::com_ptr<IDXGIKeyedMutex> keyed_mutex;

	//! Whether we currently hold the mutex
	bool mutex_acquired;
};

/*!
 * D3D11 service swapchain.
 *
 * For service-created swapchains (WebXR), we use xrt_swapchain_native as base
 * so the IPC layer can access the shared handles to send to the client.
 */
struct d3d11_service_swapchain
{
	//! Base native swapchain - must be first!
	//! Contains xrt_swapchain + images[] with shared handles for IPC
	struct xrt_swapchain_native base;

	//! Parent compositor
	struct d3d11_service_compositor *comp;

	//! Swapchain images (compositor's view of the textures)
	struct d3d11_service_image images[XRT_MAX_SWAPCHAIN_IMAGES];

	//! Image count
	uint32_t image_count;

	//! Creation info
	struct xrt_swapchain_create_info info;

	//! Whether this swapchain was created by the service (vs imported from client)
	bool service_created;
};

/*!
 * Per-client render resources.
 *
 * These resources are created when a client connects and destroyed when the
 * client disconnects. This allows multiple clients to have their own windows
 * and display processors, and allows the IPC service to start without creating a
 * window until a client actually connects.
 */
struct d3d11_client_render_resources
{
	//! Dedicated-thread window (NULL if using external HWND)
	struct comp_d3d11_window *window;

	//! HWND for swap chain and display processor (owned or external)
	HWND hwnd;

	//! Whether we own the window (created it) or it's external
	bool owns_window;

	//! DXGI swap chain for display output
	wil::com_ptr<IDXGISwapChain1> swap_chain;

	//! Back buffer render target view
	wil::com_ptr<ID3D11RenderTargetView> back_buffer_rtv;

	//! Atlas render target (tiled views, full native dims)
	wil::com_ptr<ID3D11Texture2D> atlas_texture;
	wil::com_ptr<ID3D11ShaderResourceView> atlas_srv;
	//! Parallel SRV that reinterprets the (UNORM) atlas storage as SRGB-typed
	//! so sampling auto-linearizes. Used by multi_compositor_render when the
	//! client submitted an SRGB swapchain (its swapchain bytes are gamma-
	//! encoded, and raw-copied into atlas_texture verbatim — sampling them
	//! through this SRV is what produces the linear values the DP expects).
	//! Lazy-created on first use; reset whenever atlas_texture is recreated.
	wil::com_ptr<ID3D11ShaderResourceView> atlas_srv_srgb;
	wil::com_ptr<ID3D11RenderTargetView> atlas_rtv;

	//! Content-sized crop atlas for DP input (lazy-created when content < atlas)
	wil::com_ptr<ID3D11Texture2D> crop_texture;
	wil::com_ptr<ID3D11ShaderResourceView> crop_srv;
	wil::com_ptr<ID3D11RenderTargetView> crop_rtv; //!< For shader-blit Y-flip path
	uint32_t crop_width;   //!< Current crop texture width (0 = not created)
	uint32_t crop_height;  //!< Current crop texture height

	//! Generic D3D11 display processor (vendor-agnostic weaving)
	struct xrt_display_processor_d3d11 *display_processor;

	//! HUD overlay (runtime-owned windows only)
	struct u_hud *hud;

	//! D3D11 staging texture for HUD pixel upload
	wil::com_ptr<ID3D11Texture2D> hud_texture;

	//! True if HUD GPU resources are initialized
	bool hud_initialized;

	//! Last frame timestamp for FPS calculation
	uint64_t last_frame_time_ns;

	//! Smoothed frame time for display
	float smoothed_frame_time_ms;
};


/*!
 * D3D11 service native compositor.
 */
struct d3d11_service_compositor
{
	//! Base native compositor - must be first!
	struct xrt_compositor_native base;

	//! Parent system compositor
	struct d3d11_service_system *sys;

	//! Session event sink for pushing state change events
	struct xrt_session_event_sink *xses;

	//! Current visibility state
	bool state_visible;

	//! Current focus state
	bool state_focused;

	//! True if this client was created as a headless bridge-relay session
	//! (XR_EXT_display_info + XR_MND_headless). Tracked on the compositor so
	//! compositor_destroy can clear the global g_bridge_relay_active gate
	//! when the bridge disconnects — otherwise qwerty input and a handful
	//! of bridge-specific code paths stay disabled even after the bridge is
	//! gone, breaking subsequent legacy/non-bridge WebXR sessions.
	bool is_bridge_relay;

	//! App's HWND from XR_EXT_win32_window_binding (for lazy standalone init)
	HWND app_hwnd;

	//! Set when workspace re-activates — next layer_commit tears down standalone resources
	bool pending_workspace_reentry;

	//! Whether the window has been closed (triggers session exit)
	bool window_closed;

	//! Whether the EXIT_REQUEST event has already been sent (prevent duplicates)
	bool exit_request_sent;

	//! Number of frames since window close was detected
	uint32_t window_closed_frame_count;

	//! Per-client render resources (window, swap chain, display processor)
	struct d3d11_client_render_resources render;

	//! True if the client's atlas content is Y-flipped (GL clients)
	bool atlas_flip_y;

	//! True if the client's most-recent swapchain submission used an SRGB
	//! format. Atlas storage is always UNORM, but raw-copy preserves the
	//! source bytes verbatim — so when this is true, the bytes in the atlas
	//! are gamma-encoded and need to be linearized on sample by reading
	//! through render.atlas_srv_srgb. When false, the bytes are already
	//! linear (UNORM swapchain) and the default UNORM atlas_srv is correct.
	bool atlas_holds_srgb_bytes;

	//! Accumulated layers for the current frame
	struct comp_layer_accum layer_accum;

	//! Logging level
	enum u_logging_level log_level;

	//! Frame ID
	int64_t frame_id;

	//! Thread safety
	std::mutex mutex;
};

/*!
 * D3D11 service compositor semaphore (timeline fence).
 */
struct d3d11_service_semaphore
{
	//! Base semaphore - must be first!
	struct xrt_compositor_semaphore base;

	//! Parent system
	struct d3d11_service_system *sys;

	//! The D3D11 fence
	wil::com_ptr<ID3D11Fence> fence;

	//! Event for waiting on fence
	wil::unique_event_nothrow wait_event;
};


/*!
 * D3D11 service system compositor.
 *
 * Contains shared resources used by all clients (D3D11 device, shaders, etc.)
 * Per-client resources (window, swap chain, display processor) are in d3d11_client_render_resources.
 */
struct d3d11_service_system
{
	//! Base system compositor - must be first!
	struct xrt_system_compositor base;

	//! Phase 5.8: spatial launcher app registry, pushed from the workspace
	//! controller via clear+add IPC calls. Lives on the service (not the
	//! multi-comp) so it survives multi-comp create/destroy cycles —
	//! the workspace controller can push at any time, even before
	//! workspace_activate.
	struct ipc_launcher_app launcher_apps[IPC_LAUNCHER_MAX_APPS];
	uint32_t launcher_app_count;

	//! Phase 5.9/5.10: pending launcher tile click. Set by the WM_LBUTTONDOWN
	//! handler when the user clicks a tile, consumed by the workspace
	//! controller via ipc_call_launcher_poll_click() (or
	//! xrPollLauncherClickEXT). -1 means no pending click.
	//! Also carries IPC_LAUNCHER_ACTION_BROWSE for the Browse tile.
	//! Phase 6.6: also carries IPC_LAUNCHER_ACTION_REMOVE + the removed
	//! tile's full index in pending_launcher_remove_full_index.
	int32_t pending_launcher_click_index = -1;

	//! Phase 6.6: full-space index of the tile the user right-click-removed.
	//! Set by launcher_show_context_menu, consumed by the workspace
	//! controller's poll loop which deletes the entry from g_registered_apps
	//! and re-pushes. -1 means no pending remove.
	int32_t pending_launcher_remove_full_index = -1;

	//! Phase 5.11: bitmask of running tiles (bit i = launcher_apps[i] has a
	//! matching IPC client). Pushed by the workspace controller from its
	//! client-poll loop. The render pass draws a glow border around set tiles.
	uint64_t running_tile_mask = 0;

	//! Phase 6.2: visible-space hover index — tile under the mouse cursor.
	//! -1 = cursor not on any tile. Updated every frame from GetCursorPos.
	int32_t launcher_hover_index = -1;

	//! Phase 6.5: scroll offset for the launcher tile grid, in rows.
	//! 0 = top. Incremented by mouse wheel or arrow-key overflow.
	int32_t launcher_scroll_row = 0;

	//! Phase 5.12: visible-space selection index for the launcher grid.
	//! -1 = nothing selected. Reset to 0 when the launcher becomes visible.
	int32_t launcher_selected_index = -1;

	//! Phase 5.13: bitmask of tiles the user has hidden this session via
	//! right-click → Remove. Bit i = launcher_apps[i] is hidden. Cleared on
	//! every workspace registry re-push so the state is session-only.
	uint64_t hidden_tile_mask = 0;

	//! Phase 7.2: per-app icon textures loaded from sidecar icon paths.
	struct launcher_icon {
		wil::com_ptr<ID3D11ShaderResourceView> srv_2d;
		wil::com_ptr<ID3D11ShaderResourceView> srv_3d;
		uint32_t w_2d = 0, h_2d = 0, w_3d = 0, h_3d = 0;
		char layout_3d[8] = {};
	};
	struct launcher_icon launcher_icons[IPC_LAUNCHER_MAX_APPS];

	//! MCP capture_frame cross-thread hand-off (Phase B slice 7).
	struct u_mcp_capture_request mcp_capture;

	//! Multi-compositor control interface for session state management
	struct xrt_multi_compositor_control xmcc;

	//! The device we are rendering for
	struct xrt_device *xdev;

	//! System devices for qwerty input support (passed to per-client windows)
	struct xrt_system_devices *xsysd;

	//! System used to fan out session events to every registered OpenXR
	//! session (used for RENDERING_MODE_CHANGED / HARDWARE_DISPLAY_STATE).
	struct u_system *usys;

	//! D3D11 device (owned by service, not the app)
	wil::com_ptr<ID3D11Device5> device;

	//! D3D11 immediate context
	wil::com_ptr<ID3D11DeviceContext4> context;

	//! DXGI factory
	wil::com_ptr<IDXGIFactory4> dxgi_factory;

	//! Quad layer shaders
	wil::com_ptr<ID3D11VertexShader> quad_vs;
	wil::com_ptr<ID3D11PixelShader> quad_ps;

	//! Cylinder layer shaders
	wil::com_ptr<ID3D11VertexShader> cylinder_vs;
	wil::com_ptr<ID3D11PixelShader> cylinder_ps;

	//! Equirect2 layer shaders
	wil::com_ptr<ID3D11VertexShader> equirect2_vs;
	wil::com_ptr<ID3D11PixelShader> equirect2_ps;

	//! Cube layer shaders
	wil::com_ptr<ID3D11VertexShader> cube_vs;
	wil::com_ptr<ID3D11PixelShader> cube_ps;

	//! Blit shaders for projection layer copy with SRGB conversion
	wil::com_ptr<ID3D11VertexShader> blit_vs;
	wil::com_ptr<ID3D11PixelShader> blit_ps;
	wil::com_ptr<ID3D11Buffer> blit_constant_buffer;

	//! Constant buffer for layer rendering
	wil::com_ptr<ID3D11Buffer> layer_constant_buffer;

	//! Linear sampler for layer textures
	wil::com_ptr<ID3D11SamplerState> sampler_linear;

	//! Point sampler for bitmap font rendering
	wil::com_ptr<ID3D11SamplerState> sampler_point;

	//! Blend state for alpha blending
	wil::com_ptr<ID3D11BlendState> blend_alpha;

	//! Blend state for premultiplied alpha
	wil::com_ptr<ID3D11BlendState> blend_premul;

	//! Blend state for opaque
	wil::com_ptr<ID3D11BlendState> blend_opaque;

	//! Rasterizer state for layer rendering
	wil::com_ptr<ID3D11RasterizerState> rasterizer_state;

	//! Depth stencil state (disabled)
	wil::com_ptr<ID3D11DepthStencilState> depth_disabled;

	//! Phase 2.K: Depth stencil state (LESS test + write enabled). Bound for
	//! the multi-window content + chrome blit pass so windows occlude each
	//! other per-pixel — including intersecting tilted planes — without the
	//! painter's-algorithm sort or focus-on-top override.
	wil::com_ptr<ID3D11DepthStencilState> depth_test_enabled;

	//! Atlas texture dimensions (tiled views, input to display processor)
	uint32_t display_width;
	uint32_t display_height;

	//! Output dimensions (window/swap chain, native display resolution)
	uint32_t output_width;
	uint32_t output_height;

	//! View dimensions (per eye, reported to apps)
	uint32_t view_width;
	uint32_t view_height;

	//! Tile layout for atlas (from active rendering mode, default 2x1)
	uint32_t tile_columns;
	uint32_t tile_rows;

	//! Compositor HWND for publishing view dims (set on first client window creation).
	//! The WebXR bridge reads these via GetPropW to get deferred-resize-aware tile dims.
	HWND compositor_hwnd;

	//! Display refresh rate
	float refresh_rate;

	//! Logging level
	enum u_logging_level log_level;

	//! Active compositor (for eye position queries)
	//! Points to the most recently active client's compositor.
	//! Set during layer_commit, cleared on compositor destroy.
	struct d3d11_service_compositor *active_compositor;

	//! Mutex for active_compositor access
	std::mutex active_compositor_mutex;

	//! True when display is in 3D mode (weaver active). False = 2D passthrough.
	bool hardware_display_3d;

	//! Last known 3D rendering mode index (for V-key toggle restore).
	uint32_t last_3d_mode_index;

	//! Workspace mode: multi-compositor with shared window for all clients.
	//! Read from base.info.workspace_mode on first client connect.
	bool workspace_mode;

	//! Multi-compositor (NULL when workspace_mode is false).
	struct d3d11_multi_compositor *multi_comp;

	//! Mutex for multi-compositor render (serializes D3D11 context access).
	//! Recursive because unregister_client calls render for final clear frame.
	std::recursive_mutex render_mutex;

	//! Timestamp of last workspace render (monotonic ns). Used to throttle renders
	//! to ~1 per VSync, reducing torn-atlas reads from concurrent client blits.
	uint64_t last_workspace_render_ns;
};


/*
 *
 * Multi-compositor structs
 *
 */

#define D3D11_MULTI_MAX_CLIENTS 24

//! Spatial UI dimensions in METERS — the single source of truth.
//! Both rendering and hit-testing derive from these values.
#define UI_TITLE_BAR_H_M   0.008f   //!< Title bar height: 8mm
#define UI_BTN_W_M          0.008f   //!< Close/minimize button width: 8mm
#define UI_MIN_WIN_W_M      (4.0f * UI_BTN_W_M)  //!< Min window width = 3 title-bar buttons + slack so they don't overflow the left edge
#define UI_MIN_WIN_H_M      0.02f    //!< Min window height: 20mm
#define UI_TASKBAR_H_M      0.009f   //!< Taskbar height: 9mm
#define UI_GLYPH_W_M        0.0035f  //!< Glyph width: 3.5mm (balanced aspect ratio)
#define UI_GLYPH_H_M        0.005f   //!< Glyph height: 5mm
#define UI_RESIZE_ZONE_M    0.003f   //!< Resize detection zone: 3mm
#define UI_EDGE_FEATHER_PX  2.0f     //!< Edge feather width in pixels (all windows)
#define UI_GLOW_MARGIN_M    0.008f   //!< Focus glow extent in meters (~8mm)
#define UI_GLOW_INTENSITY   0.45f    //!< Focus glow opacity (0-1)
#define UI_GLOW_FALLOFF     3.0f     //!< Focus glow Gaussian tightness
#define UI_GLOW_R           0.35f    //!< Focus glow color R (soft blue)
#define UI_GLOW_G           0.55f    //!< Focus glow color G
#define UI_GLOW_B           0.95f    //!< Focus glow color B

//! Resize edge/corner flags (bitfield).
#define RESIZE_NONE   0
#define RESIZE_LEFT   1
#define RESIZE_RIGHT  2
#define RESIZE_TOP    4
#define RESIZE_BOTTOM 8

// Forward decl — defined after sync_tile_layout (see resolve_active_view_dims
// below). Needed here because the UI meter→pixel helpers use it to get the
// active-region tile dims (post-#158) instead of the atlas-divided dims.
static inline void
resolve_active_view_dims(const struct d3d11_service_system *sys,
                         uint32_t fallback_w, uint32_t fallback_h,
                         uint32_t *out_vw, uint32_t *out_vh);

/*!
 * Convert meters to pixels inside one per-view tile.
 *
 * One tile represents the full physical display area, so the conversion
 * is m_per_px = display_m / tile_px. For the tile_px denominator use the
 * *active* per-view dims (rendering_modes[idx].view_{width,height}_pixels)
 * when running a non-legacy session — that way workspace UI laid out in meters
 * lands inside the active 1920×1080 region in stereo SBS instead of getting
 * sized for the 2160-tall atlas tile and overflowing the top (#158).
 */
static inline float
ui_m_to_tile_px_x(float meters, const struct d3d11_service_system *sys)
{
	float disp_w_m = sys->base.info.display_width_m;
	uint32_t tile_px_w, tile_px_h;
	resolve_active_view_dims(sys,
	                         sys->base.info.display_pixel_width,
	                         sys->base.info.display_pixel_height,
	                         &tile_px_w, &tile_px_h);
	if (disp_w_m <= 0.0f) disp_w_m = 0.700f;
	if (tile_px_w == 0) tile_px_w = 1920;
	return meters / disp_w_m * (float)tile_px_w;
}

static inline float
ui_m_to_tile_px_y(float meters, const struct d3d11_service_system *sys)
{
	float disp_h_m = sys->base.info.display_height_m;
	uint32_t tile_px_w, tile_px_h;
	resolve_active_view_dims(sys,
	                         sys->base.info.display_pixel_width,
	                         sys->base.info.display_pixel_height,
	                         &tile_px_w, &tile_px_h);
	if (disp_h_m <= 0.0f) disp_h_m = 0.394f;
	if (tile_px_h == 0) tile_px_h = 1080;
	return meters / disp_h_m * (float)tile_px_h;
}

//! Convenience macros: convert spatial meters to SBS-tile pixels for rendering.
//! Use inside functions where 'sys' is available.
#define TITLE_BAR_HEIGHT_PX ((int)ui_m_to_tile_px_y(UI_TITLE_BAR_H_M, sys))
#define CLOSE_BTN_WIDTH_PX  ((int)ui_m_to_tile_px_x(UI_BTN_W_M, sys))
#define TASKBAR_HEIGHT_PX   ((int)ui_m_to_tile_px_y(UI_TASKBAR_H_M, sys))
#define GLYPH_W             ((int)ui_m_to_tile_px_x(UI_GLYPH_W_M, sys))
#define GLYPH_H             ((int)ui_m_to_tile_px_y(UI_GLYPH_H_M, sys))
#define RESIZE_ZONE_PX      ((int)ui_m_to_tile_px_x(UI_RESIZE_ZONE_M, sys))

/*!
 * Client type for multi-compositor slots.
 */
enum d3d11_client_type
{
	CLIENT_TYPE_IPC = 0,     //!< OpenXR IPC client with compositor + atlas
	CLIENT_TYPE_CAPTURE = 1, //!< 2D window capture (no compositor, texture from capture API)
};

/*!
 * Per-client slot in the multi-compositor.
 */
struct d3d11_multi_client_slot
{
	//! The per-client compositor that owns the atlas (NULL for capture clients).
	struct d3d11_service_compositor *compositor;

	//! Client type: IPC (OpenXR app) or capture (2D window).
	enum d3d11_client_type client_type;

	//! App's HWND (from XR_EXT_win32_window_binding). Workspace can resize via SetWindowPos.
	HWND app_hwnd;

	//! Actual rendered content dimensions per view (from last layer_commit).
	uint32_t content_view_w;
	uint32_t content_view_h;

	//! Virtual window position in 3D space (identity = centered on display).
	struct xrt_pose window_pose;

	//! Virtual window physical dimensions (meters).
	float window_width_m;
	float window_height_m;

	//! Window rect in display pixels (where this app renders in the combined atlas).
	//! x/y can be negative (window partially off-screen). w/h are always positive.
	int32_t window_rect_x;
	int32_t window_rect_y;
	int32_t window_rect_w;
	int32_t window_rect_h;

	//! True when the HWND needs to be resized to match window_rect (one-shot).
	bool hwnd_resize_pending;

	//! True when this slot has an active client.
	bool active;

	//! True when minimized (hidden from rendering but still connected).
	bool minimized;

	//! True when maximized (fills display, toggle restores pre_max state).
	bool maximized;
	struct xrt_pose pre_max_pose;
	float pre_max_width_m;
	float pre_max_height_m;

	//! True when this window was auto-minimized by another window's fullscreen toggle.
	bool fullscreen_minimized;

	//! True after the IPC client has committed at least one projection layer
	//! since slot registration. Until then, multi_compositor_render skips
	//! drawing this slot entirely — the per-client atlas is uninitialized and
	//! `content_view_w/_h` are zero, so any draw shows undefined-memory black
	//! at fallback dims. The entry animation start time is reset on the
	//! false→true transition so the slot's grow-in animation plays once
	//! content is actually available (mirrors the capture-client pattern of
	//! gating on `capture_srv` non-null at the same render-loop site).
	bool has_first_frame_committed;

	//! Monotonic time the first projection-layer commit landed for this
	//! slot. Used to gate slot rendering for an additional grace period
	//! after first commit — Chrome's WebXR pipeline keeps submitting frames
	//! at 60 Hz while Three.js's render loop is still in WebGL warmup
	//! (texture loading, shader compile), so the atlas is filled with
	//! Chrome's GPU-cleared (black) bytes for ~1–3 s past the first
	//! commit. Without this grace window the user sees a chrome-bordered
	//! window with a black interior for those seconds.
	uint64_t first_frame_ns;

	//! App name for title bar display (from HWND title or fallback).
	char app_name[128];

	//! Animation state for smooth pose transitions.
	struct
	{
		bool active;               //!< Animation in progress
		struct xrt_pose start_pose; //!< Pose at animation start
		struct xrt_pose target_pose; //!< Target pose
		float start_width_m;       //!< Width at animation start
		float start_height_m;      //!< Height at animation start
		float target_width_m;      //!< Target width
		float target_height_m;     //!< Target height
		uint64_t start_ns;         //!< Monotonic timestamp at animation start
		uint64_t duration_ns;      //!< Animation duration in nanoseconds
	} anim;

	//! Phase 2.K Commit 8.C: chrome (pill) hover-fade state. Hover-enter
	//! seeds target=1.0 over 150 ms; hover-exit seeds target=0.0 over
	//! 300 ms. Per-frame slot_chrome_fade_tick eases alpha toward target.
	//! The chrome render block multiplies alpha into the shader output;
	//! when alpha < 0.01 the chrome render is skipped entirely.
	struct
	{
		float    alpha;       //!< Current displayed alpha [0,1]
		float    start_alpha; //!< Alpha when this fade started
		float    target;      //!< Target alpha (0 = hidden, 1 = visible)
		uint64_t start_ns;
		uint64_t duration_ns;
	} chrome_fade;

	//! Phase 2.C: controller-submitted chrome swapchain. The runtime composites
	//! the swapchain image at the controller-specified pose every render, with
	//! controller-defined hit regions and depth bias.
	//!
	//! NULL until xrCreateWorkspaceClientChromeSwapchainEXT is called for this
	//! client. C5 will gate the in-runtime chrome render block on
	//! `chrome_xsc != nullptr` so default chrome only draws when the controller
	//! has not submitted its own.
	//!
	//! Refcounted via xrt_swapchain_reference. The IPC layer owns the lifetime
	//! of the underlying d3d11_service_swapchain — we just hold a strong ref
	//! to keep the SRV+texture alive as long as the slot composites it.
	struct xrt_swapchain *chrome_xsc;
	uint32_t              chrome_swapchain_id; //!< IPC swapchain id, 0 if no chrome registered
	bool                  chrome_layout_valid;
	struct xrt_pose       chrome_pose_in_client;  //!< Pose of chrome quad in client-window-local space
	float                 chrome_size_w_m;
	float                 chrome_size_h_m;
	bool                  chrome_follows_orient;  //!< If true, chrome rotates with window
	float                 chrome_depth_bias_m;    //!< 0 = use WORKSPACE_CHROME_DEPTH_BIAS default
	uint32_t              chrome_region_count;
	struct ipc_workspace_chrome_hit_region chrome_regions[IPC_WORKSPACE_CHROME_MAX_HIT_REGIONS];

	//! @name Capture-specific fields (only valid when client_type == CLIENT_TYPE_CAPTURE)
	//! @{
	struct d3d11_capture_context *capture_ctx;                //!< Opaque capture context
	wil::com_ptr<ID3D11ShaderResourceView> capture_srv;      //!< SRV for captured texture
	ID3D11Texture2D *capture_texture_last;                   //!< Last texture pointer (for SRV recreation)
	uint32_t capture_width;                                  //!< Current capture texture width
	uint32_t capture_height;                                 //!< Current capture texture height
	WINDOWPLACEMENT saved_placement;                         //!< Original window placement (for restore)
	LONG saved_exstyle;                                      //!< Original extended window style
	//! @}
};

/*!
 * Multi-compositor: shared window + DP that composites all client atlases.
 *
 * Created lazily on first client layer_commit when workspace_mode is true.
 */
struct d3d11_multi_compositor
{
	//! Dedicated-thread window for display output.
	struct comp_d3d11_window *window;
	HWND hwnd;

	//! Swap chain for display output.
	wil::com_ptr<IDXGISwapChain1> swap_chain;
	wil::com_ptr<ID3D11RenderTargetView> back_buffer_rtv;

	//! Combined atlas (all clients composited, input to DP).
	wil::com_ptr<ID3D11Texture2D> combined_atlas;
	wil::com_ptr<ID3D11ShaderResourceView> combined_atlas_srv;
	wil::com_ptr<ID3D11RenderTargetView> combined_atlas_rtv;

	//! Phase 2.K: depth target sibling to combined_atlas. Used by the
	//! multi-window content + chrome blit pass for per-pixel occlusion.
	//! Each frame is cleared to 1.0 (far) before the per-slot render loop;
	//! per-corner SV_Position.z values from the blit VS resolve occlusion
	//! via D3D11's LESS depth test. Resets / re-creates alongside
	//! combined_atlas (multi_compositor_ensure_output / atlas teardown).
	wil::com_ptr<ID3D11Texture2D> combined_atlas_depth;
	wil::com_ptr<ID3D11DepthStencilView> combined_atlas_dsv;

	//! Display processor (single, shared).
	struct xrt_display_processor_d3d11 *display_processor;

	//! Crop texture for DP input (content-sized, lazily created).
	wil::com_ptr<ID3D11Texture2D> crop_texture;
	wil::com_ptr<ID3D11ShaderResourceView> crop_srv;
	uint32_t crop_width;
	uint32_t crop_height;

	//! Per-client slots.
	struct d3d11_multi_client_slot clients[D3D11_MULTI_MAX_CLIENTS];
	uint32_t client_count;

	//! Focused client index (-1 = none).
	int32_t focused_slot;

	//! Phase 2.K: focused-slot value last seen by the public-API drain. The
	//! drain compares against `focused_slot` and emits a FOCUS_CHANGED event
	//! to the workspace controller on each transition (TAB cycle, click
	//! auto-focus, controller-set, client disconnect). Initialised to -1 so
	//! the first drain after any non-empty focus emits a transition.
	int32_t focused_slot_last_emitted;

	//! Phase 2.K: vsync-aligned frame counter. Incremented once per
	//! `multi_compositor_render` and read by the public-API drain to emit
	//! FRAME_TICK events (capped per-batch) so controllers can pace
	//! per-frame work without polling a timer.
	volatile LONG frame_tick_count;
	LONG frame_tick_last_emitted;
	uint64_t frame_tick_last_ns;

	//! Window dismissed by user (ESC).
	bool window_dismissed;

	//! True after dismiss cleanup (EXIT_REQUEST sent, captures released).
	bool dismiss_cleanup_done;

	//! Workspace deactivated (Ctrl+Space): window hidden, DP released, captures stopped.
	//! Unlike window_dismissed, the multi-comp structure stays alive for re-activation.
	bool suspended;

	//! Phase 5.7: spatial launcher panel visible.
	//! Toggled by Ctrl+L via ipc_call_launcher_set_visible. When true, the
	//! render loop draws a rounded-corner panel at the zero-disparity plane.
	//! The app list it renders lives on d3d11_service_system (sys->launcher_apps).
	bool launcher_visible;

	//! Right-click-drag state for window repositioning.
	struct
	{
		bool active;         //!< Currently dragging?
		int32_t slot;        //!< Which slot is being dragged (-1 = none)
		POINT start_cursor;  //!< Cursor position at drag start (workspace-window client pixels)
		float start_pos_x;   //!< Window pose.position.x at drag start (meters)
		float start_pos_y;   //!< Window pose.position.y at drag start (meters)
	} drag;

	//! Left-click title bar drag state (parallel to right-click drag).
	struct
	{
		bool active;
		int32_t slot;
		POINT start_cursor;
		float start_pos_x;
		float start_pos_y;
	} title_drag;

	//! Edge/corner resize state (left-click-drag on window border).
	struct
	{
		bool active;
		int32_t slot;
		int edges;           //!< Bitfield: RESIZE_LEFT|RIGHT|TOP|BOTTOM
		POINT start_cursor;
		float start_pos_x, start_pos_y;
		float start_width_m, start_height_m;
	} resize;

	//! Cached cursor handles for resize.
	HCURSOR cursor_arrow;
	HCURSOR cursor_sizewe;   // left-right
	HCURSOR cursor_sizens;   // up-down
	HCURSOR cursor_sizenwse; // NW-SE diagonal
	HCURSOR cursor_sizenesw; // NE-SW diagonal
	HCURSOR cursor_sizeall;  // move/grab (title drag, right-click drag)

	//! Title bar right-click drag state for window rotation.
	struct
	{
		bool active;
		int32_t slot;
		POINT start_cursor;
		float start_yaw;   //!< Yaw (radians) at drag start
		float start_pitch;  //!< Pitch (radians) at drag start
	} title_rmb_drag;

	//! Hovered button: 0=none, 1=close, 2=minimize, 3=maximize, for the hovered slot.
	int hover_btn;
	int hover_btn_slot;

	//! Momentary toast notification (e.g. "how to restore after fullscreen").
	char toast_text[256];
	uint64_t toast_until_ns;

	//! Previous frame LMB/RMB state (for rising-edge detection).
	bool prev_lmb_held;
	bool prev_rmb_held;

	//! Double-click detection for title bar maximize toggle.
	DWORD last_title_click_time;
	int32_t last_title_click_slot;

	//! Font atlas for title bar text (DirectWrite-rendered, anti-aliased).
	wil::com_ptr<ID3D11Texture2D> font_atlas;
	wil::com_ptr<ID3D11ShaderResourceView> font_atlas_srv;
	float glyph_advances[96];  //!< Per-glyph advance width in atlas pixels (proportional)
	uint32_t font_glyph_w;     //!< Max glyph cell width in atlas pixels
	uint32_t font_glyph_h;     //!< Glyph cell height in atlas pixels
	uint32_t font_atlas_w;     //!< Total atlas width in pixels
	uint32_t font_atlas_h;     //!< Total atlas height in pixels

	//! Embedded DisplayXR logo PNG decoded to an SRV on first use. Rendered in
	//! the empty state (no clients, no launcher). Source bytes come from
	//! displayxr_white_png[] which is generated from doc/displayxr_white.png.
	wil::com_ptr<ID3D11ShaderResourceView> logo_srv;
	uint32_t logo_w;
	uint32_t logo_h;
	bool logo_load_tried;

	//! @name Capture client render timer
	//! @{
	std::thread capture_render_thread;              //!< Timer thread for workspace rendering
	std::atomic<bool> capture_render_running{false}; //!< Thread run flag
	uint32_t capture_client_count{0};               //!< Number of active capture-type slots
	//! Wakeup event for the render thread: signaled on shutdown or when an
	//! interaction (drag, rotation) needs a render sooner than the 14ms timeout.
	HANDLE render_wakeup_event{nullptr};
	//! @}

	//! True when display is in 2D mode due to capture client focus.
	//! Tracked separately from sys->hardware_display_3d to detect transitions.
	bool capture_forced_2d;


	//! Tracks which capture HWND currently has foreground focus for SendInput.
	//! NULL means no capture client is foreground (workspace window is foreground).
	HWND current_foreground_capture;
};


/*
 *
 * Helper functions
 *
 */

static inline struct d3d11_service_swapchain *
d3d11_service_swapchain_from_xrt(struct xrt_swapchain *xsc)
{
	return reinterpret_cast<struct d3d11_service_swapchain *>(xsc);
}

static inline struct d3d11_service_compositor *
d3d11_service_compositor_from_xrt(struct xrt_compositor *xc)
{
	return reinterpret_cast<struct d3d11_service_compositor *>(xc);
}

static inline struct d3d11_service_system *
d3d11_service_system_from_xrt(struct xrt_system_compositor *xsysc)
{
	return reinterpret_cast<struct d3d11_service_system *>(xsysc);
}

static inline struct d3d11_service_semaphore *
d3d11_service_semaphore_from_xrt(struct xrt_compositor_semaphore *xcsem)
{
	return reinterpret_cast<struct d3d11_service_semaphore *>(xcsem);
}

// Write sys->workspace_mode and mirror the flag onto the multi-comp window so
// its WndProc's ESC-close path can distinguish empty-workspace (no focused app)
// from true non-workspace mode — see comp_d3d11_window.cpp ESC handling.
static inline void
service_set_workspace_mode(struct d3d11_service_system *sys, bool active)
{
	sys->workspace_mode = active;
	if (sys->multi_comp != nullptr && sys->multi_comp->window != nullptr) {
		comp_d3d11_window_set_workspace_mode_active(sys->multi_comp->window, active);
	}
}

// True iff a bridge relay session exists AND a WebSocket client is currently
// connected to the bridge exe. Per-frame gate for bridge-specific behavior
// (crop override, atlas-resize skip, qwerty suppression, vendor hw-state
// forwarding). `g_bridge_relay_active` alone is too coarse: the bridge exe
// holds its OpenXR session alive for its entire lifetime regardless of
// whether the Chrome extension is connected, so gating on it alone disables
// legacy WebXR paths whenever the bridge process is running. The bridge
// sets/clears DXR_BridgeClientActive on the compositor HWND on WS
// accept/disconnect.
static bool
bridge_client_is_live(struct d3d11_service_system *sys, HWND live_hwnd_hint)
{
	if (!g_bridge_relay_active) return false;
	// Prefer the caller's current frame hwnd over sys->compositor_hwnd.
	// sys->compositor_hwnd is only assigned on first-session window creation
	// (line ~1939 checks `== nullptr`), so across Chrome page reloads /
	// session transitions it stays pinned to the old window. The bridge's
	// FindWindowW finds the current live window and pushes
	// DXR_BridgeClientActive there; if we check the cached pin we'd read a
	// stale (possibly destroyed) window that never has the prop.
	HWND hwnd = live_hwnd_hint != nullptr ? live_hwnd_hint
	                                       : (sys != nullptr ? sys->compositor_hwnd : nullptr);
	if (hwnd == nullptr) return false;
	return GetPropW(hwnd, L"DXR_BridgeClientActive") != nullptr;
}

// Authoritative per-frame bridge-relay gate. Unlike bridge_client_is_live,
// does not depend on the caller's c->render.hwnd — scans sys->compositor_hwnd
// plus every active client's hwnd for the DXR_BridgeClientActive prop. This
// is the gate used to drive the qwerty freeze, which is process-global and
// must not oscillate based on which client's layer_commit ran last.
//
// Other callers (crop override, atlas-resize skip, vendor hw-state) keep
// using bridge_client_is_live with the per-client hwnd — they genuinely
// want "is this specific client the bridge client" semantics.
static bool
bridge_relay_is_live_authoritative(struct d3d11_service_system *sys)
{
	if (!g_bridge_relay_active) return false;
	if (sys == nullptr) return false;

	if (sys->compositor_hwnd != nullptr &&
	    GetPropW(sys->compositor_hwnd, L"DXR_BridgeClientActive") != nullptr) {
		return true;
	}

	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (mc == nullptr) return false;
	for (int i = 0; i < D3D11_MULTI_MAX_CLIENTS; i++) {
		struct d3d11_multi_client_slot *slot = &mc->clients[i];
		if (!slot->active) continue;
		if (slot->compositor == nullptr) continue;
		HWND h = slot->compositor->render.hwnd;
		if (h == nullptr) continue;
		if (GetPropW(h, L"DXR_BridgeClientActive") != nullptr) return true;
	}
	return false;
}

// Forward declarations — defined later, used by client registration code.
static void
compute_grid_layout(const struct d3d11_service_system *sys,
                    int n, int idx,
                    float *out_x, float *out_y, float *out_z,
                    float *out_w, float *out_h);

static void
slot_animate_to(struct d3d11_multi_client_slot *slot,
                const struct xrt_pose *target_pose,
                float target_w, float target_h,
                uint64_t now_ns, uint64_t duration_ns);

#define ANIM_DURATION_NS (300ULL * 1000000ULL)

/*!
 * Sync tile layout from the active rendering mode of the head device.
 * Defaults to 2 columns, 1 row (side-by-side stereo) if not available.
 */
static void
sync_tile_layout(struct d3d11_service_system *sys)
{
	sys->tile_columns = 2;
	sys->tile_rows = 1;

	if (sys->xdev != NULL && sys->xdev->hmd != NULL) {
		uint32_t idx = sys->xdev->hmd->active_rendering_mode_index;
		if (idx < sys->xdev->rendering_mode_count) {
			uint32_t tc = sys->xdev->rendering_modes[idx].tile_columns;
			uint32_t tr = sys->xdev->rendering_modes[idx].tile_rows;
			if (tc > 0 && tr > 0) {
				sys->tile_columns = tc;
				sys->tile_rows = tr;
			}
		}
	}

	// Keep view_width/height consistent with tile layout and atlas dims.
	// On 2D/3D toggle, tile_columns changes (e.g. 2→1) but the atlas size
	// stays the same. Without this, view_width would be stale from the
	// previous mode, causing incorrect tile placement and crop-blit sizing.
	if (sys->display_width > 0 && sys->display_height > 0) {
		sys->view_width = sys->display_width / sys->tile_columns;
		sys->view_height = sys->display_height / sys->tile_rows;
	}
}

/*!
 * Fan out a rendering-mode-change session event to every registered OpenXR
 * session under this system. Also fans out a hardware-display-state-change
 * event if the 3D bit flipped between prev_idx and new_idx. No-op if the
 * index did not change.
 */
static void
broadcast_rendering_mode_change(struct d3d11_service_system *sys,
                                struct xrt_device *head,
                                uint32_t prev_idx,
                                uint32_t new_idx)
{
	if (sys == nullptr || sys->usys == nullptr || head == nullptr || head->hmd == NULL) {
		return;
	}
	if (prev_idx == new_idx) {
		return;
	}

	union xrt_session_event xse = {};
	xse.rendering_mode_change.type = XRT_SESSION_EVENT_RENDERING_MODE_CHANGE;
	xse.rendering_mode_change.previous_mode_index = prev_idx;
	xse.rendering_mode_change.current_mode_index = new_idx;
	u_system_broadcast_event(sys->usys, &xse);

	if (prev_idx < head->rendering_mode_count && new_idx < head->rendering_mode_count) {
		bool prev_3d = head->rendering_modes[prev_idx].hardware_display_3d;
		bool new_3d = head->rendering_modes[new_idx].hardware_display_3d;
		if (prev_3d != new_3d) {
			union xrt_session_event xse2 = {};
			xse2.hardware_display_state_change.type = XRT_SESSION_EVENT_HARDWARE_DISPLAY_STATE_CHANGE;
			xse2.hardware_display_state_change.hardware_display_3d = new_3d;
			u_system_broadcast_event(sys->usys, &xse2);
		}
	}
}

/*!
 * Resolve per-view tile dimensions for layout / DP handoff / capture.
 *
 * Non-legacy sessions (workspace + display-info-aware apps) use the true vendor
 * scale from the active rendering mode — for stereo on 4K this is 1920×1080
 * per view. Legacy sessions fall back to the system's compromise dims
 * (display / tile count), preserving existing behavior for apps that aren't
 * XR_EXT_display_info aware. Issue #158.
 */
static inline void
resolve_active_view_dims(const struct d3d11_service_system *sys,
                         uint32_t fallback_w, uint32_t fallback_h,
                         uint32_t *out_vw, uint32_t *out_vh)
{
	uint32_t vw = 0, vh = 0;
	if (!sys->base.info.legacy_app_tile_scaling &&
	    sys->xdev != NULL && sys->xdev->hmd != NULL) {
		uint32_t idx = sys->xdev->hmd->active_rendering_mode_index;
		if (idx < sys->xdev->rendering_mode_count) {
			vw = sys->xdev->rendering_modes[idx].view_width_pixels;
			vh = sys->xdev->rendering_modes[idx].view_height_pixels;
		}
	}
	if (vw == 0 || vh == 0) {
		uint32_t tc = sys->tile_columns > 0 ? sys->tile_columns : 1;
		uint32_t tr = sys->tile_rows > 0 ? sys->tile_rows : 1;
		vw = fallback_w / tc;
		vh = fallback_h / tr;
	}
	*out_vw = vw;
	*out_vh = vh;
}

/*!
 * Check if a DXGI format is an SRGB format.
 */
static inline bool
is_srgb_format(DXGI_FORMAT format)
{
	switch (format) {
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
	case DXGI_FORMAT_BC1_UNORM_SRGB:
	case DXGI_FORMAT_BC2_UNORM_SRGB:
	case DXGI_FORMAT_BC3_UNORM_SRGB:
	case DXGI_FORMAT_BC7_UNORM_SRGB:
		return true;
	default:
		return false;
	}
}

/*!
 * Get the SRGB variant of a format for SRV creation.
 * Returns the same format if already SRGB or no SRGB variant exists.
 */
static inline DXGI_FORMAT
get_srgb_format(DXGI_FORMAT format)
{
	switch (format) {
	case DXGI_FORMAT_R8G8B8A8_UNORM:
		return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	case DXGI_FORMAT_B8G8R8A8_UNORM:
		return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	case DXGI_FORMAT_B8G8R8X8_UNORM:
		return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
	default:
		return format;  // Return as-is
	}
}


/*
 *
 * Shader compilation helpers
 *
 */

static HRESULT
compile_shader(const char *source, const char *entry, const char *target, ID3DBlob **out_blob)
{
	ID3DBlob *errors = nullptr;
	HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, entry, target, 0, 0, out_blob,
	                        &errors);
	if (FAILED(hr)) {
		if (errors != nullptr) {
			U_LOG_E("Shader compile error: %s", (char *)errors->GetBufferPointer());
			errors->Release();
		}
	}
	if (errors != nullptr) {
		errors->Release();
	}
	return hr;
}

static bool
create_layer_shaders(struct d3d11_service_system *sys)
{
	ID3DBlob *blob = nullptr;
	HRESULT hr;

	// Quad vertex shader
	hr = compile_shader(quad_vs_hlsl, "VSMain", "vs_5_0", &blob);
	if (FAILED(hr)) {
		U_LOG_E("Failed to compile quad vertex shader");
		return false;
	}
	hr = sys->device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
	                                      sys->quad_vs.put());
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create quad vertex shader: 0x%08lx", hr);
		return false;
	}

	// Quad pixel shader
	hr = compile_shader(quad_ps_hlsl, "PSMain", "ps_5_0", &blob);
	if (FAILED(hr)) {
		U_LOG_E("Failed to compile quad pixel shader");
		return false;
	}
	hr = sys->device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
	                                     sys->quad_ps.put());
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create quad pixel shader: 0x%08lx", hr);
		return false;
	}

	// Cylinder vertex shader
	hr = compile_shader(cylinder_vs_hlsl, "VSMain", "vs_5_0", &blob);
	if (FAILED(hr)) {
		U_LOG_E("Failed to compile cylinder vertex shader");
		return false;
	}
	hr = sys->device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
	                                      sys->cylinder_vs.put());
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create cylinder vertex shader: 0x%08lx", hr);
		return false;
	}

	// Cylinder pixel shader
	hr = compile_shader(cylinder_ps_hlsl, "PSMain", "ps_5_0", &blob);
	if (FAILED(hr)) {
		U_LOG_E("Failed to compile cylinder pixel shader");
		return false;
	}
	hr = sys->device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
	                                     sys->cylinder_ps.put());
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create cylinder pixel shader: 0x%08lx", hr);
		return false;
	}

	// Equirect2 vertex shader
	hr = compile_shader(equirect2_vs_hlsl, "VSMain", "vs_5_0", &blob);
	if (FAILED(hr)) {
		U_LOG_E("Failed to compile equirect2 vertex shader");
		return false;
	}
	hr = sys->device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
	                                      sys->equirect2_vs.put());
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create equirect2 vertex shader: 0x%08lx", hr);
		return false;
	}

	// Equirect2 pixel shader
	hr = compile_shader(equirect2_ps_hlsl, "PSMain", "ps_5_0", &blob);
	if (FAILED(hr)) {
		U_LOG_E("Failed to compile equirect2 pixel shader");
		return false;
	}
	hr = sys->device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
	                                     sys->equirect2_ps.put());
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create equirect2 pixel shader: 0x%08lx", hr);
		return false;
	}

	// Cube vertex shader
	hr = compile_shader(cube_vs_hlsl, "VSMain", "vs_5_0", &blob);
	if (FAILED(hr)) {
		U_LOG_E("Failed to compile cube vertex shader");
		return false;
	}
	hr = sys->device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
	                                      sys->cube_vs.put());
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create cube vertex shader: 0x%08lx", hr);
		return false;
	}

	// Cube pixel shader
	hr = compile_shader(cube_ps_hlsl, "PSMain", "ps_5_0", &blob);
	if (FAILED(hr)) {
		U_LOG_E("Failed to compile cube pixel shader");
		return false;
	}
	hr = sys->device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
	                                     sys->cube_ps.put());
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create cube pixel shader: 0x%08lx", hr);
		return false;
	}

	// Blit vertex shader (for projection layer copy with SRGB conversion)
	hr = compile_shader(blit_vs_hlsl, "VSMain", "vs_5_0", &blob);
	if (FAILED(hr)) {
		U_LOG_E("Failed to compile blit vertex shader");
		return false;
	}
	hr = sys->device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
	                                      sys->blit_vs.put());
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create blit vertex shader: 0x%08lx", hr);
		return false;
	}

	// Blit pixel shader
	hr = compile_shader(blit_ps_hlsl, "PSMain", "ps_5_0", &blob);
	if (FAILED(hr)) {
		U_LOG_E("Failed to compile blit pixel shader");
		return false;
	}
	hr = sys->device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
	                                     sys->blit_ps.put());
	blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("Failed to create blit pixel shader: 0x%08lx", hr);
		return false;
	}

	U_LOG_I("Created all layer shaders (including blit)");
	return true;
}

static bool
create_layer_resources(struct d3d11_service_system *sys)
{
	HRESULT hr;

	// Create constant buffer (largest of all layer constant structs)
	size_t cb_size = sizeof(Equirect2LayerConstants);  // Largest
	D3D11_BUFFER_DESC cb_desc = {};
	cb_desc.ByteWidth = static_cast<UINT>((cb_size + 15) & ~15);  // 16-byte aligned
	cb_desc.Usage = D3D11_USAGE_DYNAMIC;
	cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

	hr = sys->device->CreateBuffer(&cb_desc, nullptr, sys->layer_constant_buffer.put());
	if (FAILED(hr)) {
		U_LOG_E("Failed to create layer constant buffer: 0x%08lx", hr);
		return false;
	}

	// Create blit constant buffer
	D3D11_BUFFER_DESC blit_cb_desc = {};
	blit_cb_desc.ByteWidth = static_cast<UINT>((sizeof(BlitConstants) + 15) & ~15);
	blit_cb_desc.Usage = D3D11_USAGE_DYNAMIC;
	blit_cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	blit_cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

	hr = sys->device->CreateBuffer(&blit_cb_desc, nullptr, sys->blit_constant_buffer.put());
	if (FAILED(hr)) {
		U_LOG_E("Failed to create blit constant buffer: 0x%08lx", hr);
		return false;
	}

	// Create linear sampler
	D3D11_SAMPLER_DESC samp_desc = {};
	samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;

	hr = sys->device->CreateSamplerState(&samp_desc, sys->sampler_linear.put());
	if (FAILED(hr)) {
		U_LOG_E("Failed to create linear sampler: 0x%08lx", hr);
		return false;
	}

	// Create point sampler (for bitmap font rendering — no filtering)
	D3D11_SAMPLER_DESC point_samp_desc = {};
	point_samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	point_samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	point_samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	point_samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;

	hr = sys->device->CreateSamplerState(&point_samp_desc, sys->sampler_point.put());
	if (FAILED(hr)) {
		U_LOG_E("Failed to create point sampler: 0x%08lx", hr);
		return false;
	}

	// Create blend state for alpha blending
	D3D11_BLEND_DESC blend_desc = {};
	blend_desc.RenderTarget[0].BlendEnable = TRUE;
	blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

	hr = sys->device->CreateBlendState(&blend_desc, sys->blend_alpha.put());
	if (FAILED(hr)) {
		U_LOG_E("Failed to create alpha blend state: 0x%08lx", hr);
		return false;
	}

	// Premultiplied alpha blend state
	blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
	blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;

	hr = sys->device->CreateBlendState(&blend_desc, sys->blend_premul.put());
	if (FAILED(hr)) {
		U_LOG_E("Failed to create premul blend state: 0x%08lx", hr);
		return false;
	}

	// Opaque blend state
	blend_desc.RenderTarget[0].BlendEnable = FALSE;

	hr = sys->device->CreateBlendState(&blend_desc, sys->blend_opaque.put());
	if (FAILED(hr)) {
		U_LOG_E("Failed to create opaque blend state: 0x%08lx", hr);
		return false;
	}

	// Create rasterizer state
	D3D11_RASTERIZER_DESC raster_desc = {};
	raster_desc.FillMode = D3D11_FILL_SOLID;
	raster_desc.CullMode = D3D11_CULL_NONE;
	raster_desc.FrontCounterClockwise = FALSE;
	raster_desc.DepthClipEnable = TRUE;
	raster_desc.ScissorEnable = TRUE; // Enabled for per-tile clipping

	hr = sys->device->CreateRasterizerState(&raster_desc, sys->rasterizer_state.put());
	if (FAILED(hr)) {
		U_LOG_E("Failed to create rasterizer state: 0x%08lx", hr);
		return false;
	}

	// Set default full-viewport scissor rect so non-workspace rendering isn't clipped.
	// Workspace mode overrides this per-tile in multi_compositor_render().
	{
		D3D11_RECT full_scissor = {0, 0, 16384, 16384}; // Large enough for any display
		sys->context->RSSetScissorRects(1, &full_scissor);
	}

	// Create depth stencil state (disabled)
	D3D11_DEPTH_STENCIL_DESC ds_desc = {};
	ds_desc.DepthEnable = FALSE;
	ds_desc.StencilEnable = FALSE;

	hr = sys->device->CreateDepthStencilState(&ds_desc, sys->depth_disabled.put());
	if (FAILED(hr)) {
		U_LOG_E("Failed to create depth stencil state: 0x%08lx", hr);
		return false;
	}

	// Phase 2.K: depth-test state (LESS_EQUAL, depth-write enabled). The
	// blit VS outputs SV_Position.z = corner_depth_ndc[i] * w (so after
	// perspective divide we get the [0,1] depth value back), and this
	// state turns the hardware depth test on for the multi-window blit
	// pass. LESS_EQUAL (vs strict LESS) lets equal-depth chrome elements
	// drawn in order — title-bar bg, then buttons, then glyphs — paint on
	// top of each other within a window. Inter-window occlusion is
	// unaffected since distinct windows have distinct z values.
	D3D11_DEPTH_STENCIL_DESC ds_test = {};
	ds_test.DepthEnable = TRUE;
	ds_test.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	ds_test.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
	ds_test.StencilEnable = FALSE;
	hr = sys->device->CreateDepthStencilState(&ds_test, sys->depth_test_enabled.put());
	if (FAILED(hr)) {
		U_LOG_E("Failed to create depth-test depth stencil state: 0x%08lx", hr);
		return false;
	}

	U_LOG_I("Created all layer rendering resources");
	return true;
}


/*
 *
 * Projection layer blit with SRGB conversion
 *
 */

/*!
 * Blit a region from source texture to stereo texture with optional SRGB conversion.
 *
 * This replaces CopySubresourceRegion when the source is SRGB, ensuring proper
 * gamma handling. When source is SRGB, the GPU linearizes on sample and we
 * re-encode to sRGB for the display processor.
 *
 * @param sys The system compositor
 * @param src_tex Source texture to blit from
 * @param src_srv SRV for the source texture (should be SRGB format if source is SRGB)
 * @param src_rect Source rectangle (x, y, width, height) in pixels
 * @param src_size Source texture size (width, height)
 * @param dst_x Destination X offset in stereo texture
 * @param dst_y Destination Y offset in stereo texture
 * @param is_srgb Whether source is SRGB format (triggers gamma conversion)
 */
static void
blit_to_atlas_texture(struct d3d11_service_system *sys,
                       struct d3d11_client_render_resources *res,
                       ID3D11ShaderResourceView *src_srv,
                       float src_x, float src_y, float src_w, float src_h,
                       float src_tex_w, float src_tex_h,
                       float dst_x, float dst_y,
                       float dst_w, float dst_h,
                       bool is_srgb)
{
	// Update blit constant buffer
	D3D11_MAPPED_SUBRESOURCE mapped;
	HRESULT hr = sys->context->Map(sys->blit_constant_buffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (FAILED(hr)) {
		U_LOG_E("Failed to map blit constant buffer: 0x%08lx", hr);
		return;
	}

	BlitConstants *cb = static_cast<BlitConstants *>(mapped.pData);
	cb->src_rect[0] = src_x;
	cb->src_rect[1] = src_y;
	cb->src_rect[2] = src_w;
	cb->src_rect[3] = src_h;
	cb->dst_offset[0] = dst_x;
	cb->dst_offset[1] = dst_y;
	cb->src_size[0] = src_tex_w;
	cb->src_size[1] = src_tex_h;
	cb->dst_size[0] = static_cast<float>(sys->display_width);
	cb->dst_size[1] = static_cast<float>(sys->display_height);
	cb->convert_srgb = is_srgb ? 1.0f : 0.0f;
	cb->chrome_alpha = 0.0f; // 8.C: 0=full opacity (chrome blits override)
	cb->quad_mode = 0.0f;
	cb->dst_rect_wh[0] = dst_w;
	cb->dst_rect_wh[1] = dst_h;
	cb->corner_radius = 0.0f;
	cb->corner_aspect = 0.0f;
	cb->edge_feather = 0.0f;
	cb->glow_intensity = 0.0f;

	sys->context->Unmap(sys->blit_constant_buffer.get(), 0);

	// Set up pipeline for blit
	sys->context->VSSetShader(sys->blit_vs.get(), nullptr, 0);
	sys->context->PSSetShader(sys->blit_ps.get(), nullptr, 0);
	sys->context->VSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
	sys->context->PSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
	sys->context->PSSetShaderResources(0, 1, &src_srv);
	sys->context->PSSetSamplers(0, 1, sys->sampler_linear.addressof());

	// Set render target to per-client stereo texture
	ID3D11RenderTargetView *rtvs[] = {res->atlas_rtv.get()};
	sys->context->OMSetRenderTargets(1, rtvs, nullptr);

	// Set viewport to cover destination region
	D3D11_VIEWPORT vp = {};
	vp.TopLeftX = 0;
	vp.TopLeftY = 0;
	vp.Width = static_cast<float>(sys->display_width);
	vp.Height = static_cast<float>(sys->display_height);
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	sys->context->RSSetViewports(1, &vp);

	// Set blend state to opaque (overwrite)
	float blend_factor[4] = {0, 0, 0, 0};
	sys->context->OMSetBlendState(sys->blend_opaque.get(), blend_factor, 0xFFFFFFFF);
	sys->context->OMSetDepthStencilState(sys->depth_disabled.get(), 0);
	sys->context->RSSetState(sys->rasterizer_state.get());

	// Draw fullscreen quad (4 vertices, triangle strip)
	sys->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	sys->context->IASetInputLayout(nullptr);
	sys->context->Draw(4, 0);

	// Clear shader resources to avoid hazards
	ID3D11ShaderResourceView *null_srv = nullptr;
	sys->context->PSSetShaderResources(0, 1, &null_srv);
}


/*
 *
 * Layer visibility check
 *
 */

static bool
is_layer_view_visible(const struct xrt_layer_data *data, uint32_t view_index)
{
	enum xrt_layer_eye_visibility visibility;

	switch (data->type) {
	case XRT_LAYER_QUAD: visibility = data->quad.visibility; break;
	case XRT_LAYER_CYLINDER: visibility = data->cylinder.visibility; break;
	case XRT_LAYER_EQUIRECT1: visibility = data->equirect1.visibility; break;
	case XRT_LAYER_EQUIRECT2: visibility = data->equirect2.visibility; break;
	case XRT_LAYER_CUBE: visibility = data->cube.visibility; break;
	default: return true;  // Projection layers visible in both
	}

	switch (visibility) {
	case XRT_LAYER_EYE_VISIBILITY_NONE: return false;
	case XRT_LAYER_EYE_VISIBILITY_LEFT_BIT: return view_index == 0;
	case XRT_LAYER_EYE_VISIBILITY_RIGHT_BIT: return view_index == 1;
	case XRT_LAYER_EYE_VISIBILITY_BOTH: return true;
	default: return true;
	}
}


/*
 *
 * Layer rendering helpers
 *
 */

static void
get_color_scale_bias(const struct xrt_layer_data *data, float color_scale[4], float color_bias[4])
{
	bool has_color_scale_bias = (data->flags & XRT_LAYER_COMPOSITION_COLOR_BIAS_SCALE) != 0;

	if (has_color_scale_bias) {
		color_scale[0] = data->color_scale.r;
		color_scale[1] = data->color_scale.g;
		color_scale[2] = data->color_scale.b;
		color_scale[3] = data->color_scale.a;
		color_bias[0] = data->color_bias.r;
		color_bias[1] = data->color_bias.g;
		color_bias[2] = data->color_bias.b;
		color_bias[3] = data->color_bias.a;
	} else {
		color_scale[0] = 1.0f;
		color_scale[1] = 1.0f;
		color_scale[2] = 1.0f;
		color_scale[3] = 1.0f;
		color_bias[0] = 0.0f;
		color_bias[1] = 0.0f;
		color_bias[2] = 0.0f;
		color_bias[3] = 0.0f;
	}
}

static void
set_blend_state(struct d3d11_service_system *sys, const struct xrt_layer_data *data)
{
	bool use_premul = (data->flags & XRT_LAYER_COMPOSITION_BLEND_TEXTURE_SOURCE_ALPHA_BIT) == 0;

	if (use_premul) {
		sys->context->OMSetBlendState(sys->blend_premul.get(), nullptr, 0xFFFFFFFF);
	} else {
		sys->context->OMSetBlendState(sys->blend_alpha.get(), nullptr, 0xFFFFFFFF);
	}
}

static void
render_quad_layer(struct d3d11_service_system *sys,
                  const struct comp_layer *layer,
                  uint32_t view_index,
                  const struct xrt_pose *view_pose,
                  const struct xrt_fov *fov)
{
	const struct xrt_layer_data *data = &layer->data;
	const struct xrt_layer_quad_data *q = &data->quad;

	// Get swapchain
	struct xrt_swapchain *xsc = layer->sc_array[0];
	if (xsc == nullptr) {
		return;
	}
	struct d3d11_service_swapchain *sc = d3d11_service_swapchain_from_xrt(xsc);

	uint32_t image_index = q->sub.image_index;
	if (image_index >= sc->image_count) {
		return;
	}

	ID3D11ShaderResourceView *srv = sc->images[image_index].srv.get();
	if (srv == nullptr) {
		return;
	}

	// Build MVP matrix
	struct xrt_matrix_4x4 model, view, proj, mv, mvp;

	// Model: translate + rotate + scale by quad size
	struct xrt_vec3 scale = {q->size.x, q->size.y, 1.0f};
	math_matrix_4x4_model(&q->pose, &scale, &model);

	// View matrix
	math_matrix_4x4_view_from_pose(view_pose, &view);

	// Projection matrix (Vulkan-style infinite reverse)
	math_matrix_4x4_projection_vulkan_infinite_reverse(fov, 0.1f, &proj);

	// MVP
	math_matrix_4x4_multiply(&view, &model, &mv);
	math_matrix_4x4_multiply(&proj, &mv, &mvp);

	// Fill constant buffer
	QuadLayerConstants constants = {};
	memcpy(constants.mvp, &mvp, sizeof(constants.mvp));

	// UV transform for sub-image
	constants.post_transform[0] = q->sub.norm_rect.x;
	constants.post_transform[1] = q->sub.norm_rect.y;
	constants.post_transform[2] = q->sub.norm_rect.w;
	constants.post_transform[3] = q->sub.norm_rect.h;

	// Handle Y-flip
	if (data->flip_y) {
		constants.post_transform[1] += constants.post_transform[3];
		constants.post_transform[3] = -constants.post_transform[3];
	}

	get_color_scale_bias(data, constants.color_scale, constants.color_bias);

	// Update constant buffer
	D3D11_MAPPED_SUBRESOURCE mapped;
	HRESULT hr = sys->context->Map(sys->layer_constant_buffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (SUCCEEDED(hr)) {
		memcpy(mapped.pData, &constants, sizeof(constants));
		sys->context->Unmap(sys->layer_constant_buffer.get(), 0);
	}

	// Set shaders
	sys->context->VSSetShader(sys->quad_vs.get(), nullptr, 0);
	sys->context->PSSetShader(sys->quad_ps.get(), nullptr, 0);

	// Bind resources
	ID3D11Buffer *cbs[] = {sys->layer_constant_buffer.get()};
	sys->context->VSSetConstantBuffers(0, 1, cbs);
	sys->context->PSSetConstantBuffers(0, 1, cbs);
	sys->context->PSSetShaderResources(0, 1, &srv);
	ID3D11SamplerState *samplers[] = {sys->sampler_linear.get()};
	sys->context->PSSetSamplers(0, 1, samplers);

	// Set blend state
	set_blend_state(sys, data);

	// Draw quad (triangle strip, 4 vertices)
	sys->context->Draw(4, 0);

	// Unbind SRV
	ID3D11ShaderResourceView *null_srv = nullptr;
	sys->context->PSSetShaderResources(0, 1, &null_srv);
}

static void
render_cylinder_layer(struct d3d11_service_system *sys,
                      const struct comp_layer *layer,
                      uint32_t view_index,
                      const struct xrt_pose *view_pose,
                      const struct xrt_fov *fov)
{
	const struct xrt_layer_data *data = &layer->data;
	const struct xrt_layer_cylinder_data *cyl = &data->cylinder;

	// Get swapchain
	struct xrt_swapchain *xsc = layer->sc_array[0];
	if (xsc == nullptr) {
		return;
	}
	struct d3d11_service_swapchain *sc = d3d11_service_swapchain_from_xrt(xsc);

	uint32_t image_index = cyl->sub.image_index;
	if (image_index >= sc->image_count) {
		return;
	}

	ID3D11ShaderResourceView *srv = sc->images[image_index].srv.get();
	if (srv == nullptr) {
		return;
	}

	// Build MVP matrix (cylinder is in layer pose space)
	struct xrt_matrix_4x4 model, view, proj, mv, mvp;

	// Model: just the layer pose (cylinder geometry generated in shader)
	struct xrt_vec3 scale = {1.0f, 1.0f, 1.0f};
	math_matrix_4x4_model(&cyl->pose, &scale, &model);

	// View matrix
	math_matrix_4x4_view_from_pose(view_pose, &view);

	// Projection matrix
	math_matrix_4x4_projection_vulkan_infinite_reverse(fov, 0.1f, &proj);

	// MVP
	math_matrix_4x4_multiply(&view, &model, &mv);
	math_matrix_4x4_multiply(&proj, &mv, &mvp);

	// Fill constant buffer
	CylinderLayerConstants constants = {};
	memcpy(constants.mvp, &mvp, sizeof(constants.mvp));

	// UV transform
	constants.post_transform[0] = cyl->sub.norm_rect.x;
	constants.post_transform[1] = cyl->sub.norm_rect.y;
	constants.post_transform[2] = cyl->sub.norm_rect.w;
	constants.post_transform[3] = cyl->sub.norm_rect.h;

	if (data->flip_y) {
		constants.post_transform[1] += constants.post_transform[3];
		constants.post_transform[3] = -constants.post_transform[3];
	}

	get_color_scale_bias(data, constants.color_scale, constants.color_bias);

	constants.radius = cyl->radius;
	constants.central_angle = cyl->central_angle;
	constants.aspect_ratio = cyl->aspect_ratio;

	// Update constant buffer
	D3D11_MAPPED_SUBRESOURCE mapped;
	HRESULT hr = sys->context->Map(sys->layer_constant_buffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (SUCCEEDED(hr)) {
		memcpy(mapped.pData, &constants, sizeof(constants));
		sys->context->Unmap(sys->layer_constant_buffer.get(), 0);
	}

	// Set shaders
	sys->context->VSSetShader(sys->cylinder_vs.get(), nullptr, 0);
	sys->context->PSSetShader(sys->cylinder_ps.get(), nullptr, 0);

	// Bind resources
	ID3D11Buffer *cbs[] = {sys->layer_constant_buffer.get()};
	sys->context->VSSetConstantBuffers(0, 1, cbs);
	sys->context->PSSetConstantBuffers(0, 1, cbs);
	sys->context->PSSetShaderResources(0, 1, &srv);
	ID3D11SamplerState *samplers[] = {sys->sampler_linear.get()};
	sys->context->PSSetSamplers(0, 1, samplers);

	// Set blend state
	set_blend_state(sys, data);

	// Draw cylinder (triangle strip, 2 * (subdivision + 2) vertices)
	// Subdivision count of 64, so 132 vertices
	sys->context->Draw(132, 0);

	// Unbind SRV
	ID3D11ShaderResourceView *null_srv = nullptr;
	sys->context->PSSetShaderResources(0, 1, &null_srv);
}

static void
render_equirect2_layer(struct d3d11_service_system *sys,
                       const struct comp_layer *layer,
                       uint32_t view_index,
                       const struct xrt_pose *view_pose,
                       const struct xrt_fov *fov)
{
	const struct xrt_layer_data *data = &layer->data;
	const struct xrt_layer_equirect2_data *eq = &data->equirect2;

	// Get swapchain
	struct xrt_swapchain *xsc = layer->sc_array[0];
	if (xsc == nullptr) {
		return;
	}
	struct d3d11_service_swapchain *sc = d3d11_service_swapchain_from_xrt(xsc);

	uint32_t image_index = eq->sub.image_index;
	if (image_index >= sc->image_count) {
		return;
	}

	ID3D11ShaderResourceView *srv = sc->images[image_index].srv.get();
	if (srv == nullptr) {
		return;
	}

	// Build inverse model-view matrix (for ray casting)
	struct xrt_matrix_4x4 model, view, mv, mv_inv;

	// Model: layer pose
	struct xrt_vec3 scale = {1.0f, 1.0f, 1.0f};
	math_matrix_4x4_model(&eq->pose, &scale, &model);

	// View matrix
	math_matrix_4x4_view_from_pose(view_pose, &view);

	// MV and inverse
	math_matrix_4x4_multiply(&view, &model, &mv);
	math_matrix_4x4_inverse(&mv, &mv_inv);

	// Calculate UV to tangent transform
	float to_tangent[4];
	to_tangent[0] = tanf(fov->angle_left);
	to_tangent[1] = tanf(fov->angle_down);
	to_tangent[2] = tanf(fov->angle_right) - tanf(fov->angle_left);
	to_tangent[3] = tanf(fov->angle_up) - tanf(fov->angle_down);

	// Fill constant buffer
	Equirect2LayerConstants constants = {};
	memcpy(constants.mv_inverse, &mv_inv, sizeof(constants.mv_inverse));

	// UV transform
	constants.post_transform[0] = eq->sub.norm_rect.x;
	constants.post_transform[1] = eq->sub.norm_rect.y;
	constants.post_transform[2] = eq->sub.norm_rect.w;
	constants.post_transform[3] = eq->sub.norm_rect.h;

	if (data->flip_y) {
		constants.post_transform[1] += constants.post_transform[3];
		constants.post_transform[3] = -constants.post_transform[3];
	}

	get_color_scale_bias(data, constants.color_scale, constants.color_bias);

	memcpy(constants.to_tangent, to_tangent, sizeof(constants.to_tangent));

	// Handle infinite radius (spec says +INFINITY)
	constants.radius = std::isinf(eq->radius) ? 0.0f : eq->radius;
	constants.central_horizontal_angle = eq->central_horizontal_angle;
	constants.upper_vertical_angle = eq->upper_vertical_angle;
	constants.lower_vertical_angle = eq->lower_vertical_angle;

	// Update constant buffer
	D3D11_MAPPED_SUBRESOURCE mapped;
	HRESULT hr = sys->context->Map(sys->layer_constant_buffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (SUCCEEDED(hr)) {
		memcpy(mapped.pData, &constants, sizeof(constants));
		sys->context->Unmap(sys->layer_constant_buffer.get(), 0);
	}

	// Set shaders
	sys->context->VSSetShader(sys->equirect2_vs.get(), nullptr, 0);
	sys->context->PSSetShader(sys->equirect2_ps.get(), nullptr, 0);

	// Bind resources
	ID3D11Buffer *cbs[] = {sys->layer_constant_buffer.get()};
	sys->context->VSSetConstantBuffers(0, 1, cbs);
	sys->context->PSSetConstantBuffers(0, 1, cbs);
	sys->context->PSSetShaderResources(0, 1, &srv);
	ID3D11SamplerState *samplers[] = {sys->sampler_linear.get()};
	sys->context->PSSetSamplers(0, 1, samplers);

	// Set blend state
	set_blend_state(sys, data);

	// Draw fullscreen quad
	sys->context->Draw(4, 0);

	// Unbind SRV
	ID3D11ShaderResourceView *null_srv = nullptr;
	sys->context->PSSetShaderResources(0, 1, &null_srv);
}


/*
 *
 * Per-client render resource management
 *
 */

/*!
 * Clean up per-client render resources.
 */
static void
fini_client_render_resources(struct d3d11_client_render_resources *res)
{
	if (res == nullptr) {
		return;
	}

	// Clean up HUD resources
	res->hud_texture.reset();
	u_hud_destroy(&res->hud);

	// Auto-switch to 2D mode before destroying display processor
	if (res->display_processor != nullptr) {
		xrt_display_processor_d3d11_request_display_mode(
		    res->display_processor, false);
	}
	xrt_display_processor_d3d11_destroy(&res->display_processor);

	res->back_buffer_rtv.reset();
	res->crop_srv.reset();
	res->crop_texture.reset();
	res->crop_width = 0;
	res->crop_height = 0;
	res->atlas_rtv.reset();
	res->atlas_srv.reset();
	res->atlas_srv_srgb.reset();
	res->atlas_texture.reset();
	res->swap_chain.reset();

	if (res->owns_window && res->window != nullptr) {
		comp_d3d11_window_destroy(&res->window);
	}
	res->window = nullptr;
	res->hwnd = nullptr;
	res->owns_window = false;
}

/*!
 * Initialize per-client render resources.
 *
 * @param sys The system compositor (provides device, dimensions)
 * @param external_hwnd External window handle from XR_EXT_win32_window_binding, or NULL
 * @param xsysd System devices for qwerty input (may be NULL)
 * @param res Output render resources struct
 * @return XRT_SUCCESS on success
 */
static xrt_result_t
init_client_render_resources(struct d3d11_service_system *sys,
                              void *external_hwnd,
                              struct xrt_system_devices *xsysd,
                              struct d3d11_client_render_resources *res)
{
	std::memset(res, 0, sizeof(*res));

	HRESULT hr;

	// Workspace mode: only create atlas texture. No window, swap chain, or DP.
	// The multi-compositor owns those shared resources.
	// Atlas sized to native display (app HWND is fullscreen, renders at native * scale).
	if (sys->workspace_mode) {
		uint32_t atlas_w = sys->base.info.display_pixel_width;
		uint32_t atlas_h = sys->base.info.display_pixel_height;
		if (atlas_w == 0 || atlas_h == 0) {
			atlas_w = sys->display_width;
			atlas_h = sys->display_height;
		}
		// Storage is TYPELESS so we can create both UNORM and UNORM_SRGB
		// SRVs onto the same bytes. The UNORM SRV is for clients whose
		// swapchain bytes are already linear (handle apps, UNORM legacy
		// WebXR); the SRGB SRV is for clients whose swapchain bytes are
		// gamma-encoded (SRGB swapchains, e.g. Chrome/Three.js with
		// outputColorSpace=SRGBColorSpace) so multi_compositor_render's
		// passthrough-blit reads linear values. Without TYPELESS storage,
		// CreateShaderResourceView with a different sub-format than the
		// resource format returns E_INVALIDARG (D3D11 cross-format-view
		// rule).
		D3D11_TEXTURE2D_DESC atlas_desc = {};
		atlas_desc.Width = atlas_w;
		atlas_desc.Height = atlas_h;
		atlas_desc.MipLevels = 1;
		atlas_desc.ArraySize = 1;
		atlas_desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
		atlas_desc.SampleDesc.Count = 1;
		atlas_desc.Usage = D3D11_USAGE_DEFAULT;
		atlas_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

		hr = sys->device->CreateTexture2D(&atlas_desc, nullptr, res->atlas_texture.put());
		if (FAILED(hr)) {
			U_LOG_E("Workspace mode: failed to create atlas texture (hr=0x%08X)", hr);
			return XRT_ERROR_D3D11;
		}

		// Default UNORM SRV — for linear-byte content.
		D3D11_SHADER_RESOURCE_VIEW_DESC unorm_srv_desc = {};
		unorm_srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		unorm_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		unorm_srv_desc.Texture2D.MipLevels = 1;
		unorm_srv_desc.Texture2D.MostDetailedMip = 0;
		sys->device->CreateShaderResourceView(
		    res->atlas_texture.get(), &unorm_srv_desc, res->atlas_srv.put());

		// Parallel SRGB SRV — for gamma-encoded-byte content. Selected
		// at sample time by multi_compositor_render based on the
		// per-client `atlas_holds_srgb_bytes` flag.
		D3D11_SHADER_RESOURCE_VIEW_DESC srgb_srv_desc = {};
		srgb_srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		srgb_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srgb_srv_desc.Texture2D.MipLevels = 1;
		srgb_srv_desc.Texture2D.MostDetailedMip = 0;
		sys->device->CreateShaderResourceView(
		    res->atlas_texture.get(), &srgb_srv_desc, res->atlas_srv_srgb.put());

		// RTV stays UNORM — runtime-side blits write raw bytes (no
		// auto-encode); the source bytes' color space is tracked
		// separately and resolved on read.
		D3D11_RENDER_TARGET_VIEW_DESC unorm_rtv_desc = {};
		unorm_rtv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		unorm_rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		unorm_rtv_desc.Texture2D.MipSlice = 0;
		sys->device->CreateRenderTargetView(
		    res->atlas_texture.get(), &unorm_rtv_desc, res->atlas_rtv.put());

		U_LOG_W("Workspace mode: created atlas-only resources for client (%ux%u)",
		        atlas_w, atlas_h);
		return XRT_SUCCESS;
	}

	// Get or create window
	if (external_hwnd != nullptr) {
		// Use app-provided window (XR_EXT_win32_window_binding)
		res->hwnd = (HWND)external_hwnd;
		res->owns_window = false;
		res->window = nullptr;
		U_LOG_W("Using external window handle: %p", external_hwnd);
	} else {
		// Create our own window (IPC/WebXR path)
		xrt_result_t wret = comp_d3d11_window_create(sys->output_width, sys->output_height, &res->window);
		if (wret != XRT_SUCCESS || res->window == nullptr) {
			U_LOG_E("Failed to create window for client");
			return XRT_ERROR_VULKAN;
		}
		res->hwnd = (HWND)comp_d3d11_window_get_hwnd(res->window);
		res->owns_window = true;

		// Pass system devices to window for qwerty input support
		if (xsysd != nullptr) {
			comp_d3d11_window_set_system_devices(res->window, xsysd);
			U_LOG_W("Passed xsysd to client window for qwerty input");
		}

		U_LOG_W("Created window for client: hwnd=%p (%ux%u)", res->hwnd, sys->output_width, sys->output_height);
	}

	// Track compositor HWND so the WebXR bridge can push per-view tile dims
	// via SetPropW(DXR_BridgeViewW/H) and request mode changes via DXR_RequestMode.
	if (res->hwnd != nullptr && sys->compositor_hwnd == nullptr) {
		sys->compositor_hwnd = res->hwnd;
	}

	// Get actual window client area (may differ from requested size if window
	// went fullscreen to native monitor resolution during creation)
	uint32_t actual_width = sys->output_width;
	uint32_t actual_height = sys->output_height;
	if (res->hwnd != nullptr) {
		RECT client_rect;
		if (GetClientRect(res->hwnd, &client_rect)) {
			uint32_t cw = static_cast<uint32_t>(client_rect.right - client_rect.left);
			uint32_t ch = static_cast<uint32_t>(client_rect.bottom - client_rect.top);
			if (cw > 0 && ch > 0) {
				actual_width = cw;
				actual_height = ch;
				if (cw != sys->output_width || ch != sys->output_height) {
					U_LOG_W("Window actual size differs from defaults: %ux%u (was %ux%u)",
					        cw, ch, sys->output_width, sys->output_height);
				}
			}
		}
	}

	// Create HUD overlay for runtime-owned windows
	if (res->owns_window) {
		res->smoothed_frame_time_ms = 16.67f;
		u_hud_create(&res->hud, actual_width);
	}

	// Create swap chain at actual window size (not defaults)
	DXGI_SWAP_CHAIN_DESC1 sc_desc = {};
	sc_desc.Width = actual_width;
	sc_desc.Height = actual_height;
	sc_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sc_desc.SampleDesc.Count = 1;
	sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sc_desc.BufferCount = 2;
	sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	// IGNORE so DWM doesn't composite the desktop through the bound HWND (#163).
	sc_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

	hr = sys->dxgi_factory->CreateSwapChainForHwnd(
	    sys->device.get(),
	    res->hwnd,
	    &sc_desc,
	    nullptr,
	    nullptr,
	    res->swap_chain.put());

	if (FAILED(hr)) {
		U_LOG_E("Failed to create swap chain for client: 0x%08lx", hr);
		fini_client_render_resources(res);
		return XRT_ERROR_VULKAN;
	}

	// Get back buffer RTV
	wil::com_ptr<ID3D11Texture2D> back_buffer;
	res->swap_chain->GetBuffer(0, IID_PPV_ARGS(back_buffer.put()));
	sys->device->CreateRenderTargetView(back_buffer.get(), nullptr, res->back_buffer_rtv.put());

	// Create stereo render target texture (side-by-side views)
	D3D11_TEXTURE2D_DESC atlas_desc = {};
	atlas_desc.Width = sys->display_width;
	atlas_desc.Height = sys->display_height;
	atlas_desc.MipLevels = 1;
	atlas_desc.ArraySize = 1;
	atlas_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	atlas_desc.SampleDesc.Count = 1;
	atlas_desc.Usage = D3D11_USAGE_DEFAULT;
	atlas_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

	hr = sys->device->CreateTexture2D(&atlas_desc, nullptr, res->atlas_texture.put());
	if (FAILED(hr)) {
		U_LOG_E("Failed to create atlas texture for client: 0x%08lx", hr);
		fini_client_render_resources(res);
		return XRT_ERROR_VULKAN;
	}

	// Create SRV for stereo texture
	hr = sys->device->CreateShaderResourceView(res->atlas_texture.get(), nullptr, res->atlas_srv.put());
	if (FAILED(hr)) {
		U_LOG_E("Failed to create atlas SRV for client: 0x%08lx", hr);
		fini_client_render_resources(res);
		return XRT_ERROR_VULKAN;
	}

	// Create RTV for stereo texture
	hr = sys->device->CreateRenderTargetView(res->atlas_texture.get(), nullptr, res->atlas_rtv.put());
	if (FAILED(hr)) {
		U_LOG_E("Failed to create atlas RTV for client: 0x%08lx", hr);
		fini_client_render_resources(res);
		return XRT_ERROR_VULKAN;
	}

	U_LOG_W("Created stereo render target for client (%ux%u)", sys->display_width, sys->display_height);

	// Create display processor via factory (set by the target builder at init time).
	// Phase 6.1 (#140): skip per-client DP creation when workspace mode is active.
	// The multi-compositor already owns a shared DP for the combined atlas;
	// creating a SECOND DP instance causes the SR SDK to recalibrate its
	// weaver, producing a multi-second stretched-left-eye artifact. The
	// per-client DP is only needed for standalone (non-workspace) rendering.
	if (sys->base.info.dp_factory_d3d11 != NULL && !sys->workspace_mode) {
		auto factory = (xrt_dp_factory_d3d11_fn_t)sys->base.info.dp_factory_d3d11;
		xrt_result_t dp_ret = factory(sys->device.get(), sys->context.get(), res->hwnd, &res->display_processor);
		if (dp_ret != XRT_SUCCESS) {
			U_LOG_W("D3D11 display processor factory failed (error %d), continuing without",
			        (int)dp_ret);
			res->display_processor = nullptr;
		} else {
			U_LOG_W("D3D11 display processor created via factory for client");
			// Phase 6.1 (#140): don't call request_display_mode(true)
			// here — the SR SDK's recalibration cycle causes a multi-
			// second stretched-left-eye artifact. Let the DP come up in
			// the current mode; V key and xrRequestDisplayRenderingModeEXT
			// remain the authoritative mode-switch triggers.

			// Query display pixel info from the real (windowed) display processor.
			// The temp DP at system init uses NULL window and may fail to return
			// pixel info, leaving sys dimensions at 1920x1080 defaults.  Update now.
			uint32_t dp_px_w = 0, dp_px_h = 0;
			int32_t dp_left = 0, dp_top = 0;
			if (xrt_display_processor_d3d11_get_display_pixel_info(
			        res->display_processor, &dp_px_w, &dp_px_h,
			        &dp_left, &dp_top) &&
			    dp_px_w > 0 && dp_px_h > 0 &&
			    (dp_px_w != sys->output_width || dp_px_h != sys->output_height)) {
				U_LOG_W("Updating dims from display processor: %ux%u -> %ux%u",
				        sys->output_width, sys->output_height, dp_px_w, dp_px_h);
				sync_tile_layout(sys);
				sys->output_width = dp_px_w;
				sys->output_height = dp_px_h;
				sys->view_width = dp_px_w / sys->tile_columns;
				sys->view_height = dp_px_h / sys->tile_rows;
				sys->display_width = sys->tile_columns * sys->view_width;
				sys->display_height = sys->tile_rows * sys->view_height;

				// Recreate stereo texture at correct dimensions
				res->atlas_rtv.reset();
				res->atlas_srv.reset();
				res->atlas_srv_srgb.reset();
				res->atlas_texture.reset();

				D3D11_TEXTURE2D_DESC atlas_desc = {};
				atlas_desc.Width = sys->display_width;
				atlas_desc.Height = sys->display_height;
				atlas_desc.MipLevels = 1;
				atlas_desc.ArraySize = 1;
				atlas_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				atlas_desc.SampleDesc.Count = 1;
				atlas_desc.Usage = D3D11_USAGE_DEFAULT;
				atlas_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

				hr = sys->device->CreateTexture2D(&atlas_desc, nullptr,
				                                  res->atlas_texture.put());
				if (SUCCEEDED(hr)) {
					sys->device->CreateShaderResourceView(
					    res->atlas_texture.get(), nullptr, res->atlas_srv.put());
					sys->device->CreateRenderTargetView(
					    res->atlas_texture.get(), nullptr, res->atlas_rtv.put());
					U_LOG_W("Stereo texture recreated at %ux%u",
					        sys->display_width, sys->display_height);
				}

				// Resize window and swap chain to match display
				if (res->owns_window && res->hwnd != nullptr) {
					RECT client_rect;
					if (GetClientRect(res->hwnd, &client_rect)) {
						uint32_t cw = (uint32_t)(client_rect.right - client_rect.left);
						uint32_t ch = (uint32_t)(client_rect.bottom - client_rect.top);
						if (cw != dp_px_w || ch != dp_px_h) {
							// Resize swap chain to match
							res->back_buffer_rtv.reset();
							HRESULT rhr = res->swap_chain->ResizeBuffers(
							    0, dp_px_w, dp_px_h, DXGI_FORMAT_UNKNOWN, 0);
							if (SUCCEEDED(rhr)) {
								wil::com_ptr<ID3D11Texture2D> bb;
								res->swap_chain->GetBuffer(0, IID_PPV_ARGS(bb.put()));
								sys->device->CreateRenderTargetView(
								    bb.get(), nullptr, res->back_buffer_rtv.put());
								U_LOG_W("Swap chain resized to %ux%u to match display",
								        dp_px_w, dp_px_h);
							}
						}
					}
				}
			}
		}
	} else {
		U_LOG_W("No D3D11 display processor factory provided");
		res->display_processor = nullptr;
	}

	U_LOG_W("Client render resources initialized: view=%ux%u/eye, stereo=%ux%u, output=%ux%u",
	        sys->view_width, sys->view_height,
	        sys->display_width, sys->display_height,
	        sys->output_width, sys->output_height);

	return XRT_SUCCESS;
}


/*
 *
 * Swapchain functions
 *
 */

static void
swapchain_destroy(struct xrt_swapchain *xsc)
{
	struct d3d11_service_swapchain *sc = d3d11_service_swapchain_from_xrt(xsc);

	// Release all images
	for (uint32_t i = 0; i < sc->image_count; i++) {
		if (sc->images[i].mutex_acquired && sc->images[i].keyed_mutex) {
			sc->images[i].keyed_mutex->ReleaseSync(0);
		}
		sc->images[i].srv.reset();
		sc->images[i].keyed_mutex.reset();
		sc->images[i].texture.reset();

		// Close NT handles for service-created swapchains
		if (sc->service_created && sc->base.images[i].handle != nullptr) {
			CloseHandle((HANDLE)sc->base.images[i].handle);
			sc->base.images[i].handle = nullptr;
		}
	}

	delete sc;
}

static xrt_result_t
swapchain_acquire_image(struct xrt_swapchain *xsc, uint32_t *out_index)
{
	struct d3d11_service_swapchain *sc = d3d11_service_swapchain_from_xrt(xsc);

	// Simple round-robin for now
	static uint32_t next_index = 0;
	*out_index = next_index % sc->image_count;
	next_index++;

	U_LOG_W("[#151] d3d11_service swapchain_acquire_image: index=%u image_count=%u "
	        "(w=%u h=%u format=%lld bits=0x%x service_created=%d)",
	        *out_index, sc->image_count, sc->info.width, sc->info.height,
	        (long long)sc->info.format, (unsigned)sc->info.bits, (int)sc->service_created);

	return XRT_SUCCESS;
}

static xrt_result_t
swapchain_inc_image_use(struct xrt_swapchain *xsc, uint32_t index)
{
	// No-op for service compositor
	return XRT_SUCCESS;
}

static xrt_result_t
swapchain_dec_image_use(struct xrt_swapchain *xsc, uint32_t index)
{
	// No-op for service compositor
	return XRT_SUCCESS;
}

static xrt_result_t
swapchain_wait_image(struct xrt_swapchain *xsc, int64_t timeout_ns, uint32_t index)
{
	struct d3d11_service_swapchain *sc = d3d11_service_swapchain_from_xrt(xsc);

	if (index >= sc->image_count) {
		return XRT_ERROR_NO_IMAGE_AVAILABLE;
	}

	struct d3d11_service_image *img = &sc->images[index];

	// For server-created swapchains (WebXR), the CLIENT owns the KeyedMutex
	// during wait_image/release_image. The client handles mutex synchronization
	// directly in comp_d3d11_client.cpp. Server only acquires mutex later when
	// it needs to read the texture for composition (in layer_commit).
	if (sc->service_created) {
		// Server-created: client handles mutex, just return success
		return XRT_SUCCESS;
	}

	// For client-created swapchains (imported), server acquires mutex here
	if (img->keyed_mutex && !img->mutex_acquired) {
		// Convert timeout to milliseconds
		DWORD timeout_ms = (timeout_ns < 0) ? INFINITE : static_cast<DWORD>(timeout_ns / 1000000);

		HRESULT hr = img->keyed_mutex->AcquireSync(0, timeout_ms);
		if (hr == WAIT_TIMEOUT) {
			return XRT_ERROR_NO_IMAGE_AVAILABLE;
		}
		if (FAILED(hr)) {
			U_LOG_E("KeyedMutex AcquireSync failed: 0x%08lx", hr);
			return XRT_ERROR_NO_IMAGE_AVAILABLE;
		}
		img->mutex_acquired = true;
	}

	return XRT_SUCCESS;
}

static xrt_result_t
swapchain_release_image(struct xrt_swapchain *xsc, uint32_t index)
{
	struct d3d11_service_swapchain *sc = d3d11_service_swapchain_from_xrt(xsc);

	if (index >= sc->image_count) {
		return XRT_ERROR_NO_IMAGE_AVAILABLE;
	}

	struct d3d11_service_image *img = &sc->images[index];

	// For server-created swapchains (WebXR), the CLIENT owns the KeyedMutex
	// during wait_image/release_image. The client handles mutex release
	// directly in comp_d3d11_client.cpp.
	if (sc->service_created) {
		// Server-created: client handles mutex, just return success
		return XRT_SUCCESS;
	}

	// For client-created swapchains (imported), server releases mutex here
	if (img->keyed_mutex && img->mutex_acquired) {
		img->keyed_mutex->ReleaseSync(0);
		img->mutex_acquired = false;
	}

	return XRT_SUCCESS;
}

static xrt_result_t
swapchain_barrier_image(struct xrt_swapchain *xsc, enum xrt_barrier_direction direction, uint32_t index)
{
	struct d3d11_service_swapchain *sc = d3d11_service_swapchain_from_xrt(xsc);

	if (index >= sc->image_count) {
		return XRT_ERROR_NO_IMAGE_AVAILABLE;
	}

	// D3D11 service compositor: KeyedMutex handles synchronization
	// No additional barrier needed since AcquireSync/ReleaseSync on
	// the KeyedMutex already provides the necessary synchronization
	// between client and service processes.
	(void)direction;

	return XRT_SUCCESS;
}


/*
 *
 * Semaphore functions
 *
 */

static xrt_result_t
semaphore_wait(struct xrt_compositor_semaphore *xcsem, uint64_t value, uint64_t timeout_ns)
{
	struct d3d11_service_semaphore *sem = d3d11_service_semaphore_from_xrt(xcsem);

	// Convert nanoseconds to milliseconds
	auto timeout_ms = std::chrono::milliseconds(timeout_ns / 1000000);

	return xrt::auxiliary::d3d::d3d11::waitOnFenceWithTimeout(
	    sem->fence, sem->wait_event, value, timeout_ms);
}

static void
semaphore_destroy(struct xrt_compositor_semaphore *xcsem)
{
	struct d3d11_service_semaphore *sem = d3d11_service_semaphore_from_xrt(xcsem);

	sem->wait_event.reset();
	sem->fence.reset();
	delete sem;
}


/*
 *
 * Native compositor functions
 *
 */

static xrt_result_t
compositor_get_swapchain_create_properties(struct xrt_compositor *xc,
                                            const struct xrt_swapchain_create_info *info,
                                            struct xrt_swapchain_create_properties *xsccp)
{
	xsccp->image_count = 1;  // Single buffer like SR Hydra for WebXR compatibility
	xsccp->extra_bits = (enum xrt_swapchain_usage_bits)0;
	return XRT_SUCCESS;
}

/*!
 * Convert XRT format to DXGI format.
 */
static DXGI_FORMAT
xrt_format_to_dxgi(int64_t format)
{
	// Check if this is already a DXGI format (common D3D11 formats are < 130)
	switch (format) {
	// Pass through DXGI formats directly
	case DXGI_FORMAT_R8G8B8A8_UNORM:        // 28
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:   // 29
	case DXGI_FORMAT_B8G8R8A8_UNORM:        // 87
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:   // 91
	case DXGI_FORMAT_R16G16B16A16_FLOAT:    // 10
	case DXGI_FORMAT_R16G16B16A16_UNORM:    // 11
	case DXGI_FORMAT_D24_UNORM_S8_UINT:     // 45
	case DXGI_FORMAT_D32_FLOAT:             // 40
	case DXGI_FORMAT_D16_UNORM:             // 55
	case DXGI_FORMAT_R10G10B10A2_UNORM:     // 24
		return static_cast<DXGI_FORMAT>(format);

	// Convert VK_FORMAT values to DXGI (for Vulkan compositor interop)
	case 37: // VK_FORMAT_R8G8B8A8_UNORM
		return DXGI_FORMAT_R8G8B8A8_UNORM;
	case 43: // VK_FORMAT_R8G8B8A8_SRGB
		return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	case 44: // VK_FORMAT_B8G8R8A8_UNORM
		return DXGI_FORMAT_B8G8R8A8_UNORM;
	case 50: // VK_FORMAT_B8G8R8A8_SRGB
		return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	case 64: // VK_FORMAT_A2B10G10R10_UNORM_PACK32
		return DXGI_FORMAT_R10G10B10A2_UNORM;
	case 97: // VK_FORMAT_R16G16B16A16_SFLOAT
		return DXGI_FORMAT_R16G16B16A16_FLOAT;
	case 129: // VK_FORMAT_D24_UNORM_S8_UINT
		return DXGI_FORMAT_D24_UNORM_S8_UINT;
	case 130: // VK_FORMAT_D32_SFLOAT
		return DXGI_FORMAT_D32_FLOAT;

	default:
		U_LOG_W("Unknown format %ld, using RGBA8", format);
		return DXGI_FORMAT_R8G8B8A8_UNORM;
	}
}

static xrt_result_t
compositor_create_swapchain(struct xrt_compositor *xc,
                             const struct xrt_swapchain_create_info *info,
                             struct xrt_swapchain **out_xsc)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);
	struct d3d11_service_system *sys = c->sys;

	// Strip protected content flag — not needed for service-side shared textures.
	// D3D12 client rejects this flag, but it's meaningless for workspace mode.
	struct xrt_swapchain_create_info local_info = *info;
	local_info.create = (enum xrt_swapchain_create_flags)(local_info.create & ~XRT_SWAPCHAIN_CREATE_PROTECTED_CONTENT);
	info = &local_info;

	// Use single buffer like SR Hydra for WebXR compatibility
	uint32_t image_count = 1;
	if (image_count > XRT_MAX_SWAPCHAIN_IMAGES) {
		image_count = XRT_MAX_SWAPCHAIN_IMAGES;
	}

	U_LOG_W("Creating swapchain: %u images, %ux%u, format=%u, usage=0x%x",
	        image_count, info->width, info->height, info->format, info->bits);
	U_LOG_W("[#151] d3d11_service create_swapchain: arraySize=%u mipCount=%u sampleCount=%u "
	        "faceCount=%u create=0x%x MUTABLE_FORMAT=%s SAMPLED=%s COLOR=%s DEPTH_STENCIL=%s",
	        info->array_size, info->mip_count, info->sample_count, info->face_count,
	        (unsigned)info->create,
	        (info->bits & XRT_SWAPCHAIN_USAGE_MUTABLE_FORMAT) ? "YES" : "no",
	        (info->bits & XRT_SWAPCHAIN_USAGE_SAMPLED) ? "YES" : "no",
	        (info->bits & XRT_SWAPCHAIN_USAGE_COLOR) ? "YES" : "no",
	        (info->bits & XRT_SWAPCHAIN_USAGE_DEPTH_STENCIL) ? "YES" : "no");

	// Allocate swapchain
	struct d3d11_service_swapchain *sc = new d3d11_service_swapchain();
	std::memset(sc, 0, sizeof(*sc));

	sc->base.base.destroy = swapchain_destroy;
	sc->base.base.acquire_image = swapchain_acquire_image;
	sc->base.base.inc_image_use = swapchain_inc_image_use;
	sc->base.base.dec_image_use = swapchain_dec_image_use;
	sc->base.base.wait_image = swapchain_wait_image;
	sc->base.base.barrier_image = swapchain_barrier_image;
	sc->base.base.release_image = swapchain_release_image;
	sc->base.base.reference.count = 1;
	sc->base.base.image_count = image_count;

	sc->comp = c;
	sc->image_count = image_count;
	sc->info = *info;
	sc->service_created = true; // Created by service for client

	// Convert format
	DXGI_FORMAT dxgi_format = xrt_format_to_dxgi(info->format);

	// Determine bind flags
	UINT bind_flags = D3D11_BIND_SHADER_RESOURCE; // Always need SRV for compositor
	if (info->bits & XRT_SWAPCHAIN_USAGE_COLOR) {
		bind_flags |= D3D11_BIND_RENDER_TARGET;
	}
	if (info->bits & XRT_SWAPCHAIN_USAGE_DEPTH_STENCIL) {
		bind_flags |= D3D11_BIND_DEPTH_STENCIL;
	}
	if (info->bits & XRT_SWAPCHAIN_USAGE_UNORDERED_ACCESS) {
		bind_flags |= D3D11_BIND_UNORDERED_ACCESS;
	}

	// Create texture descriptor with SHARED_KEYEDMUTEX for cross-process sharing
	D3D11_TEXTURE2D_DESC tex_desc = {};
	tex_desc.Width = info->width;
	tex_desc.Height = info->height;
	tex_desc.MipLevels = info->mip_count > 0 ? info->mip_count : 1;
	tex_desc.ArraySize = info->array_size > 0 ? info->array_size : 1;
	tex_desc.Format = dxgi_format;
	tex_desc.SampleDesc.Count = info->sample_count > 0 ? info->sample_count : 1;
	tex_desc.SampleDesc.Quality = 0;
	tex_desc.Usage = D3D11_USAGE_DEFAULT;
	tex_desc.BindFlags = bind_flags;
	tex_desc.CPUAccessFlags = 0;
	// SHARED_KEYEDMUTEX enables cross-process sharing with synchronization
	// SHARED_NTHANDLE creates real kernel handles that can be DuplicateHandle'd to Chrome
	tex_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

	// Create textures and get shared handles
	for (uint32_t i = 0; i < image_count; i++) {
		HRESULT hr = sys->device->CreateTexture2D(&tex_desc, nullptr, sc->images[i].texture.put());
		if (FAILED(hr)) {
			U_LOG_E("Failed to create shared texture [%u]: 0x%08lx", i, hr);
			swapchain_destroy(&sc->base.base);
			return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
		}

		// Get KeyedMutex for synchronization
		hr = sc->images[i].texture->QueryInterface(IID_PPV_ARGS(sc->images[i].keyed_mutex.put()));
		if (FAILED(hr)) {
			U_LOG_E("Texture has no KeyedMutex [%u]: 0x%08lx", i, hr);
			swapchain_destroy(&sc->base.base);
			return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
		}

		// Get NT handle via IDXGIResource1::CreateSharedHandle
		// NT handles are real kernel handles that can be DuplicateHandle'd to Chrome's process
		wil::com_ptr<IDXGIResource1> dxgi_resource1;
		hr = sc->images[i].texture->QueryInterface(IID_PPV_ARGS(dxgi_resource1.put()));
		if (FAILED(hr)) {
			U_LOG_E("Failed to get IDXGIResource1 [%u]: 0x%08lx", i, hr);
			swapchain_destroy(&sc->base.base);
			return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
		}

		// Create security attributes for AppContainer sharing
		SECURITY_ATTRIBUTES sa = {};
		PSECURITY_DESCRIPTOR sd = nullptr;
		create_appcontainer_sa(sa, sd);

		HANDLE shared_handle = nullptr;
		hr = dxgi_resource1->CreateSharedHandle(
		    &sa, // AppContainer security
		    DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
		    nullptr, // no name
		    &shared_handle);

		if (sd) LocalFree(sd);
		if (FAILED(hr) || shared_handle == nullptr) {
			U_LOG_E("Failed to create NT shared handle [%u]: 0x%08lx", i, hr);
			swapchain_destroy(&sc->base.base);
			return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
		}

		// Store NT handle - DO NOT set bit 0, allowing IPC to DuplicateHandle to client
		sc->base.images[i].handle = (xrt_graphics_buffer_handle_t)shared_handle;
		// Calculate texture memory size for cross-API import validation (VK needs this).
		// VK's vkGetImageMemoryRequirements adds row pitch and page alignment padding,
		// so we round up to 1MB to ensure our reported size >= VK's requirements.
		uint32_t bpp = dxgi_format_bytes_per_pixel(tex_desc.Format);
		uint64_t raw_size = (uint64_t)tex_desc.Width * tex_desc.Height * bpp * tex_desc.ArraySize;
		sc->base.images[i].size = (raw_size + 0xFFFFF) & ~(uint64_t)0xFFFFF;
		sc->base.images[i].use_dedicated_allocation = false;
		sc->base.images[i].is_dxgi_handle = false; // NT handle, use OpenSharedResource1

		U_LOG_W("Created shared texture [%u]: handle=%p (NT handle)", i, shared_handle);

		// Create SRV for compositor
		D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
		srv_desc.Format = dxgi_format;
		srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv_desc.Texture2D.MipLevels = tex_desc.MipLevels;

		hr = sys->device->CreateShaderResourceView(
		    sc->images[i].texture.get(), &srv_desc, sc->images[i].srv.put());
		if (FAILED(hr)) {
			U_LOG_E("Failed to create SRV [%u]: 0x%08lx", i, hr);
			swapchain_destroy(&sc->base.base);
			return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
		}
	}

	U_LOG_W("Created swapchain with %u shared images (%ux%u, format=%d)",
	        image_count, info->width, info->height, (int)dxgi_format);

	// Note: KeyedMutex starts in released state (key 0), so client can acquire immediately.
	// No initial release needed.
	for (uint32_t i = 0; i < image_count; i++) {
		sc->images[i].mutex_acquired = false;
	}

	*out_xsc = &sc->base.base;
	return XRT_SUCCESS;
}

static xrt_result_t
compositor_import_swapchain(struct xrt_compositor *xc,
                             const struct xrt_swapchain_create_info *info,
                             struct xrt_image_native *native_images,
                             uint32_t image_count,
                             struct xrt_swapchain **out_xsc)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);
	struct d3d11_service_system *sys = c->sys;

	if (image_count > XRT_MAX_SWAPCHAIN_IMAGES) {
		U_LOG_E("Too many images: %u > %u", image_count, XRT_MAX_SWAPCHAIN_IMAGES);
		return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
	}

	// Allocate swapchain
	struct d3d11_service_swapchain *sc = new d3d11_service_swapchain();
	std::memset(sc, 0, sizeof(*sc));

	sc->base.base.destroy = swapchain_destroy;
	sc->base.base.acquire_image = swapchain_acquire_image;
	sc->base.base.inc_image_use = swapchain_inc_image_use;
	sc->base.base.dec_image_use = swapchain_dec_image_use;
	sc->base.base.wait_image = swapchain_wait_image;
	sc->base.base.barrier_image = swapchain_barrier_image;
	sc->base.base.release_image = swapchain_release_image;
	sc->base.base.reference.count = 1;

	sc->comp = c;
	sc->image_count = image_count;
	sc->info = *info;
	sc->service_created = false; // Imported from client

	U_LOG_W("Importing swapchain: %u images, %ux%u, format=%u, usage=0x%x",
	        image_count, info->width, info->height, info->format, info->bits);

	// Import each image from the client
	for (uint32_t i = 0; i < image_count; i++) {
		HANDLE handle = native_images[i].handle;

		if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
			U_LOG_E("Invalid handle for image [%u]: %p", i, handle);
			swapchain_destroy(&sc->base.base);
			return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
		}

		// Check for DXGI handle encoding (bit 0 set)
		bool is_dxgi = native_images[i].is_dxgi_handle;
		if ((size_t)handle & 1) {
			handle = (HANDLE)((size_t)handle - 1);
			is_dxgi = true;
		}

		U_LOG_D("Image [%u]: handle=%p, is_dxgi=%d", i, handle, is_dxgi);

		// Open shared resource
		// is_dxgi = true means a legacy DXGI global handle (from IDXGIResource::GetSharedHandle,
		// created without NTHANDLE flag). These are system-wide and don't need DuplicateHandle.
		// is_dxgi = false means an NT handle (from IDXGIResource1::CreateSharedHandle,
		// created with NTHANDLE flag). These require DuplicateHandle across processes.
		HRESULT hr;
		if (is_dxgi) {
			// Legacy DXGI global handle → use OpenSharedResource (ID3D11Device)
			hr = sys->device->OpenSharedResource(handle, IID_PPV_ARGS(sc->images[i].texture.put()));
			if (FAILED(hr)) {
				U_LOG_E("OpenSharedResource (DXGI global handle) failed for image [%u]: 0x%08lx (handle=%p)",
				        i, hr, handle);
			}
		} else {
			// NT handle → use OpenSharedResource1 (ID3D11Device1)
			hr = sys->device->OpenSharedResource1(handle, IID_PPV_ARGS(sc->images[i].texture.put()));
			if (FAILED(hr)) {
				U_LOG_E("OpenSharedResource1 (NT handle) failed for image [%u]: 0x%08lx (handle=%p)",
				        i, hr, handle);
			}
		}

		if (FAILED(hr)) {
			// Log additional diagnostic information
			U_LOG_E("  Swapchain info: %ux%u, format=%u", info->width, info->height, info->format);
			swapchain_destroy(&sc->base.base);
			return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
		}

		U_LOG_W("Image [%u] imported successfully (%s handle)", i, is_dxgi ? "DXGI global" : "NT");

		// Get KeyedMutex for synchronization
		hr = sc->images[i].texture->QueryInterface(IID_PPV_ARGS(sc->images[i].keyed_mutex.put()));
		if (FAILED(hr)) {
			U_LOG_W("Shared texture has no KeyedMutex, synchronization may be unreliable");
		}

		// Create shader resource view
		D3D11_TEXTURE2D_DESC desc;
		sc->images[i].texture->GetDesc(&desc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
		srv_desc.Format = desc.Format;
		srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv_desc.Texture2D.MipLevels = 1;

		hr = sys->device->CreateShaderResourceView(
		    sc->images[i].texture.get(), &srv_desc, sc->images[i].srv.put());

		if (FAILED(hr)) {
			U_LOG_E("Failed to create SRV [%u]: 0x%08lx", i, hr);
			swapchain_destroy(&sc->base.base);
			return XRT_ERROR_SWAPCHAIN_FLAG_VALID_BUT_UNSUPPORTED;
		}
	}

	U_LOG_W("Imported swapchain with %u images (%ux%u)", image_count, info->width, info->height);

	sc->base.base.image_count = image_count;
	*out_xsc = &sc->base.base;
	return XRT_SUCCESS;
}

static xrt_result_t
compositor_import_fence(struct xrt_compositor *xc,
                         xrt_graphics_sync_handle_t handle,
                         struct xrt_compositor_fence **out_xcf)
{
	return XRT_ERROR_FENCE_CREATE_FAILED;
}

static xrt_result_t
compositor_create_semaphore(struct xrt_compositor *xc,
                             xrt_graphics_sync_handle_t *out_handle,
                             struct xrt_compositor_semaphore **out_xcsem)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);
	struct d3d11_service_system *sys = c->sys;

	// Allocate semaphore
	struct d3d11_service_semaphore *sem = new d3d11_service_semaphore();
	std::memset(&sem->base, 0, sizeof(sem->base));

	sem->sys = sys;
	sem->base.reference.count = 1;
	sem->base.wait = semaphore_wait;
	sem->base.destroy = semaphore_destroy;

	// Create the wait event
	sem->wait_event.create();
	if (!sem->wait_event) {
		U_LOG_E("Failed to create wait event for semaphore");
		delete sem;
		return XRT_ERROR_FENCE_CREATE_FAILED;
	}

	// Create security attributes for AppContainer sharing
	SECURITY_ATTRIBUTES sa = {};
	PSECURITY_DESCRIPTOR sd = nullptr;
	create_appcontainer_sa(sa, sd);

	// Create a shared D3D11 fence
	xrt_graphics_sync_handle_t handle = XRT_GRAPHICS_SYNC_HANDLE_INVALID;
	xrt_result_t xret = xrt::auxiliary::d3d::d3d11::createSharedFence(
	    *sys->device.get(),
	    false,  // share_cross_adapter
	    &handle,
	    sem->fence,
	    &sa);

	if (sd) LocalFree(sd);

	if (xret != XRT_SUCCESS) {
		U_LOG_E("Failed to create D3D11 fence for semaphore");
		delete sem;
		return XRT_ERROR_FENCE_CREATE_FAILED;
	}

	U_LOG_I("Created D3D11 compositor semaphore");

	*out_handle = handle;
	*out_xcsem = &sem->base;

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_begin_session(struct xrt_compositor *xc, const struct xrt_begin_session_info *info)
{
	U_LOG_W("D3D11 service compositor: session begin");
	return XRT_SUCCESS;
}

static xrt_result_t
compositor_end_session(struct xrt_compositor *xc)
{
	U_LOG_W("D3D11 service compositor: session end");
	return XRT_SUCCESS;
}

static xrt_result_t
compositor_predict_frame(struct xrt_compositor *xc,
                          int64_t *out_frame_id,
                          int64_t *out_wake_time_ns,
                          int64_t *out_predicted_gpu_time_ns,
                          int64_t *out_predicted_display_time_ns,
                          int64_t *out_predicted_display_period_ns)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	// Failsafe: if window closed and client keeps calling predict_frame
	// without layer_commit, push exit request after a few frames
	if (c->window_closed && c->window_closed_frame_count >= 3) {
		if (!c->exit_request_sent && c->xses != nullptr) {
			U_LOG_W("Window closed failsafe: %u frames since close, requesting session exit",
			        c->window_closed_frame_count);
			union xrt_session_event xse = XRT_STRUCT_INIT;
			xse.type = XRT_SESSION_EVENT_EXIT_REQUEST;
			xrt_session_event_sink_push(c->xses, &xse);
			c->exit_request_sent = true;
		}
	}

	c->frame_id++;
	*out_frame_id = c->frame_id;

	int64_t now_ns = static_cast<int64_t>(os_monotonic_get_ns());
	int64_t period_ns = static_cast<int64_t>(U_TIME_1S_IN_NS / c->sys->refresh_rate);

	*out_predicted_display_time_ns = now_ns + period_ns * 2;
	*out_predicted_display_period_ns = period_ns;
	*out_wake_time_ns = now_ns;
	*out_predicted_gpu_time_ns = period_ns;

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_wait_frame(struct xrt_compositor *xc,
                       int64_t *out_frame_id,
                       int64_t *out_predicted_display_time_ns,
                       int64_t *out_predicted_display_period_ns)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);

	std::lock_guard<std::mutex> lock(c->mutex);

	// If window was closed, push exit request and return dummy frame data
	if (c->window_closed) {
		if (!c->exit_request_sent && c->xses != nullptr) {
			union xrt_session_event xse = XRT_STRUCT_INIT;
			xse.type = XRT_SESSION_EVENT_EXIT_REQUEST;
			xrt_session_event_sink_push(c->xses, &xse);
			c->exit_request_sent = true;
		}
		c->frame_id++;
		*out_frame_id = c->frame_id;
		int64_t now_ns = static_cast<int64_t>(os_monotonic_get_ns());
		int64_t period_ns = static_cast<int64_t>(U_TIME_1S_IN_NS / c->sys->refresh_rate);
		*out_predicted_display_time_ns = now_ns + period_ns * 2;
		*out_predicted_display_period_ns = period_ns;
		return XRT_SUCCESS;
	}

	c->frame_id++;
	*out_frame_id = c->frame_id;

	int64_t now_ns = static_cast<int64_t>(os_monotonic_get_ns());
	int64_t period_ns = static_cast<int64_t>(U_TIME_1S_IN_NS / c->sys->refresh_rate);

	*out_predicted_display_time_ns = now_ns + period_ns * 2;
	*out_predicted_display_period_ns = period_ns;

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_mark_frame(struct xrt_compositor *xc,
                       int64_t frame_id,
                       enum xrt_compositor_frame_point point,
                       int64_t when_ns)
{
	return XRT_SUCCESS;
}

static xrt_result_t
compositor_begin_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);

	std::lock_guard<std::mutex> lock(c->mutex);
	c->layer_accum.layer_count = 0;

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_discard_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);

	std::lock_guard<std::mutex> lock(c->mutex);
	c->layer_accum.layer_count = 0;

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_layer_begin(struct xrt_compositor *xc, const struct xrt_layer_frame_data *data)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);

	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_begin(&c->layer_accum, data);

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_layer_projection(struct xrt_compositor *xc,
                             struct xrt_device *xdev,
                             struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                             const struct xrt_layer_data *data)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);

	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_projection(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_layer_projection_depth(struct xrt_compositor *xc,
                                   struct xrt_device *xdev,
                                   struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                   struct xrt_swapchain *d_xsc[XRT_MAX_VIEWS],
                                   const struct xrt_layer_data *data)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);

	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_projection_depth(&c->layer_accum, xsc, d_xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_layer_quad(struct xrt_compositor *xc,
                       struct xrt_device *xdev,
                       struct xrt_swapchain *xsc,
                       const struct xrt_layer_data *data)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);

	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_quad(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_layer_cube(struct xrt_compositor *xc,
                       struct xrt_device *xdev,
                       struct xrt_swapchain *xsc,
                       const struct xrt_layer_data *data)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);

	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_cube(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_layer_cylinder(struct xrt_compositor *xc,
                           struct xrt_device *xdev,
                           struct xrt_swapchain *xsc,
                           const struct xrt_layer_data *data)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);

	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_cylinder(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_layer_equirect1(struct xrt_compositor *xc,
                            struct xrt_device *xdev,
                            struct xrt_swapchain *xsc,
                            const struct xrt_layer_data *data)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);

	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_equirect1(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_layer_equirect2(struct xrt_compositor *xc,
                            struct xrt_device *xdev,
                            struct xrt_swapchain *xsc,
                            const struct xrt_layer_data *data)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);

	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_equirect2(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_layer_passthrough(struct xrt_compositor *xc,
                              struct xrt_device *xdev,
                              const struct xrt_layer_data *data)
{
	return XRT_SUCCESS;
}

/*!
 * Render the HUD overlay onto the back buffer (post-weave).
 * Uses CopySubresourceRegion for zero-shader simplicity.
 */
static void
d3d11_service_render_hud(struct d3d11_service_system *sys,
                          struct d3d11_client_render_resources *res,
                          bool weaving_done,
                          const struct xrt_eye_positions *eye_pos)
{
	if (!res->owns_window || res->hud == NULL) {
		return;
	}
	// When bridge is active, HUD visibility is controlled by the bridge's
	// shared memory (sample's TAB key), not the qwerty driver's TAB toggle.
	// Check bridge HUD first; fall back to u_hud_is_visible() for non-bridge.
	static HANDLE s_bridge_hud_mapping = nullptr;
	static struct bridge_hud_shared *s_bridge_hud = nullptr;
	static bool s_bridge_hud_tried = false;
	if (g_bridge_relay_active && !s_bridge_hud_tried) {
		s_bridge_hud_tried = true;
		s_bridge_hud_mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, BRIDGE_HUD_MAPPING_NAME);
		if (s_bridge_hud_mapping) {
			s_bridge_hud = (struct bridge_hud_shared *)MapViewOfFile(
			    s_bridge_hud_mapping, FILE_MAP_READ, 0, 0, sizeof(struct bridge_hud_shared));
			if (s_bridge_hud) {
				U_LOG_W("Bridge HUD shared memory opened by compositor");
			}
		}
	}
	bool bridge_hud_active = (s_bridge_hud != nullptr &&
	                          s_bridge_hud->magic == BRIDGE_HUD_MAGIC &&
	                          s_bridge_hud->visible);
	if (!bridge_hud_active && !u_hud_is_visible()) {
		return;
	}

	// Compute FPS from frame timestamps
	uint64_t now_ns = os_monotonic_get_ns();
	if (res->last_frame_time_ns != 0) {
		float dt_ms = (float)(now_ns - res->last_frame_time_ns) / 1e6f;
		// Exponential moving average (alpha=0.1 for smooth display)
		res->smoothed_frame_time_ms = res->smoothed_frame_time_ms * 0.9f + dt_ms * 0.1f;
	}
	res->last_frame_time_ns = now_ns;

	float fps = (res->smoothed_frame_time_ms > 0.0f) ? (1000.0f / res->smoothed_frame_time_ms) : 0.0f;

	// Get render and window dimensions
	uint32_t render_w = sys->view_width;
	uint32_t render_h = sys->view_height;
	uint32_t win_w = sys->output_width;
	uint32_t win_h = sys->output_height;
	if (res->hwnd != nullptr) {
		RECT rc;
		if (GetClientRect(res->hwnd, &rc)) {
			uint32_t ww = (uint32_t)(rc.right - rc.left);
			uint32_t wh = (uint32_t)(rc.bottom - rc.top);
			if (ww > 0 && wh > 0) {
				win_w = ww;
				win_h = wh;
			}
		}
	}

	// Get display physical dimensions from display processor
	float disp_w_mm = 0.0f, disp_h_mm = 0.0f;
	float nom_x = 0.0f, nom_y = 0.0f, nom_z = 600.0f;
	{
		float w_m = 0.0f, h_m = 0.0f;
		if (xrt_display_processor_d3d11_get_display_dimensions(
		        res->display_processor, &w_m, &h_m)) {
			disp_w_mm = w_m * 1000.0f;
			disp_h_mm = h_m * 1000.0f;
		}
	}

	// Fill HUD data
	struct u_hud_data data = {};
	data.device_name = sys->xdev->str;
	data.fps = fps;
	data.frame_time_ms = res->smoothed_frame_time_ms;
	data.mode_3d = sys->hardware_display_3d;
	if (sys->xdev != NULL && sys->xdev->hmd != NULL) {
		uint32_t idx = sys->xdev->hmd->active_rendering_mode_index;
		if (idx < sys->xdev->rendering_mode_count) {
			data.rendering_mode_name = sys->xdev->rendering_modes[idx].mode_name;
		}
	}
	data.render_width = render_w;
	data.render_height = render_h;
	if (sys->xdev != NULL && sys->xdev->rendering_mode_count > 0) {
		u_tiling_compute_system_atlas(sys->xdev->rendering_modes, sys->xdev->rendering_mode_count,
		                              &data.swapchain_width, &data.swapchain_height);
	}
	data.window_width = win_w;
	data.window_height = win_h;
	data.display_width_mm = disp_w_mm;
	data.display_height_mm = disp_h_mm;
	data.nominal_x = nom_x;
	data.nominal_y = nom_y;
	data.nominal_z = nom_z;
	// Use active rendering mode's view_count for eye display (not eye_pos->count,
	// which may report more eyes than the mode uses — e.g. tracker returns L/R in 2D mode).
	uint32_t mode_eye_count = eye_pos->count;
	if (sys->xdev != NULL && sys->xdev->hmd != NULL) {
		uint32_t midx = sys->xdev->hmd->active_rendering_mode_index;
		if (midx < sys->xdev->rendering_mode_count) {
			mode_eye_count = sys->xdev->rendering_modes[midx].view_count;
		}
	}
	if (mode_eye_count > eye_pos->count) mode_eye_count = eye_pos->count;
	data.eye_count = mode_eye_count;
	for (uint32_t e = 0; e < mode_eye_count && e < 8; e++) {
		data.eyes[e].x = eye_pos->eyes[e].x * 1000.0f;
		data.eyes[e].y = eye_pos->eyes[e].y * 1000.0f;
		data.eyes[e].z = eye_pos->eyes[e].z * 1000.0f;
	}
	data.eye_tracking_active = eye_pos->is_tracking;

#ifdef XRT_BUILD_DRIVER_QWERTY
	if (sys->xsysd != nullptr) {
		// Virtual display position + forward vector from qwerty device pose.
		struct xrt_pose qwerty_pose;
		if (qwerty_get_hmd_pose(sys->xsysd->xdevs, sys->xsysd->xdev_count, &qwerty_pose)) {
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
		if (qwerty_get_view_state(sys->xsysd->xdevs, sys->xsysd->xdev_count, &ss)) {
			data.camera_mode = ss.camera_mode;
			data.cam_spread_factor = ss.cam_spread_factor;
			data.cam_parallax_factor = ss.cam_parallax_factor;
			data.cam_convergence = ss.cam_convergence;
			data.cam_half_tan_vfov = ss.cam_half_tan_vfov;
			data.disp_spread_factor = ss.disp_spread_factor;
			data.disp_parallax_factor = ss.disp_parallax_factor;
			data.disp_vHeight = ss.disp_vHeight;
			data.nominal_viewer_z = ss.nominal_viewer_z;
			data.screen_height_m = ss.screen_height_m;
		}
	}
#endif

	data.bridge_hud = s_bridge_hud;

	bool dirty = u_hud_update(res->hud, &data);

	// Lazy-create the D3D11 staging texture
	if (!res->hud_initialized) {
		uint32_t hud_w = u_hud_get_width(res->hud);
		uint32_t hud_h = u_hud_get_height(res->hud);

		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = hud_w;
		desc.Height = hud_h;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = 0; // No shader binding needed, just copy source

		HRESULT hr = sys->device->CreateTexture2D(&desc, nullptr, res->hud_texture.put());
		if (FAILED(hr)) {
			U_LOG_E("Failed to create HUD texture: 0x%08lx", hr);
			return;
		}
		res->hud_initialized = true;
		dirty = true; // Force initial upload
	}

	// Upload pixels to staging texture if changed
	if (dirty && res->hud_texture) {
		uint32_t hud_w = u_hud_get_width(res->hud);
		sys->context->UpdateSubresource(res->hud_texture.get(), 0, nullptr,
		                                 u_hud_get_pixels(res->hud),
		                                 hud_w * 4, 0);
	}

	// Blit HUD texture to bottom-left of back buffer
	if (res->hud_texture && res->swap_chain) {
		wil::com_ptr<ID3D11Texture2D> back_buffer;
		res->swap_chain->GetBuffer(0, IID_PPV_ARGS(back_buffer.put()));
		if (back_buffer) {
			uint32_t hud_w = u_hud_get_width(res->hud);
			uint32_t hud_h = u_hud_get_height(res->hud);

			// Position at bottom-left with 10px margin
			uint32_t dst_x = 10;
			uint32_t dst_y = (win_h > hud_h + 10) ? (win_h - hud_h - 10) : 0;

			D3D11_BOX src_box = {0, 0, 0, hud_w, hud_h, 1};
			sys->context->CopySubresourceRegion(back_buffer.get(), 0, dst_x, dst_y, 0,
			                                     res->hud_texture.get(), 0, &src_box);
		}
	}
}

/*!
 * Crop atlas to content dimensions for the display processor.
 * Mirrors d3d11_crop_atlas_for_dp() from the in-process compositor.
 * When content is smaller than atlas (legacy compromise scale), copies
 * each view's content region to a content-sized staging texture.
 * Returns the SRV to pass to process_atlas().
 */
static ID3D11ShaderResourceView *
service_crop_atlas_for_dp(struct d3d11_service_system *sys,
                          struct d3d11_client_render_resources *res,
                          uint32_t content_view_w,
                          uint32_t content_view_h,
                          bool flip_y)
{
	uint32_t expected_w = sys->tile_columns * content_view_w;
	uint32_t expected_h = sys->tile_rows * content_view_h;

	// Content fills the full atlas — pass directly (only when no flip needed)
	if (!flip_y && expected_w == sys->display_width && expected_h == sys->display_height) {
		return res->atlas_srv.get();
	}

	// Lazy (re)create crop texture at content dimensions.
	// When flip_y is needed we must use a shader blit, which requires the
	// crop texture to be bindable as a render target.
	if (res->crop_width != expected_w || res->crop_height != expected_h) {
		res->crop_srv.reset();
		res->crop_rtv.reset();
		res->crop_texture.reset();

		D3D11_TEXTURE2D_DESC crop_desc = {};
		crop_desc.Width = expected_w;
		crop_desc.Height = expected_h;
		crop_desc.MipLevels = 1;
		crop_desc.ArraySize = 1;
		crop_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		crop_desc.SampleDesc.Count = 1;
		crop_desc.Usage = D3D11_USAGE_DEFAULT;
		crop_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

		HRESULT hr = sys->device->CreateTexture2D(
		    &crop_desc, nullptr, res->crop_texture.put());
		if (SUCCEEDED(hr)) {
			sys->device->CreateShaderResourceView(
			    res->crop_texture.get(), nullptr, res->crop_srv.put());
			sys->device->CreateRenderTargetView(
			    res->crop_texture.get(), nullptr, res->crop_rtv.put());
			res->crop_width = expected_w;
			res->crop_height = expected_h;
			U_LOG_I("Crop-blit: created %ux%u staging texture "
			        "(atlas=%ux%u, content=%ux%u/view)",
			        expected_w, expected_h,
			        sys->display_width, sys->display_height,
			        content_view_w, content_view_h);
		}
	}

	if (!res->crop_texture) {
		return res->atlas_srv.get(); // fallback
	}

	// Get atlas dimensions for shader blit src_size
	D3D11_TEXTURE2D_DESC atlas_desc = {};
	res->atlas_texture->GetDesc(&atlas_desc);
	uint32_t src_tex_w = atlas_desc.Width;
	uint32_t src_tex_h = atlas_desc.Height;

	uint32_t num_views = sys->tile_columns * sys->tile_rows;

	if (flip_y && res->crop_rtv && sys->blit_vs && sys->blit_ps) {
		// Shader blit path: crop + Y-flip in one pass per view.
		// Set up pipeline once, then draw N views with different constants.
		sys->context->VSSetShader(sys->blit_vs.get(), nullptr, 0);
		sys->context->PSSetShader(sys->blit_ps.get(), nullptr, 0);
		sys->context->VSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
		sys->context->PSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
		sys->context->PSSetSamplers(0, 1, sys->sampler_linear.addressof());
		ID3D11ShaderResourceView *src_srv = res->atlas_srv.get();
		sys->context->PSSetShaderResources(0, 1, &src_srv);

		ID3D11RenderTargetView *rtvs[] = {res->crop_rtv.get()};
		sys->context->OMSetRenderTargets(1, rtvs, nullptr);
		D3D11_VIEWPORT vp = {};
		vp.Width = (float)expected_w;
		vp.Height = (float)expected_h;
		vp.MaxDepth = 1.0f;
		sys->context->RSSetViewports(1, &vp);
		sys->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		sys->context->IASetInputLayout(nullptr);
		sys->context->RSSetState(sys->rasterizer_state.get());
		sys->context->OMSetDepthStencilState(sys->depth_disabled.get(), 0);
		sys->context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

		for (uint32_t v = 0; v < num_views && v < XRT_MAX_VIEWS; v++) {
			uint32_t src_tile_x, src_tile_y;
			u_tiling_view_origin(v, sys->tile_columns,
			                     sys->view_width, sys->view_height,
			                     &src_tile_x, &src_tile_y);
			uint32_t dst_tile_x, dst_tile_y;
			u_tiling_view_origin(v, sys->tile_columns,
			                     content_view_w, content_view_h,
			                     &dst_tile_x, &dst_tile_y);

			D3D11_MAPPED_SUBRESOURCE mapped;
			HRESULT hr = sys->context->Map(sys->blit_constant_buffer.get(), 0,
			                                D3D11_MAP_WRITE_DISCARD, 0, &mapped);
			if (FAILED(hr)) continue;
			BlitConstants *cb = static_cast<BlitConstants *>(mapped.pData);
			memset(cb, 0, sizeof(*cb));
			cb->src_rect[0] = (float)src_tile_x;
			cb->src_rect[1] = (float)src_tile_y + (float)content_view_h; // bottom (flipped origin)
			cb->src_rect[2] = (float)content_view_w;
			cb->src_rect[3] = -(float)content_view_h;                    // negative h = flip
			cb->dst_offset[0] = (float)dst_tile_x;
			cb->dst_offset[1] = (float)dst_tile_y;
			cb->src_size[0] = (float)src_tex_w;
			cb->src_size[1] = (float)src_tex_h;
			cb->dst_size[0] = (float)expected_w;
			cb->dst_size[1] = (float)expected_h;
			cb->dst_rect_wh[0] = (float)content_view_w;
			cb->dst_rect_wh[1] = (float)content_view_h;
			cb->convert_srgb = 0.0f;
			cb->chrome_alpha = 0.0f; // 8.C: 0=full opacity (chrome blits override)
			cb->quad_mode = 0.0f;
			cb->corner_radius = 0.0f;
			cb->corner_aspect = 1.0f;
			cb->edge_feather = 0.0f;
			cb->glow_intensity = 0.0f;
			sys->context->Unmap(sys->blit_constant_buffer.get(), 0);

			sys->context->Draw(4, 0);
		}

		// Unbind the SRV so the crop texture can be used as input next
		ID3D11ShaderResourceView *null_srv = nullptr;
		sys->context->PSSetShaderResources(0, 1, &null_srv);

		return res->crop_srv.get();
	}

	// Non-flip path: copy each view's content region from atlas to crop texture
	for (uint32_t v = 0; v < num_views && v < XRT_MAX_VIEWS; v++) {
		uint32_t src_tile_x, src_tile_y;
		u_tiling_view_origin(v, sys->tile_columns,
		                     sys->view_width, sys->view_height,
		                     &src_tile_x, &src_tile_y);
		uint32_t dst_tile_x, dst_tile_y;
		u_tiling_view_origin(v, sys->tile_columns,
		                     content_view_w, content_view_h,
		                     &dst_tile_x, &dst_tile_y);

		D3D11_BOX box = {};
		box.left = src_tile_x;
		box.top = src_tile_y;
		box.right = src_tile_x + content_view_w;
		box.bottom = src_tile_y + content_view_h;
		box.front = 0;
		box.back = 1;

		sys->context->CopySubresourceRegion(
		    res->crop_texture.get(), 0,
		    dst_tile_x, dst_tile_y, 0,
		    res->atlas_texture.get(), 0, &box);
	}

	return res->crop_srv.get();
}


/*
 *
 * Multi-compositor functions
 *
 */

/*!
 * Update the multi-comp window's input forwarding target to the focused app's HWND.
 * Called whenever focused_slot changes (TAB, register, unregister).
 *
 * For capture clients, also manages SetForegroundWindow so SendInput reaches
 * the correct off-screen window.
 */
static void
multi_compositor_update_input_forward(struct d3d11_multi_compositor *mc)
{
	if (mc == nullptr || mc->window == nullptr) {
		return;
	}

	HWND target = NULL;
	int32_t rx = 0, ry = 0, rw = 0, rh = 0;
	bool is_capture = false;
	if (mc->focused_slot >= 0 && mc->focused_slot < D3D11_MULTI_MAX_CLIENTS &&
	    mc->clients[mc->focused_slot].active) {
		target = mc->clients[mc->focused_slot].app_hwnd;
		rx = (int32_t)mc->clients[mc->focused_slot].window_rect_x;
		ry = (int32_t)mc->clients[mc->focused_slot].window_rect_y;
		rw = (int32_t)mc->clients[mc->focused_slot].window_rect_w;
		rh = (int32_t)mc->clients[mc->focused_slot].window_rect_h;
		is_capture = (mc->clients[mc->focused_slot].client_type == CLIENT_TYPE_CAPTURE);
	}

	// Inset the forwarding rect by the resize zone width so clicks
	// on the window edge (for resize) don't get forwarded to the app.
	// UI_RESIZE_ZONE_M = 0.003m ≈ 17px on a 1920-wide tile. Use 20 for margin.
	int32_t inset = 20;
	if (rw > 2 * inset && rh > 2 * inset && !is_capture) {
		rx += inset;
		ry += inset;
		rw -= 2 * inset;
		rh -= 2 * inset;
	}

	comp_d3d11_window_set_input_forward(mc->window, (void *)target, rx, ry, rw, rh, is_capture);

	// Track focused capture client (preview only — no foreground/input injection).
	// SetForegroundWindow disabled: it steals OS focus and constrains the mouse.
	// Input forwarding to WinUI apps is tracked in #124.
	if (is_capture && target != NULL) {
		mc->current_foreground_capture = target;
	} else {
		mc->current_foreground_capture = NULL;
	}
}

/*!
 * Dispatch buffered input events to the focused capture client via SendInput.
 *
 * Called from the render loop. Drains the ring buffer and converts WM_ messages
 * to INPUT structs for SendInput, which injects into the OS input queue.
 * The focused capture HWND must already be foreground (via update_input_forward).
 */
static void
multi_compositor_dispatch_capture_input(struct d3d11_multi_compositor *mc)
{
	if (mc == nullptr || mc->window == nullptr) {
		return;
	}

	struct workspace_input_event events[WORKSPACE_INPUT_RING_SIZE];
	uint32_t count = comp_d3d11_window_consume_input_events(mc->window, events, WORKSPACE_INPUT_RING_SIZE);
	if (count == 0) {
		return;
	}

	// Get the current foreground capture HWND for mouse coordinate mapping
	HWND fg = mc->current_foreground_capture;
	if (fg == NULL) {
		return; // No capture client is foreground, discard events
	}

	// Get screen position of the capture HWND for absolute mouse coordinates
	RECT fg_screen = {0};
	GetWindowRect(fg, &fg_screen);
	// Get client area offset within the window
	POINT client_origin = {0, 0};
	ClientToScreen(fg, &client_origin);

	// Virtual screen dimensions for MOUSEEVENTF_ABSOLUTE normalization
	int vs_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
	int vs_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
	int vs_left = GetSystemMetrics(SM_XVIRTUALSCREEN);
	int vs_top = GetSystemMetrics(SM_YVIRTUALSCREEN);

	INPUT inputs[WORKSPACE_INPUT_RING_SIZE];
	uint32_t input_count = 0;

	for (uint32_t i = 0; i < count; i++) {
		struct workspace_input_event *ev = &events[i];
		INPUT *inp = &inputs[input_count];
		memset(inp, 0, sizeof(INPUT));

		switch (ev->message) {
		case WM_CHAR: {
			// Unicode character input — works for all app frameworks
			inp->type = INPUT_KEYBOARD;
			inp->ki.wVk = 0;
			inp->ki.wScan = (WORD)ev->wParam;
			inp->ki.dwFlags = KEYEVENTF_UNICODE;
			input_count++;
			break;
		}
		case WM_SYSCHAR: {
			inp->type = INPUT_KEYBOARD;
			inp->ki.wVk = 0;
			inp->ki.wScan = (WORD)ev->wParam;
			inp->ki.dwFlags = KEYEVENTF_UNICODE;
			input_count++;
			break;
		}
		case WM_KEYDOWN:
		case WM_SYSKEYDOWN: {
			inp->type = INPUT_KEYBOARD;
			inp->ki.wVk = (WORD)ev->wParam;
			inp->ki.wScan = (WORD)((ev->lParam >> 16) & 0xFF);
			inp->ki.dwFlags = (ev->lParam & (1 << 24)) ? KEYEVENTF_EXTENDEDKEY : 0;
			input_count++;
			break;
		}
		case WM_KEYUP:
		case WM_SYSKEYUP: {
			inp->type = INPUT_KEYBOARD;
			inp->ki.wVk = (WORD)ev->wParam;
			inp->ki.wScan = (WORD)((ev->lParam >> 16) & 0xFF);
			inp->ki.dwFlags = KEYEVENTF_KEYUP |
			                  ((ev->lParam & (1 << 24)) ? KEYEVENTF_EXTENDEDKEY : 0);
			input_count++;
			break;
		}
		case WM_MOUSEMOVE: {
			if (ev->mapped_x < 0) break;
			inp->type = INPUT_MOUSE;
			// Convert app-local coords to absolute screen coords
			int screen_x = client_origin.x + ev->mapped_x;
			int screen_y = client_origin.y + ev->mapped_y;
			inp->mi.dx = (int)((float)(screen_x - vs_left) * 65535.0f / (float)(vs_width - 1));
			inp->mi.dy = (int)((float)(screen_y - vs_top) * 65535.0f / (float)(vs_height - 1));
			inp->mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
			input_count++;
			break;
		}
		case WM_LBUTTONDOWN: {
			if (ev->mapped_x < 0) break;
			// Move cursor first
			inp->type = INPUT_MOUSE;
			int screen_x = client_origin.x + ev->mapped_x;
			int screen_y = client_origin.y + ev->mapped_y;
			inp->mi.dx = (int)((float)(screen_x - vs_left) * 65535.0f / (float)(vs_width - 1));
			inp->mi.dy = (int)((float)(screen_y - vs_top) * 65535.0f / (float)(vs_height - 1));
			inp->mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK |
			                  MOUSEEVENTF_LEFTDOWN;
			input_count++;
			break;
		}
		case WM_LBUTTONUP: {
			inp->type = INPUT_MOUSE;
			if (ev->mapped_x >= 0) {
				int screen_x = client_origin.x + ev->mapped_x;
				int screen_y = client_origin.y + ev->mapped_y;
				inp->mi.dx = (int)((float)(screen_x - vs_left) * 65535.0f / (float)(vs_width - 1));
				inp->mi.dy = (int)((float)(screen_y - vs_top) * 65535.0f / (float)(vs_height - 1));
				inp->mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
			}
			inp->mi.dwFlags |= MOUSEEVENTF_LEFTUP;
			input_count++;
			break;
		}
		case WM_RBUTTONDOWN: {
			if (ev->mapped_x < 0) break;
			inp->type = INPUT_MOUSE;
			int screen_x = client_origin.x + ev->mapped_x;
			int screen_y = client_origin.y + ev->mapped_y;
			inp->mi.dx = (int)((float)(screen_x - vs_left) * 65535.0f / (float)(vs_width - 1));
			inp->mi.dy = (int)((float)(screen_y - vs_top) * 65535.0f / (float)(vs_height - 1));
			inp->mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK |
			                  MOUSEEVENTF_RIGHTDOWN;
			input_count++;
			break;
		}
		case WM_RBUTTONUP: {
			inp->type = INPUT_MOUSE;
			if (ev->mapped_x >= 0) {
				int screen_x = client_origin.x + ev->mapped_x;
				int screen_y = client_origin.y + ev->mapped_y;
				inp->mi.dx = (int)((float)(screen_x - vs_left) * 65535.0f / (float)(vs_width - 1));
				inp->mi.dy = (int)((float)(screen_y - vs_top) * 65535.0f / (float)(vs_height - 1));
				inp->mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
			}
			inp->mi.dwFlags |= MOUSEEVENTF_RIGHTUP;
			input_count++;
			break;
		}
		case WM_MBUTTONDOWN: {
			if (ev->mapped_x < 0) break;
			inp->type = INPUT_MOUSE;
			int screen_x = client_origin.x + ev->mapped_x;
			int screen_y = client_origin.y + ev->mapped_y;
			inp->mi.dx = (int)((float)(screen_x - vs_left) * 65535.0f / (float)(vs_width - 1));
			inp->mi.dy = (int)((float)(screen_y - vs_top) * 65535.0f / (float)(vs_height - 1));
			inp->mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK |
			                  MOUSEEVENTF_MIDDLEDOWN;
			input_count++;
			break;
		}
		case WM_MBUTTONUP: {
			inp->type = INPUT_MOUSE;
			if (ev->mapped_x >= 0) {
				int screen_x = client_origin.x + ev->mapped_x;
				int screen_y = client_origin.y + ev->mapped_y;
				inp->mi.dx = (int)((float)(screen_x - vs_left) * 65535.0f / (float)(vs_width - 1));
				inp->mi.dy = (int)((float)(screen_y - vs_top) * 65535.0f / (float)(vs_height - 1));
				inp->mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
			}
			inp->mi.dwFlags |= MOUSEEVENTF_MIDDLEUP;
			input_count++;
			break;
		}
		default:
			break;
		}
	}

	if (input_count > 0) {
		SendInput(input_count, inputs, sizeof(INPUT));
	}
}

// Forward declarations
static inline bool quat_is_identity(const struct xrt_quat *q);
static void capture_render_thread_start(struct d3d11_service_system *sys);

// Forward declarations — defined in the external API section below.
static void
slot_pose_to_pixel_rect(const struct d3d11_service_system *sys,
                        const struct d3d11_multi_client_slot *slot,
                        int32_t *out_x, int32_t *out_y,
                        int32_t *out_w, int32_t *out_h);

static void
slot_pose_to_pixel_rect_for_eye(const struct d3d11_service_system *sys,
                                const struct d3d11_multi_client_slot *slot,
                                float eye_x, float eye_y, float eye_z,
                                int32_t *out_x, int32_t *out_y,
                                int32_t *out_w, int32_t *out_h);

static bool
compute_projected_quad_corners(const struct d3d11_service_system *sys,
                               const struct d3d11_multi_client_slot *slot,
                               float eye_x, float eye_y, float eye_z,
                               uint32_t tile_col, uint32_t tile_row,
                               uint32_t half_w, uint32_t half_h,
                               uint32_t ca_w, uint32_t ca_h,
                               float out_corners[8], float out_w[4]);

static void
project_local_rect_for_eye(const struct d3d11_service_system *sys,
                           const struct xrt_quat *orientation,
                           float win_cx, float win_cy, float win_cz,
                           float local_left, float local_top,
                           float local_right, float local_bottom,
                           float eye_x, float eye_y, float eye_z,
                           uint32_t tile_col, uint32_t tile_row,
                           uint32_t half_w, uint32_t half_h,
                           uint32_t ca_w, uint32_t ca_h,
                           float out_corners[8], float out_w[4]);

static inline void blit_set_quad_corners(BlitConstants *cb, const float corners[8], const float w[4]);

// Phase 2.K: depth-pipeline forward declarations. Definitions live alongside
// blit_set_quad_corners' body further down. The constants are #defines so
// they're visible at point of use without needing forward declaration.
#define WORKSPACE_DEPTH_FAR_M 1.0f
#define WORKSPACE_CHROME_DEPTH_BIAS 0.001f
static inline float workspace_depth_ndc_from_distance(float eye_to_z_distance);
static inline void blit_set_axis_aligned_depth(BlitConstants *cb, float eye_z, float window_z, float chrome_bias);
static inline void blit_set_perspective_depth(BlitConstants *cb, const float w[4], float chrome_bias);

/*!
 * Register a per-client compositor with the multi-compositor.
 * Returns slot index, or -1 if full.
 */
static int
multi_compositor_register_client(struct d3d11_service_system *sys, struct d3d11_service_compositor *c)
{
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (mc == nullptr) {
		return -1;
	}

	for (int i = 0; i < D3D11_MULTI_MAX_CLIENTS; i++) {
		if (!mc->clients[i].active) {
			mc->clients[i].compositor = c;
			mc->clients[i].client_type = CLIENT_TYPE_IPC;
			mc->clients[i].active = true;
			mc->clients[i].has_first_frame_committed = false;
			mc->clients[i].first_frame_ns = 0;
			// Phase 2.C: drop any leftover chrome registration from a
			// prior tenant so the new client starts with no chrome.
			if (mc->clients[i].chrome_xsc != nullptr) {
				xrt_swapchain_reference(&mc->clients[i].chrome_xsc, NULL);
			}
			mc->clients[i].chrome_swapchain_id = 0;
			mc->clients[i].chrome_layout_valid = false;
			mc->clients[i].chrome_region_count = 0;

			// Count active clients to determine grid position
			int active_count = 0;
			for (int j = 0; j < D3D11_MULTI_MAX_CLIENTS; j++) {
				if (mc->clients[j].active) active_count++;
			}
			// Include this new client in the count
			active_count++;

			float center_x_m, center_y_m, center_z_m, grid_w, grid_h;
			compute_grid_layout(sys, active_count, active_count - 1,
			                    &center_x_m, &center_y_m, &center_z_m, &grid_w, &grid_h);

			// Entry animation: start from center (small), animate to target position.
			mc->clients[i].window_pose.orientation = {0, 0, 0, 1};
			mc->clients[i].window_pose.position = {0, 0, 0};
			mc->clients[i].window_width_m = grid_w * 0.3f; // start small
			mc->clients[i].window_height_m = grid_h * 0.3f;

			// Entry animation: grow from center to target position
			struct xrt_pose target = {};
			target.orientation.w = 1.0f;
			target.position.x = center_x_m;
			target.position.y = center_y_m;
			target.position.z = center_z_m;
			// Use inline animation setup (slot_animate_to is defined later in file)
			mc->clients[i].anim.active = true;
			mc->clients[i].anim.start_pose = mc->clients[i].window_pose;
			mc->clients[i].anim.target_pose = target;
			mc->clients[i].anim.start_width_m = grid_w * 0.3f;
			mc->clients[i].anim.start_height_m = grid_h * 0.3f;
			mc->clients[i].anim.target_width_m = grid_w;
			mc->clients[i].anim.target_height_m = grid_h;
			mc->clients[i].anim.start_ns = os_monotonic_get_ns();
			mc->clients[i].anim.duration_ns = 400ULL * 1000000ULL; // 400ms

			// Compute pixel rect from initial (small) pose
			slot_pose_to_pixel_rect(sys, &mc->clients[i],
			                        &mc->clients[i].window_rect_x,
			                        &mc->clients[i].window_rect_y,
			                        &mc->clients[i].window_rect_w,
			                        &mc->clients[i].window_rect_h);
			mc->clients[i].hwnd_resize_pending = true;

			mc->client_count++;
			if (mc->focused_slot < 0) {
				mc->focused_slot = i;
			}
			U_LOG_W("Multi-comp: registered client in slot %d (total=%u)", i, mc->client_count);
			U_LOG_W("  window: pose=(%.3f,%.3f,%.3f) size=%.3fx%.3fm rect=(%d,%d %dx%d px)",
			        mc->clients[i].window_pose.position.x,
			        mc->clients[i].window_pose.position.y,
			        mc->clients[i].window_pose.position.z,
			        mc->clients[i].window_width_m,
			        mc->clients[i].window_height_m,
			        mc->clients[i].window_rect_x,
			        mc->clients[i].window_rect_y,
			        mc->clients[i].window_rect_w,
			        mc->clients[i].window_rect_h);
			multi_compositor_update_input_forward(mc);

			// Ensure render timer is running. Normally started on capture client
			// connect, but pure 3D IPC sessions need it too — otherwise workspace UI
			// (drag, rotation) only repaints at the app's framerate (very slow on iGPU).
			capture_render_thread_start(sys);

			return i;
		}
	}
	return -1;
}

/*!
 * Unregister a per-client compositor from the multi-compositor.
 */
static void multi_compositor_render(struct d3d11_service_system *sys); // forward decl

static void
multi_compositor_unregister_client(struct d3d11_service_system *sys, struct d3d11_service_compositor *c)
{
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (mc == nullptr) {
		return;
	}

	for (int i = 0; i < D3D11_MULTI_MAX_CLIENTS; i++) {
		if (mc->clients[i].active && mc->clients[i].compositor == c) {
			mc->clients[i].active = false;
			mc->clients[i].compositor = nullptr;
			mc->client_count--;
			if (mc->focused_slot == i) {
				mc->focused_slot = -1;
				for (int j = 0; j < D3D11_MULTI_MAX_CLIENTS; j++) {
					if (mc->clients[j].active && !mc->clients[j].minimized) {
						mc->focused_slot = j;
						break;
					}
				}
				multi_compositor_update_input_forward(mc);
			}
			// Cancel any active drag on this slot
			if (mc->drag.active && mc->drag.slot == i) {
				mc->drag.active = false;
				mc->drag.slot = -1;
			}
			U_LOG_W("Multi-comp: unregistered client from slot %d (total=%u)", i, mc->client_count);

			// Render one final frame to clear the stale content.
			// Without this, the last app frame stays on screen because
			// multi_compositor_render is only called from layer_commit.
			multi_compositor_render(sys);
			break;
		}
	}
}

/*!
 * Render timer thread for capture clients.
 *
 * When capture-only clients are active (no IPC clients driving layer_commit),
 * this thread ensures the multi-compositor renders at display refresh rate.
 */
static void
capture_render_thread_func(struct d3d11_service_system *sys)
{
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	while (mc && mc->capture_render_running.load()) {
		// Wait up to 14ms (~70fps). render_wakeup_event can be signaled
		// early for instant shutdown or future drag-responsive repaints.
		if (mc->render_wakeup_event) {
			WaitForSingleObject(mc->render_wakeup_event, 14);
		} else {
			Sleep(14);
		}

		if (!mc->capture_render_running.load()) break;

		std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);
		if (sys->multi_comp) {
			// Always render: IPC clients drive via layer_commit too, but on
			// slow GPUs (e.g. Intel iGPU) they run at <10fps. The 14ms
			// throttle in layer_commit prevents double-renders, so this is safe.
			multi_compositor_render(sys);
			sys->last_workspace_render_ns = os_monotonic_get_ns();
		}
	}
}

/*!
 * Start the capture render timer thread (if not already running).
 */
static void
capture_render_thread_start(struct d3d11_service_system *sys)
{
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (mc == nullptr || mc->capture_render_running.load()) {
		return;
	}
	mc->render_wakeup_event = CreateEvent(nullptr, FALSE, FALSE, nullptr); // auto-reset
	mc->capture_render_running.store(true);
	mc->capture_render_thread = std::thread(capture_render_thread_func, sys);
	U_LOG_W("Multi-comp: capture render timer started");
}

/*!
 * Stop the capture render timer thread (if running).
 */
static void
capture_render_thread_stop(struct d3d11_service_system *sys)
{
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (mc == nullptr || !mc->capture_render_running.load()) {
		return;
	}
	mc->capture_render_running.store(false);
	// Signal the event so the thread wakes immediately instead of waiting
	// up to 14ms for the timeout to expire.
	if (mc->render_wakeup_event) {
		SetEvent(mc->render_wakeup_event);
	}
	if (mc->capture_render_thread.joinable()) {
		mc->capture_render_thread.join();
	}
	if (mc->render_wakeup_event) {
		CloseHandle(mc->render_wakeup_event);
		mc->render_wakeup_event = nullptr;
	}
	U_LOG_W("Multi-comp: capture render timer stopped");
}

/*!
 * Add a 2D window capture client to the multi-compositor.
 *
 * Starts Windows.Graphics.Capture for the given HWND and assigns a slot.
 * The captured content will be rendered as a mono textured quad.
 *
 * @return Slot index (0-7), or -1 on failure.
 */
static int
multi_compositor_add_capture_client(struct d3d11_service_system *sys, HWND hwnd, const char *name)
{
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (mc == nullptr) {
		return -1;
	}

	// Start capture
	struct d3d11_capture_context *cap_ctx =
	    d3d11_capture_start((struct ID3D11Device *)sys->device.get(), hwnd);
	if (cap_ctx == nullptr) {
		U_LOG_E("Multi-comp: failed to start capture for HWND=%p", (void *)hwnd);
		return -1;
	}

	// Find first inactive slot
	for (int i = 0; i < D3D11_MULTI_MAX_CLIENTS; i++) {
		if (!mc->clients[i].active) {
			mc->clients[i].client_type = CLIENT_TYPE_CAPTURE;
			mc->clients[i].compositor = nullptr;
			mc->clients[i].capture_ctx = cap_ctx;
			mc->clients[i].capture_srv = nullptr;
			mc->clients[i].capture_texture_last = nullptr;
			mc->clients[i].capture_width = 0;
			mc->clients[i].capture_height = 0;
			mc->clients[i].app_hwnd = hwnd;
			mc->clients[i].active = true;
			mc->clients[i].minimized = false;
			mc->clients[i].maximized = false;
			mc->clients[i].hwnd_resize_pending = false;

			// App name
			if (name && name[0]) {
				snprintf(mc->clients[i].app_name, sizeof(mc->clients[i].app_name), "%s", name);
			} else {
				char title[128] = {0};
				int len = GetWindowTextA(hwnd, title, sizeof(title));
				if (len > 0) {
					snprintf(mc->clients[i].app_name, sizeof(mc->clients[i].app_name), "%s", title);
				} else {
					snprintf(mc->clients[i].app_name, sizeof(mc->clients[i].app_name), "Capture %d", i);
				}
			}
			// Replace non-ASCII characters
			for (char *p = mc->clients[i].app_name; *p; p++) {
				if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E) *p = '-';
			}

			// Save original window placement and style (for restore on removal)
			mc->clients[i].saved_placement.length = sizeof(WINDOWPLACEMENT);
			GetWindowPlacement(hwnd, &mc->clients[i].saved_placement);
			mc->clients[i].saved_exstyle = (LONG)GetWindowLongPtr(hwnd, GWL_EXSTYLE);

			// NOTE: Off-screen move disabled — causes partial black in capture.
			// The captured window stays on the desktop, occluded by the workspace's
			// fullscreen window. Capture API gets content regardless.

			// Compute initial size from HWND DPI
			RECT client_rect = {};
			GetClientRect(hwnd, &client_rect);
			UINT dpi = GetDpiForWindow(hwnd);
			if (dpi == 0) dpi = 96;
			uint32_t px_w = client_rect.right - client_rect.left;
			uint32_t px_h = client_rect.bottom - client_rect.top;
			if (px_w == 0) px_w = 800;
			if (px_h == 0) px_h = 600;

			float width_m = ((float)px_w / (float)dpi) * 0.0254f;
			float height_m = ((float)px_h / (float)dpi) * 0.0254f;

			// Clamp to reasonable range
			float disp_w_m = sys->base.info.display_width_m;
			if (disp_w_m <= 0.0f) disp_w_m = 0.700f;
			float max_w = disp_w_m * 0.6f;
			if (width_m > max_w) {
				float scale = max_w / width_m;
				width_m *= scale;
				height_m *= scale;
			}
			if (width_m < 0.04f) width_m = 0.04f;
			if (height_m < 0.04f) height_m = 0.04f;

			// Count active clients to determine grid position
			int active_count = 0;
			for (int j = 0; j < D3D11_MULTI_MAX_CLIENTS; j++) {
				if (mc->clients[j].active) active_count++;
			}
			// Include this new client in the count
			active_count++;

			float center_x_m, center_y_m, center_z_m, grid_w, grid_h;
			compute_grid_layout(sys, active_count, active_count - 1,
			                    &center_x_m, &center_y_m, &center_z_m, &grid_w, &grid_h);

			// Maintain aspect ratio: fit within grid cell
			float aspect = (height_m > 0.001f) ? width_m / height_m : 1.0f;
			if (grid_w / aspect > grid_h) {
				// Height-limited: scale by height
				width_m = grid_h * aspect;
				height_m = grid_h;
			} else {
				// Width-limited: scale by width
				height_m = grid_w / aspect;
				width_m = grid_w;
			}

			// Entry animation: start from center (small), animate to target position
			mc->clients[i].window_pose.orientation = {0, 0, 0, 1};
			mc->clients[i].window_pose.position = {0, 0, 0};
			mc->clients[i].window_width_m = width_m * 0.3f;
			mc->clients[i].window_height_m = height_m * 0.3f;

			struct xrt_pose target = {};
			target.orientation.w = 1.0f;
			target.position.x = center_x_m;
			target.position.y = center_y_m;
			target.position.z = center_z_m;
			mc->clients[i].anim.active = true;
			mc->clients[i].anim.start_pose = mc->clients[i].window_pose;
			mc->clients[i].anim.target_pose = target;
			mc->clients[i].anim.start_width_m = width_m * 0.3f;
			mc->clients[i].anim.start_height_m = height_m * 0.3f;
			mc->clients[i].anim.target_width_m = width_m;
			mc->clients[i].anim.target_height_m = height_m;
			mc->clients[i].anim.start_ns = os_monotonic_get_ns();
			mc->clients[i].anim.duration_ns = 400ULL * 1000000ULL;

			// Content view dimensions (will be updated on first capture frame)
			mc->clients[i].content_view_w = px_w;
			mc->clients[i].content_view_h = px_h;

			// Compute pixel rect
			slot_pose_to_pixel_rect(sys, &mc->clients[i],
			    &mc->clients[i].window_rect_x,
			    &mc->clients[i].window_rect_y,
			    &mc->clients[i].window_rect_w,
			    &mc->clients[i].window_rect_h);

			mc->client_count++;
			mc->capture_client_count++;
			if (mc->focused_slot < 0) {
				mc->focused_slot = i;
			}

			U_LOG_W("Multi-comp: added capture client in slot %d HWND=%p '%s' "
			         "(%ux%u px, %.3fx%.3f m, total=%u, captures=%u)",
			         i, (void *)hwnd, mc->clients[i].app_name,
			         px_w, px_h, width_m, height_m,
			         mc->client_count, mc->capture_client_count);
			multi_compositor_update_input_forward(mc);

			// Start render timer if this is the first capture client
			if (mc->capture_client_count == 1) {
				capture_render_thread_start(sys);
			}

			return i;
		}
	}

	// No free slot
	d3d11_capture_stop(cap_ctx);
	U_LOG_E("Multi-comp: max clients (%d) reached, cannot add capture", D3D11_MULTI_MAX_CLIENTS);
	return -1;
}

/*!
 * Remove a capture client from the multi-compositor.
 */
static bool
multi_compositor_remove_capture_client(struct d3d11_service_system *sys, int slot_index)
{
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (mc == nullptr || slot_index < 0 || slot_index >= D3D11_MULTI_MAX_CLIENTS) {
		return false;
	}

	struct d3d11_multi_client_slot *slot = &mc->clients[slot_index];
	if (!slot->active || slot->client_type != CLIENT_TYPE_CAPTURE) {
		return false;
	}

	// Stop capture
	d3d11_capture_stop(slot->capture_ctx);
	slot->capture_ctx = nullptr;
	slot->capture_srv = nullptr;
	slot->capture_texture_last = nullptr;
	slot->capture_width = 0;
	slot->capture_height = 0;

	slot->active = false;
	slot->compositor = nullptr;
	slot->client_type = CLIENT_TYPE_IPC; // reset
	mc->client_count--;
	mc->capture_client_count--;

	if (mc->focused_slot == slot_index) {
		mc->focused_slot = -1;
		for (int j = 0; j < D3D11_MULTI_MAX_CLIENTS; j++) {
			if (mc->clients[j].active && !mc->clients[j].minimized) {
				mc->focused_slot = j;
				break;
			}
		}
		multi_compositor_update_input_forward(mc);
	}
	if (mc->drag.active && mc->drag.slot == slot_index) {
		mc->drag.active = false;
		mc->drag.slot = -1;
	}

	U_LOG_W("Multi-comp: removed capture client from slot %d (total=%u, captures=%u)",
	         slot_index, mc->client_count, mc->capture_client_count);

	// Stop render timer only when all clients (capture and IPC) are gone.
	// IPC-only sessions now also rely on this thread for smooth workspace UI.
	if (mc->capture_client_count == 0 && mc->client_count == 0) {
		// Don't join from render thread — stop async
		mc->capture_render_running.store(false);
	}

	// Render one final frame to clear stale content
	multi_compositor_render(sys);

	return true;
}

/*!
 * Destroy the multi-compositor and all its resources.
 */
static void
multi_compositor_destroy(struct d3d11_multi_compositor *mc)
{
	if (mc == nullptr) {
		return;
	}

	U_LOG_W("Multi-comp: destroying");

	// Stop capture render timer
	mc->capture_render_running.store(false);
	if (mc->capture_render_thread.joinable()) {
		mc->capture_render_thread.join();
	}

	// Restore and stop all capture clients
	for (int i = 0; i < D3D11_MULTI_MAX_CLIENTS; i++) {
		if (mc->clients[i].active && mc->clients[i].client_type == CLIENT_TYPE_CAPTURE) {
			d3d11_capture_stop(mc->clients[i].capture_ctx);
			mc->clients[i].capture_ctx = nullptr;
			mc->clients[i].capture_srv = nullptr;
			mc->clients[i].active = false;
		}
		// Phase 2.C: drop chrome refs across all slots on shutdown.
		if (mc->clients[i].chrome_xsc != nullptr) {
			xrt_swapchain_reference(&mc->clients[i].chrome_xsc, NULL);
			mc->clients[i].chrome_swapchain_id = 0;
			mc->clients[i].chrome_layout_valid = false;
		}
	}

	if (mc->display_processor != nullptr) {
		xrt_display_processor_d3d11_request_display_mode(mc->display_processor, false);
		xrt_display_processor_d3d11_destroy(&mc->display_processor);
	}

	mc->back_buffer_rtv.reset();
	mc->combined_atlas_rtv.reset();
	mc->combined_atlas_srv.reset();
	mc->combined_atlas.reset();
	mc->combined_atlas_dsv.reset();
	mc->combined_atlas_depth.reset();
	mc->font_atlas_srv.reset();
	mc->font_atlas.reset();
	mc->logo_srv.reset();
	mc->swap_chain.reset();

	if (mc->window != nullptr) {
		comp_d3d11_window_destroy(&mc->window);
	}
	mc->hwnd = nullptr;

	delete mc;
}

/*!
 * Lazily create the multi-compositor window, swap chain, combined atlas, and DP.
 *
 * Called on first layer_commit in workspace mode. By this time the target builder
 * has already set dp_factory_d3d11.
 */
static xrt_result_t
multi_compositor_ensure_output(struct d3d11_service_system *sys)
{
	// Serialize multi-comp init — multiple IPC client threads can call this
	// concurrently when clients connect simultaneously (e.g., workspace launching
	// D3D11 + VK apps). Without this lock, both threads create the display
	// processor, causing SR SDK state corruption and crash.
	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);

	if (sys->multi_comp == nullptr) {
		sys->multi_comp = new d3d11_multi_compositor();
		std::memset(sys->multi_comp, 0, sizeof(*sys->multi_comp));
		sys->multi_comp->focused_slot = -1;
		sys->multi_comp->focused_slot_last_emitted = -1;
	}

	struct d3d11_multi_compositor *mc = sys->multi_comp;

	// Already initialized?
	if (mc->hwnd != nullptr && mc->swap_chain) {
		return XRT_SUCCESS;
	}

	U_LOG_W("Multi-comp: creating output window and resources");

	// Create window at display native resolution (not atlas size).
	// The window goes fullscreen on the Leia display; using native res avoids
	// the DP dim mismatch teardown/recreate path.
	uint32_t win_w = sys->base.info.display_pixel_width;
	uint32_t win_h = sys->base.info.display_pixel_height;
	if (win_w == 0 || win_h == 0) {
		win_w = sys->output_width;
		win_h = sys->output_height;
	}
	xrt_result_t wret = comp_d3d11_window_create(win_w, win_h, &mc->window);
	if (wret != XRT_SUCCESS || mc->window == nullptr) {
		U_LOG_E("Multi-comp: failed to create window");
		return XRT_ERROR_D3D11;
	}
	mc->hwnd = (HWND)comp_d3d11_window_get_hwnd(mc->window);
	sys->compositor_hwnd = mc->hwnd;
	// Seed the window's workspace-mode flag from current sys state (service_set_workspace_mode
	// no-ops while multi_comp is null, so earlier activation hasn't reached the window).
	comp_d3d11_window_set_workspace_mode_active(mc->window, sys->workspace_mode);

	if (sys->xsysd != nullptr) {
		comp_d3d11_window_set_system_devices(mc->window, sys->xsysd);
	}

	// Get actual window client area
	uint32_t actual_w = sys->output_width;
	uint32_t actual_h = sys->output_height;
	RECT cr;
	if (GetClientRect(mc->hwnd, &cr)) {
		uint32_t cw = static_cast<uint32_t>(cr.right - cr.left);
		uint32_t ch = static_cast<uint32_t>(cr.bottom - cr.top);
		if (cw > 0 && ch > 0) {
			actual_w = cw;
			actual_h = ch;
		}
	}

	// Create swap chain
	DXGI_SWAP_CHAIN_DESC1 sc_desc = {};
	sc_desc.Width = actual_w;
	sc_desc.Height = actual_h;
	sc_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sc_desc.SampleDesc.Count = 1;
	sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sc_desc.BufferCount = 2;
	sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	// IGNORE so DWM doesn't composite the desktop through the bound HWND (#163).
	sc_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

	HRESULT hr = sys->dxgi_factory->CreateSwapChainForHwnd(
	    sys->device.get(), mc->hwnd, &sc_desc, nullptr, nullptr,
	    mc->swap_chain.put());
	if (FAILED(hr)) {
		U_LOG_E("Multi-comp: failed to create swap chain (hr=0x%08X)", hr);
		return XRT_ERROR_D3D11;
	}

	// Back buffer RTV
	{
		wil::com_ptr<ID3D11Texture2D> bb;
		mc->swap_chain->GetBuffer(0, IID_PPV_ARGS(bb.put()));
		sys->device->CreateRenderTargetView(bb.get(), nullptr, mc->back_buffer_rtv.put());
	}

	// Combined atlas texture (native display size to hold fullscreen app content)
	{
		uint32_t ca_w = sys->base.info.display_pixel_width;
		uint32_t ca_h = sys->base.info.display_pixel_height;
		if (ca_w == 0 || ca_h == 0) {
			ca_w = sys->display_width;
			ca_h = sys->display_height;
		}
		D3D11_TEXTURE2D_DESC atlas_desc = {};
		atlas_desc.Width = ca_w;
		atlas_desc.Height = ca_h;
		atlas_desc.MipLevels = 1;
		atlas_desc.ArraySize = 1;
		atlas_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		atlas_desc.SampleDesc.Count = 1;
		atlas_desc.Usage = D3D11_USAGE_DEFAULT;
		atlas_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

		hr = sys->device->CreateTexture2D(&atlas_desc, nullptr, mc->combined_atlas.put());
		if (FAILED(hr)) {
			U_LOG_E("Multi-comp: failed to create combined atlas (hr=0x%08X)", hr);
			return XRT_ERROR_D3D11;
		}
		sys->device->CreateShaderResourceView(mc->combined_atlas.get(), nullptr, mc->combined_atlas_srv.put());
		sys->device->CreateRenderTargetView(mc->combined_atlas.get(), nullptr, mc->combined_atlas_rtv.put());

		// Phase 2.K: depth target sibling (D32_FLOAT). Per-eye tiles share
		// the same depth buffer — depth values stay isolated per-pixel via
		// the tile coordinates, so no per-eye depth target is needed.
		D3D11_TEXTURE2D_DESC depth_desc = {};
		depth_desc.Width = ca_w;
		depth_desc.Height = ca_h;
		depth_desc.MipLevels = 1;
		depth_desc.ArraySize = 1;
		depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
		depth_desc.SampleDesc.Count = 1;
		depth_desc.Usage = D3D11_USAGE_DEFAULT;
		depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		hr = sys->device->CreateTexture2D(&depth_desc, nullptr, mc->combined_atlas_depth.put());
		if (FAILED(hr)) {
			U_LOG_E("Multi-comp: failed to create combined atlas depth (hr=0x%08X)", hr);
			return XRT_ERROR_D3D11;
		}
		sys->device->CreateDepthStencilView(mc->combined_atlas_depth.get(), nullptr,
		                                    mc->combined_atlas_dsv.put());
	}

	U_LOG_W("Multi-comp: combined atlas %ux%u",
	        sys->base.info.display_pixel_width > 0 ? sys->base.info.display_pixel_width : sys->display_width,
	        sys->base.info.display_pixel_height > 0 ? sys->base.info.display_pixel_height : sys->display_height);

	// Create font atlas using DirectWrite (anti-aliased Segoe UI)
	if (!mc->font_atlas) {
		const uint32_t FONT_SIZE = 33;
		const uint32_t CELL_H = FONT_SIZE + 8; // padding for descenders
		const uint32_t GLYPH_COUNT = 96;

		// Measure glyph widths with DirectWrite
		wil::com_ptr<IDWriteFactory> dwrite_factory;
		wil::com_ptr<IDWriteTextFormat> text_format;
		bool dwrite_ok = false;

		HRESULT dw_hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
		    __uuidof(IDWriteFactory), (IUnknown **)dwrite_factory.put());
		if (SUCCEEDED(dw_hr)) {
			dw_hr = dwrite_factory->CreateTextFormat(
			    L"Segoe UI", nullptr,
			    DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL,
			    DWRITE_FONT_STRETCH_NORMAL, (float)FONT_SIZE, L"en-us",
			    text_format.put());
			if (SUCCEEDED(dw_hr)) {
				dwrite_ok = true;
			}
		}

		// Measure each glyph and compute atlas width
		uint32_t cell_w = FONT_SIZE; // fallback cell width
		uint32_t atlas_w = 0;
		if (dwrite_ok) {
			for (int g = 0; g < (int)GLYPH_COUNT; g++) {
				WCHAR ch = (WCHAR)(0x20 + g);
				wil::com_ptr<IDWriteTextLayout> layout;
				dwrite_factory->CreateTextLayout(&ch, 1, text_format.get(),
				    1000.0f, 1000.0f, layout.put());
				DWRITE_TEXT_METRICS metrics = {};
				if (layout) layout->GetMetrics(&metrics);
				float advance = (metrics.widthIncludingTrailingWhitespace > 0)
				    ? metrics.widthIncludingTrailingWhitespace : (float)FONT_SIZE * 0.5f;
				mc->glyph_advances[g] = advance;
				// Use ceiling for cell width
				uint32_t gw = (uint32_t)(advance + 1.5f);
				if (gw > cell_w) cell_w = gw;
				atlas_w += gw;
			}
		} else {
			// Fallback: uniform width
			atlas_w = GLYPH_COUNT * cell_w;
			for (int g = 0; g < (int)GLYPH_COUNT; g++)
				mc->glyph_advances[g] = (float)cell_w;
		}

		mc->font_glyph_w = cell_w;
		mc->font_glyph_h = CELL_H;
		mc->font_atlas_w = atlas_w;
		mc->font_atlas_h = CELL_H;

		// Create atlas texture (needs RENDER_TARGET for D2D)
		D3D11_TEXTURE2D_DESC font_desc = {};
		font_desc.Width = atlas_w;
		font_desc.Height = CELL_H;
		font_desc.MipLevels = 1;
		font_desc.ArraySize = 1;
		font_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		font_desc.SampleDesc.Count = 1;
		font_desc.Usage = D3D11_USAGE_DEFAULT;
		font_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

		hr = sys->device->CreateTexture2D(&font_desc, nullptr, mc->font_atlas.put());
		if (SUCCEEDED(hr) && dwrite_ok) {
			// Render glyphs via Direct2D onto the atlas texture
			wil::com_ptr<IDXGISurface> dxgi_surface;
			mc->font_atlas->QueryInterface(IID_PPV_ARGS(dxgi_surface.put()));

			wil::com_ptr<ID2D1Factory> d2d_factory;
			D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_factory.put());

			D2D1_RENDER_TARGET_PROPERTIES rt_props = D2D1::RenderTargetProperties(
			    D2D1_RENDER_TARGET_TYPE_DEFAULT,
			    D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

			wil::com_ptr<ID2D1RenderTarget> rt;
			d2d_factory->CreateDxgiSurfaceRenderTarget(dxgi_surface.get(), &rt_props, rt.put());

			if (rt) {
				wil::com_ptr<ID2D1SolidColorBrush> brush;
				rt->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), brush.put());

				rt->BeginDraw();
				rt->Clear(D2D1::ColorF(0, 0, 0, 0)); // transparent background

				float x_cursor = 0;
				for (int g = 0; g < (int)GLYPH_COUNT; g++) {
					WCHAR ch = (WCHAR)(0x20 + g);
					float gw = mc->glyph_advances[g];
					D2D1_RECT_F rect = {x_cursor, 0, x_cursor + gw, (float)CELL_H};
					wil::com_ptr<IDWriteTextLayout> layout;
					dwrite_factory->CreateTextLayout(&ch, 1, text_format.get(),
					    gw, (float)CELL_H, layout.put());
					if (layout) {
						rt->DrawTextLayout(D2D1::Point2F(x_cursor, 2.0f),
						    layout.get(), brush.get());
					}
					x_cursor += gw;
				}

				rt->EndDraw();
			}

			sys->device->CreateShaderResourceView(mc->font_atlas.get(), nullptr,
			    mc->font_atlas_srv.put());
			U_LOG_W("Multi-comp: DirectWrite font atlas created (%ux%u, Segoe UI %upx)",
			        atlas_w, CELL_H, FONT_SIZE);
		} else if (SUCCEEDED(hr)) {
			// DWrite failed — fall back to bitmap font
			U_LOG_W("Multi-comp: DirectWrite unavailable, using bitmap font fallback");
			mc->font_atlas_w = BITMAP_FONT_ATLAS_W;
			mc->font_atlas_h = BITMAP_FONT_ATLAS_H;
			mc->font_glyph_w = BITMAP_FONT_GLYPH_W;
			mc->font_glyph_h = BITMAP_FONT_GLYPH_H;
			for (int g = 0; g < (int)GLYPH_COUNT; g++)
				mc->glyph_advances[g] = (float)BITMAP_FONT_GLYPH_W;

			// Recreate as immutable with bitmap data
			mc->font_atlas.reset();
			font_desc.Usage = D3D11_USAGE_IMMUTABLE;
			font_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			font_desc.Width = BITMAP_FONT_ATLAS_W;
			font_desc.Height = BITMAP_FONT_ATLAS_H;

			uint32_t *pixels = new uint32_t[BITMAP_FONT_ATLAS_W * BITMAP_FONT_ATLAS_H];
			std::memset(pixels, 0, BITMAP_FONT_ATLAS_W * BITMAP_FONT_ATLAS_H * sizeof(uint32_t));
			for (int g = 0; g < BITMAP_FONT_GLYPH_COUNT; g++) {
				for (int row = 0; row < BITMAP_FONT_GLYPH_H; row++) {
					uint8_t bits = bitmap_font_8x16[g][row];
					for (int bit = 0; bit < BITMAP_FONT_GLYPH_W; bit++) {
						if (bits & (0x80 >> bit)) {
							pixels[row * BITMAP_FONT_ATLAS_W + g * BITMAP_FONT_GLYPH_W + bit] = 0xFFFFFFFF;
						}
					}
				}
			}
			D3D11_SUBRESOURCE_DATA init_data = {};
			init_data.pSysMem = pixels;
			init_data.SysMemPitch = BITMAP_FONT_ATLAS_W * sizeof(uint32_t);
			sys->device->CreateTexture2D(&font_desc, &init_data, mc->font_atlas.put());
			if (mc->font_atlas)
				sys->device->CreateShaderResourceView(mc->font_atlas.get(), nullptr, mc->font_atlas_srv.put());
			delete[] pixels;
		} else {
			U_LOG_E("Multi-comp: failed to create font atlas texture (hr=0x%08X)", hr);
		}
	}

	// Create display processor via factory
	if (sys->base.info.dp_factory_d3d11 != NULL) {
		auto factory = (xrt_dp_factory_d3d11_fn_t)sys->base.info.dp_factory_d3d11;
		xrt_result_t dp_ret = factory(
		    sys->device.get(), sys->context.get(), mc->hwnd, &mc->display_processor);

		if (dp_ret == XRT_SUCCESS && mc->display_processor != nullptr) {
			U_LOG_W("Multi-comp: display processor created");

			// Store DP on window for ESC/close 2D mode switch
			if (mc->window != nullptr) {
				comp_d3d11_window_set_workspace_dp(mc->window, mc->display_processor);
			}

			// Check if DP reports different dimensions than our window
			uint32_t dp_px_w = 0, dp_px_h = 0;
			int32_t dp_left = 0, dp_top = 0;
			if (xrt_display_processor_d3d11_get_display_pixel_info(
			        mc->display_processor, &dp_px_w, &dp_px_h, &dp_left, &dp_top) &&
			    dp_px_w > 0 && dp_px_h > 0 &&
			    (dp_px_w != actual_w || dp_px_h != actual_h)) {

				U_LOG_W("Multi-comp: DP reports %ux%u but window is %ux%u - recreating",
				        dp_px_w, dp_px_h, actual_w, actual_h);

				// Teardown and recreate at correct size
				xrt_display_processor_d3d11_destroy(&mc->display_processor);
				mc->back_buffer_rtv.reset();
				mc->swap_chain.reset();
				comp_d3d11_window_destroy(&mc->window);

				// Recreate window at DP-reported size
				wret = comp_d3d11_window_create(dp_px_w, dp_px_h, &mc->window);
				if (wret != XRT_SUCCESS || mc->window == nullptr) {
					U_LOG_E("Multi-comp: failed to recreate window at %ux%u", dp_px_w, dp_px_h);
					return XRT_ERROR_D3D11;
				}
				mc->hwnd = (HWND)comp_d3d11_window_get_hwnd(mc->window);
				sys->compositor_hwnd = mc->hwnd;
				comp_d3d11_window_set_workspace_mode_active(mc->window, sys->workspace_mode);

				if (sys->xsysd != nullptr) {
					comp_d3d11_window_set_system_devices(mc->window, sys->xsysd);
				}

				// Update actual dims
				actual_w = dp_px_w;
				actual_h = dp_px_h;
				if (GetClientRect(mc->hwnd, &cr)) {
					uint32_t cw2 = static_cast<uint32_t>(cr.right - cr.left);
					uint32_t ch2 = static_cast<uint32_t>(cr.bottom - cr.top);
					if (cw2 > 0 && ch2 > 0) {
						actual_w = cw2;
						actual_h = ch2;
					}
				}

				// Update system output dims
				sys->output_width = actual_w;
				sys->output_height = actual_h;

				// Recreate swap chain
				sc_desc.Width = actual_w;
				sc_desc.Height = actual_h;
				hr = sys->dxgi_factory->CreateSwapChainForHwnd(
				    sys->device.get(), mc->hwnd, &sc_desc, nullptr, nullptr,
				    mc->swap_chain.put());
				if (FAILED(hr)) {
					U_LOG_E("Multi-comp: failed to recreate swap chain (hr=0x%08X)", hr);
					return XRT_ERROR_D3D11;
				}

				wil::com_ptr<ID3D11Texture2D> bb;
				mc->swap_chain->GetBuffer(0, IID_PPV_ARGS(bb.put()));
				sys->device->CreateRenderTargetView(bb.get(), nullptr, mc->back_buffer_rtv.put());

				// Recreate DP with new window
				dp_ret = factory(sys->device.get(), sys->context.get(), mc->hwnd, &mc->display_processor);
				if (dp_ret != XRT_SUCCESS) {
					U_LOG_E("Multi-comp: failed to recreate DP");
				}

				U_LOG_W("Multi-comp: recreated at %ux%u", actual_w, actual_h);
			}

			// Phase 6.1 (#140): do NOT call request_display_mode(true) here.
			// The SR SDK's internal init cycle responds to an immediate
			// mode switch by toggling 3D→2D→3D over several seconds,
			// causing a stretched-left-eye artifact. Instead, let the
			// display come up in whatever mode it's already in (typically
			// 3D if eye tracking is running). The user can toggle via V
			// key, and sync_tile_layout will track the actual mode each
			// frame. The qwerty V-key handler and the
			// xrRequestDisplayRenderingModeEXT path remain the
			// authoritative mode-switch triggers.
		} else {
			U_LOG_W("Multi-comp: no display processor (factory returned %d)", dp_ret);
		}
	}

	// Load cursor handles for resize feedback
	mc->cursor_arrow = LoadCursor(NULL, IDC_ARROW);
	mc->cursor_sizewe = LoadCursor(NULL, IDC_SIZEWE);
	mc->cursor_sizens = LoadCursor(NULL, IDC_SIZENS);
	mc->cursor_sizenwse = LoadCursor(NULL, IDC_SIZENWSE);
	mc->cursor_sizenesw = LoadCursor(NULL, IDC_SIZENESW);
	mc->cursor_sizeall = LoadCursor(NULL, IDC_SIZEALL);

	U_LOG_W("Multi-comp: output ready (window=%p, swap_chain=%p)", mc->hwnd, mc->swap_chain.get());
	return XRT_SUCCESS;
}

/*!
 * Result of spatial raycasting hit-test against workspace windows.
 */
struct workspace_hit_result
{
	int slot;            //!< Hit window slot (-1 = no hit)
	bool in_title_bar;   //!< Hit is in the title bar region (pill bg)
	bool in_grip_handle; //!< Hit is on the 8-dot grip in the pill center (drag/rotate region)
	bool in_close_btn;    //!< Hit is on the close button
	bool in_minimize_btn; //!< Hit is on the minimize button
	bool in_maximize_btn; //!< Hit is on the maximize/fullscreen button
	bool in_content;     //!< Hit is in the content area
	float local_x_m;    //!< Hit point in window-local meters (0 = left edge)
	float local_y_m;    //!< Hit point in window-local meters (0 = top of title bar, positive down)
	int edge_flags;      //!< RESIZE_LEFT|RIGHT|TOP|BOTTOM if near edge
};

static void launcher_set_visible(struct d3d11_service_system *sys,
                                 struct d3d11_multi_compositor *mc, bool visible);

// Phase 5.13: pop a Win32 context menu at the cursor for a launcher tile.
// Launch fires the tile like a click; Remove sets hidden_tile_mask so the
// tile disappears from the grid until the workspace re-pushes its registry.
//
// TrackPopupMenu only runs on the thread that owns the target window, so
// we dispatch via comp_d3d11_window_show_launcher_context_menu which
// SendMessages across to the window thread. The render thread blocks
// until the user picks or cancels.
static void
launcher_show_context_menu(struct d3d11_service_system *sys,
                           struct d3d11_multi_compositor *mc,
                           POINT client_pt, int full_idx)
{
	(void)client_pt;
	if (mc == nullptr || mc->window == nullptr) return;

	uint32_t result = comp_d3d11_window_show_launcher_context_menu(mc->window);
	switch (result) {
	case LAUNCHER_CTX_MENU_RESULT_LAUNCH:
		sys->pending_launcher_click_index = full_idx;
		launcher_set_visible(sys, mc, false);
		U_LOG_W("Launcher: context menu launch full=%d", full_idx);
		break;
	case LAUNCHER_CTX_MENU_RESULT_REMOVE:
		// Phase 6.6: signal the workspace to permanently remove this app from
		// registered_apps.json. The workspace's poll loop picks up the index,
		// deletes the entry, saves, and re-pushes the registry. The
		// launcher hides so the re-pushed list renders cleanly.
		sys->pending_launcher_remove_full_index = full_idx;
		launcher_set_visible(sys, mc, false);
		U_LOG_W("Launcher: context menu remove full=%d (permanent)", full_idx);
		break;
	default:
		break;
	}
}

// Phase 5.12: toggle launcher visibility AND the window's input-suppress
// flag in one place. When the launcher is up we want keyboard input to
// drive the launcher itself (arrows / Enter / Esc) rather than leaking
// through to the focused app; mirror the existing resize/drag pattern
// which uses `comp_d3d11_window_set_input_suppress` for the same purpose.
static void
launcher_set_visible(struct d3d11_service_system *sys,
                     struct d3d11_multi_compositor *mc, bool visible)
{
	if (mc == nullptr) return;
	if (mc->launcher_visible == visible) return;

	mc->launcher_visible = visible;
	if (mc->window != nullptr) {
		if (visible) {
			comp_d3d11_window_set_input_suppress(mc->window, true);
		} else {
			// Phase 5.12: set the grace-period timestamp BEFORE clearing
			// the immediate flag. If we cleared the flag first and then
			// set the grace, the window thread could sample between the
			// two writes and see neither active → forward the Esc that
			// triggered the close. 200ms covers any WM_KEYDOWN queued on
			// the window thread before the render thread got here.
			comp_d3d11_window_set_input_suppress_grace_ms(mc->window, 200);
			comp_d3d11_window_set_input_suppress(mc->window, false);
		}
	}
	// Phase 5.12: when opening, force keyboard focus onto the compositor
	// window. Without this, keys still route to whichever app previously
	// had focus (the cube) and never reach the WndProc that the launcher's
	// input-suppress gate lives in. The workspace process grants us
	// foreground-activation permission via AllowSetForegroundWindow before
	// firing this IPC.
	if (visible && mc->hwnd != nullptr) {
		SetForegroundWindow(mc->hwnd);
		SetFocus(mc->hwnd);
	}
	if (visible) {
		sys->launcher_selected_index = 0;
		sys->launcher_scroll_row = 0;
	} else {
		sys->launcher_selected_index = -1;
	}
}

// Phase 5.12+5.13+5.14: build the compacted visible-tile remap for the
// launcher. Walks sys->launcher_apps skipping any bit set in hidden_tile_mask
// and writes the full-space indices into @p out. The render pass and hit
// test share this so the grid positions agree across the frame.
//
// Returns the number of visible tiles written. Does NOT include the virtual
// "Add app…" Browse tile — that gets slot [n_visible] in both the render and
// hit-test code paths.
static uint32_t
launcher_build_visible_list(const struct d3d11_service_system *sys,
                            int out_visible_to_full[IPC_LAUNCHER_MAX_APPS])
{
	uint32_t n_apps = sys->launcher_app_count;
	if (n_apps > IPC_LAUNCHER_MAX_APPS) n_apps = IPC_LAUNCHER_MAX_APPS;
	uint32_t n_visible = 0;
	for (uint32_t i = 0; i < n_apps; i++) {
		if ((sys->hidden_tile_mask & (1ULL << i)) != 0) continue;
		out_visible_to_full[n_visible++] = (int)i;
	}
	return n_visible;
}

/*!
 * Phase 5.9 / 5.13 / 5.14: hit test the launcher grid against a cursor.
 *
 * The launcher panel sits at z=0 in display coordinates (zero-disparity plane),
 * so the cursor position on the workspace window converts directly to display
 * meters — no eye-projection raycast needed. Mirrors the layout math used by
 * the render pass so the visible tiles align with the hit boxes.
 *
 * Returns:
 *   - 0..n_visible-1      real tile hit (visible-space index, caller maps to full)
 *   - n_visible           the virtual "Add app…" Browse tile
 *   - -1                  inside panel but on a gap / title / header
 *   - -2                  outside panel entirely (caller treats as dismiss)
 */
static int
launcher_hit_test(struct d3d11_service_system *sys, POINT cursor_px, uint32_t n_visible)
{
	// Cursor is in compositor-window client pixels, which we map to
	// display meters via the same formula the taskbar hit-test uses
	// (pt.x and pt.y are assumed to span the full atlas pixel dimensions).
	float disp_w_m = sys->base.info.display_width_m;
	float disp_h_m = sys->base.info.display_height_m;
	uint32_t disp_px_w = sys->base.info.display_pixel_width;
	uint32_t disp_px_h = sys->base.info.display_pixel_height;
	if (disp_w_m <= 0.0f) disp_w_m = 0.700f;
	if (disp_h_m <= 0.0f) disp_h_m = 0.394f;
	if (disp_px_w == 0) disp_px_w = 3840;
	if (disp_px_h == 0) disp_px_h = 2160;

	float cursor_x_m = ((float)cursor_px.x - (float)disp_px_w / 2.0f) *
	                   disp_w_m / (float)disp_px_w;
	float cursor_y_m = ((float)disp_px_h / 2.0f - (float)cursor_px.y) *
	                   disp_h_m / (float)disp_px_h;

	// Panel geometry — the fractions below must match the render block.
	const float panel_w_frac = 0.60f;
	const float panel_h_frac = 0.55f;
	float panel_w_m = disp_w_m * panel_w_frac;
	float panel_h_m = disp_h_m * panel_h_frac;

	// Outside panel → caller treats as dismiss.
	if (cursor_x_m < -panel_w_m * 0.5f || cursor_x_m > panel_w_m * 0.5f ||
	    cursor_y_m < -panel_h_m * 0.5f || cursor_y_m > panel_h_m * 0.5f) {
		return -2;
	}

	// Panel-local meters, origin top-left.
	float lx = cursor_x_m + panel_w_m * 0.5f;
	float ly = panel_h_m * 0.5f - cursor_y_m;

	const int LAUNCHER_GRID_COLS = 4;
	float margin = panel_w_m * 0.04f;
	float title_h = panel_h_m * 0.13f;
	float section_h = panel_h_m * 0.07f;
	float tile_w = (panel_w_m - (LAUNCHER_GRID_COLS + 1) * margin) /
	               (float)LAUNCHER_GRID_COLS;

	// Phase 5.14: the render computes tile_h in render-pixel terms
	// (`tile_h_px = tile_w_px * 0.65`). Because ui_m_to_tile_px_x and
	// ui_m_to_tile_px_y use different px-per-meter ratios (tile_px_w =
	// display_pixel_width/tile_columns, but tile_px_h = display_pixel_height
	// /tile_rows), converting that back to meters requires an aspect-ratio
	// correction or the hit test and render disagree on row positions.
	float tile_px_w_eff = (float)(disp_px_w / (sys->tile_columns ? sys->tile_columns : 1));
	float tile_px_h_eff = (float)(disp_px_h / (sys->tile_rows    ? sys->tile_rows    : 1));
	float px_per_m_x = tile_px_w_eff / disp_w_m;
	float px_per_m_y = tile_px_h_eff / disp_h_m;
	float y_ratio = (px_per_m_y > 0.0f) ? (px_per_m_x / px_per_m_y) : 1.0f;
	float tile_h = tile_w * y_ratio;

	// Section header + label heights: render uses
	// `font_glyph_h * section_scale` where section_scale = (section_h_px * 0.65)
	// / font_glyph_h. In meters that's section_h_m * 0.65, unaffected by the
	// X/Y ratio (section_h is panel_h-derived, so y-based).
	float section_text_h = section_h * 0.65f;
	float label_h = section_text_h * 0.85f;

	float section_y = title_h + margin * 0.5f;
	float grid_top = section_y + section_text_h + margin * 0.5f;

	// Phase 6.5: apply scroll offset to grid_top (in meters, matching the
	// render's pixel-based scroll). row_h uses the same y_ratio-corrected
	// tile_h so scroll offsets agree between render and hit test.
	float row_h_m = tile_h + label_h + margin;
	grid_top -= (float)sys->launcher_scroll_row * row_h_m;

	// Walk the visible tile grid PLUS one virtual Browse tile at position n_visible.
	uint32_t n_total = n_visible + 1;
	if (n_total > IPC_LAUNCHER_MAX_APPS + 1) n_total = IPC_LAUNCHER_MAX_APPS + 1;
	for (uint32_t i = 0; i < n_total; i++) {
		int tcol = (int)(i % LAUNCHER_GRID_COLS);
		int trow = (int)(i / LAUNCHER_GRID_COLS);
		float tx = margin + (float)tcol * (tile_w + margin);
		float ty = grid_top + (float)trow * (tile_h + label_h + margin);
		// Only match tiles that are in the visible panel area.
		if (ty + tile_h < section_y + section_text_h) continue;
		if (ty > panel_h_m) continue;
		if (lx >= tx && lx <= tx + tile_w &&
		    ly >= ty && ly <= ty + tile_h) {
			return (int)i;
		}
	}

	// Inside panel but not on a tile (e.g. title bar, gaps).
	return -1;
}

/*!
 * Spatial raycast hit-test: cast a ray from the user's eye through the mouse
 * cursor position on the display surface, and intersect with workspace window planes.
 *
 * Each window is a 3D rectangle defined by (pose, width_m, height_m).
 * The display is at Z=0 with known physical dimensions.
 * The eye position comes from the display processor's face tracking.
 *
 * This approach is tiling-independent and future-proofs for angled 3D windows.
 */
static struct workspace_hit_result
workspace_raycast_hit_test(struct d3d11_service_system *sys,
                       struct d3d11_multi_compositor *mc,
                       POINT cursor_px)
{
	struct workspace_hit_result result = {};
	result.slot = -1;

	float disp_w_m = sys->base.info.display_width_m;
	float disp_h_m = sys->base.info.display_height_m;
	uint32_t disp_px_w = sys->base.info.display_pixel_width;
	uint32_t disp_px_h = sys->base.info.display_pixel_height;
	if (disp_w_m <= 0.0f) disp_w_m = 0.700f;
	if (disp_h_m <= 0.0f) disp_h_m = 0.394f;
	if (disp_px_w == 0) disp_px_w = 3840;
	if (disp_px_h == 0) disp_px_h = 2160;

	float m_per_px_x = disp_w_m / (float)disp_px_w;
	float m_per_px_y = disp_h_m / (float)disp_px_h;

	// Step 1: Convert mouse pixel to 3D point on display surface (Z=0 plane)
	float point_x = ((float)cursor_px.x - (float)disp_px_w / 2.0f) * m_per_px_x;
	float point_y = ((float)disp_px_h / 2.0f - (float)cursor_px.y) * m_per_px_y;
	float point_z = 0.0f;

	// Step 2: Get eye position for ray origin
	struct xrt_vec3 eye_left = {0, 0, 0.6f}; // Fallback: 60cm from display
	struct xrt_vec3 eye_right = {0, 0, 0.6f};
	comp_d3d11_service_get_predicted_eye_positions(&sys->base, &eye_left, &eye_right);
	// Use center eye
	float eye_x = (eye_left.x + eye_right.x) / 2.0f;
	float eye_y = (eye_left.y + eye_right.y) / 2.0f;
	float eye_z = (eye_left.z + eye_right.z) / 2.0f;
	if (eye_z <= 0.001f) eye_z = 0.6f; // Safety: eye must be in front of display

	// Ray: origin = eye, direction = (display_point - eye)
	float ray_dx = point_x - eye_x;
	float ray_dy = point_y - eye_y;
	float ray_dz = point_z - eye_z; // Negative (toward display)

	// UI dimensions in meters (spatial constants — single source of truth)
	float title_bar_h_m = UI_TITLE_BAR_H_M;
	float btn_w_m = UI_BTN_W_M;
	float resize_zone_m = UI_RESIZE_ZONE_M;

	// Step 3: Test each window (focused last = highest z-priority, test first)
	// Build reverse render order: focused first, then others
	int test_order[D3D11_MULTI_MAX_CLIENTS];
	int test_count = 0;
	if (mc->focused_slot >= 0 && mc->focused_slot < D3D11_MULTI_MAX_CLIENTS &&
	    mc->clients[mc->focused_slot].active && !mc->clients[mc->focused_slot].minimized) {
		test_order[test_count++] = mc->focused_slot;
	}
	for (int i = 0; i < D3D11_MULTI_MAX_CLIENTS; i++) {
		if (i == mc->focused_slot) continue;
		if (mc->clients[i].active && !mc->clients[i].minimized) {
			test_order[test_count++] = i;
		}
	}

	for (int ti = 0; ti < test_count; ti++) {
		int s = test_order[ti];
		float win_x = mc->clients[s].window_pose.position.x;
		float win_y = mc->clients[s].window_pose.position.y;
		float win_z = mc->clients[s].window_pose.position.z;
		float win_w = mc->clients[s].window_width_m;
		float win_h = mc->clients[s].window_height_m;
		const struct xrt_quat *win_q = &mc->clients[s].window_pose.orientation;
		bool rotated = !quat_is_identity(win_q);

		// Ray-plane intersection
		float t, hit_x, hit_y;
		if (rotated) {
			// Rotated window: compute plane normal from orientation
			struct xrt_vec3 normal_local = {0, 0, 1};
			struct xrt_vec3 normal;
			math_quat_rotate_vec3(win_q, &normal_local, &normal);
			// Plane equation: dot(normal, point - win_pos) = 0
			float ray_dot_n = ray_dx * normal.x + ray_dy * normal.y + ray_dz * normal.z;
			if (fabsf(ray_dot_n) < 1e-6f) continue; // Parallel
			float d = (win_x - eye_x) * normal.x + (win_y - eye_y) * normal.y + (win_z - eye_z) * normal.z;
			t = d / ray_dot_n;
			if (t < 0.0f) continue; // Behind eye
			float world_hit_x = eye_x + t * ray_dx;
			float world_hit_y = eye_y + t * ray_dy;
			float world_hit_z = eye_z + t * ray_dz;
			// Convert world hit to window-local coords via inverse rotation
			struct xrt_vec3 delta = {world_hit_x - win_x, world_hit_y - win_y, world_hit_z - win_z};
			struct xrt_quat inv_q;
			math_quat_invert(win_q, &inv_q);
			struct xrt_vec3 local_hit;
			math_quat_rotate_vec3(&inv_q, &delta, &local_hit);
			// local_hit.x/y are window-local coords centered at window center
			hit_x = win_x + local_hit.x; // project back to flat coords for bounds check
			hit_y = win_y + local_hit.y;
		} else {
			// Flat window: simple Z-plane intersection
			if (fabsf(ray_dz) < 1e-6f) continue;
			t = (win_z - eye_z) / ray_dz;
			if (t < 0.0f) continue;
			hit_x = eye_x + t * ray_dx;
			hit_y = eye_y + t * ray_dy;
		}

		// Window bounds (content area)
		float win_left = win_x - win_w / 2.0f;
		float win_right = win_x + win_w / 2.0f;
		float win_bottom = win_y - win_h / 2.0f;
		float win_top = win_y + win_h / 2.0f;

		// Phase 2.K Commit 8.B: floating-pill geometry — must mirror the
		// render code so click-targets line up with what the user sees.
		// Pill is 75% of content width (centered) and floats above the
		// content quad with a half-tb-height gap. Buttons live at the
		// pill's right edge (not the content's right edge).
		bool has_workspace_title_bar = (mc->clients[s].client_type != CLIENT_TYPE_CAPTURE &&
		                            !mc->clients[s].maximized);
		const float PILL_W_FRAC_HT = 0.75f;
		float pill_w_m = win_w * PILL_W_FRAC_HT;
		float pill_left = win_x - pill_w_m / 2.0f;
		float pill_right = win_x + pill_w_m / 2.0f;
		float pill_gap_m = title_bar_h_m * 0.5f;
		float pill_bot = win_top + (has_workspace_title_bar ? pill_gap_m : 0.0f);
		float pill_top = pill_bot + (has_workspace_title_bar ? title_bar_h_m : 0.0f);

		// Outer hover bounds (content + gap + pill). Resize zones extend
		// outward from the content rect on all four sides.
		float ext_top = has_workspace_title_bar ? pill_top : win_top;

		// Check if hit is within extended window bounds (including resize zone)
		if (hit_x >= win_left - resize_zone_m && hit_x < win_right + resize_zone_m &&
		    hit_y >= win_bottom - resize_zone_m && hit_y < ext_top + resize_zone_m) {

			// Window-local coordinates relative to top of the pill (or
			// content for capture clients), positive down/right.
			float local_x = hit_x - win_left;
			float local_y = ext_top - hit_y;

			result.slot = s;
			result.local_x_m = local_x;
			result.local_y_m = local_y;

			// Classify hit region.
			if (has_workspace_title_bar) {
				bool in_pill = (hit_x >= pill_left && hit_x < pill_right &&
				                hit_y >= pill_bot && hit_y < pill_top);
				bool in_content = (hit_x >= win_left && hit_x < win_right &&
				                   hit_y >= win_bottom && hit_y < win_top);
				result.in_title_bar = in_pill;
				result.in_content = in_content;
			} else {
				// Capture clients: top strip of content acts as drag zone.
				bool in_window = (hit_x >= win_left && hit_x < win_right &&
				                  hit_y >= win_bottom && hit_y < win_top);
				result.in_title_bar = in_window && (local_y < title_bar_h_m);
				result.in_content = in_window && (local_y >= title_bar_h_m);
			}

			if (result.in_title_bar && has_workspace_title_bar) {
				// Buttons live at the pill's right edge — local-x is
				// measured relative to the pill, not the window.
				float pill_local_x = hit_x - pill_left;
				result.in_close_btn = (pill_local_x >= pill_w_m - btn_w_m);
				result.in_minimize_btn = !result.in_close_btn &&
				                         (pill_local_x >= pill_w_m - 2.0f * btn_w_m);
				result.in_maximize_btn = !result.in_close_btn && !result.in_minimize_btn &&
				                         (pill_local_x >= pill_w_m - 3.0f * btn_w_m);

				// Grip handle: 8-dot grid centered in the pill (4 cols × 2
				// rows, 1 mm dots with 1 mm gaps → 7 mm × 3 mm). Mirrors
				// chrome-render geometry — drag/rotate is only valid here.
				const float DOT_SIZE_M = 0.001f;
				const float DOT_GAP_M  = 0.001f;
				const int   GRIP_COLS  = 4;
				const int   GRIP_ROWS  = 2;
				float grip_w_m = (float)GRIP_COLS * DOT_SIZE_M +
				                 (float)(GRIP_COLS - 1) * DOT_GAP_M;
				float grip_h_m = (float)GRIP_ROWS * DOT_SIZE_M +
				                 (float)(GRIP_ROWS - 1) * DOT_GAP_M;
				float grip_cx = (pill_left + pill_right) * 0.5f;
				float grip_cy = (pill_bot + pill_top) * 0.5f;
				float grip_left = grip_cx - grip_w_m * 0.5f;
				float grip_right = grip_cx + grip_w_m * 0.5f;
				float grip_bot = grip_cy - grip_h_m * 0.5f;
				float grip_top = grip_cy + grip_h_m * 0.5f;
				if (!result.in_close_btn && !result.in_minimize_btn && !result.in_maximize_btn &&
				    hit_x >= grip_left && hit_x < grip_right &&
				    hit_y >= grip_bot && hit_y < grip_top) {
					result.in_grip_handle = true;
				}
			} else if (result.in_title_bar) {
				// Capture-client compatibility (buttons relative to window).
				result.in_close_btn = (local_x >= win_w - btn_w_m);
				result.in_minimize_btn = !result.in_close_btn &&
				                         (local_x >= win_w - 2.0f * btn_w_m);
				result.in_maximize_btn = !result.in_close_btn && !result.in_minimize_btn &&
				                         (local_x >= win_w - 3.0f * btn_w_m);
			}

			// Edge detection (resize zones)
			result.edge_flags = RESIZE_NONE;
			if (hit_x < win_left + resize_zone_m) result.edge_flags |= RESIZE_LEFT;
			if (hit_x >= win_right - resize_zone_m) result.edge_flags |= RESIZE_RIGHT;
			if (hit_y < win_bottom + resize_zone_m) result.edge_flags |= RESIZE_BOTTOM;
			if (hit_y >= ext_top - resize_zone_m) result.edge_flags |= RESIZE_TOP;

			// If we're inside the window (not just in resize zone), clear edge flags
			// unless we're actually on the edge.
			bool inside_outer = (hit_x >= win_left && hit_x < win_right &&
			                     hit_y >= win_bottom && hit_y < ext_top);
			if (inside_outer && result.edge_flags == RESIZE_NONE) {
				result.edge_flags = RESIZE_NONE;
			}

			break; // First hit wins (z-priority order)
		}
	}

	return result;
}

/*!
 * Helper: create a quaternion from yaw (Y-axis rotation) in radians.
 */
static inline struct xrt_quat
quat_from_yaw(float yaw_rad)
{
	struct xrt_vec3 axis = {0.0f, 1.0f, 0.0f};
	struct xrt_quat q;
	math_quat_from_angle_vector(yaw_rad, &axis, &q);
	return q;
}

/*!
 * Helper: create a quaternion from yaw + pitch (Y then X axis) in radians.
 */
static inline struct xrt_quat
quat_from_yaw_pitch(float yaw_rad, float pitch_rad)
{
	struct xrt_vec3 y_axis = {0.0f, 1.0f, 0.0f};
	struct xrt_vec3 x_axis = {1.0f, 0.0f, 0.0f};
	struct xrt_quat qy, qp, result;
	math_quat_from_angle_vector(yaw_rad, &y_axis, &qy);
	math_quat_from_angle_vector(pitch_rad, &x_axis, &qp);
	math_quat_rotate(&qy, &qp, &result);
	return result;
}

/*!
 * Helper: check if a quaternion is identity (no rotation).
 */
static inline bool
quat_is_identity(const struct xrt_quat *q)
{
	return fabsf(q->x) < 0.0001f && fabsf(q->y) < 0.0001f &&
	       fabsf(q->z) < 0.0001f && fabsf(q->w - 1.0f) < 0.0001f;
}

// Phase 2.K Commit 8.C: chrome fade durations.
#define WORKSPACE_CHROME_FADE_IN_NS  (150ULL * 1000000ULL)
#define WORKSPACE_CHROME_FADE_OUT_NS (300ULL * 1000000ULL)

// Seed a fade toward `target` over `duration_ns`. Reuses the current alpha as
// the start so consecutive hover toggles pick up where they were rather than
// snapping back to 0/1.
static inline void
slot_chrome_fade_to(struct d3d11_multi_client_slot *slot, float target, uint64_t duration_ns, uint64_t now_ns)
{
	if (slot == nullptr) return;
	if (slot->chrome_fade.target == target && slot->chrome_fade.duration_ns != 0) {
		return; // already heading there
	}
	slot->chrome_fade.start_alpha = slot->chrome_fade.alpha;
	slot->chrome_fade.target = target;
	slot->chrome_fade.start_ns = now_ns;
	slot->chrome_fade.duration_ns = duration_ns;
}

// Tick the fade: ease-out cubic from start_alpha to target. Returns the
// resolved current alpha (also written into slot->chrome_fade.alpha).
static inline float
slot_chrome_fade_tick(struct d3d11_multi_client_slot *slot, uint64_t now_ns)
{
	if (slot->chrome_fade.duration_ns == 0) {
		return slot->chrome_fade.alpha;
	}
	uint64_t elapsed = (now_ns >= slot->chrome_fade.start_ns) ? now_ns - slot->chrome_fade.start_ns : 0;
	float t = (float)((double)elapsed / (double)slot->chrome_fade.duration_ns);
	if (t >= 1.0f) {
		slot->chrome_fade.alpha = slot->chrome_fade.target;
		slot->chrome_fade.duration_ns = 0;
		return slot->chrome_fade.alpha;
	}
	// ease-out cubic: 1 - (1-t)^3
	float f = 1.0f - t;
	float eased = 1.0f - f * f * f;
	slot->chrome_fade.alpha = slot->chrome_fade.start_alpha +
	                          (slot->chrome_fade.target - slot->chrome_fade.start_alpha) * eased;
	return slot->chrome_fade.alpha;
}

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ANIM_DURATION_NS defined earlier (forward declaration section).

//! Animation duration for initial window entry (400ms).
#define ANIM_ENTRY_DURATION_NS (400ULL * 1000000ULL)

/*!
 * Ease-out cubic: fast start, smooth deceleration.
 * t in [0,1] → result in [0,1].
 */
static inline float
ease_out_cubic(float t)
{
	float f = 1.0f - t;
	return 1.0f - f * f * f;
}

/*!
 * Start an animation on a slot toward a target pose + size.
 */
static inline void
slot_animate_to(struct d3d11_multi_client_slot *slot,
                const struct xrt_pose *target_pose,
                float target_w, float target_h,
                uint64_t now_ns, uint64_t duration_ns)
{
	slot->anim.active = true;
	slot->anim.start_pose = slot->window_pose;
	slot->anim.target_pose = *target_pose;
	slot->anim.start_width_m = slot->window_width_m;
	slot->anim.start_height_m = slot->window_height_m;
	slot->anim.target_width_m = target_w;
	slot->anim.target_height_m = target_h;
	slot->anim.start_ns = now_ns;
	slot->anim.duration_ns = duration_ns;
}

/*!
 * Tick animation for a slot. Returns true if animation is still running.
 * Updates window_pose and window_width/height_m with interpolated values.
 */
static inline bool
slot_animate_tick(struct d3d11_multi_client_slot *slot, uint64_t now_ns)
{
	if (!slot->anim.active) return false;

	uint64_t elapsed = now_ns - slot->anim.start_ns;
	float t = (float)elapsed / (float)slot->anim.duration_ns;
	if (t >= 1.0f) {
		t = 1.0f;
		slot->anim.active = false;
	}
	float eased = ease_out_cubic(t);

	// Interpolate pose (lerp position, slerp orientation)
	math_pose_interpolate(&slot->anim.start_pose, &slot->anim.target_pose,
	                      eased, &slot->window_pose);

	// Interpolate dimensions
	slot->window_width_m = slot->anim.start_width_m +
	    eased * (slot->anim.target_width_m - slot->anim.start_width_m);
	slot->window_height_m = slot->anim.start_height_m +
	    eased * (slot->anim.target_height_m - slot->anim.start_height_m);

	return slot->anim.active; // true = still running
}

/*!
 * Toggle fullscreen for a given slot. On fullscreen: animate to fill entire
 * display (no title bar), hide all other windows. On restore: animate back,
 * unhide others.
 */
static void
toggle_fullscreen(struct d3d11_service_system *sys,
                  struct d3d11_multi_compositor *mc,
                  int slot)
{
	if (mc == nullptr || slot < 0 || slot >= D3D11_MULTI_MAX_CLIENTS ||
	    !mc->clients[slot].active) {
		return;
	}

	float disp_w_m = sys->base.info.display_width_m;
	float disp_h_m = sys->base.info.display_height_m;
	if (disp_w_m <= 0.0f) disp_w_m = 0.700f;
	if (disp_h_m <= 0.0f) disp_h_m = 0.394f;
	uint64_t now_ns = os_monotonic_get_ns();

	if (mc->clients[slot].maximized) {
		// Restore from fullscreen (animated)
		slot_animate_to(&mc->clients[slot],
		                &mc->clients[slot].pre_max_pose,
		                mc->clients[slot].pre_max_width_m,
		                mc->clients[slot].pre_max_height_m,
		                now_ns, ANIM_DURATION_NS);
		mc->clients[slot].maximized = false;
		mc->toast_until_ns = 0; // dismiss restore hint
		// Un-hide windows that were hidden by fullscreen
		for (int j = 0; j < D3D11_MULTI_MAX_CLIENTS; j++) {
			if (mc->clients[j].fullscreen_minimized) {
				mc->clients[j].minimized = false;
				mc->clients[j].fullscreen_minimized = false;
			}
		}
		U_LOG_W("Multi-comp: restore slot %d from fullscreen", slot);
	} else {
		// Save current state
		mc->clients[slot].pre_max_pose = mc->clients[slot].window_pose;
		mc->clients[slot].pre_max_width_m = mc->clients[slot].window_width_m;
		mc->clients[slot].pre_max_height_m = mc->clients[slot].window_height_m;
		// Animate to fill entire display (no margins, no title bar)
		struct xrt_pose max_pose = {};
		max_pose.orientation.w = 1.0f;
		max_pose.position.x = 0;
		max_pose.position.y = 0;
		max_pose.position.z = 0;
		slot_animate_to(&mc->clients[slot],
		                &max_pose,
		                disp_w_m,
		                disp_h_m,
		                now_ns, ANIM_DURATION_NS);
		mc->clients[slot].maximized = true;
		// Show restore hint toast for 3 seconds
		snprintf(mc->toast_text, sizeof(mc->toast_text),
		         "Press F11 or Esc to restore");
		mc->toast_until_ns = os_monotonic_get_ns() + 3000000000ULL;
		// Hide all other windows
		for (int j = 0; j < D3D11_MULTI_MAX_CLIENTS; j++) {
			if (j != slot && mc->clients[j].active && !mc->clients[j].minimized) {
				mc->clients[j].minimized = true;
				mc->clients[j].fullscreen_minimized = true;
			}
		}
		U_LOG_W("Multi-comp: fullscreen slot %d", slot);
	}

	mc->focused_slot = slot;
	multi_compositor_update_input_forward(mc);

	// Update window layer's ESC-suppression flag
	bool any_max = false;
	for (int j = 0; j < D3D11_MULTI_MAX_CLIENTS; j++) {
		if (mc->clients[j].active && mc->clients[j].maximized) { any_max = true; break; }
	}
	comp_d3d11_window_set_any_maximized(mc->window, any_max);
}

/*!
 * Compute adaptive grid position for window `idx` out of `n` active windows.
 * Grid uses ceil(sqrt(N)) columns, fills row-first with 10% padding per cell.
 * All windows at Z=0.
 */
static void
compute_grid_layout(const struct d3d11_service_system *sys,
                    int n, int idx,
                    float *out_x, float *out_y, float *out_z,
                    float *out_w, float *out_h)
{
	float disp_w = sys->base.info.display_width_m;
	float disp_h = sys->base.info.display_height_m;
	if (disp_w <= 0.0f) disp_w = 0.700f;
	if (disp_h <= 0.0f) disp_h = 0.394f;

	if (n <= 0) n = 1;
	int cols = (int)ceilf(sqrtf((float)n));
	int rows = (int)ceilf((float)n / (float)cols);

	int col = idx % cols;
	int row = idx / cols;

	// 90% of display used, 10% is padding (5% each side per cell)
	float cell_w = disp_w * 0.90f / (float)cols;
	float cell_h = disp_h * 0.90f / (float)rows;

	*out_w = cell_w * 0.90f;  // 90% of cell = 10% padding between windows
	*out_h = cell_h * 0.90f;
	*out_x = (col - (cols - 1) / 2.0f) * cell_w;
	*out_y = ((rows - 1) / 2.0f - row) * cell_h;
	*out_z = 0.0f;
}

/*!
 * Update the SRV for a capture client slot.
 *
 * Gets the latest captured texture and (re)creates the SRV if the
 * texture pointer or dimensions changed. Also updates content_view_w/h
 * and window aspect ratio on size change.
 */
static void
capture_slot_update_srv(struct d3d11_service_system *sys,
                        struct d3d11_multi_client_slot *slot)
{
	if (slot->capture_ctx == nullptr) return;

	uint32_t w = 0, h = 0;
	ID3D11Texture2D *tex = d3d11_capture_get_texture(slot->capture_ctx, &w, &h);
	if (tex == nullptr || w == 0 || h == 0) return;

	// Recreate SRV if texture pointer or dimensions changed
	if (tex != slot->capture_texture_last || w != slot->capture_width || h != slot->capture_height) {
		slot->capture_srv = nullptr; // release old SRV

		D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
		srv_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv_desc.Texture2D.MipLevels = 1;

		HRESULT hr = sys->device->CreateShaderResourceView(tex, &srv_desc, slot->capture_srv.put());
		if (FAILED(hr)) {
			U_LOG_E("Capture: CreateShaderResourceView failed (hr=0x%08lx)", hr);
			return;
		}

		// Update dimensions
		bool size_changed = (w != slot->capture_width || h != slot->capture_height);
		slot->capture_texture_last = tex;
		slot->capture_width = w;
		slot->capture_height = h;
		slot->content_view_w = w;
		slot->content_view_h = h;

		// Update window aspect ratio if capture size changed
		if (size_changed && slot->capture_width > 0 && slot->capture_height > 0) {
			float aspect = (float)w / (float)h;
			slot->window_height_m = slot->window_width_m / aspect;
			slot_pose_to_pixel_rect(sys, slot,
			    &slot->window_rect_x, &slot->window_rect_y,
			    &slot->window_rect_w, &slot->window_rect_h);
		}
	}
}

/*!
 * Render all client atlases into the combined atlas using Level 2 Kooima,
 * then run DP process_atlas and present.
 *
 * Called from compositor_layer_commit in workspace mode.
 */
static void
multi_compositor_render(struct d3d11_service_system *sys)
{
	struct d3d11_multi_compositor *mc = sys->multi_comp;

	// Lazy init
	if (mc == nullptr || mc->hwnd == nullptr) {
		xrt_result_t ret = multi_compositor_ensure_output(sys);
		if (ret != XRT_SUCCESS) {
			return;
		}
		mc = sys->multi_comp;
	}

	if (mc->suspended) {
		// Workspace deactivated — don't render, wait for re-activation.
		return;
	}

	if (mc->window_dismissed) {
		// Workspace window closed (ESC / close button). Behaves like deactivate:
		// restore 2D windows, send LOSS_PENDING (not EXIT_REQUEST) to IPC
		// clients. The workspace can re-activate via Ctrl+Space.
		if (!mc->dismiss_cleanup_done) {
			mc->dismiss_cleanup_done = true;

			// Stop and restore capture clients (2D windows)
			for (int i = 0; i < D3D11_MULTI_MAX_CLIENTS; i++) {
				struct d3d11_multi_client_slot *slot = &mc->clients[i];
				if (!slot->active) continue;
				if (slot->client_type == CLIENT_TYPE_CAPTURE) {
					d3d11_capture_stop(slot->capture_ctx);
					slot->capture_ctx = nullptr;
					slot->capture_srv = nullptr;
					slot->capture_texture_last = nullptr;
					// Restore 2D window to desktop
					if (slot->app_hwnd != nullptr && IsWindow(slot->app_hwnd)) {
						SetWindowPlacement(slot->app_hwnd, &slot->saved_placement);
						SetWindowLongPtr(slot->app_hwnd, GWL_EXSTYLE, slot->saved_exstyle);
						SetWindowPos(slot->app_hwnd, HWND_TOP, 0, 0, 0, 0,
						             SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
					}
					slot->active = false;
					slot->compositor = nullptr;
					slot->client_type = CLIENT_TYPE_IPC;
					mc->client_count--;
					mc->capture_client_count--;
				} else if (slot->compositor != nullptr) {
					// Hot-switch IPC client to standalone mode
					struct d3d11_client_render_resources *res = &slot->compositor->render;
					HWND app_hwnd = slot->app_hwnd;
					if (app_hwnd != nullptr && IsWindow(app_hwnd)) {
						ShowWindow(app_hwnd, SW_SHOW);
						res->hwnd = app_hwnd;
						res->owns_window = false;

						RECT cr;
						uint32_t sc_w = sys->output_width, sc_h = sys->output_height;
						if (GetClientRect(app_hwnd, &cr)) {
							uint32_t cw = (uint32_t)(cr.right - cr.left);
							uint32_t ch = (uint32_t)(cr.bottom - cr.top);
							if (cw > 0 && ch > 0) { sc_w = cw; sc_h = ch; }
						}

						DXGI_SWAP_CHAIN_DESC1 sc_desc = {};
						sc_desc.Width = sc_w;
						sc_desc.Height = sc_h;
						sc_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
						sc_desc.SampleDesc.Count = 1;
						sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
						sc_desc.BufferCount = 2;
						sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

						HRESULT hr = sys->dxgi_factory->CreateSwapChainForHwnd(
						    sys->device.get(), app_hwnd, &sc_desc,
						    nullptr, nullptr, res->swap_chain.put());
						if (SUCCEEDED(hr)) {
							wil::com_ptr<ID3D11Texture2D> bb;
							res->swap_chain->GetBuffer(0, IID_PPV_ARGS(bb.put()));
							sys->device->CreateRenderTargetView(
							    bb.get(), nullptr, res->back_buffer_rtv.put());
						}

						if (sys->base.info.dp_factory_d3d11 != NULL) {
							auto factory = (xrt_dp_factory_d3d11_fn_t)sys->base.info.dp_factory_d3d11;
							factory(sys->device.get(), sys->context.get(),
							        app_hwnd, &res->display_processor);
							// Phase 6.1 (#140): don't call request_display_mode
							// here — same SR SDK recalibration issue as the
							// workspace activation path. Let the DP come up in the
							// current mode; the V key toggle still works.
						}
						U_LOG_W("Dismiss: hot-switched slot %d to standalone", i);
					}
				}
			}

			// Stop render thread
			capture_render_thread_stop(sys);

			// Release shared DP (per-client DPs now handle display)
			if (mc->display_processor != nullptr) {
				xrt_display_processor_d3d11_destroy(&mc->display_processor);
			}

			U_LOG_W("Multi-comp: workspace dismissed — captures restored, IPC clients hot-switched");
		}
		return;
	}

	// Check window validity — ESC or close button triggers deactivate (suspend),
	// not the old permanent dismiss. The workspace can re-activate via Ctrl+Space.
	if (mc->window != nullptr && !comp_d3d11_window_is_valid(mc->window)) {
		U_LOG_W("Multi-comp: window closed (ESC) — deactivating workspace");
		// Set workspace_mode flags to false so the workspace process detects the change
		service_set_workspace_mode(sys, false);
		sys->base.info.workspace_mode = false;
		// Run the full deactivate path (capture teardown, DP release, etc.)
		// We need to recreate the window on resume since it was destroyed by ESC,
		// so use the dismissed path which ensure_workspace_window handles.
		mc->window_dismissed = true;
		// Switch display back to 2D (lens off)
		if (mc->display_processor != nullptr) {
			xrt_display_processor_d3d11_request_display_mode(mc->display_processor, false);
		}
		return;
	}

	// Phase 5.12: launcher keyboard navigation.
	// When the launcher is visible the arrow keys move selection, Enter
	// fires the selected tile (or the Browse tile), and Esc dismisses the
	// launcher. The normal TAB/DELETE/F11/Ctrl+1-3 shortcuts are gated
	// out so e.g. Enter while launcher is open doesn't also toggle layout.
	if (mc->launcher_visible) {
		const int LAUNCHER_GRID_COLS = 4;
		int visible_to_full[IPC_LAUNCHER_MAX_APPS];
		uint32_t n_visible = launcher_build_visible_list(sys, visible_to_full);
		// Addressable range includes the Browse tile at index n_visible.
		int n_selectable = (int)n_visible + 1;

		if (sys->launcher_selected_index < 0) sys->launcher_selected_index = 0;
		if (sys->launcher_selected_index >= n_selectable) {
			sys->launcher_selected_index = n_selectable - 1;
		}

		if (GetAsyncKeyState(VK_RIGHT) & 1) {
			sys->launcher_selected_index =
			    (sys->launcher_selected_index + 1) % n_selectable;
		}
		if (GetAsyncKeyState(VK_LEFT) & 1) {
			sys->launcher_selected_index =
			    (sys->launcher_selected_index - 1 + n_selectable) % n_selectable;
		}
		if (GetAsyncKeyState(VK_DOWN) & 1) {
			int next = sys->launcher_selected_index + LAUNCHER_GRID_COLS;
			if (next < n_selectable) {
				sys->launcher_selected_index = next;
			}
		}
		if (GetAsyncKeyState(VK_UP) & 1) {
			int prev = sys->launcher_selected_index - LAUNCHER_GRID_COLS;
			if (prev >= 0) {
				sys->launcher_selected_index = prev;
			}
		}
		if (GetAsyncKeyState(VK_RETURN) & 1) {
			int sel = sys->launcher_selected_index;
			if (sel == (int)n_visible) {
				sys->pending_launcher_click_index = IPC_LAUNCHER_ACTION_BROWSE;
				launcher_set_visible(sys, mc, false);
				// Phase 5.14: see corresponding AllowSetForegroundWindow
				// call in the LMB click path — grants the workspace's file
				// dialog permission to pop to the front.
				AllowSetForegroundWindow(ASFW_ANY);
				U_LOG_W("Launcher: Enter on Browse tile");
			} else if (sel >= 0 && sel < (int)n_visible) {
				sys->pending_launcher_click_index = visible_to_full[sel];
				launcher_set_visible(sys, mc, false);
				U_LOG_W("Launcher: Enter on tile vis=%d full=%d",
				        sel, visible_to_full[sel]);
			}
		}
		if (GetAsyncKeyState(VK_ESCAPE) & 1) {
			launcher_set_visible(sys, mc, false);
			U_LOG_W("Launcher: Esc dismissed");
		}
		// Phase 6.6: Ctrl+R = refresh app list (re-scan sidecars).
		if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState('R') & 1)) {
			sys->pending_launcher_click_index = IPC_LAUNCHER_ACTION_REFRESH;
			launcher_set_visible(sys, mc, false);
			U_LOG_W("Launcher: Ctrl+R refresh");
		}

		// Phase 6.5: mouse wheel scrolls the grid. Consume scroll before
		// the normal window-resize scroll handler below.
		if (mc->window != nullptr) {
			int32_t scroll = comp_d3d11_window_consume_scroll(mc->window);
			if (scroll != 0) {
				int total_rows = (n_selectable + LAUNCHER_GRID_COLS - 1) / LAUNCHER_GRID_COLS;
				if (scroll < 0) {
					sys->launcher_scroll_row++;
				} else if (scroll > 0 && sys->launcher_scroll_row > 0) {
					sys->launcher_scroll_row--;
				}
				if (sys->launcher_scroll_row > total_rows - 1) {
					sys->launcher_scroll_row = total_rows - 1;
				}
				if (sys->launcher_scroll_row < 0) {
					sys->launcher_scroll_row = 0;
				}
			}
		}

		// Auto-scroll to keep selected tile in view.
		{
			int sel_row = sys->launcher_selected_index / LAUNCHER_GRID_COLS;
			if (sel_row < sys->launcher_scroll_row) {
				sys->launcher_scroll_row = sel_row;
			}
			// Compute how many rows fit in the visible grid area (estimated).
			// This uses pixel math matching the render block.
			float est_panel_h = ui_m_to_tile_px_y(
			    sys->base.info.display_height_m > 0 ? sys->base.info.display_height_m * 0.55f : 0.217f, sys);
			float est_margin = ui_m_to_tile_px_x(
			    (sys->base.info.display_width_m > 0 ? sys->base.info.display_width_m * 0.60f : 0.42f) * 0.04f, sys);
			float est_title = est_panel_h * 0.13f;
			float est_section = est_panel_h * 0.07f;
			float est_tile_w = (ui_m_to_tile_px_x(
			    sys->base.info.display_width_m > 0 ? sys->base.info.display_width_m * 0.60f : 0.42f, sys)
			    - 5.0f * est_margin) / 4.0f;
			uint32_t etpw = sys->base.info.display_pixel_width / sys->tile_columns;
			uint32_t etph = sys->base.info.display_pixel_height / sys->tile_rows;
			float edwm = sys->base.info.display_width_m > 0 ? sys->base.info.display_width_m : 0.700f;
			float edhm = sys->base.info.display_height_m > 0 ? sys->base.info.display_height_m : 0.394f;
			float epr = (etpw > 0 && etph > 0) ? ((edwm/(float)etpw) / (edhm/(float)etph)) : 1.0f;
			float est_tile_h = est_tile_w * epr;
			float est_section_scale = (est_section * 0.65f) /
			    (mc->font_glyph_h > 0 ? (float)mc->font_glyph_h : 16.0f);
			float est_label_h = (float)(mc->font_glyph_h > 0 ? mc->font_glyph_h : 16) *
			    est_section_scale * 0.85f;
			float est_grid_top = est_title + est_margin +
			    (float)(mc->font_glyph_h > 0 ? mc->font_glyph_h : 16) * est_section_scale +
			    est_margin * 0.5f;
			float est_row_h = est_tile_h + est_label_h + est_margin;
			float est_avail = est_panel_h - est_grid_top - est_margin;
			int max_vis_rows = (est_avail > 0 && est_row_h > 0) ? (int)(est_avail / est_row_h) : 3;
			if (max_vis_rows < 1) max_vis_rows = 1;

			if (sel_row >= sys->launcher_scroll_row + max_vis_rows) {
				sys->launcher_scroll_row = sel_row - max_vis_rows + 1;
			}
		}

		goto after_key_shortcuts;
	}

	// TAB / Shift+TAB: cycle focus forward/backward. Never unfocuses when windows exist.
	// Ignore ALT+TAB (system task switcher) — only process bare TAB.
	if ((GetAsyncKeyState(VK_TAB) & 1) && !(GetAsyncKeyState(VK_MENU) & 0x8000)) {
		if (mc->client_count > 0) {
			bool reverse = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
			int n = D3D11_MULTI_MAX_CLIENTS;
			// base: for -1, forward searches from slot 0, reverse from last slot
			int base = (mc->focused_slot >= 0) ? mc->focused_slot : (reverse ? n : -1);
			for (int i = 1; i <= n; i++) {
				int idx = reverse
				        ? ((base - i) % n + n) % n
				        : (base + i) % n;
				if (mc->clients[idx].active && !mc->clients[idx].minimized) {
					mc->focused_slot = idx;
					break;
				}
			}
			U_LOG_W("Multi-comp: %sTAB → focused slot %d", reverse ? "Shift+" : "", mc->focused_slot);
			multi_compositor_update_input_forward(mc);
		}
	}

	// DELETE: close focused client
	if (GetAsyncKeyState(VK_DELETE) & 1) {
		// Phase 2.K: keyboard shortcut now routes through the same helper
		// the public API uses. Behaviour is unchanged for users.
		if (mc->focused_slot >= 0) {
			comp_d3d11_service_workspace_request_exit_by_slot(
			    (struct xrt_system_compositor *)sys, mc->focused_slot);
		}
	}

	// F11: toggle fullscreen for the focused window
	if (GetAsyncKeyState(VK_F11) & 1) {
		if (mc->focused_slot >= 0) {
			// Phase 2.K: keyboard shortcut routes through the helper too.
			// Pass !maximized to flip the current state (the helper
			// no-ops if the requested state already matches).
			bool current_max = mc->clients[mc->focused_slot].maximized;
			comp_d3d11_service_workspace_request_fullscreen_by_slot(
			    (struct xrt_system_compositor *)sys, mc->focused_slot, !current_max);
		}
	}

	// ESC: restore maximized window (if any) before doing anything else
	if (GetAsyncKeyState(VK_ESCAPE) & 1) {
		for (int i = 0; i < D3D11_MULTI_MAX_CLIENTS; i++) {
			if (mc->clients[i].active && mc->clients[i].maximized) {
				toggle_fullscreen(sys, mc, i);
				break;
			}
		}
	}

	// Screenshot: triggered by F12 key (kept for interactive use).

	// Ctrl+O: open file dialog to launch a new app
	if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState('O') & 1)) {
		comp_d3d11_window_request_app_launch(mc->window);
	}

	// Phase 2.G: Ctrl+1..3 layout presets are owned by the workspace
	// controller now; the runtime no longer intercepts them. Keys flow
	// through xrEnumerateWorkspaceInputEventsEXT and the controller
	// pushes per-client poses via xrSetWorkspaceClientWindowPoseEXT.

after_key_shortcuts:
	(void)0; // label target for the launcher-visible fast-path above.

	// Update cursor via window thread (compositor thread can't call SetCursor directly).
	// Cursor IDs: 0=arrow, 1=sizewe, 2=sizens, 3=sizenwse, 4=sizenesw, 5=sizeall
	if (mc->window != nullptr) {
		int cursor_id = 0; // arrow
		if (mc->resize.active) {
			int e = mc->resize.edges;
			if (e == (RESIZE_LEFT | RESIZE_TOP) || e == (RESIZE_RIGHT | RESIZE_BOTTOM))
				cursor_id = 3; // sizenwse
			else if (e == (RESIZE_RIGHT | RESIZE_TOP) || e == (RESIZE_LEFT | RESIZE_BOTTOM))
				cursor_id = 4; // sizenesw
			else if (e & (RESIZE_LEFT | RESIZE_RIGHT))
				cursor_id = 1; // sizewe
			else if (e & (RESIZE_TOP | RESIZE_BOTTOM))
				cursor_id = 2; // sizens
		} else if (mc->title_drag.active) {
			cursor_id = 5; // sizeall (move during title drag)
		} else {
			POINT cpt;
			GetCursorPos(&cpt);
			ScreenToClient(mc->hwnd, &cpt);
			struct workspace_hit_result hover = workspace_raycast_hit_test(sys, mc, cpt);

			// Track button hover for highlight rendering
			mc->hover_btn = 0;
			mc->hover_btn_slot = -1;
			if (hover.in_close_btn) { mc->hover_btn = 1; mc->hover_btn_slot = hover.slot; }
			else if (hover.in_minimize_btn) { mc->hover_btn = 2; mc->hover_btn_slot = hover.slot; }
			else if (hover.in_maximize_btn) { mc->hover_btn = 3; mc->hover_btn_slot = hover.slot; }

			// Phase 2.K Commit 8.C: per-slot chrome hover-fade. Hovered
			// slot fades chrome in over 150 ms; everyone else fades out
			// over 300 ms. Idle (cursor off any slot) → all chrome
			// alphas head toward 0.
			{
				int hovered_slot = hover.slot; // -1 if no slot under cursor
				uint64_t now_ns_chrome = os_monotonic_get_ns();
				for (int sf = 0; sf < D3D11_MULTI_MAX_CLIENTS; sf++) {
					if (!mc->clients[sf].active ||
					    mc->clients[sf].client_type == CLIENT_TYPE_CAPTURE) {
						continue;
					}
					if (sf == hovered_slot) {
						slot_chrome_fade_to(&mc->clients[sf], 1.0f,
						                    WORKSPACE_CHROME_FADE_IN_NS, now_ns_chrome);
					} else {
						slot_chrome_fade_to(&mc->clients[sf], 0.0f,
						                    WORKSPACE_CHROME_FADE_OUT_NS, now_ns_chrome);
					}
				}
			}

			// Buttons get arrow cursor (no resize/drag cursor on buttons)
			if (hover.in_close_btn || hover.in_minimize_btn || hover.in_maximize_btn) {
				cursor_id = 0; // arrow
			} else {
				int ef = hover.edge_flags;
				if (ef == (RESIZE_LEFT | RESIZE_TOP) || ef == (RESIZE_RIGHT | RESIZE_BOTTOM))
					cursor_id = 3;
				else if (ef == (RESIZE_RIGHT | RESIZE_TOP) || ef == (RESIZE_LEFT | RESIZE_BOTTOM))
					cursor_id = 4;
				else if (ef & (RESIZE_LEFT | RESIZE_RIGHT))
					cursor_id = 1;
				else if (ef & (RESIZE_TOP | RESIZE_BOTTOM))
					cursor_id = 2;
				else if (hover.in_grip_handle)
					cursor_id = 5; // move cursor only on the grip dots
			}
		}
		comp_d3d11_window_set_cursor(mc->window, cursor_id);
	}

	// Left-click: focus window, close button, title bar drag, or content click.
	// Title bar extends TITLE_BAR_HEIGHT_PX above the content rect.
	{
		bool lmb_held = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
		bool lmb_just_pressed = lmb_held && !mc->prev_lmb_held;
		mc->prev_lmb_held = lmb_held;

		if (lmb_just_pressed && !mc->title_drag.active && !mc->resize.active) {
			POINT pt;
			GetCursorPos(&pt);
			ScreenToClient(mc->hwnd, &pt);

			// Phase 5.9/5.10/5.14: launcher takes click priority when visible.
			// - vis_tile in [0, n_visible-1]  → real tile, store full index
			// - vis_tile == n_visible         → Browse-for-app virtual tile
			// - vis_tile == -1                → gap/title/header, swallow
			// - vis_tile == -2                → outside panel, dismiss
			if (mc->launcher_visible) {
				int visible_to_full[IPC_LAUNCHER_MAX_APPS];
				uint32_t n_visible = launcher_build_visible_list(sys, visible_to_full);
				int vis_tile = launcher_hit_test(sys, pt, n_visible);
				if (vis_tile >= 0 && vis_tile < (int)n_visible) {
					sys->pending_launcher_click_index = visible_to_full[vis_tile];
					launcher_set_visible(sys, mc, false);
					U_LOG_W("Launcher: tile vis=%d full=%d clicked",
					        vis_tile, visible_to_full[vis_tile]);
				} else if (vis_tile == (int)n_visible) {
					sys->pending_launcher_click_index = IPC_LAUNCHER_ACTION_BROWSE;
					launcher_set_visible(sys, mc, false);
					// Phase 5.14: grant any process foreground-activation
					// permission so the workspace's GetOpenFileNameA dialog can
					// pop to the front. The service currently has foreground
					// from the click, so it has the right to grant this.
					AllowSetForegroundWindow(ASFW_ANY);
					U_LOG_W("Launcher: Browse tile clicked");
				} else if (vis_tile == -2) {
					launcher_set_visible(sys, mc, false);
					U_LOG_W("Launcher: dismissed by click outside panel");
				}
				// Either way, do not propagate this click to window logic.
				goto after_lmb_handling;
			}

			// Spatial raycast: cast ray from eye through cursor on display surface
			struct workspace_hit_result hit = workspace_raycast_hit_test(sys, mc, pt);

			// Phase 2.K: when controller has pointer capture, runtime cedes
			// edge-resize too (the controller may want to interpret edges as
			// something other than resize, e.g. carousel reflow).
			bool ctrl_owns_drag = (mc->window != nullptr) &&
			                      comp_d3d11_window_is_workspace_pointer_capture_enabled(mc->window);
			// Edge/corner resize takes priority (unless clicking a title bar button)
			if (hit.slot >= 0 && hit.edge_flags != RESIZE_NONE &&
			    !hit.in_close_btn && !hit.in_minimize_btn && !hit.in_maximize_btn &&
			    !ctrl_owns_drag) {
				mc->resize.active = true;
				mc->resize.slot = hit.slot;
				mc->resize.edges = hit.edge_flags;
				mc->resize.start_cursor = pt;
				mc->resize.start_pos_x = mc->clients[hit.slot].window_pose.position.x;
				mc->resize.start_pos_y = mc->clients[hit.slot].window_pose.position.y;
				mc->resize.start_width_m = mc->clients[hit.slot].window_width_m;
				mc->resize.start_height_m = mc->clients[hit.slot].window_height_m;
				// Suppress input forwarding during resize
				if (mc->window != nullptr)
					comp_d3d11_window_set_input_suppress(mc->window, true);
				if (hit.slot != mc->focused_slot) {
					mc->focused_slot = hit.slot;
				}
				if (mc->window != nullptr) {
					comp_d3d11_window_set_input_forward(mc->window, NULL, 0, 0, 0, 0, false);
				}
			} else {

			int hit_slot = hit.slot;
			bool in_title_bar = hit.in_title_bar;
			bool in_close_btn = hit.in_close_btn;
			bool in_minimize_btn = hit.in_minimize_btn;
			bool in_maximize_btn = hit.in_maximize_btn;

			// Debug: log click coordinates and hit results
			if (hit_slot >= 0 && in_title_bar) {
				int32_t rx = (int32_t)mc->clients[hit_slot].window_rect_x;
				int32_t rw = (int32_t)mc->clients[hit_slot].window_rect_w;
				U_LOG_W("Title click: pt=(%ld,%ld) slot=%d close_at=%d min_at=%d close=%d min=%d",
				        pt.x, pt.y, hit_slot,
				        rx + rw - CLOSE_BTN_WIDTH_PX,
				        rx + rw - 2 * CLOSE_BTN_WIDTH_PX,
				        in_close_btn, in_minimize_btn);
			}

			if (in_close_btn && hit_slot >= 0) {
				if (mc->clients[hit_slot].client_type == CLIENT_TYPE_CAPTURE) {
					// Capture client: remove directly
					multi_compositor_remove_capture_client(sys, hit_slot);
					U_LOG_W("Multi-comp: close button → removed capture slot %d", hit_slot);
				} else {
					// IPC client: send EXIT_REQUEST
					struct d3d11_service_compositor *fc = mc->clients[hit_slot].compositor;
					if (fc != nullptr && fc->xses != nullptr) {
						union xrt_session_event xse = XRT_STRUCT_INIT;
						xse.type = XRT_SESSION_EVENT_EXIT_REQUEST;
						xrt_session_event_sink_push(fc->xses, &xse);
						U_LOG_W("Multi-comp: close button → exit request for slot %d", hit_slot);
					}
				}
			} else if (in_maximize_btn && hit_slot >= 0) {
				toggle_fullscreen(sys, mc, hit_slot);
			} else if (in_minimize_btn && hit_slot >= 0) {
				// Minimize button: hide window
				mc->clients[hit_slot].minimized = true;
				U_LOG_W("Multi-comp: minimize slot %d", hit_slot);
				if (hit_slot == mc->focused_slot) {
					// Advance focus to next visible window
					mc->focused_slot = -1;
					for (int i = 0; i < D3D11_MULTI_MAX_CLIENTS; i++) {
						if (mc->clients[i].active && !mc->clients[i].minimized) {
							mc->focused_slot = i;
							break;
						}
					}
					multi_compositor_update_input_forward(mc);
				}
			} else if (in_title_bar && hit_slot >= 0) {
				// Double-click title bar → toggle maximize
				DWORD now = GetTickCount();
				bool is_double_click = (hit_slot == mc->last_title_click_slot &&
				                        (now - mc->last_title_click_time) < 400);
				mc->last_title_click_time = now;
				mc->last_title_click_slot = hit_slot;

				if (is_double_click) {
					toggle_fullscreen(sys, mc, hit_slot);
				} else {
					// Single click on title bar: focus + maybe start dragging.
					// Phase 2.K: when a workspace controller has taken pointer
					// capture, the runtime cedes drag policy to it (carousel
					// rotation, custom drag affordances, etc.). Without this
					// gate the runtime's title-bar drag fights the
					// controller's set_pose every frame.
					mc->focused_slot = hit_slot;
					multi_compositor_update_input_forward(mc);

					bool ctrl_owns_drag = (mc->window != nullptr) &&
					                      comp_d3d11_window_is_workspace_pointer_capture_enabled(mc->window);
					// Phase 2.K Commit 8 tweak: drag only starts on the
					// grip dots, not on the entire pill. Lets buttons stay
					// at the right edge while keeping the drag affordance
					// visually obvious (the dotted grip).
					if (!ctrl_owns_drag && hit.in_grip_handle) {
						mc->title_drag.active = true;
						mc->title_drag.slot = hit_slot;
						// Suppress input forwarding during drag
						if (mc->window != nullptr)
							comp_d3d11_window_set_input_suppress(mc->window, true);
						GetCursorPos(&mc->title_drag.start_cursor);
						ScreenToClient(mc->hwnd, &mc->title_drag.start_cursor);
						mc->title_drag.start_pos_x = mc->clients[hit_slot].window_pose.position.x;
						mc->title_drag.start_pos_y = mc->clients[hit_slot].window_pose.position.y;
					}
				}
			} else {
				// Content area click or taskbar click
				if (hit_slot < 0) {
					// No visible window hit — check taskbar using spatial coords.
					// Convert cursor to display-surface meters for comparison.
					float disp_w_tb = sys->base.info.display_width_m;
					float disp_h_tb = sys->base.info.display_height_m;
					if (disp_w_tb <= 0) disp_w_tb = 0.700f;
					if (disp_h_tb <= 0) disp_h_tb = 0.394f;
					float cursor_y_m = ((float)sys->base.info.display_pixel_height / 2.0f - (float)pt.y) *
					                   disp_h_tb / (float)sys->base.info.display_pixel_height;
					float cursor_x_m = ((float)pt.x - (float)sys->base.info.display_pixel_width / 2.0f) *
					                   disp_w_tb / (float)sys->base.info.display_pixel_width;
					// Taskbar at bottom: y from -disp_h/2 to -disp_h/2 + taskbar_h
					float tb_bottom_m = -disp_h_tb / 2.0f;
					float tb_top_m = tb_bottom_m + UI_TASKBAR_H_M;
					if (cursor_y_m >= tb_bottom_m && cursor_y_m < tb_top_m) {
						float pill_w_m = 6.0f * UI_GLYPH_W_M + 0.001f;
						float gap_m = 0.002f;
						int ind_idx = 0;
						for (int i = 0; i < D3D11_MULTI_MAX_CLIENTS; i++) {
							if (!mc->clients[i].active || !mc->clients[i].minimized) continue;
							float ind_left_m = -disp_w_tb / 2.0f + 0.002f + ind_idx * (pill_w_m + gap_m);
							float ind_right_m = ind_left_m + pill_w_m;
							if (cursor_x_m >= ind_left_m && cursor_x_m < ind_right_m) {
								mc->clients[i].minimized = false;
								mc->focused_slot = i;
								multi_compositor_update_input_forward(mc);
								U_LOG_W("Multi-comp: taskbar → un-minimize slot %d", i);
								break;
							}
							ind_idx++;
						}
					}
				} else {
					// Content area: focus + forward to app
					if (hit_slot != mc->focused_slot) {
						mc->focused_slot = hit_slot;
						U_LOG_W("Multi-comp: click → focused slot %d%s", mc->focused_slot,
						        mc->focused_slot < 0 ? " (unfocused)" : "");
						multi_compositor_update_input_forward(mc);
			
					}

					// For capture clients, synthesize a click to route to the correct
					// child control (sets internal focus for typing).
					// For IPC apps, the WndProc already forwards the real mouse events
					// — don't synthesize here or it cancels the drag (down+up).
					if (mc->clients[hit_slot].client_type == CLIENT_TYPE_CAPTURE &&
					    mc->clients[hit_slot].app_hwnd != nullptr) {
						float title_h_m = UI_TITLE_BAR_H_M;
						float content_local_x = hit.local_x_m;
						float content_local_y = hit.local_y_m - title_h_m;
						float win_w = mc->clients[hit_slot].window_width_m;
						float win_h = mc->clients[hit_slot].window_height_m;

						RECT target_cr;
						GetClientRect(mc->clients[hit_slot].app_hwnd, &target_cr);
						int target_w = target_cr.right - target_cr.left;
						int target_h = target_cr.bottom - target_cr.top;
						if (target_w <= 0) target_w = (int)mc->clients[hit_slot].window_rect_w;
						if (target_h <= 0) target_h = (int)mc->clients[hit_slot].window_rect_h;

						int app_x = (int)(content_local_x / win_w * (float)target_w);
						int app_y = (int)(content_local_y / win_h * (float)target_h);

						HWND click_target = mc->clients[hit_slot].app_hwnd;
						POINT child_pt = {app_x, app_y};
						HWND child = ChildWindowFromPointEx(
						    click_target, child_pt,
						    CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
						if (child != NULL && child != click_target) {
							MapWindowPoints(click_target, child, &child_pt, 1);
							click_target = child;
							app_x = child_pt.x;
							app_y = child_pt.y;
						}

						LPARAM lp = MAKELPARAM(app_x, app_y);
						PostMessage(click_target, WM_MOUSEMOVE, 0, lp);
						PostMessage(click_target, WM_LBUTTONDOWN, MK_LBUTTON, lp);
						PostMessage(click_target, WM_LBUTTONUP, 0, lp);
					}
				}
			}
			} // close: else from resize_slot check
		} else if (lmb_held && mc->resize.active) {
			// Edge/corner resize — update window dimensions
			POINT pt;
			GetCursorPos(&pt);
			ScreenToClient(mc->hwnd, &pt);
			int s = mc->resize.slot;
			if (s >= 0 && s < D3D11_MULTI_MAX_CLIENTS && mc->clients[s].active) {
				float disp_w_m = sys->base.info.display_width_m;
				float disp_h_m = sys->base.info.display_height_m;
				uint32_t disp_px_w = sys->base.info.display_pixel_width;
				uint32_t disp_px_h = sys->base.info.display_pixel_height;
				if (disp_px_w > 0 && disp_px_h > 0 && disp_w_m > 0.0f && disp_h_m > 0.0f) {
					float dx_px = (float)(pt.x - mc->resize.start_cursor.x);
					float dy_px = (float)(pt.y - mc->resize.start_cursor.y);
					float m_per_px_x = disp_w_m / (float)disp_px_w;
					float m_per_px_y = disp_h_m / (float)disp_px_h;
					float dx_m = dx_px * m_per_px_x;
					float dy_m = dy_px * m_per_px_y;

					float new_w = mc->resize.start_width_m;
					float new_h = mc->resize.start_height_m;

					if (mc->resize.edges & RESIZE_RIGHT)  new_w += dx_m;
					if (mc->resize.edges & RESIZE_LEFT)   new_w -= dx_m;
					if (mc->resize.edges & RESIZE_BOTTOM) new_h += dy_m;
					if (mc->resize.edges & RESIZE_TOP)    new_h -= dy_m;

					if (new_w < UI_MIN_WIN_W_M) new_w = UI_MIN_WIN_W_M;
					if (new_h < UI_MIN_WIN_H_M) new_h = UI_MIN_WIN_H_M;
					if (new_w > disp_w_m * 0.95f) new_w = disp_w_m * 0.95f;
					if (new_h > disp_h_m * 0.95f) new_h = disp_h_m * 0.95f;

					// Anchor the opposite edge: derive position from the *clamped*
					// width/height delta, not raw cursor delta. Otherwise once a
					// dimension hits its min, continued dragging slides the window.
					float new_x = mc->resize.start_pos_x;
					float new_y = mc->resize.start_pos_y;
					float dw = new_w - mc->resize.start_width_m;
					float dh = new_h - mc->resize.start_height_m;
					if (mc->resize.edges & RESIZE_RIGHT)  new_x += dw / 2.0f;
					if (mc->resize.edges & RESIZE_LEFT)   new_x -= dw / 2.0f;
					if (mc->resize.edges & RESIZE_BOTTOM) new_y -= dh / 2.0f;
					if (mc->resize.edges & RESIZE_TOP)    new_y += dh / 2.0f;

					mc->clients[s].window_width_m = new_w;
					mc->clients[s].window_height_m = new_h;
					mc->clients[s].window_pose.position.x = new_x;
					mc->clients[s].window_pose.position.y = new_y;

					slot_pose_to_pixel_rect(sys, &mc->clients[s],
					                        &mc->clients[s].window_rect_x,
					                        &mc->clients[s].window_rect_y,
					                        &mc->clients[s].window_rect_w,
					                        &mc->clients[s].window_rect_h);
					// Resize HWND every frame so app content adapts continuously
					mc->clients[s].hwnd_resize_pending = true;
				}
			}
		} else if (!lmb_held && mc->resize.active) {
			mc->clients[mc->resize.slot].hwnd_resize_pending = true;
			mc->resize.active = false;
			mc->resize.slot = -1;
			mc->resize.edges = RESIZE_NONE;
			// Resume input forwarding after resize
			if (mc->window != nullptr)
				comp_d3d11_window_set_input_suppress(mc->window, false);
			// Nudge cursor to force WM_SETCURSOR update
			{ POINT p; GetCursorPos(&p); SetCursorPos(p.x, p.y); }
			// Restore mouse forwarding after resize
			multi_compositor_update_input_forward(mc);
		} else if (lmb_held && mc->title_drag.active) {
			// Title bar dragging — update window position
			POINT pt;
			GetCursorPos(&pt);
			ScreenToClient(mc->hwnd, &pt);

			int s = mc->title_drag.slot;
			if (s >= 0 && s < D3D11_MULTI_MAX_CLIENTS && mc->clients[s].active) {
				float disp_w_m = sys->base.info.display_width_m;
				float disp_h_m = sys->base.info.display_height_m;
				uint32_t disp_px_w = sys->base.info.display_pixel_width;
				uint32_t disp_px_h = sys->base.info.display_pixel_height;
				if (disp_px_w > 0 && disp_px_h > 0 && disp_w_m > 0.0f && disp_h_m > 0.0f) {
					float dx_px = (float)(pt.x - mc->title_drag.start_cursor.x);
					float dy_px = (float)(pt.y - mc->title_drag.start_cursor.y);
					float m_per_px_x = disp_w_m / (float)disp_px_w;
					float m_per_px_y = disp_h_m / (float)disp_px_h;

					mc->clients[s].window_pose.position.x = mc->title_drag.start_pos_x + dx_px * m_per_px_x;
					mc->clients[s].window_pose.position.y = mc->title_drag.start_pos_y - dy_px * m_per_px_y;

					slot_pose_to_pixel_rect(sys, &mc->clients[s],
					                        &mc->clients[s].window_rect_x,
					                        &mc->clients[s].window_rect_y,
					                        &mc->clients[s].window_rect_w,
					                        &mc->clients[s].window_rect_h);

					if (s == mc->focused_slot) {
						multi_compositor_update_input_forward(mc);
					}
				}
			}
		} else if (!lmb_held && mc->title_drag.active) {
			// LMB released — end title drag.
			mc->title_drag.active = false;
			mc->title_drag.slot = -1;
			// Resume input forwarding after drag
			if (mc->window != nullptr)
				comp_d3d11_window_set_input_suppress(mc->window, false);
			{
				POINT p;
				GetCursorPos(&p);
				SetCursorPos(p.x, p.y);
			}
		}
	}
	after_lmb_handling:

	// Right-click: title bar RMB drag = rotation, content RMB = focus + forward to app.
	// Call GetAsyncKeyState ONCE to avoid consuming the & 1 press bit (Phase 2 lesson #4).
	{
		SHORT rmb_state = GetAsyncKeyState(VK_RBUTTON);
		bool rmb_held = (rmb_state & 0x8000) != 0;
		bool rmb_just_pressed = rmb_held && !mc->prev_rmb_held;
		mc->prev_rmb_held = rmb_held;

		// Phase 5.13: launcher RMB context menu. When the launcher is
		// visible, right-click on a tile pops a Win32 menu with Launch +
		// Remove (session-only) + Cancel. Branch before the existing
		// window-drag handling so it doesn't try to start a rotation drag
		// on a tile. Suppress Launch/Remove visual hit routing to the
		// window below.
		if (mc->launcher_visible && rmb_just_pressed) {
			POINT pt;
			GetCursorPos(&pt);
			ScreenToClient(mc->hwnd, &pt);
			int visible_to_full[IPC_LAUNCHER_MAX_APPS];
			uint32_t n_visible = launcher_build_visible_list(sys, visible_to_full);
			int vis_tile = launcher_hit_test(sys, pt, n_visible);
			if (vis_tile >= 0 && vis_tile < (int)n_visible) {
				int full_idx = visible_to_full[vis_tile];
				launcher_show_context_menu(sys, mc, pt, full_idx);
			}
			// Eat the RMB regardless of hit so it doesn't start a drag
			// on a window that might be peeking through the panel.
			goto after_rmb_handling;
		}

		if (rmb_held) {
			// Phase 2.K: when the controller has pointer capture, suppress
			// the runtime's RMB rotation drag — the controller owns interactive
			// motion policy.
			bool ctrl_owns_drag = (mc->window != nullptr) &&
			                      comp_d3d11_window_is_workspace_pointer_capture_enabled(mc->window);
			if (!mc->title_rmb_drag.active && rmb_just_pressed && !ctrl_owns_drag) {
				// RMB just pressed — check if on title bar to start rotation drag
				POINT pt;
				GetCursorPos(&pt);
				ScreenToClient(mc->hwnd, &pt);
				struct workspace_hit_result rmb_hit = workspace_raycast_hit_test(sys, mc, pt);
				if (rmb_hit.slot >= 0) {
					if (rmb_hit.slot != mc->focused_slot) {
						mc->focused_slot = rmb_hit.slot;
						multi_compositor_update_input_forward(mc);
					}
					// Phase 2.K Commit 8 tweak: only start rotation drag
					// when RMB lands on the grip dots (matches LMB drag).
					if (rmb_hit.in_grip_handle) {
						// Start rotation drag
						mc->title_rmb_drag.active = true;
						mc->title_rmb_drag.slot = rmb_hit.slot;
						mc->title_rmb_drag.start_cursor = pt;
						// Extract current yaw/pitch from quaternion
						struct xrt_vec3 euler;
						math_quat_to_euler_angles(&mc->clients[rmb_hit.slot].window_pose.orientation, &euler);
						mc->title_rmb_drag.start_yaw = euler.y;
						mc->title_rmb_drag.start_pitch = euler.x;
					}
				}
			} else if (mc->title_rmb_drag.active) {
				// RMB held — update rotation during drag
				POINT pt;
				GetCursorPos(&pt);
				ScreenToClient(mc->hwnd, &pt);
				int s = mc->title_rmb_drag.slot;
				if (s >= 0 && s < D3D11_MULTI_MAX_CLIENTS && mc->clients[s].active) {
					float dx = (float)(pt.x - mc->title_rmb_drag.start_cursor.x);
					float dy = (float)(pt.y - mc->title_rmb_drag.start_cursor.y);
					// ~1 degree per 10 pixels
					float deg_per_px = (float)(M_PI / 180.0) / 10.0f;
					float yaw = mc->title_rmb_drag.start_yaw + dx * deg_per_px;
					float pitch = mc->title_rmb_drag.start_pitch + dy * deg_per_px;
					// Clamp: yaw ±30°, pitch ±15°
					float max_yaw = (float)(30.0 * M_PI / 180.0);
					float max_pitch = (float)(15.0 * M_PI / 180.0);
					if (yaw < -max_yaw) yaw = -max_yaw;
					if (yaw > max_yaw) yaw = max_yaw;
					if (pitch < -max_pitch) pitch = -max_pitch;
					if (pitch > max_pitch) pitch = max_pitch;

					mc->clients[s].window_pose.orientation = quat_from_yaw_pitch(yaw, pitch);
				}
			}
		} else {
			if (mc->title_rmb_drag.active) {
				mc->title_rmb_drag.active = false;
				mc->title_rmb_drag.slot = -1;
				// Nudge cursor to force WM_SETCURSOR update
				POINT p; GetCursorPos(&p); SetCursorPos(p.x, p.y);
			}
		}

	after_rmb_handling:
		(void)0; // label target for the launcher-visible fast-path above.
	}

	// Scroll wheel (workspace consumes only when modifier held; plain scroll is
	// forwarded to the focused app by the WndProc):
	//   Shift+Scroll → Z-depth
	//   Ctrl+Scroll  → resize
	if (mc->window != nullptr && mc->focused_slot >= 0) {
		int32_t scroll = comp_d3d11_window_consume_scroll(mc->window);
		if (scroll != 0) {
			int s = mc->focused_slot;
			if (s >= 0 && s < D3D11_MULTI_MAX_CLIENTS && mc->clients[s].active) {
				bool shift_held = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
				if (shift_held) {
					// Shift+Scroll: move window in Z (~2mm per notch)
					float dz = (float)scroll / (120.0f * 500.0f); // ~0.002m per notch
					float new_z = mc->clients[s].window_pose.position.z + dz;
					if (new_z < -0.05f) new_z = -0.05f;
					if (new_z >  0.05f) new_z =  0.05f;
					mc->clients[s].window_pose.position.z = new_z;

					slot_pose_to_pixel_rect(sys, &mc->clients[s],
					                        &mc->clients[s].window_rect_x,
					                        &mc->clients[s].window_rect_y,
					                        &mc->clients[s].window_rect_w,
					                        &mc->clients[s].window_rect_h);
					U_LOG_I("Multi-comp: Shift+Scroll Z depth slot %d → Z=%.3fm", s, new_z);
				} else {
				// Plain scroll: resize (~5% per notch)
				float factor = 1.0f + (float)scroll / (120.0f * 20.0f); // 5% per notch
				if (factor < 0.5f) factor = 0.5f;
				if (factor > 2.0f) factor = 2.0f;

				float new_w = mc->clients[s].window_width_m * factor;
				float new_h = mc->clients[s].window_height_m * factor;

				// Clamp width to fit title bar buttons; height to 2cm; max 80% of display.
				float max_w = sys->base.info.display_width_m * 0.8f;
				float max_h = sys->base.info.display_height_m * 0.8f;
				if (max_w <= 0.0f) max_w = 0.560f;
				if (max_h <= 0.0f) max_h = 0.315f;

				if (new_w < UI_MIN_WIN_W_M) new_w = UI_MIN_WIN_W_M;
				if (new_h < UI_MIN_WIN_H_M) new_h = UI_MIN_WIN_H_M;
				if (new_w > max_w) new_w = max_w;
				if (new_h > max_h) new_h = max_h;

				mc->clients[s].window_width_m = new_w;
				mc->clients[s].window_height_m = new_h;

				slot_pose_to_pixel_rect(sys, &mc->clients[s],
				                        &mc->clients[s].window_rect_x,
				                        &mc->clients[s].window_rect_y,
				                        &mc->clients[s].window_rect_w,
				                        &mc->clients[s].window_rect_h);

				mc->clients[s].hwnd_resize_pending = true;
				multi_compositor_update_input_forward(mc);
				} // end else (plain scroll resize)
			}
		}
	}

	// [ / ] keys: step Z depth ±5mm for focused window.
	if (mc->focused_slot >= 0 && mc->focused_slot < D3D11_MULTI_MAX_CLIENTS &&
	    mc->clients[mc->focused_slot].active) {
		float z_step = 0.0f;
		if (GetAsyncKeyState(VK_OEM_4) & 1) z_step = -0.005f;  // [ = back
		if (GetAsyncKeyState(VK_OEM_6) & 1) z_step =  0.005f;  // ] = forward
		if (z_step != 0.0f) {
			int s = mc->focused_slot;
			float new_z = mc->clients[s].window_pose.position.z + z_step;
			if (new_z < -0.05f) new_z = -0.05f;
			if (new_z >  0.05f) new_z =  0.05f;
			mc->clients[s].window_pose.position.z = new_z;

			slot_pose_to_pixel_rect(sys, &mc->clients[s],
			                        &mc->clients[s].window_rect_x,
			                        &mc->clients[s].window_rect_y,
			                        &mc->clients[s].window_rect_w,
			                        &mc->clients[s].window_rect_h);
			U_LOG_I("Multi-comp: [/] Z depth slot %d → Z=%.3fm", s, new_z);
		}
	}

	// Handle swap chain resize
	if (mc->hwnd != nullptr && mc->swap_chain) {
		RECT client_rect;
		if (GetClientRect(mc->hwnd, &client_rect)) {
			uint32_t cw = static_cast<uint32_t>(client_rect.right - client_rect.left);
			uint32_t ch = static_cast<uint32_t>(client_rect.bottom - client_rect.top);

			DXGI_SWAP_CHAIN_DESC1 sc_desc = {};
			mc->swap_chain->GetDesc1(&sc_desc);

			if (cw > 0 && ch > 0 && (sc_desc.Width != cw || sc_desc.Height != ch)) {
				mc->back_buffer_rtv.reset();
				HRESULT hr = mc->swap_chain->ResizeBuffers(0, cw, ch, DXGI_FORMAT_UNKNOWN, 0);
				if (SUCCEEDED(hr)) {
					wil::com_ptr<ID3D11Texture2D> bb;
					mc->swap_chain->GetBuffer(0, IID_PPV_ARGS(bb.put()));
					sys->device->CreateRenderTargetView(bb.get(), nullptr, mc->back_buffer_rtv.put());
				}
			}
		}
	}

	// Sync tile layout (2D/3D mode may have changed)
	sync_tile_layout(sys);

	// Get eye positions from DP
	struct xrt_eye_positions eye_pos = {};
	if (mc->display_processor != nullptr) {
		xrt_display_processor_d3d11_get_predicted_eye_positions(mc->display_processor, &eye_pos);
	}
	if (!eye_pos.valid) {
		eye_pos.count = 2;
		eye_pos.eyes[0] = {-0.032f, 0.0f, 0.6f};
		eye_pos.eyes[1] = { 0.032f, 0.0f, 0.6f};
		eye_pos.valid = true;
	}


	// Get physical display dims (used as default virtual window size for new clients)
	float display_w_m = sys->base.info.display_width_m;
	float display_h_m = sys->base.info.display_height_m;
	if (mc->display_processor != nullptr) {
		xrt_display_processor_d3d11_get_display_dimensions(mc->display_processor, &display_w_m, &display_h_m);
	}
	(void)display_w_m;
	(void)display_h_m;

	// Tick per-slot animations (for static layout transitions + entry animations).
	// Phase 2.G: layout-preset state machine moved to the workspace controller,
	// so this no longer needs to skip when a "dynamic" layout is active.
	{
		uint64_t anim_now = os_monotonic_get_ns();
		bool focused_rect_changed = false;
		for (int s = 0; s < D3D11_MULTI_MAX_CLIENTS; s++) {
			if (!mc->clients[s].active || !mc->clients[s].anim.active) continue;
			bool still_running = slot_animate_tick(&mc->clients[s], anim_now);
			slot_pose_to_pixel_rect(sys, &mc->clients[s],
			                        &mc->clients[s].window_rect_x,
			                        &mc->clients[s].window_rect_y,
			                        &mc->clients[s].window_rect_w,
			                        &mc->clients[s].window_rect_h);
			mc->clients[s].hwnd_resize_pending = true;
			if (s == mc->focused_slot) focused_rect_changed = true;
			(void)still_running;
		}
		// Keep the window layer's input-forward rect in sync with the
		// focused slot's current pose. Without this, the forwarder is
		// pinned to the rect that was valid at register_client time
		// (initial tiny spawn at (0,0,0)), so clicks inside the
		// actually-rendered window fall outside the forward rect and
		// never reach the app. First click misses, then MOUSEMOVE with
		// the button held crosses into the stale rect and gets forwarded
		// without a paired LBUTTONDOWN — the app sees a drag with no
		// click. Title-bar drag masks the bug because it also calls
		// update_input_forward at the end.
		if (focused_rect_changed) {
			multi_compositor_update_input_forward(mc);
		}
	}

	// 2D/3D display mode auto-switch based on focused client type.
	// When a capture (2D) window gets focus → switch to 2D mode.
	// When an IPC (3D) app gets focus → restore 3D mode.
	// Uses the same mechanism as V-key toggle: changes active_rendering_mode_index,
	// calls request_display_mode on DP, and sync_tile_layout. Extension apps
	// receive XrEventDataRenderingModeChangedEXT + XrEventDataHardwareDisplayStateChangedEXT
	// automatically via the OXR session's per-frame mode index polling.
	{
		bool want_2d = false;
		if (mc->focused_slot >= 0 && mc->focused_slot < D3D11_MULTI_MAX_CLIENTS &&
		    mc->clients[mc->focused_slot].active &&
		    mc->clients[mc->focused_slot].client_type == CLIENT_TYPE_CAPTURE) {
			want_2d = true;
		}

		if (want_2d != mc->capture_forced_2d) {
			mc->capture_forced_2d = want_2d;

			struct xrt_device *head = (sys->xsysd != nullptr)
			    ? sys->xsysd->static_roles.head : nullptr;

			if (head != nullptr && head->hmd != NULL) {
				uint32_t prev_idx = head->hmd->active_rendering_mode_index;
				if (want_2d) {
					// Save current 3D mode index before switching to 2D
					uint32_t cur = head->hmd->active_rendering_mode_index;
					if (cur < head->rendering_mode_count &&
					    head->rendering_modes[cur].hardware_display_3d) {
						sys->last_3d_mode_index = cur;
					}
					head->hmd->active_rendering_mode_index = 0; // Mode 0 = 2D mono
				} else {
					// Restore 3D mode
					head->hmd->active_rendering_mode_index = sys->last_3d_mode_index;
				}
				broadcast_rendering_mode_change(sys, head, prev_idx,
				                                head->hmd->active_rendering_mode_index);
			}

			// Switch display HW mode
			if (mc->display_processor != nullptr) {
				xrt_display_processor_d3d11_request_display_mode(
				    mc->display_processor, !want_2d);
			}

			sync_tile_layout(sys);
			sys->hardware_display_3d = !want_2d;

			U_LOG_W("Multi-comp: auto display mode → %s (focused slot %d, type=%s)",
			        want_2d ? "2D" : "3D", mc->focused_slot,
			        want_2d ? "capture" : "IPC");
		}
	}

	// Deferred HWND resize: resize app windows to their assigned sub-rects.
	// Uses SWP_ASYNCWINDOWPOS to avoid cross-process deadlock.
	for (int s = 0; s < D3D11_MULTI_MAX_CLIENTS; s++) {
		if (mc->clients[s].active && mc->clients[s].hwnd_resize_pending &&
		    mc->clients[s].app_hwnd != NULL &&
		    mc->clients[s].client_type != CLIENT_TYPE_CAPTURE) {
			// HWND = visual 3D window size. App uses this for Kooima
			// (physical screen dims) and render resolution (HWND * scale).
			// Position at display center so eye offsets come from xrLocateViews.
			uint32_t hwnd_w = mc->clients[s].window_rect_w;
			uint32_t hwnd_h = mc->clients[s].window_rect_h;
			// Center on display
			uint32_t disp_w = sys->base.info.display_pixel_width;
			uint32_t disp_h = sys->base.info.display_pixel_height;
			if (disp_w == 0) disp_w = 3840;
			if (disp_h == 0) disp_h = 2160;
			int hwnd_x = ((int)disp_w - (int)hwnd_w) / 2;
			int hwnd_y = ((int)disp_h - (int)hwnd_h) / 2;
			SetWindowPos(mc->clients[s].app_hwnd, HWND_BOTTOM,
			             hwnd_x, hwnd_y,
			             (int)hwnd_w, (int)hwnd_h,
			             SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
			mc->clients[s].hwnd_resize_pending = false;
			U_LOG_I("Multi-comp: resized app HWND %p to pos=(%d,%d) size=%ux%u (centered, visual rect=%u,%u,%u,%u)",
			        (void*)mc->clients[s].app_hwnd,
			        hwnd_x, hwnd_y, hwnd_w, hwnd_h,
			        mc->clients[s].window_rect_x, mc->clients[s].window_rect_y,
			        mc->clients[s].window_rect_w, mc->clients[s].window_rect_h);
		}

		// Capture clients: resize the source HWND so the captured content
		// re-renders at the new size instead of stretching.
		if (mc->clients[s].active && mc->clients[s].hwnd_resize_pending &&
		    mc->clients[s].app_hwnd != NULL &&
		    mc->clients[s].client_type == CLIENT_TYPE_CAPTURE) {
			// Convert virtual window meters → pixels using DPI
			UINT dpi = GetDpiForWindow(mc->clients[s].app_hwnd);
			if (dpi == 0) dpi = 96;
			int new_w = (int)(mc->clients[s].window_width_m / 0.0254f * (float)dpi);
			int new_h = (int)(mc->clients[s].window_height_m / 0.0254f * (float)dpi);
			if (new_w < 200) new_w = 200;
			if (new_h < 150) new_h = 150;

			SetWindowPos(mc->clients[s].app_hwnd, NULL,
			             0, 0, new_w, new_h,
			             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
			mc->clients[s].hwnd_resize_pending = false;
			U_LOG_I("Multi-comp: resized capture HWND %p to %dx%d px",
			        (void *)mc->clients[s].app_hwnd, new_w, new_h);
		}
	}

	// Dispatch buffered input events to focused capture client via SendInput.
	// Must run before rendering so input arrives promptly (~16ms latency).
	multi_compositor_dispatch_capture_input(mc);

	// Clear combined atlas to dark gray background each frame.
	{
		float bg_color[4] = {0.102f, 0.102f, 0.102f, 1.0f}; // #1a1a1a
		sys->context->ClearRenderTargetView(mc->combined_atlas_rtv.get(), bg_color);
		// Phase 2.K: clear depth target to far (1.0) so the per-slot LESS
		// test resolves occlusion from scratch each frame.
		if (mc->combined_atlas_dsv) {
			sys->context->ClearDepthStencilView(mc->combined_atlas_dsv.get(),
			                                    D3D11_CLEAR_DEPTH, 1.0f, 0);
		}
	}

	// Empty state: DisplayXR logo + "Press Ctrl+L" hint when no clients are visible
	// and the launcher isn't open.
	if (mc->client_count == 0 && !mc->launcher_visible && mc->font_atlas_srv) {
		// Lazy-load the embedded logo PNG on first entry.
		if (!mc->logo_load_tried) {
			mc->logo_load_tried = true;
			ID3D11ShaderResourceView *srv = nullptr;
			uint32_t lw = 0, lh = 0;
			if (d3d11_icon_load_from_memory(sys->device.get(), displayxr_white_png,
			                                 displayxr_white_png_size,
			                                 "doc/displayxr_white.png", &srv, &lw, &lh)) {
				mc->logo_srv.attach(srv);
				mc->logo_w = lw;
				mc->logo_h = lh;
			}
		}

		uint32_t ca_w = sys->base.info.display_pixel_width;
		uint32_t ca_h = sys->base.info.display_pixel_height;
		if (ca_w == 0) ca_w = 3840;
		if (ca_h == 0) ca_h = 2160;
		uint32_t num_views = sys->tile_columns * sys->tile_rows;
		uint32_t half_w, half_h;
		resolve_active_view_dims(sys, ca_w, ca_h, &half_w, &half_h);
		const float scale = 3.0f;
		const float gh = (float)mc->font_glyph_h * scale;
		const char *hint = "Press Ctrl+L to open launcher";

		// Logo target height: 35% of view height, preserving aspect ratio.
		float logo_dst_h = (float)half_h * 0.35f;
		float logo_dst_w = 0.0f;
		if (mc->logo_srv && mc->logo_h > 0) {
			logo_dst_w = logo_dst_h * (float)mc->logo_w / (float)mc->logo_h;
		}
		const float gap = gh * 0.8f; // gap between logo and hint text

		sys->context->VSSetShader(sys->blit_vs.get(), nullptr, 0);
		sys->context->PSSetShader(sys->blit_ps.get(), nullptr, 0);
		sys->context->VSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
		sys->context->PSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
		sys->context->PSSetSamplers(0, 1, sys->sampler_linear.addressof());
		ID3D11RenderTargetView *rtvs[] = {mc->combined_atlas_rtv.get()};
		sys->context->OMSetRenderTargets(1, rtvs, nullptr);
		sys->context->OMSetBlendState(sys->blend_alpha.get(), nullptr, 0xFFFFFFFF);
		sys->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		sys->context->IASetInputLayout(nullptr);
		sys->context->RSSetState(sys->rasterizer_state.get());
		sys->context->OMSetDepthStencilState(sys->depth_disabled.get(), 0);
		D3D11_VIEWPORT vp = {};
		vp.Width = (float)ca_w; vp.Height = (float)ca_h; vp.MaxDepth = 1.0f;
		sys->context->RSSetViewports(1, &vp);

		for (uint32_t v = 0; v < num_views && v < XRT_MAX_VIEWS; v++) {
			uint32_t col = v % sys->tile_columns;
			uint32_t row = v / sys->tile_columns;
			float cx = (float)(col * half_w) + (float)half_w * 0.5f;
			float cy = (float)(row * half_h) + (float)half_h * 0.5f;

			// Stack: [logo] [gap] [hint]. Center the whole stack vertically.
			float block_h = (mc->logo_srv ? logo_dst_h + gap : 0.0f) + gh;
			float logo_y = cy - block_h * 0.5f;
			float hint_y = logo_y + (mc->logo_srv ? logo_dst_h + gap : 0.0f);

			// --- Logo quad ---
			if (mc->logo_srv) {
				ID3D11ShaderResourceView *logo_srv = mc->logo_srv.get();
				sys->context->PSSetShaderResources(0, 1, &logo_srv);
				D3D11_MAPPED_SUBRESOURCE m;
				if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
				              D3D11_MAP_WRITE_DISCARD, 0, &m))) {
					BlitConstants *cb = static_cast<BlitConstants *>(m.pData);
					cb->src_rect[0] = 0; cb->src_rect[1] = 0;
					cb->src_rect[2] = (float)mc->logo_w; cb->src_rect[3] = (float)mc->logo_h;
					cb->src_size[0] = (float)mc->logo_w; cb->src_size[1] = (float)mc->logo_h;
					cb->dst_size[0] = (float)ca_w; cb->dst_size[1] = (float)ca_h;
					cb->convert_srgb = 0.0f;
					cb->chrome_alpha = 0.0f; // 8.C: 0=full opacity (chrome blits override)
					cb->corner_radius = 0; cb->corner_aspect = 0;
					cb->edge_feather = 0; cb->glow_intensity = 0;
					cb->quad_mode = 0;
					cb->dst_offset[0] = cx - logo_dst_w * 0.5f;
					cb->dst_offset[1] = logo_y;
					cb->dst_rect_wh[0] = logo_dst_w;
					cb->dst_rect_wh[1] = logo_dst_h;
					memset(cb->quad_corners_01, 0, sizeof(cb->quad_corners_01));
					memset(cb->quad_corners_23, 0, sizeof(cb->quad_corners_23));
					sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
					sys->context->Draw(4, 0);
				}
			}

			// --- Hint text ---
			ID3D11ShaderResourceView *font_srv = mc->font_atlas_srv.get();
			sys->context->PSSetShaderResources(0, 1, &font_srv);
			// Measure text width
			float tw = 0;
			for (const char *p = hint; *p; p++) {
				unsigned char ch = (unsigned char)*p;
				if (ch < 0x20 || ch > 0x7E) ch = '?';
				tw += mc->glyph_advances[ch - 0x20] * scale;
			}
			float tx = cx - tw * 0.5f;
			float cursor = 0;
			for (const char *p = hint; *p; p++) {
				unsigned char ch = (unsigned char)*p;
				if (ch < 0x20 || ch > 0x7E) ch = '?';
				int gi = ch - 0x20;
				float src_gw = mc->glyph_advances[gi];
				float src_x = 0;
				for (int i = 0; i < gi; i++) src_x += mc->glyph_advances[i];
				float dst_gw = src_gw * scale;
				D3D11_MAPPED_SUBRESOURCE m;
				if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
				              D3D11_MAP_WRITE_DISCARD, 0, &m))) {
					BlitConstants *cb = static_cast<BlitConstants *>(m.pData);
					cb->src_rect[0] = src_x; cb->src_rect[1] = 0;
					cb->src_rect[2] = src_gw; cb->src_rect[3] = (float)mc->font_glyph_h;
					cb->src_size[0] = (float)mc->font_atlas_w;
					cb->src_size[1] = (float)mc->font_atlas_h;
					cb->dst_size[0] = (float)ca_w; cb->dst_size[1] = (float)ca_h;
					cb->convert_srgb = 0.0f;
					cb->chrome_alpha = 0.0f; // 8.C: 0=full opacity (chrome blits override)
					cb->corner_radius = 0; cb->corner_aspect = 0;
					cb->edge_feather = 0; cb->glow_intensity = 0;
					cb->quad_mode = 0;
					cb->dst_offset[0] = tx + cursor; cb->dst_offset[1] = hint_y;
					cb->dst_rect_wh[0] = dst_gw; cb->dst_rect_wh[1] = gh;
					memset(cb->quad_corners_01, 0, sizeof(cb->quad_corners_01));
					memset(cb->quad_corners_23, 0, sizeof(cb->quad_corners_23));
					sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
					sys->context->Draw(4, 0);
				}
				cursor += dst_gw;
			}
		}
	}

	// Copy client atlas → combined atlas, crop to content dims, send to DP.
	// Render order: back-to-front by Z depth (painter's algorithm).
	// Windows farther from viewer (lower Z) render first, closer windows on top.
	// IPC clients are excluded until they have committed at least one
	// projection layer — their per-client atlas is uninitialized GPU memory
	// in workspace mode (see comment at the atlas-clear gate around :8845), and
	// `content_view_w/_h` are zero until the first commit. Drawing them at
	// intermediate entry-animation sizes during Chrome WebGL initialization
	// produces a narrow black rectangle that jumps to full size when the
	// first frame lands. The animation start time is reset on the
	// false→true transition (in compositor_layer_commit) so the entry
	// animation plays once content is actually available. Capture clients
	// have an analogous gate via `capture_srv` non-null below.
	int render_order[D3D11_MULTI_MAX_CLIENTS];
	int render_count = 0;
	for (int s = 0; s < D3D11_MULTI_MAX_CLIENTS; s++) {
		if (mc->clients[s].active && !mc->clients[s].minimized) {
			render_order[render_count++] = s;
		}
	}
	// Sort by Z ascending (farthest first → closest last = on top)
	for (int i = 0; i < render_count - 1; i++) {
		for (int j = i + 1; j < render_count; j++) {
			float zi = mc->clients[render_order[i]].window_pose.position.z;
			float zj = mc->clients[render_order[j]].window_pose.position.z;
			if (zi > zj) {
				int tmp = render_order[i];
				render_order[i] = render_order[j];
				render_order[j] = tmp;
			}
		}
	}
	// Phase 2.K: focus-on-top override removed. The depth-test pipeline
	// resolves occlusion per-pixel from window 3D depth, so forcing the
	// focused window to paint last would break depth ordering whenever the
	// focused window is geometrically behind another window (carousel,
	// edge-resize z scrolls, controller-driven 3D layouts). The painter's
	// sort above stays useful for transparent-edge alpha blending — opaque
	// occlusion now comes from the depth buffer.
	uint32_t dp_view_w = sys->view_width;
	uint32_t dp_view_h = sys->view_height;
	for (int ri = 0; ri < render_count; ri++) {
		int s = render_order[ri];

		// Determine source SRV and dimensions based on client type.
		ID3D11ShaderResourceView *slot_srv = nullptr;
		uint32_t cvw = 0, cvh = 0;       // content view dimensions
		uint32_t src_tex_w = 0, src_tex_h = 0; // source texture dimensions (for UV)
		uint32_t slot_w_atlas = 0, slot_h_atlas = 0; // atlas-derived per-tile stride
		bool slot_is_mono = false;
		bool slot_flip_y = false;

		if (mc->clients[s].client_type == CLIENT_TYPE_CAPTURE) {
			// Capture client: get latest captured texture
			capture_slot_update_srv(sys, &mc->clients[s]);
			if (!mc->clients[s].capture_srv) continue;
			slot_srv = mc->clients[s].capture_srv.get();
			cvw = mc->clients[s].capture_width;
			cvh = mc->clients[s].capture_height;
			src_tex_w = cvw;
			src_tex_h = cvh;
			slot_is_mono = true;
			slot_flip_y = false;
		} else {
			// IPC client: use compositor atlas
			struct d3d11_service_compositor *cc = mc->clients[s].compositor;
			if (cc == nullptr || !cc->render.atlas_texture || !cc->render.atlas_srv) {
				continue;
			}

			// Pick UNORM vs SRGB-typed SRV onto the per-client atlas
			// based on whether the client's most-recent swapchain was
			// SRGB-encoded. Atlas storage is TYPELESS in workspace mode (see
			// init_client_render_resources), so both views were created
			// up-front. The SRGB-SRV path makes the GPU auto-linearize
			// on sample — the multi-comp shader (passthrough at
			// convert_srgb=0) then writes linear values to the combined
			// atlas, which is what the DP weaver expects. Falls back to
			// the UNORM SRV if the SRGB SRV isn't available (non-workspace
			// atlas storage is UNORM and only atlas_srv exists; not
			// expected to reach here in non-workspace mode but stays robust).
			if (cc->atlas_holds_srgb_bytes && cc->render.atlas_srv_srgb) {
				slot_srv = cc->render.atlas_srv_srgb.get();
			} else {
				slot_srv = cc->render.atlas_srv.get();
			}
			cvw = mc->clients[s].content_view_w;
			cvh = mc->clients[s].content_view_h;
			if (cvw == 0 || cvh == 0) {
				cvw = dp_view_w;
				cvh = dp_view_h;
			}
			D3D11_TEXTURE2D_DESC client_atlas_desc = {};
			cc->render.atlas_texture->GetDesc(&client_atlas_desc);
			src_tex_w = client_atlas_desc.Width;
			src_tex_h = client_atlas_desc.Height;
			slot_flip_y = cc->atlas_flip_y;

			// Tile-overflow guard. The per-tile source origin below is
			// `src_col * slot_w_atlas` where `slot_w_atlas = atlas_w /
			// tile_columns` — same stride compositor_layer_commit writes
			// at. cvw > slot_w_atlas would make multi-comp read past the
			// slot boundary into the neighbouring tile's content
			// (`feedback_atlas_stride_invariant`: stride and clamp must
			// use the SAME atlas-derived slot width).
			slot_w_atlas = src_tex_w / sys->tile_columns;
			slot_h_atlas = src_tex_h / sys->tile_rows;
			assert(cvw <= slot_w_atlas);
			assert(cvh <= slot_h_atlas);
			if (cvw > slot_w_atlas) cvw = slot_w_atlas;
			if (cvh > slot_h_atlas) cvh = slot_h_atlas;
		}

		// Combined atlas dimensions.
		uint32_t ca_w = sys->base.info.display_pixel_width;
		uint32_t ca_h = sys->base.info.display_pixel_height;
		if (ca_w == 0) ca_w = 3840;
		if (ca_h == 0) ca_h = 2160;
		// Tile layout dims: non-legacy sessions use the true per-view dims
		// (e.g. 1920×1080 stereo), legacy uses the atlas-divided compromise.
		// Issue #158.
		uint32_t half_w, half_h;
		resolve_active_view_dims(sys, ca_w, ca_h, &half_w, &half_h);

		// Per-eye projected pixel rects (parallax shift for windows at Z != 0).
		int32_t eye_rect_x[2], eye_rect_y[2], eye_rect_w[2], eye_rect_h[2];
		for (int eye = 0; eye < 2; eye++) {
			int ei = (eye < (int)eye_pos.count) ? eye : 0;
			slot_pose_to_pixel_rect_for_eye(sys, &mc->clients[s],
			    eye_pos.eyes[ei].x, eye_pos.eyes[ei].y, eye_pos.eyes[ei].z,
			    &eye_rect_x[eye], &eye_rect_y[eye],
			    &eye_rect_w[eye], &eye_rect_h[eye]);
		}

		// Show a spinner placeholder (8 dots in a ring, one bright
		// rotating) instead of the content blit while the IPC client
		// hasn't produced real content yet. Phase 1: pre-first-commit
		// (per-client atlas is uninitialized GPU memory). Phase 2:
		// post-first-commit grace window — Chrome's WebXR pipeline
		// keeps submitting frames at 60Hz while the page's WebGL
		// pipeline (texture loading, shader compile) is still in
		// warmup, so the swapchain (and therefore the atlas) is
		// GPU-cleared black for ~1–3s past first commit. The spinner
		// keeps the slot's chrome visible from the moment of register
		// — the user sees a loading window, not a black hole.
		// Show the spinner placeholder while the IPC client is in its
		// loading window. Two-phase gate:
		//   Phase 1 (pre-first-commit): per-client atlas is uninitialized
		//     GPU memory. The slot's content_view_w/_h are zero. Showing
		//     Chrome's content here would render garbage / black.
		//   Phase 2 (post-first-commit grace): Chrome's WebXR pipeline
		//     keeps submitting frames at 60Hz while Three.js's WebGL
		//     pipeline (texture loading, shader compile) is still in
		//     warmup. The swapchain (and therefore the per-client atlas)
		//     is GPU-cleared black for ~1–3 s past first commit. Without
		//     phase 2, the moment the gate releases the slot snaps to a
		//     black interior until WebGL produces real frames.
		// The grace window is generous (3 s) because Chrome cold-start +
		// Three.js init can be slow; better to over-shoot and have the
		// spinner cross-fade into real content than under-shoot and have
		// the user see a black flash.
		bool show_spinner = false;
		if (mc->clients[s].client_type == CLIENT_TYPE_IPC) {
			if (!mc->clients[s].has_first_frame_committed) {
				show_spinner = true;
			} else {
				const uint64_t POST_COMMIT_GRACE_NS =
				    3000ULL * 1000000ULL;
				uint64_t age = os_monotonic_get_ns() -
				    mc->clients[s].first_frame_ns;
				if (age < POST_COMMIT_GRACE_NS) {
					show_spinner = true;
				}
			}
		}

		// Shader blit each view → combined atlas.
		uint32_t num_views = sys->tile_columns * sys->tile_rows;
		for (uint32_t v = 0; v < num_views && v < XRT_MAX_VIEWS; v++) {
			uint32_t src_col = v % sys->tile_columns;
			uint32_t src_row = v / sys->tile_columns;

			// Per-tile source origin in the per-client atlas.
			// Mono clients use (0,0) for all views; IPC clients place
			// each tile at `slot_w_atlas × slot_h_atlas` stride — same
			// stride compositor_layer_commit writes at, derived from
			// atlas/tile_columns NOT sys->view_width
			// (`feedback_atlas_stride_invariant`: in workspace mode the
			// per-client atlas is created at native pixel dims while
			// sys->view_width tracks the SCALED view dim; they diverge).
			// Content within each tile may be smaller than the slot
			// (top-left), and its extent is cvw × cvh; the shader's
			// `src_rect.zw = cvw,cvh` (set below) drives sampling, the
			// per-tile origin sets `src_rect.xy`.
			float src_px_x, src_px_y;
			if (slot_is_mono) {
				src_px_x = 0.0f;
				src_px_y = 0.0f;
			} else {
				src_px_x = static_cast<float>(src_col * slot_w_atlas);
				src_px_y = static_cast<float>(src_row * slot_h_atlas);
			}

			// Per-eye destination rect (parallax-shifted for Z != 0)
			int eye_idx = (src_col < 2) ? (int)src_col : 0;
			float win_frac_x = (float)eye_rect_x[eye_idx] / (float)ca_w;
			float win_frac_y = (float)eye_rect_y[eye_idx] / (float)ca_h;
			float win_frac_w = (float)eye_rect_w[eye_idx] / (float)ca_w;
			float win_frac_h = (float)eye_rect_h[eye_idx] / (float)ca_h;
			float dest_px_x = src_col * half_w + win_frac_x * half_w;
			float dest_px_y = src_row * half_h + win_frac_y * half_h;
			float dest_px_w = win_frac_w * half_w;
			float dest_px_h = win_frac_h * half_h;

			D3D11_RECT scissor;
			scissor.left = (LONG)(src_col * half_w);
			scissor.top = (LONG)(src_row * half_h);
			scissor.right = (LONG)((src_col + 1) * half_w);
			scissor.bottom = (LONG)((src_row + 1) * half_h);
			sys->context->RSSetScissorRects(1, &scissor);

			// Check for rotated window → perspective quad mode
			int ei_for_quad = (src_col < 2) ? (int)src_col : 0;
			int ei_q = (ei_for_quad < (int)eye_pos.count) ? ei_for_quad : 0;
			float quad_corners[8] = {};
			float quad_w_vals[4] = {1, 1, 1, 1};
			bool use_quad = compute_projected_quad_corners(
			    sys, &mc->clients[s],
			    eye_pos.eyes[ei_q].x, eye_pos.eyes[ei_q].y, eye_pos.eyes[ei_q].z,
			    src_col, src_row, half_w, half_h, ca_w, ca_h,
			    quad_corners, quad_w_vals);

			// Phase 2.K Commit 8.G: focus rim glow now drawn AFTER content
			// blit (see end of v-loop) so the rim overlays the window edge
			// rather than peeking out from behind. The rim uses the content
			// quad's own corners — under tilt, project_local_rect_for_eye
			// has already projected those corners, so the rim follows the
			// window's orientation automatically with no extra plumbing.

			if (show_spinner) {
				// Loading placeholder: 8 small circles arranged
				// in a ring around the slot center, one bright
				// (white) and the rest dim (gray), with the
				// bright index advancing ~4 times per second
				// (one full revolution every 2s). Circles via
				// corner_radius=-0.5 + corner_aspect=-1.0 (all
				// four corners rounded to half the quad height).
				float slot_cx = dest_px_x + dest_px_w * 0.5f;
				float slot_cy = dest_px_y + dest_px_h * 0.5f;
				// Sized so the spinner reads at full-slot scale —
				// the entry animation grows the slot from ~244px
				// to ~1555px, and a proportionally small spinner
				// (e.g. 0.07) becomes a tiny dot against a wide
				// dark-gray interior at full size. ~0.18 keeps it
				// visible across the whole animation range.
				float ring_r =
				    fminf(dest_px_w, dest_px_h) * 0.18f;
				float dot_size =
				    fminf(dest_px_w, dest_px_h) * 0.05f;
				if (dot_size < 6.0f) dot_size = 6.0f;
				uint64_t now_ns = os_monotonic_get_ns();
				float now_s = (float)(now_ns / 1000000ULL) /
				              1000.0f;
				int active_dot = (int)(now_s * 4.0f) & 7;
				const float TWO_PI = 6.28318530718f;

				// One-time pipeline setup for the dot draws.
				sys->context->VSSetShader(
				    sys->blit_vs.get(), nullptr, 0);
				sys->context->PSSetShader(
				    sys->blit_ps.get(), nullptr, 0);
				sys->context->VSSetConstantBuffers(
				    0, 1, sys->blit_constant_buffer.addressof());
				sys->context->PSSetConstantBuffers(
				    0, 1, sys->blit_constant_buffer.addressof());
				sys->context->PSSetSamplers(
				    0, 1, sys->sampler_linear.addressof());
				ID3D11RenderTargetView *spin_rtvs[] = {
				    mc->combined_atlas_rtv.get()};
				sys->context->OMSetRenderTargets(
				    1, spin_rtvs, nullptr);
				D3D11_VIEWPORT spin_vp = {};
				spin_vp.Width = (float)ca_w;
				spin_vp.Height = (float)ca_h;
				spin_vp.MaxDepth = 1.0f;
				sys->context->RSSetViewports(1, &spin_vp);
				sys->context->IASetPrimitiveTopology(
				    D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
				sys->context->IASetInputLayout(nullptr);
				sys->context->RSSetState(sys->rasterizer_state.get());
				sys->context->OMSetDepthStencilState(
				    sys->depth_disabled.get(), 0);
				sys->context->OMSetBlendState(
				    sys->blend_alpha.get(), nullptr, 0xFFFFFFFF);

				for (int i = 0; i < 8; i++) {
					float a = (float)i * (TWO_PI / 8.0f);
					float dx = slot_cx + cosf(a) * ring_r -
					           dot_size * 0.5f;
					float dy = slot_cy + sinf(a) * ring_r -
					           dot_size * 0.5f;
					// Bright leading dot, then a trailing
					// fade so it reads as motion.
					int trail = (active_dot - i) & 7;
					float brightness;
					if (trail == 0) brightness = 1.0f;
					else if (trail == 1) brightness = 0.75f;
					else if (trail == 2) brightness = 0.55f;
					else if (trail == 3) brightness = 0.40f;
					else brightness = 0.30f;

					D3D11_MAPPED_SUBRESOURCE m;
					if (FAILED(sys->context->Map(
					    sys->blit_constant_buffer.get(), 0,
					    D3D11_MAP_WRITE_DISCARD, 0, &m))) {
						continue;
					}
					BlitConstants *cb =
					    static_cast<BlitConstants *>(m.pData);
					memset(cb, 0, sizeof(*cb));
					// Solid color mode: shader returns
					// src_rect.xyz as RGB.
					cb->src_rect[0] = brightness;
					cb->src_rect[1] = brightness;
					cb->src_rect[2] = brightness;
					cb->src_rect[3] = 1.0f;
					cb->src_size[0] = 1.0f;
					cb->src_size[1] = 1.0f;
					cb->dst_size[0] = (float)ca_w;
					cb->dst_size[1] = (float)ca_h;
					cb->convert_srgb = 2.0f;
					cb->chrome_alpha = 0.0f; // 8.C: 0=full opacity (chrome blits override)
					cb->dst_offset[0] = dx;
					cb->dst_offset[1] = dy;
					cb->dst_rect_wh[0] = dot_size;
					cb->dst_rect_wh[1] = dot_size;
					// All-four-corners rounded → full circle.
					cb->corner_radius = -0.5f;
					cb->corner_aspect = -1.0f;
					// Light feather just for edge AA on the
					// circle, no longer simulating a soft dot.
					cb->edge_feather = 0.08f;
					cb->glow_intensity = 0.0f;
					sys->context->Unmap(
					    sys->blit_constant_buffer.get(), 0);
					sys->context->Draw(4, 0);
				}
			} else {
				// Update constant buffer
				D3D11_MAPPED_SUBRESOURCE mapped;
				HRESULT hr = sys->context->Map(sys->blit_constant_buffer.get(), 0,
				                                D3D11_MAP_WRITE_DISCARD, 0, &mapped);
				if (FAILED(hr)) continue;
				BlitConstants *cb = static_cast<BlitConstants *>(mapped.pData);
				cb->src_rect[0] = src_px_x;
				cb->src_rect[2] = static_cast<float>(cvw);
				if (slot_flip_y) {
					cb->src_rect[1] = src_px_y + static_cast<float>(cvh);
					cb->src_rect[3] = -static_cast<float>(cvh);
				} else {
					cb->src_rect[1] = src_px_y;
					cb->src_rect[3] = static_cast<float>(cvh);
				}
				cb->dst_offset[0] = dest_px_x;
				cb->dst_offset[1] = dest_px_y;
				cb->src_size[0] = static_cast<float>(src_tex_w);
				cb->src_size[1] = static_cast<float>(src_tex_h);
				cb->dst_size[0] = static_cast<float>(ca_w);
				cb->dst_size[1] = static_cast<float>(ca_h);
				cb->convert_srgb = 0.0f;
				cb->chrome_alpha = 0.0f; // 8.C: 0=full opacity (chrome blits override)
				cb->quad_mode = use_quad ? 1.0f : 0.0f;
				cb->dst_rect_wh[0] = dest_px_w;
				cb->dst_rect_wh[1] = dest_px_h;
				// Phase 2.K Commit 8.B: round all four corners of the
				// content quad to coordinate with the new floating pill
				// chrome. Shader sign convention: corner_radius < 0 with
				// corner_aspect < 0 means all-four. Radius is fraction of
				// content height — 5% reads as a soft, deliberate corner.
				cb->corner_radius = -0.05f;
				cb->corner_aspect = -(mc->clients[s].window_width_m / mc->clients[s].window_height_m);
				cb->edge_feather = UI_EDGE_FEATHER_PX / dest_px_h;
				cb->glow_intensity = 0.0f;
				if (use_quad) {
					blit_set_quad_corners(cb, quad_corners, quad_w_vals);
					// Phase 2.K: per-corner depth from quad_w_vals (which
					// project_local_rect_for_eye populated as eye_z - corner_z).
					blit_set_perspective_depth(cb, quad_w_vals, 0.0f);
				} else {
					memset(cb->quad_corners_01, 0, sizeof(cb->quad_corners_01));
					memset(cb->quad_corners_23, 0, sizeof(cb->quad_corners_23));
					// Phase 2.K: uniform depth across the planar quad.
					blit_set_axis_aligned_depth(cb,
					    eye_pos.eyes[ei_q].z,
					    mc->clients[s].window_pose.position.z, 0.0f);
				}
				sys->context->Unmap(sys->blit_constant_buffer.get(), 0);

				// Pipeline setup
				sys->context->VSSetShader(sys->blit_vs.get(), nullptr, 0);
				sys->context->PSSetShader(sys->blit_ps.get(), nullptr, 0);
				sys->context->VSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
				sys->context->PSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
				sys->context->PSSetShaderResources(0, 1, &slot_srv);
				sys->context->PSSetSamplers(0, 1, sys->sampler_linear.addressof());

				// Phase 2.K: bind DSV alongside the atlas RTV so the depth
				// test resolves per-pixel occlusion between this window and
				// other windows that have already rendered this frame.
				ID3D11RenderTargetView *ca_rtvs[] = {mc->combined_atlas_rtv.get()};
				sys->context->OMSetRenderTargets(1, ca_rtvs, mc->combined_atlas_dsv.get());
				D3D11_VIEWPORT vp = {};
				vp.Width = static_cast<float>(ca_w);
				vp.Height = static_cast<float>(ca_h);
				vp.MaxDepth = 1.0f;
				sys->context->RSSetViewports(1, &vp);
				sys->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
				sys->context->IASetInputLayout(nullptr);
				sys->context->RSSetState(sys->rasterizer_state.get());
				sys->context->OMSetDepthStencilState(sys->depth_test_enabled.get(), 0);
				sys->context->OMSetBlendState(sys->blend_alpha.get(), nullptr, 0xFFFFFFFF);

				sys->context->Draw(4, 0);

				// Phase 2.K Commit 8.G: inner-edge rim glow for the focused
				// window, drawn AFTER content so the rim sits on the visible
				// edge. Reuses the same projected quad (rotated) or axis-
				// aligned dst rect as the content blit, so the rim follows
				// the window's tilt naturally — uv01 in the rim shader is
				// across whichever shape the vertex shader produced.
				if (s == mc->focused_slot && sys->blit_vs && sys->blit_ps) {
					D3D11_MAPPED_SUBRESOURCE rim_mapped;
					if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
					              D3D11_MAP_WRITE_DISCARD, 0, &rim_mapped))) {
						BlitConstants *rcb = static_cast<BlitConstants *>(rim_mapped.pData);
						rcb->src_rect[0] = 0; rcb->src_rect[1] = 0;
						rcb->src_rect[2] = 1; rcb->src_rect[3] = 1;
						rcb->src_size[0] = 1; rcb->src_size[1] = 1;
						rcb->dst_size[0] = (float)ca_w;
						rcb->dst_size[1] = (float)ca_h;
						rcb->convert_srgb = 4.0f;  // inner-rim mode
						rcb->chrome_alpha = 0.0f;
						rcb->corner_radius = 0; rcb->corner_aspect = 0;
						// Per-axis rim band width (uv01-fraction). Glow_extent
						// in X, edge_feather in Y so the rim has uniform
						// physical thickness on non-square windows.
						float win_w_m_local = mc->clients[s].window_width_m;
						float win_h_m_local = mc->clients[s].window_height_m;
						float rim_m = UI_GLOW_MARGIN_M * 0.6f; // narrower than the legacy halo
						rcb->glow_extent = rim_m / win_w_m_local;
						rcb->edge_feather = rim_m / win_h_m_local;
						rcb->glow_intensity = UI_GLOW_INTENSITY;
						rcb->glow_falloff = UI_GLOW_FALLOFF;
						rcb->glow_color[0] = UI_GLOW_R;
						rcb->glow_color[1] = UI_GLOW_G;
						rcb->glow_color[2] = UI_GLOW_B;
						rcb->glow_color[3] = 1.0f;
						if (use_quad) {
							blit_set_quad_corners(rcb, quad_corners, quad_w_vals);
							rcb->dst_offset[0] = 0; rcb->dst_offset[1] = 0;
							rcb->dst_rect_wh[0] = 0; rcb->dst_rect_wh[1] = 0;
						} else {
							rcb->quad_mode = 0;
							rcb->dst_offset[0] = dest_px_x;
							rcb->dst_offset[1] = dest_px_y;
							rcb->dst_rect_wh[0] = dest_px_w;
							rcb->dst_rect_wh[1] = dest_px_h;
							memset(rcb->quad_corners_01, 0, sizeof(rcb->quad_corners_01));
							memset(rcb->quad_corners_23, 0, sizeof(rcb->quad_corners_23));
						}
						sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
						sys->context->OMSetBlendState(sys->blend_premul.get(), nullptr, 0xFFFFFFFF);
						sys->context->OMSetDepthStencilState(sys->depth_disabled.get(), 0);
						sys->context->Draw(4, 0);
						// Restore alpha blend + depth-test for next iteration
						// in case more content is drawn this loop.
						sys->context->OMSetBlendState(sys->blend_alpha.get(), nullptr, 0xFFFFFFFF);
						sys->context->OMSetDepthStencilState(sys->depth_test_enabled.get(), 0);
					}
				}
			}
		}

		// Draw title bar for this slot (inside render_order loop for correct z-order).
		// Skip for capture clients — their captured content includes the native
		// window chrome (title bar, tabs, toolbar), so adding a workspace title bar
		// would be redundant.
		if (mc->clients[s].client_type != CLIENT_TYPE_CAPTURE)
		{
			// `TITLE_BAR_HEIGHT_PX` is in TILE-pixel scale (44.5 px on a
			// 1080-tall tile for an 8 mm bar). To get the atlas-fraction
			// that matches `wfy = eye_rect_y/ca_h` (atlas-pixel scale), we
			// must divide by `half_h` (= tile_px_h), not `ca_h` (= full
			// atlas height). Dividing by `ca_h` gives a 2× understated
			// fraction → pill rendered at half the intended height in the
			// axis-aligned path (the rotated path uses
			// project_local_rect_for_eye with `dpy/ca_h` in atlas-px and
			// renders correctly).
			float tb_h_frac = (float)TITLE_BAR_HEIGHT_PX / (float)half_h;
			// Window local-space dimensions for rotated chrome
			bool is_rotated = !quat_is_identity(&mc->clients[s].window_pose.orientation);
			float win_hw = mc->clients[s].window_width_m / 2.0f;
			float win_hh = mc->clients[s].window_height_m / 2.0f;
			float tb_h_m = UI_TITLE_BAR_H_M;
			float btn_w_m_val = UI_BTN_W_M;
			float glyph_w_m = UI_GLYPH_W_M;
			float glyph_h_m = UI_GLYPH_H_M;
			const struct xrt_quat *win_orient = &mc->clients[s].window_pose.orientation;
			float wcx = mc->clients[s].window_pose.position.x;
			float wcy = mc->clients[s].window_pose.position.y;
			float wcz = mc->clients[s].window_pose.position.z;

			// Phase 2.K Commit 8.B: floating-pill geometry. Pill is 75% of
			// the content's width (centered) and floats above the content
			// quad with a half-tb-height gap. All four corners are fully
			// rounded (radius = 50% of pill height) so the pill reads as
			// a true pill shape.
			const float PILL_W_FRAC = 0.75f;
			const float pill_hw = win_hw * PILL_W_FRAC;
			const float pill_gap_m = tb_h_m * 0.5f;
			const float pill_top_m = win_hh + pill_gap_m + tb_h_m;
			const float pill_bot_m = win_hh + pill_gap_m;
			const float pill_gap_frac = tb_h_frac * 0.5f; // mirrors pill_gap_m in atlas frac

			// Phase 2.K Commit 8.C: tick the per-slot chrome fade and gate
			// the entire render block on alpha. Below 1% alpha there's
			// nothing visible so we skip the chrome blits entirely (saves
			// ~half a dozen draw calls per faded-out window).
			//
			// Inverted shader semantic: cb->chrome_alpha = 1 - a_visible.
			// 0 means "no fade" (full opacity); 1 means "fully transparent".
			float chrome_a = slot_chrome_fade_tick(&mc->clients[s], os_monotonic_get_ns());
			float chrome_fade_attenuation = 1.0f - chrome_a;

			if (tb_h_frac > 0.0f && chrome_a >= 0.01f) {
				for (uint32_t v2 = 0; v2 < num_views && v2 < XRT_MAX_VIEWS; v2++) {
					uint32_t col2 = v2 % sys->tile_columns;
					uint32_t row2 = v2 / sys->tile_columns;
					// Per-eye window frac (parallax for Z != 0)
					int eye_idx2 = (col2 < 2) ? (int)col2 : 0;
					int ei2 = (eye_idx2 < (int)eye_pos.count) ? eye_idx2 : 0;
					float cur_eye_x = eye_pos.eyes[ei2].x;
					float cur_eye_y = eye_pos.eyes[ei2].y;
					float cur_eye_z = eye_pos.eyes[ei2].z;
					float wfx = (float)eye_rect_x[eye_idx2] / (float)ca_w;
					float wfy = (float)eye_rect_y[eye_idx2] / (float)ca_h;
					float wfw = (float)eye_rect_w[eye_idx2] / (float)ca_w;
					// Phase 2.K Commit 8.B: pill axis-aligned dst-rect.
					// `tox/tow` cover the pill (75% of content width,
					// horizontally centered above the content); `toy`
					// adds the pill_gap_frac so the pill floats above
					// rather than attaches.
					float pill_w_frac = wfw * PILL_W_FRAC;
					float pill_x_frac = wfx + wfw * (1.0f - PILL_W_FRAC) * 0.5f;
					float tb_fy = wfy - tb_h_frac - pill_gap_frac;
					float tox = col2 * half_w + pill_x_frac * half_w;
					float toy = row2 * half_h + tb_fy * half_h;
					float tow = pill_w_frac * half_w;
					float toh = tb_h_frac * half_h;

					// Scissor rect clips to tile bounds — uniform overflow on all edges.
					D3D11_RECT tb_scissor;
					tb_scissor.left = (LONG)(col2 * half_w);
					tb_scissor.top = (LONG)(row2 * half_h);
					tb_scissor.right = (LONG)((col2 + 1) * half_w);
					tb_scissor.bottom = (LONG)((row2 + 1) * half_h);
					sys->context->RSSetScissorRects(1, &tb_scissor);

					// Pipeline (opaque solid color)
					sys->context->VSSetShader(sys->blit_vs.get(), nullptr, 0);
					sys->context->PSSetShader(sys->blit_ps.get(), nullptr, 0);
					sys->context->VSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
					sys->context->PSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
					// Phase 2.K: bind DSV + LESS depth-test state so chrome
					// occludes against other windows' content / chrome by
					// per-pixel depth. CHROME_BLIT_POS biases each draw
					// toward the eye by WORKSPACE_CHROME_DEPTH_BIAS so a
					// window's own chrome wins over its own content while
					// still depth-testing across windows.
					ID3D11RenderTargetView *tb_rtvs[] = {mc->combined_atlas_rtv.get()};
					sys->context->OMSetRenderTargets(1, tb_rtvs, mc->combined_atlas_dsv.get());
					sys->context->OMSetBlendState(sys->blend_alpha.get(), nullptr, 0xFFFFFFFF);
					sys->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
					sys->context->IASetInputLayout(nullptr);
					sys->context->RSSetState(sys->rasterizer_state.get());
					sys->context->OMSetDepthStencilState(sys->depth_test_enabled.get(), 0);
					D3D11_VIEWPORT tb_vp = {};
					tb_vp.Width = (float)ca_w; tb_vp.Height = (float)ca_h; tb_vp.MaxDepth = 1.0f;
					sys->context->RSSetViewports(1, &tb_vp);

					// Helper lambda: fill CB position fields for rotated or axis-aligned chrome blit.
					// Phase 2.K: also fills cb->corner_depth_ndc[4] with chrome
					// bias so chrome wins over its own content but loses to
					// closer windows.
					#define CHROME_BLIT_POS(cb_ptr, ll, lt, lr, lb, aa_x, aa_y, aa_w, aa_h) \
						do { \
							if (is_rotated) { \
								float _corners[8], _w[4]; \
								project_local_rect_for_eye(sys, win_orient, wcx, wcy, wcz, \
								    (ll), (lt), (lr), (lb), cur_eye_x, cur_eye_y, cur_eye_z, \
								    col2, row2, half_w, half_h, ca_w, ca_h, _corners, _w); \
								blit_set_quad_corners(cb_ptr, _corners, _w); \
								(cb_ptr)->dst_offset[0] = 0; (cb_ptr)->dst_offset[1] = 0; \
								(cb_ptr)->dst_rect_wh[0] = 0; (cb_ptr)->dst_rect_wh[1] = 0; \
								blit_set_perspective_depth(cb_ptr, _w, WORKSPACE_CHROME_DEPTH_BIAS); \
							} else { \
								(cb_ptr)->quad_mode = 0; \
								(cb_ptr)->dst_offset[0] = (aa_x); (cb_ptr)->dst_offset[1] = (aa_y); \
								(cb_ptr)->dst_rect_wh[0] = (aa_w); (cb_ptr)->dst_rect_wh[1] = (aa_h); \
								memset((cb_ptr)->quad_corners_01, 0, sizeof((cb_ptr)->quad_corners_01)); \
								memset((cb_ptr)->quad_corners_23, 0, sizeof((cb_ptr)->quad_corners_23)); \
								blit_set_axis_aligned_depth(cb_ptr, cur_eye_z, wcz, \
								    WORKSPACE_CHROME_DEPTH_BIAS); \
							} \
						} while(0)

					// Phase 2.K Commit 8.B: pill background. All four
					// corners fully rounded (radius = 50% of pill height,
					// negative+negative-aspect = "all four corners" per
					// shader convention in d3d11_service_shaders.h:
					// blit_ps_hlsl). Slightly more transparent than the
					// old solid bar so the frosted-glass look reads.
					{
						D3D11_MAPPED_SUBRESOURCE mapped;
						if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
						              D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
							BlitConstants *cb = static_cast<BlitConstants *>(mapped.pData);
							// Solid color RGB packed in src_rect.xyz (convert_srgb=2.0
							// reads it). Slightly cooler tint, ~70% alpha for the
							// frosted feel; alpha is applied via the shader's edge
							// math + the dst alpha channel.
							cb->src_rect[0] = 0.20f; cb->src_rect[1] = 0.22f;
							cb->src_rect[2] = 0.28f; cb->src_rect[3] = 0.70f;
							cb->src_size[0] = 1; cb->src_size[1] = 1;
							cb->dst_size[0] = (float)ca_w; cb->dst_size[1] = (float)ca_h;
							cb->convert_srgb = 2.0f;
							cb->chrome_alpha = chrome_fade_attenuation; // 8.C: per-slot pill fade
							// All four corners, full pill curve.
							cb->corner_radius = -0.5f;
							cb->corner_aspect = -((mc->clients[s].window_width_m * PILL_W_FRAC) / UI_TITLE_BAR_H_M);
							cb->edge_feather = UI_EDGE_FEATHER_PX / toh;
							cb->glow_intensity = 0.0f;
							// Pill: 75% width centered, gap above content.
							CHROME_BLIT_POS(cb,
							    -pill_hw, pill_top_m, pill_hw, pill_bot_m,
							    tox, toy, tow, toh);
							sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
							sys->context->Draw(4, 0);
						}
					}
					// Phase 2.K Commit 8.D: 8-dot grip handle in pill center —
					// 2 rows × 4 cols of small circular dots. Visual cue that
					// the pill is the drag/rotate region. Dots are 1mm × 1mm
					// with 1mm gaps between, so the cluster reads as a
					// recognizable grip without crowding the bar's other
					// elements (icon at left, buttons at right).
					{
						const float DOT_SIZE_M = 0.001f;
						const float DOT_GAP_M  = 0.001f;
						const int   GRIP_COLS  = 4;
						const int   GRIP_ROWS  = 2;
						float pill_w_m_total = mc->clients[s].window_width_m * PILL_W_FRAC;
						float dot_w_px = (DOT_SIZE_M / pill_w_m_total) * tow;
						float dot_h_px = (DOT_SIZE_M / tb_h_m) * toh;
						float gap_w_px = (DOT_GAP_M / pill_w_m_total) * tow;
						float gap_h_px = (DOT_GAP_M / tb_h_m) * toh;
						float grid_w_px = (float)GRIP_COLS * dot_w_px + (float)(GRIP_COLS - 1) * gap_w_px;
						float grid_h_px = (float)GRIP_ROWS * dot_h_px + (float)(GRIP_ROWS - 1) * gap_h_px;
						float pill_cx_px = tox + tow * 0.5f;
						float pill_cy_px = toy + toh * 0.5f;
						float grid_x_px = pill_cx_px - grid_w_px * 0.5f;
						float grid_y_px = pill_cy_px - grid_h_px * 0.5f;
						float grip_total_w_m = (float)GRIP_COLS * DOT_SIZE_M +
						                       (float)(GRIP_COLS - 1) * DOT_GAP_M;
						float grip_total_h_m = (float)GRIP_ROWS * DOT_SIZE_M +
						                       (float)(GRIP_ROWS - 1) * DOT_GAP_M;
						float grip_left_m = -grip_total_w_m * 0.5f;
						float grip_bot_m_local = (pill_top_m + pill_bot_m) * 0.5f - grip_total_h_m * 0.5f;
						for (int gr = 0; gr < GRIP_ROWS; gr++) {
							for (int gc = 0; gc < GRIP_COLS; gc++) {
								float dx_local = grip_left_m + (float)gc * (DOT_SIZE_M + DOT_GAP_M);
								float dy_local = grip_bot_m_local + (float)gr * (DOT_SIZE_M + DOT_GAP_M);
								float dot_x_px = grid_x_px + (float)gc * (dot_w_px + gap_w_px);
								float dot_y_px = grid_y_px + (float)gr * (dot_h_px + gap_h_px);
								D3D11_MAPPED_SUBRESOURCE mapped;
								if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
								              D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
									BlitConstants *cb = static_cast<BlitConstants *>(mapped.pData);
									// Light gray dots, ~80% alpha so they read on
									// the slightly-translucent pill bg.
									cb->src_rect[0] = 0.80f; cb->src_rect[1] = 0.82f;
									cb->src_rect[2] = 0.85f; cb->src_rect[3] = 1.0f;
									cb->src_size[0] = 1; cb->src_size[1] = 1;
									cb->dst_size[0] = (float)ca_w; cb->dst_size[1] = (float)ca_h;
									cb->convert_srgb = 2.0f;
									cb->chrome_alpha = chrome_fade_attenuation;
									// All four corners + aspect 1.0 + radius 0.5 = circle.
									cb->corner_radius = -0.5f;
									cb->corner_aspect = -1.0f;
									cb->edge_feather = 0.10f;
									cb->glow_intensity = 0.0f;
									CHROME_BLIT_POS(cb,
									    dx_local, dy_local + DOT_SIZE_M,
									    dx_local + DOT_SIZE_M, dy_local,
									    dot_x_px, dot_y_px, dot_w_px, dot_h_px);
									sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
									sys->context->Draw(4, 0);
								}
							}
						}
					}
					// Phase 2.K Commit 8.E: app icon at pill left, mirror-inset
					// vs the close button at pill right. Reuses the launcher's
					// loaded SRVs (matched by app name, ignoring the " (N)"
					// duplicate-instance suffix). No icon → leave blank.
					//
					// Limitation: the running client's app_name is its OpenXR
					// applicationName (e.g. "SRCubeOpenXRExt") while the
					// launcher's entry name is the sidecar's friendly name
					// ("Cube D3D11 (Handle)"). Test apps don't match. Phase
					// 2.C lifts chrome into the controller, where the
					// controller already knows the exe path it launched and
					// can hand the runtime a pre-resolved icon directly.
					ID3D11ShaderResourceView *app_icon_srv = nullptr;
					uint32_t app_icon_w = 0, app_icon_h = 0;
					{
						char base_name[128];
						snprintf(base_name, sizeof(base_name), "%s",
						         mc->clients[s].app_name);
						char *paren = strrchr(base_name, '(');
						if (paren && paren > base_name && *(paren - 1) == ' ') {
							*(paren - 1) = '\0';
						}
						uint32_t n_apps = sys->launcher_app_count;
						if (n_apps > IPC_LAUNCHER_MAX_APPS) n_apps = IPC_LAUNCHER_MAX_APPS;
						for (uint32_t li = 0; li < n_apps; li++) {
							if (strcmp(sys->launcher_apps[li].name, base_name) == 0) {
								app_icon_srv = sys->launcher_icons[li].srv_2d.get();
								app_icon_w = sys->launcher_icons[li].w_2d;
								app_icon_h = sys->launcher_icons[li].h_2d;
								break;
							}
						}
					}

					// Phase 2.K Commit 8 tweak 3: buttons drawn as circles
					// inset within their 8mm × 8mm slot so they read as
					// distinct controls rather than connected rectangles
					// (matches the concept's small round pill-caps).
					const float BTN_INSET_FRAC = 0.18f;
					float btn_inset_m = btn_w_m_val * BTN_INSET_FRAC;
					float btn_inset_y = tb_h_m * BTN_INSET_FRAC;
					float btn_inset_px_x = (float)CLOSE_BTN_WIDTH_PX * BTN_INSET_FRAC;
					float btn_inset_px_y = toh * BTN_INSET_FRAC;

					// 8.E: blit the app icon (if any) at the pill's left edge.
					// Sized to match the round buttons' visible square (post-inset)
					// so icon and buttons share visual weight.
					if (app_icon_srv != nullptr) {
						D3D11_MAPPED_SUBRESOURCE mapped;
						if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
						              D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
							BlitConstants *cb = static_cast<BlitConstants *>(mapped.pData);
							cb->src_rect[0] = 0;
							cb->src_rect[1] = 0;
							cb->src_rect[2] = (float)app_icon_w;
							cb->src_rect[3] = (float)app_icon_h;
							cb->src_size[0] = (float)app_icon_w;
							cb->src_size[1] = (float)app_icon_h;
							cb->dst_size[0] = (float)ca_w;
							cb->dst_size[1] = (float)ca_h;
							cb->convert_srgb = 0.0f;  // icon SRV is already _SRGB-typed
							cb->chrome_alpha = chrome_fade_attenuation;
							cb->corner_radius = -0.20f;
							cb->corner_aspect = -1.0f;
							cb->edge_feather = 0.05f;
							cb->glow_intensity = 0.0f;
							float icon_x_px = tox + btn_inset_px_x;
							float icon_w_px = (float)CLOSE_BTN_WIDTH_PX - 2.0f * btn_inset_px_x;
							float icon_h_px = toh - 2.0f * btn_inset_px_y;
							float icon_y_px = toy + btn_inset_px_y;
							sys->context->PSSetShaderResources(0, 1, &app_icon_srv);
							CHROME_BLIT_POS(cb,
							    -pill_hw + btn_inset_m, pill_top_m - btn_inset_y,
							    -pill_hw + btn_w_m_val - btn_inset_m, pill_bot_m + btn_inset_y,
							    icon_x_px, icon_y_px, icon_w_px, icon_h_px);
							sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
							sys->context->Draw(4, 0);
						}
					}
					// Close button (red circle)
					{
						D3D11_MAPPED_SUBRESOURCE mapped;
						if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
						              D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
							BlitConstants *cb = static_cast<BlitConstants *>(mapped.pData);
							bool close_hover = (mc->hover_btn == 1 && mc->hover_btn_slot == s);
							cb->src_rect[0] = close_hover ? 0.97f : 0.88f;
							cb->src_rect[1] = close_hover ? 0.32f : 0.24f;
							cb->src_rect[2] = close_hover ? 0.32f : 0.24f;
							cb->src_rect[3] = 1.0f;
							cb->src_size[0] = 1; cb->src_size[1] = 1;
							cb->dst_size[0] = (float)ca_w; cb->dst_size[1] = (float)ca_h;
							cb->convert_srgb = 2.0f;
							cb->chrome_alpha = chrome_fade_attenuation;
							// Full circle: radius = 50% of (smaller) dim,
							// aspect = -1 = "all four corners".
							cb->corner_radius = -0.5f;
							cb->corner_aspect = -1.0f;
							cb->edge_feather = 0.10f;
							cb->glow_intensity = 0.0f;
							float btn_x = tox + tow - (float)CLOSE_BTN_WIDTH_PX + btn_inset_px_x;
							float btn_w_px = (float)CLOSE_BTN_WIDTH_PX - 2.0f * btn_inset_px_x;
							float btn_h_px = toh - 2.0f * btn_inset_px_y;
							float btn_y = toy + btn_inset_px_y;
							CHROME_BLIT_POS(cb,
							    pill_hw - btn_w_m_val + btn_inset_m, pill_top_m - btn_inset_y,
							    pill_hw - btn_inset_m, pill_bot_m + btn_inset_y,
							    btn_x, btn_y, btn_w_px, btn_h_px);
							sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
							sys->context->Draw(4, 0);
						}
					}
					// Minimize button (gray circle)
					{
						D3D11_MAPPED_SUBRESOURCE mapped;
						if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
						              D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
							BlitConstants *cb = static_cast<BlitConstants *>(mapped.pData);
							bool min_hover = (mc->hover_btn == 2 && mc->hover_btn_slot == s);
							cb->src_rect[0] = min_hover ? 0.62f : 0.48f;
							cb->src_rect[1] = min_hover ? 0.64f : 0.50f;
							cb->src_rect[2] = min_hover ? 0.68f : 0.54f;
							cb->src_rect[3] = 1.0f;
							cb->src_size[0] = 1; cb->src_size[1] = 1;
							cb->dst_size[0] = (float)ca_w; cb->dst_size[1] = (float)ca_h;
							cb->convert_srgb = 2.0f;
							cb->chrome_alpha = chrome_fade_attenuation;
							cb->corner_radius = -0.5f;
							cb->corner_aspect = -1.0f;
							cb->edge_feather = 0.10f;
							cb->glow_intensity = 0.0f;
							float min_x = tox + tow - 2.0f * (float)CLOSE_BTN_WIDTH_PX + btn_inset_px_x;
							float btn_w_px = (float)CLOSE_BTN_WIDTH_PX - 2.0f * btn_inset_px_x;
							float btn_h_px = toh - 2.0f * btn_inset_px_y;
							float btn_y = toy + btn_inset_px_y;
							CHROME_BLIT_POS(cb,
							    pill_hw - 2*btn_w_m_val + btn_inset_m, pill_top_m - btn_inset_y,
							    pill_hw - btn_w_m_val - btn_inset_m, pill_bot_m + btn_inset_y,
							    min_x, btn_y, btn_w_px, btn_h_px);
							sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
							sys->context->Draw(4, 0);
						}
					}
					// Maximize/fullscreen button (gray circle)
					{
						D3D11_MAPPED_SUBRESOURCE mapped;
						if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
						              D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
							BlitConstants *cb = static_cast<BlitConstants *>(mapped.pData);
							bool max_hover = (mc->hover_btn == 3 && mc->hover_btn_slot == s);
							cb->src_rect[0] = max_hover ? 0.62f : 0.48f;
							cb->src_rect[1] = max_hover ? 0.64f : 0.50f;
							cb->src_rect[2] = max_hover ? 0.68f : 0.54f;
							cb->src_rect[3] = 1.0f;
							cb->src_size[0] = 1; cb->src_size[1] = 1;
							cb->dst_size[0] = (float)ca_w; cb->dst_size[1] = (float)ca_h;
							cb->convert_srgb = 2.0f;
							cb->chrome_alpha = chrome_fade_attenuation;
							cb->corner_radius = -0.5f;
							cb->corner_aspect = -1.0f;
							cb->edge_feather = 0.10f;
							cb->glow_intensity = 0.0f;
							float max_x = tox + tow - 3.0f * (float)CLOSE_BTN_WIDTH_PX + btn_inset_px_x;
							float btn_w_px = (float)CLOSE_BTN_WIDTH_PX - 2.0f * btn_inset_px_x;
							float btn_h_px = toh - 2.0f * btn_inset_px_y;
							float btn_y = toy + btn_inset_px_y;
							CHROME_BLIT_POS(cb,
							    pill_hw - 3*btn_w_m_val + btn_inset_m, pill_top_m - btn_inset_y,
							    pill_hw - 2*btn_w_m_val - btn_inset_m, pill_bot_m + btn_inset_y,
							    max_x, btn_y, btn_w_px, btn_h_px);
							sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
							sys->context->Draw(4, 0);
						}
					}
					// Text + glyphs
					if (mc->font_atlas_srv && mc->clients[s].app_name[0] != '\0') {
						sys->context->OMSetBlendState(sys->blend_alpha.get(), nullptr, 0xFFFFFFFF);
						ID3D11ShaderResourceView *font_srv = mc->font_atlas_srv.get();
						sys->context->PSSetShaderResources(0, 1, &font_srv);
						sys->context->PSSetSamplers(0, 1, sys->sampler_linear.addressof());

						const char *name = mc->clients[s].app_name;
						// Render glyphs at 1:1 from atlas (no vertical stretch).
						// If atlas cell is shorter than title bar, text is vertically centered.
						float gh = (float)mc->font_glyph_h;
						if (gh > toh) gh = toh; // cap to title bar height
						float gpad = (toh - gh) / 2.0f; // vertical centering
						if (gpad < 0) gpad = 0;
						// Phase 2.K Commit 8 tweak 5: tunable manual y-bias for the
						// font-rendered glyphs. D2D's vertical-centering inside the
						// atlas cell is asymmetric (cell reserves descender room),
						// so the visible glyph drifts inside the cell; magnitude
						// depends on the font's exact metrics. Easier to dial in
						// than to compute from font internals.
						//
						// Units: atlas pixels. NEGATIVE = lift glyph UP. Increase
						// magnitude (more negative) if glyphs look too low; decrease
						// toward 0 if too high. Affects close-X, minimize-`-`, and
						// the title text. The procedural maximize square + grip
						// dots are unaffected (they're already centered).
						const float GLYPH_Y_BIAS_PX = -6.0f;
						// In world-local Y (+Y = up), "lift up by N atlas-px"
						// equals "+N * disp_h_m / tile_px_h meters". Sign-flip
						// since GLYPH_Y_BIAS_PX is negative-when-lifting.
						const float GLYPH_Y_LIFT_M = (-GLYPH_Y_BIAS_PX) /
						                             (float)half_h *
						                             sys->base.info.display_height_m;
						// Glyph vertical centering in meters (for rotated path)
						float glyph_render_h_m = glyph_h_m * (gh / (float)GLYPH_H);
						float glyph_vpad_m = (tb_h_m - glyph_render_h_m) / 2.0f;
						if (glyph_vpad_m < 0) glyph_vpad_m = 0;

						// Compute atlas x-offset for each glyph (cumulative advances)
						float px_cursor = 0; // pixel cursor in dest
						float avail_w = tow - (float)GLYPH_W - 3.0f * (float)CLOSE_BTN_WIDTH_PX;
						float scale = 1.0f; // 1:1 rendering from atlas
						for (int ci = 0; ci < 30 && name[ci] != '\0'; ci++) {
							unsigned char ch = (unsigned char)name[ci];
							if (ch < 0x20 || ch > 0x7E) ch = '?';
							int gi = ch - 0x20;
							float src_gw = mc->glyph_advances[gi];
							// Compute atlas x for this glyph (sum of all prior advances)
							float src_x = 0;
							for (int p = 0; p < gi; p++) src_x += mc->glyph_advances[p];

							float dst_gw = src_gw * scale;
							if (px_cursor + dst_gw > avail_w) break;

							D3D11_MAPPED_SUBRESOURCE mapped;
							if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
							              D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
								BlitConstants *cb = static_cast<BlitConstants *>(mapped.pData);
								cb->src_rect[0] = src_x; cb->src_rect[1] = 0;
								cb->src_rect[2] = src_gw; cb->src_rect[3] = (float)mc->font_glyph_h;
								cb->src_size[0] = (float)mc->font_atlas_w;
								cb->src_size[1] = (float)mc->font_atlas_h;
								cb->dst_size[0] = (float)ca_w; cb->dst_size[1] = (float)ca_h;
								cb->convert_srgb = 0.0f;
								cb->chrome_alpha = chrome_fade_attenuation; // 8.C: per-slot pill fade
								cb->corner_radius = 0; cb->corner_aspect = 0;
								cb->edge_feather = 0.0f; cb->glow_intensity = 0.0f;
								// Proportional glyph positioning — anchored
								// to the pill (not the window) under 8.B.
								float m_per_px = glyph_w_m / ((float)GLYPH_W > 0 ? (float)GLYPH_W : 1.0f);
								float gl_left = -pill_hw + glyph_w_m + px_cursor * m_per_px;
								float gl_top = pill_top_m - glyph_vpad_m + GLYPH_Y_LIFT_M;
								float gl_right = gl_left + dst_gw * m_per_px;
								float gl_bottom = gl_top - glyph_render_h_m;
								CHROME_BLIT_POS(cb,
								    gl_left, gl_top, gl_right, gl_bottom,
								    tox + (float)GLYPH_W + px_cursor, toy + gpad + GLYPH_Y_BIAS_PX, dst_gw, gh);
								sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
								sys->context->Draw(4, 0);
							}
							px_cursor += dst_gw;
						}
						// Helper: compute atlas x offset for glyph index
						auto atlas_x_for = [&](int gi) -> float {
							float x = 0;
							for (int p = 0; p < gi && p < 96; p++) x += mc->glyph_advances[p];
							return x;
						};
						float btn_scale = gh / (float)mc->font_glyph_h;

						// X glyph on close button (centered in button)
						{
							int xg = 'X' - 0x20;
							float src_gw = mc->glyph_advances[xg];
							float dst_gw = src_gw * btn_scale;
							float bx = tox + tow - (float)CLOSE_BTN_WIDTH_PX + ((float)CLOSE_BTN_WIDTH_PX - dst_gw) / 2.0f;
							// Local: centered in close button
							float xg_left = pill_hw - btn_w_m_val + (btn_w_m_val - glyph_w_m) / 2.0f;
							float xg_top = pill_top_m - glyph_vpad_m + GLYPH_Y_LIFT_M;
							D3D11_MAPPED_SUBRESOURCE mapped;
							if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
							              D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
								BlitConstants *cb = static_cast<BlitConstants *>(mapped.pData);
								cb->src_rect[0] = atlas_x_for(xg); cb->src_rect[1] = 0;
								cb->src_rect[2] = src_gw; cb->src_rect[3] = (float)mc->font_glyph_h;
								cb->src_size[0] = (float)mc->font_atlas_w;
								cb->src_size[1] = (float)mc->font_atlas_h;
								cb->dst_size[0] = (float)ca_w; cb->dst_size[1] = (float)ca_h;
								cb->convert_srgb = 0.0f;
								cb->chrome_alpha = chrome_fade_attenuation;
								cb->corner_radius = 0; cb->corner_aspect = 0;
								cb->edge_feather = 0.0f; cb->glow_intensity = 0.0f;
								CHROME_BLIT_POS(cb,
								    xg_left, xg_top, xg_left + glyph_w_m, xg_top - glyph_render_h_m,
								    bx, toy + gpad + GLYPH_Y_BIAS_PX, dst_gw, gh);
								sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
								sys->context->Draw(4, 0);
							}
						}
						// - glyph on minimize button (centered in button)
						{
							int dg = '-' - 0x20;
							float src_gw = mc->glyph_advances[dg];
							float dst_gw = src_gw * btn_scale;
							float mx = tox + tow - 2.0f * (float)CLOSE_BTN_WIDTH_PX + ((float)CLOSE_BTN_WIDTH_PX - dst_gw) / 2.0f;
							// Local: centered in minimize button
							float mg_left = pill_hw - 2*btn_w_m_val + (btn_w_m_val - glyph_w_m) / 2.0f;
							float mg_top = pill_top_m - glyph_vpad_m + GLYPH_Y_LIFT_M;
							D3D11_MAPPED_SUBRESOURCE mapped;
							if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
							              D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
								BlitConstants *cb = static_cast<BlitConstants *>(mapped.pData);
								cb->src_rect[0] = atlas_x_for(dg); cb->src_rect[1] = 0;
								cb->src_rect[2] = src_gw; cb->src_rect[3] = (float)mc->font_glyph_h;
								cb->src_size[0] = (float)mc->font_atlas_w;
								cb->src_size[1] = (float)mc->font_atlas_h;
								cb->dst_size[0] = (float)ca_w; cb->dst_size[1] = (float)ca_h;
								cb->convert_srgb = 0.0f;
								cb->chrome_alpha = chrome_fade_attenuation;
								cb->corner_radius = 0; cb->corner_aspect = 0;
								cb->edge_feather = 0.0f; cb->glow_intensity = 0.0f;
								CHROME_BLIT_POS(cb,
								    mg_left, mg_top, mg_left + glyph_w_m, mg_top - glyph_render_h_m,
								    mx, toy + gpad + GLYPH_Y_BIAS_PX, dst_gw, gh);
								sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
								sys->context->Draw(4, 0);
							}
						}
						// Hollow rounded-square icon on maximize button
						{
							bool max_hover_ic = (mc->hover_btn == 3 && mc->hover_btn_slot == s);
							float max_x_ic = tox + tow - 3.0f * (float)CLOSE_BTN_WIDTH_PX;
							float pad_x = btn_w_m_val * 0.22f;
							float pad_y = tb_h_m    * 0.22f;
							float icon_l = pill_hw - 3*btn_w_m_val + pad_x;
							float icon_r = pill_hw - 2*btn_w_m_val - pad_x;
							float icon_t = pill_top_m              - pad_y;
							float icon_b = pill_bot_m              + pad_y;
							float icon_w = icon_r - icon_l;
							float icon_h = icon_t - icon_b;
							float stroke_x = icon_w * 0.18f;
							float stroke_y = icon_h * 0.18f;

							float pad_x_px = (float)CLOSE_BTN_WIDTH_PX * 0.22f;
							float pad_y_px = toh * 0.22f;
							float ipx_l = max_x_ic + pad_x_px;
							float ipx_t = toy   + pad_y_px;
							float ipx_w = (float)CLOSE_BTN_WIDTH_PX - 2*pad_x_px;
							float ipx_h = toh - 2*pad_y_px;
							float st_px = ipx_w * 0.18f;
							float st_py = ipx_h * 0.18f;

							// Outer: white rounded rect
							D3D11_MAPPED_SUBRESOURCE mapped;
							if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
							              D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
								BlitConstants *cb = static_cast<BlitConstants *>(mapped.pData);
								cb->src_rect[0] = 0.90f; cb->src_rect[1] = 0.90f;
								cb->src_rect[2] = 0.90f; cb->src_rect[3] = 1.0f;
								cb->src_size[0] = 1; cb->src_size[1] = 1;
								cb->dst_size[0] = (float)ca_w; cb->dst_size[1] = (float)ca_h;
								cb->convert_srgb = 2.0f;
								cb->chrome_alpha = chrome_fade_attenuation; // 8.C: per-slot pill fade
								cb->corner_radius = 0.28f;
								cb->corner_aspect = icon_w / icon_h;
								cb->edge_feather = 0.0f; cb->glow_intensity = 0.0f;
								CHROME_BLIT_POS(cb, icon_l, icon_t, icon_r, icon_b,
								    ipx_l, ipx_t, ipx_w, ipx_h);
								sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
								sys->context->Draw(4, 0);
							}
							// Inner: button bg color — punches out the center to make it hollow
							if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
							              D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
								BlitConstants *cb = static_cast<BlitConstants *>(mapped.pData);
								cb->src_rect[0] = max_hover_ic ? 0.30f : 0.20f;
								cb->src_rect[1] = max_hover_ic ? 0.50f : 0.35f;
								cb->src_rect[2] = max_hover_ic ? 0.85f : 0.70f;
								cb->src_rect[3] = 1.0f;
								cb->src_size[0] = 1; cb->src_size[1] = 1;
								cb->dst_size[0] = (float)ca_w; cb->dst_size[1] = (float)ca_h;
								cb->convert_srgb = 2.0f;
								cb->chrome_alpha = chrome_fade_attenuation; // 8.C: per-slot pill fade
								cb->corner_radius = 0.20f;
								float iw2 = icon_w - 2*stroke_x;
								float ih2 = icon_h - 2*stroke_y;
								cb->corner_aspect = (ih2 > 0) ? iw2/ih2 : 1.0f;
								cb->edge_feather = 0.0f; cb->glow_intensity = 0.0f;
								CHROME_BLIT_POS(cb,
								    icon_l+stroke_x, icon_t-stroke_y, icon_r-stroke_x, icon_b+stroke_y,
								    ipx_l+st_px, ipx_t+st_py, ipx_w-2*st_px, ipx_h-2*st_py);
								sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
								sys->context->Draw(4, 0);
							}
						}
						sys->context->OMSetBlendState(sys->blend_alpha.get(), nullptr, 0xFFFFFFFF);
						sys->context->PSSetSamplers(0, 1, sys->sampler_linear.addressof());
					}
					#undef CHROME_BLIT_POS
				}
			}
		}

		// Phase 2.C: controller-submitted chrome composite. Reads the chrome
		// swapchain's image[0] SRV (single-image swapchain) and blits it at
		// the controller-specified pose in window-local meters, biased toward
		// the eye by chrome_depth_bias_m so it occludes the window's own
		// content while still depth-testing against other windows.
		//
		// C5 will gate the in-runtime chrome render block on
		// `chrome_xsc == nullptr` so the two paths don't double-render — for
		// C2 they coexist additively.
		if (mc->clients[s].chrome_xsc != nullptr && mc->clients[s].chrome_layout_valid &&
		    mc->clients[s].client_type != CLIENT_TYPE_CAPTURE) {
			struct d3d11_multi_client_slot *cs = &mc->clients[s];
			struct d3d11_service_swapchain *csc = d3d11_service_swapchain_from_xrt(cs->chrome_xsc);
			if (csc != nullptr && csc->image_count > 0 && csc->images[0].srv) {
				ID3D11ShaderResourceView *chrome_srv = csc->images[0].srv.get();

				float chrome_cx = cs->chrome_pose_in_client.position.x;
				float chrome_cy = cs->chrome_pose_in_client.position.y;
				float chrome_hw = cs->chrome_size_w_m * 0.5f;
				float chrome_hh = cs->chrome_size_h_m * 0.5f;
				float chrome_l = chrome_cx - chrome_hw;
				float chrome_r = chrome_cx + chrome_hw;
				float chrome_t = chrome_cy + chrome_hh;
				float chrome_b = chrome_cy - chrome_hh;
				float depth_bias = (cs->chrome_depth_bias_m > 0.0f) ?
				    cs->chrome_depth_bias_m : WORKSPACE_CHROME_DEPTH_BIAS;

				const struct xrt_quat *chrome_orient = &cs->window_pose.orientation;
				static const struct xrt_quat identity_quat = {0, 0, 0, 1};
				if (!cs->chrome_follows_orient) {
					chrome_orient = &identity_quat;
				}
				float wcx = cs->window_pose.position.x;
				float wcy = cs->window_pose.position.y;
				float wcz = cs->window_pose.position.z;

				D3D11_TEXTURE2D_DESC chrome_desc = {};
				csc->images[0].texture->GetDesc(&chrome_desc);

				for (uint32_t v3 = 0; v3 < num_views && v3 < XRT_MAX_VIEWS; v3++) {
					uint32_t col3 = v3 % sys->tile_columns;
					uint32_t row3 = v3 / sys->tile_columns;
					int eye_idx3 = (col3 < 2) ? (int)col3 : 0;
					int ei3 = (eye_idx3 < (int)eye_pos.count) ? eye_idx3 : 0;
					float cur_eye_x = eye_pos.eyes[ei3].x;
					float cur_eye_y = eye_pos.eyes[ei3].y;
					float cur_eye_z = eye_pos.eyes[ei3].z;

					float cc_corners[8], cc_w[4];
					project_local_rect_for_eye(sys, chrome_orient,
					    wcx, wcy, wcz,
					    chrome_l, chrome_t, chrome_r, chrome_b,
					    cur_eye_x, cur_eye_y, cur_eye_z,
					    col3, row3, half_w, half_h, ca_w, ca_h,
					    cc_corners, cc_w);

					D3D11_RECT cc_scissor;
					cc_scissor.left = (LONG)(col3 * half_w);
					cc_scissor.top = (LONG)(row3 * half_h);
					cc_scissor.right = (LONG)((col3 + 1) * half_w);
					cc_scissor.bottom = (LONG)((row3 + 1) * half_h);
					sys->context->RSSetScissorRects(1, &cc_scissor);

					sys->context->VSSetShader(sys->blit_vs.get(), nullptr, 0);
					sys->context->PSSetShader(sys->blit_ps.get(), nullptr, 0);
					sys->context->VSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
					sys->context->PSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
					ID3D11RenderTargetView *cc_rtvs[] = {mc->combined_atlas_rtv.get()};
					sys->context->OMSetRenderTargets(1, cc_rtvs, mc->combined_atlas_dsv.get());
					sys->context->OMSetBlendState(sys->blend_alpha.get(), nullptr, 0xFFFFFFFF);
					sys->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
					sys->context->IASetInputLayout(nullptr);
					sys->context->RSSetState(sys->rasterizer_state.get());
					sys->context->OMSetDepthStencilState(sys->depth_test_enabled.get(), 0);
					D3D11_VIEWPORT cc_vp = {};
					cc_vp.Width = (float)ca_w; cc_vp.Height = (float)ca_h; cc_vp.MaxDepth = 1.0f;
					sys->context->RSSetViewports(1, &cc_vp);

					D3D11_MAPPED_SUBRESOURCE mapped;
					if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
					              D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
						BlitConstants *cb = static_cast<BlitConstants *>(mapped.pData);
						memset(cb, 0, sizeof(*cb));
						cb->src_rect[0] = 0.0f; cb->src_rect[1] = 0.0f;
						cb->src_rect[2] = 1.0f; cb->src_rect[3] = 1.0f;
						cb->src_size[0] = (uint32_t)chrome_desc.Width;
						cb->src_size[1] = (uint32_t)chrome_desc.Height;
						cb->dst_size[0] = (float)ca_w; cb->dst_size[1] = (float)ca_h;
						cb->convert_srgb = 0.0f;
						cb->chrome_alpha = 0.0f;
						cb->corner_radius = 0.0f;
						cb->corner_aspect = 0.0f;
						cb->edge_feather = 0.0f;
						cb->glow_intensity = 0.0f;
						blit_set_quad_corners(cb, cc_corners, cc_w);
						cb->dst_offset[0] = 0; cb->dst_offset[1] = 0;
						cb->dst_rect_wh[0] = 0; cb->dst_rect_wh[1] = 0;
						blit_set_perspective_depth(cb, cc_w, depth_bias);
						sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
					}

					sys->context->PSSetShaderResources(0, 1, &chrome_srv);
					sys->context->PSSetSamplers(0, 1, sys->sampler_linear.addressof());
					sys->context->Draw(4, 0);
				}
			}
		}

		// (Focus glow is now drawn before content blit, inside the per-view loop above)
	}

	// (Title bar + focus border rendering is inside the render_order loop for correct z-ordering)

	// Reset scissor to full atlas for non-tiled draws (taskbar, DP processing)
	{
		uint32_t full_w = sys->base.info.display_pixel_width;
		uint32_t full_h = sys->base.info.display_pixel_height;
		if (full_w == 0) full_w = 3840;
		if (full_h == 0) full_h = 2160;
		D3D11_RECT full_scissor = {0, 0, (LONG)full_w, (LONG)full_h};
		sys->context->RSSetScissorRects(1, &full_scissor);
	}

	// Draw taskbar at bottom if any windows are user-minimized (not fullscreen-hidden)
	{
		bool has_minimized = false;
		for (int i = 0; i < D3D11_MULTI_MAX_CLIENTS; i++) {
			if (mc->clients[i].active && mc->clients[i].minimized &&
			    !mc->clients[i].fullscreen_minimized) {
				has_minimized = true;
				break;
			}
		}
		if (has_minimized && sys->blit_vs && sys->blit_ps) {
			uint32_t ca_w = sys->base.info.display_pixel_width;
			uint32_t ca_h = sys->base.info.display_pixel_height;
			if (ca_w == 0) ca_w = 3840;
			if (ca_h == 0) ca_h = 2160;
			uint32_t half_w, half_h;
			resolve_active_view_dims(sys, ca_w, ca_h, &half_w, &half_h);

			float tb_y = (float)(ca_h - TASKBAR_HEIGHT_PX);

			// Pipeline setup
			sys->context->VSSetShader(sys->blit_vs.get(), nullptr, 0);
			sys->context->PSSetShader(sys->blit_ps.get(), nullptr, 0);
			sys->context->VSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
			sys->context->PSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
			ID3D11RenderTargetView *ca_rtvs[] = {mc->combined_atlas_rtv.get()};
			sys->context->OMSetRenderTargets(1, ca_rtvs, nullptr);
			sys->context->OMSetBlendState(sys->blend_opaque.get(), nullptr, 0xFFFFFFFF);
			sys->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
			sys->context->IASetInputLayout(nullptr);
			sys->context->RSSetState(sys->rasterizer_state.get());
			sys->context->OMSetDepthStencilState(sys->depth_disabled.get(), 0);

			D3D11_VIEWPORT vp = {};
			vp.Width = (float)ca_w;
			vp.Height = (float)ca_h;
			vp.MaxDepth = 1.0f;
			sys->context->RSSetViewports(1, &vp);

			// Draw taskbar background in each SBS half
			uint32_t num_views = sys->tile_columns * sys->tile_rows;
			for (uint32_t v = 0; v < num_views && v < XRT_MAX_VIEWS; v++) {
				uint32_t col = v % sys->tile_columns;
				uint32_t row = v / sys->tile_columns;
				float bar_x = (float)(col * half_w);
				float bar_y = (float)(row * half_h) + (float)half_h - TASKBAR_HEIGHT_PX;

				D3D11_MAPPED_SUBRESOURCE mapped;
				if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
				              D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
					BlitConstants *cb = static_cast<BlitConstants *>(mapped.pData);
					cb->src_rect[0] = 0.12f; cb->src_rect[1] = 0.12f;
					cb->src_rect[2] = 0.14f; cb->src_rect[3] = 1.0f;
					cb->dst_offset[0] = bar_x;
					cb->dst_offset[1] = bar_y;
					cb->src_size[0] = 1; cb->src_size[1] = 1;
					cb->dst_size[0] = (float)ca_w;
					cb->dst_size[1] = (float)ca_h;
					cb->convert_srgb = 2.0f;
					cb->chrome_alpha = 0.0f; // 8.C: 0=full opacity (chrome blits override)
					cb->quad_mode = 0;
					cb->dst_rect_wh[0] = (float)half_w;
					cb->dst_rect_wh[1] = (float)TASKBAR_HEIGHT_PX;
					cb->corner_radius = 0; cb->corner_aspect = 0;
					cb->edge_feather = 0.0f; cb->glow_intensity = 0.0f;
					sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
					sys->context->Draw(4, 0);
				}

				// Draw indicators for each minimized app
				if (mc->font_atlas_srv) {
					sys->context->OMSetBlendState(sys->blend_alpha.get(), nullptr, 0xFFFFFFFF);
					ID3D11ShaderResourceView *font_srv = mc->font_atlas_srv.get();
					sys->context->PSSetShaderResources(0, 1, &font_srv);
					sys->context->PSSetSamplers(0, 1, sys->sampler_linear.addressof());

					int ind_idx = 0;
					for (int i = 0; i < D3D11_MULTI_MAX_CLIENTS; i++) {
						if (!mc->clients[i].active || !mc->clients[i].minimized) continue;

						// Draw indicator: colored bg + first letter
						float tgw = (float)GLYPH_W;
						float tgh = (float)GLYPH_H;
						float pill_w = 6 * tgw + 4.0f;
						float pill_h = tgh;
						float ind_x = bar_x + 6.0f + ind_idx * (pill_w + 8.0f);
						float ind_y = bar_y + ((float)TASKBAR_HEIGHT_PX - pill_h) / 2.0f;

						// Background pill (solid color)
						{
							sys->context->OMSetBlendState(sys->blend_opaque.get(), nullptr, 0xFFFFFFFF);
							D3D11_MAPPED_SUBRESOURCE mapped2;
							if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
							              D3D11_MAP_WRITE_DISCARD, 0, &mapped2))) {
								BlitConstants *cb2 = static_cast<BlitConstants *>(mapped2.pData);
								cb2->src_rect[0] = 0.25f; cb2->src_rect[1] = 0.30f;
								cb2->src_rect[2] = 0.40f; cb2->src_rect[3] = 1.0f;
								cb2->dst_offset[0] = ind_x;
								cb2->dst_offset[1] = ind_y;
								cb2->src_size[0] = 1; cb2->src_size[1] = 1;
								cb2->dst_size[0] = (float)ca_w;
								cb2->dst_size[1] = (float)ca_h;
								cb2->convert_srgb = 2.0f;
								cb2->chrome_alpha = 0.0f; // 8.C: 0=full opacity (chrome blits override)
								cb2->quad_mode = 0;
								cb2->dst_rect_wh[0] = pill_w;
								cb2->dst_rect_wh[1] = pill_h;
								cb2->corner_radius = 0; cb2->corner_aspect = 0;
								cb2->edge_feather = 0.0f; cb2->glow_intensity = 0.0f;
								sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
								sys->context->Draw(4, 0);
							}
							sys->context->OMSetBlendState(sys->blend_alpha.get(), nullptr, 0xFFFFFFFF);
						}

						// Draw first few chars of app name
						const char *name = mc->clients[i].app_name;
						int max_chars = 6;
						float tb_scale = tgh / (float)mc->font_glyph_h;
						float tb_px_cursor = 0;
						for (int ci = 0; ci < max_chars && name[ci] != '\0'; ci++) {
							unsigned char ch = (unsigned char)name[ci];
							if (ch < 0x20 || ch > 0x7E) ch = '?';
							int glyph_idx = ch - 0x20;
							float src_gw = mc->glyph_advances[glyph_idx];
							float src_x = 0;
							for (int p = 0; p < glyph_idx; p++) src_x += mc->glyph_advances[p];
							float dst_gw = src_gw * tb_scale;
							D3D11_MAPPED_SUBRESOURCE mapped3;
							if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
							              D3D11_MAP_WRITE_DISCARD, 0, &mapped3))) {
								BlitConstants *cb3 = static_cast<BlitConstants *>(mapped3.pData);
								cb3->src_rect[0] = src_x;
								cb3->src_rect[1] = 0;
								cb3->src_rect[2] = src_gw;
								cb3->src_rect[3] = (float)mc->font_glyph_h;
								cb3->dst_offset[0] = ind_x + 2.0f + tb_px_cursor;
								cb3->dst_offset[1] = ind_y;
								cb3->src_size[0] = (float)mc->font_atlas_w;
								cb3->src_size[1] = (float)mc->font_atlas_h;
								cb3->dst_size[0] = (float)ca_w;
								cb3->dst_size[1] = (float)ca_h;
								cb3->convert_srgb = 0.0f;
								cb3->chrome_alpha = 0.0f; // 8.C: 0=full opacity (chrome blits override)
								cb3->quad_mode = 0;
								cb3->dst_rect_wh[0] = dst_gw;
								cb3->dst_rect_wh[1] = tgh;
								cb3->corner_radius = 0; cb3->corner_aspect = 0;
								cb3->edge_feather = 0.0f; cb3->glow_intensity = 0.0f;
								sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
								sys->context->Draw(4, 0);
							}
							tb_px_cursor += dst_gw;
						}
						ind_idx++;
					}

					sys->context->OMSetBlendState(sys->blend_opaque.get(), nullptr, 0xFFFFFFFF);
					sys->context->PSSetSamplers(0, 1, sys->sampler_linear.addressof());
				}
			}
		}
	}

	// Toast notification overlay (momentary centered message).
	if (mc->toast_until_ns > 0 && mc->font_atlas_srv && sys->blit_vs && sys->blit_ps) {
		uint64_t now_ns = os_monotonic_get_ns();
		if (now_ns < mc->toast_until_ns) {
			uint32_t ca_w = sys->base.info.display_pixel_width;
			uint32_t ca_h = sys->base.info.display_pixel_height;
			if (ca_w == 0) ca_w = 3840;
			if (ca_h == 0) ca_h = 2160;
			uint32_t half_w, half_h;
			resolve_active_view_dims(sys, ca_w, ca_h, &half_w, &half_h);
			uint32_t num_views = sys->tile_columns * sys->tile_rows;

			const float scale = 2.0f; // 2x glyph size for toast
			float gh = (float)mc->font_glyph_h * scale;
			float pad_px = 12.0f;
			float toast_h = gh + pad_px * 2.0f;

			// Measure total text width
			const char *txt = mc->toast_text;
			float total_w = 0;
			for (int ci = 0; txt[ci] != '\0' && ci < 80; ci++) {
				unsigned char ch = (unsigned char)txt[ci];
				if (ch < 0x20 || ch > 0x7E) ch = '?';
				total_w += mc->glyph_advances[ch - 0x20] * scale;
			}
			float toast_w = total_w + pad_px * 2.0f;

			// Pipeline setup
			sys->context->VSSetShader(sys->blit_vs.get(), nullptr, 0);
			sys->context->PSSetShader(sys->blit_ps.get(), nullptr, 0);
			sys->context->VSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
			sys->context->PSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
			ID3D11RenderTargetView *t_rtvs[] = {mc->combined_atlas_rtv.get()};
			sys->context->OMSetRenderTargets(1, t_rtvs, nullptr);
			sys->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
			sys->context->IASetInputLayout(nullptr);
			sys->context->RSSetState(sys->rasterizer_state.get());
			sys->context->OMSetDepthStencilState(sys->depth_disabled.get(), 0);
			D3D11_VIEWPORT t_vp = {};
			t_vp.Width = (float)ca_w; t_vp.Height = (float)ca_h; t_vp.MaxDepth = 1.0f;
			sys->context->RSSetViewports(1, &t_vp);

			for (uint32_t v = 0; v < num_views && v < XRT_MAX_VIEWS; v++) {
				uint32_t col = v % sys->tile_columns;
				uint32_t row = v / sys->tile_columns;
				float vx = (float)(col * half_w);
				float vy = (float)(row * half_h);

				float bg_x = vx + ((float)half_w - toast_w) * 0.5f;
				float bg_y = vy + ((float)half_h - toast_h) * 0.5f;

				// Semi-transparent dark background
				sys->context->OMSetBlendState(sys->blend_alpha.get(), nullptr, 0xFFFFFFFF);
				D3D11_MAPPED_SUBRESOURCE mapped;
				if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
				              D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
					BlitConstants *cb = static_cast<BlitConstants *>(mapped.pData);
					cb->src_rect[0] = 0.08f; cb->src_rect[1] = 0.08f;
					cb->src_rect[2] = 0.10f; cb->src_rect[3] = 0.82f;
					cb->src_size[0] = 1; cb->src_size[1] = 1;
					cb->dst_size[0] = (float)ca_w; cb->dst_size[1] = (float)ca_h;
					cb->convert_srgb = 2.0f;
					cb->chrome_alpha = 0.0f; // 8.C: 0=full opacity (chrome blits override)
					cb->quad_mode = 0;
					cb->dst_offset[0] = bg_x; cb->dst_offset[1] = bg_y;
					cb->dst_rect_wh[0] = toast_w; cb->dst_rect_wh[1] = toast_h;
					cb->corner_radius = 0.3f;
					cb->corner_aspect = toast_w / toast_h;
					cb->edge_feather = 0.0f; cb->glow_intensity = 0.0f;
					sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
					sys->context->Draw(4, 0);
				}

				// Text glyphs
				ID3D11ShaderResourceView *font_srv = mc->font_atlas_srv.get();
				sys->context->PSSetShaderResources(0, 1, &font_srv);
				sys->context->PSSetSamplers(0, 1, sys->sampler_linear.addressof());

				float cursor_x = bg_x + pad_px;
				float text_y = bg_y + pad_px;
				for (int ci = 0; txt[ci] != '\0' && ci < 80; ci++) {
					unsigned char ch = (unsigned char)txt[ci];
					if (ch < 0x20 || ch > 0x7E) ch = '?';
					int gi = ch - 0x20;
					float src_x = 0;
					for (int p = 0; p < gi; p++) src_x += mc->glyph_advances[p];
					float src_gw = mc->glyph_advances[gi];
					float dst_gw = src_gw * scale;
					if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
					              D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
						BlitConstants *cb = static_cast<BlitConstants *>(mapped.pData);
						cb->src_rect[0] = src_x; cb->src_rect[1] = 0;
						cb->src_rect[2] = src_gw; cb->src_rect[3] = (float)mc->font_glyph_h;
						cb->src_size[0] = (float)mc->font_atlas_w;
						cb->src_size[1] = (float)mc->font_atlas_h;
						cb->dst_size[0] = (float)ca_w; cb->dst_size[1] = (float)ca_h;
						cb->convert_srgb = 0.0f;
						cb->chrome_alpha = 0.0f; // 8.C: 0=full opacity (chrome blits override)
						cb->quad_mode = 0;
						cb->dst_offset[0] = cursor_x; cb->dst_offset[1] = text_y;
						cb->dst_rect_wh[0] = dst_gw; cb->dst_rect_wh[1] = gh;
						cb->corner_radius = 0; cb->corner_aspect = 0;
						cb->edge_feather = 0.0f; cb->glow_intensity = 0.0f;
						sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
						sys->context->Draw(4, 0);
					}
					cursor_x += dst_gw;
				}
			}
			sys->context->OMSetBlendState(sys->blend_opaque.get(), nullptr, 0xFFFFFFFF);
			sys->context->PSSetSamplers(0, 1, sys->sampler_linear.addressof());
		} else {
			mc->toast_until_ns = 0; // expired — clear
		}
	}

	// Phase 5.7 + 5.8: spatial launcher panel.
	// Drawn at (0,0,0) in display coordinates — zero-disparity plane — so it
	// appears on the physical display surface, which maximizes viewing comfort.
	// At z=0 there is no parallax, so the panel lands at the same pixel
	// position for every eye; we just center-draw into each tile.
	//
	// Layout (5.8): title bar text "DisplayXR Launcher" → "Installed" header
	// → tile grid (4-column, wraps to additional rows). Hard-coded placeholder
	// app names for now; real apps come over IPC in a follow-up task.
	if (mc->launcher_visible) {
		uint32_t ca_w = sys->base.info.display_pixel_width;
		uint32_t ca_h = sys->base.info.display_pixel_height;
		if (ca_w == 0) ca_w = 3840;
		if (ca_h == 0) ca_h = 2160;
		uint32_t half_w, half_h;
		resolve_active_view_dims(sys, ca_w, ca_h, &half_w, &half_h);
		uint32_t num_views = sys->tile_columns * sys->tile_rows;

		// Panel size as a fraction of the physical display — resolution-
		// and aspect-independent. Using fractions of display_{width,height}_m
		// instead of absolute meters ensures the panel scales correctly across
		// laptop, tablet, and desktop displays.
		float disp_w_m = sys->base.info.display_width_m;
		float disp_h_m = sys->base.info.display_height_m;
		if (disp_w_m <= 0.0f) disp_w_m = 0.700f;
		if (disp_h_m <= 0.0f) disp_h_m = 0.394f;

		const float panel_w_frac = 0.60f;
		const float panel_h_frac = 0.85f;
		float panel_w_m = disp_w_m * panel_w_frac;
		float panel_h_m = disp_h_m * panel_h_frac;

		float panel_w_px = ui_m_to_tile_px_x(panel_w_m, sys);
		float panel_h_px = ui_m_to_tile_px_y(panel_h_m, sys);

		// Tile grid layout (panel-relative). 4 columns wraps to additional
		// rows when more apps are present. All sizes derived from panel
		// dimensions so the layout scales with display size.
		const int LAUNCHER_GRID_COLS = 4;
		float margin = panel_w_px * 0.04f;
		float title_h = panel_h_px * 0.13f;
		float section_h = panel_h_px * 0.07f;
		float tile_w = (panel_w_px - (LAUNCHER_GRID_COLS + 1) * margin) /
		               (float)LAUNCHER_GRID_COLS;
		// Physically square tiles: X pixels are wider than Y pixels in
		// SBS mode (1920 pixels span 0.700m but 2160 span 0.394m).
		// Scale tile_h so each tile is the same physical width and height.
		uint32_t tpw, tph;
		resolve_active_view_dims(sys,
		                         sys->base.info.display_pixel_width,
		                         sys->base.info.display_pixel_height,
		                         &tpw, &tph);
		float pix_ratio = (tpw > 0 && tph > 0 && disp_w_m > 0 && disp_h_m > 0)
		    ? ((disp_w_m / (float)tpw) / (disp_h_m / (float)tph))
		    : 1.0f;
		float tile_h = tile_w * pix_ratio;

		// App list pushed from the workspace controller via clear+add IPC calls.
		// Stored on sys so it survives multi-comp create/destroy. Empty until
		// the controller completes its first registered_apps_load + push;
		// empty-state branch below handles that.
		const struct ipc_launcher_app *apps = sys->launcher_apps;
		int visible_to_full[IPC_LAUNCHER_MAX_APPS];
		uint32_t n_visible = launcher_build_visible_list(sys, visible_to_full);
		uint32_t n_apps = n_visible; // rest of this block treats the list as compacted

		// Phase 6.2: per-frame hover detection for mouse-over highlight.
		// Hit-test the cursor against the tile grid so the render can draw
		// a subtle highlight on the tile under the pointer.
		if (mc->hwnd != nullptr) {
			POINT cpt;
			GetCursorPos(&cpt);
			ScreenToClient(mc->hwnd, &cpt);
			sys->launcher_hover_index = launcher_hit_test(sys, cpt, n_visible);
		} else {
			sys->launcher_hover_index = -1;
		}

		// Pipeline setup once — then per-eye scissor + draw. Bind the font
		// atlas SRV for text glyph sampling; solid-color draws ignore it.
		sys->context->VSSetShader(sys->blit_vs.get(), nullptr, 0);
		sys->context->PSSetShader(sys->blit_ps.get(), nullptr, 0);
		sys->context->VSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
		sys->context->PSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
		ID3D11RenderTargetView *launcher_rtvs[] = {mc->combined_atlas_rtv.get()};
		sys->context->OMSetRenderTargets(1, launcher_rtvs, nullptr);
		sys->context->OMSetBlendState(sys->blend_alpha.get(), nullptr, 0xFFFFFFFF);
		sys->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		sys->context->IASetInputLayout(nullptr);
		sys->context->RSSetState(sys->rasterizer_state.get());
		sys->context->OMSetDepthStencilState(sys->depth_disabled.get(), 0);
		D3D11_VIEWPORT launcher_vp = {};
		launcher_vp.Width = (float)ca_w;
		launcher_vp.Height = (float)ca_h;
		launcher_vp.MaxDepth = 1.0f;
		sys->context->RSSetViewports(1, &launcher_vp);
		if (mc->font_atlas_srv) {
			ID3D11ShaderResourceView *font_srv = mc->font_atlas_srv.get();
			sys->context->PSSetShaderResources(0, 1, &font_srv);
			sys->context->PSSetSamplers(0, 1, sys->sampler_linear.addressof());
		}

		// Helper: draw a solid-color rounded rect at (dx,dy,dw,dh) in atlas
		// pixels with the given RGBA. Used for the panel background and tile
		// backgrounds. Updates the constant buffer and issues a single Draw.
		auto draw_solid_rect = [&](float dx, float dy, float dw, float dh,
		                           float r, float g, float b, float a,
		                           float corner_radius, float glow_intensity) {
			D3D11_MAPPED_SUBRESOURCE m;
			if (FAILED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
			                             D3D11_MAP_WRITE_DISCARD, 0, &m))) {
				return;
			}
			BlitConstants *cb = static_cast<BlitConstants *>(m.pData);
			cb->src_rect[0] = r;
			cb->src_rect[1] = g;
			cb->src_rect[2] = b;
			cb->src_rect[3] = a;
			cb->src_size[0] = 1;
			cb->src_size[1] = 1;
			cb->dst_size[0] = (float)ca_w;
			cb->dst_size[1] = (float)ca_h;
			cb->convert_srgb = 2.0f; // solid-color mode
			cb->chrome_alpha = 0.0f; // 8.C: 0=full opacity (chrome blits override)
			// Negative radius + negative aspect = all four corners rounded.
			cb->corner_radius = -fabsf(corner_radius);
			cb->corner_aspect = (dh > 0.0f) ? -(dw / dh) : -1.0f;
			cb->edge_feather = (dh > 0.0f) ? (2.0f / dh) : 0.0f;
			cb->glow_intensity = glow_intensity;
			cb->quad_mode = 0;
			cb->dst_offset[0] = dx;
			cb->dst_offset[1] = dy;
			cb->dst_rect_wh[0] = dw;
			cb->dst_rect_wh[1] = dh;
			memset(cb->quad_corners_01, 0, sizeof(cb->quad_corners_01));
			memset(cb->quad_corners_23, 0, sizeof(cb->quad_corners_23));
			sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
			sys->context->Draw(4, 0);
		};

		// Helper: width of a string in atlas pixels at the given scale.
		auto measure_text = [&](const char *text, float scale) -> float {
			float w = 0.0f;
			for (const char *p = text; *p != '\0'; p++) {
				unsigned char ch = (unsigned char)*p;
				if (ch < 0x20 || ch > 0x7E) ch = '?';
				int gi = ch - 0x20;
				w += mc->glyph_advances[gi] * scale;
			}
			return w;
		};

		// Helper: draw a label centered horizontally within a (tx, tile_w)
		// box, ellipsis-truncated if it doesn't fit. Used for tile labels
		// where long sidecar names like "Cube D3D11 (Handle)" would
		// otherwise overflow into adjacent tiles.
		auto draw_label_centered = [&](const char *text, float tx, float ty,
		                               float box_w, float scale) {
			float full_w = 0.0f;
			for (const char *p = text; *p != '\0'; p++) {
				unsigned char ch = (unsigned char)*p;
				if (ch < 0x20 || ch > 0x7E) ch = '?';
				full_w += mc->glyph_advances[ch - 0x20] * scale;
			}
			if (full_w <= box_w) {
				float cx = tx + (box_w - full_w) * 0.5f;
				// Inline draw to avoid the lambda forward-decl problem.
				float cursor = 0.0f;
				float dst_gh = (float)mc->font_glyph_h * scale;
				for (const char *p = text; *p != '\0'; p++) {
					unsigned char ch = (unsigned char)*p;
					if (ch < 0x20 || ch > 0x7E) ch = '?';
					int gi = ch - 0x20;
					float src_gw = mc->glyph_advances[gi];
					float src_x = 0.0f;
					for (int j = 0; j < gi; j++) src_x += mc->glyph_advances[j];
					float dst_gw = src_gw * scale;
					D3D11_MAPPED_SUBRESOURCE mm;
					if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
					                                D3D11_MAP_WRITE_DISCARD, 0, &mm))) {
						BlitConstants *cb = static_cast<BlitConstants *>(mm.pData);
						cb->src_rect[0] = src_x; cb->src_rect[1] = 0.0f;
						cb->src_rect[2] = src_gw; cb->src_rect[3] = (float)mc->font_glyph_h;
						cb->src_size[0] = (float)mc->font_atlas_w;
						cb->src_size[1] = (float)mc->font_atlas_h;
						cb->dst_size[0] = (float)ca_w; cb->dst_size[1] = (float)ca_h;
						cb->convert_srgb = 0.0f;
						cb->chrome_alpha = 0.0f; // 8.C: 0=full opacity (chrome blits override)
						cb->corner_radius = 0.0f; cb->corner_aspect = 0.0f;
						cb->edge_feather = 0.0f; cb->glow_intensity = 0.0f;
						cb->quad_mode = 0;
						cb->dst_offset[0] = cx + cursor; cb->dst_offset[1] = ty;
						cb->dst_rect_wh[0] = dst_gw; cb->dst_rect_wh[1] = dst_gh;
						memset(cb->quad_corners_01, 0, sizeof(cb->quad_corners_01));
						memset(cb->quad_corners_23, 0, sizeof(cb->quad_corners_23));
						sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
						sys->context->Draw(4, 0);
					}
					cursor += dst_gw;
				}
				return;
			}

			// Doesn't fit — build a truncated version with trailing "..".
			// Greedy: walk chars while (width + ".."_width) <= box_w.
			float dot_w = mc->glyph_advances['.' - 0x20] * scale;
			float ellipsis_w = dot_w * 2.0f;
			char buf[160];
			int n = 0;
			float w = 0.0f;
			for (const char *p = text; *p != '\0' && n < (int)sizeof(buf) - 3; p++) {
				unsigned char ch = (unsigned char)*p;
				if (ch < 0x20 || ch > 0x7E) ch = '?';
				float gw = mc->glyph_advances[ch - 0x20] * scale;
				if (w + gw + ellipsis_w > box_w) break;
				buf[n++] = (char)ch;
				w += gw;
			}
			buf[n++] = '.'; buf[n++] = '.';
			buf[n] = '\0';
			float total_w = w + ellipsis_w;
			float cx = tx + (box_w - total_w) * 0.5f;
			if (cx < tx) cx = tx;

			// Draw the truncated buffer (same inline glyph loop).
			float cursor = 0.0f;
			float dst_gh = (float)mc->font_glyph_h * scale;
			for (int i = 0; i < n; i++) {
				int gi = (unsigned char)buf[i] - 0x20;
				float src_gw = mc->glyph_advances[gi];
				float src_x = 0.0f;
				for (int j = 0; j < gi; j++) src_x += mc->glyph_advances[j];
				float dst_gw = src_gw * scale;
				D3D11_MAPPED_SUBRESOURCE mm;
				if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
				                                D3D11_MAP_WRITE_DISCARD, 0, &mm))) {
					BlitConstants *cb = static_cast<BlitConstants *>(mm.pData);
					cb->src_rect[0] = src_x; cb->src_rect[1] = 0.0f;
					cb->src_rect[2] = src_gw; cb->src_rect[3] = (float)mc->font_glyph_h;
					cb->src_size[0] = (float)mc->font_atlas_w;
					cb->src_size[1] = (float)mc->font_atlas_h;
					cb->dst_size[0] = (float)ca_w; cb->dst_size[1] = (float)ca_h;
					cb->convert_srgb = 0.0f;
					cb->chrome_alpha = 0.0f; // 8.C: 0=full opacity (chrome blits override)
					cb->corner_radius = 0.0f; cb->corner_aspect = 0.0f;
					cb->edge_feather = 0.0f; cb->glow_intensity = 0.0f;
					cb->quad_mode = 0;
					cb->dst_offset[0] = cx + cursor; cb->dst_offset[1] = ty;
					cb->dst_rect_wh[0] = dst_gw; cb->dst_rect_wh[1] = dst_gh;
					memset(cb->quad_corners_01, 0, sizeof(cb->quad_corners_01));
					memset(cb->quad_corners_23, 0, sizeof(cb->quad_corners_23));
					sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
					sys->context->Draw(4, 0);
				}
				cursor += dst_gw;
			}
		};

		// Helper: draw a string starting at (dx, dy) (top-left, atlas px) at
		// the given scale, sampling the font atlas. One Draw call per glyph.
		auto draw_text = [&](const char *text, float dx, float dy, float scale) {
			float cursor = 0.0f;
			float dst_gh = (float)mc->font_glyph_h * scale;
			for (const char *p = text; *p != '\0'; p++) {
				unsigned char ch = (unsigned char)*p;
				if (ch < 0x20 || ch > 0x7E) ch = '?';
				int gi = ch - 0x20;
				float src_gw = mc->glyph_advances[gi];
				float src_x = 0.0f;
				for (int i = 0; i < gi; i++) {
					src_x += mc->glyph_advances[i];
				}
				float dst_gw = src_gw * scale;

				D3D11_MAPPED_SUBRESOURCE m;
				if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
				                                D3D11_MAP_WRITE_DISCARD, 0, &m))) {
					BlitConstants *cb = static_cast<BlitConstants *>(m.pData);
					cb->src_rect[0] = src_x;
					cb->src_rect[1] = 0.0f;
					cb->src_rect[2] = src_gw;
					cb->src_rect[3] = (float)mc->font_glyph_h;
					cb->src_size[0] = (float)mc->font_atlas_w;
					cb->src_size[1] = (float)mc->font_atlas_h;
					cb->dst_size[0] = (float)ca_w;
					cb->dst_size[1] = (float)ca_h;
					cb->convert_srgb = 0.0f; // textured (font atlas is linear)
					cb->chrome_alpha = 0.0f; // 8.C: 0=full opacity (chrome blits override)
					cb->corner_radius = 0.0f;
					cb->corner_aspect = 0.0f;
					cb->edge_feather = 0.0f;
					cb->glow_intensity = 0.0f;
					cb->quad_mode = 0;
					cb->dst_offset[0] = dx + cursor;
					cb->dst_offset[1] = dy;
					cb->dst_rect_wh[0] = dst_gw;
					cb->dst_rect_wh[1] = dst_gh;
					memset(cb->quad_corners_01, 0, sizeof(cb->quad_corners_01));
					memset(cb->quad_corners_23, 0, sizeof(cb->quad_corners_23));
					sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
					sys->context->Draw(4, 0);
				}
				cursor += dst_gw;
			}
		};

		for (uint32_t v = 0; v < num_views && v < XRT_MAX_VIEWS; v++) {
			uint32_t col = v % sys->tile_columns;
			uint32_t row = v / sys->tile_columns;

			// Scissor clips to this tile's bounds so the solid quad cannot
			// bleed into neighbouring eyes.
			D3D11_RECT scissor;
			scissor.left = (LONG)(col * half_w);
			scissor.top = (LONG)(row * half_h);
			scissor.right = (LONG)((col + 1) * half_w);
			scissor.bottom = (LONG)((row + 1) * half_h);
			sys->context->RSSetScissorRects(1, &scissor);

			// Tile-local center — z=0 means no parallax, identical for both eyes.
			float tile_cx = (float)(col * half_w) + (float)half_w * 0.5f;
			float tile_cy = (float)(row * half_h) + (float)half_h * 0.5f;
			float panel_x = tile_cx - panel_w_px * 0.5f;
			float panel_y = tile_cy - panel_h_px * 0.5f;

			// 1) Panel background — dark slate with ~92% alpha.
			draw_solid_rect(panel_x, panel_y, panel_w_px, panel_h_px,
			                0.08f, 0.10f, 0.14f, 0.92f, 0.08f, 0.0f);

			// 2) Title bar text "DisplayXR Launcher", centered horizontally
			// in the title strip.
			const char *title_text = "DisplayXR Launcher";
			float title_scale = (title_h * 0.55f) / (float)mc->font_glyph_h;
			float title_w = measure_text(title_text, title_scale);
			float title_x = panel_x + (panel_w_px - title_w) * 0.5f;
			float title_y = panel_y + (title_h - mc->font_glyph_h * title_scale) * 0.5f;
			draw_text(title_text, title_x, title_y, title_scale);

			// 3) "Installed" section header, left-aligned with grid margin.
			const char *installed_text = "Installed";
			float section_scale = (section_h * 0.65f) / (float)mc->font_glyph_h;
			float section_x = panel_x + margin;
			float section_y = panel_y + title_h + margin * 0.5f;
			draw_text(installed_text, section_x, section_y, section_scale);

			// 4) Tile grid — 4-column wrapping. Each row also reserves space
			// below the tile for its label. If the registry is empty (e.g.
			// scanner found no sidecars and workspace hasn't pushed yet), show
			// an empty-state hint instead of a blank grid.
			float label_scale = section_scale * 0.425f;
			float label_h = (float)mc->font_glyph_h * label_scale;
			float grid_top = section_y + (float)mc->font_glyph_h * section_scale + margin * 0.5f;

			// Phase 6.5: apply scroll offset to grid_top. Each scroll
			// row shifts the grid up by one row height in pixels.
			float row_h_px = tile_h + label_h + margin;
			float scroll_offset_px = (float)sys->launcher_scroll_row * row_h_px;
			grid_top -= scroll_offset_px;

			// Visible grid bounds (panel-local) for culling off-screen tiles.
			float grid_visible_top = section_y + (float)mc->font_glyph_h * section_scale + margin * 0.5f;
			float grid_visible_bottom = panel_y + panel_h_px - margin;

			// Render real tiles (compacted via visible_to_full) + the virtual
			// "Add app…" Browse tile at position n_visible. When there are no
			// real tiles we still draw the Browse tile plus the empty-state
			// hint above it so the launcher is never completely blank.
			if (n_apps == 0) {
				const char *empty_line1 = "No apps discovered";
				const char *empty_line2 = "Add a .displayxr.json sidecar next to your exe, or use the tile below.";
				float empty_scale_1 = section_scale * 0.95f;
				float empty_scale_2 = section_scale * 0.60f;
				float w1 = measure_text(empty_line1, empty_scale_1);
				float w2 = measure_text(empty_line2, empty_scale_2);
				float ex1 = panel_x + (panel_w_px - w1) * 0.5f;
				float ex2 = panel_x + (panel_w_px - w2) * 0.5f;
				float ey1 = grid_top + tile_h * 0.15f;
				float ey2 = ey1 + (float)mc->font_glyph_h * empty_scale_1 + margin * 0.4f;
				draw_text(empty_line1, ex1, ey1, empty_scale_1);
				draw_text(empty_line2, ex2, ey2, empty_scale_2);
			}

			for (uint32_t vi = 0; vi < n_apps; vi++) {
				int full_idx = visible_to_full[vi];
				int tcol = (int)(vi % LAUNCHER_GRID_COLS);
				int trow = (int)(vi / LAUNCHER_GRID_COLS);
				float tx = panel_x + margin + (float)tcol * (tile_w + margin);
				float ty = grid_top + (float)trow * (tile_h + label_h + margin);

				// Phase 6.5: cull tiles that scrolled out of the visible grid area.
				if (ty + tile_h + label_h < grid_visible_top || ty > grid_visible_bottom) {
					continue;
				}

				bool tile_running = (sys->running_tile_mask & (1ULL << full_idx)) != 0;
				bool tile_selected = (sys->launcher_selected_index == (int32_t)vi);
				bool tile_hovered = (sys->launcher_hover_index == (int32_t)vi);

				// Phase 5.11: glow border for running tiles. Draw an
				// oversized quad in glow mode (convert_srgb=3.0) so the
				// shader fades the inner rect into the surrounding margin.
				// Glow halo: follows keyboard selection only.
				if (tile_selected) {
					float glow_margin = tile_h * 0.18f;
					float gx = tx - glow_margin;
					float gy = ty - glow_margin;
					float gw = tile_w + 2.0f * glow_margin;
					float gh = tile_h + 2.0f * glow_margin;
					// Separate X/Y extents: glow_extent = X, edge_feather = Y.
					// Inset by corner radius so glow fills under rounded corners.
					float cr_inset = 0.06f * tile_h;
					float glow_ext_x = (glow_margin + cr_inset) / gw;
					float glow_ext_y = (glow_margin + cr_inset) / gh;

					D3D11_MAPPED_SUBRESOURCE gm;
					if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
					                                D3D11_MAP_WRITE_DISCARD, 0, &gm))) {
						BlitConstants *cb = static_cast<BlitConstants *>(gm.pData);
						cb->src_rect[0] = 0; cb->src_rect[1] = 0;
						cb->src_rect[2] = 1; cb->src_rect[3] = 1;
						cb->src_size[0] = 1; cb->src_size[1] = 1;
						cb->dst_size[0] = (float)ca_w;
						cb->dst_size[1] = (float)ca_h;
						cb->convert_srgb = 3.0f; // glow mode
						cb->chrome_alpha = 0.0f; // 8.C: 0=full opacity (chrome blits override)
						cb->corner_radius = 0.0f;
						cb->corner_aspect = 0.0f;
						cb->edge_feather = glow_ext_y; // Y extent (repurposed)
						cb->glow_intensity = 0.85f;
						cb->glow_extent = glow_ext_x;  // X extent
						cb->glow_falloff = 6.0f;
						cb->glow_color[0] = 0.30f;
						cb->glow_color[1] = 0.85f;
						cb->glow_color[2] = 1.00f;
						cb->glow_color[3] = 1.0f;
						cb->quad_mode = 0;
						cb->dst_offset[0] = gx;
						cb->dst_offset[1] = gy;
						cb->dst_rect_wh[0] = gw;
						cb->dst_rect_wh[1] = gh;
						memset(cb->quad_corners_01, 0, sizeof(cb->quad_corners_01));
						memset(cb->quad_corners_23, 0, sizeof(cb->quad_corners_23));
						sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
						sys->context->OMSetBlendState(sys->blend_premul.get(), nullptr, 0xFFFFFFFF);
						sys->context->Draw(4, 0);
						sys->context->OMSetBlendState(sys->blend_alpha.get(), nullptr, 0xFFFFFFFF);
					}
				}

				// Tile background — slightly lighter than panel, rounded.
				// Running tiles get a brighter background; hovered tiles
				// get a subtle extra lift for mouse-over feedback.
				float bg_r = tile_running ? 0.20f : 0.16f;
				float bg_g = tile_running ? 0.27f : 0.19f;
				float bg_b = tile_running ? 0.36f : 0.26f;
				if (tile_hovered) {
					bg_r += 0.06f;
					bg_g += 0.06f;
					bg_b += 0.06f;
				}
				draw_solid_rect(tx, ty, tile_w, tile_h,
				                bg_r, bg_g, bg_b, 0.95f, 0.06f, 0.0f);

				// Phase 7.3 + 7.4: icon texture over tile background.
				{
					const auto &ic = sys->launcher_icons[full_idx];
					bool use_3d = ic.srv_3d && sys->tile_columns > 1;
					ID3D11ShaderResourceView *icon_srv = use_3d
						? ic.srv_3d.get()
						: ic.srv_2d.get();

					if (icon_srv != nullptr) {
						uint32_t tex_w = use_3d ? ic.w_3d : ic.w_2d;
						uint32_t tex_h = use_3d ? ic.h_3d : ic.h_2d;

						// Compute the sub-rect of the icon texture to sample.
						float sx = 0, sy = 0, sw = (float)tex_w, sh = (float)tex_h;
						if (use_3d) {
							const char *lay = ic.layout_3d;
							bool is_sbs = (lay[0] == 's'); // sbs-lr or sbs-rl
							bool is_lr  = is_sbs && (lay[4] == 'l');
							bool is_tb  = (lay[0] == 't'); // tb
							// col=0 → left eye, col=1 → right eye
							bool right_eye = (col == 1);
							if (is_sbs) {
								sw = (float)(tex_w / 2);
								sx = (is_lr == right_eye) ? sw : 0;
							} else {
								// tb or bt
								sh = (float)(tex_h / 2);
								sy = (is_tb == right_eye) ? sh : 0;
							}
						}

						// Icons are physically square, tiles are physically square
						// (but non-square in pixels due to SBS). Fill the tile.
						float draw_w = tile_w;
						float draw_h = tile_h;
						float draw_x = tx;
						float draw_y = ty;

						D3D11_MAPPED_SUBRESOURCE im;
						if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
						                                D3D11_MAP_WRITE_DISCARD, 0, &im))) {
							BlitConstants *cb = static_cast<BlitConstants *>(im.pData);
							cb->src_rect[0] = sx;
							cb->src_rect[1] = sy;
							cb->src_rect[2] = sw;
							cb->src_rect[3] = sh;
							cb->src_size[0] = (float)tex_w;
							cb->src_size[1] = (float)tex_h;
							cb->dst_size[0] = (float)ca_w;
							cb->dst_size[1] = (float)ca_h;
							cb->convert_srgb = 0.0f;
							cb->chrome_alpha = 0.0f; // 8.C: 0=full opacity (chrome blits override)
							// Negative radius + negative aspect = all four corners.
							cb->corner_radius = -0.06f;
							cb->corner_aspect = (draw_h > 0.0f) ? -(draw_w / draw_h) : -1.0f;
							cb->edge_feather = (draw_h > 0.0f) ? (2.0f / draw_h) : 0.0f;
							cb->glow_intensity = 0.0f;
							cb->quad_mode = 0;
							cb->dst_offset[0] = draw_x;
							cb->dst_offset[1] = draw_y;
							cb->dst_rect_wh[0] = draw_w;
							cb->dst_rect_wh[1] = draw_h;
							memset(cb->quad_corners_01, 0, sizeof(cb->quad_corners_01));
							memset(cb->quad_corners_23, 0, sizeof(cb->quad_corners_23));
							sys->context->Unmap(sys->blit_constant_buffer.get(), 0);

							sys->context->PSSetShaderResources(0, 1, &icon_srv);
							sys->context->Draw(4, 0);

							// Re-bind font atlas for subsequent text draws.
							if (mc->font_atlas_srv) {
								ID3D11ShaderResourceView *font_srv = mc->font_atlas_srv.get();
								sys->context->PSSetShaderResources(0, 1, &font_srv);
							}
						}
					}
				}

				// Label centered horizontally below tile, ellipsis-truncated
				// if it would overflow into the neighbouring tile.
				const char *label = apps[full_idx].name;
				float label_y = ty + tile_h + margin * 0.15f;
				draw_label_centered(label, tx, label_y, tile_w, label_scale);
			}

			// Phase 5.14: virtual "Add app…" Browse tile at position n_apps.
			// Distinct styling (lighter, more translucent) + "+" glyph +
			// "Add app…" label. Click here → IPC_LAUNCHER_ACTION_BROWSE.
			{
				uint32_t vi = n_apps;
				int tcol = (int)(vi % LAUNCHER_GRID_COLS);
				int trow = (int)(vi / LAUNCHER_GRID_COLS);
				float tx = panel_x + margin + (float)tcol * (tile_w + margin);
				float ty = grid_top + (float)trow * (tile_h + label_h + margin);

				// Phase 6.5: cull Browse tile if scrolled out of view.
				if (ty + tile_h + label_h >= grid_visible_top && ty <= grid_visible_bottom) {

				bool browse_selected = (sys->launcher_selected_index == (int32_t)vi);
				bool browse_hovered = (sys->launcher_hover_index == (int32_t)vi);

				if (browse_selected) {
					float sm = tile_h * 0.045f;
					draw_solid_rect(tx - sm, ty - sm,
					                tile_w + 2.0f * sm, tile_h + 2.0f * sm,
					                1.00f, 1.00f, 1.00f, 0.90f, 0.06f, 0.0f);
				}

				float br_r = 0.20f, br_g = 0.22f, br_b = 0.28f;
				if (browse_hovered) { br_r += 0.06f; br_g += 0.06f; br_b += 0.06f; }
				draw_solid_rect(tx, ty, tile_w, tile_h,
				                br_r, br_g, br_b, 0.75f, 0.06f, 0.0f);

				float plus_scale = section_scale * 1.6f;
				float plus_h = (float)mc->font_glyph_h * plus_scale;
				draw_label_centered("+", tx, ty + (tile_h - plus_h) * 0.5f,
				                    tile_w, plus_scale);

				float browse_label_y = ty + tile_h + margin * 0.15f;
				draw_label_centered("Add app...", tx, browse_label_y,
				                    tile_w, label_scale);

				} // end cull check for Browse tile
			}
		}

		// Clear scissor so downstream passes aren't affected.
		D3D11_RECT full_scissor = {0, 0, (LONG)ca_w, (LONG)ca_h};
		sys->context->RSSetScissorRects(1, &full_scissor);
	}

	// Send full combined atlas to DP — content is placed at sub-rect positions,
	// background is dark gray. The DP interlaces the entire image.
	// Non-legacy sessions use true per-view dims from the active rendering mode
	// (e.g. 1920×1080 per view in stereo SBS, not 1920×2160). Legacy sessions
	// keep the atlas-divided size so compromise-scaled submissions aren't cropped.
	// Issue #158.
	ID3D11ShaderResourceView *dp_input_srv = mc->combined_atlas_srv.get();
	{
		uint32_t aw = sys->base.info.display_pixel_width;
		uint32_t ah = sys->base.info.display_pixel_height;
		if (aw == 0) aw = sys->display_width;
		if (ah == 0) ah = sys->display_height;
		resolve_active_view_dims(sys, aw, ah, &dp_view_w, &dp_view_h);
	}
	uint32_t content_w = sys->tile_columns * dp_view_w;
	uint32_t content_h = sys->tile_rows * dp_view_h;

	// Get combined atlas actual dimensions
	uint32_t atlas_w = sys->base.info.display_pixel_width;
	uint32_t atlas_h = sys->base.info.display_pixel_height;
	if (atlas_w == 0) atlas_w = sys->display_width;
	if (atlas_h == 0) atlas_h = sys->display_height;

	if (content_w != atlas_w || content_h != atlas_h) {
		// Lazy (re)create crop texture
		if (mc->crop_width != content_w || mc->crop_height != content_h) {
			mc->crop_srv.reset();
			mc->crop_texture.reset();
			D3D11_TEXTURE2D_DESC crop_desc = {};
			crop_desc.Width = content_w;
			crop_desc.Height = content_h;
			crop_desc.MipLevels = 1;
			crop_desc.ArraySize = 1;
			crop_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			crop_desc.SampleDesc.Count = 1;
			crop_desc.Usage = D3D11_USAGE_DEFAULT;
			crop_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			HRESULT chr = sys->device->CreateTexture2D(&crop_desc, nullptr, mc->crop_texture.put());
			if (SUCCEEDED(chr)) {
				sys->device->CreateShaderResourceView(mc->crop_texture.get(), nullptr, mc->crop_srv.put());
				mc->crop_width = content_w;
				mc->crop_height = content_h;
				U_LOG_W("Multi-comp: created crop texture %ux%u (view=%ux%u)", content_w, content_h, dp_view_w, dp_view_h);
			}
		}
		if (mc->crop_texture) {
			// Copy each view's content region from combined atlas to crop texture
			uint32_t num_views = sys->tile_columns * sys->tile_rows;
			for (uint32_t v = 0; v < num_views && v < XRT_MAX_VIEWS; v++) {
				uint32_t src_col = v % sys->tile_columns;
				uint32_t src_row = v / sys->tile_columns;
				uint32_t dst_col = src_col;
				uint32_t dst_row = src_row;
				D3D11_BOX box = {};
				box.left = src_col * dp_view_w;
				box.top = src_row * dp_view_h;
				box.right = box.left + dp_view_w;
				box.bottom = box.top + dp_view_h;
				box.front = 0;
				box.back = 1;
				sys->context->CopySubresourceRegion(
				    mc->crop_texture.get(), 0,
				    dst_col * dp_view_w, dst_row * dp_view_h, 0,
				    mc->combined_atlas.get(), 0, &box);
			}
			dp_input_srv = mc->crop_srv.get();
		}
	}

	// Run DP on cropped atlas → back buffer
	if (mc->display_processor != nullptr && dp_input_srv && mc->back_buffer_rtv) {
		ID3D11RenderTargetView *out_rtvs[] = {mc->back_buffer_rtv.get()};
		sys->context->OMSetRenderTargets(1, out_rtvs, nullptr);

		// Get actual back buffer dimensions
		uint32_t bb_w = sys->output_width;
		uint32_t bb_h = sys->output_height;
		if (mc->back_buffer_rtv) {
			wil::com_ptr<ID3D11Resource> bb_resource;
			mc->back_buffer_rtv->GetResource(bb_resource.put());
			wil::com_ptr<ID3D11Texture2D> bb_texture;
			if (SUCCEEDED(bb_resource->QueryInterface(IID_PPV_ARGS(bb_texture.put())))) {
				D3D11_TEXTURE2D_DESC bb_desc = {};
				bb_texture->GetDesc(&bb_desc);
				bb_w = bb_desc.Width;
				bb_h = bb_desc.Height;
			}
		}

		xrt_display_processor_d3d11_process_atlas(
		    mc->display_processor, sys->context.get(), dp_input_srv,
		    dp_view_w, dp_view_h, sys->tile_columns, sys->tile_rows,
		    DXGI_FORMAT_R8G8B8A8_UNORM, bb_w, bb_h,
		    0, 0, 0, 0);
	} else if (mc->back_buffer_rtv && mc->combined_atlas) {
		// Fallback: no DP — raw copy to back buffer
		wil::com_ptr<ID3D11Resource> back_buffer;
		mc->back_buffer_rtv->GetResource(back_buffer.put());
		sys->context->CopyResource(back_buffer.get(), mc->combined_atlas.get());
	}

	// Phase 8: screenshot file-trigger now routes through the same capture path
	// as the workspace-driven Ctrl+Shift+3 IPC call. Create
	// %TEMP%\workspace_screenshot_trigger to drop %TEMP%\workspace_screenshot_atlas.png
	// on the next frame.
	{
		static char ss_trigger[MAX_PATH] = {};
		static char ss_prefix[MAX_PATH] = {};
		if (!ss_trigger[0]) {
			const char *tmp = getenv("TEMP");
			if (!tmp) tmp = "C:\\Temp";
			snprintf(ss_trigger, sizeof(ss_trigger), "%s\\workspace_screenshot_trigger", tmp);
			snprintf(ss_prefix, sizeof(ss_prefix), "%s\\workspace_screenshot", tmp);
		}
		if (mc->combined_atlas &&
		    GetFileAttributesA(ss_trigger) != INVALID_FILE_ATTRIBUTES) {
			DeleteFileA(ss_trigger);
			struct ipc_capture_result dummy = {};
			comp_d3d11_service_capture_frame(&sys->base, ss_prefix,
			                                 IPC_CAPTURE_FLAG_ATLAS, &dummy);
		}
	}

	// MCP capture_frame: service a pending request before Present
	// so the atlas is fully populated.
	comp_d3d11_service_poll_mcp_capture((struct xrt_system_compositor *)sys);

	// Present
	if (mc->swap_chain) {
		mc->swap_chain->Present(1, 0);
	}

	// Signal WM_PAINT done
	if (mc->window != nullptr) {
		comp_d3d11_window_signal_paint_done(mc->window);
	}

	// Phase 2.K: bump the frame-tick counter once per displayed frame so the
	// public-API drain can emit FRAME_TICK events to the workspace controller
	// without polling. The drain reads the counter delta and synthesises one
	// event per missed frame (capped per batch).
	InterlockedIncrement(&mc->frame_tick_count);
}


static xrt_result_t
compositor_layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);
	struct d3d11_service_system *sys = c->sys;

	std::lock_guard<std::mutex> lock(c->mutex);

	// Check window validity - detect window close to end session
	if (!c->window_closed) {
		bool window_valid = true;
		if (c->render.owns_window && c->render.window != nullptr) {
			window_valid = comp_d3d11_window_is_valid(c->render.window);
		} else if (c->render.hwnd != nullptr) {
			window_valid = IsWindow(c->render.hwnd) != FALSE;
		}
		if (!window_valid) {
			U_LOG_W("Window closed - requesting session exit");
			c->window_closed = true;
			c->exit_request_sent = false;
			c->window_closed_frame_count = 0;
		}
	}

	if (c->window_closed) {
		c->window_closed_frame_count++;
		// Push EXIT_REQUEST once to trigger graceful session shutdown
		if (!c->exit_request_sent && c->xses != nullptr) {
			union xrt_session_event xse = XRT_STRUCT_INIT;
			xse.type = XRT_SESSION_EVENT_EXIT_REQUEST;
			xrt_session_event_sink_push(c->xses, &xse);
			c->exit_request_sent = true;
		}
		// Return success so the error doesn't propagate as XR_ERROR_INSTANCE_LOST.
		// The EXIT_REQUEST event drives the session to STOPPING so the app
		// calls xrEndSession and continues running.
		return XRT_SUCCESS;
	}

	// Track this as the active compositor for eye position queries
	{
		std::lock_guard<std::mutex> active_lock(sys->active_compositor_mutex);
		sys->active_compositor = c;
	}

	// Per-frame bridge-WS-client-live gate. Used below to enable/disable
	// bridge-specific paths (crop override, atlas-resize skip, qwerty
	// suppression, vendor hw-state forwarding). When the bridge process
	// exists but no extension is connected, this is false and legacy
	// behavior runs normally. Also drives qwerty relay state transitions
	// so qwerty wakes up the moment the WS client disconnects.
	// Per-client bridge_client_is_live is still used elsewhere in this
	// function for "is this specific client the bridge client" semantics
	// (crop override, atlas-resize skip, vendor hw-state forwarding).
	bool bridge_live = bridge_client_is_live(sys, c->render.hwnd);

	// Qwerty freeze gate uses the authoritative scan instead, so it is
	// stable across clients (doesn't oscillate when legacy + bridge-aware
	// sessions coexist, or when the bridge exe outlives its WS client).
	bool bridge_relay_live = bridge_relay_is_live_authoritative(sys);
	{
		static bool s_last_bridge_relay_live = false;
		if (bridge_relay_live != s_last_bridge_relay_live) {
			HWND sys_hwnd = sys != nullptr ? sys->compositor_hwnd : nullptr;
			bool prop_on_sys = sys_hwnd != nullptr &&
			                   GetPropW(sys_hwnd, L"DXR_BridgeClientActive") != nullptr;
			U_LOG_W("Bridge WS client %s — qwerty relay %s "
			        "(sys_hwnd=%p prop=%d, g_bridge_relay_active=%d)",
			        bridge_relay_live ? "connected" : "disconnected",
			        bridge_relay_live ? "ON" : "OFF",
			        (void *)sys_hwnd, prop_on_sys ? 1 : 0,
			        g_bridge_relay_active ? 1 : 0);
			s_last_bridge_relay_live = bridge_relay_live;
#ifdef XRT_BUILD_DRIVER_QWERTY
			qwerty_set_bridge_relay_active(bridge_relay_live);
#endif
		}
	}

	// Log frame submission (first frame and every 60 frames)
	static uint32_t frame_count = 0;
	if (frame_count == 0 || frame_count % 60 == 0) {
		U_LOG_W("layer_commit: frame %u, layers=%u", frame_count, c->layer_accum.layer_count);
	}
	frame_count++;

	// Handle window resize - check if swap chain needs to be resized
	// This is critical for SR weaving which requires viewport to match window
	// Check if in drag mode - defer expensive stereo texture reallocation during drag
	bool in_size_move = false;
	if (c->render.owns_window && c->render.window != nullptr) {
		in_size_move = comp_d3d11_window_is_in_size_move(c->render.window);
	}

	if (c->render.hwnd != nullptr && c->render.swap_chain) {
		RECT client_rect;
		if (GetClientRect(c->render.hwnd, &client_rect)) {
			uint32_t client_width = static_cast<uint32_t>(client_rect.right - client_rect.left);
			uint32_t client_height = static_cast<uint32_t>(client_rect.bottom - client_rect.top);

			// Check if swap chain size matches window client area
			DXGI_SWAP_CHAIN_DESC1 sc_desc = {};
			c->render.swap_chain->GetDesc1(&sc_desc);

			if (client_width > 0 && client_height > 0 &&
			    (sc_desc.Width != client_width || sc_desc.Height != client_height)) {
				U_LOG_W("Window resize detected: swap_chain=%ux%u, client=%ux%u - resizing%s",
				        sc_desc.Width, sc_desc.Height, client_width, client_height,
				        in_size_move ? " (drag in progress, deferring stereo resize)" : "");

				// Release back buffer RTV before resize
				c->render.back_buffer_rtv.reset();

				// Resize swap chain buffers - always do this immediately (DXGI requirement)
				HRESULT hr = c->render.swap_chain->ResizeBuffers(
				    0,  // Keep buffer count
				    client_width,
				    client_height,
				    DXGI_FORMAT_UNKNOWN,  // Keep format
				    0);

				if (SUCCEEDED(hr)) {
					// Recreate back buffer RTV
					wil::com_ptr<ID3D11Texture2D> back_buffer;
					c->render.swap_chain->GetBuffer(0, IID_PPV_ARGS(back_buffer.put()));
					sys->device->CreateRenderTargetView(back_buffer.get(), nullptr, c->render.back_buffer_rtv.put());

					U_LOG_W("Swap chain resized successfully to %ux%u", client_width, client_height);

					// Scale stereo texture proportionally to window/display ratio.
					// Skip during drag to avoid expensive texture reallocation every pixel.
					// Skip for bridge mode: the WebXR client swapchain is always
					// allocated at full-display worst-case, and the bridge pushes
					// content dims = windowSize × viewScale via DXR_BridgeViewW/H.
					// The conservative min-ratio shrink here can make the atlas
					// narrower than the bridge-computed content, clipping it.
					// The display processor handles mismatched stereo/target sizes via stretching.
					if (c->render.display_processor != nullptr && !in_size_move &&
					    !bridge_live) {
						uint32_t disp_px_w = 0, disp_px_h = 0;
						int32_t disp_left = 0, disp_top = 0;

						if (xrt_display_processor_d3d11_get_display_pixel_info(
						        c->render.display_processor, &disp_px_w, &disp_px_h,
						        &disp_left, &disp_top) &&
						    disp_px_w > 0 && disp_px_h > 0) {

							// Compute base view dims from display pixel info using tile layout
							uint32_t base_vw = disp_px_w / sys->tile_columns;
							uint32_t base_vh = disp_px_h / sys->tile_rows;

							// Scale view dims by window/display ratio
							// This preserves aspect ratio during resize
							float ratio = fminf(
							    (float)client_width / (float)disp_px_w,
							    (float)client_height / (float)disp_px_h);
							if (ratio > 1.0f) {
								ratio = 1.0f;  // Don't upscale beyond recommended
							}

							uint32_t new_view_w = (uint32_t)((float)base_vw * ratio);
							uint32_t new_view_h = (uint32_t)((float)base_vh * ratio);
							uint32_t new_atlas_w = sys->tile_columns * new_view_w;
							uint32_t new_atlas_h = sys->tile_rows * new_view_h;

							// Only resize if significantly different (avoid churn)
							D3D11_TEXTURE2D_DESC current_desc = {};
							if (c->render.atlas_texture) {
								c->render.atlas_texture->GetDesc(&current_desc);
							}

							if (current_desc.Width != new_atlas_w || current_desc.Height != new_atlas_h) {
								U_LOG_W("Resizing atlas texture: %ux%u -> %ux%u (ratio=%.3f)",
								        current_desc.Width, current_desc.Height,
								        new_atlas_w, new_atlas_h, ratio);

								// Release old stereo texture resources
								c->render.atlas_rtv.reset();
								c->render.atlas_srv.reset();
								c->render.atlas_srv_srgb.reset();
								c->render.atlas_texture.reset();

								// Create new atlas texture
								D3D11_TEXTURE2D_DESC atlas_desc = {};
								atlas_desc.Width = new_atlas_w;
								atlas_desc.Height = new_atlas_h;
								atlas_desc.MipLevels = 1;
								atlas_desc.ArraySize = 1;
								atlas_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
								atlas_desc.SampleDesc.Count = 1;
								atlas_desc.Usage = D3D11_USAGE_DEFAULT;
								atlas_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

								HRESULT atlas_hr = sys->device->CreateTexture2D(
								    &atlas_desc, nullptr, c->render.atlas_texture.put());
								if (SUCCEEDED(atlas_hr)) {
									sys->device->CreateShaderResourceView(
									    c->render.atlas_texture.get(), nullptr, c->render.atlas_srv.put());
									sys->device->CreateRenderTargetView(
									    c->render.atlas_texture.get(), nullptr, c->render.atlas_rtv.put());

									// Update system view dimensions for rendering
									sys->view_width = new_view_w;
									sys->view_height = new_view_h;
									sys->display_width = new_atlas_w;
									sys->display_height = new_atlas_h;

									U_LOG_W("Atlas texture resized: view=%ux%u, atlas=%ux%u",
									        new_view_w, new_view_h, new_atlas_w, new_atlas_h);
								} else {
									U_LOG_E("Failed to resize atlas texture: 0x%08lx", atlas_hr);
								}
							}
						}
					}
				} else {
					U_LOG_E("Failed to resize swap chain: 0x%08lx", hr);
				}
			}
		}
	}

	// Clear stereo render target.
	// In workspace mode, skip the clear — the blit overwrites the same tile positions
	// each frame, so previous content is a safe fallback. Clearing to black here
	// creates a race: if multi_compositor_render reads this atlas between the clear
	// and the blit, the window flashes black.
	if (c->render.atlas_rtv && !sys->workspace_mode) {
		float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
		sys->context->ClearRenderTargetView(c->render.atlas_rtv.get(), clear_color);
	}

	// Sync hardware_display_3d and tile layout from device's active rendering mode
	sync_tile_layout(sys);
	if (sys->xdev != NULL && sys->xdev->hmd != NULL) {
		uint32_t idx = sys->xdev->hmd->active_rendering_mode_index;
		if (idx < sys->xdev->rendering_mode_count) {
			sys->hardware_display_3d = sys->xdev->rendering_modes[idx].hardware_display_3d;
		}
	}

	// App-initiated mode change: bridge relays requestRenderingMode() from the
	// WebXR sample via HWND property. Polls each frame, triggers same server-side
	// path as qwerty V key (device update + DP toggle + broadcast).
	if (bridge_live && c->render.hwnd != nullptr && sys->xsysd != NULL) {
		uint32_t req = (uint32_t)(uintptr_t)GetPropW(c->render.hwnd, L"DXR_RequestMode");
		if (req > 0) {
			RemovePropW(c->render.hwnd, L"DXR_RequestMode");
			uint32_t modeIdx = req - 1; // decode +1 encoding
			struct xrt_device *head = sys->xsysd->static_roles.head;
			if (head != nullptr && head->hmd != NULL &&
			    modeIdx < head->rendering_mode_count &&
			    modeIdx != head->hmd->active_rendering_mode_index) {
				uint32_t prev_idx = head->hmd->active_rendering_mode_index;
				head->hmd->active_rendering_mode_index = modeIdx;
				broadcast_rendering_mode_change(sys, head, prev_idx, modeIdx);

				// Toggle DP 2D/3D
				bool want_3d = head->rendering_modes[modeIdx].hardware_display_3d;
				struct xrt_display_processor_d3d11 *dp = nullptr;
				if (sys->workspace_mode && sys->multi_comp != nullptr)
					dp = sys->multi_comp->display_processor;
				else if (c->render.display_processor != nullptr)
					dp = c->render.display_processor;
				if (dp != nullptr)
					xrt_display_processor_d3d11_request_display_mode(dp, want_3d);

				sync_tile_layout(sys);
				sys->hardware_display_3d = want_3d;
				U_LOG_W("App-initiated mode change: %u -> %u (3D=%d)", prev_idx, modeIdx, (int)want_3d);
			}
		}
	}

	// Runtime-side 2D/3D toggle (V key) — polls qwerty driver each frame.
	// Disabled when bridge is active: mode changes go through the HWND
	// property relay above (app-initiated path).
#ifdef XRT_BUILD_DRIVER_QWERTY
	if (sys->xsysd != NULL && !bridge_live) {
		bool force_2d = false;
		bool toggled = qwerty_check_display_mode_toggle(
		    sys->xsysd->xdevs, sys->xsysd->xdev_count, &force_2d);
		if (toggled) {
			struct xrt_device *head = sys->xsysd->static_roles.head;
			if (head != nullptr && head->hmd != NULL) {
				uint32_t prev_idx = head->hmd->active_rendering_mode_index;
				if (force_2d) {
					// Save current 3D mode index before switching to 2D
					uint32_t cur = head->hmd->active_rendering_mode_index;
					if (cur < head->rendering_mode_count &&
					    head->rendering_modes[cur].hardware_display_3d) {
						sys->last_3d_mode_index = cur;
					}
					head->hmd->active_rendering_mode_index = 0;
				} else {
					// Restore last 3D mode index
					head->hmd->active_rendering_mode_index = sys->last_3d_mode_index;
				}
				broadcast_rendering_mode_change(sys, head, prev_idx,
				                                head->hmd->active_rendering_mode_index);
			}
			// Switch display mode on the active DP.
			// In workspace mode, the multi-comp owns the DP (per-client has none).
			struct xrt_display_processor_d3d11 *dp = nullptr;
			if (sys->workspace_mode && sys->multi_comp != nullptr) {
				dp = sys->multi_comp->display_processor;
			} else if (c->render.display_processor != nullptr) {
				dp = c->render.display_processor;
			}
			if (dp != nullptr) {
				xrt_display_processor_d3d11_request_display_mode(dp, !force_2d);
			}
			sync_tile_layout(sys);
			sys->hardware_display_3d = !force_2d;
		}

		// Rendering mode change from qwerty 1/2/3 keys (disabled for legacy apps).
		if (!sys->base.info.legacy_app_tile_scaling) {
			int render_mode = -1;
			if (qwerty_check_rendering_mode_change(sys->xsysd->xdevs, sys->xsysd->xdev_count, &render_mode)) {
				struct xrt_device *head = sys->xsysd->static_roles.head;
				if (head != NULL && head->hmd != NULL) {
					uint32_t prev_idx = head->hmd->active_rendering_mode_index;
					xrt_device_set_property(head, XRT_DEVICE_PROPERTY_OUTPUT_MODE, render_mode);
					broadcast_rendering_mode_change(sys, head, prev_idx,
					                                head->hmd->active_rendering_mode_index);
				}
			}
		}
	}
#endif

	// Poll vendor SDK for hardware 3D state changes (e.g., Leia SR auto-switch on tracking loss).
	// This detects changes the vendor SDK made independently of the runtime.
	{
		struct xrt_display_processor_d3d11 *dp = nullptr;
		if (sys->workspace_mode && sys->multi_comp != nullptr) {
			dp = sys->multi_comp->display_processor;
		} else if (c->render.display_processor != nullptr) {
			dp = c->render.display_processor;
		}
		bool vendor_is_3d = false;
		if (xrt_display_processor_d3d11_get_hardware_3d_state(dp, &vendor_is_3d)) {
			if (vendor_is_3d != sys->hardware_display_3d) {
				U_LOG_W("Vendor SDK hardware 3D state changed: %s → %s",
				        sys->hardware_display_3d ? "3D" : "2D",
				        vendor_is_3d ? "3D" : "2D");
				sys->hardware_display_3d = vendor_is_3d;
				// When bridge is active, don't force the rendering mode
				// transition — let the app decide via requestRenderingMode().
				// The app receives a hardwarestatechange event and can react.
				// Forcing causes a brief glitch (2D content through 3D weaver).
				if (!bridge_live) {
				// Update the device's active rendering mode to match
				struct xrt_device *head = sys->xsysd ? sys->xsysd->static_roles.head : nullptr;
				if (head != nullptr && head->hmd != NULL) {
					uint32_t prev_idx = head->hmd->active_rendering_mode_index;
					if (!vendor_is_3d) {
						uint32_t cur = head->hmd->active_rendering_mode_index;
						if (cur < head->rendering_mode_count &&
						    head->rendering_modes[cur].hardware_display_3d) {
							sys->last_3d_mode_index = cur;
						}
						head->hmd->active_rendering_mode_index = 0; // mode 0 = 2D
					} else {
						head->hmd->active_rendering_mode_index = sys->last_3d_mode_index;
					}
					broadcast_rendering_mode_change(sys, head, prev_idx,
					                                head->hmd->active_rendering_mode_index);
				}
				sync_tile_layout(sys);
				}
			}
		}
	}

	// Get predicted eye positions (used for UI layers and HUD)
	struct xrt_eye_positions eye_pos = {};
	bool weaving_done = false;

	if (c->render.display_processor != nullptr) {
		xrt_display_processor_d3d11_get_predicted_eye_positions(
		    c->render.display_processor, &eye_pos);
	}
	if (!eye_pos.valid) {
		eye_pos.count = 2;
		eye_pos.eyes[0] = {-0.032f, 0.0f, 0.6f};
		eye_pos.eyes[1] = { 0.032f, 0.0f, 0.6f};
	}

	// Extract stereo pair for renderer (display processor still needs L/R)
	struct xrt_vec3 left_eye = {eye_pos.eyes[0].x, eye_pos.eyes[0].y, eye_pos.eyes[0].z};
	struct xrt_vec3 right_eye = {eye_pos.eyes[1].x, eye_pos.eyes[1].y, eye_pos.eyes[1].z};

	// Pre-compute whether there are UI overlay layers (quad/cylinder/equirect/cube).
	// Needed to decide if same-swapchain SBS can bypass the stereo texture entirely.
	bool has_ui_layers = false;
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		switch (c->layer_accum.layers[i].data.type) {
		case XRT_LAYER_QUAD:
		case XRT_LAYER_CYLINDER:
		case XRT_LAYER_EQUIRECT2:
		case XRT_LAYER_EQUIRECT1:
		case XRT_LAYER_CUBE:
			has_ui_layers = true;
			break;
		default:
			break;
		}
		if (has_ui_layers) break;
	}

	// Track zero-copy optimization: when all views are rendered into the same
	// swapchain texture with matching tiling layout, skip the blit and pass the
	// app's texture directly to the display processor.
	bool use_zero_copy = false;
	wil::com_ptr<ID3D11ShaderResourceView> zc_srv;
	ID3D11Texture2D *zc_tex = nullptr;
	uint32_t zc_view_w = 0, zc_view_h = 0;

	// Actual content dimensions from submitted rects - defaults to sys values,
	// overwritten per projection layer (may differ for legacy compromise-scale apps)
	uint32_t content_view_w = sys->view_width;
	uint32_t content_view_h = sys->view_height;

	// Bridge-relay: read active per-view tile dims pushed by the bridge.
	// The bridge (as the sample's proxy) owns windowSize × viewScale and
	// writes DXR_BridgeViewW/H via SetPropW each time the window or mode
	// changes. We crop at exactly what it pushed, guaranteeing match with
	// the sample's render — same model as cube_handle_d3d11_win, which
	// sets XrCompositionLayerProjectionView.subImage.imageRect directly.
	//
	// Fallback to display_width × viewScale when the bridge hasn't pushed
	// yet (first few frames before poll_window_metrics runs). Without this,
	// Chrome's compromise-scaled subImage.imageRect wins and views scramble.
	bool bridge_override = false;
	uint32_t active_vw = sys->view_width;
	uint32_t active_vh = sys->view_height;
	// Bridge-override also runs in workspace mode (Stage 3): the bridge pushes
	// slot-sized DXR_BridgeViewW/H so the blit crops exactly what the
	// sample rendered in each tile. Without this the non-override path
	// uses Chrome's submitted sub.rect.extent — which is the full Chrome
	// tile-stride (framebufferWidth / viewCount), not the bridge-authoritative
	// slot × viewScale. Multi-comp then reads a super-set rect from the
	// per-client atlas; only the top-left ~slot×viewScale portion is real
	// content, the rest is clear color → scene occupies only that fraction
	// of the workspace slot after the shader's source→dest scale.
	if (bridge_live) {
		uint32_t bvw = 0, bvh = 0;
		// Prefer the CURRENT frame's live HWND (c->render.hwnd) over the
		// cached sys->compositor_hwnd. When the WebXR page is reloaded the
		// Chrome compositor may recreate its window, leaving the cached
		// handle stale. Bridge's FindWindowW finds the current live window
		// and pushes DXR_BridgeViewW/H there; we must read from the same.
		HWND prop_hwnd = c->render.hwnd != nullptr ? c->render.hwnd : sys->compositor_hwnd;
		if (prop_hwnd) {
			bvw = (uint32_t)(uintptr_t)GetPropW(prop_hwnd, L"DXR_BridgeViewW");
			bvh = (uint32_t)(uintptr_t)GetPropW(prop_hwnd, L"DXR_BridgeViewH");
		}
		if (bvw > 0 && bvh > 0) {
			active_vw = bvw;
			active_vh = bvh;
			bridge_override = true;
		} else if (sys->xdev != NULL && sys->xdev->hmd != NULL &&
		           sys->display_width > 0 && sys->display_height > 0) {
			uint32_t mi = sys->xdev->hmd->active_rendering_mode_index;
			if (mi < sys->xdev->rendering_mode_count) {
				float sx = sys->xdev->rendering_modes[mi].view_scale_x;
				float sy = sys->xdev->rendering_modes[mi].view_scale_y;
				if (sx > 0.0f && sy > 0.0f) {
					uint32_t vw = (uint32_t)(sys->display_width * sx);
					uint32_t vh = (uint32_t)(sys->display_height * sy);
					if (vw > 0 && vh > 0) {
						active_vw = vw;
						active_vh = vh;
						bridge_override = true;
					}
				}
			}
		}
	}

	// Render projection layers to stereo texture (via copy)
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		struct comp_layer *layer = &c->layer_accum.layers[i];

		if (layer->data.type != XRT_LAYER_PROJECTION &&
		    layer->data.type != XRT_LAYER_PROJECTION_DEPTH) {
			continue;
		}

		// Determine view count: mono apps have 1 view, 3D mode uses all views
		uint32_t proj_view_count = layer->data.view_count;
		if (!sys->hardware_display_3d)
			proj_view_count = 1;
		if (proj_view_count > XRT_MAX_VIEWS)
			proj_view_count = XRT_MAX_VIEWS;

		// Extract per-view swapchains, textures, and image indices
		struct d3d11_service_swapchain *view_scs[XRT_MAX_VIEWS] = {};
		ID3D11Texture2D *view_textures[XRT_MAX_VIEWS] = {};
		uint32_t view_img_indices[XRT_MAX_VIEWS] = {};
		bool view_mutex_acquired[XRT_MAX_VIEWS] = {};
		D3D11_TEXTURE2D_DESC view_descs[XRT_MAX_VIEWS] = {};
		bool view_is_srgb[XRT_MAX_VIEWS] = {};

		bool views_valid = true;
		for (uint32_t eye = 0; eye < proj_view_count; eye++) {
			struct xrt_swapchain *xsc = layer->sc_array[eye];
			if (xsc == nullptr) {
				U_LOG_W("Projection layer %u missing swapchain for view %u", i, eye);
				views_valid = false;
				break;
			}
			view_scs[eye] = d3d11_service_swapchain_from_xrt(xsc);
			view_img_indices[eye] = layer->data.proj.v[eye].sub.image_index;

			if (view_img_indices[eye] >= view_scs[eye]->image_count) {
				U_LOG_W("Invalid image index in projection layer %u view %u", i, eye);
				views_valid = false;
				break;
			}
			view_textures[eye] = view_scs[eye]->images[view_img_indices[eye]].texture.get();
			if (view_textures[eye] == nullptr) {
				U_LOG_W("Missing texture in projection layer %u view %u", i, eye);
				views_valid = false;
				break;
			}
			view_textures[eye]->GetDesc(&view_descs[eye]);
			view_is_srgb[eye] = is_srgb_format(view_descs[eye].Format);
		}
		if (!views_valid) continue;

		// For service-created swapchains (WebXR), acquire KeyedMutex before reading
		const DWORD mutex_timeout_ms = 100;
		bool any_mutex_acquired = false;
		for (uint32_t eye = 0; eye < proj_view_count; eye++) {
			// Skip mutex for views sharing the same swapchain+image as a prior view
			bool already_locked = false;
			for (uint32_t prev = 0; prev < eye; prev++) {
				if (view_scs[eye] == view_scs[prev] && view_img_indices[eye] == view_img_indices[prev]) {
					already_locked = true;
					break;
				}
			}
			if (already_locked) continue;

			if (view_scs[eye]->service_created && view_scs[eye]->images[view_img_indices[eye]].keyed_mutex) {
				HRESULT hr = view_scs[eye]->images[view_img_indices[eye]].keyed_mutex->AcquireSync(0, mutex_timeout_ms);
				if (SUCCEEDED(hr)) {
					view_mutex_acquired[eye] = true;
					any_mutex_acquired = true;
				} else if (hr == static_cast<HRESULT>(WAIT_TIMEOUT)) {
					U_LOG_W("layer_commit: View %u mutex timeout (client still holding?)", eye);
				} else {
					U_LOG_W("layer_commit: Failed to acquire view %u mutex: 0x%08lx", eye, hr);
				}
			}
		}

		// Log projection layer info (first frame and every 60 frames)
		static uint32_t proj_log_count = 0;
		if (proj_log_count == 0 || proj_log_count % 60 == 0) {
			for (uint32_t eye = 0; eye < proj_view_count; eye++) {
				U_LOG_W("Projection layer %u view %u/%u: rect=(%d,%d %dx%d) array=%u fmt=%u(srgb=%d)",
				        i, eye, proj_view_count,
				        layer->data.proj.v[eye].sub.rect.offset.w, layer->data.proj.v[eye].sub.rect.offset.h,
				        layer->data.proj.v[eye].sub.rect.extent.w, layer->data.proj.v[eye].sub.rect.extent.h,
				        layer->data.proj.v[eye].sub.array_index,
				        view_descs[eye].Format, view_is_srgb[eye]);
			}
			U_LOG_W("  atlas_texture=%ux%u, view_width=%u, view_height=%u",
			        sys->display_width, sys->display_height, sys->view_width, sys->view_height);
		}
		proj_log_count++;

		// Zero-copy optimization: all views reference the same swapchain texture,
		// sub-rects match tiling layout, and texture matches content dimensions.
		// Skip the blit and pass the app's texture directly to the display processor.
		bool zero_copy = false;

		if (proj_view_count > 1 && !has_ui_layers && !any_mutex_acquired && !sys->workspace_mode) {
			// Check all views reference the same swapchain image
			bool all_same = true;
			for (uint32_t eye = 1; eye < proj_view_count; eye++) {
				if (view_scs[eye] != view_scs[0] || view_img_indices[eye] != view_img_indices[0]) {
					all_same = false;
					break;
				}
			}

			if (all_same) {
				// Use u_tiling_can_zero_copy to verify sub-rects match tiling layout
				int32_t rect_xs[XRT_MAX_VIEWS], rect_ys[XRT_MAX_VIEWS];
				uint32_t rect_ws[XRT_MAX_VIEWS], rect_hs[XRT_MAX_VIEWS];
				for (uint32_t eye = 0; eye < proj_view_count; eye++) {
					rect_xs[eye] = layer->data.proj.v[eye].sub.rect.offset.w;
					rect_ys[eye] = layer->data.proj.v[eye].sub.rect.offset.h;
					rect_ws[eye] = layer->data.proj.v[eye].sub.rect.extent.w;
					rect_hs[eye] = layer->data.proj.v[eye].sub.rect.extent.h;
				}

				// Get active rendering mode for zero-copy check
				struct xrt_device *xdev_head = (sys->xsysd != nullptr) ? sys->xsysd->static_roles.head : nullptr;
				const struct xrt_rendering_mode *active_mode = nullptr;
				if (xdev_head != nullptr && xdev_head->hmd != nullptr) {
					uint32_t idx = xdev_head->hmd->active_rendering_mode_index;
					if (idx < xdev_head->rendering_mode_count) {
						active_mode = &xdev_head->rendering_modes[idx];
					}
				}

				if (active_mode != nullptr &&
				    u_tiling_can_zero_copy(proj_view_count, rect_xs, rect_ys, rect_ws, rect_hs,
				                           view_descs[0].Width, view_descs[0].Height, active_mode)) {
					// Texture matches atlas dims exactly — zero-copy is safe
					D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
					srv_desc.Format = view_is_srgb[0] ? get_srgb_format(view_descs[0].Format) : view_descs[0].Format;
					srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
					srv_desc.Texture2D.MipLevels = 1;
					srv_desc.Texture2D.MostDetailedMip = 0;

					wil::com_ptr<ID3D11ShaderResourceView> app_srv;
					HRESULT hr = sys->device->CreateShaderResourceView(view_textures[0], &srv_desc, app_srv.put());
					if (SUCCEEDED(hr)) {
						zero_copy = true;
						use_zero_copy = true;
						zc_srv = std::move(app_srv);
						zc_tex = view_textures[0];
						zc_view_w = static_cast<uint32_t>(rect_ws[0]);
						zc_view_h = static_cast<uint32_t>(rect_hs[0]);

						static bool logged_zc = false;
						if (!logged_zc) {
							U_LOG_W("Zero-copy atlas: skipping blit, view=%ux%u, views=%u, fmt=0x%X",
							        rect_ws[0], rect_hs[0], proj_view_count, srv_desc.Format);
							logged_zc = true;
						}
					}
				}
			}
		}

		if (!zero_copy) {
		// Blit each view into its atlas tile position
		static bool logged_blit_path = false;
		if (!logged_blit_path) {
			U_LOG_W("Blit path: srgb=%d, blit_vs=%p -> %s",
			        view_is_srgb[0], (void*)sys->blit_vs.get(),
			        (view_is_srgb[0] && sys->blit_vs) ? "SHADER BLIT (linear output)" : "COPY (no conversion)");
			logged_blit_path = true;
		}

		// Record flip_y for multi-compositor render (GL clients need Y-flip)
		if (layer->data.flip_y) {
			c->atlas_flip_y = true;
		}

		static bool logged_bridge_blit = false;
		if (bridge_override && !logged_bridge_blit) {
			logged_bridge_blit = true;
			U_LOG_W("BRIDGE BLIT: active=%ux%u sys_view=%ux%u display=%ux%u "
			        "chrome_rect=(%d,%d %dx%d) tiles=%ux%u scale=%.2fx%.2f",
			        active_vw, active_vh, sys->view_width, sys->view_height,
			        sys->display_width, sys->display_height,
			        layer->data.proj.v[0].sub.rect.offset.w,
			        layer->data.proj.v[0].sub.rect.offset.h,
			        layer->data.proj.v[0].sub.rect.extent.w,
			        layer->data.proj.v[0].sub.rect.extent.h,
			        sys->tile_columns, sys->tile_rows,
			        (sys->xdev && sys->xdev->hmd && sys->xdev->hmd->active_rendering_mode_index < sys->xdev->rendering_mode_count)
			            ? sys->xdev->rendering_modes[sys->xdev->hmd->active_rendering_mode_index].view_scale_x : -1.0f,
			        (sys->xdev && sys->xdev->hmd && sys->xdev->hmd->active_rendering_mode_index < sys->xdev->rendering_mode_count)
			            ? sys->xdev->rendering_modes[sys->xdev->hmd->active_rendering_mode_index].view_scale_y : -1.0f);
		}

		for (uint32_t eye = 0; eye < proj_view_count; eye++) {
			float src_x = static_cast<float>(layer->data.proj.v[eye].sub.rect.offset.w);
			float src_y = static_cast<float>(layer->data.proj.v[eye].sub.rect.offset.h);
			float src_w = static_cast<float>(layer->data.proj.v[eye].sub.rect.extent.w);
			float src_h = static_cast<float>(layer->data.proj.v[eye].sub.rect.extent.h);

			if (bridge_override) {
				// Override: use active per-view dims (display × viewScale)
				// instead of Chrome's compromise-scaled subImage.imageRect
				// extent. The bridge sample rendered at this size. BUT
				// honor Chrome's sub.rect.offset for the tile position —
				// Chrome may allocate an fb larger than
				// tileColumns * active_vw (e.g. 7680×2160 instead of
				// 3840×2160 when it uses max-view-size per eye); each
				// view's sub-image is then placed at Chrome's offset, and
				// the bridge sample renders content at the TOP-LEFT of
				// that slot. Using tileX * active_vw for src_x would miss
				// it for bigger fbs.
				src_w = static_cast<float>(active_vw);
				src_h = static_cast<float>(active_vh);
				// src_x, src_y keep their values from Chrome's
				// sub.rect.offset above.
			}

			// Tile layout for atlas placement. The slot stride is
			// derived from the actual per-client atlas size, not from
			// `sys->view_width` — in workspace mode the per-client atlas is
			// created at native display pixels (e.g. 3840 wide) while
			// `sys->view_width` tracks the SCALED runtime view dim
			// (e.g. 960). They DIVERGE in workspace mode and using
			// `sys->view_width` as the tile stride forces a downsample
			// of any source larger than the scaled view but smaller than
			// the native slot — costing resolution and distorting aspect.
			//
			// `feedback_atlas_stride_invariant`: the invariant is
			// `slot_w = atlas_width / tile_columns`, applied identically
			// at write (here) and at read (multi_compositor_render).
			// Content can be smaller than the slot — sits top-left.
			// Content larger than the slot (Chrome's headset-scale frames
			// against a 1920-wide slot, etc.) is shader-scaled to slot.
			D3D11_TEXTURE2D_DESC atlas_desc = {};
			c->render.atlas_texture->GetDesc(&atlas_desc);
			uint32_t layout_vw = atlas_desc.Width / sys->tile_columns;
			uint32_t layout_vh = atlas_desc.Height / sys->tile_rows;
			uint32_t tile_x, tile_y;
			u_tiling_view_origin(eye, sys->tile_columns,
			                     layout_vw, layout_vh,
			                     &tile_x, &tile_y);

			// Scale only when source exceeds the slot. Handle apps in
			// workspace with reasonable HWND sizes typically render below the
			// native slot dim → raw copy at full source resolution.
			float tile_w = static_cast<float>(layout_vw);
			float tile_h = static_cast<float>(layout_vh);
			bool needs_scale = (src_w > tile_w || src_h > tile_h);
			float dst_w = needs_scale ? tile_w : 0.0f;
			float dst_h = needs_scale ? tile_h : 0.0f;

			// Color-space handling diverges between modes
			// (`feedback_srgb_blit_paths`):
			//   - non-workspace SRGB: sample through SRGB SRV → linearize on
			//     sample → write linear bytes to atlas. The DP expects
			//     linear input.
			//   - workspace mode:     atlas stays gamma-encoded;
			//     multi_compositor_render reads it as-is and the multi-comp
			//     pipeline downstream handles color space. Linearizing here
			//     would double-handle gamma.
			bool can_shader_blit = sys->blit_vs &&
			    view_scs[eye]->images[view_img_indices[eye]].srv;
			bool use_srgb_shader = can_shader_blit && view_is_srgb[eye] && !sys->workspace_mode;
			bool use_scale_shader = can_shader_blit && needs_scale && sys->workspace_mode;

			if (use_srgb_shader) {
				// Non-workspace SRGB: shader blit with SRGB SRV for linearization.
				// The GPU auto-linearizes when sampling through an SRGB SRV.
				// The DP expects linear input — without this, colors are washed out.
				wil::com_ptr<ID3D11ShaderResourceView> srgb_srv;
				D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
				srv_desc.Format = get_srgb_format(view_descs[eye].Format);
				srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				srv_desc.Texture2D.MipLevels = 1;
				srv_desc.Texture2D.MostDetailedMip = 0;
				HRESULT blit_hr = sys->device->CreateShaderResourceView(
				    view_textures[eye], &srv_desc, srgb_srv.put());
				if (SUCCEEDED(blit_hr)) {
					blit_to_atlas_texture(sys, &c->render, srgb_srv.get(),
					    src_x, src_y, src_w, src_h,
					    (float)view_descs[eye].Width, (float)view_descs[eye].Height,
					    (float)tile_x, (float)tile_y,
					    dst_w, dst_h, true);
				} else {
					// Fallback to raw copy
					D3D11_BOX box = {};
					box.left = (UINT)src_x; box.top = (UINT)src_y;
					box.right = (UINT)(src_x + src_w); box.bottom = (UINT)(src_y + src_h);
					box.front = 0; box.back = 1;
					sys->context->CopySubresourceRegion(c->render.atlas_texture.get(), 0,
					    tile_x, tile_y, 0, view_textures[eye],
					    layer->data.proj.v[eye].sub.array_index, &box);
				}
			} else if (use_scale_shader) {
				// Workspace mode + oversized client content: scale through the
				// shader using the default (non-SRGB) SRV so sampling reads
				// raw bytes and writes them unmodified — keeps the per-client
				// atlas in gamma space, matching the raw-copy path that
				// multi_compositor_render expects.
				blit_to_atlas_texture(sys, &c->render,
				    view_scs[eye]->images[view_img_indices[eye]].srv.get(),
				    src_x, src_y, src_w, src_h,
				    (float)view_descs[eye].Width, (float)view_descs[eye].Height,
				    (float)tile_x, (float)tile_y,
				    dst_w, dst_h, false);
			} else {
				// Non-SRGB, or workspace mode with content already fitting the
				// tile, or shader unavailable: raw byte copy. Multi-comp
				// (workspace) and non-SRGB DP handle the rest as today.
				D3D11_BOX box = {};
				box.left = static_cast<UINT>(src_x);
				box.top = static_cast<UINT>(src_y);
				box.right = static_cast<UINT>(src_x + src_w);
				box.bottom = static_cast<UINT>(src_y + src_h);
				box.front = 0;
				box.back = 1;

				sys->context->CopySubresourceRegion(
				    c->render.atlas_texture.get(),
				    0,                            // dst subresource
				    tile_x, tile_y, 0,            // dst x, y, z (tile position)
				    view_textures[eye],
				    layer->data.proj.v[eye].sub.array_index,  // src subresource
				    &box);
			}
		}
		} // !zero_copy

		// Track actual content dimensions for DP crop and multi-comp slot read.
		if (bridge_override) {
			// Bridge sample renders at active per-view dims — use those.
			content_view_w = active_vw;
			content_view_h = active_vh;
		} else {
			// Non-bridge: use Chrome's submitted subImage extent.
			content_view_w = static_cast<uint32_t>(layer->data.proj.v[0].sub.rect.extent.w);
			content_view_h = static_cast<uint32_t>(layer->data.proj.v[0].sub.rect.extent.h);
		}
		// Clamp to atlas slot dims (atlas_width / tile_columns). The blit
		// above placed at most one slot of content per tile; without this
		// clamp, multi_compositor_render reads past the slot boundary into
		// the neighbouring tile's slot (`feedback_atlas_stride_invariant`:
		// content can be smaller than slot but never larger; clamp dims
		// must use the SAME atlas-derived slot width as the blit's stride).
		if (c->render.atlas_texture) {
			D3D11_TEXTURE2D_DESC clamp_atlas_desc = {};
			c->render.atlas_texture->GetDesc(&clamp_atlas_desc);
			uint32_t slot_w = clamp_atlas_desc.Width / sys->tile_columns;
			uint32_t slot_h = clamp_atlas_desc.Height / sys->tile_rows;
			if (content_view_w > slot_w) content_view_w = slot_w;
			if (content_view_h > slot_h) content_view_h = slot_h;
		}

		// Track whether the bytes the raw-copy just placed in the atlas are
		// gamma-encoded (SRGB swapchain) or linear (UNORM swapchain).
		// multi_compositor_render uses this to pick atlas_srv vs
		// atlas_srv_srgb when sampling, so the DP receives linear bytes
		// regardless of the source swapchain's color-space. Eyes within a
		// projection layer share a swapchain format in practice; pick view 0.
		c->atlas_holds_srgb_bytes = view_is_srgb[0];

		// Store content dims on multi-comp slot for multi_compositor_render.
		// Also flip `has_first_frame_committed` on the false→true transition
		// and reset the entry animation's start time so the slot's grow-in
		// animation plays once Chrome (or any IPC client) has actually
		// submitted content. Without this, the multi-comp would draw the
		// slot at intermediate animation sizes with uninitialized atlas
		// content for the 2-3s while Chrome's WebGL is initializing —
		// visible as a narrow black rectangle that jumps to full size when
		// the first frame lands. Mirrors the capture-client `capture_srv`
		// readiness gate.
		if (sys->workspace_mode && sys->multi_comp != nullptr) {
			for (int s = 0; s < D3D11_MULTI_MAX_CLIENTS; s++) {
				if (sys->multi_comp->clients[s].active &&
				    sys->multi_comp->clients[s].compositor == c) {
					struct d3d11_multi_client_slot *slot =
					    &sys->multi_comp->clients[s];
					slot->content_view_w = content_view_w;
					slot->content_view_h = content_view_h;
					if (!slot->has_first_frame_committed) {
						slot->has_first_frame_committed = true;
						slot->first_frame_ns =
						    os_monotonic_get_ns();
					}
					break;
				}
			}
		}

		// Release KeyedMutex after reading
		for (uint32_t eye = 0; eye < proj_view_count; eye++) {
			if (view_mutex_acquired[eye]) {
				view_scs[eye]->images[view_img_indices[eye]].keyed_mutex->ReleaseSync(0);
			}
		}

		U_LOG_T("Rendered projection layer %u", i);
	}

	// Render UI layers if any exist and shaders are ready
	if (has_ui_layers && sys->quad_vs) {
		// Bind per-client stereo render target
		ID3D11RenderTargetView *rtvs[] = {c->render.atlas_rtv.get()};
		sys->context->OMSetRenderTargets(1, rtvs, nullptr);

		// Set common rendering state
		sys->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		sys->context->IASetInputLayout(nullptr);  // Using SV_VertexID
		sys->context->RSSetState(sys->rasterizer_state.get());
		sys->context->OMSetDepthStencilState(sys->depth_disabled.get(), 0);

		// Create default view poses and FOVs for each view
		uint32_t ui_view_count = sys->hardware_display_3d
		    ? (sys->tile_columns * sys->tile_rows) : 1;
		if (ui_view_count > XRT_MAX_VIEWS)
			ui_view_count = XRT_MAX_VIEWS;

		struct xrt_pose view_poses[XRT_MAX_VIEWS];
		struct xrt_fov fovs[XRT_MAX_VIEWS];

		// Use eye positions from display processor (interpolate for N views)
		const float fov_angle = 0.785f;  // ~45 degrees
		for (uint32_t view = 0; view < ui_view_count; view++) {
			view_poses[view].orientation.x = 0.0f;
			view_poses[view].orientation.y = 0.0f;
			view_poses[view].orientation.z = 0.0f;
			view_poses[view].orientation.w = 1.0f;

			// Use eye position if available, fall back to interpolated stereo baseline
			if (view < eye_pos.count) {
				view_poses[view].position.x = eye_pos.eyes[view].x;
				view_poses[view].position.y = eye_pos.eyes[view].y;
				view_poses[view].position.z = eye_pos.eyes[view].z;
			} else if (view == 0) {
				view_poses[view].position = left_eye;
			} else {
				view_poses[view].position = right_eye;
			}

			fovs[view].angle_left = -fov_angle;
			fovs[view].angle_right = fov_angle;
			fovs[view].angle_up = fov_angle;
			fovs[view].angle_down = -fov_angle;
		}
		for (uint32_t view_index = 0; view_index < ui_view_count; view_index++) {
			// Set viewport for this view
			D3D11_VIEWPORT viewport = {};
			if (!sys->hardware_display_3d) {
				// MONO: use output (window) dimensions so 2D content
				// fills the full window, capped to stereo texture size.
				uint32_t mono_w = (sys->output_width < sys->display_width)
				                      ? sys->output_width : sys->display_width;
				uint32_t mono_h = (sys->output_height < sys->display_height)
				                      ? sys->output_height : sys->display_height;
				viewport.TopLeftX = 0.0f;
				viewport.Width = static_cast<float>(mono_w);
				viewport.Height = static_cast<float>(mono_h);
			} else {
				// STEREO: tiled atlas layout
				uint32_t tile_x, tile_y;
				u_tiling_view_origin(view_index, sys->tile_columns,
				                     sys->view_width, sys->view_height,
				                     &tile_x, &tile_y);
				viewport.TopLeftX = static_cast<float>(tile_x);
				viewport.TopLeftY = static_cast<float>(tile_y);
				viewport.Width = static_cast<float>(sys->view_width);
				viewport.Height = static_cast<float>(sys->view_height);
			}
			viewport.MinDepth = 0.0f;
			viewport.MaxDepth = 1.0f;
			sys->context->RSSetViewports(1, &viewport);

			// Render equirect2 layers first (background/skybox)
			for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
				struct comp_layer *layer = &c->layer_accum.layers[i];
				if (layer->data.type == XRT_LAYER_EQUIRECT2) {
					if (is_layer_view_visible(&layer->data, view_index)) {
						render_equirect2_layer(sys, layer, view_index,
						                       &view_poses[view_index],
						                       &fovs[view_index]);
					}
				}
			}

			// Render cylinder layers
			for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
				struct comp_layer *layer = &c->layer_accum.layers[i];
				if (layer->data.type == XRT_LAYER_CYLINDER) {
					if (is_layer_view_visible(&layer->data, view_index)) {
						render_cylinder_layer(sys, layer, view_index,
						                      &view_poses[view_index],
						                      &fovs[view_index]);
					}
				}
			}

			// Render quad layers last (on top)
			for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
				struct comp_layer *layer = &c->layer_accum.layers[i];
				if (layer->data.type == XRT_LAYER_QUAD) {
					if (is_layer_view_visible(&layer->data, view_index)) {
						render_quad_layer(sys, layer, view_index,
						                  &view_poses[view_index],
						                  &fovs[view_index]);
					}
				}
			}
		}
	}

	// Workspace mode: per-client atlas rendering is done. The multi-compositor
	// composites all client atlases into the combined atlas and presents.
	// --- Lazy reverse hot-switch (workspace re-activated) ---
	// Tear down per-client standalone resources on the app's own thread.
	// Hide the HWND last (sends WM but app's main thread isn't blocked here
	// since we're about to return from this layer_commit).
	if (c->pending_workspace_reentry) {
		U_LOG_W("Reverse hot-switch: tearing down standalone resources");
		c->pending_workspace_reentry = false;

		if (c->render.display_processor != nullptr) {
			xrt_display_processor_d3d11_request_display_mode(c->render.display_processor, false);
			xrt_display_processor_d3d11_destroy(&c->render.display_processor);
		}
		c->render.back_buffer_rtv.reset();
		c->render.swap_chain.reset();
		if (c->render.hud != nullptr) {
			u_hud_destroy(&c->render.hud);
		}
		c->render.hwnd = nullptr;

		// Hide the app's HWND (workspace composites the content).
		if (c->app_hwnd != nullptr && IsWindow(c->app_hwnd)) {
			ShowWindowAsync(c->app_hwnd, SW_HIDE);
		}
		U_LOG_W("Reverse hot-switch: done — back to export mode");
	}

	if (sys->workspace_mode) {
		// Throttle renders to ~1 per VSync (~14ms). With N clients each calling
		// layer_commit at 60fps, we'd otherwise render N times per frame cycle.
		// Throttling reduces the chance of reading a client's atlas mid-blit.
		uint64_t now_ns = os_monotonic_get_ns();
		uint64_t elapsed_ns = now_ns - sys->last_workspace_render_ns;
		if (elapsed_ns < 14000000ULL && sys->last_workspace_render_ns != 0) {
			return XRT_SUCCESS; // Skip — another client will render soon
		}
		std::lock_guard<std::recursive_mutex> render_lock(sys->render_mutex);
		multi_compositor_render(sys);
		sys->last_workspace_render_ns = now_ns;
		return XRT_SUCCESS;
	}

	// --- Lazy standalone init (hot-switch from workspace → standalone) ---
	// Workspace was deactivated: workspace_mode is false but this client was created
	// in workspace mode (no swap chain, no DP). Create standalone resources now,
	// on the app's own IPC thread — safe from WM deadlocks.
	if (!c->render.swap_chain) {
		U_LOG_W("Hot-switch check: swap_chain=NULL, app_hwnd=%p, workspace_mode=%d",
		        (void *)c->app_hwnd, sys->workspace_mode);
	}
	if (!c->render.swap_chain && c->app_hwnd != nullptr && IsWindow(c->app_hwnd)) {
		U_LOG_W("Hot-switch: lazy standalone init for HWND=%p", (void *)c->app_hwnd);

		// Show the HWND with ShowWindowAsync to avoid deadlock with the
		// app's main thread (blocked on this IPC layer_commit). Keep
		// decorations intact — user wants the standalone window to look
		// like a normal app window.
		ShowWindowAsync(c->app_hwnd, SW_SHOWNOACTIVATE);

		c->render.hwnd = c->app_hwnd;
		c->render.owns_window = false;

		// Swap chain at display-native size (matches DP expectation).
		// The compositor's auto-resize handler will adapt it to the
		// HWND client rect on the next frame.
		uint32_t sc_w = sys->base.info.display_pixel_width;
		uint32_t sc_h = sys->base.info.display_pixel_height;
		if (sc_w == 0 || sc_h == 0) {
			sc_w = sys->output_width * 2;
			sc_h = sys->output_height * 2;
		}

		DXGI_SWAP_CHAIN_DESC1 sc_desc = {};
		sc_desc.Width = sc_w;
		sc_desc.Height = sc_h;
		sc_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sc_desc.SampleDesc.Count = 1;
		sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sc_desc.BufferCount = 2;
		sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

		HRESULT hr = sys->dxgi_factory->CreateSwapChainForHwnd(
		    sys->device.get(), c->app_hwnd, &sc_desc,
		    nullptr, nullptr, c->render.swap_chain.put());
		if (SUCCEEDED(hr)) {
			// Reduce DXGI frame latency to 1 — minimizes the queue depth
			// between Present and DWM cross-process composition. Critical
			// for smooth drag (otherwise frames are presented at stale
			// window positions).
			wil::com_ptr<IDXGIDevice1> dxgi_device;
			if (SUCCEEDED(sys->device->QueryInterface(IID_PPV_ARGS(dxgi_device.put())))) {
				dxgi_device->SetMaximumFrameLatency(1);
			}

			wil::com_ptr<ID3D11Texture2D> bb;
			c->render.swap_chain->GetBuffer(0, IID_PPV_ARGS(bb.put()));
			sys->device->CreateRenderTargetView(bb.get(), nullptr, c->render.back_buffer_rtv.put());
			U_LOG_W("Hot-switch: swap chain created (%ux%u)", sc_w, sc_h);
		} else {
			U_LOG_E("Hot-switch: swap chain failed (hr=0x%08X)", hr);
		}

		if (sys->base.info.dp_factory_d3d11 != NULL) {
			auto factory = (xrt_dp_factory_d3d11_fn_t)sys->base.info.dp_factory_d3d11;
			factory(sys->device.get(), sys->context.get(),
			        c->app_hwnd, &c->render.display_processor);
			if (c->render.display_processor != nullptr) {
				// Phase 6.1 (#140): don't call request_display_mode(true)
				// — same SR SDK recalibration issue. DP comes up in the
				// current mode; V key toggle works.
				U_LOG_W("Hot-switch: DP created — standalone rendering active");
			} else {
				U_LOG_W("Hot-switch: no DP (factory returned null) — raw copy fallback");
			}
		}

		// Enable HUD for standalone mode diagnostics
		if (c->render.hud == nullptr) {
			uint32_t hud_w = sc_w > 0 ? sc_w : sys->output_width;
			c->render.smoothed_frame_time_ms = 16.67f;
			u_hud_create(&c->render.hud, hud_w);
		}
	}

	// During drag, synchronize with the window thread's WM_PAINT cycle.
	// This ensures the window position is stable between weave() and Present(),
	// so the interlacing pattern matches the actual displayed position.
	if (c->render.owns_window && c->render.window != nullptr &&
	    comp_d3d11_window_is_in_size_move(c->render.window)) {
		comp_d3d11_window_wait_for_paint(c->render.window);
	}


	// Select display processor input: zero-copy from app's swapchain, or atlas.
	// The DP expects the atlas texture to be exactly content-sized
	// (2*view_width x view_height for SBS). When a legacy/compromise-scale app
	// renders smaller content into a larger atlas, crop-blit to a content-sized
	// staging texture before passing to the DP.
	ID3D11ShaderResourceView *input_srv = nullptr;
	uint32_t input_view_w = 0;
	uint32_t input_view_h = 0;

	if (use_zero_copy) {
		input_srv = zc_srv.get();
		input_view_w = zc_view_w;
		input_view_h = zc_view_h;
	} else {
		// Crop atlas to content dims (mirrors d3d11_crop_atlas_for_dp in in-process path)
		input_srv = service_crop_atlas_for_dp(sys, &c->render, content_view_w, content_view_h, c->atlas_flip_y);
		input_view_w = content_view_w;
		input_view_h = content_view_h;
	}

	// Always pass through the display processor — both 3D (weaving) and 2D
	// (stretch-blit). This matches the in-process compositor path where
	// process_atlas() handles all display modes. No separate mono blit needed.
	if (c->render.display_processor != nullptr && input_srv) {
		// Bind back buffer as output
		ID3D11RenderTargetView *rtvs[] = {c->render.back_buffer_rtv.get()};
		sys->context->OMSetRenderTargets(1, rtvs, nullptr);

		// Get actual back buffer dimensions for viewport
		uint32_t back_buffer_width = sys->output_width;
		uint32_t back_buffer_height = sys->output_height;
		if (c->render.back_buffer_rtv) {
			wil::com_ptr<ID3D11Resource> bb_resource;
			c->render.back_buffer_rtv->GetResource(bb_resource.put());
			wil::com_ptr<ID3D11Texture2D> bb_texture;
			if (SUCCEEDED(bb_resource->QueryInterface(IID_PPV_ARGS(bb_texture.put())))) {
				D3D11_TEXTURE2D_DESC bb_desc = {};
				bb_texture->GetDesc(&bb_desc);
				back_buffer_width = bb_desc.Width;
				back_buffer_height = bb_desc.Height;
			}
		}

		// Canvas = (0,0,0,0): IPC hosted apps always own the full window,
		// so canvas equals back buffer — no sub-rect needed.
		static bool logged_dp = false;
		if (!logged_dp) {
			logged_dp = true;
			U_LOG_W("DP HANDOFF: input_view=%ux%u tiles=%ux%u bb=%ux%u content=%ux%u zc=%d flip_y=%d",
			        input_view_w, input_view_h, sys->tile_columns, sys->tile_rows,
			        back_buffer_width, back_buffer_height,
			        content_view_w, content_view_h,
			        (int)use_zero_copy, (int)c->atlas_flip_y);
		}
		xrt_display_processor_d3d11_process_atlas(
		    c->render.display_processor, sys->context.get(), input_srv,
		    input_view_w, input_view_h, sys->tile_columns, sys->tile_rows,
		    DXGI_FORMAT_R8G8B8A8_UNORM, back_buffer_width, back_buffer_height,
		    0, 0, 0, 0);
		weaving_done = true;
	} else if (c->render.back_buffer_rtv && input_srv) {
		// Fallback: no display processor — raw copy to back buffer
		wil::com_ptr<ID3D11Resource> back_buffer;
		c->render.back_buffer_rtv->GetResource(back_buffer.put());
		if (use_zero_copy && zc_tex) {
			D3D11_BOX src_box = {0, 0, 0, sys->tile_columns * input_view_w, sys->tile_rows * input_view_h, 1};
			sys->context->CopySubresourceRegion(
			    back_buffer.get(), 0, 0, 0, 0,
			    zc_tex, 0, &src_box);
		} else if (c->render.atlas_texture) {
			sys->context->CopyResource(back_buffer.get(), c->render.atlas_texture.get());
		}
	}

	// Render HUD overlay (post-weave, pre-present)
	d3d11_service_render_hud(sys, &c->render, weaving_done, &eye_pos);

	// Present to display
	if (c->render.swap_chain) {
		c->render.swap_chain->Present(1, 0);  // VSync
		// For cross-process swap chains (post-hot-switch, external HWND),
		// DwmFlush blocks until the next DWM composition pass — minimizes
		// the latency between Present and the frame appearing on screen,
		// which improves drag smoothness. Without it, the IPC response
		// unblocks the app's modal drag loop while the frame is still
		// queued for DWM composition, and by the time DWM presents, the
		// window has moved further, causing visual stutter.
		if (!c->render.owns_window && c->app_hwnd != nullptr) {
			DwmFlush();
		}
	}

	// Signal WM_PAINT that the frame is done (unblocks modal drag loop)
	if (c->render.owns_window && c->render.window != nullptr) {
		comp_d3d11_window_signal_paint_done(c->render.window);
	}

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_layer_commit_with_semaphore(struct xrt_compositor *xc,
                                        struct xrt_compositor_semaphore *xcsem,
                                        uint64_t value)
{
	return compositor_layer_commit(xc, XRT_GRAPHICS_SYNC_HANDLE_INVALID);
}

static void
compositor_destroy(struct xrt_compositor *xc)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);
	struct d3d11_service_system *sys = c->sys;

	U_LOG_W("Destroying D3D11 service compositor for client");

	// If this was the bridge-relay session, clear the global gate so
	// subsequent non-bridge WebXR / legacy sessions get normal compositor
	// behavior (qwerty input enabled, V/number keys toggle mode, the
	// bridge_override crop path stays dormant until another bridge
	// connects). Without this the flag stays true across session ends
	// and poisons any later non-bridge session on the same service.
	if (c->is_bridge_relay) {
		U_LOG_W("Bridge relay session ending — clearing g_bridge_relay_active");
		g_bridge_relay_active = false;
#ifdef XRT_BUILD_DRIVER_QWERTY
		qwerty_set_bridge_relay_active(false);
#endif
	}

	// Unregister from multi-compositor before cleanup.
	// Always unregister if there's a multi_comp — the client may have been
	// registered in workspace mode but is now closing in standalone mode (after
	// hot-switch). Without this, the slot stays stale and shows a ghost
	// remnant on workspace re-activate.
	if (sys->multi_comp != nullptr) {
		std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);
		multi_compositor_unregister_client(sys, c);
	}

	// Clear active compositor if it's this one
	{
		std::lock_guard<std::mutex> lock(sys->active_compositor_mutex);
		if (sys->active_compositor == c) {
			sys->active_compositor = nullptr;
		}
	}

	// Clean up per-client render resources (window, swap chain, display processor)
	fini_client_render_resources(&c->render);

	delete c;
}


/*
 *
 * System compositor functions
 *
 */

static xrt_result_t
system_set_state(struct xrt_system_compositor *xsc, struct xrt_compositor *xc, bool visible, bool focused)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);

	// Only push event if state actually changed
	if (c->state_visible != visible || c->state_focused != focused) {
		c->state_visible = visible;
		c->state_focused = focused;

		union xrt_session_event xse = XRT_STRUCT_INIT;
		xse.type = XRT_SESSION_EVENT_STATE_CHANGE;
		xse.state.visible = visible;
		xse.state.focused = focused;

		U_LOG_W("D3D11 service: pushing state change event (visible=%d, focused=%d)", visible, focused);
		return xrt_session_event_sink_push(c->xses, &xse);
	}

	return XRT_SUCCESS;
}

static xrt_result_t
system_set_z_order(struct xrt_system_compositor *xsc, struct xrt_compositor *xc, int64_t z_order)
{
	// D3D11 service doesn't need z_order handling for single-client case
	(void)xsc;
	(void)xc;
	(void)z_order;
	return XRT_SUCCESS;
}

static xrt_result_t
system_create_native_compositor(struct xrt_system_compositor *xsysc,
                                const struct xrt_session_info *xsi,
                                struct xrt_session_event_sink *xses,
                                struct xrt_compositor_native **out_xcn)
{
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);

	// Create per-client native compositor
	struct d3d11_service_compositor *c = new d3d11_service_compositor();
	std::memset(&c->base, 0, sizeof(c->base));

	c->sys = sys;
	c->log_level = sys->log_level;
	c->frame_id = 0;

	// Store session event sink for pushing state change events
	c->xses = xses;
	c->state_visible = false;
	c->state_focused = false;
	c->window_closed = false;
	c->exit_request_sent = false;
	c->window_closed_frame_count = 0;
	c->is_bridge_relay = false;

	// Initialize layer accumulator
	std::memset(&c->layer_accum, 0, sizeof(c->layer_accum));

	// Bridge relay sessions (headless + XR_EXT_display_info) only need event
	// registration — skip window, swap chain, and display processor creation.
	bool is_headless_relay = (xsi != nullptr && xsi->is_bridge_relay);
	if (is_headless_relay) {
		U_LOG_W("Bridge relay session: skipping render resources (headless, events only)");
		// Session-lifecycle flag — coarse "a bridge session exists" gate.
		// Per-frame behavior in compositor_layer_commit reads the finer-
		// grained bridge_client_is_live() (session-lifecycle AND WS
		// client connected via DXR_BridgeClientActive prop). That function
		// owns qwerty_set_bridge_relay_active() so we don't pin qwerty
		// to "suppressed" while the bridge exe is idle.
		c->is_bridge_relay = true;
		g_bridge_relay_active = true;
	}

	// Phase 2.I-followup: workspace controllers (no graphics binding) talk
	// to the service to dispatch workspace + launcher extensions but render
	// nothing. Skip render resource init AND multi-compositor slot
	// registration; otherwise the controller appears as a renderable tile
	// inside its own workspace (titled with the controller's xrInstance
	// applicationName, e.g. the workspace controller's process name).
	bool is_workspace_controller = (xsi != nullptr && xsi->is_workspace_controller);
	if (is_workspace_controller) {
		U_LOG_W("Workspace-controller session: skipping render resources + slot registration");
	}

	// Initialize per-client render resources (window, swap chain, display processor)
	// Get external window handle if app provided one via XR_EXT_win32_window_binding
	void *external_hwnd = nullptr;
	if (xsi != nullptr) {
		external_hwnd = xsi->external_window_handle;
	}

	if (!is_headless_relay && !is_workspace_controller) {
		// Activate workspace mode from system compositor info (set by ipc_server_process.c
		// after init_all, before any client connects)
		if (sys->base.info.workspace_mode && !sys->workspace_mode) {
			service_set_workspace_mode(sys, true);
			U_LOG_W("Workspace mode activated for D3D11 service system");
		}

		xrt_result_t res_ret = init_client_render_resources(sys, external_hwnd, sys->xsysd, &c->render);
		if (res_ret != XRT_SUCCESS) {
			U_LOG_E("Failed to initialize client render resources");
			delete c;
			return res_ret;
		}
	}

	// Register with multi-compositor in workspace mode. Skip bridge-relay
	// AND workspace-controller sessions — neither has anything to render,
	// and a phantom slot would (a) keep mc->client_count > 0 after the
	// session ends, suppressing the empty-workspace launcher hint and
	// occluding the launcher when summoned, and (b) for the controller
	// specifically, surface its own xrInstance applicationName as a tile
	// title inside the workspace it is supposed to be controlling.
	// compositor_destroy's unregister call is already a no-op on a
	// never-registered compositor (its loop won't find a matching slot).
	if (sys->workspace_mode && !is_headless_relay && !is_workspace_controller) {
		// Ensure multi_comp struct exists for registration
		// Eagerly create multi-comp output (window + DP) on first client connect.
		// This ensures the DP is available for ipc_try_get_sr_view_poses
		// when the client calls xrLocateViews (before the first layer_commit).
		xrt_result_t mc_ret = multi_compositor_ensure_output(sys);
		if (mc_ret != XRT_SUCCESS) {
			U_LOG_E("Workspace mode: failed to create multi-comp output");
			fini_client_render_resources(&c->render);
			delete c;
			return mc_ret;
		}
		int slot;
		{
			std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);
			slot = multi_compositor_register_client(sys, c);
		}
		if (slot < 0) {
			U_LOG_E("Workspace mode: max clients (%d) reached", D3D11_MULTI_MAX_CLIENTS);
			fini_client_render_resources(&c->render);
			delete c;
			return XRT_ERROR_D3D11;
		}

		// Store app's HWND in the slot (for future workspace commands: resize, input forwarding).
		// HWND resize is done CLIENT-SIDE in oxr_session_create (before the IPC call)
		// because cross-process SetWindowPos deadlocks when called from the IPC handler.
		sys->multi_comp->clients[slot].app_hwnd = (HWND)external_hwnd;

		// Also store on compositor for lazy standalone init during hot-switch
		c->app_hwnd = (HWND)external_hwnd;

		// Get app name from HWND title for title bar display.
		// Fallback chain:
		//   1. Window text (handle apps that expose their HWND).
		//   2. xsi->application_name (clients that don't set
		//      XR_EXT_win32_window_binding, like Chrome WebXR through the
		//      bridge — ipc_handle_session_create populates this from the
		//      IPC client's xrInstance applicationInfo).
		//   3. "App <slot>" as last resort.
		// If another slot already has the same name, append "-2", "-3", etc.
		{
			char base_name[128] = {0};
			if (external_hwnd != 0) {
				int len = GetWindowTextA((HWND)external_hwnd, base_name, sizeof(base_name));
				if (len <= 0) base_name[0] = '\0';
			}
			if (base_name[0] == '\0' && xsi != NULL && xsi->application_name[0] != '\0') {
				snprintf(base_name, sizeof(base_name), "%s", xsi->application_name);
			}
			if (base_name[0] == '\0') {
				snprintf(base_name, sizeof(base_name), "App %d", slot);
			}

			// Replace non-ASCII characters with '-' (bitmap font only supports 0x20-0x7E)
			for (char *p = base_name; *p; p++) {
				if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E) *p = '-';
			}
			// Truncate at first " - " separator (strip compositor/subtitle info)
			char *sep = strstr(base_name, " - ");
			if (sep) *sep = '\0';

			// Count existing instances with the same base name.
			// Existing names may be "AppName" or "AppName (N)" format.
			int instance = 1;
			for (int i = 0; i < D3D11_MULTI_MAX_CLIENTS; i++) {
				if (i == slot || !sys->multi_comp->clients[i].active) continue;
				char existing_base[128];
				snprintf(existing_base, sizeof(existing_base), "%s", sys->multi_comp->clients[i].app_name);
				// Strip " (N)" suffix if present
				char *paren = strrchr(existing_base, '(');
				if (paren && paren > existing_base && *(paren - 1) == ' ') {
					*(paren - 1) = '\0';
				}
				if (strcmp(existing_base, base_name) == 0) {
					instance++;
				}
			}

			if (instance > 1) {
				snprintf(sys->multi_comp->clients[slot].app_name,
				         sizeof(sys->multi_comp->clients[slot].app_name),
				         "%s (%d)", base_name, instance);
			} else {
				snprintf(sys->multi_comp->clients[slot].app_name,
				         sizeof(sys->multi_comp->clients[slot].app_name),
				         "%s", base_name);
			}
		}

		// Update input forwarding now that app_hwnd is stored
		// (register_client may have set focused_slot before app_hwnd was available)
		multi_compositor_update_input_forward(sys->multi_comp);
	}

	// Set up compositor vtable
	c->base.base.get_swapchain_create_properties = compositor_get_swapchain_create_properties;
	c->base.base.create_swapchain = compositor_create_swapchain;
	c->base.base.import_swapchain = compositor_import_swapchain;
	c->base.base.import_fence = compositor_import_fence;
	c->base.base.create_semaphore = compositor_create_semaphore;
	c->base.base.begin_session = compositor_begin_session;
	c->base.base.end_session = compositor_end_session;
	c->base.base.predict_frame = compositor_predict_frame;
	c->base.base.wait_frame = compositor_wait_frame;
	c->base.base.mark_frame = compositor_mark_frame;
	c->base.base.begin_frame = compositor_begin_frame;
	c->base.base.discard_frame = compositor_discard_frame;
	c->base.base.layer_begin = compositor_layer_begin;
	c->base.base.layer_projection = compositor_layer_projection;
	c->base.base.layer_projection_depth = compositor_layer_projection_depth;
	c->base.base.layer_quad = compositor_layer_quad;
	c->base.base.layer_cube = compositor_layer_cube;
	c->base.base.layer_cylinder = compositor_layer_cylinder;
	c->base.base.layer_equirect1 = compositor_layer_equirect1;
	c->base.base.layer_equirect2 = compositor_layer_equirect2;
	c->base.base.layer_passthrough = compositor_layer_passthrough;
	c->base.base.layer_commit = compositor_layer_commit;
	c->base.base.layer_commit_with_semaphore = compositor_layer_commit_with_semaphore;
	c->base.base.destroy = compositor_destroy;

	// Set up supported formats
	// IMPORTANT: Store as VkFormat values, not DXGI! The IPC protocol and D3D11 client
	// compositor expect VkFormat values which they then convert to DXGI.
	// The d3d_dxgi_format_to_vk() function converts DXGI -> VkFormat.
	uint32_t format_count = 0;
	c->base.base.info.formats[format_count++] = d3d_dxgi_format_to_vk(DXGI_FORMAT_R8G8B8A8_UNORM);
	c->base.base.info.formats[format_count++] = d3d_dxgi_format_to_vk(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
	c->base.base.info.formats[format_count++] = d3d_dxgi_format_to_vk(DXGI_FORMAT_B8G8R8A8_UNORM);
	c->base.base.info.formats[format_count++] = d3d_dxgi_format_to_vk(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
	c->base.base.info.formats[format_count++] = d3d_dxgi_format_to_vk(DXGI_FORMAT_R16G16B16A16_FLOAT);
	c->base.base.info.format_count = format_count;

	// Log the formats being reported
	U_LOG_W("D3D11 service compositor: reporting %u VkFormat values to IPC client:", format_count);
	for (uint32_t i = 0; i < format_count; i++) {
		U_LOG_W("  format[%u] = %lld (VkFormat)", i, (long long)c->base.base.info.formats[i]);
	}

	// Set initial visibility/focus state (will be returned to client via IPC)
	// This avoids race condition where client must poll events before these are set
	c->base.base.info.initial_visible = true;
	c->base.base.info.initial_focused = true;

	U_LOG_W("D3D11 service: created native compositor for client");

	*out_xcn = &c->base;
	return XRT_SUCCESS;
}

static void
system_destroy(struct xrt_system_compositor *xsysc)
{
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);

	U_LOG_I("Destroying D3D11 service system compositor");

	// Clean up multi-compositor
	if (sys->multi_comp != nullptr) {
		multi_compositor_destroy(sys->multi_comp);
		sys->multi_comp = nullptr;
	}

	// NOTE: Per-client display processors are cleaned up in fini_client_render_resources()
	// when each client disconnects. System has no display processor anymore.

	// Clean up layer rendering resources
	sys->depth_test_enabled.reset();
	sys->depth_disabled.reset();
	sys->rasterizer_state.reset();
	sys->blend_opaque.reset();
	sys->blend_premul.reset();
	sys->blend_alpha.reset();
	sys->sampler_linear.reset();
	sys->sampler_point.reset();
	sys->layer_constant_buffer.reset();

	// Clean up layer shaders
	sys->cube_ps.reset();
	sys->cube_vs.reset();
	sys->equirect2_ps.reset();
	sys->equirect2_vs.reset();
	sys->cylinder_ps.reset();
	sys->cylinder_vs.reset();
	sys->quad_ps.reset();
	sys->quad_vs.reset();

	// Clean up blit shader resources
	sys->blit_constant_buffer.reset();
	sys->blit_ps.reset();
	sys->blit_vs.reset();

	// NOTE: Per-client resources (window, swap_chain, atlas_texture, display processor)
	// are cleaned up in fini_client_render_resources() when each client disconnects.
	// System only needs to clean up shared resources (device, shaders, etc.)

	u_mcp_capture_uninstall();
	u_mcp_capture_fini(&sys->mcp_capture);

	sys->dxgi_factory.reset();
	sys->context.reset();
	sys->device.reset();

	delete sys;
}


/*
 *
 * Exported functions
 *
 */

extern "C" xrt_result_t
comp_d3d11_service_create_system(struct xrt_device *xdev,
                                 struct xrt_system_devices *xsysd,
                                 struct u_system *usys,
                                 struct xrt_system_compositor **out_xsysc)
{
	U_LOG_W("Creating D3D11 service system compositor (xsysd=%p usys=%p)", (void *)xsysd, (void *)usys);

	// Allocate system compositor
	struct d3d11_service_system *sys = new d3d11_service_system();
	std::memset(&sys->base, 0, sizeof(sys->base));

	sys->xdev = xdev;
	sys->usys = usys;
	sys->log_level = U_LOGGING_INFO;
	sys->hardware_display_3d = true;
	sys->last_3d_mode_index = 1;

	// Default tile layout (stereo side-by-side) and display dimensions
	sys->tile_columns = 2;
	sys->tile_rows = 1;
	sync_tile_layout(sys);
	sys->output_width = 1920;
	sys->output_height = 1080;
	sys->view_width = sys->output_width / sys->tile_columns;
	sys->view_height = sys->output_height / sys->tile_rows;
	sys->display_width = sys->tile_columns * sys->view_width;
	sys->display_height = sys->tile_rows * sys->view_height;
	sys->refresh_rate = 60.0f;
	// NOTE: Display processor queries happen after D3D11 device creation (below).

	// Create D3D11 device (service owns this, independent of clients)
	UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
	flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

	D3D_FEATURE_LEVEL feature_levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	D3D_FEATURE_LEVEL actual_level;

	wil::com_ptr<ID3D11Device> device_base;
	wil::com_ptr<ID3D11DeviceContext> context_base;

	// Pin the service to the high-performance (discrete) GPU on hybrid laptops.
	// IPC shared textures cannot bridge two physical adapters, so picking dGPU
	// here keeps both the compositor and any well-behaved client (which honours
	// the LUID we publish below) on the same GPU.
	wil::com_ptr<IDXGIAdapter> preferred_adapter;
	{
		wil::com_ptr<IDXGIFactory6> factory6;
		if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory6.put()))) && factory6) {
			wil::com_ptr<IDXGIAdapter1> high_perf;
			if (SUCCEEDED(factory6->EnumAdapterByGpuPreference(
			        0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
			        IID_PPV_ARGS(high_perf.put())))) {
				DXGI_ADAPTER_DESC1 desc1{};
				if (SUCCEEDED(high_perf->GetDesc1(&desc1)) &&
				    (desc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
					preferred_adapter = high_perf;
					U_LOG_W("D3D11 service: preferring high-performance adapter '%ls'",
					        desc1.Description);
				}
			}
		}
	}

	// D3D11CreateDevice requires DRIVER_TYPE_UNKNOWN when an explicit adapter is supplied.
	HRESULT hr = D3D11CreateDevice(
	    preferred_adapter.get(),
	    preferred_adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
	    nullptr,
	    flags,
	    feature_levels, ARRAYSIZE(feature_levels),
	    D3D11_SDK_VERSION,
	    device_base.put(),
	    &actual_level,
	    context_base.put());

	if (FAILED(hr)) {
		U_LOG_E("Failed to create D3D11 device: 0x%08lx", hr);
		delete sys;
		return XRT_ERROR_VULKAN;  // Generic graphics error
	}

	// Enable D3D11 multithread protection: multiple IPC client threads + render thread
	// all share the same device. Without this, 3+ simultaneous clients crash (#108).
	{
		wil::com_ptr<ID3D11Multithread> mt;
		if (device_base.try_query_to(mt.put())) {
			mt->SetMultithreadProtected(TRUE);
			U_LOG_W("D3D11 multithread protection enabled");
		} else {
			U_LOG_W("D3D11 multithread protection not available");
		}
	}

	// Get ID3D11Device5 and ID3D11DeviceContext4 for shared resource support
	if (!device_base.try_query_to(sys->device.put())) {
		U_LOG_E("Device doesn't support ID3D11Device5");
		delete sys;
		return XRT_ERROR_VULKAN;
	}

	if (!context_base.try_query_to(sys->context.put())) {
		U_LOG_E("Context doesn't support ID3D11DeviceContext4");
		delete sys;
		return XRT_ERROR_VULKAN;
	}

	// Get DXGI factory
	wil::com_ptr<IDXGIDevice> dxgi_device;
	sys->device.try_query_to(dxgi_device.put());

	wil::com_ptr<IDXGIAdapter> adapter;
	dxgi_device->GetAdapter(adapter.put());

	// Get adapter LUID and set it in system info so clients use the same GPU
	DXGI_ADAPTER_DESC adapter_desc = {};
	hr = adapter->GetDesc(&adapter_desc);
	if (SUCCEEDED(hr)) {
		// Copy LUID to system info (LUID is 8 bytes: LowPart + HighPart)
		static_assert(sizeof(adapter_desc.AdapterLuid) == XRT_LUID_SIZE, "LUID size mismatch");
		std::memcpy(sys->base.info.client_d3d_deviceLUID.data, &adapter_desc.AdapterLuid, XRT_LUID_SIZE);
		sys->base.info.client_d3d_deviceLUID_valid = true;
		U_LOG_W("D3D11 service compositor using adapter LUID: %08lx-%08lx",
		        adapter_desc.AdapterLuid.HighPart, adapter_desc.AdapterLuid.LowPart);
	} else {
		U_LOG_W("Failed to get adapter LUID, D3D clients may use wrong GPU");
		sys->base.info.client_d3d_deviceLUID_valid = false;
	}

	hr = adapter->GetParent(IID_PPV_ARGS(sys->dxgi_factory.put()));
	if (FAILED(hr)) {
		U_LOG_E("Failed to get DXGI factory: 0x%08lx", hr);
		delete sys;
		return XRT_ERROR_VULKAN;
	}

	// Store system devices for passing to per-client windows
	sys->xsysd = xsysd;

	// Query display dimensions from display processor (if factory is available).
	// Create a temporary display processor with NULL window to query pixel info,
	// then destroy it. Per-client display processors are created later with real windows.
	if (sys->base.info.dp_factory_d3d11 != NULL) {
	}

	// Create layer shaders and resources for UI layer rendering
	// These are shared across all clients
	if (!create_layer_shaders(sys)) {
		U_LOG_W("Failed to create layer shaders, UI layers will not render");
		// Don't fail - projection layers will still work
	} else if (!create_layer_resources(sys)) {
		U_LOG_W("Failed to create layer resources, UI layers will not render");
		// Don't fail - projection layers will still work
	}

	// NOTE: Window, swap chain, and display processor are now created per-client
	// in system_create_native_compositor() -> init_client_render_resources()
	// This allows the IPC service to start without a window until a client connects.

	// Set up system compositor vtable
	sys->base.create_native_compositor = system_create_native_compositor;
	sys->base.destroy = system_destroy;

	// Set up multi-compositor control for session state management
	sys->xmcc.set_state = system_set_state;
	sys->xmcc.set_z_order = system_set_z_order;
	sys->xmcc.set_main_app_visibility = NULL;  // Not needed for single client
	sys->xmcc.notify_loss_pending = NULL;
	sys->xmcc.notify_lost = NULL;
	sys->xmcc.notify_display_refresh_changed = NULL;
	sys->base.xmcc = &sys->xmcc;

	// Fill system compositor info.
	//
	// Chrome's WebXR sizes its shared framebuffer by packing `view_count`
	// per-view slots horizontally:
	//   fb.width  = view_count × per_view.recommended.width
	//   fb.height = per_view.recommended.height
	// We want fb equal to the WORST-CASE ATLAS across all DP rendering
	// modes (independent max of width and height), so every mode's atlas
	// fits without fb re-allocation:
	//   atlas_w[mode] = tile_cols[mode] × view_width_pixels[mode]
	//                 = tile_cols[mode] × (display_w × viewScaleX[mode])
	//   atlas_h[mode] = tile_rows[mode] × view_height_pixels[mode]
	// Max over modes → per_view.width = max_atlas_w / view_count,
	//                  per_view.height = max_atlas_h.
	// view_count is whatever the HMD driver declares (2 for Leia, e.g. 5
	// for a hypothetical lightfield 3×2 mode).
	sys->base.info.max_layers = XRT_MAX_LAYERS;
	uint32_t view_count =
	    (xdev != nullptr && xdev->hmd != nullptr) ? xdev->hmd->view_count : 2;
	if (view_count == 0) view_count = 2;
	uint32_t max_atlas_w = sys->display_width;
	uint32_t max_atlas_h = sys->display_height;
	if (xdev != nullptr && xdev->rendering_mode_count > 0) {
		max_atlas_w = 0;
		max_atlas_h = 0;
		for (uint32_t i = 0; i < xdev->rendering_mode_count; i++) {
			uint32_t aw = xdev->rendering_modes[i].atlas_width_pixels;
			uint32_t ah = xdev->rendering_modes[i].atlas_height_pixels;
			if (aw > max_atlas_w) max_atlas_w = aw;
			if (ah > max_atlas_h) max_atlas_h = ah;
		}
	}
	const uint32_t per_view_w =
	    (view_count > 0) ? max_atlas_w / view_count : max_atlas_w;
	const uint32_t per_view_h = max_atlas_h;
	for (uint32_t i = 0; i < view_count && i < XRT_MAX_VIEWS; i++) {
		sys->base.info.views[i].recommended.width_pixels = per_view_w;
		sys->base.info.views[i].recommended.height_pixels = per_view_h;
		// max >= recommended, 2× headroom for framebufferScaleFactor > 1.
		sys->base.info.views[i].max.width_pixels = per_view_w * 2;
		sys->base.info.views[i].max.height_pixels = per_view_h * 2;
	}

	// Set supported blend modes (Chrome WebXR requires at least OPAQUE)
	sys->base.info.supported_blend_modes[0] = XRT_BLEND_MODE_OPAQUE;
	sys->base.info.supported_blend_mode_count = 1;

	// Populate display info for XR_EXT_display_info
	// (display_width_m and display_height_m are already set above from the temporary display processor query)
	if (sys->output_width > 0 && sys->output_height > 0) {
		sys->base.info.recommended_view_scale_x = (float)sys->view_width / (float)sys->output_width;
		sys->base.info.recommended_view_scale_y = (float)sys->view_height / (float)sys->output_height;
		sys->base.info.display_pixel_width = sys->output_width;
		sys->base.info.display_pixel_height = sys->output_height;
	}

	U_LOG_W("D3D11 service system compositor created: view=%ux%u, view_count=%u, "
	        "display=%ux%u, output=%ux%u @ %.0fHz",
	        sys->view_width, sys->view_height, view_count,
	        sys->display_width, sys->display_height,
	        sys->output_width, sys->output_height, sys->refresh_rate);

	u_mcp_capture_init(&sys->mcp_capture);
	u_mcp_capture_install(&sys->mcp_capture);

	*out_xsysc = &sys->base;
	return XRT_SUCCESS;
}

/*
 *
 * Helper functions for IPC server to get display processor data
 *
 */

bool
comp_d3d11_service_is_d3d11_service(struct xrt_system_compositor *xsysc)
{
	if (xsysc == NULL) {
		return false;
	}
	// Check by comparing function pointers - this identifies our compositor type
	bool is_d3d11_service = (xsysc->create_native_compositor == system_create_native_compositor);

	// Log first call for debugging
	static bool first_call = true;
	if (first_call) {
		first_call = false;
		U_LOG_W("comp_d3d11_service_is_d3d11_service: xsysc=%p, create_native_compositor=%p, expected=%p, match=%s",
		        (void*)xsysc,
		        (void*)xsysc->create_native_compositor,
		        (void*)system_create_native_compositor,
		        is_d3d11_service ? "YES" : "NO");
	}
	return is_d3d11_service;
}

bool
comp_d3d11_service_get_predicted_eye_positions(struct xrt_system_compositor *xsysc,
                                                struct xrt_vec3 *out_left,
                                                struct xrt_vec3 *out_right)
{
	if (xsysc == NULL || out_left == NULL || out_right == NULL) {
		return false;
	}

	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);

	// Get display processor for eye position prediction.
	// In workspace mode, use the multi-comp's DP (per-client compositors have no DP).
	// In normal mode, use the active compositor's DP.
	struct xrt_display_processor_d3d11 *dp = nullptr;
	if (sys->workspace_mode && sys->multi_comp != nullptr) {
		dp = sys->multi_comp->display_processor;
	}
	if (dp == nullptr) {
		std::lock_guard<std::mutex> lock(sys->active_compositor_mutex);
		if (sys->active_compositor != nullptr &&
		    sys->active_compositor->render.display_processor != nullptr) {
			dp = sys->active_compositor->render.display_processor;
		}
	}

	if (dp != nullptr) {
		struct xrt_eye_positions eyes;
		if (xrt_display_processor_d3d11_get_predicted_eye_positions(dp, &eyes) && eyes.valid) {
			out_left->x = eyes.eyes[0].x;
			out_left->y = eyes.eyes[0].y;
			out_left->z = eyes.eyes[0].z;
			out_right->x = eyes.eyes[1].x;
			out_right->y = eyes.eyes[1].y;
			out_right->z = eyes.eyes[1].z;

			// Log periodically for debugging
			static int log_counter = 0;
			if (++log_counter >= 60) {
				log_counter = 0;
				U_LOG_W("IPC eye positions (from display processor): L=(%.3f,%.3f,%.3f) R=(%.3f,%.3f,%.3f)",
				        out_left->x, out_left->y, out_left->z,
				        out_right->x, out_right->y, out_right->z);
			}
			return true;
		}
	}

	// Log if we have no active display processor
	static bool logged_no_dp = false;
	if (!logged_no_dp) {
		logged_no_dp = true;
		U_LOG_W("comp_d3d11_service_get_predicted_eye_positions: no active display processor available");
	}

	return false;
}

bool
comp_d3d11_service_capture_frame(struct xrt_system_compositor *xsysc,
                                 const char *path_prefix,
                                 uint32_t flags,
                                 struct ipc_capture_result *out_result)
{
	if (xsysc == nullptr || path_prefix == nullptr || out_result == nullptr || flags == 0) {
		return false;
	}

	memset(out_result, 0, sizeof(*out_result));

	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	struct d3d11_multi_compositor *mc = sys ? sys->multi_comp : nullptr;
	if (mc == nullptr || !mc->combined_atlas || sys->device == nullptr || sys->context == nullptr) {
		U_LOG_W("capture_frame: no combined atlas / device / context available");
		return false;
	}

	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);

	// Re-check under lock.
	if (!mc->combined_atlas) {
		return false;
	}

	D3D11_TEXTURE2D_DESC desc;
	mc->combined_atlas->GetDesc(&desc);
	const uint32_t atlas_w = desc.Width;
	const uint32_t atlas_h = desc.Height;
	const uint32_t tile_columns = sys->tile_columns > 0 ? sys->tile_columns : 1;
	const uint32_t tile_rows = sys->tile_rows > 0 ? sys->tile_rows : 1;
	// Crop to the active region: in non-legacy sessions each view occupies
	// view_width_pixels × view_height_pixels in the top-left of its tile
	// (e.g. 1920×1080 per eye in stereo SBS on 4K, leaving the rest black).
	// Legacy sessions use the full tile. Issue #158.
	uint32_t eye_w_res, eye_h_res;
	resolve_active_view_dims(sys, atlas_w, atlas_h, &eye_w_res, &eye_h_res);
	const uint32_t eye_w = eye_w_res;
	const uint32_t eye_h = eye_h_res;
	const uint32_t used_w = eye_w * tile_columns;
	const uint32_t used_h = eye_h * tile_rows;

	// Create CPU-readable staging texture and copy atlas into it.
	D3D11_TEXTURE2D_DESC sd = desc;
	sd.Usage = D3D11_USAGE_STAGING;
	sd.BindFlags = 0;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	sd.MiscFlags = 0;
	wil::com_ptr<ID3D11Texture2D> staging;
	HRESULT hr = sys->device->CreateTexture2D(&sd, nullptr, staging.put());
	if (FAILED(hr)) {
		U_LOG_W("capture_frame: CreateTexture2D(staging) failed 0x%08lx", hr);
		return false;
	}
	sys->context->CopyResource(staging.get(), mc->combined_atlas.get());

	D3D11_MAPPED_SUBRESOURCE m;
	hr = sys->context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m);
	if (FAILED(hr)) {
		U_LOG_W("capture_frame: Map(staging) failed 0x%08lx", hr);
		return false;
	}

	uint32_t views_written = 0;

	if (flags & IPC_CAPTURE_FLAG_ATLAS) {
		// Tightly-pack the active top-left region (used_w × used_h) into a
		// contiguous RGBA8 buffer. Drops the black padding outside the tile
		// grid and also handles staging RowPitch > used_w*4.
		std::vector<uint8_t> buf((size_t)used_w * used_h * 4u);
		const uint8_t *src = static_cast<const uint8_t *>(m.pData);
		for (uint32_t y = 0; y < used_h; y++) {
			memcpy(buf.data() + (size_t)y * used_w * 4u,
			       src + (size_t)y * m.RowPitch,
			       (size_t)used_w * 4u);
		}
		char path[MAX_PATH];
		snprintf(path, sizeof(path), "%s_atlas.png", path_prefix);
		if (stbi_write_png(path, (int)used_w, (int)used_h, 4,
		                   buf.data(), (int)(used_w * 4u)) != 0) {
			views_written |= IPC_CAPTURE_FLAG_ATLAS;
		} else {
			U_LOG_W("capture_frame: stbi_write_png failed for %s", path);
		}
	}

	sys->context->Unmap(staging.get(), 0);

	// Populate metadata. atlas_width/height report the cropped active region
	// (what was actually written to disk), not the full-display staging size.
	out_result->timestamp_ns = os_monotonic_get_ns();
	out_result->atlas_width = used_w;
	out_result->atlas_height = used_h;
	out_result->eye_width = eye_w;
	out_result->eye_height = eye_h;
	out_result->views_written = views_written;
	out_result->tile_columns = tile_columns;
	out_result->tile_rows = tile_rows;
	out_result->display_width_m = sys->base.info.display_width_m;
	out_result->display_height_m = sys->base.info.display_height_m;

	struct xrt_vec3 le = {0, 0, 0}, re = {0, 0, 0};
	if (comp_d3d11_service_get_predicted_eye_positions(xsysc, &le, &re)) {
		out_result->eye_left_m[0] = le.x;
		out_result->eye_left_m[1] = le.y;
		out_result->eye_left_m[2] = le.z;
		out_result->eye_right_m[0] = re.x;
		out_result->eye_right_m[1] = re.y;
		out_result->eye_right_m[2] = re.z;
	}

	U_LOG_W("capture_frame: prefix=%s flags=0x%x written=0x%x used=%ux%u (atlas=%ux%u) eye=%ux%u",
	        path_prefix, flags, views_written, used_w, used_h, atlas_w, atlas_h, eye_w, eye_h);

	return views_written != 0;
}

bool
comp_d3d11_service_get_display_dimensions(struct xrt_system_compositor *xsysc,
                                           float *out_width_m,
                                           float *out_height_m)
{
	if (xsysc == NULL || out_width_m == NULL || out_height_m == NULL) {
		return false;
	}

	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);

	// Try to get display dimensions from display processor.
	// In workspace mode, use multi-comp's DP; in normal mode, use active compositor's DP.
	struct xrt_display_processor_d3d11 *dp = nullptr;
	if (sys->workspace_mode && sys->multi_comp != nullptr) {
		dp = sys->multi_comp->display_processor;
	}
	if (dp == nullptr) {
		std::lock_guard<std::mutex> lock(sys->active_compositor_mutex);
		if (sys->active_compositor != nullptr &&
		    sys->active_compositor->render.display_processor != nullptr) {
			dp = sys->active_compositor->render.display_processor;
		}
	}

	if (dp != nullptr) {
		if (xrt_display_processor_d3d11_get_display_dimensions(dp, out_width_m, out_height_m)) {
			return true;
		}
	}

	// Fall back to values cached in system compositor info (set during init)
	if (sys->base.info.display_width_m > 0.0f && sys->base.info.display_height_m > 0.0f) {
		*out_width_m = sys->base.info.display_width_m;
		*out_height_m = sys->base.info.display_height_m;
		return true;
	}

	return false;
}

bool
comp_d3d11_service_get_window_metrics(struct xrt_system_compositor *xsysc,
                                       struct xrt_window_metrics *out_metrics)
{
	if (xsysc == nullptr || out_metrics == nullptr) {
		if (out_metrics != nullptr) {
			out_metrics->valid = false;
		}
		return false;
	}

	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);

	// In workspace mode, use multi-comp's window and DP.
	// In normal mode, use the active compositor's.
	struct xrt_display_processor_d3d11 *dp = nullptr;
	HWND metrics_hwnd = nullptr;

	if (sys->workspace_mode && sys->multi_comp != nullptr &&
	    sys->multi_comp->display_processor != nullptr && sys->multi_comp->hwnd != nullptr) {
		dp = sys->multi_comp->display_processor;
		metrics_hwnd = sys->multi_comp->hwnd;
	} else {
		struct d3d11_service_compositor *sc = nullptr;
		{
			std::lock_guard<std::mutex> lock(sys->active_compositor_mutex);
			sc = sys->active_compositor;
		}
		if (sc != nullptr && sc->render.hwnd != nullptr && sc->render.display_processor != nullptr) {
			dp = sc->render.display_processor;
			metrics_hwnd = sc->render.hwnd;
		}
	}

	if (dp == nullptr || metrics_hwnd == nullptr) {
		out_metrics->valid = false;
		return false;
	}

	// Get display pixel info from display processor
	uint32_t disp_px_w = 0, disp_px_h = 0;
	int32_t disp_left = 0, disp_top = 0;
	if (!xrt_display_processor_d3d11_get_display_pixel_info(
	        dp, &disp_px_w, &disp_px_h,
	        &disp_left, &disp_top)) {
		out_metrics->valid = false;
		return false;
	}

	if (disp_px_w == 0 || disp_px_h == 0) {
		out_metrics->valid = false;
		return false;
	}

	// Get physical display dimensions
	float disp_w_m = 0.0f, disp_h_m = 0.0f;
	if (!xrt_display_processor_d3d11_get_display_dimensions(dp, &disp_w_m, &disp_h_m)) {
		out_metrics->valid = false;
		return false;
	}

	// In non-workspace standalone mode (hot-switched), the app owns the full
	// display — use display dimensions directly. The DP renders to the full
	// display regardless of HWND decorations.
	// In workspace mode, this function isn't called (get_client_window_metrics
	// handles per-window Kooima).
	uint32_t win_px_w = disp_px_w;
	uint32_t win_px_h = disp_px_h;
	int32_t win_screen_left = disp_left;
	int32_t win_screen_top = disp_top;
	float win_w_m = disp_w_m;
	float win_h_m = disp_h_m;
	float offset_x_m = 0.0f;
	float offset_y_m = 0.0f;

	memset(out_metrics, 0, sizeof(*out_metrics));
	out_metrics->display_width_m = disp_w_m;
	out_metrics->display_height_m = disp_h_m;
	out_metrics->display_pixel_width = disp_px_w;
	out_metrics->display_pixel_height = disp_px_h;
	out_metrics->display_screen_left = disp_left;
	out_metrics->display_screen_top = disp_top;
	out_metrics->window_pixel_width = win_px_w;
	out_metrics->window_pixel_height = win_px_h;
	out_metrics->window_screen_left = win_screen_left;
	out_metrics->window_screen_top = win_screen_top;
	out_metrics->window_width_m = win_w_m;
	out_metrics->window_height_m = win_h_m;
	out_metrics->window_center_offset_x_m = offset_x_m;
	out_metrics->window_center_offset_y_m = offset_y_m;
	out_metrics->valid = true;

	return true;
}

/*!
 * Convert a slot's 3D window pose + dimensions to a pixel rect in the combined atlas.
 *
 * For Phase 1B (z=0, identity orientation) this is a direct meters-to-pixels conversion.
 * Future phases can handle perspective projection for depth/rotation.
 *
 * Convention: pose.position is in meters from display center, +X right, +Y up.
 * Pixel rect origin is top-left of display.
 */
static void
slot_pose_to_pixel_rect(const struct d3d11_service_system *sys,
                        const struct d3d11_multi_client_slot *slot,
                        int32_t *out_x, int32_t *out_y,
                        int32_t *out_w, int32_t *out_h)
{
	float disp_w_m = sys->base.info.display_width_m;
	float disp_h_m = sys->base.info.display_height_m;
	uint32_t disp_px_w = sys->base.info.display_pixel_width;
	uint32_t disp_px_h = sys->base.info.display_pixel_height;

	if (disp_w_m <= 0.0f || disp_h_m <= 0.0f || disp_px_w == 0 || disp_px_h == 0) {
		disp_px_w = 3840;
		disp_px_h = 2160;
		disp_w_m = 0.700f;
		disp_h_m = 0.394f;
	}

	float px_per_m_x = (float)disp_px_w / disp_w_m;
	float px_per_m_y = (float)disp_px_h / disp_h_m;

	int32_t w_px = (int32_t)(slot->window_width_m * px_per_m_x + 0.5f);
	int32_t h_px = (int32_t)(slot->window_height_m * px_per_m_y + 0.5f);

	float center_px_x = (float)disp_px_w / 2.0f + slot->window_pose.position.x * px_per_m_x;
	float center_px_y = (float)disp_px_h / 2.0f - slot->window_pose.position.y * px_per_m_y;

	// No clamping — windows can overflow off-screen (standard Windows behavior).
	// The mouse can't leave the screen, guaranteeing a visible portion.
	// The blit code clips to the visible area.
	*out_x = (int32_t)(center_px_x - (float)w_px / 2.0f + 0.5f);
	*out_y = (int32_t)(center_px_y - (float)h_px / 2.0f + 0.5f);
	*out_w = w_px;
	*out_h = h_px;
}

/*!
 * Project a window's pixel rect through an eye position to the display plane (Z=0).
 *
 * For Z=0 windows, identical to slot_pose_to_pixel_rect().
 * For Z != 0, computes parallax-shifted position and scale per-eye.
 * Used for per-eye rendering in SBS atlas (left/right halves get different rects).
 *
 * Math: project window center through eye onto Z=0 plane.
 *   scale = eye_z / (eye_z - win_z)     — closer windows appear larger
 *   proj_x = eye_x + scale * (win_x - eye_x)  — parallax shift
 */
static void
slot_pose_to_pixel_rect_for_eye(const struct d3d11_service_system *sys,
                                const struct d3d11_multi_client_slot *slot,
                                float eye_x, float eye_y, float eye_z,
                                int32_t *out_x, int32_t *out_y,
                                int32_t *out_w, int32_t *out_h)
{
	float disp_w_m = sys->base.info.display_width_m;
	float disp_h_m = sys->base.info.display_height_m;
	uint32_t disp_px_w = sys->base.info.display_pixel_width;
	uint32_t disp_px_h = sys->base.info.display_pixel_height;

	if (disp_w_m <= 0.0f || disp_h_m <= 0.0f || disp_px_w == 0 || disp_px_h == 0) {
		disp_px_w = 3840;
		disp_px_h = 2160;
		disp_w_m = 0.700f;
		disp_h_m = 0.394f;
	}

	float px_per_m_x = (float)disp_px_w / disp_w_m;
	float px_per_m_y = (float)disp_px_h / disp_h_m;

	float wx = slot->window_pose.position.x;
	float wy = slot->window_pose.position.y;
	float wz = slot->window_pose.position.z;
	float w_m = slot->window_width_m;
	float h_m = slot->window_height_m;

	// Project through eye to display plane (Z=0) for non-zero window Z
	if (fabsf(wz) > 0.0001f && eye_z > 0.01f) {
		float denom = eye_z - wz;
		if (fabsf(denom) < 0.001f) {
			denom = (denom >= 0.0f) ? 0.001f : -0.001f;
		}
		float scale = eye_z / denom;
		wx = eye_x + scale * (wx - eye_x);
		wy = eye_y + scale * (wy - eye_y);
		w_m *= scale;
		h_m *= scale;
	}

	int32_t w_px = (int32_t)(w_m * px_per_m_x + 0.5f);
	int32_t h_px = (int32_t)(h_m * px_per_m_y + 0.5f);

	float center_px_x = (float)disp_px_w / 2.0f + wx * px_per_m_x;
	float center_px_y = (float)disp_px_h / 2.0f - wy * px_per_m_y;

	*out_x = (int32_t)(center_px_x - (float)w_px / 2.0f + 0.5f);
	*out_y = (int32_t)(center_px_y - (float)h_px / 2.0f + 0.5f);
	*out_w = w_px;
	*out_h = h_px;
}

/*!
 * Project a single 3D point through an eye to the display plane (Z=0),
 * returning the result in display pixel coordinates.
 */
static inline void
project_point_for_eye(float px, float py, float pz,
                      float eye_x, float eye_y, float eye_z,
                      float disp_px_w, float disp_px_h,
                      float px_per_m_x, float px_per_m_y,
                      float *out_px_x, float *out_px_y)
{
	if (fabsf(pz) > 0.0001f && eye_z > 0.01f) {
		float denom = eye_z - pz;
		if (fabsf(denom) < 0.001f) denom = (denom >= 0.0f) ? 0.001f : -0.001f;
		float scale = eye_z / denom;
		px = eye_x + scale * (px - eye_x);
		py = eye_y + scale * (py - eye_y);
	}
	*out_px_x = disp_px_w / 2.0f + px * px_per_m_x;
	*out_px_y = disp_px_h / 2.0f - py * px_per_m_y;
}

/*!
 * Compute 4 projected corner positions (in SBS tile pixel coords) for a rotated window.
 * Corners are ordered: TL(0,0), BL(0,1), TR(1,0), BR(1,1) matching the blit VS triangle strip.
 *
 * For identity orientation, falls back to axis-aligned rect (returns false).
 * For non-identity, computes perspective-correct quad corners (returns true).
 */
static bool
compute_projected_quad_corners(const struct d3d11_service_system *sys,
                               const struct d3d11_multi_client_slot *slot,
                               float eye_x, float eye_y, float eye_z,
                               uint32_t tile_col, uint32_t tile_row,
                               uint32_t half_w, uint32_t half_h,
                               uint32_t ca_w, uint32_t ca_h,
                               float out_corners[8],
                               float out_w[4])
{
	if (quat_is_identity(&slot->window_pose.orientation)) {
		return false; // Use axis-aligned fast path
	}

	float disp_w_m = sys->base.info.display_width_m;
	float disp_h_m = sys->base.info.display_height_m;
	uint32_t disp_px_w = sys->base.info.display_pixel_width;
	uint32_t disp_px_h = sys->base.info.display_pixel_height;
	if (disp_w_m <= 0.0f || disp_h_m <= 0.0f || disp_px_w == 0 || disp_px_h == 0) {
		disp_px_w = 3840; disp_px_h = 2160;
		disp_w_m = 0.700f; disp_h_m = 0.394f;
	}
	float px_per_m_x = (float)disp_px_w / disp_w_m;
	float px_per_m_y = (float)disp_px_h / disp_h_m;

	float hw = slot->window_width_m / 2.0f;
	float hh = slot->window_height_m / 2.0f;

	// 4 corners in window-local space: TL, BL, TR, BR
	struct xrt_vec3 local[4] = {
		{-hw, +hh, 0}, // TL
		{-hw, -hh, 0}, // BL
		{+hw, +hh, 0}, // TR
		{+hw, -hh, 0}, // BR
	};

	const struct xrt_quat *q = &slot->window_pose.orientation;
	float wx = slot->window_pose.position.x;
	float wy = slot->window_pose.position.y;
	float wz = slot->window_pose.position.z;

	for (int i = 0; i < 4; i++) {
		struct xrt_vec3 world;
		math_quat_rotate_vec3(q, &local[i], &world);
		world.x += wx;
		world.y += wy;
		world.z += wz;

		// Depth from eye to corner (for perspective-correct interpolation)
		float depth = eye_z - world.z;
		if (depth < 0.01f) depth = 0.01f;
		out_w[i] = depth;

		float dpx, dpy;
		project_point_for_eye(world.x, world.y, world.z,
		                      eye_x, eye_y, eye_z,
		                      (float)disp_px_w, (float)disp_px_h,
		                      px_per_m_x, px_per_m_y,
		                      &dpx, &dpy);

		float frac_x = dpx / (float)ca_w;
		float frac_y = dpy / (float)ca_h;
		out_corners[i * 2 + 0] = tile_col * half_w + frac_x * half_w;
		out_corners[i * 2 + 1] = tile_row * half_h + frac_y * half_h;
	}
	return true;
}

/*!
 * Project an arbitrary local-space rectangle through a rotated window pose + eye to SBS tile pixels.
 * local coords: (-hw, -hh) = bottom-left, (+hw, +hh) = top-right, relative to window center.
 * Output corners are in the same order as the blit VS: TL(0), BL(1), TR(2), BR(3).
 */
static void
project_local_rect_for_eye(const struct d3d11_service_system *sys,
                           const struct xrt_quat *orientation,
                           float win_cx, float win_cy, float win_cz,
                           float local_left, float local_top,
                           float local_right, float local_bottom,
                           float eye_x, float eye_y, float eye_z,
                           uint32_t tile_col, uint32_t tile_row,
                           uint32_t half_w, uint32_t half_h,
                           uint32_t ca_w, uint32_t ca_h,
                           float out_corners[8],
                           float out_w[4])
{
	float disp_w_m = sys->base.info.display_width_m;
	float disp_h_m = sys->base.info.display_height_m;
	uint32_t disp_px_w = sys->base.info.display_pixel_width;
	uint32_t disp_px_h = sys->base.info.display_pixel_height;
	if (disp_w_m <= 0.0f || disp_h_m <= 0.0f || disp_px_w == 0 || disp_px_h == 0) {
		disp_px_w = 3840; disp_px_h = 2160;
		disp_w_m = 0.700f; disp_h_m = 0.394f;
	}
	float px_per_m_x = (float)disp_px_w / disp_w_m;
	float px_per_m_y = (float)disp_px_h / disp_h_m;

	// 4 corners in window-local space: TL, BL, TR, BR
	struct xrt_vec3 local[4] = {
		{local_left,  local_top,    0},
		{local_left,  local_bottom, 0},
		{local_right, local_top,    0},
		{local_right, local_bottom, 0},
	};

	for (int i = 0; i < 4; i++) {
		struct xrt_vec3 world;
		math_quat_rotate_vec3(orientation, &local[i], &world);
		world.x += win_cx;
		world.y += win_cy;
		world.z += win_cz;

		// Depth from eye to corner (for perspective-correct interpolation)
		float depth = eye_z - world.z;
		if (depth < 0.01f) depth = 0.01f;
		if (out_w) out_w[i] = depth;

		float dpx, dpy;
		project_point_for_eye(world.x, world.y, world.z,
		                      eye_x, eye_y, eye_z,
		                      (float)disp_px_w, (float)disp_px_h,
		                      px_per_m_x, px_per_m_y,
		                      &dpx, &dpy);

		float frac_x = dpx / (float)ca_w;
		float frac_y = dpy / (float)ca_h;
		out_corners[i * 2 + 0] = tile_col * half_w + frac_x * half_w;
		out_corners[i * 2 + 1] = tile_row * half_h + frac_y * half_h;
	}
}

// Phase 2.K: depth normalisation. Eye z is typically ~0.6 m from the display
// plane; window z spans roughly ±0.2 m around the plane (carousel back/front,
// edge-resize offsets). (eye_z - corner_z) is therefore in ~[0.4, 0.8] m.
// WORKSPACE_DEPTH_FAR_M = 1.0 m keeps depth_ndc in [0, 1] with plenty of
// resolution; WORKSPACE_CHROME_DEPTH_BIAS lets chrome bias millimetre-scale
// toward the eye so its own-window-content occlusion wins. Both #defined at
// the forward-declaration block near the top of the file.

// Phase 2.K: convert (eye_z - z_world) to NDC depth in [0, 1].
static inline float
workspace_depth_ndc_from_distance(float eye_to_z_distance)
{
	float d = eye_to_z_distance / WORKSPACE_DEPTH_FAR_M;
	if (d < 0.0f) d = 0.0f;
	if (d > 1.0f) d = 1.0f;
	return d;
}

// Phase 2.K: fill cb->corner_depth_ndc[4] for an axis-aligned (planar) blit.
// All four corners share the same depth value derived from window.z.
static inline void
blit_set_axis_aligned_depth(BlitConstants *cb, float eye_z, float window_z, float chrome_bias)
{
	float d = workspace_depth_ndc_from_distance(eye_z - window_z) - chrome_bias;
	if (d < 0.0f) d = 0.0f;
	if (d > 1.0f) d = 1.0f;
	cb->corner_depth_ndc[0] = d;
	cb->corner_depth_ndc[1] = d;
	cb->corner_depth_ndc[2] = d;
	cb->corner_depth_ndc[3] = d;
}

// Phase 2.K: fill cb->corner_depth_ndc[4] from per-corner W values that
// project_local_rect_for_eye() already computes (W = eye_z - corner_world_z).
// Each corner gets its own depth so two intersecting tilted quads occlude
// per-pixel via the hardware depth test.
static inline void
blit_set_perspective_depth(BlitConstants *cb, const float w[4], float chrome_bias)
{
	for (int i = 0; i < 4; i++) {
		float d = workspace_depth_ndc_from_distance(w[i]) - chrome_bias;
		if (d < 0.0f) d = 0.0f;
		if (d > 1.0f) d = 1.0f;
		cb->corner_depth_ndc[i] = d;
	}
}

/*!
 * Helper: write quad corner data into a BlitConstants struct.
 */
static inline void
blit_set_quad_corners(BlitConstants *cb, const float corners[8], const float w[4])
{
	cb->quad_mode = 1.0f;
	cb->quad_corners_01[0] = corners[0]; // TL.x
	cb->quad_corners_01[1] = corners[1]; // TL.y
	cb->quad_corners_01[2] = corners[2]; // BL.x
	cb->quad_corners_01[3] = corners[3]; // BL.y
	cb->quad_corners_23[0] = corners[4]; // TR.x
	cb->quad_corners_23[1] = corners[5]; // TR.y
	cb->quad_corners_23[2] = corners[6]; // BR.x
	cb->quad_corners_23[3] = corners[7]; // BR.y
	if (w) {
		cb->quad_w[0] = w[0]; // TL
		cb->quad_w[1] = w[1]; // BL
		cb->quad_w[2] = w[2]; // TR
		cb->quad_w[3] = w[3]; // BR
	} else {
		cb->quad_w[0] = cb->quad_w[1] = cb->quad_w[2] = cb->quad_w[3] = 1.0f;
	}
}

/*!
 * Find the multi-comp slot index for a given per-client compositor.
 * Returns -1 if not found.
 */
static int
multi_comp_find_slot(const struct d3d11_multi_compositor *mc,
                     const struct d3d11_service_compositor *c)
{
	if (mc == nullptr || c == nullptr) {
		return -1;
	}
	for (int i = 0; i < D3D11_MULTI_MAX_CLIENTS; i++) {
		if (mc->clients[i].active && mc->clients[i].compositor == c) {
			return i;
		}
	}
	return -1;
}

bool
comp_d3d11_service_set_client_window_pose(struct xrt_system_compositor *xsysc,
                                           struct xrt_compositor *xc,
                                           const struct xrt_pose *pose,
                                           float width_m,
                                           float height_m)
{
	if (xsysc == nullptr || xc == nullptr || pose == nullptr) {
		return false;
	}

	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	if (!sys->workspace_mode || sys->multi_comp == nullptr) {
		return false;
	}

	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);
	struct d3d11_multi_compositor *mc = sys->multi_comp;

	// Clamp dimensions to minimum 5% of display
	float min_dim = 0.02f; // ~2cm minimum
	if (width_m < min_dim) width_m = min_dim;
	if (height_m < min_dim) height_m = min_dim;

	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);

	int slot = multi_comp_find_slot(mc, c);
	if (slot < 0) {
		return false;
	}

	mc->clients[slot].window_pose = *pose;
	mc->clients[slot].window_width_m = width_m;
	mc->clients[slot].window_height_m = height_m;

	// Cancel any in-flight slot animation (e.g. entry animation from
	// register_client). Without this, slot_animate_tick keeps interpolating
	// from the animation's original start→target every frame and overwrites
	// the pose we just snapped, so the controller's set_pose appears to do
	// nothing for ~300ms after a fresh connect.
	mc->clients[slot].anim.active = false;

	// Recompute pixel rect from pose
	slot_pose_to_pixel_rect(sys, &mc->clients[slot],
	                        &mc->clients[slot].window_rect_x,
	                        &mc->clients[slot].window_rect_y,
	                        &mc->clients[slot].window_rect_w,
	                        &mc->clients[slot].window_rect_h);

	mc->clients[slot].hwnd_resize_pending = true;

	U_LOG_W("Workspace: set window pose slot %d pos=(%.3f,%.3f,%.3f) size=%.3fx%.3f → rect=(%u,%u,%u,%u)",
	        slot, pose->position.x, pose->position.y, pose->position.z,
	        width_m, height_m,
	        mc->clients[slot].window_rect_x, mc->clients[slot].window_rect_y,
	        mc->clients[slot].window_rect_w, mc->clients[slot].window_rect_h);

	return true;
}

bool
comp_d3d11_service_set_client_visibility(struct xrt_system_compositor *xsysc,
                                          struct xrt_compositor *xc,
                                          bool visible)
{
	if (xsysc == nullptr || xc == nullptr) return false;

	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	if (!sys->workspace_mode || sys->multi_comp == nullptr) return false;

	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);
	struct d3d11_multi_compositor *mc = sys->multi_comp;

	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);

	int slot = multi_comp_find_slot(mc, c);
	if (slot < 0) return false;

	mc->clients[slot].minimized = !visible;
	U_LOG_W("Workspace: set_visibility slot %d visible=%d", slot, visible);

	if (!visible && slot == mc->focused_slot) {
		mc->focused_slot = -1;
		for (int i = 0; i < D3D11_MULTI_MAX_CLIENTS; i++) {
			if (mc->clients[i].active && !mc->clients[i].minimized) {
				mc->focused_slot = i;
				break;
			}
		}
		multi_compositor_update_input_forward(mc);
	}
	return true;
}

bool
comp_d3d11_service_get_client_window_pose(struct xrt_system_compositor *xsysc,
                                           struct xrt_compositor *xc,
                                           struct xrt_pose *out_pose,
                                           float *out_width_m,
                                           float *out_height_m)
{
	if (xsysc == nullptr || xc == nullptr) return false;

	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	if (!sys->workspace_mode || sys->multi_comp == nullptr) return false;

	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);
	struct d3d11_multi_compositor *mc = sys->multi_comp;

	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);

	int slot = multi_comp_find_slot(mc, c);
	if (slot < 0) return false;

	*out_pose = mc->clients[slot].window_pose;
	*out_width_m = mc->clients[slot].window_width_m;
	*out_height_m = mc->clients[slot].window_height_m;
	return true;
}

bool
comp_d3d11_service_get_client_window_metrics(struct xrt_system_compositor *xsysc,
                                              struct xrt_compositor *xc,
                                              struct xrt_window_metrics *out_metrics)
{
	if (xsysc == nullptr || xc == nullptr || out_metrics == nullptr) {
		if (out_metrics != nullptr) {
			out_metrics->valid = false;
		}
		return false;
	}

	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	if (!sys->workspace_mode || sys->multi_comp == nullptr) {
		out_metrics->valid = false;
		return false;
	}

	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);
	struct d3d11_multi_compositor *mc = sys->multi_comp;

	int slot_idx = multi_comp_find_slot(mc, c);
	if (slot_idx < 0) {
		out_metrics->valid = false;
		return false;
	}

	const struct d3d11_multi_client_slot *slot = &mc->clients[slot_idx];

	// Get display physical dimensions
	float disp_w_m = sys->base.info.display_width_m;
	float disp_h_m = sys->base.info.display_height_m;
	uint32_t disp_px_w = sys->base.info.display_pixel_width;
	uint32_t disp_px_h = sys->base.info.display_pixel_height;

	if (disp_w_m <= 0.0f || disp_h_m <= 0.0f || disp_px_w == 0 || disp_px_h == 0) {
		out_metrics->valid = false;
		return false;
	}

	// Get display screen position from DP (needed for display_screen_left/top)
	int32_t disp_left = 0, disp_top = 0;
	if (mc->display_processor != nullptr) {
		xrt_display_processor_d3d11_get_display_pixel_info(
		    mc->display_processor, NULL, NULL, &disp_left, &disp_top);
	}

	// Virtual window center offset from display center (directly from pose)
	// pose.position.x: +X right (meters), pose.position.y: +Y up (meters)
	float offset_x_m = slot->window_pose.position.x;
	float offset_y_m = slot->window_pose.position.y;
	float offset_z_m = slot->window_pose.position.z;

	memset(out_metrics, 0, sizeof(*out_metrics));
	out_metrics->display_width_m = disp_w_m;
	out_metrics->display_height_m = disp_h_m;
	out_metrics->display_pixel_width = disp_px_w;
	out_metrics->display_pixel_height = disp_px_h;
	out_metrics->display_screen_left = disp_left;
	out_metrics->display_screen_top = disp_top;
	out_metrics->window_pixel_width = slot->window_rect_w;
	out_metrics->window_pixel_height = slot->window_rect_h;
	out_metrics->window_screen_left = disp_left + (int32_t)slot->window_rect_x;
	out_metrics->window_screen_top = disp_top + (int32_t)slot->window_rect_y;
	out_metrics->window_width_m = slot->window_width_m;
	out_metrics->window_height_m = slot->window_height_m;
	out_metrics->window_center_offset_x_m = offset_x_m;
	out_metrics->window_center_offset_y_m = offset_y_m;
	out_metrics->window_center_offset_z_m = offset_z_m;
	out_metrics->window_orientation = slot->window_pose.orientation;
	out_metrics->valid = true;

	return true;
}

bool
comp_d3d11_service_owns_window(struct xrt_system_compositor *xsysc)
{
	// Workspace mode: per-client compositors don't own windows (multi-comp does).
	// The workspace app provides its own HWND and does its own Kooima projection.
	// Returning false ensures the IPC view pose path uses the display-centric
	// Kooima with real DP eye tracking, not the camera-centric qwerty path.
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	if (sys->workspace_mode) {
		return false;
	}

	// Non-workspace mode: check the active compositor's actual ownership.
	// After hot-switch, handle apps still use their external HWND
	// (owns_window=false), not a Monado-owned one. This must return false
	// so the IPC view pose path takes the display-centric branch.
	struct d3d11_service_compositor *sc = nullptr;
	{
		std::lock_guard<std::mutex> lock(sys->active_compositor_mutex);
		sc = sys->active_compositor;
	}
	if (sc != nullptr) {
		return sc->render.owns_window;
	}

	// No active compositor — assume Monado-owned (default standalone)
	return true;
}

bool
comp_d3d11_service_window_is_valid(struct xrt_system_compositor *xsysc)
{
	// NOTE: With per-client windows, window validity is now per-client.
	// The IPC server no longer needs a single window validity check.
	// Each client's window lifecycle is handled when the client disconnects.
	// Always return true - the service doesn't maintain a global window anymore.
	(void)xsysc;
	return true;
}


/*
 *
 * Capture client public API (Phase 4A)
 *
 */

int
comp_d3d11_service_add_capture_client(struct xrt_system_compositor *xsysc,
                                       uint64_t hwnd_value,
                                       const char *name)
{
	if (xsysc == nullptr) {
		return -1;
	}

	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);

	// Activate workspace_mode from base.info if not already done.
	// Normally this happens on first IPC client connect, but capture
	// clients may arrive before any IPC client.
	if (!sys->workspace_mode && sys->base.info.workspace_mode) {
		service_set_workspace_mode(sys, true);
		U_LOG_W("Workspace mode activated for D3D11 service system (via capture client)");
	}
	if (!sys->workspace_mode) {
		U_LOG_E("Workspace: add_capture_client — workspace mode not active");
		return -1;
	}

	HWND hwnd = (HWND)(uintptr_t)hwnd_value;
	if (!IsWindow(hwnd)) {
		U_LOG_E("Workspace: add_capture_client — invalid HWND=0x%llx", (unsigned long long)hwnd_value);
		return -1;
	}

	// Ensure multi-compositor is initialized (it's normally created lazily
	// on first IPC client layer_commit, but capture clients may arrive first).
	xrt_result_t ret = multi_compositor_ensure_output(sys);
	if (ret != XRT_SUCCESS || sys->multi_comp == nullptr) {
		U_LOG_E("Workspace: add_capture_client — failed to init multi-compositor (ret=%d)",
		         (int)ret);
		return -1;
	}

	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);
	return multi_compositor_add_capture_client(sys, hwnd, name);
}

bool
comp_d3d11_service_remove_capture_client(struct xrt_system_compositor *xsysc,
                                          int slot_index)
{
	if (xsysc == nullptr) {
		return false;
	}

	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	if (!sys->workspace_mode || sys->multi_comp == nullptr) {
		return false;
	}

	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);
	return multi_compositor_remove_capture_client(sys, slot_index);
}

// Map the internal workspace_hit_result flag set onto the public
// XrWorkspaceHitRegionEXT enum. Chrome buttons take precedence over the
// title-bar bit (the WndProc geometry computes both for chrome hits).
// Compound edge flags collapse to the diagonal corners.
static uint32_t
hit_result_to_region(const struct workspace_hit_result &hit)
{
	// XrWorkspaceHitRegionEXT values — kept in sync with the public header.
	// Using literal ints here because comp_d3d11_service does not include
	// the OpenXR extension header (different layer).
	enum {
		REGION_BACKGROUND       = 0,
		REGION_CONTENT          = 1,
		REGION_TITLE_BAR        = 2,
		REGION_CLOSE_BUTTON     = 3,
		REGION_MINIMIZE_BUTTON  = 4,
		REGION_MAXIMIZE_BUTTON  = 5,
		REGION_EDGE_RESIZE_N    = 10,
		REGION_EDGE_RESIZE_S    = 11,
		REGION_EDGE_RESIZE_E    = 12,
		REGION_EDGE_RESIZE_W    = 13,
		REGION_EDGE_RESIZE_NE   = 14,
		REGION_EDGE_RESIZE_NW   = 15,
		REGION_EDGE_RESIZE_SE   = 16,
		REGION_EDGE_RESIZE_SW   = 17,
	};

	if (hit.slot < 0) {
		return REGION_BACKGROUND;
	}
	if (hit.in_close_btn)    return REGION_CLOSE_BUTTON;
	if (hit.in_minimize_btn) return REGION_MINIMIZE_BUTTON;
	if (hit.in_maximize_btn) return REGION_MAXIMIZE_BUTTON;

	const int e = hit.edge_flags;
	if (e != RESIZE_NONE) {
		// Diagonals first (compound).
		if ((e & RESIZE_TOP)    && (e & RESIZE_LEFT))  return REGION_EDGE_RESIZE_NW;
		if ((e & RESIZE_TOP)    && (e & RESIZE_RIGHT)) return REGION_EDGE_RESIZE_NE;
		if ((e & RESIZE_BOTTOM) && (e & RESIZE_LEFT))  return REGION_EDGE_RESIZE_SW;
		if ((e & RESIZE_BOTTOM) && (e & RESIZE_RIGHT)) return REGION_EDGE_RESIZE_SE;
		if (e & RESIZE_TOP)    return REGION_EDGE_RESIZE_N;
		if (e & RESIZE_BOTTOM) return REGION_EDGE_RESIZE_S;
		if (e & RESIZE_LEFT)   return REGION_EDGE_RESIZE_W;
		if (e & RESIZE_RIGHT)  return REGION_EDGE_RESIZE_E;
	}
	if (hit.in_title_bar) return REGION_TITLE_BAR;
	if (hit.in_content)   return REGION_CONTENT;

	// In-window but no specific region matched — treat as background-equivalent
	// rather than fabricate a category.
	return REGION_BACKGROUND;
}

extern "C" bool
comp_d3d11_service_workspace_drain_input_events(struct xrt_system_compositor *xsysc,
                                                 uint32_t capacity,
                                                 struct ipc_workspace_input_event_batch *out_batch)
{
	if (xsysc == nullptr || out_batch == nullptr) {
		return false;
	}
	out_batch->count = 0;

	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (!sys->workspace_mode || mc == nullptr || mc->window == nullptr) {
		return true; // No workspace active — zero events, success.
	}

	// Drain at most batch-max raw events from the window-side ring. The
	// public batch struct caps at IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX (16);
	// caller-requested capacity may be smaller.
	uint32_t want = capacity < IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX
	                  ? capacity
	                  : IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX;
	if (want == 0) {
		return true;
	}

	struct workspace_public_event_raw raw[IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX];
	uint32_t got = comp_d3d11_window_consume_workspace_public_events(mc->window, raw, want);

	// Phase 2.K: even when the WndProc-side ring is empty, FRAME_TICK and
	// FOCUS_CHANGED events still need to drain — controllers pace their
	// animation tick on FRAME_TICK alone when no user input is happening.
	// The raw-event loop only runs when there is something to translate.
	if (got > 0) {
		std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);

		for (uint32_t i = 0; i < got; i++) {
		const struct workspace_public_event_raw *r = &raw[i];
		struct ipc_workspace_input_event *ev = &out_batch->events[out_batch->count];
		memset(ev, 0, sizeof(*ev));
		ev->event_type = r->kind;
		ev->timestamp_ms = r->timestamp_ms;

		switch (r->kind) {
		case WORKSPACE_PUBLIC_EVENT_POINTER: {
			ev->u.pointer.button = r->button_or_vk;
			ev->u.pointer.is_down = r->is_down;
			ev->u.pointer.modifiers = r->modifiers;
			ev->u.pointer.cursor_x = (int64_t)r->cursor_x;
			ev->u.pointer.cursor_y = (int64_t)r->cursor_y;

			// Enrich with hit-test (run inside the same render-mutex
			// region so geometry is stable across the batch).
			POINT pt = {(LONG)r->cursor_x, (LONG)r->cursor_y};
			struct workspace_hit_result hit = workspace_raycast_hit_test(sys, mc, pt);
			ev->u.pointer.hit_region = hit_result_to_region(hit);
			if (hit.slot >= 0) {
				ev->u.pointer.hit_client_id = 1000u + (uint32_t)hit.slot;
				if (hit.in_content) {
					float win_w = mc->clients[hit.slot].window_width_m;
					float win_h = mc->clients[hit.slot].window_height_m;
					if (win_w > 0.0f) ev->u.pointer.local_u = hit.local_x_m / win_w;
					if (win_h > 0.0f) ev->u.pointer.local_v = hit.local_y_m / win_h;
				}
			}
			break;
		}
		case WORKSPACE_PUBLIC_EVENT_KEY:
			ev->u.key.vk_code = r->button_or_vk;
			ev->u.key.is_down = r->is_down;
			ev->u.key.modifiers = r->modifiers;
			break;
		case WORKSPACE_PUBLIC_EVENT_SCROLL:
			ev->u.scroll.delta_y = r->scroll_delta_y;
			ev->u.scroll.cursor_x = (int64_t)r->cursor_x;
			ev->u.scroll.cursor_y = (int64_t)r->cursor_y;
			ev->u.scroll.modifiers = r->modifiers;
			break;
		case WORKSPACE_PUBLIC_EVENT_MOTION: {
			// Phase 2.K: per-frame WM_MOUSEMOVE while pointer capture is
			// enabled. Same hit-test enrichment as POINTER (within the
			// same render_mutex region so geometry is stable across the
			// batch).
			ev->u.pointer_motion.button_mask = r->button_or_vk;
			ev->u.pointer_motion.modifiers = r->modifiers;
			ev->u.pointer_motion.cursor_x = (int64_t)r->cursor_x;
			ev->u.pointer_motion.cursor_y = (int64_t)r->cursor_y;
			POINT pt = {(LONG)r->cursor_x, (LONG)r->cursor_y};
			struct workspace_hit_result hit = workspace_raycast_hit_test(sys, mc, pt);
			ev->u.pointer_motion.hit_region = hit_result_to_region(hit);
			if (hit.slot >= 0) {
				ev->u.pointer_motion.hit_client_id = 1000u + (uint32_t)hit.slot;
				if (hit.in_content) {
					float win_w = mc->clients[hit.slot].window_width_m;
					float win_h = mc->clients[hit.slot].window_height_m;
					if (win_w > 0.0f)
						ev->u.pointer_motion.local_u = hit.local_x_m / win_w;
					if (win_h > 0.0f)
						ev->u.pointer_motion.local_v = hit.local_y_m / win_h;
				}
			}
			break;
		}
		default:
			continue; // skip unknown event kinds
		}
		out_batch->count++;
		}
	}

	// Phase 2.K: FOCUS_CHANGED + FRAME_TICK are emitted on every drain, even
	// when the WndProc-side ring is empty — controllers need to see them to
	// pace animations during idle periods. They read atomic fields and don't
	// require the render_mutex.

	// FOCUS_CHANGED: single event per drain on focused-slot transition.
	// Emitted after raw events so the controller sees the pointer event that
	// caused the focus change first.
	if (mc->focused_slot != mc->focused_slot_last_emitted &&
	    out_batch->count < IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX) {
		struct ipc_workspace_input_event *ev = &out_batch->events[out_batch->count];
		memset(ev, 0, sizeof(*ev));
		ev->event_type = IPC_WORKSPACE_INPUT_EVENT_FOCUS_CHANGED;
		ev->timestamp_ms = (uint32_t)GetTickCount();
		ev->u.focus_changed.prev_client_id =
		    (mc->focused_slot_last_emitted >= 0) ? (1000u + (uint32_t)mc->focused_slot_last_emitted) : 0;
		ev->u.focus_changed.curr_client_id =
		    (mc->focused_slot >= 0) ? (1000u + (uint32_t)mc->focused_slot) : 0;
		mc->focused_slot_last_emitted = mc->focused_slot;
		out_batch->count++;
	}

	// FRAME_TICK: one per displayed frame since the last drain, capped at
	// remaining batch capacity. If the controller drains faster than one
	// frame this is a no-op; if it falls behind we cap and the timestamp on
	// the most recent event is what matters for pacing.
	LONG cur = InterlockedCompareExchange(&mc->frame_tick_count, 0, 0);
	uint64_t now_ns = os_monotonic_get_ns();
	if (cur != mc->frame_tick_last_emitted) {
		LONG missed = cur - mc->frame_tick_last_emitted;
		if (missed < 0) missed = 1; // wrap; shouldn't happen with LONG counter
		while (missed > 0 && out_batch->count < IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX) {
			struct ipc_workspace_input_event *ev = &out_batch->events[out_batch->count];
			memset(ev, 0, sizeof(*ev));
			ev->event_type = IPC_WORKSPACE_INPUT_EVENT_FRAME_TICK;
			ev->timestamp_ms = (uint32_t)GetTickCount();
			ev->u.frame_tick.timestamp_ns = now_ns;
			out_batch->count++;
			missed--;
		}
		mc->frame_tick_last_emitted = cur;
		mc->frame_tick_last_ns = now_ns;
	}

	return true;
}

extern "C" bool
comp_d3d11_service_workspace_pointer_capture_set(struct xrt_system_compositor *xsysc,
                                                  bool enabled,
                                                  uint32_t button)
{
	if (xsysc == nullptr) {
		return false;
	}
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (mc == nullptr || mc->window == nullptr) {
		// Workspace not active — no-op success so callers don't need to
		// special-case lifecycle ordering.
		return true;
	}
	comp_d3d11_window_set_workspace_pointer_capture(mc->window, enabled, button);
	return true;
}

// Phase 2.K: targeted exit / fullscreen requests. Mirror the existing DELETE
// and F11 keyboard shortcuts but accept any slot (not only the focused one)
// so a controller can drive these from chrome / overview / scripted UI.
//
// Two entry points: a slot-based one used by the inline DELETE/F11 handlers
// and a client_id-based one used by the IPC handler. The IPC handler resolves
// capture-client client_ids (>= 1000) to slots directly and OpenXR-client
// client_ids by looking up the IPC thread table for the matching xrt_compositor
// — same pattern as workspace_set_window_pose.
//
// Helpers return XRT_ERROR_IPC_FAILURE on miss so the OpenXR layer maps it to
// XR_ERROR_HANDLE_INVALID at the API boundary.
extern "C" xrt_result_t
comp_d3d11_service_workspace_request_exit_by_slot(struct xrt_system_compositor *xsysc, int slot)
{
	if (xsysc == nullptr) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (!sys->workspace_mode || mc == nullptr) {
		return XRT_ERROR_IPC_FAILURE;
	}
	if (slot < 0 || slot >= D3D11_MULTI_MAX_CLIENTS || !mc->clients[slot].active) {
		return XRT_ERROR_IPC_FAILURE;
	}

	if (mc->clients[slot].client_type == CLIENT_TYPE_CAPTURE) {
		multi_compositor_remove_capture_client(sys, slot);
		U_LOG_W("Workspace: request_exit → removed capture slot %d", slot);
	} else {
		struct d3d11_service_compositor *fc = mc->clients[slot].compositor;
		if (fc == nullptr || fc->xses == nullptr) {
			return XRT_ERROR_IPC_FAILURE;
		}
		union xrt_session_event xse = XRT_STRUCT_INIT;
		xse.type = XRT_SESSION_EVENT_EXIT_REQUEST;
		xrt_session_event_sink_push(fc->xses, &xse);
		U_LOG_W("Workspace: request_exit → exit request for slot %d", slot);
	}
	return XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_d3d11_service_workspace_request_fullscreen_by_slot(struct xrt_system_compositor *xsysc,
                                                        int slot,
                                                        bool fullscreen)
{
	if (xsysc == nullptr) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (!sys->workspace_mode || mc == nullptr) {
		return XRT_ERROR_IPC_FAILURE;
	}
	if (slot < 0 || slot >= D3D11_MULTI_MAX_CLIENTS || !mc->clients[slot].active) {
		return XRT_ERROR_IPC_FAILURE;
	}

	bool currently_max = mc->clients[slot].maximized;
	if (fullscreen != currently_max) {
		toggle_fullscreen(sys, mc, slot);
	}
	return XRT_SUCCESS;
}

// Look up the slot bound to a given xrt_compositor (OpenXR client). Used by
// the IPC handler when it has translated client_id → ics->xc and needs the
// matching multi-compositor slot. Returns -1 on miss.
extern "C" int
comp_d3d11_service_workspace_find_slot_by_xc(struct xrt_system_compositor *xsysc, struct xrt_compositor *xc)
{
	if (xsysc == nullptr || xc == nullptr) {
		return -1;
	}
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (mc == nullptr) {
		return -1;
	}
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);
	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);
	return multi_comp_find_slot(mc, c);
}

// IPC-facing wrappers (signatures from comp_d3d11_service.h). Resolve
// capture-client ids directly; OpenXR-client ids must already be translated
// to a slot before calling — the IPC handler in ipc_server_handler.c uses
// comp_d3d11_service_workspace_find_slot_by_xc for that.
extern "C" xrt_result_t
comp_d3d11_service_workspace_request_client_exit(struct xrt_system_compositor *xsysc, uint32_t client_id)
{
	if (client_id >= 1000u) {
		return comp_d3d11_service_workspace_request_exit_by_slot(xsysc, (int)(client_id - 1000u));
	}
	// IPC handler path goes through find_slot_by_xc + request_exit_by_slot.
	// This entry point is a fallback that only handles capture clients.
	return XRT_ERROR_IPC_FAILURE;
}

extern "C" xrt_result_t
comp_d3d11_service_workspace_request_client_fullscreen(struct xrt_system_compositor *xsysc,
                                                       uint32_t client_id,
                                                       bool fullscreen)
{
	if (client_id >= 1000u) {
		return comp_d3d11_service_workspace_request_fullscreen_by_slot(
		    xsysc, (int)(client_id - 1000u), fullscreen);
	}
	return XRT_ERROR_IPC_FAILURE;
}

// Phase 2.C: chrome swapchain registration + layout setter.
// The IPC handler resolves (client_id, controller-side swapchain_id) → (slot,
// xrt_swapchain*) and calls these by-slot helpers. The runtime stores a
// strong ref to the swapchain and reads its image[0] SRV every render.

extern "C" xrt_result_t
comp_d3d11_service_workspace_register_chrome_swapchain_by_slot(struct xrt_system_compositor *xsysc,
                                                               int slot,
                                                               uint32_t swapchain_id,
                                                               struct xrt_swapchain *chrome_xsc)
{
	if (xsysc == nullptr || chrome_xsc == nullptr || swapchain_id == 0) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (mc == nullptr || slot < 0 || slot >= D3D11_MULTI_MAX_CLIENTS) {
		return XRT_ERROR_IPC_FAILURE;
	}

	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);
	struct d3d11_multi_client_slot *cs = &mc->clients[slot];
	if (!cs->active) {
		// Slot may bind a few ticks after the controller calls the create
		// RPC. The shell retries on XR_ERROR_HANDLE_INVALID; for now we
		// just refuse and let it retry.
		return XRT_ERROR_IPC_FAILURE;
	}

	// Drop any prior registration on this slot (replaces, doesn't stack).
	if (cs->chrome_xsc != nullptr) {
		xrt_swapchain_reference(&cs->chrome_xsc, NULL);
		cs->chrome_swapchain_id = 0;
	}

	xrt_swapchain_reference(&cs->chrome_xsc, chrome_xsc);
	cs->chrome_swapchain_id = swapchain_id;

	U_LOG_W("workspace: chrome swapchain registered for slot %d (id=%u)", slot, swapchain_id);
	return XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_d3d11_service_workspace_unregister_chrome_swapchain(struct xrt_system_compositor *xsysc,
                                                         uint32_t swapchain_id)
{
	if (xsysc == nullptr || swapchain_id == 0) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (mc == nullptr) {
		return XRT_SUCCESS; // workspace inactive — nothing to unregister
	}

	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);
	for (int s = 0; s < D3D11_MULTI_MAX_CLIENTS; s++) {
		struct d3d11_multi_client_slot *cs = &mc->clients[s];
		if (cs->chrome_swapchain_id == swapchain_id) {
			xrt_swapchain_reference(&cs->chrome_xsc, NULL);
			cs->chrome_swapchain_id = 0;
			cs->chrome_layout_valid = false;
			U_LOG_W("workspace: chrome swapchain unregistered (slot=%d, id=%u)", s, swapchain_id);
			return XRT_SUCCESS;
		}
	}
	return XRT_SUCCESS; // not registered — idempotent
}

extern "C" xrt_result_t
comp_d3d11_service_workspace_set_chrome_layout_by_slot(struct xrt_system_compositor *xsysc,
                                                       int slot,
                                                       const struct ipc_workspace_chrome_layout *layout)
{
	if (xsysc == nullptr || layout == nullptr) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (mc == nullptr || slot < 0 || slot >= D3D11_MULTI_MAX_CLIENTS) {
		return XRT_ERROR_IPC_FAILURE;
	}

	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);
	struct d3d11_multi_client_slot *cs = &mc->clients[slot];
	cs->chrome_pose_in_client = layout->pose_in_client;
	cs->chrome_size_w_m = layout->size_w_m;
	cs->chrome_size_h_m = layout->size_h_m;
	cs->chrome_follows_orient = (layout->follows_window_orient != 0);
	cs->chrome_depth_bias_m = layout->depth_bias_meters;
	cs->chrome_region_count = layout->hit_region_count;
	if (cs->chrome_region_count > IPC_WORKSPACE_CHROME_MAX_HIT_REGIONS) {
		cs->chrome_region_count = IPC_WORKSPACE_CHROME_MAX_HIT_REGIONS;
	}
	memcpy(cs->chrome_regions, layout->hit_regions,
	       cs->chrome_region_count * sizeof(struct ipc_workspace_chrome_hit_region));
	cs->chrome_layout_valid = true;
	return XRT_SUCCESS;
}

extern "C" bool
comp_d3d11_service_workspace_hit_test(struct xrt_system_compositor *xsysc,
                                       int32_t cursor_x,
                                       int32_t cursor_y,
                                       uint32_t *out_client_id,
                                       float *out_local_u,
                                       float *out_local_v,
                                       uint32_t *out_hit_region)
{
	if (xsysc == nullptr || out_client_id == nullptr || out_local_u == nullptr ||
	    out_local_v == nullptr || out_hit_region == nullptr) {
		return false;
	}
	*out_client_id = 0;
	*out_local_u = 0.0f;
	*out_local_v = 0.0f;
	*out_hit_region = 0; // BACKGROUND

	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (!sys->workspace_mode || mc == nullptr) {
		// Workspace not active — return success with miss output.
		return true;
	}

	POINT pt = {(LONG)cursor_x, (LONG)cursor_y};
	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);

	struct workspace_hit_result hit = workspace_raycast_hit_test(sys, mc, pt);
	*out_hit_region = hit_result_to_region(hit);

	if (hit.slot < 0) {
		// Background miss — out_client_id stays 0.
		return true;
	}

	// Slot index is the workspace's view of the client. Capture clients
	// already use slot+1000 in xrAddWorkspaceCaptureClientEXT; reuse the
	// same scheme uniformly so XrWorkspaceClientId values are consistent
	// regardless of client type.
	*out_client_id = 1000u + (uint32_t)hit.slot;

	// UV only meaningful for CONTENT hits; chrome/edge use a different
	// coordinate frame the public surface does not expose.
	if (hit.in_content) {
		float win_w = mc->clients[hit.slot].window_width_m;
		float win_h = mc->clients[hit.slot].window_height_m;
		if (win_w > 0.0f) {
			*out_local_u = hit.local_x_m / win_w;
		}
		if (win_h > 0.0f) {
			*out_local_v = hit.local_y_m / win_h;
		}
	}

	return true;
}

bool
comp_d3d11_service_set_capture_client_window_pose(struct xrt_system_compositor *xsysc,
                                                    int slot_index,
                                                    const struct xrt_pose *pose,
                                                    float width_m,
                                                    float height_m)
{
	if (xsysc == nullptr || pose == nullptr) {
		return false;
	}

	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	if (!sys->workspace_mode || sys->multi_comp == nullptr) {
		return false;
	}

	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (slot_index < 0 || slot_index >= D3D11_MULTI_MAX_CLIENTS) {
		return false;
	}

	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);

	if (!mc->clients[slot_index].active) {
		return false;
	}

	float min_dim = 0.02f;
	if (width_m < min_dim) width_m = min_dim;
	if (height_m < min_dim) height_m = min_dim;

	mc->clients[slot_index].window_pose = *pose;
	mc->clients[slot_index].window_width_m = width_m;
	mc->clients[slot_index].window_height_m = height_m;

	slot_pose_to_pixel_rect(sys, &mc->clients[slot_index],
	    &mc->clients[slot_index].window_rect_x,
	    &mc->clients[slot_index].window_rect_y,
	    &mc->clients[slot_index].window_rect_w,
	    &mc->clients[slot_index].window_rect_h);

	return true;
}

bool
comp_d3d11_service_get_capture_client_window_pose(struct xrt_system_compositor *xsysc,
                                                    int slot_index,
                                                    struct xrt_pose *out_pose,
                                                    float *out_width_m,
                                                    float *out_height_m)
{
	if (xsysc == nullptr) {
		return false;
	}

	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	if (!sys->workspace_mode || sys->multi_comp == nullptr) {
		return false;
	}

	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (slot_index < 0 || slot_index >= D3D11_MULTI_MAX_CLIENTS) {
		return false;
	}

	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);

	if (!mc->clients[slot_index].active) {
		return false;
	}

	if (out_pose) *out_pose = mc->clients[slot_index].window_pose;
	if (out_width_m) *out_width_m = mc->clients[slot_index].window_width_m;
	if (out_height_m) *out_height_m = mc->clients[slot_index].window_height_m;

	return true;
}

bool
comp_d3d11_service_ensure_workspace_window(struct xrt_system_compositor *xsysc)
{
	if (xsysc == nullptr) {
		return false;
	}

	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	if (!sys->workspace_mode) {
		service_set_workspace_mode(sys, true);
		U_LOG_W("Workspace mode activated for D3D11 service system (via ensure_workspace_window)");
	}

	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);

	// If workspace was suspended (deactivated via Ctrl+Space), resume it:
	// show window, recreate DP, restart render thread.
	if (sys->multi_comp != nullptr && sys->multi_comp->suspended) {
		struct d3d11_multi_compositor *mc = sys->multi_comp;
		U_LOG_W("Workspace: resuming from suspended state");

		mc->suspended = false;
		service_set_workspace_mode(sys, true);

		// Reverse hot-switch is LAZY: just flag each client compositor.
		// Each client's next layer_commit will tear down its own DP and
		// swap chain on its own thread (avoids cross-thread WM deadlock
		// from ShowWindow/SetWindowLongPtr while app is blocked on IPC).
		for (int i = 0; i < D3D11_MULTI_MAX_CLIENTS; i++) {
			struct d3d11_multi_client_slot *slot = &mc->clients[i];
			if (!slot->active || slot->client_type != CLIENT_TYPE_IPC) {
				continue;
			}
			if (slot->compositor == nullptr) {
				continue;
			}
			slot->compositor->pending_workspace_reentry = true;
			U_LOG_W("Workspace resume: flagged slot %d for lazy reverse hot-switch", i);
		}

		// Show the workspace window again
		if (mc->hwnd != nullptr) {
			ShowWindow(mc->hwnd, SW_SHOW);
			SetForegroundWindow(mc->hwnd);
		}

		// Recreate display processor via factory (window + swap chain still alive)
		if (mc->display_processor == nullptr && sys->base.info.dp_factory_d3d11 != NULL) {
			auto factory = (xrt_dp_factory_d3d11_fn_t)sys->base.info.dp_factory_d3d11;
			xrt_result_t dp_ret = factory(
			    sys->device.get(), sys->context.get(), mc->hwnd, &mc->display_processor);

			if (dp_ret == XRT_SUCCESS && mc->display_processor != nullptr) {
				U_LOG_W("Workspace resume: display processor recreated");
				if (mc->window != nullptr) {
					comp_d3d11_window_set_workspace_dp(mc->window, mc->display_processor);
				}
			} else {
				U_LOG_E("Workspace resume: failed to recreate display processor");
			}
		}

		// Restart render thread
		if (!mc->capture_render_running.load()) {
			capture_render_thread_start(sys);
		}

		U_LOG_W("Workspace: resumed — window shown, DP recreated, render running");
		return true;
	}

	// If a previous workspace session was dismissed (ESC), tear down its window
	// and resources so ensure_output creates a fresh one.
	if (sys->multi_comp != nullptr && sys->multi_comp->window_dismissed) {
		struct d3d11_multi_compositor *mc = sys->multi_comp;
		U_LOG_W("Workspace: resetting dismissed state from previous session");

		// Tear down window and GPU resources (same order as multi_compositor_destroy)
		if (mc->display_processor != nullptr) {
			xrt_display_processor_d3d11_destroy(&mc->display_processor);
		}
		mc->back_buffer_rtv.reset();
		mc->combined_atlas_rtv.reset();
		mc->combined_atlas_srv.reset();
		mc->combined_atlas.reset();
		mc->combined_atlas_dsv.reset();
		mc->combined_atlas_depth.reset();
		mc->swap_chain.reset();
		if (mc->window != nullptr) {
			comp_d3d11_window_destroy(&mc->window);
		}
		mc->hwnd = nullptr;

		// Reset dismiss state
		mc->window_dismissed = false;
		mc->dismiss_cleanup_done = false;
	}

	xrt_result_t ret = multi_compositor_ensure_output(sys);
	if (ret != XRT_SUCCESS || sys->multi_comp == nullptr) {
		U_LOG_E("Workspace: failed to create workspace window (ret=%d)", (int)ret);
		return false;
	}

	// Start render timer so the empty workspace window refreshes
	// (same mechanism as capture-only rendering).
	if (!sys->multi_comp->capture_render_running.load()) {
		capture_render_thread_start(sys);
	}

	U_LOG_W("Workspace: window created for empty workspace (ready for Ctrl+O)");
	return true;
}

void
comp_d3d11_service_deactivate_workspace(struct xrt_system_compositor *xsysc)
{
	if (xsysc == nullptr) {
		return;
	}

	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	struct d3d11_multi_compositor *mc = nullptr;

	// First lock scope: do all work that needs the mutex EXCEPT stopping
	// the render thread. Stopping the render thread joins it; if we hold
	// the mutex during join, the render thread can deadlock waiting for
	// the same mutex on its next iteration.
	{
		std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);

		mc = sys->multi_comp;
		if (mc == nullptr) {
			U_LOG_W("Workspace deactivate: no multi-comp — nothing to do");
			return;
		}

		if (mc->suspended) {
			U_LOG_W("Workspace deactivate: already suspended");
			return;
		}

		U_LOG_W("Workspace deactivate: beginning teardown");

		// Clear the compositor's local workspace_mode flag so layer_commit
		// takes the standalone path instead of the (now suspended) multi-comp.
		service_set_workspace_mode(sys, false);

	// --- 4C.2: Stop all capture sessions and restore 2D windows ---
	for (int i = 0; i < D3D11_MULTI_MAX_CLIENTS; i++) {
		struct d3d11_multi_client_slot *slot = &mc->clients[i];
		if (!slot->active || slot->client_type != CLIENT_TYPE_CAPTURE) {
			continue;
		}

		d3d11_capture_stop(slot->capture_ctx);
		slot->capture_ctx = nullptr;
		slot->capture_srv = nullptr;
		slot->capture_texture_last = nullptr;
		slot->capture_width = 0;
		slot->capture_height = 0;

		if (slot->app_hwnd != nullptr && IsWindow(slot->app_hwnd)) {
			SetWindowPlacement(slot->app_hwnd, &slot->saved_placement);
			SetWindowLongPtr(slot->app_hwnd, GWL_EXSTYLE, slot->saved_exstyle);
			SetWindowPos(slot->app_hwnd, HWND_TOP, 0, 0, 0, 0,
			             SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
			U_LOG_W("Workspace deactivate: restored 2D window HWND=%p", (void *)slot->app_hwnd);
		}

		slot->active = false;
		slot->compositor = nullptr;
		slot->client_type = CLIENT_TYPE_IPC;
		mc->client_count--;
		mc->capture_client_count--;
	}

	// --- 4C.3: IPC clients hot-switch to standalone (lazy) ---
	// Don't create resources here — that deadlocks (DXGI sends WM to app thread
	// which is blocked on IPC). Instead, just suspend the multi-comp.
	// Each app's next layer_commit detects workspace_mode=false and lazily creates
	// its own swap chain + DP on its own thread (no cross-thread WM).

		// Reset drag/focus state
		mc->focused_slot = -1;
		mc->drag.active = false;
		mc->title_drag.active = false;
		mc->resize.active = false;

		// Set request flag for render thread to exit. Don't join here —
		// joining while holding the mutex deadlocks if the render thread
		// is currently blocked trying to acquire it.
		mc->capture_render_running.store(false);
	} // release render_mutex

	// Now safe to join the render thread (no mutex held).
	if (mc->capture_render_thread.joinable()) {
		mc->capture_render_thread.join();
	}
	U_LOG_W("Workspace deactivate: render thread joined");

	// Re-acquire for final cleanup (DP destroy, hide window).
	{
		std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);

		if (mc->display_processor != nullptr) {
			xrt_display_processor_d3d11_request_display_mode(mc->display_processor, false);
			xrt_display_processor_d3d11_destroy(&mc->display_processor);
		}

		if (mc->hwnd != nullptr) {
			ShowWindow(mc->hwnd, SW_HIDE);
		}

		mc->suspended = true;
	}

	U_LOG_W("Workspace deactivate: complete — captures stopped, multi-comp suspended, "
	        "IPC clients will lazy-switch to standalone on next frame");
}

// Phase 5.8: empty the launcher's app list. The workspace controller calls this
// before pushing a fresh registry so the tile grid never carries stale entries.
// Stored on the system (not the multi-comp) so it survives across activations.
void
comp_d3d11_service_clear_launcher_apps(struct xrt_system_compositor *xsysc)
{
	if (xsysc == nullptr) {
		return;
	}

	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);

	sys->launcher_app_count = 0;
	// Phase 5.13: the hidden-tile state is session-only and indices refer
	// to positions in launcher_apps, so it only makes sense while that list
	// is stable. Wipe it whenever a fresh push starts.
	sys->hidden_tile_mask = 0;
	sys->launcher_selected_index = -1;

	// Phase 7.2: release all icon textures.
	for (uint32_t i = 0; i < IPC_LAUNCHER_MAX_APPS; i++) {
		sys->launcher_icons[i].srv_2d.reset();
		sys->launcher_icons[i].srv_3d.reset();
		sys->launcher_icons[i].w_2d = sys->launcher_icons[i].h_2d = 0;
		sys->launcher_icons[i].w_3d = sys->launcher_icons[i].h_3d = 0;
		sys->launcher_icons[i].layout_3d[0] = '\0';
	}
}

// Phase 5.8: append one app to the launcher's tile grid. Silently dropped if
// the array is already full. The workspace controller loops over its registry
// calling this once per entry after a clear. Lives on the system, not the
// multi-comp.
void
comp_d3d11_service_add_launcher_app(struct xrt_system_compositor *xsysc,
                                    const struct ipc_launcher_app *app)
{
	if (xsysc == nullptr || app == nullptr) {
		return;
	}

	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);

	if (sys->launcher_app_count >= IPC_LAUNCHER_MAX_APPS) {
		return;
	}

	sys->launcher_apps[sys->launcher_app_count] = *app;
	sys->launcher_app_count++;

	// Phase 7.2: load icon textures from paths carried in the IPC message.
	uint32_t idx = sys->launcher_app_count - 1;
	struct d3d11_service_system::launcher_icon &icon = sys->launcher_icons[idx];
	if (app->icon_path[0]) {
		d3d11_icon_load_from_file(sys->device.get(), app->icon_path,
		                          icon.srv_2d.put(), &icon.w_2d, &icon.h_2d);
	}
	if (app->icon_3d_path[0]) {
		d3d11_icon_load_from_file(sys->device.get(), app->icon_3d_path,
		                          icon.srv_3d.put(), &icon.w_3d, &icon.h_3d);
		snprintf(icon.layout_3d, sizeof(icon.layout_3d), "%s", app->icon_3d_layout);
	}

	// Wake the compositor window if it exists and the launcher is on-screen,
	// so the next frame picks up the new tile.
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (mc != nullptr && mc->hwnd != nullptr && mc->launcher_visible) {
		InvalidateRect(mc->hwnd, nullptr, FALSE);
	}
}

// Phase 5.11: set the running-tile bitmask. Bit i set means launcher_apps[i]
// has at least one matching IPC client connected. The render pass draws a
// glow border around any tile whose bit is set. Pushed by the workspace
// controller from its client-poll loop whenever the running set changes.
void
comp_d3d11_service_set_running_tile_mask(struct xrt_system_compositor *xsysc, uint64_t mask)
{
	if (xsysc == nullptr) {
		return;
	}

	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);

	if (sys->running_tile_mask == mask) {
		return;
	}
	sys->running_tile_mask = mask;

	// Wake the compositor if the launcher is on-screen so the new glow
	// state shows up immediately.
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (mc != nullptr && mc->hwnd != nullptr && mc->launcher_visible) {
		InvalidateRect(mc->hwnd, nullptr, FALSE);
	}
}

// Phase 5.9/5.10: poll-and-clear the pending launcher tile click. The
// WM_LBUTTONDOWN handler stores the tile index when the user clicks; the
// workspace controller calls this from its poll loop, gets the index, and
// dispatches a CreateProcess on its end (the runtime never executes binaries
// on the controller's behalf).
int32_t
comp_d3d11_service_poll_launcher_click(struct xrt_system_compositor *xsysc)
{
	if (xsysc == nullptr) {
		return -1;
	}

	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);

	// Phase 6.6: check remove first — it takes priority because the
	// launcher was hidden on remove, so no click can follow.
	if (sys->pending_launcher_remove_full_index >= 0) {
		int32_t full = sys->pending_launcher_remove_full_index;
		sys->pending_launcher_remove_full_index = -1;
		return -(IPC_LAUNCHER_ACTION_REMOVE_BASE + full);
	}

	int32_t idx = sys->pending_launcher_click_index;
	sys->pending_launcher_click_index = -1;
	return idx;
}

// Phase 5.7: spatial launcher visibility toggle. Just flips the render-thread
// bool; the render loop picks it up on the next frame and draws (or skips) the
// launcher panel overlay. No-op if there's no multi-comp yet (workspace not
// active).
void
comp_d3d11_service_set_launcher_visible(struct xrt_system_compositor *xsysc, bool visible)
{
	if (xsysc == nullptr) {
		return;
	}

	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);

	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (mc == nullptr || mc->suspended) {
		U_LOG_W("Launcher: set_visible %s ignored — multi-comp not active",
		        visible ? "true" : "false");
		return;
	}

	launcher_set_visible(sys, mc, visible);
	U_LOG_W("Launcher: %s", visible ? "shown" : "hidden");

	// Wake the render thread so the next frame reflects the new state even
	// if the render loop is idling on a timer.
	if (mc->hwnd != nullptr) {
		InvalidateRect(mc->hwnd, nullptr, FALSE);
	}
}

void
comp_d3d11_service_poll_mcp_capture(struct xrt_system_compositor *xsysc)
{
	if (xsysc == nullptr) {
		return;
	}
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	if (!sys->workspace_mode || sys->multi_comp == nullptr) {
		return;
	}

	char base[U_MCP_CAPTURE_PATH_MAX];
	if (!u_mcp_capture_poll(&sys->mcp_capture, base)) {
		return;
	}

	// Delegate atlas capture to the shared capture_frame API.
	struct ipc_capture_result cr = {};
	bool ok = comp_d3d11_service_capture_frame(xsysc, base, IPC_CAPTURE_FLAG_ATLAS, &cr);

	// Write per-window metadata JSON.
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (ok && mc != nullptr) {
		char path[U_MCP_CAPTURE_PATH_MAX + 32];
		snprintf(path, sizeof(path), "%s_windows.json", base);
		FILE *jf = fopen(path, "wb");
		if (jf != nullptr) {
			fprintf(jf, "{\n  \"atlas_width\": %u,\n  \"atlas_height\": %u,\n  \"windows\": [",
			        cr.atlas_width, cr.atlas_height);
			bool first = true;
			for (int i = 0; i < D3D11_MULTI_MAX_CLIENTS; i++) {
				const d3d11_multi_client_slot *s = &mc->clients[i];
				if (!s->active) {
					continue;
				}
				fprintf(jf, "%s\n    {\"slot\": %d, \"name\": \"%s\", "
				            "\"atlas_bbox\": {\"x\": %d, \"y\": %d, \"w\": %d, \"h\": %d}, "
				            "\"content\": {\"w\": %u, \"h\": %u}}",
				        first ? "" : ",", i, s->app_name,
				        s->window_rect_x, s->window_rect_y,
				        s->window_rect_w, s->window_rect_h,
				        s->content_view_w, s->content_view_h);
				first = false;
			}
			fprintf(jf, "\n  ]\n}\n");
			fclose(jf);
		}
	}

	// Write sentinel for the MCP tool handler's file-based poll.
	{
		char sentinel[U_MCP_CAPTURE_PATH_MAX + 32];
		snprintf(sentinel, sizeof(sentinel), "%s_DONE.txt", base);
		FILE *f = fopen(sentinel, "w");
		if (f) {
			fprintf(f, "ok=%d\n", ok);
			fclose(f);
		}
	}
	u_mcp_capture_complete(&sys->mcp_capture, ok);
}
