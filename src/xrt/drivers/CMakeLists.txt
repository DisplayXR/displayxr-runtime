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
		target_link_libraries(drv_leia PRIVATE
			xrt-interfaces aux_util aux_os aux_math
			${SR_PATH}/lib/SimulatedRealityCore.lib
			${SR_PATH}/lib/SimulatedRealityDisplays.lib
			${SR_PATH}/third_party/OpenCV/lib/x64/opencv_world343.lib
			PUBLIC drv_includes)

		# Vulkan weaver library (conditional)
		if(LEIA_SR_VULKAN_AVAILABLE)
			target_link_libraries(drv_leia PRIVATE
				${SR_PATH}/lib/SimulatedRealityVulkanBeta.lib)
			target_compile_definitions(drv_leia PUBLIC XRT_HAVE_LEIA_SR_VULKAN)
		endif()

		# D3D11 weaver library (conditional)
		if(LEIA_SR_D3D11_AVAILABLE)
			target_link_libraries(drv_leia PRIVATE
				${SR_PATH}/lib/SimulatedRealityDirectX.lib
				d3d11
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
			target_link_libraries(drv_leia PRIVATE d3d12)
			target_compile_definitions(drv_leia PUBLIC XRT_HAVE_LEIA_SR_D3D12)
		endif()

		# GL weaver library (conditional)
		if(LEIA_SR_GL_AVAILABLE)
			target_link_libraries(drv_leia PRIVATE
				${SR_PATH}/lib/SimulatedRealityOpenGL.lib
				opengl32)
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

# Simulation 3D display driver (always available, enabled by SIM_DISPLAY_ENABLE=1)
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

set(ENABLED_DRIVERS ${ENABLED_HEADSET_DRIVERS} ${ENABLED_DRIVERS})
if(ENABLED_DRIVERS)
	list(SORT ENABLED_DRIVERS)
	string(REPLACE ";" " " ENABLED_DRIVERS "${ENABLED_DRIVERS}")
	message(STATUS "Enabled drivers: ${ENABLED_DRIVERS}")
else()
	message(STATUS "No headset drivers enabled (display-only mode)")
endif()
