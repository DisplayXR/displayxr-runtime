// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `displayxr-cli catalog` (ADR-064 step 4): the front end over dxr-get.
 *
 * Driven end to end against a fake engine (tests_cli_catalog_fake_get), which
 * this test names in the ops table. The CLI itself has no way to be pointed
 * at another engine, so that override exists only here.
 *
 * What must hold (docs/specs/runtime/dxr-get.md § displayxr-cli catalog):
 *  - every verb maps to the documented engine command line, `--catalog`
 *    passed through, `--no-runtime` unless the runtime is named or allowed;
 *  - the engine's exit code is the CLI's, each with its own one-line message;
 *  - `--json` is one envelope whose `calls[].output` is the engine's document
 *    unchanged, with the licence of installed plug-ins joined beside it;
 *  - the engine is started elevated only when it answered 50, only from a
 *    terminal or with `--elevate`, never when elevation could not help;
 *  - no engine means exit 60 and no listing, and a slow engine is bounded.
 */

#include "catch_amalgamated.hpp"

#include "cli_catalog.h"

#include <cjson/cJSON.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// The writer side of the engine's log channel (pulls in <windows.h> on Windows).
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include "get_log_channel.h"

#ifdef _WIN32
#include <process.h>
#define test_pid() _getpid()
#else
#include <unistd.h>
#define test_pid() getpid()
#endif

