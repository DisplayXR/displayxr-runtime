// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `displayxr-cli camera` — XR_DXR_stereo_camera (ADR-043) over IPC.
 *
 *   camera list  [--json]                         properties + state of every camera
 *   camera calib <id> [--raw|--rectified] [--json]
 *   camera probe [<id>] [--raw|--rectified] [--format gray8|nv12|bgra8] [--fps F]
 *                [--frames N] [--seconds S] [--out DIR]
 *                [--max-disparity N] [--max-dy N] [--min-ncc C]
 *
 * R3 consent / privacy (ADR-043 §7) — the store is edited LOCALLY (no service
 * needed), the control ops go over IPC as a DIAG client:
 *   camera consent                               list the store (apps, delegating, sharing)
 *   camera allow|deny|revoke <exe>|--self        stored per-app decision
 *   camera trust|untrust <exe>|--self            user-level consent-DELEGATING list
 *   camera status                                sharing / locked / started streams (IPC)
 *   camera stop-all                              the user kill switch (IPC)
 *   camera sharing on|off                        the persistent sharing toggle (IPC)
 *   camera fake-lock on|off                      simulate the OS session lock (IPC, diagnostics)
 *
 * `list` / `calib` / `probe` connect as the CAMERA_CONSUMER class (ADR-043 R3)
 * — the same class a browser's video-capture utility declares — so the CLI
 * exercises exactly the consumer path: it is prompted / refused / allowed like
 * any app. `probe` is a real consumer: it creates and starts a stream (so it
 * goes through the service's consent decision: a refusal prints the distinct
 * result and exits 4 / 6 / 7), maps the ring READ-ONLY, waits on the per-stream wake
 * handle, acquires, and reports the measured delivery rate, frame-index gaps,
 * the SBS layout, block-matched disparities of the last frame (the sim fake's
 * scene is 12 px background / 40 px bar), and the service's stream stats.
 * With --out it writes the last frame as cam_<frameIndex>.png.
 *
 * R2: every probe also reports ROW ALIGNMENT — 2-D block matching (dx AND dy)
 * of the last frame on textured 32x32 blocks: the vertical disparity a
 * rectified pair must bring to ~0 (median / p90 / max |dy|) and the dominant
 * horizontal disparities, each converted to a depth Z = f * B / d with the
 * RECTIFIED calibration. `--rectified` insists on RECTIFIED output and fails
 * (exit 5) if the frames came back RAW-flagged or the rows do not align:
 * |SIGNED median dy| > 0.5 px (a constant vertical offset), a robust spread
 * (MAD about that median) > 1.5 px, or fewer than 20 accepted blocks. The
 * unsigned |dy| statistics are printed but not gated (block-match noise). Matching is zero-mean NCC with sub-pixel refinement; the
 * disparity window comes from the camera's own f * B (a subject at 0.4 m),
 * the vertical one is +-24 px, blocks under NCC 0.90 or peaking ON either
 * bound are counted and excluded (a bound hit is a clamp, not a measurement —
 * the first Leia SR run read "d = 64, |dy| = 10.000" that way). On the
 * distorted sim fake
 * (SIM_DISPLAY_FAKE_STEREO_CAMERA_DISTORT=1) the depths read back 2.00 m
 * (background) and 0.60 m (bar); `--raw` shows the misalignment it fixes.
 *
 * The service stats line is followed by the ONLINE VERTICAL REFINEMENT state
 * (R2): applied correction a / b, match count, and the signed dy of its first
 * (uncorrected) window next to its latest one and this probe's own last-frame
 * measurement. It settles in ~1 s after a stream starts, so give the probe a
 * few seconds (the default 90 frames is ~3 s at 30 Hz). On the fake, inject a
 * calibration error with SIM_DISPLAY_FAKE_STEREO_CAMERA_DY=2.6 and
 * _DY_SLOPE=-0.47 (px per 100 px) next to _DISTORT=1.
 *
 * Connects as a DIAG client, like `clients`: on Windows run it NON-elevated.
 */

#include "cli_common.h"

#ifdef CLI_HAVE_IPC

#include "xrt/xrt_instance.h"
#include "util/u_camera_consent.h"
#include "xrt/xrt_plugin.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_stereo_camera.h"
#include "os/os_time.h"
#include "util/u_logging.h"
#include "util/u_stereo_camera.h"

#include "client/ipc_client_connection.h"
#include "client/ipc_client.h"
#include "client/ipc_client_stereo_camera.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#include <windows.h>
#else
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
#ifdef XRT_OS_MACOS
#include <mach-o/dyld.h>
#endif

static const char *
state_str(uint32_t s)
{
	switch (s) {
	case XRT_STEREO_CAMERA_STATE_AVAILABLE: return "AVAILABLE";
	case XRT_STEREO_CAMERA_STATE_WAITING: return "WAITING";
	case XRT_STEREO_CAMERA_STATE_SUSPENDED: return "SUSPENDED";
	case XRT_STEREO_CAMERA_STATE_UNAVAILABLE: return "UNAVAILABLE";
	default: return "?";
	}
}

static const char *
format_str(uint32_t f)
{
	switch (f) {
	case XRT_STEREO_CAMERA_FORMAT_GRAY8: return "GRAY8";
	case XRT_STEREO_CAMERA_FORMAT_NV12: return "NV12";
	case XRT_STEREO_CAMERA_FORMAT_BGRA8: return "BGRA8";
	default: return "?";
	}
}

static void
flags_str(uint64_t f, char *out, size_t cap)
{
	out[0] = '\0';
	static const struct
	{
		uint64_t bit;
		const char *name;
	} k[] = {
	    {XRT_PLUGIN_STEREO_CAMERA_SHARED_WITH_EYE_TRACKING, "SHARED_WITH_EYE_TRACKING"},
	    {XRT_PLUGIN_STEREO_CAMERA_USER_FACING, "USER_FACING"},
	    {XRT_PLUGIN_STEREO_CAMERA_CALIBRATED, "CALIBRATED"},
	    {XRT_PLUGIN_STEREO_CAMERA_NATIVELY_RECTIFIED, "NATIVELY_RECTIFIED"},
	    {XRT_PLUGIN_STEREO_CAMERA_MONOCHROME, "MONOCHROME"},
	};
	for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
		if (f & k[i].bit) {
			size_t n = strlen(out);
			snprintf(out + n, cap - n, "%s%s", n ? "|" : "", k[i].name);
		}
	}
}

