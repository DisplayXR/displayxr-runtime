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
	get_http.c
	get_verify.c
	get_detect.c
	${CMAKE_SOURCE_DIR}/src/external/monocypher/monocypher.c
	${CMAKE_SOURCE_DIR}/src/external/monocypher/monocypher-ed25519.c
	${CMAKE_SOURCE_DIR}/src/external/cjson/cjson/cJSON.c
	)
target_include_directories(
	dxr-get PRIVATE ${CMAKE_SOURCE_DIR}/src/external/monocypher ${CMAKE_SOURCE_DIR}/src/external/cjson
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

install(TARGETS dxr-get RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
