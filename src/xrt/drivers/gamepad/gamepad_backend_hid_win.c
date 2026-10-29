// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Windows HID reader for Sony pads (DualSense, DualSense Edge, DualShock 4).
 *
 * PlayStation pads are plain HID game controllers on Windows: no `IG_`
 * XInput node, so `XInputGetState` never sees them. This reader finds one
 * through SetupAPI (`HidD_GetHidGuid` interfaces), keeps only a Generic
 * Desktop / Game Pad top-level collection with a Sony VID:PID it knows, and
 * reads its input reports on a thread of its own with overlapped `ReadFile`
 * (the pad pushes reports at its own rate; the hub's 250 Hz poll just copies
 * the latest one). The byte layouts are pure functions in
 * `gamepad_hid_report.c`.
 *
 * Like XInput, `ReadFile` on a HID handle is focus-INDEPENDENT, which is what
 * lets a pad drive the rig for a handle app whose window owns the keyboard.
 *
 * Hot-plug: an empty scan walks every HID interface on the box, so it runs
 * at most once a second while no pad is open (the hub already polls a
 * disconnected backend at 2 Hz). A failed read (unplug, Bluetooth drop)
 * closes the pad with one WARN; the next scan is a second later.
 *
 * Bluetooth: both pads start in a "simple" report mode; reading their
 * calibration feature report (0x05 DualSense, 0x02 DualShock 4) switches
 * them to the full one, as every PlayStation driver does. Input parses in
 * either mode; rumble uses the CRC-32-trailed Bluetooth output reports.
 *
 * @ingroup drv_gamepad
 */

#include "xrt/xrt_config_os.h"

#ifdef XRT_OS_WINDOWS

#include "gamepad_backend_win.h"
#include "gamepad_hid_report.h"

#include "util/u_logging.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

//! Minimum spacing of two scans while no pad is open.
#define HID_RESCAN_MS 1000ULL
//! Reader's wait per pending read, so a stop request is served promptly.
#define HID_READ_WAIT_MS 100
//! Longest a rumble write may block the poll thread.
#define HID_WRITE_WAIT_MS 50

//! Generic Desktop page / Game Pad usage: the only collection kind accepted.
#define HID_USAGE_PAGE_GENERIC_DESKTOP 0x01
#define HID_USAGE_GAME_PAD 0x05

struct hid_pad
{
	HANDLE dev; //!< INVALID_HANDLE_VALUE when none is open
	enum gamepad_hid_pad pad;
	bool bluetooth;
	uint16_t vid, pid;
	USHORT in_len, out_len;
	uint8_t out_seq;

	HANDLE thread;
	volatile LONG stop;
	volatile LONG failed;

	CRITICAL_SECTION lock;
	//! Latest parsed sample; guarded by @ref lock.
	struct gamepad_state latest;
};

static struct hid_pad g = {INVALID_HANDLE_VALUE};
static bool g_lock_init = false;
static ULONGLONG g_next_scan_ms = 0;


/*
 *
 * Reader thread.
 *
 */

static void
store_report(const uint8_t *buf, DWORD got)
{
	struct gamepad_state s;
	memset(&s, 0, sizeof(s));
	if (!gamepad_hid_parse(g.pad, g.bluetooth, buf, (size_t)got, &s)) {
		return; // another report id (e.g. a feature echo): not input
	}
	EnterCriticalSection(&g.lock);
	g.latest = s;
	LeaveCriticalSection(&g.lock);
}

