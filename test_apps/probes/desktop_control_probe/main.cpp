// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  desktop_control_probe — headless CLI over XR_DXR_desktop_control (ADR-056).
 *
 * A plain OpenXR app (XR_MND_headless: no graphics, no window) that enables
 * XR_DXR_desktop_control and exposes every call, for bring-up and end-to-end
 * runs on Windows, macOS and Linux:
 *
 *   desktop_control_probe state
 *   desktop_control_probe request [reason...]        waits for the outcome event
 *   desktop_control_probe list
 *   desktop_control_probe pointer
 *   desktop_control_probe pick X Y
 *   desktop_control_probe text X Y
 *   desktop_control_probe act TYPE key=value...      waits for the completion event
 *   desktop_control_probe script FILE                one command per line, one session
 *
 * Spec v2 (screen capture):
 *   desktop_control_probe request-scopes control|capture|all[+again] [reason...]
 *   desktop_control_probe capture-state
 *   desktop_control_probe capture desktop|region X Y W H|window ID [max=WxH] [--save FILE.png]
 *                         waits for the completion event, fetches the image and prints its
 *                         size, sourceRect and how many pixels are withheld — never pixel data.
 *                         Only --save writes the image (a PNG) to disk.
 *
 * act TYPE: activate | move | minimize | restore | maximize | close | type | key |
 *           move-pointer | click | drag | scroll
 * act keys: target=ID  x=  y=  x2=  y2=  rect=X,Y,W,H  button=left|right|middle
 *           count=N  sx=  sy=  text="..."  keys=ctrl+s
 *
 * Script-only commands (E2E runs):
 *   run <shell command>         e.g. `run displayxr-cli desktop stop`
 *   sleep MS
 *   expect-state STATE          fail unless xrGetDesktopControlStateDXR says STATE
 *   expect RESULT               fail unless the last act ended with RESULT
 *                               (XR_SUCCESS, XR_ERROR_DESKTOP_CONTROL_REFUSED_DXR, ...)
 *   expect-text X Y "TEXT"      fail unless the text at X Y is TEXT
 *   expect-capture-state STATE  fail unless xrGetDesktopCaptureStateDXR says STATE
 *   expect-capture RESULT [WxH] fail unless the last capture ended with RESULT (and size)
 *   expect-desktop-pixel X Y R G B A
 *                               fail unless the last captured image, at the pixel that maps
 *                               to desktop point X Y (spec §10.3 mapping), is R G B A
 *   # comment
 *
 * Exit code: 0, or the number of failed commands — in a script, failed expectations
 * (an `act` there is judged by the `expect` after it); standalone, a failed `act`.
 *
 * WARNING: against a real backend this DOES operate the desktop (after the
 * OS / DisplayXR consent). Use DXR_DESKTOP_BACKEND=fake for a dry run.
 */

#include <openxr/openxr.h>
#include <openxr/XR_DXR_desktop_control.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#define XR_MND_HEADLESS_NAME "XR_MND_headless"

namespace {

XrInstance g_instance = XR_NULL_HANDLE;
XrSession g_session = XR_NULL_HANDLE;
XrResult g_last_act = XR_SUCCESS;
int g_failures = 0;
//! In a script an `act` outcome is judged by the following `expect`, not by its result.
bool g_in_script = false;

PFN_xrGetDesktopControlStateDXR pGetState;
PFN_xrRequestDesktopControlDXR pRequest;
PFN_xrEnumerateDesktopWindowsDXR pEnumWindows;
PFN_xrGetDesktopPointerDXR pGetPointer;
PFN_xrPickDesktopDXR pPick;
PFN_xrGetDesktopTextAtDXR pTextAt;
PFN_xrSubmitDesktopActionDXR pSubmit;
PFN_xrGetDesktopCaptureStateDXR pGetCaptureState;
PFN_xrCaptureDesktopDXR pCapture;
PFN_xrGetDesktopCaptureImageDXR pGetCaptureImage;

//! The last capture: its completion event and (when it succeeded) its image.
struct LastCapture
{
	uint64_t id = 0;
	XrResult result = XR_SUCCESS;
	uint32_t w = 0, h = 0;
	XrRect2Di src = {};
	XrTime frame_time = 0;
	std::vector<uint8_t> rgba;
} g_cap;

const char *
state_name(XrDesktopControlStateDXR s)
{
	switch (s) {
	case XR_DESKTOP_CONTROL_STATE_UNSUPPORTED_DXR: return "UNSUPPORTED";
	case XR_DESKTOP_CONTROL_STATE_NOT_REQUESTED_DXR: return "NOT_REQUESTED";
	case XR_DESKTOP_CONTROL_STATE_PENDING_DXR: return "PENDING";
	case XR_DESKTOP_CONTROL_STATE_GRANTED_DXR: return "GRANTED";
	case XR_DESKTOP_CONTROL_STATE_DENIED_DXR: return "DENIED";
	case XR_DESKTOP_CONTROL_STATE_DISABLED_BY_USER_DXR: return "DISABLED_BY_USER";
	case XR_DESKTOP_CONTROL_STATE_SUSPENDED_DXR: return "SUSPENDED";
	default: return "?";
	}
}

std::string
result_name(XrResult r)
{
	char buf[XR_MAX_RESULT_STRING_SIZE] = {0};
	if (g_instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(g_instance, r, buf))) {
		return buf;
	}
	return std::to_string((int)r);
}

