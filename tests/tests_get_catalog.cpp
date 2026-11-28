// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  dxr-get's core (src/xrt/targets/get/get_core.c): the catalog model,
 *         the plan and the trust decisions of ADR-064 steps 2 and 3.
 *
 * dxr-get itself is Windows-only; its decisions are not. Everything the
 * machine contributes comes through `dxr_env`, faked here, so these run on
 * every platform CI builds.
 *
 * What must hold (docs/specs/runtime/dxr-get.md, component-catalog.md):
 *  - a schema-2 catalog and a schema-1 catalog of the same components give
 *    the same plan, and the schema-1 names keep resolving (fallback);
 *  - the plan JSON keeps the shape deployed installers scan: one FLAT object
 *    per action with `component` first;
 *  - an item whose runtime range or class ABI excludes this machine's runtime
 *    is refused with its own code, and the runtime is never downgraded to fit;
 *  - prerequisites come before what needs them, an external prerequisite's
 *    own asset before its item, and the total size is known up front;
 *  - every asset is signed by a signer delegated to its OWN publisher;
 *  - an item installed at the catalog's version is not downloaded again;
 *  - receipts drive `list --installed`, `outdated` and the dependents check.
 */

#include "catch_amalgamated.hpp"

#include "get_core.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

const time_t NOW = 1795000000; // 2026-11-17, inside every fixture's validity

// The registry and the file system do not care about case; neither does the fake.
struct NoCase
{
	bool
	operator()(const std::string &a, const std::string &b) const
	{
		return std::lexicographical_compare(
		    a.begin(), a.end(), b.begin(), b.end(),
		    [](unsigned char x, unsigned char y) { return std::tolower(x) < std::tolower(y); });
	}
};

struct Machine
{
	std::map<std::string, std::string, NoCase> sz; // "HKLM\...\Value" -> data
	std::map<std::string, uint32_t, NoCase> dword; // "key|value" -> data
	std::set<std::string, NoCase> keys, files, services;
	std::vector<dxr_edid_id> edids;
	std::vector<dxr_usb_id> usb;
	std::map<std::string, dxr_receipt> receipts;
	std::map<std::string, uint64_t> cached; // sha256 -> bytes in the download cache

	// --assume-runtime / --assume-abi, as dxr-get's command line sets them.
	const char *assume_runtime = nullptr;
	std::vector<std::pair<const char *, int>> assume_abi;

	dxr_env
	env()
	{
		dxr_env e = {};
		e.user = this;
		e.assume_runtime = assume_runtime;
		for (auto &a : assume_abi) {
			e.assume_abi[e.n_assume_abi].item_class = a.first;
			e.assume_abi[e.n_assume_abi].major = a.second;
			e.n_assume_abi++;
		}
		e.reg_sz = [](void *u, const char *p, char *out, size_t n) {
			auto &m = static_cast<Machine *>(u)->sz;
			auto it = m.find(p);
			if (it == m.end()) {
				return false;
			}
			snprintf(out, n, "%s", it->second.c_str());
			return true;
		};
		e.reg_dword = [](void *u, const char *k, const char *v, uint32_t *out) {
			auto &m = static_cast<Machine *>(u)->dword;
			auto it = m.find(std::string(k) + "|" + v);
			if (it == m.end()) {
				return false;
			}
			*out = it->second;
			return true;
		};
		e.reg_key_exists = [](void *u, const char *k) { return static_cast<Machine *>(u)->keys.count(k) > 0; };
		e.file_exists = [](void *u, const char *p) { return static_cast<Machine *>(u)->files.count(p) > 0; };
		e.service_exists = [](void *u, const char *s) {
			return static_cast<Machine *>(u)->services.count(s) > 0;
		};
		e.edids = [](void *u, dxr_edid_id *out, int max) {
			auto &v = static_cast<Machine *>(u)->edids;
			int n = 0;
			for (; n < max && n < (int)v.size(); n++) {
				out[n] = v[n];
			}
			return n;
		};
		e.usb = [](void *u, dxr_usb_id *out, int max) {
			auto &v = static_cast<Machine *>(u)->usb;
			int n = 0;
			for (; n < max && n < (int)v.size(); n++) {
				out[n] = v[n];
			}
			return n;
		};
		e.receipt = [](void *u, const char *id, dxr_receipt *out) {
			auto &m = static_cast<Machine *>(u)->receipts;
			auto it = m.find(id);
			if (it == m.end()) {
				return false;
			}
			*out = it->second;
			return true;
		};
		e.cached_bytes = [](void *u, const char *sha, const char *, uint64_t) -> uint64_t {
			auto &m = static_cast<Machine *>(u)->cached;
			auto it = m.find(sha);
			return it == m.end() ? 0 : it->second;
		};
		return e;
	}
};

const char *RT_VER = "HKLM\\Software\\DisplayXR\\Runtime\\Version";
const char *ABI_STAMP = "HKLM\\SOFTWARE\\DisplayXR\\Runtime\\Abi\\RuntimeVersion";
const char *LEIA_VER = "HKLM\\Software\\DisplayXR\\Plugins\\LeiaSR\\Version";

#define SHA_RT "1111111111111111111111111111111111111111111111111111111111111111"
#define SHA_LEIA "2222222222222222222222222222222222222222222222222222222222222222"
#define SHA_BROWSER "3333333333333333333333333333333333333333333333333333333333333333"
#define SHA_ACME "4444444444444444444444444444444444444444444444444444444444444444"
#define SHA_ACME_RT "5555555555555555555555555555555555555555555555555555555555555555"
#define SHA_HANDS "6666666666666666666666666666666666666666666666666666666666666666"
#define SHA_OPEN "7777777777777777777777777777777777777777777777777777777777777777"

// The three first-party components as scripts/gen_catalog.py writes schema 1.
const char *V1 = R"json({
 "schema": 1, "serial": 100, "generated": "2026-11-10T00:00:00Z", "expires": "2026-12-10T00:00:00Z",
 "channel": "stable", "signers": { "windows": ["Leia, Inc."] },
 "components": {
  "runtime": { "recommended": "2.70.0", "windows": {
    "version": "2.70.0", "file": "DisplayXRSetup-2.70.0.exe", "size": 50000000, "sha256": ")json" SHA_RT R"json(",
    "signer": "Leia, Inc.", "urls": ["https://example.invalid/rt.exe"], "args": ["/S", "/NOSTART"],
    "registry_version": "HKLM\\Software\\DisplayXR\\Runtime\\Version" } },
  "leia_plugin": { "recommended": "2.23.0", "kind": "display_plugin", "requires": { "runtime": ">=2.66.8" },
   "windows": { "version": "2.23.0", "file": "DisplayXRLeiaSRSetup-2.23.0.exe", "size": 30000000,
    "sha256": ")json" SHA_LEIA R"json(", "signer": "Leia, Inc.", "urls": ["https://example.invalid/leia.exe"],
    "args": ["/S"], "registry_version": "HKLM\\Software\\DisplayXR\\Plugins\\LeiaSR\\Version" },
   "detect": [
    { "kind": "edid", "man": 44048, "prod": 16711, "confidence": "high", "note": "DELL_8K32" },
    { "kind": "edid", "man": 1138, "prod": 2, "confidence": "low", "note": "generic" } ] },
  "browser": { "recommended": "1.10.3", "requires": { "runtime": ">=2.62.0" }, "windows": {
    "version": "1.10.3", "file": "DisplayXR-Browser-Setup-1.10.3.exe", "size": 200000000,
    "sha256": ")json" SHA_BROWSER R"json(", "signer": "Leia, Inc.", "urls": ["https://example.invalid/b.exe"],
    "args": ["/S"], "registry_version": "HKLM\\Software\\DisplayXR\\Browser\\Version" } }
 }
})json";

// The same three as the generator derives schema 2, plus third-party items
// that exercise what schema 1 cannot say.
const char *V2 = R"json({
 "schema": 2, "serial": 100, "generated": "2026-11-10T00:00:00Z", "expires": "2026-12-10T00:00:00Z",
 "channel": "stable",
 "publishers": {
  "displayxr": { "name": "DisplayXR", "signers": { "windows-x64": [ { "kind": "authenticode", "subject_cn": "Leia, Inc." } ] } },
  "acme": { "name": "Acme Displays", "signers": { "windows-x64": [ { "kind": "authenticode", "subject_cn": "Acme Displays, Inc." } ] } },
  "handco": { "name": "Handco", "signers": { "windows-x64": [ { "kind": "authenticode", "subject_cn": "Handco Ltd" } ] } },
  "nosigner": { "name": "No Signer" }
 },
 "revoked": [ { "sha256": "9999999999999999999999999999999999999999999999999999999999999999", "reason": "bad build" } ],
 "items": [
  { "id": "runtime", "class": "runtime", "name": "DisplayXR Runtime", "publisher": "displayxr", "version": "2.70.0",
    "abi": { "display_processor": { "min": 5, "max": 5 }, "conversion_provider": { "min": 1, "max": 1 } },
    "assets": { "windows-x64": { "version": "2.70.0", "file": "DisplayXRSetup-2.70.0.exe", "size": 50000000,
      "sha256": ")json" SHA_RT R"json(", "urls": ["https://example.invalid/rt.exe"],
      "signature": { "kind": "authenticode", "signer": "Leia, Inc." },
      "installer": { "kind": "exe", "args": ["/S", "/NOSTART"], "scope": "machine", "elevation": "required" },
      "installed": { "kind": "registry_sz", "path": "HKLM\\Software\\DisplayXR\\Runtime\\Version" } } } },
  { "id": "leia-sr", "aliases": ["leia_plugin"], "class": "display_processor", "registered_id": "leia-sr",
    "name": "Leia SR display processor", "publisher": "displayxr", "version": "2.23.0",
    "requires": [ { "kind": "runtime", "version": ">=2.66.8" } ],
    "assets": { "windows-x64": { "version": "2.23.0", "file": "DisplayXRLeiaSRSetup-2.23.0.exe", "size": 30000000,
      "sha256": ")json" SHA_LEIA R"json(", "urls": ["https://example.invalid/leia.exe"],
      "signature": { "kind": "authenticode", "signer": "Leia, Inc." },
      "installer": { "kind": "exe", "args": ["/S"], "scope": "machine", "elevation": "required" },
      "installed": { "kind": "registry_sz", "path": "HKLM\\Software\\DisplayXR\\Plugins\\LeiaSR\\Version" } } },
    "detect": [
     { "kind": "edid", "man": 44048, "prod": 16711, "confidence": "high", "note": "DELL_8K32" },
     { "kind": "edid", "man": 1138, "prod": 2, "confidence": "low", "note": "generic" } ] },
  { "id": "browser", "class": "app", "name": "DisplayXR Browser", "publisher": "displayxr", "version": "1.10.3",
    "requires": [ { "kind": "runtime", "version": ">=2.62.0" } ],
    "assets": { "windows-x64": { "version": "1.10.3", "file": "DisplayXR-Browser-Setup-1.10.3.exe", "size": 200000000,
      "sha256": ")json" SHA_BROWSER R"json(", "urls": ["https://example.invalid/b.exe"],
      "signature": { "kind": "authenticode", "signer": "Leia, Inc." },
      "installer": { "kind": "exe", "args": ["/S"], "scope": "machine", "elevation": "required" },
      "installed": { "kind": "registry_sz", "path": "HKLM\\Software\\DisplayXR\\Browser\\Version" } } } },
  { "id": "open-conversion", "class": "conversion_provider", "registered_id": "open-conversion",
    "name": "Open conversion", "summary": "2D to 3D conversion with open models.", "recommended": true,
    "publisher": "displayxr", "version": "0.1.1",
    "abi": { "conversion_provider": { "min": 1, "max": 1 } }, "runtime": { "min": "2.45.0" },
    "assets": { "windows-x64": { "version": "0.1.1", "file": "OpenConv-0.1.1.exe", "size": 12000000,
      "sha256": ")json" SHA_OPEN R"json(", "urls": ["https://example.invalid/open.exe"],
      "signature": { "kind": "authenticode", "signer": "Leia, Inc." },
      "installer": { "kind": "exe", "args": ["/S"], "scope": "machine", "elevation": "required" },
      "installed": { "kind": "registration_row" },
      "uninstall": { "kind": "registry_command", "path": "HKLM\\Software\\DisplayXR\\ConversionProviders\\open-conversion\\UninstallString", "args": ["/S"] } } } },
  { "id": "acme.depth-pro", "class": "conversion_provider", "registered_id": "acme-depth-pro",
    "name": "Acme Depth Pro", "publisher": "acme", "tier": "verified", "version": "1.4.0",
    "abi": { "conversion_provider": { "min": 1, "max": 1 } }, "runtime": { "min": "2.60.0" },
    "requires": [
     { "kind": "runtime", "version": ">=2.60.0" },
     { "kind": "item", "id": "open-conversion", "version": ">=0.1.0" },
     { "kind": "external", "name": "Acme Depth Runtime", "min_version": "3.2",
       "detect": [ { "kind": "registry_sz_min", "key": "HKLM\\SOFTWARE\\Acme\\DepthRuntime", "value": "Version", "min": "3.2" } ],
       "info_url": "https://acme.example/depth-runtime",
       "asset": { "windows-x64": { "version": "3.2.1", "file": "AcmeDepthRuntimeSetup-3.2.1.exe", "size": 20000000,
         "sha256": ")json" SHA_ACME_RT R"json(", "urls": ["https://example.invalid/acme-rt.exe"],
         "signature": { "kind": "authenticode", "signer": "Acme Displays, Inc." },
         "installer": { "kind": "exe", "args": ["/quiet"], "scope": "machine", "elevation": "required" } } } } ],
    "assets": { "windows-x64": { "version": "1.4.0", "file": "AcmeDepthProSetup-1.4.0.exe", "size": 18000000,
      "sha256": ")json" SHA_ACME R"json(", "urls": ["https://example.invalid/acme.exe"],
      "signature": { "kind": "authenticode", "signer": "Acme Displays, Inc." },
      "installer": { "kind": "exe", "args": ["/quiet"], "scope": "machine", "elevation": "required" },
      "installed": { "kind": "registration_row" } } } },
  { "id": "handco.hands", "class": "input_provider", "registered_id": "handco-hands", "name": "Handco Hands",
    "publisher": "handco", "tier": "listed", "version": "0.3.0",
    "abi": { "input_provider": { "min": 1, "max": 1 } }, "runtime": { "min": "2.60.0", "max": "2.69.99" },
    "requires": [
     { "kind": "runtime", "version": ">=2.60.0" },
     { "kind": "external", "name": "Handco Tracking Service",
       "detect": [ { "kind": "registry_key_exists", "key": "HKLM\\SOFTWARE\\Handco\\TrackingService" },
                   { "kind": "service_exists", "name": "HandcoTracking" } ],
       "info_url": "https://handco.example/tracking-service" } ],
    "assets": { "windows-x64": { "version": "0.3.0", "file": "HandcoHandsSetup-0.3.0.exe", "size": 2000000,
      "sha256": ")json" SHA_HANDS R"json(", "urls": ["https://example.invalid/hands.exe"],
      "signature": { "kind": "authenticode", "signer": "Handco Ltd" },
      "installer": { "kind": "exe", "args": ["/S"], "scope": "machine", "elevation": "required" },
      "installed": { "kind": "registration_row" } } } },
  { "id": "acme.viewer", "class": "app", "name": "Acme Viewer", "publisher": "acme", "version": "3.0.0",
    "assets": { "windows-x64": { "listing": { "url": "https://acme.example/viewer" } } } },
  { "id": "future.thing", "class": "hologram_projector", "name": "From the future", "publisher": "acme", "version": "1.0.0" }
 ]
})json";

