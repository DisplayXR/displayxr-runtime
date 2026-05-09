// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Leia GL display processor: wraps SR SDK GL weaver
 *         as an @ref xrt_display_processor_gl.
 *
 * The display processor owns the leiasr_gl handle — it creates it
 * via the factory function and destroys it on cleanup.
 *
 * The SR SDK weaver expects side-by-side (SBS) stereo input. The Leia
 * device defines its 3D mode as tile_columns=2, tile_rows=1, so the
 * compositor always delivers SBS. The compositor crop-blit guarantees
 * the atlas texture dimensions match exactly 2*view_width x view_height.
 *
 * @author David Fattal
 * @ingroup drv_leia
 */

#include "leia_display_processor_gl.h"
#include "leia_sr_gl.h"

#include "xrt/xrt_display_metrics.h"
#include "util/u_logging.h"

// GL types and functions — use glad via ogl_api.h (provides all GL symbols on Windows)
#include "xrt/xrt_windows.h"
#include "ogl/ogl_api.h"

#include <cstdlib>


// Default chroma key when the app didn't supply one (set_chroma_key key=0).
// Magenta — matches the D3D11/D3D12/VK DPs' kDefaultChromaKey for cross-API parity.
// Layout 0x00BBGGRR.
static constexpr uint32_t kDefaultChromaKey = 0x00FF00FF;

// Fullscreen-triangle vertex shader (3 vertices, no VBO).
static const char *kCkVertSrc = R"(#version 330 core
out vec2 v_uv;
void main() {
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    v_uv = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
)";

// Pre-weave fill shader: alpha=0 -> chroma key, alpha=1 -> unchanged.
// Output alpha forced to 1 so the SR weaver receives opaque RGB.
static const char *kCkFillFragSrc = R"(#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D src;
uniform vec3 chroma_rgb;
void main() {
    vec4 c = texture(src, v_uv);
    frag = vec4(mix(chroma_rgb, c.rgb, c.a), 1.0);
}
)";

// Post-weave strip shader: chroma-match -> alpha=0, else alpha=1, RGB
// premultiplied for DWM/DComp's premultiplied-alpha blend mode.
static const char *kCkStripFragSrc = R"(#version 330 core
in vec2 v_uv;
out vec4 frag;
uniform sampler2D src;
uniform vec3 chroma_rgb;
void main() {
    vec3 c = texture(src, v_uv).rgb;
    vec3 d = abs(c - chroma_rgb);
    bool match = max(max(d.r, d.g), d.b) < (1.0 / 512.0);
    float a = match ? 0.0 : 1.0;
    frag = vec4(c * a, a);
}
)";

/*!
 * Implementation struct wrapping leiasr_gl as xrt_display_processor_gl.
 */
struct leia_display_processor_gl_impl
{
	struct xrt_display_processor_gl base;
	struct leiasr_gl *leiasr; //!< Owned — destroyed in leia_dp_gl_destroy.

	GLuint read_fbo;     //!< Cached read FBO for 2D blit path.
	uint32_t view_count; //!< Active mode view count (1=2D, 2=stereo).

	//
	// Chroma-key transparency support (lazy-initialized on first frame).
	//
	bool ck_enabled;
	uint32_t ck_color;            //!< Effective key (kDefaultChromaKey if app passed 0).
	bool ck_inited;
	GLuint ck_program_fill;       //!< Pre-weave fill program.
	GLuint ck_program_strip;      //!< Post-weave strip program.
	GLint ck_fill_chroma_loc;
	GLint ck_strip_chroma_loc;
	GLint ck_fill_src_loc;
	GLint ck_strip_src_loc;

	GLuint ck_fill_fbo;           //!< FBO with ck_fill_tex attached.
	GLuint ck_fill_tex;           //!< RGBA8 fill target.
	uint32_t ck_fill_w, ck_fill_h;

	GLuint ck_strip_tex;          //!< RGBA8 sampling target for strip pass.
	GLuint ck_strip_blit_fbo;     //!< Read FBO with ck_strip_tex attached (target of glBlitFramebuffer).
	uint32_t ck_strip_w, ck_strip_h;

