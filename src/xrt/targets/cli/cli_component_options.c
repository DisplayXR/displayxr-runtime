// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The `options | set | unset` verbs shared by every component class
 *         (ADR-054 decision 7, N3c). See the header.
 * @ingroup cli
 */

#include "cli_component_options.h"
#include "cli_common.h"

#include "target_component_loader.h"

#include "xrt/xrt_config_os.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define P(...) printf(__VA_ARGS__)

//! Declared + stored keys a component may show (describe_settings + both stores).
#define CLI_OPTIONS_MAX 64


/*
 *
 * Shared checks + messages.
 *
 */

//! The `enable|disable|use` id rule (cli_input_predict.h): names a registry subkey, not flag-shaped.
static bool
id_valid(const char *id)
{
	if (id == NULL || id[0] == '\0' || id[0] == '-' || strlen(id) >= 64) {
		return false;
	}
	for (const char *c = id; *c != '\0'; c++) {
		if (*c == '\\' || *c == '/') {
			return false;
		}
	}
	return true;
}

//! The id + registration gate every verb runs first. Non-zero = refused, message printed.
static int
gate(const struct cli_component_class *k, const char *id, const char *verb)
{
	if (!id_valid(id)) {
		P("error: '%s' is not a %s id.\n", id != NULL ? id : "", k->noun);
		return 1;
	}
	if (k->registered != NULL && !k->registered(id)) {
		// A per-user Settings row for an unregistered id would be an orphan
		// that `list` can never show: refuse, write nothing (the enable|disable rule).
		P("FAIL: '%s' is not a registered %s (see '%s list') — %s.\n", id, k->noun, k->verb,
		  strcmp(verb, "options") == 0 ? "nothing to show" : "nothing written");
		return 1;
	}
	return 0;
}

static int
write_result(const struct cli_component_class *k, enum target_component_store_result r, const char *what, bool machine)
{
	switch (r) {
	case TARGET_COMPONENT_STORE_OK: return 0;
	case TARGET_COMPONENT_STORE_DENIED:
		P("FAIL: %s was denied — run from an elevated terminal (HKLM needs admin).\n", what);
		return 1;
	case TARGET_COMPONENT_STORE_UNSUPPORTED:
		if (machine) {
			P("FAIL: %s — there is no machine-wide store on this platform (drop --machine", what);
			if (k->posix_machine_hint != NULL) {
				P(", or edit %s", k->posix_machine_hint);
			}
			P(").\n");
		} else {
			P("FAIL: %s — there is no per-user settings store on this platform.\n", what);
		}
		return 1;
	case TARGET_COMPONENT_STORE_FAILED:
	default: P("FAIL: %s failed (bad key or value, or I/O).\n", what); return 1;
	}
}

//! After a write: what the component will actually read, and why, when that is not what was just written.
static void
report_effective(const struct cli_component_class *k, const char *id, const char *key, bool machine)
{
	char value[TARGET_COMPONENT_SETTING_VALUE_MAX];
	uint32_t source = TARGET_COMPONENT_SETTING_ABSENT;
	const bool set = target_component_get_setting(k->cls, id, key, value, sizeof(value), &source);
	char env[TARGET_COMPONENT_SETTING_ENV_MAX] = {0};
	(void)target_component_setting_env_name(k->cls->env_class, id, key, env, sizeof(env));
	if (!set) {
		P("Effective now: %s unset — the %s keeps its compiled default.\n", key, k->noun);
		return;
	}
	P("Effective now: %s = '%s' (source: %s)", key, value, target_component_setting_source_str(source));
	if (source == TARGET_COMPONENT_SETTING_ENVIRONMENT) {
		P(" — %s is set in THIS process's environment and always wins; the hosting process's own "
		  "environment decides there.",
		  env);
	} else if (!machine && source == TARGET_COMPONENT_SETTING_MACHINE) {
		P(" — the machine value wins: HKLM Policy\\Lock is set for this class, so per-user values are "
		  "ignored.");
	} else if (machine && source == TARGET_COMPONENT_SETTING_USER) {
		P(" — a per-user (HKCU) value for this key still wins for this user.");
	}
	P("\n");
}


