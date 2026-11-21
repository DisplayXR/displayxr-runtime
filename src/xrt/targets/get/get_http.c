// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  dxr-get HTTP: WinHTTP GET with proxy auto-detect, redirects, Range
 *         resume and bounded retries. Shaped after conv_model_store.c's
 *         fetch_http (the downloader the runtime already ships), minus the
 *         credential handling dxr-get never needs: every URL it fetches is
 *         public.
 */

#include "get.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UA L"DisplayXR-Get/1.0"

static bool
widen(const char *s, wchar_t *out, int cap)
{
	return MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap) > 0;
}

struct conn
{
	HINTERNET ses, con, req;
};

static void
conn_close(struct conn *c)
{
	if (c->req) {
		WinHttpCloseHandle(c->req);
	}
	if (c->con) {
		WinHttpCloseHandle(c->con);
	}
	if (c->ses) {
		WinHttpCloseHandle(c->ses);
	}
	memset(c, 0, sizeof(*c));
}

/*!
 * Opens and sends one GET. Returns the final HTTP status (after WinHTTP followed redirects), or -1 on a
 * transport failure. @p c stays open for reading the body on success.
 */
static int
open_get(struct conn *c, const char *url, const wchar_t *extra_headers)
{
	memset(c, 0, sizeof(*c));
	wchar_t wurl[2048];
	if (!widen(url, wurl, 2048)) {
		return -1;
	}
	c->ses =
	    WinHttpOpen(UA, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (c->ses == NULL) {
		c->ses = WinHttpOpen(UA, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
		                     WINHTTP_NO_PROXY_BYPASS, 0);
	}
	if (c->ses == NULL) {
		return -1;
	}
	(void)WinHttpSetTimeouts(c->ses, 15000, 15000, 30000, 60000);
	DWORD protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
	protos |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
	if (!WinHttpSetOption(c->ses, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof(protos))) {
		protos = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
		(void)WinHttpSetOption(c->ses, WINHTTP_OPTION_SECURE_PROTOCOLS, &protos, sizeof(protos));
	}

	URL_COMPONENTS uc;
	memset(&uc, 0, sizeof(uc));
	uc.dwStructSize = sizeof(uc);
	wchar_t host[256], path[2048];
	uc.lpszHostName = host;
	uc.dwHostNameLength = 256;
	uc.lpszUrlPath = path;
	uc.dwUrlPathLength = 2048;
	if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) {
		return -1;
	}
	// Only HTTPS. The catalog's integrity rests on its signature and every
	// payload's on its hash + Authenticode, but there is no reason to allow
	// a downgrade either.
	if (uc.nScheme != INTERNET_SCHEME_HTTPS) {
		dxr_log("refusing non-HTTPS URL: %s\n", url);
		return -1;
	}
	c->con = WinHttpConnect(c->ses, host, uc.nPort, 0);
	if (c->con == NULL) {
		return -1;
	}
	c->req = WinHttpOpenRequest(c->con, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
	                            WINHTTP_FLAG_SECURE);
	if (c->req == NULL) {
		return -1;
	}
	// Never follow a redirect to plain HTTP.
	DWORD pol = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
	(void)WinHttpSetOption(c->req, WINHTTP_OPTION_REDIRECT_POLICY, &pol, sizeof(pol));
	if (!WinHttpSendRequest(c->req, extra_headers ? extra_headers : WINHTTP_NO_ADDITIONAL_HEADERS,
	                        extra_headers ? (DWORD)-1L : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
	    !WinHttpReceiveResponse(c->req, NULL)) {
		return -1;
	}
	DWORD status = 0, sz = sizeof(status);
	if (!WinHttpQueryHeaders(c->req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
	                         WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX)) {
		return -1;
	}
	return (int)status;
}

static bool
retryable(int status)
{
	return status < 0 || status == 408 || status >= 500;
}

static const DWORD k_backoff_ms[] = {1000, 4000, 15000};
#define ATTEMPTS 4

int
dxr_http_get_mem(const char *url, char **out, size_t *out_len, size_t max_len)
{
	*out = NULL;
	*out_len = 0;
	int status = -1;
	for (int attempt = 0; attempt < ATTEMPTS; attempt++) {
		if (attempt > 0) {
			Sleep(k_backoff_ms[attempt - 1]);
		}
		struct conn c;
		status = open_get(&c, url, L"Cache-Control: no-cache\r\n");
		if (status != 200) {
			conn_close(&c);
			if (!retryable(status)) {
				return status;
			}
			dxr_log("  %s: %s, retrying\n", url, status < 0 ? "network error" : "server error");
			continue;
		}
		size_t cap = 64 * 1024, len = 0;
		char *buf = (char *)malloc(cap + 1);
		bool ok = buf != NULL;
		for (;;) {
			DWORD avail = 0, got = 0;
			if (!ok || !WinHttpQueryDataAvailable(c.req, &avail)) {
				ok = false;
				break;
			}
			if (avail == 0) {
				break;
			}
			if (len + avail > max_len) {
				ok = false;
				break;
			}
			if (len + avail > cap) {
				while (len + avail > cap) {
					cap *= 2;
				}
				char *nb = (char *)realloc(buf, cap + 1);
				if (nb == NULL) {
					ok = false;
					break;
				}
				buf = nb;
			}
			if (!WinHttpReadData(c.req, buf + len, avail, &got)) {
				ok = false;
				break;
			}
			len += got;
		}
		conn_close(&c);
		if (ok) {
			buf[len] = '\0';
			*out = buf;
			*out_len = len;
			return 200;
		}
		free(buf);
		status = -1;
	}
	return status;
}

static uint64_t
file_size(const wchar_t *path)
{
	WIN32_FILE_ATTRIBUTE_DATA fa;
	if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) {
		return 0;
	}
	return ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
}

