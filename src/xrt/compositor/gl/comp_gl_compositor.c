// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Native OpenGL compositor — direct GL rendering, no interop.
 *
 * Creates GL texture swapchains, renders SBS stereo output, presents
 * to window. Supports Windows (WGL), Android (EGL), macOS (CGL).
 *
 * @author David Fattal
 * @ingroup comp_gl
 */

#include "comp_gl_compositor.h"

#include "util/comp_layer_accum.h"

#include "xrt/xrt_handles.h"
#include "xrt/xrt_config_build.h"
#include "xrt/xrt_config_os.h"
#include "xrt/xrt_limits.h"
#include "xrt/xrt_system.h"

#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_time.h"
#include "os/os_time.h"
#include "os/os_threading.h"
#include "math/m_api.h"

#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty_interface.h"
#endif

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// GL function loading via GLAD (cross-platform)
#ifdef XRT_OS_WINDOWS
#include "ogl/ogl_api.h"
#include "ogl/wgl_api.h"
#include <d3d11.h>
// GUID for ID3D11Texture2D (needed for OpenSharedResource in C)
static const IID IID_ID3D11Texture2D_local = {
    0x6f15aaf2, 0xd208, 0x4e89,
    {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};
#elif defined(XRT_OS_ANDROID)
#include "ogl/ogl_api.h"
#include "ogl/egl_api.h"
#elif defined(__APPLE__)
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#endif

/*
 * WGL_NV_DX_interop2 function types (loaded dynamically via wglGetProcAddress)
 */
#ifdef XRT_OS_WINDOWS
typedef BOOL(WINAPI *PFN_wglDXSetResourceShareHandleNV)(void *dxObject, HANDLE shareHandle);
typedef HANDLE(WINAPI *PFN_wglDXOpenDeviceNV)(void *dxDevice);
typedef BOOL(WINAPI *PFN_wglDXCloseDeviceNV)(HANDLE hDevice);
typedef HANDLE(WINAPI *PFN_wglDXRegisterObjectNV)(HANDLE hDevice, void *dxObject,
                                                   GLuint name, GLenum type, GLenum access);
typedef BOOL(WINAPI *PFN_wglDXUnregisterObjectNV)(HANDLE hDevice, HANDLE hObject);
typedef BOOL(WINAPI *PFN_wglDXLockObjectsNV)(HANDLE hDevice, GLint count, HANDLE *hObjects);
typedef BOOL(WINAPI *PFN_wglDXUnlockObjectsNV)(HANDLE hDevice, GLint count, HANDLE *hObjects);

#define WGL_ACCESS_READ_WRITE_NV 0x0001
#endif


/*
 *
 * Constants
 *
 */

#define GL_SWAPCHAIN_MAX_IMAGES 8
#define GL_MAX_LAYERS 16

// Default window dimensions
#define GL_DEFAULT_WIDTH 2560
#define GL_DEFAULT_HEIGHT 1440


/*
 *
 * GL swapchain
 *
 */

struct comp_gl_swapchain
{
	//! Must be first — state tracker casts to xrt_swapchain_gl to read images[].
	struct xrt_swapchain_gl base;

	GLuint textures[GL_SWAPCHAIN_MAX_IMAGES];
	uint32_t image_count;
	struct xrt_swapchain_create_info info;

	int32_t acquired_index;
	int32_t waited_index;
	uint32_t last_released_index;
};

static inline struct comp_gl_swapchain *
gl_swapchain(struct xrt_swapchain *xsc)
{
	return (struct comp_gl_swapchain *)xsc;
}


/*
 *
 * GLSL shaders (embedded)
 *
 */

static const char *VS_FULLSCREEN_QUAD =
    "#version 330 core\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    float x = float((gl_VertexID & 1) << 2);\n"
    "    float y = float((gl_VertexID & 2) << 1);\n"
    "    v_uv = vec2(x * 0.5, y * 0.5);\n"
    "    gl_Position = vec4(x - 1.0, y - 1.0, 0.0, 1.0);\n"
    "}\n";

//! Fragment shader: blit single texture to screen.
static const char *FS_BLIT =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D u_texture;\n"
    "uniform vec4 u_src_rect;\n" // x, y, w, h in normalized coords
    "void main() {\n"
    "    vec2 uv = u_src_rect.xy + v_uv * u_src_rect.zw;\n"
    "    fragColor = texture(u_texture, uv);\n"
    "}\n";

//! Fragment shader: SBS side-by-side output.
static const char *FS_SBS =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D u_texture;\n" // SBS stereo texture (left|right)
    "void main() {\n"
    "    fragColor = texture(u_texture, v_uv);\n"
    "}\n";

//! Fragment shader: anaglyph (red/cyan).
static const char *FS_ANAGLYPH =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D u_texture;\n" // SBS stereo texture
    "void main() {\n"
    "    vec2 uv_left = vec2(v_uv.x * 0.5, v_uv.y);\n"
    "    vec2 uv_right = vec2(v_uv.x * 0.5 + 0.5, v_uv.y);\n"
    "    vec4 left = texture(u_texture, uv_left);\n"
    "    vec4 right = texture(u_texture, uv_right);\n"
    "    fragColor = vec4(left.r, right.g, right.b, 1.0);\n"
    "}\n";

//! Fragment shader: 50/50 blend.
static const char *FS_BLEND =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D u_texture;\n" // SBS stereo texture
    "void main() {\n"
    "    vec2 uv_left = vec2(v_uv.x * 0.5, v_uv.y);\n"
    "    vec2 uv_right = vec2(v_uv.x * 0.5 + 0.5, v_uv.y);\n"
    "    vec4 left = texture(u_texture, uv_left);\n"
    "    vec4 right = texture(u_texture, uv_right);\n"
    "    fragColor = mix(left, right, 0.5);\n"
    "}\n";


/*
 *
 * GL compositor structure
 *
 */

//! Output mode for display processor.
enum gl_output_mode
{
	GL_OUTPUT_SBS = 0,
	GL_OUTPUT_ANAGLYPH = 1,
	GL_OUTPUT_BLEND = 2,
};

struct comp_gl_compositor
{
	//! Must be first — implements xrt_compositor_native.
	struct xrt_compositor_native base;

	// --- GL resources ---
	GLuint program_blit;      //!< Shader for blitting eye to SBS texture
	GLuint program_sbs;       //!< SBS pass-through
	GLuint program_anaglyph;  //!< Anaglyph red/cyan
	GLuint program_blend;     //!< 50/50 blend
	GLuint vao_empty;         //!< Empty VAO for vertex-shader-generated fullscreen quad
	GLuint fbo;               //!< Framebuffer for rendering into SBS texture
	GLuint stereo_texture;    //!< SBS stereo texture (width = 2*view_width)
	uint32_t view_width;
	uint32_t view_height;

	// --- Layer accumulation ---
	struct comp_layer_accum layer_accum;

	// --- Platform context ---
#ifdef XRT_OS_WINDOWS
	HWND hwnd;
	HDC hdc;
	HGLRC hglrc;        //!< Compositor's own GL context
	HGLRC app_hglrc;    //!< App's GL context (shared textures)
	bool owns_window;
#elif defined(XRT_OS_ANDROID)
	void *egl_display;   //!< EGLDisplay
	void *egl_context;   //!< EGLContext
	void *egl_surface;   //!< EGLSurface
#elif defined(__APPLE__)
	void *cgl_context;   //!< CGLContextObj
#endif

	// --- Shared texture (D3D11 interop via WGL_NV_DX_interop2, Windows only) ---
#ifdef XRT_OS_WINDOWS
	bool has_shared_texture;
	ID3D11Device *dx_device;
	ID3D11DeviceContext *dx_context;
	ID3D11Texture2D *dx_shared_texture;
	HANDLE dx_interop_device;      //!< wglDXOpenDeviceNV handle
	HANDLE dx_interop_object;      //!< wglDXRegisterObjectNV handle
	GLuint shared_gl_texture;      //!< GL texture mapped to D3D11 shared texture
	uint32_t shared_width;
	uint32_t shared_height;

	// WGL_NV_DX_interop2 function pointers
	PFN_wglDXOpenDeviceNV pfn_wglDXOpenDeviceNV;
	PFN_wglDXCloseDeviceNV pfn_wglDXCloseDeviceNV;
	PFN_wglDXRegisterObjectNV pfn_wglDXRegisterObjectNV;
	PFN_wglDXUnregisterObjectNV pfn_wglDXUnregisterObjectNV;
	PFN_wglDXLockObjectsNV pfn_wglDXLockObjectsNV;
	PFN_wglDXUnlockObjectsNV pfn_wglDXUnlockObjectsNV;
#endif

	// --- State ---
	enum gl_output_mode output_mode;
	uint64_t last_frame_ns;
	struct xrt_device *xdev;
	struct xrt_system_devices *xsysd;
	bool sys_info_set;
	struct xrt_system_compositor_info sys_info;
};

static inline struct comp_gl_compositor *
gl_comp(struct xrt_compositor *xc)
{
	return (struct comp_gl_compositor *)xc;
}


/*
 *
 * GL helpers
 *
 */

static GLuint
compile_shader(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);

	GLint ok = 0;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log_buf[512];
		glGetShaderInfoLog(shader, sizeof(log_buf), NULL, log_buf);
		U_LOG_E("Shader compile error: %s", log_buf);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static GLuint
create_program(const char *vs_src, const char *fs_src)
{
	GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
	GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
	if (!vs || !fs) {
		if (vs) glDeleteShader(vs);
		if (fs) glDeleteShader(fs);
		return 0;
	}

	GLuint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);
	glDeleteShader(vs);
	glDeleteShader(fs);

	GLint ok = 0;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log_buf[512];
		glGetProgramInfoLog(prog, sizeof(log_buf), NULL, log_buf);
		U_LOG_E("Program link error: %s", log_buf);
		glDeleteProgram(prog);
		return 0;
	}
	return prog;
}

