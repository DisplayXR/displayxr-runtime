// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `displayxr-cli catalog`: a thin front end over the install engine
 *         `dxr-get` (ADR-064 step 4).
 *
 * Nothing here reads a catalog, checks a signature or downloads a file: every
 * answer is `dxr-get`'s (docs/specs/runtime/dxr-get.md). This module maps the
 * `catalog` verbs to engine command lines, relays the engine's exit code with
 * a one-line message, wraps the engine's JSON without renaming its fields,
 * and, for the verbs that change the machine, starts the engine elevated when
 * the engine says elevation is all that is missing.
 *
 * Two halves, like `dxr-get` itself:
 *  - cli_catalog.c: every decision. No operating-system calls; the engine is
 *    reached through @ref cli_catalog_ops, so the unit test drives it with a
 *    fake engine on every platform (tests/tests_cli_catalog.cpp).
 *  - cli_catalog_os.c: finding the engine, running it with a bounded wait,
 *    the `ShellExecuteEx` "runas" relaunch.
 *
 * The engine path is a field of the ops table the caller fills. The CLI fills
 * it from @ref cli_catalog_os_locate_engine only; there is no environment
 * variable or argument that names another engine, because the path may be
 * started elevated.
 *
 * @ingroup targets_cli
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cJSON;

/*
 *
 * Exit codes.
 *
 */

/*!
 * The engine's exit codes (dxr-get.md § Exit codes) are returned unchanged.
 * These are the front end's own, chosen outside the engine's range.
 */
enum cli_catalog_exit
{
	CLI_CATALOG_EXIT_OK = 0,
	CLI_CATALOG_EXIT_USAGE = 2,            //!< same value as the engine's usage code
	CLI_CATALOG_EXIT_OFFLINE = 10,         //!< engine
	CLI_CATALOG_EXIT_NOT_FETCHABLE = 40,   //!< engine; also `show` / `update` of an id the catalog does not list
	CLI_CATALOG_EXIT_INCOMPATIBLE = 41,    //!< engine
	CLI_CATALOG_EXIT_NOT_INSTALLED = 42,   //!< engine; also `update` of an item that is not installed
	CLI_CATALOG_EXIT_NEEDS_ELEVATION = 50, //!< engine; also "the elevation prompt was declined"
	CLI_CATALOG_EXIT_NO_ENGINE = 60,       //!< dxr-get is not installed, or this platform has no engine yet
	CLI_CATALOG_EXIT_SPAWN = 61,           //!< dxr-get could not be started
	CLI_CATALOG_EXIT_TIMEOUT = 62,         //!< dxr-get was still running when the bounded wait ended
	CLI_CATALOG_EXIT_BAD_OUTPUT = 63,      //!< dxr-get's output was not the JSON document the verb promises
	//! Refused here: replacing the runtime, whose directory the engine itself runs from.
	CLI_CATALOG_EXIT_NOT_FROM_HERE = 64,
	//! dxr-get.exe is not validly signed, so it was not started elevated (`--allow-unsigned-engine`).
	CLI_CATALOG_EXIT_ENGINE_UNTRUSTED = 65,
};

//! One line for an exit code, the engine's or the front end's. Never NULL.
const char *
cli_catalog_exit_message(int code);

/*
 *
 * The engine, as the front end sees it.
 *
 */

enum cli_catalog_run_status
{
	CLI_CATALOG_RUN_OK = 0,      //!< the engine ran and exited; `exit_code` is valid
	CLI_CATALOG_RUN_FAILED,      //!< it could not be started
	CLI_CATALOG_RUN_TIMEOUT,     //!< it was still running when `timeout_ms` passed
	CLI_CATALOG_RUN_DECLINED,    //!< elevated start only: the user refused the prompt
	CLI_CATALOG_RUN_UNSUPPORTED, //!< elevated start only: this platform has none
};