	GLuint ck_vao;                //!< Empty VAO required by core profile draws.
};

static inline struct leia_display_processor_gl_impl *
leia_dp_gl(struct xrt_display_processor_gl *xdp)
{
	return (struct leia_display_processor_gl_impl *)xdp;
}


/*
 *
 * Chroma-key fill/strip helpers (transparency support).
 *
 * Lazy-allocated on first frame the pass runs. ck_should_run() gates the
 * whole flow — when false (the common case) none of these execute and
 * process_atlas behaves identically to the pre-transparency path.
 *
 */

static bool
ck_should_run(struct leia_display_processor_gl_impl *ldp)
{
	return ldp->ck_enabled && ldp->ck_color != 0;
}

static void
ck_unpack_chroma_rgb(uint32_t color, float out_rgb[3])
{
	// 0x00BBGGRR layout matches D3D11/D3D12/VK DPs.
	uint8_t r = (uint8_t)((color >> 0) & 0xff);
	uint8_t g = (uint8_t)((color >> 8) & 0xff);
	uint8_t b = (uint8_t)((color >> 16) & 0xff);
	out_rgb[0] = (float)r / 255.0f;
	out_rgb[1] = (float)g / 255.0f;
	out_rgb[2] = (float)b / 255.0f;
}

static GLuint
ck_compile_shader(GLenum stage, const char *src)
{
	GLuint sh = glCreateShader(stage);
	glShaderSource(sh, 1, &src, NULL);
	glCompileShader(sh);
	GLint status = GL_FALSE;
	glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
	if (status != GL_TRUE) {
		char log[1024] = {0};
		GLsizei n = 0;
		glGetShaderInfoLog(sh, sizeof(log) - 1, &n, log);
		U_LOG_E("Leia GL DP: ck shader compile failed: %s", log);
		glDeleteShader(sh);
		return 0;
	}
	return sh;
}

static GLuint
ck_link_program(GLuint vs, GLuint fs)
{
	GLuint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);
	GLint status = GL_FALSE;
	glGetProgramiv(prog, GL_LINK_STATUS, &status);
	if (status != GL_TRUE) {
		char log[1024] = {0};
		GLsizei n = 0;
		glGetProgramInfoLog(prog, sizeof(log) - 1, &n, log);
		U_LOG_E("Leia GL DP: ck program link failed: %s", log);
		glDeleteProgram(prog);
		return 0;
	}
	return prog;
}

static bool
ck_init_pipeline(struct leia_display_processor_gl_impl *ldp)
{
	if (ldp->ck_inited) return true;

	GLuint vs = ck_compile_shader(GL_VERTEX_SHADER, kCkVertSrc);
	if (vs == 0) return false;
	GLuint fs_fill = ck_compile_shader(GL_FRAGMENT_SHADER, kCkFillFragSrc);
	if (fs_fill == 0) { glDeleteShader(vs); return false; }
	GLuint fs_strip = ck_compile_shader(GL_FRAGMENT_SHADER, kCkStripFragSrc);
	if (fs_strip == 0) { glDeleteShader(fs_fill); glDeleteShader(vs); return false; }

	ldp->ck_program_fill = ck_link_program(vs, fs_fill);
	ldp->ck_program_strip = ck_link_program(vs, fs_strip);

	glDeleteShader(fs_strip);
	glDeleteShader(fs_fill);
	glDeleteShader(vs);

	if (ldp->ck_program_fill == 0 || ldp->ck_program_strip == 0) {
		if (ldp->ck_program_fill) glDeleteProgram(ldp->ck_program_fill);
		if (ldp->ck_program_strip) glDeleteProgram(ldp->ck_program_strip);
		ldp->ck_program_fill = 0;
		ldp->ck_program_strip = 0;
		return false;
	}

	ldp->ck_fill_chroma_loc  = glGetUniformLocation(ldp->ck_program_fill,  "chroma_rgb");
	ldp->ck_strip_chroma_loc = glGetUniformLocation(ldp->ck_program_strip, "chroma_rgb");
	ldp->ck_fill_src_loc     = glGetUniformLocation(ldp->ck_program_fill,  "src");
	ldp->ck_strip_src_loc    = glGetUniformLocation(ldp->ck_program_strip, "src");

	// Empty VAO required by 3.3+ core profile.
	glGenVertexArrays(1, &ldp->ck_vao);

	ldp->ck_inited = true;
	U_LOG_W("Leia GL DP: chroma-key programs initialized (key=0x%06x)",
	        ldp->ck_color & 0x00FFFFFFu);
	return true;
}

