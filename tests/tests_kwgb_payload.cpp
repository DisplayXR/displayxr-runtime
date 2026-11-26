// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-kwin-geometry-bridge's input contract
 *         (targets/kwin_geometry_bridge/kwgb_payload.c): schema 1 only, no
 *         titles, only KWin, only KDE.
 */

#include "catch_amalgamated.hpp"

#include "kwgb_payload.h"

#include <string>

namespace {

std::string
sanitize(const char *in)
{
	char *out = kwgb_sanitize(in);
	if (out == nullptr) {
		return "<null>";
	}
	std::string s(out);
	kwgb_free(out);
	return s;
}

} // namespace

TEST_CASE("kwgb: a KWin script snapshot passes through")
{
	const char *in = R"({"version":1,"layout_mode":"logical","windows":[{"pid":42,"app_id":"cube","focus":true,)"
	                 R"("frame":[10,20,800,600],"buffer":[10,20,800,600],)"
	                 R"("monitor":{"x":0,"y":0,"w":1280,"h":800,"scale":1.25,"device_scale":1.25}}]})";
	const std::string out = sanitize(in);
	REQUIRE(out != "<null>");
	CHECK(out.find("\"pid\":42") != std::string::npos);
	CHECK(out.find("\"device_scale\":1.25") != std::string::npos);
	CHECK(out.find("\"layout_mode\":\"logical\"") != std::string::npos);
}

TEST_CASE("kwgb: titles are stripped from every window")
{
	const std::string out =
	    sanitize(R"({"version":1,"windows":[{"pid":1,"title":"secret.pdf"},{"pid":2,"title":"bank"}]})");
	REQUIRE(out != "<null>");
	CHECK(out.find("title") == std::string::npos);
	CHECK(out.find("secret") == std::string::npos);
	CHECK(out.find("\"pid\":2") != std::string::npos);
}

TEST_CASE("kwgb: a missing version is version 1; any other version is refused")
{
	const std::string v = sanitize(R"({"windows":[]})");
	CHECK(v.find("\"version\":1") != std::string::npos);
	CHECK(sanitize(R"({"version":2,"windows":[]})") == "<null>");
	CHECK(sanitize(R"({"version":"1","windows":[]})") == "<null>");
}

TEST_CASE("kwgb: malformed input is refused")
{
	CHECK(sanitize(nullptr) == "<null>");
	CHECK(sanitize("") == "<null>");
	CHECK(sanitize("not json") == "<null>");
	CHECK(sanitize("[1,2,3]") == "<null>");
	CHECK(sanitize(R"({"version":1})") == "<null>");                 // no windows
	CHECK(sanitize(R"({"version":1,"windows":{}})") == "<null>");    // not an array
	CHECK(sanitize(R"({"version":1,"windows":[1,2]})") == "<null>"); // entries not objects
	const std::string huge =
	    std::string(R"({"version":1,"windows":[],"pad":")") + std::string(KWGB_MAX_PAYLOAD, 'x') + "\"}";
	CHECK(sanitize(huge.c_str()) == "<null>");
}

TEST_CASE("kwgb: the empty snapshot is itself a valid snapshot")
{
	CHECK(sanitize(KWGB_EMPTY_SNAPSHOT) != "<null>");
}

TEST_CASE("kwgb: only KWin's compositor process may publish")
{
	CHECK(kwgb_comm_is_kwin("kwin_wayland"));
	CHECK(kwgb_comm_is_kwin("kwin_wayland\n"));
	CHECK(kwgb_comm_is_kwin("kwin_x11\n"));
	CHECK_FALSE(kwgb_comm_is_kwin("kwin_wayland_w")); // KWin's Xwayland launcher helper, not KWin
	CHECK_FALSE(kwgb_comm_is_kwin("python3\n"));
	CHECK_FALSE(kwgb_comm_is_kwin("kwin"));
	CHECK_FALSE(kwgb_comm_is_kwin(""));
	CHECK_FALSE(kwgb_comm_is_kwin(nullptr));
}

TEST_CASE("kwgb: KDE sessions only")
{
	CHECK(kwgb_desktop_is_kde("KDE"));
	CHECK(kwgb_desktop_is_kde("kde"));
	CHECK(kwgb_desktop_is_kde("X-Generic:KDE"));
	CHECK_FALSE(kwgb_desktop_is_kde("GNOME"));
	CHECK_FALSE(kwgb_desktop_is_kde("ubuntu:GNOME"));
	CHECK_FALSE(kwgb_desktop_is_kde("gamescope"));
	CHECK_FALSE(kwgb_desktop_is_kde("KDEX"));
	CHECK_FALSE(kwgb_desktop_is_kde(""));
	CHECK_FALSE(kwgb_desktop_is_kde(nullptr));
}
