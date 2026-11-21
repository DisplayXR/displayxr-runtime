// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `displayxr-cli catalog`: the decisions (no operating-system calls).
 *
 * See cli_catalog.h. Spec: docs/specs/runtime/dxr-get.md § displayxr-cli catalog.
 *
 * @ingroup targets_cli
 */

#include "cli_catalog.h"

#include <cjson/cJSON.h>

#include <stdlib.h>
#include <string.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

//! The read-only verbs: the engine's own HTTP bounds add up to a few minutes at worst.
#define READ_TIMEOUT_MS (10u * 60u * 1000u)
//! The verbs that run installers: the engine waits up to one hour per installer.
#define CHANGE_TIMEOUT_MS (2u * 60u * 60u * 1000u)

#define MAX_ENGINE_ARGS 12

/*
 *
 * Exit codes.
 *
 */

const char *
cli_catalog_exit_message(int code)
{
	switch (code) {
	case 0: return "done";
	case 2: return "dxr-get did not accept the arguments (is it the version this CLI was built with?)";
	case 10: return "offline: the catalog could not be reached, or it has expired; nothing was changed";
	case 11: return "the catalog's signature was refused (or its serial is older than one already accepted)";
	case 20: return "a download failed after every retry; nothing was installed, and the same command resumes it";
	case 21: return "verification failed: the size, the SHA-256 or the signer of a download is not the catalog's";
	case 30: return "an installer or uninstaller failed, or left the component as it was";
	case 40: return "not available: the catalog has no signed build of it for this machine, or does not list it";
	case 41: return "not compatible with the installed runtime (version range or class ABI)";
	case 42: return "not installed through the catalog: there is no install receipt for it";
	case 43: return "cannot be uninstalled here: no uninstall command was recorded, or other items require it";
	case 50: return "needs an elevated process";
	case CLI_CATALOG_EXIT_NO_ENGINE: return "the install engine (dxr-get) is not available";
	case CLI_CATALOG_EXIT_SPAWN: return "dxr-get could not be started";
	case CLI_CATALOG_EXIT_TIMEOUT: return "dxr-get was still running when the wait ended";
	case CLI_CATALOG_EXIT_BAD_OUTPUT: return "dxr-get's output was not the JSON document expected";
	default: return "dxr-get ended with an exit code this CLI does not know";
	}
}

/*
 *
 * What to expect after a change.
 *
 */

const char *
cli_catalog_expectation(const char *item_class)
{
	if (item_class == NULL) {
		return NULL;
	}
	// What is said here is what the code does: dxr-get stops the service and
	// its satellites and starts the service again around any item of a class
	// the runtime or the service loads (dxr-get.md § ensure-for, uninstall);
	// an app that hosts the runtime in-process discovers plug-ins when it
	// creates its instance; and the service chooses per class as
	// service/main.c's component watch describes.
	if (strcmp(item_class, "display_processor") == 0) {
		return "dxr-get stopped the DisplayXR service and started it again, so the service has probed its "
		       "displays with the new set. An app that was already running keeps the display processor it "
		       "started with: the change applies at its next session. Check: displayxr-cli dp list";
	}
	if (strcmp(item_class, "input_provider") == 0) {
		return "dxr-get stopped the DisplayXR service and started it again, so the service has loaded the "
		       "new set of input providers. An app that was already running in-process picks it up at its "
		       "next launch. Check: displayxr-cli input list";
	}
	if (strcmp(item_class, "conversion_provider") == 0) {
		return "dxr-get stopped the DisplayXR service and started it again, so the next conversion stream "
		       "or job selects among the new set. Check: displayxr-cli conv list";
	}
	if (strcmp(item_class, "tracking_provider") == 0) {
		return "The service picks its viewer-tracking provider once per run; dxr-get stopped it and "
		       "started it again, so it has picked again. An app that was already running in-process picks "
		       "at its next launch. Check: displayxr-cli tracking list";
	}
	if (strcmp(item_class, "workspace_controller") == 0) {
		return "dxr-get stopped the DisplayXR service and started it again, so the service has read the "
		       "controller registrations again. Check: displayxr-cli workspace list";
	}
	if (strcmp(item_class, "runtime") == 0) {
		return "The runtime was replaced: close and start again any app that was running.";
	}
	if (strcmp(item_class, "companion") == 0) {
		return "dxr-get stopped the DisplayXR service and started it again around the change.";
	}
	return NULL;
}

/*
 *
 * Windows command-line quoting.
 *
 */

bool
cli_catalog_quote_arg(const char *arg, char *out, size_t cap)
{
	size_t n = 0;
#define PUT(c)                                                                                                         \
	do {                                                                                                           \
		if (n + 1 >= cap) {                                                                                    \
			return false;                                                                                  \
		}                                                                                                      \
		out[n++] = (c);                                                                                        \
	} while (0)

	if (cap == 0) {
		return false;
	}
	const bool plain = arg[0] != '\0' && strpbrk(arg, " \t\n\v\"") == NULL;
	if (plain) {
		for (const char *p = arg; *p != '\0'; p++) {
			PUT(*p);
		}
		out[n] = '\0';
		return true;
	}
	PUT('"');
	for (const char *p = arg;; p++) {
		size_t slashes = 0;
		while (*p == '\\') {
			p++;
			slashes++;
		}
		if (*p == '\0') {
			// Backslashes before the closing quote are doubled, or they would escape it.
			for (size_t i = 0; i < slashes * 2; i++) {
				PUT('\\');
			}
			break;
		}
		if (*p == '"') {
			for (size_t i = 0; i < slashes * 2 + 1; i++) {
				PUT('\\');
			}
			PUT('"');
		} else {
			for (size_t i = 0; i < slashes; i++) {
				PUT('\\');
			}
			PUT(*p);
		}
	}
	PUT('"');
	out[n] = '\0';
	return true;
#undef PUT
}

/*
 *
 * Arguments.
 *
 */

