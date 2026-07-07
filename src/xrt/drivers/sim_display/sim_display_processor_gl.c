// Copyright 2025, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Simulation GL display processor: SBS, anaglyph, alpha-blend output.
 *
 * Implements SBS, anaglyph, alpha-blend, squeezed SBS, and quad atlas output
 * modes using GLSL shaders compiled at init. All 5 programs are pre-compiled
 * for instant runtime switching via 1/2/3/4/5 keys.
 *
 * @author David Fattal
 * @ingroup drv_sim_display
 */

#include "sim_display_interface.h"
#include "sim_display_zone_common.h"

#include "xrt/xrt_display_processor_gl.h"
#include "xrt/xrt_display_metrics.h"

#include "util/u_debug.h"
#include "util/u_logging.h"
#include "os/os_time.h"

#ifdef XRT_OS_WINDOWS
#include "ogl/ogl_api.h"
#elif defined(__APPLE__)
#include <OpenGL/gl3.h>
#else
#include "ogl/ogl_api.h"
#endif

#include <stdlib.h>
#include <string.h>

#if !defined(XRT_OS_WINDOWS) && !defined(__APPLE__)
// Desktop Linux: this plug-in .so carries its own copy of the GLAD dispatch
// table (aux_ogl/xrt-external-glad is an INTERFACE lib compiled into every
// consumer) and is dlopen'd RTLD_LOCAL, so the runtime's gladLoad never reaches
// it. Load our copy from the current GLX context before any gl* call. (#660)
extern void (*glXGetProcAddressARB(const unsigned char *procName))(void);
static GLADapiproc
sim_gl_glad_loader(void *userptr, const char *name)
{
	(void)userptr;
	return (GLADapiproc)glXGetProcAddressARB((const unsigned char *)name);
}
#endif

DEBUG_GET_ONCE_FLOAT_OPTION(sim_display_nominal_z_m_gl, "SIM_DISPLAY_NOMINAL_Z_M", 0.60f)


/*
 *
 * Embedded GLSL shader source.
 *
 */

static const char *VS_FULLSCREEN =
    "#version 330 core\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    float x = float((gl_VertexID & 1) << 2);\n"
    "    float y = float((gl_VertexID & 2) << 1);\n"
    "    v_uv = vec2(x * 0.5, y * 0.5);\n"
    "    gl_Position = vec4(x - 1.0, y - 1.0, 0.0, 1.0);\n"
    "}\n";

//! SBS with center crop: each eye rendered at full-display FOV,
//! crop center 50% horizontally to match half-display SBS layout.
static const char *FS_SBS =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D u_texture;\n"
    "uniform float u_tile_cols_inv;\n"
    "uniform float u_tile_rows_inv;\n"
    "uniform float u_tile_cols;\n"
    "uniform float u_tile_rows;\n"
    "void main() {\n"
    "    float x = v_uv.x;\n"
    "    float col_right = mod(1.0, u_tile_cols);\n"
    "    float row_right = floor(1.0 / u_tile_cols);\n"
    "    float src_u;\n"
    "    float src_v;\n"
    "    if (x < 0.5) {\n"
    "        float eye_u = x / 0.5;\n"
    "        src_u = (0.25 + eye_u * 0.5) * u_tile_cols_inv;\n"
    "        src_v = v_uv.y * u_tile_rows_inv;\n"
    "    } else {\n"
    "        float eye_u = (x - 0.5) / 0.5;\n"
    "        src_u = (0.25 + eye_u * 0.5 + col_right) * u_tile_cols_inv;\n"
    "        src_v = (v_uv.y + row_right) * u_tile_rows_inv;\n"
    "    }\n"
    "    fragColor = texture(u_texture, vec2(src_u, src_v));\n"
    "}\n";

