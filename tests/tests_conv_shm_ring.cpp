// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-053 P2 (#2032): the SPSC shm ring of the conversion-host data
 *         plane (u_shm_ring.h) with the service <-> host ring records
 *         (conv_host_wire.h).
 *
 * What must hold (roadmap conversion-providers.md §3.5):
 *  - FIFO order; wrap of the slot index and of the free-running u32 cursors;
 *  - FULL at capacity, nothing overwritten; EMPTY when drained;
 *  - no lost wake: in every interleaving of the consumer's prepare_wait and
 *    the producer's push, either the consumer does not sleep or the producer
 *    is told to signal; and under a real two-thread race with a latching wake
 *    nothing stalls;
 *  - an untrusted peer's cursor (ahead, backwards) or a mismatched geometry is
 *    refused (CORRUPT / attach false), never followed out of bounds.
 */

#include "catch_amalgamated.hpp"

#include "util/u_shm_ring.h"
#include "conversion/conv_host_wire.h"

#if defined(_WIN32) || defined(__APPLE__) || (defined(__linux__) && !defined(__ANDROID__))
#define HAVE_OS_WAKE 1
#include "os/os_process.h"
#endif

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

namespace {

struct ring_mem
{
	std::vector<uint64_t> words; // 8-aligned backing
	size_t bytes;
	explicit ring_mem(size_t b) : words((b + 7) / 8 + 1, 0), bytes(b) {}
	void *
	ptr()
	{
		return words.data();
	}
};

constexpr uint32_t kSubmit = (uint32_t)sizeof(conv_host_wire_submit_record);
constexpr uint32_t kComplete = (uint32_t)sizeof(conv_host_wire_complete_record);

conv_host_wire_submit_record
rec(uint64_t seq)
{
	conv_host_wire_submit_record r;
	memset(&r, 0, sizeof(r));
	r.seq = seq;
	r.stream = 0xABCD0000ull + seq;
	r.op = CONV_HOST_RING_OP_CONVERT;
	r.image_count = 1;
	r.in_image_idx[0] = (uint32_t)(seq % 7);
	r.in_sync_idx = CONV_HOST_WIRE_NO_INDEX;
	r.in_fence_value = seq * 3;
	r.params.viewpoint_count = 2;
	r.params.viewpoints_xyz[1][0] = (float)seq;
	return r;
}

uint32_t *
header_word(ring_mem &m, uint32_t index)
{
	return reinterpret_cast<uint32_t *>(m.ptr()) + index;
}

} // namespace

TEST_CASE("shm ring: geometry", "[conv][shm_ring]")
{
	CHECK(u_shm_ring_bytes(kSubmit, CONV_HOST_WIRE_RING_CAPACITY) ==
	      ((U_SHM_RING_HEADER_BYTES + (size_t)kSubmit * CONV_HOST_WIRE_RING_CAPACITY + 63) & ~(size_t)63));
	CHECK(u_shm_ring_bytes(kComplete, CONV_HOST_WIRE_RING_CAPACITY) > 0);
	CHECK(u_shm_ring_bytes(kSubmit, 63) == 0);  // not a power of two
	CHECK(u_shm_ring_bytes(kSubmit, 1) == 0);   // too small
	CHECK(u_shm_ring_bytes(12, 64) == 0);       // not 8-aligned
	CHECK(u_shm_ring_bytes(0, 64) == 0);

	ring_mem m(u_shm_ring_bytes(kSubmit, 64));
	u_shm_ring r{};
	CHECK_FALSE(u_shm_ring_init(&r, m.ptr(), m.bytes - 1, kSubmit, 64));
	CHECK_FALSE(u_shm_ring_init(&r, static_cast<uint8_t *>(m.ptr()) + 4, m.bytes - 8, kSubmit, 64));
	REQUIRE(u_shm_ring_init(&r, m.ptr(), m.bytes, kSubmit, 64));

	// The other side must expect exactly this geometry.
	u_shm_ring c{};
	CHECK_FALSE(u_shm_ring_attach(&c, m.ptr(), m.bytes, kComplete, 64));
	CHECK_FALSE(u_shm_ring_attach(&c, m.ptr(), m.bytes, kSubmit, 32));
	CHECK(u_shm_ring_attach(&c, m.ptr(), m.bytes, kSubmit, 64));

	// No magic, no attach.
	ring_mem blank(m.bytes);
	CHECK_FALSE(u_shm_ring_attach(&c, blank.ptr(), blank.bytes, kSubmit, 64));
}

