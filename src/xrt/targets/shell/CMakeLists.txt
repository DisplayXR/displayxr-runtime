# Copyright 2026, Leia Inc.
# SPDX-License-Identifier: BSL-1.0

add_executable(displayxr-shell WIN32 main.c)
add_sanitizers(displayxr-shell)

target_include_directories(displayxr-shell PRIVATE ipc)

target_link_libraries(displayxr-shell PRIVATE aux_util ipc_client)

install(TARGETS displayxr-shell RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