//! Anaglyph: red from left eye, cyan from right eye.
static const char *FS_ANAGLYPH =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D u_texture;\n"
    "uniform float u_tile_cols_inv;\n"
    "uniform float u_tile_rows_inv;\n"
    "uniform float u_tile_cols;\n"
    "uniform float u_tile_rows;\n"
    "void main() {\n"
    "    vec2 uv_left = vec2(v_uv.x * u_tile_cols_inv, v_uv.y * u_tile_rows_inv);\n"
    "    float col = mod(1.0, u_tile_cols);\n"
    "    float row = floor(1.0 / u_tile_cols);\n"
    "    vec2 uv_right = vec2((v_uv.x + col) * u_tile_cols_inv, (v_uv.y + row) * u_tile_rows_inv);\n"
    "    vec4 left = texture(u_texture, uv_left);\n"
    "    vec4 right = texture(u_texture, uv_right);\n"
    // Preserve alpha (max of both eyes) so translucent/transparent-background
    // regions survive the anaglyph mix — parity with the VK anaglyph.frag and
    // the Metal anaglyph_fragment (issue #392 / #630). Forcing alpha to 1.0
    // here turned premultiplied translucent 3D-zone backgrounds opaque.
    "    fragColor = vec4(left.r, right.g, right.b, max(left.a, right.a));\n"
    "}\n";

//! Squeezed SBS: left tile on left half, right tile on right half, no crop.
static const char *FS_SQUEEZED_SBS =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D u_texture;\n"
    "uniform float u_tile_cols_inv;\n"
    "uniform float u_tile_rows_inv;\n"
    "uniform float u_tile_cols;\n"
    "uniform float u_tile_rows;\n"
    "void main() {\n"
    "    float x = v_uv.x;\n"
    "    float eye_index = (x < 0.5) ? 0.0 : 1.0;\n"
    "    float eye_u = (x < 0.5) ? (x / 0.5) : ((x - 0.5) / 0.5);\n"
    "    float col = mod(eye_index, u_tile_cols);\n"
    "    float row = floor(eye_index / u_tile_cols);\n"
    "    float src_u = (eye_u + col) * u_tile_cols_inv;\n"
    "    float src_v = (v_uv.y + row) * u_tile_rows_inv;\n"
    "    fragColor = texture(u_texture, vec2(src_u, src_v));\n"
    "}\n";

//! Quad: 2x2 grid — TL=view0, TR=view1, BL=view2, BR=view3.
static const char *FS_QUAD =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D u_texture;\n"
    "uniform float u_tile_cols_inv;\n"
    "uniform float u_tile_rows_inv;\n"
    "uniform float u_tile_cols;\n"
    "uniform float u_tile_rows;\n"
    "void main() {\n"
    "    float col_idx = (v_uv.x < 0.5) ? 0.0 : 1.0;\n"
    "    float row_idx = (v_uv.y < 0.5) ? 0.0 : 1.0;\n"
    "    float view_index = row_idx * 2.0 + col_idx;\n"
    "    float local_u = fract(v_uv.x * 2.0);\n"
    "    float local_v = fract(v_uv.y * 2.0);\n"
    "    float col = mod(view_index, u_tile_cols);\n"
    "    float row = floor(view_index / u_tile_cols);\n"
    "    float atlas_u = (local_u + col) * u_tile_cols_inv;\n"
    "    float atlas_v = (local_v + row) * u_tile_rows_inv;\n"
    "    fragColor = texture(u_texture, vec2(atlas_u, atlas_v));\n"
    "}\n";

//! 50/50 blend.
static const char *FS_BLEND =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D u_texture;\n"
    "uniform float u_tile_cols_inv;\n"
    "uniform float u_tile_rows_inv;\n"
    "uniform float u_tile_cols;\n"
    "uniform float u_tile_rows;\n"
    "void main() {\n"
    "    vec2 uv_left = vec2(v_uv.x * u_tile_cols_inv, v_uv.y * u_tile_rows_inv);\n"
    "    float col = mod(1.0, u_tile_cols);\n"
    "    float row = floor(1.0 / u_tile_cols);\n"
    "    vec2 uv_right = vec2((v_uv.x + col) * u_tile_cols_inv, (v_uv.y + row) * u_tile_rows_inv);\n"
    "    vec4 left = texture(u_texture, uv_left);\n"
    "    vec4 right = texture(u_texture, uv_right);\n"
    "    fragColor = mix(left, right, 0.5);\n"
    "}\n";

static const char *FS_PASSTHROUGH =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D u_texture;\n"
    "uniform float u_tile_cols_inv;\n"
    "uniform float u_tile_rows_inv;\n"
    "uniform float u_tile_cols;\n"
    "uniform float u_tile_rows;\n"
    "void main() {\n"
    "    vec2 atlas_uv = vec2(v_uv.x * u_tile_cols_inv, v_uv.y * u_tile_rows_inv);\n"
    "    fragColor = texture(u_texture, atlas_uv);\n"
    "}\n";


/*!
 * Implementation struct for the GL simulation display processor.
 */
