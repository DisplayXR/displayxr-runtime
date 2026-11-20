// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  xrWeaveSubmitDXR's dma-buf descriptor lookups are order-independent.
 * @author David Fattal
 *
 * XR_DXR_cursor_depth spec v10 (ADR-046 Amendment 6). The weave-rect cursor's
 * TEXTURE sprite may be a dma-buf on desktop Linux, and so may the submit's
 * input. Both descriptors ride ONE next chain (the cursor request is itself a
 * link of it), so they have different structure types and each lookup goes by
 * type over the whole chain: whatever the order of the links, the input lookup
 * returns the input's descriptor and the sprite lookup the sprite's, and
 * neither ever returns the other's. These tests pin that for every order.
 */

#include "oxr_weave_chain.h"

#include "catch_amalgamated.hpp"

#include <algorithm>
#include <vector>

namespace {

struct Links
{
	XrWeaveSubmitInfoDXR submit = {(XrStructureType)XR_TYPE_WEAVE_SUBMIT_INFO_DXR};
	XrWeaveSubmitRectsDXR rects = {(XrStructureType)XR_TYPE_WEAVE_SUBMIT_RECTS_DXR};
	XrWeaveDmabufDescDXR input = {(XrStructureType)XR_TYPE_WEAVE_DMABUF_DESC_DXR};
	XrWeaveOverlayDmabufDescDXR overlay = {(XrStructureType)XR_TYPE_WEAVE_OVERLAY_DMABUF_DESC_DXR};
	XrCursorDepthWeaveRequestDXR request = {(XrStructureType)XR_TYPE_CURSOR_DEPTH_WEAVE_REQUEST_DXR};
	XrCursorDepthWeaveSpriteDmabufDescDXR sprite = {
	    (XrStructureType)XR_TYPE_CURSOR_DEPTH_WEAVE_SPRITE_DMABUF_DESC_DXR};

	Links()
	{
		input.fd = 11;
		overlay.fd = 22;
		sprite.fd = 33;
	}

	//! Chain the given links behind the submit info, in this order.
	void
	chain(const std::vector<XrBaseInStructure *> &order)
	{
		const XrBaseInStructure **tail = (const XrBaseInStructure **)&submit.next;
		*tail = nullptr;
		for (XrBaseInStructure *l : order) {
			l->next = nullptr;
			*tail = l;
			tail = &l->next;
		}
	}
};

XrBaseInStructure *
base(void *p)
{
	return (XrBaseInStructure *)p;
}

} // namespace

TEST_CASE("weave chain: the input and the sprite dma-buf descriptors are found in EVERY chain order")
{
	Links l;
	// All 5! orders of rects, input, overlay, request, sprite.
	std::vector<XrBaseInStructure *> order = {base(&l.rects), base(&l.input), base(&l.overlay), base(&l.request),
	                                          base(&l.sprite)};
	std::sort(order.begin(), order.end());
	int orders = 0;
	do {
		l.chain(order);
		orders++;
		const XrWeaveDmabufDescDXR *in = oxr_weave_chain_input_dmabuf(&l.submit);
		const XrCursorDepthWeaveSpriteDmabufDescDXR *sp = oxr_weave_chain_sprite_dmabuf(&l.submit);
		REQUIRE(in == &l.input);
		REQUIRE(sp == &l.sprite);
		REQUIRE(in->fd == 11);
		REQUIRE(sp->fd == 33);
		REQUIRE(oxr_weave_chain_cursor_request(&l.submit) == &l.request);
	} while (std::next_permutation(order.begin(), order.end()));
	CHECK(orders == 120);
}

TEST_CASE("weave chain: the two orders that used to decide which descriptor was whose")
{
	Links l;
	// The request FIRST, then the sprite's descriptor, then the input's: a
	// lookup by XrWeaveDmabufDescDXR alone would have taken the sprite's
	// buffer for the input.
	l.chain({base(&l.rects), base(&l.request), base(&l.sprite), base(&l.input)});
	CHECK(oxr_weave_chain_input_dmabuf(&l.submit) == &l.input);
	CHECK(oxr_weave_chain_sprite_dmabuf(&l.submit) == &l.sprite);

	// The request first and the input's descriptor after it, no sprite
	// descriptor: a lookup "from the request on" would have taken the input's
	// buffer for the sprite.
	l.chain({base(&l.rects), base(&l.request), base(&l.input)});
	CHECK(oxr_weave_chain_input_dmabuf(&l.submit) == &l.input);
	CHECK(oxr_weave_chain_sprite_dmabuf(&l.submit) == nullptr);

	// The order that always worked.
	l.chain({base(&l.rects), base(&l.input), base(&l.request), base(&l.sprite)});
	CHECK(oxr_weave_chain_input_dmabuf(&l.submit) == &l.input);
	CHECK(oxr_weave_chain_sprite_dmabuf(&l.submit) == &l.sprite);
}

