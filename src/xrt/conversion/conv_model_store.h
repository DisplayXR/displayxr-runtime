// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The runtime-owned conversion model store (ADR-053 decision 7, P4).
 *
 * One machine-wide, content-addressed store of model blobs that every
 * conversion provider resolves its models from (host ops `model_resolve` /
 * `model_request`, xrt_conversion_plugin.h). Layout, write protocol and read
 * check are the browser's model-distribution contract, unchanged, so a store
 * the browser installer provisioned is adopted as is:
 *
 *     <root>/
 *       displayxr-runtime.owner       the runtime owns this root (the browser
 *                                     uninstaller must not delete blobs/)
 *       packs/<pack>.models.json      a pack this store provisioned (schema 2)
 *       blobs/<sha256>                model bytes; immutable, lowercase hex
 *       blobs/<sha256>.ok             stamp, written LAST, "<sha256> <size>\n"
 *       blobs/<sha256>.<nonce>.part   an in-flight / interrupted download
 *
 * Root, in this order (conversion-provider-discovery.md §5.1):
 *   Windows: HKLM\Software\DisplayXR\Models\Path, then the browser's
 *            HKLM\Software\DisplayXR\Browser\ModelsPath, then
 *            %ProgramData%\DisplayXR\Models.
 *   POSIX:   $XDG_DATA_HOME/displayxr/models (default ~/.local/share) when
 *            writable, else the packaged root /var/lib/displayxr/models.
 *   `DXR_CONV_MODELS_ROOT` (environment of the process that owns the store)
 *   overrides all of them: tests and dev boxes only.
 *
 * Write protocol (host, CLI `conv models fetch`, installer pre-fetch):
 * download to `<sha>.<nonce>.part`, check the size, then the sha256 (mismatch:
 * delete the part), rename without replace (target exists: another writer won,
 * delete ours and read-check theirs), stamp last.
 *
 * Load check: stamp + size is the browser's read check, and it is NOT trusted
 * here: the store's ACL lets a local user plant a same-size blob and its
 * stamp. The store re-hashes a blob the first time this process hands it out
 * (cached by file id + mtime + size). A mismatch is a tampered blob: it is
 * deleted (when the ACL lets us) and fetched again, or FAILED(MODEL).
 *
 * Threads: one worker per store does every blocking step (hash, download).
 * conv_model_store_resolve() never blocks on I/O beyond a stat of the blob
 * and its stamp, so it is safe from a provider's [C] slot.
 *
 * @ingroup aux_conversion
 */

#pragma once

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_conversion_plugin.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CONV_STORE_PATH_MAX 512
#define CONV_STORE_SHA_HEX 65     //!< 64 hex + NUL
#define CONV_STORE_MAX_PACKS 8
#define CONV_STORE_MAX_MODELS 32  //!< per pack
#define CONV_STORE_MAX_VARIANTS 6 //!< per model
#define CONV_STORE_MAX_FILES 4    //!< per variant
#define CONV_STORE_MAX_DEFAULTS 8 //!< `defaults` entries per pack
#define CONV_STORE_MAX_BLOBS 128  //!< distinct blobs tracked by one store

//! Owner marker the runtime (installer or store) writes at the root.
#define CONV_STORE_OWNER_MARKER "displayxr-runtime.owner"

/*
 *
 * Root resolution.
 *
 */

/*! Every input of the root order, injectable for tests (NULL or "" = unset). */
struct conv_store_root_inputs
{
	const char *env_override;        //!< DXR_CONV_MODELS_ROOT
	const char *models_path;         //!< HKLM\Software\DisplayXR\Models\Path
	const char *browser_models_path; //!< HKLM\Software\DisplayXR\Browser\ModelsPath
	const char *program_data;        //!< %ProgramData% (Windows)
	const char *xdg_data_home;       //!< $XDG_DATA_HOME, else $HOME/.local/share (POSIX)
	bool xdg_writable;               //!< the XDG root (or its nearest existing parent) is writable
	bool windows;                    //!< which order applies
};

/*!
 * The root from @p in, in the documented order. Writes a short name of the
 * winning source into @p source ("env", "HKLM Models\\Path", ...). False only
 * when nothing usable is set (Windows without %ProgramData%).
 */
bool
conv_store_root_from(const struct conv_store_root_inputs *in,
                     char *out,
                     size_t out_size,
                     char *source,
                     size_t source_size);