struct sim_display_processor_gl
{
	struct xrt_display_processor_gl base;
	GLuint programs[6]; //!< One per output mode (SBS, anaglyph, blend, squeezed SBS, quad, passthrough)
	GLuint vao_empty;   //!< Empty VAO for vertex-shader-generated fullscreen triangle

	//! Nominal viewer parameters for faked eye positions.
	float ipd_m;
	float nominal_x_m;
	float nominal_y_m;
	float nominal_z_m;

	//! #224 / ADR-027 local-zone test double (GL port of the D3D11 triple).
	//! Shared env config + change-gated publish/clear logging; the dump
	//! readback is a same-context glGetTexImage (see the zone section).
	struct sim_zone_config zone_cfg;
	int32_t zone_last_x, zone_last_y;
	uint32_t zone_last_w, zone_last_h;
	uint32_t zone_last_mask_w, zone_last_mask_h;
	uint64_t zone_last_seq;
	bool zone_active; //!< A client mask is currently published (not cleared).
	char zone_last_map[SIM_ZONE_MAX_CELLS + 1]; //!< Last logged cell map.
};

static inline struct sim_display_processor_gl *
sim_dp_gl(struct xrt_display_processor_gl *xdp)
{
	return (struct sim_display_processor_gl *)xdp;
}


/*
 *
 * Fullscreen triangle with runtime-switchable fragment shader.
 *
 */

static void
sim_dp_gl_process_atlas(struct xrt_display_processor_gl *xdp,
                          uint32_t atlas_texture,
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
	// TODO(#85): Pass canvas_offset_x/y to vendor weaver for interlacing
	// phase correction once Leia SR SDK supports sub-rect offset.
	(void)canvas_offset_x;
	(void)canvas_offset_y;
	(void)canvas_width;
	(void)canvas_height;

	struct sim_display_processor_gl *sdp = sim_dp_gl(xdp);
	(void)format;

	// Read the current mode (may change at runtime via 1/2/3 keys).
	// Single-view input forces passthrough — 3D shaders need ≥2 views.
	enum sim_display_output_mode mode = sim_display_get_output_mode();
	if (tile_columns * tile_rows <= 1) {
		mode = SIM_DISPLAY_OUTPUT_PASSTHROUGH;
	}
	GLuint active_program = sdp->programs[mode];
	if (active_program == 0) {
		return;
	}

	glViewport(0, 0, (GLsizei)target_width, (GLsizei)target_height);

	glUseProgram(active_program);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, (GLuint)atlas_texture);
	GLint loc = glGetUniformLocation(active_program, "u_texture");
	glUniform1i(loc, 0);

	// Atlas is guaranteed content-sized by compositor crop-blit.
	float tile_cols_inv = (tile_columns > 0) ? (1.0f / (float)tile_columns) : 0.5f;
	float tile_rows_inv = (tile_rows > 0) ? (1.0f / (float)tile_rows) : 1.0f;
	GLint loc_cols = glGetUniformLocation(active_program, "u_tile_cols_inv");
	GLint loc_rows = glGetUniformLocation(active_program, "u_tile_rows_inv");
	GLint loc_tc = glGetUniformLocation(active_program, "u_tile_cols");
	GLint loc_tr = glGetUniformLocation(active_program, "u_tile_rows");
	glUniform1f(loc_cols, tile_cols_inv);
	glUniform1f(loc_rows, tile_rows_inv);
	glUniform1f(loc_tc, (float)tile_columns);
	glUniform1f(loc_tr, (float)tile_rows);

	glBindVertexArray(sdp->vao_empty);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glBindVertexArray(0);

	glUseProgram(0);
	glBindTexture(GL_TEXTURE_2D, 0);
}


static bool
sim_dp_gl_get_predicted_eye_positions(struct xrt_display_processor_gl *xdp,
                                       struct xrt_eye_positions *out)
{
	struct sim_display_processor_gl *sdp = sim_dp_gl(xdp);
	float half_ipd = sdp->ipd_m / 2.0f;
	uint32_t vc = sim_display_get_view_count();

