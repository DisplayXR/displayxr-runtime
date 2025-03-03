// Copyright 2025, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Prober for finding an Oculus Rift device based on USB VID/PID.
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup drv_rift
 */

#include "xrt/xrt_prober.h"

#include "util/u_misc.h"

#include "rift_interface.h"

static const char DK2_PRODUCT_STRING[] = "Rift DK2";

int
rift_found(struct xrt_prober *xp,
           struct xrt_prober_device **devices,
           size_t device_count,
           size_t index,
           cJSON *attached_data,
           struct xrt_device **out_xdev)
{
	struct xrt_prober_device *dev = devices[index];

	unsigned char manufacturer[128] = {0};
	int result = xrt_prober_get_string_descriptor(xp, dev, XRT_PROBER_STRING_MANUFACTURER, manufacturer,
	                                              sizeof(manufacturer));
	if (result < 0) {
		return -1;
	}

	unsigned char product[128] = {0};
	result = xrt_prober_get_string_descriptor(xp, dev, XRT_PROBER_STRING_PRODUCT, product, sizeof(product));
	if (result < 0) {
		return -1;
	}

	unsigned char serial_number[21] = {0};
	result = xrt_prober_get_string_descriptor(xp, dev, XRT_PROBER_STRING_SERIAL_NUMBER, serial_number,
	                                          sizeof(serial_number));
	if (result < 0) {
		return -1;
	}

	// Some non-oculus devices (VR-Tek HMDs) reuse the same USB IDs as the oculus headsets, so we should check the
	// manufacturer
	if (strncmp((const char *)manufacturer, "Oculus VR, Inc.", sizeof(manufacturer)) != 0) {
		return -1;
	}

	enum rift_variant variant;
	const char *name;
	switch (dev->product_id) {
	case OCULUS_DK2_PID:
		variant = RIFT_VARIANT_DK2;
		name = DK2_PRODUCT_STRING;
		break;
	default: return -1; break;
	}

	U_LOG_I("%s - Found at least the tracker of some Rift (%s) -- opening\n", __func__, name);

	struct os_hid_device *hid = NULL;
	result = xrt_prober_open_hid_interface(xp, dev, 0, &hid);
	if (result != 0) {
		return -1;
	}

	struct rift_hmd *hd = rift_hmd_create(hid, variant, (char *)product, (char *)serial_number);
	if (hd == NULL) {
		return -1;
	}
	*out_xdev = &hd->base;

	return 1;
}