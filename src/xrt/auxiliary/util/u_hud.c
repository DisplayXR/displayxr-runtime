// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Runtime HUD overlay implementation with anti-aliased text.
 *
 * Uses stb_truetype for glyph rasterization and renders a styled
 * diagnostic panel with rounded corners, color-coded sections, and
 * separator lines. The CPU renders to an RGBA pixel buffer which the
 * compositor uploads and blits post-weave.
 *
 * @author David Fattal
 * @ingroup aux_util
 */

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include "u_hud.h"
#include "u_hud_font_ttf.h"
#include "os/os_time.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <math.h>


/*
 *
 * Defines and constants.
 *
 */

#define HUD_UPDATE_INTERVAL_NS (500ULL * 1000ULL * 1000ULL)
#define HUD_BASE_W 480
#define HUD_BASE_H 370
#define HUD_MARGIN 10
#define HUD_CORNER_RADIUS 8
#define HUD_BORDER_WIDTH 1
#define HUD_FONT_SIZE_BASE 14

//! Number of cacheable ASCII glyphs (32..126 inclusive).
#define GLYPH_COUNT 95
#define GLYPH_FIRST 32
#define GLYPH_LAST 126


/*
 *
 * Color scheme.
 *
 */

struct hud_color
{
	uint8_t r, g, b, a;
};

static const struct hud_color COLOR_BG = {25, 28, 38, 210};
static const struct hud_color COLOR_BORDER = {70, 80, 110, 255};
static const struct hud_color COLOR_TITLE = {100, 180, 255, 255};
static const struct hud_color COLOR_LABEL = {170, 175, 190, 255};
static const struct hud_color COLOR_VALUE = {255, 255, 255, 255};
static const struct hud_color COLOR_DIM = {100, 100, 120, 255};
static const struct hud_color COLOR_SEP = {50, 55, 75, 255};


/*
 *
 * Font glyph cache.
 *
 */

struct hud_glyph
{
	uint8_t *alpha; //!< Anti-aliased alpha bitmap from stbtt
	int w, h;       //!< Bitmap dimensions
	int xoff, yoff; //!< Offset from cursor to bitmap top-left
	int advance;    //!< Horizontal advance in pixels
};

struct hud_font
{
	stbtt_fontinfo info;
	struct hud_glyph glyphs[GLYPH_COUNT]; //!< ASCII 32-126
	float scale;
	int ascent;      //!< Ascent in pixels
	int line_height; //!< Line height in pixels
};


/*
 *
 * Visibility state.
 *
 */

static bool g_hud_visible = false;

void
u_hud_toggle(void)
{
	g_hud_visible = !g_hud_visible;
}

bool
u_hud_is_visible(void)
{
	return g_hud_visible;
}


/*
 *
 * HUD struct.
 *
 */

struct u_hud
{
	uint8_t *pixels; //!< RGBA pixel buffer
	uint32_t width;
	uint32_t height;
	uint32_t scale;          //!< Integer scale factor (1, 2, 3...)
	uint64_t last_update_ns; //!< Timestamp of last redraw
	bool dirty;              //!< True if pixels changed since last query
	struct hud_font font;    //!< Rasterized font cache
};


/*
 *
 * Drawing primitives.
 *
 */

/*!
 * Alpha-blend a single pixel onto the RGBA buffer.
 * Uses standard "source over" compositing.
 */
static inline void
blend_pixel(struct u_hud *hud, int x, int y, struct hud_color c)
{
	if (x < 0 || x >= (int)hud->width || y < 0 || y >= (int)hud->height) {
		return;
	}

	uint8_t *dst = hud->pixels + (y * (int)hud->width + x) * 4;

	if (c.a == 255) {
		dst[0] = c.r;
		dst[1] = c.g;
		dst[2] = c.b;
		dst[3] = 255;
		return;
	}

	if (c.a == 0) {
		return;
	}

	// Source-over alpha blend
	uint32_t sa = c.a;
	uint32_t da = dst[3];
	uint32_t inv_sa = 255 - sa;

	dst[0] = (uint8_t)((sa * c.r + inv_sa * dst[0]) / 255);
	dst[1] = (uint8_t)((sa * c.g + inv_sa * dst[1]) / 255);
	dst[2] = (uint8_t)((sa * c.b + inv_sa * dst[2]) / 255);
	dst[3] = (uint8_t)(sa + (inv_sa * da) / 255);
}

