# Copyright 2019-2024, Collabora, Ltd.
# Copyright 2024-2025, NVIDIA CORPORATION.
# SPDX-License-Identifier: BSL-1.0

# c-imgui doesn't do well with IPO - lots of warnings.
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)

###
# Base GUI
add_library(
	st_gui_base STATIC
	gui_common.h
	gui_imgui.h
	gui_ogl.c
	gui_ogl.h
	gui_ogl_sink.c
	gui_scene.cpp
	gui_scene_debug.c
	gui_stb.c
	gui_widget_native_images.c
	gui_widget_native_images.h
	gui_window_record.c
	gui_window_record.h
	)
target_link_libraries(
	st_gui_base
	PRIVATE
		xrt-external-stb
		aux_util
		aux_util_sink
		aux_os
		aux_ogl
		xrt-external-imgui
	)
target_include_directories(st_gui_base INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)

###
# Main GUI
add_library(
	st_gui STATIC
	gui_prober.c
	gui_scene_calibrate.c
	gui_scene_main_menu.c
	gui_scene_record.c
	gui_scene_resampler_test.c
	gui_scene_tracking_overrides.c
	gui_scene_video.c
	)
target_link_libraries(
	st_gui
	PRIVATE
		xrt-external-stb
		aux_util
		aux_util_sink
		aux_os
		aux_ogl
		st_gui_base
		drv_includes
		xrt-external-imgui
	)
target_include_directories(st_gui INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(XRT_HAVE_GST)
	target_link_libraries(st_gui_base PRIVATE aux_gstreamer)
	target_link_libraries(st_gui PRIVATE aux_gstreamer)
endif()


if(XRT_HAVE_SDL2)
	target_link_libraries(st_gui PUBLIC xrt-external-imgui-sdl2)
endif()
