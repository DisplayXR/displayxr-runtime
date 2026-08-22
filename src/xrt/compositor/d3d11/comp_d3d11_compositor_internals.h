// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The handful of compositor-owned handles the renderer and the
 *         swapchain need, handed over explicitly instead of type-punned.
 * @ingroup comp_d3d11
 *
 * `struct comp_d3d11_compositor` is defined privately inside
 * comp_d3d11_compositor.cpp. Two sibling translation units — the renderer and
 * the swapchain — nevertheless need four handles out of it, and for a long time
 * each got them by declaring its OWN struct that mirrored the compositor's
 * first five members and `reinterpret_cast`ing the opaque pointer onto it.
 *
 * That is a type pun on a hand-maintained layout, and it had all three of the
 * failure modes such a thing has:
 *
 *   - it was **silent**. Inserting a member anywhere in the prefix shifted every
 *     mirror and handed the reader a wrong pointer with no diagnostic, which is
 *     why comp_d3d11_compositor.cpp had to carry four `offsetof` static_asserts
 *     purely to guard the layout the mirrors assumed;
 *   - it was **duplicated**. Two TUs defined the same struct tag with the same
 *     members, so keeping them in step was a convention, not a rule — and the
 *     compositor grew #918's output-device members with an explicit "append
 *     BELOW mt_lock" comment to work around it;
 *   - it **inverted the dependency**. The compositor could not reorder its own
 *     private fields without knowing who was reading them from outside.
 *
 * So the four handles are handed over by value instead. The compositor fills
 * this struct from the real one; nobody casts anything, nothing has to stay in
 * a particular order, and the tripwire is gone because there is no longer a
 * layout to trip over.
 *
 * The handles are BORROWED — no AddRef — and are fixed for the compositor's
 * lifetime, so a caller may cache the struct for the duration of one call as
 * the mirrors' `get_internals()` locals already did.
 */

#pragma once

#include "xrt/xrt_device.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>

struct comp_d3d11_compositor;

#ifdef __cplusplus
extern "C" {
#endif

//! Compositor-owned handles the renderer and the swapchain read.
struct comp_d3d11_compositor_internals
{
	//! The device we display to.
	struct xrt_device *xdev;
	//! The APP's D3D11 device — never the #918 output device.
	ID3D11Device *device;
	//! The app device's immediate context.
	ID3D11DeviceContext *context;
	//! DXGI factory for swapchain creation.
	IDXGIFactory4 *dxgi_factory;
};

/*!
 * Copy out @p c's borrowed handles. Never fails; @p c must be non-NULL.
 */
struct comp_d3d11_compositor_internals
comp_d3d11_compositor_get_internals(struct comp_d3d11_compositor *c);

#ifdef __cplusplus
}
#endif
