// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for @ref xrt_display_processor_metal interface.
 *
 * Metal variant of the display processor abstraction for vendor-specific
 * atlas-to-display output processing (interlacing, SBS, anaglyph, etc.).
 *
 * Unlike the D3D11 variant, this interface operates on Metal resources:
 * - Input is an atlas texture (id<MTLTexture>)
 * - Output goes to a provided render command encoder or texture
 * - Uses Metal command buffers rather than immediate-mode context
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

// Forward declarations for types used by optional vtable methods.
struct xrt_eye_positions;
struct xrt_window_metrics;

/*!
 * @interface xrt_display_processor_metal
 *
 * Metal display output processor that converts an atlas
 * texture into the final display output format.
 *
 * The compositor calls process_atlas() after rendering the view
 * pair into an atlas texture. The display processor writes the final
 * output to the provided render target texture.
 *
 * @ingroup xrt_iface
 */
struct xrt_display_processor_metal
{
	/*!
	 * `sizeof(struct xrt_display_processor_metal)` at the plug-in's compile
	 * time. Set by the plug-in factory; the runtime treats any slot at/past
	 * this offset as absent (NULL). 8-byte header (with @ref reserved_0) so
	 * @ref process_atlas lands at offset 8 on 64- and 32-bit. See ADR-020.
	 */
	uint32_t struct_size;

	/*! Reserved for alignment / future flags. Must be 0. */
	uint32_t reserved_0;

	/*!
	 * Process an atlas texture into the final display output.
	 *
	 * @param      xdp              Pointer to self.
	 * @param      command_buffer   Metal command buffer (id<MTLCommandBuffer>).
	 * @param      atlas_texture   Atlas texture (id<MTLTexture>).
	 * @param      view_width       Width of one eye view in pixels.
	 * @param      view_height      Height of one eye view in pixels.
	 * @param      tile_columns     Number of tile columns in the atlas layout.
	 * @param      tile_rows        Number of tile rows in the atlas layout.
	 * @param      format           MTLPixelFormat of the atlas texture (as uint32_t).
	 *                              Atlas *encoding state* (ADR-021) is conveyed via
	 *                              @ref set_atlas_encoding, not this format.
	 * @param      target_texture   Output render target (id<MTLTexture>).
	 * @param      target_width     Width of the output render target in pixels.
	 * @param      target_height    Height of the output render target in pixels.
	 * @param      canvas_offset_x  Canvas left edge in window client-area pixels (0 = no offset).
	 * @param      canvas_offset_y  Canvas top edge in window client-area pixels (0 = no offset).
	 * @param      canvas_width     Canvas width in pixels (0 = fills full window/target).
	 * @param      canvas_height    Canvas height in pixels (0 = fills full window/target).
	 */
	void (*process_atlas)(struct xrt_display_processor_metal *xdp,
	                       void *command_buffer,
	                       void *atlas_texture,
	                       uint32_t view_width,
	                       uint32_t view_height,
	                       uint32_t tile_columns,
	                       uint32_t tile_rows,
	                       uint32_t format,
	                       void *target_texture,
	                       uint32_t target_width,
	                       uint32_t target_height,
	                       int32_t canvas_offset_x,
	                       int32_t canvas_offset_y,
	                       uint32_t canvas_width,
	                       uint32_t canvas_height);

	/*!
	 * Get predicted eye positions from vendor eye tracking SDK.
	 * Optional — NULL means not supported.
	 */
	bool (*get_predicted_eye_positions)(struct xrt_display_processor_metal *xdp,
	                                    struct xrt_eye_positions *out_eye_pos);

	/*!
	 * Get window metrics for adaptive FOV calculation.
	 * Optional — NULL means not supported.
	 */
	bool (*get_window_metrics)(struct xrt_display_processor_metal *xdp,
	                           struct xrt_window_metrics *out_metrics);

	/*!
	 * Request a display mode switch (2D/3D).
	 * Optional — NULL means not supported.
	 */
	bool (*request_display_mode)(struct xrt_display_processor_metal *xdp,
	                             bool enable_3d);

	/*!
	 * Query hardware 3D display state from vendor SDK.
	 * Optional — NULL means not supported.
	 */
	bool (*get_hardware_3d_state)(struct xrt_display_processor_metal *xdp,
	                              bool *out_is_3d);

	/*!
	 * Get physical display dimensions in meters.
	 * Optional — NULL means not supported.
	 */
	bool (*get_display_dimensions)(struct xrt_display_processor_metal *xdp,
	                               float *out_width_m,
	                               float *out_height_m);