const char *
pick_kind(XrDesktopPickKindDXR k)
{
	switch (k) {
	case XR_DESKTOP_PICK_KIND_WINDOW_DXR: return "window";
	case XR_DESKTOP_PICK_KIND_DESKTOP_DXR: return "desktop";
	case XR_DESKTOP_PICK_KIND_SHELL_DXR: return "shell";
	case XR_DESKTOP_PICK_KIND_OUTSIDE_DXR: return "outside";
	default: return "?";
	}
}

void
print_window(const XrDesktopWindowDXR &w)
{
	printf("  id=%llu parent=%llu pid=%u z=%u state=0x%llx frame=(%d,%d %dx%d) app=\"%s\" title=\"%s\"\n",
	       (unsigned long long)w.windowId, (unsigned long long)w.parentWindowId, w.pid, w.zOrder,
	       (unsigned long long)w.state, w.frame.offset.x, w.frame.offset.y, w.frame.extent.width,
	       w.frame.extent.height, w.appId, w.title);
}

/*!
 * Drain events. Returns true when @p want_action matched a completion (and sets
 * @p out_result) or @p want_state_resolved saw a non-PENDING state event.
 */
bool
pump(uint64_t want_action, XrResult *out_result, bool want_state_resolved)
{
	bool hit = false;
	for (;;) {
		XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
		XrResult r = xrPollEvent(g_instance, &ev);
		if (r != XR_SUCCESS) {
			return hit;
		}
		if (ev.type == XR_TYPE_EVENT_DATA_DESKTOP_CONTROL_STATE_CHANGED_DXR) {
			auto *e = reinterpret_cast<XrEventDataDesktopControlStateChangedDXR *>(&ev);
			printf("event: state -> %s\n", state_name(e->state));
			if (want_state_resolved && e->state != XR_DESKTOP_CONTROL_STATE_PENDING_DXR) {
				hit = true;
			}
		} else if (ev.type == XR_TYPE_EVENT_DATA_DESKTOP_ACTION_COMPLETED_DXR) {
			auto *e = reinterpret_cast<XrEventDataDesktopActionCompletedDXR *>(&ev);
			printf("event: action %llu -> %s \"%s\"\n", (unsigned long long)e->actionId,
			       result_name(e->result).c_str(), e->message);
			if (e->actionId == want_action) {
				*out_result = e->result;
				hit = true;
			}
		} else if (ev.type == XR_TYPE_EVENT_DATA_DESKTOP_CAPTURE_STATE_CHANGED_DXR) {
			auto *e = reinterpret_cast<XrEventDataDesktopCaptureStateChangedDXR *>(&ev);
			printf("event: capture state -> %s\n", state_name(e->state));
		} else if (ev.type == XR_TYPE_EVENT_DATA_DESKTOP_CAPTURE_COMPLETED_DXR) {
			auto *e = reinterpret_cast<XrEventDataDesktopCaptureCompletedDXR *>(&ev);
			printf("event: capture %llu -> %s %ux%u src=(%d,%d %dx%d) \"%s\"\n",
			       (unsigned long long)e->captureId, result_name(e->result).c_str(), e->width, e->height,
			       e->sourceRect.offset.x, e->sourceRect.offset.y, e->sourceRect.extent.width,
			       e->sourceRect.extent.height, e->message);
			if (e->captureId == g_cap.id) {
				g_cap.result = e->result;
				g_cap.w = e->width;
				g_cap.h = e->height;
				g_cap.src = e->sourceRect;
				g_cap.frame_time = e->frameTime;
				if (want_action == g_cap.id && out_result != nullptr) {
					*out_result = e->result;
					hit = true;
				}
			}
		}
	}
}

