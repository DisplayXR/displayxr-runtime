// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Out-of-process viewer-tracking hosting: page name, supervisor,
 *         the service's host manager (see target_viewer_host.h).
 * @ingroup target_common
 */

#include "target_viewer_host.h"

#include "xrt/xrt_config_os.h"

#include "util/u_logging.h"
#include "util/u_viewer_pose_page.h"

#include "os/os_threading.h"
#include "os/os_time.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(XRT_OS_ANDROID)
#define VHOST_HAVE_PROCESS 1
#include "os/os_process.h"
#endif

#if defined(XRT_OS_LINUX) || defined(XRT_OS_MACOS)
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#endif


const char *
target_vhost_exit_str(int32_t code)
{
	switch (code) {
	case TARGET_VHOST_EXIT_ORDERLY: return "orderly";
	case TARGET_VHOST_EXIT_USAGE: return "usage";
	case TARGET_VHOST_EXIT_NO_PROVIDER: return "no_provider";
	case TARGET_VHOST_EXIT_START_FAILED: return "start_failed";
	case TARGET_VHOST_EXIT_PAGE: return "page";
	case TARGET_VHOST_EXIT_ELEVATED: return "elevated";
	case TARGET_VHOST_EXIT_CHANNEL: return "channel";
	default: return "abnormal";
	}
}

bool
target_viewer_page_name(char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return false;
	}
	out[0] = '\0';
	const char *env = getenv("DXR_VIEWER_POSE_PAGE");
	if (env != NULL && env[0] != '\0') {
		if (strcmp(env, "none") == 0 || strcmp(env, "0") == 0) {
			return false;
		}
		const int n = snprintf(out, cap, "%s", env);
		return n > 0 && (size_t)n < cap;
	}
#if defined(XRT_OS_WINDOWS)
	const int n = snprintf(out, cap, "Local\\DisplayXR-ViewerPose-v1");
#elif defined(XRT_OS_ANDROID)
	const int n = -1; // no named shm on Android
#else
	const int n = snprintf(out, cap, "/dxr-vpose-%u", (unsigned)getuid());
#endif
	return n > 0 && (size_t)n < cap;
}


/*
 *
 * Supervisor (pure).
 *
 */

#define MS ((uint64_t)1000 * 1000)
#define SEC ((uint64_t)1000 * MS)

void
target_vhost_sup_config_defaults(struct target_vhost_sup_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->backoff_initial_ns = 500 * MS;
	cfg->backoff_max_ns = 30 * SEC;
	cfg->healthy_reset_ns = 60 * SEC;
	cfg->crash_loop_count = 5;
	cfg->crash_loop_window_ns = 120 * SEC;
	cfg->failed_retry_ns = 600 * SEC;
	cfg->hello_timeout_ns = 10 * SEC;
}

void
target_vhost_sup_init(struct target_vhost_sup *s, const struct target_vhost_sup_config *cfg)
{
	memset(s, 0, sizeof(*s));
	if (cfg != NULL) {
		s->cfg = *cfg;
	} else {
		target_vhost_sup_config_defaults(&s->cfg);
	}
	if (s->cfg.crash_loop_count == 0 || s->cfg.crash_loop_count > TARGET_VHOST_SUP_HISTORY) {
		s->cfg.crash_loop_count = TARGET_VHOST_SUP_HISTORY;
	}
	s->state = TARGET_VHOST_SUP_IDLE;
	s->backoff_ns = s->cfg.backoff_initial_ns;
	s->last_exit = INT32_MIN;
}

void
target_vhost_sup_set_demand(struct target_vhost_sup *s, bool demand)
{
	if (demand) {
		s->demand = true; // latches: the provider then runs until teardown
	}
}

void
target_vhost_sup_spawned(struct target_vhost_sup *s, uint64_t now_ns, bool ok)
{
	if (s->state == TARGET_VHOST_SUP_STOPPED) {
		return;
	}
	if (ok) {
		s->state = TARGET_VHOST_SUP_STARTING;
		s->spawned_ns = now_ns;
		return;
	}
	(void)target_vhost_sup_exited(s, now_ns, -1, false);
}

