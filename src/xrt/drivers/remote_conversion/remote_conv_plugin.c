// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  DisplayXR-RemoteConversion: the generic remote conversion provider
 *         (ADR-057 §2, endpoint contract v1 §6).
 *
 * Serves SPLATS_MONO (cap 1) and SPLATS_STEREO (cap 2) on the job lane by
 * forwarding each job to an HTTPS endpoint that implements contract v1:
 *
 *   GET  <base>/v1/describe  tasks, limits, licence class, build stamp
 *   POST <base>/v1/jobs      multipart: `params` (JSON) + `image` | `left` + `right`
 *                            200: body = the blob, header X-DXR-Result = the facts
 *                            422 {code, message}: bad input, never retried
 *                            5xx / 429 / 408 / 401 / 403 / 404 / timeout: next host
 *                            cancel = the connection is closed
 *
 * An endpoint is configuration, not code: ENDPOINTS is an ordered fallback
 * list of base URLs (https, or http on loopback only). The bearer token comes
 * ONLY from the OS credential store, by the name in CREDENTIAL. Sending a photo
 * off the box needs the user's per-provider consent (host->get_consent,
 * `displayxr-cli conv consent remote-conversion grant`): without it both
 * capabilities report DISABLED, "consent required". The licence class is
 * reported exactly as /v1/describe states it (research_only stays research
 * only; AllowResearchOnly decides). Every result carries the build stamp of
 * the host that answered; differing builds across hosts of one list log one
 * WARN.
 *
 * Off by default, like the sims: probe() declines unless ENABLE = 1.
 *
 * @ingroup drv_remote_conversion
 */

#include "xrt/xrt_conversion_plugin.h"

#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_credential.h"
#include "util/u_logging.h"

#include "remote_conv_http.h"

#include <cjson/cJSON.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image_write.h"
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif


/*
 *
 * Defines.
 *
 */

#define RC_ID "remote-conversion"
#define RC_BUILD "remote-conversion/1 contract-1"
#define RC_CAP_MONO 1u
#define RC_CAP_STEREO 2u
#define RC_CAP_COUNT 2u
#define RC_MAX_HOSTS 8u
#define RC_URL_MAX 256u
#define RC_OPTIONS_MAX 2048u
#define RC_CONSENT_TTL_NS (1000ll * 1000 * 1000)
#define RC_SINK_CHUNK (256u * 1024u)
#define RC_DEFAULT_TIMEOUT_S 120u
#define RC_DEFAULT_MAX_RESPONSE_MB 512u //!< a SHARP-class SOG is ~10 MB; this bounds a hostile endpoint

#define COVERS(PTR, TYPE, FIELD) XRT_CONV_COVERS(PTR, TYPE, FIELD)
#define SET(PTR, TYPE, FIELD, VALUE)                                                                                   \
	do {                                                                                                           \
		if (COVERS(PTR, TYPE, FIELD)) {                                                                        \
			(PTR)->FIELD = (VALUE);                                                                        \
		}                                                                                                      \
	} while (0)

static const struct xrt_conversion_plugin_host_iface *g_host = NULL;

//! One /v1/describe answer.
struct rc_describe
{
	uint32_t contract;
	char endpoint_id[64];
	bool serves[RC_CAP_COUNT + 1]; //!< by cap id
	uint32_t max_long_side[RC_CAP_COUNT + 1];
	uint64_t max_bytes[RC_CAP_COUNT + 1];        //!< largest INPUT it accepts (0 = no limit stated)
	uint64_t max_result_bytes[RC_CAP_COUNT + 1]; //!< largest result it states (0 = not stated)
	uint32_t blob_formats[RC_CAP_COUNT + 1]; //!< 1u << XRT_CONV_BLOB_*
	bool rectified_required;
	uint32_t licence_class; //!< XRT_CONV_LICENCE_*
	char licence_name[48];
	char licence_link[160];
	/* build */
	char provider_commit[64];
	char model_id[64];
	char model_sha256[72];
	char runtime[32];
	char runtime_version[32];
	char host_kind[16];
	char host_id[64];
};

struct rc_host
{
	char base[RC_URL_MAX];
	bool described;
	struct rc_describe d;
};

struct rc
{
	struct os_mutex lock;

	/* Settings (read at probe). */
	uint32_t host_count;
	struct rc_host hosts[RC_MAX_HOSTS];
	char credential[128];
	uint32_t timeout_s;
	uint64_t max_response_bytes;  //!< MAX_RESPONSE_MB: the largest blob accepted from any endpoint
	char options[RC_OPTIONS_MAX]; //!< a JSON object, passed through as params.options ("" = none)
	uint32_t blob_format;         //!< XRT_CONV_BLOB_SOG / PLY_3DGS
	char config_error[128];       //!< a bad setting: DISABLED with this detail

	/* Readiness. */
	bool http_ok;
	char http_why[160];
	bool consent;
	int64_t consent_read_ns;
	uint32_t state; //!< XRT_CONV_STATE_* of the warm machine (both caps share it)
	uint32_t fail_reason;
	bool retryable;
	char detail[XRT_CONV_DETAIL_MAX];
	float progress;

	/* What the endpoint list said. */
	bool have_describe;
	struct rc_describe cur; //!< the host that answered last
	uint64_t generation;
	bool build_logged;
	bool build_differs_warned;

	/* The background describe (licence before the first warm). */
	struct os_thread bg;
	bool bg_started;
};


/*
 *
 * Small helpers.
 *
 */

static void
copy_str(char *dst, size_t cap, const char *src)
{
	if (cap == 0) {
		return;
	}
	size_t n = src != NULL ? strlen(src) : 0;
	if (n >= cap) {
		n = cap - 1;
	}
	if (n > 0) {
		memcpy(dst, src, n);
	}
	dst[n] = 0;
}

static bool
setting(const char *key, char *buf, uint32_t size)
{
	buf[0] = 0;
	if (g_host == NULL || !COVERS(g_host, struct xrt_conversion_plugin_host_iface, get_setting) ||
	    g_host->get_setting == NULL) {
		return false;
	}
	return g_host->get_setting(key, buf, size) && buf[0] != 0;
}

static bool
host_consent(void)
{
	// get_consent was carved from reserved[0]: the struct_size covers it on every
	// host, so a NULL pointer is what an older host looks like. Fail closed.
	return g_host != NULL && COVERS(g_host, struct xrt_conversion_plugin_host_iface, get_consent) &&
	       g_host->get_consent != NULL && g_host->get_consent(XRT_CONV_CONSENT_OFFBOX);
}

static void
notify(uint32_t cap_id)
{
	if (g_host != NULL && COVERS(g_host, struct xrt_conversion_plugin_host_iface, notify_changed) &&
	    g_host->notify_changed != NULL) {
		g_host->notify_changed(cap_id);
	}
}

static bool
never_cancelled(void *ctx)
{
	(void)ctx;
	return false;
}

//! https://..., or http:// to a loopback host only (a token never crosses a network in clear).
static bool
url_allowed(const char *u)
{
	return rc_url_endpoint_allowed(u);
}

static const char *
task_word(uint32_t cap_id)
{
	return cap_id == RC_CAP_STEREO ? "splats-stereo" : "splats-mono";
}

static const char *
format_word(uint32_t blob_format)
{
	return blob_format == XRT_CONV_BLOB_PLY_3DGS ? "ply" : "sog";
}

static uint32_t
format_parse(const char *s)
{
	if (s == NULL) {
		return 0;
	}
	if (strcmp(s, "sog") == 0) {
		return XRT_CONV_BLOB_SOG;
	}
	if (strcmp(s, "ply") == 0) {
		return XRT_CONV_BLOB_PLY_3DGS;
	}
	if (strcmp(s, "spz") == 0) {
		return XRT_CONV_BLOB_SPZ;
	}
	return 0;
}

static const char *
jstr(const cJSON *o, const char *key)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
	return cJSON_IsString(v) ? v->valuestring : NULL;
}

static bool
jnum(const cJSON *o, const char *key, double *out)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
	if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble)) {
		return false;
	}
	*out = v->valuedouble;
	return true;
}

static float
jfloat(const cJSON *o, const char *key, float dflt)
{
	double d;
	if (!jnum(o, key, &d)) {
		return dflt;
	}
	// Untrusted: keep it representable (a float cast out of range is undefined).
	return (float)(d > 1e30 ? 1e30 : d < -1e30 ? -1e30 : d);
}

/*! An untrusted JSON number as an unsigned integer in [0, @p max]; @p dflt when
 *  absent, negative or not finite. Never a float-to-int cast out of range. */
static uint64_t
jcount(const cJSON *o, const char *key, uint64_t max, uint64_t dflt)
{
	double d;
	if (!jnum(o, key, &d) || d < 0.0) {
		return dflt;
	}
	return d >= (double)max ? max : (uint64_t)d;
}

//! A bare cJSON number the same way.
static uint64_t
jcount_item(const cJSON *v, uint64_t max)
{
	if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < 0.0) {
		return 0;
	}
	return v->valuedouble >= (double)max ? max : (uint64_t)v->valuedouble;
}

