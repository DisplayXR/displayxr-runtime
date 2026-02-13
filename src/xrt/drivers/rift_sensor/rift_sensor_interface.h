// Copyright 2025-2026, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Interface to Oculus Rift sensor probing/initialization
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup drv_rift_sensor
 */

#pragma once

#include "tracking/t_camera_models.h"


#define RIFT_SENSOR_CLOCK_FREQ (40000000)
// @todo Remove when clang-format is updated in CI
// clang-format off
#define RIFT_SENSOR_CLOCK_TO_NS(x) ((timepoint_ns)(x) * 1000 / 40)
// clang-format on
#define RIFT_SENSOR_WIDTH 1280
#define RIFT_SENSOR_HEIGHT 960
#define RIFT_SENSOR_FRAME_SIZE (RIFT_SENSOR_WIDTH * RIFT_SENSOR_HEIGHT)

struct rift_sensor;
struct rift_sensor_context;

enum rift_sensor_variant
{
	RIFT_SENSOR_VARIANT_DK2,
	RIFT_SENSOR_VARIANT_CV1,
};

void
rift_sensor_context_destroy(struct rift_sensor_context *context);

int
rift_sensor_context_create(struct rift_sensor_context **out_context, struct xrt_frame_context *xfctx);
