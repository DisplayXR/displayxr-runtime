// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  conv_* IPC wire family (ADR-053): every field of every wire struct
 *         survives a round trip.
 *
 * The regression shape for G1 (aux-depth caps dropped over IPC because a
 * field-by-field copy forgot two fields). The claim "every field crosses" is
 * made mechanical, not reviewed:
 *
 *  - each source struct is filled with a byte pattern (every byte non-zero,
 *    no float is NaN), packed, sent as raw bytes, received with
 *    xrt_conv_wire_receive and unpacked into a zeroed struct;
 *  - the result is compared BYTEWISE with the source, after zeroing only the
 *    fields the helper documents as not carried (process-local / identity);
 *  - every message is packed twice, from two sources whose bytes all differ,
 *    and the two wire structs must differ in every byte outside the header
 *    and the reserved fields, so a wire field the pack forgot to fill (or
 *    filled from the wrong place) also fails.
 *
 * Adding a field to an ABI struct or a wire struct without carrying it makes
 * one of these comparisons fail.
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_conv_wire.h"

#include <cstddef>
#include <cstring>
#include <utility>
#include <vector>

extern "C" int
tests_conv_wire_c_roundtrip(void);

namespace {

using range = std::pair<size_t, size_t>; //!< [offset, offset + size)

#define FIELD_RANGE(T, F) range(offsetof(T, F), offsetof(T, F) + sizeof(((T *)0)->F))

//! Every byte in 1..63: never zero, and the top byte of every float stays
//! below 0x40 so no pattern is a NaN (bit-exact copies through float lvalues).
template <typename T>
void
fill(T &t, unsigned seed)
{
	unsigned char *p = reinterpret_cast<unsigned char *>(&t);
	for (size_t i = 0; i < sizeof(T); i++) {
		p[i] = (unsigned char)(((i * 29u) + seed) % 63u + 1u);
	}
}

template <typename T>
void
zero(T &t, const std::vector<range> &ranges)
{
	unsigned char *p = reinterpret_cast<unsigned char *>(&t);
	for (const range &r : ranges) {
		std::memset(p + r.first, 0, r.second - r.first);
	}
}

template <typename T>
void
require_same_bytes(const T &expected, const T &actual)
{
	const unsigned char *a = reinterpret_cast<const unsigned char *>(&expected);
	const unsigned char *b = reinterpret_cast<const unsigned char *>(&actual);
	size_t bad = sizeof(T);
	for (size_t i = 0; i < sizeof(T) && bad == sizeof(T); i++) {
		if (a[i] != b[i]) {
			bad = i;
		}
	}
	INFO("first differing byte offset " << bad << " of " << sizeof(T));
	REQUIRE(bad == sizeof(T));
}

//! Two packs from two different sources: every wire byte outside @p fixed
//! must differ between them, i.e. every wire byte was written from the source
//! (a wire field the pack forgot keeps the same bytes in both and fails here).
template <typename T>
void
require_all_from_source(const T &w1, const T &w2, const std::vector<range> &fixed)
{
	const unsigned char *a = reinterpret_cast<const unsigned char *>(&w1);
	const unsigned char *b = reinterpret_cast<const unsigned char *>(&w2);
	size_t bad = sizeof(T);
	for (size_t i = 0; i < sizeof(T) && bad == sizeof(T); i++) {
		bool skip = false;
		for (const range &r : fixed) {
			skip = skip || (i >= r.first && i < r.second);
		}
		if (!skip && a[i] == b[i]) {
			bad = i;
		}
	}
	INFO("wire byte offset " << bad << " of " << sizeof(T) << " does not come from the source");
	REQUIRE(bad == sizeof(T));
}

//! Every byte of a u32 / u64 argument differs between run k and run k + 1.
uint32_t
rep32(unsigned k)
{
	return 0x01010101u * k;
}

uint64_t
rep64(unsigned k)
{
	return 0x0101010101010101ull * k;
}

const range kHeader(0, XRT_CONV_WIRE_HEADER_BYTES);

//! Send @p w as the transport would (struct_size bytes, opaque) and receive it.
template <typename T>
T
transmit(const T &w)
{
	uint32_t size = 0;
	std::memcpy(&size, &w, sizeof(size));
	REQUIRE(size == sizeof(T));
	std::vector<unsigned char> bytes(size);
	std::memcpy(bytes.data(), &w, size);
	T r;
	REQUIRE(xrt_conv_wire_receive(&r, (uint32_t)sizeof(r), bytes.data(), (uint32_t)bytes.size()));
	require_same_bytes(w, r);
	return r;
}

//! xrt_conv_image fields a wire image never carries (re-materialised per process).
std::vector<range>
image_local(size_t base)
{
	using I = struct xrt_conv_image;
	std::vector<range> v = {FIELD_RANGE(I, reserved_0), FIELD_RANGE(I, handle),   FIELD_RANGE(I, import_key),
	                        FIELD_RANGE(I, cpu_ptr),    FIELD_RANGE(I, sync.reserved_0), FIELD_RANGE(I, sync.handle),
	                        FIELD_RANGE(I, sync.import_key)};
	for (range &r : v) {
		r.first += base;
		r.second += base;
	}
	return v;
}

range
at(size_t base, range r)
{
	return range(base + r.first, base + r.second);
}

template <typename A, typename W>
void
twin_round_trip(void (*pack)(const A *, W *), void (*unpack)(const W *, A *), unsigned seed)
{
	A a[2], b;
	W w[2];
	for (unsigned k = 0; k < 2; k++) {
		fill(a[k], seed + k);
		fill(w[k], 40 + k); // pack must overwrite every byte
		pack(&a[k], &w[k]);
	}
	require_all_from_source(w[0], w[1], {});
	std::memset(&b, 0, sizeof(b));
	unpack(&w[0], &b);
	require_same_bytes(a[0], b);
	// One copy: the twin is byte-identical to the ABI struct.
	REQUIRE(std::memcmp(&w[0], &a[0], sizeof(A)) == 0);
}

} // namespace


