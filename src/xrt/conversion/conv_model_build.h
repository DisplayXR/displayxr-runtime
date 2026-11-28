// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Locally built models: the store runs a signed builder on the
 *         user's explicit request (conversion-provider-discovery.md §5.6,
 *         ADR-057 amendment 2026-11-27).
 *
 * `displayxr-cli conv models fetch <pack> --model <name>` for a model whose
 * `local_build` names a `tool` and `sources` runs conv_model_store_build():
 *
 *   1. policy      the model is non-commercial: AllowResearchOnly (or the
 *                  per-run opt-in) must be on            -> policy / policy-locked
 *   2. licence     every source licence's text hashes to the pack's
 *                  text_sha256 (build-license-text), and this user accepted it
 *                  (or --accept-license names it, which records it first)
 *                                                        -> build-license
 *   3. builder     found by tool.id (registry / root-owned manifest / dev env)
 *                                                        -> build-tool-missing / -old
 *   4. provenance  the stricter-than-load rule, then `describe --json` must
 *                  agree with the pack                   -> build-tool-untrusted / -mismatch
 *   5. resources   free space, physical memory, OS       -> build-disk / -memory / -unsupported
 *   6. staging     <root>/staging/<nonce>/{source,work,out}, one build per
 *                  recipe per store (<root>/staging/<recipe>.lock) -> busy
 *   7. sources     the store's downloader, size + sha256 -> build-download / bad-download
 *   8. run         argv, no shell, minimal environment, its own job object /
 *                  process group, 10-minute no-output watchdog, cancel file
 *                                                        -> build-* by exit code
 *   9. ingest      every regular file in out/ is hashed; a (sha256, size)
 *                  match moves into blobs/<sha256> and is stamped last
 *                                                        -> built / build-output-mismatch
 *
 * Steps 1-4 read machine and user state the conversion library does not
 * own (policy, the per-user acceptance record, the builder registration and
 * the provenance classifier): the caller passes them in conv_build_ops, so
 * the sequence is one function the tests drive with fakes. Nothing here is
 * reachable from model_request: the service and the satellites never build.
 *
 * Not on Android (no builder there, §5.6.7): every call reports unsupported.
 *
 * @ingroup aux_conversion
 */

#pragma once

#include "conv_model_store.h"

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! The result of building one model. Stable names: conv_build_result_str().
enum conv_build_result
{
	CONV_BUILD_BUILT = 0,       //!< "built": built and stamped
	CONV_BUILD_PRESENT,         //!< "ok": every file was already in the store (no build ran)
	CONV_BUILD_NOT_BUILDABLE,   //!< "not-buildable": no such model, or its local_build names no tool
	CONV_BUILD_POLICY,          //!< "policy": research-only and AllowResearchOnly is off
	CONV_BUILD_POLICY_LOCKED,   //!< "policy-locked": HKLM Policy\Lock with HKLM AllowResearchOnly = 0
	CONV_BUILD_LICENSE_TEXT,    //!< "build-license-text": the licence text beside the pack is not the pinned one
	CONV_BUILD_LICENSE,         //!< "build-license": no acceptance record for a source licence
	CONV_BUILD_TOOL_MISSING,    //!< "build-tool-missing"
	CONV_BUILD_TOOL_OLD,        //!< "build-tool-old": registered Version below tool.min_version
	CONV_BUILD_TOOL_UNTRUSTED,  //!< "build-tool-untrusted": provenance verdict refused
	CONV_BUILD_TOOL_MISMATCH,   //!< "build-tool-mismatch": `describe --json` disagrees with the pack
	CONV_BUILD_DISK,            //!< "build-disk"
	CONV_BUILD_MEMORY,          //!< "build-memory"
	CONV_BUILD_UNSUPPORTED,     //!< "build-unsupported"
	CONV_BUILD_BUSY,            //!< "busy": another live process builds this recipe (no-wait mode)
	CONV_BUILD_DOWNLOAD,        //!< "build-download": a source could not be fetched (retryable)
	CONV_BUILD_BAD_DOWNLOAD,    //!< "bad-download": a source's size / sha256 is wrong
	CONV_BUILD_SOURCE_MISMATCH, //!< "build-source-mismatch": builder exit 12
	CONV_BUILD_OUTPUT_MISMATCH, //!< "build-output-mismatch": exit 13, or out/ lacks one of the model's files
	CONV_BUILD_CANCELLED,       //!< "build-cancelled": exit 17, the cancel file, or an interrupt
	CONV_BUILD_STALLED,         //!< "build-stalled": no output for the watchdog period; killed
	CONV_BUILD_FAILED,          //!< "build-failed": any other exit, or the builder did not start
	CONV_BUILD_IO,              //!< "io": the staging directory / lock / store could not be written
};