static uint32_t
refusal_parse(const char *code)
{
	if (code == NULL) {
		return XRT_CONV_REFUSAL_NONE;
	}
	struct
	{
		const char *word;
		uint32_t value;
	} map[] = {
	    {"none", XRT_CONV_REFUSAL_NONE},
	    {"metric_fit_refused", XRT_CONV_REFUSAL_METRIC_FIT_REFUSED},
	    {"baseline_missing", XRT_CONV_REFUSAL_BASELINE_MISSING},
	    {"input_too_large", XRT_CONV_REFUSAL_INPUT_TOO_LARGE},
	    {"unrectified_pair", XRT_CONV_REFUSAL_UNRECTIFIED_PAIR},
	    {"unsupported_input", XRT_CONV_REFUSAL_UNSUPPORTED_INPUT},
	    {"remote_error", XRT_CONV_REFUSAL_REMOTE_ERROR},
	};
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		size_t k = 0;
		bool eq = true;
		for (; map[i].word[k] != '\0' && eq; k++) {
			char c = code[k];
			c = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
			eq = c == map[i].word[k];
		}
		if (eq && code[k] == '\0') {
			return map[i].value;
		}
	}
	// An unknown code is still an input refusal: unsupported, with the endpoint's words.
	return XRT_CONV_REFUSAL_UNSUPPORTED_INPUT;
}

static void
set_refusal(struct xrt_conv_job_out *out, uint32_t code, const char *reason)
{
	if (out == NULL || !COVERS(out, struct xrt_conv_job_out, splat_result) || out->splat_result == NULL) {
		return;
	}
	struct xrt_conv_splat_result *r = out->splat_result;
	SET(r, struct xrt_conv_splat_result, refusal_code, code);
	if (COVERS(r, struct xrt_conv_splat_result, refusal_reason)) {
		copy_str(r->refusal_reason, sizeof(r->refusal_reason), reason);
	}
}


/*
 *
 * Settings.
 *
 */

static void
load_settings(struct rc *rc)
{
	char buf[2048];
	rc->timeout_s = RC_DEFAULT_TIMEOUT_S;
	rc->max_response_bytes = (uint64_t)RC_DEFAULT_MAX_RESPONSE_MB << 20;
	if (setting("MAX_RESPONSE_MB", buf, sizeof(buf))) {
		const unsigned long v = strtoul(buf, NULL, 10);
		if (v >= 1 && v <= 4096) {
			rc->max_response_bytes = (uint64_t)v << 20;
		}
	}
	if (setting("TIMEOUT_S", buf, sizeof(buf))) {
		const unsigned long v = strtoul(buf, NULL, 10);
		rc->timeout_s = v >= 1 && v <= 3600 ? (uint32_t)v : RC_DEFAULT_TIMEOUT_S;
	}
	rc->blob_format = XRT_CONV_BLOB_SOG;
	if (setting("FORMAT", buf, sizeof(buf))) {
		const uint32_t f = format_parse(buf);
		if (f == XRT_CONV_BLOB_SOG || f == XRT_CONV_BLOB_PLY_3DGS) {
			rc->blob_format = f;
		} else {
			(void)snprintf(rc->config_error, sizeof(rc->config_error), "FORMAT '%.32s' is not sog or ply",
			               buf);
		}
	}
	if (setting("CREDENTIAL", buf, sizeof(buf))) {
		if (u_credential_name_valid(buf)) {
			copy_str(rc->credential, sizeof(rc->credential), buf);
		} else {
			copy_str(rc->config_error, sizeof(rc->config_error),
			         "CREDENTIAL is not a valid credential name");
		}
	}
	if (setting("OPTIONS", buf, sizeof(buf))) {
		cJSON *o = cJSON_Parse(buf);
		if (cJSON_IsObject(o) && strlen(buf) < sizeof(rc->options)) {
			copy_str(rc->options, sizeof(rc->options), buf);
		} else {
			copy_str(rc->config_error, sizeof(rc->config_error), "OPTIONS is not a JSON object");
		}
		cJSON_Delete(o);
	}
	// ENDPOINTS: base URLs separated by comma, semicolon or white space, in fallback order.
	if (setting("ENDPOINTS", buf, sizeof(buf))) {
		char *save = buf;
		while (*save != '\0' && rc->host_count < RC_MAX_HOSTS) {
			while (*save == ',' || *save == ';' || *save == ' ' || *save == '\t' || *save == '\n') {
				save++;
			}
			char *start = save;
			while (*save != '\0' && *save != ',' && *save != ';' && *save != ' ' && *save != '\t' &&
			       *save != '\n') {
				save++;
			}
			if (save == start) {
				break;
			}
			const char saved = *save;
			*save = '\0';
			size_t n = strlen(start);
			while (n > 0 && start[n - 1] == '/') {
				start[--n] = '\0';
			}
			if (n == 0 || n >= RC_URL_MAX) {
				// skip
			} else if (!url_allowed(start)) {
				(void)snprintf(rc->config_error, sizeof(rc->config_error),
				               "endpoint '%.60s' is not https (plain http is allowed on loopback only)",
				               start);
			} else {
				copy_str(rc->hosts[rc->host_count].base, RC_URL_MAX, start);
				rc->host_count++;
			}
			*save = saved;
		}
	}
}


/*
 *
 * Readiness.
 *
 */

static void
bg_describe_start_locked(struct rc *rc);

//! Refresh the cached consent (at most once a second from [C] slots). rc->lock held.
static void
consent_refresh_locked(struct rc *rc, bool force)
{
	const int64_t now = os_monotonic_get_ns();
	if (!force && rc->consent_read_ns != 0 && now - rc->consent_read_ns < RC_CONSENT_TTL_NS) {
		return;
	}
	const bool was = rc->consent;
	rc->consent = host_consent();
	rc->consent_read_ns = now;
	if (was != rc->consent && rc->consent_read_ns != 0) {
		rc->generation++;
	}
	bg_describe_start_locked(rc);
}

struct rc_eval
{
	uint32_t state, reason;
	bool retryable;
	char detail[XRT_CONV_DETAIL_MAX];
};

//! What get_state answers for @p cap_id. rc->lock held.
static struct rc_eval
eval_locked(struct rc *rc, uint32_t cap_id)
{
	struct rc_eval e;
	memset(&e, 0, sizeof(e));
	if (!rc->consent) {
		e.state = XRT_CONV_STATE_DISABLED;
		copy_str(e.detail, sizeof(e.detail),
		         "consent required: displayxr-cli conv consent " RC_ID " grant (photos leave this machine)");
	} else if (!rc->http_ok) {
		e.state = XRT_CONV_STATE_ABSENT;
		e.reason = XRT_CONV_FAIL_PLATFORM;
		copy_str(e.detail, sizeof(e.detail), rc->http_why);
	} else if (rc->config_error[0] != '\0') {
		e.state = XRT_CONV_STATE_DISABLED;
		copy_str(e.detail, sizeof(e.detail), rc->config_error);
	} else if (rc->host_count == 0) {
		e.state = XRT_CONV_STATE_DISABLED;
		copy_str(e.detail, sizeof(e.detail), "no ENDPOINTS configured (conv set " RC_ID " ENDPOINTS <url>)");
	} else if (rc->state == XRT_CONV_STATE_READY && rc->have_describe && !rc->cur.serves[cap_id]) {
		e.state = XRT_CONV_STATE_ABSENT;
		e.reason = XRT_CONV_FAIL_MODEL;
		(void)snprintf(e.detail, sizeof(e.detail), "endpoint %s does not serve %s", rc->cur.endpoint_id,
		               task_word(cap_id));
	} else {
		e.state = rc->state;
		e.reason = rc->state == XRT_CONV_STATE_FAILED ? rc->fail_reason : 0;
		e.retryable = rc->retryable;
		copy_str(e.detail, sizeof(e.detail), rc->detail);
	}
	return e;
}

static void
set_state_locked(struct rc *rc, uint32_t state, uint32_t reason, bool retryable, const char *detail)
{
	rc->state = state;
	rc->fail_reason = reason;
	rc->retryable = retryable;
	copy_str(rc->detail, sizeof(rc->detail), detail);
}


/*
 *
 * Describe.
 *
 */

static void
headers_with_token(const char *token, char *auth_line, size_t auth_size, const char *extra, const char *hdrs[4])
{
	uint32_t n = 0;
	if (token != NULL && token[0] != '\0') {
		(void)snprintf(auth_line, auth_size, "Authorization: Bearer %s", token);
		hdrs[n++] = auth_line;
	}
	if (extra != NULL) {
		hdrs[n++] = extra;
	}
	hdrs[n++] = "Accept: application/json, application/octet-stream";
	hdrs[n] = NULL;
}

static void
describe_parse_build(const cJSON *b, struct rc_describe *d)
{
	copy_str(d->provider_commit, sizeof(d->provider_commit), jstr(b, "provider_commit"));
	copy_str(d->model_id, sizeof(d->model_id), jstr(b, "model_id"));
	copy_str(d->model_sha256, sizeof(d->model_sha256), jstr(b, "model_sha256"));
	copy_str(d->runtime, sizeof(d->runtime), jstr(b, "runtime"));
	copy_str(d->runtime_version, sizeof(d->runtime_version), jstr(b, "runtime_version"));
	copy_str(d->host_kind, sizeof(d->host_kind), jstr(b, "host_kind"));
	copy_str(d->host_id, sizeof(d->host_id), jstr(b, "host_id"));
}

