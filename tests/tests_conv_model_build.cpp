// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Locally built models (conversion-provider-discovery.md §5.6,
 *         ADR-057 amendment 2026-11-27): the store runs a signed builder on
 *         the user's explicit request, and nothing else ever builds.
 *
 * What must hold:
 *  - the pack schema: `local_build.tool` + `sources` + `needs` parse; a
 *    half-described build (no sources, not deterministic, a `url`, a licence
 *    text that is a path) refuses the pack;
 *  - the sequence fails closed at each step, in order, before anything is
 *    written to the store: policy / policy-locked, the licence text hash,
 *    the acceptance record (and --accept-license writes it), builder missing
 *    / too old / untrusted / `describe --json` disagreeing, disk / memory /
 *    OS, busy;
 *  - the run: sources come from the store's downloader (size + sha256), the
 *    builder runs with argv, a minimal environment and --work as its working
 *    directory, its JSON lines are forwarded (a non-JSON line is dropped),
 *    every exit code maps to its result, the no-output watchdog kills a
 *    silent builder, the cancel file stops a running one (and one that
 *    ignores it is killed after the grace period), an interrupt cancels;
 *  - ingest by hash only: the right bytes become stamped blobs, wrong bytes
 *    never do; staging and the lock are gone afterwards; stale staging is
 *    cleaned;
 *  - hand-out: a built model is READY only for a user with the acceptance
 *    record; withdrawing it reads ABSENT(policy);
 *  - model_request never builds (ABSENT, MODEL; no staging is created);
 *  - remove deletes only blobs no other model references;
 *  - the machine half (target_conv_build.c): the acceptance record round
 *    trip, the stricter provenance rule.
 */

#include "catch_amalgamated.hpp"

#include "conv_fake_model_builder_data.h"
#include "conversion/conv_model_build.h"
#include "conversion/conv_model_store.h"
#include "target_component_loader.h"
#include "target_conv_build.h"
#include "util/u_sha256.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define getpid_portable() ((long)_getpid())
#else
#include <unistd.h>
#define getpid_portable() ((long)getpid())
#endif

namespace fs = std::filesystem;

namespace {

std::string
sha_hex(const std::string &b)
{
	struct u_sha256 h;
	u_sha256_init(&h);
	u_sha256_update(&h, b.data(), b.size());
	uint8_t d[32];
	u_sha256_final(&h, d);
	char hex[65];
	for (int i = 0; i < 32; i++) {
		snprintf(hex + i * 2, 3, "%02x", d[i]);
	}
	return hex;
}

std::string
source_bytes()
{
	char buf[8192];
	return std::string(buf, fake_builder_source(buf, sizeof(buf)));
}

std::string
output_bytes(const char *variant)
{
	char buf[8192];
	return std::string(buf, fake_builder_output(variant, buf, sizeof(buf)));
}

void
write_file(const fs::path &p, const std::string &b)
{
	fs::create_directories(p.parent_path());
	std::ofstream f(p, std::ios::binary);
	f.write(b.data(), (std::streamsize)b.size());
}

std::string
file_url(const fs::path &p)
{
	std::string s = p.generic_string();
	return s[0] == '/' ? "file://" + s : "file:///" + s;
}

const char *kLicenceText = "Test licence: research use only.\n";

struct model_spec
{
	std::string name;
	std::string variant;
	bool commercial = false;
};

/*!
 * One test's world: a store root, a pack dir with the manifest, the licence
 * text and the source file, and the fake ops.
 */
struct world
{
	fs::path dir;
	fs::path root;
	fs::path pack_dir;
	fs::path manifest;
	conv_model_store *s = nullptr;
	std::unique_ptr<conv_pack> pack{new conv_pack()};

	// Fake machine state behind the ops.
	bool allow_research = true;
	bool locked = false;
	std::map<std::string, std::string> licences; //!< id -> accepted text sha
	bool record_fails = false;
	bool have_tool = true;
	std::string tool_version = FAKE_BUILDER_VERSION;
	bool trusted = true;
	conv_build_machine machine{};
	std::mutex ev_mtx;
	std::vector<std::string> events;

	explicit world(const char *tag)
	{
		dir = fs::temp_directory_path() /
		      ("dxr-build-" + std::string(tag) + "-" + std::to_string(getpid_portable()) + "-" +
		       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000));
		fs::remove_all(dir);
		root = dir / "store";
		pack_dir = dir / "pack";
		fs::create_directories(pack_dir);
		write_file(pack_dir / "test.licence.txt", kLicenceText);
		write_file(pack_dir / "src" / "checkpoint.bin", source_bytes());
		machine.phys_known = machine.free_known = true;
		machine.phys_mb = 1u << 20;
		machine.free_mb = 1u << 20;
		snprintf(machine.os, sizeof(machine.os), "%s",
#ifdef _WIN32
		         "windows"
#elif defined(__APPLE__)
		         "macos"
#else
		         "linux"
#endif
		);
		snprintf(machine.os_version, sizeof(machine.os_version), "10.0.19045");
		conv_model_store_config cfg;
		conv_model_store_config_defaults(&cfg);
		snprintf(cfg.root, sizeof(cfg.root), "%s", root.string().c_str());
		cfg.allow_research_only = true;
		cfg.licence_accepted = &world::licence_accepted_cb;
		cfg.licence_user = this;
		REQUIRE(conv_model_store_create(&cfg, &s));
	}

