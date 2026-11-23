// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  dxr-get core: the catalog model, the plan and the trust decisions,
 *         with no operating-system calls (ADR-064 steps 2 and 3).
 *
 * Everything the machine contributes (registry values, files, services,
 * monitor EDIDs, install receipts, the download cache) comes in through
 * @ref dxr_env, so this file builds and is unit-tested on every platform
 * (tests/tests_get_catalog.cpp) while dxr-get itself stays Windows-only.
 *
 * One model for both catalog schemas: a schema-1 document is converted to
 * the schema-2 shape when it is parsed, so the plan never branches on the
 * schema and no component name appears outside that one conversion.
 *
 * Spec: docs/specs/runtime/dxr-get.md, docs/specs/runtime/component-catalog.md.
 */

#pragma once

#include "get.h"

#include <stdio.h>
#include <time.h>

#include "cjson/cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

//! The one platform this engine installs for. Other platforms' assets are ignored.
#define DXR_PLATFORM "windows-x64"

#define DXR_MAX_STEPS 32
#define DXR_MAX_PREREQS 32

/*
 * Small helpers.
 */

//! Dotted numeric compare, up to 4 parts; anything after '-' or '+' is ignored.
int
dxr_vercmp(const char *a, const char *b);

//! ">=2.62.0" -> "2.62.0". NULL when @p req is NULL or empty.
const char *
dxr_floor_of(const char *req);

//! "YYYY-MM-DDTHH:MM:SSZ" -> unix seconds, 0 when malformed.
time_t
dxr_parse_utc(const char *s);

/*
 * Install receipt (HKLM\Software\DisplayXR\Catalog\<id> on Windows).
 */
struct dxr_receipt
{
	char id[96];
	char name[128];
	char item_class[40];
	char publisher[64];
	char version[64];
	char source_url[1024];
	char sha256[65];
	char file[260];
	char install_time[32];  //!< UTC, "YYYY-MM-DDTHH:MM:SSZ"
	char uninstall[1024];   //!< complete command line, "" when the catalog gave none
	char version_path[512]; //!< registry value that holds the installed version, "" when unknown
	char requires_ids[512]; //!< comma-separated catalog ids this item required
};

/*
 * What the machine contributes. Any callback may be NULL: it then answers
 * "absent" / "no".
 */
struct dxr_env
{
	void *user;
	//! REG_SZ named as `HKLM\Sub\Key\Value` (64-bit view).
	bool (*reg_sz)(void *user, const char *full_path, char *out, size_t out_size);
	//! DWORD (or decimal REG_SZ) under an `HKLM\...` key.
	bool (*reg_dword)(void *user, const char *key, const char *value, uint32_t *out);
	bool (*reg_key_exists)(void *user, const char *key);
	bool (*file_exists)(void *user, const char *path);
	bool (*service_exists)(void *user, const char *name);
	int (*edids)(void *user, struct dxr_edid_id *out, int max);
	//! Present USB devices' VID:PIDs (the `usb` detect rule, #2737).
	int (*usb)(void *user, struct dxr_usb_id *out, int max);
	bool (*receipt)(void *user, const char *id, struct dxr_receipt *out);
	//! Bytes of this asset already in the download cache (complete or partial).
	uint64_t (*cached_bytes)(void *user, const char *sha256, const char *file, uint64_t size);

	/*!
	 * `--assume-runtime X.Y.Z`: answer as if this runtime version were the one
	 * installed, whatever the machine has. For a caller that is about to lay
	 * that runtime down itself (the runtime installer) and wants to know what
	 * is compatible with it. NULL = read the machine.
	 */
	const char *assume_runtime;
	//! `--assume-abi <class>=<major>`: the class ABI majors of the assumed runtime.
	struct
	{
		const char *item_class;
		int major;
	} assume_abi[8];
	int n_assume_abi;
};

/*
 * Catalog.
 */
struct dxr_catalog
{
	cJSON *doc;   //!< the document as fetched (what `dxr-get catalog` prints)
	cJSON *model; //!< schema-2 shape; == doc for schema 2, converted (and owned) for schema 1
	int schema;   //!< 1 or 2: what was fetched
	long long serial;
	const cJSON *items;
	const cJSON *publishers;
	const cJSON *revoked;
};