static bool
describe_parse(const char *json, struct rc_describe *d, char *err, size_t err_size)
{
	memset(d, 0, sizeof(*d));
	cJSON *root = cJSON_Parse(json);
	if (!cJSON_IsObject(root)) {
		cJSON_Delete(root);
		copy_str(err, err_size, "describe is not a JSON object");
		return false;
	}
	d->contract = (uint32_t)jcount(root, "contract", 1000u, 0u);
	copy_str(d->endpoint_id, sizeof(d->endpoint_id), jstr(root, "endpoint_id"));
	const cJSON *t = NULL;
	cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(root, "tasks"))
	{
		const char *task = jstr(t, "task");
		const uint32_t cap = task == NULL                         ? 0u
		                     : strcmp(task, "splats-mono") == 0   ? RC_CAP_MONO
		                     : strcmp(task, "splats-stereo") == 0 ? RC_CAP_STEREO
		                                                          : 0u;
		if (cap == 0) {
			continue;
		}
		d->serves[cap] = true;
		d->max_long_side[cap] = (uint32_t)jcount(t, "max_long_side", 65536u, 0u);
		d->max_bytes[cap] = jcount(t, "max_bytes", 1ull << 40, 0u);
		// Optional (beyond v1's required keys): the largest result it returns.
		d->max_result_bytes[cap] = jcount(t, "max_result_bytes", 1ull << 40, 0u);
		const cJSON *rr = cJSON_GetObjectItemCaseSensitive(t, "rectified_required");
		if (cap == RC_CAP_STEREO && cJSON_IsTrue(rr)) {
			d->rectified_required = true;
		}
		const cJSON *f = NULL;
		cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(t, "formats"))
		{
			const uint32_t bf = cJSON_IsString(f) ? format_parse(f->valuestring) : 0u;
			if (bf != 0) {
				d->blob_formats[cap] |= 1u << bf;
			}
		}
	}
	const cJSON *lic = cJSON_GetObjectItemCaseSensitive(root, "licence");
	const char *cls = jstr(lic, "class");
	// As stated: remote hosting never makes a research-only model commercial.
	d->licence_class = cls == NULL                         ? XRT_CONV_LICENCE_UNKNOWN
	                   : strcmp(cls, "commercial") == 0    ? XRT_CONV_LICENCE_COMMERCIAL
	                   : strcmp(cls, "research_only") == 0 ? XRT_CONV_LICENCE_RESEARCH_ONLY
	                                                       : XRT_CONV_LICENCE_UNKNOWN;
	copy_str(d->licence_name, sizeof(d->licence_name), jstr(lic, "name"));
	copy_str(d->licence_link, sizeof(d->licence_link), jstr(lic, "link"));
	describe_parse_build(cJSON_GetObjectItemCaseSensitive(root, "build"), d);
	cJSON_Delete(root);
	if (d->contract != 1) {
		(void)snprintf(err, err_size, "endpoint speaks contract %u, this provider speaks 1", d->contract);
		return false;
	}
	return true;
}

static bool
build_equal(const struct rc_describe *a, const struct rc_describe *b)
{
	return strcmp(a->provider_commit, b->provider_commit) == 0 && strcmp(a->model_id, b->model_id) == 0 &&
	       strcmp(a->model_sha256, b->model_sha256) == 0 && strcmp(a->runtime, b->runtime) == 0 &&
	       strcmp(a->runtime_version, b->runtime_version) == 0;
}

static void
log_build(const struct rc_host *h)
{
	U_LOG_W(
	    "remote conversion: endpoint %s at %s - build provider=%s model=%s sha256=%.12s runtime=%s %s host=%s/%s, "
	    "licence %s",
	    h->d.endpoint_id[0] != '\0' ? h->d.endpoint_id : "?", h->base, h->d.provider_commit, h->d.model_id,
	    h->d.model_sha256, h->d.runtime, h->d.runtime_version, h->d.host_kind, h->d.host_id,
	    h->d.licence_class == XRT_CONV_LICENCE_COMMERCIAL      ? "commercial"
	    : h->d.licence_class == XRT_CONV_LICENCE_RESEARCH_ONLY ? "research_only"
	                                                           : "unknown");
}

/*!
 * GET <base>/v1/describe of host @p i. Fills the host's describe; WARNs once
 * when its build differs from another described host of the list. Returns the
 * HTTP outcome class: 0 = ok, 1 = fall through (transport / 5xx / 4xx),
 * 2 = credential refused (401 / 403), 3 = contract mismatch.
 */
static int
describe_host(
    struct rc *rc, uint32_t i, const char *token, bool (*is_cancelled)(void *), void *ctx, char *err, size_t err_size)
{
	char url[RC_URL_MAX + 32];
	(void)snprintf(url, sizeof(url), "%s/v1/describe", rc->hosts[i].base);
	char auth[U_CREDENTIAL_SECRET_MAX + 32];
	const char *hdrs[4];
	headers_with_token(token, auth, sizeof(auth), NULL, hdrs);
	struct rc_http_request rq;
	memset(&rq, 0, sizeof(rq));
	rq.method = "GET";
	rq.url = url;
	rq.headers = hdrs;
	rq.timeout_ms = rc->timeout_s < 30 ? rc->timeout_s * 1000u : 30000u;
	rq.max_response_bytes = 256u * 1024u;
	rq.is_cancelled = is_cancelled;
	rq.cancel_ctx = ctx;
	struct rc_http_response resp;
	rc_http_perform(&rq, &resp);
	u_credential_wipe(auth, sizeof(auth));
	int outcome = 1;
	if (resp.error != RC_HTTP_OK) {
		(void)snprintf(err, err_size, "%s: %s", rc->hosts[i].base, resp.message);
	} else if (resp.status == 401 || resp.status == 403) {
		(void)snprintf(err, err_size, "%s: HTTP %ld (the credential was refused)", rc->hosts[i].base,
		               resp.status);
		outcome = 2;
	} else if (resp.status != 200) {
		(void)snprintf(err, err_size, "%s: HTTP %ld on /v1/describe", rc->hosts[i].base, resp.status);
	} else {
		struct rc_describe d;
		char perr[96];
		if (!describe_parse(resp.body != NULL ? (const char *)resp.body : "", &d, perr, sizeof(perr))) {
			(void)snprintf(err, err_size, "%s: %s", rc->hosts[i].base, perr);
			outcome = 3;
		} else {
			os_mutex_lock(&rc->lock);
			rc->hosts[i].d = d;
			rc->hosts[i].described = true;
			bool differs = false;
			for (uint32_t k = 0; k < rc->host_count; k++) {
				if (k != i && rc->hosts[k].described && !build_equal(&rc->hosts[k].d, &d)) {
					differs = true;
				}
			}
			const bool warn = differs && !rc->build_differs_warned;
			rc->build_differs_warned = rc->build_differs_warned || differs;
			os_mutex_unlock(&rc->lock);
			log_build(&rc->hosts[i]);
			if (warn) {
				U_LOG_W(
				    "remote conversion: the hosts of one endpoint list report DIFFERENT builds (%s is "
				    "%s / "
				    "%.12s) - results are not interchangeable; keep the list on one build",
				    rc->hosts[i].base, d.provider_commit, d.model_sha256);
			}
			outcome = 0;
		}
	}
	rc_http_response_free(&resp);
	return outcome;
}

//! Read the token named by CREDENTIAL ("" when none is configured). False = configured but unreadable.
static bool
read_token(struct rc *rc, char *token, size_t token_size, char *err, size_t err_size)
{
	token[0] = '\0';
	if (rc->credential[0] == '\0') {
		return true;
	}
	return u_credential_read(rc->credential, token, token_size, err, err_size) == U_CREDENTIAL_OK;
}


/*
 * The licence class must be known BEFORE the first warm: selection drops a
 * capability whose class is UNKNOWN unless AllowResearchOnly (spec §3.4), and
 * a capability that is never selected is never warmed - so a commercial
 * endpoint would never be reachable through the service. Once the user has
 * granted consent (and the endpoint list is usable), one background GET
 * /v1/describe runs; when it answers, the generation moves and the runtime
 * re-describes the capability with the stated class. No picture is sent; the
 * state stays INSTALLED until a warm. It runs once per instance.
 */
static void *
bg_describe_main(void *ptr)
{
	struct rc *rc = (struct rc *)ptr;
	char token[U_CREDENTIAL_SECRET_MAX + 1];
	char err[256] = {0};
	bool answered = false;
	if (read_token(rc, token, sizeof(token), err, sizeof(err))) {
		os_mutex_lock(&rc->lock);
		const uint32_t hosts = rc->host_count;
		os_mutex_unlock(&rc->lock);
		for (uint32_t i = 0; i < hosts && !answered; i++) {
			if (describe_host(rc, i, token, NULL, NULL, err, sizeof(err)) == 0) {
				os_mutex_lock(&rc->lock);
				if (!rc->have_describe) {
					rc->cur = rc->hosts[i].d;
					rc->have_describe = true;
					rc->generation++;
				}
				os_mutex_unlock(&rc->lock);
				answered = true;
			}
		}
	}
	u_credential_wipe(token, sizeof(token));
	if (answered) {
		notify(0);
	}
	return NULL;
}

static void
bg_describe_start_locked(struct rc *rc)
{
	if (rc->bg_started || !rc->consent || rc->have_describe || rc->host_count == 0 || !rc->http_ok ||
	    rc->config_error[0] != '\0') {
		return;
	}
	if (os_thread_init(&rc->bg) != 0) {
		return;
	}
	if (os_thread_start(&rc->bg, bg_describe_main, rc) != 0) {
		os_thread_destroy(&rc->bg);
		return;
	}
	rc->bg_started = true;
}


/*
 *
 * Provider iface: describe / state.
 *
 */