/*!
 * Draw a filled rectangle (no rounding).
 */
static void
fill_rect(struct u_hud *hud, int x0, int y0, int w, int h, struct hud_color c)
{
	int x1 = x0 + w;
	int y1 = y0 + h;

	// Clamp to buffer
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > (int)hud->width) x1 = (int)hud->width;
	if (y1 > (int)hud->height) y1 = (int)hud->height;

	for (int y = y0; y < y1; y++) {
		for (int x = x0; x < x1; x++) {
			blend_pixel(hud, x, y, c);
		}
	}
}

/*!
 * Draw a filled rounded rectangle with anti-aliased edges.
 * Uses distance-to-corner for smooth sub-pixel blending at corners.
 */
static void
fill_rounded_rect(struct u_hud *hud, int x0, int y0, int w, int h, int radius, struct hud_color c)
{
	int x1 = x0 + w;
	int y1 = y0 + h;

	for (int y = y0; y < y1; y++) {
		for (int x = x0; x < x1; x++) {
			// Distance from nearest corner center
			float dx = 0, dy = 0;
			bool in_corner = false;

			if (x < x0 + radius && y < y0 + radius) {
				// Top-left corner
				dx = (float)(x0 + radius - x);
				dy = (float)(y0 + radius - y);
				in_corner = true;
			} else if (x >= x1 - radius && y < y0 + radius) {
				// Top-right corner
				dx = (float)(x - (x1 - radius - 1));
				dy = (float)(y0 + radius - y);
				in_corner = true;
			} else if (x < x0 + radius && y >= y1 - radius) {
				// Bottom-left corner
				dx = (float)(x0 + radius - x);
				dy = (float)(y - (y1 - radius - 1));
				in_corner = true;
			} else if (x >= x1 - radius && y >= y1 - radius) {
				// Bottom-right corner
				dx = (float)(x - (x1 - radius - 1));
				dy = (float)(y - (y1 - radius - 1));
				in_corner = true;
			}

			if (in_corner) {
				float dist = sqrtf(dx * dx + dy * dy);
				if (dist > (float)radius + 0.5f) {
					continue; // Outside corner
				}
				if (dist > (float)radius - 0.5f) {
					// Anti-alias the edge
					float coverage = (float)radius + 0.5f - dist;
					struct hud_color ac = c;
					ac.a = (uint8_t)(c.a * coverage);
					blend_pixel(hud, x, y, ac);
					continue;
				}
			}

			blend_pixel(hud, x, y, c);
		}
	}
}

/*!
 * Draw a 1px rounded-rect border (outline only) with anti-aliased corners.
 */
static void
draw_rounded_border(struct u_hud *hud, int x0, int y0, int w, int h, int radius, int thickness, struct hud_color c)
{
	int x1 = x0 + w;
	int y1 = y0 + h;
	float r_outer = (float)radius;
	float r_inner = (float)(radius - thickness);
	if (r_inner < 0) r_inner = 0;

	for (int y = y0; y < y1; y++) {
		for (int x = x0; x < x1; x++) {
			bool on_edge = (x < x0 + thickness || x >= x1 - thickness ||
			                y < y0 + thickness || y >= y1 - thickness);

			float dx = 0, dy = 0;
			bool in_corner_zone = false;

			if (x < x0 + radius && y < y0 + radius) {
				dx = (float)(x0 + radius - x);
				dy = (float)(y0 + radius - y);
				in_corner_zone = true;
			} else if (x >= x1 - radius && y < y0 + radius) {
				dx = (float)(x - (x1 - radius - 1));
				dy = (float)(y0 + radius - y);
				in_corner_zone = true;
			} else if (x < x0 + radius && y >= y1 - radius) {
				dx = (float)(x0 + radius - x);
				dy = (float)(y - (y1 - radius - 1));
				in_corner_zone = true;
			} else if (x >= x1 - radius && y >= y1 - radius) {
				dx = (float)(x - (x1 - radius - 1));
				dy = (float)(y - (y1 - radius - 1));
				in_corner_zone = true;
			}

			if (in_corner_zone) {
				float dist = sqrtf(dx * dx + dy * dy);
				// Outside outer edge
				if (dist > r_outer + 0.5f) continue;
				// Inside inner edge
				if (dist < r_inner - 0.5f) continue;

				float alpha = 1.0f;
				if (dist > r_outer - 0.5f) {
					alpha = r_outer + 0.5f - dist;
				} else if (dist < r_inner + 0.5f) {
					alpha = dist - (r_inner - 0.5f);
				}
				if (alpha < 0) alpha = 0;
				if (alpha > 1) alpha = 1;

				struct hud_color ac = c;
				ac.a = (uint8_t)(c.a * alpha);
				blend_pixel(hud, x, y, ac);
			} else if (on_edge) {
				blend_pixel(hud, x, y, c);
			}
		}
	}
}