struct Cat
{
	dxr_catalog c;
	explicit Cat(const char *body, time_t now = NOW)
	{
		char err[128] = {0};
		rc = dxr_catalog_parse(body, strlen(body), now, &c, err, sizeof(err));
		error = err;
	}
	~Cat()
	{
		dxr_catalog_free(&c);
	}
	int rc;
	std::string error;
};

dxr_plan
plan(
    Cat &cat, Machine &m, const char *x, dxr_plan_mode mode, const char *min_runtime = nullptr, bool no_runtime = false)
{
	dxr_plan p;
	dxr_env e = m.env();
	dxr_plan_opts opts = {no_runtime};
	dxr_make_plan(&cat.c, &e, x, min_runtime, mode, &opts, &p);
	return p;
}

dxr_plan
default_plan(Cat &cat, Machine &m, bool no_runtime = false)
{
	dxr_plan p;
	dxr_env e = m.env();
	dxr_plan_opts opts = {no_runtime};
	dxr_make_default_plan(&cat.c, &e, &opts, &p);
	return p;
}

// One row of `list --json`, by id. The array stays owned by the caller.
const cJSON *
row(const cJSON *arr, const char *id)
{
	const cJSON *x;
	cJSON_ArrayForEach(x, arr)
	{
		if (strcmp(cJSON_GetObjectItem(x, "id")->valuestring, id) == 0) {
			return x;
		}
	}
	return nullptr;
}

std::string
json(const dxr_plan &p)
{
	char *s = dxr_plan_json(&p);
	std::string out = s;
	cJSON_free(s);
	return out;
}

std::string
text(const dxr_plan &p)
{
	FILE *f = tmpfile();
	REQUIRE(f != nullptr);
	dxr_plan_text(&p, f);
	long n = ftell(f);
	rewind(f);
	std::string out((size_t)n, '\0');
	REQUIRE(fread(&out[0], 1, (size_t)n, f) == (size_t)n);
	fclose(f);
	return out;
}

const dxr_step *
step(const dxr_plan &p, const char *id, dxr_step_kind kind = DXR_STEP_ITEM)
{
	for (int i = 0; i < p.n; i++) {
		if (p.steps[i].kind == kind && strcmp(p.steps[i].id, id) == 0) {
			return &p.steps[i];
		}
	}
	return nullptr;
}

int
index_of(const dxr_plan &p, const char *component)
{
	for (int i = 0; i < p.n; i++) {
		if (strcmp(p.steps[i].component, component) == 0) {
			return i;
		}
	}
	return -1;
}

dxr_receipt
receipt(const char *id, const char *version, const char *sha, const char *requires_ids = "")
{
	dxr_receipt r = {};
	snprintf(r.id, sizeof(r.id), "%s", id);
	snprintf(r.version, sizeof(r.version), "%s", version);
	snprintf(r.sha256, sizeof(r.sha256), "%s", sha);
	snprintf(r.requires_ids, sizeof(r.requires_ids), "%s", requires_ids);
	snprintf(r.uninstall, sizeof(r.uninstall), "\"C:\\x\\uninstall.exe\" /S");
	return r;
}

/*
 * The scan the DisplayXR Browser installer runs over `plan --json`
 * (PlanSummary in its .nsi): from each `"component":"` to the first `}`,
 * read state / target / size. Reproduced here so a shape change that breaks
 * a deployed installer breaks this test first.
 */
struct Scanned
{
	std::string component, state, target;
	long long size = -1;
};

std::string
field(const std::string &obj, const std::string &key)
{
	size_t at = obj.find(key);
	if (at == std::string::npos) {
		return "";
	}
	at += key.size();
	return obj.substr(at, obj.find('"', at) - at);
}

std::vector<Scanned>
installer_scan(const std::string &js)
{
	std::vector<Scanned> out;
	size_t at = 0;
	while ((at = js.find("\"component\":\"", at)) != std::string::npos) {
		std::string obj = js.substr(at, js.find('}', at) - at);
		Scanned s;
		s.component = field(obj, "\"component\":\"");
		s.state = field(obj, "\"state\":\"");
		s.target = field(obj, "\"target\":\"");
		size_t sz = obj.find("\"size\":");
		if (sz != std::string::npos) {
			s.size = atoll(obj.c_str() + sz + 7);
		}
		out.push_back(s);
		at += 13;
	}
	return out;
}

} // namespace

TEST_CASE("catalog: both schemas parse, an unknown or expired one is offline")
{
	Cat v1(V1), v2(V2);
	REQUIRE(v1.rc == DXR_GET_OK);
	REQUIRE(v2.rc == DXR_GET_OK);
	CHECK(v1.c.schema == 1);
	CHECK(v2.c.schema == 2);
	CHECK(v1.c.serial == 100);
	// The document as fetched is kept for `dxr-get catalog`; the model is schema-2 shaped.
	CHECK(cJSON_GetObjectItemCaseSensitive(v1.c.doc, "components") != nullptr);
	CHECK(cJSON_GetObjectItemCaseSensitive(v1.c.model, "items") != nullptr);

	Cat expired(V2, NOW + 90 * 86400);
	CHECK(expired.rc == DXR_GET_OFFLINE);
	CHECK(expired.error.find("expired") != std::string::npos);

	Cat schema3(R"({"schema":3,"expires":"2030-01-01T00:00:00Z","items":[],"publishers":{}})");
	CHECK(schema3.rc == DXR_GET_OFFLINE);
	Cat garbage("<html>captive portal</html>");
	CHECK(garbage.rc == DXR_GET_OFFLINE);
	Cat no_items(R"({"schema":2,"expires":"2030-01-01T00:00:00Z"})");
	CHECK(no_items.rc == DXR_GET_OFFLINE);
}

TEST_CASE("catalog: dates and versions")
{
	CHECK(dxr_parse_utc("1970-01-01T00:00:00Z") == 0);
	CHECK(dxr_parse_utc("2026-11-17T01:12:14Z") == 1794877934); // the spec's serial for that instant
	CHECK(dxr_parse_utc("2024-02-29T12:00:00Z") == 1709208000);
	CHECK(dxr_parse_utc("not a date") == 0);
	CHECK(dxr_vercmp("2.69.3", "2.70.0") < 0);
	CHECK(dxr_vercmp("2.70", "2.70.0") == 0);
	CHECK(dxr_vercmp("2.70.0-3-gabc", "2.70.0") == 0);
	CHECK(dxr_vercmp("10.0", "9.9") > 0);
	CHECK(std::string(dxr_floor_of(">=2.62.0")) == "2.62.0");
}

TEST_CASE("names: schema-1 names keep resolving under schema 2")
{
	Cat v1(V1), v2(V2);
	for (Cat *c : {&v1, &v2}) {
		const cJSON *leia = dxr_item_find(&c->c, "leia_plugin");
		REQUIRE(leia != nullptr);
		CHECK(dxr_item_find(&c->c, "leia-sr") == leia);
		CHECK(std::string(dxr_item_id(leia)) == "leia-sr");
		CHECK(std::string(dxr_item_legacy_name(leia)) == "leia_plugin");
		CHECK(dxr_item_find(&c->c, "runtime") == dxr_runtime_item(&c->c));
		CHECK(dxr_item_find(&c->c, "browser") != nullptr);
		CHECK(dxr_item_find(&c->c, "nope") == nullptr);
	}
	// An item of a class this build does not know is skipped, not guessed at.
	CHECK(dxr_item_find(&v2.c, "future.thing") == nullptr);
	// A listing-only asset is nothing to install.
	CHECK(dxr_item_asset(dxr_item_find(&v2.c, "acme.viewer")) == nullptr);
}

TEST_CASE("ensure-for browser: same plan from schema 1 and schema 2")
{
	for (const char *body : {V1, V2}) {
		Cat cat(body);
		Machine m; // nothing installed, no Leia display
		dxr_plan p = plan(cat, m, "browser", DXR_PLAN_ENSURE);
		REQUIRE(p.n == 1);
		CHECK(std::string(p.steps[0].component) == "runtime");
		CHECK(p.steps[0].state == DXR_ST_MISSING);
		CHECK(std::string(p.steps[0].required) == ">=2.62.0");
		CHECK(std::string(p.steps[0].target) == "2.70.0");
		CHECK(p.gate == DXR_GET_OK);
		CHECK(p.todo == 1);
		CHECK(p.total_size == 50000000);

		// --min-runtime replaces the catalog's floor for X.
		m.sz[RT_VER] = "2.65.0";
		CHECK(plan(cat, m, "browser", DXR_PLAN_ENSURE).steps[0].state == DXR_ST_OK);
		dxr_plan p2 = plan(cat, m, "browser", DXR_PLAN_ENSURE, "2.68.0");
		CHECK(p2.steps[0].state == DXR_ST_TOO_OLD);
		CHECK(std::string(p2.steps[0].required) == ">=2.68.0");

		// A floor the catalog's runtime cannot satisfy: not fetchable (exit 40), as before.
		dxr_plan p3 = plan(cat, m, "browser", DXR_PLAN_ENSURE, "2.99.0");
		CHECK(p3.steps[0].state == DXR_ST_NOT_FETCHABLE);
		CHECK(p3.gate == DXR_GET_NOT_FETCHABLE);
		CHECK(p3.todo == 0);
	}
}

