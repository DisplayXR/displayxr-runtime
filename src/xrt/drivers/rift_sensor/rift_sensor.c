// Copyright 2025-2026, Beyley Cardellio
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Implementation of Oculus Rift sensor probing/initialization
 * @author Beyley Cardellio <ep1cm1n10n123@gmail.com>
 * @ingroup drv_rift_sensor
 */

#include "xrt/xrt_byte_order.h"

#include "util/u_debug.h"
#include "util/u_logging.h"
#include "util/u_var.h"

#include "rift/rift_interface.h"

#include "uvc/uvc_interface.h"

#include "esp570.h"
#include "esp770u.h"
#include "ar0134.h"
#include "mt9v034.h"
#include "rift_sensor_internal.h"


DEBUG_GET_ONCE_LOG_OPTION(rift_sensor_log, "RIFT_SENSOR_LOG", U_LOGGING_WARN)

#define SENSOR_TRACE(d, ...) U_LOG_IFL_T(d->log_level, __VA_ARGS__)
#define SENSOR_DEBUG(d, ...) U_LOG_IFL_D(d->log_level, __VA_ARGS__)
#define SENSOR_INFO(d, ...) U_LOG_IFL_I(d->log_level, __VA_ARGS__)
#define SENSOR_WARN(d, ...) U_LOG_IFL_W(d->log_level, __VA_ARGS__)
#define SENSOR_ERROR(d, ...) U_LOG_IFL_E(d->log_level, __VA_ARGS__)

/*
 * Internal functions
 */

static bool
rift_sensor_setup_stream_parameters_callback(uint16_t vid,
                                             uint16_t pid,
                                             bool is_usb2,
                                             libusb_device_handle *devh,
                                             struct uvc_probe_commit_control *control,
                                             struct uvc_stream_parameters *parameters,
                                             size_t *packet_size,
                                             int *alt_setting,
                                             void *user_data)
{
	int ret;

	struct rift_sensor_context *context = user_data;

	// unk vid
	if (vid != OCULUS_VR_VID) {
		return false;
	}

	switch (pid) {
	case OCULUS_DK2_SENSOR_PID:
		control->dwFrameInterval = __cpu_to_le32(166666);
		control->dwMaxVideoFrameSize = __cpu_to_le32(752 * 480);
		control->dwMaxPayloadTransferSize = __cpu_to_le32(3000);

		parameters->stride = 752;
		parameters->width = 752;
		parameters->height = 480;
		parameters->format = XRT_FORMAT_L8;

		*packet_size = 3060;
		*alt_setting = 7;

		esp570_setup_unknown_3(devh);

		return true;
	case OCULUS_CV1_SENSOR_PID:
		if (is_usb2) {
			// JPEG mode for USB 2 connection
			control->bFormatIndex = 2;
			control->bFrameIndex = 2;
			*packet_size = 2048;
			*alt_setting = 2;
			parameters->format = XRT_FORMAT_MJPEG;
		} else {
			control->bFrameIndex = 4;
			*packet_size = 16384;
			*alt_setting = 2;
			parameters->format = XRT_FORMAT_L8;
		}

		control->dwFrameInterval = __cpu_to_le32(192000);
		control->dwMaxVideoFrameSize = __cpu_to_le32(RIFT_SENSOR_FRAME_SIZE);
		control->dwMaxPayloadTransferSize = __cpu_to_le32(*packet_size);
		control->dwClockFrequency = __cpu_to_le32(RIFT_SENSOR_CLOCK_FREQ);

		parameters->stride = RIFT_SENSOR_WIDTH;
		parameters->width = RIFT_SENSOR_WIDTH;
		parameters->height = RIFT_SENSOR_HEIGHT;

		ret = rift_sensor_esp770u_init_regs(devh);
		if (ret < 0) {
			SENSOR_ERROR(context, "Failed to init CV1 sensor, reason %d", ret);
			return false;
		}

		return true;
	default: break;
	}

	return false;
}

