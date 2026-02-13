// Copyright 2025, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Interface to Oculus Rift driver code.
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup drv_rift
 */

#pragma once

#include "xrt/xrt_device.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_prober.h"


#ifdef __cplusplus
extern "C" {
#endif


struct rift_hmd;

enum rift_variant
{
	RIFT_VARIANT_DK1,
	RIFT_VARIANT_DK2,
	RIFT_VARIANT_CV1,
};

#define OCULUS_VR_VID 0x2833

#define OCULUS_DK2_PID 0x0021
#define OCULUS_DK2_SENSOR_PID 0x0201
#define OCULUS_CV1_PID 0x0031
#define OCULUS_CV1_SENSOR_PID 0x0211

#define RIFT_DK2_PRODUCT_STRING "Rift DK2"
#define RIFT_CV1_PRODUCT_STRING "Rift CV1"

/*!
 * Checks whether the given device is an Oculus device, by checking the manufacturer string.
 *
 * @ingroup drv_rift
 * @param xp The prober to use for getting string descriptors.
 * @param dev The device to check.
 * @return true if the device is an Oculus device, false otherwise.
 */
bool
rift_is_oculus(struct xrt_prober *xp, struct xrt_prober_device *dev);

/*!
 * Probing function for Oculus Rift devices.
 *
 * @ingroup drv_rift
 * @see xrt_prober_found_func_t
 */
int
rift_found(struct xrt_prober *xp,
           struct xrt_prober_device **devices,
           size_t device_count,
           size_t index,
           cJSON *attached_data,
           struct xrt_device **out_xdev);

int
rift_devices_create(struct os_hid_device *hmd_dev,
                    struct os_hid_device *radio_dev,
                    enum rift_variant variant,
                    const char *serial_number,
                    struct rift_hmd **out_hmd,
                    struct xrt_device **out_xdevs);

/*!
 * @dir drivers/rift
 *
 * @brief @ref drv_rift files.
 */

#ifdef __cplusplus
}
#endif
