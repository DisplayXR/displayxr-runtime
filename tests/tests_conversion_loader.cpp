// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief The conversion-provider loader (ADR-053, #1999): discovery parsing,
 *        ordering, the plug-in-kind router and the ABI gate.
 *
 * Host-only, no GPU. Pure parts run on every platform; the binary-loading
 * cases use three test-only stub modules (tests_conversion_loader_stub.c)
 * handed to the loader by path, and the Windows registry reader is driven
 * from a throwaway HKCU key -- nothing here ever writes HKLM.
 */

#include "target_component_loader.h"
#include "target_conversion_plugin_loader.h"
#include "target_plugin_loader.h"

#include "xrt/xrt_conversion_plugin.h"
#include "xrt/xrt_results.h"

#include "catch_amalgamated.hpp"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif


namespace {

using Entry = ::xrt_conversion_plugin_entry;

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

Entry
make_entry(const char *id, uint32_t order)
{
	Entry e;
	std::memset(&e, 0, sizeof(e));
	std::snprintf(e.id, sizeof(e.id), "%s", id);
	std::snprintf(e.source, sizeof(e.source), "src/%s", id);
	std::snprintf(e.binary_path, sizeof(e.binary_path), "/nowhere/%s.so", id);
	e.probe_order = order;
	e.enabled = true;
	return e;
}

const char *kFullManifest = R"({
  "file_format_version": "1.0",
  "plugin": {
    "id": "acme-depth",
    "display_name": "Acme Depth",
    "vendor": "Acme",
    "version": "1.2.3",
    "binary_path": "/opt/acme/libacme_conv.so",
    "probe_order": 50,
    "enabled": true,
    "hosting": "Satellite",
    "supersedes": "dp-lift",
    "packs": ["/opt/acme/packs/a.json", "/opt/acme/packs/b.json", 7],
    "settings": { "ENABLE": "1", "THREADS": 4, "FAST": true, "NESTED": { "x": 1 } }
  }
})";

} // namespace


/*
 *
 * The router: one directory, three plug-in types.
 *
 */

TEST_CASE("conversion loader: the manifest suffix routes each file to exactly one loader", "[conversion_loader]")
{
	CHECK(target_plugin_classify_manifest("250-sim-conversion-conversion-provider.json") ==
	      TARGET_PLUGIN_MANIFEST_CONVERSION);
	CHECK(target_plugin_classify_manifest("200-sim-input-input-provider.json") == TARGET_PLUGIN_MANIFEST_INPUT);
	CHECK(target_plugin_classify_manifest("200-sim-display.json") == TARGET_PLUGIN_MANIFEST_DISPLAY);
	CHECK(target_plugin_classify_manifest("050-leia-sr.json") == TARGET_PLUGIN_MANIFEST_DISPLAY);

	// Not manifests at all.
	CHECK(target_plugin_classify_manifest("DisplayXR-SimConversion.so") == TARGET_PLUGIN_MANIFEST_NONE);
	CHECK(target_plugin_classify_manifest("readme.txt") == TARGET_PLUGIN_MANIFEST_NONE);
	CHECK(target_plugin_classify_manifest(".json") == TARGET_PLUGIN_MANIFEST_NONE);
	CHECK(target_plugin_classify_manifest("-conversion-provider.json") == TARGET_PLUGIN_MANIFEST_NONE);
	CHECK(target_plugin_classify_manifest("-input-provider.json") == TARGET_PLUGIN_MANIFEST_NONE);
	CHECK(target_plugin_classify_manifest(nullptr) == TARGET_PLUGIN_MANIFEST_NONE);
	// ADR-053 P4 model packs ship beside their provider, in the same dir.
	CHECK(target_plugin_classify_manifest("sim-conversion.models.json") == TARGET_PLUGIN_MANIFEST_NONE);
	CHECK(target_plugin_classify_manifest("dxr-open-default.models.json") == TARGET_PLUGIN_MANIFEST_NONE);
	CHECK(target_plugin_classify_manifest(".models.json") == TARGET_PLUGIN_MANIFEST_NONE);

	// The input loader's own rule (requires "-input-provider.json") must stay
	// blind to conversion manifests, and the DP loader skips everything that
	// is not DISPLAY -- both follow from the classification above, pinned here
	// as the raw suffix fact the input loader relies on.
	const std::string conv = "250-sim-conversion-conversion-provider.json";
	const std::string ip = "-input-provider.json";
	CHECK(conv.compare(conv.size() - ip.size(), ip.size(), ip) != 0);
}