static const char *
result_hint(xrt_result_t x)
{
	switch (x) {
	case XRT_ERROR_NOT_AUTHORIZED:
		return " = NOT_AUTHORIZED (OS camera privacy switch off, RAW to a delegating client, or wrong client class)";
	case XRT_ERROR_STEREO_CAMERA_CONSENT_REFUSED:
		return " = CONSENT_REFUSED (no consent for this executable: the prompt was refused / unavailable, or a "
		       "stored Deny; `displayxr-cli camera allow --self` or `camera trust --self` grants it)";
	case XRT_ERROR_STEREO_CAMERA_DISABLED:
		return " = DISABLED (camera sharing is off: tray toggle or DXR_STEREO_CAMERA=0)";
	case XRT_ERROR_STEREO_CAMERA_BUSY: return " = BUSY (the source cannot be opened right now; retry)";
	case XRT_ERROR_STEREO_CAMERA_STREAM_ENDED:
		return " = STREAM_ENDED (the service ended this stream: user stop / sharing off)";
	case XRT_ERROR_FEATURE_NOT_SUPPORTED: return " = FEATURE_NOT_SUPPORTED";
	case XRT_ERROR_INPUT_UNSUPPORTED: return " = INPUT_UNSUPPORTED (bad id / format / output)";
	case XRT_ERROR_CLIENT_LIMIT_REACHED: return " = LIMIT_REACHED (8 streams per camera)";
	case XRT_ERROR_IPC_FAILURE: return " = IPC_FAILURE";
	default: return "";
	}
}

//! Exit code for a refused start, so a script can tell the refusals apart.
static int
refusal_exit_code(xrt_result_t x)
{
	switch (x) {
	case XRT_ERROR_STEREO_CAMERA_CONSENT_REFUSED: return 4;
	case XRT_ERROR_STEREO_CAMERA_DISABLED: return 6;
	case XRT_ERROR_STEREO_CAMERA_BUSY: return 7;
	default: return 2;
	}
}

static bool
cam_connect(struct ipc_connection *ipc_c, uint32_t client_class)
{
	struct xrt_instance_info ii = {0};
	snprintf(ii.app_info.application_name, sizeof(ii.app_info.application_name), "%s", "displayxr-cli");
	ii.app_info.declared_client_class = client_class;
	xrt_result_t xret = ipc_client_connection_init(ipc_c, U_LOGGING_ERROR, &ii);
	if (xret != XRT_SUCCESS) {
		printf("displayxr-cli camera: not connected to the service (xrt_result=%d).\n", (int)xret);
		printf("  Is displayxr-service running? On Windows, run from a NON-elevated prompt.\n");
		return false;
	}
	return true;
}


/*
 *
 * list / calib.
 *
 */

static int
cmd_list(struct ipc_connection *ipc_c, bool json)
{
	uint32_t n = 0;
	xrt_result_t xret = ipc_client_stereo_camera_count(ipc_c, &n);
	if (xret != XRT_SUCCESS) {
		printf("stereo_camera_count failed: %d%s\n", (int)xret, result_hint(xret));
		return 2;
	}
	if (json) {
		printf("{\"cameras\": [");
	} else {
		printf("%u stereo camera(s)\n", n);
	}
	for (uint32_t i = 0; i < n; i++) {
		struct xrt_stereo_camera_properties p;
		xret = ipc_client_stereo_camera_get_properties(ipc_c, i, &p);
		if (xret != XRT_SUCCESS) {
			continue;
		}
		char flags[160];
		flags_str(p.flags, flags, sizeof(flags));
		if (json) {
			printf(
			    "%s{\"id\": %llu, \"name\": \"%s\", \"persistentId\": \"%s\", \"state\": \"%s\", "
			    "\"flags\": \"%s\", \"viewCount\": %u, \"eyeWidth\": %u, \"eyeHeight\": %u, "
			    "\"maxFrameRate\": %.2f, \"baselineMm\": %.2f, \"horizontalFovDeg\": %.2f, "
			    "\"formats\": \"0x%llx\", \"transports\": \"0x%llx\", \"platformDeviceHint\": \"%s\"}",
			    i ? ", " : "", (unsigned long long)p.camera_id, p.display_name, p.persistent_id,
			    state_str(p.state), flags, p.view_count, p.eye_width, p.eye_height, p.max_frame_rate,
			    p.baseline_mm, p.horizontal_fov_deg, (unsigned long long)p.supported_formats,
			    (unsigned long long)p.supported_transports, p.platform_device_hint);
		} else {
			printf("  [%llu] \"%s\"  %s\n", (unsigned long long)p.camera_id, p.display_name,
			       state_str(p.state));
			printf("       persistentId %s\n", p.persistent_id);
			printf("       flags        %s\n", flags);
			char rate[64];
			if (p.max_frame_rate > 0.0f) {
				snprintf(rate, sizeof(rate), "%.1f Hz", p.max_frame_rate);
			} else {
				snprintf(rate, sizeof(rate), "rate unknown (measured once a stream runs)");
			}
			printf("       eye          %ux%u (SBS %ux%u), %u views, %s\n", p.eye_width, p.eye_height,
			       2 * p.eye_width, p.eye_height, p.view_count, rate);
			printf("       baseline     %.2f mm, HFOV %.2f deg per eye\n", p.baseline_mm,
			       p.horizontal_fov_deg);
			printf("       formats      0x%llx  transports 0x%llx  platform hint \"%s\"\n",
			       (unsigned long long)p.supported_formats, (unsigned long long)p.supported_transports,
			       p.platform_device_hint);
		}
	}
	if (json) {
		printf("]}\n");
	}
	return 0;
}

static int
cmd_calib(struct ipc_connection *ipc_c, uint64_t id, uint32_t output, bool json)
{
	struct xrt_stereo_camera_calibration c;
	xrt_result_t xret = ipc_client_stereo_camera_get_calibration(ipc_c, id, output, &c);
	if (xret != XRT_SUCCESS) {
		printf("stereo_camera_get_calibration(%llu) failed: %d%s\n", (unsigned long long)id, (int)xret,
		       result_hint(xret));
		return refusal_exit_code(xret);
	}
	if (json) {
		printf("{\"output\": \"%s\", \"baselineMm\": %.3f, \"eyes\": [",
		       output == XRT_STEREO_CAMERA_OUTPUT_RAW ? "RAW" : "RECTIFIED", c.baseline_mm);
		for (int e = 0; e < 2; e++) {
			printf(
			    "%s{\"w\": %u, \"h\": %u, \"fx\": %.3f, \"fy\": %.3f, \"cx\": %.3f, \"cy\": %.3f, "
			    "\"model\": %u}",
			    e ? ", " : "", c.eye[e].width, c.eye[e].height, c.eye[e].fx, c.eye[e].fy, c.eye[e].cx,
			    c.eye[e].cy, c.eye[e].model);
		}
		printf(
		    "], \"rightFromLeft\": {\"orientation\": [%.6f, %.6f, %.6f, %.6f], \"position\": [%.6f, "
		    "%.6f, %.6f]}}\n",
		    c.orientation[0], c.orientation[1], c.orientation[2], c.orientation[3], c.position[0],
		    c.position[1], c.position[2]);
		return 0;
	}
	printf("calibration (%s), baseline %.3f mm\n", output == XRT_STEREO_CAMERA_OUTPUT_RAW ? "RAW" : "RECTIFIED",
	       c.baseline_mm);
	for (int e = 0; e < 2; e++) {
		printf("  %s eye: %ux%u fx %.3f fy %.3f cx %.3f cy %.3f model %u\n", e ? "right" : "left ",
		       c.eye[e].width, c.eye[e].height, c.eye[e].fx, c.eye[e].fy, c.eye[e].cx, c.eye[e].cy,
		       c.eye[e].model);
	}
	printf("  right camera in left frame: q (%.5f %.5f %.5f %.5f)  p (%.5f %.5f %.5f) m\n", c.orientation[0],
	       c.orientation[1], c.orientation[2], c.orientation[3], c.position[0], c.position[1], c.position[2]);
	if (output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED) {
		// OpenCV-style projection matrices of the rectified pair (Tx in mm).
		double tx = -(double)c.position[0] * 1000.0;
		printf("  P1 = [%.3f 0 %.3f 0; 0 %.3f %.3f 0; 0 0 1 0]\n", c.eye[0].fx, c.eye[0].cx, c.eye[0].fy,
		       c.eye[0].cy);
		printf("  P2 = [%.3f 0 %.3f %.3f; 0 %.3f %.3f 0; 0 0 1 0]   (P2[0][3] = f * Tx, Tx = %.3f mm)\n",
		       c.eye[1].fx, c.eye[1].cx, c.eye[1].fx * tx, c.eye[1].fy, c.eye[1].cy, tx);
		printf("  depth from disparity: Z = f * B / d = %.1f / d(px) m\n",
		       c.eye[0].fx * c.baseline_mm / 1000.0);
	}
	return 0;
}


