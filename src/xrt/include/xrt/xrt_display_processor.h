// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for @ref xrt_display_processor interface.
 *
 * Abstracts vendor-specific atlas-to-display output processing
 * (interlacing for light field displays, SBS layout, anaglyph, etc.)
 * so the compositor remains vendor-agnostic.
 *
 * @author David Fattal
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_display_color.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h> // offsetof — used by the ABI tripwire at the end of this header

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations — avoid pulling in full Vulkan headers.
typedef struct VkCommandBuffer_T *VkCommandBuffer;

#ifdef XRT_64_BIT
typedef struct VkImage_T *VkImage_XDP;
typedef struct VkImageView_T *VkImageView;
typedef struct VkFramebuffer_T *VkFramebuffer;
typedef struct VkRenderPass_T *VkRenderPass;
#else
typedef uint64_t VkImage_XDP;
typedef uint64_t VkImageView;
typedef uint64_t VkFramebuffer;
typedef uint64_t VkRenderPass;
#endif

// Re-use Vulkan enum values without including vulkan.h.
typedef int32_t VkFormat_XDP;

// Forward declarations for types used by optional vtable methods.
struct xrt_eye_positions;
struct xrt_window_metrics;


/*!
 * @interface xrt_display_processor
 *
 * Generic display output processor that converts a tiled atlas texture
 * into the final display output format. Each vendor (Leia SR SDK, CNSDK,
 * simulation, etc.) provides its own implementation.
 *
 * The compositor calls process_atlas() after compositing all views
 * into an atlas texture, and the display processor produces the final output
 * (interlaced light field pattern, side-by-side, anaglyph, etc.).
 *
 * Lifecycle:
 * - Created by the vendor driver or builder
 * - Passed to the compositor at init time
 * - Compositor calls process_atlas() each frame
 * - Compositor calls xrt_display_processor_destroy() at shutdown
 *
 * @ingroup xrt_iface
 */
struct xrt_display_processor
{
	/*!
	 * `sizeof(struct xrt_display_processor)` as known at the *plug-in's*
	 * compile time. Set by the plug-in factory before handing the vtable
	 * back. The runtime treats any vtable slot whose bytes fall at or past
	 * this offset as absent (NULL / unsupported) — see @ref XRT_DP_HAS_SLOT
	 * and ADR-020 rule 1. Together with @ref reserved_0 this is an 8-byte
	 * header (mirroring @ref xrt_plugin_display_info) so @ref process_atlas
	 * lands at offset 8 on both 64-bit and 32-bit/Android targets.
	 */
	uint32_t struct_size;

	/*! Reserved for alignment / future flags. Must be 0. */
	uint32_t reserved_0;

	/*!
	 * @name Interface Methods
	 * @{
	 */

	/*!
	 * Process a tiled atlas texture into the final display output.
	 *
	 * Called by the compositor after layer compositing is complete.
	 * The implementation records Vulkan commands into @p cmd_buffer
	 * that transform the atlas texture into the target framebuffer
	 * in the display's native format.
	 *
	 * @param      xdp              Pointer to self.
	 * @param      cmd_buffer       Vulkan command buffer to record into.
	 * @param      atlas_image      Atlas VkImage handle (for copy/blit ops).
	 * @param      atlas_view       Atlas image view (tiled views).
	 * @param      view_width       Width of one view in the atlas in pixels.
	 * @param      view_height      Height of one view in the atlas in pixels.
	 * @param      tile_columns     Number of tile columns in the atlas layout.
	 * @param      tile_rows        Number of tile rows in the atlas layout.
	 * @param      view_format      Vulkan format of the atlas texture. (The atlas
	 *                              *encoding state* — ADR-021 — is conveyed
	 *                              out-of-band via @ref set_atlas_encoding, not
	 *                              this format, so older plug-ins that consume
	 *                              the real format keep working.)
	 * @param      target_fb        Target framebuffer to render into.
	 * @param      target_image     Target VkImage handle (for blit/copy ops).
	 * @param      target_width     Width of the target framebuffer in pixels.
	 * @param      target_height    Height of the target framebuffer in pixels.
	 * @param      target_format    Vulkan format of the target framebuffer.
	 * @param      canvas_offset_x  Canvas left edge in window client-area pixels (0 = no offset).
	 * @param      canvas_offset_y  Canvas top edge in window client-area pixels (0 = no offset).
	 * @param      canvas_width     Canvas width in pixels (0 = fills full window/target).
	 * @param      canvas_height    Canvas height in pixels (0 = fills full window/target).
	 */
	void (*process_atlas)(struct xrt_display_processor *xdp,
	                      VkCommandBuffer cmd_buffer,
	                      VkImage_XDP atlas_image,
	                      VkImageView atlas_view,
	                      uint32_t view_width,
	                      uint32_t view_height,
	                      uint32_t tile_columns,
	                      uint32_t tile_rows,
	                      VkFormat_XDP view_format,
	                      VkFramebuffer target_fb,
	                      VkImage_XDP target_image,
	                      uint32_t target_width,
	                      uint32_t target_height,
	                      VkFormat_XDP target_format,
	                      int32_t canvas_offset_x,
	                      int32_t canvas_offset_y,
	                      uint32_t canvas_width,
	                      uint32_t canvas_height);

