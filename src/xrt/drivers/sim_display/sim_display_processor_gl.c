// Copyright 2025, Leia Inc.
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
    "    fragColor = vec4(left.r, right.g, right.b, 1.0);\n"
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


/*!
 * Implementation struct for the GL simulation display processor.
 */
struct sim_display_processor_gl
{
	struct xrt_display_processor_gl base;
	GLuint programs[5]; //!< One per output mode (SBS, anaglyph, blend, squeezed SBS, quad)
	GLuint vao_empty;   //!< Empty VAO for vertex-shader-generated fullscreen triangle

	//! Nominal viewer parameters for faked eye positions.
	float ipd_m;
	float nominal_x_m;
	float nominal_y_m;
	float nominal_z_m;
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
                          uint32_t target_height)
{
	struct sim_display_processor_gl *sdp = sim_dp_gl(xdp);
	(void)format;

	// Read the current mode (may change at runtime via 1/2/3 keys)
	enum sim_display_output_mode mode = sim_display_get_output_mode();
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

	// Compute UV scale from view/atlas dimensions (not 1/tile_columns)
	// so mapping is correct when atlas is larger than the tiled region.
	GLint atlas_w = 0, atlas_h = 0;
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &atlas_w);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &atlas_h);
	float tile_cols_inv = (atlas_w > 0) ? ((float)view_width / (float)atlas_w) : (1.0f / (float)tile_columns);
	float tile_rows_inv = (atlas_h > 0) ? ((float)view_height / (float)atlas_h) : (1.0f / (float)tile_rows);
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
	out->is_tracking = false; // Nominal, not real tracking
	return true;
}

static void
sim_dp_gl_destroy(struct xrt_display_processor_gl *xdp)
{
	struct sim_display_processor_gl *sdp = sim_dp_gl(xdp);

	for (int i = 0; i < 5; i++) {
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
 *
 * Exported creation function.
 *
 */

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

	sdp->base.destroy = sim_dp_gl_destroy;
	sdp->base.process_atlas = sim_dp_gl_process_atlas;
	sdp->base.get_predicted_eye_positions = sim_dp_gl_get_predicted_eye_positions;

	// Nominal viewer parameters (same defaults as sim_display_hmd_create)
	sdp->ipd_m = 0.06f;
	sdp->nominal_x_m = 0.0f;
	sdp->nominal_y_m = 0.1f;
	sdp->nominal_z_m = debug_get_float_option_sim_display_nominal_z_m_gl();

	// Compile all 5 shader programs for instant runtime switching
	const char *fs_sources[5] = {FS_SBS, FS_ANAGLYPH, FS_BLEND, FS_SQUEEZED_SBS, FS_QUAD};
	const char *mode_names[5] = {"SBS", "Anaglyph", "Blend", "Squeezed SBS", "Quad"};

	for (int i = 0; i < 5; i++) {
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

	U_LOG_W("Created sim display GL processor (all 5 shaders), initial mode: %s",
	        mode == SIM_DISPLAY_OUTPUT_SBS           ? "SBS" :
	        mode == SIM_DISPLAY_OUTPUT_ANAGLYPH       ? "Anaglyph" :
	        mode == SIM_DISPLAY_OUTPUT_SQUEEZED_SBS   ? "Squeezed SBS" :
	        mode == SIM_DISPLAY_OUTPUT_QUAD            ? "Quad" : "Blend");

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