void
target_vhost_sup_hello(struct target_vhost_sup *s, uint64_t now_ns)
{
	if (s->state == TARGET_VHOST_SUP_STARTING) {
		s->state = TARGET_VHOST_SUP_RUNNING;
		s->running_since_ns = now_ns;
	}
}

static bool
crash_loop(struct target_vhost_sup *s, uint64_t now_ns)
{
	s->crash_ns[s->crash_head % TARGET_VHOST_SUP_HISTORY] = now_ns;
	s->crash_head++;
	if (s->crash_head < s->cfg.crash_loop_count) {
		return false;
	}
	// The crash_loop_count'th most recent crash, inside the window?
	const uint32_t idx = (s->crash_head - s->cfg.crash_loop_count) % TARGET_VHOST_SUP_HISTORY;
	return now_ns - s->crash_ns[idx] <= s->cfg.crash_loop_window_ns;
}

bool
target_vhost_sup_exited(struct target_vhost_sup *s, uint64_t now_ns, int32_t code, bool requested)
{
	s->last_exit = code;
	if (s->state == TARGET_VHOST_SUP_STOPPED) {
		return false;
	}
	if (requested && code == TARGET_VHOST_EXIT_ORDERLY) {
		s->state = TARGET_VHOST_SUP_IDLE;
		return false;
	}
	if (code == TARGET_VHOST_EXIT_NO_PROVIDER || code == TARGET_VHOST_EXIT_USAGE ||
	    code == TARGET_VHOST_EXIT_ELEVATED) {
		// Terminal: a respawn would say the same thing.
		s->state = TARGET_VHOST_SUP_FAILED;
		s->failed_ns = now_ns;
		return false;
	}
	// Abnormal: a crash, a watchdog kill, a failed spawn, an unrequested exit.
	if (s->state == TARGET_VHOST_SUP_RUNNING && now_ns - s->running_since_ns >= s->cfg.healthy_reset_ns) {
		s->backoff_ns = s->cfg.backoff_initial_ns; // it was healthy for a while
	}
	s->restarts++;
	if (crash_loop(s, now_ns)) {
		s->state = TARGET_VHOST_SUP_FAILED;
		s->failed_ns = now_ns;
		return true;
	}
	s->state = TARGET_VHOST_SUP_BACKOFF;
	s->next_spawn_ns = now_ns + s->backoff_ns;
	s->backoff_ns = s->backoff_ns * 2u > s->cfg.backoff_max_ns ? s->cfg.backoff_max_ns : s->backoff_ns * 2u;
	return true;
}

void
target_vhost_sup_stop(struct target_vhost_sup *s)
{
	s->state = TARGET_VHOST_SUP_STOPPED;
}

enum target_vhost_sup_action
target_vhost_sup_tick(struct target_vhost_sup *s, uint64_t now_ns)
{
	switch (s->state) {
	case TARGET_VHOST_SUP_IDLE:
		if (s->demand) {
			s->spawns++;
			return TARGET_VHOST_SUP_ACTION_SPAWN;
		}
		return TARGET_VHOST_SUP_ACTION_NONE;
	case TARGET_VHOST_SUP_STARTING:
		if (now_ns - s->spawned_ns > s->cfg.hello_timeout_ns) {
			return TARGET_VHOST_SUP_ACTION_KILL;
		}
		return TARGET_VHOST_SUP_ACTION_NONE;
	case TARGET_VHOST_SUP_BACKOFF:
		if (s->demand && now_ns >= s->next_spawn_ns) {
			s->spawns++;
			return TARGET_VHOST_SUP_ACTION_SPAWN;
		}
		return TARGET_VHOST_SUP_ACTION_NONE;
	case TARGET_VHOST_SUP_FAILED:
		if (s->demand && now_ns - s->failed_ns >= s->cfg.failed_retry_ns) {
			s->crash_head = 0;
			s->backoff_ns = s->cfg.backoff_initial_ns;
			s->spawns++;
			return TARGET_VHOST_SUP_ACTION_SPAWN;
		}
		return TARGET_VHOST_SUP_ACTION_NONE;
	case TARGET_VHOST_SUP_RUNNING:
	case TARGET_VHOST_SUP_STOPPED:
	default: return TARGET_VHOST_SUP_ACTION_NONE;
	}
}

