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

## Library type: SHARED rather than MODULE.
##
## The OpenXR loader still resolves this DLL via LoadLibrary at runtime, which
## works equally for SHARED and MODULE on Windows. We pick SHARED because, in
## addition to producing the .dll the loader loads, vendor plug-in DLLs now
## link against the runtime's exported aux surface (ADR-019) via the import
## library (DisplayXRClient.lib). CMake's MODULE target type intentionally
## hides the import library from $<TARGET_LINKER_FILE:> and from the ARCHIVE
## install slot because MODULE communicates "loadable plug-in, do not link
## against." SHARED tracks the import library as a first-class artifact and
## makes it installable via the normal install(TARGETS ... ARCHIVE ...) path.
add_library(${RUNTIME_TARGET} SHARED target.c libopenxr.def)

# On Windows, output the runtime as DisplayXRClient.dll for the installer.
# On Linux/macOS, strip the default `lib` prefix that CMake adds to SHARED
# libraries so the produced file matches the historical MODULE name
# (`openxr_displayxr.so` / `.dylib`) and the existing OpenXR loader manifest.
set_target_properties(${RUNTIME_TARGET} PROPERTIES PREFIX "")
if(WIN32)
	set_target_properties(${RUNTIME_TARGET} PROPERTIES OUTPUT_NAME "DisplayXRClient")

	# DllMain + delay-load notification hook: resolves /DELAYLOAD'd DLLs
	# from the runtime's install dir without requiring $INSTDIR on PATH.
	target_sources(${RUNTIME_TARGET} PRIVATE target_dll_init.c)

	# Delay-load Vulkan so the runtime can load on systems where it's
	# absent. The D3D11 native compositor path never calls into Vulkan
	# weaver functions, so the delay-loaded DLL is never triggered.
	target_link_options(${RUNTIME_TARGET} PRIVATE "/DELAYLOAD:vulkan-1.dll")
	target_link_libraries(${RUNTIME_TARGET} PRIVATE delayimp)
	# Vendor-SDK delay-loads (SR Vulkan beta, etc.) live in the vendor
	# plug-in DLL's link line — see DisplayXR/displayxr-leia-plugin.
	# The runtime DLL holds zero vendor identifiers (ADR-019/#263).
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
			comp_client
			ipc_client
		)
endif()

if(APPLE)
	# macOS: drive exports from exported_symbols.list (single source of
	# truth shared with libopenxr.def on Windows and libopenxr.version on
	# Linux). The list includes the Khronos negotiate symbol, the legacy
	# in-process sim_display hook (for test apps that resolve it via
	# dlsym), the aux export surface enumerated in
	# docs/adr/ADR-019-vendor-plugin-aux-boundary.md, and the macOS-only
	# oxr_macos_* window-closed accessors.
	set_property(
		TARGET ${RUNTIME_TARGET}
		APPEND_STRING
		PROPERTY
			LINK_FLAGS
			" -Wl,-exported_symbols_list,\"${CMAKE_CURRENT_SOURCE_DIR}/exported_symbols.list\""
		)
	set_property(
		TARGET ${RUNTIME_TARGET}
		APPEND
		PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/exported_symbols.list"
		)
elseif(NOT WIN32)
	# Force the main "negotiate" symbol's inclusion
	# and use a version script to ensure only the gated symbols are
	# exposed. The version script enumerates the aux export surface per
	# ADR-019; only those + xrNegotiateLoaderRuntimeInterface are global.
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