	~world()
	{
		conv_model_store_destroy(&s);
		std::error_code ec;
		fs::remove_all(dir, ec);
	}

	//! Write and load a pack with @p models built from the fake recipe.
	void
	make_pack(const std::vector<model_spec> &models,
	          const std::string &extra_lb = "",
	          const std::string &source_sha = "",
	          const std::string &text_sha = "",
	          const std::string &min_version = "1.0.0",
	          const std::string &source_url = "")
	{
		const std::string src = source_bytes();
		std::string j = "{ \"schema\": 2, \"pack\": \"dxr-test-build\", \"provider\": \"test\", \"models\": [";
		for (size_t i = 0; i < models.size(); i++) {
			const std::string out = output_bytes(models[i].variant.c_str());
			j +=
			    std::string(i > 0 ? "," : "") + "{ \"name\": \"" + models[i].name +
			    "\", \"version\": \"1.0.0\", \"task\": \"splats-mono\", \"license\": { \"spdx\": "
			    "\"LicenseRef-test\", \"commercial\": " +
			    (models[i].commercial ? "true" : "false") +
			    " }, \"local_build\": { \"script\": \"scripts/export.py\", \"deterministic\": true, "
			    "\"recipe\": "
			    "\"" FAKE_BUILDER_RECIPE "\", \"variant\": \"" +
			    models[i].variant +
			    "\", \"tool\": { \"id\": \"" FAKE_BUILDER_TOOL_ID
			    "\", \"catalog_id\": \"open-conversion-model-builder\", \"min_version\": \"" +
			    min_version +
			    "\", \"signer\": \"first-party\" }, \"sources\": [{ \"role\": \"checkpoint\", \"url\": \"" +
			    (source_url.empty() ? file_url(pack_dir / "src" / "checkpoint.bin") : source_url) +
			    "\", \"revision\": \"r1\", \"sha256\": \"" +
			    (source_sha.empty() ? sha_hex(src) : source_sha) +
			    "\", \"size\": " + std::to_string(src.size()) +
			    ", \"license\": { \"id\": \"" FAKE_BUILDER_LICENCE
			    "\", \"name\": \"Test licence\", \"link\": \"https://example.invalid/LICENSE\", \"text\": "
			    "\"test.licence.txt\", \"text_sha256\": \"" +
			    (text_sha.empty() ? sha_hex(kLicenceText) : text_sha) +
			    "\" } }], \"needs\": { \"ram_mb\": 64, \"disk_mb\": 1, \"os\": [\"windows>=10.0.19041\", "
			    "\"linux\", \"macos\"] }" +
			    extra_lb +
			    " }, \"variants\": [{ \"id\": \"v\", \"files\": [{ \"role\": \"model\", \"format\": "
			    "\"bin\", "
			    "\"sha256\": \"" +
			    sha_hex(out) + "\", \"size\": " + std::to_string(out.size()) + " }] }] }";
		}
		j += "] }";
		manifest = pack_dir / "dxr-test-build.models.json";
		write_file(manifest, j);
		char err[256];
		INFO(j);
		REQUIRE(conv_pack_load(manifest.string().c_str(), pack.get(), err, sizeof(err)));
		REQUIRE(conv_model_store_add_pack(s, pack.get()));
	}

	static bool
	licence_accepted_cb(void *u, const char *id, const char *sha)
	{
		world *w = static_cast<world *>(u);
		auto it = w->licences.find(id);
		return it != w->licences.end() && it->second == sha;
	}

	static bool
	licence_record_cb(void *u, const char *id, const char *sha)
	{
		world *w = static_cast<world *>(u);
		if (w->record_fails) {
			return false;
		}
		w->licences[id] = sha;
		return true;
	}

	static bool
	find_tool_cb(void *u, const char *id, conv_build_tool *out)
	{
		world *w = static_cast<world *>(u);
		memset(out, 0, sizeof(*out));
		if (!w->have_tool || strcmp(id, FAKE_BUILDER_TOOL_ID) != 0) {
			return false;
		}
		snprintf(out->id, sizeof(out->id), "%s", id);
		snprintf(out->binary, sizeof(out->binary), "%s", FAKE_MODEL_BUILDER);
		snprintf(out->version, sizeof(out->version), "%s", w->tool_version.c_str());
		snprintf(out->source, sizeof(out->source), "test");
		return true;
	}

	static bool
	trusted_cb(void *u, const conv_build_tool *, conv_build_trust *out)
	{
		world *w = static_cast<world *>(u);
		memset(out, 0, sizeof(*out));
		snprintf(out->provenance, sizeof(out->provenance), "%s", w->trusted ? "allowlisted" : "unverified");
		snprintf(out->reason, sizeof(out->reason), "%s", w->trusted ? "" : "test says no");
		return w->trusted;
	}

