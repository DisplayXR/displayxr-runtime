// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  MJPEG -> NV12 for the V4L2 UVC capture backend — see os_uvc_mjpeg.h.
 * @ingroup aux_os
 */

#include "os/os_uvc_mjpeg.h"
#include "util/u_logging.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The built-in decoder: stb_image, JPEG only, private to this file (the CLI
// compiles its own static copy for other formats).
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#if defined(__GNUC__)
// stb's API declarations for the formats compiled out stay declared static and
// unused; GCC reports those at the end of the translation unit, so this one
// cannot be scoped with push / pop.
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif


/*
 *
 * JPEG markers.
 *
 */

//! Walk the marker segments before SOS; calls @p fn(marker, payload, payload_len). Stops when it returns true.
static bool
walk_markers(const uint8_t *p,
             size_t len,
             bool (*fn)(uint8_t marker, const uint8_t *payload, size_t n, void *ud),
             void *ud)
{
	if (p == NULL || len < 4 || p[0] != 0xFF || p[1] != 0xD8) {
		return false;
	}
	size_t i = 2;
	while (i + 1 < len) {
		if (p[i] != 0xFF) {
			return false; // not at a marker: corrupt
		}
		while (i < len && p[i] == 0xFF) {
			i++; // fill bytes
		}
		if (i >= len) {
			return false;
		}
		const uint8_t m = p[i++];
		if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
			continue; // no length
		}
		if (m == 0xD9 || i + 2 > len) {
			return false; // EOI before SOS, or truncated
		}
		const size_t seg = ((size_t)p[i] << 8) | p[i + 1];
		if (seg < 2 || i + seg > len) {
			return false;
		}
		if (fn(m, p + i + 2, seg - 2, ud)) {
			return true;
		}
		if (m == 0xDA) {
			return false; // SOS: the entropy-coded data follows
		}
		i += seg;
	}
	return false;
}

struct sof_ud
{
	uint32_t w, h;
};

static bool
find_sof(uint8_t m, const uint8_t *pl, size_t n, void *ud)
{
	const bool sof = m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC;
	if (!sof || n < 5) {
		return false;
	}
	struct sof_ud *s = (struct sof_ud *)ud;
	s->h = ((uint32_t)pl[1] << 8) | pl[2];
	s->w = ((uint32_t)pl[3] << 8) | pl[4];
	return true;
}

bool
os_uvc_mjpeg_header(const uint8_t *jpg, size_t len, uint32_t *out_w, uint32_t *out_h)
{
	struct sof_ud s = {0, 0};
	if (!walk_markers(jpg, len, find_sof, &s) || s.w == 0 || s.h == 0) {
		return false;
	}
	*out_w = s.w;
	*out_h = s.h;
	return true;
}

static bool
find_dht(uint8_t m, const uint8_t *pl, size_t n, void *ud)
{
	(void)pl;
	(void)n;
	(void)ud;
	return m == 0xC4;
}

bool
os_uvc_mjpeg_has_dht(const uint8_t *jpg, size_t len)
{
	return walk_markers(jpg, len, find_dht, NULL);
}

// JPEG Annex K.3: the standard Huffman tables (as MJPEG's AVI1 convention assumes).
static const uint8_t k_dc_lum_bits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
static const uint8_t k_dc_chr_bits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
static const uint8_t k_dc_vals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const uint8_t k_ac_lum_bits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
static const uint8_t k_ac_lum_vals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71,
    0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
    0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};
static const uint8_t k_ac_chr_bits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
static const uint8_t k_ac_chr_vals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22,
    0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
    0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36,
    0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
    0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
    0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba,
    0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

static uint8_t *
put_table(uint8_t *o, uint8_t tc_th, const uint8_t bits[16], const uint8_t *vals, size_t nvals)
{
	*o++ = tc_th;
	memcpy(o, bits, 16);
	o += 16;
	memcpy(o, vals, nvals);
	return o + nvals;
}

