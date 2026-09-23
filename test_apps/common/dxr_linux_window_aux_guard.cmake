# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# Drift guard for the two header-only helpers the Linux app window shares with
# the runtime. displayxr-common's displayxr::linux_window (the ONE Linux window
# implementation — it used to live here as test_apps/common/dxr_linux_window)
# carries copies of
#
#     src/xrt/auxiliary/util/u_x11_scale.h      (placement-quantum arithmetic)
#     src/xrt/auxiliary/util/u_wayland_geom.h   (logical -> device conversion)
#
# because the app's output match / landing check and the runtime's own
# conversion must be the SAME function — two parties agreeing is the point
# (#1595/#1596). The copies live at common/linux/xrt_aux/util/ and must stay
# byte-identical to the runtime's. When you change one of these headers here,
# land the same bytes in displayxr-common and bump the pin; this check fails the
# test-app build (and so Linux CI) until you do.
#
# Usage, after FetchContent_MakeAvailable(displayxr_common):
#     include(${CMAKE_SOURCE_DIR}/../common/dxr_linux_window_aux_guard.cmake)

include_guard(GLOBAL)

foreach(_dxr_aux_header "u_x11_scale.h" "u_wayland_geom.h")
    set(_ours "${CMAKE_CURRENT_LIST_DIR}/../../src/xrt/auxiliary/util/${_dxr_aux_header}")
    set(_theirs "${displayxr_common_SOURCE_DIR}/common/linux/xrt_aux/util/${_dxr_aux_header}")
    if(NOT EXISTS "${_theirs}")
        message(FATAL_ERROR "displayxr-common has no common/linux/xrt_aux/util/${_dxr_aux_header} — "
                            "the pinned displayxr-common predates displayxr::linux_window")
    endif()
    file(SHA256 "${_ours}" _h_ours)
    file(SHA256 "${_theirs}" _h_theirs)
    if(NOT _h_ours STREQUAL _h_theirs)
        message(FATAL_ERROR
            "${_dxr_aux_header} differs between the runtime (src/xrt/auxiliary/util/) and the pinned "
            "displayxr-common (common/linux/xrt_aux/util/). The Linux app window and the runtime must "
            "share these bytes: copy the runtime's header into displayxr-common, tag it, and bump the "
            "GIT_TAG here.")
    endif()
endforeach()
