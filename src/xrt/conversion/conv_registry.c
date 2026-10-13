// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Conversion registry + describe cache (pure).
 * @ingroup aux_conversion
 */

#include "conv_registry.h"

#include <string.h>

// xrt_conv_provider_status::state shares xrt_plugin_platform_state values.
#define CONV_PLATFORM_STATE_PLATFORM_ABSENT 2u
#define CONV_PLATFORM_STATE_INCOMPATIBLE 5u

#define CONV_DESCRIBE_MAX_WALKS 3

/*
 *
 * Helpers.
 *
 */

static void
copy_str(char *dst, size_t dst_size, const char *src)
{
	if (src == NULL) {
		dst[0] = '\0';
		return;
	}
	size_t n = strlen(src);
	if (n >= dst_size) {
		n = dst_size - 1;
	}
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static struct conv_readiness_event
make_event(uint32_t type, uint64_t now_ns)
{
	struct conv_readiness_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.now_ns = now_ns;
	return ev;
}

static void
remove_row(struct conv_registry *reg, uint32_t index)
{
	for (uint32_t i = index + 1; i < reg->row_count; i++) {
		reg->rows[i - 1] = reg->rows[i];
	}
	reg->row_count--;
}

/*
 *
 * Providers.
 *
 */

void
conv_registry_init(struct conv_registry *reg)
{
	memset(reg, 0, sizeof(*reg));
}

int32_t
conv_registry_find_provider(const struct conv_registry *reg, const char *id)
{
	if (id == NULL) {
		return -1;
	}
	for (uint32_t i = 0; i < reg->provider_count; i++) {
		if (strcmp(reg->providers[i].id, id) == 0) {
			return (int32_t)i;
		}
	}
	return -1;
}

int32_t
conv_registry_add(struct conv_registry *reg, const struct conv_provider_desc *desc)
{
	if (desc == NULL || desc->id == NULL || desc->id[0] == '\0' || strlen(desc->id) >= CONV_ID_MAX) {
		return -1;
	}
	int32_t idx = conv_registry_find_provider(reg, desc->id);
	if (idx < 0) {
		if (reg->provider_count >= CONV_REGISTRY_MAX_PROVIDERS) {
			return -1;
		}
		idx = (int32_t)reg->provider_count++;
		memset(&reg->providers[idx], 0, sizeof(reg->providers[idx]));
		reg->providers[idx].usable = true;
	}
	struct conv_provider *p = &reg->providers[idx];
	copy_str(p->id, sizeof(p->id), desc->id);
	p->probe_order = desc->probe_order;
	p->hosting = desc->hosting;
	copy_str(p->supersedes, sizeof(p->supersedes), desc->supersedes);
	p->enabled = desc->enabled;
	p->flags = desc->flags;
	copy_str(p->binary_path, sizeof(p->binary_path), desc->binary_path);
	p->binary_size = desc->binary_size;
	p->binary_mtime = desc->binary_mtime;
	p->iface = desc->iface;
	p->inst = desc->inst;
	p->host = desc->iface == NULL ? desc->host : NULL;
	return idx;
}

void
conv_registry_set_usable(struct conv_registry *reg, uint32_t provider, bool usable)
{
	if (provider < reg->provider_count) {
		reg->providers[provider].usable = usable;
	}
}

bool
conv_registry_is_hidden(const struct conv_registry *reg, uint32_t provider)
{
	if (provider >= reg->provider_count) {
		return false;
	}
	const char *id = reg->providers[provider].id;
	for (uint32_t i = 0; i < reg->provider_count; i++) {
		if (i == provider) {
			continue; // a provider cannot hide itself
		}
		if (reg->providers[i].supersedes[0] != '\0' && strcmp(reg->providers[i].supersedes, id) == 0) {
			return true;
		}
	}
	return false;
}

bool
conv_registry_discovery_settled(const struct conv_registry *reg)
{
	for (uint32_t i = 0; i < reg->provider_count; i++) {
		const struct conv_provider *p = &reg->providers[i];
		if (!p->described && p->enabled && p->usable) {
			return false;
		}
	}
	return true;
}

int32_t
conv_registry_find_row(const struct conv_registry *reg, uint32_t provider, uint32_t cap_id)
{
	for (uint32_t i = 0; i < reg->row_count; i++) {
		if (reg->rows[i].provider == provider && reg->rows[i].cap.cap_id == cap_id) {
			return (int32_t)i;
		}
	}
	return -1;
}

/*
 *
 * Validation.
 *
 */

uint32_t
conv_cap_validate(const struct xrt_conv_capability *cap)
{
	if (cap->cap_id == 0) {
		return CONV_CAP_ERR_CAP_ID_ZERO;
	}
	if (cap->task == XRT_CONV_TASK_NONE || cap->task >= XRT_CONV_TASK_RESERVED_FIRST) {
		return CONV_CAP_ERR_TASK;
	}
	const uint32_t known_lanes = XRT_CONV_LANE_STREAM | XRT_CONV_LANE_JOB;
	if (cap->lanes == 0 || (cap->lanes & ~known_lanes) != 0) {
		return CONV_CAP_ERR_LANES;
	}
	if (cap->min_views > cap->max_views || cap->max_views > XRT_CONV_MAX_VIEWS) {
		return CONV_CAP_ERR_VIEWS;
	}
	if ((cap->task == XRT_CONV_TASK_VIEWS_MONO || cap->task == XRT_CONV_TASK_VIEWS_FROM_DEPTH) &&
	    (cap->flags & XRT_CONV_CAP_EXPLICIT_VIEWPOINTS) == 0) {
		return CONV_CAP_ERR_VIEWPOINTS;
	}
	if (cap->licence_class > XRT_CONV_LICENCE_RESEARCH_ONLY) {
		return CONV_CAP_ERR_LICENCE;
	}
	return CONV_CAP_OK;
}

uint32_t
conv_cap_validate_state(const struct xrt_conv_state *st)
{
	return conv_state_in_range(st->state) ? CONV_CAP_OK : CONV_CAP_ERR_STATE;
}

/*
 *
 * Describe.
 *
 */

static void
note_error(struct conv_provider *p, uint32_t err)
{
	if (p->bad_descriptors == 0) {
		p->first_error = err;
	}
	p->bad_descriptors++;
}

/*!
 * Replace provider @p pi's rows with @p caps (already validated), keeping the
 * readiness of every cap_id that survives.
 */
static uint32_t
install_rows(
    struct conv_registry *reg, uint32_t pi, const struct xrt_conv_capability *caps, uint32_t n, uint64_t now_ns)
{
	// Drop rows whose cap_id vanished.
	for (uint32_t i = reg->row_count; i > 0; i--) {
		struct conv_cap_row *row = &reg->rows[i - 1];
		if (row->provider != pi) {
			continue;
		}
		bool kept = false;
		for (uint32_t k = 0; k < n; k++) {
			if (caps[k].cap_id == row->cap.cap_id) {
				kept = true;
				break;
			}
		}
		if (!kept) {
			remove_row(reg, i - 1);
		}
	}

	uint32_t accepted = 0;
	for (uint32_t k = 0; k < n; k++) {
		int32_t ri = conv_registry_find_row(reg, pi, caps[k].cap_id);
		if (ri >= 0) {
			reg->rows[ri].cap = caps[k]; // refined description, same readiness
			accepted++;
			continue;
		}
		if (reg->row_count >= CONV_REGISTRY_MAX_ROWS) {
			note_error(&reg->providers[pi], CONV_CAP_ERR_COUNT);
			continue;
		}
		struct conv_cap_row *row = &reg->rows[reg->row_count++];
		memset(row, 0, sizeof(*row));
		row->provider = pi;
		row->cap = caps[k];
		conv_readiness_init(&row->rd);
		struct conv_readiness_event ev = make_event(CONV_EV_DESCRIBED, now_ns);
		conv_readiness_apply(&row->rd, &ev);
		accepted++;
	}
	return accepted;
}

//! Validate @p cap and append it to @p caps unless malformed or a duplicate.
static void
accept_cap(struct conv_provider *p,
           const struct xrt_conv_capability *cap,
           struct xrt_conv_capability *caps,
           uint32_t *n)
{
	uint32_t err = conv_cap_validate(cap);
	if (err == CONV_CAP_OK) {
		for (uint32_t k = 0; k < *n; k++) {
			if (caps[k].cap_id == cap->cap_id) {
				err = CONV_CAP_ERR_DUPLICATE_CAP_ID;
				break;
			}
		}
	}
	if (err != CONV_CAP_OK) {
		note_error(p, err);
		return;
	}
	caps[(*n)++] = *cap;
}

int32_t
conv_registry_describe(struct conv_registry *reg, uint32_t pi, uint64_t now_ns)
{
	if (pi >= reg->provider_count) {
		return -1;
	}
	struct conv_provider *p = &reg->providers[pi];
	const struct xrt_conversion_plugin_iface *iface = p->iface;
	// A satellite provider is not loaded here: its host answers the [C] calls
	// from the describe it got from the host process.
	struct conv_host *host = iface == NULL ? p->host : NULL;
	if (host == NULL && (iface == NULL || !XRT_CONV_IFACE_HAS(iface, describe_cap))) {
		return -1;
	}

	// Provider status: FALLBACK flag and platform usability.
	const bool has_status = host != NULL ? host->ops->get_provider_status != NULL
	                                     : XRT_CONV_IFACE_HAS(iface, get_provider_status);
	if (has_status) {
		struct xrt_conv_provider_status st;
		memset(&st, 0, sizeof(st));
		st.struct_size = (uint32_t)sizeof(st);
		if (host != NULL ? host->ops->get_provider_status(host, &st) : iface->get_provider_status(&st)) {
			p->platform_state = st.state;
			if ((st.flags & XRT_CONV_PROVIDER_FLAG_FALLBACK) != 0) {
				p->flags |= CONV_PROV_FALLBACK;
			}
			if (st.state == CONV_PLATFORM_STATE_PLATFORM_ABSENT ||
			    st.state == CONV_PLATFORM_STATE_INCOMPATIBLE) {
				p->usable = false;
			}
		}
	}

	const bool has_gen = host != NULL || XRT_CONV_IFACE_HAS(iface, get_generation);
	struct xrt_conv_capability caps[XRT_CONV_MAX_CAPABILITIES];
	uint32_t n = 0;
	uint64_t gen_after = 0;

	for (int walk = 0; walk < CONV_DESCRIBE_MAX_WALKS; walk++) {
		p->bad_descriptors = 0;
		p->first_error = CONV_CAP_OK;
		n = 0;

		uint64_t gen_before = !has_gen ? 0 : host != NULL ? conv_host_get_generation(host) : iface->get_generation(p->inst);

		uint32_t count = 0;
		const xrt_result_t cr = host != NULL ? conv_host_describe_cap(host, 0, NULL, &count)
		                                     : iface->describe_cap(p->inst, 0, NULL, &count);
		if (cr != XRT_SUCCESS) {
			return -1;
		}
		if (count > XRT_CONV_MAX_CAPABILITIES) {
			note_error(p, CONV_CAP_ERR_COUNT);
			count = XRT_CONV_MAX_CAPABILITIES;
		}
		for (uint32_t i = 0; i < count; i++) {
			struct xrt_conv_capability cap;
			memset(&cap, 0, sizeof(cap));
			cap.struct_size = (uint32_t)sizeof(cap);
			const xrt_result_t dr = host != NULL ? conv_host_describe_cap(host, i, &cap, NULL)
			                                     : iface->describe_cap(p->inst, i, &cap, NULL);
			if (dr != XRT_SUCCESS) {
				note_error(p, CONV_CAP_ERR_DESCRIBE_FAILED);
				continue;
			}
			cap.struct_size = (uint32_t)sizeof(cap); // the provider never owns it
			accept_cap(p, &cap, caps, &n);
		}

		gen_after = !has_gen ? 0 : host != NULL ? conv_host_get_generation(host) : iface->get_generation(p->inst);
		if (gen_after == gen_before) {
			break;
		}
	}

	uint32_t accepted = install_rows(reg, pi, caps, n, now_ns);
	p->described = true;
	p->from_cache = false;
	p->generation = gen_after;
	return (int32_t)accepted;
}

/*
 *
 * Candidates.
 *
 */

static void
fill_select_row(const struct conv_registry *reg, uint32_t ri, struct conv_select_row *out)
{
	const struct conv_cap_row *row = &reg->rows[ri];
	const struct conv_provider *p = &reg->providers[row->provider];
	memset(out, 0, sizeof(*out));
	out->provider_id = p->id;
	out->probe_order = p->probe_order;
	out->provider_flags = p->flags;
	out->provider_enabled = p->enabled;
	out->provider_usable = p->usable;
	out->hidden = conv_registry_is_hidden(reg, row->provider);
	out->composed = false;
	out->cap_id = row->cap.cap_id;
	out->task = row->cap.task;
	out->lanes = row->cap.lanes;
	out->input_kinds = row->cap.input_kinds;
	out->flags = row->cap.flags;
	out->min_views = row->cap.min_views;
	out->max_views = row->cap.max_views;
	out->licence_class = row->cap.licence_class;
	out->model_id = row->cap.model_id;
	out->state = row->rd.state;
	out->reason = row->rd.reason;
}

uint32_t
conv_registry_build_candidates(const struct conv_registry *reg,
                               uint32_t task,
                               bool include_composed,
                               struct conv_select_row *out,
                               uint32_t *out_cap_row,
                               uint32_t max)
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < reg->row_count && n < max; i++) {
		if (reg->rows[i].cap.task != task) {
			continue;
		}
		fill_select_row(reg, i, &out[n]);
		if (out_cap_row != NULL) {
			out_cap_row[n] = i;
		}
		n++;
	}
	if (!include_composed || task != XRT_CONV_TASK_VIEWS_MONO) {
		return n;
	}
	// Composed VIEWS_MONO: a stream-lane depth row + the reference view synthesis.
	for (uint32_t i = 0; i < reg->row_count && n < max; i++) {
		const struct xrt_conv_capability *c = &reg->rows[i].cap;
		if (c->task != XRT_CONV_TASK_DEPTH_MONO && c->task != XRT_CONV_TASK_DEPTH_VIDEO) {
			continue;
		}
		if ((c->lanes & XRT_CONV_LANE_STREAM) == 0) {
			continue;
		}
		fill_select_row(reg, i, &out[n]);
		out[n].composed = true;
		out[n].task = XRT_CONV_TASK_VIEWS_MONO;
		out[n].lanes = XRT_CONV_LANE_STREAM;
		out[n].min_views = 2;
		out[n].max_views = XRT_CONV_MAX_VIEWS;
		out[n].flags |= XRT_CONV_CAP_EXPLICIT_VIEWPOINTS; // the stage honours them
		if (out_cap_row != NULL) {
			out_cap_row[n] = i;
		}
		n++;
	}
	return n;
}

