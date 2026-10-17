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
 *  - #2195: the POSIX downloader. A blob served over http by a loopback
 *    server in this test goes download -> verify -> rename -> stamp, shows
 *    DOWNLOADING with progress, follows a redirect; wrong bytes are rejected
 *    with no blob, stamp or part left; a 404 is a retryable network failure.
 *  - #2194: get_path. The scratch root per platform, a provider's scratch
 *    directory created owner-only on first request, MODEL_STORE = the
 *    process store's root, LOGS = the log file's directory.
 */

#include "catch_amalgamated.hpp"

#include "conversion/conv_host_paths.h"
#include "conversion/conv_model_store.h"
#include "util/u_sha256.h"

#include <algorithm>
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
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
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

	explicit fake_pack(const std::string &bytes = blob_bytes(),
	                   bool relative_base = false,
	                   const std::string &base_override = std::string())
	{
		src = dir.path / "src";
		fs::create_directories(src);
		sha = sha_hex(blob_bytes());
		write_file(src / (sha + ".bin"), bytes);
		const std::string rbytes = blob_bytes('R');
		sha_r = sha_hex(rbytes);
		write_file(src / (sha_r + ".bin"), rbytes);
		const std::string base = !base_override.empty() ? base_override
		                         : relative_base        ? std::string("src")
		                                                : "file://" + src.generic_u8string();
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


/*
 *
 * #2194: get_path (conv_host_paths.h).
 *
 */

namespace {

void
set_env(const char *name, const char *value)
{
#ifdef XRT_OS_WINDOWS
	_putenv_s(name, value != nullptr ? value : "");
	SetEnvironmentVariableA(name, value);
#else
	if (value != nullptr) {
		setenv(name, value, 1);
	} else {
		unsetenv(name);
	}
#endif
}

} // namespace

TEST_CASE("conv_paths: the scratch root per platform")
{
	char out[512];
	conv_scratch_root_inputs in{};

	in.os = CONV_SCRATCH_OS_MACOS;
	in.home = "/Users/u";
	REQUIRE(conv_scratch_root_from(&in, out, sizeof(out)));
	CHECK(std::string(out) == "/Users/u/Library/Caches/DisplayXR/Conversion");

	in = conv_scratch_root_inputs{};
	in.os = CONV_SCRATCH_OS_LINUX;
	in.home = "/home/u";
	REQUIRE(conv_scratch_root_from(&in, out, sizeof(out)));
	CHECK(std::string(out) == "/home/u/.cache/displayxr/conversion");
	in.xdg_cache_home = "/var/cache/u";
	REQUIRE(conv_scratch_root_from(&in, out, sizeof(out)));
	CHECK(std::string(out) == "/var/cache/u/displayxr/conversion");
	in.xdg_cache_home = "relative/cache"; // invalid per XDG: ignored
	REQUIRE(conv_scratch_root_from(&in, out, sizeof(out)));
	CHECK(std::string(out) == "/home/u/.cache/displayxr/conversion");

	in = conv_scratch_root_inputs{};
	in.os = CONV_SCRATCH_OS_WINDOWS;
	in.local_app_data = "C:\\Users\\u\\AppData\\Local";
	REQUIRE(conv_scratch_root_from(&in, out, sizeof(out)));
	CHECK(std::string(out) == "C:\\Users\\u\\AppData\\Local\\DisplayXR\\Conversion");

	in.env_override = "/tmp/x";
	REQUIRE(conv_scratch_root_from(&in, out, sizeof(out)));
	CHECK(std::string(out) == "/tmp/x");

	in = conv_scratch_root_inputs{};
	in.os = CONV_SCRATCH_OS_WINDOWS; // no LOCALAPPDATA
	CHECK_FALSE(conv_scratch_root_from(&in, out, sizeof(out)));
	CHECK(out[0] == '\0');
	in.os = CONV_SCRATCH_OS_LINUX; // no HOME, no XDG
	CHECK_FALSE(conv_scratch_root_from(&in, out, sizeof(out)));
	in.home = "/home/u";
	char tiny[8];
	CHECK_FALSE(conv_scratch_root_from(&in, tiny, sizeof(tiny))); // never truncated
	CHECK(tiny[0] == '\0');
}

TEST_CASE("conv_paths: a provider id is one path component")
{
	CHECK(conv_scratch_id_valid("open-conversion"));
	CHECK(conv_scratch_id_valid("sim_conversion.2"));
	CHECK_FALSE(conv_scratch_id_valid(nullptr));
	CHECK_FALSE(conv_scratch_id_valid(""));
	CHECK_FALSE(conv_scratch_id_valid("."));
	CHECK_FALSE(conv_scratch_id_valid(".."));
	CHECK_FALSE(conv_scratch_id_valid("a/b"));
	CHECK_FALSE(conv_scratch_id_valid("a\\b"));
	CHECK_FALSE(conv_scratch_id_valid("a b"));
	CHECK_FALSE(conv_scratch_id_valid(std::string(64, 'a').c_str()));
	CHECK(conv_scratch_id_valid(std::string(63, 'a').c_str()));
}

TEST_CASE("conv_paths: get_path answers a provider's scratch dir, the store root and the log dir")
{
	temp_dir t;
	const fs::path root = t.path / "scratch-root"; // does not exist yet
	set_env("DXR_CONV_SCRATCH_ROOT", root.u8string().c_str());
	struct Restore
	{
		~Restore()
		{
			set_env("DXR_CONV_SCRATCH_ROOT", nullptr);
		}
	} restore;

	char buf[512];
	REQUIRE(conv_host_get_path("open-conversion", XRT_CONV_PATH_SCRATCH, nullptr, buf, sizeof(buf)));
	const fs::path dir = root / "open-conversion";
	CHECK(fs::path(buf) == dir);
	CHECK(fs::is_directory(dir)); // created on first request
#ifndef XRT_OS_WINDOWS
	struct stat st = {};
	REQUIRE(stat(dir.c_str(), &st) == 0);
	CHECK((st.st_mode & 0777) == 0700);
#endif
	// Idempotent, and a provider's files survive the next request.
	write_file(dir / "cache.bin", "x");
	REQUIRE(conv_host_get_path("open-conversion", XRT_CONV_PATH_SCRATCH, nullptr, buf, sizeof(buf)));
	CHECK(fs::exists(dir / "cache.bin"));
	// Each provider its own.
	REQUIRE(conv_host_get_path("other", XRT_CONV_PATH_SCRATCH, nullptr, buf, sizeof(buf)));
	CHECK(fs::path(buf) == root / "other");

	// Refusals: no id, an id that names a path, a buffer too small, an unknown kind.
	CHECK_FALSE(conv_host_get_path("", XRT_CONV_PATH_SCRATCH, nullptr, buf, sizeof(buf)));
	CHECK(buf[0] == '\0');
	CHECK_FALSE(conv_host_get_path("../escape", XRT_CONV_PATH_SCRATCH, nullptr, buf, sizeof(buf)));
	CHECK_FALSE(fs::exists(t.path / "escape"));
	char tiny[8];
	CHECK_FALSE(conv_host_get_path("open-conversion", XRT_CONV_PATH_SCRATCH, nullptr, tiny, sizeof(tiny)));
	CHECK(tiny[0] == '\0');
	CHECK_FALSE(conv_host_get_path("open-conversion", 99u, nullptr, buf, sizeof(buf)));

	SECTION("a root that cannot be created is a false, not a path")
	{
		write_file(t.path / "a-file", "x");
		set_env("DXR_CONV_SCRATCH_ROOT", (t.path / "a-file" / "sub").u8string().c_str());
		CHECK_FALSE(conv_host_get_path("open-conversion", XRT_CONV_PATH_SCRATCH, nullptr, buf, sizeof(buf)));
		CHECK(buf[0] == '\0');
	}

	SECTION("MODEL_STORE: the process store's root (what it would resolve before it exists)")
	{
		const fs::path models = t.path / "Models";
		set_env("DXR_CONV_MODELS_ROOT", models.u8string().c_str());
		REQUIRE(conv_host_get_path("open-conversion", XRT_CONV_PATH_MODEL_STORE, nullptr, buf, sizeof(buf)));
		CHECK(fs::path(buf) == models);
		set_env("DXR_CONV_MODELS_ROOT", nullptr);
	}

	SECTION("LOGS: the directory of the host's log file; none without one")
	{
		const std::string log = (t.path / "logs" / "DisplayXR_x.log").u8string();
		REQUIRE(conv_host_get_path("open-conversion", XRT_CONV_PATH_LOGS, log.c_str(), buf, sizeof(buf)));
		CHECK(fs::path(buf) == t.path / "logs");
		CHECK_FALSE(conv_host_get_path("open-conversion", XRT_CONV_PATH_LOGS, "", buf, sizeof(buf)));
	}
}


/*
 *
 * #2195: the POSIX downloader (libcurl at run time) against a loopback server.
 *
 */

#ifndef XRT_OS_WINDOWS
namespace {

/*!
 * A minimal HTTP/1.0 server on 127.0.0.1:<ephemeral> serving the files of
 * @p root: `GET /<name>` = the file (sent in 64-byte pieces, 5 ms apart, so a
 * download is observably in flight), `GET /redirect/<name>` = 302 to
 * `/<name>`, anything else 404. One connection at a time, Connection: close.
 */
struct http_server
{
	fs::path root;
	int fd = -1;
	int port = 0;
	std::atomic<bool> stop{false};
	std::atomic<int> hits{0};
	std::thread th;

	explicit http_server(fs::path r) : root(std::move(r))
	{
		fd = socket(AF_INET, SOCK_STREAM, 0);
		REQUIRE(fd >= 0);
		int one = 1;
		(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		a.sin_port = 0;
		REQUIRE(bind(fd, (sockaddr *)&a, sizeof(a)) == 0);
		REQUIRE(listen(fd, 8) == 0);
		socklen_t len = sizeof(a);
		REQUIRE(getsockname(fd, (sockaddr *)&a, &len) == 0);
		port = ntohs(a.sin_port);
		th = std::thread([this] { loop(); });
	}

	~http_server()
	{
		stop = true;
		th.join();
		close(fd);
	}

	std::string
	base() const
	{
		return "http://127.0.0.1:" + std::to_string(port);
	}

	static bool
	send_all(int c, const char *p, size_t n)
	{
		while (n > 0) {
#ifdef MSG_NOSIGNAL
			const ssize_t w = send(c, p, n, MSG_NOSIGNAL);
#else
			const ssize_t w = send(c, p, n, 0);
#endif
			if (w <= 0) {
				return false;
			}
			p += w;
			n -= (size_t)w;
		}
		return true;
	}

	void
	serve(int c)
	{
#ifdef SO_NOSIGPIPE
		int one = 1;
		(void)setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
		std::string req;
		char buf[1024];
		while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192) {
			pollfd p{c, POLLIN, 0};
			if (poll(&p, 1, 2000) <= 0) {
				return;
			}
			const ssize_t n = recv(c, buf, sizeof(buf), 0);
			if (n <= 0) {
				return;
			}
			req.append(buf, (size_t)n);
		}
		hits++;
		const size_t sp1 = req.find(' ');
		const size_t sp2 = req.find(' ', sp1 + 1);
		const std::string path = sp1 == std::string::npos ? "" : req.substr(sp1 + 1, sp2 - sp1 - 1);
		const std::string redirect = "/redirect/";
		if (path.rfind(redirect, 0) == 0) {
			const std::string h = "HTTP/1.0 302 Found\r\nLocation: " + base() + "/" +
			                      path.substr(redirect.size()) +
			                      "\r\nContent-Length: 5\r\nConnection: close\r\n\r\nmoved";
			(void)send_all(c, h.data(), h.size());
			return;
		}
		const fs::path file = root / (path.size() > 1 ? path.substr(1) : std::string("-"));
		if (path.find("..") != std::string::npos || !fs::is_regular_file(file)) {
			const std::string h =
			    "HTTP/1.0 404 Not Found\r\nContent-Length: 9\r\nConnection: close\r\n\r\nnot found";
			(void)send_all(c, h.data(), h.size());
			return;
		}
		const std::string body = read_file(file);
		const std::string h = "HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " +
		                      std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
		if (!send_all(c, h.data(), h.size())) {
			return;
		}
		for (size_t off = 0; off < body.size() && !stop; off += 64) {
			if (!send_all(c, body.data() + off, std::min<size_t>(64, body.size() - off))) {
				return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}

	void
	loop()
	{
		while (!stop) {
			pollfd p{fd, POLLIN, 0};
			if (poll(&p, 1, 50) <= 0) {
				continue;
			}
			const int c = accept(fd, nullptr, nullptr);
			if (c < 0) {
				continue;
			}
			serve(c);
			close(c);
		}
	}
};

xrt_conv_model_location
wait_settled(conv_model_store *s, const char *ref, bool *saw_downloading, float *max_progress)
{
	xrt_conv_model_location loc{};
	for (int i = 0; i < 1000; i++) {
		loc = resolve(s, ref);
		if (loc.state == XRT_CONV_STATE_DOWNLOADING) {
			*saw_downloading = true;
			*max_progress = std::max(*max_progress, loc.progress);
		}
		if (loc.state == XRT_CONV_STATE_READY || loc.state == XRT_CONV_STATE_FAILED) {
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return loc;
}

} // namespace

TEST_CASE("conv_store: the POSIX downloader fetches over http, verifies, stamps last")
{
	char why[160];
	if (!conv_model_store_http_available(why, sizeof(why))) {
		SKIP(why); // a Linux box without libcurl.so.4
	}
	temp_dir srv_dir;
	http_server srv(srv_dir.path);
	temp_dir root;
	const fs::path store_root = root.path / "Models";
	const fs::path blobs = store_root / "blobs";

	SECTION("download -> verify -> rename -> stamp, DOWNLOADING with progress")
	{
		fake_pack fp(blob_bytes(), false, srv.base());
		fs::copy_file(fp.src / (fp.sha + ".bin"), srv_dir.path / (fp.sha + ".bin"));
		store_handle h(store_root);
		REQUIRE(conv_model_store_add_pack_file(h.s, fp.manifest.u8string().c_str()));
		REQUIRE(conv_model_store_request(h.s, "sim/fake"));
		bool saw = false;
		float maxp = -1.0f;
		const auto loc = wait_settled(h.s, "sim/fake", &saw, &maxp);
		REQUIRE(loc.state == XRT_CONV_STATE_READY);
		CHECK(saw);
		CHECK(maxp > 0.0f);
		CHECK(maxp <= 1.0f);
		CHECK(fs::path(loc.path) == blobs / fp.sha);
		CHECK(read_file(blobs / fp.sha) == blob_bytes());
		// The browser contract's stamp, byte for byte.
		CHECK(read_file(blobs / (fp.sha + ".ok")) == fp.sha + " 1024\n");
		CHECK_FALSE(has_part(blobs));
		CHECK(fs::exists(store_root / CONV_STORE_OWNER_MARKER));
		CHECK(srv.hits.load() == 1);
	}

	SECTION("a redirect is followed")
	{
		fake_pack fp(blob_bytes(), false, srv.base() + "/redirect");
		fs::copy_file(fp.src / (fp.sha + ".bin"), srv_dir.path / (fp.sha + ".bin"));
		store_handle h(store_root);
		REQUIRE(conv_model_store_add_pack_file(h.s, fp.manifest.u8string().c_str()));
		REQUIRE(conv_model_store_request(h.s, "sim/fake"));
		REQUIRE(conv_model_store_wait_idle(h.s, 10000));
		CHECK(resolve(h.s, "sim/fake").state == XRT_CONV_STATE_READY);
		CHECK(read_file(blobs / fp.sha) == blob_bytes());
		CHECK(srv.hits.load() == 2);
	}

	SECTION("wrong bytes (same size, wrong sha256) never become a blob: no blob, no stamp, no part")
	{
		fake_pack fp(blob_bytes('B'), false, srv.base());
		fs::copy_file(fp.src / (fp.sha + ".bin"), srv_dir.path / (fp.sha + ".bin")); // the 'B' bytes
		store_handle h(store_root);
		REQUIRE(conv_model_store_add_pack_file(h.s, fp.manifest.u8string().c_str()));
		REQUIRE(conv_model_store_request(h.s, "sim/fake"));
		REQUIRE(conv_model_store_wait_idle(h.s, 10000));
		const auto loc = resolve(h.s, "sim/fake");
		CHECK(loc.state == XRT_CONV_STATE_FAILED);
		CHECK(loc.failure_reason == XRT_CONV_FAIL_MODEL);
		CHECK_FALSE(fs::exists(blobs / fp.sha));
		CHECK_FALSE(fs::exists(blobs / (fp.sha + ".ok")));
		CHECK_FALSE(has_part(blobs));
	}

	SECTION("a longer body than the pack says is cut off and rejected")
	{
		fake_pack fp(blob_bytes(), false, srv.base());
		write_file(srv_dir.path / (fp.sha + ".bin"), blob_bytes() + "extra");
		store_handle h(store_root);
		REQUIRE(conv_model_store_add_pack_file(h.s, fp.manifest.u8string().c_str()));
		REQUIRE(conv_model_store_request(h.s, "sim/fake"));
		REQUIRE(conv_model_store_wait_idle(h.s, 10000));
		CHECK(resolve(h.s, "sim/fake").failure_reason == XRT_CONV_FAIL_MODEL);
		CHECK_FALSE(fs::exists(blobs / fp.sha));
		CHECK_FALSE(fs::exists(blobs / (fp.sha + ".ok")));
		CHECK_FALSE(has_part(blobs));
	}

	SECTION("a 404 is a retryable network failure, nothing written")
	{
		fake_pack fp(blob_bytes(), false, srv.base()); // nothing copied to the server
		store_handle h(store_root);
		REQUIRE(conv_model_store_add_pack_file(h.s, fp.manifest.u8string().c_str()));
		REQUIRE(conv_model_store_request(h.s, "sim/fake"));
		REQUIRE(conv_model_store_wait_idle(h.s, 10000));
		const auto loc = resolve(h.s, "sim/fake");
		CHECK(loc.state == XRT_CONV_STATE_FAILED);
		CHECK(loc.failure_reason == XRT_CONV_FAIL_NETWORK);
		CHECK_FALSE(fs::exists(blobs / fp.sha));
		CHECK_FALSE(has_part(blobs));

		// Served now: a new request retries and succeeds.
		fs::copy_file(fp.src / (fp.sha + ".bin"), srv_dir.path / (fp.sha + ".bin"));
		REQUIRE(conv_model_store_request(h.s, "sim/fake"));
		REQUIRE(conv_model_store_wait_idle(h.s, 10000));
		CHECK(resolve(h.s, "sim/fake").state == XRT_CONV_STATE_READY);
	}

	SECTION("the synchronous path (conv models fetch / installer pre-fetch) uses it too")
	{
		fake_pack fp(blob_bytes(), false, srv.base());
		fs::copy_file(fp.src / (fp.sha + ".bin"), srv_dir.path / (fp.sha + ".bin"));
		store_handle h(store_root);
		conv_pack p{};
		char err[256];
		REQUIRE(conv_pack_load(fp.manifest.u8string().c_str(), &p, err, sizeof(err)));
		const conv_pack_model *m = conv_pack_find_model(&p, "fake");
		REQUIRE(m != nullptr);
		CHECK(conv_model_store_fetch_model(h.s, &p, m, nullptr, true) == CONV_STORE_BLOB_OK);
		CHECK(read_file(blobs / (fp.sha + ".ok")) == fp.sha + " 1024\n");
		CHECK(conv_model_store_fetch_model(h.s, &p, m, nullptr, true) == CONV_STORE_BLOB_SKIPPED);
	}

	SECTION("a remote pack manifest loads over http")
	{
		fake_pack fp(blob_bytes(), false, srv.base());
		fs::copy_file(fp.manifest, srv_dir.path / "sim.models.json");
		conv_pack p{};
		char err[256];
		REQUIRE(conv_pack_load_url((srv.base() + "/sim.models.json").c_str(), &p, err, sizeof(err)));
		CHECK(std::string(p.pack) == "sim");
	}
}
#endif
