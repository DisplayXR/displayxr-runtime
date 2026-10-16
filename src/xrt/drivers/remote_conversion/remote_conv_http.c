// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The remote conversion provider's HTTP client (see remote_conv_http.h).
 * @ingroup drv_remote_conversion
 */

#include "remote_conv_http.h"

#include "xrt/xrt_config_os.h"

#include "os/os_threading.h"
#include "os/os_time.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(XRT_OS_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#endif

#define RC_HTTP_DEFAULT_TIMEOUT_MS 120000u
#define RC_HTTP_DEFAULT_MAX_BYTES (512ull * 1024ull * 1024ull) //!< when a caller sets no cap

static void
set_msg(struct rc_http_response *r, int error, const char *fmt, const char *arg)
{
	r->error = error;
	(void)snprintf(r->message, sizeof(r->message), fmt, arg != NULL ? arg : "");
}

//! Append @p n bytes to a growable buffer; false past @p max.
static bool
buf_append(uint8_t **buf, size_t *size, size_t *cap, const void *data, size_t n, uint64_t max)
{
	if ((uint64_t)*size + n > max) {
		return false;
	}
	if (*size + n + 1 > *cap) {
		size_t nc = *cap != 0 ? *cap : 64u * 1024u;
		while (nc < *size + n + 1) {
			nc *= 2;
		}
		// Never grow past the cap (+1 for the NUL): a 512 MiB limit must not
		// become a 1 GiB allocation by doubling.
		if ((uint64_t)nc > max + 1) {
			nc = (size_t)(max + 1);
		}
		uint8_t *nb = (uint8_t *)realloc(*buf, nc);
		if (nb == NULL) {
			return false;
		}
		*buf = nb;
		*cap = nc;
	}
	memcpy(*buf + *size, data, n);
	*size += n;
	(*buf)[*size] = 0; // always NUL-terminated (headers are parsed as text)
	return true;
}

void
rc_http_response_free(struct rc_http_response *resp)
{
	if (resp == NULL) {
		return;
	}
	free(resp->body);
	free(resp->headers);
	resp->body = NULL;
	resp->headers = NULL;
	resp->body_size = resp->headers_size = 0;
}

bool
rc_url_endpoint_allowed(const char *url)
{
	if (url == NULL) {
		return false;
	}
	for (const unsigned char *c = (const unsigned char *)url; *c != '\0'; c++) {
		if (*c <= 0x20 || *c == 0x7f || *c == '\\') {
			return false; // white space, control characters, backslashes: never in an endpoint
		}
	}
	bool https;
	const char *a;
	if (strncmp(url, "https://", 8) == 0) {
		https = true;
		a = url + 8;
	} else if (strncmp(url, "http://", 7) == 0) {
		https = false;
		a = url + 7;
	} else {
		return false;
	}
	// The authority ends at the first '/', '?' or '#'.
	size_t alen = strcspn(a, "/?#");
	if (alen == 0 || memchr(a, '@', alen) != NULL) {
		return false; // no host, or userinfo (the classic `allowed-host@other-host` bypass)
	}
	// Split host and port.
	size_t hlen;
	const char *port = NULL;
	if (a[0] == '[') {
		const char *rb = memchr(a, ']', alen);
		if (rb == NULL) {
			return false;
		}
		hlen = (size_t)(rb - a) + 1;
		if (hlen < alen) {
			if (a[hlen] != ':') {
				return false;
			}
			port = a + hlen + 1;
		}
	} else {
		const char *colon = memchr(a, ':', alen);
		hlen = colon != NULL ? (size_t)(colon - a) : alen;
		port = colon != NULL ? colon + 1 : NULL;
	}
	if (hlen == 0) {
		return false;
	}
	if (port != NULL) {
		const size_t plen = alen - (size_t)(port - a);
		if (plen == 0 || plen > 5) {
			return false;
		}
		for (size_t i = 0; i < plen; i++) {
			if (port[i] < '0' || port[i] > '9') {
				return false;
			}
		}
	}
	if (https) {
		return true;
	}
	static const char *const loop[] = {"localhost", "127.0.0.1", "[::1]"};
	for (size_t i = 0; i < sizeof(loop) / sizeof(loop[0]); i++) {
		if (strlen(loop[i]) == hlen && strncmp(a, loop[i], hlen) == 0) {
			return true;
		}
	}
	return false;
}

