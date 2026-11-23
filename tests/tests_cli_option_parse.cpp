// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The pure pieces of the shared `<class> set` verb (cli_option_parse.h).
 *
 * What must hold:
 *  - `set <id> <KEY> <value>` and `set <id> <KEY> --stdin` are the two forms;
 *    both a value and `--stdin` is refused, as are too few arguments;
 *  - `--stdin` reads ONE line: the trailing newline (and a CR before it) is
 *    dropped, nothing after the first line is taken, an empty line is
 *    refused, a line that does not fit the store is refused and the buffer
 *    wiped; every byte past the value is zero.
 */

#include "catch_amalgamated.hpp"

#include "cli_option_parse.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

//! A stream holding @p bytes, read from the start (a real file: tmpfile() may need a writable root on Windows).
struct stream
{
	std::filesystem::path path;
	FILE *f = nullptr;

	explicit stream(const std::string &bytes)
	{
		static int seq = 0;
		path = std::filesystem::temp_directory_path() / ("tests_cli_option_parse_" + std::to_string(++seq) +
		                                                 "_" + std::to_string((uintptr_t)this) + ".txt");
		f = std::fopen(path.string().c_str(), "w+b");
		REQUIRE(f != nullptr);
		if (!bytes.empty()) {
			REQUIRE(std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size());
		}
		std::rewind(f);
	}
	~stream()
	{
		if (f != nullptr) {
			std::fclose(f);
		}
		std::error_code ec;
		std::filesystem::remove(path, ec);
	}
};

bool
zero_past(const char *buf, size_t from, size_t cap)
{
	for (size_t i = from; i < cap; i++) {
		if (buf[i] != '\0') {
			return false;
		}
	}
	return true;
}

} // namespace

TEST_CASE("set: exactly one source for the value", "[cli][set]")
{
	CHECK(cli_set_args_check(3, false) == CLI_SET_ARGS_VALUE);
	CHECK(cli_set_args_check(2, true) == CLI_SET_ARGS_STDIN);
	CHECK(cli_set_args_check(3, true) == CLI_SET_ARGS_BOTH);
	CHECK(cli_set_args_check(2, false) == CLI_SET_ARGS_MISSING);
	CHECK(cli_set_args_check(1, true) == CLI_SET_ARGS_MISSING);
	CHECK(cli_set_args_check(0, false) == CLI_SET_ARGS_MISSING);
}

TEST_CASE("set --stdin: one line, newline stripped", "[cli][set][stdin]")
{
	char buf[64];
	SECTION("LF")
	{
		stream s("abc-123\n");
		CHECK(cli_read_value_line(s.f, buf, sizeof(buf)) == CLI_VALUE_READ_OK);
		CHECK(std::string(buf) == "abc-123");
		CHECK(zero_past(buf, 7, sizeof(buf)));
	}
	SECTION("CRLF")
	{
		stream s("abc\r\n");
		CHECK(cli_read_value_line(s.f, buf, sizeof(buf)) == CLI_VALUE_READ_OK);
		CHECK(std::string(buf) == "abc");
	}
	SECTION("no newline at the end of input")
	{
		stream s("abc");
		CHECK(cli_read_value_line(s.f, buf, sizeof(buf)) == CLI_VALUE_READ_OK);
		CHECK(std::string(buf) == "abc");
	}
	SECTION("only the first line is taken; the rest is left unread")
	{
		stream s("first\nsecond\n");
		CHECK(cli_read_value_line(s.f, buf, sizeof(buf)) == CLI_VALUE_READ_OK);
		CHECK(std::string(buf) == "first");
		char rest[16] = {0};
		CHECK(std::fgets(rest, sizeof(rest), s.f) != nullptr);
		CHECK(std::string(rest) == "second\n");
	}
	SECTION("inner blanks and any byte are kept as given")
	{
		stream s("  a b=c;d  \n");
		CHECK(cli_read_value_line(s.f, buf, sizeof(buf)) == CLI_VALUE_READ_OK);
		CHECK(std::string(buf) == "  a b=c;d  ");
	}
}

TEST_CASE("set --stdin: empty, too long and failures are refused", "[cli][set][stdin]")
{
	char buf[8];
	SECTION("empty input")
	{
		stream s("");
		CHECK(cli_read_value_line(s.f, buf, sizeof(buf)) == CLI_VALUE_READ_EMPTY);
		CHECK(zero_past(buf, 0, sizeof(buf)));
	}
	SECTION("an empty first line")
	{
		stream s("\nvalue\n");
		CHECK(cli_read_value_line(s.f, buf, sizeof(buf)) == CLI_VALUE_READ_EMPTY);
	}
	SECTION("a lone CR is empty")
	{
		stream s("\r\n");
		CHECK(cli_read_value_line(s.f, buf, sizeof(buf)) == CLI_VALUE_READ_EMPTY);
	}
	SECTION("exactly cap - 1 bytes fit")
	{
		stream s("1234567\n");
		CHECK(cli_read_value_line(s.f, buf, sizeof(buf)) == CLI_VALUE_READ_OK);
		CHECK(std::string(buf) == "1234567");
	}
	SECTION("cap bytes do not fit, and the buffer is wiped")
	{
		stream s("12345678\nnext\n");
		CHECK(cli_read_value_line(s.f, buf, sizeof(buf)) == CLI_VALUE_READ_TOO_LONG);
		CHECK(zero_past(buf, 0, sizeof(buf)));
		// The over-long line was consumed whole: nothing of it is left for a later read.
		char rest[16] = {0};
		CHECK(std::fgets(rest, sizeof(rest), s.f) != nullptr);
		CHECK(std::string(rest) == "next\n");
	}
	SECTION("no stream, no buffer")
	{
		CHECK(cli_read_value_line(nullptr, buf, sizeof(buf)) == CLI_VALUE_READ_IO);
		CHECK(cli_read_value_line(stdin, nullptr, 8) == CLI_VALUE_READ_IO);
		CHECK(cli_read_value_line(stdin, buf, 0) == CLI_VALUE_READ_IO);
	}
}

