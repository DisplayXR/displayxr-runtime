// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Desktop-control backend: Win32 (XR_DXR_desktop_control, ADR-056).
 *
 * Mechanism only (u_desktop.h): window list / hit test / UI text / window
 * actions / input injection, in the APP's process. No policy lives here.
 *
 * Design notes (the non-obvious parts):
 *
 * - **Coordinates.** Every geometry call runs inside a scoped per-monitor-v2
 *   thread DPI context (docs/reference/dpi-awareness.md): this code is a DLL in
 *   the host's process and must not inherit its awareness. All values are
 *   physical virtual-desktop pixels. Frames are the DWM extended frame bounds
 *   (the visible frame, without the invisible resize border / shadow).
 * - **No blocking on hung apps.** Observation reads window state without
 *   sending messages (InternalGetWindowText, no GetWindowText); window actions
 *   use ShowWindowAsync / SWP_ASYNCWINDOWPOS and verify by polling with a bound,
 *   so a hung target produces TIMEOUT instead of wedging the worker.
 * - **UI Automation runs on its own MTA thread.** UIA must not be driven from a
 *   UI thread (Unity's main thread calls text_at): a provider living on the
 *   calling thread would deadlock. A private worker owns COM (its own MTA init,
 *   so the caller's apartment is never touched) and the caller waits with a
 *   bound. Points over the calling process's own windows are answered with
 *   InternalGetWindowText instead of UIA, for the same reason.
 * - **UIPI.** SendInput / PostMessage into a higher-integrity (elevated) process
 *   is blocked by Windows, often silently. Input actions compare the target's
 *   integrity level with ours first and return REFUSED rather than pretend.
 * - **Delivery guarantee (contract v2).** type_text / key_chord re-check the
 *   guard and that the foreground root is guard->target_id (or its same-process
 *   dialog) before every batch, every Enter / Tab and every key-down;
 *   pointer_button / scroll re-check the guard, the cursor (2 px) and the hit
 *   window at the verified point before every press / burst, and act AT it.
 * - **Consent prompt lifetime.** The MessageBox thread is joinable; destroy()
 *   closes the dialog (answer UNKNOWN), joins, and done() has run before it
 *   returns. The dialog defaults to "No" and ignores input for 500 ms.
 * - **Chord keys follow the ACTIVE layout** (u_desktop.h): a character key is
 *   mapped with VkKeyScanExW on the foreground thread's HKL ("ctrl+z" = the key
 *   labelled Z, on AZERTY too); AltGr-only characters are UNSUPPORTED. Named
 *   keys (enter, f5, arrows, ...) are fixed virtual keys.
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <ole2.h>
#include <oleauto.h>
#include <dwmapi.h>
#include <wtsapi32.h>
#include <uiautomation.h>

#include "u_desktop.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <string>
#include <utility>
#include <vector>

/*
 *
 * Small helpers.
 *
 */

#define WIN32_UIA_WAIT_MS 200        // caller-side bound on one text_at
#define WIN32_UIA_PROVIDER_MS 1000   // UIA's own connection / transaction timeouts
#define WIN32_UIA_MAX_CHARS 16384    // a document's ValuePattern can be megabytes
#define WIN32_ACTIVATE_MS 500        // foreground verification budget
#define WIN32_WINDOW_OP_MS 1000      // show / move verification budget
#define WIN32_TEXT_BATCH_CHARS 32    // characters per SendInput batch
#define WIN32_SCROLL_MAX_NOTCHES 100 // per call, per axis
#define WIN32_ALT_MASK_VK 0xE8
#define WIN32_PROMPT_ARM_MS 500   // the consent dialog ignores input this long
#define WIN32_PROMPT_JOIN_MS 3000 // destroy(): bound on closing the dialog
#define WIN32_PROMPT_REASON_MAX                                                                                        \
	200 // code points of the app's reason shown       // unassigned VK: masks the Alt tap so no menu bar activates

static void
set_msg(char *msg, const char *fmt, ...)
{
	if (msg == NULL) {
		return;
	}
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, U_DESKTOP_MSG_SIZE, fmt, ap);
	va_end(ap);
}

static inline HWND
to_hwnd(uint64_t id)
{
	return (HWND)(uintptr_t)id;
}

static inline uint64_t
to_id(HWND h)
{
	return (uint64_t)(uintptr_t)h;
}

static inline HWND
root_of(HWND h)
{
	return h != NULL ? GetAncestor(h, GA_ROOT) : NULL;
}

//! UTF-16 -> UTF-8 into a fixed buffer, truncated on a code-point boundary.
static void
utf16_to_utf8_bounded(const wchar_t *w, int wlen, char *out, size_t cap)
{
	if (out == NULL || cap == 0) {
		return;
	}
	out[0] = '\0';
	if (w == NULL || wlen <= 0) {
		return;
	}
	int need = WideCharToMultiByte(CP_UTF8, 0, w, wlen, NULL, 0, NULL, NULL);
	if (need <= 0) {
		return;
	}
	if ((size_t)need < cap) {
		WideCharToMultiByte(CP_UTF8, 0, w, wlen, out, need, NULL, NULL);
		out[need] = '\0';
		return;
	}
	std::string tmp((size_t)need, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, wlen, &tmp[0], need, NULL, NULL);
	size_t n = cap - 1;
	// tmp[n] is the first byte left out: if it continues a code point, drop that
	// whole code point.
	while (n > 0 && ((unsigned char)tmp[n] & 0xC0) == 0x80) {
		n--;
	}
	memcpy(out, tmp.data(), n);
	out[n] = '\0';
}

static std::wstring
utf8_to_utf16(const char *s)
{
	std::wstring out;
	if (s == NULL || s[0] == '\0') {
		return out;
	}
	int need = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	if (need <= 1) {
		return out;
	}
	out.resize((size_t)need);
	MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], need);
	out.resize((size_t)need - 1); // drop the terminator
	return out;
}

/*
 *
 * Per-monitor-v2 DPI scope (resolved dynamically: Windows 10 1607+).
 *
 */

typedef DPI_AWARENESS_CONTEXT(WINAPI *pfn_set_thread_dpi_ctx)(DPI_AWARENESS_CONTEXT);

static pfn_set_thread_dpi_ctx
get_set_thread_dpi_ctx(void)
{
	static pfn_set_thread_dpi_ctx fn = []() -> pfn_set_thread_dpi_ctx {
		HMODULE user32 = GetModuleHandleW(L"user32.dll");
		if (user32 == NULL) {
			return NULL;
		}
		return (pfn_set_thread_dpi_ctx)(void *)GetProcAddress(user32, "SetThreadDpiAwarenessContext");
	}();
	return fn;
}

class DpiScope
{
public:
	DpiScope()
	{
		pfn_set_thread_dpi_ctx fn = get_set_thread_dpi_ctx();
		if (fn != NULL) {
			m_prev = fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
		}
	}
	~DpiScope()
	{
		pfn_set_thread_dpi_ctx fn = get_set_thread_dpi_ctx();
		if (fn != NULL && m_prev != NULL) {
			fn(m_prev);
		}
	}
	DpiScope(const DpiScope &) = delete;
	DpiScope &
	operator=(const DpiScope &) = delete;

private:
	DPI_AWARENESS_CONTEXT m_prev = NULL;
};

/*
 *
 * Process identity: exe base name, integrity level.
 *
 */

static void
exe_base_name_for_pid(DWORD pid, char *out, size_t cap)
{
	out[0] = '\0';
	HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (p == NULL) {
		return;
	}
	wchar_t path[1024];
	DWORD n = (DWORD)(sizeof(path) / sizeof(path[0]));
	if (QueryFullProcessImageNameW(p, 0, path, &n) && n > 0) {
		const wchar_t *base = path;
		for (DWORD i = 0; i < n; i++) {
			if (path[i] == L'\\' || path[i] == L'/') {
				base = &path[i + 1];
			}
		}
		utf16_to_utf8_bounded(base, (int)(n - (DWORD)(base - path)), out, cap);
	}
	CloseHandle(p);
}

//! Mandatory integrity RID of a process (SECURITY_MANDATORY_*_RID). False = unknown.
static bool
process_integrity(DWORD pid, DWORD *out_rid)
{
	HANDLE p = pid == GetCurrentProcessId() ? GetCurrentProcess()
	                                        : OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (p == NULL) {
		return false;
	}
	bool ok = false;
	HANDLE tok = NULL;
	if (OpenProcessToken(p, TOKEN_QUERY, &tok)) {
		BYTE buf[256];
		DWORD len = 0;
		if (GetTokenInformation(tok, TokenIntegrityLevel, buf, sizeof(buf), &len)) {
			TOKEN_MANDATORY_LABEL *tml = (TOKEN_MANDATORY_LABEL *)buf;
			PSID sid = tml->Label.Sid;
			UCHAR count = *GetSidSubAuthorityCount(sid);
			if (count > 0) {
				*out_rid = *GetSidSubAuthority(sid, (DWORD)(count - 1));
				ok = true;
			}
		}
		CloseHandle(tok);
	}
	if (p != GetCurrentProcess()) {
		CloseHandle(p);
	}
	return ok;
}

static DWORD
self_integrity(void)
{
	static DWORD rid = []() -> DWORD {
		DWORD r = SECURITY_MANDATORY_MEDIUM_RID;
		process_integrity(GetCurrentProcessId(), &r);
		return r;
	}();
	return rid;
}

/*!
 * True when @p h belongs to a process at a higher integrity level than ours:
 * UIPI will drop our input / messages to it. Unknown -> false (the SendInput
 * return value is still checked).
 */
