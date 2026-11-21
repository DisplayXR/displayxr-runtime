// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The `displayxr://activate` deep link (ADR-064 §5,
 *         docs/specs/runtime/activation-deep-link.md), hardware-free:
 *
 *  - the strict link parser against a table of hostile links (malformed,
 *    oversized, wrong action, unknown / repeated keys, embedded quotes and
 *    spaces, %00, control characters, non-UTF-8, overlong UTF-8);
 *  - the purchase-URL builder (what a vendor's page receives);
 *  - the per-user pending store: single use, expiry, the user binding, a
 *    corrupt record, the per-user cap, owner-only files, the failure counter;
 *  - the handler end to end against a scripted component (the reference
 *    licence script): begin -> link -> handler -> the component reads
 *    licensed -> a selection that skipped it now picks it; and that a link
 *    can never choose the component, a nonce is spent by its first use, a
 *    flood is ignored without spending anything, and no token reaches the log;
 *  - the service's live-activation slot (chunked staging, one activation at
 *    a time, a departed client, NOT_HOSTED).
 */

#include "activation_handler.h"

#include "util/u_activation_store.h"
#include "util/u_activation_url.h"
#include "xrt/xrt_component_license.h"
#include "xrt/xrt_config_os.h"

#include "sim_common/sim_license.h"

// The service's slot is a service-target source compiled into this test.
#include "service_component_activate.h"

#include "catch_amalgamated.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef XRT_OS_WINDOWS
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

const char *const kNonce = "AbCdEfGhIjKlMnOpQrStUvWxYz0123456789-_AbCdE"; // 43 base64url characters

//! A scratch store directory that does not exist yet (the store creates it owner-only).
struct ScratchDir
{
	fs::path path;
	ScratchDir()
	{
		static std::atomic<int> n{0};
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		path = fs::temp_directory_path() /
		       ("dxr-activation-test-" + std::to_string(stamp) + "-" + std::to_string(n++));
		fs::remove_all(path);
	}
	~ScratchDir()
	{
		std::error_code ec;
		fs::remove_all(path, ec);
	}
	std::string
	str() const
	{
		return path.string();
	}
	std::vector<fs::path>
	files(const std::string &ext) const
	{
		std::vector<fs::path> out;
		std::error_code ec;
		for (const auto &e : fs::directory_iterator(path, ec)) {
			if (e.path().extension() == ext) {
				out.push_back(e.path());
			}
		}
		return out;
	}
	std::string
	read(const std::string &name) const
	{
		std::ifstream f(path / name, std::ios::binary);
		std::stringstream ss;
		ss << f.rdbuf();
		return ss.str();
	}
};

u_activation_url_error
parse(const std::string &url, u_activation_url *out = nullptr)
{
	static u_activation_url scratch;
	return u_activation_url_parse(url.c_str(), out != nullptr ? out : &scratch);
}

std::string
mk_link(const std::string &nonce, const std::string &token, const std::string &extra = "")
{
	return "displayxr://activate?nonce=" + nonce + "&token=" + token + extra;
}

} // namespace


/*
 *
 * The parser.
 *
 */

TEST_CASE("activation link: a well-formed link parses to its three fields", "[activation][url]")
{
	u_activation_url u;
	REQUIRE(parse(mk_link(kNonce, "DXR-SIM-TEST-KEY"), &u) == U_ACTIVATION_URL_OK);
	CHECK(std::string(u.nonce) == kNonce);
	CHECK(std::string(u.token) == "DXR-SIM-TEST-KEY");
	CHECK(u.token_size == 16u);
	CHECK_FALSE(u.has_item);

	// Any order, the optional echo with either separator, an optional slash, a case-insensitive scheme.
	REQUIRE(parse(std::string("DisplayXR://activate/?item=conv%2Fsim-conversion&token=a.b_c~d&nonce=") + kNonce,
	              &u) == U_ACTIVATION_URL_OK);
	CHECK(u.has_item);
	CHECK(std::string(u.item_class) == "conv");
	CHECK(std::string(u.item_id) == "sim-conversion");
	CHECK(std::string(u.token) == "a.b_c~d");
	REQUIRE(parse(mk_link(kNonce, "t", "&item=tracking/sim-tracking"), &u) == U_ACTIVATION_URL_OK);
	CHECK(std::string(u.item_class) == "tracking");

	// Percent-encoding decodes to bytes; well-formed UTF-8 is kept (U+00E9, U+20AC, U+1F600).
	REQUIRE(parse(mk_link(kNonce, "a%2Bb%2Fc%3D%3D%20%C3%A9%E2%82%AC%F0%9F%98%80"), &u) == U_ACTIVATION_URL_OK);
	CHECK(std::string(u.token) == "a+b/c== \xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");

	// The largest token: 2047 bytes, here fully percent-encoded (the longest link there is).
	std::string big;
	for (int i = 0; i < 2047; i++) {
		big += "%41";
	}
	REQUIRE(parse(mk_link(kNonce, big), &u) == U_ACTIVATION_URL_OK);
	CHECK(u.token_size == 2047u);
	CHECK(std::string(u.token) == std::string(2047, 'A'));
	CHECK(mk_link(kNonce, big).size() <= U_ACTIVATION_URL_MAX);

	u_activation_url_wipe(&u);
	CHECK(u.token[0] == '\0');
	CHECK(u.nonce[0] == '\0');
	CHECK(u.token_size == 0u);
}

