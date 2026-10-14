// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-053 P4 (#2108): the runtime-owned model store.
 *
 * What must hold (conversion-provider-discovery.md §5, roadmap §9 P4, §11.2):
 *  - the root order: HKLM Models\Path, then the browser's ModelsPath, then
 *    %ProgramData%\DisplayXR\Models (POSIX: $XDG_DATA_HOME, then /var/lib);
 *  - the write protocol with a fake pack served from a file:// URL: the
 *    blob lands under blobs/<sha256> with its stamp, no .part is left, the
 *    pack is recorded and DOWNLOADING is observable with progress;
 *  - a tampered same-size blob (a planted blob + stamp) is rejected, deleted
 *    and fetched again; bytes that fail the hash never become a blob;
 *  - policy: downloads off => ABSENT(policy); research-only models are gated
 *    by AllowResearchOnly;
 *  - prune deletes only unreferenced blobs; pack summaries;
 *  - Windows: a root the store creates is readable by BUILTIN\Users and by
 *    AppContainer processes, writable (create files) by the logged-in user,
 *    and the blobs inside it are not writable by other users.
 */

#include "catch_amalgamated.hpp"

#include "conversion/conv_model_store.h"
#include "util/u_sha256.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#ifdef XRT_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#endif

namespace fs = std::filesystem;

namespace {

std::string
blob_bytes(char salt = 0)
{
	// The sim provider's fake blob: 1 KiB of "0123456789abcdef" (CMake writes the same).
	std::string s;
	for (int i = 0; i < 64; i++) {
		s += "0123456789abcdef";
	}
	if (salt != 0) {
		s[100] = salt; // same size, different bytes
	}
	return s;
}

std::string
sha_hex(const std::string &bytes)
{
	uint8_t d[32];
	u_sha256(bytes.data(), bytes.size(), d);
	char hex[65];
	for (int i = 0; i < 32; i++) {
		snprintf(hex + i * 2, 3, "%02x", d[i]);
	}
	return hex;
}

void
write_file(const fs::path &p, const std::string &bytes)
{
	std::ofstream f(p, std::ios::binary);
	f.write(bytes.data(), (std::streamsize)bytes.size());
}

std::string
read_file(const fs::path &p)
{
	std::ifstream f(p, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

struct temp_dir
{
	fs::path path;
	temp_dir()
	{
		static int n = 0;
		path = fs::temp_directory_path() /
		       ("dxr_conv_store_" + std::to_string((unsigned long long)std::chrono::steady_clock::now()
		                                               .time_since_epoch()
		                                               .count()) +
		        "_" + std::to_string(n++));
		fs::create_directories(path);
	}
	~temp_dir()
	{
		std::error_code ec;
		fs::remove_all(path, ec);
	}
};

//! A fake pack "sim" with model "fake" (commercial) and "research" (non-commercial).
struct fake_pack
{
	temp_dir dir;
	fs::path src;      //!< file:// blob source
	fs::path manifest; //!< sim.models.json
	std::string sha, sha_r;

	explicit fake_pack(const std::string &bytes = blob_bytes(), bool relative_base = false)
	{
		src = dir.path / "src";
		fs::create_directories(src);
		sha = sha_hex(blob_bytes());
		write_file(src / (sha + ".bin"), bytes);
		const std::string rbytes = blob_bytes('R');
		sha_r = sha_hex(rbytes);
		write_file(src / (sha_r + ".bin"), rbytes);
		const std::string base = relative_base ? std::string("src") : "file://" + src.generic_u8string();
		const std::string json = R"({
  "schema": 2, "pack": "sim", "provider": "sim-conversion", "generated": "2026-10-14",
  "blobBaseUrl": ")" + base + R"(",
  "models": [
    { "name": "fake", "version": "1.0.0", "task": "views-mono", "prefetch": true,
      "license": { "spdx": "BSL-1.0", "commercial": true },
      "variants": [ { "id": "cpu", "backend": "cpu",
        "files": [ { "role": "model", "format": "bin", "sha256": ")" + sha + R"(", "size": 1024 } ] } ] },
    { "name": "research", "version": "1.0.0", "task": "depth-mono",
      "license": { "spdx": "MIT", "commercial": true,
                   "dependencies": [ { "name": "backbone", "spdx": "CC-BY-NC-4.0", "commercial": false } ] },
      "variants": [ { "id": "cpu", "backend": "cpu",
        "files": [ { "format": "bin", "sha256": ")" + sha_r + R"(", "size": 1024 } ] } ] }
  ],
  "defaults": { "views-mono": { "low": "fake", "medium": "fake", "high": "fake" } }
})";
		manifest = dir.path / "sim.models.json";
		write_file(manifest, json);
	}
};

struct store_handle
{
	conv_model_store *s = nullptr;
	explicit store_handle(const fs::path &root,
	                      bool downloads = true,
	                      bool research = false,
	                      uint32_t throttle_ms = 0,
	                      uint32_t chunk = 0)
	{
		conv_model_store_config cfg;
		conv_model_store_config_defaults(&cfg);
		snprintf(cfg.root, sizeof(cfg.root), "%s", root.u8string().c_str());
		cfg.allow_downloads = downloads;
		cfg.allow_research_only = research;
		cfg.throttle_ms = throttle_ms;
		cfg.chunk_bytes = chunk;
		REQUIRE(conv_model_store_create(&cfg, &s));
	}
	~store_handle()
	{
		conv_model_store_destroy(&s);
	}
};

xrt_conv_model_location
resolve(conv_model_store *s, const char *ref)
{
	xrt_conv_model_location loc{};
	loc.struct_size = sizeof(loc);
	conv_model_store_resolve(s, ref, &loc);
	return loc;
}

bool
has_part(const fs::path &blobs)
{
	if (!fs::exists(blobs)) {
		return false;
	}
	for (const auto &e : fs::directory_iterator(blobs)) {
		if (e.path().extension() == ".part") {
			return true;
		}
	}
	return false;
}

} // namespace

