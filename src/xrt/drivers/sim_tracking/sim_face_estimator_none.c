// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  webcam eye tracking (#1855, sim_tracking SOURCE=webcam): the default build's
 *         estimator slot — empty. Which face-landmark model and inference
 *         dependency to ship is an open question (see
 *         sim_face_estimator.h); until one lands, webcam tracking
 *         reports itself unavailable and never opens a camera.
 * @ingroup drv_sim_tracking
 */

#include "sim_face_estimator.h"

#include <stddef.h>

bool
sim_face_estimator_default_available(const struct sim_webcam_settings *settings)
{
	(void)settings;
	return false;
}

const char *
sim_face_estimator_default_name(void)
{
	return "none";
}

struct sim_face_estimator *
sim_face_estimator_create_default(const struct sim_webcam_settings *settings)
{
	(void)settings;
	return NULL;
}
