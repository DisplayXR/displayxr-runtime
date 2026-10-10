// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Display dashboard phase 8: workspace-controller launch settings —
 *         the hotkey combo grammar, service.json round trip with the
 *         `controllers` map, the active-controller selection rule and the
 *         `workspace list --json` document.
 */

#include "catch_amalgamated.hpp"

#include "service_config.h"
#include "service_hotkey.h"
#include "service_workspace_registry.h"

#include <cjson/cJSON.h>

#include <cstdlib>
#include <cstring>
#include <string>

namespace {

std::string
canon(const char *text)
{
	char buf[SERVICE_HOTKEY_MAX];
	if (!service_hotkey_canonicalize(text, buf, sizeof(buf))) {
		return "<invalid>";
	}
	return buf;
}

workspace_controller_entry
entry(const char *id, const char *binary)
{
	workspace_controller_entry e;
	std::memset(&e, 0, sizeof(e));
	std::snprintf(e.id, sizeof(e.id), "%s", id);
	std::snprintf(e.binary, sizeof(e.binary), "%s", binary);
	std::snprintf(e.display_name, sizeof(e.display_name), "Controller %s", id);
	std::snprintf(e.vendor, sizeof(e.vendor), "Vendor");
	std::snprintf(e.version, sizeof(e.version), "1.2.3");
	return e;
}

service_config
defaults()
{
	service_config cfg;
	service_config_set_defaults(&cfg);
	return cfg;
}

service_config
round_trip(const service_config &in)
{
	char *json = service_config_to_json(&in);
	REQUIRE(json != nullptr);
	service_config out = defaults();
	service_config_parse_json(&out, json);
	std::free(json);
	return out;
}

} // namespace

TEST_CASE("hotkey combos parse and format canonically", "[service][hotkey]")
{
	CHECK(canon("Ctrl+Space") == "Ctrl+Space");
	CHECK(canon(SERVICE_HOTKEY_DEFAULT) == "Ctrl+Space");
	CHECK(canon("ctrl+space") == "Ctrl+Space");
	CHECK(canon("Shift+Ctrl+F9") == "Ctrl+Shift+F9");
	CHECK(canon("Win+Alt+Shift+Ctrl+BracketRight") == "Ctrl+Shift+Alt+Win+BracketRight");
	CHECK(canon("Alt+a") == "Alt+A");
	CHECK(canon("Ctrl+0") == "Ctrl+0");
	CHECK(canon("Ctrl+F1") == "Ctrl+F1");
	CHECK(canon("Ctrl+F24") == "Ctrl+F24");
	CHECK(canon("Ctrl+ArrowLeft") == "Ctrl+Left");
	CHECK(canon("Ctrl+Alt+PageDown") == "Ctrl+Alt+PageDown");

	const char *keys[] = {"Space",     "Tab",   "Enter", "Backquote", "Minus",     "Equals", "BracketLeft",
	                      "BracketRight", "Semicolon", "Quote", "Comma", "Period", "Slash",   "Backslash",
	                      "Insert",    "Delete", "Home",  "End",       "PageUp",    "PageDown", "Left",
	                      "Up",        "Right", "Down",  "Z",         "9",         "F12"};
	for (const char *k : keys) {
		std::string combo = std::string("Ctrl+Shift+") + k;
		INFO(combo);
		CHECK(canon(combo.c_str()) == combo);
	}

	service_hotkey hk;
	REQUIRE(service_hotkey_parse("Ctrl+Space", &hk));
	CHECK(hk.mods == SERVICE_HOTKEY_MOD_CTRL);
	CHECK(hk.vk == 0x20u);
	REQUIRE(service_hotkey_parse("Ctrl+Shift+Alt+Win+F9", &hk));
	CHECK(hk.mods == (SERVICE_HOTKEY_MOD_CTRL | SERVICE_HOTKEY_MOD_SHIFT | SERVICE_HOTKEY_MOD_ALT |
	                  SERVICE_HOTKEY_MOD_WIN));
	CHECK(hk.vk == 0x78u); // VK_F9
}

