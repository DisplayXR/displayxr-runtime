# Copyright 2024, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0

# DisplayXR NSIS Installer CMake integration

if(NOT WIN32)
	message(FATAL_ERROR "Installer can only be built on Windows")
endif()

# Find NSIS
find_program(NSIS_EXECUTABLE makensis
	PATHS
		"$ENV{PROGRAMFILES}/NSIS"
		"$ENV{PROGRAMFILES\(X86\)}/NSIS"
		"C:/Program Files/NSIS"
		"C:/Program Files (x86)/NSIS"
)

if(NOT NSIS_EXECUTABLE)
	message(WARNING "NSIS not found - installer target will not be available")
	return()
endif()

message(STATUS "Found NSIS: ${NSIS_EXECUTABLE}")

# Version information
set(DISPLAYXR_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(DISPLAYXR_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(DISPLAYXR_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(DISPLAYXR_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}")
# What the installer is NAMED after (#1790): X.Y.Z on a release tag, else
# X.Y.Z+<n>.g<sha> so a main/PR build can never pass for the release. Values
# other tools parse as a version (HKLM ...\Runtime\Version, which the Leia
# plug-in and browser installers gate on; ARP DisplayVersion; VIProductVersion)
# keep the plain X.Y.Z above.
set(DISPLAYXR_VERSION_LABEL "${DXR_RUNTIME_VERSION_PACKAGE}")

# Build number (from CI or default to 0)
if(NOT DEFINED BUILD_NUM)
	set(BUILD_NUM "0")
endif()

# Optional code-signing command, sourced from the SIGN_CMD environment
# variable at configure time. Empty on any machine without it (clones,
# GitHub-hosted CI) -> the installer + uninstaller build unsigned. On a
# signing-capable build machine SIGN_CMD points at the configured signer.
# Passed through to makensis, where the .nsi uses it in !uninstfinalize
# (and !finalize) to sign the uninstaller stub that is embedded at build
# time. No cert/secret lives in this repo.
#
# FORCE so the live SIGN_CMD env always wins on reconfigure: a plain CACHE
# var is seeded only on the first configure, so a build tree first configured
# with SIGN_CMD unset would keep caching "" even after SIGN_CMD is later set,
# silently leaving the embedded uninstaller unsigned (#664 validation).
set(SIGN_CMD "$ENV{SIGN_CMD}" CACHE STRING "Code-signing command for installer/uninstaller (empty = unsigned)" FORCE)

# Generate version header for NSIS
configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/Version.nsh.in"
	"${CMAKE_CURRENT_BINARY_DIR}/Version.nsh"
	@ONLY
)

# Paths to build artifacts (use install prefix, not build dir)
set(DISPLAYXR_BIN_DIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}")

# #2234: the default conversion model pack is the open provider's
# packs/dxr-open-default.models.json at the tag versions.json pins
# (`conversion_open`, auto-bumped on every provider release), so the URL the
# installer falls back to always names a file that exists. The .nsi keeps a
# literal default for a makensis run outside CMake.
file(READ "${CMAKE_SOURCE_DIR}/versions.json" _dxr_versions_json)
string(JSON DXR_CONVERSION_OPEN_TAG ERROR_VARIABLE _dxr_conv_open_err GET "${_dxr_versions_json}" conversion_open)
if(_dxr_conv_open_err)
	message(FATAL_ERROR "versions.json has no conversion_open pin: ${_dxr_conv_open_err}")
endif()

# Create installer target
# White icon is set via MUI_ICON in the .nsi script (visible in dark mode Explorer).
# Do NOT use rcedit post-build — it corrupts NSIS self-extracting executables.
# ADR-054 N4b (PR B): the provenance allowlist rows for the plug-in DLL(s) the
# installer ships, computed from the exact bytes in the install prefix right
# before makensis runs (see gen_allowlist_nsh.cmake).
set(DISPLAYXR_ALLOWLIST_NSH "${CMAKE_CURRENT_BINARY_DIR}/Allowlist.nsh")
# makensis resolves `!include` against a native (backslash) path only.
file(TO_NATIVE_PATH "${DISPLAYXR_ALLOWLIST_NSH}" DISPLAYXR_ALLOWLIST_NSH_NATIVE)

add_custom_target(installer
	COMMAND "${CMAKE_COMMAND}"
		"-DBIN_DIR=${DISPLAYXR_BIN_DIR}"
		"-DVERSION=${DISPLAYXR_VERSION}"
		"-DOUT=${DISPLAYXR_ALLOWLIST_NSH}"
		-P "${CMAKE_CURRENT_SOURCE_DIR}/gen_allowlist_nsh.cmake"
	COMMAND "${NSIS_EXECUTABLE}"
		"/DVERSION=${DISPLAYXR_VERSION}"
		"/DVERSION_LABEL=${DISPLAYXR_VERSION_LABEL}"
		"/DVERSION_MAJOR=${DISPLAYXR_VERSION_MAJOR}"
		"/DVERSION_MINOR=${DISPLAYXR_VERSION_MINOR}"
		"/DVERSION_PATCH=${DISPLAYXR_VERSION_PATCH}"
		"/DBUILD_NUM=${BUILD_NUM}"
		"/DBIN_DIR=${DISPLAYXR_BIN_DIR}"
		"/DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
		"/DOUTPUT_DIR=${CMAKE_INSTALL_PREFIX}"
		"/DSIGN_CMD=${SIGN_CMD}"
		"/DALLOWLIST_NSH=${DISPLAYXR_ALLOWLIST_NSH_NATIVE}"
		"/DDXR_CONVERSION_OPEN_TAG=${DXR_CONVERSION_OPEN_TAG}"
		"${CMAKE_CURRENT_SOURCE_DIR}/DisplayXRInstaller.nsi"
	WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
	COMMENT "Building DisplayXR installer..."
	VERBATIM
)

# Dependencies for installer target
if(TARGET ${RUNTIME_TARGET})
	add_dependencies(installer ${RUNTIME_TARGET})
endif()
if(TARGET displayxr-service)
	add_dependencies(installer displayxr-service)
endif()
# ADR-053 P2: the conversion-host satellite ships beside the service.
if(TARGET displayxr-conversion-host)
	add_dependencies(installer displayxr-conversion-host)
endif()
if(TARGET cli)
	add_dependencies(installer cli)
endif()
# Vendor plug-in: sim-display. The runtime installer ships this DLL as
# the vendor-neutral fallback (ADR-019 / issue #256); vendor plug-ins
# like DisplayXR-LeiaSR.dll come from their own installers.
if(TARGET drv_sim_display_plugin)
	add_dependencies(installer drv_sim_display_plugin)
endif()

# Vendor plug-in installers (Leia SR, etc.) ship from their own repos
# per ADR-019 / issues #256 & #263 — see
# DisplayXR/displayxr-leia-plugin for the Leia plug-in's installer
# (DisplayXRLeiaSRSetup-*.exe). The runtime installer hard-requires
# the runtime registry key, then each vendor's installer self-registers
# at HKLM\Software\DisplayXR\DisplayProcessors\<id> for the runtime's
# registry-driven discovery to pick up. The runtime uninstaller
# cascade-cleans any registered vendor plug-in via its UninstallString.
#
# Workspace controllers (the DisplayXR shell, third-party verticals, …)
# ship as their own products with their own installers — the runtime
# installer does not bundle them. The DisplayXR Shell ships from
# DisplayXR/displayxr-shell-pvt with its own installer/CMakeLists.txt
# (publishes to displayxr-shell-releases). See
# docs/specs/runtime/workspace-controller-registration.md for the
# self-registration contract any workspace app uses.