static bool
rift_sensor_post_init_callback(uint16_t vid, uint16_t pid, bool is_usb2, libusb_device_handle *devh, void *user_data)
{
	int ret;

	struct rift_sensor_context *context = user_data;

	// In JPEG mode, we have some extra init packets to send
	if (pid == OCULUS_CV1_SENSOR_PID && is_usb2) {
		ret = rift_sensor_esp770u_init_jpeg(devh);
		if (ret < 0) {
			SENSOR_ERROR(context, "Failed to init CV1 sensor in JPEG mode");
			return false;
		}
	}

	return true;
}

static int
rift_sensor_read_calibration(struct rift_sensor_context *context,
                             struct rift_sensor *sensor,
                             const struct libusb_device_descriptor *desc)
{
	int ret;
	double fx, fy, cx, cy;

	switch (desc->idProduct) {
	case OCULUS_DK2_SENSOR_PID: {
		sensor->frame_interval = 16666 * OS_NS_PER_USEC;
		sensor->variant = RIFT_SENSOR_VARIANT_DK2;
		sensor->calibration.distortion_model = T_DISTORTION_OPENCV_RADTAN_5;
		struct t_camera_calibration_rt5_params calibration_params;

		struct rift_sensor_dk2_calib calib;

		// Read 4 32-byte blocks at EEPROM address 0x2000
		for (int i = 0; i < 128; i += 32) {
			ret = esp570_eeprom_read(sensor->hid_dev, 0x2000 + i, ((uint8_t *)&calib) + i, 32);
			if (ret < 0) {
				SENSOR_ERROR(context, "DK2 EEPROM read failed! reason %d", ret);
				return ret;
			}
		}

		fx = __lef64_to_cpu(calib.fx);
		fy = __lef64_to_cpu(calib.fy);
		cx = __lef64_to_cpu(calib.cx);
		cy = __lef64_to_cpu(calib.cy);

		calibration_params.k1 = __lef64_to_cpu(calib.k1);
		calibration_params.k2 = __lef64_to_cpu(calib.k2);
		calibration_params.p1 = __lef64_to_cpu(calib.p1);
		calibration_params.p2 = __lef64_to_cpu(calib.p2);
		calibration_params.k3 = __lef64_to_cpu(calib.k3);

		sensor->calibration.rt5 = calibration_params;

		break;
	}
	case OCULUS_CV1_SENSOR_PID: {
		sensor->frame_interval = 19200 * OS_NS_PER_USEC;
		sensor->variant = RIFT_SENSOR_VARIANT_CV1;
		sensor->calibration.distortion_model = T_DISTORTION_FISHEYE_KB4;
		struct t_camera_calibration_kb4_params calibration_params;

		struct rift_sensor_cv1_calib calib;

		// Read a 128-byte block at EEPROM address 0x1d000
		ret = rift_sensor_esp770u_flash_read(sensor->hid_dev, 0x1d000, ((uint8_t *)&calib), sizeof(calib));
		if (ret < 0) {
			SENSOR_ERROR(context, "CV1 EEPROM read failed! reason %d", ret);
			return ret;
		}

		// Fisheye distortion model parameters from firmware
		fx = fy = __lef32_to_cpu(calib.fxy);
		cx = __lef32_to_cpu(calib.cx);
		cy = __lef32_to_cpu(calib.cy);

		calibration_params.k1 = __lef32_to_cpu(calib.k1);
		calibration_params.k2 = __lef32_to_cpu(calib.k2);
		calibration_params.k3 = __lef32_to_cpu(calib.k3);
		calibration_params.k4 = __lef32_to_cpu(calib.k4);

		sensor->calibration.kb4 = calibration_params;

		break;
	}
	default: assert(false);
	}

	// clang-format off
	memcpy(sensor->calibration.intrinsics[0], (double[3]){fx,   0.0f, cx}, sizeof(double) * 3);
	memcpy(sensor->calibration.intrinsics[1], (double[3]){0.0f, fy,   cy}, sizeof(double) * 3);
	memcpy(sensor->calibration.intrinsics[2], (double[3]){0.0,  0.0f, 1.0}, sizeof(double) * 3);
	// clang-format on

	return 0;
}