/*
 *
 * Frozen twins.
 *
 */

TEST_CASE("conv wire: frozen twins round-trip every field", "[conv_wire]")
{
	twin_round_trip(xrt_conv_wire_pack_depth_info, xrt_conv_wire_unpack_depth_info, 1);
	twin_round_trip(xrt_conv_wire_pack_views_info, xrt_conv_wire_unpack_views_info, 3);
	twin_round_trip(xrt_conv_wire_pack_splat_info, xrt_conv_wire_unpack_splat_info, 5);
	twin_round_trip(xrt_conv_wire_pack_stereo_calibration, xrt_conv_wire_unpack_stereo_calibration, 7);
}


/*
 *
 * Image + params blocks.
 *
 */

TEST_CASE("conv wire: image descriptor carries every non-process-local field", "[conv_wire]")
{
	using WI = struct xrt_conv_wire_image;
	struct xrt_conv_image a[2], b;
	WI w[2];
	for (unsigned k = 0; k < 2; k++) {
		fill(a[k], 9 + k);
		fill(w[k], 40 + k);
		xrt_conv_wire_pack_image(&a[k], rep32(k + 1), rep64(k + 1), &w[k]);
	}
	require_all_from_source(w[0], w[1], {FIELD_RANGE(WI, reserved_0)});
	CHECK(w[0].reserved_0 == 0u);

	uint32_t role = 0;
	uint64_t shm = 0;
	fill(b, 20); // unpack must clear the process-local fields itself
	xrt_conv_wire_unpack_image(&w[0], &b, &role, &shm);
	CHECK(role == rep32(1));
	CHECK(shm == rep64(1));

	struct xrt_conv_image expected = a[0];
	zero(expected, image_local(0));
	require_same_bytes(expected, b);
}

TEST_CASE("conv wire: params carry every field but struct_size", "[conv_wire]")
{
	using WP = struct xrt_conv_wire_params;
	struct xrt_conv_params a[2], b;
	WP w[2];
	for (unsigned k = 0; k < 2; k++) {
		fill(a[k], 11 + k);
		a[k].struct_size = (uint32_t)sizeof(a[k]);
		fill(w[k], 40 + k);
		xrt_conv_wire_pack_params(&a[k], &w[k]);
	}
	require_all_from_source(w[0], w[1], {FIELD_RANGE(WP, reserved_0)});
	CHECK(w[0].reserved_0 == 0u);
	fill(b, 21);
	xrt_conv_wire_unpack_params(&w[0], &b);
	require_same_bytes(a[0], b);
}