static GLenum
xrt_format_to_gl_internal(int64_t fmt)
{
	// GL internal format enums
	switch (fmt) {
	case 0x8058: return GL_RGBA8;          // GL_RGBA8
	case 0x8C43: return GL_SRGB8_ALPHA8;   // GL_SRGB8_ALPHA8
	case 0x881A: return GL_RGBA16F;         // GL_RGBA16F
	case 0x8814: return GL_RGBA32F;         // GL_RGBA32F
	default:     return GL_RGBA8;
	}
}


/*
 *
 * Swapchain functions
 *
 */

static void
gl_swapchain_destroy(struct xrt_swapchain *xsc)
{
	struct comp_gl_swapchain *sc = gl_swapchain(xsc);
	if (sc->image_count > 0) {
		glDeleteTextures(sc->image_count, sc->textures);
	}
	free(sc);
}

static xrt_result_t
gl_swapchain_acquire_image(struct xrt_swapchain *xsc, uint32_t *out_index)
{
	struct comp_gl_swapchain *sc = gl_swapchain(xsc);
	uint32_t next = (sc->last_released_index + 1) % sc->image_count;
	sc->acquired_index = (int32_t)next;
	*out_index = next;
	return XRT_SUCCESS;
}

static xrt_result_t
gl_swapchain_wait_image(struct xrt_swapchain *xsc, int64_t timeout_ns, uint32_t index)
{
	struct comp_gl_swapchain *sc = gl_swapchain(xsc);
	sc->waited_index = (int32_t)index;
	return XRT_SUCCESS;
}

