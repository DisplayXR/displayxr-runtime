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

#include "xrt/xrt_handles.h"
#include "xrt/xrt_limits.h"
#include "xrt/xrt_session.h"
#include "xrt/xrt_display_processor_d3d11.h"
#include "xrt/xrt_display_metrics.h"

#include "util/u_logging.h"
#include "util/u_misc.h"
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

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <mutex>
#include <sddl.h>


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
	wil::com_ptr<ID3D11RenderTargetView> atlas_rtv;

	//! Content-sized crop atlas for DP input (lazy-created when content < atlas)
	wil::com_ptr<ID3D11Texture2D> crop_texture;
	wil::com_ptr<ID3D11ShaderResourceView> crop_srv;
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

	//! Multi-compositor control interface for session state management
	struct xrt_multi_compositor_control xmcc;

	//! The device we are rendering for
	struct xrt_device *xdev;

	//! System devices for qwerty input support (passed to per-client windows)
	struct xrt_system_devices *xsysd;

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

	//! Shell mode: multi-compositor with shared window for all clients.
	//! Read from base.info.shell_mode on first client connect.
	bool shell_mode;

	//! Multi-compositor (NULL when shell_mode is false).
	struct d3d11_multi_compositor *multi_comp;

	//! Mutex for multi-compositor render (serializes D3D11 context access).
	//! Recursive because unregister_client calls render for final clear frame.
	std::recursive_mutex render_mutex;
};


/*
 *
 * Multi-compositor structs
 *
 */

#define D3D11_MULTI_MAX_CLIENTS 8

//! Spatial UI dimensions in METERS — the single source of truth.
//! Both rendering and hit-testing derive from these values.
#define UI_TITLE_BAR_H_M   0.008f   //!< Title bar height: 8mm
#define UI_BTN_W_M          0.008f   //!< Close/minimize button width: 8mm
#define UI_TASKBAR_H_M      0.009f   //!< Taskbar height: 9mm
#define UI_GLYPH_W_M        0.0035f  //!< Glyph width: 3.5mm (balanced aspect ratio)
#define UI_GLYPH_H_M        0.005f   //!< Glyph height: 5mm
#define UI_RESIZE_ZONE_M    0.003f   //!< Resize detection zone: 3mm

//! Resize edge/corner flags (bitfield).
#define RESIZE_NONE   0
#define RESIZE_LEFT   1
#define RESIZE_RIGHT  2
#define RESIZE_TOP    4
#define RESIZE_BOTTOM 8

/*!
 * Convert meters to pixels in the SBS tile (not full display).
 * Each tile represents the full physical display, so m_per_px = display_m / tile_px.
 */
static inline float
ui_m_to_tile_px_x(float meters, const struct d3d11_service_system *sys)
{
	float disp_w_m = sys->base.info.display_width_m;
	uint32_t tile_px_w = sys->base.info.display_pixel_width / sys->tile_columns;
	if (disp_w_m <= 0.0f) disp_w_m = 0.700f;
	if (tile_px_w == 0) tile_px_w = 1920;
	return meters / disp_w_m * (float)tile_px_w;
}