TEST_CASE("activation link: every malformed or hostile link is refused and yields nothing", "[activation][url]")
{
	struct Case
	{
		const char *what;
		std::string url;
		u_activation_url_error expect;
	};
	const std::string n = kNonce;
	const std::string ok = mk_link(n, "tok");
	const std::vector<Case> cases = {
	    {"empty", "", U_ACTIVATION_URL_EMPTY},
	    {"another scheme", "https://activate?nonce=" + n + "&token=tok", U_ACTIVATION_URL_BAD_SCHEME},
	    {"scheme without //", "displayxr:activate?nonce=" + n + "&token=tok", U_ACTIVATION_URL_BAD_SCHEME},
	    {"file scheme", "file:///etc/passwd", U_ACTIVATION_URL_BAD_SCHEME},
	    {"too short", "displayxr", U_ACTIVATION_URL_BAD_SCHEME},
	    {"wrong action", "displayxr://open?nonce=" + n + "&token=tok", U_ACTIVATION_URL_BAD_ACTION},
	    {"install action", "displayxr://install?item=conv/x", U_ACTIVATION_URL_BAD_ACTION},
	    {"action is case-sensitive", "displayxr://Activate?nonce=" + n + "&token=tok", U_ACTIVATION_URL_BAD_ACTION},
	    {"action prefix", "displayxr://activate2?nonce=" + n + "&token=tok", U_ACTIVATION_URL_BAD_ACTION},
	    {"a path after the action", "displayxr://activate/x?nonce=" + n + "&token=tok",
	     U_ACTIVATION_URL_BAD_ACTION},
	    {"two slashes", "displayxr://activate//?nonce=" + n + "&token=tok", U_ACTIVATION_URL_BAD_ACTION},
	    {"a port", "displayxr://activate:80?nonce=" + n + "&token=tok", U_ACTIVATION_URL_BAD_ACTION},
	    {"user-info", "displayxr://user@activate?nonce=" + n + "&token=tok", U_ACTIVATION_URL_BAD_CHARACTER},
	    {"no query", "displayxr://activate", U_ACTIVATION_URL_BAD_QUERY},
	    {"empty query", "displayxr://activate?", U_ACTIVATION_URL_BAD_QUERY},
	    {"a fragment", ok + "#frag", U_ACTIVATION_URL_BAD_CHARACTER},
	    {"unknown key", ok + "&next=x", U_ACTIVATION_URL_BAD_QUERY},
	    {"a key that looks like a flag", ok + "&--machine=1", U_ACTIVATION_URL_BAD_QUERY},
	    {"repeated nonce", ok + "&nonce=" + n, U_ACTIVATION_URL_BAD_QUERY},
	    {"repeated token", ok + "&token=other", U_ACTIVATION_URL_BAD_QUERY},
	    {"repeated item", ok + "&item=conv/a&item=conv/b", U_ACTIVATION_URL_BAD_QUERY},
	    {"trailing ampersand", ok + "&", U_ACTIVATION_URL_BAD_QUERY},
	    {"empty parameter", "displayxr://activate?&nonce=" + n + "&token=tok", U_ACTIVATION_URL_BAD_QUERY},
	    {"key without value sign", "displayxr://activate?nonce", U_ACTIVATION_URL_BAD_QUERY},
	    {"missing nonce", "displayxr://activate?token=tok", U_ACTIVATION_URL_BAD_NONCE},
	    {"short nonce", mk_link(n.substr(1), "tok"), U_ACTIVATION_URL_BAD_NONCE},
	    {"long nonce", mk_link(n + "A", "tok"), U_ACTIVATION_URL_BAD_NONCE},
	    {"empty nonce", mk_link("", "tok"), U_ACTIVATION_URL_BAD_NONCE},
	    {"nonce with a dot", mk_link(n.substr(0, 42) + ".", "tok"), U_ACTIVATION_URL_BAD_NONCE},
	    {"percent-encoded nonce", mk_link(n.substr(0, 40) + "%41", "tok"), U_ACTIVATION_URL_BAD_NONCE},
	    {"path traversal nonce", mk_link("..%2F..%2F..%2Fetc%2Fpasswd", "tok"), U_ACTIVATION_URL_BAD_NONCE},
	    {"missing token", "displayxr://activate?nonce=" + n, U_ACTIVATION_URL_BAD_TOKEN},
	    {"empty token", mk_link(n, ""), U_ACTIVATION_URL_BAD_TOKEN},
	    {"%00 in the token", mk_link(n, "a%00b"), U_ACTIVATION_URL_BAD_TOKEN},
	    {"only %00", mk_link(n, "%00"), U_ACTIVATION_URL_BAD_TOKEN},
	    {"control character (newline)", mk_link(n, "a%0Ab"), U_ACTIVATION_URL_BAD_TOKEN},
	    {"control character (CR)", mk_link(n, "a%0Db"), U_ACTIVATION_URL_BAD_TOKEN},
	    {"control character (ESC)", mk_link(n, "%1B%5B2J"), U_ACTIVATION_URL_BAD_TOKEN},
	    {"DEL", mk_link(n, "a%7Fb"), U_ACTIVATION_URL_BAD_TOKEN},
	    {"C1 control (U+0085)", mk_link(n, "a%C2%85b"), U_ACTIVATION_URL_BAD_TOKEN},
	    {"truncated escape", mk_link(n, "abc%4"), U_ACTIVATION_URL_BAD_TOKEN},
	    {"lone percent", mk_link(n, "abc%"), U_ACTIVATION_URL_BAD_TOKEN},
	    {"non-hex escape", mk_link(n, "a%ZZb"), U_ACTIVATION_URL_BAD_TOKEN},
	    {"non-UTF-8 (lone continuation)", mk_link(n, "%80"), U_ACTIVATION_URL_BAD_TOKEN},
	    {"non-UTF-8 (Latin-1 e-acute)", mk_link(n, "caf%E9"), U_ACTIVATION_URL_BAD_TOKEN},
	    {"non-UTF-8 (truncated sequence)", mk_link(n, "%E2%82"), U_ACTIVATION_URL_BAD_TOKEN},
	    {"overlong UTF-8 (C0 AF)", mk_link(n, "%C0%AF"), U_ACTIVATION_URL_BAD_TOKEN},
	    {"overlong NUL (C0 80)", mk_link(n, "%C0%80"), U_ACTIVATION_URL_BAD_TOKEN},
	    {"overlong 3-byte", mk_link(n, "%E0%80%AF"), U_ACTIVATION_URL_BAD_TOKEN},
	    {"UTF-16 surrogate", mk_link(n, "%ED%A0%80"), U_ACTIVATION_URL_BAD_TOKEN},
	    {"above U+10FFFF", mk_link(n, "%F4%90%80%80"), U_ACTIVATION_URL_BAD_TOKEN},
	    {"0xFF byte", mk_link(n, "%FF"), U_ACTIVATION_URL_BAD_TOKEN},
	    {"token one byte too long", mk_link(n, std::string(2048, 'A')), U_ACTIVATION_URL_BAD_TOKEN},
	    {"raw double quote", mk_link(n, "a\"b"), U_ACTIVATION_URL_BAD_CHARACTER},
	    {"quote then an injected flag", mk_link(n, "a\" --machine \""), U_ACTIVATION_URL_BAD_CHARACTER},
	    {"raw single quote", mk_link(n, "a'b"), U_ACTIVATION_URL_BAD_CHARACTER},
	    {"raw space", mk_link(n, "a b"), U_ACTIVATION_URL_BAD_CHARACTER},
	    {"leading space", " " + ok, U_ACTIVATION_URL_BAD_CHARACTER},
	    {"trailing newline", ok + "\n", U_ACTIVATION_URL_BAD_CHARACTER},
	    {"tab", mk_link(n, "a\tb"), U_ACTIVATION_URL_BAD_CHARACTER},
	    {"plus is not a space", mk_link(n, "a+b"), U_ACTIVATION_URL_BAD_CHARACTER},
	    {"backslash", mk_link(n, "a\\b"), U_ACTIVATION_URL_BAD_CHARACTER},
	    {"shell metacharacters", mk_link(n, "$(calc)"), U_ACTIVATION_URL_BAD_CHARACTER},
	    {"backtick", mk_link(n, "`id`"), U_ACTIVATION_URL_BAD_CHARACTER},
	    {"pipe", mk_link(n, "a|b"), U_ACTIVATION_URL_BAD_CHARACTER},
	    {"semicolon", mk_link(n, "a;b"), U_ACTIVATION_URL_BAD_CHARACTER},
	    {"angle brackets", mk_link(n, "<script>"), U_ACTIVATION_URL_BAD_CHARACTER},
	    {"raw non-ASCII byte", mk_link(n, "caf\xC3\xA9"), U_ACTIVATION_URL_BAD_CHARACTER},
	    {"raw 0x01", mk_link(n, std::string("a\x01") + "b"), U_ACTIVATION_URL_BAD_CHARACTER},
	    {"item without id", ok + "&item=conv", U_ACTIVATION_URL_BAD_ITEM},
	    {"item with an unknown class", ok + "&item=shell/x", U_ACTIVATION_URL_BAD_ITEM},
	    {"item with an empty id", ok + "&item=conv/", U_ACTIVATION_URL_BAD_ITEM},
	    {"item id with a slash", ok + "&item=conv/a/b", U_ACTIVATION_URL_BAD_ITEM},
	    {"item id is ..", ok + "&item=conv/..", U_ACTIVATION_URL_BAD_ITEM},
	    {"item with another escape", ok + "&item=conv%5Cx", U_ACTIVATION_URL_BAD_ITEM},
	    {"item id too long", ok + "&item=conv/" + std::string(64, 'a'), U_ACTIVATION_URL_BAD_ITEM},
	    {"oversized link", mk_link(n, std::string(U_ACTIVATION_URL_MAX, 'A')), U_ACTIVATION_URL_TOO_LONG},
	    {"enormous link", mk_link(n, std::string(1024 * 1024, 'A')), U_ACTIVATION_URL_TOO_LONG},
	};
	for (const Case &c : cases) {
		INFO(c.what);
		u_activation_url u;
		std::memset(&u, 0x5A, sizeof(u));
		const u_activation_url_error e = u_activation_url_parse(c.url.c_str(), &u);
		CHECK(std::string(u_activation_url_error_str(e)) == u_activation_url_error_str(c.expect));
		// A refused link leaves NOTHING behind: no nonce, no token, no item.
		const std::vector<unsigned char> zero(sizeof(u), 0);
		CHECK(std::memcmp(&u, zero.data(), sizeof(u)) == 0);
	}
	u_activation_url u;
	CHECK(u_activation_url_parse(nullptr, &u) == U_ACTIVATION_URL_EMPTY);
	CHECK(u_activation_url_parse("displayxr://activate?nonce=x&token=y", nullptr) == U_ACTIVATION_URL_EMPTY);
}

