# Copyright 2019-2021, Collabora, Ltd.
#
# SPDX-License-Identifier: BSL-1.0

set(ENABLED_HEADSET_DRIVERS)
set(ENABLED_DRIVERS)

add_library(drv_includes INTERFACE)
target_include_directories(drv_includes INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

if(XRT_BUILD_DRIVER_QWERTY)
	set(QWERTY_SOURCES
		qwerty/qwerty_device.c
		qwerty/qwerty_device.h
		qwerty/qwerty_interface.h
		qwerty/qwerty_prober.c
	)

	# SDL backend (cross-platform debug GUI input) — only when SDL2 is available
	if(XRT_HAVE_SDL2)
		list(APPEND QWERTY_SOURCES qwerty/qwerty_sdl.c)
	endif()

	# On Windows, add Win32 input handler for D3D11 compositor direct input
	if(WIN32)
		list(APPEND QWERTY_SOURCES qwerty/qwerty_win32.c)
	endif()

	# On macOS, add NSEvent input handler for Vulkan compositor window
	if(APPLE)
		list(APPEND QWERTY_SOURCES qwerty/qwerty_macos.m)
	endif()

	add_library(drv_qwerty STATIC ${QWERTY_SOURCES})
	target_link_libraries(drv_qwerty PRIVATE xrt-interfaces aux_util)
	if(XRT_HAVE_SDL2)
		target_link_libraries(drv_qwerty PRIVATE SDL2::SDL2)
	endif()
	if(APPLE)
		target_link_libraries(drv_qwerty PRIVATE "-framework Cocoa" "-framework Carbon")
	endif()
	list(APPEND ENABLED_DRIVERS qwerty)

	add_library(drv_qwerty_includes INTERFACE)
	target_include_directories(drv_qwerty_includes INTERFACE qwerty)
endif()

# Vendor drv_* sources (Leia SR, etc.) ship from separate plug-in repos
# per ADR-019 / issues #256 & #263. The runtime DLL holds zero vendor
# identifiers in its link line; vendor plug-ins are loaded at
# xrCreateInstance time via registry-driven discovery (Windows) or
# JSON-manifest discovery (POSIX) — see docs/specs/runtime/plugin-discovery.md
# and docs/guides/vendor-plugin-onboarding.md.
#
# Reference plug-in implementations:
#   DisplayXR/displayxr-leia-plugin — Leia SR display-processor plug-in
#   drivers/sim_display/sim_display_plugin.c (below) — vendor-neutral fallback

# Header-only interface for sim_display (used by st_oxr for mode switching)
add_library(drv_sim_display_includes INTERFACE)
target_include_directories(drv_sim_display_includes INTERFACE sim_display)

# Simulation 3D display driver (always available as fallback, FORCE_SIM_DISPLAY=1 overrides vendor drivers)
set(SIM_DISPLAY_SOURCES
	sim_display/sim_display_device.c
	sim_display/sim_display_interface.h
	)

# Vulkan display processor (anaglyph/blend shaders)
if(XRT_HAVE_VULKAN)
	spirv_shaders(
		SIM_DISPLAY_SHADER_HEADERS
		SPIRV_VERSION
		1.0
		SOURCES
		sim_display/shaders/fullscreen.vert
		sim_display/shaders/anaglyph.frag
		sim_display/shaders/blend.frag
		sim_display/shaders/sbs.frag
		sim_display/shaders/squeezed_sbs.frag
		sim_display/shaders/quad.frag
		sim_display/shaders/passthrough.frag
		)
	list(APPEND SIM_DISPLAY_SOURCES
		${SIM_DISPLAY_SHADER_HEADERS}
		sim_display/sim_display_processor.c)
endif()

# D3D11 display processor (HLSL shaders compiled at runtime)
if(WIN32)
	list(APPEND SIM_DISPLAY_SOURCES
		sim_display/sim_display_processor_d3d11.cpp)
endif()

# D3D12 display processor (HLSL shaders compiled at runtime into PSOs)
if(WIN32 AND XRT_HAVE_D3D12)
	list(APPEND SIM_DISPLAY_SOURCES
		sim_display/sim_display_processor_d3d12.cpp)
endif()

# GL display processor (GLSL shaders compiled at runtime)
list(APPEND SIM_DISPLAY_SOURCES
	sim_display/sim_display_processor_gl.c)

# macOS visible frame helper (NSScreen query)
if(APPLE)
	list(APPEND SIM_DISPLAY_SOURCES
		sim_display/sim_display_macos.m
		sim_display/sim_display_processor_metal.m)
endif()

add_library(drv_sim_display STATIC ${SIM_DISPLAY_SOURCES})
target_link_libraries(drv_sim_display PRIVATE xrt-interfaces aux_util aux_os aux_ogl PUBLIC drv_includes)

# aux_ogl headers (ogl_api.h) are in auxiliary/ogl/
target_include_directories(drv_sim_display PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../auxiliary)

if(XRT_HAVE_VULKAN)
	target_link_libraries(drv_sim_display PRIVATE aux_vk)
	target_include_directories(drv_sim_display PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
endif()

if(WIN32)
	target_link_libraries(drv_sim_display PRIVATE d3d11 d3dcompiler opengl32)
	if(XRT_HAVE_D3D12)
		target_link_libraries(drv_sim_display PRIVATE d3d12)
	endif()
endif()

if(APPLE)
	target_link_libraries(drv_sim_display PRIVATE "-framework CoreGraphics" "-framework AppKit" "-framework Metal" "-framework OpenGL")
	target_compile_definitions(drv_sim_display PRIVATE GL_SILENCE_DEPRECATION)
endif()

list(APPEND ENABLED_DRIVERS drv_sim_display)

####
# DisplayXR-SimDisplay plug-in DLL.
#
# Same sources as the static drv_sim_display target above, but rebuilt
# with XRT_USING_RUNTIME_DLL so that XRT_API_FUNC declarations in the
# aux headers (see ADR-019) expand to __declspec(dllimport). All aux
# symbols enumerated in libopenxr.def therefore resolve to
# DisplayXRClient.dll at runtime — a single copy of u_log /
# g_log_sink_func / gTracker / etc. per process. Non-exported aux
# helpers (math, containers, instance-based factories) are still
# statically linked from aux_util / aux_os / aux_ogl; the linker pulls
# in only the .obj files satisfying unresolved symbols, so the
# exported TUs (u_logging.o, u_var.o, …) are NOT duplicated into the
# plug-in DLL.
#
# v1 ships this alongside the runtime — discovery + load wiring lands
# in the next sequencing step.
add_library(drv_sim_display_plugin SHARED ${SIM_DISPLAY_SOURCES} sim_display/sim_display_plugin.c)
target_compile_definitions(drv_sim_display_plugin PRIVATE XRT_USING_RUNTIME_DLL)
target_link_libraries(
	drv_sim_display_plugin
	PRIVATE
		# DisplayXRClient.lib — import library for the runtime DLL's
		# aux export surface. Generator expression to skip pulling in
		# the runtime target's PUBLIC INTERFACE_LINK_LIBRARIES (which
		# would drag in st_oxr, every compositor, etc.).
		$<TARGET_LINKER_FILE:${RUNTIME_TARGET}>
		# Non-exported aux helpers. The linker only pulls in .obj
		# files that satisfy unresolved symbols, so this does not
		# duplicate exported TUs into the plug-in DLL.
		xrt-interfaces
		aux_util
		aux_os
		aux_ogl
		drv_includes
	)
add_dependencies(drv_sim_display_plugin ${RUNTIME_TARGET})
target_include_directories(drv_sim_display_plugin PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/sim_display
	${CMAKE_CURRENT_SOURCE_DIR}/../../auxiliary
	)

if(XRT_HAVE_VULKAN)
	target_link_libraries(drv_sim_display_plugin PRIVATE aux_vk)
	target_include_directories(drv_sim_display_plugin PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
endif()

if(WIN32)
	target_link_libraries(drv_sim_display_plugin PRIVATE d3d11 d3dcompiler opengl32)
	if(XRT_HAVE_D3D12)
		target_link_libraries(drv_sim_display_plugin PRIVATE d3d12)
	endif()
endif()

if(APPLE)
	target_link_libraries(drv_sim_display_plugin PRIVATE "-framework CoreGraphics" "-framework AppKit" "-framework Metal" "-framework OpenGL")
	target_compile_definitions(drv_sim_display_plugin PRIVATE GL_SILENCE_DEPRECATION)
endif()

# Honor the ADR-019 invariant that every plug-in DLL exports exactly
# one symbol: xrtPluginNegotiate. The XRT_PLUGIN_EXPORT decoration in
# sim_display_plugin.c does the heavy lifting on Windows (dllexport).
# On Linux / macOS the visibility=hidden defaults only hide the
# plug-in's own TUs; static archives pulled in from aux_util / aux_os
# / aux_math carry their original (default-visible) symbols into the
# plug-in dylib unless we explicitly filter at the link step.
set_target_properties(drv_sim_display_plugin PROPERTIES
	OUTPUT_NAME "DisplayXR-SimDisplay"
	PREFIX ""
	C_VISIBILITY_PRESET hidden
	CXX_VISIBILITY_PRESET hidden
	VISIBILITY_INLINES_HIDDEN ON
	)

# Single-symbol export filter for the plug-in dylib. Mirrors the
# runtime's exported_symbols.list-driven Mach-O filter and Linux's
# version-script approach. xrtPluginNegotiate is the only ABI symbol
# the discovery loader resolves via dlsym.
if(APPLE)
	target_link_options(drv_sim_display_plugin PRIVATE
		"LINKER:-exported_symbol,_xrtPluginNegotiate")
elseif(NOT WIN32 AND NOT ANDROID)
	target_link_options(drv_sim_display_plugin PRIVATE
		"LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/plugin_exports.version")
endif()

# Install the plug-in DLL next to the runtime DLL. The plan defines
# $INSTDIR\plugins\ as the home, but the runtime DLL's transitive
# dependencies (cjson.dll, pthread, …) live in $INSTDIR\bin\; the
# loader-side LoadLibraryExW(LOAD_WITH_ALTERED_SEARCH_PATH) + an
# AddDllDirectory("…/bin") call lands with the discovery wiring.
if(WIN32)
	install(
		TARGETS drv_sim_display_plugin
		RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/plugins COMPONENT Runtime
		LIBRARY DESTINATION ${CMAKE_INSTALL_BINDIR}/plugins COMPONENT Runtime
		)
elseif(NOT ANDROID)
	install(
		TARGETS drv_sim_display_plugin
		LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/displayxr/plugins COMPONENT Runtime
		)
endif()

set(ENABLED_DRIVERS ${ENABLED_HEADSET_DRIVERS} ${ENABLED_DRIVERS})
if(ENABLED_DRIVERS)
	list(SORT ENABLED_DRIVERS)
	string(REPLACE ";" " " ENABLED_DRIVERS "${ENABLED_DRIVERS}")
	message(STATUS "Enabled drivers: ${ENABLED_DRIVERS}")
else()
	message(STATUS "No headset drivers enabled (display-only mode)")
endif()
