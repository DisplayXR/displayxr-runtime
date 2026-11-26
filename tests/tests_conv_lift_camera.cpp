// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift spec 6: the camera block a GAUSSIANS lift caller receives
 *         (conv_lift_camera_block) and its runtime focus fallback
 *         (conv_splat_focus_from_ply), provider-agnostic.
 */

#include "xrt/xrt_conversion_plugin.h"

#include "conversion/conv_camera_block.h"
#include "conversion/conv_splat_focus.h"

#include <cjson/cJSON.h>

#include "catch_amalgamated.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Splat
{
	float x, y, z, opacity;
};

//! A binary little-endian 3DGS-like PLY (x y z nx ny nz f_dc_0..2 opacity
//! scale_0..2 rot_0..3), plus a trailing element the reader must ignore.
std::vector<uint8_t>
make_ply(const std::vector<Splat> &s)
{
	std::string h = "ply\nformat binary_little_endian 1.0\nelement vertex " + std::to_string(s.size()) + "\n";
	const char *props[] = {"x",       "y",      "z",      "nx",      "ny",      "nz",
	                       "f_dc_0",  "f_dc_1", "f_dc_2", "opacity", "scale_0", "scale_1",
	                       "scale_2", "rot_0",  "rot_1",  "rot_2",   "rot_3"};
	for (const char *p : props) {
		h += std::string("property float ") + p + "\n";
	}
	h += "element extrinsic 16\nproperty float extrinsic\nend_header\n";
	std::vector<uint8_t> out(h.begin(), h.end());
	for (const Splat &v : s) {
		float rec[17] = {v.x, v.y, v.z, 0, 0, 0, 0.5f, 0.5f, 0.5f, v.opacity, -5, -5, -5, 1, 0, 0, 0};
		const uint8_t *b = reinterpret_cast<const uint8_t *>(rec);
		out.insert(out.end(), b, b + sizeof(rec));
	}
	for (int i = 0; i < 16; i++) {
		const float e = i % 5 == 0 ? 1.0f : 0.0f;
		const uint8_t *b = reinterpret_cast<const uint8_t *>(&e);
		out.insert(out.end(), b, b + sizeof(e));
	}
	return out;
}

/*!
 * A photo-like OpenCV cloud seen by a 1536 x 1024 camera (fx = 1200): a
 * subject filling the central half of the frame at ~@p subject_z, a far
 * background wall at 20 m behind it, and faint floaters right at the lens
 * that must not drive the screen plane. @p rub flips it to +y up, +z back.
 */
std::vector<Splat>
photo_cloud(float subject_z, bool rub)
{
	const float fx = 1200.0f, cx = 768.0f, cy = 512.0f, W = 1536.0f, H = 1024.0f;
	std::vector<Splat> s;
	uint32_t seed = 12345;
	auto rnd = [&] {
		seed = seed * 1664525u + 1013904223u;
		return (float)(seed >> 8) / (float)(1u << 24);
	};
	auto put = [&](float px, float py, float z, float op) {
		Splat v{(px - cx) / fx * z, (py - cy) / fx * z, z, op};
		if (rub) {
			v.y = -v.y;
			v.z = -v.z;
		}
		s.push_back(v);
	};
	for (int i = 0; i < 60000; i++) { // background wall
		put(rnd() * W, rnd() * H, 20.0f + rnd(), 3.0f);
	}
	for (int i = 0; i < 60000; i++) { // subject: central half of the frame, slight relief
		put(W * (0.25f + 0.5f * rnd()), H * (0.25f + 0.5f * rnd()), subject_z * (1.0f + 0.05f * rnd()), 3.0f);
	}
	for (int i = 0; i < 2000; i++) { // faint floaters near the lens
		put(rnd() * W, rnd() * H, 0.05f + 0.02f * rnd(), -6.0f);
	}
	return s;
}

