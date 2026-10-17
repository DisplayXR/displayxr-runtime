// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  DisplayXR-SimConversion: the hardware-free reference conversion
 *         provider (ADR-053; discovery spec §6.1).
 *
 * Exposes EVERY task of the v1 vocabulary as one capability each (cap_id ==
 * task value), computes on the CPU (DETERMINISTIC), and imports D3D11 NT
 * handles on Windows only to exercise that transport. Readiness and timings
 * are scriptable through host->get_setting (README.md in this directory), so
 * tests and CI can drive WARMING → READY, DOWNLOADING with progress, every
 * FAILED reason, a crash, a hang and mid-job failures without any model.
 *
 * FALLBACK (ProbeOrder 250, XRT_CONV_PROVIDER_FLAG_FALLBACK): selection never
 * picks it over a real capability. probe() declines unless setting ENABLE=1.
 *
 * @ingroup drv_sim_conversion
 */

#include "xrt/xrt_conversion_plugin.h"

#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_logging.h"

#include "sim_conv_fake_ply.h"
#include "sim_conv_synth.h"

#ifdef _WIN32
#include "sim_conv_d3d11_io.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 *
 * Constants.
 *
 */

#define SIM_CONV_ID "sim-conversion"
#define SIM_CONV_CAP_COUNT 7u //!< DEPTH_MONO .. SPLATS_STEREO; cap_id == task
#define SIM_CONV_MAX_STREAMS 8u
#define SIM_CONV_MAX_STEPS 8u
#define SIM_CONV_POLL_NS (50ll * 1000 * 1000) //!< cancel poll period (≤ 50 ms, contract ≤ 250 ms)
#define SIM_CONV_SINK_CHUNK (64u * 1024u)

#ifdef _WIN32
#define SIM_CONV_IMPORT_KINDS (XRT_CONV_MEM_CPU | XRT_CONV_MEM_WIN32_NT_HANDLE)
#define SIM_CONV_SYNC_KINDS ((1u << XRT_CONV_SYNC_NONE) | (1u << XRT_CONV_SYNC_FENCE_NT))
#else
#define SIM_CONV_IMPORT_KINDS XRT_CONV_MEM_CPU
#define SIM_CONV_SYNC_KINDS (1u << XRT_CONV_SYNC_NONE)
#endif

#define SIM_CONV_COLOR_FORMATS                                                                                         \
	((1u << XRT_CONV_FMT_RGBA8_UNORM) | (1u << XRT_CONV_FMT_RGBA8_SRGB) | (1u << XRT_CONV_FMT_BGRA8_UNORM) |       \
	 (1u << XRT_CONV_FMT_BGRA8_SRGB))

#define COVERS(PTR, TYPE, FIELD) XRT_CONV_COVERS(PTR, TYPE, FIELD)
#define SET(PTR, TYPE, FIELD, VALUE)                                                                                   \
	do {                                                                                                           \
		if (COVERS(PTR, TYPE, FIELD)) {                                                                        \
			(PTR)->FIELD = (VALUE);                                                                        \
		}                                                                                                      \
	} while (0)

/*
 *
 * Types.
 *
 */

struct sim_step
{
	uint32_t state;  //!< XRT_CONV_STATE_*
	uint32_t reason; //!< XRT_CONV_FAIL_* (FAILED only)
	uint32_t ms;     //!< duration; ignored on the last (terminal) step
};

struct sim_cap
{
	uint32_t task;
	// Script.
	struct sim_step steps[SIM_CONV_MAX_STEPS];
	uint32_t step_count;
	bool cold; //!< report INSTALLED before the first warm even for a one-step script
	// Runtime.
	bool started;
	bool evicted;
	int64_t start_ns;
	uint32_t override_state; //!< non-zero: DEVICE_LOST etc. until the next warm
	uint32_t override_reason;
	uint32_t last_reported; //!< for notify_changed on transitions
	// #2108: the DOWNLOADING step gated on the host's model store ("sim/fake").
	bool store_gate;       //!< the host's store knows the fake pack: the step waits for it
	bool gate_done;        //!< the store reported READY
	bool gate_terminal;    //!< the store refused / failed: the next warm restarts the script
	int64_t gate_extra_ns; //!< how long the step was held past its DOWNLOAD_MS
};

//! The fake pack model the DOWNLOADING step fetches through the host's store.
#define SIM_CONV_FAKE_MODEL "sim/fake"

struct sim_stream
{
	bool used;
	uint64_t id;
	uint32_t cap_id;
	struct xrt_conv_stream_info info;
	uint64_t frames;
	uint64_t phase; //!< DEPTH_VIDEO temporal phase; FRAME_RESET snaps it to 0
};

struct sim_scratch
{
	uint8_t *ptr;
	size_t size;
};

struct sim_conv
{
	struct os_mutex lock;
	struct sim_cap caps[SIM_CONV_CAP_COUNT];
	struct sim_stream streams[SIM_CONV_MAX_STREAMS];
	uint64_t next_stream_id;

	// Knobs (settings).
	uint32_t latency_ms;
	uint32_t job_ms;
	float job_fail_at; //!< < 0 = never
	uint32_t hang_ms;
	uint32_t crash_after;
	uint32_t device_lost_every;
	bool research;

	// Counters.
	uint64_t work_count;
	bool crash_looped;

	struct sim_scratch scratch[3];
#ifdef _WIN32
	struct sim_conv_d3d11_io *io;
#endif
};

static const struct xrt_conversion_plugin_host_iface *g_host = NULL;

/*
 *
 * Small helpers.
 *
 */

static char
lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

//! Case-insensitive equality of [s, s+n) with z (portable; no strncasecmp).
static bool
eq_n(const char *s, size_t n, const char *z)
{
	size_t i = 0;
	for (; i < n && z[i] != 0; i++) {
		if (lower(s[i]) != lower(z[i])) {
			return false;
		}
	}
	return i == n && z[i] == 0;
}

static void
copy_str(char *dst, size_t cap, const char *src)
{
	size_t n = strlen(src);
	if (n >= cap) {
		n = cap - 1;
	}
	memcpy(dst, src, n);
	dst[n] = 0;
}

static bool
setting(const char *key, char *buf, uint32_t size)
{
	buf[0] = 0;
	if (g_host == NULL || !COVERS(g_host, struct xrt_conversion_plugin_host_iface, get_setting) ||
	    g_host->get_setting == NULL) {
		return false;
	}
	return g_host->get_setting(key, buf, size) && buf[0] != 0;
}

static bool
setting_u32(const char *key, uint32_t *out)
{
	char buf[32];
	if (!setting(key, buf, sizeof(buf))) {
		return false;
	}
	char *end = NULL;
	unsigned long v = strtoul(buf, &end, 10);
	if (end == buf) {
		return false;
	}
	*out = (uint32_t)v;
	return true;
}

static void
notify(uint32_t cap_id)
{
	if (g_host != NULL && COVERS(g_host, struct xrt_conversion_plugin_host_iface, notify_changed) &&
	    g_host->notify_changed != NULL) {
		g_host->notify_changed(cap_id);
	}
}

static bool
sleep_cancellable(uint32_t ms, bool (*is_cancelled)(void *), void *ctx)
{
	int64_t end = os_monotonic_get_ns() + (int64_t)ms * 1000 * 1000;
	for (;;) {
		if (is_cancelled != NULL && is_cancelled(ctx)) {
			return false;
		}
		int64_t now = os_monotonic_get_ns();
		if (now >= end) {
			return true;
		}
		int64_t d = end - now;
		os_nanosleep(d < SIM_CONV_POLL_NS ? d : SIM_CONV_POLL_NS);
	}
}

static uint8_t *
scratch_get(struct sim_conv *sc, uint32_t i, size_t size)
{
	struct sim_scratch *s = &sc->scratch[i];
	if (s->size < size) {
		uint8_t *p = (uint8_t *)realloc(s->ptr, size);
		if (p == NULL) {
			return NULL;
		}
		s->ptr = p;
		s->size = size;
	}
	return s->ptr;
}

static const char *
task_name(uint32_t task)
{
	switch (task) {
	case XRT_CONV_TASK_DEPTH_MONO: return "depth-mono";
	case XRT_CONV_TASK_DEPTH_VIDEO: return "depth-video";
	case XRT_CONV_TASK_DISPARITY_STEREO: return "disparity-stereo";
	case XRT_CONV_TASK_VIEWS_MONO: return "views-mono";
	case XRT_CONV_TASK_VIEWS_FROM_DEPTH: return "views-from-depth";
	case XRT_CONV_TASK_SPLATS_MONO: return "splats-mono";
	case XRT_CONV_TASK_SPLATS_STEREO: return "splats-stereo";
	default: return "?";
	}
}

static const char *
state_name(uint32_t s)
{
	switch (s) {
	case XRT_CONV_STATE_READY: return "ready";
	case XRT_CONV_STATE_WARMING: return "warming";
	case XRT_CONV_STATE_ABSENT: return "absent";
	case XRT_CONV_STATE_INCOMPATIBLE: return "incompatible";
	case XRT_CONV_STATE_UNLICENSED: return "unlicensed";
	case XRT_CONV_STATE_FAILED: return "failed";
	case XRT_CONV_STATE_DISABLED: return "disabled";
	case XRT_CONV_STATE_INSTALLED: return "installed";
	case XRT_CONV_STATE_DOWNLOADING: return "downloading";
	case XRT_CONV_STATE_EVICTED: return "evicted";
	default: return "unknown";
	}
}

static const char *const k_reason_names[] = {
    "none", "licence", "load", "platform", "network", "version", "resources", "model", "device_lost", "crash_loop",
};

static bool
parse_state(const char *s, size_t n, uint32_t *out)
{
	for (uint32_t v = XRT_CONV_STATE_READY; v <= XRT_CONV_STATE_EVICTED; v++) {
		if (eq_n(s, n, state_name(v))) {
			*out = v;
			return true;
		}
	}
	return false;
}

static bool
parse_reason(const char *s, size_t n, uint32_t *out)
{
	for (uint32_t v = 0; v < sizeof(k_reason_names) / sizeof(k_reason_names[0]); v++) {
		if (eq_n(s, n, k_reason_names[v])) {
			*out = v;
			return true;
		}
	}
	// Accept "license" too.
	if (eq_n(s, n, "license")) {
		*out = XRT_CONV_FAIL_LICENCE;
		return true;
	}
	return false;
}