	static bool
	machine_cb(void *u, const char *, conv_build_machine *out)
	{
		*out = static_cast<world *>(u)->machine;
		return true;
	}

	static void
	event_cb(void *u, const char *line)
	{
		world *w = static_cast<world *>(u);
		std::lock_guard<std::mutex> l(w->ev_mtx);
		w->events.emplace_back(line);
	}

	conv_build_ops
	ops()
	{
		conv_build_ops o{};
		o.user = this;
		o.allow_research_only = allow_research;
		o.research_locked_off = locked;
		o.licence_accepted = licence_accepted_cb;
		o.licence_record = licence_record_cb;
		o.find_tool = find_tool_cb;
		o.tool_trusted = trusted_cb;
		o.machine = machine_cb;
		o.on_event = event_cb;
		return o;
	}

	conv_build_options
	options()
	{
		conv_build_options o;
		conv_build_options_defaults(&o);
		snprintf(o.log_dir, sizeof(o.log_dir), "%s", (dir / "logs").string().c_str());
		o.lock_poll_ms = 50;
		return o;
	}

	std::vector<conv_build_model_result>
	build(const std::vector<std::string> &names, const conv_build_options *opt = nullptr)
	{
		conv_build_ops o = ops();
		conv_build_options def = options();
		std::vector<const char *> c;
		for (const std::string &n : names) {
			c.push_back(n.c_str());
		}
		std::vector<conv_build_model_result> r(names.size());
		(void)conv_model_store_build(s, pack.get(), c.data(), (uint32_t)c.size(), &o,
		                             opt != nullptr ? opt : &def, r.data());
		return r;
	}

	bool
	blob_stamped(const char *variant)
	{
		const std::string sha = sha_hex(output_bytes(variant));
		return fs::exists(root / "blobs" / sha) && fs::exists(root / "blobs" / (sha + ".ok"));
	}

	bool
	staging_empty()
	{
		const fs::path st = root / "staging";
		if (!fs::exists(st)) {
			return true;
		}
		for (const auto &e : fs::directory_iterator(st)) {
			(void)e;
			return false;
		}
		return true;
	}

	size_t
	count_events(const char *needle)
	{
		std::lock_guard<std::mutex> l(ev_mtx);
		size_t n = 0;
		for (const std::string &e : events) {
			n += e.find(needle) != std::string::npos ? 1 : 0;
		}
		return n;
	}
};

const std::vector<model_spec> kOne = {{"m-a", "a"}};

} // namespace


TEST_CASE("conv_build: pack schema for a buildable model", "[conv_build]")
{
	world w("schema");
	w.make_pack({{"m-a", "a"}, {"m-b", "b"}});
	REQUIRE(w.pack->build_count == 2);
	const conv_pack_model *m = conv_pack_find_model(w.pack.get(), "m-b");
	REQUIRE(m != nullptr);
	const conv_pack_build *b = conv_pack_model_build(w.pack.get(), m);
	REQUIRE(b != nullptr);
	CHECK(std::string(b->recipe) == FAKE_BUILDER_RECIPE);
	CHECK(std::string(b->variant) == "b");
	CHECK(std::string(b->tool_id) == FAKE_BUILDER_TOOL_ID);
	CHECK(std::string(b->tool_catalog_id) == "open-conversion-model-builder");
	CHECK(b->source_count == 1);
	CHECK(std::string(b->sources[0].license.id) == FAKE_BUILDER_LICENCE);
	CHECK(b->needs_ram_mb == 64);
	CHECK(b->os_count == 3);
	char err[256];
	CHECK(conv_pack_license_text_verify(w.pack.get(), &b->sources[0].license, err, sizeof(err)));

	auto refused = [&](const std::string &lb_tail, const std::string &model_extra = "",
	                   const std::string &det = "true") {
		const std::string j =
		    "{ \"schema\": 2, \"pack\": \"x\", \"models\": [{ \"name\": \"m\", " + model_extra +
		    "\"local_build\": { \"deterministic\": " + det +
		    ", \"recipe\": \"r\", \"variant\": \"v\", \"tool\": { \"id\": "
		    "\"t\", \"min_version\": \"1.0\" }" +
		    lb_tail +
		    " }, \"variants\": [{ \"files\": [{ \"sha256\": "
		    "\"0000000000000000000000000000000000000000000000000000000000000000\", \"size\": 1 }] }] }] }";
		std::unique_ptr<conv_pack> p(new conv_pack());
		char e[256];
		return !conv_pack_parse(j.c_str(), "/tmp", p.get(), e, sizeof(e));
	};
	const std::string good_src =
	    ", \"sources\": [{ \"role\": \"checkpoint\", \"url\": \"https://example.invalid/c.pt\", \"sha256\": "
	    "\"1111111111111111111111111111111111111111111111111111111111111111\", \"size\": 5, \"license\": { "
	    "\"id\": \"l\", \"text\": \"l.txt\", \"text_sha256\": "
	    "\"2222222222222222222222222222222222222222222222222222222222222222\" } }]";
	CHECK_FALSE(refused(good_src));
	CHECK(refused(""));                                                       // tool without sources
	CHECK(refused(good_src, "\"url\": \"https://example.invalid/m.bin\", ")); // a download path
	std::string bad_text = good_src;
	bad_text.replace(bad_text.find("l.txt"), 5, "../l.txt");
	CHECK(refused(bad_text)); // the licence text is a path out of the pack dir
	std::string no_licence = good_src.substr(0, good_src.find(", \"license\"")) + " }]";
	CHECK(refused(no_licence));
	CHECK(refused(good_src, "", "false")); // a buildable model must be deterministic
}

