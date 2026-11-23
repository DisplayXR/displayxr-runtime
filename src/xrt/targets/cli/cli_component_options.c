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
#include "cli_option_parse.h"

#include "target_component_loader.h"

#include "xrt/xrt_component_license.h"
#include "util/u_status_snapshot.h" // u_status_license_add_cjson
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
report_effective(const struct cli_component_class *k, const char *id, const char *key, bool machine, bool secret)
{
	char value[TARGET_COMPONENT_SETTING_VALUE_MAX];
	uint32_t source = TARGET_COMPONENT_SETTING_ABSENT;
	const bool set = target_component_get_setting(k->cls, id, key, value, sizeof(value), &source);
	if (secret) {
		snprintf(value, sizeof(value), "<set>"); // a secret is shown as set / not set, never as a value
	}
	char env[TARGET_COMPONENT_SETTING_ENV_MAX] = {0};
	(void)target_component_setting_env_name(k->cls->env_class, id, key, env, sizeof(env));
	if (!set) {
		P("Effective now: %s unset — the %s keeps its compiled default.\n", key, k->noun);
		return;
	}
	P("Effective now: %s = %s%s%s (source: %s)", key, secret ? "" : "'", value, secret ? "" : "'",
	  target_component_setting_source_str(source));
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
	char value[TARGET_COMPONENT_SETTING_VALUE_MAX]; //!< "" for a secret: never held past the read
	uint32_t source;
	bool declared;
	bool secret; //!< ADR-054 N5: shown as set / not set, never as a value
};