static bool
reason_retryable(uint32_t r)
{
	return r == XRT_CONV_FAIL_LOAD || r == XRT_CONV_FAIL_NETWORK || r == XRT_CONV_FAIL_RESOURCES ||
	       r == XRT_CONV_FAIL_DEVICE_LOST;
}

static struct sim_cap *
cap_get(struct sim_conv *sc, uint32_t cap_id)
{
	if (cap_id == 0 || cap_id > SIM_CONV_CAP_COUNT) {
		return NULL;
	}
	return &sc->caps[cap_id - 1];
}

/*
 *
 * Script (settings → per-capability step lists).
 *
 */

struct cap_plan
{
	uint32_t download_ms;
	uint32_t warm_ms;
	struct sim_step terminal; //!< default READY
	bool explicit_steps;      //!< "state=" given: steps[] below wins
	struct sim_step steps[SIM_CONV_MAX_STEPS];
	uint32_t step_count;
	bool cold;
};

static void
plan_build(const struct cap_plan *p, struct sim_cap *c)
{
	c->cold = p->cold;
	if (p->explicit_steps) {
		memcpy(c->steps, p->steps, sizeof(p->steps));
		c->step_count = p->step_count;
		return;
	}
	uint32_t n = 0;
	if (p->download_ms > 0) {
		c->steps[n++] = (struct sim_step){XRT_CONV_STATE_DOWNLOADING, 0, p->download_ms};
	}
	if (p->warm_ms > 0) {
		c->steps[n++] = (struct sim_step){XRT_CONV_STATE_WARMING, 0, p->warm_ms};
	}
	c->steps[n++] = p->terminal;
	c->step_count = n;
}

//! "name[(reason)][:ms]" → step.
static bool
parse_step(const char *s, size_t n, struct sim_step *out)
{
	size_t name_end = n;
	for (size_t i = 0; i < n; i++) {
		if (s[i] == '(' || s[i] == ':') {
			name_end = i;
			break;
		}
	}
	struct sim_step st = {0, 0, 0};
	if (!parse_state(s, name_end, &st.state)) {
		return false;
	}
	size_t i = name_end;
	if (i < n && s[i] == '(') {
		size_t close = i + 1;
		while (close < n && s[close] != ')') {
			close++;
		}
		if (close >= n || !parse_reason(s + i + 1, close - i - 1, &st.reason)) {
			return false;
		}
		i = close + 1;
	}
	if (i < n && s[i] == ':') {
		char num[16];
		size_t len = n - i - 1;
		if (len == 0 || len >= sizeof(num)) {
			return false;
		}
		memcpy(num, s + i + 1, len);
		num[len] = 0;
		char *end = NULL;
		st.ms = (uint32_t)strtoul(num, &end, 10);
		if (end == num) {
			// STATE_<cap> = failed:<reason> (spec §6.1 form).
			if (st.state == XRT_CONV_STATE_FAILED && parse_reason(s + i + 1, len, &st.reason)) {
				st.ms = 0;
			} else {
				return false;
			}
		}
		i = n;
	}
	if (i != n) {
		return false;
	}
	if (st.state == XRT_CONV_STATE_FAILED && st.reason == XRT_CONV_FAIL_NONE) {
		st.reason = XRT_CONV_FAIL_LOAD;
	}
	*out = st;
	return true;
}

static bool
parse_steps(const char *s, size_t n, struct sim_step *steps, uint32_t *count)
{
	uint32_t c = 0;
	size_t b = 0;
	for (size_t i = 0; i <= n; i++) {
		if (i == n || s[i] == ',') {
			if (c >= SIM_CONV_MAX_STEPS || !parse_step(s + b, i - b, &steps[c])) {
				return false;
			}
			c++;
			b = i + 1;
		}
	}
	*count = c;
	return c > 0;
}

//! Target of a directive: 0 = every cap, else a cap_id.
static bool
parse_target(const char *s, size_t n, uint32_t *out)
{
	if (n == 0) {
		*out = 0;
		return true;
	}
	if (n == 1 && s[0] >= '1' && s[0] <= '0' + (char)SIM_CONV_CAP_COUNT) {
		*out = (uint32_t)(s[0] - '0');
		return true;
	}
	for (uint32_t t = 1; t <= SIM_CONV_CAP_COUNT; t++) {
		const char *name = task_name(t);
		// views-mono and VIEWS_MONO both accepted.
		bool match = strlen(name) == n;
		for (size_t i = 0; match && i < n; i++) {
			char a = lower(s[i]) == '_' ? '-' : lower(s[i]);
			match = a == name[i];
		}
		if (match) {
			*out = t;
			return true;
		}
	}
	return false;
}

static void
apply_directive(struct sim_conv *sc, struct cap_plan plans[], const char *s, size_t n)
{
	// Trim.
	while (n > 0 && (s[0] == ' ' || s[0] == '\t')) {
		s++;
		n--;
	}
	while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
		n--;
	}
	if (n == 0) {
		return;
	}
	size_t eq = 0;
	while (eq < n && s[eq] != '=') {
		eq++;
	}
	if (eq == n) {
		U_LOG_W("sim conversion: SCRIPT directive without '=' ignored: %.*s", (int)n, s);
		return;
	}
	size_t dot = eq;
	for (size_t i = 0; i < eq; i++) {
		if (s[i] == '.') {
			dot = i;
		}
	}
	uint32_t target = 0;
	const char *key = s;
	size_t key_n = eq;
	if (dot < eq) {
		if (!parse_target(s, dot, &target)) {
			U_LOG_W("sim conversion: SCRIPT unknown task '%.*s' ignored", (int)dot, s);
			return;
		}
		key = s + dot + 1;
		key_n = eq - dot - 1;
	}
	const char *val = s + eq + 1;
	const size_t val_n = n - eq - 1;
	char num[16] = {0};
	if (val_n < sizeof(num)) {
		memcpy(num, val, val_n);
	}
	const uint32_t u = (uint32_t)strtoul(num, NULL, 10);

	// Global knobs (no per-task form).
	if (eq_n(key, key_n, "latency_ms")) {
		sc->latency_ms = u;
		return;
	}
	if (eq_n(key, key_n, "job_ms")) {
		sc->job_ms = u;
		return;
	}
	if (eq_n(key, key_n, "crash_after")) {
		sc->crash_after = u;
		return;
	}
	if (eq_n(key, key_n, "hang_ms")) {
		sc->hang_ms = u;
		return;
	}

	for (uint32_t c = 1; c <= SIM_CONV_CAP_COUNT; c++) {
		if (target != 0 && target != c) {
			continue;
		}
		struct cap_plan *p = &plans[c - 1];
		if (eq_n(key, key_n, "state")) {
			if (!parse_steps(val, val_n, p->steps, &p->step_count)) {
				U_LOG_W("sim conversion: SCRIPT bad state list '%.*s' ignored", (int)val_n, val);
				return;
			}
			p->explicit_steps = true;
		} else if (eq_n(key, key_n, "download") || eq_n(key, key_n, "download_ms")) {
			p->download_ms = u;
		} else if (eq_n(key, key_n, "warm") || eq_n(key, key_n, "warm_ms")) {
			p->warm_ms = u;
		} else if (eq_n(key, key_n, "fail")) {
			uint32_t r = 0;
			if (!parse_reason(val, val_n, &r)) {
				U_LOG_W("sim conversion: SCRIPT unknown failure reason '%.*s' ignored", (int)val_n,
				        val);
				return;
			}
			p->terminal = (struct sim_step){XRT_CONV_STATE_FAILED, r, 0};
		} else if (eq_n(key, key_n, "cold")) {
			p->cold = u != 0;
		} else {
			U_LOG_W("sim conversion: SCRIPT unknown key '%.*s' ignored", (int)key_n, key);
			return;
		}
	}
}

static void
load_settings(struct sim_conv *sc)
{
	struct cap_plan plans[SIM_CONV_CAP_COUNT];
	memset(plans, 0, sizeof(plans));

	uint32_t warm_ms = 0, download_ms = 0;
	setting_u32("WARM_MS", &warm_ms);
	setting_u32("DOWNLOAD_MS", &download_ms);

	sc->latency_ms = 8;
	sc->job_ms = 2000;
	sc->job_fail_at = -1.0f;
	setting_u32("LATENCY_MS", &sc->latency_ms);
	setting_u32("JOB_MS", &sc->job_ms);
	setting_u32("HANG_MS", &sc->hang_ms);
	setting_u32("CRASH_AFTER", &sc->crash_after);
	setting_u32("DEVICE_LOST_EVERY", &sc->device_lost_every);

	char buf[512];
	if (setting("JOB_FAIL_AT", buf, sizeof(buf))) {
		sc->job_fail_at = (float)strtod(buf, NULL);
	}
	if (setting("LICENCE", buf, sizeof(buf)) || setting("LICENSE", buf, sizeof(buf))) {
		sc->research = eq_n(buf, strlen(buf), "research");
	}

	for (uint32_t c = 1; c <= SIM_CONV_CAP_COUNT; c++) {
		struct cap_plan *p = &plans[c - 1];
		p->warm_ms = warm_ms;
		p->download_ms = download_ms;
		p->terminal = (struct sim_step){XRT_CONV_STATE_READY, 0, 0};

		// STATE_<cap>: the cap id, or the task name upper-cased (STATE_VIEWS_MONO).
		char key[48];
		char tn[32];
		copy_str(tn, sizeof(tn), task_name(c));
		for (char *q = tn; *q; q++) {
			*q = *q == '-' ? '_' : (char)(*q >= 'a' && *q <= 'z' ? *q - 'a' + 'A' : *q);
		}
		bool got = false;
		snprintf(key, sizeof(key), "STATE_%u", c);
		got = setting(key, buf, sizeof(buf));
		if (!got) {
			snprintf(key, sizeof(key), "STATE_%s", tn);
			got = setting(key, buf, sizeof(buf));
		}
		if (got) {
			struct sim_step st;
			if (!parse_step(buf, strlen(buf), &st)) {
				U_LOG_W("sim conversion: %s='%s' not understood; ignored", key, buf);
			} else if (st.state == XRT_CONV_STATE_INSTALLED) {
				p->cold = true; // present and cold: warm() makes it READY
			} else {
				p->terminal = st;
				p->warm_ms = 0;
				p->download_ms = 0;
			}
		}
	}

	if (setting("SCRIPT", buf, sizeof(buf))) {
		size_t n = strlen(buf);
		size_t b = 0;
		for (size_t i = 0; i <= n; i++) {
			if (i == n || buf[i] == ';') {
				apply_directive(sc, plans, buf + b, i - b);
				b = i + 1;
			}
		}
	}

	for (uint32_t c = 1; c <= SIM_CONV_CAP_COUNT; c++) {
		sc->caps[c - 1].task = c;
		plan_build(&plans[c - 1], &sc->caps[c - 1]);
	}
}

