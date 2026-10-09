// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  EDID base-block parser (see os_display_edid_parse.h).
 * @ingroup aux_os
 */

#include "os_display_edid_parse.h"

#include <string.h>


/*
 *
 * EDID.
 *
 */

static uint16_t
le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

bool
os_display_edid_parse(const uint8_t *edid, size_t len, struct os_display_edid_parsed *out)
{
	static const uint8_t header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

	if (out == NULL) {
		return false;
	}
	memset(out, 0, sizeof(*out));
	if (edid == NULL || len < 128 || memcmp(edid, header, sizeof(header)) != 0) {
		return false;
	}

	out->manufacturer_id = le16(&edid[8]);
	out->product_id = le16(&edid[10]);
	out->serial_number =
	    (uint32_t)edid[12] | ((uint32_t)edid[13] << 8) | ((uint32_t)edid[14] << 16) | ((uint32_t)edid[15] << 24);

	// Bytes 21/22: max image size in cm. One of them 0 means the other is an
	// aspect ratio (EDID 1.4), not a size — report nothing then.
	if (edid[21] != 0 && edid[22] != 0) {
		out->cm_width_mm = (uint32_t)edid[21] * 10u;
		out->cm_height_mm = (uint32_t)edid[22] * 10u;
	}

	// First detailed timing descriptor (bytes 54-71). Pixel clock 0 means it
	// is a display descriptor (name, range limits...) instead — eDP panels
	// often carry their timing in a DisplayID extension and leave this empty.
	const uint8_t *d = &edid[54];
	const uint32_t clock_10khz = le16(&d[0]);
	if (clock_10khz != 0) {
		const uint32_t h_active = (uint32_t)d[2] | ((uint32_t)(d[4] & 0xF0) << 4);
		const uint32_t h_blank = (uint32_t)d[3] | ((uint32_t)(d[4] & 0x0F) << 8);
		const uint32_t v_active = (uint32_t)d[5] | ((uint32_t)(d[7] & 0xF0) << 4);
		const uint32_t v_blank = (uint32_t)d[6] | ((uint32_t)(d[7] & 0x0F) << 8);
		out->dtd_width_px = h_active;
		out->dtd_height_px = v_active;
		out->dtd_width_mm = (uint32_t)d[12] | ((uint32_t)(d[14] & 0xF0) << 4);
		out->dtd_height_mm = (uint32_t)d[13] | ((uint32_t)(d[14] & 0x0F) << 8);
		const uint64_t total = (uint64_t)(h_active + h_blank) * (uint64_t)(v_active + v_blank);
		if (total > 0) {
			out->dtd_refresh_mhz =
			    (uint32_t)(((uint64_t)clock_10khz * 10000ull * 1000ull + total / 2) / total);
		}
	}

	// Display descriptors (bytes 54-125, four of 18 bytes): 00 00 00 FC 00
	// then up to 13 bytes of name, LF-terminated and space-padded.
	for (uint32_t k = 0; k < 4; k++) {
		const uint8_t *x = &edid[54 + 18 * k];
		if (x[0] != 0 || x[1] != 0 || x[3] != 0xFC) {
			continue;
		}
		size_t n = 0;
		for (; n < 13; n++) {
			const uint8_t ch = x[5 + n];
			if (ch == 0x0A || ch == 0x00) {
				break;
			}
			// Printable ASCII only: the field is "ASCII" by spec, and
			// anything else would corrupt a log line or a JSON string.
			out->monitor_name[n] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '?';
		}
		while (n > 0 && out->monitor_name[n - 1] == ' ') {
			n--;
		}
		out->monitor_name[n] = '\0';
		break;
	}

	return true;
}

void
os_display_edid_parsed_mm(const struct os_display_edid_parsed *p, uint32_t *out_w_mm, uint32_t *out_h_mm)
{
	uint32_t w = 0;
	uint32_t h = 0;
	if (p != NULL) {
		if (p->dtd_width_mm > 0 && p->dtd_height_mm > 0) {
			w = p->dtd_width_mm;
			h = p->dtd_height_mm;
		} else {
			w = p->cm_width_mm;
			h = p->cm_height_mm;
		}
	}
	*out_w_mm = w;
	*out_h_mm = h;
}
