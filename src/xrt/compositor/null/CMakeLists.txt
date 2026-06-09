# Copyright 2019-2022, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0

add_library(comp_null STATIC null_compositor.c null_compositor.h)
target_link_libraries(
	comp_null
	PUBLIC xrt-interfaces
	PRIVATE
		aux_util
		aux_os
		aux_vk
		comp_util
		comp_multi
	)
target_include_directories(comp_null PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Android out-of-process present path (#510): the null compositor is the runtime
# service's system compositor on Android, and drives a per-session
# comp_target_swapchain that presents to the app's ANativeWindow
# (comp_window_android, surface injected via android_globals). Those shared
# main/ sources are not built anywhere else in the lightweight fork (comp_main
# was removed, #25), so compile them here.
if(ANDROID)
	target_sources(
		comp_null
		PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}/../main/comp_target_swapchain.c
			${CMAKE_CURRENT_SOURCE_DIR}/../main/comp_window_android.c
		)
	target_link_libraries(comp_null PRIVATE aux_android)
endif()