TEST_CASE("ensure-for: a hardware item installs on high-confidence evidence only, and follows the runtime")
{
	for (const char *body : {V1, V2}) {
		Cat cat(body);
		Machine m;
		m.sz[RT_VER] = "2.64.0";

		// Low confidence: reported by detect, never planned.
		m.edids = {{1138, 2}};
		CHECK(plan(cat, m, "browser", DXR_PLAN_ENSURE).n == 1);

		// High confidence: the plug-in is wanted and its own floor joins the runtime's.
		m.edids = {{44048, 16711}};
		dxr_plan p = plan(cat, m, "browser", DXR_PLAN_ENSURE);
		REQUIRE(p.n == 2);
		CHECK(std::string(p.steps[0].component) == "runtime");
		CHECK(p.steps[0].state == DXR_ST_TOO_OLD); // 2.64.0 < the plug-in's 2.66.8
		CHECK(std::string(p.steps[0].required) == ">=2.66.8");
		CHECK(std::string(p.steps[1].component) == "leia_plugin");
		CHECK(std::string(p.steps[1].id) == "leia-sr");
		CHECK(p.steps[1].state == DXR_ST_MISSING);
		CHECK(std::string(p.steps[1].reason).find("DELL_8K32") != std::string::npos);
		CHECK(p.total_size == 80000000);

		// Runtime moves and an older plug-in is present: it is brought to the matched pin.
		m.sz[LEIA_VER] = "2.20.0";
		p = plan(cat, m, "browser", DXR_PLAN_ENSURE);
		CHECK(p.steps[1].state == DXR_ST_TOO_OLD);

		// Runtime stays: the older plug-in is left alone (this is not an updater).
		m.sz[RT_VER] = "2.70.0";
		p = plan(cat, m, "browser", DXR_PLAN_ENSURE);
		CHECK(p.steps[0].state == DXR_ST_OK);
		CHECK(p.steps[1].state == DXR_ST_OK);
		CHECK(p.todo == 0);

		// ensure-for the plug-in itself never plans the plug-in.
		p = plan(cat, m, "leia_plugin", DXR_PLAN_ENSURE);
		REQUIRE(p.n == 1);
		CHECK(std::string(p.steps[0].component) == "runtime");

		// ensure-for runtime: no runtime step, the detected plug-in only when absent.
		m.sz.erase(LEIA_VER);
		p = plan(cat, m, "runtime", DXR_PLAN_ENSURE);
		REQUIRE(p.n == 1);
		CHECK(std::string(p.steps[0].component) == "leia_plugin");
		CHECK(p.steps[0].state == DXR_ST_MISSING);
	}
}

TEST_CASE("plan JSON: the flat shape deployed installers scan")
{
	Cat cat(V2);
	Machine m;
	m.edids = {{44048, 16711}};
	dxr_plan p = plan(cat, m, "browser", DXR_PLAN_ENSURE, "2.62.0");
	std::string js = json(p);
	INFO(js);
	std::vector<Scanned> s = installer_scan(js);
	REQUIRE(s.size() == 2);
	CHECK(s[0].component == "runtime");
	CHECK(s[0].state == "missing");
	CHECK(s[0].target == "2.70.0");
	CHECK(s[0].size == 50000000);
	CHECK(s[1].component == "leia_plugin");
	CHECK(s[1].state == "missing");
	CHECK(s[1].target == "2.23.0");
	CHECK(s[1].size == 30000000);

	// It is JSON, the schema-1 fields are there, and no action nests anything.
	cJSON *o = cJSON_Parse(js.c_str());
	REQUIRE(o != nullptr);
	CHECK(std::string(cJSON_GetObjectItem(o, "for")->valuestring) == "browser");
	CHECK(cJSON_GetObjectItem(o, "total_size")->valuedouble == 80000000);
	const cJSON *a;
	cJSON_ArrayForEach(a, cJSON_GetObjectItem(o, "actions"))
	{
		CHECK(std::string(a->child->string) == "component");
		const cJSON *f;
		cJSON_ArrayForEach(f, a)
		{
			CHECK_FALSE(cJSON_IsObject(f));
			CHECK_FALSE(cJSON_IsArray(f));
		}
		CHECK(cJSON_IsNull(cJSON_GetObjectItem(a, "installed")));
		CHECK(std::string(cJSON_GetObjectItem(a, "action")->valuestring) == "install");
	}
	cJSON_Delete(o);
}

TEST_CASE("skip-if-current: an item at the catalog's version is not downloaded")
{
	Cat cat(V2);
	Machine m;
	m.sz[RT_VER] = "2.70.0";
	dxr_plan p = plan(cat, m, "runtime", DXR_PLAN_INSTALL);
	REQUIRE(p.n == 1);
	CHECK(p.steps[0].state == DXR_ST_OK);
	CHECK(p.steps[0].current);
	CHECK(p.steps[0].hash_match == -1); // no receipt: the hash is not known
	CHECK(p.todo == 0);
	CHECK(p.total_download == 0);
	CHECK(text(p).find("is the catalog's version -> not downloaded") != std::string::npos);
	CHECK(text(p).find("Nothing to install.") != std::string::npos);
	std::string js = json(p);
	CHECK(js.find("\"action\":\"none\"") != std::string::npos);
	CHECK(js.find("\"current\":true") != std::string::npos);
	CHECK(js.find("\"download\":0") != std::string::npos);

	// With a receipt the hash is compared too.
	m.receipts["runtime"] = receipt("runtime", "2.70.0", SHA_RT);
	p = plan(cat, m, "runtime", DXR_PLAN_INSTALL);
	CHECK(p.steps[0].hash_match == 1);
	CHECK(p.todo == 0);
	CHECK(json(p).find("\"hash_match\":true") != std::string::npos);

	// Same version, another build than the one installed: an explicit install re-installs it...
	m.receipts["runtime"] = receipt("runtime", "2.70.0", SHA_LEIA);
	p = plan(cat, m, "runtime", DXR_PLAN_INSTALL);
	CHECK(p.steps[0].state == DXR_ST_TOO_OLD);
	CHECK(p.steps[0].rebuild);
	CHECK(json(p).find("\"action\":\"reinstall\"") != std::string::npos);
	// ...but ensure-for, which is not an updater, leaves it.
	p = plan(cat, m, "browser", DXR_PLAN_ENSURE);
	CHECK(p.steps[0].state == DXR_ST_OK);

	// Newer than the catalog: never downgraded.
	m.receipts.clear();
	m.sz[RT_VER] = "2.71.0";
	p = plan(cat, m, "runtime", DXR_PLAN_INSTALL);
	CHECK(p.steps[0].state == DXR_ST_OK);
	CHECK_FALSE(p.steps[0].current);
	CHECK(p.todo == 0);

	// Older: upgraded, and what the download cache already holds is not fetched again.
	m.sz[RT_VER] = "2.60.0";
	m.cached[SHA_RT] = 20000000;
	p = plan(cat, m, "runtime", DXR_PLAN_INSTALL);
	CHECK(p.steps[0].state == DXR_ST_TOO_OLD);
	CHECK(p.total_size == 50000000);
	CHECK(p.total_download == 30000000);
	CHECK(text(p).find("Already downloaded: 19.1 MB") != std::string::npos);
}

TEST_CASE("requires: prerequisites first, an external prerequisite's asset before its item, total size up front")
{
	Cat cat(V2);
	Machine m; // bare machine
	dxr_plan p = plan(cat, m, "acme.depth-pro", DXR_PLAN_INSTALL);
	INFO(text(p));
	REQUIRE(p.n == 4);
	CHECK(std::string(p.steps[0].component) == "runtime");
	CHECK(std::string(p.steps[0].required) == ">=2.60.0");
	CHECK(std::string(p.steps[1].component) == "open-conversion");
	CHECK(std::string(p.steps[1].required) == ">=0.1.0");
	CHECK(p.steps[2].kind == DXR_STEP_EXTERNAL);
	CHECK(std::string(p.steps[2].component) == "Acme Depth Runtime");
	CHECK(std::string(p.steps[2].target) == "3.2.1");
	CHECK(std::string(p.steps[3].component) == "acme.depth-pro");
	CHECK(p.steps[3].is_target);
	for (int i = 0; i < 4; i++) {
		CHECK(p.steps[i].state == DXR_ST_MISSING);
	}
	CHECK(p.gate == DXR_GET_OK);
	CHECK(p.todo == 4);
	CHECK(p.total_size == 50000000 + 12000000 + 20000000 + 18000000);
	CHECK(text(p).find("Total download: 95.4 MB") != std::string::npos);
	REQUIRE(p.n_prereqs == 1);
	CHECK_FALSE(p.prereqs[0].met);
	CHECK(p.prereqs[0].installable);

	// Prerequisites that are present are left alone; an external one is never upgraded.
	m.sz[RT_VER] = "2.70.0";
	m.sz["HKLM\\Software\\DisplayXR\\ConversionProviders\\open-conversion\\Version"] = "0.1.0";
	m.sz["HKLM\\SOFTWARE\\Acme\\DepthRuntime\\Version"] = "3.2";
	p = plan(cat, m, "acme.depth-pro", DXR_PLAN_INSTALL);
	REQUIRE(p.n == 3); // no external step: it is present
	CHECK(p.steps[0].state == DXR_ST_OK);
	CHECK(p.steps[1].state == DXR_ST_OK); // 0.1.0 satisfies >=0.1.0; not updated to 0.1.1
	CHECK(p.steps[2].state == DXR_ST_MISSING);
	CHECK(p.todo == 1);
	CHECK(p.total_size == 18000000);
	CHECK(p.prereqs[0].met);
	CHECK(text(p).find("Prerequisite present: Acme Depth Runtime") != std::string::npos);

	// Too old an external runtime is "absent" to its detect rule: installed first.
	m.sz["HKLM\\SOFTWARE\\Acme\\DepthRuntime\\Version"] = "3.1";
	p = plan(cat, m, "acme.depth-pro", DXR_PLAN_INSTALL);
	REQUIRE(p.n == 4);
	CHECK(index_of(p, "Acme Depth Runtime") < index_of(p, "acme.depth-pro"));

	// A prerequisite item below its floor is upgraded first.
	m.sz["HKLM\\Software\\DisplayXR\\ConversionProviders\\open-conversion\\Version"] = "0.0.9";
	p = plan(cat, m, "acme.depth-pro", DXR_PLAN_INSTALL);
	CHECK(step(p, "open-conversion")->state == DXR_ST_TOO_OLD);

	// `ensure-for X` installs X's prerequisites (and its external asset), never X.
	p = plan(cat, m, "acme.depth-pro", DXR_PLAN_ENSURE);
	CHECK(index_of(p, "acme.depth-pro") == -1);
	CHECK(index_of(p, "open-conversion") >= 0);
	CHECK(index_of(p, "Acme Depth Runtime") >= 0);
}

TEST_CASE("requires: a detection-only external prerequisite is reported, never installed")
{
	Cat cat(V2);
	Machine m;
	m.sz[RT_VER] = "2.69.0";
	dxr_plan p = plan(cat, m, "handco.hands", DXR_PLAN_INSTALL);
	REQUIRE(p.n == 2);
	CHECK(p.steps[1].state == DXR_ST_MISSING); // the item still installs
	CHECK(p.gate == DXR_GET_OK);
	REQUIRE(p.n_prereqs == 1);
	CHECK_FALSE(p.prereqs[0].met);
	CHECK_FALSE(p.prereqs[0].installable);
	CHECK(text(p).find("Prerequisite MISSING: Handco Tracking Service") != std::string::npos);
	CHECK(text(p).find("https://handco.example/tracking-service") != std::string::npos);
	CHECK(json(p).find("\"met\":false") != std::string::npos);

	// Either detector satisfies it: the registry key, or the installed service.
	m.services.insert("HandcoTracking");
	CHECK(plan(cat, m, "handco.hands", DXR_PLAN_INSTALL).prereqs[0].met);
	m.services.clear();
	m.keys.insert("HKLM\\SOFTWARE\\Handco\\TrackingService");
	CHECK(plan(cat, m, "handco.hands", DXR_PLAN_INSTALL).prereqs[0].met);
}

