// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The conversion core: providers, hosts, readiness, selection,
 *         properties, prewarm, the control thread.
 * @ingroup aux_conversion
 */

#include "conv_core_internal.h"
#include "conv_host_inproc.h"

#include "os/os_time.h"
#include "util/u_logging.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>


/*
 *
 * Small helpers.
 *
 */

static uint64_t
now_ns(void)
{
	return os_monotonic_get_ns();
}

static struct conv_readiness_event
mk_ev(uint32_t type, uint64_t now)
{
	struct conv_readiness_event e;
	memset(&e, 0, sizeof(e));
	e.type = type;
	e.now_ns = now;
	e.progress = -1.0f;
	return e;
}

void
conv_core_emit(struct conv_core *core, uint64_t client_id, const struct xrt_conv_wire_event *ev)
{
	if (core->cfg.on_event != NULL) {
		core->cfg.on_event(core->cfg.event_ud, client_id, ev);
	}
}

static void
emit_simple(struct conv_core *core,
            uint32_t kind,
            uint32_t task,
            uint32_t state,
            uint32_t prev,
            uint32_t reason,
            float progress)
{
	struct xrt_conv_wire_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.struct_size = (uint32_t)sizeof(ev);
	ev.family_version = XRT_CONV_WIRE_FAMILY_VERSION_CURRENT;
	ev.kind = kind;
	ev.task = task;
	ev.state = state;
	ev.previous_state = prev;
	ev.failure_reason = reason;
	ev.progress = progress;
	conv_core_emit(core, 0, &ev);
}

struct conv_host *
conv_core_host(struct conv_core *core, uint32_t provider)
{
	if (provider >= CONV_REGISTRY_MAX_PROVIDERS || core->hosts[provider].hung) {
		return NULL;
	}
	return core->hosts[provider].host;
}

static int32_t
row_index_locked(struct conv_core *core, uint32_t provider, uint32_t cap_id)
{
	return conv_registry_find_row(&core->reg, provider, cap_id);
}

uint32_t
conv_core_row_state_locked(struct conv_core *core, uint32_t provider, uint32_t cap_id)
{
	const int32_t ri = row_index_locked(core, provider, cap_id);
	return ri < 0 ? XRT_CONV_STATE_ABSENT : core->reg.rows[ri].rd.state;
}

bool
conv_core_row_cap_locked(struct conv_core *core, uint32_t provider, uint32_t cap_id, struct xrt_conv_capability *out)
{
	const int32_t ri = row_index_locked(core, provider, cap_id);
	if (ri < 0) {
		return false;
	}
	*out = core->reg.rows[ri].cap;
	return true;
}

bool
conv_core_row_validated_locked(struct conv_core *core, uint32_t provider, uint32_t cap_id)
{
	if (provider >= core->reg.provider_count) {
		return false;
	}
	const char *id = core->reg.providers[provider].id;
	for (uint32_t i = 0; i < core->validated_count; i++) {
		if (core->validated[i].cap_id == cap_id && strcmp(core->validated[i].provider_id, id) == 0) {
			return true;
		}
	}
	return false;
}


/*
 *
 * Readiness + warm.
 *
 */

static void
start_warm_locked(struct conv_core *core, uint32_t provider, uint32_t cap_id);

static uint32_t
apply_locked(struct conv_core *core, uint32_t ri, const struct conv_readiness_event *ev)
{
	struct conv_cap_row *row = &core->reg.rows[ri];
	const uint32_t prev = row->rd.state;
	const uint32_t provider = row->provider;
	const uint32_t cap_id = row->cap.cap_id;
	const uint32_t r = conv_readiness_apply(&row->rd, ev);

	if ((r & CONV_RD_CHANGED) != 0) {
		core->status_gen++;
		U_LOG_W("[conv] %s cap %u (%s): %s -> %s%s%s", core->reg.providers[provider].id, cap_id,
		        conv_task_name(row->cap.task), conv_state_name(prev), conv_state_name(row->rd.state),
		        row->rd.reason != 0 ? " reason=" : "",
		        row->rd.reason != 0 ? conv_reason_name(row->rd.reason) : "");
		emit_simple(core, XRT_CONV_WIRE_EVENT_TASK_STATE_CHANGED, row->cap.task, row->rd.state, prev,
		            row->rd.reason, row->rd.progress);
		if (conv_state_is_terminal(row->rd.state)) {
			core->rebind_check = true;
		}
		if (row->rd.state == XRT_CONV_STATE_READY) {
			core->wake_pending = true;
		}
		os_cond_signal(&core->ctrl_cond);
	} else if ((r & CONV_RD_PROGRESS) != 0) {
		core->status_gen++;
	}
	if ((r & CONV_RD_ACT_REDESCRIBE) != 0 && provider < CONV_REGISTRY_MAX_PROVIDERS) {
		core->redescribe[provider] = true;
		os_cond_signal(&core->ctrl_cond);
	}
	if ((r & CONV_RD_ACT_WARM) != 0) {
		conv_core_request_warm_locked(core, provider, cap_id, CONV_EV_WARM_REQUESTED);
	}
	return r;
}

static bool
apply_by_cap_locked(struct conv_core *core, uint32_t provider, uint32_t cap_id, const struct conv_readiness_event *ev)
{
	const int32_t ri = row_index_locked(core, provider, cap_id);
	if (ri < 0) {
		return false;
	}
	apply_locked(core, (uint32_t)ri, ev);
	return true;
}

void
conv_core_request_warm_locked(struct conv_core *core, uint32_t provider, uint32_t cap_id, uint32_t ev_type)
{
	const int32_t ri = row_index_locked(core, provider, cap_id);
	if (ri < 0) {
		return;
	}
	struct conv_readiness_event ev = mk_ev(ev_type, now_ns());
	apply_locked(core, (uint32_t)ri, &ev);
	const uint32_t s = core->reg.rows[ri].rd.state;
	if (s == XRT_CONV_STATE_WARMING || s == XRT_CONV_STATE_DOWNLOADING) {
		start_warm_locked(core, provider, cap_id);
	}
}

/*! The provider's own answer is "unavailable, and retrying will not help"
 *  (ABSENT / DISABLED with retryable = 0): a terminal state, not a failure. */
static bool
declared_absent(const struct xrt_conv_state *st)
{
	return st->retryable == 0 && (st->state == XRT_CONV_STATE_ABSENT || st->state == XRT_CONV_STATE_DISABLED);
}

//! ABSENT(policy) for a provider-side DISABLED (a kill switch), else the provider's reason (model).
static uint32_t
declared_reason(const struct xrt_conv_state *st)
{
	if (st->state == XRT_CONV_STATE_DISABLED) {
		return CONV_REASON_POLICY;
	}
	return st->failure_reason != 0 ? st->failure_reason : XRT_CONV_FAIL_MODEL;
}

static bool
warm_is_cancelled(void *ctx)
{
	struct conv_warm_op *op = (struct conv_warm_op *)ctx;
	os_mutex_lock(&op->core->registry_mtx);
	const bool c = op->cancel || op->core->stop;
	os_mutex_unlock(&op->core->registry_mtx);
	return c;
}

