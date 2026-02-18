# Copyright 2019-2021, Collabora, Ltd.
#
# SPDX-License-Identifier: BSL-1.0

set(ENABLED_HEADSET_DRIVERS)
set(ENABLED_DRIVERS)

add_library(drv_includes INTERFACE)
target_include_directories(drv_includes INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

if(XRT_BUILD_DRIVER_ARDUINO)
	add_library(
		drv_arduino STATIC arduino/arduino_device.c arduino/arduino_interface.h
				   arduino/arduino_prober.c
		)
	target_link_libraries(drv_arduino PRIVATE xrt-interfaces aux_util aux_os aux_os_ble)
	list(APPEND ENABLED_DRIVERS arduino)
endif()

add_library(drv_cemu STATIC ht_ctrl_emu/ht_ctrl_emu.cpp ht_ctrl_emu/ht_ctrl_emu_interface.h)
target_link_libraries(drv_cemu PRIVATE xrt-interfaces aux_generated_bindings_helpers aux_util)
list(APPEND ENABLED_HEADSET_DRIVERS drv_cemu)

if(XRT_BUILD_DRIVER_BLUBUR_S1)
	add_library(
		drv_blubur_s1 STATIC blubur_s1/blubur_s1_hmd.c blubur_s1/blubur_s1_interface.h
				     blubur_s1/blubur_s1_prober.c
		)
	target_link_libraries(drv_blubur_s1 PRIVATE xrt-interfaces aux_util aux_os)
	list(APPEND ENABLED_DRIVERS blubur_s1)
endif()

if(XRT_BUILD_DRIVER_DAYDREAM)
	add_library(
		drv_daydream STATIC daydream/daydream_device.c daydream/daydream_device.h
				    daydream/daydream_interface.h daydream/daydream_prober.c
		)
	target_link_libraries(drv_daydream PRIVATE xrt-interfaces aux_util aux_os aux_os_ble)
	list(APPEND ENABLED_DRIVERS daydream)
endif()

if(XRT_BUILD_DRIVER_DEPTHAI)
	add_library(drv_depthai STATIC depthai/depthai_driver.cpp depthai/depthai_interface.h)
	target_link_libraries(
		drv_depthai
		PRIVATE
			xrt-interfaces
			aux_os
			aux_util
			aux_tracking
			${OpenCV_LIBRARIES}
			depthai::core
			XLink
		)
	target_include_directories(drv_depthai PRIVATE ${OpenCV_INCLUDE_DIRS})
	list(APPEND ENABLED_DRIVERS depthai)
endif()

if(XRT_BUILD_DRIVER_SIMULATED)
	add_library(
		drv_simulated STATIC simulated/simulated_controller.c simulated/simulated_hmd.c
				     simulated/simulated_interface.h simulated/simulated_prober.c
		)
	target_link_libraries(drv_simulated PRIVATE xrt-interfaces aux_util)
	list(APPEND ENABLED_HEADSET_DRIVERS simulated)
endif()

if(XRT_BUILD_DRIVER_TWRAP)
	add_library(drv_twrap STATIC twrap/twrap_slam.c twrap/twrap_interface.h)
	target_link_libraries(drv_twrap PRIVATE xrt-interfaces aux_util)
endif()

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

if(XRT_BUILD_DRIVER_HDK)
	set(HDK_SOURCE_FILES)

	add_library(
		drv_hdk STATIC hdk/hdk_device.cpp hdk/hdk_device.h hdk/hdk_interface.h
			       hdk/hdk_prober.c
		)
	target_link_libraries(drv_hdk PRIVATE xrt-interfaces aux_math)
	list(APPEND ENABLED_HEADSET_DRIVERS hdk)
endif()

if(XRT_BUILD_DRIVER_HYDRA)
	set(HYDRA_SOURCE_FILES)

	add_library(drv_hydra STATIC hydra/hydra_driver.c hydra/hydra_interface.h)
	target_link_libraries(drv_hydra PRIVATE xrt-interfaces aux_os aux_util)
	list(APPEND ENABLED_DRIVERS hydra)
endif()

if(XRT_BUILD_DRIVER_NS)
	add_library(
		drv_ns STATIC
		north_star/distortion_3d/utility_northstar.h
		north_star/distortion_3d/deformation_northstar.h
		north_star/distortion_3d/deformation_northstar.cpp
		north_star/ns_hmd.h
		north_star/ns_hmd.c
		north_star/ns_interface.h
		)
	target_link_libraries(drv_ns PRIVATE xrt-interfaces aux_math xrt-external-cjson)
	list(APPEND ENABLED_HEADSET_DRIVERS ns)
endif()

if(XRT_BUILD_DRIVER_XREAL_AIR)
	add_library(
		drv_xreal_air STATIC xreal_air/xreal_air_hmd.h xreal_air/xreal_air_hmd.c
				     xreal_air/xreal_air_interface.h xreal_air/xreal_air_packet.c
		)
	target_link_libraries(
		drv_xreal_air PRIVATE xrt-interfaces HIDAPI::hidapi aux_util xrt-external-cjson
		)
	target_include_directories(drv_xreal_air PRIVATE ${HIDAPI_INCLUDE_DIRS})
	list(APPEND ENABLED_HEADSET_DRIVERS xreal_air)
endif()

if(XRT_BUILD_DRIVER_ULV2)
	add_library(drv_ulv2 STATIC ultraleap_v2/ulv2_driver.cpp ultraleap_v2/ulv2_interface.h)
	target_link_libraries(drv_ulv2 PRIVATE xrt-interfaces aux_util aux_math LeapV2::LeapV2)
endif()

if(XRT_BUILD_DRIVER_ULV5)
	add_library(drv_ulv5 STATIC ultraleap_v5/ulv5_driver.cpp ultraleap_v5/ulv5_interface.h)
	target_link_libraries(drv_ulv5 PRIVATE xrt-interfaces aux_util aux_math LeapSDK::LeapC)
endif()

if(XRT_BUILD_DRIVER_OHMD)
	add_library(
		drv_ohmd STATIC ohmd/oh_device.c ohmd/oh_device.h ohmd/oh_interface.h
				ohmd/oh_prober.c
		)
	target_link_libraries(drv_ohmd PRIVATE xrt-interfaces OpenHMD::OpenHMD aux_util aux_math)
	list(APPEND ENABLED_HEADSET_DRIVERS openhmd)
endif()

if(XRT_BUILD_DRIVER_OPENGLOVES)
	add_library(
		drv_opengloves STATIC
		opengloves/opengloves_interface.h
		opengloves/opengloves_device.c
		opengloves/opengloves_prober.c
		opengloves/opengloves_device.h
		opengloves/communication/serial/opengloves_serial.h
		opengloves/communication/serial/opengloves_serial.c
		opengloves/encoding/alpha_encoding.h
		opengloves/encoding/alpha_encoding.cpp
		opengloves/encoding/encoding.h
		opengloves/communication/bluetooth/opengloves_bt_serial.h
		opengloves/communication/bluetooth/opengloves_bt_serial.c
		opengloves/communication/opengloves_communication.h
		opengloves/communication/serial/opengloves_prober_serial.h
		opengloves/communication/serial/opengloves_prober_serial.c
		opengloves/communication/bluetooth/opengloves_prober_bt.h
		opengloves/communication/bluetooth/opengloves_prober_bt.c
		)
	target_link_libraries(drv_opengloves PRIVATE xrt-interfaces aux_util aux_os bluetooth)
	list(APPEND ENABLED_DRIVERS opengloves)

endif()

if(XRT_BUILD_DRIVER_PSMV)
	add_library(drv_psmv STATIC psmv/psmv_driver.c psmv/psmv_interface.h)
	target_link_libraries(
		drv_psmv
		PRIVATE xrt-interfaces aux_util
		PUBLIC aux_os aux_tracking
		)
	list(APPEND ENABLED_DRIVERS psmv)
endif()

if(XRT_BUILD_DRIVER_PSSENSE)
	add_library(drv_pssense STATIC pssense/pssense_driver.c pssense/pssense_interface.h)
	target_link_libraries(drv_pssense PRIVATE aux_os aux_util)
	list(APPEND ENABLED_DRIVERS pssense)
endif()

if(XRT_BUILD_DRIVER_PSVR)
	add_library(
		drv_psvr STATIC
		psvr/psvr_device.c
		psvr/psvr_device.h
		psvr/psvr_interface.h
		psvr/psvr_packet.c
		psvr/psvr_prober.c
		)
	target_link_libraries(drv_psvr PRIVATE xrt-interfaces HIDAPI::hidapi aux_util)
	target_include_directories(drv_psvr PRIVATE ${HIDAPI_INCLUDE_DIRS})
	list(APPEND ENABLED_HEADSET_DRIVERS psvr)
endif()

if(XRT_BUILD_DRIVER_REALSENSE)
	add_library(
		drv_realsense STATIC
		realsense/rs_ddev.c
		realsense/rs_hdev.c
		realsense/rs_prober.c
		realsense/rs_driver.h
		realsense/rs_interface.h
		)
	target_link_libraries(drv_realsense PRIVATE xrt-interfaces realsense2::realsense2 aux_util)
	list(APPEND ENABLED_HEADSET_DRIVERS rs)
endif()

if(XRT_BUILD_DRIVER_REMOTE)
	add_library(
		drv_remote STATIC
		remote/r_device.c
		remote/r_hmd.c
		remote/r_hub.c
		remote/r_interface.h
		remote/r_internal.h
		)
	target_link_libraries(drv_remote PRIVATE xrt-interfaces aux_util aux_vive)
	if(WIN32)
		target_link_libraries(drv_remote PRIVATE ws2_32)
	endif()
	list(APPEND ENABLED_HEADSET_DRIVERS remote)
endif()

if(XRT_BUILD_DRIVER_ROKID)
	add_library(drv_rokid STATIC rokid/rokid_hmd.c rokid/rokid_interface.h)
	target_link_libraries(
		drv_rokid
		PRIVATE
			xrt-interfaces
			aux_util
			aux_os
			aux_math
			${LIBUSB1_LIBRARIES}
		)
	target_include_directories(drv_rokid PUBLIC ${LIBUSB1_INCLUDE_DIRS})
	list(APPEND ENABLED_HEADSET_DRIVERS rokid)
endif()

if(XRT_BUILD_DRIVER_RIFT)
	add_library(drv_rift STATIC rift/rift_hmd.c rift/rift_distortion.c rift/rift_prober.c)
	target_include_directories(drv_rift SYSTEM PRIVATE)
	target_link_libraries(
		drv_rift
		PRIVATE
			xrt-interfaces
			aux_util
			aux_util_sink
			aux_math
			xrt-external-cjson
			xrt-external-tinyceres
		)
	list(APPEND ENABLED_HEADSET_DRIVERS rift)
endif()

if(XRT_BUILD_DRIVER_RIFT_S)
	add_library(
		drv_rift_s STATIC
		rift_s/rift_s_interface.h
		rift_s/rift_s_camera.c
		rift_s/rift_s_camera.h
		rift_s/rift_s_controller.c
		rift_s/rift_s_controller.h
		rift_s/rift_s_firmware.c
		rift_s/rift_s_firmware.h
		rift_s/rift_s_hmd.c
		rift_s/rift_s_hmd.h
		rift_s/rift_s_protocol.c
		rift_s/rift_s_protocol.h
		rift_s/rift_s_radio.c
		rift_s/rift_s_radio.h
		rift_s/rift_s_tracker.c
		rift_s/rift_s_tracker.h
		rift_s/rift_s_util.cpp
		rift_s/rift_s_util.h
		rift_s/rift_s.c
		rift_s/rift_s.h
		)
	target_link_libraries(
		drv_rift_s
		PRIVATE
			xrt-interfaces
			aux_util
			aux_util_sink
			aux_math
			xrt-external-cjson
			xrt-external-tinyceres
		)
	if(XRT_BUILD_DRIVER_HANDTRACKING)
		target_link_libraries(drv_rift_s PRIVATE drv_ht)
	endif()
	list(APPEND ENABLED_HEADSET_DRIVERS rift-s)
endif()

set(VIVE_CONFIG_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/vive")
if(XRT_BUILD_DRIVER_VIVE)
	add_library(
		drv_vive STATIC
		vive/vive_device.h
		vive/vive_device.c
		vive/vive_prober.h
		vive/vive_prober.c
		vive/vive_protocol.c
		vive/vive_protocol.h
		vive/vive_controller.h
		vive/vive_controller.c
		vive/vive_lighthouse.h
		vive/vive_lighthouse.c
		vive/vive_source.h
		vive/vive_source.c
		)
	target_link_libraries(
		drv_vive
		PRIVATE
			xrt-interfaces
			aux_os
			aux_util
			aux_math
			xrt-external-cjson
			aux_vive
		)
	target_link_libraries(drv_vive PRIVATE ${ZLIB_LIBRARIES})
	target_include_directories(drv_vive PRIVATE ${ZLIB_INCLUDE_DIRS})
	list(APPEND ENABLED_HEADSET_DRIVERS vive)
endif()

if(XRT_HAVE_V4L2)
	add_library(drv_v4l2 STATIC v4l2/v4l2_driver.h v4l2/v4l2_driver.c)
	target_link_libraries(drv_v4l2 PRIVATE xrt-interfaces aux_os aux_util)
	list(APPEND ENABLED_DRIVERS v4l2)
endif()

if(XRT_BUILD_DRIVER_VF)
	add_library(drv_vf STATIC vf/vf_driver.c)
	target_link_libraries(drv_vf PRIVATE xrt-interfaces aux_os aux_util ${GST_LIBRARIES})
	target_include_directories(drv_vf PRIVATE ${GST_INCLUDE_DIRS})
	list(APPEND ENABLED_DRIVERS vf)
endif()

if(XRT_BUILD_DRIVER_HANDTRACKING)
	add_library(drv_ht STATIC ht/ht_driver.c ht/ht_interface.h)
	target_link_libraries(
		drv_ht
		PRIVATE
			xrt-interfaces
			aux_os
			aux_util
			aux_math
			t_ht_mercury
			hand_async
		)

	target_include_directories(drv_ht PRIVATE ${OpenCV_INCLUDE_DIRS})
	list(APPEND ENABLED_DRIVERS ht)
endif()

if(XRT_BUILD_DRIVER_SURVIVE)
	add_library(drv_survive STATIC survive/survive_driver.c survive/survive_interface.h)
	target_link_libraries(
		drv_survive
		PRIVATE
			xrt-interfaces
			aux_os
			aux_util
			aux_math
			aux_vive
			PkgConfig::SURVIVE
		)
	list(APPEND ENABLED_HEADSET_DRIVERS survive)
endif()

if(XRT_BUILD_DRIVER_ANDROID)
	add_library(
		drv_android STATIC android/android_prober.c android/android_prober.h
				   android/android_sensors.c android/android_sensors.h
		)
	target_link_libraries(
		drv_android
		PRIVATE
			xrt-interfaces
			aux_util
			aux_os
			aux_android
			${ANDROID_LIBRARY}
		)
	list(APPEND ENABLED_DRIVERS android)
endif()

if(XRT_BUILD_DRIVER_ILLIXR)
	add_library(
		drv_illixr STATIC
		illixr/illixr_device.cpp
		illixr/illixr_interface.h
		illixr/illixr_prober.c
		illixr/illixr_component.cpp
		illixr/illixr_component.h
		)
	target_link_libraries(drv_illixr PUBLIC ${CMAKE_DL_LIBS} xrt-interfaces aux_util aux_os)
	target_include_directories(drv_illixr PUBLIC ${ILLIXR_PATH})
	list(APPEND ENABLED_HEADSET_DRIVERS illixr)
endif()

add_library(drv_multi STATIC multi_wrapper/multi.c multi_wrapper/multi.h)
target_link_libraries(drv_multi PUBLIC xrt-interfaces aux_util)
list(APPEND ENABLED_HEADSET_DRIVERS drv_multi)

if(XRT_BUILD_DRIVER_WMR)
	add_library(
		drv_wmr STATIC
		wmr/wmr_camera.h
		wmr/wmr_common.h
		wmr/wmr_config.c
		wmr/wmr_config.h
		wmr/wmr_controller_base.c
		wmr/wmr_controller_base.h
		wmr/wmr_controller_og.c
		wmr/wmr_controller_hp.c
		wmr/wmr_controller.c
		wmr/wmr_controller.h
		wmr/wmr_bt_controller.c
		wmr/wmr_bt_controller.h
		wmr/wmr_hmd.c
		wmr/wmr_hmd.h
		wmr/wmr_hmd_controller.c
		wmr/wmr_hmd_controller.h
		wmr/wmr_interface.h
		wmr/wmr_prober.c
		wmr/wmr_protocol.c
		wmr/wmr_protocol.h
		wmr/wmr_controller_protocol.c
		wmr/wmr_controller_protocol.h
		wmr/wmr_source.c
		wmr/wmr_source.h
		)
	target_link_libraries(
		drv_wmr
		PRIVATE
			xrt-interfaces
			aux_util
			aux_util_sink # TODO remove this when converted to builders.
			aux_math
			aux_tracking
			xrt-external-cjson
		)
	list(APPEND ENABLED_HEADSET_DRIVERS wmr)

	# Can only build camera support with libusb
	if(XRT_HAVE_LIBUSB)
		target_sources(drv_wmr PRIVATE wmr/wmr_camera.c)
		target_include_directories(drv_wmr PUBLIC ${LIBUSB1_INCLUDE_DIRS})
		target_link_libraries(drv_wmr PRIVATE ${LIBUSB1_LIBRARIES})
	endif()

	if(XRT_BUILD_DRIVER_HANDTRACKING)
		target_link_libraries(drv_wmr PRIVATE drv_ht)
		target_link_libraries(drv_wmr PRIVATE drv_cemu)
	endif()
endif()

if(XRT_BUILD_DRIVER_EUROC)
	add_library(
		drv_euroc STATIC
		euroc/euroc_player.cpp
		euroc/euroc_driver.h
		euroc/euroc_device.c
		euroc/euroc_interface.h
		euroc/euroc_runner.c
		)
	target_link_libraries(
		drv_euroc PRIVATE xrt-interfaces aux_util aux_tracking ${OpenCV_LIBRARIES}
		)
	target_include_directories(drv_euroc PRIVATE ${OpenCV_INCLUDE_DIRS})
	list(APPEND ENABLED_DRIVERS euroc)
endif()

if(XRT_BUILD_DRIVER_SIMULAVR)
	add_library(drv_svr STATIC simula/svr_hmd.c simula/svr_interface.h)
	target_link_libraries(drv_svr PRIVATE xrt-interfaces aux_math xrt-external-cjson)
	list(APPEND ENABLED_HEADSET_DRIVERS svr)
endif()

if(XRT_BUILD_DRIVER_STEAMVR_LIGHTHOUSE)
	add_library(drv_vp2 STATIC vp2/vp2_hid.c vp2/vp2_hid.h)
	target_link_libraries(drv_vp2 PRIVATE aux_os aux_util aux_math aux_vive)
endif()

if(XRT_BUILD_DRIVER_STEAMVR_LIGHTHOUSE)
	add_library(
		drv_steamvr_lh STATIC
		steamvr_lh/steamvr_lh.cpp
		steamvr_lh/device.cpp
		steamvr_lh/interfaces/driver_manager.cpp
		steamvr_lh/interfaces/iobuffer.cpp
		steamvr_lh/interfaces/resources.cpp
		steamvr_lh/interfaces/settings.cpp
		steamvr_lh/interfaces/blockqueue.cpp
		steamvr_lh/interfaces/paths.cpp
		)
	target_link_libraries(
		drv_steamvr_lh
		PRIVATE
			aux_util
			aux_math
			aux_vive
			drv_vp2
			drv_includes
			xrt-interfaces
			xrt-external-openvr
			xrt-external-cjson
			xrt-external-vdf
			${CMAKE_DL_LIBS}
		)
	list(APPEND ENABLED_HEADSET_DRIVERS steamvr_lh)
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

	# Common sources (device, interface, types) — always present
	list(APPEND LEIA_SOURCES
		leia/leia_device.c
		leia/leia_interface.h
		leia/leia_types.h)

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

add_library(drv_sim_display STATIC ${SIM_DISPLAY_SOURCES})
target_link_libraries(drv_sim_display PRIVATE xrt-interfaces aux_util aux_os PUBLIC drv_includes)

if(XRT_HAVE_VULKAN)
	target_link_libraries(drv_sim_display PRIVATE aux_vk)
	target_include_directories(drv_sim_display PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
endif()

if(WIN32)
	target_link_libraries(drv_sim_display PRIVATE d3d11 d3dcompiler)
endif()

if(APPLE)
	target_link_libraries(drv_sim_display PRIVATE "-framework CoreGraphics")
endif()

list(APPEND ENABLED_DRIVERS drv_sim_display)

if(XRT_BUILD_SAMPLES)
	# We build the sample driver to make sure it stays valid,
	# but it never gets linked into a final target.
	add_library(
		drv_sample STATIC sample/sample_hmd.c sample/sample_interface.h
				  sample/sample_prober.c
		)
	target_link_libraries(drv_sample PRIVATE xrt-interfaces aux_util)
endif()

if(ENABLED_HEADSET_DRIVERS)
	set(ENABLED_DRIVERS ${ENABLED_HEADSET_DRIVERS} ${ENABLED_DRIVERS})
	list(SORT ENABLED_DRIVERS)
	string(REPLACE ";" " " ENABLED_DRIVERS "${ENABLED_DRIVERS}")
	message(STATUS "Enabled drivers: ${ENABLED_DRIVERS}")
else()
	message(FATAL_ERROR "You must enable at least one headset driver to build Monado.")
endif()