// Recreate ck_fill_tex + ck_fill_fbo at (w, h). Returns false on failure.
static bool
ck_ensure_fill_target(struct leia_display_processor_gl_impl *ldp, uint32_t w, uint32_t h)
{
	if (ldp->ck_fill_tex != 0 && ldp->ck_fill_w == w && ldp->ck_fill_h == h) {
		return true;
	}

	if (ldp->ck_fill_fbo != 0) glDeleteFramebuffers(1, &ldp->ck_fill_fbo);
	if (ldp->ck_fill_tex != 0) glDeleteTextures(1, &ldp->ck_fill_tex);
	ldp->ck_fill_fbo = 0;
	ldp->ck_fill_tex = 0;

	glGenTextures(1, &ldp->ck_fill_tex);
	glBindTexture(GL_TEXTURE_2D, ldp->ck_fill_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
	             GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	// Strip pass uses GL_NEAREST too, but we sample the fill texture in the
	// weaver's input path — linear is fine there since fill produces opaque RGB.
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	glGenFramebuffers(1, &ldp->ck_fill_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, ldp->ck_fill_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                        GL_TEXTURE_2D, ldp->ck_fill_tex, 0);
	GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (st != GL_FRAMEBUFFER_COMPLETE) {
		U_LOG_E("Leia GL DP: ck fill FBO incomplete: 0x%x", (unsigned)st);
		return false;
	}

	ldp->ck_fill_w = w;
	ldp->ck_fill_h = h;
	return true;
}

// Recreate ck_strip_tex + ck_strip_blit_fbo at (w, h). Returns false on failure.
static bool
ck_ensure_strip_source(struct leia_display_processor_gl_impl *ldp, uint32_t w, uint32_t h)
{
	if (ldp->ck_strip_tex != 0 && ldp->ck_strip_w == w && ldp->ck_strip_h == h) {
		return true;
	}

	if (ldp->ck_strip_blit_fbo != 0) glDeleteFramebuffers(1, &ldp->ck_strip_blit_fbo);
	if (ldp->ck_strip_tex != 0) glDeleteTextures(1, &ldp->ck_strip_tex);
	ldp->ck_strip_blit_fbo = 0;
	ldp->ck_strip_tex = 0;

	glGenTextures(1, &ldp->ck_strip_tex);
	glBindTexture(GL_TEXTURE_2D, ldp->ck_strip_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h, 0,
	             GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);

	glGenFramebuffers(1, &ldp->ck_strip_blit_fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, ldp->ck_strip_blit_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	                        GL_TEXTURE_2D, ldp->ck_strip_tex, 0);
	GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (st != GL_FRAMEBUFFER_COMPLETE) {
		U_LOG_E("Leia GL DP: ck strip blit FBO incomplete: 0x%x", (unsigned)st);
		return false;
	}

	ldp->ck_strip_w = w;
	ldp->ck_strip_h = h;
	return true;
}

// Pre-weave fill: render atlas RGBA -> ck_fill_tex with alpha=0 pixels filled
// by the chroma key. Returns the GL texture id to feed the weaver as input,
// or 0 on failure.
static GLuint
ck_run_pre_weave_fill(struct leia_display_processor_gl_impl *ldp,
                      GLuint atlas_texture,
                      uint32_t atlas_w, uint32_t atlas_h)
{
	if (!ck_ensure_fill_target(ldp, atlas_w, atlas_h)) return 0;

	// Save state we'll trample.
	GLint prev_fbo = 0, prev_program = 0, prev_vao = 0;
	GLint prev_active_tex = 0, prev_tex2d = 0;
	GLint prev_viewport[4] = {0, 0, 0, 0};
	GLboolean prev_blend = GL_FALSE, prev_depth = GL_FALSE,
	          prev_cull = GL_FALSE, prev_scissor = GL_FALSE;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);
	glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active_tex);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex2d);
	glGetIntegerv(GL_VIEWPORT, prev_viewport);
	prev_blend = glIsEnabled(GL_BLEND);
	prev_depth = glIsEnabled(GL_DEPTH_TEST);
	prev_cull  = glIsEnabled(GL_CULL_FACE);
	prev_scissor = glIsEnabled(GL_SCISSOR_TEST);

	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);

	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ldp->ck_fill_fbo);
	glViewport(0, 0, (GLsizei)atlas_w, (GLsizei)atlas_h);

	glBindVertexArray(ldp->ck_vao);
	glUseProgram(ldp->ck_program_fill);

	float chroma[3] = {0.0f, 0.0f, 0.0f};
	ck_unpack_chroma_rgb(ldp->ck_color, chroma);
	if (ldp->ck_fill_chroma_loc >= 0) {
		glUniform3fv(ldp->ck_fill_chroma_loc, 1, chroma);
	}
	if (ldp->ck_fill_src_loc >= 0) {
		glUniform1i(ldp->ck_fill_src_loc, 0);
	}

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, atlas_texture);

	glDrawArrays(GL_TRIANGLES, 0, 3);

	// Restore state.
	glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex2d);
	glActiveTexture((GLenum)prev_active_tex);
	glUseProgram((GLuint)prev_program);
	glBindVertexArray((GLuint)prev_vao);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_fbo);
	glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
	if (prev_blend)   glEnable(GL_BLEND);
	if (prev_depth)   glEnable(GL_DEPTH_TEST);
	if (prev_cull)    glEnable(GL_CULL_FACE);
	if (prev_scissor) glEnable(GL_SCISSOR_TEST);

	return ldp->ck_fill_tex;
}

