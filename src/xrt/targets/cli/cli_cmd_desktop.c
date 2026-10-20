// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `displayxr-cli desktop` — the user's controls over XR_DXR_desktop_control
 *         (ADR-056 D3, spec §6.3).
 *
 *   desktop status [--json]          kill switch, idle_ms, per-app decisions, paths
 *   desktop stop | resume            the kill switch (every session sees it on its next call)
 *   desktop allow|deny|forget <exe> [--scope control|capture|all]
 *                                    per-app decision (allow is meaningful on Windows,
 *                                    where DisplayXR's own prompt is the consent). Spec
 *                                    v2: one answer per scope; default `control` for
 *                                    allow, `all` for deny / forget
 *   desktop audit [--tail N] [--json]  the last N audit records (default 20)
 *
 * Reads and writes the store and the audit log ONLY (u_desktop_policy) — it
 * never creates a desktop backend and never touches the desktop.
 */

#include "cli_common.h"

#include "xrt/xrt_config_os.h"

#include "desktop/u_desktop.h"
#include "desktop/u_desktop_policy.h"
#include "os/os_time.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#include <windows.h>
#include <shellapi.h> // CommandLineToArgvW
#endif

static void
usage(void)
{
	printf(
	    "usage: displayxr-cli desktop status [--json]\n"
	    "       desktop stop | resume              kill switch for every app's desktop control\n"
	    "       desktop allow|deny|forget <exe> [--scope control|capture|all]\n"
	    "                                          per-app decision (full executable path); default\n"
	    "                                          scope: control for allow, all for deny / forget\n"
	    "       desktop audit [--tail N] [--json]  last N audit records (default 20)\n");
}

//! "<exe>" as the full path the runtime will see for that process (UTF-8).
static bool
resolve_exe_arg(int argc, const char **argv, int idx, char *out, size_t cap)
{
	if (idx >= argc || argv[idx][0] == '\0') {
		return false;
	}
#if defined(XRT_OS_WINDOWS)
	// argv is in the ANSI code page; the store is keyed by UTF-8.
	int wargc = 0;
	wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
	bool ok = false;
	if (wargv != NULL && wargc == argc) {
		wchar_t full[2048];
		DWORD n = GetFullPathNameW(wargv[idx], (DWORD)(sizeof(full) / sizeof(full[0])), full, NULL);
		const wchar_t *src = (n > 0 && n < sizeof(full) / sizeof(full[0])) ? full : wargv[idx];
		ok = WideCharToMultiByte(CP_UTF8, 0, src, -1, out, (int)cap, NULL, NULL) > 1;
	}
	if (wargv != NULL) {
		LocalFree(wargv);
	}
	if (!ok) {
		snprintf(out, cap, "%s", argv[idx]);
	}
#else
	// The runtime reads its own executable resolved (realpath / /proc/self/exe).
	char *resolved = realpath(argv[idx], NULL);
	snprintf(out, cap, "%s", resolved != NULL ? resolved : argv[idx]);
	free(resolved);
#endif
	return out[0] != '\0';
}

static const char *
decision_str(enum u_desktop_app_decision d)
{
	switch (d) {
	case U_DESKTOP_APP_ALLOW: return "allow";
	case U_DESKTOP_APP_DENY: return "deny";
	default: return "?";
	}
}

static const char *
decision_or_dash(enum u_desktop_app_decision d)
{
	return d == U_DESKTOP_APP_NONE ? "-" : decision_str(d);
}

static void
print_app(void *ctx, const char *exe, enum u_desktop_app_decision control, enum u_desktop_app_decision capture)
{
	(void)ctx;
	printf("  control %-5s capture %-5s %s\n", decision_or_dash(control), decision_or_dash(capture), exe);
}

static void
json_app(void *ctx, const char *exe, enum u_desktop_app_decision control, enum u_desktop_app_decision capture)
{
	cJSON *e = cJSON_AddObjectToObject((cJSON *)ctx, exe);
	if (e == NULL) {
		return;
	}
	if (control != U_DESKTOP_APP_NONE) {
		cJSON_AddStringToObject(e, "control", decision_str(control));
	}
	if (capture != U_DESKTOP_APP_NONE) {
		cJSON_AddStringToObject(e, "capture", decision_str(capture));
	}
}

static int
cmd_status(bool json)
{
	char store[1024] = {0}, audit[1024] = {0};
	u_desktop_policy_store_path(store, sizeof(store));
	u_desktop_policy_audit_path(audit, sizeof(audit));
	struct u_desktop_policy *p = u_desktop_policy_create();
	if (p == NULL) {
		fprintf(stderr, "desktop: out of memory\n");
		return 1;
	}
	const char *env = getenv("DXR_DESKTOP_CONTROL");
	bool env_off = env != NULL && strcmp(env, "0") == 0;

	if (json) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddBoolToObject(o, "suspended", u_desktop_policy_is_suspended(p));
		cJSON_AddBoolToObject(o, "store_unreadable", u_desktop_policy_is_unreadable(p));
		cJSON_AddBoolToObject(o, "dir_override",
		                      getenv(U_DESKTOP_POLICY_DIR_ENV) != NULL &&
		                          getenv(U_DESKTOP_POLICY_DIR_ENV)[0] != '\0');
		cJSON_AddNumberToObject(o, "idle_ms", u_desktop_policy_idle_ms(p));
		cJSON *apps = cJSON_AddObjectToObject(o, "apps");
		u_desktop_policy_for_each_app_scopes(p, json_app, apps);
		cJSON_AddStringToObject(o, "store", store);
		cJSON_AddStringToObject(o, "audit", audit);
		cJSON_AddBoolToObject(o, "env_disabled", env_off);
		char *s = cJSON_Print(o);
		printf("%s\n", s != NULL ? s : "{}");
		cJSON_free(s);
		cJSON_Delete(o);
	} else {
		printf("desktop control (XR_DXR_desktop_control, ADR-056)\n");
		if (u_desktop_policy_is_unreadable(p)) {
			printf(
			    "  STORE UNREADABLE: %s cannot be parsed - every app is suspended until it is fixed or\n"
			    "                    removed ('desktop stop' moves it aside and writes a suspended "
			    "store)\n",
			    store);
		}
		const char *ov = getenv(U_DESKTOP_POLICY_DIR_ENV);
		if (ov != NULL && ov[0] != '\0') {
			printf("  override:     %s=%s (not the user's store)\n", U_DESKTOP_POLICY_DIR_ENV, ov);
		}
		printf("  kill switch:  %s\n", u_desktop_policy_is_suspended(p)
		                                   ? "ON - every app is refused (`displayxr-cli desktop resume`)"
		                                   : "off");
		printf("  idle wait:    %u ms\n", u_desktop_policy_idle_ms(p));
		if (env_off) {
			printf("  environment:  DXR_DESKTOP_CONTROL=0 (this shell's apps report UNSUPPORTED)\n");
		}
		printf("  store:        %s\n", store);
		printf("  audit log:    %s\n", audit);
		printf("  apps:\n");
		u_desktop_policy_for_each_app_scopes(p, print_app, NULL);
	}
	u_desktop_policy_destroy(&p);
	return 0;
}