const char *
conv_build_result_str(enum conv_build_result r);

//! A registered builder (§5.6.3 step 3).
struct conv_build_tool
{
	char id[64];
	char binary[CONV_STORE_PATH_MAX]; //!< absolute
	char version[32];                 //!< the registered `Version` ("" = unknown: the dev override)
	char display_name[128];
	char source[CONV_STORE_PATH_MAX]; //!< where it was found: registry key, manifest path, env var name
	bool dev_override;                //!< found through DXR_MODEL_BUILDER_<ID> (the dev rule)
};

//! The provenance verdict of a builder (§5.6.3 step 4).
struct conv_build_trust
{
	char provenance[32]; //!< target_component_provenance_str() of the verdict
	char signer[128];    //!< leaf CN when signed
	char reason[256];    //!< why, one line
};

//! What the machine offers a build (§5.6.3 step 5).
struct conv_build_machine
{
	bool phys_known;
	uint64_t phys_mb; //!< physical memory
	bool free_known;
	uint64_t free_mb;    //!< free space on the store's volume
	char os[16];         //!< "windows" | "macos" | "linux"
	char os_version[32]; //!< dotted ("10.0.19045", "15.3", "6.8.0")
};

/*!
 * Everything steps 1-5 and the event stream need from outside the library.
 * Every function pointer may be NULL: a NULL licence / tool / trust function
 * fails its step closed; a NULL @ref machine reads the real machine.
 */
struct conv_build_ops
{
	void *user;
	//! Step 1: AllowResearchOnly as resolved (policy, or the per-run --allow-research-only).
	bool allow_research_only;
	//! Step 1: HKLM Policy\Lock with HKLM AllowResearchOnly = 0 (nothing can turn it on).
	bool research_locked_off;
	//! Step 2: does this user have an acceptance record for this licence + text hash?
	bool (*licence_accepted)(void *user, const char *licence_id, const char *text_sha256);
	//! Step 2: write the acceptance record (`--accept-license`). False = could not.
	bool (*licence_record)(void *user, const char *licence_id, const char *text_sha256);
	//! Step 3: look the builder up by tool id. False = not registered.
	bool (*find_tool)(void *user, const char *tool_id, struct conv_build_tool *out);
	//! Step 4: the provenance verdict under §5.6.3's stricter rule. False = refused.
	bool (*tool_trusted)(void *user, const struct conv_build_tool *tool, struct conv_build_trust *out);
	//! Step 5 (tests): the machine; NULL = conv_build_machine_read().
	bool (*machine)(void *user, const char *store_root, struct conv_build_machine *out);
	/*!
	 * One NDJSON line (no newline): every builder line forwarded unchanged
	 * (a JSON object with "v":1 and "event"; anything else on its stdout is
	 * dropped) and the store's own `phase` / `progress` lines
	 * (`download-source`, `ingest`). Called on the building thread.
	 */
	void (*on_event)(void *user, const char *line);
};

struct conv_build_options
{
	const char *const *accept_licences; //!< `--accept-license <id>` values (recorded first, step 2)
	uint32_t accept_count;
	bool no_wait;                 //!< report `busy` instead of waiting for another build's lock (--events)
	uint32_t stall_ms;            //!< no-output watchdog; 0 = 10 minutes
	uint32_t cancel_grace_ms;     //!< after the cancel file, kill the tree this much later; 0 = 10 s
	uint32_t describe_timeout_ms; //!< `describe --json`; 0 = 60 s
	uint32_t lock_poll_ms;        //!< waiting on another build's lock; 0 = 1 s
	const volatile sig_atomic_t *interrupt; //!< non-zero = cancel (SIGINT / Ctrl+C in the CLI)
	char log_dir[CONV_STORE_PATH_MAX];      //!< builder stderr; "" = the per-user scratch dir "model-builder"
};

