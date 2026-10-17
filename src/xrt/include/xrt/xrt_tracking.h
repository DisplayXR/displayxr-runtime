// Copyright 2019, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Header defining the tracking system integration in Monado.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup xrt_iface
 */

#pragma once

#define XRT_TRACKING_NAME_LEN 256
#define XRT_TRACKING_MAX_SLAM_CAMS 5

#include "xrt/xrt_defines.h"


#ifdef __cplusplus
extern "C" {
#endif


struct time_state;
struct xrt_device;
struct xrt_tracking;

//! @todo This is from u_time, duplicated to avoid layer violation.
typedef int64_t timepoint_ns;

/*!
 * @addtogroup xrt_iface
 * @{
 */

/*!
 * What kind of tracking system is this.
 *
 * @todo Is none, Colour, IR, Magnetic the kind of type we need to know about?
 */
enum xrt_tracking_type
{
	// The device(s) are never tracked.
	XRT_TRACKING_TYPE_NONE,

	// The device(s) are tracked by RGB camera(s).
	XRT_TRACKING_TYPE_RGB,

	// The device(s) are tracked by Ligthhouse
	XRT_TRACKING_TYPE_LIGHTHOUSE,

	// The device(s) are tracked by magnetic fields, eg. Razer Hydra.
	XRT_TRACKING_TYPE_MAGNETIC,

	// The device(s) are tracked by external SLAM
	XRT_TRACKING_TYPE_EXTERNAL_SLAM,

	// The device(s) are tracked by other methods.
	XRT_TRACKING_TYPE_OTHER,

	/*!
	 * The device's poses are DISPLAY-PLANE-RELATIVE (rig-local): origin at the
	 * display centre, +X right, +Y up, +Z toward the viewer, metres. The runtime
	 * anchors this origin at the initial rig (u_space_overseer::rig_initial), so
	 * world = rig(t) o pose with no standing-height or mount knowledge in the
	 * provider. New providers use this; TYPE_OTHER keeps today's stage-anchored
	 * behaviour (ultraleap's ul_mount_offset_default {0, 1.45, -0.10} stays).
	 */
	XRT_TRACKING_TYPE_RIG_LOCAL,
};

/*!
 * A tracking system or device origin.
 *
 * Tracking systems will typically extend this structure.
 */
struct xrt_tracking_origin
{
	//! For debugging.
	char name[XRT_TRACKING_NAME_LEN];

	//! What can the state tracker expect from this tracking system.
	enum xrt_tracking_type type;

	/*!
	 * Read-only. Set once by the builder, or in some cases, the driver.
	 * With some systems, the offset may be changed at runtime using the space overseer.
	 */
	struct xrt_pose initial_offset;
};

/*!
 * IMU Sample.
 */
struct xrt_imu_sample
{
	timepoint_ns timestamp_ns;
	struct xrt_vec3_f64 accel_m_s2;
	struct xrt_vec3_f64 gyro_rad_secs;
};

/*!
 * Pose sample.
 */
struct xrt_pose_sample
{
	timepoint_ns timestamp_ns;
	struct xrt_pose pose;
};

/*!
 * Masks (bounding boxes) of different hands from current views
 */
struct xrt_hand_masks_sample
{
	struct xrt_hand_masks_sample_camera
	{
		bool enabled; //!< Whether any hand mask for this camera is being reported
		struct xrt_hand_masks_sample_hand
		{
			bool enabled;             //!< Whether a mask for this hand is being reported
			struct xrt_rect_f32 rect; //!< The mask itself in pixel coordinates
		} hands[2];
	} views[XRT_TRACKING_MAX_SLAM_CAMS];
};

/*!
 * @interface xrt_imu_sink
 *
 * An object to send IMU samples to.
 *
 * Similar to @ref xrt_frame_sink but the interface implementation must manage
 * its own resources, not through a context graph.
 */
struct xrt_imu_sink
{
	/*!
	 * Push an IMU sample into the sink
	 */
	void (*push_imu)(struct xrt_imu_sink *, struct xrt_imu_sample *sample);
};

/*!
 * @interface xrt_pose_sink
 *
 * An object to send pairs of timestamps and poses to. @see xrt_imu_sink.
 */
struct xrt_pose_sink
{
	void (*push_pose)(struct xrt_pose_sink *, struct xrt_pose_sample *sample);
};

/*!
 * @interface xrt_hand_masks_sink
 *
 * An object to push @ref xrt_hand_masks_sample to.
 */
struct xrt_hand_masks_sink
{
	void (*push_hand_masks)(struct xrt_hand_masks_sink *, struct xrt_hand_masks_sample *hand_masks);
};


/*!
 * Container of pointers to sinks that could be used for a SLAM system. Sinks
 * are considered disabled if they are null.
 */
struct xrt_slam_sinks
{
	int cam_count;
	struct xrt_frame_sink *cams[XRT_TRACKING_MAX_SLAM_CAMS];
	struct xrt_imu_sink *imu;
	struct xrt_pose_sink *gt; //!< Can receive ground truth poses if available
	struct xrt_hand_masks_sink *hand_masks;
};

/*
 *
 * Helper functions.
 *
 */

//! @public @memberof xrt_imu_sink
static inline void
xrt_sink_push_imu(struct xrt_imu_sink *sink, struct xrt_imu_sample *sample)
{
	sink->push_imu(sink, sample);
}

//! @public @memberof xrt_pose_sink
static inline void
xrt_sink_push_pose(struct xrt_pose_sink *sink, struct xrt_pose_sample *sample)
{
	sink->push_pose(sink, sample);
}

//! @public @memberof xrt_hand_masks_sink
static inline void
xrt_sink_push_hand_masks(struct xrt_hand_masks_sink *sink, struct xrt_hand_masks_sample *hand_masks)
{
	sink->push_hand_masks(sink, hand_masks);
}

/*!
 * @}
 */


#ifdef __cplusplus
}
#endif
