// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Stereo camera consent policy (see u_camera_consent.h). Pure: every
 *         fact comes through the injected vtables.
 * @ingroup aux_util
 */

#include "util/u_camera_consent.h"
#include "util/u_logging.h"
#include "util/u_sha256.h"

#include "os/os_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#include <windows.h>
#endif

static bool
exe_known(const char *exe)
{
	return exe != NULL && exe[0] != '\0';
}

static void
random_bytes(uint8_t *out, size_t n)
{
	// A per-process fallback key only needs to be unpredictable, not
	// cryptographic-grade: mix the clock, the address space and libc rand.
	uint64_t seed = (uint64_t)os_monotonic_get_ns() ^ (uint64_t)(uintptr_t)out;
#ifdef XRT_OS_WINDOWS
	seed ^= (uint64_t)GetCurrentProcessId() << 32;
#endif
	srand((unsigned)(seed ^ (seed >> 32)));
	for (size_t i = 0; i < n; i++) {
		seed = seed * 6364136223846793005ull + 1442695040888963407ull;
		out[i] = (uint8_t)((seed >> 56) ^ (uint8_t)rand());
	}
}

void
u_camera_consent_init(struct u_camera_consent *c,
                      const struct u_camera_consent_store_ops *store,
                      void *store_ctx,
                      const struct u_camera_consent_env_ops *env,
                      void *env_ctx)
{
	memset(c, 0, sizeof(*c));
	c->store = store;
	c->store_ctx = store_ctx;
	c->env = env;
	c->env_ctx = env_ctx;
	c->prompt_enabled = true;
	c->prompt_timeout_ms = 60000;
}

const char *
u_camera_consent_why_str(enum u_camera_consent_why why)
{
	switch (why) {
	case U_CAMERA_CONSENT_WHY_KILL_SWITCH: return "kill switch (DXR_STEREO_CAMERA=0)";
	case U_CAMERA_CONSENT_WHY_SHARING_OFF: return "camera sharing is off (user toggle)";
	case U_CAMERA_CONSENT_WHY_DEV_OVERRIDE: return "DXR_STEREO_CAMERA_DEV_ALLOW=1 (dev override)";
	case U_CAMERA_CONSENT_WHY_NO_IDENTITY: return "the peer executable could not be verified";
	case U_CAMERA_CONSENT_WHY_DELEGATING: return "registered consent-delegating client";
	case U_CAMERA_CONSENT_WHY_OS_DENIED: return "the OS camera privacy setting denies this app";
	case U_CAMERA_CONSENT_WHY_STORED_ALLOW: return "stored decision: Allow";
	case U_CAMERA_CONSENT_WHY_STORED_DENY: return "stored decision: Deny";
	case U_CAMERA_CONSENT_WHY_ALLOW_ONCE: return "Allow once (this process)";
	case U_CAMERA_CONSENT_WHY_PROMPT_ALLOW: return "prompt: Allow (stored)";
	case U_CAMERA_CONSENT_WHY_PROMPT_ALLOW_ONCE: return "prompt: Allow once";
	case U_CAMERA_CONSENT_WHY_PROMPT_DENY: return "prompt: Deny (stored)";
	case U_CAMERA_CONSENT_WHY_PROMPT_UNAVAILABLE: return "no consent prompt available (headless / prompt off)";
	case U_CAMERA_CONSENT_WHY_PROMPT_TIMEOUT: return "the consent prompt was not answered";
	default: return "?";
	}
}

const char *
u_camera_consent_delegation_skip_str(enum u_camera_consent_delegation_skip skip)
{
	switch (skip) {
	case U_CAMERA_CONSENT_DELEGATION_APPLIES: return "";
	case U_CAMERA_CONSENT_DELEGATION_DECLINED: return "the client declined delegation (DECLINE_DELEGATION)";
	case U_CAMERA_CONSENT_DELEGATION_NO_SIGNER:
		return "user-writable path and the delegating entry records no signer";
	case U_CAMERA_CONSENT_DELEGATION_NOT_SIGNED:
		return "user-writable path and the executable has no valid code signature";
	case U_CAMERA_CONSENT_DELEGATION_SIGNER_MISMATCH:
		return "user-writable path and the executable is signed by someone other than the entry's signer";
	default: return "?";
	}
}