	if (vc == 1) {
		out->eyes[0] = (struct xrt_eye_position){sdp->nominal_x_m, sdp->nominal_y_m, sdp->nominal_z_m};
		out->count = 1;
	} else if (vc >= 4) {
		out->eyes[0] = (struct xrt_eye_position){sdp->nominal_x_m - half_ipd, sdp->nominal_y_m - 0.032f, sdp->nominal_z_m};
		out->eyes[1] = (struct xrt_eye_position){sdp->nominal_x_m + half_ipd, sdp->nominal_y_m - 0.032f, sdp->nominal_z_m};
		out->eyes[2] = (struct xrt_eye_position){sdp->nominal_x_m - half_ipd, sdp->nominal_y_m + 0.032f, sdp->nominal_z_m};
		out->eyes[3] = (struct xrt_eye_position){sdp->nominal_x_m + half_ipd, sdp->nominal_y_m + 0.032f, sdp->nominal_z_m};
		out->count = 4;
	} else {
		out->eyes[0] = (struct xrt_eye_position){sdp->nominal_x_m - half_ipd, sdp->nominal_y_m, sdp->nominal_z_m};
		out->eyes[1] = (struct xrt_eye_position){sdp->nominal_x_m + half_ipd, sdp->nominal_y_m, sdp->nominal_z_m};
		out->count = 2;
	}
	out->timestamp_ns = os_monotonic_get_ns();
	out->valid = true;
	out->is_tracking = sim_display_fake_tracking_is_tracking(); // false unless SIM_DISPLAY_FAKE_TRACKING (#441)
	return true;
}

static void
sim_dp_gl_destroy(struct xrt_display_processor_gl *xdp)
{
	struct sim_display_processor_gl *sdp = sim_dp_gl(xdp);

	for (int i = 0; i < 6; i++) {
		if (sdp->programs[i] != 0) {
			glDeleteProgram(sdp->programs[i]);
		}
	}
	if (sdp->vao_empty != 0) {
		glDeleteVertexArrays(1, &sdp->vao_empty);
	}

	free(sdp);
}


/*
 *
 * Helper: compile GLSL shader and link program.
 *
 */

static GLuint
compile_shader(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);

	GLint status = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!status) {
		char log_buf[512];
		glGetShaderInfoLog(shader, sizeof(log_buf), NULL, log_buf);
		U_LOG_E("sim_display GL: shader compile error: %s", log_buf);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static GLuint
create_program(const char *vs_source, const char *fs_source)
{
	GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_source);
	if (vs == 0) {
		return 0;
	}

	GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_source);
	if (fs == 0) {
		glDeleteShader(vs);
		return 0;
	}

	GLuint program = glCreateProgram();
	glAttachShader(program, vs);
	glAttachShader(program, fs);
	glLinkProgram(program);

	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint status = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (!status) {
		char log_buf[512];
		glGetProgramInfoLog(program, sizeof(log_buf), NULL, log_buf);
		U_LOG_E("sim_display GL: program link error: %s", log_buf);
		glDeleteProgram(program);
		return 0;
	}

	return program;
}


/*
 * sim_display GL preserves alpha to the default framebuffer — the blend /
 * anaglyph / SBS fragment shaders write atlas alpha straight through.
 * Declare alpha-native so callers can route transparency requests
 * directly without chroma-key fallback.
 */
static bool
sim_dp_gl_is_alpha_native(struct xrt_display_processor_gl *xdp)
{
	(void)xdp;
	return true;
}

// #491 part 3 — test double: record the 2D-under backdrop handoff (no captured
// desktop to composite under). The one-shot WARN proves the runtime → DP
// set_background_2d wiring works without Leia hardware.
static void
sim_dp_gl_set_background_2d(struct xrt_display_processor_gl *xdp,
                            uint32_t background_tex,
                            uint32_t width,
                            uint32_t height)
{
	(void)xdp;
	if (background_tex != 0) {
		static bool logged = false;
		if (!logged) {
			logged = true;
			U_LOG_W("sim_display GL #491 part3: received 2D-under backdrop %ux%u via set_background_2d "
			        "(test double — handoff recorded, not composited)",
			        width, height);
		}
	}
}


/*
 *
 * Exported creation function.
 *
 */

/*
 *
 * #224 / ADR-027 local 2D/3D zones — GL port of the D3D11 test double.
 *
 * Change-gated log lines proving the runtime publishes the right wish at the
 * right screen anchor (CI-greppable). The publish runs with the runtime's GL
 * context current (per the vtable contract), so SIM_DISPLAY_ZONE_DUMP gets a
 * REAL content readback here: a same-context glGetTexImage of the published
 * R8 texture, OR-downsampled into the simulated cell grid via the shared
 * helper (SIM_DISPLAY_WISH_QUANTIZE=band collapses columns).
 *
 */