//! Redact a row whose key is a secret: keep where it came from, drop what it is.
static void
row_redact(struct option_row *r)
{
	r->secret = target_component_setting_is_secret(r->declared ? r->desc.type : NULL, r->desc.key);
	if (r->secret) {
		memset(r->value, 0, sizeof(r->value));
	}
}

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
		row_redact(r);
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
		row_redact(r);
	}
	// The scratch copies held every stored value, secrets included.
	memset(views, 0, (size_t)max * sizeof(*views));
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
			// A secret's value is never emitted: `value` is null, `set` says
			// whether one is stored. `secret` is on every row.
			cJSON_AddBoolToObject(o, "secret", r->secret);
			if (r->secret) {
				cJSON_AddNullToObject(o, "value");
				cJSON_AddBoolToObject(o, "set", r->source != TARGET_COMPONENT_SETTING_ABSENT);
			} else if (r->source == TARGET_COMPONENT_SETTING_ABSENT) {
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
			  r->source == TARGET_COMPONENT_SETTING_ABSENT ? "<unset>" : (r->secret ? "<set>" : r->value),
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

//! Is @p key of @p id a secret: by its declared type, else by its name (target_component_setting_is_secret).
static bool
key_is_secret(const struct cli_component_class *k, const char *id, const char *key)
{
	const char *type = NULL;
	struct target_component_setting_desc *descs = calloc(CLI_OPTIONS_MAX, sizeof(*descs));
	const int nd = descs != NULL && k->describe != NULL ? k->describe(id, descs, CLI_OPTIONS_MAX) : 0;
	for (int i = 0; i < nd; i++) {
		if (ci_equal(descs[i].key, key)) {
			type = descs[i].type;
			break;
		}
	}
	const bool secret = target_component_setting_is_secret(type, key);
	free(descs);
	return secret;
}

/*!
 * `set`, the value from an argument (@p value) or, with @p from_stdin, one
 * line of standard input. A stdin value is wiped from memory after the write.
 */
static int
component_set(const struct cli_component_class *k,
              const char *id,
              const char *key,
              const char *value,
              bool from_stdin,
              bool machine)
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
	// ADR-054 N5: a `secret` (a licence key) goes to the same store, may be
	// longer, and is never echoed back.
	const bool secret = key_is_secret(k, id, key);
	const size_t value_max =
	    secret ? (size_t)TARGET_COMPONENT_SETTING_SECRET_VALUE_MAX : (size_t)TARGET_COMPONENT_SETTING_VALUE_MAX;
	char *line = NULL;
	if (from_stdin) {
		// One line, the trailing newline stripped; nothing of it is ever printed.
		line = calloc(1, value_max);
		if (line == NULL) {
			return 1;
		}
		const enum cli_value_read r = cli_read_value_line(stdin, line, value_max);
		if (r != CLI_VALUE_READ_OK) {
			memset(line, 0, value_max);
			free(line);
			if (r == CLI_VALUE_READ_EMPTY) {
				P("error: --stdin read an empty value — nothing written (use '%s unset' to clear "
				  "it).\n",
				  k->verb);
			} else if (r == CLI_VALUE_READ_TOO_LONG) {
				P("error: the value read from --stdin must be under %d bytes — nothing written.\n",
				  (int)value_max);
			} else {
				P("error: could not read the value from standard input — nothing written.\n");
			}
			return 1;
		}
		value = line;
	} else if (secret) {
		// Still accepted (scripts), but a command line is visible to other
		// processes and kept in shell history.
		fprintf(
		    stderr,
		    "warning: %s is a secret; a value on the command line can be seen by other programs and kept in "
		    "shell history — prefer '%s set %s %s --stdin'.\n",
		    key, k->verb, id, key);
	}
	if (value == NULL || strlen(value) >= value_max) {
		P("error: the value must be under %d bytes.\n", (int)value_max);
		free(line);
		return 1;
	}
	const int w =
	    write_result(k,
	                 secret ? target_component_set_secret(k->cls, id, key, value, machine)
	                        : target_component_set_setting(k->cls, id, key, value, machine),
	                 machine ? "writing the machine-wide setting" : "writing the per-user setting", machine);
	if (w != 0) {
		if (line != NULL) {
			memset(line, 0, value_max);
			free(line);
		}
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
	if (secret) {
		P("%s: %s = <set> %s (%s). The value is a secret: it is never shown again.\n", id, upper,
		  tier_str(machine), store);
	} else {
		P("%s: %s = '%s' %s (%s).\n", id, upper, value, tier_str(machine), store);
	}
	if (line != NULL) {
		memset(line, 0, value_max); // the value lives in the store now, not here
		free(line);
		line = NULL;
		value = NULL;
	}
	report_effective(k, id, upper, machine, secret);
	if (k->note_pickup != NULL) {
		k->note_pickup();
	}
	return 0;
}

int
cli_component_set(const struct cli_component_class *k, const char *id, const char *key, const char *value, bool machine)
{
	return component_set(k, id, key, value, false, machine);
}

int
cli_component_set_stdin(const struct cli_component_class *k, const char *id, const char *key, bool machine)
{
	return component_set(k, id, key, NULL, true, machine);
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
	report_effective(k, id, key, machine, key_is_secret(k, id, key));
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
	P("  set <id> <KEY> --stdin [--machine]\n");
	P("                               The same, the value read from standard input: one line, the trailing\n");
	P("                               newline dropped; empty is refused. Never echoed. Use it for a secret.\n");
	P("  unset <id> <KEY> [--machine] Clear it (per user, or the machine value with --machine).\n");
	P("                               A setting of type 'secret' (a licence key) is stored the same way and\n");
	P("                               shown only as <set> / <unset>; set it with --stdin (a value on the\n");
	P("                               command line still works, with a warning).\n");
	if (k->activate != NULL) {
		P("  activate <id> <token | @file> [--json]\n");
		P("                               Hand an activation token (a licence key, or the token a vendor's\n");
		P("                               checkout returned) to the %s. Opaque to the runtime:\n", k->noun);
		P("                               never parsed, logged or stored by it. @file reads the first line.\n");
	}
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
		switch (cli_set_args_check(np, cli_has_flag(argc, argv, "--stdin"))) {
		case CLI_SET_ARGS_VALUE: return cli_component_set(k, pos[0], pos[1], pos[2], machine);
		case CLI_SET_ARGS_STDIN: return cli_component_set_stdin(k, pos[0], pos[1], machine);
		case CLI_SET_ARGS_BOTH:
			P("error: '%s set' takes the value either as an argument or with --stdin, not both.\n\n",
			  k->verb);
			cli_component_options_usage(k);
			return 1;
		case CLI_SET_ARGS_MISSING:
		default:
			P("error: '%s set' needs <id> <KEY> <value>, or <id> <KEY> --stdin.\n\n", k->verb);
			cli_component_options_usage(k);
			return 1;
		}
	}
	if (strcmp(verb, "unset") == 0) {
		if (np < 2) {
			P("error: '%s unset' needs <id> <KEY>.\n\n", k->verb);
			cli_component_options_usage(k);
			return 1;
		}
		return cli_component_unset(k, pos[0], pos[1], machine);
	}
	if (strcmp(verb, "activate") == 0 && k->activate != NULL) {
		if (np < 2) {
			P("error: '%s activate' needs <id> <token | @file>.\n\n", k->verb);
			cli_component_options_usage(k);
			return 1;
		}
		return cli_component_activate(k, pos[0], pos[1], json);
	}
	return -1;
}


