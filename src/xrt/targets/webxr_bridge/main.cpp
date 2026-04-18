// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  DisplayXR WebXR Bridge v2 — metadata sideband host (Phase 2).
 *
 * Headless OpenXR client coexisting with Chrome's WebXR session against the
 * same displayxr-service. Enables XR_EXT_display_info + XR_MND_headless,
 * enumerates display info and rendering modes, polls events and eye poses,
 * and exposes everything via a loopback WebSocket on 127.0.0.1:9014.
 *
 * JSON protocol v1 — see webxr-bridge/PROTOCOL.md.
 */

#include <winsock2.h>
#include <ws2tcpip.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>

#include <openxr/openxr.h>
#include <openxr/XR_EXT_display_info.h>
#include "../../auxiliary/util/u_bridge_hud_shared.h"

// ---------------------------------------------------------------------------
// Logging.
// ---------------------------------------------------------------------------

static void log_line(const char *level, const char *fmt, ...) {
	std::fprintf(stdout, "[webxr-bridge][%s] ", level);
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(stdout, fmt, ap);
	va_end(ap);
	std::fputc('\n', stdout);
	std::fflush(stdout);
}

#define LOG_I(...) log_line("info", __VA_ARGS__)
#define LOG_W(...) log_line("warn", __VA_ARGS__)
#define LOG_E(...) log_line("error", __VA_ARGS__)

static const char *xr_result_str(XrInstance inst, XrResult r) {
	static thread_local char buf[XR_MAX_RESULT_STRING_SIZE];
	if (inst != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(inst, r, buf))) {
		return buf;
	}
	std::snprintf(buf, sizeof(buf), "XrResult(%d)", (int)r);
	return buf;
}

#define XR_CHECK(inst, call)                                                    \
	do {                                                                        \
		XrResult _r = (call);                                                   \
		if (XR_FAILED(_r)) {                                                    \
			LOG_E("%s failed: %s", #call, xr_result_str((inst), _r));           \
			return false;                                                       \
		}                                                                       \
	} while (0)

// ---------------------------------------------------------------------------
// Minimal SHA-1 (for WebSocket handshake Sec-WebSocket-Accept).
// ---------------------------------------------------------------------------

static void sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
	uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE;
	uint32_t h3 = 0x10325476, h4 = 0xC3D2E1F0;
	uint64_t bits = (uint64_t)len * 8;
	size_t padded = ((len + 8) / 64 + 1) * 64;
	std::vector<uint8_t> msg(padded, 0);
	std::memcpy(msg.data(), data, len);
	msg[len] = 0x80;
	for (int i = 0; i < 8; i++)
		msg[padded - 1 - i] = (uint8_t)(bits >> (i * 8));
	for (size_t off = 0; off < padded; off += 64) {
		uint32_t w[80];
		for (int i = 0; i < 16; i++)
			w[i] = (msg[off+i*4]<<24)|(msg[off+i*4+1]<<16)|(msg[off+i*4+2]<<8)|msg[off+i*4+3];
		for (int i = 16; i < 80; i++) {
			uint32_t v = w[i-3]^w[i-8]^w[i-14]^w[i-16];
			w[i] = (v<<1)|(v>>31);
		}
		uint32_t a=h0, b=h1, c=h2, d=h3, e=h4;
		for (int i = 0; i < 80; i++) {
			uint32_t f, k;
			if (i<20)      { f=(b&c)|((~b)&d); k=0x5A827999; }
			else if (i<40) { f=b^c^d;           k=0x6ED9EBA1; }
			else if (i<60) { f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDC; }
			else           { f=b^c^d;           k=0xCA62C1D6; }
			uint32_t t = ((a<<5)|(a>>27)) + f + e + k + w[i];
			e=d; d=c; c=(b<<30)|(b>>2); b=a; a=t;
		}
		h0+=a; h1+=b; h2+=c; h3+=d; h4+=e;
	}
	uint32_t hh[5] = {h0,h1,h2,h3,h4};
	for (int i = 0; i < 5; i++) {
		out[i*4]=(uint8_t)(hh[i]>>24); out[i*4+1]=(uint8_t)(hh[i]>>16);
		out[i*4+2]=(uint8_t)(hh[i]>>8); out[i*4+3]=(uint8_t)(hh[i]);
	}
}

static std::string base64_encode(const uint8_t *data, size_t len) {
	static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string r;
	for (size_t i = 0; i < len; i += 3) {
		uint32_t v = (uint32_t)data[i] << 16;
		if (i+1<len) v |= (uint32_t)data[i+1] << 8;
		if (i+2<len) v |= data[i+2];
		r += t[(v>>18)&63]; r += t[(v>>12)&63];
		r += (i+1<len) ? t[(v>>6)&63] : '=';
		r += (i+2<len) ? t[v&63] : '=';
	}
	return r;
}

// ---------------------------------------------------------------------------
// Minimal RFC 6455 WebSocket server — single client, text frames only.
// ---------------------------------------------------------------------------

static const int WS_PORT = 9014;

static bool ws_validate_origin(const std::string &origin) {
	if (origin.empty()) return true;
	if (origin.find("http://localhost") == 0) return true;
	if (origin.find("http://127.0.0.1") == 0) return true;
	if (origin.find("https://localhost") == 0) return true;
	if (origin.find("https://127.0.0.1") == 0) return true;
	if (origin.find("file://") == 0) return true;
	if (origin.find("chrome-extension://") == 0) return true;
	return false;
}

static bool ws_recv_exact(SOCKET s, char *buf, int n) {
	int got = 0;
	while (got < n) {
		int r = recv(s, buf + got, n - got, 0);
		if (r <= 0) return false;
		got += r;
	}
	return true;
}

static bool ws_do_handshake(SOCKET client) {
	char buf[4096];
	int total = 0;
	while (total < (int)sizeof(buf) - 1) {
		int r = recv(client, buf + total, 1, 0);
		if (r <= 0) return false;
		total += r;
		if (total >= 4 && std::memcmp(buf + total - 4, "\r\n\r\n", 4) == 0)
			break;
	}
	buf[total] = '\0';
	std::string req(buf, total);

	auto hdr_val = [&](const char *name) -> std::string {
		std::string needle = std::string("\r\n") + name + ": ";
		size_t pos = req.find(needle);
		if (pos == std::string::npos) {
			std::string lower_needle = std::string("\r\n") + name;
			for (auto &c : lower_needle) c = (char)std::tolower(c);
			std::string lower_req = req;
			for (auto &c : lower_req) c = (char)std::tolower(c);
			pos = lower_req.find(lower_needle);
			if (pos == std::string::npos) return "";
			pos = req.find(": ", pos);
			if (pos == std::string::npos) return "";
			pos += 2;
			size_t end = req.find("\r\n", pos);
			return req.substr(pos, end - pos);
		}
		pos += needle.size();
		size_t end = req.find("\r\n", pos);
		return req.substr(pos, end - pos);
	};

	std::string ws_key = hdr_val("Sec-WebSocket-Key");
	std::string origin = hdr_val("Origin");
	if (ws_key.empty()) {
		LOG_W("WS handshake: missing Sec-WebSocket-Key");
		return false;
	}
	if (!ws_validate_origin(origin)) {
		LOG_W("WS handshake: rejected origin '%s'", origin.c_str());
		const char *resp = "HTTP/1.1 403 Forbidden\r\n\r\n";
		send(client, resp, (int)std::strlen(resp), 0);
		return false;
	}

	std::string accept_src = ws_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	uint8_t hash[20];
	sha1((const uint8_t *)accept_src.c_str(), accept_src.size(), hash);
	std::string accept = base64_encode(hash, 20);

	std::string resp = "HTTP/1.1 101 Switching Protocols\r\n"
	                   "Upgrade: websocket\r\n"
	                   "Connection: Upgrade\r\n"
	                   "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
	send(client, resp.c_str(), (int)resp.size(), 0);
	return true;
}

