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

# MCP adapter (Phase A): stdio ↔ per-PID-socket/pipe bridge.
add_subdirectory(mcp_adapter)

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

	# DisplayXR Shell (workspace controller) source has moved to
	# DisplayXR/displayxr-shell-pvt and ships from there. The runtime
	# discovers any installed workspace controller via the registry
	# contract; see docs/specs/workspace-controller-registration.md.

	# DisplayXR WebXR bridge host (metadata sideband for Chrome's native WebXR)
	add_subdirectory(webxr_bridge)
endif()
