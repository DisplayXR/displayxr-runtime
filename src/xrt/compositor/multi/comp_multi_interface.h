// Copyright 2021, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Interface for the multi-client layer code.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup comp_main
 *
 * @note This interface fronts the PRODUCTION system compositor on macOS, Linux
 * and Android: `compositor/null/null_compositor.c` is its sole entry point, and
 * `targets/common/target_instance.c` selects it whenever `XRT_D3D11_SERVICE_ONLY`
 * is unset. Windows ships `compositor/d3d11_service/` instead, so comp_multi is
 * a non-shipping fallback there. Design: `docs/architecture/comp-multi-one-pipeline.md`
 * (#967); platform shape: `docs/architecture/service-architecture.md` §7.
 */

/*!
 * @page comp_multi_locking comp_multi locking (#1005, #1007)
 *
 * Three locks, ONE order — always acquire left to right, never the reverse:
 *
 *     msc->render_lock  ->  msc->list_and_timing_lock  ->  mc->slot_lock
 *
 * - **render_lock** — held by the multi main loop for one whole frame:
 *   layer transfer, native layer_commit, and every per-client / shared-surface
 *   record, submit, bounded fence wait, DP call (`process_atlas`, on_pause /
 *   on_resume) and present. Also taken by anything that must not overlap a
 *   frame in flight: client destroy (around the unlist), end_session (around
 *   the session-render teardown flags), the session-DP detach, the system-wide
 *   display-mode request, and the Android visibility tick (trylock — a busy
 *   lock means the main loop is running that pass itself). It is NEVER taken
 *   while holding list_and_timing_lock or a slot_lock.
 * - **list_and_timing_lock** — `msc->clients[]`, the pacing timings and every
 *   client's app pacer (`u_pa_*`), slot moves. Held only for SHORT bookkeeping
 *   sections; no GPU work, no fence wait, no DP call and no present runs under
 *   it (the main loop renders a snapshot, `msc->render_set`, taken under it
 *   with render_lock held). A read-only accessor that only needs a consistent
 *   view of `clients[]` / per-client state may take list_and_timing_lock
 *   alone; it must not call into a DP or Vulkan while holding it.
 * - **slot_lock** — per client, the progress/scheduled slot handshake with
 *   that client's wait thread.
 *
 * Consequences: a client create/destroy or a pacing call waits for at most
 * the bookkeeping, not for a frame; a destroy waits for at most the frame in
 * flight (whose waits are bounded, #1007) and never for another client's GPU.
 */

#pragma once

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_config_build.h"
#include "xrt/xrt_compositor.h"


/*!
 * @def COMP_MULTI_HAVE_WEAVE
 * Defined when this build carries a comp_multi XR_DXR_weave engine — the
 * `comp_multi_weave_*` entry points declared in comp_multi_private.h, called
 * from the IPC server's weave handlers exactly where the Windows build calls
 * `comp_d3d11_service_weave_*`.
 *
 * The ONE compile-time gate for that question: comp_multi's declarations and
 * teardown, and every weave fence in src/xrt/ipc/, key on it, so the engine's
 * callers and its implementation cannot disagree about whether it exists.
 *
 * - macOS: comp_multi_weave_macos.c (IOSurface, #759).
 * - Android: comp_multi_weave_android.c (AHardwareBuffer, #1036).
 * - Desktop Linux: comp_multi_weave_linux.c (fd transport, #1699 piece R2),
 *   behind XRT_FEATURE_COMP_MULTI_WEAVE_LINUX (default ON).
 *   `XRT_OS_LINUX && !XRT_OS_ANDROID` is the XRT_OS_LINUX_DESKTOP condition
 *   spelled out until that macro is defined globally in xrt_config_os.h (#1702).
 */
#if defined(XRT_OS_MACOS) || defined(XRT_OS_ANDROID) ||                                                                \
    (defined(XRT_OS_LINUX) && !defined(XRT_OS_ANDROID) && defined(XRT_FEATURE_COMP_MULTI_WEAVE_LINUX))
#define COMP_MULTI_HAVE_WEAVE
#endif

/*!
 * @def COMP_MULTI_SHARED_SURFACE
 * Defined when this build COMPILES the comp_multi SHARED SPATIAL SURFACE
 * (#59): one service-owned full-screen target, every client app composited
 * into one combined atlas at its workspace pose, one display-processor weave,
 * one present (the analogue of the Windows D3D11 monolith). Whether it is the
 * path a given frame takes is @ref multi_shared_surface_active — see
 * COMP_MULTI_PER_SESSION_TARGET.
 *
 * - macOS: service-owned NSWindow (comp_window_macos).
 * - Desktop Linux (#710, #967): service-owned X11 window, Xorg or XWayland
 *   (comp_window_linux).
 * - Android (#1006, ADR-036 D4): the WORKSPACE OVERLAY MODE — the target is
 *   the verified workspace controller's own fullscreen surface, and the mode
 *   is entered only while that controller is active.
 */
#if defined(XRT_OS_MACOS) || defined(XRT_OS_LINUX_DESKTOP) || defined(XRT_OS_ANDROID)
#define COMP_MULTI_SHARED_SURFACE
#endif

/*!
 * @def COMP_MULTI_PER_SESSION_TARGET
 * Defined when this build compiles the per-session target path
 * (render_session_to_own_target: one target + one DP per client) — Android's
 * default (ADR-036 D1/D3) and the unused Windows fallback.
 *
 * Where BOTH are defined (Android) the mode is chosen at runtime by
 * @ref multi_shared_surface_active: per-session unless a verified workspace
 * controller has activated the overlay. Where only COMP_MULTI_SHARED_SURFACE is
 * defined (macOS, desktop Linux) the predicate is a compile-time `true`, so
 * those builds are unchanged.
 */
#if !defined(XRT_OS_MACOS) && !defined(XRT_OS_LINUX_DESKTOP)
#define COMP_MULTI_PER_SESSION_TARGET
#endif


#ifdef __cplusplus
extern "C" {
#endif

struct u_pacing_app_factory;
struct comp_target_service;

/*!
 * Create a "system compositor" that can handle multiple clients (each
 * through a "multi compositor") and that drives a single native compositor.
 * Both the native compositor and the pacing factory is owned by the system
 * compositor and destroyed by it.
 *
 * @param xcn                        Native compositor that client are multi-plexed to.
 * @param upaf                       App pacing factory, one pacer created per client.
 * @param xsci                       Information to be exposed.
 * @param do_warm_start              Should we always submit a frame at startup.
 * @param target_service             Target service for per-session rendering (may be NULL).
 * @param out_xsysc                  Created @ref xrt_system_compositor.
 *
 * @public @memberof multi_system_compositor
 */
xrt_result_t
comp_multi_create_system_compositor(struct xrt_compositor_native *xcn,
                                    struct u_pacing_app_factory *upaf,
                                    const struct xrt_system_compositor_info *xsci,
                                    bool do_warm_start,
                                    struct comp_target_service *target_service,
                                    struct xrt_system_compositor **out_xsysc);


#ifdef __cplusplus
}
#endif

#ifdef XRT_OS_ANDROID
/*!
 * #1278: drive the Android visibility/weave-idle convergent pass from an
 * always-running context (the IPC server 20 Hz mainloop). The multi main loop
 * only runs with composited sessions; a pure present-owner weave client
 * leaves it parked. Safe to call from any thread; try-takes render_lock, then
 * list_and_timing_lock (see @ref comp_multi_locking).
 */
void
multi_system_compositor_android_visibility_tick(struct xrt_system_compositor *xsysc);
#endif
