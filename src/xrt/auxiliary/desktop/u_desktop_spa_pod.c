// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Hand-built SPA pods for the GNOME capture stream. See u_desktop_spa_pod.h.
 */

#include "u_desktop_spa_pod.h"

#include <string.h>

struct pod_writer
{
	uint8_t *buf;
	size_t cap, len;
	bool overflow;
};

static void
put(struct pod_writer *w, const void *data, size_t n)
{
	if (w->overflow || w->len + n > w->cap) {
		w->overflow = true;
		return;
	}
	memcpy(w->buf + w->len, data, n);
	w->len += n;
}

static void
put_u32(struct pod_writer *w, uint32_t v)
{
	put(w, &v, 4);
}

static void
pad8(struct pod_writer *w)
{
	static const uint8_t zero[8] = {0};
	size_t r = w->len % 8;
	if (r != 0) {
		put(w, zero, 8 - r);
	}
}

//! Patch the size field of the pod whose header starts at @p at.
static void
close_pod(struct pod_writer *w, size_t at, size_t body_end)
{
	if (w->overflow) {
		return;
	}
	uint32_t size = (uint32_t)(body_end - at - 8);
	memcpy(w->buf + at, &size, 4);
}

//! Begin an Object pod; returns where its header is.
static size_t
begin_object(struct pod_writer *w, uint32_t type, uint32_t id)
{
	size_t at = w->len;
	put_u32(w, 0); // size, patched
	put_u32(w, U_SPA_TYPE_Object);
	put_u32(w, type);
	put_u32(w, id);
	return at;
}

static void
prop_header(struct pod_writer *w, uint32_t key)
{
	put_u32(w, key);
	put_u32(w, 0); // flags
}

static void
prop_id(struct pod_writer *w, uint32_t key, uint32_t id)
{
	prop_header(w, key);
	put_u32(w, 4);
	put_u32(w, U_SPA_TYPE_Id);
	put_u32(w, id);
	pad8(w);
}

/*!
 * A Choice property of @p n values of @p vsize bytes each (child type @p vtype).
 * The Choice pod's size excludes its trailing padding.
 */
static void
prop_choice(struct pod_writer *w,
            uint32_t key,
            uint32_t choice,
            uint32_t vtype,
            uint32_t vsize,
            const uint32_t *values,
            uint32_t n_words)
{
	prop_header(w, key);
	size_t at = w->len;
	put_u32(w, 0);
	put_u32(w, U_SPA_TYPE_Choice);
	put_u32(w, choice);
	put_u32(w, 0);     // choice flags
	put_u32(w, vsize); // child pod: one value's size and type
	put_u32(w, vtype);
	put(w, values, (size_t)n_words * 4);
	close_pod(w, at, w->len);
	pad8(w);
}

size_t
u_spa_build_enum_format(uint8_t *buf, size_t cap)
{
	struct pod_writer w = {buf, cap, 0, false};
	size_t at = begin_object(&w, U_SPA_TYPE_OBJECT_Format, U_SPA_PARAM_EnumFormat);
	prop_id(&w, U_SPA_FORMAT_mediaType, U_SPA_MEDIA_TYPE_video);
	prop_id(&w, U_SPA_FORMAT_mediaSubtype, U_SPA_MEDIA_SUBTYPE_raw);
	// Enum: the first value is the default. mutter's SHM monitor streams are BGRx.
	const uint32_t fmts[] = {U_SPA_VIDEO_FORMAT_BGRx, U_SPA_VIDEO_FORMAT_BGRx, U_SPA_VIDEO_FORMAT_BGRA,
	                         U_SPA_VIDEO_FORMAT_RGBx, U_SPA_VIDEO_FORMAT_RGBA};
	prop_choice(&w, U_SPA_FORMAT_VIDEO_format, U_SPA_CHOICE_Enum, U_SPA_TYPE_Id, 4, fmts, 5);
	// Range: default, min, max.
	const uint32_t sizes[] = {1920, 1080, 1, 1, 16384, 16384};
	prop_choice(&w, U_SPA_FORMAT_VIDEO_size, U_SPA_CHOICE_Range, U_SPA_TYPE_Rectangle, 8, sizes, 6);
	const uint32_t rates[] = {30, 1, 0, 1, 1000, 1};
	prop_choice(&w, U_SPA_FORMAT_VIDEO_framerate, U_SPA_CHOICE_Range, U_SPA_TYPE_Fraction, 8, rates, 6);
	close_pod(&w, at, w.len);
	return w.overflow ? 0 : w.len;
}