TEST_CASE("requires: an unknown item or a cycle fails the plan instead of looping")
{
	Cat cat(R"json({
 "schema": 2, "serial": 1, "expires": "2026-12-10T00:00:00Z", "publishers": { "p": { "name": "P" } },
 "items": [
  { "id": "a", "class": "app", "publisher": "p", "version": "1.0.0", "requires": [ { "kind": "item", "id": "b" } ] },
  { "id": "b", "class": "app", "publisher": "p", "version": "1.0.0", "requires": [ { "kind": "item", "id": "a" } ] },
  { "id": "c", "class": "app", "publisher": "p", "version": "1.0.0", "requires": [ { "kind": "item", "id": "ghost" } ] }
 ]
})json");
	REQUIRE(cat.rc == DXR_GET_OK);
	Machine m;
	dxr_plan p = plan(cat, m, "a", DXR_PLAN_INSTALL);
	CHECK(p.gate == DXR_GET_NOT_FETCHABLE);
	CHECK(std::string(p.gate_reason).find("cycle") != std::string::npos);
	p = plan(cat, m, "c", DXR_PLAN_INSTALL);
	CHECK(p.gate == DXR_GET_NOT_FETCHABLE);
	CHECK(std::string(p.gate_reason).find("ghost") != std::string::npos);
	p = plan(cat, m, "nope", DXR_PLAN_INSTALL);
	CHECK(p.gate == DXR_GET_NOT_FETCHABLE);
	// An installer may ask for an item the catalog does not list yet (the Leia
	// plug-in's runs `install leia_conversion --yes`) and reads exit 40 as "not
	// in the catalog yet". Both schemas answer that way, and nothing is planned.
	Cat v1(V1), v2(V2);
	for (Cat *c : {&v1, &v2}) {
		p = plan(*c, m, "leia_conversion", DXR_PLAN_INSTALL);
		CHECK(p.gate == DXR_GET_NOT_FETCHABLE);
		CHECK(p.n == 0);
		CHECK(p.todo == 0);
	}
}

TEST_CASE("gating: the runtime range")
{
	Cat cat(V2);
	Machine m;

	// Inside [2.60.0, 2.69.99]: fine.
	m.sz[RT_VER] = "2.69.3";
	dxr_plan p = plan(cat, m, "handco.hands", DXR_PLAN_INSTALL);
	CHECK(p.gate == DXR_GET_OK);
	CHECK(step(p, "handco.hands")->state == DXR_ST_MISSING);

	// Above the maximum: refused with its own code, and nothing is planned for it.
	m.sz[RT_VER] = "2.70.0";
	p = plan(cat, m, "handco.hands", DXR_PLAN_INSTALL);
	CHECK(p.gate == DXR_GET_INCOMPATIBLE);
	CHECK(step(p, "handco.hands")->state == DXR_ST_INCOMPATIBLE);
	CHECK(p.todo == 0);
	CHECK(std::string(p.gate_reason).find("2.69.99") != std::string::npos);
	CHECK(std::string(p.gate_reason).find("2.70.0") != std::string::npos);
	CHECK(text(p).find("Not compatible:") != std::string::npos);
	CHECK(json(p).find("\"gate\":41") != std::string::npos);
	CHECK(json(p).find("\"state\":\"incompatible\"") != std::string::npos);

	// No runtime: the plan would install the catalog's 2.70.0, which is above the
	// maximum. The runtime is never downgraded (or held back) to fit.
	m.sz.erase(RT_VER);
	p = plan(cat, m, "handco.hands", DXR_PLAN_INSTALL);
	CHECK(p.gate == DXR_GET_INCOMPATIBLE);
	CHECK(std::string(p.gate_reason).find("would get 2.70.0") != std::string::npos);

	// Below an item's `runtime.min` that the catalog's runtime cannot reach: 41, not 40.
	Cat far(R"json({
 "schema": 2, "serial": 1, "expires": "2026-12-10T00:00:00Z", "publishers": { "p": { "name": "P" } },
 "items": [
  { "id": "runtime", "class": "runtime", "publisher": "p", "version": "2.70.0" },
  { "id": "next", "class": "app", "publisher": "p", "version": "1.0.0", "runtime": { "min": "3.0.0" } }
 ]
})json");
	m.sz[RT_VER] = "2.70.0";
	p = plan(far, m, "next", DXR_PLAN_INSTALL);
	CHECK(p.gate == DXR_GET_INCOMPATIBLE);
	CHECK(std::string(p.gate_reason).find("3.0.0") != std::string::npos);
	// The same refusal reaches an installer that calls ensure-for for it.
	p = plan(far, m, "next", DXR_PLAN_ENSURE);
	CHECK(p.gate == DXR_GET_INCOMPATIBLE);

	// A detected hardware item that is out of range is skipped; it does not fail X.
	Cat hw(R"json({
 "schema": 2, "serial": 1, "expires": "2026-12-10T00:00:00Z", "publishers": { "p": { "name": "P" } },
 "items": [
  { "id": "runtime", "class": "runtime", "publisher": "p", "version": "2.70.0" },
  { "id": "x", "class": "app", "publisher": "p", "version": "1.0.0" },
  { "id": "old-dp", "class": "display_processor", "registered_id": "old-dp", "publisher": "p", "version": "1.0.0",
    "runtime": { "max": "2.50.0" }, "detect": [ { "kind": "file_exists", "path": "C:\\Panel\\present.bin", "confidence": "high" } ] }
 ]
})json");
	m.files.insert("C:\\Panel\\present.bin");
	p = plan(hw, m, "x", DXR_PLAN_ENSURE);
	CHECK(p.gate == DXR_GET_OK);
	REQUIRE(step(p, "old-dp") != nullptr);
	CHECK(step(p, "old-dp")->state == DXR_ST_INCOMPATIBLE);
	CHECK(p.todo == 0);
}

TEST_CASE("gating: the class ABI")
{
	Cat cat(V2);
	Machine m;
	m.sz[RT_VER] = "2.69.0";

	// The installed runtime does not publish its ABI: not checked, not refused, and said so.
	dxr_plan p = plan(cat, m, "open-conversion", DXR_PLAN_INSTALL);
	CHECK(p.gate == DXR_GET_OK);
	CHECK(std::string(step(p, "open-conversion")->reason).find("ABI not checked") != std::string::npos);

	// Abi values with no stamp saying which runtime wrote them are not trusted.
	m.dword["HKLM\\SOFTWARE\\DisplayXR\\Runtime\\Abi|conversion_provider"] = 2;
	p = plan(cat, m, "open-conversion", DXR_PLAN_INSTALL);
	CHECK(p.gate == DXR_GET_OK);
	CHECK(std::string(step(p, "open-conversion")->reason).find("ABI not checked") != std::string::npos);

	// It publishes a matching major, stamped with the installed version: fine.
	m.sz[ABI_STAMP] = "2.69.0";
	m.dword["HKLM\\SOFTWARE\\DisplayXR\\Runtime\\Abi|conversion_provider"] = 1;
	p = plan(cat, m, "open-conversion", DXR_PLAN_INSTALL);
	CHECK(p.gate == DXR_GET_OK);
	CHECK(std::string(step(p, "open-conversion")->reason).find("ABI not checked") == std::string::npos);

	// It publishes another major: the loader would reject the plug-in, so it is not installed.
	m.dword["HKLM\\SOFTWARE\\DisplayXR\\Runtime\\Abi|conversion_provider"] = 2;
	p = plan(cat, m, "open-conversion", DXR_PLAN_INSTALL);
	CHECK(p.gate == DXR_GET_INCOMPATIBLE);
	CHECK(step(p, "open-conversion")->state == DXR_ST_INCOMPATIBLE);
	CHECK(std::string(p.gate_reason).find("conversion_provider ABI 1") != std::string::npos);
	CHECK(p.todo == 0);

	// An older runtime was installed over the one that wrote the values (it does
	// not know the key and leaves it behind): the stamp no longer names the
	// installed version, so the stale values are ignored -- not checked, not refused.
	m.sz[RT_VER] = "2.68.0";
	p = plan(cat, m, "open-conversion", DXR_PLAN_INSTALL);
	CHECK(p.gate == DXR_GET_OK);
	CHECK(step(p, "open-conversion")->state == DXR_ST_MISSING);
	CHECK(std::string(step(p, "open-conversion")->reason).find("ABI not checked") != std::string::npos);
	// The same holds for `list`.
	{
		dxr_env e = m.env();
		cJSON *all = dxr_list_catalog(&cat.c, &e, false, false);
		CHECK(cJSON_IsTrue(cJSON_GetObjectItem(row(all, "open-conversion"), "compatible")));
		cJSON_Delete(all);
	}
	// Re-installing the runtime that wrote them makes them count again.
	m.sz[RT_VER] = "2.69.0";
	CHECK(plan(cat, m, "open-conversion", DXR_PLAN_INSTALL).gate == DXR_GET_INCOMPATIBLE);

	// The runtime is being replaced in this plan: the installed runtime's ABI no
	// longer matters, the catalog's runtime item says what the new one implements.
	m.sz[RT_VER] = "2.50.0";
	p = plan(cat, m, "acme.depth-pro", DXR_PLAN_INSTALL); // floor 2.60.0 -> runtime moves to 2.70.0
	CHECK(step(p, "runtime")->state == DXR_ST_TOO_OLD);
	CHECK(p.gate == DXR_GET_OK); // 2.70.0 implements conversion_provider 1
}

TEST_CASE("trust: every asset is signed by a signer delegated to its own publisher")
{
	Cat cat(V2);
	char why[256];
	const cJSON *acme = dxr_item_find(&cat.c, "acme.depth-pro");
	const cJSON *asset = dxr_item_asset(acme);
	REQUIRE(asset != nullptr);

	// The catalog side alone, then with the certificate subject the file really carries.
	CHECK(dxr_asset_check(&cat.c, acme, asset, nullptr, why, sizeof(why)) == DXR_GET_OK);
	CHECK(dxr_asset_check(&cat.c, acme, asset, "Acme Displays, Inc.", why, sizeof(why)) == DXR_GET_OK);
	CHECK(dxr_asset_check(&cat.c, acme, asset, "Leia, Inc.", why, sizeof(why)) == DXR_GET_VERIFY);
	CHECK(dxr_asset_check(&cat.c, acme, asset, "Acme Displays, Inc", why, sizeof(why)) == DXR_GET_VERIFY);

	// Another publisher's signer cannot vouch for this item, even a first-party one.
	cJSON *copy = cJSON_Duplicate(asset, 1);
	cJSON_ReplaceItemInObject(cJSON_GetObjectItem(copy, "signature"), "signer", cJSON_CreateString("Leia, Inc."));
	CHECK(dxr_asset_check(&cat.c, acme, copy, "Leia, Inc.", why, sizeof(why)) == DXR_GET_VERIFY);
	CHECK(std::string(why).find("not delegated to publisher \"acme\"") != std::string::npos);

	// No signature: nothing installable. There is no hash-pinned-only form.
	cJSON_DeleteItemFromObject(copy, "signature");
	CHECK(dxr_asset_check(&cat.c, acme, copy, nullptr, why, sizeof(why)) == DXR_GET_NOT_FETCHABLE);
	CHECK(std::string(why).find("not signed") != std::string::npos);

	// An unknown signature kind is refused, not skipped.
	cJSON *sig = cJSON_AddObjectToObject(copy, "signature");
	cJSON_AddStringToObject(sig, "kind", "ed25519");
	cJSON_AddStringToObject(sig, "key_id", "1a2b3c4d");
	CHECK(dxr_asset_check(&cat.c, acme, copy, nullptr, why, sizeof(why)) == DXR_GET_VERIFY);
	cJSON_Delete(copy);

	// A revoked digest is refused whoever signed it.
	copy = cJSON_Duplicate(asset, 1);
	cJSON_ReplaceItemInObject(
	    copy, "sha256", cJSON_CreateString("9999999999999999999999999999999999999999999999999999999999999999"));
	CHECK(dxr_asset_check(&cat.c, acme, copy, nullptr, why, sizeof(why)) == DXR_GET_VERIFY);
	CHECK(std::string(why).find("revoked: bad build") != std::string::npos);
	cJSON_Delete(copy);

	// Malformed entries: a path in the file name, a plain-http URL, an installer kind this build does not run.
	copy = cJSON_Duplicate(asset, 1);
	cJSON_ReplaceItemInObject(copy, "file", cJSON_CreateString("..\\evil.exe"));
	CHECK(dxr_asset_check(&cat.c, acme, copy, nullptr, why, sizeof(why)) == DXR_GET_NOT_FETCHABLE);
	cJSON_Delete(copy);
	copy = cJSON_Duplicate(asset, 1);
	cJSON_ReplaceItemInObject(copy, "urls", cJSON_Parse("[\"http://example.invalid/a.exe\"]"));
	CHECK(dxr_asset_check(&cat.c, acme, copy, nullptr, why, sizeof(why)) == DXR_GET_NOT_FETCHABLE);
	cJSON_Delete(copy);
	copy = cJSON_Duplicate(asset, 1);
	cJSON_ReplaceItemInObject(cJSON_GetObjectItem(copy, "installer"), "kind", cJSON_CreateString("msi"));
	CHECK(dxr_asset_check(&cat.c, acme, copy, nullptr, why, sizeof(why)) == DXR_GET_NOT_FETCHABLE);
	cJSON_Delete(copy);

	// The external prerequisite's asset is held to the same rule, against the ITEM's publisher.
	const cJSON *ext = cJSON_GetArrayItem(cJSON_GetObjectItem(acme, "requires"), 2);
	const cJSON *ext_asset = cJSON_GetObjectItem(cJSON_GetObjectItem(ext, "asset"), "windows-x64");
	CHECK(dxr_asset_check(&cat.c, acme, ext_asset, "Acme Displays, Inc.", why, sizeof(why)) == DXR_GET_OK);
	CHECK(dxr_asset_check(&cat.c, acme, ext_asset, "Handco Ltd", why, sizeof(why)) == DXR_GET_VERIFY);

	// Schema 1: the compiled-in signer list is the delegation.
	Cat v1(V1);
	const cJSON *rt = dxr_runtime_item(&v1.c);
	CHECK(dxr_asset_check(&v1.c, rt, dxr_item_asset(rt), "Leia, Inc.", why, sizeof(why)) == DXR_GET_OK);
	copy = cJSON_Duplicate(dxr_item_asset(rt), 1);
	cJSON_ReplaceItemInObject(cJSON_GetObjectItem(copy, "signature"), "signer", cJSON_CreateString("Mallory LLC"));
	CHECK(dxr_asset_check(&v1.c, rt, copy, "Mallory LLC", why, sizeof(why)) == DXR_GET_VERIFY);
	cJSON_Delete(copy);
}

