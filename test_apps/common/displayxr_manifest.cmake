# Copyright 2026, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# displayxr_install_manifest(target manifest_dir)
#
# Copies every file in manifest_dir (e.g. the app's displayxr/ sub-directory)
# next to the built executable so the DisplayXR shell launcher can discover it.
#
# Expected layout in manifest_dir:
#   <exe_basename>.displayxr.json   (required — the sidecar)
#   icon.png                        (optional — 2D tile image)
#   icon_sbs.png                    (optional — side-by-side stereo tile image)
#
# The shell scanner looks for `<exe_basename>.displayxr.json` next to each
# discovered executable. See docs/specs/runtime/displayxr-app-manifest.md.

function(displayxr_install_manifest target manifest_dir)
    if(NOT IS_DIRECTORY "${manifest_dir}")
        message(WARNING "displayxr_install_manifest: directory not found: ${manifest_dir}")
        return()
    endif()

    file(GLOB _displayxr_files "${manifest_dir}/*")
    if(NOT _displayxr_files)
        message(WARNING "displayxr_install_manifest: no files in ${manifest_dir}")
        return()
    endif()

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${_displayxr_files}
            "$<TARGET_FILE_DIR:${target}>/"
        COMMENT "Copying DisplayXR manifest files for ${target}"
        VERBATIM
    )
endfunction()