static bool
window_above_us(HWND h)
{
	if (h == NULL) {
		return false;
	}
	DWORD pid = 0;
	GetWindowThreadProcessId(h, &pid);
	if (pid == 0 || pid == GetCurrentProcessId()) {
		return false;
	}
	DWORD rid = 0;
	if (!process_integrity(pid, &rid)) {
		return false;
	}
	return rid > self_integrity();
}

static const char *const k_msg_elevated = "target runs as administrator: Windows blocks input to it (UIPI)";

/*
 *
 * Window classification + entries.
 *
 */

static void
class_of(HWND h, wchar_t *cls, int cap)
{
	cls[0] = L'\0';
	if (GetClassNameW(h, cls, cap) <= 0) {
		cls[0] = L'\0';
	}
}

static bool
class_is_desktop(const wchar_t *cls)
{
	return wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0;
}

static bool
class_is_shell(const wchar_t *cls)
{
	static const wchar_t *const k_shell[] = {
	    L"Shell_TrayWnd",
	    L"Shell_SecondaryTrayWnd",
	    L"NotifyIconOverflowWindow",
	    L"TopLevelWindowForOverflowXamlIsland", // Windows 11 tray overflow
	    L"Windows.UI.Core.CoreWindow",          // top-level only: Start, search, action center
	    L"XamlExplorerHostIslandWindow",        // Windows 11 task view / snap layouts
	};
	for (const wchar_t *s : k_shell) {
		if (wcscmp(cls, s) == 0) {
			return true;
		}
	}
	return false;
}

static bool
is_cloaked(HWND h)
{
	DWORD cloaked = 0;
	return SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
}

//! Visible frame in physical px. Caller holds a DpiScope.
static bool
window_frame(HWND h, RECT *out)
{
	if (IsIconic(h)) {
		// The iconic rect is (-32000, -32000): report where the window will
		// come back instead (rcNormalPosition is workspace-relative).
		WINDOWPLACEMENT wp;
		memset(&wp, 0, sizeof(wp));
		wp.length = sizeof(wp);
		if (!GetWindowPlacement(h, &wp)) {
			return false;
		}
		RECT r = wp.rcNormalPosition;
		if ((GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) == 0) {
			HMONITOR mon = MonitorFromRect(&r, MONITOR_DEFAULTTOPRIMARY);
			MONITORINFO mi;
			memset(&mi, 0, sizeof(mi));
			mi.cbSize = sizeof(mi);
			if (mon != NULL && GetMonitorInfoW(mon, &mi)) {
				OffsetRect(&r, mi.rcWork.left - mi.rcMonitor.left, mi.rcWork.top - mi.rcMonitor.top);
			}
		}
		*out = r;
		return true;
	}
	if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, out, sizeof(*out)))) {
		return true;
	}
	return GetWindowRect(h, out) != FALSE;
}

//! Caller holds a DpiScope.
static bool
window_is_fullscreen(HWND h, const RECT *frame)
{
	if (IsZoomed(h) || IsIconic(h)) {
		return false;
	}
	HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONULL);
	if (mon == NULL) {
		return false;
	}
	MONITORINFO mi;
	memset(&mi, 0, sizeof(mi));
	mi.cbSize = sizeof(mi);
	if (!GetMonitorInfoW(mon, &mi)) {
		return false;
	}
	return frame->left <= mi.rcMonitor.left && frame->top <= mi.rcMonitor.top &&
	       frame->right >= mi.rcMonitor.right && frame->bottom >= mi.rcMonitor.bottom;
}

//! A tiny per-call pid -> exe name cache (list_windows sees many windows per process).
struct AppIdCache
{
	std::vector<std::pair<DWORD, std::string>> entries;

	void
	lookup(DWORD pid, char *out, size_t cap)
	{
		for (const auto &e : entries) {
			if (e.first == pid) {
				snprintf(out, cap, "%s", e.second.c_str());
				return;
			}
		}
		exe_base_name_for_pid(pid, out, cap);
		entries.emplace_back(pid, std::string(out));
	}
};

//! Fill one entry. Caller holds a DpiScope.
static void
fill_window(HWND h, HWND fg_root, AppIdCache *cache, struct u_desktop_window *w)
{
	memset(w, 0, sizeof(*w));
	w->id = to_id(h);
	w->parent_id = to_id(GetWindow(h, GW_OWNER));

	DWORD pid = 0;
	GetWindowThreadProcessId(h, &pid);
	w->pid = pid;
	if (cache != NULL) {
		cache->lookup(pid, w->app_id, sizeof(w->app_id));
	} else {
		exe_base_name_for_pid(pid, w->app_id, sizeof(w->app_id));
	}

	// InternalGetWindowText never sends WM_GETTEXT: a hung window cannot block us.
	wchar_t title[512];
	int tlen = InternalGetWindowText(h, title, (int)(sizeof(title) / sizeof(title[0])));
	utf16_to_utf8_bounded(title, tlen, w->title, sizeof(w->title));

	RECT r = {0, 0, 0, 0};
	window_frame(h, &r);
	w->frame.x = r.left;
	w->frame.y = r.top;
	w->frame.w = r.right - r.left;
	w->frame.h = r.bottom - r.top;

	uint32_t st = 0;
	if (fg_root != NULL && fg_root == h) {
		st |= U_DESKTOP_WINDOW_FOCUSED;
	}
	if (IsIconic(h)) {
		st |= U_DESKTOP_WINDOW_MINIMIZED;
	}
	if (IsZoomed(h)) {
		st |= U_DESKTOP_WINDOW_MAXIMIZED;
	}
	if (window_is_fullscreen(h, &r)) {
		st |= U_DESKTOP_WINDOW_FULLSCREEN;
	}
	if (pid == GetCurrentProcessId()) {
		st |= U_DESKTOP_WINDOW_OWN;
	}
	w->state = st;
}

//! The list_windows filter. Caller holds a DpiScope.
static bool
window_is_listable(HWND h)
{
	if (!IsWindowVisible(h) || is_cloaked(h)) {
		return false;
	}
	LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
	if ((ex & WS_EX_TOOLWINDOW) != 0 && (ex & WS_EX_APPWINDOW) == 0) {
		return false;
	}
	wchar_t cls[128];
	class_of(h, cls, 128);
	if (class_is_desktop(cls) || wcscmp(cls, L"Shell_TrayWnd") == 0 ||
	    wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0) {
		return false;
	}
	wchar_t title[2];
	bool titled = InternalGetWindowText(h, title, 2) > 0;
	if (!titled && GetWindow(h, GW_OWNER) == NULL) {
		return false;
	}
	RECT r;
	if (!window_frame(h, &r) || r.right <= r.left || r.bottom <= r.top) {
		return false;
	}
	return true;
}

static BOOL CALLBACK
enum_collect(HWND h, LPARAM lp)
{
	((std::vector<HWND> *)lp)->push_back(h);
	return TRUE;
}

/*
 *
 * UI Automation worker (text_at).
 *
 */

struct uia_worker
{
	volatile LONG refs; // backend + thread
	SRWLOCK lock;
	CONDITION_VARIABLE cv;
	HANDLE thread;

	// Guarded by lock.
	bool quit;
	bool claimed;      // a caller owns the request slot
	bool busy;         // the thread is inside UIA
	uint64_t req_seq;  // last request posted
	uint64_t done_seq; // last request answered
	POINT pt;
	std::wstring result;
	HRESULT hr;
	bool is_password;
};

static void
uia_worker_unref(uia_worker *w)
{
	if (InterlockedDecrement(&w->refs) == 0) {
		delete w;
	}
}

static HRESULT
uia_text_at(IUIAutomation *uia, POINT pt, std::wstring &out, bool *is_password)
{
	IUIAutomationElement *el = NULL;
	HRESULT hr = uia->ElementFromPoint(pt, &el);
	if (FAILED(hr) || el == NULL) {
		return FAILED(hr) ? hr : S_OK;
	}

	// Never read a password field's value (or its name, which some
	// frameworks fill with the value).
	BOOL pw = FALSE;
	if (SUCCEEDED(el->get_CurrentIsPassword(&pw)) && pw) {
		*is_password = true;
		el->Release();
		return S_OK;
	}

	IUIAutomationValuePattern *vp = NULL;
	if (SUCCEEDED(el->GetCurrentPatternAs(UIA_ValuePatternId, __uuidof(IUIAutomationValuePattern), (void **)&vp)) &&
	    vp != NULL) {
		BSTR b = NULL;
		if (SUCCEEDED(vp->get_CurrentValue(&b)) && b != NULL) {
			UINT n = SysStringLen(b);
			if (n > WIN32_UIA_MAX_CHARS) {
				n = WIN32_UIA_MAX_CHARS;
			}
			out.assign(b, n);
		}
		SysFreeString(b);
		vp->Release();
	}
	if (out.empty()) {
		BSTR b = NULL;
		if (SUCCEEDED(el->get_CurrentName(&b)) && b != NULL) {
			UINT n = SysStringLen(b);
			if (n > WIN32_UIA_MAX_CHARS) {
				n = WIN32_UIA_MAX_CHARS;
			}
			out.assign(b, n);
		}
		SysFreeString(b);
	}
	el->Release();
	return S_OK;
}

