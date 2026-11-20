// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Where xrWeaveSubmitDXR finds its dma-buf descriptors on the submit's
 *         next chain, and which carrier a TEXTURE cursor sprite resolves to.
 * @author David Fattal
 * @ingroup oxr_main
 *
 * A next chain is ONE linked list. XrCursorDepthWeaveRequestDXR is itself a
 * link of the submit's chain, so "chained on the request" names no sub-chain:
 * everything after the request is equally after the submit info. Two structs
 * of one type on that list are told apart by nothing but their order.
 *
 * That is why the cursor sprite's dma-buf has its own structure type
 * (XrCursorDepthWeaveSpriteDmabufDescDXR, XR_DXR_cursor_depth spec v10): the
 * INPUT is XrWeaveDmabufDescDXR, the OVERLAY XrWeaveOverlayDmabufDescDXR, the
 * SPRITE this one - three types, each at most once, so every lookup walks the
 * whole chain from the submit info and the order of the links never matters.
 * The input lookup cannot return the sprite's descriptor and the sprite
 * lookup cannot return the input's.
 *
 * Header-only and free of runtime state, so tests_oxr_weave_chain pins it.
 */

#pragma once

#include <openxr/openxr.h>
#include <openxr/XR_DXR_weave.h>
#include <openxr/XR_DXR_cursor_depth.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

//! The first struct of @p type on the chain starting AT @p head (NULL if none).
static inline const void *
oxr_weave_chain_find(const void *head, XrStructureType type)
{
	const XrBaseInStructure *b = (const XrBaseInStructure *)head;
	while (b != NULL) {
		if (b->type == type) {
			return b;
		}
		b = b->next;
	}
	return NULL;
}

//! The submit's INPUT dma-buf descriptor: XrWeaveDmabufDescDXR, anywhere on the chain.
static inline const XrWeaveDmabufDescDXR *
oxr_weave_chain_input_dmabuf(const XrWeaveSubmitInfoDXR *submit_info)
{
	return (const XrWeaveDmabufDescDXR *)oxr_weave_chain_find(submit_info,
	                                                          (XrStructureType)XR_TYPE_WEAVE_DMABUF_DESC_DXR);
}

//! The cursor request, anywhere on the chain.
static inline const XrCursorDepthWeaveRequestDXR *
oxr_weave_chain_cursor_request(const XrWeaveSubmitInfoDXR *submit_info)
{
	return (const XrCursorDepthWeaveRequestDXR *)oxr_weave_chain_find(
	    submit_info, (XrStructureType)XR_TYPE_CURSOR_DEPTH_WEAVE_REQUEST_DXR);
}

/*!
 * The TEXTURE sprite's dma-buf descriptor: XrCursorDepthWeaveSpriteDmabufDescDXR,
 * anywhere on the chain - before or after the request, before or after the
 * input's descriptor. Never an XrWeaveDmabufDescDXR.
 */
static inline const XrCursorDepthWeaveSpriteDmabufDescDXR *
oxr_weave_chain_sprite_dmabuf(const XrWeaveSubmitInfoDXR *submit_info)
{
	return (const XrCursorDepthWeaveSpriteDmabufDescDXR *)oxr_weave_chain_find(
	    submit_info, (XrStructureType)XR_TYPE_CURSOR_DEPTH_WEAVE_SPRITE_DMABUF_DESC_DXR);
}

//! What a TEXTURE sprite travels as on desktop Linux.
enum oxr_weave_sprite_carrier
{
	OXR_WEAVE_SPRITE_CARRIER_INVALID = 0,
	//! The dma-buf of the sprite descriptor (sprite.texture is ignored).
	OXR_WEAVE_SPRITE_CARRIER_DMABUF,
	//! A same-driver Vulkan OPAQUE_FD in sprite.texture.
	OXR_WEAVE_SPRITE_CARRIER_OPAQUE_FD,
};

/*!
 * Resolve a TEXTURE sprite's carrier on desktop Linux from its textureKind and
 * whether the sprite descriptor is chained:
 *
 *   DMABUF            + descriptor     -> DMABUF
 *   DMABUF            + no descriptor  -> invalid
 *   OPAQUE_FD         + no descriptor  -> OPAQUE_FD
 *   OPAQUE_FD         + descriptor     -> invalid (which one is meant?)
 *   PLATFORM_DEFAULT  + descriptor     -> DMABUF
 *   PLATFORM_DEFAULT  + no descriptor  -> OPAQUE_FD
 *   any other kind                     -> invalid
 *
 * The INPUT's XrWeaveDmabufDescDXR plays no part: a dma-buf input with an
 * OPAQUE_FD sprite, and the reverse, are both fine.
 */
static inline enum oxr_weave_sprite_carrier
oxr_weave_sprite_carrier_linux(XrWeaveHandleKindDXR kind, bool have_sprite_desc, const char **out_why)
{
	const char *why = NULL;
	enum oxr_weave_sprite_carrier c = OXR_WEAVE_SPRITE_CARRIER_INVALID;
	switch (kind) {
	case XR_WEAVE_HANDLE_KIND_DMABUF_DXR:
		if (have_sprite_desc) {
			c = OXR_WEAVE_SPRITE_CARRIER_DMABUF;
		} else {
			why =
			    "textureKind DMABUF needs an XrCursorDepthWeaveSpriteDmabufDescDXR on the submit's next "
			    "chain";
		}
		break;
	case XR_WEAVE_HANDLE_KIND_OPAQUE_FD_DXR:
		if (!have_sprite_desc) {
			c = OXR_WEAVE_SPRITE_CARRIER_OPAQUE_FD;
		} else {
			why = "textureKind OPAQUE_FD must not come with an XrCursorDepthWeaveSpriteDmabufDescDXR";
		}
		break;
	case XR_WEAVE_HANDLE_KIND_PLATFORM_DEFAULT_DXR:
		c = have_sprite_desc ? OXR_WEAVE_SPRITE_CARRIER_DMABUF : OXR_WEAVE_SPRITE_CARRIER_OPAQUE_FD;
		break;
	default: why = "textureKind must be DMABUF, OPAQUE_FD or PLATFORM_DEFAULT on desktop Linux"; break;
	}
	if (out_why != NULL) {
		*out_why = why;
	}
	return c;
}

#ifdef __cplusplus
}
#endif