/*
 *
 * probe.
 *
 */

struct ro_map
{
	const uint8_t *ptr;
	uint64_t size;
#ifdef XRT_OS_WINDOWS
	HANDLE section;
#else
	int section;
#endif
};

static bool
map_readonly(struct ro_map *m, xrt_shmem_handle_t h, uint64_t size)
{
	m->size = size;
	m->section = h;
#ifdef XRT_OS_WINDOWS
	m->ptr = (const uint8_t *)MapViewOfFile(h, FILE_MAP_READ, 0, 0, (SIZE_T)size);
	return m->ptr != NULL;
#else
	void *p = mmap(NULL, (size_t)size, PROT_READ, MAP_SHARED, h, 0);
	m->ptr = p == MAP_FAILED ? NULL : (const uint8_t *)p;
	return m->ptr != NULL;
#endif
}

static void
unmap_readonly(struct ro_map *m)
{
#ifdef XRT_OS_WINDOWS
	if (m->ptr != NULL) {
		UnmapViewOfFile(m->ptr);
	}
	if (m->section != NULL) {
		CloseHandle(m->section);
	}
#else
	if (m->ptr != NULL) {
		munmap((void *)m->ptr, (size_t)m->size);
	}
	if (m->section >= 0) {
		close(m->section);
	}
#endif
	memset(m, 0, sizeof(*m));
}

//! Wait for the stream's wake handle, then drain it. true = signalled.
static bool
wait_wake(xrt_graphics_sync_handle_t w, int timeout_ms)
{
#ifdef XRT_OS_WINDOWS
	return WaitForSingleObject(w, (DWORD)timeout_ms) == WAIT_OBJECT_0;
#else
	struct pollfd pfd = {.fd = w, .events = POLLIN, .revents = 0};
	if (poll(&pfd, 1, timeout_ms) <= 0) {
		return false;
	}
	char buf[64];
	while (read(w, buf, sizeof(buf)) > 0) {
	}
	return true;
#endif
}

static void
close_wake(xrt_graphics_sync_handle_t w)
{
#ifdef XRT_OS_WINDOWS
	if (w != NULL) {
		CloseHandle(w);
	}
#else
	if (w >= 0) {
		close(w);
	}
#endif
}

