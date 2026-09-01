# Copyright 2019-2022, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0

####
# Main os library
#
# Avoid linking with libraries that brings in DSOs as this
# library is used everywhere. Including libraries loaded in
# by applications like the OpenXR runtime library.
#

add_library(
	aux_os STATIC
	os_documentation.h
	os_hid.h
	os_hid_hidraw.c
	os_threading.h
	os_time.cpp
	)
target_link_libraries(aux_os PUBLIC aux-includes xrt-pthreads)

# Only uses normal Windows libraries, doesn't add anything extra.
if(WIN32)
	target_link_libraries(aux_os PRIVATE winmm)
endif()

# EDID-based display identification (vendor-neutral monitor enumeration)
target_sources(aux_os PRIVATE os_display_edid.h)
if(WIN32)
	target_sources(aux_os PRIVATE os_display_edid_win32.c)
	target_link_libraries(aux_os PRIVATE setupapi)
else()
	target_sources(aux_os PRIVATE os_display_edid_stubs.c)
endif()

# Desktop rect + device name of the monitor the 3D panel is on (#1301).
# Deliberately light: no SetupAPI on Windows, and the X11 entry points are
# dlopen'd rather than linked, so this respects the no-DSO rule at the top of
# this file. Android and anything else falls through to the stub.
target_sources(aux_os PRIVATE os_display_desktop.h)
if(WIN32)
	target_sources(aux_os PRIVATE os_display_desktop_win32.c)
elseif(APPLE)
	target_sources(aux_os PRIVATE os_display_desktop_macos.c)
	# System frameworks, always present — CoreGraphics for the display bounds,
	# CoreFoundation for the display UUID string.
	target_link_libraries(aux_os PRIVATE "-framework CoreGraphics" "-framework CoreFoundation")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
	# Desktop Linux only — the XRT_OS_LINUX_DESKTOP distinction in CMake form.
	# Uses CMAKE_SYSTEM_NAME (the repo idiom) rather than the LINUX variable,
	# which only exists from CMake 3.25 and would silently evaluate FALSE here
	# (this project requires 3.22), dropping us to the stub with no diagnostic.
	target_sources(aux_os PRIVATE os_display_desktop_x11.c)
	# dlopen/dlsym only; libX11/libXrandr themselves are NOT linked.
	target_link_libraries(aux_os PRIVATE ${CMAKE_DL_LIBS})
else()
	target_sources(aux_os PRIVATE os_display_desktop_stubs.c)
endif()

if(APPLE)
	target_sources(aux_os PRIVATE os_macos.c)
endif()

####
# BLE library
#

add_library(aux_os_ble STATIC os_ble.h)
target_link_libraries(aux_os_ble PUBLIC aux-includes aux_os)

if(XRT_HAVE_DBUS)
	target_sources(aux_os_ble PRIVATE os_ble_dbus.c)
	target_link_libraries(aux_os_ble PRIVATE ${DBUS_LIBRARIES})
	target_include_directories(aux_os_ble SYSTEM PRIVATE ${DBUS_INCLUDE_DIRS})
else()
	target_sources(aux_os_ble PRIVATE os_ble_stubs.c)
endif()
