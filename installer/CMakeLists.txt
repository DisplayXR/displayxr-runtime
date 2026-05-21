# Copyright 2024, Leia Inc.
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

# Build number (from CI or default to 0)
if(NOT DEFINED BUILD_NUM)
	set(BUILD_NUM "0")
endif()

# Generate version header for NSIS
configure_file(
	"${CMAKE_CURRENT_SOURCE_DIR}/Version.nsh.in"
	"${CMAKE_CURRENT_BINARY_DIR}/Version.nsh"
	@ONLY
)

# Paths to build artifacts (use install prefix, not build dir)
set(DISPLAYXR_BIN_DIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}")

# Create installer target
# White icon is set via MUI_ICON in the .nsi script (visible in dark mode Explorer).
# Do NOT use rcedit post-build — it corrupts NSIS self-extracting executables.
add_custom_target(installer
	COMMAND "${NSIS_EXECUTABLE}"
		"/DVERSION=${DISPLAYXR_VERSION}"
		"/DVERSION_MAJOR=${DISPLAYXR_VERSION_MAJOR}"
		"/DVERSION_MINOR=${DISPLAYXR_VERSION_MINOR}"
		"/DVERSION_PATCH=${DISPLAYXR_VERSION_PATCH}"
		"/DBUILD_NUM=${BUILD_NUM}"
		"/DBIN_DIR=${DISPLAYXR_BIN_DIR}"
		"/DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
		"/DOUTPUT_DIR=${CMAKE_INSTALL_PREFIX}"
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
if(TARGET DisplayXRSwitcher)
	add_dependencies(installer DisplayXRSwitcher)
endif()
# Vendor plug-in: sim-display. The runtime installer ships this DLL as
# the vendor-neutral fallback (ADR-019 / issue #256); vendor plug-ins
# like DisplayXR-LeiaSR.dll come from their own installers.
if(TARGET drv_sim_display_plugin)
	add_dependencies(installer drv_sim_display_plugin)
endif()

# ---------------------------------------------------------------------
# Leia SR plug-in installer (separate product per plan §4.6).
#
# Independent NSIS .exe that ships DisplayXR-LeiaSR.dll +
# SimulatedRealityVulkanBeta.dll fallback. Prereq-checks the runtime
# via HKLM\Software\DisplayXR\Runtime\InstallPath and registers itself
# at HKLM\Software\DisplayXR\DisplayProcessors\leia-sr with
# ProbeOrder=50.
#
# Gated on XRT_HAVE_LEIA_SR so a runtime build without the SR SDK
# doesn't try to bundle a non-existent plug-in DLL.
# ---------------------------------------------------------------------
if(XRT_HAVE_LEIA_SR AND TARGET drv_leia_plugin)
	# Path to the bundled SR Vulkan Beta DLL (test_apps/common/ has the
	# canonical copy used by both the in-proc fallback and now the
	# plug-in installer). Pass as a /D define to NSIS, with backslashes
	# — NSIS's `File` command parses forward-slash paths inconsistently
	# when the path contains spaces (the runtime installer's BIN_DIR
	# avoids this only because the resolved File arg ends in
	# `${BIN_DIR}\file.dll`, where the trailing backslash terminates
	# the spaces-laden prefix).
	set(SR_VK_BETA_DLL "${CMAKE_SOURCE_DIR}/test_apps/common/SimulatedRealityVulkanBeta.dll")
	file(TO_NATIVE_PATH "${SR_VK_BETA_DLL}" SR_VK_BETA_DLL)

	add_custom_target(installer-leia
		COMMAND "${NSIS_EXECUTABLE}"
			"/DVERSION=${DISPLAYXR_VERSION}"
			"/DVERSION_MAJOR=${DISPLAYXR_VERSION_MAJOR}"
			"/DVERSION_MINOR=${DISPLAYXR_VERSION_MINOR}"
			"/DVERSION_PATCH=${DISPLAYXR_VERSION_PATCH}"
			"/DBUILD_NUM=${BUILD_NUM}"
			"/DBIN_DIR=${DISPLAYXR_BIN_DIR}"
			"/DSR_VK_BETA_DLL=${SR_VK_BETA_DLL}"
			"/DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
			"/DOUTPUT_DIR=${CMAKE_INSTALL_PREFIX}"
			"${CMAKE_CURRENT_SOURCE_DIR}/DisplayXRLeiaSRInstaller.nsi"
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
		COMMENT "Building DisplayXR Leia SR plug-in installer..."
		VERBATIM
	)
	add_dependencies(installer-leia drv_leia_plugin)

	# Make the umbrella `installer` target build both installers.
	# `scripts\build_windows.bat installer` invokes `installer`, so this
	# picks up the Leia installer for free in dev + CI flows.
	add_dependencies(installer installer-leia)
endif()
# Workspace controllers (the DisplayXR shell, third-party verticals, …)
# ship as their own products with their own installers — the runtime
# installer does not bundle them. The DisplayXR Shell ships from
# DisplayXR/displayxr-shell-pvt with its own installer/CMakeLists.txt
# (publishes to displayxr-shell-releases). See
# docs/specs/runtime/workspace-controller-registration.md for the
# self-registration contract any workspace app uses.