static int
cmp_int(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

//! Nearest subject the probe expects: its disparity bounds the search.
#define PROBE_NEAREST_M 0.4

/*!
 * Disparity search bound from the camera's own geometry: the disparity of a
 * subject at PROBE_NEAREST_M, f * B / Z, with f from the per-eye HFOV. A fixed
 * 64 px was fine for the 50 mm sim fake and useless on the 120 mm Leia SR
 * tracking camera, where a face at 0.6 m is ~95 px and every block clamped at
 * 64. Uncalibrated: a quarter of the eye width. Always within [64, 0.6 * eye].
 */
static uint32_t
probe_max_disparity(const struct xrt_stereo_camera_properties *p, uint32_t eye_w)
{
	double d = eye_w / 4.0;
	if (p != NULL && p->baseline_mm > 0.0f && p->horizontal_fov_deg > 1.0f && p->horizontal_fov_deg < 179.0f) {
		double f = (eye_w / 2.0) / tan(p->horizontal_fov_deg * 0.5 * 3.14159265358979323846 / 180.0);
		d = f * (p->baseline_mm / 1000.0) / PROBE_NEAREST_M;
	}
	double cap = eye_w * 0.6;
	if (d > cap) {
		d = cap;
	}
	return d < 64.0 ? 64u : (uint32_t)ceil(d);
}

//! Block-match the last frame on a 64x64 grid and print the two dominant
//! disparities (the sim fake: background 12 px, bar 40 px). Blocks whose best
//! match sits ON the search bound are a clamp, not a measurement: counted and
//! reported, never folded into the modes.
static void
report_disparity(const uint8_t *gray, uint32_t pitch, uint32_t eye_w, uint32_t h, uint32_t max_d, char *out, size_t cap)
{
	int vals[512];
	int n = 0, at_edge = 0;
	for (uint32_t y = 48; y + 64 <= h && n < 512; y += 64) {
		for (uint32_t x = 64; x + 64 <= eye_w && n < 512; x += 64) {
			float d = 0.0f;
			if (u_stereo_camera_estimate_disparity(gray, pitch, eye_w, h, x, y, 64, 64, max_d, &d)) {
				uint32_t lim = max_d < x ? max_d : x;
				if (d >= (float)lim - 0.5f) {
					at_edge++;
					continue;
				}
				vals[n++] = (int)(d + 0.5f);
			}
		}
	}
	if (n == 0) {
		snprintf(out, cap, "n/a (%s)", at_edge > 0 ? "every block hit the search bound" : "frame too small");
		return;
	}
	qsort(vals, (size_t)n, sizeof(int), cmp_int);
	// Histogram modes.
	int best_v[2] = {-1, -1}, best_c[2] = {0, 0};
	for (int i = 0; i < n;) {
		int j = i;
		while (j < n && vals[j] == vals[i]) {
			j++;
		}
		int c = j - i;
		if (c > best_c[0]) {
			best_v[1] = best_v[0];
			best_c[1] = best_c[0];
			best_v[0] = vals[i];
			best_c[0] = c;
		} else if (c > best_c[1]) {
			best_v[1] = vals[i];
			best_c[1] = c;
		}
		i = j;
	}
	int k = snprintf(out, cap, "%d px (%d/%d blocks), %d px (%d/%d blocks), range %d..%d px (search 0..%u)",
	                 best_v[0], best_c[0], n, best_v[1], best_c[1], n, vals[0], vals[n - 1], max_d);
	if (at_edge > 0 && k > 0 && (size_t)k < cap) {
		snprintf(out + k, cap - (size_t)k, "; %d block(s) AT THE SEARCH BOUND, excluded", at_edge);
	}
}

static int
cmp_float(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;
	return (x > y) - (x < y);
}

struct row_alignment
{
	int textured;            //!< blocks that passed the texture gate and were matched
	int blocks;              //!< ...of which accepted (NCC >= min, peak inside both windows)
	int low_corr;            //!< rejected: peak NCC below the threshold
	int dx_edge;             //!< rejected: peak ON the disparity bound (a clamp, not a measurement)
	int dy_edge;             //!< rejected: peak ON the vertical bound
	float dy_signed_median;  //!< right y - left y, px (a constant vertical offset shows here)
	float dy_median, dy_p90; //!< |dy|, px (informational: block-match noise on a face scene inflates these)
	float dy_mad;            //!< median |dy - signed median|, px: the robust spread the gate uses
	int outliers;            //!< accepted blocks with |dy| > 1 px
	int modes;
	float mode_dx[2];
	int mode_count[2];
};

struct row_params
{
	uint32_t max_d;  //!< disparity window (probe_max_disparity)
	uint32_t max_dy; //!< vertical window, +-px
	float min_ncc;   //!< acceptance threshold on the peak correlation
};

/*!
 * 2-D NCC block matching (u_stereo_camera_match_block) of a GRAY8 SBS frame on
 * a 32 px grid, textured blocks only (a flat wall matches anywhere): signed and
 * |dy| statistics and the two dominant horizontal disparities (1 px clusters).
 * Blocks below the correlation threshold or whose peak sits on either window's
 * bound are counted and excluded — never clamped into the statistics.
 */
static void
measure_rows(const uint8_t *gray,
             uint32_t pitch,
             uint32_t eye_w,
             uint32_t h,
             const struct row_params *rp,
             struct row_alignment *out)
{
	memset(out, 0, sizeof(*out));
	enum
	{
		B = 32,
		MAXB = 1024
	};
	static float dxs[MAXB], ady[MAXB], sdy[MAXB];
	int n = 0;
	for (uint32_t y = 16; y + B + 16 <= h && out->textured < MAXB; y += B) {
		for (uint32_t x = 16; x + B + 8 <= eye_w && out->textured < MAXB; x += B) {
			// Texture gate: standard deviation of the left block >= 6 levels.
			double sum = 0, sq = 0;
			for (uint32_t j = 0; j < B; j++) {
				const uint8_t *r = gray + (size_t)(y + j) * pitch + x;
				for (uint32_t i = 0; i < B; i++) {
					sum += r[i];
					sq += (double)r[i] * r[i];
				}
			}
			double mean = sum / (B * B);
			if (sq / (B * B) - mean * mean < 36.0) {
				continue;
			}
			struct u_stereo_camera_block_match m;
			if (!u_stereo_camera_match_block(gray, pitch, eye_w, h, x, y, B, B, rp->max_d, rp->max_dy,
			                                 &m)) {
				continue;
			}
			out->textured++;
			if (m.ncc < rp->min_ncc) {
				out->low_corr++;
				continue;
			}
			if (m.dx_at_edge) {
				out->dx_edge++;
				continue;
			}
			if (m.dy_at_edge) {
				out->dy_edge++;
				continue;
			}
			dxs[n] = m.dx;
			sdy[n] = m.dy;
			ady[n] = m.dy < 0 ? -m.dy : m.dy;
			n++;
		}
	}
	out->blocks = n;
	if (n == 0) {
		return;
	}
	qsort(sdy, (size_t)n, sizeof(float), cmp_float);
	out->dy_signed_median = sdy[n / 2];
	qsort(ady, (size_t)n, sizeof(float), cmp_float);
	out->dy_median = ady[n / 2];
	out->dy_p90 = ady[(n * 9) / 10];
	// Robust spread about the signed median (MAD). ady[] is free again once its stats are read.
	for (int i = 0; i < n; i++) {
		float r = sdy[i] - out->dy_signed_median;
		ady[i] = r < 0 ? -r : r;
	}
	qsort(ady, (size_t)n, sizeof(float), cmp_float);
	out->dy_mad = ady[n / 2];
	for (int i = 0; i < n; i++) {
		out->outliers += ady[i] > 1.0f;
	}
	qsort(dxs, (size_t)n, sizeof(float), cmp_float);
	// Greedy 1 px clusters; keep the two most populated.
	for (int i = 0; i < n;) {
		int j = i;
		double s = 0;
		while (j < n && dxs[j] - dxs[i] <= 1.0f) {
			s += dxs[j];
			j++;
		}
		int c = j - i;
		float mean = (float)(s / c);
		if (c > out->mode_count[0]) {
			out->mode_dx[1] = out->mode_dx[0];
			out->mode_count[1] = out->mode_count[0];
			out->mode_dx[0] = mean;
			out->mode_count[0] = c;
		} else if (c > out->mode_count[1]) {
			out->mode_dx[1] = mean;
			out->mode_count[1] = c;
		}
		i = j;
	}
	out->modes = out->mode_count[1] > 0 ? 2 : 1;
}

static int
cmd_probe(struct ipc_connection *ipc_c, int argc, const char **argv)
{
	uint64_t id = 0;
	uint32_t output = XRT_STEREO_CAMERA_OUTPUT_RECTIFIED;
	uint32_t format = XRT_STEREO_CAMERA_FORMAT_NV12;
	float fps = 0.0f;
	int frames = 90;
	float seconds = 0.0f;
	const char *out_dir = NULL;
	bool require_rectified = false;
	uint32_t max_d_arg = 0; // 0 = from the camera's geometry
	struct row_params rp = {.max_d = 0, .max_dy = 24, .min_ncc = 0.90f};
	char rows_txt[64] = "";
	bool have_rows = false;
	for (int i = 3; i < argc; i++) {
		if (strcmp(argv[i], "--raw") == 0) {
			output = XRT_STEREO_CAMERA_OUTPUT_RAW;
		} else if (strcmp(argv[i], "--rectified") == 0) {
			output = XRT_STEREO_CAMERA_OUTPUT_RECTIFIED;
			require_rectified = true;
		} else if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
			const char *f = argv[++i];
			format = strcmp(f, "gray8") == 0   ? XRT_STEREO_CAMERA_FORMAT_GRAY8
			         : strcmp(f, "bgra8") == 0 ? XRT_STEREO_CAMERA_FORMAT_BGRA8
			                                   : XRT_STEREO_CAMERA_FORMAT_NV12;
		} else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
			fps = (float)atof(argv[++i]);
		} else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
			frames = atoi(argv[++i]);
		} else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
			seconds = (float)atof(argv[++i]);
		} else if (strcmp(argv[i], "--max-disparity") == 0 && i + 1 < argc) {
			max_d_arg = (uint32_t)strtoul(argv[++i], NULL, 10);
		} else if (strcmp(argv[i], "--max-dy") == 0 && i + 1 < argc) {
			rp.max_dy = (uint32_t)strtoul(argv[++i], NULL, 10);
		} else if (strcmp(argv[i], "--min-ncc") == 0 && i + 1 < argc) {
			rp.min_ncc = (float)atof(argv[++i]);
		} else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
			out_dir = argv[++i];
		} else if (argv[i][0] != '-') {
			id = strtoull(argv[i], NULL, 10);
		}
	}
	struct xrt_stereo_camera_properties props;
	memset(&props, 0, sizeof(props));
	{
		uint32_t n = 0;
		bool found = false;
		if (ipc_client_stereo_camera_count(ipc_c, &n) == XRT_SUCCESS) {
			for (uint32_t i = 0; i < n && !found; i++) {
				if (ipc_client_stereo_camera_get_properties(ipc_c, i, &props) == XRT_SUCCESS &&
				    (id == 0 || props.camera_id == id)) {
					found = true;
				}
			}
		}
		if (!found && id == 0) {
			printf("camera probe: the service exposes no stereo camera.\n");
			return 3;
		}
		if (!found) {
			memset(&props, 0, sizeof(props)); // stream_create reports the bad id
		}
		id = found ? props.camera_id : id;
	}

	struct xrt_stereo_camera_stream_request req = {
	    .camera_id = id,
	    .output = output,
	    .format = format,
	    .transport = XRT_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY,
	    .max_frame_rate = fps,
	};
	uint64_t sid = 0;
	xrt_result_t xret = ipc_client_stereo_camera_stream_create(ipc_c, &req, &sid);
	if (xret != XRT_SUCCESS) {
		printf("stream create failed: %d%s\n", (int)xret, result_hint(xret));
		return 2;
	}
	xret = ipc_client_stereo_camera_stream_start(ipc_c, sid);
	if (xret != XRT_SUCCESS) {
		printf("stream start failed: %d%s\n", (int)xret, result_hint(xret));
		ipc_client_stereo_camera_stream_destroy(ipc_c, sid);
		return refusal_exit_code(xret);
	}
	struct xrt_stereo_camera_stream_layout lay;
	xrt_shmem_handle_t section;
	xrt_graphics_sync_handle_t wake;
	xret = ipc_client_stereo_camera_stream_get_transport(ipc_c, sid, &lay, &section, &wake);
	if (xret != XRT_SUCCESS) {
		printf("stream transport failed: %d%s\n", (int)xret, result_hint(xret));
		ipc_client_stereo_camera_stream_destroy(ipc_c, sid);
		return 2;
	}
	struct ro_map map;
	memset(&map, 0, sizeof(map));
	if (!map_readonly(&map, section, lay.section_size)) {
		printf("could not map the ring read-only\n");
		close_wake(wake);
		ipc_client_stereo_camera_stream_destroy(ipc_c, sid);
		return 2;
	}
	char cap[48];
	if (lay.max_frame_rate > 0.0f) {
		snprintf(cap, sizeof(cap), "cap %.1f Hz", lay.max_frame_rate);
	} else {
		snprintf(cap, sizeof(cap), "cap = source rate (not measured yet)");
	}
	printf("stream %llu on camera %llu: %ux%u %s SBS (%s), %s, ring %u x %llu B (section %llu B)\n",
	       (unsigned long long)sid, (unsigned long long)id, lay.width, lay.height, format_str(lay.format),
	       lay.output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED ? "RECTIFIED" : "RAW (flagged)", cap, lay.slot_count,
	       (unsigned long long)lay.slot_stride, (unsigned long long)lay.section_size);

	int64_t t_start = os_monotonic_get_ns();
	int64_t deadline = seconds > 0.0f ? t_start + (int64_t)(seconds * 1e9) : t_start + 60ll * 1000000000;
	int got = 0, wakes = 0, not_ready = 0;
	bool rows_ok = true;
	bool ended = false;
	int events = 0;
	uint64_t first_index = 0, last_index = 0, gaps = 0;
	int64_t t_first = 0, t_last = 0;
	struct xrt_stereo_camera_frame_info last;
	memset(&last, 0, sizeof(last));
	while ((seconds > 0.0f || got < frames) && os_monotonic_get_ns() < deadline && !ended) {
		// R3: the service's events (state changes, a stream it ended) are
		// polled like xrPollEvent would; printed with the time since start.
		for (;;) {
			bool has = false;
			struct xrt_stereo_camera_event ev;
			if (ipc_client_stereo_camera_poll_event(ipc_c, &has, &ev) != XRT_SUCCESS || !has) {
				break;
			}
			events++;
			double t = (double)(os_monotonic_get_ns() - t_start) * 1e-9;
			if (ev.kind == XRT_STEREO_CAMERA_EVENT_CAMERA_STATE) {
				printf("[%6.2f s] event: camera %llu state -> %s\n", t, (unsigned long long)ev.camera_id,
				       state_str(ev.value));
			} else if (ev.kind == XRT_STEREO_CAMERA_EVENT_STREAM_ENDED) {
				printf("[%6.2f s] event: stream %llu ENDED by the service (reason %u = %s)\n", t,
				       (unsigned long long)ev.stream_id, ev.value,
				       ev.value == XRT_STEREO_CAMERA_END_USER_STOPPED ? "USER_STOPPED"
				       : ev.value == XRT_STEREO_CAMERA_END_DISABLED  ? "DISABLED"
				                                                     : "SOURCE_LOST");
				ended = ended || ev.stream_id == sid;
			} else {
				printf("[%6.2f s] event: cameras changed\n", t);
			}
		}
		if (ended) {
			break;
		}
		if (!wait_wake(wake, 250)) {
			continue;
		}
		wakes++;
		bool ready = false;
		struct xrt_stereo_camera_frame_info fi;
		xret = ipc_client_stereo_camera_acquire(ipc_c, sid, &ready, &fi);
		if (xret == XRT_ERROR_STEREO_CAMERA_STREAM_ENDED) {
			printf("acquire: %d%s\n", (int)xret, result_hint(xret));
			ended = true;
			continue; // drain the ENDED event on the next pass
		}
		if (xret != XRT_SUCCESS) {
			printf("acquire failed: %d%s\n", (int)xret, result_hint(xret));
			break;
		}
		if (!ready) {
			not_ready++;
			continue;
		}
		int64_t now = os_monotonic_get_ns();
		if (got == 0) {
			first_index = fi.frame_index;
			t_first = now;
		} else if (fi.frame_index > last_index + 1) {
			gaps += fi.frame_index - last_index - 1;
		}
		last_index = fi.frame_index;
		t_last = now;
		last = fi;
		got++;
	}
	// One last drain: a stream the service ended queued its STREAM_ENDED event
	// behind the acquire that reported it.
	for (;;) {
		bool has = false;
		struct xrt_stereo_camera_event ev;
		if (ipc_client_stereo_camera_poll_event(ipc_c, &has, &ev) != XRT_SUCCESS || !has) {
			break;
		}
		events++;
		double t = (double)(os_monotonic_get_ns() - t_start) * 1e-9;
		if (ev.kind == XRT_STEREO_CAMERA_EVENT_STREAM_ENDED) {
			printf("[%6.2f s] event: stream %llu ENDED by the service (reason %u = %s)\n", t,
			       (unsigned long long)ev.stream_id, ev.value,
			       ev.value == XRT_STEREO_CAMERA_END_USER_STOPPED ? "USER_STOPPED"
			       : ev.value == XRT_STEREO_CAMERA_END_DISABLED  ? "DISABLED"
			                                                     : "SOURCE_LOST");
			ended = ended || ev.stream_id == sid;
		} else if (ev.kind == XRT_STEREO_CAMERA_EVENT_CAMERA_STATE) {
			printf("[%6.2f s] event: camera %llu state -> %s\n", t, (unsigned long long)ev.camera_id,
			       state_str(ev.value));
		}
	}
	double span = (double)(t_last - t_first) * 1e-9;
	double rate = (got > 1 && span > 0.0) ? (got - 1) / span : 0.0;
	printf(
	    "received %d frames (index %llu..%llu, %llu source frames not delivered to this stream), %d wakes, "
	    "%d not-ready, %d event(s)%s\n",
	    got, (unsigned long long)first_index, (unsigned long long)last_index, (unsigned long long)gaps, wakes,
	    not_ready, events, ended ? " — STREAM ENDED BY THE SERVICE" : "");
	printf("measured delivery rate: %.2f Hz over %.2f s\n", rate, span);

	if (got > 0) {
		const uint8_t *slot = map.ptr + (uint64_t)last.slot * lay.slot_stride;
		printf(
		    "last frame: index %llu, slot %u, %ux%u %s, pitch %u/%u, planes @%llu/%llu, %s time, output %s, "
		    "calibration gen %u\n",
		    (unsigned long long)last.frame_index, last.slot, last.width, last.height, format_str(last.format),
		    last.row_pitch[0], last.row_pitch[1], (unsigned long long)last.plane_offset[0],
		    (unsigned long long)last.plane_offset[1], last.time_is_exposure ? "exposure" : "arrival",
		    last.output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED ? "RECTIFIED" : "RAW",
		    last.calibration_generation);

		// Luma of the last frame, for the disparity probe and a GRAY8 PNG.
		struct u_stereo_camera_planes gl;
		u_stereo_camera_layout(XRT_STEREO_CAMERA_FORMAT_GRAY8, last.width, last.height, &gl);
		uint8_t *gray = calloc((size_t)gl.size, 1);
		const uint8_t *src[2] = {slot + last.plane_offset[0], slot + last.plane_offset[1]};
		if (gray != NULL &&
		    u_stereo_camera_convert(last.format, src, last.row_pitch, XRT_STEREO_CAMERA_FORMAT_GRAY8, gray, &gl,
		                            last.width, last.height)) {
			char disp[256];
			rp.max_d = max_d_arg > 0 ? max_d_arg : probe_max_disparity(&props, last.width / 2);
			report_disparity(gray, gl.pitch[0], last.width / 2, last.height, rp.max_d, disp, sizeof(disp));
			printf("block disparity (left x - right x): %s\n", disp);

			// R2: row alignment + depth of the dominant disparities.
			struct row_alignment ra;
			measure_rows(gray, gl.pitch[0], last.width / 2, last.height, &rp, &ra);
			struct xrt_stereo_camera_calibration rc;
			double fb = 0.0; // f * B, px * m
			if (last.output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED &&
			    ipc_client_stereo_camera_get_calibration(ipc_c, id, XRT_STEREO_CAMERA_OUTPUT_RECTIFIED,
			                                             &rc) == XRT_SUCCESS) {
				fb = rc.eye[0].fx * rc.baseline_mm / 1000.0;
			}
			const char *out_name = last.output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED ? "RECTIFIED" : "RAW";
			printf(
			    "row alignment (%s): NCC 32x32 blocks, search d -8..%u px, dy +-%u px, accept NCC >= "
			    "%.2f\n",
			    out_name, rp.max_d, rp.max_dy, rp.min_ncc);
			printf(
			    "  %d textured block(s): %d accepted, %d below NCC %.2f, %d AT the disparity bound, %d AT "
			    "the dy bound\n",
			    ra.textured, ra.blocks, ra.low_corr, rp.min_ncc, ra.dx_edge, ra.dy_edge);
			if (ra.dx_edge * 5 > ra.textured || ra.dy_edge * 5 > ra.textured) {
				printf(
				    "  WARNING: over 20%% of blocks peak ON a search bound — that is a clamp, not a "
				    "measurement; widen it (--max-disparity N / --max-dy N)\n");
			}
			if (ra.blocks < 5) {
				printf("  row alignment: n/a (fewer than 5 accepted blocks)\n");
				rows_ok = false;
			} else {
				printf(
				    "  dy (right - left) median %+.3f px SIGNED; |dy| median %.3f px, p90 %.3f px, %d "
				    "block(s) > 1 px\n",
				    ra.dy_signed_median, ra.dy_median, ra.dy_p90, ra.outliers);
				for (int k = 0; k < ra.modes; k++) {
					if (fb > 0.0 && ra.mode_dx[k] > 0.25f) {
						printf(
						    "  disparity mode %d: %.2f px (%d blocks) -> Z = f*B/d = %.3f m\n",
						    k + 1, ra.mode_dx[k], ra.mode_count[k], fb / ra.mode_dx[k]);
					} else {
						printf("  disparity mode %d: %.2f px (%d blocks)\n", k + 1,
						       ra.mode_dx[k], ra.mode_count[k]);
					}
				}
				snprintf(rows_txt, sizeof(rows_txt), "%+.2f px (%d blocks)", ra.dy_signed_median,
				         ra.blocks);
				have_rows = true;
				// A constant vertical offset fails even when |dy| scatter is small.
				// The gate is ROBUST: |signed median| <= 0.5 px (the rows' actual offset) with a
				// bounded MAD spread and enough blocks. The unsigned |dy| median is NOT gated: on a
				// face scene block-match noise alone put it at ~1.0 px while the signed median was
				// -0.1 px (SR laptop, 2026-09-27).
				float sm = ra.dy_signed_median < 0 ? -ra.dy_signed_median : ra.dy_signed_median;
				rows_ok = sm <= 0.5f && ra.dy_mad <= 1.5f && ra.blocks >= 20;
				printf("  row gate: |signed median| %.3f px (<= 0.5), MAD %.3f px (<= 1.5), %d blocks "
				       "(>= 20)\n",
				       sm, ra.dy_mad, ra.blocks);
			}
		}
		if (out_dir != NULL) {
			char path[1024];
			snprintf(path, sizeof(path), "%s/cam_%llu.png", out_dir, (unsigned long long)last.frame_index);
			int ok = 0;
			if (last.format == XRT_STEREO_CAMERA_FORMAT_GRAY8 && gray != NULL) {
				ok = stbi_write_png(path, (int)last.width, (int)last.height, 1, gray, (int)gl.pitch[0]);
			} else {
				struct u_stereo_camera_planes bl;
				u_stereo_camera_layout(XRT_STEREO_CAMERA_FORMAT_BGRA8, last.width, last.height, &bl);
				uint8_t *rgba = calloc((size_t)bl.size, 1);
				if (rgba != NULL && u_stereo_camera_convert(last.format, src, last.row_pitch,
				                                            XRT_STEREO_CAMERA_FORMAT_BGRA8, rgba, &bl,
				                                            last.width, last.height)) {
					for (uint32_t y = 0; y < last.height; y++) {
						uint8_t *r = rgba + (size_t)y * bl.pitch[0];
						for (uint32_t x = 0; x < last.width; x++) {
							uint8_t t = r[4 * x];
							r[4 * x] = r[4 * x + 2];
							r[4 * x + 2] = t;
						}
					}
					ok = stbi_write_png(path, (int)last.width, (int)last.height, 4, rgba,
					                    (int)bl.pitch[0]);
				}
				free(rgba);
			}
			printf("%s %s\n", ok ? "wrote" : "FAILED to write", path);
		}
		free(gray);
	}

	struct xrt_stereo_camera_stream_stats st;
	if (ipc_client_stereo_camera_stats(ipc_c, sid, &st) == XRT_SUCCESS) {
		printf(
		    "service stats: source %.2f Hz, delivered %.2f Hz, published %llu, skipped %llu, acquired %llu, "
		    "mean latency %.3f ms\n",
		    st.source_frame_rate, st.delivered_frame_rate, (unsigned long long)st.frames_published,
		    (unsigned long long)st.frames_skipped, (unsigned long long)st.frames_acquired,
		    (double)st.mean_latency_ns * 1e-6);
		// R2: the service's online vertical-alignment refinement.
		static const char *const rs[] = {"n/a", "OFF (DXR_STEREO_CAMERA_REFINE=0)", "MEASURING",
		                                 "ALIGNED (calibration within 0.2 px)", "APPLIED"};
		if (st.refine_state != XRT_STEREO_CAMERA_REFINE_NONE &&
		    st.refine_state <= XRT_STEREO_CAMERA_REFINE_APPLIED) {
			printf(
			    "vertical refinement: %s — correction a %+.2f px, b %+.3f px/100 px; %u update(s), "
			    "%u window(s), %u matches in the latest, %.2f ms per measurement\n",
			    rs[st.refine_state], st.refine_offset_px, st.refine_slope_per_100px, st.refine_updates,
			    st.refine_windows, st.refine_matches, st.refine_measure_ms);
			if (st.refine_windows > 0) {
				printf(
				    "vertical refinement: signed dy BEFORE (first window, uncorrected) %+.2f px -> "
				    "latest window %+.2f px; this probe's last frame %s\n",
				    st.refine_initial_dy_px, st.refine_residual_dy_px, have_rows ? rows_txt : "n/a");
			}
		}
	}

	unmap_readonly(&map);
	close_wake(wake);
	ipc_client_stereo_camera_stream_stop(ipc_c, sid);
	ipc_client_stereo_camera_stream_destroy(ipc_c, sid);
	if (ended) {
		return 8; // the service ended the stream (user stop / sharing off)
	}
	if (got == 0) {
		return 9;
	}
	if (require_rectified) {
		if (last.output != XRT_STEREO_CAMERA_OUTPUT_RECTIFIED) {
			printf("--rectified: FAIL — frames are RAW (the camera is not calibrated / not rectifiable)\n");
			return 5;
		}
		if (!rows_ok) {
			printf(
			    "--rectified: FAIL — rows do not align (|signed median dy| > 0.5 px, MAD > 1.5 px, or fewer "
			    "than 20 matched blocks)\n");
			return 5;
		}
		printf("--rectified: PASS — rows aligned\n");
	}
	return 0;
}


