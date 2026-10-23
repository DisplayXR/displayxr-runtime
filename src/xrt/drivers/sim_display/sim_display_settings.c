// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display's settings shim: host iface first, legacy env aliases
 *         second (one release), cached per key. See the header. This is the
 *         one file under drivers/sim_display/ allowed to call getenv
 *         (lint.yml guard).
 * @ingroup drv_sim_display
 */

#include "sim_display_settings.h"

#include "util/u_logging.h"

#include <stdlib.h>
#include <string.h>


/*
 *
 * The declaration (describe_settings): every knob, with the legacy name it
 * replaces. The alias is derived: `SIM_DISPLAY_` + legacy suffix.
 *
 */

struct knob
{
	struct xrt_component_setting_desc desc;
	const char *legacy_suffix; //!< the former SIM_DISPLAY_<suffix> environment name
};

static const struct knob g_knobs[] = {
    // Tracking (ADR-022 / #441): honest "no eye tracking" unless faked.
    {{"TRACKING", XRT_COMPONENT_SETTING_TYPE_BOOL, "0",
      "fake eye tracking: advertise MANUAL_BIT + tracked 3D modes with nominal eyes (was SIM_DISPLAY_FAKE_TRACKING)"},
     "FAKE_TRACKING"},
    {{"TRACKING_PERIOD_MS", XRT_COMPONENT_SETTING_TYPE_INT, "0",
      "square-wave is_tracking with this period to exercise tracking-loss edges; 0 = always tracking"},
     "FAKE_TRACKING_PERIOD_MS"},
    // N6 Part 1 (#2144): a deterministic viewer motion so the target-time eye
    // query (get_eye_positions_at) has something to predict; 0 = static.
    {{"TRACKING_SWAY_MM", XRT_COMPONENT_SETTING_TYPE_FLOAT, "0",
      "fake tracking: sway the viewer sideways by this amplitude (mm) on a closed path; 0 = static"},
     "FAKE_TRACKING_SWAY_MM"},
    {{"TRACKING_SWAY_PERIOD_MS", XRT_COMPONENT_SETTING_TYPE_INT, "4000", "fake tracking: sway period (>= 100)"},
     "FAKE_TRACKING_SWAY_PERIOD_MS"},
    // Panel geometry + output.
    {{"OUTPUT", XRT_COMPONENT_SETTING_TYPE_ENUM, "blend",
      "initial output mode: 2d|passthrough, anaglyph, sbs, squeezed|squeezed_sbs, quad, interlaced, blend"},
     "OUTPUT"},
    {{"FORCE_MODE", XRT_COMPONENT_SETTING_TYPE_INT, "-1",
      "pin the rendering mode index (0=2D 1=Anaglyph 2=Cropped SBS 3=Squeezed SBS 4=Quad) for every app; -1 = off"},
     "FORCE_MODE"},
    {{"WIDTH_M", XRT_COMPONENT_SETTING_TYPE_FLOAT, "0.344", "panel width in metres"}, "WIDTH_M"},
    {{"HEIGHT_M", XRT_COMPONENT_SETTING_TYPE_FLOAT, "0.194", "panel height in metres"}, "HEIGHT_M"},
    {{"NOMINAL_Z_M", XRT_COMPONENT_SETTING_TYPE_FLOAT, "0.60", "nominal viewer distance in metres"}, "NOMINAL_Z_M"},
    {{"PIXEL_W", XRT_COMPONENT_SETTING_TYPE_INT, "1920",
      "panel width in pixels (the real monitor's device pixels when unset, where the platform reports them)"},
     "PIXEL_W"},
    {{"PIXEL_H", XRT_COMPONENT_SETTING_TYPE_INT, "1080", "panel height in pixels (as PIXEL_W)"}, "PIXEL_H"},
    {{"STRICT_PANEL", XRT_COMPONENT_SETTING_TYPE_BOOL, "0",
      "refuse a target whose size differs from the declared panel instead of scaling"},
     "STRICT_PANEL"},
    {{"INTERLACE_PERIOD", XRT_COMPONENT_SETTING_TYPE_INT, "1", "interlaced mode: stripe width in panel pixels"},
     "INTERLACE_PERIOD"},
    {{"METAL_SNAP_PERIOD", XRT_COMPONENT_SETTING_TYPE_INT, "0",
      "macOS Metal DP: snap the present origin to this period in px; 0 = off"},
     "METAL_SNAP_PERIOD"},
    {{"LENS", XRT_COMPONENT_SETTING_TYPE_BOOL, "0",
      "fake a lens: react to the wish mask like a vendor lens would (was SIM_DISPLAY_FAKE_LENS)"},
     "FAKE_LENS"},
    {{"WEAVE_SCOPE", XRT_COMPONENT_SETTING_TYPE_ENUM, "canvas", "weave scope the DP declares: canvas, region, scanout"},
     "WEAVE_SCOPE"},
    // Zones (ADR-027).
    {{"ZONE_GRID", XRT_COMPONENT_SETTING_TYPE_STRING, "1x1", "lens cell grid WxH (1..16 cells) for zone tests"},
     "ZONE_GRID"},
    {{"ZONE_DUMP", XRT_COMPONENT_SETTING_TYPE_BOOL, "0", "dump the per-cell zone decisions to the log"}, "ZONE_DUMP"},
    {{"WISH_QUANTIZE", XRT_COMPONENT_SETTING_TYPE_ENUM, "off", "quantise the wish mask: off, band, cell"},
     "WISH_QUANTIZE"},
    // Lift (ADR-042; Windows D3D11).
    {{"LIFT", XRT_COMPONENT_SETTING_TYPE_BOOL, "0",
      "fake 2D->3D lift on the D3D11 DP (synthetic depth / views) (was SIM_DISPLAY_FAKE_LIFT)"},
     "FAKE_LIFT"},
    {{"LIFT_LATENCY_MS", XRT_COMPONENT_SETTING_TYPE_INT, "8", "fake lift: latency per frame"}, "FAKE_LIFT_LATENCY_MS"},
    // Stereo camera (ADR-043).
    {{"STEREO_CAMERA", XRT_COMPONENT_SETTING_TYPE_BOOL, "0",
      "fake stereo camera source (was SIM_DISPLAY_FAKE_STEREO_CAMERA)"},
     "FAKE_STEREO_CAMERA"},
    {{"STEREO_CAMERA_SIZE", XRT_COMPONENT_SETTING_TYPE_STRING, "640x480", "fake camera: per-eye size WxH"},
     "FAKE_STEREO_CAMERA_SIZE"},
    {{"STEREO_CAMERA_FPS", XRT_COMPONENT_SETTING_TYPE_FLOAT, "30", "fake camera: frame rate"},
     "FAKE_STEREO_CAMERA_FPS"},
    {{"STEREO_CAMERA_ADVERTISED_FPS", XRT_COMPONENT_SETTING_TYPE_FLOAT, "",
      "fake camera: advertised rate (default = FPS)"},
     "FAKE_STEREO_CAMERA_ADVERTISED_FPS"},
    {{"STEREO_CAMERA_BASELINE_MM", XRT_COMPONENT_SETTING_TYPE_FLOAT, "65", "fake camera: baseline (10..300)"},
     "FAKE_STEREO_CAMERA_BASELINE_MM"},
    {{"STEREO_CAMERA_FORMAT", XRT_COMPONENT_SETTING_TYPE_ENUM, "gray8", "fake camera: gray8, nv12, bgra8"},
     "FAKE_STEREO_CAMERA_FORMAT"},
    {{"STEREO_CAMERA_SUSPEND_PERIOD_MS", XRT_COMPONENT_SETTING_TYPE_INT, "0",
      "fake camera: suspend square-wave period; 0 = never"},
     "FAKE_STEREO_CAMERA_SUSPEND_PERIOD_MS"},
    {{"STEREO_CAMERA_DISTORT", XRT_COMPONENT_SETTING_TYPE_BOOL, "0", "fake camera: emit a distorted raw pair"},
     "FAKE_STEREO_CAMERA_DISTORT"},
    {{"STEREO_CAMERA_DY", XRT_COMPONENT_SETTING_TYPE_FLOAT, "0",
      "fake camera: uncalibrated vertical offset of the right eye, px (|dy| <= 20)"},
     "FAKE_STEREO_CAMERA_DY"},
    {{"STEREO_CAMERA_DY_SLOPE", XRT_COMPONENT_SETTING_TYPE_FLOAT, "0",
      "fake camera: vertical offset slope per 100 px (|s| <= 5)"},
     "FAKE_STEREO_CAMERA_DY_SLOPE"},
    // Webcam eye tracking (#1855).
    {{"WEBCAM_TRACKING", XRT_COMPONENT_SETTING_TYPE_BOOL, "0",
      "DEPRECATED (one release): track eyes with a webcam + the face estimator; use sim-tracking SOURCE=webcam"},
     "WEBCAM_TRACKING"},
    {{"WEBCAM_ESTIMATOR", XRT_COMPONENT_SETTING_TYPE_ENUM, "auto",
      "auto (the built-in face estimator), none, or the built-in name (yunet-onnx)"},
     "WEBCAM_ESTIMATOR"},
    {{"WEBCAM_DEVICE", XRT_COMPONENT_SETTING_TYPE_STRING, "",
      "webcam index or a substring of its name (default: the first)"},
     "WEBCAM_DEVICE"},
    {{"WEBCAM_MODEL", XRT_COMPONENT_SETTING_TYPE_STRING, "",
      "face-estimator model path (default: next to the plug-in)"},
     "WEBCAM_MODEL"},
    {{"WEBCAM_HFOV_DEG", XRT_COMPONENT_SETTING_TYPE_FLOAT, "", "webcam horizontal field of view (10..170)"},
     "WEBCAM_HFOV_DEG"},
    {{"WEBCAM_IPD_MM", XRT_COMPONENT_SETTING_TYPE_FLOAT, "", "assumed interpupillary distance (40..90)"},
     "WEBCAM_IPD_MM"},
    {{"WEBCAM_OFFSET_MM", XRT_COMPONENT_SETTING_TYPE_STRING, "", "camera offset from the panel centre, \"x,y,z\" mm"},
     "WEBCAM_OFFSET_MM"},
    {{"WEBCAM_MIRROR", XRT_COMPONENT_SETTING_TYPE_BOOL, "0", "the webcam image is mirrored"}, "WEBCAM_MIRROR"},
};
#define KNOB_COUNT (sizeof(g_knobs) / sizeof(g_knobs[0]))