static bool
sim_dp_gl_get_local_zone_caps(struct xrt_display_processor_gl *xdp, struct xrt_dp_local_zone_caps *out_caps)
{
	struct sim_display_processor_gl *sdp = sim_dp_gl(xdp);
	if (out_caps == NULL || out_caps->struct_size < XRT_DP_LOCAL_ZONE_CAPS_SIZE_V1) {
		// V1 floor only — see the D3D11 variant for the append rationale.
		return false;
	}
	SIM_ZONE_FILL_CAPS(out_caps, &sdp->zone_cfg);
	return true;
}

static bool
sim_dp_gl_publish_local_zone_mask(struct xrt_display_processor_gl *xdp,
                                  uint32_t mask_tex,
                                  uint32_t mask_width,
                                  uint32_t mask_height,
                                  int32_t screen_x,
                                  int32_t screen_y,
                                  uint32_t screen_w,
                                  uint32_t screen_h,
                                  uint64_t seq)
{
	struct sim_display_processor_gl *sdp = sim_dp_gl(xdp);
	if (mask_tex == 0 || mask_width == 0 || mask_height == 0) {
		return false;
	}

	// Change-gated geometry log (seq ticks every frame — not a change).
	bool geo_changed = !sdp->zone_active || screen_x != sdp->zone_last_x || screen_y != sdp->zone_last_y ||
	                   screen_w != sdp->zone_last_w || screen_h != sdp->zone_last_h ||
	                   mask_width != sdp->zone_last_mask_w || mask_height != sdp->zone_last_mask_h;
	if (geo_changed) {
		U_LOG_W("SIM ZONE PUBLISH (GL): mask=%ux%u screen=(%d,%d %ux%u) grid=%ux%u seq=%llu", mask_width,
		        mask_height, screen_x, screen_y, screen_w, screen_h, sdp->zone_cfg.grid_w, sdp->zone_cfg.grid_h,
		        (unsigned long long)seq);
	}
	sdp->zone_active = true;
	sdp->zone_last_x = screen_x;
	sdp->zone_last_y = screen_y;
	sdp->zone_last_w = screen_w;
	sdp->zone_last_h = screen_h;
	sdp->zone_last_mask_w = mask_width;
	sdp->zone_last_mask_h = mask_height;
	sdp->zone_last_seq = seq;

	// Optional content readback (desktop GL: glGetTexImage is available —
	// this TU is excluded on Android/GLES).
	uint32_t cells = sdp->zone_cfg.grid_w * sdp->zone_cfg.grid_h;
	if (!sdp->zone_cfg.dump || cells == 0 || cells > SIM_ZONE_MAX_CELLS) {
		return true;
	}

	uint8_t *pixels = malloc((size_t)mask_width * mask_height);
	if (pixels == NULL) {
		return true;
	}
	GLint prev_tex = 0;
	GLint prev_pack = 4;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
	glGetIntegerv(GL_PACK_ALIGNMENT, &prev_pack);
	glBindTexture(GL_TEXTURE_2D, (GLuint)mask_tex);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, pixels);
	glPixelStorei(GL_PACK_ALIGNMENT, prev_pack);
	glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);

	char map[SIM_ZONE_MAX_CELLS + 1];
	if (sim_zone_downsample_map(pixels, mask_width, mask_width, mask_height, &sdp->zone_cfg, map) &&
	    strcmp(map, sdp->zone_last_map) != 0) {
		U_LOG_W("SIM ZONE CELLS (GL) [%ux%u]: %s (seq=%llu)", sdp->zone_cfg.grid_w, sdp->zone_cfg.grid_h, map,
		        (unsigned long long)seq);
		memcpy(sdp->zone_last_map, map, sizeof(map));
	}
	free(pixels);
	return true;
}

static bool
sim_dp_gl_clear_local_zone_mask(struct xrt_display_processor_gl *xdp)
{
	struct sim_display_processor_gl *sdp = sim_dp_gl(xdp);
	if (sdp->zone_active) {
		U_LOG_W("SIM ZONE CLEAR (GL): client contribution withdrawn (last seq=%llu)",
		        (unsigned long long)sdp->zone_last_seq);
	}
	sdp->zone_active = false;
	sdp->zone_last_map[0] = '\0';
	return true;
}


