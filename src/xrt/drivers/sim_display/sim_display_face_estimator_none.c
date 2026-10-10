// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display webcam eye tracking (#1855): the default build's
 *         estimator slot — empty. Which face-landmark model and inference
 *         dependency to ship is an open question (see
 *         sim_display_face_estimator.h); until one lands, webcam tracking
 *         reports itself unavailable and never opens a camera.
 * @ingroup drv_sim_display
 */

#include "sim_display_face_estimator.h"

#include <stddef.h>

bool
sim_face_estimator_default_available(void)
{
	return false;
}

struct sim_face_estimator *
sim_face_estimator_create_default(void)
{
	return NULL;
}