TEST_CASE("activation link: the text rule a token must pass", "[activation][url]")
{
	CHECK(u_activation_text_valid("plain ASCII", 11));
	CHECK(u_activation_text_valid("\xC3\xA9", 2));
	CHECK(u_activation_text_valid("", 0));
	CHECK_FALSE(u_activation_text_valid("a\0b", 3));
	CHECK_FALSE(u_activation_text_valid("\x1F", 1));
	CHECK_FALSE(u_activation_text_valid("\xC3", 1));
	CHECK_FALSE(u_activation_text_valid("\xED\xBF\xBF", 3)); // U+DFFF
	CHECK_FALSE(u_activation_text_valid(nullptr, 0));

	CHECK(u_activation_class_valid("dp"));
	CHECK(u_activation_class_valid("tracking"));
	CHECK_FALSE(u_activation_class_valid("DP"));
	CHECK_FALSE(u_activation_class_valid(""));
	CHECK_FALSE(u_activation_class_valid(nullptr));
	CHECK(u_activation_id_valid("leia-sr"));
	CHECK(u_activation_id_valid("a.b_c-9"));
	CHECK_FALSE(u_activation_id_valid(""));
	CHECK_FALSE(u_activation_id_valid("."));
	CHECK_FALSE(u_activation_id_valid(".."));
	CHECK_FALSE(u_activation_id_valid("a/b"));
	CHECK_FALSE(u_activation_id_valid("a\\b"));
	CHECK_FALSE(u_activation_id_valid("a b"));
	CHECK_FALSE(u_activation_id_valid(std::string(64, 'a').c_str()));
	CHECK(u_activation_id_valid(std::string(63, 'a').c_str()));
}

TEST_CASE("activation link: the purchase URL a vendor's page receives", "[activation][url]")
{
	char out[512];
	REQUIRE(u_activation_purchase_url_build("https://vendor.example/buy", "conv", "sim-conversion", kNonce, out,
	                                        sizeof(out)) == U_ACTIVATION_PURCHASE_OK);
	CHECK(std::string(out) ==
	      std::string("https://vendor.example/buy?displayxr_nonce=") + kNonce +
	          "&displayxr_item=conv%2Fsim-conversion&displayxr_return=displayxr%3A%2F%2Factivate");
	// An existing query is extended; a fragment stays last.
	REQUIRE(u_activation_purchase_url_build("https://vendor.example/buy?sku=7#top", "dp", "x", kNonce, out,
	                                        sizeof(out)) == U_ACTIVATION_PURCHASE_OK);
	CHECK(std::string(out) == std::string("https://vendor.example/buy?sku=7&displayxr_nonce=") + kNonce +
	                              "&displayxr_item=dp%2Fx&displayxr_return=displayxr%3A%2F%2Factivate#top");
	REQUIRE(u_activation_purchase_url_build("https://vendor.example/buy?", "dp", "x", kNonce, out, sizeof(out)) ==
	        U_ACTIVATION_PURCHASE_OK);
	CHECK(std::string(out).find("buy?displayxr_nonce=") != std::string::npos);

	// What a third-party binary may NOT make the Dashboard open.
	const auto build = [&](const char *url) {
		return u_activation_purchase_url_build(url, "conv", "sim-conversion", kNonce, out, sizeof(out));
	};
	CHECK(build("") == U_ACTIVATION_PURCHASE_NOT_HTTPS);
	CHECK(build(nullptr) == U_ACTIVATION_PURCHASE_NOT_HTTPS);
	CHECK(build("http://vendor.example/buy") == U_ACTIVATION_PURCHASE_NOT_HTTPS);
	CHECK(build("file:///C:/Windows/System32/calc.exe") == U_ACTIVATION_PURCHASE_NOT_HTTPS);
	CHECK(build("javascript:alert(1)") == U_ACTIVATION_PURCHASE_NOT_HTTPS);
	CHECK(build("ms-settings:") == U_ACTIVATION_PURCHASE_NOT_HTTPS);
	CHECK(build("https://") == U_ACTIVATION_PURCHASE_NOT_HTTPS);
	CHECK(build("https:///path") == U_ACTIVATION_PURCHASE_NOT_HTTPS);
	CHECK(build(" https://vendor.example") == U_ACTIVATION_PURCHASE_NOT_HTTPS);
	CHECK(build("https://vendor.example/a b") == U_ACTIVATION_PURCHASE_BAD_CHARACTER);
	CHECK(build("https://vendor.example/\" & calc") == U_ACTIVATION_PURCHASE_BAD_CHARACTER);
	CHECK(build("https://vendor.example/'x") == U_ACTIVATION_PURCHASE_BAD_CHARACTER);
	CHECK(build("https://vendor.example/\n") == U_ACTIVATION_PURCHASE_BAD_CHARACTER);
	CHECK(build("https://vendor.example/caf\xC3\xA9") == U_ACTIVATION_PURCHASE_BAD_CHARACTER);
	CHECK(out[0] == '\0');
	CHECK(u_activation_purchase_url_build("https://v.example", "shell", "x", kNonce, out, sizeof(out)) ==
	      U_ACTIVATION_PURCHASE_BAD_ARGUMENT);
	CHECK(u_activation_purchase_url_build("https://v.example", "conv", "a/b", kNonce, out, sizeof(out)) ==
	      U_ACTIVATION_PURCHASE_BAD_ARGUMENT);
	CHECK(u_activation_purchase_url_build("https://v.example", "conv", "x", "short", out, sizeof(out)) ==
	      U_ACTIVATION_PURCHASE_BAD_ARGUMENT);
	char tiny[32];
	CHECK(u_activation_purchase_url_build("https://v.example", "conv", "x", kNonce, tiny, sizeof(tiny)) ==
	      U_ACTIVATION_PURCHASE_TOO_LONG);
	CHECK(tiny[0] == '\0');
}


/*
 *
 * The pending store.
 *
 */