static DWORD WINAPI
uia_thread_main(LPVOID param)
{
	uia_worker *w = (uia_worker *)param;

	// This thread is ours: pin it per-monitor-v2 for good (ElementFromPoint
	// takes physical px), and own its COM apartment.
	pfn_set_thread_dpi_ctx set_dpi = get_set_thread_dpi_ctx();
	if (set_dpi != NULL) {
		set_dpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	}
	HRESULT ci = CoInitializeEx(NULL, COINIT_MULTITHREADED);

	IUIAutomation *uia = NULL;
	HRESULT create_hr = CoCreateInstance(__uuidof(CUIAutomation), NULL, CLSCTX_INPROC_SERVER,
	                                     __uuidof(IUIAutomation), (void **)&uia);
	if (SUCCEEDED(create_hr) && uia != NULL) {
		IUIAutomation2 *uia2 = NULL;
		if (SUCCEEDED(uia->QueryInterface(__uuidof(IUIAutomation2), (void **)&uia2)) && uia2 != NULL) {
			uia2->put_ConnectionTimeout(WIN32_UIA_PROVIDER_MS);
			uia2->put_TransactionTimeout(WIN32_UIA_PROVIDER_MS);
			uia2->Release();
		}
	}

	AcquireSRWLockExclusive(&w->lock);
	for (;;) {
		while (!w->quit && w->req_seq == w->done_seq) {
			SleepConditionVariableSRW(&w->cv, &w->lock, INFINITE, 0);
		}
		if (w->quit) {
			break;
		}
		uint64_t seq = w->req_seq;
		POINT pt = w->pt;
		w->busy = true;
		ReleaseSRWLockExclusive(&w->lock);

		std::wstring text;
		bool pw = false;
		HRESULT hr = uia != NULL ? uia_text_at(uia, pt, text, &pw) : create_hr;

		AcquireSRWLockExclusive(&w->lock);
		w->result.swap(text);
		w->hr = hr;
		w->is_password = pw;
		w->done_seq = seq;
		w->busy = false;
		WakeAllConditionVariable(&w->cv);
	}
	ReleaseSRWLockExclusive(&w->lock);

	if (uia != NULL) {
		uia->Release();
	}
	if (SUCCEEDED(ci)) {
		CoUninitialize();
	}
	uia_worker_unref(w);
	return 0;
}

//! Wait on the worker's condvar until @p deadline (GetTickCount64). False on timeout.
static bool
uia_wait(uia_worker *w, ULONGLONG deadline)
{
	ULONGLONG now = GetTickCount64();
	if (now >= deadline) {
		return false;
	}
	SleepConditionVariableSRW(&w->cv, &w->lock, (DWORD)(deadline - now), 0);
	return true;
}

/*
 *
 * The backend.
 *
 */

struct win32_backend
{
	struct u_desktop_backend base;

	SRWLOCK uia_init_lock;
	uia_worker *uia; // lazily started on the first text_at
	bool uia_failed;

	SRWLOCK prompt_lock;
	struct prompt_state *prompt; // the consent prompt (joinable), or NULL
};

static inline win32_backend *
win32(struct u_desktop_backend *b)
{
	return (win32_backend *)b;
}

static uia_worker *
uia_get(win32_backend *wb)
{
	AcquireSRWLockExclusive(&wb->uia_init_lock);
	if (wb->uia == NULL && !wb->uia_failed) {
		uia_worker *w = new uia_worker();
		w->refs = 2;
		InitializeSRWLock(&w->lock);
		InitializeConditionVariable(&w->cv);
		w->quit = false;
		w->claimed = false;
		w->busy = false;
		w->req_seq = 0;
		w->done_seq = 0;
		w->hr = S_OK;
		w->is_password = false;
		w->thread = CreateThread(NULL, 0, uia_thread_main, w, 0, NULL);
		if (w->thread == NULL) {
			delete w;
			wb->uia_failed = true;
		} else {
			wb->uia = w;
		}
	}
	uia_worker *w = wb->uia;
	ReleaseSRWLockExclusive(&wb->uia_init_lock);
	return w;
}

/*
 *
 * Consent.
 *
 */

static enum u_desktop_access
win32_access_status(struct u_desktop_backend *b)
{
	(void)b;
	// Windows has no OS-side grant: the common layer owns the stored answer.
	return U_DESKTOP_ACCESS_UNKNOWN;
}

/*
 * The consent prompt. A joinable thread runs a MessageBoxW whose input is
 * ignored for its first WIN32_PROMPT_ARM_MS (a CBT hook disables the dialog as
 * it activates and a timer re-enables it), so a click or Enter meant for
 * something else cannot answer it. "No" is the default button. destroy()
 * cancels it (posts IDNO to the dialog, answer reported as UNKNOWN), joins the
 * thread, and guarantees done() ran exactly once before it returns.
 */

struct prompt_state
{
	volatile LONG refs; // backend + thread
	SRWLOCK lock;
	std::wstring text;
	u_desktop_access_done_fn done;
	void *ctx;
	HANDLE thread;
	DWORD tid;

	// Guarded by lock.
	bool done_called;
	bool cancelled; // destroy() is closing it: the answer is UNKNOWN
	HWND dialog;
};

static thread_local prompt_state *t_prompt = NULL;

static void
prompt_unref(prompt_state *p)
{
	if (InterlockedDecrement(&p->refs) == 0) {
		if (p->thread != NULL) {
			CloseHandle(p->thread);
		}
		delete p;
	}
}

//! Call done() unless it was already claimed. The claim is under the lock, the
//! call is not: done() may take the common layer's lock, which its destroy()
//! caller may hold while it waits (bounded) for our join.
static void
prompt_finish(prompt_state *p, enum u_desktop_access result)
{
	AcquireSRWLockExclusive(&p->lock);
	bool call = !p->done_called;
	p->done_called = true;
	ReleaseSRWLockExclusive(&p->lock);
	if (call) {
		p->done(p->ctx, result);
	}
}

static void
prompt_post_no(HWND dialog)
{
	PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDNO, BN_CLICKED), 0);
}

static VOID CALLBACK
prompt_arm_timer(HWND h, UINT msg, UINT_PTR id, DWORD time)
{
	(void)msg;
	(void)time;
	KillTimer(h, id);
	EnableWindow(h, TRUE);
	HWND no = GetDlgItem(h, IDNO);
	if (no != NULL) {
		SetFocus(no);
	}
}

static LRESULT CALLBACK
prompt_cbt_hook(int code, WPARAM wp, LPARAM lp)
{
	prompt_state *p = t_prompt;
	if (code == HCBT_ACTIVATE && p != NULL) {
		HWND h = (HWND)wp;
		wchar_t cls[16];
		class_of(h, cls, 16);
		AcquireSRWLockExclusive(&p->lock);
		bool first = p->dialog == NULL && wcscmp(cls, L"#32770") == 0;
		if (first) {
			p->dialog = h;
		}
		bool cancel = p->cancelled;
		ReleaseSRWLockExclusive(&p->lock);
		if (first) {
			EnableWindow(h, FALSE);
			SetTimer(h, 1, WIN32_PROMPT_ARM_MS, prompt_arm_timer);
			if (cancel) {
				prompt_post_no(h);
			}
		}
	}
	return CallNextHookEx(NULL, code, wp, lp);
}

static DWORD WINAPI
prompt_thread_main(LPVOID param)
{
	prompt_state *p = (prompt_state *)param;
	t_prompt = p;

	AcquireSRWLockExclusive(&p->lock);
	bool cancel = p->cancelled;
	ReleaseSRWLockExclusive(&p->lock);

	int r = 0;
	if (!cancel) {
		HHOOK hook = SetWindowsHookExW(WH_CBT, prompt_cbt_hook, NULL, GetCurrentThreadId());
		r = MessageBoxW(NULL, p->text.c_str(), L"DisplayXR",
		                MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_TOPMOST | MB_SETFOREGROUND);
		if (hook != NULL) {
			UnhookWindowsHookEx(hook);
		}
	}

	AcquireSRWLockExclusive(&p->lock);
	cancel = p->cancelled;
	p->dialog = NULL;
	ReleaseSRWLockExclusive(&p->lock);

	// A destroy-initiated close is not the user's "no": it must not be stored.
	enum u_desktop_access result = U_DESKTOP_ACCESS_UNKNOWN;
	if (!cancel && r == IDYES) {
		result = U_DESKTOP_ACCESS_GRANTED;
	} else if (!cancel && r == IDNO) {
		result = U_DESKTOP_ACCESS_DENIED;
	}
	prompt_finish(p, result);

	t_prompt = NULL;
	prompt_unref(p);
	return 0;
}

static BOOL CALLBACK
prompt_close_enum(HWND h, LPARAM lp)
{
	(void)lp;
	wchar_t cls[16];
	class_of(h, cls, 16);
	if (wcscmp(cls, L"#32770") == 0) {
		prompt_post_no(h);
	}
	return TRUE;
}

//! Cancel, close and join the prompt; done() has run when this returns.
static void
prompt_shutdown(prompt_state *p)
{
	AcquireSRWLockExclusive(&p->lock);
	p->cancelled = true;
	HWND dialog = p->dialog;
	ReleaseSRWLockExclusive(&p->lock);
	if (dialog != NULL) {
		prompt_post_no(dialog);
	}
	ULONGLONG end = GetTickCount64() + WIN32_PROMPT_JOIN_MS;
	while (WaitForSingleObject(p->thread, 50) == WAIT_TIMEOUT && GetTickCount64() < end) {
		// Covers a dialog that appeared after the flag was read, and a
		// failed CBT hook.
		EnumThreadWindows(p->tid, prompt_close_enum, 0);
	}
	// Normally a no-op (the thread already answered UNKNOWN). If the thread is
	// somehow still alive and has not claimed the answer, this answers for it
	// and the claim stops it from ever calling done() after we return.
	prompt_finish(p, U_DESKTOP_ACCESS_UNKNOWN);
	prompt_unref(p);
}

