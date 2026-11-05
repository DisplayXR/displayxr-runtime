// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  C side of tests_stereo_camera_manager: a minimal ipc_server +
 *         ipc_client_state around the REAL camera manager
 *         (ipc_server_stereo_camera.c, compiled into the test), its IPC
 *         handlers called directly, an in-memory consent store, and the UVC
 *         source on its fake backend. No pipe, no service process.
 */

#include "tests_stereo_camera_manager_glue.h"

#include "server/ipc_server.h"
#include "server/ipc_server_stereo_camera.h"
#include "ipc_server_generated.h"

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_instance.h"
#include "xrt/xrt_plugin.h"
#include "os/os_time.h"
#include "util/u_camera_consent.h"
#include "util/u_app_share_fake.h"

#include <stddef.h>
#include <stdio.h>

#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
#ifdef XRT_OS_MACOS
#include <CoreFoundation/CoreFoundation.h>
#endif
#include "util/u_handles.h"

/*
 * What ipc_server_stereo_camera.c needs from the rest of ipc_server (which
 * this test does not link: the handler file drags in every compositor).
 */

const char *
ipc_server_client_class_str(uint32_t client_class)
{
	(void)client_class;
	return "TEST";
}

// The CAMERA_CONSUMER quota the in-process consumers share with connections:
// no connections here, the real quota (4).
uint32_t
ipc_server_client_class_count(struct ipc_server *s, uint32_t client_class)
{
	(void)s;
	(void)client_class;
	return 0;
}

uint32_t
ipc_server_client_class_quota(const struct ipc_server *s, uint32_t client_class)
{
	(void)s;
	return client_class == XRT_CLIENT_CLASS_CAMERA_CONSUMER ? 4u : 0u;
}

#ifdef XRT_OS_MACOS
bool
ipc_server_macos_pid_app_visible(long pid)
{
	(void)pid;
	return true;
}
#endif


/*
 * In-memory consent: every executable has a stored Allow, the OS switch is on.
 */

static bool
store_get(void *ctx, const char *exe, enum u_camera_consent_stored *out)
{
	(void)ctx;
	(void)exe;
	*out = U_CAMERA_CONSENT_STORED_ALLOW;
	return true;
}

static bool
store_sharing(void *ctx)
{
	(void)ctx;
	return true;
}

static const struct u_camera_consent_store_ops k_store = {
    .get = store_get,
    .sharing_enabled = store_sharing,
};

static bool
env_os_allowed(void *ctx, const char *exe)
{
	(void)ctx;
	(void)exe;
	return true;
}

static bool
env_user_writable(void *ctx, const char *exe)
{
	(void)ctx;
	(void)exe;
	return false;
}

static const struct u_camera_consent_env_ops k_env = {
    .os_camera_allowed = env_os_allowed,
    .path_user_writable = env_user_writable,
};


struct scm_fixture
{
	struct ipc_server server;
	struct ipc_client_state ics;
	struct ipc_server_stereo_camera *mgr;
	struct u_stereo_uvc_config cfg;
	// Read-only view of the last stream's ring.
	const uint8_t *map;
	uint64_t map_size;
	uint64_t slot_stride;
	//! ADR-061: the app metadata of the last acquired frame.
	struct xrt_app_share_frame_info last_app;
#ifdef XRT_OS_WINDOWS
	HANDLE section;
#else
	int section;
#endif
};

/*
 * A fake display plug-in whose camera list a test edits between rescans
 * (#1961): no calibration, GRAY8, frames never arrive (wait_frame times out).
 */

static struct
{
	uint32_t count;
	char identity[XRT_STEREO_CAMERA_MAX_CAMERAS][128];
	char name[XRT_STEREO_CAMERA_MAX_CAMERAS][128];
	uint64_t monitor[XRT_STEREO_CAMERA_MAX_CAMERAS];
	uint32_t orientation[XRT_STEREO_CAMERA_MAX_CAMERAS]; //!< plug-in enum; 0 = do not write the field
	uint32_t enumerate_calls;
} g_fake;

struct xrt_plugin_stereo_camera
{
	int dummy;
};

static uint32_t
fake_enumerate(struct xrt_plugin_instance *inst, uint32_t capacity, struct xrt_plugin_stereo_camera_info *out)
{
	(void)inst;
	g_fake.enumerate_calls++;
	for (uint32_t i = 0; i < g_fake.count && i < capacity && out != NULL; i++) {
		struct xrt_plugin_stereo_camera_info *o = &out[i];
		snprintf(o->display_name, sizeof(o->display_name), "%s", g_fake.name[i]);
		snprintf(o->device_identity, sizeof(o->device_identity), "%s", g_fake.identity[i]);
		o->platform_device_hint[0] = '\0';
		o->flags = XRT_PLUGIN_STEREO_CAMERA_USER_FACING;
		o->eye_width = 640;
		o->eye_height = 480;
		o->max_frame_rate = 0.0f;
		o->native_format = XRT_PLUGIN_STEREO_CAMERA_FORMAT_GRAY8;
		// Appended field: written only when the runtime's struct_size covers it.
		const size_t need = offsetof(struct xrt_plugin_stereo_camera_info, monitor_id) + sizeof(o->monitor_id);
		if (o->struct_size >= need) {
			o->monitor_id = g_fake.monitor[i];
		}
		// Spec v7, same gate; 0 = behave like a plug-in that predates the field.
		const size_t need_o =
		    offsetof(struct xrt_plugin_stereo_camera_info, frame_orientation) + sizeof(o->frame_orientation);
		if (o->struct_size >= need_o && g_fake.orientation[i] != 0) {
			o->frame_orientation = g_fake.orientation[i];
		}
	}
	return g_fake.count;
}

