// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Stereo camera consent on Android: subject, built-in trusted list,
 *         store view (see u_camera_consent_android.h).
 * @ingroup aux_util
 */

#include "util/u_camera_consent_android.h"

//! SHA-256 of the DisplayXR Browser's release signing certificate (public).
#define U_CAMERA_CONSENT_ANDROID_BROWSER_CERT "d52153793c7a403f3cd733032ed43314f26a7748f6d5c0a2b5e12847ff0df5b2"

#include <stdio.h>
#include <string.h>

/*
 * The built-in trusted list. An entry is a PACKAGE and the SHA-256 of the
 * certificate its release APK is signed with (`apksigner verify
 * --print-certs <apk>`, "Signer #1 certificate SHA-256 digest"). Only add a
 * client that shows its own per-origin camera prompt and in-use indicator —
 * delegation means "no runtime prompt" (spec §7.1 step 6).
 */
static const struct u_camera_consent_android_trusted k_builtin[] = {
    {
        // The DisplayXR Browser (displayxr-browser releases, Android arm64
        // APK; digest read from DisplayXR-Browser-1.8.0-android-arm64.apk).
        // It asks per origin (Chromium's getUserMedia prompt) and shows its
        // own tab indicator; the runtime adds the in-use notification.
        .package = "org.chromium.chrome",
        .cert_sha256 = U_CAMERA_CONSENT_ANDROID_BROWSER_CERT,
        .what = "DisplayXR Browser (release signing certificate)",
    },
};

const struct u_camera_consent_android_trusted *
u_camera_consent_android_builtin_trusted(uint32_t *out_count)
{
	if (out_count != NULL) {
		*out_count = (uint32_t)(sizeof(k_builtin) / sizeof(k_builtin[0]));
	}
	return k_builtin;
}

bool
u_camera_consent_android_package_valid(const char *pkg)
{
	if (pkg == NULL || pkg[0] == '\0') {
		return false;
	}
	size_t n = 0;
	for (const char *p = pkg; *p != '\0'; p++, n++) {
		char c = *p;
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
		          c == '.' || c == ':';
		if (!ok || n + 1 >= U_CAMERA_CONSENT_ANDROID_PACKAGE_MAX) {
			return false;
		}
	}
	return true;
}

bool
u_camera_consent_android_digest_normalize(const char *in, char out[U_CAMERA_CONSENT_ANDROID_DIGEST_SIZE])
{
	out[0] = '\0';
	if (in == NULL) {
		return false;
	}
	size_t n = 0;
	for (const char *p = in; *p != '\0'; p++) {
		char c = *p;
		if (c == ':' || c == ' ') {
			continue;
		}
		if (c >= 'A' && c <= 'F') {
			c = (char)(c - 'A' + 'a');
		}
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) || n >= 64) {
			out[0] = '\0';
			return false;
		}
		out[n++] = c;
	}
	if (n != 64) {
		out[0] = '\0';
		return false;
	}
	out[64] = '\0';
	return true;
}

bool
u_camera_consent_android_subject_format(const char *pkg, const char *digest, char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return false;
	}
	out[0] = '\0';
	char d[U_CAMERA_CONSENT_ANDROID_DIGEST_SIZE];
	if (!u_camera_consent_android_package_valid(pkg) || !u_camera_consent_android_digest_normalize(digest, d)) {
		return false;
	}
	int n = snprintf(out, cap, "%s%s@%s", U_CAMERA_CONSENT_ANDROID_PREFIX, pkg, d);
	if (n <= 0 || (size_t)n >= cap) {
		out[0] = '\0';
		return false;
	}
	return true;
}

