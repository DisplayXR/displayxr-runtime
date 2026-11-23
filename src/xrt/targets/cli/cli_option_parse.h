// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pure pieces of the shared `options | set` verbs
 *         (`cli_component_options.c`), header-only so `tests_cli_option_parse`
 *         pins them without a registry, a manifest dir or a component:
 *
 *  - @ref cli_set_args_check: the `set <id> <KEY> <value>` / `set <id> <KEY>
 *    --stdin` argument rule (exactly one source for the value);
 *  - @ref cli_read_value_line: one value line from a stream (`--stdin`), the
 *    trailing newline stripped, an empty or over-long line refused.
 *
 * @ingroup cli
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 *
 * set: where the value comes from.
 *
 */

//! What @ref cli_set_args_check decided.
enum cli_set_args
{
	CLI_SET_ARGS_VALUE = 0,   //!< `set <id> <KEY> <value>`: the value is the third positional argument.
	CLI_SET_ARGS_STDIN = 1,   //!< `set <id> <KEY> --stdin`: the value is read from standard input.
	CLI_SET_ARGS_MISSING = 2, //!< Too few positional arguments for either form.
	CLI_SET_ARGS_BOTH = 3,    //!< A value argument AND `--stdin`: refused (exactly one source).
};

/*!
 * The `set` argument rule over the positional arguments after the verb
 * (@p positional, flags not counted) and whether `--stdin` was given.
 */
static inline enum cli_set_args
cli_set_args_check(int positional, bool from_stdin)
{
	if (from_stdin) {
		return positional >= 3   ? CLI_SET_ARGS_BOTH
		       : positional == 2 ? CLI_SET_ARGS_STDIN
		                         : CLI_SET_ARGS_MISSING;
	}
	return positional >= 3 ? CLI_SET_ARGS_VALUE : CLI_SET_ARGS_MISSING;
}


/*
 *
 * --stdin: one value line.
 *
 */

//! What @ref cli_read_value_line read.
enum cli_value_read
{
	CLI_VALUE_READ_OK = 0,       //!< A non-empty value is in the buffer.
	CLI_VALUE_READ_EMPTY = 1,    //!< Nothing before the newline / end of input.
	CLI_VALUE_READ_TOO_LONG = 2, //!< The line does not fit (buffer wiped).
	CLI_VALUE_READ_IO = 3,       //!< The stream failed (buffer wiped).
};

/*!
 * Read ONE line from @p f into @p out (@p cap bytes, so at most `cap - 1`
 * value bytes): everything up to the first `\n` or the end of input, with the
 * trailing `\n` and any `\r` before it stripped. Nothing after the first line
 * is read. Every byte of @p out past the value is zero, and on any failure the
 * whole buffer is wiped, so a secret never lingers in a refused buffer.
 */
static inline enum cli_value_read
cli_read_value_line(FILE *f, char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return CLI_VALUE_READ_IO;
	}
	memset(out, 0, cap);
	if (f == NULL) {
		return CLI_VALUE_READ_IO;
	}
	size_t n = 0;
	bool overflow = false;
	for (;;) {
		const int c = fgetc(f);
		if (c == EOF) {
			if (ferror(f)) {
				memset(out, 0, cap);
				return CLI_VALUE_READ_IO;
			}
			break;
		}
		if (c == '\n') {
			break;
		}
		if (n + 1 >= cap) {
			overflow = true; // keep consuming the line so nothing of it is left for a later read
			continue;
		}
		out[n++] = (char)c;
	}
	if (overflow) {
		memset(out, 0, cap);
		return CLI_VALUE_READ_TOO_LONG;
	}
	while (n > 0 && out[n - 1] == '\r') {
		out[--n] = '\0';
	}
	return n == 0 ? CLI_VALUE_READ_EMPTY : CLI_VALUE_READ_OK;
}

#ifdef __cplusplus
}
#endif