/*
 *
 * Describe cache.
 *
 * Layout v1 (native byte order, tagged):
 *   header (32 B): magic "DXRCONVD", u32 version, u32 endian tag 0x01020304,
 *                  u32 cap_size, u32 record_count, u32 payload_size,
 *                  u32 payload_fnv1a
 *   record: char id[64], char binary_path[512], u64 binary_size,
 *           u64 binary_mtime, u64 generation, u32 platform_state,
 *           u32 provider_flags, u32 row_count, u32 reserved,
 *           row_count × cap_size bytes of xrt_conv_capability
 *
 * cap_size makes the format survive capability growth: a reader copies
 * min(cap_size, sizeof) and zero-fills the tail.
 *
 */

static const char k_cache_magic[8] = {'D', 'X', 'R', 'C', 'O', 'N', 'V', 'D'};
#define CONV_CACHE_ENDIAN_TAG 0x01020304u
#define CONV_CACHE_HEADER_SIZE 32u
#define CONV_CACHE_RECORD_HEAD (CONV_ID_MAX + CONV_BINARY_PATH_MAX + 3u * 8u + 4u * 4u)

static uint32_t
fnv1a(const uint8_t *p, size_t n)
{
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < n; i++) {
		h ^= p[i];
		h *= 16777619u;
	}
	return h;
}

