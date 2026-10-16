// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief The shared component loader (ADR-054 N2): the class-agnostic row
 *        reader, ordering + shadowing, the load path and the ABI gate.
 *
 * Host-only, no GPU. Every case drives the loader with a test-only class row,
 * so nothing here depends on which real classes exist. The binary-loading
 * cases reuse the conversion loader's stub modules (a conversion-shaped
 * module and a display-processor-shaped one) handed to the loader by path;
 * the Windows registry reader is driven from a throwaway HKCU key -- nothing
 * here ever writes HKLM.
 */

#include "target_component_loader.h"
#include "target_plugin_loader.h"

#include "xrt/xrt_results.h"

#include "catch_amalgamated.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif


namespace {

using Entry = ::target_component_entry;
using Class = ::target_component_class;

void
set_env(const char *name, const char *value)
{
#ifdef _WIN32
	_putenv_s(name, value != nullptr ? value : "");
#else
	if (value != nullptr) {
		setenv(name, value, 1);
	} else {
		unsetenv(name);
	}
#endif
}

//! A class with no private fields: what a brand-new class looks like.
Class
plain_class(const char *entrypoint, uint32_t abi)
{
	Class c;
	std::memset(&c, 0, sizeof(c));
	c.name = "test component";
	c.log_tag = "test loader";
	c.entrypoint = entrypoint;
	c.abi_current = abi;
	c.registry_root = "Software\\DisplayXR-Test\\NoSuchRoot";
	c.manifest_kind = TARGET_PLUGIN_MANIFEST_DISPLAY;
	c.android_prefix = nullptr;
	return c;
}

std::unique_ptr<Entry>
make_entry(const char *id, uint32_t order, const char *source = nullptr)
{
	std::unique_ptr<Entry> e(new Entry);
	target_component_entry_init(e.get());
	std::snprintf(e->id, sizeof(e->id), "%s", id);
	std::snprintf(e->source, sizeof(e->source), "%s", source != nullptr ? source : id);
	std::snprintf(e->binary_path, sizeof(e->binary_path), "/nowhere/%s.so", id);
	e->probe_order = order;
	return e;
}

// The private-field hook, as a class would write it.
struct extra_rec
{
	char colour[32];
	uint32_t hook_calls;
	uint32_t raw_kind;
};

void
read_extra(const Class *cls, const void *raw, enum target_component_raw_kind kind, Entry *e)
{
	(void)cls;
	(void)raw;
	auto *x = reinterpret_cast<extra_rec *>(e->extra);
	x->hook_calls++;
	x->raw_kind = (uint32_t)kind;
	std::snprintf(x->colour, sizeof(x->colour), "%s", "seen");
	if (std::strcmp(e->id, "reject-me") == 0) {
		target_component_mark_malformed(e, "the class said no");
	}
}

} // namespace


/*
 *
 * Manifest parsing (pure).
 *
 */

TEST_CASE("component loader: a manifest's common fields are read and every field stored", "[component_loader]")
{
	const Class cls = plain_class("xrtTestNegotiate", 3);
	const char *full = R"({
	  "file_format_version": "1.0",
	  "plugin": {
	    "id": "acme-thing",
	    "display_name": "Acme Thing",
	    "vendor": "Acme",
	    "version": "1.2.3",
	    "binary_path": "/opt/acme/libacme.so",
	    "probe_order": 50,
	    "enabled": true,
	    "settings": { "ENABLE": "1", "THREADS": 4, "FAST": true, "NESTED": { "x": 1 } }
	  }
	})";

	std::unique_ptr<Entry> e(new Entry);
	REQUIRE(target_component_parse_manifest_json(&cls, full, "/x/050-acme-thing.json", e.get()));
	CHECK(e->flags == 0u);
	CHECK(std::string(e->id) == "acme-thing");
	CHECK(std::string(e->display_name) == "Acme Thing");
	CHECK(std::string(e->vendor) == "Acme");
	CHECK(std::string(e->version) == "1.2.3");
	CHECK(std::string(e->binary_path) == "/opt/acme/libacme.so");
	CHECK(std::string(e->source) == "/x/050-acme-thing.json");
	CHECK(e->probe_order == 50u);
	CHECK(e->enabled);
	REQUIRE(e->setting_count == 3u); // the nested object is not a setting
	CHECK(std::string(e->settings[0].key) == "ENABLE");
	CHECK(std::string(e->settings[0].value) == "1");
	CHECK(std::string(e->settings[1].key) == "THREADS");
	CHECK(std::string(e->settings[1].value) == "4");
	CHECK(std::string(e->settings[2].key) == "FAST");
	CHECK(std::string(e->settings[2].value) == "1");
}

TEST_CASE("component loader: optional manifest fields default sensibly", "[component_loader]")
{
	const Class cls = plain_class("xrtTestNegotiate", 3);
	const char *minimal = R"({"file_format_version":"1.0","plugin":{"id":"min","binary_path":"/opt/min.so"}})";

	std::unique_ptr<Entry> e(new Entry);
	REQUIRE(target_component_parse_manifest_json(&cls, minimal, "/x/min.json", e.get()));
	CHECK(e->probe_order == TARGET_COMPONENT_DEFAULT_PROBE_ORDER);
	CHECK(e->enabled);
	CHECK(std::string(e->display_name) == "min"); // falls back to the id
	CHECK(e->setting_count == 0u);

	// No probe_order field: the NNN- file-name prefix is the fallback.
	REQUIRE(target_component_parse_manifest_json(&cls, minimal, "/x/250-min.json", e.get()));
	CHECK(e->probe_order == 250u);

	// The field wins over the prefix.
	const char *with_order =
	    R"({"file_format_version":"1.0","plugin":{"id":"min","binary_path":"/opt/min.so","probe_order":7}})";
	REQUIRE(target_component_parse_manifest_json(&cls, with_order, "/x/250-min.json", e.get()));
	CHECK(e->probe_order == 7u);

	// enabled:false is a usable row that is DISABLED, not malformed.
	const char *disabled =
	    R"({"file_format_version":"1.0","plugin":{"id":"off","binary_path":"/o.so","enabled":false}})";
	REQUIRE(target_component_parse_manifest_json(&cls, disabled, "/x/off.json", e.get()));
	CHECK_FALSE(e->enabled);
	CHECK((e->flags & TARGET_COMPONENT_FLAG_DISABLED) != 0u);
	CHECK((e->flags & TARGET_COMPONENT_FLAG_MALFORMED) == 0u);
	CHECK(e->reason[0] != '\0');
}