/*
 *
 * Messages with an ABI counterpart.
 *
 */

TEST_CASE("conv wire: conv_enum_task carries the capability + readiness, never identity", "[conv_wire]")
{
	using W = struct xrt_conv_wire_task_desc;
	struct xrt_conv_capability cap[2];
	struct xrt_conv_state st[2];
	W w[2];
	for (unsigned k = 0; k < 2; k++) {
		fill(cap[k], 13 + k);
		fill(st[k], 15 + k);
		xrt_conv_wire_pack_task_desc(&cap[k], &st[k], rep32(k + 1), &w[k]);
	}
	CHECK(w[0].struct_size == sizeof(W));
	CHECK(w[0].family_version == XRT_CONV_WIRE_FAMILY_VERSION_CURRENT);
	require_all_from_source(w[0], w[1], {kHeader});

	W r = transmit(w[0]);

	struct xrt_conv_capability cap2;
	struct xrt_conv_state st2;
	uint32_t flags = 0;
	xrt_conv_wire_unpack_task_desc(&r, &cap2, &st2, &flags);
	CHECK(flags == rep32(1));

	using C = struct xrt_conv_capability;
	using S = struct xrt_conv_state;
	struct xrt_conv_capability cap_expected = cap[0];
	zero(cap_expected, {FIELD_RANGE(C, cap_id), FIELD_RANGE(C, licence_spdx), FIELD_RANGE(C, model_id),
	                    FIELD_RANGE(C, model_version), FIELD_RANGE(C, backend), FIELD_RANGE(C, display_name),
	                    FIELD_RANGE(C, reserved_0)});
	cap_expected.struct_size = (uint32_t)sizeof(cap_expected);
	require_same_bytes(cap_expected, cap2);

	struct xrt_conv_state st_expected = st[0];
	zero(st_expected, {FIELD_RANGE(S, reserved_0), FIELD_RANGE(S, version), FIELD_RANGE(S, detail)});
	st_expected.struct_size = (uint32_t)sizeof(st_expected);
	require_same_bytes(st_expected, st2);
}

TEST_CASE("conv wire: G1 shape - aux-depth capability survives the wire", "[conv_wire]")
{
	struct xrt_conv_capability cap;
	std::memset(&cap, 0, sizeof(cap));
	cap.struct_size = (uint32_t)sizeof(cap);
	cap.cap_id = 1;
	cap.task = XRT_CONV_TASK_VIEWS_MONO;
	cap.lanes = XRT_CONV_LANE_STREAM;
	cap.outputs = XRT_CONV_OUT_VIEWS | XRT_CONV_OUT_DEPTH; // same-inference aux depth
	cap.depth_kind = XRT_CONV_DEPTH_AFFINE_INVERSE;
	cap.depth_output_format = XRT_CONV_FMT_R32_FLOAT;
	struct xrt_conv_state st;
	std::memset(&st, 0, sizeof(st));
	st.struct_size = (uint32_t)sizeof(st);
	st.state = XRT_CONV_STATE_READY;

	struct xrt_conv_wire_task_desc w;
	xrt_conv_wire_pack_task_desc(&cap, &st, 0, &w);
	struct xrt_conv_wire_task_desc r = transmit(w);
	struct xrt_conv_capability got;
	struct xrt_conv_state got_st;
	xrt_conv_wire_unpack_task_desc(&r, &got, &got_st, nullptr);
	CHECK((got.outputs & XRT_CONV_OUT_DEPTH) != 0);
	CHECK(got.depth_kind == XRT_CONV_DEPTH_AFFINE_INVERSE);
	CHECK(got.depth_output_format == XRT_CONV_FMT_R32_FLOAT);
	CHECK(got_st.state == XRT_CONV_STATE_READY);
}