uint32_t
target_vhost_sup_page_state(enum target_vhost_sup_state st)
{
	switch (st) {
	case TARGET_VHOST_SUP_IDLE: return U_VPP_STATE_IDLE;
	case TARGET_VHOST_SUP_STARTING: return U_VPP_STATE_STARTING;
	case TARGET_VHOST_SUP_RUNNING: return U_VPP_STATE_RUNNING;
	case TARGET_VHOST_SUP_BACKOFF: return U_VPP_STATE_BACKOFF;
	case TARGET_VHOST_SUP_FAILED: return U_VPP_STATE_FAILED;
	case TARGET_VHOST_SUP_STOPPED:
	default: return U_VPP_STATE_STOPPED;
	}
}


/*
 *
 * Manager.
 *
 */

#ifdef VHOST_HAVE_PROCESS

//! How long the host must have been silent (no heartbeat) before the manager kills it.
#define VHOST_HEARTBEAT_KILL_NS ((int64_t)3000 * 1000 * 1000)
//! Supervisor wake period.
#define VHOST_TICK_NS ((int64_t)100 * 1000 * 1000)
//! Orderly-stop grace before terminate.
#define VHOST_STOP_GRACE_MS 3000u
//! Child channel connect timeout (Windows named pipe).
#define VHOST_ACCEPT_MS 5000u

//! The one control byte: stop.
#define VHOST_CMD_STOP 'q'

/*!
 * Where the host's stdout + stderr go (POSIX; on Windows the host writes its
 * own `%LOCALAPPDATA%\DisplayXR\DisplayXR_tracking-host.<pid>_<ts>.log`):
 * macOS `~/Library/Logs/DisplayXR/DisplayXR_tracking-host.log`, Linux
 * `$XDG_STATE_HOME/displayxr/` (default `~/.local/state/displayxr/`), else
 * `/tmp`. Appended across restarts. NULL = discard.
 */
static const char *
host_log_path(void)
{
#if defined(XRT_OS_LINUX) || defined(XRT_OS_MACOS)
	static char path[1024];
	if (path[0] != '\0') {
		return path;
	}
	char dir[900] = {0};
	const char *home = getenv("HOME");
#if defined(XRT_OS_MACOS)
	if (home != NULL && home[0] != '\0') {
		snprintf(dir, sizeof(dir), "%s/Library/Logs/DisplayXR", home);
	}
#else
	const char *xdg = getenv("XDG_STATE_HOME");
	if (xdg != NULL && xdg[0] == '/') {
		snprintf(dir, sizeof(dir), "%s/displayxr", xdg);
	} else if (home != NULL && home[0] != '\0') {
		snprintf(dir, sizeof(dir), "%s/.local/state/displayxr", home);
	}
#endif
	// mkdir -p, owner-only for what we create.
	for (char *p = dir + 1; dir[0] != '\0' && *p != '\0'; p++) {
		if (*p == '/') {
			*p = '\0';
			(void)mkdir(dir, 0700);
			*p = '/';
		}
	}
	if (dir[0] == '\0' || (mkdir(dir, 0700) != 0 && errno != EEXIST)) {
		snprintf(dir, sizeof(dir), "/tmp");
	}
	snprintf(path, sizeof(path), "%s/DisplayXR_tracking-host.log", dir);
	return path;
#else
	return NULL;
#endif
}

struct target_viewer_host
{
	struct os_thread_helper oth;
	char host_exe[1024];
	char page_name[128];
	void *page;
	char provider_id[U_VPP_ID_MAX];
	struct xrt_tracking_host_display_geometry geo;
	bool have_geo;
	int trust_enforce;

	// Under oth's lock.
	struct target_vhost_sup sup;
	bool demand;
	struct os_process *proc;
	struct os_channel *chan;
	uint32_t generation;
	bool stop_requested;
	uint32_t last_written_state; // UINT32_MAX = never
	uint32_t last_written_restarts;
	int64_t last_written_pid;
};

