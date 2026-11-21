# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0

# dxr-get (#2702 / #2704 / #2705): fetch, verify and install DisplayXR
# components from the signed component catalog. Deliberately standalone --
# it links nothing from the runtime, so a component installer (the DisplayXR
# Browser's first) can embed this one exe and run it on a box with no runtime.
# Spec: docs/specs/runtime/dxr-get.md.

add_executable(
	dxr-get
	get_main.c
	get_core.c
	get_http.c
	get_verify.c
	get_detect.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../../external/monocypher/monocypher.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../../external/monocypher/monocypher-ed25519.c
	${CMAKE_CURRENT_SOURCE_DIR}/../../../external/cjson/cjson/cJSON.c
	)
target_include_directories(
	dxr-get PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../../external/monocypher ${CMAKE_CURRENT_SOURCE_DIR}/../../../external/cjson
	)
target_compile_definitions(dxr-get PRIVATE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
target_link_libraries(dxr-get PRIVATE winhttp wintrust crypt32 bcrypt setupapi shell32 ole32 advapi32)
if(MSVC)
	# Static CRT: it is copied into other installers and run on boxes that
	# may not have the VC++ redistributable yet.
	set_property(TARGET dxr-get PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
	target_link_options(
		dxr-get PRIVATE "/MANIFEST:EMBED"
		"/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/../common/dpi_aware.manifest"
		)
endif()

# TEST ONLY. 64 hex digits of an Ed25519 public key that REPLACES the catalog
# root keys in dxr-get, so a local fixture catalog signed with a throwaway key
# (scripts/make_test_catalog.py) can be verified on a test machine without
# signing test content with the production key. Compile time only: there is no
# run-time override. Such a binary prints a banner on every run, reads local
# catalog files only, is not installed into the package, and makes the
# `installer` target fail. CI asserts no shipped dxr-get carries it.
# Recipe: docs/specs/runtime/dxr-get.md, "Validating on a test machine".
set(DXR_GET_TEST_ROOT_KEY
    ""
    CACHE STRING "TEST ONLY: Ed25519 public key (64 hex) replacing dxr-get's catalog root keys. Never for a build that ships."
    )
if(DXR_GET_TEST_ROOT_KEY)
	string(LENGTH "${DXR_GET_TEST_ROOT_KEY}" _dxr_get_key_len)
	if(NOT DXR_GET_TEST_ROOT_KEY MATCHES "^[0-9a-fA-F]+$" OR NOT _dxr_get_key_len EQUAL 64)
		message(FATAL_ERROR "DXR_GET_TEST_ROOT_KEY must be 64 hex digits (an Ed25519 public key)")
	endif()
	message(WARNING "dxr-get: TEST ROOT KEY build. It is not installed, and must never be distributed. "
	                "Clear it with -UDXR_GET_TEST_ROOT_KEY."
		)
	target_compile_definitions(dxr-get PRIVATE DXR_GET_TEST_ROOT_KEY_HEX="${DXR_GET_TEST_ROOT_KEY}")
else()
	install(TARGETS dxr-get RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
