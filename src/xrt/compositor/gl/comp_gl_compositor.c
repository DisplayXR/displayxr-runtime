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
#ifdef _WIN32
#include "d3d11/comp_d3d11_window.h"
#endif

#include "util/comp_layer_accum.h"

#include "xrt/xrt_device.h"
#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_display_processor_gl.h"
#include "xrt/xrt_handles.h"
#include "xrt/xrt_config_build.h"
#include "xrt/xrt_config_os.h"
#include "xrt/xrt_limits.h"
#include "xrt/xrt_system.h"

#include "util/u_logging.h"
#include "util/u_misc.h"
#include "util/u_tiling.h"
#include "util/u_canvas.h"
#include "util/u_time.h"
#include "util/u_hud.h"
#include "os/os_time.h"
#include "os/os_threading.h"
#include "math/m_api.h"

#ifdef XRT_BUILD_DRIVER_QWERTY
#include "qwerty_interface.h"
#endif

#include <math.h>
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
#include "comp_gl_window_macos.h"
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
#ifndef GL_MAX_LAYERS
#define GL_MAX_LAYERS 16
#endif

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
    "uniform float u_flip_y;\n" // 0.0 = normal, 1.0 = flip Y (for IOSurface)
    "void main() {\n"
    "    float x = float((gl_VertexID & 1) << 2);\n"
    "    float y = float((gl_VertexID & 2) << 1);\n"
    "    float uv_y = y * 0.5;\n"
    "    v_uv = vec2(x * 0.5, mix(uv_y, 1.0 - uv_y, u_flip_y));\n"
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

//! Vertex shader: positioned quad for window-space layers.
//! Takes uniform position/size in NDC.
static const char *VS_WINDOW_SPACE =
    "#version 330 core\n"
    "out vec2 v_uv;\n"
    "uniform vec4 u_rect;\n" // x, y, w, h in NDC [-1,1]
    "void main() {\n"
    "    float x = float((gl_VertexID & 1) << 1) - 1.0;\n" // -1 or 1
    "    float y = float((gl_VertexID & 2)) - 1.0;\n"       // -1 or 1
    "    v_uv = vec2((x + 1.0) * 0.5, (y + 1.0) * 0.5);\n"
    "    gl_Position = vec4(u_rect.x + (x * 0.5 + 0.5) * u_rect.z,\n"
    "                       u_rect.y + (y * 0.5 + 0.5) * u_rect.w,\n"
    "                       0.0, 1.0);\n"
    "}\n";

//! Fragment shader: textured quad with alpha.
static const char *FS_TEXTURED =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D u_texture;\n"
    "uniform vec4 u_src_rect;\n" // x, y, w, h in texture coords
    "void main() {\n"
    "    vec2 uv = u_src_rect.xy + v_uv * u_src_rect.zw;\n"
    "    fragColor = texture(u_texture, uv);\n"
    "}\n";

/*
 *
 * GL compositor structure
 *
 */

struct comp_gl_compositor
{
	//! Must be first — implements xrt_compositor_native.
	struct xrt_compositor_native base;

	// --- GL resources ---
	GLuint program_blit;      //!< Shader for blitting eye to SBS texture
	GLuint program_window_space; //!< Window-space layer (positioned quad)
	GLuint vao_empty;         //!< Empty VAO for vertex-shader-generated fullscreen quad
	GLuint fbo;               //!< Framebuffer for rendering into SBS texture
	GLuint atlas_texture;    //!< Atlas stereo texture (tile_columns * view_width x tile_rows * view_height)
	uint32_t atlas_tex_width;  //!< Atlas texture width (fixed at init)
	uint32_t atlas_tex_height; //!< Atlas texture height (fixed at init)
	uint32_t view_width;
	uint32_t view_height;
	uint32_t tile_columns;    //!< Tile columns in atlas layout (default 2 for SBS stereo)
	uint32_t tile_rows;       //!< Tile rows in atlas layout (default 1 for SBS stereo)

	// --- Layer accumulation ---
	struct comp_layer_accum layer_accum;

	// --- Platform context ---
#ifdef XRT_OS_WINDOWS
	HWND hwnd;
	HDC hdc;
	HGLRC hglrc;        //!< Compositor's own GL context
	HGLRC app_hglrc;    //!< App's GL context (shared textures)
	HDC app_hdc;        //!< App's device context (for restoring after compositor work)
	struct comp_d3d11_window *own_window; //!< Self-owned window (hosted mode)
	bool owns_window;
#elif defined(XRT_OS_ANDROID)
	void *egl_display;   //!< EGLDisplay
	void *egl_context;   //!< EGLContext
	void *egl_surface;   //!< EGLSurface
#elif defined(__APPLE__)
	struct comp_gl_window_macos *macos_window;  //!< macOS window helper
	bool owns_window;
	bool has_shared_iosurface;
	GLuint iosurface_gl_texture;    //!< GL texture backed by shared IOSurface
	uint32_t iosurface_width;
	uint32_t iosurface_height;
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

	// --- Display processor ---
	struct xrt_display_processor_gl *display_processor;
	GLuint dp_crop_fbo;            //!< FBO for cropping atlas to content dims before DP
	GLuint dp_input_texture;       //!< Intermediate texture at content dims for DP input
	uint32_t dp_input_width;       //!< Current dp_input_texture width (0 = not allocated)
	uint32_t dp_input_height;      //!< Current dp_input_texture height

	// --- Eye tracking cache ---
	//! Cached eye positions from layer_commit (where SR weaver has fresh data).
	//! Returned by get_predicted_eye_positions for xrLocateViews (called before layer_commit).
	struct xrt_eye_positions cached_eye_pos;
	bool have_cached_eye_pos;

	// --- State ---
	bool hardware_display_3d;  //!< True when in 3D mode, false = 2D passthrough
	uint64_t last_frame_ns;
	struct u_hud *hud;          //!< HUD overlay (shared u_hud system)
	GLuint hud_texture;         //!< GL texture for HUD pixel upload
	float smoothed_frame_time_ms; //!< Smoothed frame time for HUD FPS display
	struct xrt_device *xdev;
	struct xrt_system_devices *xsysd;
	bool sys_info_set;
	struct xrt_system_compositor_info sys_info;
	uint32_t last_3d_mode_index;       //!< Last 3D mode index (for V-key toggle restore)
	bool legacy_app_tile_scaling;      //!< True if app is legacy (gates 1/2/3 key mode selection)

	//! Canvas output rect for shared-texture apps.
	struct u_canvas_rect canvas;
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

	// Ensure compositor's GL context is current for texture creation
#ifdef XRT_OS_WINDOWS
	HDC prev_hdc = wglGetCurrentDC();
	HGLRC prev_hglrc = wglGetCurrentContext();
	wglMakeCurrent(c->hdc, c->hglrc);
#elif defined(__APPLE__)
	CGLContextObj prev_cgl_ctx = CGLGetCurrentContext();
	comp_gl_window_macos_make_current(c->macos_window);
#endif

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