// Post-weave strip: copy the woven default framebuffer to ck_strip_tex via
// glBlitFramebuffer, then render the strip pass back into the default
// framebuffer with chroma-matching pixels set to alpha=0 (premultiplied).
//
// On entry: GL_DRAW_FRAMEBUFFER_BINDING is the target framebuffer (FBO 0
//           for the default framebuffer; or a DComp interop FBO).
// On exit:  same binding restored.
static void
ck_run_post_weave_strip(struct leia_display_processor_gl_impl *ldp,
                        uint32_t target_w, uint32_t target_h)
{
	if (!ck_ensure_strip_source(ldp, target_w, target_h)) return;

	// Save state.
	GLint prev_draw_fbo = 0, prev_read_fbo = 0, prev_program = 0, prev_vao = 0;
	GLint prev_active_tex = 0, prev_tex2d = 0;
	GLint prev_viewport[4] = {0, 0, 0, 0};
	GLboolean prev_blend = GL_FALSE, prev_depth = GL_FALSE,
	          prev_cull = GL_FALSE, prev_scissor = GL_FALSE;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw_fbo);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read_fbo);
	glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active_tex);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex2d);
	glGetIntegerv(GL_VIEWPORT, prev_viewport);
	prev_blend = glIsEnabled(GL_BLEND);
	prev_depth = glIsEnabled(GL_DEPTH_TEST);
	prev_cull  = glIsEnabled(GL_CULL_FACE);
	prev_scissor = glIsEnabled(GL_SCISSOR_TEST);

	// Step 1: blit current draw FBO -> ck_strip_blit_fbo. Read FBO is the
	// previous draw FBO (default framebuffer for the WGL path; DComp
	// interop FBO for the transparent path). Use NEAREST to avoid linear
	// blending across the chroma boundary.
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_draw_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, ldp->ck_strip_blit_fbo);
	glBlitFramebuffer(0, 0, (GLsizei)target_w, (GLsizei)target_h,
	                  0, 0, (GLsizei)target_w, (GLsizei)target_h,
	                  GL_COLOR_BUFFER_BIT, GL_NEAREST);

	// Step 2: render strip into the original target FBO sampling ck_strip_tex.
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prev_read_fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prev_draw_fbo);
	glViewport(0, 0, (GLsizei)target_w, (GLsizei)target_h);

	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);

	glBindVertexArray(ldp->ck_vao);
	glUseProgram(ldp->ck_program_strip);

	float chroma[3] = {0.0f, 0.0f, 0.0f};
	ck_unpack_chroma_rgb(ldp->ck_color, chroma);
	if (ldp->ck_strip_chroma_loc >= 0) {
		glUniform3fv(ldp->ck_strip_chroma_loc, 1, chroma);
	}
	if (ldp->ck_strip_src_loc >= 0) {
		glUniform1i(ldp->ck_strip_src_loc, 0);
	}

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, ldp->ck_strip_tex);

	glDrawArrays(GL_TRIANGLES, 0, 3);

	// Restore state.
	glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex2d);
	glActiveTexture((GLenum)prev_active_tex);
	glUseProgram((GLuint)prev_program);
	glBindVertexArray((GLuint)prev_vao);
	glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
	if (prev_blend)   glEnable(GL_BLEND);
	if (prev_depth)   glEnable(GL_DEPTH_TEST);
	if (prev_cull)    glEnable(GL_CULL_FACE);
	if (prev_scissor) glEnable(GL_SCISSOR_TEST);
}