static void
warm_done(struct conv_host_cmd *cmd)
{
	struct conv_warm_op *op = (struct conv_warm_op *)cmd->user;
	struct conv_core *core = op->core;
	struct xrt_conv_state st;
	memset(&st, 0, sizeof(st));
	st.struct_size = (uint32_t)sizeof(st);
	struct conv_host *host = core->hosts[op->provider].host;
	// A satellite host that died under the warm completes it as failed AFTER
	// bumping its epoch: that is the crash path's (WARMING(restarting), re-warm
	// on RESTORED), never a warm failure of the capability.
	// A warm refused because the host is down (CONV_CMD_HOST_DOWN) is the same:
	// RESTORED re-warms the row.
	const bool casualty = cmd->result != XRT_SUCCESS && host != NULL &&
	                      (conv_host_get_epoch(host) != op->epoch || (cmd->blocked & CONV_CMD_HOST_DOWN) != 0);
	const bool have_state =
	    !casualty && host != NULL && conv_host_get_state(host, op->cap_id, &st) == XRT_SUCCESS;

	os_mutex_lock(&core->registry_mtx);
	op->busy = false;
	const uint64_t t = now_ns();
	if (casualty) {
		U_LOG_I("[conv] %s cap %u: warm lost with its host process (restart pending)",
		        core->reg.providers[op->provider].id, op->cap_id);
		// Completed after the host came back: the RESTORED re-warm skipped
		// this busy op, so ask again.
		if (!core->hosts[op->provider].down) {
			core->hosts[op->provider].rewarm = true;
		}
	} else if (!op->cancel && !core->stop) {
		if (cmd->result == XRT_SUCCESS && (!have_state || st.state == XRT_CONV_STATE_READY)) {
			struct conv_readiness_event ev = mk_ev(CONV_EV_WARM_OK, t);
			apply_by_cap_locked(core, op->provider, op->cap_id, &ev);
		} else if (have_state && declared_absent(&st)) {
			// #2029: the provider says it is not there and a retry will not
			// change that: terminal ABSENT, not FAILED + the 15 s backoff loop
			// that kept a spec-3 caller at ACTIVATING forever. DISABLED (a kill
			// switch) is policy; ABSENT is the provider's own reason (model).
			U_LOG_I(
			    "[conv] %s cap %u: the provider declares it unavailable (%s); terminal until a registry "
			    "change or `conv warm`",
			    core->reg.providers[op->provider].id, op->cap_id, st.detail[0] != '\0' ? st.detail : "-");
			struct conv_readiness_event ev = mk_ev(CONV_EV_DECLARED_ABSENT, t);
			ev.reason = declared_reason(&st);
			apply_by_cap_locked(core, op->provider, op->cap_id, &ev);
		} else if (cmd->result != XRT_SUCCESS || st.state == XRT_CONV_STATE_FAILED ||
		           conv_state_is_terminal(st.state)) {
			struct conv_readiness_event ev = mk_ev(CONV_EV_WARM_FAILED, t);
			ev.reason = have_state && st.failure_reason != 0 ? st.failure_reason : XRT_CONV_FAIL_LOAD;
			ev.retryable = have_state ? st.retryable != 0 : true;
			apply_by_cap_locked(core, op->provider, op->cap_id, &ev);
		}
		// Else: still warming on the provider's side; the control thread polls.
	}
	op->cancel = false;
	os_cond_signal(&core->ctrl_cond);
	os_mutex_unlock(&core->registry_mtx);
}

static void
start_warm_locked(struct conv_core *core, uint32_t provider, uint32_t cap_id)
{
	if (provider >= CONV_REGISTRY_MAX_PROVIDERS) {
		return;
	}
	struct conv_host_slot *hs = &core->hosts[provider];
	if (hs->host == NULL || hs->hung || core->stop) {
		// Cache-only rows warm once their provider is loaded.
		return;
	}
	if (hs->down && hs->host->hosting != CONV_HOSTING_SATELLITE) {
		return;
	}
	// A satellite that is down refuses the warm at once (CONV_CMD_HOST_DOWN);
	// the refusal is the demand that respawns it, and RESTORED re-warms every
	// WARMING row (#2056).
	struct conv_warm_op *op = NULL;
	for (uint32_t i = 0; i < XRT_CONV_MAX_CAPABILITIES; i++) {
		if (hs->warms[i].cap_id == cap_id) {
			op = &hs->warms[i];
			break;
		}
	}
	for (uint32_t i = 0; op == NULL && i < XRT_CONV_MAX_CAPABILITIES; i++) {
		if (hs->warms[i].cap_id == 0) {
			op = &hs->warms[i];
			op->cap_id = cap_id;
		}
	}
	if (op == NULL || op->busy) {
		return;
	}
	memset(&op->cmd, 0, sizeof(op->cmd));
	op->core = core;
	op->provider = provider;
	op->busy = true;
	op->cancel = false;
	op->epoch = conv_host_get_epoch(hs->host);
	op->cmd.op = CONV_OP_WARM;
	op->cmd.cap_id = cap_id;
	op->cmd.is_cancelled = warm_is_cancelled;
	op->cmd.cancel_ctx = op;
	op->cmd.on_complete = warm_done;
	op->cmd.user = op;
	if (conv_host_submit(hs->host, &op->cmd) != XRT_SUCCESS) {
		op->busy = false;
	}
}


/*
 *
 * Satellite host lifecycle (roadmap §3.5: "crash → WARMING; streams survive").
 *
 */

//! conv_host::on_event: any host thread, no host lock held. Queue only.
static void
conv_core_on_host_event(void *ud, struct conv_host *h, uint32_t ev, int32_t detail)
{
	(void)h;
	struct conv_host_listener *l = (struct conv_host_listener *)ud;
	if (l == NULL || l->core == NULL) {
		return;
	}
	struct conv_core *core = l->core;
	os_mutex_lock(&core->evq_mtx);
	if (core->evq_count < CONV_CORE_HOST_EVQ) {
		struct conv_host_evq_entry *e = &core->evq[core->evq_count++];
		e->provider = l->provider;
		e->ev = ev;
		e->detail = detail;
	} else {
		core->evq_dropped++;
	}
	os_mutex_unlock(&core->evq_mtx);
	// Wake the control thread; it applies the queue under registry_mtx.
	os_cond_signal(&core->ctrl_cond);
}

//! Apply one lifecycle event (registry_mtx held).
static void
host_event_locked(struct conv_core *core, const struct conv_host_evq_entry *e, uint64_t t)
{
	const uint32_t p = e->provider;
	if (p >= core->reg.provider_count || core->hosts[p].host == NULL) {
		return;
	}
	struct conv_host_slot *hs = &core->hosts[p];
	const char *id = core->reg.providers[p].id;
	switch (e->ev) {
	case CONV_HOST_EV_LOST: {
		const bool crash = conv_host_exit_is_crash(e->detail);
		const bool update = conv_host_exit_is_update(e->detail);
		// After a crash the host comes back on its own (backoff) and says
		// RESTORED; after an update (exit 20) it comes back once the
		// maintenance mutex is free (or the registry row changes) and says
		// RESTORED too; an orderly exit is respawned by the next demand, so
		// the next warm must be allowed to reach it.
		hs->down = crash || update;
		hs->rewarm = false;
		hs->restarts++;
		U_LOG_W("[conv] provider %s: host process exited (code %d%s); streams stay, outputs keep their last result",
		        id, (int)e->detail, crash ? ", crash" : update ? ", closed for update" : "");
		for (uint32_t i = 0; i < core->reg.row_count; i++) {
			if (core->reg.rows[i].provider != p) {
				continue;
			}
			if (crash) {
				// WARMING(restarting, n), or FAILED(crash_loop) at the FSM's own cutoff.
				struct conv_readiness_event ev = mk_ev(CONV_EV_HOST_CRASH, t);
				apply_locked(core, i, &ev);
			} else if (update) {
				// WARMING(updating): no crash record; RESTORED re-warms it.
				struct conv_readiness_event ev = mk_ev(CONV_EV_HOST_UPDATING, t);
				apply_locked(core, i, &ev);
			} else if (core->reg.rows[i].rd.state == XRT_CONV_STATE_READY) {
				// Orderly exit (idle, update, adapter change): the model went
				// with the process; the host's return re-warms it.
				struct conv_readiness_event ev = mk_ev(CONV_EV_EVICTED, t);
				apply_locked(core, i, &ev);
				hs->rewarm_evicted = true;
			}
		}
		break;
	}
	case CONV_HOST_EV_RESTORED:
		hs->down = false;
		hs->rewarm = true;
		core->redescribe[p] = true; // the fresh process's describe, then the re-warm
		U_LOG_W("[conv] provider %s: host process back (epoch %u); re-warming", id,
		        conv_host_get_epoch(hs->host));
		break;
	case CONV_HOST_EV_CRASH_LOOP:
		hs->down = true;
		hs->rewarm = false;
		U_LOG_W("[conv] provider %s: host crash loop: FAILED(crash_loop) until a registry change", id);
		for (uint32_t i = 0; i < core->reg.row_count; i++) {
			if (core->reg.rows[i].provider == p) {
				struct conv_readiness_event ev = mk_ev(CONV_EV_CRASH_LOOP, t);
				apply_locked(core, i, &ev);
			}
		}
		break;
	case CONV_HOST_EV_FAILED:
		hs->down = true;
		hs->rewarm = false;
		U_LOG_W("[conv] provider %s: host failed terminally (%s); FAILED until a registry change", id,
		        conv_reason_name((uint32_t)e->detail));
		for (uint32_t i = 0; i < core->reg.row_count; i++) {
			if (core->reg.rows[i].provider == p) {
				struct conv_readiness_event ev = mk_ev(CONV_EV_RUNTIME_FAILED, t);
				ev.reason = e->detail != 0 ? (uint32_t)e->detail : XRT_CONV_FAIL_LOAD;
				ev.retryable = false;
				apply_locked(core, i, &ev);
			}
		}
		break;
	default: break;
	}
}