const float kIntr[6] = {1200.0f, 1200.0f, 768.0f, 512.0f, 1536.0f, 1024.0f};

std::vector<uint8_t>
tiny_zip(const std::string &meta)
{
	auto w16 = [](std::vector<uint8_t> &v, uint32_t x) {
		v.push_back((uint8_t)x);
		v.push_back((uint8_t)(x >> 8));
	};
	auto w32 = [&](std::vector<uint8_t> &v, uint32_t x) {
		w16(v, x & 0xffff);
		w16(v, x >> 16);
	};
	std::vector<uint8_t> z;
	const std::string name = "meta.json";
	w32(z, 0x04034b50);
	for (uint32_t v : {20u, 0u, 0u, 0u, 0u}) {
		w16(z, v);
	}
	w32(z, 0);
	w32(z, (uint32_t)meta.size());
	w32(z, (uint32_t)meta.size());
	w16(z, (uint32_t)name.size());
	w16(z, 0);
	z.insert(z.end(), name.begin(), name.end());
	z.insert(z.end(), meta.begin(), meta.end());
	const uint32_t cd = (uint32_t)z.size();
	w32(z, 0x02014b50);
	for (uint32_t v : {20u, 20u, 0u, 0u, 0u, 0u}) {
		w16(z, v);
	}
	w32(z, 0);
	w32(z, (uint32_t)meta.size());
	w32(z, (uint32_t)meta.size());
	w16(z, (uint32_t)name.size());
	for (int i = 0; i < 4; i++) {
		w16(z, 0);
	}
	w32(z, 0);
	w32(z, 0);
	z.insert(z.end(), name.begin(), name.end());
	const uint32_t cd_size = (uint32_t)z.size() - cd;
	w32(z, 0x06054b50);
	for (uint32_t v : {0u, 0u, 1u, 1u}) {
		w16(z, v);
	}
	w32(z, cd_size);
	w32(z, cd);
	w16(z, 0);
	return z;
}

double
num(const cJSON *o, const char *k)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
	return cJSON_IsNumber(v) ? v->valuedouble : NAN;
}

std::string
str(const cJSON *o, const char *k)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
	return cJSON_IsString(v) ? v->valuestring : "";
}

} // namespace

TEST_CASE("splat focus: the PLY reader finds x / y / z / opacity and refuses the rest", "[conv_lift_camera]")
{
	const std::vector<uint8_t> ply = make_ply({{1, 2, 3, 0.5f}, {4, 5, 6, 0.5f}});
	size_t header = 0;
	uint64_t count = 0;
	uint32_t stride = 0;
	int32_t off[4];
	REQUIRE(conv_ply_vertex_layout(ply.data(), ply.size(), &header, &count, &stride, off));
	CHECK(count == 2u);
	CHECK(stride == 17u * 4u);
	CHECK(off[0] == 0);
	CHECK(off[2] == 8);
	CHECK(off[3] == 36);

	CHECK_FALSE(conv_ply_vertex_layout(ply.data(), header + 10, &header, &count, &stride, off)); // truncated
	const std::string ascii =
	    "ply\nformat ascii 1.0\nelement vertex 1\nproperty float x\nproperty float y\n"
	    "property float z\nend_header\n1 2 3\n";
	CHECK_FALSE(conv_ply_vertex_layout(reinterpret_cast<const uint8_t *>(ascii.data()), ascii.size(), &header,
	                                   &count, &stride, off));
	const uint8_t junk[64] = {1, 2, 3};
	CHECK_FALSE(conv_ply_vertex_layout(junk, sizeof(junk), &header, &count, &stride, off));
}