static xrt_result_t
gl_swapchain_release_image(struct xrt_swapchain *xsc, uint32_t index)
{
	struct comp_gl_swapchain *sc = gl_swapchain(xsc);
	sc->last_released_index = index;
	sc->acquired_index = -1;
	sc->waited_index = -1;
	return XRT_SUCCESS;
}

static xrt_result_t
gl_swapchain_barrier_image(struct xrt_swapchain *xsc,
                           enum xrt_barrier_direction direction,
                           uint32_t index)
{
	(void)xsc;
	(void)direction;
	(void)index;
	return XRT_SUCCESS;
}

static xrt_result_t
gl_swapchain_inc_image_use(struct xrt_swapchain *xsc, uint32_t index)
{
	(void)xsc;
	(void)index;
	return XRT_SUCCESS;
}

static xrt_result_t
gl_swapchain_dec_image_use(struct xrt_swapchain *xsc, uint32_t index)
{
	(void)xsc;
	(void)index;
	return XRT_SUCCESS;
}


/*
 *
 * Compositor functions
 *
 */

static xrt_result_t
gl_compositor_get_swapchain_create_properties(struct xrt_compositor *xc,
                                               const struct xrt_swapchain_create_info *info,
                                               struct xrt_swapchain_create_properties *xsccp)
{
	xsccp->image_count = 3;
	xsccp->extra_bits = (enum xrt_swapchain_usage_bits)0;
	return XRT_SUCCESS;
}

static xrt_result_t
gl_compositor_create_swapchain(struct xrt_compositor *xc,
                                const struct xrt_swapchain_create_info *info,
                                struct xrt_swapchain **out_xsc)
{
	struct comp_gl_compositor *c = gl_comp(xc);
	(void)c;

	uint32_t image_count = 3;
	if (image_count > GL_SWAPCHAIN_MAX_IMAGES) {
		image_count = GL_SWAPCHAIN_MAX_IMAGES;
	}

	struct comp_gl_swapchain *sc = U_TYPED_CALLOC(struct comp_gl_swapchain);
	sc->image_count = image_count;
	sc->info = *info;
	sc->acquired_index = -1;
	sc->waited_index = -1;
	sc->last_released_index = 0;

	// Create GL textures
	GLenum internal_format = xrt_format_to_gl_internal(info->format);
	glGenTextures(image_count, sc->textures);

