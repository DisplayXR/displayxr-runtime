# Copyright 2024, Leia Inc.
# SPDX-License-Identifier: BSL-1.0

# LeiaXR Runtime Switcher - Switch between SRMonado and SRHydra

set(SWITCHER_TARGET LeiaXRSwitcher)

add_executable(${SWITCHER_TARGET} WIN32
	switcher_main.cpp
	switcher_registry.cpp
	switcher_registry.h
	switcher_resources.rc
)

target_include_directories(${SWITCHER_TARGET} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(${SWITCHER_TARGET} PRIVATE
	Shlwapi
	Comctl32
)

# Enable Unicode and visual styles
target_compile_definitions(${SWITCHER_TARGET} PRIVATE
	UNICODE
	_UNICODE
)

# Install to the same location as the runtime
install(TARGETS ${SWITCHER_TARGET} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
