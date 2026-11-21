// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief ADR-054 N5 / ADR-064 §5: the licence-state contract every component
 *        class shares, the activation hand-over, and the `secret` setting
 *        type.
 *
 * What must hold:
 *  - a component that reports nothing reads `not_applicable` and never
 *    blocks; only `unlicensed` and `expired` block (selection falls through);
 *  - whatever a third-party binary writes is sanitised before it is used
 *    (unknown state / source, unterminated strings, unknown activation bits);
 *  - the conversion class's mapping: a blocking licence state reads
 *    XRT_CONV_STATE_UNLICENSED / XRT_CONV_FAIL_LICENCE on a capability, and a
 *    non-blocking one leaves the capability's own state alone;
 *  - an activation token is bounded, handed over untouched, and an absent
 *    slot is "unsupported", not a failure of the component;
 *  - a `secret` is redacted by declared type (and by name when undeclared),
 *    may be longer than an ordinary value, and round-trips through the same
 *    per-user store, which stays owner-only;
 *  - the reference script (`sim_license.h`): each LICENSE_SIM state, the
 *    key, and activation flipping `unlicensed` to `licensed`.
 *
 * Host-only, no GPU. The live round trip redirects HOME / XDG_DATA_HOME and
 * XRT_PLUGIN_SEARCH_PATH to throwaway directories.
 */

#include "target_component_loader.h"

#include "xrt/xrt_component_license.h"
#include "xrt/xrt_component_settings.h"
#include "xrt/xrt_conversion_plugin.h"

#include "util/u_status_snapshot.h"

#include "sim_common/sim_license.h"

#include "catch_amalgamated.hpp"

#include <cjson/cJSON.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif


namespace {

using License = ::xrt_component_license_state;

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

/*
 * Fake slots: what a component could answer.
 */

bool
slot_says_nothing(License *out)
{
	(void)out;
	return false;
}

bool
slot_trial(License *out)
{
	out->state = XRT_COMPONENT_LICENSE_TRIAL;
	out->source = XRT_COMPONENT_LICENSE_SOURCE_TRIAL;
	out->expiry_unix_s = 4102444800ll;
	std::snprintf(out->message, sizeof(out->message), "12 days left");
	std::snprintf(out->purchase_url, sizeof(out->purchase_url), "https://example.invalid/buy");
	out->activation = XRT_COMPONENT_LICENSE_ACTIVATION_DEEP_LINK;
	return true;
}

//! A hostile answer: values out of range, strings with no terminator, struct_size overwritten.
bool
slot_garbage(License *out)
{
	std::memset(out, 0x7f, sizeof(*out));
	return true;
}

std::string g_seen_token;
uint32_t g_seen_size = 0;
int g_activate_calls = 0;

bool
slot_activate(const char *token, uint32_t token_size, License *out_state, char *out_error, uint32_t error_size)
{
	g_activate_calls++;
	g_seen_token.assign(token, token_size);
	g_seen_size = token_size;
	if (g_seen_token == "good") {
		out_state->state = XRT_COMPONENT_LICENSE_LICENSED;
		out_state->source = XRT_COMPONENT_LICENSE_SOURCE_KEY;
		return true;
	}
	out_state->state = XRT_COMPONENT_LICENSE_UNLICENSED;
	std::snprintf(out_error, error_size, "that key is for another product");
	return false;
}

std::string
json_of(cJSON *o)
{
	char *s = cJSON_PrintUnformatted(o);
	std::string out = s != nullptr ? s : "";
	cJSON_free(s);
	return out;
}

} // namespace


/*
 *
 * Vocabulary.
 *
 */