/*
 *
 * Readiness.
 *
 */

struct eval
{
	uint32_t state;
	uint32_t reason;
	float progress;
	uint64_t eta_ns;
	bool terminal; //!< in the last step (warm() is done)
};

static bool
host_has_resolve(void);

//! Current state of @p c. Caller holds the lock. May advance the store gate.
static struct eval
cap_eval(const struct sim_conv *sc, struct sim_cap *c, int64_t now)
{
	struct eval e = {XRT_CONV_STATE_UNKNOWN, 0, 0.0f, 0, true};
	if (sc->crash_looped) {
		e.state = XRT_CONV_STATE_FAILED;
		e.reason = XRT_CONV_FAIL_CRASH_LOOP;
		return e;
	}
	if (c->override_state != 0) {
		e.state = c->override_state;
		e.reason = c->override_reason;
		return e;
	}
	if (!c->started) {
		if (c->evicted) {
			e.state = XRT_CONV_STATE_EVICTED;
		} else if (c->step_count == 1 && !c->cold) {
			e.state = c->steps[0].state;
			e.reason = c->steps[0].reason;
		} else {
			e.state = XRT_CONV_STATE_INSTALLED;
		}
		e.terminal = false;
		return e;
	}
	int64_t t = now - c->start_ns - c->gate_extra_ns;
	for (uint32_t i = 0; i + 1 < c->step_count; i++) {
		const int64_t d = (int64_t)c->steps[i].ms * 1000 * 1000;
		float store_progress = -1.0f;
		if (c->store_gate && !c->gate_done && c->steps[i].state == XRT_CONV_STATE_DOWNLOADING &&
		    host_has_resolve() && t >= 0) {
			// The host's store protocol (spec §6.1): the step lasts DOWNLOAD_MS AND
			// until the store has the blob; refusal / failure ends the script.
			struct xrt_conv_model_location loc;
			memset(&loc, 0, sizeof(loc));
			loc.struct_size = (uint32_t)sizeof(loc);
			(void)g_host->model_resolve(SIM_CONV_FAKE_MODEL, &loc);
			if (loc.state == XRT_CONV_STATE_READY) {
				c->gate_done = true;
			} else if (loc.state == XRT_CONV_STATE_ABSENT || loc.state == XRT_CONV_STATE_FAILED) {
				c->gate_terminal = true;
				e.state = loc.state;
				e.reason = loc.failure_reason != 0 ? loc.failure_reason : XRT_CONV_FAIL_MODEL;
				e.terminal = true;
				return e;
			} else {
				if (t >= d) {
					c->gate_extra_ns += t - d + 1;
					t = d - 1;
				}
				store_progress = loc.progress;
			}
		}
		if (t < d) {
			e.state = c->steps[i].state;
			e.reason = c->steps[i].reason;
			e.progress = store_progress >= 0.0f ? store_progress : d > 0 ? (float)((double)t / (double)d) : 0.0f;
			e.eta_ns = (uint64_t)(d - t);
			for (uint32_t j = i + 1; j + 1 < c->step_count; j++) {
				e.eta_ns += (uint64_t)c->steps[j].ms * 1000 * 1000;
			}
			e.terminal = false;
			return e;
		}
		t -= d;
	}
	const struct sim_step *last = &c->steps[c->step_count - 1];
	e.state = last->state;
	e.reason = last->reason;
	e.progress =
	    (last->state == XRT_CONV_STATE_WARMING || last->state == XRT_CONV_STATE_DOWNLOADING) ? -1.0f : 0.0f;
	return e;
}

static bool
host_has_resolve(void)
{
	return g_host != NULL && COVERS(g_host, struct xrt_conversion_plugin_host_iface, model_resolve) &&
	       g_host->model_resolve != NULL;
}

static bool
cap_ready(struct sim_conv *sc, uint32_t cap_id)
{
	struct sim_cap *c = cap_get(sc, cap_id);
	return c != NULL && cap_eval(sc, c, os_monotonic_get_ns()).state == XRT_CONV_STATE_READY;
}

/*!
 * Count one unit of work (convert / job) for CRASH_AFTER and DEVICE_LOST_EVERY.
 * Returns false when the work must fail. Caller holds the lock.
 */
static bool
work_tick(struct sim_conv *sc, uint32_t cap_id, uint32_t *notify_cap)
{
	*notify_cap = UINT32_MAX;
	if (sc->crash_looped) {
		return false;
	}
	sc->work_count++;
	if (sc->crash_after > 0 && sc->work_count > sc->crash_after) {
		const bool satellite = g_host != NULL && (g_host->host_flags & 1u) != 0;
		if (satellite) {
			// The satellite test vehicle: a real process death, which the
			// supervisor sees as a crash (restart / crash-loop cutoff).
			U_LOG_E("sim conversion: CRASH_AFTER=%u reached in a satellite host; exiting the process",
			        sc->crash_after);
			_Exit(70);
		}
		// In-service the provider must never take the service down: report
		// the crash-loop cutoff the supervisor would have reached.
		U_LOG_W("sim conversion: CRASH_AFTER=%u reached in-service; every capability now FAILED(CRASH_LOOP)",
		        sc->crash_after);
		sc->crash_looped = true;
		*notify_cap = 0;
		return false;
	}
	if (sc->device_lost_every > 0 && sc->work_count % sc->device_lost_every == 0) {
		struct sim_cap *c = cap_get(sc, cap_id);
		if (c != NULL) {
			c->override_state = XRT_CONV_STATE_FAILED;
			c->override_reason = XRT_CONV_FAIL_DEVICE_LOST;
			*notify_cap = cap_id;
		}
		return false;
	}
	return true;
}

/*
 *
 * Image access.
 *
 */

struct cpu_view
{
	const uint8_t *ptr;
	uint32_t stride;
	uint32_t width, height;
	uint32_t format;
};

static bool
is_bgra(uint32_t f)
{
	return f == XRT_CONV_FMT_BGRA8_UNORM || f == XRT_CONV_FMT_BGRA8_SRGB;
}

static bool
is_color4(uint32_t f)
{
	return f < 32 && (SIM_CONV_COLOR_FORMATS & (1u << f)) != 0;
}

static bool
read_image(struct sim_conv *sc, const struct xrt_conv_image *img, uint32_t slot, struct cpu_view *out)
{
	(void)sc;
	(void)slot;
	if (img == NULL || img->width == 0 || img->height == 0) {
		return false;
	}
	out->width = img->width;
	out->height = img->height;
	out->format = img->format;
	if (img->memory_kind == XRT_CONV_MEM_CPU) {
		if (img->cpu_ptr == NULL) {
			return false;
		}
		out->ptr = (const uint8_t *)img->cpu_ptr;
		out->stride = img->row_stride != 0 ? img->row_stride : img->width * 4u;
		return true;
	}
#ifdef _WIN32
	if (img->memory_kind == XRT_CONV_MEM_WIN32_NT_HANDLE) {
		if (sc->io == NULL) {
			sc->io = sim_conv_d3d11_io_create();
		}
		return sim_conv_d3d11_io_read(sc->io, img, slot, &out->ptr, &out->stride);
	}
#endif
	return false;
}

//! A writable CPU destination for target @p img (its memory, or scratch @p si to upload later).
static uint8_t *
target_dst(
    struct sim_conv *sc, const struct xrt_conv_image *img, uint32_t si, uint32_t bpp, uint32_t layers, uint32_t *stride)
{
	if (img->memory_kind == XRT_CONV_MEM_CPU) {
		*stride = img->row_stride != 0 ? img->row_stride : img->width * bpp;
		return (uint8_t *)img->cpu_ptr;
	}
	*stride = img->width * bpp;
	return scratch_get(sc, si, (size_t)*stride * img->height * layers);
}

//! Push a scratch-built target to its GPU memory (no-op for CPU targets).
static bool
target_commit(
    struct sim_conv *sc, const struct xrt_conv_image *img, const uint8_t *src, uint32_t stride, uint32_t layers)
{
	if (img->memory_kind == XRT_CONV_MEM_CPU) {
		return true;
	}
#ifdef _WIN32
	if (img->memory_kind == XRT_CONV_MEM_WIN32_NT_HANDLE) {
		// A CPU input with GPU outputs (the satellite's lane: CPU slots in,
		// host-allocated NT-handle outputs) reaches here before any GPU read
		// created the I/O device.
		if (sc->io == NULL) {
			sc->io = sim_conv_d3d11_io_create();
		}
		if (sc->io == NULL) {
			return false;
		}
		if (layers <= 1) {
			return sim_conv_d3d11_io_write(sc->io, img, -1, src, stride);
		}
		for (uint32_t l = 0; l < layers; l++) {
			if (!sim_conv_d3d11_io_write(sc->io, img, (int32_t)l, src + (size_t)l * stride * img->height,
			                             stride)) {
				return false;
			}
		}
		return true;
	}
#else
	(void)sc;
	(void)src;
	(void)stride;
	(void)layers;
#endif
	return false;
}

static bool
target_supported(const struct xrt_conv_image *img)
{
	if (img->memory_kind == XRT_CONV_MEM_CPU) {
		return img->cpu_ptr != NULL;
	}
	return (img->memory_kind & SIM_CONV_IMPORT_KINDS) != 0 && img->sync.kind < 32 &&
	       (SIM_CONV_SYNC_KINDS & (1u << img->sync.kind)) != 0;
}

/*
 *
 * Provider iface: describe / state.
 *
 */