static struct xrt_component_setting_desc g_descs[KNOB_COUNT];
static bool g_descs_ready = false;

uint32_t
sim_display_describe_settings(const struct xrt_component_setting_desc **out_descs)
{
	if (!g_descs_ready) {
		for (size_t i = 0; i < KNOB_COUNT; i++) {
			g_descs[i] = g_knobs[i].desc;
		}
		g_descs_ready = true;
	}
	if (out_descs != NULL) {
		*out_descs = g_descs;
	}
	return (uint32_t)KNOB_COUNT;
}


/*
 *
 * Lookup, cached per key (the DEBUG_GET_ONCE contract: several of these sit
 * on per-frame paths, and a setting is read at init by contract anyway).
 *
 */

static xrt_component_get_setting_fn_t g_get_setting = NULL;

struct cache_row
{
	bool resolved;
	bool set;
	char value[256];
};
static struct cache_row g_cache[KNOB_COUNT];
static bool g_alias_warned[KNOB_COUNT];

void
sim_display_settings_bind(const struct xrt_plugin_host_iface *host)
{
	g_get_setting = NULL;
	memset(g_cache, 0, sizeof(g_cache));
	if (host == NULL) {
		return;
	}
	// Carved from reserved[]: an older runtime leaves it NULL, and a runtime
	// older than the slot has a struct_size that does not even cover it.
	if (host->struct_size >= offsetof(struct xrt_plugin_host_iface, get_setting) + sizeof(host->get_setting)) {
		g_get_setting = host->get_setting;
	}
}

