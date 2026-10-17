# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0

# ADR-053 P1a: the conversion core's pure modules (#2000) - the provider and
# capability registry with its persisted describe cache, deterministic
# selection, and the readiness state machine. Pure C11: no threads, no GPU, no
# IPC, no registry or file I/O, no logging; host-tested by tests_conv_*.
add_library(
	conv_core STATIC
	# #2030: the dp-lift row's static describe, shared by the in-service
	# adapter and the headless status (target_conv_status.c).
	conv_dp_lift_describe.c
	conv_dp_lift_describe.h
	conv_readiness.c
	conv_readiness.h
	conv_registry.c
	conv_registry.h
	conv_select.c
	conv_select.h
	# ADR-053 P2 (#2032): the satellite's wire format and the supervisor's
	# pure lifecycle FSM (no process, no clock: the proxy drives it).
	conv_host_wire.h
	conv_supervisor.c
	conv_supervisor.h
	)
add_library(displayxr::conversion_core ALIAS conv_core)
# Consumers include "conversion/conv_*.h".
target_include_directories(conv_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(conv_core PUBLIC xrt-interfaces)

# ADR-053 P1a (#2001): the conversion core's runtime half - host interface +
# in-service shim, stream lane (one dispatcher per host), pipeline composer,
# job lane + result staging, policy stages, and conv_core, the one object the
# service owns. Threads (os_threading) and logging, so a separate target: the
# pure modules above stay dependency-free. Graphics-API work is injected
# (conv_core_config), so this is still host-testable (tests_conv_lanes).
add_library(
	conv_runtime STATIC
	conv_core.c
	conv_core.h
	conv_core_internal.h
	conv_host_iface.h
	conv_host_inproc.c
	conv_host_inproc.h
	conv_job.c
	conv_job.h
	conv_observer_autoconv.c
	conv_observer_autoconv.h
	conv_pipeline.c
	conv_pipeline.h
	conv_policy.c
	conv_policy.h
	# #2003: the legacy DP-lift adapter (provider-ABI half; the lift thread is
	# the D3D11 service's, reached through conv_dp_lift_backend).
	conv_provider_legacy_dp.cpp
	conv_provider_legacy_dp.h
	conv_stage.c
	conv_stage.h
	conv_stream.c
	conv_stream.h
	)
add_library(displayxr::conversion_runtime ALIAS conv_runtime)
target_link_libraries(conv_runtime PUBLIC conv_core aux_util aux_os)

# ADR-053 P2 (#2034): the satellite host proxy (conv_host_ops over the host
# channel + shm rings). os_process is not built on Android, whose
# `:conversion` service is a different mechanism (roadmap §4.3).
if(NOT ANDROID)
	target_sources(conv_runtime PRIVATE conv_host_proxy.c conv_host_proxy.h)
	target_compile_definitions(conv_runtime PUBLIC CONV_HAVE_HOST_PROXY)
endif()

# ADR-053 P4 (#2108): the runtime-owned model store (root resolution, schema-2
# pack manifests, the browser's write protocol, the per-process hash check,
# the store ACL) and the host-op glue both hosts put behind model_resolve /
# model_request. Its own target: the CLI and both hosts link it, the pure
# core does not.
add_library(conv_store STATIC conv_model_store.c conv_model_store.h
	# ADR-057 S1: facts JSON + the convenience camera-block writer (CLI, tests).
	conv_camera_block.c conv_camera_block.h)
add_library(displayxr::conversion_store ALIAS conv_store)
target_link_libraries(conv_store PUBLIC conv_core aux_util aux_os xrt-external-cjson)
if(WIN32)
	target_link_libraries(conv_store PRIVATE winhttp advapi32)
endif()