	for (uint32_t i = 0; i < image_count; i++) {
		glBindTexture(GL_TEXTURE_2D, sc->textures[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, internal_format,
		             info->width, info->height, 0,
		             GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		// Store GL texture name in the swapchain_gl images array
		// (this is what the state tracker reads via xrt_swapchain_gl)
		sc->base.images[i] = sc->textures[i];
	}
	glBindTexture(GL_TEXTURE_2D, 0);

	// Set up vtable
	sc->base.base.destroy = gl_swapchain_destroy;
	sc->base.base.acquire_image = gl_swapchain_acquire_image;
	sc->base.base.wait_image = gl_swapchain_wait_image;
	sc->base.base.release_image = gl_swapchain_release_image;
	sc->base.base.barrier_image = gl_swapchain_barrier_image;
	sc->base.base.inc_image_use = gl_swapchain_inc_image_use;
	sc->base.base.dec_image_use = gl_swapchain_dec_image_use;
	sc->base.base.image_count = image_count;
	sc->base.base.reference.count = 1;

	*out_xsc = &sc->base.base;

	U_LOG_W("Created GL swapchain: %ux%u, %u images, format 0x%x",
	         info->width, info->height, image_count, (unsigned)info->format);

	return XRT_SUCCESS;
}

static xrt_result_t
gl_compositor_begin_session(struct xrt_compositor *xc, const struct xrt_begin_session_info *info)
{
	(void)xc;
	(void)info;
	return XRT_SUCCESS;
}

static xrt_result_t
gl_compositor_end_session(struct xrt_compositor *xc)
{
	(void)xc;
	return XRT_SUCCESS;
}

static xrt_result_t
gl_compositor_predict_frame(struct xrt_compositor *xc,
                             int64_t *out_frame_id,
                             int64_t *out_wake_time_ns,
                             int64_t *out_predicted_gpu_time_ns,
                             int64_t *out_predicted_display_time_ns,
                             int64_t *out_predicted_display_period_ns)
{
	int64_t now_ns = (int64_t)os_monotonic_get_ns();
	int64_t period_ns = (int64_t)(1000000000.0 / 60.0); // 60 Hz

	static int64_t frame_id = 0;
	*out_frame_id = ++frame_id;
	*out_wake_time_ns = now_ns;
	*out_predicted_gpu_time_ns = now_ns + period_ns / 2;
	*out_predicted_display_time_ns = now_ns + period_ns;
	*out_predicted_display_period_ns = period_ns;

	return XRT_SUCCESS;
}

static xrt_result_t
gl_compositor_mark_frame(struct xrt_compositor *xc,
                          int64_t frame_id,
                          enum xrt_compositor_frame_point point,
                          int64_t when_ns)
{
	(void)xc;
	(void)frame_id;
	(void)point;
	(void)when_ns;
	return XRT_SUCCESS;
}

static xrt_result_t
gl_compositor_wait_frame(struct xrt_compositor *xc,
                          int64_t *out_frame_id,
                          int64_t *out_predicted_display_time,
                          int64_t *out_predicted_display_period)
{
	int64_t frame_id, wake, gpu_time;
	return gl_compositor_predict_frame(xc, &frame_id, &wake, &gpu_time,
	                                    out_predicted_display_time,
	                                    out_predicted_display_period);
}

static xrt_result_t
gl_compositor_begin_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	(void)xc;
	(void)frame_id;
	return XRT_SUCCESS;
}

static xrt_result_t
gl_compositor_discard_frame(struct xrt_compositor *xc, int64_t frame_id)
{
	(void)xc;
	(void)frame_id;
	return XRT_SUCCESS;
}


/*
 *
 * Layer functions
 *
 */

static xrt_result_t
gl_compositor_layer_begin(struct xrt_compositor *xc, const struct xrt_layer_frame_data *data)
{
	struct comp_gl_compositor *c = gl_comp(xc);
	comp_layer_accum_begin(&c->layer_accum, data);
	return XRT_SUCCESS;
}

static xrt_result_t
gl_compositor_layer_projection(struct xrt_compositor *xc,
                                struct xrt_device *xdev,
                                struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                const struct xrt_layer_data *data)
{
	struct comp_gl_compositor *c = gl_comp(xc);
	comp_layer_accum_projection(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
gl_compositor_layer_projection_depth(struct xrt_compositor *xc,
                                      struct xrt_device *xdev,
                                      struct xrt_swapchain *xsc[XRT_MAX_VIEWS],
                                      struct xrt_swapchain *d_xsc[XRT_MAX_VIEWS],
                                      const struct xrt_layer_data *data)
{
	struct comp_gl_compositor *c = gl_comp(xc);
	comp_layer_accum_projection_depth(&c->layer_accum, xsc, d_xsc, data);
	return XRT_SUCCESS;
}

static xrt_result_t
gl_compositor_layer_quad(struct xrt_compositor *xc,
                          struct xrt_device *xdev,
                          struct xrt_swapchain *xsc,
                          const struct xrt_layer_data *data)
{
	struct comp_gl_compositor *c = gl_comp(xc);
	comp_layer_accum_quad(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}


/*
 *
 * Layer commit — render SBS and present
 *
 */

static xrt_result_t
gl_compositor_layer_commit(struct xrt_compositor *xc, xrt_graphics_sync_handle_t sync_handle)
{
	struct comp_gl_compositor *c = gl_comp(xc);

	// Frame timing
	uint64_t now_ns = os_monotonic_get_ns();
	c->last_frame_ns = now_ns;

	if (c->layer_accum.layer_count == 0) {
		return XRT_SUCCESS;
	}

	// --- Step 1: Render layers into SBS stereo texture ---
	glBindFramebuffer(GL_FRAMEBUFFER, c->fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                        c->stereo_texture, 0);

	glViewport(0, 0, c->view_width * 2, c->view_height);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(c->program_blit);
	glBindVertexArray(c->vao_empty);

	GLint loc_tex = glGetUniformLocation(c->program_blit, "u_texture");
	GLint loc_rect = glGetUniformLocation(c->program_blit, "u_src_rect");

	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		struct comp_layer *layer = &c->layer_accum.layers[i];

		if (layer->data.type != XRT_LAYER_PROJECTION &&
		    layer->data.type != XRT_LAYER_PROJECTION_DEPTH) {
			continue;
		}

		for (uint32_t eye = 0; eye < 2; eye++) {
			struct xrt_swapchain *sc = layer->sc_array[eye];
			if (sc == NULL) {
				continue;
			}

			struct comp_gl_swapchain *gsc = gl_swapchain(sc);
			uint32_t img_idx = layer->data.proj.v[eye].sub.image_index;
			if (img_idx >= gsc->image_count) {
				continue;
			}

			// Source rect from layer
			struct xrt_normalized_rect nr = layer->data.proj.v[eye].sub.norm_rect;
			if (nr.w <= 0.0f || nr.h <= 0.0f) {
				nr.x = 0.0f;
				nr.y = 0.0f;
				nr.w = 1.0f;
				nr.h = 1.0f;
			}

			// Set viewport for this eye in the SBS texture
			glViewport(eye * c->view_width, 0, c->view_width, c->view_height);

			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, gsc->textures[img_idx]);
			glUniform1i(loc_tex, 0);
			glUniform4f(loc_rect, nr.x, nr.y, nr.w, nr.h);

			// Draw fullscreen quad (3 vertices, generated in vertex shader)
			glDrawArrays(GL_TRIANGLES, 0, 3);
		}
	}

	// --- Step 2: Present SBS texture ---
#ifdef XRT_OS_WINDOWS
	if (c->has_shared_texture) {
		// Shared texture mode: blit SBS stereo texture to the DX-interop GL texture
		if (c->pfn_wglDXLockObjectsNV(c->dx_interop_device, 1, &c->dx_interop_object)) {
			glBindFramebuffer(GL_FRAMEBUFFER, c->fbo);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
			                        c->shared_gl_texture, 0);

			glViewport(0, 0, c->shared_width, c->shared_height);

			// Select output shader based on display mode
			GLuint output_program;
			switch (c->output_mode) {
			case GL_OUTPUT_ANAGLYPH: output_program = c->program_anaglyph; break;
			case GL_OUTPUT_BLEND:    output_program = c->program_blend; break;
			default:                 output_program = c->program_sbs; break;
			}

			glUseProgram(output_program);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, c->stereo_texture);
			GLint loc_out_tex = glGetUniformLocation(output_program, "u_texture");
			glUniform1i(loc_out_tex, 0);
			glDrawArrays(GL_TRIANGLES, 0, 3);

			glFlush();

			// Restore FBO to not target shared texture
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
			glBindFramebuffer(GL_FRAMEBUFFER, 0);

			c->pfn_wglDXUnlockObjectsNV(c->dx_interop_device, 1, &c->dx_interop_object);
		}
	} else
#endif
	{
		// Normal window mode: present to screen
		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		GLuint output_program;
		switch (c->output_mode) {
		case GL_OUTPUT_ANAGLYPH: output_program = c->program_anaglyph; break;
		case GL_OUTPUT_BLEND:    output_program = c->program_blend; break;
		default:                 output_program = c->program_sbs; break;
		}

		glViewport(0, 0, c->view_width * 2, c->view_height);
		glUseProgram(output_program);

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, c->stereo_texture);
		GLint loc_out_tex = glGetUniformLocation(output_program, "u_texture");
		glUniform1i(loc_out_tex, 0);