static void
write_owner_locked(struct target_viewer_host *h, bool force)
{
	const uint32_t st = target_vhost_sup_page_state(h->sup.state);
	const int64_t pid = h->proc != NULL ? (int64_t)os_process_pid(h->proc) : 0;
	if (!force && st == h->last_written_state && h->sup.restarts == h->last_written_restarts &&
	    pid == h->last_written_pid) {
		return;
	}
	struct u_vpp_owner_status o;
	memset(&o, 0, sizeof(o));
	o.state = st;
	o.restarts = h->sup.restarts;
	o.generation = h->generation;
	o.host_pid = pid;
	o.in_process = false;
	snprintf(o.provider_id, sizeof(o.provider_id), "%s", h->provider_id);
	u_vpp_write_owner(h->page, &o);
	h->last_written_state = st;
	h->last_written_restarts = h->sup.restarts;
	h->last_written_pid = pid;
}

static void
close_child_locked(struct target_viewer_host *h)
{
	if (h->chan != NULL) {
		os_channel_destroy(&h->chan);
	}
	if (h->proc != NULL) {
		os_process_destroy(&h->proc);
	}
}

static bool
spawn_locked(struct target_viewer_host *h, uint64_t now)
{
	h->generation++;
	char gen[16], geo[160], trust[4];
	snprintf(gen, sizeof(gen), "%u", h->generation);
	snprintf(geo, sizeof(geo), "%.6f,%.6f,%.6f,%.6f,%.6f", (double)h->geo.display_width_m,
	         (double)h->geo.display_height_m, (double)h->geo.nominal_viewer_x_m, (double)h->geo.nominal_viewer_y_m,
	         (double)h->geo.nominal_viewer_z_m);
	char parent[32];
	snprintf(parent, sizeof(parent), "%ld", os_process_current_pid());

	char token[256] = {0};
	uint64_t child_handle = OS_NATIVE_HANDLE_INVALID;
	struct os_channel *chan = NULL;
	if (os_channel_create_for_child("displayxr-tracking-host", &chan, token, sizeof(token), &child_handle) != 0) {
		U_LOG_W("viewer tracking host: could not create the control channel");
		return false;
	}

	const char *args[16];
	uint32_t n = 0;
	args[n++] = "--serve";
	args[n++] = "--provider";
	args[n++] = h->provider_id;
	args[n++] = "--page";
	args[n++] = h->page_name;
	args[n++] = "--generation";
	args[n++] = gen;
	args[n++] = "--parent";
	args[n++] = parent;
	args[n++] = "--channel";
	args[n++] = token;
	if (h->have_geo) {
		args[n++] = "--geometry";
		args[n++] = geo;
	}
	if (h->trust_enforce >= 0) {
		snprintf(trust, sizeof(trust), "%d", h->trust_enforce != 0 ? 1 : 0);
		args[n++] = "--trust-enforce";
		args[n++] = trust;
	}

	struct os_process_spawn_info si;
	memset(&si, 0, sizeof(si));
	si.exe_path = h->host_exe;
	si.args = args;
	si.arg_count = n;
	si.kill_on_parent_exit = true;
	si.channel_child_handle = child_handle;
	si.log_path = host_log_path();
	// macOS: the host, not the service, owns the camera grant. It carries the
	// usage string (embedded Info.plist); a LaunchAgent-started service has none
	// and would get the camera denied without a prompt.
	si.disclaim_responsibility = true;

	struct os_process *proc = NULL;
	if (os_process_spawn(&si, &proc) != 0) {
		os_native_handle_close(child_handle);
		os_channel_destroy(&chan);
		U_LOG_W("viewer tracking host: could not start %s", h->host_exe);
		return false;
	}
	if (os_channel_accept_child(chan, proc, child_handle, VHOST_ACCEPT_MS) != 0) {
		U_LOG_W("viewer tracking host: pid %ld did not connect its control channel; killing it",
		        os_process_pid(proc));
		(void)os_process_terminate(proc, TARGET_VHOST_EXIT_CHANNEL);
		int32_t code = 0;
		(void)os_process_wait(proc, 2000, &code);
		os_process_destroy(&proc);
		os_channel_destroy(&chan);
		return false;
	}
	h->proc = proc;
	h->chan = chan;
	U_LOG_W("viewer tracking host: started pid %ld for provider '%s' (generation %u%s)", os_process_pid(proc),
	        h->provider_id, h->generation, h->sup.restarts > 0 ? ", restart" : "");
	(void)now;
	return true;
}

