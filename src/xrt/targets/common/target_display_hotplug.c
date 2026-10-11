// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Display hotplug — see target_display_hotplug.h (ADR-047 Amendment 7).
 * @ingroup targets_common
 */

#include "target_display_hotplug.h"

#include "os/os_display_hotplug.h"
#include "os/os_threading.h"
#include "os/os_time.h"
#include "util/u_logging.h"
#include "util/u_misc.h"

#include "xrt/xrt_config_os.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
 *
 * Scheduler.
 *
 */

#define MS(x) ((uint64_t)(x)*1000ull * 1000ull)

void
target_hotplug_sched_init(struct target_hotplug_sched *s, uint64_t settle_ns, uint64_t max_delay_ns)
{
	if (s == NULL) {
		return;
	}
	memset(s, 0, sizeof(*s));
	s->settle_ns = settle_ns != 0 ? settle_ns : TARGET_HOTPLUG_SETTLE_NS;
	s->max_delay_ns = max_delay_ns != 0 ? max_delay_ns : TARGET_HOTPLUG_MAX_DELAY_NS;
	if (s->max_delay_ns < s->settle_ns) {
		s->max_delay_ns = s->settle_ns;
	}
	static const uint32_t offs_ms[TARGET_HOTPLUG_FOLLOWUPS] = {500,  1000, 1500,  2000,  3000,  4000,
	                                                           6000, 8000, 12000, 16000, 24000, 32000};
	for (uint32_t i = 0; i < TARGET_HOTPLUG_FOLLOWUPS; i++) {
		s->followup_ns[i] = MS(offs_ms[i]);
	}
	s->followup_count = TARGET_HOTPLUG_FOLLOWUPS;
}

void
target_hotplug_sched_event(struct target_hotplug_sched *s, uint64_t now_ns, enum target_hotplug_source src)
{
	if (s == NULL) {
		return;
	}
	if (!s->pending) {
		s->pending = true;
		s->first_ns = now_ns;
		s->sources = 0;
		s->want_followups = false;
	}
	s->last_ns = now_ns;
	s->sources |= (uint32_t)src;
	if (src != TARGET_HOTPLUG_SOURCE_PLUGIN) {
		s->want_followups = true;
	}
}

uint64_t
target_hotplug_sched_deadline(const struct target_hotplug_sched *s)
{
	if (s == NULL) {
		return TARGET_HOTPLUG_NEVER;
	}
	uint64_t d = TARGET_HOTPLUG_NEVER;
	if (s->pending) {
		const uint64_t quiet = s->last_ns + s->settle_ns;
		const uint64_t cap = s->first_ns + s->max_delay_ns;
		d = quiet < cap ? quiet : cap;
	}
	if (s->followups_armed && s->followup_next < s->followup_count) {
		const uint64_t f = s->followup_base_ns + s->followup_ns[s->followup_next];
		if (f < d) {
			d = f;
		}
	}
	return d;
}

bool
target_hotplug_sched_take(struct target_hotplug_sched *s, uint64_t now_ns, bool *out_followup, uint32_t *out_sources)
{
	if (out_followup != NULL) {
		*out_followup = false;
	}
	if (out_sources != NULL) {
		*out_sources = 0;
	}
	if (s == NULL) {
		return false;
	}
	if (s->pending) {
		const uint64_t quiet = s->last_ns + s->settle_ns;
		const uint64_t cap = s->first_ns + s->max_delay_ns;
		if (now_ns >= quiet || now_ns >= cap) {
			if (out_sources != NULL) {
				*out_sources = s->sources;
			}
			s->pending = false;
			if (s->want_followups) {
				// (Re)arm from this run: a new OS event restarts the window.
				s->followups_armed = true;
				s->followup_base_ns = now_ns;
				s->followup_next = 0;
			}
			s->want_followups = false;
			s->sources = 0;
			return true;
		}
	}
	if (s->followups_armed && s->followup_next < s->followup_count &&
	    now_ns >= s->followup_base_ns + s->followup_ns[s->followup_next]) {
		// Skip every offset already passed (a late wake-up runs once, not N times).
		while (s->followup_next < s->followup_count &&
		       now_ns >= s->followup_base_ns + s->followup_ns[s->followup_next]) {
			s->followup_next++;
		}
		if (s->followup_next >= s->followup_count) {
			s->followups_armed = false;
		}
		if (out_followup != NULL) {
			*out_followup = true;
		}
		return true;
	}
	return false;
}