TEST_CASE("splat focus: the screen plane follows the subject, never floaters or the wall", "[conv_lift_camera]")
{
	for (bool with_intrinsics : {true, false}) {
		CAPTURE(with_intrinsics);
		const std::vector<uint8_t> ply = make_ply(photo_cloud(0.6f, false));
		conv_splat_focus f;
		REQUIRE(
		    conv_splat_focus_from_ply(ply.data(), ply.size(), false, with_intrinsics ? kIntr : nullptr, &f));
		CHECK(f.method != CONV_SPLAT_FOCUS_NONE);
		// On the subject (0.6 .. 0.63 m), not the wall at 20 m (measured: 0.605).
		CHECK(f.subject_m >= 0.55f);
		CHECK(f.subject_m <= 0.75f);
		CHECK(f.near_m >= 0.55f); // the faint floaters at 5 cm were ignored
		CHECK(f.far_m >= 15.0f);
		CHECK(f.point[0] == 0.0f);
		CHECK(f.point[2] == Catch::Approx(f.subject_m));
	}
	// RUB: the same scene, +z back -> the point is on -z, same distance.
	const std::vector<uint8_t> rub = make_ply(photo_cloud(0.6f, true));
	conv_splat_focus f;
	REQUIRE(conv_splat_focus_from_ply(rub.data(), rub.size(), true, kIntr, &f));
	CHECK(f.point[2] == Catch::Approx(-f.subject_m));
	CHECK(f.subject_m >= 0.55f);
	CHECK(f.subject_m <= 0.75f);

	// Nothing in front of the camera: no estimate.
	std::vector<Splat> behind;
	for (int i = 0; i < 500; i++) {
		behind.push_back({0.01f * i, 0, -1.0f, 3.0f});
	}
	const std::vector<uint8_t> b = make_ply(behind);
	CHECK_FALSE(conv_splat_focus_from_ply(b.data(), b.size(), false, kIntr, &f));
	CHECK(f.method == CONV_SPLAT_FOCUS_NONE);
}

TEST_CASE("lift camera block: no facts at all -> the runtime's focus from the splats", "[conv_lift_camera]")
{
	const std::vector<uint8_t> ply = make_ply(photo_cloud(0.6f, false));
	char err[128] = {0};
	cJSON *b =
	    conv_lift_camera_block(nullptr, 1.0, 1.0, ply.data(), ply.size(), XRT_CONV_BLOB_PLY_3DGS, err, sizeof(err));
	REQUIRE(b != nullptr);
	CHECK(str(b, "source") == "auto");
	CHECK(str(b, "rig") == "camera");
	CHECK(str(b, "convention") == "opencv");
	// No intrinsics reported -> none written (never a nominal field of view).
	CHECK(cJSON_GetObjectItemCaseSensitive(b, "intrinsics") == nullptr);
	const cJSON *focus = cJSON_GetObjectItemCaseSensitive(b, "focus");
	REQUIRE(cJSON_IsObject(focus));
	CHECK(str(focus, "source") == "runtime");
	CHECK(num(focus, "subject_m") >= 0.55);
	CHECK(num(focus, "subject_m") <= 0.75);
	const cJSON *pt = cJSON_GetObjectItemCaseSensitive(focus, "point");
	REQUIRE(cJSON_GetArraySize(pt) == 3);
	CHECK(cJSON_GetArrayItem(pt, 2)->valuedouble == Catch::Approx(num(focus, "subject_m")).epsilon(1e-5));
	// Same key order as the SOG writer: focus before dxr.
	const cJSON *last = b->child;
	while (last->next != nullptr) {
		last = last->next;
	}
	CHECK(std::string(last->string) == "dxr");
	cJSON_Delete(b);

	// Not a splat format: no block.
	CHECK(conv_lift_camera_block(nullptr, 1.0, 1.0, ply.data(), ply.size(), XRT_CONV_BLOB_SPZ, err, sizeof(err)) ==
	      nullptr);
}

