// Copyright 2026, DisplayXR
// SPDX-License-Identifier: Apache-2.0
//
// PROVISIONAL — DXR is DisplayXR's Khronos-registered OpenXR author ID, but
// the values in this header are NOT yet registered in the Khronos OpenXR
// registry: they sit in a provisional experimental block (1004999xxx) pending
// official assignment. Names are expected to be stable; numeric values are not.
// See GOVERNANCE.md.
//
/*!
 * @file
 * @brief  Runtime-wide DisplayXR XrResult codes (not tied to one extension).
 * @author DisplayXR
 * @ingroup external_openxr
 *
 * Every other XR_DXR_*.h header defines the surface of one extension. This one
 * does not define an extension at all — it collects `XrResult` values the
 * DisplayXR runtime can return from **core** OpenXR entry points, where no
 * extension has been (or can have been) enabled yet.
 *
 * There is nothing to put in `XrInstanceCreateInfo::enabledExtensionNames` for
 * these; an application simply includes this header and compares. A runtime
 * that does not know the code never returns it, so the comparison is safe
 * against any runtime.
 */
#ifndef XR_DXR_RESULT_CODES_H
#define XR_DXR_RESULT_CODES_H 1

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

// Values from the DisplayXR provisional 1004999xxx block — decade
// 1004999230–239, recorded in this directory's README.md registry. Error-class
// XrResults are negative per OpenXR's result-code convention, so the decade's
// values appear here negated.

/*!
 * @brief `xrCreateInstance` failed because the client library and the running
 * DisplayXR service are from different builds (browser#103).
 *
 * The DisplayXR service and the runtime client library that loads into the
 * application's process are versioned in lockstep by build-time git tag. When
 * they disagree — the usual cause being an installer that upgraded the runtime
 * while an application was already running — the connection handshake is
 * refused and `xrCreateInstance` fails with this code instead of the generic
 * `XR_ERROR_RUNTIME_FAILURE`.
 *
 * Applications should treat it as **distinct but not permanent**:
 *
 * - The remedy to *report* is "relaunch the application", not "retry harder":
 *   nothing the application does in-process will make the two builds agree.
 * - Do NOT latch it as fatal. A rollback, or an installer that simply had not
 *   finished writing when the attempt was made, recovers on its own — so an
 *   application that reconnects on a backoff ladder should drop to a long tail
 *   rather than switching its DisplayXR integration off.
 *
 * A runtime older than this code reports `XR_ERROR_RUNTIME_FAILURE` for the
 * same condition, which is indistinguishable from a transient failure; that is
 * exactly why the code was added.
 */
#define XR_ERROR_RUNTIME_VERSION_SKEW_DXR ((XrResult)-1004999230)

#ifdef __cplusplus
}
#endif

#endif // XR_DXR_RESULT_CODES_H