TEST_CASE("conv_build: pure helpers", "[conv_build]")
{
	CHECK(conv_build_version_cmp("1.10.0", "1.9") > 0);
	CHECK(conv_build_version_cmp("1.0", "1.0.0") == 0);
	CHECK(conv_build_version_cmp("0.9.9", "1.0.0") < 0);
	CHECK(conv_build_version_cmp("1.2.0-rc1", "1.2.0") == 0);
	CHECK(conv_build_os_matches("windows>=10.0.19041", "windows", "10.0.19045"));
	CHECK_FALSE(conv_build_os_matches("windows>=10.0.19041", "windows", "10.0.17763"));
	CHECK(conv_build_os_matches("linux", "linux", "6.8.0"));
	CHECK_FALSE(conv_build_os_matches("linux", "macos", "15.0"));
	for (int c : {0, 10, 11, 12, 13, 14, 15, 16, 17, 1, 2, 99}) {
		const conv_build_result r = conv_build_result_of_exit(c);
		const char *want = c == 0    ? "built"
		                   : c == 10 ? "build-license"
		                   : c == 11 ? "build-download"
		                   : c == 12 ? "build-source-mismatch"
		                   : c == 13 ? "build-output-mismatch"
		                   : c == 14 ? "build-memory"
		                   : c == 15 ? "build-disk"
		                   : c == 16 ? "build-unsupported"
		                   : c == 17 ? "build-cancelled"
		                             : "build-failed";
		CHECK(std::string(conv_build_result_str(r)) == want);
	}
	char env[96];
	REQUIRE(conv_build_dev_env_name("open-model-builder", env, sizeof(env)));
	CHECK(std::string(env) == "DXR_MODEL_BUILDER_OPEN_MODEL_BUILDER");
	conv_build_tool t;
	CHECK(conv_build_tool_manifest_parse(
	    "{\"id\":\"open-model-builder\",\"version\":\"1.0.0\",\"binary\":\"/usr/lib/displayxr/dxr-model-builder\"}",
	    "open-model-builder", &t));
	CHECK(std::string(t.binary) == "/usr/lib/displayxr/dxr-model-builder");
	CHECK_FALSE(
	    conv_build_tool_manifest_parse("{\"id\":\"open-model-builder\",\"version\":\"1.0.0\",\"binary\":"
	                                   "\"rel/dxr-model-builder\"}",
	                                   "open-model-builder", &t));
	CHECK_FALSE(conv_build_tool_manifest_parse("{\"id\":\"other\",\"version\":\"1.0.0\",\"binary\":\"/x\"}",
	                                           "open-model-builder", &t));
}

TEST_CASE("conv_build: describe --json must agree with the pack", "[conv_build]")
{
	world w("describe");
	w.make_pack(kOne);
	const conv_pack_build *b = &w.pack->builds[0];
	const std::string src = source_bytes();
	auto doc = [&](const std::string &tool, const std::string &ver, const std::string &sha,
	               const std::string &lic) {
		return "{\"v\":1,\"tool\":\"" + tool + "\",\"version\":\"" + ver +
		       "\",\"recipes\":[{\"recipe\":\"fake\",\"variants\":[\"a\"],\"sources\":[{\"role\":"
		       "\"checkpoint\","
		       "\"sha256\":\"" +
		       sha + "\",\"size\":" + std::to_string(src.size()) + ",\"license_id\":\"" + lic + "\"}]}]}";
	};
	const char *vars[] = {"a"};
	char why[256];
	CHECK(conv_build_describe_check(doc(FAKE_BUILDER_TOOL_ID, "1.2.0", sha_hex(src), FAKE_BUILDER_LICENCE).c_str(),
	                                b, vars, 1, why, sizeof(why)));
	CHECK_FALSE(conv_build_describe_check(doc("other", "1.2.0", sha_hex(src), FAKE_BUILDER_LICENCE).c_str(), b,
	                                      vars, 1, why, sizeof(why)));
	CHECK_FALSE(
	    conv_build_describe_check(doc(FAKE_BUILDER_TOOL_ID, "0.1.0", sha_hex(src), FAKE_BUILDER_LICENCE).c_str(), b,
	                              vars, 1, why, sizeof(why)));
	CHECK_FALSE(
	    conv_build_describe_check(doc(FAKE_BUILDER_TOOL_ID, "1.2.0", sha_hex("x"), FAKE_BUILDER_LICENCE).c_str(), b,
	                              vars, 1, why, sizeof(why)));
	CHECK_FALSE(conv_build_describe_check(doc(FAKE_BUILDER_TOOL_ID, "1.2.0", sha_hex(src), "other").c_str(), b,
	                                      vars, 1, why, sizeof(why)));
	const char *missing[] = {"zz"};
	CHECK_FALSE(
	    conv_build_describe_check(doc(FAKE_BUILDER_TOOL_ID, "1.2.0", sha_hex(src), FAKE_BUILDER_LICENCE).c_str(), b,
	                              missing, 1, why, sizeof(why)));
}