	// Restore previous GL context
#ifdef XRT_OS_WINDOWS
	if (prev_hglrc != NULL) {
		wglMakeCurrent(prev_hdc, prev_hglrc);
	} else {
		wglMakeCurrent(NULL, NULL);
	}
#elif defined(__APPLE__)
	if (prev_cgl_ctx != NULL) {
		CGLSetCurrentContext(prev_cgl_ctx);
	}
#endif

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
#ifdef XRT_OS_WINDOWS
	// Check if self-owned window was closed
	{
		struct comp_gl_compositor *c = gl_comp(xc);
		if (c->owns_window && c->own_window != NULL &&
		    !comp_d3d11_window_is_valid(c->own_window)) {
			U_LOG_I("Window closed - signaling session exit");
			return XRT_ERROR_IPC_FAILURE;
		}
	}
#endif

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
	int64_t wake, gpu_time;
	return gl_compositor_predict_frame(xc, out_frame_id, &wake, &gpu_time,
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

static xrt_result_t
gl_compositor_layer_window_space(struct xrt_compositor *xc,
                                  struct xrt_device *xdev,
                                  struct xrt_swapchain *xsc,
                                  const struct xrt_layer_data *data)
{
	struct comp_gl_compositor *c = gl_comp(xc);
	comp_layer_accum_window_space(&c->layer_accum, xsc, data);
	return XRT_SUCCESS;
}


/*
 *
 * HUD overlay (shared u_hud system, cross-platform)
 *
 */

static void
gl_compositor_render_hud(struct comp_gl_compositor *c, float dt, uint32_t win_w, uint32_t win_h)
{
	if (c->hud == NULL || !u_hud_is_visible()) {
		return;
	}

	// Smooth frame time (every frame for accuracy)
	float dt_ms = dt * 1000.0f;
	if (dt_ms > 0.0f) {
		c->smoothed_frame_time_ms = c->smoothed_frame_time_ms * 0.9f + dt_ms * 0.1f;
	}
	float fps = (c->smoothed_frame_time_ms > 0.0f) ? (1000.0f / c->smoothed_frame_time_ms) : 0.0f;

	// Display dimensions from sys_info
	float disp_w_mm = 0, disp_h_mm = 0;
	float nom_x = 0, nom_y = 0, nom_z = 600.0f;
	if (c->sys_info_set) {
		disp_w_mm = c->sys_info.display_width_m * 1000.0f;
		disp_h_mm = c->sys_info.display_height_m * 1000.0f;
		nom_y = c->sys_info.nominal_viewer_y_m * 1000.0f;
		nom_z = c->sys_info.nominal_viewer_z_m * 1000.0f;
	}

	// Eye positions from display processor (fallback to nominal stereo)
	struct xrt_eye_positions eye_pos = {0};
	bool have_eyes = false;
	if (c->display_processor != NULL) {
		have_eyes = xrt_display_processor_gl_get_predicted_eye_positions(
		    c->display_processor, &eye_pos) && eye_pos.valid;
	}
	{
		static int hud_eye_log = 0;
		if (hud_eye_log < 5) {
			U_LOG_W("EYE-HUD[%d]: have=%d e0=(%.4f,%.4f,%.4f) e1=(%.4f,%.4f,%.4f)",
			        hud_eye_log, have_eyes,
			        eye_pos.eyes[0].x, eye_pos.eyes[0].y, eye_pos.eyes[0].z,
			        eye_pos.eyes[1].x, eye_pos.eyes[1].y, eye_pos.eyes[1].z);
			hud_eye_log++;
		}
	}
	if (!have_eyes) {
		eye_pos.count = 2;
		eye_pos.eyes[0] = (struct xrt_eye_position){-0.032f, nom_y / 1000.0f, nom_z / 1000.0f};
		eye_pos.eyes[1] = (struct xrt_eye_position){ 0.032f, nom_y / 1000.0f, nom_z / 1000.0f};
	}

	// Fill HUD data
	struct u_hud_data data = {0};
	data.device_name = (c->xdev != NULL) ? c->xdev->str : "Unknown";
	data.fps = fps;
	data.frame_time_ms = c->smoothed_frame_time_ms;
	data.mode_3d = c->hardware_display_3d;
	if (c->xdev != NULL && c->xdev->hmd != NULL) {
		uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
		if (idx < c->xdev->rendering_mode_count) {
			data.rendering_mode_name = c->xdev->rendering_modes[idx].mode_name;
		}
	}
	data.render_width = c->view_width;
	data.render_height = c->view_height;
	if (c->xdev != NULL && c->xdev->rendering_mode_count > 0) {
		u_tiling_compute_system_atlas(c->xdev->rendering_modes, c->xdev->rendering_mode_count,
		                              &data.swapchain_width, &data.swapchain_height);
	}
	data.window_width = win_w;
	data.window_height = win_h;
	data.display_width_mm = disp_w_mm;
	data.display_height_mm = disp_h_mm;
	data.nominal_x = nom_x;
	data.nominal_y = nom_y;
	data.nominal_z = nom_z;
	data.eye_count = eye_pos.count;
	for (uint32_t e = 0; e < eye_pos.count && e < 8; e++) {
		data.eyes[e].x = eye_pos.eyes[e].x * 1000.0f;
		data.eyes[e].y = eye_pos.eyes[e].y * 1000.0f;
		data.eyes[e].z = eye_pos.eyes[e].z * 1000.0f;
	}
	data.eye_tracking_active = eye_pos.is_tracking;

#ifdef XRT_BUILD_DRIVER_QWERTY
	if (c->xsysd != NULL) {
		struct xrt_pose qwerty_pose;
		if (qwerty_get_hmd_pose(c->xsysd->xdevs, c->xsysd->xdev_count, &qwerty_pose)) {
			data.vdisp_x = qwerty_pose.position.x;
			data.vdisp_y = qwerty_pose.position.y;
			data.vdisp_z = qwerty_pose.position.z;
			struct xrt_vec3 fwd_in = {0, 0, -1};
			struct xrt_vec3 fwd_out;
			math_quat_rotate_vec3(&qwerty_pose.orientation, &fwd_in, &fwd_out);
			data.forward_x = fwd_out.x;
			data.forward_y = fwd_out.y;
			data.forward_z = fwd_out.z;
		}

		struct qwerty_view_state ss;
		if (qwerty_get_view_state(c->xsysd->xdevs, c->xsysd->xdev_count, &ss)) {
			data.camera_mode = ss.camera_mode;
			data.cam_spread_factor = ss.cam_spread_factor;
			data.cam_parallax_factor = ss.cam_parallax_factor;
			data.cam_convergence = ss.cam_convergence;
			data.cam_half_tan_vfov = ss.cam_half_tan_vfov;
			data.disp_spread_factor = ss.disp_spread_factor;
			data.disp_parallax_factor = ss.disp_parallax_factor;
			data.disp_vHeight = ss.disp_vHeight;
			data.nominal_viewer_z = ss.nominal_viewer_z;
			data.screen_height_m = ss.screen_height_m;
		}
	}
#endif

	bool dirty = u_hud_update(c->hud, &data);

	// Lazy-create GL texture
	if (c->hud_texture == 0) {
		uint32_t hud_w = u_hud_get_width(c->hud);
		uint32_t hud_h = u_hud_get_height(c->hud);
		glGenTextures(1, &c->hud_texture);
		glBindTexture(GL_TEXTURE_2D, c->hud_texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, hud_w, hud_h, 0,
		             GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glBindTexture(GL_TEXTURE_2D, 0);
		dirty = true;
	}

	// Upload pixels if changed
	if (dirty) {
		uint32_t hud_w = u_hud_get_width(c->hud);
		uint32_t hud_h = u_hud_get_height(c->hud);
		glBindTexture(GL_TEXTURE_2D, c->hud_texture);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, hud_w, hud_h,
		                GL_RGBA, GL_UNSIGNED_BYTE, u_hud_get_pixels(c->hud));
		glBindTexture(GL_TEXTURE_2D, 0);
	}

	// Blit HUD to bottom-left of screen with alpha blending.
	// Scale down if HUD would exceed 50% of window width.
	uint32_t hud_w = u_hud_get_width(c->hud);
	uint32_t hud_h = u_hud_get_height(c->hud);
	uint32_t margin = 10;
	float scale = 1.0f;
	float max_frac = 0.5f;
	if (hud_w > (uint32_t)(win_w * max_frac)) {
		scale = (win_w * max_frac) / (float)hud_w;
	}
	uint32_t draw_w = (uint32_t)(hud_w * scale);
	uint32_t draw_h = (uint32_t)(hud_h * scale);

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glUseProgram(c->program_blit);
	glBindVertexArray(c->vao_empty);
	glViewport(margin, margin, draw_w, draw_h);

	GLint loc_rect = glGetUniformLocation(c->program_blit, "u_src_rect");
	glUniform4f(loc_rect, 0.0f, 0.0f, 1.0f, 1.0f);
	GLint loc_flip = glGetUniformLocation(c->program_blit, "u_flip_y");
	glUniform1f(loc_flip, 1.0f); // Flip Y: u_hud is top-down, GL is bottom-up

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, c->hud_texture);
	GLint loc_tex = glGetUniformLocation(c->program_blit, "u_texture");
	glUniform1i(loc_tex, 0);

	glDrawArrays(GL_TRIANGLES, 0, 3);

	glDisable(GL_BLEND);
}


/*
 *
 * Crop atlas to content dims and pass to display processor
 *
 */

/*!
 * Crop the valid content region from the (potentially oversized) atlas texture
 * and pass it to the display processor. If the atlas exactly matches the
 * content dimensions, the atlas is passed directly (no copy).
 *
 * @param c           GL compositor
 * @param atlas_tex   Source atlas texture (may be oversized)
 * @param output_w    Output surface width (window, shared texture, IOSurface)
 * @param output_h    Output surface height
 */
static void
gl_crop_and_process_dp(struct comp_gl_compositor *c,
                       GLuint atlas_tex,
                       uint32_t output_w,
                       uint32_t output_h)
{
	uint32_t content_w = c->tile_columns * c->view_width;
	uint32_t content_h = c->tile_rows * c->view_height;

	GLuint dp_tex = atlas_tex;

	if (content_w != c->atlas_tex_width || content_h != c->atlas_tex_height) {
		// Content is smaller than atlas — need to crop.
		// Lazily (re)create intermediate texture at content dims.
		if (c->dp_input_width != content_w || c->dp_input_height != content_h) {
			if (c->dp_input_texture != 0) {
				glDeleteTextures(1, &c->dp_input_texture);
			}
			glGenTextures(1, &c->dp_input_texture);
			glBindTexture(GL_TEXTURE_2D, c->dp_input_texture);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
			             content_w, content_h, 0,
			             GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glBindTexture(GL_TEXTURE_2D, 0);
			c->dp_input_width = content_w;
			c->dp_input_height = content_h;
			U_LOG_I("GL crop: created DP input texture %ux%u (atlas %ux%u)",
			        content_w, content_h, c->atlas_tex_width, c->atlas_tex_height);
		}

		// Blit content region from atlas into intermediate texture
		glBindFramebuffer(GL_READ_FRAMEBUFFER, c->fbo);
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                        GL_TEXTURE_2D, atlas_tex, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, c->dp_crop_fbo);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                        GL_TEXTURE_2D, c->dp_input_texture, 0);

