// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_stylus `MODE=mouse`: the pure half — monitor pick, pixel ->
 *         glass mapping and the pen state machine. No OS calls. See the
 *         header for the controls.
 * @ingroup drv_sim_stylus
 */

#include "sim_stylus_mouse.h"

#include "math/m_mathinclude.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <strings.h>
#endif


static const int64_t kNsPerMs = 1000000LL;

static float
cfg_width(const struct sim_stylus_mouse_config *cfg)
{
	return cfg->display_width_m > 0.0f ? cfg->display_width_m : SIM_STYLUS_DISPLAY_WIDTH_FALLBACK_M;
}

static float
cfg_height(const struct sim_stylus_mouse_config *cfg)
{
	return cfg->display_height_m > 0.0f ? cfg->display_height_m : SIM_STYLUS_DISPLAY_HEIGHT_FALLBACK_M;
}

static float
clamp_height(float h)
{
	if (h < 0.0f) {
		return 0.0f;
	}
	if (h > SIM_STYLUS_MOUSE_HEIGHT_MAX_M) {
		return SIM_STYLUS_MOUSE_HEIGHT_MAX_M;
	}
	return h;
}

static float
ramp(const struct sim_stylus_mouse_config *cfg, int64_t since_ns, int64_t now_ns)
{
	if (cfg->ramp_ms <= 0) {
		return 1.0f;
	}
	const double t = (double)(now_ns - since_ns) / (double)(cfg->ramp_ms * kNsPerMs);
	if (t <= 0.0) {
		return 0.0f;
	}
	return t >= 1.0 ? 1.0f : (float)t;
}

//! Feed one button sample; true while a double tap's pulse is live.
static bool
tap_update(struct sim_stylus_mouse_tap *tap, bool down, int64_t now_ns, int64_t window_ms)
{
	if (window_ms <= 0) {
		window_ms = SIM_STYLUS_MOUSE_DOUBLE_TAP_DEFAULT_MS;
	}
	if (down && !tap->was_down) {
		if (tap->have_press && now_ns - tap->last_press_ns <= window_ms * kNsPerMs) {
			tap->pulse_until_ns = now_ns + SIM_STYLUS_MOUSE_DOUBLE_TAP_PULSE_MS * kNsPerMs;
			// A third press starts a new pair rather than chaining.
			tap->have_press = false;
		} else {
			tap->have_press = true;
			tap->last_press_ns = now_ns;
		}
	}
	tap->was_down = down;
	return now_ns < tap->pulse_until_ns;
}

//! Feed one key sample; how many steps to apply now (press, then auto-repeat).
static int
repeat_update(struct sim_stylus_mouse_repeat *r, bool down, int64_t now_ns)
{
	int steps = 0;
	if (down && !r->was_down) {
		steps = 1;
		r->next_ns = now_ns + SIM_STYLUS_MOUSE_KEY_DELAY_MS * kNsPerMs;
	} else if (down && now_ns >= r->next_ns) {
		steps = 1;
		r->next_ns += SIM_STYLUS_MOUSE_KEY_REPEAT_MS * kNsPerMs;
		if (r->next_ns <= now_ns) {
			// Sampled too rarely to keep up: one step per sample, not a burst.
			r->next_ns = now_ns + SIM_STYLUS_MOUSE_KEY_REPEAT_MS * kNsPerMs;
		}
	}
	r->was_down = down;
	return steps;
}

static void
release_all(struct sim_stylus_mouse_machine *m)
{
	m->tip_down = false;
	m->middle_down = false;
	memset(&m->front, 0, sizeof(m->front));
	memset(&m->back, 0, sizeof(m->back));
	memset(&m->up, 0, sizeof(m->up));
	memset(&m->down, 0, sizeof(m->down));
	m->home_was_down = false;
	m->have_prev = false;
	m->velocity.x = m->velocity.y = m->velocity.z = 0.0f;
}

void
sim_stylus_mouse_machine_init(struct sim_stylus_mouse_machine *m, const struct sim_stylus_mouse_config *cfg)
{
	memset(m, 0, sizeof(*m));
	m->hover_height_m =
	    clamp_height(cfg->hover_height_m >= 0.0f ? cfg->hover_height_m : SIM_STYLUS_HOVER_HEIGHT_DEFAULT_M);
}

