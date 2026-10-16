// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Depth swapchains for OpenGL clients over IPC (WGL_NV_DX_interop2 path):
 *         which GL depth formats can be backed, and how one is moved into the
 *         service's shared depth image. Pure, and therefore testable (#1956).
 * @ingroup comp_client
 *
 * The D3D11 service allocates a depth swapchain image as a TYPELESS,
 * NT-handle-shared, keyed-mutex texture (`R32_TYPELESS` / `R16_TYPELESS`, bind
 * `DEPTH_STENCIL | SHADER_RESOURCE`; comp_d3d11_service_formats.h). The GL
 * client cannot hand that texture to the app directly:
 *
 *  - WGL_NV_DX_interop2 defines no D3D→GL format mapping at all, says nothing
 *    about TYPELESS or keyed-mutex resources, and its only depth example
 *    registers a depth-stencil surface as a GL **renderbuffer**. The OpenXR GL
 *    binding needs a **texture** name (`XrSwapchainImageOpenGLKHR::image`), so
 *    even where a driver accepts depth as a renderbuffer it cannot be a
 *    swapchain image, and depth-as-texture registration is undefined behaviour
 *    that varies by vendor.
 *  - The colour path already never registers the shared texture: it registers a
 *    device-local `SRV | RT` staging copy and `CopyResource`s it into the shared
 *    image under the keyed mutex at release (#1995). A depth family has no
 *    RENDER_TARGET, so that staging recipe cannot be created for depth as-is.
 *
 * So a depth image here is two objects:
 *
 *  1. **The app's image** — a GL-native texture with exactly the requested
 *     depth internal format (`GL_DEPTH_COMPONENT32F` / `GL_DEPTH_COMPONENT16`),
 *     2D or 2D_ARRAY. The app attaches it as `GL_DEPTH_ATTACHMENT` like any
 *     depth texture; `glGetTexLevelParameteriv(GL_TEXTURE_INTERNAL_FORMAT)`
 *     reports the format it asked for.
 *  2. **The interop image** — a D3D11 staging texture in a *colour* member of
 *     the shared image's typeless family (`R32_FLOAT` for `R32_TYPELESS`,
 *     `R16_UNORM` for `R16_TYPELESS`), `SRV | RT` like the colour staging, and
 *     registered with wglDXRegisterObjectNV as an ordinary colour texture —
 *     the one thing every vendor's interop does support.
 *
 * At xrReleaseSwapchainImage the depth bits move 1 → 2 entirely on the GPU and
 * with no shader: `glGetTexImage(GL_DEPTH_COMPONENT, <type>)` into a pixel-pack
 * buffer, then `glTexSubImage(GL_RED, <same type>)` from it as a pixel-unpack
 * buffer — all mips, all array layers in one call each. The read type equals the
 * stored type (FLOAT for D32F, UNSIGNED_SHORT for D16) and the interop format
 * has the same bits per texel, so the copy is bit-exact: no conversion, no
 * clamping. `glCopyImageSubData` cannot do this (ARB_copy_image: depth formats
 * are compatible only with themselves) and neither can `glBlitFramebuffer`
 * (depth blits only into depth). Then the existing release path
 * `CopyResource`s staging → shared under the keyed mutex; D3D11 copies freely
 * within a typeless family, which is what makes the colour-typed staging a
 * legal source for the depth-bound shared image (WARP-checked in
 * tests_comp_gl_d3d11_depth).
 *
 * Stencil formats have no honest backing: the `R24G8_TYPELESS` and
 * `R32G8X24_TYPELESS` families have no RENDER_TARGET member, so there is no
 * interop staging to copy through (and the service's NT-handle probe already
 * refuses both on WARP). They are not offered — never offered-then-failed.
 */

#pragma once

#include "ogl/ogl_api.h"

#include <dxgiformat.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/*!
 * How one GL depth format is carried to the D3D11 service.
 */
struct client_gl_d3d11_depth_recipe
{
	//! False: this format cannot be backed on this path and must not be offered.
	bool supported;
	//! The D3D11 interop staging texture (colour member of the shared image's family).
	DXGI_FORMAT interop_dxgi;
	//! glGetTexImage format/type reading the app's depth texture.
	GLenum read_format;
	GLenum read_type;
	//! glTexSubImage format/type writing the interop texture.
	GLenum upload_format;
	GLenum upload_type;
	//! Bytes per texel in the transfer buffer (read and upload agree).
	uint32_t bytes_per_texel;
};

