// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `displayxr-activate.exe`: the Windows handler of the `displayxr://`
 *         URL scheme (ADR-064 §5, `docs/specs/runtime/activation-deep-link.md`).
 *
 * Registered by the runtime installer as
 *
 *     HKLM\Software\Classes\displayxr\shell\open\command = "<dir>\displayxr-activate.exe" "%1"
 *
 * so a browser (or any program) that opens a `displayxr://` link starts this
 * process with the link as its one argument. Everything in that argument is
 * attacker-controlled. This program:
 *
 *  - takes its command line from `GetCommandLineW` and splits it with
 *    `CommandLineToArgvW`: EXACTLY one argument is accepted. The classic
 *    scheme-handler injection (a link containing `" --other-flag`) produces
 *    more than one argument and is refused before anything is parsed, and
 *    there is no flag to inject in any case: this program has no options and
 *    one action;
 *  - is a GUI-subsystem program (no console window flashes over the browser)
 *    with the default `asInvoker` manifest: it runs as the user who clicked,
 *    never asks for elevation and has no elevated mode;
 *  - removes the current directory from the DLL search path first (a browser
 *    may start it with the Downloads folder as the working directory);
 *  - never opens, executes, or passes on any part of the link;
 *  - shows one message box with the outcome, whose text never contains
 *    anything from the link (activation_outcome_message). A flood of bad
 *    links is answered silently once the per-user failure limit is reached.
 *
 * A dedicated program rather than `displayxr-cli.exe activate-url "%1"`: the
 * CLI is a console program (a console window would flash on every link) with
 * dozens of verbs and flags, and the registered command line is the one place
 * where a single-purpose binary costs nothing and removes a class of mistakes.
 * The CLI verb exists too, for a terminal and for tests; both call
 * activation_handle_url().
 *
 * @ingroup targets
 */

#include "activation_deliver.h"
#include "activation_handler.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

#include <stdlib.h>
#include <string.h>

int WINAPI
WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
	(void)hInstance;
	(void)hPrevInstance;
	(void)lpCmdLine; // the ANSI command line is lossy: never used
	(void)nCmdShow;

	// Before anything can load a DLL by name: drop the current directory from the search path.
	(void)SetDllDirectoryW(L"");

	// The link, as exactly one argument, converted strictly (an unpaired
	// surrogate or an over-long argument is an invalid link, not a crash).
	char *url = (char *)calloc(1, U_ACTIVATION_URL_MAX + 8);
	if (url == NULL) {
		return 1;
	}
	int argc = 0;
	LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (argv != NULL && argc == 2 && argv[1] != NULL) {
		if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argv[1], -1, url,
		                        (int)(U_ACTIVATION_URL_MAX + 8), NULL, NULL) <= 0) {
			url[0] = '\0';
		}
	}
	if (argv != NULL) {
		// The wide copy of the link holds the token too.
		for (int i = 0; i < argc; i++) {
			if (argv[i] != NULL) {
				SecureZeroMemory(argv[i], wcslen(argv[i]) * sizeof(wchar_t));
			}
		}
		LocalFree(argv);
	}

	struct activation_ops ops;
	memset(&ops, 0, sizeof(ops));
	ops.deliver = activation_deliver_default;

	struct activation_outcome out;
	activation_handle_url(url, &ops, &out); // "" (no / several / unconvertible arguments) is an invalid link
	SecureZeroMemory(url, U_ACTIVATION_URL_MAX + 8);
	free(url);

	if (!out.quiet) {
		(void)activation_notify(&out);
	}
	return activation_outcome_exit_code(out.kind);
}