	/*!
	 * Get predicted eye positions from vendor eye tracking SDK.
	 * Optional — NULL means not supported.
	 *
	 * @param      xdp           Pointer to self.
	 * @param[out] out_eye_pos   Predicted N-view eye positions.
	 * @return true if eye positions are valid.
	 */
	bool (*get_predicted_eye_positions)(struct xrt_display_processor *xdp,
	                                    struct xrt_eye_positions *out_eye_pos);

	/*!
	 * Get window metrics for adaptive FOV calculation.
	 * Optional — NULL means not supported.
	 *
	 * @param      xdp          Pointer to self.
	 * @param[out] out_metrics  Window and display geometry.
	 * @return true if metrics are valid.
	 */
	bool (*get_window_metrics)(struct xrt_display_processor *xdp,
	                           struct xrt_window_metrics *out_metrics);

	/*!
	 * Request a display mode switch (2D/3D).
	 * Optional — NULL means not supported.
	 *
	 * @param xdp        Pointer to self.
	 * @param enable_3d  true for 3D mode, false for 2D mode.
	 * @return true if the request was accepted.
	 */
	bool (*request_display_mode)(struct xrt_display_processor *xdp,
	                             bool enable_3d);

	/*!
	 * Query hardware 3D display state from vendor SDK.
	 * Optional — NULL means not supported.
	 *
	 * @param      xdp           Pointer to self.
	 * @param[out] out_is_3d     true if hardware is currently in 3D mode.
	 * @return true if query succeeded (vendor SDK available).
	 */
	bool (*get_hardware_3d_state)(struct xrt_display_processor *xdp,
	                              bool *out_is_3d);

	/*!
	 * Get the Vulkan render pass used by this display processor.
	 * Required by compositors that need to create VkFramebuffers
	 * for the display processor's render pass.
	 * Optional — NULL means not supported.
	 *
	 * @param xdp  Pointer to self.
	 * @return VkRenderPass handle, or VK_NULL_HANDLE if not available.
	 */
	VkRenderPass (*get_render_pass)(struct xrt_display_processor *xdp);

	/*!
	 * Get physical display dimensions in meters.
	 * Optional — NULL means not supported.
	 *
	 * @param      xdp           Pointer to self.
	 * @param[out] out_width_m   Physical width in meters.
	 * @param[out] out_height_m  Physical height in meters.
	 * @return true if dimensions are valid.
	 */
	bool (*get_display_dimensions)(struct xrt_display_processor *xdp,
	                               float *out_width_m,
	                               float *out_height_m);

	/*!
	 * Get native display pixel info (resolution and screen position).
	 * Optional — NULL means not supported.
	 *
	 * @param      xdp               Pointer to self.
	 * @param[out] out_pixel_width   Native panel width in pixels.
	 * @param[out] out_pixel_height  Native panel height in pixels.
	 * @param[out] out_screen_left   Display left edge in screen coordinates.
	 * @param[out] out_screen_top    Display top edge in screen coordinates.
	 * @return true if info is valid.
	 */
	bool (*get_display_pixel_info)(struct xrt_display_processor *xdp,
	                               uint32_t *out_pixel_width,
	                               uint32_t *out_pixel_height,
	                               int32_t *out_screen_left,
	                               int32_t *out_screen_top);

