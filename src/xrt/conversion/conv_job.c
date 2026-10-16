// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Conversion job lane: one worker per host, progress, cancel, staging.
 * @ingroup aux_conversion
 */

#include "conv_core_internal.h"

#include "os/os_time.h"
#include "util/u_logging.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

//! Worker wait while idle or while the provider is not READY.
#define CONV_JOB_WAIT_NS (100ull * 1000000ull)

static uint64_t
now_ns(void)
{
	return os_monotonic_get_ns();
}

static void
emit_locked(struct conv_core *core, struct conv_job *j)
{
	struct xrt_conv_wire_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.struct_size = (uint32_t)sizeof(ev);
	ev.family_version = XRT_CONV_WIRE_FAMILY_VERSION_CURRENT;
	ev.kind = XRT_CONV_WIRE_EVENT_JOB_STATE_CHANGED;
	ev.task = j->task;
	ev.state = j->status;
	ev.failure_reason = j->failure_reason;
	ev.progress = j->progress;
	ev.job_id = j->id;
	j->last_emit_ns = now_ns();
	j->last_emit_progress = j->progress;
	conv_core_emit(core, j->client_id, &ev);
}

static void
set_status_locked(struct conv_core *core, struct conv_job *j, uint32_t status)
{
	if (j->status == status) {
		return;
	}
	j->status = status;
	emit_locked(core, j); // state changes always go out
	// Enqueue / start / finish move the row's `jobs_queued` column (#2029);
	// jobs_mtx -> registry_mtx is the lock order.
	conv_core_activity_changed(core);
}

static struct conv_job *
find_locked(struct conv_core *core, uint64_t id)
{
	for (struct conv_job *j = core->jobs; j != NULL; j = j->next) {
		if (j->id == id && !j->destroyed) {
			return j;
		}
	}
	return NULL;
}

static void
unlink_locked(struct conv_core *core, struct conv_job *j)
{
	struct conv_job **pp = &core->jobs;
	while (*pp != NULL && *pp != j) {
		pp = &(*pp)->next;
	}
	if (*pp == j) {
		*pp = j->next;
	}
	j->next = NULL;
}

static void
free_inputs(struct conv_job *j)
{
	for (uint32_t i = 0; i < XRT_CONV_MAX_FRAME_IMAGES; i++) {
		free(j->owned[i]);
		j->owned[i] = NULL;
	}
}

static void
job_free(struct conv_job *j)
{
	free_inputs(j);
	conv_stage_fini(&j->stage);
	free(j);
}

static bool
is_waiting(const struct conv_job *j)
{
	return j->status == CONV_JOB_QUEUED || j->status == CONV_JOB_WAITING_FOR_PROVIDER;
}

static bool
is_finished(const struct conv_job *j)
{
	return j->status == CONV_JOB_SUCCEEDED || j->status == CONV_JOB_FAILED || j->status == CONV_JOB_CANCELLED;
}

static bool
is_splat_task(uint32_t task)
{
	return task == XRT_CONV_TASK_SPLATS_MONO || task == XRT_CONV_TASK_SPLATS_STEREO;
}

/*! ADR-057 §4: a SPLATS_* job carries the runtime-owned splat result, zero-filled
 *  per run (a retry after a host loss starts from nothing). */
static void
job_point_splat_result(struct conv_job *j)
{
	memset(&j->splat_result, 0, sizeof(j->splat_result));
	if (is_splat_task(j->task)) {
		j->splat_result.struct_size = (uint32_t)sizeof(j->splat_result);
		j->out.splat_result = &j->splat_result;
	} else {
		j->out.splat_result = NULL;
	}
}

/*! The failure reason of a job its provider failed: a refusal it reported
 *  (ADR-057) is the input's fault or the remote's, never a load failure. */