size_t
os_uvc_mjpeg_insert_dht(const uint8_t *jpg, size_t len, uint8_t *out, size_t cap)
{
	if (jpg == NULL || out == NULL || len < 2 || jpg[0] != 0xFF || jpg[1] != 0xD8 ||
	    cap < len + OS_UVC_MJPEG_DHT_SIZE) {
		return 0;
	}
	uint8_t *o = out;
	*o++ = 0xFF;
	*o++ = 0xD8;
	*o++ = 0xFF;
	*o++ = 0xC4;
	const size_t seg = OS_UVC_MJPEG_DHT_SIZE - 2; // the length field counts itself, not the marker
	*o++ = (uint8_t)(seg >> 8);
	*o++ = (uint8_t)seg;
	o = put_table(o, 0x00, k_dc_lum_bits, k_dc_vals, sizeof(k_dc_vals));
	o = put_table(o, 0x10, k_ac_lum_bits, k_ac_lum_vals, sizeof(k_ac_lum_vals));
	o = put_table(o, 0x01, k_dc_chr_bits, k_dc_vals, sizeof(k_dc_vals));
	o = put_table(o, 0x11, k_ac_chr_bits, k_ac_chr_vals, sizeof(k_ac_chr_vals));
	memcpy(o, jpg + 2, len - 2);
	o += len - 2;
	return (size_t)(o - out);
}


/*
 *
 * Geometry + colour.
 *
 */

void
os_uvc_mjpeg_pick_scale(const int *num,
                        const int *den,
                        uint32_t count,
                        uint32_t w,
                        uint32_t h,
                        uint32_t want_w,
                        uint32_t want_h,
                        uint32_t *out_num,
                        uint32_t *out_den)
{
	*out_num = 1;
	*out_den = 1;
	if (want_w == 0 || want_h == 0 || num == NULL || den == NULL) {
		return;
	}
	double best = 1.0;
	for (uint32_t i = 0; i < count; i++) {
		if (num[i] <= 0 || den[i] <= 0 || num[i] >= den[i]) {
			continue; // only reductions; 1/1 is the default
		}
		const uint64_t sw = ((uint64_t)w * (uint64_t)num[i] + (uint64_t)den[i] - 1) / (uint64_t)den[i];
		const uint64_t sh = ((uint64_t)h * (uint64_t)num[i] + (uint64_t)den[i] - 1) / (uint64_t)den[i];
		if (sw < want_w || sh < want_h || (sw % 4) != 0 || (sh % 2) != 0) {
			continue;
		}
		const double f = (double)num[i] / (double)den[i];
		if (f < best) {
			best = f;
			*out_num = (uint32_t)num[i];
			*out_den = (uint32_t)den[i];
		}
	}
}

static inline uint32_t
shift_of(uint32_t f)
{
	return f == 4 ? 2u : f == 2 ? 1u : 0u;
}

void
os_uvc_mjpeg_chroma_to_nv12(const uint8_t *u,
                            const uint8_t *v,
                            uint32_t pitch,
                            uint32_t hf,
                            uint32_t vf,
                            uint32_t w,
                            uint32_t h,
                            uint8_t *uv,
                            uint32_t uv_pitch)
{
	const uint32_t cw = w / 2, chh = h / 2;
	if (u == NULL || v == NULL) {
		for (uint32_t j = 0; j < chh; j++) {
			memset(uv + (size_t)j * uv_pitch, 128, (size_t)cw * 2);
		}
		return;
	}
	const uint32_t hs = shift_of(hf), vs = shift_of(vf);
	if (hs == 1 && vs == 1) { // 4:2:0: one source sample per NV12 sample
		for (uint32_t j = 0; j < chh; j++) {
			const uint8_t *ur = u + (size_t)j * pitch, *vr = v + (size_t)j * pitch;
			uint8_t *o = uv + (size_t)j * uv_pitch;
			for (uint32_t i = 0; i < cw; i++) {
				o[2 * i] = ur[i];
				o[2 * i + 1] = vr[i];
			}
		}
		return;
	}
	for (uint32_t j = 0; j < chh; j++) {
		const size_t r0 = (size_t)((2 * j) >> vs) * pitch, r1 = (size_t)((2 * j + 1) >> vs) * pitch;
		uint8_t *o = uv + (size_t)j * uv_pitch;
		for (uint32_t i = 0; i < cw; i++) {
			const uint32_t c0 = (2 * i) >> hs, c1 = (2 * i + 1) >> hs;
			o[2 * i] = (uint8_t)((u[r0 + c0] + u[r0 + c1] + u[r1 + c0] + u[r1 + c1] + 2) >> 2);
			o[2 * i + 1] = (uint8_t)((v[r0 + c0] + v[r0 + c1] + v[r1 + c0] + v[r1 + c1] + 2) >> 2);
		}
	}
}