static DWORD WINAPI
reader_main(LPVOID unused)
{
	(void)unused;
	uint8_t *buf = (uint8_t *)calloc(1, g.in_len);
	OVERLAPPED ov;
	memset(&ov, 0, sizeof(ov));
	ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (buf == NULL || ov.hEvent == NULL) {
		InterlockedExchange(&g.failed, 1);
		free(buf);
		if (ov.hEvent != NULL) {
			CloseHandle(ov.hEvent);
		}
		return 0;
	}

	bool pending = false;
	while (InterlockedCompareExchange(&g.stop, 0, 0) == 0) {
		if (!pending) {
			DWORD got = 0;
			ResetEvent(ov.hEvent);
			if (ReadFile(g.dev, buf, g.in_len, &got, &ov)) {
				store_report(buf, got); // completed synchronously
				continue;
			}
			if (GetLastError() != ERROR_IO_PENDING) {
				InterlockedExchange(&g.failed, 1);
				break;
			}
			pending = true;
		}
		const DWORD w = WaitForSingleObject(ov.hEvent, HID_READ_WAIT_MS);
		if (w == WAIT_TIMEOUT) {
			continue; // keep the read pending; re-check stop
		}
		pending = false;
		DWORD got = 0;
		if (w != WAIT_OBJECT_0 || !GetOverlappedResult(g.dev, &ov, &got, FALSE)) {
			InterlockedExchange(&g.failed, 1);
			break;
		}
		store_report(buf, got);
	}

	if (pending) {
		DWORD got = 0;
		CancelIoEx(g.dev, &ov);
		GetOverlappedResult(g.dev, &ov, &got, TRUE);
	}
	CloseHandle(ov.hEvent);
	free(buf);
	return 0;
}


/*
 *
 * Output.
 *
 */

static void
write_report(const uint8_t *report, size_t len)
{
	if (g.dev == INVALID_HANDLE_VALUE || g.out_len == 0 || len > g.out_len) {
		return;
	}
	// Windows wants exactly the device's output report length.
	uint8_t *buf = (uint8_t *)calloc(1, g.out_len);
	OVERLAPPED ov;
	memset(&ov, 0, sizeof(ov));
	ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (buf == NULL || ov.hEvent == NULL) {
		free(buf);
		if (ov.hEvent != NULL) {
			CloseHandle(ov.hEvent);
		}
		return;
	}
	memcpy(buf, report, len);

	DWORD done = 0;
	if (!WriteFile(g.dev, buf, g.out_len, &done, &ov)) {
		if (GetLastError() == ERROR_IO_PENDING) {
			if (WaitForSingleObject(ov.hEvent, HID_WRITE_WAIT_MS) != WAIT_OBJECT_0) {
				CancelIoEx(g.dev, &ov);
			}
			GetOverlappedResult(g.dev, &ov, &done, TRUE);
		}
		// A refused write is not worth a log line: rumble is best effort.
	}
	CloseHandle(ov.hEvent);
	free(buf);
}

static void
send_rumble(float low, float high)
{
	uint8_t report[96];
	const size_t len = gamepad_hid_build_rumble(g.pad, g.bluetooth, g.out_seq, low, high, report, sizeof(report));
	g.out_seq = (uint8_t)((g.out_seq + 1) & 0x0F);
	if (len > 0) {
		write_report(report, len);
	}
}


/*
 *
 * Open / close / scan.
 *
 */

static void
close_pad(bool lost)
{
	if (g.dev == INVALID_HANDLE_VALUE) {
		return;
	}
	InterlockedExchange(&g.stop, 1);
	if (g.thread != NULL) {
		// The reader re-checks stop every HID_READ_WAIT_MS.
		WaitForSingleObject(g.thread, 2000);
		CloseHandle(g.thread);
		g.thread = NULL;
	}
	if (!lost) {
		send_rumble(0.0f, 0.0f); // leave the motors off
	}
	CloseHandle(g.dev);
	g.dev = INVALID_HANDLE_VALUE;

	if (lost) {
		U_LOG_W("gamepad: HID %s (%04X:%04X) lost — rescanning.", gamepad_hid_pad_name(g.pad), g.vid, g.pid);
	} else {
		U_LOG_W("gamepad: HID %s (%04X:%04X) closed.", gamepad_hid_pad_name(g.pad), g.vid, g.pid);
	}
	g.pad = GAMEPAD_HID_PAD_NONE;
}

static bool
path_has_ig(const wchar_t *path)
{
	// XInput-wrapped pads expose an "IG_" HID node; XInput owns those.
	for (const wchar_t *p = path; p[0] != L'\0' && p[1] != L'\0' && p[2] != L'\0'; p++) {
		if ((p[0] == L'I' || p[0] == L'i') && (p[1] == L'G' || p[1] == L'g') && p[2] == L'_') {
			return true;
		}
	}
	return false;
}