bool
wait_for(uint64_t action, XrResult *result, bool state_resolved, int timeout_ms)
{
	auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (std::chrono::steady_clock::now() < end) {
		if (pump(action, result, state_resolved)) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return false;
}

XrDesktopControlStateDXR
get_state(XrDesktopControlCapabilityFlagsDXR *caps)
{
	XrDesktopControlStateDXR s = XR_DESKTOP_CONTROL_STATE_UNSUPPORTED_DXR;
	XrResult r = pGetState(g_session, &s, caps);
	if (XR_FAILED(r)) {
		printf("xrGetDesktopControlStateDXR: %s\n", result_name(r).c_str());
		g_failures++;
	}
	return s;
}

//! Split a line into tokens; "double quotes" group, key="v a l" keeps the key.
std::vector<std::string>
tokenize(const std::string &line)
{
	std::vector<std::string> out;
	std::string cur;
	bool in_q = false, have = false;
	for (char ch : line) {
		if (ch == '"') {
			in_q = !in_q;
			have = true;
		} else if ((ch == ' ' || ch == '\t') && !in_q) {
			if (have) {
				out.push_back(cur);
			}
			cur.clear();
			have = false;
		} else {
			cur += ch;
			have = true;
		}
	}
	if (have) {
		out.push_back(cur);
	}
	return out;
}

bool
parse_type(const std::string &t, XrDesktopActionTypeDXR *out)
{
	static const struct
	{
		const char *n;
		XrDesktopActionTypeDXR t;
	} table[] = {
	    {"activate", XR_DESKTOP_ACTION_TYPE_ACTIVATE_DXR},
	    {"move", XR_DESKTOP_ACTION_TYPE_MOVE_RESIZE_DXR},
	    {"minimize", XR_DESKTOP_ACTION_TYPE_MINIMIZE_DXR},
	    {"restore", XR_DESKTOP_ACTION_TYPE_RESTORE_DXR},
	    {"maximize", XR_DESKTOP_ACTION_TYPE_MAXIMIZE_DXR},
	    {"close", XR_DESKTOP_ACTION_TYPE_CLOSE_DXR},
	    {"type", XR_DESKTOP_ACTION_TYPE_TYPE_TEXT_DXR},
	    {"key", XR_DESKTOP_ACTION_TYPE_KEY_CHORD_DXR},
	    {"move-pointer", XR_DESKTOP_ACTION_TYPE_POINTER_MOVE_DXR},
	    {"click", XR_DESKTOP_ACTION_TYPE_CLICK_DXR},
	    {"drag", XR_DESKTOP_ACTION_TYPE_DRAG_DXR},
	    {"scroll", XR_DESKTOP_ACTION_TYPE_SCROLL_DXR},
	};
	for (const auto &e : table) {
		if (t == e.n) {
			*out = e.t;
			return true;
		}
	}
	return false;
}

int
cmd_act(const std::vector<std::string> &args)
{
	if (args.size() < 2) {
		printf("act: TYPE required\n");
		return 1;
	}
	XrDesktopActionDXR a = {XR_TYPE_DESKTOP_ACTION_DXR};
	if (!parse_type(args[1], &a.actionType)) {
		printf("act: unknown type '%s'\n", args[1].c_str());
		return 1;
	}
	std::string text, keys;
	bool have_text = false, have_keys = false;
	for (size_t i = 2; i < args.size(); i++) {
		size_t eq = args[i].find('=');
		if (eq == std::string::npos) {
			printf("act: expected key=value, got '%s'\n", args[i].c_str());
			return 1;
		}
		std::string k = args[i].substr(0, eq), v = args[i].substr(eq + 1);
		long n = strtol(v.c_str(), nullptr, 10);
		if (k == "target" || k == "id") {
			a.targetWindowId = strtoull(v.c_str(), nullptr, 10);
		} else if (k == "x") {
			a.point.x = (int32_t)n;
		} else if (k == "y") {
			a.point.y = (int32_t)n;
		} else if (k == "x2") {
			a.point2.x = (int32_t)n;
		} else if (k == "y2") {
			a.point2.y = (int32_t)n;
		} else if (k == "rect") {
			int x = 0, y = 0, w = 0, h = 0;
			sscanf(v.c_str(), "%d,%d,%d,%d", &x, &y, &w, &h);
			a.rect = {{x, y}, {w, h}};
		} else if (k == "button") {
			a.button = v == "right"    ? XR_DESKTOP_POINTER_BUTTON_RIGHT_DXR
			           : v == "middle" ? XR_DESKTOP_POINTER_BUTTON_MIDDLE_DXR
			                           : XR_DESKTOP_POINTER_BUTTON_LEFT_DXR;
		} else if (k == "count") {
			a.clickCount = (uint32_t)n;
		} else if (k == "sx") {
			a.scrollX = (int32_t)n;
		} else if (k == "sy") {
			a.scrollY = (int32_t)n;
		} else if (k == "text") {
			text = v;
			have_text = true;
		} else if (k == "keys") {
			keys = v;
			have_keys = true;
		} else {
			printf("act: unknown key '%s'\n", k.c_str());
			return 1;
		}
	}
	a.text = have_text ? text.c_str() : nullptr;
	a.keys = have_keys ? keys.c_str() : nullptr;

	uint64_t id = 0;
	XrResult r = pSubmit(g_session, &a, &id);
	if (XR_FAILED(r)) {
		printf("act %s: submit -> %s\n", args[1].c_str(), result_name(r).c_str());
		g_last_act = r;
		return g_in_script ? 0 : 1;
	}
	printf("act %s: queued as %llu\n", args[1].c_str(), (unsigned long long)id);
	XrResult done = XR_SUCCESS;
	if (!wait_for(id, &done, false, 30000)) {
		printf("act %s: no completion event within 30 s\n", args[1].c_str());
		g_last_act = XR_TIMEOUT_EXPIRED;
		return 1;
	}
	g_last_act = done;
	return g_in_script || done == XR_SUCCESS ? 0 : 1;
}

int
cmd_text(int32_t x, int32_t y, std::string *out)
{
	uint32_t n = 0;
	XrResult r;
	std::vector<char> buf;
	for (int attempt = 0; attempt < 4; attempt++) {
		r = pTextAt(g_session, {x, y}, 0, &n, nullptr);
		if (XR_FAILED(r)) {
			break;
		}
		buf.assign(n, '\0');
		r = pTextAt(g_session, {x, y}, n, &n, buf.data());
		if (r != XR_ERROR_SIZE_INSUFFICIENT) {
			break; // live UI: grown text between the calls -> retry
		}
	}
	if (XR_FAILED(r)) {
		printf("text: %s\n", result_name(r).c_str());
		return 1;
	}
	*out = buf.data();
	return 0;
}

/*
 * A minimal PNG writer (stored deflate blocks, no compression): only for --save.
 */
uint32_t
crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
	static uint32_t table[256];
	static bool init = false;
	if (!init) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++) {
				c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			}
			table[i] = c;
		}
		init = true;
	}
	crc = ~crc;
	for (size_t i = 0; i < n; i++) {
		crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
	}
	return ~crc;
}