namespace {

void
set_env(const char *name, const char *value)
{
#ifdef _WIN32
	(void)_putenv_s(name, value != nullptr ? value : "");
#else
	if (value != nullptr) {
		(void)setenv(name, value, 1);
	} else {
		(void)unsetenv(name);
	}
#endif
}

std::string
slurp(const std::filesystem::path &p)
{
	std::ifstream f(p, std::ios::binary);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

struct Result
{
	int exit = -1;
	std::string out;
	std::string err;
	std::vector<std::string> engine; //!< the fake engine's command lines, arguments joined by '|'
	cJSON *json = nullptr;

	~Result()
	{
		cJSON_Delete(json);
	}
	Result() = default;
	Result(Result &&o) noexcept
	    : exit(o.exit), out(std::move(o.out)), err(std::move(o.err)), engine(std::move(o.engine)), json(o.json)
	{
		o.json = nullptr;
	}

	const cJSON *
	at(std::initializer_list<const char *> path) const
	{
		const cJSON *n = json;
		for (const char *k : path) {
			if (k[0] >= '0' && k[0] <= '9') {
				n = cJSON_GetArrayItem(n, atoi(k));
			} else {
				n = cJSON_GetObjectItemCaseSensitive(n, k);
			}
		}
		return n;
	}
	std::string
	str(std::initializer_list<const char *> path) const
	{
		const cJSON *n = at(path);
		return cJSON_IsString(n) ? n->valuestring : "<none>";
	}
	int
	num(std::initializer_list<const char *> path) const
	{
		const cJSON *n = at(path);
		return cJSON_IsNumber(n) ? n->valueint : -999;
	}
};

struct Host
{
	bool interactive = false;
	bool admin = false; //!< the CLI itself runs elevated
	bool have_engine = true;
	enum cli_catalog_run_status elevated_status = CLI_CATALOG_RUN_OK;
	int elevated_runs = 0;
	bool elevated_admin = true; //!< the elevated engine answers as an elevated process
	bool engine_signed = true;
	bool self_signed = false; //!< a development CLI
	std::string engine_cn = "Leia, Inc."; //!< a first-party signer (get_signers.h)
	int signer_asks = 0;
	std::vector<std::string> log_tokens;
	bool log_channel = true;     //!< false: the channel cannot be opened
	bool absolute_paths = false; //!< true: the host resolves `--catalog <file>`, as the CLI does
	std::vector<std::string> licence_asks;

	static enum cli_catalog_run_status
	run_elevated(void *ud, struct cli_catalog_run *r)
	{
		Host *h = static_cast<Host *>(ud);
		h->elevated_runs++;
		if (h->elevated_status != CLI_CATALOG_RUN_OK) {
			return h->elevated_status;
		}
		// The consent prompt, granted: the same engine, now answering as an
		// elevated process, and with nothing to read back but its exit code.
		REQUIRE_FALSE(r->capture_stdout);
		set_env("FAKE_DXR_GET_ADMIN", h->elevated_admin ? "1" : nullptr);
		const enum cli_catalog_run_status st = cli_catalog_os_run(nullptr, r);
		set_env("FAKE_DXR_GET_ADMIN", h->admin ? "1" : nullptr);
		return st;
	}

	static bool
	is_interactive(void *ud)
	{
		return static_cast<Host *>(ud)->interactive;
	}

	static bool
	signer(void *ud, const char *path, char *out, size_t cap)
	{
		Host *h = static_cast<Host *>(ud);
		h->signer_asks++;
		const bool is_signed = path != nullptr ? h->engine_signed : h->self_signed;
		(void)snprintf(out, cap, "%s",
		               !is_signed ? "" : (path != nullptr ? h->engine_cn.c_str() : "Leia, Inc."));
		return is_signed;
	}

	static bool
	log_open(void *ud, char *token, size_t cap)
	{
		Host *h = static_cast<Host *>(ud);
		if (!h->log_channel || !cli_catalog_os_log_open(nullptr, token, cap)) {
			return false;
		}
		h->log_tokens.push_back(token);
		return true;
	}

	static bool
	absolute_path(void *, const char *path, char *out, size_t cap)
	{
		(void)snprintf(out, cap, "/abs/%s", path);
		return true;
	}

	//! The channel of @p token is closed: nothing is left behind for it.
	static bool
	channel_gone(const std::string &token)
	{
#ifdef _WIN32
		return dxr_log_channel_open(token.c_str()) == DXR_LOG_CHANNEL_NONE;
#else
		char name[1024];
		dxr_log_channel_name(token.c_str(), name, sizeof(name));
		return !std::filesystem::exists(name);
#endif
	}

	static cJSON *
	licence(void *ud, const char *item_class, const char *registered_id, char *cell, size_t cap)
	{
		Host *h = static_cast<Host *>(ud);
		h->licence_asks.push_back(std::string(item_class) + "/" + registered_id);
		if (strcmp(item_class, "display_processor") != 0) {
			return nullptr; // not registered on this machine
		}
		(void)snprintf(cell, cap, "licensed (hardware)");
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "state", "licensed");
		cJSON_AddStringToObject(o, "source", "hardware");
		return o;
	}

	Result
	run(std::vector<const char *> args)
	{
		static int serial = 0;
		const std::filesystem::path dir = std::filesystem::temp_directory_path();
		const std::string tag =
		    "dxr_cli_catalog_" + std::to_string(test_pid()) + "_" + std::to_string(serial++);
		const std::filesystem::path log = dir / (tag + ".log");
		const std::filesystem::path outp = dir / (tag + ".out");
		const std::filesystem::path errp = dir / (tag + ".err");
		std::filesystem::remove(log);
		set_env("FAKE_DXR_GET_LOG", log.string().c_str());
		set_env("FAKE_DXR_GET_ADMIN", admin ? "1" : nullptr);

		std::vector<const char *> argv = {"displayxr-cli", "catalog"};
		argv.insert(argv.end(), args.begin(), args.end());

		FILE *out = fopen(outp.string().c_str(), "wb");
		FILE *err = fopen(errp.string().c_str(), "wb");
		REQUIRE(out != nullptr);
		REQUIRE(err != nullptr);
		struct cli_catalog_ops ops = {};
		ops.ud = this;
		ops.engine = have_engine ? FAKE_DXR_GET : nullptr;
		ops.engine_missing = have_engine ? nullptr : "no engine on this platform";
		ops.run = cli_catalog_os_run;
		ops.run_elevated = run_elevated;
		ops.is_interactive = is_interactive;
		ops.signer = signer;
		ops.log_open = log_open;
		ops.log_take = cli_catalog_os_log_take;
		ops.absolute_path = absolute_paths ? absolute_path : nullptr;
		ops.licence = licence;
		ops.out = out;
		ops.err = err;

		Result r;
		r.exit = cli_catalog_main(&ops, static_cast<int>(argv.size()), argv.data());
		fclose(out);
		fclose(err);
		r.out = slurp(outp);
		r.err = slurp(errp);
		std::istringstream lines(slurp(log));
		for (std::string line; std::getline(lines, line);) {
			r.engine.push_back(line);
		}
		r.json = cJSON_Parse(r.out.c_str());
		std::filesystem::remove(log);
		std::filesystem::remove(outp);
		std::filesystem::remove(errp);
		return r;
	}
};

bool
has(const std::string &hay, const char *needle)
{
	return hay.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("catalog: every exit code has its own one-line message", "[cli][catalog]")
{
	const int codes[] = {0, 2, 10, 11, 20, 21, 30, 40, 41, 42, 43, 50, 60, 61, 62, 63, 64, 65};
	const std::string unknown = cli_catalog_exit_message(77);
	for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
		const std::string a = cli_catalog_exit_message(codes[i]);
		CHECK(a != unknown);
		CHECK(a.find('\n') == std::string::npos);
		for (size_t j = i + 1; j < sizeof(codes) / sizeof(codes[0]); j++) {
			CHECK(a != cli_catalog_exit_message(codes[j]));
		}
	}
}

TEST_CASE("catalog: the command line is checked before anything runs", "[cli][catalog]")
{
	struct cli_catalog_request req;
	char err[240];
	auto parse = [&](std::vector<const char *> args) {
		std::vector<const char *> argv = {"displayxr-cli", "catalog"};
		argv.insert(argv.end(), args.begin(), args.end());
		return cli_catalog_parse(static_cast<int>(argv.size()), argv.data(), &req, err, sizeof(err));
	};
	CHECK(parse({"list"}));
	CHECK(parse({"list", "--installed", "--json"}));
	CHECK(parse({"outdated", "--json"}));
	CHECK(parse({"show", "leia-sr"}));
	CHECK(parse({"install", "a", "b", "--elevate"}));
	CHECK(req.n_ids == 2);
	CHECK(parse({"install", "--default", "--allow-runtime"}));
	CHECK(parse({"uninstall", "a", "--no-elevate"}));
	CHECK(parse({"update", "--all", "--timeout", "30"}));
	CHECK(req.timeout_s == 30);
	CHECK(cli_catalog_timeout_ms(&req, true) == 30000u);
	CHECK(parse({"update", "a", "--catalog", "C:\\cat.json"}));
	CHECK(std::string(req.catalog) == "C:\\cat.json");

	CHECK_FALSE(parse({}));
	CHECK_FALSE(parse({"remove", "a"}));
	CHECK_FALSE(parse({"list", "a"}));
	CHECK_FALSE(parse({"show"}));
	CHECK_FALSE(parse({"show", "a", "b"}));
	CHECK_FALSE(parse({"install"}));
	CHECK_FALSE(parse({"install", "a", "--default"}));
	CHECK_FALSE(parse({"uninstall"}));
	CHECK_FALSE(parse({"update"})); // a bare `update` never means "everything"
	CHECK_FALSE(parse({"update", "a", "--all"}));
	CHECK_FALSE(parse({"list", "--elevate"}));
	CHECK_FALSE(parse({"install", "a", "--elevate", "--no-elevate"}));
	CHECK_FALSE(parse({"install", "a", "--installed"}));
	CHECK_FALSE(parse({"uninstall", "a", "--allow-runtime"}));
	CHECK_FALSE(parse({"install", "a", "--timeout", "soon"}));
	CHECK_FALSE(parse({"install", "a", "--frobnicate"}));
	// Nothing with a control character is handed to the engine (it would refuse it too):
	// a line feed in an id could otherwise forge a `result:` line in the engine's log.
	CHECK_FALSE(parse({"install", "a\nresult: b: 0 installed"}));
	CHECK(has(err, "control character"));
	CHECK_FALSE(parse({"uninstall", "a\rb"}));
	CHECK_FALSE(parse({"list", "--catalog", "C:\\cat\x1b[2J.json"}));
	CHECK(parse({"install", "caf\xC3\xA9"})); // UTF-8 is not a control character
	CHECK(parse({"install", "a", "--elevate", "--allow-unsigned-engine"}));
	CHECK(req.allow_unsigned_engine);
	CHECK_FALSE(parse({"list", "--allow-unsigned-engine"}));

	// The defaults: minutes for a listing, hours for what runs installers.
	REQUIRE(parse({"list"}));
	CHECK(cli_catalog_timeout_ms(&req, false) == 600000u);
	CHECK(cli_catalog_timeout_ms(&req, true) == 7200000u);
}

TEST_CASE("catalog: Windows command-line quoting", "[cli][catalog]")
{
	char out[128];
	auto q = [&](const char *s) {
		REQUIRE(cli_catalog_quote_arg(s, out, sizeof(out)));
		return std::string(out);
	};
	CHECK(q("install") == "install");
	CHECK(q("C:\\dir\\dxr-get.exe") == "C:\\dir\\dxr-get.exe");
	CHECK(q("") == "\"\"");
	CHECK(q("a b") == "\"a b\"");
	CHECK(q("C:\\Program Files\\x\\") == "\"C:\\Program Files\\x\\\\\"");
	CHECK(q("say \"hi\"") == "\"say \\\"hi\\\"\"");
	CHECK(q("a\\\"b c") == "\"a\\\\\\\"b c\"");
	char tiny[4];
	CHECK_FALSE(cli_catalog_quote_arg("too long", tiny, sizeof(tiny)));
}

TEST_CASE("catalog: no engine means no answer, not a stub", "[cli][catalog]")
{
	Host h;
	h.have_engine = false;
	Result r = h.run({"list"});
	CHECK(r.exit == 60);
	CHECK(r.out.empty());
	CHECK(has(r.err, "no engine on this platform"));
	CHECK(r.engine.empty());

	Result j = h.run({"install", "open-conv", "--json", "--elevate"});
	CHECK(j.exit == 60);
	CHECK(j.num({"exit"}) == 60);
	CHECK(cJSON_IsNull(j.at({"engine"})));
	CHECK(cJSON_GetArraySize(j.at({"calls"})) == 0);
	CHECK(h.elevated_runs == 0);

	// The shipped locator: nothing on macOS / Linux, and it says why.
#ifndef _WIN32
	char path[64], why[300];
	CHECK_FALSE(cli_catalog_os_locate_engine(path, sizeof(path), why, sizeof(why)));
	CHECK(has(why, "Windows only"));
#endif
}

TEST_CASE("catalog list --json wraps the engine's document unchanged", "[cli][catalog]")
{
	Host h;
	Result r = h.run({"list", "--json"});
	REQUIRE(r.exit == 0);
	REQUIRE(r.json != nullptr);
	REQUIRE(r.engine.size() == 1);
	CHECK(r.engine[0] == "list|--json");
	CHECK(r.num({"schema"}) == 1);
	CHECK(r.str({"verb"}) == "list");
	CHECK(r.num({"exit"}) == 0);
	CHECK(r.str({"engine"}) == FAKE_DXR_GET);
	CHECK(r.num({"calls", "0", "exit"}) == 0);
	CHECK(cJSON_IsFalse(r.at({"calls", "0", "elevated"})));
	// The engine's own field names, untouched.
	CHECK(r.num({"calls", "0", "output", "schema"}) == 2);
	CHECK(cJSON_GetArraySize(r.at({"calls", "0", "output", "items"})) == 5);
	CHECK(r.str({"calls", "0", "output", "items", "1", "registered_id"}) == "leia-sr");
	CHECK(r.str({"calls", "0", "output", "items", "1", "state"}) == "outdated");
	// The licence of installed plug-ins only, keyed by catalog id.
	CHECK(r.str({"licences", "leia-sr", "state"}) == "licensed");
	CHECK(r.at({"licences", "open-conv"}) == nullptr);    // not installed: never asked
	CHECK(r.at({"licences", "vendor.hands"}) == nullptr); // asked, not registered here
	CHECK(h.licence_asks == std::vector<std::string>{"display_processor/leia-sr", "input_provider/vendor-hands"});
}

TEST_CASE("catalog list, outdated, show for a person", "[cli][catalog]")
{
	Host h;
	Result l = h.run({"list"});
	REQUIRE(l.exit == 0);
	CHECK(l.engine == std::vector<std::string>{"list|--json"});
	CHECK(has(l.out, "LICENCE"));
	CHECK(has(l.out, "leia-sr"));
	CHECK(has(l.out, "licensed (hardware)"));
	CHECK(has(l.out, "not_installed"));

	Result o = h.run({"outdated"});
	REQUIRE(o.exit == 0);
	CHECK(o.engine == std::vector<std::string>{"outdated|--json"});
	CHECK(has(o.out, "vendor.hands"));
	CHECK_FALSE(has(o.out, "browser"));

	Result none = h.run({"outdated", "--catalog", "current"});
	CHECK(none.exit == 0);
	CHECK(has(none.out, "Everything installed is at the catalog's version."));

	Result inst = h.run({"list", "--installed", "--catalog", "ignored"});
	REQUIRE(inst.exit == 0);
	CHECK(inst.engine == std::vector<std::string>{"list|--installed|--json"}); // the receipts need no catalog
	CHECK(has(inst.out, "INSTALLED AT"));

	// By alias, as the engine resolves items.
	Result s = h.run({"show", "leia_plugin"});
	REQUIRE(s.exit == 0);
	CHECK(has(s.out, "leia-sr"));
	CHECK(has(s.out, "Weaving for Leia panels"));
	CHECK(has(s.out, "high confidence"));
	CHECK(has(s.out, "licensed (hardware)"));

	Result sj = h.run({"show", "browser", "--json"});
	REQUIRE(sj.exit == 0);
	CHECK(sj.str({"item", "id"}) == "browser");
	CHECK(sj.str({"item", "state"}) == "current");

	Result missing = h.run({"show", "ghost", "--json"});
	CHECK(missing.exit == 40);
	CHECK(cJSON_IsNull(missing.at({"item"})));
	CHECK(has(missing.str({"message"}), "does not list it"));
}

TEST_CASE("catalog: --catalog reaches the engine as written", "[cli][catalog]")
{
	Host h;
	// A value that needs every quoting rule on Windows.
	const char *odd = "C:\\My Cat\\a \"b\"\\";
	Result r = h.run({"list", "--json", "--catalog", odd});
	REQUIRE(r.exit == 0);
	REQUIRE(r.engine.size() == 1);
	CHECK(r.engine[0] == std::string("--catalog|") + odd + "|list|--json");

	// The CLI's host makes a catalog FILE absolute, for every engine call of the
	// command: the elevated run does not start in the caller's directory. A URL
	// is left as written.
	Host abs;
	abs.absolute_paths = true;
	Result rel = abs.run({"install", "open-conv", "--json", "--elevate", "--catalog", "cat.json"});
	REQUIRE(rel.engine.size() >= 2);
	CHECK(rel.engine[0] == "--catalog|/abs/cat.json|install|open-conv|--no-runtime|--json");
	CHECK(rel.engine[1] == "--catalog|/abs/cat.json|install|open-conv|--no-runtime|--log-pipe|<log>");
	Result url = abs.run({"list", "--json", "--catalog", "HTTPS://example.invalid/c.json"});
	CHECK(url.engine[0] == "--catalog|HTTPS://example.invalid/c.json|list|--json");
}

TEST_CASE("catalog: offline, unexpected output and a slow engine", "[cli][catalog]")
{
	Host h;
	Result off = h.run({"list", "--json", "--catalog", "offline"});
	CHECK(off.exit == 10);
	CHECK(off.num({"exit"}) == 10);
	CHECK(cJSON_IsTrue(off.at({"calls", "0", "output", "offline"})));
	CHECK(has(off.str({"message"}), "offline"));

	Result offh = h.run({"show", "leia-sr", "--catalog", "offline"});
	CHECK(offh.exit == 10);
	CHECK(has(offh.err, "offline"));

	Result bad = h.run({"list", "--catalog", "garbage"});
	CHECK(bad.exit == 63);
	CHECK(bad.out.empty());

	// Bounded: the fake sleeps 5 s, the wait ends after 1 s, and a read-only
	// call is ended rather than left behind.
	// install / uninstall: exit 0 without the document is 63 too, never "installed".
	Result badi = h.run({"install", "open-conv", "--json", "--catalog", "garbage"});
	CHECK(badi.exit == 63);
	CHECK(badi.num({"items", "0", "exit"}) == 63);
	CHECK(cJSON_GetArraySize(badi.at({"items", "0", "changed"})) == 0);
	CHECK(cJSON_IsNull(badi.at({"calls", "0", "output"})));
	Result badu = h.run({"uninstall", "leia-sr", "open-conv", "--catalog", "garbage"});
	CHECK(badu.exit == 63);
	CHECK(has(badu.err, "exited 0 without saying what it did"));
	CHECK_FALSE(has(badu.out, "uninstalled"));
	CHECK(h.elevated_runs == 0);

	Result slow = h.run({"list", "--json", "--catalog", "slow", "--timeout", "1"});
	CHECK(slow.exit == 62);
	CHECK(slow.num({"calls", "0", "exit"}) == 62);
}

TEST_CASE("catalog install: elevation is asked for only when it is all that is missing", "[cli][catalog]")
{
	SECTION("--json never elevates by itself")
	{
		Host h;
		h.interactive = true; // even at a terminal
		Result r = h.run({"install", "open-conv", "--json"});
		CHECK(r.exit == 50);
		CHECK(h.elevated_runs == 0);
		CHECK(h.signer_asks == 0); // nothing is verified unless something is about to be elevated
		CHECK(r.engine == std::vector<std::string>{"install|open-conv|--no-runtime|--json"});
		CHECK(r.num({"items", "0", "exit"}) == 50);
		CHECK(has(r.str({"items", "0", "message"}), "--elevate"));
		// The plan the engine printed is still there for the caller.
		CHECK(r.str({"calls", "0", "output", "actions", "0", "action"}) == "install");
	}
	SECTION("not at a terminal: no prompt")
	{
		Host h;
		Result r = h.run({"install", "open-conv"});
		CHECK(r.exit == 50);
		CHECK(h.elevated_runs == 0);
		CHECK(has(r.err, "needs an elevated process"));
	}
	SECTION("--json --elevate: one elevated run of the same command, without --json, with a log")
	{
		Host h;
		Result r = h.run({"install", "open-conv", "--json", "--elevate"});
		CHECK(r.exit == 0);
		CHECK(h.elevated_runs == 1);
		// The last line is this CLI reading the receipts, to say what to expect.
		CHECK(r.engine == std::vector<std::string>{"install|open-conv|--no-runtime|--json",
		                                           "install|open-conv|--no-runtime|--log-pipe|<log>",
		                                           "list|--installed|--json"});
		CHECK(cJSON_GetArraySize(r.at({"calls"})) == 2);
		CHECK(r.num({"calls", "0", "exit"}) == 50);
		CHECK(cJSON_IsTrue(r.at({"calls", "1", "elevated"})));
		CHECK(cJSON_IsNull(r.at({"calls", "1", "output"})));
		// The elevated engine's own words, relayed through the pipe, which is gone afterwards.
		CHECK(has(r.str({"calls", "1", "log"}), "open-conv: installed"));
		REQUIRE(h.log_tokens.size() == 1);
		CHECK(dxr_log_token_ok(h.log_tokens[0].c_str())); // a token, not a path
		CHECK(r.str({"calls", "1", "args", "4"}) == h.log_tokens[0]);
		CHECK(Host::channel_gone(h.log_tokens[0]));
		CHECK(r.num({"items", "0", "exit"}) == 0);
		CHECK(cJSON_IsTrue(r.at({"items", "0", "elevated"})));
		CHECK(r.str({"items", "0", "changed", "0"}) == "open-conv");
		CHECK(r.str({"items", "0", "expect", "0", "class"}) == "conversion_provider");
		CHECK(has(r.str({"items", "0", "expect", "0", "note"}), "displayxr-cli conv list"));
	}
	SECTION("at a terminal: asks, says so, and relays what the engine said")
	{
		Host h;
		h.interactive = true;
		Result r = h.run({"install", "leia-sr"});
		CHECK(r.exit == 0);
		CHECK(h.elevated_runs == 1);
		CHECK(has(r.err, "Asking for elevation"));
		CHECK(has(r.err, "dxr-get: leia-sr: installed"));
		CHECK(has(r.out, "leia-sr: installed"));
		CHECK(has(r.out, "next session")); // a running app keeps its display processor
	}
	SECTION("--no-elevate wins at a terminal")
	{
		Host h;
		h.interactive = true;
		Result r = h.run({"install", "leia-sr", "--no-elevate"});
		CHECK(r.exit == 50);
		CHECK(h.elevated_runs == 0);
	}
	SECTION("a declined prompt is exit 50 and changes nothing")
	{
		Host h;
		h.elevated_status = CLI_CATALOG_RUN_DECLINED;
		Result r = h.run({"install", "open-conv", "--json", "--elevate"});
		CHECK(r.exit == 50);
		CHECK(h.elevated_runs == 1);
		CHECK(has(r.str({"items", "0", "message"}), "declined"));
		CHECK(cJSON_GetArraySize(r.at({"items", "0", "changed"})) == 0);
		REQUIRE(h.log_tokens.size() == 1);
		CHECK(Host::channel_gone(h.log_tokens[0])); // closed even when nothing connected to it
	}
	SECTION("already elevated: one call does the work")
	{
		Host h;
		h.admin = true;
		Result r = h.run({"install", "open-conv", "--json"});
		CHECK(r.exit == 0);
		CHECK(h.elevated_runs == 0);
		CHECK(h.signer_asks == 0);
		CHECK(cJSON_GetArraySize(r.at({"calls"})) == 1);
		CHECK(cJSON_IsFalse(r.at({"items", "0", "elevated"})));
		CHECK(r.str({"items", "0", "changed", "0"}) == "open-conv");
	}
	SECTION("what elevation cannot fix is answered without a prompt")
	{
		Host h;
		// The engine checks for elevation before it reports "not fetchable";
		// the plan's gate already says it.
		Result r = h.run({"install", "nofetch", "--json", "--elevate"});
		CHECK(r.exit == 40);
		CHECK(h.elevated_runs == 0);
		CHECK(has(r.str({"items", "0", "message"}), "no signed Windows build"));

		Result none = h.run({"install", "browser", "--json", "--elevate"});
		CHECK(none.exit == 0);
		CHECK(h.elevated_runs == 0);
		CHECK(has(none.str({"items", "0", "message"}), "nothing to do"));
		CHECK(cJSON_GetArraySize(none.at({"items", "0", "changed"})) == 0);
	}
}

TEST_CASE("catalog: several items are one engine call, and one consent prompt", "[cli][catalog]")
{
	SECTION("install: one call with this process's rights, one elevated call for what needs it")
	{
		Host h;
		Result r =
		    h.run({"install", "open-conv", "browser", "incompat", "nofetch", "leia-sr", "--json", "--elevate"});
		CHECK(h.elevated_runs == 1);
		REQUIRE(r.engine.size() == 3);
		CHECK(r.engine[0] == "install|open-conv|browser|incompat|nofetch|leia-sr|--no-runtime|--json");
		// Only the two that elevation helps: not the current one, not the refused ones.
		CHECK(r.engine[1] == "install|open-conv|leia-sr|--no-runtime|--log-pipe|<log>");
		CHECK(r.engine[2] == "list|--installed|--json");
		CHECK(r.exit == 41); // the first failure, in the order given
		REQUIRE(cJSON_GetArraySize(r.at({"items"})) == 5);
		CHECK(r.num({"items", "0", "exit"}) == 0);
		CHECK(cJSON_IsTrue(r.at({"items", "0", "elevated"})));
		CHECK(r.num({"items", "1", "exit"}) == 0);
		CHECK(cJSON_IsFalse(r.at({"items", "1", "elevated"})));
		CHECK(has(r.str({"items", "1", "message"}), "nothing to do"));
		CHECK(r.num({"items", "2", "exit"}) == 41);
		CHECK(r.num({"items", "3", "exit"}) == 40);
		CHECK(r.num({"items", "4", "exit"}) == 0);
		CHECK(r.str({"items", "4", "expect", "0", "class"}) == "display_processor");
		// The engine's several-item document, embedded as it printed it.
		CHECK(r.num({"calls", "0", "output", "exit"}) == 50);
		CHECK(r.str({"calls", "0", "output", "items", "2", "result"}) == "incompatible");
		CHECK(r.str({"calls", "0", "output", "items", "0", "plan", "actions", "0", "action"}) == "install");
	}
	SECTION("each item's own exit code comes back from the elevated run's log")
	{
		Host h;
		h.interactive = true;
		// Elevated, the first is removed and the second still cannot be.
		Result r = h.run({"uninstall", "leia-sr", "browser", "open-conv", "ghost"});
		CHECK(h.elevated_runs == 1);
		CHECK(r.engine == std::vector<std::string>{"list|--installed|--json",
		                                           "uninstall|leia-sr|browser|open-conv|ghost|--json",
		                                           "uninstall|leia-sr|open-conv|--log-pipe|<log>"});
		CHECK(r.exit == 43);
		CHECK(has(r.out, "leia-sr: uninstalled"));
		CHECK(has(r.out, "open-conv: uninstalled"));
		CHECK(has(r.err, "one consent prompt for 2 items"));
		CHECK(has(r.err, "browser"));
		CHECK(has(r.err, "ghost"));
		CHECK_FALSE(has(r.err, "result: ")); // the machine-readable lines are not shown to a person
	}
	SECTION("a failure inside the elevated run belongs to its item")
	{
		Host h;
		h.elevated_admin = false; // the elevated engine still answers 50: nothing was installed
		Result r = h.run({"install", "open-conv", "leia-sr", "--json", "--elevate"});
		CHECK(r.exit == 50);
		CHECK(r.num({"items", "0", "exit"}) == 50);
		CHECK(r.num({"items", "1", "exit"}) == 50);
		CHECK(cJSON_GetArraySize(r.at({"items", "0", "expect"})) == 0);
	}
	SECTION("without a prompt allowed, every item that needs one says so")
	{
		Host h;
		Result r = h.run({"install", "open-conv", "leia-sr", "--json"});
		CHECK(r.exit == 50);
		CHECK(h.elevated_runs == 0);
		CHECK(r.engine.size() == 1);
		CHECK(has(r.str({"items", "1", "message"}), "--elevate"));
	}
	SECTION("a long log does not lose an item's result")
	{
		// ~600 KB of messages around the result lines: more than the CLI keeps.
		// The second item's own code must still be its own, not the first failure's.
		Host h;
		h.elevated_admin = true;
		Result r = h.run({"uninstall", "leia-sr", "open-conv", "--json", "--elevate", "--catalog", "flood"});
		CHECK(h.elevated_runs == 1);
		CHECK(r.exit == 0);
		CHECK(r.num({"items", "0", "exit"}) == 0);
		CHECK(r.num({"items", "1", "exit"}) == 0);
		const std::string log = r.str({"calls", "1", "log"});
		CHECK(log.size() < 400u * 1024u); // bounded
		CHECK(has(log, "only result lines follow"));
		CHECK(has(log, "result: leia-sr: 0 uninstalled"));
		CHECK(has(log, "result: open-conv: 0 uninstalled"));

		// And when the run fails for one item only: each keeps its own code.
		Host f;
		f.elevated_admin = false; // elevated, both still answer 50
		Result mixed =
		    f.run({"uninstall", "leia-sr", "open-conv", "--json", "--elevate", "--catalog", "flood"});
		CHECK(mixed.num({"items", "0", "exit"}) == 50);
		CHECK(mixed.num({"items", "1", "exit"}) == 50);

		// For a person the relay is bounded too, and says so.
		Host p;
		p.interactive = true;
		Result shown = p.run({"uninstall", "leia-sr", "open-conv", "--catalog", "flood"});
		CHECK(shown.exit == 0);
		CHECK(has(shown.err, "more lines not shown"));
		CHECK(shown.err.size() < 100u * 1024u);
	}
	SECTION("a failed run that reports no result for an item is not guessed at")
	{
		Host h;
		h.elevated_admin = false; // the run fails (50)...
		Result r = h.run({"install", "open-conv", "leia-sr", "--json", "--elevate", "--catalog", "mute"});
		CHECK(h.elevated_runs == 1);
		// ...and says nothing per item: neither inherits the first failure's code.
		CHECK(r.exit == 63);
		CHECK(r.num({"items", "0", "exit"}) == 63);
		CHECK(r.num({"items", "1", "exit"}) == 63);
		CHECK(has(r.str({"items", "1", "message"}), "reported no result for this item"));
		CHECK(cJSON_GetArraySize(r.at({"items", "0", "expect"})) == 0);

		// Exit 0 of a several-item run means every item succeeded: no line needed.
		Host ok;
		Result fine = ok.run({"install", "open-conv", "leia-sr", "--json", "--elevate", "--catalog", "mute"});
		CHECK(fine.exit == 0);
		CHECK(fine.num({"items", "1", "exit"}) == 0);
	}
	SECTION("no log channel: several items are not started elevated; one item is")
	{
		Host h;
		h.log_channel = false;
		Result r = h.run({"install", "open-conv", "leia-sr", "--json", "--elevate"});
		CHECK(h.elevated_runs == 0);
		CHECK(r.engine.size() == 1);
		CHECK(r.exit == 61);
		CHECK(r.num({"items", "0", "exit"}) == 61);
		CHECK(r.num({"items", "1", "exit"}) == 61);
		CHECK(has(r.str({"items", "0", "message"}), "Nothing was changed"));

		// One item: its exit code is the whole answer, so it runs, without --log-pipe.
		Result one = h.run({"install", "open-conv", "--json", "--elevate"});
		CHECK(h.elevated_runs == 1);
		CHECK(one.exit == 0);
		CHECK(one.engine[1] == "install|open-conv|--no-runtime");
		CHECK(cJSON_IsNull(one.at({"calls", "1", "log"})));
	}
	SECTION("the result line of an engine log")
	{
		char id[64];
		int code = -1;
		CHECK(cli_catalog_parse_result_line("result: leia-sr: 0 installed", id, sizeof(id), &code) == 1);
		CHECK(std::string(id) == "leia-sr");
		CHECK(code == 0);
		CHECK(cli_catalog_parse_result_line("result: vendor: hands: 41 incompatible", id, sizeof(id), &code) ==
		      1);
		CHECK(std::string(id) == "vendor: hands");
		CHECK(code == 41);
		CHECK(cli_catalog_parse_result_line("leia-sr: installed", id, sizeof(id), &code) == 0);
		CHECK(cli_catalog_parse_result_line("result: no code here", id, sizeof(id), &code) == 0);
		char tiny[4];
		CHECK(cli_catalog_parse_result_line("result: leia-sr: 0 installed", tiny, sizeof(tiny), &code) == 0);
	}
}

TEST_CASE("catalog: an engine that is not validly signed is never started elevated", "[cli][catalog]")
{
	SECTION("unsigned engine: refused, unless explicitly allowed")
	{
		Host h;
		h.engine_signed = false;
		Result r = h.run({"install", "open-conv", "--json", "--elevate"});
		CHECK(r.exit == 65);
		CHECK(h.elevated_runs == 0);
		CHECK(has(r.str({"items", "0", "message"}), "no valid Authenticode signature"));
		CHECK(has(r.str({"items", "0", "message"}), "--allow-unsigned-engine"));
		// It was still run with this process's own rights: that needs no signature.
		CHECK(r.engine == std::vector<std::string>{"install|open-conv|--no-runtime|--json"});

		// --elevate does not imply it.
		Result ok = h.run({"install", "open-conv", "--json", "--elevate", "--allow-unsigned-engine"});
		CHECK(ok.exit == 0);
		CHECK(h.elevated_runs == 1);
	}
	SECTION("unsigned engine: listing and non-elevated runs stay allowed")
	{
		Host h;
		h.engine_signed = false;
		CHECK(h.run({"list", "--json"}).exit == 0);
		h.admin = true; // an elevated terminal runs it directly; nothing is relaunched
		CHECK(h.run({"install", "open-conv", "--json"}).exit == 0);
		CHECK(h.signer_asks == 0);
	}
	SECTION("validly signed, but not by a DisplayXR signer")
	{
		// Whether or not the CLI is signed: a valid signature of anyone else's is not enough.
		for (bool cli_signed : {false, true}) {
			Host h;
			h.self_signed = cli_signed;
			h.engine_cn = "Somebody Else";
			Result r = h.run({"uninstall", "leia-sr", "--json", "--elevate"});
			CHECK(r.exit == 65);
			CHECK(h.elevated_runs == 0);
			CHECK(has(r.str({"items", "0", "message"}), "Somebody Else"));
			CHECK(has(r.str({"items", "0", "message"}), "not a DisplayXR signer"));

			// The explicit switch stays the only way around it.
			Result ok = h.run({"uninstall", "leia-sr", "--json", "--elevate", "--allow-unsigned-engine"});
			CHECK(ok.exit == 0);
			CHECK(h.elevated_runs == 1);
		}
	}
	SECTION("the rule")
	{
		char why[200];
		CHECK(cli_catalog_engine_trusted(true, "Leia, Inc.", true, "Leia, Inc.", why, sizeof(why)));
		// A development CLI is unsigned: the engine must still be a first-party one.
		CHECK(cli_catalog_engine_trusted(true, "Leia, Inc.", false, "", why, sizeof(why)));
		CHECK_FALSE(cli_catalog_engine_trusted(true, "Other", false, "", why, sizeof(why)));
		CHECK(has(why, "not a DisplayXR signer"));
		CHECK_FALSE(cli_catalog_engine_trusted(true, "", false, "", why, sizeof(why)));
		CHECK_FALSE(cli_catalog_engine_trusted(true, "leia, inc.", false, "", why, sizeof(why))); // exact name
		CHECK_FALSE(cli_catalog_engine_trusted(false, "", false, "", why, sizeof(why)));
		CHECK_FALSE(cli_catalog_engine_trusted(false, "Leia, Inc.", true, "Leia, Inc.", why, sizeof(why)));
		CHECK_FALSE(cli_catalog_engine_trusted(true, "Other", true, "Leia, Inc.", why, sizeof(why)));
		// A first-party engine beside a CLI signed by someone else is refused too.
		CHECK_FALSE(cli_catalog_engine_trusted(true, "Leia, Inc.", true, "Other", why, sizeof(why)));
		CHECK(has(why, "this CLI's signer"));
	}
}

TEST_CASE("catalog: the runtime itself is never installed or replaced from the CLI", "[cli][catalog]")
{
	Host h;
	h.admin = true;
	// --no-runtime on every install, whatever is asked.
	CHECK(h.run({"install", "open-conv"}).engine[0] == "install|open-conv|--no-runtime|--json");
	CHECK(h.run({"install", "--default"}).engine[0] == "install|--default|--no-runtime|--json");
	CHECK(h.run({"install", "--default", "--catalog", "x"}).engine[0] ==
	      "--catalog|x|install|--default|--no-runtime|--json");

	// Naming the runtime: refused before any engine call, and the rest still happens.
	Result named = h.run({"install", "runtime", "open-conv", "--json"});
	CHECK(named.exit == 64);
	CHECK(named.engine[0] == "install|open-conv|--no-runtime|--json");
	CHECK(named.str({"items", "0", "id"}) == "runtime");
	CHECK(named.num({"items", "0", "exit"}) == 64);
	CHECK(has(named.str({"items", "0", "message"}), "runtime installer or the bundle"));
	CHECK(named.num({"items", "1", "exit"}) == 0);

	Result upd = h.run({"update", "runtime"});
	CHECK(upd.exit == 64);
	CHECK(upd.engine == std::vector<std::string>{"outdated|--json"});
	CHECK(has(upd.err, "runtime installer or the bundle"));

	// --allow-runtime is reserved: nothing runs.
	Result allow = h.run({"install", "open-conv", "--allow-runtime", "--json"});
	CHECK(allow.exit == 64);
	CHECK(allow.engine.empty());
	CHECK(has(allow.str({"message"}), "not available yet"));

	// The default set reports each plug-in it changed.
	Result d = h.run({"install", "--default", "--json"});
	CHECK(d.exit == 0);
	CHECK(cJSON_IsNull(d.at({"items", "0", "id"})));
	CHECK(cJSON_GetArraySize(d.at({"items", "0", "changed"})) == 2);
	CHECK(d.str({"items", "0", "expect", "1", "class"}) == "display_processor");

	// A refusal because of the runtime says what to do.
	Result inc = h.run({"install", "incompat"});
	CHECK(inc.exit == 41);
	CHECK(has(inc.err, "needs runtime 3.0.0"));
	CHECK(has(inc.err, "update the runtime first"));
}

TEST_CASE("catalog: the engine's exit code is the CLI's", "[cli][catalog]")
{
	Host h;
	h.admin = true;
	const struct
	{
		const char *id;
		int code;
	} cases[] = {{"badsig", 11}, {"dlfail", 20}, {"verify", 21}, {"child", 30}, {"incompat", 41}};
	for (const auto &c : cases) {
		Result r = h.run({"install", c.id, "--json"});
		CHECK(r.exit == c.code);
		CHECK(r.num({"exit"}) == c.code);
		CHECK(r.num({"items", "0", "exit"}) == c.code);
		CHECK(has(r.str({"items", "0", "message"}), cli_catalog_exit_message(c.code)));
	}

	// Several items: each is tried, the first failure is the exit code.
	Result many = h.run({"install", "open-conv", "verify", "dlfail", "browser", "--json"});
	CHECK(many.exit == 21);
	CHECK(many.engine[0] == "install|open-conv|verify|dlfail|browser|--no-runtime|--json");
	CHECK(cJSON_GetArraySize(many.at({"items"})) == 4);
	CHECK(many.num({"items", "0", "exit"}) == 0);
	CHECK(many.num({"items", "2", "exit"}) == 20);
	CHECK(many.num({"items", "3", "exit"}) == 0);
}

TEST_CASE("catalog uninstall", "[cli][catalog]")
{
	Host h;
	// 42 and 43 are the engine's answer before it asks for elevation.
	Result none = h.run({"uninstall", "ghost", "--json", "--elevate"});
	CHECK(none.exit == 42);
	CHECK(h.elevated_runs == 0);
	CHECK(none.engine.back() == "uninstall|ghost|--json");
	CHECK(has(none.str({"items", "0", "message"}), "no receipt"));

	Result cannot = h.run({"uninstall", "browser", "--json", "--elevate"});
	CHECK(cannot.exit == 43);
	CHECK(h.elevated_runs == 0);
	CHECK(has(cannot.str({"items", "0", "message"}), "Windows Settings"));

	Result need = h.run({"uninstall", "leia-sr", "--json"});
	CHECK(need.exit == 50);
	CHECK(h.elevated_runs == 0);

	Result ok = h.run({"uninstall", "leia-sr", "--json", "--elevate"});
	CHECK(ok.exit == 0);
	CHECK(h.elevated_runs == 1);
	// The class is read from the receipt BEFORE the item is removed.
	CHECK(ok.engine == std::vector<std::string>{"list|--installed|--json", "uninstall|leia-sr|--json",
	                                            "uninstall|leia-sr|--log-pipe|<log>"});
	CHECK(ok.str({"items", "0", "expect", "0", "class"}) == "display_processor");
	CHECK(has(ok.str({"items", "0", "message"}), "uninstalled"));

	Host a;
	a.admin = true;
	Result adm = a.run({"uninstall", "leia-sr", "ghost"});
	CHECK(adm.exit == 42);
	CHECK(a.elevated_runs == 0);
	CHECK(has(adm.out, "leia-sr: uninstalled"));
	CHECK(has(adm.err, "ghost"));
}

TEST_CASE("catalog update is install for what outdated reports", "[cli][catalog]")
{
	SECTION("--all: never the runtime, never a link-out")
	{
		Host h;
		h.admin = true;
		Result r = h.run({"update", "--all", "--json"});
		CHECK(r.exit == 0);
		CHECK(r.engine[0] == "outdated|--json");
		CHECK(r.engine[1] == "install|leia-sr|--no-runtime|--json");
		for (const std::string &line : r.engine) {
			CHECK_FALSE(has(line, "|runtime"));
			CHECK_FALSE(has(line, "vendor.hands"));
		}
		REQUIRE(cJSON_GetArraySize(r.at({"items"})) == 3);
		CHECK(r.str({"items", "0", "id"}) == "runtime");
		CHECK(r.str({"items", "0", "action"}) == "none");
		CHECK(r.num({"items", "0", "exit"}) == 0); // reported, not a failure of --all
		CHECK(has(r.str({"items", "0", "message"}), "runtime installer or the bundle"));
		CHECK(r.str({"items", "1", "id"}) == "vendor.hands");
		CHECK(r.str({"items", "1", "action"}) == "none");
		CHECK(r.str({"items", "2", "id"}) == "leia-sr");
		CHECK(r.str({"items", "2", "action"}) == "install");
	}
	SECTION("--all with several outdated items: one install call, one consent prompt")
	{
		Host h;
		Result r = h.run({"update", "--all", "--json", "--elevate", "--catalog", "two"});
		CHECK(r.exit == 0);
		CHECK(h.elevated_runs == 1);
		CHECK(r.engine ==
		      std::vector<std::string>{"--catalog|two|outdated|--json",
		                               "--catalog|two|install|leia-sr|open-conv|--no-runtime|--json",
		                               "--catalog|two|install|leia-sr|open-conv|--no-runtime|--log-pipe|<log>",
		                               "list|--installed|--json"});
		CHECK(r.num({"items", "1", "exit"}) == 0);
		CHECK(r.num({"items", "2", "exit"}) == 0);
		CHECK(cJSON_IsTrue(r.at({"items", "2", "elevated"})));
	}
	SECTION("an alias names the catalog item")
	{
		Host h;
		h.admin = true;
		Result r = h.run({"update", "leia_plugin", "--json"});
		CHECK(r.exit == 0);
		CHECK(r.engine[1] == "install|leia-sr|--no-runtime|--json");
	}
	SECTION("named items that are not outdated")
	{
		Host h;
		h.admin = true;
		Result r = h.run({"update", "browser", "open-conv", "ghost", "--json"});
		// One listing explains all three; nothing is installed.
		CHECK(r.engine == std::vector<std::string>{"outdated|--json", "list|--json"});
		CHECK(r.exit == 42);
		CHECK(r.num({"items", "0", "exit"}) == 0);
		CHECK(has(r.str({"items", "0", "message"}), "already at the catalog's version"));
		CHECK(r.num({"items", "1", "exit"}) == 42); // update never installs what is not installed
		CHECK(r.num({"items", "2", "exit"}) == 40);
	}
	SECTION("nothing outdated, and offline")
	{
		Host h;
		Result cur = h.run({"update", "--all", "--catalog", "current"});
		CHECK(cur.exit == 0);
		CHECK(has(cur.out, "Everything installed is at the catalog's version."));
		CHECK(cur.engine.size() == 1);

		Result off = h.run({"update", "--all", "--json", "--catalog", "offline"});
		CHECK(off.exit == 10);
		CHECK(cJSON_GetArraySize(off.at({"items"})) == 0);
	}
}

TEST_CASE("catalog: what to expect after a change, per class", "[cli][catalog]")
{
	// A display processor is not swapped under a running app (service/main.c,
	// plugin-discovery.md §4.2); the service picks its tracking provider once
	// per run. An application touches nothing the runtime loads.
	CHECK(has(cli_catalog_expectation("display_processor"), "next session"));
	CHECK(has(cli_catalog_expectation("tracking_provider"), "once per run"));
	CHECK(has(cli_catalog_expectation("input_provider"), "displayxr-cli input list"));
	CHECK(has(cli_catalog_expectation("conversion_provider"), "displayxr-cli conv list"));
	CHECK(cli_catalog_expectation("runtime") != nullptr);
	CHECK(cli_catalog_expectation("app") == nullptr);
	CHECK(cli_catalog_expectation("sdk") == nullptr);
	CHECK(cli_catalog_expectation(nullptr) == nullptr);
}

TEST_CASE("catalog: the log channel keeps every result line, and is a token", "[cli][catalog]")
{
	// The reading end, as the CLI opens it: a random token, never the same twice.
	char a[64], b[64], tiny[8];
	REQUIRE(cli_catalog_os_log_open(nullptr, a, sizeof(a)));
	CHECK(dxr_log_token_ok(a));
	// What a writer sends arrives, through the engine's own writer code.
	dxr_log_channel_t ch = dxr_log_channel_open(a);
	REQUIRE(ch != DXR_LOG_CHANNEL_NONE);
	const std::string msg = "hello\nresult: x: 0 installed\n";
	CHECK(dxr_log_channel_write(ch, msg.c_str(), msg.size()));
	dxr_log_channel_close(ch);
	char *text = cli_catalog_os_log_take(nullptr, a);
	REQUIRE(text != nullptr);
	CHECK(std::string(text) == msg);
	free(text);
	// Taken: the channel is gone, and its token opens nothing.
	CHECK(dxr_log_channel_open(a) == DXR_LOG_CHANNEL_NONE);
	CHECK(cli_catalog_os_log_take(nullptr, a) == nullptr);

	REQUIRE(cli_catalog_os_log_open(nullptr, b, sizeof(b)));
	CHECK(std::string(a) != std::string(b));
	CHECK(cli_catalog_os_log_take(nullptr, "not-the-token") == nullptr);
	CHECK(cli_catalog_os_log_take(nullptr, b) == nullptr); // nothing was sent
	CHECK_FALSE(cli_catalog_os_log_open(nullptr, tiny, sizeof(tiny)));

	// A writer takes a token only: nothing that could name a file.
	CHECK_FALSE(dxr_log_token_ok(nullptr));
	CHECK_FALSE(dxr_log_token_ok(""));
	CHECK_FALSE(dxr_log_token_ok("C:\\Windows\\win.ini"));
	CHECK_FALSE(dxr_log_token_ok("../../../../etc/passwd"));
	CHECK_FALSE(dxr_log_token_ok("0123456789abcdef0123456789abcde"));   // 31
	CHECK_FALSE(dxr_log_token_ok("0123456789abcdef0123456789abcdef0")); // 33
	CHECK_FALSE(dxr_log_token_ok("0123456789ABCDEF0123456789abcdef"));  // lower case only
	CHECK_FALSE(dxr_log_token_ok("0123456789abcdef0123456789abcd/f"));
	CHECK_FALSE(dxr_log_token_ok("0123456789abcdef0123456789abcd\\f"));
	CHECK(dxr_log_token_ok("0123456789abcdef0123456789abcdef"));
	CHECK(dxr_log_channel_open("C:\\Windows\\win.ini") == DXR_LOG_CHANNEL_NONE);
	CHECK(dxr_log_channel_open("0123456789abcdef0123456789abcdef") == DXR_LOG_CHANNEL_NONE); // nobody listens
}

#if defined(_WIN32) && defined(REAL_DXR_GET)
/*
 * The real engine, where only it can answer. It is run with the rights of
 * this test, against the registry of this machine and a local file: no
 * network, and nothing is installed.
 */
namespace {

struct Real
{
	int exit = -1;
	std::string out;
	std::string log; //!< what it sent through the log pipe
};

Real
run_real(std::vector<std::string> args, bool with_pipe = true)
{
	Real res;
	char token[64] = {0};
	if (with_pipe) {
		REQUIRE(cli_catalog_os_log_open(nullptr, token, sizeof(token)));
		args.insert(args.begin(), {"--log-pipe", token});
	}
	std::vector<const char *> argv;
	for (const std::string &a : args) {
		argv.push_back(a.c_str());
	}
	struct cli_catalog_run r = {};
	r.exe = REAL_DXR_GET;
	r.args = argv.data();
	r.n_args = static_cast<int>(argv.size());
	r.timeout_ms = 120000;
	r.capture_stdout = true;
	r.kill_on_timeout = true;
	const enum cli_catalog_run_status st = cli_catalog_os_run(nullptr, &r);
	REQUIRE(st == CLI_CATALOG_RUN_OK);
	res.exit = r.exit_code;
	res.out = r.out != nullptr ? r.out : "";
	free(r.out);
	if (with_pipe) {
		char *text = cli_catalog_os_log_take(nullptr, token);
		res.log = text != nullptr ? text : "";
		free(text);
	}
	return res;
}

std::string
utf8(const std::filesystem::path &p)
{
	const auto u = p.u8string();
	return std::string(u.begin(), u.end());
}

} // namespace

TEST_CASE("dxr-get: takes no path to log to, and reports through the caller's pipe", "[cli][catalog][engine]")
{
	const std::filesystem::path victim =
	    std::filesystem::temp_directory_path() / ("dxr_get_victim_" + std::to_string(test_pid()) + ".txt");
	{
		std::ofstream f(victim, std::ios::binary);
		f << "untouched";
	}
	// `--log <file>` does not exist: a usage error, and the file is as it was.
	Real a = run_real({"--log", utf8(victim), "list", "--installed"}, false);
	CHECK(a.exit == 2);
	// `--log-pipe` takes a token and nothing else.
	Real b = run_real({"--log-pipe", utf8(victim), "list", "--installed"}, false);
	CHECK(b.exit == 2);
	Real c = run_real({"--log-pipe", "..\\..\\..\\x", "list", "--installed"}, false);
	CHECK(c.exit == 2);
	CHECK(slurp(victim) == "untouched");
	std::filesystem::remove(victim);

	// A well-formed token nobody listens on: the engine carries on without the channel.
	Real d = run_real({"--log-pipe", "0123456789abcdef0123456789abcdef", "list", "--installed", "--json"}, false);
	CHECK(d.exit == 0);
	CHECK(has(d.out, "\"installed\""));

	// The channel itself, end to end: this process's pipe, the engine's writer.
	Real e = run_real({"--catalog", "C:\\no\\such\\dxr-catalog.json", "list", "--json"});
	CHECK(e.exit == 10);
	CHECK(has(e.log, "cannot read catalog file"));
	CHECK(has(e.log, "OFFLINE"));
}

TEST_CASE("dxr-get: an argument with a control character is refused", "[cli][catalog][engine]")
{
	Real r = run_real({"uninstall", "ghost\nresult: ghost: 0 uninstalled", "other"});
	CHECK(r.exit == 2);                  // refused before anything is logged to the channel or done
	CHECK_FALSE(has(r.log, "result: ")); // the forged line was never written
	Real esc = run_real({"uninstall", "gh\x1b[2Jost"});
	CHECK(esc.exit == 2);
}

TEST_CASE("dxr-get: the build that ships ignores the switches that weaken verification", "[cli][catalog][engine]")
{
	const std::filesystem::path cat =
	    std::filesystem::temp_directory_path() / ("dxr_get_unsigned_" + std::to_string(test_pid()) + ".json");
	{
		std::ofstream f(cat, std::ios::binary);
		f << "{\"schema\":2,\"serial\":1,\"items\":[]}";
	}
	set_env("DXR_GET_DEV", "1");
	set_env("DXR_GET_ALLOW_UNSIGNED", "1");
	// The argument is a usage error here: it exists in a test-root-key build only.
	Real nv = run_real({"--catalog", utf8(cat), "--no-verify-catalog", "list", "--json"});
	CHECK(nv.exit == 2);
	CHECK(has(nv.log, "test-root-key build"));
	// And the environment alone changes nothing: the unsigned catalog is not read.
	Real env = run_real({"--catalog", utf8(cat), "list", "--json"});
	CHECK(env.exit == 10);
	CHECK(has(env.log, "cannot read catalog signature"));
	CHECK(has(env.log, "ignored"));
	CHECK_FALSE(has(env.log, "NOT verified"));
	CHECK_FALSE(has(env.out, "\"items\""));
	set_env("DXR_GET_DEV", nullptr);
	set_env("DXR_GET_ALLOW_UNSIGNED", nullptr);
	std::filesystem::remove(cat);
}

TEST_CASE("dxr-get: a --catalog path outside the ANSI code page is read", "[cli][catalog][engine]")
{
	// "café-日本" in the directory name: no single ANSI code page holds both.
	const std::filesystem::path dir =
	    std::filesystem::temp_directory_path() / (L"dxr-get-caf\u00e9-\u65e5\u672c-" + std::to_wstring(test_pid()));
	std::filesystem::create_directories(dir);
	const std::filesystem::path cat = dir / "cat.json";
	{
		std::ofstream f(cat, std::ios::binary);
		f << "{}";
	}
	Real r = run_real({"--catalog", utf8(cat), "list", "--json"});
	// The FILE was opened (it then wants the signature beside it, which is absent).
	CHECK(r.exit == 10);
	CHECK(has(r.log, "cannot read catalog signature"));
	CHECK_FALSE(has(r.log, "cannot read catalog file"));
	CHECK(has(r.log, utf8(dir).c_str())); // and the path came through as UTF-8, unchanged
	std::filesystem::remove_all(dir);
}
#endif