void
cli_catalog_print_usage(FILE *f)
{
	fprintf(
	    f,
	    "Usage: displayxr-cli catalog <list|show|install|uninstall|update|outdated> [--json]\n"
	    "  list [--installed]           Every catalog item: installed and available version, state, and the\n"
	    "                               licence state of installed plug-ins. --installed: only what was\n"
	    "                               installed through the catalog (the receipts; needs no network).\n"
	    "  outdated                     The installed items the catalog has a newer build of.\n"
	    "  show <id>                    One item: publisher, tier, versions, compatibility, prerequisites.\n"
	    "  install <id>... | --default  Install items and what they need. --default: the catalog's\n"
	    "                               recommended set plus the plug-ins of the hardware that is attached.\n"
	    "  uninstall <id>...            Remove items that were installed through the catalog.\n"
	    "  update <id>... | --all       Install the catalog's version of items 'outdated' reports.\n"
	    "Options:\n"
	    "  --json                       One JSON document on stdout: this CLI's envelope around dxr-get's own.\n"
	    "  --elevate | --no-elevate     install / uninstall / update need an elevated process. From a\n"
	    "                               terminal the engine is started elevated (one consent prompt per\n"
	    "                               item that needs it). With --json, or when not at a terminal, it is\n"
	    "                               not: exit 50, unless --elevate is given. --no-elevate: never.\n"
	    "  --allow-runtime              Let install / update replace the runtime itself when an item needs a\n"
	    "                               newer one. Without it such an item is refused (exit 41). Naming the\n"
	    "                               runtime ('update runtime') implies it for that item.\n"
	    "  --timeout <seconds>          How long to wait for the engine (default 600 for list / show /\n"
	    "                               outdated, 7200 for the verbs that install).\n"
	    "  --catalog <url|file>         Another catalog, handed to dxr-get; it must still be signed.\n"
	    "The work is done by dxr-get (Windows; macOS and Linux have no install engine yet). Exit codes are\n"
	    "dxr-get's: 0 ok, 10 offline, 11 catalog signature, 20 download, 21 verification, 30 installer failed,\n"
	    "40 not available, 41 not compatible, 42 not installed, 43 cannot uninstall, 50 needs elevation;\n"
	    "and this CLI's: 60 no engine, 61 engine did not start, 62 timed out, 63 unexpected engine output.\n");
}

static bool
fail(char *err, size_t cap, const char *fmt, const char *a)
{
	(void)snprintf(err, cap, fmt, a);
	return false;
}

bool
cli_catalog_parse(int argc, const char **argv, struct cli_catalog_request *out, char *err, size_t err_cap)
{
	static const struct
	{
		const char *name;
		enum cli_catalog_verb verb;
	} verbs[] = {
	    {"list", CLI_CATALOG_VERB_LIST},       {"show", CLI_CATALOG_VERB_SHOW},
	    {"install", CLI_CATALOG_VERB_INSTALL}, {"uninstall", CLI_CATALOG_VERB_UNINSTALL},
	    {"update", CLI_CATALOG_VERB_UPDATE},   {"outdated", CLI_CATALOG_VERB_OUTDATED},
	};
	memset(out, 0, sizeof(*out));
	err[0] = '\0';
	if (argc < 3) {
		return fail(err, err_cap, "%s", "a verb is needed");
	}
	for (size_t i = 0; i < ARRAY_SIZE(verbs); i++) {
		if (strcmp(argv[2], verbs[i].name) == 0) {
			out->verb = verbs[i].verb;
			out->verb_name = verbs[i].name;
		}
	}
	if (out->verb == CLI_CATALOG_VERB_NONE) {
		return fail(err, err_cap, "unknown verb '%s'", argv[2]);
	}
	for (int i = 3; i < argc; i++) {
		const char *a = argv[i];
		if (strcmp(a, "--json") == 0) {
			out->json = true;
		} else if (strcmp(a, "--elevate") == 0) {
			out->elevate = true;
		} else if (strcmp(a, "--no-elevate") == 0) {
			out->no_elevate = true;
		} else if (strcmp(a, "--allow-runtime") == 0) {
			out->allow_runtime = true;
		} else if (strcmp(a, "--installed") == 0) {
			out->installed = true;
		} else if (strcmp(a, "--default") == 0) {
			out->use_default = true;
		} else if (strcmp(a, "--all") == 0) {
			out->all = true;
		} else if (strcmp(a, "--catalog") == 0 && i + 1 < argc) {
			out->catalog = argv[++i];
		} else if (strcmp(a, "--timeout") == 0 && i + 1 < argc) {
			char *end = NULL;
			const long v = strtol(argv[++i], &end, 10);
			if (end == argv[i] || *end != '\0' || v <= 0 || v > 24 * 60 * 60) {
				return fail(err, err_cap, "--timeout needs a number of seconds (1..86400), not '%s'",
				            argv[i]);
			}
			out->timeout_s = (uint32_t)v;
		} else if (a[0] == '-') {
			return fail(err, err_cap, "unknown option '%s'", a);
		} else if (out->n_ids >= CLI_CATALOG_MAX_IDS) {
			return fail(err, err_cap, "%s", "too many items on one command line");
		} else {
			out->ids[out->n_ids++] = a;
		}
	}

	const enum cli_catalog_verb v = out->verb;
	const bool changes =
	    v == CLI_CATALOG_VERB_INSTALL || v == CLI_CATALOG_VERB_UNINSTALL || v == CLI_CATALOG_VERB_UPDATE;
	if (out->installed && v != CLI_CATALOG_VERB_LIST) {
		return fail(err, err_cap, "--installed belongs to 'list', not '%s'", out->verb_name);
	}
	if (out->use_default && v != CLI_CATALOG_VERB_INSTALL) {
		return fail(err, err_cap, "--default belongs to 'install', not '%s'", out->verb_name);
	}
	if (out->all && v != CLI_CATALOG_VERB_UPDATE) {
		return fail(err, err_cap, "--all belongs to 'update', not '%s'", out->verb_name);
	}
	if ((out->elevate || out->no_elevate || out->allow_runtime) && !changes) {
		return fail(err, err_cap,
		            "'%s' changes nothing: --elevate, --no-elevate and --allow-runtime do not apply",
		            out->verb_name);
	}
	if (out->elevate && out->no_elevate) {
		return fail(err, err_cap, "%s", "--elevate and --no-elevate exclude each other");
	}
	if (out->allow_runtime && v == CLI_CATALOG_VERB_UNINSTALL) {
		return fail(err, err_cap, "%s", "--allow-runtime does not apply to 'uninstall'");
	}
	switch (v) {
	case CLI_CATALOG_VERB_LIST:
	case CLI_CATALOG_VERB_OUTDATED:
		if (out->n_ids != 0) {
			return fail(err, err_cap, "'%s' takes no item", out->verb_name);
		}
		break;
	case CLI_CATALOG_VERB_SHOW:
		if (out->n_ids != 1) {
			return fail(err, err_cap, "%s", "'show' needs exactly one item id");
		}
		break;
	case CLI_CATALOG_VERB_INSTALL:
		if ((out->n_ids > 0) == out->use_default) {
			return fail(err, err_cap, "%s", "'install' needs item ids, or --default (not both)");
		}
		break;
	case CLI_CATALOG_VERB_UNINSTALL:
		if (out->n_ids == 0) {
			return fail(err, err_cap, "%s", "'uninstall' needs an item id");
		}
		break;
	case CLI_CATALOG_VERB_UPDATE:
		if ((out->n_ids > 0) == out->all) {
			return fail(err, err_cap, "%s", "'update' needs item ids, or --all (not both)");
		}
		break;
	default: break;
	}
	return true;
}