TEST_CASE("conversion loader: Android libdxrc / libdxrp prefixes never cross", "[conversion_loader]")
{
	uint32_t order = 0;
	char id[64] = {0};

	REQUIRE(target_plugin_parse_android_filename("libdxrc250_sim_conversion.so",
	                                             TARGET_PLUGIN_ANDROID_PREFIX_CONVERSION, &order, id, sizeof(id)));
	CHECK(order == 250);
	CHECK(std::string(id) == "sim_conversion");

	// The DP loader's prefix does not claim a conversion provider, and vice versa.
	CHECK_FALSE(target_plugin_parse_android_filename("libdxrc250_sim_conversion.so",
	                                                 TARGET_PLUGIN_ANDROID_PREFIX_DISPLAY, &order, id, sizeof(id)));
	CHECK_FALSE(target_plugin_parse_android_filename(
	    "libdxrp200_sim_display.so", TARGET_PLUGIN_ANDROID_PREFIX_CONVERSION, &order, id, sizeof(id)));
	REQUIRE(target_plugin_parse_android_filename("libdxrp200_sim_display.so", TARGET_PLUGIN_ANDROID_PREFIX_DISPLAY,
	                                             &order, id, sizeof(id)));
	CHECK(order == 200);
	CHECK(std::string(id) == "sim_display");

	// Malformed names.
	const char *bad[] = {"libdxrc25_x.so",    "libdxrc250x.so",  "libdxrc250_.so",
	                     "libdxrc250_x.so.1", "libdxrcabc_x.so", "libdxrc250_x"};
	for (const char *b : bad) {
		INFO(b);
		CHECK_FALSE(target_plugin_parse_android_filename(b, TARGET_PLUGIN_ANDROID_PREFIX_CONVERSION, &order, id,
		                                                 sizeof(id)));
	}
	// An id that does not fit the caller's buffer is refused, not truncated.
	char tiny[4];
	CHECK_FALSE(target_plugin_parse_android_filename(
	    "libdxrc050_toolong.so", TARGET_PLUGIN_ANDROID_PREFIX_CONVERSION, &order, tiny, sizeof(tiny)));
}


/*
 *
 * POSIX manifest parsing (the parser itself is platform-neutral).
 *
 */

TEST_CASE("conversion loader: a full manifest is read and every field stored", "[conversion_loader]")
{
	Entry e;
	REQUIRE(target_conversion_plugin_parse_manifest_json(kFullManifest,
	                                                     "/x/050-acme-depth-conversion-provider.json", &e));
	CHECK(e.status == XRT_CONV_LOAD_NOT_LOADED);
	CHECK(e.flags == 0u);
	CHECK(std::string(e.id) == "acme-depth");
	CHECK(std::string(e.display_name) == "Acme Depth");
	CHECK(std::string(e.vendor) == "Acme");
	CHECK(std::string(e.version) == "1.2.3");
	CHECK(std::string(e.binary_path) == "/opt/acme/libacme_conv.so");
	CHECK(std::string(e.source) == "/x/050-acme-depth-conversion-provider.json");
	CHECK(e.probe_order == 50u);
	CHECK(e.enabled);
	CHECK(e.hosting == XRT_CONV_HOSTING_SATELLITE); // case-insensitive
	CHECK(std::string(e.hosting_raw) == "Satellite");
	CHECK(std::string(e.supersedes) == "dp-lift");

	REQUIRE(e.pack_count == 2u); // the non-string element is ignored
	CHECK(std::string(e.packs[0]) == "/opt/acme/packs/a.json");
	CHECK(std::string(e.packs[1]) == "/opt/acme/packs/b.json");

	REQUIRE(e.setting_count == 3u); // the nested object is not a setting
	CHECK(std::string(e.settings[0].key) == "ENABLE");
	CHECK(std::string(e.settings[0].value) == "1");
	CHECK(std::string(e.settings[1].key) == "THREADS");
	CHECK(std::string(e.settings[1].value) == "4");
	CHECK(std::string(e.settings[2].key) == "FAST");
	CHECK(std::string(e.settings[2].value) == "1");

	CHECK(e.iface == nullptr);
	CHECK(e.module == nullptr);
}

TEST_CASE("conversion loader: optional fields default sensibly", "[conversion_loader]")
{
	const char *minimal = R"({"file_format_version":"1.0","plugin":{"id":"min","binary_path":"/opt/min.so"}})";

	Entry e;
	REQUIRE(target_conversion_plugin_parse_manifest_json(minimal, "/x/min-conversion-provider.json", &e));
	CHECK(e.probe_order == XRT_CONV_LOADER_DEFAULT_PROBE_ORDER);
	CHECK(e.enabled);
	CHECK(e.hosting == XRT_CONV_HOSTING_AUTO);
	CHECK(std::string(e.display_name) == "min"); // falls back to the id
	CHECK(e.pack_count == 0u);
	CHECK(e.setting_count == 0u);

	// No probe_order field: the NNN- file-name prefix is the fallback.
	REQUIRE(target_conversion_plugin_parse_manifest_json(minimal, "/x/250-min-conversion-provider.json", &e));
	CHECK(e.probe_order == 250u);

	// The field wins over the prefix.
	const char *with_order =
	    R"({"file_format_version":"1.0","plugin":{"id":"min","binary_path":"/opt/min.so","probe_order":7}})";
	REQUIRE(target_conversion_plugin_parse_manifest_json(with_order, "/x/250-min-conversion-provider.json", &e));
	CHECK(e.probe_order == 7u);

	// enabled:false is a usable row that is DISABLED, not malformed.
	const char *disabled =
	    R"({"file_format_version":"1.0","plugin":{"id":"off","binary_path":"/o.so","enabled":false}})";
	REQUIRE(target_conversion_plugin_parse_manifest_json(disabled, "/x/off-conversion-provider.json", &e));
	CHECK_FALSE(e.enabled);
	CHECK(e.status == XRT_CONV_LOAD_DISABLED);
	CHECK((e.flags & XRT_CONV_ENTRY_FLAG_DISABLED) != 0u);
	CHECK((e.flags & XRT_CONV_ENTRY_FLAG_MALFORMED) == 0u);

	// An unknown Hosting value is carried, flagged, and treated as auto.
	const char *odd =
	    R"({"file_format_version":"1.0","plugin":{"id":"odd","binary_path":"/o.so","hosting":"elsewhere"}})";
	REQUIRE(target_conversion_plugin_parse_manifest_json(odd, "/x/odd-conversion-provider.json", &e));
	CHECK(e.hosting == XRT_CONV_HOSTING_AUTO);
	CHECK((e.flags & XRT_CONV_ENTRY_FLAG_HOSTING_UNKNOWN) != 0u);
	CHECK(std::string(e.hosting_raw) == "elsewhere");
}