TEST_CASE("conv_store: root resolution order")
{
	char out[CONV_STORE_PATH_MAX], src[64];
	conv_store_root_inputs in{};
	in.windows = true;
	in.models_path = "D:\\Models";
	in.browser_models_path = "E:\\BrowserModels";
	in.program_data = "C:\\ProgramData";

	SECTION("HKLM Models\\Path wins over everything")
	{
		REQUIRE(conv_store_root_from(&in, out, sizeof(out), src, sizeof(src)));
		CHECK(std::string(out) == "D:\\Models");
		CHECK(std::string(src) == "HKLM Models\\Path");
	}
	SECTION("then the browser's ModelsPath (installed blobs are not re-downloaded)")
	{
		in.models_path = "";
		REQUIRE(conv_store_root_from(&in, out, sizeof(out), src, sizeof(src)));
		CHECK(std::string(out) == "E:\\BrowserModels");
	}
	SECTION("then %ProgramData%\\DisplayXR\\Models")
	{
		in.models_path = nullptr;
		in.browser_models_path = nullptr;
		REQUIRE(conv_store_root_from(&in, out, sizeof(out), src, sizeof(src)));
		CHECK(std::string(out) == "C:\\ProgramData\\DisplayXR\\Models");
	}
	SECTION("nothing set on Windows: no root")
	{
		in.models_path = in.browser_models_path = in.program_data = nullptr;
		CHECK_FALSE(conv_store_root_from(&in, out, sizeof(out), src, sizeof(src)));
	}
	SECTION("the dev / test override beats the registry")
	{
		in.env_override = "T:\\override";
		REQUIRE(conv_store_root_from(&in, out, sizeof(out), src, sizeof(src)));
		CHECK(std::string(out) == "T:\\override");
	}
	SECTION("POSIX: the writable XDG root first, /var/lib only as the packaged fallback")
	{
		conv_store_root_inputs p{};
		p.windows = false;
		p.models_path = "/ignored"; // registry keys mean nothing on POSIX
		p.xdg_data_home = "/home/u/.local/share";
		p.xdg_writable = true;
		REQUIRE(conv_store_root_from(&p, out, sizeof(out), src, sizeof(src)));
		CHECK(std::string(out) == "/home/u/.local/share/displayxr/models");
		p.xdg_writable = false; // a read-only home: the packaged root
		REQUIRE(conv_store_root_from(&p, out, sizeof(out), src, sizeof(src)));
		CHECK(std::string(out) == "/var/lib/displayxr/models");
		p.xdg_data_home = nullptr;
		REQUIRE(conv_store_root_from(&p, out, sizeof(out), src, sizeof(src)));
		CHECK(std::string(out) == "/var/lib/displayxr/models");
	}
}

