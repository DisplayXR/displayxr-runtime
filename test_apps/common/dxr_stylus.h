// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The cube apps' `--stylus` mode: the graphics-API-agnostic OpenXR half.
 *
 * ADR-034 Amendment 8 (epic #2679). One header every cube test app shares, so
 * each app only adds the drawing:
 *
 *   - the extension: XR_LOGITECH_mx_ink_stylus_interaction is enabled at
 *     xrCreateInstance only when the app was asked for `--stylus` AND the
 *     runtime lists it (dxr::StylusExtensionListed);
 *   - the action set: every input of /interaction_profiles/logitech/
 *     mx_ink_stylus_logitech (tip pose + force, middle-cluster force, front /
 *     back cluster click + double tap, dock, aim + grip pose) and its haptic
 *     output, suggested on both hands (dxr::StylusSetup);
 *   - per frame (dxr::StylusUpdate): sync (together with one more action set,
 *     e.g. the d3d11 / Metal cubes' `--actions` set), read, locate the tip /
 *     aim / grip spaces, and fire a short haptic pulse on a rising edge of the
 *     front cluster click; a throttled INFO progress line through the app's
 *     logger;
 *   - what to draw (dxr::StylusBuildDraws): a box list (tip marker scaled and
 *     coloured by tip force, the barrel, the aim ray) plus a line list (the
 *     aim ray and a three-axis "jack" at the tip, both coloured by pressure).
 *     Apps whose cube shader takes no colour draw the boxes with their own
 *     textured cube and get the pressure colour from the lines, drawn with
 *     their existing coloured grid pipeline (dxr::StylusSegmentModel maps one
 *     segment of the app's grid vertex buffer onto any line — no new pipeline).
 *
 * Matrices are column-major float[16] (m[12..14] = translation), the layout
 * the Metal / GL / Vulkan cubes' mat4 helpers use. A DirectXMath app loads the
 * same 16 floats with XMLoadFloat4x4 and gets its row-vector matrix directly.
 *
 * Header-only, C++ (the cubes are C++ / Objective-C++), no platform calls:
 * every OpenXR entry point it uses is core 1.0, so the loader resolves them.
 * Drive it with the sim-stylus input provider (DXR_INPUT_SIM_STYLUS_ENABLE=1)
 * or any stylus input provider.
 */

#pragma once

#include <openxr/openxr.h>

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Older loader headers may predate the extension; its name is all the apps need.
#ifndef XR_LOGITECH_MX_INK_STYLUS_INTERACTION_EXTENSION_NAME
#define XR_LOGITECH_MX_INK_STYLUS_INTERACTION_EXTENSION_NAME "XR_LOGITECH_mx_ink_stylus_interaction"
#endif

namespace dxr {

//! What the app's logger receives (@p error: an error line, else INFO).
typedef void (*StylusLogFn)(bool error, const char *message);

//! Per-hand state, refreshed by StylusUpdate. Index 0 = left, 1 = right.
struct StylusHand
{
	bool tipValid = false;
	bool aimValid = false;
	bool gripValid = false;
	XrPosef tip = {{0, 0, 0, 1}, {0, 0, 0}};
	XrPosef aim = {{0, 0, 0, 1}, {0, 0, 0}};
	XrPosef grip = {{0, 0, 0, 1}, {0, 0, 0}};
	float tipForce = 0.0f;
	float middleForce = 0.0f;
	bool frontClick = false;
	bool frontDoubleTap = false;
	bool backClick = false;
	bool backDoubleTap = false;
	bool docked = false;
	bool prevFrontClick = false;
};

struct Stylus
{
	//! Optional: where the setup / progress lines go. Null = stderr.
	StylusLogFn log = nullptr;

	XrActionSet actionSet = XR_NULL_HANDLE;
	XrAction tipPose = XR_NULL_HANDLE;
	XrAction aimPose = XR_NULL_HANDLE;
	XrAction gripPose = XR_NULL_HANDLE;
	XrAction tipForce = XR_NULL_HANDLE;
	XrAction middleForce = XR_NULL_HANDLE;
	XrAction frontClick = XR_NULL_HANDLE;
	XrAction frontDoubleTap = XR_NULL_HANDLE;
	XrAction backClick = XR_NULL_HANDLE;
	XrAction backDoubleTap = XR_NULL_HANDLE;
	XrAction docked = XR_NULL_HANDLE;
	XrAction haptic = XR_NULL_HANDLE;
	XrPath handPaths[2] = {XR_NULL_PATH, XR_NULL_PATH};
	XrSpace tipSpaces[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
	XrSpace aimSpaces[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
	XrSpace gripSpaces[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};

	StylusHand hand[2];
	uint64_t syncCount = 0;
	uint64_t hapticsFired = 0;
};

inline void
StylusLog(const Stylus &s, bool error, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (s.log != nullptr) {
		s.log(error, buf);
	} else {
		fprintf(stderr, "%s%s\n", error ? "ERROR: " : "", buf);
	}
}

//! True when @p props (xrEnumerateInstanceExtensionProperties) lists the extension.
inline bool
StylusExtensionListed(const XrExtensionProperties *props, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		if (strcmp(props[i].extensionName, XR_LOGITECH_MX_INK_STYLUS_INTERACTION_EXTENSION_NAME) == 0) {
			return true;
		}
	}
	return false;
}

//! `--stylus` on a command line (argv form) or DXR_STYLUS set to anything but 0.
inline bool
StylusRequested(int argc, const char *const *argv)
{
	for (int i = 1; i < argc; i++) {
		if (argv[i] != nullptr && strcmp(argv[i], "--stylus") == 0) {
			return true;
		}
	}
	const char *e = getenv("DXR_STYLUS");
	return e != nullptr && e[0] != '\0' && e[0] != '0';
}

inline void
StylusCopyName(char *dst, size_t cap, const char *src)
{
	size_t n = strlen(src);
	if (n >= cap) {
		n = cap - 1;
	}
	memcpy(dst, src, n);
	dst[n] = '\0';
}

//! Create the set + actions and suggest the MX Ink bindings (no attach).
inline bool
StylusCreateActions(Stylus &s, XrInstance instance)
{
	XrActionSetCreateInfo setInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
	StylusCopyName(setInfo.actionSetName, sizeof(setInfo.actionSetName), "stylus");
	StylusCopyName(setInfo.localizedActionSetName, sizeof(setInfo.localizedActionSetName), "Stylus");
	if (XR_FAILED(xrCreateActionSet(instance, &setInfo, &s.actionSet))) {
		StylusLog(s, true, "stylus: xrCreateActionSet failed");
		return false;
	}

	xrStringToPath(instance, "/user/hand/left", &s.handPaths[0]);
	xrStringToPath(instance, "/user/hand/right", &s.handPaths[1]);

	struct Def
	{
		XrAction *out;
		XrActionType type;
		const char *name;
		const char *localized;
		const char *binding; // suffix under /user/hand/<side>
	} defs[] = {
	    {&s.tipPose, XR_ACTION_TYPE_POSE_INPUT, "tip_pose", "Tip Pose", "/input/tip_logitech/pose"},
	    {&s.aimPose, XR_ACTION_TYPE_POSE_INPUT, "aim_pose", "Aim Pose", "/input/aim/pose"},
	    {&s.gripPose, XR_ACTION_TYPE_POSE_INPUT, "grip_pose", "Grip Pose", "/input/grip/pose"},
	    {&s.tipForce, XR_ACTION_TYPE_FLOAT_INPUT, "tip_force", "Tip Force", "/input/tip_logitech/force"},
	    {&s.middleForce, XR_ACTION_TYPE_FLOAT_INPUT, "middle_force", "Middle Button Force",
	     "/input/cluster_middle_logitech/force"},
	    {&s.frontClick, XR_ACTION_TYPE_BOOLEAN_INPUT, "front_click", "Front Button",
	     "/input/cluster_front_logitech/click"},
	    {&s.frontDoubleTap, XR_ACTION_TYPE_BOOLEAN_INPUT, "front_double_tap", "Front Button Double Tap",
	     "/input/cluster_front_logitech/double_tap_logitech"},
	    {&s.backClick, XR_ACTION_TYPE_BOOLEAN_INPUT, "back_click", "Back Button",
	     "/input/cluster_back_logitech/click"},
	    {&s.backDoubleTap, XR_ACTION_TYPE_BOOLEAN_INPUT, "back_double_tap", "Back Button Double Tap",
	     "/input/cluster_back_logitech/double_tap_logitech"},
	    {&s.docked, XR_ACTION_TYPE_BOOLEAN_INPUT, "docked", "Docked", "/input/dock_logitech/docked_logitech"},
	    {&s.haptic, XR_ACTION_TYPE_VIBRATION_OUTPUT, "stylus_haptic", "Stylus Haptic", "/output/haptic"},
	};
	const uint32_t defCount = (uint32_t)(sizeof(defs) / sizeof(defs[0]));

	XrActionSuggestedBinding suggested[2 * (sizeof(defs) / sizeof(defs[0]))];
	uint32_t n = 0;
	for (uint32_t i = 0; i < defCount; i++) {
		const Def &d = defs[i];
		XrActionCreateInfo ai = {XR_TYPE_ACTION_CREATE_INFO};
		ai.countSubactionPaths = 2;
		ai.subactionPaths = s.handPaths;
		ai.actionType = d.type;
		StylusCopyName(ai.actionName, sizeof(ai.actionName), d.name);
		StylusCopyName(ai.localizedActionName, sizeof(ai.localizedActionName), d.localized);
		if (XR_FAILED(xrCreateAction(s.actionSet, &ai, d.out))) {
			StylusLog(s, true, "stylus: create action '%s' failed", d.name);
			return false;
		}
		for (int h = 0; h < 2; h++) {
			char full[128];
			snprintf(full, sizeof(full), "%s%s", h == 0 ? "/user/hand/left" : "/user/hand/right", d.binding);
			XrPath p = XR_NULL_PATH;
			xrStringToPath(instance, full, &p);
			suggested[n].action = *d.out;
			suggested[n].binding = p;
			n++;
		}
	}

	XrPath profile = XR_NULL_PATH;
	xrStringToPath(instance, "/interaction_profiles/logitech/mx_ink_stylus_logitech", &profile);
	XrInteractionProfileSuggestedBinding suggest = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
	suggest.interactionProfile = profile;
	suggest.suggestedBindings = suggested;
	suggest.countSuggestedBindings = n;
	const XrResult r = xrSuggestInteractionProfileBindings(instance, &suggest);
	if (XR_FAILED(r)) {
		StylusLog(s, true, "stylus: suggest bindings for mx_ink_stylus_logitech failed (%d)", (int)r);
		return false;
	}
	return true;
}

//! The tip / aim / grip action spaces (after the attach).
inline bool
StylusCreateSpaces(Stylus &s, XrSession session)
{
	for (int h = 0; h < 2; h++) {
		XrActionSpaceCreateInfo si = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
		si.subactionPath = s.handPaths[h];
		si.poseInActionSpace.orientation.w = 1.0f;
		const XrAction actions[3] = {s.tipPose, s.aimPose, s.gripPose};
		XrSpace *outs[3] = {&s.tipSpaces[h], &s.aimSpaces[h], &s.gripSpaces[h]};
		for (int k = 0; k < 3; k++) {
			si.action = actions[k];
			if (XR_FAILED(xrCreateActionSpace(session, &si, outs[k]))) {
				StylusLog(s, true, "stylus: create action space %d (hand %d) failed", k, h);
				return false;
			}
		}
	}
	return true;
}

/*!
 * Create, suggest, attach (together with @p extraSet when it is not
 * XR_NULL_HANDLE — one attach per session) and create the spaces. The caller
 * creates @p extraSet's own spaces after this returns true.
 */
inline bool
StylusSetup(Stylus &s, XrInstance instance, XrSession session, XrActionSet extraSet)
{
	if (!StylusCreateActions(s, instance)) {
		return false;
	}
	XrActionSet sets[2] = {s.actionSet, extraSet};
	XrSessionActionSetsAttachInfo attach = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
	attach.countActionSets = extraSet != XR_NULL_HANDLE ? 2 : 1;
	attach.actionSets = sets;
	if (XR_FAILED(xrAttachSessionActionSets(session, &attach))) {
		StylusLog(s, true, "stylus: xrAttachSessionActionSets failed");
		return false;
	}
	if (!StylusCreateSpaces(s, session)) {
		return false;
	}
	StylusLog(s, false,
	          "stylus: action set attached (logitech/mx_ink_stylus_logitech: tip + aim + grip pose, tip + middle "
	          "force, front / back click + double tap, dock, haptic)%s",
	          extraSet != XR_NULL_HANDLE ? " together with the --actions set" : "");
	return true;
}

inline bool
StylusLocate(XrSpace space, XrSpace base, XrTime t, XrPosef *out)
{
	if (space == XR_NULL_HANDLE) {
		return false;
	}
	XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
	if (XR_FAILED(xrLocateSpace(space, base, t, &loc))) {
		return false;
	}
	const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	if ((loc.locationFlags & needed) != needed) {
		return false;
	}
	*out = loc.pose;
	return true;
}

inline bool
StylusGetBool(XrSession session, XrAction action, XrPath sub)
{
	XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
	gi.action = action;
	gi.subactionPath = sub;
	XrActionStateBoolean st = {XR_TYPE_ACTION_STATE_BOOLEAN};
	return XR_SUCCEEDED(xrGetActionStateBoolean(session, &gi, &st)) && st.isActive && st.currentState == XR_TRUE;
}

inline float
StylusGetFloat(XrSession session, XrAction action, XrPath sub)
{
	XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
	gi.action = action;
	gi.subactionPath = sub;
	XrActionStateFloat st = {XR_TYPE_ACTION_STATE_FLOAT};
	if (XR_SUCCEEDED(xrGetActionStateFloat(session, &gi, &st)) && st.isActive) {
		return st.currentState;
	}
	return 0.0f;
}

/*!
 * Per frame: sync the stylus set (with @p extraSet, if any, in the same
 * xrSyncActions — an app that also syncs @p extraSet on its own should call
 * this after it), read every input, locate the poses in @p base at @p t and
 * buzz on a front-click rising edge.
 */
inline void
StylusUpdate(Stylus &s, XrSession session, XrSpace base, XrTime t, XrActionSet extraSet)
{
	if (s.actionSet == XR_NULL_HANDLE) {
		return;
	}
	XrActiveActionSet active[2] = {{s.actionSet, XR_NULL_PATH}, {extraSet, XR_NULL_PATH}};
	XrActionsSyncInfo syncInfo = {XR_TYPE_ACTIONS_SYNC_INFO};
	syncInfo.countActiveActionSets = extraSet != XR_NULL_HANDLE ? 2 : 1;
	syncInfo.activeActionSets = active;
	const XrResult sr = xrSyncActions(session, &syncInfo);
	if (sr == XR_SESSION_NOT_FOCUSED || XR_FAILED(sr)) {
		// Normal until the session reaches FOCUSED; the pen stays hidden.
		for (int h = 0; h < 2; h++) {
			s.hand[h].tipValid = s.hand[h].aimValid = s.hand[h].gripValid = false;
		}
		return;
	}
	s.syncCount++;

	for (int h = 0; h < 2; h++) {
		StylusHand &hs = s.hand[h];
		const XrPath sub = s.handPaths[h];
		hs.prevFrontClick = hs.frontClick;
		hs.tipValid = hs.aimValid = hs.gripValid = false;
		hs.frontClick = hs.frontDoubleTap = hs.backClick = hs.backDoubleTap = hs.docked = false;
		hs.tipForce = hs.middleForce = 0.0f;

		XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
		gi.action = s.tipPose;
		gi.subactionPath = sub;
		XrActionStatePose ps = {XR_TYPE_ACTION_STATE_POSE};
		if (XR_FAILED(xrGetActionStatePose(session, &gi, &ps)) || !ps.isActive) {
			continue; // no pen on this hand
		}
		hs.tipValid = StylusLocate(s.tipSpaces[h], base, t, &hs.tip);
		hs.aimValid = StylusLocate(s.aimSpaces[h], base, t, &hs.aim);
		hs.gripValid = StylusLocate(s.gripSpaces[h], base, t, &hs.grip);

		hs.tipForce = StylusGetFloat(session, s.tipForce, sub);
		hs.middleForce = StylusGetFloat(session, s.middleForce, sub);
		hs.frontClick = StylusGetBool(session, s.frontClick, sub);
		hs.frontDoubleTap = StylusGetBool(session, s.frontDoubleTap, sub);
		hs.backClick = StylusGetBool(session, s.backClick, sub);
		hs.backDoubleTap = StylusGetBool(session, s.backDoubleTap, sub);
		hs.docked = StylusGetBool(session, s.docked, sub);

		// Rising edge of the front button -> a short buzz on that hand.
		if (hs.frontClick && !hs.prevFrontClick) {
			XrHapticVibration vib = {XR_TYPE_HAPTIC_VIBRATION};
			vib.amplitude = 0.6f;
			vib.frequency = XR_FREQUENCY_UNSPECIFIED;
			vib.duration = 50 * 1000 * 1000; // 50 ms
			XrHapticActionInfo hi = {XR_TYPE_HAPTIC_ACTION_INFO};
			hi.action = s.haptic;
			hi.subactionPath = sub;
			if (XR_SUCCEEDED(xrApplyHapticFeedback(session, &hi, (const XrHapticBaseHeader *)&vib))) {
				s.hapticsFired++;
			}
		}
	}

	// Throttled progress line (~every 2 s at 60 FPS).
	if (s.syncCount % 120 == 1) {
		for (int h = 0; h < 2; h++) {
			const StylusHand &hs = s.hand[h];
			if (!hs.tipValid) {
				continue;
			}
			StylusLog(s, false,
			          "stylus: sync #%llu %s tip=(%.3f,%.3f,%.3f) force=%.2f middle=%.2f docked=%d front=%d/%d "
			          "back=%d/%d aim=%d grip=%d haptics=%llu",
			          (unsigned long long)s.syncCount, h == 0 ? "L" : "R", hs.tip.position.x, hs.tip.position.y,
			          hs.tip.position.z, hs.tipForce, hs.middleForce, hs.docked, hs.frontClick, hs.frontDoubleTap,
			          hs.backClick, hs.backDoubleTap, hs.aimValid, hs.gripValid,
			          (unsigned long long)s.hapticsFired);
		}
	}
}

//! Destroy the spaces and the set (the instance would, too; this is for tidy shutdowns).
inline void
StylusDestroy(Stylus &s)
{
	for (int h = 0; h < 2; h++) {
		XrSpace *sp[3] = {&s.tipSpaces[h], &s.aimSpaces[h], &s.gripSpaces[h]};
		for (int k = 0; k < 3; k++) {
			if (*sp[k] != XR_NULL_HANDLE) {
				xrDestroySpace(*sp[k]);
				*sp[k] = XR_NULL_HANDLE;
			}
		}
	}
	if (s.actionSet != XR_NULL_HANDLE) {
		xrDestroyActionSet(s.actionSet);
		s.actionSet = XR_NULL_HANDLE;
	}
}

// ---------------------------------------------------------------------------
// What to draw.
// ---------------------------------------------------------------------------

//! Tip colour: green (hovering) -> yellow -> red (full pressure); blue while docked.
inline void
StylusTipColor(float force, bool docked, float out[4])
{
	out[3] = 1.0f;
	if (docked) {
		out[0] = 0.2f, out[1] = 0.4f, out[2] = 1.0f;
		return;
	}
	const float f = force < 0.0f ? 0.0f : (force > 1.0f ? 1.0f : force);
	if (f <= 0.0f) {
		out[0] = 0.1f, out[1] = 0.9f, out[2] = 0.2f;
		return;
	}
	out[0] = f < 0.5f ? 0.6f + 0.8f * f : 1.0f;
	out[1] = f < 0.5f ? 0.9f : 0.9f * (1.0f - (f - 0.5f) * 2.0f);
	out[2] = 0.1f;
}

//! Column-major model matrix (R | t) from a pose.
inline void
StylusPoseMatrix(const XrPosef &p, float m[16])
{
	const float x = p.orientation.x, y = p.orientation.y, z = p.orientation.z, w = p.orientation.w;
	m[0] = 1 - 2 * (y * y + z * z), m[1] = 2 * (x * y + w * z), m[2] = 2 * (x * z - w * y), m[3] = 0;
	m[4] = 2 * (x * y - w * z), m[5] = 1 - 2 * (x * x + z * z), m[6] = 2 * (y * z + w * x), m[7] = 0;
	m[8] = 2 * (x * z + w * y), m[9] = 2 * (y * z - w * x), m[10] = 1 - 2 * (x * x + y * y), m[11] = 0;
	m[12] = p.position.x, m[13] = p.position.y, m[14] = p.position.z, m[15] = 1;
}

//! out = a * b (column-major; out may not alias).
inline void
StylusMatMul(const float a[16], const float b[16], float out[16])
{
	for (int c = 0; c < 4; c++) {
		for (int r = 0; r < 4; r++) {
			float v = 0.0f;
			for (int k = 0; k < 4; k++) {
				v += a[k * 4 + r] * b[c * 4 + k];
			}
			out[c * 4 + r] = v;
		}
	}
}

//! pose * translate(t) * scale(s): a box in the pose's frame.
inline void
StylusBoxMatrix(const XrPosef &pose, const float t[3], const float s[3], float out[16])
{
	float pm[16];
	StylusPoseMatrix(pose, pm);
	float ts[16] = {s[0], 0, 0, 0, 0, s[1], 0, 0, 0, 0, s[2], 0, t[0], t[1], t[2], 1};
	StylusMatMul(pm, ts, out);
}

//! Point @p local (in the pose's frame) in the base frame.
inline void
StylusPosePoint(const XrPosef &pose, const float local[3], float out[3])
{
	float pm[16];
	StylusPoseMatrix(pose, pm);
	for (int r = 0; r < 3; r++) {
		out[r] = pm[r] * local[0] + pm[4 + r] * local[1] + pm[8 + r] * local[2] + pm[12 + r];
	}
}

/*!
 * An affine (rank-1) model matrix that maps the segment @p a -> @p b of an
 * existing line vertex buffer onto @p p0 -> @p p1, so an app draws an
 * arbitrary coloured line by re-drawing those two vertices with its grid
 * pipeline: M x = p0 + (p1 - p0) * dot(b - a, x - a) / |b - a|^2.
 */
inline void
StylusSegmentModel(const float a[3], const float b[3], const float p0[3], const float p1[3], float out[16])
{
	const float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
	float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
	if (dd <= 0.0f) {
		dd = 1.0f;
	}
	const float e[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
	for (int c = 0; c < 3; c++) {
		for (int r = 0; r < 3; r++) {
			out[c * 4 + r] = e[r] * d[c] / dd;
		}
		out[c * 4 + 3] = 0.0f;
	}
	for (int r = 0; r < 3; r++) {
		const float ma = out[0 * 4 + r] * a[0] + out[1 * 4 + r] * a[1] + out[2 * 4 + r] * a[2];
		out[12 + r] = p0[r] - ma;
	}
	out[15] = 1.0f;
}

enum StylusBoxKind
{
	STYLUS_BOX_TIP = 0,
	STYLUS_BOX_BARREL = 1,
	STYLUS_BOX_RAY = 2,
};

struct StylusBox
{
	StylusBoxKind kind;
	float model[16]; //!< maps the unit box (-0.5 .. 0.5) into the base space
	float color[4];  //!< for apps whose cube path takes a colour
};

struct StylusLine
{
	float p0[3];
	float p1[3];
	float color[4];
};

struct StylusDraws
{
	StylusBox boxes[6];
	int boxCount = 0;
	StylusLine lines[8];
	int lineCount = 0;
};

/*!
 * The pen, for every hand that holds one: the tip marker (grows with tip
 * force, coloured by it, blue while docked), the barrel (+Z of the tip pose
 * is toward the back of the pen) and the 12 cm aim ray (-Z of the aim pose);
 * the lines repeat the ray and add a three-axis jack at the tip, both in the
 * pressure colour. @p cubeExtent: the edge length of the app's cube vertex
 * buffer in model units (1 for -0.5..0.5, 2 for -1..1), so the boxes come
 * out at the same physical size on every app.
 */
inline void
StylusBuildDraws(const Stylus &s, float cubeExtent, StylusDraws &out)
{
	out.boxCount = 0;
	out.lineCount = 0;
	const float k = cubeExtent > 0.0f ? 1.0f / cubeExtent : 1.0f;
	for (int h = 0; h < 2; h++) {
		const StylusHand &hs = s.hand[h];
		if (!hs.tipValid) {
			continue;
		}
		float color[4];
		StylusTipColor(hs.tipForce, hs.docked, color);
		const float sz = 0.006f + 0.008f * hs.tipForce;

		StylusBox &tip = out.boxes[out.boxCount++];
		tip.kind = STYLUS_BOX_TIP;
		{
			const float t[3] = {0, 0, 0};
			const float sc[3] = {sz * k, sz * k, sz * k};
			StylusBoxMatrix(hs.tip, t, sc, tip.model);
		}
		memcpy(tip.color, color, sizeof(color));

		StylusBox &barrel = out.boxes[out.boxCount++];
		barrel.kind = STYLUS_BOX_BARREL;
		{
			const float t[3] = {0, 0, 0.07f + 0.5f * sz};
			const float sc[3] = {0.008f * k, 0.008f * k, 0.14f * k};
			StylusBoxMatrix(hs.tip, t, sc, barrel.model);
		}
		barrel.color[0] = 0.75f, barrel.color[1] = 0.75f, barrel.color[2] = 0.78f, barrel.color[3] = 1.0f;

		// Jack at the tip: three short lines along the tip frame's axes.
		const float j = sz + 0.006f;
		for (int axis = 0; axis < 3; axis++) {
			float a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
			a[axis] = -j;
			b[axis] = j;
			StylusLine &l = out.lines[out.lineCount++];
			StylusPosePoint(hs.tip, a, l.p0);
			StylusPosePoint(hs.tip, b, l.p1);
			memcpy(l.color, color, sizeof(color));
		}

		if (hs.aimValid) {
			StylusBox &ray = out.boxes[out.boxCount++];
			ray.kind = STYLUS_BOX_RAY;
			{
				const float t[3] = {0, 0, -0.06f};
				const float sc[3] = {0.0015f * k, 0.0015f * k, 0.12f * k};
				StylusBoxMatrix(hs.aim, t, sc, ray.model);
			}
			memcpy(ray.color, color, sizeof(color));

			const float a[3] = {0, 0, 0}, b[3] = {0, 0, -0.12f};
			StylusLine &l = out.lines[out.lineCount++];
			StylusPosePoint(hs.aim, a, l.p0);
			StylusPosePoint(hs.aim, b, l.p1);
			memcpy(l.color, color, sizeof(color));
		}
	}
}

} // namespace dxr