static xrt_result_t
fake_get_calibration(struct xrt_plugin_instance *inst, uint32_t index, struct xrt_plugin_stereo_camera_calibration *out)
{
	(void)inst;
	(void)index;
	(void)out;
	return XRT_ERROR_FEATURE_NOT_SUPPORTED;
}

static xrt_result_t
fake_open(struct xrt_plugin_instance *inst, uint32_t index, struct xrt_plugin_stereo_camera **out_cam)
{
	(void)inst;
	(void)index;
	*out_cam = (struct xrt_plugin_stereo_camera *)calloc(1, sizeof(struct xrt_plugin_stereo_camera));
	return *out_cam != NULL ? XRT_SUCCESS : XRT_ERROR_ALLOCATION;
}

static uint32_t
fake_wait_frame(struct xrt_plugin_stereo_camera *cam, int64_t timeout_ns, struct xrt_plugin_stereo_camera_frame *out)
{
	(void)cam;
	(void)out;
	os_nanosleep(timeout_ns < 10000000 ? timeout_ns : 10000000);
	return XRT_PLUGIN_STEREO_CAMERA_WAIT_TIMEOUT;
}

static void
fake_release_frame(struct xrt_plugin_stereo_camera *cam)
{
	(void)cam;
}

static void
fake_close(struct xrt_plugin_stereo_camera *cam)
{
	free(cam);
}

static struct xrt_plugin_iface g_fake_iface;
static struct xrt_instance g_fake_xinst;

static bool
fake_get_active_plugin(struct xrt_instance *xinst,
                       const struct xrt_plugin_iface **out_iface,
                       struct xrt_plugin_instance **out_inst)
{
	(void)xinst;
	*out_iface = &g_fake_iface;
	*out_inst = NULL;
	return true;
}

void
scm_fake_plugin_set(const char *const *identities, const char *const *names, uint32_t count)
{
	g_fake.count = count > XRT_STEREO_CAMERA_MAX_CAMERAS ? XRT_STEREO_CAMERA_MAX_CAMERAS : count;
	for (uint32_t i = 0; i < g_fake.count; i++) {
		snprintf(g_fake.identity[i], sizeof(g_fake.identity[i]), "%s", identities[i]);
		snprintf(g_fake.name[i], sizeof(g_fake.name[i]), "%s", names[i]);
		g_fake.monitor[i] = 0;
		g_fake.orientation[i] = 0;
	}
}

void
scm_fake_plugin_set_orientations(const uint32_t *plugin_values, uint32_t count)
{
	for (uint32_t i = 0; i < XRT_STEREO_CAMERA_MAX_CAMERAS; i++) {
		g_fake.orientation[i] = i < count ? plugin_values[i] : 0;
	}
}

void
scm_fake_plugin_set_monitors(const uint64_t *monitor_ids, uint32_t count)
{
	for (uint32_t i = 0; i < XRT_STEREO_CAMERA_MAX_CAMERAS; i++) {
		g_fake.monitor[i] = i < count ? monitor_ids[i] : 0;
	}
}

uint32_t
scm_fake_plugin_enumerate_calls(void)
{
	return g_fake.enumerate_calls;
}

static enum u_camera_consent_prompt_answer
scm_share_prompt(void *ctx, const char *consumer_exe, long consumer_pid, const char *app_name, const char *app_exe,
                 uint32_t timeout_ms);

static struct scm_fixture *
create_with_apps(const struct u_stereo_uvc_config *cfg,
                 struct xrt_instance *xinst,
                 int32_t rescan_period_ms,
                 int32_t dev_override,
                 const struct scm_app_options *ao);

static struct scm_fixture *
create_with(const struct u_stereo_uvc_config *cfg,
            struct xrt_instance *xinst,
            int32_t rescan_period_ms,
            int32_t dev_override)
{
	// Existing tests: no app entries, whatever the environment says.
	const struct scm_app_options none = {.dev = -1, .fake = -1};
	return create_with_apps(cfg, xinst, rescan_period_ms, dev_override, &none);
}