/*!
 * Parses a catalog body. Returns DXR_GET_OK, or DXR_GET_OFFLINE with @p err
 * filled when the document is unparsable, of an unknown schema, or expired
 * (an expired catalog is never an authority). The signature and the serial
 * are the caller's to check: they need the key list and the machine.
 */
int
dxr_catalog_parse(const char *body, size_t len, time_t now, struct dxr_catalog *c, char *err, size_t err_size);

void
dxr_catalog_free(struct dxr_catalog *c);

//! Item by `id` or by one of its `aliases`. NULL when unknown or of a class this build does not know.
const cJSON *
dxr_item_find(const struct dxr_catalog *c, const char *name);

const char *
dxr_item_id(const cJSON *item);

//! The name a schema-1 caller used: the first alias, else the id.
const char *
dxr_item_legacy_name(const cJSON *item);

const char *
dxr_item_label(const cJSON *item);

const char *
dxr_item_class(const cJSON *item);

//! The installable asset for this platform; NULL when absent or listing-only.
const cJSON *
dxr_item_asset(const cJSON *item);

const cJSON *
dxr_runtime_item(const struct dxr_catalog *c);

//! true for the classes the service or the runtime loads (everything but `app` and `sdk`).
bool
dxr_class_is_loaded(const char *item_class);

//! Registry value holding the item's installed version. false when the catalog does not say.
bool
dxr_installed_path(const cJSON *item, char *out, size_t out_size);

//! Installed version: the catalog's rule, else the receipt. false when not installed (or unknowable).
bool
dxr_installed_version(const struct dxr_env *env, const cJSON *item, char *out, size_t out_size);

/*!
 * May this asset be installed? Checks, without touching the file: it is not
 * revoked, it carries an Authenticode signature, its installer kind is one
 * this engine runs, and its signer is delegated to the item's publisher.
 * With @p actual_cn (the certificate subject WinVerifyTrust reported) it also
 * checks that the file was signed by exactly that signer.
 *
 * Returns DXR_GET_OK, DXR_GET_NOT_FETCHABLE (nothing installable here) or
 * DXR_GET_VERIFY (a trust rule refuses it); @p reason says why.
 */
int
dxr_asset_check(const struct dxr_catalog *c,
                const cJSON *item,
                const cJSON *asset,
                const char *actual_cn,
                char *reason,
                size_t reason_size);

//! Evaluates detect rules; true on the first match, with the evidence in @p why.
bool
dxr_detect_rules(const cJSON *rules, const struct dxr_env *env, bool high_only, char *why, size_t why_size);

/*
 * Plan.
 */
enum dxr_state
{
	DXR_ST_OK,
	DXR_ST_MISSING,
	DXR_ST_TOO_OLD,
	DXR_ST_NOT_FETCHABLE,
	DXR_ST_INCOMPATIBLE,
};

enum dxr_step_kind
{
	DXR_STEP_ITEM,
	DXR_STEP_EXTERNAL, //!< an external prerequisite that carries its own installer asset
};

struct dxr_step
{
	enum dxr_step_kind kind;
	const cJSON *item;   //!< the catalog item (for an external step: the item that requires it)
	const cJSON *asset;  //!< what would be fetched; NULL when nothing is
	char component[128]; //!< schema-1 name (or the prerequisite's name)
	char id[96];
	char label[128];
	char installed[64]; //!< "" when absent
	enum dxr_state state;
	char required[64]; //!< ">=X.Y.Z", "" when none
	const char *target;
	double size;     //!< bytes of the asset
	double download; //!< bytes still to fetch (size minus what the cache holds)
	bool needed;     //!< false for a detected-hardware item: its failure does not fail the plan
	bool is_target;  //!< the item `install` was asked for
	bool current;    //!< installed at exactly the catalog's version
	bool rebuild;    //!< same version, different asset hash than the one installed
	int hash_match;  //!< -1 unknown (no receipt), 0 differs, 1 equal
	char reason[320];
};

struct dxr_prereq
{
	char name[128];
	char for_id[128];
	bool met;
	bool installable; //!< carries an asset for this platform
	char info_url[512];
	char evidence[256];
};

