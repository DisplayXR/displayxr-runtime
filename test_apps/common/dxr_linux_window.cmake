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
# X11-only. The xdg-shell and xdg-output client glue is generated at build time
# by wayland-scanner from the XML VENDORED at common/wayland-protocols/ — the
# wayland-protocols package ships that XML and is not installed everywhere
# (notably not in CI images), so the tree carries its own copy.
#
# xdg-output (#1596) is what makes the panel match possible at all. Core
# wl_output publishes a LOGICAL origin, a DEVICE-pixel mode, and an INTEGER
# scale — and the integer is wrong on any fractionally-scaled output (2 for a
# 1.6667 monitor on the measured box), so it cannot bridge the two spaces.
# zxdg_output_v1.logical_size can: mode / logical_size IS the fractional scale.
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

    # ONE definition of the logical->device conversion (#1595/#1596), shared
    # verbatim with the runtime rather than re-derived app-side: the app matches
    # outputs in device pixels, the runtime converts the window rect in device
    # pixels, and the two agreeing is the whole point. Header-only, no link
    # dependency — this pulls in src/xrt/auxiliary/util/u_wayland_geom.h and
    # nothing else.
    get_filename_component(_dxr_aux "${DXR_LINUX_WINDOW_DIR}/../../src/xrt/auxiliary" ABSOLUTE)
    if(NOT EXISTS "${_dxr_aux}/util/u_wayland_geom.h")
        message(FATAL_ERROR
            "${TARGET}: u_wayland_geom.h not found at ${_dxr_aux}/util — this helper must be built "
            "from inside a displayxr-runtime checkout")
    endif()
    target_include_directories(${TARGET} PRIVATE "${_dxr_aux}")

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

    set(_gen "${CMAKE_CURRENT_BINARY_DIR}/wayland-generated")
    set(_wl_generated "")
    # One entry per protocol, named by its XML STEM — that is both the file's
    # basename and the prefix wayland-scanner gives its outputs, and therefore
    # the name the #includes in dxr_linux_window.cpp use. (A list of "a;b"
    # pairs would not survive foreach, which flattens its arguments.)
    # xdg-decoration / cursor-shape (+ tablet-v2, which cursor-shape references)
    # / ext-background-effect (blur behind the translucent bar) serve the title
    # bar (#1654, dxr_wl_chrome.cpp).
    foreach(_stem "xdg-shell" "xdg-output-unstable-v1" "viewporter" "fractional-scale-v1"
                  "xdg-decoration-unstable-v1" "cursor-shape-v1" "tablet-v2"
                  "ext-background-effect-v1")
        set(_xml "${DXR_LINUX_WINDOW_DIR}/wayland-protocols/${_stem}.xml")
        set(_hdr "${_gen}/${_stem}-client-protocol.h")
        set(_src "${_gen}/${_stem}-protocol.c")

        add_custom_command(
            OUTPUT "${_hdr}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_gen}"
            COMMAND "${_scanner}" client-header "${_xml}" "${_hdr}"
            DEPENDS "${_xml}"
            COMMENT "wayland-scanner client-header ${_stem} (${TARGET})"
            VERBATIM
        )
        add_custom_command(
            OUTPUT "${_src}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_gen}"
            COMMAND "${_scanner}" private-code "${_xml}" "${_src}"
            DEPENDS "${_xml}"
            COMMENT "wayland-scanner private-code ${_stem} (${TARGET})"
            VERBATIM
        )
        list(APPEND _wl_generated "${_hdr}" "${_src}")
    endforeach()

    target_sources(${TARGET} PRIVATE ${_wl_generated})
    target_include_directories(${TARGET} PRIVATE "${_gen}" ${WAYLAND_CLIENT_INCLUDE_DIRS})
    target_link_libraries(${TARGET} PRIVATE ${WAYLAND_CLIENT_LIBRARIES})
    target_link_directories(${TARGET} PRIVATE ${WAYLAND_CLIENT_LIBRARY_DIRS})
    target_compile_definitions(${TARGET} PRIVATE DXR_APP_HAVE_WAYLAND)

    # Title bar for the native-Wayland leg (#1654): the Wayland glue here, the
    # painter from displayxr-common's displayxr::csd (the ONE chrome
    # implementation, shared with the X11 demos — displayxr-common#52). An app
    # pinned to a displayxr-common older than v2.17.0 has no such target and
    # simply builds without chrome (the pre-#1654 undecorated window).
    if(TARGET displayxr::csd)
        target_sources(${TARGET} PRIVATE "${DXR_LINUX_WINDOW_DIR}/dxr_wl_chrome.cpp"
                                         "${DXR_LINUX_WINDOW_DIR}/dxr_wl_placement.cpp")
        # libdbus-1 (optional): the client of the compositor's drag lattice,
        # which keeps the interlace phase still while the compositor drags the
        # window (#1609). Without it the title bar drags exactly as before.
        if(PkgConfig_FOUND)
            pkg_check_modules(DXR_DBUS QUIET dbus-1)
        endif()
        if(DXR_DBUS_FOUND)
            target_include_directories(${TARGET} PRIVATE ${DXR_DBUS_INCLUDE_DIRS})
            target_link_libraries(${TARGET} PRIVATE ${DXR_DBUS_LIBRARIES})
            target_link_directories(${TARGET} PRIVATE ${DXR_DBUS_LIBRARY_DIRS})
            target_compile_definitions(${TARGET} PRIVATE DXR_APP_HAVE_DBUS)
            message(STATUS "${TARGET}: drag lattice client ENABLED (libdbus ${DXR_DBUS_VERSION})")
        else()
            message(STATUS "${TARGET}: libdbus-1 NOT found — title-bar drags are unconstrained "
                           "(install libdbus-1-dev for the phase-snapped drag)")
        endif()
        target_link_libraries(${TARGET} PRIVATE displayxr::csd)
        target_compile_definitions(${TARGET} PRIVATE DXR_APP_HAVE_WL_CHROME)
        message(STATUS "${TARGET}: Wayland title bar ENABLED (displayxr::csd)")
    else()
        message(STATUS "${TARGET}: displayxr::csd NOT available (displayxr-common < v2.17.0) — "
                       "native-Wayland windows build without a title bar")
    endif()

    message(STATUS "${TARGET}: Wayland backend ENABLED "
                   "(wayland-client ${WAYLAND_CLIENT_VERSION}, scanner ${_scanner})")
endfunction()
