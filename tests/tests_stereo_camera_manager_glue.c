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
#include "util/u_camera_consent.h"

#include <stdlib.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

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
#ifdef XRT_OS_WINDOWS
	HANDLE section;
#else
	int section;
#endif
};

struct scm_fixture *
scm_create(const struct u_stereo_uvc_config *cfg)
{
	struct scm_fixture *f = (struct scm_fixture *)calloc(1, sizeof(*f));
	if (f == NULL) {
		return NULL;
	}
	f->cfg = *cfg;
	struct ipc_server_stereo_camera_options o;
	memset(&o, 0, sizeof(o));
	o.consent_store = &k_store;
	o.consent_env = &k_env;
	o.uvc_config = &f->cfg;
	f->mgr = ipc_server_stereo_camera_create_ex(NULL, &o);
	f->server.stereo_camera = f->mgr;
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
	xrt_result_t xret = ipc_handle_stereo_camera_acquire(&f->ics, id, out_ready, out_frame);
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