static struct scm_fixture *
create_with_apps(const struct u_stereo_uvc_config *cfg,
                 struct xrt_instance *xinst,
                 int32_t rescan_period_ms,
                 int32_t dev_override,
                 const struct scm_app_options *ao)
{
	struct scm_fixture *f = (struct scm_fixture *)calloc(1, sizeof(*f));
	if (f == NULL) {
		return NULL;
	}
	if (cfg != NULL) {
		f->cfg = *cfg;
	}
	struct ipc_server_stereo_camera_options o;
	memset(&o, 0, sizeof(o));
	o.consent_store = &k_store;
	o.consent_env = &k_env;
	o.uvc_config = &f->cfg; // an empty config (count 0) = no UVC camera
	o.rescan_period_ms = rescan_period_ms;
	o.dev_override = dev_override;
	o.app_share_dev = ao->dev;
	o.app_share_fake = ao->fake;
	o.app_share_fake_link = ao->fake_link != 0 ? ao->fake_link : -1;
	o.app_linger_ms = ao->linger_ms;
	o.share_prompt_mode = ao->prompt_mode;
	o.share_resolution = 1; // 720p, whatever the user store says
	o.share_prompt = scm_share_prompt;
	f->mgr = ipc_server_stereo_camera_create_ex(xinst, &o);
	f->server.stereo_camera = f->mgr;
	ipc_server_stereo_camera_attach_server(f->mgr, &f->server);
	for (uint32_t i = 0; i < IPC_MAX_CLIENTS; i++) {
		f->server.threads[i].ics.server_thread_index = -1; // no connection
		f->server.threads[i].ics.server = &f->server;
	}
	if (ao->consumer_sees_apps) {
		f->ics.client_state.info.stereo_camera_client_flags |= XRT_STEREO_CAMERA_CLIENT_INTERNAL_APP_SHARE;
	}
	f->ics.server = &f->server;
	f->ics.client_state.client_class = XRT_CLIENT_CLASS_CAMERA_CONSUMER;
#ifdef XRT_OS_WINDOWS
	f->ics.peer_pid = (long)GetCurrentProcessId();
	f->section = NULL;
#else
	f->ics.peer_pid = (long)getpid();
	f->section = -1;
#endif
	return f;
}

struct scm_fixture *
scm_create(const struct u_stereo_uvc_config *cfg)
{
	return create_with(cfg, NULL, 0, 0);
}

struct scm_fixture *
scm_create_fake_plugin(void)
{
	return scm_create_fake_plugin_ex(0);
}

struct scm_fixture *
scm_create_fake_plugin_ex(int32_t dev_override)
{
	memset(&g_fake_iface, 0, sizeof(g_fake_iface));
	g_fake_iface.struct_size = (uint32_t)sizeof(g_fake_iface);
	g_fake_iface.stereo_camera_enumerate = fake_enumerate;
	g_fake_iface.stereo_camera_get_calibration = fake_get_calibration;
	g_fake_iface.stereo_camera_open = fake_open;
	g_fake_iface.stereo_camera_wait_frame = fake_wait_frame;
	g_fake_iface.stereo_camera_release_frame = fake_release_frame;
	g_fake_iface.stereo_camera_close = fake_close;
	memset(&g_fake_xinst, 0, sizeof(g_fake_xinst));
	g_fake_xinst.get_active_plugin = fake_get_active_plugin;
	return create_with(NULL, &g_fake_xinst, -1, dev_override); // the test drives every rescan
}

static struct
{
	uint32_t count;
	char ids[XRT_PLUGIN_CLAIMED_CAMERAS_MAX][XRT_PLUGIN_CLAIMED_CAMERA_ID_MAX];
} g_claims;

static uint32_t
fake_get_claimed_cameras(struct xrt_plugin_instance *inst,
                         uint32_t capacity,
                         char (*out_ids)[XRT_PLUGIN_CLAIMED_CAMERA_ID_MAX])
{
	(void)inst;
	for (uint32_t i = 0; i < g_claims.count && i < capacity && out_ids != NULL; i++) {
		snprintf(out_ids[i], XRT_PLUGIN_CLAIMED_CAMERA_ID_MAX, "%s", g_claims.ids[i]);
	}
	return g_claims.count;
}

void
scm_fake_claims_set(const char *const *ids, uint32_t count)
{
	g_claims.count = count > XRT_PLUGIN_CLAIMED_CAMERAS_MAX ? XRT_PLUGIN_CLAIMED_CAMERAS_MAX : count;
	for (uint32_t i = 0; i < g_claims.count; i++) {
		snprintf(g_claims.ids[i], sizeof(g_claims.ids[i]), "%s", ids[i]);
	}
}

struct scm_fixture *
scm_create_claiming_plugin(const struct u_stereo_uvc_config *cfg)
{
	memset(&g_fake_iface, 0, sizeof(g_fake_iface));
	g_fake_iface.struct_size = (uint32_t)sizeof(g_fake_iface);
	g_fake_iface.get_claimed_cameras = fake_get_claimed_cameras; // no camera slots at all
	memset(&g_fake_xinst, 0, sizeof(g_fake_xinst));
	g_fake_xinst.get_active_plugin = fake_get_active_plugin;
	return create_with(cfg, &g_fake_xinst, -1, 0);
}

bool
scm_rescan(struct scm_fixture *f)
{
	return ipc_server_stereo_camera_rescan(f->mgr);
}