TEST_CASE("conversion loader: malformed manifests are rejected with a reason", "[conversion_loader]")
{
	struct Case
	{
		const char *what;
		const char *json;
		const char *expect_id;
	};
	const Case cases[] = {
	    {"not JSON", "{ this is not json", "bad-conversion-provider.json"},
	    {"empty", "", "bad-conversion-provider.json"},
	    {"no format version", R"({"plugin":{"id":"a","binary_path":"/a.so"}})", "bad-conversion-provider.json"},
	    {"wrong format version", R"({"file_format_version":"2.0","plugin":{"id":"a","binary_path":"/a.so"}})",
	     "bad-conversion-provider.json"},
	    {"no plugin object", R"({"file_format_version":"1.0"})", "bad-conversion-provider.json"},
	    {"missing binary_path", R"({"file_format_version":"1.0","plugin":{"id":"nobin"}})", "nobin"},
	    {"missing id", R"({"file_format_version":"1.0","plugin":{"binary_path":"/a.so"}})",
	     "bad-conversion-provider.json"},
	    {"negative probe_order",
	     R"({"file_format_version":"1.0","plugin":{"id":"neg","binary_path":"/a.so","probe_order":-1}})", "neg"},
	    {"string probe_order",
	     R"({"file_format_version":"1.0","plugin":{"id":"s","binary_path":"/a.so","probe_order":"50"}})", "s"},
	    {"enabled not bool",
	     R"({"file_format_version":"1.0","plugin":{"id":"e","binary_path":"/a.so","enabled":"yes"}})", "e"},
	    {"packs not array",
	     R"({"file_format_version":"1.0","plugin":{"id":"p","binary_path":"/a.so","packs":"/x.json"}})", "p"},
	    {"settings not object",
	     R"({"file_format_version":"1.0","plugin":{"id":"t","binary_path":"/a.so","settings":[1]}})", "t"},
	};
	for (const Case &c : cases) {
		INFO(c.what);
		Entry e;
		CHECK_FALSE(
		    target_conversion_plugin_parse_manifest_json(c.json, "/x/bad-conversion-provider.json", &e));
		CHECK(e.status == XRT_CONV_LOAD_MALFORMED);
		CHECK((e.flags & XRT_CONV_ENTRY_FLAG_MALFORMED) != 0u);
		CHECK(e.reason[0] != '\0');
		CHECK(std::string(e.id) == c.expect_id);
	}

	Entry e;
	CHECK_FALSE(target_conversion_plugin_parse_manifest_json(nullptr, "/x/null-conversion-provider.json", &e));
	CHECK(e.status == XRT_CONV_LOAD_MALFORMED);

	// A malformed row is refused by negotiate_one without touching any binary.
	CHECK_FALSE(target_conversion_plugin_negotiate_one(&e, nullptr));
	CHECK(e.status == XRT_CONV_LOAD_MALFORMED);
	CHECK(e.module == nullptr);
	CHECK(e.iface == nullptr);
}


/*
 *
 * Ordering, duplicates, kill switches.
 *
 */