static uint32_t
job_refusal_reason(const struct conv_job *j)
{
	switch (is_splat_task(j->task) ? j->splat_result.refusal_code : XRT_CONV_REFUSAL_NONE) {
	case XRT_CONV_REFUSAL_NONE:
	case XRT_CONV_REFUSAL_METRIC_FIT_REFUSED: return XRT_CONV_FAIL_LOAD; // not a reason to fail by itself
	case XRT_CONV_REFUSAL_REMOTE_ERROR: return XRT_CONV_FAIL_NETWORK;
	default: return XRT_CONV_FAIL_INPUT;
	}
}


/*
 *
 * Provider callbacks (provider thread, no core lock held by the caller).
 *
 */

static void
job_progress(void *ctx, float fraction, const char *stage)
{
	struct conv_job *j = (struct conv_job *)ctx;
	struct conv_core *core = j->core;
	os_mutex_lock(&core->jobs_mtx);
	const uint64_t t = now_ns();
	j->progress = isfinite(fraction) ? (fraction < 0.0f ? -1.0f : (fraction > 1.0f ? 1.0f : fraction)) : -1.0f;
	j->last_progress_ns = t;
	if (stage != NULL) {
		strncpy(j->stage_label, stage, sizeof(j->stage_label) - 1);
		j->stage_label[sizeof(j->stage_label) - 1] = '\0';
	}
	// Rate limit: 2 Hz AND 5 % steps (roadmap §11.1).
	if (t - j->last_emit_ns >= CONV_JOB_PROGRESS_MIN_INTERVAL_NS &&
	    fabsf(j->progress - j->last_emit_progress) >= CONV_JOB_PROGRESS_MIN_STEP) {
		emit_locked(core, j);
	}
	os_mutex_unlock(&core->jobs_mtx);
}

static bool
job_is_cancelled(void *ctx)
{
	struct conv_job *j = (struct conv_job *)ctx;
	os_mutex_lock(&j->core->jobs_mtx);
	const bool c = j->cancel || j->destroyed;
	os_mutex_unlock(&j->core->jobs_mtx);
	return c;
}


/*
 *
 * Worker.
 *
 */

static struct conv_job *
pick_locked(struct conv_core *core, uint32_t provider)
{
	struct conv_job *best = NULL;
	for (struct conv_job *j = core->jobs; j != NULL; j = j->next) {
		if (j->bind.provider != provider || j->destroyed || j->cancel || !is_waiting(j)) {
			continue;
		}
		if (best == NULL || j->priority > best->priority ||
		    (j->priority == best->priority && j->id < best->id)) {
			best = j;
		}
	}
	return best;
}

enum job_gate
{
	JOB_GATE_WAIT = 0, //!< WAITING_FOR_PROVIDER: warming, host down, ...
	JOB_GATE_READY,    //!< run it
	JOB_GATE_FAIL,     //!< the provider went terminal and nothing else serves the task
};

/*! The job's row is not READY (or its host is down): keep it warming; move a
 *  queued job off a terminal row (it has not started, so nothing is lost);
 *  fail it only when its provider is terminal and no other candidate serves
 *  the task (§7.4: never for readiness alone). jobs_mtx held. */
static enum job_gate
gate_locked(struct conv_core *core, struct conv_job *j, uint32_t *out_reason)
{
	os_mutex_lock(&core->registry_mtx);
	uint32_t state = conv_core_row_state_locked(core, j->bind.provider, j->bind.cap_id);
	enum job_gate g = state == XRT_CONV_STATE_READY ? JOB_GATE_READY : JOB_GATE_WAIT;
	if (g == JOB_GATE_READY && conv_host_is_down(conv_core_host(core, j->bind.provider))) {
		g = JOB_GATE_WAIT; // a satellite between incarnations (#2056): no thread waits on it
	}
	if (g != JOB_GATE_READY) {
		if (conv_state_is_terminal(state)) {
			struct conv_binding nb;
			if (conv_core_select_locked(core, j->task, XRT_CONV_LANE_JOB, j->in.input_kind, 0, NULL, &nb,
			                            NULL) <= CONV_SEL_OK_FALLTHROUGH &&
			    (nb.provider != j->bind.provider || nb.cap_id != j->bind.cap_id)) {
				j->bind = nb;
				state = conv_core_row_state_locked(core, j->bind.provider, j->bind.cap_id);
			}
		}
		if (conv_state_is_terminal(state)) {
			const int32_t ri = conv_registry_find_row(&core->reg, j->bind.provider, j->bind.cap_id);
			*out_reason = ri >= 0 && core->reg.rows[ri].rd.reason < CONV_REASON_LOCAL_BASE
			                  ? core->reg.rows[ri].rd.reason
			                  : XRT_CONV_FAIL_LOAD;
			g = JOB_GATE_FAIL;
		} else {
			conv_core_request_warm_locked(core, j->bind.provider, j->bind.cap_id, CONV_EV_CREATE_REQUESTED);
		}
	}
	os_mutex_unlock(&core->registry_mtx);
	return g;
}

