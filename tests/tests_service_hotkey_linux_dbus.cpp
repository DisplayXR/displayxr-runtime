// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The Linux launch-hotkey worker's GNOME-extension backend against a
 *         fake org.displayxr.WorkspaceHotkey1 (tests/fake_workspace_hotkey_ext.py)
 *         on a PRIVATE session bus (dbus-run-session) — never the desktop's.
 *
 * Checks: arm → Configure(accelerator in GTK syntax); Activated → the activate
 * callback; suspend → Suspend(true, 60000); disarm → Configure(""); a press the
 * extension carried across a service start (pending) → the callback; an
 * extension restart (bus name re-owned) → the accelerator is pushed again.
 */

#include "catch_amalgamated.hpp"

#include "service_hotkey_linux.h"

#include <cjson/cJSON.h>
#include <dbus/dbus.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>

namespace {

std::atomic<int> g_presses{0};

void
on_press(void)
{
	g_presses++;
}

struct bus
{
	DBusConnection *c = nullptr;

	bus()
	{
		DBusError err;
		dbus_error_init(&err);
		c = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
		dbus_error_free(&err);
		REQUIRE(c != nullptr);
		dbus_connection_set_exit_on_disconnect(c, FALSE);
	}

	~bus()
	{
		dbus_connection_close(c);
		dbus_connection_unref(c);
	}

	//! Call org.displayxr.Test1.<method> with no args or one bool; returns the
	//! string reply of State(), "" otherwise.
	std::string
	call(const char *method, int bool_arg = -1)
	{
		DBusMessage *m = dbus_message_new_method_call(
		    "org.displayxr.WindowGeometry", "/org/displayxr/WorkspaceHotkey", "org.displayxr.Test1", method);
		if (bool_arg >= 0) {
			dbus_bool_t b = bool_arg ? TRUE : FALSE;
			dbus_message_append_args(m, DBUS_TYPE_BOOLEAN, &b, DBUS_TYPE_INVALID);
		}
		DBusError err;
		dbus_error_init(&err);
		DBusMessage *r = dbus_connection_send_with_reply_and_block(c, m, 5000, &err);
		dbus_message_unref(m);
		std::string out;
		if (r != nullptr) {
			const char *s = nullptr;
			if (dbus_message_get_args(r, &err, DBUS_TYPE_STRING, &s, DBUS_TYPE_INVALID) && s != nullptr) {
				out = s;
			}
			dbus_message_unref(r);
		}
		dbus_error_free(&err);
		return out;
	}
};

//! Number of Configure calls recorded, and the last accelerator.
void
configures(bus &b, int *count, std::string *last, std::string *unit = nullptr)
{
	*count = 0;
	last->clear();
	cJSON *root = cJSON_Parse(b.call("State").c_str());
	if (root == nullptr) {
		return;
	}
	const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "configures");
	*count = cJSON_GetArraySize(arr);
	if (*count > 0) {
		const cJSON *e = cJSON_GetArrayItem(arr, *count - 1);
		const cJSON *a = cJSON_GetObjectItemCaseSensitive(e, "accelerator");
		*last = cJSON_IsString(a) ? a->valuestring : "";
		if (unit != nullptr) {
			const cJSON *u = cJSON_GetObjectItemCaseSensitive(e, "unit");
			*unit = cJSON_IsString(u) ? u->valuestring : "";
		}
	}
	cJSON_Delete(root);
}

bool
eventually(const std::function<bool()> &pred, int timeout_ms = 5000)
{
	for (int t = 0; t < timeout_ms; t += 20) {
		if (pred()) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	return pred();
}

} // namespace

TEST_CASE("GNOME extension backend over a private session bus", "[service][hotkey][linux][dbus]")
{
	REQUIRE(std::getenv("DBUS_SESSION_BUS_ADDRESS") != nullptr);
	unsetenv("DISPLAY");
	bus b;

	service_hotkey_linux_start(on_press);

	// Arm: the combo reaches the extension in GTK syntax.
	service_hotkey_linux_arm("Ctrl+Space");
	int n = 0;
	std::string last;
	std::string unit;
	REQUIRE(eventually([&] {
		configures(b, &n, &last, &unit);
		return n >= 1;
	}));
	CHECK(last == "<Control>space");
	INFO("unit pushed: '" << unit << "'");

	// A press: Activated -> the callback.
	b.call("Press");
	REQUIRE(eventually([] { return g_presses.load() == 1; }));

	// A new combo is pushed.
	service_hotkey_linux_arm("Ctrl+Alt+W");
	REQUIRE(eventually([&] {
		configures(b, &n, &last);
		return last == "<Control><Alt>w";
	}));

	// Suspend / resume reach the extension.
	service_hotkey_linux_suspend(true);
	REQUIRE(eventually([&] {
		cJSON *root = cJSON_Parse(b.call("State").c_str());
		bool ok = false;
		if (root != nullptr) {
			const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "suspends");
			for (int i = 0; i < cJSON_GetArraySize(arr); i++) {
				const cJSON *e = cJSON_GetArrayItem(arr, i);
				const cJSON *on = cJSON_GetObjectItemCaseSensitive(e, "suspend");
				const cJSON *ms = cJSON_GetObjectItemCaseSensitive(e, "timeoutMs");
				ok = ok || (cJSON_IsTrue(on) && cJSON_IsNumber(ms) && ms->valuedouble == 60000);
			}
			cJSON_Delete(root);
		}
		return ok;
	}));
	service_hotkey_linux_suspend(false);

	// A press the extension carried across a service start: Configure says
	// pending -> the callback runs once.
	b.call("SetPending", 1);
	service_hotkey_linux_arm("Ctrl+Space");
	REQUIRE(eventually([] { return g_presses.load() == 2; }));

	// The extension restarts (lock screen / shell restart): pushed again.
	configures(b, &n, &last);
	const int before = n;
	b.call("Reown");
	REQUIRE(eventually([&] {
		configures(b, &n, &last);
		return n > before;
	}));
	CHECK(last == "<Control>space");

	// Disarm: Configure("").
	service_hotkey_linux_arm("");
	REQUIRE(eventually([&] {
		configures(b, &n, &last);
		return last.empty() && n > before + 1;
	}));

	service_hotkey_linux_stop();
	std::this_thread::sleep_for(std::chrono::milliseconds(300));
	CHECK(g_presses.load() == 2);
}
