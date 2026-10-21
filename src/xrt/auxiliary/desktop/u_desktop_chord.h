// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Key-chord parser for XR_DXR_desktop_control (spec §5, ADR-056).
 *
 * Grammar: `+`-separated tokens, case-insensitive, surrounding spaces ignored.
 * Modifiers: `ctrl` / `control`, `shift`, `alt` / `option`,
 * `super` / `win` / `cmd` / `command` / `meta`. At most ONE key, from the
 * canonical list documented on struct u_desktop_chord (u_desktop.h). Modifiers
 * alone are valid ("super"). Empty tokens ("ctrl++a", "+a"), unknown names, a
 * second key and an empty string are rejected.
 */

#pragma once

#include "u_desktop.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Parse @p s into @p out. Returns false (and leaves @p out zeroed) on any
 * error. A repeated modifier ("ctrl+ctrl+s") is accepted.
 */
bool
u_desktop_chord_parse(const char *s, struct u_desktop_chord *out);

/*!
 * The @p i-th canonical named key ("enter", "numpadadd", "rightctrl", ...; not the
 * letters, digits and f-keys), NULL past the last one. For tests and docs: every
 * backend's key map is checked against this list.
 */
const char *
u_desktop_chord_named_key(size_t i);

/*!
 * Format @p chord canonically ("ctrl+shift+v", modifiers in the order ctrl,
 * shift, alt, super). Returns false if @p cap is too small.
 */
bool
u_desktop_chord_format(const struct u_desktop_chord *chord, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