/*!
 * Draw a horizontal separator line.
 */
static void
draw_hline(struct u_hud *hud, int x0, int x1, int y, struct hud_color c)
{
	for (int x = x0; x < x1; x++) {
		blend_pixel(hud, x, y, c);
	}
}


/*
 *
 * Text rendering.
 *
 */

/*!
 * Draw a single anti-aliased glyph at the given position.
 * @param baseline_y  Y coordinate of the text baseline.
 * @return Horizontal advance in pixels.
 */
static int
draw_char_aa(struct u_hud *hud, int px, int baseline_y, char ch, struct hud_color color)
{
	int idx = (int)(unsigned char)ch - GLYPH_FIRST;
	if (idx < 0 || idx >= GLYPH_COUNT) {
		return hud->font.glyphs[0].advance; // space advance
	}

	struct hud_glyph *g = &hud->font.glyphs[idx];
	if (g->alpha == NULL || g->w == 0 || g->h == 0) {
		return g->advance;
	}

	int gx = px + g->xoff;
	int gy = baseline_y + g->yoff;

	for (int row = 0; row < g->h; row++) {
		for (int col = 0; col < g->w; col++) {
			uint8_t a = g->alpha[row * g->w + col];
			if (a == 0) continue;

			struct hud_color c = color;
			c.a = (uint8_t)((color.a * a) / 255);
			blend_pixel(hud, gx + col, gy + row, c);
		}
	}

	return g->advance;
}

/*!
 * Draw a string and return the total horizontal advance.
 */
static int
draw_string_aa(struct u_hud *hud, int px, int baseline_y, const char *str, struct hud_color color)
{
	int x = px;
	while (*str != '\0') {
		x += draw_char_aa(hud, x, baseline_y, *str, color);
		str++;
	}
	return x - px;
}

/*!
 * Measure the width of a string without drawing.
 */
static int
measure_string(struct u_hud *hud, const char *str)
{
	int w = 0;
	while (*str != '\0') {
		int idx = (int)(unsigned char)*str - GLYPH_FIRST;
		if (idx >= 0 && idx < GLYPH_COUNT) {
			w += hud->font.glyphs[idx].advance;
		} else {
			w += hud->font.glyphs[0].advance;
		}
		str++;
	}
	return w;
}


/*
 *
 * Create / destroy.
 *
 */

bool
u_hud_create(struct u_hud **out_hud, uint32_t target_width)
{
	if (out_hud == NULL) {
		return false;
	}

	// Integer scale: 1x for <=1920, 2x for <=3840, etc.
	uint32_t scale = 1;
	if (target_width > 1920) {
		scale = (target_width + 1919) / 1920;
	}

	uint32_t width = HUD_BASE_W * scale;
	uint32_t height = HUD_BASE_H * scale;

	struct u_hud *hud = (struct u_hud *)calloc(1, sizeof(struct u_hud));
	if (hud == NULL) {
		return false;
	}

	hud->pixels = (uint8_t *)calloc(width * height * 4, 1);
	if (hud->pixels == NULL) {
		free(hud);
		return false;
	}

	hud->width = width;
	hud->height = height;
	hud->scale = scale;
	hud->last_update_ns = 0;
	hud->dirty = false;

	// Initialize TrueType font
	if (!stbtt_InitFont(&hud->font.info, u_hud_font_ttf_data, 0)) {
		free(hud->pixels);
		free(hud);
		return false;
	}

	int font_size_px = HUD_FONT_SIZE_BASE * (int)scale;
	hud->font.scale = stbtt_ScaleForPixelHeight(&hud->font.info, (float)font_size_px);

	int ascent, descent, line_gap;
	stbtt_GetFontVMetrics(&hud->font.info, &ascent, &descent, &line_gap);
	hud->font.ascent = (int)(ascent * hud->font.scale + 0.5f);
	hud->font.line_height = (int)((ascent - descent + line_gap) * hud->font.scale + 0.5f);

	// Add a bit of extra line spacing for readability
	hud->font.line_height = (int)(hud->font.line_height * 1.3f);

	// Rasterize ASCII 32-126
	for (int i = 0; i < GLYPH_COUNT; i++) {
		int ch = GLYPH_FIRST + i;
		struct hud_glyph *g = &hud->font.glyphs[i];

		g->alpha = stbtt_GetCodepointBitmap(
		    &hud->font.info,
		    hud->font.scale, hud->font.scale,
		    ch,
		    &g->w, &g->h, &g->xoff, &g->yoff);

		int advance_raw, lsb;
		stbtt_GetCodepointHMetrics(&hud->font.info, ch, &advance_raw, &lsb);
		g->advance = (int)(advance_raw * hud->font.scale + 0.5f);
	}

	*out_hud = hud;
	return true;
}