/*!
 * Is @p path a supported pad? Opened with no access rights (works for every
 * HID collection, keyboards and mice included, and disturbs nothing): a
 * known VID:PID first, then the top-level collection must be Game Pad.
 */
static enum gamepad_hid_pad
probe_path(const wchar_t *path, uint16_t *out_vid, uint16_t *out_pid)
{
	if (path_has_ig(path)) {
		return GAMEPAD_HID_PAD_NONE;
	}
	HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		return GAMEPAD_HID_PAD_NONE;
	}
	enum gamepad_hid_pad pad = GAMEPAD_HID_PAD_NONE;
	HIDD_ATTRIBUTES attr;
	memset(&attr, 0, sizeof(attr));
	attr.Size = sizeof(attr);
	if (HidD_GetAttributes(h, &attr)) {
		pad = gamepad_hid_match(attr.VendorID, attr.ProductID);
	}
	if (pad != GAMEPAD_HID_PAD_NONE) {
		PHIDP_PREPARSED_DATA pp = NULL;
		HIDP_CAPS caps;
		memset(&caps, 0, sizeof(caps));
		bool game_pad = false;
		if (HidD_GetPreparsedData(h, &pp)) {
			game_pad = HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS &&
			           caps.UsagePage == HID_USAGE_PAGE_GENERIC_DESKTOP && caps.Usage == HID_USAGE_GAME_PAD;
			HidD_FreePreparsedData(pp);
		}
		if (!game_pad) {
			pad = GAMEPAD_HID_PAD_NONE;
		}
		*out_vid = attr.VendorID;
		*out_pid = attr.ProductID;
	}
	CloseHandle(h);
	return pad;
}

static bool
open_pad(const wchar_t *path, enum gamepad_hid_pad pad, uint16_t vid, uint16_t pid)
{
	HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
	                       OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
	bool writable = true;
	if (h == INVALID_HANDLE_VALUE) {
		// Something holds write access exclusively: read-only still gives input.
		h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		                FILE_FLAG_OVERLAPPED, NULL);
		writable = false;
	}
	if (h == INVALID_HANDLE_VALUE) {
		return false;
	}

	PHIDP_PREPARSED_DATA pp = NULL;
	HIDP_CAPS caps;
	memset(&caps, 0, sizeof(caps));
	if (!HidD_GetPreparsedData(h, &pp)) {
		CloseHandle(h);
		return false;
	}
	const bool caps_ok = HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS;
	HidD_FreePreparsedData(pp);
	if (!caps_ok || caps.InputReportByteLength < 10) {
		CloseHandle(h);
		return false;
	}

	g.dev = h;
	g.pad = pad;
	g.vid = vid;
	g.pid = pid;
	g.in_len = caps.InputReportByteLength;
	g.out_len = writable ? caps.OutputReportByteLength : 0;
	// USB reports are 64 bytes (report id included) on all three pads;
	// Bluetooth's are longer (78 on DualSense, 547 on DualShock 4).
	g.bluetooth = caps.InputReportByteLength > 64;
	g.out_seq = 0;
	memset(&g.latest, 0, sizeof(g.latest));
	g.latest.connected = true; // centred and released until the first report

	if (g.bluetooth && caps.FeatureReportByteLength > 0) {
		// Reading the calibration feature report switches the pad to its full report.
		uint8_t *feat = (uint8_t *)calloc(1, caps.FeatureReportByteLength);
		if (feat != NULL) {
			feat[0] = (pad == GAMEPAD_HID_PAD_DUALSHOCK4) ? 0x02 : 0x05;
			HidD_GetFeature(h, feat, caps.FeatureReportByteLength);
			free(feat);
		}
	}

	InterlockedExchange(&g.stop, 0);
	InterlockedExchange(&g.failed, 0);
	g.thread = CreateThread(NULL, 0, reader_main, NULL, 0, NULL);
	if (g.thread == NULL) {
		CloseHandle(h);
		g.dev = INVALID_HANDLE_VALUE;
		g.pad = GAMEPAD_HID_PAD_NONE;
		return false;
	}

	U_LOG_W("gamepad: HID %s (%04X:%04X) opened over %s%s.", gamepad_hid_pad_name(pad), vid, pid,
	        g.bluetooth ? "bt" : "usb", writable ? "" : ", read-only (no rumble)");
	return true;
}