TEST_CASE("component loader: malformed manifests are rejected with a reason", "[component_loader]")
{
	const Class cls = plain_class("xrtTestNegotiate", 3);
	struct Case
	{
		const char *what;
		const char *json;
		const char *expect_id;
	};
	const Case cases[] = {
	    {"not JSON", "{ this is not json", "bad.json"},
	    {"empty", "", "bad.json"},
	    {"no format version", R"({"plugin":{"id":"a","binary_path":"/a.so"}})", "bad.json"},
	    {"wrong format version", R"({"file_format_version":"2.0","plugin":{"id":"a","binary_path":"/a.so"}})",
	     "bad.json"},
	    {"no plugin object", R"({"file_format_version":"1.0"})", "bad.json"},
	    {"missing binary_path", R"({"file_format_version":"1.0","plugin":{"id":"nobin"}})", "nobin"},
	    {"missing id", R"({"file_format_version":"1.0","plugin":{"binary_path":"/a.so"}})", "bad.json"},
	    {"negative probe_order",
	     R"({"file_format_version":"1.0","plugin":{"id":"neg","binary_path":"/a.so","probe_order":-1}})", "neg"},
	    {"string probe_order",
	     R"({"file_format_version":"1.0","plugin":{"id":"s","binary_path":"/a.so","probe_order":"50"}})", "s"},
	    {"enabled not bool",
	     R"({"file_format_version":"1.0","plugin":{"id":"e","binary_path":"/a.so","enabled":"yes"}})", "e"},
	    {"settings not object",
	     R"({"file_format_version":"1.0","plugin":{"id":"t","binary_path":"/a.so","settings":[1]}})", "t"},
	};
	for (const Case &c : cases) {
		INFO(c.what);
		std::unique_ptr<Entry> e(new Entry);
		CHECK_FALSE(target_component_parse_manifest_json(&cls, c.json, "/x/bad.json", e.get()));
		CHECK((e->flags & TARGET_COMPONENT_FLAG_MALFORMED) != 0u);
		CHECK(e->reason[0] != '\0');
		CHECK(std::string(e->id) == c.expect_id);
	}

	std::unique_ptr<Entry> e(new Entry);
	CHECK_FALSE(target_component_parse_manifest_json(&cls, nullptr, "/x/null.json", e.get()));
	CHECK((e->flags & TARGET_COMPONENT_FLAG_MALFORMED) != 0u);
}

TEST_CASE("component loader: the class hook sees the plugin object and may reject the row", "[component_loader]")
{
	Class cls = plain_class("xrtTestNegotiate", 3);
	cls.read_extra = read_extra;

	const char *ok = R"({"file_format_version":"1.0","plugin":{"id":"fine","binary_path":"/a.so"}})";
	std::unique_ptr<Entry> e(new Entry);
	REQUIRE(target_component_parse_manifest_json(&cls, ok, "/x/fine.json", e.get()));
	const auto *x = reinterpret_cast<const extra_rec *>(e->extra);
	CHECK(x->hook_calls == 1u);
	CHECK(x->raw_kind == (uint32_t)TARGET_COMPONENT_RAW_MANIFEST_PLUGIN);
	CHECK(std::string(x->colour) == "seen");

	const char *bad = R"({"file_format_version":"1.0","plugin":{"id":"reject-me","binary_path":"/a.so"}})";
	CHECK_FALSE(target_component_parse_manifest_json(&cls, bad, "/x/reject.json", e.get()));
	CHECK((e->flags & TARGET_COMPONENT_FLAG_MALFORMED) != 0u);
	CHECK(std::string(e->reason) == "the class said no");

	// The first reason wins.
	target_component_mark_malformed(e.get(), "a later reason");
	CHECK(std::string(e->reason) == "the class said no");
}


/*
 *
 * Ordering and shadowing.
 *
 */

TEST_CASE("component loader: order is ProbeOrder ascending, then id, with tie flags", "[component_loader]")
{
	const Class cls = plain_class("xrtTestNegotiate", 3);
	std::vector<Entry> v;
	v.push_back(*make_entry("zeta", 100));
	v.push_back(*make_entry("alpha", 100));
	v.push_back(*make_entry("first", 10));
	v.push_back(*make_entry("last", 250));

	target_component_sort(&cls, v.data(), (int)v.size());
	CHECK(std::string(v[0].id) == "first");
	CHECK(std::string(v[1].id) == "alpha");
	CHECK(std::string(v[2].id) == "zeta");
	CHECK(std::string(v[3].id) == "last");
	CHECK((v[0].flags & TARGET_COMPONENT_FLAG_PROBE_ORDER_TIE) == 0u);
	CHECK((v[1].flags & TARGET_COMPONENT_FLAG_PROBE_ORDER_TIE) != 0u);
	CHECK((v[2].flags & TARGET_COMPONENT_FLAG_PROBE_ORDER_TIE) != 0u);
	CHECK((v[3].flags & TARGET_COMPONENT_FLAG_PROBE_ORDER_TIE) == 0u);

	// Re-sorting a sorted array clears stale tie flags before re-marking.
	v[3].probe_order = 5;
	target_component_sort(&cls, v.data(), (int)v.size());
	CHECK(std::string(v[0].id) == "last");
	CHECK((v[0].flags & TARGET_COMPONENT_FLAG_PROBE_ORDER_TIE) == 0u);

	// Degenerate inputs are no-ops.
	target_component_sort(&cls, nullptr, 3);
	target_component_sort(&cls, v.data(), 0);
}