//! App-supplied text for the prompt (reason, app name): one line, no control,
//! bidi, zero-width or format characters (no faking the prompt's own lines, no
//! hidden or reordered text), at most @p max code points.
static std::wstring
prompt_sanitize(const std::wstring &in, size_t max)
{
	std::wstring out;
	size_t cps = 0;
	for (size_t i = 0; i < in.size(); i++) {
		wchar_t c = in[i];
		// Invisible / reordering characters: bidi controls and overrides, zero-width
		// and format characters, the Arabic letter mark, the BOM. They can hide or
		// rearrange what the prompt shows the user.
		if ((c >= 0x200B && c <= 0x200F) || (c >= 0x2028 && c <= 0x202F) || (c >= 0x2060 && c <= 0x206F) ||
		    c == 0xFEFF || c == 0x061C) {
			continue;
		}
		if (cps == max) {
			out += L"\x2026";
			break;
		}
		if (c < 0x20 || c == 0x7F) {
			c = L' ';
		}
		out += c;
		if (IS_HIGH_SURROGATE(c) && i + 1 < in.size() && IS_LOW_SURROGATE(in[i + 1])) {
			out += in[++i];
		}
		cps++;
	}
	return out;
}

static void
win32_request_access(struct u_desktop_backend *b,
                     const struct u_desktop_app_info *app,
                     u_desktop_access_done_fn done,
                     void *ctx)
{
	win32_backend *wb = win32(b);

	std::wstring name;
	if (app != NULL && app->display_name != NULL && app->display_name[0] != '\0') {
		name = prompt_sanitize(utf8_to_utf16(app->display_name), 80);
	} else if (app != NULL && app->exe_path != NULL && app->exe_path[0] != '\0') {
		std::wstring path = utf8_to_utf16(app->exe_path);
		size_t slash = path.find_last_of(L"\\/");
		name = prompt_sanitize(slash == std::wstring::npos ? path : path.substr(slash + 1), 80);
	}
	if (name.empty()) {
		name = L"An app";
	}
	std::wstring exe = app != NULL && app->exe_path != NULL && app->exe_path[0] != '\0'
	                       ? prompt_sanitize(utf8_to_utf16(app->exe_path), 400)
	                       : name;

	std::wstring text = name;
	text +=
	    L" wants to control your computer.\n\n"
	    L"It will be able to see your windows, and type and click in other apps.\n\n";
	if (app != NULL && app->reason != NULL && app->reason[0] != '\0') {
		text += L"The app says: \"";
		text += prompt_sanitize(utf8_to_utf16(app->reason), WIN32_PROMPT_REASON_MAX);
		text += L"\"\n\n";
	}
	text += L"To stop it, run: displayxr-cli desktop deny \"";
	text += exe;
	text += L"\" (or 'displayxr-cli desktop stop' to pause every app)";

	AcquireSRWLockExclusive(&wb->prompt_lock);
	if (wb->prompt != NULL) {
		if (WaitForSingleObject(wb->prompt->thread, 0) != WAIT_OBJECT_0) {
			// One prompt at a time; this request is not answered by the user.
			ReleaseSRWLockExclusive(&wb->prompt_lock);
			done(ctx, U_DESKTOP_ACCESS_UNKNOWN);
			return;
		}
		prompt_unref(wb->prompt); // finished: its done() already ran
		wb->prompt = NULL;
	}

	prompt_state *p = new prompt_state();
	p->refs = 2;
	InitializeSRWLock(&p->lock);
	p->text.swap(text);
	p->done = done;
	p->ctx = ctx;
	p->done_called = false;
	p->cancelled = false;
	p->dialog = NULL;
	p->thread = CreateThread(NULL, 0, prompt_thread_main, p, CREATE_SUSPENDED, &p->tid);
	if (p->thread == NULL) {
		ReleaseSRWLockExclusive(&wb->prompt_lock);
		delete p;
		// No prompt was shown: that is not the user's "no".
		done(ctx, U_DESKTOP_ACCESS_UNKNOWN);
		return;
	}
	wb->prompt = p;
	ResumeThread(p->thread);
	ReleaseSRWLockExclusive(&wb->prompt_lock);
}

/*
 *
 * Session state.
 *
 */

typedef BOOL(WINAPI *pfn_wts_query)(HANDLE, DWORD, WTS_INFO_CLASS, LPWSTR *, DWORD *);
typedef void(WINAPI *pfn_wts_free)(PVOID);

struct wts_fns
{
	pfn_wts_query query;
	pfn_wts_free free_mem;
};

static const wts_fns &
get_wts(void)
{
	static wts_fns fns = []() -> wts_fns {
		wts_fns f = {NULL, NULL};
		HMODULE m = LoadLibraryExW(L"wtsapi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (m != NULL) {
			f.query = (pfn_wts_query)(void *)GetProcAddress(m, "WTSQuerySessionInformationW");
			f.free_mem = (pfn_wts_free)(void *)GetProcAddress(m, "WTSFreeMemory");
		}
		return f;
	}();
	return fns;
}

static bool
win32_is_locked(struct u_desktop_backend *b)
{
	(void)b;

	// 1. The input desktop: "Default" normally, "Winlogon" on the lock screen
	//    and the UAC secure desktop (where OpenInputDesktop fails outright).
	HDESK desk = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
	if (desk == NULL) {
		return true;
	}
	wchar_t name[64] = {0};
	DWORD len = 0;
	bool named = GetUserObjectInformationW(desk, UOI_NAME, name, sizeof(name) - sizeof(wchar_t), &len) != FALSE;
	CloseDesktop(desk);
	if (named && _wcsicmp(name, L"Default") != 0) {
		return true;
	}

	// 2. The session's lock flag (Windows 10+ semantics; 7/2008 R2 had it inverted).
	const wts_fns &wts = get_wts();
	if (wts.query != NULL && wts.free_mem != NULL) {
		LPWSTR buf = NULL;
		DWORD bytes = 0;
		if (wts.query(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSSessionInfoEx, &buf, &bytes) &&
		    buf != NULL) {
			bool locked = false;
			if (bytes >= sizeof(WTSINFOEXW)) {
				const WTSINFOEXW *ex = (const WTSINFOEXW *)buf;
				if (ex->Level == 1 &&
				    ex->Data.WTSInfoExLevel1.SessionFlags == (LONG)WTS_SESSIONSTATE_LOCK) {
					locked = true;
				}
			}
			wts.free_mem(buf);
			if (locked) {
				return true;
			}
		}
	}
	return false;
}

static bool
win32_user_idle_ms(struct u_desktop_backend *b, uint64_t *out_ms)
{
	(void)b;
	LASTINPUTINFO lii;
	lii.cbSize = sizeof(lii);
	if (!GetLastInputInfo(&lii)) {
		return false;
	}
	// Both sides are 32-bit tick counts: unsigned subtraction is wrap-safe
	// (valid for idle times under 49.7 days).
	DWORD now = (DWORD)GetTickCount64();
	*out_ms = (uint64_t)(DWORD)(now - lii.dwTime);
	return true;
}

/*
 *
 * Observation.
 *
 */

static enum u_desktop_result
win32_list_windows(struct u_desktop_backend *b, struct u_desktop_window *out, uint32_t cap, uint32_t *count)
{
	(void)b;
	DpiScope dpi;

	std::vector<HWND> all;
	all.reserve(512);
	EnumWindows(enum_collect, (LPARAM)&all); // top-most first

	HWND fg_root = root_of(GetForegroundWindow());
	AppIdCache cache;
	uint32_t n = 0;
	for (HWND h : all) {
		if (!window_is_listable(h)) {
			continue;
		}
		if (out != NULL && n < cap) {
			fill_window(h, fg_root, &cache, &out[n]);
			out[n].z = n;
		}
		n++;
	}
	if (count != NULL) {
		*count = n;
	}
	return U_DESKTOP_OK;
}

static enum u_desktop_result
win32_get_pointer(struct u_desktop_backend *b, int32_t *x, int32_t *y)
{
	(void)b;
	DpiScope dpi;
	POINT pt;
	if (!GetCursorPos(&pt)) {
		return U_DESKTOP_ERR_FAILED; // e.g. a secure desktop is up
	}
	*x = pt.x;
	*y = pt.y;
	return U_DESKTOP_OK;
}

static enum u_desktop_result
win32_pick(struct u_desktop_backend *b, int32_t x, int32_t y, struct u_desktop_pick *out)
{
	(void)b;
	memset(out, 0, sizeof(*out));
	out->x = x;
	out->y = y;

	DpiScope dpi;
	POINT pt = {x, y};
	if (MonitorFromPoint(pt, MONITOR_DEFAULTTONULL) == NULL) {
		out->kind = U_DESKTOP_PICK_OUTSIDE;
		return U_DESKTOP_OK;
	}
	HWND root = root_of(WindowFromPoint(pt));
	if (root == NULL) {
		out->kind = U_DESKTOP_PICK_DESKTOP;
		return U_DESKTOP_OK;
	}
	wchar_t cls[128];
	class_of(root, cls, 128);
	if (class_is_desktop(cls)) {
		out->kind = U_DESKTOP_PICK_DESKTOP;
		return U_DESKTOP_OK;
	}
	if (class_is_shell(cls)) {
		out->kind = U_DESKTOP_PICK_SHELL;
		return U_DESKTOP_OK;
	}
	out->kind = U_DESKTOP_PICK_WINDOW;
	fill_window(root, root_of(GetForegroundWindow()), NULL, &out->window);
	return U_DESKTOP_OK;
}

static enum u_desktop_result
win32_focused_window(struct u_desktop_backend *b, uint64_t *out_id)
{
	(void)b;
	*out_id = to_id(root_of(GetForegroundWindow()));
	return U_DESKTOP_OK;
}