static int
knob_index(const char *key)
{
	if (key == NULL) {
		return -1;
	}
	for (size_t i = 0; i < KNOB_COUNT; i++) {
		if (strcmp(g_knobs[i].desc.key, key) == 0) {
			return (int)i;
		}
	}
	return -1;
}

static void
copy_str(char *dst, size_t cap, const char *src)
{
	if (cap == 0) {
		return;
	}
	size_t n = strlen(src);
	if (n >= cap) {
		n = cap - 1;
	}
	memcpy(dst, src, n);
	dst[n] = '\0';
}

//! Resolve @p i once: host first, then the legacy alias in this process's environment.
static const struct cache_row *
resolve(int i)
{
	struct cache_row *r = &g_cache[i];
	if (r->resolved) {
		return r;
	}
	r->resolved = true;
	r->set = false;
	r->value[0] = '\0';
	if (g_get_setting != NULL && g_get_setting(SIM_DISPLAY_ID, g_knobs[i].desc.key, r->value, sizeof(r->value)) &&
	    r->value[0] != '\0') {
		r->set = true;
		return r;
	}
	r->value[0] = '\0';
	char legacy[96];
	copy_str(legacy, sizeof(legacy), "SIM_DISPLAY_");
	copy_str(legacy + strlen(legacy), sizeof(legacy) - strlen(legacy), g_knobs[i].legacy_suffix);
	const char *v = getenv(legacy);
	if (v == NULL || v[0] == '\0') {
		return r;
	}
	if (!g_alias_warned[i]) {
		g_alias_warned[i] = true;
		U_LOG_W(
		    "sim-display: %s is deprecated — use 'displayxr-cli dp set sim-display %s %s' (or "
		    "DXR_DP_SIM_DISPLAY_%s in the hosting process's environment); the alias goes away one release "
		    "after N3c (ADR-054 decision 7).",
		    legacy, g_knobs[i].desc.key, v, g_knobs[i].desc.key);
	}
	copy_str(r->value, sizeof(r->value), v);
	r->set = true;
	return r;
}

