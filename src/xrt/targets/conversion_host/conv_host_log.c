// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-conversion-host logging: file sink + WARN+ forward queue.
 * @ingroup conv_host
 */

#include "conv_host_log.h"

#include "xrt/xrt_config_os.h"
#include "os/os_threading.h"
#include "util/u_file_logging.h"
#include "util/u_logging.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef XRT_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#else
#include <dlfcn.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

struct fwd_line
{
	uint32_t level;
	char text[CONV_HOST_LOG_LINE_MAX];
};

static struct
{
	bool inited;
	FILE *file;
	char path[1024];
	struct os_mutex mtx; //!< guards the forward queue only (stdio locks the FILE per call)
	struct fwd_line q[CONV_HOST_LOG_FORWARD_MAX];
	uint32_t head, count, dropped;
} g;

static const char *
level_str(enum u_logging_level level)
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
now_local(struct tm *out_tm, long *out_ms)
{
	struct timespec ts;
	if (timespec_get(&ts, TIME_UTC) == 0) {
		ts.tv_sec = time(NULL);
		ts.tv_nsec = 0;
	}
	time_t t = ts.tv_sec;
#ifdef XRT_OS_WINDOWS
	localtime_s(out_tm, &t);
#else
	localtime_r(&t, out_tm);
#endif
	*out_ms = (long)(ts.tv_nsec / 1000000);
}

static void
enqueue(uint32_t level, const char *body)
{
	os_mutex_lock(&g.mtx);
	if (g.count == CONV_HOST_LOG_FORWARD_MAX) {
		g.head = (g.head + 1) % CONV_HOST_LOG_FORWARD_MAX; // drop the oldest
		g.count--;
		g.dropped++;
	}
	struct fwd_line *l = &g.q[(g.head + g.count) % CONV_HOST_LOG_FORWARD_MAX];
	l->level = level;
	(void)snprintf(l->text, sizeof(l->text), "%s", body);
	g.count++;
	os_mutex_unlock(&g.mtx);
}

#ifdef XRT_OS_WINDOWS
static void
file_write(const char *func, enum u_logging_level level, const char *fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	u_file_logging_write_va(func, level, fmt, a);
	va_end(a);
}
#endif

static void
sink(const char *file, int line, const char *func, enum u_logging_level level, const char *format, va_list args,
     void *data)
{
	(void)file;
	(void)line;
	(void)data;

	char body[2048];
	int m = vsnprintf(body, sizeof(body), format, args);
	if (m < 0) {
		return;
	}
	// Strip one trailing newline (some callers add their own).
	size_t bl = strlen(body);
	if (bl > 0 && body[bl - 1] == '\n') {
		body[--bl] = '\0';
	}

#ifdef XRT_OS_WINDOWS
	// The shared file logger (u_file_logging) owns the file on Windows.
	file_write(func != NULL ? func : "?", level, "%s", body);
#endif
	if (g.file != NULL) {
		struct tm tm;
		long ms = 0;
		now_local(&tm, &ms);
		char buf[2304];
		int n = snprintf(buf, sizeof(buf), "[%04d-%02d-%02d %02d:%02d:%02d.%03ld] [%s] [%s] %s\n",
		                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ms,
		                 level_str(level), func != NULL ? func : "?", body);
		if (n > 0) {
			if ((size_t)n >= sizeof(buf)) {
				n = (int)sizeof(buf) - 1;
				buf[n - 1] = '\n';
			}
			// One stdio call per line: the CRT locks the FILE per call.
			fwrite(buf, 1, (size_t)n, g.file);
			fflush(g.file);
		}
	}

	if (level >= U_LOGGING_WARN && level != U_LOGGING_RAW) {
		enqueue((uint32_t)level, body);
	}
}

#ifndef XRT_OS_WINDOWS
static bool
make_dirs(const char *path)
{
	char tmp[1024];
	(void)snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p != '\0'; p++) {
		if (*p == '/' || *p == '\\') {
			const char c = *p;
			*p = '\0';
#ifdef XRT_OS_WINDOWS
			(void)CreateDirectoryA(tmp, NULL);
#else
			(void)mkdir(tmp, 0700);
#endif
			*p = c;
		}
	}
#ifdef XRT_OS_WINDOWS
	return CreateDirectoryA(tmp, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
#else
	return mkdir(tmp, 0700) == 0 || errno == EEXIST;
#endif
}

//! Candidate log directories, best first. Returns how many were written.
static int
log_dirs(char dirs[3][1024])
{
	int n = 0;
#ifdef XRT_OS_WINDOWS
	char base[MAX_PATH];
	if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base))) {
		(void)snprintf(dirs[n++], 1024, "%s\\DisplayXR", base);
	}
	DWORD tl = GetTempPathA(MAX_PATH, base);
	if (tl > 0 && tl < MAX_PATH) {
		// GetTempPath ends with a backslash.
		if (base[tl - 1] == '\\') {
			base[tl - 1] = '\0';
		}
		(void)snprintf(dirs[n++], 1024, "%s", base);
	}
#else
	const char *home = getenv("HOME");
#ifdef XRT_OS_MACOS
	if (home != NULL && home[0] != '\0') {
		(void)snprintf(dirs[n++], 1024, "%s/Library/Logs/DisplayXR", home);
	}
#else
	const char *xdg = getenv("XDG_STATE_HOME");
	if (xdg != NULL && xdg[0] == '/') {
		(void)snprintf(dirs[n++], 1024, "%s/displayxr", xdg);
	} else if (home != NULL && home[0] != '\0') {
		(void)snprintf(dirs[n++], 1024, "%s/.local/state/displayxr", home);
	}