static bool
get_delegation(const struct u_camera_consent_store_ops *st,
               void *sc,
               const char *exe,
               struct u_camera_consent_delegation *out)
{
	memset(out, 0, sizeof(*out));
	if (st == NULL || st->get_delegation == NULL || !st->get_delegation(sc, exe, out)) {
		memset(out, 0, sizeof(*out));
		return false;
	}
	out->signer[sizeof(out->signer) - 1] = '\0';
	return out->scope != U_CAMERA_CONSENT_DELEGATION_NONE;
}

bool
u_camera_consent_is_registered_delegating(const struct u_camera_consent_store_ops *store,
                                          void *store_ctx,
                                          const char *exe)
{
	struct u_camera_consent_delegation d;
	return exe_known(exe) && get_delegation(store, store_ctx, exe, &d);
}

//! A small "seen this executable in this run" set; the oldest entry is overwritten.
static bool
seen_check_and_add(char (*set)[U_CAMERA_CONSENT_EXE_MAX], uint32_t *count, const char *exe)
{
	uint32_t n = *count < U_CAMERA_CONSENT_SEEN_MAX ? *count : U_CAMERA_CONSENT_SEEN_MAX;
	for (uint32_t i = 0; i < n; i++) {
		if (strncmp(set[i], exe, U_CAMERA_CONSENT_EXE_MAX - 1) == 0) {
			return true;
		}
	}
	snprintf(set[*count % U_CAMERA_CONSENT_SEEN_MAX], U_CAMERA_CONSENT_EXE_MAX, "%s", exe);
	(*count)++;
	return false;
}

/*!
 * Step 6's trust check for a registered entry (spec §7.1.1). Admin-protected
 * path: the path is the identity. User-writable path: the entry must name a
 * signer and the executable must be validly signed by exactly that signer.
 */
static enum u_camera_consent_delegation_skip
delegation_trust(struct u_camera_consent *c, const char *exe, const struct u_camera_consent_delegation *d)
{
	const struct u_camera_consent_env_ops *env = c->env;
	// NULL = assume user-writable: fail closed.
	bool user_writable = env == NULL || env->path_user_writable == NULL || env->path_user_writable(c->env_ctx, exe);
	if (!user_writable) {
		return U_CAMERA_CONSENT_DELEGATION_APPLIES;
	}
	if (d->signer[0] == '\0') {
		return U_CAMERA_CONSENT_DELEGATION_NO_SIGNER;
	}
	char actual[U_CAMERA_CONSENT_SIGNER_MAX] = {0};
	if (env == NULL || env->exe_signer == NULL || !env->exe_signer(c->env_ctx, exe, actual, sizeof(actual))) {
		return U_CAMERA_CONSENT_DELEGATION_NOT_SIGNED;
	}
	actual[sizeof(actual) - 1] = '\0';
	if (!u_camera_consent_signer_equal(actual, d->signer)) {
		return U_CAMERA_CONSENT_DELEGATION_SIGNER_MISMATCH;
	}
	return U_CAMERA_CONSENT_DELEGATION_APPLIES;
}

bool
u_camera_consent_sharing_enabled(struct u_camera_consent *c)
{
	if (c->kill_switch) {
		return false;
	}
	if (c->store != NULL && c->store->sharing_enabled != NULL && !c->store->sharing_enabled(c->store_ctx)) {
		return false;
	}
	return true;
}

static bool
once_find(const struct u_camera_consent *c, const char *exe, long pid)
{
	for (uint32_t i = 0; i < c->once_count; i++) {
		if (c->once[i].pid == pid && strcmp(c->once[i].exe, exe) == 0) {
			return true;
		}
	}
	return false;
}

static void
once_add(struct u_camera_consent *c, const char *exe, long pid)
{
	if (once_find(c, exe, pid)) {
		return;
	}
	uint32_t i = c->once_count < U_CAMERA_CONSENT_ONCE_MAX ? c->once_count++ : (U_CAMERA_CONSENT_ONCE_MAX - 1);
	snprintf(c->once[i].exe, sizeof(c->once[i].exe), "%s", exe);
	c->once[i].pid = pid;
}

void
u_camera_consent_forget_once(struct u_camera_consent *c)
{
	c->once_count = 0;
}