//! Drain the lifecycle queue (registry_mtx held).
static void
host_events_locked(struct conv_core *core, uint64_t t)
{
	struct conv_host_evq_entry q[CONV_CORE_HOST_EVQ];
	os_mutex_lock(&core->evq_mtx);
	const uint32_t n = core->evq_count;
	memcpy(q, core->evq, sizeof(q[0]) * n);
	core->evq_count = 0;
	const uint32_t dropped = core->evq_dropped;
	core->evq_dropped = 0;
	os_mutex_unlock(&core->evq_mtx);
	if (dropped != 0) {
		U_LOG_W("[conv] %u host lifecycle event(s) dropped (queue full)", dropped);
	}
	for (uint32_t i = 0; i < n; i++) {
		host_event_locked(core, &q[i], t);
	}
}

//! After a RESTORED + its re-describe: re-warm what the crash left warming (registry_mtx held).
static void
rewarm_locked(struct conv_core *core, uint32_t p)
{
	struct conv_host_slot *hs = &core->hosts[p];
	if (!hs->rewarm || hs->down) {
		return;
	}
	hs->rewarm = false;
	const bool evicted = hs->rewarm_evicted;
	hs->rewarm_evicted = false;
	for (uint32_t i = 0; i < core->reg.row_count; i++) {
		const struct conv_cap_row *row = &core->reg.rows[i];
		if (row->provider != p) {
			continue;
		}
		if (row->rd.state == XRT_CONV_STATE_WARMING) {
			start_warm_locked(core, p, row->cap.cap_id);
		} else if (evicted && row->rd.state == XRT_CONV_STATE_EVICTED) {
			conv_core_request_warm_locked(core, p, row->cap.cap_id, CONV_EV_WARM_REQUESTED);
		}
	}
}


/*
 *
 * Selection.
 *
 */

static const char *
preferred_for(struct conv_core *core, uint32_t task)
{
	if (task >= XRT_CONV_TASK_RESERVED_FIRST || core->preferred[task][0] == '\0') {
		return NULL;
	}
	return core->preferred[task];
}

static bool
include_composed(const struct conv_core *core)
{
	return core->cfg.viewsyn.render != NULL;
}

uint32_t
conv_core_select_locked(struct conv_core *core,
                        uint32_t task,
                        uint32_t lane,
                        uint32_t input_kind,
                        uint32_t view_count,
                        const bool *busy_providers,
                        struct conv_binding *out,
                        uint32_t *out_state)
{
	static struct conv_select_row rows[CONV_REGISTRY_MAX_ROWS * 2]; // registry_mtx serialises
	static uint32_t row_map[CONV_REGISTRY_MAX_ROWS * 2];
	static uint32_t order[CONV_REGISTRY_MAX_ROWS * 2];
	const uint32_t n = conv_registry_build_candidates(&core->reg, task, include_composed(core), rows, row_map,
	                                                  CONV_REGISTRY_MAX_ROWS * 2);
	struct conv_select_request req = {task, input_kind, lane, view_count};
	struct conv_select_policy pol = {preferred_for(core, task), core->cfg.allow_research_only};
	struct conv_select_result res = conv_select(rows, n, &req, &pol, NULL, order);
	if (res.row < 0) {
		return res.reason;
	}
	int32_t pick = res.row;
	if (busy_providers != NULL && lane == XRT_CONV_LANE_JOB) {
		const uint32_t prov = core->reg.rows[row_map[pick]].provider;
		if (busy_providers[prov] && (rows[pick].flags & XRT_CONV_CAP_LANES_CONCURRENT) == 0) {
			bool after = false;
			for (uint32_t k = 0; k < res.candidate_count; k++) {
				const uint32_t idx = order[k];
				if ((int32_t)idx == res.row) {
					after = true;
					continue;
				}
				if (!after || !conv_state_is_selectable(rows[idx].state)) {
					continue;
				}
				const uint32_t p2 = core->reg.rows[row_map[idx]].provider;
				if (!busy_providers[p2] || (rows[idx].flags & XRT_CONV_CAP_LANES_CONCURRENT) != 0) {
					pick = (int32_t)idx;
					break;
				}
			}
		}
	}
	if (res.tie_warn) {
		U_LOG_I("[conv] task %s: two providers tie on ProbeOrder; provider id breaks the tie",
		        conv_task_name(task));
	}
	if (out != NULL) {
		out->provider = core->reg.rows[row_map[pick]].provider;
		out->cap_id = rows[pick].cap_id;
		out->composed = rows[pick].composed;
	}
	if (out_state != NULL) {
		*out_state = rows[pick].state;
	}
	return res.reason;
}

static bool
select_simple_locked(struct conv_core *core, uint32_t task, struct conv_binding *out, uint32_t *out_state)
{
	// Properties describe the default stream: two views for a view task.
	const uint32_t views = conv_task_produces_views(task) ? 2u : 0u;
	const uint32_t r = conv_core_select_locked(core, task, 0, 0, views, NULL, out, out_state);
	return r <= CONV_SEL_OK_FALLTHROUGH;
}


/*
 *
 * Providers.
 *
 */

/*! Mirror the provider's own view of each row after a describe: READY rows
 *  become READY, ABSENT ones ABSENT (registry_mtx held). */
static void
sync_states_locked(struct conv_core *core, uint32_t provider)
{
	struct conv_host *host = conv_core_host(core, provider);
	if (host == NULL) {
		return;
	}
	const uint64_t t = now_ns();
	for (uint32_t i = 0; i < core->reg.row_count; i++) {
		struct conv_cap_row *row = &core->reg.rows[i];
		if (row->provider != provider) {
			continue;
		}
		struct xrt_conv_state st;
		memset(&st, 0, sizeof(st));
		st.struct_size = (uint32_t)sizeof(st);
		if (conv_host_get_state(host, row->cap.cap_id, &st) != XRT_SUCCESS) {
			continue;
		}
		if (st.state == XRT_CONV_STATE_READY && row->rd.state == XRT_CONV_STATE_INSTALLED) {
			struct conv_readiness_event ev = mk_ev(CONV_EV_WARM_OK, t);
			apply_locked(core, i, &ev);
		} else if (st.state == XRT_CONV_STATE_DISABLED && declared_absent(&st) &&
		           row->rd.state == XRT_CONV_STATE_INSTALLED &&
		           !(provider < CONV_REGISTRY_MAX_PROVIDERS && core->retrying[provider])) {
			// A kill switch the provider reports before any warm: terminal at
			// once (v3 UNAVAILABLE without first going through a warm).
			struct conv_readiness_event ev = mk_ev(CONV_EV_DECLARED_ABSENT, t);
			ev.reason = CONV_REASON_POLICY;
			apply_locked(core, i, &ev);
		} else if (st.state == XRT_CONV_STATE_ABSENT && row->rd.state == XRT_CONV_STATE_INSTALLED &&
		           !(provider < CONV_REGISTRY_MAX_PROVIDERS && core->retrying[provider])) {
			// Terminal already (no timer); a re-describe that finds it lifts it.
			struct conv_readiness_event ev = mk_ev(CONV_EV_DESCRIBE_FAILED, t);
			ev.reason = st.failure_reason != 0 ? st.failure_reason : XRT_CONV_FAIL_MODEL;
			apply_locked(core, i, &ev);
		}
	}
}

static void
describe_locked(struct conv_core *core, uint32_t provider)
{
	const int32_t n = conv_registry_describe(&core->reg, provider, now_ns());
	struct conv_host *host = conv_core_host(core, provider);
	if (host != NULL) {
		conv_host_refresh_caps(host);
		core->hosts[provider].generation_seen = conv_host_get_generation(host);
	}
	core->conv_gen++;
	core->status_gen++;
	if (n >= 0) {
		sync_states_locked(core, provider);
	}
	if (provider < CONV_REGISTRY_MAX_PROVIDERS) {
		core->retrying[provider] = false;
	}
	emit_simple(core, XRT_CONV_WIRE_EVENT_PROVIDERS_CHANGED, 0, 0, 0, 0, -1.0f);
}