TEST_CASE("component loader: duplicate ids -- the first root / file wins", "[component_loader]")
{
	const Class cls = plain_class("xrtTestNegotiate", 3);
	std::vector<Entry> out(TARGET_COMPONENT_MAX_ENTRIES);

	// Root 1 (per-user): acme + sim.
	std::vector<Entry> r1;
	r1.push_back(*make_entry("acme", 50, "user/050-acme.json"));
	r1.push_back(*make_entry("sim", 250, "user/250-sim.json"));
	int n = target_component_merge(&cls, out.data(), 0, (int)out.size(), r1.data(), (int)r1.size());
	REQUIRE(n == 2);

	// Root 2 (system): acme again (shadowed), a new one, and a malformed row
	// whose fallback id collides -- malformed rows never shadow and are never
	// shadowed, so it is still listed for diagnostics.
	std::vector<Entry> r2;
	r2.push_back(*make_entry("acme", 10, "sys/010-acme.json"));
	r2.push_back(*make_entry("open", 200, "sys/200-open.json"));
	r2.push_back(*make_entry("acme", 100, "sys/bad.json"));
	target_component_mark_malformed(&r2[2], "broken");
	n = target_component_merge(&cls, out.data(), n, (int)out.size(), r2.data(), (int)r2.size());
	REQUIRE(n == 4);

	int acme_usable = 0;
	for (int i = 0; i < n; i++) {
		if (std::string(out[i].id) == "acme" && (out[i].flags & TARGET_COMPONENT_FLAG_MALFORMED) == 0) {
			acme_usable++;
			CHECK(std::string(out[i].source) == "user/050-acme.json");
			CHECK(out[i].probe_order == 50u);
		}
	}
	CHECK(acme_usable == 1);

	// Truncation at max.
	std::vector<Entry> out3(1);
	std::vector<Entry> two;
	two.push_back(*make_entry("x", 1));
	two.push_back(*make_entry("y", 2));
	CHECK(target_component_merge(&cls, out3.data(), 0, 1, two.data(), 2) == 1);
}


/*
 *
 * The ABI gate (pure).
 *
 */

TEST_CASE("component loader: the ABI gate classifies every negotiate outcome the same way", "[component_loader]")
{
	const Class cls = plain_class("xrtTestNegotiate", 7);
	int iface_storage = 0;
	const void *iface = &iface_storage;
	char reason[192];

	CHECK(target_component_abi_gate(&cls, "ok", XRT_SUCCESS, iface, 7, reason, sizeof(reason)) ==
	      TARGET_COMPONENT_ABI_OK);
	CHECK(reason[0] == '\0');

	struct
	{
		const char *what;
		xrt_result_t xret;
		const void *iface;
		uint32_t version;
		enum target_component_abi_verdict verdict;
	} cases[] = {
	    {"next major, success", XRT_SUCCESS, iface, 8, TARGET_COMPONENT_ABI_MISMATCH},
	    {"next major, refuses", XRT_ERROR_FEATURE_NOT_SUPPORTED, nullptr, 8, TARGET_COMPONENT_ABI_MISMATCH},
	    {"no version written", XRT_SUCCESS, iface, 0, TARGET_COMPONENT_ABI_MISMATCH},
	    {"declined", XRT_ERROR_PROBER_NOT_SUPPORTED, nullptr, 7, TARGET_COMPONENT_ABI_DECLINED},
	    {"declined wins over an odd version", XRT_ERROR_PROBER_NOT_SUPPORTED, nullptr, 8,
	     TARGET_COMPONENT_ABI_DECLINED},
	    {"null iface", XRT_SUCCESS, nullptr, 7, TARGET_COMPONENT_ABI_NEGOTIATE_FAILED},
	    {"error, right version", XRT_ERROR_ALLOCATION, nullptr, 7, TARGET_COMPONENT_ABI_NEGOTIATE_FAILED},
	};
	for (const auto &c : cases) {
		INFO(c.what);
		CHECK(target_component_abi_gate(&cls, "stub", c.xret, c.iface, c.version, reason, sizeof(reason)) ==
		      c.verdict);
		CHECK(reason[0] != '\0');
	}
	// NULL reason is allowed.
	CHECK(target_component_abi_gate(&cls, "stub", XRT_SUCCESS, iface, 8, nullptr, 0) ==
	      TARGET_COMPONENT_ABI_MISMATCH);
}

TEST_CASE("component loader: load-result names are stable", "[component_loader]")
{
	CHECK(std::string(target_component_load_result_str(TARGET_COMPONENT_LOAD_OK)) == "ok");
	CHECK(std::string(target_component_load_result_str(TARGET_COMPONENT_LOAD_BINARY_MISSING)) == "binary-missing");
	CHECK(std::string(target_component_load_result_str(TARGET_COMPONENT_LOAD_PATH_REFUSED)) == "path-refused");
	CHECK(std::string(target_component_load_result_str(TARGET_COMPONENT_LOAD_NO_ENTRY_POINT)) == "no-entrypoint");
	CHECK(std::string(target_component_load_result_str((enum target_component_load_result)99)) == "unknown");
}