	/*!
	 * Get native display pixel info (resolution and screen position).
	 * Optional — NULL means not supported.
	 */
	bool (*get_display_pixel_info)(struct xrt_display_processor_metal *xdp,
	                               uint32_t *out_pixel_width,
	                               uint32_t *out_pixel_height,
	                               int32_t *out_screen_left,
	                               int32_t *out_screen_top);

	/*!
	 * Whether this display processor passes per-pixel alpha through to its
	 * output stage. true for sim_display-style processors; false (or NULL)
	 * for Leia-style weavers that need the chroma-key trick.
	 * Optional — NULL means false.
	 */
	bool (*is_alpha_native)(struct xrt_display_processor_metal *xdp);

	/*!
	 * Inform the DP of session-level transparency configuration.
	 * @p key_color is the app-supplied chroma key (0x00BBGGRR); 0 means
	 * the DP picks its own internal color. @p transparent_bg_enabled
	 * tells the DP whether to run its pre-weave fill / post-weave strip
	 * pass.
	 * Optional — NULL means the DP doesn't respect transparency requests.
	 */
	void (*set_chroma_key)(struct xrt_display_processor_metal *xdp,
	                       uint32_t key_color,
	                       bool transparent_bg_enabled);

	/*!
	 * Destroy this display processor and free all resources.
	 *
	 * @param xdp Pointer to self.
	 */
	void (*destroy)(struct xrt_display_processor_metal *xdp);

	/*!
	 * Declare which atlas encoding state(s) this DP accepts at handoff
	 * (ADR-021 §3, @ref xrt_dp_color_capability). Optional — absent slot or
	 * NULL ⟹ @ref XRT_DP_COLOR_ENCODED. Appended per ADR-020.
	 */
	enum xrt_dp_color_capability (*get_handoff_color_capability)(struct xrt_display_processor_metal *xdp);

	/*!
	 * Declare the atlas encoding for the next process_atlas (ADR-021 per-frame
	 * runtime intent; out-of-band so the format arg stays real). Optional —
	 * absent slot or NULL ⟹ DP assumes @ref XRT_ATLAS_ENCODING_ENCODED.
	 */
	void (*set_atlas_encoding)(struct xrt_display_processor_metal *xdp, enum xrt_atlas_encoding atlas_encoding);

	/*!
	 * Hand the DP this frame's flattened 2D-under backdrop (#491 part 3).
	 * Called once per frame, immediately before @ref process_atlas, when the
	 * frame carries Local2D layers before the projection (the "under" layers).
	 * The runtime flattens them into a single premultiplied-RGBA Metal texture
	 * in the client-window pixel space / canvas rect and passes it here. The DP
	 * composites it OVER its captured desktop background and uses the result as
	 * the under-3D background for the NEXT process_atlas. The texture must
	 * outlive that call and be sampleable.
	 *
	 * Pass NULL (or width/height 0) to clear — desktop-only background.
	 *
	 * Optional — absent slot or NULL ⟹ no-op (part-1-only behavior). Appended
	 * per ADR-020 (append-only within a major).
	 *
	 * @param xdp              Pointer to self.
	 * @param background_texture id<MTLTexture> of the flattened backdrop (or NULL to clear).
	 * @param width            Backdrop width in pixels.
	 * @param height           Backdrop height in pixels.
	 */
	void (*set_background_2d)(struct xrt_display_processor_metal *xdp,
	                          void *background_texture,
	                          uint32_t width,
	                          uint32_t height);
};

/*
 * ── Plug-in ABI tripwire (ADR-020) ─────────────────────────────────────────
 *
 * Per-API DP vtable; part of the same versioned plug-in ABI as the base
 * @ref xrt_display_processor. As of ABI major v2 it carries the 8-byte
 * struct_size header, so appending a method at the END is compatible within a
 * major; any other layout change is breaking and must bump
 * XRT_PLUGIN_API_VERSION_CURRENT (xrt_plugin.h) + re-pin every plug-in.
 */
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
#ifndef XRT_DP_HAS_SLOT
#define XRT_DP_HAS_SLOT(xdp, field)                                                                                    \
	((xdp) != NULL && ((const char *)&(xdp)->field + sizeof((xdp)->field)) <=                                       \
	                      ((const char *)(xdp) + (xdp)->struct_size))
#endif