void
put_be32(std::vector<uint8_t> &v, uint32_t x)
{
	v.push_back((uint8_t)(x >> 24));
	v.push_back((uint8_t)(x >> 16));
	v.push_back((uint8_t)(x >> 8));
	v.push_back((uint8_t)x);
}

void
png_chunk(FILE *f, const char *type, const std::vector<uint8_t> &data)
{
	std::vector<uint8_t> buf;
	put_be32(buf, (uint32_t)data.size());
	buf.insert(buf.end(), type, type + 4);
	buf.insert(buf.end(), data.begin(), data.end());
	uint32_t crc = crc32_update(0, buf.data() + 4, buf.size() - 4);
	put_be32(buf, crc);
	fwrite(buf.data(), 1, buf.size(), f);
}

bool
write_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h)
{
	FILE *f = fopen(path, "wb");
	if (f == nullptr) {
		return false;
	}
	static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
	fwrite(sig, 1, 8, f);
	std::vector<uint8_t> ihdr;
	put_be32(ihdr, w);
	put_be32(ihdr, h);
	ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA
	png_chunk(f, "IHDR", ihdr);
	std::vector<uint8_t> raw;
	raw.reserve((size_t)h * (w * 4 + 1));
	for (uint32_t y = 0; y < h; y++) {
		raw.push_back(0); // filter: none
		raw.insert(raw.end(), rgba + (size_t)y * w * 4, rgba + (size_t)(y + 1) * w * 4);
	}
	std::vector<uint8_t> z = {0x78, 0x01};
	uint32_t a = 1, b = 0;
	for (uint8_t c : raw) {
		a = (a + c) % 65521;
		b = (b + a) % 65521;
	}
	for (size_t off = 0; off < raw.size() || off == 0; off += 65535) {
		size_t n = raw.size() - off < 65535 ? raw.size() - off : 65535;
		z.push_back(off + n >= raw.size() ? 1 : 0);
		z.push_back((uint8_t)n);
		z.push_back((uint8_t)(n >> 8));
		z.push_back((uint8_t)~n);
		z.push_back((uint8_t)(~n >> 8));
		z.insert(z.end(), raw.begin() + (long)off, raw.begin() + (long)(off + n));
		if (n == 0) {
			break;
		}
	}
	put_be32(z, (b << 16) | a);
	png_chunk(f, "IDAT", z);
	png_chunk(f, "IEND", {});
	return fclose(f) == 0;
}

