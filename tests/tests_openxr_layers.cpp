// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  u_openxr_layers: recognising a named API layer from its manifest,
 *         and the advisory text the CLI and the Ultraleap provider share.
 *
 * The registry walk itself is Windows-only; what is pure — reading a
 * manifest and wording the advice — runs everywhere.
 */

#include "catch_amalgamated.hpp"

#include "util/u_openxr_layers.h"

#include <cstring>
#include <string>

namespace {

// The shape Ultraleap's installer writes (an implicit-layer manifest).
const char *kUltraleapManifest = R"({
  "file_format_version": "1.0.0",
  "api_layer": {
    "name": "XR_APILAYER_ULTRALEAP_hand_tracking",
    "library_path": ".\\UltraleapHandTracking.dll",
    "api_version": "1.0",
    "implementation_version": "1",
    "description": "Ultraleap Hand Tracking",
    "disable_environment": "DISABLE_XR_APILAYER_ULTRALEAP_HAND_TRACKING_1"
  }
})";

} // namespace

TEST_CASE("manifest: the named layer is recognised and its env switches read", "[openxr_layers]")
{
	char dis[128];
	char en[128];
	CHECK(u_openxr_layer_manifest_declares(kUltraleapManifest, U_OPENXR_LAYER_ULTRALEAP_HAND_TRACKING, dis,
	                                       sizeof(dis), en, sizeof(en)));
	CHECK(std::string(dis) == U_OPENXR_LAYER_ULTRALEAP_DISABLE_ENV);
	CHECK(std::string(en).empty());
}

TEST_CASE("manifest: another layer, junk and NULLs are not a match", "[openxr_layers]")
{
	char dis[128] = "stale";
	char en[128] = "stale";
	CHECK_FALSE(u_openxr_layer_manifest_declares(kUltraleapManifest, "XR_APILAYER_LUNARG_core_validation", dis,
	                                             sizeof(dis), en, sizeof(en)));
	CHECK(std::string(dis).empty()); // outputs are cleared on every call
	CHECK(std::string(en).empty());
	CHECK_FALSE(u_openxr_layer_manifest_declares("{not json", U_OPENXR_LAYER_ULTRALEAP_HAND_TRACKING, dis,
	                                             sizeof(dis), en, sizeof(en)));
	CHECK_FALSE(u_openxr_layer_manifest_declares(
	    R"({"api_layer": {"name": 3}})", U_OPENXR_LAYER_ULTRALEAP_HAND_TRACKING, dis, sizeof(dis), en, sizeof(en)));
	CHECK_FALSE(u_openxr_layer_manifest_declares(nullptr, U_OPENXR_LAYER_ULTRALEAP_HAND_TRACKING, dis, sizeof(dis),
	                                             en, sizeof(en)));
	CHECK_FALSE(u_openxr_layer_manifest_declares(kUltraleapManifest, nullptr, nullptr, 0, nullptr, 0));
}

TEST_CASE("manifest: an enable_environment is reported", "[openxr_layers]")
{
	const char *m = R"({"api_layer": {"name": "XR_APILAYER_test", "enable_environment": "ENABLE_TEST_LAYER"}})";
	char dis[128];
	char en[128];
	CHECK(u_openxr_layer_manifest_declares(m, "XR_APILAYER_test", dis, sizeof(dis), en, sizeof(en)));
	CHECK(std::string(dis).empty());
	CHECK(std::string(en) == "ENABLE_TEST_LAYER");
}

TEST_CASE("advice names the layer, where it is registered and the workaround", "[openxr_layers]")
{
	struct u_openxr_implicit_layer layer = {};
	layer.found = layer.enabled_in_registry = layer.active = true;
	std::strcpy(layer.hive, "HKLM");
	std::strcpy(layer.manifest_path, "C:\\Program Files\\Ultraleap\\OpenXR\\UltraleapHandTracking.json");
	std::strcpy(layer.disable_env, U_OPENXR_LAYER_ULTRALEAP_DISABLE_ENV);

	char buf[1024];
	u_openxr_layer_ultraleap_advice(&layer, buf, sizeof(buf));
	const std::string s(buf);
	CHECK(s.find(U_OPENXR_LAYER_ULTRALEAP_HAND_TRACKING) != std::string::npos);
	CHECK(s.find("HKLM") != std::string::npos);
	CHECK(s.find("UltraleapHandTracking.json") != std::string::npos);
	CHECK(s.find(std::string(U_OPENXR_LAYER_ULTRALEAP_DISABLE_ENV) + "=1") != std::string::npos);
	CHECK(s.find("Desktop mode") != std::string::npos);
	CHECK(s.size() < sizeof(buf) - 1); // never truncated at the size the callers use

	// A manifest that did not say: the known variable name is still given.
	layer.disable_env[0] = '\0';
	u_openxr_layer_ultraleap_advice(&layer, buf, sizeof(buf));
	CHECK(std::string(buf).find(U_OPENXR_LAYER_ULTRALEAP_DISABLE_ENV) != std::string::npos);
}

TEST_CASE("off Windows the registry walk finds nothing (and never fails)", "[openxr_layers]")
{
	struct u_openxr_implicit_layer layer;
	const bool found = u_openxr_implicit_layer_find(U_OPENXR_LAYER_ULTRALEAP_HAND_TRACKING, &layer);
#ifndef _WIN32
	CHECK_FALSE(found);
	CHECK_FALSE(layer.active);
#else
	(void)found; // depends on the box
#endif
	CHECK_FALSE(u_openxr_implicit_layer_find(nullptr, &layer));
	CHECK_FALSE(u_openxr_implicit_layer_find(U_OPENXR_LAYER_ULTRALEAP_HAND_TRACKING, nullptr));
}