static void
update_velocity(struct sim_stylus_mouse_machine *m, struct xrt_vec3 tip, int64_t now_ns)
{
	if (!m->have_prev || now_ns - m->prev_ns > 200 * kNsPerMs) {
		m->velocity.x = m->velocity.y = m->velocity.z = 0.0f;
		m->have_prev = true;
		m->prev_tip = tip;
		m->prev_ns = now_ns;
		return;
	}
	const int64_t dt_ns = now_ns - m->prev_ns;
	if (dt_ns < 2 * kNsPerMs) {
		return; // too close to the last sample to say anything new
	}
	const float inv = 1e9f / (float)dt_ns;
	const struct xrt_vec3 v = {(tip.x - m->prev_tip.x) * inv, (tip.y - m->prev_tip.y) * inv,
	                           (tip.z - m->prev_tip.z) * inv};
	// Light smoothing: the cursor moves in whole pixels.
	m->velocity.x = 0.5f * (m->velocity.x + v.x);
	m->velocity.y = 0.5f * (m->velocity.y + v.y);
	m->velocity.z = 0.5f * (m->velocity.z + v.z);
	m->prev_tip = tip;
	m->prev_ns = now_ns;
}

void
sim_stylus_mouse_machine_step(struct sim_stylus_mouse_machine *m,
                              const struct sim_stylus_mouse_config *cfg,
                              const struct sim_stylus_mouse_sample *s,
                              int64_t now_ns,
                              struct sim_stylus_state *out)
{
	memset(out, 0, sizeof(*out));
	out->tip.orientation.w = 1.0f;
	out->grip.orientation.w = 1.0f;

	if (!s->gate_open) {
		release_all(m);
		return; // gone: untracked, nothing pressed
	}

	// Keys first: they work wherever the cursor is.
	const bool home_edge = s->home && !m->home_was_down;
	m->home_was_down = s->home;
	if (home_edge) {
		m->docked = !m->docked;
	}
	const float step = s->shift ? SIM_STYLUS_MOUSE_HEIGHT_STEP_BIG_M : SIM_STYLUS_MOUSE_HEIGHT_STEP_M;
	const int ups = repeat_update(&m->up, s->page_up, now_ns);
	const int downs = repeat_update(&m->down, s->page_down, now_ns);
	m->hover_height_m = clamp_height(m->hover_height_m + step * (float)(ups - downs));

	if (m->docked) {
		sim_stylus_dock_pose(cfg_width(cfg), cfg_height(cfg), &out->tip, &out->grip);
		out->docked = true;
		out->tracked = true;
		// Nothing is pressed in the dock; a press held through undocking starts fresh.
		m->tip_down = false;
		m->middle_down = false;
		m->have_prev = false;
		(void)tap_update(&m->front, s->right, now_ns, cfg->double_tap_ms);
		(void)tap_update(&m->back, s->middle, now_ns, cfg->double_tap_ms);
		return;
	}

	if (!s->on_panel) {
		// Out of the tracking volume: still connected, not tracked.
		m->tip_down = false;
		m->middle_down = false;
		m->have_prev = false;
		(void)tap_update(&m->front, s->right, now_ns, cfg->double_tap_ms);
		(void)tap_update(&m->back, s->middle, now_ns, cfg->double_tap_ms);
		return;
	}

	// Tip.
	if (s->left && !m->tip_down) {
		m->tip_down = true;
		m->tip_down_ns = now_ns;
	} else if (!s->left) {
		m->tip_down = false;
	}
	struct xrt_vec3 tip = {s->x_m, s->y_m, m->tip_down ? 0.0f : m->hover_height_m};
	out->tip_force = m->tip_down ? ramp(cfg, m->tip_down_ns, now_ns) : 0.0f;
	out->contact = m->tip_down;

	// Middle cluster force: Ctrl or X1.
	const bool mid = s->ctrl || s->x1;
	if (mid && !m->middle_down) {
		m->middle_down = true;
		m->middle_down_ns = now_ns;
	} else if (!mid) {
		m->middle_down = false;
	}
	out->middle_force = m->middle_down ? ramp(cfg, m->middle_down_ns, now_ns) : 0.0f;