enum dxr_plan_mode
{
	DXR_PLAN_ENSURE,  //!< the prerequisites of X (the caller installs X itself), plus detected hardware items
	DXR_PLAN_INSTALL, //!< X and its prerequisites
};

struct dxr_plan_opts
{
	/*!
	 * `--no-runtime`: never install or upgrade the runtime itself. A plan that
	 * would need to is refused (DXR_GET_INCOMPATIBLE) instead. For the runtime
	 * installer, which must not start a second runtime installer inside itself.
	 */
	bool no_runtime;
};

struct dxr_plan
{
	const char *for_name;
	int schema;
	struct dxr_step steps[DXR_MAX_STEPS];
	int n;
	struct dxr_prereq prereqs[DXR_MAX_PREREQS];
	int n_prereqs;
	/*!
	 * DXR_GET_OK, DXR_GET_NOT_FETCHABLE (a needed component cannot be had) or
	 * DXR_GET_INCOMPATIBLE (a needed item's runtime range or class ABI
	 * excludes this machine's runtime).
	 */
	int gate;
	char gate_reason[320];
	double total_size;
	double total_download;
	int todo; //!< steps that need an installer run
};

void
dxr_make_plan(const struct dxr_catalog *c,
              const struct dxr_env *env,
              const char *for_name,
              const char *min_runtime,
              enum dxr_plan_mode mode,
              const struct dxr_plan_opts *opts,
              struct dxr_plan *p);

/*!
 * `install --default`: the catalog's recommended set plus every item whose
 * detect rules match this machine with high confidence, and what they
 * require. Each candidate is judged on its own first: one that cannot be
 * installed here (not compatible, nothing fetchable) is listed as skipped
 * and does not fail the others. The runtime item itself is never selected.
 */
void
dxr_make_default_plan(const struct dxr_catalog *c,
                      const struct dxr_env *env,
                      const struct dxr_plan_opts *opts,
                      struct dxr_plan *p);

static inline bool
dxr_step_needs_work(const struct dxr_step *s)
{
	return s->state == DXR_ST_MISSING || s->state == DXR_ST_TOO_OLD;
}

//! Caller frees with cJSON_free().
char *
dxr_plan_json(const struct dxr_plan *p);

void
dxr_plan_text(const struct dxr_plan *p, FILE *out);

/*
 * Inventory.
 */

/*!
 * `list`: every catalog item with its installed state, what installing it
 * would download, whether it is compatible with this machine's runtime, and
 * whether its detect rules match. @p outdated_only: `outdated`.
 * @p fixed_runtime: judge compatibility against the runtime as it is (or as
 * assumed), without counting on an upgrade of it.
 */
cJSON *
dxr_list_catalog(const struct dxr_catalog *c, const struct dxr_env *env, bool outdated_only, bool fixed_runtime);

/*!
 * `list --installed`: the receipts, no catalog needed. Each row carries the
 * receipt's `version` (what was installed) and, when the receipt recorded where
 * the component keeps its version and @p env can read it, `installed`: that
 * value now, or null when it is gone (removed by the component's own
 * uninstaller, which leaves the receipt behind).
 */
cJSON *
dxr_list_receipts(const struct dxr_env *env, const struct dxr_receipt *r, int n);

/*!
 * May `uninstall` run? @p r is the item's receipt (NULL: none), @p all every
 * receipt on the machine. Returns DXR_GET_OK, or the exit code with @p result
 * (the `--json` result word) and @p detail (what to do instead).
 *
 * Order: what elevation cannot fix first (no receipt: 42; no uninstall
 * command, or required by another installed item: 43), then elevation (50).
 * A caller is only told to elevate when elevating is all that is missing.
 */
int
dxr_uninstall_check(const struct dxr_receipt *r,
                    const struct dxr_receipt *all,
                    int n,
                    bool elevated,
                    const char **result,
                    char *detail,
                    size_t detail_size);

/*
 * Several items in one `install` / `uninstall` (so a front end asks for
 * elevation once). Each item is judged and run alone, in the order given:
 * one that cannot be installed or removed is reported and skipped, and the
 * rest proceed. One item on the command line is the single-item verb, with
 * its output unchanged.
 */

