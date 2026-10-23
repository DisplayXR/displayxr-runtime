// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-tracking-host: the process a viewer-tracking provider runs
 *         in, spawned and supervised by the service
 *         (tracking-provider-discovery.md §4a "Out-of-process hosting").
 *
 *     displayxr-tracking-host --probe --page <name> --parent <pid> [--trust-enforce 0|1]
 *     displayxr-tracking-host --serve --provider <id> --page <name> --parent <pid>
 *                             --generation <n> --channel <token>
 *                             [--geometry w,h,x,y,z] [--trust-enforce 0|1]
 *
 * One binary for every provider. Both modes run the tracking class scan
 * through the shared loader exactly as an in-process runtime does (path
 * guard, the ADR-054 N4b provenance gate, the ABI gate, probe), so the
 * service never loads a provider binary itself.
 *
 *  - `--probe` writes the rank-0 provider's id + describe() into the page's
 *    producer block and exits 0, or exits 10 when no provider is usable.
 *  - `--serve` starts the named provider with a sink that IS the page writer
 *    (u_vpp_write_sample, called from the provider's own thread: the ABI is
 *    unchanged), publishes its status + a heartbeat every 100 ms, and stops
 *    the provider and exits 0 when the service sends the stop byte or its
 *    control channel closes (the service went away).
 *
 * Exit codes: target_viewer_host.h (TARGET_VHOST_EXIT_*).
 *
 * Host-only knob: DXR_TRACKING_HOST_ALLOW_ELEVATED=1 turns the elevation
 * refusal into a WARN (CI runners that are elevated themselves).
 *
 * @ingroup target_common
 */

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_tracking_plugin.h"

#include "target_component_loader.h"
#include "target_tracking_plugin_loader.h"
#include "target_viewer_host.h"

#include "os/os_process.h"
#include "os/os_shm.h"
#include "os/os_time.h"
#include "util/u_file_logging.h"
#include "util/u_git_tag.h"
#include "util/u_logging.h"
#include "util/u_viewer_pose_page.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//! Status / heartbeat period, and the control channel poll.
#define HOST_TICK_MS 100u

struct host_args
{
	bool probe;
	bool serve;
	const char *provider;
	const char *page;
	const char *channel;
	long parent;
	uint32_t generation;
	bool have_geometry;
	struct xrt_tracking_host_display_geometry geo;
	int trust_enforce;
};

static bool
parse_args(int argc, char *argv[], struct host_args *a)
{
	memset(a, 0, sizeof(*a));
	a->trust_enforce = -1;
	for (int i = 1; i < argc; i++) {
		const char *s = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;
		if (strcmp(s, "--probe") == 0) {
			a->probe = true;
		} else if (strcmp(s, "--serve") == 0) {
			a->serve = true;
		} else if (strcmp(s, "--provider") == 0 && v != NULL) {
			a->provider = v;
			i++;
		} else if (strcmp(s, "--page") == 0 && v != NULL) {
			a->page = v;
			i++;
		} else if (strcmp(s, "--channel") == 0 && v != NULL) {
			a->channel = v;
			i++;
		} else if (strcmp(s, "--parent") == 0 && v != NULL) {
			a->parent = strtol(v, NULL, 10);
			i++;
		} else if (strcmp(s, "--generation") == 0 && v != NULL) {
			a->generation = (uint32_t)strtoul(v, NULL, 10);
			i++;
		} else if (strcmp(s, "--geometry") == 0 && v != NULL) {
			float g[5];
			if (sscanf(v, "%f,%f,%f,%f,%f", &g[0], &g[1], &g[2], &g[3], &g[4]) != 5) {
				return false;
			}
			a->geo.struct_size = (uint32_t)sizeof(a->geo);
			a->geo.display_width_m = g[0];
			a->geo.display_height_m = g[1];
			a->geo.nominal_viewer_x_m = g[2];
			a->geo.nominal_viewer_y_m = g[3];
			a->geo.nominal_viewer_z_m = g[4];
			a->have_geometry = true;
			i++;
		} else if (strcmp(s, "--trust-enforce") == 0 && v != NULL) {
			a->trust_enforce = v[0] == '1' ? 1 : 0;
			i++;
		} else {
			return false;
		}
	}
	if (a->probe == a->serve || a->page == NULL) {
		return false;
	}
	if (a->serve && (a->provider == NULL || a->channel == NULL)) {
		return false;
	}
	return true;
}