static int
cmd_suspend(bool suspended)
{
	// The kill switch must land: a store held open by another process can fail the
	// rename (Windows), so `stop` retries a few times before giving up loudly.
	bool ok = false;
	for (int attempt = 0; attempt < (suspended ? 3 : 1) && !ok; attempt++) {
		if (attempt > 0) {
			os_nanosleep(250 * 1000 * 1000);
		}
		ok = u_desktop_policy_set_suspended(suspended, "cli");
	}
	if (!ok) {
		char store[1024] = {0};
		u_desktop_policy_store_path(store, sizeof(store));
		if (suspended) {
			fprintf(stderr,
			        "desktop: could not save the kill switch - is the store locked? (%s)\n"
			        "desktop: desktop control is NOT stopped\n",
			        store);
		} else {
			fprintf(stderr, "desktop: could not write the store %s (if it cannot be parsed, fix or remove it)\n",
			        store);
		}
		return 1;
	}
	printf(suspended ? "desktop control stopped for every app (kill switch on)\n"
	                 : "desktop control resumed (kill switch off)\n");
	return 0;
}

static int
cmd_app(const char *op, int argc, const char **argv)
{
	char exe[2048];
	if (!resolve_exe_arg(argc, argv, 3, exe, sizeof(exe))) {
		usage();
		return 2;
	}
	enum u_desktop_app_decision d = strcmp(op, "allow") == 0  ? U_DESKTOP_APP_ALLOW
	                                : strcmp(op, "deny") == 0 ? U_DESKTOP_APP_DENY
	                                                          : U_DESKTOP_APP_NONE;
	// Spec v2 §10.6: allow defaults to CONTROL (granting pixels must be asked for by
	// name); deny / forget default to every scope.
	uint32_t scopes =
	    d == U_DESKTOP_APP_ALLOW ? U_DESKTOP_SCOPE_CONTROL : U_DESKTOP_SCOPE_CONTROL | U_DESKTOP_SCOPE_CAPTURE;
	for (int i = 4; i < argc; i++) {
		if (strcmp(argv[i], "--scope") != 0) {
			continue;
		}
		const char *v = i + 1 < argc ? argv[i + 1] : "";
		if (strcmp(v, "control") == 0) {
			scopes = U_DESKTOP_SCOPE_CONTROL;
		} else if (strcmp(v, "capture") == 0) {
			scopes = U_DESKTOP_SCOPE_CAPTURE;
		} else if (strcmp(v, "all") == 0) {
			scopes = U_DESKTOP_SCOPE_CONTROL | U_DESKTOP_SCOPE_CAPTURE;
		} else {
			usage();
			return 2;
		}
	}
	if (!u_desktop_policy_set_app_scope(exe, scopes, d, "cli")) {
		char store[1024] = {0};
		u_desktop_policy_store_path(store, sizeof(store));
		fprintf(stderr, "desktop: could not write the store %s (if it cannot be parsed, fix or remove it)\n",
		        store);
		return 1;
	}
	printf("%s (%s): %s\n", op, u_desktop_scope_str(scopes), exe);
	return 0;
}