TEST_CASE("hotkey parser rejects everything outside the grammar", "[service][hotkey]")
{
	const char *bad[] = {
	    "",           "Space",         "A",          "Ctrl",          "Ctrl+",        "+Space",
	    "Ctrl++Space", "Ctrl+Ctrl+A",  "Ctrl+Shift", "Ctrl+Space+A",  "Ctrl + Space", " Ctrl+Space",
	    "Ctrl+F0",    "Ctrl+F25",      "Ctrl+F01",   "Ctrl+Esc",      "Ctrl+Escape",  "Meta+A",
	    "Cmd+Space",  "Ctrl+Plus",     "Ctrl+-",     "Ctrl+Spacebar", "Ctrl+AB",      "Ctrl+Space ",
	};
	for (const char *b : bad) {
		INFO("'" << b << "'");
		service_hotkey hk = {7, 7};
		CHECK_FALSE(service_hotkey_parse(b, &hk));
		CHECK(hk.mods == 0);
		CHECK(hk.vk == 0);
	}
	service_hotkey hk;
	CHECK_FALSE(service_hotkey_parse(nullptr, &hk));

	char buf[SERVICE_HOTKEY_MAX];
	service_hotkey none = {0, 0x20};
	CHECK_FALSE(service_hotkey_format(&none, buf, sizeof(buf))); // no modifier
	service_hotkey unknown = {SERVICE_HOTKEY_MOD_CTRL, 0x1B};      // VK_ESCAPE, not in the grammar
	CHECK_FALSE(service_hotkey_format(&unknown, buf, sizeof(buf)));
	service_hotkey ok = {SERVICE_HOTKEY_MOD_CTRL, 0x20};
	char tiny[4];
	CHECK_FALSE(service_hotkey_format(&ok, tiny, sizeof(tiny)));
}

namespace {

struct x11_key
{
	uint32_t keysym;
	uint32_t mods;
};

x11_key
x11(const char *text)
{
	service_hotkey hk;
	REQUIRE(service_hotkey_parse(text, &hk));
	x11_key k{0, 0};
	REQUIRE(service_hotkey_to_x11(&hk, &k.keysym, &k.mods));
	return k;
}

std::string
accel(const char *text)
{
	service_hotkey hk;
	REQUIRE(service_hotkey_parse(text, &hk));
	char buf[64];
	REQUIRE(service_hotkey_to_accelerator(&hk, buf, sizeof(buf)));
	return buf;
}

} // namespace