/*
 *
 * Real binaries: the conversion loader's stub modules.
 *
 */

#if defined(CONV_STUB_OK_PATH) && defined(CONV_STUB_DP_PATH)

TEST_CASE("component loader: the load path resolves each class's entry point and refuses the other's",
          "[component_loader]")
{
	// Stubs live in the build tree, which the #952 dev-path guard refuses by
	// design; the documented override lets a test (and a dev) load them.
	set_env("DXR_ALLOW_DEV_PLUGIN_PATHS", "1");
	// They are also unsigned and unlisted, and CI runs elevated (no dev
	// override there): pin the provenance gate to audit — this test is about
	// entry points; the gate has its own cases below (ADR-054 N4b PR B).
	target_component_trust_set_enforce_override(0);

	const Class conv = plain_class("xrtConversionPluginNegotiate", 1);
	const Class disp = plain_class("xrtPluginNegotiate", 1);

	std::unique_ptr<Entry> ok = make_entry("stub-conversion", 250);
	std::snprintf(ok->binary_path, sizeof(ok->binary_path), "%s", CONV_STUB_OK_PATH);

	void *module = nullptr;
	void *fn = nullptr;
	uint32_t os_error = 1;
	char reason[192] = "x";
	REQUIRE(target_component_load_binary(&conv, ok.get(), &module, &fn, &os_error, reason, sizeof(reason)) ==
	        TARGET_COMPONENT_LOAD_OK);
	CHECK(module != nullptr);
	CHECK(fn != nullptr);
	CHECK(os_error == 0u);
	CHECK(reason[0] == '\0');
	target_component_unload(module);
	target_component_unload(nullptr); // no-op

	// The same binary asked for another class's entry point: refused, unloaded.
	module = nullptr;
	fn = nullptr;
	CHECK(target_component_load_binary(&disp, ok.get(), &module, &fn, &os_error, reason, sizeof(reason)) ==
	      TARGET_COMPONENT_LOAD_NO_ENTRY_POINT);
	CHECK(module == nullptr);
	CHECK(fn == nullptr);
	CHECK(std::string(reason).find("xrtPluginNegotiate") != std::string::npos);

	// And the display-shaped stub the other way round.
	std::unique_ptr<Entry> dp = make_entry("stub-dp", 250);
	std::snprintf(dp->binary_path, sizeof(dp->binary_path), "%s", CONV_STUB_DP_PATH);
	REQUIRE(target_component_load_binary(&disp, dp.get(), &module, &fn, nullptr, nullptr, 0) ==
	        TARGET_COMPONENT_LOAD_OK);
	target_component_unload(module);
	CHECK(target_component_load_binary(&conv, dp.get(), &module, &fn, nullptr, nullptr, 0) ==
	      TARGET_COMPONENT_LOAD_NO_ENTRY_POINT);

	// Missing file: an orphan registration, named as such.
	std::unique_ptr<Entry> gone = make_entry("gone", 250);
	std::snprintf(gone->binary_path, sizeof(gone->binary_path), "%s.does-not-exist", CONV_STUB_OK_PATH);
	CHECK(target_component_load_binary(&conv, gone.get(), &module, &fn, &os_error, reason, sizeof(reason)) ==
	      TARGET_COMPONENT_LOAD_BINARY_MISSING);
	CHECK(module == nullptr);
	CHECK(std::string(reason).find("does not exist") != std::string::npos);

	// Bad arguments never touch a binary.
	CHECK(target_component_load_binary(&conv, ok.get(), nullptr, &fn, nullptr, nullptr, 0) ==
	      TARGET_COMPONENT_LOAD_FAILED);
	CHECK(target_component_load_binary(nullptr, ok.get(), &module, &fn, nullptr, nullptr, 0) ==
	      TARGET_COMPONENT_LOAD_FAILED);

	set_env("DXR_ALLOW_DEV_PLUGIN_PATHS", nullptr);
	target_component_trust_set_enforce_override(-1);

	// The #952 REFUSED verdict cannot be provoked from here: the path guard
	// trusts anything under the directory of the module the loader is linked
	// into, and in a test executable that IS the build tree. The guard has its
	// own unit coverage; this test only shows the load path honours it.
}

#endif


/*
 *
 * Windows registry rows, from a throwaway HKCU key (never HKLM).
 *
 */

#ifdef _WIN32

namespace {

void
reg_sz(HKEY k, const wchar_t *name, const wchar_t *value)
{
	RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value),
	               (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
}

void
reg_dword(HKEY k, const wchar_t *name, DWORD v)
{
	RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&v), sizeof(v));
}

HKEY
reg_create(HKEY parent, const wchar_t *sub)
{
	HKEY k = nullptr;
	REQUIRE(RegCreateKeyExW(parent, sub, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &k, nullptr) == ERROR_SUCCESS);
	return k;
}

bool
reserved_policy(const char *name)
{
	return _stricmp(name, "Policy") == 0;
}

} // namespace