xrt_result_t
sim_display_processor_gl_create(enum sim_display_output_mode mode,
                                 struct xrt_display_processor_gl **out_xdp)
{
	if (out_xdp == NULL) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct sim_display_processor_gl *sdp = calloc(1, sizeof(*sdp));
	if (sdp == NULL) {
		return XRT_ERROR_ALLOCATION;
	}

#if !defined(XRT_OS_WINDOWS) && !defined(__APPLE__)
	// Load THIS .so's GLAD dispatch (see note at sim_gl_glad_loader). The caller
	// (comp_gl compositor) has made a GL context current before invoking us.
	if (gladLoadGLUserPtr(sim_gl_glad_loader, NULL) == 0) {
		U_LOG_E("sim_display GL: gladLoadGLUserPtr failed — no current GLX context?");
		free(sdp);
		return XRT_ERROR_VULKAN;
	}
#endif

	// ADR-020 rule 1: advertise the vtable size (calloc zeroed reserved_0).
	sdp->base.struct_size = (uint32_t)sizeof(struct xrt_display_processor_gl);
	sdp->base.destroy = sim_dp_gl_destroy;
	sdp->base.process_atlas = sim_dp_gl_process_atlas;
	sdp->base.get_predicted_eye_positions = sim_dp_gl_get_predicted_eye_positions;
	sdp->base.is_alpha_native = sim_dp_gl_is_alpha_native;
	sdp->base.set_background_2d = sim_dp_gl_set_background_2d; // #491 part 3
	sdp->base.get_local_zone_caps = sim_dp_gl_get_local_zone_caps;         // #224 / ADR-027
	sdp->base.publish_local_zone_mask = sim_dp_gl_publish_local_zone_mask; // #224 / ADR-027
	sdp->base.clear_local_zone_mask = sim_dp_gl_clear_local_zone_mask;     // #224 / ADR-027

	// #224 / ADR-027 zone test double config (shared parser).
	sim_zone_config_from_env(&sdp->zone_cfg, "GL");

	// Nominal viewer parameters (same defaults as sim_display_hmd_create)
	sdp->ipd_m = 0.06f;
	sdp->nominal_x_m = 0.0f;
	sdp->nominal_y_m = 0.1f;
	sdp->nominal_z_m = debug_get_float_option_sim_display_nominal_z_m_gl();

	// Compile all 6 shader programs for instant runtime switching
	const char *fs_sources[6] = {FS_SBS, FS_ANAGLYPH, FS_BLEND, FS_SQUEEZED_SBS, FS_QUAD, FS_PASSTHROUGH};
	const char *mode_names[6] = {"SBS", "Anaglyph", "Blend", "Squeezed SBS", "Quad", "Passthrough"};

	for (int i = 0; i < 6; i++) {
		sdp->programs[i] = create_program(VS_FULLSCREEN, fs_sources[i]);
		if (sdp->programs[i] == 0) {
			U_LOG_E("sim_display GL: failed to create %s program", mode_names[i]);
			sim_dp_gl_destroy(&sdp->base);
			return XRT_ERROR_VULKAN;
		}
	}

	// Empty VAO for fullscreen triangle
	glGenVertexArrays(1, &sdp->vao_empty);

	// Set the initial output mode (atomic global read by process_atlas each frame)
	sim_display_set_output_mode(mode);

	U_LOG_W("Created sim display GL processor (all 6 shaders), initial mode: %s",
	        mode == SIM_DISPLAY_OUTPUT_SBS           ? "SBS" :
	        mode == SIM_DISPLAY_OUTPUT_ANAGLYPH       ? "Anaglyph" :
	        mode == SIM_DISPLAY_OUTPUT_SQUEEZED_SBS   ? "Squeezed SBS" :
	        mode == SIM_DISPLAY_OUTPUT_QUAD           ? "Quad" :
	        mode == SIM_DISPLAY_OUTPUT_PASSTHROUGH    ? "Passthrough" : "Blend");

	*out_xdp = &sdp->base;
	return XRT_SUCCESS;
}


/*
 *
 * Factory function — matches xrt_dp_factory_gl_fn_t signature.
 *
 */

xrt_result_t
sim_display_dp_factory_gl(void *window_handle,
                           struct xrt_display_processor_gl **out_xdp)
{
	(void)window_handle;

	enum sim_display_output_mode mode = sim_display_get_output_mode();

	return sim_display_processor_gl_create(mode, out_xdp);
}
