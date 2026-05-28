// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Compile-time sanity stub for the vendor plug-in ABI.
 *
 * Includes `xrt/xrt_plugin.h` so the header is exercised on every build
 * and asserts a small set of structural invariants the runtime side will
 * rely on when registry enumeration / probe-loop code lands (Step 5 of
 * `docs/roadmap/vendor-plugin-architecture.md`). No runtime behavior —
 * the file contains only `_Static_assert`s.
 *
 * Will be replaced by the real plug-in discovery + probe loop in a
 * later step. Keeping it as its own TU now means:
 *   1. The header parses cleanly under MSVC / clang / GCC on every CI
 *      configuration without a hypothetical future TU as a leak detector.
 *   2. The struct-layout asserts catch any unintended field reorder
 *      before the runtime grows code that depends on the layout.
 *
 * @ingroup oxr_main
 */

#include "xrt/xrt_plugin.h"

#include <stddef.h>


/*
 * Layout asserts. If any of these fire, the cause is a structural change
 * to xrt_plugin.h that needs an XRT_PLUGIN_API_VERSION_* bump.
 */

/* The entry-point symbol the runtime resolves at LoadLibrary time. */
_Static_assert(sizeof(XRT_PLUGIN_ENTRYPOINT_NAME) == sizeof("xrtPluginNegotiate"),
               "xrt_plugin: entry-point symbol name changed; coordinate with all plug-in builds");

/* Current API version is the v2 line (ADR-020 rules 1–3 landed) — bump
 * deliberately, not by accident. */
_Static_assert(XRT_PLUGIN_API_VERSION_CURRENT == XRT_PLUGIN_API_VERSION_2,
               "xrt_plugin: API version current/v2 drift");

/*
 * Host iface layout. struct_size must be the first field so plug-ins can
 * always read it regardless of API version, and host_api_version sits
 * immediately after for the cheap version check.
 */
_Static_assert(offsetof(struct xrt_plugin_host_iface, struct_size) == 0,
               "xrt_plugin: host_iface.struct_size must be the first field");
_Static_assert(offsetof(struct xrt_plugin_host_iface, host_api_version) == sizeof(uint32_t),
               "xrt_plugin: host_iface.host_api_version must follow struct_size");

/*
 * Plug-in iface layout. struct_size first, then id/display_name pointers
 * for the runtime's logged-line "found plug-in <id> (<display_name>)"
 * trace at probe time.
 */
_Static_assert(offsetof(struct xrt_plugin_iface, struct_size) == 0,
               "xrt_plugin: iface.struct_size must be the first field");
_Static_assert(offsetof(struct xrt_plugin_iface, id) < offsetof(struct xrt_plugin_iface, probe),
               "xrt_plugin: iface.id must precede the probe function pointer");
_Static_assert(offsetof(struct xrt_plugin_iface, display_name) < offsetof(struct xrt_plugin_iface, probe),
               "xrt_plugin: iface.display_name must precede the probe function pointer");

/*
 * v1 commits to having all five per-API DP factories present (each may
 * be NULL at runtime if the plug-in does not support that API). Assert
 * they are all there so plug-ins can rely on the layout.
 */
_Static_assert(offsetof(struct xrt_plugin_iface, create_dp_vk) < offsetof(struct xrt_plugin_iface, destroy),
               "xrt_plugin: create_dp_vk must precede destroy");
_Static_assert(offsetof(struct xrt_plugin_iface, create_dp_d3d11) < offsetof(struct xrt_plugin_iface, destroy),
               "xrt_plugin: create_dp_d3d11 must precede destroy");
_Static_assert(offsetof(struct xrt_plugin_iface, create_dp_d3d12) < offsetof(struct xrt_plugin_iface, destroy),
               "xrt_plugin: create_dp_d3d12 must precede destroy");
_Static_assert(offsetof(struct xrt_plugin_iface, create_dp_gl) < offsetof(struct xrt_plugin_iface, destroy),
               "xrt_plugin: create_dp_gl must precede destroy");
_Static_assert(offsetof(struct xrt_plugin_iface, create_dp_metal) < offsetof(struct xrt_plugin_iface, destroy),
               "xrt_plugin: create_dp_metal must precede destroy");

/*
 * get_display_info is the first additive vtable field after the v1
 * "core" methods. New extensions go after it; this assert keeps
 * destroy as the boundary between the v1 core and the appended
 * surface so plug-ins built against the bare v1 header keep working
 * (their struct_size won't include get_display_info; the runtime
 * checks via struct_size before dereferencing).
 */
_Static_assert(offsetof(struct xrt_plugin_iface, destroy) < offsetof(struct xrt_plugin_iface, get_display_info),
               "xrt_plugin: get_display_info must follow destroy (additive-only growth)");
_Static_assert(offsetof(struct xrt_plugin_iface, get_display_info) < offsetof(struct xrt_plugin_iface, set_pose_source),
               "xrt_plugin: set_pose_source must follow get_display_info (additive-only growth)");

/*
 * Display-info struct's first field must be struct_size so plug-ins
 * can detect host's compile-time sizeof and clamp writes accordingly.
 */
_Static_assert(offsetof(struct xrt_plugin_display_info, struct_size) == 0,
               "xrt_plugin: display_info.struct_size must be the first field");
