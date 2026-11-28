// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The macOS AppKit pump's key character -> Win32 VK code mapping
 *         (#2782): a key reaches the wire with the VK code Windows sends for
 *         it, so the controller's reserved chords match identically.
 */

#include "catch_amalgamated.hpp"

#include "ipc_server_macos_keys.h"

#include <cstdint>

namespace {

uint32_t
vk(uint32_t ch, bool ctrl = false)
{
	return ipc_server_macos_key_to_vk(ch, ctrl);
}

} // namespace

TEST_CASE("macos keys: letters are VK_A..VK_Z whatever the case", "[ipc][macos][keys]")
{
	for (uint32_t c = 'a'; c <= 'z'; c++) {
		CHECK(vk(c) == c - 'a' + 'A');
		CHECK(vk(c - 'a' + 'A') == c - 'a' + 'A'); // Shift held
	}
	// The Shell's reserved letter chords ({'L', CTRL}, {'O', CTRL}, {'C', CTRL}).
	CHECK(vk('l', true) == 0x4C);
	CHECK(vk('o', true) == 0x4F);
	CHECK(vk('c', true) == 0x43);
}

TEST_CASE("macos keys: a Ctrl control character maps back to its letter", "[ipc][macos][keys]")
{
	for (uint32_t c = 0x01; c <= 0x1A; c++) {
		CHECK(vk(c, true) == 'A' + (c - 0x01));
	}
	CHECK(vk(0x0C, true) == 'L');
	// Without Control the same characters are Tab / Return / Backspace, not letters.
	CHECK(vk(0x09) == 0x09); // VK_TAB
	CHECK(vk(0x0D) == 0x0D); // VK_RETURN
	CHECK(vk(0x08) == 0x08); // VK_BACK
	CHECK(vk(0x0C) == 0);    // a bare form feed is no key
}

TEST_CASE("macos keys: digits and their Shift symbols are VK_0..VK_9", "[ipc][macos][keys]")
{
	for (uint32_t c = '0'; c <= '9'; c++) {
		CHECK(vk(c) == c);
		CHECK(vk(c, true) == c); // Ctrl+1 .. Ctrl+9 (Shell chords)
	}
	const char *shifted = ")!@#$%^&*(";
	for (uint32_t d = 0; d < 10; d++) {
		CHECK(vk((uint32_t)(unsigned char)shifted[d]) == '0' + d);
	}
}

TEST_CASE("macos keys: named keys match the Windows VK codes", "[ipc][macos][keys]")
{
	CHECK(vk(' ') == 0x20);    // VK_SPACE
	CHECK(vk(0x19) == 0x09);   // NSBackTabCharacter (Shift+Tab) -> VK_TAB
	CHECK(vk(0x03) == 0x0D);   // keypad Enter -> VK_RETURN
	CHECK(vk(0x1B) == 0x1B);   // VK_ESCAPE
	CHECK(vk(0x7F) == 0x08);   // Backspace -> VK_BACK
	CHECK(vk(0xF728) == 0x2E); // forward Delete -> VK_DELETE
	CHECK(vk(0xF727) == 0x2D); // VK_INSERT
	CHECK(vk(0xF729) == 0x24); // VK_HOME
	CHECK(vk(0xF72B) == 0x23); // VK_END
	CHECK(vk(0xF72C) == 0x21); // VK_PRIOR
	CHECK(vk(0xF72D) == 0x22); // VK_NEXT
	CHECK(vk(0xF700) == 0x26); // VK_UP
	CHECK(vk(0xF701) == 0x28); // VK_DOWN
	CHECK(vk(0xF702) == 0x25); // VK_LEFT
	CHECK(vk(0xF703) == 0x27); // VK_RIGHT
	for (uint32_t f = 0; f < 12; f++) {
		CHECK(vk(0xF704 + f) == 0x70 + f); // VK_F1..VK_F12
	}
	CHECK(vk(0xF70E) == 0x7A); // F11 maximize
}

TEST_CASE("macos keys: punctuation is VK_OEM_*, shifted or not", "[ipc][macos][keys]")
{
	struct
	{
		char plain, shifted;
		uint32_t vk;
	} const oem[] = {
	    {';', ':', 0xBA}, {'=', '+', 0xBB},  {',', '<', 0xBC},
	    {'-', '_', 0xBD}, {'.', '>', 0xBE},  {'/', '?', 0xBF},
	    {'`', '~', 0xC0}, {'[', '{', 0xDB},  {0x5C /* backslash */, '|', 0xDC},
	    {']', '}', 0xDD}, {'\'', '"', 0xDE},
	};
	for (const auto &k : oem) {
		CHECK(vk((uint32_t)k.plain) == k.vk);
		CHECK(vk((uint32_t)k.shifted) == k.vk);
	}
	// The default reserved [ and ] (window Z back / forward).
	CHECK(vk('[') == 0xDB);
	CHECK(vk(']') == 0xDD);
}

TEST_CASE("macos keys: a character with no VK code maps to 0", "[ipc][macos][keys]")
{
	CHECK(vk(0x00E9) == 0); // e acute (a non-US layout's own key)
	CHECK(vk(0xF710) == 0); // NSF13FunctionKey: beyond the Windows F1-F12 the Linux path maps
	CHECK(vk(0x0000) == 0);
}