TEST_CASE("trust: an unsigned or undelegated asset is not planned")
{
	Cat cat(R"json({
 "schema": 2, "serial": 1, "expires": "2026-12-10T00:00:00Z",
 "publishers": { "nosigner": { "name": "No Signer" } },
 "items": [
  { "id": "unsigned", "class": "app", "publisher": "nosigner", "version": "1.0.0",
    "assets": { "windows-x64": { "version": "1.0.0", "file": "a.exe", "size": 10,
      "sha256": ")json" SHA_ACME R"json(", "urls": ["https://example.invalid/a.exe"],
      "installer": { "kind": "exe", "args": [], "scope": "machine", "elevation": "required" },
      "installed": { "kind": "registry_sz", "path": "HKLM\\Software\\X\\Version" } } } },
  { "id": "undelegated", "class": "app", "publisher": "nosigner", "version": "1.0.0",
    "assets": { "windows-x64": { "version": "1.0.0", "file": "b.exe", "size": 10,
      "sha256": ")json" SHA_ACME R"json(", "urls": ["https://example.invalid/b.exe"],
      "signature": { "kind": "authenticode", "signer": "Somebody" },
      "installer": { "kind": "exe", "args": [], "scope": "machine", "elevation": "required" },
      "installed": { "kind": "registry_sz", "path": "HKLM\\Software\\Y\\Version" } } } }
 ]
})json");
	REQUIRE(cat.rc == DXR_GET_OK);
	Machine m;
	for (const char *id : {"unsigned", "undelegated"}) {
		dxr_plan p = plan(cat, m, id, DXR_PLAN_INSTALL);
		REQUIRE(p.n == 1);
		CHECK(p.steps[0].state == DXR_ST_NOT_FETCHABLE);
		CHECK(p.gate == DXR_GET_NOT_FETCHABLE);
		CHECK(p.todo == 0);
	}
}

TEST_CASE("install: a link-out item is reported with its listing, never fetched")
{
	Cat cat(V2);
	Machine m;
	dxr_plan p = plan(cat, m, "acme.viewer", DXR_PLAN_INSTALL);
	REQUIRE(p.n == 1);
	CHECK(p.steps[0].state == DXR_ST_NOT_FETCHABLE);
	CHECK(std::string(p.steps[0].reason).find("https://acme.example/viewer") != std::string::npos);
	CHECK(p.gate == DXR_GET_NOT_FETCHABLE);
}

TEST_CASE("detect rules: every kind, and an unknown kind is not a match")
{
	Machine m;
	dxr_env e = m.env();
	char why[256];
	auto match = [&](const char *rules, bool high_only = false) {
		cJSON *r = cJSON_Parse(rules);
		REQUIRE(r != nullptr);
		bool ok = dxr_detect_rules(r, &e, high_only, why, sizeof(why));
		cJSON_Delete(r);
		return ok;
	};
	CHECK_FALSE(match(R"([{"kind":"edid","man":1,"prod":2,"confidence":"high"}])"));
	m.edids = {{1, 2}};
	CHECK(match(R"([{"kind":"edid","man":1,"prod":2,"confidence":"high"}])", true));
	CHECK_FALSE(match(R"([{"kind":"edid","man":1,"prod":2,"confidence":"low"}])", true));

	CHECK_FALSE(match(R"([{"kind":"registry_dword_min","key":"HKLM\\K","value":"N","min":1}])"));
	m.dword["HKLM\\K|N"] = 1;
	CHECK(match(R"([{"kind":"registry_dword_min","key":"HKLM\\K","value":"N","min":1}])"));
	CHECK_FALSE(match(R"([{"kind":"registry_dword_min","key":"HKLM\\K","value":"N","min":2}])"));

	m.sz["HKLM\\K\\Version"] = "3.10";
	CHECK(match(R"([{"kind":"registry_sz_min","key":"HKLM\\K","value":"Version","min":"3.2"}])"));
	CHECK_FALSE(match(R"([{"kind":"registry_sz_min","key":"HKLM\\K","value":"Version","min":"3.11"}])"));

	m.keys.insert("HKLM\\K");
	CHECK(match(R"([{"kind":"registry_key_exists","key":"HKLM\\K"}])"));
	CHECK_FALSE(match(R"([{"kind":"registry_key_exists","key":"HKLM\\Other"}])"));

	// file_exists: an absolute path only, no environment expansion.
	m.files = {"C:\\Program Files\\Acme\\a.dll", "%ProgramFiles%\\Acme\\a.dll", "a.dll"};
	CHECK(match(R"([{"kind":"file_exists","path":"C:\\Program Files\\Acme\\a.dll"}])"));
	CHECK_FALSE(match(R"([{"kind":"file_exists","path":"%ProgramFiles%\\Acme\\a.dll"}])"));
	CHECK_FALSE(match(R"([{"kind":"file_exists","path":"a.dll"}])"));

	m.services.insert("AcmeSvc");
	CHECK(match(R"([{"kind":"service_exists","name":"AcmeSvc"}])"));
	CHECK_FALSE(match(R"([{"kind":"service_exists","name":"Other"}])"));

	CHECK_FALSE(match(R"([{"kind":"telepathy","confidence":"high"}])"));
	CHECK_FALSE(match(R"({"not":"an array"})"));
}

TEST_CASE("detect rules (#2737): edid product range, usb VID:PID, vendor-only rules are never high")
{
	Machine m;
	dxr_env e = m.env();
	char why[256];
	auto match = [&](const char *rules, bool high_only = false) {
		cJSON *r = cJSON_Parse(rules);
		REQUIRE(r != nullptr);
		bool ok = dxr_detect_rules(r, &e, high_only, why, sizeof(why));
		cJSON_Delete(r);
		return ok;
	};
	m.edids = {{44048, 16711}};

	// edid product range: inclusive, high when the catalog says so.
	CHECK(match(R"([{"kind":"edid","man":44048,"prod_min":16700,"prod_max":16711,"confidence":"high"}])", true));
	CHECK(std::string(why).find("high confidence") != std::string::npos);
	CHECK(match(R"([{"kind":"edid","man":44048,"prod_min":16711,"prod_max":16720,"confidence":"high"}])", true));
	CHECK_FALSE(match(R"([{"kind":"edid","man":44048,"prod_min":16712,"prod_max":16720,"confidence":"high"}])"));
	CHECK_FALSE(match(R"([{"kind":"edid","man":44049,"prod_min":0,"prod_max":65535,"confidence":"high"}])"));
	// Malformed: inverted range, half a range, exact + range, out of 16 bits.
	CHECK_FALSE(match(R"([{"kind":"edid","man":44048,"prod_min":16720,"prod_max":16700,"confidence":"high"}])"));
	CHECK_FALSE(match(R"([{"kind":"edid","man":44048,"prod_min":16700,"confidence":"high"}])"));
	CHECK_FALSE(match(R"([{"kind":"edid","man":44048,"prod":16711,"prod_max":16720,"confidence":"high"}])"));
	CHECK_FALSE(match(R"([{"kind":"edid","man":44048,"prod":80000,"confidence":"high"}])"));
	// Not an integer (#2742): 16711.9 is malformed, not truncated to 16711.
	CHECK_FALSE(match(R"([{"kind":"edid","man":44048,"prod":16711.9,"confidence":"high"}])"));
	CHECK_FALSE(match(R"([{"kind":"edid","man":44048.5,"prod":16711,"confidence":"high"}])"));
	CHECK_FALSE(match(R"([{"kind":"edid","man":44048,"prod_min":16700.5,"prod_max":16711,"confidence":"high"}])"));
	CHECK_FALSE(match(R"([{"kind":"edid","man":44048,"prod_min":16700,"prod_max":16711.2,"confidence":"high"}])"));
	CHECK(match(R"([{"kind":"edid","man":44048,"prod":16711.0,"confidence":"high"}])", true));

	// Manufacturer only: matches, but never as high confidence (Dell ships 2D monitors).
	CHECK(match(R"([{"kind":"edid","man":44048,"confidence":"high"}])"));
	CHECK(std::string(why).find("low confidence") != std::string::npos);
	CHECK_FALSE(match(R"([{"kind":"edid","man":44048,"confidence":"high"}])", true));

	// usb: exact, range, vendor-only.
	CHECK_FALSE(match(R"([{"kind":"usb","vid":11012,"pid":7,"confidence":"high"}])"));
	m.usb = {{0x046D, 0xC34B}, {11012, 7}};
	CHECK(match(R"([{"kind":"usb","vid":11012,"pid":7,"confidence":"high","note":"tracking camera"}])", true));
	CHECK(std::string(why) == "USB device 2B04:0007 tracking camera (high confidence)");
	CHECK(match(R"([{"kind":"usb","vid":11012,"pid_min":1,"pid_max":9,"confidence":"high"}])", true));
	CHECK_FALSE(match(R"([{"kind":"usb","vid":11012,"pid":8,"confidence":"high"}])"));
	CHECK(match(R"([{"kind":"usb","vid":1133,"confidence":"high"}])"));
	CHECK_FALSE(match(R"([{"kind":"usb","vid":1133,"confidence":"high"}])", true));
	CHECK_FALSE(match(R"([{"kind":"usb","pid":7,"confidence":"high"}])"));
	CHECK_FALSE(match(R"([{"kind":"usb","vid":11012,"pid":7.5,"confidence":"high"}])"));
	CHECK_FALSE(match(R"([{"kind":"usb","vid":11012.1,"pid":7,"confidence":"high"}])"));
	CHECK_FALSE(match(R"([{"kind":"usb","vid":11012,"pid_min":1,"pid_max":9.9,"confidence":"high"}])"));

	// An unknown kind before a matching rule does not stop evaluation.
	const char *unknown_then_edid =
	    R"([{"kind":"telepathy","confidence":"high"},{"kind":"edid","man":44048,"prod":16711,"confidence":"high"}])";
	CHECK(match(unknown_then_edid, true));
}

TEST_CASE("installed version: the catalog's rule, the registration row, then the receipt")
{
	Cat cat(V2);
	Machine m;
	dxr_env e = m.env();
	char v[64], path[512];
	const cJSON *open = dxr_item_find(&cat.c, "open-conversion");
	REQUIRE(dxr_installed_path(open, path, sizeof(path)));
	CHECK(std::string(path) == "HKLM\\Software\\DisplayXR\\ConversionProviders\\open-conversion\\Version");
	CHECK_FALSE(dxr_installed_version(&e, open, v, sizeof(v)));
	m.sz[path] = "0.1.1";
	CHECK(dxr_installed_version(&e, open, v, sizeof(v)));
	CHECK(std::string(v) == "0.1.1");

	// A link-out app has no rule: only a receipt can say it is installed.
	const cJSON *viewer = dxr_item_find(&cat.c, "acme.viewer");
	CHECK_FALSE(dxr_installed_path(viewer, path, sizeof(path)));
	CHECK_FALSE(dxr_installed_version(&e, viewer, v, sizeof(v)));
	m.receipts["acme.viewer"] = receipt("acme.viewer", "3.0.0", "");
	CHECK(dxr_installed_version(&e, viewer, v, sizeof(v)));

	CHECK(dxr_class_is_loaded("display_processor"));
	CHECK(dxr_class_is_loaded("runtime"));
	CHECK_FALSE(dxr_class_is_loaded("app"));
}