static void
put(uint8_t *buf, size_t *off, const void *src, size_t n)
{
	memcpy(buf + *off, src, n);
	*off += n;
}

static void
put_u32(uint8_t *buf, size_t *off, uint32_t v)
{
	put(buf, off, &v, sizeof(v));
}

static void
put_u64(uint8_t *buf, size_t *off, uint64_t v)
{
	put(buf, off, &v, sizeof(v));
}

static uint32_t
get_u32(const uint8_t *buf, size_t off)
{
	uint32_t v;
	memcpy(&v, buf + off, sizeof(v));
	return v;
}

static uint64_t
get_u64(const uint8_t *buf, size_t off)
{
	uint64_t v;
	memcpy(&v, buf + off, sizeof(v));
	return v;
}

static uint32_t
rows_of(const struct conv_registry *reg, uint32_t pi)
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < reg->row_count; i++) {
		if (reg->rows[i].provider == pi) {
			n++;
		}
	}
	return n;
}

size_t
conv_registry_cache_save(const struct conv_registry *reg, uint8_t *buf, size_t buf_size)
{
	const size_t cap_size = sizeof(struct xrt_conv_capability);
	size_t need = CONV_CACHE_HEADER_SIZE;
	uint32_t records = 0;
	for (uint32_t pi = 0; pi < reg->provider_count; pi++) {
		if (!reg->providers[pi].described) {
			continue;
		}
		need += CONV_CACHE_RECORD_HEAD + rows_of(reg, pi) * cap_size;
		records++;
	}
	if (buf == NULL || buf_size < need) {
		return need;
	}

	size_t off = CONV_CACHE_HEADER_SIZE;
	for (uint32_t pi = 0; pi < reg->provider_count; pi++) {
		const struct conv_provider *p = &reg->providers[pi];
		if (!p->described) {
			continue;
		}
		char id[CONV_ID_MAX];
		char path[CONV_BINARY_PATH_MAX];
		memset(id, 0, sizeof(id));
		memset(path, 0, sizeof(path));
		copy_str(id, sizeof(id), p->id);
		copy_str(path, sizeof(path), p->binary_path);
		put(buf, &off, id, sizeof(id));
		put(buf, &off, path, sizeof(path));
		put_u64(buf, &off, p->binary_size);
		put_u64(buf, &off, p->binary_mtime);
		put_u64(buf, &off, p->generation);
		put_u32(buf, &off, p->platform_state);
		put_u32(buf, &off, p->flags & CONV_PROV_FALLBACK);
		put_u32(buf, &off, rows_of(reg, pi));
		put_u32(buf, &off, 0);
		for (uint32_t i = 0; i < reg->row_count; i++) {
			if (reg->rows[i].provider == pi) {
				put(buf, &off, &reg->rows[i].cap, cap_size);
			}
		}
	}

	size_t hoff = 0;
	put(buf, &hoff, k_cache_magic, sizeof(k_cache_magic));
	put_u32(buf, &hoff, CONV_CACHE_VERSION);
	put_u32(buf, &hoff, CONV_CACHE_ENDIAN_TAG);
	put_u32(buf, &hoff, (uint32_t)cap_size);
	put_u32(buf, &hoff, records);
	put_u32(buf, &hoff, (uint32_t)(need - CONV_CACHE_HEADER_SIZE));
	put_u32(buf, &hoff, fnv1a(buf + CONV_CACHE_HEADER_SIZE, need - CONV_CACHE_HEADER_SIZE));
	return need;
}