TEST_CASE("component licence: only unlicensed and expired block", "[component_license]")
{
	CHECK_FALSE(xrt_component_license_blocks(XRT_COMPONENT_LICENSE_NOT_APPLICABLE));
	CHECK_FALSE(xrt_component_license_blocks(XRT_COMPONENT_LICENSE_LICENSED));
	CHECK_FALSE(xrt_component_license_blocks(XRT_COMPONENT_LICENSE_TRIAL));
	CHECK(xrt_component_license_blocks(XRT_COMPONENT_LICENSE_EXPIRED));
	CHECK(xrt_component_license_blocks(XRT_COMPONENT_LICENSE_UNLICENSED));
	CHECK_FALSE(xrt_component_license_blocks(99)); // an unknown value never blocks

	// The values cross four plug-in ABIs and two satellite channels.
	STATIC_REQUIRE(XRT_COMPONENT_LICENSE_NOT_APPLICABLE == 0u);
	STATIC_REQUIRE(XRT_COMPONENT_LICENSE_LICENSED == 1u);
	STATIC_REQUIRE(XRT_COMPONENT_LICENSE_TRIAL == 2u);
	STATIC_REQUIRE(XRT_COMPONENT_LICENSE_EXPIRED == 3u);
	STATIC_REQUIRE(XRT_COMPONENT_LICENSE_UNLICENSED == 4u);
	STATIC_REQUIRE(XRT_COMPONENT_LICENSE_SOURCE_HARDWARE == 1u);
	STATIC_REQUIRE(XRT_COMPONENT_LICENSE_SOURCE_OEM == 4u);
	STATIC_REQUIRE(sizeof(License) == 4 * 4 + 8 + 128 + 256);
	STATIC_REQUIRE(offsetof(License, expiry_unix_s) == 16);
	STATIC_REQUIRE(offsetof(License, message) == 24);
	STATIC_REQUIRE(offsetof(License, purchase_url) == 152);

	CHECK(std::string(xrt_component_license_state_str(XRT_COMPONENT_LICENSE_NOT_APPLICABLE)) == "not_applicable");
	CHECK(std::string(xrt_component_license_state_str(XRT_COMPONENT_LICENSE_LICENSED)) == "licensed");
	CHECK(std::string(xrt_component_license_state_str(XRT_COMPONENT_LICENSE_TRIAL)) == "trial");
	CHECK(std::string(xrt_component_license_state_str(XRT_COMPONENT_LICENSE_EXPIRED)) == "expired");
	CHECK(std::string(xrt_component_license_state_str(XRT_COMPONENT_LICENSE_UNLICENSED)) == "unlicensed");
	CHECK(std::string(xrt_component_license_source_str(XRT_COMPONENT_LICENSE_SOURCE_HARDWARE)) == "hardware");
	CHECK(std::string(xrt_component_license_source_str(XRT_COMPONENT_LICENSE_SOURCE_KEY)) == "key");
	CHECK(std::string(xrt_component_license_source_str(XRT_COMPONENT_LICENSE_SOURCE_TRIAL)) == "trial");
	CHECK(std::string(xrt_component_license_source_str(XRT_COMPONENT_LICENSE_SOURCE_OEM)) == "oem");
	CHECK(std::string(xrt_component_license_source_str(XRT_COMPONENT_LICENSE_SOURCE_NONE)).empty());
}

TEST_CASE("component licence: a component that implements nothing is not_applicable", "[component_license]")
{
	License st;
	std::memset(&st, 0xAA, sizeof(st));

	// No slot at all (an existing provider): nothing to report, nothing blocks.
	CHECK(xrt_component_license_query(nullptr, &st) == XRT_COMPONENT_LICENSE_NOT_APPLICABLE);
	CHECK(st.struct_size == sizeof(st));
	CHECK(st.state == XRT_COMPONENT_LICENSE_NOT_APPLICABLE);
	CHECK(st.message[0] == '\0');
	CHECK(st.purchase_url[0] == '\0');
	CHECK_FALSE(xrt_component_license_blocks(st.state));

	// A slot that returns false: the same.
	std::memset(&st, 0xAA, sizeof(st));
	CHECK(xrt_component_license_query(slot_says_nothing, &st) == XRT_COMPONENT_LICENSE_NOT_APPLICABLE);
	CHECK(st.state == XRT_COMPONENT_LICENSE_NOT_APPLICABLE);

	// A slot that answers: carried as given.
	CHECK(xrt_component_license_query(slot_trial, &st) == XRT_COMPONENT_LICENSE_TRIAL);
	CHECK(st.source == XRT_COMPONENT_LICENSE_SOURCE_TRIAL);
	CHECK(st.expiry_unix_s == 4102444800ll);
	CHECK(std::string(st.message) == "12 days left");
	CHECK(std::string(st.purchase_url) == "https://example.invalid/buy");
	CHECK(st.activation == XRT_COMPONENT_LICENSE_ACTIVATION_DEEP_LINK);
}