		glDrawArrays(GL_TRIANGLES, 0, 3);

		// Platform-specific swap
#ifdef XRT_OS_WINDOWS
		SwapBuffers(c->hdc);
#elif defined(XRT_OS_ANDROID)
		// eglSwapBuffers(c->egl_display, c->egl_surface);
#elif defined(__APPLE__)
		// CGLFlushDrawable(c->cgl_context);
#endif
	}

	glBindVertexArray(0);
	glUseProgram(0);

	return XRT_SUCCESS;
}


/*
 *
 * Compositor destroy
 *
 */

static void
gl_compositor_destroy(struct xrt_compositor *xc)
{
	struct comp_gl_compositor *c = gl_comp(xc);

	if (c->program_blit) glDeleteProgram(c->program_blit);
	if (c->program_sbs) glDeleteProgram(c->program_sbs);
	if (c->program_anaglyph) glDeleteProgram(c->program_anaglyph);
	if (c->program_blend) glDeleteProgram(c->program_blend);
	if (c->vao_empty) glDeleteVertexArrays(1, &c->vao_empty);
	if (c->fbo) glDeleteFramebuffers(1, &c->fbo);
	if (c->stereo_texture) glDeleteTextures(1, &c->stereo_texture);

#ifdef XRT_OS_WINDOWS
	// Clean up D3D11 interop resources
	if (c->has_shared_texture) {
		if (c->dx_interop_object && c->pfn_wglDXUnregisterObjectNV) {
			c->pfn_wglDXUnregisterObjectNV(c->dx_interop_device, c->dx_interop_object);
		}
		if (c->shared_gl_texture) {
			glDeleteTextures(1, &c->shared_gl_texture);
		}
		if (c->dx_interop_device && c->pfn_wglDXCloseDeviceNV) {
			c->pfn_wglDXCloseDeviceNV(c->dx_interop_device);
		}
		if (c->dx_shared_texture) {
			c->dx_shared_texture->lpVtbl->Release(c->dx_shared_texture);
		}
		if (c->dx_context) {
			c->dx_context->lpVtbl->Release(c->dx_context);
		}
		if (c->dx_device) {
			c->dx_device->lpVtbl->Release(c->dx_device);
		}
	}

	if (c->hglrc) {
		wglMakeCurrent(NULL, NULL);
		wglDeleteContext(c->hglrc);
	}
	if (c->owns_window && c->hwnd) {
		DestroyWindow(c->hwnd);
	}
#endif

	free(c);
}