static void
prewarm_locked(struct conv_core *core, uint64_t mask)
{
	for (uint32_t task = XRT_CONV_TASK_DEPTH_MONO; task < XRT_CONV_TASK_RESERVED_FIRST; task++) {
		if ((mask & (1ull << task)) == 0 || !conv_task_is_served(task)) {
			continue;
		}
		struct conv_binding b;
		if (select_simple_locked(core, task, &b, NULL)) {
			conv_core_request_warm_locked(core, b.provider, b.cap_id, CONV_EV_WARM_REQUESTED);
		}
	}
}

static uint64_t
prewarm_mask(const struct conv_core *core)
{
	return core->cfg.prewarm_task_mask != 0 ? core->cfg.prewarm_task_mask : CONV_PREWARM_DEFAULT_MASK;
}

int32_t
conv_core_add_provider(struct conv_core *core, const struct conv_provider_desc *desc)
{
	struct conv_host *host = NULL;
	if (desc->iface != NULL) {
		if (conv_host_inproc_create(desc->id, desc->iface, desc->inst, &host) != XRT_SUCCESS) {
			U_LOG_W("[conv] provider %s: could not create its in-service host", desc->id);
			return -1;
		}
	} else if (desc->host != NULL) {
		host = desc->host; // a satellite proxy: ours from here on
	}
	os_mutex_lock(&core->registry_mtx);
	const int32_t pi = conv_registry_add(&core->reg, desc);
	if (pi < 0) {
		os_mutex_unlock(&core->registry_mtx);
		conv_host_destroy(&host);
		return -1;
	}
	struct conv_host *old = core->hosts[pi].host;
	core->hosts[pi].host = host;
	core->hosts[pi].hung = false;
	core->hosts[pi].down = false;
	core->hosts[pi].rewarm = false;
	core->hosts[pi].listener.core = core;
	core->hosts[pi].listener.provider = (uint32_t)pi;
	if (host != NULL) {
		// Before the host's first command (the describe below).
		host->event_ud = &core->hosts[pi].listener;
		host->on_event = conv_core_on_host_event;
	}
	if (host != NULL) {
		describe_locked(core, (uint32_t)pi);
	} else {
		core->conv_gen++;
	}
	os_mutex_unlock(&core->registry_mtx);

	if (old != NULL && old != host) {
		// Same id re-added: the old host keeps no lanes of its own (lanes are
		// per provider index); in-service, it is adopted at restart (spec §4.1).
		U_LOG_W("[conv] provider %s re-registered; the previous instance is leaked until restart", desc->id);
	}
	if (host != NULL) {
		conv_stream_lane_start(core, (uint32_t)pi);
		conv_job_lane_start(core, (uint32_t)pi);
	}

	os_mutex_lock(&core->registry_mtx);
	if (core->cfg.prewarm_on == CONV_PREWARM_START ||
	    (core->cfg.prewarm_on == CONV_PREWARM_INTEREST && core->interest_seen)) {
		prewarm_locked(core, prewarm_mask(core));
	}
	os_mutex_unlock(&core->registry_mtx);
	U_LOG_W("[conv] provider %s registered (%u capability rows, %s)", desc->id, core->reg.row_count,
	        host == NULL                                  ? "describe cache only"
	        : host->hosting == CONV_HOSTING_SATELLITE ? "satellite host"
	                                                  : "in-service host");
	return pi;
}

bool
conv_core_provider_described(struct conv_core *core, const char *provider_id)
{
	os_mutex_lock(&core->registry_mtx);
	const int32_t pi = conv_registry_find_provider(&core->reg, provider_id);
	const bool d = pi >= 0 && core->reg.providers[pi].described;
	os_mutex_unlock(&core->registry_mtx);
	return d;
}

int32_t
conv_core_load_describe_cache(struct conv_core *core, const uint8_t *buf, size_t len)
{
	os_mutex_lock(&core->registry_mtx);
	const int32_t r = conv_registry_cache_load(&core->reg, buf, len, now_ns());
	if (r > 0) {
		core->conv_gen++;
		core->status_gen++;
	}
	os_mutex_unlock(&core->registry_mtx);
	return r;
}

size_t
conv_core_save_describe_cache(struct conv_core *core, uint8_t *buf, size_t buf_size)
{
	os_mutex_lock(&core->registry_mtx);
	const size_t r = conv_registry_cache_save(&core->reg, buf, buf_size);
	os_mutex_unlock(&core->registry_mtx);
	return r;
}

void
conv_core_set_corpus_validated(struct conv_core *core, const char *provider_id, uint32_t cap_id, bool validated)
{
	os_mutex_lock(&core->registry_mtx);
	for (uint32_t i = 0; i < core->validated_count; i++) {
		if (core->validated[i].cap_id == cap_id && strcmp(core->validated[i].provider_id, provider_id) == 0) {
			if (!validated) {
				core->validated[i] = core->validated[--core->validated_count];
			}
			os_mutex_unlock(&core->registry_mtx);
			return;
		}
	}
	if (validated && core->validated_count < CONV_CORE_MAX_VALIDATED) {
		struct conv_validated *v = &core->validated[core->validated_count++];
		memset(v, 0, sizeof(*v));
		strncpy(v->provider_id, provider_id, sizeof(v->provider_id) - 1);
		v->cap_id = cap_id;
	}
	os_mutex_unlock(&core->registry_mtx);
}

void
conv_core_provider_changed(struct conv_core *core, const char *provider_id)
{
	os_mutex_lock(&core->registry_mtx);
	const int32_t pi = conv_registry_find_provider(&core->reg, provider_id);
	if (pi >= 0) {
		// #2005: a change retries the provider's ABSENT and FAILED rows (a
		// model installed, a binary replaced) -- re-describing alone keeps a
		// surviving row's readiness, so an ABSENT row stayed UNAVAILABLE
		// until restart (G5's shape, on the core).
		const uint64_t t = now_ns();
		for (uint32_t i = 0; i < core->reg.row_count; i++) {
			if (core->reg.rows[i].provider == (uint32_t)pi) {
				struct conv_readiness_event ev = mk_ev(CONV_EV_REGISTRY_CHANGED, t);
				apply_locked(core, i, &ev);
			}
		}
		core->redescribe[pi] = true;
		core->retrying[pi] = true;
		core->prewarm_after_describe = true;
		os_cond_signal(&core->ctrl_cond);
	}
	os_mutex_unlock(&core->registry_mtx);
}

void
conv_core_set_provider_enabled(struct conv_core *core, const char *provider_id, bool enabled)
{
	os_mutex_lock(&core->registry_mtx);
	const int32_t pi = conv_registry_find_provider(&core->reg, provider_id);
	if (pi >= 0) {
		core->reg.providers[pi].enabled = enabled;
		const uint64_t t = now_ns();
		for (uint32_t i = 0; i < core->reg.row_count; i++) {
			if (core->reg.rows[i].provider == (uint32_t)pi) {
				struct conv_readiness_event ev = mk_ev(enabled ? CONV_EV_ENABLE : CONV_EV_DISABLE, t);
				apply_locked(core, i, &ev);
			}
		}
		core->conv_gen++;
	}
	os_mutex_unlock(&core->registry_mtx);
}

bool
conv_core_set_policy(struct conv_core *core, const char *const *preferred, bool allow_research_only)
{
	bool changed = false;
	os_mutex_lock(&core->registry_mtx);
	for (uint32_t t = 0; t < XRT_CONV_TASK_RESERVED_FIRST; t++) {
		const char *p = preferred != NULL && preferred[t] != NULL ? preferred[t] : "";
		if (strncmp(core->preferred[t], p, sizeof(core->preferred[t]) - 1) == 0) {
			continue;
		}
		U_LOG_W("[conv] policy: PreferredProvider\\%s '%s' -> '%s'", conv_task_name(t), core->preferred[t], p);
		memset(core->preferred[t], 0, sizeof(core->preferred[t]));
		strncpy(core->preferred[t], p, sizeof(core->preferred[t]) - 1);
		changed = true;
	}
	if (core->cfg.allow_research_only != allow_research_only) {
		U_LOG_W("[conv] policy: AllowResearchOnly %d -> %d", (int)core->cfg.allow_research_only,
		        (int)allow_research_only);
		core->cfg.allow_research_only = allow_research_only;
		changed = true;
	}
	if (changed) {
		// The effective capability of a task may have moved: properties /
		// status readers wake, and the new selection follows PrewarmOn.
		core->conv_gen++;
		core->status_gen++;
		core->prewarm_after_describe = true;
		emit_simple(core, XRT_CONV_WIRE_EVENT_PROVIDERS_CHANGED, 0, 0, 0, 0, -1.0f);
		os_cond_signal(&core->ctrl_cond);
	}
	os_mutex_unlock(&core->registry_mtx);
	return changed;
}