		glBlitFramebuffer(
		    0, 0, content_w, content_h,   // src rect (content region)
		    0, 0, content_w, content_h,   // dst rect (same size)
		    GL_COLOR_BUFFER_BIT, GL_NEAREST);

		// Restore FBO state
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

		dp_tex = c->dp_input_texture;
	}

	// Pass (possibly cropped) texture to DP
	glViewport(0, 0, output_w, output_h);
	xrt_display_processor_gl_process_atlas(
	    c->display_processor,
	    dp_tex,
	    c->view_width,
	    c->view_height,
	    c->tile_columns,
	    c->tile_rows,
	    GL_RGBA8,
	    output_w,
	    output_h);
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
	float dt = (c->last_frame_ns > 0) ? (float)(now_ns - c->last_frame_ns) / 1e9f : 0.016f;
	c->last_frame_ns = now_ns;

	if (c->layer_accum.layer_count == 0) {
		return XRT_SUCCESS;
	}

	// Save previous GL context and switch to compositor's
#ifdef XRT_OS_WINDOWS
	HDC prev_hdc = wglGetCurrentDC();
	HGLRC prev_hglrc = wglGetCurrentContext();
	wglMakeCurrent(c->hdc, c->hglrc);
#elif defined(__APPLE__)
	CGLContextObj prev_cgl_ctx = CGLGetCurrentContext();
	comp_gl_window_macos_make_current(c->macos_window);
#endif

	// Save and set GL state — the app may have left depth test, blend, etc. on
	GLboolean prev_depth_test = glIsEnabled(GL_DEPTH_TEST);
	GLboolean prev_blend = glIsEnabled(GL_BLEND);
	GLboolean prev_cull_face = glIsEnabled(GL_CULL_FACE);
	GLboolean prev_scissor_test = glIsEnabled(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);

	// Runtime-side 2D/3D toggle from qwerty V key
#ifdef XRT_BUILD_DRIVER_QWERTY
	if (c->xsysd != NULL) {
		bool force_2d = false;
		bool toggled = qwerty_check_display_mode_toggle(c->xsysd->xdevs, c->xsysd->xdev_count, &force_2d);
		if (toggled) {
			struct xrt_device *head = c->xsysd->static_roles.head;
			if (head != NULL && head->hmd != NULL) {
				if (force_2d) {
					uint32_t cur = head->hmd->active_rendering_mode_index;
					if (cur < head->rendering_mode_count &&
					    head->rendering_modes[cur].hardware_display_3d) {
						c->last_3d_mode_index = cur;
					}
					head->hmd->active_rendering_mode_index = 0;
				} else {
					head->hmd->active_rendering_mode_index = c->last_3d_mode_index;
				}
			}
			comp_gl_compositor_request_display_mode(&c->base.base, !force_2d);
		}

		// Rendering mode change from qwerty 0/1/2/3/4 keys.
		// Legacy apps only support V toggle — skip direct mode selection.
		if (!c->legacy_app_tile_scaling) {
			int render_mode = -1;
			if (qwerty_check_rendering_mode_change(c->xsysd->xdevs, c->xsysd->xdev_count, &render_mode)) {
				struct xrt_device *head = c->xsysd->static_roles.head;
				if (head != NULL) {
					xrt_device_set_property(head, XRT_DEVICE_PROPERTY_OUTPUT_MODE, render_mode);
				}
			}
		}
	}