bool
scm_poll_event(struct scm_fixture *f, struct xrt_stereo_camera_event *out)
{
	bool has = false;
	ipc_handle_stereo_camera_poll_event(&f->ics, &has, out);
	return has;
}

xrt_result_t
scm_set_sharing(struct scm_fixture *f, bool enabled)
{
	(void)f;
	return ipc_server_stereo_camera_set_sharing(enabled, "test") ? XRT_SUCCESS : XRT_ERROR_FEATURE_NOT_SUPPORTED;
}

static void
unmap(struct scm_fixture *f)
{
	if (f->map != NULL) {
#ifdef XRT_OS_WINDOWS
		UnmapViewOfFile((LPCVOID)f->map);
#else
		munmap((void *)f->map, (size_t)f->map_size);
#endif
		f->map = NULL;
	}
#ifdef XRT_OS_WINDOWS
	if (f->section != NULL) {
		CloseHandle(f->section);
		f->section = NULL;
	}
#else
	if (f->section >= 0) {
		close(f->section);
		f->section = -1;
	}
#endif
}

void
scm_destroy(struct scm_fixture *f)
{
	if (f == NULL) {
		return;
	}
	unmap(f);
	ipc_server_client_stereo_camera_link_release(&f->ics);
	ipc_server_client_stereo_camera_release(&f->ics);
	ipc_server_stereo_camera_destroy(&f->mgr);
	free(f);
}

uint32_t
scm_count(struct scm_fixture *f)
{
	uint32_t n = 0;
	ipc_handle_stereo_camera_count(&f->ics, &n);
	return n;
}

xrt_result_t
scm_properties(struct scm_fixture *f, uint32_t index, struct xrt_stereo_camera_properties *out)
{
	return ipc_handle_stereo_camera_get_properties(&f->ics, index, out);
}

xrt_result_t
scm_calibration(struct scm_fixture *f, uint64_t camera_id, uint32_t output, struct xrt_stereo_camera_calibration *out)
{
	return ipc_handle_stereo_camera_get_calibration(&f->ics, camera_id, output, out);
}

xrt_result_t
scm_stream_create(struct scm_fixture *f, const struct xrt_stereo_camera_stream_request *req, uint64_t *out_id)
{
	return ipc_handle_stereo_camera_stream_create(&f->ics, req, out_id);
}

xrt_result_t
scm_stream_start(struct scm_fixture *f, uint64_t id)
{
	return ipc_handle_stereo_camera_stream_start(&f->ics, id);
}

xrt_result_t
scm_stream_map(struct scm_fixture *f, uint64_t id, struct xrt_stereo_camera_stream_layout *out_layout)
{
	unmap(f);
	xrt_shmem_handle_t h = XRT_SHMEM_HANDLE_INVALID;
	uint32_t count = 0;
	xrt_result_t xret = ipc_handle_stereo_camera_stream_get_section(&f->ics, id, out_layout, 1, &h, &count);
	if (xret != XRT_SUCCESS || count != 1) {
		return xret != XRT_SUCCESS ? xret : XRT_ERROR_IPC_FAILURE;
	}
	// The handler hands out the manager's own read-only handle (the dispatch
	// would duplicate it into the peer): map a duplicate, read-only.
#ifdef XRT_OS_WINDOWS
	if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &f->section, 0, FALSE,
	                     DUPLICATE_SAME_ACCESS)) {
		return XRT_ERROR_IPC_FAILURE;
	}
	f->map = (const uint8_t *)MapViewOfFile(f->section, FILE_MAP_READ, 0, 0, (SIZE_T)out_layout->section_size);
#else
	f->section = dup(h);
	void *p = mmap(NULL, (size_t)out_layout->section_size, PROT_READ, MAP_SHARED, f->section, 0);
	f->map = p == MAP_FAILED ? NULL : (const uint8_t *)p;
#endif
	f->map_size = out_layout->section_size;
	f->slot_stride = out_layout->slot_stride;
	return f->map != NULL ? XRT_SUCCESS : XRT_ERROR_IPC_FAILURE;
}

xrt_result_t
scm_acquire(struct scm_fixture *f,
            uint64_t id,
            bool *out_ready,
            struct xrt_stereo_camera_frame_info *out_frame,
            const uint8_t **out_slot)
{
	xrt_result_t xret = ipc_handle_stereo_camera_acquire(&f->ics, id, out_ready, out_frame, &f->last_app);
	*out_slot = NULL;
	if (xret == XRT_SUCCESS && *out_ready && f->map != NULL) {
		*out_slot = f->map + (uint64_t)out_frame->slot * f->slot_stride;
	}
	return xret;
}

xrt_result_t
scm_stats(struct scm_fixture *f, uint64_t id, struct xrt_stereo_camera_stream_stats *out)
{
	return ipc_handle_stereo_camera_stream_stats(&f->ics, id, out);
}

xrt_result_t
scm_stream_destroy(struct scm_fixture *f, uint64_t id)
{
	unmap(f);
	return ipc_handle_stereo_camera_stream_destroy(&f->ics, id);
}

static bool g_scm_foreground = false;

