// Copyright 2019-2021, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Recording window gui.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup gui
 */

#pragma once

#include "xrt/xrt_frame.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_config_have.h"


#ifdef __cplusplus
extern "C" {
#endif

struct xrt_frame_sink;
struct gui_program;
struct gui_ogl_texture;


struct gui_record_window
{
	struct xrt_frame_sink sink;

	struct
	{
		uint32_t width, height;
		enum xrt_format format;
	} source;

	struct
	{
		struct xrt_frame_context xfctx;

		float scale;
		bool rotate_180;

		struct xrt_frame_sink *sink;
		struct gui_ogl_texture *ogl;
	} texture;
};


/*!
 * Initialise a embeddable record window.
 */
bool
gui_window_record_init(struct gui_record_window *rw);

/*!
 * Renders all controls of a record window.
 */
void
gui_window_record_render(struct gui_record_window *rw, struct gui_program *p);

/*!
 * Draw the sink image as the background to the background of the render view.
 * Basically the main window in which all ImGui windows lives in, not to a
 * ImGui window.
 */
void
gui_window_record_to_background(struct gui_record_window *rw, struct gui_program *p);

/*!
 * Frees all resources associated with a record window. Make sure to only call
 * this function on the main gui thread, and that nothing is pushing into the
 * record windows sink.
 */
void
gui_window_record_close(struct gui_record_window *rw);


#ifdef __cplusplus
}
#endif