TEST_CASE("weave chain: neither lookup ever accepts the other's structure type")
{
	Links l;
	// Only a sprite descriptor: there is no input descriptor.
	l.chain({base(&l.rects), base(&l.request), base(&l.sprite)});
	CHECK(oxr_weave_chain_input_dmabuf(&l.submit) == nullptr);
	CHECK(oxr_weave_chain_sprite_dmabuf(&l.submit) == &l.sprite);

	// Only an input (and an overlay) descriptor: there is no sprite descriptor.
	l.chain({base(&l.input), base(&l.overlay), base(&l.request)});
	CHECK(oxr_weave_chain_input_dmabuf(&l.submit) == &l.input);
	CHECK(oxr_weave_chain_sprite_dmabuf(&l.submit) == nullptr);

	// Nothing chained.
	l.chain({});
	CHECK(oxr_weave_chain_input_dmabuf(&l.submit) == nullptr);
	CHECK(oxr_weave_chain_sprite_dmabuf(&l.submit) == nullptr);
	CHECK(oxr_weave_chain_cursor_request(&l.submit) == nullptr);

	// The three types are distinct values.
	CHECK((int)XR_TYPE_CURSOR_DEPTH_WEAVE_SPRITE_DMABUF_DESC_DXR != (int)XR_TYPE_WEAVE_DMABUF_DESC_DXR);
	CHECK((int)XR_TYPE_CURSOR_DEPTH_WEAVE_SPRITE_DMABUF_DESC_DXR != (int)XR_TYPE_WEAVE_OVERLAY_DMABUF_DESC_DXR);
	CHECK((int)XR_TYPE_CURSOR_DEPTH_WEAVE_SPRITE_DMABUF_DESC_DXR == 1004999391);
}

TEST_CASE("weave chain: the sprite descriptor is XrWeaveDmabufDescDXR field for field")
{
	CHECK(sizeof(XrCursorDepthWeaveSpriteDmabufDescDXR) == sizeof(XrWeaveDmabufDescDXR));
	CHECK(offsetof(XrCursorDepthWeaveSpriteDmabufDescDXR, fd) == offsetof(XrWeaveDmabufDescDXR, fd));
	CHECK(offsetof(XrCursorDepthWeaveSpriteDmabufDescDXR, width) == offsetof(XrWeaveDmabufDescDXR, width));
	CHECK(offsetof(XrCursorDepthWeaveSpriteDmabufDescDXR, height) == offsetof(XrWeaveDmabufDescDXR, height));
	CHECK(offsetof(XrCursorDepthWeaveSpriteDmabufDescDXR, drmFourcc) == offsetof(XrWeaveDmabufDescDXR, drmFourcc));
	CHECK(offsetof(XrCursorDepthWeaveSpriteDmabufDescDXR, drmModifier) ==
	      offsetof(XrWeaveDmabufDescDXR, drmModifier));
	CHECK(offsetof(XrCursorDepthWeaveSpriteDmabufDescDXR, planeCount) ==
	      offsetof(XrWeaveDmabufDescDXR, planeCount));
	CHECK(offsetof(XrCursorDepthWeaveSpriteDmabufDescDXR, offsets) == offsetof(XrWeaveDmabufDescDXR, offsets));
	CHECK(offsetof(XrCursorDepthWeaveSpriteDmabufDescDXR, strides) == offsetof(XrWeaveDmabufDescDXR, strides));
	CHECK(offsetof(XrCursorDepthWeaveSpriteDmabufDescDXR, bufferId) == offsetof(XrWeaveDmabufDescDXR, bufferId));
}

TEST_CASE("weave chain: a TEXTURE sprite's carrier on desktop Linux, from its kind and its own descriptor")
{
	const char *why = nullptr;
	// PLATFORM_DEFAULT with the sprite descriptor still means dma-buf.
	CHECK(oxr_weave_sprite_carrier_linux(XR_WEAVE_HANDLE_KIND_PLATFORM_DEFAULT_DXR, true, &why) ==
	      OXR_WEAVE_SPRITE_CARRIER_DMABUF);
	CHECK(why == nullptr);
	CHECK(oxr_weave_sprite_carrier_linux(XR_WEAVE_HANDLE_KIND_PLATFORM_DEFAULT_DXR, false, &why) ==
	      OXR_WEAVE_SPRITE_CARRIER_OPAQUE_FD);
	CHECK(oxr_weave_sprite_carrier_linux(XR_WEAVE_HANDLE_KIND_DMABUF_DXR, true, &why) ==
	      OXR_WEAVE_SPRITE_CARRIER_DMABUF);
	CHECK(oxr_weave_sprite_carrier_linux(XR_WEAVE_HANDLE_KIND_OPAQUE_FD_DXR, false, &why) ==
	      OXR_WEAVE_SPRITE_CARRIER_OPAQUE_FD);
	CHECK(why == nullptr);

	// The contradictions are refused, with a reason.
	CHECK(oxr_weave_sprite_carrier_linux(XR_WEAVE_HANDLE_KIND_DMABUF_DXR, false, &why) ==
	      OXR_WEAVE_SPRITE_CARRIER_INVALID);
	CHECK(why != nullptr);
	why = nullptr;
	CHECK(oxr_weave_sprite_carrier_linux(XR_WEAVE_HANDLE_KIND_OPAQUE_FD_DXR, true, &why) ==
	      OXR_WEAVE_SPRITE_CARRIER_INVALID);
	CHECK(why != nullptr);
	CHECK(oxr_weave_sprite_carrier_linux(XR_WEAVE_HANDLE_KIND_IOSURFACE_DXR, true, &why) ==
	      OXR_WEAVE_SPRITE_CARRIER_INVALID);
	CHECK(oxr_weave_sprite_carrier_linux(XR_WEAVE_HANDLE_KIND_D3D11_NT_DXR, false, nullptr) ==
	      OXR_WEAVE_SPRITE_CARRIER_INVALID);

	// An end-to-end chain: a dma-buf INPUT with an OPAQUE_FD sprite. The
	// input's descriptor does not make the sprite a dma-buf.
	Links l;
	l.chain({base(&l.request), base(&l.input)});
	CHECK(oxr_weave_sprite_carrier_linux(XR_WEAVE_HANDLE_KIND_PLATFORM_DEFAULT_DXR,
	                                     oxr_weave_chain_sprite_dmabuf(&l.submit) != nullptr,
	                                     &why) == OXR_WEAVE_SPRITE_CARRIER_OPAQUE_FD);
}