static bool
scm_consumer_foreground(void *ctx)
{
	(void)ctx;
	return g_scm_foreground;
}

void
scm_set_front_rule(bool enabled)
{
	if (!enabled) {
		ipc_server_stereo_camera_set_platform(NULL);
		return;
	}
	struct ipc_server_stereo_camera_platform p;
	memset(&p, 0, sizeof(p));
	p.user_facing_requires_presenting = true;
	p.consumer_foreground = scm_consumer_foreground;
	g_scm_foreground = false;
	ipc_server_stereo_camera_set_platform(&p);
}

void
scm_set_live_weave(struct scm_fixture *f, bool live)
{
	// The front rule scans the server's connection table for the peer's uid:
	// one other connection of the same app that submits weaves.
	volatile struct ipc_client_state *o = &f->server.threads[0].ics;
	o->server_thread_index = 0;
	o->peer_uid = f->ics.peer_uid;
	o->last_weave_ns = live ? (int64_t)os_monotonic_get_ns() : 0;
}

void
scm_set_present_owner(struct scm_fixture *f, bool present_owner, bool foreground)
{
	volatile struct ipc_client_state *o = &f->server.threads[0].ics;
	o->server_thread_index = 0;
	o->peer_uid = f->ics.peer_uid;
	o->last_weave_ns = 0;
	o->client_state.client_class =
	    present_owner ? XRT_CLIENT_CLASS_PRESENT_OWNER : XRT_CLIENT_CLASS_CAMERA_CONSUMER;
	g_scm_foreground = foreground;
}


/*
 * ADR-061 app share.
 */

static int g_share_answer = U_CAMERA_CONSENT_PROMPT_ALLOW;
static int g_share_prompts = 0;

static enum u_camera_consent_prompt_answer
scm_share_prompt(void *ctx, const char *consumer_exe, long consumer_pid, const char *app_name, const char *app_exe,
                 uint32_t timeout_ms)
{
	(void)ctx;
	(void)consumer_exe;
	(void)consumer_pid;
	(void)app_name;
	(void)app_exe;
	(void)timeout_ms;
	g_share_prompts++;
	return (enum u_camera_consent_prompt_answer)g_share_answer;
}

void
scm_set_share_answer(int answer)
{
	g_share_answer = answer;
	g_share_prompts = 0;
}

int
scm_share_prompts(void)
{
	return g_share_prompts;
}

struct scm_fixture *
scm_create_apps(const struct scm_app_options *o)
{
	memset(&g_fake_iface, 0, sizeof(g_fake_iface));
	g_fake_iface.struct_size = (uint32_t)sizeof(g_fake_iface);
	g_fake_iface.stereo_camera_enumerate = fake_enumerate;
	g_fake_iface.stereo_camera_get_calibration = fake_get_calibration;
	g_fake_iface.stereo_camera_open = fake_open;
	g_fake_iface.stereo_camera_wait_frame = fake_wait_frame;
	g_fake_iface.stereo_camera_release_frame = fake_release_frame;
	g_fake_iface.stereo_camera_close = fake_close;
	memset(&g_fake_xinst, 0, sizeof(g_fake_xinst));
	g_fake_xinst.get_active_plugin = fake_get_active_plugin;
	g_share_prompts = 0;
	return create_with_apps(NULL, &g_fake_xinst, -1, -1, o);
}

void
scm_set_consumer_sees_apps(struct scm_fixture *f, bool on)
{
	if (on) {
		f->ics.client_state.info.stereo_camera_client_flags |= XRT_STEREO_CAMERA_CLIENT_INTERNAL_APP_SHARE;
	} else {
		f->ics.client_state.info.stereo_camera_client_flags &= ~XRT_STEREO_CAMERA_CLIENT_INTERNAL_APP_SHARE;
	}
}

void
scm_app_connect(struct scm_fixture *f, uint32_t slot, uint32_t client_id, long pid, const char *name, uint32_t cls,
                bool opted_out)
{
	volatile struct ipc_client_state *o = &f->server.threads[slot].ics;
	o->server_thread_index = (int)slot;
	o->peer_pid = pid;
	o->client_state.id = client_id;
	o->client_state.client_class = cls;
	snprintf((char *)o->client_state.info.application_name, sizeof(o->client_state.info.application_name), "%s",
	         name);
	// Never dereferenced by the manager: it only asks "is there a session + compositor?".
	o->xs = (struct xrt_session *)(uintptr_t)0x1000;
	o->xc = (struct xrt_compositor *)(uintptr_t)0x2000;
	o->app_share_opt_out = opted_out;
}

void
scm_app_disconnect(struct scm_fixture *f, uint32_t slot)
{
	volatile struct ipc_client_state *o = &f->server.threads[slot].ics;
	o->xs = NULL;
	o->xc = NULL;
	o->server_thread_index = -1;
}

void
scm_app_session_ending(struct scm_fixture *f, uint32_t slot)
{
	ipc_server_stereo_camera_app_session_ending(&f->server.threads[slot].ics);
}

bool
scm_rescan_apps(struct scm_fixture *f)
{
	return ipc_server_stereo_camera_rescan_apps(f->mgr);
}