TEST_CASE("component licence: a hostile answer is sanitised", "[component_license]")
{
	License st;
	const uint32_t state = xrt_component_license_query(slot_garbage, &st);
	CHECK(state == XRT_COMPONENT_LICENSE_NOT_APPLICABLE); // out of range reads as nothing
	CHECK(st.struct_size == sizeof(st));
	CHECK(st.source == XRT_COMPONENT_LICENSE_SOURCE_NONE);
	CHECK((st.activation & ~(XRT_COMPONENT_LICENSE_ACTIVATION_KEY | XRT_COMPONENT_LICENSE_ACTIVATION_DEEP_LINK)) ==
	      0u);
	CHECK(std::strlen(st.message) == sizeof(st.message) - 1);
	CHECK(std::strlen(st.purchase_url) == sizeof(st.purchase_url) - 1);
}

TEST_CASE("component licence: the conversion mapping onto UNLICENSED", "[component_license]")
{
	struct xrt_conv_state st;
	std::memset(&st, 0, sizeof(st));
	st.struct_size = (uint32_t)sizeof(st);
	st.state = XRT_CONV_STATE_READY;
	st.retryable = 1;

	// Nothing to report / licensed / trial: the capability's own state stands.
	for (uint32_t ok :
	     {XRT_COMPONENT_LICENSE_NOT_APPLICABLE, XRT_COMPONENT_LICENSE_LICENSED, XRT_COMPONENT_LICENSE_TRIAL}) {
		CHECK_FALSE(xrt_conv_state_apply_license(ok, &st));
		CHECK(st.state == XRT_CONV_STATE_READY);
	}
	// Unlicensed / expired: every capability reads UNLICENSED, which selection falls through.
	for (uint32_t blocked : {XRT_COMPONENT_LICENSE_UNLICENSED, XRT_COMPONENT_LICENSE_EXPIRED}) {
		st.state = XRT_CONV_STATE_READY;
		st.failure_reason = 0;
		st.retryable = 1;
		CHECK(xrt_conv_state_apply_license(blocked, &st));
		CHECK(st.state == XRT_CONV_STATE_UNLICENSED);
		CHECK(st.failure_reason == XRT_CONV_FAIL_LICENCE);
		CHECK(st.retryable == 0);
	}
	CHECK_FALSE(xrt_conv_state_apply_license(XRT_COMPONENT_LICENSE_UNLICENSED, nullptr));
}


/*
 *
 * Activation.
 *
 */