TEST_CASE("conv_build: each gate fails closed, in order, and writes nothing", "[conv_build]")
{
	world w("gates");
	w.make_pack(kOne);
	w.licences[FAKE_BUILDER_LICENCE] = sha_hex(kLicenceText);
	auto one = [&]() { return std::string(conv_build_result_str(w.build({"m-a"})[0].result)); };
	auto nothing_written = [&]() { return !w.blob_stamped("a") && w.staging_empty(); };

	SECTION("policy")
	{
		w.allow_research = false;
		CHECK(one() == "policy");
		w.locked = true;
		CHECK(one() == "policy-locked");
	}
	SECTION("licence text tampered")
	{
		write_file(w.pack_dir / "test.licence.txt", "another text\n");
		CHECK(one() == "build-license-text");
	}
	SECTION("no acceptance record, then --accept-license records it")
	{
		w.licences.clear();
		CHECK(one() == "build-license");
		CHECK(w.licences.empty());
		w.record_fails = true;
		conv_build_options o = w.options();
		const char *acc[] = {FAKE_BUILDER_LICENCE};
		o.accept_licences = acc;
		o.accept_count = 1;
		CHECK(std::string(conv_build_result_str(w.build({"m-a"}, &o)[0].result)) == "io");
		w.record_fails = false;
		w.have_tool = false; // stop after step 2 so only the record is observed
		CHECK(std::string(conv_build_result_str(w.build({"m-a"}, &o)[0].result)) == "build-tool-missing");
		CHECK(w.licences[FAKE_BUILDER_LICENCE] == sha_hex(kLicenceText));
	}
	SECTION("an old acceptance (another text hash) is not this text's")
	{
		w.licences[FAKE_BUILDER_LICENCE] = sha_hex("an older text");
		CHECK(one() == "build-license");
	}
	SECTION("builder missing, too old, untrusted")
	{
		w.have_tool = false;
		const conv_build_model_result r = w.build({"m-a"})[0];
		CHECK(std::string(conv_build_result_str(r.result)) == "build-tool-missing");
		CHECK(std::string(r.hint).find("open-conversion-model-builder") != std::string::npos);
		w.have_tool = true;
		w.tool_version = "0.9.0";
		CHECK(one() == "build-tool-old");
		w.tool_version = FAKE_BUILDER_VERSION;
		w.trusted = false;
		CHECK(one() == "build-tool-untrusted");
	}
	SECTION("describe disagrees: the pack needs a newer builder than the binary says it is")
	{
		w.tool_version = ""; // the dev override: no registered version, describe decides
		w.make_pack(kOne, "", "", "", "9.0.0");
		CHECK(one() == "build-tool-mismatch");
	}
	SECTION("disk, memory, OS")
	{
		w.machine.free_mb = 0;
		CHECK(one() == "build-disk");
		w.machine.free_mb = 1u << 20;
		w.machine.phys_mb = 1;
		CHECK(one() == "build-memory");
		w.machine.phys_mb = 1u << 20;
		snprintf(w.machine.os, sizeof(w.machine.os), "plan9");
		CHECK(one() == "build-unsupported");
	}
	SECTION("busy: a live process holds the recipe's lock")
	{
		fs::create_directories(w.root / "staging");
		write_file(w.root / "staging" / "fake.lock",
		           std::to_string(getpid_portable()) + " other-nonce dxr-test-build/m-a\n");
		conv_build_options o = w.options();
		o.no_wait = true;
		CHECK(std::string(conv_build_result_str(w.build({"m-a"}, &o)[0].result)) == "busy");
		fs::remove(w.root / "staging" / "fake.lock");
	}
	CHECK(nothing_written());
}