/*! The root for this process: real registry (64-bit view) / environment. */
bool
conv_store_root_resolve(char *out, size_t out_size, char *source, size_t source_size);


/*
 *
 * Pack manifests (schema 2, discovery spec §5.2).
 *
 */

struct conv_pack_file
{
	char role[16];   //!< "model" (default), "data", ...
	char format[16]; //!< "onnx", ...: the blob URL extension
	char sha256[CONV_STORE_SHA_HEX];
	uint64_t size;
};

struct conv_pack_variant
{
	char id[48];
	char backend[48];
	uint32_t file_count;
	struct conv_pack_file files[CONV_STORE_MAX_FILES];
};

struct conv_pack_model
{
	char name[64];
	char version[32];
	char task[32];
	char spdx[48];
	bool commercial; //!< AND over the model and every dependency (missing = false)
	bool prefetch;   //!< in the installer's default pre-fetch set
	uint32_t variant_count;
	struct conv_pack_variant variants[CONV_STORE_MAX_VARIANTS];
};

struct conv_pack_default
{
	char task[32];
	char low[64], medium[64], high[64];
};

struct conv_pack
{
	char pack[64];
	char provider[64];
	char generated[32];
	char blob_base_url[CONV_STORE_PATH_MAX]; //!< as written
	char base_dir[CONV_STORE_PATH_MAX];      //!< the manifest's directory (relative base URLs)
	char manifest_path[CONV_STORE_PATH_MAX]; //!< "" when parsed from memory
	uint32_t model_count;
	struct conv_pack_model models[CONV_STORE_MAX_MODELS];
	uint32_t default_count;
	struct conv_pack_default defaults[CONV_STORE_MAX_DEFAULTS];
};

/*!
 * Parse one schema-2 manifest from @p json. @p base_dir resolves a relative
 * `blobBaseUrl`. False with @p err filled on any schema violation: schema != 2,
 * missing pack / models, a file without a 64-hex sha256 or a size, a
 * `defaults` entry naming a model the pack does not have.
 */
bool
conv_pack_parse(const char *json, const char *base_dir, struct conv_pack *out, char *err, size_t err_size);

//! conv_pack_parse() of a file (base_dir = its directory).
bool
conv_pack_load(const char *path, struct conv_pack *out, char *err, size_t err_size);

/*!
 * The download URL of @p f: `<blobBaseUrl>/<sha256>.<format>` (no extension
 * when the format is empty). A relative base URL (no scheme) is resolved
 * against the manifest's directory and returned as a `file://` URL.
 */
void
conv_pack_blob_url(const struct conv_pack *p, const struct conv_pack_file *f, char *out, size_t out_size);

/*!
 * conv_pack_load() of a local path, a `file://` URL, or (Windows) an
 * `http(s)://` URL fetched into memory; a remote manifest's relative
 * `blobBaseUrl` resolves against the manifest URL's directory.
 */
bool
conv_pack_load_url(const char *url_or_path, struct conv_pack *out, char *err, size_t err_size);

//! Model @p name of @p p (NULL = none).
const struct conv_pack_model *
conv_pack_find_model(const struct conv_pack *p, const char *name);


/*
 *
 * The store.
 *
 */

struct conv_model_store;

/*! Download progress of one blob, from the worker / calling thread. */
typedef void (*conv_store_progress_fn)(void *user, const char *sha256, uint64_t done, uint64_t total);

struct conv_model_store_config
{
	char root[CONV_STORE_PATH_MAX];     //!< "" = conv_store_root_resolve()
	bool allow_downloads;               //!< false: a missing blob is ABSENT(policy)
	bool allow_research_only;           //!< AllowResearchOnly: non-commercial models may be fetched / loaded
	char nonce[48];                     //!< .part nonce; "" = "<pid>-<random>"; the installer uses "installer"
	conv_store_progress_fn on_progress; //!< optional; rate-limited (2 Hz / 5 %, plus the last chunk)
	void *user;
	uint32_t chunk_bytes; //!< copy / read chunk (0 = 1 MiB)
	uint32_t throttle_ms; //!< sleep per chunk (tests: an observable DOWNLOADING)
};

//! Defaults: allow_downloads = true, everything else zero.
void
conv_model_store_config_defaults(struct conv_model_store_config *cfg);

//! Create (starts the worker). The root is NOT created until a blob is written.
bool
conv_model_store_create(const struct conv_model_store_config *cfg, struct conv_model_store **out);

void
conv_model_store_destroy(struct conv_model_store **s);