size_t
u_spa_build_buffers(uint8_t *buf, size_t cap)
{
	struct pod_writer w = {buf, cap, 0, false};
	size_t at = begin_object(&w, U_SPA_TYPE_OBJECT_ParamBuffers, U_SPA_PARAM_Buffers);
	// SHM only: never DMA-BUF (no GPU import in the app's process).
	const uint32_t types[] = {(1u << U_SPA_DATA_MemPtr) | (1u << U_SPA_DATA_MemFd)};
	prop_choice(&w, U_SPA_PARAM_BUFFERS_dataType, U_SPA_CHOICE_Flags, U_SPA_TYPE_Int, 4, types, 1);
	close_pod(&w, at, w.len);
	return w.overflow ? 0 : w.len;
}

static uint32_t
rd32(const uint8_t *p)
{
	uint32_t v;
	memcpy(&v, p, 4);
	return v;
}

/*!
 * The value of a property: @p v is the value pod (header at v). For a Choice the
 * first (default) value is returned with the child type. Returns the value's
 * body pointer and sets *type / *size, or NULL.
 */
static const uint8_t *
prop_value(const uint8_t *v, const uint8_t *end, uint32_t *type, uint32_t *size)
{
	if (v + 8 > end) {
		return NULL;
	}
	uint32_t sz = rd32(v), ty = rd32(v + 4);
	if (v + 8 + sz > end) {
		return NULL;
	}
	if (ty != U_SPA_TYPE_Choice) {
		*type = ty;
		*size = sz;
		return v + 8;
	}
	if (sz < 16) {
		return NULL;
	}
	uint32_t csize = rd32(v + 16), ctype = rd32(v + 20);
	if (csize == 0 || 16 + csize > sz) {
		return NULL;
	}
	*type = ctype;
	*size = csize;
	return v + 24;
}

bool
u_spa_parse_video_format(const void *pod, size_t len, uint32_t *w, uint32_t *h, uint32_t *format)
{
	const uint8_t *p = (const uint8_t *)pod;
	if (p == NULL || len < 16) {
		return false;
	}
	uint32_t size = rd32(p), type = rd32(p + 4);
	if (type != U_SPA_TYPE_Object || 8 + (size_t)size > len || size < 8) {
		return false;
	}
	if (rd32(p + 8) != U_SPA_TYPE_OBJECT_Format) {
		return false;
	}
	const uint8_t *end = p + 8 + size;
	const uint8_t *q = p + 16;
	bool have_size = false, have_fmt = false, video = false;
	while (q + 16 <= end) {
		uint32_t key = rd32(q);
		const uint8_t *v = q + 8;
		uint32_t vsz = rd32(v);
		uint32_t vt = 0, vs = 0;
		const uint8_t *body = prop_value(v, end, &vt, &vs);
		if (body != NULL) {
			if (key == U_SPA_FORMAT_mediaType && vt == U_SPA_TYPE_Id && vs >= 4) {
				video = rd32(body) == U_SPA_MEDIA_TYPE_video;
			} else if (key == U_SPA_FORMAT_VIDEO_format && vt == U_SPA_TYPE_Id && vs >= 4) {
				*format = rd32(body);
				have_fmt = true;
			} else if (key == U_SPA_FORMAT_VIDEO_size && vt == U_SPA_TYPE_Rectangle && vs >= 8) {
				*w = rd32(body);
				*h = rd32(body + 4);
				have_size = true;
			}
		}
		size_t step = 8 + 8 + (size_t)vsz;
		step = (step + 7) & ~(size_t)7;
		q += step;
	}
	return video && have_size && have_fmt && *w > 0 && *h > 0;
}
