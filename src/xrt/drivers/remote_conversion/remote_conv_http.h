// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The remote conversion provider's HTTP client (ADR-057 §6).
 *
 * One synchronous request at a time, cancellable from the caller's own
 * is_cancelled poll (cancel = the connection is closed, contract v1), with an
 * overall timeout. No new build or package dependency:
 *  - Windows: WinHTTP (the model store's downloader uses it too);
 *  - macOS and desktop Linux: libcurl, loaded at run time (macOS always ships
 *    it; on Linux a missing libcurl.so.4 makes the provider report ABSENT
 *    with that reason rather than failing to load).
 *
 * @ingroup drv_remote_conversion
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! @name rc_http_response::error @{ */
#define RC_HTTP_OK 0          //!< a response arrived (any status)
#define RC_HTTP_TIMEOUT 1     //!< the overall timeout passed
#define RC_HTTP_CANCELLED 2   //!< is_cancelled said so; the connection was closed
#define RC_HTTP_CONNECT 3     //!< DNS / connect / TLS / transfer failure
#define RC_HTTP_UNSUPPORTED 4 //!< no HTTP stack on this machine
#define RC_HTTP_TOO_LARGE 5   //!< the response body passed max_response_bytes
/*! @} */

struct rc_http_request
{
	const char *method; //!< "GET" / "POST"
	const char *url;
	const char *const *headers; //!< "Name: value" strings, NULL-terminated; may be NULL
	const void *body;
	size_t body_size;
	uint32_t timeout_ms;         //!< whole request; 0 = 120 s
	uint64_t max_response_bytes; //!< 0 = 1 GiB
	bool (*is_cancelled)(void *ctx);
	void *cancel_ctx;
	//! Upload / download progress in [0,1] (optional; called on the request thread).
	void (*progress)(void *ctx, float up, float down);
	void *progress_ctx;
};

struct rc_http_response
{
	int error;   //!< RC_HTTP_*
	long status; //!< HTTP status when error == RC_HTTP_OK
	uint8_t *body;
	size_t body_size;
	char *headers; //!< the raw response header block (every line, CRLF separated)
	size_t headers_size;
	char message[192]; //!< a one-line reason when error != RC_HTTP_OK
};

/*! Is an HTTP stack available here? False with @p why filled (Linux without libcurl). */
bool
rc_http_available(char *why, size_t why_size);

/*! Perform @p req. Always fills @p resp (free it with rc_http_response_free). */
void
rc_http_perform(const struct rc_http_request *req, struct rc_http_response *resp);

void
rc_http_response_free(struct rc_http_response *resp);

/*!
 * The value of header @p name (case-insensitive) in @p resp, trimmed, into
 * @p out (the LAST occurrence: redirects keep earlier blocks). False when
 * absent or too long for @p out_size.
 */
bool
rc_http_header(const struct rc_http_response *resp, const char *name, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