static int
finish(int code)
{
	U_LOG_W("tracking host: exiting %d (%s)", code, target_vhost_exit_str(code));
	return code;
}


/*
 *
 * The sink: the page writer.
 *
 */

static struct u_vpp_producer g_producer;

static void
sink_push(const struct xrt_tracking_sink *sink, const struct xrt_tracking_viewer_sample *s)
{
	(void)sink;
	// One producing thread: the provider's own (the ABI's contract).
	u_vpp_write_sample(&g_producer, s);
}

//! Fill the producer block from the provider's describe().
static void
describe_into(const struct xrt_tracking_plugin_iface *iface,
              struct xrt_tracking_plugin_instance *inst,
              const char *row_id,
              struct u_vpp_producer_status *ps)
{
	snprintf(ps->provider_id, sizeof(ps->provider_id), "%s", row_id);
	ps->tracks = XRT_TRACKING_TRACKS_EYE_PAIR;
	if (iface->describe != NULL) {
		struct xrt_tracking_provider_info info;
		memset(&info, 0, sizeof(info));
		info.struct_size = (uint32_t)sizeof(info);
		if (iface->describe(inst, &info) == XRT_SUCCESS) {
			info.source[sizeof(info.source) - 1] = '\0';
			info.camera_key[sizeof(info.camera_key) - 1] = '\0';
			ps->tracks = info.tracks;
			snprintf(ps->source, sizeof(ps->source), "%s", info.source);
			snprintf(ps->camera_key, sizeof(ps->camera_key), "%s", info.camera_key);
		}
	}
}