TEST_CASE("conversion loader: order is ProbeOrder ascending, then id", "[conversion_loader]")
{
	std::vector<Entry> v = {make_entry("zeta", 100), make_entry("sim-conversion", 250), make_entry("alpha", 100),
	                        make_entry("vendor", 50), make_entry("open", 200)};
	target_conversion_plugin_sort(v.data(), (int)v.size());

	const char *expect[] = {"vendor", "alpha", "zeta", "open", "sim-conversion"};
	for (size_t i = 0; i < v.size(); i++) {
		INFO(i);
		CHECK(std::string(v[i].id) == expect[i]);
	}
	// The tied pair is flagged; the others are not.
	CHECK((v[1].flags & XRT_CONV_ENTRY_FLAG_PROBE_ORDER_TIE) != 0u);
	CHECK((v[2].flags & XRT_CONV_ENTRY_FLAG_PROBE_ORDER_TIE) != 0u);
	CHECK((v[0].flags & XRT_CONV_ENTRY_FLAG_PROBE_ORDER_TIE) == 0u);
	CHECK((v[3].flags & XRT_CONV_ENTRY_FLAG_PROBE_ORDER_TIE) == 0u);
	CHECK((v[4].flags & XRT_CONV_ENTRY_FLAG_PROBE_ORDER_TIE) == 0u);

	// Deterministic regardless of input order.
	std::vector<Entry> w = {v[4], v[2], v[0], v[3], v[1]};
	target_conversion_plugin_sort(w.data(), (int)w.size());
	for (size_t i = 0; i < w.size(); i++) {
		CHECK(std::string(w[i].id) == expect[i]);
	}

	// A re-sort after the tie is gone clears the stale flag.
	w[2].probe_order = 101;
	target_conversion_plugin_sort(w.data(), (int)w.size());
	for (const Entry &e : w) {
		CHECK((e.flags & XRT_CONV_ENTRY_FLAG_PROBE_ORDER_TIE) == 0u);
	}
}

TEST_CASE("conversion loader: duplicate ids -- the first root / file wins", "[conversion_loader]")
{
	std::vector<Entry> out(XRT_CONV_LOADER_MAX_ENTRIES);

	// Root 1 (per-user): acme + sim.
	Entry user_acme = make_entry("acme", 50);
	std::snprintf(user_acme.source, sizeof(user_acme.source), "user/050-acme-conversion-provider.json");
	Entry user_sim = make_entry("sim-conversion", 250);
	Entry r1[] = {user_acme, user_sim};
	int n = target_conversion_plugin_merge(out.data(), 0, (int)out.size(), r1, 2);
	REQUIRE(n == 2);

	// Root 2 (system): acme again (shadowed), a new one, and a malformed row
	// whose fallback id happens to collide -- malformed rows never shadow and
	// are never shadowed, so it is still listed for diagnostics.
	Entry sys_acme = make_entry("acme", 10);
	std::snprintf(sys_acme.source, sizeof(sys_acme.source), "sys/010-acme-conversion-provider.json");
	Entry sys_new = make_entry("open", 200);
	Entry sys_bad = make_entry("acme", 100);
	sys_bad.flags |= XRT_CONV_ENTRY_FLAG_MALFORMED;
	sys_bad.status = XRT_CONV_LOAD_MALFORMED;
	Entry r2[] = {sys_acme, sys_new, sys_bad};
	n = target_conversion_plugin_merge(out.data(), n, (int)out.size(), r2, 3);
	REQUIRE(n == 4);

	int acme_usable = 0;
	for (int i = 0; i < n; i++) {
		if (std::string(out[i].id) == "acme" && (out[i].flags & XRT_CONV_ENTRY_FLAG_MALFORMED) == 0) {
			acme_usable++;
			CHECK(std::string(out[i].source) == "user/050-acme-conversion-provider.json");
			CHECK(out[i].probe_order == 50u);
		}
	}
	CHECK(acme_usable == 1);

	// Within ONE root the first file wins as well.
	Entry a = make_entry("dup", 100);
	Entry b = make_entry("dup", 1);
	Entry same_root[] = {a, b};
	std::vector<Entry> out2(4);
	CHECK(target_conversion_plugin_merge(out2.data(), 0, 4, same_root, 2) == 1);
	CHECK(out2[0].probe_order == 100u);

	// Truncation at max.
	std::vector<Entry> out3(1);
	Entry two[] = {make_entry("x", 1), make_entry("y", 2)};
	CHECK(target_conversion_plugin_merge(out3.data(), 0, 1, two, 2) == 1);
}

TEST_CASE("conversion loader: DXR_CONV_PROVIDERS / DXR_CONV_EXCLUSIVE suppress rows", "[conversion_loader]")
{
	auto fresh = [] {
		std::vector<Entry> v = {make_entry("a", 1), make_entry("b", 2), make_entry("c", 3)};
		v[2].flags |= XRT_CONV_ENTRY_FLAG_MALFORMED;
		v[2].status = XRT_CONV_LOAD_MALFORMED;
		std::snprintf(v[2].reason, sizeof(v[2].reason), "missing Binary");
		return v;
	};

	std::vector<Entry> v = fresh();
	target_conversion_plugin_apply_kill_switches(v.data(), 3, nullptr, nullptr);
	CHECK(v[0].flags == 0u);
	CHECK(v[1].status == XRT_CONV_LOAD_NOT_LOADED);

	v = fresh();
	target_conversion_plugin_apply_kill_switches(v.data(), 3, "1", "");
	CHECK(v[0].flags == 0u); // only "0" is the kill switch

	v = fresh();
	target_conversion_plugin_apply_kill_switches(v.data(), 3, "0", nullptr);
	for (const Entry &e : v) {
		CHECK((e.flags & XRT_CONV_ENTRY_FLAG_SUPPRESSED) != 0u);
	}
	CHECK(v[0].status == XRT_CONV_LOAD_SUPPRESSED);
	CHECK(v[2].status == XRT_CONV_LOAD_MALFORMED); // keeps saying why it is broken
	CHECK(std::string(v[2].reason) == "missing Binary");

	v = fresh();
	target_conversion_plugin_apply_kill_switches(v.data(), 3, nullptr, "b");
	CHECK(v[0].status == XRT_CONV_LOAD_SUPPRESSED);
	CHECK(v[1].status == XRT_CONV_LOAD_NOT_LOADED);
	CHECK((v[1].flags & XRT_CONV_ENTRY_FLAG_SUPPRESSED) == 0u);

	// A suppressed row is refused by negotiate_one without loading anything.
	CHECK_FALSE(target_conversion_plugin_negotiate_one(&v[0], nullptr));
	CHECK(v[0].status == XRT_CONV_LOAD_SUPPRESSED);
	CHECK(v[0].module == nullptr);
}

