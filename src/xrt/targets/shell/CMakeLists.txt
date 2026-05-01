# Copyright 2026, Leia Inc.
# SPDX-License-Identifier: BSL-1.0

set(DISPLAYXR_SHELL_SOURCES
    main.c
    shell_pe_scan.c
    shell_pe_scan.h
    shell_app_scan.c
    shell_app_scan.h
    shell_openxr.cpp
    shell_openxr.h
    shell_chrome.cpp
    shell_chrome.h
    displayxr_shell_resource.h
)

# Phase 2.I: shell migrates onto the public OpenXR extensions. Discover
# the OpenXR loader the same way webxr_bridge does (via OpenXR_ROOT
# passed from scripts/build_windows.bat). The shell does not render
# swapchains; it only needs xrCreate{Instance,Session} + xrGetInstanceProcAddr.
find_package(OpenXR CONFIG QUIET)
if(NOT OpenXR_FOUND)
    if(DEFINED OpenXR_ROOT AND EXISTS "${OpenXR_ROOT}/x64/lib/openxr_loader.lib")
        add_library(OpenXR::openxr_loader SHARED IMPORTED)
        set_target_properties(OpenXR::openxr_loader PROPERTIES
            IMPORTED_IMPLIB "${OpenXR_ROOT}/x64/lib/openxr_loader.lib"
            IMPORTED_LOCATION "${OpenXR_ROOT}/x64/bin/openxr_loader.dll"
        )
        set(OpenXR_FOUND TRUE)
    endif()
endif()
if(NOT OpenXR_FOUND)
    message(FATAL_ERROR "displayxr-shell requires the OpenXR loader (Phase 2.I migration)")
endif()

if(WIN32)
    list(APPEND DISPLAYXR_SHELL_SOURCES displayxr_shell.rc)
endif()

add_executable(displayxr-shell WIN32 ${DISPLAYXR_SHELL_SOURCES})
add_sanitizers(displayxr-shell)

target_include_directories(displayxr-shell PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../external/stb  # stb_image_write.h (Browse-for-app icon extract)
    ${CMAKE_SOURCE_DIR}/src/external/openxr_includes   # XR_EXT_spatial_workspace.h, XR_EXT_app_launcher.h
)

# Shell consumes only public OpenXR + extension headers + the loader,
# plus cJSON for parsing app-sidecar JSON config (a stable third-party
# library, not internal to the runtime project). Decoupled from
# aux_util / ipc_client / xrt_* headers so this binary can move to its
# own repo.
if(XRT_HAVE_SYSTEM_CJSON)
    target_link_libraries(displayxr-shell PRIVATE OpenXR::openxr_loader cJSON::cJSON)
else()
    target_link_libraries(displayxr-shell PRIVATE OpenXR::openxr_loader xrt-external-cjson)
endif()

if(WIN32)
    # Dbghelp: ImageDirectoryEntryToData (PE import scan)
    # Comdlg32: GetOpenFileNameA (Phase 5.14 Browse-for-app tile)
    # D3D11/DXGI: Phase 2.C — shell creates an ID3D11Device on the runtime's
    #     adapter to author chrome swapchain images.
    # D3DCompiler: Phase 2.C C3.C — runtime HLSL compile for chrome shaders.
    target_link_libraries(displayxr-shell PRIVATE dbghelp comdlg32 d3d11 dxgi d3dcompiler)
endif()

install(TARGETS displayxr-shell RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