TEST_CASE("conv_build: a build downloads, runs, ingests by hash and cleans up", "[conv_build]")
{
	world w("ok");
	w.make_pack({{"m-a", "a"}, {"m-b", "b"}, {"m-noise", "noise"}, {"m-env", "envcheck"}});
	conv_build_options o = w.options();
	const char *acc[] = {FAKE_BUILDER_LICENCE};
	o.accept_licences = acc;
	o.accept_count = 1;
	const auto r = w.build({"m-a", "m-b", "m-noise", "m-env"}, &o);
	for (const auto &x : r) {
		INFO(x.name << ": " << conv_build_result_str(x.result) << " " << x.hint);
		CHECK(x.result == CONV_BUILD_BUILT);
		CHECK(x.ran);
		CHECK(x.exit_code == 0);
		CHECK(std::string(x.provenance) == "allowlisted");
	}
	CHECK(w.blob_stamped("a"));
	CHECK(w.blob_stamped("b"));
	CHECK(w.blob_stamped("noise"));
	CHECK(w.blob_stamped("envcheck"));
	CHECK(w.staging_empty()); // staging dir and the lock are gone
	CHECK(fs::exists(w.root / "packs" / "dxr-test-build.models.json"));
	// One run served every model of the recipe (one source download, one start).
	CHECK(w.count_events("\"event\":\"start\"") == 1);
	CHECK(w.count_events("\"phase\":\"download-source\"") >= 1);
	CHECK(w.count_events("\"phase\":\"ingest\"") == 1);
	CHECK(w.count_events("\"event\":\"done\"") == 1);
	CHECK(w.count_events("not an event") == 0); // a non-JSON stdout line is dropped

	// Hand-out: READY for the user with the record; ABSENT(policy) once withdrawn.
	xrt_conv_model_location loc{};
	loc.struct_size = sizeof(loc);
	REQUIRE(conv_model_store_resolve(w.s, "dxr-test-build/m-a", &loc));
	CHECK(loc.state == XRT_CONV_STATE_READY);
	w.licences.clear();
	REQUIRE(conv_model_store_resolve(w.s, "dxr-test-build/m-a", &loc));
	CHECK(loc.state == XRT_CONV_STATE_ABSENT);
	CHECK(loc.failure_reason == XRT_CONV_FAIL_POLICY);

	// A second fetch finds it present: no build.
	w.licences[FAKE_BUILDER_LICENCE] = sha_hex(kLicenceText);
	w.events.clear();
	CHECK(w.build({"m-a"})[0].result == CONV_BUILD_PRESENT);
	CHECK(w.count_events("\"event\":\"start\"") == 0);

	// remove: m-a's blob goes; m-b's stays.
	uint64_t bytes = 0;
	CHECK(conv_model_store_remove_model(w.s, "dxr-test-build", "m-a", &bytes) == 1);
	CHECK(bytes == output_bytes("a").size());
	CHECK_FALSE(w.blob_stamped("a"));
	CHECK(w.blob_stamped("b"));
	REQUIRE(conv_model_store_resolve(w.s, "dxr-test-build/m-a", &loc));
	CHECK(loc.state == XRT_CONV_STATE_ABSENT);
}

TEST_CASE("conv_build: wrong output and every builder exit code", "[conv_build]")
{
	world w("exits");
	w.licences[FAKE_BUILDER_LICENCE] = sha_hex(kLicenceText);
	{
		w.make_pack({{"m-wrong", "wrong"}});
		const auto r = w.build({"m-wrong"})[0];
		CHECK(std::string(conv_build_result_str(r.result)) == "build-output-mismatch");
		CHECK(r.exit_code == 0);
		CHECK_FALSE(w.blob_stamped("wrong"));
		CHECK(w.staging_empty());
	}
	for (int c : {1, 10, 11, 12, 13, 14, 15, 16, 17}) {
		const std::string name = "m-exit-" + std::to_string(c);
		w.make_pack({{name, "exit-" + std::to_string(c)}});
		const auto r = w.build({name})[0];
		INFO(name << " -> " << conv_build_result_str(r.result) << " " << r.hint);
		CHECK(r.result == conv_build_result_of_exit(c));
		CHECK(r.exit_code == c);
		CHECK(std::string(r.hint).find("scripted") != std::string::npos); // the builder's `error` line
		CHECK(w.staging_empty());
	}
}

TEST_CASE("conv_build: sources are verified by the store's downloader", "[conv_build]")
{
	world w("sources");
	w.licences[FAKE_BUILDER_LICENCE] = sha_hex(kLicenceText);
	SECTION("the server sends other bytes than the pinned ones")
	{
		w.make_pack(kOne);
		write_file(w.pack_dir / "src" / "checkpoint.bin", "not the checkpoint");
		CHECK(std::string(conv_build_result_str(w.build({"m-a"})[0].result)) == "bad-download");
	}
	SECTION("the pack pins a source the builder does not (describe disagrees, before any download)")
	{
		w.make_pack(kOne, "", sha_hex("not the checkpoint"));
		CHECK(std::string(conv_build_result_str(w.build({"m-a"})[0].result)) == "build-tool-mismatch");
	}
	SECTION("unreachable")
	{
		w.make_pack(kOne, "", "", "", "1.0.0", file_url(w.pack_dir / "src" / "missing.bin"));
		CHECK(std::string(conv_build_result_str(w.build({"m-a"})[0].result)) == "build-download");
	}
	CHECK_FALSE(w.blob_stamped("a"));
	CHECK(w.staging_empty());
}