/*
 *
 * Licence state + activate (ADR-054 N5).
 *
 */

void
cli_component_license_json(struct cJSON *row, const struct xrt_component_license_state *lic)
{
	u_status_license_add_cjson(row, lic); // one writer for every `licence` object (aux_util)
}

const char *
cli_component_license_cell(const struct xrt_component_license_state *lic, char *buf, size_t cap)
{
	char day[24];
	u_status_unix_time_str(lic->expiry_unix_s, false, day, sizeof(day));
	const bool dated = lic->expiry_unix_s != 0 && day[0] != '\0';
	const char *src = xrt_component_license_source_str(lic->source);
	switch (lic->state) {
	case XRT_COMPONENT_LICENSE_LICENSED:
		if (dated) {
			snprintf(buf, cap, "licensed until %s", day);
		} else if (src[0] != '\0') {
			snprintf(buf, cap, "licensed (%s)", src);
		} else {
			snprintf(buf, cap, "licensed");
		}
		break;
	case XRT_COMPONENT_LICENSE_TRIAL:
		if (dated) {
			snprintf(buf, cap, "trial until %s", day);
		} else {
			snprintf(buf, cap, "trial");
		}
		break;
	case XRT_COMPONENT_LICENSE_EXPIRED:
		if (dated) {
			snprintf(buf, cap, "EXPIRED %s", day);
		} else {
			snprintf(buf, cap, "EXPIRED");
		}
		break;
	case XRT_COMPONENT_LICENSE_UNLICENSED: snprintf(buf, cap, "UNLICENSED"); break;
	default: snprintf(buf, cap, "-"); break;
	}
	return buf;
}

/*!
 * The token of `activate`: @p arg itself, or for `@path` the first line of
 * that file (trailing CR / LF / blanks dropped). False (message printed) when
 * the file cannot be read or the token is empty / too long.
 */
static bool
read_token(const char *arg, char *out, size_t cap)
{
	out[0] = '\0';
	if (arg == NULL) {
		return false;
	}
	if (arg[0] != '@') {
		if (strlen(arg) >= cap) {
			P("error: the token must be under %d bytes.\n", (int)cap);
			return false;
		}
		snprintf(out, cap, "%s", arg);
		return out[0] != '\0';
	}
	FILE *f = fopen(arg + 1, "rb");
	if (f == NULL) {
		P("error: cannot read the token file '%s'.\n", arg + 1);
		return false;
	}
	const size_t n = fread(out, 1, cap - 1, f);
	const bool more = n == cap - 1 && fgetc(f) != EOF;
	fclose(f);
	out[n] = '\0';
	size_t len = strcspn(out, "\r\n");
	out[len] = '\0';
	while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\t')) {
		out[--len] = '\0';
	}
	if (len == 0) {
		P("error: the token file '%s' is empty.\n", arg + 1);
		return false;
	}
	if (more && len == cap - 1) {
		memset(out, 0, cap);
		P("error: the token must be under %d bytes.\n", (int)cap);
		return false;
	}
	return true;
}

