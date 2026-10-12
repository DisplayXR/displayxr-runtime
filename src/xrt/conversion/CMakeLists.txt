# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0

# ADR-053 P1a: the conversion core's pure modules (#2000) - the provider and
# capability registry with its persisted describe cache, deterministic
# selection, and the readiness state machine. Pure C11: no threads, no GPU, no
# IPC, no registry or file I/O, no logging; host-tested by tests_conv_*.
add_library(
	conv_core STATIC
	conv_readiness.c
	conv_readiness.h
	conv_select.c
	conv_select.h
	)
add_library(displayxr::conversion_core ALIAS conv_core)
# Consumers include "conversion/conv_*.h".
target_include_directories(conv_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(conv_core PUBLIC xrt-interfaces)