static void
ck_release_resources(struct leia_display_processor_gl_impl *ldp)
{
	if (ldp->ck_fill_fbo != 0)       glDeleteFramebuffers(1, &ldp->ck_fill_fbo);
	if (ldp->ck_fill_tex != 0)       glDeleteTextures(1, &ldp->ck_fill_tex);
	if (ldp->ck_strip_blit_fbo != 0) glDeleteFramebuffers(1, &ldp->ck_strip_blit_fbo);
	if (ldp->ck_strip_tex != 0)      glDeleteTextures(1, &ldp->ck_strip_tex);
	if (ldp->ck_program_fill != 0)   glDeleteProgram(ldp->ck_program_fill);
	if (ldp->ck_program_strip != 0)  glDeleteProgram(ldp->ck_program_strip);
	if (ldp->ck_vao != 0)            glDeleteVertexArrays(1, &ldp->ck_vao);
	ldp->ck_fill_fbo = 0;
	ldp->ck_fill_tex = 0;
	ldp->ck_strip_blit_fbo = 0;
	ldp->ck_strip_tex = 0;
	ldp->ck_program_fill = 0;
	ldp->ck_program_strip = 0;
	ldp->ck_vao = 0;
	ldp->ck_inited = false;
}


/*
 *
 * xrt_display_processor_gl interface methods.
 *
 */

static void
leia_dp_gl_process_atlas(struct xrt_display_processor_gl *xdp,
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

	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);

	// 2D mode: bypass weaver, blit atlas content directly via glBlitFramebuffer
	if (ldp->view_count == 1) {
		// Lazily create the read FBO
		if (ldp->read_fbo == 0) {
			glGenFramebuffers(1, &ldp->read_fbo);
		}
		// Bind atlas to a temporary read FBO
		glBindFramebuffer(GL_READ_FRAMEBUFFER, ldp->read_fbo);
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                       GL_TEXTURE_2D, atlas_texture, 0);
		// Blit content region (single view) to full draw framebuffer
		glBlitFramebuffer(0, 0, (GLint)view_width, (GLint)view_height,
		                  0, 0, (GLint)target_width, (GLint)target_height,
		                  GL_COLOR_BUFFER_BIT, GL_LINEAR);
		// Restore read framebuffer
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		return;
	}

	// Atlas is guaranteed content-sized SBS (2*view_width x view_height)
	// by compositor crop-blit. Pass directly to weaver.

	// Chroma-key transparency: substitute atlas with an opaque-RGB version.
	GLuint weaver_input = atlas_texture;
	uint32_t atlas_w = view_width * tile_columns;
	uint32_t atlas_h = view_height * tile_rows;
	bool ck_active = ck_should_run(ldp);
	if (ck_active) {
		if (ck_init_pipeline(ldp)) {
			GLuint filled = ck_run_pre_weave_fill(ldp, atlas_texture, atlas_w, atlas_h);
			if (filled != 0) {
				weaver_input = filled;
			} else {
				ck_active = false;
			}
		} else {
			ck_active = false;
		}
	}

	leiasr_gl_set_input_texture(ldp->leiasr, weaver_input, view_width, view_height, format);

	// Restore target viewport — SR weaver reads glViewport at weave() time,
	// but set_input_texture may have reset it to input dimensions.
	glViewport(0, 0, target_width, target_height);

	// Perform weaving to the currently bound framebuffer
	leiasr_gl_weave(ldp->leiasr);

	if (ck_active) {
		ck_run_post_weave_strip(ldp, target_width, target_height);
	}
}

