# Copyright 2026, DisplayXR
# SPDX-License-Identifier: BSL-1.0

######
# displayxr-desktop-capture (#2264): the one DisplayXR process that holds the
# macOS Screen Recording grant. An LSUIElement app bundle so the prompt and the
# System Settings entry read "DisplayXR Desktop Capture", started on demand by
# launchd through the MachServices entry of its LaunchAgent
# (com.displayxr.desktop-capture.plist) — never at login.

add_executable(displayxr-desktop-capture MACOSX_BUNDLE main.m)
set_target_properties(
	displayxr-desktop-capture
	PROPERTIES OUTPUT_NAME "DisplayXRDesktopCapture"
		   MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
		   MACOSX_BUNDLE_GUI_IDENTIFIER "com.displayxr.desktop-capture"
		   MACOSX_BUNDLE_BUNDLE_NAME "DisplayXR Desktop Capture"
		   MACOSX_BUNDLE_BUNDLE_VERSION "${CMAKE_PROJECT_VERSION}"
		   MACOSX_BUNDLE_SHORT_VERSION_STRING "${CMAKE_PROJECT_VERSION}"
	)
target_compile_options(displayxr-desktop-capture PRIVATE -fobjc-arc)
target_include_directories(displayxr-desktop-capture PRIVATE ${CMAKE_SOURCE_DIR}/src/xrt/auxiliary)
target_link_libraries(
	displayxr-desktop-capture
	PRIVATE "-framework Foundation"
		"-framework CoreGraphics"
		"-framework CoreMedia"
		"-framework CoreVideo"
		"-framework IOSurface"
		"-framework Metal"
		# Weak: the helper still launches (and answers UNSUPPORTED) on macOS < 12.3.
		"-weak_framework ScreenCaptureKit"
	)

# Dev LaunchAgent pointing at this build tree (scripts/macos_desktop_capture_dev.sh installs it).
configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/com.displayxr.desktop-capture.plist.in
	${CMAKE_CURRENT_BINARY_DIR}/com.displayxr.desktop-capture.dev.plist @ONLY
	)