	out->front_click = s->right;
	out->back_click = s->middle;
	out->front_double_tap = tap_update(&m->front, s->right, now_ns, cfg->double_tap_ms);
	out->back_double_tap = tap_update(&m->back, s->middle, now_ns, cfg->double_tap_ms);
	out->system_click = s->x2;

	// Pose: a fixed lean, the barrel's back toward the viewer, down and to the hand's side.
	const float side = cfg->left_handed ? -1.0f : 1.0f;
	const float t = cfg->tilt_deg * (float)(M_PI / 180.0);
	struct xrt_vec3 back = {side * 0.6f * sinf(t), -0.8f * sinf(t), cosf(t)};
	const float n = sqrtf(back.x * back.x + back.y * back.y + back.z * back.z);
	back.x /= n;
	back.y /= n;
	back.z /= n;

	out->tip.position = tip;
	out->tip.orientation = sim_stylus_orientation_from_back(back);
	out->grip = sim_stylus_grip_from_tip(&out->tip, back);

	update_velocity(m, tip, now_ns);
	out->tip_linear_velocity = m->velocity;
	out->tracked = true;
}


/*
 *
 * Monitor selection and mapping.
 *
 */

const char *
sim_stylus_mouse_pick_rule_str(enum sim_stylus_mouse_pick_rule rule)
{
	switch (rule) {
	case SIM_STYLUS_MOUSE_PICK_SETTING: return "SCREEN setting";
	case SIM_STYLUS_MOUSE_PICK_SIZE: return "physical size match";
	case SIM_STYLUS_MOUSE_PICK_ASPECT: return "aspect match";
	case SIM_STYLUS_MOUSE_PICK_PRIMARY: return "primary (nothing matched)";
	default: return "none";
	}
}

static bool
name_matches(const char *mon_name, const char *want)
{
	// Accept the full GDI name or its tail after the last backslash ("DISPLAY2").
	const char *tail = strrchr(mon_name, '\\');
	tail = tail != NULL ? tail + 1 : mon_name;
#ifdef _WIN32
	return _stricmp(mon_name, want) == 0 || _stricmp(tail, want) == 0;
#else
	return strcasecmp(mon_name, want) == 0 || strcasecmp(tail, want) == 0;
#endif
}

static bool
is_index(const char *s)
{
	if (s == NULL || *s == '\0') {
		return false;
	}
	for (; *s != '\0'; s++) {
		if (!isdigit((unsigned char)*s)) {
			return false;
		}
	}
	return true;
}

static float
rel_err(float a, float b)
{
	if (b <= 0.0f) {
		return 1e9f;
	}
	return fabsf(a - b) / b;
}