bool
u_camera_consent_android_subject_parse(const char *subject,
                                       char *pkg,
                                       size_t pkg_cap,
                                       char digest[U_CAMERA_CONSENT_ANDROID_DIGEST_SIZE])
{
	const size_t plen = strlen(U_CAMERA_CONSENT_ANDROID_PREFIX);
	if (subject == NULL || strncmp(subject, U_CAMERA_CONSENT_ANDROID_PREFIX, plen) != 0) {
		return false;
	}
	const char *body = subject + plen;
	const char *at = strrchr(body, '@');
	if (at == NULL || at == body) {
		return false;
	}
	char p[U_CAMERA_CONSENT_ANDROID_PACKAGE_MAX];
	size_t pl = (size_t)(at - body);
	if (pl >= sizeof(p)) {
		return false;
	}
	memcpy(p, body, pl);
	p[pl] = '\0';
	char d[U_CAMERA_CONSENT_ANDROID_DIGEST_SIZE];
	if (!u_camera_consent_android_package_valid(p) || !u_camera_consent_android_digest_normalize(at + 1, d) ||
	    strlen(at + 1) != 64) {
		return false;
	}
	if (pkg != NULL && pkg_cap > 0) {
		snprintf(pkg, pkg_cap, "%s", p);
	}
	if (digest != NULL) {
		memcpy(digest, d, sizeof(d));
	}
	return true;
}

enum u_camera_consent_android_trust
u_camera_consent_android_trust_lookup(const struct u_camera_consent_android_trusted *list,
                                      uint32_t n,
                                      const char *pkg,
                                      const char *digest,
                                      const struct u_camera_consent_android_trusted **hit)
{
	if (hit != NULL) {
		*hit = NULL;
	}
	if (list == NULL || pkg == NULL) {
		return U_CAMERA_CONSENT_ANDROID_NOT_LISTED;
	}
	char d[U_CAMERA_CONSENT_ANDROID_DIGEST_SIZE];
	bool have_d = u_camera_consent_android_digest_normalize(digest, d);
	bool listed = false;
	for (uint32_t i = 0; i < n; i++) {
		if (list[i].package == NULL || strcmp(list[i].package, pkg) != 0) {
			continue;
		}
		listed = true;
		char e[U_CAMERA_CONSENT_ANDROID_DIGEST_SIZE];
		if (have_d && u_camera_consent_android_digest_normalize(list[i].cert_sha256, e) && strcmp(e, d) == 0) {
			if (hit != NULL) {
				*hit = &list[i];
			}
			return U_CAMERA_CONSENT_ANDROID_TRUSTED;
		}
	}
	return listed ? U_CAMERA_CONSENT_ANDROID_PACKAGE_ONLY : U_CAMERA_CONSENT_ANDROID_NOT_LISTED;
}

enum u_camera_consent_android_identity_result
u_camera_consent_android_identity(const struct u_camera_consent_android_trusted *list,
                                  uint32_t n,
                                  const char *pkg,
                                  const char *digest,
                                  char *subject,
                                  size_t cap)
{
	if (subject != NULL && cap > 0) {
		subject[0] = '\0';
	}
	if (!u_camera_consent_android_package_valid(pkg)) {
		return U_CAMERA_CONSENT_ANDROID_ID_NO_PACKAGE;
	}
	char d[U_CAMERA_CONSENT_ANDROID_DIGEST_SIZE];
	if (!u_camera_consent_android_digest_normalize(digest, d)) {
		// A listed package with no readable certificate is no better than a
		// mismatch, and neither is anything else: refuse both.
		return U_CAMERA_CONSENT_ANDROID_ID_NO_CERT;
	}
	if (u_camera_consent_android_trust_lookup(list, n, pkg, d, NULL) == U_CAMERA_CONSENT_ANDROID_PACKAGE_ONLY) {
		return U_CAMERA_CONSENT_ANDROID_ID_CERT_MISMATCH;
	}
	if (!u_camera_consent_android_subject_format(pkg, d, subject, cap)) {
		return U_CAMERA_CONSENT_ANDROID_ID_NO_PACKAGE;
	}
	return U_CAMERA_CONSENT_ANDROID_ID_OK;
}