TEST_CASE("conv_store: schema-2 pack manifests")
{
	fake_pack fp;
	conv_pack p{};
	char err[160];
	REQUIRE(conv_pack_load(fp.manifest.u8string().c_str(), &p, err, sizeof(err)));
	CHECK(std::string(p.pack) == "sim");
	REQUIRE(p.model_count == 2);
	CHECK(p.models[0].commercial);
	CHECK(p.models[0].prefetch);
	CHECK_FALSE(p.models[1].commercial); // a non-commercial dependency makes the AND false
	REQUIRE(p.default_count == 1);
	CHECK(std::string(p.defaults[0].medium) == "fake");
	char url[CONV_STORE_PATH_MAX];
	conv_pack_blob_url(&p, &p.models[0].variants[0].files[0], url, sizeof(url));
	CHECK(std::string(url).rfind("file://", 0) == 0);
	CHECK(std::string(url).find(fp.sha + ".bin") != std::string::npos);

	CHECK_FALSE(conv_pack_parse(R"({"schema":1,"pack":"x","models":[]})", ".", &p, err, sizeof(err)));
	CHECK(std::string(err) == "schema is not 2");
	CHECK_FALSE(conv_pack_parse(
	    R"({"schema":2,"pack":"x","models":[{"name":"a","variants":[{"files":[{"sha256":"zz","size":1}]}]}]})", ".",
	    &p, err, sizeof(err)));
	const std::string bad_default =
	    R"({"schema":2,"pack":"x","models":[{"name":"a","variants":[{"files":[{"sha256":")" + fp.sha +
	    R"(","size":1}]}]}],"defaults":{"depth-mono":{"low":"nope"}}})";
	CHECK_FALSE(conv_pack_parse(bad_default.c_str(), ".", &p, err, sizeof(err)));
	CHECK(std::string(err).find("nope") != std::string::npos);
}