TEST_CASE("hotkey grammar maps on to X11 keysyms and modifier masks", "[service][hotkey][linux]")
{
	// keysymdef.h: XK_space 0x20, XK_a 0x61, XK_F1 0xffbe, XK_Return 0xff0d ...
	CHECK(x11("Ctrl+Space").keysym == 0x0020u);
	CHECK(x11("Ctrl+Space").mods == SERVICE_HOTKEY_X11_CONTROL);
	CHECK(x11("Alt+A").keysym == 0x0061u); // the level-0 (lower-case) keysym
	CHECK(x11("Alt+Z").keysym == 0x007au);
	CHECK(x11("Alt+A").mods == SERVICE_HOTKEY_X11_MOD1);
	CHECK(x11("Win+0").keysym == 0x0030u);
	CHECK(x11("Win+9").keysym == 0x0039u);
	CHECK(x11("Win+9").mods == SERVICE_HOTKEY_X11_MOD4);
	CHECK(x11("Shift+F1").keysym == 0xffbeu);
	CHECK(x11("Shift+F12").keysym == 0xffc9u);
	CHECK(x11("Shift+F24").keysym == 0xffd5u);
	CHECK(x11("Ctrl+Enter").keysym == 0xff0du);
	CHECK(x11("Ctrl+Tab").keysym == 0xff09u);
	CHECK(x11("Ctrl+Backquote").keysym == 0x0060u);
	CHECK(x11("Ctrl+Quote").keysym == 0x0027u);
	CHECK(x11("Ctrl+Delete").keysym == 0xffffu);
	CHECK(x11("Ctrl+PageUp").keysym == 0xff55u);
	CHECK(x11("Ctrl+PageDown").keysym == 0xff56u);
	CHECK(x11("Ctrl+ArrowLeft").keysym == 0xff51u);
	CHECK(x11("Ctrl+Down").keysym == 0xff54u);
	CHECK(x11("Ctrl+Shift+Alt+Win+BracketRight").keysym == 0x005du);
	CHECK(x11("Ctrl+Shift+Alt+Win+BracketRight").mods == (SERVICE_HOTKEY_X11_CONTROL | SERVICE_HOTKEY_X11_SHIFT |
	                                                      SERVICE_HOTKEY_X11_MOD1 | SERVICE_HOTKEY_X11_MOD4));

	// Every key the grammar accepts maps (no hole in the table).
	const char *keys[] = {"Space",       "Tab",          "Enter",     "Backquote", "Minus", "Equals",
	                      "BracketLeft", "BracketRight", "Semicolon", "Quote",     "Comma", "Period",
	                      "Slash",       "Backslash",    "Insert",    "Delete",    "Home",  "End",
	                      "PageUp",      "PageDown",     "Left",      "Up",        "Right", "Down"};
	for (const char *k : keys) {
		std::string combo = std::string("Ctrl+") + k;
		INFO(combo);
		CHECK(x11(combo.c_str()).keysym != 0u);
		CHECK(!accel(combo.c_str()).empty());
	}

	// Invalid input is refused with zeroed outputs.
	service_hotkey none{0, 0};
	uint32_t ks = 1, m = 1;
	CHECK_FALSE(service_hotkey_to_x11(&none, &ks, &m));
	CHECK(ks == 0u);
	CHECK(m == 0u);
	service_hotkey bogus{SERVICE_HOTKEY_MOD_CTRL, 0x07};
	CHECK_FALSE(service_hotkey_to_x11(&bogus, &ks, &m));
}

TEST_CASE("hotkey grammar maps on to GTK accelerators", "[service][hotkey][linux]")
{
	CHECK(accel("Ctrl+Space") == "<Control>space");
	CHECK(accel("ctrl+shift+f5") == "<Control><Shift>F5");
	CHECK(accel("Alt+Win+A") == "<Alt><Super>a");
	CHECK(accel("Win+PageDown") == "<Super>Page_Down");
	CHECK(accel("Ctrl+Equals") == "<Control>equal");
	CHECK(accel("Ctrl+Enter") == "<Control>Return");
	CHECK(accel("Ctrl+Quote") == "<Control>apostrophe");
	CHECK(accel("Ctrl+Backquote") == "<Control>grave");
	CHECK(accel("Shift+Alt+7") == "<Shift><Alt>7");

	service_hotkey hk;
	REQUIRE(service_hotkey_parse("Ctrl+Shift+Alt+Win+BracketRight", &hk));
	char small[8];
	CHECK_FALSE(service_hotkey_to_accelerator(&hk, small, sizeof(small)));
	CHECK(small[0] == '\0');
	service_hotkey none{0, 0};
	char buf[64];
	CHECK_FALSE(service_hotkey_to_accelerator(&none, buf, sizeof(buf)));
}

TEST_CASE("defaults: Ctrl+Space, auto, source default", "[service][config]")
{
	service_config cfg = defaults();
	service_launch_resolved r;
	service_config_resolve_launch(&cfg, "shell", true, &r);
	CHECK(r.mode == SERVICE_CHILD_AUTO);
	CHECK(r.has_hotkey);
	CHECK(std::string(r.hotkey_text) == "Ctrl+Space");
	CHECK_FALSE(r.user);

	// A missing / empty / malformed file is the defaults.
	service_config_parse_json(&cfg, "");
	service_config_parse_json(&cfg, "not json");
	service_config_parse_json(&cfg, "{\"controllers\": 3}");
	CHECK(cfg.controller_count == 0);
	CHECK(cfg.workspace == SERVICE_CHILD_AUTO);
}

