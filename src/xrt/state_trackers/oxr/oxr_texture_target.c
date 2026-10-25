// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_win32_window_binding v9 — texture target binding (ADR-060,
 *         docs/specs/extensions/XR_DXR_win32_window_binding_v9_texture_target.md).
 * @ingroup oxr_main
 *
 * A shared-texture session names where its texture is drawn: a reference
 * window plus a rect in its client area. This file is the OpenXR glue: the
 * create-time validation, the per-frame struct on xrEndFrame / xrLocateViews,
 * the state reply on xrWaitFrame and the system property. The geometry itself
 * (one resolve → phase + metrics + segments) is the compositor's
 * (compositor/util/comp_texture_target*.h). Native D3D11 only for now; D3D12,
 * GL and Vulkan get it through the ADR-059 hub (ADR-060 decision 3).
 */

#include "xrt/xrt_compiler.h"

#include "util/u_logging.h"

#include "oxr_objects.h"
#include "oxr_logger.h"
#include "oxr_chain.h"

#ifdef OXR_HAVE_DXR_win32_window_binding

#include <windows.h>
#include <string.h>

#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
#include "d3d11/comp_d3d11_compositor.h"
#include "util/comp_dp_factory.h"
#include "xrt/xrt_plugin.h" // XRT_DP_API_BIT_HINT_WINDOWLESS_PHASE
#endif

const XrWin32TextureTargetBindingDXR *
oxr_texture_target_find_binding(const XrSessionCreateInfo *createInfo)
{
	const XrWin32WindowBindingCreateInfoDXR *wb = OXR_GET_INPUT_FROM_CHAIN(
	    createInfo, XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_DXR, XrWin32WindowBindingCreateInfoDXR);
	if (wb == NULL) {
		return NULL;
	}
	return OXR_GET_INPUT_FROM_CHAIN(wb->next, XR_TYPE_WIN32_TEXTURE_TARGET_BINDING_DXR,
	                                XrWin32TextureTargetBindingDXR);
}

//! A reference window must be a live top-level window of THIS process (spec §3).
static bool
reference_window_valid(void *ref)
{
	if (ref == NULL) {
		return true;
	}
	HWND h = (HWND)ref;
	if (!IsWindow(h) || GetAncestor(h, GA_ROOT) != h) {
		return false;
	}
	DWORD pid = 0;
	GetWindowThreadProcessId(h, &pid);
	return pid == GetCurrentProcessId();
}

XrResult
oxr_texture_target_verify_create(struct oxr_logger *log, const XrSessionCreateInfo *createInfo)
{
	const XrWin32TextureTargetBindingDXR *tb = oxr_texture_target_find_binding(createInfo);
	if (tb == NULL) {
		return XR_SUCCESS;
	}
	const XrWin32WindowBindingCreateInfoDXR *wb = OXR_GET_INPUT_FROM_CHAIN(
	    createInfo, XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_DXR, XrWin32WindowBindingCreateInfoDXR);
	if (wb->sharedTextureHandle == NULL) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE,
		                 "(XrWin32TextureTargetBindingDXR) requires XrWin32WindowBindingCreateInfoDXR::"
		                 "sharedTextureHandle: a target binding only describes a texture target");
	}
	if (wb->readbackCallback != NULL) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE,
		                 "(XrWin32TextureTargetBindingDXR) readbackCallback must be NULL");
	}
	if (!reference_window_valid(tb->referenceWindow)) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE,
		                 "(XrWin32TextureTargetBindingDXR::referenceWindow) %p is not a live top-level window of "
		                 "the calling process",
		                 tb->referenceWindow);
	}
	if (tb->initialTargetRect.extent.width <= 0 || tb->initialTargetRect.extent.height <= 0) {
		return oxr_error(log, XR_ERROR_VALIDATION_FAILURE,
		                 "(XrWin32TextureTargetBindingDXR::initialTargetRect) extent {%d, %d} must be positive",
		                 tb->initialTargetRect.extent.width, tb->initialTargetRect.extent.height);
	}
	return XR_SUCCESS;
}