	/*!
	 * Whether this display processor passes per-pixel alpha through to its
	 * output stage. true for processors that sample atlas alpha and write it
	 * to the framebuffer (sim_display); false (or NULL) for processors that
	 * interlace into opaque RGB and need the chroma-key trick to recover
	 * transparency post-weave (Leia).
	 *
	 * Optional — NULL means false.
	 *
	 * @param xdp Pointer to self.
	 * @return true if alpha is preserved end-to-end through process_atlas.
	 */
	bool (*is_alpha_native)(struct xrt_display_processor *xdp);

	/*!
	 * Whether process_atlas records into its own VkCommandBuffer and submits
	 * it to the queue internally, rather than recording into the cmd_buffer
	 * passed by the compositor.
	 *
	 * When true, the compositor:
	 *   - Ends and submits its pre-DP cmd_buffer (window-space layer
	 *     composite, atlas crop) before calling process_atlas, so the GPU
	 *     order matches the source order.
	 *   - Passes VK_NULL_HANDLE for the cmd_buffer arg to process_atlas
	 *     (the DP is expected to ignore it).
	 *   - Skips its own post-process_atlas queue submit for the frame.
	 *
	 * Used by DPs that wrap a vendor SDK whose API records and submits
	 * internally (e.g. CNSDK's leia_interlacer_vulkan_do_post_process).
	 *
	 * Optional — NULL means false (compositor records-and-submits as usual).
	 *
	 * @param xdp Pointer to self.
	 * @return true if process_atlas submits its own cmd buffer.
	 */
	bool (*is_self_submitting)(struct xrt_display_processor *xdp);

	/*!
	 * Inform the DP of session-level transparency configuration.
	 * Called once at session start when @p transparent_bg_enabled is set on
	 * the corresponding window-binding extension. DPs that need the
	 * chroma-key trick (Leia) use this to enable an internal pre-weave
	 * fill (transparent atlas pixels → key color) and post-weave strip
	 * (key color → alpha=0). DPs that are alpha_native may treat this as
	 * a no-op.
	 *
	 * @p key_color is honored as an app-supplied override when non-zero
	 * (matches today's XR_EXT_win32_window_binding.chromaKeyColor); when
	 * zero, the DP picks its own internal color.
	 *
	 * Optional — NULL means the DP doesn't respect transparency requests.
	 *
	 * @param xdp                     Pointer to self.
	 * @param key_color                App-supplied chroma key (0x00BBGGRR);
	 *                                 0 means DP-picks (recommended).
	 * @param transparent_bg_enabled  True when transparency was requested.
	 */
	void (*set_chroma_key)(struct xrt_display_processor *xdp,
	                       uint32_t key_color,
	                       bool transparent_bg_enabled);

	/*!
	 * Notify the display processor that the host activity has paused
	 * (backgrounded). Vendor SDKs use this to drop face-tracking
	 * cameras, dim backlights, and otherwise save power.
	 *
	 * Idempotent. Safe to call before the vendor SDK is fully
	 * initialized — the vendor wrapper should no-op in that case.
	 *
	 * Optional — NULL means the vendor doesn't need pause notifications.
	 *
	 * @param xdp Pointer to self.
	 */
	void (*on_pause)(struct xrt_display_processor *xdp);

	/*!
	 * Notify the display processor that the host activity has resumed
	 * (foregrounded). Counterpart of @ref on_pause.
	 *
	 * Optional — NULL means no-op.
	 *
	 * @param xdp Pointer to self.
	 */
	void (*on_resume)(struct xrt_display_processor *xdp);

	/*!
	 * Destroy this display processor and free all resources.
	 *
	 * @param xdp Pointer to self.
	 */
	void (*destroy)(struct xrt_display_processor *xdp);

	/*!
	 * Declare which atlas encoding state(s) this DP accepts at handoff
	 * (ADR-021 §3, @ref xrt_dp_color_capability).
	 *
	 * Optional — an absent slot (older plug-in `struct_size`) or NULL ⟹
	 * @ref XRT_DP_COLOR_ENCODED, preserving Model-A passthrough. Appended at
	 * the end per ADR-020 (append-only within a major); see the ABI tripwire
	 * below.
	 *
	 * @param xdp Pointer to self.
	 * @return The DP's accepted handoff encoding capability.
	 */
	enum xrt_dp_color_capability (*get_handoff_color_capability)(struct xrt_display_processor *xdp);

