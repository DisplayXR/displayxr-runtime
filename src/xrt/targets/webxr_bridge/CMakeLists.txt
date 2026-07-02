# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# displayxr-webxr-bridge — tiny OpenXR client that coexists with Chrome's
# built-in WebXR session as a metadata/control sideband. Enables
# XR_EXT_display_info and XR_MND_headless, logs display info and
# rendering-mode events. Phase 1 of docs/roadmap/webxr-bridge-v2-plan.md.

if(NOT WIN32)
	return()
endif()

# The bridge is an OpenXR *client*, so it needs the OpenXR loader.
# The top-level build does not normally link the loader (the runtime
# itself is an OpenXR implementation), so we discover it here via an
# OpenXR_ROOT hint passed from scripts/build_windows.bat.
find_package(OpenXR CONFIG QUIET)
if(NOT OpenXR_FOUND)
	if(DEFINED OpenXR_ROOT AND EXISTS "${OpenXR_ROOT}/x64/lib/openxr_loader.lib")
		add_library(OpenXR::openxr_loader SHARED IMPORTED)
		set_target_properties(OpenXR::openxr_loader PROPERTIES
			IMPORTED_IMPLIB "${OpenXR_ROOT}/x64/lib/openxr_loader.lib"
			IMPORTED_LOCATION "${OpenXR_ROOT}/x64/bin/openxr_loader.dll"
		)
		set(OpenXR_FOUND TRUE)
	endif()
endif()

# The prebuilt OpenXR SDK's config package declares the loader target for a
# single configuration (RelWithDebInfo only). Multi-config generators (the
# Visual Studio one in particular) hard-error when generating any other
# configuration of a consumer: "IMPORTED_IMPLIB not set for ... MinSizeRel".
# Backfill every configuration this build generates from whichever implib the
# package did declare — the loader is a plain prebuilt DLL, so one binary
# serves all configs.
if(TARGET OpenXR::openxr_loader)
	get_target_property(_xr_implib OpenXR::openxr_loader IMPORTED_IMPLIB)
	get_target_property(_xr_dll OpenXR::openxr_loader IMPORTED_LOCATION)
	get_target_property(_xr_configs OpenXR::openxr_loader IMPORTED_CONFIGURATIONS)
	if(NOT _xr_implib AND _xr_configs)
		list(GET _xr_configs 0 _xr_cfg0)
		get_target_property(_xr_implib OpenXR::openxr_loader IMPORTED_IMPLIB_${_xr_cfg0})
		get_target_property(_xr_dll OpenXR::openxr_loader IMPORTED_LOCATION_${_xr_cfg0})
	endif()
	if(_xr_implib)
		foreach(_xr_cfg ${CMAKE_CONFIGURATION_TYPES} ${CMAKE_BUILD_TYPE})
			string(TOUPPER "${_xr_cfg}" _xr_cfg_uc)
			get_target_property(_xr_have OpenXR::openxr_loader IMPORTED_IMPLIB_${_xr_cfg_uc})
			if(NOT _xr_have)
				set_property(
					TARGET OpenXR::openxr_loader APPEND
					PROPERTY IMPORTED_CONFIGURATIONS ${_xr_cfg_uc}
					)
				set_target_properties(
					OpenXR::openxr_loader
					PROPERTIES IMPORTED_IMPLIB_${_xr_cfg_uc} "${_xr_implib}"
						   IMPORTED_LOCATION_${_xr_cfg_uc} "${_xr_dll}"
					)
			endif()
		endforeach()
	endif()
endif()

if(NOT OpenXR_FOUND)
	message(STATUS "webxr_bridge: OpenXR loader not found, skipping displayxr-webxr-bridge")
	return()
endif()

add_executable(displayxr-webxr-bridge main.cpp)
add_sanitizers(displayxr-webxr-bridge)

target_include_directories(displayxr-webxr-bridge PRIVATE
	${CMAKE_SOURCE_DIR}/src/external/openxr_includes
)

target_link_libraries(displayxr-webxr-bridge PRIVATE
	OpenXR::openxr_loader
	aux_util
	ipc_client
	ipc_shared
	ws2_32
)

# Default (dynamic) CRT matches the in-tree libs we now link against
# (aux_util, ipc_client, ipc_shared). Switching to /MT here would LNK2038
# against those libraries' /MD runtime. Matches the workspace controller's
# convention — it also uses /MD and links the same libs.

install(TARGETS displayxr-webxr-bridge RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Copy the OpenXR loader DLL next to the exe in the build tree so it can
# be run from _package/bin/ after install. _xr_dll was resolved above from
# whichever IMPORTED_LOCATION(_<CONFIG>) the package/fallback declared.
get_target_property(_loader_dll OpenXR::openxr_loader IMPORTED_LOCATION)
if(NOT _loader_dll AND _xr_dll)
	set(_loader_dll "${_xr_dll}")
endif()
if(_loader_dll)
	add_custom_command(TARGET displayxr-webxr-bridge POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${_loader_dll}" "$<TARGET_FILE_DIR:displayxr-webxr-bridge>/"
		COMMENT "Copying openxr_loader.dll next to displayxr-webxr-bridge.exe"
	)
	install(FILES "${_loader_dll}" DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