static bool ws_send_text(SOCKET s, const std::string &msg) {
	size_t len = msg.size();
	std::vector<uint8_t> frame;
	frame.push_back(0x81); // FIN + text opcode
	if (len < 126) {
		frame.push_back((uint8_t)len);
	} else if (len < 65536) {
		frame.push_back(126);
		frame.push_back((uint8_t)(len >> 8));
		frame.push_back((uint8_t)(len & 0xFF));
	} else {
		frame.push_back(127);
		for (int i = 7; i >= 0; i--)
			frame.push_back((uint8_t)((len >> (i * 8)) & 0xFF));
	}
	frame.insert(frame.end(), msg.begin(), msg.end());
	int sent = 0, total = (int)frame.size();
	while (sent < total) {
		int r = send(s, (const char *)frame.data() + sent, total - sent, 0);
		if (r <= 0) return false;
		sent += r;
	}
	return true;
}

static bool ws_recv_text(SOCKET s, std::string &out, int timeout_ms = 100) {
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(s, &fds);
	struct timeval tv;
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	int sel = select(0, &fds, nullptr, nullptr, &tv);
	if (sel <= 0) return false;

	uint8_t hdr[2];
	if (!ws_recv_exact(s, (char *)hdr, 2)) return false;
	uint8_t opcode = hdr[0] & 0x0F;
	bool masked = (hdr[1] & 0x80) != 0;
	uint64_t payload_len = hdr[1] & 0x7F;
	if (payload_len == 126) {
		uint8_t ext[2];
		if (!ws_recv_exact(s, (char *)ext, 2)) return false;
		payload_len = ((uint64_t)ext[0] << 8) | ext[1];
	} else if (payload_len == 127) {
		uint8_t ext[8];
		if (!ws_recv_exact(s, (char *)ext, 8)) return false;
		payload_len = 0;
		for (int i = 0; i < 8; i++)
			payload_len = (payload_len << 8) | ext[i];
	}

	uint8_t mask_key[4] = {};
	if (masked) {
		if (!ws_recv_exact(s, (char *)mask_key, 4)) return false;
	}

	if (payload_len > 1024 * 1024) return false; // sanity cap at 1 MB
	std::vector<char> payload((size_t)payload_len);
	if (payload_len > 0 && !ws_recv_exact(s, payload.data(), (int)payload_len))
		return false;
	if (masked) {
		for (size_t i = 0; i < payload_len; i++)
			payload[i] ^= mask_key[i % 4];
	}

	if (opcode == 0x8) { // close
		out.clear();
		return false;
	}
	if (opcode == 0x9) { // ping → pong
		uint8_t pong[2] = {0x8A, 0x00};
		send(s, (const char *)pong, 2, 0);
		out.clear();
		return true;
	}
	if (opcode == 0xA) { // pong — ignore
		out.clear();
		return true;
	}
	out.assign(payload.begin(), payload.end());
	return true;
}

static void ws_send_close(SOCKET s, uint16_t code) {
	uint8_t frame[4] = {0x88, 0x02, (uint8_t)(code >> 8), (uint8_t)(code & 0xFF)};
	send(s, (const char *)frame, 4, 0);
}

// ---------------------------------------------------------------------------
// JSON helpers (hand-rolled — protocol schema is flat and tiny).
// ---------------------------------------------------------------------------

static std::string json_escape(const char *s) {
	std::string r;
	for (; *s; s++) {
		if (*s == '"') r += "\\\"";
		else if (*s == '\\') r += "\\\\";
		else if (*s == '\n') r += "\\n";
		else r += *s;
	}
	return r;
}

static std::string json_f(float v) {
	char b[32]; std::snprintf(b, sizeof(b), "%.6g", v); return b;
}
static std::string json_u(uint32_t v) {
	char b[16]; std::snprintf(b, sizeof(b), "%u", v); return b;
}
static std::string json_i(int v) {
	char b[16]; std::snprintf(b, sizeof(b), "%d", v); return b;
}

// ---------------------------------------------------------------------------
// Ctrl+C handling.
// ---------------------------------------------------------------------------

static std::atomic<bool> g_running{true};

static BOOL WINAPI console_ctrl_handler(DWORD type) {
	switch (type) {
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
	case CTRL_CLOSE_EVENT:
	case CTRL_LOGOFF_EVENT:
	case CTRL_SHUTDOWN_EVENT:
		LOG_I("Ctrl+C / close event received, shutting down");
		g_running.store(false);
		return TRUE;
	default:
		return FALSE;
	}
}

// ---------------------------------------------------------------------------
// Outgoing message queue (main thread → WS thread).
// ---------------------------------------------------------------------------

struct MessageQueue {
	std::mutex mtx;
	std::condition_variable cv;
	std::queue<std::string> q;

	void push(const std::string &msg) {
		{
			std::lock_guard<std::mutex> lk(mtx);
			q.push(msg);
		}
		cv.notify_one();
	}

	bool pop(std::string &out, int timeout_ms) {
		std::unique_lock<std::mutex> lk(mtx);
		if (!cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
		                 [&] { return !q.empty(); }))
			return false;
		out = std::move(q.front());
		q.pop();
		return true;
	}
};

// ---------------------------------------------------------------------------
// Bridge state.
// ---------------------------------------------------------------------------

// Compositor window metrics (client area of the service's compositor HWND).
// Found via FindWindowW(L"DisplayXRD3D11", ...). Values are in display-centric
// coords — window center offset from display center (meters, +right/+up).
struct WindowMetrics {
	bool valid = false;
	int pixelW = 0;
	int pixelH = 0;
	float sizeWm = 0.0f;
	float sizeHm = 0.0f;
	float centerOffsetXm = 0.0f;
	float centerOffsetYm = 0.0f;
	uint32_t viewWidth = 0;   // Compositor's live per-view width (from HWND props)
	uint32_t viewHeight = 0;  // Compositor's live per-view height (from HWND props)
};

struct Bridge {
	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId system_id = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	XrSpace local_space = XR_NULL_HANDLE;
	bool has_display_info_ext = false;
	bool has_headless_ext = false;
	PFN_xrEnumerateDisplayRenderingModesEXT pfnEnumerateDisplayRenderingModes = nullptr;
	PFN_xrRequestDisplayRenderingModeEXT pfnRequestDisplayRenderingMode = nullptr;
	PFN_xrRequestEyeTrackingModeEXT pfnRequestEyeTrackingMode = nullptr;

	// Cached display info for JSON serialization.
	XrDisplayInfoEXT display_info{};
	std::vector<XrDisplayRenderingModeInfoEXT> modes;
	uint32_t current_mode_index = 0;
	std::vector<XrViewConfigurationView> config_views;

	// Compositor window metrics (polled from service HWND).
	WindowMetrics window_metrics;

	// Eye pose streaming.
	bool stream_eye_poses = false;
	bool session_begun = false;

	// WS state.
	MessageQueue outgoing;
	std::atomic<bool> ws_client_connected{false};

	// Bridge HUD shared memory (cross-process with compositor).
	HANDLE hud_mapping = nullptr;
	struct bridge_hud_shared *hud_shared = nullptr;
};

// ---------------------------------------------------------------------------
// Compositor window metrics probing.
// ---------------------------------------------------------------------------
//
// Bridge runs headless — no window of its own. The service hosts the real
// compositor window (class "DisplayXRD3D11"). We query it via Win32 so the
// bridge can advertise window-relative Kooima inputs to the page (matches the
// native cube_handle_d3d11_win app's approach at test_apps/cube_handle_d3d11_win/main.cpp:342-433).