static void
print_audit_line(void *ctx, const char *line)
{
	bool json = *(bool *)ctx;
	if (json) {
		printf("%s\n", line);
		return;
	}
	cJSON *o = cJSON_Parse(line);
	if (o == NULL) {
		printf("%s\n", line);
		return;
	}
	const cJSON *t = cJSON_GetObjectItemCaseSensitive(o, "t");
	const cJSON *action = cJSON_GetObjectItemCaseSensitive(o, "action");
	const cJSON *exe = cJSON_GetObjectItemCaseSensitive(o, "exe");
	const cJSON *result = cJSON_GetObjectItemCaseSensitive(o, "result");
	const cJSON *target = cJSON_GetObjectItemCaseSensitive(o, "target");
	const cJSON *chars = cJSON_GetObjectItemCaseSensitive(o, "chars");
	const cJSON *keys = cJSON_GetObjectItemCaseSensitive(o, "keys");
	const cJSON *msg = cJSON_GetObjectItemCaseSensitive(o, "message");
	const cJSON *subject = cJSON_GetObjectItemCaseSensitive(o, "subject");
	const cJSON *scope = cJSON_GetObjectItemCaseSensitive(o, "scope");
	const cJSON *count = cJSON_GetObjectItemCaseSensitive(o, "count");
	const cJSON *ow = cJSON_GetObjectItemCaseSensitive(o, "w");
	const cJSON *oh = cJSON_GetObjectItemCaseSensitive(o, "h");

	const char *base = cJSON_IsString(exe) ? exe->valuestring : "?";
	for (const char *p = base; *p != '\0'; p++) {
		if (*p == '/' || *p == '\\') {
			base = p + 1;
		}
	}
	printf("%s  %-14s %-12s %-11s", cJSON_IsString(t) ? t->valuestring : "?", base,
	       cJSON_IsString(action) ? action->valuestring : "?", cJSON_IsString(result) ? result->valuestring : "");
	const cJSON *kind = cJSON_IsObject(target) ? cJSON_GetObjectItemCaseSensitive(target, "kind") : NULL;
	if (cJSON_IsObject(target) && cJSON_IsString(kind) && strcmp(kind->valuestring, "window") != 0) {
		const cJSON *rect = cJSON_GetObjectItemCaseSensitive(target, "rect");
		printf(" -> %s", kind->valuestring);
		if (cJSON_IsArray(rect) && cJSON_GetArraySize(rect) == 4) {
			printf(" (%d,%d %dx%d)", (int)cJSON_GetArrayItem(rect, 0)->valuedouble,
			       (int)cJSON_GetArrayItem(rect, 1)->valuedouble,
			       (int)cJSON_GetArrayItem(rect, 2)->valuedouble,
			       (int)cJSON_GetArrayItem(rect, 3)->valuedouble);
		}
	} else if (cJSON_IsObject(target)) {
		const cJSON *app = cJSON_GetObjectItemCaseSensitive(target, "app");
		const cJSON *title = cJSON_GetObjectItemCaseSensitive(target, "title");
		const cJSON *id = cJSON_GetObjectItemCaseSensitive(target, "id");
		printf(" -> %s \"%s\" (%s)", cJSON_IsString(app) ? app->valuestring : "?",
		       cJSON_IsString(title) ? title->valuestring : "", cJSON_IsString(id) ? id->valuestring : "?");
	}
	if (cJSON_IsString(scope)) {
		printf(" [%s]", scope->valuestring);
	}
	if (cJSON_IsNumber(ow) && cJSON_IsNumber(oh)) {
		printf(" %dx%d", (int)ow->valuedouble, (int)oh->valuedouble);
	}
	if (cJSON_IsNumber(count)) {
		printf(" x%d", (int)count->valuedouble);
	}
	if (cJSON_IsString(subject)) {
		printf(" %s", subject->valuestring);
	}
	if (cJSON_IsNumber(chars)) {
		printf(" chars=%d", (int)chars->valuedouble);
	}
	if (cJSON_IsString(keys)) {
		printf(" keys=%s", keys->valuestring);
	}
	if (cJSON_IsString(msg)) {
		printf("  (%s)", msg->valuestring);
	}
	printf("\n");
	cJSON_Delete(o);
}

