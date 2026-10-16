# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0

####
# Desktop control (XR_DXR_desktop_control, ADR-056).
#
# Common policy layer (consent order, kill switch, idle wait, target
# verification, audit) + exactly ONE OS backend. Linked into the runtime library
# only: every operation runs inside the app's process so the OS consent is the
# app's (ADR-056 D2). Never link this into displayxr-service or displayxr-cli
# for anything but the store / audit readers (u_desktop_policy).
#
# u_desktop_fake.c is an in-memory desktop compiled on every OS; it is only
# ever selected by DXR_DESKTOP_BACKEND=fake (tests, CI, the probe's E2E run).
#

add_library(
	aux_desktop STATIC
	u_desktop.h
	u_desktop_chord.c
	u_desktop_chord.h
	u_desktop_control.c
	u_desktop_control.h
	u_desktop_fake.c
	u_desktop_fake.h
	u_desktop_policy.c
	u_desktop_policy.h
	u_desktop_select.c
	)
target_link_libraries(aux_desktop PUBLIC aux-includes aux_os PRIVATE aux_util xrt-external-cjson)

if(WIN32)
	target_sources(aux_desktop PRIVATE u_desktop_win32.cpp)
	target_link_libraries(aux_desktop PRIVATE user32 advapi32 ole32 oleaut32 uuid dwmapi)
elseif(APPLE)
	target_sources(aux_desktop PRIVATE u_desktop_macos.m)
	target_link_libraries(
		aux_desktop PRIVATE "-framework ApplicationServices" "-framework AppKit" "-framework CoreGraphics"
				    "-framework CoreFoundation"
		)
elseif(XRT_HAVE_LINUX AND NOT ANDROID AND XRT_HAVE_DBUS)
	# GNOME Wayland: RemoteDesktop portal + libei (dlopen'd: no DT_NEEDED, libei is
	# absent on Ubuntu 22.04 and is not on package_deb_linux.sh's STABLE_SONAMES),
	# the DisplayXR GNOME extension (read-only, v16) and AT-SPI, all over libdbus.
	target_sources(aux_desktop PRIVATE u_desktop_gnome.c)
	target_include_directories(aux_desktop SYSTEM PRIVATE ${DBUS_INCLUDE_DIRS})
	target_link_libraries(aux_desktop PRIVATE ${DBUS_LIBRARIES} ${CMAKE_DL_LIBS})
else()
	target_sources(aux_desktop PRIVATE u_desktop_none.c)
endif()

# Bring-up tool: drives the platform backend directly (no OpenXR, NO safety layer).
add_executable(desktop_backend_smoke desktop_backend_smoke.c)
target_link_libraries(desktop_backend_smoke PRIVATE aux_desktop)