//! The child exited (or was killed) with @p code: reap, account, log once.
static void
child_exited_locked(struct target_viewer_host *h, uint64_t now, int32_t code, const char *why)
{
	const long pid = h->proc != NULL ? os_process_pid(h->proc) : 0;
	close_child_locked(h);
	const bool requested = h->stop_requested;
	const bool crash = target_vhost_sup_exited(&h->sup, now, code, requested);
	if (h->sup.state == TARGET_VHOST_SUP_STOPPED || (requested && code == TARGET_VHOST_EXIT_ORDERLY)) {
		return;
	}
	if (crash && h->sup.state == TARGET_VHOST_SUP_BACKOFF) {
		U_LOG_W("viewer tracking host: pid %ld exited %d (%s%s%s) — restart %u in %llu ms", pid, (int)code,
		        target_vhost_exit_str(code), why ? ", " : "", why ? why : "", h->sup.restarts,
		        (unsigned long long)((h->sup.next_spawn_ns - now) / MS));
	} else if (h->sup.state == TARGET_VHOST_SUP_FAILED) {
		U_LOG_W("viewer tracking host: pid %ld exited %d (%s%s%s) — %s; not restarted for %llu s", pid,
		        (int)code, target_vhost_exit_str(code), why ? ", " : "", why ? why : "",
		        crash ? "crash loop" : "provider unusable",
		        (unsigned long long)(h->sup.cfg.failed_retry_ns / SEC));
	}
}

static void
kill_child_locked(struct target_viewer_host *h, uint64_t now, const char *why)
{
	if (h->proc == NULL) {
		return;
	}
	(void)os_process_terminate(h->proc, 30);
	int32_t code = 30;
	(void)os_process_wait(h->proc, 2000, &code);
	child_exited_locked(h, now, code, why);
}

static void
tick_locked(struct target_viewer_host *h)
{
	const uint64_t now = (uint64_t)os_monotonic_get_ns();
	const int64_t page_demand = u_vpp_read_demand(h->page);
	const bool reader_demand = page_demand > 0 && (int64_t)now - page_demand < U_VPP_DEMAND_FRESH_NS;
	target_vhost_sup_set_demand(&h->sup, h->demand || reader_demand);

	if (h->proc != NULL) {
		int32_t code = 0;
		const int r = os_process_wait(h->proc, 0, &code);
		if (r == 1) {
			child_exited_locked(h, now, code, NULL);
		} else if (h->sup.state == TARGET_VHOST_SUP_STARTING) {
			// Hello: the producer block names this child and this generation.
			struct u_vpp_producer_status ps;
			if (u_vpp_read_producer(h->page, &ps) && ps.pid == (int64_t)os_process_pid(h->proc) &&
			    ps.generation == h->generation) {
				target_vhost_sup_hello(&h->sup, now);
			}
		} else if (h->sup.state == TARGET_VHOST_SUP_RUNNING) {
			const int64_t hb = u_vpp_read_heartbeat(h->page);
			if ((int64_t)now - hb > VHOST_HEARTBEAT_KILL_NS) {
				U_LOG_W("viewer tracking host: pid %ld silent for %lld ms (no heartbeat) — killing it",
				        os_process_pid(h->proc), (long long)(((int64_t)now - hb) / (int64_t)MS));
				kill_child_locked(h, now, "watchdog");
			}
		}
	}

	switch (target_vhost_sup_tick(&h->sup, now)) {
	case TARGET_VHOST_SUP_ACTION_SPAWN: target_vhost_sup_spawned(&h->sup, now, spawn_locked(h, now)); break;
	case TARGET_VHOST_SUP_ACTION_KILL: kill_child_locked(h, now, "no hello"); break;
	case TARGET_VHOST_SUP_ACTION_NONE:
	default: break;
	}
	write_owner_locked(h, false);
}