const char *
conv_model_store_root(const struct conv_model_store *s);

//! Replace the policy (allow_downloads / allow_research_only); clears ABSENT(policy) marks.
void
conv_model_store_set_policy(struct conv_model_store *s, bool allow_downloads, bool allow_research_only);

//! Register a pack (copied). Same pack id again replaces it.
bool
conv_model_store_add_pack(struct conv_model_store *s, const struct conv_pack *p);

//! conv_pack_load() + add. Logs (WARN) and returns false on a bad manifest.
bool
conv_model_store_add_pack_file(struct conv_model_store *s, const char *path);

uint32_t
conv_model_store_pack_count(struct conv_model_store *s);

//! A copy of pack @p i (false past the end).
bool
conv_model_store_get_pack(struct conv_model_store *s, uint32_t i, struct conv_pack *out);

/*!
 * Host op `model_resolve`. @p ref = "<pack>/<model>[/<variant>]" or a 64-hex
 * sha256. Never blocks on hashing or the network. out->state:
 *  - READY: every file present, stamped and hash-verified by this process;
 *    out->path = the variant's "model" file (else its first file);
 *  - INSTALLED: on disk and stamped, not yet verified by this process: call
 *    model_request (warm-up) to verify it;
 *  - WARMING: the worker is verifying it now;
 *  - DOWNLOADING: in flight, progress = bytes / total over the files;
 *  - ABSENT: not in the store (failure_reason XRT_CONV_FAIL_POLICY when a
 *    request was refused by policy: downloads off, or a research-only model
 *    without AllowResearchOnly);
 *  - FAILED: download / verify failed (NETWORK = retryable, MODEL = bad bytes).
 * Returns false only for an unknown ref (state ABSENT, reason MODEL).
 */
bool
conv_model_store_resolve(struct conv_model_store *s, const char *ref, struct xrt_conv_model_location *out);

/*!
 * Host op `model_request`: queue verification / download of every file of
 * @p ref that is not READY. Concurrent requests for one sha256 share one job.
 * False when the ref is unknown or policy refuses it (resolve then reports
 * ABSENT(policy)). A FAILED blob is retried by a new request.
 */
bool
conv_model_store_request(struct conv_model_store *s, const char *ref);

/*! Block until nothing is queued or in flight, or @p timeout_ms passed. True = idle. */
bool
conv_model_store_wait_idle(struct conv_model_store *s, uint32_t timeout_ms);


/*
 *
 * Synchronous operations (CLI, installer pre-fetch). Each runs the same write
 * protocol on the calling thread.
 *
 */

/*! Result of one blob operation. */
enum conv_store_blob_result
{
	CONV_STORE_BLOB_OK = 0,          //!< present, verified (or freshly written)
	CONV_STORE_BLOB_SKIPPED,         //!< stamped at the right size: left alone (installer never re-hashes)
	CONV_STORE_BLOB_MISSING,         //!< not present (verify)
	CONV_STORE_BLOB_TAMPERED,        //!< present + stamped, wrong bytes: deleted
	CONV_STORE_BLOB_TAMPERED_LOCKED, //!< wrong bytes we may not delete (ACL)
	CONV_STORE_BLOB_BAD_DOWNLOAD,    //!< downloaded bytes failed size / hash: part deleted
	CONV_STORE_BLOB_NETWORK,         //!< fetch failed (unreachable, HTTP error, no downloader)
	CONV_STORE_BLOB_POLICY,          //!< refused: downloads off or research-only
	CONV_STORE_BLOB_IO,              //!< cannot create / write / rename in the store
	CONV_STORE_BLOB_CANCELLED,       //!< the store is shutting down
};

const char *
conv_store_blob_result_str(enum conv_store_blob_result r);

/*!
 * Fetch every file of @p m (variant @p variant, NULL = every variant) with
 * the write protocol. @p skip_stamped: a stamped blob of the right size is
 * skipped without hashing (the installer's upgrade rule: never re-hash, never
 * replace a blob a running process may have mapped). Honours the policy
 * (research-only models refused without AllowResearchOnly). On success the
 * pack is recorded under packs/.
 */
enum conv_store_blob_result
conv_model_store_fetch_model(struct conv_model_store *s,
                             const struct conv_pack *p,
                             const struct conv_pack_model *m,
                             const char *variant,
                             bool skip_stamped);