/*
 *
 * options
 *
 */

struct option_row
{
	struct target_component_setting_desc desc; //!< key + type / default / help ("" when undeclared)
	char value[TARGET_COMPONENT_SETTING_VALUE_MAX];
	uint32_t source;
	bool declared;
};

static bool
ci_equal(const char *a, const char *b)
{
	for (;; a++, b++) {
		int ca = (unsigned char)*a;
		int cb = (unsigned char)*b;
		if (ca >= 'a' && ca <= 'z') {
			ca -= 'a' - 'A';
		}
		if (cb >= 'a' && cb <= 'z') {
			cb -= 'a' - 'A';
		}
		if (ca != cb) {
			return false;
		}
		if (ca == '\0') {
			return true;
		}
	}
}

//! The union of what the component declares and what the stores hold, each resolved through the chain.
static int
collect(const struct cli_component_class *k, const char *id, struct option_row *rows, int max)
{
	int n = 0;
	struct target_component_setting_desc *descs = calloc((size_t)max, sizeof(*descs));
	struct target_component_setting_view *views = calloc((size_t)max, sizeof(*views));
	if (descs == NULL || views == NULL) {
		free(descs);
		free(views);
		return 0;
	}
	const int nd = k->describe != NULL ? k->describe(id, descs, max) : 0;
	for (int i = 0; i < nd && n < max; i++) {
		struct option_row *r = &rows[n++];
		memset(r, 0, sizeof(*r));
		r->desc = descs[i];
		r->declared = true;
		(void)target_component_get_setting(k->cls, id, r->desc.key, r->value, sizeof(r->value), &r->source);
	}
	const int nv = target_component_list_settings(k->cls, id, views, max);
	for (int i = 0; i < nv && n < max; i++) {
		bool known = false;
		for (int j = 0; j < n; j++) {
			if (ci_equal(rows[j].desc.key, views[i].key)) {
				known = true;
				break;
			}
		}
		if (known) {
			continue;
		}
		struct option_row *r = &rows[n++];
		memset(r, 0, sizeof(*r));
		snprintf(r->desc.key, sizeof(r->desc.key), "%s", views[i].key);
		snprintf(r->value, sizeof(r->value), "%s", views[i].value);
		r->source = views[i].source;
		r->declared = false;
	}
	free(descs);
	free(views);
	return n;
}

