// Copyright 2026, DisplayXR / Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Audit log implementation (Phase B §5).
 * @ingroup aux_util
 */

#include "xrt/xrt_config_os.h"

#include "u_mcp_audit.h"
#include "util/u_logging.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef XRT_OS_WINDOWS
#include <direct.h>
#include <shlobj.h>
#include <windows.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/types.h>
#include <unistd.h>
#define MKDIR(p) mkdir((p), 0700)
#endif

#define LOG_PFX "[mcp-audit] "
#define MAX_FILE_BYTES (1024 * 1024)

static struct
{
	pthread_mutex_t mutex;
	bool initialized;
	char path[1024];
} g_state = {.mutex = PTHREAD_MUTEX_INITIALIZER};


static bool
ensure_path_locked(void)
{
	if (g_state.initialized) {
		return g_state.path[0] != '\0';
	}
	g_state.initialized = true;

#ifdef XRT_OS_WINDOWS
	char appdata[MAX_PATH];
	if (!SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata))) {
		g_state.path[0] = '\0';
		return false;
	}
	char dir[MAX_PATH];
	snprintf(dir, sizeof(dir), "%s\\DisplayXR", appdata);
	MKDIR(dir);
	snprintf(g_state.path, sizeof(g_state.path), "%s\\mcp-audit.log", dir);
	return true;
#else
	const char *home = getenv("HOME");
	if (home == NULL || home[0] == '\0') {
		g_state.path[0] = '\0';
		return false;
	}
	char dir[512];
	snprintf(dir, sizeof(dir), "%s/.config", home);
	MKDIR(dir);
	snprintf(dir, sizeof(dir), "%s/.config/displayxr", home);
	MKDIR(dir);
	snprintf(g_state.path, sizeof(g_state.path), "%s/mcp-audit.log", dir);
	return true;
#endif
}

static void
rotate_if_needed_locked(void)
{
	struct stat st;
	if (stat(g_state.path, &st) != 0) {
		return;
	}
	if (st.st_size < (off_t)MAX_FILE_BYTES) {
		return;
	}
	char backup[1024];
	snprintf(backup, sizeof(backup), "%s.1", g_state.path);
	// Best-effort rotate; if rename fails the caller just keeps appending.
	(void)remove(backup);
	(void)rename(g_state.path, backup);
}

static uint64_t
now_ns(void)
{
#ifdef XRT_OS_WINDOWS
	// QueryPerformanceCounter is monotonic on modern Windows.
	LARGE_INTEGER freq, ctr;
	if (QueryPerformanceFrequency(&freq) && QueryPerformanceCounter(&ctr)) {
		return (uint64_t)((double)ctr.QuadPart * 1e9 / (double)freq.QuadPart);
	}
	return 0;
#else
	struct timespec ts;
	if (timespec_get(&ts, TIME_UTC) == TIME_UTC) {
		return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
	}
	return 0;
#endif
}

void
u_mcp_audit_append(const char *tool, uint32_t client_id, uint64_t args_hash)
{
	if (tool == NULL) {
		return;
	}
	pthread_mutex_lock(&g_state.mutex);
	if (!ensure_path_locked()) {
		pthread_mutex_unlock(&g_state.mutex);
		return;
	}
	rotate_if_needed_locked();

	FILE *f = fopen(g_state.path, "ab");
	if (f == NULL) {
		U_LOG_W(LOG_PFX "fopen(%s) failed", g_state.path);
		pthread_mutex_unlock(&g_state.mutex);
		return;
	}
	fprintf(f, "%llu,%s,%u,0x%016llx\n",
	        (unsigned long long)now_ns(), tool,
	        (unsigned)client_id, (unsigned long long)args_hash);
	fclose(f);
	pthread_mutex_unlock(&g_state.mutex);
}

const char *
u_mcp_audit_path(void)
{
	pthread_mutex_lock(&g_state.mutex);
	(void)ensure_path_locked();
	const char *p = g_state.path;
	pthread_mutex_unlock(&g_state.mutex);
	return p;
}
