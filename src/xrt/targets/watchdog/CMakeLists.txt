# Copyright 2024, Leia Inc.
# SPDX-License-Identifier: BSL-1.0

# SRMonado Watchdog - Windows service launcher and monitor

set(WATCHDOG_TARGET SRMonadoWatchdog)

add_executable(${WATCHDOG_TARGET} WIN32
	watchdog_main.cpp
	watchdog_service_mgr.cpp
	watchdog_service_mgr.h
	watchdog_client_mon.cpp
	watchdog_client_mon.h
	watchdog_tray.cpp
	watchdog_tray.h
	watchdog_resources.rc
)

target_include_directories(${WATCHDOG_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${WATCHDOG_TARGET} PRIVATE
	aux_util
	Shlwapi
	shell32
)

# Enable Unicode
target_compile_definitions(${WATCHDOG_TARGET} PRIVATE
	UNICODE
	_UNICODE
)

# Install to the same location as the service
install(TARGETS ${WATCHDOG_TARGET} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
