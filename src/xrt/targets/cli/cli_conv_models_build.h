// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `conv models` for locally built models (conversion-provider-discovery.md
 *         §5.6): fetch's build path, `cancel`, `remove`, `license`, and the
 *         `list --json` fields the Dashboard row reads. C++ only.
 * @ingroup cli
 */

#pragma once

#include "conversion/conv_model_store.h"

#include <string>
#include <vector>

struct cJSON;
struct target_conv_policy;

//! Every value of a repeatable `--<name> <v>` flag (argv[3..]).
std::vector<std::string>
cli_flag_values(int argc, const char **argv, const char *name);

/*!
 * A `<pack>` argument: a manifest path / URL as it is, or the id of a
 * registered pack (its manifest path). False when it is neither.
 */
bool
cli_conv_resolve_pack(const char *arg, std::string &out);

/*!
 * Build @p names (buildable models of @p pack, named by --model) through the
 * §5.6.3 sequence. Appends one entry per model to @p arr (JSON / --events),
 * else prints a line each. @p events: forward the NDJSON stream to stdout.
 * Returns how many failed.
 */
int
cli_conv_build_models(conv_model_store *s,
                      const conv_pack *pack,
                      const std::vector<std::string> &names,
                      int argc,
                      const char **argv,
                      bool json,
                      bool events,
                      cJSON *arr);

//! `conv models cancel <pack> --model <name> [--json]`.
int
cli_conv_models_cancel(int argc, const char **argv, conv_model_store *s, bool json);

//! `conv models remove <pack> --model <name> [--json]`.
int
cli_conv_models_remove(int argc, const char **argv, conv_model_store *s, bool json);

//! `conv models license accept|withdraw|status <licence-id> [--pack <pack>] [--json]`.
int
cli_conv_models_license(int argc, const char **argv, conv_model_store *s, bool json);

//! `list --json`: a buildable model's `local_build` object and `license_accepted`.
void
cli_conv_models_list_model(cJSON *e, const conv_pack *p, const conv_pack_model *m, conv_model_store *s);

//! `list --json`: the root's `research` and `machine` objects (the Dashboard's Build checks).
void
cli_conv_models_list_root(cJSON *root, conv_model_store *s, const target_conv_policy *policy);