TEST_CASE("lift camera block: facts without a focus suggestion -> intrinsics kept, runtime focus", "[conv_lift_camera]")
{
	xrt_conv_splat_info si{};
	si.coords = XRT_CONV_COORDS_OPENCV;
	si.intrinsics_source = XRT_CONV_INTRINSICS_ESTIMATED;
	si.fx = si.fy = 600.0f; // the provider's pixels: half the caller's photo
	si.cx = 384.0f;
	si.cy = 256.0f;
	si.grid_width = 768;
	si.grid_height = 512;
	cJSON *facts = conv_splat_facts_to_json(&si, nullptr);
	// The facade downscaled a 1536 x 1024 photo by 2: re-express in the caller's pixels.
	conv_splat_facts_scale_intrinsics(facts, 2.0, 2.0);
	const std::vector<uint8_t> ply = make_ply(photo_cloud(0.6f, false));
	cJSON *b = conv_lift_camera_block(facts, 1.0, 1.0, ply.data(), ply.size(), XRT_CONV_BLOB_PLY_3DGS, nullptr, 0);
	REQUIRE(b != nullptr);
	const cJSON *in = cJSON_GetObjectItemCaseSensitive(b, "intrinsics");
	REQUIRE(cJSON_IsObject(in));
	CHECK(num(in, "fx") == Catch::Approx(1200.0));
	CHECK(num(in, "cx") == Catch::Approx(768.0));
	CHECK(num(in, "width") == 1536.0);
	CHECK(num(in, "height") == 1024.0);
	const cJSON *focus = cJSON_GetObjectItemCaseSensitive(b, "focus");
	REQUIRE(cJSON_IsObject(focus));
	CHECK(str(focus, "source") == "runtime");
	CHECK(num(focus, "subject_m") <= 0.75);
	// The facts keep saying where the intrinsics came from.
	CHECK(str(cJSON_GetObjectItemCaseSensitive(facts, "intrinsics"), "source") == "estimated");
	cJSON_Delete(b);
	cJSON_Delete(facts);
}

TEST_CASE("lift camera block: the provider's focus suggestion wins, exactly as the SOG writer emits it",
          "[conv_lift_camera]")
{
	xrt_conv_splat_info si{};
	si.coords = XRT_CONV_COORDS_OPENCV;
	si.intrinsics_source = XRT_CONV_INTRINSICS_ESTIMATED;
	si.fx = si.fy = 1200.0f;
	si.cx = 768.0f;
	si.cy = 512.0f;
	si.grid_width = 1536;
	si.grid_height = 1024;
	xrt_conv_splat_result sr{};
	sr.struct_size = sizeof(sr);
	sr.valid = XRT_CONV_SPLAT_VALID_FOCUS;
	sr.focus_point[2] = 0.6092f;
	sr.focus_subject_m = 0.6092f;
	sr.focus_near_m = 0.5773f;
	sr.focus_far_m = 22.6354f;
	sr.focus_source = XRT_CONV_FOCUS_SOURCE_CONVERGENCE;
	cJSON *facts = conv_splat_facts_to_json(&si, &sr);
	const std::vector<uint8_t> ply = make_ply(photo_cloud(3.0f, false)); // splats disagree: ignored
	cJSON *b = conv_lift_camera_block(facts, 1.0, 1.0, ply.data(), ply.size(), XRT_CONV_BLOB_PLY_3DGS, nullptr, 0);
	REQUIRE(b != nullptr);
	cJSON *ref = conv_camera_block_from_facts(facts, nullptr, 0);
	REQUIRE(ref != nullptr);
	char *a = cJSON_PrintUnformatted(b);
	char *r = cJSON_PrintUnformatted(ref);
	CHECK(std::string(a) == std::string(r)); // byte for byte the `conv run --camera-block auto` object
	const cJSON *focus = cJSON_GetObjectItemCaseSensitive(b, "focus");
	CHECK(num(focus, "subject_m") == Catch::Approx(0.6092));
	CHECK(str(focus, "source") == "auto");
	std::free(a);
	std::free(r);
	cJSON_Delete(ref);
	cJSON_Delete(b);
	cJSON_Delete(facts);
}

