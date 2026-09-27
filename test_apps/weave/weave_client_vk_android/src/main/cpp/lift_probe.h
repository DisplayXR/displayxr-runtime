// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
//
// XR_DXR_lift probe for weave_client_vk_android (ADR-042, Android step 1) —
// the fastest on-device proof of the Android lift path without a browser.
// Off unless `adb shell setprop debug.dxr.lift.probe <mode>`:
//
//   weave     rect 0 of every weave submit carries a 2D frame and is flagged
//             XrWeaveSubmitLiftRectsDXR (an SBS stream): the service snapshots
//             it, converts it on the lift thread, and weaves the latest result
//             at the rect's position — flat until the first result.
//   explicit  a separate CPU-painted 2D AHardwareBuffer goes through
//             xrSubmitLiftFrameDXR + xrAcquireLiftResultDXR every frame; the
//             probe logs per-result latency (service + caller side) and drops.
//   both      both at once (two streams).
//   depth     the explicit path on a DEPTH stream; the first result is locked
//             on the CPU (AHardwareBuffer_lock) and checksummed.
//
// `debug.dxr.lift.probe_frames N` (default 300): after N explicit submits the
// probe logs a SUMMARY line and stops submitting (the weave mode keeps going).
// Everything logs under the weave_client_vk_android tag, prefixed LIFT_PROBE.

#pragma once

#include <openxr/openxr.h>
#include <openxr/XR_DXR_lift.h>

#include <stdint.h>

//! The probe mode from debug.dxr.lift.probe, read once. False = probe off.
bool
lift_probe_enabled();

//! Resolve the XR_DXR_lift entry points (the extension must be enabled). False
//! = probe disabled.
bool
lift_probe_init(XrInstance instance, XrSession session);

//! Once per frame, before the weave submit: properties, stream creation,
//! explicit submit/acquire, stats.
void
lift_probe_frame(uint64_t frame);

//! The weave-mode stream to flag rect 0 with, or XR_NULL_HANDLE (not ready /
//! not in weave mode).
XrLiftStreamDXR
lift_probe_weave_stream();

void
lift_probe_shutdown();
