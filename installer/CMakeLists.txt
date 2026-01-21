# Copyright 2024, Leia Inc.
# SPDX-License-Identifier: BSL-1.0

# SRMonado NSIS Installer CMake integration

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
set(SRMONADO_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(SRMONADO_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(SRMONADO_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(SRMONADO_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}")

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
set(SRMONADO_BIN_DIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}")

# Create installer target
add_custom_target(installer
	COMMAND "${NSIS_EXECUTABLE}"
		"/DVERSION=${SRMONADO_VERSION}"
		"/DVERSION_MAJOR=${SRMONADO_VERSION_MAJOR}"
		"/DVERSION_MINOR=${SRMONADO_VERSION_MINOR}"
		"/DVERSION_PATCH=${SRMONADO_VERSION_PATCH}"
		"/DBUILD_NUM=${BUILD_NUM}"
		"/DBIN_DIR=${SRMONADO_BIN_DIR}"
		"/DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
		"/DOUTPUT_DIR=${CMAKE_INSTALL_PREFIX}"
		"${CMAKE_CURRENT_SOURCE_DIR}/SRMonadoInstaller.nsi"
	WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
	COMMENT "Building SRMonado installer..."
	VERBATIM
)

# Dependencies for installer target
if(TARGET ${RUNTIME_TARGET})
	add_dependencies(installer ${RUNTIME_TARGET})
endif()
if(TARGET monado-service)
	add_dependencies(installer monado-service)
endif()
if(TARGET LeiaXRSwitcher)
	add_dependencies(installer LeiaXRSwitcher)
endif()