TEST_CASE("conv wire: conv_stream_create request carries every stream_info field", "[conv_wire]")
{
	using W = struct xrt_conv_wire_stream_desc;
	struct xrt_conv_stream_info info[2];
	W w[2];
	for (unsigned k = 0; k < 2; k++) {
		fill(info[k], 17 + k);
		info[k].struct_size = (uint32_t)sizeof(info[k]);
		xrt_conv_wire_pack_stream_desc(rep32(k + 1), rep32(k + 3), rep32(k + 5), &info[k], &w[k]);
	}
	require_all_from_source(w[0], w[1], {kHeader, FIELD_RANGE(W, reserved_0), FIELD_RANGE(W, reserved_1)});

	W r = transmit(w[0]);
	struct xrt_conv_stream_info back;
	uint32_t task = 0, quality = 0, export_kind = 0;
	xrt_conv_wire_unpack_stream_desc(&r, &task, &quality, &export_kind, &back);
	CHECK(task == rep32(1));
	CHECK(quality == rep32(3));
	CHECK(export_kind == rep32(5));
	require_same_bytes(info[0], back);
}

TEST_CASE("conv wire: conv_submit carries images, params and depth_in_info", "[conv_wire]")
{
	using W = struct xrt_conv_wire_submit;
	using WI = struct xrt_conv_wire_image;
	using WP = struct xrt_conv_wire_params;
	struct xrt_conv_frame_in in[2];
	struct xrt_conv_params params[2];
	W w[2];
	for (unsigned k = 0; k < 2; k++) {
		fill(in[k], 19 + k);
		fill(params[k], 21 + k);
		in[k].struct_size = (uint32_t)sizeof(in[k]);
		in[k].image_count = XRT_CONV_WIRE_MAX_SUBMIT_IMAGES;
		params[k].struct_size = (uint32_t)sizeof(params[k]);
		const uint64_t shm[2] = {rep64(k + 1), rep64(k + 3)};
		REQUIRE(xrt_conv_wire_pack_submit(rep64(k + 5), &in[k], shm, &params[k], &w[k]));
	}
	std::vector<range> fixed = {kHeader, FIELD_RANGE(W, image_count), FIELD_RANGE(W, reserved_0),
	                            at(offsetof(W, params), FIELD_RANGE(WP, reserved_0))};
	for (uint32_t i = 0; i < XRT_CONV_WIRE_MAX_SUBMIT_IMAGES; i++) {
		fixed.push_back(at(offsetof(W, images) + i * sizeof(WI), FIELD_RANGE(WI, reserved_0)));
	}
	require_all_from_source(w[0], w[1], fixed);

	W r = transmit(w[0]);
	uint64_t stream_id = 0;
	uint64_t shm_back[2] = {0, 0};
	struct xrt_conv_frame_in back;
	struct xrt_conv_params params_back;
	REQUIRE(xrt_conv_wire_unpack_submit(&r, &stream_id, &back, shm_back, &params_back));
	CHECK(stream_id == rep64(5));
	CHECK(shm_back[0] == rep64(1));
	CHECK(shm_back[1] == rep64(3));
	require_same_bytes(params[0], params_back);

	using F = struct xrt_conv_frame_in;
	struct xrt_conv_frame_in expected = in[0];
	for (uint32_t i = 0; i < XRT_CONV_WIRE_MAX_SUBMIT_IMAGES; i++) {
		zero(expected, image_local(offsetof(F, images) + i * sizeof(struct xrt_conv_image)));
	}
	// Slots past the wire's array are not carried (and not used).
	for (uint32_t i = XRT_CONV_WIRE_MAX_SUBMIT_IMAGES; i < XRT_CONV_MAX_FRAME_IMAGES; i++) {
		expected.image_role[i] = 0;
		std::memset(&expected.images[i], 0, sizeof(expected.images[i]));
	}
	require_same_bytes(expected, back);

	// Too many images is refused, never truncated silently.
	in[0].image_count = XRT_CONV_WIRE_MAX_SUBMIT_IMAGES + 1;
	CHECK_FALSE(xrt_conv_wire_pack_submit(1, &in[0], nullptr, &params[0], &w[0]));
}