int
cmd_capture(const std::vector<std::string> &args)
{
	if (args.size() < 2) {
		printf("capture desktop|region X Y W H|window ID [max=WxH] [--save FILE.png]\n");
		return 1;
	}
	XrDesktopCaptureInfoDXR info = {XR_TYPE_DESKTOP_CAPTURE_INFO_DXR};
	info.maxWidth = 8192;
	info.maxHeight = 8192;
	size_t i = 2;
	if (args[1] == "desktop") {
		info.target = XR_DESKTOP_CAPTURE_TARGET_DESKTOP_DXR;
	} else if (args[1] == "region" && args.size() >= 6) {
		info.target = XR_DESKTOP_CAPTURE_TARGET_REGION_DXR;
		info.region = {{atoi(args[2].c_str()), atoi(args[3].c_str())}, {atoi(args[4].c_str()), atoi(args[5].c_str())}};
		i = 6;
	} else if (args[1] == "window" && args.size() >= 3) {
		info.target = XR_DESKTOP_CAPTURE_TARGET_WINDOW_DXR;
		info.windowId = strtoull(args[2].c_str(), nullptr, 10);
		i = 3;
	} else {
		printf("capture: bad target\n");
		return 1;
	}
	std::string save;
	for (; i < args.size(); i++) {
		if (args[i].rfind("max=", 0) == 0) {
			sscanf(args[i].c_str() + 4, "%ux%u", &info.maxWidth, &info.maxHeight);
		} else if (args[i] == "--save" && i + 1 < args.size()) {
			save = args[++i];
		} else {
			printf("capture: unknown argument '%s'\n", args[i].c_str());
			return 1;
		}
	}
	g_cap = LastCapture();
	auto t0 = std::chrono::steady_clock::now();
	XrResult r = pCapture(g_session, &info, &g_cap.id);
	if (XR_FAILED(r)) {
		printf("capture: %s\n", result_name(r).c_str());
		g_cap.result = r;
		return g_in_script ? 0 : 1;
	}
	XrResult done = XR_SUCCESS;
	if (!wait_for(g_cap.id, &done, false, 30000)) {
		printf("capture: no completion event within 30 s\n");
		g_cap.result = XR_TIMEOUT_EXPIRED;
		return 1;
	}
	long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
	              .count();
	if (done != XR_SUCCESS) {
		printf("capture: %s after %ld ms\n", result_name(done).c_str(), ms);
		return g_in_script ? 0 : 1;
	}
	uint32_t n = 0;
	r = pGetCaptureImage(g_session, g_cap.id, 0, &n, nullptr);
	if (XR_SUCCEEDED(r)) {
		g_cap.rgba.assign(n, 0);
		r = pGetCaptureImage(g_session, g_cap.id, n, &n, g_cap.rgba.data());
	}
	if (XR_FAILED(r)) {
		printf("capture: image fetch -> %s\n", result_name(r).c_str());
		g_cap.result = r;
		return 1;
	}
	// Statistics only: no pixel data is ever printed.
	size_t withheld = 0, partial = 0;
	for (size_t k = 3; k < g_cap.rgba.size(); k += 4) {
		withheld += g_cap.rgba[k] == 0 ? 1 : 0;
		partial += g_cap.rgba[k] != 0 && g_cap.rgba[k] != 255 ? 1 : 0;
	}
	printf("capture: %ux%u from (%d,%d %dx%d) in %ld ms, %zu withheld px, %zu edge px\n", g_cap.w, g_cap.h,
	       g_cap.src.offset.x, g_cap.src.offset.y, g_cap.src.extent.width, g_cap.src.extent.height, ms, withheld,
	       partial);
	if (!save.empty()) {
		bool ok = write_png(save.c_str(), g_cap.rgba.data(), g_cap.w, g_cap.h);
		printf("capture: %s %s\n", ok ? "saved" : "could not save", save.c_str());
	}
	return 0;
}

int run_line(const std::string &line);