TEST_CASE("lift camera block: a SOG's authored block is returned verbatim", "[conv_lift_camera]")
{
	const std::string authored =
	    "{\"count\":1,\"camera\":{\"source\":\"author\",\"rig\":\"display\","
	    "\"focus\":{\"point\":[0,0,2],\"subject_m\":2}}}";
	const std::vector<uint8_t> sog = tiny_zip(authored);
	xrt_conv_splat_info si{};
	si.fx = 1000.0f;
	si.cx = 500.0f;
	si.cy = 400.0f;
	si.grid_width = 1000;
	si.grid_height = 800;
	cJSON *facts = conv_splat_facts_to_json(&si, nullptr);
	cJSON *b = conv_lift_camera_block(facts, 1.0, 1.0, sog.data(), sog.size(), XRT_CONV_BLOB_SOG, nullptr, 0);
	REQUIRE(b != nullptr);
	CHECK(str(b, "source") == "author");
	CHECK(str(b, "rig") == "display");
	CHECK(cJSON_GetObjectItemCaseSensitive(b, "intrinsics") == nullptr); // untouched by the facts
	cJSON_Delete(b);

	// An auto block in the SOG lends its focus when the facts carry none.
	const std::string autob =
	    "{\"count\":1,\"camera\":{\"source\":\"auto\",\"focus\":{\"point\":[0,0,1.25],"
	    "\"subject_m\":1.25,\"source\":\"auto\"}}}";
	const std::vector<uint8_t> sog2 = tiny_zip(autob);
	b = conv_lift_camera_block(facts, 1.0, 1.0, sog2.data(), sog2.size(), XRT_CONV_BLOB_SOG, nullptr, 0);
	REQUIRE(b != nullptr);
	CHECK(num(cJSON_GetObjectItemCaseSensitive(b, "intrinsics"), "fx") == Catch::Approx(1000.0));
	CHECK(num(cJSON_GetObjectItemCaseSensitive(b, "focus"), "subject_m") == Catch::Approx(1.25));
	cJSON_Delete(b);
	cJSON_Delete(facts);
}

/*
 * #2836: an authored SOG block reaches a lift caller with its intrinsics in the
 * caller's pixels (the facts' scale), every other field as authored; the blob
 * itself is never rewritten.
 */

namespace {

//! A provider-authored block in the pixels the provider was handed (a 768 x 512
//! downscale of the caller's 1536 x 1024 photo), with keys the runtime does not know.
const char *const kAuthoredMeta =
    "{\"count\":1,\"camera\":{\"version\":2,\"source\":\"provider\",\"convention\":\"opencv\","
    "\"rig\":\"camera\",\"rest\":{\"position\":[0,0,0],\"rotation\":[0,0,0,1]},"
    "\"intrinsics\":{\"fx\":600.5,\"fy\":601.25,\"cx\":383.75,\"cy\":255.5,\"width\":768,\"height\":511,"
    "\"model\":\"pinhole\"},"
    "\"focus\":{\"point\":[0,0,0.75],\"subject_m\":0.75},\"dxr\":{\"vendor_key\":\"x\"}}}";

//! The facts the same provider reports for that job (its own pixels).
cJSON *
provider_facts()
{
	xrt_conv_splat_info si{};
	si.coords = XRT_CONV_COORDS_OPENCV;
	si.intrinsics_source = XRT_CONV_INTRINSICS_ESTIMATED;
	si.fx = 600.5f;
	si.fy = 601.25f;
	si.cx = 383.75f;
	si.cy = 255.5f;
	si.grid_width = 768;
	si.grid_height = 511;
	return conv_splat_facts_to_json(&si, nullptr);
}

std::string
print(const cJSON *o)
{
	char *p = cJSON_PrintUnformatted(o);
	std::string s = p != nullptr ? p : "";
	std::free(p);
	return s;
}

std::string
print_without_intrinsics(const cJSON *o)
{
	cJSON *d = cJSON_Duplicate(o, true);
	cJSON_DeleteItemFromObjectCaseSensitive(d, "intrinsics");
	std::string s = print(d);
	cJSON_Delete(d);
	return s;
}

} // namespace