/*
 *
 * Supported formats
 *
 */

static void
gl_compositor_set_formats(struct comp_gl_compositor *c)
{
	// GL format enum values
	c->base.base.info.format_count = 4;
	c->base.base.info.formats[0] = 0x8058; // GL_RGBA8
	c->base.base.info.formats[1] = 0x8C43; // GL_SRGB8_ALPHA8
	c->base.base.info.formats[2] = 0x881A; // GL_RGBA16F
	c->base.base.info.formats[3] = 0x8814; // GL_RGBA32F
}


/*
 *
 * Platform-specific window/context creation
 *
 */

#ifdef XRT_OS_WINDOWS

//! GLAD loader: try wglGetProcAddress first, fall back to GetProcAddress on opengl32.dll.
static GLADapiproc
gl_get_proc_addr(void *userptr, const char *name)
{
	GLADapiproc ret = (GLADapiproc)wglGetProcAddress(name);
	if (ret == NULL) {
		ret = (GLADapiproc)GetProcAddress((HMODULE)userptr, name);
	}
	return ret;
}

static const wchar_t GL_WINDOW_CLASS[] = L"MonadoGLCompositor";

static LRESULT CALLBACK
gl_window_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	case WM_CLOSE:
		return 0; // Prevent close
	default:
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}
}

static bool
gl_create_window_and_context(struct comp_gl_compositor *c,
                              void *window_handle,
                              void *app_gl_context)
{
	// Register window class
	WNDCLASSEXW wc = {0};
	wc.cbSize = sizeof(wc);
	wc.style = CS_OWNDC;
	wc.lpfnWndProc = gl_window_proc;
	wc.hInstance = GetModuleHandleW(NULL);
	wc.lpszClassName = GL_WINDOW_CLASS;
	RegisterClassExW(&wc);

	if (window_handle != NULL) {
		c->hwnd = (HWND)window_handle;
		c->owns_window = false;
	} else {
		c->hwnd = CreateWindowExW(
		    0, GL_WINDOW_CLASS, L"OpenXR GL Compositor",
		    WS_OVERLAPPEDWINDOW | WS_VISIBLE,
		    CW_USEDEFAULT, CW_USEDEFAULT,
		    GL_DEFAULT_WIDTH, GL_DEFAULT_HEIGHT,
		    NULL, NULL, GetModuleHandleW(NULL), NULL);
		c->owns_window = true;
	}

	if (c->hwnd == NULL) {
		U_LOG_E("Failed to create window");
		return false;
	}

	c->hdc = GetDC(c->hwnd);

	// Set pixel format
	PIXELFORMATDESCRIPTOR pfd = {0};
	pfd.nSize = sizeof(pfd);
	pfd.nVersion = 1;
	pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
	pfd.iPixelType = PFD_TYPE_RGBA;
	pfd.cColorBits = 32;
	pfd.cDepthBits = 24;
	int pf = ChoosePixelFormat(c->hdc, &pfd);
	SetPixelFormat(c->hdc, pf, &pfd);

	// Create GL context (sharing with app context if provided)
	c->app_hglrc = (HGLRC)app_gl_context;
	c->hglrc = wglCreateContext(c->hdc);
	if (c->hglrc == NULL) {
		U_LOG_E("Failed to create WGL context");
		return false;
	}

	// Share texture namespace with app context
	if (c->app_hglrc != NULL) {
		if (!wglShareLists(c->app_hglrc, c->hglrc)) {
			U_LOG_E("wglShareLists failed: %lu", GetLastError());
			return false;
		}
	}

	wglMakeCurrent(c->hdc, c->hglrc);

	// Load GL and WGL function pointers via GLAD
	HMODULE opengl_dll = LoadLibraryW(L"opengl32.dll");
	if (opengl_dll == NULL) {
		U_LOG_E("Failed to load opengl32.dll");
		return false;
	}

	int wgl_result = gladLoadWGLUserPtr(c->hdc, gl_get_proc_addr, opengl_dll);
	int gl_result = gladLoadGLUserPtr(gl_get_proc_addr, opengl_dll);

	if (wgl_result == 0 || gl_result == 0) {
		U_LOG_E("Failed to load GLAD functions: WGL=%d, GL=%d", wgl_result, gl_result);
		FreeLibrary(opengl_dll);
		return false;
	}

	U_LOG_W("GLAD loaded: GL %d.%d, renderer: %s",
	         GLAD_VERSION_MAJOR(gl_result), GLAD_VERSION_MINOR(gl_result),
	         glGetString ? (const char *)glGetString(GL_RENDERER) : "unknown");

	return true;
}
#endif // XRT_OS_WINDOWS