static void
scan(void)
{
	GUID hid_guid;
	HidD_GetHidGuid(&hid_guid);
	HDEVINFO set = SetupDiGetClassDevsW(&hid_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (set == INVALID_HANDLE_VALUE) {
		return;
	}

	SP_DEVICE_INTERFACE_DATA ifd;
	memset(&ifd, 0, sizeof(ifd));
	ifd.cbSize = sizeof(ifd);
	for (DWORD i = 0; g.dev == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, NULL, &hid_guid, i, &ifd);
	     i++) {
		DWORD need = 0;
		SetupDiGetDeviceInterfaceDetailW(set, &ifd, NULL, 0, &need, NULL);
		if (need < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
			continue;
		}
		SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)calloc(1, need);
		if (detail == NULL) {
			break;
		}
		detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
		if (SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, need, NULL, NULL)) {
			uint16_t vid = 0, pid = 0;
			const enum gamepad_hid_pad pad = probe_path(detail->DevicePath, &vid, &pid);
			if (pad != GAMEPAD_HID_PAD_NONE) {
				open_pad(detail->DevicePath, pad, vid, pid);
			}
		}
		free(detail);
	}
	SetupDiDestroyDeviceInfoList(set);
}


/*
 *
 * The reader API (gamepad_backend_win.h).
 *
 */

bool
gamepad_hid_init(void)
{
	if (!g_lock_init) {
		InitializeCriticalSection(&g.lock);
		g_lock_init = true;
	}
	g.dev = INVALID_HANDLE_VALUE;
	g.pad = GAMEPAD_HID_PAD_NONE;
	g.thread = NULL;
	g_next_scan_ms = 0; // scan on the first poll
	return true;
}

void
gamepad_hid_fini(void)
{
	close_pad(false);
	if (g_lock_init) {
		DeleteCriticalSection(&g.lock);
		g_lock_init = false;
	}
}

void
gamepad_hid_poll(struct gamepad_state *out)
{
	const ULONGLONG now = GetTickCount64();

	if (g.dev != INVALID_HANDLE_VALUE && InterlockedCompareExchange(&g.failed, 0, 0) != 0) {
		close_pad(true);
		g_next_scan_ms = now + HID_RESCAN_MS;
	}
	if (g.dev == INVALID_HANDLE_VALUE && now >= g_next_scan_ms) {
		scan();
		g_next_scan_ms = now + HID_RESCAN_MS;
	}
	if (g.dev == INVALID_HANDLE_VALUE) {
		out->connected = false;
		out->buttons = 0;
		out->left_x = out->left_y = out->right_x = out->right_y = 0.0f;
		out->left_trigger = out->right_trigger = 0.0f;
		return;
	}

	EnterCriticalSection(&g.lock);
	const struct gamepad_state s = g.latest;
	LeaveCriticalSection(&g.lock);
	const int64_t ts = out->timestamp_ns;
	*out = s;
	out->connected = true;
	out->timestamp_ns = ts; // stamped by the caller
}

void
gamepad_hid_rumble(float low_frequency, float high_frequency)
{
	if (g.dev == INVALID_HANDLE_VALUE || InterlockedCompareExchange(&g.failed, 0, 0) != 0) {
		return;
	}
	send_rumble(low_frequency, high_frequency);
}

const char *
gamepad_hid_name(void)
{
	switch (g.pad) {
	case GAMEPAD_HID_PAD_DUALSENSE:
	case GAMEPAD_HID_PAD_DUALSENSE_EDGE: return "hid:dualsense";
	case GAMEPAD_HID_PAD_DUALSHOCK4: return "hid:dualshock4";
	default: return "none";
	}
}

#else
// Keep the translation unit non-empty where this backend is compiled out (-Wempty-translation-unit).
typedef int gamepad_backend_hid_win_not_built_here_t;
#endif // XRT_OS_WINDOWS