static void *
job_lane_main(void *ptr)
{
	struct conv_job_lane *lane = (struct conv_job_lane *)ptr;
	struct conv_core *core = lane->core;
	os_mutex_lock(&core->jobs_mtx);
	while (!lane->stop) {
		struct conv_job *j = pick_locked(core, lane->provider);
		if (j == NULL) {
			os_cond_wait_timeout_ns(&lane->cond, &core->jobs_mtx, CONV_JOB_WAIT_NS);
			continue;
		}
		uint32_t fail_reason = 0;
		const enum job_gate gate = gate_locked(core, j, &fail_reason);
		if (gate == JOB_GATE_FAIL) {
			// The provider went terminal (crash loop, removed, ...) and no other
			// candidate serves the task: the only way a job fails before it ran.
			free_inputs(j);
			j->failure_reason = fail_reason;
			j->ended_ns = now_ns();
			set_status_locked(core, j, CONV_JOB_FAILED);
			U_LOG_W("[conv] job %llu (%s) failed before it ran: its provider is %s", (unsigned long long)j->id,
			        conv_task_name(j->task), conv_reason_name(fail_reason));
			if (j->destroyed) {
				unlink_locked(core, j);
				job_free(j);
			}
			continue;
		}
		if (gate != JOB_GATE_READY) {
			// Never a failure for readiness alone: WAITING_FOR_PROVIDER.
			set_status_locked(core, j, CONV_JOB_WAITING_FOR_PROVIDER);
			if (j->bind.provider != lane->provider) {
				// Re-selected onto another provider: its worker takes it.
				if (core->hosts[j->bind.provider].jlane.started) {
					os_cond_broadcast(&core->hosts[j->bind.provider].jlane.cond);
				}
				continue;
			}
			os_cond_wait_timeout_ns(&lane->cond, &core->jobs_mtx, CONV_JOB_WAIT_NS);
			continue;
		}
		struct conv_host *host = conv_core_host(core, j->bind.provider);
		if (host == NULL) {
			os_cond_wait_timeout_ns(&lane->cond, &core->jobs_mtx, CONV_JOB_WAIT_NS);
			continue;
		}
		j->running = true;
		j->started_ns = now_ns();
		j->last_progress_ns = j->started_ns;
		j->progress = 0.0f;
		set_status_locked(core, j, CONV_JOB_RUNNING);

		struct xrt_conv_job_control ctl;
		memset(&ctl, 0, sizeof(ctl));
		ctl.struct_size = (uint32_t)sizeof(ctl);
		ctl.sinks = conv_stage_sinks(&j->stage, &ctl.sink_count);
		ctl.ctx = j;
		ctl.progress = job_progress;
		ctl.is_cancelled = job_is_cancelled;
		memset(&j->out, 0, sizeof(j->out));
		j->out.struct_size = (uint32_t)sizeof(j->out);
		job_point_splat_result(j);
		const uint32_t provider = j->bind.provider;
		const uint32_t cap_id = j->bind.cap_id;
		const uint32_t epoch = conv_host_get_epoch(host);
		os_mutex_unlock(&core->jobs_mtx);

		struct conv_host_cmd cmd;
		memset(&cmd, 0, sizeof(cmd));
		cmd.op = CONV_OP_JOB;
		cmd.cap_id = cap_id;
		cmd.job_in = &j->in;
		cmd.params = &j->params;
		cmd.job_ctl = &ctl;
		cmd.job_out = &j->out;
		const xrt_result_t r = conv_host_call(host, &cmd);
		if (r != XRT_SUCCESS && (cmd.blocked & CONV_CMD_HOST_DOWN) != 0) {
			// Refused before it started: its host went down in the race with the
			// gate. Back to WAITING_FOR_PROVIDER; it starts after RESTORED.
			os_mutex_lock(&core->jobs_mtx);
			j->running = false;
			set_status_locked(core, j, CONV_JOB_WAITING_FOR_PROVIDER);
			continue;
		}
		// Its host process died under it (satellite): retried ONCE from its
		// original input after RESTORED, partial output discarded, same id.
		const bool host_lost = r != XRT_SUCCESS && host->hosting == CONV_HOSTING_SATELLITE &&
		                       ((cmd.blocked & CONV_CMD_HOST_LOST) != 0 || conv_host_get_epoch(host) != epoch) &&
		                       !job_is_cancelled(j);
		if (host_lost) {
			os_mutex_lock(&core->jobs_mtx);
			j->running = false;
			j->host_losses++;
			if (j->host_losses == 1 && !j->destroyed && conv_stage_reset(&j->stage)) {
				j->progress = 0.0f;
				j->last_emit_progress = 0.0f;
				memset(&j->out, 0, sizeof(j->out));
				j->out.struct_size = (uint32_t)sizeof(j->out);
				job_point_splat_result(j);
				U_LOG_W("[conv] job %llu (%s): its host process died after %.2f s; retried once on the "
				        "respawned host",
				        (unsigned long long)j->id, conv_task_name(j->task),
				        (double)(now_ns() - j->started_ns) / 1e9);
				set_status_locked(core, j, CONV_JOB_WAITING_FOR_PROVIDER);
				continue;
			}
			os_mutex_unlock(&core->jobs_mtx);
		}
		if (r != XRT_SUCCESS && !job_is_cancelled(j)) {
			conv_core_call_failed(core, provider, cap_id, r);
		}

		os_mutex_lock(&core->jobs_mtx);
		j->running = false;
		j->ended_ns = now_ns();
		j->result = r;
		free_inputs(j);
		if (j->cancel || j->destroyed) {
			j->failure_reason = XRT_CONV_FAIL_NONE;
			set_status_locked(core, j, CONV_JOB_CANCELLED);
		} else if (r == XRT_SUCCESS) {
			j->progress = 1.0f;
			set_status_locked(core, j, CONV_JOB_SUCCEEDED);
		} else {
			// A second host loss is the crash reason; a refusal the provider
			// reported (ADR-057) its own; anything else a load failure.
			j->failure_reason = host_lost ? XRT_CONV_FAIL_CRASH_LOOP : job_refusal_reason(j);
			set_status_locked(core, j, CONV_JOB_FAILED);
		}
		U_LOG_W("[conv] job %llu (%s) %s in %.2f s", (unsigned long long)j->id, conv_task_name(j->task),
		        j->status == CONV_JOB_SUCCEEDED ? "succeeded"
		                                        : (j->status == CONV_JOB_CANCELLED ? "cancelled" : "failed"),
		        (double)(j->ended_ns - j->started_ns) / 1e9);
		if (j->destroyed) {
			unlink_locked(core, j);
			job_free(j);
		}
	}
	os_mutex_unlock(&core->jobs_mtx);
	return NULL;
}