uint32_t
cli_catalog_timeout_ms(const struct cli_catalog_request *req, bool changes_machine)
{
	if (req->timeout_s != 0) {
		return req->timeout_s * 1000u;
	}
	return changes_machine ? CHANGE_TIMEOUT_MS : READ_TIMEOUT_MS;
}

int
cli_catalog_engine_args(const struct cli_catalog_request *req,
                        const char *engine_verb,
                        const char *id,
                        bool no_runtime,
                        bool json,
                        const char **args,
                        int cap)
{
	int n = 0;
#define ADD(s)                                                                                                         \
	do {                                                                                                           \
		if (n < cap) {                                                                                         \
			args[n] = (s);                                                                                 \
		}                                                                                                      \
		n++;                                                                                                   \
	} while (0)

	if (req->catalog != NULL) {
		ADD("--catalog");
		ADD(req->catalog);
	}
	ADD(engine_verb);
	if (id != NULL) {
		ADD(id);
	} else if (strcmp(engine_verb, "install") == 0) {
		ADD("--default");
	}
	if (no_runtime) {
		ADD("--no-runtime");
	}
	if (json) {
		ADD("--json");
	}
	if (n < cap) {
		args[n] = NULL;
	}
	return n;
#undef ADD
}

/*
 *
 * A run of the front end.
 *
 */

struct ctx
{
	const struct cli_catalog_ops *ops;
	const struct cli_catalog_request *req;
	cJSON *calls;    //!< every engine invocation, in order
	cJSON *items;    //!< the outcome per item, for the verbs that change the machine
	cJSON *receipts; //!< `list --installed --json`, read once; NULL until asked
	bool receipts_read;
	int exit_code; //!< the first failure
};