#define XRT_DP_METAL_BASE_OFF offsetof(struct xrt_display_processor_metal, process_atlas)
// clang-format off
XRT_DP_ABI_ASSERT(XRT_DP_METAL_BASE_OFF == 8, XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_metal, process_atlas)               == XRT_DP_METAL_BASE_OFF + 0 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_metal, get_predicted_eye_positions) == XRT_DP_METAL_BASE_OFF + 1 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_metal, get_window_metrics)          == XRT_DP_METAL_BASE_OFF + 2 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_metal, request_display_mode)        == XRT_DP_METAL_BASE_OFF + 3 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_metal, get_hardware_3d_state)       == XRT_DP_METAL_BASE_OFF + 4 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_metal, get_display_dimensions)      == XRT_DP_METAL_BASE_OFF + 5 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_metal, get_display_pixel_info)      == XRT_DP_METAL_BASE_OFF + 6 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_metal, is_alpha_native)             == XRT_DP_METAL_BASE_OFF + 7 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_metal, set_chroma_key)              == XRT_DP_METAL_BASE_OFF + 8 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_metal, destroy)                     == XRT_DP_METAL_BASE_OFF + 9 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_metal, get_handoff_color_capability) == XRT_DP_METAL_BASE_OFF + 10 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_metal, set_atlas_encoding)           == XRT_DP_METAL_BASE_OFF + 11 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_metal, set_background_2d)            == XRT_DP_METAL_BASE_OFF + 12 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(sizeof(struct xrt_display_processor_metal)                                == XRT_DP_METAL_BASE_OFF + 13 * sizeof(void *), XRT_DP_ABI_MSG);
// clang-format on

/*!
 * @copydoc xrt_display_processor_metal::process_atlas
 *
 * Helper for calling through the function pointer.
 *
 * @public @memberof xrt_display_processor_metal
 */
static inline void
xrt_display_processor_metal_process_atlas(struct xrt_display_processor_metal *xdp,
                                           void *command_buffer,
                                           void *atlas_texture,
                                           uint32_t view_width,
                                           uint32_t view_height,
                                           uint32_t tile_columns,
                                           uint32_t tile_rows,
                                           uint32_t format,
                                           void *target_texture,
                                           uint32_t target_width,
                                           uint32_t target_height,
                                           int32_t canvas_offset_x,
                                           int32_t canvas_offset_y,
                                           uint32_t canvas_width,
                                           uint32_t canvas_height)
{
	xdp->process_atlas(xdp, command_buffer, atlas_texture, view_width, view_height, tile_columns, tile_rows,
	                    format, target_texture, target_width, target_height, canvas_offset_x, canvas_offset_y,
	                    canvas_width, canvas_height);
}

/*!
 * @copydoc xrt_display_processor_metal::get_predicted_eye_positions
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_metal
 */
static inline bool
xrt_display_processor_metal_get_predicted_eye_positions(struct xrt_display_processor_metal *xdp,
                                                        struct xrt_eye_positions *out_eye_pos)
{
	if (!XRT_DP_HAS_SLOT(xdp, get_predicted_eye_positions) || xdp->get_predicted_eye_positions == NULL) {
		return false;
	}
	return xdp->get_predicted_eye_positions(xdp, out_eye_pos);
}

/*!
 * @copydoc xrt_display_processor_metal::get_window_metrics
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_metal
 */
static inline bool
xrt_display_processor_metal_get_window_metrics(struct xrt_display_processor_metal *xdp,
                                               struct xrt_window_metrics *out_metrics)
{
	if (!XRT_DP_HAS_SLOT(xdp, get_window_metrics) || xdp->get_window_metrics == NULL) {
		return false;
	}
	return xdp->get_window_metrics(xdp, out_metrics);
}

/*!
 * @copydoc xrt_display_processor_metal::request_display_mode
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_metal
 */
static inline bool
xrt_display_processor_metal_request_display_mode(struct xrt_display_processor_metal *xdp, bool enable_3d)
{
	if (!XRT_DP_HAS_SLOT(xdp, request_display_mode) || xdp->request_display_mode == NULL) {
		return false;
	}
	return xdp->request_display_mode(xdp, enable_3d);
}

/*!
 * @copydoc xrt_display_processor_metal::get_hardware_3d_state
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_metal
 */
static inline bool
xrt_display_processor_metal_get_hardware_3d_state(struct xrt_display_processor_metal *xdp,
                                                  bool *out_is_3d)
{
	if (!XRT_DP_HAS_SLOT(xdp, get_hardware_3d_state) || xdp->get_hardware_3d_state == NULL) {
		return false;
	}
	return xdp->get_hardware_3d_state(xdp, out_is_3d);
}

/*!
 * @copydoc xrt_display_processor_metal::get_display_dimensions
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_metal
 */
static inline bool
xrt_display_processor_metal_get_display_dimensions(struct xrt_display_processor_metal *xdp,
                                                   float *out_width_m,
                                                   float *out_height_m)
{
	if (!XRT_DP_HAS_SLOT(xdp, get_display_dimensions) || xdp->get_display_dimensions == NULL) {
		return false;
	}
	return xdp->get_display_dimensions(xdp, out_width_m, out_height_m);
}