static bool compute_window_metrics_impl(HWND hwnd, const Bridge &b, WindowMetrics &out) {
	if (hwnd == nullptr) return false;

	RECT cr;
	if (!GetClientRect(hwnd, &cr)) return false;
	int winPxW = (int)(cr.right - cr.left);
	int winPxH = (int)(cr.bottom - cr.top);
	if (winPxW <= 0 || winPxH <= 0) return false;

	POINT clientOrigin = {0, 0};
	if (!ClientToScreen(hwnd, &clientOrigin)) return false;

	HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi{};
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfoW(hMon, &mi)) return false;

	uint32_t dispPxW = b.display_info.displayPixelWidth;
	uint32_t dispPxH = b.display_info.displayPixelHeight;
	if (dispPxW == 0 || dispPxH == 0) return false;

	float pxSizeX = b.display_info.displaySizeMeters.width / (float)dispPxW;
	float pxSizeY = b.display_info.displaySizeMeters.height / (float)dispPxH;

	float winCenterX = (float)(clientOrigin.x - mi.rcMonitor.left) + (float)winPxW / 2.0f;
	float winCenterY = (float)(clientOrigin.y - mi.rcMonitor.top) + (float)winPxH / 2.0f;
	float dispScreenW = (float)(mi.rcMonitor.right - mi.rcMonitor.left);
	float dispScreenH = (float)(mi.rcMonitor.bottom - mi.rcMonitor.top);

	out.valid = true;
	out.pixelW = winPxW;
	out.pixelH = winPxH;
	out.sizeWm = (float)winPxW * pxSizeX;
	out.sizeHm = (float)winPxH * pxSizeY;
	out.centerOffsetXm = (winCenterX - dispScreenW / 2.0f) * pxSizeX;
	// Screen Y grows downward; display-centric Y grows upward. Flip sign.
	out.centerOffsetYm = -((winCenterY - dispScreenH / 2.0f) * pxSizeY);
	return true;
}

// Cached HWND so the WinEvent hook can filter by window without recomputing.
static std::atomic<HWND> g_compositor_hwnd{nullptr};
// Set by the WinEvent hook when the compositor window's geometry changes.
// Cleared by the main loop after polling metrics.
static std::atomic<bool> g_window_event_pending{false};

// Poll current metrics and update b.window_metrics. Returns true if values
// changed compared to the cached state (including valid↔invalid transitions).
static bool poll_window_metrics(Bridge &b) {
	HWND hwnd = FindWindowW(L"DisplayXRD3D11", nullptr);
	g_compositor_hwnd.store(hwnd);
	WindowMetrics neu;
	bool ok = compute_window_metrics_impl(hwnd, b, neu);
	// Read compositor's live per-view dims from HWND properties.
	if (hwnd) {
		neu.viewWidth = (uint32_t)(uintptr_t)GetPropW(hwnd, L"DXR_ViewW");
		neu.viewHeight = (uint32_t)(uintptr_t)GetPropW(hwnd, L"DXR_ViewH");
	}
	const WindowMetrics &old = b.window_metrics;
	const float eps = 0.0001f;
	bool changed = false;
	if (ok) {
		changed = !old.valid ||
		          old.pixelW != neu.pixelW || old.pixelH != neu.pixelH ||
		          std::fabs(old.sizeWm - neu.sizeWm) > eps ||
		          std::fabs(old.sizeHm - neu.sizeHm) > eps ||
		          std::fabs(old.centerOffsetXm - neu.centerOffsetXm) > eps ||
		          std::fabs(old.centerOffsetYm - neu.centerOffsetYm) > eps ||
		          old.viewWidth != neu.viewWidth || old.viewHeight != neu.viewHeight;
		b.window_metrics = neu;
	} else if (old.valid) {
		changed = true;
		b.window_metrics = WindowMetrics{};
	}
	return changed;
}

// ---------------------------------------------------------------------------
// Window event hook (event-driven resize / move detection).
// ---------------------------------------------------------------------------
//
// SetWinEventHook with EVENT_OBJECT_LOCATIONCHANGE fires whenever any window
// in the system moves or resizes. We filter by HWND (cached after the first
// successful FindWindowW call) and idObject == OBJID_WINDOW (ignore caret,
// cursor, etc). Hook runs on a dedicated thread with a message pump.

static void CALLBACK win_event_proc(HWINEVENTHOOK /*hook*/, DWORD /*event*/,
                                    HWND hwnd, LONG idObject, LONG /*idChild*/,
                                    DWORD /*eventThread*/, DWORD /*eventTime*/) {
	if (idObject != OBJID_WINDOW) return;
	HWND target = g_compositor_hwnd.load();
	if (target != nullptr && hwnd == target) {
		g_window_event_pending.store(true);
	}
}

// ---------------------------------------------------------------------------
// Raw input forwarding (keyboard + mouse) — bridge captures Win32 events
// targeting the compositor window and relays them as WS messages so the page
// owns input semantics. Compositor's qwerty processing is gated on
// !g_bridge_relay_active so there's no double-handling.
// ---------------------------------------------------------------------------

struct InputEvent {
	enum Kind { KEY, MOUSE, WHEEL };
	Kind kind;
	bool down;       // KEY/MOUSE: pressed vs released
	uint32_t code;   // KEY: virtual-key code
	bool repeat;     // KEY: auto-repeat
	int button;      // MOUSE: 0=left,1=right,2=middle (down/up only)
	int mevent;      // MOUSE: 0=move,1=down,2=up
	int x, y;        // MOUSE/WHEEL: client-area pixels (DPI physical)
	int buttons;     // MOUSE: held-button bitmask
	int wheel_delta; // WHEEL: WHEEL_DELTA units
	bool ctrl, shift, alt;
};

static std::mutex g_input_mtx;
static std::vector<InputEvent> g_input_queue; // Drained by main loop.

static void push_input_event(const InputEvent &e) {
	std::lock_guard<std::mutex> lk(g_input_mtx);
	if (g_input_queue.size() >= 256) g_input_queue.erase(g_input_queue.begin());
	g_input_queue.push_back(e);
}

// Cached focus check — only forward events when compositor window has focus.
static inline bool compositor_window_has_focus() {
	HWND target = g_compositor_hwnd.load();
	if (target == nullptr) return false;
	HWND fg = GetForegroundWindow();
	if (fg == target) return true;
	// Also accept descendants (e.g., child controls) of the compositor window.
	while (fg != nullptr) {
		fg = GetAncestor(fg, GA_PARENT);
		if (fg == target) return true;
	}
	return false;
}

static inline void fill_modifiers(InputEvent &e) {
	e.ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
	e.shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
	e.alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
}

