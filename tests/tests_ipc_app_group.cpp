// Copyright 2026, DisplayXR contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Input focus follows the application: the pure sibling rule
 *         (ipc/shared/ipc_app_group.h).
 *
 * The case in hand is the browser: its GPU process presents (the focused
 * connection) and its browser process owns the hand trackers; the second must
 * share the first's INPUT focus. The rule must also refuse an unrelated
 * instance of the same application when the process tree is visible.
 */

#include "catch_amalgamated.hpp"

#include "shared/ipc_app_group.h"

TEST_CASE("ipc_app_group_same: browser GPU process + browser process share focus", "[ipc][focus]")
{
	// browser.exe pid 100 (parent explorer 7) presents from gpu pid 120 (parent 100).
	CHECK(ipc_app_group_same("ChromiumInline3D", "C:/b/chrome.exe", 120, 100, //
	                         "ChromiumInline3D", "C:/b/chrome.exe", 100, 7));
	CHECK(ipc_app_group_same("ChromiumInline3D", "C:/b/chrome.exe", 100, 7, //
	                         "ChromiumInline3D", "C:/b/chrome.exe", 120, 100));
	// Two children of the one browser process (renderer + gpu) are siblings.
	CHECK(ipc_app_group_same("ChromiumInline3D", "C:/b/chrome.exe", 120, 100, //
	                         "ChromiumInline3D", "C:/b/chrome.exe", 130, 100));
}

TEST_CASE("ipc_app_group_same: the declared name decides first", "[ipc][focus]")
{
	// Different applications in one process tree (a launcher and its child app).
	CHECK_FALSE(ipc_app_group_same("Launcher", "C:/l/launcher.exe", 100, 7, //
	                               "Cube", "C:/l/cube.exe", 120, 100));
	// Unknown names never group.
	CHECK_FALSE(ipc_app_group_same("", "C:/b/chrome.exe", 120, 100, //
	                               "", "C:/b/chrome.exe", 100, 7));
	CHECK_FALSE(ipc_app_group_same("Cube", "C:/l/cube.exe", 120, 100, //
	                               "", "C:/l/cube.exe", 100, 7));
}

TEST_CASE("ipc_app_group_same: a visible tree refuses an unrelated instance", "[ipc][focus]")
{
	// Two separate browser instances (two trees under explorer 7): same name,
	// same exe, but neither parent/child nor siblings — each keeps its own focus.
	CHECK_FALSE(ipc_app_group_same("ChromiumInline3D", "C:/b/chrome.exe", 120, 100, //
	                               "ChromiumInline3D", "C:/b/chrome.exe", 220, 200));
	// ...unless they are siblings under one parent (both spawned by one shell).
	CHECK(ipc_app_group_same("Cube", "C:/l/cube.exe", 100, 7, //
	                         "Cube", "C:/l/cube.exe", 200, 7));
	// Two connections of ONE process always group.
	CHECK(ipc_app_group_same("Engine", "C:/e/engine.exe", 300, 7, //
	                         "Engine", "C:/e/engine.exe", 300, 7));
}

TEST_CASE("ipc_app_group_same: unknown tree falls back to the executable", "[ipc][focus]")
{
	// ppid unknown on one side (platform stub, or a peer we could not read).
	CHECK(ipc_app_group_same("ChromiumInline3D", "C:/b/chrome.exe", 120, 0, //
	                         "ChromiumInline3D", "C:/b/chrome.exe", 100, 7));
	CHECK_FALSE(ipc_app_group_same("Cube", "C:/one/cube.exe", 120, 0, //
	                               "Cube", "C:/two/cube.exe", 100, 7));
	// Neither tree nor executable: the declared name is all there is.
	CHECK(ipc_app_group_same("Cube", "", 0, 0, "Cube", "", 0, 0));
}
