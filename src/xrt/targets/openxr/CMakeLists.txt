# Copyright 2019, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0

######
# Create a loadable OpenXR driver.

set(RUNTIME_SUFFIX _${XRT_OXR_RUNTIME_SUFFIX})

set(RUNTIME_TARGET
    ${RUNTIME_PREFIX}openxr${RUNTIME_SUFFIX}
    CACHE INTERNAL "" FORCE
	)

# OpenXR 1.0
set(XR_API_MAJOR "1")

add_library(${RUNTIME_TARGET} MODULE target.c libopenxr.def)

# On Windows, output the runtime as SRMonadoClient.dll for the installer
if(WIN32)
	set_target_properties(${RUNTIME_TARGET} PROPERTIES OUTPUT_NAME "SRMonadoClient")

	# Delay-load Vulkan and SR Vulkan DLLs so the runtime can load on systems
	# where these are absent. The D3D11 native compositor path never calls into
	# Vulkan weaver functions, so the delay-loaded DLLs are never triggered.
	target_link_options(${RUNTIME_TARGET} PRIVATE
		"/DELAYLOAD:vulkan-1.dll"
		"/DELAYLOAD:SimulatedRealityVulkanBeta.dll"
	)
	target_link_libraries(${RUNTIME_TARGET} PRIVATE delayimp)
endif()

# Note: Order may matter in these lists!
target_link_libraries(${RUNTIME_TARGET} PUBLIC aux_vk aux_os aux_util aux_math)

if(XRT_FEATURE_HYBRID_MODE)
	# Hybrid mode: link both native compositor and IPC client
	# Runtime auto-selects based on sandbox detection
	target_link_libraries(
		${RUNTIME_TARGET}
		PUBLIC
			st_oxr
			st_prober
			target_lists
			target_instance_hybrid  # Native compositor (renamed to avoid symbol conflict)
			ipc_client              # IPC client for sandboxed apps
			comp_main
			comp_client
		)
	target_compile_definitions(${RUNTIME_TARGET} PRIVATE XRT_FEATURE_HYBRID_MODE)
elseif(XRT_FEATURE_IPC_CLIENT)
	target_link_libraries(${RUNTIME_TARGET} PUBLIC st_oxr ipc_client comp_client comp_multi)
else()
	target_link_libraries(
		${RUNTIME_TARGET}
		PUBLIC
			st_oxr
			st_prober
			target_lists
			target_instance
			comp_main
			comp_client
		)
endif()

if(APPLE)
	# macOS: export only the negotiate symbol using linker flags.
	# GNU ld version scripts are not supported by the macOS linker.
	set_property(
		TARGET ${RUNTIME_TARGET}
		APPEND_STRING
		PROPERTY
			LINK_FLAGS
			" -Wl,-exported_symbol,_xrNegotiateLoaderRuntimeInterface"
		)
elseif(NOT WIN32)
	# Force the main "negotiate" symbol's inclusion
	# and use a version script to ensure that's the only one we expose.
	set_property(
		TARGET ${RUNTIME_TARGET}
		APPEND_STRING
		PROPERTY
			LINK_FLAGS
			"-u xrNegotiateLoaderRuntimeInterface -Wl,--version-script=\"${CMAKE_CURRENT_SOURCE_DIR}/libopenxr.version\""
		)
	# Re-link if the version script changes.
	set_property(
		TARGET ${RUNTIME_TARGET}
		APPEND
		PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/libopenxr.version"
		)
endif()

###
# Generate runtime manifest with absolute path to runtime intended for development without installing

# 3.9+ have a global property with the truth
get_property(IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

if(IS_MULTI_CONFIG)
	set(DEV_MANIFEST_OUTPUT "${CMAKE_BINARY_DIR}/$<CONFIG>/${RUNTIME_TARGET}-dev.json")
else()
	set(DEV_MANIFEST_OUTPUT "${CMAKE_BINARY_DIR}/${RUNTIME_TARGET}-dev.json")
endif()

set(_libmonado_arg)
if(TARGET monado)
	set(_libmonado_arg LIBMONADO monado)
endif()

include(GenerateOpenXRRuntimeManifest)
generate_openxr_runtime_manifest_buildtree(
	${_libmonado_arg}
	RUNTIME_TARGET ${RUNTIME_TARGET} #
	OUT_FILE ${DEV_MANIFEST_OUTPUT}
	)

###
# Prepare the installable manifest: will be generated completely at install time,
# by a script we generate now.
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	# Windows: install manifest to top level with SRMonado naming
	# The installer copies everything to a flat directory, so use "." for the
	# relative path (manifest and DLL will be in the same directory)
	set(_extra_args RUNTIME_DIR_RELATIVE_TO_MANIFEST .)
	if(XRT_OPENXR_INSTALL_ABSOLUTE_RUNTIME_PATH)
		set(_extra_args ABSOLUTE_RUNTIME_PATH)
	endif()
	generate_openxr_runtime_manifest_at_install(
		${_extra_args}
		RUNTIME_TARGET ${RUNTIME_TARGET}
		DESTINATION .
		RELATIVE_RUNTIME_DIR ${CMAKE_INSTALL_BINDIR}
		OUT_FILENAME "SRMonado_win64.json"
		)
	install(
		TARGETS ${RUNTIME_TARGET}
		LIBRARY DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Runtime
		RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Runtime
		)
	if("${CMAKE_VERSION}" VERSION_GREATER_EQUAL 3.21)
		# This generator expression is easier to control than install(RUNTIME_DEPENDENCY_SETS),
		# which is prone to trying to distribute system DLLs.
		install(
			FILES $<TARGET_RUNTIME_DLLS:${RUNTIME_TARGET}>
			DESTINATION ${CMAKE_INSTALL_BINDIR}
			COMPONENT Runtime
			)
	endif()
elseif(NOT ANDROID)
	# Linux, BSD, etc.
	set(MANIFEST_RELATIVE_DIR share/openxr/${XR_API_MAJOR}/)
	if(XRT_OPENXR_INSTALL_ACTIVE_RUNTIME)
		configure_file(
			active_runtime.cmake ${CMAKE_CURRENT_BINARY_DIR}/active_runtime.cmake @ONLY
			)
		install(SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/active_runtime.cmake)
	endif()

	set(_extra_args)
	if(XRT_OPENXR_INSTALL_ABSOLUTE_RUNTIME_PATH)
		set(_extra_args ABSOLUTE_RUNTIME_PATH)
	elseif(XRT_OPENXR_INSTALL_MANIFEST_RELATIVE_RUNTIME_PATH)
		set(_extra_args RUNTIME_DIR_RELATIVE_TO_MANIFEST ../../../${CMAKE_INSTALL_LIBDIR})
	endif()
	generate_openxr_runtime_manifest_at_install(
		${_extra_args} ${_libmonado_arg}
		RUNTIME_TARGET ${RUNTIME_TARGET}
		DESTINATION share/openxr/${XR_API_MAJOR}
		RELATIVE_RUNTIME_DIR ${CMAKE_INSTALL_LIBDIR}
		)
	install(
		TARGETS ${RUNTIME_TARGET} #
		LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Runtime
		RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Runtime
		)
endif()

# Optional debug ui
if(XRT_FEATURE_CLIENT_DEBUG_GUI)
	target_link_libraries(${RUNTIME_TARGET} PRIVATE aux_util_debug_gui)
endif()