TEST_CASE("component loader: Windows registry rows are read in full for any class", "[component_loader]")
{
	wchar_t wroot[128];
	swprintf(wroot, 128, L"Software\\DisplayXR-Test\\ComponentLoader-%lu", GetCurrentProcessId());
	char root[128];
	std::snprintf(root, sizeof(root), "Software\\DisplayXR-Test\\ComponentLoader-%lu", GetCurrentProcessId());

	HKEY r = reg_create(HKEY_CURRENT_USER, wroot);

	// A full row.
	HKEY a = reg_create(r, L"acme-thing");
	reg_sz(a, L"Binary", L"C:\\Program Files\\Acme\\Acme.dll");
	reg_sz(a, L"DisplayName", L"Acme Thing");
	reg_sz(a, L"Vendor", L"Acme");
	reg_sz(a, L"Version", L"1.2.3");
	reg_dword(a, L"ProbeOrder", 50);
	reg_dword(a, L"Enabled", 1);
	HKEY s = reg_create(a, L"Settings");
	reg_sz(s, L"ENABLE", L"1");
	reg_dword(s, L"THREADS", 4);
	RegCloseKey(s);
	RegCloseKey(a);

	// Missing Binary: listed, malformed.
	HKEY b = reg_create(r, L"no-binary");
	reg_sz(b, L"DisplayName", L"Broken");
	RegCloseKey(b);

	// Disabled, defaults otherwise.
	HKEY c = reg_create(r, L"off");
	reg_sz(c, L"Binary", L"C:\\x\\off.dll");
	reg_dword(c, L"Enabled", 0);
	RegCloseKey(c);

	// A policy subkey the class reserves, in odd case: never a row.
	HKEY pol = reg_create(r, L"POLICY");
	reg_dword(pol, L"Lock", 1);
	RegCloseKey(pol);
	// Root values do not show up as subkeys at all.
	reg_dword(r, L"AllowResearchOnly", 0);
	RegCloseKey(r);

	Class cls = plain_class("xrtTestNegotiate", 3);
	cls.is_reserved_subkey = reserved_policy;
	cls.read_extra = read_extra;

	std::vector<Entry> out(TARGET_COMPONENT_MAX_ENTRIES);
	int n = target_component_enumerate_registry(&cls, true, root, out.data(), (int)out.size());
	RegDeleteTreeW(HKEY_CURRENT_USER, wroot);
	RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\DisplayXR-Test"); // only if now empty

	REQUIRE(n == 3);
	target_component_sort(&cls, out.data(), n);
	// acme (50) first; "no-binary" and "off" share the default 100 -> by id.
	CHECK(std::string(out[0].id) == "acme-thing");
	CHECK(std::string(out[1].id) == "no-binary");
	CHECK(std::string(out[2].id) == "off");

	const Entry &e = out[0];
	CHECK(e.flags == 0u);
	CHECK(std::string(e.binary_path) == "C:\\Program Files\\Acme\\Acme.dll");
	CHECK(std::string(e.display_name) == "Acme Thing");
	CHECK(std::string(e.vendor) == "Acme");
	CHECK(std::string(e.version) == "1.2.3");
	CHECK(std::string(e.source) == std::string("HKCU\\") + root + "\\acme-thing");
	CHECK(e.probe_order == 50u);
	CHECK(e.enabled);
	REQUIRE(e.setting_count == 2u);
	bool saw_enable = false, saw_threads = false;
	for (uint32_t i = 0; i < e.setting_count; i++) {
		if (std::string(e.settings[i].key) == "ENABLE") {
			saw_enable = std::string(e.settings[i].value) == "1";
		}
		if (std::string(e.settings[i].key) == "THREADS") {
			saw_threads = std::string(e.settings[i].value) == "4";
		}
	}
	CHECK(saw_enable);
	CHECK(saw_threads);
	const auto *x = reinterpret_cast<const extra_rec *>(e.extra);
	CHECK(x->hook_calls == 1u);
	CHECK(x->raw_kind == (uint32_t)TARGET_COMPONENT_RAW_REGISTRY_KEY);

	CHECK((out[1].flags & TARGET_COMPONENT_FLAG_MALFORMED) != 0u);
	CHECK(std::string(out[1].reason).find("Binary") != std::string::npos);
	CHECK(std::string(out[1].display_name) == "Broken");

	CHECK((out[2].flags & TARGET_COMPONENT_FLAG_DISABLED) != 0u);
	CHECK((out[2].flags & TARGET_COMPONENT_FLAG_MALFORMED) == 0u);
	CHECK_FALSE(out[2].enabled);
	CHECK(out[2].probe_order == TARGET_COMPONENT_DEFAULT_PROBE_ORDER);
	CHECK(std::string(out[2].display_name) == "off");

	// An absent root is zero rows, not an error.
	CHECK(target_component_enumerate_registry(&cls, true, "Software\\DisplayXR-Test\\NoSuchRoot-Ever", out.data(),
	                                          (int)out.size()) == 0);
}