static void *
run(void *ptr)
{
	struct target_viewer_host *h = (struct target_viewer_host *)ptr;
	os_thread_helper_name(&h->oth, "dxr-viewer-host");
	os_thread_helper_lock(&h->oth);
	while (os_thread_helper_is_running_locked(&h->oth)) {
		tick_locked(h);
		os_thread_helper_unlock(&h->oth);
		os_nanosleep(VHOST_TICK_NS);
		os_thread_helper_lock(&h->oth);
	}
	os_thread_helper_unlock(&h->oth);
	return NULL;
}

struct target_viewer_host *
target_viewer_host_create(const struct target_viewer_host_config *cfg)
{
	if (cfg == NULL || cfg->host_exe == NULL || cfg->page_name == NULL || cfg->page_mem == NULL ||
	    cfg->provider_id == NULL || cfg->provider_id[0] == '\0') {
		return NULL;
	}
	struct target_viewer_host *h = (struct target_viewer_host *)calloc(1, sizeof(*h));
	if (h == NULL) {
		return NULL;
	}
	snprintf(h->host_exe, sizeof(h->host_exe), "%s", cfg->host_exe);
	snprintf(h->page_name, sizeof(h->page_name), "%s", cfg->page_name);
	snprintf(h->provider_id, sizeof(h->provider_id), "%s", cfg->provider_id);
	h->page = cfg->page_mem;
	h->geo = cfg->geometry;
	h->have_geo = cfg->have_geometry;
	h->trust_enforce = cfg->trust_enforce;
	h->last_written_state = UINT32_MAX;
	target_vhost_sup_init(&h->sup, cfg->sup);
	if (os_thread_helper_init(&h->oth) != 0) {
		free(h);
		return NULL;
	}
	os_thread_helper_lock(&h->oth);
	write_owner_locked(h, true);
	os_thread_helper_unlock(&h->oth);
	if (os_thread_helper_start(&h->oth, run, h) != 0) {
		os_thread_helper_destroy(&h->oth);
		free(h);
		return NULL;
	}
	return h;
}

void
target_viewer_host_demand(struct target_viewer_host *h)
{
	if (h == NULL) {
		return;
	}
	os_thread_helper_lock(&h->oth);
	h->demand = true;
	os_thread_helper_unlock(&h->oth);
}

void
target_viewer_host_get_status(struct target_viewer_host *h, struct target_viewer_host_status *out)
{
	memset(out, 0, sizeof(*out));
	out->last_exit = INT32_MIN;
	if (h == NULL) {
		out->state = TARGET_VHOST_SUP_STOPPED;
		return;
	}
	os_thread_helper_lock(&h->oth);
	out->state = h->sup.state;
	out->pid = h->proc != NULL ? (int64_t)os_process_pid(h->proc) : 0;
	out->restarts = h->sup.restarts;
	out->spawns = h->sup.spawns;
	out->last_exit = h->sup.last_exit;
	os_thread_helper_unlock(&h->oth);
}

void
target_viewer_host_destroy(struct target_viewer_host **h_ptr)
{
	if (h_ptr == NULL || *h_ptr == NULL) {
		return;
	}
	struct target_viewer_host *h = *h_ptr;
	*h_ptr = NULL;
	os_thread_helper_stop_and_wait(&h->oth);

	// The thread is gone: no lock needed from here.
	h->stop_requested = true;
	if (h->proc != NULL) {
		const uint8_t cmd = VHOST_CMD_STOP;
		(void)os_channel_send(h->chan, &cmd, 1, NULL, 0);
		int32_t code = 0;
		if (os_process_wait(h->proc, VHOST_STOP_GRACE_MS, &code) != 1) {
			U_LOG_W("viewer tracking host: pid %ld did not stop in %u ms — terminating it",
			        os_process_pid(h->proc), VHOST_STOP_GRACE_MS);
			(void)os_process_terminate(h->proc, 30);
			(void)os_process_wait(h->proc, 2000, &code);
		}
		close_child_locked(h);
	}
	target_vhost_sup_stop(&h->sup);
	write_owner_locked(h, true);
	os_thread_helper_destroy(&h->oth);
	free(h);
}