static const char *
jstr(const cJSON *o, const char *k)
{
	const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
	return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void
note_exit(struct ctx *c, int code)
{
	if (c->exit_code == 0 && code != 0) {
		c->exit_code = code;
	}
}

/*!
 * One engine invocation, recorded in `calls`. Returns the exit code to
 * report: the engine's, or 61 / 62 when it did not run to an end. With
 * @p elevated the engine is started through the elevation prompt; @p status
 * (optional) gets the raw status so the caller can tell a declined prompt.
 * @p out_doc (optional) gets the parsed stdout, owned by the `calls` record.
 */
static int
call_engine(struct ctx *c,
            const char *const *args,
            int n_args,
            bool changes_machine,
            bool elevated,
            enum cli_catalog_run_status *status,
            const cJSON **out_doc)
{
	struct cli_catalog_run r;
	memset(&r, 0, sizeof(r));
	r.exe = c->ops->engine;
	r.args = args;
	r.n_args = n_args;
	r.timeout_ms = cli_catalog_timeout_ms(c->req, changes_machine);
	r.capture_stdout = !elevated;
	// A read-only call may be ended. One that may be running an installer is
	// left alone, as dxr-get leaves a slow installer alone.
	r.kill_on_timeout = !changes_machine;

	enum cli_catalog_run_status st;
	if (elevated) {
		st = c->ops->run_elevated != NULL ? c->ops->run_elevated(c->ops->ud, &r) : CLI_CATALOG_RUN_UNSUPPORTED;
	} else {
		st = c->ops->run(c->ops->ud, &r);
	}
	int code;
	switch (st) {
	case CLI_CATALOG_RUN_OK: code = r.exit_code; break;
	case CLI_CATALOG_RUN_TIMEOUT: code = CLI_CATALOG_EXIT_TIMEOUT; break;
	case CLI_CATALOG_RUN_DECLINED: code = CLI_CATALOG_EXIT_NEEDS_ELEVATION; break;
	default: code = CLI_CATALOG_EXIT_SPAWN; break;
	}

	cJSON *rec = cJSON_CreateObject();
	cJSON *a = cJSON_AddArrayToObject(rec, "args");
	for (int i = 0; i < n_args; i++) {
		cJSON_AddItemToArray(a, cJSON_CreateString(args[i]));
	}
	cJSON_AddNumberToObject(rec, "exit", code);
	cJSON_AddStringToObject(rec, "message",
	                        st == CLI_CATALOG_RUN_DECLINED ? "the elevation prompt was declined"
	                                                       : cli_catalog_exit_message(code));
	cJSON_AddBoolToObject(rec, "elevated", elevated);
	cJSON *doc = r.out != NULL ? cJSON_Parse(r.out) : NULL;
	if (doc != NULL) {
		cJSON_AddItemToObject(rec, "output", doc);
	} else {
		cJSON_AddNullToObject(rec, "output");
	}
	free(r.out);
	cJSON_AddItemToArray(c->calls, rec);
	if (status != NULL) {
		*status = st;
	}
	if (out_doc != NULL) {
		*out_doc = doc;
	}
	return code;
}

//! Print the envelope (`--json`), free the run's state, and return its exit code.
static int
finish(struct ctx *c, const char *message, cJSON *extra_key_owner)
{
	const struct cli_catalog_request *req = c->req;
	if (req->json) {
		cJSON *root = cJSON_CreateObject();
		cJSON_AddNumberToObject(root, "schema", 1);
		cJSON_AddStringToObject(root, "verb", req->verb_name);
		cJSON_AddNumberToObject(root, "exit", c->exit_code);
		cJSON_AddStringToObject(root, "message",
		                        message != NULL ? message : cli_catalog_exit_message(c->exit_code));
		if (c->ops->engine != NULL) {
			cJSON_AddStringToObject(root, "engine", c->ops->engine);
		} else {
			cJSON_AddNullToObject(root, "engine");
		}
		cJSON_AddItemToObject(root, "calls", c->calls);
		c->calls = NULL;
		if (c->items != NULL) {
			cJSON_AddItemToObject(root, "items", c->items);
			c->items = NULL;
		}
		// The verb's own keys (`item`, `licences`), moved in.
		while (extra_key_owner != NULL && extra_key_owner->child != NULL) {
			cJSON *k = cJSON_DetachItemViaPointer(extra_key_owner, extra_key_owner->child);
			cJSON_AddItemToObject(root, k->string, k);
		}
		char *s = cJSON_Print(root);
		if (s != NULL) {
			fprintf(c->ops->out, "%s\n", s);
			cJSON_free(s);
		}
		cJSON_Delete(root);
	} else if (c->exit_code != 0 && message != NULL) {
		fprintf(c->ops->err, "%s\n", message);
	}
	(void)fflush(c->ops->out);
	cJSON_Delete(extra_key_owner);
	cJSON_Delete(c->calls);
	cJSON_Delete(c->items);
	cJSON_Delete(c->receipts);
	return c->exit_code;
}

/*
 *
 * list, outdated, show.
 *
 */

static bool
state_is_installed(const char *state)
{
	return state != NULL && strcmp(state, "not_installed") != 0;
}

/*!
 * The licence of an installed plug-in row, joined from the class's own
 * licence surface: its human cell in @p cell, and its object added to
 * @p licences (keyed by the catalog id) when that is given.
 */
static void
row_licence(struct ctx *c, const cJSON *row, cJSON *licences, char *cell, size_t cap)
{
	(void)snprintf(cell, cap, "-");
	const char *reg = jstr(row, "registered_id");
	const char *id = jstr(row, "id");
	if (c->ops->licence == NULL || reg == NULL || id == NULL || !state_is_installed(jstr(row, "state"))) {
		return;
	}
	char tmp[96];
	(void)snprintf(tmp, sizeof(tmp), "-");
	cJSON *lic = c->ops->licence(c->ops->ud, jstr(row, "class"), reg, tmp, sizeof(tmp));
	if (lic == NULL) {
		return;
	}
	(void)snprintf(cell, cap, "%s", tmp);
	if (licences != NULL) {
		cJSON_AddItemToObject(licences, id, lic);
	} else {
		cJSON_Delete(lic);
	}
}

static const cJSON *
find_row(const cJSON *rows, const char *want)
{
	const cJSON *x;
	cJSON_ArrayForEach(x, rows)
	{
		const char *id = jstr(x, "id");
		if (id != NULL && strcmp(id, want) == 0) {
			return x;
		}
	}
	cJSON_ArrayForEach(x, rows)
	{
		const cJSON *al;
		cJSON_ArrayForEach(al, cJSON_GetObjectItemCaseSensitive(x, "aliases"))
		{
			if (cJSON_IsString(al) && strcmp(al->valuestring, want) == 0) {
				return x;
			}
		}
	}
	return NULL;
}

static int
width_of(const cJSON *rows, const char *key, const char *head, int max)
{
	int w = (int)strlen(head);
	const cJSON *x;
	cJSON_ArrayForEach(x, rows)
	{
		const char *s = jstr(x, key);
		const int n = s != NULL ? (int)strlen(s) : 1;
		w = n > w ? n : w;
	}
	return w > max ? max : w;
}

static const char *
or_dash(const char *s)
{
	return s != NULL && s[0] != '\0' ? s : "-";
}

static void
print_rows(struct ctx *c, const cJSON *rows, cJSON *licences, const char *empty)
{
	FILE *f = c->ops->out;
	if (cJSON_GetArraySize(rows) == 0) {
		fprintf(f, "%s\n", empty);
		return;
	}
	const int w_id = width_of(rows, "id", "ID", 32);
	const int w_cls = width_of(rows, "class", "CLASS", 22);
	const int w_name = width_of(rows, "name", "NAME", 36);
	const int w_state = width_of(rows, "state", "STATE", 14);
	fprintf(f, "%-*s  %-*s  %-*s  %-10s  %-10s  %-*s  %s\n", w_id, "ID", w_cls, "CLASS", w_name, "NAME",
	        "INSTALLED", "AVAILABLE", w_state, "STATE", "LICENCE");
	const cJSON *x;
	cJSON_ArrayForEach(x, rows)
	{
		char cell[96];
		row_licence(c, x, licences, cell, sizeof(cell));
		fprintf(f, "%-*s  %-*s  %-*.*s  %-10s  %-10s  %-*s  %s\n", w_id, or_dash(jstr(x, "id")), w_cls,
		        or_dash(jstr(x, "class")), w_name, w_name, or_dash(jstr(x, "name")),
		        or_dash(jstr(x, "installed")), or_dash(jstr(x, "available")), w_state,
		        or_dash(jstr(x, "state")), cell);
	}
}

static void
print_receipts(struct ctx *c, const cJSON *rows)
{
	FILE *f = c->ops->out;
	if (cJSON_GetArraySize(rows) == 0) {
		fprintf(f,
		        "Nothing was installed through the catalog on this machine. Components installed by "
		        "their own installers have no receipt; 'catalog list' shows them.\n");
		return;
	}
	const int w_id = width_of(rows, "id", "ID", 32);
	const int w_cls = width_of(rows, "class", "CLASS", 22);
	fprintf(f, "%-*s  %-*s  %-10s  %-20s  %s\n", w_id, "ID", w_cls, "CLASS", "VERSION", "INSTALLED AT",
	        "UNINSTALL");
	const cJSON *x;
	cJSON_ArrayForEach(x, rows)
	{
		fprintf(f, "%-*s  %-*s  %-10s  %-20s  %s\n", w_id, or_dash(jstr(x, "id")), w_cls,
		        or_dash(jstr(x, "class")), or_dash(jstr(x, "version")), or_dash(jstr(x, "install_time")),
		        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(x, "can_uninstall")) ? "yes" : "no");
	}
}

