// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Locally built models, the machine half (conversion-provider-discovery.md
 *         §5.6.3 / §5.6.4): the per-user licence acceptance record, the
 *         builder's registration and its provenance under the stricter rule.
 *
 * conv_model_store_build() (conversion/conv_model_build.h) runs the sequence;
 * this file fills its conv_build_ops from the real machine. The service and
 * the satellite use only target_conv_licence_accepted() (the hand-out rule);
 * nothing here builds.
 *
 * @ingroup targets_common
 */

#pragma once

#include "conversion/conv_model_build.h"
#include "target_conv_status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! HKCU subkey of the acceptance records (Windows), under TARGET_CONV_REG_ROOT.
#define TARGET_CONV_LICENCE_SUBKEY "LicenceAccepted"
//! POSIX: `<prefix><licence-id>` in the per-user dir, beside TARGET_CONV_RESEARCH_FILE.
#define TARGET_CONV_LICENCE_FILE_PREFIX "conversion-licence-"
//! Windows: the builders' registration root under HKLM (64-bit view).
#define TARGET_CONV_BUILDERS_REG_ROOT "Software\\DisplayXR\\ModelBuilders"

/*
 *
 * The acceptance record (§5.6.4): per user, per licence id, per text hash.
 * Windows: HKCU\Software\DisplayXR\ConversionProviders\LicenceAccepted, value
 * <licence-id> (REG_SZ) = "<text_sha256> <unix-seconds>". Linux / macOS: the
 * file conversion-licence-<licence-id> in the §3.5 per-user dir, holding the
 * same text and a newline. Never written by an installer; no machine tier.
 *
 */

//! Parse a record's text ("<64 hex> <seconds>"). False when it is not one.
bool
target_conv_licence_parse(const char *text, char *sha_out, size_t sha_size, uint64_t *when_out);

//! This user's record for @p licence_id. False when there is none (or it is malformed).
bool
target_conv_licence_read(const char *licence_id, char *sha_out, size_t sha_size, uint64_t *when_out);

//! Write (replace) this user's record: @p text_sha256 accepted now.
bool
target_conv_licence_write(const char *licence_id, const char *text_sha256);

//! Delete this user's record. True when none is left (it was deleted, or there was none).
bool
target_conv_licence_withdraw(const char *licence_id);

//! conv_store_licence_fn / conv_build_ops::licence_accepted: a record with this text hash exists.
bool
target_conv_licence_accepted(void *user, const char *licence_id, const char *text_sha256);

//! conv_build_ops::licence_record: target_conv_licence_write().
bool
target_conv_licence_record(void *user, const char *licence_id, const char *text_sha256);

/*
 *
 * The builder (§5.6.3 steps 3-4).
 *
 */

/*!
 * conv_build_ops::find_tool. In order: the dev override
 * `DXR_MODEL_BUILDER_<ID>=<path>` (only with DXR_ALLOW_DEV_PLUGIN_PATHS on a
 * non-elevated process); Windows HKLM\Software\DisplayXR\ModelBuilders\<id>
 * (`Binary`, `Version` -- written last, so a row without it is not complete);
 * POSIX `<id>.json` in /usr/share/displayxr/model-builders/ (Linux) or
 * /Library/Application Support/DisplayXR/ModelBuilders/ (macOS), which must
 * be root-owned up to "/".
 */
bool
target_conv_build_find_tool(void *user, const char *tool_id, struct conv_build_tool *out);

/*!
 * The §5.6.3 step 4 rule over a provenance verdict, pure (tests pin it).
 * Windows: SIGNED by a DXR_FIRST_PARTY_SIGNERS name, or ALLOWLISTED. POSIX:
 * PLATFORM_TRUSTED or ALLOWLISTED. Everywhere: DEV_OVERRIDE only for a
 * builder found through the dev override. Whatever Trust\Enforce says.
 */
bool
target_conv_build_trust_rule(
    uint32_t provenance, const char *signer, bool windows, bool found_by_dev_override, char *why, size_t why_size);

//! conv_build_ops::tool_trusted: the shared classifier, then target_conv_build_trust_rule().
bool
target_conv_build_tool_trusted(void *user, const struct conv_build_tool *tool, struct conv_build_trust *out);

/*!
 * Fill @p ops for this machine and user: steps 1-4 from @p policy (plus the
 * per-run opt-in @p per_run_research), the acceptance records, the builder
 * lookup and provenance. on_event / machine stay NULL for the caller.
 */
void
target_conv_build_ops_fill(struct conv_build_ops *ops, const struct target_conv_policy *policy, bool per_run_research);

#ifdef __cplusplus
}
#endif