TEST_CASE("inventory: list, outdated and receipts")
{
	Cat cat(V2);
	Machine m;
	m.sz[RT_VER] = "2.69.3";
	m.sz[LEIA_VER] = "2.23.0";
	m.sz["HKLM\\Software\\DisplayXR\\ConversionProviders\\open-conversion\\Version"] = "0.1.1";
	m.receipts["leia-sr"] = receipt("leia-sr", "2.23.0", SHA_LEIA, "runtime");
	m.receipts["open-conversion"] = receipt("open-conversion", "0.1.1", SHA_ACME); // another build
	m.receipts["open-conversion"].uninstall[0] = '\0'; // installed from an entry that names no uninstall command
	dxr_env e = m.env();

	auto state_of = [](const cJSON *arr, const char *id) -> std::string {
		const cJSON *x;
		cJSON_ArrayForEach(x, arr)
		{
			if (strcmp(cJSON_GetObjectItem(x, "id")->valuestring, id) == 0) {
				return cJSON_GetObjectItem(x, "state")->valuestring;
			}
		}
		return "(absent)";
	};

	cJSON *all = dxr_list_catalog(&cat.c, &e, false, false);
	CHECK(cJSON_GetArraySize(all) == 7); // the unknown-class item is not listed
	CHECK(state_of(all, "runtime") == "outdated");
	CHECK(state_of(all, "leia-sr") == "current");
	CHECK(state_of(all, "open-conversion") == "rebuilt");
	CHECK(state_of(all, "browser") == "not_installed");
	CHECK(state_of(all, "acme.viewer") == "not_installed");
	CHECK(state_of(all, "future.thing") == "(absent)");
	const cJSON *x;
	cJSON_ArrayForEach(x, all)
	{
		const std::string id = cJSON_GetObjectItem(x, "id")->valuestring;
		if (id == "acme.viewer") {
			CHECK(cJSON_IsFalse(cJSON_GetObjectItem(x, "installable")));
			CHECK(std::string(cJSON_GetObjectItem(x, "listing")->valuestring) ==
			      "https://acme.example/viewer");
		}
		if (id == "acme.depth-pro") {
			CHECK(std::string(cJSON_GetObjectItem(x, "tier")->valuestring) == "verified");
		}
		if (id == "runtime") {
			CHECK(std::string(cJSON_GetObjectItem(x, "tier")->valuestring) == "listed"); // absent = listed
			CHECK(cJSON_IsFalse(cJSON_GetObjectItem(x, "receipt")));
			CHECK(cJSON_IsFalse(cJSON_GetObjectItem(x, "can_uninstall"))); // no receipt: nothing to run
		}
		// `can_uninstall`: the receipt recorded an uninstall command. A receipt alone is not enough.
		if (id == "leia-sr") {
			CHECK(cJSON_IsTrue(cJSON_GetObjectItem(x, "receipt")));
			CHECK(cJSON_IsTrue(cJSON_GetObjectItem(x, "can_uninstall")));
		}
		if (id == "open-conversion") {
			CHECK(cJSON_IsTrue(cJSON_GetObjectItem(x, "receipt")));
			CHECK(cJSON_IsFalse(cJSON_GetObjectItem(x, "can_uninstall")));
		}
		if (id == "browser") {
			CHECK(cJSON_IsFalse(cJSON_GetObjectItem(x, "can_uninstall")));
		}
	}
	cJSON_Delete(all);

	cJSON *out = dxr_list_catalog(&cat.c, &e, true, false);
	CHECK(cJSON_GetArraySize(out) == 2);
	CHECK(state_of(out, "runtime") == "outdated");
	CHECK(state_of(out, "open-conversion") == "rebuilt");
	cJSON_Delete(out);

	// A newer runtime than the catalog's is not "outdated".
	m.sz[RT_VER] = "2.71.0";
	out = dxr_list_catalog(&cat.c, &e, true, false);
	CHECK(state_of(out, "runtime") == "(absent)");
	cJSON_Delete(out);

	// `list --installed` is the receipts, with no catalog.
	dxr_receipt rs[3] = {receipt("runtime", "2.70.0", SHA_RT), receipt("leia-sr", "2.23.0", SHA_LEIA, "runtime"),
	                     receipt("acme.depth-pro", "1.4.0", SHA_ACME, "runtime,open-conversion")};
	rs[0].uninstall[0] = '\0'; // the catalog gave no uninstall command for it
	// Where each keeps its version: the runtime's is there, the plug-in's is gone (removed by
	// its own uninstaller, the receipt left behind), the third receipt does not say.
	(void)snprintf(rs[0].version_path, sizeof(rs[0].version_path), "%s", RT_VER);
	(void)snprintf(rs[1].version_path, sizeof(rs[1].version_path), "%s",
	               "HKLM\\Software\\DisplayXR\\Plugins\\Gone\\Version");
	cJSON *inst = dxr_list_receipts(&e, rs, 3);
	REQUIRE(cJSON_GetArraySize(inst) == 3);
	CHECK(std::string(cJSON_GetObjectItem(cJSON_GetArrayItem(inst, 0), "installed")->valuestring) == "2.71.0");
	CHECK(std::string(cJSON_GetObjectItem(cJSON_GetArrayItem(inst, 0), "version")->valuestring) == "2.70.0");
	CHECK(cJSON_IsNull(cJSON_GetObjectItem(cJSON_GetArrayItem(inst, 1), "installed")));
	CHECK(cJSON_GetObjectItem(cJSON_GetArrayItem(inst, 2), "installed") == nullptr);
	CHECK(cJSON_IsFalse(cJSON_GetObjectItem(cJSON_GetArrayItem(inst, 0), "can_uninstall")));
	CHECK(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetArrayItem(inst, 1), "can_uninstall")));
	CHECK(std::string(cJSON_GetObjectItem(cJSON_GetArrayItem(inst, 1), "sha256")->valuestring) == SHA_LEIA);
	cJSON_Delete(inst);

	// Uninstalling something another installed item required is refused.
	char deps[256];
	CHECK(dxr_receipt_dependents(rs, 3, "runtime", deps, sizeof(deps)) == 2);
	CHECK(std::string(deps) == "leia-sr, acme.depth-pro");
	CHECK(dxr_receipt_dependents(rs, 3, "open-conversion", deps, sizeof(deps)) == 1);
	CHECK(dxr_receipt_dependents(rs, 3, "leia-sr", deps, sizeof(deps)) == 0);
	CHECK(dxr_receipt_dependents(rs, 3, "open", deps, sizeof(deps)) == 0); // a prefix is not a match
}

TEST_CASE("list: one call answers a setup page")
{
	Cat cat(V2);
	Machine m;
	m.sz[RT_VER] = "2.69.3";
	m.edids = {{44048, 16711}};
	m.cached[SHA_ACME] = 8000000;
	dxr_env e = m.env();
	cJSON *all = dxr_list_catalog(&cat.c, &e, false, false);

	// The fields that were there before are still there, unchanged.
	const cJSON *leia = row(all, "leia-sr");
	REQUIRE(leia != nullptr);
	for (const char *k : {"id", "name", "class", "publisher", "tier", "installed", "available", "state",
	                      "installable", "receipt", "can_uninstall"}) {
		INFO(k);
		CHECK(cJSON_GetObjectItem(leia, k) != nullptr);
	}
	CHECK(std::string(cJSON_GetObjectItem(leia, "state")->valuestring) == "not_installed");

	// Hardware match, the schema-1 name, and what installing it would fetch.
	CHECK(std::string(cJSON_GetObjectItem(leia, "detected")->valuestring) == "high");
	CHECK(std::string(cJSON_GetObjectItem(leia, "evidence")->valuestring).find("DELL_8K32") != std::string::npos);
	CHECK(std::string(cJSON_GetArrayItem(cJSON_GetObjectItem(leia, "aliases"), 0)->valuestring) == "leia_plugin");
	CHECK(cJSON_IsFalse(cJSON_GetObjectItem(leia, "recommended")));
	CHECK(cJSON_GetObjectItem(leia, "size")->valuedouble == 30000000);
	CHECK(cJSON_GetObjectItem(leia, "download")->valuedouble == 30000000);
	CHECK(cJSON_IsTrue(cJSON_GetObjectItem(leia, "compatible")));
	CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(leia, "prerequisites")) == 0);

	// The recommended flag and the summary come from the catalog.
	const cJSON *open = row(all, "open-conversion");
	CHECK(cJSON_IsTrue(cJSON_GetObjectItem(open, "recommended")));
	CHECK(std::string(cJSON_GetObjectItem(open, "summary")->valuestring) ==
	      "2D to 3D conversion with open models.");
	CHECK(std::string(cJSON_GetObjectItem(open, "detected")->valuestring) == "none");
	CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(open, "aliases")) == 0);

	// download = the item's asset plus its unmet external prerequisite's asset,
	// minus what the cache holds. The runtime and other items have their own rows.
	const cJSON *acme = row(all, "acme.depth-pro");
	CHECK(cJSON_GetObjectItem(acme, "size")->valuedouble == 18000000);
	CHECK(cJSON_GetObjectItem(acme, "download")->valuedouble == (18000000 - 8000000) + 20000000);
	const cJSON *pre = cJSON_GetObjectItem(acme, "prerequisites");
	REQUIRE(cJSON_GetArraySize(pre) == 1);
	CHECK(std::string(cJSON_GetObjectItem(cJSON_GetArrayItem(pre, 0), "name")->valuestring) ==
	      "Acme Depth Runtime");
	CHECK(cJSON_IsFalse(cJSON_GetObjectItem(cJSON_GetArrayItem(pre, 0), "met")));
	CHECK(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetArrayItem(pre, 0), "installable")));

	// A detection-only prerequisite is reported with its state, and adds nothing to the download.
	const cJSON *hands = row(all, "handco.hands");
	CHECK(cJSON_GetObjectItem(hands, "download")->valuedouble == 2000000);
	pre = cJSON_GetObjectItem(hands, "prerequisites");
	REQUIRE(cJSON_GetArraySize(pre) == 1);
	CHECK(cJSON_IsFalse(cJSON_GetObjectItem(cJSON_GetArrayItem(pre, 0), "met")));
	CHECK(cJSON_IsFalse(cJSON_GetObjectItem(cJSON_GetArrayItem(pre, 0), "installable")));
	CHECK(cJSON_IsTrue(cJSON_GetObjectItem(hands, "compatible"))); // 2.69.3 is inside its range

	// The runtime row: outdated, its own download, never judged against itself.
	const cJSON *rt = row(all, "runtime");
	CHECK(cJSON_GetObjectItem(rt, "download")->valuedouble == 50000000);
	CHECK(cJSON_IsTrue(cJSON_GetObjectItem(rt, "compatible")));

	// A link-out fetches nothing.
	CHECK(cJSON_GetObjectItem(row(all, "acme.viewer"), "download")->valuedouble == 0);
	cJSON_Delete(all);

	// A prerequisite that is present is `met`; an installed item fetches nothing.
	m.keys.insert("HKLM\\SOFTWARE\\Handco\\TrackingService");
	m.sz[LEIA_VER] = "2.23.0";
	all = dxr_list_catalog(&cat.c, &e, false, false);
	pre = cJSON_GetObjectItem(row(all, "handco.hands"), "prerequisites");
	CHECK(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetArrayItem(pre, 0), "met")));
	CHECK(cJSON_GetObjectItem(row(all, "leia-sr"), "download")->valuedouble == 0);
	cJSON_Delete(all);
}