/*
 *
 * Properties.
 *
 */

void
conv_core_get_generation(struct conv_core *core, struct xrt_conv_wire_generation *out)
{
	memset(out, 0, sizeof(*out));
	out->struct_size = (uint32_t)sizeof(*out);
	out->family_version = XRT_CONV_WIRE_FAMILY_VERSION_CURRENT;
	os_mutex_lock(&core->registry_mtx);
	out->conv_gen = core->conv_gen;
	out->status_gen = core->status_gen;
	os_mutex_unlock(&core->registry_mtx);
}

bool
conv_core_get_task_desc(struct conv_core *core, uint32_t task, struct xrt_conv_wire_task_desc *out)
{
	if (!conv_task_is_served(task)) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	out->struct_size = (uint32_t)sizeof(*out);
	out->family_version = XRT_CONV_WIRE_FAMILY_VERSION_CURRENT;
	out->task = task;
	out->progress = -1.0f;

	os_mutex_lock(&core->registry_mtx);
	struct conv_binding b;
	uint32_t state = 0;
	if (!select_simple_locked(core, task, &b, &state)) {
		out->state =
		    conv_registry_discovery_settled(&core->reg) ? XRT_CONV_STATE_ABSENT : XRT_CONV_STATE_UNKNOWN;
		// No candidate, but a provider declares the task: report why it cannot
		// serve (FAILED(VERSION), UNLICENSED, DISABLED, ...) rather than
		// ABSENT, which means "nobody can" (spec 4: the reason is app-visible).
		// The first-ranked such row (lowest ProbeOrder) speaks for the task.
		int32_t best = -1;
		for (uint32_t i = 0; i < core->reg.row_count; i++) {
			const struct conv_cap_row *row = &core->reg.rows[i];
			if (row->cap.task != task || row->provider >= core->reg.provider_count ||
			    !conv_state_is_terminal(row->rd.state) || row->rd.state == XRT_CONV_STATE_ABSENT) {
				continue;
			}
			if (best < 0 || core->reg.providers[row->provider].probe_order <
			                    core->reg.providers[core->reg.rows[best].provider].probe_order) {
				best = (int32_t)i;
			}
		}
		if (best >= 0 && out->state == XRT_CONV_STATE_ABSENT) {
			const struct conv_cap_row *row = &core->reg.rows[best];
			out->state = row->rd.state;
			out->failure_reason = row->rd.reason < CONV_REASON_LOCAL_BASE ? row->rd.reason : 0;
			out->retryable = row->rd.retryable ? 1u : 0u;
		}
		os_mutex_unlock(&core->registry_mtx);
		return true;
	}
	const int32_t ri = row_index_locked(core, b.provider, b.cap_id);
	const struct conv_cap_row *row = &core->reg.rows[ri];
	const struct xrt_conv_capability *c = &row->cap;
	out->state = row->rd.state;
	out->failure_reason = row->rd.reason < CONV_REASON_LOCAL_BASE ? row->rd.reason : 0;
	out->retryable = row->rd.retryable ? 1u : 0u;
	out->progress = (row->rd.state == XRT_CONV_STATE_WARMING || row->rd.state == XRT_CONV_STATE_DOWNLOADING)
	                    ? row->rd.progress
	                    : -1.0f;
	out->lanes = c->lanes;
	out->input_kinds = c->input_kinds;
	out->outputs = c->outputs;
	out->flags = c->flags;
	out->import_memory_kinds = c->import_memory_kinds;
	out->import_sync_kinds = c->import_sync_kinds;
	out->input_formats = c->input_formats;
	out->depth_output_format = c->depth_output_format;
	out->min_views = c->min_views;
	out->max_views = c->max_views;
	out->max_streams = c->max_streams;
	out->max_concurrent = c->max_concurrent;
	out->max_input_edge = c->max_input_edge;
	out->preferred_input_edge = c->preferred_input_edge;
	out->depth_kind = c->depth_kind;
	out->depth_measure = c->depth_measure;
	out->intrinsics_source = c->intrinsics_source;
	out->splat_formats = c->splat_formats;
	out->licence_class = c->licence_class;
	out->commercial_ok = c->commercial_ok;
	out->est_warmup_ms = c->est_warmup_ms;
	out->est_latency_ms = c->est_latency_ms;
	out->est_vram_mb = c->est_vram_mb;
	out->est_ram_mb = c->est_ram_mb;
	out->device_luid = c->device_luid;
	if (b.composed) {
		out->desc_flags |= XRT_CONV_WIRE_DESC_COMPOSED;
		out->lanes = XRT_CONV_LANE_STREAM;
		out->outputs = XRT_CONV_OUT_VIEWS | XRT_CONV_OUT_DEPTH;
		out->flags |= XRT_CONV_CAP_EXPLICIT_VIEWPOINTS;
		out->min_views = 2;
		out->max_views = XRT_CONV_MAX_VIEWS;
	}
	os_mutex_unlock(&core->registry_mtx);
	return true;
}

uint32_t
conv_core_task_state(struct conv_core *core, uint32_t task)
{
	os_mutex_lock(&core->registry_mtx);
	uint32_t state = XRT_CONV_STATE_UNKNOWN;
	struct conv_binding b;
	if (!select_simple_locked(core, task, &b, &state)) {
		state = conv_registry_discovery_settled(&core->reg) ? XRT_CONV_STATE_ABSENT : XRT_CONV_STATE_UNKNOWN;
	}
	os_mutex_unlock(&core->registry_mtx);
	return state;
}

uint32_t
conv_core_v3_state(struct conv_core *core, uint32_t v3_modes, bool photo_content)
{
	if (v3_modes == 0) {
		v3_modes = CONV_V3_MODE_DEPTH | CONV_V3_MODE_SBS | CONV_V3_MODE_NVIEW | CONV_V3_MODE_GAUSSIANS;
	}
	uint32_t states[4];
	uint32_t n = 0;
	os_mutex_lock(&core->registry_mtx);
	for (uint32_t bit = 1; bit <= CONV_V3_MODE_GAUSSIANS; bit <<= 1) {
		if ((v3_modes & bit) == 0) {
			continue;
		}
		const uint32_t task = conv_legacy_mode_task(bit, photo_content);
		struct conv_binding b;
		uint32_t s = 0;
		if (task != XRT_CONV_TASK_NONE && select_simple_locked(core, task, &b, &s)) {
			states[n++] = s;
		}
	}
	const bool settled = conv_registry_discovery_settled(&core->reg);
	os_mutex_unlock(&core->registry_mtx);
	return conv_legacy_v3_state(states, n, settled);
}

bool
conv_core_discovery_settled(struct conv_core *core)
{
	os_mutex_lock(&core->registry_mtx);
	const bool settled = conv_registry_discovery_settled(&core->reg);
	os_mutex_unlock(&core->registry_mtx);
	return settled;
}

bool
conv_core_task_has_candidate(struct conv_core *core, uint32_t task)
{
	if (!conv_task_is_served(task)) {
		return false;
	}
	os_mutex_lock(&core->registry_mtx);
	struct conv_binding b;
	const bool ok = select_simple_locked(core, task, &b, NULL);
	os_mutex_unlock(&core->registry_mtx);
	return ok;
}

void
conv_core_copy_registry(struct conv_core *core, struct conv_registry *out)
{
	os_mutex_lock(&core->registry_mtx);
	memcpy(out, &core->reg, sizeof(*out));
	os_mutex_unlock(&core->registry_mtx);
}

void
conv_core_activity_changed(struct conv_core *core)
{
	os_mutex_lock(&core->registry_mtx);
	core->status_gen++;
	os_mutex_unlock(&core->registry_mtx);
}

