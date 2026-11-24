// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Pins the setting status contract (plugin-discovery.md § Component
 *         settings, "Setting status"): the layout of
 *         `xrt_component_setting_status`, the `get_setting_status` slot
 *         appended to `xrt_plugin_iface` after `activate`, the absent-slot
 *         rule (a plug-in that predates the slot reports `unknown`), and the
 *         runtime's half of the call (zero-fill, struct_size, NUL forcing,
 *         an unknown state or a false return read as `unknown`).
 */

#include "catch_amalgamated.hpp"

#include "xrt/xrt_plugin.h"
#include "xrt/xrt_component_settings.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

#ifndef XRT_PLUGIN_IFACE_HAS_SETTING_STATUS
#error "XRT_PLUGIN_IFACE_HAS_SETTING_STATUS must be defined"
#endif
#ifndef XRT_COMPONENT_SETTINGS_HAS_SETTING_STATUS
#error "XRT_COMPONENT_SETTINGS_HAS_SETTING_STATUS must be defined"
#endif

namespace {

// The status struct's layout: a plug-in built against this header writes at these offsets.
static_assert(offsetof(xrt_component_setting_status, struct_size) == 0, "struct_size moved");
static_assert(offsetof(xrt_component_setting_status, state) == 4, "state moved");
static_assert(offsetof(xrt_component_setting_status, effective_value) == 8, "effective_value moved");
static_assert(offsetof(xrt_component_setting_status, note) == 8 + XRT_COMPONENT_SETTING_STATUS_VALUE_MAX, "note moved");
static_assert(offsetof(xrt_component_setting_status, reserved) ==
                  8 + XRT_COMPONENT_SETTING_STATUS_VALUE_MAX + XRT_COMPONENT_SETTING_STATUS_NOTE_MAX,
              "reserved moved");
static_assert(sizeof(xrt_component_setting_status) == 264,
              "xrt_component_setting_status changed size: append under struct_size and update this test");
static_assert(XRT_COMPONENT_SETTING_STATE_UNKNOWN == 0 && XRT_COMPONENT_SETTING_STATE_APPLIES == 1 &&
                  XRT_COMPONENT_SETTING_STATE_NOT_SUPPORTED == 2 && XRT_COMPONENT_SETTING_STATE_RESTART_REQUIRED == 3,
              "setting states are ABI");

// The slot is appended right after `activate` (ADR-020: append-only, gated by struct_size).
static_assert(offsetof(xrt_plugin_iface, get_setting_status) ==
                  offsetof(xrt_plugin_iface, activate) + sizeof(xrt_component_activate_fn_t),
              "get_setting_status must directly follow activate");
// ADR-047 Amendment 10 appended get_rendering_modes_for_monitor right after it.
static_assert(offsetof(xrt_plugin_iface, get_rendering_modes_for_monitor) ==
                  offsetof(xrt_plugin_iface, get_setting_status) + sizeof(xrt_component_get_setting_status_fn_t),
              "get_rendering_modes_for_monitor must directly follow get_setting_status");
static_assert(sizeof(xrt_plugin_iface) == offsetof(xrt_plugin_iface, get_rendering_modes_for_monitor) +
                                              sizeof(((xrt_plugin_iface *)nullptr)->get_rendering_modes_for_monitor),
              "a slot was appended after get_rendering_modes_for_monitor: pin it here and in xrt_plugin_iface.md");

enum class Mode
{
	NotSupported, // MULTI_VIEWER-like: the stored value cannot take effect here
	Applies,
	Declines,     // returns false
	BadState,     // a state the runtime does not know
	Unterminated, // fills both strings to the brim with no NUL
	Scribbles,    // writes reserved[] and an effective_value with APPLIES
};
Mode g_mode = Mode::NotSupported;
uint32_t g_seen_struct_size = 0;
std::string g_seen_key;

bool
fake_get_setting_status(const char *key, xrt_component_setting_status *out)
{
	g_seen_struct_size = out->struct_size;
	g_seen_key = key;
	switch (g_mode) {
	case Mode::NotSupported:
		out->state = XRT_COMPONENT_SETTING_STATE_NOT_SUPPORTED;
		std::snprintf(out->effective_value, sizeof(out->effective_value), "%s", "system");
		std::snprintf(out->note, sizeof(out->note), "%s",
		              "The installed display software has no multi-viewer mode.");
		return true;
	case Mode::Applies: out->state = XRT_COMPONENT_SETTING_STATE_APPLIES; return true;
	case Mode::Declines:
		out->state = XRT_COMPONENT_SETTING_STATE_NOT_SUPPORTED; // written, then declined: must not leak
		return false;
	case Mode::BadState:
		out->state = 77;
		std::snprintf(out->note, sizeof(out->note), "%s", "junk");
		return true;
	case Mode::Unterminated:
		out->state = XRT_COMPONENT_SETTING_STATE_NOT_SUPPORTED;
		std::memset(out->effective_value, 'v', sizeof(out->effective_value));
		std::memset(out->note, 'n', sizeof(out->note));
		return true;
	case Mode::Scribbles:
		out->state = XRT_COMPONENT_SETTING_STATE_APPLIES;
		std::snprintf(out->effective_value, sizeof(out->effective_value), "%s", "off");
		out->reserved[0] = 42;
		return true;
	}
	return false;
}

xrt_plugin_iface
make_iface(bool with_slot)
{
	xrt_plugin_iface iface;
	std::memset(&iface, 0, sizeof(iface));
	iface.struct_size = sizeof(iface);
	iface.id = "status-test";
	iface.get_setting_status = fake_get_setting_status;
	if (!with_slot) {
		// A plug-in built against the header before the slot: its struct ends at `activate`.
		iface.struct_size = (uint32_t)(offsetof(xrt_plugin_iface, activate) + sizeof(iface.activate));
	}
	return iface;
}

} // namespace