int
cli_component_activate(const struct cli_component_class *k, const char *id, const char *token_arg, bool json)
{
	const int rc = gate(k, id, "activate");
	if (rc != 0) {
		return rc;
	}
	char *token = calloc(1, XRT_COMPONENT_ACTIVATION_TOKEN_MAX);
	if (token == NULL) {
		return 1;
	}
	if (!read_token(token_arg, token, XRT_COMPONENT_ACTIVATION_TOKEN_MAX)) {
		free(token);
		return 1;
	}
	struct xrt_component_license_state lic;
	xrt_component_license_state_init(&lic);
	char err[XRT_COMPONENT_ACTIVATION_ERROR_MAX] = {0};
	const enum target_component_activate_result r =
	    (enum target_component_activate_result)k->activate(id, token, &lic, err, sizeof(err));
	memset(token, 0, XRT_COMPONENT_ACTIVATION_TOKEN_MAX); // the token is the component's, not ours to keep
	free(token);

	if (json) {
		cJSON *root = cJSON_CreateObject();
		cJSON_AddStringToObject(root, "id", id);
		cJSON_AddStringToObject(root, "class", k->verb);
		cJSON_AddStringToObject(root, "result", target_component_activate_result_str(r));
		cJSON_AddBoolToObject(root, "accepted", r == TARGET_COMPONENT_ACTIVATE_OK);
		cJSON_AddStringToObject(root, "error", err);
		cli_component_license_json(root, &lic);
		char *out = cJSON_Print(root);
		if (out != NULL) {
			printf("%s\n", out);
			cJSON_free(out);
		}
		cJSON_Delete(root);
		return r == TARGET_COMPONENT_ACTIVATE_OK ? 0 : 1;
	}

	char cell[64];
	switch (r) {
	case TARGET_COMPONENT_ACTIVATE_OK:
		P("%s: activation accepted — licence: %s%s%s.\n", id,
		  cli_component_license_cell(&lic, cell, sizeof(cell)), lic.message[0] != '\0' ? " — " : "",
		  lic.message);
		P("This reached the %s in this process. One already running (in the service or its host) follows\n",
		  k->noun);
		P("when it re-reads its licence; restart the service to be sure.\n");
		return 0;
	case TARGET_COMPONENT_ACTIVATE_REFUSED:
		P("FAIL: '%s' refused the token%s%s (licence: %s).\n", id, err[0] != '\0' ? " — " : "", err,
		  cli_component_license_cell(&lic, cell, sizeof(cell)));
		return 1;
	case TARGET_COMPONENT_ACTIVATE_UNSUPPORTED:
		P("FAIL: '%s' has no activation slot — it takes no token this way (see '%s options %s' for a "
		  "licence-key setting).\n",
		  id, k->verb, id);
		return 1;
	case TARGET_COMPONENT_ACTIVATE_BAD_TOKEN:
		P("FAIL: the token is empty or longer than %u bytes — nothing was delivered.\n",
		  (unsigned)(XRT_COMPONENT_ACTIVATION_TOKEN_MAX - 1));
		return 1;
	case TARGET_COMPONENT_ACTIVATE_NOT_LOADED:
	default:
		P("FAIL: '%s' could not be loaded (its binary is missing, refused by the provenance gate, or did "
		  "not negotiate) — see '%s list'.\n",
		  id, k->verb);
		return 1;
	}
}
