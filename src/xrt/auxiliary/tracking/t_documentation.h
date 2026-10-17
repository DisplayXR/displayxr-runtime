// Copyright 2019-2021, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Documentation-only header.
 * @author Pete Black <pblack@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @ingroup aux_tracking
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif


/*!
 * @defgroup aux_tracking Tracking
 * @ingroup aux
 * @brief IMU orientation fusion (flexkalman) — the one piece of Monado's
 * tracking library a 3D display still needs (handheld devices: orientation
 * and IMU-assisted eye prediction). The camera trackers, calibration and
 * SLAM code that used to live here were removed in #703.
 */

/*!
 * @dir auxiliary/tracking
 * @ingroup aux
 *
 * @brief Trackers, filters and associated helper code.
 */


#ifdef __cplusplus

namespace xrt::auxiliary {
	/*!
	 * @brief Namespace used by C++ interfaces in the auxiliary tracking library code.
	 */
	namespace tracking {
		// Empty
	} // namespace tracking
} // namespace xrt::auxiliary
#endif


#ifdef __cplusplus
}
#endif