TEST_CASE("conversion loader: reserved policy subkeys", "[conversion_loader]")
{
	CHECK(target_conversion_plugin_is_reserved_subkey("Policy"));
	CHECK(target_conversion_plugin_is_reserved_subkey("policy"));
	CHECK(target_conversion_plugin_is_reserved_subkey("PreferredProvider"));
	CHECK(target_conversion_plugin_is_reserved_subkey("PREWARM"));
	CHECK_FALSE(target_conversion_plugin_is_reserved_subkey("Prewarmer"));
	CHECK_FALSE(target_conversion_plugin_is_reserved_subkey("sim-conversion"));
	CHECK_FALSE(target_conversion_plugin_is_reserved_subkey(""));
	CHECK_FALSE(target_conversion_plugin_is_reserved_subkey(nullptr));
	int n = 0;
	while (target_conversion_plugin_reserved_subkeys[n] != nullptr) {
		n++;
	}
	CHECK(n == 3);
}

TEST_CASE("conversion loader: Hosting strings", "[conversion_loader]")
{
	bool known = false;
	CHECK(target_conversion_plugin_parse_hosting(nullptr, &known) == XRT_CONV_HOSTING_AUTO);
	CHECK(known);
	CHECK(target_conversion_plugin_parse_hosting("", &known) == XRT_CONV_HOSTING_AUTO);
	CHECK(target_conversion_plugin_parse_hosting("AUTO", &known) == XRT_CONV_HOSTING_AUTO);
	CHECK(target_conversion_plugin_parse_hosting("in-service", &known) == XRT_CONV_HOSTING_IN_SERVICE);
	CHECK(known);
	CHECK(target_conversion_plugin_parse_hosting("Satellite", &known) == XRT_CONV_HOSTING_SATELLITE);
	CHECK(target_conversion_plugin_parse_hosting("inservice", &known) == XRT_CONV_HOSTING_AUTO);
	CHECK_FALSE(known);
	CHECK(target_conversion_plugin_parse_hosting("satellites", nullptr) == XRT_CONV_HOSTING_AUTO);
}

TEST_CASE("conversion loader: effective hosting (P2: auto = satellite; env override wins)", "[conversion_loader]")
{
	bool from_env = true;
	// Registered value alone: auto -> satellite for every loader-registered DLL.
	CHECK(target_conversion_plugin_resolve_hosting(XRT_CONV_HOSTING_AUTO, nullptr, &from_env) ==
	      XRT_CONV_HOSTING_SATELLITE);
	CHECK_FALSE(from_env);
	CHECK(target_conversion_plugin_resolve_hosting(XRT_CONV_HOSTING_SATELLITE, "", nullptr) ==
	      XRT_CONV_HOSTING_SATELLITE);
	CHECK(target_conversion_plugin_resolve_hosting(XRT_CONV_HOSTING_IN_SERVICE, nullptr, nullptr) ==
	      XRT_CONV_HOSTING_IN_SERVICE);
	// DXR_CONV_<ID>_HOSTING overrides the registry either way.
	CHECK(target_conversion_plugin_resolve_hosting(XRT_CONV_HOSTING_AUTO, "in-service", &from_env) ==
	      XRT_CONV_HOSTING_IN_SERVICE);
	CHECK(from_env);
	CHECK(target_conversion_plugin_resolve_hosting(XRT_CONV_HOSTING_IN_SERVICE, "Satellite", &from_env) ==
	      XRT_CONV_HOSTING_SATELLITE);
	CHECK(from_env);
	// "auto" in the env is a real value: back to the default.
	CHECK(target_conversion_plugin_resolve_hosting(XRT_CONV_HOSTING_IN_SERVICE, "auto", &from_env) ==
	      XRT_CONV_HOSTING_SATELLITE);
	CHECK(from_env);
	// An unrecognised env string is ignored.
	CHECK(target_conversion_plugin_resolve_hosting(XRT_CONV_HOSTING_IN_SERVICE, "inservice", &from_env) ==
	      XRT_CONV_HOSTING_IN_SERVICE);
	CHECK_FALSE(from_env);

	char name[64];
	REQUIRE(target_conversion_plugin_hosting_env_name("sim-conversion", name, sizeof(name)));
	CHECK(std::string(name) == "DXR_CONV_SIM_CONVERSION_HOSTING");
	REQUIRE(target_conversion_plugin_hosting_env_name("com.vendor.depth", name, sizeof(name)));
	CHECK(std::string(name) == "DXR_CONV_COM_VENDOR_DEPTH_HOSTING");
	char tiny[8];
	CHECK_FALSE(target_conversion_plugin_hosting_env_name("sim-conversion", tiny, sizeof(tiny)));
	CHECK_FALSE(target_conversion_plugin_hosting_env_name("", name, sizeof(name)));
}