//! Defaults: everything zero (the documented defaults apply).
void
conv_build_options_defaults(struct conv_build_options *opt);

//! The outcome for one requested model.
struct conv_build_model_result
{
	char name[64];
	enum conv_build_result result;
	char hint[320]; //!< one line for the user ("" = none)
	//! A build ran (or was attempted past step 5) for this model: the fields below are set.
	bool ran;
	char tool[64];
	char tool_version[32];
	char provenance[32];
	char staging[CONV_STORE_PATH_MAX];
	char log[CONV_STORE_PATH_MAX];
	int32_t exit_code; //!< the builder's exit code (-1 = did not exit by itself / never started)
};

/*!
 * Build models @p models (names in @p p) into @p s: the §5.6.3 sequence.
 * Models that share a recipe and a tool are built in one run; groups run one
 * after the other. @p out has @p count entries, one per requested name, in
 * order. Returns how many did not end `built` / `ok`.
 */
uint32_t
conv_model_store_build(struct conv_model_store *s,
                       const struct conv_pack *p,
                       const char *const *models,
                       uint32_t count,
                       const struct conv_build_ops *ops,
                       const struct conv_build_options *opt,
                       struct conv_build_model_result *out);

/*!
 * `conv models cancel`: create the cancel file of the live build that holds
 * @p pack / @p model. False when no live build has it.
 */
bool
conv_model_store_build_cancel(struct conv_model_store *s, const char *pack, const char *model);

/*!
 * Delete staging directories no live lock names, older than @p max_age_s and
 * written by this user (and stale lock files of dead processes). Returns how
 * many directories were deleted. The next build and `conv models prune` run it.
 */
uint32_t
conv_model_store_clean_staging(struct conv_model_store *s, uint64_t max_age_s);

/*!
 * `conv models remove`: delete the blobs (and stamps) of model @p model of
 * pack @p pack that no other model of a pack registered in @p s references.
 * Returns how many blobs were deleted; @p bytes_out their size. Never touches
 * AllowResearchOnly or an acceptance record.
 */
uint32_t
conv_model_store_remove_model(struct conv_model_store *s, const char *pack, const char *model, uint64_t *bytes_out);

/*
 *
 * Pure helpers (tests pin them).
 *
 */

//! Dotted numeric compare ("1.10.0" > "1.9"); missing parts are 0, a non-digit ends a part.
int
conv_build_version_cmp(const char *a, const char *b);

/*!
 * Does `needs.os` entry @p entry ("windows>=10.0.19041", "linux", "macos>=14")
 * admit OS @p os at version @p version? Only `>=` is defined.
 */
bool
conv_build_os_matches(const char *entry, const char *os, const char *version);

//! The real machine: physical memory, free space at (the nearest existing parent of) @p root, OS + version.
bool
conv_build_machine_read(const char *root, struct conv_build_machine *out);

/*!
 * Parse a POSIX builder manifest (`{"id","version","binary"}`, §5.6.3 step 3).
 * False when it is not that shape, the id is not @p want_id, or the binary is
 * not absolute.
 */
bool
conv_build_tool_manifest_parse(const char *json, const char *want_id, struct conv_build_tool *out);

/*!
 * Check `describe --json` output against the pack (§5.6.2): v = 1, the tool
 * id, version >= tool.min_version, the recipe with every requested variant,
 * and the same source pins (role, sha256, size, license_id). False with
 * @p why on any disagreement.
 */
bool
conv_build_describe_check(const char *json,
                          const struct conv_pack_build *b,
                          const char *const *variants,
                          uint32_t variant_count,
                          char *why,
                          size_t why_size);

/*!
 * Map a builder exit code to a result (§5.6.3 step 8): 0 BUILT (pending
 * ingest), 10 LICENSE, 11 DOWNLOAD, 12 SOURCE_MISMATCH, 13 OUTPUT_MISMATCH,
 * 14 MEMORY, 15 DISK, 16 UNSUPPORTED, 17 CANCELLED, anything else FAILED.
 */
enum conv_build_result
conv_build_result_of_exit(int32_t code);

//! `DXR_MODEL_BUILDER_<ID>`: upper case, '-' and '.' -> '_'. False when it does not fit.
bool
conv_build_dev_env_name(const char *tool_id, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