//! One engine invocation.
struct cli_catalog_run
{
	const char *exe;         //!< the engine's path
	const char *const *args; //!< its arguments, without argv[0]
	int n_args;
	uint32_t timeout_ms;  //!< the bound on the whole wait
	bool capture_stdout;  //!< collect stdout into `out` instead of passing it through
	bool kill_on_timeout; //!< a read-only call may be ended; an install is left running

	char *out;     //!< captured stdout, NUL-terminated, `free()`d by the caller. NULL when not captured
	int exit_code; //!< valid when the status is CLI_CATALOG_RUN_OK
};

/*!
 * What the front end needs from its host. The CLI fills it with the functions
 * of cli_catalog_os.c; the unit test with its own.
 */
struct cli_catalog_ops
{
	void *ud;

	//! The engine's path, or NULL when there is none; then `engine_missing` says why.
	const char *engine;
	const char *engine_missing;

	//! Run the engine as this process's child, with this process's rights. stderr passes through.
	enum cli_catalog_run_status (*run)(void *ud, struct cli_catalog_run *r);

	//! Run the engine elevated (one consent prompt). Its output cannot be captured.
	enum cli_catalog_run_status (*run_elevated)(void *ud, struct cli_catalog_run *r);

	//! A person is at the terminal (stdin and stderr are terminals).
	bool (*is_interactive)(void *ud);

	/*!
	 * The Authenticode signer of @p path, or of this executable when @p path
	 * is NULL: true and its name in @p signer when the file carries a valid
	 * signature that chains to a trusted root. NULL: nothing can be verified,
	 * and the engine is then not started elevated without
	 * `--allow-unsigned-engine`.
	 */
	bool (*signer)(void *ud, const char *path, char *signer, size_t cap);

	/*!
	 * Create an empty file only this user (and administrators) can open, in
	 * the user's temporary directory, for the elevated engine's `--log`.
	 * Its path goes to @p path. May be NULL: then nothing is relayed.
	 */
	bool (*log_create)(void *ud, char *path, size_t cap);

	//! Read that file (the caller `free()`s the text; NULL when empty) and delete it.
	char *(*log_take)(void *ud, const char *path);

	/*!
	 * The licence state of the installed component @p registered_id of catalog
	 * class @p item_class, as its own class verb reports it (`<class> list
	 * --json` → `licence`). Returns the `licence` object (the caller owns it)
	 * and fills @p cell with the human cell; NULL when the class has no
	 * licence surface or the component is not registered. May be NULL.
	 */
	struct cJSON *(*licence)(void *ud, const char *item_class, const char *registered_id, char *cell, size_t cap);

	FILE *out; //!< results (stdout)
	FILE *err; //!< messages (stderr)
};

/*
 *
 * The verbs.
 *
 */

enum cli_catalog_verb
{
	CLI_CATALOG_VERB_NONE = 0,
	CLI_CATALOG_VERB_LIST,
	CLI_CATALOG_VERB_SHOW,
	CLI_CATALOG_VERB_INSTALL,
	CLI_CATALOG_VERB_UNINSTALL,
	CLI_CATALOG_VERB_UPDATE,
	CLI_CATALOG_VERB_OUTDATED,
};

#define CLI_CATALOG_MAX_IDS 32

//! A parsed `catalog …` command line.
struct cli_catalog_request
{
	enum cli_catalog_verb verb;
	const char *verb_name;
	bool json;
	bool elevate;        //!< `--elevate`: start the engine elevated even on a `--json` / non-interactive call
	bool no_elevate;     //!< `--no-elevate`: never start it elevated
	bool allow_runtime;  //!< `--allow-runtime`: reserved; refused with exit 64 until the engine can do it
	bool allow_unsigned_engine; //!< `--allow-unsigned-engine`: start an unsigned engine elevated (development)
	bool installed;      //!< `list --installed`
	bool use_default;    //!< `install --default`
	bool all;            //!< `update --all`
	const char *catalog; //!< `--catalog <url|file>`, handed to the engine as it is
	uint32_t timeout_s;  //!< `--timeout <seconds>`; 0 = the verb's default
	const char *ids[CLI_CATALOG_MAX_IDS];
	int n_ids;
};