TEST_CASE("activation store: a nonce is single use, bound to its component, and never on disk", "[activation][store]")
{
	ScratchDir d;
	const int64_t t0 = 1800000000;
	char nonce[U_ACTIVATION_NONCE_CHARS + 1];
	u_activation_record rec;
	REQUIRE(u_activation_store_begin(d.str().c_str(), "conv", "sim-conversion", t0, U_ACTIVATION_TTL_S, nonce,
	                                 &rec) == U_ACTIVATION_STORE_OK);
	CHECK(u_activation_nonce_valid(nonce));
	CHECK(std::string(rec.cls) == "conv");
	CHECK(std::string(rec.id) == "sim-conversion");
	CHECK(rec.expires_unix_s == t0 + U_ACTIVATION_TTL_S);
	CHECK(std::strlen(rec.nonce_id) == U_ACTIVATION_NONCE_ID_CHARS);
	char id8[U_ACTIVATION_NONCE_ID_CHARS + 1];
	u_activation_nonce_id(nonce, id8);
	CHECK(std::string(id8) == rec.nonce_id);

	// One record; neither its name nor its content holds the nonce.
	const auto pending = d.files(".pending");
	REQUIRE(pending.size() == 1u);
	CHECK(pending[0].filename().string().find(nonce) == std::string::npos);
	CHECK(d.read(pending[0].filename().string()).find(nonce) == std::string::npos);
#ifndef XRT_OS_WINDOWS
	struct stat st;
	REQUIRE(stat(d.str().c_str(), &st) == 0);
	CHECK((st.st_mode & 0777) == 0700);
	REQUIRE(stat(pending[0].string().c_str(), &st) == 0);
	CHECK((st.st_mode & 0777) == 0600);
#endif

	// Two nonces never collide, and a different nonce finds nothing (and spends nothing).
	char nonce2[U_ACTIVATION_NONCE_CHARS + 1];
	REQUIRE(u_activation_store_begin(d.str().c_str(), "dp", "leia-sr", t0, U_ACTIVATION_TTL_S, nonce2, nullptr) ==
	        U_ACTIVATION_STORE_OK);
	CHECK(std::string(nonce) != nonce2);
	u_activation_record got;
	CHECK(u_activation_store_consume(d.str().c_str(), kNonce, t0 + 1, &got) == U_ACTIVATION_STORE_NOT_FOUND);
	CHECK(d.files(".pending").size() == 2u);

	// First use: OK, and what comes back is what was begun, not anything from a link.
	REQUIRE(u_activation_store_consume(d.str().c_str(), nonce, t0 + 60, &got) == U_ACTIVATION_STORE_OK);
	CHECK(std::string(got.cls) == "conv");
	CHECK(std::string(got.id) == "sim-conversion");
	CHECK(std::string(got.nonce_id) == rec.nonce_id);
	// Replay: gone.
	CHECK(u_activation_store_consume(d.str().c_str(), nonce, t0 + 61, &got) == U_ACTIVATION_STORE_NOT_FOUND);
	CHECK(got.id[0] == '\0');
	CHECK(d.files(".pending").size() == 1u);

	CHECK(u_activation_store_consume(d.str().c_str(), "not-a-nonce", t0, &got) == U_ACTIVATION_STORE_BAD_ARGUMENT);
	CHECK(u_activation_store_consume(d.str().c_str(), "../../etc/passwd", t0, &got) ==
	      U_ACTIVATION_STORE_BAD_ARGUMENT);
	CHECK(u_activation_store_begin(d.str().c_str(), "shell", "x", t0, 900, nonce, nullptr) ==
	      U_ACTIVATION_STORE_BAD_ARGUMENT);
	CHECK(u_activation_store_begin(d.str().c_str(), "conv", "../x", t0, 900, nonce, nullptr) ==
	      U_ACTIVATION_STORE_BAD_ARGUMENT);
	CHECK(u_activation_store_begin(d.str().c_str(), "conv", "x", t0, 0, nonce, nullptr) ==
	      U_ACTIVATION_STORE_BAD_ARGUMENT);
}

TEST_CASE("activation store: expiry, a clock set back, and a store that was never created", "[activation][store]")
{
	ScratchDir d;
	const int64_t t0 = 1800000000;
	char nonce[U_ACTIVATION_NONCE_CHARS + 1];
	u_activation_record got;

	// Nothing was ever begun: not found, and the directory is not created by a lookup.
	CHECK(u_activation_store_consume(d.str().c_str(), kNonce, t0, &got) == U_ACTIVATION_STORE_NOT_FOUND);
	CHECK_FALSE(fs::exists(d.path));

	// Valid up to and including its last second.
	REQUIRE(u_activation_store_begin(d.str().c_str(), "conv", "a", t0, 900, nonce, nullptr) ==
	        U_ACTIVATION_STORE_OK);
	CHECK(u_activation_store_consume(d.str().c_str(), nonce, t0 + 900, &got) == U_ACTIVATION_STORE_OK);

	// One second late: expired, and spent all the same.
	REQUIRE(u_activation_store_begin(d.str().c_str(), "conv", "a", t0, 900, nonce, nullptr) ==
	        U_ACTIVATION_STORE_OK);
	CHECK(u_activation_store_consume(d.str().c_str(), nonce, t0 + 901, &got) == U_ACTIVATION_STORE_EXPIRED);
	CHECK(got.id[0] == '\0');
	CHECK(u_activation_store_consume(d.str().c_str(), nonce, t0 + 10, &got) == U_ACTIVATION_STORE_NOT_FOUND);

	// A record "created" well in the future (the clock was set back) is refused.
	REQUIRE(u_activation_store_begin(d.str().c_str(), "conv", "a", t0 + 100000, 900, nonce, nullptr) ==
	        U_ACTIVATION_STORE_OK);
	CHECK(u_activation_store_consume(d.str().c_str(), nonce, t0, &got) == U_ACTIVATION_STORE_EXPIRED);

	// Prune drops what expired and keeps what is live.
	char live[U_ACTIVATION_NONCE_CHARS + 1];
	REQUIRE(u_activation_store_begin(d.str().c_str(), "conv", "old", t0, 900, nonce, nullptr) ==
	        U_ACTIVATION_STORE_OK);
	REQUIRE(u_activation_store_begin(d.str().c_str(), "conv", "new", t0 + 800, 900, live, nullptr) ==
	        U_ACTIVATION_STORE_OK);
	CHECK(u_activation_store_prune(d.str().c_str(), t0 + 1000) == 1);
	CHECK(d.files(".pending").size() == 1u);
	CHECK(u_activation_store_consume(d.str().c_str(), live, t0 + 1000, &got) == U_ACTIVATION_STORE_OK);
	CHECK(std::string(got.id) == "new");
}