TEST_CASE("set --stdin: a secret-sized value fits the secret store", "[cli][set][stdin]")
{
	// The store takes up to 2047 bytes of a secret (2048 with the NUL).
	std::string jwt(2047, 'x');
	std::vector<char> buf(2048);
	{
		stream s(jwt + "\n");
		CHECK(cli_read_value_line(s.f, buf.data(), buf.size()) == CLI_VALUE_READ_OK);
		CHECK(std::strlen(buf.data()) == 2047);
	}
	{
		stream s(jwt + "y\n");
		CHECK(cli_read_value_line(s.f, buf.data(), buf.size()) == CLI_VALUE_READ_TOO_LONG);
		CHECK(buf[0] == '\0');
	}
}

namespace {

//! The words cli_enum_values_from_help() reads from @p help (empty = not parsed).
std::vector<std::string>
words_of(const char *help)
{
	char out[CLI_ENUM_VALUES_MAX][CLI_ENUM_VALUE_LEN];
	const int n = cli_enum_values_from_help(help, out, CLI_ENUM_VALUES_MAX);
	return std::vector<std::string>(out, out + n);
}

using W = std::vector<std::string>;

} // namespace

TEST_CASE("options --json values: in-tree enum help strings that list their words first", "[cli][options][enum]")
{
	// remote_conversion FORMAT
	CHECK(words_of("sog | ply: the blob asked for when the job names none") == W{"sog", "ply"});
	// sim_input HANDS
	CHECK(words_of("which simulated hands track: both | left | right | none (a hand left out stays a device but "
	               "is never active)")
	          .empty()); // prose first: not a leading run
	CHECK(words_of("both | left | right | none (a hand left out stays a device but is never active)") ==
	      W{"both", "left", "right", "none"});
	// ultraleap MAPPING: the sentence's full stop is not part of the word
	CHECK(words_of("relative | physical. relative: hands placed in display heights (ANCHOR_*_H, GAIN_H_PER_M), "
	               "no measuring; physical: 1:1 metres from a measured mount (MOUNT_*_M)") ==
	      W{"relative", "physical"});
	// The documented compact form.
	CHECK(words_of("a|b|c -- explanation") == W{"a", "b", "c"});
	CHECK(words_of("anaglyph|sbs|squeezed|quad") == W{"anaglyph", "sbs", "squeezed", "quad"});
	CHECK(words_of("  0|1|2") == W{"0", "1", "2"});
	CHECK(words_of("on|off, the rest") == W{"on", "off"});
	CHECK(words_of("v1.2|v2.0-beta|x_y") == W{"v1.2", "v2.0-beta", "x_y"});
}

TEST_CASE("options --json values: help strings that must NOT parse", "[cli][options][enum]")
{
	// sim_display OUTPUT: prose, then a comma list.
	CHECK(
	    words_of("the 3D kind mode 1 draws: anaglyph, sbs, squeezed|squeezed_sbs, quad (4 views: an inspection "
	             "kind, stereo apps fall back to 2D), interlaced, blend; 2d|passthrough = anaglyph and start in 2D")
	        .empty());
	// sim_display WEAVE_SCOPE / WISH_QUANTIZE / STEREO_CAMERA_FORMAT / WEBCAM_ESTIMATOR
	CHECK(words_of("weave scope the DP declares: canvas, region, scanout").empty());
	CHECK(words_of("quantise the wish mask: off, band, cell").empty());
	CHECK(words_of("fake camera: gray8, nv12, bgra8").empty());
	CHECK(words_of("auto (the built-in face estimator), none, or the built-in name (yunet-onnx)").empty());
	// sim_common LICENSE_SIM: the words come after prose.
	CHECK(words_of("scripted licence state: licensed|trial|expired|unlicensed (unset = no licence to report)")
	          .empty());
	// sim_input PINCH, sim_tracking SOURCE / SCRIPT: a parenthesis between the words.
	CHECK(
	    words_of("wave (open-fist curl wave, 3 s) | hold (thumb + index tips held pinched) | open (never pinches)")
	        .empty());
	CHECK(words_of("script (a scripted viewer; see SCRIPT, CAMERA) | webcam (the webcam eye tracker)").empty());
	CHECK(
	    words_of("sweep (the head sweeps left-right, AMPLITUDE_M over PERIOD_MS) | offset (holds still)").empty());
	// sim_conversion LICENCE
	CHECK(words_of("research = RESEARCH_ONLY, commercial_ok 0 (AllowResearchOnly gate)").empty());
	// Display driver MULTI_VIEWER as a comma list.
	CHECK(words_of("multi-viewer weave: system, on, off").empty());
	// Malformed runs.
	CHECK(words_of("").empty());
	CHECK(words_of(nullptr).empty());
	CHECK(words_of("single").empty());
	CHECK(words_of("a||b").empty());
	CHECK(words_of("|a|b").empty());
	CHECK(words_of("a|b|").empty());
	CHECK(words_of("a|b!").empty());
	CHECK(words_of("a|b/c").empty());
	CHECK(words_of("a|...").empty());
	CHECK(words_of("a|0123456789012345678901234567890123").empty()); // a word too long
	CHECK(words_of("a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|p|q").empty());    // 17 words: too many
	CHECK(words_of("a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|p").size() == 16);
}