TEST_CASE("conv_store: write protocol with the fake pack via file://")
{
	fake_pack fp;
	temp_dir root;
	const fs::path store_root = root.path / "Models";
	const fs::path blobs = store_root / "blobs";
	// 64-byte chunks, 15 ms each: ~240 ms of observable DOWNLOADING.
	store_handle h(store_root, true, false, 15, 64);
	REQUIRE(conv_model_store_add_pack_file(h.s, fp.manifest.u8string().c_str()));

	// Before anything: absent, nothing created.
	auto loc = resolve(h.s, "sim/fake");
	CHECK(loc.state == XRT_CONV_STATE_ABSENT);
	CHECK(loc.failure_reason == 0);
	CHECK_FALSE(fs::exists(store_root));

	REQUIRE(conv_model_store_request(h.s, "sim/fake"));
	bool saw_downloading = false;
	float max_progress = -1.0f;
	for (int i = 0; i < 400; i++) {
		loc = resolve(h.s, "sim/fake");
		if (loc.state == XRT_CONV_STATE_DOWNLOADING) {
			saw_downloading = true;
			if (loc.progress > max_progress) {
				max_progress = loc.progress;
			}
		}
		if (loc.state == XRT_CONV_STATE_READY) {
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	CHECK(saw_downloading);
	CHECK(max_progress > 0.0f);
	CHECK(max_progress <= 1.0f);
	REQUIRE(loc.state == XRT_CONV_STATE_READY);
	CHECK(fs::path(loc.path) == blobs / fp.sha);
	CHECK(read_file(blobs / fp.sha) == blob_bytes());
	CHECK(read_file(blobs / (fp.sha + ".ok")) == fp.sha + " 1024\n");
	CHECK_FALSE(has_part(blobs));
	CHECK(fs::exists(store_root / CONV_STORE_OWNER_MARKER));
	CHECK(fs::exists(store_root / "packs" / "sim.models.json"));

	// Same ref by sha256.
	CHECK(resolve(h.s, fp.sha.c_str()).state == XRT_CONV_STATE_READY);
	// Unknown refs.
	xrt_conv_model_location u{};
	u.struct_size = sizeof(u);
	CHECK_FALSE(conv_model_store_resolve(h.s, "sim/nope", &u));
	CHECK(u.failure_reason == XRT_CONV_FAIL_MODEL);
	CHECK_FALSE(conv_model_store_request(h.s, "other/fake"));

	conv_store_pack_summary sum{};
	REQUIRE(conv_model_store_pack_summary(h.s, 0, &sum));
	CHECK(std::string(sum.state) == "installed"); // the default set = the prefetch model
	CHECK(sum.models == 2);
	CHECK(sum.models_installed == 1);
	CHECK(sum.research_models == 1);
	CHECK(sum.bytes_total == 2048);
	CHECK(sum.bytes_installed == 1024);
}

TEST_CASE("conv_store: a relative blobBaseUrl resolves under the manifest")
{
	fake_pack fp(blob_bytes(), true);
	temp_dir root;
	store_handle h(root.path / "Models");
	REQUIRE(conv_model_store_add_pack_file(h.s, fp.manifest.u8string().c_str()));
	REQUIRE(conv_model_store_request(h.s, "sim/fake/cpu"));
	REQUIRE(conv_model_store_wait_idle(h.s, 5000));
	CHECK(resolve(h.s, "sim/fake").state == XRT_CONV_STATE_READY);
}

TEST_CASE("conv_store: tamper rejection")
{
	fake_pack fp;
	temp_dir root;
	const fs::path store_root = root.path / "Models";
	const fs::path blob = store_root / "blobs" / fp.sha;
	{
		store_handle h(store_root);
		REQUIRE(conv_model_store_add_pack_file(h.s, fp.manifest.u8string().c_str()));
		REQUIRE(conv_model_store_request(h.s, "sim/fake"));
		REQUIRE(conv_model_store_wait_idle(h.s, 5000));
		REQUIRE(resolve(h.s, "sim/fake").state == XRT_CONV_STATE_READY);

		SECTION("a blob replaced after this process verified it is not READY any more")
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(20)); // a distinct mtime
			write_file(blob, blob_bytes('X'));
			CHECK(resolve(h.s, "sim/fake").state == XRT_CONV_STATE_INSTALLED);
			REQUIRE(conv_model_store_request(h.s, "sim/fake"));
			REQUIRE(conv_model_store_wait_idle(h.s, 5000));
			CHECK(resolve(h.s, "sim/fake").state == XRT_CONV_STATE_READY);
			CHECK(read_file(blob) == blob_bytes());
		}
	}

	SECTION("a planted same-size blob + stamp is rejected by a fresh process, deleted, and re-fetched")
	{
		write_file(blob, blob_bytes('X')); // stamp from the honest run stays
		REQUIRE(fs::exists(store_root / "blobs" / (fp.sha + ".ok")));
		store_handle h(store_root);
		REQUIRE(conv_model_store_add_pack_file(h.s, fp.manifest.u8string().c_str()));
		// Stamp + size pass the browser's read check: not trusted, so not READY.
		CHECK(resolve(h.s, "sim/fake").state == XRT_CONV_STATE_INSTALLED);
		REQUIRE(conv_model_store_request(h.s, "sim/fake"));
		REQUIRE(conv_model_store_wait_idle(h.s, 5000));
		auto loc = resolve(h.s, "sim/fake");
		CHECK(loc.state == XRT_CONV_STATE_READY);
		CHECK(read_file(blob) == blob_bytes());
	}

	SECTION("with downloads off a tampered blob is deleted and the model FAILED(MODEL)")
	{
		write_file(blob, blob_bytes('X'));
		store_handle h(store_root, false);
		REQUIRE(conv_model_store_add_pack_file(h.s, fp.manifest.u8string().c_str()));
		REQUIRE(conv_model_store_request(h.s, "sim/fake")); // present on disk: a verify, not a download
		REQUIRE(conv_model_store_wait_idle(h.s, 5000));
		auto loc = resolve(h.s, "sim/fake");
		CHECK(loc.state == XRT_CONV_STATE_FAILED);
		CHECK(loc.failure_reason == XRT_CONV_FAIL_MODEL);
		CHECK_FALSE(fs::exists(blob));
		CHECK_FALSE(fs::exists(store_root / "blobs" / (fp.sha + ".ok")));
	}

	SECTION("conv models verify: the synchronous check deletes a tampered blob")
	{
		write_file(blob, blob_bytes('X'));
		store_handle h(store_root);
		CHECK(conv_model_store_verify_blob(h.s, fp.sha.c_str(), 1024) == CONV_STORE_BLOB_TAMPERED);
		CHECK_FALSE(fs::exists(blob));
		CHECK(conv_model_store_verify_blob(h.s, fp.sha.c_str(), 1024) == CONV_STORE_BLOB_MISSING);
	}
}