static inline uint8_t
clamp_u8(int32_t x)
{
	return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
}

void
os_uvc_mjpeg_rgb_to_nv12(const uint8_t *rgb,
                         uint32_t rgb_pitch,
                         uint32_t w,
                         uint32_t h,
                         uint8_t *y,
                         uint32_t y_pitch,
                         uint8_t *uv,
                         uint32_t uv_pitch)
{
	// JFIF, 16.16 fixed point: Y = .299 R + .587 G + .114 B; Cb/Cr centred on 128.
	for (uint32_t r = 0; r < h; r++) {
		const uint8_t *s = rgb + (size_t)r * rgb_pitch;
		uint8_t *d = y + (size_t)r * y_pitch;
		for (uint32_t x = 0; x < w; x++) {
			const int32_t R = s[3 * x], G = s[3 * x + 1], B = s[3 * x + 2];
			d[x] = clamp_u8((19595 * R + 38470 * G + 7471 * B + 32768) >> 16);
		}
	}
	for (uint32_t j = 0; j < h / 2; j++) {
		const uint8_t *s0 = rgb + (size_t)(2 * j) * rgb_pitch, *s1 = s0 + rgb_pitch;
		uint8_t *o = uv + (size_t)j * uv_pitch;
		for (uint32_t i = 0; i < w / 2; i++) {
			const uint8_t *a = s0 + 6 * i, *b = s1 + 6 * i;
			const int32_t R = a[0] + a[3] + b[0] + b[3], G = a[1] + a[4] + b[1] + b[4],
			              B = a[2] + a[5] + b[2] + b[5]; // 4x sums
			// Cb = -.168736 R - .331264 G + .5 B; Cr = .5 R - .418688 G - .081312 B (on 4x sums: >> 18).
			o[2 * i] = clamp_u8(128 + ((-11059 * R - 21709 * G + 32768 * B + (1 << 17)) >> 18));
			o[2 * i + 1] = clamp_u8(128 + ((32768 * R - 27439 * G - 5329 * B + (1 << 17)) >> 18));
		}
	}
}


/*
 *
 * libturbojpeg, dlopen'd.
 *
 */

typedef void *tjhandle;
typedef struct
{
	int num, denom;
} tj_scaling_factor;

#define TJ_FLAG_FASTUPSAMPLE 256
#define TJ_FLAG_FASTDCT 2048
#define TJ_ERR_WARNING 0
#define TJ_CS_YCbCr 1
#define TJ_CS_GRAY 2
#define TJ_SAMP_GRAY 3

static struct
{
	pthread_once_t once;
	bool ok;
	char why[256];
	tjhandle (*init_decompress)(void);
	int (*decompress_header3)(tjhandle, const unsigned char *, unsigned long, int *, int *, int *, int *);
	int (*decompress_to_yuv_planes)(
	    tjhandle, const unsigned char *, unsigned long, unsigned char **, int, int *, int, int);
	tj_scaling_factor *(*get_scaling_factors)(int *);
	int (*destroy)(tjhandle);
	char *(*get_error_str2)(tjhandle); // optional (1.4+)
	int (*get_error_code)(tjhandle);   // optional (2.0+)
	int nfactors;
	int fnum[32], fden[32];
} g_tj = {PTHREAD_ONCE_INIT};

