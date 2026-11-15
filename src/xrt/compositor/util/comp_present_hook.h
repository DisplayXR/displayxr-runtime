// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Present-hook re-entry detection, the portable half (#2635).
 *
 * The hazard. An OpenXR injector calls xrEndFrame from inside its own
 * IDXGISwapChain::Present hook, and that hook is commonly entered through
 * another process-wide Present hook (an overlay) that holds a lock across the
 * call. The app thread therefore owns the hook's lock while it waits for the
 * compositor lock in layer_commit. A repaint thread that holds the compositor
 * lock and presents the runtime's own chain enters the same hook and waits for
 * the same lock: deadlock, at the first repaint, forever.
 *
 * The check. Resolve which module owns our chain's Present entry point (the
 * vtable slot, then any jump detour at its entry). If it is not the system's
 * DXGI, that module hooks Present. If the APP thread's stack, captured inside
 * our app-frame present, contains that module, the app is calling us from
 * inside a hooked Present, and a present from any other runtime thread while
 * we hold the compositor lock can deadlock. Repaints stop for the session.
 *
 * Here: the pure parts (x64 jump decoding, stack/range matching), so they are
 * unit-testable on every platform. The Windows probe that reads live code and
 * captures the stack lives with the target that uses it.
 *
 * @ingroup comp_util
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

//! What the instruction at a function's entry does.
enum comp_present_hook_jump
{
	COMP_PRESENT_HOOK_JUMP_NONE = 0, //!< Not a recognised jump: real code.
	COMP_PRESENT_HOOK_JUMP_DIRECT,   //!< Jumps to *out_target.
	COMP_PRESENT_HOOK_JUMP_INDIRECT, //!< Jumps through the 8-byte pointer AT *out_target.
};

//! Bytes @ref comp_present_hook_decode_jump may read at the entry.
#define COMP_PRESENT_HOOK_DECODE_BYTES 16

static inline int32_t
comp_present_hook_rd32(const uint8_t *p)
{
	uint32_t v;
	memcpy(&v, p, sizeof(v));
	return (int32_t)v;
}

static inline uint64_t
comp_present_hook_rd64(const uint8_t *p)
{
	uint64_t v;
	memcpy(&v, p, sizeof(v));
	return v;
}

/*!
 * Decode the x64 detour shapes hooking libraries write at a function's entry.
 *
 * @param code  COMP_PRESENT_HOOK_DECODE_BYTES readable bytes at the entry.
 * @param ip    The entry's address (for RIP-relative forms).
 * @param out_target  The jump target (DIRECT) or the pointer slot (INDIRECT).
 */
static inline enum comp_present_hook_jump
comp_present_hook_decode_jump(const uint8_t *code, uint64_t ip, uint64_t *out_target)
{
	// jmp rel32
	if (code[0] == 0xE9) {
		*out_target = ip + 5 + (int64_t)comp_present_hook_rd32(code + 1);
		return COMP_PRESENT_HOOK_JUMP_DIRECT;
	}
	// jmp rel8
	if (code[0] == 0xEB) {
		*out_target = ip + 2 + (int64_t)(int8_t)code[1];
		return COMP_PRESENT_HOOK_JUMP_DIRECT;
	}
	// jmp qword ptr [rip+disp32] (optionally REX.W-prefixed)
	{
		const uint8_t *p = code;
		uint64_t at = ip;
		if (p[0] == 0x48) {
			p++;
			at++;
		}
		if (p[0] == 0xFF && p[1] == 0x25) {
			*out_target = at + 6 + (int64_t)comp_present_hook_rd32(p + 2);
			return COMP_PRESENT_HOOK_JUMP_INDIRECT;
		}
	}
	// mov rax, imm64 ; jmp rax
	if (code[0] == 0x48 && code[1] == 0xB8 && code[10] == 0xFF && code[11] == 0xE0) {
		*out_target = comp_present_hook_rd64(code + 2);
		return COMP_PRESENT_HOOK_JUMP_DIRECT;
	}
	// mov r11, imm64 ; jmp r11
	if (code[0] == 0x49 && code[1] == 0xBB && code[10] == 0x41 && code[11] == 0xFF && code[12] == 0xE3) {
		*out_target = comp_present_hook_rd64(code + 2);
		return COMP_PRESENT_HOOK_JUMP_DIRECT;
	}
	// push imm32 ; mov dword ptr [rsp+4], imm32 ; ret
	if (code[0] == 0x68 && code[5] == 0xC7 && code[6] == 0x44 && code[7] == 0x24 && code[8] == 0x04 &&
	    code[13] == 0xC3) {
		const uint64_t lo = (uint32_t)comp_present_hook_rd32(code + 1);
		const uint64_t hi = (uint32_t)comp_present_hook_rd32(code + 9);
		*out_target = (hi << 32) | lo;
		return COMP_PRESENT_HOOK_JUMP_DIRECT;
	}
	return COMP_PRESENT_HOOK_JUMP_NONE;
}

//! A module's image, [base, end).
struct comp_present_hook_range
{
	uint64_t base;
	uint64_t end;
};

/*!
 * Does any return address in @p frames fall inside one of @p ranges?
 * The app-thread half of the check: "is a Present hook's module on this stack".
 */
static inline bool
comp_present_hook_stack_hits(const uint64_t *frames,
                             uint32_t frame_count,
                             const struct comp_present_hook_range *ranges,
                             uint32_t range_count)
{
	for (uint32_t i = 0; i < frame_count; i++) {
		for (uint32_t r = 0; r < range_count; r++) {
			if (ranges[r].end > ranges[r].base && frames[i] >= ranges[r].base &&
			    frames[i] < ranges[r].end) {
				return true;
			}
		}
	}
	return false;
}

#ifdef __cplusplus
}
#endif
