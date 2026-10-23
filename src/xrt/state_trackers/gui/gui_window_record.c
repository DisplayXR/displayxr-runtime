// Copyright 2019-2021, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Recording window gui.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup gui
 */

#include "xrt/xrt_config_have.h"
#include "xrt/xrt_config_drivers.h"

#include "os/os_threading.h"

#include "math/m_api.h"

#include "util/u_var.h"
#include "util/u_misc.h"
#include "util/u_sink.h"
#include "util/u_file.h"
#include "util/u_json.h"
#include "util/u_frame.h"
#include "util/u_format.h"

#include "xrt/xrt_frame.h"
#include "xrt/xrt_tracking.h"
#include "xrt/xrt_frameserver.h"

#include "gui_window_record.h"

#include "gui_ogl.h"
#include "gui_imgui.h"
#include "gui_common.h"
#include "gui_window_record.h"

#include "stb_image_write.h"

#include <assert.h>


/*
 *
 * Misc helpers and interface functions.
 *
 */

static void
window_draw_misc(struct gui_record_window *rw)
{
	igSliderFloat("", &rw->texture.scale, 20.0, 300.f, "Scale %f%%", ImGuiSliderFlags_None);

	static ImVec2 button_dims = {0, 0};

	igSameLine(0.0f, 4.0f);
	bool minus = igButton("-", button_dims);

	igSameLine(0.0f, 4.0f);
	bool plus = igButton("+", button_dims);

	static const double scales[] = {
	    25.0, 50.0, 100.0, 200.0, 300.0,
	};

	if (plus) {
		for (uint32_t i = 0; i < ARRAY_SIZE(scales); i++) {
			if (rw->texture.scale >= scales[i]) {
				continue;
			}
			rw->texture.scale = scales[i];
			break;
		}
	} else if (minus) {
		// Initial length always greater then 0 so safe.
		for (uint32_t i = ARRAY_SIZE(scales); i-- > 0;) {
			if (rw->texture.scale <= scales[i]) {
				continue;
			}
			rw->texture.scale = scales[i];
			break;
		}
	}

	igSameLine(0, 30);

	igCheckbox("Rotate 180 degrees", &rw->texture.rotate_180);

	igSameLine(0, 30);

	igText("Sequence %u", (uint32_t)rw->texture.ogl->seq);
}

static void
window_frame(struct xrt_frame_sink *xfs, struct xrt_frame *xf)
{
	struct gui_record_window *rw = container_of(xfs, struct gui_record_window, sink);

	if (rw->source.width != xf->width || rw->source.height != xf->height || rw->source.format != xf->format) {
		if (rw->source.width != 0 || rw->source.height != 0) {
			U_LOG_E("Changing properties! Old: %ux%u:%s(%u), new %ux%u:%s(%u)", rw->source.width,
			        rw->source.height, u_format_str(rw->source.format), rw->source.format, xf->width,
			        xf->height, u_format_str(xf->format), xf->format);
		}
		assert(rw->source.width == 0 && rw->source.height == 0);

		rw->source.width = xf->width;
		rw->source.height = xf->height;
		rw->source.format = xf->format;
	}

	xrt_sink_push_frame(rw->texture.sink, xf);
}


/*
 *
 * 'Exported' functions.
 *
 */

bool
gui_window_record_init(struct gui_record_window *rw)
{
	// Basic init.
	rw->sink.push_frame = window_frame;

	// Setup the preview texture.
	// 50% scale.
	rw->texture.scale = 50.0;
	struct xrt_frame_sink *tmp = NULL;
	rw->texture.ogl = gui_ogl_sink_create("View", &rw->texture.xfctx, &tmp);
	u_sink_create_to_r8g8b8_r8g8b8a8_r8g8b8x8_or_l8(&rw->texture.xfctx, tmp, &tmp);
	u_sink_simple_queue_create(&rw->texture.xfctx, tmp, &rw->texture.sink);

	return true;
}

void
gui_window_record_to_background(struct gui_record_window *rw, struct gui_program *p)
{
	struct gui_ogl_texture *tex = rw->texture.ogl;

	gui_ogl_sink_update(tex);

	gui_ogl_draw_background(    //
	    (uint32_t)tex->w,       // width
	    (uint32_t)tex->h,       // height
	    tex->id,                // tex_id
	    rw->texture.rotate_180, // rotate_180
	    false);                 // flip_y
}

void
gui_window_record_render(struct gui_record_window *rw, struct gui_program *p)
{
	// Make all IDs unique.
	igPushID_Ptr(rw);

	gui_ogl_sink_update(rw->texture.ogl);

	window_draw_misc(rw);

	struct gui_ogl_texture *tex = rw->texture.ogl;

	gui_ogl_draw_image(             //
	    (uint32_t)tex->w,           // width
	    (uint32_t)tex->h,           // height
	    tex->id,                    // tex_id
	    rw->texture.scale / 100.0f, // scale
	    rw->texture.rotate_180,     // rotate_180
	    false);                     // flip_y

	// Pop the ID making everything unique.
	igPopID();
}

void
gui_window_record_close(struct gui_record_window *rw)
{
	xrt_frame_context_destroy_nodes(&rw->texture.xfctx);

	/*
	 * This is safe to do, because we require that our sink 'window_frame'
	 * function is not called when close is called.
	 */
	rw->texture.sink = NULL;
	rw->texture.ogl = NULL;
}