TEST_CASE("activation store: another user's record and a tampered record are refused and spent", "[activation][store]")
{
	ScratchDir d;
	const int64_t t0 = 1800000000;
	char nonce[U_ACTIVATION_NONCE_CHARS + 1];
	u_activation_record got;

	const auto rewrite = [&](const std::string &from, const std::string &to) {
		const auto pending = d.files(".pending");
		REQUIRE(pending.size() == 1u);
		std::string txt = d.read(pending[0].filename().string());
		const size_t at = txt.find(from);
		REQUIRE(at != std::string::npos);
		const size_t eol = txt.find('\n', at);
		txt.replace(at, eol - at, to);
		std::ofstream f(pending[0], std::ios::binary | std::ios::trunc);
		f << txt;
	};

	// A record begun by another user (same directory, another identity).
	REQUIRE(u_activation_store_begin(d.str().c_str(), "conv", "a", t0, 900, nonce, nullptr) ==
	        U_ACTIVATION_STORE_OK);
	rewrite("user=", "user=uid:4294967294");
	CHECK(u_activation_store_consume(d.str().c_str(), nonce, t0 + 1, &got) == U_ACTIVATION_STORE_WRONG_USER);
	CHECK(got.id[0] == '\0');
	CHECK(d.files(".pending").empty());
	CHECK(u_activation_store_consume(d.str().c_str(), nonce, t0 + 1, &got) == U_ACTIVATION_STORE_NOT_FOUND);

	// A record whose class was edited to something this module never writes.
	REQUIRE(u_activation_store_begin(d.str().c_str(), "conv", "a", t0, 900, nonce, nullptr) ==
	        U_ACTIVATION_STORE_OK);
	rewrite("class=", "class=shell");
	CHECK(u_activation_store_consume(d.str().c_str(), nonce, t0 + 1, &got) == U_ACTIVATION_STORE_CORRUPT);
	CHECK(d.files(".pending").empty());

	// An id that would escape a directory.
	REQUIRE(u_activation_store_begin(d.str().c_str(), "conv", "a", t0, 900, nonce, nullptr) ==
	        U_ACTIVATION_STORE_OK);
	rewrite("id=", "id=../../x");
	CHECK(u_activation_store_consume(d.str().c_str(), nonce, t0 + 1, &got) == U_ACTIVATION_STORE_CORRUPT);

	// An emptied record.
	REQUIRE(u_activation_store_begin(d.str().c_str(), "conv", "a", t0, 900, nonce, nullptr) ==
	        U_ACTIVATION_STORE_OK);
	rewrite("v=", "garbage");
	CHECK(u_activation_store_consume(d.str().c_str(), nonce, t0 + 1, &got) == U_ACTIVATION_STORE_CORRUPT);

#ifndef XRT_OS_WINDOWS
	// A record readable by group / others is not one this user wrote privately: refused.
	REQUIRE(u_activation_store_begin(d.str().c_str(), "conv", "a", t0, 900, nonce, nullptr) ==
	        U_ACTIVATION_STORE_OK);
	REQUIRE(chmod(d.files(".pending")[0].string().c_str(), 0644) == 0);
	CHECK(u_activation_store_consume(d.str().c_str(), nonce, t0 + 1, &got) == U_ACTIVATION_STORE_CORRUPT);
	CHECK(d.files(".pending").empty());

	// A directory open to others is tightened before it is used.
	REQUIRE(chmod(d.str().c_str(), 0755) == 0);
	REQUIRE(u_activation_store_begin(d.str().c_str(), "conv", "a", t0, 900, nonce, nullptr) ==
	        U_ACTIVATION_STORE_OK);
	struct stat st;
	REQUIRE(stat(d.str().c_str(), &st) == 0);
	CHECK((st.st_mode & 0777) == 0700);
#endif
}

TEST_CASE("activation store: bounded pending records, and the failure counter", "[activation][store]")
{
	ScratchDir d;
	const int64_t t0 = 1800000000;
	char nonce[U_ACTIVATION_NONCE_CHARS + 1];
	for (int i = 0; i < U_ACTIVATION_MAX_PENDING; i++) {
		REQUIRE(u_activation_store_begin(d.str().c_str(), "conv", "a", t0 + i, 900, nonce, nullptr) ==
		        U_ACTIVATION_STORE_OK);
	}
	CHECK(u_activation_store_begin(d.str().c_str(), "conv", "a", t0 + 20, 900, nonce, nullptr) ==
	      U_ACTIVATION_STORE_TOO_MANY);
	CHECK(nonce[0] == '\0');
	// Once they expired there is room again.
	CHECK(u_activation_store_begin(d.str().c_str(), "conv", "a", t0 + 2000, 900, nonce, nullptr) ==
	      U_ACTIVATION_STORE_OK);
	CHECK(d.files(".pending").size() == 1u);

	// The failure counter: LIMIT failures inside the window stop the handler; they age out.
	CHECK_FALSE(u_activation_store_rate_limited(d.str().c_str(), t0));
	for (int i = 0; i < U_ACTIVATION_FAIL_LIMIT - 1; i++) {
		u_activation_store_note_failure(d.str().c_str(), t0 + i);
	}
	CHECK_FALSE(u_activation_store_rate_limited(d.str().c_str(), t0 + 10));
	u_activation_store_note_failure(d.str().c_str(), t0 + 10);
	CHECK(u_activation_store_rate_limited(d.str().c_str(), t0 + 11));
	CHECK(u_activation_store_rate_limited(d.str().c_str(), t0 + U_ACTIVATION_FAIL_WINDOW_S - 1));
	CHECK_FALSE(u_activation_store_rate_limited(d.str().c_str(), t0 + U_ACTIVATION_FAIL_WINDOW_S + 11));
	// A flood does not grow the file without bound.
	for (int i = 0; i < 200; i++) {
		u_activation_store_note_failure(d.str().c_str(), t0 + 5000 + i);
	}
	CHECK(fs::file_size(d.path / "failures") < 512u);
	CHECK(u_activation_store_rate_limited(d.str().c_str(), t0 + 5200));

	// The log keeps to its fields' character set, whatever it is handed.
	u_activation_store_log(d.str().c_str(), t0, "abcd1234", "conv", "sim-conversion", "activated");
	u_activation_store_log(d.str().c_str(), t0, "x\ny", "co nv", "a\"b", "evil\r\noutcome=activated");
	const std::string log = d.read("activation.log");
	CHECK(log.find("nonce=abcd1234 class=conv id=sim-conversion outcome=activated\n") != std::string::npos);
	CHECK(log.find("nonce=x?y class=co?nv id=a?b outcome=evil??outcome?activated\n") != std::string::npos);
	CHECK(std::count(log.begin(), log.end(), '\n') == 2);
}


/*
 *
 * The handler, end to end, against a scripted component.
 *
 */

namespace {

//! Two components, as a selection sees them: a paid one ranked first, a free one behind it.
struct World
{
	sim_license paid{};     //!< "paid-conv": the reference licence script, unlicensed until activated
	int deliveries = 0;     //!< how many times ANY component's activate was reached
	std::string last_class; //!< where the last delivery went
	std::string last_id;
	std::string last_token;

	World()
	{
		sim_license_load(&paid, "unlicensed", nullptr);
	}

	//! The fall-through rule every class shares: the first candidate whose licence does not block.
	std::string
	selected() const
	{
		xrt_component_license_state st;
		xrt_component_license_state_init(&st);
		const bool has = sim_license_get(&paid, &st);
		if (!has || !xrt_component_license_blocks(st.state)) {
			return "paid-conv";
		}
		return "free-conv";
	}
};

int
world_deliver(void *ud,
              const char *cls,
              const char *id,
              const char *token,
              uint32_t token_size,
              struct xrt_component_license_state *out_state,
              char *err,
              size_t err_cap,
              char *via,
              size_t via_cap)
{
	World *w = static_cast<World *>(ud);
	w->deliveries++;
	w->last_class = cls;
	w->last_id = id;
	w->last_token.assign(token, token_size);
	(void)snprintf(via, via_cap, "process");
	if (std::string(cls) != "conv" || std::string(id) != "paid-conv") {
		return ACTIVATION_DELIVER_NOT_LOADED;
	}
	return sim_license_activate(&w->paid, token, token_size, out_state, err, (uint32_t)err_cap)
	           ? ACTIVATION_DELIVER_OK
	           : ACTIVATION_DELIVER_REFUSED;
}

struct Handler
{
	ScratchDir dir;
	World world;
	int64_t now = 1800000000;