xrt_result_t
conv_job_lane_start(struct conv_core *core, uint32_t provider)
{
	struct conv_job_lane *lane = &core->hosts[provider].jlane;
	os_mutex_lock(&core->jobs_mtx);
	if (lane->started) {
		os_mutex_unlock(&core->jobs_mtx);
		return XRT_SUCCESS;
	}
	lane->core = core;
	lane->provider = provider;
	lane->stop = false;
	if (os_cond_init(&lane->cond) != 0 || os_thread_init(&lane->thread) != 0) {
		os_mutex_unlock(&core->jobs_mtx);
		return XRT_ERROR_THREADING_INIT_FAILURE;
	}
	if (os_thread_start(&lane->thread, job_lane_main, lane) != 0) {
		os_cond_destroy(&lane->cond);
		os_mutex_unlock(&core->jobs_mtx);
		return XRT_ERROR_THREADING_INIT_FAILURE;
	}
	lane->started = true;
	os_mutex_unlock(&core->jobs_mtx);
	return XRT_SUCCESS;
}

void
conv_job_lane_stop(struct conv_core *core, uint32_t provider)
{
	struct conv_job_lane *lane = &core->hosts[provider].jlane;
	os_mutex_lock(&core->jobs_mtx);
	if (!lane->started) {
		os_mutex_unlock(&core->jobs_mtx);
		return;
	}
	lane->stop = true;
	// A running job sees is_cancelled through the host's own shutdown later;
	// cancel it now so the worker returns promptly.
	for (struct conv_job *j = core->jobs; j != NULL; j = j->next) {
		if (j->bind.provider == provider && j->running) {
			j->cancel = true;
		}
	}
	os_cond_broadcast(&lane->cond);
	os_mutex_unlock(&core->jobs_mtx);
	os_thread_join(&lane->thread);
	os_thread_destroy(&lane->thread);
	os_mutex_lock(&core->jobs_mtx);
	lane->started = false;
	os_cond_destroy(&lane->cond);
	os_mutex_unlock(&core->jobs_mtx);
}