TEST_CASE("component licence: activation hands the token over untouched", "[component_license]")
{
	License st;
	char err[XRT_COMPONENT_ACTIVATION_ERROR_MAX];

	// No slot: unsupported, and nothing is called.
	g_activate_calls = 0;
	CHECK(target_component_activate_call(nullptr, "good", &st, err, sizeof(err)) ==
	      TARGET_COMPONENT_ACTIVATE_UNSUPPORTED);
	CHECK(st.state == XRT_COMPONENT_LICENSE_NOT_APPLICABLE);

	// Empty or oversize: never delivered.
	CHECK(target_component_activate_call(slot_activate, "", &st, err, sizeof(err)) ==
	      TARGET_COMPONENT_ACTIVATE_BAD_TOKEN);
	CHECK(target_component_activate_call(slot_activate, nullptr, &st, err, sizeof(err)) ==
	      TARGET_COMPONENT_ACTIVATE_BAD_TOKEN);
	const std::string too_long(XRT_COMPONENT_ACTIVATION_TOKEN_MAX, 'x');
	CHECK(target_component_activate_call(slot_activate, too_long.c_str(), &st, err, sizeof(err)) ==
	      TARGET_COMPONENT_ACTIVATE_BAD_TOKEN);
	CHECK(g_activate_calls == 0);

	// The largest token that fits arrives whole.
	const std::string biggest(XRT_COMPONENT_ACTIVATION_TOKEN_MAX - 1, 'k');
	CHECK(target_component_activate_call(slot_activate, biggest.c_str(), &st, err, sizeof(err)) ==
	      TARGET_COMPONENT_ACTIVATE_REFUSED);
	CHECK(g_seen_size == XRT_COMPONENT_ACTIVATION_TOKEN_MAX - 1);
	CHECK(g_seen_token == biggest);
	CHECK(std::string(err) == "that key is for another product");
	CHECK(st.state == XRT_COMPONENT_LICENSE_UNLICENSED);

	// Accepted: the new state comes back, the error stays empty.
	CHECK(target_component_activate_call(slot_activate, "good", &st, err, sizeof(err)) ==
	      TARGET_COMPONENT_ACTIVATE_OK);
	CHECK(st.state == XRT_COMPONENT_LICENSE_LICENSED);
	CHECK(st.source == XRT_COMPONENT_LICENSE_SOURCE_KEY);
	CHECK(st.struct_size == sizeof(st));
	CHECK(err[0] == '\0');

	// The caller may want neither the state nor the error.
	CHECK(target_component_activate_call(slot_activate, "good", nullptr, nullptr, 0) ==
	      TARGET_COMPONENT_ACTIVATE_OK);

	CHECK(std::string(target_component_activate_result_str(TARGET_COMPONENT_ACTIVATE_OK)) == "ok");
	CHECK(std::string(target_component_activate_result_str(TARGET_COMPONENT_ACTIVATE_REFUSED)) == "refused");
	CHECK(std::string(target_component_activate_result_str(TARGET_COMPONENT_ACTIVATE_UNSUPPORTED)) ==
	      "unsupported");
	CHECK(std::string(target_component_activate_result_str(TARGET_COMPONENT_ACTIVATE_BAD_TOKEN)) == "bad-token");
	CHECK(std::string(target_component_activate_result_str(TARGET_COMPONENT_ACTIVATE_NOT_LOADED)) == "not-loaded");
}


/*
 *
 * The reference script (sim_license.h): what sim-conversion / sim-tracking report.
 *
 */