static LRESULT CALLBACK keyboard_hook_proc(int nCode, WPARAM wParam, LPARAM lParam) {
	if (nCode == HC_ACTION && compositor_window_has_focus()) {
		KBDLLHOOKSTRUCT *kb = (KBDLLHOOKSTRUCT *)lParam;
		bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
		bool up = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
		if (down || up) {
			InputEvent e{};
			e.kind = InputEvent::KEY;
			e.down = down;
			e.code = kb->vkCode;
			e.repeat = down && (kb->flags & LLKHF_INJECTED) == 0; // best-effort
			fill_modifiers(e);
			push_input_event(e);
		}
	}
	return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

static LRESULT CALLBACK mouse_hook_proc(int nCode, WPARAM wParam, LPARAM lParam) {
	if (nCode == HC_ACTION && compositor_window_has_focus()) {
		MSLLHOOKSTRUCT *ms = (MSLLHOOKSTRUCT *)lParam;
		HWND target = g_compositor_hwnd.load();
		POINT p = ms->pt;
		if (target != nullptr) ScreenToClient(target, &p);

		InputEvent e{};
		e.x = p.x; e.y = p.y;
		// Compose held-button bitmask from async key state.
		int buttons = 0;
		if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) buttons |= 1;
		if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) buttons |= 2;
		if (GetAsyncKeyState(VK_MBUTTON) & 0x8000) buttons |= 4;
		e.buttons = buttons;
		fill_modifiers(e);

		switch (wParam) {
		case WM_MOUSEMOVE:
			// Only forward moves during drag (any button held). Idle cursor
			// movement floods the WS and can hang Chrome.
			if (buttons != 0) {
				e.kind = InputEvent::MOUSE; e.mevent = 0;
				push_input_event(e);
			}
			break;
		case WM_LBUTTONDOWN: e.kind = InputEvent::MOUSE; e.mevent = 1; e.button = 0; e.down = true;  push_input_event(e); break;
		case WM_LBUTTONUP:   e.kind = InputEvent::MOUSE; e.mevent = 2; e.button = 0; e.down = false; push_input_event(e); break;
		case WM_RBUTTONDOWN: e.kind = InputEvent::MOUSE; e.mevent = 1; e.button = 1; e.down = true;  push_input_event(e); break;
		case WM_RBUTTONUP:   e.kind = InputEvent::MOUSE; e.mevent = 2; e.button = 1; e.down = false; push_input_event(e); break;
		case WM_MBUTTONDOWN: e.kind = InputEvent::MOUSE; e.mevent = 1; e.button = 2; e.down = true;  push_input_event(e); break;
		case WM_MBUTTONUP:   e.kind = InputEvent::MOUSE; e.mevent = 2; e.button = 2; e.down = false; push_input_event(e); break;
		case WM_MOUSEWHEEL: {
			e.kind = InputEvent::WHEEL;
			e.wheel_delta = (int)(short)HIWORD(ms->mouseData);
			push_input_event(e);
			break;
		}
		default: break;
		}
	}
	return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

static void window_event_thread_func() {
	HWINEVENTHOOK win_hook = SetWinEventHook(
	    EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
	    nullptr, win_event_proc, 0, 0,
	    WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
	if (win_hook == nullptr) {
		LOG_W("SetWinEventHook failed: %lu (window resize will fall back to polling)",
		      GetLastError());
	} else {
		LOG_I("Window event hook installed (EVENT_OBJECT_LOCATIONCHANGE)");
	}

	HHOOK kbd_hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_hook_proc,
	                                    GetModuleHandleW(nullptr), 0);
	if (kbd_hook == nullptr) {
		LOG_W("SetWindowsHookEx(WH_KEYBOARD_LL) failed: %lu (keyboard input will not forward)",
		      GetLastError());
	} else {
		LOG_I("Keyboard hook installed (WH_KEYBOARD_LL)");
	}

	HHOOK mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_hook_proc,
	                                      GetModuleHandleW(nullptr), 0);
	if (mouse_hook == nullptr) {
		LOG_W("SetWindowsHookEx(WH_MOUSE_LL) failed: %lu (mouse input will not forward)",
		      GetLastError());
	} else {
		LOG_I("Mouse hook installed (WH_MOUSE_LL)");
	}

	MSG msg;
	while (g_running.load()) {
		if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		} else {
			Sleep(10);
		}
	}
	if (mouse_hook) UnhookWindowsHookEx(mouse_hook);
	if (kbd_hook) UnhookWindowsHookEx(kbd_hook);
	if (win_hook) UnhookWinEvent(win_hook);
	LOG_I("Input + window hooks removed");
}

// ---------------------------------------------------------------------------
// JSON message builders.
// ---------------------------------------------------------------------------

static std::string build_views_json(const std::vector<XrViewConfigurationView> &views) {
	std::string s = "[";
	for (size_t i = 0; i < views.size(); i++) {
		if (i) s += ",";
		s += "{\"index\":" + json_u((uint32_t)i)
		   + ",\"recommendedImageRectWidth\":" + json_u(views[i].recommendedImageRectWidth)
		   + ",\"recommendedImageRectHeight\":" + json_u(views[i].recommendedImageRectHeight)
		   + ",\"maxImageRectWidth\":" + json_u(views[i].maxImageRectWidth)
		   + ",\"maxImageRectHeight\":" + json_u(views[i].maxImageRectHeight) + "}";
	}
	return s + "]";
}

static std::string build_window_info_fields(const WindowMetrics &w) {
	// Inline fragment (no surrounding braces) — embedded into display-info or
	// the standalone window-info message.
	std::string s = ",\"windowInfo\":{\"valid\":";
	s += (w.valid ? "true" : "false");
	if (w.valid) {
		s += ",\"windowPixelSize\":[" + json_u((uint32_t)w.pixelW) + "," + json_u((uint32_t)w.pixelH) + "]";
		s += ",\"windowSizeMeters\":[" + json_f(w.sizeWm) + "," + json_f(w.sizeHm) + "]";
		s += ",\"windowCenterOffsetMeters\":[" + json_f(w.centerOffsetXm) + "," + json_f(w.centerOffsetYm) + "]";
		if (w.viewWidth > 0 && w.viewHeight > 0) {
			s += ",\"viewWidth\":" + json_u(w.viewWidth);
			s += ",\"viewHeight\":" + json_u(w.viewHeight);
		}
	}
	s += "}";
	return s;
}

static std::string build_window_info_json(const WindowMetrics &w) {
	std::string s = "{\"type\":\"window-info\",\"version\":1";
	// Reuse the same nested object so the field shape matches display-info.
	s += build_window_info_fields(w);
	s += "}";
	return s;
}

static std::string build_display_info_json(const Bridge &b) {
	const auto &di = b.display_info;
	std::string s = "{\"type\":\"display-info\",\"version\":1";
	s += ",\"displayPixelSize\":[" + json_u(di.displayPixelWidth) + "," + json_u(di.displayPixelHeight) + "]";
	s += ",\"displaySizeMeters\":[" + json_f(di.displaySizeMeters.width) + "," + json_f(di.displaySizeMeters.height) + "]";
	s += ",\"recommendedViewScale\":[" + json_f(di.recommendedViewScaleX) + "," + json_f(di.recommendedViewScaleY) + "]";
	s += ",\"nominalViewerPosition\":[" + json_f(di.nominalViewerPositionInDisplaySpace.x)
	   + "," + json_f(di.nominalViewerPositionInDisplaySpace.y)
	   + "," + json_f(di.nominalViewerPositionInDisplaySpace.z) + "]";

	s += ",\"renderingModes\":[";
	for (size_t i = 0; i < b.modes.size(); i++) {
		if (i) s += ",";
		const auto &m = b.modes[i];
		s += "{\"index\":" + json_u(m.modeIndex)
		   + ",\"name\":\"" + json_escape(m.modeName) + "\""
		   + ",\"viewCount\":" + json_u(m.viewCount)
		   + ",\"tileColumns\":" + json_u(m.tileColumns)
		   + ",\"tileRows\":" + json_u(m.tileRows)
		   + ",\"viewScale\":[" + json_f(m.viewScaleX) + "," + json_f(m.viewScaleY) + "]"
		   + ",\"hardware3D\":" + (m.hardwareDisplay3D ? "true" : "false") + "}";
	}
	s += "]";

	s += ",\"currentModeIndex\":" + json_u(b.current_mode_index);
	s += ",\"views\":" + build_views_json(b.config_views);
	s += build_window_info_fields(b.window_metrics);
	s += "}";
	return s;
}

static std::string build_mode_changed_json(uint32_t prev, uint32_t curr, bool hw3d,
                                           const std::vector<XrViewConfigurationView> &views) {
	std::string s = "{\"type\":\"mode-changed\",\"version\":1";
	s += ",\"previousModeIndex\":" + json_u(prev);
	s += ",\"currentModeIndex\":" + json_u(curr);
	s += ",\"hardware3D\":" + std::string(hw3d ? "true" : "false");
	s += ",\"views\":" + build_views_json(views);
	s += "}";
	return s;
}