TEST_CASE("conv_store: bytes that fail the hash never become a blob")
{
	fake_pack fp(blob_bytes('B')); // the source serves the wrong (same-size) bytes
	temp_dir root;
	const fs::path store_root = root.path / "Models";
	store_handle h(store_root);
	REQUIRE(conv_model_store_add_pack_file(h.s, fp.manifest.u8string().c_str()));
	REQUIRE(conv_model_store_request(h.s, "sim/fake"));
	REQUIRE(conv_model_store_wait_idle(h.s, 5000));
	auto loc = resolve(h.s, "sim/fake");
	CHECK(loc.state == XRT_CONV_STATE_FAILED);
	CHECK(loc.failure_reason == XRT_CONV_FAIL_MODEL);
	CHECK_FALSE(fs::exists(store_root / "blobs" / fp.sha));
	CHECK_FALSE(fs::exists(store_root / "blobs" / (fp.sha + ".ok")));
	CHECK_FALSE(has_part(store_root / "blobs"));

	SECTION("a missing source is a retryable network failure")
	{
		fs::remove(fp.src / (fp.sha + ".bin"));
		REQUIRE(conv_model_store_request(h.s, "sim/fake")); // FAILED is retried by a new request
		REQUIRE(conv_model_store_wait_idle(h.s, 5000));
		CHECK(resolve(h.s, "sim/fake").failure_reason == XRT_CONV_FAIL_NETWORK);
	}
}

TEST_CASE("conv_store: policy gates")
{
	fake_pack fp;
	temp_dir root;
	const fs::path store_root = root.path / "Models";

	SECTION("downloads forbidden by policy: ABSENT(policy), nothing written")
	{
		store_handle h(store_root, false);
		REQUIRE(conv_model_store_add_pack_file(h.s, fp.manifest.u8string().c_str()));
		CHECK_FALSE(conv_model_store_request(h.s, "sim/fake"));
		auto loc = resolve(h.s, "sim/fake");
		CHECK(loc.state == XRT_CONV_STATE_ABSENT);
		CHECK(loc.failure_reason == XRT_CONV_FAIL_POLICY);
		CHECK_FALSE(fs::exists(store_root / "blobs"));
		// The policy lifts: a new request downloads.
		conv_model_store_set_policy(h.s, true, false);
		CHECK(resolve(h.s, "sim/fake").failure_reason == 0);
		REQUIRE(conv_model_store_request(h.s, "sim/fake"));
		REQUIRE(conv_model_store_wait_idle(h.s, 5000));
		CHECK(resolve(h.s, "sim/fake").state == XRT_CONV_STATE_READY);
	}
	SECTION("a research-only model needs AllowResearchOnly, to fetch AND to load")
	{
		store_handle h(store_root);
		REQUIRE(conv_model_store_add_pack_file(h.s, fp.manifest.u8string().c_str()));
		CHECK_FALSE(conv_model_store_request(h.s, "sim/research"));
		auto loc = resolve(h.s, "sim/research");
		CHECK(loc.state == XRT_CONV_STATE_ABSENT);
		CHECK(loc.failure_reason == XRT_CONV_FAIL_POLICY);
		CHECK_FALSE(fs::exists(store_root / "blobs" / fp.sha_r));
		conv_pack p{};
		REQUIRE(conv_model_store_get_pack(h.s, 0, &p));
		CHECK(conv_model_store_fetch_model(h.s, &p, &p.models[1], nullptr, false) == CONV_STORE_BLOB_POLICY);

		conv_model_store_set_policy(h.s, true, true);
		REQUIRE(conv_model_store_request(h.s, "sim/research"));
		REQUIRE(conv_model_store_wait_idle(h.s, 5000));
		CHECK(resolve(h.s, "sim/research").state == XRT_CONV_STATE_READY);
		// Opt-in withdrawn: present on disk, still never handed out.
		conv_model_store_set_policy(h.s, true, false);
		loc = resolve(h.s, "sim/research");
		CHECK(loc.state == XRT_CONV_STATE_ABSENT);
		CHECK(loc.failure_reason == XRT_CONV_FAIL_POLICY);
	}
}

