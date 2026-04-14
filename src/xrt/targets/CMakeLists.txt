# Copyright 2019-2020, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0

######
# This is where we collect all of the pieces from the different parts of
# the source tree and build a complete driver or integration part.

add_subdirectory(common)

# Monado management library
if(XRT_FEATURE_SERVICE AND XRT_HAVE_LINUX)
	add_subdirectory(libmonado)
endif()

if(XRT_FEATURE_OPENXR)
	add_subdirectory(openxr)
endif()

if(XRT_MODULE_MONADO_CLI)
	add_subdirectory(cli)
endif()

# MCP adapter (Phase A): stdio ↔ per-PID-socket bridge.
# POSIX only for now; Windows named-pipe variant lands in Phase A slice 7.
if(NOT WIN32)
	add_subdirectory(mcp_adapter)
endif()

if(XRT_MODULE_MONADO_GUI)
	add_subdirectory(gui)
endif()

if(XRT_FEATURE_SERVICE AND NOT WIN32)
	add_subdirectory(ctl)
endif()

if(XRT_FEATURE_SERVICE AND XRT_FEATURE_OPENXR)
	if(ANDROID)
		add_subdirectory(service-lib)
	else()
		add_subdirectory(service)
	endif()
endif()

if(XRT_FEATURE_STEAMVR_PLUGIN)
	add_subdirectory(steamvr_drv)
endif()

if(XRT_FEATURE_SERVICE
   AND XRT_HAVE_SDL2
   AND XRT_HAVE_VULKAN
	)
	add_subdirectory(sdl_test)
endif()

# DisplayXR Windows components
if(WIN32)
	# DisplayXR runtime switcher
	if(XRT_BUILD_SWITCHER)
		add_subdirectory(switcher)
	endif()

	# DisplayXR spatial shell (management client)
	if(XRT_FEATURE_SERVICE)
		add_subdirectory(shell)
	endif()
endif()