void
conv_jobs_wake(struct conv_core *core)
{
	os_mutex_lock(&core->jobs_mtx);
	for (uint32_t p = 0; p < CONV_REGISTRY_MAX_PROVIDERS; p++) {
		if (core->hosts[p].jlane.started) {
			os_cond_broadcast(&core->hosts[p].jlane.cond);
		}
	}
	os_mutex_unlock(&core->jobs_mtx);
}

uint32_t
conv_jobs_bound_count(struct conv_core *core, uint32_t provider, uint32_t cap_id)
{
	uint32_t n = 0;
	os_mutex_lock(&core->jobs_mtx);
	for (struct conv_job *j = core->jobs; j != NULL; j = j->next) {
		if (!j->destroyed && !is_finished(j) && j->bind.provider == provider && j->bind.cap_id == cap_id) {
			n++;
		}
	}
	os_mutex_unlock(&core->jobs_mtx);
	return n;
}

uint32_t
conv_jobs_row_count(struct conv_core *core, uint32_t provider, uint32_t cap_id, bool composed)
{
	uint32_t n = 0;
	os_mutex_lock(&core->jobs_mtx);
	for (struct conv_job *j = core->jobs; j != NULL; j = j->next) {
		if (!j->destroyed && !is_finished(j) && j->bind.provider == provider && j->bind.cap_id == cap_id &&
		    j->bind.composed == composed) {
			n++;
		}
	}
	os_mutex_unlock(&core->jobs_mtx);
	return n;
}

void
conv_jobs_destroy_all(struct conv_core *core)
{
	os_mutex_lock(&core->jobs_mtx);
	struct conv_job *list = core->jobs;
	core->jobs = NULL;
	os_mutex_unlock(&core->jobs_mtx);
	while (list != NULL) {
		struct conv_job *next = list->next;
		if (!list->running) {
			job_free(list);
		}
		list = next;
	}
}


/*
 *
 * Public job API.
 *
 */

static uint32_t
primary_output(uint32_t task)
{
	switch (task) {
	case XRT_CONV_TASK_SPLATS_MONO:
	case XRT_CONV_TASK_SPLATS_STEREO: return XRT_CONV_OUT_SPLAT_BLOB;
	case XRT_CONV_TASK_DEPTH_MONO:
	case XRT_CONV_TASK_DEPTH_VIDEO: return XRT_CONV_OUT_DEPTH;
	case XRT_CONV_TASK_DISPARITY_STEREO: return XRT_CONV_OUT_DISPARITY;
	default: return XRT_CONV_OUT_VIEWS;
	}
}