static int
cmd_audit(int argc, const char **argv, bool json)
{
	uint32_t n = 20;
	for (int i = 3; i + 1 < argc; i++) {
		if (strcmp(argv[i], "--tail") == 0) {
			long v = strtol(argv[i + 1], NULL, 10);
			if (v <= 0 || v > 100000) {
				usage();
				return 2;
			}
			n = (uint32_t)v;
		}
	}
	if (!u_desktop_audit_tail(n, print_audit_line, &json)) {
		if (!json) {
			printf("(no desktop-control audit log yet)\n");
		}
	}
	return 0;
}

int
cli_cmd_desktop(int argc, const char **argv)
{
	const char *sub = argc >= 3 ? argv[2] : "status";
	bool json = cli_has_flag(argc, argv, "--json");

	if (strcmp(sub, "status") == 0) {
		return cmd_status(json);
	}
	if (strcmp(sub, "stop") == 0) {
		return cmd_suspend(true);
	}
	if (strcmp(sub, "resume") == 0) {
		return cmd_suspend(false);
	}
	if (strcmp(sub, "allow") == 0 || strcmp(sub, "deny") == 0 || strcmp(sub, "forget") == 0) {
		return cmd_app(sub, argc, argv);
	}
	if (strcmp(sub, "audit") == 0) {
		return cmd_audit(argc, argv, json);
	}
	usage();
	return 2;
}