TEST_CASE("conv_build: watchdog, cancel file, grace kill, interrupt", "[conv_build]")
{
	world w("cancel");
	w.make_pack({{"m-silent", "silent"}, {"m-cancel", "cancel"}, {"m-stubborn", "stubborn"}});
	w.licences[FAKE_BUILDER_LICENCE] = sha_hex(kLicenceText);

	SECTION("a builder that prints nothing is killed")
	{
		conv_build_options o = w.options();
		o.stall_ms = 1500;
		const auto t0 = std::chrono::steady_clock::now();
		const auto r = w.build({"m-silent"}, &o)[0];
		CHECK(std::string(conv_build_result_str(r.result)) == "build-stalled");
		CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(30));
	}
	SECTION("conv models cancel: the builder sees the cancel file and exits 17")
	{
		std::vector<conv_build_model_result> r;
		std::thread t([&] { r = w.build({"m-cancel"}); });
		bool asked = false;
		for (int i = 0; i < 300 && !asked; i++) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			asked = w.count_events("\"phase\":\"convert\"") > 0 &&
			        conv_model_store_build_cancel(w.s, "dxr-test-build", "m-cancel");
		}
		t.join();
		REQUIRE(asked);
		CHECK(std::string(conv_build_result_str(r[0].result)) == "build-cancelled");
		CHECK(r[0].exit_code == 17);
		CHECK_FALSE(conv_model_store_build_cancel(w.s, "dxr-test-build", "m-cancel")); // nothing running now
	}
	SECTION("a builder that ignores the cancel file is killed after the grace period")
	{
		conv_build_options o = w.options();
		o.cancel_grace_ms = 1000;
		std::vector<conv_build_model_result> r;
		std::thread t([&] { r = w.build({"m-stubborn"}, &o); });
		bool asked = false;
		for (int i = 0; i < 300 && !asked; i++) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			asked = w.count_events("\"phase\":\"convert\"") > 0 &&
			        conv_model_store_build_cancel(w.s, "dxr-test-build", "m-stubborn");
		}
		t.join();
		REQUIRE(asked);
		CHECK(std::string(conv_build_result_str(r[0].result)) == "build-cancelled");
		CHECK(r[0].exit_code == -1);
	}
	SECTION("an interrupt (Ctrl+C in the CLI) cancels")
	{
		static volatile sig_atomic_t flag = 0;
		flag = 0;
		conv_build_options o = w.options();
		o.interrupt = &flag;
		std::vector<conv_build_model_result> r;
		std::thread t([&] { r = w.build({"m-cancel"}, &o); });
		for (int i = 0; i < 300 && w.count_events("\"phase\":\"convert\"") == 0; i++) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		flag = 1;
		t.join();
		CHECK(std::string(conv_build_result_str(r[0].result)) == "build-cancelled");
	}
	CHECK(w.staging_empty());
	CHECK_FALSE(w.blob_stamped("cancel"));
}

TEST_CASE("conv_build: stale staging is cleaned; a live lock's is not", "[conv_build]")
{
	world w("stale");
	w.make_pack(kOne);
	fs::create_directories(w.root / "staging" / "old-nonce" / "work");
	write_file(w.root / "staging" / "old-nonce" / "work" / "junk", "x");
	fs::create_directories(w.root / "staging" / "live-nonce");
	write_file(w.root / "staging" / "fake.lock",
	           std::to_string(getpid_portable()) + " live-nonce dxr-test-build/m-a\n");
	CHECK(conv_model_store_clean_staging(w.s, 0) == 1);
	CHECK_FALSE(fs::exists(w.root / "staging" / "old-nonce"));
	CHECK(fs::exists(w.root / "staging" / "live-nonce"));
	// A lock is not stale by age: a younger staging dir stays (max age 1 h).
	fs::create_directories(w.root / "staging" / "young");
	CHECK(conv_model_store_clean_staging(w.s, 3600) == 0);
	CHECK(fs::exists(w.root / "staging" / "young"));
}

TEST_CASE("conv_build: model_request and the hosts never build", "[conv_build]")
{
	world w("request");
	w.make_pack(kOne);
	w.licences[FAKE_BUILDER_LICENCE] = sha_hex(kLicenceText);
	CHECK_FALSE(conv_model_store_request(w.s, "dxr-test-build/m-a"));
	CHECK(conv_model_store_wait_idle(w.s, 5000));
	xrt_conv_model_location loc{};
	loc.struct_size = sizeof(loc);
	REQUIRE(conv_model_store_resolve(w.s, "dxr-test-build/m-a", &loc));
	CHECK(loc.state == XRT_CONV_STATE_ABSENT);
	CHECK(loc.failure_reason == XRT_CONV_FAIL_MODEL); // build-locally, not policy
	CHECK_FALSE(fs::exists(w.root / "staging"));
	CHECK_FALSE(w.blob_stamped("a"));
	// Without the acceptance record the request is refused by policy, still with no build.
	w.licences.clear();
	conv_model_store_set_licence_check(w.s, &world::licence_accepted_cb, &w);
	CHECK_FALSE(conv_model_store_request(w.s, "dxr-test-build/m-a"));
	REQUIRE(conv_model_store_resolve(w.s, "dxr-test-build/m-a", &loc));
	CHECK(loc.state == XRT_CONV_STATE_ABSENT);
	CHECK(loc.failure_reason == XRT_CONV_FAIL_POLICY);
	CHECK_FALSE(fs::exists(w.root / "staging"));
	// The installer's synchronous fetch path never builds either: it is a
	// download (nothing serves the bytes) and reports build-locally.
	CHECK(conv_model_store_fetch_model(w.s, w.pack.get(), &w.pack->models[0], nullptr, true) ==
	      CONV_STORE_BLOB_LOCAL_BUILD);
	CHECK_FALSE(fs::exists(w.root / "staging"));
}