TEST_CASE("--assume-runtime: compatibility with a runtime that is about to be installed")
{
	Cat cat(V2);
	Machine m;
	m.sz[RT_VER] = "2.60.0";                                                    // what the machine has now
	m.dword["HKLM\\SOFTWARE\\DisplayXR\\Runtime\\Abi|conversion_provider"] = 9; // what THAT runtime implements

	auto listed = [&](const char *id, const char *key) {
		dxr_env e = m.env();
		cJSON *all = dxr_list_catalog(&cat.c, &e, false, true);
		const cJSON *v = cJSON_GetObjectItem(row(all, id), key);
		std::string out = cJSON_IsString(v)
		                      ? v->valuestring
		                      : (cJSON_IsTrue(v) ? "true" : (cJSON_IsFalse(v) ? "false" : "(absent)"));
		cJSON_Delete(all);
		return out;
	};

	// The runtime installer is laying down 2.70.0: the machine's 2.60.0 and its ABI no longer count.
	m.assume_runtime = "2.70.0";
	m.assume_abi = {{"conversion_provider", 1}, {"input_provider", 1}, {"display_processor", 5}};
	CHECK(listed("runtime", "installed") == "2.70.0");
	CHECK(listed("open-conversion", "compatible") == "true");
	CHECK(listed("acme.depth-pro", "compatible") == "true");
	// Above the item's maximum.
	CHECK(listed("handco.hands", "compatible") == "false");
	CHECK(listed("handco.hands", "incompatible_reason").find("2.69.99") != std::string::npos);

	// An older runtime being installed: below a floor, and it is not going to move.
	m.assume_runtime = "2.50.0";
	CHECK(listed("acme.depth-pro", "compatible") == "false");
	CHECK(listed("acme.depth-pro", "incompatible_reason").find("2.60.0") != std::string::npos);
	CHECK(listed("leia-sr", "compatible") == "false"); // a `requires` floor counts the same
	CHECK(listed("open-conversion", "compatible") == "true");

	// The class ABI of the assumed runtime is the caller's to state.
	m.assume_runtime = "2.70.0";
	m.assume_abi = {{"conversion_provider", 2}};
	CHECK(listed("open-conversion", "compatible") == "false");
	CHECK(listed("open-conversion", "incompatible_reason").find("conversion_provider ABI 1") != std::string::npos);
	// Not stated, and it is exactly the catalog's runtime version: the catalog's runtime item says.
	m.assume_abi.clear();
	CHECK(listed("open-conversion", "compatible") == "true");

	// `plan` takes it too: the runtime is treated as present at that version.
	m.assume_runtime = "2.69.0";
	dxr_plan p = plan(cat, m, "handco.hands", DXR_PLAN_ENSURE);
	CHECK(std::string(p.steps[0].installed) == "2.69.0");
	CHECK(p.steps[0].state == DXR_ST_OK);
	CHECK(p.gate == DXR_GET_OK);
}

TEST_CASE("--no-runtime: the runtime is never installed or upgraded")
{
	Cat cat(V2);
	Machine m;

	// The runtime satisfies the item: nothing changes.
	m.sz[RT_VER] = "2.69.0";
	dxr_plan p = plan(cat, m, "leia_plugin", DXR_PLAN_INSTALL, nullptr, true);
	CHECK(p.gate == DXR_GET_OK);
	CHECK(step(p, "runtime")->state == DXR_ST_OK);
	CHECK(step(p, "leia-sr")->state == DXR_ST_MISSING);
	CHECK(p.todo == 1);

	// The item needs a newer runtime: refused with 41, and no runtime installer is planned.
	m.sz[RT_VER] = "2.60.0";
	p = plan(cat, m, "leia_plugin", DXR_PLAN_INSTALL, nullptr, true);
	CHECK(p.gate == DXR_GET_INCOMPATIBLE);
	CHECK(std::string(p.gate_reason).find("--no-runtime") != std::string::npos);
	CHECK(std::string(p.gate_reason).find("2.66.8") != std::string::npos);
	CHECK(std::string(p.gate_reason).find("2.60.0") != std::string::npos);
	CHECK_FALSE(dxr_step_needs_work(step(p, "runtime")));
	// Without the switch the same call upgrades the runtime first.
	p = plan(cat, m, "leia_plugin", DXR_PLAN_INSTALL);
	CHECK(p.gate == DXR_GET_OK);
	CHECK(step(p, "runtime")->state == DXR_ST_TOO_OLD);

	// No runtime at all.
	m.sz.erase(RT_VER);
	p = plan(cat, m, "browser", DXR_PLAN_ENSURE, nullptr, true);
	CHECK(p.gate == DXR_GET_INCOMPATIBLE);
	CHECK(std::string(p.gate_reason).find("not installed") != std::string::npos);
	for (int i = 0; i < p.n; i++) {
		CHECK_FALSE((p.steps[i].id == std::string("runtime") && dxr_step_needs_work(&p.steps[i])));
	}

	// Asking for the runtime itself is refused the same way; at the catalog's version it is a no-op.
	p = plan(cat, m, "runtime", DXR_PLAN_INSTALL, nullptr, true);
	CHECK(p.gate == DXR_GET_INCOMPATIBLE);
	CHECK(p.todo == 0);
	m.sz[RT_VER] = "2.70.0";
	p = plan(cat, m, "runtime", DXR_PLAN_INSTALL, nullptr, true);
	CHECK(p.gate == DXR_GET_OK);
	CHECK(p.todo == 0);
}

TEST_CASE("install --default: the recommended set plus high-confidence hardware")
{
	Cat cat(V2);
	Machine m;
	m.sz[RT_VER] = "2.70.0";

	// No Leia display: the recommended item only.
	dxr_plan p = default_plan(cat, m);
	INFO(text(p));
	REQUIRE(p.n == 2);
	CHECK(std::string(p.steps[0].component) == "runtime");
	CHECK(p.steps[0].state == DXR_ST_OK);
	CHECK(std::string(p.steps[1].component) == "open-conversion");
	CHECK(p.steps[1].state == DXR_ST_MISSING);
	CHECK(std::string(p.steps[1].reason).find("recommended") != std::string::npos);
	CHECK(p.gate == DXR_GET_OK);
	CHECK(p.todo == 1);
	CHECK(p.total_size == 12000000);

	// A low-confidence match selects nothing; a high-confidence one adds the plug-in.
	m.edids = {{1138, 2}};
	CHECK(default_plan(cat, m).todo == 1);
	m.edids = {{44048, 16711}};
	p = default_plan(cat, m);
	CHECK(p.todo == 2);
	REQUIRE(step(p, "leia-sr") != nullptr);
	CHECK(step(p, "leia-sr")->state == DXR_ST_MISSING);
	CHECK(std::string(step(p, "leia-sr")->reason).find("detected") != std::string::npos);
	CHECK(p.total_size == 42000000);
	// It is the same flat plan JSON.
	std::vector<Scanned> s = installer_scan(json(p));
	CHECK(s.size() == (size_t)p.n);

	// Skip-if-current: what is at the catalog's version is not downloaded.
	m.sz["HKLM\\Software\\DisplayXR\\ConversionProviders\\open-conversion\\Version"] = "0.1.1";
	m.sz[LEIA_VER] = "2.23.0";
	p = default_plan(cat, m);
	CHECK(p.todo == 0);
	CHECK(p.total_download == 0);
	CHECK(p.gate == DXR_GET_OK);
	CHECK(step(p, "open-conversion")->current);
	// Older than the catalog: brought up to it (this is `install`).
	m.sz[LEIA_VER] = "2.20.0";
	p = default_plan(cat, m);
	CHECK(step(p, "leia-sr")->state == DXR_ST_TOO_OLD);
	CHECK(p.todo == 1);

	// --no-runtime with a runtime too old for the plug-in: the plug-in is skipped
	// with the reason, the rest of the set still installs, and the plan is not refused.
	m.sz.erase(LEIA_VER);
	m.sz.erase("HKLM\\Software\\DisplayXR\\ConversionProviders\\open-conversion\\Version");
	m.sz[RT_VER] = "2.50.0";
	p = default_plan(cat, m, true);
	INFO(text(p));
	CHECK(p.gate == DXR_GET_OK);
	CHECK(step(p, "leia-sr")->state == DXR_ST_INCOMPATIBLE);
	CHECK(std::string(step(p, "leia-sr")->reason).find("--no-runtime") != std::string::npos);
	CHECK(step(p, "open-conversion")->state == DXR_ST_MISSING); // its floor is 2.45.0
	CHECK(p.todo == 1);
	for (int i = 0; i < p.n; i++) {
		CHECK_FALSE((p.steps[i].id == std::string("runtime") && dxr_step_needs_work(&p.steps[i])));
	}
	// Without the switch the runtime is upgraded for the plug-in, and both install.
	p = default_plan(cat, m);
	CHECK(step(p, "runtime")->state == DXR_ST_TOO_OLD);
	CHECK(p.todo == 3);

	// A schema-1 catalog has no recommended flag: only the hardware match is selected.
	Cat v1(V1);
	m.sz[RT_VER] = "2.70.0";
	p = default_plan(v1, m);
	CHECK(p.todo == 1);
	CHECK(step(p, "leia-sr") != nullptr);
	m.edids.clear();
	p = default_plan(v1, m);
	CHECK(p.n == 0);
	CHECK(p.todo == 0);
	CHECK(text(p).find("Nothing to install.") != std::string::npos);
}

TEST_CASE("research items (§5.6.6): never pre-ticked, badged, and installing one builds nothing")
{
	// The model builder as the catalog lists it, plus the two keys a catalog
	// must not give a research item (recommended, a high-confidence detect
	// rule): dxr-get still never selects it on its own.
	std::string body = V2;
	const size_t end = body.rfind("\n ]\n}");
	REQUIRE(end != std::string::npos);
	body.insert(end, R"json(,
  { "id": "open-conversion-model-builder", "class": "model_builder", "registered_id": "open-model-builder",
    "name": "Model builder", "publisher": "displayxr", "version": "1.0.0", "research": true, "recommended": true,
    "license": { "class": "open", "spdx": "BSL-1.0" },
    "detect": [ { "kind": "edid", "man": 44048, "prod": 16711, "confidence": "high", "note": "hostile" } ],
    "assets": { "windows-x64": { "version": "1.0.0", "file": "ModelBuilderSetup-1.0.0.exe", "size": 3000000,
      "sha256": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "urls": ["https://example.invalid/mb.exe"],
      "signature": { "kind": "authenticode", "signer": "Leia, Inc." },
      "installer": { "kind": "exe", "args": ["/S"], "scope": "machine", "elevation": "required" },
      "installed": { "kind": "registration_row" } } } })json");
	Cat cat(body.c_str());
	REQUIRE(cat.rc == DXR_GET_OK);
	Machine m;
	m.sz[RT_VER] = "2.70.0";
	m.edids = {{44048, 16711}};
	const cJSON *it = dxr_item_find(&cat.c, "open-conversion-model-builder");
	REQUIRE(it != nullptr); // a known class
	CHECK(dxr_item_research(it));

	// install --default and the ensure-for hardware pass never pick it.
	dxr_plan p = default_plan(cat, m);
	INFO(text(p));
	CHECK(step(p, "open-conversion-model-builder") == nullptr);
	CHECK(step(p, "leia-sr") != nullptr); // the same EDID still selects the real plug-in
	p = plan(cat, m, "browser", DXR_PLAN_ENSURE);
	CHECK(step(p, "open-conversion-model-builder") == nullptr);

	// list --json: not recommended, never detected, the badge.
	dxr_env e = m.env();
	cJSON *all = dxr_list_catalog(&cat.c, &e, false, false);
	const cJSON *r = row(all, "open-conversion-model-builder");
	REQUIRE(r != nullptr);
	CHECK(cJSON_IsFalse(cJSON_GetObjectItem(r, "recommended")));
	CHECK(cJSON_IsTrue(cJSON_GetObjectItem(r, "research")));
	CHECK(std::string(cJSON_GetObjectItem(r, "badge")->valuestring) == "Research preview (non-commercial)");
	CHECK(std::string(cJSON_GetObjectItem(r, "detected")->valuestring) == "none");
	CHECK(cJSON_IsFalse(cJSON_GetObjectItem(row(all, "leia-sr"), "research")));
	cJSON_Delete(all);

	// Named, it installs: its own installer and nothing else (no model, no
	// other item); the install is recognised by its ModelBuilders row.
	p = plan(cat, m, "open-conversion-model-builder", DXR_PLAN_INSTALL);
	INFO(text(p));
	CHECK(p.gate == DXR_GET_OK);
	CHECK(p.todo == 1);
	for (int i = 0; i < p.n; i++) {
		const std::string id = p.steps[i].id;
		CHECK((id == "open-conversion-model-builder" || id == "runtime"));
	}
	char path[512];
	REQUIRE(dxr_installed_path(it, path, sizeof(path)));
	CHECK(std::string(path) == "HKLM\\Software\\DisplayXR\\ModelBuilders\\open-model-builder\\Version");
	m.sz[path] = "1.0.0";
	CHECK(plan(cat, m, "open-conversion-model-builder", DXR_PLAN_INSTALL).todo == 0);
}

