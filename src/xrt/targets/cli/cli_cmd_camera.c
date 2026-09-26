// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `displayxr-cli camera` — XR_DXR_stereo_camera (ADR-043) over IPC.
 *
 *   camera list  [--json]                         properties + state of every camera
 *   camera calib <id> [--raw|--rectified] [--json]
 *   camera probe [<id>] [--raw] [--format gray8|nv12|bgra8] [--fps F]
 *                [--frames N] [--seconds S] [--out DIR]
 *
 * `probe` is a real consumer: it creates and starts a stream (so it goes
 * through the service's authorisation — in R1, DXR_STEREO_CAMERA_DEV_ALLOW=1
 * on the SERVICE), maps the ring READ-ONLY, waits on the per-stream wake
 * handle, acquires, and reports the measured delivery rate, frame-index gaps,
 * the SBS layout, block-matched disparities of the last frame (the sim fake's
 * scene is 12 px background / 40 px bar), and the service's stream stats.
 * With --out it writes the last frame as cam_<frameIndex>.png.
 *
 * Connects as a DIAG client, like `clients`: on Windows run it NON-elevated.
 */

#include "cli_common.h"

#ifdef CLI_HAVE_IPC

#include "xrt/xrt_instance.h"
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
		return " = NOT_AUTHORIZED (R1: set DXR_STEREO_CAMERA_DEV_ALLOW=1 in the SERVICE environment)";
	case XRT_ERROR_FEATURE_NOT_SUPPORTED: return " = FEATURE_NOT_SUPPORTED";
	case XRT_ERROR_INPUT_UNSUPPORTED: return " = INPUT_UNSUPPORTED (bad id / format / output)";
	case XRT_ERROR_CLIENT_LIMIT_REACHED: return " = LIMIT_REACHED (8 streams per camera)";
	case XRT_ERROR_IPC_FAILURE: return " = IPC_FAILURE";
	default: return "";
	}
}

static bool
cam_connect(struct ipc_connection *ipc_c)
{
	struct xrt_instance_info ii = {0};
	snprintf(ii.app_info.application_name, sizeof(ii.app_info.application_name), "%s", "displayxr-cli");
	ii.app_info.declared_client_class = XRT_CLIENT_CLASS_DIAG;
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
			printf("       eye          %ux%u (SBS %ux%u), %u views, up to %.1f Hz\n", p.eye_width,
			       p.eye_height, 2 * p.eye_width, p.eye_height, p.view_count, p.max_frame_rate);
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
		return 2;
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

//! Block-match the last frame on a 64x64 grid and print the two dominant
//! disparities (the sim fake: background 12 px, bar 40 px).
static void
report_disparity(const uint8_t *gray, uint32_t pitch, uint32_t eye_w, uint32_t h, char *out, size_t cap)
{
	int vals[512];
	int n = 0;
	for (uint32_t y = 48; y + 64 <= h && n < 512; y += 64) {
		for (uint32_t x = 64; x + 64 <= eye_w && n < 512; x += 64) {
			float d = 0.0f;
			if (u_stereo_camera_estimate_disparity(gray, pitch, eye_w, h, x, y, 64, 64, 64, &d)) {
				vals[n++] = (int)(d + 0.5f);
			}
		}
	}
	if (n == 0) {
		snprintf(out, cap, "n/a (frame too small)");
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
	snprintf(out, cap, "%d px (%d/%d blocks), %d px (%d/%d blocks), range %d..%d px", best_v[0], best_c[0], n,
	         best_v[1], best_c[1], n, vals[0], vals[n - 1]);
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
	for (int i = 3; i < argc; i++) {
		if (strcmp(argv[i], "--raw") == 0) {
			output = XRT_STEREO_CAMERA_OUTPUT_RAW;
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
		} else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
			out_dir = argv[++i];
		} else if (argv[i][0] != '-') {
			id = strtoull(argv[i], NULL, 10);
		}
	}
	if (id == 0) {
		struct xrt_stereo_camera_properties p;
		uint32_t n = 0;
		if (ipc_client_stereo_camera_count(ipc_c, &n) != XRT_SUCCESS || n == 0 ||
		    ipc_client_stereo_camera_get_properties(ipc_c, 0, &p) != XRT_SUCCESS) {
			printf("camera probe: the service exposes no stereo camera.\n");
			return 3;
		}
		id = p.camera_id;
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
		return 2;
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
	printf("stream %llu on camera %llu: %ux%u %s SBS (%s), cap %.1f Hz, ring %u x %llu B (section %llu B)\n",
	       (unsigned long long)sid, (unsigned long long)id, lay.width, lay.height, format_str(lay.format),
	       lay.output == XRT_STEREO_CAMERA_OUTPUT_RECTIFIED ? "RECTIFIED" : "RAW (flagged)", lay.max_frame_rate,
	       lay.slot_count, (unsigned long long)lay.slot_stride, (unsigned long long)lay.section_size);

	int64_t t_start = os_monotonic_get_ns();
	int64_t deadline = seconds > 0.0f ? t_start + (int64_t)(seconds * 1e9) : t_start + 60ll * 1000000000;
	int got = 0, wakes = 0, not_ready = 0;
	uint64_t first_index = 0, last_index = 0, gaps = 0;
	int64_t t_first = 0, t_last = 0;
	struct xrt_stereo_camera_frame_info last;
	memset(&last, 0, sizeof(last));
	while ((seconds > 0.0f || got < frames) && os_monotonic_get_ns() < deadline) {
		if (!wait_wake(wake, 1000)) {
			continue;
		}
		wakes++;
		bool ready = false;
		struct xrt_stereo_camera_frame_info fi;
		xret = ipc_client_stereo_camera_acquire(ipc_c, sid, &ready, &fi);
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
	double span = (double)(t_last - t_first) * 1e-9;
	double rate = (got > 1 && span > 0.0) ? (got - 1) / span : 0.0;
	printf(
	    "received %d frames (index %llu..%llu, %llu source frames not delivered to this stream), %d wakes, "
	    "%d not-ready\n",
	    got, (unsigned long long)first_index, (unsigned long long)last_index, (unsigned long long)gaps, wakes,
	    not_ready);
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
			char disp[160];
			report_disparity(gray, gl.pitch[0], last.width / 2, last.height, disp, sizeof(disp));
			printf("block disparity (left x - right x): %s\n", disp);
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
	}

	unmap_readonly(&map);
	close_wake(wake);
	ipc_client_stereo_camera_stream_stop(ipc_c, sid);
	ipc_client_stereo_camera_stream_destroy(ipc_c, sid);
	return got > 0 ? 0 : 4;
}

int
cli_cmd_camera(int argc, const char **argv)
{
	const char *sub = argc >= 3 ? argv[2] : "list";
	bool json = cli_has_flag(argc, argv, "--json");
	struct ipc_connection ipc_c = {0};
	if (!cam_connect(&ipc_c)) {
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
		printf(
		    "usage: displayxr-cli camera list [--json] | calib <id> [--raw|--rectified] [--json] |\n"
		    "       probe [<id>] [--raw] [--format gray8|nv12|bgra8] [--fps F] [--frames N] [--seconds S] "
		    "[--out DIR]\n");
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