TEST_CASE("component loader: the per-user Enabled overlay (N3a) -- HKCU wins, flags stay consistent",
          "[component_loader]")
{
	// The overlay reads HKCU\<registry_root>\<id>\Enabled, so point the class
	// at a throwaway HKCU root. (HKLM <root>\Policy\Lock is never written by a
	// test; its absence is the "no lock" case exercised here.)
	wchar_t wroot[128];
	swprintf(wroot, 128, L"Software\\DisplayXR-Test\\Overlay-%lu", GetCurrentProcessId());
	char root[128];
	std::snprintf(root, sizeof(root), "Software\\DisplayXR-Test\\Overlay-%lu", GetCurrentProcessId());

	HKEY r = reg_create(HKEY_CURRENT_USER, wroot);
	HKEY a = reg_create(r, L"user-off"); // HKLM enabled, user disables
	reg_dword(a, L"Enabled", 0);
	RegCloseKey(a);
	HKEY b = reg_create(r, L"user-on"); // HKLM disabled, user re-enables
	reg_dword(b, L"Enabled", 1);
	RegCloseKey(b);
	HKEY c = reg_create(r, L"broken"); // malformed rows are never overlaid
	reg_dword(c, L"Enabled", 0);
	RegCloseKey(c);
	RegCloseKey(r);

	Class cls = plain_class("xrtTestNegotiate", 3);
	cls.registry_root = root;

	std::vector<Entry> rows;
	rows.push_back(*make_entry("user-off", 10));
	rows.push_back(*make_entry("user-on", 20));
	rows[1].enabled = false;
	rows[1].flags |= TARGET_COMPONENT_FLAG_DISABLED;
	std::snprintf(rows[1].reason, sizeof(rows[1].reason), "%s", "disabled (Enabled = 0)");
	rows.push_back(*make_entry("untouched", 30));
	rows.push_back(*make_entry("broken", 40));
	target_component_mark_malformed(&rows[3], "missing required 'Binary' value");

	const int applied = target_component_apply_user_overlay(&cls, rows.data(), rows.size());
	RegDeleteTreeW(HKEY_CURRENT_USER, wroot);
	RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\DisplayXR-Test"); // only if now empty

	CHECK(applied == 2);

	CHECK_FALSE(rows[0].enabled);
	CHECK((rows[0].flags & TARGET_COMPONENT_FLAG_DISABLED) != 0u);
	CHECK((rows[0].flags & TARGET_COMPONENT_FLAG_USER_OVERLAY) != 0u);
	CHECK(std::string(rows[0].reason).find("this user") != std::string::npos);

	CHECK(rows[1].enabled);
	CHECK((rows[1].flags & TARGET_COMPONENT_FLAG_DISABLED) == 0u);
	CHECK((rows[1].flags & TARGET_COMPONENT_FLAG_USER_OVERLAY) != 0u);
	CHECK(std::string(rows[1].reason).empty());

	CHECK(rows[2].enabled);
	CHECK(rows[2].flags == 0u);

	CHECK(rows[3].enabled); // the HKCU value is not applied to a malformed row
	CHECK((rows[3].flags & TARGET_COMPONENT_FLAG_USER_OVERLAY) == 0u);
	CHECK((rows[3].flags & TARGET_COMPONENT_FLAG_MALFORMED) != 0u);

	// Degenerate arguments are refused without touching the registry.
	CHECK(target_component_apply_user_overlay(nullptr, rows.data(), rows.size()) == 0);
	CHECK(target_component_apply_user_overlay(&cls, nullptr, 1) == 0);
	CHECK(target_component_apply_user_overlay(&cls, rows.data(), 0) == 0);
}

#endif

/*
 *
 * Provenance (ADR-054 N4b).
 *
 */

TEST_CASE("component loader: provenance names are stable", "[component_loader]")
{
	CHECK(std::string(target_component_provenance_str(TARGET_COMPONENT_PROVENANCE_UNKNOWN)) == "unknown");
	CHECK(std::string(target_component_provenance_str(TARGET_COMPONENT_PROVENANCE_SIGNED)) == "signed");
	CHECK(std::string(target_component_provenance_str(TARGET_COMPONENT_PROVENANCE_ALLOWLISTED)) == "allowlisted");
	CHECK(std::string(target_component_provenance_str(TARGET_COMPONENT_PROVENANCE_PLATFORM_TRUSTED)) ==
	      "platform-trusted");
	CHECK(std::string(target_component_provenance_str(TARGET_COMPONENT_PROVENANCE_DEV_OVERRIDE)) == "dev-override");
	CHECK(std::string(target_component_provenance_str(TARGET_COMPONENT_PROVENANCE_UNVERIFIED)) == "unverified");
	CHECK(std::string(target_component_provenance_str(99)) == "unknown");
	CHECK(std::string(target_component_load_result_str(TARGET_COMPONENT_LOAD_UNTRUSTED)) == "untrusted");
}

TEST_CASE("component loader: the provenance rule -- first true wins, no override for an elevated host",
          "[component_loader]")
{
	char reason[160];
	const char *hex = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

	// Signed beats everything.
	CHECK(target_component_provenance_classify(true, true, true, true, true, hex, reason, sizeof(reason)) ==
	      TARGET_COMPONENT_PROVENANCE_SIGNED);
	CHECK(reason[0] != '\0');
	// Then the allowlist, then the platform.
	CHECK(target_component_provenance_classify(false, true, true, false, false, hex, reason, sizeof(reason)) ==
	      TARGET_COMPONENT_PROVENANCE_ALLOWLISTED);
	CHECK(target_component_provenance_classify(false, false, true, false, false, hex, reason, sizeof(reason)) ==
	      TARGET_COMPONENT_PROVENANCE_PLATFORM_TRUSTED);
	// The dev override admits an unsigned, unlisted binary on a NON-elevated host only.
	CHECK(target_component_provenance_classify(false, false, false, true, false, hex, reason, sizeof(reason)) ==
	      TARGET_COMPONENT_PROVENANCE_DEV_OVERRIDE);
	CHECK(target_component_provenance_classify(false, false, false, true, true, hex, reason, sizeof(reason)) ==
	      TARGET_COMPONENT_PROVENANCE_UNVERIFIED);
	CHECK(std::string(reason).find("elevated") != std::string::npos);
	// Nothing at all: unverified, and the reason tells the admin the exact allowlist row.
	CHECK(target_component_provenance_classify(false, false, false, false, false, hex, reason, sizeof(reason)) ==
	      TARGET_COMPONENT_PROVENANCE_UNVERIFIED);
	CHECK(std::string(reason).find(hex) != std::string::npos);
	// An unreadable file still yields a verdict and a reason.
	CHECK(target_component_provenance_classify(false, false, false, false, false, nullptr, reason, sizeof(reason)) ==
	      TARGET_COMPONENT_PROVENANCE_UNVERIFIED);
	CHECK(std::string(reason).find("unreadable") != std::string::npos);
	// NULL reason is allowed.
	CHECK(target_component_provenance_classify(true, false, false, false, false, hex, nullptr, 0) ==
	      TARGET_COMPONENT_PROVENANCE_SIGNED);
}

