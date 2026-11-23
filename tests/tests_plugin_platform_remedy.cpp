// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pins the platform remedy (ADR-045 Amendment 2026-11-22): the
 *         update-recommended / update-required notice a plug-in reports
 *         through `get_platform_state`. The loader's struct_size-gated query
 *         and sanitiser (`target_plugin_query_iface_platform_state`,
 *         `target_plugin_platform_remedy_from_status`), the plug-in side's
 *         `XRT_PLUGIN_PLATFORM_STATUS_HAS`, and the one human line + JSON
 *         keys the cli, tray and Dashboard read (`u_status_remedy_*`).
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_plugin.h"
#include "util/u_status_snapshot.h"
#include "target_plugin_loader.h"

#include <cjson/cJSON.h>

#include <cstddef>
#include <cstring>
#include <string>

namespace {

// The header's layout promise: the remedy fields come after reserved[8], so
// the pre-amendment prefix (what an older runtime passes) is unchanged.
constexpr size_t kPreAmendmentSize = offsetof(xrt_plugin_platform_status, reserved) + sizeof(uint64_t) * 8;
static_assert(offsetof(xrt_plugin_platform_status, hint) == 16, "hint moved");
static_assert(offsetof(xrt_plugin_platform_status, reserved) == 16 + XRT_PLUGIN_PLATFORM_HINT_MAX, "reserved moved");
static_assert(offsetof(xrt_plugin_platform_status, installed_version) == kPreAmendmentSize,
              "remedy fields must start right after reserved[8]");
static_assert(offsetof(xrt_plugin_platform_status, required_version) ==
                  kPreAmendmentSize + XRT_PLUGIN_PLATFORM_VERSION_MAX,
              "required_version moved");
static_assert(offsetof(xrt_plugin_platform_status, action_label) ==
                  kPreAmendmentSize + 2 * XRT_PLUGIN_PLATFORM_VERSION_MAX,
              "action_label moved");
static_assert(offsetof(xrt_plugin_platform_status, action_url) ==
                  kPreAmendmentSize + 2 * XRT_PLUGIN_PLATFORM_VERSION_MAX + XRT_PLUGIN_PLATFORM_ACTION_LABEL_MAX,
              "action_url moved");
static_assert(sizeof(xrt_plugin_platform_status) == kPreAmendmentSize + 2 * XRT_PLUGIN_PLATFORM_VERSION_MAX +
                                                        XRT_PLUGIN_PLATFORM_ACTION_LABEL_MAX +
                                                        XRT_PLUGIN_PLATFORM_ACTION_URL_MAX,
              "xrt_plugin_platform_status grew past the remedy fields: update this test and ADR-045");
#ifndef XRT_PLUGIN_HAS_PLATFORM_REMEDY
#error "XRT_PLUGIN_HAS_PLATFORM_REMEDY must be defined"
#endif

enum class Mode
{
	OldPlugin,    // predates the amendment: writes state + hint only
	Remedy,       // a well-formed remedy, written under XRT_PLUGIN_PLATFORM_STATUS_HAS
	Unterminated, // fills every remedy string to the brim with no NUL
	HttpUrl,      // a non-https action URL
	UnknownFlags, // only bits the runtime does not know
};
Mode g_mode = Mode::OldPlugin;
uint32_t g_seen_struct_size = 0;

void
fill(char *dst, size_t cap, const char *src)
{
	std::snprintf(dst, cap, "%s", src);
}

bool
fake_get_platform_state(xrt_plugin_platform_status *out)
{
	g_seen_struct_size = out->struct_size;
	out->state = XRT_PLUGIN_PLATFORM_STATE_READY;
	fill(out->hint, sizeof(out->hint), "fine");
	switch (g_mode) {
	case Mode::OldPlugin: break;
	case Mode::Remedy:
		out->flags = XRT_PLUGIN_PLATFORM_FLAG_UPDATE_RECOMMENDED;
		if (XRT_PLUGIN_PLATFORM_STATUS_HAS(out, installed_version)) {
			fill(out->installed_version, sizeof(out->installed_version), "1.2.0");
		}
		if (XRT_PLUGIN_PLATFORM_STATUS_HAS(out, required_version)) {
			fill(out->required_version, sizeof(out->required_version), "1.4.0");
		}
		if (XRT_PLUGIN_PLATFORM_STATUS_HAS(out, action_label)) {
			fill(out->action_label, sizeof(out->action_label), "Download the update");
		}
		if (XRT_PLUGIN_PLATFORM_STATUS_HAS(out, action_url)) {
			fill(out->action_url, sizeof(out->action_url), "https://example.com/update");
		}
		break;
	case Mode::Unterminated:
		out->flags = XRT_PLUGIN_PLATFORM_FLAG_UPDATE_REQUIRED;
		std::memset(out->installed_version, '1', sizeof(out->installed_version));
		std::memset(out->required_version, '2', sizeof(out->required_version));
		std::memset(out->action_label, 'L', sizeof(out->action_label));
		std::memcpy(out->action_url, "https://", 8);
		std::memset(out->action_url + 8, 'u', sizeof(out->action_url) - 8);
		break;
	case Mode::HttpUrl:
		out->flags = XRT_PLUGIN_PLATFORM_FLAG_UPDATE_REQUIRED;
		fill(out->action_label, sizeof(out->action_label), "Get it");
		fill(out->action_url, sizeof(out->action_url), "http://example.com/update");
		break;
	case Mode::UnknownFlags:
		out->flags = 1u << 7;
		fill(out->installed_version, sizeof(out->installed_version), "9");
		break;
	}
	return true;
}

xrt_plugin_iface
fake_iface()
{
	xrt_plugin_iface iface = {};
	iface.struct_size = sizeof(iface);
	iface.id = "remedy-test";
	iface.get_platform_state = fake_get_platform_state;
	return iface;
}

xrt_status_platform_remedy
query(Mode mode)
{
	g_mode = mode;
	xrt_plugin_iface iface = fake_iface();
	xrt_plugin_platform_status ps;
	REQUIRE(target_plugin_query_iface_platform_state(&iface, &ps));
	CHECK(ps.state == XRT_PLUGIN_PLATFORM_STATE_READY);
	CHECK(g_seen_struct_size == sizeof(xrt_plugin_platform_status));
	xrt_status_platform_remedy r;
	(void)target_plugin_platform_remedy_from_status(&ps, &r);
	return r;
}

std::string
line(const xrt_status_platform_remedy &r)
{
	char buf[512];
	u_status_remedy_format(&r, buf, sizeof(buf));
	return buf;
}

xrt_status_platform_remedy
remedy(bool required, const char *from, const char *to, const char *label, const char *url)
{
	xrt_status_platform_remedy r = {};
	r.update_required = required;
	r.update_recommended = !required;
	fill(r.installed_version, sizeof(r.installed_version), from);
	fill(r.required_version, sizeof(r.required_version), to);
	fill(r.action_label, sizeof(r.action_label), label);
	fill(r.action_url, sizeof(r.action_url), url);
	return r;
}

} // namespace