//! Is @p gl_format one of the GL depth(-stencil) internal formats?
static inline bool
client_gl_d3d11_is_depth_format(int64_t gl_format)
{
	switch (gl_format) {
	case GL_DEPTH_COMPONENT16:
	case GL_DEPTH_COMPONENT24:
	case GL_DEPTH_COMPONENT32F:
	case GL_DEPTH24_STENCIL8:
	case GL_DEPTH32F_STENCIL8: return true;
	default: return false;
	}
}

/*!
 * The recipe for a depth format; `.supported == false` for colour formats and
 * for depth formats this path cannot back.
 */
static inline struct client_gl_d3d11_depth_recipe
client_gl_d3d11_depth_recipe_for(int64_t gl_format)
{
	struct client_gl_d3d11_depth_recipe r = {false, DXGI_FORMAT_UNKNOWN, 0, 0, 0, 0, 0};
	switch (gl_format) {
	case GL_DEPTH_COMPONENT32F:
		r.supported = true;
		r.interop_dxgi = DXGI_FORMAT_R32_FLOAT; // R32_TYPELESS family (service: D32_FLOAT)
		r.read_format = GL_DEPTH_COMPONENT;
		r.read_type = GL_FLOAT;
		r.upload_format = GL_RED;
		r.upload_type = GL_FLOAT;
		r.bytes_per_texel = 4;
		break;
	case GL_DEPTH_COMPONENT16:
		r.supported = true;
		r.interop_dxgi = DXGI_FORMAT_R16_UNORM; // R16_TYPELESS family (service: D16_UNORM)
		r.read_format = GL_DEPTH_COMPONENT;
		r.read_type = GL_UNSIGNED_SHORT;
		r.upload_format = GL_RED;
		r.upload_type = GL_UNSIGNED_SHORT;
		r.bytes_per_texel = 2;
		break;
	default: break;
	}
	return r;
}

/*!
 * Filter the GL formats derived from the service's advertised list in place:
 * every colour format is kept, a depth format only when it has a recipe.
 * Order is preserved (colour stays first). Returns the new count.
 */
static inline uint32_t
client_gl_d3d11_filter_formats(int64_t *formats, uint32_t count)
{
	uint32_t kept = 0;
	for (uint32_t i = 0; i < count; i++) {
		const int64_t f = formats[i];
		if (client_gl_d3d11_is_depth_format(f) && !client_gl_d3d11_depth_recipe_for(f).supported) {
			continue;
		}
		formats[kept++] = f;
	}
	return kept;
}

//! Extent of mip @p level for a base extent (never below 1).
static inline uint32_t
client_gl_d3d11_mip_extent(uint32_t base, uint32_t level)
{
	const uint32_t e = level < 32 ? (base >> level) : 0;
	return e > 0 ? e : 1;
}

/*!
 * Byte offset of mip @p level in the transfer buffer, laid out tightly
 * (pack/unpack alignment 1) level after level, each level holding all
 * @p layers. `client_gl_d3d11_depth_level_offset(r, w, h, layers, mips)` is
 * therefore the whole buffer size.
 */
static inline uint64_t
client_gl_d3d11_depth_level_offset(
    const struct client_gl_d3d11_depth_recipe *r, uint32_t width, uint32_t height, uint32_t layers, uint32_t level)
{
	uint64_t offset = 0;
	const uint32_t l = layers > 0 ? layers : 1;
	for (uint32_t i = 0; i < level; i++) {
		offset += (uint64_t)client_gl_d3d11_mip_extent(width, i) * client_gl_d3d11_mip_extent(height, i) * l *
		          r->bytes_per_texel;
	}
	return offset;
}

/*!
 * Can a depth swapchain with this shape be carried? The PBO transfer reads a
 * 2D / 2D_ARRAY texture; multisample textures cannot be read with
 * glGetTexImage and a depth cube map is not a projection-layer depth image.
 * Such a request fails xrCreateSwapchain instead of returning a broken image.
 */
static inline bool
client_gl_d3d11_depth_shape_supported(uint32_t sample_count, uint32_t face_count)
{
	return sample_count <= 1 && face_count <= 1;
}


#ifdef __cplusplus
}
#endif