static void
tj_load(void)
{
	static const char *names[] = {"libturbojpeg.so.0", "libturbojpeg.so"};
	void *lib = NULL;
	const char *name = NULL;
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && lib == NULL; i++) {
		lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
		name = names[i];
		if (lib == NULL && i == 0) {
			// Report the soname's failure, not the dev-symlink fallback's.
			const char *e = dlerror();
			snprintf(g_tj.why, sizeof(g_tj.why), "%s", e != NULL ? e : "libturbojpeg.so.0 not found");
		}
	}
	if (lib == NULL) {
		return;
	}
	*(void **)&g_tj.init_decompress = dlsym(lib, "tjInitDecompress");
	*(void **)&g_tj.decompress_header3 = dlsym(lib, "tjDecompressHeader3");
	*(void **)&g_tj.decompress_to_yuv_planes = dlsym(lib, "tjDecompressToYUVPlanes");
	*(void **)&g_tj.get_scaling_factors = dlsym(lib, "tjGetScalingFactors");
	*(void **)&g_tj.destroy = dlsym(lib, "tjDestroy");
	*(void **)&g_tj.get_error_str2 = dlsym(lib, "tjGetErrorStr2");
	*(void **)&g_tj.get_error_code = dlsym(lib, "tjGetErrorCode");
	if (g_tj.init_decompress == NULL || g_tj.decompress_header3 == NULL || g_tj.decompress_to_yuv_planes == NULL ||
	    g_tj.destroy == NULL) {
		snprintf(g_tj.why, sizeof(g_tj.why), "%s lacks the TurboJPEG 1.4 API (tjDecompressToYUVPlanes)", name);
		dlclose(lib);
		return;
	}
	if (g_tj.get_scaling_factors != NULL) {
		int n = 0;
		tj_scaling_factor *f = g_tj.get_scaling_factors(&n);
		for (int i = 0; f != NULL && i < n && g_tj.nfactors < 32; i++) {
			g_tj.fnum[g_tj.nfactors] = f[i].num;
			g_tj.fden[g_tj.nfactors] = f[i].denom;
			g_tj.nfactors++;
		}
	}
	Dl_info info;
	memset(&info, 0, sizeof(info));
	if (dladdr(*(void **)&g_tj.init_decompress, &info) != 0 && info.dli_fname != NULL) {
		snprintf(g_tj.why, sizeof(g_tj.why), "%s", info.dli_fname);
	} else {
		snprintf(g_tj.why, sizeof(g_tj.why), "%s", name);
	}
	g_tj.ok = true; // the library stays loaded for the process
}

bool
os_uvc_mjpeg_turbojpeg_available(char *why, size_t why_cap)
{
	pthread_once(&g_tj.once, tj_load);
	if (why != NULL && why_cap > 0) {
		snprintf(why, why_cap, "%s", g_tj.why);
	}
	return g_tj.ok;
}


/*
 *
 * The decoder.
 *
 */

struct os_uvc_mjpeg
{
	enum os_uvc_mjpeg_impl impl; //!< TURBOJPEG or BUILTIN, resolved
	tjhandle tj;
	uint8_t *nv12; //!< the output (Y then UV)
	size_t nv12_cap;
	uint8_t *chroma; //!< libturbojpeg's planar Cb + Cr
	size_t chroma_cap;
	uint8_t *fixed; //!< a frame with the standard DHT inserted
	size_t fixed_cap;
	bool warned_corrupt;
};

static bool
grow(uint8_t **buf, size_t *cap, size_t need)
{
	if (*cap >= need) {
		return true;
	}
	uint8_t *n = (uint8_t *)realloc(*buf, need);
	if (n == NULL) {
		return false;
	}
	*buf = n;
	*cap = need;
	return true;
}

