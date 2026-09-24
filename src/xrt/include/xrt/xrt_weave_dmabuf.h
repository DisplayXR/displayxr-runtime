// Copyright 2026, DisplayXR contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Desktop-Linux XR_DXR_weave transport descriptors: a dma-buf with its
 *         DRM format modifier, in both directions (#1699, spec v10 / piece R4).
 *
 * The contract between the IPC server's weave handlers (R4 wire) and the
 * desktop-Linux weave engine comp_multi_weave_linux.c (R2). Nothing here is
 * platform-neutral by accident: an fd is not an identity (every SCM_RIGHTS
 * receive yields a new number), carries no dimensions, no format and no tiling,
 * so everything a Vulkan import with VK_EXT_image_drm_format_modifier needs
 * travels beside it. Values are what `gfx::NativePixmapHandle` / `gbm_bo_*`
 * report on the producer side.
 *
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_config_os.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Max memory planes a dma-buf import carries (Intel CCS modifiers use 2).
#define XRT_WEAVE_DMABUF_MAX_PLANES 4

/*!
 * One dma-buf the client hands the engine (input or overlay).
 *
 * Ownership: the fd is the engine's the moment the handler passes this struct
 * in. A successful Vulkan import consumes it; an import-cache hit closes it; a
 * failed import closes it. The caller never touches it again.
 */
struct xrt_weave_dmabuf_desc
{
	//! The dma-buf. Single fd; all planes are offsets into it (#1699 v1 scope).
	int fd;
	uint32_t width;
	uint32_t height;
	//! DRM fourcc (DRM_FORMAT_ABGR8888 for Chromium's RGBA_8888, ARGB8888 for BGRA_8888).
	uint32_t drm_fourcc;
	//! DRM format modifier, passed through verbatim (never DRM_FORMAT_MOD_INVALID).
	uint64_t drm_modifier;
	uint32_t plane_count;
	uint32_t offsets[XRT_WEAVE_DMABUF_MAX_PLANES];
	uint32_t strides[XRT_WEAVE_DMABUF_MAX_PLANES];
	/*!
	 * Caller-chosen stable identity of the underlying buffer (0 = none). The
	 * engine's import cache keys on this when present and on the fd's
	 * (st_dev, st_ino) otherwise, so a producer rotating a small pool does not
	 * re-import every frame.
	 */
	uint64_t buffer_id;
};

/*!
 * The woven output the engine exports to the client.
 *
 * Ownership: the engine owns the fd and hands out ONE dup per export call; the
 * transport (SCM_RIGHTS) dups again on send and the client owns what it
 * receives. Lifetime follows the output allocation: exported on the first frame
 * and on every reallocation, unchanged between.
 */
struct xrt_weave_dmabuf_output_desc
{
	int fd;
	uint32_t width;
	uint32_t height;
	uint32_t drm_fourcc;
	uint64_t drm_modifier;
	uint32_t plane_count;
	uint32_t offsets[XRT_WEAVE_DMABUF_MAX_PLANES];
	uint32_t strides[XRT_WEAVE_DMABUF_MAX_PLANES];
	//! Total allocation size in bytes (what an OPAQUE_FD-style importer needs).
	uint64_t size;
};

#ifdef __cplusplus
}
#endif