void
u_hud_destroy(struct u_hud **hud_ptr)
{
	if (hud_ptr == NULL || *hud_ptr == NULL) {
		return;
	}

	struct u_hud *hud = *hud_ptr;

	// Free glyph bitmaps
	for (int i = 0; i < GLYPH_COUNT; i++) {
		if (hud->font.glyphs[i].alpha != NULL) {
			stbtt_FreeBitmap(hud->font.glyphs[i].alpha, NULL);
		}
	}

	free(hud->pixels);
	free(hud);
	*hud_ptr = NULL;
}


/*
 *
 * Layout helpers.
 *
 */

/*!
 * Draw a label + value pair. Label in gray, value in white.
 * @return Y advance (line height).
 */
static int
draw_label_value(struct u_hud *hud, int x, int baseline_y, const char *label, const char *value)
{
	int lw = draw_string_aa(hud, x, baseline_y, label, COLOR_LABEL);
	draw_string_aa(hud, x + lw, baseline_y, value, COLOR_VALUE);
	return hud->font.line_height;
}


/*
 *
 * Update / render.
 *
 */

bool
u_hud_update(struct u_hud *hud, const struct u_hud_data *data)
{
	if (hud == NULL || data == NULL) {
		return false;
	}

	// Rate limit to ~2Hz
	uint64_t now_ns = os_monotonic_get_ns();
	if (hud->last_update_ns != 0 && (now_ns - hud->last_update_ns) < HUD_UPDATE_INTERVAL_NS) {
		return false;
	}
	hud->last_update_ns = now_ns;

	int s = (int)hud->scale;
	int margin = HUD_MARGIN * s;
	int radius = HUD_CORNER_RADIUS * s;
	int border = HUD_BORDER_WIDTH * s;
	int panel_w = (int)hud->width;
	int panel_h = (int)hud->height;

	// Clear to fully transparent
	memset(hud->pixels, 0, hud->width * hud->height * 4);

	// Draw panel background (rounded rect)
	fill_rounded_rect(hud, 0, 0, panel_w, panel_h, radius, COLOR_BG);

	// Draw border
	draw_rounded_border(hud, 0, 0, panel_w, panel_h, radius, border, COLOR_BORDER);

	// Text layout
	int x = margin;
	int y = margin + hud->font.ascent; // baseline of first line
	int lh = hud->font.line_height;
	int sep_x0 = margin;
	int sep_x1 = panel_w - margin;
	char buf[80];

	// === Title ===
	if (data->device_name) {
		const char *mode_str = data->mode_3d ? "3D" : "2D";
		const char *out_str = data->output_mode ? data->output_mode : "";
		if (out_str[0] != '\0') {
			snprintf(buf, sizeof(buf), "%s (%s %s)", data->device_name, mode_str, out_str);
		} else {
			snprintf(buf, sizeof(buf), "%s (%s)", data->device_name, mode_str);
		}
		draw_string_aa(hud, x, y, buf, COLOR_TITLE);
	}
	y += lh;

	// --- Separator ---
	draw_hline(hud, sep_x0, sep_x1, y - hud->font.ascent + 2 * s, COLOR_SEP);
	y += s * 3;

	// === Performance section ===
	snprintf(buf, sizeof(buf), "%.1f    (%.1f ms)", data->fps, data->frame_time_ms);
	y += draw_label_value(hud, x, y, "FPS     ", buf);

	snprintf(buf, sizeof(buf), "%u x %u", data->render_width, data->render_height);
	y += draw_label_value(hud, x, y, "Render  ", buf);

	snprintf(buf, sizeof(buf), "%u x %u", data->window_width, data->window_height);
	y += draw_label_value(hud, x, y, "Window  ", buf);

	// --- Separator ---
	draw_hline(hud, sep_x0, sep_x1, y - hud->font.ascent + 2 * s, COLOR_SEP);
	y += s * 3;

	// === Display / Eye section ===
	snprintf(buf, sizeof(buf), "%.0f x %.0f mm", data->display_width_mm, data->display_height_mm);
	y += draw_label_value(hud, x, y, "Display ", buf);

	snprintf(buf, sizeof(buf), "%.0f, %.0f, %.0f mm", data->left_eye_x, data->left_eye_y, data->left_eye_z);
	y += draw_label_value(hud, x, y, "L Eye   ", buf);

	snprintf(buf, sizeof(buf), "%.0f, %.0f, %.0f mm", data->right_eye_x, data->right_eye_y, data->right_eye_z);
	y += draw_label_value(hud, x, y, "R Eye   ", buf);

	// --- Separator ---
	draw_hline(hud, sep_x0, sep_x1, y - hud->font.ascent + 2 * s, COLOR_SEP);
	y += s * 3;

	// === Camera / Position section ===
	snprintf(buf, sizeof(buf), "%.2f, %.2f, %.2f m", data->vdisp_x, data->vdisp_y, data->vdisp_z);
	y += draw_label_value(hud, x, y, "Pos     ", buf);

	snprintf(buf, sizeof(buf), "%.2f, %.2f, %.2f", data->forward_x, data->forward_y, data->forward_z);
	y += draw_label_value(hud, x, y, "Fwd     ", buf);

	if (data->zoom_scale > 0.0f) {
		snprintf(buf, sizeof(buf), "%.2f", data->zoom_scale);
		y += draw_label_value(hud, x, y, "Zoom    ", buf);
	}

	// --- Separator ---
	draw_hline(hud, sep_x0, sep_x1, y - hud->font.ascent + 2 * s, COLOR_SEP);
	y += s * 3;

	// === Stereo controls section ===
	{
		if (data->camera_mode) {
			snprintf(buf, sizeof(buf), "Camera [P]  IPD/Prlx:%.3f [Sh+Wh]",
			         data->cam_ipd_factor);
			draw_string_aa(hud, x, y, buf, COLOR_VALUE);
			y += lh;

			float vfov_deg = 2.0f * atanf(data->cam_half_tan_vfov) * 180.0f / 3.14159265f;
			float derived_persp = 1.0f;
			if (data->nominal_viewer_z > 0.0f && data->cam_half_tan_vfov > 0.0f) {
				derived_persp = data->screen_height_m /
				                (2.0f * data->nominal_viewer_z * data->cam_half_tan_vfov);
			}
			snprintf(buf, sizeof(buf), "Conv:%.2f dp [Wh]  vFOV:%.1f  Persp*:%.2f",
			         data->cam_convergence, vfov_deg, derived_persp);
		} else {
			snprintf(buf, sizeof(buf), "Display [P]  IPD/Prlx:%.3f [Sh+Wh]",
			         data->disp_ipd_factor);
			draw_string_aa(hud, x, y, buf, COLOR_VALUE);
			y += lh;

			snprintf(buf, sizeof(buf), "vH:%.2fm [Wh]",
			         data->disp_vHeight);
		}
		draw_string_aa(hud, x, y, buf, COLOR_VALUE);
		y += lh;
	}

	// --- Separator ---
	draw_hline(hud, sep_x0, sep_x1, y - hud->font.ascent + 2 * s, COLOR_SEP);
	y += s * 3;

	// === Key hints (dimmed) ===
	draw_string_aa(hud, x, y, "TAB=HUD  V=2D/3D  P=Cam/Disp  ESC=Quit", COLOR_DIM);

	hud->dirty = true;
	return true;
}


/*
 *
 * Accessors.
 *
 */

const uint8_t *
u_hud_get_pixels(struct u_hud *hud)
{
	if (hud == NULL) {
		return NULL;
	}
	return hud->pixels;
}

uint32_t
u_hud_get_width(struct u_hud *hud)
{
	if (hud == NULL) {
		return 0;
	}
	return hud->width;
}

uint32_t
u_hud_get_height(struct u_hud *hud)
{
	if (hud == NULL) {
		return 0;
	}
	return hud->height;
}