static int
rift_sensor_create(struct rift_sensor_context *context,
                   struct rift_sensor *sensor,
                   libusb_device *device,
                   const struct libusb_device_descriptor *desc)
{
	int ret;

	struct libusb_device_handle *device_handle;
	ret = libusb_open(device, &device_handle);
	if (ret < 0) {
		SENSOR_WARN(context, "Failed to open USB device, reason %d", ret);
		return ret;
	}

	sensor->hid_dev = device_handle;

	ret = uvc_fs_create(context->usb_ctx, device_handle, desc, rift_sensor_setup_stream_parameters_callback,
	                    rift_sensor_post_init_callback, context, context->xfctx, &sensor->frame_server);
	if (ret < 0) {
		libusb_close(device_handle);
		SENSOR_ERROR(context, "Failed to create UVC frameserver, reason %d", ret);
		return ret;
	}

	struct xrt_fs_mode *supported_modes;
	uint32_t mode_count;
	bool success = xrt_fs_enumerate_modes(sensor->frame_server, &supported_modes, &mode_count);
	if (!success || mode_count == 0) {
		libusb_close(device_handle);
		SENSOR_ERROR(context, "Failed to enumerate UVC frameserver for sensor, mode count %d, skipping.",
		             mode_count);

		return -1;
	}

	// Pick the mode we want (the first)
	struct xrt_fs_mode mode = *supported_modes;
	free(supported_modes);

	sensor->usb2 = (desc->bcdUSB < 0x300);
	sensor->calibration.image_size_pixels = (struct xrt_size){(int)mode.width, (int)mode.height};
	ret = rift_sensor_read_calibration(context, sensor, desc);
	if (ret < 0) {
		libusb_close(device_handle);
		SENSOR_ERROR(context, "Failed to read calibration for sensor, reason %d", ret);
		return ret;
	}

	return 0;
}

static void *
rift_sensor_usb_thread_run(void *user_ptr)
{
	int result = 0;
	struct rift_sensor_context *context = (struct rift_sensor_context *)user_ptr;

	const char *thread_name = "Rift Sensor USB";

	U_TRACE_SET_THREAD_NAME(thread_name);
	os_thread_helper_name(&context->usb_thread, thread_name);

#ifdef XRT_OS_LINUX
	// Try to raise priority of this thread.
	u_linux_try_to_set_realtime_priority_on_thread(context->log_level, thread_name);
#endif

	os_thread_helper_lock(&context->usb_thread);

	// #define TICK_DEBUG
#ifdef TICK_DEBUG
	int ticks = 0;
#endif

	while (os_thread_helper_is_running_locked(&context->usb_thread) && result >= 0) {
		os_thread_helper_unlock(&context->usb_thread);

		result = libusb_handle_events_timeout_completed(context->usb_ctx,
		                                                &(struct timeval){.tv_sec = 0, .tv_usec = 1000}, NULL);

		// if timeout, ignore and just loop again to check if we should exit, otherwise handle the error
		if (result == LIBUSB_ERROR_TIMEOUT) {
			result = 0;
		}

		os_thread_helper_lock(&context->usb_thread);
#ifdef TICK_DEBUG
		ticks++;
#endif
	}

	os_thread_helper_unlock(&context->usb_thread);

	return NULL;
}

static void
rift_sensor_destroy(struct rift_sensor *sensor)
{
	if (sensor->hid_dev) {
		libusb_close(sensor->hid_dev);

		libusb_unref_device(libusb_get_device(sensor->hid_dev));
	}

	// @note uvc_fs_destroy is called by the xrt_frame_node destroy callback, so we don't need to call it here
}

/*
 * Exported functions
 */

