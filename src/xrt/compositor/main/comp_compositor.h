// Copyright 2019-2023, Collabora, Ltd.
// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Minimal comp_compositor shim for the lightweight fork.
 * @ingroup comp_main
 *
 * The full Monado main compositor (comp_main / comp_compositor.c) was removed
 * from this fork (issue #25 — native per-API compositors replaced it). A few
 * shared helpers kept in `main/` still reference `struct comp_compositor` —
 * notably @ref comp_target_swapchain, which the out-of-process Android service
 * path reuses (#510). It needs only three things from the compositor: the
 * embedded @ref vk_bundle (`base.vk`), a fake-pacing frame interval
 * (`frame_interval_ns`), and a log level for the COMP_* macros.
 *
 * This shim provides exactly that surface. It is layout-compatible at offset 0
 * with any @ref comp_base subclass (e.g. @ref null_compositor), so a subclass
 * pointer up-cast to `struct comp_compositor *` resolves `base.vk` correctly —
 * the documented null-compositor reuse pattern. Fields past `base` (settings,
 * frame_interval_ns) do NOT line up across that cast: callers must pre-create
 * the @ref comp_target_swapchain pacer so `frame_interval_ns` is never read,
 * and the COMP_* log level is best-effort (cosmetic if it reads across a cast).
 *
 * If the full main compositor is ever reintroduced, this shim must be replaced
 * by the real header.
 */

#pragma once

#include "util/comp_base.h"
#include "util/u_logging.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Minimal stand-in for the removed Monado main compositor struct. See file docs.
 *
 * @ingroup comp_main
 */
struct comp_compositor
{
	//! Must be first — shared layout with every @ref comp_base subclass.
	struct comp_base base;

	//! Only @p log_level is consumed (by the COMP_* macros below).
	struct
	{
		enum u_logging_level log_level;
	} settings;

	//! Fake-pacing frame interval; only read by comp_target_swapchain when its
	//! pacer was not pre-created. The service path pre-creates the pacer.
	int64_t frame_interval_ns;
};

#define COMP_SPEW(c, ...) U_LOG_IFL_T((c)->settings.log_level, __VA_ARGS__);
#define COMP_DEBUG(c, ...) U_LOG_IFL_D((c)->settings.log_level, __VA_ARGS__);
#define COMP_INFO(c, ...) U_LOG_IFL_I((c)->settings.log_level, __VA_ARGS__);
#define COMP_WARN(c, ...) U_LOG_IFL_W((c)->settings.log_level, __VA_ARGS__);
#define COMP_ERROR(c, ...) U_LOG_IFL_E((c)->settings.log_level, __VA_ARGS__);

#ifdef __cplusplus
}
#endif
