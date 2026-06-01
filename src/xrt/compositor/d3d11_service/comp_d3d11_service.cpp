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

#include "shared/ipc_protocol.h" // workspace IPC structs

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
#include "util/comp_dp_factory.h"

#include "comp_d3d11_window.h"

#include "math/m_api.h"
#include "math/m_vec3.h"
#include "math/m_display3d_view.h"

#include "d3d/d3d_d3d11_fence.hpp"
#include "d3d/d3d_dxgi_formats.h"

#include "util/u_hud.h"
#include "util/u_tiling.h"
#include <displayxr_mcp/mcp_capture.h>

#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty_interface.h"
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <dcomp.h>
#include <d3dcompiler.h>

#include <wil/com.h>
#include <wil/result.h>

#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <mutex>
#include <map>
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

	//! DirectComposition resources (transparent path only — null on default path).
	//! Set when the client requested transparentBackgroundEnabled and we have a
	//! non-null external HWND. The swap_chain above was created via
	//! CreateSwapChainForComposition (HWND-less) and is bound to the app's HWND
	//! through DComp instead of via DXGI, so DWM can blend per-pixel alpha.
	wil::com_ptr<IDCompositionDevice> dcomp_device;
	wil::com_ptr<IDCompositionTarget> dcomp_target;
	wil::com_ptr<IDCompositionVisual> dcomp_visual;

	//! Chroma-key color (0x00BBGGRR / Win32 COLORREF) for the post-weave alpha-conversion
	//! shader pass. Zero disables. Lazy-initialized resources for that pass below.
	uint32_t chroma_key_color;
	wil::com_ptr<ID3D11VertexShader>     ck_vs;
	wil::com_ptr<ID3D11PixelShader>      ck_ps;
	wil::com_ptr<ID3D11Texture2D>        ck_intermediate;
	wil::com_ptr<ID3D11ShaderResourceView> ck_intermediate_srv;
	wil::com_ptr<ID3D11Buffer>           ck_constants;
	wil::com_ptr<ID3D11SamplerState>     ck_sampler;
	UINT                                  ck_intermediate_w;
	UINT                                  ck_intermediate_h;
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

	//! Phase 1 diagnostic — last logged zero-copy decision per client.
	//! Drives the one-shot `[ZC]` breadcrumb in compositor_layer_commit:
	//! we only emit a log line when the decision FLIPS, not every frame.
	bool zc_last_logged_set;
	bool zc_last_logged_value;
	const char *zc_last_logged_reason;

	//! Phase 1 diagnostic — `[MUTEX]` rate-limited (1× / 10 s) per-client
	//! summary of KeyedMutex acquire health on the service render thread.
	//! Acquire latencies and timeout counts accumulate during the window;
	//! the window flush emits one `U_LOG_I` line and resets the counters.
	int64_t mutex_window_start_ns;
	uint32_t mutex_timeouts_in_window;
	uint32_t mutex_acquires_in_window;
	int64_t mutex_acquire_total_ns_in_window;

	//! Phase 1 diagnostic — `[CLIENT_FRAME_NS]` env-gated per-client
	//! commit-to-commit interval. Measures the rate at which THIS client
	//! is actually submitting frames (its `xrEndFrame` cadence as seen on
	//! the service side). Works in both workspace and standalone modes —
	//! diff the same client's number across the two for an apples-to-
	//! apples per-app frame-rate comparison.
	int64_t last_commit_ns;

	//! Phase 2 — per-IPC-client shared `ID3D11Fence` that replaces the
	//! per-view `IDXGIKeyedMutex::AcquireSync` CPU wait with a GPU-side
	//! `ID3D11DeviceContext4::Wait`. Created at session-create on the
	//! service device and exported as an NT handle to the client; the
	//! client increments `last_signaled_fence_value` once per `xrEndFrame`
	//! after submitting render commands and ships the new value over the
	//! `compositor_layer_sync` IPC. The service per-view loop reads the
	//! atomic, queues a GPU wait if it advanced, and skip-blits the view
	//! (reusing the persistent atlas slot) if the value is stale.
	//! `nullptr` ⇒ legacy KeyedMutex path runs unchanged (WebXR bridge,
	//! `_ipc` apps without fence support).
	wil::com_ptr<ID3D11Fence> workspace_sync_fence;
	HANDLE workspace_sync_fence_handle;            // shared NT handle for IPC export; nullptr when disabled
	std::atomic<uint64_t> last_signaled_fence_value;
	uint64_t last_composed_fence_value[XRT_MAX_VIEWS];

	//! Phase 2 diagnostic — `[FENCE]` rate-limited (1× / 10 s) per-client
	//! summary of GPU-wait queueing and stale-view occurrence. Mirrors the
	//! `[MUTEX]` window pattern above so the bench harness can A/B compare
	//! `acquires` vs `waits_queued` directly.
	int64_t fence_window_start_ns;
	uint32_t fence_waits_queued_in_window;
	uint32_t fence_stale_views_in_window;
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


//! spec_version 21: one entry in the keyed overlay map (see d3d11_service_system::overlays).
//! Presence in the map == visible; the controller removes an overlay by pushing
//! !visible / NULL swapchain for its id (erases the entry). z = 0, no disparity.
struct overlay_slot
{
	struct xrt_swapchain *xsc; //!< borrowed; controller owns lifetime
	float anchor_x;            //!< Normalized display position X [0,1] of dock point
	float anchor_y;            //!< Normalized display position Y [0,1] of dock point
	float pivot_x;             //!< Normalized sprite UV X [0,1] mapped onto anchor
	float pivot_y;             //!< Normalized sprite UV Y [0,1] mapped onto anchor
	float size_w_m;            //!< Physical overlay width in meters
	float size_h_m;            //!< Physical overlay height in meters
	bool  stereo_sbs;          //!< spec_version 19: image is side-by-side stereo
};