bool
target_viewer_host_probe(const char *host_exe,
                         const char *page_name,
                         const void *page_mem,
                         int trust_enforce,
                         uint32_t timeout_ms,
                         char *out_id,
                         size_t id_cap,
                         bool *out_ran)
{
	if (out_ran != NULL) {
		*out_ran = false;
	}
	if (out_id != NULL && id_cap > 0) {
		out_id[0] = '\0';
	}
	if (host_exe == NULL || page_name == NULL || page_mem == NULL) {
		return false;
	}
	char parent[32], trust[4];
	snprintf(parent, sizeof(parent), "%ld", os_process_current_pid());
	const char *args[8];
	uint32_t n = 0;
	args[n++] = "--probe";
	args[n++] = "--page";
	args[n++] = page_name;
	args[n++] = "--parent";
	args[n++] = parent;
	if (trust_enforce >= 0) {
		snprintf(trust, sizeof(trust), "%d", trust_enforce != 0 ? 1 : 0);
		args[n++] = "--trust-enforce";
		args[n++] = trust;
	}
	struct os_process_spawn_info si;
	memset(&si, 0, sizeof(si));
	si.exe_path = host_exe;
	si.args = args;
	si.arg_count = n;
	si.kill_on_parent_exit = true;
	si.channel_child_handle = OS_NATIVE_HANDLE_INVALID;
	si.log_path = host_log_path();
	struct os_process *proc = NULL;
	if (os_process_spawn(&si, &proc) != 0) {
		return false;
	}
	int32_t code = -1;
	if (os_process_wait(proc, timeout_ms, &code) != 1) {
		U_LOG_W("viewer tracking host: probe pid %ld took over %u ms — killed; no provider this run",
		        os_process_pid(proc), timeout_ms);
		(void)os_process_terminate(proc, 30);
		(void)os_process_wait(proc, 2000, &code);
		os_process_destroy(&proc);
		if (out_ran != NULL) {
			*out_ran = true;
		}
		return false;
	}
	os_process_destroy(&proc);
	if (out_ran != NULL) {
		*out_ran = true;
	}
	if (code != TARGET_VHOST_EXIT_ORDERLY) {
		if (code != TARGET_VHOST_EXIT_NO_PROVIDER) {
			U_LOG_W("viewer tracking host: probe exited %d (%s)", (int)code, target_vhost_exit_str(code));
		}
		return false;
	}
	struct u_vpp_producer_status ps;
	if (!u_vpp_read_producer(page_mem, &ps) || ps.provider_id[0] == '\0') {
		return false;
	}
	if (out_id != NULL && id_cap > 0) {
		snprintf(out_id, id_cap, "%s", ps.provider_id);
	}
	return true;
}

#else // !VHOST_HAVE_PROCESS (Android)

struct target_viewer_host *
target_viewer_host_create(const struct target_viewer_host_config *cfg)
{
	(void)cfg;
	return NULL;
}

void
target_viewer_host_demand(struct target_viewer_host *h)
{
	(void)h;
}

void
target_viewer_host_get_status(struct target_viewer_host *h, struct target_viewer_host_status *out)
{
	(void)h;
	memset(out, 0, sizeof(*out));
	out->state = TARGET_VHOST_SUP_STOPPED;
	out->last_exit = INT32_MIN;
}

void
target_viewer_host_destroy(struct target_viewer_host **h_ptr)
{
	(void)h_ptr;
}

bool
target_viewer_host_probe(const char *host_exe,
                         const char *page_name,
                         const void *page_mem,
                         int trust_enforce,
                         uint32_t timeout_ms,
                         char *out_id,
                         size_t id_cap,
                         bool *out_ran)
{
	(void)host_exe;
	(void)page_name;
	(void)page_mem;
	(void)trust_enforce;
	(void)timeout_ms;
	(void)out_id;
	(void)id_cap;
	if (out_ran != NULL) {
		*out_ran = false;
	}
	return false;
}

#endif