int
main(int argc, char *argv[])
{
#ifdef XRT_OS_WINDOWS
	u_file_logging_set_name("tracking-host");
#endif
	struct host_args a;
	if (!parse_args(argc, argv, &a)) {
		fprintf(
		    stderr,
		    "usage: displayxr-tracking-host --probe|--serve --page <name> [--provider <id>] [--parent <pid>]\n"
		    "       [--generation <n>] [--channel <token>] [--geometry w,h,x,y,z] [--trust-enforce 0|1]\n");
		return finish(TARGET_VHOST_EXIT_USAGE);
	}
	U_LOG_W("tracking host: %s provider '%s' page '%s' parent %ld generation %u tag %s",
	        a.probe ? "probe" : "serve", a.provider != NULL ? a.provider : "(rank 0)", a.page, a.parent,
	        a.generation, u_git_tag);

	// Integrity: inherit the service's medium token, never elevated.
	if (os_process_is_elevated()) {
		const char *allow = getenv("DXR_TRACKING_HOST_ALLOW_ELEVATED");
		if (allow == NULL || strcmp(allow, "1") != 0) {
			U_LOG_E("tracking host: refusing to run elevated");
			return finish(TARGET_VHOST_EXIT_ELEVATED);
		}
		U_LOG_W("tracking host: running ELEVATED because DXR_TRACKING_HOST_ALLOW_ELEVATED=1 (CI / dev only)");
	}
	if (a.trust_enforce >= 0) {
		target_component_trust_set_enforce_override(a.trust_enforce);
		U_LOG_W("tracking host: provenance gate pinned to %s by the parent (ADR-054 N4b)",
		        a.trust_enforce != 0 ? "enforce" : "audit");
	}
	if (a.parent > 0) {
		const int pw = os_process_watch_parent(a.parent, TARGET_VHOST_EXIT_ORDERLY);
		if (pw != 0) {
			U_LOG_W("tracking host: parent-death watch on %ld not armed (%d); relying on the channel",
			        a.parent, pw);
		}
	}

	// The control channel first: the service waits a bounded time for it.
	struct os_channel *chan = NULL;
	if (a.serve && os_channel_open_child(a.channel, &chan) != 0) {
		U_LOG_E("tracking host: control channel '%s' did not open", a.channel);
		return finish(TARGET_VHOST_EXIT_CHANNEL);
	}

	struct os_shm shm;
	if (os_shm_open(a.page, U_VPP_PAGE_BYTES, true, &shm) != 0 || !u_vpp_attach_check(shm.map, shm.size)) {
		U_LOG_E("tracking host: pose page '%s' missing or invalid", a.page);
		os_shm_close(&shm);
		os_channel_destroy(&chan);
		return finish(TARGET_VHOST_EXIT_PAGE);
	}

	if (a.have_geometry) {
		target_tracking_plugin_set_display_geometry(&a.geo);
	}

	// The scan: the shared loader's path guard, provenance gate, ABI gate and probe.
	const int count = target_tracking_plugin_get_count();
	int index = -1;
	for (int i = 0; i < count && index < 0; i++) {
		if (a.probe || strcmp(target_tracking_plugin_get_row_id(i), a.provider) == 0) {
			index = i;
		}
	}
	if (index < 0) {
		U_LOG_W("tracking host: provider '%s' is not usable here (%d resident)",
		        a.provider != NULL ? a.provider : "(any)", count);
		os_shm_close(&shm);
		os_channel_destroy(&chan);
		return finish(TARGET_VHOST_EXIT_NO_PROVIDER);
	}
	const struct xrt_tracking_plugin_iface *iface = target_tracking_plugin_get_iface(index);
	struct xrt_tracking_plugin_instance *inst = target_tracking_plugin_get_instance(index);
	const char *row_id = target_tracking_plugin_get_row_id(index);

	struct u_vpp_producer_status ps;
	memset(&ps, 0, sizeof(ps));
	describe_into(iface, inst, row_id, &ps);

	if (a.probe) {
		// The answer the service decides on: who would serve, and what it tracks.
		ps.pid = 0;
		ps.provider_status = XRT_TRACKING_PROVIDER_STATUS_STOPPED;
		u_vpp_write_producer(shm.map, &ps);
		U_LOG_W("tracking host: probe: provider '%s' usable (%s)", row_id, ps.source);
		os_shm_close(&shm);
		return finish(TARGET_VHOST_EXIT_ORDERLY);
	}

	// Serve: the sink is the page writer.
	u_vpp_producer_begin(&g_producer, shm.map);
	struct xrt_tracking_sink sink;
	memset(&sink, 0, sizeof(sink));
	sink.struct_size = (uint32_t)sizeof(sink);
	sink.push = sink_push;
	const xrt_result_t xret = iface->start(inst, &sink);
	if (xret != XRT_SUCCESS) {
		U_LOG_E("tracking host: provider '%s' failed to start (%d)", row_id, (int)xret);
		os_shm_close(&shm);
		os_channel_destroy(&chan);
		return finish(TARGET_VHOST_EXIT_START_FAILED);
	}

	// Hello: this pid + this generation in the producer block.
	ps.pid = os_process_current_pid();
	ps.generation = a.generation;
	ps.provider_status = XRT_TRACKING_PROVIDER_STATUS_RUNNING;
	u_vpp_heartbeat(shm.map, os_monotonic_get_ns());
	u_vpp_write_producer(shm.map, &ps);
	U_LOG_W("tracking host: provider '%s' started (%s%s%s)", row_id, ps.source, ps.camera_key[0] ? ", camera " : "",
	        ps.camera_key);

	for (;;) {
		uint8_t cmd = 0;
		const int r = os_channel_recv(chan, &cmd, 1, NULL, NULL, HOST_TICK_MS);
		if (r == 1 && cmd == 'q') {
			U_LOG_W("tracking host: stop requested");
			break;
		}
		if (r < 0) {
			U_LOG_W("tracking host: control channel closed (%d) — the service is gone", r);
			break;
		}
		u_vpp_heartbeat(shm.map, os_monotonic_get_ns());
		const uint32_t st = XRT_TRACKING_PLUGIN_IFACE_HAS(iface, get_status)
		                        ? (uint32_t)iface->get_status(inst)
		                        : (uint32_t)XRT_TRACKING_PROVIDER_STATUS_RUNNING;
		if (st != ps.provider_status) {
			ps.provider_status = st;
			u_vpp_write_producer(shm.map, &ps);
		}
	}

	iface->stop(inst);
	ps.provider_status = XRT_TRACKING_PROVIDER_STATUS_STOPPED;
	u_vpp_write_producer(shm.map, &ps);
	os_shm_close(&shm);
	os_channel_destroy(&chan);
	return finish(TARGET_VHOST_EXIT_ORDERLY);
}