TEST_CASE("lift camera block: an authored block's intrinsics follow the facts into the caller's pixels",
          "[conv_lift_camera]")
{
	const std::vector<uint8_t> sog = tiny_zip(kAuthoredMeta);
	const std::vector<uint8_t> sog_before = sog;
	cJSON *meta = cJSON_Parse(kAuthoredMeta);
	const cJSON *authored = cJSON_GetObjectItemCaseSensitive(meta, "camera");

	// The facade downscaled 1536 x 1024 to 768 x 512 (511 rows after the provider's crop):
	// caller pixels = provider pixels x 2 (job_scale_x / job_scale_y).
	const double sx = 2.0, sy = 1024.0 / 512.0;
	cJSON *facts = provider_facts();
	conv_splat_facts_scale_intrinsics(facts, sx, sy);
	cJSON *b = conv_lift_camera_block(facts, sx, sy, sog.data(), sog.size(), XRT_CONV_BLOB_SOG, nullptr, 0);
	REQUIRE(b != nullptr);

	const cJSON *in = cJSON_GetObjectItemCaseSensitive(b, "intrinsics");
	REQUIRE(cJSON_IsObject(in));
	CHECK(num(in, "fx") == 1201.0);
	CHECK(num(in, "fy") == 1202.5);
	CHECK(num(in, "cx") == 767.5);
	CHECK(num(in, "cy") == 511.0);
	CHECK(num(in, "width") == 1536.0);
	CHECK(num(in, "height") == 1022.0);
	CHECK(str(in, "model") == "pinhole"); // an unknown intrinsics key is kept

	// The block and the facts carry the same numbers, printed the same way.
	const cJSON *fin = cJSON_GetObjectItemCaseSensitive(facts, "intrinsics");
	for (const char *k : {"fx", "fy", "cx", "cy", "width", "height"}) {
		INFO(k);
		CHECK(print(cJSON_GetObjectItemCaseSensitive(in, k)) ==
		      print(cJSON_GetObjectItemCaseSensitive(fin, k)));
	}

	// Everything else is the author's, key order included.
	CHECK(print_without_intrinsics(b) == print_without_intrinsics(authored));
	std::string order;
	const cJSON *it = nullptr;
	cJSON_ArrayForEach(it, b)
	{
		order += std::string(it->string) + ",";
	}
	CHECK(order == "version,source,convention,rig,rest,intrinsics,focus,dxr,");

	// The file is never rewritten: the blob bytes are untouched.
	CHECK(sog == sog_before);
	char *meta_after = conv_sog_read_meta_mem(sog.data(), sog.size(), nullptr, 0);
	REQUIRE(meta_after != nullptr);
	CHECK(std::string(meta_after) == kAuthoredMeta);
	std::free(meta_after);

	cJSON_Delete(b);
	cJSON_Delete(facts);
	cJSON_Delete(meta);
}

TEST_CASE("lift camera block: an authored block is verbatim when nothing was downscaled", "[conv_lift_camera]")
{
	const std::vector<uint8_t> sog = tiny_zip(kAuthoredMeta);
	cJSON *meta = cJSON_Parse(kAuthoredMeta);
	cJSON *facts = provider_facts();
	cJSON *b = conv_lift_camera_block(facts, 1.0, 1.0, sog.data(), sog.size(), XRT_CONV_BLOB_SOG, nullptr, 0);
	REQUIRE(b != nullptr);
	CHECK(print(b) == print(cJSON_GetObjectItemCaseSensitive(meta, "camera")));
	cJSON_Delete(b);
	cJSON_Delete(facts);
	cJSON_Delete(meta);
}

