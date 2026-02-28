# Copyright 2019-2023, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0

####
# Lists
#

add_library(
	target_lists STATIC target_builder_interface.h target_builder_legacy.c
			    target_builder_qwerty_input.c target_builder_qwerty_input.h
			    target_lists.c target_lists.h
	)
target_link_libraries(target_lists PRIVATE xrt-interfaces aux_util aux_util_sink drv_includes)
target_include_directories(target_lists PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

###
# Builders
#

if(XRT_BUILD_DRIVER_REMOTE)
	target_sources(target_lists PRIVATE target_builder_remote.c)
endif()

if(XRT_BUILD_DRIVER_PSVR OR XRT_BUILD_DRIVER_PSMV)
	target_sources(target_lists PRIVATE target_builder_rgb_tracking.c)
endif()

if(XRT_BUILD_DRIVER_RIFT_S)
	target_sources(target_lists PRIVATE target_builder_rift_s.c)
endif()

if(XRT_BUILD_DRIVER_SURVIVE OR XRT_BUILD_DRIVER_VIVE)
	target_sources(target_lists PRIVATE target_builder_lighthouse.c)
endif()

if(XRT_BUILD_DRIVER_STEAMVR_LIGHTHOUSE)
	target_sources(target_lists PRIVATE target_builder_steamvr.c)
endif()

if(XRT_BUILD_DRIVER_SIMULATED)
	target_sources(target_lists PRIVATE target_builder_simulated.c)
endif()

if(XRT_BUILD_DRIVER_SIMULAVR)
	target_sources(target_lists PRIVATE target_builder_simulavr.c)
endif()

if(XRT_BUILD_DRIVER_NS)
	target_sources(target_lists PRIVATE target_builder_north_star.c)
	target_link_libraries(target_lists PRIVATE drv_ns)
endif()

if(XRT_BUILD_DRIVER_WMR)
	target_sources(target_lists PRIVATE target_builder_wmr.c)
	target_link_libraries(target_lists PRIVATE drv_wmr)
endif()

if(XRT_BUILD_DRIVER_QWERTY)
	target_sources(target_lists PRIVATE target_builder_qwerty.c)
	target_link_libraries(target_lists PRIVATE drv_qwerty)
endif()

if(XRT_BUILD_DRIVER_XREAL_AIR)
	target_sources(target_lists PRIVATE target_builder_xreal_air.c)
	target_link_libraries(target_lists PRIVATE drv_xreal_air)
endif()

# Leia 3D display builder (only when SR SDK is available)
if(XRT_HAVE_LEIA_SR)
	target_sources(target_lists PRIVATE target_builder_leia.c)
	target_link_libraries(target_lists PRIVATE drv_leia)
endif()

# Sim display builder (always available, guards itself via env var)
target_sources(target_lists PRIVATE target_builder_sim_display.c)
target_link_libraries(target_lists PRIVATE drv_sim_display)
if(XRT_BUILD_DRIVER_QWERTY)
	# sim_display builder uses qwerty_device.h for pose delegation config
	target_link_libraries(target_lists PRIVATE drv_qwerty drv_qwerty_includes)
endif()

###
# Drivers
#

if(XRT_BUILD_DRIVER_ARDUINO)
	target_link_libraries(target_lists PRIVATE drv_arduino)
endif()

if(XRT_BUILD_DRIVER_BLUBUR_S1)
	target_link_libraries(target_lists PRIVATE drv_blubur_s1)
endif()

target_link_libraries(target_lists PRIVATE drv_cemu)

if(XRT_BUILD_DRIVER_DAYDREAM)
	target_link_libraries(target_lists PRIVATE drv_daydream)
endif()

if(XRT_BUILD_DRIVER_DEPTHAI)
	target_link_libraries(target_lists PRIVATE drv_depthai)
endif()

if(XRT_BUILD_DRIVER_SIMULATED)
	target_link_libraries(target_lists PRIVATE drv_simulated)
endif()

if(XRT_BUILD_DRIVER_HDK)
	target_link_libraries(target_lists PRIVATE drv_hdk)
endif()

if(XRT_BUILD_DRIVER_HYDRA)
	target_link_libraries(target_lists PRIVATE drv_hydra)
endif()

if(XRT_BUILD_DRIVER_NS)
	target_link_libraries(target_lists PRIVATE drv_ns)
endif()

if(XRT_BUILD_DRIVER_TWRAP)
	target_link_libraries(target_lists PRIVATE drv_twrap)
endif()

if(XRT_BUILD_DRIVER_ULV2)
	target_link_libraries(target_lists PRIVATE drv_ulv2)
endif()

if(XRT_BUILD_DRIVER_ULV5)
	target_link_libraries(target_lists PRIVATE drv_ulv5)
endif()

if(XRT_BUILD_DRIVER_OHMD)
	target_link_libraries(target_lists PRIVATE drv_ohmd)
endif()

if(XRT_BUILD_DRIVER_OPENGLOVES)
	target_link_libraries(target_lists PRIVATE drv_opengloves)
endif()

if(XRT_BUILD_DRIVER_HANDTRACKING)
	target_link_libraries(target_lists PRIVATE drv_ht)
endif()

if(XRT_BUILD_DRIVER_PSMV)
	target_link_libraries(target_lists PRIVATE drv_psmv)
endif()

if(XRT_BUILD_DRIVER_PSSENSE)
	target_link_libraries(target_lists PRIVATE drv_pssense)
endif()

if(XRT_BUILD_DRIVER_PSVR)
	target_link_libraries(target_lists PRIVATE drv_psvr)
endif()

if(XRT_BUILD_DRIVER_REALSENSE)
	target_link_libraries(target_lists PRIVATE drv_realsense)
endif()

if(XRT_BUILD_DRIVER_REMOTE)
	target_link_libraries(target_lists PRIVATE drv_remote)
endif()

if(XRT_BUILD_DRIVER_RIFT)
	target_link_libraries(target_lists PRIVATE drv_rift)
endif()

if(XRT_BUILD_DRIVER_RIFT_S)
	target_link_libraries(target_lists PRIVATE drv_rift_s)
endif()

if(XRT_BUILD_DRIVER_ROKID)
	target_link_libraries(target_lists PRIVATE drv_rokid)
endif()

if(XRT_HAVE_V4L2)
	target_link_libraries(target_lists PRIVATE drv_v4l2)
endif()

if(XRT_BUILD_DRIVER_VF)
	target_link_libraries(target_lists PRIVATE drv_vf)
endif()

if(XRT_BUILD_DRIVER_VIVE)
	target_link_libraries(target_lists PRIVATE drv_vive)
endif()

if(XRT_BUILD_DRIVER_SURVIVE)
	target_link_libraries(target_lists PRIVATE drv_survive)
endif()

if(XRT_BUILD_DRIVER_STEAMVR_LIGHTHOUSE)
	target_link_libraries(target_lists PRIVATE drv_steamvr_lh)
	target_sources(target_lists PRIVATE target_builder_steamvr.c)
endif()

if(XRT_BUILD_DRIVER_ANDROID)
	target_link_libraries(target_lists PRIVATE drv_android)
endif()

if(XRT_BUILD_DRIVER_ILLIXR)
	target_link_libraries(target_lists PRIVATE drv_illixr)
endif()

target_link_libraries(target_lists PRIVATE drv_multi)

if(XRT_BUILD_DRIVER_QWERTY)
	target_link_libraries(target_lists PRIVATE drv_qwerty)
endif()

if(XRT_BUILD_DRIVER_WMR)
	target_link_libraries(target_lists PRIVATE drv_wmr)
endif()

if(XRT_BUILD_DRIVER_XREAL_AIR)
	target_link_libraries(target_lists PRIVATE drv_xreal_air)
endif()

if(XRT_BUILD_DRIVER_EUROC)
	target_link_libraries(target_lists PRIVATE drv_euroc)
endif()

if(XRT_BUILD_DRIVER_SIMULAVR)
	target_link_libraries(target_lists PRIVATE drv_svr)
endif()

####
# Instance
#

if(XRT_MODULE_COMPOSITOR_MAIN OR XRT_MODULE_COMPOSITOR_NULL)
	add_library(target_instance STATIC target_instance.c)
	target_link_libraries(
		target_instance
		PRIVATE
			xrt-interfaces
			aux_util
			st_prober
			target_lists
			drv_includes
		)
	target_include_directories(target_instance PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

	# When hybrid mode is enabled on Windows, the service (which uses target_instance)
	# should ONLY use D3D11 compositor to avoid Vulkan dependencies.
	# On macOS, both the service and native path use comp_main (Vulkan via MoltenVK).
	if(XRT_FEATURE_HYBRID_MODE AND WIN32)
		# D3D11-only mode for service - no Vulkan compositor linked
		target_link_libraries(target_instance PRIVATE comp_d3d11_service)
		target_include_directories(target_instance PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../compositor)
		# XRT_USE_D3D11_SERVICE_COMPOSITOR: enables D3D11 compositor
		# XRT_D3D11_SERVICE_ONLY: disables Vulkan fallback (service doesn't need it)
		target_compile_definitions(target_instance PRIVATE XRT_USE_D3D11_SERVICE_COMPOSITOR XRT_D3D11_SERVICE_ONLY)
		# Note: NOT linking comp_main here - service uses D3D11 only
	else()
		# Standard mode (and macOS hybrid service): link Vulkan compositor
		if(XRT_MODULE_COMPOSITOR_MAIN)
			target_link_libraries(target_instance PRIVATE comp_main)
		endif()
	endif()

	if(XRT_MODULE_COMPOSITOR_NULL)
		target_link_libraries(target_instance PRIVATE comp_null)
	endif()

	# Link SR driver for dimension query when available
	if(XRT_HAVE_LEIA_SR)
		target_link_libraries(target_instance PRIVATE drv_leia)
		target_include_directories(target_instance PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers)
	endif()

	# Hybrid mode: create a separate library that exports native_instance_create
	# instead of xrt_instance_create to avoid symbol conflict with IPC client
	if(XRT_FEATURE_HYBRID_MODE)
		add_library(target_instance_hybrid STATIC target_instance.c)
		target_link_libraries(
			target_instance_hybrid
			PRIVATE
				xrt-interfaces
				aux_util
				st_prober
				target_lists
				drv_includes
			)
		target_include_directories(target_instance_hybrid PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
		# Add compositor directory for main/comp_main_interface.h include
		# (needed because XRT_MODULE_COMPOSITOR_MAIN is defined globally)
		target_include_directories(target_instance_hybrid PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../compositor)

		if(WIN32)
			# Windows: XRT_HYBRID_USE_NULL_COMPOSITOR forces null compositor during
			# instance creation (no window) — the D3D11 native compositor handles
			# rendering during session creation
			target_compile_definitions(target_instance_hybrid PRIVATE
				XRT_FEATURE_HYBRID_MODE
				XRT_HYBRID_USE_NULL_COMPOSITOR
			)
			if(XRT_MODULE_COMPOSITOR_NULL)
				target_link_libraries(target_instance_hybrid PRIVATE comp_null)
			endif()
		else()
			# macOS: use comp_main (Vulkan via MoltenVK) for the native path
			# Same Vulkan compositor as the service, no null compositor override
			target_compile_definitions(target_instance_hybrid PRIVATE
				XRT_FEATURE_HYBRID_MODE
			)
			if(XRT_MODULE_COMPOSITOR_MAIN)
				target_link_libraries(target_instance_hybrid PRIVATE comp_main)
			endif()
			# Link comp_null to satisfy symbol references from XRT_MODULE_COMPOSITOR_NULL
			# code paths (compiled but not executed since USE_NULL_DEFAULT=false)
			if(XRT_MODULE_COMPOSITOR_NULL)
				target_link_libraries(target_instance_hybrid PRIVATE comp_null)
			endif()
		endif()

		# Link SR driver for dimension query when available (hybrid mode)
		if(XRT_HAVE_LEIA_SR)
			target_link_libraries(target_instance_hybrid PRIVATE drv_leia)
			target_include_directories(target_instance_hybrid PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../drivers)
		endif()

	endif()
endif()

####
# Instance without Compositor
#

add_library(target_instance_no_comp STATIC target_instance_no_comp.c)
target_link_libraries(
	target_instance_no_comp
	PRIVATE
		xrt-interfaces
		aux_util
		st_prober
		target_lists
		drv_includes
	)
target_include_directories(target_instance_no_comp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

####
# Platform

if(ANDROID)
	target_link_libraries(target_instance PRIVATE aux_android)

	target_link_libraries(target_instance_no_comp PRIVATE aux_android)

	if(XRT_FEATURE_HYBRID_MODE)
		target_link_libraries(target_instance_hybrid PRIVATE aux_android)
	endif()
endif()