TEST_CASE("setting status: a plug-in that predates the slot reports unknown", "[setting_status]")
{
	auto iface = make_iface(false);
	xrt_component_setting_status st;
	std::memset(&st, 0xAB, sizeof(st));
	g_seen_struct_size = 0;
	CHECK(xrt_plugin_iface_setting_status(&iface, "MULTI_VIEWER", &st) == XRT_COMPONENT_SETTING_STATE_UNKNOWN);
	CHECK(g_seen_struct_size == 0); // never called past its struct_size
	CHECK(st.struct_size == sizeof(st));
	CHECK(st.effective_value[0] == '\0');
	CHECK(st.note[0] == '\0');
	CHECK(std::string(xrt_component_setting_state_str(st.state)) == "unknown");

	// A NULL slot and a NULL iface read the same.
	auto nul = make_iface(true);
	nul.get_setting_status = nullptr;
	CHECK(xrt_plugin_iface_setting_status(&nul, "MULTI_VIEWER", &st) == XRT_COMPONENT_SETTING_STATE_UNKNOWN);
	CHECK(xrt_plugin_iface_setting_status(nullptr, "MULTI_VIEWER", &st) == XRT_COMPONENT_SETTING_STATE_UNKNOWN);
}

TEST_CASE("setting status: not supported carries the value used and the note", "[setting_status]")
{
	auto iface = make_iface(true);
	g_mode = Mode::NotSupported;
	xrt_component_setting_status st;
	CHECK(xrt_plugin_iface_setting_status(&iface, "MULTI_VIEWER", &st) ==
	      XRT_COMPONENT_SETTING_STATE_NOT_SUPPORTED);
	CHECK(g_seen_struct_size == sizeof(st));
	CHECK(g_seen_key == "MULTI_VIEWER");
	CHECK(std::string(st.effective_value) == "system");
	CHECK(std::string(st.note) == "The installed display software has no multi-viewer mode.");
	CHECK(std::string(xrt_component_setting_state_str(st.state)) == "not_supported");
}

TEST_CASE("setting status: a false return or an unknown state reads as unknown", "[setting_status]")
{
	auto iface = make_iface(true);
	xrt_component_setting_status st;
	g_mode = Mode::Declines;
	CHECK(xrt_plugin_iface_setting_status(&iface, "X", &st) == XRT_COMPONENT_SETTING_STATE_UNKNOWN);
	CHECK(st.state == XRT_COMPONENT_SETTING_STATE_UNKNOWN);
	g_mode = Mode::BadState;
	CHECK(xrt_plugin_iface_setting_status(&iface, "X", &st) == XRT_COMPONENT_SETTING_STATE_UNKNOWN);
	CHECK(st.note[0] == '\0');
	// An empty key is never passed on.
	g_mode = Mode::Applies;
	g_seen_key = "untouched";
	CHECK(xrt_plugin_iface_setting_status(&iface, "", &st) == XRT_COMPONENT_SETTING_STATE_UNKNOWN);
	CHECK(g_seen_key == "untouched");
}

TEST_CASE("setting status: strings with no NUL are cut and terminated", "[setting_status]")
{
	auto iface = make_iface(true);
	g_mode = Mode::Unterminated;
	xrt_component_setting_status st;
	CHECK(xrt_plugin_iface_setting_status(&iface, "X", &st) == XRT_COMPONENT_SETTING_STATE_NOT_SUPPORTED);
	CHECK(std::strlen(st.effective_value) == sizeof(st.effective_value) - 1);
	CHECK(std::strlen(st.note) == sizeof(st.note) - 1);
}

TEST_CASE("setting status: effective_value is meaningful only for not_supported; reserved is cleared",
          "[setting_status]")
{
	auto iface = make_iface(true);
	g_mode = Mode::Scribbles;
	xrt_component_setting_status st;
	CHECK(xrt_plugin_iface_setting_status(&iface, "X", &st) == XRT_COMPONENT_SETTING_STATE_APPLIES);
	CHECK(st.effective_value[0] == '\0');
	CHECK(st.reserved[0] == 0);
	CHECK(std::string(xrt_component_setting_state_str(XRT_COMPONENT_SETTING_STATE_RESTART_REQUIRED)) ==
	      "restart_required");
	CHECK(std::string(xrt_component_setting_state_str(XRT_COMPONENT_SETTING_STATE_APPLIES)) == "applies");
}