TEST_CASE("service.json round-trips the controllers map", "[service][config]")
{
	service_config cfg = defaults();
	cfg.start_on_login = false;
	std::snprintf(cfg.workspace_binary, sizeof(cfg.workspace_binary), "%s", "shell");
	REQUIRE(service_config_set_controller_hotkey(&cfg, "shell", "shift+ctrl+f9"));
	REQUIRE(service_config_set_controller_hotkey(&cfg, "cockpit", nullptr)); // --no-hotkey
	REQUIRE(service_config_set_controller_mode(&cfg, "kiosk", SERVICE_CHILD_DISABLE, false));
	CHECK(cfg.controller_count == 3);

	char *json = service_config_to_json(&cfg);
	REQUIRE(json != nullptr);
	cJSON *root = cJSON_Parse(json);
	REQUIRE(root != nullptr);
	cJSON *controllers = cJSON_GetObjectItemCaseSensitive(root, "controllers");
	REQUIRE(cJSON_IsObject(controllers));
	cJSON *shell = cJSON_GetObjectItemCaseSensitive(controllers, "shell");
	CHECK(std::string(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(shell, "hotkey"))) == "Ctrl+Shift+F9");
	CHECK(cJSON_GetObjectItemCaseSensitive(shell, "mode") == nullptr);
	cJSON *cockpit = cJSON_GetObjectItemCaseSensitive(controllers, "cockpit");
	CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(cockpit, "hotkey")));
	cJSON *kiosk = cJSON_GetObjectItemCaseSensitive(controllers, "kiosk");
	CHECK(std::string(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(kiosk, "mode"))) == "disabled");
	CHECK(cJSON_GetObjectItemCaseSensitive(kiosk, "hotkey") == nullptr);
	// Back-compat keys stay.
	CHECK(std::string(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "workspace"))) == "auto");
	CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(root, "start_on_login")));
	cJSON_Delete(root);
	std::free(json);

	service_config back = round_trip(cfg);
	CHECK(back.start_on_login == false);
	CHECK(std::string(back.workspace_binary) == "shell");
	CHECK(back.controller_count == 3);

	service_launch_resolved r;
	service_config_resolve_launch(&back, "shell", true, &r);
	CHECK(std::string(r.hotkey_text) == "Ctrl+Shift+F9");
	CHECK(r.mode == SERVICE_CHILD_AUTO);
	CHECK(r.user);
	service_config_resolve_launch(&back, "cockpit", false, &r);
	CHECK_FALSE(r.has_hotkey);
	CHECK(std::string(r.hotkey_text).empty());
	CHECK(r.user);
	service_config_resolve_launch(&back, "kiosk", false, &r);
	CHECK(r.mode == SERVICE_CHILD_DISABLE);
	CHECK(std::string(r.hotkey_text) == "Ctrl+Space");
	service_config_resolve_launch(&back, "unknown", false, &r);
	CHECK_FALSE(r.user);
	CHECK(r.mode == SERVICE_CHILD_AUTO);
}

TEST_CASE("a malformed stored hotkey falls back to the default", "[service][config]")
{
	service_config cfg = defaults();
	service_config_parse_json(&cfg, "{\"controllers\": {\"shell\": {\"hotkey\": \"Hyper+Q\"}, "
	                                "\"other\": {\"hotkey\": \"ctrl+q\", \"mode\": \"bogus\"}}}");
	service_launch_resolved r;
	service_config_resolve_launch(&cfg, "shell", true, &r);
	CHECK(std::string(r.hotkey_text) == "Ctrl+Space");
	CHECK_FALSE(r.user); // pruned: nothing customised survived
	service_config_resolve_launch(&cfg, "other", false, &r);
	CHECK(std::string(r.hotkey_text) == "Ctrl+Q"); // stored canonicalised
	CHECK(r.mode == SERVICE_CHILD_AUTO);
}