static bool
sc_get_provider_status(struct xrt_conv_provider_status *out)
{
	if (out == NULL) {
		return false;
	}
	SET(out, struct xrt_conv_provider_status, state, 1u /* XRT_PLUGIN_PLATFORM_STATE_READY */);
	SET(out, struct xrt_conv_provider_status, flags, XRT_CONV_PROVIDER_FLAG_FALLBACK);
	if (COVERS(out, struct xrt_conv_provider_status, hint)) {
		copy_str(out->hint, sizeof(out->hint),
		         "hardware-free reference provider; synthetic outputs, not a model");
	}
	return true;
}

/*!
 * SCRATCH_TOUCH=1 (#2194): ask the host for XRT_CONV_PATH_SCRATCH and write
 * `sim-scratch-touch` there (temp + rename, the §2.4 rule), so a test can
 * see which directory a host hands a provider, across the satellite boundary.
 */
static void
scratch_touch(void)
{
	char buf[16];
	if (!setting("SCRATCH_TOUCH", buf, sizeof(buf)) || strtoul(buf, NULL, 10) == 0) {
		return;
	}
	char dir[512];
	if (g_host == NULL || !COVERS(g_host, struct xrt_conversion_plugin_host_iface, get_path) ||
	    g_host->get_path == NULL || !g_host->get_path(XRT_CONV_PATH_SCRATCH, dir, sizeof(dir))) {
		U_LOG_W("sim conversion: SCRATCH_TOUCH: the host gave no scratch directory");
		return;
	}
	char tmp[600], fin[600];
	(void)snprintf(tmp, sizeof(tmp), "%s/sim-scratch-touch.tmp", dir);
	(void)snprintf(fin, sizeof(fin), "%s/sim-scratch-touch", dir);
	FILE *f = fopen(tmp, "wb");
	if (f == NULL) {
		U_LOG_W("sim conversion: SCRATCH_TOUCH: cannot write %s", tmp);
		return;
	}
	(void)fprintf(f, "%s\n", dir);
	(void)fclose(f);
	(void)remove(fin); // Windows rename does not replace
	if (rename(tmp, fin) != 0) {
		(void)remove(tmp);
		U_LOG_W("sim conversion: SCRATCH_TOUCH: cannot rename into %s", fin);
		return;
	}
	U_LOG_W("sim conversion: SCRATCH_TOUCH: wrote %s", fin);
}