bool
rc_http_header(const struct rc_http_response *resp, const char *name, char *out, size_t out_size)
{
	if (resp == NULL || resp->headers == NULL || name == NULL || out == NULL || out_size == 0) {
		return false;
	}
	const size_t nlen = strlen(name);
	bool found = false;
	const char *line = resp->headers;
	while (*line != '\0') {
		const char *eol = strchr(line, '\n');
		const size_t len = eol != NULL ? (size_t)(eol - line) : strlen(line);
		const char *colon = memchr(line, ':', len);
		if (colon != NULL && (size_t)(colon - line) == nlen) {
			bool eq = true;
			for (size_t i = 0; i < nlen && eq; i++) {
				eq = tolower((unsigned char)line[i]) == tolower((unsigned char)name[i]);
			}
			if (eq) {
				const char *v = colon + 1;
				const char *end = line + len;
				while (v < end && (*v == ' ' || *v == '\t')) {
					v++;
				}
				while (end > v && (end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) {
					end--;
				}
				const size_t vlen = (size_t)(end - v);
				if (vlen + 1 <= out_size) {
					memcpy(out, v, vlen);
					out[vlen] = '\0';
					found = true; // keep scanning: the last block wins
				} else {
					found = false;
				}
			}
		}
		if (eol == NULL) {
			break;
		}
		line = eol + 1;
	}
	return found;
}


#if defined(XRT_OS_WINDOWS)

/*
 *
 * Windows: WinHTTP, synchronous; a watcher thread closes the request handle on
 * cancel or at the deadline (closing a handle aborts the blocking call).
 *
 */

/*
 * Handle ownership (the cancel race): the request handle belongs to the
 * calling thread. The watcher only RECORDS a cancel / deadline; it closes the
 * handle itself only while the calling thread is inside a blocking WinHTTP
 * call (`in_call`), which is the documented way to abort a synchronous
 * call. The calling thread checks `stop` (under the lock) before every call
 * and never touches the handle once `closed` is set; non-blocking queries run
 * with the lock held, so the watcher cannot close under them. Whoever has not
 * closed it at the end - the calling thread - does.
 */
struct win_watch
{
	struct os_mutex mtx;
	HINTERNET req;
	bool done;
	bool in_call; //!< the calling thread is in (or entering) a blocking call on req
	bool closed;  //!< the watcher closed req: never touch it again
	bool cancelled;
	bool timed_out;
	uint64_t deadline_ns;
	const struct rc_http_request *rq;
};

static void *
win_watch_main(void *ptr)
{
	struct win_watch *w = (struct win_watch *)ptr;
	for (;;) {
		os_nanosleep(50ll * 1000 * 1000);
		os_mutex_lock(&w->mtx);
		if (w->done) {
			os_mutex_unlock(&w->mtx);
			return NULL;
		}
		if (!w->cancelled && !w->timed_out) {
			const bool cancel = w->rq->is_cancelled != NULL && w->rq->is_cancelled(w->rq->cancel_ctx);
			const bool late = (uint64_t)os_monotonic_get_ns() > w->deadline_ns;
			w->cancelled = cancel;
			w->timed_out = !cancel && late;
		}
		if ((w->cancelled || w->timed_out) && w->in_call && !w->closed) {
			WinHttpCloseHandle(w->req); // aborts the blocking call in progress
			w->closed = true;
		}
		os_mutex_unlock(&w->mtx);
	}
}

//! Enter a blocking call on w->req; false = stopped (cancel / deadline / closed): do not call.
static bool
win_call_begin(struct win_watch *w)
{
	os_mutex_lock(&w->mtx);
	const bool go = !w->cancelled && !w->timed_out && !w->closed;
	w->in_call = go;
	os_mutex_unlock(&w->mtx);
	return go;
}

static void
win_call_end(struct win_watch *w)
{
	os_mutex_lock(&w->mtx);
	w->in_call = false;
	os_mutex_unlock(&w->mtx);
}

//! Lock for a NON-blocking call on w->req; false (unlocked) = stopped.
static bool
win_quick_begin(struct win_watch *w)
{
	os_mutex_lock(&w->mtx);
	if (w->cancelled || w->timed_out || w->closed) {
		os_mutex_unlock(&w->mtx);
		return false;
	}
	return true;
}

static wchar_t *
widen_alloc(const char *s)
{
	const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	if (n <= 0) {
		return NULL;
	}
	wchar_t *w = (wchar_t *)calloc((size_t)n, sizeof(wchar_t));
	if (w != NULL) {
		(void)MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
	}
	return w;
}

bool
rc_http_available(char *why, size_t why_size)
{
	if (why != NULL && why_size > 0) {
		why[0] = '\0';
	}
	return true;
}

void
rc_http_perform(const struct rc_http_request *rq, struct rc_http_response *resp)
{
	memset(resp, 0, sizeof(*resp));
	const uint32_t timeout = rq->timeout_ms != 0 ? rq->timeout_ms : RC_HTTP_DEFAULT_TIMEOUT_MS;
	const uint64_t max = rq->max_response_bytes != 0 ? rq->max_response_bytes : RC_HTTP_DEFAULT_MAX_BYTES;
	wchar_t *wurl = widen_alloc(rq->url);
	wchar_t host[256], path[4096];
	URL_COMPONENTS uc;
	memset(&uc, 0, sizeof(uc));
	uc.dwStructSize = sizeof(uc);
	uc.lpszHostName = host;
	uc.dwHostNameLength = 256;
	uc.lpszUrlPath = path;
	uc.dwUrlPathLength = 4096;
	if (wurl == NULL || !WinHttpCrackUrl(wurl, 0, 0, &uc)) {
		free(wurl);
		set_msg(resp, RC_HTTP_CONNECT, "bad URL %s", rq->url);
		return;
	}
	free(wurl);
	// The path buffer holds path + query (WinHttpCrackUrl without extra info).
	HINTERNET ses = WinHttpOpen(L"DisplayXR-RemoteConversion/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
	                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	if (ses == NULL) {
		ses = WinHttpOpen(L"DisplayXR-RemoteConversion/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	}
	HINTERNET con = ses != NULL ? WinHttpConnect(ses, host, uc.nPort, 0) : NULL;
	wchar_t *wmethod = widen_alloc(rq->method != NULL ? rq->method : "GET");
	HINTERNET req =
	    con != NULL && wmethod != NULL
	        ? WinHttpOpenRequest(con, wmethod, path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
	                             uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
	        : NULL;
	free(wmethod);
	if (req == NULL) {
		set_msg(resp, RC_HTTP_CONNECT, "cannot open a connection to %s", rq->url);
		goto out_handles;
	}
	(void)WinHttpSetTimeouts(req, (int)timeout, (int)timeout, (int)timeout, (int)timeout);
	// Never follow a redirect: the Authorization header must reach only the configured host.
	DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
	(void)WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof(never));

	// Headers: one CRLF-joined wide block.
	size_t hlen = 0;
	for (const char *const *h = rq->headers; h != NULL && *h != NULL; h++) {
		hlen += strlen(*h) + 2;
	}
	char *hjoin = (char *)calloc(hlen + 1, 1);
	for (const char *const *h = rq->headers; hjoin != NULL && h != NULL && *h != NULL; h++) {
		strcat(hjoin, *h);
		strcat(hjoin, "\r\n");
	}
	wchar_t *whdr = hjoin != NULL && hlen > 0 ? widen_alloc(hjoin) : NULL;
	if (hjoin != NULL) {
		SecureZeroMemory(hjoin, hlen); // it may hold the bearer token
		free(hjoin);
	}

	struct win_watch w;
	memset(&w, 0, sizeof(w));
	(void)os_mutex_init(&w.mtx);
	w.req = req;
	w.rq = rq;
	w.deadline_ns = (uint64_t)os_monotonic_get_ns() + (uint64_t)timeout * 1000000ull;
	struct os_thread th;
	const bool watching = os_thread_init(&th) == 0 && os_thread_start(&th, win_watch_main, &w) == 0;

	BOOL ok = win_call_begin(&w);
	if (ok) {
		ok = WinHttpSendRequest(req, whdr != NULL ? whdr : WINHTTP_NO_ADDITIONAL_HEADERS,
		                        whdr != NULL ? (DWORD)-1L : 0, (LPVOID)rq->body, (DWORD)rq->body_size,
		                        (DWORD)rq->body_size, 0);
		win_call_end(&w);
	}
	if (whdr != NULL) {
		SecureZeroMemory(whdr, wcslen(whdr) * sizeof(wchar_t));
		free(whdr);
	}
	if (rq->progress != NULL && ok) {
		rq->progress(rq->progress_ctx, 1.0f, 0.0f);
	}
	if (ok && (ok = win_call_begin(&w))) {
		ok = WinHttpReceiveResponse(req, NULL);
		win_call_end(&w);
	}
	if (ok && (ok = win_quick_begin(&w))) {
		DWORD status = 0, sz = sizeof(status);
		ok = WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
		                         WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
		resp->status = (long)status;
		os_mutex_unlock(&w.mtx);
	}
	if (ok && win_quick_begin(&w)) {
		DWORD wsz = 0;
		(void)WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, NULL, &wsz,
		                          WINHTTP_NO_HEADER_INDEX);
		wchar_t *wh = wsz > 0 ? (wchar_t *)calloc(wsz / sizeof(wchar_t) + 1, sizeof(wchar_t)) : NULL;
		if (wh != NULL && WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
		                                      wh, &wsz, WINHTTP_NO_HEADER_INDEX)) {
			const int n = WideCharToMultiByte(CP_UTF8, 0, wh, -1, NULL, 0, NULL, NULL);
			resp->headers = n > 0 ? (char *)calloc((size_t)n + 1, 1) : NULL;
			if (resp->headers != NULL) {
				(void)WideCharToMultiByte(CP_UTF8, 0, wh, -1, resp->headers, n, NULL, NULL);
				resp->headers_size = strlen(resp->headers);
			}
		}
		free(wh);
		os_mutex_unlock(&w.mtx);
	}
	size_t cap = 0;
	bool too_large = false;
	uint8_t *chunk = (uint8_t *)malloc(64 * 1024);
	while (ok && chunk != NULL) {
		DWORD got = 0;
		if (!win_call_begin(&w)) {
			ok = FALSE;
			break;
		}
		ok = WinHttpReadData(req, chunk, 64 * 1024, &got);
		win_call_end(&w);
		if (!ok || got == 0) {
			break;
		}
		if (!buf_append(&resp->body, &resp->body_size, &cap, chunk, got, max)) {
			too_large = true;
			break;
		}
	}
	free(chunk);
	os_mutex_lock(&w.mtx);
	w.done = true;
	const bool cancelled = w.cancelled, timed_out = w.timed_out;
	if (!w.closed) {
		WinHttpCloseHandle(w.req); // ours: the watcher never closed it
		w.closed = true;
	}
	os_mutex_unlock(&w.mtx);
	if (watching) {
		os_thread_join(&th);
		os_thread_destroy(&th);
	}
	os_mutex_destroy(&w.mtx);
	req = NULL; // closed above (by us or the watcher)

	if (cancelled) {
		set_msg(resp, RC_HTTP_CANCELLED, "cancelled%s", "");
	} else if (timed_out) {
		set_msg(resp, RC_HTTP_TIMEOUT, "timed out talking to %s", rq->url);
	} else if (too_large) {
		set_msg(resp, RC_HTTP_TOO_LARGE, "response from %s is too large", rq->url);
	} else if (!ok && resp->status == 0) {
		char what[64];
		(void)snprintf(what, sizeof(what), "WinHTTP error %lu", (unsigned long)GetLastError());
		set_msg(resp, RC_HTTP_CONNECT, "%s", what);
	} else {
		resp->error = RC_HTTP_OK;
	}
	if (resp->error == RC_HTTP_OK && rq->progress != NULL) {
		rq->progress(rq->progress_ctx, 1.0f, 1.0f);
	}

out_handles:
	if (req != NULL) {
		WinHttpCloseHandle(req);
	}
	if (con != NULL) {
		WinHttpCloseHandle(con);
	}
	if (ses != NULL) {
		WinHttpCloseHandle(ses);
	}
}

#else

/*
 *
 * macOS / desktop Linux: libcurl, resolved at run time. The handful of
 * entry points and option numbers used are libcurl's stable ABI (7.32+).
 *
 */

typedef void CURL;
struct curl_slist;
typedef CURL *(*easy_init_fn)(void);
typedef int (*easy_setopt_fn)(CURL *, int, ...);
typedef int (*easy_perform_fn)(CURL *);
typedef int (*easy_getinfo_fn)(CURL *, int, ...);
typedef void (*easy_cleanup_fn)(CURL *);
typedef const char *(*easy_strerror_fn)(int);
typedef struct curl_slist *(*slist_append_fn)(struct curl_slist *, const char *);
typedef void (*slist_free_fn)(struct curl_slist *);
typedef int (*global_init_fn)(long);

#define RC_CURLOPT_WRITEDATA 10001
#define RC_CURLOPT_URL 10002
#define RC_CURLOPT_POSTFIELDS 10015
#define RC_CURLOPT_HTTPHEADER 10023
#define RC_CURLOPT_HEADERDATA 10029
#define RC_CURLOPT_CUSTOMREQUEST 10036
#define RC_CURLOPT_XFERINFODATA 10057
#define RC_CURLOPT_WRITEFUNCTION 20011
#define RC_CURLOPT_HEADERFUNCTION 20079
#define RC_CURLOPT_XFERINFOFUNCTION 20219
#define RC_CURLOPT_NOPROGRESS 43
#define RC_CURLOPT_POST 47
#define RC_CURLOPT_FOLLOWLOCATION 52
#define RC_CURLOPT_HTTPGET 80
#define RC_CURLOPT_NOSIGNAL 99
#define RC_CURLOPT_TIMEOUT_MS 155
#define RC_CURLOPT_CONNECTTIMEOUT_MS 156
#define RC_CURLOPT_POSTFIELDSIZE_LARGE 30120
#define RC_CURLINFO_RESPONSE_CODE 0x200002
#define RC_CURLE_OK 0
#define RC_CURLE_WRITE_ERROR 23
#define RC_CURLE_OPERATION_TIMEDOUT 28
#define RC_CURLE_ABORTED_BY_CALLBACK 42
#define RC_CURL_GLOBAL_ALL 3L

static struct
{
	bool tried;
	bool ok;
	char why[160];
	void *lib;
	easy_init_fn init;
	easy_setopt_fn setopt;
	easy_perform_fn perform;
	easy_getinfo_fn getinfo;
	easy_cleanup_fn cleanup;
	easy_strerror_fn strerror;
	slist_append_fn slist_append;
	slist_free_fn slist_free;
} g_curl;

static pthread_mutex_t g_curl_mtx = PTHREAD_MUTEX_INITIALIZER;

static bool
curl_load(void)
{
	// Called under g_curl_mtx.
	if (g_curl.tried) {
		return g_curl.ok;
	}
	g_curl.tried = true;
#if defined(XRT_OS_MACOS)
	static const char *names[] = {"/usr/lib/libcurl.4.dylib", "libcurl.4.dylib", "libcurl.dylib", NULL};
#else
	static const char *names[] = {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl-nss.so.4", NULL};
#endif
	for (int i = 0; names[i] != NULL && g_curl.lib == NULL; i++) {
		g_curl.lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
	}
	if (g_curl.lib == NULL) {
		(void)snprintf(g_curl.why, sizeof(g_curl.why),
		               "libcurl (libcurl.so.4) is not installed: the remote provider cannot reach an endpoint");
		return false;
	}
	g_curl.init = (easy_init_fn)dlsym(g_curl.lib, "curl_easy_init");
	g_curl.setopt = (easy_setopt_fn)dlsym(g_curl.lib, "curl_easy_setopt");
	g_curl.perform = (easy_perform_fn)dlsym(g_curl.lib, "curl_easy_perform");
	g_curl.getinfo = (easy_getinfo_fn)dlsym(g_curl.lib, "curl_easy_getinfo");
	g_curl.cleanup = (easy_cleanup_fn)dlsym(g_curl.lib, "curl_easy_cleanup");
	g_curl.strerror = (easy_strerror_fn)dlsym(g_curl.lib, "curl_easy_strerror");
	g_curl.slist_append = (slist_append_fn)dlsym(g_curl.lib, "curl_slist_append");
	g_curl.slist_free = (slist_free_fn)dlsym(g_curl.lib, "curl_slist_free_all");
	global_init_fn global_init = (global_init_fn)dlsym(g_curl.lib, "curl_global_init");
	if (g_curl.init == NULL || g_curl.setopt == NULL || g_curl.perform == NULL || g_curl.getinfo == NULL ||
	    g_curl.cleanup == NULL || g_curl.slist_append == NULL || g_curl.slist_free == NULL || global_init == NULL) {
		(void)snprintf(g_curl.why, sizeof(g_curl.why), "libcurl is missing required entry points");
		return false;
	}
	if (global_init(RC_CURL_GLOBAL_ALL) != RC_CURLE_OK) {
		(void)snprintf(g_curl.why, sizeof(g_curl.why), "curl_global_init failed");
		return false;
	}
	g_curl.ok = true;
	return true;
}

static bool
curl_ready(char *why, size_t why_size)
{
	pthread_mutex_lock(&g_curl_mtx);
	const bool ok = curl_load();
	if (!ok && why != NULL && why_size > 0) {
		(void)snprintf(why, why_size, "%s", g_curl.why);
	}
	pthread_mutex_unlock(&g_curl_mtx);
	return ok;
}

bool
rc_http_available(char *why, size_t why_size)
{
	if (why != NULL && why_size > 0) {
		why[0] = '\0';
	}
	return curl_ready(why, why_size);
}

struct curl_ctx
{
	struct rc_http_response *resp;
	size_t body_cap, hdr_cap;
	uint64_t max;
	bool too_large;
	bool cancelled;
	const struct rc_http_request *rq;
};

static size_t
curl_on_body(char *ptr, size_t size, size_t nmemb, void *ud)
{
	struct curl_ctx *c = (struct curl_ctx *)ud;
	const size_t n = size * nmemb;
	if (!buf_append(&c->resp->body, &c->resp->body_size, &c->body_cap, ptr, n, c->max)) {
		c->too_large = true;
		return 0; // CURLE_WRITE_ERROR
	}
	return n;
}

static size_t
curl_on_header(char *ptr, size_t size, size_t nmemb, void *ud)
{
	struct curl_ctx *c = (struct curl_ctx *)ud;
	const size_t n = size * nmemb;
	uint8_t *h = (uint8_t *)c->resp->headers;
	if (!buf_append(&h, &c->resp->headers_size, &c->hdr_cap, ptr, n, 1024u * 1024u)) {
		return 0;
	}
	c->resp->headers = (char *)h;
	return n;
}

static int
curl_on_progress(void *ud, int64_t dltotal, int64_t dlnow, int64_t ultotal, int64_t ulnow)
{
	struct curl_ctx *c = (struct curl_ctx *)ud;
	if (c->rq->is_cancelled != NULL && c->rq->is_cancelled(c->rq->cancel_ctx)) {
		c->cancelled = true;
		return 1; // abort: the connection is closed (contract v1 cancel)
	}
	if (c->rq->progress != NULL) {
		const float up = ultotal > 0 ? (float)ulnow / (float)ultotal : 0.0f;
		const float down = dltotal > 0 ? (float)dlnow / (float)dltotal : 0.0f;
		c->rq->progress(c->rq->progress_ctx, up, down);
	}
	return 0;
}

void
rc_http_perform(const struct rc_http_request *rq, struct rc_http_response *resp)
{
	memset(resp, 0, sizeof(*resp));
	char why[160];
	if (!curl_ready(why, sizeof(why))) {
		set_msg(resp, RC_HTTP_UNSUPPORTED, "%s", why);
		return;
	}
	CURL *h = g_curl.init();
	if (h == NULL) {
		set_msg(resp, RC_HTTP_CONNECT, "curl_easy_init failed%s", "");
		return;
	}
	struct curl_ctx c;
	memset(&c, 0, sizeof(c));
	c.resp = resp;
	c.rq = rq;
	c.max = rq->max_response_bytes != 0 ? rq->max_response_bytes : RC_HTTP_DEFAULT_MAX_BYTES;
	const long timeout = (long)(rq->timeout_ms != 0 ? rq->timeout_ms : RC_HTTP_DEFAULT_TIMEOUT_MS);

	struct curl_slist *hdrs = NULL;
	for (const char *const *p = rq->headers; p != NULL && *p != NULL; p++) {
		hdrs = g_curl.slist_append(hdrs, *p);
	}
	g_curl.setopt(h, RC_CURLOPT_URL, rq->url);
	g_curl.setopt(h, RC_CURLOPT_NOSIGNAL, 1L);
	g_curl.setopt(h, RC_CURLOPT_FOLLOWLOCATION, 0L); // the token reaches only the configured host
	g_curl.setopt(h, RC_CURLOPT_TIMEOUT_MS, timeout);
	g_curl.setopt(h, RC_CURLOPT_CONNECTTIMEOUT_MS, timeout < 15000L ? timeout : 15000L);
	g_curl.setopt(h, RC_CURLOPT_WRITEFUNCTION, curl_on_body);
	g_curl.setopt(h, RC_CURLOPT_WRITEDATA, (void *)&c);
	g_curl.setopt(h, RC_CURLOPT_HEADERFUNCTION, curl_on_header);
	g_curl.setopt(h, RC_CURLOPT_HEADERDATA, (void *)&c);
	g_curl.setopt(h, RC_CURLOPT_XFERINFOFUNCTION, curl_on_progress);
	g_curl.setopt(h, RC_CURLOPT_XFERINFODATA, (void *)&c);
	g_curl.setopt(h, RC_CURLOPT_NOPROGRESS, 0L);
	if (hdrs != NULL) {
		g_curl.setopt(h, RC_CURLOPT_HTTPHEADER, hdrs);
	}
	if (rq->method != NULL && strcmp(rq->method, "POST") == 0) {
		g_curl.setopt(h, RC_CURLOPT_POST, 1L);
		g_curl.setopt(h, RC_CURLOPT_POSTFIELDSIZE_LARGE, (int64_t)rq->body_size);
		g_curl.setopt(h, RC_CURLOPT_POSTFIELDS, rq->body != NULL ? rq->body : (const void *)"");
	} else if (rq->method == NULL || strcmp(rq->method, "GET") == 0) {
		g_curl.setopt(h, RC_CURLOPT_HTTPGET, 1L);
	} else {
		g_curl.setopt(h, RC_CURLOPT_CUSTOMREQUEST, rq->method);
	}
	const int rc = g_curl.perform(h);
	long status = 0;
	(void)g_curl.getinfo(h, RC_CURLINFO_RESPONSE_CODE, &status);
	resp->status = status;
	g_curl.cleanup(h);
	g_curl.slist_free(hdrs); // the list held the Authorization line; curl frees (not wipes) it

	if (c.cancelled || (rc == RC_CURLE_ABORTED_BY_CALLBACK && rq->is_cancelled != NULL)) {
		set_msg(resp, RC_HTTP_CANCELLED, "cancelled%s", "");
	} else if (c.too_large) {
		set_msg(resp, RC_HTTP_TOO_LARGE, "response from %s is too large", rq->url);
	} else if (rc == RC_CURLE_OPERATION_TIMEDOUT) {
		set_msg(resp, RC_HTTP_TIMEOUT, "timed out talking to %s", rq->url);
	} else if (rc != RC_CURLE_OK) {
		set_msg(resp, RC_HTTP_CONNECT, "%s", g_curl.strerror != NULL ? g_curl.strerror(rc) : "transfer failed");
	} else {
		resp->error = RC_HTTP_OK;
	}
}

#endif