//! Cap on simultaneously-composited overlays — a buggy controller can't grow the
//! map unbounded. The shell needs ~3 (taskbar/launcher/toast); 16 is ample.
#define D3D11_SERVICE_MAX_OVERLAYS 16


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

	//! spec_version 13: controller-pushed cursor sprite source. The controller
	//! creates a session-global swapchain (xrCreateWorkspaceCursorSwapchainEXT),
	//! renders its sprite into it, and points the runtime at it via
	//! xrSetWorkspaceCursorEXT. The runtime samples the swapchain's latest
	//! released image in the cursor render pass. NULL = cursor hidden.
	//!
	//! cursor_xsc is a borrowed ref (the controller owns the lifetime via the
	//! XrSwapchain handle). When the controller destroys the swapchain we
	//! clear this on the runtime side via either an explicit set-cursor with
	//! XR_NULL_HANDLE or implicitly when the IPC swapchain table releases.
	struct xrt_swapchain *cursor_xsc;
	float cursor_hot_x;            //!< Sprite UV X of click point [0,1]
	float cursor_hot_y;            //!< Sprite UV Y of click point [0,1]
	float cursor_size_m;           //!< Physical size (width = height)
	bool  cursor_visible;          //!< Controller can hide without releasing swapchain

	//! spec_version 17/21: controller-pushed overlay sources (display-spanning UI,
	//! e.g. taskbar + launcher + toast). Same borrowed-ref model as cursor_xsc, but
	//! docked at z = 0 (zero disparity) — no raycast, no per-eye disparity. The
	//! controller creates session-global swapchains (xrCreateWorkspaceOverlaySwapchainEXT),
	//! renders UI into them, and docks each via xrSetWorkspaceOverlayEXT with an
	//! overlayId. The map is keyed by that id; std::map iterates ascending so the
	//! composite draws low ids behind high ids (z-order). An entry's presence == it
	//! is visible — !visible / NULL swapchain erases its id. Guarded by render_mutex.
	std::map<uint32_t, overlay_slot> overlays;

	//! #308 (spec_version 18): controller input grab is active (modal UI like the
	//! launcher band is up). While set, the cursor renders at z = 0 (zero
	//! disparity) instead of the per-frame window raycast depth, so it stays
	//! aligned with the z=0 overlay the controller is showing.
	bool  input_grabbed;

	//! MCP capture_frame cross-thread hand-off (Phase B slice 7).
	struct mcp_capture_request mcp_capture;

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
	//! #308: premultiplied box-blur variant of blit_ps. Used only for the
	//! empty-state splash logo while it's pushed behind the launcher band, to
	//! give it depth-of-field. Blur radius (UV) comes from glow_falloff.
	wil::com_ptr<ID3D11PixelShader> blit_blur_ps;
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

	/*!
	 * @name Phase 3 — `[RENDER]` diagnostic.
	 *
	 * Per-render measurement of the workspace-mode render-loop machinery.
	 * Emitted 1×/10 s from `capture_render_thread_func`. Atomics because the
	 * counters are touched from both the capture thread and per-client
	 * `compositor_layer_commit` threads (one per IPC client).
	 *
	 * Fields are reset (atomic exchange) at each window emission. Inline
	 * `{0}` initializers keep them well-defined across `new d3d11_service_system()`.
	 *
	 * Format: `[RENDER] capture_renders=N capture_avg_us=R client_renders=K
	 *          client_skips=S client_avg_us=R wait_avg_us=W window_s=10`
	 *  - `capture_*` come from `capture_render_thread_func`
	 *  - `client_*` come from `compositor_layer_commit` workspace path
	 *  - `client_skips` is the 14ms throttle-skip count
	 *  - `wait_avg_us` is `sys->render_mutex` acquire latency, averaged across
	 *    both drivers
	 * @{
	 */
	std::atomic<int64_t> render_diag_window_start_ns{0};
	std::atomic<uint32_t> render_diag_capture_renders{0};
	std::atomic<int64_t> render_diag_capture_render_total_ns{0};
	std::atomic<uint32_t> render_diag_client_renders{0};
	std::atomic<uint32_t> render_diag_client_skips{0};
	std::atomic<int64_t> render_diag_client_render_total_ns{0};
	std::atomic<int64_t> render_diag_mutex_wait_total_ns{0};
	std::atomic<uint32_t> render_diag_mutex_wait_count{0};
	/*! @} */

	//! Phase 5b — rate-limited cache of `xrt_display_processor_d3d11_get_hardware_3d_state`.
	//! The vendor SDK call is synchronous and blocks ~10 ms per invocation
	//! (measured on Leia SR dev box). Calling it per-cube-per-frame from
	//! `compositor_layer_commit` saturated the IPC-side hot path with 4 cubes
	//! at workspace cadence and pinned per-cube fps to ~30 fps. The fix:
	//! a CAS-protected once-per-window poll caches the result; all subsequent
	//! commits within the window read the cache atomically. The state-change
	//! broadcast logic (line ~9886) acts on the cached/freshly-polled value
	//! exactly as before. 100 ms TTL gives 10 Hz polling — way more than
	//! enough for hardware-mode-switch detection.
	std::atomic<int64_t> last_3d_state_poll_ns{0};
	std::atomic<bool>    cached_3d_state{false};
	std::atomic<bool>    cached_3d_state_valid{false};

	//! Monotonic ns when the most recent acked-flip landed (FLIPPING -> IDLE).
	//! The vendor poll suppresses counter-correction flips for
	//! `kPostFlipCooldownNs` after this stamp — the vendor SDK's cached
	//! `is_3d` state can lag the post-flip reality by a frame or two, and
	//! without the gate the poll would request a flip back to the prior mode
	//! within ~200 ms of the user's V keypress. Zero = no flip has landed yet.
	std::atomic<int64_t> last_flip_landed_ns{0};

	// Note: a service-level eye-pos cache previously lived here as a
	// throw-rate mitigation (#248). It moved into the Leia DP itself,
	// which now subscribes to the SR SDK's EyePairStream and maintains
	// the snapshot as a vendor-internal concern (separation of concerns:
	// the compositor must not own eye-tracking caching). All consumers
	// here call the DP via `xrt_display_processor_d3d11_get_predicted_eye_positions`
	// which is now a cheap snapshot read.

	//! Phase 2.C spec_version 8: auto-reset Win32 event signaled whenever an
	//! async workspace state change occurs that the controller might want to
	//! react to (input event pushed onto the public ring, focused-slot
	//! transition, hovered-slot transition). Created lazily on first
	//! workspace_acquire_wakeup_event RPC; the IPC handler DuplicateHandles
	//! it into the controller's process. Auto-reset semantics: SetEvent
	//! wakes one waiter and immediately clears; controller is expected to
	//! drain ALL pending state on each wake. NULL on platforms that don't
	//! support Win32 events (currently macOS / Linux — wakeup event is a
	//! Windows-only optimization).
	void *workspace_wakeup_event; // HANDLE on Win32, opaque void* in header
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
#define UI_GLYPH_W_M        0.0035f  //!< Glyph width: 3.5mm (balanced aspect ratio)
#define UI_GLYPH_H_M        0.005f   //!< Glyph height: 5mm
#define UI_RESIZE_ZONE_M    0.003f   //!< Resize detection zone: 3mm
#define UI_EDGE_FEATHER_PX  2.0f     //!< Edge feather width in pixels (all windows)
// C5: UI_GLOW_* constants deleted with the focus-rim glow render in
// comp_d3d11_service_render_pass. Controllers can render their own
// focus glow as a separate chrome layer if needed.

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
 * Acked-flip + curtain state machine for workspace display-mode transitions
 * (issue #234). Replaces the historical "flip DP + sync_tile_layout
 * immediately, hope apps catch up" pattern that exposed a raw-atlas glitch
 * on every IPC-mode mode-flip (bridge V-toggle, workspace V-toggle / focus-
 * adaptive, modal-open). State lives on d3d11_multi_compositor::mode_flip.
 *
 * IDLE         → no transition pending.
 * WAITING_ACK  → event broadcast at request time; curtain ON; per-frame tick
 *                waits for all active slots (less auto-acked capture/2D
 *                slots) to submit a frame whose projection-layer extent
 *                matches the target mode's per-view dims, OR for a fairness
 *                timeout. Then advances to FLIPPING.
 * FLIPPING     → DP request_display_mode + sync_tile_layout + active mode
 *                index write have landed; curtain stays ON while the vendor
 *                hardware transition completes (polled via
 *                get_hardware_3d_state, bounded by a safety ceiling).
 */
enum mode_flip_phase
{
	MFP_IDLE = 0,
	MFP_WAITING_ACK,
	MFP_FLIPPING,
};

//! Frames to wait for all slots to ack the new layout before force-flipping
//! the DP anyway (curtain still masks the un-acked slot's brief mismatch).
//! ~500 ms at 60 Hz.
#define MFP_ACK_TIMEOUT_FRAMES 30

//! Frames to hold the curtain ON after DP request_display_mode lands before
//! lifting it unconditionally — covers the vendor's hardware transition
//! window when get_hardware_3d_state lies or hasn't converged. The 60-frame
//! bump was a band-aid for Issue 3 (per-slot stride mismatch during the
//! catch-up window). Now that the workspace per-tile blit uses each slot's
//! OWN stride snapshot (write/read stay coupled per-slot), the curtain
//! only needs to mask SR SDK hardware settle (~250 ms ≈ 16 frames).
#define MFP_HW_CEILING_FRAMES 16

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

	//! Workspace acked-flip state (#234): true once this slot has submitted a
	//! projection layer whose per-view dims differ from the snapshot taken at
	//! request_mode_flip time. Workspace apps submit window-scaled extents
	//! (not the canonical rendering_modes[i].view_width_pixels), so we detect
	//! "app caught up to the broadcast event" by extent CHANGE rather than
	//! by absolute match. Reset by multi_compositor_request_mode_flip; set by
	//! the ack-detect block in compositor_layer_commit's workspace branch.
	//! Read by multi_compositor_apply_pending_mode_flip to decide quorum.
	bool acked_target_mode;

	//! Per-slot extent snapshots used by the ack detector (#234).
	//! `last_commit_view_w/h` are refreshed on every workspace-mode
	//! compositor_layer_commit. `pre_flip_view_w/h` are snapshotted by
	//! multi_compositor_request_mode_flip from the current last_commit
	//! values; the ack fires when a subsequent commit's extent differs from
	//! the pre_flip snapshot (meaning the app re-rendered after consuming
	//! XrEventDataRenderingModeChangedEXT).
	uint32_t last_commit_view_w;
	uint32_t last_commit_view_h;
	uint32_t pre_flip_view_w;
	uint32_t pre_flip_view_h;

	//! Per-slot stride snapshot (#234, Issue 3). Captured at content-clamp
	//! time in compositor_layer_commit using `atlas_w / sys->tile_columns`
	//! AT THE TIME OF WRITE — same formula the clamp itself uses, so write
	//! and read stay coupled through the slot's own snapshot even when the
	//! global sys->tile_columns flips ahead of an unacked slot. Without
	//! this, multi_compositor_render reads with the NEW global stride while
	//! the slot's atlas still holds OLD-stride content; visible as L/R
	//! mashing during the post-flip curtain drop. Set to 0 pre-first-commit
	//! so the blit can fall back to the global formula.
	uint32_t blit_slot_w;
	uint32_t blit_slot_h;
	uint32_t blit_tile_columns;
	uint32_t blit_tile_rows;

	//! Window-space layer snapshot for `multi_compositor_render`'s WS pass.
	//! Updated under `ws_snapshot_mutex` at the end of compositor_layer_commit
	//! (after all layers for this frame have been added and processed),
	//! read by multi-comp under the same mutex. Decouples multi-comp from
	//! the per-client layer accumulator's mid-frame transitions
	//! (`compositor_layer_begin` resets `layer_count=0`; a read between
	//! that reset and the add of the WS layer would see no HUD →
	//! per-frame HUD flicker).
	std::mutex ws_snapshot_mutex;
	struct comp_layer ws_snapshot[XRT_MAX_LAYERS];
	uint32_t ws_snapshot_count;

	//! Projection-layer composition flags from the most recent client commit
	//! (snapshot guarded by ws_snapshot_mutex, captured alongside ws_snapshot).
	//! Multi-comp reads this when picking the per-tile blend mode — reading
	//! cc->layer_accum directly races with the client's xrBeginFrame reset
	//! and per-call adds (same root cause as the HUD-snapshot pattern above).
	//! `projection_flags_valid == false` means no projection layer was seen
	//! this frame; we keep the last-known flags rather than resetting so
	//! the blend mode is stable across the begin→add gap.
	enum xrt_layer_composition_flags projection_flags_snapshot;
	bool projection_flags_valid;

	//! Runtime-side cache of the WS layer's source texture, populated by
	//! GPU `CopyResource` from the app's HUD swapchain whenever
	//! multi_compositor_render successfully `AcquireSync`s the cross-process
	//! keyed mutex. Multi-comp always blits from THIS cache (not the live
	//! HUD swapchain) — when an `AcquireSync` fails (cube is mid-write
	//! holding the mutex past our timeout), the previous tick's content
	//! is still here, so the HUD does not flicker out for one frame.
	//! One cache per WS layer index (matches `ws_snapshot[i]`); recreated
	//! lazily when the source dim changes.
	wil::com_ptr<ID3D11Texture2D> hud_cache_tex[XRT_MAX_LAYERS];
	wil::com_ptr<ID3D11ShaderResourceView> hud_cache_srv[XRT_MAX_LAYERS];
	uint32_t hud_cache_w[XRT_MAX_LAYERS];
	uint32_t hud_cache_h[XRT_MAX_LAYERS];
	bool hud_cache_valid[XRT_MAX_LAYERS]; // true once at least one acquire succeeded

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

	//! Per-client xrWaitFrame cap in Hz set by xrSetWorkspaceClientFrameRateCapEXT
	//! (spec_version 14). 0.0f = uncapped (native refresh). Resets to 0.0f on
	//! slot register/unregister; controllers re-apply on reconnect. Read
	//! lock-free from compositor_predict_frame — single float, torn read at
	//! worst applies last-frame's value for one call.
	float frame_rate_cap_hz;

	//! spec_version 16 (#304): one-shot, set at register (slot-bind). The
	//! drain emits one CLIENT_CONNECTED event per slot with this set, then
	//! clears it; the controller responds with per-client setup (place via
	//! xrSetWorkspaceClientWindowPoseEXT, chrome, style, focus). Race-free —
	//! the slot is bound before the event fires.
	bool announce_connected;

	//! spec_version 16 (#304): the controller has placed this client (called
	//! set_pose at least once). The runtime no longer picks an initial pose,
	//! so it must NOT composite the slot until the controller has placed it —
	//! multi_compositor_render gates drawing on placed && has_first_frame_
	//! committed. Flips true on the first set_client_window_pose; structural
	//! flash/race fix (controller latency can never show an unplaced client).
	bool placed;

	//! True after the IPC client has committed at least one projection layer
	//! since slot registration. Until then, multi_compositor_render skips
	//! drawing this slot entirely — the per-client atlas is uninitialized and
	//! `content_view_w/_h` are zero, so any draw shows undefined-memory black
	//! at fallback dims (mirrors the capture-client pattern of gating on
	//! `capture_srv` non-null at the same render-loop site). The entry grow-in
	//! animation is controller-owned now (#306); the runtime only gates drawing.
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

	//! Phase 2.C: controller-submitted chrome swapchain. The runtime composites
	//! the swapchain image at the controller-specified pose every render, with
	//! controller-defined hit regions and depth bias.
	//!
	//! NULL until xrCreateWorkspaceClientChromeSwapchainEXT is called for this
	//! client. C5: with the in-runtime chrome render block deleted, chrome is
	//! only ever visible when the controller has submitted its own.
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
	bool                  chrome_anchor_top_edge; //!< spec_version 8: pose_y is offset above window top
	float                 chrome_width_fraction;  //!< spec_version 8: 0 = absolute, > 0 = win_w * fraction
	//! Phase 2.C C5 follow-up: OpenXR client_id (the workspace-side ID
	//! returned by xrEnumerateWorkspaceClientsEXT) for the client that
	//! owns this slot's chrome. Set by the IPC register_chrome_swapchain
	//! handler when chrome is bound to a slot. Used by POINTER_HOVER
	//! emission so controllers can look up their per-client chrome by
	//! the same ID they used at create time. 0 = unset (no chrome
	//! registered). Distinct from the legacy `1000 + slot_index` form
	//! used by hit_client_id on POINTER / POINTER_MOTION events.
	uint32_t              workspace_client_id;

	//! spec_version 8: last-emitted pose+size snapshot for this slot. The
	//! drain compares the current window_pose / window_width_m / window_
	//! height_m against this snapshot and emits WINDOW_POSE_CHANGED on
	//! any difference, so controllers re-push chrome layout (and other
	//! window-tracking UI) when the runtime resizes the window via edge
	//! drag, fullscreen toggle, etc. Initialised to {identity, zero} so
	//! the first valid frame always emits one transition.
	struct xrt_pose       window_pose_last_emitted;
	float                 window_w_last_emitted;
	float                 window_h_last_emitted;

	//! spec_version 10: a Win32 modal popup is open in this client. Set by
	//! ipc_handle_session_set_modal_state when the in-app CBT hook
	//! (oxr_workspace_modal_win32) detects a dialog. Read by the drain loop
	//! to emit MODAL_OPEN / MODAL_CLOSE events to the workspace controller,
	//! and by the frame-starvation timeout logic to keep presenting the
	//! last-good frame while the user interacts with the dialog.
	bool                  modal_open;
	bool                  modal_open_last_emitted;

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

	//! Phase 2.C spec_version 9: per-client visual style pushed by the
	//! workspace controller via xrSetWorkspaceClientStyleEXT. Cached
	//! per-slot and applied at content blit time. Zero-init = runtime
	//! defaults (no rounding, no feather, no glow). The focus glow
	//! fields are only consulted when this slot equals mc->focused_slot.
	//! Distinct from the chrome layout (which positions the chrome quad);
	//! this struct shapes how the client's CONTENT itself composites.
	bool style_pushed;                                       //!< false until controller pushes a style; defaults active until then
	float style_corner_radius;                               //!< fraction of window height; 0 = sharp
	float style_edge_feather_meters;                         //!< soft alpha falloff width in meters
	float style_focus_glow_color[4];                         //!< RGBA, gated on focus
	float style_focus_glow_intensity;                        //!< 0 disables even when focused
	float style_focus_glow_falloff_meters;                   //!< halo extent in meters
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

	//! spec_version 8: last value of focused_slot we signaled the wakeup event
	//! for. The drain emits FOCUS_CHANGED based on focused_slot_last_emitted;
	//! this separate snapshot lives in render_pass so we can wake the
	//! controller on every focus transition without having to instrument
	//! every focused_slot write site individually.
	int32_t focused_slot_signaled_value;

	//! Phase 2.K: vsync-aligned frame counter. Incremented once per
	//! `multi_compositor_render` and read by the public-API drain to emit
	//! FRAME_TICK events (capped per-batch) so controllers can pace
	//! per-frame work without polling a timer.
	volatile LONG frame_tick_count;
	LONG frame_tick_last_emitted;
	uint64_t frame_tick_last_ns;
	//! spec_version 20: viewer eye-midpoint (display space, meters), cached once
	//! per displayed frame on the render thread and copied into the FRAME_TICK
	//! event so the workspace controller can billboard windows toward the live
	//! (head-tracked) viewer. Plain floats: written render-side, read drain-side;
	//! a torn read just yields a 1-frame-stale viewer, which is harmless.
	float frame_tick_viewer_x, frame_tick_viewer_y, frame_tick_viewer_z;
	volatile LONG frame_tick_viewer_valid;

	//! Window dismissed by user (ESC).
	bool window_dismissed;

	//! True after dismiss cleanup (EXIT_REQUEST sent, captures released).
	bool dismiss_cleanup_done;

	//! Workspace deactivated (Ctrl+Space): window hidden, DP released, captures stopped.
	//! Unlike window_dismissed, the multi-comp structure stays alive for re-activation.
	bool suspended;

	//! Cursor render inputs for the controller-pushed sprite (sys->cursor_xsc).
	//! cursor_panel_x/y is the OS cursor position sampled per frame in
	//! render_pass (runtime-owned). cursor_hit_z_m + cursor_over_window +
	//! cursor_dim_factor are pushed per frame by the workspace controller via
	//! xrSetWorkspaceCursorDepthEXT (spec_version 22; dim factor added in 23) —
	//! the controller owns the hit-test and the cursor look-and-feel. The cursor
	//! render pass uses cursor_hit_z_m for per-eye disparity, cursor_over_window
	//! for whether to dim, and cursor_dim_factor as the over-window body alpha.
	//! Shape / visibility are also controller-pushed (sys->cursor_xsc +
	//! cursor_visible).
	int32_t cursor_panel_x;
	int32_t cursor_panel_y;
	float   cursor_hit_z_m;
	bool    cursor_over_window;
	float   cursor_dim_factor; //!< (#376) over-window cursor body alpha; default 0.30.

	//! Previous frame LMB/RMB state (for rising-edge detection).
	bool prev_lmb_held;
	bool prev_rmb_held;

	//! Font atlas for title bar text (DirectWrite-rendered, anti-aliased).
	wil::com_ptr<ID3D11Texture2D> font_atlas;
	wil::com_ptr<ID3D11ShaderResourceView> font_atlas_srv;
	float glyph_advances[96];  //!< Per-glyph advance width in atlas pixels (proportional)
	uint32_t font_glyph_w;     //!< Max glyph cell width in atlas pixels
	uint32_t font_glyph_h;     //!< Glyph cell height in atlas pixels
	uint32_t font_atlas_w;     //!< Total atlas width in pixels
	uint32_t font_atlas_h;     //!< Total atlas height in pixels

	//! Embedded DisplayXR logo PNG decoded to an SRV on first use. Rendered in
	//! the empty state (no clients). Source bytes come from
	//! displayxr_white_png[] which is generated from assets/displayxr_white.png.
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

	//! Acked-flip + curtain state machine (#234). Replaces the historical
	//! "flip DP + sync_tile_layout immediately, hope apps catch up" pattern.
	//! See multi_compositor_request_mode_flip / multi_compositor_apply_pending_mode_flip.
	struct
	{
		enum mode_flip_phase phase;   //!< IDLE / WAITING_ACK / FLIPPING (see enum).
		uint32_t target_mode_index;   //!< Mode we're transitioning TO.
		uint32_t source_mode_index;   //!< Mode we were in when transition started.
		uint32_t ack_frame_counter;   //!< Frames spent in WAITING_ACK.
		uint32_t flip_frame_counter;  //!< Frames spent in FLIPPING.
		bool target_is_3d;            //!< Resolved at request time for vendor poll target.
		int origin_slot;              //!< Slot that initiated (-1 = system: focus-adaptive, vendor poll).
		uint32_t saved_mode_index;    //!< Pre-modal mode to restore on modal-close.
		bool curtain_active;          //!< Per-tile blit pass collapses to eye-0 / tile-(0,0) when true.
	} mode_flip;

	//! XR_EXT_workspace_file_dialog: pending Tier 1 picker requests. Bounded
	//! buffer keyed by request_id. Allocated in
	//! comp_d3d11_service_workspace_post_file_picker_request and consumed by
	//! comp_d3d11_service_workspace_file_picker_result. The drain loop emits
	//! IPC_WORKSPACE_INPUT_EVENT_FILE_PICKER_REQUEST for entries whose
	//! `needs_emit` flag is set. All access is guarded by `sys->render_mutex`.
	struct
	{
		bool                         in_use;       //!< Slot occupied.
		bool                         needs_emit;   //!< Drain loop should emit a request event.
		int                          owner_slot;   //!< Workspace slot that issued the request.
		uint64_t                     request_id;
		struct ipc_file_picker_info  info;
	} file_picker[8];
	uint64_t next_file_picker_request_id;
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

// Phase 2.C spec_version 8: signal the workspace-controller wakeup event.
// Cheap (single SetEvent on Win32; no-op when no controller has acquired the
// handle yet). Safe to call from any thread — Win32 events are themselves
// thread-safe. Centralized here so every call site that produces async state
// the controller might react to (input event push, hovered/focused-slot
// transitions, future client-connect/disconnect signals) calls one helper.
static inline void
service_signal_workspace_wakeup(struct d3d11_service_system *sys)
{
#ifdef _WIN32
	if (sys != nullptr && sys->workspace_wakeup_event != nullptr) {
		SetEvent((HANDLE)sys->workspace_wakeup_event);
	}
#else
	(void)sys;
#endif
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
 * Begin a workspace display-mode transition to @p target_mode_idx (#234).
 *
 * Single entry point for every IPC-mode mode flip — focus-adaptive, qwerty V,
 * app-initiated XR_EXT, vendor SDK auto, modal-open/close. Replaces the
 * previous "flip DP + sync_tile_layout + broadcast immediately, hope apps
 * catch up" pattern that exposed a 1–N frame raw-atlas artifact while clients
 * lagged the layout change.
 *
 * Caller is responsible for any pre-flip bookkeeping (e.g. saving
 * sys->last_3d_mode_index before transitioning to 2D). This helper:
 *   1. Broadcasts XrEventDataRenderingModeChangedEXT immediately so clients
 *      can begin re-submitting at the new layout.
 *   2. Marks the multi-comp as MFP_WAITING_ACK with the curtain ON.
 *   3. Does NOT touch active_rendering_mode_index / sync_tile_layout / DP.
 *      Those land in MFP_FLIPPING, triggered by
 *      multi_compositor_apply_pending_mode_flip once the per-slot ack quorum
 *      forms (or fairness timeout fires).
 *
 * No-op outside workspace/IPC mode (sys->multi_comp == nullptr).
 */
static void
multi_compositor_request_mode_flip(struct d3d11_service_system *sys,
                                   uint32_t target_mode_idx,
                                   int origin_slot)
{
	if (sys == nullptr || sys->multi_comp == nullptr) {
		return;
	}
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	struct xrt_device *head = (sys->xsysd != nullptr) ? sys->xsysd->static_roles.head : nullptr;
	if (head == nullptr || head->hmd == NULL) {
		return;
	}
	if (target_mode_idx >= head->rendering_mode_count) {
		return;
	}

	uint32_t source_mode_idx = head->hmd->active_rendering_mode_index;
	bool target_is_3d = head->rendering_modes[target_mode_idx].hardware_display_3d;

	// Toggle-back guard: user mashed V (or focus-bounced) and the target is
	// the same mode we were in before the still-pending flip. Abort cleanly.
	if (mc->mode_flip.phase != MFP_IDLE &&
	    target_mode_idx == mc->mode_flip.source_mode_index) {
		U_LOG_W("[mode_flip] toggle-back to source %u while pending — aborting",
		        target_mode_idx);
		mc->mode_flip.phase = MFP_IDLE;
		mc->mode_flip.curtain_active = false;
		return;
	}

	// No-op: already at target and nothing pending.
	if (mc->mode_flip.phase == MFP_IDLE && target_mode_idx == source_mode_idx) {
		U_LOG_W("[mode_flip] no-op: already at target %u (IDLE)", target_mode_idx);
		return;
	}

	mc->mode_flip.phase = MFP_WAITING_ACK;
	mc->mode_flip.target_mode_index = target_mode_idx;
	mc->mode_flip.source_mode_index = source_mode_idx;
	mc->mode_flip.ack_frame_counter = 0;
	mc->mode_flip.flip_frame_counter = 0;
	mc->mode_flip.target_is_3d = target_is_3d;
	mc->mode_flip.origin_slot = origin_slot;
	mc->mode_flip.curtain_active = true;

	// Reset per-slot ack flags. Auto-ack capture / 2D-only slots — their
	// per-slot atlas layout does not change across rendering modes, so they
	// would never produce a "new layout" frame for the quorum. Snapshot the
	// pre-flip per-view extent for IPC slots so the ack detector can detect
	// "app has re-rendered after consuming XrEventDataRenderingModeChangedEXT"
	// by extent change, not by absolute match against view_width_pixels
	// (workspace apps submit window-scaled extents, not canonical mode dims).
	for (int s = 0; s < D3D11_MULTI_MAX_CLIENTS; s++) {
		bool ipc_3d = mc->clients[s].active &&
		              mc->clients[s].client_type == CLIENT_TYPE_IPC;
		mc->clients[s].acked_target_mode = !ipc_3d;
		mc->clients[s].pre_flip_view_w = mc->clients[s].last_commit_view_w;
		mc->clients[s].pre_flip_view_h = mc->clients[s].last_commit_view_h;
	}

	// Broadcast event NOW so clients begin their re-submit cycle while the
	// curtain masks the geometry mismatch. Device active index intentionally
	// not yet written — apps consume the event payload, not the device state.
	broadcast_rendering_mode_change(sys, head, source_mode_idx, target_mode_idx);

	U_LOG_W("[mode_flip] request %u -> %u (target_is_3d=%d, origin_slot=%d) — broadcast, curtain ON",
	        source_mode_idx, target_mode_idx, (int)target_is_3d, origin_slot);
}

/*!
 * Per-frame tick for the workspace mode-flip state machine (#234).
 *
 * Called once near the top of multi_compositor_render, before the per-tile
 * blit pass reads sys->tile_columns. Owns the WAITING_ACK -> FLIPPING and
 * FLIPPING -> IDLE transitions; curtain_active stays true across both so the
 * blit pass collapses to a uniform single-eye view until the DP and the
 * hardware lens transition have both settled.
 */
static void
multi_compositor_apply_pending_mode_flip(struct d3d11_service_system *sys)
{
	if (sys == nullptr || sys->multi_comp == nullptr) {
		return;
	}
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (mc->mode_flip.phase == MFP_IDLE) {
		return;
	}
	struct xrt_device *head = (sys->xsysd != nullptr) ? sys->xsysd->static_roles.head : nullptr;
	if (head == nullptr || head->hmd == NULL) {
		return;
	}

	if (mc->mode_flip.phase == MFP_WAITING_ACK) {
		mc->mode_flip.ack_frame_counter++;
		// Teardown guard: if no active IPC slots remain (last app exited
		// mid-flip) or the DP is gone, abort the pending flip rather than
		// kicking a torn-down DP. Race vs slot-removal observed at session
		// end on close-button click.
		int active_ipc_count = 0;
		for (int s = 0; s < D3D11_MULTI_MAX_CLIENTS; s++) {
			if (mc->clients[s].active &&
			    mc->clients[s].client_type == CLIENT_TYPE_IPC) {
				active_ipc_count++;
			}
		}
		if (active_ipc_count == 0 || mc->display_processor == nullptr) {
			U_LOG_W("[mode_flip] aborting pending flip — teardown (active_ipc=%d, dp=%p)",
			        active_ipc_count, (void *)mc->display_processor);
			mc->mode_flip.phase = MFP_IDLE;
			mc->mode_flip.curtain_active = false;
			return;
		}
		bool quorum = true;
		for (int s = 0; s < D3D11_MULTI_MAX_CLIENTS; s++) {
			if (!mc->clients[s].active) continue;
			if (mc->clients[s].client_type != CLIENT_TYPE_IPC) continue;
			if (!mc->clients[s].acked_target_mode) {
				quorum = false;
				break;
			}
		}
		bool timeout = mc->mode_flip.ack_frame_counter >= MFP_ACK_TIMEOUT_FRAMES;
		if (!quorum && !timeout) {
			return; // keep waiting, curtain stays on
		}
		if (timeout && !quorum) {
			U_LOG_W("[mode_flip] WAITING_ACK timeout at %u frames — force-flipping (curtain still masks)",
			        mc->mode_flip.ack_frame_counter);
		}

		// Land the flip: device state, DP, tile layout all in lockstep.
		// Mirror the legacy xrRequestDisplayRenderingModeEXT path's
		// xrt_device_set_property() call so apps that poll the device
		// OUTPUT_MODE property (instead of consuming the event) also see
		// the change. Apps that DO consume the event still get it via the
		// broadcast in request_mode_flip.
		head->hmd->active_rendering_mode_index = mc->mode_flip.target_mode_index;
		xrt_device_set_property(head, XRT_DEVICE_PROPERTY_OUTPUT_MODE,
		                        (int32_t)mc->mode_flip.target_mode_index);
		xrt_display_processor_d3d11_request_display_mode(
		    mc->display_processor, mc->mode_flip.target_is_3d);
		sync_tile_layout(sys);
		sys->hardware_display_3d = mc->mode_flip.target_is_3d;

		mc->mode_flip.phase = MFP_FLIPPING;
		mc->mode_flip.flip_frame_counter = 0;
		U_LOG_W("[mode_flip] FLIPPING: DP request_display_mode(%d), sync_tile_layout done",
		        (int)mc->mode_flip.target_is_3d);
		return;
	}

	if (mc->mode_flip.phase == MFP_FLIPPING) {
		mc->mode_flip.flip_frame_counter++;
		// Teardown guard: DP went away mid-FLIPPING. Bail without polling.
		if (mc->display_processor == nullptr) {
			U_LOG_W("[mode_flip] aborting FLIPPING — display_processor gone");
			mc->mode_flip.curtain_active = false;
			mc->mode_flip.phase = MFP_IDLE;
			return;
		}
		bool hw_settled = false;
		bool current_is_3d = false;
		if (xrt_display_processor_d3d11_get_hardware_3d_state(
		        mc->display_processor, &current_is_3d)) {
			hw_settled = (current_is_3d == mc->mode_flip.target_is_3d);
		}
		bool ceiling = mc->mode_flip.flip_frame_counter >= MFP_HW_CEILING_FRAMES;
		if (hw_settled || ceiling) {
			if (ceiling && !hw_settled) {
				U_LOG_W("[mode_flip] FLIPPING ceiling at %u frames — lifting curtain unconditionally",
				        mc->mode_flip.flip_frame_counter);
			}
			// Pre-load the vendor-poll cache and stamp the cooldown timer
			// before going IDLE. The vendor SDK's `is_3d` reading at this
			// point is what we just commanded; writing it into the cache so
			// the next CAS-protected poll reads a consistent value (instead
			// of the stale pre-flip cache) avoids a single-frame mismatch
			// against `sys->hardware_display_3d`. The cooldown stamp gives
			// the vendor SDK's internal state time to fully settle so a
			// genuine vendor-initiated change (which would arrive after the
			// 2-second window) can still take effect.
			int64_t landed_ns = os_monotonic_get_ns();
			sys->cached_3d_state.store(mc->mode_flip.target_is_3d, std::memory_order_relaxed);
			sys->cached_3d_state_valid.store(true, std::memory_order_release);
			sys->last_3d_state_poll_ns.store(landed_ns, std::memory_order_release);
			sys->last_flip_landed_ns.store(landed_ns, std::memory_order_release);
			mc->mode_flip.curtain_active = false;
			mc->mode_flip.phase = MFP_IDLE;
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

	// #308: blur variant for the pushed-back empty-state splash (non-fatal).
	hr = compile_shader(blit_blur_ps_hlsl, "PSMain", "ps_5_0", &blob);
	if (SUCCEEDED(hr)) {
		hr = sys->device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(),
		                                     nullptr, sys->blit_blur_ps.put());
		blob->Release();
		if (FAILED(hr)) {
			U_LOG_W("Failed to create blit blur pixel shader: 0x%08lx (splash blur disabled)", hr);
		}
	} else {
		U_LOG_W("Failed to compile blit blur pixel shader (splash blur disabled)");
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
	// OpenXR semantics:
	//   BLEND_TEXTURE_SOURCE_ALPHA_BIT clear  -> layer is opaque, no blend.
	//   BLEND_TEXTURE_SOURCE_ALPHA_BIT set + UNPREMULTIPLIED_ALPHA_BIT clear -> premultiplied.
	//   BLEND_TEXTURE_SOURCE_ALPHA_BIT set + UNPREMULTIPLIED_ALPHA_BIT set   -> straight alpha.
	const enum xrt_layer_composition_flags f = data->flags;
	if ((f & XRT_LAYER_COMPOSITION_BLEND_TEXTURE_SOURCE_ALPHA_BIT) == 0) {
		sys->context->OMSetBlendState(sys->blend_opaque.get(), nullptr, 0xFFFFFFFF);
	} else if ((f & XRT_LAYER_COMPOSITION_UNPREMULTIPLIED_ALPHA_BIT) != 0) {
		sys->context->OMSetBlendState(sys->blend_alpha.get(), nullptr, 0xFFFFFFFF);
	} else {
		sys->context->OMSetBlendState(sys->blend_premul.get(), nullptr, 0xFFFFFFFF);
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

// Window-space layer (XR_EXT_window_space_layer) rendering is now inline
// in `multi_compositor_render` (search for "Window-space layer pass") —
// the HUD blits onto the combined atlas alongside chrome rather than into
// the per-client atlas, so all WS GPU work runs on the capture-render
// thread under `sys->render_mutex` and avoids the cross-thread D3D11
// immediate-context contention that destabilised rendering when WS draws
// were issued from `compositor_layer_commit`.

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

	// Release DComp resources before the swapchain (visual holds a swapchain reference;
	// target holds the visual). DWM tears down on-screen content when target releases.
	res->dcomp_visual.reset();
	res->dcomp_target.reset();
	res->dcomp_device.reset();

	// Release chroma-key pass resources.
	res->ck_intermediate_srv.reset();
	res->ck_intermediate.reset();
	res->ck_constants.reset();
	res->ck_sampler.reset();
	res->ck_ps.reset();
	res->ck_vs.reset();

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
                              bool transparent_hwnd,
                              uint32_t chroma_key_color,
                              struct xrt_system_devices *xsysd,
                              struct d3d11_client_render_resources *res)
{
	std::memset(res, 0, sizeof(*res));
	res->chroma_key_color = chroma_key_color;

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
		xrt_result_t wret = comp_d3d11_window_create(
		    sys->output_width, sys->output_height,
		    sys->base.info.display_screen_left,
		    sys->base.info.display_screen_top,
		    &res->window);
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

	// Create swap chain at actual window size (not defaults).
	//
	// Default: flip-model + ALPHA_MODE_IGNORE (#163) — opaque present, no DWM bleed-through.
	// Transparent opt-in: flip-model + ALPHA_MODE_PREMULTIPLIED via
	// CreateSwapChainForComposition, bound to the app's HWND through
	// DirectComposition. DWM blends per-pixel (no chroma key, no
	// disocclusion fringe, no LWA_COLORKEY required on the plugin side).
	// Only meaningful when the app owns the HWND and we're not in
	// workspace/shell mode (workspace path uses a service-owned compositor window).
	const bool use_transparent =
	    transparent_hwnd && external_hwnd != nullptr && !sys->workspace_mode;

	DXGI_SWAP_CHAIN_DESC1 sc_desc = {};
	sc_desc.Width = actual_width;
	sc_desc.Height = actual_height;
	sc_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sc_desc.SampleDesc.Count = 1;
	sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sc_desc.BufferCount = 2;
	sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	if (use_transparent) {
		sc_desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
	} else {
		// IGNORE so DWM doesn't composite the desktop through the bound HWND (#163).
		sc_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	}
	if (transparent_hwnd && !use_transparent) {
		U_LOG_W("Transparent HWND requested but ignored "
		        "(external_hwnd=%p, workspace_mode=%d)",
		        external_hwnd, (int)sys->workspace_mode);
	}

	if (use_transparent) {
		hr = sys->dxgi_factory->CreateSwapChainForComposition(
		    sys->device.get(),
		    &sc_desc,
		    nullptr,
		    res->swap_chain.put());
		U_LOG_W("Transparent HWND opt-in: DComp + flip-model swapchain "
		        "(FLIP_DISCARD + PREMULTIPLIED, bc=2)");
	} else {
		hr = sys->dxgi_factory->CreateSwapChainForHwnd(
		    sys->device.get(),
		    res->hwnd,
		    &sc_desc,
		    nullptr,
		    nullptr,
		    res->swap_chain.put());
	}

	if (FAILED(hr)) {
		U_LOG_E("Failed to create swap chain for client: 0x%08lx", hr);
		fini_client_render_resources(res);
		return XRT_ERROR_VULKAN;
	}

	// Transparent path: bind composition swapchain to HWND through DComp.
	if (use_transparent) {
		hr = DCompositionCreateDevice2(
		    /*renderingDevice*/ nullptr,
		    __uuidof(IDCompositionDevice),
		    reinterpret_cast<void **>(res->dcomp_device.put()));
		if (FAILED(hr) || res->dcomp_device.get() == nullptr) {
			U_LOG_E("DCompositionCreateDevice2 failed: 0x%08lx", hr);
			fini_client_render_resources(res);
			return XRT_ERROR_VULKAN;
		}

		hr = res->dcomp_device->CreateTargetForHwnd(
		    res->hwnd, /*topmost*/ TRUE, res->dcomp_target.put());
		if (FAILED(hr) || res->dcomp_target.get() == nullptr) {
			U_LOG_E("IDCompositionDevice::CreateTargetForHwnd failed: 0x%08lx", hr);
			fini_client_render_resources(res);
			return XRT_ERROR_VULKAN;
		}

		hr = res->dcomp_device->CreateVisual(res->dcomp_visual.put());
		if (FAILED(hr) || res->dcomp_visual.get() == nullptr) {
			U_LOG_E("IDCompositionDevice::CreateVisual failed: 0x%08lx", hr);
			fini_client_render_resources(res);
			return XRT_ERROR_VULKAN;
		}

		hr = res->dcomp_visual->SetContent(res->swap_chain.get());
		if (SUCCEEDED(hr)) {
			hr = res->dcomp_target->SetRoot(res->dcomp_visual.get());
		}
		if (SUCCEEDED(hr)) {
			hr = res->dcomp_device->Commit();
		}
		if (FAILED(hr)) {
			U_LOG_E("DComp visual setup failed: 0x%08lx", hr);
			fini_client_render_resources(res);
			return XRT_ERROR_VULKAN;
		}
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
	void *dp_fac = comp_dp_factory_for_window(&sys->base.info, COMP_DP_PRIMARY_MONITOR, COMP_DP_API_D3D11);
	if (dp_fac != NULL && !sys->workspace_mode) {
		auto factory = (xrt_dp_factory_d3d11_fn_t)dp_fac;
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

	U_LOG_D("[#151] d3d11_service swapchain_acquire_image: index=%u image_count=%u "
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

// Per-client frame-rate cap (spec_version 14, xrSetWorkspaceClientFrameRateCapEXT).
//
// Reads the controller-supplied cap for this compositor's slot and converts it
// to a period multiplier. Pure mechanism — the runtime makes no decision about
// which clients get throttled; the workspace controller sets the cap per
// xrSetWorkspaceClientFrameRateCapEXT and the runtime applies it.
//
// Standalone (non-workspace) clients always return 1. Unprotected read of
// mc->clients[*] is intentional: worst-case torn read applies the previous
// frame's cap for one wait_frame call. Taking sys->render_mutex on every
// client wait would contend with the render thread and defeat the savings.
//
// Kill switch: DISPLAYXR_WORKSPACE_FRAME_BUDGET=off disables all throttling.
static int
compositor_predict_frame_cap_multiplier(struct d3d11_service_compositor *c)
{
	static int budget_enabled = -1;
	if (budget_enabled < 0) {
		const char *e = getenv("DISPLAYXR_WORKSPACE_FRAME_BUDGET");
		budget_enabled = (e != nullptr && (e[0] == 'o' || e[0] == 'O') &&
		                  (e[1] == 'f' || e[1] == 'F'))
		                     ? 0
		                     : 1;
	}
	if (!budget_enabled) {
		return 1;
	}

	struct d3d11_service_system *sys = c->sys;
	if (!sys->workspace_mode || sys->multi_comp == nullptr) {
		return 1;
	}

	struct d3d11_multi_compositor *mc = sys->multi_comp;
	for (int s = 0; s < D3D11_MULTI_MAX_CLIENTS; s++) {
		if (mc->clients[s].active && mc->clients[s].compositor == c) {
			float cap_hz = mc->clients[s].frame_rate_cap_hz;
			if (cap_hz <= 0.0f) {
				return 1;
			}
			float refresh_hz = sys->refresh_rate;
			if (refresh_hz <= 0.0f || cap_hz >= refresh_hz) {
				return 1;
			}
			int m = (int)lrintf(refresh_hz / cap_hz);
			return m < 1 ? 1 : m;
		}
	}
	return 1;
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

	// IPC clients drive xrWaitFrame entirely via predict_frame's out_wake_time_ns
	// (see ipc_client_compositor.c:ipc_compositor_wait_frame — client sleeps
	// client-side via u_wait_until, server isn't parked). Apply the per-client
	// cap here so the client waits client-side. In-process callers go through
	// compositor_wait_frame, which is intentionally NOT throttled.
	int multiplier = compositor_predict_frame_cap_multiplier(c);
	int64_t adjusted_period_ns = period_ns * multiplier;

	*out_predicted_display_time_ns = now_ns + adjusted_period_ns * 2;
	*out_predicted_display_period_ns = adjusted_period_ns;
	*out_wake_time_ns = now_ns + period_ns * (multiplier - 1);
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
compositor_layer_window_space(struct xrt_compositor *xc,
                               struct xrt_device *xdev,
                               struct xrt_swapchain *xsc,
                               const struct xrt_layer_data *data)
{
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);

	std::lock_guard<std::mutex> lock(c->mutex);
	comp_layer_accum_window_space(&c->layer_accum, xsc, data);

	return XRT_SUCCESS;
}

static xrt_result_t
compositor_layer_passthrough(struct xrt_compositor *xc,
                              struct xrt_device *xdev,
                              const struct xrt_layer_data *data)
{
	return XRT_SUCCESS;
}

/*
 *
 * Chroma-key post-weave alpha-conversion pass (D3D11 service)
 *
 * Same as the D3D11 in-process pass: rewrite back-buffer alpha based on chroma-key
 * RGB so DComp's per-pixel alpha presentation can punch transparent regions through
 * to the desktop. Inserted between HUD overlay and Present.
 *
 */

static const char *kChromaKeySvcVS = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint vid : SV_VertexID) {
    VSOut o;
    o.uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
)";

static const char *kChromaKeySvcPS = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
Texture2D<float4> src : register(t0);
SamplerState samp : register(s0);
cbuffer Constants : register(b0) { float3 chroma_rgb; float pad; };
float4 main(VSOut i) : SV_Target {
    float3 c = src.Sample(samp, i.uv).rgb;
    float3 d = abs(c - chroma_rgb);
    bool match = max(max(d.r, d.g), d.b) < (1.0/512.0);
    // Swapchain is DXGI_ALPHA_MODE_PREMULTIPLIED — RGB must already be * alpha.
    // For transparent (alpha=0) pixels RGB MUST be (0,0,0); otherwise DWM's
    // src.rgb + (1-alpha)*dst.rgb blend adds the matched chroma color to the
    // desktop and saturates to white instead of showing through.
    float a = match ? 0.0 : 1.0;
    return float4(c * a, a);
}
)";

struct ChromaKeySvcConstants {
	float chroma_rgb[3];
	float pad;
};

static bool
svc_chroma_key_init_pipeline(struct d3d11_service_system *sys,
                              struct d3d11_client_render_resources *res)
{
	if (res->ck_vs.get() != nullptr) return true;
	HRESULT hr;
	wil::com_ptr<ID3DBlob> vs_blob, ps_blob, err_blob;
	hr = D3DCompile(kChromaKeySvcVS, strlen(kChromaKeySvcVS), nullptr, nullptr, nullptr,
	                "main", "vs_5_0", 0, 0, vs_blob.put(), err_blob.put());
	if (FAILED(hr)) {
		U_LOG_E("Chroma-key VS compile failed: 0x%08lx %s", hr,
		        err_blob ? (const char *)err_blob->GetBufferPointer() : "");
		return false;
	}
	err_blob.reset();
	hr = D3DCompile(kChromaKeySvcPS, strlen(kChromaKeySvcPS), nullptr, nullptr, nullptr,
	                "main", "ps_5_0", 0, 0, ps_blob.put(), err_blob.put());
	if (FAILED(hr)) {
		U_LOG_E("Chroma-key PS compile failed: 0x%08lx %s", hr,
		        err_blob ? (const char *)err_blob->GetBufferPointer() : "");
		return false;
	}

	hr = sys->device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
	                                      nullptr, res->ck_vs.put());
	if (FAILED(hr)) { U_LOG_E("CreateVertexShader: 0x%08lx", hr); return false; }
	hr = sys->device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(),
	                                     nullptr, res->ck_ps.put());
	if (FAILED(hr)) { U_LOG_E("CreatePixelShader: 0x%08lx", hr); return false; }

	D3D11_BUFFER_DESC cb_desc = {};
	cb_desc.ByteWidth = sizeof(ChromaKeySvcConstants);
	cb_desc.Usage = D3D11_USAGE_DYNAMIC;
	cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	hr = sys->device->CreateBuffer(&cb_desc, nullptr, res->ck_constants.put());
	if (FAILED(hr)) { U_LOG_E("CB create: 0x%08lx", hr); return false; }

	D3D11_SAMPLER_DESC samp_desc = {};
	samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samp_desc.MaxLOD = D3D11_FLOAT32_MAX;
	hr = sys->device->CreateSamplerState(&samp_desc, res->ck_sampler.put());
	if (FAILED(hr)) { U_LOG_E("Sampler create: 0x%08lx", hr); return false; }

	U_LOG_W("Post-weave chroma-key conversion enabled: 0x%08X", res->chroma_key_color);
	return true;
}

static bool
svc_chroma_key_ensure_intermediate(struct d3d11_service_system *sys,
                                    struct d3d11_client_render_resources *res,
                                    UINT width, UINT height)
{
	if (res->ck_intermediate.get() != nullptr &&
	    res->ck_intermediate_w == width && res->ck_intermediate_h == height) {
		return true;
	}
	res->ck_intermediate_srv.reset();
	res->ck_intermediate.reset();

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	HRESULT hr = sys->device->CreateTexture2D(&desc, nullptr, res->ck_intermediate.put());
	if (FAILED(hr)) { U_LOG_E("Chroma-key intermediate create: 0x%08lx", hr); return false; }
	hr = sys->device->CreateShaderResourceView(res->ck_intermediate.get(), nullptr,
	                                            res->ck_intermediate_srv.put());
	if (FAILED(hr)) { U_LOG_E("Chroma-key intermediate SRV: 0x%08lx", hr); return false; }
	res->ck_intermediate_w = width;
	res->ck_intermediate_h = height;
	return true;
}

static void
svc_chroma_key_pass_execute(struct d3d11_service_system *sys,
                             struct d3d11_client_render_resources *res)
{
	if (res->chroma_key_color == 0) return;
	if (!svc_chroma_key_init_pipeline(sys, res)) return;

	// Get back-buffer dims via the RTV's underlying resource.
	if (!res->back_buffer_rtv) return;
	wil::com_ptr<ID3D11Resource> bb_resource;
	res->back_buffer_rtv->GetResource(bb_resource.put());
	wil::com_ptr<ID3D11Texture2D> bb_texture;
	if (FAILED(bb_resource->QueryInterface(IID_PPV_ARGS(bb_texture.put())))) return;
	D3D11_TEXTURE2D_DESC bb_desc = {};
	bb_texture->GetDesc(&bb_desc);
	if (!svc_chroma_key_ensure_intermediate(sys, res, bb_desc.Width, bb_desc.Height)) return;

	// Copy back buffer to intermediate (source for the shader sample).
	sys->context->CopyResource(res->ck_intermediate.get(), bb_texture.get());

	// Update constant buffer.
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	HRESULT hr = sys->context->Map(res->ck_constants.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (FAILED(hr)) return;
	uint32_t k = res->chroma_key_color;
	ChromaKeySvcConstants *cb = reinterpret_cast<ChromaKeySvcConstants *>(mapped.pData);
	cb->chroma_rgb[0] = ((k >>  0) & 0xFF) / 255.0f;
	cb->chroma_rgb[1] = ((k >>  8) & 0xFF) / 255.0f;
	cb->chroma_rgb[2] = ((k >> 16) & 0xFF) / 255.0f;
	cb->pad = 0.0f;
	sys->context->Unmap(res->ck_constants.get(), 0);

	// Bind back-buffer RTV + viewport.
	ID3D11RenderTargetView *rtvs[] = { res->back_buffer_rtv.get() };
	sys->context->OMSetRenderTargets(1, rtvs, nullptr);
	D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)bb_desc.Width, (float)bb_desc.Height, 0.0f, 1.0f };
	sys->context->RSSetViewports(1, &vp);

	// Set fullscreen-triangle pipeline state.
	sys->context->IASetInputLayout(nullptr);
	sys->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11Buffer *null_vb = nullptr;
	UINT zero = 0;
	sys->context->IASetVertexBuffers(0, 1, &null_vb, &zero, &zero);
	sys->context->VSSetShader(res->ck_vs.get(), nullptr, 0);
	sys->context->PSSetShader(res->ck_ps.get(), nullptr, 0);
	ID3D11ShaderResourceView *srvs[] = { res->ck_intermediate_srv.get() };
	sys->context->PSSetShaderResources(0, 1, srvs);
	ID3D11SamplerState *samps[] = { res->ck_sampler.get() };
	sys->context->PSSetSamplers(0, 1, samps);
	ID3D11Buffer *cbs[] = { res->ck_constants.get() };
	sys->context->PSSetConstantBuffers(0, 1, cbs);
	sys->context->Draw(3, 0);

	// Unbind so subsequent passes don't see this SRV.
	ID3D11ShaderResourceView *null_srv = nullptr;
	sys->context->PSSetShaderResources(0, 1, &null_srv);
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
	// Gate on modal_open (ADR-017, #232): while the focused client has a
	// Win32 modal popup, suppress click forwarding to its (hidden) app
	// HWND. Without this, the user clicking the dimmed app's content in
	// the workspace view forwards LMB-down to the app HWND, which shifts
	// the app's child-window focus and breaks the modal's modal-disable.
	// Target stays NULL → comp_d3d11_window's WndProc forwards nothing for
	// this frame, which is the right behavior while a modal is up.
	if (mc->focused_slot >= 0 && mc->focused_slot < D3D11_MULTI_MAX_CLIENTS &&
	    mc->clients[mc->focused_slot].active &&
	    !mc->clients[mc->focused_slot].modal_open) {
		target = mc->clients[mc->focused_slot].app_hwnd;
		rx = (int32_t)mc->clients[mc->focused_slot].window_rect_x;
		ry = (int32_t)mc->clients[mc->focused_slot].window_rect_y;
		rw = (int32_t)mc->clients[mc->focused_slot].window_rect_w;
		rh = (int32_t)mc->clients[mc->focused_slot].window_rect_h;
		is_capture = (mc->clients[mc->focused_slot].client_type == CLIENT_TYPE_CAPTURE);
	}

	// Forwarding-rect inset: HISTORICALLY 20 px to keep edge-of-tile
	// clicks off the app (treated as resize-handle area instead).
	// PROBLEM: the inset is subtracted from rel_x/y BEFORE scaling to
	// HWND coords downstream (comp_d3d11_window.cpp:~828):
	//   rel_x = workspace_x - rx_inset
	//   app_x = rel_x * HWND_w / tile_w_inset
	// That shift consistently pushes mapped HWND coords toward the
	// HWND centre by ~inset px — so app UI rendered near the top-
	// left or bottom-right of the tile becomes unreachable. Clicks
	// that visually land ON the UI map to HWND coords ~20 px
	// LEFT/UP of where the UI was rendered, missing the hit-test.
	// The gauss demo's "Open…" button (#228 Tier 1 integration
	// testing) was bitten by this on every tile size.
	//
		// FIX: edge-resize detection is owned by the workspace controller
		// (it runs its own hit-test and drives resize via window-pose RPCs),
		// so the inset here was vestigial. Setting it to 0 lets
	// every cursor position inside the tile forward at the correct
	// HWND coord. Resize-handle behaviour is unchanged.
	(void)is_capture; // (kept for the capture-path branch below)

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
			mc->clients[i].frame_rate_cap_hz = 0.0f;
			// Phase 2.C: drop any leftover chrome registration from a
			// prior tenant so the new client starts with no chrome.
			if (mc->clients[i].chrome_xsc != nullptr) {
				xrt_swapchain_reference(&mc->clients[i].chrome_xsc, NULL);
			}
			mc->clients[i].chrome_swapchain_id = 0;
			mc->clients[i].chrome_layout_valid = false;
			mc->clients[i].chrome_region_count = 0;

			// ADR-018 (#304): the runtime no longer picks an initial window
			// layout (compute_grid_layout is gone). Register at a sentinel
			// pose, mark the slot unplaced, and announce CLIENT_CONNECTED so
			// the controller owns placement (grid / cascade / PIP / restored
			// last-known) via xrSetWorkspaceClientWindowPoseEXT. The slot is
			// NOT composited until placed && first-frame-committed, so the
			// sentinel is never shown. The grow-in entry animation moves to
			// the controller (#306).
			float disp_w_np = sys->base.info.display_width_m;
			float disp_h_np = sys->base.info.display_height_m;
			if (disp_w_np <= 0.0f) disp_w_np = 0.700f;
			if (disp_h_np <= 0.0f) disp_h_np = 0.394f;
			mc->clients[i].window_pose.orientation = {0, 0, 0, 1};
			mc->clients[i].window_pose.position = {0, 0, 0};
			mc->clients[i].window_width_m = disp_w_np * 0.30f;
			mc->clients[i].window_height_m = disp_h_np * 0.30f;
			mc->clients[i].announce_connected = true;
			mc->clients[i].placed = false;
			// #304 id-unification: the slot's permanent workspace client id is
			// 1000 + slot, assigned here at register (NOT at chrome-create as
			// before). Every event + API uses this one id. Chrome create/
			// destroy no longer touches it; has-chrome gating uses chrome_xsc.
			mc->clients[i].workspace_client_id = 1000u + (uint32_t)i;

			// Compute pixel rect from the sentinel pose.
			slot_pose_to_pixel_rect(sys, &mc->clients[i],
			                        &mc->clients[i].window_rect_x,
			                        &mc->clients[i].window_rect_y,
			                        &mc->clients[i].window_rect_w,
			                        &mc->clients[i].window_rect_h);
			mc->clients[i].hwnd_resize_pending = true;

			mc->client_count++;
			// ADR-018: the runtime no longer auto-focuses the first
			// client. The workspace controller owns focus — it sees the
			// new client in xrEnumerateWorkspaceClientsEXT and calls
			// xrSetWorkspaceFocusedClientEXT (shell auto-focus-first path).
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
			// Release strong swapchain refs the WS-layer snapshot holds
			// (#234 follow-on). Once cleared, the IPC layer's swapchain
			// ref drop (ipc_server_per_client_thread.c:510) will be the
			// last ref, destroying the swapchain SAFELY because we run
			// inside sys->render_mutex (caller's contract) so no render
			// thread can be mid-snapshot-deref.
			{
				std::lock_guard<std::mutex> snap_lock(
				    mc->clients[i].ws_snapshot_mutex);
				for (uint32_t j = 0; j < mc->clients[i].ws_snapshot_count; j++) {
					for (size_t k = 0;
					     k < ARRAY_SIZE(mc->clients[i].ws_snapshot[j].sc_array);
					     k++) {
						xrt_swapchain_reference(
						    &mc->clients[i].ws_snapshot[j].sc_array[k], NULL);
					}
				}
				mc->clients[i].ws_snapshot_count = 0;
				mc->clients[i].projection_flags_valid = false;
			}

			mc->clients[i].active = false;
			mc->clients[i].compositor = nullptr;
			mc->client_count--;
			if (mc->focused_slot == i) {
				// ADR-018: the disconnecting client held focus. Clear it so
				// keyboard forwarding never targets a freed slot, but DON'T
				// pick the successor — that's the controller's call. The
				// shell's client-list diff sees the disconnect and auto-
				// focuses a survivor on its next tick.
				mc->focused_slot = -1;
				multi_compositor_update_input_forward(mc);
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
 * Phase 3 — emit `[RENDER]` once per 10s window from the capture thread (the
 * only single-reader site so no exchange race). Resets all counters at emit.
 */
static void
emit_render_diag_if_window_elapsed(struct d3d11_service_system *sys)
{
	int64_t now_ns = (int64_t)os_monotonic_get_ns();
	int64_t window_start = sys->render_diag_window_start_ns.load(std::memory_order_relaxed);
	if (window_start == 0) {
		sys->render_diag_window_start_ns.store(now_ns, std::memory_order_relaxed);
		return;
	}
	int64_t window_ns = now_ns - window_start;
	if (window_ns < 10LL * U_TIME_1S_IN_NS) {
		return;
	}

	uint32_t cap_r =
	    sys->render_diag_capture_renders.exchange(0, std::memory_order_relaxed);
	int64_t cap_total =
	    sys->render_diag_capture_render_total_ns.exchange(0, std::memory_order_relaxed);
	uint32_t cli_r =
	    sys->render_diag_client_renders.exchange(0, std::memory_order_relaxed);
	uint32_t cli_s =
	    sys->render_diag_client_skips.exchange(0, std::memory_order_relaxed);
	int64_t cli_total =
	    sys->render_diag_client_render_total_ns.exchange(0, std::memory_order_relaxed);
	int64_t wait_total =
	    sys->render_diag_mutex_wait_total_ns.exchange(0, std::memory_order_relaxed);
	uint32_t wait_count =
	    sys->render_diag_mutex_wait_count.exchange(0, std::memory_order_relaxed);

	uint32_t cap_avg_us = (cap_r > 0) ? (uint32_t)(cap_total / 1000 / cap_r) : 0u;
	uint32_t cli_avg_us = (cli_r > 0) ? (uint32_t)(cli_total / 1000 / cli_r) : 0u;
	uint32_t wait_avg_us = (wait_count > 0) ? (uint32_t)(wait_total / 1000 / wait_count) : 0u;

	U_LOG_W(
	    "[RENDER] capture_renders=%u capture_avg_us=%u client_renders=%u client_skips=%u client_avg_us=%u wait_avg_us=%u window_s=10",
	    cap_r, cap_avg_us, cli_r, cli_s, cli_avg_us, wait_avg_us);

	sys->render_diag_window_start_ns.store(now_ns, std::memory_order_relaxed);
}

/*!
 * Render timer thread for capture clients.
 *
 * When capture-only clients are active (no IPC clients driving layer_commit),
 * this thread ensures the multi-compositor renders at display refresh rate.
 */
static void
capture_render_thread_func(struct d3d11_service_system *sys)
try {
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

		// Phase 3 [RENDER] — measure render_mutex acquire latency and
		// multi_compositor_render duration. Both atomics are touched from
		// capture_render_thread_func and from compositor_layer_commit.
		int64_t t_before_lock = (int64_t)os_monotonic_get_ns();
		{
			std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);
			int64_t t_after_lock = (int64_t)os_monotonic_get_ns();
			sys->render_diag_mutex_wait_total_ns.fetch_add(
			    t_after_lock - t_before_lock, std::memory_order_relaxed);
			sys->render_diag_mutex_wait_count.fetch_add(1, std::memory_order_relaxed);

			if (sys->multi_comp) {
				// Always render: IPC clients drive via layer_commit too, but on
				// slow GPUs (e.g. Intel iGPU) they run at <10fps. The 14ms
				// throttle in layer_commit prevents double-renders, so this is safe.
				int64_t t_before_render = (int64_t)os_monotonic_get_ns();
				multi_compositor_render(sys);
				int64_t t_after_render = (int64_t)os_monotonic_get_ns();
				sys->render_diag_capture_renders.fetch_add(
				    1, std::memory_order_relaxed);
				sys->render_diag_capture_render_total_ns.fetch_add(
				    t_after_render - t_before_render,
				    std::memory_order_relaxed);
				sys->last_workspace_render_ns = os_monotonic_get_ns();
			}
		}

		emit_render_diag_if_window_elapsed(sys);
	}
} catch (std::exception const &e) {
	// Defense-in-depth. The render thread runs in a std::thread; any
	// uncaught C++ exception escaping this function triggers
	// std::terminate() and silently kills the whole service (no log
	// shutdown line, no WER dump). Catching here downgrades a process
	// kill to a logged graceful thread stop — the service stays up,
	// render thread exits, and we get a diagnostic. Same pattern is
	// worth adding to any other long-running std::thread entry in the
	// service.
	U_LOG_E("capture_render_thread_func: uncaught std::exception: %s — render thread dying gracefully", e.what());
} catch (...) {
	U_LOG_E("capture_render_thread_func: uncaught non-std exception — render thread dying gracefully");
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
			mc->clients[i].hwnd_resize_pending = false;
			mc->clients[i].frame_rate_cap_hz = 0.0f;

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

			// ADR-018 (#304): no runtime grid placement. Register the capture
			// client at a sentinel pose (centered, intrinsic aspect-correct
			// size from the clamp above), mark it unplaced, and announce
			// CLIENT_CONNECTED; the controller places it. Not composited until
			// placed && first-frame-committed. No entry animation (→ #306).
			mc->clients[i].window_pose.orientation = {0, 0, 0, 1};
			mc->clients[i].window_pose.position = {0, 0, 0};
			mc->clients[i].window_width_m = width_m;
			mc->clients[i].window_height_m = height_m;
			mc->clients[i].announce_connected = true;
			// #304 id-unification: permanent workspace id = 1000 + slot. For
			// capture clients this matches the id add_capture already returns.
			mc->clients[i].workspace_client_id = 1000u + (uint32_t)i;
			// Capture clients are controller-chosen (added via add_capture) and
			// the controller already knows them; start them placed so the
			// dormant 2D-capture path keeps drawing at the sentinel rather than
			// vanishing behind the placed-gate. The placed-gate's flash-
			// prevention targets auto-connecting OpenXR clients. (When 2D
			// capture is re-enabled, the controller should pose captures on
			// CLIENT_CONNECTED like any other client — follow-up.)
			mc->clients[i].placed = true;

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
			// ADR-018: no runtime auto-focus on register (see the IPC-client
			// register path). The controller focuses via the workspace API.

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
		// ADR-018: clear focus for forwarding safety; controller picks the
		// successor (shell auto-focus-next path on its client-list diff).
		mc->focused_slot = -1;
		multi_compositor_update_input_forward(mc);
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
		sys->multi_comp->focused_slot_signaled_value = -1;
		// #376: default the over-window cursor dim to the previous hardcoded
		// value so the first frame (before the controller's first push) is
		// unchanged. The controller overrides this per frame via spec_version 23.
		sys->multi_comp->cursor_dim_factor = 0.30f;
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
	xrt_result_t wret = comp_d3d11_window_create(
	    win_w, win_h,
	    sys->base.info.display_screen_left,
	    sys->base.info.display_screen_top,
	    &mc->window);
	if (wret != XRT_SUCCESS || mc->window == nullptr) {
		U_LOG_E("Multi-comp: failed to create window");
		return XRT_ERROR_D3D11;
	}
	mc->hwnd = (HWND)comp_d3d11_window_get_hwnd(mc->window);
	sys->compositor_hwnd = mc->hwnd;
	// Seed the window's workspace-mode flag from current sys state (service_set_workspace_mode
	// no-ops while multi_comp is null, so earlier activation hasn't reached the window).
	comp_d3d11_window_set_workspace_mode_active(mc->window, sys->workspace_mode);
	// spec_version 8: if the controller already acquired the wakeup event
	// before the window existed (controller activation can race with the
	// first client connect that creates the multi-compositor window),
	// hand it down now so the public-ring push site has a handle to signal.
	if (sys->workspace_wakeup_event != nullptr) {
		comp_d3d11_window_set_workspace_wakeup_event(mc->window, sys->workspace_wakeup_event);
	}

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
	void *dp_fac = comp_dp_factory_for_window(&sys->base.info, COMP_DP_PRIMARY_MONITOR, COMP_DP_API_D3D11);
	if (dp_fac != NULL) {
		auto factory = (xrt_dp_factory_d3d11_fn_t)dp_fac;
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
				wret = comp_d3d11_window_create(
				    dp_px_w, dp_px_h,
				    sys->base.info.display_screen_left,
				    sys->base.info.display_screen_top,
				    &mc->window);
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

	U_LOG_W("Multi-comp: output ready (window=%p, swap_chain=%p)", mc->hwnd, mc->swap_chain.get());
	return XRT_SUCCESS;
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

// C5: WORKSPACE_CHROME_FADE_{IN,OUT}_NS, slot_chrome_fade_to, and
// slot_chrome_fade_tick deleted with the in-runtime chrome render block.
// Hover-fade now lives controller-side in shell_chrome_tick (same
// 150 ms in / 300 ms out timings, same ease-out cubic).

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// #307: the maximize/fullscreen state machine (toggle_fullscreen) and its
// ease helpers (ease_out_cubic / slot_animate_to / slot_animate_tick) moved to
// the workspace controller (ADR-018). The controller drives maximize via
// xrSetWorkspaceClientWindowPoseEXT (with its own ease) + xrSetWorkspaceClient-
// VisibilityEXT (hide the backdrop windows). The runtime keeps only the
// mechanism.

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

						void *dp_fac_dismiss = comp_dp_factory_for_window(
						    &sys->base.info, COMP_DP_PRIMARY_MONITOR, COMP_DP_API_D3D11);
						if (dp_fac_dismiss != NULL) {
							auto factory = (xrt_dp_factory_d3d11_fn_t)dp_fac_dismiss;
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

	// ADR-018: the runtime no longer consumes TAB to cycle focus. TAB is
	// already emitted on the workspace input-event queue (comp_d3d11_window
	// pushes WORKSPACE_PUBLIC_EVENT_KEY before any suppression), so the
	// controller drains it and applies its own focus-cycle policy. ALT+TAB
	// (system task switcher) and SHIFT+TAB (app HUD toggles) are unaffected.

	// #376: DELETE (close focused client) is no longer intercepted here.
	// Window-close is controller policy (ADR-018); DELETE flows to the
	// controller as a workspace KEY event (pushed by comp_d3d11_window before
	// any forwarding) and the controller drives the close via the public
	// xrRequestWorkspaceClientExitEXT path.

	// #307: F11 (maximize toggle) and ESC (restore) are no longer intercepted
	// here. Both flow to the controller as workspace KEY events (pushed by
	// comp_d3d11_window before any forwarding), which owns the maximize policy
	// (ADR-018) and drives it via set_pose / set_visibility. ESC for a
	// non-maximized, no-focus workspace is still swallowed in comp_d3d11_window.

	// Screenshot: triggered by F12 key (kept for interactive use).

	// #376: Ctrl+O (browse + launch an arbitrary exe) is no longer intercepted
	// here. The "browse + launch" affordance moved to the controller, which
	// owns its own launch path (DISPLAYXR_WORKSPACE_SESSION + XR_RUNTIME_JSON).

	// Phase 2.G: Ctrl+1..3 layout presets are owned by the workspace
	// controller now; the runtime no longer intercepts them. Keys flow
	// through xrEnumerateWorkspaceInputEventsEXT and the controller
	// pushes per-client poses via xrSetWorkspaceClientWindowPoseEXT.

	// Per-frame: sample the OS cursor position for the cursor render pass and
	// signal the controller's event-driven wakeup on focus / window-pose
	// transitions. spec_version 13: cursor SHAPE / SPRITE / VISIBILITY are the
	// workspace controller's job (xrSetWorkspaceCursorEXT). spec_version 22:
	// cursor DEPTH (hit_z) + over-window dimming are also controller-owned —
	// pushed via xrSetWorkspaceCursorDepthEXT — because they depend on the
	// hit-test the controller now owns. The runtime keeps only POSITION here
	// (it owns the HWND / OS cursor).
	if (mc->window != nullptr) {
		POINT cpt = {0, 0};
		GetCursorPos(&cpt);
		ScreenToClient(mc->hwnd, &cpt);

		// spec_version 8: wake the controller on focused_slot transitions so
		// its event-driven wait stays asleep unless there's something to drain.
		if (mc->focused_slot != mc->focused_slot_signaled_value) {
			mc->focused_slot_signaled_value = mc->focused_slot;
			service_signal_workspace_wakeup(sys);
		}
		// Same for window pose / size — wakes the controller on edge
		// resize, fullscreen toggle, layout-preset glide.
		for (int sw = 0; sw < D3D11_MULTI_MAX_CLIENTS; sw++) {
			struct d3d11_multi_client_slot *cs = &mc->clients[sw];
			if (!cs->active || cs->client_type == CLIENT_TYPE_CAPTURE) continue;
			// #304: has-chrome gate (was the workspace_client_id==0 proxy,
			// which is now always non-zero). Only chromed slots need the
			// pose-change wakeup so the controller can re-anchor its pill.
			if (cs->chrome_xsc == nullptr) continue;
			const float kEps = 1e-5f;
			if (fabsf(cs->window_width_m  - cs->window_w_last_emitted) > kEps ||
			    fabsf(cs->window_height_m - cs->window_h_last_emitted) > kEps ||
			    fabsf(cs->window_pose.position.x - cs->window_pose_last_emitted.position.x) > kEps ||
			    fabsf(cs->window_pose.position.y - cs->window_pose_last_emitted.position.y) > kEps ||
			    fabsf(cs->window_pose.position.z - cs->window_pose_last_emitted.position.z) > kEps) {
				service_signal_workspace_wakeup(sys);
				break;
			}
		}

		// Publish OS cursor screen position for the cursor render pass. Depth
		// (cursor_hit_z_m) + over-window flag come from the controller via
		// comp_d3d11_service_workspace_set_cursor_depth(). Wake the controller
		// on cursor movement so its event-driven wait drains the FRAME_TICK
		// (which carries this position) promptly and re-runs its hit-test —
		// this replaces the hover-transition wakeup the raycast used to emit.
		if (mc->cursor_panel_x != cpt.x || mc->cursor_panel_y != cpt.y) {
			service_signal_workspace_wakeup(sys);
		}
		mc->cursor_panel_x = cpt.x;
		mc->cursor_panel_y = cpt.y;
	}

	// The render-thread LMB handler was removed in spec_version 22 along with
	// the raycast. Click policy (focus, close, drag, resize, chrome buttons)
	// is the workspace controller's job (ADR-018): it reacts to the POINTER
	// events it drains. Content-click forwarding to app HWNDs is driven by the
	// focused-slot input-forward rect (multi_compositor_update_input_forward +
	// WndProc); the old synthetic-DOWN "drag-in-one-click" shim only mattered
	// for handle apps rendering in their own window, which do not appear here.

	// Right-click: title bar RMB drag = rotation, content RMB = focus + forward to app.
	// Call GetAsyncKeyState ONCE to avoid consuming the & 1 press bit (Phase 2 lesson #4).
	{
		SHORT rmb_state = GetAsyncKeyState(VK_RBUTTON);
		bool rmb_held = (rmb_state & 0x8000) != 0;
		bool rmb_just_pressed = rmb_held && !mc->prev_rmb_held;
		mc->prev_rmb_held = rmb_held;

		// RMB rotation drag and RMB-content focus are the workspace
		// controller's job (ADR-018 / PR 4 of the runtime→controller
		// migration). Controllers see POINTER(R,*) events with hit_region
		// + chrome_region_id and decide what to do: rotation drag on
		// grip-hit (capture + per-frame xrSetWorkspaceClientWindowPoseEXT
		// with quaternion from yaw/pitch), focus via
		// xrSetWorkspaceFocusedClientEXT on RMB-on-tile, or context menu.
		(void)rmb_just_pressed;
	}

	// #305: scroll-to-resize, Shift+Scroll Z-depth, and [ / ] Z-step are now
	// owned by the workspace controller (ADR-018 — controller owns interactive
	// policy). The runtime emits SCROLL_EXT / KEY_EXT on the public event
	// surface; the controller drives size/Z via xrSetWorkspaceClientWindowPoseEXT.

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

	// Get eye positions from DP — now a cheap snapshot read (the Leia DP
	// subscribes to SR's EyePairStream listener and maintains the cache
	// internally, see leia_sr_d3d11.cpp). No SDK call from here.
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

	// #307: the per-slot animation tick is gone. The only remaining animation
	// was the maximize/restore ease, which moved to the controller (the entry
	// grow-in moved in #306, layout presets in Phase 2.G). The controller now
	// drives all window motion via per-frame xrSetWorkspaceClientWindowPoseEXT,
	// which already refreshes the input-forward rect (set_client_window_pose).

	// Per-frame tick for the workspace mode-flip state machine (#234). Owns
	// the WAITING_ACK -> FLIPPING and FLIPPING -> IDLE transitions and the
	// curtain on/off lifecycle. Must run BEFORE the per-tile blit pass reads
	// sys->tile_columns so the read sees the just-landed (or still-pending)
	// tile geometry consistently with the curtain state used by the blit.
	multi_compositor_apply_pending_mode_flip(sys);

	// 2D/3D display mode auto-switch based on focused client type.
	// When a capture (2D) window gets focus → switch to 2D mode.
	// When an IPC (3D) app gets focus → restore 3D mode.
	// Routes through multi_compositor_request_mode_flip (#234) so the DP /
	// sync_tile_layout / device-state writes are deferred to the apply step
	// above once all active IPC slots have re-submitted at the new layout
	// (or the fairness timeout fires). The curtain masks the catch-up window.
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
				uint32_t target_idx;
				if (want_2d) {
					uint32_t cur = head->hmd->active_rendering_mode_index;
					if (cur < head->rendering_mode_count &&
					    head->rendering_modes[cur].hardware_display_3d) {
						sys->last_3d_mode_index = cur;
					}
					target_idx = 0; // mode 0 = 2D mono
				} else {
					target_idx = sys->last_3d_mode_index;
				}
				multi_compositor_request_mode_flip(sys, target_idx, mc->focused_slot);
			}

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

	// Empty state: DisplayXR logo + a neutral hint when no clients are visible.
	// The runtime no longer owns the launcher (migrated to the workspace
	// controller in #308), so the hint doesn't reference any controller-specific
	// hotkey.
	if (mc->client_count == 0 && mc->font_atlas_srv) {
		// Lazy-load the embedded logo PNG on first entry.
		if (!mc->logo_load_tried) {
			mc->logo_load_tried = true;
			ID3D11ShaderResourceView *srv = nullptr;
			uint32_t lw = 0, lh = 0;
			if (d3d11_icon_load_from_memory(sys->device.get(), displayxr_white_png,
			                                 displayxr_white_png_size,
			                                 "assets/displayxr_white.png", &srv, &lw, &lh)) {
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
		const char *hint = "No windows open";

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

			// #308: when the launcher band is up (controller grabbed input), push
			// this empty-state splash BEHIND the display plane so it recedes like
			// the pushed-back windows rather than fighting the band at z = 0. Add
			// uncrossed per-eye disparity of ~5% of the splash width (±2.5%/eye).
			// Sign is the inverse of the cursor's front (crossed) convention:
			// behind → LEFT eye shifts left, RIGHT eye shifts right.
			float splash_disp = 0.0f;
			float blur_screen_px = 0.0f; // depth-of-field blur radius, in destination px
			if (sys->input_grabbed) {
				float disp_base = (logo_dst_w > 0.0f) ? logo_dst_w : (float)half_w * 0.5f;
				float half = 0.025f * disp_base; // 5% total separation
				int eye_idx = (int)(col % 2);
				splash_disp = (eye_idx == 0) ? -half : +half;
				// Screen-space blur radius applied uniformly to the logo AND the
				// hint text below it (matched depth-of-field). ~3% of the splash
				// width (the logo's 6% read as too strong).
				blur_screen_px = 0.03f * disp_base;
			}

			// --- Logo quad ---
			if (mc->logo_srv) {
				ID3D11ShaderResourceView *logo_srv = mc->logo_srv.get();
				sys->context->PSSetShaderResources(0, 1, &logo_srv);

				// #308: when pushed behind the band, render the logo through the
				// box-blur pixel shader (depth-of-field) at a UV radius of ~2% of
				// the image width. Single draw — the blur is computed in-shader, so
				// it doesn't depend on blend-state accumulation. Falls back to the
				// sharp blit_ps when not pushed back (or if the blur PS is missing).
				bool blurred = sys->input_grabbed && sys->blit_blur_ps;
				if (blurred) {
					sys->context->PSSetShader(sys->blit_blur_ps.get(), nullptr, 0);
				}
				D3D11_MAPPED_SUBRESOURCE m;
				if (SUCCEEDED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
				              D3D11_MAP_WRITE_DISCARD, 0, &m))) {
					BlitConstants *cb = static_cast<BlitConstants *>(m.pData);
					cb->src_rect[0] = 0; cb->src_rect[1] = 0;
					cb->src_rect[2] = (float)mc->logo_w; cb->src_rect[3] = (float)mc->logo_h;
					cb->src_size[0] = (float)mc->logo_w; cb->src_size[1] = (float)mc->logo_h;
					cb->dst_size[0] = (float)ca_w; cb->dst_size[1] = (float)ca_h;
					cb->convert_srgb = 0.0f;
					cb->chrome_alpha = 0.0f;
					cb->_pad_chrome[0] = 0.0f; // chrome_alpha.y: keep out of solid-color mode
					cb->corner_radius = 0; cb->corner_aspect = 0;
					cb->edge_feather = 0; cb->glow_intensity = 0;
					// Blur PS reads glow_falloff/glow_extent as the X/Y UV radius.
					// Convert the screen-space radius to this quad's UV scale so the
					// logo blurs by the same screen amount as the hint text below.
					cb->glow_falloff = blurred ? (blur_screen_px / logo_dst_w) : 0.0f;
					cb->glow_extent  = blurred ? (blur_screen_px / logo_dst_h) : 0.0f;
					cb->quad_mode = 0;
					cb->dst_offset[0] = cx - logo_dst_w * 0.5f + splash_disp;
					cb->dst_offset[1] = logo_y;
					cb->dst_rect_wh[0] = logo_dst_w;
					cb->dst_rect_wh[1] = logo_dst_h;
					memset(cb->quad_corners_01, 0, sizeof(cb->quad_corners_01));
					memset(cb->quad_corners_23, 0, sizeof(cb->quad_corners_23));
					sys->context->Unmap(sys->blit_constant_buffer.get(), 0);
					sys->context->Draw(4, 0);
				}
				if (blurred) {
					sys->context->PSSetShader(sys->blit_ps.get(), nullptr, 0); // restore for hint text
				}
			}

			// --- Hint text ---
			ID3D11ShaderResourceView *font_srv = mc->font_atlas_srv.get();
			sys->context->PSSetShaderResources(0, 1, &font_srv);
			// Match the logo's screen-space blur on the hint text (depth-of-field).
			// The per-glyph UV radius reduces to a constant because the glyph width
			// cancels (dst_gw = src_gw * scale): rx = blur_px / (atlas_w * scale).
			bool hint_blur = sys->input_grabbed && sys->blit_blur_ps;
			float hint_rx = hint_blur ? (blur_screen_px / ((float)mc->font_atlas_w * scale)) : 0.0f;
			float hint_ry = hint_blur ? (blur_screen_px / ((float)mc->font_atlas_h * scale)) : 0.0f;
			if (hint_blur) {
				sys->context->PSSetShader(sys->blit_blur_ps.get(), nullptr, 0);
			}
			// Measure text width
			float tw = 0;
			for (const char *p = hint; *p; p++) {
				unsigned char ch = (unsigned char)*p;
				if (ch < 0x20 || ch > 0x7E) ch = '?';
				tw += mc->glyph_advances[ch - 0x20] * scale;
			}
			float tx = cx - tw * 0.5f + splash_disp;
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
					cb->glow_falloff = hint_rx; // blur PS X/Y UV radius (0 when sharp)
					cb->glow_extent = hint_ry;
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
			if (hint_blur) {
				sys->context->PSSetShader(sys->blit_ps.get(), nullptr, 0); // restore sharp PS
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
		// ADR-018 (#304): never composite a client the controller has not
		// placed yet (placed flips on its first set_pose). Combined with the
		// register-time sentinel this guarantees no flash at an
		// unplaced/default pose regardless of controller latency.
		if (mc->clients[s].active && !mc->clients[s].minimized &&
		    mc->clients[s].placed) {
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
		// Per-client projection-layer composition flags, used to drive the
		// tile-blit blend mode. Sourced from the snapshot populated under
		// ws_snapshot_mutex in compositor_layer_commit (reading layer_accum
		// directly races with the client's xrBeginFrame reset, identical to
		// the WS-layer flicker that the snapshot pattern was introduced for).
		// `slot_flags_valid == false` means we never observed a projection
		// layer for this slot — fall through to opaque blend.
		enum xrt_layer_composition_flags slot_layer_flags = (enum xrt_layer_composition_flags)0;
		bool slot_flags_valid = false;

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

			// Per-slot stride (#234, Issue 3): use the slot's OWN
			// snapshot of atlas/tile_columns captured at commit time,
			// not the current global sys->tile_columns. The two diverge
			// during the curtain window of a workspace mode flip — the
			// slot's atlas was WRITTEN at the snapshotted stride, so
			// reading at the global stride would mash L/R together.
			// Fallback to the atlas-derived global formula for the very
			// first commit (snapshot is zero until then).
			slot_w_atlas = mc->clients[s].blit_slot_w;
			slot_h_atlas = mc->clients[s].blit_slot_h;
			if (slot_w_atlas == 0 || slot_h_atlas == 0) {
				uint32_t tc_fb = sys->tile_columns > 0 ? sys->tile_columns : 1;
				uint32_t tr_fb = sys->tile_rows > 0 ? sys->tile_rows : 1;
				slot_w_atlas = src_tex_w / tc_fb;
				slot_h_atlas = src_tex_h / tr_fb;
			}
			if (cvw > slot_w_atlas) cvw = slot_w_atlas;
			if (cvh > slot_h_atlas) cvh = slot_h_atlas;

			// Read projection-layer flags from the slot's snapshot —
			// snapshot is populated under ws_snapshot_mutex by
			// compositor_layer_commit when the client commits a frame.
			{
				struct d3d11_multi_client_slot *slot = &mc->clients[s];
				std::lock_guard<std::mutex> snap_lock(slot->ws_snapshot_mutex);
				if (slot->projection_flags_valid) {
					slot_layer_flags = slot->projection_flags_snapshot;
					slot_flags_valid = true;
				}
			}
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

		// Phase 2.K Commit 8.D: HUD compose setup. The XR_EXT_window_space_layer
		// HUD is composited INSIDE the chrome content-blit pixel shader (slot 1
		// SRV) so the rounded-window mask covers HUD pixels at corners — an
		// icon HUD in the top-left clips along the same arc as the cube
		// content. Hoisting the AcquireSync + cache refresh above the per-view
		// loop matches the chrome-blit pattern: one acquire per slot per
		// multi-comp tick, not per view.
		//
		// Only IPC clients with at least one XR_EXT_window_space_layer in their
		// `ws_snapshot` get a HUD; capture clients (2D windows) and slots in
		// the spinner gate render bare. Multi-HUD slots compose only the FIRST
		// WS layer for now (cube test app only submits one); a future change
		// can either composite multiple HUDs in the shader (texture array /
		// loop) or add a fallback post-chrome HUD pass for layers ≥ 1.
		struct comp_layer hud_layer_local;
		bool hud_active = false;
		const struct xrt_layer_window_space_data *hud_ws = nullptr;
		D3D11_TEXTURE2D_DESC hud_tex_desc = {};
		ID3D11ShaderResourceView *hud_srv_to_bind = nullptr;
		bool hud_layer_premultiplied = true;
		if (mc->clients[s].client_type != CLIENT_TYPE_CAPTURE && !show_spinner) {
			struct d3d11_multi_client_slot *slot_hud = &mc->clients[s];
			// Snapshot read — local copy so AcquireSync/CopyResource runs
			// without holding `ws_snapshot_mutex` (would otherwise serialize
			// with `compositor_layer_commit`'s snapshot write).
			memset(&hud_layer_local, 0, sizeof(hud_layer_local));
			{
				std::lock_guard<std::mutex> snap_lock(slot_hud->ws_snapshot_mutex);
				for (uint32_t i = 0; i < slot_hud->ws_snapshot_count; i++) {
					if (slot_hud->ws_snapshot[i].data.type == XRT_LAYER_WINDOW_SPACE) {
						hud_layer_local = slot_hud->ws_snapshot[i];
						hud_active = true;
						break;
					}
				}
			}
			if (hud_active) {
				hud_ws = &hud_layer_local.data.window_space;
				hud_layer_premultiplied =
				    (hud_layer_local.data.flags &
				     XRT_LAYER_COMPOSITION_BLEND_TEXTURE_SOURCE_ALPHA_BIT) == 0;
				struct xrt_swapchain *xsc = hud_layer_local.sc_array[0];
				struct d3d11_service_swapchain *sc =
				    (xsc != nullptr) ? d3d11_service_swapchain_from_xrt(xsc) : nullptr;
				uint32_t img_idx = (sc != nullptr) ? hud_ws->sub.image_index : 0;
				if (sc != nullptr && img_idx < sc->image_count) {
					sc->images[img_idx].texture->GetDesc(&hud_tex_desc);

					// Lazy-(re)create per-slot HUD cache if dim changed.
					// Same shape as the prior post-chrome HUD pass — see
					// the cache-rationale comment block on `hud_cache_tex`
					// in the slot struct: cache shields the per-frame
					// compose from missed AcquireSync (cube cpu-format path
					// holds the keyed mutex 5–10 ms; a missed acquire just
					// re-uses last tick's content).
					if (!slot_hud->hud_cache_tex[0] ||
					    slot_hud->hud_cache_w[0] != hud_tex_desc.Width ||
					    slot_hud->hud_cache_h[0] != hud_tex_desc.Height) {
						slot_hud->hud_cache_tex[0].reset();
						slot_hud->hud_cache_srv[0].reset();
						D3D11_TEXTURE2D_DESC cache_desc = {};
						cache_desc.Width = hud_tex_desc.Width;
						cache_desc.Height = hud_tex_desc.Height;
						cache_desc.MipLevels = 1;
						cache_desc.ArraySize = 1;
						cache_desc.Format = (hud_tex_desc.Format == DXGI_FORMAT_UNKNOWN)
						                        ? DXGI_FORMAT_R8G8B8A8_UNORM
						                        : hud_tex_desc.Format;
						cache_desc.SampleDesc.Count = 1;
						cache_desc.Usage = D3D11_USAGE_DEFAULT;
						cache_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
						HRESULT hr_c = sys->device->CreateTexture2D(
						    &cache_desc, nullptr, slot_hud->hud_cache_tex[0].put());
						if (SUCCEEDED(hr_c)) {
							sys->device->CreateShaderResourceView(
							    slot_hud->hud_cache_tex[0].get(), nullptr,
							    slot_hud->hud_cache_srv[0].put());
							slot_hud->hud_cache_w[0] = hud_tex_desc.Width;
							slot_hud->hud_cache_h[0] = hud_tex_desc.Height;
							slot_hud->hud_cache_valid[0] = false;
						}
					}

					// Non-blocking AcquireSync + CopyResource refresh.
					// `feedback_acquiresync_load_bearing`: AcquireSync(0)
					// is the cross-process cache barrier — without it the
					// reader sees stale (or zero-init) content even when
					// the writer has fenced. Non-blocking timeout matches
					// the projection blit; a miss leaves the cache content
					// from the previous successful acquire (no flicker).
					IDXGIKeyedMutex *hud_mutex = sc->images[img_idx].keyed_mutex.get();
					if (sc->service_created && hud_mutex != nullptr) {
						HRESULT hr = hud_mutex->AcquireSync(0, 0 /* non-blocking */);
						if (SUCCEEDED(hr)) {
							if (slot_hud->hud_cache_tex[0]) {
								sys->context->CopyResource(
								    slot_hud->hud_cache_tex[0].get(),
								    sc->images[img_idx].texture.get());
								slot_hud->hud_cache_valid[0] = true;
							}
							hud_mutex->ReleaseSync(0);
						}
					}
					if (slot_hud->hud_cache_valid[0] && slot_hud->hud_cache_srv[0]) {
						hud_srv_to_bind = slot_hud->hud_cache_srv[0].get();
					} else {
						// First-frame-before-acquire — skip compose this tick.
						hud_active = false;
					}
				} else {
					hud_active = false;
				}
			}
		}

		// Shader blit each view → combined atlas.
		uint32_t num_views = sys->tile_columns * sys->tile_rows;
		// Per-slot tile layout (#234, Issue 3): captured at commit time so
		// the source coord stays aligned with the slot's actual atlas
		// contents even when sys->tile_columns is ahead of an unacked slot.
		uint32_t slot_tc = mc->clients[s].blit_tile_columns;
		uint32_t slot_tr = mc->clients[s].blit_tile_rows;
		if (slot_tc == 0) slot_tc = sys->tile_columns > 0 ? sys->tile_columns : 1;
		if (slot_tr == 0) slot_tr = sys->tile_rows > 0 ? sys->tile_rows : 1;
		for (uint32_t v = 0; v < num_views && v < XRT_MAX_VIEWS; v++) {
			uint32_t src_col = v % sys->tile_columns;
			uint32_t src_row = v / sys->tile_columns;

			// Map destination tile → slot's source tile. If the slot
			// last wrote with FEWER tiles than the global expects (slot
			// is behind a mode flip), replicate view 0 into the extra
			// dest tiles — still valid content from the slot, just at a
			// smaller layout. Reverse case (slot has more views than
			// global) drops the extras naturally because num_views is
			// the global count.
			uint32_t slot_col = (src_col < slot_tc) ? src_col : 0;
			uint32_t slot_row = (src_row < slot_tr) ? src_row : 0;

			float src_px_x, src_px_y;
			if (slot_is_mono || mc->mode_flip.curtain_active) {
				// Curtain (#234): during a workspace mode flip, force every
				// destination tile to sample source tile (0,0). Combined
				// with the eye_idx=0 collapse below, this writes identical
				// content to both halves of combined_atlas so the SR weaver
				// sees a uniform mono frame regardless of which slots have
				// caught up to the new layout.
				src_px_x = 0.0f;
				src_px_y = 0.0f;
			} else {
				// Use slot's OWN stride snapshot — same value the slot's
				// commit-time clamp computed, guaranteeing write/read stay
				// coupled regardless of the global tile_columns state.
				src_px_x = static_cast<float>(slot_col * slot_w_atlas);
				src_px_y = static_cast<float>(slot_row * slot_h_atlas);
			}

			// Per-eye destination rect (parallax-shifted for Z != 0).
			// Curtain (#234): force eye_idx=0 for both halves during a mode
			// flip — without this, the per-eye parallax shift still produces
			// column-by-column doubling under the SR weaver even with
			// identical source content. Source AND destination must collapse.
			int eye_idx;
			if (mc->mode_flip.curtain_active) {
				eye_idx = 0;
			} else {
				eye_idx = (src_col < 2) ? (int)src_col : 0;
			}
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

			// Check for rotated window → perspective quad mode.
			// Curtain (#234): use eye 0 for both halves so the projected quad
			// is identical across the dual-write — same rationale as the
			// eye_idx collapse above.
			int ei_for_quad = mc->mode_flip.curtain_active
			                      ? 0
			                      : ((src_col < 2) ? (int)src_col : 0);
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
				// Phase 2.C spec_version 9: resolve per-client visual style.
				// Default values (when controller hasn't pushed a style)
				// preserve the prior look: 5 % rounded corners, 2 px feather.
				const struct d3d11_multi_client_slot &cs = mc->clients[s];
				const bool style_active = cs.style_pushed;
				const float win_h_m = cs.window_height_m > 0.0f ? cs.window_height_m : 0.001f;

				// Sign convention: corner_radius < 0 with corner_aspect < 0
				// rounds all four corners (see d3d11_service_shaders.h:716).
				const float style_corner_r =
				    style_active
				        ? (cs.style_corner_radius > 0.0f ? -cs.style_corner_radius : 0.0f)
				        : -0.05f;
				// edge_feather is sent as fraction of destination pixel height.
				// Convert meters → fraction-of-window-height (same physical
				// dimension): meters / window_height_m. Always-on (focused or
				// not) so all windows soften at the perimeter.
				//
				// Cap at the corner radius: the shader uses
				// `feather_band = edge_feather / ry` for the rounded-corner
				// alpha falloff, and when feather_band > 1 the corner_alpha
				// never reaches 1.0 (saturate clamps it down) — leaving the
				// corner interior tinted while the straight-edge interior
				// reaches full opacity. The visible discontinuity at the
				// corner→edge boundary appears as "broken / segmented" focus
				// glow on small windows where the requested 3 mm physical
				// feather exceeds the proportional corner radius. Capping
				// edge_feather to the corner radius keeps both regions
				// reaching opacity 1 at their interior. Larger windows (where
				// the requested feather is below the corner radius) are
				// unaffected.
				float style_feather_frac =
				    style_active
				        ? (cs.style_edge_feather_meters > 0.0f
				               ? cs.style_edge_feather_meters / win_h_m
				               : 0.0f)
				        : (UI_EDGE_FEATHER_PX / dest_px_h);
				if (style_corner_r != 0.0f) {
					float corner_r_frac = -style_corner_r;
					if (style_feather_frac > corner_r_frac) {
						style_feather_frac = corner_r_frac;
					}
				}

				// Phase 2.C spec_version 9: focus tint. When this slot is
				// the focused workspace client AND the controller's pushed
				// style enables a glow, write the focus color/intensity
				// into the content blit's cbuffer. The shader blends color
				// toward this tint inside the existing edge_feather band
				// (same falloff geometry; just ends in the controller's
				// color instead of transparent). Works on both axis-aligned
				// and perspective/tilted clients — no separate pre-pass.
				const bool focus_tint_enabled =
				    s == mc->focused_slot
				    && style_active
				    && cs.style_focus_glow_intensity > 0.0f;

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
				// Phase 2.C spec_version 9: corner radius + edge feather come
				// from the slot's per-client style (defaults preserve the
				// prior 5 % rounding + 2 px feather when no style is pushed).
				// Aspect sign convention: negative + negative aspect = all
				// four corners (see d3d11_service_shaders.h:716).
				cb->corner_radius = style_corner_r;
				cb->corner_aspect = -(cs.window_width_m / win_h_m);
				cb->edge_feather = style_feather_frac;
				if (focus_tint_enabled) {
					cb->glow_intensity = cs.style_focus_glow_intensity;
					cb->glow_color[0] = cs.style_focus_glow_color[0];
					cb->glow_color[1] = cs.style_focus_glow_color[1];
					cb->glow_color[2] = cs.style_focus_glow_color[2];
					cb->glow_color[3] = cs.style_focus_glow_color[3];
				} else {
					cb->glow_intensity = 0.0f;
				}
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

				// Phase 2.K Commit 8.D: HUD compose state. The shader's PS
				// samples t1 (hud_tex) and porter-duff'es it OVER the cube
				// content sample BEFORE corner_alpha/feather_alpha apply.
				// `hud_dst_rect` is in window-local 0..1 UV (matches the
				// shader's `quad_uv`); per-eye disparity is added to .x so
				// HUDs at forward depth get parallax.
				if (hud_active && hud_ws != nullptr) {
					float eye_shift_frac = 0.0f;
					if (sys->hardware_display_3d) {
						eye_shift_frac = (eye_idx == 0)
						                     ? -hud_ws->disparity / 2.0f
						                     : +hud_ws->disparity / 2.0f;
					}
					cb->hud_dst_rect[0] = hud_ws->x + eye_shift_frac;
					cb->hud_dst_rect[1] = hud_ws->y;
					cb->hud_dst_rect[2] = hud_ws->width;
					cb->hud_dst_rect[3] = hud_ws->height;
					float src_u0 = hud_ws->sub.norm_rect.x;
					float src_v0 = hud_ws->sub.norm_rect.y;
					float src_uw = hud_ws->sub.norm_rect.w;
					float src_vh = hud_ws->sub.norm_rect.h;
					if (hud_layer_local.data.flip_y) {
						src_v0 += src_vh;
						src_vh = -src_vh;
					}
					cb->hud_src_rect[0] = src_u0;
					cb->hud_src_rect[1] = src_v0;
					cb->hud_src_rect[2] = src_uw;
					cb->hud_src_rect[3] = src_vh;
					cb->hud_present = 1.0f;
					cb->hud_premul = hud_layer_premultiplied ? 1.0f : 0.0f;
				} else {
					cb->hud_present = 0.0f;
					cb->hud_premul = 0.0f;
					cb->hud_dst_rect[0] = 0.0f;
					cb->hud_dst_rect[1] = 0.0f;
					cb->hud_dst_rect[2] = 0.0f;
					cb->hud_dst_rect[3] = 0.0f;
					cb->hud_src_rect[0] = 0.0f;
					cb->hud_src_rect[1] = 0.0f;
					cb->hud_src_rect[2] = 0.0f;
					cb->hud_src_rect[3] = 0.0f;
				}
				sys->context->Unmap(sys->blit_constant_buffer.get(), 0);

				// Pipeline setup
				sys->context->VSSetShader(sys->blit_vs.get(), nullptr, 0);
				sys->context->PSSetShader(sys->blit_ps.get(), nullptr, 0);
				sys->context->VSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
				sys->context->PSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
				// Phase 2.K Commit 8.D: bind cube-content SRV at t0 and HUD
				// cache SRV at t1 in one call. When the slot has no active
				// HUD layer, t1 is bound to NULL — D3D11 returns float4(0)
				// on sample, which makes the shader's compose collapse to a
				// passthrough no-op (matches `cb->hud_present = 0`).
				ID3D11ShaderResourceView *content_srvs[2] = {slot_srv, hud_srv_to_bind};
				sys->context->PSSetShaderResources(0, 2, content_srvs);
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
				// Per-client tile blend respects the OpenXR
				// BLEND_TEXTURE_SOURCE_ALPHA_BIT / UNPREMULTIPLIED_ALPHA_BIT
				// flags from the client's projection layer. Workspace OUTPUT
				// stays opaque (the workspace's combined atlas to the DP is
				// always opaque); per-tile alpha only affects how each
				// client tile composes against the workspace background.
				if (slot_flags_valid) {
					struct xrt_layer_data fake_data = {};
					fake_data.flags = slot_layer_flags;
					set_blend_state(sys, &fake_data);
				} else {
					// Capture clients (2D window snapshots) and clients
					// with no projection layer this frame -> opaque blend.
					sys->context->OMSetBlendState(
					    sys->blend_opaque.get(), nullptr, 0xFFFFFFFF);
				}

				sys->context->Draw(4, 0);

				// C5: focus-rim inner glow deleted along with the in-runtime
				// chrome render block. Was rectangular (followed the content
				// quad's corners, not the rounded controller pill) — a
				// pre-existing polish item now resolved by removal. If a
				// rounded focus rim is desired, the controller can render
				// its own outer-glow quad as a separate chrome layer.
			}
		}

		// Phase 2.K Commit 8.D: clear t1 binding so subsequent draws (chrome
		// composite, taskbar, toast, launcher) don't sample stale HUD memory.
		// Their cb fills don't all set `hud_present`/`hud_dst_rect`; even
		// though garbage values usually compose to a no-op against an
		// unbound t1, an explicit unbind here is cheap defense.
		{
			ID3D11ShaderResourceView *null_srv1 = nullptr;
			sys->context->PSSetShaderResources(1, 1, &null_srv1);
		}

		// Phase 2.C: controller-submitted chrome composite. Reads the chrome
		// swapchain's image[0] SRV (single-image swapchain) and blits it at
		// the controller-specified pose in window-local meters, biased toward
		// the eye by chrome_depth_bias_m so it occludes the window's own
		// content while still depth-testing against other windows.
		//
		// C5: in-runtime chrome render block deleted — this is the ONLY
		// chrome path now. If no controller has submitted a chrome
		// swapchain for this slot (chrome_xsc == nullptr), the slot
		// renders bare with no chrome at all.
		if (mc->clients[s].chrome_xsc != nullptr && mc->clients[s].chrome_layout_valid &&
		    mc->clients[s].client_type != CLIENT_TYPE_CAPTURE) {
			struct d3d11_multi_client_slot *cs = &mc->clients[s];
			struct d3d11_service_swapchain *csc = d3d11_service_swapchain_from_xrt(cs->chrome_xsc);
			if (csc != nullptr && csc->image_count > 0 && csc->images[0].srv) {
				ID3D11ShaderResourceView *chrome_srv = csc->images[0].srv.get();

				// C3.B: bracket the chrome read with keyed-mutex acquire/release
				// so cross-process GPU writes from the shell become visible on
				// the runtime's D3D11 device. Service-created swapchains use a
				// shared NT handle + KEYEDMUTEX; the shell takes key=0 in
				// xrWaitSwapchainImage and releases it in xrReleaseSwapchainImage,
				// but the runtime's swapchain_wait_image is a no-op for
				// service_created swapchains (comment at line ~2419) — the
				// runtime is expected to acquire when it actually reads. Hoisted
				// above the per-view loop: one acquire/release per composite
				// tick, not per view.
				IDXGIKeyedMutex *chrome_mutex = csc->images[0].keyed_mutex.get();
				bool chrome_mutex_held = false;
				if (chrome_mutex != nullptr) {
					HRESULT hr = chrome_mutex->AcquireSync(0, 4 /* ms */);
					if (SUCCEEDED(hr)) {
						chrome_mutex_held = true;
					}
				}

				float chrome_cx = cs->chrome_pose_in_client.position.x;
				float chrome_cy = cs->chrome_pose_in_client.position.y;
				float chrome_size_w_eff = cs->chrome_size_w_m;
				// spec_version 8: width_fraction > 0 → auto-scale chrome
				// width to current window width every frame. Lets the
				// controller push layout once at create and have the
				// pill follow window resizes without re-pushing.
				if (cs->chrome_width_fraction > 0.0f) {
					chrome_size_w_eff = cs->window_width_m * cs->chrome_width_fraction;
				}
				// spec_version 8: anchor_top_edge → pose_y is offset
				// ABOVE the window's top edge (positive = above) using
				// CURRENT window height. Without this the pose_y is
				// stale (controller's last-seen win_h) and the chrome
				// lags one frame behind the window edge during resize.
				if (cs->chrome_anchor_top_edge) {
					chrome_cy = cs->window_height_m * 0.5f + cs->chrome_pose_in_client.position.y;
				}
				float chrome_hw = chrome_size_w_eff * 0.5f;
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
						// src_rect is in source-texture pixels (xy=offset,
						// zw=extent). Sample the entire chrome image.
						cb->src_rect[0] = 0.0f;
						cb->src_rect[1] = 0.0f;
						cb->src_rect[2] = (float)chrome_desc.Width;
						cb->src_rect[3] = (float)chrome_desc.Height;
						cb->src_size[0] = (float)chrome_desc.Width;
						cb->src_size[1] = (float)chrome_desc.Height;
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

				if (chrome_mutex_held) {
					chrome_mutex->ReleaseSync(0);
				}
			}
		}

		// (Focus glow is now drawn before content blit, inside the per-view loop above)

		// Window-space layer (XR_EXT_window_space_layer) HUDs are now
		// composited INSIDE the chrome content-blit pixel shader (slot 1
		// SRV), so the rounded-window mask covers HUD pixels at corners.
		// See "HUD compose setup" block above the per-view content-blit
		// loop in this function for the AcquireSync + cache-refresh, and
		// the `if (hud_active && hud_ws != nullptr)` block in the cb fill
		// for `hud_dst_rect` / `hud_src_rect` plumbing. The post-chrome
		// HUD pass that used to live here was removed in Commit 8.D —
		// drawing AFTER chrome put the HUD on top of the rounded /
		// feathered window border, defeating the corner clip.
	}

	// (Title bar + focus border rendering is inside the render_order loop for correct z-ordering)

	// Reset scissor to full atlas for non-tiled draws (DP processing)
	{
		uint32_t full_w = sys->base.info.display_pixel_width;
		uint32_t full_h = sys->base.info.display_pixel_height;
		if (full_w == 0) full_w = 3840;
		if (full_h == 0) full_h = 2160;
		D3D11_RECT full_scissor = {0, 0, (LONG)full_w, (LONG)full_h};
		sys->context->RSSetScissorRects(1, &full_scissor);
	}

	// #307 slice B: the taskbar moved to the workspace controller (ADR-018).
	// The controller renders the user-minimize taskbar onto a display-spanning
	// overlay swapchain (xrCreateWorkspaceOverlaySwapchainEXT +
	// xrSetWorkspaceOverlayEXT) — composited by the overlay render pass above at
	// z = 0. The runtime no longer draws a taskbar or tracks which windows are
	// minimized for UI purposes; `minimized` is now purely the composite gate,
	// set by the controller via xrSetWorkspaceClientVisibilityEXT.

	// #307: the "Press F11 or Esc to restore" toast was dropped. Maximize is
	// controller-owned now (ADR-018); restore is discoverable via the MAX chrome
	// button (a real toggle), ESC, F11, and double-click. A toast could be
	// re-added controller-side on the new overlay swapchain mechanism — deferred
	// to a follow-up.


	// spec_version 17/21: workspace overlays (controller-pushed display-spanning
	// UI, e.g. taskbar + launcher + toast). Docked at z = 0 (zero disparity) →
	// unlike the cursor below there is NO raycast, NO per-eye disparity: the same
	// sprite is drawn at the same docked position in every atlas tile. Rendered
	// AFTER windows + chrome and BEFORE the cursor, so the cursor stays on top.
	// Controller owns all content + layout (overlay_slot anchor/pivot/size) via
	// xrCreateWorkspaceOverlaySwapchainEXT + xrSetWorkspaceOverlayEXT. The map is
	// keyed by overlayId; std::map iterates ascending so lower ids composite
	// behind higher ids (the shell picks ids to control z-order).
	for (const auto &ov_kv : sys->overlays) {
		const overlay_slot &ov = ov_kv.second;
		if (ov.xsc == nullptr) {
			continue;
		}
		uint32_t ca_w = sys->base.info.display_pixel_width;
		uint32_t ca_h = sys->base.info.display_pixel_height;
		if (ca_w == 0) ca_w = 3840;
		if (ca_h == 0) ca_h = 2160;

		// Resolve controller swapchain → SRV + texture dims. Acquire the keyed
		// mutex once (KEYEDMUTEX flushes controller-side writes into our device).
		struct d3d11_service_swapchain *ov_sc = d3d11_service_swapchain_from_xrt(ov.xsc);
		ID3D11ShaderResourceView *overlay_srv = nullptr;
		uint32_t sprite_w_px = 0, sprite_h_px = 0;
		IDXGIKeyedMutex *overlay_mutex = nullptr;
		bool overlay_mutex_held = false;
		if (ov_sc != nullptr && ov_sc->image_count > 0 && ov_sc->images[0].srv) {
			overlay_srv = ov_sc->images[0].srv.get();
			D3D11_TEXTURE2D_DESC sdesc = {};
			ov_sc->images[0].texture->GetDesc(&sdesc);
			sprite_w_px = sdesc.Width;
			sprite_h_px = sdesc.Height;
			overlay_mutex = ov_sc->images[0].keyed_mutex.get();
			if (overlay_mutex != nullptr) {
				if (SUCCEEDED(overlay_mutex->AcquireSync(0, 4 /* ms */))) {
					overlay_mutex_held = true;
				}
			}
		}
		if (overlay_srv != nullptr && sprite_w_px > 0 && sprite_h_px > 0) {
			float disp_w_m = sys->base.info.display_width_m;
			if (disp_w_m <= 0.0f) disp_w_m = 0.700f;
			float disp_h_m = sys->base.info.display_height_m;
			if (disp_h_m <= 0.0f) disp_h_m = 0.392f;

			// Per-tile atlas region from the active rendering mode (matches the
			// cursor pass; handles SBS 2×1, half-active LeiaSR 3D, etc.).
			const uint32_t n_cols = sys->tile_columns > 0 ? sys->tile_columns : 1;
			const uint32_t n_rows = sys->tile_rows > 0 ? sys->tile_rows : 1;
			uint32_t tile_w_v = 0, tile_h_v = 0;
			resolve_active_view_dims(sys, ca_w, ca_h, &tile_w_v, &tile_h_v);
			const uint32_t tile_w = tile_w_v;
			const uint32_t tile_h = tile_h_v;

			// Physical meters → atlas pixels through the per-mode tile dims:
			// tile_w spans disp_w_m of display width, tile_h spans disp_h_m.
			float overlay_w_atlas = ov.size_w_m / disp_w_m * (float)tile_w;
			float overlay_h_atlas = ov.size_h_m / disp_h_m * (float)tile_h;

			// Dock point: normalized display anchor → in-tile atlas pixels, then
			// subtract the sprite pivot (in atlas pixels) so the pivot lands on
			// the anchor. z = 0 → identical in every tile (no disparity offset).
			float anchor_px_x = ov.anchor_x * (float)tile_w;
			float anchor_px_y = ov.anchor_y * (float)tile_h;
			float base_tile_x = anchor_px_x - ov.pivot_x * overlay_w_atlas;
			float base_tile_y = anchor_px_y - ov.pivot_y * overlay_h_atlas;

			// Common pipeline state (same blit shader as cursor/windows).
			// The controller renders the overlay with Direct2D, which outputs
			// PREMULTIPLIED alpha — so composite with the premultiplied blend
			// (ONE, INV_SRC_ALPHA), not the straight-alpha state the cursor uses.
			ID3D11RenderTargetView *ortvs[] = {mc->combined_atlas_rtv.get()};
			sys->context->OMSetRenderTargets(1, ortvs, nullptr);
			sys->context->OMSetBlendState(sys->blend_premul.get(), nullptr, 0xFFFFFFFF);
			sys->context->OMSetDepthStencilState(sys->depth_disabled.get(), 0);
			sys->context->RSSetState(sys->rasterizer_state.get());
			sys->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
			sys->context->IASetInputLayout(nullptr);
			D3D11_VIEWPORT ovp = {};
			ovp.Width = (float)ca_w;
			ovp.Height = (float)ca_h;
			ovp.MaxDepth = 1.0f;
			sys->context->RSSetViewports(1, &ovp);
			sys->context->VSSetShader(sys->blit_vs.get(), nullptr, 0);
			sys->context->PSSetShader(sys->blit_ps.get(), nullptr, 0);
			sys->context->VSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
			sys->context->PSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
			sys->context->PSSetShaderResources(0, 1, &overlay_srv);
			sys->context->PSSetSamplers(0, 1, sys->sampler_linear.addressof());

			for (uint32_t row = 0; row < n_rows; row++) {
				for (uint32_t col = 0; col < n_cols; col++) {
					float dest_x = (float)(col * tile_w) + base_tile_x;
					float dest_y = (float)(row * tile_h) + base_tile_y;

					D3D11_MAPPED_SUBRESOURCE m;
					if (FAILED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
					                              D3D11_MAP_WRITE_DISCARD, 0, &m))) {
						continue;
					}
					BlitConstants *cb = static_cast<BlitConstants *>(m.pData);
					memset(cb, 0, sizeof(*cb));
					// spec_version 19: side-by-side stereo overlay → sample the
					// matching half per eye (col%2: 0 = left, 1 = right), stretched
					// to the full overlay footprint. Mono = whole image both eyes.
					float src_w = ov.stereo_sbs
					                  ? (float)(sprite_w_px / 2u)
					                  : (float)sprite_w_px;
					int eye_idx = (int)(col % 2);
					cb->src_rect[0] = ov.stereo_sbs ? (float)eye_idx * src_w : 0.0f;
					cb->src_rect[1] = 0;
					cb->src_rect[2] = src_w;
					cb->src_rect[3] = (float)sprite_h_px;
					cb->src_size[0] = (float)sprite_w_px;
					cb->src_size[1] = (float)sprite_h_px;
					cb->dst_size[0] = (float)ca_w;
					cb->dst_size[1] = (float)ca_h;
					cb->dst_offset[0] = dest_x;
					cb->dst_offset[1] = dest_y;
					cb->dst_rect_wh[0] = overlay_w_atlas;
					cb->dst_rect_wh[1] = overlay_h_atlas;
					cb->convert_srgb = 0.0f;
					cb->chrome_alpha = 0.0f;
					cb->quad_mode = 0.0f;
					cb->glow_intensity = 0.0f;
					sys->context->Unmap(sys->blit_constant_buffer.get(), 0);

					// Tile-local scissor so a docked overlay can't bleed across tiles.
					D3D11_RECT oscissor;
					oscissor.left   = (LONG)(col * tile_w);
					oscissor.top    = (LONG)(row * tile_h);
					oscissor.right  = (LONG)((col + 1) * tile_w);
					oscissor.bottom = (LONG)((row + 1) * tile_h);
					sys->context->RSSetScissorRects(1, &oscissor);

					sys->context->Draw(4, 0);
				}
			}

			// Restore full-atlas scissor for downstream passes (cursor, DP, etc.).
			D3D11_RECT ofull = {0, 0, (LONG)ca_w, (LONG)ca_h};
			sys->context->RSSetScissorRects(1, &ofull);
		}
		if (overlay_mutex_held) {
			overlay_mutex->ReleaseSync(0);
		}
	}

	// Phase 2.J / 3D cursor: render the OS-style cursor sprite at the per-
	// frame raycast hit's z-depth so the cursor floats at the same depth as
	// whatever the user is pointing at. When hit_z = 0 (no slot hit) the
	// cursor falls back to the panel plane (zero disparity). Renders into
	// every tile of the atlas (tile_columns × tile_rows) so multi-view
	// layouts (SBS 2×1, quad 2×2, dense multiview, etc.) all work — the
	// DP weaves all tiles into the final display so the cursor reads as a
	// single 3D-positioned point regardless of how the panel multiplexes
	// views. Per-tile X disparity is keyed off col % 2 (eye index).
	// spec_version 13: cursor sprite source is controller-pushed (a chrome-
	// style swapchain on sys->cursor_xsc). Runtime keeps the per-tile per-
	// eye-disparity rendering with hit-Z math + over-window dimming;
	// controller owns the sprite content (shape, color, animation,
	// branding) via xrCreateWorkspaceCursorSwapchainEXT + xrSetWorkspaceCursorEXT.
	if (sys->cursor_visible && sys->cursor_xsc != nullptr) {
		uint32_t ca_w = sys->base.info.display_pixel_width;
		uint32_t ca_h = sys->base.info.display_pixel_height;
		if (ca_w == 0) ca_w = 3840;
		if (ca_h == 0) ca_h = 2160;

		// Resolve controller swapchain → SRV + texture dims. Acquire the
		// keyed-mutex once per cursor render pass (KEYEDMUTEX semantics
		// flush controller-side writes into the runtime's D3D11 device).
		struct d3d11_service_swapchain *cur_sc = d3d11_service_swapchain_from_xrt(sys->cursor_xsc);
		ID3D11ShaderResourceView *cursor_srv = nullptr;
		uint32_t sprite_w_px = 0, sprite_h_px = 0;
		IDXGIKeyedMutex *cursor_mutex = nullptr;
		bool cursor_mutex_held = false;
		if (cur_sc != nullptr && cur_sc->image_count > 0 && cur_sc->images[0].srv) {
			cursor_srv = cur_sc->images[0].srv.get();
			D3D11_TEXTURE2D_DESC sdesc = {};
			cur_sc->images[0].texture->GetDesc(&sdesc);
			sprite_w_px = sdesc.Width;
			sprite_h_px = sdesc.Height;
			cursor_mutex = cur_sc->images[0].keyed_mutex.get();
			if (cursor_mutex != nullptr) {
				if (SUCCEEDED(cursor_mutex->AcquireSync(0, 4 /* ms */))) {
					cursor_mutex_held = true;
				}
			}
		}
		if (cursor_srv != nullptr && sprite_w_px > 0 && sprite_h_px > 0) {
			// Eye positions: prefer the runtime's predicted eye positions
			// (Leia SR provides head-tracked values); fall back to fixed
			// 64 mm IPD at 60 cm if unavailable.
			struct xrt_vec3 eye_l = {-0.032f, 0.0f, 0.6f};
			struct xrt_vec3 eye_r = {+0.032f, 0.0f, 0.6f};
			(void)comp_d3d11_service_get_predicted_eye_positions(&sys->base, &eye_l, &eye_r);

			float disp_w_m = sys->base.info.display_width_m;
			if (disp_w_m <= 0.0f) disp_w_m = 0.700f;
			uint32_t panel_w = sys->base.info.display_pixel_width;
			if (panel_w == 0) panel_w = ca_w;
			uint32_t panel_h = sys->base.info.display_pixel_height;
			if (panel_h == 0) panel_h = ca_h;

			// Per-tile atlas region size from the active rendering mode.
			// For SBS 2×1 fullsize: tile = (ca_w/2, ca_h). For LeiaSR 3D
			// half-active mode: tile = (ca_w/4, ca_h/2) and the tiles
			// occupy only the top-left quadrant of the swapchain — the
			// DP later upscales that sub-rect to the full panel. We
			// MUST use the per-mode view_width_pixels / view_height_pixels
			// (via resolve_active_view_dims) instead of ca_w/n_cols, or
			// the cursor renders at the wrong proportional position when
			// the active area isn't the full atlas.
			const uint32_t n_cols = sys->tile_columns > 0 ? sys->tile_columns : 1;
			const uint32_t n_rows = sys->tile_rows > 0 ? sys->tile_rows : 1;
			uint32_t tile_w_v = 0, tile_h_v = 0;
			resolve_active_view_dims(sys, ca_w, ca_h, &tile_w_v, &tile_h_v);
			const uint32_t tile_w = tile_w_v;
			const uint32_t tile_h = tile_h_v;

			// Half-disparity in atlas pixels. Sign convention: positive
			// half_disp_atlas_px = cursor in front of panel (crossed
			// disparity), so the LEFT eye's view shifts the cursor to
			// the right and the RIGHT eye's view shifts it left.
			//   t = eye_z / (eye_z - hit_z),  per-eye-screen-shift = ipd/2 * (t - 1)
			// = ipd/2 * hit_z / (eye_z - hit_z)
			float ipd_half_m = (eye_r.x - eye_l.x) * 0.5f;
			float eye_z_m = (eye_l.z + eye_r.z) * 0.5f;
			float hit_z = mc->cursor_hit_z_m;
			float half_disp_panel_m = 0.0f;
			if (eye_z_m > hit_z + 0.001f) {
				half_disp_panel_m = ipd_half_m * hit_z / (eye_z_m - hit_z);
			}
			float half_disp_tile_px =
			    half_disp_panel_m / disp_w_m * (float)tile_w;

			// Base cursor POSITION within one tile (panel pixel → tile-
			// local atlas pixel: panel coords get scaled by tile_w/panel_w
			// and tile_h/panel_h). NOT used for cursor SIZE — atlas pixels
			// map 1:1 to perceived panel pixels post-DP-weave, so the
			// sprite renders at native dimensions.
			const float scale_x = (float)tile_w / (float)panel_w;
			const float scale_y = (float)tile_h / (float)panel_h;
			float base_tile_x = (float)mc->cursor_panel_x * scale_x;
			float base_tile_y = (float)mc->cursor_panel_y * scale_y;

			// Cursor sprite size + hot spot. spec_version 13: physical
			// size comes from the controller (sys->cursor_size_m), hot
			// spot from the controller (sys->cursor_hot_x/y in sprite UV).
			// Map physical meters → atlas pixels through the per-mode
			// tile width: cursor_w_atlas = (size_m / display_w_m) *
			// tile_w. After the DP weaver upscales the tile to the
			// panel, the cursor appears at the requested physical size.
			(void)n_cols; (void)n_rows; (void)panel_w; (void)panel_h; // size ratio no longer needed
			float cursor_w_atlas = sys->cursor_size_m / disp_w_m * (float)tile_w;
			float cursor_h_atlas = cursor_w_atlas; // square aspect for v1
			float hot_x_atlas    = sys->cursor_hot_x * cursor_w_atlas;
			float hot_y_atlas    = sys->cursor_hot_y * cursor_h_atlas;

			// Over-window cosmetic: render the cursor at 30 % alpha so
			// it doesn't fight content behind it (reduces lenticular
			// crosstalk on the cursor's bright pixels). spec_version 22:
			// the flag is pushed by the controller (which owns the
			// hit-test) via xrSetWorkspaceCursorDepthEXT, rather than
			// derived from a runtime raycast. An explicit flag is needed
			// because hit_z alone wouldn't work for windows at z = 0
			// (panel plane), where hit_z is 0 even though the cursor IS
			// over a workspace client.
			const bool over_window = mc->cursor_over_window;
			// #376: the over-window body alpha is controller-owned (look-and-feel)
			// — pushed via xrSetWorkspaceCursorDepthEXT (spec 23). Defaults to the
			// previous hardcoded 0.30 until the first controller push.
			const float body_tint[4]  = {1.00f, 1.00f, 1.00f, mc->cursor_dim_factor};

			// Common pipeline state
			ID3D11RenderTargetView *crtvs[] = {mc->combined_atlas_rtv.get()};
			sys->context->OMSetRenderTargets(1, crtvs, nullptr);
			sys->context->OMSetBlendState(sys->blend_alpha.get(), nullptr, 0xFFFFFFFF);
			sys->context->OMSetDepthStencilState(sys->depth_disabled.get(), 0);
			sys->context->RSSetState(sys->rasterizer_state.get());
			sys->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
			sys->context->IASetInputLayout(nullptr);
			D3D11_VIEWPORT cvp = {};
			cvp.Width = (float)ca_w;
			cvp.Height = (float)ca_h;
			cvp.MaxDepth = 1.0f;
			sys->context->RSSetViewports(1, &cvp);
			sys->context->VSSetShader(sys->blit_vs.get(), nullptr, 0);
			sys->context->PSSetShader(sys->blit_ps.get(), nullptr, 0);
			sys->context->VSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());
			sys->context->PSSetConstantBuffers(0, 1, sys->blit_constant_buffer.addressof());

			sys->context->PSSetShaderResources(0, 1, &cursor_srv);
			sys->context->PSSetSamplers(0, 1, sys->sampler_linear.addressof());

			// Iterate every (col, row) tile in the atlas. Each tile gets
			// one cursor draw at the proportional in-tile position with
			// per-eye disparity offset. eye_idx = col % 2 matches the
			// existing window-blit eye-mapping convention; multiview
			// layouts (col > 1) collapse to one of the two tracked eye
			// positions the same way windows do.
			for (uint32_t row = 0; row < n_rows; row++) {
				for (uint32_t col = 0; col < n_cols; col++) {
					int eye_idx = (int)(col % 2);
					float disp_off = (eye_idx == 0) ? +half_disp_tile_px
					                                : -half_disp_tile_px;
					float dest_x = (float)(col * tile_w)
					             + base_tile_x - hot_x_atlas + disp_off;
					float dest_y = (float)(row * tile_h)
					             + base_tile_y - hot_y_atlas;

					D3D11_MAPPED_SUBRESOURCE m;
					if (FAILED(sys->context->Map(sys->blit_constant_buffer.get(), 0,
					                              D3D11_MAP_WRITE_DISCARD, 0, &m))) {
						continue;
					}
					BlitConstants *cb = static_cast<BlitConstants *>(m.pData);
					memset(cb, 0, sizeof(*cb));
					cb->src_rect[0] = 0;
					cb->src_rect[1] = 0;
					cb->src_rect[2] = (float)sprite_w_px;
					cb->src_rect[3] = (float)sprite_h_px;
					cb->src_size[0] = (float)sprite_w_px;
					cb->src_size[1] = (float)sprite_h_px;
					cb->dst_size[0] = (float)ca_w;
					cb->dst_size[1] = (float)ca_h;
					cb->dst_offset[0] = dest_x;
					cb->dst_offset[1] = dest_y;
					cb->dst_rect_wh[0] = cursor_w_atlas;
					cb->dst_rect_wh[1] = cursor_h_atlas;
					cb->convert_srgb = 0.0f;
					cb->chrome_alpha = 0.0f;
					cb->quad_mode = 0.0f;
					if (over_window) {
						// Multiplicative tint via the shader's
						// flat-tint path (edge_feather <= 0,
						// glow_intensity > 0). RGB stays at 1.0
						// so the cursor keeps its natural color;
						// alpha drops to 0.55 so the cursor reads
						// as semi-transparent over content,
						// reducing lenticular crosstalk on its
						// bright pixels.
						cb->edge_feather = 0.0f;
						cb->glow_intensity = 1.0f;
						cb->glow_color[0] = body_tint[0];
						cb->glow_color[1] = body_tint[1];
						cb->glow_color[2] = body_tint[2];
						cb->glow_color[3] = body_tint[3];
					} else {
						cb->glow_intensity = 0.0f;
					}
					sys->context->Unmap(sys->blit_constant_buffer.get(), 0);

					// Tile-local scissor — keeps a left-tile cursor with
					// positive disparity from spilling into the right tile.
					D3D11_RECT cscissor;
					cscissor.left   = (LONG)(col * tile_w);
					cscissor.top    = (LONG)(row * tile_h);
					cscissor.right  = (LONG)((col + 1) * tile_w);
					cscissor.bottom = (LONG)((row + 1) * tile_h);
					sys->context->RSSetScissorRects(1, &cscissor);

					sys->context->Draw(4, 0);
				}
			}

			// Restore full-atlas scissor for downstream passes (DP, etc.).
			D3D11_RECT cfull = {0, 0, (LONG)ca_w, (LONG)ca_h};
			sys->context->RSSetScissorRects(1, &cfull);
		}
		if (cursor_mutex_held) {
			cursor_mutex->ReleaseSync(0);
		}
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

	// Phase 1 Task 1.4 — env-gated Present-to-Present interval log for the
	// workspace-mode multi-compositor swap chain. Production builds pay
	// nothing (one getenv on first frame, then a static-cached 0/1
	// branch). Bench harness flips DISPLAYXR_LOG_PRESENT_NS=1 to enable.
	// Greppable.
	{
		static int log_present_ns = -1;
		if (log_present_ns < 0) {
			const char *e = getenv("DISPLAYXR_LOG_PRESENT_NS");
			log_present_ns = (e != nullptr && e[0] == '1') ? 1 : 0;
		}
		if (log_present_ns) {
			static int64_t last_present_ns = 0;
			int64_t now_ns = os_monotonic_get_ns();
			if (last_present_ns != 0) {
				U_LOG_W("[PRESENT_NS] client=workspace dt_ns=%lld",
				        (long long)(now_ns - last_present_ns));
			}
			last_present_ns = now_ns;
		}
	}

	// Present
	if (mc->swap_chain) {
		mc->swap_chain->Present(1, 0);
	}

	// Signal WM_PAINT done
	if (mc->window != nullptr) {
		comp_d3d11_window_signal_paint_done(mc->window);
	}

	// spec_version 20: cache the live viewer eye-midpoint for the FRAME_TICK
	// event (head-tracked billboarding in the controller). On the render thread
	// here, so the display processor is stable — same call the cursor path makes
	// above. Falls back silently (valid stays as last) if no DP is available.
	{
		struct xrt_vec3 vl, vr;
		if (comp_d3d11_service_get_predicted_eye_positions(&sys->base, &vl, &vr)) {
			mc->frame_tick_viewer_x = 0.5f * (vl.x + vr.x);
			mc->frame_tick_viewer_y = 0.5f * (vl.y + vr.y);
			mc->frame_tick_viewer_z = 0.5f * (vl.z + vr.z);
			mc->frame_tick_viewer_valid = 1;
		}
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

	// Phase 5a — per-stage profile of compositor_layer_commit. Stack-local
	// timestamps; emitted as one [COMMIT_PROFILE_SVC] line at the workspace
	// branch return, env-gated by DISPLAYXR_LOG_PRESENT_NS=1 (same gate as
	// [CLIENT_FRAME_NS]). Capturing 5 os_monotonic_get_ns() per commit is
	// ~100 ns each on Windows; cheap enough to leave unconditional.
	int64_t profile_s0 = os_monotonic_get_ns();
	int64_t profile_s1 = 0, profile_s2 = 0, profile_s3 = 0, profile_s4 = 0;

	// Phase 1 diagnostic — env-gated per-client commit interval. One
	// line per client per xrEndFrame; tagged by client struct pointer so
	// multi-client runs can be split out. Cheap (one getenv on first
	// frame, then a static-cached branch), and works in BOTH workspace
	// and standalone modes for direct comparison.
	{
		static int log_client_frame_ns = -1;
		if (log_client_frame_ns < 0) {
			const char *e = getenv("DISPLAYXR_LOG_PRESENT_NS");
			log_client_frame_ns = (e != nullptr && e[0] == '1') ? 1 : 0;
		}
		if (log_client_frame_ns) {
			if (c->last_commit_ns != 0) {
				U_LOG_W("[CLIENT_FRAME_NS] client=%p dt_ns=%lld",
				        (void *)c,
				        (long long)(profile_s0 - c->last_commit_ns));
			}
			c->last_commit_ns = profile_s0;
		}
	}

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
	// WebXR sample via HWND property. Polls each frame; when the multi-comp is
	// active, routes through the acked-flip path (#234) so the broadcast →
	// per-slot re-submit → DP flip sequence is masked by the curtain. Without
	// multi-comp (legacy bridge with its own per-client DP), preserve the
	// immediate-flip behavior.
	if (bridge_live && c->render.hwnd != nullptr && sys->xsysd != NULL) {
		uint32_t req = (uint32_t)(uintptr_t)GetPropW(c->render.hwnd, L"DXR_RequestMode");
		if (req > 0) {
			RemovePropW(c->render.hwnd, L"DXR_RequestMode");
			uint32_t modeIdx = req - 1; // decode +1 encoding
			struct xrt_device *head = sys->xsysd->static_roles.head;
			if (head != nullptr && head->hmd != NULL &&
			    modeIdx < head->rendering_mode_count &&
			    modeIdx != head->hmd->active_rendering_mode_index) {
				if (sys->multi_comp != nullptr) {
					multi_compositor_request_mode_flip(sys, modeIdx, /*origin=*/-1);
					U_LOG_W("App-initiated mode change: request %u (via acked-flip)", modeIdx);
				} else {
					uint32_t prev_idx = head->hmd->active_rendering_mode_index;
					head->hmd->active_rendering_mode_index = modeIdx;
					broadcast_rendering_mode_change(sys, head, prev_idx, modeIdx);

					bool want_3d = head->rendering_modes[modeIdx].hardware_display_3d;
					if (c->render.display_processor != nullptr) {
						xrt_display_processor_d3d11_request_display_mode(
						    c->render.display_processor, want_3d);
					}

					sync_tile_layout(sys);
					sys->hardware_display_3d = want_3d;
					U_LOG_W("App-initiated mode change: %u -> %u (3D=%d, immediate)",
					        prev_idx, modeIdx, (int)want_3d);
				}
			}
		}
	}

	// Runtime-side 2D/3D toggle (V key) + 1/2/3 mode-select — polls qwerty
	// driver each frame.
	// Disabled when bridge is active: mode changes go through the HWND
	// property relay above (app-initiated path). When the multi-comp is
	// active, routes through the acked-flip path (#234); otherwise preserves
	// the immediate-flip behavior on the per-client DP.
	// #303: also disabled under workspace mode — the workspace controller
	// owns these keys and drives mode via xrRequestDisplayRenderingModeEXT
	// (ADR-018 "controller owns input"). qwerty is already suppressed at the
	// window WM_KEYDOWN gate under workspace mode, but gate the apply site too
	// so an activation/deactivation race can't sneak a flip through here.
#ifdef XRT_BUILD_DRIVER_QWERTY
	if (sys->xsysd != NULL && !bridge_live && !sys->workspace_mode) {
		bool force_2d = false;
		bool toggled = qwerty_check_display_mode_toggle(
		    sys->xsysd->xdevs, sys->xsysd->xdev_count, &force_2d);
		if (toggled) {
			struct xrt_device *head = sys->xsysd->static_roles.head;
			if (head != nullptr && head->hmd != NULL) {
				uint32_t target_idx;
				if (force_2d) {
					uint32_t cur = head->hmd->active_rendering_mode_index;
					if (cur < head->rendering_mode_count &&
					    head->rendering_modes[cur].hardware_display_3d) {
						sys->last_3d_mode_index = cur;
					}
					target_idx = 0; // mode 0 = 2D mono
				} else {
					target_idx = sys->last_3d_mode_index;
				}

				if (sys->multi_comp != nullptr) {
					multi_compositor_request_mode_flip(sys, target_idx, /*origin=*/-1);
				} else {
					uint32_t prev_idx = head->hmd->active_rendering_mode_index;
					head->hmd->active_rendering_mode_index = target_idx;
					broadcast_rendering_mode_change(sys, head, prev_idx, target_idx);
					if (c->render.display_processor != nullptr) {
						xrt_display_processor_d3d11_request_display_mode(
						    c->render.display_processor, !force_2d);
					}
					sync_tile_layout(sys);
					sys->hardware_display_3d = !force_2d;
				}
			}
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

	int64_t profile_s0a = os_monotonic_get_ns(); // Phase 5a — pre vendor 3d-state poll.

	// Poll vendor SDK for hardware 3D state changes (e.g., Leia SR auto-switch on tracking loss).
	// This detects changes the vendor SDK made independently of the runtime.
	//
	// Phase 5b — rate-limited cache. The raw vendor call blocks ~10 ms (Leia
	// SR dev box). At 4 cubes × 60 Hz = 240 commits/sec, calling per-frame
	// saturated the IPC hot path. CAS-protected once-per-100ms slow path:
	// only one cube poll per window does the vendor call; everyone else
	// reads the cached state. Hardware mode changes happen at human-
	// interaction speed (sub-Hz), so 10 Hz polling is overkill but cheap.
	{
		bool vendor_is_3d = false;
		bool got_state = false;

		const int64_t kPollIntervalNs = 100LL * 1000000LL; // 100 ms
		int64_t now_ns = profile_s0a;
		int64_t last_poll = sys->last_3d_state_poll_ns.load(std::memory_order_acquire);
		bool cache_valid = sys->cached_3d_state_valid.load(std::memory_order_acquire);

		bool need_fresh_poll = !cache_valid || (now_ns - last_poll) >= kPollIntervalNs;
		bool won_poll_race = false;
		if (need_fresh_poll) {
			// CAS guards against multiple cubes simultaneously deciding
			// to refresh the cache. Only one wins; others fall through
			// to read whatever the winner publishes (or the prior cache
			// if not yet published — minor staleness, no correctness issue).
			won_poll_race = sys->last_3d_state_poll_ns.compare_exchange_strong(
			    last_poll, now_ns, std::memory_order_acq_rel, std::memory_order_acquire);
		}

		if (won_poll_race) {
			struct xrt_display_processor_d3d11 *dp = nullptr;
			if (sys->workspace_mode && sys->multi_comp != nullptr) {
				dp = sys->multi_comp->display_processor;
			} else if (c->render.display_processor != nullptr) {
				dp = c->render.display_processor;
			}
			if (xrt_display_processor_d3d11_get_hardware_3d_state(dp, &vendor_is_3d)) {
				sys->cached_3d_state.store(vendor_is_3d, std::memory_order_relaxed);
				sys->cached_3d_state_valid.store(true, std::memory_order_release);
				got_state = true;
			}
		} else if (cache_valid) {
			vendor_is_3d = sys->cached_3d_state.load(std::memory_order_relaxed);
			got_state = true;
		}

		if (got_state) {
			// Suppress vendor-poll detection while an acked-flip is in progress
			// (#234). During WAITING_ACK / FLIPPING the workspace has written
			// sys->hardware_display_3d to the TARGET state but the vendor
			// hardware is still mid-transition — the poll would (re)trigger a
			// flip in the OPPOSITE direction, creating a feedback loop. Let
			// the in-flight flip land; the post-flip apply_pending tick polls
			// get_hardware_3d_state itself and bounds the wait.
			bool pending_flip = sys->multi_comp != nullptr &&
			                    sys->multi_comp->mode_flip.phase != MFP_IDLE;
			// Post-flip cooldown (#234). After a flip lands the vendor SDK's
			// `is_3d` reading can briefly disagree with sys->hardware_display_3d
			// because the vendor's cached state lags ~1 frame behind the
			// hardware command. Without this gate the very next 100 ms poll
			// window observes the disagreement and requests a counter-correction
			// flip back to the prior mode. Two seconds is generous — enough for
			// any vendor-side settle, short enough that a genuine vendor-
			// initiated change (e.g. user covers the eye-tracking camera) is
			// still picked up promptly.
			const int64_t kPostFlipCooldownNs = 2000LL * 1000000LL;
			int64_t last_landed = sys->last_flip_landed_ns.load(std::memory_order_acquire);
			bool in_cooldown = last_landed > 0 && (now_ns - last_landed) < kPostFlipCooldownNs;
			// Under workspace mode the controller is the sole mode authority.
			// If the vendor SDK silently flips itself (SR auto-fallback,
			// SR Dashboard external toggle), re-assert the runtime's desired
			// state on the DP rather than counter-correcting the runtime.
			// Observed at shell-with-apps launch: vendor reports 3D briefly,
			// then drops to 2D ~2s later (right as the post-flip cooldown
			// expires) — display would look 2D even though apps render 3D.
			// Re-asserting via DP request keeps the hardware locked to what
			// the workspace controller asked for. #234.
			if (sys->workspace_mode && vendor_is_3d != sys->hardware_display_3d &&
			    !pending_flip && !in_cooldown) {
				if (sys->multi_comp != nullptr && sys->multi_comp->display_processor != nullptr) {
					U_LOG_W("[force_3d] vendor drifted (vendor=%s runtime=%s) — re-asserting DP to runtime state",
					        vendor_is_3d ? "3D" : "2D",
					        sys->hardware_display_3d ? "3D" : "2D");
					xrt_display_processor_d3d11_request_display_mode(
					    sys->multi_comp->display_processor,
					    sys->hardware_display_3d);
					// Refresh cooldown so we don't hammer the DP on every poll
					// while the vendor settles back.
					sys->cached_3d_state.store(sys->hardware_display_3d, std::memory_order_relaxed);
					sys->last_3d_state_poll_ns.store(now_ns, std::memory_order_release);
					sys->last_flip_landed_ns.store(now_ns, std::memory_order_release);
				}
				got_state = false; // skip the legacy counter-correction below
			}
			if (vendor_is_3d != sys->hardware_display_3d && !pending_flip && !in_cooldown && got_state) {
				U_LOG_W("Vendor SDK hardware 3D state changed: %s → %s",
				        sys->hardware_display_3d ? "3D" : "2D",
				        vendor_is_3d ? "3D" : "2D");
				// When bridge is active, don't force the rendering mode
				// transition — let the app decide via requestRenderingMode().
				// The app receives a hardwarestatechange event and can react.
				// Forcing causes a brief glitch (2D content through 3D weaver).
				if (!bridge_live) {
				struct xrt_device *head = sys->xsysd ? sys->xsysd->static_roles.head : nullptr;
				if (head != nullptr && head->hmd != NULL) {
					uint32_t target_idx;
					if (!vendor_is_3d) {
						uint32_t cur = head->hmd->active_rendering_mode_index;
						if (cur < head->rendering_mode_count &&
						    head->rendering_modes[cur].hardware_display_3d) {
							sys->last_3d_mode_index = cur;
						}
						target_idx = 0; // mode 0 = 2D
					} else {
						target_idx = sys->last_3d_mode_index;
					}
					if (sys->multi_comp != nullptr) {
						// Vendor-initiated: hardware is ALREADY at vendor_is_3d
						// (the poll just discovered it). We still route through
						// the acked-flip path so apps re-submit at the new
						// layout; FLIPPING-phase vendor poll will see the
						// hardware already settled and lift the curtain quickly.
						multi_compositor_request_mode_flip(sys, target_idx, /*origin=*/-1);
					} else {
						uint32_t prev_idx = head->hmd->active_rendering_mode_index;
						head->hmd->active_rendering_mode_index = target_idx;
						broadcast_rendering_mode_change(sys, head, prev_idx, target_idx);
						sync_tile_layout(sys);
						sys->hardware_display_3d = vendor_is_3d;
					}
				}
				} else {
					// Bridge path: just mirror sys->hardware_display_3d so the
					// app's hardwarestatechange event fires; don't touch mode.
					sys->hardware_display_3d = vendor_is_3d;
				}
			}
		}
	}

	int64_t profile_s0b = os_monotonic_get_ns(); // Phase 5a — post vendor 3d-state poll.

	// Get predicted eye positions (used for UI layers and HUD).
	// This is a cheap snapshot read — the Leia DP subscribes to SR's
	// EyePairStream listener and maintains the cache internally (#248
	// Tier 2). No SDK polling on the per-client commit path.
	struct xrt_eye_positions eye_pos = {};
	bool weaving_done = false;
	if (c->render.display_processor != nullptr) {
		xrt_display_processor_d3d11_get_predicted_eye_positions(
		    c->render.display_processor, &eye_pos);
	}

	int64_t profile_s0c = os_monotonic_get_ns(); // Phase 5a — post vendor eye-pos poll.
	if (!eye_pos.valid) {
		eye_pos.count = 2;
		eye_pos.eyes[0] = {-0.032f, 0.0f, 0.6f};
		eye_pos.eyes[1] = { 0.032f, 0.0f, 0.6f};
	}

	// Extract stereo pair for renderer (display processor still needs L/R)
	struct xrt_vec3 left_eye = {eye_pos.eyes[0].x, eye_pos.eyes[0].y, eye_pos.eyes[0].z};
	struct xrt_vec3 right_eye = {eye_pos.eyes[1].x, eye_pos.eyes[1].y, eye_pos.eyes[1].z};

	// Pre-compute whether there are UI overlay layers (quad/cylinder/equirect/cube)
	// or window-space layers. Either disables zero-copy because we must blit on
	// top of the projection content.
	bool has_ui_layers = false;
	bool has_window_space_layers = false;
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		switch (c->layer_accum.layers[i].data.type) {
		case XRT_LAYER_QUAD:
		case XRT_LAYER_CYLINDER:
		case XRT_LAYER_EQUIRECT2:
		case XRT_LAYER_EQUIRECT1:
		case XRT_LAYER_CUBE:
			has_ui_layers = true;
			break;
		case XRT_LAYER_WINDOW_SPACE:
			has_window_space_layers = true;
			break;
		default:
			break;
		}
		if (has_ui_layers && has_window_space_layers) break;
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

	profile_s1 = os_monotonic_get_ns(); // Phase 5a — end of pre-loop setup.

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
		// Phase 1 Task 1.1 — per-view zero-copy eligibility. Default true;
		// flipped false below by anything that disqualifies the view (the
		// view required a service-side mutex acquire, or that acquire
		// timed out / failed). Replaces the old per-commit
		// `any_mutex_acquired` flag so one ineligible view does not nuke
		// the fast path for its siblings.
		bool view_zc_eligible[XRT_MAX_VIEWS] = {};
		// Phase 1 Task 1.2 — per-view "skip the blit" flag. Set when the
		// service could NOT safely read this view's source texture (mutex
		// timeout). The per-client atlas slot is persistent, so skipping
		// the blit reuses last frame's tile content for that one view —
		// converting a 100 ms render-thread stall into a 1-frame quality
		// blip. Other views in the same client commit are unaffected.
		bool view_skip_blit[XRT_MAX_VIEWS] = {};
		for (uint32_t e = 0; e < proj_view_count; e++) {
			view_zc_eligible[e] = true;
		}

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

		// Phase 1 Task 1.2 — drop service-thread KeyedMutex timeout from
		// 100 ms to a frame-budget value (4 ms, matching the chrome-overlay
		// path at the top of this file). On timeout we skip this view's
		// blit entirely so the per-client atlas slot retains its prior
		// content — a 1-frame quality blip on that one tile rather than a
		// ~100 ms render-thread stall that drops 6 frames at 60 Hz.
		const DWORD mutex_timeout_ms = 4;
		for (uint32_t eye = 0; eye < proj_view_count; eye++) {
			// Skip mutex for views sharing the same swapchain+image as a prior view
			bool already_locked = false;
			uint32_t prev_idx = 0;
			for (uint32_t prev = 0; prev < eye; prev++) {
				if (view_scs[eye] == view_scs[prev] && view_img_indices[eye] == view_img_indices[prev]) {
					already_locked = true;
					prev_idx = prev;
					break;
				}
			}
			if (already_locked) {
				// Inherit eligibility / skip from the view that did
				// the actual acquire so the per-view arrays stay
				// consistent across the blit / release loops.
				view_zc_eligible[eye] = view_zc_eligible[prev_idx];
				view_skip_blit[eye] = view_skip_blit[prev_idx];
				continue;
			}

			if (view_scs[eye]->service_created && view_scs[eye]->images[view_img_indices[eye]].keyed_mutex) {
				// Phase 2 fence path is opt-in per client. The service
				// always creates a workspace_sync_fence at session-create,
				// but only clients that import + signal it
				// (comp_d3d11_client, comp_vk_client) participate. The
				// remaining clients (comp_d3d12_client, comp_gl_*_client)
				// never call get_workspace_sync_fence and never signal,
				// so `last_signaled_fence_value` stays 0 for the session.
				// Treat that 0 as the "no fence in use" sentinel the
				// original Phase 2 commit message described and fall
				// through to the legacy KeyedMutex path — without this
				// gate, every d3d12/gl projection-view blit is marked
				// stale and skipped, leaving the per-client atlas empty
				// (chrome panel shows through; HUD WS-layers still work
				// because they bypass this loop).
				uint64_t fence_signaled = c->workspace_sync_fence
				    ? c->last_signaled_fence_value.load(std::memory_order_acquire)
				    : 0;
				bool use_fence_path = c->workspace_sync_fence && fence_signaled != 0;
				if (use_fence_path) {
					// Phase 2 — GPU-side fence path. No CPU
					// wait, no IDXGIKeyedMutex acquire. The
					// client signals `last_signaled_fence_value`
					// at xrEndFrame after submitting render
					// commands; we cheaply read the atomic and
					// either queue a non-blocking
					// `ID3D11DeviceContext4::Wait` (frame is
					// fresh) or skip the blit entirely (no new
					// frame since last compose, atlas slot is
					// reused — same trick Phase 1's mutex
					// timeout handler uses, but driven by the
					// fence rather than a 4 ms wall-clock
					// timeout).
					uint64_t signaled = fence_signaled;
					if (signaled == c->last_composed_fence_value[eye]) {
						// Client hasn't produced a new frame
						// since we last composed this view —
						// reuse persistent atlas slot content.
						view_skip_blit[eye] = true;
						view_zc_eligible[eye] = false;
						c->fence_stale_views_in_window++;
					} else {
						// Fresh frame. The shared swapchain
						// texture still has
						// `D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX`
						// set (we left swapchain creation
						// alone so legacy / WebXR-bridge
						// clients keep working). Per the
						// D3D11 spec, every cross-process
						// access to such a texture must be
						// bracketed by AcquireSync /
						// ReleaseSync — that is what issues
						// the cross-process GPU memory
						// barrier. Skipping it on the reader
						// side means stale / undefined data
						// (manifested empirically as empty
						// cubes on the dev box).
						//
						// The fence guarantees the client's
						// render commands are done by the
						// time it signaled + shipped the
						// value, so we acquire with a
						// 0-timeout: succeeds immediately in
						// steady state, or returns
						// WAIT_TIMEOUT if the writer is
						// still mid-release (treat as a
						// stale view, reuse the persistent
						// atlas slot — same Phase 1 trick).
						// This is the cheapest possible
						// CPU touchpoint that preserves the
						// SHARED_KEYEDMUTEX contract; the
						// real GPU sync still rides on the
						// fence Wait below.
						HRESULT hr_a =
						    view_scs[eye]->images[view_img_indices[eye]].keyed_mutex->AcquireSync(
						        0, 0);
						if (SUCCEEDED(hr_a)) {
							view_mutex_acquired[eye] = true;
							sys->context->Wait(
							    c->workspace_sync_fence.get(),
							    signaled);
							c->last_composed_fence_value[eye] = signaled;
							c->fence_waits_queued_in_window++;
						} else {
							view_skip_blit[eye] = true;
							view_zc_eligible[eye] = false;
							c->fence_stale_views_in_window++;
						}
						// Phase 2 leaves zero-copy semantics
						// unchanged - `view_zc_eligible[eye]`
						// stays at its init value (true) and
						// the existing downstream gates
						// (single_view, ui_layers,
						// workspace_mode, etc.) continue to
						// govern the zc decision. Phase 3 may
						// revisit this once per-client pacing
						// removes the workspace_mode gate.
					}
				} else {
					int64_t acq_start_ns = os_monotonic_get_ns();
					HRESULT hr = view_scs[eye]->images[view_img_indices[eye]].keyed_mutex->AcquireSync(0, mutex_timeout_ms);
					int64_t acq_dt_ns = os_monotonic_get_ns() - acq_start_ns;
					c->mutex_acquires_in_window++;
					c->mutex_acquire_total_ns_in_window += acq_dt_ns;
					if (SUCCEEDED(hr)) {
						view_mutex_acquired[eye] = true;
						// Holding a cross-process keyed mutex
						// disqualifies this view from the
						// downstream zero-copy path: that path
						// would have to keep the mutex held all
						// the way through DP submit + Present,
						// blocking the client's next AcquireSync.
						view_zc_eligible[eye] = false;
					} else if (hr == static_cast<HRESULT>(WAIT_TIMEOUT)) {
						// Skip this view's blit; previous frame's
						// tile in the per-client atlas is reused.
						view_skip_blit[eye] = true;
						view_zc_eligible[eye] = false;
						c->mutex_timeouts_in_window++;
						// Demoted from U_LOG_W: timeouts are
						// expected on slow clients and would spam
						// the service log otherwise. The
						// rate-limited [MUTEX] line below is the
						// authoritative signal.
						U_LOG_D("layer_commit: View %u mutex timeout (client still holding?) skipping blit", eye);
					} else {
						view_skip_blit[eye] = true;
						view_zc_eligible[eye] = false;
						U_LOG_W("layer_commit: Failed to acquire view %u mutex: 0x%08lx", eye, hr);
					}
				}
			}
		}

		// Phase 1 Task 1.3 — emit one [MUTEX] line per client per ~10 s
		// window summarising acquire health on the service render thread.
		// Greppable from the service log under %LOCALAPPDATA%\DisplayXR\.
		{
			int64_t now_ns = os_monotonic_get_ns();
			if (c->mutex_window_start_ns == 0) {
				c->mutex_window_start_ns = now_ns;
			}
			int64_t window_ns = now_ns - c->mutex_window_start_ns;
			if (window_ns >= 10LL * 1000LL * 1000LL * 1000LL) {
				uint32_t avg_us = 0;
				if (c->mutex_acquires_in_window > 0) {
					avg_us = (uint32_t)((c->mutex_acquire_total_ns_in_window /
					                     (int64_t)c->mutex_acquires_in_window) / 1000);
				}
				U_LOG_W("[MUTEX] client=%p timeouts=%u acquires=%u avg_acquire_us=%u window_s=%lld",
				        (void *)c,
				        c->mutex_timeouts_in_window,
				        c->mutex_acquires_in_window,
				        avg_us,
				        (long long)(window_ns / 1000000000LL));
				c->mutex_window_start_ns = now_ns;
				c->mutex_timeouts_in_window = 0;
				c->mutex_acquires_in_window = 0;
				c->mutex_acquire_total_ns_in_window = 0;
			}
		}

		// Phase 2 — emit one [FENCE] line per client per ~10 s window
		// summarising the new GPU-wait path. Mirrors the [MUTEX] window
		// pattern so the bench harness can A/B compare directly.
		// Greppable; emitted at U_LOG_W (the project filter drops U_LOG_I).
		{
			int64_t now_ns = os_monotonic_get_ns();
			if (c->fence_window_start_ns == 0) {
				c->fence_window_start_ns = now_ns;
			}
			int64_t window_ns = now_ns - c->fence_window_start_ns;
			if (window_ns >= 10LL * 1000LL * 1000LL * 1000LL) {
				uint64_t last_value =
				    c->last_signaled_fence_value.load(std::memory_order_relaxed);
				U_LOG_W("[FENCE] client=%p waits_queued=%u stale_views=%u last_value=%llu window_s=%lld",
				        (void *)c,
				        c->fence_waits_queued_in_window,
				        c->fence_stale_views_in_window,
				        (unsigned long long)last_value,
				        (long long)(window_ns / 1000000000LL));
				c->fence_window_start_ns = now_ns;
				c->fence_waits_queued_in_window = 0;
				c->fence_stale_views_in_window = 0;
			}
		}

		// Log projection layer info (first frame and every 60 frames)
		static uint32_t proj_log_count = 0;
		if (proj_log_count == 0 || proj_log_count % 60 == 0) {
			for (uint32_t eye = 0; eye < proj_view_count; eye++) {
				U_LOG_D("Projection layer %u view %u/%u: rect=(%d,%d %dx%d) array=%u fmt=%u(srgb=%d)",
				        i, eye, proj_view_count,
				        layer->data.proj.v[eye].sub.rect.offset.w, layer->data.proj.v[eye].sub.rect.offset.h,
				        layer->data.proj.v[eye].sub.rect.extent.w, layer->data.proj.v[eye].sub.rect.extent.h,
				        layer->data.proj.v[eye].sub.array_index,
				        view_descs[eye].Format, view_is_srgb[eye]);
			}
			U_LOG_D("  atlas_texture=%ux%u, view_width=%u, view_height=%u",
			        sys->display_width, sys->display_height, sys->view_width, sys->view_height);
		}
		proj_log_count++;

		// Zero-copy optimization: all views reference the same swapchain texture,
		// sub-rects match tiling layout, and texture matches content dimensions.
		// Skip the blit and pass the app's texture directly to the display processor.
		bool zero_copy = false;
		const char *zc_reason = "ok";

		// Phase 1 Task 1.1 — per-view eligibility AND. Old code used a single
		// per-commit `any_mutex_acquired` flag; the new array preserves
		// the same semantics (zero-copy needs every view safe to read
		// without holding a cross-process mutex) at a finer granularity
		// that Phase 2's shared-fence path will further leverage.
		bool all_views_zc_eligible = true;
		for (uint32_t e = 0; e < proj_view_count; e++) {
			if (!view_zc_eligible[e]) { all_views_zc_eligible = false; break; }
		}
		if (proj_view_count <= 1) zc_reason = "single_view";
		else if (has_ui_layers) zc_reason = "ui_layers";
		else if (has_window_space_layers) zc_reason = "window_space_layers";
		else if (!all_views_zc_eligible) zc_reason = "view_ineligible";
		else if (sys->workspace_mode) zc_reason = "workspace_mode";

		if (proj_view_count > 1 && !has_ui_layers && !has_window_space_layers && all_views_zc_eligible && !sys->workspace_mode) {
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

				if (active_mode == nullptr) {
					zc_reason = "no_active_mode";
				} else if (!u_tiling_can_zero_copy(proj_view_count, rect_xs, rect_ys, rect_ws, rect_hs,
				                                  view_descs[0].Width, view_descs[0].Height, active_mode)) {
					zc_reason = "tiling_mismatch";
				} else {
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
					} else {
						zc_reason = "srv_create_failed";
					}
				}
			} else {
				zc_reason = "view_unique_textures";
			}
		}

		// Phase 1 Task 1.3 — emit [ZC] one-shot per client whenever the
		// decision (or its reason) FLIPS. Greppable from the service log.
		if (!c->zc_last_logged_set ||
		    c->zc_last_logged_value != zero_copy ||
		    (c->zc_last_logged_reason != nullptr &&
		     zc_reason != nullptr &&
		     strcmp(c->zc_last_logged_reason, zc_reason) != 0)) {
			U_LOG_W("[ZC] client=%p views=%u zero_copy=%c reason=%s",
			        (void *)c,
			        proj_view_count,
			        zero_copy ? 'Y' : 'N',
			        zc_reason ? zc_reason : "?");
			c->zc_last_logged_set = true;
			c->zc_last_logged_value = zero_copy;
			c->zc_last_logged_reason = zc_reason;
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
			// Phase 1 Task 1.2 — mutex acquire timed out earlier;
			// the source texture is unsafe to read. Leave the per-
			// client atlas slot for this view untouched so it keeps
			// last frame's content. One-frame blip on this tile only.
			if (view_skip_blit[eye]) {
				continue;
			}
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
		// so the slot starts compositing only once Chrome (or any IPC client)
		// has actually submitted content. Without this, the multi-comp would
		// draw the slot with uninitialized atlas content for the 2-3s while
		// Chrome's WebGL is initializing — visible as a black rectangle.
		// Mirrors the capture-client `capture_srv` readiness gate.
		if (sys->workspace_mode && sys->multi_comp != nullptr) {
			for (int s = 0; s < D3D11_MULTI_MAX_CLIENTS; s++) {
				if (sys->multi_comp->clients[s].active &&
				    sys->multi_comp->clients[s].compositor == c) {
					struct d3d11_multi_client_slot *slot =
					    &sys->multi_comp->clients[s];
					slot->content_view_w = content_view_w;
					slot->content_view_h = content_view_h;

					// Snapshot per-slot stride for the workspace
					// per-tile blit (#234, Issue 3). Same formula
					// the clamp at L11385 uses — write/clamp/read
					// must agree, and capturing here keeps the slot
					// self-consistent even when sys->tile_columns
					// flips ahead of this slot's next commit.
					if (c->render.atlas_texture) {
						D3D11_TEXTURE2D_DESC sc_desc = {};
						c->render.atlas_texture->GetDesc(&sc_desc);
						uint32_t tc = sys->tile_columns > 0 ? sys->tile_columns : 1;
						uint32_t tr = sys->tile_rows > 0 ? sys->tile_rows : 1;
						slot->blit_slot_w = sc_desc.Width / tc;
						slot->blit_slot_h = sc_desc.Height / tr;
						slot->blit_tile_columns = tc;
						slot->blit_tile_rows = tr;
					}

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

	profile_s2 = os_monotonic_get_ns(); // Phase 5a — end of projection-layer loop.

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

	// Window-space layers (XR_EXT_window_space_layer) are NOT drawn here in
	// workspace mode — they are blitted onto the combined atlas at per-eye
	// window pixel coords inside `multi_compositor_render`, alongside the
	// chrome blit (which already runs there under `sys->render_mutex`).
	// Drawing on the per-client atlas from this commit-thread context
	// destabilised rendering (asymmetric left-eye flicker on one of two
	// cubes) because of D3D11 immediate-context contention with the
	// capture-render thread's in-flight chrome work. The combined-atlas
	// pass in multi_compositor_render keeps all WS GPU work on the same
	// thread that owns the combined atlas, eliminating the contention.
	//
	// Snapshot the WS layers into the slot's `ws_snapshot` so multi-comp
	// reads a stable post-commit state. Reading `c->layer_accum` directly
	// from multi-comp races with the cube's `xrBeginFrame` reset
	// (`layer_count = 0`) and per-call adds — manifesting as per-frame HUD
	// flicker (multi-comp ticks at ~70 Hz, cube xrEndFrame at ~60 Hz, ~50%
	// of multi-comp ticks fall inside the cube's begin→add window and see
	// no/partial WS layers).
	//
	// Standalone mode (no workspace_mode, single client renders directly
	// to its swap-chain back buffer) is not yet wired to render WS layers
	// — the legacy per-client-atlas path was removed with this change to
	// keep the single-source-of-truth invariant. If a standalone-mode
	// HUD is needed, follow the chrome-style pattern in this file.
	if (sys->workspace_mode && sys->multi_comp != nullptr) {
		for (int s_snap = 0; s_snap < D3D11_MULTI_MAX_CLIENTS; s_snap++) {
			struct d3d11_multi_client_slot *slot_snap = &sys->multi_comp->clients[s_snap];
			if (!slot_snap->active || slot_snap->compositor != c) {
				continue;
			}
			std::lock_guard<std::mutex> snap_lock(slot_snap->ws_snapshot_mutex);
			// Release strong refs the previous snapshot held on its
			// swapchains (#234 follow-on). comp_layer's sc_array is raw
			// pointers; we take xrt_swapchain refs on snapshot write so
			// the underlying swapchains stay alive while the render
			// thread reads them. Otherwise the IPC per-client cleanup
			// path (ipc_server_per_client_thread.c:510) drops xscs refs
			// WITHOUT sys->render_mutex, freeing the swapchains while
			// multi_compositor_render is still mid-HUD-pass — observed
			// as a UAF on `sc->images[img_idx].texture->GetDesc(...)` at
			// comp_d3d11_service.cpp:8213 (close-button crash).
			for (uint32_t j = 0; j < slot_snap->ws_snapshot_count; j++) {
				for (size_t k = 0;
				     k < ARRAY_SIZE(slot_snap->ws_snapshot[j].sc_array);
				     k++) {
					xrt_swapchain_reference(
					    &slot_snap->ws_snapshot[j].sc_array[k], NULL);
				}
			}
			slot_snap->ws_snapshot_count = 0;
			uint32_t snap_n = 0;
			bool saw_projection = false;
			for (uint32_t i = 0;
			     i < c->layer_accum.layer_count && snap_n < XRT_MAX_LAYERS;
			     i++) {
				const struct comp_layer *l = &c->layer_accum.layers[i];
				if ((l->data.type == XRT_LAYER_PROJECTION ||
				     l->data.type == XRT_LAYER_PROJECTION_DEPTH) &&
				    !saw_projection) {
					slot_snap->projection_flags_snapshot = l->data.flags;
					slot_snap->projection_flags_valid = true;
					saw_projection = true;

					// Acked-flip detection (#234): refresh the per-
					// slot extent snapshot every commit; when a
					// workspace mode-flip is pending, ack when the
					// extent has CHANGED since the pre_flip snapshot
					// (taken at request_mode_flip time). Workspace
					// apps submit window-scaled extents not the
					// canonical rendering_modes[].view_width_pixels,
					// so an absolute match doesn't work — but the
					// extent does change when the app re-renders at
					// the new tile_columns after consuming
					// XrEventDataRenderingModeChangedEXT.
					uint32_t got_w = l->data.proj.v[0].sub.rect.extent.w;
					uint32_t got_h = l->data.proj.v[0].sub.rect.extent.h;
					slot_snap->last_commit_view_w = got_w;
					slot_snap->last_commit_view_h = got_h;
					struct d3d11_multi_compositor *mc_ack = sys->multi_comp;
					if (mc_ack != nullptr &&
					    mc_ack->mode_flip.phase == MFP_WAITING_ACK &&
					    !slot_snap->acked_target_mode) {
						if (slot_snap->pre_flip_view_w != got_w ||
						    slot_snap->pre_flip_view_h != got_h) {
							slot_snap->acked_target_mode = true;
						}
					}
				}
				if (l->data.type != XRT_LAYER_WINDOW_SPACE) {
					continue;
				}
				// Copy non-pointer layer data, then take strong refs on
				// the sc_array pointers (#234 follow-on, see comment
				// above the snapshot-clear loop). The raw `*l` would
				// dangle when IPC drops the swapchain ref unsynchronized.
				slot_snap->ws_snapshot[snap_n] = *l;
				for (size_t k = 0;
				     k < ARRAY_SIZE(slot_snap->ws_snapshot[snap_n].sc_array);
				     k++) {
					struct xrt_swapchain *raw =
					    slot_snap->ws_snapshot[snap_n].sc_array[k];
					slot_snap->ws_snapshot[snap_n].sc_array[k] = NULL;
					if (raw != NULL) {
						xrt_swapchain_reference(
						    &slot_snap->ws_snapshot[snap_n].sc_array[k],
						    raw);
					}
				}
				snap_n++;
			}
			slot_snap->ws_snapshot_count = snap_n;
			break;
		}
	}

	profile_s3 = os_monotonic_get_ns(); // Phase 5a — end of UI-layers block.

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
		// Phase 3 — per-client commits no longer drive multi_compositor_render.
		// `capture_render_thread_func` is the sole driver of the workspace
		// atlas render at ~70 Hz. Per-client tile-blit work above (lines
		// ~9700–10485) already wrote this client's tile into the shared
		// atlas; that work is GPU-async on `sys->context` and returns
		// immediately. The capture thread later samples the atlas and
		// composes; Phase 2's per-client `workspace_sync_fence` queues a
		// GPU `Wait()` ensuring the capture thread does not read a
		// half-written tile.
		//
		// Pre-Phase-3 the per-client render trigger added ~25 ms of
		// `sys->render_mutex` wait per `xrEndFrame` (4 clients all queued
		// on one mutex), capping per-cube cadence at ~12 fps with 4 cubes.
		// The 14 ms throttle that gated this path was aspirational
		// torn-atlas protection; real protection lives in the fence path.
		//
		// `client_skips` is still incremented so the bench harness can
		// observe per-client commit volume (no client_renders should fire
		// in workspace mode now).
		sys->render_diag_client_skips.fetch_add(1, std::memory_order_relaxed);

		// Phase 5a — emit per-stage [COMMIT_PROFILE_SVC] line. Same env
		// gate as [CLIENT_FRAME_NS]. Captures the four stages of service-
		// side commit work to attribute the per-cube xrEndFrame cost.
		// Sub-stages of "setup" attribute time to vendor-SDK polls
		// (`get_hardware_3d_state`, `get_predicted_eye_positions`) — both
		// suspect for blocking-on-vendor behavior.
		profile_s4 = os_monotonic_get_ns();
		if (profile_s3 == 0) profile_s3 = profile_s4; // No UI layers branch ran.
		{
			static int log_commit_profile = -1;
			if (log_commit_profile < 0) {
				const char *e = getenv("DISPLAYXR_LOG_PRESENT_NS");
				log_commit_profile = (e != nullptr && e[0] == '1') ? 1 : 0;
			}
			if (log_commit_profile) {
				U_LOG_W("[COMMIT_PROFILE_SVC] client=%p setup_pre_ns=%lld setup_3dstate_ns=%lld setup_eyepos_ns=%lld setup_post_ns=%lld proj_ns=%lld ui_ns=%lld post_ns=%lld total_ns=%lld",
				        (void *)c,
				        (long long)(profile_s0a - profile_s0),
				        (long long)(profile_s0b - profile_s0a),
				        (long long)(profile_s0c - profile_s0b),
				        (long long)(profile_s1 - profile_s0c),
				        (long long)(profile_s2 - profile_s1),
				        (long long)(profile_s3 - profile_s2),
				        (long long)(profile_s4 - profile_s3),
				        (long long)(profile_s4 - profile_s0));
			}
		}
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

		void *dp_fac_hs = comp_dp_factory_for_window(&sys->base.info, COMP_DP_PRIMARY_MONITOR, COMP_DP_API_D3D11);
		if (dp_fac_hs != NULL) {
			auto factory = (xrt_dp_factory_d3d11_fn_t)dp_fac_hs;
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

	// Post-weave chroma-key alpha conversion (no-op when chroma_key_color == 0).
	svc_chroma_key_pass_execute(sys, &c->render);

	// Phase 1 diagnostic — same env-gated [PRESENT_NS] used for the
	// workspace multi-comp swap chain. In standalone mode this fires
	// per client per frame against THIS client's own swap chain. Tagged
	// with the client struct pointer so workspace mode
	// (multi_compositor_render's Present, tagged client=workspace) and
	// standalone (here, tagged client=<client ptr>) can be told apart
	// by grep.
	{
		static int log_present_ns = -1;
		if (log_present_ns < 0) {
			const char *e = getenv("DISPLAYXR_LOG_PRESENT_NS");
			log_present_ns = (e != nullptr && e[0] == '1') ? 1 : 0;
		}
		if (log_present_ns) {
			static int64_t last_present_ns_standalone = 0;
			int64_t now_ns = os_monotonic_get_ns();
			if (last_present_ns_standalone != 0) {
				U_LOG_W("[PRESENT_NS] client=%p dt_ns=%lld",
				        (void *)c,
				        (long long)(now_ns - last_present_ns_standalone));
			}
			last_present_ns_standalone = now_ns;
		}
	}

	// Present to display
	if (c->render.swap_chain) {
		c->render.swap_chain->Present(1, 0);  // VSync
		// DComp path: publish the new frame to dwm.exe. Cheap — IPC of delta state,
		// no GPU work. Only present on the transparent opt-in path.
		if (c->render.dcomp_device) {
			c->render.dcomp_device->Commit();
		}
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

	// Unregister from multi-compositor AND tear down all per-client GPU
	// resources under sys->render_mutex (#234 follow-on). Previously only
	// the unregister was inside the lock; fini_client_render_resources and
	// fence cleanup happened outside, leaving a window where the render
	// thread (capture_render_thread_func acquires the same mutex) could
	// iterate mc->clients[] AFTER unregister null'd the slot but BEFORE
	// the per-client D3D11 resources were released — and a stale snapshot
	// of slot->compositor taken in the per-tile blit pass could deref a
	// just-freed COM interface (observed: vtable call on rcx=feeefeee at
	// multi_compositor_render +0x4781 when user clicks the close button).
	// Holding the render mutex through the full teardown serializes the
	// destroy with any in-progress render so the slot is guaranteed both
	// unregistered AND fully torn down before the render thread can next
	// observe it.
	//
	// Always unregister if there's a multi_comp — the client may have been
	// registered in workspace mode but is now closing in standalone mode
	// (after hot-switch). Without this, the slot stays stale and shows a
	// ghost remnant on workspace re-activate.
	if (sys->multi_comp != nullptr) {
		std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);
		multi_compositor_unregister_client(sys, c);

		// Clear active compositor if it's this one (still under render mutex
		// so the render thread can't snapshot a stale active_compositor).
		{
			std::lock_guard<std::mutex> aclock(sys->active_compositor_mutex);
			if (sys->active_compositor == c) {
				sys->active_compositor = nullptr;
			}
		}

		// Tear down per-client render resources (window, swap chain, DP,
		// atlas textures / SRVs / RTVs) while still holding render_mutex.
		fini_client_render_resources(&c->render);

		// workspace_sync_fence: drop the shared D3D11 fence. com_ptr release
		// drops the fence object; the shared NT handle was DuplicateHandle'd
		// into the client process by the IPC layer, so closing the source
		// handle here doesn't disturb the client's open fence.
		if (c->workspace_sync_fence_handle != nullptr) {
			CloseHandle(c->workspace_sync_fence_handle);
			c->workspace_sync_fence_handle = nullptr;
		}
		c->workspace_sync_fence.reset();
	} else {
		// No multi_comp: legacy single-client teardown, no render-thread
		// race to guard against.
		{
			std::lock_guard<std::mutex> aclock(sys->active_compositor_mutex);
			if (sys->active_compositor == c) {
				sys->active_compositor = nullptr;
			}
		}
		fini_client_render_resources(&c->render);
		if (c->workspace_sync_fence_handle != nullptr) {
			CloseHandle(c->workspace_sync_fence_handle);
			c->workspace_sync_fence_handle = nullptr;
		}
		c->workspace_sync_fence.reset();
	}

	delete c;
}


/*
 *
 * Phase 2 — workspace_sync_fence public surface (declared in
 * comp_d3d11_service.h). Defined here so the static `compositor_destroy`
 * function-pointer is in scope for the type-tag check.
 *
 */

extern "C" bool
comp_d3d11_service_compositor_export_workspace_sync_fence(struct xrt_compositor *xc,
                                                          xrt_graphics_sync_handle_t *out_handle)
{
	if (out_handle == nullptr) {
		return false;
	}
	*out_handle = XRT_GRAPHICS_SYNC_HANDLE_INVALID;
	if (xc == nullptr || xc->destroy != compositor_destroy) {
		return false;
	}
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);
	if (c->workspace_sync_fence_handle == nullptr) {
		return false;
	}
	*out_handle = (xrt_graphics_sync_handle_t)c->workspace_sync_fence_handle;
	return true;
}

extern "C" void
comp_d3d11_service_compositor_set_workspace_sync_fence_value(struct xrt_compositor *xc, uint64_t value)
{
	if (xc == nullptr || xc->destroy != compositor_destroy) {
		return;
	}
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);
	c->last_signaled_fence_value.store(value, std::memory_order_release);
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

	// #342 / ADR-020: re-derive dp_factory_* from the plug-in loader before
	// any per-client DP-factory read in this function (and downstream
	// `multi_compositor_ensure_output` calls). Picks up a vendor plug-in
	// registered after the service started without requiring a service
	// restart. NULL on builds without the loader hook.
	if (sys->base.info.refresh_display_processors != NULL) {
		sys->base.info.refresh_display_processors(&sys->base.info);
	}

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
	// to the service to dispatch workspace extensions but render
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
	bool transparent_hwnd = false;
	uint32_t chroma_key_color = 0;
	if (xsi != nullptr) {
		external_hwnd = xsi->external_window_handle;
		transparent_hwnd = xsi->transparent_background_enabled;
		chroma_key_color = xsi->chroma_key_color;
	}

	if (!is_headless_relay && !is_workspace_controller) {
		// Activate workspace mode from system compositor info (set by ipc_server_process.c
		// after init_all, before any client connects)
		if (sys->base.info.workspace_mode && !sys->workspace_mode) {
			service_set_workspace_mode(sys, true);
			U_LOG_W("Workspace mode activated for D3D11 service system");
		}

		xrt_result_t res_ret = init_client_render_resources(
		    sys, external_hwnd, transparent_hwnd, chroma_key_color, sys->xsysd, &c->render);
		if (res_ret != XRT_SUCCESS) {
			U_LOG_E("Failed to initialize client render resources");
			delete c;
			return res_ret;
		}

		// Phase 2: per-IPC-client workspace_sync_fence — replaces the
		// per-view CPU-side IDXGIKeyedMutex::AcquireSync wait with a GPU-side
		// ID3D11DeviceContext4::Wait. Created on the service device,
		// exported as a shared NT handle that the IPC layer DuplicateHandle's
		// into the client process. Failure leaves workspace_sync_fence null
		// and the legacy KeyedMutex path runs unchanged for this client
		// (preserves WebXR bridge / older _ipc app compatibility).
		c->workspace_sync_fence_handle = nullptr;
		c->last_signaled_fence_value.store(0, std::memory_order_relaxed);
		c->fence_window_start_ns = 0;
		c->fence_waits_queued_in_window = 0;
		c->fence_stale_views_in_window = 0;
		for (uint32_t v = 0; v < XRT_MAX_VIEWS; v++) {
			c->last_composed_fence_value[v] = 0;
		}
		{
			HRESULT hr_fence = sys->device->CreateFence(
			    0,
			    static_cast<D3D11_FENCE_FLAG>(D3D11_FENCE_FLAG_SHARED |
			                                  D3D11_FENCE_FLAG_SHARED_CROSS_ADAPTER),
			    IID_PPV_ARGS(c->workspace_sync_fence.put()));
			if (FAILED(hr_fence)) {
				U_LOG_W("Phase 2: CreateFence(_SHARED|_SHARED_CROSS_ADAPTER) failed "
				        "(hr=0x%08lX); retrying SHARED-only.",
				        (long)hr_fence);
				hr_fence = sys->device->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
				                                    IID_PPV_ARGS(c->workspace_sync_fence.put()));
			}
			if (SUCCEEDED(hr_fence) && c->workspace_sync_fence) {
				HANDLE fh = nullptr;
				HRESULT hr_h = c->workspace_sync_fence->CreateSharedHandle(
				    nullptr, GENERIC_ALL, nullptr, &fh);
				if (SUCCEEDED(hr_h) && fh != nullptr) {
					c->workspace_sync_fence_handle = fh;
					U_LOG_W("[FENCE] client=%p workspace_sync_fence created "
					        "(handle=%p)",
					        (void *)c, fh);
				} else {
					U_LOG_W("Phase 2: CreateSharedHandle on workspace_sync_fence "
					        "failed (hr=0x%08lX); legacy KeyedMutex path stays "
					        "in effect for this client.",
					        (long)hr_h);
					c->workspace_sync_fence.reset();
				}
			} else {
				U_LOG_W("Phase 2: CreateFence failed (hr=0x%08lX); legacy "
				        "KeyedMutex path stays in effect for this client.",
				        (long)hr_fence);
				c->workspace_sync_fence.reset();
			}
		}
	}

	// Register with multi-compositor in workspace mode. Skip bridge-relay
	// AND workspace-controller sessions — neither has anything to render,
	// and a phantom slot would (a) keep mc->client_count > 0 after the
	// session ends, suppressing the empty-workspace hint, and (b) for the controller
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
	c->base.base.layer_window_space = compositor_layer_window_space;
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

/*!
 * Hook for oxr_xrRequestDisplayRenderingModeEXT when called by a workspace
 * controller session (#234). Routes through the acked-flip + curtain path
 * on the multi-compositor; the OXR layer then SKIPS the legacy immediate
 * device-mode-update path, avoiding the raw-atlas glitch that's otherwise
 * visible whenever the controller drives a mode change (shell V-toggle,
 * focus-adaptive 2D, etc.).
 *
 * Returns false if there's no multi_comp (caller falls back to legacy path).
 */
static bool
system_request_workspace_mode_flip(struct xrt_system_compositor *xsysc, uint32_t mode_index)
{
	return comp_d3d11_service_workspace_request_mode_flip(xsysc, mode_index);
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

#ifdef _WIN32
	// spec_version 8: close the workspace wakeup event handle. Controllers
	// hold their own DuplicateHandle ref; closing this one doesn't disturb
	// them. They'll close their copy on shell exit.
	if (sys->workspace_wakeup_event != nullptr) {
		CloseHandle((HANDLE)sys->workspace_wakeup_event);
		sys->workspace_wakeup_event = nullptr;
	}
#endif

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

	mcp_capture_uninstall();
	mcp_capture_fini(&sys->mcp_capture);

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
	if (comp_dp_factory_for_window(&sys->base.info, COMP_DP_PRIMARY_MONITOR, COMP_DP_API_D3D11) != NULL) {
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
	sys->base.request_workspace_mode_flip = system_request_workspace_mode_flip;

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

	// Set supported blend modes. Chrome WebXR requires at least OPAQUE.
	// ALPHA_BLEND is also advertised because the service compositor honours
	// XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT per projection
	// layer (see the per-tile blend-mode selection guarded by ws_snapshot
	// elsewhere in this file), so workspace apps can opt into transparency
	// without any extension chain.
	sys->base.info.supported_blend_modes[0] = XRT_BLEND_MODE_OPAQUE;
	sys->base.info.supported_blend_modes[1] = XRT_BLEND_MODE_ALPHA_BLEND;
	sys->base.info.supported_blend_mode_count = 2;

	// Populate display info for XR_EXT_display_info
	// (display_width_m and display_height_m are already set above from the temporary display processor query)
	if (sys->output_width > 0 && sys->output_height > 0) {
		sys->base.info.recommended_view_scale_x = (float)sys->view_width / (float)sys->output_width;
		sys->base.info.recommended_view_scale_y = (float)sys->view_height / (float)sys->output_height;
		sys->base.info.display_pixel_width = sys->output_width;
		sys->base.info.display_pixel_height = sys->output_height;
	}

	// These dims are the pre-DP placeholder defaults (set at struct init,
	// search "output_width = 1920"). The real panel resolution is reported
	// later by the active display processor — see the "XR_EXT_display_info"
	// line (display_pixel_*) and the combined-atlas size, which are the
	// authoritative values. Labelled here so this early line isn't mistaken
	// for the actual display resolution.
	U_LOG_W("D3D11 service system compositor created: view_count=%u, "
	        "default/placeholder dims (pending display-processor query): "
	        "view=%ux%u display=%ux%u output=%ux%u @ %.0fHz",
	        view_count,
	        sys->view_width, sys->view_height,
	        sys->display_width, sys->display_height,
	        sys->output_width, sys->output_height, sys->refresh_rate);

	mcp_capture_init(&sys->mcp_capture);
	mcp_capture_install(&sys->mcp_capture);

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

	// #363: hold render_mutex across the DP pointer read AND the call into the
	// DP. compositor_destroy holds render_mutex through its entire teardown
	// (unregister -> null active_compositor -> fini_client_render_resources,
	// which frees render.display_processor). Without this lock an IPC client
	// thread could latch `dp` here, the teardown thread frees it, and the call
	// below derefs freed memory — the use-after-free seen on last-client
	// (total->0) churn. render_mutex is recursive, so callers that already
	// hold it (multi_compositor_render, capture_frame) are unaffected.
	std::lock_guard<std::recursive_mutex> render_lock(sys->render_mutex);

	// Caching now lives inside the DP itself (vendor-internal, populated
	// by SR's EyePairStream listener). This is just a thin pass-through.
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
				U_LOG_D("IPC eye positions (from display processor): L=(%.3f,%.3f,%.3f) R=(%.3f,%.3f,%.3f)",
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

	// #363: hold render_mutex across the DP pointer read AND the call into the
	// DP, to serialize with compositor_destroy's full teardown (which frees
	// render.display_processor). Same read-then-use-after-unlock UAF as
	// get_predicted_eye_positions. render_mutex is recursive.
	std::lock_guard<std::recursive_mutex> render_lock(sys->render_mutex);

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

	// #363: hold render_mutex across the DP pointer read AND every call into
	// the DP below, to serialize with compositor_destroy's full teardown
	// (which frees render.display_processor). Same read-then-use-after-unlock
	// UAF as get_predicted_eye_positions. render_mutex is recursive.
	std::lock_guard<std::recursive_mutex> render_lock(sys->render_mutex);

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
	// ADR-018 (#304): first set_pose marks the client placed; the runtime
	// starts compositing it from here (gated with first-frame-committed).
	mc->clients[slot].placed = true;

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

	// Refresh the WindowProc's forwarding rect so the new pose's pixel
	// rect is used to decide which clicks reach the app. Without this
	// the forwarding rect goes stale at whatever the rect was at first
	// register_client (or last layout-preset animation tick) — clicks
	// on the now-larger / moved tile fall outside the stale rect and
	// are silently dropped. Only matters for the focused slot, but
	// recompute always since the call is cheap and the result is
	// idempotent for non-focused slots.
	if (slot == mc->focused_slot) {
		multi_compositor_update_input_forward(mc);
	}

	return true;
}

bool
comp_d3d11_service_set_client_frame_rate_cap(struct xrt_system_compositor *xsysc,
                                              struct xrt_compositor *xc,
                                              float max_fps)
{
	if (xsysc == nullptr || xc == nullptr) return false;
	if (max_fps < 0.0f) return false;

	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	if (!sys->workspace_mode || sys->multi_comp == nullptr) return false;

	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);
	struct d3d11_multi_compositor *mc = sys->multi_comp;

	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);

	int slot = multi_comp_find_slot(mc, c);
	if (slot < 0) return false;

	float prev = mc->clients[slot].frame_rate_cap_hz;
	mc->clients[slot].frame_rate_cap_hz = max_fps;
	if (prev != max_fps) {
		U_LOG_W("Workspace: set_frame_rate_cap slot %d %.1f → %.1f Hz", slot, prev, max_fps);
	}
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

	// #307 slice B: `minimized` is now purely the composite gate. The controller
	// owns the distinction between user-minimize and maximize-hide (it renders
	// its own taskbar on the overlay swapchain and tracks which windows it hid),
	// so the runtime no longer needs a separate controller_hidden flag.
	mc->clients[slot].minimized = !visible;
	U_LOG_W("Workspace: set_visibility slot %d visible=%d", slot, visible);

	if (!visible && slot == mc->focused_slot) {
		// ADR-018: hiding the focused client clears focus for forwarding
		// safety; the controller picks the successor (shell auto-focus-next).
		mc->focused_slot = -1;
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
			// spec_version 22: the runtime no longer raycasts. The controller
			// owns the eye→cursor hit-test and fills hit_client_id / hit_region /
			// local_u/v / chrome_region_id itself from cursor_x/y (left zeroed by
			// the memset above).
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
			// enabled. spec_version 22: the runtime no longer raycasts; the
			// controller fills the hit fields itself from cursor_x/y.
			ev->u.pointer_motion.button_mask = r->button_or_vk;
			ev->u.pointer_motion.modifiers = r->modifiers;
			ev->u.pointer_motion.cursor_x = (int64_t)r->cursor_x;
			ev->u.pointer_motion.cursor_y = (int64_t)r->cursor_y;
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

	// POINTER_HOVER was removed in spec_version 22. The runtime no longer
	// raycasts, so it cannot detect hovered-slot / region transitions. The
	// workspace controller now generates its own hover transitions from the
	// per-frame cursor feed (POINTER_MOTION + its own hit-test) and drives
	// chrome fade / cursor-shape switching itself.

	// spec_version 8: WINDOW_POSE_CHANGED for any slot whose stored pose /
	// dims have drifted since the last drain. Catches runtime-driven changes
	// (edge resize, fullscreen toggle, etc.) so controllers can re-push
	// chrome layout instead of leaving the pill stranded at the old size.
	// Shell-driven set_pose RPCs also flow through this path — controllers
	// that initiated them should dedupe (the new pose === what they pushed)
	// or just re-push idempotently (push_layout is cheap).
	for (int s = 0; s < D3D11_MULTI_MAX_CLIENTS &&
	     out_batch->count < IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX; s++) {
		struct d3d11_multi_client_slot *cs = &mc->clients[s];
		if (!cs->active || cs->client_type == CLIENT_TYPE_CAPTURE) continue;
		if (cs->chrome_xsc == nullptr) continue; // #304: has-chrome gate (was workspace_client_id==0 proxy)

		const struct xrt_pose &p = cs->window_pose;
		const struct xrt_pose &q = cs->window_pose_last_emitted;
		const float kEps = 1e-5f;
		bool pose_changed =
		    fabsf(p.position.x - q.position.x) > kEps ||
		    fabsf(p.position.y - q.position.y) > kEps ||
		    fabsf(p.position.z - q.position.z) > kEps ||
		    fabsf(p.orientation.x - q.orientation.x) > kEps ||
		    fabsf(p.orientation.y - q.orientation.y) > kEps ||
		    fabsf(p.orientation.z - q.orientation.z) > kEps ||
		    fabsf(p.orientation.w - q.orientation.w) > kEps;
		bool size_changed =
		    fabsf(cs->window_width_m - cs->window_w_last_emitted) > kEps ||
		    fabsf(cs->window_height_m - cs->window_h_last_emitted) > kEps;
		if (!pose_changed && !size_changed) continue;

		struct ipc_workspace_input_event *ev = &out_batch->events[out_batch->count];
		memset(ev, 0, sizeof(*ev));
		ev->event_type = IPC_WORKSPACE_INPUT_EVENT_WINDOW_POSE_CHANGED;
		ev->timestamp_ms = (uint32_t)GetTickCount();
		ev->u.window_pose_changed.client_id    = cs->workspace_client_id;
		ev->u.window_pose_changed.pose_orient_x = p.orientation.x;
		ev->u.window_pose_changed.pose_orient_y = p.orientation.y;
		ev->u.window_pose_changed.pose_orient_z = p.orientation.z;
		ev->u.window_pose_changed.pose_orient_w = p.orientation.w;
		ev->u.window_pose_changed.pose_pos_x   = p.position.x;
		ev->u.window_pose_changed.pose_pos_y   = p.position.y;
		ev->u.window_pose_changed.pose_pos_z   = p.position.z;
		ev->u.window_pose_changed.width_m      = cs->window_width_m;
		ev->u.window_pose_changed.height_m     = cs->window_height_m;
		out_batch->count++;

		cs->window_pose_last_emitted = p;
		cs->window_w_last_emitted = cs->window_width_m;
		cs->window_h_last_emitted = cs->window_height_m;
	}

	// spec_version 10: MODAL_OPEN / MODAL_CLOSE on per-slot transition. Set
	// by ipc_handle_session_set_modal_state when an in-app CBT hook detects
	// a Win32 modal popup (file dialog, MessageBox, etc.) being created or
	// destroyed. Same per-slot delta-vs-shadow pattern as FOCUS_CHANGED and
	// WINDOW_POSE_CHANGED. Filtered to slots with workspace_client_id != 0
	// because controllers without chrome bound to the slot have no UI to
	// dim — the runtime side (frame-starvation timeout extension) still
	// reads modal_open regardless.
	for (int s = 0; s < D3D11_MULTI_MAX_CLIENTS &&
	     out_batch->count < IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX; s++) {
		struct d3d11_multi_client_slot *cs = &mc->clients[s];
		if (!cs->active || cs->client_type == CLIENT_TYPE_CAPTURE) continue;
		if (cs->chrome_xsc == nullptr) continue; // #304: has-chrome gate (was workspace_client_id==0 proxy)
		if (cs->modal_open == cs->modal_open_last_emitted) continue;

		struct ipc_workspace_input_event *ev = &out_batch->events[out_batch->count];
		memset(ev, 0, sizeof(*ev));
		ev->event_type = cs->modal_open ? IPC_WORKSPACE_INPUT_EVENT_MODAL_OPEN
		                                : IPC_WORKSPACE_INPUT_EVENT_MODAL_CLOSE;
		ev->timestamp_ms = (uint32_t)GetTickCount();
		ev->u.modal.client_id = cs->workspace_client_id;
		out_batch->count++;

		cs->modal_open_last_emitted = cs->modal_open;

		// Item 4 (#232): on MODAL_CLOSE, restore enough state that the
		// next user input reaches the app's dialog activation path.
		//
		//   (a) Workspace-internal focus → this slot, so the runtime's
		//       click-forward + chrome-emission paths point at the right
		//       app HWND for subsequent input.
		//   (b) Grant the app process foreground-set rights via
		//       AllowSetForegroundWindow. The app's HWND is hidden in
		//       workspace mode (SW_HIDE in oxr_session.c), so calling
		//       SetForegroundWindow on it directly silently fails on a
		//       hidden window AND can leave Windows' foreground lock in
		//       a state that DENIES the next dialog activation from the
		//       app — producing "subsequent dialogs open invisible". The
		//       safer move: don't try to surface the hidden HWND; grant
		//       the app process the right to activate its own dialog
		//       windows on its next user-input edge. The grant is one-
		//       shot per call and the runtime is the foreground process
		//       at this point, so the grant is what Windows actually
		//       needs to let the next dialog claim foreground.
		if (!cs->modal_open && cs->app_hwnd != NULL) {
			mc->focused_slot = s;
			multi_compositor_update_input_forward(mc);
			DWORD app_pid = 0;
			GetWindowThreadProcessId(cs->app_hwnd, &app_pid);
			if (app_pid != 0) {
				(void)AllowSetForegroundWindow(app_pid);
			}
		}
	}

	// #307: FULLSCREEN_TOGGLED is no longer emitted — the maximize state machine
	// moved to the controller (ADR-018), which drives maximize itself and focuses
	// the client directly. The IPC/XR enum constant is retained for ABI stability.

	// spec_version 16 (#304): CLIENT_CONNECTED, one-shot per slot set at
	// register (slot-bind). The runtime no longer owns per-client policy —
	// the controller responds with placement (xrSetWorkspaceClientWindowPoseEXT)
	// and, per its design, chrome / style / focus. client_id is the unified
	// "1000 + slot" workspace id every event + API uses. Includes captures.
	for (int s = 0; s < D3D11_MULTI_MAX_CLIENTS &&
	     out_batch->count < IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX; s++) {
		struct d3d11_multi_client_slot *cs = &mc->clients[s];
		if (!cs->active || !cs->announce_connected) continue;

		struct ipc_workspace_input_event *ev = &out_batch->events[out_batch->count];
		memset(ev, 0, sizeof(*ev));
		ev->event_type = IPC_WORKSPACE_INPUT_EVENT_CLIENT_CONNECTED;
		ev->timestamp_ms = (uint32_t)GetTickCount();
		ev->u.client_connected.client_id = 1000u + (uint32_t)s;
		out_batch->count++;

		cs->announce_connected = false;
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
			// spec_version 20: live viewer eye-midpoint for billboarding.
			ev->u.frame_tick.viewer_x = mc->frame_tick_viewer_x;
			ev->u.frame_tick.viewer_y = mc->frame_tick_viewer_y;
			ev->u.frame_tick.viewer_z = mc->frame_tick_viewer_z;
			ev->u.frame_tick.viewer_valid = (uint32_t)mc->frame_tick_viewer_valid;
			// spec_version 22: OS cursor position (workspace-window client px,
			// top-left). The controller runs its own hit-test over this each
			// frame to generate hover transitions + push cursor depth, now that
			// the runtime no longer raycasts.
			ev->u.frame_tick.cursor_x = mc->cursor_panel_x;
			ev->u.frame_tick.cursor_y = mc->cursor_panel_y;
			out_batch->count++;
			missed--;
		}
		mc->frame_tick_last_emitted = cur;
		mc->frame_tick_last_ns = now_ns;
	}

	// XR_EXT_workspace_file_dialog: drain pending picker requests one at a
	// time. Payload is tiny (client_id + request_id) so it fits comfortably
	// in IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX; the controller fetches the
	// full XrFilePickerInfoEXT-equivalent via
	// workspace_get_file_picker_request once it sees the event.
	for (size_t i = 0; i < sizeof(mc->file_picker) / sizeof(mc->file_picker[0]) &&
	     out_batch->count < IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX; i++) {
		if (!mc->file_picker[i].in_use || !mc->file_picker[i].needs_emit) continue;
		int s = mc->file_picker[i].owner_slot;
		if (s < 0 || s >= D3D11_MULTI_MAX_CLIENTS || !mc->clients[s].active) {
			// Slot disappeared between post and drain — drop the
			// pending entry; a late workspace_file_dialog_result
			// will hit the same active-slot check and be discarded.
			mc->file_picker[i].in_use = false;
			mc->file_picker[i].needs_emit = false;
			continue;
		}
		struct ipc_workspace_input_event *ev = &out_batch->events[out_batch->count];
		memset(ev, 0, sizeof(*ev));
		ev->event_type = IPC_WORKSPACE_INPUT_EVENT_FILE_PICKER_REQUEST;
		ev->timestamp_ms = (uint32_t)GetTickCount();
		ev->u.file_picker_request.client_id = mc->clients[s].workspace_client_id;
		ev->u.file_picker_request.request_id = mc->file_picker[i].request_id;
		out_batch->count++;
		mc->file_picker[i].needs_emit = false;
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

	// #307: maximize is owned by the workspace controller now (ADR-018), the
	// same way display-mode is workspace-owned. xrRequestWorkspaceClientFullscreenEXT
	// is a no-op for workspace clients — an app cannot self-maximize; the
	// controller decides (MAX button / F11 / double-click) and drives it via
	// set_pose + set_visibility. Kept as a successful no-op to preserve the API.
	(void)fullscreen;
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

extern "C" bool
comp_d3d11_service_workspace_get_client_state(struct xrt_system_compositor *xsysc,
                                              struct xrt_compositor *xc,
                                              bool *out_visible,
                                              bool *out_focused)
{
	if (xsysc == nullptr || xc == nullptr) {
		return false;
	}
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (mc == nullptr) {
		return false;
	}
	struct d3d11_service_compositor *c = d3d11_service_compositor_from_xrt(xc);
	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);
	int slot = multi_comp_find_slot(mc, c);
	if (slot < 0) {
		return false;
	}
	// WORKSPACE visibility = not minimized. Distinct from IPC session_visible.
	if (out_visible != nullptr) {
		*out_visible = !mc->clients[slot].minimized;
	}
	if (out_focused != nullptr) {
		*out_focused = (slot == mc->focused_slot);
	}
	return true;
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
                                                               uint32_t client_id,
                                                               uint32_t swapchain_id,
                                                               struct xrt_swapchain *chrome_xsc)
{
	if (xsysc == nullptr || chrome_xsc == nullptr) {
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
	// #304 id-unification: workspace_client_id is the permanent slot id set at
	// register; chrome create/destroy no longer owns it. (client_id passed in
	// equals 1000+slot post-unification — kept only for the log below.)

	U_LOG_W("workspace: chrome swapchain registered for slot %d (client_id=%u, sc_id=%u)",
	        slot, client_id, swapchain_id);
	return XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_d3d11_service_workspace_unregister_chrome_swapchain(struct xrt_system_compositor *xsysc,
                                                         uint32_t swapchain_id)
{
	if (xsysc == nullptr) {
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
		// Match on chrome_xsc != null too — chrome_swapchain_id 0 is a
		// valid IPC id (first slot in xscs[]), so unregistered slots
		// would spuriously match swapchain_id=0 without this guard.
		if (cs->chrome_xsc != nullptr && cs->chrome_swapchain_id == swapchain_id) {
			xrt_swapchain_reference(&cs->chrome_xsc, NULL);
			cs->chrome_swapchain_id = 0;
			cs->chrome_layout_valid = false;
			// #304: keep workspace_client_id (permanent slot id); only chrome
			// state is torn down here.
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
	cs->chrome_anchor_top_edge = (layout->anchor_to_window_top_edge != 0);
	cs->chrome_width_fraction = layout->width_as_fraction_of_window;
	cs->chrome_region_count = layout->hit_region_count;
	if (cs->chrome_region_count > IPC_WORKSPACE_CHROME_MAX_HIT_REGIONS) {
		cs->chrome_region_count = IPC_WORKSPACE_CHROME_MAX_HIT_REGIONS;
	}
	memcpy(cs->chrome_regions, layout->hit_regions,
	       cs->chrome_region_count * sizeof(struct ipc_workspace_chrome_hit_region));
	cs->chrome_layout_valid = true;
	return XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_d3d11_service_workspace_update_chrome_layer_pose_by_slot(struct xrt_system_compositor *xsysc,
                                                              int slot,
                                                              const struct xrt_pose *pose_in_client)
{
	if (xsysc == nullptr || pose_in_client == nullptr) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (mc == nullptr || slot < 0 || slot >= D3D11_MULTI_MAX_CLIENTS) {
		return XRT_ERROR_IPC_FAILURE;
	}

	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);
	mc->clients[slot].chrome_pose_in_client = *pose_in_client;
	// Note: chrome_layout_valid is NOT toggled here. A pose-only update on a
	// slot whose layout hasn't been pushed yet just stages the pose for the
	// controller's eventual set_chrome_layout_by_slot to inherit. If a
	// layout has already been pushed (the common case for cursor + animated
	// chrome), the flag is already true and stays that way.
	return XRT_SUCCESS;
}

// spec_version 13: store the controller-pushed cursor source. xsc may be
// NULL (caller passed XR_NULL_HANDLE / invalid swapchain) — that hides the
// cursor without tearing anything down. The runtime samples this swapchain
// in its per-tile cursor render pass.
extern "C" xrt_result_t
comp_d3d11_service_workspace_set_cursor(struct xrt_system_compositor *xsysc,
                                         struct xrt_swapchain *xsc,
                                         float hot_x, float hot_y,
                                         float size_meters,
                                         bool visible)
{
	if (xsysc == nullptr) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);
	sys->cursor_xsc = xsc; // borrowed; controller owns lifetime
	sys->cursor_hot_x = hot_x;
	sys->cursor_hot_y = hot_y;
	sys->cursor_size_m = size_meters > 0.0f ? size_meters : 0.012f;
	sys->cursor_visible = visible;
	return XRT_SUCCESS;
}

// spec_version 17: store the controller-pushed overlay source. xsc may be NULL
// (caller passed XR_NULL_HANDLE / invalid swapchain) — that hides the overlay
// without tearing anything down. The runtime composites this swapchain at z = 0
// in its per-tile overlay render pass (docked per anchor/pivot, no disparity).
extern "C" xrt_result_t
comp_d3d11_service_workspace_set_overlay(struct xrt_system_compositor *xsysc,
                                          uint32_t overlay_id,
                                          struct xrt_swapchain *xsc,
                                          float anchor_x, float anchor_y,
                                          float pivot_x, float pivot_y,
                                          float size_w_m, float size_h_m,
                                          bool visible, bool stereo_sbs)
{
	if (xsysc == nullptr) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);

	// spec_version 21: !visible OR NULL swapchain = remove this id; other ids
	// are untouched. Hides the overlay without tearing anything down.
	if (!visible || xsc == nullptr) {
		sys->overlays.erase(overlay_id);
		return XRT_SUCCESS;
	}

	// Reject a brand-new id beyond the cap (updates to existing ids always
	// proceed). Bounds a buggy controller without dropping live overlays.
	if (sys->overlays.find(overlay_id) == sys->overlays.end() &&
	    sys->overlays.size() >= D3D11_SERVICE_MAX_OVERLAYS) {
		U_LOG_W("workspace overlay map full (%u); ignoring new overlay id %u",
		        (unsigned)D3D11_SERVICE_MAX_OVERLAYS, overlay_id);
		return XRT_SUCCESS;
	}

	overlay_slot &slot = sys->overlays[overlay_id];
	slot.xsc = xsc; // borrowed; controller owns lifetime
	slot.stereo_sbs = stereo_sbs;
	slot.anchor_x = anchor_x;
	slot.anchor_y = anchor_y;
	slot.pivot_x = pivot_x;
	slot.pivot_y = pivot_y;
	slot.size_w_m = size_w_m > 0.0f ? size_w_m : 0.10f;
	slot.size_h_m = size_h_m > 0.0f ? size_h_m : 0.02f;
	return XRT_SUCCESS;
}

// Phase 2.C spec_version 9: shared body for both IPC and capture client style
// pushes. Copies fields; does NOT validate (state tracker already validated).
static void
apply_workspace_style_to_slot(struct d3d11_multi_client_slot *cs,
                              const struct ipc_workspace_client_style *style)
{
	cs->style_pushed = true;
	cs->style_corner_radius = style->corner_radius;
	cs->style_edge_feather_meters = style->edge_feather_meters;
	cs->style_focus_glow_color[0] = style->focus_glow_color[0];
	cs->style_focus_glow_color[1] = style->focus_glow_color[1];
	cs->style_focus_glow_color[2] = style->focus_glow_color[2];
	cs->style_focus_glow_color[3] = style->focus_glow_color[3];
	cs->style_focus_glow_intensity = style->focus_glow_intensity;
	cs->style_focus_glow_falloff_meters = style->focus_glow_falloff_meters;
}

extern "C" bool
comp_d3d11_service_set_client_style_by_slot(struct xrt_system_compositor *xsysc,
                                            int slot,
                                            const struct ipc_workspace_client_style *style)
{
	if (xsysc == nullptr || style == nullptr) {
		return false;
	}
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (mc == nullptr || slot < 0 || slot >= D3D11_MULTI_MAX_CLIENTS) {
		return false;
	}
	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);
	apply_workspace_style_to_slot(&mc->clients[slot], style);
	return true;
}

extern "C" bool
comp_d3d11_service_set_capture_client_style(struct xrt_system_compositor *xsysc,
                                            int slot_index,
                                            const struct ipc_workspace_client_style *style)
{
	// Capture clients live in the same mc->clients[] array as IPC clients
	// (they distinguish via client_type). The IPC handler maps client_id
	// >= 1000 to slot = client_id - 1000 directly — that lands at the
	// same slot index used here.
	if (xsysc == nullptr || style == nullptr) {
		return false;
	}
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (mc == nullptr || slot_index < 0 || slot_index >= D3D11_MULTI_MAX_CLIENTS) {
		return false;
	}
	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);
	apply_workspace_style_to_slot(&mc->clients[slot_index], style);
	return true;
}

// One-click-drag helper (#370 Phase 2). When the controller moves focus to a
// previously-unfocused window while the primary mouse button is held, the
// original WM_LBUTTONDOWN was forwarded to the OLD focus (or nowhere) by the
// WndProc — so the newly focused app never saw the DOWN and a click+drag can't
// start in one motion. The runtime owns the app HWNDs, so it re-delivers that
// DOWN here. This is input-forwarding plumbing (translating a spatial click to
// a Win32 click on the target HWND), not focus/click POLICY — the controller
// already decided the focus. No raycast: we map the current OS cursor through
// the focused slot's content rect, exactly as the WndProc maps MOVE/UP.
static void
synthesize_focus_click_down(struct d3d11_multi_compositor *mc, int slot)
{
	if (slot < 0 || slot >= D3D11_MULTI_MAX_CLIENTS) {
		return;
	}
	struct d3d11_multi_client_slot *cs = &mc->clients[slot];
	HWND target = cs->app_hwnd;
	// Modal gate (ADR-017, #232): never forward a synthetic click to a slot
	// whose Win32 modal popup is up — it could shift focus out of the modal chain.
	if (target == nullptr || !IsWindow(target) || cs->modal_open) {
		return;
	}
	// Only when the primary button is actually held (a drag-in-progress). RMB
	// focus changes (rotation) must not inject an LBUTTONDOWN.
	if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
		return;
	}
	// If the WndProc already delivered the DOWN to this HWND, don't double it.
	if ((HWND)comp_d3d11_window_get_last_pointer_down_target(mc->window) == target) {
		return;
	}

	POINT pt = {0, 0};
	GetCursorPos(&pt);
	ScreenToClient(mc->hwnd, &pt);

	int32_t rx = cs->window_rect_x;
	int32_t ry = cs->window_rect_y;
	int32_t rw = cs->window_rect_w;
	int32_t rh = cs->window_rect_h;
	if (rw <= 0 || rh <= 0 || pt.x < rx || pt.x >= rx + rw || pt.y < ry || pt.y >= ry + rh) {
		return; // cursor not over this slot's content rect
	}

	// Map workspace-window pixels → app-client pixels (scale if the app HWND
	// client size differs from the rendered rect), matching the WndProc.
	RECT cr;
	GetClientRect(target, &cr);
	int tw = cr.right - cr.left;
	int th = cr.bottom - cr.top;
	int rel_x = pt.x - rx;
	int rel_y = pt.y - ry;
	int app_x, app_y;
	if (tw > 0 && th > 0 && (tw != rw || th != rh)) {
		app_x = (int)((float)rel_x * (float)tw / (float)rw);
		app_y = (int)((float)rel_y * (float)th / (float)rh);
	} else {
		app_x = rel_x;
		app_y = rel_y;
	}

	// Capture clients (adopted 2D windows): route to the child control under
	// the point so the DOWN lands on the right widget.
	HWND click_target = target;
	if (cs->client_type == CLIENT_TYPE_CAPTURE) {
		POINT child_pt = {app_x, app_y};
		HWND child = ChildWindowFromPointEx(click_target, child_pt,
		                                    CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
		if (child != nullptr && child != click_target) {
			MapWindowPoints(click_target, child, &child_pt, 1);
			click_target = child;
			app_x = child_pt.x;
			app_y = child_pt.y;
		}
	}

	// Seed a MOUSEMOVE first so the app's stored last-known cursor matches the
	// click position (apps computing drag deltas from last-move avoid a jump).
	LPARAM lp = MAKELPARAM(app_x, app_y);
	PostMessage(click_target, WM_MOUSEMOVE, 0, lp);
	PostMessage(click_target, WM_LBUTTONDOWN, MK_LBUTTON, lp);
}

extern "C" void
comp_d3d11_service_set_focused_slot(struct xrt_system_compositor *xsysc, int slot)
{
	// Phase 2.C spec_version 9: explicit setter so the IPC layer's
	// xrSetWorkspaceFocusedClientEXT path can update the compositor's
	// focused_slot (used by the per-client focus-glow gate at blit
	// time). Validates range — out-of-range slots clamp to -1 (no
	// focus). Holding render_mutex matches the existing register /
	// unregister focus-update sites.
	if (xsysc == nullptr) {
		return;
	}
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (mc == nullptr) {
		return;
	}
	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);
	int prev_focused = mc->focused_slot;
	if (slot < 0 || slot >= D3D11_MULTI_MAX_CLIENTS || !mc->clients[slot].active) {
		mc->focused_slot = -1;
	} else {
		mc->focused_slot = slot;
	}

	// Re-evaluate the WindowProc forwarding target so the freshly-
	// focused slot's HWND becomes the new fwd HWND. Without this the
	// fwd HWND stays at whatever the prior focused slot was (often a
	// closed picker / dialog → a now-dead HWND that PostMessage
	// silently swallows), so the user has to trigger another route
	// (Ctrl+1 layout preset, mouse drag, etc.) before clicks reach
	// the new focused app. Surfaced by #228 spatial picker dismissal
	// returning focus to the requester (gauss demo) — without this
	// refresh the gauss received no mouse input until a layout
	// preset reset the fwd state.
	multi_compositor_update_input_forward(mc);

	// #370 Phase 2: if focus just moved to a new window while LMB is held,
	// re-deliver the lost WM_LBUTTONDOWN so a click-to-focus drag starts in one
	// motion (the WndProc forwarded the original DOWN to the prior focus).
	if (mc->focused_slot >= 0 && mc->focused_slot != prev_focused) {
		synthesize_focus_click_down(mc, mc->focused_slot);
	}
}

extern "C" void
comp_d3d11_service_set_client_modal_state(struct xrt_system_compositor *xsysc, int slot, bool is_open)
{
	// spec_version 10: simple bool toggle on the slot. The drain loop
	// below picks up the transition vs modal_open_last_emitted and emits
	// MODAL_OPEN / MODAL_CLOSE events to the workspace controller; the
	// frame-starvation timeout extension (comp_d3d11_service_render path)
	// reads the same field so the compositor keeps presenting the last-
	// good frame while the user interacts with the dialog.
	if (xsysc == nullptr) {
		return;
	}
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (mc == nullptr) {
		return;
	}
	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);
	if (slot < 0 || slot >= D3D11_MULTI_MAX_CLIENTS || !mc->clients[slot].active) {
		return;
	}
	mc->clients[slot].modal_open = is_open;

	// Modal-driven 2D-flip (#234, ADR-017 §"Required response" item 2):
	// when the FOCUSED slot opens a modal, present a flat workspace so the
	// OS-native dialog reads cleanly. Routes through the acked-flip path so
	// the catch-up artifact is masked by the curtain. On modal-close,
	// restore the pre-modal mode unless the user has manually changed mode
	// in the meantime (defensive: don't yank a user-initiated 3D toggle).
	// Non-focused-slot modals do not drive mode.
	if (slot == mc->focused_slot && sys->xsysd != NULL) {
		struct xrt_device *head = sys->xsysd->static_roles.head;
		if (head != nullptr && head->hmd != NULL && head->rendering_mode_count > 0) {
			uint32_t cur_idx = head->hmd->active_rendering_mode_index;
			// Resolve the 2D mode index (first mode with hardware_display_3d == false).
			uint32_t idx_2d = UINT32_MAX;
			for (uint32_t i = 0; i < head->rendering_mode_count; i++) {
				if (!head->rendering_modes[i].hardware_display_3d) {
					idx_2d = i;
					break;
				}
			}
			if (idx_2d != UINT32_MAX) {
				if (is_open) {
					// Only save the pre-modal mode when we actually transition
					// FROM a 3D mode to 2D. If we're already in 2D (e.g., a
					// second modal opens while the first is still up, or focus-
					// adaptive already put us in 2D), preserve the existing
					// saved_mode_index so the eventual modal-close still
					// restores the original 3D mode.
					if (cur_idx != idx_2d) {
						mc->mode_flip.saved_mode_index = cur_idx;
						multi_compositor_request_mode_flip(sys, idx_2d, slot);
						U_LOG_W("[mode_flip] modal-open on focused slot %d: %u -> 2D (%u)",
						        slot, cur_idx, idx_2d);
					}
				} else {
					// Only restore if we're still in 2D (presumed to be the
					// mode we forced on MODAL_OPEN); otherwise leave alone.
					uint32_t want = mc->mode_flip.saved_mode_index;
					if (cur_idx == idx_2d && want != idx_2d &&
					    want < head->rendering_mode_count) {
						multi_compositor_request_mode_flip(sys, want, slot);
						U_LOG_W("[mode_flip] modal-close on focused slot %d: 2D -> %u (restore)",
						        slot, want);
					}
				}
			}
		}
	}

	// Item 4 (#232) cont'd: when transitioning to modal-open, grant the
	// app process foreground-activation rights so its in-flight dialog
	// CreateWindow can claim foreground. The IPC RPC fires synchronously
	// from the app's CBT hook, BEFORE the dialog HWND is materialized —
	// so granting here lands in the gauss process foreground-lock state
	// just in time for the dialog activation. Without this, the FIRST
	// dialog ever still works (gauss inherits foreground from process
	// startup), but subsequent ones after a CLOSE → OPEN cycle activate
	// invisibly because the workspace compositor is foreground by then
	// and Windows' lock denies cross-process activation. The MODAL_CLOSE
	// emit path also grants, but the 5-second AllowSetForegroundWindow
	// window can lapse between CLOSE and the next OPEN; granting on OPEN
	// too closes the timing hole.
	if (is_open && mc->clients[slot].app_hwnd != NULL) {
		DWORD app_pid = 0;
		GetWindowThreadProcessId(mc->clients[slot].app_hwnd, &app_pid);
		if (app_pid != 0) {
			(void)AllowSetForegroundWindow(app_pid);
		}
	}

	// Item 3 (#232) cont'd: kick the input-forward refresh immediately so
	// the cached input_forward_hwnd in the window goes to NULL while
	// modal_open is true (preventing keystrokes and clicks from reaching
	// the app HWND and spawning nested modals). Without this kick, the
	// modal_open flag is set on this IPC thread but the window's cached
	// forwarding target keeps pointing at the app until something else
	// happens to call update_input_forward (e.g., a focus change or the
	// next chrome layout push) — leaving a window where subsequent L-
	// presses on a hidden gauss app open more dialogs.
	//
	// comp_d3d11_window_set_input_forward writes via InterlockedExchange,
	// so calling from this IPC thread is safe even though the WndProc
	// thread reads concurrently.
	multi_compositor_update_input_forward(mc);
}

extern "C" xrt_result_t
comp_d3d11_service_workspace_post_file_picker_request(struct xrt_system_compositor *xsysc,
                                                      int slot,
                                                      const struct ipc_file_picker_info *info,
                                                      uint64_t *out_request_id)
{
	if (xsysc == nullptr || info == nullptr || out_request_id == nullptr) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (mc == nullptr) {
		return XRT_ERROR_IPC_FAILURE;
	}
	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);
	if (slot < 0 || slot >= D3D11_MULTI_MAX_CLIENTS || !mc->clients[slot].active) {
		return XRT_ERROR_IPC_FAILURE;
	}

	size_t entry_count = sizeof(mc->file_picker) / sizeof(mc->file_picker[0]);
	for (size_t i = 0; i < entry_count; i++) {
		if (mc->file_picker[i].in_use) continue;
		mc->next_file_picker_request_id++;
		if (mc->next_file_picker_request_id == 0) {
			mc->next_file_picker_request_id = 1; // never hand out 0 (XR_NULL_ASYNC_REQUEST_ID_EXT).
		}
		mc->file_picker[i].in_use = true;
		mc->file_picker[i].needs_emit = true;
		mc->file_picker[i].owner_slot = slot;
		mc->file_picker[i].request_id = mc->next_file_picker_request_id;
		mc->file_picker[i].info = *info;
		*out_request_id = mc->file_picker[i].request_id;
		U_LOG_W("file_picker: queued request_id=%llu (slot=%d, mode=%u, filters=%u)",
		        (unsigned long long)mc->file_picker[i].request_id, slot,
		        info->mode, info->filter_count);
		// Wake the workspace controller so it drains the new event in
		// the same frame the request arrives, instead of waiting for
		// its next periodic input poll (the shell sleeps on the
		// wakeup-event handle when nothing else is happening).
		service_signal_workspace_wakeup(sys);
		return XRT_SUCCESS;
	}

	U_LOG_W("file_picker: pending table full (%zu entries); rejecting request",
	        entry_count);
	*out_request_id = 0;
	return XRT_ERROR_IPC_FAILURE;
}

extern "C" xrt_result_t
comp_d3d11_service_workspace_get_file_picker_request(struct xrt_system_compositor *xsysc,
                                                     uint64_t request_id,
                                                     uint32_t *out_found,
                                                     uint32_t *out_client_id,
                                                     struct ipc_file_picker_info *out_info)
{
	if (out_found != nullptr) *out_found = 0;
	if (out_client_id != nullptr) *out_client_id = 0;
	if (out_info != nullptr) memset(out_info, 0, sizeof(*out_info));

	if (xsysc == nullptr || request_id == 0) {
		return XRT_SUCCESS; // not-found semantics, not an error.
	}
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (mc == nullptr) {
		return XRT_SUCCESS;
	}
	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);
	size_t entry_count = sizeof(mc->file_picker) / sizeof(mc->file_picker[0]);
	for (size_t i = 0; i < entry_count; i++) {
		if (!mc->file_picker[i].in_use) continue;
		if (mc->file_picker[i].request_id != request_id) continue;
		int s = mc->file_picker[i].owner_slot;
		if (s < 0 || s >= D3D11_MULTI_MAX_CLIENTS) continue;
		if (out_found != nullptr) *out_found = 1;
		if (out_client_id != nullptr) *out_client_id = mc->clients[s].workspace_client_id;
		if (out_info != nullptr) *out_info = mc->file_picker[i].info;
		return XRT_SUCCESS;
	}
	return XRT_SUCCESS;
}

extern "C" xrt_result_t
comp_d3d11_service_workspace_file_picker_result(struct xrt_system_compositor *xsysc,
                                                uint64_t request_id,
                                                uint32_t result_code,
                                                const struct ipc_file_picker_result_path *path)
{
	if (xsysc == nullptr || request_id == 0) {
		return XRT_ERROR_IPC_FAILURE;
	}
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (mc == nullptr) {
		return XRT_ERROR_IPC_FAILURE;
	}
	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);
	size_t entry_count = sizeof(mc->file_picker) / sizeof(mc->file_picker[0]);
	for (size_t i = 0; i < entry_count; i++) {
		if (!mc->file_picker[i].in_use) continue;
		if (mc->file_picker[i].request_id != request_id) continue;

		int s = mc->file_picker[i].owner_slot;
		mc->file_picker[i].in_use = false;
		mc->file_picker[i].needs_emit = false;

		if (s < 0 || s >= D3D11_MULTI_MAX_CLIENTS || !mc->clients[s].active ||
		    mc->clients[s].compositor == nullptr) {
			U_LOG_W("file_picker: dropping result for request_id=%llu (requesting slot %d is gone)",
			        (unsigned long long)request_id, s);
			return XRT_SUCCESS; // not an error path — late result is expected on requester crash.
		}

		struct xrt_session_event_sink *xses = mc->clients[s].compositor->xses;
		if (xses == nullptr) {
			U_LOG_W("file_picker: slot %d has no event sink — dropping result for %llu",
			        s, (unsigned long long)request_id);
			return XRT_SUCCESS;
		}

		union xrt_session_event xse = {};
		xse.file_picker_complete.type = XRT_SESSION_EVENT_FILE_PICKER_COMPLETE;
		xse.file_picker_complete.request_id = request_id;
		xse.file_picker_complete.result = result_code;
		if (path != nullptr) {
			static_assert(sizeof(xse.file_picker_complete.path) >= IPC_FILE_PICKER_PATH_MAX,
			              "session-event path buffer must hold a full IPC path");
			memcpy(xse.file_picker_complete.path, path->path,
			       sizeof(xse.file_picker_complete.path) - 1);
			xse.file_picker_complete.path[sizeof(xse.file_picker_complete.path) - 1] = '\0';
		}
		xrt_session_event_sink_push(xses, &xse);
		U_LOG_W("file_picker: delivered result_code=%u to slot %d (request_id=%llu)",
		        result_code, s, (unsigned long long)request_id);
		return XRT_SUCCESS;
	}

	U_LOG_W("file_picker: result for unknown request_id=%llu (already delivered or stale)",
	        (unsigned long long)request_id);
	return XRT_SUCCESS;
}

extern "C" bool
comp_d3d11_service_workspace_request_mode_flip(struct xrt_system_compositor *xsysc, uint32_t mode_index)
{
	if (xsysc == nullptr) {
		U_LOG_W("[mode_flip] hook: xsysc=NULL — returning false (caller falls back to legacy)");
		return false;
	}
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	if (sys == nullptr || sys->multi_comp == nullptr) {
		U_LOG_W("[mode_flip] hook: target=%u — sys=%p multi_comp=%p — returning false",
		        mode_index, (void *)sys, (void *)(sys ? sys->multi_comp : nullptr));
		return false;
	}
	U_LOG_W("[mode_flip] hook: entering request_mode_flip(target=%u) from controller session",
	        mode_index);
	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);
	// origin_slot = -1 (system origin) — the workspace controller is logically
	// the system, not a renderable slot.
	multi_compositor_request_mode_flip(sys, mode_index, /*origin=*/-1);
	return true;
}

extern "C" bool
comp_d3d11_service_force_display_3d(struct xrt_system_compositor *xsysc)
{
	if (xsysc == nullptr) {
		return false;
	}
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	if (sys == nullptr || sys->multi_comp == nullptr) {
		return false;
	}
	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	struct xrt_device *head = (sys->xsysd != nullptr) ? sys->xsysd->static_roles.head : nullptr;
	if (head == nullptr || head->hmd == NULL) {
		return false;
	}
	uint32_t target_idx = 1;
	if (target_idx >= head->rendering_mode_count) {
		return false;
	}
	uint32_t source_idx = head->hmd->active_rendering_mode_index;
	bool was_3d = sys->hardware_display_3d;

	// Route through the acked-flip state machine so the DP write happens
	// with curtain ON and the FLIPPING handler waits for hardware settle.
	// Calling xrt_display_processor_d3d11_request_display_mode() directly
	// on a freshly-created DP isn't reliable (Phase 6.1 #140 — the SR
	// SDK's recalibration cycle can swallow the request), so we mimic the
	// state machine's WAITING_ACK→FLIPPING transition and let apply_pending
	// drop the curtain once `get_hardware_3d_state` confirms.
	if (source_idx != target_idx) {
		// Normal case: source mode != target. Engage WAITING_ACK; the
		// state machine broadcasts the event, waits for app ack /
		// timeout, then transitions to FLIPPING.
		multi_compositor_request_mode_flip(sys, target_idx, /*origin=*/-1);
		U_LOG_W("[force_3d] engaged via state machine (was idx=%u 3d=%d → target=1)",
		        source_idx, (int)was_3d);
		return true;
	}

	// source_idx == target_idx case: the no-op guard in
	// multi_compositor_request_mode_flip would skip the DP write. But the
	// runtime's view can disagree with actual hardware (e.g. service just
	// started, sys->hardware_display_3d=false because of struct zero-init;
	// or SR Dashboard external toggle). We need the DP write to land
	// regardless. Skip WAITING_ACK (no apps need to re-render) and set up
	// FLIPPING directly, replicating the WAITING_ACK→FLIPPING transition
	// (DP request, sync_tile_layout, hardware_display_3d update); the
	// apply_pending FLIPPING handler then waits for the hardware to
	// actually settle before dropping the curtain.
	if (mc->display_processor == nullptr) {
		U_LOG_W("[force_3d] no DP yet — caller should retry after DP attach");
		return false;
	}

	mc->mode_flip.phase = MFP_FLIPPING;
	mc->mode_flip.target_mode_index = target_idx;
	mc->mode_flip.source_mode_index = source_idx;
	mc->mode_flip.target_is_3d = true;
	mc->mode_flip.origin_slot = -1;
	mc->mode_flip.ack_frame_counter = 0;
	mc->mode_flip.flip_frame_counter = 0;
	mc->mode_flip.curtain_active = true;
	for (int s = 0; s < D3D11_MULTI_MAX_CLIENTS; s++) {
		mc->clients[s].acked_target_mode = true;
		mc->clients[s].pre_flip_view_w = mc->clients[s].last_commit_view_w;
		mc->clients[s].pre_flip_view_h = mc->clients[s].last_commit_view_h;
	}

	xrt_device_set_property(head, XRT_DEVICE_PROPERTY_OUTPUT_MODE, (int32_t)target_idx);
	xrt_display_processor_d3d11_request_display_mode(mc->display_processor, /*hardware_display_3d=*/true);
	sync_tile_layout(sys);
	sys->hardware_display_3d = true;

	U_LOG_W("[force_3d] same-target fast path (idx=%u, was 3d=%d) — FLIPPING engaged, curtain ON",
	        target_idx, (int)was_3d);
	return true;
}

extern "C" void *
comp_d3d11_service_workspace_get_wakeup_event(struct xrt_system_compositor *xsysc)
{
	if (xsysc == nullptr) {
		return nullptr;
	}
#ifdef _WIN32
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	// Lazy-create on first call. Auto-reset (FALSE first BOOL), initial
	// state non-signaled (FALSE second BOOL). The handle returned to the
	// IPC layer is the runtime's source-of-truth — the IPC handler then
	// DuplicateHandle's it into the controller process so each process
	// has its own ref. Single Win32 event suffices for any number of
	// controllers (only one workspace controller exists at a time per
	// the activation auth handshake).
	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);
	if (sys->workspace_wakeup_event == nullptr) {
		HANDLE h = CreateEventA(NULL, FALSE /* auto-reset */, FALSE /* initial */, NULL);
		if (h == NULL) {
			U_LOG_W("workspace: CreateEventA(wakeup) failed: 0x%08lx", GetLastError());
			return nullptr;
		}
		sys->workspace_wakeup_event = (void *)h;
		// Propagate the handle to the WndProc so the public-ring push
		// path can SetEvent without a back-reference to sys.
		if (sys->multi_comp != nullptr && sys->multi_comp->window != nullptr) {
			comp_d3d11_window_set_workspace_wakeup_event(sys->multi_comp->window, h);
		}
	}
	return sys->workspace_wakeup_event;
#else
	(void)xsysc;
	return nullptr;
#endif
}

extern "C" void
comp_d3d11_service_workspace_set_cursor_depth(struct xrt_system_compositor *xsysc,
                                              float hit_z_m,
                                              bool over_window,
                                              float dim_factor)
{
	if (xsysc == nullptr) {
		return;
	}
	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (!sys->workspace_mode || mc == nullptr) {
		return; // Workspace not active — drop.
	}
	// Cached for the next composited frame; the cursor render block applies
	// hit_z_m to the per-eye disparity, over_window to whether to dim, and
	// dim_factor (#376) as the over-window cursor body alpha.
	mc->cursor_hit_z_m = hit_z_m;
	mc->cursor_over_window = over_window;
	mc->cursor_dim_factor = dim_factor;
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
	// ADR-018 (#304): first set_pose marks the client placed (slot-addressed
	// path — serves both capture clients and OpenXR clients posed by 1000+slot).
	mc->clients[slot_index].placed = true;

	slot_pose_to_pixel_rect(sys, &mc->clients[slot_index],
	    &mc->clients[slot_index].window_rect_x,
	    &mc->clients[slot_index].window_rect_y,
	    &mc->clients[slot_index].window_rect_w,
	    &mc->clients[slot_index].window_rect_h);

	// #320 regression fix: OpenXR clients are posed through this slot-addressed
	// (1000+slot) path too. The canonical-id setter resizes the app HWND on
	// every pose change — and that resize is what delivers WM_SIZE to the app
	// so it updates its window dims (Kooima physical screen size + render
	// resolution). Without it the app keeps rendering at its launch size,
	// scaled into a moved/resized tile, and its window-relative projection goes
	// stale. Restore that for non-capture clients (and refresh the focused
	// slot's input-forward rect so clicks track the moved/resized tile).
	// Capture clients resize their source HWND on their own edge-drag schedule,
	// so leave them untouched here.
	if (mc->clients[slot_index].client_type != CLIENT_TYPE_CAPTURE) {
		mc->clients[slot_index].hwnd_resize_pending = true;
		if (slot_index == mc->focused_slot) {
			multi_compositor_update_input_forward(mc);
		}
	}

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
		void *dp_fac_resume = comp_dp_factory_for_window(&sys->base.info, COMP_DP_PRIMARY_MONITOR, COMP_DP_API_D3D11);
		if (mc->display_processor == nullptr && dp_fac_resume != NULL) {
			auto factory = (xrt_dp_factory_d3d11_fn_t)dp_fac_resume;
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

		// Reset focus state
		mc->focused_slot = -1;

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

// Modal input grab (XR_EXT_spatial_workspace spec_version 18). Drives the
// window's input-suppress flag: while grabbed the WndProc stops forwarding
// keyboard / mouse-button / scroll input to the focused app and routes it all
// to the controller via the public event ring. The controller sets this while
// a modal UI it owns (e.g. the launcher band) is up, and clears it on dismiss.
// No-op if there's no multi-comp yet (workspace not active).
void
comp_d3d11_service_set_input_grab(struct xrt_system_compositor *xsysc, bool grab)
{
	if (xsysc == nullptr) {
		return;
	}

	struct d3d11_service_system *sys = d3d11_service_system_from_xrt(xsysc);
	std::lock_guard<std::recursive_mutex> lock(sys->render_mutex);

	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (mc == nullptr || mc->suspended) {
		U_LOG_W("Workspace: set_input_grab %s ignored — multi-comp not active",
		        grab ? "true" : "false");
		return;
	}

	if (mc->window != nullptr) {
		comp_d3d11_window_set_input_suppress(mc->window, grab);
	}
	sys->input_grabbed = grab; // cursor render reads this to pin to z=0

	// On grab, pull the compositor window to the foreground + give it keyboard
	// focus. WM_MOUSEWHEEL (and WM_KEYDOWN) are delivered to the focus window —
	// without this the controller's modal UI (launcher band) wouldn't receive
	// wheel events to forward as SCROLL_EXT. The controller grants us
	// foreground rights via AllowSetForegroundWindow before this IPC. (Mirrors
	// the focus grab the old runtime launcher did on open.)
	if (grab && mc->hwnd != nullptr) {
		SetForegroundWindow(mc->hwnd);
		SetFocus(mc->hwnd);
	}
	U_LOG_W("Workspace: input grab %s", grab ? "on" : "off");
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

	char base[MCP_CAPTURE_PATH_MAX];
	// Workspace compositor only supports POST_COMPOSE today; ignore mode
	// for now (the workspace already captures the combined atlas at the
	// post-compose point regardless of any per-client window-space layers).
	if (!mcp_capture_poll(&sys->mcp_capture, base, NULL)) {
		return;
	}

	// Delegate atlas capture to the shared capture_frame API.
	struct ipc_capture_result cr = {};
	bool ok = comp_d3d11_service_capture_frame(xsysc, base, IPC_CAPTURE_FLAG_ATLAS, &cr);

	// Write per-window metadata JSON.
	struct d3d11_multi_compositor *mc = sys->multi_comp;
	if (ok && mc != nullptr) {
		char path[MCP_CAPTURE_PATH_MAX + 32];
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
		char sentinel[MCP_CAPTURE_PATH_MAX + 32];
		snprintf(sentinel, sizeof(sentinel), "%s_DONE.txt", base);
		FILE *f = fopen(sentinel, "w");
		if (f) {
			fprintf(f, "ok=%d\n", ok);
			fclose(f);
		}
	}
	mcp_capture_complete(&sys->mcp_capture, ok);
}