	/*!
	 * Declare the encoding state of the atlas the runtime will send on the
	 * *next* @ref process_atlas call (ADR-021 per-frame runtime intent). The DP
	 * stores it and configures itself (e.g. enabling an output sRGB encode when
	 * the atlas is LINEAR). Conveyed out-of-band (not via the process_atlas
	 * format arg) so older plug-ins that consume the real format are unaffected.
	 *
	 * Optional — an absent slot (older plug-in `struct_size`) or NULL means the
	 * runtime can't declare and the DP assumes @ref XRT_ATLAS_ENCODING_ENCODED
	 * (Model-A passthrough). Appended per ADR-020 (append-only within a major).
	 *
	 * @param xdp            Pointer to self.
	 * @param atlas_encoding Encoding of the atlas for the next process_atlas.
	 */
	void (*set_atlas_encoding)(struct xrt_display_processor *xdp, enum xrt_atlas_encoding atlas_encoding);

	/*! @} */
};

/*
 * ── Plug-in ABI tripwire (ADR-020) ─────────────────────────────────────────
 *
 * `xrt_display_processor` is a vtable shared across the runtime ↔ vendor-plug-in
 * DLL boundary (ADR-019). As of ABI major v2 it carries a `struct_size` header
 * (see the struct above): the plug-in reports how many slots it built, and the
 * runtime treats any slot at/past that offset as absent (NULL) via
 * @ref XRT_DP_HAS_SLOT. That makes *appending* a new method at the END
 * backward- and forward-compatible WITHIN a major — no version bump needed.
 *
 * What is STILL breaking (and trips an assert below): reordering an existing
 * slot, removing one, changing a signature, or inserting anywhere but the end.
 * That is exactly what broke standalone-VK weaving — on_pause/on_resume were
 * inserted mid-struct (before destroy) without a version bump, so the runtime's
 * set_chroma_key call hit a plug-in's destroy. See ADR-020.
 *
 * If you change anything that trips an assert below, you are making a BREAKING
 * ABI change. In the SAME change you MUST:
 *   1. Bump XRT_PLUGIN_API_VERSION_CURRENT in xrt_plugin.h (major),
 *   2. Update the slot indices / count here,
 *   3. Re-pin + rebuild every vendor plug-in (e.g. leia-plugin's
 *      DXR_RUNTIME_GIT_TAG and its CI runtime-checkout ref) and the bundle.
 * To add a method WITHOUT a major bump: append it at the very END (after the
 * last slot below), add its assert, bump the size assert, and gate its wrapper
 * on @ref XRT_DP_HAS_SLOT — old plug-ins (smaller struct_size) see it as absent.
 *
 * Offsets are anchored at XRT_DP_BASE_OFF (the offset of the first slot, after
 * the 8-byte struct_size header) plus (slot index) * sizeof(void *), so the
 * check holds on both 64-bit and 32-bit (Android) builds.
 */
// Guarded so the per-API xrt_display_processor_<api>.h headers can define the
// same helpers regardless of include order (no MSVC C4005 redefinition).
#ifndef XRT_DP_ABI_ASSERT
#if defined(__cplusplus)
#define XRT_DP_ABI_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define XRT_DP_ABI_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif
#endif
#ifndef XRT_DP_ABI_MSG
#define XRT_DP_ABI_MSG                                                                                                  \
	"xrt_display_processor ABI changed — see ADR-020: bump XRT_PLUGIN_API_VERSION_CURRENT and re-pin every plug-in."
#endif

/*!
 * Offset of the first vtable slot (`process_atlas`), i.e. the size of the
 * struct_size header. Asserted == 8 below so it holds identically on 64-bit
 * (8 = one pointer slot) and 32-bit/Android (8 = two pointer slots).
 */
#define XRT_DP_BASE_OFF offsetof(struct xrt_display_processor, process_atlas)