/*
 *
 * R3: consent store (local) and control ops (IPC, DIAG).
 *
 */

//! Absolute path of this executable — what the service sees as the peer.
static bool
self_exe_path(char *out, size_t cap)
{
	out[0] = '\0';
#if defined(XRT_OS_WINDOWS)
	wchar_t w[1024];
	DWORD n = GetModuleFileNameW(NULL, w, (DWORD)(sizeof(w) / sizeof(w[0])));
	if (n == 0) {
		return false;
	}
	int m = WideCharToMultiByte(CP_UTF8, 0, w, (int)n, out, (int)cap - 1, NULL, NULL);
	if (m <= 0) {
		return false;
	}
	out[m] = '\0';
	return true;
#elif defined(XRT_OS_MACOS)
	char tmp[1024];
	uint32_t sz = sizeof(tmp);
	if (_NSGetExecutablePath(tmp, &sz) != 0) {
		return false;
	}
	// proc_pidpath (what the service uses) returns the resolved path.
	return realpath(tmp, out) != NULL || snprintf(out, cap, "%s", tmp) > 0;
#else
	ssize_t n = readlink("/proc/self/exe", out, cap - 1);
	if (n <= 0) {
		return false;
	}
	out[n] = '\0';
	return true;
#endif
}

//! "<exe>" or "--self" -> the path to act on.
static bool
resolve_exe_arg(int argc, const char **argv, int idx, char *out, size_t cap)
{
	if (idx >= argc) {
		return false;
	}
	if (strcmp(argv[idx], "--self") == 0) {
		return self_exe_path(out, cap);
	}
	snprintf(out, cap, "%s", argv[idx]);
	return out[0] != '\0';
}

