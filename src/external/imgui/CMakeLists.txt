# Copyright 2020-2025, Collabora, Ltd.
# Copyright 2025-2026, NVIDIA CORPORATION.
# SPDX-License-Identifier: BSL-1.0

if(NOT XRT_HAVE_OPENGL)
	message(FATAL_ERROR "XRT_HAVE_OPENGL is required for imgui but is not enabled")
endif()

add_library(
	xrt-external-imgui STATIC
	# imgui 1.90.8-docking
	imgui/imconfig.h
	imgui/imgui.cpp
	imgui/imgui_tables.cpp
	imgui/imgui.h
	imgui/imgui_demo.cpp
	imgui/imgui_draw.cpp
	imgui/backends/imgui_impl_opengl3.cpp
	imgui/backends/imgui_impl_opengl3.h
	imgui/imgui_internal.h
	imgui/imgui_widgets.cpp
	imgui/imstb_rectpack.h
	imgui/imstb_textedit.h
	imgui/imstb_truetype.h
	# cimgui 7c16d31cdb9d2db3038b324fe967ffa76b02c8c4
	cimgui/cimgui.cpp
	cimgui/cimgui.h
	cimgui/cimgui_impl.h
	# implot 0.17 f156599faefe316f7dd20fe6c783bf87c8bb6fd9
	implot/implot.cpp
	implot/implot.h
	implot/implot_internal.h
	implot/implot_items.cpp
	implot/implot_demo.cpp
	# cimplot 439f705b1cfae0e9fbe42c13ab5b45becc47d404
	cimplot/cimplot.cpp
	cimplot/cimplot.h
	imgui_monado/imgui_monado.cpp
	)

target_include_directories(
	xrt-external-imgui SYSTEM
	PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
	PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/imgui ${CMAKE_CURRENT_SOURCE_DIR}/cimgui
		${CMAKE_CURRENT_SOURCE_DIR}/implot
	)
target_compile_definitions(xrt-external-imgui PUBLIC CIMGUI_NO_EXPORT CIMGUI_USE_OPENGL3)

if(WIN32)
	target_compile_definitions(
		xrt-external-imgui PUBLIC "IMGUI_IMPL_API=extern\ \"C\" __declspec\(dllexport\)"
		)
else()
	target_compile_definitions(xrt-external-imgui PUBLIC "IMGUI_IMPL_API=extern\ \"C\"")
endif()

set_target_properties(xrt-external-imgui PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
target_link_libraries(xrt-external-imgui PUBLIC xrt-external-glad)

if(XRT_HAVE_SDL2)
	add_library(
		xrt-external-imgui-sdl2 STATIC imgui/backends/imgui_impl_sdl2.cpp
					       imgui/backends/imgui_impl_sdl2.h
		)
	target_include_directories(
		xrt-external-imgui-sdl2 SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/imgui
		)
	target_compile_definitions(xrt-external-imgui-sdl2 PUBLIC CIMGUI_USE_SDL2)
	target_link_libraries(xrt-external-imgui-sdl2 PUBLIC xrt-external-imgui xrt-external-sdl2)
	set_target_properties(xrt-external-imgui-sdl2 PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
endif()