static bool
rc_get_provider_status(struct xrt_conv_provider_status *out)
{
	if (out == NULL) {
		return false;
	}
	char why[160];
	const bool ok = rc_http_available(why, sizeof(why));
	SET(out, struct xrt_conv_provider_status, state, ok ? 1u /* READY */ : 2u /* PLATFORM_ABSENT */);
	SET(out, struct xrt_conv_provider_status, flags, 0u);
	if (COVERS(out, struct xrt_conv_provider_status, hint)) {
		copy_str(out->hint, sizeof(out->hint),
		         ok ? "forwards splat jobs to a remote endpoint (contract v1); needs consent and ENDPOINTS"
		            : why);
	}
	return true;
}

static xrt_result_t
rc_probe(struct xrt_conversion_plugin_instance **out_inst)
{
	char buf[16];
	if (out_inst == NULL || !setting("ENABLE", buf, sizeof(buf)) || strtoul(buf, NULL, 10) == 0) {
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}
	struct rc *rc = (struct rc *)calloc(1, sizeof(*rc));
	if (rc == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	if (os_mutex_init(&rc->lock) != 0) {
		free(rc);
		return XRT_ERROR_ALLOCATION;
	}
	load_settings(rc);
	rc->http_ok = rc_http_available(rc->http_why, sizeof(rc->http_why));
	rc->state = XRT_CONV_STATE_INSTALLED;
	rc->generation = 1;
	consent_refresh_locked(rc, true);
	U_LOG_W("remote conversion: provider active (%u endpoint(s), credential %s, consent %s%s%s)", rc->host_count,
	        rc->credential[0] != '\0' ? rc->credential : "none", rc->consent ? "granted" : "NOT granted",
	        rc->http_ok ? "" : "; ", rc->http_ok ? "" : rc->http_why);
	*out_inst = (struct xrt_conversion_plugin_instance *)rc;
	return XRT_SUCCESS;
}

static void
fill_cap(struct rc *rc, uint32_t cap_id, struct xrt_conv_capability *o)
{
#define C(FIELD, VALUE) SET(o, struct xrt_conv_capability, FIELD, VALUE)
#define S(FIELD, VALUE)                                                                                                \
	do {                                                                                                           \
		if (COVERS(o, struct xrt_conv_capability, FIELD)) {                                                    \
			copy_str(o->FIELD, sizeof(o->FIELD), (VALUE));                                                 \
		}                                                                                                      \
	} while (0)
	os_mutex_lock(&rc->lock);
	const bool have = rc->have_describe;
	const struct rc_describe d = rc->cur;
	os_mutex_unlock(&rc->lock);

	const uint32_t task = cap_id == RC_CAP_STEREO ? XRT_CONV_TASK_SPLATS_STEREO : XRT_CONV_TASK_SPLATS_MONO;
	C(cap_id, cap_id);
	C(task, task);
	C(lanes, XRT_CONV_LANE_JOB);
	C(input_kinds, cap_id == RC_CAP_STEREO ? XRT_CONV_INPUT_STEREO_PAIR : XRT_CONV_INPUT_MONO);
	C(outputs, XRT_CONV_OUT_SPLAT_BLOB);
	C(flags, XRT_CONV_CAP_REMOTE | XRT_CONV_CAP_NEEDS_NETWORK_TO_WARM | XRT_CONV_CAP_LANES_CONCURRENT);
	C(import_memory_kinds, XRT_CONV_MEM_CPU);
	C(import_sync_kinds, 1u << XRT_CONV_SYNC_NONE);
	C(input_formats, (1u << XRT_CONV_FMT_RGBA8_UNORM) | (1u << XRT_CONV_FMT_RGBA8_SRGB) |
	                     (1u << XRT_CONV_FMT_BGRA8_UNORM) | (1u << XRT_CONV_FMT_BGRA8_SRGB));
	C(min_views, 0u);
	C(max_views, 0u);
	C(max_streams, 0u);
	C(max_concurrent, 1u);
	C(max_input_edge, have ? d.max_long_side[cap_id] : 0u); // the runtime downscales above it
	C(preferred_input_edge, 0u);
	C(intrinsics_source, XRT_CONV_INTRINSICS_ESTIMATED);
	C(splat_formats, have && d.blob_formats[cap_id] != 0
	                     ? d.blob_formats[cap_id]
	                     : (1u << XRT_CONV_BLOB_SOG) | (1u << XRT_CONV_BLOB_PLY_3DGS));
	// Reported as the endpoint states it; unknown until the first describe.
	C(licence_class, have ? d.licence_class : XRT_CONV_LICENCE_UNKNOWN);
	C(commercial_ok, have && d.licence_class == XRT_CONV_LICENCE_COMMERCIAL ? 1u : 0u);
	S(licence_spdx, have && d.licence_name[0] != '\0' ? d.licence_name : "LicenseRef-remote-endpoint");
	S(model_id, have && d.model_id[0] != '\0' ? d.model_id : "remote");
	S(model_version, have ? d.provider_commit : "");
	S(backend, "remote-https");
	S(display_name, cap_id == RC_CAP_STEREO ? "Remote splats (stereo)" : "Remote splats (mono)");
	C(device_luid, 0ull);
	C(est_warmup_ms, 1000u);
	C(est_latency_ms, 10000u);
	C(est_vram_mb, 0u);
	C(est_ram_mb, 64u);
#undef C
#undef S
}

static xrt_result_t
rc_describe_cap(struct xrt_conversion_plugin_instance *inst,
                uint32_t index,
                struct xrt_conv_capability *out,
                uint32_t *out_count)
{
	struct rc *rc = (struct rc *)inst;
	if (out_count != NULL) {
		*out_count = RC_CAP_COUNT;
	}
	if (out == NULL) {
		return out_count != NULL ? XRT_SUCCESS : XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (rc == NULL || index >= RC_CAP_COUNT) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	fill_cap(rc, index + 1, out);
	return XRT_SUCCESS;
}

static uint64_t
rc_get_generation(struct xrt_conversion_plugin_instance *inst)
{
	struct rc *rc = (struct rc *)inst;
	if (rc == NULL) {
		return 0;
	}
	os_mutex_lock(&rc->lock);
	const uint64_t g = rc->generation;
	os_mutex_unlock(&rc->lock);
	return g;
}

static xrt_result_t
rc_get_state(struct xrt_conversion_plugin_instance *inst, uint32_t cap_id, struct xrt_conv_state *out)
{
	struct rc *rc = (struct rc *)inst;
	if (rc == NULL || out == NULL || cap_id == 0 || cap_id > RC_CAP_COUNT) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	os_mutex_lock(&rc->lock);
	consent_refresh_locked(rc, false);
	const struct rc_eval e = eval_locked(rc, cap_id);
	const float progress = rc->progress;
	os_mutex_unlock(&rc->lock);
	SET(out, struct xrt_conv_state, state, e.state);
	SET(out, struct xrt_conv_state, failure_reason,
	    e.state == XRT_CONV_STATE_FAILED || e.state == XRT_CONV_STATE_ABSENT ? e.reason : 0u);
	SET(out, struct xrt_conv_state, retryable, e.state == XRT_CONV_STATE_FAILED && e.retryable ? 1u : 0u);
	SET(out, struct xrt_conv_state, progress, e.state == XRT_CONV_STATE_WARMING ? progress : -1.0f);
	if (COVERS(out, struct xrt_conv_state, version)) {
		copy_str(out->version, sizeof(out->version), RC_BUILD);
	}
	if (COVERS(out, struct xrt_conv_state, detail)) {
		copy_str(out->detail, sizeof(out->detail), e.detail);
	}
	return XRT_SUCCESS;
}


/*
 *
 * warm / evict.
 *
 */

static xrt_result_t
rc_warm(struct xrt_conversion_plugin_instance *inst, uint32_t cap_id, bool (*is_cancelled)(void *), void *ctx)
{
	struct rc *rc = (struct rc *)inst;
	if (rc == NULL || cap_id == 0 || cap_id > RC_CAP_COUNT) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (is_cancelled == NULL) {
		is_cancelled = never_cancelled;
	}
	os_mutex_lock(&rc->lock);
	consent_refresh_locked(rc, true);
	const struct rc_eval pre = eval_locked(rc, cap_id);
	if (pre.state == XRT_CONV_STATE_DISABLED || pre.state == XRT_CONV_STATE_ABSENT) {
		os_mutex_unlock(&rc->lock);
		return pre.state == XRT_CONV_STATE_DISABLED ? XRT_ERROR_NOT_AUTHORIZED
		                                            : XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (rc->state == XRT_CONV_STATE_READY) {
		os_mutex_unlock(&rc->lock);
		return XRT_SUCCESS;
	}
	set_state_locked(rc, XRT_CONV_STATE_WARMING, 0, false, "asking the endpoint list (/v1/describe)");
	rc->progress = 0.0f;
	const uint32_t hosts = rc->host_count;
	os_mutex_unlock(&rc->lock);
	notify(0);

	char token[U_CREDENTIAL_SECRET_MAX + 1];
	char err[256] = {0};
	if (!read_token(rc, token, sizeof(token), err, sizeof(err))) {
		U_LOG_W("remote conversion: %s", err);
		os_mutex_lock(&rc->lock);
		set_state_locked(rc, XRT_CONV_STATE_FAILED, XRT_CONV_FAIL_LICENCE, true, err);
		os_mutex_unlock(&rc->lock);
		notify(0);
		return XRT_ERROR_NOT_AUTHORIZED;
	}
	int answered = -1, worst = 1;
	for (uint32_t i = 0; i < hosts && answered < 0 && !is_cancelled(ctx); i++) {
		const int o = describe_host(rc, i, token, is_cancelled, ctx, err, sizeof(err));
		if (o == 0) {
			answered = (int)i;
		} else {
			U_LOG_W("remote conversion: %s", err);
			worst = o > worst ? o : worst;
		}
		os_mutex_lock(&rc->lock);
		rc->progress = (float)(i + 1) / (float)hosts;
		os_mutex_unlock(&rc->lock);
	}
	u_credential_wipe(token, sizeof(token));

	os_mutex_lock(&rc->lock);
	if (answered >= 0) {
		rc->cur = rc->hosts[answered].d;
		rc->have_describe = true;
		rc->generation++; // licence class, limits and formats are now known
		char d[XRT_CONV_DETAIL_MAX];
		(void)snprintf(d, sizeof(d), "endpoint %s (%s)", rc->cur.endpoint_id, rc->hosts[answered].base);
		set_state_locked(rc, XRT_CONV_STATE_READY, 0, false, d);
	} else if (is_cancelled(ctx)) {
		set_state_locked(rc, XRT_CONV_STATE_INSTALLED, 0, false, "warm cancelled");
	} else {
		const uint32_t reason = worst == 2   ? XRT_CONV_FAIL_LICENCE
		                        : worst == 3 ? XRT_CONV_FAIL_VERSION
		                                     : XRT_CONV_FAIL_NETWORK;
		set_state_locked(rc, XRT_CONV_STATE_FAILED, reason, true, err);
	}
	const bool ready = rc->state == XRT_CONV_STATE_READY;
	os_mutex_unlock(&rc->lock);
	notify(0);
	return ready ? XRT_SUCCESS : XRT_ERROR_OUTPUT_REQUEST_FAILURE;
}

static xrt_result_t
rc_evict(struct xrt_conversion_plugin_instance *inst, uint32_t cap_id)
{
	struct rc *rc = (struct rc *)inst;
	if (rc == NULL || cap_id == 0 || cap_id > RC_CAP_COUNT) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	os_mutex_lock(&rc->lock);
	if (rc->state == XRT_CONV_STATE_READY) {
		set_state_locked(rc, XRT_CONV_STATE_EVICTED, 0, false, "idle");
	}
	os_mutex_unlock(&rc->lock);
	return XRT_SUCCESS;
}


/*
 *
 * Job lane.
 *
 */

//! A growable byte buffer.
struct rc_buf
{
	uint8_t *p;
	size_t n, cap;
	bool failed;
};

static void
buf_put(struct rc_buf *b, const void *data, size_t n)
{
	if (b->failed) {
		return;
	}
	if (b->n + n > b->cap) {
		size_t nc = b->cap != 0 ? b->cap : 256u * 1024u;
		while (nc < b->n + n) {
			nc *= 2;
		}
		uint8_t *np = (uint8_t *)realloc(b->p, nc);
		if (np == NULL) {
			b->failed = true;
			return;
		}
		b->p = np;
		b->cap = nc;
	}
	memcpy(b->p + b->n, data, n);
	b->n += n;
}

static void
buf_puts(struct rc_buf *b, const char *s)
{
	buf_put(b, s, strlen(s));
}

static void
png_write_cb(void *ctx, void *data, int size)
{
	buf_put((struct rc_buf *)ctx, data, (size_t)size);
}

static bool
is_bgra(uint32_t f)
{
	return f == XRT_CONV_FMT_BGRA8_UNORM || f == XRT_CONV_FMT_BGRA8_SRGB;
}

static bool
is_color4(uint32_t f)
{
	return f == XRT_CONV_FMT_RGBA8_UNORM || f == XRT_CONV_FMT_RGBA8_SRGB || is_bgra(f);
}

/*!
 * Encode columns [x0, x0 + w) of a CPU RGBA8 / BGRA8 image as an RGB PNG
 * into @p out. False for anything else (the runtime gives a provider CPU
 * pixels when it lists only XRT_CONV_MEM_CPU).
 */
static bool
encode_png(const struct xrt_conv_image *img, uint32_t x0, uint32_t w, struct rc_buf *out)
{
	if (img->memory_kind != XRT_CONV_MEM_CPU || img->cpu_ptr == NULL || !is_color4(img->format) || w == 0 ||
	    img->height == 0 || x0 + w > img->width) {
		return false;
	}
	const uint32_t stride = img->row_stride != 0 ? img->row_stride : img->width * 4u;
	uint8_t *rgb = (uint8_t *)malloc((size_t)w * img->height * 3u);
	if (rgb == NULL) {
		return false;
	}
	const bool bgra = is_bgra(img->format);
	for (uint32_t y = 0; y < img->height; y++) {
		const uint8_t *src = (const uint8_t *)img->cpu_ptr + (size_t)y * stride + (size_t)x0 * 4u;
		uint8_t *dst = rgb + (size_t)y * w * 3u;
		for (uint32_t x = 0; x < w; x++) {
			dst[3 * x + 0] = bgra ? src[4 * x + 2] : src[4 * x + 0];
			dst[3 * x + 1] = src[4 * x + 1];
			dst[3 * x + 2] = bgra ? src[4 * x + 0] : src[4 * x + 2];
		}
	}
	stbi_write_png_compression_level = 4; // speed over a few % of upload
	const int ok = stbi_write_png_to_func(png_write_cb, out, (int)w, (int)img->height, 3, rgb, (int)w * 3);
	free(rgb);
	return ok != 0 && !out->failed;
}

static void
part_header(struct rc_buf *b, const char *boundary, const char *name, const char *filename, const char *type)
{
	char h[256];
	if (filename != NULL) {
		(void)snprintf(h, sizeof(h),
		               "--%s\r\nContent-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\nContent-Type: "
		               "%s\r\n\r\n",
		               boundary, name, filename, type);
	} else {
		(void)snprintf(h, sizeof(h),
		               "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\nContent-Type: %s\r\n\r\n",
		               boundary, name, type);
	}
	buf_puts(b, h);
}

struct rc_job
{
	const struct xrt_conv_job_control *ctl;
	bool (*is_cancelled)(void *);
	void (*progress)(void *, float, const char *);
	void *ctx;
};

static void
job_progress(const struct rc_job *j, float f, const char *stage)
{
	if (j->progress != NULL) {
		j->progress(j->ctx, f, stage);
	}
}

static void
http_progress(void *ud, float up, float down)
{
	const struct rc_job *j = (const struct rc_job *)ud;
	// 0.1 .. 0.45 uploading, 0.45 .. 0.95 waiting / receiving.
	const float f = down > 0.0f ? 0.45f + 0.5f * down : 0.1f + 0.35f * up;
	job_progress(j, f, down > 0.0f ? "receiving" : "uploading");
}

static bool
sink_emit(const struct xrt_conv_job_control *ctl, uint32_t format, const void *bytes, uint64_t size)
{
	if (ctl == NULL || !COVERS(ctl, struct xrt_conv_job_control, sinks) || ctl->sinks == NULL) {
		return false;
	}
	for (uint32_t i = 0; i < ctl->sink_count; i++) {
		struct xrt_conv_blob_sink *k = &ctl->sinks[i];
		if (!COVERS(k, struct xrt_conv_blob_sink, write) || k->role != XRT_CONV_OUT_SPLAT_BLOB ||
		    k->begin == NULL || k->write == NULL) {
			continue;
		}
		if (!k->begin(k->ctx, format, size)) {
			return false;
		}
		const uint8_t *p = (const uint8_t *)bytes;
		for (uint64_t off = 0; off < size; off += RC_SINK_CHUNK) {
			const uint64_t n = size - off < RC_SINK_CHUNK ? size - off : RC_SINK_CHUNK;
			if (!k->write(k->ctx, p + off, n)) {
				return false;
			}
		}
		return true;
	}
	return false;
}

//! The facts of X-DXR-Result into the job's out-structs (within their struct_size).
static void
apply_result(
    const cJSON *res, const struct rc_describe *d, const char *base, uint32_t blob_format, struct xrt_conv_job_out *out)
{
	struct xrt_conv_splat_info si;
	memset(&si, 0, sizeof(si));
	si.blob_format = blob_format;
	const char *coords = jstr(res, "coords");
	si.coords = coords != NULL && strcmp(coords, "rub") == 0 ? XRT_CONV_COORDS_RUB : XRT_CONV_COORDS_OPENCV;
	si.color_space = XRT_CONV_CS_SRGB;
	const cJSON *in = cJSON_GetObjectItemCaseSensitive(res, "intrinsics");
	si.fx = jfloat(in, "fx", 0.0f);
	si.fy = jfloat(in, "fy", 0.0f);
	si.cx = jfloat(in, "cx", 0.0f);
	si.cy = jfloat(in, "cy", 0.0f);
	si.grid_width = (uint32_t)jcount(in, "width", 65536u, 0u);
	si.grid_height = (uint32_t)jcount(in, "height", 65536u, 0u);
	const char *isrc = jstr(in, "source");
	si.intrinsics_source = si.fx <= 0.0f                                  ? XRT_CONV_INTRINSICS_NONE
	                       : isrc != NULL && strcmp(isrc, "given") == 0   ? XRT_CONV_INTRINSICS_GIVEN
	                       : isrc != NULL && strcmp(isrc, "assumed") == 0 ? XRT_CONV_INTRINSICS_ASSUMED
	                                                                      : XRT_CONV_INTRINSICS_ESTIMATED;
	const cJSON *metric = cJSON_GetObjectItemCaseSensitive(res, "metric");
	const float baseline = jfloat(res, "baseline_m", 0.0f);
	// Metric only with a real baseline: never claimed on a guess.
	si.metric = cJSON_IsTrue(metric) && baseline > 0.0f ? 1u : 0u;
	si.baseline_m = si.metric != 0 ? baseline : 0.0f;
	si.near_m = jfloat(res, "near_m", 0.0f);
	si.far_m = jfloat(res, "far_m", 0.0f);
	si.splat_count = jcount(res, "splat_count", 1ull << 40, 0u);
	const cJSON *fs = cJSON_GetObjectItemCaseSensitive(res, "focus_suggestion");
	const cJSON *pt = cJSON_GetObjectItemCaseSensitive(fs, "point");
	if (cJSON_IsArray(pt) && cJSON_GetArraySize(pt) == 3) {
		si.focus_z_m =
		    (float)cJSON_GetArrayItem(pt, 2)->valuedouble; // ADR-053 field: the SUGGESTED focus depth
	}
	if (COVERS(out, struct xrt_conv_job_out, splat_info)) {
		out->splat_info = si;
	}

	if (!COVERS(out, struct xrt_conv_job_out, splat_result) || out->splat_result == NULL) {
		return;
	}
	struct xrt_conv_splat_result *r = out->splat_result;
	struct xrt_conv_splat_result f;
	memset(&f, 0, sizeof(f));
	f.struct_size = r->struct_size;
	f.valid = XRT_CONV_SPLAT_VALID_BUILD;
	// The stamp of the build that ANSWERED: the result's own, else the host's describe.
	struct rc_describe b = *d;
	const cJSON *build = cJSON_GetObjectItemCaseSensitive(res, "build");
	if (cJSON_IsObject(build)) {
		describe_parse_build(build, &b);
	}
	copy_str(f.build_provider, sizeof(f.build_provider), b.provider_commit);
	copy_str(f.build_model_id, sizeof(f.build_model_id), b.model_id);
	copy_str(f.build_model_sha256, sizeof(f.build_model_sha256), b.model_sha256);
	copy_str(f.build_runtime, sizeof(f.build_runtime), b.runtime);
	copy_str(f.build_runtime_version, sizeof(f.build_runtime_version), b.runtime_version);
	char hid[XRT_CONV_STAMP_MAX];
	(void)snprintf(hid, sizeof(hid), "%s@%s", d->endpoint_id[0] != '\0' ? d->endpoint_id : base,
	               b.host_id[0] != '\0' ? b.host_id : "?");
	copy_str(f.build_host_id, sizeof(f.build_host_id), hid);
	f.host_kind = XRT_CONV_HOST_KIND_REMOTE;

	const cJSON *ref = cJSON_GetObjectItemCaseSensitive(res, "refusal");
	f.refusal_code = refusal_parse(jstr(ref, "code"));
	copy_str(f.refusal_reason, sizeof(f.refusal_reason), jstr(ref, "reason"));

	const cJSON *mf = cJSON_GetObjectItemCaseSensitive(res, "metric_fit");
	if (cJSON_IsObject(mf)) {
		f.valid |= XRT_CONV_SPLAT_VALID_METRIC_FIT;
		f.metric_scale = jfloat(mf, "scale", 0.0f);
		const char *src = jstr(mf, "source");
		f.metric_source = src == NULL ? XRT_CONV_METRIC_SOURCE_NONE
		                  : strstr(src, "stereo") != NULL || strstr(src, "fs") != NULL
		                      ? XRT_CONV_METRIC_SOURCE_STEREO
		                  : strcmp(src, "model") == 0 ? XRT_CONV_METRIC_SOURCE_MODEL
		                                              : XRT_CONV_METRIC_SOURCE_OTHER;
		f.metric_inlier_fraction = jfloat(mf, "inlier_fraction", jfloat(mf, "inliers", -1.0f));
		f.metric_mad_log = jfloat(mf, "mad_log", -1.0f);
		f.principal_point_shift_px =
		    jfloat(mf, "principal_point_shift_px", jfloat(mf, "infinity_shift_px", 0.0f));
	}
	const cJSON *layers = cJSON_GetObjectItemCaseSensitive(res, "layers");
	if (cJSON_IsArray(layers)) {
		f.valid |= XRT_CONV_SPLAT_VALID_LAYERS;
		const cJSON *l = NULL;
		cJSON_ArrayForEach(l, layers)
		{
			if (f.layer_count >= XRT_CONV_MAX_SPLAT_LAYERS) {
				break;
			}
			uint32_t fl = 0;
			fl |=
			    cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(l, "visible")) ? XRT_CONV_LAYER_VISIBLE : 0u;
			fl |= cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(l, "inpainted")) ? XRT_CONV_LAYER_INPAINTED
			                                                                     : 0u;
			f.layer_flags[f.layer_count++] = fl;
		}
	} else if (cJSON_IsNumber(layers) && layers->valuedouble > 0) {
		f.valid |= XRT_CONV_SPLAT_VALID_LAYERS;
		f.layer_count = (uint32_t)jcount_item(layers, XRT_CONV_MAX_SPLAT_LAYERS);
		f.layer_count = f.layer_count > XRT_CONV_MAX_SPLAT_LAYERS ? XRT_CONV_MAX_SPLAT_LAYERS : f.layer_count;
	}
	if (cJSON_IsArray(pt) && cJSON_GetArraySize(pt) == 3) {
		f.valid |= XRT_CONV_SPLAT_VALID_FOCUS;
		for (int k = 0; k < 3; k++) {
			f.focus_point[k] = (float)cJSON_GetArrayItem(pt, k)->valuedouble;
		}
		f.focus_subject_m = jfloat(fs, "subject_m", 0.0f);
		f.focus_near_m = jfloat(fs, "near_m", 0.0f);
		f.focus_far_m = jfloat(fs, "far_m", 0.0f);
		const char *src = jstr(fs, "source");
		f.focus_source = src == NULL                          ? XRT_CONV_FOCUS_SOURCE_OTHER
		                 : strcmp(src, "nearest-clump") == 0  ? XRT_CONV_FOCUS_SOURCE_NEAREST_CLUMP
		                 : strstr(src, "convergence") != NULL ? XRT_CONV_FOCUS_SOURCE_CONVERGENCE
		                                                      : XRT_CONV_FOCUS_SOURCE_OTHER;
	}
	// Within the runtime's struct_size only.
	const size_t n = r->struct_size < sizeof(f) ? r->struct_size : sizeof(f);
	memcpy(r, &f, n);
}

//! Stamp the result with a host's describe when there is no X-DXR-Result (a refusal before any answer).
static void
stamp_from_describe(const struct rc_describe *d, const char *base, struct xrt_conv_job_out *out)
{
	cJSON *empty = cJSON_CreateObject();
	struct xrt_conv_splat_info keep = out->splat_info;
	apply_result(empty, d, base, 0, out);
	out->splat_info = keep;
	cJSON_Delete(empty);
}

static xrt_result_t
rc_job_execute(struct xrt_conversion_plugin_instance *inst,
               uint32_t cap_id,
               const struct xrt_conv_job_in *in,
               const struct xrt_conv_params *params,
               const struct xrt_conv_job_control *ctl,
               struct xrt_conv_job_out *out)
{
	struct rc *rc = (struct rc *)inst;
	if (rc == NULL || in == NULL || ctl == NULL || out == NULL || cap_id == 0 || cap_id > RC_CAP_COUNT ||
	    !COVERS(in, struct xrt_conv_job_in, images)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	struct rc_job job;
	job.ctl = ctl;
	job.is_cancelled = COVERS(ctl, struct xrt_conv_job_control, is_cancelled) && ctl->is_cancelled != NULL
	                       ? ctl->is_cancelled
	                       : never_cancelled;
	job.progress = COVERS(ctl, struct xrt_conv_job_control, progress) ? ctl->progress : NULL;
	job.ctx = COVERS(ctl, struct xrt_conv_job_control, ctx) ? ctl->ctx : NULL;

	// Consent is re-read for every job: a revoke stops the next upload.
	os_mutex_lock(&rc->lock);
	consent_refresh_locked(rc, true);
	const bool consent = rc->consent;
	const bool ready = rc->state == XRT_CONV_STATE_READY;
	os_mutex_unlock(&rc->lock);
	if (!consent) {
		U_LOG_W("remote conversion: job refused: consent required (displayxr-cli conv consent " RC_ID
		        " grant)");
		return XRT_ERROR_NOT_AUTHORIZED;
	}
	if (!ready) {
		const xrt_result_t wr = rc_warm(inst, cap_id, job.is_cancelled, job.ctx);
		if (wr != XRT_SUCCESS) {
			set_refusal(out, XRT_CONV_REFUSAL_REMOTE_ERROR, rc->detail);
			return wr;
		}
	}
	os_mutex_lock(&rc->lock);
	const struct rc_describe d0 = rc->cur;
	os_mutex_unlock(&rc->lock);
	if (!d0.serves[cap_id]) {
		set_refusal(out, XRT_CONV_REFUSAL_UNSUPPORTED_INPUT, "the endpoint does not serve this task");
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}

	/* Inputs: one image (mono), or a pair (two images, or one side-by-side). */
	const uint32_t job_flags = COVERS(in, struct xrt_conv_job_in, job_flags) ? in->job_flags : 0u;
	const uint32_t max_side =
	    COVERS(in, struct xrt_conv_job_in, max_input_long_side) ? in->max_input_long_side : 0u;
	const bool stereo = cap_id == RC_CAP_STEREO;
	const struct xrt_conv_stereo_calibration *cal =
	    params != NULL && COVERS(params, struct xrt_conv_params, stereo) ? &params->stereo : NULL;
	const float baseline = cal != NULL && cal->baseline_m > 0.0f ? cal->baseline_m : 0.0f;
	const float focal =
	    params != NULL && COVERS(params, struct xrt_conv_params, focal_px) ? params->focal_px : 0.0f;
	if (stereo && (job_flags & (XRT_CONV_JOB_PAIR_RECTIFIED | XRT_CONV_JOB_PAIR_RAW)) == 0) {
		set_refusal(out, XRT_CONV_REFUSAL_UNRECTIFIED_PAIR,
		            "the pair is not declared rectified or raw (it is never guessed)");
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	if (stereo && (job_flags & XRT_CONV_JOB_PAIR_RAW) != 0 && d0.rectified_required) {
		set_refusal(out, XRT_CONV_REFUSAL_UNRECTIFIED_PAIR,
		            "the endpoint needs a rectified pair; this one is raw");
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	const struct xrt_conv_image *img0 = in->image_count > 0 ? &in->images[0] : NULL;
	const struct xrt_conv_image *img1 = in->image_count > 1 ? &in->images[1] : NULL;
	const bool sbs = stereo && img1 == NULL && cal != NULL && cal->sbs_packed != 0;
	if (img0 == NULL || (stereo && img1 == NULL && !sbs)) {
		set_refusal(out, XRT_CONV_REFUSAL_UNSUPPORTED_INPUT,
		            stereo ? "a pair needs two images or one side-by-side" : "no input image");
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	const uint32_t long_side_limit = d0.max_long_side[cap_id];
	const uint32_t eye_w = sbs ? img0->width / 2 : img0->width;
	const uint32_t long_side = eye_w > img0->height ? eye_w : img0->height;
	if (long_side_limit != 0 && long_side > long_side_limit) {
		char why[128];
		(void)snprintf(why, sizeof(why), "input long side %u px > the endpoint's %u px (downscale first)",
		               long_side, long_side_limit);
		set_refusal(out, XRT_CONV_REFUSAL_INPUT_TOO_LARGE, why);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	job_progress(&job, 0.02f, "encoding");
	struct rc_buf png[2];
	memset(png, 0, sizeof(png));
	bool enc_ok;
	const bool swap = stereo && cal != NULL && cal->eye_order == 1;
	if (sbs) {
		enc_ok = encode_png(img0, swap ? eye_w : 0, eye_w, &png[0]) &&
		         encode_png(img0, swap ? 0 : eye_w, eye_w, &png[1]);
	} else if (stereo) {
		const struct xrt_conv_image *l = swap ? img1 : img0, *r = swap ? img0 : img1;
		enc_ok = encode_png(l, 0, l->width, &png[0]) && encode_png(r, 0, r->width, &png[1]);
	} else {
		enc_ok = encode_png(img0, 0, img0->width, &png[0]);
	}
	if (!enc_ok) {
		free(png[0].p);
		free(png[1].p);
		set_refusal(out, XRT_CONV_REFUSAL_UNSUPPORTED_INPUT, "input must be CPU RGBA8 / BGRA8");
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	const uint64_t max_bytes = d0.max_bytes[cap_id];
	if (max_bytes != 0 && png[0].n + png[1].n > max_bytes) {
		free(png[0].p);
		free(png[1].p);
		set_refusal(out, XRT_CONV_REFUSAL_INPUT_TOO_LARGE,
		            "the encoded input exceeds the endpoint's max_bytes");
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	/* params (contract v1 §6). */
	const uint32_t requested =
	    COVERS(in, struct xrt_conv_job_in, requested_blob_format) ? in->requested_blob_format : 0u;
	const uint32_t format = requested != 0 ? requested : rc->blob_format;
	if (format != XRT_CONV_BLOB_SOG && format != XRT_CONV_BLOB_PLY_3DGS) {
		free(png[0].p);
		free(png[1].p);
		return XRT_ERROR_OUTPUT_UNSUPPORTED;
	}
	cJSON *pj = cJSON_CreateObject();
	cJSON_AddStringToObject(pj, "task", task_word(cap_id));
	cJSON_AddStringToObject(pj, "format", format_word(format));
	if (baseline > 0.0f) {
		cJSON_AddNumberToObject(pj, "baseline_m", baseline); // omitted = unknown; never defaulted
	}
	if (stereo) {
		cJSON_AddBoolToObject(pj, "rectified", (job_flags & XRT_CONV_JOB_PAIR_RECTIFIED) != 0);
	}
	if ((job_flags & XRT_CONV_JOB_FOCAL_GIVEN) != 0 && focal > 0.0f) {
		cJSON_AddNumberToObject(pj, "focal_px", focal);
	}
	if (max_side != 0) {
		cJSON_AddNumberToObject(pj, "max_long_side", max_side);
	}
	if (rc->options[0] != '\0') {
		cJSON_AddItemToObject(pj, "options", cJSON_Parse(rc->options));
	}
	char *pjs = cJSON_PrintUnformatted(pj);
	cJSON_Delete(pj);

	char boundary[48];
	(void)snprintf(boundary, sizeof(boundary), "dxr-%016llx", (unsigned long long)os_monotonic_get_ns());
	struct rc_buf body;
	memset(&body, 0, sizeof(body));
	part_header(&body, boundary, "params", NULL, "application/json");
	buf_puts(&body, pjs != NULL ? pjs : "{}");
	buf_puts(&body, "\r\n");
	if (stereo) {
		part_header(&body, boundary, "left", "left.png", "image/png");
		buf_put(&body, png[0].p, png[0].n);
		buf_puts(&body, "\r\n");
		part_header(&body, boundary, "right", "right.png", "image/png");
		buf_put(&body, png[1].p, png[1].n);
		buf_puts(&body, "\r\n");
	} else {
		part_header(&body, boundary, "image", "image.png", "image/png");
		buf_put(&body, png[0].p, png[0].n);
		buf_puts(&body, "\r\n");
	}
	char closing[64];
	(void)snprintf(closing, sizeof(closing), "--%s--\r\n", boundary);
	buf_puts(&body, closing);
	free(pjs);
	free(png[0].p);
	free(png[1].p);
	if (body.failed) {
		free(body.p);
		return XRT_ERROR_ALLOCATION;
	}

	char token[U_CREDENTIAL_SECRET_MAX + 1];
	char err[256] = {0};
	if (!read_token(rc, token, sizeof(token), err, sizeof(err))) {
		free(body.p);
		U_LOG_W("remote conversion: %s", err);
		os_mutex_lock(&rc->lock);
		set_state_locked(rc, XRT_CONV_STATE_FAILED, XRT_CONV_FAIL_LICENCE, true, err);
		os_mutex_unlock(&rc->lock);
		notify(0);
		set_refusal(out, XRT_CONV_REFUSAL_REMOTE_ERROR, err);
		return XRT_ERROR_NOT_AUTHORIZED;
	}
	char ctype[96];
	(void)snprintf(ctype, sizeof(ctype), "Content-Type: multipart/form-data; boundary=%s", boundary);

	/* The fallback walk. */
	xrt_result_t result = XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	bool decided = false; // a definitive answer (success, or a non-retryable refusal)
	char last[256] = "no endpoint answered";
	os_mutex_lock(&rc->lock);
	const uint32_t hosts = rc->host_count;
	os_mutex_unlock(&rc->lock);
	for (uint32_t i = 0; i < hosts && !decided; i++) {
		if (job.is_cancelled(job.ctx)) {
			break;
		}
		os_mutex_lock(&rc->lock);
		const bool described = rc->hosts[i].described;
		os_mutex_unlock(&rc->lock);
		if (!described) {
			// An endpoint switch: describe it (build stamp, contract) before using it.
			const int o = describe_host(rc, i, token, job.is_cancelled, job.ctx, err, sizeof(err));
			if (o != 0) {
				copy_str(last, sizeof(last), err);
				U_LOG_W("remote conversion: %s - next host", err);
				continue;
			}
		}
		os_mutex_lock(&rc->lock);
		const struct rc_describe d = rc->hosts[i].d;
		const char *base = rc->hosts[i].base;
		os_mutex_unlock(&rc->lock);

		char url[RC_URL_MAX + 16];
		(void)snprintf(url, sizeof(url), "%s/v1/jobs", base);
		char auth[U_CREDENTIAL_SECRET_MAX + 32];
		const char *hdrs[4];
		headers_with_token(token, auth, sizeof(auth), ctype, hdrs);
		struct rc_http_request rq;
		memset(&rq, 0, sizeof(rq));
		rq.method = "POST";
		rq.url = url;
		rq.headers = hdrs;
		rq.body = body.p;
		rq.body_size = body.n;
		rq.timeout_ms = rc->timeout_s * 1000u;
		// The result is buffered: bound it by the setting, and by what the host
		// itself states (describe's max_result_bytes) when that is smaller.
		rq.max_response_bytes = rc->max_response_bytes;
		if (d.max_result_bytes[cap_id] != 0 && d.max_result_bytes[cap_id] < rq.max_response_bytes) {
			rq.max_response_bytes = d.max_result_bytes[cap_id];
		}
		rq.is_cancelled = job.is_cancelled;
		rq.cancel_ctx = job.ctx;
		rq.progress = http_progress;
		rq.progress_ctx = &job;
		job_progress(&job, 0.1f, "uploading");
		struct rc_http_response resp;
		rc_http_perform(&rq, &resp);
		u_credential_wipe(auth, sizeof(auth));

		if (resp.error == RC_HTTP_CANCELLED) {
			rc_http_response_free(&resp);
			break; // the connection is closed: that IS the cancel (contract v1)
		}
		if (resp.error != RC_HTTP_OK) {
			(void)snprintf(last, sizeof(last), "%s: %s", base, resp.message);
		} else if (resp.status == 200) {
			char hdr[16384];
			cJSON *res = rc_http_header(&resp, "X-DXR-Result", hdr, sizeof(hdr)) ? cJSON_Parse(hdr) : NULL;
			if (res == NULL) {
				U_LOG_W(
				    "remote conversion: %s answered 200 without a readable X-DXR-Result: facts unknown",
				    base);
				res = cJSON_CreateObject();
			}
			const uint32_t got_format = format_parse(jstr(res, "format"));
			const uint32_t bf = got_format != 0 ? got_format : format;
			if (resp.body_size == 0 || !sink_emit(ctl, bf, resp.body, resp.body_size)) {
				(void)snprintf(last, sizeof(last), "%s: empty blob, or the sink refused it", base);
				result = XRT_ERROR_OUTPUT_REQUEST_FAILURE;
				decided = true;
			} else {
				apply_result(res, &d, base, bf, out);
				out->written = XRT_CONV_OUT_SPLAT_BLOB;
				result = XRT_SUCCESS;
				decided = true;
				const bool have_sr =
				    COVERS(out, struct xrt_conv_job_out, splat_result) && out->splat_result != NULL;
				const uint32_t refusal = have_sr ? out->splat_result->refusal_code : 0u;
				U_LOG_W("remote conversion: %s %s -> %llu bytes (%s) from %s%s%s", task_word(cap_id),
				        out->splat_info.metric != 0 ? "metric" : "non-metric",
				        (unsigned long long)resp.body_size, format_word(bf), base,
				        refusal != 0 ? "; refusal: " : "",
				        refusal != 0 ? out->splat_result->refusal_reason : "");
			}
			cJSON_Delete(res);
			job_progress(&job, 1.0f, "done");
		} else if (resp.status == 422 || resp.status == 413 ||
		           (resp.status >= 400 && resp.status < 500 && resp.status != 401 && resp.status != 403 &&
		            resp.status != 404 && resp.status != 408 && resp.status != 429)) {
			// Bad input: never retried (another host would say the same).
			cJSON *e = resp.body != NULL ? cJSON_Parse((const char *)resp.body) : NULL;
			const char *code = jstr(e, "code");
			const char *msg = jstr(e, "message");
			const uint32_t rcode = resp.status == 413 ? XRT_CONV_REFUSAL_INPUT_TOO_LARGE
			                       : resp.status == 422
			                           ? refusal_parse(code != NULL ? code : "unsupported_input")
			                           : XRT_CONV_REFUSAL_UNSUPPORTED_INPUT;
			char why[XRT_CONV_DETAIL_MAX];
			(void)snprintf(why, sizeof(why), "%s", msg != NULL ? msg : "the endpoint refused the input");
			stamp_from_describe(&d, base, out);
			set_refusal(out, rcode, why);
			U_LOG_W("remote conversion: %s refused the input (HTTP %ld %s): %s - not retried", base,
			        resp.status, code != NULL ? code : "", why);
			cJSON_Delete(e);
			result = XRT_ERROR_INPUT_UNSUPPORTED;
			decided = true;
		} else {
			// 5xx, 429, 408, 401, 403, 404: the next host.
			(void)snprintf(last, sizeof(last), "%s: HTTP %ld", base, resp.status);
		}
		if (!decided) {
			U_LOG_W("remote conversion: %s - next host", last);
		}
		rc_http_response_free(&resp);
	}
	u_credential_wipe(token, sizeof(token));
	free(body.p);

	if (job.is_cancelled(job.ctx) && !decided) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE; // the core reports CANCELLED
	}
	if (!decided) {
		char why[XRT_CONV_DETAIL_MAX];
		(void)snprintf(why, sizeof(why), "every endpoint failed; last: %s", last);
		set_refusal(out, XRT_CONV_REFUSAL_REMOTE_ERROR, why);
		os_mutex_lock(&rc->lock);
		// The whole list is down: FAILED(NETWORK, retryable), so selection falls
		// through and the runtime re-warms on its backoff.
		set_state_locked(rc, XRT_CONV_STATE_FAILED, XRT_CONV_FAIL_NETWORK, true, why);
		os_mutex_unlock(&rc->lock);
		notify(0);
		U_LOG_W("remote conversion: %s", why);
	}
	return result;
}


/*
 *
 * Stream lane: not served (job-only capabilities).
 *
 */

static xrt_result_t
rc_stream_create(struct xrt_conversion_plugin_instance *inst,
                 uint32_t cap_id,
                 const struct xrt_conv_stream_info *info,
                 uint64_t *out_stream)
{
	(void)inst;
	(void)cap_id;
	(void)info;
	(void)out_stream;
	return XRT_ERROR_FEATURE_NOT_SUPPORTED;
}

static xrt_result_t
rc_stream_get_output_desc(struct xrt_conversion_plugin_instance *inst,
                          uint64_t stream,
                          uint32_t input_width,
                          uint32_t input_height,
                          struct xrt_conv_output_desc *out)
{
	(void)inst;
	(void)stream;
	(void)input_width;
	(void)input_height;
	(void)out;
	return XRT_ERROR_FEATURE_NOT_SUPPORTED;
}

static xrt_result_t
rc_stream_convert(struct xrt_conversion_plugin_instance *inst,
                  uint64_t stream,
                  const struct xrt_conv_frame_in *in,
                  const struct xrt_conv_params *params,
                  struct xrt_conv_frame_out *out)
{
	(void)inst;
	(void)stream;
	(void)in;
	(void)params;
	(void)out;
	return XRT_ERROR_FEATURE_NOT_SUPPORTED;
}

static void
rc_stream_destroy(struct xrt_conversion_plugin_instance *inst, uint64_t stream)
{
	(void)inst;
	(void)stream;
}

static void
rc_destroy(struct xrt_conversion_plugin_instance *inst)
{
	struct rc *rc = (struct rc *)inst;
	if (rc == NULL) {
		return;
	}
	os_mutex_lock(&rc->lock);
	const bool bg = rc->bg_started;
	os_mutex_unlock(&rc->lock);
	if (bg) {
		os_thread_join(&rc->bg); // bounded by the describe timeout
		os_thread_destroy(&rc->bg);
	}
	os_mutex_destroy(&rc->lock);
	free(rc);
}


/*
 *
 * Settings + negotiate.
 *
 */

static const struct xrt_component_setting_desc g_rc_settings[] = {
    {"ENABLE", XRT_COMPONENT_SETTING_TYPE_BOOL, "0", "probe() declines unless 1 (off by default)"},
    {"ENDPOINTS", XRT_COMPONENT_SETTING_TYPE_STRING, "",
     "base URLs implementing endpoint contract v1, in fallback order (comma separated); https, or http on loopback"},
    {"CREDENTIAL", XRT_COMPONENT_SETTING_TYPE_STRING, "",
     "name of the bearer token in the OS credential store (Credential Manager / Keychain / libsecret); never the "
     "token itself"},
    {"TIMEOUT_S", XRT_COMPONENT_SETTING_TYPE_INT, "120",
     "per-request timeout; a timeout falls through to the next host"},
    {"FORMAT", XRT_COMPONENT_SETTING_TYPE_ENUM, "sog", "sog | ply: the blob asked for when the job names none"},
    {"MAX_RESPONSE_MB", XRT_COMPONENT_SETTING_TYPE_INT, "512",
     "the largest result blob accepted (1..4096 MB); a larger answer fails that host"},
    {"OPTIONS", XRT_COMPONENT_SETTING_TYPE_STRING, "",
     "a JSON object passed through as params.options (model-specific switches)"},
};

static uint32_t
rc_describe_settings(const struct xrt_component_setting_desc **out_descs)
{
	if (out_descs != NULL) {
		*out_descs = g_rc_settings;
	}
	return (uint32_t)(sizeof(g_rc_settings) / sizeof(g_rc_settings[0]));
}

static struct xrt_conversion_plugin_iface g_iface = {
    .struct_size = (uint32_t)sizeof(struct xrt_conversion_plugin_iface),
    .id = RC_ID,
    .display_name = "DisplayXR Remote Conversion",
    .vendor = "DisplayXR",
    .version = "1",
    .get_provider_status = rc_get_provider_status,
    .probe = rc_probe,
    .describe_cap = rc_describe_cap,
    .get_generation = rc_get_generation,
    .get_state = rc_get_state,
    .warm = rc_warm,
    .evict = rc_evict,
    .stream_create = rc_stream_create,
    .stream_get_output_desc = rc_stream_get_output_desc,
    .stream_convert = rc_stream_convert,
    .stream_destroy = rc_stream_destroy,
    .job_execute = rc_job_execute,
    .destroy = rc_destroy,
    .describe_settings = rc_describe_settings,
};

/*!
 * The plug-in's only export. Tests link this TU statically (the
 * remote_conversion_static object library) and call it directly.
 */
XRT_CONVERSION_PLUGIN_EXPORT xrt_result_t
xrtConversionPluginNegotiate(uint32_t runtime_api_version,
                             const struct xrt_conversion_plugin_host_iface *host,
                             struct xrt_conversion_plugin_iface **out_iface,
                             uint32_t *out_plugin_api_version)
{
	if (out_iface == NULL || out_plugin_api_version == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	*out_plugin_api_version = XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT;
	if (runtime_api_version != XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT) {
		*out_iface = NULL;
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	g_host = host;
	*out_iface = &g_iface;
	return XRT_SUCCESS;
}