/*
 *
 * Screen-list comparison.
 *
 */

static bool
geometry_equal(const struct xrt_screen *a, const struct xrt_screen *b)
{
	return a->desktop_left == b->desktop_left && a->desktop_top == b->desktop_top &&
	       a->desktop_width == b->desktop_width && a->desktop_height == b->desktop_height &&
	       a->native_width == b->native_width && a->native_height == b->native_height &&
	       a->desktop_scale == b->desktop_scale;
}

static uint32_t
list_count(const struct xrt_screen_list *l)
{
	if (l == NULL) {
		return 0;
	}
	return l->count < XRT_SCREEN_LIST_MAX ? l->count : XRT_SCREEN_LIST_MAX;
}

static uint64_t
default_id(const struct xrt_screen_list *l)
{
	for (uint32_t i = 0; i < list_count(l); i++) {
		if ((l->screens[i].flags & XRT_SCREEN_FLAG_SYSTEM_DEFAULT) != 0) {
			return l->screens[i].id;
		}
	}
	return 0;
}

bool
target_screens_compare(const struct xrt_screen_list *before,
                       const struct xrt_screen_list *after,
                       struct target_screens_change *out)
{
	struct target_screens_change c;
	memset(&c, 0, sizeof(c));
	const uint32_t nb = list_count(before);
	const uint32_t na = list_count(after);

	for (uint32_t i = 0; i < na; i++) {
		const struct xrt_screen *a = &after->screens[i];
		const struct xrt_screen *b = xrt_screen_list_find(before, a->id);
		if (b == NULL) {
			c.added++;
			continue;
		}
		if (strcmp(a->plugin_id, b->plugin_id) != 0) {
			c.claimant_changed++;
		} else if (!geometry_equal(a, b)) {
			c.geometry_changed++;
		} else if (memcmp(a, b, sizeof(*a)) != 0) {
			c.other_changed++;
		}
	}
	for (uint32_t i = 0; i < nb; i++) {
		if (xrt_screen_list_find(after, before->screens[i].id) == NULL) {
			c.removed++;
		}
	}
	c.default_moved = default_id(before) != default_id(after);

	const bool order_or_any =
	    nb != na || (nb > 0 && memcmp(before->screens, after->screens, sizeof(before->screens[0]) * nb) != 0);
	if (out != NULL) {
		*out = c;
	}
	return c.added != 0 || c.removed != 0 || c.claimant_changed != 0 || c.geometry_changed != 0 ||
	       c.other_changed != 0 || c.default_moved || order_or_any;
}

#define APPEND(...)                                                                                                    \
	do {                                                                                                           \
		if (used < size) {                                                                                     \
			int w_ = snprintf(buf + used, size - used, __VA_ARGS__);                                       \
			if (w_ > 0) {                                                                                  \
				used += (size_t)w_ < size - used ? (size_t)w_ : size - used - 1;                       \
			}                                                                                              \
		}                                                                                                      \
	} while (false)

void
target_screens_change_format(const struct xrt_screen_list *before,
                             const struct xrt_screen_list *after,
                             char *buf,
                             size_t size)
{
	if (buf == NULL || size == 0) {
		return;
	}
	buf[0] = '\0';
	size_t used = 0;
	for (uint32_t i = 0; i < list_count(after); i++) {
		const struct xrt_screen *a = &after->screens[i];
		const struct xrt_screen *b = xrt_screen_list_find(before, a->id);
		if (b == NULL) {
			APPEND("%s+0x%016llx '%s' %d,%d %ux%u by '%s'", used > 0 ? "; " : "", (unsigned long long)a->id,
			       a->device_name, a->desktop_left, a->desktop_top, a->desktop_width, a->desktop_height,
			       a->plugin_id);
		} else if (strcmp(a->plugin_id, b->plugin_id) != 0) {
			APPEND("%s0x%016llx '%s' claimant '%s' -> '%s'", used > 0 ? "; " : "",
			       (unsigned long long)a->id, a->device_name, b->plugin_id, a->plugin_id);
		} else if (!geometry_equal(a, b)) {
			APPEND("%s0x%016llx '%s' %d,%d %ux%u -> %d,%d %ux%u", used > 0 ? "; " : "",
			       (unsigned long long)a->id, a->device_name, b->desktop_left, b->desktop_top,
			       b->desktop_width, b->desktop_height, a->desktop_left, a->desktop_top, a->desktop_width,
			       a->desktop_height);
		}
	}
	for (uint32_t i = 0; i < list_count(before); i++) {
		const struct xrt_screen *b = &before->screens[i];
		if (xrt_screen_list_find(after, b->id) == NULL) {
			APPEND("%s-0x%016llx '%s' (was '%s')", used > 0 ? "; " : "", (unsigned long long)b->id,
			       b->device_name, b->plugin_id);
		}
	}
	const uint64_t db = default_id(before);
	const uint64_t da = default_id(after);
	if (db != da) {
		APPEND("%ssystem default 0x%016llx -> 0x%016llx", used > 0 ? "; " : "", (unsigned long long)db,
		       (unsigned long long)da);
	}
	if (used == 0) {
		APPEND("no screen added, removed, re-claimed or moved");
	}
}