TEST_CASE("setters prune back to defaults and keep the top-level mode in step", "[service][config]")
{
	service_config cfg = defaults();

	// Active controller disabled → top-level `workspace` follows (back-compat).
	REQUIRE(service_config_set_controller_mode(&cfg, "shell", SERVICE_CHILD_DISABLE, true));
	CHECK(cfg.workspace == SERVICE_CHILD_DISABLE);
	service_launch_resolved r;
	service_config_resolve_launch(&cfg, "shell", true, &r);
	CHECK(r.mode == SERVICE_CHILD_DISABLE);
	CHECK(r.user);

	// Back to auto + the default combo → the entry disappears → "default".
	REQUIRE(service_config_set_controller_mode(&cfg, "shell", SERVICE_CHILD_AUTO, true));
	REQUIRE(service_config_set_controller_hotkey(&cfg, "shell", "Ctrl+Space"));
	CHECK(cfg.controller_count == 0);
	CHECK(cfg.workspace == SERVICE_CHILD_AUTO);
	service_config_resolve_launch(&cfg, "shell", true, &r);
	CHECK_FALSE(r.user);

	// ENABLE is not a per-controller mode.
	CHECK_FALSE(service_config_set_controller_mode(&cfg, "shell", SERVICE_CHILD_ENABLE, true));
	// An invalid combo leaves cfg untouched.
	CHECK_FALSE(service_config_set_controller_hotkey(&cfg, "shell", "Space"));
	CHECK(cfg.controller_count == 0);
	// An unusable id.
	CHECK_FALSE(service_config_set_controller_hotkey(&cfg, "", "Ctrl+A"));

	// reset clears every customisation and the top-level mode of the active one.
	REQUIRE(service_config_set_controller_hotkey(&cfg, "shell", nullptr));
	REQUIRE(service_config_set_controller_mode(&cfg, "shell", SERVICE_CHILD_DISABLE, true));
	service_config_reset_controller(&cfg, "shell", true);
	CHECK(cfg.controller_count == 0);
	CHECK(cfg.workspace == SERVICE_CHILD_AUTO);
}

TEST_CASE("the tray's top-level mode writes mirror into the active entry", "[service][config]")
{
	service_config cfg = defaults();
	cfg.workspace = SERVICE_CHILD_DISABLE;
	service_config_sync_active_mode(&cfg, "shell");
	REQUIRE(cfg.controller_count == 1);
	service_launch_resolved r;
	service_config_resolve_launch(&cfg, "shell", true, &r);
	CHECK(r.mode == SERVICE_CHILD_DISABLE);

	// Tray → Enable (legacy always-on): the entry's mode goes, ENABLE resolves.
	cfg.workspace = SERVICE_CHILD_ENABLE;
	service_config_sync_active_mode(&cfg, "shell");
	CHECK(cfg.controller_count == 0);
	service_config_resolve_launch(&cfg, "shell", true, &r);
	CHECK(r.mode == SERVICE_CHILD_ENABLE);
	CHECK(r.user); // differs from the default (auto)
	CHECK(std::string(service_config_launch_mode_str(r.mode)) == "auto");
	// The top-level mode never speaks for a non-active controller.
	service_config_resolve_launch(&cfg, "other", false, &r);
	CHECK(r.mode == SERVICE_CHILD_AUTO);
	CHECK_FALSE(r.user);

	// A hand-edited disagreement: the per-controller entry wins.
	service_config_parse_json(&cfg, "{\"workspace\": \"auto\", \"controllers\": {\"shell\": {\"mode\": \"disabled\"}}}");
	service_config_resolve_launch(&cfg, "shell", true, &r);
	CHECK(r.mode == SERVICE_CHILD_DISABLE);
}