TEST_CASE("conv_store: synchronous fetch (installer pre-fetch), prune, pack states")
{
	fake_pack fp;
	temp_dir root;
	const fs::path store_root = root.path / "Models";
	const fs::path blobs = store_root / "blobs";
	store_handle h(store_root);
	REQUIRE(conv_model_store_add_pack_file(h.s, fp.manifest.u8string().c_str()));
	conv_pack p{};
	REQUIRE(conv_model_store_get_pack(h.s, 0, &p));

	conv_store_pack_summary sum{};
	REQUIRE(conv_model_store_pack_summary(h.s, 0, &sum));
	CHECK(std::string(sum.state) == "absent");

	CHECK(conv_model_store_fetch_model(h.s, &p, &p.models[0], nullptr, true) == CONV_STORE_BLOB_OK);
	CHECK(conv_model_store_fetch_model(h.s, &p, &p.models[0], nullptr, true) == CONV_STORE_BLOB_SKIPPED);
	REQUIRE(conv_model_store_pack_summary(h.s, 0, &sum));
	CHECK(std::string(sum.state) == "installed");

	// A .part of a pack blob (another writer mid-download) reads as downloading.
	fs::remove(blobs / fp.sha);
	fs::remove(blobs / (fp.sha + ".ok"));
	write_file(blobs / (fp.sha + ".installer.part"), std::string(256, 'x'));
	REQUIRE(conv_model_store_pack_summary(h.s, 0, &sum));
	CHECK(std::string(sum.state) == "downloading");
	CHECK(sum.download_done == 256);
	CHECK(sum.download_total == 1024);
	fs::remove(blobs / (fp.sha + ".installer.part"));

	// Wiped under us (an older browser uninstaller): evicted, and a request fetches it again.
	REQUIRE(conv_model_store_pack_summary(h.s, 0, &sum));
	CHECK(std::string(sum.state) == "evicted");
	REQUIRE(conv_model_store_request(h.s, "sim/fake"));
	REQUIRE(conv_model_store_wait_idle(h.s, 5000));
	CHECK(resolve(h.s, "sim/fake").state == XRT_CONV_STATE_READY);

	// Prune: an unreferenced blob goes, referenced ones stay.
	const std::string orphan = sha_hex("orphan");
	write_file(blobs / orphan, "orphan");
	write_file(blobs / (orphan + ".ok"), orphan + " 6\n");
	write_file(blobs / "README.txt", "not ours");
	uint64_t bytes = 0;
	CHECK(conv_model_store_prune(h.s, &bytes) == 1);
	CHECK(bytes == 6);
	CHECK_FALSE(fs::exists(blobs / orphan));
	CHECK_FALSE(fs::exists(blobs / (orphan + ".ok")));
	CHECK(fs::exists(blobs / fp.sha));
	CHECK(fs::exists(blobs / "README.txt"));
}

TEST_CASE("conv_store: process glue serves the host ops")
{
	fake_pack fp;
	temp_dir root;
	const std::string r = (root.path / "Models").u8string();
#ifdef XRT_OS_WINDOWS
	_putenv_s("DXR_CONV_MODELS_ROOT", r.c_str());
#else
	setenv("DXR_CONV_MODELS_ROOT", r.c_str(), 1);
#endif
	const std::string m = fp.manifest.u8string();
	const char *packs[] = {m.c_str()};
	conv_store_host_configure(packs, 1, true, false);
	REQUIRE(conv_store_host_get() != nullptr);
	CHECK(std::string(conv_model_store_root(conv_store_host_get())) == r);
	REQUIRE(conv_store_host_model_request("sim/fake"));
	REQUIRE(conv_model_store_wait_idle(conv_store_host_get(), 5000));
	xrt_conv_model_location loc{};
	loc.struct_size = sizeof(loc);
	REQUIRE(conv_store_host_model_resolve("sim/fake", &loc));
	CHECK(loc.state == XRT_CONV_STATE_READY);
	// A truncated out-struct (an older provider) is written only within struct_size.
	struct
	{
		uint32_t struct_size;
		uint32_t state;
		uint32_t canary;
	} tiny{8u, 0u, 0xC0FFEEu};
	CHECK(conv_store_host_model_resolve("sim/fake", (xrt_conv_model_location *)&tiny));
	CHECK(tiny.state == XRT_CONV_STATE_READY);
	CHECK(tiny.canary == 0xC0FFEEu);
	conv_store_host_shutdown();
	CHECK(conv_store_host_get() == nullptr);
#ifdef XRT_OS_WINDOWS
	_putenv_s("DXR_CONV_MODELS_ROOT", "");
#else
	unsetenv("DXR_CONV_MODELS_ROOT");
#endif
}