static void
decide(struct u_camera_consent_decision *out, enum u_camera_consent_verdict v, enum u_camera_consent_why why)
{
	out->verdict = v;
	out->why = why;
}

void
u_camera_consent_evaluate(struct u_camera_consent *c,
                          const char *exe,
                          const char *app_name,
                          long pid,
                          uint32_t flags,
                          struct u_camera_consent_decision *out)
{
	memset(out, 0, sizeof(*out));
	const struct u_camera_consent_store_ops *st = c->store;
	void *sc = c->store_ctx;

	// 1. sharing off beats everything, the dev override included: a user who
	//    switched the camera off must not find a dev box ignoring it.
	if (c->kill_switch) {
		decide(out, U_CAMERA_CONSENT_DISABLED, U_CAMERA_CONSENT_WHY_KILL_SWITCH);
		return;
	}
	if (st != NULL && st->sharing_enabled != NULL && !st->sharing_enabled(sc)) {
		decide(out, U_CAMERA_CONSENT_DISABLED, U_CAMERA_CONSENT_WHY_SHARING_OFF);
		return;
	}
	// 2. dev override — one WARN per service lifetime, never silent.
	if (c->dev_override) {
		if (!c->dev_override_logged) {
			c->dev_override_logged = true;
			U_LOG_W("stereo camera consent: DXR_STEREO_CAMERA_DEV_ALLOW=1 — every consumer is ALLOWED without "
			        "consent (development override; never set this on a user's machine)");
		}
		decide(out, U_CAMERA_CONSENT_ALLOWED, U_CAMERA_CONSENT_WHY_DEV_OVERRIDE);
		return;
	}
	// 3. no identity, no consent: nothing to store a decision against.
	if (!exe_known(exe)) {
		decide(out, U_CAMERA_CONSENT_REFUSED, U_CAMERA_CONSENT_WHY_NO_IDENTITY);
		return;
	}
	// A fact about the executable, reported whatever the verdict (the service
	// keys the RAW refusal on it). It only DECIDES at step 6.
	struct u_camera_consent_delegation reg;
	out->delegating = get_delegation(st, sc, exe, &reg);
	out->delegation_scope = reg.scope;
	// 4. the OS switch, for every client, delegating ones included: the service
	//    opens the camera, not the client, so the OS never sees this consumer
	//    and nobody else will enforce the user's switch for it.
	if (c->env != NULL && c->env->os_camera_allowed != NULL && !c->env->os_camera_allowed(c->env_ctx, exe)) {
		decide(out, U_CAMERA_CONSENT_OS_DENIED, U_CAMERA_CONSENT_WHY_OS_DENIED);
		return;
	}
	// 5. a stored Deny is the user's explicit "no" for this executable and
	//    beats any registration (an installer's included).
	enum u_camera_consent_stored stored = U_CAMERA_CONSENT_STORED_NONE;
	if (st == NULL || st->get == NULL || !st->get(sc, exe, &stored)) {
		stored = U_CAMERA_CONSENT_STORED_NONE;
	}
	if (stored == U_CAMERA_CONSENT_STORED_DENY) {
		decide(out, U_CAMERA_CONSENT_REFUSED, U_CAMERA_CONSENT_WHY_STORED_DENY);
		return;
	}
	// 6. a registered delegating client prompts per origin itself: no runtime
	//    prompt and no stored decision needed — and nothing more than that.
	//    Not when the client declined it for this instance (it runs with its
	//    own prompt bypassed), and not when the entry cannot be trusted (a
	//    user-writable path without a matching signature, §7.1.1): then the
	//    executable is an ordinary app from here on.
	if (out->delegating) {
		enum u_camera_consent_delegation_skip skip = (flags & U_CAMERA_CONSENT_FLAG_DECLINE_DELEGATION) != 0
		                                                 ? U_CAMERA_CONSENT_DELEGATION_DECLINED
		                                                 : delegation_trust(c, exe, &reg);
		out->delegation_skip = skip;
		if (skip == U_CAMERA_CONSENT_DELEGATION_APPLIES) {
			if (reg.scope == U_CAMERA_CONSENT_DELEGATION_USER &&
			    !seen_check_and_add(c->user_noticed, &c->user_noticed_count, exe)) {
				U_LOG_W("stereo camera consent: %s allowed by a USER-level delegation entry (not an "
				        "installer's); `displayxr-cli camera untrust` removes it",
				        exe);
				if (c->env != NULL && c->env->user_delegation_notice != NULL) {
					c->env->user_delegation_notice(c->env_ctx, exe, app_name);
				}
			}
			out->delegated = true;
			decide(out, U_CAMERA_CONSENT_ALLOWED, U_CAMERA_CONSENT_WHY_DELEGATING);
			return;
		}
		if (skip != U_CAMERA_CONSENT_DELEGATION_DECLINED &&
		    !seen_check_and_add(c->untrusted_warned, &c->untrusted_warned_count, exe)) {
			U_LOG_W(
			    "stereo camera consent: the %s delegating entry for %s is NOT applied — %s%s%s; it is "
			    "treated as an ordinary app (stored decision / prompt)",
			    reg.scope == U_CAMERA_CONSENT_DELEGATION_SYSTEM ? "machine-level" : "user-level", exe,
			    u_camera_consent_delegation_skip_str(skip), reg.signer[0] ? ", entry signer: " : "",
			    reg.signer);
		}
	}
	// 7. a stored Allow.
	if (stored == U_CAMERA_CONSENT_STORED_ALLOW) {
		decide(out, U_CAMERA_CONSENT_ALLOWED, U_CAMERA_CONSENT_WHY_STORED_ALLOW);
		return;
	}
	// 8. "Allow once" already given to this process.
	if (once_find(c, exe, pid)) {
		decide(out, U_CAMERA_CONSENT_ALLOWED, U_CAMERA_CONSENT_WHY_ALLOW_ONCE);
		return;
	}
	// 9. ask.
	if (!c->prompt_enabled || c->env == NULL || c->env->prompt == NULL) {
		decide(out, U_CAMERA_CONSENT_REFUSED, U_CAMERA_CONSENT_WHY_PROMPT_UNAVAILABLE);
		return;
	}
	enum u_camera_consent_prompt_answer a = c->env->prompt(c->env_ctx, exe, app_name, pid, c->prompt_timeout_ms);
	switch (a) {
	case U_CAMERA_CONSENT_PROMPT_ALLOW:
		if (st != NULL && st->set != NULL && !st->set(sc, exe, U_CAMERA_CONSENT_STORED_ALLOW)) {
			U_LOG_W("stereo camera consent: could not persist Allow for %s — it will be asked again", exe);
			once_add(c, exe, pid);
		}
		decide(out, U_CAMERA_CONSENT_ALLOWED, U_CAMERA_CONSENT_WHY_PROMPT_ALLOW);
		return;
	case U_CAMERA_CONSENT_PROMPT_ALLOW_ONCE:
		once_add(c, exe, pid);
		decide(out, U_CAMERA_CONSENT_ALLOWED, U_CAMERA_CONSENT_WHY_PROMPT_ALLOW_ONCE);
		return;
	case U_CAMERA_CONSENT_PROMPT_DENY:
		if (st != NULL && st->set != NULL && !st->set(sc, exe, U_CAMERA_CONSENT_STORED_DENY)) {
			U_LOG_W("stereo camera consent: could not persist Deny for %s", exe);
		}
		decide(out, U_CAMERA_CONSENT_REFUSED, U_CAMERA_CONSENT_WHY_PROMPT_DENY);
		return;
	case U_CAMERA_CONSENT_PROMPT_TIMEOUT:
		decide(out, U_CAMERA_CONSENT_REFUSED, U_CAMERA_CONSENT_WHY_PROMPT_TIMEOUT);
		return;
	case U_CAMERA_CONSENT_PROMPT_UNAVAILABLE:
	default: decide(out, U_CAMERA_CONSENT_REFUSED, U_CAMERA_CONSENT_WHY_PROMPT_UNAVAILABLE); return;
	}
}