//! true when a 206 answer's Content-Range starts at @p have (or carries no parsable Content-Range).
static bool
range_starts_at(struct conn *c, uint64_t have)
{
	wchar_t cr[128];
	DWORD sz = sizeof(cr);
	unsigned long long start = 0;
	if (!WinHttpQueryHeaders(c->req, WINHTTP_QUERY_CONTENT_RANGE, WINHTTP_HEADER_NAME_BY_INDEX, cr, &sz,
	                         WINHTTP_NO_HEADER_INDEX) ||
	    swscanf(cr, L"bytes %llu-", &start) != 1) {
		return true;
	}
	return start == (unsigned long long)have;
}

bool
dxr_http_get_file(const char *url, const wchar_t *path, uint64_t expect_size, bool octet)
{
	for (int attempt = 0; attempt < ATTEMPTS; attempt++) {
		if (attempt > 0) {
			Sleep(k_backoff_ms[attempt - 1]);
		}
		uint64_t have = file_size(path);
		if (have > expect_size) {
			(void)DeleteFileW(path);
			have = 0;
		}
		if (have == expect_size && expect_size > 0) {
			return true;
		}
		wchar_t hdr[256];
		int n = 0;
		if (octet) {
			n += _snwprintf(hdr + n, 256 - n, L"Accept: application/octet-stream\r\n");
		}
		if (have > 0) {
			n += _snwprintf(hdr + n, 256 - n, L"Range: bytes=%llu-\r\n", (unsigned long long)have);
		}
		struct conn c;
		int status = open_get(&c, url, n > 0 ? hdr : NULL);
		if (status == 200 && have > 0) {
			// Server ignored the Range: start over.
			have = 0;
		} else if (status == 206 && !range_starts_at(&c, have)) {
			// A partial answer for another range than the one asked for would
			// be appended at the wrong offset. The hash check would catch it,
			// but only after the whole download: start over now instead.
			conn_close(&c);
			(void)DeleteFileW(path);
			dxr_log("  server answered with a different range, restarting the download\n");
			continue;
		} else if (status == 416) {
			conn_close(&c);
			(void)DeleteFileW(path);
			continue;
		} else if (status != 200 && status != 206) {
			conn_close(&c);
			if (!retryable(status)) {
				dxr_log("  download refused (HTTP %d): %s\n", status, url);
				return false;
			}
			dxr_log("  download %s (HTTP %d), retrying\n", status < 0 ? "interrupted" : "failed", status);
			continue;
		}
		HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, have > 0 ? OPEN_ALWAYS : CREATE_ALWAYS,
		                       FILE_ATTRIBUTE_NORMAL, NULL);
		if (f == INVALID_HANDLE_VALUE) {
			conn_close(&c);
			dxr_log("  cannot write %ls (error %lu)\n", path, GetLastError());
			return false;
		}
		if (have > 0) {
			LARGE_INTEGER z;
			z.QuadPart = 0;
			(void)SetFilePointerEx(f, z, NULL, FILE_END);
		}
		static char buf[256 * 1024];
		uint64_t written = have, last_pct = 0;
		bool ok = true;
		for (;;) {
			DWORD got = 0;
			if (!WinHttpReadData(c.req, buf, sizeof(buf), &got)) {
				ok = false;
				break;
			}
			if (got == 0) {
				break;
			}
			DWORD w = 0;
			if (!WriteFile(f, buf, got, &w, NULL) || w != got) {
				ok = false;
				break;
			}
			written += got;
			if (written > expect_size) {
				ok = false;
				break;
			}
			uint64_t pct = expect_size ? written * 100 / expect_size : 0;
			if (pct >= last_pct + 10) {
				last_pct = pct - pct % 10;
				dxr_log("  %llu%%\n", (unsigned long long)last_pct);
			}
		}
		CloseHandle(f);
		conn_close(&c);
		if (ok && written == expect_size) {
			return true;
		}
		if (written > expect_size) {
			(void)DeleteFileW(path);
		}
		dxr_log("  download incomplete (%llu of %llu bytes), retrying\n", (unsigned long long)written,
		        (unsigned long long)expect_size);
	}
	return false;
}