struct os_uvc_mjpeg *
os_uvc_mjpeg_create(enum os_uvc_mjpeg_impl impl)
{
	bool tj = false;
	if (impl != OS_UVC_MJPEG_BUILTIN) {
		tj = os_uvc_mjpeg_turbojpeg_available(NULL, 0);
		if (!tj && impl == OS_UVC_MJPEG_TURBOJPEG) {
			return NULL;
		}
	}
	struct os_uvc_mjpeg *d = (struct os_uvc_mjpeg *)calloc(1, sizeof(*d));
	if (d == NULL) {
		return NULL;
	}
	d->impl = OS_UVC_MJPEG_BUILTIN;
	if (tj) {
		d->tj = g_tj.init_decompress();
		if (d->tj != NULL) {
			d->impl = OS_UVC_MJPEG_TURBOJPEG;
		} else if (impl == OS_UVC_MJPEG_TURBOJPEG) {
			free(d);
			return NULL;
		}
	}
	return d;
}

const char *
os_uvc_mjpeg_name(const struct os_uvc_mjpeg *d)
{
	return d != NULL && d->impl == OS_UVC_MJPEG_TURBOJPEG ? "libturbojpeg" : "built-in (stb_image)";
}

void
os_uvc_mjpeg_destroy(struct os_uvc_mjpeg **d_ptr)
{
	if (d_ptr == NULL || *d_ptr == NULL) {
		return;
	}
	struct os_uvc_mjpeg *d = *d_ptr;
	if (d->tj != NULL) {
		g_tj.destroy(d->tj);
	}
	free(d->nv12);
	free(d->chroma);
	free(d->fixed);
	free(d);
	*d_ptr = NULL;
}

static inline uint32_t
pad_to(uint32_t x, uint32_t m)
{
	return (x + m - 1) / m * m;
}

static bool
decode_tj(struct os_uvc_mjpeg *d,
          const uint8_t *jpg,
          size_t len,
          uint32_t want_w,
          uint32_t want_h,
          struct u_stereo_uvc_raw_frame *out)
{
	int w = 0, h = 0, samp = -1, cs = -1;
	if (g_tj.decompress_header3(d->tj, jpg, (unsigned long)len, &w, &h, &samp, &cs) != 0 || w <= 0 || h <= 0) {
		return false;
	}
	static const uint8_t hfs[7] = {1, 2, 2, 1, 1, 4, 1}; // 444 422 420 GRAY 440 411 441
	static const uint8_t vfs[7] = {1, 1, 2, 1, 2, 1, 4};
	if (samp < 0 || samp > 6 || (cs != TJ_CS_YCbCr && cs != TJ_CS_GRAY)) {
		return false;
	}
	uint32_t num = 1, den = 1;
	os_uvc_mjpeg_pick_scale(g_tj.fnum, g_tj.fden, (uint32_t)g_tj.nfactors, (uint32_t)w, (uint32_t)h, want_w, want_h,
	                        &num, &den);
	const uint32_t sw = ((uint32_t)w * num + den - 1) / den, sh = ((uint32_t)h * num + den - 1) / den;
	const uint32_t ow = sw & ~3u, oh = sh & ~1u;
	if (ow < 8 || oh < 2) {
		return false;
	}
	const uint32_t hf = hfs[samp], vf = vfs[samp];
	// Luma plane padded to the MCU (libturbojpeg writes PAD(w, hf) x PAD(h, vf)), then the NV12 UV plane.
	const uint32_t ypitch = pad_to(sw, 4), yrows = pad_to(sh, 4);
	const size_t ysize = (size_t)ypitch * yrows;
	const uint32_t uvpitch = ypitch;
	if (!grow(&d->nv12, &d->nv12_cap, ysize + (size_t)uvpitch * (oh / 2))) {
		return false;
	}
	const uint32_t cpitch = pad_to(sw, 4) / hf + 4, crows = pad_to(sh, 4) / vf + 4;
	const bool gray = samp == TJ_SAMP_GRAY;
	if (!gray && !grow(&d->chroma, &d->chroma_cap, 2 * (size_t)cpitch * crows)) {
		return false;
	}
	unsigned char *planes[3] = {d->nv12, gray ? NULL : d->chroma, gray ? NULL : d->chroma + (size_t)cpitch * crows};
	int strides[3] = {(int)ypitch, (int)cpitch, (int)cpitch};
	if (g_tj.decompress_to_yuv_planes(d->tj, jpg, (unsigned long)len, planes, (int)sw, strides, (int)sh,
	                                  TJ_FLAG_FASTDCT | TJ_FLAG_FASTUPSAMPLE) != 0) {
		// A WARNING (e.g. premature end of data) still produced a picture.
		if (g_tj.get_error_code == NULL || g_tj.get_error_code(d->tj) != TJ_ERR_WARNING) {
			if (!d->warned_corrupt) {
				d->warned_corrupt = true;
				U_LOG_W("uvc capture (V4L2): libturbojpeg could not decode a frame (%s) — dropped",
				        g_tj.get_error_str2 != NULL ? g_tj.get_error_str2(d->tj) : "?");
			}
			return false;
		}
	}
	uint8_t *uv = d->nv12 + ysize;
	os_uvc_mjpeg_chroma_to_nv12(planes[1], planes[2], cpitch, hf, vf, ow, oh, uv, uvpitch);
	out->pixel = U_STEREO_UVC_PIXEL_NV12;
	out->width = ow;
	out->height = oh;
	out->planes[0] = d->nv12;
	out->planes[1] = uv;
	out->pitches[0] = ypitch;
	out->pitches[1] = uvpitch;
	return true;
}