static xrt_result_t
sc_probe(struct xrt_conversion_plugin_instance **out_inst)
{
	char buf[16];
	if (out_inst == NULL || !setting("ENABLE", buf, sizeof(buf)) || strtoul(buf, NULL, 10) == 0) {
		return XRT_ERROR_PROBER_NOT_SUPPORTED;
	}
	struct sim_conv *sc = (struct sim_conv *)calloc(1, sizeof(*sc));
	if (sc == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	if (os_mutex_init(&sc->lock) != 0) {
		free(sc);
		return XRT_ERROR_ALLOCATION;
	}
	load_settings(sc);
	scratch_touch();
	for (uint32_t c = 1; c <= SIM_CONV_CAP_COUNT; c++) {
		sc->caps[c - 1].last_reported = cap_eval(sc, &sc->caps[c - 1], os_monotonic_get_ns()).state;
	}
	U_LOG_W("sim conversion: provider active (FALLBACK, synthetic outputs; latency %u ms, job %u ms)",
	        sc->latency_ms, sc->job_ms);
	*out_inst = (struct xrt_conversion_plugin_instance *)sc;
	return XRT_SUCCESS;
}

static void
fill_cap(const struct sim_conv *sc, uint32_t task, struct xrt_conv_capability *o)
{
#define C(FIELD, VALUE) SET(o, struct xrt_conv_capability, FIELD, VALUE)
#define S(FIELD, VALUE)                                                                                                \
	do {                                                                                                           \
		if (COVERS(o, struct xrt_conv_capability, FIELD)) {                                                    \
			copy_str(o->FIELD, sizeof(o->FIELD), (VALUE));                                                 \
		}                                                                                                      \
	} while (0)

	uint32_t lanes = XRT_CONV_LANE_STREAM, input = XRT_CONV_INPUT_MONO, outputs = 0;
	uint32_t flags = XRT_CONV_CAP_DETERMINISTIC;
	uint32_t depth_kind = XRT_CONV_DEPTH_UNKNOWN, intr = XRT_CONV_INTRINSICS_NONE;
	uint32_t min_views = 0, max_views = 0, splats = 0, max_streams = SIM_CONV_MAX_STREAMS;
	switch (task) {
	case XRT_CONV_TASK_DEPTH_MONO:
		lanes = XRT_CONV_LANE_STREAM | XRT_CONV_LANE_JOB;
		outputs = XRT_CONV_OUT_DEPTH;
		depth_kind = XRT_CONV_DEPTH_AFFINE;
		break;
	case XRT_CONV_TASK_DEPTH_VIDEO:
		outputs = XRT_CONV_OUT_DEPTH;
		depth_kind = XRT_CONV_DEPTH_AFFINE;
		flags |= XRT_CONV_CAP_TEMPORAL;
		break;
	case XRT_CONV_TASK_DISPARITY_STEREO:
		lanes = XRT_CONV_LANE_STREAM | XRT_CONV_LANE_JOB;
		input = XRT_CONV_INPUT_STEREO_PAIR;
		outputs = XRT_CONV_OUT_DISPARITY;
		depth_kind = XRT_CONV_DEPTH_STEREO_DISPARITY_PX;
		break;
	case XRT_CONV_TASK_VIEWS_MONO:
		outputs = XRT_CONV_OUT_VIEWS | XRT_CONV_OUT_DEPTH;
		flags |= XRT_CONV_CAP_EXPLICIT_VIEWPOINTS | XRT_CONV_CAP_SUGGESTS_CONVERGENCE;
		depth_kind = XRT_CONV_DEPTH_METRIC;
		intr = XRT_CONV_INTRINSICS_ASSUMED;
		min_views = 2;
		max_views = XRT_CONV_MAX_VIEWS;
		break;
	case XRT_CONV_TASK_VIEWS_FROM_DEPTH:
		input = XRT_CONV_INPUT_COLOR_DEPTH;
		outputs = XRT_CONV_OUT_VIEWS;
		flags |= XRT_CONV_CAP_EXPLICIT_VIEWPOINTS;
		min_views = 2;
		max_views = XRT_CONV_MAX_VIEWS;
		break;
	case XRT_CONV_TASK_SPLATS_MONO:
	case XRT_CONV_TASK_SPLATS_STEREO:
		lanes = XRT_CONV_LANE_JOB;
		input = task == XRT_CONV_TASK_SPLATS_MONO ? XRT_CONV_INPUT_MONO : XRT_CONV_INPUT_STEREO_PAIR;
		outputs = XRT_CONV_OUT_SPLAT_BLOB;
		intr = XRT_CONV_INTRINSICS_ASSUMED;
		splats = 1u << XRT_CONV_BLOB_PLY_3DGS;
		max_streams = 0;
		break;
	default: break;
	}
	// DOWNLOADING is simulated (no network), so NEEDS_NETWORK_TO_WARM stays clear.

	char buf[XRT_CONV_NAME_MAX];
	C(cap_id, task);
	C(task, task);
	C(lanes, lanes);
	C(input_kinds, input);
	C(outputs, outputs);
	C(flags, flags);
	C(import_memory_kinds, SIM_CONV_IMPORT_KINDS);
	C(import_sync_kinds, SIM_CONV_SYNC_KINDS);
	C(input_formats, SIM_CONV_COLOR_FORMATS);
	C(depth_output_format, XRT_CONV_FMT_R32_FLOAT);
	C(min_views, min_views);
	C(max_views, max_views);
	C(max_streams, max_streams);
	C(max_concurrent, 1u);
	C(max_input_edge, 0u);
	C(preferred_input_edge, 0u);
	C(depth_kind, depth_kind);
	C(depth_measure, XRT_CONV_MEASURE_PLANAR);
	C(intrinsics_source, intr);
	C(splat_formats, splats);
	C(licence_class, sc->research ? XRT_CONV_LICENCE_RESEARCH_ONLY : XRT_CONV_LICENCE_PERMISSIVE);
	C(commercial_ok, sc->research ? 0u : 1u);
	S(licence_spdx, sc->research ? "LicenseRef-sim-research" : "BSL-1.0");
	snprintf(buf, sizeof(buf), "sim/%s", task_name(task));
	S(model_id, buf);
	S(model_version, "1");
	S(backend, "cpu-reference");
	snprintf(buf, sizeof(buf), "Sim %s", task_name(task));
	S(display_name, buf);
	C(device_luid, 0ull);
	uint32_t warm = 0;
	for (uint32_t i = 0; i + 1 < sc->caps[task - 1].step_count; i++) {
		warm += sc->caps[task - 1].steps[i].ms;
	}
	C(est_warmup_ms, warm);
	C(est_latency_ms, (lanes & XRT_CONV_LANE_STREAM) ? sc->latency_ms : sc->job_ms);
	C(est_vram_mb, 0u);
	C(est_ram_mb, 1u);
#undef C
#undef S
}

static xrt_result_t
sc_describe_cap(struct xrt_conversion_plugin_instance *inst,
                uint32_t index,
                struct xrt_conv_capability *out,
                uint32_t *out_count)
{
	struct sim_conv *sc = (struct sim_conv *)inst;
	if (out_count != NULL) {
		*out_count = SIM_CONV_CAP_COUNT;
	}
	if (out == NULL) {
		return out_count != NULL ? XRT_SUCCESS : XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (sc == NULL || index >= SIM_CONV_CAP_COUNT) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	fill_cap(sc, index + 1, out);
	return XRT_SUCCESS;
}

static uint64_t
sc_get_generation(struct xrt_conversion_plugin_instance *inst)
{
	(void)inst;
	return 1; // the capability set never changes; readiness goes through get_state + notify_changed
}

static xrt_result_t
sc_get_state(struct xrt_conversion_plugin_instance *inst, uint32_t cap_id, struct xrt_conv_state *out)
{
	struct sim_conv *sc = (struct sim_conv *)inst;
	if (sc == NULL || out == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	os_mutex_lock(&sc->lock);
	struct sim_cap *c = cap_get(sc, cap_id);
	if (c == NULL) {
		os_mutex_unlock(&sc->lock);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	struct eval e = cap_eval(sc, c, os_monotonic_get_ns());
	os_mutex_unlock(&sc->lock);

	SET(out, struct xrt_conv_state, state, e.state);
	SET(out, struct xrt_conv_state, failure_reason,
	    e.state == XRT_CONV_STATE_FAILED || e.state == XRT_CONV_STATE_ABSENT ? e.reason : 0u);
	SET(out, struct xrt_conv_state, retryable,
	    e.state == XRT_CONV_STATE_FAILED && reason_retryable(e.reason) ? 1u : 0u);
	SET(out, struct xrt_conv_state, progress, e.progress);
	SET(out, struct xrt_conv_state, eta_ns, e.eta_ns);
	if (COVERS(out, struct xrt_conv_state, version)) {
		copy_str(out->version, sizeof(out->version), "sim-1");
	}
	if (COVERS(out, struct xrt_conv_state, detail)) {
		if (e.state == XRT_CONV_STATE_FAILED) {
			snprintf(out->detail, sizeof(out->detail), "sim: scripted failed(%s)",
			         e.reason < sizeof(k_reason_names) / sizeof(k_reason_names[0])
			             ? k_reason_names[e.reason]
			             : "?");
		} else {
			snprintf(out->detail, sizeof(out->detail), "sim: scripted %s", state_name(e.state));
		}
	}
	return XRT_SUCCESS;
}

/*
 *
 * warm / evict.
 *
 */

static bool
sc_never_cancelled(void *ctx)
{
	(void)ctx;
	return false;
}

static xrt_result_t
sc_warm(struct xrt_conversion_plugin_instance *inst, uint32_t cap_id, bool (*is_cancelled)(void *ctx), void *cancel_ctx)
{
	struct sim_conv *sc = (struct sim_conv *)inst;
	if (is_cancelled == NULL) {
		is_cancelled = sc_never_cancelled;
	}
	os_mutex_lock(&sc->lock);
	struct sim_cap *c = cap_get(sc, cap_id);
	if (c == NULL) {
		os_mutex_unlock(&sc->lock);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	bool request_download = false;
	struct eval e0 = cap_eval(sc, c, os_monotonic_get_ns());
	const bool failed_retry =
	    (e0.state == XRT_CONV_STATE_FAILED && reason_retryable(e0.reason)) || c->gate_terminal;
	if (!c->started || c->override_state != 0 || failed_retry) {
		// (Re)load: run the script from its first step.
		c->started = true;
		c->evicted = false;
		c->override_state = 0;
		c->override_reason = 0;
		c->start_ns = os_monotonic_get_ns();
		c->store_gate = false;
		c->gate_done = false;
		c->gate_terminal = false;
		c->gate_extra_ns = 0;
		for (uint32_t i = 0; i < c->step_count; i++) {
			request_download |= c->steps[i].state == XRT_CONV_STATE_DOWNLOADING;
		}
	}
	os_mutex_unlock(&sc->lock);

	if (request_download && g_host != NULL &&
	    COVERS(g_host, struct xrt_conversion_plugin_host_iface, model_request) && g_host->model_request != NULL) {
		// The host's store protocol (spec §6.1). A host whose store knows the
		// fake pack gates the DOWNLOADING step on it (#2108); any other host
		// (no store, no pack registered) keeps the scripted timing alone.
		(void)g_host->model_request(SIM_CONV_FAKE_MODEL);
		bool known = false;
		if (host_has_resolve()) {
			struct xrt_conv_model_location loc;
			memset(&loc, 0, sizeof(loc));
			loc.struct_size = (uint32_t)sizeof(loc);
			known = g_host->model_resolve(SIM_CONV_FAKE_MODEL, &loc);
		}
		os_mutex_lock(&sc->lock);
		c->store_gate = known;
		os_mutex_unlock(&sc->lock);
	}

	for (;;) {
		os_mutex_lock(&sc->lock);
		struct eval e = cap_eval(sc, c, os_monotonic_get_ns());
		const bool changed = e.state != c->last_reported;
		c->last_reported = e.state;
		os_mutex_unlock(&sc->lock);
		if (changed) {
			U_LOG_W("sim conversion: %s → %s", task_name(cap_id), state_name(e.state));
			notify(cap_id);
		}
		if (e.terminal) {
			switch (e.state) {
			case XRT_CONV_STATE_READY: return XRT_SUCCESS;
			case XRT_CONV_STATE_FAILED: return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
			default: return XRT_ERROR_FEATURE_NOT_SUPPORTED;
			}
		}
		if (is_cancelled(cancel_ctx)) {
			return XRT_TIMEOUT; // cancelled; the load keeps its clock (a later warm resumes)
		}
		os_nanosleep(e.eta_ns > 0 && (int64_t)e.eta_ns < SIM_CONV_POLL_NS ? (int64_t)e.eta_ns
		                                                                  : SIM_CONV_POLL_NS);
	}
}

static xrt_result_t
sc_evict(struct xrt_conversion_plugin_instance *inst, uint32_t cap_id)
{
	struct sim_conv *sc = (struct sim_conv *)inst;
	os_mutex_lock(&sc->lock);
	struct sim_cap *c = cap_get(sc, cap_id);
	if (c == NULL) {
		os_mutex_unlock(&sc->lock);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	struct eval e = cap_eval(sc, c, os_monotonic_get_ns());
	bool changed = false;
	if (e.state == XRT_CONV_STATE_READY) {
		c->started = false;
		c->evicted = true;
		c->last_reported = XRT_CONV_STATE_EVICTED;
		changed = true;
	}
	os_mutex_unlock(&sc->lock);
#ifdef _WIN32
	sim_conv_d3d11_io_clear_cache(sc->io);
#endif
	if (changed) {
		notify(cap_id);
	}
	return XRT_SUCCESS;
}

/*
 *
 * Stream lane.
 *
 */

static struct sim_stream *
stream_find(struct sim_conv *sc, uint64_t id)
{
	for (uint32_t i = 0; i < SIM_CONV_MAX_STREAMS; i++) {
		if (sc->streams[i].used && sc->streams[i].id == id) {
			return &sc->streams[i];
		}
	}
	return NULL;
}

static xrt_result_t
sc_stream_create(struct xrt_conversion_plugin_instance *inst,
                 uint32_t cap_id,
                 const struct xrt_conv_stream_info *info,
                 uint64_t *out_stream)
{
	struct sim_conv *sc = (struct sim_conv *)inst;
	if (sc == NULL || info == NULL || out_stream == NULL ||
	    !COVERS(info, struct xrt_conv_stream_info, input_kind)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	struct xrt_conv_capability cap;
	memset(&cap, 0, sizeof(cap));
	cap.struct_size = (uint32_t)sizeof(cap);
	if (cap_id == 0 || cap_id > SIM_CONV_CAP_COUNT) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	fill_cap(sc, cap_id, &cap);
	if ((cap.lanes & XRT_CONV_LANE_STREAM) == 0 || (info->input_kind & cap.input_kinds) == 0) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	// Copy only what the caller's struct covers; the rest stays zero.
	struct xrt_conv_stream_info si;
	memset(&si, 0, sizeof(si));
	memcpy(&si, info, info->struct_size < sizeof(si) ? info->struct_size : sizeof(si));
	si.struct_size = (uint32_t)sizeof(si);
	if (cap.max_views > 0 && (si.view_count < cap.min_views || si.view_count > cap.max_views)) {
		return XRT_ERROR_OUTPUT_UNSUPPORTED;
	}
	if (si.views_layout != XRT_CONV_VIEWS_ROW && si.views_layout != XRT_CONV_VIEWS_ARRAY) {
		return XRT_ERROR_OUTPUT_UNSUPPORTED;
	}

	xrt_result_t r = XRT_ERROR_CLIENT_LIMIT_REACHED;
	os_mutex_lock(&sc->lock);
	if (!cap_ready(sc, cap_id)) {
		r = XRT_ERROR_FEATURE_NOT_SUPPORTED;
	} else {
		for (uint32_t i = 0; i < SIM_CONV_MAX_STREAMS; i++) {
			struct sim_stream *s = &sc->streams[i];
			if (!s->used) {
				memset(s, 0, sizeof(*s));
				s->used = true;
				s->id = ++sc->next_stream_id;
				s->cap_id = cap_id;
				s->info = si;
				*out_stream = s->id;
				r = XRT_SUCCESS;
				break;
			}
		}
	}
	os_mutex_unlock(&sc->lock);
	return r;
}

static xrt_result_t
sc_stream_get_output_desc(struct xrt_conversion_plugin_instance *inst,
                          uint64_t stream,
                          uint32_t input_width,
                          uint32_t input_height,
                          struct xrt_conv_output_desc *out)
{
	struct sim_conv *sc = (struct sim_conv *)inst;
	if (sc == NULL || out == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	os_mutex_lock(&sc->lock);
	struct sim_stream *s = stream_find(sc, stream);
	struct sim_stream copy;
	if (s != NULL) {
		copy = *s;
	}
	os_mutex_unlock(&sc->lock);
	if (s == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	const uint32_t w = input_width, h = input_height;
#define D(FIELD, VALUE) SET(out, struct xrt_conv_output_desc, FIELD, VALUE)
	switch (copy.cap_id) {
	case XRT_CONV_TASK_DEPTH_MONO:
	case XRT_CONV_TASK_DEPTH_VIDEO:
		D(depth_width, w);
		D(depth_height, h);
		D(depth_format, XRT_CONV_FMT_R32_FLOAT);
		break;
	case XRT_CONV_TASK_DISPARITY_STEREO:
		// Constant disparity: any extent is exact; one eye of an SBS input.
		D(disparity_width, w);
		D(disparity_height, h);
		D(disparity_format, XRT_CONV_FMT_R32_FLOAT);
		break;
	case XRT_CONV_TASK_VIEWS_MONO:
	case XRT_CONV_TASK_VIEWS_FROM_DEPTH: {
		const uint32_t n = copy.info.view_count;
		D(views_width, copy.info.views_layout == XRT_CONV_VIEWS_ROW ? w * n : w);
		D(views_height, h);
		D(views_format, XRT_CONV_FMT_RGBA8_UNORM);
		if (copy.cap_id == XRT_CONV_TASK_VIEWS_MONO && (copy.info.outputs & XRT_CONV_OUT_DEPTH) != 0) {
			D(depth_width, w);
			D(depth_height, h);
			D(depth_format, XRT_CONV_FMT_R32_FLOAT);
		}
		break;
	}
	default: return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
#undef D
	return XRT_SUCCESS;
}

//! Fill the affine depth ramp's depth_info (DEPTH_MONO / DEPTH_VIDEO, stream and job).
static void
depth_info_ramp(struct xrt_conv_depth_info *di, uint32_t w, uint32_t h, float factor, bool temporal)
{
	memset(di, 0, sizeof(*di));
	di->kind = XRT_CONV_DEPTH_AFFINE;
	di->measure = XRT_CONV_MEASURE_PLANAR;
	di->intrinsics_source = XRT_CONV_INTRINSICS_NONE;
	di->value_scale = 1.0f;
	di->invalid_sample = NAN;
	di->range_min = factor * 0.5f / (float)h;
	di->range_max = factor * ((float)h - 0.5f) / (float)h;
	di->source_width = w;
	di->source_height = h;
	di->temporal = temporal ? 1u : 0u;
	di->same_inference = 1;
}

static void
depth_info_disparity(struct xrt_conv_depth_info *di, uint32_t w, uint32_t h, float value)
{
	memset(di, 0, sizeof(*di));
	di->kind = XRT_CONV_DEPTH_STEREO_DISPARITY_PX;
	di->measure = XRT_CONV_MEASURE_PLANAR;
	di->value_scale = 1.0f;
	di->invalid_sample = NAN;
	di->range_min = value;
	di->range_max = value;
	di->source_width = w;
	di->source_height = h;
	di->same_inference = 1;
}

static void
read_params(const struct xrt_conv_params *p,
            uint32_t *frame_flags,
            uint64_t *source_time_ns,
            float *strength,
            const float **vp,
            uint32_t *vp_count,
            float *rect_w,
            float *baseline,
            uint32_t *conv_mode,
            float *convergence,
            struct xrt_conv_stereo_calibration *stereo)
{
	*frame_flags = COVERS(p, struct xrt_conv_params, frame_flags) ? p->frame_flags : 0;
	*source_time_ns = COVERS(p, struct xrt_conv_params, source_time_ns) ? p->source_time_ns : 0;
	*strength = COVERS(p, struct xrt_conv_params, strength) ? p->strength : 1.0f;
	*conv_mode = COVERS(p, struct xrt_conv_params, convergence_mode) ? p->convergence_mode : 0;
	*convergence = COVERS(p, struct xrt_conv_params, convergence) ? p->convergence : 0.0f;
	const bool has_vp = COVERS(p, struct xrt_conv_params, viewpoints_xyz) && p->viewpoint_count > 0;
	*vp = has_vp ? &p->viewpoints_xyz[0][0] : NULL;
	*vp_count = has_vp ? (p->viewpoint_count < XRT_CONV_MAX_VIEWS ? p->viewpoint_count : XRT_CONV_MAX_VIEWS) : 0;
	*rect_w = COVERS(p, struct xrt_conv_params, rect_width_m) ? p->rect_width_m : 0.0f;
	*baseline = COVERS(p, struct xrt_conv_params, baseline_m) ? p->baseline_m : 0.0f;
	memset(stereo, 0, sizeof(*stereo));
	if (COVERS(p, struct xrt_conv_params, stereo)) {
		*stereo = p->stereo;
	}
}

static xrt_result_t
sc_stream_convert(struct xrt_conversion_plugin_instance *inst,
                  uint64_t stream,
                  const struct xrt_conv_frame_in *in,
                  const struct xrt_conv_params *params,
                  struct xrt_conv_frame_out *out)
{
	struct sim_conv *sc = (struct sim_conv *)inst;
	if (sc == NULL || in == NULL || out == NULL || !COVERS(in, struct xrt_conv_frame_in, images) ||
	    !COVERS(out, struct xrt_conv_frame_out, written)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}

	os_mutex_lock(&sc->lock);
	struct sim_stream *s = stream_find(sc, stream);
	if (s == NULL) {
		os_mutex_unlock(&sc->lock);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	uint32_t notify_cap = UINT32_MAX;
	const bool ready = cap_ready(sc, s->cap_id);
	const bool go = ready && work_tick(sc, s->cap_id, &notify_cap);
	const uint32_t cap_id = s->cap_id;
	const struct xrt_conv_stream_info info = s->info;
	const uint64_t frame = s->frames++;
	uint32_t ff0 = 0;
	if (params != NULL && COVERS(params, struct xrt_conv_params, frame_flags)) {
		ff0 = params->frame_flags;
	}
	if ((ff0 & XRT_CONV_FRAME_RESET) != 0) {
		s->phase = 0;
	}
	const uint64_t phase = s->phase++;
	os_mutex_unlock(&sc->lock);
	if (notify_cap != UINT32_MAX) {
		notify(notify_cap);
	}
	if (!go) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}

	uint32_t frame_flags, conv_mode;
	uint64_t source_time_ns;
	float strength, rect_w, baseline, convergence;
	const float *vp;
	uint32_t vp_count;
	struct xrt_conv_stereo_calibration stereo;
	struct xrt_conv_params empty;
	memset(&empty, 0, sizeof(empty));
	read_params(params != NULL ? params : &empty, &frame_flags, &source_time_ns, &strength, &vp, &vp_count, &rect_w,
	            &baseline, &conv_mode, &convergence, &stereo);
	(void)frame_flags;

	// Colour (or the left eye / SBS pair) is the first COLOR image.
	const struct xrt_conv_image *color = NULL, *depth_in = NULL;
	for (uint32_t i = 0; i < in->image_count && i < XRT_CONV_MAX_FRAME_IMAGES; i++) {
		const uint32_t role = COVERS(in, struct xrt_conv_frame_in, image_role) ? in->image_role[i] : 0;
		if (role == XRT_CONV_IMG_COLOR && color == NULL) {
			color = &in->images[i];
		} else if (role == XRT_CONV_IMG_DEPTH_IN && depth_in == NULL) {
			depth_in = &in->images[i];
		}
	}
	struct cpu_view src;
	if (color == NULL || !read_image(sc, color, 0, &src)) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	const uint32_t w = src.width, h = src.height;

	const struct xrt_conv_image *written[3] = {NULL, NULL, NULL};
	uint32_t written_count = 0;
	uint32_t written_bits = 0;
	xrt_result_t r = XRT_SUCCESS;

	const bool want_views = COVERS(out, struct xrt_conv_frame_out, views) && out->views.memory_kind != 0;
	const bool want_depth = COVERS(out, struct xrt_conv_frame_out, depth) && out->depth.memory_kind != 0;
	const bool want_disp = COVERS(out, struct xrt_conv_frame_out, disparity) && out->disparity.memory_kind != 0;

	switch (cap_id) {
	case XRT_CONV_TASK_DEPTH_MONO:
	case XRT_CONV_TASK_DEPTH_VIDEO: {
		const bool video = cap_id == XRT_CONV_TASK_DEPTH_VIDEO;
		const float factor = video ? 1.0f + 0.1f * sinf((float)phase) : 1.0f;
		if (!want_depth || !target_supported(&out->depth)) {
			r = XRT_ERROR_OUTPUT_UNSUPPORTED;
			break;
		}
		uint32_t stride;
		uint8_t *dst = target_dst(sc, &out->depth, 1, 4, 1, &stride);
		if (dst == NULL) {
			r = XRT_ERROR_ALLOCATION;
			break;
		}
		sim_conv_synth_depth_ramp((float *)dst, stride, out->depth.width, out->depth.height, factor, 0.0f);
		if (!target_commit(sc, &out->depth, dst, stride, 1)) {
			r = XRT_ERROR_OUTPUT_REQUEST_FAILURE;
			break;
		}
		written[written_count++] = &out->depth;
		written_bits |= XRT_CONV_OUT_DEPTH;
		if (COVERS(out, struct xrt_conv_frame_out, depth_info)) {
			depth_info_ramp(&out->depth_info, w, h, factor, video);
			out->depth_info.source_time_ns = source_time_ns;
			out->depth_info.provider_frame_id = frame;
		}
		break;
	}
	case XRT_CONV_TASK_DISPARITY_STEREO: {
		const float value = stereo.infinity_shift_px + 4.0f;
		if (!want_disp || !target_supported(&out->disparity)) {
			r = XRT_ERROR_OUTPUT_UNSUPPORTED;
			break;
		}
		uint32_t stride;
		uint8_t *dst = target_dst(sc, &out->disparity, 1, 4, 1, &stride);
		if (dst == NULL) {
			r = XRT_ERROR_ALLOCATION;
			break;
		}
		sim_conv_synth_fill_r32f((float *)dst, stride, out->disparity.width, out->disparity.height, value);
		if (!target_commit(sc, &out->disparity, dst, stride, 1)) {
			r = XRT_ERROR_OUTPUT_REQUEST_FAILURE;
			break;
		}
		written[written_count++] = &out->disparity;
		written_bits |= XRT_CONV_OUT_DISPARITY;
		if (COVERS(out, struct xrt_conv_frame_out, depth_info)) {
			depth_info_disparity(&out->depth_info, out->disparity.width, out->disparity.height, value);
			out->depth_info.source_time_ns = source_time_ns;
			out->depth_info.provider_frame_id = frame;
		}
		break;
	}
	case XRT_CONV_TASK_VIEWS_MONO:
	case XRT_CONV_TASK_VIEWS_FROM_DEPTH: {
		const uint32_t n = info.view_count;
		const bool row = info.views_layout == XRT_CONV_VIEWS_ROW;
		if (!want_views || !target_supported(&out->views) || !is_color4(src.format) ||
		    !is_color4(out->views.format)) {
			r = XRT_ERROR_OUTPUT_UNSUPPORTED;
			break;
		}
		struct cpu_view dep = {0};
		const float *dep_ptr = NULL;
		uint32_t dep_stride = 0;
		if (cap_id == XRT_CONV_TASK_VIEWS_FROM_DEPTH && depth_in != NULL &&
		    depth_in->format == XRT_CONV_FMT_R32_FLOAT && read_image(sc, depth_in, 1, &dep) && dep.width == w &&
		    dep.height == h) {
			dep_ptr = (const float *)dep.ptr;
			dep_stride = dep.stride;
		}
		float shifts[SIM_CONV_SYNTH_MAX_VIEWS];
		sim_conv_synth_view_shifts(n, strength, vp, vp_count, rect_w, shifts);
		const uint32_t layers = row ? 1 : n;
		uint32_t stride;
		uint8_t *dst = target_dst(sc, &out->views, 0, 4, layers, &stride);
		if (dst == NULL) {
			r = XRT_ERROR_ALLOCATION;
			break;
		}
		const uint32_t vw = row ? out->views.width / n : out->views.width;
		const uint32_t vh = out->views.height;
		if (vw != w || vh != h) {
			r = XRT_ERROR_OUTPUT_UNSUPPORTED; // targets are sized by stream_get_output_desc
			break;
		}
		const bool swap = is_bgra(src.format) != is_bgra(out->views.format);
		for (uint32_t v = 0; v < n; v++) {
			uint8_t *vd = row ? dst + (size_t)v * w * 4u : dst + (size_t)v * stride * h;
			sim_conv_synth_view_rgba(src.ptr, src.stride, w, h, dep_ptr, dep_stride, shifts[v], swap, vd,
			                         stride);
		}
		if (!target_commit(sc, &out->views, dst, stride, layers)) {
			r = XRT_ERROR_OUTPUT_REQUEST_FAILURE;
			break;
		}
		written[written_count++] = &out->views;
		written_bits |= XRT_CONV_OUT_VIEWS;
		if (COVERS(out, struct xrt_conv_frame_out, views_info)) {
			struct xrt_conv_views_info *vi = &out->views_info;
			memset(vi, 0, sizeof(*vi));
			vi->view_count = n;
			vi->layout = info.views_layout;
			for (uint32_t v = 0; v < n; v++) {
				// The viewpoints actually rendered: the given ones, else the
				// fixed fan's equivalent on a nominal 0.3 m rect.
				if (vp != NULL && vp_count >= n) {
					vi->eye_xyz[v][0] = vp[v * 3 + 0];
					vi->eye_xyz[v][1] = vp[v * 3 + 1];
					vi->eye_xyz[v][2] = vp[v * 3 + 2];
				} else {
					const float rw = rect_w > 0.0f ? rect_w : 0.3f;
					vi->eye_xyz[v][0] = shifts[v] * rw / SIM_CONV_SYNTH_PARALLAX_GAIN /
					                    (strength > 0.0f ? strength : 1.0f);
				}
			}
			vi->baseline_m = baseline;
			vi->zero_parallax_disparity = 0.0f;
		}

		// Aux depth of the SAME inference (VIEWS_MONO): metric ramp 1 m (top) .. 2 m.
		if (cap_id == XRT_CONV_TASK_VIEWS_MONO && want_depth && target_supported(&out->depth)) {
			uint32_t dstride;
			uint8_t *dd = target_dst(sc, &out->depth, 1, 4, 1, &dstride);
			if (dd != NULL) {
				sim_conv_synth_depth_ramp((float *)dd, dstride, out->depth.width, out->depth.height,
				                          1.0f, 0.0f);
				if (target_commit(sc, &out->depth, dd, dstride, 1)) {
					written[written_count++] = &out->depth;
					written_bits |= XRT_CONV_OUT_DEPTH;
					if (COVERS(out, struct xrt_conv_frame_out, depth_info)) {
						struct xrt_conv_depth_info *di = &out->depth_info;
						memset(di, 0, sizeof(*di));
						di->kind = XRT_CONV_DEPTH_METRIC;
						di->measure = XRT_CONV_MEASURE_PLANAR;
						di->intrinsics_source = XRT_CONV_INTRINSICS_ASSUMED;
						di->value_scale = 1.0f;
						di->value_shift = 1.0f;
						di->near_value = 1.0f;
						di->far_value = 2.0f;
						di->invalid_sample = NAN;
						di->range_min = 1.0f + 0.5f / (float)out->depth.height;
						di->range_max = 2.0f - 0.5f / (float)out->depth.height;
						const float f = 0.78f * (float)(out->depth.width > out->depth.height
						                                    ? out->depth.width
						                                    : out->depth.height);
						di->fx = f;
						di->fy = f;
						di->cx = 0.5f * (float)out->depth.width;
						di->cy = 0.5f * (float)out->depth.height;
						di->source_width = w;
						di->source_height = h;
						// The plane at zero disparity: the given one (DECODED), else the
						// mid-range.
						di->convergence_value =
						    conv_mode == XRT_CONV_CONVERGENCE_DECODED ? convergence : 1.5f;
						di->suggested_convergence = 1.5f;
						di->suggest_flags = 1u | 4u;
						di->same_inference = 1;
						di->source_time_ns = source_time_ns;
						di->provider_frame_id = frame;
					}
				}
			}
		}
		break;
	}
	default: r = XRT_ERROR_FEATURE_NOT_SUPPORTED; break;
	}

#ifdef _WIN32
	if (r == XRT_SUCCESS && sc->io != NULL && !sim_conv_d3d11_io_finish(sc->io, written, written_count)) {
		r = XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
#endif
	(void)written;
	(void)written_count;
	if (r != XRT_SUCCESS) {
		return r;
	}
	out->written = written_bits;

	// The fake is quick but not free; HANG_MS exercises the stall watchdog.
	if (sc->latency_ms > 0) {
		os_nanosleep((int64_t)sc->latency_ms * 1000 * 1000);
	}
	if (sc->hang_ms > 0) {
		os_nanosleep((int64_t)sc->hang_ms * 1000 * 1000);
	}
	return XRT_SUCCESS;
}

static void
sc_stream_destroy(struct xrt_conversion_plugin_instance *inst, uint64_t stream)
{
	struct sim_conv *sc = (struct sim_conv *)inst;
	if (sc == NULL) {
		return;
	}
	os_mutex_lock(&sc->lock);
	struct sim_stream *s = stream_find(sc, stream);
	if (s != NULL) {
		memset(s, 0, sizeof(*s));
	}
	os_mutex_unlock(&sc->lock);
#ifdef _WIN32
	sim_conv_d3d11_io_clear_cache(sc->io);
#endif
}

/*
 *
 * Job lane.
 *
 */

static bool
sink_emit(const struct xrt_conv_job_control *ctl, uint32_t role, uint32_t format, const void *bytes, uint64_t size)
{
	if (ctl == NULL || !COVERS(ctl, struct xrt_conv_job_control, sinks) || ctl->sinks == NULL) {
		return false;
	}
	for (uint32_t i = 0; i < ctl->sink_count; i++) {
		struct xrt_conv_blob_sink *k = &ctl->sinks[i];
		if (!COVERS(k, struct xrt_conv_blob_sink, write) || k->role != role || k->begin == NULL ||
		    k->write == NULL) {
			continue;
		}
		if (!k->begin(k->ctx, format, size)) {
			return false;
		}
		const uint8_t *p = (const uint8_t *)bytes;
		for (uint64_t off = 0; off < size; off += SIM_CONV_SINK_CHUNK) {
			const uint64_t n = size - off < SIM_CONV_SINK_CHUNK ? size - off : SIM_CONV_SINK_CHUNK;
			if (!k->write(k->ctx, p + off, n)) {
				return false;
			}
		}
		return true;
	}
	return false;
}

static xrt_result_t
sc_job_execute(struct xrt_conversion_plugin_instance *inst,
               uint32_t cap_id,
               const struct xrt_conv_job_in *in,
               const struct xrt_conv_params *params,
               const struct xrt_conv_job_control *ctl,
               struct xrt_conv_job_out *out)
{
	struct sim_conv *sc = (struct sim_conv *)inst;
	if (sc == NULL || in == NULL || ctl == NULL || !COVERS(in, struct xrt_conv_job_in, images)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	bool (*is_cancelled)(void *) =
	    COVERS(ctl, struct xrt_conv_job_control, is_cancelled) && ctl->is_cancelled != NULL ? ctl->is_cancelled
	                                                                                        : sc_never_cancelled;
	void (*progress)(void *, float, const char *) =
	    COVERS(ctl, struct xrt_conv_job_control, progress) ? ctl->progress : NULL;
	void *ctx = COVERS(ctl, struct xrt_conv_job_control, ctx) ? ctl->ctx : NULL;

	struct xrt_conv_capability cap;
	memset(&cap, 0, sizeof(cap));
	cap.struct_size = (uint32_t)sizeof(cap);
	if (cap_id == 0 || cap_id > SIM_CONV_CAP_COUNT) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	fill_cap(sc, cap_id, &cap);
	if ((cap.lanes & XRT_CONV_LANE_JOB) == 0) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	const uint32_t requested =
	    COVERS(in, struct xrt_conv_job_in, requested_blob_format) ? in->requested_blob_format : 0;
	const bool splats = cap_id == XRT_CONV_TASK_SPLATS_MONO || cap_id == XRT_CONV_TASK_SPLATS_STEREO;
	if (splats && requested != 0 && requested != XRT_CONV_BLOB_PLY_3DGS) {
		return XRT_ERROR_OUTPUT_UNSUPPORTED;
	}
	if (!splats && requested != 0 && requested != XRT_CONV_BLOB_RAW_IMAGE) {
		return XRT_ERROR_OUTPUT_UNSUPPORTED;
	}

	os_mutex_lock(&sc->lock);
	uint32_t notify_cap = UINT32_MAX;
	const bool go = cap_ready(sc, cap_id) && work_tick(sc, cap_id, &notify_cap);
	os_mutex_unlock(&sc->lock);
	if (notify_cap != UINT32_MAX) {
		notify(notify_cap);
	}
	if (!go) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}

	uint32_t frame_flags, conv_mode, vp_count;
	uint64_t source_time_ns;
	float strength, rect_w, baseline, convergence;
	const float *vp;
	struct xrt_conv_stereo_calibration stereo;
	struct xrt_conv_params empty;
	memset(&empty, 0, sizeof(empty));
	read_params(params != NULL ? params : &empty, &frame_flags, &source_time_ns, &strength, &vp, &vp_count, &rect_w,
	            &baseline, &conv_mode, &convergence, &stereo);
	(void)frame_flags;
	(void)vp;
	(void)vp_count;
	(void)strength;
	(void)rect_w;
	(void)baseline;
	(void)conv_mode;
	(void)convergence;

	const struct xrt_conv_image *color = NULL;
	for (uint32_t i = 0; i < in->image_count && i < XRT_CONV_MAX_FRAME_IMAGES; i++) {
		const uint32_t role = COVERS(in, struct xrt_conv_job_in, image_role) ? in->image_role[i] : 0;
		if (role == XRT_CONV_IMG_COLOR) {
			color = &in->images[i];
			break;
		}
	}
	struct cpu_view src;
	if (color == NULL || !read_image(sc, color, 0, &src)) {
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}

	// "Inference": JOB_MS of progress, cancel polled every ≤ 50 ms.
	const int64_t total_ns = (int64_t)sc->job_ms * 1000 * 1000;
	const int64_t t0 = os_monotonic_get_ns();
	for (;;) {
		if (is_cancelled(ctx)) {
			return XRT_TIMEOUT;
		}
		const int64_t t = os_monotonic_get_ns() - t0;
		const float f = total_ns > 0 ? (float)((double)(t < total_ns ? t : total_ns) / (double)total_ns) : 1.0f;
		if (progress != NULL) {
			progress(ctx, f, "sim");
		}
		if (sc->job_fail_at >= 0.0f && f >= sc->job_fail_at) {
			U_LOG_W("sim conversion: %s job failing at %.2f (JOB_FAIL_AT)", task_name(cap_id), f);
			return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
		}
		if (t >= total_ns) {
			break;
		}
		const int64_t left = total_ns - t;
		os_nanosleep(left < SIM_CONV_POLL_NS ? left : SIM_CONV_POLL_NS);
	}

	const bool has_out = out != NULL && COVERS(out, struct xrt_conv_job_out, written);
	const uint32_t w = src.width, h = src.height;
	if (splats) {
		// The PLY colours its front layer from RGBA bytes.
		const uint8_t *rgba = src.ptr;
		uint32_t pitch = src.stride;
		if (is_bgra(src.format)) {
			uint8_t *tmp = scratch_get(sc, 2, (size_t)w * h * 4u);
			if (tmp == NULL) {
				return XRT_ERROR_ALLOCATION;
			}
			sim_conv_synth_view_rgba(src.ptr, src.stride, w, h, NULL, 0, 0.0f, true, tmp, w * 4u);
			rgba = tmp;
			pitch = w * 4u;
		}
		const size_t need = sim_fake_ply_write(NULL, 0, NULL, w, h, 0);
		uint8_t *ply = scratch_get(sc, 0, need);
		if (ply == NULL) {
			return XRT_ERROR_ALLOCATION;
		}
		sim_fake_ply_write(ply, need, rgba, w, h, pitch);
		if (!sink_emit(ctl, XRT_CONV_OUT_SPLAT_BLOB, XRT_CONV_BLOB_PLY_3DGS, ply, need)) {
			return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
		}
		if (has_out) {
			out->written = XRT_CONV_OUT_SPLAT_BLOB;
			if (COVERS(out, struct xrt_conv_job_out, splat_info)) {
				struct xrt_conv_splat_info *si = &out->splat_info;
				memset(si, 0, sizeof(*si));
				si->blob_format = XRT_CONV_BLOB_PLY_3DGS;
				si->coords = XRT_CONV_COORDS_RUB;
				si->color_space = XRT_CONV_CS_SRGB;
				// Metric only for a stereo job with a known baseline (never defaulted).
				si->metric =
				    cap_id == XRT_CONV_TASK_SPLATS_STEREO && stereo.baseline_m > 0.0f ? 1u : 0u;
				si->intrinsics_source = XRT_CONV_INTRINSICS_ASSUMED;
				const float f = 0.78f * (float)(w > h ? w : h);
				si->fx = f;
				si->fy = f;
				si->cx = 0.5f * (float)w;
				si->cy = 0.5f * (float)h;
				si->grid_width = SIM_FAKE_PLY_GRID_X;
				si->grid_height = SIM_FAKE_PLY_GRID_Y;
				si->baseline_m = cap_id == XRT_CONV_TASK_SPLATS_STEREO ? stereo.baseline_m : 0.0f;
				si->splat_count = SIM_FAKE_PLY_SPLATS;
			}
		}
		return XRT_SUCCESS;
	}

	// DEPTH_MONO / DISPARITY_STEREO: one R32F RAW_IMAGE, w × h, tightly packed.
	float *img = (float *)scratch_get(sc, 0, (size_t)w * h * 4u);
	if (img == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	struct xrt_conv_depth_info di;
	uint32_t role;
	if (cap_id == XRT_CONV_TASK_DEPTH_MONO) {
		sim_conv_synth_depth_ramp(img, w * 4u, w, h, 1.0f, 0.0f);
		depth_info_ramp(&di, w, h, 1.0f, false);
		role = XRT_CONV_OUT_DEPTH;
	} else {
		const float value = stereo.infinity_shift_px + 4.0f;
		sim_conv_synth_fill_r32f(img, w * 4u, w, h, value);
		depth_info_disparity(&di, w, h, value);
		role = XRT_CONV_OUT_DISPARITY;
	}
	di.source_time_ns = source_time_ns;
	if (!sink_emit(ctl, role, XRT_CONV_BLOB_RAW_IMAGE, img, (uint64_t)w * h * 4u)) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE;
	}
	if (has_out) {
		out->written = role;
		if (COVERS(out, struct xrt_conv_job_out, depth_info)) {
			out->depth_info = di;
		}
	}
	return XRT_SUCCESS;
}

static void
sc_destroy(struct xrt_conversion_plugin_instance *inst)
{
	struct sim_conv *sc = (struct sim_conv *)inst;
	if (sc == NULL) {
		return;
	}
#ifdef _WIN32
	sim_conv_d3d11_io_destroy(sc->io);
#endif
	for (uint32_t i = 0; i < 3; i++) {
		free(sc->scratch[i].ptr);
	}
	os_mutex_destroy(&sc->lock);
	free(sc);
}

/*
 *
 * Negotiate.
 *
 */

/*
 * The one options contract (ADR-054 decision 7, N3c): what this provider
 * reads through host->get_setting, for `displayxr-cli conv options
 * sim-conversion`. Same table as README.md § Settings; STATE_<cap> is
 * per-capability and described under SCRIPT.
 */
static const struct xrt_component_setting_desc g_sim_conv_settings[] = {
    {"ENABLE", XRT_COMPONENT_SETTING_TYPE_BOOL, "0", "probe() declines unless 1 (the fallback provider is opt-in)"},
    {"WARM_MS", XRT_COMPONENT_SETTING_TYPE_INT, "0", "every capability: WARMING for this long on its first warm"},
    {"DOWNLOAD_MS", XRT_COMPONENT_SETTING_TYPE_INT, "0",
     "every capability: DOWNLOADING for this long first (host->model_request(\"sim/fake\"))"},
    {"LATENCY_MS", XRT_COMPONENT_SETTING_TYPE_INT, "8", "sleep per stream_convert"},
    {"JOB_MS", XRT_COMPONENT_SETTING_TYPE_INT, "2000",
     "job_execute duration, with progress; cancel polled every 50 ms"},
    {"JOB_FAIL_AT", XRT_COMPONENT_SETTING_TYPE_FLOAT, "", "a job fails once its progress reaches this fraction (0..1)"},
    {"HANG_MS", XRT_COMPONENT_SETTING_TYPE_INT, "0", "extra sleep per stream_convert (the stall watchdog)"},
    {"CRASH_AFTER", XRT_COMPONENT_SETTING_TYPE_INT, "0",
     "after N converts / jobs: satellite exits (70), in-service reports FAILED(CRASH_LOOP)"},
    {"DEVICE_LOST_EVERY", XRT_COMPONENT_SETTING_TYPE_INT, "0",
     "every Nth convert / job fails with FAILED(DEVICE_LOST), retryable"},
    {"LICENCE", XRT_COMPONENT_SETTING_TYPE_ENUM, "",
     "research = RESEARCH_ONLY, commercial_ok 0 (AllowResearchOnly gate)"},
    {"SCRATCH_TOUCH", XRT_COMPONENT_SETTING_TYPE_BOOL, "0",
     "probe() writes sim-scratch-touch into host->get_path(SCRATCH) (tests)"},
    {"SCRIPT", XRT_COMPONENT_SETTING_TYPE_STRING, "",
     "per-task script (README.md); applied last. STATE_<cap> = absent|incompatible|unlicensed|disabled|"
     "failed:<reason>|installed also works per capability"},
};

static uint32_t
sc_describe_settings(const struct xrt_component_setting_desc **out_descs)
{
	if (out_descs != NULL) {
		*out_descs = g_sim_conv_settings;
	}
	return (uint32_t)(sizeof(g_sim_conv_settings) / sizeof(g_sim_conv_settings[0]));
}

static struct xrt_conversion_plugin_iface g_iface = {
    .struct_size = (uint32_t)sizeof(struct xrt_conversion_plugin_iface),
    .id = SIM_CONV_ID,
    .display_name = "DisplayXR Sim Conversion",
    .vendor = "DisplayXR",
    .version = "1",
    .get_provider_status = sc_get_provider_status,
    .probe = sc_probe,
    .describe_cap = sc_describe_cap,
    .get_generation = sc_get_generation,
    .get_state = sc_get_state,
    .warm = sc_warm,
    .evict = sc_evict,
    .stream_create = sc_stream_create,
    .stream_get_output_desc = sc_stream_get_output_desc,
    .stream_convert = sc_stream_convert,
    .stream_destroy = sc_stream_destroy,
    .job_execute = sc_job_execute,
    .destroy = sc_destroy,
    .describe_settings = sc_describe_settings,
};

/*!
 * The plug-in's only export. Tests link this TU statically (the
 * sim_conversion_static object library) and call it directly — no dlopen,
 * no registry.
 */
XRT_CONVERSION_PLUGIN_EXPORT xrt_result_t
xrtConversionPluginNegotiate(uint32_t runtime_api_version,
                             const struct xrt_conversion_plugin_host_iface *host,
                             struct xrt_conversion_plugin_iface **out_iface,
                             uint32_t *out_plugin_api_version)
{
	if (out_iface == NULL || out_plugin_api_version == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	*out_plugin_api_version = XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT;
	if (runtime_api_version != XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT) {
		// The loader rejects the major mismatch itself; answer anyway.
		*out_iface = NULL;
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	g_host = host;
	*out_iface = &g_iface;
	return XRT_SUCCESS;
}