#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
void
oxr_texture_target_fill_d3d11_info(struct oxr_system *sys,
                                   const XrWin32TextureTargetBindingDXR *tb,
                                   uint64_t bound_display_id,
                                   struct comp_d3d11_texture_target_info *out)
{
	memset(out, 0, sizeof(*out));
	if (tb == NULL) {
		return;
	}
	out->bound = true;
	out->reference_window = tb->referenceWindow;
	out->x = tb->initialTargetRect.offset.x;
	out->y = tb->initialTargetRect.offset.y;
	out->w = (uint32_t)tb->initialTargetRect.extent.width;
	out->h = (uint32_t)tb->initialTargetRect.extent.height;
	out->windowless_dp_entry =
	    sys->xsysc != NULL ? (const void *)comp_dp_entry_for_session(&sys->xsysc->info, bound_display_id) : NULL;
}
#endif

/*!
 * Validate + forward one XrWin32TextureTargetFrameDXR. @p at_frame_end: an
 * xrEndFrame value (errors are returned), else a locate (best effort).
 */
static XrResult
forward_frame(struct oxr_logger *log, struct oxr_session *sess, const XrWin32TextureTargetFrameDXR *tf, bool at_frame_end)
{
	if (tf->targetRect.extent.width <= 0 || tf->targetRect.extent.height <= 0) {
		return at_frame_end ? oxr_error(log, XR_ERROR_VALIDATION_FAILURE,
		                                "(XrWin32TextureTargetFrameDXR::targetRect) extent {%d, %d} must be positive",
		                                tf->targetRect.extent.width, tf->targetRect.extent.height)
		                    : XR_SUCCESS;
	}
	if (tf->referenceWindow != NULL && !IsWindow((HWND)tf->referenceWindow)) {
		return at_frame_end ? oxr_error(log, XR_ERROR_VALIDATION_FAILURE,
		                                "(XrWin32TextureTargetFrameDXR::referenceWindow) %p was destroyed",
		                                tf->referenceWindow)
		                    : XR_SUCCESS;
	}
#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
	if (sess->is_d3d11_native_compositor && sess->xcn != NULL) {
		comp_d3d11_compositor_set_texture_target(
		    &sess->xcn->base, !at_frame_end, tf->referenceWindow, tf->targetRect.offset.x,
		    tf->targetRect.offset.y, (uint32_t)tf->targetRect.extent.width,
		    (uint32_t)tf->targetRect.extent.height, (uint64_t)tf->flags);
	}
#endif
	return XR_SUCCESS;
}

XrResult
oxr_texture_target_frame_end(struct oxr_logger *log, struct oxr_session *sess, const XrFrameEndInfo *frameEndInfo)
{
	if (!sess->texture_target_bound) {
		return XR_SUCCESS; // inert outside a bound session
	}
	const XrWin32TextureTargetFrameDXR *tf = OXR_GET_INPUT_FROM_CHAIN(
	    frameEndInfo, XR_TYPE_WIN32_TEXTURE_TARGET_FRAME_DXR, XrWin32TextureTargetFrameDXR);
	if (tf == NULL) {
		return XR_SUCCESS; // the last value is reused (spec §3)
	}
	return forward_frame(log, sess, tf, true);
}

void
oxr_texture_target_locate(struct oxr_logger *log, struct oxr_session *sess, const XrViewLocateInfo *viewLocateInfo)
{
	if (!sess->texture_target_bound) {
		return;
	}
	const XrWin32TextureTargetFrameDXR *tf = OXR_GET_INPUT_FROM_CHAIN(
	    viewLocateInfo, XR_TYPE_WIN32_TEXTURE_TARGET_FRAME_DXR, XrWin32TextureTargetFrameDXR);
	if (tf != NULL) {
		(void)forward_frame(log, sess, tf, false);
	}
}

void
oxr_texture_target_locate_end(struct oxr_session *sess)
{
	if (!sess->texture_target_bound) {
		return;
	}
#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
	if (sess->is_d3d11_native_compositor && sess->xcn != NULL) {
		comp_d3d11_compositor_end_texture_target_locate(&sess->xcn->base);
	}
#endif
}