TEST_CASE("remedy: a plug-in that predates the amendment reports none", "[remedy]")
{
	const xrt_status_platform_remedy r = query(Mode::OldPlugin);
	CHECK_FALSE(r.update_recommended);
	CHECK_FALSE(r.update_required);
	CHECK(r.installed_version[0] == '\0');
	CHECK(r.action_url[0] == '\0');
	CHECK(line(r).empty());
}

TEST_CASE("remedy: a well-formed remedy is copied verbatim", "[remedy]")
{
	const xrt_status_platform_remedy r = query(Mode::Remedy);
	CHECK(r.update_recommended);
	CHECK_FALSE(r.update_required);
	CHECK(std::string(r.installed_version) == "1.2.0");
	CHECK(std::string(r.required_version) == "1.4.0");
	CHECK(std::string(r.action_label) == "Download the update");
	CHECK(std::string(r.action_url) == "https://example.com/update");
	CHECK(line(r) == "Update recommended: 1.2.0 -> 1.4.0. Download the update: https://example.com/update");
}

TEST_CASE("remedy: strings with no NUL are cut and terminated", "[remedy]")
{
	const xrt_status_platform_remedy r = query(Mode::Unterminated);
	CHECK(r.update_required);
	CHECK(std::strlen(r.installed_version) == sizeof(r.installed_version) - 1);
	CHECK(std::strlen(r.required_version) == sizeof(r.required_version) - 1);
	CHECK(std::strlen(r.action_label) == sizeof(r.action_label) - 1);
	CHECK(std::strlen(r.action_url) == sizeof(r.action_url) - 1);
	CHECK(std::strncmp(r.action_url, "https://uuu", 11) == 0);
}

TEST_CASE("remedy: a non-https URL is dropped, the notice stays", "[remedy]")
{
	const xrt_status_platform_remedy r = query(Mode::HttpUrl);
	CHECK(r.update_required);
	CHECK(std::string(r.action_label) == "Get it");
	CHECK(r.action_url[0] == '\0');
	CHECK(line(r) == "Update required. Get it");
}

TEST_CASE("remedy: unknown flag bits are not a remedy", "[remedy]")
{
	const xrt_status_platform_remedy r = query(Mode::UnknownFlags);
	CHECK_FALSE(r.update_recommended);
	CHECK_FALSE(r.update_required);
	CHECK(r.installed_version[0] == '\0'); // strings only travel with a remedy bit
}

