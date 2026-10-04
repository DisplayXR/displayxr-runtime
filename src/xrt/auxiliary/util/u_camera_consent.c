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
	// 4. a registered delegating client prompts per origin itself.
	if (st != NULL && st->is_delegating != NULL && st->is_delegating(sc, exe)) {
		out->delegating = true;
		decide(out, U_CAMERA_CONSENT_ALLOWED, U_CAMERA_CONSENT_WHY_DELEGATING);
		return;
	}
	// 5. the OS switch: a camera that ignores it would surprise the user.
	if (c->env != NULL && c->env->os_camera_allowed != NULL && !c->env->os_camera_allowed(c->env_ctx, exe)) {
		decide(out, U_CAMERA_CONSENT_OS_DENIED, U_CAMERA_CONSENT_WHY_OS_DENIED);
		return;
	}
	// 6. the stored decision.
	enum u_camera_consent_stored stored = U_CAMERA_CONSENT_STORED_NONE;
	if (st != NULL && st->get != NULL && st->get(sc, exe, &stored)) {
		if (stored == U_CAMERA_CONSENT_STORED_ALLOW) {
			decide(out, U_CAMERA_CONSENT_ALLOWED, U_CAMERA_CONSENT_WHY_STORED_ALLOW);
			return;
		}
		if (stored == U_CAMERA_CONSENT_STORED_DENY) {
			decide(out, U_CAMERA_CONSENT_REFUSED, U_CAMERA_CONSENT_WHY_STORED_DENY);
			return;
		}
	}
	// 7. "Allow once" already given to this process.
	if (once_find(c, exe, pid)) {
		decide(out, U_CAMERA_CONSENT_ALLOWED, U_CAMERA_CONSENT_WHY_ALLOW_ONCE);
		return;
	}
	// 8. ask.
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
