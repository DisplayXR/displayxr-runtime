# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0

######
# The displayxr://activate deep-link handler (ADR-064 §5,
# docs/specs/runtime/activation-deep-link.md).
#
#   activation_handler     the flow, pure over an injected delivery: strict
#                          parse -> consume the pending nonce -> deliver to the
#                          one component it was begun for. Unit-tested as is.
#   target_activation      + the shipped delivery (the running service's live
#                          instance over DIAG, else this process's loaders) and
#                          the native message. Linked by displayxr-cli
#                          (`activate-url`, what the Linux .desktop entry runs)
#                          and by the two handler programs below.
#   displayxr-activate     Windows: the GUI-subsystem program the installer
#                          registers for the URL scheme.
#   DisplayXRActivate.app  macOS: the LSUIElement bundle whose Info.plist
#                          declares the scheme (a URL scheme needs a bundle). A
#                          shim over the displayxr-cli beside it.

add_library(activation_handler STATIC activation_handler.c activation_handler.h)
target_link_libraries(activation_handler PUBLIC xrt-interfaces aux_util)
target_include_directories(activation_handler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(
	target_activation STATIC activation_deliver.c activation_deliver.h activation_notify.c
	)
target_link_libraries(
	target_activation
	PUBLIC activation_handler
	PRIVATE
		aux_os
		aux_util
		target_lists
		displayxr::conversion_core
		displayxr::conversion_store
	)

# The live-instance delivery talks to the running service as a DIAG client.
# Same gating as the CLI's own IPC (targets/cli).
if(XRT_FEATURE_SERVICE AND XRT_MODULE_IPC)
	target_link_libraries(target_activation PRIVATE ipc_client)
	target_compile_definitions(target_activation PRIVATE ACTIVATION_HAVE_IPC)
endif()

if(WIN32)
	add_executable(displayxr-activate WIN32 main_win.c ../common/app_icon.rc)
	add_sanitizers(displayxr-activate)
	target_link_libraries(
		displayxr-activate
		PRIVATE
			target_activation
			xrt-interfaces
			aux_os
			aux_util
			# The loaders the in-process delivery calls, with what they pull in
			# (the same set displayxr-cli links for its `activate` verbs).
			target_instance_no_comp
			target_lists
			drv_includes
			displayxr::conversion_core
			displayxr::conversion_store
			shell32
			user32
		)
	if(XRT_FEATURE_SERVICE AND XRT_MODULE_IPC)
		target_link_libraries(displayxr-activate PRIVATE ipc_client)
	endif()
	# The shared per-monitor-v2 manifest every DisplayXR executable embeds
	# (#1201), merged with the default asInvoker UAC manifest: this program
	# never asks for elevation.
	if(MSVC)
		target_link_options(
			displayxr-activate PRIVATE "/MANIFEST:EMBED"
			"/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/../common/dpi_aware.manifest"
			)
	endif()
	install(TARGETS displayxr-activate RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(APPLE AND NOT IOS)
	# A thin shim: it runs the displayxr-cli beside it (`activate-url --json`)
	# and shows what that reports. System frameworks only, so it needs no
	# rpath fixup and no entitlements, like the desktop-capture helper bundle.
	add_executable(displayxr-activate-app MACOSX_BUNDLE main_macos.m)
	set_target_properties(
		displayxr-activate-app
		PROPERTIES OUTPUT_NAME "DisplayXRActivate"
			   MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
			   MACOSX_BUNDLE_GUI_IDENTIFIER "com.displayxr.activate"
			   MACOSX_BUNDLE_BUNDLE_NAME "DisplayXR Activation"
			   MACOSX_BUNDLE_BUNDLE_VERSION "${CMAKE_PROJECT_VERSION}"
			   MACOSX_BUNDLE_SHORT_VERSION_STRING "${CMAKE_PROJECT_VERSION}"
		)
	target_compile_options(displayxr-activate-app PRIVATE -fobjc-arc)
	target_link_libraries(displayxr-activate-app PRIVATE "-framework Foundation" "-framework AppKit")
endif()
