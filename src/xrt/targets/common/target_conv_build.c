// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Locally built models, the machine half (see target_conv_build.h).
 * @ingroup targets_common
 */

#include "target_conv_build.h"
#include "target_component_loader.h"

#include "get_signers.h"

#include "util/u_logging.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef XRT_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static void
cpy(char *dst, size_t n, const char *src)
{
	if (dst != NULL && n > 0) {
		(void)snprintf(dst, n, "%s", src != NULL ? src : "");
	}
}

//! [A-Za-z0-9._-]: a licence id names a registry value / a file-name part, never a path.
static bool
id_ok_n(const char *s, size_t max)
{
	const size_t n = s != NULL ? strlen(s) : 0;
	if (n == 0 || n >= max || strcmp(s, ".") == 0 || strcmp(s, "..") == 0) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		const char c = s[i];
		if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) {
			return false;
		}
	}
	return true;
}

static bool
id_ok(const char *s)
{
	return id_ok_n(s, 48);
}

static bool
sha_ok(const char *s)
{
	if (s == NULL || strlen(s) != 64) {
		return false;
	}
	for (int i = 0; i < 64; i++) {
		if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) {
			return false;
		}
	}
	return true;
}

bool
target_conv_licence_parse(const char *text, char *sha_out, size_t sha_size, uint64_t *when_out)
{
	if (text == NULL) {
		return false;
	}
	char sha[80] = {0};
	unsigned long long when = 0;
	if (sscanf(text, "%79s %llu", sha, &when) != 2 || !sha_ok(sha)) {
		return false;
	}
	cpy(sha_out, sha_size, sha);
	if (when_out != NULL) {
		*when_out = (uint64_t)when;
	}
	return true;
}

#ifndef XRT_OS_WINDOWS
//! POSIX record path; @p create_dir makes the per-user dir.
static bool
licence_file(const char *licence_id, char *out, size_t cap, bool create_dir)
{
	char research[PATH_MAX];
	if (!id_ok(licence_id) || !target_conv_research_file_path(research, sizeof(research), create_dir)) {
		return false;
	}
	char *slash = strrchr(research, '/');
	if (slash == NULL) {
		return false;
	}
	*slash = '\0';
	const int n = snprintf(out, cap, "%s/%s%s", research, TARGET_CONV_LICENCE_FILE_PREFIX, licence_id);
	return n > 0 && (size_t)n < cap;
}
#endif

bool
target_conv_licence_read(const char *licence_id, char *sha_out, size_t sha_size, uint64_t *when_out)
{
	if (!id_ok(licence_id)) {
		return false;
	}
	char text[160] = {0};
#ifdef XRT_OS_WINDOWS
	DWORD sz = sizeof(text);
	if (RegGetValueA(HKEY_CURRENT_USER, TARGET_CONV_REG_ROOT "\\" TARGET_CONV_LICENCE_SUBKEY, licence_id,
	                 RRF_RT_REG_SZ, NULL, text, &sz) != ERROR_SUCCESS) {
		return false;
	}
#elif defined(XRT_OS_ANDROID)
	(void)text;
	return false;
#else
	char path[PATH_MAX];
	if (!licence_file(licence_id, path, sizeof(path), false)) {
		return false;
	}
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return false;
	}
	const size_t got = fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	text[got] = '\0';
#endif
	return target_conv_licence_parse(text, sha_out, sha_size, when_out);
}