int
run_command(const std::vector<std::string> &args)
{
	const std::string &c = args[0];
	if (c == "state") {
		XrDesktopControlCapabilityFlagsDXR caps = 0;
		XrDesktopControlStateDXR s = get_state(&caps);
		printf("state: %s caps=0x%llx\n", state_name(s), (unsigned long long)caps);
		pump(0, nullptr, false);
		return 0;
	}
	if (c == "request") {
		std::string reason;
		for (size_t i = 1; i < args.size(); i++) {
			reason += (i > 1 ? " " : "") + args[i];
		}
		XrDesktopControlRequestInfoDXR info = {XR_TYPE_DESKTOP_CONTROL_REQUEST_INFO_DXR};
		info.reason = reason.c_str();
		XrResult r = pRequest(g_session, reason.empty() ? nullptr : &info);
		printf("request: %s\n", result_name(r).c_str());
		if (XR_FAILED(r)) {
			return 1;
		}
		if (!wait_for(0, nullptr, true, 120000)) {
			printf("request: no outcome within 120 s\n");
		}
		printf("state: %s\n", state_name(get_state(nullptr)));
		return 0;
	}
	if (c == "list") {
		uint32_t n = 0;
		XrResult r = pEnumWindows(g_session, 0, &n, nullptr);
		if (XR_SUCCEEDED(r)) {
			std::vector<XrDesktopWindowDXR> w(n, {XR_TYPE_DESKTOP_WINDOW_DXR});
			r = pEnumWindows(g_session, n, &n, w.data());
			if (XR_SUCCEEDED(r)) {
				printf("windows: %u\n", n);
				for (uint32_t i = 0; i < n; i++) {
					print_window(w[i]);
				}
			}
		}
		if (XR_FAILED(r)) {
			printf("list: %s\n", result_name(r).c_str());
			return 1;
		}
		return 0;
	}
	if (c == "pointer" || c == "pick") {
		XrDesktopPickDXR p = {XR_TYPE_DESKTOP_PICK_DXR};
		XrResult r;
		if (c == "pointer") {
			r = pGetPointer(g_session, &p);
		} else {
			if (args.size() < 3) {
				printf("pick X Y\n");
				return 1;
			}
			r = pPick(g_session, {atoi(args[1].c_str()), atoi(args[2].c_str())}, &p);
		}
		if (XR_FAILED(r)) {
			printf("%s: %s\n", c.c_str(), result_name(r).c_str());
			return 1;
		}
		printf("%s: (%d,%d) %s\n", c.c_str(), p.point.x, p.point.y, pick_kind(p.kind));
		if (p.kind == XR_DESKTOP_PICK_KIND_WINDOW_DXR) {
			print_window(p.window);
		}
		return 0;
	}
	if (c == "text") {
		if (args.size() < 3) {
			printf("text X Y\n");
			return 1;
		}
		std::string t;
		if (cmd_text(atoi(args[1].c_str()), atoi(args[2].c_str()), &t) != 0) {
			return 1;
		}
		printf("text: \"%s\"\n", t.c_str());
		return 0;
	}
	if (c == "act") {
		return cmd_act(args);
	}
	if (c == "request-scopes") {
		if (args.size() < 2) {
			printf("request-scopes control|capture|all [reason...]\n");
			return 1;
		}
		XrDesktopControlRequestScopeDXR sc = {XR_TYPE_DESKTOP_CONTROL_REQUEST_SCOPE_DXR};
		// control | capture | all, optionally "+again" (ask again even if granted).
		std::string what = args[1];
		bool again = false;
		size_t plus = what.find("+again");
		if (plus != std::string::npos) {
			again = true;
			what = what.substr(0, plus);
		}
		sc.scopes = what == "capture" ? XR_DESKTOP_CONTROL_SCOPE_CAPTURE_BIT_DXR
		            : what == "all"
		                ? XR_DESKTOP_CONTROL_SCOPE_CONTROL_BIT_DXR | XR_DESKTOP_CONTROL_SCOPE_CAPTURE_BIT_DXR
		                : XR_DESKTOP_CONTROL_SCOPE_CONTROL_BIT_DXR;
		if (again) {
			sc.scopes |= XR_DESKTOP_CONTROL_SCOPE_ASK_AGAIN_BIT_DXR;
		}
		std::string reason;
		for (size_t i = 2; i < args.size(); i++) {
			reason += (i > 2 ? " " : "") + args[i];
		}
		XrDesktopControlRequestInfoDXR info = {XR_TYPE_DESKTOP_CONTROL_REQUEST_INFO_DXR};
		info.next = &sc;
		info.reason = reason.empty() ? nullptr : reason.c_str();
		XrResult r = pRequest(g_session, &info);
		printf("request-scopes: %s\n", result_name(r).c_str());
		if (XR_FAILED(r)) {
			return 1;
		}
		// Wait until no listed scope is PENDING any more (the dialog may take minutes).
		auto end = std::chrono::steady_clock::now() + std::chrono::seconds(120);
		XrDesktopControlStateDXR cs = XR_DESKTOP_CONTROL_STATE_PENDING_DXR, ps = cs;
		while (std::chrono::steady_clock::now() < end) {
			pump(0, nullptr, false);
			cs = get_state(nullptr);
			pGetCaptureState(g_session, &ps);
			bool pending = ((sc.scopes & XR_DESKTOP_CONTROL_SCOPE_CONTROL_BIT_DXR) != 0 &&
			                cs == XR_DESKTOP_CONTROL_STATE_PENDING_DXR) ||
			               ((sc.scopes & XR_DESKTOP_CONTROL_SCOPE_CAPTURE_BIT_DXR) != 0 &&
			                ps == XR_DESKTOP_CONTROL_STATE_PENDING_DXR);
			if (!pending) {
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		pump(0, nullptr, false);
		printf("state: control %s, capture %s\n", state_name(cs), state_name(ps));
		return 0;
	}
	if (c == "capture-state") {
		XrDesktopControlStateDXR s = XR_DESKTOP_CONTROL_STATE_UNSUPPORTED_DXR;
		XrResult r = pGetCaptureState(g_session, &s);
		pump(0, nullptr, false);
		printf("capture state: %s (%s)\n", state_name(s), result_name(r).c_str());
		return XR_SUCCEEDED(r) ? 0 : 1;
	}
	if (c == "capture") {
		return cmd_capture(args);
	}
	if (c == "expect-capture-state") {
		XrDesktopControlStateDXR s = XR_DESKTOP_CONTROL_STATE_UNSUPPORTED_DXR;
		pGetCaptureState(g_session, &s);
		pump(0, nullptr, false);
		bool ok = args.size() > 1 && args[1] == state_name(s);
		printf("expect-capture-state %s: %s (is %s)\n", args.size() > 1 ? args[1].c_str() : "?",
		       ok ? "PASS" : "FAIL", state_name(s));
		return ok ? 0 : 1;
	}
	if (c == "expect-capture") {
		bool ok = args.size() > 1 && args[1] == result_name(g_cap.result);
		if (ok && args.size() > 2) {
			unsigned w = 0, h = 0;
			sscanf(args[2].c_str(), "%ux%u", &w, &h);
			ok = w == g_cap.w && h == g_cap.h;
		}
		printf("expect-capture %s%s%s: %s (was %s %ux%u)\n", args.size() > 1 ? args[1].c_str() : "?",
		       args.size() > 2 ? " " : "", args.size() > 2 ? args[2].c_str() : "", ok ? "PASS" : "FAIL",
		       result_name(g_cap.result).c_str(), g_cap.w, g_cap.h);
		return ok ? 0 : 1;
	}
	if (c == "expect-desktop-pixel") {
		bool ok = false;
		int got[4] = {-1, -1, -1, -1};
		if (args.size() >= 7 && !g_cap.rgba.empty() && g_cap.src.extent.width > 0 && g_cap.src.extent.height > 0) {
			// Inverse of x = src.x + (px + 0.5) * src.w / w.
			double dx = atof(args[1].c_str()) + 0.5, dy = atof(args[2].c_str()) + 0.5;
			long px = (long)((dx - g_cap.src.offset.x) * g_cap.w / g_cap.src.extent.width);
			long py = (long)((dy - g_cap.src.offset.y) * g_cap.h / g_cap.src.extent.height);
			if (px >= 0 && py >= 0 && px < (long)g_cap.w && py < (long)g_cap.h) {
				const uint8_t *p = &g_cap.rgba[((size_t)py * g_cap.w + (size_t)px) * 4];
				ok = true;
				for (int k = 0; k < 4; k++) {
					got[k] = p[k];
					ok = ok && p[k] == atoi(args[3 + k].c_str());
				}
			}
		}
		printf("expect-desktop-pixel %s %s: %s (is %d %d %d %d)\n", args.size() > 1 ? args[1].c_str() : "?",
		       args.size() > 2 ? args[2].c_str() : "?", ok ? "PASS" : "FAIL", got[0], got[1], got[2], got[3]);
		return ok ? 0 : 1;
	}
	if (c == "sleep") {
		std::this_thread::sleep_for(std::chrono::milliseconds(args.size() > 1 ? atoi(args[1].c_str()) : 100));
		pump(0, nullptr, false);
		return 0;
	}
	if (c == "run") {
		std::string cmd;
		for (size_t i = 1; i < args.size(); i++) {
			cmd += (i > 1 ? " " : "") + args[i];
		}
		printf("run: %s\n", cmd.c_str());
		fflush(stdout);
		int rc = system(cmd.c_str());
		fflush(stdout);
		return rc == 0 ? 0 : 1;
	}
	if (c == "expect-state") {
		XrDesktopControlStateDXR s = get_state(nullptr);
		pump(0, nullptr, false);
		bool ok = args.size() > 1 && args[1] == state_name(s);
		printf("expect-state %s: %s (is %s)\n", args.size() > 1 ? args[1].c_str() : "?", ok ? "PASS" : "FAIL",
		       state_name(s));
		return ok ? 0 : 1;
	}
	if (c == "expect") {
		bool ok = args.size() > 1 && args[1] == result_name(g_last_act);
		printf("expect %s: %s (was %s)\n", args.size() > 1 ? args[1].c_str() : "?", ok ? "PASS" : "FAIL",
		       result_name(g_last_act).c_str());
		return ok ? 0 : 1;
	}
	if (c == "expect-text") {
		std::string t;
		bool ok = args.size() > 3 && cmd_text(atoi(args[1].c_str()), atoi(args[2].c_str()), &t) == 0 &&
		          t == args[3];
		printf("expect-text \"%s\": %s (is \"%s\")\n", args.size() > 3 ? args[3].c_str() : "?",
		       ok ? "PASS" : "FAIL", t.c_str());
		return ok ? 0 : 1;
	}
	if (c == "script") {
		if (args.size() < 2) {
			printf("script FILE\n");
			return 1;
		}
		std::ifstream f(args[1]);
		if (!f) {
			printf("script: cannot open %s\n", args[1].c_str());
			return 1;
		}
		int fails = 0;
		g_in_script = true;
		std::string line;
		while (std::getline(f, line)) {
			fails += run_line(line);
		}
		return fails;
	}
	printf("unknown command '%s'\n", c.c_str());
	return 1;
}

int
run_line(const std::string &line)
{
	std::string l = line;
	while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) {
		l.pop_back();
	}
	std::vector<std::string> args = tokenize(l);
	if (args.empty() || args[0][0] == '#') {
		return 0;
	}
	printf("> %s\n", l.c_str());
	fflush(stdout);
	int rc = run_command(args);
	fflush(stdout);
	return rc;
}

bool
setup()
{
	uint32_t n = 0;
	xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
	std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
	xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data());
	bool have_dc = false, have_headless = false;
	for (auto &p : props) {
		have_dc |= strcmp(p.extensionName, XR_DXR_DESKTOP_CONTROL_EXTENSION_NAME) == 0;
		have_headless |= strcmp(p.extensionName, XR_MND_HEADLESS_NAME) == 0;
	}
	if (!have_dc || !have_headless) {
		fprintf(stderr, "runtime lacks %s%s\n", have_dc ? "" : XR_DXR_DESKTOP_CONTROL_EXTENSION_NAME " ",
		        have_headless ? "" : XR_MND_HEADLESS_NAME);
		return false;
	}

	const char *exts[] = {XR_MND_HEADLESS_NAME, XR_DXR_DESKTOP_CONTROL_EXTENSION_NAME};
	XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
	snprintf(ici.applicationInfo.applicationName, sizeof(ici.applicationInfo.applicationName),
	         "desktop_control_probe");
	ici.applicationInfo.applicationVersion = 1;
	snprintf(ici.applicationInfo.engineName, sizeof(ici.applicationInfo.engineName), "none");
	ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	ici.enabledExtensionCount = 2;
	ici.enabledExtensionNames = exts;
	XrResult r = xrCreateInstance(&ici, &g_instance);
	if (XR_FAILED(r)) {
		fprintf(stderr, "xrCreateInstance: %d\n", (int)r);
		return false;
	}

#define GET(name, var)                                                                                                 \
	if (XR_FAILED(xrGetInstanceProcAddr(g_instance, #name, (PFN_xrVoidFunction *)&var)) || var == nullptr) {       \
		fprintf(stderr, "missing %s\n", #name);                                                                \
		return false;                                                                                          \
	}
	GET(xrGetDesktopControlStateDXR, pGetState);
	GET(xrRequestDesktopControlDXR, pRequest);
	GET(xrEnumerateDesktopWindowsDXR, pEnumWindows);
	GET(xrGetDesktopPointerDXR, pGetPointer);
	GET(xrPickDesktopDXR, pPick);
	GET(xrGetDesktopTextAtDXR, pTextAt);
	GET(xrSubmitDesktopActionDXR, pSubmit);
	GET(xrGetDesktopCaptureStateDXR, pGetCaptureState);
	GET(xrCaptureDesktopDXR, pCapture);
	GET(xrGetDesktopCaptureImageDXR, pGetCaptureImage);
#undef GET

	XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId sys = XR_NULL_SYSTEM_ID;
	r = xrGetSystem(g_instance, &sgi, &sys);
	if (XR_FAILED(r)) {
		fprintf(stderr, "xrGetSystem: %s\n", result_name(r).c_str());
		return false;
	}
	XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
	sci.systemId = sys;
	r = xrCreateSession(g_instance, &sci, &g_session);
	if (XR_FAILED(r)) {
		fprintf(stderr, "xrCreateSession (headless): %s\n", result_name(r).c_str());
		return false;
	}
	return true;
}

} // namespace

int
main(int argc, char **argv)
{
	setvbuf(stdout, nullptr, _IOLBF, 0);
	if (argc < 2) {
		fprintf(stderr,
		        "usage: desktop_control_probe state | request [reason] | list | pointer | pick X Y |\n"
		        "       text X Y | act TYPE key=value... | script FILE |\n"
		        "       request-scopes control|capture|all [reason] | capture-state |\n"
		        "       capture desktop|region X Y W H|window ID [max=WxH] [--save FILE.png]\n");
		return 2;
	}
	if (!setup()) {
		return 3;
	}
	std::vector<std::string> args(argv + 1, argv + argc);
	int rc = run_command(args);
	g_failures += rc;

	xrDestroySession(g_session);
	xrDestroyInstance(g_instance);
	return g_failures > 255 ? 255 : g_failures;
}