TEST_CASE("conv wire: conv_acquire_result carries extents, views and depth info", "[conv_wire]")
{
	using W = struct xrt_conv_wire_result;
	struct xrt_conv_frame_out fo[2];
	W w[2];
	for (unsigned k = 0; k < 2; k++) {
		fill(fo[k], 23 + k);
		fo[k].struct_size = (uint32_t)sizeof(fo[k]);
		xrt_conv_wire_pack_result(rep64(k + 1), rep64(k + 3), rep64(k + 5), rep64(k + 7), rep32(k + 9), &fo[k],
		                          &w[k]);
	}
	require_all_from_source(w[0], w[1], {kHeader, FIELD_RANGE(W, reserved_0)});

	W r = transmit(w[0]);
	uint64_t stream_id = 0, frame_id = 0, source_time = 0, latency = 0;
	uint32_t realloc_flag = 0;
	struct xrt_conv_frame_out back;
	xrt_conv_wire_unpack_result(&r, &stream_id, &frame_id, &source_time, &latency, &realloc_flag, &back);
	CHECK(stream_id == rep64(1));
	CHECK(frame_id == rep64(3));
	CHECK(source_time == rep64(5));
	CHECK(latency == rep64(7));
	CHECK(realloc_flag == rep32(9));
	CHECK(back.struct_size == sizeof(back));
	CHECK(back.written == fo[0].written);

	const struct xrt_conv_image *src[4] = {&fo[0].views, &fo[0].depth, &fo[0].confidence, &fo[0].disparity};
	const struct xrt_conv_image *dst[4] = {&back.views, &back.depth, &back.confidence, &back.disparity};
	for (int i = 0; i < 4; i++) {
		INFO("target " << i);
		CHECK(dst[i]->width == src[i]->width);
		CHECK(dst[i]->height == src[i]->height);
		CHECK(dst[i]->format == src[i]->format);
		// Handles never ride this message.
		CHECK(dst[i]->handle == 0u);
		CHECK(dst[i]->memory_kind == 0u);
	}
	require_same_bytes(fo[0].views_info, back.views_info);
	require_same_bytes(fo[0].depth_info, back.depth_info);
}

TEST_CASE("conv wire: conv_job_submit carries every non-reserved job_in field", "[conv_wire]")
{
	using W = struct xrt_conv_wire_job_desc;
	using WI = struct xrt_conv_wire_image;
	using WP = struct xrt_conv_wire_params;
	struct xrt_conv_job_in in[2];
	struct xrt_conv_params params[2];
	W w[2];
	for (unsigned k = 0; k < 2; k++) {
		fill(in[k], 25 + k);
		fill(params[k], 27 + k);
		in[k].struct_size = (uint32_t)sizeof(in[k]);
		in[k].image_count = XRT_CONV_MAX_FRAME_IMAGES;
		params[k].struct_size = (uint32_t)sizeof(params[k]);
		const uint64_t shm[4] = {rep64(k + 1), rep64(k + 3), rep64(k + 5), rep64(k + 7)};
		REQUIRE(xrt_conv_wire_pack_job_desc(rep32(k + 9), rep32(k + 11), &in[k], shm, &params[k], &w[k]));
	}
	std::vector<range> fixed = {kHeader, FIELD_RANGE(W, image_count), FIELD_RANGE(W, reserved_0),
	                            at(offsetof(W, params), FIELD_RANGE(WP, reserved_0))};
	for (uint32_t i = 0; i < XRT_CONV_MAX_FRAME_IMAGES; i++) {
		fixed.push_back(at(offsetof(W, images) + i * sizeof(WI), FIELD_RANGE(WI, reserved_0)));
	}
	require_all_from_source(w[0], w[1], fixed);

	W r = transmit(w[0]);
	uint32_t task = 0, quality = 0;
	uint64_t shm_back[4] = {0, 0, 0, 0};
	struct xrt_conv_job_in back;
	struct xrt_conv_params params_back;
	REQUIRE(xrt_conv_wire_unpack_job_desc(&r, &task, &quality, &back, shm_back, &params_back));
	CHECK(task == rep32(9));
	CHECK(quality == rep32(11));
	for (unsigned i = 0; i < 4; i++) {
		CHECK(shm_back[i] == rep64(2 * i + 1));
	}
	require_same_bytes(params[0], params_back);

	using J = struct xrt_conv_job_in;
	struct xrt_conv_job_in expected = in[0];
	for (uint32_t i = 0; i < XRT_CONV_MAX_FRAME_IMAGES; i++) {
		zero(expected, image_local(offsetof(J, images) + i * sizeof(struct xrt_conv_image)));
	}
	// Reserved in v1 (XRT_CONV_INPUT_ENCODED): not on the wire.
	zero(expected, {FIELD_RANGE(J, encoded_count), FIELD_RANGE(J, encoded), FIELD_RANGE(J, encoded_size),
	                FIELD_RANGE(J, reserved_0)});
	require_same_bytes(expected, back);
}