static void
consent_list_cb(void *ctx, char kind, const char *exe, const char *value)
{
	(void)ctx;
	switch (kind) {
	case 's': printf("  sharing:      %s\n", value); break;
	case 'a': printf("  app %-6s    %s\n", value, exe); break;
	case 'd': printf("  delegating    %s  (%s list)\n", exe, value); break;
	default: break;
	}
}

static int
cmd_consent(void)
{
	char path[1024];
	if (u_camera_consent_store_path(path, sizeof(path))) {
		printf("stereo camera consent store: %s\n", path);
	}
	u_camera_consent_store_list(consent_list_cb, NULL);
	char self[1024];
	if (self_exe_path(self, sizeof(self))) {
		printf("  (this CLI: %s)\n", self);
	}
	return 0;
}

static int
cmd_store(const char *op, int argc, const char **argv)
{
	char exe[1024];
	if (!resolve_exe_arg(argc, argv, 3, exe, sizeof(exe))) {
		printf("usage: displayxr-cli camera %s <exe>|--self\n", op);
		return 1;
	}
	const struct u_camera_consent_store_ops *st = u_camera_consent_store_default();
	bool ok;
	if (strcmp(op, "allow") == 0) {
		ok = st->set(NULL, exe, U_CAMERA_CONSENT_STORED_ALLOW);
	} else if (strcmp(op, "deny") == 0) {
		ok = st->set(NULL, exe, U_CAMERA_CONSENT_STORED_DENY);
	} else if (strcmp(op, "revoke") == 0) {
		ok = st->set(NULL, exe, U_CAMERA_CONSENT_STORED_NONE);
	} else if (strcmp(op, "trust") == 0) {
		ok = u_camera_consent_store_set_delegating(exe, true);
	} else { // untrust
		ok = u_camera_consent_store_set_delegating(exe, false);
	}
	printf("%s %s: %s\n", op, exe, ok ? "ok (takes effect on the next stream start)" : "FAILED to write the store");
	return ok ? 0 : 2;
}