static void
print_item(struct ctx *c, const cJSON *x, const char *lic_cell)
{
	FILE *f = c->ops->out;
	const cJSON *v;
	fprintf(f, "%s  (%s)\n", or_dash(jstr(x, "id")), or_dash(jstr(x, "name")));
	if (jstr(x, "summary") != NULL) {
		fprintf(f, "  %s\n", jstr(x, "summary"));
	}
	fprintf(f, "  class:        %s", or_dash(jstr(x, "class")));
	if (jstr(x, "registered_id") != NULL) {
		fprintf(f, " (registers as '%s')", jstr(x, "registered_id"));
	}
	fprintf(f, "\n  publisher:    %s (%s)\n", or_dash(jstr(x, "publisher")), or_dash(jstr(x, "tier")));
	fprintf(f, "  installed:    %s\n", or_dash(jstr(x, "installed")));
	fprintf(f, "  available:    %s\n", or_dash(jstr(x, "available")));
	fprintf(f, "  state:        %s%s\n", or_dash(jstr(x, "state")),
	        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(x, "receipt")) ? " (installed through the catalog)" : "");
	if (cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(x, "installable"))) {
		fprintf(f, "  installable:  no%s%s\n", jstr(x, "listing") != NULL ? "; get it from " : "",
		        jstr(x, "listing") != NULL ? jstr(x, "listing") : "");
	}
	if (cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(x, "compatible"))) {
		fprintf(f, "  compatible:   NO: %s\n", or_dash(jstr(x, "incompatible_reason")));
	}
	if (jstr(x, "detected") != NULL && strcmp(jstr(x, "detected"), "none") != 0) {
		fprintf(f, "  detected:     %s confidence: %s\n", jstr(x, "detected"), or_dash(jstr(x, "evidence")));
	}
	if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(x, "recommended"))) {
		fprintf(f, "  recommended:  yes (part of 'catalog install --default')\n");
	}
	v = cJSON_GetObjectItemCaseSensitive(x, "download");
	if (cJSON_IsNumber(v) && v->valuedouble > 0) {
		fprintf(f, "  download:     %.1f MB\n", v->valuedouble / (1024.0 * 1024.0));
	}
	const cJSON *p;
	cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(x, "prerequisites"))
	{
		const bool met = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(p, "met"));
		fprintf(f, "  prerequisite: %s: %s%s%s\n", or_dash(jstr(p, "name")), met ? "present" : "MISSING",
		        !met && jstr(p, "info_url") != NULL ? "; " : "",
		        !met && jstr(p, "info_url") != NULL ? jstr(p, "info_url") : "");
	}
	if (strcmp(lic_cell, "-") != 0) {
		fprintf(f, "  licence:      %s\n", lic_cell);
	}
}

static void
add_catalog_arg(const struct cli_catalog_request *req, const char **args, int *n)
{
	if (req->catalog != NULL) {
		args[(*n)++] = "--catalog";
		args[(*n)++] = req->catalog;
	}
}

/*!
 * `dxr-get <list|outdated> [--installed] --json`, into @p rows (the array
 * under @p key). Returns the engine's code; 63 when it exited 0 without the
 * document.
 */
static int
engine_rows(struct ctx *c, const char *engine_verb, bool installed, const char *key, const cJSON **rows)
{
	const char *args[MAX_ENGINE_ARGS];
	int n = 0;
	if (!installed) { // the receipts need no catalog
		add_catalog_arg(c->req, args, &n);
	}
	args[n++] = engine_verb;
	if (installed) {
		args[n++] = "--installed";
	}
	args[n++] = "--json";
	const cJSON *doc = NULL;
	int code = call_engine(c, args, n, false, false, NULL, &doc);
	*rows = cJSON_GetObjectItemCaseSensitive(doc, key);
	if (code == 0 && !cJSON_IsArray(*rows)) {
		code = CLI_CATALOG_EXIT_BAD_OUTPUT;
	}
	return code;
}

static int
do_list(struct ctx *c)
{
	const struct cli_catalog_request *req = c->req;
	const bool outdated = req->verb == CLI_CATALOG_VERB_OUTDATED;
	const char *key = req->installed ? "installed" : (outdated ? "outdated" : "items");
	const cJSON *rows = NULL;
	note_exit(c, engine_rows(c, outdated ? "outdated" : "list", req->installed, key, &rows));
	if (c->exit_code != 0) {
		return finish(c, cli_catalog_exit_message(c->exit_code), NULL);
	}
	cJSON *extra = cJSON_CreateObject();
	if (req->installed) {
		if (!req->json) {
			print_receipts(c, rows);
		}
		return finish(c, NULL, extra);
	}
	cJSON *licences = cJSON_AddObjectToObject(extra, "licences");
	if (req->json) {
		const cJSON *x;
		cJSON_ArrayForEach(x, rows)
		{
			char cell[96];
			row_licence(c, x, licences, cell, sizeof(cell));
		}
	} else {
		print_rows(c, rows, NULL,
		           outdated ? "Everything installed is at the catalog's version."
		                    : "The catalog lists no items.");
	}
	return finish(c, NULL, extra);
}

static int
do_show(struct ctx *c)
{
	const struct cli_catalog_request *req = c->req;
	const cJSON *rows = NULL;
	note_exit(c, engine_rows(c, "list", false, "items", &rows));
	if (c->exit_code != 0) {
		return finish(c, cli_catalog_exit_message(c->exit_code), NULL);
	}
	cJSON *extra = cJSON_CreateObject();
	const cJSON *row = find_row(rows, req->ids[0]);
	if (row == NULL) {
		char msg[200];
		(void)snprintf(msg, sizeof(msg), "%s: the catalog does not list it ('displayxr-cli catalog list')",
		               req->ids[0]);
		note_exit(c, CLI_CATALOG_EXIT_NOT_FETCHABLE);
		cJSON_AddNullToObject(extra, "item");
		return finish(c, msg, extra);
	}
	cJSON *licences = cJSON_CreateObject();
	char cell[96];
	row_licence(c, row, licences, cell, sizeof(cell));
	cJSON_AddItemToObject(extra, "item", cJSON_Duplicate(row, 1));
	cJSON_AddItemToObject(extra, "licences", licences);
	if (!req->json) {
		print_item(c, row, cell);
	}
	return finish(c, NULL, extra);
}

