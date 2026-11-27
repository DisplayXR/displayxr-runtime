// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS key character → Win32 virtual-key code, for workspace input (#2782).
 * @ingroup ipc_server
 */

#include "ipc_server_macos_keys.h"

uint32_t
ipc_server_macos_key_to_vk(uint32_t ch, bool ctrl_held)
{
	// Only with Control held does charactersIgnoringModifiers sometimes return
	// the ASCII control character (Ctrl+L → 0x0C); map those back to letters so
	// the controller's letter chords match. Gating on Control is essential:
	// otherwise Return (0x0D), Tab (0x09) etc. would turn into letters.
	if (ctrl_held && ch >= 0x01 && ch <= 0x1A) {
		return 'A' + (ch - 0x01);
	}
	if (ch >= 'a' && ch <= 'z') { // VK_A..VK_Z are upper-case ASCII
		return ch - 0x20;
	}
	if (ch >= 'A' && ch <= 'Z') {
		return ch;
	}
	if (ch >= '0' && ch <= '9') { // VK_0..VK_9 == '0'..'9'
		return ch;
	}
	// NSF1FunctionKey..NSF12FunctionKey -> VK_F1..VK_F12
	if (ch >= 0xF704 && ch <= 0xF70F) {
		return 0x70 + (ch - 0xF704);
	}
	switch (ch) {
	case 0x20: return 0x20;   // space                     -> VK_SPACE
	case 0x09:                // Tab                       -> VK_TAB
	case 0x19: return 0x09;   // NSBackTabCharacter (Shift+Tab) -> VK_TAB
	case 0x0D:                // Return                    -> VK_RETURN
	case 0x03: return 0x0D;   // NSEnterCharacter (keypad) -> VK_RETURN
	case 0x1B: return 0x1B;   // Escape                    -> VK_ESCAPE
	case 0x7F:                // Backspace (NSDeleteCharacter)
	case 0x08: return 0x08;   //                           -> VK_BACK
	case 0xF700: return 0x26; // NSUpArrowFunctionKey      -> VK_UP
	case 0xF701: return 0x28; // NSDownArrowFunctionKey    -> VK_DOWN
	case 0xF702: return 0x25; // NSLeftArrowFunctionKey    -> VK_LEFT
	case 0xF703: return 0x27; // NSRightArrowFunctionKey   -> VK_RIGHT
	case 0xF727: return 0x2D; // NSInsertFunctionKey       -> VK_INSERT
	case 0xF728: return 0x2E; // NSDeleteFunctionKey (fwd) -> VK_DELETE
	case 0xF729: return 0x24; // NSHomeFunctionKey         -> VK_HOME
	case 0xF72B: return 0x23; // NSEndFunctionKey          -> VK_END
	case 0xF72C: return 0x21; // NSPageUpFunctionKey       -> VK_PRIOR
	case 0xF72D: return 0x22; // NSPageDownFunctionKey     -> VK_NEXT
	case '!': return '1';     // Shift+digit (US layout): the digit's VK code
	case '@': return '2';
	case '#': return '3';
	case '$': return '4';
	case '%': return '5';
	case '^': return '6';
	case '&': return '7';
	case '*': return '8';
	case '(': return '9';
	case ')': return '0';
	// US punctuation, unshifted and shifted.
	case ';':
	case ':': return 0xBA; // VK_OEM_1
	case '=':
	case '+': return 0xBB; // VK_OEM_PLUS
	case ',':
	case '<': return 0xBC; // VK_OEM_COMMA
	case '-':
	case '_': return 0xBD; // VK_OEM_MINUS
	case '.':
	case '>': return 0xBE; // VK_OEM_PERIOD
	case '/':
	case '?': return 0xBF; // VK_OEM_2
	case '`':
	case '~': return 0xC0; // VK_OEM_3
	case '[':
	case '{': return 0xDB; // VK_OEM_4
	case 0x5C:             // backslash
	case '|': return 0xDC; // VK_OEM_5
	case ']':
	case '}': return 0xDD; // VK_OEM_6
	case '\'':
	case '"': return 0xDE; // VK_OEM_7
	default: return 0;
	}
}