xrt_result_t
scm_app_source(struct scm_fixture *f, uint64_t camera_id, struct xrt_app_share_source_info *out)
{
	return ipc_handle_stereo_camera_get_app_source(&f->ics, camera_id, out);
}

struct xrt_app_share_frame_info
scm_last_app(struct scm_fixture *f)
{
	return f->last_app;
}

/*
 * The fake share tap: what the Windows service compositor will supply.
 */

static int g_tap_attached = 0;

static bool
tap_query(void *ctx, uint32_t client_id, struct xrt_app_share_source_info *io, int64_t *out_last_commit_ns)
{
	(void)ctx;
	(void)client_id;
	io->availability = XRT_APP_SHARE_AVAILABLE;
	io->content_eye_width = 1600;
	io->content_eye_height = 900;
	io->window_x = 10;
	io->window_y = 20;
	io->window_width = 1600;
	io->window_height = 900;
	io->view_count = 4;
	io->active_view_count = 2;
	*out_last_commit_ns = 0;
	return true;
}

static xrt_result_t
tap_attach(void *ctx, uint32_t client_id, uint32_t eye_w, uint32_t eye_h, float fps, void **out_tap)
{
	(void)ctx;
	(void)client_id;
	*out_tap = u_app_share_fake_create(eye_w, eye_h, fps);
	if (*out_tap == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	g_tap_attached++;
	return XRT_SUCCESS;
}

static uint32_t
tap_wait(void *ctx,
         void *tap,
         int64_t timeout_ns,
         struct xrt_plugin_stereo_camera_frame *out,
         struct xrt_app_share_frame_info *info)
{
	(void)ctx;
	struct u_app_share_fake_frame ff;
	if (!u_app_share_fake_next((struct u_app_share_fake *)tap, timeout_ns, &ff)) {
		return XRT_PLUGIN_STEREO_CAMERA_WAIT_TIMEOUT;
	}
	out->sequence = ff.sequence;
	out->time_ns = ff.time_ns;
	out->width = ff.width;
	out->height = ff.height;
	out->format = XRT_PLUGIN_STEREO_CAMERA_FORMAT_NV12;
	out->planes[0] = ff.planes[0];
	out->planes[1] = ff.planes[1];
	out->pitches[0] = ff.pitches[0];
	out->pitches[1] = ff.pitches[1];
	// A 16:9 source letterboxed nowhere, but report a sub-rect + mono to see it carried.
	info->content_x = 0;
	info->content_y = 8;
	info->content_width = ff.width / 2;
	info->content_height = ff.height - 16;
	info->mono = 1;
	info->content_generation = 3;
	info->zero_disparity_shift_frac = 0.0f;
	info->commit_time_ns = ff.time_ns;
	return XRT_PLUGIN_STEREO_CAMERA_WAIT_OK;
}

static int g_tap_releases = 0;

static void
tap_release(void *ctx, void *tap)
{
	(void)ctx;
	(void)tap;
	// The contract: never called with the manager lock held. This takes it (a
	// non-recursive mutex): a violation hangs here instead of passing.
	(void)ipc_server_stereo_camera_is_session_locked();
	g_tap_releases++;
}

int
scm_tap_releases(void)
{
	return g_tap_releases;
}

void
scm_apps_changed(struct scm_fixture *f)
{
	ipc_server_stereo_camera_apps_changed(f->mgr);
}

static void
tap_detach(void *ctx, void *tap)
{
	(void)ctx;
	struct u_app_share_fake *f = (struct u_app_share_fake *)tap;
	u_app_share_fake_destroy(&f);
	g_tap_attached--;
}

void
scm_set_tap(bool enabled)
{
	if (!enabled) {
		ipc_server_stereo_camera_set_platform(NULL);
		return;
	}
	struct ipc_server_stereo_camera_platform p;
	memset(&p, 0, sizeof(p));
	p.share_tap_query = tap_query;
	p.share_tap_attach = tap_attach;
	p.share_tap_wait_frame = tap_wait;
	p.share_tap_release_frame = tap_release;
	p.share_tap_detach = tap_detach;
	ipc_server_stereo_camera_set_platform(&p);
}

int
scm_tap_attached(void)
{
	return g_tap_attached;
}

/*
 * ADR-061 arm 2: service links.
 */

void
scm_link_connect(struct scm_fixture *f, uint32_t slot, uint32_t client_id, long pid, uint32_t cls)
{
	volatile struct ipc_client_state *o = &f->server.threads[slot].ics;
	o->server_thread_index = (int)slot;
	o->peer_pid = pid;
	o->client_state.id = client_id;
	o->client_state.client_class = cls;
	o->xs = NULL; // sessionless: the session is in the app process
	o->xc = NULL;
	o->app_share_opt_out = false;
}

xrt_result_t
scm_link_register(struct scm_fixture *f, uint32_t slot, const struct xrt_share_link_registration *reg)
{
	return ipc_handle_service_link_register(&f->server.threads[slot].ics, reg);
}

xrt_result_t
scm_link_poll(struct scm_fixture *f, uint32_t slot, struct xrt_share_link_poll *out)
{
	return ipc_handle_service_link_share_poll(&f->server.threads[slot].ics, out);
}

//! A real, closable stand-in for a shared texture handle (the importer below only closes it).
static xrt_graphics_buffer_handle_t
dummy_buffer_handle(void)
{
#if defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_WIN32_HANDLE)
	return CreateEventW(NULL, FALSE, FALSE, NULL);
#elif defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_METAL)
	const uint8_t b = 0;
	return (xrt_graphics_buffer_handle_t)CFDataCreate(NULL, &b, 1);
#elif defined(XRT_GRAPHICS_BUFFER_HANDLE_IS_FD)
	return open("/dev/null", O_RDONLY);
#else
	return XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
#endif
}

static xrt_graphics_sync_handle_t
dummy_sync_handle(void)
{
#if defined(XRT_GRAPHICS_SYNC_HANDLE_IS_WIN32_HANDLE)
	return CreateEventW(NULL, FALSE, FALSE, NULL);
#elif defined(XRT_GRAPHICS_SYNC_HANDLE_IS_FD)
	return open("/dev/null", O_RDONLY);
#else
	return XRT_GRAPHICS_SYNC_HANDLE_INVALID;
#endif
}

xrt_result_t
scm_link_publish_ring_n(struct scm_fixture *f,
                        uint32_t slot,
                        const struct xrt_share_link_ring_desc *desc,
                        uint32_t texture_count)
{
	xrt_graphics_buffer_handle_t tex[XRT_SHARE_LINK_RING_SLOTS + 1];
	uint32_t n = texture_count > XRT_SHARE_LINK_RING_SLOTS + 1 ? XRT_SHARE_LINK_RING_SLOTS + 1 : texture_count;
	for (uint32_t i = 0; i < n; i++) {
		tex[i] = dummy_buffer_handle();
	}
	// The handler owns the received handles (as the transport hands them over).
	return ipc_handle_service_link_share_publish_ring(&f->server.threads[slot].ics, desc, tex, n);
}

xrt_result_t
scm_link_publish_ring(struct scm_fixture *f, uint32_t slot, const struct xrt_share_link_ring_desc *desc)
{
	return scm_link_publish_ring_n(f, slot, desc, desc->slot_count);
}

xrt_result_t
scm_link_publish_fence(struct scm_fixture *f, uint32_t slot, uint32_t generation)
{
	xrt_graphics_sync_handle_t h = dummy_sync_handle();
	return ipc_handle_service_link_share_publish_fence(&f->server.threads[slot].ics, generation, &h, 1);
}

xrt_result_t
scm_link_frame_ready(struct scm_fixture *f,
                     uint32_t slot,
                     const struct xrt_share_link_frame *frame,
                     struct xrt_share_link_poll *out)
{
	return ipc_handle_service_link_share_frame_ready(&f->server.threads[slot].ics, frame, out);
}

void
scm_link_disconnect(struct scm_fixture *f, uint32_t slot)
{
	volatile struct ipc_client_state *o = &f->server.threads[slot].ics;
	ipc_server_client_stereo_camera_link_release(o);
	ipc_server_client_stereo_camera_release(o);
	o->server_thread_index = -1;
}

struct fake_import
{
	struct u_app_share_fake *gen;
	uint32_t eye_w, eye_h;
};

static int g_link_imports = 0;
static bool g_link_import_fails = false;
static uint32_t g_link_last_tex_count = 0;
static bool g_link_last_had_fence = false;
static int g_link_live = 0;
static uint32_t g_link_last_gen = 0;
static uint32_t g_link_last_slot_w = 0;
static uint8_t g_link_luid[XRT_LUID_SIZE];
static bool g_link_luid_known = false;

static bool
link_adapter_luid(void *ctx, uint8_t out[XRT_LUID_SIZE])
{
	(void)ctx;
	if (!g_link_luid_known) {
		return false;
	}
	memcpy(out, g_link_luid, XRT_LUID_SIZE);
	return true;
}

static xrt_result_t
link_import(void *ctx,
            const struct xrt_share_link_ring_desc *desc,
            const xrt_graphics_buffer_handle_t *textures,
            uint32_t texture_count,
            xrt_graphics_sync_handle_t fence,
            uint32_t eye_w,
            uint32_t eye_h,
            void **out_import)
{
	(void)ctx;
	// The contract: import owns every handle, on success and on failure.
	g_link_last_tex_count = texture_count;
	g_link_last_had_fence = xrt_graphics_sync_handle_is_valid(fence);
	for (uint32_t i = 0; i < texture_count; i++) {
		xrt_graphics_buffer_handle_t h = textures[i];
		u_graphics_buffer_unref(&h);
	}
	u_graphics_sync_unref(&fence);
	if (g_link_import_fails) {
		return XRT_ERROR_ALLOCATION;
	}
	struct fake_import *imp = (struct fake_import *)calloc(1, sizeof(*imp));
	if (imp == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	imp->gen = u_app_share_fake_create(eye_w, eye_h, 1000.0f);
	imp->eye_w = eye_w;
	imp->eye_h = eye_h;
	if (imp->gen == NULL) {
		free(imp);
		return XRT_ERROR_ALLOCATION;
	}
	g_link_imports++;
	g_link_live++;
	g_link_last_gen = desc->generation;
	g_link_last_slot_w = desc->slot_width;
	*out_import = imp;
	return XRT_SUCCESS;
}

static uint32_t
link_convert(void *ctx,
             void *import,
             const struct xrt_share_link_frame *frame,
             struct xrt_plugin_stereo_camera_frame *out,
             struct xrt_app_share_frame_info *info)
{
	(void)ctx;
	struct fake_import *imp = (struct fake_import *)import;
	struct u_app_share_fake_frame ff;
	if (!u_app_share_fake_next(imp->gen, 50ll * 1000 * 1000, &ff)) {
		return XRT_PLUGIN_STEREO_CAMERA_WAIT_TIMEOUT;
	}
	out->sequence = ff.sequence;
	out->time_ns = frame->commit_ns;
	out->width = ff.width;
	out->height = ff.height;
	out->format = XRT_PLUGIN_STEREO_CAMERA_FORMAT_NV12;
	out->planes[0] = ff.planes[0];
	out->planes[1] = ff.planes[1];
	out->pitches[0] = ff.pitches[0];
	out->pitches[1] = ff.pitches[1];
	info->content_x = 0;
	info->content_y = 0;
	info->content_width = imp->eye_w;
	info->content_height = imp->eye_h;
	info->mono = frame->mono;
	info->content_generation = frame->content_generation;
	info->zero_disparity_shift_frac = frame->zero_disparity_shift_frac;
	info->commit_time_ns = frame->commit_ns;
	return XRT_PLUGIN_STEREO_CAMERA_WAIT_OK;
}

static void
link_release_frame(void *ctx, void *import)
{
	(void)ctx;
	(void)import;
	// Never called with the manager lock held (this takes it).
	(void)ipc_server_stereo_camera_is_session_locked();
}

static void
link_unimport(void *ctx, void *import)
{
	(void)ctx;
	struct fake_import *imp = (struct fake_import *)import;
	u_app_share_fake_destroy(&imp->gen);
	free(imp);
	g_link_live--;
}

void
scm_set_link_import_fails(bool fails)
{
	g_link_import_fails = fails;
}

uint32_t
scm_link_last_import_texture_count(void)
{
	return g_link_last_tex_count;
}

bool
scm_link_last_import_had_fence(void)
{
	return g_link_last_had_fence;
}

void
scm_set_link_partial_hooks(void)
{
	// An import without an unimport: the manager must refuse the ring hooks whole.
	struct ipc_server_stereo_camera_platform p;
	memset(&p, 0, sizeof(p));
	p.share_link_import = link_import;
	p.share_link_convert = link_convert;
	ipc_server_stereo_camera_set_platform(&p);
}

void
scm_link_register_as(struct scm_fixture *f,
                     uint32_t slot,
                     uint32_t client_id,
                     const struct xrt_share_link_registration *reg,
                     xrt_result_t *out)
{
	f->server.threads[slot].ics.client_state.id = client_id;
	*out = ipc_handle_service_link_register(&f->server.threads[slot].ics, reg);
}

void
scm_set_link_importer(bool enabled, const uint8_t *service_luid)
{
	if (!enabled) {
		ipc_server_stereo_camera_set_platform(NULL);
		return;
	}
	g_link_imports = 0;
	g_link_live = 0;
	g_link_import_fails = false;
	g_link_last_tex_count = 0;
	g_link_last_had_fence = false;
	g_link_last_gen = 0;
	g_link_last_slot_w = 0;
	g_link_luid_known = service_luid != NULL;
	if (service_luid != NULL) {
		memcpy(g_link_luid, service_luid, XRT_LUID_SIZE);
	}
	struct ipc_server_stereo_camera_platform p;
	memset(&p, 0, sizeof(p));
	p.share_link_adapter_luid = link_adapter_luid;
	p.share_link_import = link_import;
	p.share_link_convert = link_convert;
	p.share_link_release_frame = link_release_frame;
	p.share_link_unimport = link_unimport;
	ipc_server_stereo_camera_set_platform(&p);
}

int
scm_link_imports(void)
{
	return g_link_imports;
}

int
scm_link_live_imports(void)
{
	return g_link_live;
}

uint32_t
scm_link_last_import_generation(void)
{
	return g_link_last_gen;
}

uint32_t
scm_link_last_import_slot_width(void)
{
	return g_link_last_slot_w;
}

uint32_t
scm_stop_all(void)
{
	return ipc_server_stereo_camera_stop_all("test");
}

bool
scm_status(uint32_t *out_share_streams, char *out_shares, size_t cap)
{
	struct ipc_server_stereo_camera_status st;
	if (!ipc_server_stereo_camera_get_status(&st)) {
		return false;
	}
	*out_share_streams = st.share_streams;
	snprintf(out_shares, cap, "%s", st.shares);
	return true;
}