/*
 *
 * install, uninstall, update.
 *
 */

//! The class a receipt records for @p id, or NULL. The receipts are read once per run.
static const char *
receipt_class(struct ctx *c, const char *id)
{
	if (!c->receipts_read) {
		c->receipts_read = true;
		const char *args[] = {"list", "--installed", "--json"};
		struct cli_catalog_run r;
		memset(&r, 0, sizeof(r));
		r.exe = c->ops->engine;
		r.args = args;
		r.n_args = 3;
		r.timeout_ms = cli_catalog_timeout_ms(c->req, false);
		r.capture_stdout = true;
		r.kill_on_timeout = true;
		// Not recorded in `calls`: it is this CLI's lookup, not part of the answer.
		if (c->ops->run(c->ops->ud, &r) == CLI_CATALOG_RUN_OK && r.exit_code == 0 && r.out != NULL) {
			c->receipts = cJSON_Parse(r.out);
		}
		free(r.out);
	}
	const cJSON *x;
	cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(c->receipts, "installed"))
	{
		const char *rid = jstr(x, "id");
		if (rid != NULL && strcmp(rid, id) == 0) {
			return jstr(x, "class");
		}
	}
	return NULL;
}

static bool
may_elevate(const struct ctx *c)
{
	const struct cli_catalog_request *req = c->req;
	if (req->no_elevate) {
		return false;
	}
	if (req->elevate) {
		return true;
	}
	return !req->json && c->ops->is_interactive != NULL && c->ops->is_interactive(c->ops->ud);
}

//! The plan `install --json` printed, for a person.
static void
print_plan(struct ctx *c, const cJSON *plan)
{
	FILE *f = c->ops->out;
	const cJSON *a;
	cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(plan, "actions"))
	{
		const char *action = jstr(a, "action");
		const char *name = jstr(a, "id") != NULL ? jstr(a, "id") : or_dash(jstr(a, "component"));
		const cJSON *dl = cJSON_GetObjectItemCaseSensitive(a, "download");
		const double mb = cJSON_IsNumber(dl) ? dl->valuedouble / (1024.0 * 1024.0) : 0.0;
		if (action == NULL || strcmp(action, "none") == 0) {
			fprintf(f, "  %s: %s is installed, nothing to do\n", name, or_dash(jstr(a, "installed")));
		} else if (strcmp(action, "skip") == 0) {
			fprintf(f, "  %s: skipped (%s)%s%s\n", name, or_dash(jstr(a, "state")),
			        jstr(a, "reason") != NULL ? ": " : "",
			        jstr(a, "reason") != NULL ? jstr(a, "reason") : "");
		} else if (jstr(a, "installed") != NULL) {
			fprintf(f, "  %s: %s %s -> %s (%.1f MB to download)\n", name, action, jstr(a, "installed"),
			        or_dash(jstr(a, "target")), mb);
		} else {
			fprintf(f, "  %s: %s %s (%.1f MB to download)\n", name, action, or_dash(jstr(a, "target")), mb);
		}
	}
	cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(plan, "prerequisites"))
	{
		if (cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(a, "met")) &&
		    cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(a, "installable"))) {
			fprintf(f, "  prerequisite MISSING: %s (needed by %s); DisplayXR does not install it: %s\n",
			        or_dash(jstr(a, "name")), or_dash(jstr(a, "for")), or_dash(jstr(a, "info_url")));
		}
	}
	(void)fflush(f);
}

/*!
 * One `install` or `uninstall` through the engine, with the elevation step.
 * @p id is NULL for `install --default`. Records the outcome in `items`.
 */