TEST_CASE("active-controller selection rule", "[service][config]")
{
	workspace_controller_entry e[3] = {entry("alpha", "C:\\a\\alpha.exe"), entry("beta", "C:\\b\\beta.exe"),
	                                   entry("gamma", "C:\\g\\gamma.exe")};
	service_config cfg = defaults();

	CHECK(service_config_pick_controller(&cfg, e, 0) == -1);  // none registered
	CHECK(service_config_pick_controller(&cfg, e, 3) == 0);   // default: the first
	std::snprintf(cfg.workspace_binary, sizeof(cfg.workspace_binary), "%s", "gamma");
	CHECK(service_config_pick_controller(&cfg, e, 3) == 2);   // preferred id
	std::snprintf(cfg.workspace_binary, sizeof(cfg.workspace_binary), "%s", "missing");
	CHECK(service_config_pick_controller(&cfg, e, 3) == 0);   // unknown id → first
	std::snprintf(cfg.workspace_binary, sizeof(cfg.workspace_binary), "%s", "C:\\dev\\controller.exe");
	CHECK(service_config_pick_controller(&cfg, e, 3) == SERVICE_PICK_DEV_OVERRIDE);
	std::snprintf(cfg.workspace_binary, sizeof(cfg.workspace_binary), "%s", "/opt/dev/controller");
	CHECK(service_config_pick_controller(&cfg, e, 0) == SERVICE_PICK_DEV_OVERRIDE);
#ifdef _WIN32
	std::snprintf(cfg.workspace_binary, sizeof(cfg.workspace_binary), "%s", "BETA");
	CHECK(service_config_pick_controller(&cfg, e, 3) == 1); // registry ids are case-insensitive
#endif
}

TEST_CASE("workspace list --json document", "[service][config]")
{
	workspace_controller_entry e[2] = {entry("alpha", "C:\\a\\alpha.exe"), entry("beta", "C:\\b\\beta.exe")};
	service_config cfg = defaults();
	std::snprintf(cfg.workspace_binary, sizeof(cfg.workspace_binary), "%s", "beta");
	REQUIRE(service_config_set_controller_hotkey(&cfg, "beta", "Ctrl+Shift+F9"));

	// Headless: connected / pid null.
	cJSON *root = (cJSON *)service_workspace_controllers_to_cjson(&cfg, e, 2, nullptr);
	REQUIRE(root != nullptr);
	CHECK(std::string(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "active_id"))) == "beta");
	CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(root, "dev_override")));
	cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "controllers");
	REQUIRE(cJSON_GetArraySize(arr) == 2);
	cJSON *a = cJSON_GetArrayItem(arr, 0);
	CHECK(std::string(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "id"))) == "alpha");
	CHECK(std::string(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "display_name"))) == "Controller alpha");
	CHECK(std::string(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "vendor"))) == "Vendor");
	CHECK(std::string(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "version"))) == "1.2.3");
	CHECK(std::string(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "binary"))) == "C:\\a\\alpha.exe");
	CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(a, "registered")));
	CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(a, "connected")));
	CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(a, "pid")));
	cJSON *la = cJSON_GetObjectItemCaseSensitive(a, "launch");
	CHECK(std::string(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(la, "mode"))) == "auto");
	CHECK(std::string(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(la, "hotkey"))) == "Ctrl+Space");
	CHECK(std::string(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(la, "source"))) == "default");
	cJSON *lb = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(arr, 1), "launch");
	CHECK(std::string(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(lb, "hotkey"))) == "Ctrl+Shift+F9");
	CHECK(std::string(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(lb, "source"))) == "user");
	cJSON_Delete(root);

	// Live: connected + pid from the service.
	service_controller_live live[2] = {{true, false, 0}, {true, true, 4242}};
	root = (cJSON *)service_workspace_controllers_to_cjson(&cfg, e, 2, live);
	arr = cJSON_GetObjectItemCaseSensitive(root, "controllers");
	CHECK(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(arr, 0), "connected")));
	CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(arr, 0), "pid")));
	CHECK(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(arr, 1), "connected")));
	CHECK(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(arr, 1), "pid")->valuedouble == 4242.0);
	cJSON_Delete(root);

	// --no-hotkey → "hotkey": null; nothing registered → active_id null.
	REQUIRE(service_config_set_controller_hotkey(&cfg, "beta", nullptr));
	root = (cJSON *)service_workspace_controllers_to_cjson(&cfg, e, 2, nullptr);
	lb = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "controllers"), 1),
	                                      "launch");
	CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(lb, "hotkey")));
	cJSON_Delete(root);
	root = (cJSON *)service_workspace_controllers_to_cjson(&cfg, e, 0, nullptr);
	CHECK(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(root, "active_id")));
	CHECK(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(root, "controllers")) == 0);
	cJSON_Delete(root);
}