/*!
 * Parse `argv[2..]` (argv[1] is `catalog`). False on a malformed call, with
 * the reason in @p err.
 */
bool
cli_catalog_parse(int argc, const char **argv, struct cli_catalog_request *out, char *err, size_t err_cap);

//! How long a verb waits for the engine, in milliseconds, honouring `--timeout`.
uint32_t
cli_catalog_timeout_ms(const struct cli_catalog_request *req, bool changes_machine);

/*!
 * The engine arguments of one `install` / `uninstall` call, in @p args
 * (at most @p cap, NULL-terminated when room): @p n_ids items, or none for
 * `install --default`. @p log_path: `--log <file>`. Returns the count.
 */
int
cli_catalog_engine_args(const struct cli_catalog_request *req,
                        const char *engine_verb,
                        const char *const *ids,
                        int n_ids,
                        bool no_runtime,
                        bool json,
                        const char *log_path,
                        const char **args,
                        int cap);

/*!
 * May the engine be started elevated? It must carry a valid Authenticode
 * signature whose signer is one of the first-party signers `dxr-get` itself
 * trusts (get_signers.h), and when this CLI is signed too, the CLI's signer.
 * False with the reason in @p why. `--allow-unsigned-engine` is the caller's
 * to honour, and the only way around it.
 */
bool
cli_catalog_engine_trusted(
    bool engine_signed, const char *engine_signer, bool self_signed, const char *self_signer, char *why, size_t cap);

/*!
 * One line of an engine log: `result: <id>: <exit code> <word>`, what
 * dxr-get writes at the end of each item of a several-item run. Returns 1
 * and fills @p id and @p code, or 0 for any other line.
 */
int
cli_catalog_parse_result_line(const char *line, char *id, size_t id_cap, int *code);

/*!
 * What the user should expect after an item of @p item_class was installed
 * or removed: which processes have re-read their components and which have
 * not. NULL when nothing in the runtime is affected (an application).
 */
const char *
cli_catalog_expectation(const char *item_class);

/*!
 * Quote one argument for a Windows command line, by the rules
 * `CommandLineToArgvW` and the C runtime read it back with. Pure string code,
 * so the host test covers it. Returns false when @p cap is too small.
 */
bool
cli_catalog_quote_arg(const char *arg, char *out, size_t cap);

void
cli_catalog_print_usage(FILE *f);

/*!
 * Run a parsed request. Returns the process exit code: the engine's, or one
 * of @ref cli_catalog_exit.
 */
int
cli_catalog_execute(const struct cli_catalog_ops *ops, const struct cli_catalog_request *req);

//! Parse and run: `displayxr-cli catalog …` with the given host.
int
cli_catalog_main(const struct cli_catalog_ops *ops, int argc, const char **argv);

/*
 *
 * The host (cli_catalog_os.c).
 *
 */

/*!
 * Find the installed engine: next to this executable, then in the runtime's
 * install directory (`HKLM\Software\DisplayXR\Runtime\InstallPath`). False
 * with the reason in @p why when there is none; always false on macOS and
 * Linux, which have no engine yet (ADR-064 step 5).
 */
bool
cli_catalog_os_locate_engine(char *out, size_t cap, char *why, size_t why_cap);

enum cli_catalog_run_status
cli_catalog_os_run(void *ud, struct cli_catalog_run *r);

enum cli_catalog_run_status
cli_catalog_os_run_elevated(void *ud, struct cli_catalog_run *r);

bool
cli_catalog_os_is_interactive(void *ud);

bool
cli_catalog_os_signer(void *ud, const char *path, char *signer, size_t cap);

bool
cli_catalog_os_log_create(void *ud, char *path, size_t cap);

char *
cli_catalog_os_log_take(void *ud, const char *path);

#ifdef __cplusplus
}
#endif