static int
cmd_control(uint32_t op, uint32_t arg, const char *what)
{
	struct ipc_connection ipc_c = {0};
	if (!cam_connect(&ipc_c, XRT_CLIENT_CLASS_DIAG)) {
		return 2;
	}
	uint32_t value = 0;
	xrt_result_t xret = ipc_client_stereo_camera_control(&ipc_c, op, arg, &value);
	if (xret != XRT_SUCCESS) {
		printf("%s failed: %d%s\n", what, (int)xret, result_hint(xret));
	} else if (op == XRT_STEREO_CAMERA_CONTROL_STATUS) {
		printf("stereo camera: sharing %s, session %s, %u started stream(s)\n", (value & 1u) ? "on" : "OFF",
		       (value & 2u) ? "LOCKED" : "unlocked", (value >> 8) & 0xffu);
	} else if (op == XRT_STEREO_CAMERA_CONTROL_STOP_ALL) {
		printf("%s: %u stream(s) ended\n", what, value);
	} else {
		printf("%s: ok\n", what);
	}
	ipc_client_connection_fini(&ipc_c);
	return xret == XRT_SUCCESS ? 0 : 2;
}

static bool
on_off_arg(int argc, const char **argv, int idx, uint32_t *out)
{
	if (idx >= argc) {
		return false;
	}
	if (strcmp(argv[idx], "on") == 0 || strcmp(argv[idx], "1") == 0) {
		*out = 1;
		return true;
	}
	if (strcmp(argv[idx], "off") == 0 || strcmp(argv[idx], "0") == 0) {
		*out = 0;
		return true;
	}
	return false;
}