// clang-format off
XRT_DP_ABI_ASSERT(XRT_DP_BASE_OFF == 8, XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor, process_atlas)                == XRT_DP_BASE_OFF +  0 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor, get_predicted_eye_positions)  == XRT_DP_BASE_OFF +  1 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor, get_window_metrics)           == XRT_DP_BASE_OFF +  2 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor, request_display_mode)         == XRT_DP_BASE_OFF +  3 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor, get_hardware_3d_state)        == XRT_DP_BASE_OFF +  4 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor, get_render_pass)              == XRT_DP_BASE_OFF +  5 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor, get_display_dimensions)       == XRT_DP_BASE_OFF +  6 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor, get_display_pixel_info)       == XRT_DP_BASE_OFF +  7 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor, is_alpha_native)              == XRT_DP_BASE_OFF +  8 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor, is_self_submitting)           == XRT_DP_BASE_OFF +  9 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor, set_chroma_key)               == XRT_DP_BASE_OFF + 10 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor, on_pause)                     == XRT_DP_BASE_OFF + 11 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor, on_resume)                    == XRT_DP_BASE_OFF + 12 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor, destroy)                      == XRT_DP_BASE_OFF + 13 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor, get_handoff_color_capability) == XRT_DP_BASE_OFF + 14 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor, set_atlas_encoding)            == XRT_DP_BASE_OFF + 15 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(sizeof(struct xrt_display_processor)                                 == XRT_DP_BASE_OFF + 16 * sizeof(void *), XRT_DP_ABI_MSG);
// clang-format on

/*!
 * True when the vtable @p xdp actually carries the slot @p field — i.e. the
 * plug-in that built it reported a `struct_size` large enough to include that
 * field's bytes. The runtime treats appended-but-unknown-to-this-plug-in slots
 * as absent (ADR-020 rule 1). Optional-method wrappers below gate on this in
 * addition to the per-pointer NULL check, so a runtime built against a newer
 * header never reads a slot an older plug-in never allocated.
 *
 * Evaluates @p xdp once would require a statement-expr; the macro instead
 * tolerates re-evaluation — callers always pass a plain local pointer.
 *
 * The body is type-generic (it only needs `field` and `struct_size` members),
 * so the per-API `xrt_display_processor_<api>.h` headers reuse the same macro;
 * the `#ifndef` guard lets whichever header is included first define it.
 */
#ifndef XRT_DP_HAS_SLOT
#define XRT_DP_HAS_SLOT(xdp, field)                                                                                    \
	((xdp) != NULL && ((const char *)&(xdp)->field + sizeof((xdp)->field)) <=                                       \
	                      ((const char *)(xdp) + (xdp)->struct_size))
#endif

/*!
 * @copydoc xrt_display_processor::process_atlas
 *
 * Helper for calling through the function pointer.
 *
 * @public @memberof xrt_display_processor
 */
static inline void
xrt_display_processor_process_atlas(struct xrt_display_processor *xdp,
                                    VkCommandBuffer cmd_buffer,
                                    VkImage_XDP atlas_image,
                                    VkImageView atlas_view,
                                    uint32_t view_width,
                                    uint32_t view_height,
                                    uint32_t tile_columns,
                                    uint32_t tile_rows,
                                    VkFormat_XDP view_format,
                                    VkFramebuffer target_fb,
                                    VkImage_XDP target_image,
                                    uint32_t target_width,
                                    uint32_t target_height,
                                    VkFormat_XDP target_format,
                                    int32_t canvas_offset_x,
                                    int32_t canvas_offset_y,
                                    uint32_t canvas_width,
                                    uint32_t canvas_height)
{
	// Mandatory slot (offset XRT_DP_BASE_OFF): every plug-in's struct_size
	// covers it, so no XRT_DP_HAS_SLOT gate — see ADR-020.
	xdp->process_atlas(xdp, cmd_buffer, atlas_image, atlas_view,
	                   view_width, view_height,
	                   tile_columns, tile_rows,
	                   view_format, target_fb,
	                   target_image,
	                   target_width, target_height,
	                   target_format,
	                   canvas_offset_x, canvas_offset_y,
	                   canvas_width, canvas_height);
}

/*!
 * @copydoc xrt_display_processor::get_predicted_eye_positions
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor
 */