/*
 *
 * Topology signature.
 *
 */

static uint64_t
fnv_bytes(uint64_t h, const void *p, size_t n)
{
	const uint8_t *b = (const uint8_t *)p;
	for (size_t i = 0; i < n; i++) {
		h ^= (uint64_t)b[i];
		h *= 1099511628211ULL;
	}
	return h;
}

uint64_t
target_display_topology_hash(const struct os_display_desktop_info *mons, uint32_t count)
{
	uint64_t h = 1469598103934665603ULL;
	h = fnv_bytes(h, &count, sizeof(count));
	for (uint32_t i = 0; mons != NULL && i < count; i++) {
		const struct os_display_desktop_info *m = &mons[i];
		const int32_t geo[2] = {m->left, m->top};
		const uint32_t dims[5] = {m->width, m->height, m->native_width, m->native_height,
		                          m->is_primary ? 1u : 0u};
		const int32_t scale_milli = (int32_t)(m->scale * 1000.0 + 0.5);
		h = fnv_bytes(h, geo, sizeof(geo));
		h = fnv_bytes(h, dims, sizeof(dims));
		h = fnv_bytes(h, &scale_milli, sizeof(scale_milli));
		h = fnv_bytes(h, m->device_name, strnlen(m->device_name, sizeof(m->device_name)));
	}
	return h;
}

uint64_t
target_display_topology_signature(void)
{
	struct os_display_desktop_info mons[OS_DISPLAY_DESKTOP_MAX_MONITORS];
	memset(mons, 0, sizeof(mons));
	const uint32_t n = os_display_desktop_enumerate(mons, OS_DISPLAY_DESKTOP_MAX_MONITORS);
	return target_display_topology_hash(mons, n);
}


/*
 *
 * The monitor.
 *
 */

struct target_display_hotplug
{
	target_display_hotplug_rebuild_fn fn;
	void *user;

	struct os_mutex mutex;
	struct os_cond cond;
	struct os_thread thread;
	bool stop;

	struct target_hotplug_sched sched;
	uint64_t poll_period_ns; //!< 0 = no poll
	uint64_t next_poll_ns;
	uint64_t last_sig;
	bool have_sig;
	bool os_subscribed;

	//! Last reason text a request carried (logged with the run).
	char reason[96];
};

//! The running monitor; requests from plug-ins and the OS reach it here.
static pthread_mutex_t g_hp_lock = PTHREAD_MUTEX_INITIALIZER;
static struct target_display_hotplug *g_hp = NULL;
//! A request that arrived while no monitor ran (served by the next start).
static uint32_t g_latched_sources = 0;
static char g_latched_reason[96];

static const char *
sources_str(uint32_t s, char *buf, size_t size)
{
	buf[0] = '\0';
	size_t used = 0;
	if (s & TARGET_HOTPLUG_SOURCE_OS) {
		APPEND("%sOS", used > 0 ? "+" : "");
	}
	if (s & TARGET_HOTPLUG_SOURCE_POLL) {
		APPEND("%spoll", used > 0 ? "+" : "");
	}
	if (s & TARGET_HOTPLUG_SOURCE_PLUGIN) {
		APPEND("%splug-in", used > 0 ? "+" : "");
	}
	if (s & TARGET_HOTPLUG_SOURCE_TEST) {
		APPEND("%stest", used > 0 ? "+" : "");
	}
	if (used == 0) {
		APPEND("follow-up");
	}
	return buf;
}

//! Caller holds hp->mutex.
static void
event_locked(struct target_display_hotplug *hp, enum target_hotplug_source src, const char *reason)
{
	target_hotplug_sched_event(&hp->sched, os_monotonic_get_ns(), src);
	if (reason != NULL && reason[0] != '\0') {
		(void)snprintf(hp->reason, sizeof(hp->reason), "%s", reason);
	}
	os_cond_signal(&hp->cond);
}

