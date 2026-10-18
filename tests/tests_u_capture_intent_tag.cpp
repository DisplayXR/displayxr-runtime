// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pin the capture-trigger plumbing the service weave engine uses (#2207):
 *         the per-client tag trigger, the input / atlas / woven sibling names,
 *         and the readback -> RGBA8 conversion (BGRA swap, raw-alpha opt-in).
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_config_os.h"
#include "util/u_capture_intent.h"
#include "util/u_image_capture.h"

#include <string>

#ifndef XRT_OS_WINDOWS
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

TEST_CASE("sibling path: the infix goes before the extension of the last component")
{
	char out[512];
	u_capture_intent_sibling_path("/tmp/x/displayxr_atlas.png", ".woven", out, sizeof(out));
	CHECK(std::string(out) == "/tmp/x/displayxr_atlas.woven.png");
	u_capture_intent_sibling_path("/tmp/x/displayxr_atlas.png", "", out, sizeof(out));
	CHECK(std::string(out) == "/tmp/x/displayxr_atlas.png");
	// Per-exe output: only the LAST dot is the extension.
	u_capture_intent_sibling_path("/tmp/displayxr_atlas.chrome.png", ".input", out, sizeof(out));
	CHECK(std::string(out) == "/tmp/displayxr_atlas.chrome.input.png");
	// A dot in a directory name is not an extension.
	u_capture_intent_sibling_path("/tmp/a.b/atlas", ".woven", out, sizeof(out));
	CHECK(std::string(out) == "/tmp/a.b/atlas.woven.png");
	u_capture_intent_sibling_path("C:\\T.mp\\atlas", ".woven", out, sizeof(out));
	CHECK(std::string(out) == "C:\\T.mp\\atlas.woven.png");
}

TEST_CASE("readback conversion: BGRA swaps, alpha forced opaque unless raw")
{
	const uint8_t bgra[8] = {10, 20, 30, 0, 40, 50, 60, 128};
	uint8_t out[8] = {};
	u_image_capture_to_rgba8(out, bgra, 2, 1, 8, true, false);
	CHECK(out[0] == 30);
	CHECK(out[1] == 20);
	CHECK(out[2] == 10);
	CHECK(out[3] == 255);
	CHECK(out[4] == 60);
	CHECK(out[7] == 255);

	u_image_capture_to_rgba8(out, bgra, 2, 1, 8, false, true);
	CHECK(out[0] == 10);
	CHECK(out[2] == 30);
	CHECK(out[3] == 0);
	CHECK(out[7] == 128);
}

TEST_CASE("readback conversion: a padded source stride packs tight")
{
	// 1x2 image, 8-byte rows (4 bytes of padding each).
	const uint8_t src[16] = {1, 2, 3, 4, 9, 9, 9, 9, 5, 6, 7, 8, 9, 9, 9, 9};
	uint8_t out[8] = {};
	u_image_capture_to_rgba8(out, src, 1, 2, 8, false, true);
	const uint8_t want[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	for (int i = 0; i < 8; i++) {
		CHECK(out[i] == want[i]);
	}
}

#ifndef XRT_OS_WINDOWS

namespace {

std::string
make_dir()
{
	char tmpl[] = "/tmp/dxr_capture_tag_XXXXXX";
	const char *d = mkdtemp(tmpl);
	REQUIRE(d != nullptr);
	return d;
}

void
touch(const std::string &p)
{
	int fd = open(p.c_str(), O_CREAT | O_WRONLY, 0600);
	REQUIRE(fd >= 0);
	close(fd);
}

bool
exists(const std::string &p)
{
	struct stat st;
	return stat(p.c_str(), &st) == 0;
}

} // namespace

TEST_CASE("capture dir: TMPDIR with trailing slashes is trimmed")
{
	const std::string d = make_dir();
	setenv("TMPDIR", (d + "//").c_str(), 1);
	char out[512];
	u_capture_intent_dir(out, sizeof(out));
	CHECK(std::string(out) == d);
	rmdir(d.c_str());
}

TEST_CASE("tag trigger: consumed, names the client, projection wins, .exe stripped")
{
	const std::string d = make_dir();
	setenv("TMPDIR", d.c_str(), 1);
	struct u_capture_intent in = {};

	// Nothing there.
	CHECK_FALSE(u_capture_intent_poll_tag(&in, "chrome"));
	CHECK_FALSE(in.pending);

	// POST_COMPOSE.
	touch(d + "/displayxr_atlas_trigger.chrome");
	REQUIRE(u_capture_intent_poll_tag(&in, "chrome"));
	CHECK(in.pending);
	CHECK(in.mode == MCP_CAPTURE_MODE_POST_COMPOSE);
	CHECK(std::string(in.path) == d + "/displayxr_atlas.chrome.png");
	CHECK_FALSE(exists(d + "/displayxr_atlas_trigger.chrome")); // consumed
	CHECK_FALSE(u_capture_intent_poll_tag(&in, "chrome"));      // once only

	// Both present: projection-only first (same priority as the global pair).
	touch(d + "/displayxr_atlas_trigger.chrome");
	touch(d + "/displayxr_atlas_trigger.chrome.projection");
	REQUIRE(u_capture_intent_poll_tag(&in, "chrome"));
	CHECK(in.mode == MCP_CAPTURE_MODE_PROJECTION_ONLY);
	CHECK(std::string(in.path) == d + "/displayxr_atlas.chrome.projection.png");
	REQUIRE(u_capture_intent_poll_tag(&in, "chrome"));
	CHECK(in.mode == MCP_CAPTURE_MODE_POST_COMPOSE);

	// A Windows-style basename keeps the per-exe convention (no ".exe").
	touch(d + "/displayxr_atlas_trigger.SampleGame");
	REQUIRE(u_capture_intent_poll_tag(&in, "SampleGame.exe"));
	CHECK(std::string(in.path) == d + "/displayxr_atlas.SampleGame.png");

	// Another client's trigger is not this client's.
	touch(d + "/displayxr_atlas_trigger.other");
	CHECK_FALSE(u_capture_intent_poll_tag(&in, "chrome"));
	CHECK(exists(d + "/displayxr_atlas_trigger.other"));
	unlink((d + "/displayxr_atlas_trigger.other").c_str());

	// Empty / NULL / path-bearing tags are never polled.
	CHECK_FALSE(u_capture_intent_poll_tag(&in, ""));
	CHECK_FALSE(u_capture_intent_poll_tag(&in, nullptr));
	CHECK_FALSE(u_capture_intent_poll_tag(&in, "../x"));

	rmdir(d.c_str());
}

TEST_CASE("global trigger: u_capture_intent_poll serves the weave engine's fallback")
{
	// u_capture_intent_poll caches its paths on first use, so this is the
	// only case in this binary that calls it.
	const std::string d = make_dir();
	setenv("TMPDIR", d.c_str(), 1);
	struct u_capture_intent in = {};
	touch(d + "/displayxr_atlas_trigger");
	u_capture_intent_poll(&in, nullptr);
	REQUIRE(in.pending);
	CHECK(in.mode == MCP_CAPTURE_MODE_POST_COMPOSE);
	CHECK(std::string(in.path) == d + "/displayxr_atlas.png");
	CHECK_FALSE(exists(d + "/displayxr_atlas_trigger"));

	char woven[600];
	u_capture_intent_sibling_path(in.path, ".woven", woven, sizeof(woven));
	CHECK(std::string(woven) == d + "/displayxr_atlas.woven.png");
	rmdir(d.c_str());
}

#endif // !XRT_OS_WINDOWS