TEST_CASE("component loader: the file digest is SHA-256 of the bytes", "[component_loader]")
{
	// FIPS 180-4 test vector: SHA-256("abc").
	char path[1024];
	std::snprintf(path, sizeof(path), "%s%ctests_component_loader_sha256_%lu.bin",
	              std::getenv("TMP") != nullptr ? std::getenv("TMP")
	                                           : (std::getenv("TMPDIR") != nullptr ? std::getenv("TMPDIR") : "."),
#ifdef _WIN32
	              '\\',
#else
	              '/',
#endif
	              (unsigned long)
#ifdef _WIN32
	                  GetCurrentProcessId()
#else
	                  getpid()
#endif
	);
	FILE *f = std::fopen(path, "wb");
	REQUIRE(f != nullptr);
	std::fputs("abc", f);
	std::fclose(f);

	char hex[65] = {0};
	REQUIRE(target_component_sha256_file(path, hex, sizeof(hex)));
	CHECK(std::string(hex) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
	std::remove(path);

	// Unreadable / degenerate.
	CHECK_FALSE(target_component_sha256_file(path, hex, sizeof(hex)));
	CHECK(hex[0] == '\0');
	char small[8];
	CHECK_FALSE(target_component_sha256_file(path, small, sizeof(small)));
	CHECK_FALSE(target_component_sha256_file(nullptr, hex, sizeof(hex)));
}

#if defined(CONV_STUB_OK_PATH)

TEST_CASE("component loader: a real unsigned binary gets a verdict that follows the override and elevation",
          "[component_loader]")
{
	const Class cls = plain_class("xrtConversionPluginNegotiate", 1);
	std::unique_ptr<Entry> e = make_entry("stub-conversion", 250);
	std::snprintf(e->binary_path, sizeof(e->binary_path), "%s", CONV_STUB_OK_PATH);

	// The test stubs are unsigned and nobody allowlists a build-tree hash, so
	// the verdict is decided by the override and the host's elevation alone —
	// exactly the rule the classifier test pins, now through the real inputs.
	const bool elevated = target_component_process_elevated();

	set_env("DXR_ALLOW_DEV_PLUGIN_PATHS", nullptr);
	CHECK_FALSE(target_component_check_provenance(&cls, e.get()));
	CHECK(e->provenance == TARGET_COMPONENT_PROVENANCE_UNVERIFIED);
	CHECK(std::strlen(e->sha256) == 64);
	CHECK(e->signer[0] == '\0');
	CHECK(std::string(e->provenance_reason).find(e->sha256) != std::string::npos);

	set_env("DXR_ALLOW_DEV_PLUGIN_PATHS", "1");
	CHECK(target_component_dev_override_set());
	const bool admitted = target_component_check_provenance(&cls, e.get());
	if (elevated) {
		CHECK_FALSE(admitted);
		CHECK(e->provenance == TARGET_COMPONENT_PROVENANCE_UNVERIFIED);
		CHECK(std::string(e->provenance_reason).find("elevated") != std::string::npos);
	} else {
		CHECK(admitted);
		CHECK(e->provenance == TARGET_COMPONENT_PROVENANCE_DEV_OVERRIDE);
	}
	// Second call hits the per-process cache and agrees.
	std::string first_sha = e->sha256;
	(void)target_component_check_provenance(&cls, e.get());
	CHECK(std::string(e->sha256) == first_sha);

	set_env("DXR_ALLOW_DEV_PLUGIN_PATHS", nullptr);

	// A missing file: no digest, unverified, still a reason.
	std::unique_ptr<Entry> gone = make_entry("gone", 250);
	std::snprintf(gone->binary_path, sizeof(gone->binary_path), "%s.does-not-exist", CONV_STUB_OK_PATH);
	CHECK_FALSE(target_component_check_provenance(&cls, gone.get()));
	CHECK(gone->sha256[0] == '\0');
	CHECK(gone->provenance == TARGET_COMPONENT_PROVENANCE_UNVERIFIED);
	CHECK(gone->provenance_reason[0] != '\0');

	// The gate, both ways, on the same unsigned + unlisted stub (PR B: the
	// Windows default is enforcing, so this is the path a stray dev DLL hits).
	set_env("DXR_ALLOW_DEV_PLUGIN_PATHS", "1"); // the #952 path guard still wants it for a build-tree path
	{
		// Audit: loaded anyway, verdict left on the entry.
		target_component_trust_set_enforce_override(0);
		void *module = nullptr;
		void *fn = nullptr;
		REQUIRE(target_component_load_binary(&cls, e.get(), &module, &fn, nullptr, nullptr, 0) ==
		        TARGET_COMPONENT_LOAD_OK);
		CHECK(e->provenance != TARGET_COMPONENT_PROVENANCE_UNKNOWN);
		target_component_unload(module);
	}
	if (elevated) {
		// Enforcing on an elevated host: the override is refused, the verdict
		// is unverified, and the load is REFUSED with a reason naming the
		// allowlist row — before any LoadLibrary / dlopen.
		target_component_trust_set_enforce_override(1);
		void *module = nullptr;
		void *fn = nullptr;
		char reason[256] = {0};
		CHECK(target_component_load_binary(&cls, e.get(), &module, &fn, nullptr, reason, sizeof(reason)) ==
		      TARGET_COMPONENT_LOAD_UNTRUSTED);
		CHECK(module == nullptr);
		CHECK(e->provenance == TARGET_COMPONENT_PROVENANCE_UNVERIFIED);
		CHECK(std::string(reason).find("untrusted") == 0);
		CHECK(std::string(reason).find(e->sha256) != std::string::npos);
	} else {
		// Enforcing on a normal prompt: the dev override still admits it.
		target_component_trust_set_enforce_override(1);
		void *module = nullptr;
		void *fn = nullptr;
		REQUIRE(target_component_load_binary(&cls, e.get(), &module, &fn, nullptr, nullptr, 0) ==
		        TARGET_COMPONENT_LOAD_OK);
		CHECK(e->provenance == TARGET_COMPONENT_PROVENANCE_DEV_OVERRIDE);
		target_component_unload(module);
	}
	target_component_trust_set_enforce_override(-1);
	set_env("DXR_ALLOW_DEV_PLUGIN_PATHS", nullptr);
}

TEST_CASE("component loader: the enforce policy -- an explicit knob wins, else the platform default (PR B)",
          "[component_loader]")
{
	// Knob present: 0 = audit, anything else = enforce, whatever the platform.
	CHECK_FALSE(target_component_trust_policy(true, true, true));
	CHECK_FALSE(target_component_trust_policy(true, true, false));
	CHECK(target_component_trust_policy(true, false, true));
	CHECK(target_component_trust_policy(true, false, false));
	// Knob absent: the platform default (Windows enforces, POSIX audits).
	CHECK(target_component_trust_policy(false, false, true));
	CHECK_FALSE(target_component_trust_policy(false, false, false));
	CHECK_FALSE(target_component_trust_policy(false, true, false)); // knob_zero means nothing without a knob

	// The test override pins the process either way and -1 releases it.
	target_component_trust_set_enforce_override(1);
	CHECK(target_component_trust_enforced());
	target_component_trust_set_enforce_override(0);
	CHECK_FALSE(target_component_trust_enforced());
	target_component_trust_set_enforce_override(-1);
#if defined(XRT_OS_WINDOWS)
	// Released: the platform default applies — enforcing, unless this box
	// wrote the opt-out knob (then the policy helper above already covers it).
	DWORD v = 1;
	DWORD bytes = sizeof(v);
	const bool present = RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\DisplayXR\\Trust", L"Enforce",
	                                  RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY, NULL, &v, &bytes) == ERROR_SUCCESS;
	CHECK(target_component_trust_enforced() == target_component_trust_policy(present, present && v == 0, true));
#endif
}