/*
 *
 * Pure store helpers.
 *
 */

static char
path_norm_ch(char c, bool windows_rules)
{
	if (windows_rules) {
		if (c == '/') {
			return '\\';
		}
		if (c >= 'A' && c <= 'Z') {
			return (char)(c - 'A' + 'a');
		}
	}
	return c;
}

bool
u_camera_consent_path_equal(const char *a, const char *b, bool windows_rules)
{
	if (a == NULL || b == NULL || a[0] == '\0' || b[0] == '\0') {
		return false;
	}
	while (*a != '\0' && *b != '\0') {
		if (path_norm_ch(*a, windows_rules) != path_norm_ch(*b, windows_rules)) {
			return false;
		}
		a++;
		b++;
	}
	return *a == '\0' && *b == '\0';
}

bool
u_camera_consent_path_is_under(const char *path, const char *dir, bool windows_rules)
{
	if (path == NULL || dir == NULL || path[0] == '\0' || dir[0] == '\0') {
		return false;
	}
	// Any ".." component could climb back out of @p dir.
	for (const char *p = path; *p != '\0'; p++) {
		bool at_start = p == path || p[-1] == '/' || (windows_rules && p[-1] == '\\');
		if (at_start && p[0] == '.' && p[1] == '.' &&
		    (p[2] == '\0' || p[2] == '/' || (windows_rules && p[2] == '\\'))) {
			return false;
		}
	}
	size_t dlen = strlen(dir);
	// Drop trailing separators from dir.
	while (dlen > 0 && (dir[dlen - 1] == '/' || (windows_rules && dir[dlen - 1] == '\\'))) {
		dlen--;
	}
	if (dlen == 0) {
		return false;
	}
	for (size_t i = 0; i < dlen; i++) {
		if (path[i] == '\0' || path_norm_ch(path[i], windows_rules) != path_norm_ch(dir[i], windows_rules)) {
			return false;
		}
	}
	// A separator boundary, and something after it: "C:\Program FilesX\a" is not under "C:\Program Files".
	char sep = path_norm_ch(path[dlen], windows_rules);
	return (sep == '/' || (windows_rules && sep == '\\')) && path[dlen + 1] != '\0';
}