#endif

	// Sync hardware_display_3d, tile layout, and per-view dimensions
	// from device's active rendering mode (MUST be before zero-copy check and blit)
	if (c->xdev != NULL && c->xdev->hmd != NULL) {
		uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
		if (idx < c->xdev->rendering_mode_count) {
			const struct xrt_rendering_mode *mode = &c->xdev->rendering_modes[idx];
			c->hardware_display_3d = mode->hardware_display_3d;
			if (mode->tile_columns > 0) {
				c->tile_columns = mode->tile_columns;
				c->tile_rows = mode->tile_rows;
			}
			// Sync view dims from active mode every frame — needed for
			// correct crop before DP and correct blit UV calculation.
			// Legacy apps: view dims are fixed at compromise scale, skip.
			if (!c->legacy_app_tile_scaling && mode->view_width_pixels > 0) {
				c->view_width = mode->view_width_pixels;
				c->view_height = mode->view_height_pixels;
				if (c->canvas.valid) {
					u_tiling_compute_canvas_view(mode, c->canvas.w, c->canvas.h,
					                             &c->view_width, &c->view_height);
				}
			}
		}
	}

	// HUD is rendered in the window-mode present path (after weave, before swap)

	// Zero-copy check: can we pass the app's swapchain directly to the DP?
	bool zero_copy = false;
	GLuint zc_texture = 0;
	{
		const struct xrt_rendering_mode *mode = NULL;
		if (c->xdev != NULL && c->xdev->hmd != NULL) {
			uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
			if (idx < c->xdev->rendering_mode_count)
				mode = &c->xdev->rendering_modes[idx];
		}
		if (mode != NULL && c->layer_accum.layer_count == 1) {
			struct comp_layer *layer = &c->layer_accum.layers[0];
			if (layer->data.type == XRT_LAYER_PROJECTION ||
			    layer->data.type == XRT_LAYER_PROJECTION_DEPTH) {
				uint32_t vc = mode->view_count;
				bool same_sc = (vc > 0 && vc <= XRT_MAX_VIEWS && layer->sc_array[0] != NULL);
				for (uint32_t v = 1; v < vc && same_sc; v++) {
					if (layer->sc_array[v] != layer->sc_array[0])
						same_sc = false;
				}
				if (same_sc) {
					uint32_t img_idx = layer->data.proj.v[0].sub.image_index;
					bool same_idx = true;
					for (uint32_t v = 1; v < vc; v++) {
						if (layer->data.proj.v[v].sub.image_index != img_idx) {
							same_idx = false;
							break;
						}
					}
					bool all_array_zero = same_idx;
					for (uint32_t v = 0; v < vc && all_array_zero; v++) {
						if (layer->data.proj.v[v].sub.array_index != 0)
							all_array_zero = false;
					}
					if (all_array_zero) {
						struct comp_gl_swapchain *gsc = gl_swapchain(layer->sc_array[0]);
						int32_t rxs[XRT_MAX_VIEWS], rys[XRT_MAX_VIEWS];
						uint32_t rws[XRT_MAX_VIEWS], rhs_arr[XRT_MAX_VIEWS];
						for (uint32_t v = 0; v < vc; v++) {
							rxs[v] = layer->data.proj.v[v].sub.rect.offset.w;
							rys[v] = layer->data.proj.v[v].sub.rect.offset.h;
							rws[v] = layer->data.proj.v[v].sub.rect.extent.w;
							rhs_arr[v] = layer->data.proj.v[v].sub.rect.extent.h;
						}
						if (u_tiling_can_zero_copy(vc, rxs, rys, rws, rhs_arr,
						                           gsc->info.width, gsc->info.height, mode)) {
							zero_copy = true;
							zc_texture = gsc->textures[img_idx];
						}
					}
				}
			}
		}
	}

	// --- Step 1: Render layers into SBS stereo texture (skip if zero-copy) ---
	if (!zero_copy) {
	glBindFramebuffer(GL_FRAMEBUFFER, c->fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                        c->atlas_texture, 0);

	glViewport(0, 0, c->tile_columns * c->view_width, c->tile_rows * c->view_height);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glUseProgram(c->program_blit);
	glBindVertexArray(c->vao_empty);

	GLint loc_tex = glGetUniformLocation(c->program_blit, "u_texture");
	GLint loc_rect = glGetUniformLocation(c->program_blit, "u_src_rect");
	// Ensure no Y-flip for atlas blit (u_flip_y may be stale from IOSurface blit)
	GLint loc_flip_atlas = glGetUniformLocation(c->program_blit, "u_flip_y");
	glUniform1f(loc_flip_atlas, 0.0f);

	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		struct comp_layer *layer = &c->layer_accum.layers[i];

		if (layer->data.type != XRT_LAYER_PROJECTION &&
		    layer->data.type != XRT_LAYER_PROJECTION_DEPTH) {
			continue;
		}

		// Use min of compositor's tile count and layer's actual view count
		// (during mode transitions the app may submit fewer views than the
		// new tile layout expects)
		uint32_t mode_views = c->hardware_display_3d ? (c->tile_columns * c->tile_rows) : 1;
		uint32_t view_count = (layer->data.view_count < mode_views) ? layer->data.view_count : mode_views;
		if (view_count == 0) view_count = 1;
		for (uint32_t eye = 0; eye < view_count; eye++) {

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

			// Set viewport for this eye in the atlas texture
			if (!c->hardware_display_3d) {
				glViewport(0, 0, c->tile_columns * c->view_width, c->tile_rows * c->view_height);
			} else {
				uint32_t tile_x = eye % c->tile_columns;
				uint32_t tile_y = eye / c->tile_columns;
				glViewport(tile_x * c->view_width, tile_y * c->view_height,
				           c->view_width, c->view_height);
			}

			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, gsc->textures[img_idx]);
			glUniform1i(loc_tex, 0);
			glUniform4f(loc_rect, nr.x, nr.y, nr.w, nr.h);

			// One-shot diagnostic: log blit params for both eyes
			{
				static int blit_log = 0;
				if (blit_log < 4) {
					U_LOG_W("GL BLIT eye=%u tex=%u img=%u nr=(%.3f,%.3f,%.3f,%.3f) "
					        "vp=(%u,%u,%u,%u) view_count=%u hw3d=%d",
					        eye, gsc->textures[img_idx], img_idx,
					        nr.x, nr.y, nr.w, nr.h,
					        eye % c->tile_columns * c->view_width,
					        eye / c->tile_columns * c->view_height,
					        c->view_width, c->view_height,
					        view_count, c->hardware_display_3d);
					blit_log++;
				}
			}

			// Draw fullscreen quad (3 vertices, generated in vertex shader)
			glDrawArrays(GL_TRIANGLES, 0, 3);
		}
	}

	// --- Step 1b: Render window-space layers (HUD overlays) ---
	for (uint32_t i = 0; i < c->layer_accum.layer_count; i++) {
		struct comp_layer *layer = &c->layer_accum.layers[i];

		if (layer->data.type != XRT_LAYER_WINDOW_SPACE) {
			continue;
		}

		const struct xrt_layer_window_space_data *ws = &layer->data.window_space;
		struct xrt_swapchain *sc = layer->sc_array[0];
		if (sc == NULL) {
			continue;
		}

		struct comp_gl_swapchain *gsc = gl_swapchain(sc);
		uint32_t img_idx = ws->sub.image_index;
		if (img_idx >= gsc->image_count) {
			continue;
		}

		// Sub-image UV rect
		struct xrt_normalized_rect nr = ws->sub.norm_rect;
		if (nr.w <= 0.0f || nr.h <= 0.0f) {
			nr.x = 0.0f;
			nr.y = 0.0f;
			nr.w = 1.0f;
			nr.h = 1.0f;
		}

		glUseProgram(c->program_window_space);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		GLint loc_ws_rect = glGetUniformLocation(c->program_window_space, "u_rect");
		GLint loc_ws_tex = glGetUniformLocation(c->program_window_space, "u_texture");
		GLint loc_ws_src = glGetUniformLocation(c->program_window_space, "u_src_rect");

		uint32_t effective_views = c->hardware_display_3d ? (c->tile_columns * c->tile_rows) : 1;
		for (uint32_t eye = 0; eye < effective_views; eye++) {
			// Set viewport for this eye
			if (!c->hardware_display_3d) {
				glViewport(0, 0, c->tile_columns * c->view_width, c->tile_rows * c->view_height);
			} else {
				uint32_t tile_x = eye % c->tile_columns;
				uint32_t tile_y = eye / c->tile_columns;
				glViewport(tile_x * c->view_width, tile_y * c->view_height,
				           c->view_width, c->view_height);
			}

			// Per-eye disparity offset
			float half_disp = ws->disparity / 2.0f;
			float eye_shift = (eye == 0) ? -half_disp : half_disp;

			// Window-space fractional coords → NDC [-1, 1]
			float ndc_x = (ws->x + eye_shift) * 2.0f - 1.0f;
			float ndc_y = 1.0f - (ws->y + ws->height) * 2.0f; // flip Y: GL origin is bottom-left
			float ndc_w = ws->width * 2.0f;
			float ndc_h = ws->height * 2.0f;

			glUniform4f(loc_ws_rect, ndc_x, ndc_y, ndc_w, ndc_h);
			glUniform4f(loc_ws_src, nr.x, nr.y, nr.w, nr.h);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, gsc->textures[img_idx]);
			glUniform1i(loc_ws_tex, 0);

			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		}

		glDisable(GL_BLEND);
	}
	} // end if (!zero_copy)

	// --- Step 2: Present SBS texture ---
	// Ensure VAO is bound for present draw calls (zero-copy skips the atlas
	// blit which normally binds it, causing GL_INVALID_OPERATION in core profile)
	glBindVertexArray(c->vao_empty);
	GLuint atlas_for_present = zero_copy ? zc_texture : c->atlas_texture;
