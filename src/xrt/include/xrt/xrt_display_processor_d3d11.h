// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header for @ref xrt_display_processor_d3d11 interface.
 *
 * D3D11 variant of the display processor abstraction for vendor-specific
 * atlas-to-display output processing (interlacing, SBS, anaglyph, etc.).
 *
 * Unlike the Vulkan variant, this interface operates on D3D11 resources:
 * - Input is an atlas SRV (not separate left/right views)
 * - Output goes to the currently bound render target (no framebuffer param)
 * - No command buffer — D3D11 uses immediate-mode device context
 *
 * @author David Fattal
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_results.h"

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
 * @interface xrt_display_processor_d3d11
 *
 * D3D11 display output processor that converts an atlas
 * texture into the final display output format.
 *
 * The compositor calls process_atlas() after rendering the view
 * pair into an atlas texture. The display processor writes the final
 * output (interlaced pattern, etc.) to the currently bound render target.
 *
 * @ingroup xrt_iface
 */
struct xrt_display_processor_d3d11
{
	/*!
	 * `sizeof(struct xrt_display_processor_d3d11)` at the plug-in's compile
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
	 * The output render target must already be bound via OMSetRenderTargets.
	 * The implementation will set the viewport and perform the display-
	 * specific processing (interlacing, etc.).
	 *
	 * @param      xdp              Pointer to self.
	 * @param      d3d11_context    D3D11 device context (ID3D11DeviceContext*).
	 * @param      atlas_srv       Atlas texture SRV (ID3D11ShaderResourceView*).
	 * @param      view_width       Width of one eye view in pixels.
	 * @param      view_height      Height of one eye view in pixels.
	 * @param      tile_columns     Number of tile columns in the atlas layout.
	 * @param      tile_rows        Number of tile rows in the atlas layout.
	 * @param      format           DXGI format of the atlas texture (DXGI_FORMAT as uint32_t).
	 * @param      target_width     Width of the output render target in pixels.
	 * @param      target_height    Height of the output render target in pixels.
	 * @param      canvas_offset_x  Canvas left edge in window client-area pixels (0 = no offset).
	 * @param      canvas_offset_y  Canvas top edge in window client-area pixels (0 = no offset).
	 * @param      canvas_width     Canvas width in pixels (0 = fills full window/target).
	 * @param      canvas_height    Canvas height in pixels (0 = fills full window/target).
	 */
	void (*process_atlas)(struct xrt_display_processor_d3d11 *xdp,
	                       void *d3d11_context,
	                       void *atlas_srv,
	                       uint32_t view_width,
	                       uint32_t view_height,
	                       uint32_t tile_columns,
	                       uint32_t tile_rows,
	                       uint32_t format,
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
	bool (*get_predicted_eye_positions)(struct xrt_display_processor_d3d11 *xdp,
	                                    struct xrt_eye_positions *out_eye_pos);

	/*!
	 * Get window metrics for adaptive FOV calculation.
	 * Optional — NULL means not supported.
	 */
	bool (*get_window_metrics)(struct xrt_display_processor_d3d11 *xdp,
	                           struct xrt_window_metrics *out_metrics);

	/*!
	 * Request a display mode switch (2D/3D).
	 * Optional — NULL means not supported.
	 */
	bool (*request_display_mode)(struct xrt_display_processor_d3d11 *xdp,
	                             bool enable_3d);

	/*!
	 * Query hardware 3D display state from vendor SDK.
	 * Optional — NULL means not supported.
	 *
	 * @param      xdp           Pointer to self.
	 * @param[out] out_is_3d     true if hardware is currently in 3D mode.
	 * @return true if query succeeded (vendor SDK available).
	 */
	bool (*get_hardware_3d_state)(struct xrt_display_processor_d3d11 *xdp,
	                              bool *out_is_3d);

	/*!
	 * Get physical display dimensions in meters.
	 * Optional — NULL means not supported.
	 */
	bool (*get_display_dimensions)(struct xrt_display_processor_d3d11 *xdp,
	                               float *out_width_m,
	                               float *out_height_m);

	/*!
	 * Get native display pixel info (resolution and screen position).
	 * Optional — NULL means not supported.
	 */
	bool (*get_display_pixel_info)(struct xrt_display_processor_d3d11 *xdp,
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
	bool (*is_alpha_native)(struct xrt_display_processor_d3d11 *xdp);

	/*!
	 * Inform the DP of session-level transparency configuration.
	 * @p key_color is the app-supplied chroma key (0x00BBGGRR); 0 means
	 * the DP picks its own internal color. @p transparent_bg_enabled
	 * tells the DP whether to run its pre-weave fill / post-weave strip
	 * pass.
	 * Optional — NULL means the DP doesn't respect transparency requests.
	 */
	void (*set_chroma_key)(struct xrt_display_processor_d3d11 *xdp,
	                       uint32_t key_color,
	                       bool transparent_bg_enabled);

	/*!
	 * Destroy this display processor and free all resources.
	 *
	 * @param xdp Pointer to self.
	 */
	void (*destroy)(struct xrt_display_processor_d3d11 *xdp);
};

/*
 * ── Plug-in ABI tripwire (ADR-020) ─────────────────────────────────────────
 *
 * The per-API DP vtable is part of the same versioned plug-in ABI as the base
 * @ref xrt_display_processor (the compositor calls it directly; the plug-in
 * factory fills it). As of ABI major v2 it carries the same 8-byte struct_size
 * header, so appending a method at the END is compatible within a major; any
 * other layout change is breaking and must bump XRT_PLUGIN_API_VERSION_CURRENT
 * (xrt_plugin.h) + re-pin every plug-in. The asserts pin each slot at
 * base-offset + index*sizeof(void *) so they hold on 64- and 32-bit.
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

#define XRT_DP_D3D11_BASE_OFF offsetof(struct xrt_display_processor_d3d11, process_atlas)
// clang-format off
XRT_DP_ABI_ASSERT(XRT_DP_D3D11_BASE_OFF == 8, XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d11, process_atlas)               == XRT_DP_D3D11_BASE_OFF + 0 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d11, get_predicted_eye_positions) == XRT_DP_D3D11_BASE_OFF + 1 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d11, get_window_metrics)          == XRT_DP_D3D11_BASE_OFF + 2 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d11, request_display_mode)        == XRT_DP_D3D11_BASE_OFF + 3 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d11, get_hardware_3d_state)       == XRT_DP_D3D11_BASE_OFF + 4 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d11, get_display_dimensions)      == XRT_DP_D3D11_BASE_OFF + 5 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d11, get_display_pixel_info)      == XRT_DP_D3D11_BASE_OFF + 6 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d11, is_alpha_native)             == XRT_DP_D3D11_BASE_OFF + 7 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d11, set_chroma_key)              == XRT_DP_D3D11_BASE_OFF + 8 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(offsetof(struct xrt_display_processor_d3d11, destroy)                     == XRT_DP_D3D11_BASE_OFF + 9 * sizeof(void *), XRT_DP_ABI_MSG);
XRT_DP_ABI_ASSERT(sizeof(struct xrt_display_processor_d3d11)                                == XRT_DP_D3D11_BASE_OFF + 10 * sizeof(void *), XRT_DP_ABI_MSG);
// clang-format on

/*!
 * @copydoc xrt_display_processor_d3d11::process_atlas
 *
 * Helper for calling through the function pointer.
 *
 * @public @memberof xrt_display_processor_d3d11
 */
static inline void
xrt_display_processor_d3d11_process_atlas(struct xrt_display_processor_d3d11 *xdp,
                                           void *d3d11_context,
                                           void *atlas_srv,
                                           uint32_t view_width,
                                           uint32_t view_height,
                                           uint32_t tile_columns,
                                           uint32_t tile_rows,
                                           uint32_t format,
                                           uint32_t target_width,
                                           uint32_t target_height,
                                           int32_t canvas_offset_x,
                                           int32_t canvas_offset_y,
                                           uint32_t canvas_width,
                                           uint32_t canvas_height)
{
	xdp->process_atlas(xdp, d3d11_context, atlas_srv, view_width, view_height, tile_columns, tile_rows, format,
	                    target_width, target_height, canvas_offset_x, canvas_offset_y, canvas_width,
	                    canvas_height);
}

/*!
 * @copydoc xrt_display_processor_d3d11::get_predicted_eye_positions
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_d3d11
 */
static inline bool
xrt_display_processor_d3d11_get_predicted_eye_positions(struct xrt_display_processor_d3d11 *xdp,
                                                        struct xrt_eye_positions *out_eye_pos)
{
	if (!XRT_DP_HAS_SLOT(xdp, get_predicted_eye_positions) || xdp->get_predicted_eye_positions == NULL) {
		return false;
	}
	return xdp->get_predicted_eye_positions(xdp, out_eye_pos);
}

/*!
 * @copydoc xrt_display_processor_d3d11::get_window_metrics
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_d3d11
 */
static inline bool
xrt_display_processor_d3d11_get_window_metrics(struct xrt_display_processor_d3d11 *xdp,
                                               struct xrt_window_metrics *out_metrics)
{
	if (!XRT_DP_HAS_SLOT(xdp, get_window_metrics) || xdp->get_window_metrics == NULL) {
		return false;
	}
	return xdp->get_window_metrics(xdp, out_metrics);
}

/*!
 * @copydoc xrt_display_processor_d3d11::request_display_mode
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_d3d11
 */
static inline bool
xrt_display_processor_d3d11_request_display_mode(struct xrt_display_processor_d3d11 *xdp, bool enable_3d)
{
	if (!XRT_DP_HAS_SLOT(xdp, request_display_mode) || xdp->request_display_mode == NULL) {
		return false;
	}
	return xdp->request_display_mode(xdp, enable_3d);
}

/*!
 * @copydoc xrt_display_processor_d3d11::get_hardware_3d_state
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_d3d11
 */
static inline bool
xrt_display_processor_d3d11_get_hardware_3d_state(struct xrt_display_processor_d3d11 *xdp,
                                                  bool *out_is_3d)
{
	if (!XRT_DP_HAS_SLOT(xdp, get_hardware_3d_state) || xdp->get_hardware_3d_state == NULL) {
		return false;
	}
	return xdp->get_hardware_3d_state(xdp, out_is_3d);
}

/*!
 * @copydoc xrt_display_processor_d3d11::get_display_dimensions
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_d3d11
 */
static inline bool
xrt_display_processor_d3d11_get_display_dimensions(struct xrt_display_processor_d3d11 *xdp,
                                                   float *out_width_m,
                                                   float *out_height_m)
{
	if (!XRT_DP_HAS_SLOT(xdp, get_display_dimensions) || xdp->get_display_dimensions == NULL) {
		return false;
	}
	return xdp->get_display_dimensions(xdp, out_width_m, out_height_m);
}

/*!
 * @copydoc xrt_display_processor_d3d11::get_display_pixel_info
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_d3d11
 */
static inline bool
xrt_display_processor_d3d11_get_display_pixel_info(struct xrt_display_processor_d3d11 *xdp,
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
 * @copydoc xrt_display_processor_d3d11::is_alpha_native
 * Returns false if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_d3d11
 */
static inline bool
xrt_display_processor_d3d11_is_alpha_native(struct xrt_display_processor_d3d11 *xdp)
{
	if (!XRT_DP_HAS_SLOT(xdp, is_alpha_native) || xdp->is_alpha_native == NULL) {
		return false;
	}
	return xdp->is_alpha_native(xdp);
}

/*!
 * @copydoc xrt_display_processor_d3d11::set_chroma_key
 * No-op if not supported (function pointer is NULL).
 * @public @memberof xrt_display_processor_d3d11
 */
static inline void
xrt_display_processor_d3d11_set_chroma_key(struct xrt_display_processor_d3d11 *xdp,
                                            uint32_t key_color,
                                            bool transparent_bg_enabled)
{
	if (!XRT_DP_HAS_SLOT(xdp, set_chroma_key) || xdp->set_chroma_key == NULL) {
		return;
	}
	xdp->set_chroma_key(xdp, key_color, transparent_bg_enabled);
}

/*!
 * Factory function type for creating a D3D11 display processor.
 *
 * Called by the compositor to create a display processor for a session.
 * The factory is set by the target builder at init time and stored in
 * xrt_system_compositor_info.
 *
 * @param d3d11_device   D3D11 device (ID3D11Device*).
 * @param d3d11_context  D3D11 immediate context (ID3D11DeviceContext*).
 * @param window_handle  Native window handle (HWND), may be NULL.
 * @param[out] out_xdp   Created display processor on success.
 * @return XRT_SUCCESS on success.
 */
typedef xrt_result_t (*xrt_dp_factory_d3d11_fn_t)(void *d3d11_device,
                                                   void *d3d11_context,
                                                   void *window_handle,
                                                   struct xrt_display_processor_d3d11 **out_xdp);

/*!
 * Destroy an xrt_display_processor_d3d11 — helper function.
 *
 * @param[in,out] xdp_ptr  A pointer to your display processor pointer.
 *
 * Will destroy the processor if *xdp_ptr is not NULL.
 * Will then set *xdp_ptr to NULL.
 *
 * @public @memberof xrt_display_processor_d3d11
 */
static inline void
xrt_display_processor_d3d11_destroy(struct xrt_display_processor_d3d11 **xdp_ptr)
{
	struct xrt_display_processor_d3d11 *xdp = *xdp_ptr;
	if (xdp == NULL) {
		return;
	}

	xdp->destroy(xdp);
	*xdp_ptr = NULL;
}


#ifdef __cplusplus
}
#endif
