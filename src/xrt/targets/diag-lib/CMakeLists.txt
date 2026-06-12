# Copyright 2026, Leia Inc.
# SPDX-License-Identifier: BSL-1.0

# JNI bridge for the Android diagnostics dashboard (#558). Runs the
# displayxr-cli query core (headless plug-in discovery + display info +
# self-test) inside the APK's short-lived :diag process and returns the
# snapshot as a JSON string. Built for BOTH APK flavors; the live IPC
# section is compiled in only when the service exists (out-of-process).
add_library(displayxr-diag MODULE diag_target.cpp)

target_link_libraries(
	displayxr-diag
	PRIVATE
		aux_util
		aux_android
		st_prober
		cli_query
		target_instance_no_comp
		target_lists
		xrt-external-cjson
	)

if(XRT_FEATURE_SERVICE AND XRT_MODULE_IPC)
	target_link_libraries(displayxr-diag PRIVATE ipc_client)
	target_compile_definitions(displayxr-diag PRIVATE DIAG_HAVE_IPC)
endif()