void
conv_core_row_activity(struct conv_core *core,
                       const char *provider_id,
                       uint32_t cap_id,
                       bool composed,
                       uint32_t *out_streams,
                       uint32_t *out_jobs)
{
	uint32_t streams = 0, jobs = 0;
	if (core != NULL && provider_id != NULL) {
		os_mutex_lock(&core->registry_mtx);
		const int32_t pi = conv_registry_find_provider(&core->reg, provider_id);
		os_mutex_unlock(&core->registry_mtx);
		if (pi >= 0) {
			// No registry lock across the counts: streams_mtx / jobs_mtx come first.
			streams = conv_streams_row_count(core, (uint32_t)pi, cap_id, composed);
			jobs = conv_jobs_row_count(core, (uint32_t)pi, cap_id, composed);
		}
	}
	if (out_streams != NULL) {
		*out_streams = streams;
	}
	if (out_jobs != NULL) {
		*out_jobs = jobs;
	}
}

void
conv_core_hint_interest(struct conv_core *core, uint64_t client_id, uint64_t task_mask)
{
	(void)client_id;
	os_mutex_lock(&core->registry_mtx);
	const bool first = !core->interest_seen;
	core->interest_seen = true;
	if (core->cfg.prewarm_on == CONV_PREWARM_INTEREST) {
		uint64_t mask = prewarm_mask(core);
		if (task_mask != 0) {
			mask &= task_mask;
		}
		prewarm_locked(core, mask);
	}
	os_mutex_unlock(&core->registry_mtx);
	if (first) {
		U_LOG_W("[conv] first conv_hint_interest (PrewarmOn=%s)",
		        core->cfg.prewarm_on == CONV_PREWARM_INTEREST
		            ? "interest"
		            : (core->cfg.prewarm_on == CONV_PREWARM_START ? "start" : "first-create"));
	}
}

xrt_result_t
conv_core_warm_task(struct conv_core *core, uint32_t task)
{
	os_mutex_lock(&core->registry_mtx);
	struct conv_binding b;
	const bool ok = select_simple_locked(core, task, &b, NULL);
	if (ok) {
		conv_core_request_warm_locked(core, b.provider, b.cap_id, CONV_EV_WARM_REQUESTED);
	}
	os_mutex_unlock(&core->registry_mtx);
	return ok ? XRT_SUCCESS : XRT_ERROR_FEATURE_NOT_SUPPORTED;
}

xrt_result_t
conv_core_warm_task_explicit(struct conv_core *core, uint32_t task, uint32_t *out_prev, uint32_t *out_state)
{
	uint32_t prev = XRT_CONV_STATE_UNKNOWN, now = XRT_CONV_STATE_UNKNOWN;
	xrt_result_t r = XRT_ERROR_FEATURE_NOT_SUPPORTED;
	if (core != NULL && conv_task_is_served(task)) {
		os_mutex_lock(&core->registry_mtx);
		struct conv_binding b;
		uint32_t state = 0;
		bool have = select_simple_locked(core, task, &b, &state);
		prev = have ? state
		            : (conv_registry_discovery_settled(&core->reg) ? XRT_CONV_STATE_ABSENT
		                                                           : XRT_CONV_STATE_UNKNOWN);
		if (!have || conv_state_is_terminal(state)) {
			// Every candidate is terminal: the explicit warm retries them (a
			// registry-change event per row, so FAILED of any reason and
			// ABSENT(policy) alike go back to INSTALLED; DISABLED stays).
			const uint64_t t = now_ns();
			bool any = false;
			for (uint32_t i = 0; i < core->reg.row_count; i++) {
				struct conv_cap_row *row = &core->reg.rows[i];
				const uint32_t s = row->rd.state;
				if (row->cap.task != task ||
				    (s != XRT_CONV_STATE_FAILED && s != XRT_CONV_STATE_ABSENT)) {
					continue;
				}
				struct conv_readiness_event ev = mk_ev(CONV_EV_REGISTRY_CHANGED, t);
				apply_locked(core, i, &ev);
				if (row->provider < CONV_REGISTRY_MAX_PROVIDERS) {
					// The re-describe this queues must not demote the row
					// before its warm answered (conv_core_provider_changed's rule).
					core->retrying[row->provider] = true;
				}
				any = true;
			}
			if (any) {
				have = select_simple_locked(core, task, &b, &state);
			}
		}
		if (have) {
			r = XRT_SUCCESS;
			if (state != XRT_CONV_STATE_READY) {
				conv_core_request_warm_locked(core, b.provider, b.cap_id, CONV_EV_WARM_REQUESTED);
			}
			now = conv_core_row_state_locked(core, b.provider, b.cap_id);
			U_LOG_W("[conv] conv warm %s: %s cap %u %s -> %s", conv_task_name(task),
			        core->reg.providers[b.provider].id, b.cap_id, conv_state_name(prev),
			        conv_state_name(now));
		} else {
			now = prev;
		}
		os_mutex_unlock(&core->registry_mtx);
	}
	if (out_prev != NULL) {
		*out_prev = prev;
	}
	if (out_state != NULL) {
		*out_state = now;
	}
	return r;
}

struct evict_op
{
	struct conv_host_cmd cmd;
	struct conv_core *core;
	uint32_t provider;
};

static void
evict_done(struct conv_host_cmd *cmd)
{
	struct evict_op *op = (struct evict_op *)cmd->user;
	struct conv_core *core = op->core;
	if (cmd->result == XRT_SUCCESS) {
		os_mutex_lock(&core->registry_mtx);
		struct conv_readiness_event ev = mk_ev(CONV_EV_EVICTED, now_ns());
		apply_by_cap_locked(core, op->provider, cmd->cap_id, &ev);
		os_mutex_unlock(&core->registry_mtx);
	}
	free(op);
}