static bool
leia_dp_gl_is_alpha_native(struct xrt_display_processor_gl *xdp)
{
	(void)xdp;
	// SR GL weaver outputs opaque RGB; transparency requires the
	// chroma-key fill+strip trick implemented in this DP.
	return false;
}

static void
leia_dp_gl_set_chroma_key(struct xrt_display_processor_gl *xdp,
                          uint32_t key_color,
                          bool transparent_bg_enabled)
{
	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);
	ldp->ck_enabled = transparent_bg_enabled;
	ldp->ck_color = (key_color != 0) ? key_color : kDefaultChromaKey;

	if (transparent_bg_enabled) {
		U_LOG_W("Leia GL DP: chroma-key ENABLED (key=0x%06x %s)",
		        ldp->ck_color & 0x00FFFFFFu,
		        (key_color != 0) ? "— app override" : "— DP default magenta");
	} else {
		U_LOG_I("Leia GL DP: chroma-key disabled");
	}
}

static bool
leia_dp_gl_get_predicted_eye_positions(struct xrt_display_processor_gl *xdp,
                                        struct xrt_eye_positions *out_eye_pos)
{
	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);
	float left[3], right[3];
	if (!leiasr_gl_get_predicted_eye_positions(ldp->leiasr, left, right)) {
		return false;
	}
	out_eye_pos->eyes[0].x = left[0];
	out_eye_pos->eyes[0].y = left[1];
	out_eye_pos->eyes[0].z = left[2];
	out_eye_pos->eyes[1].x = right[0];
	out_eye_pos->eyes[1].y = right[1];
	out_eye_pos->eyes[1].z = right[2];
	out_eye_pos->count = 2;
	out_eye_pos->valid = true;
	out_eye_pos->is_tracking = true;
	// In 2D mode, average L/R to a single midpoint eye.
	if (ldp->view_count == 1 && out_eye_pos->count >= 2) {
		out_eye_pos->eyes[0].x = (out_eye_pos->eyes[0].x + out_eye_pos->eyes[1].x) * 0.5f;
		out_eye_pos->eyes[0].y = (out_eye_pos->eyes[0].y + out_eye_pos->eyes[1].y) * 0.5f;
		out_eye_pos->eyes[0].z = (out_eye_pos->eyes[0].z + out_eye_pos->eyes[1].z) * 0.5f;
		out_eye_pos->count = 1;
	}
	return true;
}

static bool
leia_dp_gl_get_window_metrics(struct xrt_display_processor_gl *xdp,
                               struct xrt_window_metrics *out_metrics)
{
	(void)xdp;
	(void)out_metrics;
	return false;
}

static bool
leia_dp_gl_request_display_mode(struct xrt_display_processor_gl *xdp, bool enable_3d)
{
	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);
	bool ok = leiasr_gl_request_display_mode(ldp->leiasr, enable_3d);
	if (ok) {
		ldp->view_count = enable_3d ? 2 : 1;
	}
	return ok;
}