static std::string build_hardware_state_json(bool hw3d) {
	return "{\"type\":\"hardware-state-changed\",\"version\":1,\"hardware3D\":"
	       + std::string(hw3d ? "true" : "false") + "}";
}

static std::string build_input_event_json(const InputEvent &e) {
	std::string s = "{\"type\":\"input\",\"version\":1";
	s += ",\"modifiers\":{\"ctrl\":";
	s += (e.ctrl ? "true" : "false");
	s += ",\"shift\":"; s += (e.shift ? "true" : "false");
	s += ",\"alt\":";   s += (e.alt   ? "true" : "false");
	s += "}";
	switch (e.kind) {
	case InputEvent::KEY:
		s += ",\"kind\":\"key\"";
		s += ",\"down\":"; s += (e.down ? "true" : "false");
		s += ",\"code\":" + json_u(e.code);
		s += ",\"repeat\":"; s += (e.repeat ? "true" : "false");
		break;
	case InputEvent::MOUSE:
		s += ",\"kind\":\"mouse\"";
		s += ",\"event\":\"";
		s += (e.mevent == 0 ? "move" : (e.mevent == 1 ? "down" : "up"));
		s += "\"";
		if (e.mevent != 0) s += ",\"button\":" + json_i(e.button);
		s += ",\"x\":" + json_i(e.x);
		s += ",\"y\":" + json_i(e.y);
		s += ",\"buttons\":" + json_i(e.buttons);
		break;
	case InputEvent::WHEEL:
		s += ",\"kind\":\"wheel\"";
		s += ",\"deltaY\":" + json_i(e.wheel_delta);
		s += ",\"x\":" + json_i(e.x);
		s += ",\"y\":" + json_i(e.y);
		break;
	}
	s += "}";
	return s;
}

static std::string build_eye_poses_json(const XrView *views, uint32_t count) {
	std::string s = "{\"type\":\"eye-poses\",\"version\":1,\"format\":\"raw\",\"eyes\":[";
	for (uint32_t i = 0; i < count; i++) {
		if (i) s += ",";
		const auto &p = views[i].pose.position;
		const auto &o = views[i].pose.orientation;
		s += "{\"position\":[" + json_f(p.x) + "," + json_f(p.y) + "," + json_f(p.z) + "]"
		   + ",\"orientation\":[" + json_f(o.x) + "," + json_f(o.y) + "," + json_f(o.z) + "," + json_f(o.w) + "]"
		   + ",\"fov\":{\"angleLeft\":" + json_f(views[i].fov.angleLeft)
		   + ",\"angleRight\":" + json_f(views[i].fov.angleRight)
		   + ",\"angleUp\":" + json_f(views[i].fov.angleUp)
		   + ",\"angleDown\":" + json_f(views[i].fov.angleDown) + "}}";
	}
	s += "]}";
	return s;
}

// ---------------------------------------------------------------------------
// Parse incoming WS messages from the extension.
// ---------------------------------------------------------------------------

static void handle_ws_message(Bridge &b, const std::string &msg) {
	auto find_str = [&](const char *key) -> std::string {
		std::string needle = std::string("\"") + key + "\":\"";
		size_t pos = msg.find(needle);
		if (pos == std::string::npos) return "";
		pos += needle.size();
		size_t end = msg.find('"', pos);
		return (end != std::string::npos) ? msg.substr(pos, end - pos) : "";
	};
	auto find_int = [&](const char *key) -> int {
		std::string needle = std::string("\"") + key + "\":";
		size_t pos = msg.find(needle);
		if (pos == std::string::npos) return -1;
		return std::atoi(msg.c_str() + pos + needle.size());
	};

	std::string type = find_str("type");

	// Debug: log all incoming WS message types
	if (!type.empty()) {
		static int ws_msg_count = 0;
		if (++ws_msg_count <= 20 || type != "configure") {
			LOG_I("WS recv type='%s' len=%zu", type.c_str(), msg.size());
		}
	}

	if (type == "hello") {
		int version = find_int("version");
		if (version != 1) {
			LOG_W("WS hello: unsupported version %d", version);
			return;
		}
		LOG_I("WS hello received, sending display-info");
		// Refresh window metrics on hello so initial display-info carries them.
		poll_window_metrics(b);
		b.outgoing.push(build_display_info_json(b));
	} else if (type == "request-mode") {
		int idx = find_int("modeIndex");
		if (idx < 0 || (uint32_t)idx >= b.modes.size()) {
			LOG_W("WS request-mode: invalid modeIndex %d", idx);
			return;
		}
		// Relay to compositor via HWND property. The compositor polls this
		// each frame and triggers the server-side mode change (DP toggle).
		// +1 encoding: 0 means "no request", 1=mode 0, 2=mode 1, etc.
		HWND hwnd = g_compositor_hwnd.load();
		if (hwnd) {
			SetPropW(hwnd, L"DXR_RequestMode", (HANDLE)(uintptr_t)(idx + 1));
			LOG_I("WS request-mode %d: posted to compositor", idx);
		} else {
			LOG_W("WS request-mode %d: no compositor HWND yet", idx);
		}
	} else if (type == "request-eye-tracking-mode") {
		int mode = find_int("mode");
		if (b.pfnRequestEyeTrackingMode && b.session != XR_NULL_HANDLE) {
			XrEyeTrackingModeEXT xr_mode = (mode == 1)
			    ? XR_EYE_TRACKING_MODE_MANUAL_EXT : XR_EYE_TRACKING_MODE_MANAGED_EXT;
			XrResult r = b.pfnRequestEyeTrackingMode(b.session, xr_mode);
			LOG_I("WS request-eye-tracking-mode %d: %s", mode, xr_result_str(b.instance, r));
		}
	} else if (type == "configure") {
		std::string fmt = find_str("eyePoseFormat");
		if (fmt == "raw") {
			b.stream_eye_poses = true;
			LOG_I("WS configure: eye pose streaming ON (raw)");
		} else if (fmt == "none" || fmt == "render-ready") {
			b.stream_eye_poses = false;
			LOG_I("WS configure: eye pose streaming OFF");
		}
	} else if (type == "hud-update") {
		if (b.hud_shared) {
			// Parse "visible" and "lines" array from JSON.
			bool vis = false;
			{
				auto p = msg.find("\"visible\"");
				if (p != std::string::npos) {
					vis = (msg.find("true", p) == p + 10); // crude but matches {"visible":true
				}
			}
			b.hud_shared->visible = vis ? 1 : 0;
			b.hud_shared->line_count = 0;

			if (vis) {
				// Parse lines array: each {"label":"...","text":"..."}
				size_t pos = msg.find("\"lines\"");
				if (pos != std::string::npos) {
					uint32_t count = 0;
					size_t search = pos;
					while (count < BRIDGE_HUD_MAX_LINES) {
						size_t lb = msg.find("\"label\"", search);
						if (lb == std::string::npos) break;
						size_t tb = msg.find("\"text\"", lb);
						if (tb == std::string::npos) break;

						auto extract = [&](size_t key_end) -> std::string {
							size_t q1 = msg.find('"', key_end + 1);
							if (q1 == std::string::npos) return "";
							q1++; // skip opening quote
							size_t q2 = msg.find('"', q1);
							if (q2 == std::string::npos) return "";
							return msg.substr(q1, q2 - q1);
						};

						std::string label = extract(lb + 6);
						std::string text = extract(tb + 5);

						auto &line = b.hud_shared->lines[count];
						strncpy(line.label, label.c_str(), BRIDGE_HUD_LABEL_LEN - 1);
						line.label[BRIDGE_HUD_LABEL_LEN - 1] = '\0';
						strncpy(line.text, text.c_str(), BRIDGE_HUD_TEXT_LEN - 1);
						line.text[BRIDGE_HUD_TEXT_LEN - 1] = '\0';

						count++;
						search = tb + 5;
					}
					b.hud_shared->line_count = count;
				}
			}
		}
	} else if (!type.empty()) {
		LOG_I("WS unknown message type '%s'", type.c_str());
	}
}