static void
change_item(struct ctx *c, const char *engine_verb, const char *id, bool no_runtime)
{
	const struct cli_catalog_request *req = c->req;
	const bool install = strcmp(engine_verb, "install") == 0;
	const char *label = id != NULL ? id : "the default set";
	FILE *out = c->ops->out, *err = c->ops->err;
	char msg[600];
	msg[0] = '\0';

	// A receipt is gone once its item is removed: read the class first.
	const char *removed_class = install ? NULL : receipt_class(c, id);
	char removed_class_buf[64] = {0};
	if (removed_class != NULL) {
		(void)snprintf(removed_class_buf, sizeof(removed_class_buf), "%s", removed_class);
	}

	// First with this process's rights. The engine answers everything that
	// elevation cannot fix before it asks for elevation (dxr-get.md), so 50
	// means an elevated run is all that is missing -- and an already elevated
	// terminal simply does the work here.
	const char *args[MAX_ENGINE_ARGS];
	int n = cli_catalog_engine_args(req, engine_verb, id, no_runtime, true, args, MAX_ENGINE_ARGS);
	const cJSON *doc = NULL;
	int code = call_engine(c, args, n, true, false, NULL, &doc);
	bool elevated = false;

	// The ids the plan changes, kept by value: `doc` belongs to the call record.
	cJSON *changed = cJSON_CreateArray();
	if (install) {
		const cJSON *a;
		cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(doc, "actions"))
		{
			const char *action = jstr(a, "action");
			const char *kind = jstr(a, "kind");
			if (jstr(a, "id") != NULL && action != NULL && (kind == NULL || strcmp(kind, "item") == 0) &&
			    (strcmp(action, "install") == 0 || strcmp(action, "upgrade") == 0 ||
			     strcmp(action, "reinstall") == 0)) {
				cJSON_AddItemToArray(changed, cJSON_CreateString(jstr(a, "id")));
			}
		}
		if (!req->json && doc != NULL && cJSON_GetObjectItemCaseSensitive(doc, "actions") != NULL) {
			fprintf(out, "%s:\n", label);
			print_plan(c, doc);
		}
	} else if (id != NULL) {
		cJSON_AddItemToArray(changed, cJSON_CreateString(id));
	}

	if (code == CLI_CATALOG_EXIT_NEEDS_ELEVATION) {
		const cJSON *gate = cJSON_GetObjectItemCaseSensitive(doc, "gate");
		if (install && cJSON_IsNumber(gate) && gate->valueint == CLI_CATALOG_EXIT_NOT_FETCHABLE) {
			// The engine checks for elevation before it reports this; an
			// elevated run would only return 40 after a consent prompt.
			code = CLI_CATALOG_EXIT_NOT_FETCHABLE;
			(void)snprintf(msg, sizeof(msg), "%s: %s%s%s", label, cli_catalog_exit_message(code),
			               jstr(doc, "gate_reason") != NULL ? ": " : "",
			               jstr(doc, "gate_reason") != NULL ? jstr(doc, "gate_reason") : "");
		} else if (!may_elevate(c)) {
			(void)snprintf(msg, sizeof(msg), "%s: %s. Run this from an elevated terminal%s.", label,
			               cli_catalog_exit_message(code),
			               req->no_elevate ? "" : ", or add --elevate to be asked for consent");
		} else {
			if (!req->json) {
				fprintf(err,
				        "%s: asking for elevation to run dxr-get (one consent prompt). Its own window "
				        "shows the progress.\n",
				        label);
				(void)fflush(err);
			}
			// The same command, without --json: nothing can be read back from
			// an elevated child but its exit code.
			n = cli_catalog_engine_args(req, engine_verb, id, no_runtime, false, args, MAX_ENGINE_ARGS);
			enum cli_catalog_run_status st = CLI_CATALOG_RUN_OK;
			code = call_engine(c, args, n, true, true, &st, NULL);
			elevated = st == CLI_CATALOG_RUN_OK;
			if (st == CLI_CATALOG_RUN_DECLINED) {
				(void)snprintf(msg, sizeof(msg),
				               "%s: the elevation prompt was declined; nothing was changed", label);
			} else if (st == CLI_CATALOG_RUN_UNSUPPORTED) {
				(void)snprintf(msg, sizeof(msg), "%s: this platform has no elevated start", label);
			} else if (st == CLI_CATALOG_RUN_TIMEOUT) {
				(void)snprintf(msg, sizeof(msg),
				               "%s: %s. It was not ended: an installer may still be running", label,
				               cli_catalog_exit_message(code));
			}
		}
	} else if (code == CLI_CATALOG_EXIT_TIMEOUT) {
		(void)snprintf(msg, sizeof(msg), "%s: %s. It was not ended: an installer may still be running", label,
		               cli_catalog_exit_message(code));
	} else if (code == CLI_CATALOG_EXIT_INCOMPATIBLE && install && no_runtime) {
		const char *why = jstr(doc, "gate_reason");
		(void)snprintf(
		    msg, sizeof(msg),
		    "%s: %s%s%s. If it needs a newer runtime, add --allow-runtime, or update the runtime first", label,
		    cli_catalog_exit_message(code), why != NULL ? ": " : "", why != NULL ? why : "");
	}

	const bool did_change = code == 0 && cJSON_GetArraySize(changed) > 0;
	if (did_change && install) {
		// The receipts this run read earlier do not know what was just installed.
		cJSON_Delete(c->receipts);
		c->receipts = NULL;
		c->receipts_read = false;
	}
	if (msg[0] == '\0') {
		if (code != 0) {
			const char *detail =
			    install ? jstr(doc, "gate_reason") : (elevated ? NULL : jstr(doc, "detail"));
			(void)snprintf(msg, sizeof(msg), "%s: %s%s%s", label, cli_catalog_exit_message(code),
			               detail != NULL ? ": " : "", detail != NULL ? detail : "");
		} else if (!install) {
			(void)snprintf(msg, sizeof(msg), "%s: uninstalled", label);
		} else if (did_change) {
			(void)snprintf(msg, sizeof(msg), "%s: installed", label);
		} else {
			(void)snprintf(msg, sizeof(msg), "%s: nothing to do, already at the catalog's version", label);
		}
	}

	cJSON *item = cJSON_CreateObject();
	if (id != NULL) {
		cJSON_AddStringToObject(item, "id", id);
	} else {
		cJSON_AddNullToObject(item, "id");
	}
	cJSON_AddStringToObject(item, "action", engine_verb);
	cJSON_AddNumberToObject(item, "exit", code);
	cJSON_AddStringToObject(item, "message", msg);
	cJSON_AddBoolToObject(item, "elevated", elevated);
	cJSON *expect = cJSON_AddArrayToObject(item, "expect");
	if (!req->json) {
		fprintf(code == 0 ? out : err, "%s\n", msg);
	}
	if (did_change) {
		const cJSON *ch;
		cJSON_ArrayForEach(ch, changed)
		{
			const char *cid = ch->valuestring;
			const char *cls =
			    install ? receipt_class(c, cid) : (removed_class_buf[0] ? removed_class_buf : NULL);
			const char *note = cli_catalog_expectation(cls);
			if (note == NULL) {
				continue;
			}
			cJSON *e = cJSON_CreateObject();
			cJSON_AddStringToObject(e, "id", cid);
			cJSON_AddStringToObject(e, "class", cls);
			cJSON_AddStringToObject(e, "note", note);
			cJSON_AddItemToArray(expect, e);
			if (!req->json) {
				fprintf(out, "  %s: %s\n", cid, note);
			}
		}
	}
	cJSON_AddItemToObject(item, "changed", did_change ? changed : cJSON_CreateArray());
	if (!did_change) {
		cJSON_Delete(changed);
	}
	cJSON_AddItemToArray(c->items, item);
	note_exit(c, code);
	(void)fflush(out);
}

//! Record an outcome that needed no engine call (`update` of an item that is current, unknown, ...).
static void
note_item(struct ctx *c, const char *id, int code, const char *text)
{
	char msg[400];
	(void)snprintf(msg, sizeof(msg), "%s: %s", id, text);
	cJSON *item = cJSON_CreateObject();
	cJSON_AddStringToObject(item, "id", id);
	cJSON_AddStringToObject(item, "action", "none");
	cJSON_AddNumberToObject(item, "exit", code);
	cJSON_AddStringToObject(item, "message", msg);
	cJSON_AddBoolToObject(item, "elevated", false);
	cJSON_AddArrayToObject(item, "expect");
	cJSON_AddArrayToObject(item, "changed");
	cJSON_AddItemToArray(c->items, item);
	if (!c->req->json) {
		fprintf(code == 0 ? c->ops->out : c->ops->err, "%s\n", msg);
	}
	note_exit(c, code);
}