	activation_outcome
	handle(const std::string &url)
	{
		activation_ops ops{};
		ops.deliver = world_deliver;
		ops.userdata = &world;
		const std::string d = dir.str();
		ops.store_dir = d.c_str();
		ops.now_unix_s = now;
		activation_outcome out;
		activation_handle_url(url.c_str(), &ops, &out);
		return out;
	}

	//! What `activate-begin` does: a nonce bound to (class, id), as the URL a vendor's page receives.
	std::string
	begin(const char *cls, const char *id)
	{
		char nonce[U_ACTIVATION_NONCE_CHARS + 1];
		REQUIRE(u_activation_store_begin(dir.str().c_str(), cls, id, now, U_ACTIVATION_TTL_S, nonce, nullptr) ==
		        U_ACTIVATION_STORE_OK);
		char url[512];
		REQUIRE(u_activation_purchase_url_build("https://vendor.example/buy", cls, id, nonce, url,
		                                        sizeof(url)) == U_ACTIVATION_PURCHASE_OK);
		// The vendor's page reads displayxr_nonce back out of its own URL.
		const std::string u = url;
		const size_t at = u.find("displayxr_nonce=") + 16;
		return u.substr(at, U_ACTIVATION_NONCE_CHARS);
	}
};

} // namespace

TEST_CASE("activation handler: begin -> link -> handler -> licensed -> selection changes", "[activation][handler]")
{
	Handler h;
	CHECK(h.world.selected() == "free-conv"); // the unlicensed provider is skipped

	const std::string nonce = h.begin("conv", "paid-conv");
	const activation_outcome out = h.handle(mk_link(nonce, SIM_LICENSE_TEST_KEY, "&item=conv%2Fpaid-conv"));
	CHECK(std::string(activation_outcome_str(out.kind)) == "activated");
	CHECK(activation_outcome_exit_code(out.kind) == 0);
	CHECK(std::string(out.cls) == "conv");
	CHECK(std::string(out.id) == "paid-conv");
	CHECK(std::string(out.via) == "process");
	CHECK(out.licence.state == XRT_COMPONENT_LICENSE_LICENSED);
	CHECK(out.licence.source == XRT_COMPONENT_LICENSE_SOURCE_KEY);
	CHECK_FALSE(out.quiet);
	CHECK(h.world.deliveries == 1);
	CHECK(h.world.last_token == SIM_LICENSE_TEST_KEY); // handed over untouched

	CHECK(h.world.selected() == "paid-conv"); // selection follows the new state

	// The user is told which component, and that it worked.
	char title[96];
	char body[1024];
	activation_outcome_message(&out, title, sizeof(title), body, sizeof(body));
	CHECK(std::string(title).find("complete") != std::string::npos);
	CHECK(std::string(body).find("paid-conv") != std::string::npos);
	CHECK(std::string(body).find("licensed") != std::string::npos);
	CHECK(std::string(body).find(SIM_LICENSE_TEST_KEY) == std::string::npos);

	// The link is spent: replaying it reaches no component.
	const activation_outcome again = h.handle(mk_link(nonce, SIM_LICENSE_TEST_KEY));
	CHECK(std::string(activation_outcome_str(again.kind)) == "not-pending");
	CHECK(std::string(again.detail) == "not-found");
	CHECK(h.world.deliveries == 1);

	// The log: nonce id + component + outcome, and no token anywhere.
	const std::string log = h.dir.read("activation.log");
	CHECK(log.find(std::string("nonce=") + out.nonce_id + " class=conv id=paid-conv outcome=activated") !=
	      std::string::npos);
	CHECK(log.find(SIM_LICENSE_TEST_KEY) == std::string::npos);
	CHECK(log.find(nonce) == std::string::npos);
}

TEST_CASE("activation handler: a link never chooses the component", "[activation][handler]")
{
	Handler h;
	// The activation was begun for another component; the link names the paid one.
	const std::string nonce = h.begin("conv", "free-conv");
	const activation_outcome out = h.handle(mk_link(nonce, SIM_LICENSE_TEST_KEY, "&item=conv/paid-conv"));
	CHECK(std::string(activation_outcome_str(out.kind)) == "item-mismatch");
	CHECK(h.world.deliveries == 0);
	CHECK(h.world.selected() == "free-conv");
	// ...and that nonce is spent by the attempt.
	CHECK(std::string(activation_outcome_str(h.handle(mk_link(nonce, SIM_LICENSE_TEST_KEY)).kind)) ==
	      "not-pending");

	// Without the echo, the token goes to the component of the RECORD, never elsewhere.
	const std::string n2 = h.begin("dp", "some-display");
	const activation_outcome o2 = h.handle(mk_link(n2, SIM_LICENSE_TEST_KEY));
	CHECK(h.world.deliveries == 1);
	CHECK(h.world.last_class == "dp");
	CHECK(h.world.last_id == "some-display");
	CHECK(std::string(activation_outcome_str(o2.kind)) == "not-loaded");
	CHECK(h.world.selected() == "free-conv"); // the paid provider was never reached

	// A same-id component of another class is another component.
	const std::string n3 = h.begin("conv", "paid-conv");
	CHECK(std::string(activation_outcome_str(
	          h.handle(mk_link(n3, SIM_LICENSE_TEST_KEY, "&item=dp/paid-conv")).kind)) == "item-mismatch");
	CHECK(h.world.deliveries == 1);
}

TEST_CASE("activation handler: a nonce is spent by its first use, whatever the outcome", "[activation][handler]")
{
	Handler h;
	const std::string nonce = h.begin("conv", "paid-conv");
	const activation_outcome bad = h.handle(mk_link(nonce, "not-the-key"));
	CHECK(std::string(activation_outcome_str(bad.kind)) == "refused");
	CHECK(activation_outcome_exit_code(bad.kind) == 1);
	CHECK(std::string(bad.error) != "");
	CHECK(bad.licence.state == XRT_COMPONENT_LICENSE_UNLICENSED);
	CHECK(h.world.deliveries == 1);
	// The right key on the same link no longer works: the nonce was single use.
	const activation_outcome retry = h.handle(mk_link(nonce, SIM_LICENSE_TEST_KEY));
	CHECK(std::string(activation_outcome_str(retry.kind)) == "not-pending");
	CHECK(h.world.deliveries == 1);
	CHECK(h.world.selected() == "free-conv");

	// An expired activation: refused, nothing delivered.
	const std::string old = h.begin("conv", "paid-conv");
	h.now += U_ACTIVATION_TTL_S + 1;
	const activation_outcome late = h.handle(mk_link(old, SIM_LICENSE_TEST_KEY));
	CHECK(std::string(activation_outcome_str(late.kind)) == "not-pending");
	CHECK(std::string(late.detail) == "expired");
	CHECK(h.world.deliveries == 1);

	// A component's refusal is not a "failed link": it does not count toward the flood limit.
	h.now += U_ACTIVATION_FAIL_WINDOW_S + 1;
	for (int i = 0; i < U_ACTIVATION_FAIL_LIMIT + 2; i++) {
		const std::string n = h.begin("conv", "paid-conv");
		CHECK(std::string(activation_outcome_str(h.handle(mk_link(n, "wrong")).kind)) == "refused");
	}
	const std::string good = h.begin("conv", "paid-conv");
	CHECK(std::string(activation_outcome_str(h.handle(mk_link(good, SIM_LICENSE_TEST_KEY)).kind)) == "activated");
}