TEST_CASE("conv wire: conv_job_fetch carries the job_out info blocks", "[conv_wire]")
{
	using W = struct xrt_conv_wire_job_result;
	struct xrt_conv_job_out jo[2];
	W w[2];
	for (unsigned k = 0; k < 2; k++) {
		fill(jo[k], 29 + k);
		jo[k].struct_size = (uint32_t)sizeof(jo[k]);
		xrt_conv_wire_pack_job_result(rep64(k + 1), rep32(k + 3), rep32(k + 5), rep64(k + 7), &jo[k], &w[k]);
	}
	require_all_from_source(w[0], w[1], {kHeader, FIELD_RANGE(W, reserved_0), FIELD_RANGE(W, reserved_1)});

	W r = transmit(w[0]);
	uint64_t job_id = 0, byte_size = 0;
	uint32_t blob_format = 0, delivery = 0;
	struct xrt_conv_job_out back;
	xrt_conv_wire_unpack_job_result(&r, &job_id, &blob_format, &delivery, &byte_size, &back);
	CHECK(job_id == rep64(1));
	CHECK(blob_format == rep32(3));
	CHECK(delivery == rep32(5));
	CHECK(byte_size == rep64(7));
	CHECK(back.struct_size == sizeof(back));
	CHECK(back.written == jo[0].written);
	require_same_bytes(jo[0].splat_info, back.splat_info);
	require_same_bytes(jo[0].depth_info, back.depth_info);
	require_same_bytes(jo[0].views_info, back.views_info);
}


/*
 *
 * Plain messages (no ABI counterpart): opaque transport round trip.
 *
 */

namespace {

template <typename T>
void
plain_round_trip(unsigned seed)
{
	T w;
	fill(w, seed);
	const uint32_t hdr[2] = {(uint32_t)sizeof(T), XRT_CONV_WIRE_FAMILY_VERSION_CURRENT};
	std::memcpy(&w, hdr, sizeof(hdr));
	(void)transmit(w);
}

} // namespace

TEST_CASE("conv wire: plain messages round-trip bytewise", "[conv_wire]")
{
	plain_round_trip<struct xrt_conv_wire_hint_interest>(20);
	plain_round_trip<struct xrt_conv_wire_generation>(21);
	plain_round_trip<struct xrt_conv_wire_stream_created>(22);
	plain_round_trip<struct xrt_conv_wire_job_status>(23);
	plain_round_trip<struct xrt_conv_wire_task_request>(24);
	plain_round_trip<struct xrt_conv_wire_event>(25);
}


/*
 *
 * Receive semantics.
 *
 */

