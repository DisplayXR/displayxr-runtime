// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Unit tests for the present-hook re-entry detection's portable half
 *         (#2635): the x64 detour decoder that resolves who owns a chain's
 *         Present entry, and the "is the hook's module on the app's stack" match.
 */

#include "catch_amalgamated.hpp"

#include "util/comp_present_hook.h"

#include <cstring>

static void
pad(uint8_t *buf, const uint8_t *code, size_t n)
{
	memset(buf, 0xCC, COMP_PRESENT_HOOK_DECODE_BYTES);
	memcpy(buf, code, n);
}

TEST_CASE("decode: jmp rel32 forward and backward", "[present_hook]")
{
	uint8_t b[COMP_PRESENT_HOOK_DECODE_BYTES];
	uint64_t t = 0;
	const uint8_t fwd[] = {0xE9, 0x00, 0x10, 0x00, 0x00};
	pad(b, fwd, sizeof(fwd));
	REQUIRE(comp_present_hook_decode_jump(b, 0x7ffa00001000ULL, &t) == COMP_PRESENT_HOOK_JUMP_DIRECT);
	CHECK(t == 0x7ffa00001000ULL + 5 + 0x1000);

	const uint8_t back[] = {0xE9, 0xFB, 0xFF, 0xFF, 0xFF}; // -5: jumps to itself
	pad(b, back, sizeof(back));
	REQUIRE(comp_present_hook_decode_jump(b, 0x1000, &t) == COMP_PRESENT_HOOK_JUMP_DIRECT);
	CHECK(t == 0x1000);
}

TEST_CASE("decode: jmp rel8", "[present_hook]")
{
	uint8_t b[COMP_PRESENT_HOOK_DECODE_BYTES];
	uint64_t t = 0;
	const uint8_t c[] = {0xEB, 0xFE};
	pad(b, c, sizeof(c));
	REQUIRE(comp_present_hook_decode_jump(b, 0x2000, &t) == COMP_PRESENT_HOOK_JUMP_DIRECT);
	CHECK(t == 0x2000);
}

TEST_CASE("decode: jmp [rip+disp32] is indirect through the slot", "[present_hook]")
{
	uint8_t b[COMP_PRESENT_HOOK_DECODE_BYTES];
	uint64_t t = 0;
	const uint8_t c[] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00}; // slot right after the instruction
	pad(b, c, sizeof(c));
	REQUIRE(comp_present_hook_decode_jump(b, 0x3000, &t) == COMP_PRESENT_HOOK_JUMP_INDIRECT);
	CHECK(t == 0x3006);

	const uint8_t rex[] = {0x48, 0xFF, 0x25, 0x10, 0x00, 0x00, 0x00};
	pad(b, rex, sizeof(rex));
	REQUIRE(comp_present_hook_decode_jump(b, 0x3000, &t) == COMP_PRESENT_HOOK_JUMP_INDIRECT);
	CHECK(t == 0x3000 + 1 + 6 + 0x10);
}

TEST_CASE("decode: absolute register jumps and push/ret", "[present_hook]")
{
	uint8_t b[COMP_PRESENT_HOOK_DECODE_BYTES];
	uint64_t t = 0;
	const uint8_t rax[] = {0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xFF, 0xE0};
	pad(b, rax, sizeof(rax));
	REQUIRE(comp_present_hook_decode_jump(b, 0, &t) == COMP_PRESENT_HOOK_JUMP_DIRECT);
	CHECK(t == 0x1122334455667788ULL);

	const uint8_t r11[] = {0x49, 0xBB, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x41, 0xFF, 0xE3};
	pad(b, r11, sizeof(r11));
	REQUIRE(comp_present_hook_decode_jump(b, 0, &t) == COMP_PRESENT_HOOK_JUMP_DIRECT);
	CHECK(t == 0x1122334455667788ULL);

	const uint8_t pr[] = {0x68, 0x44, 0x33, 0x22, 0x11, 0xC7, 0x44, 0x24, 0x04, 0xFA, 0x7F, 0x00, 0x00, 0xC3};
	pad(b, pr, sizeof(pr));
	REQUIRE(comp_present_hook_decode_jump(b, 0, &t) == COMP_PRESENT_HOOK_JUMP_DIRECT);
	CHECK(t == 0x00007FFA11223344ULL);
}

TEST_CASE("decode: an ordinary prologue is not a jump", "[present_hook]")
{
	uint8_t b[COMP_PRESENT_HOOK_DECODE_BYTES];
	uint64_t t = 0xdead;
	// mov [rsp+8],rbx ; push rdi ; sub rsp,20h — a typical unhooked entry.
	const uint8_t c[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20};
	pad(b, c, sizeof(c));
	CHECK(comp_present_hook_decode_jump(b, 0x4000, &t) == COMP_PRESENT_HOOK_JUMP_NONE);
	// mov rax, imm64 NOT followed by jmp rax is just a load.
	const uint8_t load[] = {0x48, 0xB8, 1, 2, 3, 4, 5, 6, 7, 8, 0x48, 0x89};
	pad(b, load, sizeof(load));
	CHECK(comp_present_hook_decode_jump(b, 0x4000, &t) == COMP_PRESENT_HOOK_JUMP_NONE);
	CHECK(t == 0xdead);
}

TEST_CASE("stack match: the hook's module on the app stack is the deadlock shape", "[present_hook]")
{
	// The #2635 dump, shape-for-shape: the app thread's xrEndFrame sits under
	// the injector's hook, which sits under the overlay's hook. The overlay
	// owns our chain's Present entry.
	const struct comp_present_hook_range overlay = {0x7ffa73200000ULL, 0x7ffa73400000ULL};
	const uint64_t app_stack[] = {
	    0x7ffa2800f801ULL, // runtime: lock_guard in layer_commit
	    0x7ffa27f8ea65ULL, // runtime: xrEndFrame
	    0x7ff9f250a100ULL, // injector
	    0x7ffa7325c24fULL, // overlay's Present hook
	    0x7ff7a2b8c6efULL, // game
	};
	CHECK(comp_present_hook_stack_hits(app_stack, 5, &overlay, 1));

	// An app that calls xrEndFrame from its own render loop, overlay loaded but
	// not on the stack: no latch, repaints stay on.
	const uint64_t plain_stack[] = {0x7ffa2800f801ULL, 0x7ffa27f8ea65ULL, 0x7ff7a2b8c6efULL};
	CHECK_FALSE(comp_present_hook_stack_hits(plain_stack, 3, &overlay, 1));

	// Range bounds: end is exclusive; an empty range never matches.
	const uint64_t edge[] = {0x7ffa73400000ULL};
	CHECK_FALSE(comp_present_hook_stack_hits(edge, 1, &overlay, 1));
	const struct comp_present_hook_range empty = {0, 0};
	const uint64_t zero[] = {0};
	CHECK_FALSE(comp_present_hook_stack_hits(zero, 1, &empty, 1));
}