static enum u_desktop_result
win32_text_at(struct u_desktop_backend *b, int32_t x, int32_t y, char *buf, size_t cap, char *msg)
{
	if (buf == NULL || cap == 0) {
		return U_DESKTOP_ERR_FAILED;
	}
	buf[0] = '\0';

	POINT pt = {x, y};
	HWND hit = NULL;
	{
		DpiScope dpi;
		hit = WindowFromPoint(pt);
	}
	if (hit == NULL) {
		return U_DESKTOP_OK;
	}

	// Our own process: UIA could need this very thread to answer. Read the
	// control's text directly instead (no message sent).
	DWORD pid = 0;
	GetWindowThreadProcessId(hit, &pid);
	if (pid == GetCurrentProcessId()) {
		wchar_t t[1024];
		int n = InternalGetWindowText(hit, t, (int)(sizeof(t) / sizeof(t[0])));
		utf16_to_utf8_bounded(t, n, buf, cap);
		return U_DESKTOP_OK;
	}

	uia_worker *w = uia_get(win32(b));
	if (w == NULL) {
		set_msg(msg, "UI Automation worker could not start");
		return U_DESKTOP_ERR_FAILED;
	}

	ULONGLONG deadline = GetTickCount64() + WIN32_UIA_WAIT_MS;
	enum u_desktop_result res = U_DESKTOP_OK;
	AcquireSRWLockExclusive(&w->lock);
	// Wait for the slot: no other caller, and no abandoned request still in UIA.
	while (w->claimed || w->busy || w->req_seq != w->done_seq) {
		if (!uia_wait(w, deadline)) {
			ReleaseSRWLockExclusive(&w->lock);
			set_msg(msg, "accessibility query busy (a previous one has not answered)");
			return U_DESKTOP_ERR_TIMEOUT;
		}
	}
	w->claimed = true;
	w->pt = pt;
	uint64_t seq = ++w->req_seq;
	WakeAllConditionVariable(&w->cv);
	while (w->done_seq != seq) {
		if (!uia_wait(w, deadline)) {
			break;
		}
	}
	if (w->done_seq != seq) {
		set_msg(msg, "accessibility query did not answer within %d ms", WIN32_UIA_WAIT_MS);
		res = U_DESKTOP_ERR_TIMEOUT;
	} else if (FAILED(w->hr)) {
		set_msg(msg, "UI Automation failed (hr 0x%08lx)", (unsigned long)w->hr);
		res = U_DESKTOP_ERR_FAILED;
	} else if (!w->is_password) {
		utf16_to_utf8_bounded(w->result.c_str(), (int)w->result.size(), buf, cap);
	}
	w->claimed = false;
	WakeAllConditionVariable(&w->cv);
	ReleaseSRWLockExclusive(&w->lock);
	return res;
}

/*
 *
 * Window actions (worker thread).
 *
 */

static bool
guard_allows(const struct u_desktop_input_guard *g)
{
	return g == NULL || g->still_allowed == NULL || g->still_allowed(g->ctx);
}

static const char *const k_msg_stopped = "stopped: desktop control was paused, denied or the screen locked";

/*!
 * @p root is the target, or a dialog of it: owned by the target AND in the same
 * process (a cross-process popup owned by the target is not its dialog).
 */
static bool
is_target_or_dialog(HWND root, HWND target)
{
	if (root == NULL || target == NULL) {
		return false;
	}
	if (root == target) {
		return true;
	}
	if (GetWindow(root, GW_OWNER) != target) {
		return false;
	}
	DWORD p_root = 0;
	DWORD p_target = 0;
	GetWindowThreadProcessId(root, &p_root);
	GetWindowThreadProcessId(target, &p_target);
	return p_root != 0 && p_root == p_target;
}

static bool
fg_matches(HWND target, HWND activated)
{
	HWND fg = root_of(GetForegroundWindow());
	if (fg == NULL) {
		return false;
	}
	return fg == activated || is_target_or_dialog(fg, target);
}

static bool
poll_fg(HWND target, HWND activated, DWORD ms)
{
	ULONGLONG end = GetTickCount64() + ms;
	for (;;) {
		if (fg_matches(target, activated)) {
			return true;
		}
		if (GetTickCount64() >= end) {
			return false;
		}
		Sleep(10);
	}
}

static enum u_desktop_result
op_activate(HWND h, const struct u_desktop_input_guard *guard, char *msg)
{
	ULONGLONG end = GetTickCount64() + WIN32_ACTIVATE_MS;
	if (IsIconic(h)) {
		ShowWindowAsync(h, SW_RESTORE);
	}
	// Activate what Alt+Tab would: the window's enabled last active popup
	// (its modal dialog), else the window itself.
	HWND act = GetLastActivePopup(h);
	if (act == NULL || !IsWindowVisible(act) || !IsWindowEnabled(act) || !is_target_or_dialog(act, h)) {
		act = h;
	}
	if (fg_matches(h, act) && !IsIconic(h)) {
		return U_DESKTOP_OK;
	}

	// 1. Plain request (works when we own the foreground or the lock is off).
	SetForegroundWindow(act);
	if (poll_fg(h, act, 60)) {
		return U_DESKTOP_OK;
	}

	// 2. Share the foreground thread's input state, then ask again.
	HWND fg = GetForegroundWindow();
	DWORD fg_tid = fg != NULL ? GetWindowThreadProcessId(fg, NULL) : 0;
	DWORD my_tid = GetCurrentThreadId();
	MSG m;
	PeekMessageW(&m, NULL, 0, 0, PM_NOREMOVE); // make sure this thread has a queue
	bool attached = fg_tid != 0 && fg_tid != my_tid && AttachThreadInput(my_tid, fg_tid, TRUE);
	BringWindowToTop(act);
	SetForegroundWindow(act);
	if (attached) {
		AttachThreadInput(my_tid, fg_tid, FALSE);
	}
	if (poll_fg(h, act, 100)) {
		return U_DESKTOP_OK;
	}

	// 3. The Alt tap: an injected keystroke makes us the last-input process,
	//    which lifts the foreground lock. The masking key keeps the Alt
	//    release from opening the current window's menu bar. It is an
	//    injection, so the guard is consulted first.
	if (!guard_allows(guard)) {
		set_msg(msg, "%s", k_msg_stopped);
		return U_DESKTOP_ERR_REFUSED;
	}
	INPUT in[4];
	memset(in, 0, sizeof(in));
	for (int i = 0; i < 4; i++) {
		in[i].type = INPUT_KEYBOARD;
	}
	in[0].ki.wVk = VK_MENU;
	in[1].ki.wVk = WIN32_ALT_MASK_VK;
	in[2].ki.wVk = WIN32_ALT_MASK_VK;
	in[2].ki.dwFlags = KEYEVENTF_KEYUP;
	in[3].ki.wVk = VK_MENU;
	in[3].ki.dwFlags = KEYEVENTF_KEYUP;
	SendInput(4, in, sizeof(INPUT));
	SetForegroundWindow(act);

	ULONGLONG now = GetTickCount64();
	if (poll_fg(h, act, now < end ? (DWORD)(end - now) : 50)) {
		return U_DESKTOP_OK;
	}
	set_msg(msg, "Windows refused to bring the window to the foreground (foreground lock)");
	return U_DESKTOP_ERR_REFUSED;
}

static enum u_desktop_result
show_and_verify(HWND h, int cmd, enum u_desktop_window_op op, char *msg)
{
	auto reached = [&]() -> bool {
		switch (op) {
		case U_DESKTOP_OP_MINIMIZE: return IsIconic(h) != FALSE;
		case U_DESKTOP_OP_MAXIMIZE: return IsZoomed(h) != FALSE;
		default: return !IsIconic(h) && !IsZoomed(h);
		}
	};
	if (reached()) {
		return U_DESKTOP_OK;
	}
	ShowWindowAsync(h, cmd);
	ULONGLONG end = GetTickCount64() + WIN32_WINDOW_OP_MS;
	while (GetTickCount64() < end) {
		if (!IsWindow(h)) {
			return U_DESKTOP_ERR_WINDOW_LOST;
		}
		if (reached()) {
			return U_DESKTOP_OK;
		}
		Sleep(10);
	}
	if (window_above_us(h)) {
		set_msg(msg, "%s", k_msg_elevated);
		return U_DESKTOP_ERR_REFUSED;
	}
	set_msg(msg, "the window did not change state within %d ms", WIN32_WINDOW_OP_MS);
	return U_DESKTOP_ERR_TIMEOUT;
}

