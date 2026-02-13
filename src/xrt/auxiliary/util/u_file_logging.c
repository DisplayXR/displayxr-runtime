// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  File logging implementation for SRMonado.
 * @author David Fattal
 * @ingroup aux_util
 */

#include "u_file_logging.h"
#include "u_logging.h"

#include "xrt/xrt_config_os.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <direct.h>

// Log file handle
static FILE *g_log_file = NULL;
static int g_initialized = 0;

// Forward declaration
static void
file_logging_sink(const char *file,
                  int line,
                  const char *func,
                  enum u_logging_level level,
                  const char *format,
                  va_list args,
                  void *data);

static const char *
level_to_string(enum u_logging_level level)
{
	switch (level) {
	case U_LOGGING_TRACE: return "TRACE";
	case U_LOGGING_DEBUG: return "DEBUG";
	case U_LOGGING_INFO: return "INFO ";
	case U_LOGGING_WARN: return "WARN ";
	case U_LOGGING_ERROR: return "ERROR";
	case U_LOGGING_RAW: return "RAW  ";
	default: return "?????";
	}
}

static void
file_logging_sink(const char *file,
                  int line,
                  const char *func,
                  enum u_logging_level level,
                  const char *format,
                  va_list args,
                  void *data)
{
	if (g_log_file == NULL) {
		return;
	}

	// Get current time with milliseconds
	SYSTEMTIME st;
	GetLocalTime(&st);

	// Write timestamp, level, and function
	fprintf(g_log_file, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [%s] [%s] ",
	        st.wYear, st.wMonth, st.wDay,
	        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
	        level_to_string(level),
	        func ? func : "?");

	// Write the message
	vfprintf(g_log_file, format, args);
	fprintf(g_log_file, "\n");

	// Flush to ensure logs are written immediately (important for crash debugging)
	fflush(g_log_file);
}

static int
create_directory_recursive(const char *path)
{
	char tmp[MAX_PATH];
	char *p = NULL;
	size_t len;

	snprintf(tmp, sizeof(tmp), "%s", path);
	len = strlen(tmp);
	if (tmp[len - 1] == '\\' || tmp[len - 1] == '/') {
		tmp[len - 1] = 0;
	}

	for (p = tmp + 1; *p; p++) {
		if (*p == '\\' || *p == '/') {
			*p = 0;
			_mkdir(tmp);
			*p = '\\';
		}
	}
	return _mkdir(tmp);
}

void
u_file_logging_init(void)
{
	if (g_initialized) {
		return;
	}
	g_initialized = 1;

	// Get %LOCALAPPDATA%
	char local_app_data[MAX_PATH];
	if (FAILED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, local_app_data))) {
		return;
	}

	// Build log directory path: %LOCALAPPDATA%\LeiaSR\SRMonado
	char log_dir[MAX_PATH];
	snprintf(log_dir, sizeof(log_dir), "%s\\LeiaSR\\SRMonado", local_app_data);

	// Create directory if it doesn't exist
	create_directory_recursive(log_dir);

	// Get current timestamp for filename
	SYSTEMTIME st;
	GetLocalTime(&st);

	// Get process executable name and PID for log identification
	char exe_path[MAX_PATH];
	GetModuleFileNameA(NULL, exe_path, MAX_PATH);
	char *exe_name = strrchr(exe_path, '\\');
	exe_name = exe_name ? exe_name + 1 : exe_path;
	DWORD pid = GetCurrentProcessId();

	// Build log file path with process name, PID, and timestamp
	char log_path[MAX_PATH];
	snprintf(log_path, sizeof(log_path), "%s\\SRMonado_%s.%u_%04d-%02d-%02d_%02d-%02d-%02d.log",
	         log_dir, exe_name, (unsigned)pid,
	         st.wYear, st.wMonth, st.wDay,
	         st.wHour, st.wMinute, st.wSecond);

	// Open log file
	g_log_file = fopen(log_path, "a");
	if (g_log_file == NULL) {
		return;
	}

	// Register our sink
	u_log_set_sink(file_logging_sink, NULL);

	// Write header
	fprintf(g_log_file, "=== SRMonado Log Started ===\n");
	fprintf(g_log_file, "Log file: %s\n", log_path);
	fprintf(g_log_file, "Process: %s (PID %u)\n", exe_name, (unsigned)pid);
	fprintf(g_log_file, "Timestamp: %04d-%02d-%02d %02d:%02d:%02d\n",
	        st.wYear, st.wMonth, st.wDay,
	        st.wHour, st.wMinute, st.wSecond);
	fprintf(g_log_file, "============================\n\n");
	fflush(g_log_file);

	// Register cleanup at exit
	atexit(u_file_logging_shutdown);
}

void
u_file_logging_write_raw(const char *msg)
{
	u_file_logging_init();
	if (g_log_file == NULL || msg == NULL) {
		return;
	}

	SYSTEMTIME st;
	GetLocalTime(&st);

	fprintf(g_log_file, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] [OXR  ] %s",
	        st.wYear, st.wMonth, st.wDay,
	        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
	        msg);
	fflush(g_log_file);
}

void
u_file_logging_shutdown(void)
{
	if (g_log_file != NULL) {
		fprintf(g_log_file, "\n=== SRMonado Log Ended ===\n");
		fflush(g_log_file);
		fclose(g_log_file);
		g_log_file = NULL;
	}
	g_initialized = 0;
}

#else /* !XRT_OS_WINDOWS */

// Stub implementations for non-Windows platforms
void
u_file_logging_init(void)
{
	// File logging only implemented on Windows
}

void
u_file_logging_write_raw(const char *msg)
{
	(void)msg;
	// File logging only implemented on Windows
}

void
u_file_logging_shutdown(void)
{
	// File logging only implemented on Windows
}

#endif /* XRT_OS_WINDOWS */