/*
 *
 * GL resource initialization
 *
 */

static bool
gl_init_resources(struct comp_gl_compositor *c, uint32_t width, uint32_t height)
{
	c->view_width = width / 2;
	c->view_height = height;

	// Compile shaders
	c->program_blit = create_program(VS_FULLSCREEN_QUAD, FS_BLIT);
	c->program_sbs = create_program(VS_FULLSCREEN_QUAD, FS_SBS);
	c->program_anaglyph = create_program(VS_FULLSCREEN_QUAD, FS_ANAGLYPH);
	c->program_blend = create_program(VS_FULLSCREEN_QUAD, FS_BLEND);

	if (!c->program_blit || !c->program_sbs || !c->program_anaglyph || !c->program_blend) {
		U_LOG_E("Failed to compile GL compositor shaders");
		return false;
	}

	// Empty VAO for vertex-shader-generated geometry
	glGenVertexArrays(1, &c->vao_empty);

	// FBO for offscreen rendering into SBS texture
	glGenFramebuffers(1, &c->fbo);

	// SBS stereo texture (left|right side by side)
	glGenTextures(1, &c->stereo_texture);
	glBindTexture(GL_TEXTURE_2D, c->stereo_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
	             c->view_width * 2, c->view_height, 0,
	             GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glBindTexture(GL_TEXTURE_2D, 0);

	// Parse output mode from environment
	const char *mode_env = getenv("SIM_DISPLAY_OUTPUT_MODE");
	if (mode_env != NULL) {
		if (strcmp(mode_env, "anaglyph") == 0) {
			c->output_mode = GL_OUTPUT_ANAGLYPH;
		} else if (strcmp(mode_env, "blend") == 0) {
			c->output_mode = GL_OUTPUT_BLEND;
		} else {
			c->output_mode = GL_OUTPUT_SBS;
		}
	} else {
		c->output_mode = GL_OUTPUT_SBS;
	}

	U_LOG_W("GL compositor resources initialized: %ux%u per eye, mode=%s",
	         c->view_width, c->view_height,
	         c->output_mode == GL_OUTPUT_SBS ? "SBS" :
	         c->output_mode == GL_OUTPUT_ANAGLYPH ? "Anaglyph" : "Blend");

	return true;
}


/*
 *
 * Public API
 *
 */

void
comp_gl_compositor_set_system_devices(struct xrt_compositor *xc, struct xrt_system_devices *xsysd)
{
	struct comp_gl_compositor *c = gl_comp(xc);
	c->xsysd = xsysd;
}

void
comp_gl_compositor_set_sys_info(struct xrt_compositor *xc, const struct xrt_system_compositor_info *info)
{
	struct comp_gl_compositor *c = gl_comp(xc);
	c->sys_info = *info;
	c->sys_info_set = true;
}

xrt_result_t
comp_gl_compositor_create(struct xrt_device *xdev,
                          void *window_handle,
                          void *gl_context,
                          void *gl_display,
                          void *dp_factory_gl,
                          void *shared_texture_handle,
                          struct xrt_compositor_native **out_xcn)
{
	struct comp_gl_compositor *c = U_TYPED_CALLOC(struct comp_gl_compositor);
	c->xdev = xdev;

	// Get window dimensions
	uint32_t width = GL_DEFAULT_WIDTH;
	uint32_t height = GL_DEFAULT_HEIGHT;

	if (xdev != NULL && xdev->hmd != NULL &&
	    xdev->hmd->screens[0].w_pixels > 0) {
		width = xdev->hmd->screens[0].w_pixels;
		height = xdev->hmd->screens[0].h_pixels;
	}

	// Platform-specific context/window setup
#ifdef XRT_OS_WINDOWS
	if (!gl_create_window_and_context(c, window_handle, gl_context)) {
		free(c);
		return XRT_ERROR_OPENGL;
	}

	// Set up D3D11 interop if shared texture handle provided
	if (shared_texture_handle != NULL) {
		// Load WGL_NV_DX_interop2 function pointers
		c->pfn_wglDXOpenDeviceNV = (PFN_wglDXOpenDeviceNV)wglGetProcAddress("wglDXOpenDeviceNV");
		c->pfn_wglDXCloseDeviceNV = (PFN_wglDXCloseDeviceNV)wglGetProcAddress("wglDXCloseDeviceNV");
		c->pfn_wglDXRegisterObjectNV = (PFN_wglDXRegisterObjectNV)wglGetProcAddress("wglDXRegisterObjectNV");
		c->pfn_wglDXUnregisterObjectNV = (PFN_wglDXUnregisterObjectNV)wglGetProcAddress("wglDXUnregisterObjectNV");
		c->pfn_wglDXLockObjectsNV = (PFN_wglDXLockObjectsNV)wglGetProcAddress("wglDXLockObjectsNV");
		c->pfn_wglDXUnlockObjectsNV = (PFN_wglDXUnlockObjectsNV)wglGetProcAddress("wglDXUnlockObjectsNV");

		if (!c->pfn_wglDXOpenDeviceNV || !c->pfn_wglDXRegisterObjectNV ||
		    !c->pfn_wglDXLockObjectsNV || !c->pfn_wglDXUnlockObjectsNV) {
			U_LOG_E("WGL_NV_DX_interop2 not available — shared texture requires NVIDIA or AMD GPU");
			free(c);
			return XRT_ERROR_OPENGL;
		}

		// Create D3D11 device for interop
		HRESULT hr = D3D11CreateDevice(
		    NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
		    NULL, 0, D3D11_SDK_VERSION,
		    &c->dx_device, NULL, &c->dx_context);
		if (FAILED(hr)) {
			U_LOG_E("Failed to create D3D11 device for GL interop: 0x%08x", hr);
			free(c);
			return XRT_ERROR_OPENGL;
		}

		// Open the shared D3D11 texture
		hr = c->dx_device->lpVtbl->OpenSharedResource(
		    c->dx_device, (HANDLE)shared_texture_handle,
		    &IID_ID3D11Texture2D_local, (void **)&c->dx_shared_texture);
		if (FAILED(hr)) {
			U_LOG_E("Failed to open shared D3D11 texture: 0x%08x", hr);
			c->dx_context->lpVtbl->Release(c->dx_context);
			c->dx_device->lpVtbl->Release(c->dx_device);
			free(c);
			return XRT_ERROR_OPENGL;
		}

		// Get shared texture dimensions
		D3D11_TEXTURE2D_DESC desc;
		c->dx_shared_texture->lpVtbl->GetDesc(c->dx_shared_texture, &desc);
		c->shared_width = desc.Width;
		c->shared_height = desc.Height;

		// Register D3D11 device with WGL
		c->dx_interop_device = c->pfn_wglDXOpenDeviceNV(c->dx_device);
		if (!c->dx_interop_device) {
			U_LOG_E("wglDXOpenDeviceNV failed: %lu", GetLastError());
			c->dx_shared_texture->lpVtbl->Release(c->dx_shared_texture);
			c->dx_context->lpVtbl->Release(c->dx_context);
			c->dx_device->lpVtbl->Release(c->dx_device);
			free(c);
			return XRT_ERROR_OPENGL;
		}

		// Create GL texture and map it to the D3D11 shared texture
		glGenTextures(1, &c->shared_gl_texture);
		c->dx_interop_object = c->pfn_wglDXRegisterObjectNV(
		    c->dx_interop_device, c->dx_shared_texture,
		    c->shared_gl_texture, GL_TEXTURE_2D, WGL_ACCESS_READ_WRITE_NV);
		if (!c->dx_interop_object) {
			U_LOG_E("wglDXRegisterObjectNV failed: %lu", GetLastError());
			glDeleteTextures(1, &c->shared_gl_texture);
			c->pfn_wglDXCloseDeviceNV(c->dx_interop_device);
			c->dx_shared_texture->lpVtbl->Release(c->dx_shared_texture);
			c->dx_context->lpVtbl->Release(c->dx_context);
			c->dx_device->lpVtbl->Release(c->dx_device);
			free(c);
			return XRT_ERROR_OPENGL;
		}

		c->has_shared_texture = true;
		U_LOG_W("D3D11/GL interop initialized: shared texture %ux%u mapped to GL texture %u",
		         c->shared_width, c->shared_height, c->shared_gl_texture);
	}
#elif defined(__APPLE__)
	// macOS: use provided CGL context directly
	c->cgl_context = gl_context;
	(void)shared_texture_handle;
#else
	(void)shared_texture_handle;
#endif

	// Initialize GL resources
	if (!gl_init_resources(c, width, height)) {
		free(c);
		return XRT_ERROR_OPENGL;
	}

	// Set up compositor interface
	struct xrt_compositor *xc = &c->base.base;
	xc->get_swapchain_create_properties = gl_compositor_get_swapchain_create_properties;
	xc->create_swapchain = gl_compositor_create_swapchain;
	xc->begin_session = gl_compositor_begin_session;
	xc->end_session = gl_compositor_end_session;
	xc->predict_frame = gl_compositor_predict_frame;
	xc->mark_frame = gl_compositor_mark_frame;
	xc->wait_frame = gl_compositor_wait_frame;
	xc->begin_frame = gl_compositor_begin_frame;
	xc->discard_frame = gl_compositor_discard_frame;
	xc->layer_begin = gl_compositor_layer_begin;
	xc->layer_projection = gl_compositor_layer_projection;
	xc->layer_projection_depth = gl_compositor_layer_projection_depth;
	xc->layer_quad = gl_compositor_layer_quad;
	xc->layer_commit = gl_compositor_layer_commit;
	xc->destroy = gl_compositor_destroy;

	// Set formats
	gl_compositor_set_formats(c);

	// Visibility/focus flags for state transitions
	xc->info.initial_visible = true;
	xc->info.initial_focused = true;

	*out_xcn = &c->base;

	U_LOG_W("Native OpenGL compositor created: %ux%u", width, height);

	return XRT_SUCCESS;
}