TEST_CASE("conv wire: receive zero-extends an older sender and clamps a newer one", "[conv_wire]")
{
	struct xrt_conv_wire_result full;
	fill(full, 30);
	const uint32_t hdr[2] = {(uint32_t)sizeof(full), XRT_CONV_WIRE_FAMILY_VERSION_CURRENT};
	std::memcpy(&full, hdr, sizeof(hdr));

	SECTION("older sender: fields past its struct_size read as zero")
	{
		const uint32_t older = (uint32_t)offsetof(struct xrt_conv_wire_result, views_info);
		struct xrt_conv_wire_result sent = full;
		sent.struct_size = older;
		struct xrt_conv_wire_result r;
		REQUIRE(xrt_conv_wire_receive(&r, (uint32_t)sizeof(r), &sent, older));
		CHECK(r.struct_size == older);
		CHECK(std::memcmp(&r, &sent, older) == 0);
		const unsigned char *p = reinterpret_cast<const unsigned char *>(&r);
		size_t nonzero = 0;
		for (size_t i = older; i < sizeof(r); i++) {
			nonzero += p[i] != 0 ? 1u : 0u;
		}
		CHECK(nonzero == 0u);
	}
	SECTION("newer sender: the receiver keeps what it knows, struct_size clamped")
	{
		std::vector<unsigned char> bytes(sizeof(full) + 64, 0x5A);
		struct xrt_conv_wire_result sent = full;
		sent.struct_size = (uint32_t)bytes.size();
		std::memcpy(bytes.data(), &sent, sizeof(sent));
		struct xrt_conv_wire_result r;
		REQUIRE(xrt_conv_wire_receive(&r, (uint32_t)sizeof(r), bytes.data(), (uint32_t)bytes.size()));
		CHECK(r.struct_size == sizeof(r));
		CHECK(std::memcmp(reinterpret_cast<const unsigned char *>(&r) + 4,
		                  reinterpret_cast<const unsigned char *>(&sent) + 4, sizeof(r) - 4) == 0);
	}
	SECTION("malformed headers are refused and leave the receiver zeroed")
	{
		struct xrt_conv_wire_result r;
		struct xrt_conv_wire_result bad = full;
		bad.family_version = XRT_CONV_WIRE_FAMILY_VERSION_CURRENT + 1;
		CHECK_FALSE(xrt_conv_wire_receive(&r, (uint32_t)sizeof(r), &bad, (uint32_t)sizeof(bad)));
		CHECK(r.struct_size == 0u);

		bad = full;
		bad.struct_size = 4;
		CHECK_FALSE(xrt_conv_wire_receive(&r, (uint32_t)sizeof(r), &bad, (uint32_t)sizeof(bad)));

		bad = full;
		CHECK_FALSE(xrt_conv_wire_receive(&r, (uint32_t)sizeof(r), &bad, (uint32_t)sizeof(bad) - 1));
		CHECK_FALSE(xrt_conv_wire_receive(&r, (uint32_t)sizeof(r), nullptr, 64));
		CHECK_FALSE(xrt_conv_wire_receive(&r, (uint32_t)sizeof(r), &bad, 4));
	}
}

TEST_CASE("conv wire: every message fits one IPC buffer", "[conv_wire]")
{
	STATIC_REQUIRE(sizeof(struct xrt_conv_wire_hint_interest) <= XRT_CONV_WIRE_MSG_MAX_BYTES);
	STATIC_REQUIRE(sizeof(struct xrt_conv_wire_generation) <= XRT_CONV_WIRE_MSG_MAX_BYTES);
	STATIC_REQUIRE(sizeof(struct xrt_conv_wire_task_desc) <= XRT_CONV_WIRE_MSG_MAX_BYTES);
	STATIC_REQUIRE(sizeof(struct xrt_conv_wire_stream_desc) <= XRT_CONV_WIRE_MSG_MAX_BYTES);
	STATIC_REQUIRE(sizeof(struct xrt_conv_wire_stream_created) <= XRT_CONV_WIRE_MSG_MAX_BYTES);
	STATIC_REQUIRE(sizeof(struct xrt_conv_wire_submit) <= XRT_CONV_WIRE_MSG_MAX_BYTES);
	STATIC_REQUIRE(sizeof(struct xrt_conv_wire_result) <= XRT_CONV_WIRE_MSG_MAX_BYTES);
	STATIC_REQUIRE(sizeof(struct xrt_conv_wire_job_desc) <= XRT_CONV_WIRE_MSG_MAX_BYTES);
	STATIC_REQUIRE(sizeof(struct xrt_conv_wire_job_status) <= XRT_CONV_WIRE_MSG_MAX_BYTES);
	STATIC_REQUIRE(sizeof(struct xrt_conv_wire_job_result) <= XRT_CONV_WIRE_MSG_MAX_BYTES);
	STATIC_REQUIRE(sizeof(struct xrt_conv_wire_task_request) <= XRT_CONV_WIRE_MSG_MAX_BYTES);
	STATIC_REQUIRE(sizeof(struct xrt_conv_wire_event) <= XRT_CONV_WIRE_MSG_MAX_BYTES);
}

TEST_CASE("conv wire: the helpers behave the same from a C11 translation unit", "[conv_wire]")
{
	CHECK(tests_conv_wire_c_roundtrip() == 0);
}