void
oxr_texture_target_note_ignored(struct oxr_session *sess, const XrSessionCreateInfo *createInfo)
{
	if (sess->texture_target_bound || oxr_texture_target_find_binding(createInfo) == NULL) {
		return;
	}
	// Spec v9 is honoured by the in-process D3D11 compositor only (ADR-060
	// decision 3). An app that dropped windowHandle trusting it would weave
	// display-scoped with nothing in the log otherwise.
	U_LOG_W(
	    "xrCreateSession: XrWin32TextureTargetBindingDXR IGNORED — only the in-process D3D11 compositor honours "
	    "the texture target binding (D3D12 / GL / Vulkan arrive via the hub; IPC sessions are not covered by "
	    "v9). windowHandle, if given, carries the weave as in SPEC_VERSION 8.");
}

void
oxr_texture_target_wait(struct oxr_session *sess, XrFrameState *frameState)
{
	XrWin32TextureTargetStateDXR *st =
	    OXR_GET_OUTPUT_FROM_CHAIN(frameState, XR_TYPE_WIN32_TEXTURE_TARGET_STATE_DXR, XrWin32TextureTargetStateDXR);
	if (st == NULL) {
		return;
	}
	st->flags = 0;
	st->wovenScreenOrigin.x = 0;
	st->wovenScreenOrigin.y = 0;
	st->wovenTargetRect.offset.x = 0;
	st->wovenTargetRect.offset.y = 0;
	st->wovenTargetRect.extent.width = 0;
	st->wovenTargetRect.extent.height = 0;
	st->wovenDisplayTime = 0;
	if (!sess->texture_target_bound) {
		return;
	}
#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
	struct comp_d3d11_texture_target_state s;
	if (sess->is_d3d11_native_compositor && sess->xcn != NULL &&
	    comp_d3d11_compositor_get_texture_target_state(&sess->xcn->base, &s)) {
		st->flags = (XrWin32TextureTargetStateFlagsDXR)s.flags;
		if (s.valid) {
			st->wovenScreenOrigin.x = s.screen_x;
			st->wovenScreenOrigin.y = s.screen_y;
			st->wovenTargetRect.offset.x = s.rect_x;
			st->wovenTargetRect.offset.y = s.rect_y;
			st->wovenTargetRect.extent.width = (int32_t)s.rect_w;
			st->wovenTargetRect.extent.height = (int32_t)s.rect_h;
			st->wovenDisplayTime = (XrTime)s.display_time_ns;
		}
	}
#endif
}

void
oxr_texture_target_system_properties(struct oxr_system *sys, XrSystemProperties *properties)
{
	XrSystemWin32TextureTargetPropertiesDXR *p = OXR_GET_OUTPUT_FROM_CHAIN(
	    properties, XR_TYPE_SYSTEM_WIN32_TEXTURE_TARGET_PROPERTIES_DXR, XrSystemWin32TextureTargetPropertiesDXR);
	if (p == NULL) {
		return;
	}
	p->supportsTextureTarget = XR_FALSE;
	p->windowlessPhase = XR_FALSE;
#ifdef XRT_HAVE_D3D11_NATIVE_COMPOSITOR
	// In-process native D3D11 only: an IPC session's texture class is not
	// covered by v9 (spec §8).
	const bool service_mode = sys->xsysc != NULL && sys->xsysc->info.is_service_mode;
	p->supportsTextureTarget = service_mode ? XR_FALSE : XR_TRUE;
	// A PREDICTION from the plug-in-level hint (ADR-060 decision 4); the
	// session's state flags are the authority.
	const struct xrt_dp_registry_entry *e =
	    sys->xsysc != NULL ? comp_dp_entry_for_session(&sys->xsysc->info, 0) : NULL;
	p->windowlessPhase =
	    (!service_mode && e != NULL && (e->claim_apis & XRT_DP_API_BIT_HINT_WINDOWLESS_PHASE) != 0) ? XR_TRUE
	                                                                                               : XR_FALSE;
#else
	(void)sys;
#endif
}

#endif // OXR_HAVE_DXR_win32_window_binding