static inline float
ui_m_to_tile_px_y(float meters, const struct d3d11_service_system *sys)
{
	float disp_h_m = sys->base.info.display_height_m;
	uint32_t tile_px_h = sys->base.info.display_pixel_height / sys->tile_rows;
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
 * Per-client slot in the multi-compositor.
 */
struct d3d11_multi_client_slot
{
	//! The per-client compositor that owns the atlas.
	struct d3d11_service_compositor *compositor;

	//! App's HWND (from XR_EXT_win32_window_binding). Shell can resize via SetWindowPos.
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
};

/*!
 * Multi-compositor: shared window + DP that composites all client atlases.
 *
 * Created lazily on first client layer_commit when shell_mode is true.
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

	//! Window dismissed by user (ESC).
	bool window_dismissed;

	//! Right-click-drag state for window repositioning.
	struct
	{
		bool active;         //!< Currently dragging?
		int32_t slot;        //!< Which slot is being dragged (-1 = none)
		POINT start_cursor;  //!< Cursor position at drag start (shell-window client pixels)
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

	//! Current layout preset (-1=none, 0-4=preset index). Used for TAB Z-reorder in Stack.
	int32_t current_layout;

	//! Dynamic layout state (carousel, orbital, helix, expose).
	//! When mode >= 0, the layout continuously drives window poses each frame.
	struct
	{
		int mode;                //!< -1=inactive, 0=carousel, 1=orbital, 2=helix, 3=expose
		float angle_offset;      //!< Current rotation angle (radians)
		float angular_velocity;  //!< Auto-rotation speed (rad/s)
		bool user_dragging;      //!< Mouse is controlling rotation
		float drag_start_angle;  //!< angle_offset when drag started
		POINT drag_start_cursor; //!< Cursor position when drag started
		uint64_t last_tick_ns;   //!< Last frame timestamp
		float radius_m;          //!< Layout radius (meters)
		float base_win_w;        //!< Base window width for this layout
		float base_win_h;        //!< Base window height for this layout
		int prev_layout;         //!< Layout before entering dynamic mode (for expose return)
		// Momentum tracking for drag release
		float prev_angle_offset; //!< Previous frame's angle_offset (for velocity calculation)
		uint64_t prev_drag_ns;   //!< Previous frame's timestamp during drag
		// Pause state: after TAB brings a window to front, pause auto-rotation
		uint64_t pause_until_ns; //!< Auto-rotation paused until this timestamp (0 = not paused)
		float target_angle;      //!< Target angle to animate toward (for TAB snap-to-front)
	} dynamic_layout;

	//! Hovered button: 0=none, 1=close, 2=minimize, for the hovered slot.
	int hover_btn;
	int hover_btn_slot;

	//! Previous frame LMB/RMB state (for rising-edge detection).
	bool prev_lmb_held;
	bool prev_rmb_held;

	//! Double-click detection for title bar maximize toggle.
	DWORD last_title_click_time;
	int32_t last_title_click_slot;

	//! Bitmap font atlas for title bar text (768x16, 96 ASCII glyphs).
	wil::com_ptr<ID3D11Texture2D> font_atlas;
	wil::com_ptr<ID3D11ShaderResourceView> font_atlas_srv;
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

	// Set default full-viewport scissor rect so non-shell rendering isn't clipped.
	// Shell mode overrides this per-tile in multi_compositor_render().
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
	cb->quad_mode = 0.0f;
	cb->dst_rect_wh[0] = dst_w;
	cb->dst_rect_wh[1] = dst_h;
	cb->padding2[0] = 0.0f;
	cb->padding2[1] = 0.0f;

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

	// Shell mode: only create atlas texture. No window, swap chain, or DP.
	// The multi-compositor owns those shared resources.
	// Atlas sized to native display (app HWND is fullscreen, renders at native * scale).
	if (sys->shell_mode) {
		uint32_t atlas_w = sys->base.info.display_pixel_width;
		uint32_t atlas_h = sys->base.info.display_pixel_height;
		if (atlas_w == 0 || atlas_h == 0) {
			atlas_w = sys->display_width;
			atlas_h = sys->display_height;
		}
		D3D11_TEXTURE2D_DESC atlas_desc = {};
		atlas_desc.Width = atlas_w;
		atlas_desc.Height = atlas_h;
		atlas_desc.MipLevels = 1;
		atlas_desc.ArraySize = 1;
		atlas_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		atlas_desc.SampleDesc.Count = 1;
		atlas_desc.Usage = D3D11_USAGE_DEFAULT;
		atlas_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

		hr = sys->device->CreateTexture2D(&atlas_desc, nullptr, res->atlas_texture.put());
		if (FAILED(hr)) {
			U_LOG_E("Shell mode: failed to create atlas texture (hr=0x%08X)", hr);
			return XRT_ERROR_D3D11;
		}
		sys->device->CreateShaderResourceView(res->atlas_texture.get(), nullptr, res->atlas_srv.put());
		sys->device->CreateRenderTargetView(res->atlas_texture.get(), nullptr, res->atlas_rtv.put());

		U_LOG_W("Shell mode: created atlas-only resources for client (%ux%u)",
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

	// Create display processor via factory (set by the target builder at init time)
	if (sys->base.info.dp_factory_d3d11 != NULL) {
		auto factory = (xrt_dp_factory_d3d11_fn_t)sys->base.info.dp_factory_d3d11;
		xrt_result_t dp_ret = factory(sys->device.get(), sys->context.get(), res->hwnd, &res->display_processor);
		if (dp_ret != XRT_SUCCESS) {
			U_LOG_W("D3D11 display processor factory failed (error %d), continuing without",
			        (int)dp_ret);
			res->display_processor = nullptr;
		} else {
			U_LOG_W("D3D11 display processor created via factory for client");
			// Auto-switch to 3D mode when display processor is ready
			if (sys->hardware_display_3d) {
				xrt_display_processor_d3d11_request_display_mode(
				    res->display_processor, true);
			}

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
	// D3D12 client rejects this flag, but it's meaningless for shell mode.
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
	if (!res->owns_window || res->hud == NULL || !u_hud_is_visible()) {
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
                          uint32_t content_view_h)
{
	uint32_t expected_w = sys->tile_columns * content_view_w;
	uint32_t expected_h = sys->tile_rows * content_view_h;

	// Content fills the full atlas — pass directly
	if (expected_w == sys->display_width && expected_h == sys->display_height) {
		return res->atlas_srv.get();
	}

	// Lazy (re)create crop texture at content dimensions
	if (res->crop_width != expected_w || res->crop_height != expected_h) {
		res->crop_srv.reset();
		res->crop_texture.reset();

		D3D11_TEXTURE2D_DESC crop_desc = {};
		crop_desc.Width = expected_w;
		crop_desc.Height = expected_h;
		crop_desc.MipLevels = 1;
		crop_desc.ArraySize = 1;
		crop_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		crop_desc.SampleDesc.Count = 1;
		crop_desc.Usage = D3D11_USAGE_DEFAULT;
		crop_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		HRESULT hr = sys->device->CreateTexture2D(
		    &crop_desc, nullptr, res->crop_texture.put());
		if (SUCCEEDED(hr)) {
			sys->device->CreateShaderResourceView(
			    res->crop_texture.get(), nullptr, res->crop_srv.put());
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

	// Copy each view's content region from atlas to crop texture
	uint32_t num_views = sys->tile_columns * sys->tile_rows;
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
 */
static void
multi_compositor_update_input_forward(struct d3d11_multi_compositor *mc)
{
	if (mc == nullptr || mc->window == nullptr) {
		return;
	}

	HWND target = NULL;
	int32_t rx = 0, ry = 0, rw = 0, rh = 0;
	if (mc->focused_slot >= 0 && mc->focused_slot < D3D11_MULTI_MAX_CLIENTS &&
	    mc->clients[mc->focused_slot].active) {
		target = mc->clients[mc->focused_slot].app_hwnd;
		rx = (int32_t)mc->clients[mc->focused_slot].window_rect_x;
		ry = (int32_t)mc->clients[mc->focused_slot].window_rect_y;
		rw = (int32_t)mc->clients[mc->focused_slot].window_rect_w;
		rh = (int32_t)mc->clients[mc->focused_slot].window_rect_h;
	}

	comp_d3d11_window_set_input_forward(mc->window, (void *)target, rx, ry, rw, rh);
}

// Forward declarations
static inline bool quat_is_identity(const struct xrt_quat *q);

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
			mc->clients[i].active = true;

			// Default window pose: 40% of display size, placed in left/right halves.
			// Convention: position is meters from display center, +X right, +Y up.
			// Slot 0: left window centered at (-25%, +25%) of display
			// Slot 1: right window centered at (+25%, +25%) of display
			float disp_w_m = sys->base.info.display_width_m;
			float disp_h_m = sys->base.info.display_height_m;
			if (disp_w_m <= 0.0f) disp_w_m = 0.700f;
			if (disp_h_m <= 0.0f) disp_h_m = 0.394f;

			float win_w_m = disp_w_m * 0.40f;
			float win_h_m = disp_h_m * 0.40f;
			// Slot 0 center: (-0.25 * disp_w, +0.25 * disp_h) → left, upper
			// Slot 1 center: (+0.25 * disp_w, +0.25 * disp_h) → right, upper
			float center_x_m = (i == 0) ? (-0.25f * disp_w_m) : (+0.25f * disp_w_m);
			float center_y_m = 0.25f * disp_h_m;  // +Y up = upper half

			// Entry animation: start from center (small), animate to target position.
			// Set initial pose at display center, small size.
			mc->clients[i].window_pose.orientation.x = 0.0f;
			mc->clients[i].window_pose.orientation.y = 0.0f;
			mc->clients[i].window_pose.orientation.z = 0.0f;
			mc->clients[i].window_pose.orientation.w = 1.0f;
			mc->clients[i].window_pose.position.x = 0.0f;
			mc->clients[i].window_pose.position.y = 0.0f;
			mc->clients[i].window_pose.position.z = 0.0f;
			mc->clients[i].window_width_m = win_w_m * 0.3f; // start small
			mc->clients[i].window_height_m = win_h_m * 0.3f;

			// Entry animation: grow from center to target position
			struct xrt_pose target = {};
			target.orientation.w = 1.0f;
			target.position.x = center_x_m;
			target.position.y = center_y_m;
			target.position.z = 0.0f;
			// Use inline animation setup (slot_animate_to is defined later in file)
			mc->clients[i].anim.active = true;
			mc->clients[i].anim.start_pose = mc->clients[i].window_pose;
			mc->clients[i].anim.target_pose = target;
			mc->clients[i].anim.start_width_m = win_w_m * 0.3f;
			mc->clients[i].anim.start_height_m = win_h_m * 0.3f;
			mc->clients[i].anim.target_width_m = win_w_m;
			mc->clients[i].anim.target_height_m = win_h_m;
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
			multi_compositor_update_input_forward(mc);
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
 * Destroy the multi-compositor and all its resources.
 */
static void
multi_compositor_destroy(struct d3d11_multi_compositor *mc)
{
	if (mc == nullptr) {
		return;
	}

	U_LOG_W("Multi-comp: destroying");

	if (mc->display_processor != nullptr) {
		xrt_display_processor_d3d11_request_display_mode(mc->display_processor, false);
		xrt_display_processor_d3d11_destroy(&mc->display_processor);
	}

	mc->back_buffer_rtv.reset();
	mc->combined_atlas_rtv.reset();
	mc->combined_atlas_srv.reset();
	mc->combined_atlas.reset();
	mc->font_atlas_srv.reset();
	mc->font_atlas.reset();
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
 * Called on first layer_commit in shell mode. By this time the target builder
 * has already set dp_factory_d3d11.
 */
static xrt_result_t
multi_compositor_ensure_output(struct d3d11_service_system *sys)
{
	// Serialize multi-comp init — multiple IPC client threads can call this
	// concurrently when clients connect simultaneously (e.g., shell launching
	// D3D11 + VK apps). Without this lock, both threads create the display
	// processor, causing SR SDK state corruption and crash.
	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);

	if (sys->multi_comp == nullptr) {
		sys->multi_comp = new d3d11_multi_compositor();
		std::memset(sys->multi_comp, 0, sizeof(*sys->multi_comp));
		sys->multi_comp->focused_slot = -1;
		sys->multi_comp->dynamic_layout.mode = -1; // No dynamic layout at startup
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
	}

	U_LOG_W("Multi-comp: combined atlas %ux%u",
	        sys->base.info.display_pixel_width > 0 ? sys->base.info.display_pixel_width : sys->display_width,
	        sys->base.info.display_pixel_height > 0 ? sys->base.info.display_pixel_height : sys->display_height);

	// Create bitmap font atlas texture (768x16, 96 ASCII glyphs)
	if (!mc->font_atlas) {
		uint32_t fa_w = BITMAP_FONT_ATLAS_W;
		uint32_t fa_h = BITMAP_FONT_ATLAS_H;
		uint32_t *pixels = new uint32_t[fa_w * fa_h];
		std::memset(pixels, 0, fa_w * fa_h * sizeof(uint32_t));

		for (int g = 0; g < BITMAP_FONT_GLYPH_COUNT; g++) {
			for (int row = 0; row < BITMAP_FONT_GLYPH_H; row++) {
				uint8_t bits = bitmap_font_8x16[g][row];
				for (int bit = 0; bit < BITMAP_FONT_GLYPH_W; bit++) {
					if (bits & (0x80 >> bit)) {
						uint32_t px = g * BITMAP_FONT_GLYPH_W + bit;
						uint32_t py = row;
						pixels[py * fa_w + px] = 0xFFFFFFFF; // white, opaque
					}
				}
			}
		}

		D3D11_TEXTURE2D_DESC font_desc = {};
		font_desc.Width = fa_w;
		font_desc.Height = fa_h;
		font_desc.MipLevels = 1;
		font_desc.ArraySize = 1;
		font_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		font_desc.SampleDesc.Count = 1;
		font_desc.Usage = D3D11_USAGE_IMMUTABLE;
		font_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		D3D11_SUBRESOURCE_DATA init_data = {};
		init_data.pSysMem = pixels;
		init_data.SysMemPitch = fa_w * sizeof(uint32_t);

		hr = sys->device->CreateTexture2D(&font_desc, &init_data, mc->font_atlas.put());
		if (SUCCEEDED(hr)) {
			sys->device->CreateShaderResourceView(mc->font_atlas.get(), nullptr, mc->font_atlas_srv.put());
			U_LOG_W("Multi-comp: font atlas created (%ux%u)", fa_w, fa_h);
		} else {
			U_LOG_E("Multi-comp: failed to create font atlas (hr=0x%08X)", hr);
		}

		delete[] pixels;
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
				comp_d3d11_window_set_shell_dp(mc->window, mc->display_processor);
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

			// Enable 3D mode
			if (mc->display_processor != nullptr && sys->hardware_display_3d) {
				xrt_display_processor_d3d11_request_display_mode(mc->display_processor, true);
			}
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
 * Result of spatial raycasting hit-test against shell windows.
 */
struct shell_hit_result
{
	int slot;            //!< Hit window slot (-1 = no hit)
	bool in_title_bar;   //!< Hit is in the title bar region
	bool in_close_btn;   //!< Hit is on the close button
	bool in_minimize_btn; //!< Hit is on the minimize button
	bool in_content;     //!< Hit is in the content area
	float local_x_m;    //!< Hit point in window-local meters (0 = left edge)
	float local_y_m;    //!< Hit point in window-local meters (0 = top of title bar, positive down)
	int edge_flags;      //!< RESIZE_LEFT|RIGHT|TOP|BOTTOM if near edge
};

/*!
 * Spatial raycast hit-test: cast a ray from the user's eye through the mouse
 * cursor position on the display surface, and intersect with shell window planes.
 *
 * Each window is a 3D rectangle defined by (pose, width_m, height_m).
 * The display is at Z=0 with known physical dimensions.
 * The eye position comes from the display processor's face tracking.
 *
 * This approach is tiling-independent and future-proofs for angled 3D windows.
 */
static struct shell_hit_result
shell_raycast_hit_test(struct d3d11_service_system *sys,
                       struct d3d11_multi_compositor *mc,
                       POINT cursor_px)
{
	struct shell_hit_result result = {};
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

		// Extended bounds including title bar (above content)
		float ext_top = win_top + title_bar_h_m;

		// Check if hit is within extended window bounds (including resize zone)
		if (hit_x >= win_left - resize_zone_m && hit_x < win_right + resize_zone_m &&
		    hit_y >= win_bottom - resize_zone_m && hit_y < ext_top + resize_zone_m) {

			// Window-local coordinates: (0,0) = top of title bar, positive down/right
			float local_x = hit_x - win_left;
			float local_y = ext_top - hit_y; // Positive downward from title bar top

			result.slot = s;
			result.local_x_m = local_x;
			result.local_y_m = local_y;

			// Classify hit region
			bool in_window = (hit_x >= win_left && hit_x < win_right &&
			                  hit_y >= win_bottom && hit_y < ext_top);
			result.in_title_bar = in_window && (local_y < title_bar_h_m);
			result.in_content = in_window && (local_y >= title_bar_h_m);

			if (result.in_title_bar) {
				result.in_close_btn = (local_x >= win_w - btn_w_m);
				result.in_minimize_btn = !result.in_close_btn &&
				                         (local_x >= win_w - 2.0f * btn_w_m);
			}

			// Edge detection (resize zones)
			result.edge_flags = RESIZE_NONE;
			if (hit_x < win_left + resize_zone_m) result.edge_flags |= RESIZE_LEFT;
			if (hit_x >= win_right - resize_zone_m) result.edge_flags |= RESIZE_RIGHT;
			if (hit_y < win_bottom + resize_zone_m) result.edge_flags |= RESIZE_BOTTOM;
			if (hit_y >= ext_top - resize_zone_m) result.edge_flags |= RESIZE_TOP;

			// If we're inside the window (not just in resize zone), clear edge flags
			// unless we're actually on the edge
			if (in_window && result.edge_flags == RESIZE_NONE) {
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

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

//! Default animation duration for layout transitions (300ms).
#define ANIM_DURATION_NS (300ULL * 1000000ULL)

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

//! Default auto-rotation speed for dynamic layouts (~10 deg/sec).
#define DYNAMIC_ROTATION_SPEED 0.175f
//! Friction coefficient for momentum deceleration after drag release.
#define DYNAMIC_FRICTION 3.0f

/*!
 * Compute the maximum comfortable Z depth: ±1/5 of max(display_w, display_h).
 * Windows should stay within this range for comfortable viewing on the lenticular display.
 */
static inline float
compute_zmax(const struct d3d11_service_system *sys)
{
	float dw = sys->base.info.display_width_m;
	float dh = sys->base.info.display_height_m;
	if (dw <= 0.0f) dw = 0.700f;
	if (dh <= 0.0f) dh = 0.394f;
	return (dw > dh ? dw : dh) / 5.0f;
}

/*!
 * Compute carousel window pose for a given window index and angle offset.
 * Full 360° circle, front window near Z=0, back windows recede within ±zmax.
 */
static void
carousel_compute_pose(int idx, int n, float angle_offset, float radius_m,
                      float base_w, float base_h, float zmax,
                      struct xrt_pose *out_pose, float *out_w, float *out_h)
{
	float base_angle = (2.0f * (float)M_PI / (float)n) * idx;
	float world_angle = base_angle + angle_offset;

	out_pose->position.x = sinf(world_angle) * radius_m;
	// Map Z so front is at Z=0, back is at -zmax (not -2*radius)
	float raw_depth = cosf(world_angle); // +1 = front, -1 = back
	out_pose->position.z = (raw_depth - 1.0f) * zmax * 0.5f; // front=0, back=-zmax
	out_pose->position.y = 0.0f;

	// Depth-based scaling: front = full, back = 70%
	float depth_t = (raw_depth + 1.0f) / 2.0f; // 0=back, 1=front
	float scale = 0.70f + 0.30f * depth_t;

	*out_w = base_w * scale;
	*out_h = base_h * scale;

	// Yaw to face center
	out_pose->orientation = quat_from_yaw(-world_angle);
}

/*!
 * Compute orbital window pose — elliptical orbits at different radii.
 */
static void
orbital_compute_pose(int idx, int n, float angle_offset, float base_radius,
                     float base_w, float base_h, int focused_idx, float zmax,
                     struct xrt_pose *out_pose, float *out_w, float *out_h)
{
	// Focused window gets innermost orbit
	int orbit_idx = idx;
	if (idx == focused_idx) orbit_idx = 0;
	else if (idx < focused_idx) orbit_idx = idx + 1;

	float radius_step = 0.02f;
	float orbit_radius = base_radius + orbit_idx * radius_step;
	float orbit_speed = 1.0f / (1.0f + orbit_idx * 0.3f);
	float angle = angle_offset * orbit_speed + (2.0f * (float)M_PI / (float)n) * idx;

	out_pose->position.x = sinf(angle) * orbit_radius * 1.5f; // wider X
	// Clamp Z within ±zmax
	float raw_z = cosf(angle) * orbit_radius * 0.5f - base_radius * 0.3f;
	if (raw_z < -zmax) raw_z = -zmax;
	if (raw_z > zmax) raw_z = zmax;
	out_pose->position.z = raw_z;
	out_pose->position.y = 0.0f;

	float scale = (orbit_idx == 0) ? 1.0f : (0.85f - orbit_idx * 0.05f);
	if (scale < 0.5f) scale = 0.5f;
	*out_w = base_w * scale;
	*out_h = base_h * scale;

	out_pose->orientation = quat_from_yaw(-angle * 0.5f);
}

/*!
 * Compute helix window pose — vertical spiral staircase.
 */
static void
helix_compute_pose(int idx, int n, float angle_offset, float radius_m,
                   float base_w, float base_h, float zmax,
                   struct xrt_pose *out_pose, float *out_w, float *out_h)
{
	float base_angle = (2.0f * (float)M_PI / (float)n) * idx;
	float world_angle = base_angle + angle_offset;

	out_pose->position.x = sinf(world_angle) * radius_m;
	// Map Z within ±zmax like carousel
	float raw_depth = cosf(world_angle);
	out_pose->position.z = (raw_depth - 1.0f) * zmax * 0.5f;
	// Vertical offset: gentle spiral pitch
	float helix_pitch_m = 0.04f; // 4cm per revolution
	float raw_y = helix_pitch_m * (world_angle / (2.0f * (float)M_PI));
	float range = helix_pitch_m * 1.5f;
	out_pose->position.y = fmodf(raw_y + range / 2.0f, range) - range / 2.0f;

	// Depth-based scaling
	float depth_t = (raw_depth + 1.0f) / 2.0f;
	float scale = 0.65f + 0.35f * depth_t;
	*out_w = base_w * scale;
	*out_h = base_h * scale;

	out_pose->orientation = quat_from_yaw(-world_angle);
}

/*!
 * Compute expose (grid overview) window pose.
 */
static void
expose_compute_pose(int idx, int n, float disp_w_m, float disp_h_m,
                    struct xrt_pose *out_pose, float *out_w, float *out_h)
{
	int cols = (int)ceilf(sqrtf((float)n));
	if (cols < 1) cols = 1;
	int rows = (n + cols - 1) / cols;
	int col = idx % cols;
	int row = idx / cols;

	float margin = 0.008f; // 8mm gap between windows
	float total_w = disp_w_m * 0.85f;
	float total_h = disp_h_m * 0.85f;
	float cell_w = (total_w - (cols - 1) * margin) / cols;
	float cell_h = (total_h - (rows - 1) * margin) / rows;

	out_pose->position.x = (col - (cols - 1) / 2.0f) * (cell_w + margin);
	out_pose->position.y = ((rows - 1) / 2.0f - row) * (cell_h + margin);
	out_pose->position.z = -0.03f; // slightly behind display
	out_pose->orientation.x = 0; out_pose->orientation.y = 0;
	out_pose->orientation.z = 0; out_pose->orientation.w = 1;

	*out_w = cell_w;
	*out_h = cell_h;
}

/*!
 * Tick the dynamic layout: update angle, compute poses, apply to all windows.
 * Called every frame when dynamic_layout.mode >= 0.
 */
static void
dynamic_layout_tick(struct d3d11_service_system *sys, struct d3d11_multi_compositor *mc, uint64_t now_ns)
{
	auto &dl = mc->dynamic_layout;
	if (dl.mode < 0) return;

	// Compute dt
	float dt = 0.0f;
	if (dl.last_tick_ns > 0) {
		dt = (float)(now_ns - dl.last_tick_ns) / 1e9f;
		if (dt > 0.1f) dt = 0.1f; // cap at 100ms to avoid jumps
	}
	dl.last_tick_ns = now_ns;

	// Update angle: auto-rotation when not dragging
	if (!dl.user_dragging && dl.mode != 3) { // expose doesn't rotate
		bool paused = (dl.pause_until_ns > 0 && now_ns < dl.pause_until_ns);

		if (paused) {
			// Animate toward target angle (TAB snap-to-front), then hold
			float diff = dl.target_angle - dl.angle_offset;
			if (fabsf(diff) > 0.001f) {
				// Ease toward target
				float snap_speed = diff * 5.0f; // arrive quickly
				dl.angle_offset += snap_speed * dt;
				// Don't overshoot
				if ((diff > 0 && dl.angle_offset > dl.target_angle) ||
				    (diff < 0 && dl.angle_offset < dl.target_angle)) {
					dl.angle_offset = dl.target_angle;
				}
			} else {
				dl.angle_offset = dl.target_angle;
			}
			dl.angular_velocity = 0.0f;
		} else {
			dl.pause_until_ns = 0;
			dl.angle_offset += dl.angular_velocity * dt;
			// Apply friction (exponential decay)
			float friction = DYNAMIC_FRICTION;
			dl.angular_velocity *= expf(-friction * dt);
			// Ease toward target auto-rotation speed
			float target = DYNAMIC_ROTATION_SPEED;
			float spd_diff = target - dl.angular_velocity;
			dl.angular_velocity += spd_diff * (1.0f - expf(-1.0f * dt));
		}
	}

	// Collect active windows
	int active[D3D11_MULTI_MAX_CLIENTS];
	int n = 0;
	for (int i = 0; i < D3D11_MULTI_MAX_CLIENTS; i++) {
		if (mc->clients[i].active && !mc->clients[i].minimized) {
			active[n++] = i;
		}
	}
	if (n == 0) return;

	float disp_w_m = sys->base.info.display_width_m;
	float disp_h_m = sys->base.info.display_height_m;
	if (disp_w_m <= 0.0f) disp_w_m = 0.700f;
	if (disp_h_m <= 0.0f) disp_h_m = 0.394f;

	// Compute comfortable Z range: ±1/5 of max(display_w, display_h)
	float zmax = compute_zmax(sys);

	// Compute and apply poses for each window
	for (int idx = 0; idx < n; idx++) {
		int s = active[idx];
		struct xrt_pose pose = {};
		float w_m = 0, h_m = 0;

		switch (dl.mode) {
		case 0: // Carousel
			carousel_compute_pose(idx, n, dl.angle_offset, dl.radius_m,
			                      dl.base_win_w, dl.base_win_h, zmax, &pose, &w_m, &h_m);
			break;
		case 1: // Orbital
			orbital_compute_pose(idx, n, dl.angle_offset, dl.radius_m,
			                     dl.base_win_w, dl.base_win_h, mc->focused_slot, zmax,
			                     &pose, &w_m, &h_m);
			break;
		case 2: // Helix
			helix_compute_pose(idx, n, dl.angle_offset, dl.radius_m,
			                   dl.base_win_w, dl.base_win_h, zmax, &pose, &w_m, &h_m);
			break;
		case 3: // Expose
			expose_compute_pose(idx, n, disp_w_m, disp_h_m, &pose, &w_m, &h_m);
			break;
		}

		mc->clients[s].window_pose = pose;
		mc->clients[s].window_width_m = w_m;
		mc->clients[s].window_height_m = h_m;
		mc->clients[s].anim.active = false; // dynamic layout overrides animation

		slot_pose_to_pixel_rect(sys, &mc->clients[s],
		                        &mc->clients[s].window_rect_x,
		                        &mc->clients[s].window_rect_y,
		                        &mc->clients[s].window_rect_w,
		                        &mc->clients[s].window_rect_h);
		mc->clients[s].hwnd_resize_pending = true;
	}
}

/*!
 * Enter a dynamic layout mode. Initializes state and starts transition.
 */
static void
enter_dynamic_layout(struct d3d11_service_system *sys, struct d3d11_multi_compositor *mc, int mode)
{
	float disp_w_m = sys->base.info.display_width_m;
	float disp_h_m = sys->base.info.display_height_m;
	if (disp_w_m <= 0.0f) disp_w_m = 0.700f;
	if (disp_h_m <= 0.0f) disp_h_m = 0.394f;

	// Un-minimize all windows
	for (int i = 0; i < D3D11_MULTI_MAX_CLIENTS; i++) {
		if (mc->clients[i].active) mc->clients[i].minimized = false;
	}

	mc->dynamic_layout.prev_layout = mc->current_layout;
	mc->dynamic_layout.mode = mode;
	mc->dynamic_layout.angle_offset = 0.0f;
	mc->dynamic_layout.angular_velocity = (mode == 3) ? 0.0f : DYNAMIC_ROTATION_SPEED;
	mc->dynamic_layout.user_dragging = false;
	mc->dynamic_layout.last_tick_ns = os_monotonic_get_ns();
	mc->dynamic_layout.radius_m = 0.12f; // 12cm default radius
	mc->dynamic_layout.base_win_w = disp_w_m * 0.40f;
	mc->dynamic_layout.base_win_h = disp_h_m * 0.40f;

	mc->current_layout = 4 + mode; // 4=carousel, 5=orbital, 6=helix, 7=expose

	const char *mode_names[] = {"carousel", "orbital", "helix", "expose"};
	U_LOG_W("Multi-comp: dynamic layout → %s", mode_names[mode]);
}

/*!
 * Apply a layout preset to all active (non-minimized) windows.
 * layout_id: 0=side-by-side, 1=stacked, 2=theater, 3=stack
 * For dynamic layouts (carousel etc.), use enter_dynamic_layout() instead.
 */
static void
apply_layout(struct d3d11_service_system *sys, struct d3d11_multi_compositor *mc, int layout_id)
{
	float disp_w_m = sys->base.info.display_width_m;
	float disp_h_m = sys->base.info.display_height_m;
	if (disp_w_m <= 0.0f) disp_w_m = 0.700f;
	if (disp_h_m <= 0.0f) disp_h_m = 0.394f;

	// Count ALL active clients (un-minimize them for layout)
	int active[D3D11_MULTI_MAX_CLIENTS];
	int n = 0;
	for (int i = 0; i < D3D11_MULTI_MAX_CLIENTS; i++) {
		if (mc->clients[i].active) {
			mc->clients[i].minimized = false; // Un-minimize for layout
			active[n++] = i;
		}
	}
	if (n == 0) return;

	const char *layout_names[] = {"side-by-side", "stacked", "theater", "stack", "carousel"};
	if (layout_id < 0 || layout_id > 4) return;
	U_LOG_W("Multi-comp: layout %s (%d windows)", layout_names[layout_id], n);
	mc->current_layout = layout_id;
	mc->dynamic_layout.mode = -1; // Exit dynamic mode when entering static layout

	for (int idx = 0; idx < n; idx++) {
		int s = active[idx];
		float new_x = 0, new_y = 0, new_z = 0, new_w = 0, new_h = 0;
		struct xrt_quat new_orient = {0, 0, 0, 1}; // identity

		switch (layout_id) {
		case 0: // Side-by-side: split width equally
			new_w = disp_w_m * 0.90f / n;
			new_h = disp_h_m * 0.90f;
			new_x = (idx - (n - 1) / 2.0f) * (disp_w_m * 0.90f / n);
			new_y = 0;
			break;

		case 1: // Stacked: split height equally
			new_w = disp_w_m * 0.90f;
			new_h = disp_h_m * 0.90f / n;
			new_x = 0;
			new_y = ((n - 1) / 2.0f - idx) * (disp_h_m * 0.90f / n);
			break;

		case 2: { // Theater: focused at Z=0 large, others recessed + angled inward
			bool is_focused = (s == mc->focused_slot || (mc->focused_slot < 0 && idx == 0));
			if (is_focused) {
				new_w = disp_w_m * 0.60f;
				new_h = disp_h_m * 0.60f;
				new_x = 0;
				new_y = 0;
				new_z = 0;
			} else {
				new_w = disp_w_m * 0.40f;
				new_h = disp_h_m * 0.40f;
				new_z = -0.03f; // 3cm behind display
				// Spread non-focused windows left/right
				int side_idx = idx;
				if (s == mc->focused_slot) side_idx = 0; // shouldn't happen
				int side_count = n - 1;
				if (side_count <= 0) side_count = 1;
				float spread = (float)(side_idx - (side_count - 1) / 2.0f);
				new_x = spread * disp_w_m * 0.35f;
				new_y = 0;
				// Angle inward: positive yaw for left windows, negative for right
				float yaw = (new_x > 0) ? -(float)(15.0 * M_PI / 180.0)
				                         : (float)(15.0 * M_PI / 180.0);
				if (fabsf(new_x) < 0.01f) yaw = 0;
				new_orient = quat_from_yaw(yaw);
			}
			break;
		}

		case 3: { // Stack: card pile at varying Z, focused at front
			new_w = disp_w_m * 0.55f;
			new_h = disp_h_m * 0.55f;
			// Focused window at front Z, others behind
			bool is_focused = (s == mc->focused_slot || (mc->focused_slot < 0 && idx == 0));
			if (is_focused) {
				new_z = 0.02f; // 2cm in front
				new_x = 0;
				new_y = 0;
			} else {
				// Distribute behind, with XY offset for card-stack look
				float z_step = 0.04f / (n > 1 ? (float)(n - 1) : 1.0f);
				new_z = 0.02f - (idx + 1) * z_step;
				if (new_z < -0.02f) new_z = -0.02f;
				new_x = idx * 0.005f;  // 5mm right per layer
				new_y = -idx * 0.005f; // 5mm down per layer
			}
			break;
		}

		default:
			return;
		}

		// Animate to target pose instead of instant snap
		struct xrt_pose target_pose;
		target_pose.position.x = new_x;
		target_pose.position.y = new_y;
		target_pose.position.z = new_z;
		target_pose.orientation = new_orient;

		uint64_t now_ns = os_monotonic_get_ns();
		slot_animate_to(&mc->clients[s], &target_pose, new_w, new_h,
		                now_ns, ANIM_DURATION_NS);
	}

	multi_compositor_update_input_forward(mc);
}

/*!
 * Render all client atlases into the combined atlas using Level 2 Kooima,
 * then run DP process_atlas and present.
 *
 * Called from compositor_layer_commit in shell mode.
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

	if (mc->window_dismissed) {
		// Shell was dismissed (ESC), but a new client has connected and is
		// submitting frames. Reopen the shell window.
		if (mc->client_count > 0) {
			U_LOG_W("Multi-comp: reopening shell window (new client after dismiss)");
			// Destroy old window resources
			if (mc->window != nullptr) {
				comp_d3d11_window_destroy(&mc->window);
			}
			mc->hwnd = nullptr;
			mc->swap_chain = nullptr;
			mc->back_buffer_rtv = nullptr;
			mc->display_processor = nullptr;
			mc->crop_texture = nullptr;
			mc->crop_srv = nullptr;
			mc->window_dismissed = false;

			// ensure_output will recreate everything
			xrt_result_t ret = multi_compositor_ensure_output(sys);
			if (ret != XRT_SUCCESS) {
				return;
			}
			mc = sys->multi_comp;
		} else {
			return;
		}
	}

	// Check window validity
	if (mc->window != nullptr && !comp_d3d11_window_is_valid(mc->window)) {
		U_LOG_W("Multi-comp: window closed - dismissing");
		mc->window_dismissed = true;
		// Switch display back to 2D (lens off) on dismiss
		if (mc->display_processor != nullptr) {
			xrt_display_processor_d3d11_request_display_mode(mc->display_processor, false);
		}
		return;
	}

	// TAB: cycle focus — includes unfocused state (-1)
	// Cycle: slot 0 → slot 1 → ... → -1 (unfocused) → slot 0
	if (GetAsyncKeyState(VK_TAB) & 1) {
		if (mc->client_count > 0) {
			// Start from current and advance
			int next = mc->focused_slot + 1;
			bool found = false;
			// Search active slots starting from next
			for (int i = 0; i < D3D11_MULTI_MAX_CLIENTS; i++) {
				int idx = (next + i) % D3D11_MULTI_MAX_CLIENTS;
				if (idx <= mc->focused_slot && mc->focused_slot >= 0) {
					// Wrapped around — go to unfocused
					break;
				}
				if (mc->clients[idx].active && !mc->clients[idx].minimized) {
					mc->focused_slot = idx;
					found = true;
					break;
				}
			}
			if (!found) {
				mc->focused_slot = -1; // unfocused
			}
			U_LOG_W("Multi-comp: TAB → focused slot %d%s", mc->focused_slot,
			        mc->focused_slot < 0 ? " (unfocused)" : "");
			multi_compositor_update_input_forward(mc);

			// Stack layout: TAB reorders Z — focused window to front
			if (mc->current_layout == 3 && mc->focused_slot >= 0) {
				apply_layout(sys, mc, 3);
			}

			// Dynamic carousel/orbital/helix: TAB rotates to bring focused window to front + pause 5s
			if (mc->dynamic_layout.mode >= 0 && mc->dynamic_layout.mode <= 2 && mc->focused_slot >= 0) {
				// Find the index of the focused slot among active windows
				int active_idx = -1;
				int n_active = 0;
				for (int i = 0; i < D3D11_MULTI_MAX_CLIENTS; i++) {
					if (mc->clients[i].active && !mc->clients[i].minimized) {
						if (i == mc->focused_slot) active_idx = n_active;
						n_active++;
					}
				}
				if (active_idx >= 0 && n_active > 0) {
					// Target angle: the angle_offset that places active_idx at the front (angle=0)
					float base_angle = (2.0f * (float)M_PI / (float)n_active) * active_idx;
					// We want base_angle + target_offset = 0 (mod 2π) → target_offset = -base_angle
					float target = -base_angle;
					// Find shortest rotation from current angle
					float diff = target - mc->dynamic_layout.angle_offset;
					// Normalize to [-π, π]
					while (diff > (float)M_PI) diff -= 2.0f * (float)M_PI;
					while (diff < -(float)M_PI) diff += 2.0f * (float)M_PI;
					mc->dynamic_layout.target_angle = mc->dynamic_layout.angle_offset + diff;
					mc->dynamic_layout.angular_velocity = diff * 3.0f; // arrive in ~0.33s
					mc->dynamic_layout.pause_until_ns = os_monotonic_get_ns() + 5000000000ULL; // 5 seconds
				}
			}
		}
	}

	// DELETE: close focused client
	if (GetAsyncKeyState(VK_DELETE) & 1) {
		if (mc->focused_slot >= 0 && mc->focused_slot < D3D11_MULTI_MAX_CLIENTS &&
		    mc->clients[mc->focused_slot].active) {
			struct d3d11_service_compositor *fc = mc->clients[mc->focused_slot].compositor;
			if (fc != nullptr && fc->xses != nullptr) {
				union xrt_session_event xse = XRT_STRUCT_INIT;
				xse.type = XRT_SESSION_EVENT_EXIT_REQUEST;
				xrt_session_event_sink_push(fc->xses, &xse);
				U_LOG_W("Multi-comp: DELETE → exit request for slot %d", mc->focused_slot);
			}
		}
	}

	// Layout presets: Ctrl+1-4 static, Ctrl+5-8 dynamic
	// 1=SBS, 2=stacked, 3=theater, 4=stack, 5=carousel, 6=orbital, 7=helix, 8=expose
	if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
		for (int k = '1'; k <= '8'; k++) {
			if (GetAsyncKeyState(k) & 1) {
				if (k <= '4') {
					apply_layout(sys, mc, k - '1');
				} else {
					enter_dynamic_layout(sys, mc, k - '5'); // 0=carousel, 1=orbital, 2=helix, 3=expose
				}
				break;
			}
		}
	}

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
			struct shell_hit_result hover = shell_raycast_hit_test(sys, mc, cpt);

			// Track button hover for highlight rendering
			mc->hover_btn = 0;
			mc->hover_btn_slot = -1;
			if (hover.in_close_btn) { mc->hover_btn = 1; mc->hover_btn_slot = hover.slot; }
			else if (hover.in_minimize_btn) { mc->hover_btn = 2; mc->hover_btn_slot = hover.slot; }

			// Buttons get arrow cursor (no resize/drag cursor on buttons)
			if (hover.in_close_btn || hover.in_minimize_btn) {
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
				else if (hover.in_title_bar)
					cursor_id = 5; // move cursor on title bar hover
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

		// Dynamic layout drag: title bar LMB controls carousel rotation.
		// Content clicks/drags pause rotation and forward to app normally.
		if (mc->dynamic_layout.mode >= 0 && mc->dynamic_layout.mode != 3) {
			if (lmb_just_pressed) {
				POINT pt;
				GetCursorPos(&pt);
				ScreenToClient(mc->hwnd, &pt);
				struct shell_hit_result hit = shell_raycast_hit_test(sys, mc, pt);
				// Close/minimize/background/taskbar → normal handler
				if (hit.slot < 0 || hit.in_close_btn || hit.in_minimize_btn) {
					goto normal_lmb_handling;
				}
				// Content click → pause rotation, focus window.
				// Mouse events forwarded to app via WndProc input forwarding.
				if (hit.in_content) {
					mc->dynamic_layout.angular_velocity = 0.0f;
					mc->dynamic_layout.pause_until_ns = UINT64_MAX; // pause until release
					if (hit.slot != mc->focused_slot) {
						mc->focused_slot = hit.slot;
						multi_compositor_update_input_forward(mc);
					}
				}
				// Only title bar drag controls carousel rotation
				if (hit.slot >= 0 && hit.in_title_bar) {
					// Start drag: capture angle + cursor
					mc->dynamic_layout.user_dragging = true;
					mc->dynamic_layout.drag_start_angle = mc->dynamic_layout.angle_offset;
					mc->dynamic_layout.drag_start_cursor = pt;
					mc->dynamic_layout.prev_angle_offset = mc->dynamic_layout.angle_offset;
					mc->dynamic_layout.prev_drag_ns = os_monotonic_get_ns();
					mc->dynamic_layout.angular_velocity = 0.0f; // stop auto-rotation
					mc->dynamic_layout.pause_until_ns = 0; // cancel any TAB pause
					// Focus the clicked window
					if (hit.slot != mc->focused_slot) {
						mc->focused_slot = hit.slot;
						multi_compositor_update_input_forward(mc);
					}
				}
			} else if (lmb_held && mc->dynamic_layout.user_dragging) {
				// Drag in progress: update angle from horizontal mouse delta
				POINT pt;
				GetCursorPos(&pt);
				ScreenToClient(mc->hwnd, &pt);
				float dx_px = (float)(pt.x - mc->dynamic_layout.drag_start_cursor.x);
				// Sensitivity: ~1 radian per half-display-width
				float disp_px_w = (float)sys->base.info.display_pixel_width;
				if (disp_px_w <= 0) disp_px_w = 3840;
				float sensitivity = 2.0f / disp_px_w; // radians per pixel
				mc->dynamic_layout.angle_offset = mc->dynamic_layout.drag_start_angle + dx_px * sensitivity;
				// Track velocity for momentum
				uint64_t now = os_monotonic_get_ns();
				float drag_dt = (float)(now - mc->dynamic_layout.prev_drag_ns) / 1e9f;
				if (drag_dt > 0.001f) {
					float dangle = mc->dynamic_layout.angle_offset - mc->dynamic_layout.prev_angle_offset;
					mc->dynamic_layout.angular_velocity = dangle / drag_dt;
					mc->dynamic_layout.prev_angle_offset = mc->dynamic_layout.angle_offset;
					mc->dynamic_layout.prev_drag_ns = now;
				}
			} else if (!lmb_held && mc->dynamic_layout.user_dragging) {
				// Release: resume with momentum (angular_velocity already set from tracking)
				mc->dynamic_layout.user_dragging = false;
				// Clamp momentum to reasonable range
				if (mc->dynamic_layout.angular_velocity > 3.0f) mc->dynamic_layout.angular_velocity = 3.0f;
				if (mc->dynamic_layout.angular_velocity < -3.0f) mc->dynamic_layout.angular_velocity = -3.0f;
			}
			// When LMB released in dynamic mode: resume rotation after 3s pause
			if (!lmb_held && !mc->dynamic_layout.user_dragging &&
			    mc->dynamic_layout.pause_until_ns == UINT64_MAX) {
				mc->dynamic_layout.pause_until_ns = os_monotonic_get_ns() + 3000000000ULL;
				mc->dynamic_layout.target_angle = mc->dynamic_layout.angle_offset;
			}
			// Only skip normal LMB handling when actively carousel-dragging
			if (mc->dynamic_layout.user_dragging) {
				goto after_lmb_handling;
			}
		}

		// Expose click: click a window to select it and return to previous layout
		if (mc->dynamic_layout.mode == 3 && lmb_just_pressed) {
			POINT pt;
			GetCursorPos(&pt);
			ScreenToClient(mc->hwnd, &pt);
			struct shell_hit_result hit = shell_raycast_hit_test(sys, mc, pt);
			if (hit.slot >= 0) {
				mc->focused_slot = hit.slot;
				multi_compositor_update_input_forward(mc);
				// Return to previous layout
				int prev = mc->dynamic_layout.prev_layout;
				mc->dynamic_layout.mode = -1;
				if (prev >= 0 && prev <= 3) {
					apply_layout(sys, mc, prev);
				} else {
					apply_layout(sys, mc, 0); // fallback to SBS
				}
			}
			goto after_lmb_handling;
		}

		normal_lmb_handling:
		if (lmb_just_pressed && !mc->title_drag.active && !mc->resize.active) {
			POINT pt;
			GetCursorPos(&pt);
			ScreenToClient(mc->hwnd, &pt);

			// Spatial raycast: cast ray from eye through cursor on display surface
			struct shell_hit_result hit = shell_raycast_hit_test(sys, mc, pt);

			// Edge/corner resize takes priority (unless clicking a title bar button)
			if (hit.slot >= 0 && hit.edge_flags != RESIZE_NONE &&
			    !hit.in_close_btn && !hit.in_minimize_btn) {
				mc->resize.active = true;
				mc->resize.slot = hit.slot;
				mc->resize.edges = hit.edge_flags;
				mc->resize.start_cursor = pt;
				mc->resize.start_pos_x = mc->clients[hit.slot].window_pose.position.x;
				mc->resize.start_pos_y = mc->clients[hit.slot].window_pose.position.y;
				mc->resize.start_width_m = mc->clients[hit.slot].window_width_m;
				mc->resize.start_height_m = mc->clients[hit.slot].window_height_m;
				if (hit.slot != mc->focused_slot) {
					mc->focused_slot = hit.slot;
				}
				if (mc->window != nullptr) {
					comp_d3d11_window_set_input_forward(mc->window, NULL, 0, 0, 0, 0);
				}
			} else {

			int hit_slot = hit.slot;
			bool in_title_bar = hit.in_title_bar;
			bool in_close_btn = hit.in_close_btn;
			bool in_minimize_btn = hit.in_minimize_btn;

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
				// Close button: send EXIT_REQUEST
				struct d3d11_service_compositor *fc = mc->clients[hit_slot].compositor;
				if (fc != nullptr && fc->xses != nullptr) {
					union xrt_session_event xse = XRT_STRUCT_INIT;
					xse.type = XRT_SESSION_EVENT_EXIT_REQUEST;
					xrt_session_event_sink_push(fc->xses, &xse);
					U_LOG_W("Multi-comp: close button → exit request for slot %d", hit_slot);
				}
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
					float disp_w_m = sys->base.info.display_width_m;
					float disp_h_m = sys->base.info.display_height_m;
					if (disp_w_m <= 0.0f) disp_w_m = 0.700f;
					if (disp_h_m <= 0.0f) disp_h_m = 0.394f;

					if (mc->clients[hit_slot].maximized) {
						// Restore from pre-max state
						mc->clients[hit_slot].window_pose = mc->clients[hit_slot].pre_max_pose;
						mc->clients[hit_slot].window_width_m = mc->clients[hit_slot].pre_max_width_m;
						mc->clients[hit_slot].window_height_m = mc->clients[hit_slot].pre_max_height_m;
						mc->clients[hit_slot].maximized = false;
						U_LOG_W("Multi-comp: restore slot %d from maximized", hit_slot);
					} else {
						// Save current state and maximize
						mc->clients[hit_slot].pre_max_pose = mc->clients[hit_slot].window_pose;
						mc->clients[hit_slot].pre_max_width_m = mc->clients[hit_slot].window_width_m;
						mc->clients[hit_slot].pre_max_height_m = mc->clients[hit_slot].window_height_m;
						mc->clients[hit_slot].window_pose.position.x = 0;
						mc->clients[hit_slot].window_pose.position.y = 0;
						mc->clients[hit_slot].window_width_m = disp_w_m * 0.95f;
						mc->clients[hit_slot].window_height_m = disp_h_m * 0.95f;
						mc->clients[hit_slot].maximized = true;
						U_LOG_W("Multi-comp: maximize slot %d", hit_slot);
					}
					slot_pose_to_pixel_rect(sys, &mc->clients[hit_slot],
					                        &mc->clients[hit_slot].window_rect_x,
					                        &mc->clients[hit_slot].window_rect_y,
					                        &mc->clients[hit_slot].window_rect_w,
					                        &mc->clients[hit_slot].window_rect_h);
					mc->clients[hit_slot].hwnd_resize_pending = true;
					mc->focused_slot = hit_slot;
					multi_compositor_update_input_forward(mc);
				} else {
					// Single click on title bar: start dragging
					mc->focused_slot = hit_slot;
					multi_compositor_update_input_forward(mc);

					mc->title_drag.active = true;
					mc->title_drag.slot = hit_slot;
					GetCursorPos(&mc->title_drag.start_cursor);
					ScreenToClient(mc->hwnd, &mc->title_drag.start_cursor);
					mc->title_drag.start_pos_x = mc->clients[hit_slot].window_pose.position.x;
					mc->title_drag.start_pos_y = mc->clients[hit_slot].window_pose.position.y;
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

					// Synthesize mouse-move + click to the app using spatial hit coords
					if (mc->clients[hit_slot].app_hwnd != nullptr) {
						float title_h_m = UI_TITLE_BAR_H_M;
						float content_local_x = hit.local_x_m;
						float content_local_y = hit.local_y_m - title_h_m;
						float win_w = mc->clients[hit_slot].window_width_m;
						float win_h = mc->clients[hit_slot].window_height_m;
						int app_x = (int)(content_local_x / win_w * (float)mc->clients[hit_slot].window_rect_w);
						int app_y = (int)(content_local_y / win_h * (float)mc->clients[hit_slot].window_rect_h);
						LPARAM lp = MAKELPARAM(app_x, app_y);
						PostMessage(mc->clients[hit_slot].app_hwnd, WM_MOUSEMOVE, 0, lp);
						PostMessage(mc->clients[hit_slot].app_hwnd, WM_LBUTTONDOWN,
						            MK_LBUTTON, lp);
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
					float new_x = mc->resize.start_pos_x;
					float new_y = mc->resize.start_pos_y;

					// Asymmetric: opposite edge stays fixed. Center shifts by delta/2.
					if (mc->resize.edges & RESIZE_RIGHT)  { new_w += dx_m; new_x += dx_m / 2.0f; }
					if (mc->resize.edges & RESIZE_LEFT)   { new_w -= dx_m; new_x += dx_m / 2.0f; }
					if (mc->resize.edges & RESIZE_BOTTOM) { new_h += dy_m; new_y -= dy_m / 2.0f; }
					if (mc->resize.edges & RESIZE_TOP)    { new_h -= dy_m; new_y -= dy_m / 2.0f; }

					float min_dim = 0.02f;
					if (new_w < min_dim) new_w = min_dim;
					if (new_h < min_dim) new_h = min_dim;
					if (new_w > disp_w_m * 0.95f) new_w = disp_w_m * 0.95f;
					if (new_h > disp_h_m * 0.95f) new_h = disp_h_m * 0.95f;

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
			// LMB released — end title drag. Nudge cursor to force WM_SETCURSOR update.
			mc->title_drag.active = false;
			mc->title_drag.slot = -1;
			{
				POINT p;
				GetCursorPos(&p);
				SetCursorPos(p.x, p.y);
			}
		}
	}
	after_lmb_handling:

	// Scroll in dynamic mode: adjust radius
	if (mc->dynamic_layout.mode >= 0 && mc->dynamic_layout.mode != 3) {
		if (mc->window != nullptr) {
			int32_t scroll = comp_d3d11_window_consume_scroll(mc->window);
			if (scroll != 0) {
				float delta = (float)scroll / (120.0f * 20.0f); // ~5% per notch
				mc->dynamic_layout.radius_m *= (1.0f + delta);
				if (mc->dynamic_layout.radius_m < 0.05f) mc->dynamic_layout.radius_m = 0.05f;
				if (mc->dynamic_layout.radius_m > 0.30f) mc->dynamic_layout.radius_m = 0.30f;
			}
		}
	}

	// Right-click: title bar RMB drag = rotation, content RMB = focus + forward to app.
	// Call GetAsyncKeyState ONCE to avoid consuming the & 1 press bit (Phase 2 lesson #4).
	{
		SHORT rmb_state = GetAsyncKeyState(VK_RBUTTON);
		bool rmb_held = (rmb_state & 0x8000) != 0;
		bool rmb_just_pressed = rmb_held && !mc->prev_rmb_held;
		mc->prev_rmb_held = rmb_held;

		if (rmb_held) {
			if (!mc->title_rmb_drag.active && rmb_just_pressed) {
				// RMB just pressed — check if on title bar to start rotation drag
				POINT pt;
				GetCursorPos(&pt);
				ScreenToClient(mc->hwnd, &pt);
				struct shell_hit_result rmb_hit = shell_raycast_hit_test(sys, mc, pt);
				if (rmb_hit.slot >= 0) {
					if (rmb_hit.slot != mc->focused_slot) {
						mc->focused_slot = rmb_hit.slot;
						multi_compositor_update_input_forward(mc);
					}
					if (rmb_hit.in_title_bar && !rmb_hit.in_close_btn && !rmb_hit.in_minimize_btn) {
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
					mc->current_layout = -1; // Manual rotation breaks layout
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
	}

	// Scroll wheel: Shift+Scroll = Z-depth, plain scroll = resize.
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

				// Clamp to minimum 2cm and maximum 80% of display
				float min_dim = 0.02f;
				float max_w = sys->base.info.display_width_m * 0.8f;
				float max_h = sys->base.info.display_height_m * 0.8f;
				if (max_w <= 0.0f) max_w = 0.560f;
				if (max_h <= 0.0f) max_h = 0.315f;

				if (new_w < min_dim) new_w = min_dim;
				if (new_h < min_dim) new_h = min_dim;
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

	// Tick dynamic layout (carousel/orbital/helix/expose) — continuously drives window poses.
	if (mc->dynamic_layout.mode >= 0) {
		dynamic_layout_tick(sys, mc, os_monotonic_get_ns());
		// Update input forwarding rect every frame — windows move continuously
		// in dynamic mode, so the forwarding rect from focus-change is stale.
		multi_compositor_update_input_forward(mc);
	}

	// Tick per-slot animations (for static layout transitions + entry animations).
	// Skipped when dynamic layout is active (it drives poses directly).
	if (mc->dynamic_layout.mode < 0) {
		uint64_t anim_now = os_monotonic_get_ns();
		for (int s = 0; s < D3D11_MULTI_MAX_CLIENTS; s++) {
			if (!mc->clients[s].active || !mc->clients[s].anim.active) continue;
			bool still_running = slot_animate_tick(&mc->clients[s], anim_now);
			slot_pose_to_pixel_rect(sys, &mc->clients[s],
			                        &mc->clients[s].window_rect_x,
			                        &mc->clients[s].window_rect_y,
			                        &mc->clients[s].window_rect_w,
			                        &mc->clients[s].window_rect_h);
			mc->clients[s].hwnd_resize_pending = true;
			(void)still_running;
		}
	}

	// Deferred HWND resize: resize app windows to their assigned sub-rects.
	// Uses SWP_ASYNCWINDOWPOS to avoid cross-process deadlock.
	for (int s = 0; s < D3D11_MULTI_MAX_CLIENTS; s++) {
		if (mc->clients[s].active && mc->clients[s].hwnd_resize_pending &&
		    mc->clients[s].app_hwnd != NULL) {
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
	}

	// Clear combined atlas to dark gray background each frame.
	// Prevents stale images after app close and provides background for windowed rendering.
	{
		float bg_color[4] = {0.102f, 0.102f, 0.102f, 1.0f}; // #1a1a1a
		sys->context->ClearRenderTargetView(mc->combined_atlas_rtv.get(), bg_color);
	}

	// Copy client atlas → combined atlas, crop to content dims, send to DP.
	// Render order: back-to-front by Z depth (painter's algorithm).
	// Windows farther from viewer (lower Z) render first, closer windows on top.
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

	uint32_t dp_view_w = sys->view_width;
	uint32_t dp_view_h = sys->view_height;
	for (int ri = 0; ri < render_count; ri++) {
		int s = render_order[ri];
		struct d3d11_service_compositor *cc = mc->clients[s].compositor;
		if (cc == nullptr || !cc->render.atlas_texture || !cc->render.atlas_srv) {
			continue;
		}
		// Client content dimensions (source texture size, same for both eyes).
		uint32_t cvw = mc->clients[s].content_view_w;
		uint32_t cvh = mc->clients[s].content_view_h;
		if (cvw == 0 || cvh == 0) {
			cvw = dp_view_w;
			cvh = dp_view_h;
		}

		// Combined atlas dimensions.
		uint32_t ca_w = sys->base.info.display_pixel_width;
		uint32_t ca_h = sys->base.info.display_pixel_height;
		if (ca_w == 0) ca_w = 3840;
		if (ca_h == 0) ca_h = 2160;
		uint32_t half_w = ca_w / sys->tile_columns;
		uint32_t half_h = ca_h / sys->tile_rows;

		// Per-eye projected pixel rects (parallax shift for windows at Z != 0).
		// Each eye sees the window at a different position on the display plane,
		// creating stereo depth on the lenticular display.
		int32_t eye_rect_x[2], eye_rect_y[2], eye_rect_w[2], eye_rect_h[2];
		for (int eye = 0; eye < 2; eye++) {
			int ei = (eye < (int)eye_pos.count) ? eye : 0;
			slot_pose_to_pixel_rect_for_eye(sys, &mc->clients[s],
			    eye_pos.eyes[ei].x, eye_pos.eyes[ei].y, eye_pos.eyes[ei].z,
			    &eye_rect_x[eye], &eye_rect_y[eye],
			    &eye_rect_w[eye], &eye_rect_h[eye]);
		}

		// Get client atlas dimensions for SRV
		D3D11_TEXTURE2D_DESC client_atlas_desc = {};
		cc->render.atlas_texture->GetDesc(&client_atlas_desc);

		// Shader blit each view from client atlas → combined atlas with scaling.
		// The blit constant buffer defines source UV rect and dest pixel rect.
		uint32_t num_views = sys->tile_columns * sys->tile_rows;
		for (uint32_t v = 0; v < num_views && v < XRT_MAX_VIEWS; v++) {
			uint32_t src_col = v % sys->tile_columns;
			uint32_t src_row = v / sys->tile_columns;

			// Source rect (pixels in client atlas)
			float src_px_x = static_cast<float>(src_col * cvw);
			float src_px_y = static_cast<float>(src_row * cvh);

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

			// Set scissor rect to this tile's bounds — GPU clips overflow uniformly
			// on all edges. No manual source/dest clipping needed.
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

			// Update constant buffer
			D3D11_MAPPED_SUBRESOURCE mapped;
			HRESULT hr = sys->context->Map(sys->blit_constant_buffer.get(), 0,
			                                D3D11_MAP_WRITE_DISCARD, 0, &mapped);
			if (FAILED(hr)) continue;
			BlitConstants *cb = static_cast<BlitConstants *>(mapped.pData);
			cb->src_rect[0] = src_px_x;
			cb->src_rect[2] = static_cast<float>(cvw);
			// GL clients render bottom-up; flip source Y to correct orientation
			if (cc->atlas_flip_y) {
				cb->src_rect[1] = src_px_y + static_cast<float>(cvh);
				cb->src_rect[3] = -static_cast<float>(cvh);
			} else {
				cb->src_rect[1] = src_px_y;
				cb->src_rect[3] = static_cast<float>(cvh);
			}
			cb->dst_offset[0] = dest_px_x;
			cb->dst_offset[1] = dest_px_y;
			cb->src_size[0] = static_cast<float>(client_atlas_desc.Width);
			cb->src_size[1] = static_cast<float>(client_atlas_desc.Height);
			cb->dst_size[0] = static_cast<float>(ca_w);
			cb->dst_size[1] = static_cast<float>(ca_h);
			cb->convert_srgb = 0.0f;
			cb->quad_mode = use_quad ? 1.0f : 0.0f;
			cb->dst_rect_wh[0] = dest_px_w;
			cb->dst_rect_wh[1] = dest_px_h;
			cb->padding2[0] = 0.0f;
			cb->padding2[1] = 0.0f;
			if (use_quad) {
				blit_set_quad_corners(cb, quad_corners, quad_w_vals);
			} else {
				memset(cb->quad_corners_01, 0, sizeof(cb->quad_corners_01));
				memset(cb->quad_corners_23, 0, sizeof(cb->quad_corners_23));
			}
			sys->context->Unmap(sys->blit_constant_buffer.get(), 0);

			// Pipeline setup
			sys->context->VSSetShader(sys->blit_vs.get(), nullptr, 0);
			sys->context->PSSetShader(sys->blit_ps.get(), nullptr, 0);
			sys->context->VSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
			sys->context->PSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
			ID3D11ShaderResourceView *srv = cc->render.atlas_srv.get();
			sys->context->PSSetShaderResources(0, 1, &srv);
			sys->context->PSSetSamplers(0, 1, sys->sampler_linear.addressof());

			// Render to combined atlas
			ID3D11RenderTargetView *ca_rtvs[] = {mc->combined_atlas_rtv.get()};
			sys->context->OMSetRenderTargets(1, ca_rtvs, nullptr);
			D3D11_VIEWPORT vp = {};
			vp.Width = static_cast<float>(ca_w);
			vp.Height = static_cast<float>(ca_h);
			vp.MaxDepth = 1.0f;
			sys->context->RSSetViewports(1, &vp);
			sys->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
			sys->context->IASetInputLayout(nullptr);
			sys->context->RSSetState(sys->rasterizer_state.get());
			sys->context->OMSetDepthStencilState(sys->depth_disabled.get(), 0);

			sys->context->Draw(4, 0);
		}

		// Draw title bar for this slot (inside render_order loop for correct z-order)
		{
			float tb_h_frac = (float)TITLE_BAR_HEIGHT_PX / (float)ca_h;
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

			if (tb_h_frac > 0.0f) {
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
					float tb_fy = wfy - tb_h_frac;
					float tox = col2 * half_w + wfx * half_w;
					float toy = row2 * half_h + tb_fy * half_h;
					float tow = wfw * half_w;
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
					ID3D11RenderTargetView *tb_rtvs[] = {mc->combined_atlas_rtv.get()};
					sys->context->OMSetRenderTargets(1, tb_rtvs, nullptr);
					sys->context->OMSetBlendState(sys->blend_opaque.get(), nullptr, 0xFFFFFFFF);
					sys->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
					sys->context->IASetInputLayout(nullptr);
					sys->context->RSSetState(sys->rasterizer_state.get());
					sys->context->OMSetDepthStencilState(sys->depth_disabled.get(), 0);
					D3D11_VIEWPORT tb_vp = {};
					tb_vp.Width = (float)ca_w; tb_vp.Height = (float)ca_h; tb_vp.MaxDepth = 1.0f;
					sys->context->RSSetViewports(1, &tb_vp);

					// Helper lambda: fill CB position fields for rotated or axis-aligned chrome blit
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
							} else { \
								(cb_ptr)->quad_mode = 0; \
								(cb_ptr)->dst_offset[0] = (aa_x); (cb_ptr)->dst_offset[1] = (aa_y); \
								(cb_ptr)->dst_rect_wh[0] = (aa_w); (cb_ptr)->dst_rect_wh[1] = (aa_h); \
								memset((cb_ptr)->quad_corners_01, 0, sizeof((cb_ptr)->quad_corners_01)); \
								memset((cb_ptr)->quad_corners_23, 0, sizeof((cb_ptr)->quad_corners_23)); \
							} \
						} while(0)

					// Title bar background
					{
						D3D11_MAPPED_SUBRESOURCE mapped;
						if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
						              D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
							BlitConstants *cb = static_cast<BlitConstants *>(mapped.pData);
							cb->src_rect[0] = 0.18f; cb->src_rect[1] = 0.20f;
							cb->src_rect[2] = 0.25f; cb->src_rect[3] = 1.0f;
							cb->src_size[0] = 1; cb->src_size[1] = 1;
							cb->dst_size[0] = (float)ca_w; cb->dst_size[1] = (float)ca_h;
							cb->convert_srgb = 2.0f;
							cb->padding2[0] = 0; cb->padding2[1] = 0;
							// Title bar: full width, above content
							CHROME_BLIT_POS(cb,
							    -win_hw, win_hh + tb_h_m, win_hw, win_hh,
							    tox, toy, tow, toh);
							sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
							sys->context->Draw(4, 0);
						}
					}
					// Close button (red)
					{
						D3D11_MAPPED_SUBRESOURCE mapped;
						if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
						              D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
							BlitConstants *cb = static_cast<BlitConstants *>(mapped.pData);
							bool close_hover = (mc->hover_btn == 1 && mc->hover_btn_slot == s);
							cb->src_rect[0] = close_hover ? 0.90f : 0.70f;
							cb->src_rect[1] = close_hover ? 0.25f : 0.15f;
							cb->src_rect[2] = close_hover ? 0.25f : 0.15f;
							cb->src_rect[3] = 1.0f;
							cb->src_size[0] = 1; cb->src_size[1] = 1;
							cb->dst_size[0] = (float)ca_w; cb->dst_size[1] = (float)ca_h;
							cb->convert_srgb = 2.0f;
							cb->padding2[0] = 0; cb->padding2[1] = 0;
							float btn_x = tox + tow - (float)CLOSE_BTN_WIDTH_PX;
							CHROME_BLIT_POS(cb,
							    win_hw - btn_w_m_val, win_hh + tb_h_m, win_hw, win_hh,
							    btn_x, toy, (float)CLOSE_BTN_WIDTH_PX, toh);
							sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
							sys->context->Draw(4, 0);
						}
					}
					// Minimize button (gray)
					{
						D3D11_MAPPED_SUBRESOURCE mapped;
						if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
						              D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
							BlitConstants *cb = static_cast<BlitConstants *>(mapped.pData);
							bool min_hover = (mc->hover_btn == 2 && mc->hover_btn_slot == s);
							cb->src_rect[0] = min_hover ? 0.45f : 0.30f;
							cb->src_rect[1] = min_hover ? 0.48f : 0.33f;
							cb->src_rect[2] = min_hover ? 0.50f : 0.36f;
							cb->src_rect[3] = 1.0f;
							cb->src_size[0] = 1; cb->src_size[1] = 1;
							cb->dst_size[0] = (float)ca_w; cb->dst_size[1] = (float)ca_h;
							cb->convert_srgb = 2.0f;
							cb->padding2[0] = 0; cb->padding2[1] = 0;
							float min_x = tox + tow - 2.0f * (float)CLOSE_BTN_WIDTH_PX;
							CHROME_BLIT_POS(cb,
							    win_hw - 2*btn_w_m_val, win_hh + tb_h_m, win_hw - btn_w_m_val, win_hh,
							    min_x, toy, (float)CLOSE_BTN_WIDTH_PX, toh);
							sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
							sys->context->Draw(4, 0);
						}
					}
					// Text + glyphs
					if (mc->font_atlas_srv && mc->clients[s].app_name[0] != '\0') {
						sys->context->OMSetBlendState(sys->blend_alpha.get(), nullptr, 0xFFFFFFFF);
						ID3D11ShaderResourceView *font_srv = mc->font_atlas_srv.get();
						sys->context->PSSetShaderResources(0, 1, &font_srv);
						sys->context->PSSetSamplers(0, 1, sys->sampler_point.addressof());

						const char *name = mc->clients[s].app_name;
						float gw = (float)GLYPH_W;
						float gh = (float)GLYPH_H;
						float gpad = (toh - gh) / 2.0f; // vertical centering
						if (gpad < 0) gpad = 0;
						// Glyph vertical centering in meters (for rotated path)
						float glyph_vpad_m = (tb_h_m - glyph_h_m) / 2.0f;
						if (glyph_vpad_m < 0) glyph_vpad_m = 0;

						int max_chars = ((int)tow - (int)gw - 2 * CLOSE_BTN_WIDTH_PX) / (int)gw;
						if (max_chars < 0) max_chars = 0;
						if (max_chars > 30) max_chars = 30;
						for (int ci = 0; ci < max_chars && name[ci] != '\0'; ci++) {
							unsigned char ch = (unsigned char)name[ci];
							if (ch < 0x20 || ch > 0x7E) ch = '?';
							int gi = ch - 0x20;
							D3D11_MAPPED_SUBRESOURCE mapped;
							if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
							              D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
								BlitConstants *cb = static_cast<BlitConstants *>(mapped.pData);
								cb->src_rect[0] = (float)(gi * 8); cb->src_rect[1] = 0;
								cb->src_rect[2] = 8; cb->src_rect[3] = 16;
								cb->src_size[0] = 768; cb->src_size[1] = 16;
								cb->dst_size[0] = (float)ca_w; cb->dst_size[1] = (float)ca_h;
								cb->convert_srgb = 0.0f;
								cb->padding2[0] = 0; cb->padding2[1] = 0;
								// Glyph local rect: left edge + 1 glyph padding + ci * glyph_w
								float gl_left = -win_hw + glyph_w_m + ci * glyph_w_m;
								float gl_top = win_hh + tb_h_m - glyph_vpad_m;
								float gl_right = gl_left + glyph_w_m;
								float gl_bottom = gl_top - glyph_h_m;
								CHROME_BLIT_POS(cb,
								    gl_left, gl_top, gl_right, gl_bottom,
								    tox + gw + ci * gw, toy + gpad, gw, gh);
								sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
								sys->context->Draw(4, 0);
							}
						}
						// X glyph on close button (centered in button)
						{
							int xg = 'X' - 0x20;
							float bx = tox + tow - (float)CLOSE_BTN_WIDTH_PX + ((float)CLOSE_BTN_WIDTH_PX - gw) / 2.0f;
							// Local: centered in close button
							float xg_left = win_hw - btn_w_m_val + (btn_w_m_val - glyph_w_m) / 2.0f;
							float xg_top = win_hh + tb_h_m - glyph_vpad_m;
							D3D11_MAPPED_SUBRESOURCE mapped;
							if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
							              D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
								BlitConstants *cb = static_cast<BlitConstants *>(mapped.pData);
								cb->src_rect[0] = (float)(xg * 8); cb->src_rect[1] = 0;
								cb->src_rect[2] = 8; cb->src_rect[3] = 16;
								cb->src_size[0] = 768; cb->src_size[1] = 16;
								cb->dst_size[0] = (float)ca_w; cb->dst_size[1] = (float)ca_h;
								cb->convert_srgb = 0.0f;
								cb->padding2[0] = 0; cb->padding2[1] = 0;
								CHROME_BLIT_POS(cb,
								    xg_left, xg_top, xg_left + glyph_w_m, xg_top - glyph_h_m,
								    bx, toy + gpad, gw, gh);
								sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
								sys->context->Draw(4, 0);
							}
						}
						// - glyph on minimize button (centered in button)
						{
							int dg = '-' - 0x20;
							float mx = tox + tow - 2.0f * (float)CLOSE_BTN_WIDTH_PX + ((float)CLOSE_BTN_WIDTH_PX - gw) / 2.0f;
							// Local: centered in minimize button
							float mg_left = win_hw - 2*btn_w_m_val + (btn_w_m_val - glyph_w_m) / 2.0f;
							float mg_top = win_hh + tb_h_m - glyph_vpad_m;
							D3D11_MAPPED_SUBRESOURCE mapped;
							if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
							              D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
								BlitConstants *cb = static_cast<BlitConstants *>(mapped.pData);
								cb->src_rect[0] = (float)(dg * 8); cb->src_rect[1] = 0;
								cb->src_rect[2] = 8; cb->src_rect[3] = 16;
								cb->src_size[0] = 768; cb->src_size[1] = 16;
								cb->dst_size[0] = (float)ca_w; cb->dst_size[1] = (float)ca_h;
								cb->convert_srgb = 0.0f;
								cb->padding2[0] = 0; cb->padding2[1] = 0;
								CHROME_BLIT_POS(cb,
								    mg_left, mg_top, mg_left + glyph_w_m, mg_top - glyph_h_m,
								    mx, toy + gpad, gw, gh);
								sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
								sys->context->Draw(4, 0);
							}
						}
						sys->context->OMSetBlendState(sys->blend_opaque.get(), nullptr, 0xFFFFFFFF);
						sys->context->PSSetSamplers(0, 1, sys->sampler_linear.addressof());
					}
					#undef CHROME_BLIT_POS
				}
			}
		}

		// Draw cyan focus border for this slot if focused (inside render_order loop for Z-depth ordering)
		if (s == mc->focused_slot && sys->blit_vs && sys->blit_ps) {
			bool fb_rotated = !quat_is_identity(&mc->clients[s].window_pose.orientation);
			float fb_hw = mc->clients[s].window_width_m / 2.0f;
			float fb_hh = mc->clients[s].window_height_m / 2.0f;
			float fb_tb_h = UI_TITLE_BAR_H_M;
			float disp_w_m_fb = sys->base.info.display_width_m;
			if (disp_w_m_fb <= 0.0f) disp_w_m_fb = 0.700f;
			float bw_m = 3.0f * disp_w_m_fb / (float)(ca_w > 0 ? ca_w : 3840);
			float bw = 3.0f;

			struct { float l, t, r, b; } border_edges_local[4] = {
				{-fb_hw,        fb_hh+fb_tb_h,        fb_hw,        fb_hh+fb_tb_h-bw_m},
				{-fb_hw,        -fb_hh+bw_m,           fb_hw,        -fb_hh},
				{-fb_hw,        fb_hh+fb_tb_h-bw_m,   -fb_hw+bw_m,  -fb_hh+bw_m},
				{fb_hw-bw_m,    fb_hh+fb_tb_h-bw_m,   fb_hw,        -fb_hh+bw_m},
			};

			sys->context->VSSetShader(sys->blit_vs.get(), nullptr, 0);
			sys->context->PSSetShader(sys->blit_ps.get(), nullptr, 0);
			sys->context->VSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
			sys->context->PSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
			sys->context->PSSetSamplers(0, 1, sys->sampler_linear.addressof());
			sys->context->OMSetBlendState(sys->blend_opaque.get(), nullptr, 0xFFFFFFFF);

			for (uint32_t bv = 0; bv < num_views && bv < XRT_MAX_VIEWS; bv++) {
				uint32_t bcol = bv % sys->tile_columns;
				uint32_t brow = bv / sys->tile_columns;
				int bei = (bcol < 2) ? (int)bcol : 0;
				int bei2 = (bei < (int)eye_pos.count) ? bei : 0;

				D3D11_RECT bsc;
				bsc.left = (LONG)(bcol * half_w); bsc.top = (LONG)(brow * half_h);
				bsc.right = (LONG)((bcol+1) * half_w); bsc.bottom = (LONG)((brow+1) * half_h);
				sys->context->RSSetScissorRects(1, &bsc);

				// Non-rotated fallback positions
				int32_t brd_y = eye_rect_y[bei] - TITLE_BAR_HEIGHT_PX;
				int32_t brd_h = eye_rect_h[bei] + (eye_rect_y[bei] - brd_y);
				float bfx = (float)eye_rect_x[bei] / ca_w;
				float bfy = (float)brd_y / ca_h;
				float bfw = (float)eye_rect_w[bei] / ca_w;
				float bfh = (float)brd_h / ca_h;
				float box = bcol * half_w + bfx * half_w;
				float boy = brow * half_h + bfy * half_h;
				float bew = bfw * half_w;
				float beh = bfh * half_h;
				struct { float x, y, w, h; } edges_aa[4] = {
					{box,          boy,          bew, bw},
					{box,          boy+beh-bw,   bew, bw},
					{box,          boy+bw,       bw,  beh-2*bw},
					{box+bew-bw,   boy+bw,       bw,  beh-2*bw},
				};

				D3D11_VIEWPORT bvp = {};
				bvp.Width = (float)ca_w; bvp.Height = (float)ca_h; bvp.MaxDepth = 1.0f;
				sys->context->RSSetViewports(1, &bvp);

				for (int e = 0; e < 4; e++) {
					D3D11_MAPPED_SUBRESOURCE mapped;
					if (FAILED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
					           D3D11_MAP_WRITE_DISCARD, 0, &mapped))) continue;
					BlitConstants *cb = static_cast<BlitConstants *>(mapped.pData);
					cb->src_rect[0] = 0; cb->src_rect[1] = 1; cb->src_rect[2] = 1; cb->src_rect[3] = 1;
					cb->src_size[0] = 1; cb->src_size[1] = 1;
					cb->dst_size[0] = (float)ca_w; cb->dst_size[1] = (float)ca_h;
					cb->convert_srgb = 2.0f; cb->padding2[0] = 0; cb->padding2[1] = 0;
					if (fb_rotated) {
						float corners[8], cw[4];
						project_local_rect_for_eye(sys, &mc->clients[s].window_pose.orientation,
						    mc->clients[s].window_pose.position.x, mc->clients[s].window_pose.position.y,
						    mc->clients[s].window_pose.position.z,
						    border_edges_local[e].l, border_edges_local[e].t,
						    border_edges_local[e].r, border_edges_local[e].b,
						    eye_pos.eyes[bei2].x, eye_pos.eyes[bei2].y, eye_pos.eyes[bei2].z,
						    bcol, brow, half_w, half_h, ca_w, ca_h, corners, cw);
						blit_set_quad_corners(cb, corners, cw);
						cb->dst_offset[0] = 0; cb->dst_offset[1] = 0;
						cb->dst_rect_wh[0] = 0; cb->dst_rect_wh[1] = 0;
					} else {
						cb->quad_mode = 0;
						cb->dst_offset[0] = edges_aa[e].x; cb->dst_offset[1] = edges_aa[e].y;
						cb->dst_rect_wh[0] = edges_aa[e].w; cb->dst_rect_wh[1] = edges_aa[e].h;
						memset(cb->quad_corners_01, 0, sizeof(cb->quad_corners_01));
						memset(cb->quad_corners_23, 0, sizeof(cb->quad_corners_23));
					}
					sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
					sys->context->Draw(4, 0);
				}
			}
		}
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

	// Draw taskbar at bottom if any windows are minimized
	{
		bool has_minimized = false;
		for (int i = 0; i < D3D11_MULTI_MAX_CLIENTS; i++) {
			if (mc->clients[i].active && mc->clients[i].minimized) {
				has_minimized = true;
				break;
			}
		}
		if (has_minimized && sys->blit_vs && sys->blit_ps) {
			uint32_t ca_w = sys->base.info.display_pixel_width;
			uint32_t ca_h = sys->base.info.display_pixel_height;
			if (ca_w == 0) ca_w = 3840;
			if (ca_h == 0) ca_h = 2160;
			uint32_t half_w = ca_w / sys->tile_columns;
			uint32_t half_h = ca_h / sys->tile_rows;

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
					cb->quad_mode = 0;
					cb->dst_rect_wh[0] = (float)half_w;
					cb->dst_rect_wh[1] = (float)TASKBAR_HEIGHT_PX;
					cb->padding2[0] = 0; cb->padding2[1] = 0;
					sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
					sys->context->Draw(4, 0);
				}

				// Draw indicators for each minimized app
				if (mc->font_atlas_srv) {
					sys->context->OMSetBlendState(sys->blend_alpha.get(), nullptr, 0xFFFFFFFF);
					ID3D11ShaderResourceView *font_srv = mc->font_atlas_srv.get();
					sys->context->PSSetShaderResources(0, 1, &font_srv);
					sys->context->PSSetSamplers(0, 1, sys->sampler_point.addressof());

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
								cb2->quad_mode = 0;
								cb2->dst_rect_wh[0] = pill_w;
								cb2->dst_rect_wh[1] = pill_h;
								cb2->padding2[0] = 0; cb2->padding2[1] = 0;
								sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
								sys->context->Draw(4, 0);
							}
							sys->context->OMSetBlendState(sys->blend_alpha.get(), nullptr, 0xFFFFFFFF);
						}

						// Draw first few chars of app name
						const char *name = mc->clients[i].app_name;
						int max_chars = 6;
						for (int ci = 0; ci < max_chars && name[ci] != '\0'; ci++) {
							unsigned char ch = (unsigned char)name[ci];
							if (ch < 0x20 || ch > 0x7E) ch = '?';
							int glyph_idx = ch - 0x20;
							D3D11_MAPPED_SUBRESOURCE mapped3;
							if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
							              D3D11_MAP_WRITE_DISCARD, 0, &mapped3))) {
								BlitConstants *cb3 = static_cast<BlitConstants *>(mapped3.pData);
								cb3->src_rect[0] = (float)(glyph_idx * 8);
								cb3->src_rect[1] = 0;
								cb3->src_rect[2] = 8;
								cb3->src_rect[3] = 16;
								cb3->dst_offset[0] = ind_x + 2.0f + ci * tgw;
								cb3->dst_offset[1] = ind_y;
								cb3->src_size[0] = 768; cb3->src_size[1] = 16;
								cb3->dst_size[0] = (float)ca_w;
								cb3->dst_size[1] = (float)ca_h;
								cb3->convert_srgb = 0.0f;
								cb3->quad_mode = 0;
								cb3->dst_rect_wh[0] = tgw;
								cb3->dst_rect_wh[1] = tgh;
								cb3->padding2[0] = 0; cb3->padding2[1] = 0;
								sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
								sys->context->Draw(4, 0);
							}
						}
						ind_idx++;
					}

					sys->context->OMSetBlendState(sys->blend_opaque.get(), nullptr, 0xFFFFFFFF);
					sys->context->PSSetSamplers(0, 1, sys->sampler_linear.addressof());
				}
			}
		}
	}

	// Send full combined atlas to DP — content is placed at sub-rect positions,
	// background is dark gray. The DP interlaces the entire image.
	// View width/height = full atlas divided by tile layout (not sub-rect).
	ID3D11ShaderResourceView *dp_input_srv = mc->combined_atlas_srv.get();
	{
		uint32_t aw = sys->base.info.display_pixel_width;
		uint32_t ah = sys->base.info.display_pixel_height;
		if (aw == 0) aw = sys->display_width;
		if (ah == 0) ah = sys->display_height;
		dp_view_w = aw / sys->tile_columns;
		dp_view_h = ah / sys->tile_rows;
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

	// Present
	if (mc->swap_chain) {
		mc->swap_chain->Present(1, 0);
	}

	// Signal WM_PAINT done
	if (mc->window != nullptr) {
		comp_d3d11_window_signal_paint_done(mc->window);
	}
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
					// The display processor handles mismatched stereo/target sizes via stretching.
					if (c->render.display_processor != nullptr && !in_size_move) {
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

	// Clear stereo render target
	if (c->render.atlas_rtv) {
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

	// Runtime-side 2D/3D toggle (V key) — polls qwerty driver each frame
#ifdef XRT_BUILD_DRIVER_QWERTY
	if (sys->xsysd != NULL) {
		bool force_2d = false;
		bool toggled = qwerty_check_display_mode_toggle(
		    sys->xsysd->xdevs, sys->xsysd->xdev_count, &force_2d);
		if (toggled) {
			struct xrt_device *head = sys->xsysd->static_roles.head;
			if (head != nullptr && head->hmd != NULL) {
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
			}
			// Switch display mode on the active DP.
			// In shell mode, the multi-comp owns the DP (per-client has none).
			struct xrt_display_processor_d3d11 *dp = nullptr;
			if (sys->shell_mode && sys->multi_comp != nullptr) {
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
				if (head != NULL) {
					xrt_device_set_property(head, XRT_DEVICE_PROPERTY_OUTPUT_MODE, render_mode);
				}
			}
		}
	}
#endif

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

		if (proj_view_count > 1 && !has_ui_layers && !any_mutex_acquired && !sys->shell_mode) {
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

		for (uint32_t eye = 0; eye < proj_view_count; eye++) {
			float src_x = static_cast<float>(layer->data.proj.v[eye].sub.rect.offset.w);
			float src_y = static_cast<float>(layer->data.proj.v[eye].sub.rect.offset.h);
			float src_w = static_cast<float>(layer->data.proj.v[eye].sub.rect.extent.w);
			float src_h = static_cast<float>(layer->data.proj.v[eye].sub.rect.extent.h);

			// In shell mode, use actual content dims for tile layout (atlas is native-sized).
			// In non-shell mode, use sys->view_width/height (atlas is DP-sized).
			uint32_t layout_vw = sys->shell_mode ? static_cast<uint32_t>(src_w) : sys->view_width;
			uint32_t layout_vh = sys->shell_mode ? static_cast<uint32_t>(src_h) : sys->view_height;
			uint32_t tile_x, tile_y;
			u_tiling_view_origin(eye, sys->tile_columns,
			                     layout_vw, layout_vh,
			                     &tile_x, &tile_y);

			// When client swapchain > atlas tile (window resized smaller),
			// scale source to fit tile. Otherwise use 1:1 placement and
			// let the crop-blit handle any excess (#102).
			float tile_w = static_cast<float>(layout_vw);
			float tile_h = static_cast<float>(layout_vh);
			bool needs_scale = !sys->shell_mode && (src_w > tile_w || src_h > tile_h);
			float dst_w = needs_scale ? tile_w : 0.0f;
			float dst_h = needs_scale ? tile_h : 0.0f;

			if (view_is_srgb[eye] && sys->blit_vs && view_scs[eye]->images[view_img_indices[eye]].srv) {
			} else {
				// Non-SRGB: use fast CopySubresourceRegion
				// D3D11 silently clips to destination bounds — no manual clamping needed
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

		// Track actual content dimensions from submitted rects (may differ from
		// sys->view_width/height for legacy apps that render at compromise scale).
		content_view_w = static_cast<uint32_t>(layer->data.proj.v[0].sub.rect.extent.w);
		content_view_h = static_cast<uint32_t>(layer->data.proj.v[0].sub.rect.extent.h);
		if (!sys->shell_mode) {
			// Clamp to atlas tile dims in case client swapchain > atlas (#102).
			// In shell mode, atlas is native-display-sized so no clamping needed.
			if (content_view_w > sys->view_width) content_view_w = sys->view_width;
			if (content_view_h > sys->view_height) content_view_h = sys->view_height;
		}

		// Store content dims on multi-comp slot for multi_compositor_render
		if (sys->shell_mode && sys->multi_comp != nullptr) {
			for (int s = 0; s < D3D11_MULTI_MAX_CLIENTS; s++) {
				if (sys->multi_comp->clients[s].active &&
				    sys->multi_comp->clients[s].compositor == c) {
					sys->multi_comp->clients[s].content_view_w = content_view_w;
					sys->multi_comp->clients[s].content_view_h = content_view_h;
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

	// Shell mode: per-client atlas rendering is done. The multi-compositor
	// composites all client atlases into the combined atlas and presents.
	if (sys->shell_mode) {
		std::lock_guard<std::recursive_mutex> render_lock(sys->render_mutex);
		multi_compositor_render(sys);
		return XRT_SUCCESS;
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
		input_srv = service_crop_atlas_for_dp(sys, &c->render, content_view_w, content_view_h);
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

	// Unregister from multi-compositor before cleanup
	if (sys->shell_mode) {
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

	// Initialize layer accumulator
	std::memset(&c->layer_accum, 0, sizeof(c->layer_accum));

	// Initialize per-client render resources (window, swap chain, display processor)
	// Get external window handle if app provided one via XR_EXT_win32_window_binding
	void *external_hwnd = nullptr;
	if (xsi != nullptr) {
		external_hwnd = xsi->external_window_handle;
	}

	// Activate shell mode from system compositor info (set by ipc_server_process.c
	// after init_all, before any client connects)
	if (sys->base.info.shell_mode && !sys->shell_mode) {
		sys->shell_mode = true;
		U_LOG_W("Shell mode activated for D3D11 service system");
	}

	xrt_result_t res_ret = init_client_render_resources(sys, external_hwnd, sys->xsysd, &c->render);
	if (res_ret != XRT_SUCCESS) {
		U_LOG_E("Failed to initialize client render resources");
		delete c;
		return res_ret;
	}

	// Register with multi-compositor in shell mode
	if (sys->shell_mode) {
		// Ensure multi_comp struct exists for registration
		// Eagerly create multi-comp output (window + DP) on first client connect.
		// This ensures the DP is available for ipc_try_get_sr_view_poses
		// when the client calls xrLocateViews (before the first layer_commit).
		xrt_result_t mc_ret = multi_compositor_ensure_output(sys);
		if (mc_ret != XRT_SUCCESS) {
			U_LOG_E("Shell mode: failed to create multi-comp output");
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
			U_LOG_E("Shell mode: max clients (%d) reached", D3D11_MULTI_MAX_CLIENTS);
			fini_client_render_resources(&c->render);
			delete c;
			return XRT_ERROR_D3D11;
		}

		// Store app's HWND in the slot (for future shell commands: resize, input forwarding).
		// HWND resize is done CLIENT-SIDE in oxr_session_create (before the IPC call)
		// because cross-process SetWindowPos deadlocks when called from the IPC handler.
		sys->multi_comp->clients[slot].app_hwnd = (HWND)external_hwnd;

		// Get app name from HWND title for title bar display.
		// If another slot already has the same name, append "-2", "-3", etc.
		{
			char base_name[128] = {0};
			if (external_hwnd != 0) {
				int len = GetWindowTextA((HWND)external_hwnd, base_name, sizeof(base_name));
				if (len <= 0) {
					snprintf(base_name, sizeof(base_name), "App %d", slot);
				}
			} else {
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
                                 struct xrt_system_compositor **out_xsysc)
{
	U_LOG_W("Creating D3D11 service system compositor (xsysd=%p)", (void *)xsysd);

	// Allocate system compositor
	struct d3d11_service_system *sys = new d3d11_service_system();
	std::memset(&sys->base, 0, sizeof(sys->base));

	sys->xdev = xdev;
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

	HRESULT hr = D3D11CreateDevice(
	    nullptr,                     // Default adapter
	    D3D_DRIVER_TYPE_HARDWARE,
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
		auto factory = (xrt_dp_factory_d3d11_fn_t)sys->base.info.dp_factory_d3d11;
		struct xrt_display_processor_d3d11 *tmp_dp = nullptr;
		xrt_result_t dp_ret = factory(sys->device.get(), sys->context.get(), NULL, &tmp_dp);
		if (dp_ret == XRT_SUCCESS && tmp_dp != nullptr) {
			uint32_t disp_px_w = 0, disp_px_h = 0;
			int32_t disp_left = 0, disp_top = 0;
			if (xrt_display_processor_d3d11_get_display_pixel_info(
			        tmp_dp, &disp_px_w, &disp_px_h, &disp_left, &disp_top) &&
			    disp_px_w > 0 && disp_px_h > 0) {
				// Compute per-view dims using tile layout from active rendering mode
				sys->view_width = disp_px_w / sys->tile_columns;
				sys->view_height = disp_px_h / sys->tile_rows;
				sys->display_width = sys->tile_columns * sys->view_width;
				sys->display_height = sys->tile_rows * sys->view_height;
				sys->output_width = disp_px_w;
				sys->output_height = disp_px_h;
				U_LOG_W("Display processor pixel info: %ux%u, view=%ux%u per eye (tiles %ux%u)",
				        disp_px_w, disp_px_h, sys->view_width, sys->view_height,
				        sys->tile_columns, sys->tile_rows);
			} else {
				U_LOG_W("Display processor created but pixel info unavailable, using defaults");
			}

			// Query display physical dimensions
			float w_m = 0.0f, h_m = 0.0f;
			if (xrt_display_processor_d3d11_get_display_dimensions(tmp_dp, &w_m, &h_m)) {
				sys->base.info.display_width_m = w_m;
				sys->base.info.display_height_m = h_m;
			}

			xrt_display_processor_d3d11_destroy(&tmp_dp);
		} else {
			U_LOG_W("Temporary display processor creation failed, using default dimensions");
		}
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

	// Fill system compositor info
	sys->base.info.max_layers = XRT_MAX_LAYERS;
	sys->base.info.views[0].recommended.width_pixels = sys->view_width;
	sys->base.info.views[0].recommended.height_pixels = sys->view_height;
	sys->base.info.views[0].max.width_pixels = sys->view_width * 2;
	sys->base.info.views[0].max.height_pixels = sys->view_height * 2;
	sys->base.info.views[1] = sys->base.info.views[0];

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

	U_LOG_W("D3D11 service system compositor created: view=%ux%u/eye, stereo=%ux%u, output=%ux%u @ %.0fHz",
	        sys->view_width, sys->view_height,
	        sys->display_width, sys->display_height,
	        sys->output_width, sys->output_height, sys->refresh_rate);

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
	// In shell mode, use the multi-comp's DP (per-client compositors have no DP).
	// In normal mode, use the active compositor's DP.
	struct xrt_display_processor_d3d11 *dp = nullptr;
	if (sys->shell_mode && sys->multi_comp != nullptr) {
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
comp_d3d11_service_get_display_dimensions(struct xrt_system_compositor *xsysc,
                                           float *out_width_m,
                                           float *out_height_m)
{
	if (xsysc == NULL || out_width_m == NULL || out_height_m == NULL) {
		return false;
	}

	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);

	// Try to get display dimensions from display processor.
	// In shell mode, use multi-comp's DP; in normal mode, use active compositor's DP.
	struct xrt_display_processor_d3d11 *dp = nullptr;
	if (sys->shell_mode && sys->multi_comp != nullptr) {
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

	// In shell mode, use multi-comp's window and DP.
	// In normal mode, use the active compositor's.
	struct xrt_display_processor_d3d11 *dp = nullptr;
	HWND metrics_hwnd = nullptr;

	if (sys->shell_mode && sys->multi_comp != nullptr &&
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

	// Get window client rect
	RECT rect;
	if (!GetClientRect(metrics_hwnd, &rect)) {
		out_metrics->valid = false;
		return false;
	}
	uint32_t win_px_w = static_cast<uint32_t>(rect.right - rect.left);
	uint32_t win_px_h = static_cast<uint32_t>(rect.bottom - rect.top);
	if (win_px_w == 0 || win_px_h == 0) {
		out_metrics->valid = false;
		return false;
	}

	// Get window screen position
	POINT client_origin = {0, 0};
	ClientToScreen(metrics_hwnd, &client_origin);

	// Compute pixel size (meters per pixel)
	float pixel_size_x = disp_w_m / (float)disp_px_w;
	float pixel_size_y = disp_h_m / (float)disp_px_h;

	// Window physical size
	float win_w_m = (float)win_px_w * pixel_size_x;
	float win_h_m = (float)win_px_h * pixel_size_y;

	// Window center offset in meters
	float win_center_px_x = (float)(client_origin.x - disp_left) + (float)win_px_w / 2.0f;
	float win_center_px_y = (float)(client_origin.y - disp_top) + (float)win_px_h / 2.0f;
	float disp_center_px_x = (float)disp_px_w / 2.0f;
	float disp_center_px_y = (float)disp_px_h / 2.0f;
	float offset_x_m = (win_center_px_x - disp_center_px_x) * pixel_size_x;
	float offset_y_m = -((win_center_px_y - disp_center_px_y) * pixel_size_y);

	memset(out_metrics, 0, sizeof(*out_metrics));
	out_metrics->display_width_m = disp_w_m;
	out_metrics->display_height_m = disp_h_m;
	out_metrics->display_pixel_width = disp_px_w;
	out_metrics->display_pixel_height = disp_px_h;
	out_metrics->display_screen_left = disp_left;
	out_metrics->display_screen_top = disp_top;
	out_metrics->window_pixel_width = win_px_w;
	out_metrics->window_pixel_height = win_px_h;
	out_metrics->window_screen_left = static_cast<int32_t>(client_origin.x);
	out_metrics->window_screen_top = static_cast<int32_t>(client_origin.y);
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
	if (!sys->shell_mode || sys->multi_comp == nullptr) {
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

	// Recompute pixel rect from pose
	slot_pose_to_pixel_rect(sys, &mc->clients[slot],
	                        &mc->clients[slot].window_rect_x,
	                        &mc->clients[slot].window_rect_y,
	                        &mc->clients[slot].window_rect_w,
	                        &mc->clients[slot].window_rect_h);

	mc->clients[slot].hwnd_resize_pending = true;

	U_LOG_W("Shell: set window pose slot %d pos=(%.3f,%.3f,%.3f) size=%.3fx%.3f → rect=(%u,%u,%u,%u)",
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
	if (!sys->shell_mode || sys->multi_comp == nullptr) return false;

	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);
	struct d3d11_multi_compositor *mc = sys->multi_comp;

	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);

	int slot = multi_comp_find_slot(mc, c);
	if (slot < 0) return false;

	mc->clients[slot].minimized = !visible;
	U_LOG_W("Shell: set_visibility slot %d visible=%d", slot, visible);

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
	if (!sys->shell_mode || sys->multi_comp == nullptr) return false;

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
	if (!sys->shell_mode || sys->multi_comp == nullptr) {
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
	// Shell mode: per-client compositors don't own windows (multi-comp does).
	// The shell app provides its own HWND and does its own Kooima projection.
	// Returning false ensures the IPC view pose path uses the display-centric
	// Kooima with real DP eye tracking, not the camera-centric qwerty path.
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	if (sys->shell_mode) {
		return false;
	}

	// Non-shell IPC clients always get Monado-owned windows.
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
