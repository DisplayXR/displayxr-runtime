// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `displayxr-cli activate-url [--json] [--notify] <link | ->`: handle one
 *         `displayxr://activate` deep link (ADR-064 §5,
 *         `docs/specs/runtime/activation-deep-link.md`).
 *
 * The same handler the platform's registered scheme handler runs
 * (`displayxr-activate.exe` on Windows). On desktop Linux this verb IS the
 * registered handler (`displayxr-url-handler.desktop`: `Exec=displayxr-cli
 * activate-url --notify %u`), and on macOS the registered bundle
 * (`DisplayXRActivate.app`) runs it with `--json -`, writes the link to its
 * standard input and shows its `title` / `message`. Also the terminal and
 * test entry on every platform.
 *
 * The link argument `-` means "read the link from standard input" (one line,
 * at most U_ACTIVATION_URL_MAX bytes). An argument is visible to every user of
 * the machine in the process list for as long as the handler runs; standard
 * input is not. Whoever already holds the link privately (the macOS bundle
 * gets it as an Apple Event) uses `-`. The Windows and Linux registrations
 * cannot: the OS itself puts the link on the handler's command line.
 *
 * `--json`: `{"result", "accepted", "quiet", "title", "message", "detail",
 * "class", "id", "nonce_id", "via", "error", "licence": {...}}`. `result` is
 * one of `activated | refused | unsupported | not-loaded | unknown | busy |
 * invalid-link | not-pending | item-mismatch | store-error`; `quiet` is true
 * when a failed link should raise no dialog (too many failed links recently);
 * `class` / `id` name the component the NONCE was begun for ("" when the link
 * never resolved to one); `via` is `service` or `process`. Exit 0 = activated.
 *
 * The link is attacker-controlled, so the argument rule is fixed: the link is
 * the LAST argument and is never read as an option, whatever it looks like
 * (a last argument of exactly `-` selects standard input, which is empty under
 * a browser launch: an invalid link); the only options are `--json` and
 * `--notify`, and only before it; anything else is refused before the link is
 * looked at. Nothing of the link is printed, opened or executed.
 *
 * @ingroup cli
 */

#include "cli_common.h"
#include "cli_component_options.h"

#include "activation_deliver.h"
#include "activation_handler.h"

#include <cjson/cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define P(...) fprintf(stderr, __VA_ARGS__)

//! Zero and free the buffer a link was read into (it holds the token). NULL is fine.
static void
wipe_free(char *buf, size_t cap)
{
	if (buf == NULL) {
		return;
	}
	volatile char *p = (volatile char *)buf;
	for (size_t i = 0; i < cap; i++) {
		p[i] = 0;
	}
	free(buf);
}

static int
usage(void)
{
	P("Usage: displayxr-cli activate-url [--json] [--notify] <displayxr://activate?... | ->\n");
	P("  Handle one activation deep link: check it, consume its pending nonce (begun with\n");
	P("  '<class> activate-begin <id>'), and hand its token to the one component the nonce was\n");
	P("  begun for. A link works once; nothing of it is printed or stored.\n");
	P("  -          read the link from standard input (keeps it out of the process list).\n");
	P("  --json     one JSON object with the outcome on stdout.\n");
	P("  --notify   also raise the desktop's native message (what the registered handler does).\n");
	return 2;
}

int
cli_cmd_activate_url(int argc, const char **argv)
{
	// argv[0] = program, argv[1] = "activate-url", argv[argc-1] = the link.
	if (argc < 3) {
		return usage();
	}
	bool json = false;
	bool notify = false;
	for (int i = 2; i < argc - 1; i++) {
		if (strcmp(argv[i], "--json") == 0) {
			json = true;
		} else if (strcmp(argv[i], "--notify") == 0) {
			notify = true;
		} else {
			// More than one non-option argument: a link that was split by a
			// launcher's quoting, or an injected argument. Refuse it whole.
			P("error: activate-url takes one link, as the last argument.\n\n");
			return usage();
		}
	}
	const char *url = argv[argc - 1];

	// "-": the link comes on standard input, one line. Over-long input is
	// handed on over-long, so the parser refuses it as it would an argument.
	char *from_stdin = NULL;
	const size_t stdin_cap = U_ACTIVATION_URL_MAX + 8;
	if (strcmp(url, "-") == 0) {
		from_stdin = (char *)calloc(1, stdin_cap);
		if (from_stdin == NULL) {
			return 1;
		}
		size_t n = fread(from_stdin, 1, stdin_cap - 1, stdin);
		from_stdin[n] = '\0';
		while (n > 0 && (from_stdin[n - 1] == '\n' || from_stdin[n - 1] == '\r')) {
			from_stdin[--n] = '\0';
		}
		url = from_stdin;
	}

	// No silent activation: when asked to show the outcome and there is no
	// way to, stop BEFORE the link is looked at (its nonce stays pending).
	if (notify && !activation_notify_available()) {
		P("error: no desktop notifier found (notify-send, zenity or kdialog); the link was not handled.\n");
		P("       Run 'displayxr-cli activate-url <link>' in a terminal, or enter the licence key instead.\n");
		wipe_free(from_stdin, stdin_cap);
		return 2;
	}

	struct activation_ops ops;
	memset(&ops, 0, sizeof(ops));
	ops.deliver = activation_deliver_default;
	struct activation_outcome out;
	activation_handle_url(url, &ops, &out);
	wipe_free(from_stdin, stdin_cap); // it held the token

	char title[96];
	char body[1024];
	activation_outcome_message(&out, title, sizeof(title), body, sizeof(body));
	if (json) {
		cJSON *root = cJSON_CreateObject();
		cJSON_AddStringToObject(root, "result", activation_outcome_str(out.kind));
		cJSON_AddBoolToObject(root, "accepted", out.kind == ACTIVATION_OUTCOME_ACTIVATED);
		// What a front end shows (the macOS handler bundle does): a short title and
		// a plain-text message, unless `quiet` (this link named no pending activation
		// and too many such links came recently: no dialog for each one of a flood).
		cJSON_AddBoolToObject(root, "quiet", out.quiet);
		cJSON_AddStringToObject(root, "title", title);
		cJSON_AddStringToObject(root, "message", body);
		cJSON_AddStringToObject(root, "detail", out.detail);
		cJSON_AddStringToObject(root, "class", out.cls);
		cJSON_AddStringToObject(root, "id", out.id);
		cJSON_AddStringToObject(root, "nonce_id", out.nonce_id);
		cJSON_AddStringToObject(root, "via", out.via);
		cJSON_AddStringToObject(root, "error", out.error);
		cli_component_license_json(root, &out.licence);
		char *txt = cJSON_Print(root);
		if (txt != NULL) {
			printf("%s\n", txt);
			cJSON_free(txt);
		}
		cJSON_Delete(root);
	} else {
		P("%s\n%s\n", title, body);
	}
	if (notify && !out.quiet) {
		(void)activation_notify(&out);
	}
	return activation_outcome_exit_code(out.kind);
}