static void
usage(void)
{
	printf(
	    "usage: displayxr-cli camera list [--json] | calib <id> [--raw|--rectified] [--json] |\n"
	    "       probe [<id>] [--raw|--rectified] [--format gray8|nv12|bgra8] [--fps F] [--frames N] "
	    "[--seconds S] "
	    "[--out DIR]\n"
	    "             [--max-disparity N] [--max-dy N (24)] [--min-ncc C (0.90)]\n"
	    "       consent                      list the consent store\n"
	    "       allow|deny|revoke <exe>|--self   stored per-app decision (local)\n"
	    "       trust|untrust <exe>|--self       user-level consent-delegating list (local)\n"
	    "       status | stop-all | sharing on|off | fake-lock on|off   (over IPC, DIAG)\n"
	    "exit codes: 4 consent refused, 6 sharing off, 7 busy, 8 ended by the service, 5 rows misaligned\n");
}

int
cli_cmd_camera(int argc, const char **argv)
{
	const char *sub = argc >= 3 ? argv[2] : "list";
	bool json = cli_has_flag(argc, argv, "--json");

	// Local store edits: no service needed.
	if (strcmp(sub, "consent") == 0) {
		return cmd_consent();
	}
	if (strcmp(sub, "allow") == 0 || strcmp(sub, "deny") == 0 || strcmp(sub, "revoke") == 0 ||
	    strcmp(sub, "trust") == 0 || strcmp(sub, "untrust") == 0) {
		return cmd_store(sub, argc, argv);
	}
	// Control ops: DIAG class over IPC.
	uint32_t onoff = 0;
	if (strcmp(sub, "status") == 0) {
		return cmd_control(XRT_STEREO_CAMERA_CONTROL_STATUS, 0, "status");
	}
	if (strcmp(sub, "stop-all") == 0) {
		return cmd_control(XRT_STEREO_CAMERA_CONTROL_STOP_ALL, 0, "stop-all");
	}
	if (strcmp(sub, "sharing") == 0) {
		if (!on_off_arg(argc, argv, 3, &onoff)) {
			usage();
			return 1;
		}
		return cmd_control(XRT_STEREO_CAMERA_CONTROL_SET_SHARING, onoff, onoff ? "sharing on" : "sharing off");
	}
	if (strcmp(sub, "fake-lock") == 0) {
		if (!on_off_arg(argc, argv, 3, &onoff)) {
			usage();
			return 1;
		}
		return cmd_control(XRT_STEREO_CAMERA_CONTROL_SET_LOCKED, onoff, onoff ? "fake-lock on" : "fake-lock off");
	}
	if (strcmp(sub, "list") != 0 && strcmp(sub, "calib") != 0 && strcmp(sub, "probe") != 0) {
		usage();
		return 1;
	}

	// The consumer path: the same class a browser's capture utility declares.
	struct ipc_connection ipc_c = {0};
	if (!cam_connect(&ipc_c, XRT_CLIENT_CLASS_CAMERA_CONSUMER)) {
		return 2;
	}
	int ret;
	if (strcmp(sub, "list") == 0) {
		ret = cmd_list(&ipc_c, json);
	} else if (strcmp(sub, "calib") == 0 && argc >= 4) {
		uint32_t output = cli_has_flag(argc, argv, "--rectified") ? XRT_STEREO_CAMERA_OUTPUT_RECTIFIED
		                                                          : XRT_STEREO_CAMERA_OUTPUT_RAW;
		ret = cmd_calib(&ipc_c, strtoull(argv[3], NULL, 10), output, json);
	} else if (strcmp(sub, "probe") == 0) {
		ret = cmd_probe(&ipc_c, argc, argv);
	} else {
		usage();
		ret = 1;
	}
	ipc_client_connection_fini(&ipc_c);
	return ret;
}

#else // !CLI_HAVE_IPC

#include <stdio.h>

int
cli_cmd_camera(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	printf("displayxr-cli camera: this build has no IPC client (XRT_FEATURE_SERVICE off).\n");
	return 1;
}

#endif