static bool
decode_builtin(struct os_uvc_mjpeg *d, const uint8_t *jpg, size_t len, struct u_stereo_uvc_raw_frame *out)
{
	int w = 0, h = 0, comp = 0;
	uint8_t *rgb = stbi_load_from_memory(jpg, (int)len, &w, &h, &comp, 3);
	if (rgb == NULL) {
		if (!d->warned_corrupt) {
			d->warned_corrupt = true;
			U_LOG_W("uvc capture (V4L2): the built-in decoder could not decode a frame (%s) — dropped",
			        stbi_failure_reason() != NULL ? stbi_failure_reason() : "?");
		}
		return false;
	}
	const uint32_t ow = (uint32_t)w & ~3u, oh = (uint32_t)h & ~1u;
	const size_t ysize = (size_t)ow * oh;
	if (ow < 8 || oh < 2 || !grow(&d->nv12, &d->nv12_cap, ysize + ysize / 2)) {
		stbi_image_free(rgb);
		return false;
	}
	os_uvc_mjpeg_rgb_to_nv12(rgb, (uint32_t)w * 3, ow, oh, d->nv12, ow, d->nv12 + ysize, ow);
	stbi_image_free(rgb);
	out->pixel = U_STEREO_UVC_PIXEL_NV12;
	out->width = ow;
	out->height = oh;
	out->planes[0] = d->nv12;
	out->planes[1] = d->nv12 + ysize;
	out->pitches[0] = ow;
	out->pitches[1] = ow;
	return true;
}

bool
os_uvc_mjpeg_decode(struct os_uvc_mjpeg *d,
                    const uint8_t *jpg,
                    size_t len,
                    uint32_t want_w,
                    uint32_t want_h,
                    struct u_stereo_uvc_raw_frame *out)
{
	if (d == NULL || jpg == NULL || len < 4 || len > (size_t)0x7fffffff || out == NULL) {
		return false;
	}
	// Some devices pad a frame after EOI, some end it early; both decoders
	// cope. A frame without Huffman tables gets the standard ones.
	if (!os_uvc_mjpeg_has_dht(jpg, len)) {
		if (!grow(&d->fixed, &d->fixed_cap, len + OS_UVC_MJPEG_DHT_SIZE)) {
			return false;
		}
		size_t n = os_uvc_mjpeg_insert_dht(jpg, len, d->fixed, d->fixed_cap);
		if (n == 0) {
			return false;
		}
		jpg = d->fixed;
		len = n;
	}
	return d->impl == OS_UVC_MJPEG_TURBOJPEG ? decode_tj(d, jpg, len, want_w, want_h, out)
	                                         : decode_builtin(d, jpg, len, out);
}