TEST_CASE("component licence: the reference script reports each state", "[component_license][sim_license]")
{
	struct sim_license c;
	License st;

	// Off (the default): the slot reports nothing, so the provider is not_applicable.
	sim_license_load(&c, nullptr, nullptr);
	xrt_component_license_state_init(&st);
	CHECK_FALSE(sim_license_get(&c, &st));
	sim_license_load(&c, "", SIM_LICENSE_TEST_KEY); // a key alone scripts nothing
	CHECK_FALSE(sim_license_get(&c, &st));
	sim_license_load(&c, "bogus", nullptr);
	CHECK_FALSE(sim_license_get(&c, &st));

	sim_license_load(&c, "licensed", nullptr);
	xrt_component_license_state_init(&st);
	REQUIRE(sim_license_get(&c, &st));
	CHECK(st.state == XRT_COMPONENT_LICENSE_LICENSED);
	CHECK(st.source == XRT_COMPONENT_LICENSE_SOURCE_HARDWARE); // "included with your display"
	CHECK(st.purchase_url[0] == '\0');

	sim_license_load(&c, "trial", nullptr);
	xrt_component_license_state_init(&st);
	REQUIRE(sim_license_get(&c, &st));
	CHECK(st.state == XRT_COMPONENT_LICENSE_TRIAL);
	CHECK(st.source == XRT_COMPONENT_LICENSE_SOURCE_TRIAL);
	CHECK(st.expiry_unix_s == SIM_LICENSE_TRIAL_END_UNIX_S);
	CHECK_FALSE(xrt_component_license_blocks(st.state));

	sim_license_load(&c, "expired", nullptr);
	xrt_component_license_state_init(&st);
	REQUIRE(sim_license_get(&c, &st));
	CHECK(st.state == XRT_COMPONENT_LICENSE_EXPIRED);
	CHECK(st.expiry_unix_s == SIM_LICENSE_EXPIRED_AT_UNIX_S);
	CHECK(xrt_component_license_blocks(st.state));

	sim_license_load(&c, "unlicensed", nullptr);
	xrt_component_license_state_init(&st);
	REQUIRE(sim_license_get(&c, &st));
	CHECK(st.state == XRT_COMPONENT_LICENSE_UNLICENSED);
	CHECK(xrt_component_license_blocks(st.state));
	CHECK(std::string(st.purchase_url) == SIM_LICENSE_PURCHASE_URL);
	CHECK((st.activation & XRT_COMPONENT_LICENSE_ACTIVATION_KEY) != 0u);
	CHECK((st.activation & XRT_COMPONENT_LICENSE_ACTIVATION_DEEP_LINK) != 0u);

	// The key setting (a `secret`) turns unlicensed into licensed-by-key.
	sim_license_load(&c, "unlicensed", "wrong");
	xrt_component_license_state_init(&st);
	REQUIRE(sim_license_get(&c, &st));
	CHECK(st.state == XRT_COMPONENT_LICENSE_UNLICENSED);
	sim_license_load(&c, "unlicensed", SIM_LICENSE_TEST_KEY);
	xrt_component_license_state_init(&st);
	REQUIRE(sim_license_get(&c, &st));
	CHECK(st.state == XRT_COMPONENT_LICENSE_LICENSED);
	CHECK(st.source == XRT_COMPONENT_LICENSE_SOURCE_KEY);
	CHECK(st.expiry_unix_s == 0);

	// A caller compiled against a shorter struct is never overrun.
	sim_license_load(&c, "unlicensed", nullptr);
	alignas(License) unsigned char guarded[sizeof(License) + 16];
	std::memset(guarded, 0xEE, sizeof(guarded));
	License *small = reinterpret_cast<License *>(guarded);
	small->struct_size = 24; // header + expiry only
	REQUIRE(sim_license_get(&c, small));
	CHECK(small->struct_size == 24);
	CHECK(small->state == XRT_COMPONENT_LICENSE_UNLICENSED);
	CHECK(guarded[24] == 0xEE); // nothing past the announced size
}

TEST_CASE("component licence: activation flips the reference script's state", "[component_license][sim_license]")
{
	struct sim_license c;
	License st;
	char err[XRT_COMPONENT_ACTIVATION_ERROR_MAX] = {0};
	const uint32_t key_len = (uint32_t)std::strlen(SIM_LICENSE_TEST_KEY);

	sim_license_load(&c, "unlicensed", nullptr);
	xrt_component_license_state_init(&st);
	REQUIRE(sim_license_get(&c, &st));
	REQUIRE(st.state == XRT_COMPONENT_LICENSE_UNLICENSED);

	// A wrong token: refused with a reason, the state unchanged.
	xrt_component_license_state_init(&st);
	CHECK_FALSE(sim_license_activate(&c, "nope", 4, &st, err, sizeof(err)));
	CHECK(std::string(err) == "not the simulated test key");
	CHECK(st.state == XRT_COMPONENT_LICENSE_UNLICENSED);

	// The test key: accepted, and every later read says licensed by key.
	err[0] = '\0';
	xrt_component_license_state_init(&st);
	CHECK(sim_license_activate(&c, SIM_LICENSE_TEST_KEY, key_len, &st, err, sizeof(err)));
	CHECK(err[0] == '\0');
	CHECK(st.state == XRT_COMPONENT_LICENSE_LICENSED);
	CHECK(st.source == XRT_COMPONENT_LICENSE_SOURCE_KEY);
	xrt_component_license_state_init(&st);
	REQUIRE(sim_license_get(&c, &st));
	CHECK(st.state == XRT_COMPONENT_LICENSE_LICENSED);
	CHECK_FALSE(xrt_component_license_blocks(st.state));

	// Through the shared helper, the way every class's loader calls the slot.
	sim_license_load(&c, "expired", nullptr);
	static struct sim_license *bound = nullptr;
	bound = &c;
	auto slot = [](const char *token, uint32_t n, License *out, char *e, uint32_t en) -> bool {
		return sim_license_activate(bound, token, n, out, e, en);
	};
	CHECK(target_component_activate_call(slot, "nope", &st, err, sizeof(err)) == TARGET_COMPONENT_ACTIVATE_REFUSED);
	CHECK(st.state == XRT_COMPONENT_LICENSE_EXPIRED);
	CHECK(target_component_activate_call(slot, SIM_LICENSE_TEST_KEY, &st, err, sizeof(err)) ==
	      TARGET_COMPONENT_ACTIVATE_OK);
	CHECK(st.state == XRT_COMPONENT_LICENSE_LICENSED);

	// With the script off there is nothing to activate.
	sim_license_load(&c, nullptr, nullptr);
	CHECK_FALSE(sim_license_activate(&c, SIM_LICENSE_TEST_KEY, key_len, nullptr, err, sizeof(err)));
	CHECK(std::string(err).find("LICENSE_SIM") != std::string::npos);
}