int
cli_component_options(const struct cli_component_class *k, const char *id, bool json)
{
	const int rc = gate(k, id, "options");
	if (rc != 0) {
		return rc;
	}
	struct option_row *rows = calloc(CLI_OPTIONS_MAX, sizeof(*rows));
	if (rows == NULL) {
		return 1;
	}
	const int n = collect(k, id, rows, CLI_OPTIONS_MAX);

	char env_prefix[TARGET_COMPONENT_SETTING_ENV_MAX] = {0};
	if (target_component_setting_env_name(k->cls->env_class, id, "KEY", env_prefix, sizeof(env_prefix))) {
		env_prefix[strlen(env_prefix) - 3] = '\0'; // "DXR_<CLASS>_<ID>_"
	}
	char user_store[512] = {0};
	char machine_store[512] = {0};
	(void)target_component_setting_store_path(k->cls, id, false, user_store, sizeof(user_store));
	(void)target_component_setting_store_path(k->cls, id, true, machine_store, sizeof(machine_store));

	if (json) {
		cJSON *root = cJSON_CreateObject();
		cJSON_AddStringToObject(root, "id", id);
		cJSON_AddStringToObject(root, "class", k->verb);
		cJSON_AddStringToObject(root, "env_prefix", env_prefix);
		cJSON_AddStringToObject(root, "user_store", user_store);
		cJSON_AddStringToObject(root, "machine_store", machine_store);
		cJSON *arr = cJSON_AddArrayToObject(root, "settings");
		for (int i = 0; i < n; i++) {
			const struct option_row *r = &rows[i];
			cJSON *o = cJSON_CreateObject();
			cJSON_AddStringToObject(o, "key", r->desc.key);
			cJSON_AddBoolToObject(o, "declared", r->declared);
			if (r->declared) {
				cJSON_AddStringToObject(o, "type", r->desc.type);
				cJSON_AddStringToObject(o, "default", r->desc.default_value);
				cJSON_AddStringToObject(o, "help", r->desc.help);
			} else {
				cJSON_AddNullToObject(o, "type");
				cJSON_AddNullToObject(o, "default");
				cJSON_AddNullToObject(o, "help");
			}
			if (r->source == TARGET_COMPONENT_SETTING_ABSENT) {
				cJSON_AddNullToObject(o, "value");
			} else {
				cJSON_AddStringToObject(o, "value", r->value);
			}
			cJSON_AddStringToObject(o, "source", target_component_setting_source_str(r->source));
			cJSON_AddItemToArray(arr, o);
		}
		char *out = cJSON_Print(root);
		if (out != NULL) {
			printf("%s\n", out);
			cJSON_free(out);
		}
		cJSON_Delete(root);
		free(rows);
		return 0;
	}

	P(" :: %s settings (%s)\n", id, k->noun);
	if (n == 0) {
		P("\t(nothing declared and nothing stored)\n");
	} else {
		P("\t%-24s %-7s %-10s %-20s %-12s %s\n", "key", "type", "default", "value", "source", "help");
		for (int i = 0; i < n; i++) {
			const struct option_row *r = &rows[i];
			P("\t%-24s %-7s %-10s %-20s %-12s %s\n", r->desc.key, r->declared ? r->desc.type : "?",
			  r->declared && r->desc.default_value[0] != '\0' ? r->desc.default_value : "-",
			  r->source == TARGET_COMPONENT_SETTING_ABSENT ? "<unset>" : r->value,
			  target_component_setting_source_str(r->source),
			  r->declared ? r->desc.help : "(stored, not declared by the component)");
		}
	}
	P("\tenvironment: %s<KEY> in the hosting process always wins\n",
	  env_prefix[0] != '\0' ? env_prefix : "DXR_<CLASS>_<ID>_");
	P("\tper-user:    %s   (%s set %s <KEY> <value>)\n",
	  user_store[0] != '\0' ? user_store : "(none on this platform)", k->verb, id);
#ifdef XRT_OS_WINDOWS
	const char *machine_how = "--machine, admin; HKCU wins unless Policy\\Lock";
#else
	const char *machine_how = "edited by hand; the per-user file wins";
#endif
	P("\tmachine:     %s   (%s)\n",
	  machine_store[0] != '\0' ? machine_store : (k->posix_machine_hint != NULL ? k->posix_machine_hint : "(none)"),
	  machine_how);
	free(rows);
	return 0;
}


/*
 *
 * set / unset
 *
 */

//! Which tier a write went to, for the confirmation line (no directive inside a macro argument: MSVC C2059).
static const char *
tier_str(bool machine)
{
#ifdef XRT_OS_WINDOWS
	return machine ? "machine-wide (HKLM)" : "for this user (HKCU)";
#else
	(void)machine;
	return "for this user";
#endif
}