static inline bool
xrt_display_processor_get_predicted_eye_positions(struct xrt_display_processor *xdp,
                                                  struct xrt_eye_positions *out_eye_pos)
{
	if (!XRT_DP_HAS_SLOT(xdp, get_predicted_eye_positions) || xdp->get_predicted_eye_positions == NULL) {
		return false;
	}
	return xdp->get_predicted_eye_positions(xdp, out_eye_pos);
}

/*!
 * @copydoc xrt_display_processor::get_window_metrics
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor
 */
static inline bool
xrt_display_processor_get_window_metrics(struct xrt_display_processor *xdp,
                                         struct xrt_window_metrics *out_metrics)
{
	if (!XRT_DP_HAS_SLOT(xdp, get_window_metrics) || xdp->get_window_metrics == NULL) {
		return false;
	}
	return xdp->get_window_metrics(xdp, out_metrics);
}

/*!
 * @copydoc xrt_display_processor::request_display_mode
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor
 */
static inline bool
xrt_display_processor_request_display_mode(struct xrt_display_processor *xdp, bool enable_3d)
{
	if (!XRT_DP_HAS_SLOT(xdp, request_display_mode) || xdp->request_display_mode == NULL) {
		return false;
	}
	return xdp->request_display_mode(xdp, enable_3d);
}

/*!
 * @copydoc xrt_display_processor::get_hardware_3d_state
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor
 */
static inline bool
xrt_display_processor_get_hardware_3d_state(struct xrt_display_processor *xdp,
                                            bool *out_is_3d)
{
	if (!XRT_DP_HAS_SLOT(xdp, get_hardware_3d_state) || xdp->get_hardware_3d_state == NULL) {
		return false;
	}
	return xdp->get_hardware_3d_state(xdp, out_is_3d);
}

/*!
 * @copydoc xrt_display_processor::get_render_pass
 * Returns VK_NULL_HANDLE if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor
 */
static inline VkRenderPass
xrt_display_processor_get_render_pass(struct xrt_display_processor *xdp)
{
	if (!XRT_DP_HAS_SLOT(xdp, get_render_pass) || xdp->get_render_pass == NULL) {
		return (VkRenderPass)0;
	}
	return xdp->get_render_pass(xdp);
}

/*!
 * @copydoc xrt_display_processor::get_display_dimensions
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor
 */
static inline bool
xrt_display_processor_get_display_dimensions(struct xrt_display_processor *xdp,
                                             float *out_width_m,
                                             float *out_height_m)
{
	if (!XRT_DP_HAS_SLOT(xdp, get_display_dimensions) || xdp->get_display_dimensions == NULL) {
		return false;
	}
	return xdp->get_display_dimensions(xdp, out_width_m, out_height_m);
}

/*!
 * @copydoc xrt_display_processor::get_display_pixel_info
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor
 */
static inline bool
xrt_display_processor_get_display_pixel_info(struct xrt_display_processor *xdp,
                                             uint32_t *out_pixel_width,
                                             uint32_t *out_pixel_height,
                                             int32_t *out_screen_left,
                                             int32_t *out_screen_top)
{
	if (!XRT_DP_HAS_SLOT(xdp, get_display_pixel_info) || xdp->get_display_pixel_info == NULL) {
		return false;
	}
	return xdp->get_display_pixel_info(xdp, out_pixel_width, out_pixel_height, out_screen_left, out_screen_top);
}

/*!
 * @copydoc xrt_display_processor::is_alpha_native
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor
 */
static inline bool
xrt_display_processor_is_alpha_native(struct xrt_display_processor *xdp)
{
	if (!XRT_DP_HAS_SLOT(xdp, is_alpha_native) || xdp->is_alpha_native == NULL) {
		return false;
	}
	return xdp->is_alpha_native(xdp);
}

/*!
 * @copydoc xrt_display_processor::is_self_submitting
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor
 */
static inline bool
xrt_display_processor_is_self_submitting(struct xrt_display_processor *xdp)
{
	if (!XRT_DP_HAS_SLOT(xdp, is_self_submitting) || xdp->is_self_submitting == NULL) {
		return false;
	}
	return xdp->is_self_submitting(xdp);
}