TEST_CASE("shm ring: FIFO, full, empty, wrap", "[conv][shm_ring]")
{
	const uint32_t cap = 4;
	ring_mem m(u_shm_ring_bytes(kSubmit, cap));
	u_shm_ring prod{}, cons{};
	REQUIRE(u_shm_ring_init(&prod, m.ptr(), m.bytes, kSubmit, cap));
	REQUIRE(u_shm_ring_attach(&cons, m.ptr(), m.bytes, kSubmit, cap));

	conv_host_wire_submit_record out;
	CHECK(u_shm_ring_pop(&cons, &out) == U_SHM_RING_EMPTY);

	// Fill to capacity: the next push is FULL and writes nothing.
	for (uint64_t i = 0; i < cap; i++) {
		auto r = rec(i);
		REQUIRE(u_shm_ring_push(&prod, &r, nullptr) == U_SHM_RING_OK);
	}
	CHECK(u_shm_ring_count(&prod) == cap);
	auto extra = rec(999);
	CHECK(u_shm_ring_push(&prod, &extra, nullptr) == U_SHM_RING_FULL);

	REQUIRE(u_shm_ring_pop(&cons, &out) == U_SHM_RING_OK);
	CHECK(out.seq == 0);
	auto r4 = rec(4);
	CHECK(u_shm_ring_push(&prod, &r4, nullptr) == U_SHM_RING_OK);

	// Wrap the slot index many times; every record arrives intact, in order.
	uint64_t next_in = 5, next_out = 1;
	for (int round = 0; round < 1000; round++) {
		while (u_shm_ring_pop(&cons, &out) == U_SHM_RING_OK) {
			auto want = rec(next_out++);
			REQUIRE(memcmp(&out, &want, sizeof(out)) == 0);
		}
		for (uint32_t k = 0; k < (uint32_t)(round % cap) + 1; k++) {
			auto r = rec(next_in++);
			REQUIRE(u_shm_ring_push(&prod, &r, nullptr) == U_SHM_RING_OK);
		}
	}
	while (u_shm_ring_pop(&cons, &out) == U_SHM_RING_OK) {
		REQUIRE(out.seq == next_out++);
	}
	CHECK(next_out == next_in);
	CHECK(u_shm_ring_count(&cons) == 0);
}

TEST_CASE("shm ring: the free-running u32 cursors wrap", "[conv][shm_ring]")
{
	const uint32_t cap = 8;
	ring_mem m(u_shm_ring_bytes(kComplete, cap));
	u_shm_ring prod{}, cons{};
	REQUIRE(u_shm_ring_init(&prod, m.ptr(), m.bytes, kComplete, cap));
	// Start both cursors just below 2^32 (word 16 = head, 32 = tail).
	*header_word(m, 16) = 0xFFFFFFFDu;
	*header_word(m, 32) = 0xFFFFFFFDu;
	REQUIRE(u_shm_ring_attach(&prod, m.ptr(), m.bytes, kComplete, cap));
	REQUIRE(u_shm_ring_attach(&cons, m.ptr(), m.bytes, kComplete, cap));

	for (uint64_t i = 0; i < 20; i++) {
		conv_host_wire_complete_record c;
		memset(&c, 0, sizeof(c));
		c.seq = i;
		c.status = -(int32_t)i;
		c.out_image_idx[CONV_HOST_WIRE_OUT_VIEWS] = (uint32_t)i;
		REQUIRE(u_shm_ring_push(&prod, &c, nullptr) == U_SHM_RING_OK);
		conv_host_wire_complete_record o;
		REQUIRE(u_shm_ring_pop(&cons, &o) == U_SHM_RING_OK);
		CHECK(o.seq == i);
		CHECK(o.status == -(int32_t)i);
	}
	CHECK(*header_word(m, 16) == 0xFFFFFFFDu + 20u); // wrapped through 0
}

TEST_CASE("shm ring: no lost wake, every interleaving", "[conv][shm_ring]")
{
	ring_mem m(u_shm_ring_bytes(kSubmit, 64));
	u_shm_ring prod{}, cons{};
	REQUIRE(u_shm_ring_init(&prod, m.ptr(), m.bytes, kSubmit, 64));
	REQUIRE(u_shm_ring_attach(&cons, m.ptr(), m.bytes, kSubmit, 64));
	conv_host_wire_submit_record out;
	bool wake = true;

	SECTION("consumer waits first: the push must signal")
	{
		REQUIRE(u_shm_ring_prepare_wait(&cons));
		auto r = rec(1);
		REQUIRE(u_shm_ring_push(&prod, &r, &wake) == U_SHM_RING_OK);
		CHECK(wake);
		u_shm_ring_finish_wait(&cons);
		CHECK(u_shm_ring_pop(&cons, &out) == U_SHM_RING_OK);
	}
	SECTION("producer pushes first: the consumer must not sleep")
	{
		auto r = rec(1);
		REQUIRE(u_shm_ring_push(&prod, &r, &wake) == U_SHM_RING_OK);
		CHECK_FALSE(wake);
		CHECK_FALSE(u_shm_ring_prepare_wait(&cons));
		CHECK(u_shm_ring_pop(&cons, &out) == U_SHM_RING_OK);
	}
	SECTION("after finish_wait no signal is owed")
	{
		REQUIRE(u_shm_ring_prepare_wait(&cons));
		u_shm_ring_finish_wait(&cons);
		auto r = rec(1);
		REQUIRE(u_shm_ring_push(&prod, &r, &wake) == U_SHM_RING_OK);
		CHECK_FALSE(wake);
	}
}

