// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS key character → Win32 virtual-key code, for workspace input (#2782).
 * @ingroup ipc_server
 *
 * The AppKit pump (ipc_server_macos_appkit.m) reads a key as the first
 * character of NSEvent's charactersIgnoringModifiers. The wire carries a Win32
 * VK code (XR_DXR_workspace_client_input, and the reserved-key table a
 * controller registers with xrSetWorkspaceReservedKeysDXR), so this maps the
 * character to the VK code Windows reports for the same key, the way desktop
 * Linux maps a keysym (comp_window_linux_keysym_to_vk). Plain C with no AppKit
 * dependency, so it is unit-tested on every platform.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Map one charactersIgnoringModifiers character to a Win32 VK code.
 *
 * - Letters, either case, map to VK_A..VK_Z ('A'..'Z'): a VK code names the
 *   key, not the character, so Shift does not change it.
 * - Digits and their US-layout Shift symbols ('!' → VK_1) map to VK_0..VK_9;
 *   US punctuation and its Shift symbols map to the VK_OEM_* codes.
 * - Space, Tab, Return, keypad Enter, Escape and Backspace map to their VK
 *   codes; the AppKit function-key characters (arrows, F1-F12, Insert, forward
 *   Delete, Home, End, Page Up/Down) map to the matching VK_* codes.
 * - With Control held, charactersIgnoringModifiers can return the ASCII
 *   control character (Ctrl+L → 0x0C); 0x01..0x1A then maps back to the letter.
 *
 * @param ch        The first UTF-16 unit of charactersIgnoringModifiers.
 * @param ctrl_held Control is held.
 * @return The VK code, or 0 when the key has none (nothing to forward).
 */
uint32_t
ipc_server_macos_key_to_vk(uint32_t ch, bool ctrl_held);

#ifdef __cplusplus
}
#endif
