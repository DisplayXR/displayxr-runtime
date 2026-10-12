// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Job result staging: chunked blobs or a caller-opened file.
 * @ingroup aux_conversion
 */

#include "conv_stage.h"

#include <stdlib.h>
#include <string.h>

void
conv_stage_init(struct conv_stage *st, uint64_t max_bytes)
{
	memset(st, 0, sizeof(*st));
	st->max_bytes = max_bytes;
}

void
conv_stage_fini(struct conv_stage *st)
{
	for (uint32_t i = 0; i < st->count; i++) {
		struct conv_stage_output *o = &st->outs[i];
		for (uint32_t c = 0; c < o->chunk_count; c++) {
			free(o->chunks[c]);
		}
		free(o->chunks);
		o->chunks = NULL;
		o->chunk_count = o->chunk_cap = 0;
	}
	st->count = 0;
}

static bool
sink_begin(void *ctx, uint32_t blob_format, uint64_t size_hint)
{
	struct conv_stage_output *o = (struct conv_stage_output *)ctx;
	if (o->failed) {
		return false;
	}
	o->begun = true;
	o->blob_format = blob_format;
	o->size_hint = size_hint;
	return true;
}

static bool
grow_chunks(struct conv_stage_output *o)
{
	if (o->chunk_count < o->chunk_cap) {
		return true;
	}
	uint32_t cap = o->chunk_cap == 0 ? 8u : o->chunk_cap * 2u;
	uint8_t **n = (uint8_t **)realloc(o->chunks, sizeof(uint8_t *) * cap);
	if (n == NULL) {
		return false;
	}
	o->chunks = n;
	o->chunk_cap = cap;
	return true;
}

static bool
sink_write_impl(struct conv_stage_output *o, uint64_t max_bytes, const void *bytes, uint64_t count)
{
	if (o->failed || !o->begun) {
		o->failed = true;
		return false;
	}
	if (count == 0) {
		return true;
	}
	if (o->file_write != NULL) {
		if (!o->file_write(o->file_ctx, bytes, count)) {
			o->failed = true;
			return false;
		}
		o->size += count;
		return true;
	}
	if (max_bytes != 0 && o->size + count > max_bytes) {
		o->failed = true;
		return false;
	}
	const uint8_t *src = (const uint8_t *)bytes;
	while (count > 0) {
		const uint64_t in_chunk = o->size % CONV_STAGE_CHUNK_BYTES;
		if (in_chunk == 0 && o->size / CONV_STAGE_CHUNK_BYTES == o->chunk_count) {
			if (!grow_chunks(o)) {
				o->failed = true;
				return false;
			}
			uint8_t *c = (uint8_t *)malloc(CONV_STAGE_CHUNK_BYTES);
			if (c == NULL) {
				o->failed = true;
				return false;
			}
			o->chunks[o->chunk_count++] = c;
		}
		uint64_t n = CONV_STAGE_CHUNK_BYTES - in_chunk;
		if (n > count) {
			n = count;
		}
		memcpy(o->chunks[o->size / CONV_STAGE_CHUNK_BYTES] + in_chunk, src, (size_t)n);
		o->size += n;
		src += n;
		count -= n;
	}
	return true;
}

static bool
sink_write(void *ctx, const void *bytes, uint64_t count)
{
	struct conv_stage_output *o = (struct conv_stage_output *)ctx;
	return sink_write_impl(o, o->max_bytes, bytes, count);
}

int32_t
conv_stage_add_output(struct conv_stage *st, uint32_t role, conv_stage_file_write_fn file_write, void *file_ctx)
{
	if (st->count >= CONV_STAGE_MAX_OUTPUTS || conv_stage_find(st, role) >= 0) {
		return -1;
	}
	const uint32_t i = st->count++;
	struct conv_stage_output *o = &st->outs[i];
	memset(o, 0, sizeof(*o));
	o->role = role;
	o->file_write = file_write;
	o->file_ctx = file_ctx;
	o->max_bytes = st->max_bytes;
	struct xrt_conv_blob_sink *s = &st->sinks[i];
	memset(s, 0, sizeof(*s));
	s->struct_size = (uint32_t)sizeof(*s);
	s->role = role;
	s->ctx = o;
	s->begin = sink_begin;
	s->write = sink_write;
	return (int32_t)i;
}

struct xrt_conv_blob_sink *
conv_stage_sinks(struct conv_stage *st, uint32_t *out_count)
{
	*out_count = st->count;
	return st->sinks;
}

int32_t
conv_stage_find(const struct conv_stage *st, uint32_t role)
{
	for (uint32_t i = 0; i < st->count; i++) {
		if (st->outs[i].role == role) {
			return (int32_t)i;
		}
	}
	return -1;
}

uint64_t
conv_stage_size(const struct conv_stage *st, uint32_t index)
{
	return index < st->count ? st->outs[index].size : 0;
}

uint64_t
conv_stage_read(const struct conv_stage *st, uint32_t index, uint64_t offset, void *buf, uint64_t cap)
{
	if (index >= st->count) {
		return 0;
	}
	const struct conv_stage_output *o = &st->outs[index];
	if (o->file_write != NULL || offset >= o->size || buf == NULL) {
		return 0;
	}
	uint64_t n = o->size - offset;
	if (n > cap) {
		n = cap;
	}
	uint8_t *dst = (uint8_t *)buf;
	uint64_t done = 0;
	while (done < n) {
		const uint64_t pos = offset + done;
		const uint64_t in_chunk = pos % CONV_STAGE_CHUNK_BYTES;
		uint64_t k = CONV_STAGE_CHUNK_BYTES - in_chunk;
		if (k > n - done) {
			k = n - done;
		}
		memcpy(dst + done, o->chunks[pos / CONV_STAGE_CHUNK_BYTES] + in_chunk, (size_t)k);
		done += k;
	}
	return n;
}

uint32_t
conv_stage_delivery(const struct conv_stage *st, uint32_t index)
{
	if (index >= st->count) {
		return 0u;
	}
	const struct conv_stage_output *o = &st->outs[index];
	if (o->failed || o->size == 0) {
		return 0u; // NONE
	}
	if (o->file_write != NULL) {
		return 3u; // FILE
	}
	return o->size <= CONV_STAGE_INLINE_MAX ? 1u : 2u; // INLINE : SHM
}