TEST_CASE("activation handler: hostile links reach nothing, and a flood is ignored quietly", "[activation][handler]")
{
	Handler h;
	const std::string pending = h.begin("conv", "paid-conv");

	const std::vector<std::string> hostile = {
	    "",
	    "displayxr://open?nonce=" + pending + "&token=" SIM_LICENSE_TEST_KEY,
	    mk_link(pending, "a\" --json \""),
	    mk_link(pending, "%00"),
	};
	for (const std::string &u : hostile) {
		const activation_outcome out = h.handle(u);
		CHECK(std::string(activation_outcome_str(out.kind)) == "invalid-link");
		CHECK_FALSE(out.quiet);
		CHECK(out.id[0] == '\0');
		CHECK(out.nonce_id[0] == '\0');
	}
	// A guessed nonce: not pending. That is the fifth failure.
	CHECK(std::string(activation_outcome_str(h.handle(mk_link(kNonce, "x")).kind)) == "not-pending");
	CHECK(h.world.deliveries == 0);

	// From here the handler does not look at links at all, and asks for no dialog.
	const activation_outcome flood = h.handle(mk_link(kNonce, "x"));
	CHECK(std::string(activation_outcome_str(flood.kind)) == "rate-limited");
	CHECK(flood.quiet);
	// Even the valid pending link is ignored while limited, and it is NOT spent by that...
	const activation_outcome held = h.handle(mk_link(pending, SIM_LICENSE_TEST_KEY));
	CHECK(std::string(activation_outcome_str(held.kind)) == "rate-limited");
	CHECK(h.world.deliveries == 0);
	// ...so once the window has passed (and it is still within its 15 minutes) it works.
	h.now += U_ACTIVATION_FAIL_WINDOW_S + 1;
	const activation_outcome ok = h.handle(mk_link(pending, SIM_LICENSE_TEST_KEY));
	CHECK(std::string(activation_outcome_str(ok.kind)) == "activated");
	CHECK(h.world.deliveries == 1);

	// No message ever carries a byte of a link.
	char title[96];
	char body[1024];
	activation_outcome bad = h.handle("displayxr://activate?nonce=EVILNONCE&token=EVILTOKEN");
	activation_outcome_message(&bad, title, sizeof(title), body, sizeof(body));
	CHECK(std::string(body).find("EVIL") == std::string::npos);
	const std::string log = h.dir.read("activation.log");
	CHECK(log.find("EVIL") == std::string::npos);
	CHECK(log.find(SIM_LICENSE_TEST_KEY) == std::string::npos);
}

TEST_CASE("activation handler: a component's own text is shown only when it is text", "[activation][handler]")
{
	activation_outcome o{};
	xrt_component_license_state_init(&o.licence);
	o.kind = ACTIVATION_OUTCOME_REFUSED;
	(void)snprintf(o.cls, sizeof(o.cls), "tracking");
	(void)snprintf(o.id, sizeof(o.id), "vendor-tracker");
	(void)snprintf(o.error, sizeof(o.error), "key revoked");
	char title[96];
	char body[1024];
	activation_outcome_message(&o, title, sizeof(title), body, sizeof(body));
	CHECK(std::string(body).find("tracking provider \"vendor-tracker\"") != std::string::npos);
	CHECK(std::string(body).find("key revoked") != std::string::npos);
	// Control characters or invalid UTF-8 from a component are dropped, not shown.
	(void)snprintf(o.error, sizeof(o.error), "bad\x1B[2Jtext");
	activation_outcome_message(&o, title, sizeof(title), body, sizeof(body));
	CHECK(std::string(body).find("bad") == std::string::npos);
	(void)snprintf(o.error, sizeof(o.error), "caf\xE9");
	activation_outcome_message(&o, title, sizeof(title), body, sizeof(body));
	CHECK(std::string(body).find("caf") == std::string::npos);
	CHECK(std::string(activation_class_noun("dp")) == "display processor");
	CHECK(std::string(activation_class_noun("nope")) == "component");
}


/*
 *
 * The service's live-activation slot.
 *
 */

