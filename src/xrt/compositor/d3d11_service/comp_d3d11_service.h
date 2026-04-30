// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D11 service compositor interface.
 * @author David Fattal
 * @ingroup comp_d3d11_service
 *
 * This compositor is designed for IPC/service mode where the compositor
 * runs in a separate process from the OpenXR application. It:
 * - Creates its own D3D11 device (not using the app's device)
 * - Imports swapchain images from clients via DXGI shared handles
 * - Uses KeyedMutex for cross-process synchronization
 * - Integrates with Leia SR for light field display output
 */

#pragma once

#include "xrt/xrt_compositor.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_system.h"

#ifdef __cplusplus
extern "C" {
#endif

struct u_system;
// Forward decls from ipc_protocol.h — full definition is included by callers.
struct ipc_launcher_app;
struct ipc_capture_result;


/*!
 * @defgroup comp_d3d11_service D3D11 Service Compositor
 * @ingroup xrt
 *
 * D3D11-based compositor for Windows service mode (IPC).
 * Avoids Vulkan-D3D11 interop issues on Intel iGPUs by
 * using pure D3D11 throughout.
 */

/*!
 * Create the D3D11 service system compositor.
 *
 * This creates a compositor that runs in service mode with its own
 * D3D11 device, capable of importing swapchains from client processes
 * via DXGI shared handles.
 *
 * @param xdev The head device to render for.
 * @param xsysd System devices for qwerty input support (may be NULL).
 * @param usys System used to fan out session events (may be NULL).
 * @param[out] out_xsysc Pointer to receive the system compositor.
 * @return XRT_SUCCESS on success.
 *
 * @ingroup comp_d3d11_service
 */
xrt_result_t
comp_d3d11_service_create_system(struct xrt_device *xdev,
                                 struct xrt_system_devices *xsysd,
                                 struct u_system *usys,
                                 struct xrt_system_compositor **out_xsysc);

/*!
 * Check if the given system compositor is a D3D11 service compositor.
 *
 * @param xsysc The system compositor to check.
 * @return true if this is a D3D11 service compositor.
 *
 * @ingroup comp_d3d11_service
 */
bool
comp_d3d11_service_is_d3d11_service(struct xrt_system_compositor *xsysc);

/*!
 * Get predicted eye positions from the D3D11 service compositor's SR weaver.
 *
 * @param xsysc The system compositor (must be D3D11 service compositor).
 * @param[out] out_left Left eye position in meters (x, y, z).
 * @param[out] out_right Right eye position in meters (x, y, z).
 * @return true if eye positions were obtained, false otherwise.
 *
 * @ingroup comp_d3d11_service
 */
bool
comp_d3d11_service_get_predicted_eye_positions(struct xrt_system_compositor *xsysc,
                                                struct xrt_vec3 *out_left,
                                                struct xrt_vec3 *out_right);

/*!
 * Get display dimensions from the D3D11 service compositor's SR weaver.
 *
 * @param xsysc The system compositor (must be D3D11 service compositor).
 * @param[out] out_width_m Display width in meters.
 * @param[out] out_height_m Display height in meters.
 * @return true if dimensions were obtained, false otherwise.
 *
 * @ingroup comp_d3d11_service
 */
bool
comp_d3d11_service_get_display_dimensions(struct xrt_system_compositor *xsysc,
                                           float *out_width_m,
                                           float *out_height_m);

/*!
 * Check if the compositor owns its window (not using session_target).
 *
 * When using XR_EXT_win32_window_binding, the app provides its own window and
 * the compositor doesn't create one. This affects head position handling:
 * - Session target (app window): App controls head position, no offset applied
 * - Own window (Monado window): Apply standing height offset for VR apps
 *
 * @param xsysc The system compositor (must be D3D11 service compositor).
 * @return true if compositor owns the window, false if using app's window.
 *
 * @ingroup comp_d3d11_service
 */
bool
comp_d3d11_service_owns_window(struct xrt_system_compositor *xsysc);

/*!
 * Get window metrics from the active D3D11 service compositor.
 *
 * Returns window physical dimensions and center offset from display center,
 * used for window-relative Kooima projection in the IPC path.
 *
 * @param xsysc The system compositor (must be D3D11 service compositor).
 * @param[out] out_metrics Filled with window dimensions and center offset.
 * @return true if window metrics were obtained, false otherwise.
 *
 * @ingroup comp_d3d11_service
 */
bool
comp_d3d11_service_get_window_metrics(struct xrt_system_compositor *xsysc,
                                       struct xrt_window_metrics *out_metrics);

/*!
 * Set a client's virtual window pose and dimensions.
 *
 * Updates the window position, orientation, and size for the given client
 * compositor. The multi-compositor will recompute the pixel rect and trigger
 * a deferred HWND resize on the next render frame.
 *
 * @param xsysc The system compositor (must be D3D11 service compositor).
 * @param xc The client's compositor (used to find the multi-comp slot).
 * @param pose Window pose in display space (meters from display center).
 * @param width_m Window physical width in meters.
 * @param height_m Window physical height in meters.
 * @return true if the pose was applied, false otherwise.
 *
 * @ingroup comp_d3d11_service
 */
bool
comp_d3d11_service_set_client_window_pose(struct xrt_system_compositor *xsysc,
                                           struct xrt_compositor *xc,
                                           const struct xrt_pose *pose,
                                           float width_m,
                                           float height_m);

/*!
 * Get per-client virtual window metrics for Kooima projection.
 *
 * Returns window dimensions and center offset based on the client's virtual
 * window pose, not the physical HWND position. Used by the IPC view pose
 * computation to produce correct per-client Kooima projections.
 *
 * @param xsysc The system compositor (must be D3D11 service compositor).
 * @param xc The client's compositor (used to find the multi-comp slot).
 * @param[out] out_metrics Filled with virtual window dimensions and center offset.
 * @return true if metrics were obtained, false otherwise.
 *
 * @ingroup comp_d3d11_service
 */
bool
comp_d3d11_service_get_client_window_metrics(struct xrt_system_compositor *xsysc,
                                              struct xrt_compositor *xc,
                                              struct xrt_window_metrics *out_metrics);

/*!
 * Check if the compositor's window is still valid (not closed by user).
 *
 * This should be checked periodically by the IPC server to detect when
 * the user closes the Monado window (ESC, close button, etc.) so the
 * service can be shut down gracefully.
 *
 * @param xsysc The system compositor (must be D3D11 service compositor).
 * @return true if window is valid, false if closed or no window exists.
 *
 * @ingroup comp_d3d11_service
 */
bool
comp_d3d11_service_window_is_valid(struct xrt_system_compositor *xsysc);

/*!
 * Set visibility (minimize/un-minimize) for a client's window in the multi-compositor.
 */
bool
comp_d3d11_service_set_client_visibility(struct xrt_system_compositor *xsysc,
                                          struct xrt_compositor *xc,
                                          bool visible);

/*!
 * Get the current window pose and dimensions for a client in the multi-compositor.
 */
bool
comp_d3d11_service_get_client_window_pose(struct xrt_system_compositor *xsysc,
                                           struct xrt_compositor *xc,
                                           struct xrt_pose *out_pose,
                                           float *out_width_m,
                                           float *out_height_m);

/*!
 * @name Capture client management (Phase 4A)
 * @{
 */

/*!
 * Add a 2D window capture client to the multi-compositor.
 *
 * Starts Windows.Graphics.Capture for the given HWND and assigns a slot.
 * The captured window is displayed as a mono textured quad with spatial parallax.
 *
 * @param xsysc      The system compositor (must be D3D11 service in workspace mode).
 * @param hwnd_value  Window handle as uint64_t (cast from HWND).
 * @param name        Display name for the captured window (may be NULL).
 * @return Slot index (0-7) on success, -1 on failure.
 *
 * @ingroup comp_d3d11_service
 */
int
comp_d3d11_service_add_capture_client(struct xrt_system_compositor *xsysc,
                                       uint64_t hwnd_value,
                                       const char *name);

/*!
 * Remove a capture client from the multi-compositor.
 *
 * Stops capture and removes the slot.
 *
 * @param xsysc       The system compositor.
 * @param slot_index   Slot index returned by comp_d3d11_service_add_capture_client().
 * @return true on success, false if slot invalid or not a capture client.
 *
 * @ingroup comp_d3d11_service
 */
bool
comp_d3d11_service_remove_capture_client(struct xrt_system_compositor *xsysc,
                                          int slot_index);

/*!
 * Set window pose for a capture client by slot index.
 *
 * @param xsysc       The system compositor.
 * @param slot_index   Slot index.
 * @param pose         Window pose in display space.
 * @param width_m      Window width in meters.
 * @param height_m     Window height in meters.
 * @return true on success.
 *
 * @ingroup comp_d3d11_service
 */
bool
comp_d3d11_service_set_capture_client_window_pose(struct xrt_system_compositor *xsysc,
                                                    int slot_index,
                                                    const struct xrt_pose *pose,
                                                    float width_m,
                                                    float height_m);

/*!
 * Get window pose for a capture client by slot index.
 *
 * @param xsysc        The system compositor.
 * @param slot_index    Slot index.
 * @param[out] out_pose Window pose.
 * @param[out] out_width_m Window width in meters.
 * @param[out] out_height_m Window height in meters.
 * @return true on success.
 *
 * @ingroup comp_d3d11_service
 */
bool
comp_d3d11_service_get_capture_client_window_pose(struct xrt_system_compositor *xsysc,
                                                    int slot_index,
                                                    struct xrt_pose *out_pose,
                                                    float *out_width_m,
                                                    float *out_height_m);

/*!
 * Eagerly create the workspace compositor window (for empty workspace startup).
 * Called after workspace_activate when no clients are connected yet, so the
 * window exists to receive Ctrl+O app launch requests.
 */
bool
comp_d3d11_service_ensure_workspace_window(struct xrt_system_compositor *xsysc);

/*!
 * Deactivate the workspace: stop captures and restore 2D windows, suspend
 * multi-compositor (hide window, release DP, stop render loop).
 * Called by ipc_handle_workspace_deactivate().
 *
 * @param xsysc The system compositor (must be D3D11 service in workspace mode).
 *
 * @ingroup comp_d3d11_service
 */
void
comp_d3d11_service_deactivate_workspace(struct xrt_system_compositor *xsysc);

/*!
 * Show or hide the spatial launcher panel. When visible, the multi-compositor
 * draws a rounded-corner launcher overlay at the zero-disparity plane
 * (z = 0 in display coordinates). Called by
 * ipc_handle_launcher_set_visible() in response to Ctrl+L from the workspace controller.
 *
 * No-op if the workspace is not currently active.
 *
 * @ingroup comp_d3d11_service
 */
void
comp_d3d11_service_set_launcher_visible(struct xrt_system_compositor *xsysc, bool visible);

/*!
 * Empty the spatial launcher's app list. Called by the workspace controller at the start of
 * each registry push (clear-then-add-N pattern keeps each IPC message under
 * the per-message buffer cap).
 *
 * @ingroup comp_d3d11_service
 */
void
comp_d3d11_service_clear_launcher_apps(struct xrt_system_compositor *xsysc);

/*!
 * Append one app to the spatial launcher's tile grid. Silently dropped if the
 * list is already full (IPC_LAUNCHER_MAX_APPS). Called by the workspace controller once per
 * registered app after a clear.
 *
 * @ingroup comp_d3d11_service
 */
void
comp_d3d11_service_add_launcher_app(struct xrt_system_compositor *xsysc,
                                    const struct ipc_launcher_app *app);

/*!
 * Phase 5.9/5.10: poll-and-clear the pending launcher tile click. Returns the
 * tile index the user clicked since the last poll, or -1 if none. Called by
 * the workspace controller from its main poll loop; on a hit the controller looks up the
 * corresponding registered app and launches it via its launch-registered-app handler.
 *
 * @ingroup comp_d3d11_service
 */
int32_t
comp_d3d11_service_poll_launcher_click(struct xrt_system_compositor *xsysc);

/*!
 * Phase 5.11: set the bitmask of currently-running tiles. Bit @c i set means
 * the registered app at index @c i has at least one matching IPC client
 * connected to the service. The launcher draws a glow border around tiles
 * whose bit is set so the user can tell which apps are already open.
 *
 * Pushed by the workspace controller whenever its computed running set changes (typically
 * after each client connect/disconnect poll).
 *
 * @ingroup comp_d3d11_service
 */
void
comp_d3d11_service_set_running_tile_mask(struct xrt_system_compositor *xsysc, uint64_t mask);

/*!
 * Phase 8: capture the current pre-weave combined atlas to disk. Writes a
 * PNG of the full multi-view atlas (cropped to the active region in non-
 * legacy sessions) and fills @p out_result with metadata for the workspace controller's
 * sidecar JSON file.
 *
 * The runtime appends @c "_atlas.png" to @p path_prefix. Caller owns the
 * prefix string and the result struct.
 *
 * Must be called while the multi-compositor render mutex is held by this
 * thread, OR from a thread that does not hold the lock (this function takes
 * the lock itself).
 *
 * @param xsysc       The system compositor (must be D3D11 service).
 * @param path_prefix Filename prefix without extension.
 * @param flags       Bitmask of IPC_CAPTURE_FLAG_* views to write.
 * @param out_result  Filled with capture metadata. May not be NULL.
 * @return true if at least one view was successfully written.
 *
 * @ingroup comp_d3d11_service
 */
bool
comp_d3d11_service_capture_frame(struct xrt_system_compositor *xsysc,
                                 const char *path_prefix,
                                 uint32_t flags,
                                 struct ipc_capture_result *out_result);

/*!
 * Service a pending MCP capture_frame request. Delegates to
 * comp_d3d11_service_capture_frame for the atlas, then writes
 * {base}_windows.json with per-slot bbox metadata.
 *
 * Called from multi_compositor_render just before Present.
 *
 * @ingroup comp_d3d11_service
 */
void
comp_d3d11_service_poll_mcp_capture(struct xrt_system_compositor *xsysc);

/*!
 * Spatial raycast hit-test against workspace windows (Phase 2.F public surface).
 *
 * Translates a screen-space cursor in display pixels (origin top-left) to a
 * client window hit. Internally calls workspace_raycast_hit_test and maps the
 * internal flag set onto the XrWorkspaceHitRegionEXT vocabulary the controller
 * speaks. UV is meaningful only for CONTENT hits; chrome/edge hits set UV to 0
 * since the chrome coordinate frame is not exposed at the public surface.
 *
 * @param xsysc           The system compositor (must be D3D11 service in workspace mode).
 * @param cursor_x        Cursor X in display pixels (origin top-left).
 * @param cursor_y        Cursor Y in display pixels.
 * @param[out] out_client_id  Client id of the hit window, or 0 for miss (background).
 * @param[out] out_local_u    U coordinate of content hit (0..1); 0 for non-content hits.
 * @param[out] out_local_v    V coordinate of content hit (0..1); 0 for non-content hits.
 * @param[out] out_hit_region Classification (XrWorkspaceHitRegionEXT cast to uint32_t).
 * @return true on success (including miss); false only on usage error.
 *
 * @ingroup comp_d3d11_service
 */
bool
comp_d3d11_service_workspace_hit_test(struct xrt_system_compositor *xsysc,
                                       int32_t cursor_x,
                                       int32_t cursor_y,
                                       uint32_t *out_client_id,
                                       float *out_local_u,
                                       float *out_local_v,
                                       uint32_t *out_hit_region);

/*!
 * Phase 2.D: drain the workspace public-event ring, enriching POINTER events
 * with hit-test (clientId / region / UV). KEY and SCROLL events pass through.
 * Up to @p capacity events are written into @p out_batch (max
 * IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX). Caller passes the batch struct
 * directly because the IPC handler already has it.
 *
 * @param xsysc      The system compositor.
 * @param capacity   Max events the caller can accept (clamped to batch max).
 * @param out_batch  The batch to populate (count + events array).
 * @return true on success (including zero events); false only on usage error.
 */
struct ipc_workspace_input_event_batch;
bool
comp_d3d11_service_workspace_drain_input_events(struct xrt_system_compositor *xsysc,
                                                 uint32_t capacity,
                                                 struct ipc_workspace_input_event_batch *out_batch);

/*!
 * Phase 2.D: set the workspace pointer-capture flag. While enabled the WndProc
 * does not filter out-of-content button-up events from the public ring.
 *
 * @return true on success; false if @p xsysc is invalid.
 */
bool
comp_d3d11_service_workspace_pointer_capture_set(struct xrt_system_compositor *xsysc,
                                                  bool enabled,
                                                  uint32_t button);

/*!
 * Phase 2.K: ask the runtime to close a specific workspace client. For OpenXR
 * clients the runtime emits XRT_SESSION_EVENT_EXIT_REQUEST so the client exits
 * cleanly; for capture clients the runtime tears down the capture immediately.
 *
 * Capture-client convenience: client_id >= 1000 is treated as slot
 * (client_id - 1000). For OpenXR clients use the slot resolved via
 * comp_d3d11_service_workspace_find_slot_by_xc + the _by_slot helper below.
 *
 * @return XRT_SUCCESS on success;
 *         XRT_ERROR_IPC_FAILURE if @p xsysc is invalid or the client is unknown.
 */
xrt_result_t
comp_d3d11_service_workspace_request_client_exit(struct xrt_system_compositor *xsysc, uint32_t client_id);

/*!
 * Phase 2.K: ask the runtime to toggle fullscreen for a specific workspace
 * client. Mirrors the runtime's built-in F11 shortcut, but targeted at any
 * client. Same client_id encoding as request_client_exit.
 *
 * @return XRT_SUCCESS on success;
 *         XRT_ERROR_IPC_FAILURE if @p xsysc is invalid or the client is unknown.
 */
xrt_result_t
comp_d3d11_service_workspace_request_client_fullscreen(struct xrt_system_compositor *xsysc,
                                                       uint32_t client_id,
                                                       bool fullscreen);

/*!
 * Phase 2.K: slot-form of request_client_exit. The IPC handler resolves
 * OpenXR client_ids to slots via find_slot_by_xc and calls this directly.
 */
xrt_result_t
comp_d3d11_service_workspace_request_exit_by_slot(struct xrt_system_compositor *xsysc, int slot);

/*!
 * Phase 2.K: slot-form of request_client_fullscreen.
 */
xrt_result_t
comp_d3d11_service_workspace_request_fullscreen_by_slot(struct xrt_system_compositor *xsysc,
                                                        int slot,
                                                        bool fullscreen);

/*!
 * Phase 2.K: resolve the multi-compositor slot bound to an xrt_compositor.
 * Returns -1 on miss. Used by the IPC handler to translate OpenXR client_ids
 * (looked up via the IPC thread table) to slot indices for the request_*_by_slot
 * helpers.
 */
int
comp_d3d11_service_workspace_find_slot_by_xc(struct xrt_system_compositor *xsysc, struct xrt_compositor *xc);

/*! @} */


#ifdef __cplusplus
}
#endif