TEST_CASE("conv_camera_block_scale_intrinsics: no intrinsics, unit scale and malformed blocks are left alone",
          "[conv_lift_camera]")
{
	// No intrinsics: untouched (a viewer estimates them).
	cJSON *b = cJSON_Parse("{\"source\":\"author\",\"rig\":\"display\",\"focus\":{\"subject_m\":2}}");
	const std::string before = print(b);
	CHECK_FALSE(conv_camera_block_scale_intrinsics(b, 2.0, 2.0));
	CHECK(print(b) == before);
	cJSON_Delete(b);

	// Unit / invalid scale: untouched.
	b = cJSON_Parse("{\"source\":\"author\",\"intrinsics\":{\"fx\":600,\"cx\":300,\"width\":601}}");
	const std::string before2 = print(b);
	CHECK_FALSE(conv_camera_block_scale_intrinsics(b, 1.0, 1.0));
	CHECK_FALSE(conv_camera_block_scale_intrinsics(b, 0.0, 2.0));
	CHECK_FALSE(conv_camera_block_scale_intrinsics(b, 2.0, -1.0));
	CHECK(print(b) == before2);
	// A partial block scales the keys it has, by the facts' rule (width rounded).
	CHECK(conv_camera_block_scale_intrinsics(b, 1.5, 3.0));
	const cJSON *in = cJSON_GetObjectItemCaseSensitive(b, "intrinsics");
	CHECK(num(in, "fx") == 900.0);
	CHECK(num(in, "cx") == 450.0);
	CHECK(num(in, "width") == 902.0); // 901.5 rounds half up, as the facts do
	cJSON_Delete(b);

	// Malformed: intrinsics not an object, or a key that is not a number. As authored, never partly scaled.
	for (const char *j : {"{\"source\":\"author\",\"intrinsics\":[600,600,300,200]}",
	                      "{\"source\":\"author\",\"intrinsics\":\"pinhole\"}",
	                      "{\"source\":\"author\",\"intrinsics\":{\"fx\":600,\"cx\":\"300\",\"width\":600}}",
	                      "{\"source\":\"author\",\"intrinsics\":{\"fx\":600,\"cy\":null}}"}) {
		INFO(j);
		cJSON *m = cJSON_Parse(j);
		REQUIRE(m != nullptr);
		const std::string mb = print(m);
		CHECK_FALSE(conv_camera_block_scale_intrinsics(m, 2.0, 2.0));
		CHECK(print(m) == mb);
		cJSON_Delete(m);
	}

	// Not an object at all, or NULL: no crash, no change.
	cJSON *arr = cJSON_Parse("[1,2,3]");
	CHECK_FALSE(conv_camera_block_scale_intrinsics(arr, 2.0, 2.0));
	cJSON_Delete(arr);
	CHECK_FALSE(conv_camera_block_scale_intrinsics(nullptr, 2.0, 2.0));
}

TEST_CASE("lift camera block: a SOG whose meta.json does not parse falls back to the facts' block",
          "[conv_lift_camera]")
{
	const std::vector<uint8_t> sog = tiny_zip("{\"count\":1,\"camera\":{\"source\":\"provider\",\"intrin");
	cJSON *facts = provider_facts();
	conv_splat_facts_scale_intrinsics(facts, 2.0, 2.0);
	cJSON *b = conv_lift_camera_block(facts, 2.0, 2.0, sog.data(), sog.size(), XRT_CONV_BLOB_SOG, nullptr, 0);
	REQUIRE(b != nullptr);
	CHECK(str(b, "source") == "auto");
	CHECK(num(cJSON_GetObjectItemCaseSensitive(b, "intrinsics"), "fx") == 1201.0); // facts, not doubled twice
	cJSON_Delete(b);
	cJSON_Delete(facts);
}
