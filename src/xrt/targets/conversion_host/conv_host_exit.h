// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-conversion-host exit codes (ADR-053 roadmap §3.4).
 *
 * The table itself is the wire header's (conv_host_wire.h, shared with the
 * supervisor): 0 orderly, 10 ABI reject (also: hello refused), 11 negotiate /
 * describe failed, 12 licence, 13 no usable device, 14 elevated, 20 closed for
 * update, 30 watchdog (supervisor-side). Two host-private codes are added
 * here for failures that happen before there is anyone to talk to; the
 * supervisor counts them as crashes, which is right: the host could not even
 * start.
 *
 * @ingroup conv_host
 */

#pragma once

#include "conversion/conv_host_wire.h"

#define CONV_HOST_EXIT_USAGE 2    //!< bad command line (the supervisor never spawns that)
#define CONV_HOST_EXIT_CHANNEL 15 //!< the inherited control channel is unusable

//! Short name of a host exit code, for logs ("crash" for anything unlisted).
static inline const char *
conv_host_exit_str(int code)
{
	switch (code) {
	case CONV_HOST_EXIT_ORDERLY: return "orderly";
	case CONV_HOST_EXIT_USAGE: return "usage";
	case CONV_HOST_EXIT_ABI_REJECT: return "abi-reject";
	case CONV_HOST_EXIT_NEGOTIATE_FAILED: return "negotiate-failed";
	case CONV_HOST_EXIT_LICENCE: return "licence";
	case CONV_HOST_EXIT_NO_DEVICE: return "no-device";
	case CONV_HOST_EXIT_ELEVATED: return "elevated";
	case CONV_HOST_EXIT_CHANNEL: return "channel";
	case CONV_HOST_EXIT_UPDATE: return "closed-for-update";
	case CONV_HOST_EXIT_WATCHDOG: return "watchdog";
	default: return "crash";
	}
}