bool
target_conv_licence_write(const char *licence_id, const char *text_sha256)
{
	if (!id_ok(licence_id) || !sha_ok(text_sha256)) {
		return false;
	}
	char text[160];
	(void)snprintf(text, sizeof(text), "%s %llu", text_sha256, (unsigned long long)time(NULL));
#ifdef XRT_OS_WINDOWS
	HKEY k = NULL;
	if (RegCreateKeyExA(HKEY_CURRENT_USER, TARGET_CONV_REG_ROOT "\\" TARGET_CONV_LICENCE_SUBKEY, 0, NULL, 0,
	                    KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) {
		return false;
	}
	const bool ok =
	    RegSetValueExA(k, licence_id, 0, REG_SZ, (const BYTE *)text, (DWORD)strlen(text) + 1) == ERROR_SUCCESS;
	RegCloseKey(k);
	return ok;
#elif defined(XRT_OS_ANDROID)
	return false;
#else
	char path[PATH_MAX], tmp[PATH_MAX + 16];
	if (!licence_file(licence_id, path, sizeof(path), true)) {
		return false;
	}
	(void)snprintf(tmp, sizeof(tmp), "%s.%ld.tmp", path, (long)getpid());
	FILE *f = fopen(tmp, "wb");
	if (f == NULL) {
		return false;
	}
	const bool wrote = fprintf(f, "%s\n", text) > 0;
	if (fclose(f) != 0 || !wrote || rename(tmp, path) != 0) {
		(void)unlink(tmp);
		return false;
	}
	return true;
#endif
}

bool
target_conv_licence_withdraw(const char *licence_id)
{
	if (!id_ok(licence_id)) {
		return false;
	}
#ifdef XRT_OS_WINDOWS
	HKEY k = NULL;
	const LONG o = RegOpenKeyExA(HKEY_CURRENT_USER, TARGET_CONV_REG_ROOT "\\" TARGET_CONV_LICENCE_SUBKEY, 0,
	                             KEY_SET_VALUE, &k);
	if (o == ERROR_FILE_NOT_FOUND) {
		return true;
	}
	if (o != ERROR_SUCCESS) {
		return false;
	}
	const LONG r = RegDeleteValueA(k, licence_id);
	RegCloseKey(k);
	return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
#elif defined(XRT_OS_ANDROID)
	return true;
#else
	char path[PATH_MAX];
	if (!licence_file(licence_id, path, sizeof(path), false)) {
		return true;
	}
	return unlink(path) == 0 || access(path, F_OK) != 0;
#endif
}

bool
target_conv_licence_accepted(void *user, const char *licence_id, const char *text_sha256)
{
	(void)user;
	char sha[80];
	return text_sha256 != NULL && target_conv_licence_read(licence_id, sha, sizeof(sha), NULL) &&
	       strcmp(sha, text_sha256) == 0;
}

bool
target_conv_licence_record(void *user, const char *licence_id, const char *text_sha256)
{
	(void)user;
	return target_conv_licence_write(licence_id, text_sha256);
}


/*
 *
 * The builder.
 *
 */

static bool
dev_tool(const char *tool_id, struct conv_build_tool *out)
{
	char name[128];
	if (!conv_build_dev_env_name(tool_id, name, sizeof(name))) {
		return false;
	}
	const char *path = getenv(name);
	if (path == NULL || path[0] == '\0') {
		return false;
	}
	if (!target_component_dev_override_set() || target_component_process_elevated()) {
		U_LOG_W(
		    "[conv-build] %s is set but ignored: it needs DXR_ALLOW_DEV_PLUGIN_PATHS on a non-elevated "
		    "process",
		    name);
		return false;
	}
	if (!target_component_path_is_absolute(path, false)) {
		U_LOG_W("[conv-build] %s is not an absolute path: ignored", name);
		return false;
	}
	cpy(out->id, sizeof(out->id), tool_id);
	cpy(out->binary, sizeof(out->binary), path);
	cpy(out->display_name, sizeof(out->display_name), tool_id);
	cpy(out->source, sizeof(out->source), name);
	out->version[0] = '\0';
	out->dev_override = true;
	return true;
}

bool
target_conv_build_find_tool(void *user, const char *tool_id, struct conv_build_tool *out)
{
	(void)user;
	memset(out, 0, sizeof(*out));
	if (!id_ok_n(tool_id, 64)) {
		return false;
	}
	if (dev_tool(tool_id, out)) {
		return true;
	}
#ifdef XRT_OS_WINDOWS
	char key[256];
	(void)snprintf(key, sizeof(key), "%s\\%s", TARGET_CONV_BUILDERS_REG_ROOT, tool_id);
	DWORD sz = sizeof(out->version);
	if (RegGetValueA(HKEY_LOCAL_MACHINE, key, "Version", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, NULL, out->version,
	                 &sz) != ERROR_SUCCESS ||
	    out->version[0] == '\0') {
		return false; // absent, or its installer has not finished (Version is written last)
	}
	sz = sizeof(out->binary);
	if (RegGetValueA(HKEY_LOCAL_MACHINE, key, "Binary", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, NULL, out->binary,
	                 &sz) != ERROR_SUCCESS ||
	    !target_component_path_is_absolute(out->binary, true)) {
		U_LOG_W("[conv-build] HKLM\\%s: Binary missing or not absolute", key);
		return false;
	}
	sz = sizeof(out->display_name);
	if (RegGetValueA(HKEY_LOCAL_MACHINE, key, "DisplayName", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, NULL,
	                 out->display_name, &sz) != ERROR_SUCCESS) {
		cpy(out->display_name, sizeof(out->display_name), tool_id);
	}
	cpy(out->id, sizeof(out->id), tool_id);
	(void)snprintf(out->source, sizeof(out->source), "HKLM\\%s", key);
	return true;
#elif defined(XRT_OS_ANDROID)
	return false;
#else
#if defined(XRT_OS_MACOS)
	const char *dir = "/Library/Application Support/DisplayXR/ModelBuilders";
#else
	const char *dir = "/usr/share/displayxr/model-builders";
#endif
	char path[PATH_MAX], real[PATH_MAX], why[256];
	(void)snprintf(path, sizeof(path), "%s/%s.json", dir, tool_id);
	if (realpath(path, real) == NULL) {
		return false;
	}
	if (!target_component_owner_chain_ok(real, 0, NULL, why, sizeof(why))) {
		U_LOG_W("[conv-build] %s ignored: %s (a builder manifest must be root-owned)", path, why);
		return false;
	}
	FILE *f = fopen(real, "rb");
	if (f == NULL) {
		return false;
	}
	char text[4096];
	const size_t got = fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	text[got] = '\0';
	if (!conv_build_tool_manifest_parse(text, tool_id, out)) {
		U_LOG_W("[conv-build] %s is not a builder manifest for '%s'", path, tool_id);
		return false;
	}
	cpy(out->source, sizeof(out->source), path);
	return true;
#endif
}

bool
target_conv_build_trust_rule(
    uint32_t provenance, const char *signer, bool windows, bool found_by_dev_override, char *why, size_t why_size)
{
	cpy(why, why_size, "");
	switch (provenance) {
	case TARGET_COMPONENT_PROVENANCE_ALLOWLISTED: return true;
	case TARGET_COMPONENT_PROVENANCE_SIGNED: {
		static const char *const first_party[] = DXR_FIRST_PARTY_SIGNERS;
		for (size_t i = 0; i < sizeof(first_party) / sizeof(first_party[0]); i++) {
			if (signer != NULL && strcmp(signer, first_party[i]) == 0) {
				return true;
			}
		}
		(void)snprintf(why, why_size, "signed by \"%s\", which is not a DisplayXR signer",
		               signer != NULL ? signer : "");
		return false;
	}
	case TARGET_COMPONENT_PROVENANCE_PLATFORM_TRUSTED:
		if (!windows) {
			return true;
		}
		cpy(why, why_size, "a path is not a provenance on Windows");
		return false;
	case TARGET_COMPONENT_PROVENANCE_DEV_OVERRIDE:
		if (found_by_dev_override) {
			return true;
		}
		cpy(why, why_size,
		    "unverified; the dev override applies only to a builder named by DXR_MODEL_BUILDER_<ID>");
		return false;
	default:
		cpy(why, why_size,
		    windows ? "not signed by a DisplayXR signer and not allowlisted"
		            : "not root-owned under a system path and not allowlisted");
		return false;
	}
}

bool
target_conv_build_tool_trusted(void *user, const struct conv_build_tool *tool, struct conv_build_trust *out)
{
	(void)user;
	memset(out, 0, sizeof(*out));
	struct target_component_entry *e = calloc(1, sizeof(*e));
	if (e == NULL) {
		return false;
	}
	cpy(e->id, sizeof(e->id), tool->id);
	cpy(e->binary_path, sizeof(e->binary_path), tool->binary);
	(void)target_component_check_provenance(NULL, e);
	cpy(out->provenance, sizeof(out->provenance), target_component_provenance_str(e->provenance));
	cpy(out->signer, sizeof(out->signer), e->signer);
#ifdef XRT_OS_WINDOWS
	const bool windows = true;
#else
	const bool windows = false;
#endif
	char why[200];
	const bool ok =
	    target_conv_build_trust_rule(e->provenance, e->signer, windows, tool->dev_override, why, sizeof(why));
	(void)snprintf(out->reason, sizeof(out->reason), "%s%s%s", why,
	               why[0] != '\0' && e->provenance_reason[0] != '\0' ? " -- " : "", ok ? "" : e->provenance_reason);
	if (ok) {
		cpy(out->reason, sizeof(out->reason), e->provenance_reason);
	}
	free(e);
	return ok;
}

void
target_conv_build_ops_fill(struct conv_build_ops *ops, const struct target_conv_policy *policy, bool per_run_research)
{
	memset(ops, 0, sizeof(*ops));
	const bool allow = policy != NULL && policy->allow_research_only;
	// Policy\Lock with HKLM AllowResearchOnly != 1 resolves to off, and nothing a
	// user does lifts it: not HKCU, not the per-run --allow-research-only (§5.6.3 step 1).
	ops->research_locked_off = policy != NULL && policy->policy_lock && !allow;
	ops->allow_research_only = allow || (per_run_research && !ops->research_locked_off);
	ops->licence_accepted = target_conv_licence_accepted;
	ops->licence_record = target_conv_licence_record;
	ops->find_tool = target_conv_build_find_tool;
	ops->tool_trusted = target_conv_build_tool_trusted;
}