TEST_CASE("remedy: an older runtime's struct_size hides the new fields from the plug-in", "[remedy]")
{
	xrt_plugin_platform_status ps = {};
	ps.struct_size = (uint32_t)kPreAmendmentSize;
	CHECK(XRT_PLUGIN_PLATFORM_STATUS_HAS(&ps, hint));
	CHECK(XRT_PLUGIN_PLATFORM_STATUS_HAS(&ps, reserved));
	CHECK_FALSE(XRT_PLUGIN_PLATFORM_STATUS_HAS(&ps, installed_version));
	CHECK_FALSE(XRT_PLUGIN_PLATFORM_STATUS_HAS(&ps, action_url));
	ps.struct_size = (uint32_t)sizeof(ps);
	CHECK(XRT_PLUGIN_PLATFORM_STATUS_HAS(&ps, action_url));
	const xrt_plugin_platform_status *none = nullptr;
	CHECK_FALSE(XRT_PLUGIN_PLATFORM_STATUS_HAS(none, hint));
}

TEST_CASE("remedy: the URL rule", "[remedy]")
{
	CHECK(target_plugin_remedy_url_ok("https://example.com"));
	CHECK(target_plugin_remedy_url_ok("HTTPS://example.com/a?b=c#d"));
	CHECK_FALSE(target_plugin_remedy_url_ok(nullptr));
	CHECK_FALSE(target_plugin_remedy_url_ok(""));
	CHECK_FALSE(target_plugin_remedy_url_ok("https://"));
	CHECK_FALSE(target_plugin_remedy_url_ok("https:///path"));
	CHECK_FALSE(target_plugin_remedy_url_ok("http://example.com"));
	CHECK_FALSE(target_plugin_remedy_url_ok("file:///C:/x.exe"));
	CHECK_FALSE(target_plugin_remedy_url_ok("javascript:alert(1)"));
	CHECK_FALSE(target_plugin_remedy_url_ok("https://example.com/a b"));
	CHECK_FALSE(target_plugin_remedy_url_ok("https://example.com/\"x"));
	CHECK_FALSE(target_plugin_remedy_url_ok("https://example.com\\evil"));
}

TEST_CASE("remedy: the human line omits empty parts", "[remedy]")
{
	CHECK(line(remedy(true, "1.2.0", "1.4.0", "Download", "https://e.com/u")) ==
	      "Update required: 1.2.0 -> 1.4.0. Download: https://e.com/u");
	CHECK(line(remedy(false, "", "1.4.0", "", "")) == "Update recommended: 1.4.0.");
	CHECK(line(remedy(false, "1.2.0", "", "", "")) == "Update recommended: 1.2.0.");
	CHECK(line(remedy(false, "", "", "", "https://e.com/u")) == "Update recommended. https://e.com/u");
	CHECK(line(remedy(false, "", "", "", "")) == "Update recommended.");
	xrt_status_platform_remedy both = remedy(true, "", "", "", "");
	both.update_recommended = true;
	CHECK(line(both) == "Update required."); // REQUIRED wins
	xrt_status_platform_remedy none = {};
	char buf[8] = "x";
	CHECK_FALSE(u_status_remedy_format(&none, buf, sizeof(buf)));
	CHECK(buf[0] == '\0');
}

TEST_CASE("remedy: JSON keys are always present", "[remedy]")
{
	cJSON *o = cJSON_CreateObject();
	u_status_remedy_add_cjson(o, nullptr);
	CHECK(cJSON_IsFalse(cJSON_GetObjectItem(o, "update_recommended")));
	CHECK(cJSON_IsFalse(cJSON_GetObjectItem(o, "update_required")));
	CHECK(std::string(cJSON_GetObjectItem(o, "action_url")->valuestring).empty());
	cJSON_Delete(o);

	o = cJSON_CreateObject();
	const xrt_status_platform_remedy r = remedy(true, "1.2.0", "1.4.0", "Download", "https://e.com/u");
	u_status_remedy_add_cjson(o, &r);
	CHECK(cJSON_IsTrue(cJSON_GetObjectItem(o, "update_required")));
	CHECK(std::string(cJSON_GetObjectItem(o, "installed_version")->valuestring) == "1.2.0");
	CHECK(std::string(cJSON_GetObjectItem(o, "required_version")->valuestring) == "1.4.0");
	CHECK(std::string(cJSON_GetObjectItem(o, "action_label")->valuestring) == "Download");
	CHECK(std::string(cJSON_GetObjectItem(o, "action_url")->valuestring) == "https://e.com/u");
	cJSON_Delete(o);
}