int32_t
sim_stylus_mouse_pick_monitor(const struct sim_stylus_mouse_monitor *mons,
                              uint32_t count,
                              float display_width_m,
                              float display_height_m,
                              const char *screen,
                              enum sim_stylus_mouse_pick_rule *out_rule,
                              bool *out_setting_unmatched)
{
	*out_rule = SIM_STYLUS_MOUSE_PICK_NONE;
	*out_setting_unmatched = false;
	if (count == 0) {
		return -1;
	}

	if (screen != NULL && screen[0] != '\0') {
		if (is_index(screen)) {
			const long i = strtol(screen, NULL, 10);
			if (i >= 0 && (uint32_t)i < count) {
				*out_rule = SIM_STYLUS_MOUSE_PICK_SETTING;
				return (int32_t)i;
			}
		} else {
			for (uint32_t i = 0; i < count; i++) {
				if (name_matches(mons[i].name, screen)) {
					*out_rule = SIM_STYLUS_MOUSE_PICK_SETTING;
					return (int32_t)i;
				}
			}
		}
		*out_setting_unmatched = true;
	}

	int32_t primary = 0;
	for (uint32_t i = 0; i < count; i++) {
		if (mons[i].is_primary) {
			primary = (int32_t)i;
			break;
		}
	}

	if (display_width_m <= 0.0f || display_height_m <= 0.0f) {
		*out_rule = SIM_STYLUS_MOUSE_PICK_PRIMARY;
		return primary;
	}
	const float want_aspect = display_width_m / display_height_m;
	const float want_w_mm = display_width_m * 1000.0f;
	const float want_h_mm = display_height_m * 1000.0f;

	// 1. Physical size AND aspect: the closest size wins.
	int32_t best = -1;
	float best_err = 0.0f;
	for (uint32_t i = 0; i < count; i++) {
		const struct sim_stylus_mouse_monitor *m = &mons[i];
		if (m->width <= 0 || m->height <= 0 || m->width_mm <= 0 || m->height_mm <= 0) {
			continue;
		}
		const float aspect = (float)m->width / (float)m->height;
		const float ew = rel_err((float)m->width_mm, want_w_mm);
		const float eh = rel_err((float)m->height_mm, want_h_mm);
		if (rel_err(aspect, want_aspect) > 0.03f || ew > 0.10f || eh > 0.10f) {
			continue;
		}
		const float err = ew + eh;
		if (best < 0 || err < best_err) {
			best = (int32_t)i;
			best_err = err;
		}
	}
	if (best >= 0) {
		*out_rule = SIM_STYLUS_MOUSE_PICK_SIZE;
		return best;
	}

	// 2. Aspect alone: the closest aspect wins, the primary on a tie.
	for (uint32_t i = 0; i < count; i++) {
		const struct sim_stylus_mouse_monitor *m = &mons[i];
		if (m->width <= 0 || m->height <= 0) {
			continue;
		}
		const float err = rel_err((float)m->width / (float)m->height, want_aspect);
		if (err > 0.03f) {
			continue;
		}
		const bool better =
		    best < 0 || err < best_err - 1e-4f || (fabsf(err - best_err) <= 1e-4f && m->is_primary);
		if (better) {
			best = (int32_t)i;
			best_err = err;
		}
	}
	if (best >= 0) {
		*out_rule = SIM_STYLUS_MOUSE_PICK_ASPECT;
		return best;
	}

	*out_rule = SIM_STYLUS_MOUSE_PICK_PRIMARY;
	return primary;
}

bool
sim_stylus_mouse_map_pixel(const struct sim_stylus_mouse_monitor *mon,
                           int32_t x,
                           int32_t y,
                           float width_m,
                           float height_m,
                           float *out_x_m,
                           float *out_y_m)
{
	if (mon->width <= 0 || mon->height <= 0) {
		return false;
	}
	if (x < mon->left || y < mon->top || x >= mon->left + mon->width || y >= mon->top + mon->height) {
		return false;
	}
	// Pixel centres: the leftmost pixel maps half a pixel in from the edge.
	const float u = ((float)(x - mon->left) + 0.5f) / (float)mon->width;
	const float v = ((float)(y - mon->top) + 0.5f) / (float)mon->height;
	*out_x_m = (u - 0.5f) * width_m;
	*out_y_m = (0.5f - v) * height_m;
	return true;
}

bool
sim_stylus_mouse_parse_gate(const char *s, enum sim_stylus_mouse_gate *out)
{
	if (s == NULL) {
		return false;
	}
	if (strcmp(s, "scrolllock") == 0) {
		*out = SIM_STYLUS_MOUSE_GATE_SCROLLLOCK;
		return true;
	}
	if (strcmp(s, "none") == 0) {
		*out = SIM_STYLUS_MOUSE_GATE_NONE;
		return true;
	}
	return false;
}

bool
sim_stylus_mouse_toggle_update(struct sim_stylus_mouse_toggle *t, bool queue_toggle, bool key_down)
{
	if (!t->init) {
		t->init = true;
		t->on = queue_toggle;
		t->last_queue_toggle = queue_toggle;
		t->key_was_down = key_down;
		return t->on;
	}
	if (queue_toggle != t->last_queue_toggle) {
		// The queue caught up: it is the truth as of now.
		t->on = queue_toggle;
		t->last_queue_toggle = queue_toggle;
	} else if (key_down && !t->key_was_down) {
		t->on = !t->on;
	}
	t->key_was_down = key_down;
	return t->on;
}