/*! Re-hash one blob in the store; deletes it (and its stamp) when tampered. */
enum conv_store_blob_result
conv_model_store_verify_blob(struct conv_model_store *s, const char *sha256, uint64_t size);

/*! One blob's on-disk state (no hashing). */
struct conv_store_blob_disk
{
	bool present;        //!< blobs/<sha> exists
	bool stamped;        //!< blobs/<sha>.ok exists
	uint64_t size;       //!< size of blobs/<sha>
	uint64_t part_bytes; //!< largest .part for it (0 = none)
};

void
conv_model_store_blob_disk(struct conv_model_store *s, const char *sha256, struct conv_store_blob_disk *out);

/*!
 * Delete every blob (+ stamp, + .part) that no registered pack references.
 * Returns how many blobs were deleted; @p bytes_out their bytes. Never
 * automatic (spec §5.1): the CLI's `conv models prune` only.
 */
uint32_t
conv_model_store_prune(struct conv_model_store *s, uint64_t *bytes_out);

/*!
 * Create the root (and blobs/, packs/) when missing. A root this call creates
 * gets the owner marker and, on Windows, the store ACL: SYSTEM and
 * Administrators full, BUILTIN\Users read everywhere + create-files /
 * create-folders on directories (the service and the browser run as the
 * logged-in user), ALL APPLICATION PACKAGES read (AppContainer readers),
 * CREATOR OWNER full on what a writer creates. An existing root's ACL is
 * left alone.
 */
bool
conv_model_store_ensure_root(struct conv_model_store *s);

#ifdef XRT_OS_WINDOWS
//! The SDDL conv_model_store_ensure_root() applies to a root it creates.
#define CONV_STORE_ROOT_SDDL                                                                                           \
	"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)(A;CI;0x6;;;BU)(A;OICIIO;FA;;;CO)"                \
	"(A;OICI;0x1200a9;;;AC)"
#endif


/*
 *
 * Per-pack summary (status snapshot rows, `conv models list`).
 *
 */

struct conv_store_pack_summary
{
	char pack[64];
	char provider[64];
	char state[16]; //!< "installed" | "downloading" | "partial" | "evicted" | "absent"
	uint32_t models;
	uint32_t models_installed;
	uint32_t research_models; //!< models whose licence is not commercial
	uint64_t bytes_total;     //!< distinct blobs of every model
	uint64_t bytes_installed;
	uint64_t download_done; //!< in-flight bytes (this process, else .part sizes)
	uint64_t download_total;
};

/*!
 * Summarise pack @p i from disk + this store's in-flight jobs. "installed":
 * every blob of the pack's default set (its `prefetch` models; every model
 * when none is flagged) is stamped at the right size; "downloading": a blob of
 * the pack is in flight (or has a .part); "evicted": the store provisioned the
 * pack (packs/<pack>.models.json) but default-set blobs are gone, e.g. wiped by
 * an older browser uninstaller (never a latched failure: a request re-fetches);
 * "partial": some blobs present; "absent": none.
 */
bool
conv_model_store_pack_summary(struct conv_model_store *s, uint32_t i, struct conv_store_pack_summary *out);


/*
 *
 * Host glue: one process-wide store behind the two host ops, shared by the
 * in-service host (service_conversion.c) and the satellite host
 * (conv_host_provider.c). The ops carry no provider context (spec §2.1), so
 * the store serves the union of the packs it was given.
 *
 */

/*!
 * A registered pack path (`Packs` / `packs[]`) as a file path: absolute paths
 * as they are, relative ones under the directory of @p binary_path.
 */
void
conv_store_pack_path(const char *pack, const char *binary_path, char *out, size_t out_size);

/*!
 * (Re)configure the process store: packs are added (manifest paths), the
 * policy replaced. Creates the store on first call. Thread-safe.
 */
void
conv_store_host_configure(const char *const *pack_paths,
                          uint32_t pack_count,
                          bool allow_downloads,
                          bool allow_research_only);

//! The process store (NULL before conv_store_host_configure()).
struct conv_model_store *
conv_store_host_get(void);

//! `xrt_conversion_plugin_host_iface::model_resolve` over the process store.
bool
conv_store_host_model_resolve(const char *ref, struct xrt_conv_model_location *out);

//! `xrt_conversion_plugin_host_iface::model_request` over the process store.
bool
conv_store_host_model_request(const char *ref);

//! Tear down the process store (tests; process exit).
void
conv_store_host_shutdown(void);

#ifdef __cplusplus
}
#endif
