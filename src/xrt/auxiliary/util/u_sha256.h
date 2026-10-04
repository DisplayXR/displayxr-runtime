// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Compact SHA-256 + HMAC-SHA-256 (FIPS 180-4 / RFC 2104). Used to key
 *         identifiers that must not be derivable without a secret (the stereo
 *         camera persistentId, ADR-043 §7.5). Not a general crypto library.
 * @ingroup aux_util
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct u_sha256
{
	uint32_t h[8];
	uint64_t len;
	uint8_t buf[64];
	size_t buf_len;
};

void
u_sha256_init(struct u_sha256 *s);

void
u_sha256_update(struct u_sha256 *s, const void *data, size_t len);

void
u_sha256_final(struct u_sha256 *s, uint8_t out[32]);

//! One-shot digest.
void
u_sha256(const void *data, size_t len, uint8_t out[32]);

//! HMAC-SHA-256 of @p data under @p key (any key length).
void
u_hmac_sha256(const void *key, size_t key_len, const void *data, size_t len, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