/*!
 * @copydoc xrt_display_processor_metal::get_display_pixel_info
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_metal
 */
static inline bool
xrt_display_processor_metal_get_display_pixel_info(struct xrt_display_processor_metal *xdp,
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
 * @copydoc xrt_display_processor_metal::is_alpha_native
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_metal
 */
static inline bool
xrt_display_processor_metal_is_alpha_native(struct xrt_display_processor_metal *xdp)
{
	if (!XRT_DP_HAS_SLOT(xdp, is_alpha_native) || xdp->is_alpha_native == NULL) {
		return false;
	}
	return xdp->is_alpha_native(xdp);
}

/*!
 * @copydoc xrt_display_processor_metal::set_chroma_key
 * No-op if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_metal
 */
static inline void
xrt_display_processor_metal_set_chroma_key(struct xrt_display_processor_metal *xdp,
                                            uint32_t key_color,
                                            bool transparent_bg_enabled)
{
	if (!XRT_DP_HAS_SLOT(xdp, set_chroma_key) || xdp->set_chroma_key == NULL) {
		return;
	}
	xdp->set_chroma_key(xdp, key_color, transparent_bg_enabled);
}

/*!
 * @copydoc xrt_display_processor_metal::get_handoff_color_capability
 * Returns @ref XRT_DP_COLOR_ENCODED if not supported (slot absent or NULL).
 * @public @memberof xrt_display_processor_metal
 */
static inline enum xrt_dp_color_capability
xrt_display_processor_metal_get_handoff_color_capability(struct xrt_display_processor_metal *xdp)
{
	if (!XRT_DP_HAS_SLOT(xdp, get_handoff_color_capability) || xdp->get_handoff_color_capability == NULL) {
		return XRT_DP_COLOR_ENCODED;
	}
	return xdp->get_handoff_color_capability(xdp);
}

/*!
 * @copydoc xrt_display_processor_metal::set_atlas_encoding
 * No-op if not supported (slot absent or NULL) — the DP then assumes ENCODED.
 * @public @memberof xrt_display_processor_metal
 */
static inline void
xrt_display_processor_metal_set_atlas_encoding(struct xrt_display_processor_metal *xdp,
                                               enum xrt_atlas_encoding atlas_encoding)
{
	if (!XRT_DP_HAS_SLOT(xdp, set_atlas_encoding) || xdp->set_atlas_encoding == NULL) {
		return;
	}
	xdp->set_atlas_encoding(xdp, atlas_encoding);
}

/*!
 * @copydoc xrt_display_processor_metal::set_background_2d
 * No-op when the DP doesn't expose the slot (older plug-in) or leaves it NULL.
 * @public @memberof xrt_display_processor_metal
 */
static inline void
xrt_display_processor_metal_set_background_2d(struct xrt_display_processor_metal *xdp,
                                              void *background_texture,
                                              uint32_t width,
                                              uint32_t height)
{
	if (!XRT_DP_HAS_SLOT(xdp, set_background_2d) || xdp->set_background_2d == NULL) {
		return;
	}
	xdp->set_background_2d(xdp, background_texture, width, height);
}

/*!
 * Factory function type for creating a Metal display processor.
 *
 * Called by the compositor to create a display processor for a session.
 * The factory is set by the target builder at init time and stored in
 * xrt_system_compositor_info.
 *
 * @param metal_device    Metal device (id<MTLDevice>).
 * @param command_queue   Metal command queue (id<MTLCommandQueue>).
 * @param window_handle   Native window handle (NSView*), may be NULL.
 * @param[out] out_xdp    Created display processor on success.
 * @return XRT_SUCCESS on success.
 */
typedef xrt_result_t (*xrt_dp_factory_metal_fn_t)(void *metal_device,
                                                   void *command_queue,
                                                   void *window_handle,
                                                   struct xrt_display_processor_metal **out_xdp);

/*!
 * Destroy an xrt_display_processor_metal — helper function.
 *
 * @param[in,out] xdp_ptr  A pointer to your display processor pointer.
 *
 * Will destroy the processor if *xdp_ptr is not NULL.
 * Will then set *xdp_ptr to NULL.
 *
 * @public @memberof xrt_display_processor_metal
 */
static inline void
xrt_display_processor_metal_destroy(struct xrt_display_processor_metal **xdp_ptr)
{
	struct xrt_display_processor_metal *xdp = *xdp_ptr;
	if (xdp == NULL) {
		return;
	}

	xdp->destroy(xdp);
	*xdp_ptr = NULL;
}


#ifdef __cplusplus
}
#endif