/*
 *
 * The ABI gate, driven with in-process negotiate stubs.
 *
 */

namespace {

void
noop_destroy(struct xrt_conversion_plugin_instance *)
{}

struct xrt_conversion_plugin_iface g_good_iface;
const struct xrt_conversion_plugin_host_iface *g_seen_host = nullptr;

xrt_result_t
neg_good(uint32_t,
         const struct xrt_conversion_plugin_host_iface *host,
         struct xrt_conversion_plugin_iface **out,
         uint32_t *ver)
{
	g_seen_host = host;
	std::memset(&g_good_iface, 0, sizeof(g_good_iface));
	g_good_iface.struct_size = (uint32_t)sizeof(g_good_iface);
	g_good_iface.id = "stub";
	g_good_iface.destroy = noop_destroy;
	*out = &g_good_iface;
	*ver = XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT;
	return XRT_SUCCESS;
}

// Built against the next major: fills its iface and reports its own version.
xrt_result_t
neg_next_major_success(uint32_t,
                       const struct xrt_conversion_plugin_host_iface *,
                       struct xrt_conversion_plugin_iface **out,
                       uint32_t *ver)
{
	neg_good(0, nullptr, out, ver);
	*ver = XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT + 1;
	return XRT_SUCCESS;
}

// Built against the next major and refusing ours with an error.
xrt_result_t
neg_next_major_refuses(uint32_t,
                       const struct xrt_conversion_plugin_host_iface *,
                       struct xrt_conversion_plugin_iface **out,
                       uint32_t *ver)
{
	*out = nullptr;
	*ver = XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT + 1;
	return XRT_ERROR_FEATURE_NOT_SUPPORTED;
}

// Success but never wrote a version: the vtable shape is unknown.
xrt_result_t
neg_no_version(uint32_t,
               const struct xrt_conversion_plugin_host_iface *,
               struct xrt_conversion_plugin_iface **out,
               uint32_t *ver)
{
	neg_good(0, nullptr, out, ver);
	*ver = 0;
	return XRT_SUCCESS;
}

xrt_result_t
neg_decline(uint32_t,
            const struct xrt_conversion_plugin_host_iface *,
            struct xrt_conversion_plugin_iface **out,
            uint32_t *ver)
{
	*out = nullptr;
	*ver = XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT;
	return XRT_ERROR_PROBER_NOT_SUPPORTED;
}

xrt_result_t
neg_null_iface(uint32_t,
               const struct xrt_conversion_plugin_host_iface *,
               struct xrt_conversion_plugin_iface **out,
               uint32_t *ver)
{
	*out = nullptr;
	*ver = XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT;
	return XRT_SUCCESS;
}

xrt_result_t
neg_short_iface(uint32_t,
                const struct xrt_conversion_plugin_host_iface *,
                struct xrt_conversion_plugin_iface **out,
                uint32_t *ver)
{
	neg_good(0, nullptr, out, ver);
	g_good_iface.struct_size = (uint32_t)offsetof(struct xrt_conversion_plugin_iface, destroy);
	return XRT_SUCCESS;
}

} // namespace

TEST_CASE("conversion loader: the ABI gate classifies every negotiate outcome", "[conversion_loader]")
{
	Entry e = make_entry("stub", 100);

	REQUIRE(target_conversion_plugin_negotiate_with(&e, neg_good, nullptr));
	CHECK(e.status == XRT_CONV_LOAD_NEGOTIATED);
	CHECK(e.iface == &g_good_iface);
	CHECK(e.plugin_api_version == XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT);
	CHECK(e.module == nullptr); // the gate never loads anything
	// NULL host = the default, process-lifetime host whose slots are all set.
	REQUIRE(g_seen_host == target_conversion_plugin_default_host());
	CHECK(g_seen_host->struct_size == sizeof(struct xrt_conversion_plugin_host_iface));
	CHECK(g_seen_host->host_api_version == XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT);
	CHECK(g_seen_host->get_setting != nullptr);
	char buf[8] = "x";
	CHECK_FALSE(g_seen_host->get_setting("ENABLE", buf, sizeof(buf)));
	CHECK(buf[0] == '\0');

	struct
	{
		const char *what;
		xrt_conversion_plugin_negotiate_fn_t fn;
		uint32_t status;
	} cases[] = {
	    {"next major, success", neg_next_major_success, XRT_CONV_LOAD_ABI_MISMATCH},
	    {"next major, refuses", neg_next_major_refuses, XRT_CONV_LOAD_ABI_MISMATCH},
	    {"no version written", neg_no_version, XRT_CONV_LOAD_ABI_MISMATCH},
	    {"declined", neg_decline, XRT_CONV_LOAD_DECLINED},
	    {"null iface", neg_null_iface, XRT_CONV_LOAD_NEGOTIATE_FAILED},
	    {"iface shorter than v1", neg_short_iface, XRT_CONV_LOAD_NEGOTIATE_FAILED},
	    {"no entry point", nullptr, XRT_CONV_LOAD_NO_ENTRYPOINT},
	};
	for (const auto &c : cases) {
		INFO(c.what);
		Entry f = make_entry("stub", 100);
		CHECK_FALSE(target_conversion_plugin_negotiate_with(&f, c.fn, nullptr));
		CHECK(f.status == c.status);
		CHECK(f.iface == nullptr);
		CHECK(f.reason[0] != '\0');
	}
}