xrt_result_t
conv_core_evict_task(struct conv_core *core, uint32_t task)
{
	os_mutex_lock(&core->registry_mtx);
	struct conv_binding b;
	uint32_t state = 0;
	const bool ok = select_simple_locked(core, task, &b, &state);
	os_mutex_unlock(&core->registry_mtx);
	if (!ok || state != XRT_CONV_STATE_READY) {
		return ok ? XRT_SUCCESS : XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	// Never while a stream or job is bound (keep-alive rule 1).
	if (conv_streams_bound_count(core, b.provider, b.cap_id, false) > 0 ||
	    conv_jobs_bound_count(core, b.provider, b.cap_id) > 0) {
		return XRT_SUCCESS;
	}
	struct conv_host *host = conv_core_host(core, b.provider);
	if (host == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	struct evict_op *op = (struct evict_op *)calloc(1, sizeof(*op));
	if (op == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	op->core = core;
	op->provider = b.provider;
	op->cmd.op = CONV_OP_EVICT;
	op->cmd.cap_id = b.cap_id;
	op->cmd.on_complete = evict_done;
	op->cmd.user = op;
	xrt_result_t r = conv_host_submit(host, &op->cmd);
	if (r != XRT_SUCCESS) {
		free(op);
	}
	return r;
}


/*
 *
 * Failures.
 *
 */

void
conv_core_call_failed(struct conv_core *core, uint32_t provider, uint32_t cap_id, xrt_result_t r)
{
	struct conv_host *host = conv_core_host(core, provider);
	if (host != NULL && r == XRT_ERROR_IPC_FAILURE && host->hosting == CONV_HOSTING_SATELLITE) {
		// The host process went away under the call: the crash path owns the
		// row (CONV_HOST_EV_LOST), not the call's failure.
		return;
	}
	struct xrt_conv_state st;
	memset(&st, 0, sizeof(st));
	st.struct_size = (uint32_t)sizeof(st);
	const bool have = host != NULL && conv_host_get_state(host, cap_id, &st) == XRT_SUCCESS;

	os_mutex_lock(&core->registry_mtx);
	if (have && st.state == XRT_CONV_STATE_FAILED) {
		// Device loss and friends: FAILED(reason, retryable), then re-warm on backoff.
		struct conv_readiness_event ev = mk_ev(CONV_EV_RUNTIME_FAILED, now_ns());
		ev.reason = st.failure_reason != 0 ? st.failure_reason : XRT_CONV_FAIL_DEVICE_LOST;
		ev.retryable = st.retryable != 0;
		apply_by_cap_locked(core, provider, cap_id, &ev);
	} else if (host == NULL && r != XRT_SUCCESS && core->hosts[provider].host != NULL) {
		// The host is gone (abandoned): its rows already went FAILED(hung).
	}
	os_mutex_unlock(&core->registry_mtx);
}


/*
 *
 * Eyes.
 *
 */

static bool
eyes_key_ok(uint64_t key)
{
	return key != CONV_EYES_DP_ANY && key != CONV_EYES_DP_NONE;
}

//! eyes_mtx held. Slot of @p key, or -1.
static int32_t
eyes_find_locked(struct conv_core *core, uint64_t key)
{
	for (uint32_t i = 0; i < CONV_CORE_MAX_DPS; i++) {
		if (core->eyes_key[i] == key) {
			return (int32_t)i;
		}
	}
	return -1;
}

void
conv_core_eyes_publish(struct conv_core *core, uint64_t dp_key, const struct conv_eyes *eyes)
{
	if (core == NULL || eyes == NULL || !eyes_key_ok(dp_key)) {
		return;
	}
	os_mutex_lock(&core->eyes_mtx);
	int32_t s = eyes_find_locked(core, dp_key);
	if (s < 0) {
		// A free slot, else a retired one, else the least recently published.
		int32_t retired = -1, lru = -1;
		for (uint32_t i = 0; i < CONV_CORE_MAX_DPS && s < 0; i++) {
			if (core->eyes_key[i] == 0) {
				s = (int32_t)i;
			} else if (!core->eyes[i].live && retired < 0) {
				retired = (int32_t)i;
			} else if (lru < 0 || core->eyes_publish_seq[i] < core->eyes_publish_seq[lru]) {
				lru = (int32_t)i;
			}
		}
		if (s < 0) {
			s = retired >= 0 ? retired : lru;
		}
		core->eyes_key[s] = dp_key;
	}
	conv_eyes_slot_publish(&core->eyes[s], eyes);
	core->eyes_publish_seq[s] = ++core->eyes_seq;
	if (core->eyes_first_key == 0) {
		core->eyes_first_key = dp_key;
	} else if (dp_key != core->eyes_first_key) {
		core->eyes_multi_dp = true; // sticky for the service lifetime
	}
	conv_eyes_slot_publish(&core->eyes_any, eyes);
	core->eyes_any_key = dp_key;
	os_mutex_unlock(&core->eyes_mtx);
}

void
conv_core_eyes_retire(struct conv_core *core, uint64_t dp_key)
{
	if (core == NULL || !eyes_key_ok(dp_key)) {
		return;
	}
	os_mutex_lock(&core->eyes_mtx);
	const int32_t s = eyes_find_locked(core, dp_key);
	if (s >= 0) {
		// The key stays (its readers must see "gone", not "unknown") until
		// the slot is reused for another DP.
		conv_eyes_slot_retire(&core->eyes[s]);
	}
	if (core->eyes_any_key == dp_key) {
		conv_eyes_slot_retire(&core->eyes_any);
		core->eyes_any_key = 0;
	}
	os_mutex_unlock(&core->eyes_mtx);
}

bool
conv_core_eyes_read(struct conv_core *core, uint64_t dp_key, uint64_t now, struct conv_eyes *out)
{
	memset(out, 0, sizeof(*out));
	if (core == NULL || dp_key == CONV_EYES_DP_NONE) {
		return false;
	}
	os_mutex_lock(&core->eyes_mtx);
	const int32_t s = dp_key == CONV_EYES_DP_ANY ? -1 : eyes_find_locked(core, dp_key);
	if (s < 0 && dp_key != CONV_EYES_DP_ANY && core->eyes_multi_dp) {
		// An unknown key once two DPs exist: another screen's eyes would be in
		// another display space. Stale: the nominal viewer.
		os_mutex_unlock(&core->eyes_mtx);
		return false;
	}
	struct conv_eyes_slot *slot = s >= 0 ? &core->eyes[s] : &core->eyes_any;
	const bool fresh = conv_eyes_slot_read(slot, now, core->cfg.eyes_max_age_ns, out);
	os_mutex_unlock(&core->eyes_mtx);
	return fresh;
}


/*
 *
 * Control thread.
 *
 */

static void
watchdog_locked(struct conv_core *core, uint64_t t)
{
	const uint64_t stream_min =
	    core->cfg.watchdog_stream_ns != 0 ? core->cfg.watchdog_stream_ns : 10ull * CONV_NS_PER_S;
	for (uint32_t p = 0; p < core->reg.provider_count; p++) {
		struct conv_host_slot *hs = &core->hosts[p];
		if (hs->host == NULL || hs->hung || hs->down) {
			continue;
		}
		struct conv_host_heartbeat hb;
		conv_host_get_heartbeat(hs->host, &hb);
		if (hb.oldest_since_ns == 0 || t <= hb.oldest_since_ns) {
			continue;
		}
		uint64_t limit = 0;
		struct xrt_conv_capability cap;
		const bool have_cap = conv_core_row_cap_locked(core, p, hb.oldest_cap_id, &cap);
		switch (hb.oldest_op) {
		case CONV_OP_CONVERT:
		case CONV_OP_STREAM_CREATE:
		case CONV_OP_STREAM_DESTROY: {
			const uint64_t lat = have_cap ? (uint64_t)cap.est_latency_ms * 1000000ull * 20ull : 0;
			limit = lat > stream_min ? lat : stream_min;
			break;
		}
		case CONV_OP_WARM:
			limit =
			    (have_cap ? (uint64_t)cap.est_warmup_ms * 3ull * 1000000ull : 0) + 60ull * CONV_NS_PER_S;
			break;
		default: limit = 0; break; // jobs: judged by progress (conv_job.c)
		}
		if (limit == 0 || t - hb.oldest_since_ns <= limit) {
			continue;
		}
		if (hs->host->hosting == CONV_HOSTING_SATELLITE) {
			// Satellite: kill the host (exit 30); the loss then takes the
			// crash path (WARMING(restarting), respawn after the backoff).
			U_LOG_W("[conv] watchdog: provider %s cap %u stuck in %s for %.1f s: killing its host",
			        core->reg.providers[p].id, hb.oldest_cap_id,
			        hb.oldest_op == CONV_OP_WARM ? "warm" : "a stream call",
			        (double)(t - hb.oldest_since_ns) / 1e9);
			hs->down = true; // once: the LOST event confirms it
			conv_host_abandon(hs->host);
			continue;
		}
		U_LOG_W("[conv] watchdog: provider %s cap %u stuck in %s for %.1f s: FAILED(hung)",
		        core->reg.providers[p].id, hb.oldest_cap_id,
		        hb.oldest_op == CONV_OP_WARM ? "warm" : "a stream call",
		        (double)(t - hb.oldest_since_ns) / 1e9);
		hs->hung = true;
		conv_host_abandon(hs->host);
		for (uint32_t i = 0; i < core->reg.row_count; i++) {
			if (core->reg.rows[i].provider == p) {
				struct conv_readiness_event ev = mk_ev(CONV_EV_RUNTIME_FAILED, t);
				ev.reason = CONV_REASON_HUNG;
				ev.retryable = false;
				apply_locked(core, i, &ev);
			}
		}
	}
}

static void
poll_providers_locked(struct conv_core *core, uint64_t t)
{
	for (uint32_t i = 0; i < core->reg.row_count; i++) {
		struct conv_cap_row *row = &core->reg.rows[i];
		struct conv_host *host = conv_core_host(core, row->provider);
		// Timers: backoff / crash-loop retries.
		struct conv_readiness_event tick = mk_ev(CONV_EV_TIMER, t);
		apply_locked(core, i, &tick);
		if (host == NULL) {
			continue;
		}
		const uint32_t s = row->rd.state;
		if (s != XRT_CONV_STATE_WARMING && s != XRT_CONV_STATE_DOWNLOADING && s != XRT_CONV_STATE_READY) {
			continue;
		}
		struct xrt_conv_state st;
		memset(&st, 0, sizeof(st));
		st.struct_size = (uint32_t)sizeof(st);
		if (conv_host_get_state(host, row->cap.cap_id, &st) != XRT_SUCCESS) {
			continue;
		}
		if (s == XRT_CONV_STATE_WARMING) {
			if (st.state == XRT_CONV_STATE_READY) {
				struct conv_readiness_event ev = mk_ev(CONV_EV_WARM_OK, t);
				apply_locked(core, i, &ev);
			} else if (st.progress >= 0.0f && st.progress != row->rd.progress) {
				struct conv_readiness_event ev = mk_ev(CONV_EV_WARM_PROGRESS, t);
				ev.progress = st.progress;
				apply_locked(core, i, &ev);
			}
		} else if (s == XRT_CONV_STATE_DOWNLOADING) {
			if (st.state == XRT_CONV_STATE_WARMING) {
				struct conv_readiness_event ev = mk_ev(CONV_EV_DOWNLOAD_DONE, t);
				apply_locked(core, i, &ev);
			} else if (st.progress >= 0.0f && st.progress != row->rd.progress) {
				struct conv_readiness_event ev = mk_ev(CONV_EV_DOWNLOAD_PROGRESS, t);
				ev.progress = st.progress;
				apply_locked(core, i, &ev);
			}
		} else if (st.state == XRT_CONV_STATE_FAILED) {
			struct conv_readiness_event ev = mk_ev(CONV_EV_RUNTIME_FAILED, t);
			ev.reason = st.failure_reason;
			ev.retryable = st.retryable != 0;
			apply_locked(core, i, &ev);
		}
	}
	// A changed generation means describe_cap would answer differently.
	for (uint32_t p = 0; p < core->reg.provider_count; p++) {
		struct conv_host *host = conv_core_host(core, p);
		if (host != NULL && conv_host_get_generation(host) != core->hosts[p].generation_seen) {
			core->redescribe[p] = true;
		}
	}
}

static void *
ctrl_main(void *ptr)
{
	struct conv_core *core = (struct conv_core *)ptr;
	const uint64_t tick_ns = (uint64_t)(core->cfg.tick_ms != 0 ? core->cfg.tick_ms : 100u) * 1000000ull;
	uint64_t last_tick = 0;
	os_mutex_lock(&core->registry_mtx);
	while (!core->stop) {
		const uint64_t t = now_ns();
		host_events_locked(core, t);
		if (t - last_tick >= tick_ns) {
			last_tick = t;
			poll_providers_locked(core, t);
			watchdog_locked(core, t);
		}
		for (uint32_t p = 0; p < core->reg.provider_count; p++) {
			if (core->redescribe[p]) {
				core->redescribe[p] = false;
				if (conv_core_host(core, p) != NULL) {
					describe_locked(core, p);
					core->rebind_check = true;
				}
			}
			rewarm_locked(core, p);
		}
		// #2005: rows a change brought back follow PrewarmOn like rows of a
		// provider added after the first hint (conv_core_add_provider).
		if (core->prewarm_after_describe) {
			core->prewarm_after_describe = false;
			if (core->cfg.prewarm_on == CONV_PREWARM_START ||
			    (core->cfg.prewarm_on == CONV_PREWARM_INTEREST && core->interest_seen)) {
				prewarm_locked(core, prewarm_mask(core));
			}
		}
		const bool wake = core->wake_pending;
		const bool rebind = core->rebind_check;
		core->wake_pending = false;
		core->rebind_check = false;
		if (wake || rebind) {
			os_mutex_unlock(&core->registry_mtx);
			if (rebind) {
				conv_streams_rebind_check(core);
			}
			conv_streams_wake(core);
			conv_jobs_wake(core);
			os_mutex_lock(&core->registry_mtx);
			continue;
		}
		os_cond_wait_timeout_ns(&core->ctrl_cond, &core->registry_mtx, tick_ns);
	}
	os_mutex_unlock(&core->registry_mtx);
	return NULL;
}


/*
 *
 * Create / destroy.
 *
 */

xrt_result_t
conv_core_create(const struct conv_core_config *cfg, struct conv_core **out_core)
{
	if (cfg == NULL || out_core == NULL || cfg->alloc.alloc == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	struct conv_core *core = (struct conv_core *)calloc(1, sizeof(*core));
	if (core == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	core->cfg = *cfg;
	for (uint32_t t = 0; t < XRT_CONV_TASK_RESERVED_FIRST; t++) {
		if (cfg->preferred[t] != NULL) {
			strncpy(core->preferred[t], cfg->preferred[t], sizeof(core->preferred[t]) - 1);
		}
		core->cfg.preferred[t] = NULL; // the copies are authoritative
	}
	conv_registry_init(&core->reg);
	core->next_stream_id = 1;
	core->next_job_id = 1;
	if (os_mutex_init(&core->registry_mtx) != 0 || os_cond_init(&core->ctrl_cond) != 0 ||
	    os_mutex_init(&core->streams_mtx) != 0 || os_mutex_init(&core->jobs_mtx) != 0 ||
	    os_mutex_init(&core->eyes_mtx) != 0 ||
	    os_mutex_init(&core->evq_mtx) != 0) {
		free(core);
		return XRT_ERROR_THREADING_INIT_FAILURE;
	}
	for (uint32_t i = 0; i < CONV_CORE_MAX_DPS; i++) {
		conv_eyes_slot_init(&core->eyes[i]);
	}
	conv_eyes_slot_init(&core->eyes_any);
	if (os_thread_init(&core->ctrl_thread) != 0 || os_thread_start(&core->ctrl_thread, ctrl_main, core) != 0) {
		struct conv_core *c = core;
		conv_core_destroy(&c);
		return XRT_ERROR_THREADING_INIT_FAILURE;
	}
	core->ctrl_started = true;
	*out_core = core;
	return XRT_SUCCESS;
}

void
conv_core_set_gpu(struct conv_core *core, const struct conv_core_gpu *gpu)
{
	if (core == NULL) {
		return;
	}
	os_mutex_lock(&core->streams_mtx);
	os_mutex_lock(&core->registry_mtx);
	if (gpu != NULL) {
		core->cfg.viewsyn = gpu->viewsyn;
		core->cfg.readback = gpu->readback;
		if (gpu->nominal_z_m > 0.0f) {
			core->cfg.nominal_z_m = gpu->nominal_z_m;
		}
	} else {
		memset(&core->cfg.viewsyn, 0, sizeof(core->cfg.viewsyn));
		memset(&core->cfg.readback, 0, sizeof(core->cfg.readback));
	}
	core->conv_gen++; // composed rows appear / disappear with the view synthesis
	os_mutex_unlock(&core->registry_mtx);
	os_mutex_unlock(&core->streams_mtx);
}

void
conv_core_destroy(struct conv_core **core_ptr)
{
	if (core_ptr == NULL || *core_ptr == NULL) {
		return;
	}
	struct conv_core *core = *core_ptr;
	*core_ptr = NULL;

	os_mutex_lock(&core->registry_mtx);
	core->stop = true;
	for (uint32_t p = 0; p < CONV_REGISTRY_MAX_PROVIDERS; p++) {
		for (uint32_t i = 0; i < XRT_CONV_MAX_CAPABILITIES; i++) {
			core->hosts[p].warms[i].cancel = true;
		}
	}
	os_cond_broadcast(&core->ctrl_cond);
	os_mutex_unlock(&core->registry_mtx);
	// Satellite hosts first (#2056): once the control thread is gone nothing
	// watches a hung host, so a lane stuck mid-convert on one would block its
	// join forever. Shutting the proxy down wakes that convert with a
	// host-lost result. In-service hosts have no begin_shutdown (no-op).
	for (uint32_t p = 0; p < CONV_REGISTRY_MAX_PROVIDERS; p++) {
		conv_host_begin_shutdown(core->hosts[p].host);
	}
	if (core->ctrl_started) {
		os_thread_join(&core->ctrl_thread);
		os_thread_destroy(&core->ctrl_thread);
	}

	bool any_hung = false;
	for (uint32_t p = 0; p < CONV_REGISTRY_MAX_PROVIDERS; p++) {
		any_hung = any_hung || core->hosts[p].hung;
		if (!core->hosts[p].hung) {
			conv_stream_lane_stop(core, p);
			conv_job_lane_stop(core, p);
		}
	}
	conv_streams_destroy_all(core);
	conv_jobs_destroy_all(core);
	for (uint32_t p = 0; p < CONV_REGISTRY_MAX_PROVIDERS; p++) {
		conv_host_destroy(&core->hosts[p].host); // a hung host leaks itself
	}
	if (any_hung) {
		// A dispatcher may still be waiting inside an abandoned provider call
		// and points at the core: leak it (in-service hangs last until restart).
		return;
	}
	for (uint32_t i = 0; i < CONV_CORE_MAX_DPS; i++) {
		conv_eyes_slot_fini(&core->eyes[i]);
	}
	conv_eyes_slot_fini(&core->eyes_any);
	os_mutex_destroy(&core->eyes_mtx);
	os_cond_destroy(&core->ctrl_cond);
	os_mutex_destroy(&core->evq_mtx);
	os_mutex_destroy(&core->jobs_mtx);
	os_mutex_destroy(&core->streams_mtx);
	os_mutex_destroy(&core->registry_mtx);
	free(core);
}