namespace {

struct SlotScript
{
	std::atomic<int> calls{0};
	std::atomic<bool> hold{false}; //!< keep the delivery inside "the component" until released
	std::atomic<bool> inside{false};
	std::string token;
	std::string id;
	uint32_t class_id = 0;
	bool hosted = true;
	sim_license lic{};
};

enum service_component_activate_delivery
slot_deliver(void *ud,
             uint32_t class_id,
             const char *id,
             const char *token,
             uint32_t token_size,
             uint32_t *out_result,
             struct xrt_component_license_state *out_state,
             char *err,
             size_t err_cap)
{
	SlotScript *s = static_cast<SlotScript *>(ud);
	s->calls++;
	s->class_id = class_id;
	s->id = id;
	s->token.assign(token, token_size);
	s->inside = true;
	while (s->hold) {
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	s->inside = false;
	if (!s->hosted) {
		return SERVICE_COMPONENT_ACTIVATE_DELIVERY_NOT_HOSTED;
	}
	// 0 = accepted, 1 = refused (enum target_component_activate_result).
	*out_result = sim_license_activate(&s->lic, token, token_size, out_state, err, (uint32_t)err_cap) ? 0u : 1u;
	return SERVICE_COMPONENT_ACTIVATE_DELIVERY_CALLED;
}

uint32_t
put(uint64_t owner, uint32_t offset, const std::string &bytes)
{
	return service_component_activate_put(owner, offset, reinterpret_cast<const uint8_t *>(bytes.data()),
	                                      (uint32_t)bytes.size());
}

uint32_t
poll_until_done(uint64_t owner, uint32_t *result, xrt_component_license_state *st, char *err, size_t cap)
{
	for (int i = 0; i < 2000; i++) {
		const uint32_t s = service_component_activate_poll(owner, result, st, err, cap);
		if (s != SERVICE_COMPONENT_ACTIVATE_PENDING) {
			return s;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return SERVICE_COMPONENT_ACTIVATE_PENDING;
}

} // namespace

TEST_CASE("service activation slot: a chunked token reaches the live component once", "[activation][service]")
{
	SlotScript script;
	sim_license_load(&script.lic, "unlicensed", nullptr);
	REQUIRE(service_component_activate_start(slot_deliver, &script));

	uint32_t result = 99;
	xrt_component_license_state st;
	char err[XRT_COMPONENT_ACTIVATION_ERROR_MAX];

	// Nothing staged: nothing to commit or collect.
	CHECK(service_component_activate_commit(7, 3, "sim-conversion", 4) == SERVICE_COMPONENT_ACTIVATE_NONE);
	CHECK(service_component_activate_poll(7, &result, &st, err, sizeof(err)) == SERVICE_COMPONENT_ACTIVATE_NONE);

	// The test key in three chunks.
	const std::string key = SIM_LICENSE_TEST_KEY;
	REQUIRE(put(7, 0, key.substr(0, 5)) == SERVICE_COMPONENT_ACTIVATE_PENDING);
	REQUIRE(put(7, 5, key.substr(5, 5)) == SERVICE_COMPONENT_ACTIVATE_PENDING);
	REQUIRE(put(7, 10, key.substr(10)) == SERVICE_COMPONENT_ACTIVATE_PENDING);
	REQUIRE(service_component_activate_commit(7, 3, "sim-conversion", (uint32_t)key.size()) ==
	        SERVICE_COMPONENT_ACTIVATE_PENDING);
	REQUIRE(poll_until_done(7, &result, &st, err, sizeof(err)) == SERVICE_COMPONENT_ACTIVATE_DONE);
	CHECK(result == 0u);
	CHECK(st.state == XRT_COMPONENT_LICENSE_LICENSED);
	CHECK(script.calls == 1);
	CHECK(script.token == key);
	CHECK(script.id == "sim-conversion");
	CHECK(script.class_id == 3u);
	// The answer is handed out once.
	CHECK(service_component_activate_poll(7, &result, &st, err, sizeof(err)) == SERVICE_COMPONENT_ACTIVATE_NONE);

	// A refused token comes back with the component's line.
	REQUIRE(put(7, 0, "wrong") == SERVICE_COMPONENT_ACTIVATE_PENDING);
	REQUIRE(service_component_activate_commit(7, 3, "sim-conversion", 5) == SERVICE_COMPONENT_ACTIVATE_PENDING);
	REQUIRE(poll_until_done(7, &result, &st, err, sizeof(err)) == SERVICE_COMPONENT_ACTIVATE_DONE);
	CHECK(result == 1u);
	CHECK(std::string(err) != "");

	// No live instance: NOT_HOSTED, which tells the handler nothing was called.
	script.hosted = false;
	REQUIRE(put(7, 0, key) == SERVICE_COMPONENT_ACTIVATE_PENDING);
	REQUIRE(service_component_activate_commit(7, 4, "sim-tracking", (uint32_t)key.size()) ==
	        SERVICE_COMPONENT_ACTIVATE_PENDING);
	CHECK(poll_until_done(7, &result, &st, err, sizeof(err)) == SERVICE_COMPONENT_ACTIVATE_NOT_HOSTED);
	script.hosted = true;
	const int calls = script.calls;

	// Malformed requests never reach a component, and discard the staging.
	REQUIRE(put(7, 0, "abc") == SERVICE_COMPONENT_ACTIVATE_PENDING);
	CHECK(put(7, 5, "gap") == SERVICE_COMPONENT_ACTIVATE_BAD_REQUEST);
	CHECK(service_component_activate_commit(7, 3, "x", 3) == SERVICE_COMPONENT_ACTIVATE_NONE);
	REQUIRE(put(7, 0, "abc") == SERVICE_COMPONENT_ACTIVATE_PENDING);
	CHECK(service_component_activate_commit(7, 3, "x", 4) == SERVICE_COMPONENT_ACTIVATE_BAD_REQUEST); // size lie
	REQUIRE(put(7, 0, std::string("a\0b", 3)) == SERVICE_COMPONENT_ACTIVATE_PENDING);
	CHECK(service_component_activate_commit(7, 3, "x", 3) == SERVICE_COMPONENT_ACTIVATE_BAD_REQUEST); // NUL inside
	REQUIRE(put(7, 0, "abc") == SERVICE_COMPONENT_ACTIVATE_PENDING);
	CHECK(service_component_activate_commit(7, 3, "../x", 3) == SERVICE_COMPONENT_ACTIVATE_BAD_REQUEST);
	REQUIRE(put(7, 0, "abc") == SERVICE_COMPONENT_ACTIVATE_PENDING);
	CHECK(service_component_activate_commit(7, 0, "x", 3) == SERVICE_COMPONENT_ACTIVATE_BAD_REQUEST); // no class
	// More than a token's worth.
	REQUIRE(put(7, 0, std::string(700, 'A')) == SERVICE_COMPONENT_ACTIVATE_PENDING);
	REQUIRE(put(7, 700, std::string(700, 'A')) == SERVICE_COMPONENT_ACTIVATE_PENDING);
	CHECK(put(7, 1400, std::string(700, 'A')) == SERVICE_COMPONENT_ACTIVATE_BAD_REQUEST);
	CHECK(put(7, 0, "") == SERVICE_COMPONENT_ACTIVATE_BAD_REQUEST);
	CHECK(put(0, 0, "abc") == SERVICE_COMPONENT_ACTIVATE_BAD_REQUEST);
	CHECK(script.calls == calls);

	service_component_activate_stop(1000);
	CHECK(put(7, 0, "abc") == SERVICE_COMPONENT_ACTIVATE_UNSUPPORTED);
}

TEST_CASE("service activation slot: one activation at a time, and a client that leaves", "[activation][service]")
{
	SlotScript script;
	sim_license_load(&script.lic, "unlicensed", nullptr);
	REQUIRE(service_component_activate_start(slot_deliver, &script));
	const std::string key = SIM_LICENSE_TEST_KEY;
	uint32_t result = 99;
	xrt_component_license_state st;
	char err[XRT_COMPONENT_ACTIVATION_ERROR_MAX];

	// While one connection is staging, another is told BUSY and takes nothing over.
	REQUIRE(put(1, 0, "abc") == SERVICE_COMPONENT_ACTIVATE_PENDING);
	CHECK(put(2, 0, "xyz") == SERVICE_COMPONENT_ACTIVATE_BUSY);
	CHECK(put(2, 3, "xyz") == SERVICE_COMPONENT_ACTIVATE_NONE); // cannot append to another's staging
	CHECK(service_component_activate_commit(2, 3, "x", 3) == SERVICE_COMPONENT_ACTIVATE_NONE);
	// The first connection goes away: its staging is dropped and the slot is free.
	service_component_activate_client_gone(1);
	CHECK(service_component_activate_commit(1, 3, "x", 3) == SERVICE_COMPONENT_ACTIVATE_NONE);
	REQUIRE(put(2, 0, key) == SERVICE_COMPONENT_ACTIVATE_PENDING);

	// While the component is being asked (it may block), everything else is BUSY / PENDING.
	script.hold = true;
	REQUIRE(service_component_activate_commit(2, 3, "sim-conversion", (uint32_t)key.size()) ==
	        SERVICE_COMPONENT_ACTIVATE_PENDING);
	for (int i = 0; i < 2000 && !script.inside; i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	REQUIRE(script.inside);
	CHECK(service_component_activate_poll(2, &result, &st, err, sizeof(err)) == SERVICE_COMPONENT_ACTIVATE_PENDING);
	CHECK(put(3, 0, "abc") == SERVICE_COMPONENT_ACTIVATE_BUSY);
	CHECK(put(2, 0, "abc") == SERVICE_COMPONENT_ACTIVATE_BUSY); // not even its owner restarts it
	CHECK(service_component_activate_poll(3, &result, &st, err, sizeof(err)) == SERVICE_COMPONENT_ACTIVATE_NONE);
	script.hold = false;
	REQUIRE(poll_until_done(2, &result, &st, err, sizeof(err)) == SERVICE_COMPONENT_ACTIVATE_DONE);
	CHECK(result == 0u);
	CHECK(script.calls == 1);

	// A client that leaves mid-delivery: the delivery finishes, its answer is dropped, the slot frees.
	sim_license_load(&script.lic, "unlicensed", nullptr);
	REQUIRE(put(4, 0, key) == SERVICE_COMPONENT_ACTIVATE_PENDING);
	script.hold = true;
	REQUIRE(service_component_activate_commit(4, 3, "sim-conversion", (uint32_t)key.size()) ==
	        SERVICE_COMPONENT_ACTIVATE_PENDING);
	for (int i = 0; i < 2000 && !script.inside; i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	service_component_activate_client_gone(4);
	script.hold = false;
	bool freed = false;
	for (int i = 0; i < 2000 && !freed; i++) {
		freed = put(5, 0, "abc") == SERVICE_COMPONENT_ACTIVATE_PENDING;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	CHECK(freed);
	CHECK(service_component_activate_poll(4, &result, &st, err, sizeof(err)) == SERVICE_COMPONENT_ACTIVATE_NONE);
	CHECK(script.calls == 2);

	service_component_activate_stop(1000);
}