TEST_CASE("shm ring: an untrusted peer cannot drive us out of bounds", "[conv][shm_ring]")
{
	const uint32_t cap = 8;
	ring_mem m(u_shm_ring_bytes(kSubmit, cap));
	u_shm_ring prod{}, cons{};
	REQUIRE(u_shm_ring_init(&prod, m.ptr(), m.bytes, kSubmit, cap));
	REQUIRE(u_shm_ring_attach(&cons, m.ptr(), m.bytes, kSubmit, cap));
	conv_host_wire_submit_record out;

	SECTION("producer claims more than capacity in flight")
	{
		*header_word(m, 16) = cap + 1; // head
		CHECK(u_shm_ring_pop(&cons, &out) == U_SHM_RING_CORRUPT);
		CHECK(u_shm_ring_is_corrupt(&cons));
		CHECK_FALSE(u_shm_ring_prepare_wait(&cons));
		// Sticky.
		*header_word(m, 16) = 0;
		CHECK(u_shm_ring_pop(&cons, &out) == U_SHM_RING_CORRUPT);
	}
	SECTION("consumer moves its tail ahead of our head")
	{
		*header_word(m, 32) = 3; // tail > head (0)
		auto r = rec(0);
		CHECK(u_shm_ring_push(&prod, &r, nullptr) == U_SHM_RING_CORRUPT);
		CHECK(u_shm_ring_is_corrupt(&prod));
	}
	SECTION("our own cursor in shm is not trusted")
	{
		auto r = rec(0);
		REQUIRE(u_shm_ring_push(&prod, &r, nullptr) == U_SHM_RING_OK);
		*header_word(m, 16) = 5; // the consumer rewrites OUR head
		auto r1 = rec(1);
		REQUIRE(u_shm_ring_push(&prod, &r1, nullptr) == U_SHM_RING_OK);
		CHECK(*header_word(m, 16) == 2); // republished from the local cursor
	}
	SECTION("attach refuses a header with more than capacity in flight")
	{
		*header_word(m, 16) = 100;
		u_shm_ring late{};
		CHECK_FALSE(u_shm_ring_attach(&late, m.ptr(), m.bytes, kSubmit, cap));
	}
}

#ifdef HAVE_OS_WAKE
TEST_CASE("shm ring: two threads + a latching wake never stall", "[conv][shm_ring]")
{
	const uint32_t cap = 64;
	const uint64_t total = 200000;
	ring_mem m(u_shm_ring_bytes(kComplete, cap));
	u_shm_ring prod{}, cons{};
	REQUIRE(u_shm_ring_init(&prod, m.ptr(), m.bytes, kComplete, cap));
	REQUIRE(u_shm_ring_attach(&cons, m.ptr(), m.bytes, kComplete, cap));

	os_wake to_cons{}, to_prod{};
	uint64_t r1 = OS_NATIVE_HANDLE_INVALID, r2 = OS_NATIVE_HANDLE_INVALID;
	REQUIRE(os_wake_create_pair(&to_cons, &r1) == 0);
	REQUIRE(os_wake_create_pair(&to_prod, &r2) == 0);
	os_wake cons_end{}, prod_end{};
#ifdef _WIN32
	cons_end.native = r1;
	prod_end.native = r2;
#else
	os_wake_from_native(&cons_end, r1);
	os_wake_from_native(&prod_end, r2);
#endif

	std::atomic<uint32_t> stalls{0};
	std::thread producer([&] {
		for (uint64_t i = 0; i < total;) {
			conv_host_wire_complete_record c;
			memset(&c, 0, sizeof(c));
			c.seq = i;
			bool wake = false;
			enum u_shm_ring_result res = u_shm_ring_push(&prod, &c, &wake);
			if (res == U_SHM_RING_OK) {
				if (wake) {
					os_wake_signal(&to_cons);
				}
				i++;
			} else if (res == U_SHM_RING_FULL) {
				// Back-pressure: wait for the consumer to make room.
				if (os_wake_wait(&prod_end, 1000) == 0 && u_shm_ring_count(&prod) == cap) {
					stalls++;
				}
			} else {
				break;
			}
		}
	});

	uint64_t expect = 0;
	while (expect < total) {
		conv_host_wire_complete_record o;
		enum u_shm_ring_result res = u_shm_ring_pop(&cons, &o);
		if (res == U_SHM_RING_OK) {
			REQUIRE(o.seq == expect);
			expect++;
			os_wake_signal(&to_prod); // room freed (latching; coalesces)
			continue;
		}
		REQUIRE(res == U_SHM_RING_EMPTY);
		if (u_shm_ring_prepare_wait(&cons)) {
			// A lost wake would show up as a full 1 s timeout with data waiting.
			if (os_wake_wait(&cons_end, 1000) == 0 && u_shm_ring_count(&cons) > 0) {
				stalls++;
			}
			u_shm_ring_finish_wait(&cons);
		}
	}
	producer.join();
	CHECK(expect == total);
	CHECK(stalls.load() == 0);

#ifdef _WIN32
	os_wake_destroy(&to_cons);
	os_wake_destroy(&to_prod);
#else
	os_wake_destroy(&to_cons);
	os_wake_destroy(&to_prod);
	os_wake_destroy(&cons_end);
	os_wake_destroy(&prod_end);
#endif
}
#endif