/*
 *
 * Real binaries: the stub modules built next to this test.
 *
 */

#if defined(CONV_STUB_OK_PATH) && defined(CONV_STUB_ABI_PATH) && defined(CONV_STUB_DP_PATH)

TEST_CASE("conversion loader: negotiate_one loads, gates and unloads real binaries", "[conversion_loader]")
{
	// Stubs live in the build tree, which the #952 dev-path guard refuses by
	// design; the documented override lets a test (and a dev) load them.
	set_env("DXR_ALLOW_DEV_PLUGIN_PATHS", "1");
	// Unsigned + unlisted, and CI runs elevated: pin the provenance gate to
	// audit (ADR-054 N4b PR B) — this test is about the ABI gate.
	target_component_trust_set_enforce_override(0);

	Entry ok = make_entry("stub-conversion", 250);
	std::snprintf(ok.binary_path, sizeof(ok.binary_path), "%s", CONV_STUB_OK_PATH);
	REQUIRE(target_conversion_plugin_negotiate_one(&ok, nullptr));
	CHECK(ok.status == XRT_CONV_LOAD_NEGOTIATED);
	REQUIRE(ok.iface != nullptr);
	CHECK(std::string(ok.iface->id) == "stub-conversion");
	CHECK(ok.module != nullptr);
	CHECK(ok.plugin_api_version == XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT);
	// Idempotent.
	CHECK(target_conversion_plugin_negotiate_one(&ok, nullptr));
	target_conversion_plugin_unload(&ok);
	CHECK(ok.status == XRT_CONV_LOAD_NOT_LOADED);
	CHECK(ok.iface == nullptr);
	CHECK(ok.module == nullptr);
	target_conversion_plugin_unload(&ok); // safe twice

	// Built against another ABI major: rejected before any dispatch.
	Entry abi = make_entry("stub-abi", 250);
	std::snprintf(abi.binary_path, sizeof(abi.binary_path), "%s", CONV_STUB_ABI_PATH);
	CHECK_FALSE(target_conversion_plugin_negotiate_one(&abi, nullptr));
	CHECK(abi.status == XRT_CONV_LOAD_ABI_MISMATCH);
	CHECK(abi.plugin_api_version == XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT + 1);
	CHECK(abi.iface == nullptr);
	CHECK(abi.module == nullptr);

	// A display-processor DLL registered as a conversion provider: no entry point.
	Entry dp = make_entry("stub-dp", 250);
	std::snprintf(dp.binary_path, sizeof(dp.binary_path), "%s", CONV_STUB_DP_PATH);
	CHECK_FALSE(target_conversion_plugin_negotiate_one(&dp, nullptr));
	CHECK(dp.status == XRT_CONV_LOAD_NO_ENTRYPOINT);
	CHECK(dp.module == nullptr);

	// Missing file.
	Entry gone = make_entry("gone", 250);
	std::snprintf(gone.binary_path, sizeof(gone.binary_path), "%s.does-not-exist", CONV_STUB_OK_PATH);
	CHECK_FALSE(target_conversion_plugin_negotiate_one(&gone, nullptr));
	CHECK(gone.status == XRT_CONV_LOAD_LOAD_FAILED);

	// Disabled rows are never loaded.
	Entry off = make_entry("off", 250);
	std::snprintf(off.binary_path, sizeof(off.binary_path), "%s", CONV_STUB_OK_PATH);
	off.enabled = false;
	off.flags |= XRT_CONV_ENTRY_FLAG_DISABLED;
	CHECK_FALSE(target_conversion_plugin_negotiate_one(&off, nullptr));
	CHECK(off.status == XRT_CONV_LOAD_DISABLED);
	CHECK(off.module == nullptr);

	set_env("DXR_ALLOW_DEV_PLUGIN_PATHS", nullptr);
	target_component_trust_set_enforce_override(-1);
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

} // namespace