int
cli_component_set(const struct cli_component_class *k, const char *id, const char *key, const char *value, bool machine)
{
	const int rc = gate(k, id, "set");
	if (rc != 0) {
		return rc;
	}
	if (!target_component_setting_key_valid(key)) {
		P("error: '%s' is not a setting key (letters, digits and '_', not starting with a digit).\n",
		  key != NULL ? key : "");
		return 1;
	}
	if (value == NULL || strlen(value) >= TARGET_COMPONENT_SETTING_VALUE_MAX) {
		P("error: the value must be under %d bytes.\n", TARGET_COMPONENT_SETTING_VALUE_MAX);
		return 1;
	}
	const int w =
	    write_result(k, target_component_set_setting(k->cls, id, key, value, machine),
	                 machine ? "writing the machine-wide setting" : "writing the per-user setting", machine);
	if (w != 0) {
		return w;
	}
	char store[512] = {0};
	(void)target_component_setting_store_path(k->cls, id, machine, store, sizeof(store));
	char upper[TARGET_COMPONENT_SETTING_KEY_MAX];
	for (size_t i = 0; i < sizeof(upper); i++) {
		const char c = key[i];
		upper[i] = (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
		if (c == '\0') {
			break;
		}
	}
	upper[sizeof(upper) - 1] = '\0';
	P("%s: %s = '%s' %s (%s).\n", id, upper, value, tier_str(machine), store);
	report_effective(k, id, upper, machine);
	if (k->note_pickup != NULL) {
		k->note_pickup();
	}
	return 0;
}

int
cli_component_unset(const struct cli_component_class *k, const char *id, const char *key, bool machine)
{
	const int rc = gate(k, id, "unset");
	if (rc != 0) {
		return rc;
	}
	if (!target_component_setting_key_valid(key)) {
		P("error: '%s' is not a setting key (letters, digits and '_', not starting with a digit).\n",
		  key != NULL ? key : "");
		return 1;
	}
	const int w =
	    write_result(k, target_component_unset_setting(k->cls, id, key, machine),
	                 machine ? "clearing the machine-wide setting" : "clearing the per-user setting", machine);
	if (w != 0) {
		return w;
	}
	P("%s: %s cleared %s.\n", id, key, tier_str(machine));
	report_effective(k, id, key, machine);
	if (k->note_pickup != NULL) {
		k->note_pickup();
	}
	return 0;
}


/*
 *
 * Dispatch + usage.
 *
 */

void
cli_component_options_usage(const struct cli_component_class *k)
{
	P("  options <id> [--json]        Every setting the %s declares or has stored: type, default,\n", k->noun);
	P("                               effective value and where it came from (environment / user /\n");
	P("                               machine / manifest). The ADR-054 'options' verb.\n");
	P("  set <id> <KEY> <value> [--machine]\n");
	P("                               Setting <KEY> on the %s's row: per user by default (no admin),\n", k->noun);
	P("                               machine-wide (HKLM, admin) with --machine. DXR_%s_<ID>_<KEY> in the\n",
	  k->cls != NULL && k->cls->env_class != NULL ? k->cls->env_class : "<CLASS>");
	P("                               hosting process's environment always wins.\n");
	P("  unset <id> <KEY> [--machine] Clear it (per user, or the machine value with --machine).\n");
}

int
cli_component_options_dispatch(const struct cli_component_class *k, int argc, const char **argv)
{
	if (argc < 3) {
		return -1;
	}
	const char *verb = argv[2];
	const bool machine = cli_has_flag(argc, argv, "--machine");
	const bool json = cli_has_flag(argc, argv, "--json");
	// Positional arguments after the verb, flags skipped.
	const char *pos[3] = {NULL, NULL, NULL};
	int np = 0;
	for (int i = 3; i < argc && np < 3; i++) {
		if (argv[i][0] == '-' && argv[i][1] == '-') {
			continue;
		}
		pos[np++] = argv[i];
	}
	if (strcmp(verb, "options") == 0) {
		if (np < 1) {
			P("error: '%s options' needs a %s id.\n\n", k->verb, k->noun);
			cli_component_options_usage(k);
			return 1;
		}
		return cli_component_options(k, pos[0], json);
	}
	if (strcmp(verb, "set") == 0) {
		if (np < 3) {
			P("error: '%s set' needs <id> <KEY> <value>.\n\n", k->verb);
			cli_component_options_usage(k);
			return 1;
		}
		return cli_component_set(k, pos[0], pos[1], pos[2], machine);
	}
	if (strcmp(verb, "unset") == 0) {
		if (np < 2) {
			P("error: '%s unset' needs <id> <KEY>.\n\n", k->verb);
			cli_component_options_usage(k);
			return 1;
		}
		return cli_component_unset(k, pos[0], pos[1], machine);
	}
	return -1;
}
