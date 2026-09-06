// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #1277 P2 — the in-process/IPC routing-property grammar.
 * @author David Fattal
 *
 * `debug.dxr.force_ipc` / `ro.dxr.force_ipc` decide whether an Android app's
 * session is created in-process (Architecture A) or as a client of the service
 * (Architecture C, the deployment that can reach the weave satellite). The
 * property read is Android-only and needs a device; the DECISION is a pure
 * string comparison, and that is what actually has to be right — a targeted
 * allow-list quietly widening to the whole device is the pre-ADR-036 failure.
 */

#include "catch_amalgamated.hpp"

#include "util/u_sandbox.h"

static bool
dev(const char *value, const char *proc)
{
	return u_sandbox_route_prop_selects(value, proc, true);
}

static bool
ship(const char *value, const char *proc)
{
	return u_sandbox_route_prop_selects(value, proc, false);
}

TEST_CASE("route policy: back-compatible device-wide booleans")
{
	// What `setprop debug.dxr.force_ipc 1` has always meant, and what every
	// existing script and doc sets.
	CHECK(dev("1", "com.displayxr.modelviewer"));
	CHECK(dev("true", "com.displayxr.modelviewer"));
	CHECK(dev("t", "com.displayxr.modelviewer"));
	CHECK(dev("yes", "com.displayxr.modelviewer"));
	CHECK(dev("on", "com.displayxr.modelviewer"));
	CHECK(dev("all", "com.displayxr.modelviewer"));
	CHECK(dev("*", "com.displayxr.modelviewer"));

	// A device-wide force works even when the process name is unknown --
	// it names no process, so there is nothing to fail to match.
	CHECK(dev("1", nullptr));

	CHECK_FALSE(dev("0", "com.displayxr.modelviewer"));
	CHECK_FALSE(dev("false", "com.displayxr.modelviewer"));
	CHECK_FALSE(dev("off", "com.displayxr.modelviewer"));
	CHECK_FALSE(dev("", "com.displayxr.modelviewer"));
	CHECK_FALSE(dev(nullptr, "com.displayxr.modelviewer"));
}

TEST_CASE("route policy: the shipping tier gets an allow-list, never a switch")
{
	// ADR-036's flavor merge exists because a device-wide deployment
	// decision pushed apps that wanted in-process out of process. A
	// read-only property is OEM-set and unremovable in the field, so it may
	// not be able to express that.
	CHECK_FALSE(ship("1", "com.displayxr.modelviewer"));
	CHECK_FALSE(ship("true", "com.displayxr.modelviewer"));
	CHECK_FALSE(ship("all", "com.displayxr.modelviewer"));
	CHECK_FALSE(ship("*", "com.displayxr.modelviewer"));
	// ... including a bare star smuggled into a list.
	CHECK_FALSE(ship("com.other,*", "com.displayxr.modelviewer"));

	// It can still name packages, which is the point of the tier.
	CHECK(ship("com.displayxr.modelviewer", "com.displayxr.modelviewer"));
	CHECK(ship("com.displayxr.*", "com.displayxr.modelviewer"));
}

TEST_CASE("route policy: allow-lists")
{
	const char *list = "com.displayxr.modelviewer,com.displayxr.gaussiansplat";

	CHECK(dev(list, "com.displayxr.modelviewer"));
	CHECK(dev(list, "com.displayxr.gaussiansplat"));
	CHECK_FALSE(dev(list, "com.displayxr.mediaplayer"));
	CHECK_FALSE(dev(list, "com.android.chrome"));

	// Separators: comma, space, tab and semicolon all work, so a value
	// pasted from a doc or a shell line behaves the same either way.
	CHECK(dev("com.a com.b", "com.b"));
	CHECK(dev("com.a;com.b", "com.b"));
	CHECK(dev("com.a,\tcom.b", "com.b"));
	CHECK(dev(",,com.b,,", "com.b"));

	// A prefix must not match on its own -- an allow-list of exact names is
	// the safe default, and `*` is how you ask for more.
	CHECK_FALSE(dev("com.displayxr", "com.displayxr.modelviewer"));
	CHECK_FALSE(dev("com.displayxr.modelviewer2", "com.displayxr.modelviewer"));
	CHECK(dev("com.displayxr.*", "com.displayxr.modelviewer"));
	CHECK_FALSE(dev("com.displayxr.*", "com.other.app"));
}

TEST_CASE("route policy: satellite slot processes match their package")
{
	// An Android satellite slot runs as "<pkg>:dxrN". An operator types the
	// package, so the base has to match as well as the full name -- the
	// same colon-stripping the occlusion-file path resolver does.
	CHECK(dev("com.displayxr.modelviewer", "com.displayxr.modelviewer:dxr0"));
	CHECK(dev("com.displayxr.modelviewer:dxr0", "com.displayxr.modelviewer:dxr0"));
	CHECK_FALSE(dev("com.displayxr.other", "com.displayxr.modelviewer:dxr0"));
}

TEST_CASE("route policy: an unreadable process name never widens a list")
{
	// The dangerous direction. If /proc/self/cmdline could not be read we
	// know nothing about who we are, and a list that names somebody else
	// must not be read as "everybody".
	CHECK_FALSE(dev("com.displayxr.modelviewer", nullptr));
	CHECK_FALSE(dev("com.displayxr.modelviewer", ""));
	CHECK_FALSE(ship("com.displayxr.modelviewer", nullptr));
}