bool
sim_display_setting(const char *key, char *buf, size_t cap)
{
	if (buf == NULL || cap == 0) {
		return false;
	}
	buf[0] = '\0';
	const int i = knob_index(key);
	if (i < 0) {
		U_LOG_W("sim-display: '%s' is not a declared setting — add it to sim_display_settings.c.",
		        key != NULL ? key : "(null)");
		return false;
	}
	const struct cache_row *r = resolve(i);
	if (!r->set) {
		return false;
	}
	copy_str(buf, cap, r->value);
	return true;
}

bool
sim_display_setting_is_set(const char *key)
{
	const int i = knob_index(key);
	return i >= 0 && resolve(i)->set;
}

bool
sim_display_setting_on(const char *key)
{
	char v[32];
	if (!sim_display_setting(key, v, sizeof(v))) {
		return false;
	}
	return strcmp(v, "0") != 0 && strcmp(v, "false") != 0 && strcmp(v, "no") != 0 && strcmp(v, "off") != 0;
}

long
sim_display_setting_num(const char *key, long def)
{
	char v[64];
	if (!sim_display_setting(key, v, sizeof(v))) {
		return def;
	}
	char *end = NULL;
	const long n = strtol(v, &end, 0);
	return (end == v) ? def : n;
}

float
sim_display_setting_float(const char *key, float def)
{
	char v[64];
	if (!sim_display_setting(key, v, sizeof(v))) {
		return def;
	}
	char *end = NULL;
	const float f = strtof(v, &end);
	return (end == v) ? def : f;
}