static enum u_desktop_result
op_move_resize(HWND h, const struct u_desktop_rect *rect, char *msg)
{
	if (rect == NULL) {
		set_msg(msg, "no rectangle");
		return U_DESKTOP_ERR_FAILED;
	}
	// A maximized / minimized window must be restored first, or the move is
	// applied to its restore position only.
	if (IsIconic(h) || IsZoomed(h)) {
		enum u_desktop_result r = show_and_verify(h, SW_RESTORE, U_DESKTOP_OP_RESTORE, msg);
		if (r != U_DESKTOP_OK) {
			return r;
		}
	}
	bool keep_size = rect->w <= 0 || rect->h <= 0;

	DpiScope dpi;
	RECT fb = {0, 0, 0, 0};
	// Two passes: moving across monitors of different DPI rescales the
	// invisible borders, so the second pass corrects with the new ones.
	for (int pass = 0; pass < 2; pass++) {
		RECT wr;
		if (!GetWindowRect(h, &wr)) {
			return IsWindow(h) ? U_DESKTOP_ERR_FAILED : U_DESKTOP_ERR_WINDOW_LOST;
		}
		RECT vis = wr;
		if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &vis, sizeof(vis)))) {
			vis = wr;
		}
		// The requested rect is the VISIBLE frame: add the invisible border back.
		int bl = vis.left - wr.left;
		int bt = vis.top - wr.top;
		int br = wr.right - vis.right;
		int bb = wr.bottom - vis.bottom;

		UINT flags = SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS;
		if (keep_size) {
			flags |= SWP_NOSIZE;
		}
		// 64-bit arithmetic, clamped: rect values may be anywhere in int32.
		auto clamp32 = [](int64_t v) -> int {
			return v > INT32_MAX ? INT32_MAX : (v < INT32_MIN ? INT32_MIN : (int)v);
		};
		int nx = clamp32((int64_t)rect->x - bl);
		int ny = clamp32((int64_t)rect->y - bt);
		int nw = clamp32((int64_t)rect->w + bl + br);
		int nh = clamp32((int64_t)rect->h + bt + bb);
		if (!SetWindowPos(h, NULL, nx, ny, nw, nh, flags)) {
			DWORD err = GetLastError();
			if (err == ERROR_ACCESS_DENIED || window_above_us(h)) {
				set_msg(msg, "%s", k_msg_elevated);
				return U_DESKTOP_ERR_REFUSED;
			}
			set_msg(msg, "SetWindowPos failed (error %lu)", (unsigned long)err);
			return U_DESKTOP_ERR_FAILED;
		}

		ULONGLONG end = GetTickCount64() + WIN32_WINDOW_OP_MS / 2;
		for (;;) {
			if (!IsWindow(h)) {
				return U_DESKTOP_ERR_WINDOW_LOST;
			}
			if (window_frame(h, &fb) && llabs((long long)fb.left - rect->x) <= 2 &&
			    llabs((long long)fb.top - rect->y) <= 2) {
				// Position is ours to set; a size the window refuses (minimum
				// size, a DPI-change rescale) is the window's right.
				return U_DESKTOP_OK;
			}
			if (GetTickCount64() >= end) {
				break;
			}
			Sleep(10);
		}
	}
	if (window_above_us(h)) {
		set_msg(msg, "%s", k_msg_elevated);
		return U_DESKTOP_ERR_REFUSED;
	}
	set_msg(msg, "window settled at (%ld,%ld %ldx%ld)", (long)fb.left, (long)fb.top, (long)(fb.right - fb.left),
	        (long)(fb.bottom - fb.top));
	return U_DESKTOP_ERR_FAILED;
}

static enum u_desktop_result
win32_window_op(struct u_desktop_backend *b,
                const struct u_desktop_input_guard *guard,
                enum u_desktop_window_op op,
                uint64_t window_id,
                const struct u_desktop_rect *rect,
                char *msg)
{
	(void)b;
	HWND h = to_hwnd(window_id);
	if (window_id == 0 || !IsWindow(h)) {
		return U_DESKTOP_ERR_WINDOW_LOST;
	}
	if (op != U_DESKTOP_OP_CLOSE && IsHungAppWindow(h)) {
		set_msg(msg, "the window is not responding");
		return U_DESKTOP_ERR_REFUSED;
	}

	switch (op) {
	case U_DESKTOP_OP_ACTIVATE: return op_activate(h, guard, msg);
	case U_DESKTOP_OP_MOVE_RESIZE: return op_move_resize(h, rect, msg);
	case U_DESKTOP_OP_MINIMIZE: return show_and_verify(h, SW_MINIMIZE, op, msg);
	case U_DESKTOP_OP_RESTORE: return show_and_verify(h, SW_RESTORE, op, msg);
	case U_DESKTOP_OP_MAXIMIZE: return show_and_verify(h, SW_MAXIMIZE, op, msg);
	case U_DESKTOP_OP_CLOSE:
		if (!PostMessageW(h, WM_CLOSE, 0, 0)) {
			DWORD err = GetLastError();
			if (err == ERROR_ACCESS_DENIED || window_above_us(h)) {
				set_msg(msg, "%s", k_msg_elevated);
				return U_DESKTOP_ERR_REFUSED;
			}
			if (!IsWindow(h)) {
				return U_DESKTOP_ERR_WINDOW_LOST;
			}
			set_msg(msg, "PostMessage(WM_CLOSE) failed (error %lu)", (unsigned long)err);
			return U_DESKTOP_ERR_FAILED;
		}
		// The app decides (it may ask to save): no verification.
		return U_DESKTOP_OK;
	default: set_msg(msg, "unknown window op %d", (int)op); return U_DESKTOP_ERR_UNSUPPORTED;
	}
}

/*
 *
 * Input (worker thread).
 *
 */

//! Inject @p n events; map a short count to the honest result.
static enum u_desktop_result
send_inputs(INPUT *in, UINT n, char *msg)
{
	if (n == 0) {
		return U_DESKTOP_OK;
	}
	SetLastError(0);
	UINT sent = SendInput(n, in, sizeof(INPUT));
	if (sent == n) {
		return U_DESKTOP_OK;
	}
	DWORD err = GetLastError();
	// SendInput delivers a prefix. A key whose down went out but whose up did not
	// (e.g. only the down of an Enter / Tab pair) would stay pressed: send each
	// missing up now, ungated — releasing what we pressed is always allowed.
	for (UINT i = 0; i < sent && i < n; i++) {
		if (in[i].type != INPUT_KEYBOARD || (in[i].ki.dwFlags & KEYEVENTF_KEYUP) != 0) {
			continue;
		}
		bool released = false;
		for (UINT k = i + 1; k < sent && !released; k++) {
			released = in[k].type == INPUT_KEYBOARD && (in[k].ki.dwFlags & KEYEVENTF_KEYUP) != 0 &&
			           in[k].ki.wVk == in[i].ki.wVk && in[k].ki.wScan == in[i].ki.wScan &&
			           (in[k].ki.dwFlags & KEYEVENTF_UNICODE) == (in[i].ki.dwFlags & KEYEVENTF_UNICODE);
		}
		if (!released) {
			INPUT up = in[i];
			up.ki.dwFlags |= KEYEVENTF_KEYUP;
			SendInput(1, &up, sizeof(INPUT));
		}
	}
	if (sent == 0 && err == ERROR_ACCESS_DENIED) {
		set_msg(msg, "input blocked by Windows (UIPI or secure desktop)");
		return U_DESKTOP_ERR_REFUSED;
	}
	set_msg(msg, "injected %u of %u input events (error %lu)", sent, n, (unsigned long)err);
	return U_DESKTOP_ERR_FAILED;
}

static void
key_vk(INPUT *in, WORD vk, bool up)
{
	memset(in, 0, sizeof(*in));
	in->type = INPUT_KEYBOARD;
	in->ki.wVk = vk;
	in->ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
	in->ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
}

static void
key_unicode(INPUT *in, wchar_t unit, bool up)
{
	memset(in, 0, sizeof(*in));
	in->type = INPUT_KEYBOARD;
	in->ki.wScan = (WORD)unit;
	in->ki.dwFlags = KEYEVENTF_UNICODE | (up ? KEYEVENTF_KEYUP : 0);
}

/*!
 * The keyboard half of the delivery guarantee: immediately before a batch, the
 * guard still allows and the foreground root is the target or its dialog.
 */
static enum u_desktop_result
check_focus(const struct u_desktop_input_guard *guard, char *msg)
{
	if (!guard_allows(guard)) {
		set_msg(msg, "%s", k_msg_stopped);
		return U_DESKTOP_ERR_REFUSED;
	}
	if (guard == NULL || guard->target_id == 0) {
		set_msg(msg, "no verified target");
		return U_DESKTOP_ERR_REFUSED;
	}
	if (!is_target_or_dialog(root_of(GetForegroundWindow()), to_hwnd(guard->target_id))) {
		set_msg(msg, "focus moved away from the target window");
		return U_DESKTOP_ERR_REFUSED;
	}
	return U_DESKTOP_OK;
}

/*!
 * The pointer half: the guard still allows, the cursor is still at the verified
 * point (2 px), and the window there is still the target or its dialog.
 */
static enum u_desktop_result
check_point(const struct u_desktop_input_guard *guard, int32_t x, int32_t y, char *msg)
{
	if (!guard_allows(guard)) {
		set_msg(msg, "%s", k_msg_stopped);
		return U_DESKTOP_ERR_REFUSED;
	}
	if (guard == NULL || guard->target_id == 0) {
		set_msg(msg, "no verified target");
		return U_DESKTOP_ERR_REFUSED;
	}
	DpiScope dpi;
	POINT cur;
	if (!GetCursorPos(&cur) || llabs((long long)cur.x - x) > 2 || llabs((long long)cur.y - y) > 2) {
		set_msg(msg, "the pointer moved away from the verified point");
		return U_DESKTOP_ERR_REFUSED;
	}
	POINT pt = {x, y};
	HWND root = root_of(WindowFromPoint(pt));
	if (!is_target_or_dialog(root, to_hwnd(guard->target_id))) {
		set_msg(msg, "target no longer under the point");
		return U_DESKTOP_ERR_REFUSED;
	}
	if (window_above_us(root)) {
		set_msg(msg, "%s", k_msg_elevated);
		return U_DESKTOP_ERR_REFUSED;
	}
	return U_DESKTOP_OK;
}

//! Put the cursor exactly on the verified point (it is within 2 px already).
static void
cursor_to(int32_t x, int32_t y)
{
	DpiScope dpi;
	POINT cur;
	if (!GetCursorPos(&cur) || cur.x != x || cur.y != y) {
		SetCursorPos(x, y);
	}
}

