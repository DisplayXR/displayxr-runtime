# Copyright 2026, Leia Inc.
# SPDX-License-Identifier: BSL-1.0

add_executable(displayxr-shell WIN32
    main.c
    shell_pe_scan.c
    shell_pe_scan.h
    shell_app_scan.c
    shell_app_scan.h
)
add_sanitizers(displayxr-shell)

target_include_directories(displayxr-shell PRIVATE ipc)

target_link_libraries(displayxr-shell PRIVATE aux_util ipc_client)

if(WIN32)
    # Dbghelp: ImageDirectoryEntryToData (PE import scan)
    target_link_libraries(displayxr-shell PRIVATE dbghelp)
endif()

install(TARGETS displayxr-shell RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
