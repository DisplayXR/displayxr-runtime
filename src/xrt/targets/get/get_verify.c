// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  dxr-get verification: SHA-256 (CNG), Authenticode signer
 *         (WinVerifyTrust) and the catalog's Ed25519 signature (Monocypher).
 */

#include "get.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <stdio.h>
#include <string.h>

#include "monocypher-ed25519.h"

/*
 * The catalog keys this build trusts. Rotation = ship a dxr-get with the new
 * key ADDED, wait for it to be what installers embed, then start signing with
 * it. Never remove a key a deployed dxr-get still needs in the same release.
 * docs/specs/runtime/component-catalog.md.
 */
static const struct
{
	const char *id; //!< first 8 hex of sha256(pubkey)
	uint8_t key[32];
} k_catalog_keys[] = {
    {"9b762f9a", {0x74, 0xfc, 0x4d, 0x1d, 0x2e, 0x50, 0x2e, 0xdb, 0x2b, 0x7b, 0x35, 0x56, 0x77, 0x46, 0x5e, 0x39,
                  0x87, 0x0d, 0x55, 0x51, 0x4a, 0xc2, 0xea, 0x13, 0xbb, 0xc6, 0xf3, 0x50, 0xc1, 0x2e, 0xf4, 0x34}},
};

bool
dxr_sha256_file(const wchar_t *path, char out_hex[65])
{
	bool ok = false;
	BCRYPT_ALG_HANDLE alg = NULL;
	BCRYPT_HASH_HANDLE h = NULL;
	HANDLE f =
	    CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (f == INVALID_HANDLE_VALUE) {
		return false;
	}
	if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0 ||
	    BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) != 0) {
		goto out;
	}
	static uint8_t buf[1 << 20];
	for (;;) {
		DWORD got = 0;
		if (!ReadFile(f, buf, sizeof(buf), &got, NULL)) {
			goto out;
		}
		if (got == 0) {
			break;
		}
		if (BCryptHashData(h, buf, got, 0) != 0) {
			goto out;
		}
	}
	uint8_t d[32];
	if (BCryptFinishHash(h, d, 32, 0) != 0) {
		goto out;
	}
	for (int i = 0; i < 32; i++) {
		(void)snprintf(out_hex + 2 * i, 3, "%02x", d[i]);
	}
	ok = true;
out:
	if (h) {
		BCryptDestroyHash(h);
	}
	if (alg) {
		BCryptCloseAlgorithmProvider(alg, 0);
	}
	CloseHandle(f);
	return ok;
}

bool
dxr_authenticode_signer(const wchar_t *path, char *cn, size_t cn_size)
{
	cn[0] = '\0';
	WINTRUST_FILE_INFO fi;
	memset(&fi, 0, sizeof(fi));
	fi.cbStruct = sizeof(fi);
	fi.pcwszFilePath = path;
	WINTRUST_DATA wd;
	memset(&wd, 0, sizeof(wd));
	wd.cbStruct = sizeof(wd);
	wd.dwUIChoice = WTD_UI_NONE;
	wd.fdwRevocationChecks = WTD_REVOKE_NONE;
	wd.dwUnionChoice = WTD_CHOICE_FILE;
	wd.pFile = &fi;
	wd.dwStateAction = WTD_STATEACTION_VERIFY;
	// Revocation over the network would turn a captive portal into a hard
	// failure; the payload hash pinned by the signed catalog already binds
	// the exact bytes, so the chain check here is about WHO signed them.
	wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
	GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
	LONG st = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
	bool ok = false;
	if (st == ERROR_SUCCESS) {
		CRYPT_PROVIDER_DATA *pd = WTHelperProvDataFromStateData(wd.hWVTStateData);
		CRYPT_PROVIDER_SGNR *sg = pd ? WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0) : NULL;
		CRYPT_PROVIDER_CERT *pc = sg ? WTHelperGetProvCertFromChain(sg, 0) : NULL;
		if (pc != NULL && pc->pCert != NULL) {
			wchar_t w[256];
			if (CertGetNameStringW(pc->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, w, 256) > 1) {
				(void)WideCharToMultiByte(CP_UTF8, 0, w, -1, cn, (int)cn_size, NULL, NULL);
				ok = true;
			}
		}
	} else {
		dxr_log("  Authenticode: not validly signed (0x%08lx)\n", (unsigned long)st);
	}
	wd.dwStateAction = WTD_STATEACTION_CLOSE;
	(void)WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
	return ok;
}

static int
b64v(char c)
{
	if (c >= 'A' && c <= 'Z') {
		return c - 'A';
	}
	if (c >= 'a' && c <= 'z') {
		return c - 'a' + 26;
	}
	if (c >= '0' && c <= '9') {
		return c - '0' + 52;
	}
	if (c == '+') {
		return 62;
	}
	if (c == '/') {
		return 63;
	}
	return -1;
}

//! Decodes standard base64 (whitespace skipped, '=' ends). Returns bytes written or -1.
static int
b64_decode(const char *in, uint8_t *out, int cap)
{
	uint32_t acc = 0;
	int bits = 0, n = 0;
	for (; *in && *in != '='; in++) {
		if (*in == '\n' || *in == '\r' || *in == ' ' || *in == '\t') {
			continue;
		}
		int v = b64v(*in);
		if (v < 0) {
			return -1;
		}
		acc = (acc << 6) | (uint32_t)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (n >= cap) {
				return -1;
			}
			out[n++] = (uint8_t)(acc >> bits);
		}
	}
	return n;
}

bool
dxr_catalog_signature_ok(const char *msg, size_t msg_len, const char *sig_b64, char key_id_out[9])
{
	uint8_t sig[64];
	if (b64_decode(sig_b64, sig, 64) != 64) {
		return false;
	}
	for (size_t i = 0; i < sizeof(k_catalog_keys) / sizeof(k_catalog_keys[0]); i++) {
		if (crypto_ed25519_check(sig, k_catalog_keys[i].key, (const uint8_t *)msg, msg_len) == 0) {
			(void)snprintf(key_id_out, 9, "%s", k_catalog_keys[i].id);
			return true;
		}
	}
	return false;
}