#ifdef XRT_OS_WINDOWS
	if (c->has_shared_texture) {
		// Shared texture mode: render into the DX-interop GL texture
		if (c->pfn_wglDXLockObjectsNV(c->dx_interop_device, 1, &c->dx_interop_object)) {
			glBindFramebuffer(GL_FRAMEBUFFER, c->fbo);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
			                        c->shared_gl_texture, 0);

			if (c->display_processor != NULL) {
				// DP target: use canvas dims for texture apps
				uint32_t dp_w = c->shared_width;
				uint32_t dp_h = c->shared_height;
				if (c->canvas.valid && c->canvas.w > 0 && c->canvas.h > 0) {
					dp_w = c->canvas.w;
					dp_h = c->canvas.h;
				}
				// Crop atlas to content dims, then pass to DP
				gl_crop_and_process_dp(c, atlas_for_present, dp_w, dp_h);
			} else {
				// No display processor: simple blit
				glViewport(0, 0, c->shared_width, c->shared_height);
				glUseProgram(c->program_blit);
				glBindVertexArray(c->vao_empty);
				GLint loc_rect = glGetUniformLocation(c->program_blit, "u_src_rect");
				float sh_w = (c->atlas_tex_width > 0)
				    ? (float)(c->tile_columns * c->view_width) / (float)c->atlas_tex_width : 1.0f;
				float sh_h = (c->atlas_tex_height > 0)
				    ? (float)(c->tile_rows * c->view_height) / (float)c->atlas_tex_height : 1.0f;
				glUniform4f(loc_rect, 0.0f, 0.0f, sh_w, sh_h);
				GLint loc_flip = glGetUniformLocation(c->program_blit, "u_flip_y");
				glUniform1f(loc_flip, 0.0f);
				glActiveTexture(GL_TEXTURE0);
				glBindTexture(GL_TEXTURE_2D, atlas_for_present);
				GLint loc_out_tex = glGetUniformLocation(c->program_blit, "u_texture");
				glUniform1i(loc_out_tex, 0);
				glDrawArrays(GL_TRIANGLES, 0, 3);
			}

			glFlush();

			// Restore FBO to not target shared texture
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
			glBindFramebuffer(GL_FRAMEBUFFER, 0);

			c->pfn_wglDXUnlockObjectsNV(c->dx_interop_device, 1, &c->dx_interop_object);
		}
	} else
#endif
#ifdef __APPLE__
	if (c->has_shared_iosurface) {
		// Shared IOSurface mode: render into the IOSurface
		glBindFramebuffer(GL_FRAMEBUFFER, c->fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                        GL_TEXTURE_RECTANGLE, c->iosurface_gl_texture, 0);

		if (c->display_processor != NULL) {
			// DP target: use canvas dims for texture apps
			uint32_t dp_w = c->iosurface_width;
			uint32_t dp_h = c->iosurface_height;
			if (c->canvas.valid && c->canvas.w > 0 && c->canvas.h > 0) {
				dp_w = c->canvas.w;
				dp_h = c->canvas.h;
			}
			// Crop atlas to content dims, then pass to DP
			gl_crop_and_process_dp(c, atlas_for_present, dp_w, dp_h);
		} else {
			// No display processor: simple blit, no Y-flip.
			// Content stays GL bottom-up; app's blit shader must NOT flip Y.
			glViewport(0, 0, c->iosurface_width, c->iosurface_height);
			glUseProgram(c->program_blit);
			glBindVertexArray(c->vao_empty);
			GLint loc_rect = glGetUniformLocation(c->program_blit, "u_src_rect");
			float used_w = (c->atlas_tex_width > 0)
			    ? (float)(c->tile_columns * c->view_width) / (float)c->atlas_tex_width : 1.0f;
			float used_h = (c->atlas_tex_height > 0)
			    ? (float)(c->tile_rows * c->view_height) / (float)c->atlas_tex_height : 1.0f;
			glUniform4f(loc_rect, 0.0f, 0.0f, used_w, used_h);
			GLint loc_flip = glGetUniformLocation(c->program_blit, "u_flip_y");
			glUniform1f(loc_flip, 0.0f);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, atlas_for_present);
			GLint loc_out_tex = glGetUniformLocation(c->program_blit, "u_texture");
			glUniform1i(loc_out_tex, 0);
			glDrawArrays(GL_TRIANGLES, 0, 3);
		}

		glFlush();

		// Restore FBO
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		                        GL_TEXTURE_RECTANGLE, 0, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	} else
#endif
	{
		// Normal window mode: present to screen
		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		// Use actual window backing dimensions
		uint32_t present_w = c->tile_columns * c->view_width;
		uint32_t present_h = c->tile_rows * c->view_height;
#ifdef XRT_OS_WINDOWS
		if (c->hwnd != NULL) {
			RECT rc;
			if (GetClientRect(c->hwnd, &rc)) {
				uint32_t ww = (uint32_t)(rc.right - rc.left);
				uint32_t wh = (uint32_t)(rc.bottom - rc.top);
				if (ww > 0 && wh > 0) {
					present_w = ww;
					present_h = wh;
				}
			}
		}
#elif defined(__APPLE__)
		comp_gl_window_macos_get_dimensions(c->macos_window, &present_w, &present_h);
#endif

		if (c->display_processor != NULL) {
			// Crop atlas to content dims, then pass to DP
			// (DP handles both 2D and 3D modes internally)
			gl_crop_and_process_dp(c, atlas_for_present, present_w, present_h);
		} else {
			// No display processor: simple blit
			glViewport(0, 0, present_w, present_h);
			glUseProgram(c->program_blit);
			GLint loc_rect = glGetUniformLocation(c->program_blit, "u_src_rect");
			float blit_w = (c->atlas_tex_width > 0)
			    ? (float)(c->tile_columns * c->view_width) / (float)c->atlas_tex_width : 1.0f;
			float blit_h = (c->atlas_tex_height > 0)
			    ? (float)(c->tile_rows * c->view_height) / (float)c->atlas_tex_height : 1.0f;
			glUniform4f(loc_rect, 0.0f, 0.0f, blit_w, blit_h);
			GLint loc_flip = glGetUniformLocation(c->program_blit, "u_flip_y");
			glUniform1f(loc_flip, 0.0f);

			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, atlas_for_present);
			GLint loc_out_tex = glGetUniformLocation(c->program_blit, "u_texture");
			glUniform1i(loc_out_tex, 0);

			glDrawArrays(GL_TRIANGLES, 0, 3);
		}

		// HUD overlay (post-weave, before swap)
		if (c->owns_window) {
			gl_compositor_render_hud(c, dt, present_w, present_h);
		}

		// Platform-specific swap