static bool
leia_dp_gl_get_hardware_3d_state(struct xrt_display_processor_gl *xdp, bool *out_is_3d)
{
	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);
	return leiasr_gl_get_hardware_3d_state(ldp->leiasr, out_is_3d);
}

static bool
leia_dp_gl_get_display_dimensions(struct xrt_display_processor_gl *xdp,
                                   float *out_width_m,
                                   float *out_height_m)
{
	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);
	struct leiasr_display_dimensions dims = {};
	if (!leiasr_gl_get_display_dimensions(ldp->leiasr, &dims) || !dims.valid) {
		return false;
	}
	*out_width_m = dims.width_m;
	*out_height_m = dims.height_m;
	return true;
}

static bool
leia_dp_gl_get_display_pixel_info(struct xrt_display_processor_gl *xdp,
                                   uint32_t *out_pixel_width,
                                   uint32_t *out_pixel_height,
                                   int32_t *out_screen_left,
                                   int32_t *out_screen_top)
{
	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);
	float w_m, h_m; // unused but required by API
	return leiasr_gl_get_display_pixel_info(ldp->leiasr, out_pixel_width, out_pixel_height, out_screen_left,
	                                        out_screen_top, &w_m, &h_m);
}

static void
leia_dp_gl_destroy(struct xrt_display_processor_gl *xdp)
{
	struct leia_display_processor_gl_impl *ldp = leia_dp_gl(xdp);

	ck_release_resources(ldp);

	if (ldp->read_fbo != 0) {
		glDeleteFramebuffers(1, &ldp->read_fbo);
	}

	if (ldp->leiasr != NULL) {
		leiasr_gl_destroy(&ldp->leiasr);
	}
	free(ldp);
}


/*
 *
 * Helper to populate vtable entries on an impl struct.
 *
 */

static void
leia_dp_gl_init_vtable(struct leia_display_processor_gl_impl *ldp)
{
	ldp->base.process_atlas = leia_dp_gl_process_atlas;
	ldp->base.get_predicted_eye_positions = leia_dp_gl_get_predicted_eye_positions;
	ldp->base.get_window_metrics = leia_dp_gl_get_window_metrics;
	ldp->base.request_display_mode = leia_dp_gl_request_display_mode;
	ldp->base.get_hardware_3d_state = leia_dp_gl_get_hardware_3d_state;
	ldp->base.get_display_dimensions = leia_dp_gl_get_display_dimensions;
	ldp->base.get_display_pixel_info = leia_dp_gl_get_display_pixel_info;
	ldp->base.is_alpha_native = leia_dp_gl_is_alpha_native;
	ldp->base.set_chroma_key = leia_dp_gl_set_chroma_key;
	ldp->base.destroy = leia_dp_gl_destroy;
}


/*
 *
 * Factory function — matches xrt_dp_factory_gl_fn_t signature.
 *
 */

extern "C" xrt_result_t
leia_dp_factory_gl(void *window_handle,
                    struct xrt_display_processor_gl **out_xdp)
{
	// Create weaver — view dimensions are set per-frame via setInputViewTexture,
	// so we pass 0,0 here.
	struct leiasr_gl *weaver = NULL;
	xrt_result_t ret = leiasr_gl_create(5.0, window_handle, 0, 0, &weaver);
	if (ret != XRT_SUCCESS || weaver == NULL) {
		U_LOG_W("Failed to create SR GL weaver");
		return ret != XRT_SUCCESS ? ret : XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct leia_display_processor_gl_impl *ldp =
	    (struct leia_display_processor_gl_impl *)calloc(1, sizeof(*ldp));
	if (ldp == NULL) {
		leiasr_gl_destroy(&weaver);
		return XRT_ERROR_ALLOCATION;
	}

	leia_dp_gl_init_vtable(ldp);
	ldp->leiasr = weaver;
	ldp->view_count = 2;

	*out_xdp = &ldp->base;

	U_LOG_W("Created Leia SR GL display processor (factory, owns weaver)");

	return XRT_SUCCESS;
}