const char *
u_camera_consent_android_identity_str(enum u_camera_consent_android_identity_result r)
{
	switch (r) {
	case U_CAMERA_CONSENT_ANDROID_ID_OK: return "ok";
	case U_CAMERA_CONSENT_ANDROID_ID_NO_PACKAGE: return "no single valid package for the peer uid";
	case U_CAMERA_CONSENT_ANDROID_ID_NO_CERT: return "no readable signing certificate";
	case U_CAMERA_CONSENT_ANDROID_ID_CERT_MISMATCH:
		return "a trusted package name signed with a different certificate (not the listed build)";
	default: return "?";
	}
}

/*
 *
 * The Android store view.
 *
 */

static bool
sa_get(void *ctx, const char *exe, enum u_camera_consent_stored *out)
{
	struct u_camera_consent_store_android *s = ctx;
	*out = U_CAMERA_CONSENT_STORED_NONE;
	return s->base != NULL && s->base->get != NULL && s->base->get(s->base_ctx, exe, out);
}

static bool
sa_set(void *ctx, const char *exe, enum u_camera_consent_stored value)
{
	struct u_camera_consent_store_android *s = ctx;
	return s->base != NULL && s->base->set != NULL && s->base->set(s->base_ctx, exe, value);
}

static bool
sa_get_delegation(void *ctx, const char *exe, struct u_camera_consent_delegation *out)
{
	struct u_camera_consent_store_android *s = ctx;
	memset(out, 0, sizeof(*out));
	char pkg[U_CAMERA_CONSENT_ANDROID_PACKAGE_MAX];
	char d[U_CAMERA_CONSENT_ANDROID_DIGEST_SIZE];
	if (!u_camera_consent_android_subject_parse(exe, pkg, sizeof(pkg), d)) {
		return false;
	}
	if (u_camera_consent_android_trust_lookup(s->list, s->list_count, pkg, d, NULL) !=
	    U_CAMERA_CONSENT_ANDROID_TRUSTED) {
		return false;
	}
	out->scope = U_CAMERA_CONSENT_DELEGATION_SYSTEM;
	return true;
}

static bool
sa_sharing(void *ctx)
{
	struct u_camera_consent_store_android *s = ctx;
	return s->base == NULL || s->base->sharing_enabled == NULL || s->base->sharing_enabled(s->base_ctx);
}

static bool
sa_set_sharing(void *ctx, bool enabled)
{
	struct u_camera_consent_store_android *s = ctx;
	return s->base != NULL && s->base->set_sharing_enabled != NULL &&
	       s->base->set_sharing_enabled(s->base_ctx, enabled);
}

static bool
sa_secret(void *ctx, uint8_t out[U_CAMERA_CONSENT_SECRET_SIZE])
{
	struct u_camera_consent_store_android *s = ctx;
	return s->base != NULL && s->base->get_secret != NULL && s->base->get_secret(s->base_ctx, out);
}

void
u_camera_consent_store_android_init(struct u_camera_consent_store_android *s,
                                    const struct u_camera_consent_store_ops *base,
                                    void *base_ctx,
                                    const struct u_camera_consent_android_trusted *list,
                                    uint32_t list_count)
{
	memset(s, 0, sizeof(*s));
	s->ops.get = sa_get;
	s->ops.set = sa_set;
	s->ops.get_delegation = sa_get_delegation;
	s->ops.sharing_enabled = sa_sharing;
	s->ops.set_sharing_enabled = sa_set_sharing;
	s->ops.get_secret = sa_secret;
	s->base = base;
	s->base_ctx = base_ctx;
	s->list = list;
	s->list_count = list_count;
}

void
u_camera_consent_subject_label(const char *subject, char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return;
	}
	out[0] = '\0';
	if (subject == NULL || subject[0] == '\0') {
		return;
	}
	char pkg[U_CAMERA_CONSENT_ANDROID_PACKAGE_MAX];
	if (u_camera_consent_android_subject_parse(subject, pkg, sizeof(pkg), NULL)) {
		snprintf(out, cap, "%s", pkg);
		return;
	}
	const char *base = subject;
	for (const char *p = subject; *p != '\0'; p++) {
		if (*p == '\\' || *p == '/') {
			base = p + 1;
		}
	}
	snprintf(out, cap, "%s", base);
}