void
target_display_hotplug_request(enum target_hotplug_source src, const char *reason)
{
	pthread_mutex_lock(&g_hp_lock);
	struct target_display_hotplug *hp = g_hp;
	if (hp != NULL) {
		os_mutex_lock(&hp->mutex);
		event_locked(hp, src, reason);
		os_mutex_unlock(&hp->mutex);
	} else {
		g_latched_sources |= (uint32_t)src;
		(void)snprintf(g_latched_reason, sizeof(g_latched_reason), "%s", reason != NULL ? reason : "");
	}
	pthread_mutex_unlock(&g_hp_lock);
	if (hp == NULL) {
		U_LOG_I("display hotplug: request (%s) latched — no monitor running yet; the next one serves it",
		        reason != NULL ? reason : "-");
	}
}

void
target_display_hotplug_plugin_request(const char *reason)
{
	target_display_hotplug_request(TARGET_HOTPLUG_SOURCE_PLUGIN, reason != NULL ? reason : "plug-in");
}

static void
on_os_reconfigured(void *user)
{
	(void)user;
	target_display_hotplug_request(TARGET_HOTPLUG_SOURCE_OS, "OS display reconfiguration");
}

static void *
hotplug_thread(void *ptr)
{
	struct target_display_hotplug *hp = (struct target_display_hotplug *)ptr;
	os_thread_name(&hp->thread, "dxr-display-hotplug");

	os_mutex_lock(&hp->mutex);
	while (!hp->stop) {
		uint64_t now = os_monotonic_get_ns();

		// The topology poll: cheap (the OS monitor list), off the lock.
		if (hp->poll_period_ns != 0 && now >= hp->next_poll_ns) {
			hp->next_poll_ns = now + hp->poll_period_ns;
			os_mutex_unlock(&hp->mutex);
			const uint64_t sig = target_display_topology_signature();
			os_mutex_lock(&hp->mutex);
			if (hp->have_sig && sig != hp->last_sig) {
				event_locked(hp, TARGET_HOTPLUG_SOURCE_POLL, "monitor list changed");
			}
			hp->last_sig = sig;
			hp->have_sig = true;
			now = os_monotonic_get_ns();
		}

		bool followup = false;
		uint32_t sources = 0;
		if (target_hotplug_sched_take(&hp->sched, now, &followup, &sources)) {
			char why[96];
			(void)snprintf(why, sizeof(why), "%s", hp->reason);
			if (!followup) {
				hp->reason[0] = '\0';
			}
			os_mutex_unlock(&hp->mutex);

			char src_buf[48];
			const uint64_t t0 = os_monotonic_get_ns();
			const bool changed = hp->fn(hp->user, sources, followup);
			const uint64_t dt_ms = (os_monotonic_get_ns() - t0) / (1000ull * 1000ull);
			if (changed || !followup) {
				U_LOG_W("display hotplug: claim sweep (%s%s%s) took %llu ms — %s",
				        sources_str(sources, src_buf, sizeof(src_buf)),
				        why[0] != '\0' && !followup ? ": " : "", !followup ? why : "",
				        (unsigned long long)dt_ms,
				        changed ? "screen table CHANGED, published" : "no change");
			} else {
				U_LOG_I("display hotplug: follow-up claim sweep took %llu ms — no change",
				        (unsigned long long)dt_ms);
			}

			// The sweep saw the topology as of now: re-base the poll on it, so
			// the change that triggered an OS-notified sweep is not seen a
			// second time by the next poll.
			const uint64_t sig = hp->poll_period_ns != 0 ? target_display_topology_signature() : 0;
			os_mutex_lock(&hp->mutex);
			if (hp->poll_period_ns != 0) {
				hp->last_sig = sig;
				hp->have_sig = true;
			}
			continue;
		}

		uint64_t deadline = target_hotplug_sched_deadline(&hp->sched);
		if (hp->poll_period_ns != 0 && hp->next_poll_ns < deadline) {
			deadline = hp->next_poll_ns;
		}
		uint64_t wait_ns = deadline > now ? deadline - now : 0;
		if (wait_ns > MS(1000)) {
			wait_ns = MS(1000);
		}
		if (wait_ns > 0) {
			(void)os_cond_wait_timeout_ns(&hp->cond, &hp->mutex, wait_ns);
		}
	}
	os_mutex_unlock(&hp->mutex);
	return NULL;
}