static enum u_desktop_result
win32_type_text(struct u_desktop_backend *b, const struct u_desktop_input_guard *guard, const char *utf8, char *msg)
{
	(void)b;
	if (utf8 == NULL || utf8[0] == '\0') {
		return U_DESKTOP_OK;
	}
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0) <= 0) {
		set_msg(msg, "text is not valid UTF-8");
		return U_DESKTOP_ERR_FAILED;
	}
	std::wstring w = utf8_to_utf16(utf8);

	if (window_above_us(root_of(GetForegroundWindow()))) {
		set_msg(msg, "%s", k_msg_elevated);
		return U_DESKTOP_ERR_REFUSED;
	}

	// One "character" = one code point (or one Enter / Tab key), never split
	// across batches so a surrogate pair is always consecutive.
	size_t total_chars = 0;
	for (size_t i = 0; i < w.size(); i++) {
		if (!(IS_HIGH_SURROGATE(w[i]) && i + 1 < w.size() && IS_LOW_SURROGATE(w[i + 1]))) {
			total_chars++;
		}
	}

	std::vector<INPUT> batch;
	size_t i = 0;
	size_t chars_done = 0;
	while (i < w.size()) {
		if (w[i] == L'\r' && i + 1 < w.size() && w[i + 1] == L'\n') {
			i++; // CRLF -> one Enter
			continue;
		}

		// Re-verified before EVERY batch, and every Enter / Tab is a batch of
		// its own: a newline must never land in a window that took focus.
		enum u_desktop_result chk = check_focus(guard, msg);
		if (chk != U_DESKTOP_OK) {
			if (chars_done > 0) {
				char inner[U_DESKTOP_MSG_SIZE];
				snprintf(inner, sizeof(inner), "%s", msg != NULL ? msg : "");
				set_msg(msg, "%s (after %zu of %zu characters)", inner, chars_done, total_chars);
			}
			return chk;
		}

		batch.clear();
		size_t batch_chars = 0;
		wchar_t c = w[i];
		if (c == L'\n' || c == L'\r' || c == L'\t') {
			// Apps expect the Enter / Tab key, not a raw control character.
			INPUT in[2];
			WORD vk = c == L'\t' ? VK_TAB : VK_RETURN;
			key_vk(&in[0], vk, false);
			key_vk(&in[1], vk, true);
			batch.insert(batch.end(), in, in + 2);
			batch_chars = 1;
			i++;
		} else {
			while (i < w.size() && batch_chars < WIN32_TEXT_BATCH_CHARS) {
				c = w[i];
				if (c == L'\n' || c == L'\r' || c == L'\t') {
					break;
				}
				INPUT in[4];
				UINT k = 0;
				if (IS_HIGH_SURROGATE(c) && i + 1 < w.size() && IS_LOW_SURROGATE(w[i + 1])) {
					key_unicode(&in[k++], c, false);
					key_unicode(&in[k++], w[i + 1], false);
					key_unicode(&in[k++], c, true);
					key_unicode(&in[k++], w[i + 1], true);
					i += 2;
				} else {
					key_unicode(&in[k++], c, false);
					key_unicode(&in[k++], c, true);
					i++;
				}
				batch.insert(batch.end(), in, in + k);
				batch_chars++;
			}
		}

		// Every batch carries its own key-ups: nothing is left pressed on a stop.
		enum u_desktop_result r = send_inputs(batch.data(), (UINT)batch.size(), msg);
		if (r != U_DESKTOP_OK) {
			if (chars_done > 0 || r == U_DESKTOP_ERR_FAILED) {
				char inner[U_DESKTOP_MSG_SIZE];
				snprintf(inner, sizeof(inner), "%s", msg != NULL ? msg : "");
				set_msg(msg, "typed %zu of %zu characters: %s", chars_done, total_chars, inner);
				return U_DESKTOP_ERR_FAILED;
			}
			return r;
		}
		chars_done += batch_chars;
		if (i < w.size()) {
			Sleep(1); // let the target drain its queue between batches
		}
	}
	return U_DESKTOP_OK;
}

struct named_key
{
	const char *name;
	WORD vk;
};

//! Layout-independent keys: fixed virtual-key codes.
static const named_key k_named_keys[] = {
    {"enter", VK_RETURN},
    {"tab", VK_TAB},
    {"escape", VK_ESCAPE},
    {"backspace", VK_BACK},
    {"delete", VK_DELETE},
    {"insert", VK_INSERT},
    {"home", VK_HOME},
    {"end", VK_END},
    {"pageup", VK_PRIOR},
    {"pagedown", VK_NEXT},
    {"left", VK_LEFT},
    {"right", VK_RIGHT},
    {"up", VK_UP},
    {"down", VK_DOWN},
    {"space", VK_SPACE},
    {"capslock", VK_CAPITAL},
    {"printscreen", VK_SNAPSHOT},
    {"menu", VK_APPS},
};

//! Character keys: the canonical name -> the character it stands for.
static const struct
{
	const char *name;
	wchar_t ch;
} k_char_keys[] = {
    {"minus", L'-'},       {"equal", L'='},        {"comma", L','},     {"period", L'.'},
    {"slash", L'/'},       {"backslash", L'\\'},   {"semicolon", L';'}, {"quote", L'\''},
    {"bracketleft", L'['}, {"bracketright", L']'}, {"grave", L'`'},
};

//! Keys whose make code is E0-prefixed (MapVirtualKeyEx does not flag all of them).
static bool
vk_is_extended(WORD vk)
{
	switch (vk) {
	case VK_INSERT:
	case VK_DELETE:
	case VK_HOME:
	case VK_END:
	case VK_PRIOR:
	case VK_NEXT:
	case VK_LEFT:
	case VK_RIGHT:
	case VK_UP:
	case VK_DOWN:
	case VK_SNAPSHOT:
	case VK_APPS:
	case VK_LWIN:
	case VK_RWIN:
	case VK_RCONTROL:
	case VK_RMENU:
	case VK_DIVIDE:
	case VK_NUMLOCK: return true;
	default: return false;
	}
}

//! A key event by virtual key, with the scan code of the active layout.
static void
key_vk_hkl(INPUT *in, WORD vk, HKL hkl, bool up)
{
	UINT sc = MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC_EX, hkl);
	bool ext = vk_is_extended(vk) || ((sc >> 8) & 0xFF) == 0xE0 || ((sc >> 8) & 0xFF) == 0xE1;
	memset(in, 0, sizeof(*in));
	in->type = INPUT_KEYBOARD;
	in->ki.wVk = vk;
	in->ki.wScan = (WORD)(sc & 0xFF);
	in->ki.dwFlags = (ext ? KEYEVENTF_EXTENDEDKEY : 0) | (up ? KEYEVENTF_KEYUP : 0);
}

/*!
 * Resolve the chord's key to a virtual key on @p hkl (u_desktop.h: characters
 * on the ACTIVE layout, named keys fixed). @p need_shift is set when the
 * character sits on the Shift level of its key.
 */
static enum u_desktop_result
resolve_chord_key(const char *name, HKL hkl, WORD *out_vk, bool *need_shift, char *msg)
{
	*need_shift = false;
	for (const named_key &k : k_named_keys) {
		if (strcmp(k.name, name) == 0) {
			*out_vk = k.vk;
			return U_DESKTOP_OK;
		}
	}
	if (name[0] == 'f' && name[1] >= '1' && name[1] <= '9') {
		int n = atoi(name + 1);
		if (n >= 1 && n <= 24) {
			*out_vk = (WORD)(VK_F1 + (n - 1));
			return U_DESKTOP_OK;
		}
	}

	wchar_t ch = 0;
	bool is_digit = false;
	if (name[0] != '\0' && name[1] == '\0' &&
	    ((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= '0' && name[0] <= '9'))) {
		ch = (wchar_t)name[0];
		is_digit = name[0] >= '0' && name[0] <= '9';
	} else {
		for (const auto &k : k_char_keys) {
			if (strcmp(k.name, name) == 0) {
				ch = k.ch;
				break;
			}
		}
	}
	if (ch == 0) {
		set_msg(msg, "unknown key name");
		return U_DESKTOP_ERR_FAILED;
	}

	SHORT r = VkKeyScanExW(ch, hkl);
	if (r == -1) {
		if (is_digit) {
			// Every Windows layout keeps VK_0..VK_9 on the number row.
			*out_vk = (WORD)ch;
			return U_DESKTOP_OK;
		}
		set_msg(msg, "this keyboard layout has no key for that character");
		return U_DESKTOP_ERR_UNSUPPORTED;
	}
	WORD vk = (WORD)(r & 0xFF);
	int shift_state = (r >> 8) & 0xFF;
	if ((shift_state & (2 | 4)) != 0) { // Ctrl / Alt (AltGr) level
		set_msg(msg, "this keyboard layout has no key for that character (it needs AltGr)");
		return U_DESKTOP_ERR_UNSUPPORTED;
	}
	*out_vk = vk;
	// Windows matches shortcuts on the VK: AZERTY's number row is VK_1..VK_0
	// on the SHIFT level, and "ctrl+1" there means Ctrl + VK_1, not Ctrl+Shift.
	*need_shift = (shift_state & 1) != 0 && !(is_digit && vk == (WORD)ch);
	return U_DESKTOP_OK;
}

