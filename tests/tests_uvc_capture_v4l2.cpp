// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-043 Amendment 4, desktop Linux: the V4L2 UVC capture backend —
 *         every part that needs no camera.
 *
 *  - enumeration from a FAKE sysfs + udev tree (no /dev node is opened):
 *    capture vs metadata nodes (udev's ID_V4L_CAPABILITIES, else the sysfs
 *    index), USB VID:PID + product from the interface's parent, the friendly
 *    name, USB devices ahead of platform (ISP) nodes; and that the result
 *    feeds the platform-neutral matching rule (vid_pid, name, ambiguity, a
 *    plug-in's platform_device_hint) the same way Windows' does;
 *  - format selection: exact size, closest rate, NV12 > YUYV > MJPEG, the
 *    "hardware only" policy excluding MJPEG, unusable formats ignored, the
 *    mode list folding duplicates across pixel formats;
 *  - MJPEG: header parse, the standard-DHT insertion for frames that omit
 *    their Huffman tables (UVC's AVI1 convention), DCT-scale choice, chroma
 *    subsampling -> NV12, RGB -> NV12, the built-in decoder against a known
 *    SBS image (and libturbojpeg when it loads; SKIPped otherwise);
 *  - the real backend end to end on a `file:` MJPEG stream: list_modes ->
 *    open -> read every frame -> end of stream -> close, then the SBS split
 *    gives each eye its own half.
 */

#include "catch_amalgamated.hpp"

#include "os/os_uvc_capture.h"
#include "os/os_uvc_capture_v4l2.h"
#include "os/os_uvc_mjpeg.h"
#include "util/u_stereo_camera.h"
#include "util/u_stereo_uvc.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <linux/videodev2.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

/*
 * A scratch directory tree.
 */
struct temp_dir
{
	std::string path;
	temp_dir()
	{
		char tmpl[] = "/tmp/dxr_uvc_v4l2_XXXXXX";
		const char *p = mkdtemp(tmpl);
		REQUIRE(p != nullptr);
		path = p;
	}
	~temp_dir()
	{
		std::string cmd = "rm -rf '" + path + "'";
		(void)!system(cmd.c_str());
	}
	std::string
	mkdir_p(const std::string &rel) const
	{
		std::string full = path;
		size_t i = 0;
		while (i <= rel.size()) {
			size_t j = rel.find('/', i);
			if (j == std::string::npos) {
				j = rel.size();
			}
			full += "/" + rel.substr(i, j - i);
			::mkdir(full.c_str(), 0755);
			i = j + 1;
		}
		return full;
	}
	void
	write(const std::string &rel, const std::string &text) const
	{
		FILE *f = fopen((path + "/" + rel).c_str(), "wb");
		REQUIRE(f != nullptr);
		fwrite(text.data(), 1, text.size(), f);
		fclose(f);
	}
	void
	link(const std::string &target_rel, const std::string &link_rel) const
	{
		REQUIRE(::symlink((path + "/" + target_rel).c_str(), (path + "/" + link_rel).c_str()) == 0);
	}
};

//! A fake /sys/class/video4linux node.
void
add_node(const temp_dir &t,
         const char *node,
         const char *card,
         const char *devnum,
         const char *index,
         const char *device_target_rel)
{
	t.mkdir_p(std::string("sys/") + node);
	t.write(std::string("sys/") + node + "/name", std::string(card) + "\n");
	t.write(std::string("sys/") + node + "/dev", std::string(devnum) + "\n");
	if (index != nullptr) {
		t.write(std::string("sys/") + node + "/index", std::string(index) + "\n");
	}
	if (device_target_rel != nullptr) {
		t.link(device_target_rel, std::string("sys/") + node + "/device");
	}
}

//! A fake USB device with one interface directory; returns the interface's path (relative).
std::string
add_usb(const temp_dir &t, const char *busdev, const char *vid, const char *pid, const char *product)
{
	std::string dev = std::string("devices/usb1/") + busdev;
	t.mkdir_p(dev + "/" + busdev + ":1.0");
	t.write(dev + "/idVendor", std::string(vid) + "\n");
	t.write(dev + "/idProduct", std::string(pid) + "\n");
	if (product != nullptr) {
		t.write(dev + "/product", std::string(product) + "\n");
	}
	return dev + "/" + busdev + ":1.0";
}

std::vector<u_stereo_uvc_device>
enumerate_fake(const temp_dir &t)
{
	std::vector<u_stereo_uvc_device> devs(U_STEREO_UVC_MAX_DEVICES);
	uint32_t n = os_uvc_v4l2_enumerate_at((t.path + "/sys").c_str(), (t.path + "/udev").c_str(), "/dev",
	                                      devs.data(), (uint32_t)devs.size());
	devs.resize(n);
	return devs;
}

u_stereo_uvc_config
parse_ok(const char *json)
{
	u_stereo_uvc_config cfg;
	char err[256] = {0};
	bool ok = u_stereo_uvc_config_parse(json, &cfg, err, sizeof(err));
	INFO(err);
	REQUIRE(ok);
	return cfg;
}


/*
 * JPEG fixtures.
 */

void
append_bytes(void *ctx, void *data, int size)
{
	auto *v = static_cast<std::vector<uint8_t> *>(ctx);
	v->insert(v->end(), (uint8_t *)data, (uint8_t *)data + size);
}

//! An SBS RGB picture: the left half a bright horizontal ramp, the right half dark and flat-ish.
std::vector<uint8_t>
sbs_rgb(uint32_t w, uint32_t h)
{
	std::vector<uint8_t> rgb((size_t)w * h * 3);
	for (uint32_t y = 0; y < h; y++) {
		for (uint32_t x = 0; x < w; x++) {
			uint8_t *p = &rgb[((size_t)y * w + x) * 3];
			if (x < w / 2) {
				const uint8_t v = (uint8_t)(160 + (x * 80) / (w / 2)); // 160..240
				p[0] = v;
				p[1] = v;
				p[2] = v;
			} else {
				p[0] = 40;
				p[1] = (uint8_t)(40 + (y % 8));
				p[2] = 40;
			}
		}
	}
	return rgb;
}

std::vector<uint8_t>
encode_jpeg(const std::vector<uint8_t> &rgb, uint32_t w, uint32_t h, int quality)
{
	std::vector<uint8_t> out;
	REQUIRE(stbi_write_jpg_to_func(append_bytes, &out, (int)w, (int)h, 3, rgb.data(), quality) != 0);
	return out;
}

//! Remove every DHT segment before SOS (what a UVC camera's MJPEG frame looks like).
std::vector<uint8_t>
strip_dht(const std::vector<uint8_t> &j)
{
	std::vector<uint8_t> out(j.begin(), j.begin() + 2);
	size_t i = 2;
	while (i + 4 <= j.size()) {
		REQUIRE(j[i] == 0xFF);
		const uint8_t m = j[i + 1];
		const size_t seg = ((size_t)j[i + 2] << 8) | j[i + 3];
		if (m == 0xDA) {
			out.insert(out.end(), j.begin() + (long)i, j.end());
			return out;
		}
		if (m != 0xC4) {
			out.insert(out.end(), j.begin() + (long)i, j.begin() + (long)(i + 2 + seg));
		}
		i += 2 + seg;
	}
	FAIL("no SOS");
	return out;
}

double
mean_y(const u_stereo_uvc_raw_frame &f, uint32_t x0, uint32_t x1)
{
	double s = 0;
	for (uint32_t y = 0; y < f.height; y++) {
		for (uint32_t x = x0; x < x1; x++) {
			s += f.planes[0][(size_t)y * f.pitches[0] + x];
		}
	}
	return s / ((double)(x1 - x0) * f.height);
}

// libjpeg-turbo's scaling factors, in its own order.
const int k_tj_num[] = {2, 15, 7, 13, 3, 11, 5, 9, 1, 7, 3, 5, 1, 3, 1, 1};
const int k_tj_den[] = {1, 8, 4, 8, 2, 8, 4, 8, 1, 8, 4, 8, 2, 8, 4, 8};

} // namespace


TEST_CASE("v4l2: friendly name — the card when it carries the USB product, else product (card)")
{
	char out[128];
	os_uvc_v4l2_friendly_name("Integrated Camera: Integrated C", "Integrated Camera", out, sizeof(out));
	CHECK(std::string(out) == "Integrated Camera: Integrated C");
	// The kernel truncates the card to 31 bytes: the product no longer fits in it.
	os_uvc_v4l2_friendly_name("Acme Stereo 3D Webcam Pro: Acme", "Acme Stereo 3D Webcam Pro Max", out, sizeof(out));
	CHECK(std::string(out) == "Acme Stereo 3D Webcam Pro Max (Acme Stereo 3D Webcam Pro: Acme)");
	os_uvc_v4l2_friendly_name("ipu7 isys capture 0", nullptr, out, sizeof(out));
	CHECK(std::string(out) == "ipu7 isys capture 0");
	os_uvc_v4l2_friendly_name("", "Only Product", out, sizeof(out));
	CHECK(std::string(out) == "Only Product");
}

TEST_CASE("v4l2: udev record — ID_V4L_CAPABILITIES decides capture vs metadata")
{
	CHECK(os_uvc_v4l2_udev_capture("S:v4l/by-id/x\nE:ID_V4L_VERSION=2\nE:ID_V4L_CAPABILITIES=:capture:\n") == 1);
	CHECK(os_uvc_v4l2_udev_capture("E:ID_V4L_CAPABILITIES=:video_output:capture:\nE:X=1") == 1);
	CHECK(os_uvc_v4l2_udev_capture("E:ID_V4L_VERSION=2\nE:ID_V4L_CAPABILITIES=:\n") == 0);
	CHECK(os_uvc_v4l2_udev_capture("E:ID_V4L_VERSION=2\nE:ID_VENDOR_ID=04f2\n") == -1);
	CHECK(os_uvc_v4l2_udev_capture("") == -1);
	CHECK(os_uvc_v4l2_udev_capture(nullptr) == -1);
}

TEST_CASE("v4l2: enumeration from sysfs + udev, no device opened")
{
	temp_dir t;
	t.mkdir_p("udev");
	const std::string stereo = add_usb(t, "1-2", "1234", "abcd", "Stereo Eyes");
	const std::string laptop = add_usb(t, "3-4", "04f2", "b87a", "Integrated Camera");
	t.mkdir_p("devices/platform/ipu");
	// video0: the stereo webcam's capture node; video1: its UVC metadata node.
	add_node(t, "video0", "Stereo Eyes: Stereo Eyes", "81:0", "0", stereo.c_str());
	t.write("udev/c81:0", "E:ID_V4L_VERSION=2\nE:ID_V4L_CAPABILITIES=:capture:\n");
	add_node(t, "video1", "Stereo Eyes: Stereo Eyes", "81:1", "1", stereo.c_str());
	t.write("udev/c81:1", "E:ID_V4L_VERSION=2\nE:ID_V4L_CAPABILITIES=:\n");
	// video2 / video3: a platform ISP, no udev record: the sysfs index decides.
	add_node(t, "video2", "ipu7 isys capture 0", "81:2", "0", "devices/platform/ipu");
	add_node(t, "video3", "ipu7 isys meta 0", "81:3", "1", "devices/platform/ipu");
	// video10: the laptop camera, listed after the ISP by node number.
	add_node(t, "video10", "Integrated Camera: Integrated C", "81:10", "0", laptop.c_str());
	t.write("udev/c81:10", "E:ID_V4L_CAPABILITIES=:capture:\n");
	// Not a video node at all.
	t.mkdir_p("sys/v4l-subdev0");

	auto devs = enumerate_fake(t);
	REQUIRE(devs.size() == 3);
	// USB first, in node order; then the platform node.
	CHECK(std::string(devs[0].id) == "/dev/video0");
	CHECK(std::string(devs[0].name) == "Stereo Eyes: Stereo Eyes");
	CHECK(devs[0].has_vid_pid);
	CHECK(devs[0].vid == 0x1234);
	CHECK(devs[0].pid == 0xabcd);
	CHECK(std::string(devs[1].id) == "/dev/video10");
	CHECK(devs[1].vid == 0x04f2);
	CHECK(devs[1].pid == 0xb87a);
	CHECK(std::string(devs[2].id) == "/dev/video2");
	CHECK_FALSE(devs[2].has_vid_pid);
	CHECK(std::string(devs[2].name) == "ipu7 isys capture 0");

	SECTION("the platform-neutral matching rule over V4L2 devices")
	{
		auto cfg = parse_ok(R"({"uvc":[{"vid_pid":"1234:ABCD","layout":"sbs-half"}]})");
		int32_t a[U_STEREO_UVC_MAX_ENTRIES];
		u_stereo_uvc_assign(&cfg, devs.data(), (uint32_t)devs.size(), nullptr, a);
		CHECK(a[0] == 0);

		// A Windows-style friendly-name substring matches the Linux name too.
		cfg = parse_ok(R"({"uvc":[{"match":"stereo eyes"}]})");
		u_stereo_uvc_assign(&cfg, devs.data(), (uint32_t)devs.size(), nullptr, a);
		CHECK(a[0] == 0);

		// A loose name matching several devices claims none.
		cfg = parse_ok(R"({"uvc":[{"match":"e"}]})");
		u_stereo_uvc_assign(&cfg, devs.data(), (uint32_t)devs.size(), nullptr, a);
		CHECK(a[0] == U_STEREO_UVC_ASSIGN_AMBIGUOUS);

		// A plug-in naming the node, or its VID:PID, keeps it.
		CHECK(u_stereo_uvc_device_claimed_by_hint(&devs[0], "/dev/video0"));
		CHECK(u_stereo_uvc_device_claimed_by_hint(&devs[0], "1234:ABCD"));
		CHECK_FALSE(u_stereo_uvc_device_claimed_by_hint(&devs[1], "/dev/video0"));

		// ADR-043 Amendment 5: a declared tracker camera is refused even for an
		// explicit VID:PID entry (the CLI and the service share this rule).
		const char *declared[] = {"1234:abcd"};
		u_stereo_uvc_denylist dl{nullptr, 0, declared, 1};
		CHECK(u_stereo_uvc_refusal(&devs[0], &dl) == U_STEREO_UVC_REFUSAL_PLUGIN_DECLARED);
		CHECK(u_stereo_uvc_refusal(&devs[1], &dl) == U_STEREO_UVC_REFUSAL_NONE);
	}
}

TEST_CASE("v4l2: enumeration of a missing sysfs dir is empty")
{
	u_stereo_uvc_device d[2];
	CHECK(os_uvc_v4l2_enumerate_at("/nonexistent/dxr/sys", "/nonexistent", "/dev", d, 2) == 0);
}

TEST_CASE("v4l2: format selection — exact size, closest rate, NV12 > YUYV > MJPEG")
{
	const os_uvc_v4l2_format f[] = {
	    {V4L2_PIX_FMT_H264, 1280, 720, 30.0f},  // unusable
	    {V4L2_PIX_FMT_MJPEG, 1280, 720, 30.0f}, //
	    {V4L2_PIX_FMT_MJPEG, 1280, 720, 15.0f}, //
	    {V4L2_PIX_FMT_YUYV, 1280, 720, 10.0f},  //
	    {V4L2_PIX_FMT_YUYV, 640, 480, 30.0f},   //
	    {V4L2_PIX_FMT_NV12, 640, 480, 30.0f},   //
	    {V4L2_PIX_FMT_MJPEG, 640, 480, 30.0f},  //
	    {V4L2_PIX_FMT_JPEG, 2560, 720, 60.0f},  // "JPEG" = MJPEG on some cameras
	};
	const uint32_t n = sizeof(f) / sizeof(f[0]);
	os_uvc_v4l2_format p;
	u_stereo_uvc_mode want = {1280, 720, 0.0f};
	REQUIRE(os_uvc_v4l2_pick_format(f, n, &want, true, &p));
	CHECK(p.fourcc == V4L2_PIX_FMT_MJPEG); // the fastest at that size
	CHECK(p.fps == 30.0f);
	want.fps = 10.0f;
	REQUIRE(os_uvc_v4l2_pick_format(f, n, &want, true, &p));
	CHECK(p.fourcc == V4L2_PIX_FMT_YUYV);
	want.fps = 14.0f;
	REQUIRE(os_uvc_v4l2_pick_format(f, n, &want, true, &p));
	CHECK(p.fourcc == V4L2_PIX_FMT_MJPEG);
	CHECK(p.fps == 15.0f);
	want = {640, 480, 0.0f};
	REQUIRE(os_uvc_v4l2_pick_format(f, n, &want, true, &p));
	CHECK(p.fourcc == V4L2_PIX_FMT_NV12); // rate tie: the best rank
	// "hardware only" on Linux = no MJPEG decoder: YUYV even though slower.
	want = {1280, 720, 0.0f};
	REQUIRE(os_uvc_v4l2_pick_format(f, n, &want, false, &p));
	CHECK(p.fourcc == V4L2_PIX_FMT_YUYV);
	want = {2560, 720, 0.0f};
	REQUIRE(os_uvc_v4l2_pick_format(f, n, &want, true, &p));
	CHECK(p.fourcc == V4L2_PIX_FMT_JPEG);
	CHECK_FALSE(os_uvc_v4l2_pick_format(f, n, &want, false, &p));
	want = {1920, 1080, 0.0f};
	CHECK_FALSE(os_uvc_v4l2_pick_format(f, n, &want, true, &p));

	u_stereo_uvc_mode modes[16];
	uint32_t nm = os_uvc_v4l2_modes_from_formats(f, n, modes, 16);
	// 1280x720 @30/@15/@10, 640x480 @30 (folded across 3 formats), 2560x720 @60; H264 ignored.
	CHECK(nm == 5);
	CHECK(os_uvc_v4l2_fourcc_rank(V4L2_PIX_FMT_H264) == 0);
}

TEST_CASE("mjpeg: header, DHT detection and the standard-table insertion")
{
	const uint32_t W = 64, H = 32;
	auto jpg = encode_jpeg(sbs_rgb(W, H), W, H, 80);
	uint32_t w = 0, h = 0;
	REQUIRE(os_uvc_mjpeg_header(jpg.data(), jpg.size(), &w, &h));
	CHECK(w == W);
	CHECK(h == H);
	CHECK(os_uvc_mjpeg_has_dht(jpg.data(), jpg.size()));

	auto bare = strip_dht(jpg);
	CHECK(bare.size() == jpg.size() - OS_UVC_MJPEG_DHT_SIZE); // stb writes the same one standard DHT segment
	CHECK_FALSE(os_uvc_mjpeg_has_dht(bare.data(), bare.size()));
	REQUIRE(os_uvc_mjpeg_header(bare.data(), bare.size(), &w, &h));

	std::vector<uint8_t> fixed(bare.size() + OS_UVC_MJPEG_DHT_SIZE);
	size_t n = os_uvc_mjpeg_insert_dht(bare.data(), bare.size(), fixed.data(), fixed.size());
	REQUIRE(n == bare.size() + OS_UVC_MJPEG_DHT_SIZE);
	CHECK(os_uvc_mjpeg_has_dht(fixed.data(), n));
	// The inserted segment is byte for byte the Annex K table stb_image_write emits.
	const uint8_t dht_marker[] = {0xFF, 0xC4};
	auto at = std::search(jpg.begin(), jpg.end(), dht_marker, dht_marker + 2);
	REQUIRE(at != jpg.end());
	CHECK(std::equal(fixed.begin() + 2, fixed.begin() + 2 + OS_UVC_MJPEG_DHT_SIZE, at));
	CHECK(os_uvc_mjpeg_insert_dht(bare.data(), bare.size(), fixed.data(), fixed.size() - 1) == 0);
	const uint8_t not_jpeg[] = {0x00, 0x11, 0x22, 0x33};
	CHECK(os_uvc_mjpeg_insert_dht(not_jpeg, 4, fixed.data(), fixed.size()) == 0);
	CHECK_FALSE(os_uvc_mjpeg_header(not_jpeg, 4, &w, &h));

	// Decoding the bare frame (tables inserted on the fly) gives exactly the original's pixels.
	os_uvc_mjpeg *d = os_uvc_mjpeg_create(OS_UVC_MJPEG_BUILTIN);
	REQUIRE(d != nullptr);
	u_stereo_uvc_raw_frame a{}, b{};
	REQUIRE(os_uvc_mjpeg_decode(d, jpg.data(), jpg.size(), 0, 0, &a));
	std::vector<uint8_t> ya(a.planes[0], a.planes[0] + (size_t)a.pitches[0] * a.height);
	REQUIRE(os_uvc_mjpeg_decode(d, bare.data(), bare.size(), 0, 0, &b));
	std::vector<uint8_t> yb(b.planes[0], b.planes[0] + (size_t)b.pitches[0] * b.height);
	CHECK(ya == yb);
	// A truncated / garbage frame is dropped, not fatal.
	CHECK_FALSE(os_uvc_mjpeg_decode(d, jpg.data(), 40, 0, 0, &a));
	CHECK_FALSE(os_uvc_mjpeg_decode(d, not_jpeg, 4, 0, 0, &a));
	os_uvc_mjpeg_destroy(&d);
	CHECK(d == nullptr);
}

TEST_CASE("mjpeg: DCT-domain scale choice")
{
	const uint32_t n = sizeof(k_tj_num) / sizeof(k_tj_num[0]);
	uint32_t num = 0, den = 0;
	// Half-SBS 3840x2160 for a 2560x720 output: 1/2 is too narrow, 5/8 gives 2400 < 2560, 3/4 fits.
	os_uvc_mjpeg_pick_scale(k_tj_num, k_tj_den, n, 3840, 2160, 2560, 720, &num, &den);
	CHECK(num == 3);
	CHECK(den == 4);
	// Full-SBS 2560x960 for 1280x480: exactly 1/2.
	os_uvc_mjpeg_pick_scale(k_tj_num, k_tj_den, n, 2560, 960, 1280, 480, &num, &den);
	CHECK(num == 1);
	CHECK(den == 2);
	// Already the output size, or no preference: no scaling.
	os_uvc_mjpeg_pick_scale(k_tj_num, k_tj_den, n, 2560, 720, 2560, 720, &num, &den);
	CHECK(num == den);
	os_uvc_mjpeg_pick_scale(k_tj_num, k_tj_den, n, 3840, 2160, 0, 0, &num, &den);
	CHECK(num == den);
	// Every reduction lands on a width not a multiple of 4 or an odd height: no scaling.
	os_uvc_mjpeg_pick_scale(k_tj_num, k_tj_den, n, 1000, 750, 100, 100, &num, &den);
	CHECK(num == den);
}

TEST_CASE("mjpeg: chroma planes at any subsampling -> NV12")
{
	const uint32_t W = 8, H = 4;
	std::vector<uint8_t> uv((W / 2) * 2 * (H / 2), 0);
	SECTION("4:2:0 copies")
	{
		uint8_t u[2][4] = {{1, 2, 3, 4}, {5, 6, 7, 8}}, v[2][4] = {{11, 12, 13, 14}, {15, 16, 17, 18}};
		os_uvc_mjpeg_chroma_to_nv12(&u[0][0], &v[0][0], 4, 2, 2, W, H, uv.data(), W);
		CHECK(uv[0] == 1);
		CHECK(uv[1] == 11);
		CHECK(uv[6] == 4);
		CHECK(uv[7] == 14);
		CHECK(uv[W + 0] == 5);
		CHECK(uv[W + 7] == 18);
	}
	SECTION("4:2:2 averages row pairs")
	{
		uint8_t u[4][4] = {{10, 0, 0, 0}, {20, 0, 0, 0}, {0, 0, 0, 100}, {0, 0, 0, 51}};
		uint8_t v[4][4] = {{0}};
		os_uvc_mjpeg_chroma_to_nv12(&u[0][0], &v[0][0], 4, 2, 1, W, H, uv.data(), W);
		CHECK(uv[0] == 15);
		CHECK(uv[W + 6] == 76); // (100 + 51 + 1) / 2, rounded
	}
	SECTION("4:4:4 averages 2x2")
	{
		uint8_t u[4][8] = {{0}}, v[4][8] = {{0}};
		u[0][0] = 4;
		u[0][1] = 8;
		u[1][0] = 12;
		u[1][1] = 16;
		os_uvc_mjpeg_chroma_to_nv12(&u[0][0], &v[0][0], 8, 1, 1, W, H, uv.data(), W);
		CHECK(uv[0] == 10);
	}
	SECTION("greyscale is neutral")
	{
		os_uvc_mjpeg_chroma_to_nv12(nullptr, nullptr, 0, 1, 1, W, H, uv.data(), W);
		for (uint8_t c : uv) {
			CHECK(c == 128);
		}
	}
}

TEST_CASE("mjpeg: RGB -> full-range BT.601 NV12")
{
	auto one = [](uint8_t r, uint8_t g, uint8_t b, int ey, int eu, int ev) {
		uint8_t rgb[2 * 2 * 3];
		for (int i = 0; i < 4; i++) {
			rgb[3 * i] = r;
			rgb[3 * i + 1] = g;
			rgb[3 * i + 2] = b;
		}
		uint8_t y[4], uv[2];
		os_uvc_mjpeg_rgb_to_nv12(rgb, 6, 2, 2, y, 2, uv, 2);
		CHECK(std::abs((int)y[0] - ey) <= 1);
		CHECK(std::abs((int)uv[0] - eu) <= 1);
		CHECK(std::abs((int)uv[1] - ev) <= 1);
	};
	one(255, 255, 255, 255, 128, 128);
	one(0, 0, 0, 0, 128, 128);
	one(255, 0, 0, 76, 85, 255);
	one(0, 0, 255, 29, 255, 107);
}

TEST_CASE("mjpeg: the built-in decoder against a known SBS picture, then the split")
{
	const uint32_t W = 320, H = 120;
	auto rgb = sbs_rgb(W, H);
	for (int quality : {80, 95}) { // 4:2:0 and 4:4:4
		auto jpg = strip_dht(encode_jpeg(rgb, W, H, quality));
		os_uvc_mjpeg *d = os_uvc_mjpeg_create(OS_UVC_MJPEG_BUILTIN);
		REQUIRE(d != nullptr);
		CHECK(std::string(os_uvc_mjpeg_name(d)) == "built-in (stb_image)");
		u_stereo_uvc_raw_frame f{};
		REQUIRE(os_uvc_mjpeg_decode(d, jpg.data(), jpg.size(), 160, 60, &f)); // no scaling in the built-in
		CHECK(f.pixel == U_STEREO_UVC_PIXEL_NV12);
		CHECK(f.width == W);
		CHECK(f.height == H);
		CHECK(std::fabs(mean_y(f, 0, W / 2) - 200.0) < 4.0);
		CHECK(std::fabs(mean_y(f, W / 2, W) - 41.0) < 4.0);

		for (bool swap : {false, true}) {
			u_stereo_camera_planes lay{};
			REQUIRE(u_stereo_camera_layout(XRT_PLUGIN_STEREO_CAMERA_FORMAT_NV12, W, H, &lay));
			std::vector<uint8_t> dst((size_t)lay.size);
			REQUIRE(u_stereo_uvc_split(&f, swap, W / 2, H, dst.data(), &lay));
			u_stereo_uvc_raw_frame o{};
			o.width = W;
			o.height = H;
			o.planes[0] = dst.data() + lay.offset[0];
			o.pitches[0] = lay.pitch[0];
			const double l = mean_y(o, 0, W / 2), r = mean_y(o, W / 2, W);
			CHECK((swap ? r : l) > 190.0);
			CHECK((swap ? l : r) < 50.0);
		}
		os_uvc_mjpeg_destroy(&d);
	}
}

TEST_CASE("mjpeg: libturbojpeg (when loadable) matches the built-in and scales in the DCT domain")
{
	char why[256] = {0};
	if (!os_uvc_mjpeg_turbojpeg_available(why, sizeof(why))) {
		SKIP("libturbojpeg not loadable: " << why);
	}
	INFO("libturbojpeg: " << why);
	const uint32_t W = 640, H = 240;
	auto rgb = sbs_rgb(W, H);
	for (int quality : {80, 95}) {
		auto jpg = strip_dht(encode_jpeg(rgb, W, H, quality));
		os_uvc_mjpeg *tj = os_uvc_mjpeg_create(OS_UVC_MJPEG_TURBOJPEG);
		os_uvc_mjpeg *bi = os_uvc_mjpeg_create(OS_UVC_MJPEG_BUILTIN);
		REQUIRE(tj != nullptr);
		REQUIRE(bi != nullptr);
		CHECK(std::string(os_uvc_mjpeg_name(tj)) == "libturbojpeg");
		u_stereo_uvc_raw_frame a{}, b{};
		REQUIRE(os_uvc_mjpeg_decode(tj, jpg.data(), jpg.size(), 0, 0, &a));
		REQUIRE(os_uvc_mjpeg_decode(bi, jpg.data(), jpg.size(), 0, 0, &b));
		REQUIRE(a.width == W);
		REQUIRE(a.height == H);
		double diff = 0, cdiff = 0;
		for (uint32_t y = 0; y < H; y++) {
			for (uint32_t x = 0; x < W; x++) {
				diff += std::abs((int)a.planes[0][(size_t)y * a.pitches[0] + x] -
				                 (int)b.planes[0][(size_t)y * b.pitches[0] + x]);
			}
		}
		for (uint32_t y = 0; y < H / 2; y++) {
			for (uint32_t x = 0; x < W; x++) {
				cdiff += std::abs((int)a.planes[1][(size_t)y * a.pitches[1] + x] -
				                  (int)b.planes[1][(size_t)y * b.pitches[1] + x]);
			}
		}
		CHECK(diff / (W * H) < 2.0);
		CHECK(cdiff / (W * H / 2) < 3.0);
		// Asked for a quarter of the area: 1/2 DCT scale, the same picture.
		REQUIRE(os_uvc_mjpeg_decode(tj, jpg.data(), jpg.size(), W / 2, H / 2, &a));
		CHECK(a.width == W / 2);
		CHECK(a.height == H / 2);
		CHECK(std::fabs(mean_y(a, 0, W / 4) - 200.0) < 4.0);
		CHECK(std::fabs(mean_y(a, W / 4, W / 2) - 41.0) < 4.0);
		os_uvc_mjpeg_destroy(&tj);
		os_uvc_mjpeg_destroy(&bi);
	}
}

TEST_CASE("v4l2: an MJPEG stream file splits into its frames")
{
	const uint32_t W = 64, H = 32;
	auto j = encode_jpeg(sbs_rgb(W, H), W, H, 80);
	// A JPEG whose APP segment carries a stray EOI (an EXIF thumbnail does) — not a frame end.
	std::vector<uint8_t> app = {0xFF, 0xE1, 0x00, 0x08, 0xFF, 0xD8, 0x00, 0xFF, 0xD9, 0x00};
	std::vector<uint8_t> j2(j.begin(), j.begin() + 2);
	j2.insert(j2.end(), app.begin(), app.end());
	j2.insert(j2.end(), j.begin() + 2, j.end());
	std::vector<uint8_t> s;
	s.insert(s.end(), j.begin(), j.end());
	s.push_back(0x00); // padding between frames
	s.push_back(0x00);
	s.insert(s.end(), j2.begin(), j2.end());
	s.insert(s.end(), j.begin(), j.end());
	s.insert(s.end(), j.begin(), j.begin() + (long)(j.size() / 2)); // truncated tail
	size_t off[8], len[8];
	uint32_t n = os_uvc_v4l2_mjpeg_split_stream(s.data(), s.size(), off, len, 8);
	REQUIRE(n == 3);
	CHECK(off[0] == 0);
	CHECK(len[0] == j.size());
	CHECK(off[1] == j.size() + 2);
	CHECK(len[1] == j2.size());
	CHECK(off[2] == j.size() + 2 + j2.size());
	CHECK(len[2] == j.size());
}

TEST_CASE("v4l2: the real backend end to end on a file: MJPEG stream")
{
	u_stereo_uvc_backend b{};
	REQUIRE(os_uvc_capture_backend(&b));
	CHECK(std::string(b.name) == "v4l2");

	char report[2048] = {0};
	os_uvc_capture_decoder_report(report, sizeof(report));
	CHECK(std::string(report).find("decoder policy") != std::string::npos);

	const uint32_t W = 256, H = 96;
	auto rgb = sbs_rgb(W, H);
	temp_dir t;
	const std::string path = t.path + "/sbs.mjpeg";
	{
		FILE *f = fopen(path.c_str(), "wb");
		REQUIRE(f != nullptr);
		for (int i = 0; i < 5; i++) {
			auto j = strip_dht(encode_jpeg(rgb, W, H, 85)); // camera-style: no Huffman tables
			fwrite(j.data(), 1, j.size(), f);
		}
		fclose(f);
	}
	const std::string id = "file:" + path;
	u_stereo_uvc_mode modes[4];
	REQUIRE(b.list_modes(b.ctx, id.c_str(), modes, 4) == 1);
	CHECK(modes[0].width == W);
	CHECK(modes[0].height == H);
	CHECK(b.list_modes(b.ctx, "file:/nonexistent/dxr.mjpeg", modes, 4) == 0);

	void *h = nullptr;
	REQUIRE(b.open(b.ctx, id.c_str(), &modes[0], W, H, &h));
	u_stereo_uvc_raw_frame f{};
	for (int i = 0; i < 5; i++) {
		REQUIRE(b.read(h, 100000000, &f) == U_STEREO_UVC_READ_OK);
		CHECK(f.width == W);
		CHECK(f.height == H);
		CHECK(f.time_ns > 0);
	}
	u_stereo_camera_planes lay{};
	REQUIRE(u_stereo_camera_layout(XRT_PLUGIN_STEREO_CAMERA_FORMAT_NV12, W, H, &lay));
	std::vector<uint8_t> dst((size_t)lay.size);
	REQUIRE(u_stereo_uvc_split(&f, false, W / 2, H, dst.data(), &lay));
	u_stereo_uvc_raw_frame o{};
	o.width = W;
	o.height = H;
	o.planes[0] = dst.data() + lay.offset[0];
	o.pitches[0] = lay.pitch[0];
	CHECK(mean_y(o, 0, W / 2) > 190.0);
	CHECK(mean_y(o, W / 2, W) < 50.0);
	CHECK(b.read(h, 100000000, &f) == U_STEREO_UVC_READ_ERROR); // end of stream
	b.close(h);

	// A device id that does not exist fails to open cleanly.
	void *h2 = nullptr;
	const u_stereo_uvc_mode m = {640, 480, 30.0f};
	CHECK_FALSE(b.open(b.ctx, "/nonexistent/dxr-video99", &m, 0, 0, &h2));
	CHECK(h2 == nullptr);
	CHECK(b.list_modes(b.ctx, "/nonexistent/dxr-video99", modes, 4) == 0);
}
