# Copyright 2025, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# Single source of truth for test-app classification, platform gating, and run
# environment. Included by ./CMakeLists.txt (the platform-aware aggregator); the
# run-env rule below is mirrored verbatim by the run-script generators in
# scripts/build_windows.bat and scripts/build_macos.sh.
#
# Naming contract (enforced by scripts/check_displayxr_app.py):
#   test_apps/<class>/<name>_<api>_<platform>/
#     class    = parent folder: handle | hosted | texture | legacy | probes.
#                It's the compositor-path axis every doc keys on; `zones` is a
#                FEATURE modifier, NOT a class, so cube_zones_* live under
#                handle/ and cube_zones_texture_* under texture/.
#     api      = d3d11 | d3d12 | vk | gl | metal
#     platform = SUFFIX: win | macos | android | linux  (drives the gate below)

# dxr_testapp_platform_ok(<app_dir_name> <out_var>)
#   Sets <out_var> to ON iff the app's dir-name suffix matches the platform
#   currently being configured. This is what makes the "orphaned app" failure
#   mode (an app on disk that no build list references — #692 item 1) structurally
#   impossible: the aggregator DISCOVERS every <class>/<app>/CMakeLists.txt and
#   asks this predicate, instead of carrying a hand-maintained add_subdirectory
#   list that silently drifts.
function(dxr_testapp_platform_ok _app _out)
    set(_ok OFF)
    if(_app MATCHES "_win$")
        if(WIN32)
            set(_ok ON)
        endif()
    elseif(_app MATCHES "_macos$")
        if(APPLE)
            set(_ok ON)
        endif()
    elseif(_app MATCHES "_android$")
        if(ANDROID)
            set(_ok ON)
        endif()
    elseif(_app MATCHES "_linux$")
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
            set(_ok ON)
        endif()
    endif()
    set(${_out} ${_ok} PARENT_SCOPE)
endfunction()

# Run-environment rule (DERIVED from the app name; no per-app table needed).
# The build scripts generate one run_<app>.{bat,sh} per built app applying this:
#
#   * name contains "zones"  → set DISPLAYXR_ZONES=1 so the dev runtime
#                              advertises XR_EXT_display_zones (#613/#673).
#   * name contains "_vk"    → the app owns its own VkInstance, so DON'T disable
#                              Vulkan implicit layers. Every other app disables
#                              them (VK_LOADER_LAYERS_DISABLE=*) to dodge buggy
#                              third-party layers (#105).
#
# dxr_testapp_run_env is provided for callers that want the rule from CMake; the
# scripts reimplement the identical two predicates inline.
function(dxr_testapp_run_env _app _out_zones _out_vk_app)
    string(FIND "${_app}" "zones" _z)
    string(FIND "${_app}" "_vk" _v)
    if(_z GREATER -1)
        set(${_out_zones} ON PARENT_SCOPE)
    else()
        set(${_out_zones} OFF PARENT_SCOPE)
    endif()
    if(_v GREATER -1)
        set(${_out_vk_app} ON PARENT_SCOPE)
    else()
        set(${_out_vk_app} OFF PARENT_SCOPE)
    endif()
endfunction()