#endif

TEST_CASE("component loader: the production enumeration of an empty class is well-formed", "[component_loader]")
{
	// A class nobody has registered anywhere on this box: zero rows, and the
	// degenerate arguments are refused without touching the discovery root.
	// (No manifest kind either, so a POSIX box's real DP manifests are not
	// mistaken for this class's rows.)
	Class cls = plain_class("xrtNoSuchClassNegotiate", 1);
	cls.manifest_kind = TARGET_PLUGIN_MANIFEST_NONE;
	std::vector<Entry> out(TARGET_COMPONENT_MAX_ENTRIES);
	CHECK(target_component_enumerate(&cls, out.data(), (int)out.size()) == 0);
	CHECK(target_component_enumerate(nullptr, out.data(), 4) == 0);
	CHECK(target_component_enumerate(&cls, nullptr, 4) == 0);
	CHECK(target_component_enumerate(&cls, out.data(), 0) == 0);
}

#if !defined(_WIN32) && !defined(__ANDROID__)
TEST_CASE("component loader: a model pack in a discovery root is not a manifest of any class", "[component_loader]")
{
	// ADR-053 P4: the packagers install `sim-conversion.models.json` beside
	// its provider in the shared plug-in dir. It must be neither a DP row nor
	// a malformed one (it used to be both: "unsupported file_format_version").
	char dir[] = "/tmp/dxr_tc_packs_XXXXXX";
	REQUIRE(mkdtemp(dir) != nullptr);
	const std::string root(dir);
	const std::string pack = root + "/sim-conversion.models.json";
	const std::string dp = root + "/200-acme-dp.json";
	FILE *f = std::fopen(pack.c_str(), "w");
	REQUIRE(f != nullptr);
	std::fputs(R"({"schema":2,"pack":"sim","provider":"sim-conversion","models":[]})", f);
	std::fclose(f);
	f = std::fopen(dp.c_str(), "w");
	REQUIRE(f != nullptr);
	std::fputs(R"({"file_format_version":"1.0","plugin":{"id":"acme-dp","binary_path":"/nowhere/acme.so"}})", f);
	std::fclose(f);

	const char *old = std::getenv("XRT_PLUGIN_SEARCH_PATH");
	const std::string saved = old != nullptr ? old : "";
	const bool had_old = old != nullptr;
	set_env("XRT_PLUGIN_SEARCH_PATH", dir);

	const Class cls = plain_class("xrtTestNegotiate", 1); // DISPLAY manifest kind
	std::vector<Entry> out(TARGET_COMPONENT_MAX_ENTRIES);
	const int n = target_component_enumerate(&cls, out.data(), (int)out.size());

	set_env("XRT_PLUGIN_SEARCH_PATH", had_old ? saved.c_str() : nullptr);
	std::remove(pack.c_str());
	std::remove(dp.c_str());
	rmdir(dir);

	// Only rows from our root: a dev box may have real DP manifests installed.
	int ours = 0;
	for (int i = 0; i < n; i++) {
		const std::string src(out[i].source);
		if (src.compare(0, root.size(), root) != 0) {
			continue;
		}
		ours++;
		CHECK(src == dp);
		CHECK((out[i].flags & TARGET_COMPONENT_FLAG_MALFORMED) == 0u);
	}
	CHECK(ours == 1);
}
#endif