/*
 *
 * JSON: the one `licence` object.
 *
 */

TEST_CASE("component licence: the licence object and its dates", "[component_license]")
{
	char buf[40];
	u_status_unix_time_str(0, true, buf, sizeof(buf));
	CHECK(std::string(buf) == "1970-01-01T00:00:00Z");
	u_status_unix_time_str(4102444800ll, false, buf, sizeof(buf));
	CHECK(std::string(buf) == "2100-01-01");
	u_status_unix_time_str(946684799ll, true, buf, sizeof(buf));
	CHECK(std::string(buf) == "1999-12-31T23:59:59Z");
	u_status_unix_time_str(1709164800ll, false, buf, sizeof(buf)); // a leap day
	CHECK(std::string(buf) == "2024-02-29");
	u_status_unix_time_str(-1, true, buf, sizeof(buf));
	CHECK(std::string(buf) == "1969-12-31T23:59:59Z");
	u_status_unix_time_str(INT64_MAX, true, buf, sizeof(buf)); // out of range: empty, never garbage
	CHECK(std::string(buf).empty());

	License st;
	(void)xrt_component_license_query(slot_trial, &st);
	cJSON *row = cJSON_CreateObject();
	u_status_license_add_cjson(row, &st);
	const std::string trial = json_of(row);
	cJSON_Delete(row);
	CHECK(
	    trial ==
	    R"({"licence":{"state":"trial","source":"trial","expires_unix":4102444800,"expires":"2100-01-01T00:00:00Z",)"
	    R"("message":"12 days left","purchase_url":"https://example.invalid/buy",)"
	    R"("activation":{"key":false,"deep_link":true},"blocks":false}})");

	// Nothing to report: the object is still there, so a consumer never branches on its presence.
	(void)xrt_component_license_query(nullptr, &st);
	row = cJSON_CreateObject();
	u_status_license_add_cjson(row, &st);
	const std::string none = json_of(row);
	cJSON_Delete(row);
	CHECK(none ==
	      R"({"licence":{"state":"not_applicable","source":null,"expires_unix":null,"expires":null,)"
	      R"("message":"","purchase_url":"","activation":{"key":false,"deep_link":false},"blocks":false}})");
}


/*
 *
 * The `secret` setting type.
 *
 */