#ifdef XRT_OS_WINDOWS
		SwapBuffers(c->hdc);
		if (c->owns_window && c->own_window != NULL) {
			comp_d3d11_window_signal_paint_done(c->own_window);
		}
#elif defined(XRT_OS_ANDROID)
		// eglSwapBuffers(c->egl_display, c->egl_surface);
#elif defined(__APPLE__)
		comp_gl_window_macos_swap_buffers(c->macos_window);
#endif
	}

	// Cache eye positions AFTER process_atlas (which updates the SR weaver's
	// eye tracker). xrLocateViews calls get_predicted_eye_positions BEFORE
	// layer_commit, so it needs cached data from the previous frame.
	if (c->display_processor != NULL) {
		struct xrt_eye_positions fresh_eyes = {0};
		bool fresh_ok = xrt_display_processor_gl_get_predicted_eye_positions(
		        c->display_processor, &fresh_eyes);
		static int cache_log = 0;
		if (cache_log < 5) {
			U_LOG_W("EYE-CACHE[%d]: ok=%d valid=%d count=%d "
			        "e0=(%.4f,%.4f,%.4f) e1=(%.4f,%.4f,%.4f)",
			        cache_log, fresh_ok, fresh_eyes.valid, fresh_eyes.count,
			        fresh_eyes.eyes[0].x, fresh_eyes.eyes[0].y, fresh_eyes.eyes[0].z,
			        fresh_eyes.eyes[1].x, fresh_eyes.eyes[1].y, fresh_eyes.eyes[1].z);
			cache_log++;
		}
		if (fresh_ok && fresh_eyes.valid) {
			c->cached_eye_pos = fresh_eyes;
			c->have_cached_eye_pos = true;
		}
	}

	glBindVertexArray(0);
	glUseProgram(0);
	glBindTexture(GL_TEXTURE_2D, 0);

	// Restore previous GL state
	if (prev_depth_test) glEnable(GL_DEPTH_TEST);
	if (prev_blend) glEnable(GL_BLEND);
	if (prev_cull_face) glEnable(GL_CULL_FACE);
	if (prev_scissor_test) glEnable(GL_SCISSOR_TEST);

	// Restore previous GL context (critical for shared texture mode where
	// app has its own context and needs it back after compositor work)
#ifdef XRT_OS_WINDOWS
	if (prev_hglrc != NULL) {
		wglMakeCurrent(prev_hdc, prev_hglrc);
	} else {
		wglMakeCurrent(NULL, NULL);
	}
#elif defined(__APPLE__)
	if (prev_cgl_ctx != NULL) {
		CGLSetCurrentContext(prev_cgl_ctx);
	}
#endif

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

#ifdef XRT_OS_WINDOWS
	// Make compositor context current for GL resource cleanup
	if (c->hglrc) {
		wglMakeCurrent(c->hdc, c->hglrc);
	}
#endif

	xrt_display_processor_gl_destroy(&c->display_processor);

	// Destroy HUD
	if (c->hud_texture) glDeleteTextures(1, &c->hud_texture);
	u_hud_destroy(&c->hud);

	if (c->dp_input_texture) glDeleteTextures(1, &c->dp_input_texture);
	if (c->dp_crop_fbo) glDeleteFramebuffers(1, &c->dp_crop_fbo);
	if (c->program_blit) glDeleteProgram(c->program_blit);
	if (c->program_window_space) glDeleteProgram(c->program_window_space);
	if (c->vao_empty) glDeleteVertexArrays(1, &c->vao_empty);
	if (c->fbo) glDeleteFramebuffers(1, &c->fbo);
	if (c->atlas_texture) glDeleteTextures(1, &c->atlas_texture);

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
	if (c->owns_window && c->own_window != NULL) {
		comp_d3d11_window_destroy(&c->own_window);
	} else if (c->owns_window && c->hwnd) {
		DestroyWindow(c->hwnd);
	}
