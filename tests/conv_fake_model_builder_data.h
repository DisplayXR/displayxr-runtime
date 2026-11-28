// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The bytes the fake model builder (conv_fake_model_builder.c) pins
 *         and writes, shared with the tests that build its pack
 *         (tests_conv_model_build.cpp). Text on purpose: CMake writes the
 *         same source file for the CLI end-to-end test with string(REPEAT).
 */

#pragma once

#include <stdio.h>
#include <string.h>

#define FAKE_BUILDER_TOOL_ID "fake-model-builder"
#define FAKE_BUILDER_VERSION "1.2.0"
#define FAKE_BUILDER_RECIPE "fake"
#define FAKE_BUILDER_LICENCE "test-licence"
#define FAKE_BUILDER_SOURCE_UNIT "dxr-fake-checkpoint\n"
#define FAKE_BUILDER_SOURCE_REPEAT 200
#define FAKE_BUILDER_OUTPUT_REPEAT 100

//! The checkpoint: FAKE_BUILDER_SOURCE_UNIT x FAKE_BUILDER_SOURCE_REPEAT. Returns its size.
static inline size_t
fake_builder_source(char *buf, size_t cap)
{
	const size_t u = strlen(FAKE_BUILDER_SOURCE_UNIT);
	size_t n = 0;
	for (int i = 0; i < FAKE_BUILDER_SOURCE_REPEAT && n + u <= cap; i++) {
		memcpy(buf + n, FAKE_BUILDER_SOURCE_UNIT, u);
		n += u;
	}
	return n;
}

//! A variant's output: "dxr-fake-output:<variant>\n" x FAKE_BUILDER_OUTPUT_REPEAT. Returns its size.
static inline size_t
fake_builder_output(const char *variant, char *buf, size_t cap)
{
	char unit[128];
	const int u = snprintf(unit, sizeof(unit), "dxr-fake-output:%s\n", variant);
	size_t n = 0;
	for (int i = 0; u > 0 && i < FAKE_BUILDER_OUTPUT_REPEAT && n + (size_t)u <= cap; i++) {
		memcpy(buf + n, unit, (size_t)u);
		n += (size_t)u;
	}
	return n;
}