TEST_CASE("component settings: what counts as a secret", "[component_license][component_settings]")
{
	// Declared: the type decides, whatever the key is called.
	CHECK(target_component_setting_is_secret(XRT_COMPONENT_SETTING_TYPE_SECRET, "ANYTHING"));
	CHECK(target_component_setting_is_secret("SECRET", "ANYTHING"));
	CHECK_FALSE(target_component_setting_is_secret(XRT_COMPONENT_SETTING_TYPE_STRING, "LICENSE_KEY"));
	CHECK_FALSE(target_component_setting_is_secret(XRT_COMPONENT_SETTING_TYPE_BOOL, "ENABLE"));

	// Undeclared (the component could not be loaded to say): the name decides, towards hiding.
	for (const char *k : {"LICENSE_KEY", "license_key", "API_TOKEN", "KEY", "token", "CLIENT_SECRET", "DB_PASSWORD",
	                      "SIGNING_PASSPHRASE"}) {
		INFO(k);
		CHECK(target_component_setting_is_secret(nullptr, k));
		CHECK(target_component_setting_is_secret("", k));
	}
	for (const char *k : {"ENABLE", "MONKEY", "KEYBOARD", "TOKENS_PER_S", "LICENSE_SIM", "CAMERA_KEY_X"}) {
		INFO(k);
		CHECK_FALSE(target_component_setting_is_secret(nullptr, k));
	}
	CHECK_FALSE(target_component_setting_is_secret(nullptr, nullptr));

	STATIC_REQUIRE(XRT_COMPONENT_SETTING_SECRET_MAX == TARGET_COMPONENT_SETTING_SECRET_VALUE_MAX);
	STATIC_REQUIRE(XRT_COMPONENT_SETTING_SECRET_MAX == XRT_COMPONENT_ACTIVATION_TOKEN_MAX);
	STATIC_REQUIRE(TARGET_COMPONENT_SETTING_SECRET_VALUE_MAX > TARGET_COMPONENT_SETTING_VALUE_MAX);
}

#if !defined(_WIN32) && !defined(__ANDROID__)

namespace {

std::string
read_file(const std::string &path)
{
	std::string out;
	FILE *f = std::fopen(path.c_str(), "rb");
	if (f == nullptr) {
		return out;
	}
	char buf[4096];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
		out.append(buf, n);
	}
	std::fclose(f);
	return out;
}

} // namespace

