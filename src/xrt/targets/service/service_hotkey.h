// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Workspace-controller launch hotkey: combo grammar, parser, formatter.
 *
 * Grammar (display dashboard phase 8):
 *
 *     [Ctrl+][Shift+][Alt+][Win+]<Key>
 *
 * At least one modifier is required. `<Key>` is one of `Space`, `A`-`Z`,
 * `0`-`9`, `F1`-`F24`, `Tab`, `Enter`, `Backquote`, `Minus`, `Equals`,
 * `BracketLeft`, `BracketRight`, `Semicolon`, `Quote`, `Comma`, `Period`,
 * `Slash`, `Backslash`, `Insert`, `Delete`, `Home`, `End`, `PageUp`,
 * `PageDown`, `Left`, `Up`, `Right`, `Down`. The parser is case-insensitive,
 * accepts the modifiers in any order (each at most once) and also accepts
 * `ArrowLeft`/`ArrowUp`/`ArrowRight`/`ArrowDown`; the formatter always writes
 * the canonical spelling in the modifier order above. Anything else is
 * rejected. Pure C, no platform calls: the key codes are the Windows
 * virtual-key numbers (the only platform with a global hook today).
 *
 * See `docs/specs/runtime/workspace-controller-registration.md` §Launch settings.
 *
 * @ingroup ipc
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SERVICE_HOTKEY_MOD_CTRL (1u << 0)
#define SERVICE_HOTKEY_MOD_SHIFT (1u << 1)
#define SERVICE_HOTKEY_MOD_ALT (1u << 2)
#define SERVICE_HOTKEY_MOD_WIN (1u << 3)

//! The default launch combo of every workspace controller.
#define SERVICE_HOTKEY_DEFAULT "Ctrl+Space"

//! Longest canonical combo ("Ctrl+Shift+Alt+Win+BracketRight") plus slack.
#define SERVICE_HOTKEY_MAX 48

/*!
 * A parsed combo. `vk == 0` means "no hotkey".
 */
struct service_hotkey
{
	uint32_t mods; //!< SERVICE_HOTKEY_MOD_* bits; never 0 for a valid combo.
	uint32_t vk;   //!< Windows virtual-key code of the non-modifier key.
};

/*!
 * Parse @p text into @p out. Returns false (and leaves @p out zeroed) on any
 * deviation from the grammar: empty / whitespace tokens, unknown key,
 * duplicate or missing modifier, a modifier as the key, more than one key.
 */
bool
service_hotkey_parse(const char *text, struct service_hotkey *out);

/*!
 * Write the canonical spelling of @p hk into @p buf. Returns false if @p hk
 * is not a valid combo (no modifier, unknown key) or @p buf is too small.
 */
bool
service_hotkey_format(const struct service_hotkey *hk, char *buf, size_t buf_size);

/*!
 * Parse + format in one step: @p out receives the canonical spelling of
 * @p text. Returns false when @p text is not a valid combo.
 */
bool
service_hotkey_canonicalize(const char *text, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