bool
target_display_hotplug_enabled(void)
{
	const char *e = getenv("DXR_DISPLAY_HOTPLUG");
	if (e != NULL && e[0] != '\0') {
		return e[0] != '0';
	}
#if defined(XRT_OS_MACOS)
	return true;
#else
	return false;
#endif
}

struct target_display_hotplug *
target_display_hotplug_start(target_display_hotplug_rebuild_fn fn, void *user)
{
	if (fn == NULL || !target_display_hotplug_enabled()) {
		return NULL;
	}
	struct target_display_hotplug *hp = U_TYPED_CALLOC(struct target_display_hotplug);
	if (hp == NULL) {
		return NULL;
	}
	hp->fn = fn;
	hp->user = user;
	target_hotplug_sched_init(&hp->sched, 0, 0);
	hp->poll_period_ns = MS(500);
	const char *poll = getenv("DXR_DISPLAY_HOTPLUG_POLL_MS");
	if (poll != NULL && poll[0] != '\0') {
		hp->poll_period_ns = MS(strtoul(poll, NULL, 10));
	}
	if (os_mutex_init(&hp->mutex) != 0) {
		free(hp);
		return NULL;
	}
	if (os_cond_init(&hp->cond) != 0) {
		os_mutex_destroy(&hp->mutex);
		free(hp);
		return NULL;
	}
	os_thread_init(&hp->thread);

	pthread_mutex_lock(&g_hp_lock);
	if (g_hp != NULL) {
		pthread_mutex_unlock(&g_hp_lock);
		U_LOG_W("display hotplug: a monitor already runs in this process — not starting a second");
		os_cond_destroy(&hp->cond);
		os_mutex_destroy(&hp->mutex);
		free(hp);
		return NULL;
	}
	g_hp = hp;
	if (g_latched_sources != 0) {
		// Served by this monitor's first sweep (one event per latched source).
		for (uint32_t bit = 1; bit <= TARGET_HOTPLUG_SOURCE_TEST; bit <<= 1) {
			if ((g_latched_sources & bit) != 0) {
				target_hotplug_sched_event(&hp->sched, os_monotonic_get_ns(),
				                           (enum target_hotplug_source)bit);
			}
		}
		(void)snprintf(hp->reason, sizeof(hp->reason), "latched: %s", g_latched_reason);
		g_latched_sources = 0;
	}
	pthread_mutex_unlock(&g_hp_lock);

	if (os_thread_start(&hp->thread, hotplug_thread, hp) != 0) {
		pthread_mutex_lock(&g_hp_lock);
		g_hp = NULL;
		pthread_mutex_unlock(&g_hp_lock);
		os_cond_destroy(&hp->cond);
		os_mutex_destroy(&hp->mutex);
		free(hp);
		return NULL;
	}
	hp->os_subscribed = os_display_hotplug_subscribe(on_os_reconfigured, NULL);
	U_LOG_W(
	    "display hotplug: watching for monitors and panels coming and going (OS notification: %s; poll: %s) — "
	    "a panel plugged in while this app runs is claimed live (ADR-047 Amendment 7)",
	    hp->os_subscribed ? "yes" : "no",
	    hp->poll_period_ns != 0 ? "every 500 ms or DXR_DISPLAY_HOTPLUG_POLL_MS" : "off");
	return hp;
}

void
target_display_hotplug_stop(struct target_display_hotplug **hp_ptr)
{
	if (hp_ptr == NULL || *hp_ptr == NULL) {
		return;
	}
	struct target_display_hotplug *hp = *hp_ptr;
	*hp_ptr = NULL;

	if (hp->os_subscribed) {
		os_display_hotplug_unsubscribe();
	}
	// No request can reach it once it is unpublished (the request path holds
	// g_hp_lock across its use of the object).
	pthread_mutex_lock(&g_hp_lock);
	if (g_hp == hp) {
		g_hp = NULL;
	}
	pthread_mutex_unlock(&g_hp_lock);

	os_mutex_lock(&hp->mutex);
	hp->stop = true;
	os_cond_signal(&hp->cond);
	os_mutex_unlock(&hp->mutex);
	os_thread_join(&hp->thread);
	os_thread_destroy(&hp->thread);

	os_cond_destroy(&hp->cond);
	os_mutex_destroy(&hp->mutex);
	free(hp);
}