static enum u_desktop_result
win32_key_chord(struct u_desktop_backend *b,
                const struct u_desktop_input_guard *guard,
                const struct u_desktop_chord *chord,
                char *msg)
{
	(void)b;
	if (chord == NULL) {
		return U_DESKTOP_ERR_FAILED;
	}

	HWND fg = root_of(GetForegroundWindow());
	if (window_above_us(fg)) {
		set_msg(msg, "%s", k_msg_elevated);
		return U_DESKTOP_ERR_REFUSED;
	}
	// The layout that will interpret the keys: the foreground thread's.
	HKL hkl = GetKeyboardLayout(fg != NULL ? GetWindowThreadProcessId(fg, NULL) : 0);

	bool have_key = chord->key[0] != '\0';
	WORD vk = 0;
	bool need_shift = false;
	if (have_key) {
		char name[sizeof(chord->key) + 1];
		snprintf(name, sizeof(name), "%.*s", (int)sizeof(chord->key), chord->key);
		enum u_desktop_result r = resolve_chord_key(name, hkl, &vk, &need_shift, msg);
		if (r != U_DESKTOP_OK) {
			return r;
		}
	} else if (chord->mods == 0) {
		set_msg(msg, "empty chord");
		return U_DESKTOP_ERR_FAILED;
	}
	uint32_t mods_bits = chord->mods | (need_shift ? (uint32_t)U_DESKTOP_MOD_SHIFT : 0u);

	// Left-hand modifiers, pressed in a fixed order, released in reverse.
	struct
	{
		uint32_t bit;
		WORD vk;
	} const mods[] = {
	    {U_DESKTOP_MOD_CTRL, VK_LCONTROL},
	    {U_DESKTOP_MOD_SHIFT, VK_LSHIFT},
	    {U_DESKTOP_MOD_ALT, VK_LMENU},
	    {U_DESKTOP_MOD_SUPER, VK_LWIN},
	};
	WORD pressed[4];
	UINT n_pressed = 0;
	auto release_pressed = [&]() {
		INPUT rel[4];
		UINT k = 0;
		for (UINT j = n_pressed; j > 0; j--) {
			key_vk_hkl(&rel[k++], pressed[j - 1], hkl, true);
		}
		if (k > 0) {
			SendInput(k, rel, sizeof(INPUT));
		}
		n_pressed = 0;
	};

	// Every key-down is preceded by the guard + focus check.
	for (const auto &m : mods) {
		if ((mods_bits & m.bit) == 0) {
			continue;
		}
		enum u_desktop_result r = check_focus(guard, msg);
		if (r == U_DESKTOP_OK) {
			INPUT in;
			key_vk_hkl(&in, m.vk, hkl, false);
			r = send_inputs(&in, 1, msg);
		}
		if (r != U_DESKTOP_OK) {
			release_pressed();
			return r;
		}
		pressed[n_pressed++] = m.vk;
	}
	if (have_key) {
		enum u_desktop_result r = check_focus(guard, msg);
		if (r == U_DESKTOP_OK) {
			INPUT in[2];
			key_vk_hkl(&in[0], vk, hkl, false);
			key_vk_hkl(&in[1], vk, hkl, true);
			r = send_inputs(in, 2, msg);
		}
		if (r != U_DESKTOP_OK) {
			release_pressed();
			return r;
		}
	}
	// Releases are never gated: nothing may stay held.
	INPUT rel[4];
	UINT k = 0;
	for (UINT j = n_pressed; j > 0; j--) {
		key_vk_hkl(&rel[k++], pressed[j - 1], hkl, true);
	}
	n_pressed = 0;
	enum u_desktop_result r = send_inputs(rel, k, msg);
	if (r != U_DESKTOP_OK) {
		SendInput(k, rel, sizeof(INPUT)); // one retry: a held modifier is worse than a duplicate key-up
	}
	return r;
}

static enum u_desktop_result
win32_pointer_move(struct u_desktop_backend *b, int32_t x, int32_t y, char *msg)
{
	(void)b;
	DpiScope dpi;
	if (!SetCursorPos(x, y)) {
		set_msg(msg, "SetCursorPos failed (error %lu)", (unsigned long)GetLastError());
		return U_DESKTOP_ERR_FAILED;
	}
	POINT pt;
	if (!GetCursorPos(&pt) || llabs((long long)pt.x - x) > 2 || llabs((long long)pt.y - y) > 2) {
		set_msg(msg, "pointer did not arrive (at %ld,%ld)", (long)pt.x, (long)pt.y);
		return U_DESKTOP_ERR_FAILED;
	}
	return U_DESKTOP_OK;
}

static enum u_desktop_result
win32_pointer_button(struct u_desktop_backend *b,
                     const struct u_desktop_input_guard *guard,
                     enum u_desktop_button button,
                     bool down,
                     int32_t x,
                     int32_t y,
                     char *msg)
{
	(void)b;
	DWORD flag = 0;
	switch (button) {
	case U_DESKTOP_BUTTON_LEFT: flag = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
	case U_DESKTOP_BUTTON_RIGHT: flag = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
	case U_DESKTOP_BUTTON_MIDDLE: flag = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
	default: set_msg(msg, "unknown button %d", (int)button); return U_DESKTOP_ERR_FAILED;
	}
	// A press is gated (guard, cursor still on the point, target still under it,
	// not elevated); a release always goes through — never leave a button held.
	if (down) {
		enum u_desktop_result r = check_point(guard, x, y, msg);
		if (r != U_DESKTOP_OK) {
			return r;
		}
	}
	cursor_to(x, y); // the event happens AT the verified point
	INPUT in;
	memset(&in, 0, sizeof(in));
	in.type = INPUT_MOUSE;
	in.mi.dwFlags = flag; // no MOVE: acts at the cursor, now on (x, y)
	return send_inputs(&in, 1, msg);
}

static enum u_desktop_result
win32_scroll(struct u_desktop_backend *b,
             const struct u_desktop_input_guard *guard,
             int32_t x,
             int32_t y,
             int32_t dx,
             int32_t dy,
             char *msg)
{
	(void)b;
	auto clamp = [](int32_t v) -> int32_t {
		return v > WIN32_SCROLL_MAX_NOTCHES ? WIN32_SCROLL_MAX_NOTCHES
		                                    : (v < -WIN32_SCROLL_MAX_NOTCHES ? -WIN32_SCROLL_MAX_NOTCHES : v);
	};
	dx = clamp(dx);
	dy = clamp(dy);

	// Two bursts (vertical, horizontal), each re-verified before it is sent.
	// One event per notch: some apps act on a single WHEEL_DELTA per message.
	for (int axis = 0; axis < 2; axis++) {
		int32_t v = axis == 0 ? dy : dx;
		if (v == 0) {
			continue;
		}
		enum u_desktop_result r = check_point(guard, x, y, msg);
		if (r != U_DESKTOP_OK) {
			return r;
		}
		cursor_to(x, y);
		std::vector<INPUT> in;
		for (int32_t i = 0; i < (v < 0 ? -v : v); i++) {
			INPUT e;
			memset(&e, 0, sizeof(e));
			e.type = INPUT_MOUSE;
			if (axis == 0) {
				e.mi.dwFlags = MOUSEEVENTF_WHEEL;
				e.mi.mouseData =
				    (DWORD)(v > 0 ? -WHEEL_DELTA : WHEEL_DELTA); // +y down = toward the user
			} else {
				e.mi.dwFlags = MOUSEEVENTF_HWHEEL;
				e.mi.mouseData = (DWORD)(v > 0 ? WHEEL_DELTA : -WHEEL_DELTA); // +x right
			}
			in.push_back(e);
		}
		r = send_inputs(in.data(), (UINT)in.size(), msg);
		if (r != U_DESKTOP_OK) {
			return r;
		}
	}
	return U_DESKTOP_OK;
}

/*
 *
 * Lifecycle.
 *
 */

static void
win32_destroy(struct u_desktop_backend *b)
{
	win32_backend *wb = win32(b);

	// Lifetime (u_desktop.h): never return while a consent answer is owed.
	AcquireSRWLockExclusive(&wb->prompt_lock);
	prompt_state *p = wb->prompt;
	wb->prompt = NULL;
	ReleaseSRWLockExclusive(&wb->prompt_lock);
	if (p != NULL) {
		prompt_shutdown(p);
	}

	uia_worker *w = wb->uia;
	if (w != NULL) {
		AcquireSRWLockExclusive(&w->lock);
		w->quit = true;
		WakeAllConditionVariable(&w->cv);
		ReleaseSRWLockExclusive(&w->lock);
		// A provider stuck past UIA's own timeouts leaves the thread running;
		// it then frees the shared state itself (refcount) when it returns.
		// Accepted for v1: a thread still stuck here would crash if the runtime DLL were unloaded.
		WaitForSingleObject(w->thread, 2 * WIN32_UIA_PROVIDER_MS);
		CloseHandle(w->thread);
		uia_worker_unref(w);
	}
	delete wb;
}

extern "C" struct u_desktop_backend *
u_desktop_backend_create_platform(void)
{
	win32_backend *wb = new win32_backend();
	memset(&wb->base, 0, sizeof(wb->base));
	InitializeSRWLock(&wb->uia_init_lock);
	wb->uia = NULL;
	wb->uia_failed = false;
	InitializeSRWLock(&wb->prompt_lock);
	wb->prompt = NULL;

	// Resolve the lazily-initialised statics now, off the hot path.
	(void)get_set_thread_dpi_ctx();
	(void)self_integrity();

	struct u_desktop_backend *b = &wb->base;
	b->name = "win32";
	b->caps = U_DESKTOP_CAP_LIST_WINDOWS | U_DESKTOP_CAP_POINTER_POSITION | U_DESKTOP_CAP_PICK |
	          U_DESKTOP_CAP_READ_TEXT | U_DESKTOP_CAP_WINDOW_ACTIONS | U_DESKTOP_CAP_INPUT;
	b->remembers_in_store = true;
	b->access_status = win32_access_status;
	b->request_access = win32_request_access;
	b->is_locked = win32_is_locked;
	b->user_idle_ms = win32_user_idle_ms;
	b->list_windows = win32_list_windows;
	b->get_pointer = win32_get_pointer;
	b->pick = win32_pick;
	b->focused_window = win32_focused_window;
	b->text_at = win32_text_at;
	b->prepare_input = NULL; // nothing slow to set up on Windows
	b->window_op = win32_window_op;
	b->type_text = win32_type_text;
	b->key_chord = win32_key_chord;
	b->pointer_move = win32_pointer_move;
	b->pointer_button = win32_pointer_button;
	b->scroll = win32_scroll;
	b->destroy = win32_destroy;
	return b;
}
