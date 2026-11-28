// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The model store's internals the §5.6 builder (conv_model_build.c)
 *         uses: the downloader into an arbitrary path, the ingest of a file
 *         already hashed, the cancel hook and the store's UTF-8 file-system
 *         helpers. Not a public API: only conv_model_store.c and
 *         conv_model_build.c include it.
 * @ingroup aux_conversion
 */

#pragma once

#include "conv_model_store.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

//! True = stop: a running build's cancel file appeared, or the user interrupted it.
typedef bool (*conv_store_cancel_fn)(void *user);

//! Install (or clear, fn = NULL) the cancel check every transfer and hash of @p s polls.
void
conv_store_i_set_cancel(struct conv_model_store *s, conv_store_cancel_fn fn, void *user);

//! The store is stopping, or the cancel check said so.
bool
conv_store_i_cancelled(struct conv_model_store *s);

void
conv_store_i_blob_paths(const struct conv_model_store *s, const char *sha, char *blob, char *stamp);

//! The .part nonce rule ("<pid>-<random>", or cfg.nonce).
void
conv_store_i_nonce(const struct conv_model_store *s, char *out, size_t n);

//! Record @p p under packs/ (creates the root when missing).
void
conv_store_i_record_pack(struct conv_model_store *s, const struct conv_pack *p);

//! Full hash of a file (counted in conv_model_store_hash_count()). False on read error / cancel.
bool
conv_store_i_hash_file(struct conv_model_store *s, const char *path, char hex[CONV_STORE_SHA_HEX], uint64_t *bytes);

/*!
 * The store's downloader (file://, http(s):// with §5.5's auth rules) into
 * @p dest: `<dest>.part` first, size then sha256 while streaming, then a
 * rename. NETWORK / AUTH / BAD_DOWNLOAD / IO / CANCELLED, never a store blob.
 * @p fn receives progress at most 4 times a second (plus the first and last).
 */
enum conv_store_blob_result
conv_store_i_download(struct conv_model_store *s,
                      const char *url,
                      const struct conv_pack_auth *auth,
                      const char *sha,
                      uint64_t size,
                      const char *dest,
                      conv_store_progress_fn fn,
                      void *user);

/*!
 * Move @p path (already hashed by the caller to @p sha / @p size: this is its
 * entry check) into blobs/<sha> without replacing, then stamp it. A blob
 * already present is kept when it passes the load rule (ours is deleted).
 */
enum conv_store_blob_result
conv_store_i_ingest(struct conv_model_store *s, const char *path, const char *sha, uint64_t size);

//! Some variant of @p m is fully present, at size, stamped.
bool
conv_store_i_model_on_disk(struct conv_model_store *s, const struct conv_pack_model *m);

//! Does any registered model other than @p pack / @p model reference blob @p sha?
bool
conv_store_i_referenced_elsewhere(struct conv_model_store *s, const char *pack, const char *model, const char *sha);

//! Drop this process's "verified" mark of a blob (after `remove` deleted it).
void
conv_store_i_forget_blob(struct conv_model_store *s, const char *sha);

/* The store's UTF-8 file-system helpers (wide on Windows). */
bool
conv_store_fs_exists(const char *path);
bool
conv_store_fs_stat(const char *path, bool *is_dir, uint64_t *size);
FILE *
conv_store_fs_open(const char *path, const char *mode);
bool
conv_store_fs_remove(const char *path);
bool
conv_store_fs_mkdir_p(const char *path);
void
conv_store_fs_join(char *out, size_t n, const char *a, const char *b);
bool
conv_store_fs_write_text(const char *path, const char *text);
//! At most @p max bytes, NUL-terminated; free() it. NULL when unreadable.
char *
conv_store_fs_read_all(const char *path, size_t max);
//! Regular files of @p dir (not "." entries).
void
conv_store_fs_list(const char *dir, void (*fn)(void *user, const char *name, uint64_t size), void *user);
//! 0 = renamed, 1 = the target exists, -1 = error.
int
conv_store_fs_rename_noreplace(const char *from, const char *to);

#ifdef __cplusplus
}
#endif
