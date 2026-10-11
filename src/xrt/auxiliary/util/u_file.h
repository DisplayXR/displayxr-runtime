// Copyright 2019-2025, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Very simple file opening functions.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup aux_util
 */

#pragma once

#include <stdio.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int
u_file_get_config_dir(char *out_path, size_t out_path_size);

int
u_file_get_path_in_config_dir(const char *suffix, char *out_path, size_t out_path_size);

FILE *
u_file_open_file_in_config_dir(const char *filename, const char *mode);

FILE *
u_file_open_file_in_config_dir_subpath(const char *subpath, const char *filename, const char *mode);

int
u_file_get_hand_tracking_models_dir(char *out_path, size_t out_path_size);

int
u_file_get_runtime_dir(char *out_path, size_t out_path_size);

char *
u_file_read_content(FILE *file, size_t *out_file_size);

char *
u_file_read_content_from_path(const char *path, size_t *out_file_size);

int
u_file_get_path_in_runtime_dir(const char *suffix, char *out_path, size_t out_path_size);

/*!
 * Copy a filesystem path into a unix socket address's `sun_path` (pass
 * `sizeof(addr.sun_path)`, 108 bytes on Linux). Returns false — writing an
 * empty string — when the path plus its terminator does not fit; a socket
 * path that long (a deep XDG_RUNTIME_DIR) cannot be bound or dialled, and an
 * unchecked strcpy there overflows the address (glibc's fortify then aborts
 * the process with "buffer overflow detected").
 */
bool
u_file_copy_unix_socket_path(char *sun_path, size_t sun_path_size, const char *path);

#ifdef __cplusplus
}
#endif