TEST_CASE("conv_build: the acceptance record and the provenance rule (machine half)", "[conv_build]")
{
#ifndef _WIN32
	// The record lives in the per-user dir: point it at a temp HOME.
	const fs::path home = fs::temp_directory_path() / ("dxr-build-home-" + std::to_string(getpid_portable()));
	fs::create_directories(home);
	const char *old_home = getenv("HOME");
	const char *old_xdg = getenv("XDG_DATA_HOME");
	const std::string saved_home = old_home != nullptr ? old_home : "";
	const std::string saved_xdg = old_xdg != nullptr ? old_xdg : "";
	setenv("HOME", home.string().c_str(), 1);
	unsetenv("XDG_DATA_HOME");
#endif
	const std::string id = "dxr-test-licence-" + std::to_string(getpid_portable());
	const std::string sha = sha_hex("text");
	char got[80];
	uint64_t when = 0;
	(void)target_conv_licence_withdraw(id.c_str());
	CHECK_FALSE(target_conv_licence_read(id.c_str(), got, sizeof(got), &when));
	CHECK_FALSE(target_conv_licence_accepted(nullptr, id.c_str(), sha.c_str()));
	REQUIRE(target_conv_licence_write(id.c_str(), sha.c_str()));
	CHECK(target_conv_licence_read(id.c_str(), got, sizeof(got), &when));
	CHECK(std::string(got) == sha);
	CHECK(when > 0);
	CHECK(target_conv_licence_accepted(nullptr, id.c_str(), sha.c_str()));
	CHECK_FALSE(target_conv_licence_accepted(nullptr, id.c_str(), sha_hex("another text").c_str()));
	CHECK(target_conv_licence_withdraw(id.c_str()));
	CHECK_FALSE(target_conv_licence_accepted(nullptr, id.c_str(), sha.c_str()));
	CHECK_FALSE(target_conv_licence_write("../escape", sha.c_str())); // an id is never a path
	CHECK_FALSE(target_conv_licence_write(id.c_str(), "not-a-hash"));
	char parsed[80];
	CHECK(target_conv_licence_parse((sha + " 1764201600").c_str(), parsed, sizeof(parsed), &when));
	CHECK(when == 1764201600ull);
	CHECK_FALSE(target_conv_licence_parse("garbage", parsed, sizeof(parsed), &when));
#ifndef _WIN32
	if (saved_home.empty()) {
		unsetenv("HOME");
	} else {
		setenv("HOME", saved_home.c_str(), 1);
	}
	if (!saved_xdg.empty()) {
		setenv("XDG_DATA_HOME", saved_xdg.c_str(), 1);
	}
	std::error_code ec;
	fs::remove_all(home, ec);
#endif

	char why[256];
	// Windows: a first-party signature, or the allowlist. A valid signature by anyone else is not enough.
	CHECK(target_conv_build_trust_rule(TARGET_COMPONENT_PROVENANCE_SIGNED, "Leia, Inc.", true, false, why,
	                                   sizeof(why)));
	CHECK_FALSE(target_conv_build_trust_rule(TARGET_COMPONENT_PROVENANCE_SIGNED, "Someone Else", true, false, why,
	                                         sizeof(why)));
	CHECK(target_conv_build_trust_rule(TARGET_COMPONENT_PROVENANCE_ALLOWLISTED, "", true, false, why, sizeof(why)));
	CHECK_FALSE(target_conv_build_trust_rule(TARGET_COMPONENT_PROVENANCE_PLATFORM_TRUSTED, "", true, false, why,
	                                         sizeof(why)));
	// POSIX: root-owned under a system path, or the allowlist.
	CHECK(target_conv_build_trust_rule(TARGET_COMPONENT_PROVENANCE_PLATFORM_TRUSTED, "", false, false, why,
	                                   sizeof(why)));
	CHECK_FALSE(
	    target_conv_build_trust_rule(TARGET_COMPONENT_PROVENANCE_UNVERIFIED, "", false, false, why, sizeof(why)));
	// The dev override counts only for a builder found through DXR_MODEL_BUILDER_<ID>.
	CHECK(
	    target_conv_build_trust_rule(TARGET_COMPONENT_PROVENANCE_DEV_OVERRIDE, "", false, true, why, sizeof(why)));
	CHECK_FALSE(
	    target_conv_build_trust_rule(TARGET_COMPONENT_PROVENANCE_DEV_OVERRIDE, "", true, false, why, sizeof(why)));

	// Policy\Lock with the machine opt-in off: the per-run flag cannot lift it.
	target_conv_policy pol{};
	pol.policy_lock = true;
	pol.allow_research_only = false;
	conv_build_ops ops;
	target_conv_build_ops_fill(&ops, &pol, /*per_run_research=*/true);
	CHECK(ops.research_locked_off);
	CHECK_FALSE(ops.allow_research_only);
	pol.policy_lock = false;
	target_conv_build_ops_fill(&ops, &pol, true);
	CHECK(ops.allow_research_only);
}
