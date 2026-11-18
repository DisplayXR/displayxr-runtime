// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Why a windowless display processor was not made: the closed tokens
 *         shared by the per-API makers (comp_dp_windowless_d3d11.h,
 *         comp_dp_windowless_d3d12.h).
 *
 * @ingroup comp_util
 */

#pragma once

/*!
 * @name Why no windowless DP was made — closed tokens
 * @{
 */
//! No registry entry, no per-screen factory for the API, or no screen rect to phase against.
#define COMP_DP_WINDOWLESS_NO_FACTORY "no_per_screen_factory"
//! The plug-in did not hint windowless phase for this screen.
#define COMP_DP_WINDOWLESS_NO_HINT "no_windowless_hint"
//! The per-screen factory refused (or returned no DP).
#define COMP_DP_WINDOWLESS_REFUSED "factory_refused"
//! The DP was made but does not declare PHASE_FROM_PRESENT_ORIGIN; it was destroyed.
#define COMP_DP_WINDOWLESS_NO_FLAG "no_phase_from_present_origin"
/*! @} */