TEST_CASE("conversion loader: Windows registry rows are read in full", "[conversion_loader]")
{
	wchar_t wroot[128];
	swprintf(wroot, 128, L"Software\\DisplayXR-Test\\ConversionLoader-%lu", GetCurrentProcessId());
	char root[128];
	std::snprintf(root, sizeof(root), "Software\\DisplayXR-Test\\ConversionLoader-%lu", GetCurrentProcessId());

	HKEY r = reg_create(HKEY_CURRENT_USER, wroot);

	// A full row.
	HKEY a = reg_create(r, L"acme-depth");
	reg_sz(a, L"Binary", L"C:\\Program Files\\Acme\\AcmeConv.dll");
	reg_sz(a, L"DisplayName", L"Acme Depth");
	reg_sz(a, L"Vendor", L"Acme");
	reg_sz(a, L"Version", L"1.2.3");
	reg_dword(a, L"ProbeOrder", 50);
	reg_dword(a, L"Enabled", 1);
	reg_sz(a, L"Hosting", L"in-service");
	reg_sz(a, L"Supersedes", L"dp-lift");
	const wchar_t packs[] = L"C:\\p\\a.json\0C:\\p\\b.json\0";
	RegSetValueExW(a, L"Packs", 0, REG_MULTI_SZ, reinterpret_cast<const BYTE *>(packs), sizeof(packs));
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

	// The spec's policy subkeys (§3.1 root values), in odd case: never rows.
	HKEY pol = reg_create(r, L"Policy");
	reg_dword(pol, L"Lock", 1);
	RegCloseKey(pol);
	HKEY pref = reg_create(r, L"PREFERREDPROVIDER");
	reg_sz(pref, L"views-mono", L"acme-depth");
	RegCloseKey(pref);
	HKEY pw = reg_create(r, L"prewarm");
	reg_dword(pw, L"depth-video", 0);
	RegCloseKey(pw);
	// Root values the spec also names do not show up as subkeys at all.
	reg_dword(r, L"AllowResearchOnly", 0);
	reg_dword(r, L"KeepAliveSec", 0);
	RegCloseKey(r);

	std::vector<Entry> out(XRT_CONV_LOADER_MAX_ENTRIES);
	int n = target_conversion_plugin_enumerate_registry_key(true, root, out.data(), (int)out.size());
	RegDeleteTreeW(HKEY_CURRENT_USER, wroot);
	RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\DisplayXR-Test"); // only if now empty

	REQUIRE(n == 3);
	target_conversion_plugin_sort(out.data(), n);
	// acme (50) first; "no-binary" and "off" share the default 100 -> by id.
	CHECK(std::string(out[0].id) == "acme-depth");
	CHECK(std::string(out[1].id) == "no-binary");
	CHECK(std::string(out[2].id) == "off");

	const Entry &e = out[0];
	CHECK(e.status == XRT_CONV_LOAD_NOT_LOADED);
	CHECK(std::string(e.binary_path) == "C:\\Program Files\\Acme\\AcmeConv.dll");
	CHECK(std::string(e.display_name) == "Acme Depth");
	CHECK(std::string(e.vendor) == "Acme");
	CHECK(std::string(e.version) == "1.2.3");
	CHECK(e.probe_order == 50u);
	CHECK(e.enabled);
	CHECK(e.hosting == XRT_CONV_HOSTING_IN_SERVICE);
	CHECK(std::string(e.supersedes) == "dp-lift");
	REQUIRE(e.pack_count == 2u);
	CHECK(std::string(e.packs[0]) == "C:\\p\\a.json");
	CHECK(std::string(e.packs[1]) == "C:\\p\\b.json");
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

	CHECK(out[1].status == XRT_CONV_LOAD_MALFORMED);
	CHECK(std::string(out[1].reason).find("Binary") != std::string::npos);

	CHECK(out[2].status == XRT_CONV_LOAD_DISABLED);
	CHECK_FALSE(out[2].enabled);
	CHECK(out[2].probe_order == XRT_CONV_LOADER_DEFAULT_PROBE_ORDER);
	CHECK(std::string(out[2].display_name) == "off");
}

#endif

TEST_CASE("conversion loader: the production enumeration is well-formed on this box", "[conversion_loader]")
{
	// Reads the real discovery root (read-only); its content is box-dependent,
	// so only the invariants are checked.
	std::vector<Entry> out(XRT_CONV_LOADER_MAX_ENTRIES);
	int n = target_conversion_plugin_enumerate(out.data(), (int)out.size());
	REQUIRE(n >= 0);
	REQUIRE(n <= (int)out.size());
	for (int i = 1; i < n; i++) {
		const bool ordered =
		    out[i - 1].probe_order < out[i].probe_order ||
		    (out[i - 1].probe_order == out[i].probe_order && std::strcmp(out[i - 1].id, out[i].id) <= 0);
		CHECK(ordered);
	}
	for (int i = 0; i < n; i++) {
		CHECK(out[i].iface == nullptr);
		CHECK(out[i].module == nullptr);
	}
	CHECK(target_conversion_plugin_enumerate(nullptr, 4) == 0);
	CHECK(target_conversion_plugin_enumerate(out.data(), 0) == 0);
}
