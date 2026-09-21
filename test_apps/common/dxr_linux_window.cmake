# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# Wire the shared dual-backend Linux window helper (test_apps/common/
# dxr_linux_window.{h,cpp}) into an app target.
#
# X11 is mandatory (find_package(X11 REQUIRED) is the caller's job — every Linux
# test app needs it). Wayland is OPTIONAL and detected here: with
# libwayland-client present the helper's Wayland leg is compiled in and
# DXR_APP_HAVE_WAYLAND is defined; without it the app still builds and runs,
# X11-only. The xdg-shell client glue is generated at build time by
# wayland-scanner from the XML VENDORED at common/wayland-protocols/ — the
# wayland-protocols package ships that XML and is not installed everywhere
# (notably not in CI images), so the tree carries its own copy.
#
# Usage, from an app's CMakeLists.txt, AFTER add_executable():
#     include(${CMAKE_SOURCE_DIR}/../common/dxr_linux_window.cmake)
#     dxr_target_add_linux_window(my_app)

include_guard(GLOBAL)

set(DXR_LINUX_WINDOW_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(dxr_target_add_linux_window TARGET)
    target_sources(${TARGET} PRIVATE "${DXR_LINUX_WINDOW_DIR}/dxr_linux_window.cpp")
    target_include_directories(${TARGET} PRIVATE "${DXR_LINUX_WINDOW_DIR}")
    # u_x11_scale.h: header-only, dependency-free placement-quantum arithmetic
    # shared with the runtime (and pinned by tests/tests_aux_x11_scale.cpp), so
    # the helper's landing check and the runtime's lattice search agree.
    target_include_directories(${TARGET} PRIVATE "${DXR_LINUX_WINDOW_DIR}/../../src/xrt/auxiliary/util")

    # Xrandr (optional, libxrandr-dev). _NET_WM_FULLSCREEN_MONITORS targets a
    # monitor by RandR INDEX, and XRRGetMonitors is what turns the panel rect
    # into one (#729). Without it the helper still goes fullscreen — it just
    # relies on the window already sitting on the right output, which is what
    # the post-map move + pump arranges. NOTE: this must stay ABOVE the Wayland
    # detection, which returns early when libwayland-client is absent.
    find_package(X11 QUIET)
    if(TARGET X11::Xrandr)
        target_link_libraries(${TARGET} PRIVATE X11::Xrandr)
        target_compile_definitions(${TARGET} PRIVATE DXR_APP_HAVE_XRANDR)
        message(STATUS "${TARGET}: Xrandr found — X11 fullscreen targets the panel's monitor index")
    else()
        message(STATUS "${TARGET}: Xrandr NOT found — X11 fullscreen falls back to the window's "
                       "current output (install libxrandr-dev for _NET_WM_FULLSCREEN_MONITORS)")
    endif()

    find_package(PkgConfig QUIET)
    set(_wl_found FALSE)
    if(PkgConfig_FOUND)
        pkg_check_modules(WAYLAND_CLIENT QUIET wayland-client)
        if(WAYLAND_CLIENT_FOUND)
            set(_wl_found TRUE)
        endif()
    endif()

    if(NOT _wl_found)
        message(STATUS "${TARGET}: libwayland-client NOT found — X11-only build "
                       "(XR_DXR_wayland_surface_binding backend compiled out)")
        return()
    endif()

    # wayland-scanner: prefer the one the pkg-config file points at (matches the
    # installed libwayland), fall back to PATH.
    set(_scanner "")
    if(PkgConfig_FOUND)
        pkg_check_modules(WAYLAND_SCANNER_PC QUIET wayland-scanner)
        if(WAYLAND_SCANNER_PC_FOUND)
            pkg_get_variable(_scanner wayland-scanner wayland_scanner)
        endif()
    endif()
    if(NOT _scanner)
        find_program(_scanner NAMES wayland-scanner)
    endif()
    if(NOT _scanner)
        message(STATUS "${TARGET}: wayland-scanner NOT found — X11-only build "
                       "(XR_DXR_wayland_surface_binding backend compiled out)")
        return()
    endif()

    set(_xml "${DXR_LINUX_WINDOW_DIR}/wayland-protocols/xdg-shell.xml")
    set(_gen "${CMAKE_CURRENT_BINARY_DIR}/wayland-generated")
    set(_hdr "${_gen}/xdg-shell-client-protocol.h")
    set(_src "${_gen}/xdg-shell-protocol.c")

    add_custom_command(
        OUTPUT "${_hdr}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_gen}"
        COMMAND "${_scanner}" client-header "${_xml}" "${_hdr}"
        DEPENDS "${_xml}"
        COMMENT "wayland-scanner client-header xdg-shell (${TARGET})"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${_src}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_gen}"
        COMMAND "${_scanner}" private-code "${_xml}" "${_src}"
        DEPENDS "${_xml}"
        COMMENT "wayland-scanner private-code xdg-shell (${TARGET})"
        VERBATIM
    )

    target_sources(${TARGET} PRIVATE "${_hdr}" "${_src}")
    target_include_directories(${TARGET} PRIVATE "${_gen}" ${WAYLAND_CLIENT_INCLUDE_DIRS})
    target_link_libraries(${TARGET} PRIVATE ${WAYLAND_CLIENT_LIBRARIES})
    target_link_directories(${TARGET} PRIVATE ${WAYLAND_CLIENT_LIBRARY_DIRS})
    target_compile_definitions(${TARGET} PRIVATE DXR_APP_HAVE_WAYLAND)

    message(STATUS "${TARGET}: Wayland backend ENABLED "
                   "(wayland-client ${WAYLAND_CLIENT_VERSION}, scanner ${_scanner})")
endfunction()