# Strip vulkan-1.dll from the BUILD-TREE output dir too (issue #105). The
# install rules above filter it from _package/, but the dev manifest
# (DEV_MANIFEST_OUTPUT) points XR_RUNTIME_JSON at the build-tree
# DisplayXRClient.dll, and X_VCPKG_APPLOCAL_DEPS stages vulkan-1.dll right next
# to it as its own POST_BUILD step — a second Vulkan loader that shadows
# C:\Windows\System32\vulkan-1.dll → NVIDIA ICD null-deref in
# vkEnumeratePhysicalDevices the moment a dev app runs against the build tree.
# This POST_BUILD runs after applocal's (later registration = later execution),
# so the build-tree runtime dir relies on the system loader like the install
# does. Idempotent; Windows-only (vulkan-1.dll is a Windows loader name).
if(WIN32)
	add_custom_command(
		TARGET ${RUNTIME_TARGET}
		POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E rm -f "$<TARGET_FILE_DIR:${RUNTIME_TARGET}>/vulkan-1.dll"
		COMMENT "Removing build-tree vulkan-1.dll (runtime uses the system loader; issue #105)"
		VERBATIM
		)
endif()

###
# Prepare the installable manifest: will be generated completely at install time,
# by a script we generate now.
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
	# Windows: install manifest to top level with DisplayXR naming
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
		OUT_FILENAME "DisplayXR_win64.json"
		)
	install(
		TARGETS ${RUNTIME_TARGET}
		LIBRARY DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Runtime
		RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Runtime
		# DisplayXRClient.lib — import library that vendor plug-in DLLs
		# link against to resolve the aux export surface (ADR-019).
		# The SHARED target type makes MSVC's auto-generated .lib a
		# first-class CMake artifact promotable via the ARCHIVE slot.
		ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Development
		)
	if("${CMAKE_VERSION}" VERSION_GREATER_EQUAL 3.21)
		# This generator expression is easier to control than install(RUNTIME_DEPENDENCY_SETS),
		# which is prone to trying to distribute system DLLs.
		#
		# Exclude vulkan-1.dll: the runtime delay-loads it, so it shows up here,
		# but shipping our own copy next to DisplayXRClient.dll creates a SECOND
		# Vulkan loader in the app process (the app's VkInstance is built by the
		# system loader, the runtime resolves vkEnumeratePhysicalDevices from our
		# local copy → mismatched ICD/terminator state → NVIDIA ICD null-deref).
		# The installed runtime dir must rely on the system vulkan-1.dll. The NSIS
		# installer already excludes it; this filter closes the same gap for the
		# _package/ tree used by local dev deploys. See issue #105.
		install(
			FILES $<FILTER:$<TARGET_RUNTIME_DLLS:${RUNTIME_TARGET}>,EXCLUDE,vulkan-1\\.dll>
			DESTINATION ${CMAKE_INSTALL_BINDIR}
			COMPONENT Runtime
			)
	endif()
	# Ship openxr_loader.dll + pthreadVC3.dll inside $INSTDIR so
	# downstream consumers (displayxr-shell.exe, third-party workspace
	# apps) can resolve them via the standard exe-directory DLL search
	# when they live in the same install tree. The runtime DLL itself
	# does NOT link openxr_loader (the loader loads US) and (on this
	# branch) no longer imports pthreadVC3; these install rules exist
	# only for the downstream-consumer convenience surface.
	if(DEFINED OpenXR_ROOT AND EXISTS "${OpenXR_ROOT}/x64/bin/openxr_loader.dll")
		install(
			FILES "${OpenXR_ROOT}/x64/bin/openxr_loader.dll"
			DESTINATION ${CMAKE_INSTALL_BINDIR}
			COMPONENT Runtime
			)
	endif()
	# pthreadVC3 (C-only, no C++ exception unwind) and cjson are what
	# displayxr-shell.exe v1.2.5 dynamically imports. Source from the
	# vcpkg-installed bin dir; X_VCPKG_APPLOCAL_DEPS_INSTALL only copies
	# what the runtime DLL itself transitively links, which on this
	# branch is neither variant (pthread unused after plug-in extract;
	# cjson routed to MCP's vendored static lib via XRT_HAVE_LINKED_CJSON).
	if(EXISTS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/pthreadVC3.dll")
		install(
			FILES "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/pthreadVC3.dll"
			DESTINATION ${CMAKE_INSTALL_BINDIR}
			COMPONENT Runtime
			)
	endif()
	if(EXISTS "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/cjson.dll")
		install(
			FILES "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/cjson.dll"
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