// ---------------------------------------------------------------------------
// WebSocket server thread.
// ---------------------------------------------------------------------------

static void ws_thread_func(Bridge &b) {
	SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listen_sock == INVALID_SOCKET) {
		LOG_E("WS socket() failed: %d", WSAGetLastError());
		return;
	}

	int opt = 1;
	setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

	struct sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(WS_PORT);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
		LOG_E("WS bind(127.0.0.1:%d) failed: %d", WS_PORT, WSAGetLastError());
		closesocket(listen_sock);
		return;
	}
	if (listen(listen_sock, 1) == SOCKET_ERROR) {
		LOG_E("WS listen() failed: %d", WSAGetLastError());
		closesocket(listen_sock);
		return;
	}
	LOG_I("WS server listening on 127.0.0.1:%d", WS_PORT);

	while (g_running.load()) {
		// Accept with timeout so we can check g_running.
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(listen_sock, &fds);
		struct timeval tv = {1, 0};
		if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;

		SOCKET client = accept(listen_sock, nullptr, nullptr);
		if (client == INVALID_SOCKET) continue;

		// Reject second client.
		if (b.ws_client_connected.load()) {
			LOG_W("WS rejecting second client (single-client mode)");
			ws_send_close(client, 1008);
			closesocket(client);
			continue;
		}

		if (!ws_do_handshake(client)) {
			closesocket(client);
			continue;
		}

		LOG_I("WS client connected");
		b.ws_client_connected.store(true);

		// Client loop: send outgoing messages, receive incoming.
		while (g_running.load() && b.ws_client_connected.load()) {
			// Drain outgoing queue.
			std::string out_msg;
			while (b.outgoing.pop(out_msg, 0)) {
				if (!ws_send_text(client, out_msg)) {
					LOG_W("WS send failed, disconnecting client");
					b.ws_client_connected.store(false);
					break;
				}
			}
			if (!b.ws_client_connected.load()) break;

			// Check for incoming message (non-blocking).
			std::string in_msg;
			if (ws_recv_text(client, in_msg, 10)) {
				if (!in_msg.empty()) {
					handle_ws_message(b, in_msg);
				}
			} else {
				// recv returned false — check if socket is dead.
				fd_set errfds;
				FD_ZERO(&errfds);
				FD_SET(client, &errfds);
				struct timeval zero = {0, 0};
				if (select(0, nullptr, nullptr, &errfds, &zero) > 0) {
					LOG_I("WS client disconnected (socket error)");
					b.ws_client_connected.store(false);
					break;
				}
			}
		}

		b.stream_eye_poses = false;
		b.ws_client_connected.store(false);
		closesocket(client);
		LOG_I("WS client disconnected");
	}

	closesocket(listen_sock);
	LOG_I("WS server shut down");
}

// ---------------------------------------------------------------------------
// Environment setup.
// ---------------------------------------------------------------------------

static void force_ipc_mode_env() {
	// Must use SetEnvironmentVariableA (Win32 API) instead of _putenv (CRT).
	// The bridge uses static CRT (/MT), and DisplayXRClient.dll also uses
	// static CRT — each has its own C runtime environment block. _putenv
	// only affects the bridge's CRT, invisible to the DLL. The Win32 API
	// sets the process-level environment, visible to all CRTs.
	char buf[256];
	if (GetEnvironmentVariableA("XRT_FORCE_MODE", buf, sizeof(buf)) > 0 && buf[0] != '\0') {
		LOG_I("XRT_FORCE_MODE already set to '%s' (leaving as-is)", buf);
	} else {
		SetEnvironmentVariableA("XRT_FORCE_MODE", "ipc");
		LOG_I("XRT_FORCE_MODE=ipc (forced by bridge — IPC to displayxr-service required)");
	}
	if (GetEnvironmentVariableA("XRT_LOG", buf, sizeof(buf)) == 0 || buf[0] == '\0') {
		SetEnvironmentVariableA("XRT_LOG", "info");
	}
}

// ---------------------------------------------------------------------------
// Setup helpers.
// ---------------------------------------------------------------------------

static bool create_instance(Bridge &b) {
	uint32_t ext_count = 0;
	XR_CHECK(b.instance, xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr));

	std::vector<XrExtensionProperties> exts(ext_count, {XR_TYPE_EXTENSION_PROPERTIES});
	XR_CHECK(b.instance,
	         xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, exts.data()));

	LOG_I("Runtime exposes %u extensions", ext_count);
	for (const auto &e : exts) {
		if (std::strcmp(e.extensionName, XR_EXT_DISPLAY_INFO_EXTENSION_NAME) == 0) {
			b.has_display_info_ext = true;
		}
		if (std::strcmp(e.extensionName, XR_MND_HEADLESS_EXTENSION_NAME) == 0) {
			b.has_headless_ext = true;
		}
	}
	LOG_I("XR_EXT_display_info: %s", b.has_display_info_ext ? "yes" : "NO");
	LOG_I("XR_MND_headless:     %s", b.has_headless_ext ? "yes" : "NO");

	if (!b.has_display_info_ext) {
		LOG_E("XR_EXT_display_info is required by this bridge; aborting");
		return false;
	}
	if (!b.has_headless_ext) {
		LOG_E("XR_MND_headless is required by this bridge (headless metadata session); aborting");
		return false;
	}

	std::vector<const char *> enabled_exts;
	enabled_exts.push_back(XR_EXT_DISPLAY_INFO_EXTENSION_NAME);
	enabled_exts.push_back(XR_MND_HEADLESS_EXTENSION_NAME);

	XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
	std::strncpy(ici.applicationInfo.applicationName, "displayxr-webxr-bridge",
	             XR_MAX_APPLICATION_NAME_SIZE - 1);
	ici.applicationInfo.applicationVersion = 1;
	std::strncpy(ici.applicationInfo.engineName, "DisplayXR",
	             XR_MAX_ENGINE_NAME_SIZE - 1);
	ici.applicationInfo.engineVersion = 1;
	ici.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
	ici.enabledExtensionCount = (uint32_t)enabled_exts.size();
	ici.enabledExtensionNames = enabled_exts.data();

	XR_CHECK(b.instance, xrCreateInstance(&ici, &b.instance));
	LOG_I("xrCreateInstance OK");

	XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
	if (XR_SUCCEEDED(xrGetInstanceProperties(b.instance, &ip))) {
		LOG_I("Runtime: %s v%u.%u.%u", ip.runtimeName,
		      XR_VERSION_MAJOR(ip.runtimeVersion),
		      XR_VERSION_MINOR(ip.runtimeVersion),
		      XR_VERSION_PATCH(ip.runtimeVersion));
	}

	return true;
}