//! `runtime`, the name the runtime answers to with either schema (dxr-get.md § Commands).
static bool
names_runtime(const char *id)
{
	return strcmp(id, "runtime") == 0;
}

static int
do_install(struct ctx *c)
{
	const struct cli_catalog_request *req = c->req;
	if (req->use_default) {
		change_item(c, "install", NULL, !req->allow_runtime);
	}
	for (int i = 0; i < req->n_ids; i++) {
		change_item(c, "install", req->ids[i], !req->allow_runtime && !names_runtime(req->ids[i]));
	}
	return finish(c, NULL, NULL);
}

static int
do_uninstall(struct ctx *c)
{
	for (int i = 0; i < c->req->n_ids; i++) {
		change_item(c, "uninstall", c->req->ids[i], false);
	}
	return finish(c, NULL, NULL);
}

static bool
row_is_runtime(const cJSON *row)
{
	const char *cls = jstr(row, "class");
	return cls != NULL && strcmp(cls, "runtime") == 0;
}

static int
do_update(struct ctx *c)
{
	const struct cli_catalog_request *req = c->req;
	// `update` is `install` for what `outdated` reports.
	const cJSON *rows = NULL;
	note_exit(c, engine_rows(c, "outdated", false, "outdated", &rows));
	if (c->exit_code != 0) {
		return finish(c, cli_catalog_exit_message(c->exit_code), NULL);
	}

	if (req->all) {
		int n = 0;
		const cJSON *x;
		cJSON_ArrayForEach(x, rows)
		{
			const char *id = jstr(x, "id");
			if (id == NULL) {
				continue;
			}
			n++;
			if (row_is_runtime(x) && !req->allow_runtime) {
				// The runtime is never touched unless it is named.
				note_item(c, id, 0,
				          "a newer runtime is available; --all leaves the runtime alone. Name it to "
				          "update it: displayxr-cli catalog update runtime");
			} else if (cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(x, "installable"))) {
				note_item(c, id, 0,
				          "a newer version exists, but the catalog has no build to install here");
			} else {
				change_item(c, "install", id, !req->allow_runtime && !row_is_runtime(x));
			}
		}
		if (n == 0 && !req->json) {
			fprintf(c->ops->out, "Everything installed is at the catalog's version.\n");
		}
		return finish(c, NULL, NULL);
	}

	const cJSON *all_rows = NULL; // `list --json`, only when a named item is not outdated
	bool listed = false;
	for (int i = 0; i < req->n_ids; i++) {
		const char *want = req->ids[i];
		const cJSON *row = find_row(rows, want);
		if (row != NULL) {
			// Named, so the runtime itself may be the item.
			change_item(c, "install", jstr(row, "id"), !req->allow_runtime && !row_is_runtime(row));
			continue;
		}
		if (!listed) {
			listed = true;
			if (engine_rows(c, "list", false, "items", &all_rows) != 0) {
				all_rows = NULL;
			}
		}
		const cJSON *any = find_row(all_rows, want);
		if (all_rows == NULL) {
			note_item(c, want, 0, "'outdated' does not report it; nothing to do");
		} else if (any == NULL) {
			note_item(c, want, CLI_CATALOG_EXIT_NOT_FETCHABLE,
			          "the catalog does not list it ('displayxr-cli catalog list')");
		} else if (!state_is_installed(jstr(any, "state"))) {
			note_item(c, want, CLI_CATALOG_EXIT_NOT_INSTALLED,
			          "it is not installed; 'displayxr-cli catalog install' installs it");
		} else {
			note_item(c, want, 0, "already at the catalog's version; nothing to do");
		}
	}
	return finish(c, NULL, NULL);
}

/*
 *
 * Entry points.
 *
 */

int
cli_catalog_execute(const struct cli_catalog_ops *ops, const struct cli_catalog_request *req)
{
	struct ctx c;
	memset(&c, 0, sizeof(c));
	c.ops = ops;
	c.req = req;
	c.calls = cJSON_CreateArray();
	const bool changes = req->verb == CLI_CATALOG_VERB_INSTALL || req->verb == CLI_CATALOG_VERB_UNINSTALL ||
	                     req->verb == CLI_CATALOG_VERB_UPDATE;
	if (changes) {
		c.items = cJSON_CreateArray();
	}

	if (ops->engine == NULL) {
		// No engine, no answer: never a stub listing.
		char msg[600];
		(void)snprintf(msg, sizeof(msg), "displayxr-cli catalog: %s",
		               ops->engine_missing != NULL ? ops->engine_missing
		                                           : cli_catalog_exit_message(CLI_CATALOG_EXIT_NO_ENGINE));
		c.exit_code = CLI_CATALOG_EXIT_NO_ENGINE;
		return finish(&c, msg, NULL);
	}

	switch (req->verb) {
	case CLI_CATALOG_VERB_LIST:
	case CLI_CATALOG_VERB_OUTDATED: return do_list(&c);
	case CLI_CATALOG_VERB_SHOW: return do_show(&c);
	case CLI_CATALOG_VERB_INSTALL: return do_install(&c);
	case CLI_CATALOG_VERB_UNINSTALL: return do_uninstall(&c);
	case CLI_CATALOG_VERB_UPDATE: return do_update(&c);
	default: break;
	}
	c.exit_code = CLI_CATALOG_EXIT_USAGE;
	return finish(&c, NULL, NULL);
}

int
cli_catalog_main(const struct cli_catalog_ops *ops, int argc, const char **argv)
{
	struct cli_catalog_request req;
	char err[240];
	if (argc >= 3 && (strcmp(argv[2], "help") == 0 || strcmp(argv[2], "--help") == 0)) {
		cli_catalog_print_usage(ops->out);
		return CLI_CATALOG_EXIT_OK;
	}
	if (!cli_catalog_parse(argc, argv, &req, err, sizeof(err))) {
		fprintf(ops->err, "error: %s.\n\n", err);
		cli_catalog_print_usage(ops->err);
		return CLI_CATALOG_EXIT_USAGE;
	}
	return cli_catalog_execute(ops, &req);
}