static char
ascii_lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool
u_camera_consent_signer_equal(const char *a, const char *b)
{
	if (a == NULL || b == NULL) {
		return false;
	}
	while (*a == ' ') {
		a++;
	}
	while (*b == ' ') {
		b++;
	}
	size_t la = strlen(a), lb = strlen(b);
	while (la > 0 && a[la - 1] == ' ') {
		la--;
	}
	while (lb > 0 && b[lb - 1] == ' ') {
		lb--;
	}
	if (la == 0 || la != lb) {
		return false;
	}
	for (size_t i = 0; i < la; i++) {
		if (ascii_lower(a[i]) != ascii_lower(b[i])) {
			return false;
		}
	}
	return true;
}

bool
u_camera_consent_list_find(
    u_camera_consent_enum_fn fn, void *ctx, const char *exe, bool windows_rules, char *match_name, size_t match_cap)
{
	if (fn == NULL || !exe_known(exe)) {
		return false;
	}
	char name[1024];
	char path[4096];
	for (uint32_t i = 0; i < U_CAMERA_CONSENT_LIST_MAX; i++) {
		name[0] = '\0';
		path[0] = '\0';
		enum u_camera_consent_enum_result r = fn(ctx, i, name, sizeof(name), path, sizeof(path));
		if (r == U_CAMERA_CONSENT_ENUM_END) {
			return false;
		}
		if (r != U_CAMERA_CONSENT_ENUM_ENTRY) {
			continue; // one bad entry never hides the ones after it
		}
		name[sizeof(name) - 1] = '\0';
		path[sizeof(path) - 1] = '\0';
		if (u_camera_consent_path_equal(path, exe, windows_rules)) {
			if (match_name != NULL && match_cap > 0) {
				snprintf(match_name, match_cap, "%s", name);
			}
			return true;
		}
	}
	return false;
}


/*
 *
 * Keyed persistentId.
 *
 */

void
u_camera_consent_persistent_id_keyed(
    const uint8_t *key, size_t key_len, const char *device_identity, const char *consumer_exe, char out[64])
{
	// Length-prefixed fields: "ab" + "c" never collides with "a" + "bc".
	uint8_t msg[2 * 1024 + 64];
	size_t n = 0;
	const char *fields[2] = {device_identity != NULL ? device_identity : "",
	                         consumer_exe != NULL ? consumer_exe : ""};
	for (int i = 0; i < 2; i++) {
		size_t len = strlen(fields[i]);
		if (len > 1024) {
			len = 1024;
		}
		msg[n++] = (uint8_t)(len >> 8);
		msg[n++] = (uint8_t)len;
		memcpy(msg + n, fields[i], len);
		n += len;
	}
	uint8_t mac[32];
	u_hmac_sha256(key, key_len, msg, n, mac);
	// 128 bits is plenty for an opaque id and keeps it within the 64-char field.
	int w = snprintf(out, 64, "dxrcam-");
	for (int i = 0; i < 16; i++) {
		w += snprintf(out + w, 64 - (size_t)w, "%02x", mac[i]);
	}
}

