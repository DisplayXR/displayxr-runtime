# Copyright 2026, Leia Inc.
# SPDX-License-Identifier: BSL-1.0

######
# DisplayXR Control Panel — a thin ImGui + SDL2 GUI over `displayxr-cli`.
#
# It shells out to the sibling `displayxr-cli` (`info --json`,
# `selftest --json`, `dp ...`) and renders the results. It deliberately links
# ZERO runtime/vendor code: only ImGui+SDL2 (GUI), aux_ogl (glad/GL loader),
# and aux_util (cJSON for parsing). The process boundary makes ADR-019 trivial
# — the panel never touches a vendor symbol.

# WIN32 → GUI subsystem (no spurious console window); SDL2main supplies the
# WinMain entry that forwards to our main(). Ignored on non-Windows.
add_executable(control_panel WIN32 control_panel_main.c)
add_sanitizers(control_panel)

set_target_properties(control_panel PROPERTIES OUTPUT_NAME displayxr-control-panel PREFIX "")

target_link_libraries(
	control_panel
	PRIVATE
		xrt-external-imgui-sdl2 # ImGui (cimgui) + SDL2 + OpenGL3 backends
		aux_ogl                 # compiled glad (gladLoadGL + GL fns)
		aux_util                # cJSON (+ include path) for --json parsing
	)

if(WIN32)
	# cimgui/SDL sources are UTF-8; MSVC defaults to the active code page.
	target_compile_options(control_panel PRIVATE "$<$<C_COMPILER_ID:MSVC>:/utf-8>")
	# SDL2 supplies the WinMain entry point via SDL2main.
	target_link_libraries(control_panel PRIVATE SDL2::SDL2main)
endif()

install(TARGETS control_panel RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