//! Most items one `install` / `uninstall` accepts.
#define DXR_MAX_IDS 32

//! What `install` does with a plan, before any download. The order is the single-item verb's.
enum dxr_install_verdict
{
	DXR_INSTALL_PROCEED,       //!< run the plan
	DXR_INSTALL_NOTHING,       //!< nothing to do: exit 0
	DXR_INSTALL_INCOMPATIBLE,  //!< exit 41
	DXR_INSTALL_NEEDS_ADMIN,   //!< exit 50
	DXR_INSTALL_NOT_FETCHABLE, //!< exit 40
};

/*!
 * Judge an `install` plan: not compatible (41) first, then nothing to do (0),
 * then elevation (50), then not fetchable (40). Elevation is asked for before
 * "not fetchable" is reported; that is the single-item verb's order and
 * deployed callers know it.
 */
enum dxr_install_verdict
dxr_install_judge(const struct dxr_plan *p, bool elevated);

/*!
 * The result word of one item of a several-item `install`: `installed`,
 * `nothing_to_do`, `incompatible`, `needs_elevation`, `not_fetchable`, or
 * `failed` (the plan ran and ended with @p code != 0).
 */
const char *
dxr_install_result_word(enum dxr_install_verdict v, int code);

/*!
 * One entry of a several-item `install --json`: `{"id","exit","result","plan"}`.
 * @p plan_json is the item's plan as @ref dxr_plan_json printed it (NULL: no
 * `plan` key).
 */
cJSON *
dxr_install_item_json(const char *id, int code, const char *result, const char *plan_json);

/*!
 * The document of a several-item `install` / `uninstall --json`:
 * `{"exit": <the first item exit code that is not 0, else 0>, "items": [...]}`.
 * Takes ownership of @p items; @p out_exit (optional) gets `exit`.
 */
cJSON *
dxr_multi_json(cJSON *items, int *out_exit);

//! Ids (from @p all) whose receipts say they required @p id. Returns the count, names in @p out.
int
dxr_receipt_dependents(const struct dxr_receipt *all, int n, const char *id, char *out, size_t out_size);

/*
 * Text that came from the command line or the environment.
 *
 * dxr-get may run elevated on behalf of a caller that is not, and what it
 * logs is read back by that caller's front end one line at a time. No
 * argument may therefore carry a control character (a newline in an item id
 * would forge a `result:` line), and nothing that is logged may either.
 */

//! true when @p arg holds no ASCII control character (0x00-0x1F, 0x7F). Bytes of a UTF-8 sequence pass.
bool
dxr_arg_ok(const char *arg);

/*!
 * Make @p text safe to log, in place: every ASCII control character except
 * the line feed and the tab becomes `?`. The length does not change.
 */
void
dxr_log_sanitize(char *text);

//! The verification-weakening switches a run honours. All false in every build that ships.
struct dxr_dev_switches
{
	bool no_verify_catalog; //!< `--no-verify-catalog` with `DXR_GET_DEV=1`: the catalog signature is not checked
	bool allow_unsigned;    //!< `DXR_GET_ALLOW_UNSIGNED=1`: the Authenticode check of a download is skipped
};

/*!
 * Decide which switches apply. They exist only in a TEST ROOT KEY build
 * (@p test_build; CMake DXR_GET_TEST_ROOT_KEY), which is never installed,
 * never attached to a release, and prints a banner on every run. In every
 * other build the environment variables are ignored and the argument is a
 * usage error: a signed dxr-get.exe is started elevated behind a consent
 * prompt that shows its publisher, and nothing the calling user can set may
 * make it install something the signed catalog does not name.
 *
 * Returns false (usage error, @p why says which) when `--no-verify-catalog`
 * was given and cannot be honoured. @p ignored (optional) gets a note when an
 * environment switch was set and ignored, else an empty string.
 */
bool
dxr_dev_switches_resolve(bool test_build,
                         bool arg_no_verify,
                         bool env_dev,
                         bool env_allow_unsigned,
                         struct dxr_dev_switches *out,
                         char *why,
                         size_t why_size,
                         char *ignored,
                         size_t ignored_size);

#ifdef __cplusplus
}
#endif