/*!
 * @copydoc xrt_display_processor::set_chroma_key
 * No-op if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor
 */
static inline void
xrt_display_processor_set_chroma_key(struct xrt_display_processor *xdp,
                                     uint32_t key_color,
                                     bool transparent_bg_enabled)
{
	if (!XRT_DP_HAS_SLOT(xdp, set_chroma_key) || xdp->set_chroma_key == NULL) {
		return;
	}
	xdp->set_chroma_key(xdp, key_color, transparent_bg_enabled);
}

/*!
 * @copydoc xrt_display_processor::on_pause
 * No-op if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor
 */
static inline void
xrt_display_processor_on_pause(struct xrt_display_processor *xdp)
{
	if (!XRT_DP_HAS_SLOT(xdp, on_pause) || xdp->on_pause == NULL) {
		return;
	}
	xdp->on_pause(xdp);
}

/*!
 * @copydoc xrt_display_processor::on_resume
 * No-op if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor
 */
static inline void
xrt_display_processor_on_resume(struct xrt_display_processor *xdp)
{
	if (!XRT_DP_HAS_SLOT(xdp, on_resume) || xdp->on_resume == NULL) {
		return;
	}
	xdp->on_resume(xdp);
}

/*!
 * @copydoc xrt_display_processor::get_handoff_color_capability
 * Returns @ref XRT_DP_COLOR_ENCODED if not supported (slot absent or NULL) —
 * the ADR-021 back-compat default.
 * @public @memberof xrt_display_processor
 */
static inline enum xrt_dp_color_capability
xrt_display_processor_get_handoff_color_capability(struct xrt_display_processor *xdp)
{
	if (!XRT_DP_HAS_SLOT(xdp, get_handoff_color_capability) || xdp->get_handoff_color_capability == NULL) {
		return XRT_DP_COLOR_ENCODED;
	}
	return xdp->get_handoff_color_capability(xdp);
}

/*!
 * @copydoc xrt_display_processor::set_atlas_encoding
 * No-op if not supported (slot absent or NULL) — the DP then assumes ENCODED.
 * @public @memberof xrt_display_processor
 */
static inline void
xrt_display_processor_set_atlas_encoding(struct xrt_display_processor *xdp, enum xrt_atlas_encoding atlas_encoding)
{
	if (!XRT_DP_HAS_SLOT(xdp, set_atlas_encoding) || xdp->set_atlas_encoding == NULL) {
		return;
	}
	xdp->set_atlas_encoding(xdp, atlas_encoding);
}

/*!
 * Factory function type for creating a Vulkan display processor.
 *
 * Called by the compositor to create a display processor for a session.
 * The factory is set by the target builder at init time and stored in
 * xrt_system_compositor_info. The implementation creates and owns all
 * vendor-specific resources internally.
 *
 * @param vk_bundle      Opaque pointer to the compositor's struct vk_bundle.
 * @param vk_cmd_pool    Vulkan command pool (VkCommandPool as void*) for recording.
 * @param window_handle  Native window handle (HWND on Windows, etc.), may be NULL.
 * @param target_format  Target framebuffer format (VkFormat as int32_t).
 * @param[out] out_xdp   Created display processor on success.
 * @return XRT_SUCCESS on success.
 */
typedef xrt_result_t (*xrt_dp_factory_vk_fn_t)(void *vk_bundle,
                                               void *vk_cmd_pool,
                                               void *window_handle,
                                               int32_t target_format,
                                               struct xrt_display_processor **out_xdp);

/*!
 * Destroy an xrt_display_processor — helper function.
 *
 * @param[in,out] xdp_ptr  A pointer to your display processor pointer.
 *
 * Will destroy the processor if *xdp_ptr is not NULL.
 * Will then set *xdp_ptr to NULL.
 *
 * @public @memberof xrt_display_processor
 */
static inline void
xrt_display_processor_destroy(struct xrt_display_processor **xdp_ptr)
{
	struct xrt_display_processor *xdp = *xdp_ptr;
	if (xdp == NULL) {
		return;
	}

	// Mandatory slot: covered by every plug-in's struct_size, so un-gated.
	xdp->destroy(xdp);
	*xdp_ptr = NULL;
}


#ifdef __cplusplus
}
#endif