#elif defined(__APPLE__)
	if (c->iosurface_gl_texture) {
		glDeleteTextures(1, &c->iosurface_gl_texture);
	}
	if (c->macos_window != NULL) {
		comp_gl_window_macos_destroy(&c->macos_window);
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

static const wchar_t GL_WINDOW_CLASS[] = L"DisplayXRGLCompositor";

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
                              void *app_gl_context,
                              uint32_t width,
                              uint32_t height)
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
		// Use shared window module — borderless fullscreen on Leia display,
		// dedicated thread with message pump, QWERTY input support.
		uint32_t win_w = width > 0 ? width : GL_DEFAULT_WIDTH;
		uint32_t win_h = height > 0 ? height : GL_DEFAULT_HEIGHT;
		xrt_result_t xret = comp_d3d11_window_create(win_w, win_h, &c->own_window);
		if (xret != XRT_SUCCESS) {
			U_LOG_E("Failed to create self-owned window for GL compositor");
			return false;
		}
		c->hwnd = (HWND)comp_d3d11_window_get_hwnd(c->own_window);
		c->owns_window = true;
	}

	if (c->hwnd == NULL) {
		U_LOG_E("Failed to get window handle");
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
	// Initialize tile layout from active rendering mode if available
	if (c->xdev != NULL && c->xdev->hmd != NULL) {
		uint32_t idx = c->xdev->hmd->active_rendering_mode_index;
		if (idx < c->xdev->rendering_mode_count &&
		    c->xdev->rendering_modes[idx].tile_columns > 0) {
			c->tile_columns = c->xdev->rendering_modes[idx].tile_columns;
			c->tile_rows = c->xdev->rendering_modes[idx].tile_rows;
		}
	}
	// Default to 2x1 (SBS stereo) if not set
	if (c->tile_columns == 0) {
		c->tile_columns = 2;
		c->tile_rows = 1;
	}

	c->view_width = width / c->tile_columns;
	c->view_height = height / c->tile_rows;

	// Compile shaders
	c->program_blit = create_program(VS_FULLSCREEN_QUAD, FS_BLIT);
	c->program_window_space = create_program(VS_WINDOW_SPACE, FS_TEXTURED);

	if (!c->program_blit || !c->program_window_space) {
		U_LOG_E("Failed to compile GL compositor shaders");
		return false;
	}

	// Empty VAO for vertex-shader-generated geometry
	glGenVertexArrays(1, &c->vao_empty);

	// FBO for offscreen rendering into atlas texture
	glGenFramebuffers(1, &c->fbo);

	// FBO for cropping atlas to content dims before DP
	glGenFramebuffers(1, &c->dp_crop_fbo);

	// Atlas stereo texture — always worst-case sized across all rendering modes.
	// Per-frame content region may be smaller; compositor crops before DP.
	uint32_t atlas_width = c->tile_columns * c->view_width;
	uint32_t atlas_height = c->tile_rows * c->view_height;
	if (c->xdev != NULL && c->xdev->rendering_mode_count > 0) {
		u_tiling_compute_system_atlas(c->xdev->rendering_modes,
		                              c->xdev->rendering_mode_count,
		                              &atlas_width, &atlas_height);
	}
	c->atlas_tex_width = atlas_width;
	c->atlas_tex_height = atlas_height;
	glGenTextures(1, &c->atlas_texture);
	glBindTexture(GL_TEXTURE_2D, c->atlas_texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
	             atlas_width, atlas_height, 0,
	             GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glBindTexture(GL_TEXTURE_2D, 0);

	c->hardware_display_3d = true;

	U_LOG_W("GL compositor resources initialized: %ux%u per eye, atlas %ux%u (%u cols x %u rows)",
	         c->view_width, c->view_height, atlas_width, atlas_height, c->tile_columns, c->tile_rows);

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
#ifdef XRT_OS_WINDOWS
	if (c->own_window != NULL) {
		comp_d3d11_window_set_system_devices(c->own_window, xsysd);
	}
#endif
}

void
comp_gl_compositor_set_sys_info(struct xrt_compositor *xc, const struct xrt_system_compositor_info *info)
{
	struct comp_gl_compositor *c = gl_comp(xc);
	c->sys_info = *info;
	c->sys_info_set = true;
	c->legacy_app_tile_scaling = info->legacy_app_tile_scaling;
	c->last_3d_mode_index = 1;

	// Legacy apps: fix view dims at the actual recommended size the app was told to render at.
	if (info->legacy_app_tile_scaling &&
	    info->legacy_view_width_pixels > 0 && info->legacy_view_height_pixels > 0) {
		c->view_width = info->legacy_view_width_pixels;
		c->view_height = info->legacy_view_height_pixels;
	}
}

void
comp_gl_compositor_set_output_rect(struct xrt_compositor *xc,
                                    int32_t x, int32_t y,
                                    uint32_t w, uint32_t h)
{
	struct comp_gl_compositor *c = gl_comp(xc);
	c->canvas = (struct u_canvas_rect){.valid = true, .x = x, .y = y, .w = w, .h = h};
}

bool
comp_gl_compositor_request_display_mode(struct xrt_compositor *xc, bool enable_3d)
{
	if (xc == NULL) {
		return false;
	}

	struct comp_gl_compositor *c = gl_comp(xc);

	if (c->display_processor != NULL) {
		return xrt_display_processor_gl_request_display_mode(c->display_processor, enable_3d);
	}

	return false;
}

bool
comp_gl_compositor_get_predicted_eye_positions(struct xrt_compositor *xc,
                                               struct xrt_eye_positions *out_eye_pos)
{
	if (xc == NULL || out_eye_pos == NULL) {
		return false;
	}

	struct comp_gl_compositor *c = gl_comp(xc);

	if (c->display_processor == NULL) {
		return false;
	}

	return xrt_display_processor_gl_get_predicted_eye_positions(
		c->display_processor, out_eye_pos);
}

bool
comp_gl_compositor_get_window_metrics(struct xrt_compositor *xc,
                                      struct xrt_window_metrics *out_metrics)
{
	if (xc == NULL || out_metrics == NULL) {
		return false;
	}

	struct comp_gl_compositor *c = gl_comp(xc);
	memset(out_metrics, 0, sizeof(*out_metrics));

#ifdef XRT_OS_WINDOWS
	if (!c->sys_info_set || c->hwnd == NULL) {
		return false;
	}

	float disp_w_m = c->sys_info.display_width_m;
	float disp_h_m = c->sys_info.display_height_m;
	uint32_t disp_px_w = c->sys_info.display_pixel_width;
	uint32_t disp_px_h = c->sys_info.display_pixel_height;
	if (disp_px_w == 0 || disp_px_h == 0 || disp_w_m <= 0 || disp_h_m <= 0) {
		return false;
	}

	RECT rect;
	if (!GetClientRect(c->hwnd, &rect)) {
		return false;
	}
	uint32_t win_px_w = (uint32_t)(rect.right - rect.left);
	uint32_t win_px_h = (uint32_t)(rect.bottom - rect.top);
	if (win_px_w == 0 || win_px_h == 0) {
		return false;
	}

	POINT client_origin = {0, 0};
	ClientToScreen(c->hwnd, &client_origin);

	float pixel_size_x = disp_w_m / (float)disp_px_w;
	float pixel_size_y = disp_h_m / (float)disp_px_h;

	out_metrics->display_width_m = disp_w_m;
	out_metrics->display_height_m = disp_h_m;
	out_metrics->display_pixel_width = disp_px_w;
	out_metrics->display_pixel_height = disp_px_h;
	out_metrics->display_screen_left = 0;
	out_metrics->display_screen_top = 0;

	out_metrics->window_pixel_width = win_px_w;
	out_metrics->window_pixel_height = win_px_h;
	out_metrics->window_screen_left = (int32_t)client_origin.x;
	out_metrics->window_screen_top = (int32_t)client_origin.y;

	out_metrics->window_width_m = (float)win_px_w * pixel_size_x;
	out_metrics->window_height_m = (float)win_px_h * pixel_size_y;

	float win_center_px_x = (float)(client_origin.x) + (float)win_px_w / 2.0f;
	float win_center_px_y = (float)(client_origin.y) + (float)win_px_h / 2.0f;
	float disp_center_px_x = (float)disp_px_w / 2.0f;
	float disp_center_px_y = (float)disp_px_h / 2.0f;

	out_metrics->window_center_offset_x_m = (win_center_px_x - disp_center_px_x) * pixel_size_x;
	out_metrics->window_center_offset_y_m = -((win_center_px_y - disp_center_px_y) * pixel_size_y);

	out_metrics->valid = true;
	u_canvas_apply_to_metrics(out_metrics, &c->canvas);
	return true;
#elif defined(__APPLE__)
	if (!c->sys_info_set || c->macos_window == NULL) {
		return false;
	}

	float disp_w_m = c->sys_info.display_width_m;
	float disp_h_m = c->sys_info.display_height_m;
	uint32_t disp_px_w = c->sys_info.display_pixel_width;
	uint32_t disp_px_h = c->sys_info.display_pixel_height;
	if (disp_px_w == 0 || disp_px_h == 0 || disp_w_m <= 0 || disp_h_m <= 0) {
		return false;
	}

	uint32_t win_px_w = 0, win_px_h = 0;
	comp_gl_window_macos_get_dimensions(c->macos_window, &win_px_w, &win_px_h);
	if (win_px_w == 0 || win_px_h == 0) {
		return false;
	}

	float pixel_size_x = disp_w_m / (float)disp_px_w;
	float pixel_size_y = disp_h_m / (float)disp_px_h;

	out_metrics->display_width_m = disp_w_m;
	out_metrics->display_height_m = disp_h_m;
	out_metrics->display_pixel_width = disp_px_w;
	out_metrics->display_pixel_height = disp_px_h;
	out_metrics->display_screen_left = 0;
	out_metrics->display_screen_top = 0;

	out_metrics->window_pixel_width = win_px_w;
	out_metrics->window_pixel_height = win_px_h;
	out_metrics->window_screen_left = 0;
	out_metrics->window_screen_top = 0;

	out_metrics->window_width_m = (float)win_px_w * pixel_size_x;
	out_metrics->window_height_m = (float)win_px_h * pixel_size_y;

	// Center offset (assume centered — no screen position API for GL macOS window yet)
	float win_center_px_x = (float)win_px_w / 2.0f;
	float win_center_px_y = (float)win_px_h / 2.0f;
	float disp_center_px_x = (float)disp_px_w / 2.0f;
	float disp_center_px_y = (float)disp_px_h / 2.0f;

	out_metrics->window_center_offset_x_m = (win_center_px_x - disp_center_px_x) * pixel_size_x;
	out_metrics->window_center_offset_y_m = -((win_center_px_y - disp_center_px_y) * pixel_size_y);

	out_metrics->valid = true;
	u_canvas_apply_to_metrics(out_metrics, &c->canvas);
	return true;
#else
	(void)c;
	return false;
#endif
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

	// Save caller's GL context so we can restore after init
#ifdef XRT_OS_WINDOWS
	HDC caller_hdc = wglGetCurrentDC();
	HGLRC caller_hglrc = wglGetCurrentContext();
#elif defined(__APPLE__)
	CGLContextObj caller_cgl_ctx = CGLGetCurrentContext();
#endif

	// Platform-specific context/window setup
#ifdef XRT_OS_WINDOWS
	if (!gl_create_window_and_context(c, window_handle, gl_context, width, height)) {
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
	// macOS: create window/context via NSOpenGLView helper
	if (shared_texture_handle != NULL) {
		// Shared IOSurface mode: offscreen GL context, render into IOSurface
		xrt_result_t xret = comp_gl_window_macos_create_offscreen(
		    gl_context, &c->macos_window);
		if (xret != XRT_SUCCESS) {
			free(c);
			return XRT_ERROR_OPENGL;
		}
		c->owns_window = false;
		comp_gl_window_macos_make_current(c->macos_window);

		// Map the IOSurface to a GL texture
		GLuint io_tex = 0;
		uint32_t io_w = 0, io_h = 0;
		xret = comp_gl_window_macos_map_iosurface(
		    c->macos_window, shared_texture_handle, &io_tex, &io_w, &io_h);
		if (xret != XRT_SUCCESS) {
			free(c);
			return XRT_ERROR_OPENGL;
		}
		c->iosurface_gl_texture = io_tex;
		c->iosurface_width = io_w;
		c->iosurface_height = io_h;
		c->has_shared_iosurface = true;
	} else if (window_handle != NULL) {
		// App provided an NSView — set up external
		xrt_result_t xret = comp_gl_window_macos_setup_external(
		    window_handle, gl_context, &c->macos_window);
		if (xret != XRT_SUCCESS) {
			free(c);
			return XRT_ERROR_OPENGL;
		}
		c->owns_window = false;
		comp_gl_window_macos_make_current(c->macos_window);
	} else {
		// Create our own window
		xrt_result_t xret = comp_gl_window_macos_create(
		    width, height, gl_context, &c->macos_window);
		if (xret != XRT_SUCCESS) {
			free(c);
			return XRT_ERROR_OPENGL;
		}
		c->owns_window = true;
		comp_gl_window_macos_make_current(c->macos_window);
	}
	(void)gl_display;
#else
	(void)shared_texture_handle;
#endif

	// Scale to Retina physical pixels on macOS.
	// width/height are logical points from screens[0]; the atlas texture
	// and rendering resources must match the actual backing resolution.
#ifdef __APPLE__
	{
		float backing_scale = comp_gl_window_macos_get_backing_scale();
		width = (uint32_t)(width * backing_scale);
		height = (uint32_t)(height * backing_scale);
	}
#endif

	// Create display processor via factory.
	// For hosted apps (no external window), use the compositor's own window
	// so the SR SDK GL weaver gets a valid HWND.
	if (dp_factory_gl != NULL) {
		void *dp_window = window_handle;
#ifdef XRT_OS_WINDOWS
		if (dp_window == NULL && c->hwnd != NULL) {
			dp_window = (void *)c->hwnd;
		}
#endif
		xrt_dp_factory_gl_fn_t factory = (xrt_dp_factory_gl_fn_t)dp_factory_gl;
		xrt_result_t dp_ret = factory(dp_window, &c->display_processor);
		if (dp_ret == XRT_SUCCESS && c->display_processor != NULL) {
			U_LOG_W("GL compositor: display processor created via factory");
		} else {
			U_LOG_W("GL compositor: display processor factory returned %d, using built-in shaders", dp_ret);
			c->display_processor = NULL;
		}
	}

	// Initialize GL resources (atlas worst-case sized, crop before DP per-frame)
	if (!gl_init_resources(c, width, height)) {
		free(c);
		return XRT_ERROR_OPENGL;
	}

	// Create HUD overlay for runtime-owned windows
	if (c->owns_window) {
		u_hud_create(&c->hud, width);
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
	xc->layer_window_space = gl_compositor_layer_window_space;
	xc->layer_commit = gl_compositor_layer_commit;
	xc->destroy = gl_compositor_destroy;

	// Set formats
	gl_compositor_set_formats(c);

	// Visibility/focus flags for state transitions
	xc->info.initial_visible = true;
	xc->info.initial_focused = true;

	*out_xcn = &c->base;

	// Restore caller's GL context (don't leave compositor's context current)
#ifdef XRT_OS_WINDOWS
	if (caller_hglrc != NULL) {
		wglMakeCurrent(caller_hdc, caller_hglrc);
	} else {
		wglMakeCurrent(NULL, NULL);
	}
	// Store app's context for restore in layer_commit
	c->app_hdc = (HDC)gl_display;
#elif defined(__APPLE__)
	if (caller_cgl_ctx != NULL) {
		CGLSetCurrentContext(caller_cgl_ctx);
	}
#endif

	U_LOG_W("Native OpenGL compositor created: %ux%u", width, height);

	return XRT_SUCCESS;
}
