// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Linkage decoration for the runtime DLL's exported aux surface.
 *
 * Per `docs/adr/ADR-019-vendor-plugin-aux-boundary.md`, the runtime DLL
 * (`DisplayXRClient.dll`) exports a small set of state-bearing aux
 * functions so that vendor plug-in DLLs loaded into the runtime's
 * process see exactly one copy of `u_log`, `u_var`, `u_metrics`, etc.
 *
 * The actual *export* on the runtime side is driven by explicit
 * platform-specific export lists (`libopenxr.def` on Windows,
 * `libopenxr.version` on Linux, `exported_symbols.list` on macOS) — the
 * same mechanism the project already uses to gate the runtime DLL's
 * exports down to `xrNegotiateLoaderRuntimeInterface` and the
 * additions from this work. Headers do NOT carry `__declspec(dllexport)`
 * annotations; the export lists are the source of truth.
 *
 * This header exists purely for the *import* side: plug-in builds need
 * `__declspec(dllimport)` on Windows so the compiler emits the correct
 * indirect-call sequence through the DLL's IAT. On Linux / macOS no
 * annotation is required at the call site (the linker resolves
 * dynamically against the runtime DLL's `default`-visibility export).
 *
 * @ingroup xrt_iface
 */

#pragma once


/*!
 * Linkage decoration for declarations of functions exported by the
 * runtime DLL. Used in aux headers (`u_logging.h`, `u_var.h`,
 * `u_metrics.h`, etc.) on the declarations of functions enumerated by
 * ADR-019's "Exported surface" section.
 *
 * Defining `XRT_USING_RUNTIME_DLL` in a plug-in's build (typically via a
 * `target_compile_definitions(... PRIVATE XRT_USING_RUNTIME_DLL)` line
 * in the plug-in's CMakeLists) makes this expand to
 * `__declspec(dllimport)` on Windows; the compiler then emits direct
 * IAT loads instead of the slower link-time-stub round-trip.
 *
 * Targets that **define** the symbols (i.e. that get linked into the
 * runtime DLL itself) and in-tree static consumers (the cube test apps,
 * `displayxr-service.exe`, `displayxr-cli.exe`, etc. that statically
 * link `aux_util`) MUST NOT define `XRT_USING_RUNTIME_DLL`. For them the
 * macro expands to nothing and the symbols resolve through the local
 * static copy of aux.
 *
 * On Linux + macOS the macro expands to nothing in both modes — ELF /
 * Mach-O dynamic linking does not need an import-side annotation.
 */
#if defined(XRT_USING_RUNTIME_DLL) && (defined(_WIN32) || defined(__CYGWIN__))
#define XRT_API_FUNC __declspec(dllimport)
#else
#define XRT_API_FUNC
#endif
