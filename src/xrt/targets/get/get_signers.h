// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The first-party Authenticode signers, in one place.
 *
 * Certificate subject names (the leaf's simple display name) that sign
 * DisplayXR's own Windows binaries. Two readers:
 *  - `dxr-get` (get_core.c): the signers a **schema-1** catalog payload may
 *    carry. A schema-2 catalog names its publishers' signers itself, in the
 *    signed index, and this list plays no part there (ADR-064 §4).
 *  - `displayxr-cli catalog` (cli_catalog.c): `dxr-get.exe` is started
 *    elevated only when it is signed by one of these.
 *
 * It has no dependencies, so both can include it. A new signing certificate
 * with another subject is added HERE.
 */

#pragma once

//! Initialiser of a `const char *const []`.
#define DXR_FIRST_PARTY_SIGNERS                                                                                        \
	{                                                                                                              \
		"Leia, Inc."                                                                                           \
	}