TEST_CASE("component settings: a secret round-trips through the per-user store, owner-only",
          "[component_license][component_settings]")
{
	char tmpl[] = "/tmp/dxr-license-test-XXXXXX";
	REQUIRE(mkdtemp(tmpl) != nullptr);
	const std::string root = tmpl;
	const std::string manifests = root + "/manifests";
	REQUIRE(mkdir(manifests.c_str(), 0755) == 0);
	const char *prev_home = std::getenv("HOME");
	const std::string saved_home = prev_home != nullptr ? prev_home : "";
	const char *prev_xdg = std::getenv("XDG_DATA_HOME");
	const std::string saved_xdg = prev_xdg != nullptr ? prev_xdg : "";
	const char *prev_search = std::getenv("XRT_PLUGIN_SEARCH_PATH");
	const std::string saved_search = prev_search != nullptr ? prev_search : "";
	set_env("HOME", root.c_str());
	set_env("XDG_DATA_HOME", (root + "/xdg").c_str());
	set_env("XRT_PLUGIN_SEARCH_PATH", manifests.c_str());

	::target_component_class cls;
	std::memset(&cls, 0, sizeof(cls));
	cls.name = "test component";
	cls.log_tag = "test loader";
	cls.entrypoint = "xrtTestNegotiate";
	cls.abi_current = 1;
	cls.registry_root = "Software\\DisplayXR-Test\\NoSuchRoot";
	cls.manifest_kind = TARGET_PLUGIN_MANIFEST_DISPLAY;
	cls.env_class = "TEST";

	FILE *mf = std::fopen((manifests + "/050-acme-paid.json").c_str(), "wb");
	REQUIRE(mf != nullptr);
	std::fputs(R"({"file_format_version":"1.0","plugin":{"id":"acme-paid","binary_path":"/opt/acme/libpaid.so",
	  "settings_schema":[{"key":"LICENSE_KEY","type":"secret","default":"","help":"your licence key"}]}})",
	           mf);
	std::fclose(mf);

	// The manifest's schema declares it secret: that is what every reader redacts on.
	target_component_setting_desc descs[4];
	REQUIRE(target_component_manifest_settings_schema(&cls, "acme-paid", descs, 4) == 1);
	CHECK(std::string(descs[0].type) == XRT_COMPONENT_SETTING_TYPE_SECRET);
	CHECK(target_component_setting_is_secret(descs[0].type, descs[0].key));

	// A signed token is longer than an ordinary value: `set` refuses it, `set_secret` takes it.
	const std::string token(1500, 'J');
	CHECK(target_component_set_setting(&cls, "acme-paid", "LICENSE_KEY", token.c_str(), false) ==
	      TARGET_COMPONENT_STORE_FAILED);
	REQUIRE(target_component_set_secret(&cls, "acme-paid", "LICENSE_KEY", token.c_str(), false) ==
	        TARGET_COMPONENT_STORE_OK);
	const std::string longest(TARGET_COMPONENT_SETTING_SECRET_VALUE_MAX - 1, 'L');
	const std::string over(TARGET_COMPONENT_SETTING_SECRET_VALUE_MAX, 'L');
	CHECK(target_component_set_secret(&cls, "acme-paid", "OTHER_KEY", over.c_str(), false) ==
	      TARGET_COMPONENT_STORE_FAILED);
	REQUIRE(target_component_set_secret(&cls, "acme-paid", "OTHER_KEY", longest.c_str(), false) ==
	        TARGET_COMPONENT_STORE_OK);

	// The component reads it whole through the same chain (its host `get_setting`).
	std::string buf(TARGET_COMPONENT_SETTING_SECRET_VALUE_MAX, '\0');
	uint32_t src = 0;
	REQUIRE(target_component_get_setting(&cls, "acme-paid", "LICENSE_KEY", buf.data(), buf.size(), &src));
	CHECK(std::string(buf.c_str()) == token);
	CHECK(src == TARGET_COMPONENT_SETTING_USER);
	REQUIRE(target_component_get_setting(&cls, "acme-paid", "OTHER_KEY", buf.data(), buf.size(), &src));
	CHECK(std::string(buf.c_str()) == longest);

	// The environment override carries one too.
	set_env("DXR_TEST_ACME_PAID_LICENSE_KEY", "from-the-environment");
	REQUIRE(target_component_get_setting(&cls, "acme-paid", "LICENSE_KEY", buf.data(), buf.size(), &src));
	CHECK(std::string(buf.c_str()) == "from-the-environment");
	CHECK(src == TARGET_COMPONENT_SETTING_ENVIRONMENT);
	set_env("DXR_TEST_ACME_PAID_LICENSE_KEY", nullptr);

	// The per-user file is the user's alone.
	char path[512];
	REQUIRE(target_component_setting_store_path(&cls, "acme-paid", false, path, sizeof(path)));
	struct stat sb;
	REQUIRE(stat(path, &sb) == 0);
	CHECK((sb.st_mode & 0777) == 0600);
	CHECK(read_file(path).find(token) != std::string::npos); // stored like any setting; hidden by every reader

	// `unset` clears it like any setting.
	CHECK(target_component_unset_setting(&cls, "acme-paid", "LICENSE_KEY", false) == TARGET_COMPONENT_STORE_OK);
	CHECK(target_component_unset_setting(&cls, "acme-paid", "OTHER_KEY", false) == TARGET_COMPONENT_STORE_OK);
	CHECK_FALSE(target_component_get_setting(&cls, "acme-paid", "LICENSE_KEY", buf.data(), buf.size(), &src));

	std::remove(path);
	std::remove((manifests + "/050-acme-paid.json").c_str());
	set_env("HOME", saved_home.c_str());
	set_env("XDG_DATA_HOME", prev_xdg != nullptr ? saved_xdg.c_str() : nullptr);
	set_env("XRT_PLUGIN_SEARCH_PATH", prev_search != nullptr ? saved_search.c_str() : nullptr);
}

#endif /* desktop POSIX */
