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

if(XRT_HAVE_LEIA_SR)
	# Set SR_PATH from LEIASR_SDKROOT (defined in root CMakeLists.txt from environment)
	set(SR_PATH "${LEIASR_SDKROOT}")

	if(NOT SR_PATH OR SR_PATH STREQUAL "")
		message(WARNING "LEIASR_SDKROOT environment variable is not set! SR weaver will not be available.")
		message(WARNING "Please set LEIASR_SDKROOT to point to your Simulated Reality SDK installation.")
	else()
		message(STATUS "Leia SR SDK path: ${SR_PATH}")
	endif()

	# Start with empty source list - weavers are added conditionally based on available SR libraries
	set(LEIA_SOURCES)

	# Common sources (device, interface, types, probe) — always present
	list(APPEND LEIA_SOURCES
		leia/leia_device.c
		leia/leia_interface.h
		leia/leia_types.h
		leia/leia_sr_probe.cpp)

	# Check for Vulkan weaver support (SimulatedRealityVulkanBeta.lib)
	set(LEIA_SR_VULKAN_AVAILABLE FALSE)
	if(EXISTS "${SR_PATH}/lib/SimulatedRealityVulkanBeta.lib")
		message(STATUS "Found SimulatedRealityVulkanBeta.lib - enabling Vulkan weaver")
		list(APPEND LEIA_SOURCES
			leia/leia_sr.cpp leia/leia_sr.h
			leia/leia_display_processor.cpp leia/leia_display_processor.h)

		# Compile chroma-key shaders to SPIR-V headers (transparency support)
		spirv_shaders(
			LEIA_SHADER_HEADERS
			SPIRV_VERSION
			1.0
			SOURCES
			leia/shaders/fullscreen_tri.vert
			leia/shaders/ck_fill.frag
			leia/shaders/ck_strip.frag
			leia/shaders/compose_under_bg.frag
			leia/shaders/alpha_gate.frag
		)
		list(APPEND LEIA_SOURCES ${LEIA_SHADER_HEADERS})
		set(LEIA_SR_VULKAN_AVAILABLE TRUE)
	else()
		message(STATUS "SimulatedRealityVulkanBeta.lib not found - Vulkan weaver disabled")
	endif()

	# Check for D3D11 weaver support on Windows
	set(LEIA_SR_D3D11_AVAILABLE FALSE)
	if(WIN32 AND XRT_HAVE_D3D11)
		if(EXISTS "${SR_PATH}/lib/SimulatedRealityDirectX.lib")
			message(STATUS "Found SimulatedRealityDirectX.lib - enabling D3D11 weaver")
			list(APPEND LEIA_SOURCES
				leia/leia_sr_d3d11.cpp leia/leia_sr_d3d11.h
				leia/leia_display_processor_d3d11.cpp leia/leia_display_processor_d3d11.h)
			set(LEIA_SR_D3D11_AVAILABLE TRUE)
		else()
			message(STATUS "SimulatedRealityDirectX.lib not found - D3D11 weaver disabled")
		endif()
	endif()

	# Check for D3D12 weaver support on Windows
	set(LEIA_SR_D3D12_AVAILABLE FALSE)
	if(WIN32 AND XRT_HAVE_D3D12)
		if(EXISTS "${SR_PATH}/lib/SimulatedRealityDirectX.lib")
			message(STATUS "Found SimulatedRealityDirectX.lib - enabling D3D12 weaver")
			list(APPEND LEIA_SOURCES
				leia/leia_sr_d3d12.cpp leia/leia_sr_d3d12.h
				leia/leia_display_processor_d3d12.cpp leia/leia_display_processor_d3d12.h)
			set(LEIA_SR_D3D12_AVAILABLE TRUE)
		else()
			message(STATUS "SimulatedRealityDirectX.lib not found - D3D12 weaver disabled")
		endif()
	endif()

	# Check for GL weaver support on Windows
	set(LEIA_SR_GL_AVAILABLE FALSE)
	if(WIN32)
		if(EXISTS "${SR_PATH}/lib/SimulatedRealityOpenGL.lib")
			message(STATUS "Found SimulatedRealityOpenGL.lib - enabling GL weaver")
			list(APPEND LEIA_SOURCES
				leia/leia_sr_gl.cpp leia/leia_sr_gl.h
				leia/leia_display_processor_gl.cpp leia/leia_display_processor_gl.h)
			set(LEIA_SR_GL_AVAILABLE TRUE)
		else()
			message(STATUS "SimulatedRealityOpenGL.lib not found - GL weaver disabled")
		endif()
	endif()

	# Only create the driver if at least one weaver is available
	if(LEIA_SOURCES)
		add_library(drv_leia STATIC ${LEIA_SOURCES})
		target_include_directories(drv_leia PRIVATE
			${SR_PATH}/include
			${SR_PATH}/third_party/OpenCV/include)

		# Core SR libraries (always needed)
		# SimulatedRealityFaceTrackers.lib added for the EyeTracker /
		# EyePairStream listener subscription used by the DP eye-pos
		# snapshot cache (#248 Tier 2 — eliminates per-frame SDK polling).
		target_link_libraries(drv_leia PRIVATE
			xrt-interfaces aux_util aux_os aux_math
			${SR_PATH}/lib/SimulatedRealityCore.lib
			${SR_PATH}/lib/SimulatedRealityDisplays.lib
			${SR_PATH}/lib/SimulatedRealityFaceTrackers.lib
			${SR_PATH}/third_party/OpenCV/lib/x64/opencv_world343.lib
			PUBLIC drv_includes)

		# Vulkan weaver library (conditional)
		if(LEIA_SR_VULKAN_AVAILABLE)
			target_link_libraries(drv_leia PRIVATE
				${SR_PATH}/lib/SimulatedRealityVulkanBeta.lib)
			target_compile_definitions(drv_leia PUBLIC XRT_HAVE_LEIA_SR_VULKAN)

			# Generated SPIR-V chroma-key headers live in the build tree.
			target_include_directories(drv_leia PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

			# Install the bundled VK weaver runtime DLL alongside the runtime
			# binaries. Single source of truth for every VK app: the runtime
			# installer adds INSTDIR to system PATH so installed apps load it
			# transparently; dev `_package/bin/` is added to PATH by the
			# generated `run_*.bat` scripts. Apps no longer bundle their own
			# copy. When the SR SDK starts shipping this DLL natively, delete
			# this install() rule.
			if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/test_apps/common/SimulatedRealityVulkanBeta.dll")
				install(FILES "${CMAKE_SOURCE_DIR}/test_apps/common/SimulatedRealityVulkanBeta.dll"
						DESTINATION "${CMAKE_INSTALL_BINDIR}")
			endif()
		endif()

		# D3D11 weaver library (conditional)
		if(LEIA_SR_D3D11_AVAILABLE)
			target_link_libraries(drv_leia PRIVATE
				${SR_PATH}/lib/SimulatedRealityDirectX.lib
				d3d11
				d3dcompiler
				dxgi)
			target_compile_definitions(drv_leia PUBLIC XRT_HAVE_LEIA_SR_D3D11)
		endif()

		# D3D12 weaver library (conditional)
		if(LEIA_SR_D3D12_AVAILABLE)
			# SimulatedRealityDirectX.lib covers both D3D11 and D3D12 weavers
			if(NOT LEIA_SR_D3D11_AVAILABLE)
				target_link_libraries(drv_leia PRIVATE
					${SR_PATH}/lib/SimulatedRealityDirectX.lib)
			endif()
			target_link_libraries(drv_leia PRIVATE d3d12 d3dcompiler)
			target_compile_definitions(drv_leia PUBLIC XRT_HAVE_LEIA_SR_D3D12)
		endif()

		# Windows Graphics Capture (WGC) background capture, used by D3D11 / D3D12
		# Leia DPs to compose desktop pixels under transparent app tiles in place
		# of the chroma-key trick. Win-only; consumes runtimeobject.lib for the
		# WinRT activation factories.
		if(WIN32 AND (LEIA_SR_D3D11_AVAILABLE OR LEIA_SR_D3D12_AVAILABLE))
			target_sources(drv_leia PRIVATE
				leia/leia_bg_capture_win.cpp
				leia/leia_bg_capture_win.h)
			target_link_libraries(drv_leia PRIVATE runtimeobject dxguid)
		endif()

		# GL weaver library (conditional)
		if(LEIA_SR_GL_AVAILABLE)
			target_link_libraries(drv_leia PRIVATE
				${SR_PATH}/lib/SimulatedRealityOpenGL.lib
				opengl32
				aux_ogl)
			target_compile_definitions(drv_leia PUBLIC XRT_HAVE_LEIA_SR_GL)
		endif()

		# Eye tracking is handled by the weaver's getPredictedEyePositions() - no separate library needed

		# CNSDK support (Android interlacing) — added to drv_leia when both SDKs available
		if(XRT_HAVE_CNSDK)
			target_sources(drv_leia PRIVATE leia/leia_cnsdk.cpp leia/leia_cnsdk.h)
			target_link_libraries(drv_leia PRIVATE CNSDK::leiaSDK)
			target_compile_definitions(drv_leia PUBLIC XRT_HAVE_CNSDK)
		endif()

		# Export availability for other CMake components (targets/common needs this)
		set(LEIA_SR_VULKAN_AVAILABLE ${LEIA_SR_VULKAN_AVAILABLE} CACHE INTERNAL "LeiaSR Vulkan weaver available")
		set(LEIA_SR_D3D11_AVAILABLE ${LEIA_SR_D3D11_AVAILABLE} CACHE INTERNAL "LeiaSR D3D11 weaver available")
		set(LEIA_SR_D3D12_AVAILABLE ${LEIA_SR_D3D12_AVAILABLE} CACHE INTERNAL "LeiaSR D3D12 weaver available")
		set(LEIA_SR_GL_AVAILABLE ${LEIA_SR_GL_AVAILABLE} CACHE INTERNAL "LeiaSR GL weaver available")

		list(APPEND ENABLED_DRIVERS drv_leia)
	else()
		message(WARNING "XRT_HAVE_LEIA_SR is set but no SR weaver libraries found - disabling Leia SR support")
		set(XRT_HAVE_LEIA_SR OFF PARENT_SCOPE)
	endif()
endif(XRT_HAVE_LEIA_SR)

# EDID-based Leia display identification (no SR SDK required, Windows only)
if(WIN32 AND TARGET drv_leia)
	target_sources(drv_leia PRIVATE leia/leia_edid_probe.c)
endif()

####
# DisplayXR-LeiaSR plug-in DLL.
#
# Mirror of drv_sim_display_plugin (Step 4a) — rebuilds the same drv_leia
# sources with XRT_USING_RUNTIME_DLL so the aux headers' XRT_API_FUNC
# expansions become __declspec(dllimport). All u_log / u_var / u_metrics
# calls from inside the plug-in resolve to DisplayXRClient.dll's exported
# surface, giving one copy of g_log_sink_func / gTracker / etc. per
# process. The SR SDK static imports (SimulatedRealityCore.dll etc.) move
# to the plug-in DLL's link line — the headline goal of ADR-019.
#
# The plug-in entry point sources only leia_plugin.c plus the existing
# LEIA_SOURCES (compiled with the same shader includes etc.). At runtime
# the plug-in DLL is loaded via the registry-driven loader (Step 5) when
# its leia-sr subkey wins the ProbeOrder=50 race.
#
# Static drv_leia stays in the tree for backward compatibility; ADR-019
# Step 6c will gate it behind XRT_PLUGIN_BUILD_INPROC_FALLBACK.
#
# Issue #256.
if(XRT_HAVE_LEIA_SR AND TARGET drv_leia)
	# Mirror the conditional WGC bg-capture sources the static drv_leia
	# target gets via target_sources(... PRIVATE) further down — the
	# SHARED target needs all symbols resolved at the .dll link step, so
	# they must be in the initial source list.
	set(LEIA_PLUGIN_SOURCES ${LEIA_SOURCES} leia/leia_edid_probe.c leia/leia_plugin.c)
	if(WIN32 AND (LEIA_SR_D3D11_AVAILABLE OR LEIA_SR_D3D12_AVAILABLE))
		list(APPEND LEIA_PLUGIN_SOURCES leia/leia_bg_capture_win.cpp leia/leia_bg_capture_win.h)
	endif()

	add_library(drv_leia_plugin SHARED ${LEIA_PLUGIN_SOURCES})
	target_compile_definitions(drv_leia_plugin PRIVATE XRT_USING_RUNTIME_DLL)
	target_include_directories(drv_leia_plugin PRIVATE
		${SR_PATH}/include
		${SR_PATH}/third_party/OpenCV/include
		${CMAKE_CURRENT_SOURCE_DIR}/leia
		${CMAKE_CURRENT_SOURCE_DIR}/../../auxiliary
		${CMAKE_CURRENT_BINARY_DIR}
		)
	target_link_libraries(
		drv_leia_plugin
		PRIVATE
			# DisplayXRClient.lib — import lib for the runtime DLL's
			# aux export surface (ADR-019). Generator expression to
			# pick up only the .lib, not the runtime's PUBLIC INTERFACE
			# link deps (which would drag in every compositor + state
			# tracker).
			$<TARGET_LINKER_FILE:${RUNTIME_TARGET}>
			# Non-exported aux helpers — linker pulls in only the .obj
			# files satisfying unresolved symbols.
			xrt-interfaces
			aux_util
			aux_os
			aux_math
			# aux_vk satisfies the vk_create_image_simple /
			# vk_create_view / vkAllocate* calls from leia_sr.cpp +
			# leia_display_processor.cpp. The static drv_leia target
			# doesn't need this because its unresolved Vulkan symbols
			# are deferred to the final runtime DLL link; SHARED has
			# no such deferral.
			aux_vk
			drv_includes
			# SR SDK static imports — same set the in-tree drv_leia
			# target pulls in. After Step 6c removes the in-tree
			# static-link path, this is the only place these libs
			# appear on a link line.
			${SR_PATH}/lib/SimulatedRealityCore.lib
			${SR_PATH}/lib/SimulatedRealityDisplays.lib
			${SR_PATH}/lib/SimulatedRealityFaceTrackers.lib
			${SR_PATH}/third_party/OpenCV/lib/x64/opencv_world343.lib
		)
	add_dependencies(drv_leia_plugin ${RUNTIME_TARGET})

	if(LEIA_SR_VULKAN_AVAILABLE)
		target_link_libraries(drv_leia_plugin PRIVATE ${SR_PATH}/lib/SimulatedRealityVulkanBeta.lib)
		target_compile_definitions(drv_leia_plugin PRIVATE XRT_HAVE_LEIA_SR_VULKAN)
		# Mirror the runtime DLL's DELAYLOAD shape: SR Vulkan beta DLL
		# isn't always present on every machine; the in-process Leia
		# probe never hits the Vulkan weaver code on D3D11/D3D12 paths,
		# so delay-loading keeps the plug-in loadable.
		target_link_options(drv_leia_plugin PRIVATE
			"/DELAYLOAD:SimulatedRealityVulkanBeta.dll"
			"/DELAYLOAD:vulkan-1.dll"
		)
		target_link_libraries(drv_leia_plugin PRIVATE delayimp)
	endif()

	if(LEIA_SR_D3D11_AVAILABLE)
		target_link_libraries(drv_leia_plugin PRIVATE
			${SR_PATH}/lib/SimulatedRealityDirectX.lib
			d3d11 d3dcompiler dxgi)
		target_compile_definitions(drv_leia_plugin PRIVATE XRT_HAVE_LEIA_SR_D3D11)
	endif()

	if(LEIA_SR_D3D12_AVAILABLE)
		if(NOT LEIA_SR_D3D11_AVAILABLE)
			target_link_libraries(drv_leia_plugin PRIVATE
				${SR_PATH}/lib/SimulatedRealityDirectX.lib)
		endif()
		target_link_libraries(drv_leia_plugin PRIVATE d3d12 d3dcompiler)
		target_compile_definitions(drv_leia_plugin PRIVATE XRT_HAVE_LEIA_SR_D3D12)
	endif()

	# Windows Graphics Capture for D3D11/D3D12 transparency compose-under-bg.
	if(WIN32 AND (LEIA_SR_D3D11_AVAILABLE OR LEIA_SR_D3D12_AVAILABLE))
		target_link_libraries(drv_leia_plugin PRIVATE runtimeobject dxguid)
	endif()

	if(LEIA_SR_GL_AVAILABLE)
		target_link_libraries(drv_leia_plugin PRIVATE
			${SR_PATH}/lib/SimulatedRealityOpenGL.lib
			opengl32
			aux_ogl)
		target_compile_definitions(drv_leia_plugin PRIVATE XRT_HAVE_LEIA_SR_GL)
	endif()

	# Same one-symbol export discipline as drv_sim_display_plugin.
	set_target_properties(drv_leia_plugin PROPERTIES
		OUTPUT_NAME "DisplayXR-LeiaSR"
		PREFIX ""
		C_VISIBILITY_PRESET hidden
		CXX_VISIBILITY_PRESET hidden
		VISIBILITY_INLINES_HIDDEN ON
		)

	if(WIN32)
		install(
			TARGETS drv_leia_plugin
			RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/plugins COMPONENT Runtime
			LIBRARY DESTINATION ${CMAKE_INSTALL_BINDIR}/plugins COMPONENT Runtime
			)
	endif()
endif()

# CNSDK-only build (Android) — create drv_leia if SR SDK is not available
if(XRT_HAVE_CNSDK AND NOT TARGET drv_leia)
	add_library(drv_leia STATIC
		leia/leia_device.c
		leia/leia_interface.h
		leia/leia_types.h
		leia/leia_sr_probe.cpp
		leia/leia_cnsdk.cpp
		leia/leia_cnsdk.h)
	target_link_libraries(drv_leia PRIVATE
		xrt-interfaces aux_util aux_os aux_math
		CNSDK::leiaSDK
		PUBLIC drv_includes)
	target_compile_definitions(drv_leia PUBLIC XRT_HAVE_CNSDK)
	list(APPEND ENABLED_DRIVERS drv_leia)
endif()

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
# On Linux / macOS we apply visibility=hidden defaults below so any
# accidentally-non-static symbols stay private.
set_target_properties(drv_sim_display_plugin PROPERTIES
	OUTPUT_NAME "DisplayXR-SimDisplay"
	PREFIX ""
	C_VISIBILITY_PRESET hidden
	CXX_VISIBILITY_PRESET hidden
	VISIBILITY_INLINES_HIDDEN ON
	)

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