void
rift_sensor_context_destroy(struct rift_sensor_context *context)
{
	u_var_remove_root(context);

	for (size_t i = 0; i < context->num_sensors; i++) {
		rift_sensor_destroy(&context->sensors[i]);
	}

	libusb_exit(context->usb_ctx);

	if (context->sensors != NULL) {
		free(context->sensors);
	}

	free(context);

	return;
}

int
rift_sensor_context_create(struct rift_sensor_context **out_context, struct xrt_frame_context *xfctx)
{
	int ret;

	struct rift_sensor_context *context = U_TYPED_CALLOC(struct rift_sensor_context);
	context->log_level = debug_get_log_option_rift_sensor_log();
	context->xfctx = xfctx;

	u_var_add_root(context, "Rift Sensors", false);
	u_var_add_log_level(context, &context->log_level, "Log Level");

	ret = libusb_init(&context->usb_ctx);
	if (ret < 0) {
		SENSOR_ERROR(context, "Failed to initialize libusb, reason %d", ret);
		goto fail;
	}

	if (!libusb_has_capability(LIBUSB_CAP_SUPPORTS_DETACH_KERNEL_DRIVER) ||
	    !libusb_has_capability(LIBUSB_CAP_HAS_HID_ACCESS)) {
		SENSOR_WARN(context,
		            "libusb does not have expected capabilities,"
		            "may not be able to access Rift sensors");
	}

	libusb_device **devices = NULL;
	ssize_t num_devices = libusb_get_device_list(context->usb_ctx, &devices);
	if (num_devices < 0) {
		SENSOR_ERROR(context, "Failed to get device list from libusb, reason %zd", num_devices);
		ret = -1;
		goto fail;
	}

	// Find how many sensors are connected
	size_t found_sensors = 0;
	for (ssize_t i = 0; i < num_devices; i++) {
		libusb_device *device = devices[i];

		struct libusb_device_descriptor desc;
		ret = libusb_get_device_descriptor(device, &desc);
		if (ret < 0) {
			SENSOR_WARN(context, "Failed to get device descriptor for device, reason %d, skipping.", ret);
			continue;
		}

		// skip oculus devices
		if (desc.idVendor != OCULUS_VR_VID)
			continue;

		switch (desc.idProduct) {
		case OCULUS_CV1_SENSOR_PID:
		case OCULUS_DK2_SENSOR_PID: {
			found_sensors++;
			break;
		}
		default: continue;
		}
	}

	context->sensors = U_TYPED_ARRAY_CALLOC(struct rift_sensor, found_sensors);

	for (ssize_t i = 0; i < num_devices; i++) {
		libusb_device *device = devices[i];

		struct libusb_device_descriptor desc;
		ret = libusb_get_device_descriptor(device, &desc);
		if (ret < 0) {
			SENSOR_WARN(context, "Failed to get device descriptor for device, reason %d, skipping.", ret);
			continue;
		}

		if (desc.idVendor != OCULUS_VR_VID)
			continue;

		switch (desc.idProduct) {
		case OCULUS_CV1_SENSOR_PID:
		case OCULUS_DK2_SENSOR_PID: {
			struct rift_sensor *sensor = &context->sensors[context->num_sensors];

			ret = rift_sensor_create(context, sensor, device, &desc);
			if (ret < 0) {
				SENSOR_ERROR(context, "Failed to create sensor for device %x:%x, reason %d, skipping.",
				             desc.idVendor, desc.idProduct, ret);
				// skip...
				continue;
			}

			SENSOR_DEBUG(context, "Found Rift sensor: %04x:%04x, usb2: %d", desc.idVendor, desc.idProduct,
			             sensor->usb2);

			libusb_ref_device(device);

			context->num_sensors++;

			break;
		}
		}
	}

	libusb_free_device_list(devices, 1);

	*out_context = context;
	return 0;

fail:
	rift_sensor_context_destroy(context);
	return ret;
}
