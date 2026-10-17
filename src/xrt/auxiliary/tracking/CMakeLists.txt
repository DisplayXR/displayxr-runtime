# Copyright 2019-2022, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0

xrt_optimized_math_flags()

# IMU orientation fusion (flexkalman). Kept for handheld 3D displays
# (orientation + IMU-assisted eye prediction, #703). The camera-tracker,
# calibration, PSMV/PSVR, SLAM/VIT and OpenVR pieces that used to live here
# were Monado leftovers no 3D display can use and were removed in #703.
add_library(
	aux_tracking STATIC
	t_imu_fusion.hpp
	t_imu.cpp
	t_imu.h
	)
target_link_libraries(
	aux_tracking
	PUBLIC aux-includes
	PRIVATE
		aux_math
		aux_util
		xrt-external-flexkalman
		xrt-optimized-math
	)
