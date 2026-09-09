// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The XR_DXR_weave handle-export latch rule (#1427).
 *
 * @c xrWeaveSubmitDXR hands the caller the woven texture + fence HANDLEs ONCE —
 * "on the first successful submit and on reallocation" (spec §2) — and remembers
 * that it did in @c oxr_session::weave. Until #1427 it remembered
 * unconditionally, including on the frames where the export legitimately
 * produced nothing:
 *
 *   - Windows: @c comp_d3d11_service_weave_export_output reports false while
 *     @c weave_output_handle is still null, and the per-peer handle duplication
 *     can fail outright (spec §4c rule 2, the refused peer declaration).
 *   - Android: @c comp_multi_weave_export_output reports false **by design**
 *     while the #1277 weave satellite presents the output itself; when the
 *     satellite later disables itself the client is already latched.
 *
 * One such frame and the caller never sees @c weavedTexture again at those dims
 * — sticky black tiles for the life of the session, healed only by a resize.
 *
 * The rule below is deliberately a pure boolean with no runtime dependency, so
 * it can be pinned on the host (tests/tests_oxr_weave_latch.cpp) without a
 * service, a window or a panel — the same shape as the ADR-027 tier-1 dispatch
 * and the mini-window tell.
 *
 * @ingroup oxr_main
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Does the weave service on THIS platform hand back a fence alongside the woven
 * texture?
 *
 * Windows (the D3D11 service compositor) exports a shared fence the caller waits
 * on before sampling the woven texture. macOS (#759) completes the weave
 * synchronously and Android hands back an AHardwareBuffer, so neither ever
 * exports one — @c ipc_handle_weave_get_fence has no non-Windows branch at all.
 * Requiring a fence there would mean never latching.
 */
static inline bool
oxr_weave_platform_exports_fence(void)
{
#ifdef XRT_OS_WINDOWS
	return true;
#else
	return false;
#endif
}

/*!
 * Should the one-shot export latch (@c exported / @c last_w / @c last_h) be
 * armed for this submit?
 *
 * Only when every export this platform is expected to produce actually produced
 * a valid handle. A texture-only success on Windows does NOT latch: the caller
 * (the browser) needs the fence to know when the woven texture is safe to
 * sample, so latching there would hand it a texture it can never synchronise
 * against and never re-offer the fence. The cost of not latching is that the
 * next frame re-exports the texture too — a handful of duplicated handles across
 * a transient, which the caller closes, versus a session-long black tile.
 *
 * @param platform_exports_fence @ref oxr_weave_platform_exports_fence in
 *        production; a parameter so both platform classes are testable on one
 *        host.
 * @param got_texture The output export returned a valid buffer handle.
 * @param got_fence   The fence export returned a valid sync handle.
 */
static inline bool
oxr_weave_should_latch_export(bool platform_exports_fence, bool got_texture, bool got_fence)
{
	if (!got_texture) {
		return false;
	}
	if (platform_exports_fence && !got_fence) {
		return false;
	}
	return true;
}

#ifdef __cplusplus
}
#endif