int32_t
conv_registry_cache_load(struct conv_registry *reg, const uint8_t *buf, size_t len, uint64_t now_ns)
{
	if (buf == NULL || len < CONV_CACHE_HEADER_SIZE) {
		return CONV_CACHE_ERR_SHORT;
	}
	if (memcmp(buf, k_cache_magic, sizeof(k_cache_magic)) != 0) {
		return CONV_CACHE_ERR_MAGIC;
	}
	if (get_u32(buf, 12) != CONV_CACHE_ENDIAN_TAG) {
		return CONV_CACHE_ERR_ENDIAN;
	}
	if (get_u32(buf, 8) != CONV_CACHE_VERSION) {
		return CONV_CACHE_ERR_VERSION;
	}
	const uint32_t cap_size = get_u32(buf, 16);
	const uint32_t records = get_u32(buf, 20);
	const uint32_t payload = get_u32(buf, 24);
	if ((size_t)payload + CONV_CACHE_HEADER_SIZE > len) {
		return CONV_CACHE_ERR_SHORT;
	}
	if (fnv1a(buf + CONV_CACHE_HEADER_SIZE, payload) != get_u32(buf, 28)) {
		return CONV_CACHE_ERR_CHECKSUM;
	}
	if (cap_size < offsetof(struct xrt_conv_capability, input_kinds)) { // cap_id, task, lanes at least
		return CONV_CACHE_ERR_FORMAT;
	}

	// Pass 1: check the record structure fits the payload exactly.
	const size_t end = CONV_CACHE_HEADER_SIZE + (size_t)payload;
	size_t off = CONV_CACHE_HEADER_SIZE;
	for (uint32_t r = 0; r < records; r++) {
		if (off + CONV_CACHE_RECORD_HEAD > end) {
			return CONV_CACHE_ERR_FORMAT;
		}
		uint32_t row_count = get_u32(buf, off + CONV_ID_MAX + CONV_BINARY_PATH_MAX + 24 + 8);
		if (row_count > XRT_CONV_MAX_CAPABILITIES) {
			return CONV_CACHE_ERR_FORMAT;
		}
		off += CONV_CACHE_RECORD_HEAD + (size_t)row_count * cap_size;
		if (off > end) {
			return CONV_CACHE_ERR_FORMAT;
		}
	}
	if (off != end) {
		return CONV_CACHE_ERR_FORMAT;
	}

	// Pass 2: restore.
	int32_t restored = 0;
	off = CONV_CACHE_HEADER_SIZE;
	for (uint32_t r = 0; r < records; r++) {
		char id[CONV_ID_MAX];
		char path[CONV_BINARY_PATH_MAX];
		memcpy(id, buf + off, sizeof(id));
		id[CONV_ID_MAX - 1] = '\0';
		memcpy(path, buf + off + CONV_ID_MAX, sizeof(path));
		path[CONV_BINARY_PATH_MAX - 1] = '\0';
		size_t f = off + CONV_ID_MAX + CONV_BINARY_PATH_MAX;
		uint64_t bsize = get_u64(buf, f);
		uint64_t bmtime = get_u64(buf, f + 8);
		uint64_t gen = get_u64(buf, f + 16);
		uint32_t platform_state = get_u32(buf, f + 24);
		uint32_t pflags = get_u32(buf, f + 28);
		uint32_t row_count = get_u32(buf, f + 32);
		size_t rows_off = off + CONV_CACHE_RECORD_HEAD;
		off = rows_off + (size_t)row_count * cap_size;

		int32_t pi = conv_registry_find_provider(reg, id);
		if (pi < 0) {
			continue; // no longer registered
		}
		struct conv_provider *p = &reg->providers[pi];
		if (p->described || strcmp(p->binary_path, path) != 0 || p->binary_size != bsize ||
		    p->binary_mtime != bmtime) {
			continue; // live already, or a different binary: re-describe on demand
		}

		struct xrt_conv_capability caps[XRT_CONV_MAX_CAPABILITIES];
		uint32_t n = 0;
		p->bad_descriptors = 0;
		p->first_error = CONV_CAP_OK;
		for (uint32_t k = 0; k < row_count; k++) {
			struct xrt_conv_capability cap;
			memset(&cap, 0, sizeof(cap));
			size_t copy = cap_size < sizeof(cap) ? cap_size : sizeof(cap);
			memcpy(&cap, buf + rows_off + (size_t)k * cap_size, copy);
			cap.struct_size = (uint32_t)sizeof(cap);
			// Strings must stay terminated whatever the file says.
			cap.licence_spdx[sizeof(cap.licence_spdx) - 1] = '\0';
			cap.model_id[sizeof(cap.model_id) - 1] = '\0';
			cap.model_version[sizeof(cap.model_version) - 1] = '\0';
			cap.backend[sizeof(cap.backend) - 1] = '\0';
			cap.display_name[sizeof(cap.display_name) - 1] = '\0';
			accept_cap(p, &cap, caps, &n);
		}
		install_rows(reg, (uint32_t)pi, caps, n, now_ns);
		p->described = true;
		p->from_cache = true;
		p->generation = gen;
		p->platform_state = platform_state;
		p->flags |= pflags & CONV_PROV_FALLBACK;
		restored++;
	}
	return restored;
}