void
u_camera_consent_persistent_id(struct u_camera_consent *c,
                               const char *device_identity,
                               const char *consumer_exe,
                               char out[64])
{
	if (!c->secret_ready) {
		bool ok = c->store != NULL && c->store->get_secret != NULL && c->store->get_secret(c->store_ctx, c->secret);
		if (!ok) {
			random_bytes(c->secret, sizeof(c->secret));
			U_LOG_W("stereo camera consent: no persistent secret in the store — persistentId is keyed per "
			        "service run (stable until restart)");
		}
		c->secret_ready = true;
	}
	u_camera_consent_persistent_id_keyed(c->secret, sizeof(c->secret), device_identity, consumer_exe, out);
}


/*
 *
 * The prompt hand-off (#1842).
 *
 */

uint64_t
u_camera_consent_handoff_begin(struct u_camera_consent_handoff *h)
{
	h->generation++;
	if (h->generation == 0) {
		h->generation = 1; // 0 means "no request"
	}
	h->state = U_CAMERA_CONSENT_HANDOFF_PENDING;
	h->answer = U_CAMERA_CONSENT_PROMPT_TIMEOUT;
	return h->generation;
}

enum u_camera_consent_handoff_result
u_camera_consent_handoff_answer(struct u_camera_consent_handoff *h,
                                uint64_t generation,
                                enum u_camera_consent_prompt_answer answer)
{
	enum u_camera_consent_handoff_result r = U_CAMERA_CONSENT_HANDOFF_ACCEPTED;
	if (generation == 0 || generation != h->generation) {
		r = U_CAMERA_CONSENT_HANDOFF_DROPPED_STALE;
	} else if (h->state == U_CAMERA_CONSENT_HANDOFF_TIMED_OUT) {
		r = U_CAMERA_CONSENT_HANDOFF_DROPPED_LATE;
	} else if (h->state != U_CAMERA_CONSENT_HANDOFF_PENDING) {
		r = U_CAMERA_CONSENT_HANDOFF_DROPPED_DOUBLE;
	}
	if (r != U_CAMERA_CONSENT_HANDOFF_ACCEPTED) {
		h->dropped++;
		return r;
	}
	h->state = U_CAMERA_CONSENT_HANDOFF_ANSWERED;
	h->answer = answer;
	return r;
}

enum u_camera_consent_prompt_answer
u_camera_consent_handoff_finish(struct u_camera_consent_handoff *h, uint64_t generation)
{
	if (generation == 0 || generation != h->generation) {
		// Not the current request (cannot happen with one requester at a
		// time): whatever became of it, it was not answered for us.
		return U_CAMERA_CONSENT_PROMPT_TIMEOUT;
	}
	if (h->state == U_CAMERA_CONSENT_HANDOFF_ANSWERED) {
		return h->answer;
	}
	h->state = U_CAMERA_CONSENT_HANDOFF_TIMED_OUT;
	return U_CAMERA_CONSENT_PROMPT_TIMEOUT;
}

bool
u_camera_consent_handoff_is_pending(const struct u_camera_consent_handoff *h, uint64_t generation)
{
	return generation != 0 && generation == h->generation && h->state == U_CAMERA_CONSENT_HANDOFF_PENDING;
}

const char *
u_camera_consent_handoff_result_str(enum u_camera_consent_handoff_result r)
{
	switch (r) {
	case U_CAMERA_CONSENT_HANDOFF_ACCEPTED: return "accepted";
	case U_CAMERA_CONSENT_HANDOFF_DROPPED_LATE: return "the request had already timed out";
	case U_CAMERA_CONSENT_HANDOFF_DROPPED_DOUBLE: return "the request was already answered";
	case U_CAMERA_CONSENT_HANDOFF_DROPPED_STALE: return "the dialog belongs to an older request";
	default: return "?";
	}
}