TEST_CASE("uninstall: what elevation cannot fix is reported before elevation is asked for")
{
	dxr_receipt rs[3] = {receipt("runtime", "2.70.0", SHA_RT), receipt("leia-sr", "2.23.0", SHA_LEIA, "runtime"),
	                     receipt("browser", "1.10.3", SHA_BROWSER)};
	rs[2].uninstall[0] = '\0'; // installed from a catalog entry with no uninstall rule (every schema-1 item)
	snprintf(rs[2].name, sizeof(rs[2].name), "DisplayXR Browser");
	const char *result = nullptr;
	char detail[700];

	for (bool elevated : {false, true}) {
		INFO("elevated: " << elevated);
		// No receipt: 42, elevated or not.
		CHECK(dxr_uninstall_check(nullptr, rs, 3, elevated, &result, detail, sizeof(detail)) ==
		      DXR_GET_NOT_INSTALLED);
		CHECK(std::string(result) == "not_installed");

		// No uninstall command: 43, elevated or not, and the message says what to do instead.
		CHECK(dxr_uninstall_check(&rs[2], rs, 3, elevated, &result, detail, sizeof(detail)) ==
		      DXR_GET_CANNOT_UNINSTALL);
		CHECK(std::string(result) == "no_uninstall_command");
		CHECK(std::string(detail).find("DisplayXR Browser") != std::string::npos);
		CHECK(std::string(detail).find("schema-1") != std::string::npos);
		CHECK(std::string(detail).find("Windows Settings") != std::string::npos);

		// Required by another installed item: 43.
		CHECK(dxr_uninstall_check(&rs[0], rs, 3, elevated, &result, detail, sizeof(detail)) ==
		      DXR_GET_CANNOT_UNINSTALL);
		CHECK(std::string(result) == "required_by_others");
		CHECK(std::string(detail).find("leia-sr") != std::string::npos);
	}

	// Everything else in order: only now does elevation decide.
	CHECK(dxr_uninstall_check(&rs[1], rs, 3, false, &result, detail, sizeof(detail)) == DXR_GET_NEEDS_ADMIN);
	CHECK(std::string(result) == "needs_elevation");
	CHECK(dxr_uninstall_check(&rs[1], rs, 3, true, &result, detail, sizeof(detail)) == DXR_GET_OK);
}

TEST_CASE("several items in one install / uninstall: each judged alone, exit = the first failure")
{
	Cat cat(V2);
	Machine m;

	// The verdict of one item's plan, in the single-item verb's order.
	m.sz[RT_VER] = "2.69.0";
	dxr_plan work = plan(cat, m, "leia_plugin", DXR_PLAN_INSTALL, nullptr, true);
	REQUIRE(work.todo == 1);
	CHECK(dxr_install_judge(&work, true) == DXR_INSTALL_PROCEED);
	CHECK(dxr_install_judge(&work, false) == DXR_INSTALL_NEEDS_ADMIN);

	m.sz[RT_VER] = "2.60.0"; // needs a newer runtime, and --no-runtime forbids moving it
	dxr_plan refused = plan(cat, m, "leia_plugin", DXR_PLAN_INSTALL, nullptr, true);
	REQUIRE(refused.gate == DXR_GET_INCOMPATIBLE);
	// Not compatible is answered before elevation is asked for: elevating would not help.
	CHECK(dxr_install_judge(&refused, false) == DXR_INSTALL_INCOMPATIBLE);
	CHECK(dxr_install_judge(&refused, true) == DXR_INSTALL_INCOMPATIBLE);

	m.sz[RT_VER] = "2.70.0";
	dxr_plan current = plan(cat, m, "runtime", DXR_PLAN_INSTALL, nullptr, true);
	REQUIRE(current.todo == 0);
	CHECK(dxr_install_judge(&current, false) == DXR_INSTALL_NOTHING); // nothing to do needs no elevation

	dxr_plan unknown = plan(cat, m, "no-such-item", DXR_PLAN_INSTALL, nullptr, true);
	REQUIRE(unknown.gate == DXR_GET_NOT_FETCHABLE);
	// The order deployed callers know: elevation first, then "not fetchable".
	CHECK(dxr_install_judge(&unknown, false) == DXR_INSTALL_NEEDS_ADMIN);
	CHECK(dxr_install_judge(&unknown, true) == DXR_INSTALL_NOT_FETCHABLE);

	CHECK(std::string(dxr_install_result_word(DXR_INSTALL_PROCEED, DXR_GET_OK)) == "installed");
	CHECK(std::string(dxr_install_result_word(DXR_INSTALL_PROCEED, DXR_GET_DOWNLOAD)) == "failed");
	CHECK(std::string(dxr_install_result_word(DXR_INSTALL_NOTHING, DXR_GET_OK)) == "nothing_to_do");
	CHECK(std::string(dxr_install_result_word(DXR_INSTALL_INCOMPATIBLE, DXR_GET_INCOMPATIBLE)) == "incompatible");
	CHECK(std::string(dxr_install_result_word(DXR_INSTALL_NEEDS_ADMIN, DXR_GET_NEEDS_ADMIN)) == "needs_elevation");
	CHECK(std::string(dxr_install_result_word(DXR_INSTALL_NOT_FETCHABLE, DXR_GET_NOT_FETCHABLE)) ==
	      "not_fetchable");

	// The document: one entry per item in the order given, each with its own
	// plan (the single-item `--json` document, unchanged), and the first
	// failure as the exit code. A failed item does not hide the ones after it.
	char *work_js = dxr_plan_json(&work);
	char *refused_js = dxr_plan_json(&refused);
	cJSON *items = cJSON_CreateArray();
	cJSON_AddItemToArray(items, dxr_install_item_json("a", DXR_GET_OK, "installed", work_js));
	cJSON_AddItemToArray(items, dxr_install_item_json("b", DXR_GET_INCOMPATIBLE, "incompatible", refused_js));
	cJSON_AddItemToArray(items, dxr_install_item_json("c", DXR_GET_DOWNLOAD, "failed", nullptr));
	cJSON_AddItemToArray(items, dxr_install_item_json("d", DXR_GET_OK, "installed", work_js));
	int code = -1;
	cJSON *doc = dxr_multi_json(items, &code);
	CHECK(code == DXR_GET_INCOMPATIBLE);
	CHECK(cJSON_GetObjectItem(doc, "exit")->valueint == DXR_GET_INCOMPATIBLE);
	const cJSON *arr = cJSON_GetObjectItem(doc, "items");
	REQUIRE(cJSON_GetArraySize(arr) == 4);
	const cJSON *b = cJSON_GetArrayItem(arr, 1);
	CHECK(std::string(cJSON_GetObjectItem(b, "id")->valuestring) == "b");
	CHECK(std::string(cJSON_GetObjectItem(b, "result")->valuestring) == "incompatible");
	CHECK(cJSON_GetObjectItem(cJSON_GetObjectItem(b, "plan"), "gate")->valueint == DXR_GET_INCOMPATIBLE);
	CHECK(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 2), "plan") == nullptr);
	CHECK(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 3), "exit")->valueint == DXR_GET_OK);
	// The embedded plan is the document `install <one item> --json` prints.
	char *again = cJSON_PrintUnformatted(cJSON_GetObjectItem(cJSON_GetArrayItem(arr, 0), "plan"));
	cJSON *ref = cJSON_Parse(work_js);
	char *ref_s = cJSON_PrintUnformatted(ref);
	CHECK(std::string(again) == ref_s);
	cJSON_free(again);
	cJSON_free(ref_s);
	cJSON_Delete(ref);
	cJSON_Delete(doc);
	cJSON_free(work_js);
	cJSON_free(refused_js);

	// Nothing failed: exit 0. The uninstall entries are the single-item objects.
	cJSON *un = cJSON_CreateArray();
	cJSON *u = cJSON_CreateObject();
	cJSON_AddStringToObject(u, "id", "a");
	cJSON_AddStringToObject(u, "result", "uninstalled");
	cJSON_AddNumberToObject(u, "exit", DXR_GET_OK);
	cJSON_AddItemToArray(un, u);
	cJSON *undoc = dxr_multi_json(un, &code);
	CHECK(code == DXR_GET_OK);
	cJSON_Delete(undoc);
}

TEST_CASE("dxr-get: no argument and no log line carries a control character", "[get][log]")
{
	CHECK(dxr_arg_ok("leia-sr"));
	CHECK(dxr_arg_ok("C:\\Program Files\\x\\cat.json"));
	CHECK(dxr_arg_ok("caf\xC3\xA9-\xE6\x97\xA5")); // UTF-8 passes
	CHECK(dxr_arg_ok(""));
	CHECK_FALSE(dxr_arg_ok(nullptr));
	// A line feed in an item id would write a second, forged line into the log.
	CHECK_FALSE(dxr_arg_ok("a\nresult: b: 0 installed"));
	CHECK_FALSE(dxr_arg_ok("a\rb"));
	CHECK_FALSE(dxr_arg_ok("a\tb"));
	CHECK_FALSE(dxr_arg_ok("a\x1b[2Jb"));
	CHECK_FALSE(dxr_arg_ok("a\x7f"));
	CHECK_FALSE(dxr_arg_ok("\x01"));

	// What is logged: the message's own line feeds and tabs stay, nothing else does.
	char text[] = "one\ttwo\r\nthree\x1b[31m\x07\x7f caf\xC3\xA9\n";
	const size_t len = strlen(text);
	dxr_log_sanitize(text);
	CHECK(std::string(text) == "one\ttwo?\nthree?[31m?? caf\xC3\xA9\n");
	CHECK(strlen(text) == len);
}

TEST_CASE("dxr-get: the switches that weaken verification exist in a test-root-key build only", "[get][trust]")
{
	dxr_dev_switches sw = {true, true};
	char why[200], ignored[200];

	// The build that ships. Whatever the environment says, nothing is weakened...
	REQUIRE(dxr_dev_switches_resolve(false, false, true, true, &sw, why, sizeof(why), ignored, sizeof(ignored)));
	CHECK_FALSE(sw.no_verify_catalog);
	CHECK_FALSE(sw.allow_unsigned);
	CHECK(std::string(ignored).find("DXR_GET_DEV and DXR_GET_ALLOW_UNSIGNED") != std::string::npos);
	REQUIRE(dxr_dev_switches_resolve(false, false, false, true, &sw, why, sizeof(why), ignored, sizeof(ignored)));
	CHECK_FALSE(sw.allow_unsigned);
	CHECK(std::string(ignored).find("DXR_GET_ALLOW_UNSIGNED") != std::string::npos);
	REQUIRE(dxr_dev_switches_resolve(false, false, false, false, &sw, why, sizeof(why), ignored, sizeof(ignored)));
	CHECK(ignored[0] == '\0');
	// ...and --no-verify-catalog is a usage error, with or without DXR_GET_DEV.
	sw = {true, true};
	CHECK_FALSE(dxr_dev_switches_resolve(false, true, true, true, &sw, why, sizeof(why), ignored, sizeof(ignored)));
	CHECK_FALSE(sw.no_verify_catalog);
	CHECK_FALSE(sw.allow_unsigned);
	CHECK(std::string(why).find("test-root-key build") != std::string::npos);
	CHECK_FALSE(dxr_dev_switches_resolve(false, true, false, false, &sw, why, sizeof(why), nullptr, 0));

	// A test-root-key build: as before.
	REQUIRE(dxr_dev_switches_resolve(true, true, true, false, &sw, why, sizeof(why), ignored, sizeof(ignored)));
	CHECK(sw.no_verify_catalog);
	CHECK_FALSE(sw.allow_unsigned);
	REQUIRE(dxr_dev_switches_resolve(true, false, false, true, &sw, why, sizeof(why), ignored, sizeof(ignored)));
	CHECK_FALSE(sw.no_verify_catalog);
	CHECK(sw.allow_unsigned);
	CHECK(ignored[0] == '\0');
	CHECK_FALSE(
	    dxr_dev_switches_resolve(true, true, false, false, &sw, why, sizeof(why), ignored, sizeof(ignored)));
	CHECK(std::string(why) == "--no-verify-catalog needs DXR_GET_DEV=1");
	CHECK_FALSE(sw.no_verify_catalog);
}
