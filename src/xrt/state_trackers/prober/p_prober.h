// Copyright 2019, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Main prober code.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup st_prober
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_prober.h"

#include "util/u_logging.h"
#include "util/u_config_json.h"


/*
 *
 * Struct and defines
 *
 */

#define P_TRACE(d, ...) U_LOG_IFL_T(d->log_level, __VA_ARGS__)
#define P_DEBUG(d, ...) U_LOG_IFL_D(d->log_level, __VA_ARGS__)
#define P_INFO(d, ...) U_LOG_IFL_I(d->log_level, __VA_ARGS__)
#define P_WARN(d, ...) U_LOG_IFL_W(d->log_level, __VA_ARGS__)
#define P_ERROR(d, ...) U_LOG_IFL_E(d->log_level, __VA_ARGS__)

/*!
 * @implements xrt_prober
 */
struct prober
{
	struct xrt_prober base;

	struct xrt_prober_entry_lists *lists;

	struct u_config_json json;

	/*!
	 * List of created builders. They live until the prober is destroyed:
	 * a builder may own state its system keeps using (the sim_display
	 * builder owns the rig composer).
	 */
	struct xrt_builder **builders;

	/*!
	 * The number of created builders.
	 */
	size_t builder_count;

	enum u_logging_level log_level;
};