static uint64_t
cpu_bytes(const struct xrt_conv_image *img)
{
	if (img->byte_size != 0) {
		return img->byte_size;
	}
	return (uint64_t)img->row_stride * img->height;
}

xrt_result_t
conv_core_job_submit(struct conv_core *core,
                     uint64_t client_id,
                     const struct conv_job_create_info *info,
                     uint64_t *out_job_id)
{
	if (info == NULL || out_job_id == NULL || !conv_task_is_served(info->task) ||
	    info->image_count > XRT_CONV_MAX_FRAME_IMAGES) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	// ADR-057 §4: a baseline is validated, never defaulted (0 = unknown = no
	// metric result); a pair is declared rectified OR raw, never both.
	const float baseline = info->params.stereo.baseline_m;
	if (!(baseline >= 0.0f && baseline <= XRT_CONV_MAX_BASELINE_M) ||
	    (info->job_flags & (XRT_CONV_JOB_PAIR_RECTIFIED | XRT_CONV_JOB_PAIR_RAW)) ==
	        (XRT_CONV_JOB_PAIR_RECTIFIED | XRT_CONV_JOB_PAIR_RAW) ||
	    (info->job_flags & ~(XRT_CONV_JOB_PAIR_RECTIFIED | XRT_CONV_JOB_PAIR_RAW | XRT_CONV_JOB_FOCAL_GIVEN)) !=
	        0) {
		U_LOG_W("[conv] job refused: baseline %.4f m / job flags 0x%x invalid", (double)baseline,
		        info->job_flags);
		return XRT_ERROR_INPUT_UNSUPPORTED;
	}
	// Providers with live streams: a job is routed away from them unless
	// LANES_CONCURRENT lets it run beside the streams.
	bool busy[CONV_REGISTRY_MAX_PROVIDERS];
	for (uint32_t p = 0; p < CONV_REGISTRY_MAX_PROVIDERS; p++) {
		busy[p] = conv_streams_bound_count(core, p, 0, true) > 0;
	}

	struct conv_binding b;
	os_mutex_lock(&core->registry_mtx);
	const uint32_t sel =
	    conv_core_select_locked(core, info->task, XRT_CONV_LANE_JOB, info->input_kind, 0, busy, &b, NULL);
	if (sel > CONV_SEL_OK_FALLTHROUGH) {
		os_mutex_unlock(&core->registry_mtx);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	conv_core_request_warm_locked(core, b.provider, b.cap_id, CONV_EV_CREATE_REQUESTED);
	os_mutex_unlock(&core->registry_mtx);

	struct conv_job *j = (struct conv_job *)calloc(1, sizeof(*j));
	if (j == NULL) {
		return XRT_ERROR_ALLOCATION;
	}
	j->core = core;
	j->client_id = client_id;
	j->task = info->task;
	j->priority = info->priority != 0 ? info->priority : CONV_JOB_PRIORITY_NORMAL;
	j->bind = b;
	j->progress = -1.0f;
	j->last_emit_progress = -1.0f;
	j->created_ns = now_ns();
	j->params = info->params;
	j->params.struct_size = (uint32_t)sizeof(j->params);
	j->in.struct_size = (uint32_t)sizeof(j->in);
	j->in.input_kind = info->input_kind != 0 ? info->input_kind : XRT_CONV_INPUT_MONO;
	j->in.image_count = info->image_count;
	j->in.requested_blob_format = info->requested_blob_format;
	j->in.job_flags = info->job_flags;
	j->in.max_input_long_side = info->max_input_long_side;
	for (uint32_t i = 0; i < info->image_count; i++) {
		j->in.image_role[i] = info->image_role[i];
		j->in.images[i] = info->images[i];
		if (info->images[i].memory_kind == XRT_CONV_MEM_CPU) {
			// Decoded pixels are copied before the call returns.
			const uint64_t n = cpu_bytes(&info->images[i]);
			if (info->images[i].cpu_ptr == NULL || n == 0 || (j->owned[i] = malloc((size_t)n)) == NULL) {
				job_free(j);
				return XRT_ERROR_ALLOCATION;
			}
			memcpy(j->owned[i], info->images[i].cpu_ptr, (size_t)n);
			j->in.images[i].cpu_ptr = j->owned[i];
			j->in.images[i].byte_size = n;
		}
	}
	conv_stage_init(&j->stage, info->max_result_bytes);
	const uint32_t primary = primary_output(info->task);
	j->requested_outputs = info->requested_outputs != 0 ? info->requested_outputs : primary;
	conv_stage_add_output(&j->stage, primary, info->file_write, info->file_ctx);
	for (uint32_t bit = 1; bit <= XRT_CONV_OUT_SPLAT_BLOB; bit <<= 1) {
		if ((j->requested_outputs & bit) != 0 && bit != primary) {
			conv_stage_add_output(&j->stage, bit, NULL, NULL);
		}
	}

	os_mutex_lock(&core->jobs_mtx);
	uint32_t queued = 0;
	for (struct conv_job *it = core->jobs; it != NULL; it = it->next) {
		if (it->client_id == client_id && !it->destroyed && (is_waiting(it) || it->running)) {
			queued++;
		}
	}
	if (queued >= CONV_JOB_MAX_QUEUED_PER_CLIENT) {
		os_mutex_unlock(&core->jobs_mtx);
		job_free(j);
		return XRT_ERROR_CLIENT_LIMIT_REACHED;
	}
	j->id = core->next_job_id++;
	j->next = core->jobs;
	core->jobs = j;
	set_status_locked(core, j, CONV_JOB_QUEUED);
	if (core->hosts[b.provider].jlane.started) {
		os_cond_broadcast(&core->hosts[b.provider].jlane.cond);
	}
	*out_job_id = j->id;
	os_mutex_unlock(&core->jobs_mtx);
	return XRT_SUCCESS;
}

xrt_result_t
conv_core_job_status(struct conv_core *core, uint64_t job_id, struct conv_job_status *out)
{
	os_mutex_lock(&core->jobs_mtx);
	struct conv_job *j = find_locked(core, job_id);
	if (j == NULL) {
		os_mutex_unlock(&core->jobs_mtx);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	memset(out, 0, sizeof(*out));
	out->job_id = j->id;
	out->status = j->status;
	out->failure_reason = j->failure_reason;
	out->progress = j->progress;
	const uint64_t end = is_finished(j) ? j->ended_ns : now_ns();
	out->elapsed_ns = j->started_ns != 0 && end > j->started_ns ? end - j->started_ns : 0;
	memcpy(out->stage, j->stage_label, sizeof(out->stage));
	if (j->status == CONV_JOB_SUCCEEDED) {
		for (uint32_t i = 0; i < j->stage.count; i++) {
			out->output_count += conv_stage_size(&j->stage, i) > 0 ? 1u : 0u;
		}
	}
	os_mutex_unlock(&core->jobs_mtx);
	return XRT_SUCCESS;
}

xrt_result_t
conv_core_job_result(struct conv_core *core,
                     uint64_t job_id,
                     uint32_t output_index,
                     struct xrt_conv_wire_job_result *out)
{
	os_mutex_lock(&core->jobs_mtx);
	struct conv_job *j = find_locked(core, job_id);
	if (j == NULL || j->status != CONV_JOB_SUCCEEDED || output_index >= j->stage.count) {
		os_mutex_unlock(&core->jobs_mtx);
		return j == NULL ? XRT_ERROR_FEATURE_NOT_SUPPORTED : XRT_ERROR_NO_IMAGE_AVAILABLE;
	}
	xrt_conv_wire_pack_job_result(j->id, j->stage.outs[output_index].blob_format,
	                              conv_stage_delivery(&j->stage, output_index),
	                              conv_stage_size(&j->stage, output_index), &j->out, out);
	os_mutex_unlock(&core->jobs_mtx);
	return XRT_SUCCESS;
}

xrt_result_t
conv_core_job_read(struct conv_core *core,
                   uint64_t job_id,
                   uint32_t output_index,
                   uint64_t offset,
                   void *buf,
                   uint64_t capacity,
                   uint64_t *out_count,
                   uint64_t *out_total)
{
	os_mutex_lock(&core->jobs_mtx);
	struct conv_job *j = find_locked(core, job_id);
	if (j == NULL || j->status != CONV_JOB_SUCCEEDED || output_index >= j->stage.count) {
		os_mutex_unlock(&core->jobs_mtx);
		return j == NULL ? XRT_ERROR_FEATURE_NOT_SUPPORTED : XRT_ERROR_NO_IMAGE_AVAILABLE;
	}
	if (out_total != NULL) {
		*out_total = conv_stage_size(&j->stage, output_index);
	}
	const uint64_t n = conv_stage_read(&j->stage, output_index, offset, buf, capacity);
	if (out_count != NULL) {
		*out_count = n;
	}
	os_mutex_unlock(&core->jobs_mtx);
	return XRT_SUCCESS;
}

xrt_result_t
conv_core_job_splat_result(struct conv_core *core, uint64_t job_id, struct xrt_conv_splat_result *out)
{
	if (out == NULL || out->struct_size < sizeof(uint32_t)) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	os_mutex_lock(&core->jobs_mtx);
	struct conv_job *j = find_locked(core, job_id);
	if (j == NULL) {
		os_mutex_unlock(&core->jobs_mtx);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	if (!is_splat_task(j->task) || (j->status != CONV_JOB_SUCCEEDED && j->status != CONV_JOB_FAILED)) {
		os_mutex_unlock(&core->jobs_mtx);
		return XRT_ERROR_NO_IMAGE_AVAILABLE;
	}
	const uint32_t want = out->struct_size;
	const size_t n = want < sizeof(j->splat_result) ? want : sizeof(j->splat_result);
	memcpy(out, &j->splat_result, n);
	out->struct_size = (uint32_t)n;
	os_mutex_unlock(&core->jobs_mtx);
	return XRT_SUCCESS;
}

xrt_result_t
conv_core_job_cancel(struct conv_core *core, uint64_t job_id)
{
	os_mutex_lock(&core->jobs_mtx);
	struct conv_job *j = find_locked(core, job_id);
	if (j == NULL) {
		os_mutex_unlock(&core->jobs_mtx);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	j->cancel = true;
	if (is_waiting(j)) {
		// Never started: cancelled at once.
		j->ended_ns = now_ns();
		free_inputs(j);
		set_status_locked(core, j, CONV_JOB_CANCELLED);
	}
	os_mutex_unlock(&core->jobs_mtx);
	return XRT_SUCCESS;
}

xrt_result_t
conv_core_job_destroy(struct conv_core *core, uint64_t job_id)
{
	os_mutex_lock(&core->jobs_mtx);
	struct conv_job *j = find_locked(core, job_id);
	if (j == NULL) {
		os_mutex_unlock(&core->jobs_mtx);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	const bool was_counted = !is_finished(j);
	j->destroyed = true;
	j->cancel = true;
	if (!j->running) {
		unlink_locked(core, j);
		job_free(j);
	}
	if (was_counted) {
		conv_core_activity_changed(core); // a queued job left the `jobs_queued` column
	}
	os_mutex_unlock(&core->jobs_mtx);
	return XRT_SUCCESS;
}

void
conv_core_client_gone(struct conv_core *core, uint64_t client_id)
{
	conv_streams_client_gone(core, client_id);
	bool counted = false;
	os_mutex_lock(&core->jobs_mtx);
	struct conv_job **pp = &core->jobs;
	while (*pp != NULL) {
		struct conv_job *j = *pp;
		if (j->client_id != client_id) {
			pp = &j->next;
			continue;
		}
		counted = counted || (!j->destroyed && !is_finished(j));
		j->destroyed = true;
		j->cancel = true;
		if (j->running) {
			pp = &j->next; // its worker frees it
			continue;
		}
		*pp = j->next;
		job_free(j);
	}
	if (counted) {
		conv_core_activity_changed(core);
	}
	os_mutex_unlock(&core->jobs_mtx);
}
