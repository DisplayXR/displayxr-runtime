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

# Find rcedit for post-build icon swap (installer .exe gets white icon for dark Explorer)
find_program(RCEDIT_EXECUTABLE rcedit
	PATHS
		"${CMAKE_SOURCE_DIR}/tools"
		"$ENV{PROGRAMFILES}/rcedit"
)

# Create installer target
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

# Post-build: swap the .exe icon to white (visible in dark Explorer/taskbar)
# The wizard UI keeps the black icon (baked by NSIS via MUI_ICON).
# rcedit replaces the .exe's main icon resource shown by Explorer.
if(RCEDIT_EXECUTABLE)
	add_custom_command(TARGET installer POST_BUILD
		COMMAND "${RCEDIT_EXECUTABLE}"
			"${CMAKE_INSTALL_PREFIX}/DisplayXRSetup-${DISPLAYXR_VERSION}.${BUILD_NUM}.exe"
			--set-icon "${CMAKE_SOURCE_DIR}/doc/displayxr_white.ico"
		COMMENT "Replacing installer .exe icon with white variant for dark mode Explorer"
		VERBATIM
	)
else()
	message(STATUS "rcedit not found - installer .exe will use black icon (install rcedit for dark mode support)")
endif()

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
