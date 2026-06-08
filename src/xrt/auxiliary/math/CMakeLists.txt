# Copyright 2019-2022, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0

xrt_optimized_math_flags()

# #396 W7: the Kooima rig math (formerly the hand-synced m_display3d_view /
# m_camera3d_view / m_multiview ports) comes from displayxr-common's
# type-neutral core via the xrt-typed FOV-only wrapper displayxr::math_xrt.
# DISPLAYXR_BUILD_MATH_OPENXR=OFF keeps the fetch OpenXR-free (no OpenXR-SDK
# headers, no displayxr-extensions fetch) — the runtime provides only its own
# xrt headers via DISPLAYXR_XRT_INCLUDE_DIR. Because apps/engines link the
# same core through the OpenXR-typed wrapper, runtime render-ready views and
# app-from-raw views are the same function by construction (XR_EXT_view_rig
# equivalence guarantee, docs/adr/ADR-024-raw-vs-render-ready-views.md).
include(FetchContent)
set(DISPLAYXR_BUILD_MATH_OPENXR OFF CACHE BOOL "" FORCE)
set(DISPLAYXR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
get_filename_component(_dxr_xrt_includes "${CMAKE_CURRENT_SOURCE_DIR}/../../include" ABSOLUTE)
set(DISPLAYXR_XRT_INCLUDE_DIR "${_dxr_xrt_includes}" CACHE PATH "" FORCE)
FetchContent_Declare(
	displayxr_common
	GIT_REPOSITORY https://github.com/DisplayXR/displayxr-common.git
	GIT_TAG v0.4.0
	GIT_SHALLOW TRUE
	)
FetchContent_MakeAvailable(displayxr_common)

add_library(
	aux_math STATIC
	m_api.h
	m_base.cpp
	m_clock_tracking.c
	m_clock_tracking.h
	m_documentation.hpp
	m_eigen_interop.hpp
	m_filter_fifo.c
	m_filter_fifo.h
	m_filter_one_euro.c
	m_filter_one_euro.h
	m_hash.cpp
	m_imu_3dof.c
	m_imu_3dof.h
	m_imu_pre.c
	m_imu_pre.h
	m_lowpass_float.cpp
	m_lowpass_float.h
	m_lowpass_float.hpp
	m_lowpass_float_vector.hpp
	m_lowpass_integer.cpp
	m_lowpass_integer.h
	m_lowpass_integer.hpp
	m_matrix_2x2.h
	m_matrix_4x4_f64.h
	m_matrix_projection.cpp
	m_optics.c
	m_permutation.c
	m_permutation.h
	m_predict.c
	m_predict.h
	m_quatexpmap.cpp
	m_rational.hpp
	m_relation_history.cpp
	m_relation_history.h
	m_space.cpp
	m_space.h
	m_vec2.h
	m_vec3.h
	)
target_link_libraries(
	aux_math
	PUBLIC aux-includes aux_util Eigen3::Eigen displayxr::math_xrt
	PRIVATE xrt-optimized-math
	)

if(MSVC)
	get_target_property(options aux_math COMPILE_OPTIONS)
	message(STATUS "COMPILE_OPTIONS: ${options}")
endif()
