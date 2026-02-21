// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Runtime HUD overlay implementation.
 * @author David Fattal
 * @ingroup aux_util
 */

#include "u_hud.h"
#include "u_hud_font.h"
#include "os/os_time.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

/*
 * Glyph dimensions from the embedded font.
 */
#define GLYPH_W 8
#define GLYPH_H 16

/*
 * Minimum interval between HUD redraws (500ms = ~2Hz).
 */
#define HUD_UPDATE_INTERVAL_NS (500ULL * 1000ULL * 1000ULL)

/*
 * Margin in pixels around text content.
 */
#define HUD_MARGIN 6

/*
 * Global visibility flag. Single bool, safe for single-writer (TAB key thread)
 * and single-reader (compositor render thread) without atomics.
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
 * Base HUD dimensions at 1x scale (designed for ~1920px displays).
 */
#define HUD_BASE_W 480
#define HUD_BASE_H 304

struct u_hud
{
	uint8_t *pixels; //!< RGBA pixel buffer
	uint32_t width;
	uint32_t height;
	uint32_t scale;          //!< Integer scale factor (1, 2, 3...)
	uint64_t last_update_ns; //!< Timestamp of last redraw
	bool dirty;              //!< True if pixels changed since last query
};

bool
u_hud_create(struct u_hud **out_hud, uint32_t target_width)
{
	if (out_hud == NULL) {
		return false;
	}

	// Integer scale: 1x for <=1920, 2x for <=3840, etc.
	uint32_t scale = 1;
	if (target_width > 1920) {
		scale = (target_width + 1919) / 1920; // ceiling division
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
	free(hud->pixels);
	free(hud);
	*hud_ptr = NULL;
}

/*!
 * Draw a single character at (px, py) in the pixel buffer.
 * White text (255,255,255,255) on whatever background is already there.
 */
static void
draw_char(struct u_hud *hud, int px, int py, char ch)
{
	unsigned int c = (unsigned char)ch;
	if (c >= 128) {
		return;
	}

	uint32_t s = hud->scale;

	for (int row = 0; row < GLYPH_H; row++) {
		unsigned char bits = u_hud_font_data[c][row];
		for (int col = 0; col < GLYPH_W; col++) {
			if (!(bits & (0x80 >> col))) {
				continue;
			}

			// Fill a scale x scale block for this font pixel
			for (uint32_t sy = 0; sy < s; sy++) {
				int y = py + row * (int)s + (int)sy;
				if (y < 0 || y >= (int)hud->height) {
					continue;
				}
				for (uint32_t sx = 0; sx < s; sx++) {
					int x = px + col * (int)s + (int)sx;
					if (x < 0 || x >= (int)hud->width) {
						continue;
					}
					uint32_t offset = (y * hud->width + x) * 4;
					hud->pixels[offset + 0] = 255; // R
					hud->pixels[offset + 1] = 255; // G
					hud->pixels[offset + 2] = 255; // B
					hud->pixels[offset + 3] = 255; // A
				}
			}
		}
	}
}

/*!
 * Draw a string at (px, py), advancing horizontally by GLYPH_W per character.
 */
static void
draw_string(struct u_hud *hud, int px, int py, const char *str)
{
	int x = px;
	int advance = GLYPH_W * (int)hud->scale;
	while (*str != '\0') {
		draw_char(hud, x, py, *str);
		x += advance;
		str++;
	}
}

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

	// Clear to semi-transparent black background
	uint32_t total_pixels = hud->width * hud->height;
	for (uint32_t i = 0; i < total_pixels; i++) {
		hud->pixels[i * 4 + 0] = 0;   // R
		hud->pixels[i * 4 + 1] = 0;   // G
		hud->pixels[i * 4 + 2] = 0;   // B
		hud->pixels[i * 4 + 3] = 160; // A (semi-transparent)
	}

	// Format and draw text lines
	char line[80];
	int s = (int)hud->scale;
	int x = HUD_MARGIN * s;
	int y = HUD_MARGIN * s;
	int line_h = (GLYPH_H + 2) * s; // 2px spacing between lines (scaled)

	// Device name
	if (data->device_name) {
		draw_string(hud, x, y, data->device_name);
		y += line_h;
	}

	// FPS
	snprintf(line, sizeof(line), "FPS: %.0f  (%.1f ms)", data->fps, data->frame_time_ms);
	draw_string(hud, x, y, line);
	y += line_h;

	// Mode and output
	snprintf(line, sizeof(line), "Mode: %s  Output: %s", data->mode_3d ? "3D" : "2D",
	         data->output_mode ? data->output_mode : "N/A");
	draw_string(hud, x, y, line);
	y += line_h;

	// Render size
	snprintf(line, sizeof(line), "Render: %ux%u", data->render_width, data->render_height);
	draw_string(hud, x, y, line);
	y += line_h;

	// Window size
	snprintf(line, sizeof(line), "Window: %ux%u", data->window_width, data->window_height);
	draw_string(hud, x, y, line);
	y += line_h;

	// Display dimensions
	snprintf(line, sizeof(line), "Display: (%.0fx%.0f) mm", data->display_width_mm, data->display_height_mm);
	draw_string(hud, x, y, line);
	y += line_h;

	// Nominal position
	snprintf(line, sizeof(line), "Nominal: (%.0f, %.0f, %.0f) mm", data->nominal_x, data->nominal_y,
	         data->nominal_z);
	draw_string(hud, x, y, line);
	y += line_h;

	// Left eye
	snprintf(line, sizeof(line), "L eye: (%.0f, %.0f, %.0f) mm", data->left_eye_x, data->left_eye_y,
	         data->left_eye_z);
	draw_string(hud, x, y, line);
	y += line_h;

	// Right eye
	snprintf(line, sizeof(line), "R eye: (%.0f, %.0f, %.0f) mm", data->right_eye_x, data->right_eye_y,
	         data->right_eye_z);
	draw_string(hud, x, y, line);
	y += line_h;

	// Tracking status
	snprintf(line, sizeof(line), "Track: %s", data->eye_tracking_active ? "Active" : "Inactive");
	draw_string(hud, x, y, line);
	y += line_h;

	// Zoom factor (only show if meaningful)
	if (data->zoom_scale > 0.0f) {
		snprintf(line, sizeof(line), "Zoom: %.2f", data->zoom_scale);
		draw_string(hud, x, y, line);
		y += line_h;
	}

	// Virtual display position
	snprintf(line, sizeof(line), "Virtual Display: (%.2f, %.2f, %.2f) m", data->vdisp_x, data->vdisp_y,
	         data->vdisp_z);
	draw_string(hud, x, y, line);
	y += line_h;

	// Forward vector
	snprintf(line, sizeof(line), "Fwd: (%.2f, %.2f, %.2f)", data->forward_x, data->forward_y, data->forward_z);
	draw_string(hud, x, y, line);
	y += line_h;

	// Blank separator
	y += line_h / 2;

	// Key hints
	draw_string(hud, x, y, "TAB=HUD  V=2D/3D  ESC=Quit");

	hud->dirty = true;
	return true;
}

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
