// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  dxr-get: fetch, verify and install DisplayXR components from the
 *         signed component catalog (#2702 / #2704 / #2705).
 *
 * Standalone on purpose: it links nothing from the runtime, so the browser
 * installer (and any other component installer) can embed the one exe and
 * run it on a box that has no DisplayXR runtime yet.
 *
 * Contract: docs/specs/runtime/dxr-get.md.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum dxr_get_exit
{
	DXR_GET_OK = 0,
	DXR_GET_USAGE = 2,
	DXR_GET_OFFLINE = 10,       //!< catalog unreachable, unparsable or expired
	DXR_GET_BAD_SIGNATURE = 11, //!< catalog signature (or serial) rejected
	DXR_GET_DOWNLOAD = 20,
	DXR_GET_VERIFY = 21, //!< size / hash / Authenticode signer mismatch
	DXR_GET_CHILD = 30,  //!< a component installer exited non-zero
	DXR_GET_NOT_FETCHABLE = 40,
	DXR_GET_INCOMPATIBLE = 41,     //!< the item's runtime range or class ABI excludes this machine's runtime
	DXR_GET_NOT_INSTALLED = 42,    //!< uninstall: no install receipt for that id
	DXR_GET_CANNOT_UNINSTALL = 43, //!< uninstall: no uninstall command recorded, or other items require it
	DXR_GET_NEEDS_ADMIN = 50,
};

/*
 * Logging: human text to stderr; stdout is reserved for machine output.
 */
void
dxr_log(const char *fmt, ...);

/*
 * HTTP (get_http.c). WinHTTP, system/auto proxy, redirects followed, retries
 * with backoff on 5xx/408/transport errors, none on 4xx.
 */

//! GET into a malloc'd buffer (NUL-terminated for convenience). Returns HTTP-ish status: 200 ok, <0 transport.
int
dxr_http_get_mem(const char *url, char **out, size_t *out_len, size_t max_len);

//! GET into @p path, resuming from @p path's current size with a Range request. @p octet adds
//! `Accept: application/octet-stream` (the GitHub API asset URL needs it). Returns true on a complete file of
//! @p expect_size bytes.
bool
dxr_http_get_file(const char *url, const wchar_t *path, uint64_t expect_size, bool octet);

/*
 * Verification (get_verify.c).
 */

bool
dxr_sha256_file(const wchar_t *path, char out_hex[65]);

//! true when @p path carries a valid Authenticode signature chaining to a trusted root; its leaf
//! signer's simple display name (the CN) goes to @p cn.
bool
dxr_authenticode_signer(const wchar_t *path, char *cn, size_t cn_size);

//! Ed25519 check of @p msg against the compiled-in catalog keys. @p sig_b64 is the .sig file body.
bool
dxr_catalog_signature_ok(const char *msg, size_t msg_len, const char *sig_b64, char key_id_out[9]);

/*
 * Detection (get_detect.c).
 */

struct dxr_edid_id
{
	uint16_t man;  //!< EDID bytes 8-9 read little-endian (the Leia table's convention: DEL = 44048 = 0xAC10)
	uint16_t prod; //!< EDID bytes 10-11, little-endian
};

//! Fills up to @p max EDID ids of present monitors. Returns the count.
int
dxr_detect_edids(struct dxr_edid_id *out, int max);

//! Reads a DWORD (or a decimal REG_SZ) under HKLM, 64-bit view. Returns false when absent.
bool
dxr_reg_read_dword(const char *subkey, const char *value, uint32_t *out);

//! Reads a REG_SZ given as `HKLM\\Sub\\Key\\ValueName` (64-bit view). Returns false when absent.
bool
dxr_reg_read_path_sz(const char *full, char *out, size_t out_size);

//! true when the `HKLM\\...` key exists (64-bit view).
bool
dxr_reg_key_exists(const char *key);

//! true when @p path names an existing file or directory.
bool
dxr_file_exists(const char *path);

//! true when a Windows service of that name is installed (running or not).
bool
dxr_service_exists(const char *name);

#ifdef __cplusplus
}
#endif