static bool get_system_and_display_info(Bridge &b) {
	XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XR_CHECK(b.instance, xrGetSystem(b.instance, &sgi, &b.system_id));
	LOG_I("xrGetSystem OK, systemId=%llu", (unsigned long long)b.system_id);

	XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
	b.display_info = {(XrStructureType)XR_TYPE_DISPLAY_INFO_EXT};
	sp.next = &b.display_info;
	XR_CHECK(b.instance, xrGetSystemProperties(b.instance, b.system_id, &sp));

	const auto &di = b.display_info;
	LOG_I("System name: %s", sp.systemName);
	LOG_I("Display info:");
	LOG_I("  displayPixelSize       : %u x %u", di.displayPixelWidth, di.displayPixelHeight);
	LOG_I("  displaySizeMeters      : %.4f x %.4f", di.displaySizeMeters.width, di.displaySizeMeters.height);
	LOG_I("  recommendedViewScale   : %.3f x %.3f", di.recommendedViewScaleX, di.recommendedViewScaleY);
	LOG_I("  nominalViewerPosition  : (%.4f, %.4f, %.4f) m",
	      di.nominalViewerPositionInDisplaySpace.x,
	      di.nominalViewerPositionInDisplaySpace.y,
	      di.nominalViewerPositionInDisplaySpace.z);

	uint32_t view_count = 0;
	XR_CHECK(b.instance, xrEnumerateViewConfigurationViews(
	                         b.instance, b.system_id,
	                         XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
	                         &view_count, nullptr));
	b.config_views.resize(view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
	XR_CHECK(b.instance, xrEnumerateViewConfigurationViews(
	                         b.instance, b.system_id,
	                         XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, view_count,
	                         &view_count, b.config_views.data()));
	LOG_I("PRIMARY_STEREO view configuration (%u views):", view_count);
	for (uint32_t i = 0; i < view_count; i++) {
		LOG_I("  view[%u] recommended=%ux%u max=%ux%u", i,
		      b.config_views[i].recommendedImageRectWidth, b.config_views[i].recommendedImageRectHeight,
		      b.config_views[i].maxImageRectWidth, b.config_views[i].maxImageRectHeight);
	}

	return true;
}

static bool create_session_and_enumerate_modes(Bridge &b) {
	XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
	sci.systemId = b.system_id;
	XR_CHECK(b.instance, xrCreateSession(b.instance, &sci, &b.session));
	LOG_I("xrCreateSession OK (headless)");

	// Create LOCAL reference space for xrLocateViews.
	XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
	rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	rsci.poseInReferenceSpace.orientation = {0, 0, 0, 1};
	rsci.poseInReferenceSpace.position = {0, 0, 0};
	XrResult rs_r = xrCreateReferenceSpace(b.session, &rsci, &b.local_space);
	if (XR_FAILED(rs_r)) {
		LOG_W("xrCreateReferenceSpace(LOCAL) failed: %s (eye pose streaming unavailable)",
		      xr_result_str(b.instance, rs_r));
	} else {
		LOG_I("LOCAL reference space created for eye pose streaming");
	}

	// Rendering mode enumeration.
	XrResult r = xrGetInstanceProcAddr(
	    b.instance, "xrEnumerateDisplayRenderingModesEXT",
	    (PFN_xrVoidFunction *)&b.pfnEnumerateDisplayRenderingModes);
	if (XR_FAILED(r) || b.pfnEnumerateDisplayRenderingModes == nullptr) {
		LOG_W("xrEnumerateDisplayRenderingModesEXT not resolved: %s",
		      xr_result_str(b.instance, r));
		return true;
	}

	xrGetInstanceProcAddr(b.instance, "xrRequestDisplayRenderingModeEXT",
	                      (PFN_xrVoidFunction *)&b.pfnRequestDisplayRenderingMode);
	xrGetInstanceProcAddr(b.instance, "xrRequestEyeTrackingModeEXT",
	                      (PFN_xrVoidFunction *)&b.pfnRequestEyeTrackingMode);

	uint32_t mode_count = 0;
	r = b.pfnEnumerateDisplayRenderingModes(b.session, 0, &mode_count, nullptr);
	if (XR_FAILED(r) || mode_count == 0) {
		LOG_W("xrEnumerateDisplayRenderingModesEXT(count) returned %s, count=%u",
		      xr_result_str(b.instance, r), mode_count);
		return true;
	}
	b.modes.resize(mode_count);
	for (auto &m : b.modes) {
		m.type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_EXT;
		m.next = nullptr;
	}
	r = b.pfnEnumerateDisplayRenderingModes(b.session, mode_count, &mode_count, b.modes.data());
	if (XR_FAILED(r)) {
		LOG_W("xrEnumerateDisplayRenderingModesEXT(data) returned %s",
		      xr_result_str(b.instance, r));
		return true;
	}

	LOG_I("Display rendering modes (%u):", mode_count);
	for (uint32_t i = 0; i < mode_count; i++) {
		const auto &m = b.modes[i];
		LOG_I("  [%u] \"%s\" views=%u tiles=%ux%u viewScale=%.3fx%.3f hw3D=%d",
		      m.modeIndex, m.modeName, m.viewCount, m.tileColumns, m.tileRows,
		      m.viewScaleX, m.viewScaleY, (int)m.hardwareDisplay3D);
	}

	// Default current_mode_index to the first 3D mode (Leia device starts in 3D).
	// XrDisplayInfoEXT doesn't expose the active mode, so infer from hardware3D.
	for (uint32_t i = 0; i < mode_count; i++) {
		if (b.modes[i].hardwareDisplay3D) {
			b.current_mode_index = i;
			break;
		}
	}
	LOG_I("Initial current_mode_index: %u", b.current_mode_index);

	return true;
}

// ---------------------------------------------------------------------------
// Re-read view config after mode change.
// ---------------------------------------------------------------------------

static void refresh_view_config(Bridge &b, const char *reason) {
	uint32_t view_count = 0;
	XrResult r = xrEnumerateViewConfigurationViews(
	    b.instance, b.system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
	    &view_count, nullptr);
	if (XR_FAILED(r)) {
		LOG_W("re-enumerate view config (%s): %s", reason, xr_result_str(b.instance, r));
		return;
	}
	b.config_views.resize(view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
	r = xrEnumerateViewConfigurationViews(
	    b.instance, b.system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
	    view_count, &view_count, b.config_views.data());
	if (XR_FAILED(r)) return;
	LOG_I("  post-%s view config (%u views):", reason, view_count);
	for (uint32_t i = 0; i < view_count; i++) {
		LOG_I("    view[%u] recommended=%ux%u", i, b.config_views[i].recommendedImageRectWidth,
		      b.config_views[i].recommendedImageRectHeight);
	}
}

// ---------------------------------------------------------------------------
// Event pump.
// ---------------------------------------------------------------------------

static void handle_event(Bridge &b, const XrEventDataBuffer &evt) {
	switch ((int)evt.type) {
	case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: {
		auto *e = reinterpret_cast<const XrEventDataInstanceLossPending *>(&evt);
		LOG_W("INSTANCE_LOSS_PENDING lossTime=%lld", (long long)e->lossTime);
		g_running.store(false);
	} break;

	case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
		auto *e = reinterpret_cast<const XrEventDataSessionStateChanged *>(&evt);
		LOG_I("SESSION_STATE_CHANGED state=%d", (int)e->state);
		if (e->state == XR_SESSION_STATE_READY && b.session != XR_NULL_HANDLE) {
			XrSessionBeginInfo sbi{XR_TYPE_SESSION_BEGIN_INFO};
			sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			XrResult r = xrBeginSession(b.session, &sbi);
			LOG_I("  xrBeginSession on READY: %s", xr_result_str(b.instance, r));
			if (XR_SUCCEEDED(r)) b.session_begun = true;
		} else if (e->state == XR_SESSION_STATE_STOPPING) {
			xrEndSession(b.session);
			b.session_begun = false;
			LOG_I("  xrEndSession on STOPPING");
		} else if (e->state == XR_SESSION_STATE_EXITING ||
		           e->state == XR_SESSION_STATE_LOSS_PENDING) {
			g_running.store(false);
		}
	} break;

	case XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_EXT: {
		auto *e = reinterpret_cast<const XrEventDataRenderingModeChangedEXT *>(&evt);
		uint32_t prev = b.current_mode_index;
		b.current_mode_index = e->currentModeIndex;
		LOG_I("RENDERING_MODE_CHANGED previous=%u current=%u", e->previousModeIndex,
		      e->currentModeIndex);
		refresh_view_config(b, "mode-change");

		bool hw3d = false;
		if (e->currentModeIndex < b.modes.size())
			hw3d = b.modes[e->currentModeIndex].hardwareDisplay3D;

		if (b.ws_client_connected.load()) {
			b.outgoing.push(build_mode_changed_json(
			    e->previousModeIndex, e->currentModeIndex, hw3d, b.config_views));
		}
	} break;

	case XR_TYPE_EVENT_DATA_HARDWARE_DISPLAY_STATE_CHANGED_EXT: {
		LOG_I("HARDWARE_DISPLAY_STATE_CHANGED_EXT (physical 3D state flipped)");
		if (b.ws_client_connected.load()) {
			bool hw3d = false;
			if (b.current_mode_index < b.modes.size())
				hw3d = b.modes[b.current_mode_index].hardwareDisplay3D;
			b.outgoing.push(build_hardware_state_json(hw3d));
		}
	} break;

	default:
		LOG_I("event type=%d (unhandled)", (int)evt.type);
		break;
	}
}

// ---------------------------------------------------------------------------
// Eye pose polling (RAW via xrLocateViews).
// ---------------------------------------------------------------------------

static void poll_eye_poses(Bridge &b) {
	if (!b.stream_eye_poses || b.local_space == XR_NULL_HANDLE || !b.session_begun)
		return;
	if (!b.ws_client_connected.load())
		return;

	// Backpressure: skip if the outgoing queue has pending messages.
	// Eye poses are stateless — only the latest matters. Dropping
	// intermediate samples prevents TCP buffer overflow which causes
	// WS disconnect/reconnect loops and loses mode-changed events.
	{
		std::lock_guard<std::mutex> lk(b.outgoing.mtx);
		if (b.outgoing.q.size() > 2) return;
	}

	XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
	vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	vli.displayTime = now.QuadPart;
	vli.space = b.local_space;

	XrViewState vs{XR_TYPE_VIEW_STATE};
	uint32_t view_count = 8;
	XrView views[8];
	for (uint32_t i = 0; i < 8; i++) views[i] = {XR_TYPE_VIEW};

	XrResult r = xrLocateViews(b.session, &vli, &vs, 8, &view_count, views);
	if (XR_FAILED(r)) return;

	b.outgoing.push(build_eye_poses_json(views, view_count));
}

// ---------------------------------------------------------------------------
// Main event loop.
// ---------------------------------------------------------------------------

static void bridge_hud_create(Bridge &b) {
	b.hud_mapping = CreateFileMappingW(
	    INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
	    0, sizeof(struct bridge_hud_shared), BRIDGE_HUD_MAPPING_NAME);
	if (b.hud_mapping) {
		b.hud_shared = (struct bridge_hud_shared *)MapViewOfFile(
		    b.hud_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(struct bridge_hud_shared));
		if (b.hud_shared) {
			memset(b.hud_shared, 0, sizeof(*b.hud_shared));
			b.hud_shared->magic = BRIDGE_HUD_MAGIC;
			b.hud_shared->version = BRIDGE_HUD_VERSION;
			LOG_I("Bridge HUD shared memory created");
		}
	}
}

static void bridge_hud_destroy(Bridge &b) {
	if (b.hud_shared) {
		b.hud_shared->visible = 0;
		UnmapViewOfFile(b.hud_shared);
		b.hud_shared = nullptr;
	}
	if (b.hud_mapping) {
		CloseHandle(b.hud_mapping);
		b.hud_mapping = nullptr;
	}
}

static void run_event_loop(Bridge &b) {
	LOG_I("Entering event loop. Ctrl+C to exit.");
	int window_poll_counter = 0;
	while (g_running.load()) {
		XrEventDataBuffer evt{XR_TYPE_EVENT_DATA_BUFFER};
		XrResult r = xrPollEvent(b.instance, &evt);
		if (r == XR_SUCCESS) {
			handle_event(b, evt);
		} else if (r == XR_EVENT_UNAVAILABLE) {
			poll_eye_poses(b);

			// Event-driven resize: the WinEvent hook flips this flag whenever
			// the compositor window moves or resizes. Drain it every loop
			// iteration (~10 ms latency to client).
			bool window_changed = false;
			if (g_window_event_pending.exchange(false)) {
				window_changed = poll_window_metrics(b);
			}
			// Poll for compositor window. Before the window is found, poll
			// every frame so the sample gets window info ASAP (the sample
			// needs windowPixelSize × viewScale to render at correct tile
			// dims). After the window is found, fall back to ~500 ms.
			if (!b.window_metrics.valid || ++window_poll_counter >= 50) {
				window_poll_counter = 0;
				window_changed = poll_window_metrics(b) || window_changed;
			}
			if (window_changed && b.ws_client_connected.load()) {
				b.outgoing.push(build_window_info_json(b.window_metrics));
			}

			// Drain queued input events from the hook thread.
			if (b.ws_client_connected.load()) {
				std::vector<InputEvent> drained;
				{
					std::lock_guard<std::mutex> lk(g_input_mtx);
					drained.swap(g_input_queue);
				}
				for (const auto &ev : drained) {
					b.outgoing.push(build_input_event_json(ev));
				}
			}
			Sleep(10);
		} else {
			LOG_W("xrPollEvent error: %s", xr_result_str(b.instance, r));
			Sleep(100);
		}
	}
}

// ---------------------------------------------------------------------------
// Entry point.
// ---------------------------------------------------------------------------

int main() {
	SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

	// Per-monitor DPI awareness so GetClientRect / GetMonitorInfo on the
	// compositor's HWND return PHYSICAL pixels (matching the swap chain
	// dimensions the runtime sees). Without this, on a system at 125% DPI,
	// a 1920×1080 fullscreen window appears as 1536×864 → bridge's
	// windowPixelSize would mismatch the compositor's atlas tile dims.
	if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
		// Fallback for older Windows: SetProcessDPIAware is system-wide
		// (no per-monitor) but still gives physical pixels.
		LOG_W("SetProcessDpiAwarenessContext failed: %lu (falling back to SetProcessDPIAware)",
		      GetLastError());
		SetProcessDPIAware();
	}

	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		LOG_E("WSAStartup failed");
		return 1;
	}

	LOG_I("=== DisplayXR WebXR Bridge v2 — host (Phase 2) ===");
	LOG_I("Metadata + eye-pose sideband; frames stay on Chrome's native WebXR path.");

	force_ipc_mode_env();

	Bridge b;

	if (!create_instance(b)) {
		LOG_E("xrCreateInstance failed in IPC mode — is displayxr-service running?");
		LOG_E("  Start Chrome WebXR (Enter VR) first so the service is up, then re-run.");
		WSACleanup();
		return 1;
	}
	if (!get_system_and_display_info(b)) {
		if (b.instance != XR_NULL_HANDLE) xrDestroyInstance(b.instance);
		WSACleanup();
		return 1;
	}
	if (!create_session_and_enumerate_modes(b)) {
		if (b.session != XR_NULL_HANDLE) xrDestroySession(b.session);
		if (b.instance != XR_NULL_HANDLE) xrDestroyInstance(b.instance);
		WSACleanup();
		return 1;
	}

	// Create bridge HUD shared memory for cross-process HUD overlay.
	bridge_hud_create(b);

	// Start WS server + window-event hook on dedicated threads.
	std::thread ws_thread(ws_thread_func, std::ref(b));
	std::thread win_evt_thread(window_event_thread_func);

	run_event_loop(b);

	LOG_I("Shutting down...");
	// Signal worker threads to stop (g_running already false).
	if (ws_thread.joinable()) ws_thread.join();
	// Post a dummy message to unstick the window-event thread's PeekMessage.
	if (win_evt_thread.joinable()) win_evt_thread.join();

	bridge_hud_destroy(b);
	if (b.local_space != XR_NULL_HANDLE) xrDestroySpace(b.local_space);
	if (b.session != XR_NULL_HANDLE) xrDestroySession(b.session);
	if (b.instance != XR_NULL_HANDLE) xrDestroyInstance(b.instance);

	WSACleanup();
	LOG_I("Bye.");
	return 0;
}