#ifdef XRT_OS_WINDOWS
namespace {

ACCESS_MASK
rights_for(const fs::path &p, const char *sid_str)
{
	PSID sid = nullptr;
	REQUIRE(ConvertStringSidToSidA(sid_str, &sid));
	PACL dacl = nullptr;
	PSECURITY_DESCRIPTOR sd = nullptr;
	REQUIRE(GetNamedSecurityInfoW(p.wstring().c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
	                              &dacl, nullptr, &sd) == ERROR_SUCCESS);
	TRUSTEE_W t{};
	BuildTrusteeWithSidW(&t, sid);
	ACCESS_MASK m = 0;
	REQUIRE(GetEffectiveRightsFromAclW(dacl, &t, &m) == ERROR_SUCCESS);
	LocalFree(sd);
	LocalFree(sid);
	return m;
}

} // namespace

TEST_CASE("conv_store: the ACL of a root the store creates (Windows)")
{
	fake_pack fp;
	temp_dir root;
	const fs::path store_root = root.path / "Models";
	store_handle h(store_root);
	REQUIRE(conv_model_store_add_pack_file(h.s, fp.manifest.u8string().c_str()));
	REQUIRE(conv_model_store_request(h.s, "sim/fake")); // creates the root + writes as this (service) user
	REQUIRE(conv_model_store_wait_idle(h.s, 5000));
	REQUIRE(resolve(h.s, "sim/fake").state == XRT_CONV_STATE_READY);

	const char *users = "S-1-5-32-545";     // BUILTIN\Users
	const char *packages = "S-1-15-2-1";    // ALL APPLICATION PACKAGES
	const fs::path blobs = store_root / "blobs";
	const ACCESS_MASK root_users = rights_for(store_root, users);
	const ACCESS_MASK blobs_users = rights_for(blobs, users);
	// Users: list + traverse + read, and create files / folders on directories ...
	CHECK((root_users & FILE_LIST_DIRECTORY) != 0);
	CHECK((root_users & FILE_TRAVERSE) != 0);
	CHECK((blobs_users & FILE_ADD_FILE) != 0);
	CHECK((blobs_users & FILE_ADD_SUBDIRECTORY) != 0);
	CHECK((blobs_users & FILE_DELETE_CHILD) == 0);
	// ... and only read on files: no overwrite of another writer's blob.
	const ACCESS_MASK blob_users = rights_for(blobs / fp.sha, users);
	CHECK((blob_users & FILE_READ_DATA) != 0);
	CHECK((blob_users & FILE_WRITE_DATA) == 0);
	CHECK((blob_users & FILE_APPEND_DATA) == 0);
	CHECK((blob_users & DELETE) == 0);
	// AppContainer readers (a sandboxed browser GPU process) can read the blobs.
	CHECK((rights_for(blobs, packages) & FILE_LIST_DIRECTORY) != 0);
	CHECK((rights_for(blobs / fp.sha, packages) & FILE_READ_DATA) != 0);
	CHECK((rights_for(blobs / fp.sha, packages) & FILE_WRITE_DATA) == 0);
	// SYSTEM (an installer pre-fetch) has full control.
	CHECK((rights_for(blobs, "S-1-5-18") & FILE_ALL_ACCESS) == FILE_ALL_ACCESS);

	// An existing root's ACL is left alone (the installer's, or the browser's).
	temp_dir other;
	store_handle h2(other.path);
	CHECK(conv_model_store_ensure_root(h2.s));
	CHECK_FALSE(fs::exists(other.path / CONV_STORE_OWNER_MARKER));
	CHECK(fs::exists(other.path / "blobs"));
}
#endif