#endif
	(void)snprintf(dirs[n++], 1024, "/tmp");
#endif
	return n;
}

#endif // !XRT_OS_WINDOWS

static unsigned long
self_pid(void)
{
#ifdef XRT_OS_WINDOWS
	return (unsigned long)GetCurrentProcessId();
#else
	return (unsigned long)getpid();
#endif
}

bool
conv_host_log_init(const char *provider_id)
{
	if (g.inited) {
		return g.path[0] != 0;
	}
	g.inited = true;
	(void)os_mutex_init(&g.mtx);

	struct tm tm;
	long ms = 0;
	now_local(&tm, &ms);

	// The provider id goes into a file name: keep it to a safe alphabet.
	char safe_id[64];
	size_t k = 0;
	for (const char *c = provider_id != NULL ? provider_id : "unknown"; *c != '\0' && k < sizeof(safe_id) - 1;
	     c++) {
		const char ch = *c;
		const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
		                ch == '-' || ch == '_' || ch == '.';
		safe_id[k++] = ok ? ch : '_';
	}
	safe_id[k] = '\0';

#ifdef XRT_OS_WINDOWS
	// u_log auto-starts the shared file logger on its first message and
	// installs its own sink; start it NOW, under our name, then take the sink.
	char name[96];
	(void)snprintf(name, sizeof(name), "conversion-host_%s", safe_id);
	u_file_logging_set_name(name);
	u_file_logging_init();
	(void)snprintf(g.path, sizeof(g.path), "%s", u_file_logging_get_path());
	u_log_set_sink(sink, NULL);
	return g.path[0] != '\0';
#else
	char dirs[3][1024];
	const int nd = log_dirs(dirs);
	for (int i = 0; i < nd && g.file == NULL; i++) {
		if (!make_dirs(dirs[i])) {
			continue;
		}
		(void)snprintf(g.path, sizeof(g.path), "%s%cDisplayXR_conversion-host_%s.%lu_%04d-%02d-%02d_%02d-%02d-%02d.log",
		               dirs[i],
#ifdef XRT_OS_WINDOWS
		               '\\',
#else
		               '/',
#endif
		               safe_id, self_pid(), tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
		               tm.tm_sec);
		g.file = fopen(g.path, "a");
	}
	if (g.file == NULL) {
		g.path[0] = '\0';
	} else {
		fprintf(g.file, "=== DisplayXR conversion host (%s) pid %lu ===\n", safe_id, self_pid());
		fflush(g.file);
	}

	u_log_set_sink(sink, NULL);
	atexit(conv_host_log_shutdown);
	return g.file != NULL;
#endif
}

const char *
conv_host_log_path(void)
{
	return g.path;
}

typedef void (*set_sink_fn_t)(u_log_sink_func_t func, void *data);

void
conv_host_log_adopt_provider_logger(void *provider_module)
{
	set_sink_fn_t set_sink = NULL;
#ifdef XRT_OS_WINDOWS
	(void)provider_module;
	// The provider is built with XRT_USING_RUNTIME_DLL: it imports u_log*
	// from the runtime DLL, which therefore is loaded now.
	HMODULE rt = GetModuleHandleW(L"DisplayXRClient.dll");
	if (rt != NULL) {
		set_sink = (set_sink_fn_t)(void *)GetProcAddress(rt, "u_log_set_sink");
		// That DLL's u_log also auto-starts ITS file logger on its first
		// message, which would take its sink back: consume that one-shot
		// first (it leaves a header-only DisplayXR_<exe>.<pid> file).
		typedef void (*init_fn_t)(void);
		init_fn_t rt_init = (init_fn_t)(void *)GetProcAddress(rt, "u_file_logging_init");
		if (rt_init != NULL && set_sink != NULL) {
			rt_init();
		}
	}
#else
	if (provider_module != NULL) {
		// Searches the provider image and its dependencies (macOS: the
		// runtime dylib). ELF providers export only their entry point, so
		// this finds nothing there and their lines go to stderr.
		set_sink = (set_sink_fn_t)dlsym(provider_module, "u_log_set_sink");
	}
#endif
	if (set_sink != NULL && set_sink != u_log_set_sink) {
		set_sink(sink, NULL);
		U_LOG_I("conversion host: the provider's runtime logger now writes to this log");
	}
}

uint32_t
conv_host_log_drain(void (*fn)(void *ctx, uint32_t level, const char *line), void *ctx, uint32_t *out_dropped)
{
	if (!g.inited) {
		if (out_dropped != NULL) {
			*out_dropped = 0;
		}
		return 0;
	}
	uint32_t delivered = 0;
	struct fwd_line l;
	for (;;) {
		os_mutex_lock(&g.mtx);
		if (g.count == 0) {
			if (out_dropped != NULL) {
				*out_dropped = g.dropped;
			}
			g.dropped = 0;
			os_mutex_unlock(&g.mtx);
			break;
		}
		l = g.q[g.head];
		g.head = (g.head + 1) % CONV_HOST_LOG_FORWARD_MAX;
		g.count--;
		os_mutex_unlock(&g.mtx);
		if (fn != NULL) {
			fn(ctx, l.level, l.text);
		}
		delivered++;
	}
	return delivered;
}

void
conv_host_log_shutdown(void)
{
	// Footer + flush, but never fclose: an abandoned provider thread may
	// still log while the process exits, and the OS closes the file anyway.
	static bool done = false;
	if (g.file != NULL && !done) {
		done = true;
		fprintf(g.file, "=== conversion host log ended ===\n");
		fflush(g.file);
	}
}
