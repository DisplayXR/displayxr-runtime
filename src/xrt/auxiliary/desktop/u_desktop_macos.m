// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Desktop-control backend: macOS (XR_DXR_desktop_control, ADR-056).
 *
 * Mechanism only (u_desktop.h): CGWindowList for windows / z-order / hit tests,
 * the Accessibility API (AX) for text, window actions and the CGWindowID <->
 * AXUIElement bridge, CGEvent for input. Consent is TCC's: Accessibility (needed
 * for everything that mutates, for AX reads and for posting events) and Screen
 * Recording (only makes other apps' window titles visible in CGWindowList; we
 * fall back to AXTitle without it). TCC attributes both to the RESPONSIBLE
 * process — the app bundle, or the terminal when launched from one — see
 * docs/specs/extensions/XR_DXR_desktop_control_macos_test_plan.md.
 *
 * Coordinates: global points, origin top-left of the primary display, y down —
 * the space CGWindowList, CGEvent and AX already use. NSScreen (bottom-left) is
 * converted where used.
 *
 * Threading: observation runs on any thread. Everything it touches is either a
 * stateless CG / AX call or a small cache behind `lock`, never held across an AX
 * round trip. AppKit main-thread-only API (NSScreen, NSApp windows) is used only
 * when the caller IS the main thread; off the main thread screen geometry comes
 * from CGDisplay + CGWindowList + the Dock's AX frame. AX requests to OUR OWN
 * process are serviced on our main run loop, so they are never made from the
 * main thread (they would block until the messaging timeout).
 *
 * Built without ARC: every Copy/Create is CFRelease'd explicitly.
 */

#include "u_desktop.h"

#import <AppKit/AppKit.h>
#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h> // Text Input Sources + UCKeyTranslate (active keyboard layout)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>

#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 *
 * Small helpers.
 *
 */

static uint64_t
now_ns(void)
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static void
sleep_ms(unsigned ms)
{
	usleep(ms * 1000u);
}

static void
set_msg(char *msg, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

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

//! Copy UTF-8 @p src into @p dst (capacity @p cap), truncating on a code-point boundary.
static void
copy_utf8(char *dst, size_t cap, const char *src)
{
	if (cap == 0) {
		return;
	}
	size_t n = src ? strlen(src) : 0;
	if (n >= cap) {
		n = cap - 1;
		// Back off continuation bytes so we never cut a code point in half.
		while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
			n--;
		}
	}
	if (n > 0) {
		memcpy(dst, src, n);
	}
	dst[n] = '\0';
}

//! CFString -> UTF-8, truncated on a code-point boundary. False if @p s is not a non-empty string.
static bool
cfstr_to_utf8(CFTypeRef s, char *buf, size_t cap)
{
	if (cap > 0) {
		buf[0] = '\0';
	}
	if (s == NULL || CFGetTypeID(s) != CFStringGetTypeID() || CFStringGetLength((CFStringRef)s) == 0) {
		return false;
	}
	CFIndex max = CFStringGetMaximumSizeForEncoding(CFStringGetLength((CFStringRef)s), kCFStringEncodingUTF8) + 1;
	char *tmp = malloc((size_t)max);
	if (tmp == NULL) {
		return false;
	}
	bool ok = CFStringGetCString((CFStringRef)s, tmp, max, kCFStringEncodingUTF8);
	if (ok) {
		copy_utf8(buf, cap, tmp);
	}
	free(tmp);
	return ok && buf[0] != '\0';
}

static int32_t
iround(double v)
{
	return (int32_t)lround(v);
}

static struct u_desktop_rect
rect_from_cg(CGRect r)
{
	struct u_desktop_rect o = {iround(r.origin.x), iround(r.origin.y), iround(r.size.width),
	                           iround(r.size.height)};
	return o;
}

static bool
rect_near(CGRect a, CGRect b, double tol)
{
	return fabs(a.origin.x - b.origin.x) <= tol && fabs(a.origin.y - b.origin.y) <= tol &&
	       fabs(a.size.width - b.size.width) <= tol && fabs(a.size.height - b.size.height) <= tol;
}

static bool
is_main_thread(void)
{
	return pthread_main_np() != 0;
}

/*
 *
 * CGWindowList dictionary accessors.
 *
 */

static int64_t
dict_int(CFDictionaryRef d, CFStringRef key, int64_t def)
{
	CFNumberRef n = CFDictionaryGetValue(d, key);
	int64_t v = def;
	if (n != NULL && CFGetTypeID(n) == CFNumberGetTypeID()) {
		CFNumberGetValue(n, kCFNumberSInt64Type, &v);
	}
	return v;
}

static double
dict_double(CFDictionaryRef d, CFStringRef key, double def)
{
	CFNumberRef n = CFDictionaryGetValue(d, key);
	double v = def;
	if (n != NULL && CFGetTypeID(n) == CFNumberGetTypeID()) {
		CFNumberGetValue(n, kCFNumberDoubleType, &v);
	}
	return v;
}

static CGRect
dict_bounds(CFDictionaryRef d)
{
	CGRect r = CGRectZero;
	CFDictionaryRef b = CFDictionaryGetValue(d, kCGWindowBounds);
	if (b != NULL) {
		CGRectMakeWithDictionaryRepresentation(b, &r);
	}
	return r;
}

//! Info dictionary of one window (on screen or not), or NULL if it no longer exists. Caller releases.
static CFDictionaryRef
cg_window_info(CGWindowID wid)
{
	const void *ids[1] = {(const void *)(uintptr_t)wid};
	CFArrayRef arr = CFArrayCreate(NULL, ids, 1, NULL);
	if (arr == NULL) {
		return NULL;
	}
	CFArrayRef desc = CGWindowListCreateDescriptionFromArray(arr);
	CFRelease(arr);
	CFDictionaryRef out = NULL;
	if (desc != NULL) {
		if (CFArrayGetCount(desc) > 0) {
			out = CFRetain(CFArrayGetValueAtIndex(desc, 0));
		}
		CFRelease(desc);
	}
	return out;
}

/*
 *
 * AX helpers.
 *
 */

//! Private but long-stable (used by every macOS window manager): AX window -> CGWindowID.
typedef AXError (*ax_get_window_fn)(AXUIElementRef el, CGWindowID *out);

#define AX_OBSERVE_TIMEOUT_S 0.15f
#define AX_ACTION_TIMEOUT_S 1.0f

static CFTypeRef
ax_copy(AXUIElementRef el, CFStringRef attr)
{
	CFTypeRef v = NULL;
	if (el == NULL || AXUIElementCopyAttributeValue(el, attr, &v) != kAXErrorSuccess) {
		return NULL;
	}
	return v;
}

static bool
ax_get_bool(AXUIElementRef el, CFStringRef attr, bool *out)
{
	CFTypeRef v = ax_copy(el, attr);
	if (v == NULL) {
		return false;
	}
	bool ok = CFGetTypeID(v) == CFBooleanGetTypeID();
	if (ok) {
		*out = CFBooleanGetValue(v);
	}
	CFRelease(v);
	return ok;
}

static bool
ax_get_frame(AXUIElementRef el, CGRect *out)
{
	CFTypeRef p = ax_copy(el, kAXPositionAttribute);
	CFTypeRef s = ax_copy(el, kAXSizeAttribute);
	bool ok = false;
	if (p != NULL && s != NULL && CFGetTypeID(p) == AXValueGetTypeID() && CFGetTypeID(s) == AXValueGetTypeID()) {
		CGPoint pt;
		CGSize sz;
		ok = AXValueGetValue(p, kAXValueCGPointType, &pt) && AXValueGetValue(s, kAXValueCGSizeType, &sz);
		if (ok) {
			*out = CGRectMake(pt.x, pt.y, sz.width, sz.height);
		}
	}
	if (p) {
		CFRelease(p);
	}
	if (s) {
		CFRelease(s);
	}
	return ok;
}

static bool
ax_set_point(AXUIElementRef el, CFStringRef attr, CGPoint pt)
{
	AXValueRef v = AXValueCreate(kAXValueCGPointType, &pt);
	AXError e = AXUIElementSetAttributeValue(el, attr, v);
	CFRelease(v);
	return e == kAXErrorSuccess;
}

static bool
ax_set_size(AXUIElementRef el, CGSize sz)
{
	AXValueRef v = AXValueCreate(kAXValueCGSizeType, &sz);
	AXError e = AXUIElementSetAttributeValue(el, kAXSizeAttribute, v);
	CFRelease(v);
	return e == kAXErrorSuccess;
}

static bool
ax_set_bool(AXUIElementRef el, CFStringRef attr, bool v)
{
	return AXUIElementSetAttributeValue(el, attr, v ? kCFBooleanTrue : kCFBooleanFalse) == kAXErrorSuccess;
}

static bool
ax_role_is(AXUIElementRef el, CFStringRef role)
{
	CFTypeRef r = ax_copy(el, kAXRoleAttribute);
	bool is = r != NULL && CFGetTypeID(r) == CFStringGetTypeID() && CFEqual(r, role);
	if (r) {
		CFRelease(r);
	}
	return is;
}

static AXUIElementRef
ax_app_create(pid_t pid, float timeout)
{
	AXUIElementRef app = AXUIElementCreateApplication(pid);
	if (app != NULL) {
		AXUIElementSetMessagingTimeout(app, timeout);
	}
	return app;
}

/*
 *
 * Backend state.
 *
 */

#define AX_CACHE_APPS 32
#define AX_CACHE_WINDOWS 64
#define AX_CACHE_TTL_NS (1000ull * 1000 * 1000)
#define APPID_CACHE 64
#define SAVED_FRAMES 64
#define MINIMIZED_MAX 64
#define SCREENS_MAX 16
#define SCREENS_TTL_NS (2000ull * 1000 * 1000)

struct ax_win
{
	CGWindowID wid;
	CGWindowID parent; //!< for sheets: the window the sheet hangs off, else 0
	AXUIElementRef el; //!< retained
	bool minimized;    //!< AXMinimized at build time
	char title[U_DESKTOP_TITLE_SIZE]; //!< AXTitle at build time
};

struct ax_app_entry
{
	pid_t pid;      //!< 0 = free slot
	uint64_t built; //!< now_ns() at build
	uint64_t used;  //!< LRU
	bool sheets_scanned;
	uint32_t n;
	struct ax_win w[AX_CACHE_WINDOWS];
};

struct appid_entry
{
	pid_t pid;
	uint64_t at;
	char app_id[U_DESKTOP_APP_ID_SIZE];
	bool regular; //!< NSApplicationActivationPolicyRegular
};

struct saved_frame
{
	CGWindowID wid;
	CGRect frame;
};

struct screen_info
{
	CGRect bounds;  //!< CGDisplayBounds, top-left space
	CGRect visible; //!< minus menu bar and Dock, top-left space
};

struct macos_backend
{
	struct u_desktop_backend base;

	pthread_mutex_t lock; //!< guards every cache below; never held across AX / CG calls
	ax_get_window_fn ax_get_window;

	struct ax_app_entry ax[AX_CACHE_APPS];
	struct appid_entry appid[APPID_CACHE];

	struct saved_frame saved[SAVED_FRAMES]; //!< pre-MAXIMIZE frames, for RESTORE
	uint32_t saved_next;

	struct u_desktop_window minimized[MINIMIZED_MAX];
	uint32_t minimized_n;
	uint64_t minimized_at;

	struct screen_info screens[SCREENS_MAX];
	uint32_t screens_n;
	uint64_t screens_at;

	// AX refresher: keeps the per-pid AX tables and the minimized list warm so
	// observation never makes a per-window AX round trip (an App-Napped app
	// takes 15-50 ms to answer the first one, a hung one the whole timeout).
	pthread_cond_t cond;
	pthread_t thread;
	bool thread_started;
	bool stop;
	uint64_t demand_at;    //!< last list_windows / pick
	uint64_t snapshot_gen; //!< bumped after every refresh
	struct
	{
		pid_t pid;
		uint64_t until;
	} slow[16]; //!< apps that timed out: skipped by the refresher until `until`

	// Worker-thread-only input state.
	CGEventSourceRef src;
	uint32_t buttons_down; //!< bit per enum u_desktop_button
	int last_click_button;
	uint64_t last_click_ns;
	CGPoint last_click_pt;
	int64_t click_count;
};

static inline struct macos_backend *
mb(struct u_desktop_backend *b)
{
	return (struct macos_backend *)b;
}

static bool
trusted(void)
{
	return AXIsProcessTrusted();
}

/*
 *
 * App identity cache (bundle id / owner name, activation policy).
 *
 */

static void
app_identity(struct macos_backend *m, pid_t pid, const char *owner_name, char *out, size_t cap, bool *regular)
{
	uint64_t t = now_ns();
	pthread_mutex_lock(&m->lock);
	for (int i = 0; i < APPID_CACHE; i++) {
		if (m->appid[i].pid == pid && t - m->appid[i].at < 10ull * 1000 * 1000 * 1000) {
			copy_utf8(out, cap, m->appid[i].app_id);
			if (regular) {
				*regular = m->appid[i].regular;
			}
			pthread_mutex_unlock(&m->lock);
			return;
		}
	}
	pthread_mutex_unlock(&m->lock);

	char id[U_DESKTOP_APP_ID_SIZE] = {0};
	bool reg = false;
	@autoreleasepool {
		NSRunningApplication *app = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
		if (app != nil) {
			reg = app.activationPolicy == NSApplicationActivationPolicyRegular;
			NSString *bid = app.bundleIdentifier;
			if (bid != nil && bid.length > 0) {
				copy_utf8(id, sizeof(id), bid.UTF8String);
			} else if (app.localizedName != nil) {
				copy_utf8(id, sizeof(id), app.localizedName.UTF8String);
			}
		}
	}
	if (id[0] == '\0' && owner_name != NULL) {
		copy_utf8(id, sizeof(id), owner_name);
	}

	pthread_mutex_lock(&m->lock);
	int slot = 0;
	for (int i = 0; i < APPID_CACHE; i++) {
		if (m->appid[i].pid == pid || m->appid[i].at < m->appid[slot].at) {
			slot = i;
			if (m->appid[i].pid == pid) {
				break;
			}
		}
	}
	m->appid[slot].pid = pid;
	m->appid[slot].at = t;
	m->appid[slot].regular = reg;
	copy_utf8(m->appid[slot].app_id, sizeof(m->appid[slot].app_id), id);
	pthread_mutex_unlock(&m->lock);

	copy_utf8(out, cap, id);
	if (regular) {
		*regular = reg;
	}
}

/*
 *
 * CGWindowID <-> AXUIElement, cached per pid.
 *
 */

static void
ax_entry_clear(struct ax_app_entry *e)
{
	for (uint32_t i = 0; i < e->n; i++) {
		if (e->w[i].el) {
			CFRelease(e->w[i].el);
		}
	}
	memset(e, 0, sizeof(*e));
}

//! Build the AX window table of @p pid (outside the lock). Fallback without the private
//! symbol: match each AX window to a CG window of the same pid by frame, then title.
static void
ax_build_table(struct macos_backend *m, pid_t pid, float timeout, struct ax_app_entry *out)
{
	memset(out, 0, sizeof(*out));
	out->pid = pid;
	out->built = now_ns();

	AXUIElementRef app = ax_app_create(pid, timeout);
	if (app == NULL) {
		return;
	}
	CFTypeRef wins = ax_copy(app, kAXWindowsAttribute);
	CFRelease(app);
	if (wins == NULL || CFGetTypeID(wins) != CFArrayGetTypeID()) {
		if (wins) {
			CFRelease(wins);
		}
		return;
	}

	CFArrayRef cg_all = NULL;
	if (m->ax_get_window == NULL) {
		cg_all = CGWindowListCopyWindowInfo(kCGWindowListOptionAll, kCGNullWindowID);
	}
	bool taken[1024] = {0};

	CFIndex n = CFArrayGetCount(wins);
	for (CFIndex i = 0; i < n && out->n < AX_CACHE_WINDOWS; i++) {
		AXUIElementRef el = (AXUIElementRef)CFArrayGetValueAtIndex(wins, i);
		CGWindowID wid = 0;
		if (m->ax_get_window != NULL) {
			if (m->ax_get_window(el, &wid) != kAXErrorSuccess) {
				wid = 0;
			}
		} else if (cg_all != NULL) {
			CGRect fr;
			if (ax_get_frame(el, &fr)) {
				char title[U_DESKTOP_TITLE_SIZE] = {0};
				CFTypeRef t = ax_copy(el, kAXTitleAttribute);
				cfstr_to_utf8(t, title, sizeof(title));
				if (t) {
					CFRelease(t);
				}
				CFIndex best = -1;
				CFIndex cn = CFArrayGetCount(cg_all);
				for (CFIndex k = 0; k < cn && k < 1024; k++) {
					CFDictionaryRef d = CFArrayGetValueAtIndex(cg_all, k);
					if (taken[k] || dict_int(d, kCGWindowOwnerPID, -1) != pid ||
					    dict_int(d, kCGWindowLayer, -1) != 0 || !rect_near(dict_bounds(d), fr, 1.0)) {
						continue;
					}
					char cgt[U_DESKTOP_TITLE_SIZE] = {0};
					cfstr_to_utf8(CFDictionaryGetValue(d, kCGWindowName), cgt, sizeof(cgt));
					if (best < 0 || (title[0] && strcmp(title, cgt) == 0)) {
						best = k;
					}
				}
				if (best >= 0) {
					taken[best] = true;
					wid = (CGWindowID)dict_int(CFArrayGetValueAtIndex(cg_all, best), kCGWindowNumber, 0);
				}
			}
		}
		if (wid == 0) {
			continue;
		}
		struct ax_win *w = &out->w[out->n++];
		w->wid = wid;
		w->parent = 0;
		w->el = CFRetain(el);
		CFTypeRef t = ax_copy(el, kAXTitleAttribute);
		cfstr_to_utf8(t, w->title, sizeof(w->title));
		if (t) {
			CFRelease(t);
		}
		bool mini = false;
		w->minimized = ax_get_bool(el, kAXMinimizedAttribute, &mini) && mini;
	}
	CFRelease(wins);
	if (cg_all) {
		CFRelease(cg_all);
	}
}

//! Add the AXSheet children of every window of @p e (sheets are separate CG windows).
static void
ax_scan_sheets(struct macos_backend *m, struct ax_app_entry *e)
{
	e->sheets_scanned = true;
	if (m->ax_get_window == NULL) {
		return;
	}
	uint32_t top = e->n;
	for (uint32_t i = 0; i < top && e->n < AX_CACHE_WINDOWS; i++) {
		CFTypeRef kids = ax_copy(e->w[i].el, kAXChildrenAttribute);
		if (kids == NULL || CFGetTypeID(kids) != CFArrayGetTypeID()) {
			if (kids) {
				CFRelease(kids);
			}
			continue;
		}
		CFIndex kn = CFArrayGetCount(kids);
		for (CFIndex k = 0; k < kn && e->n < AX_CACHE_WINDOWS; k++) {
			AXUIElementRef c = (AXUIElementRef)CFArrayGetValueAtIndex(kids, k);
			if (!ax_role_is(c, kAXSheetRole)) {
				continue;
			}
			CGWindowID wid = 0;
			if (m->ax_get_window(c, &wid) == kAXErrorSuccess && wid != 0) {
				struct ax_win *w = &e->w[e->n++];
				memset(w, 0, sizeof(*w));
				w->wid = wid;
				w->parent = e->w[i].wid;
				w->el = CFRetain(c);
				CFTypeRef t = ax_copy(c, kAXTitleAttribute);
				cfstr_to_utf8(t, w->title, sizeof(w->title));
				if (t) {
					CFRelease(t);
				}
			}
		}
		CFRelease(kids);
	}
}

static bool
ax_entry_find(struct ax_app_entry *e, CGWindowID wid, AXUIElementRef *out_el, CGWindowID *out_parent)
{
	for (uint32_t i = 0; i < e->n; i++) {
		if (e->w[i].wid == wid) {
			if (out_el) {
				*out_el = CFRetain(e->w[i].el);
			}
			if (out_parent) {
				*out_parent = e->w[i].parent;
			}
			return true;
		}
	}
	return false;
}

//! Publish a freshly built table (replacing this pid's slot, else the LRU one); frees @p built.
static void
ax_publish(struct macos_backend *m, struct ax_app_entry *built)
{
	uint64_t t = now_ns();
	pthread_mutex_lock(&m->lock);
	int slot = 0;
	for (int i = 0; i < AX_CACHE_APPS; i++) {
		if (m->ax[i].pid == built->pid) {
			slot = i;
			break;
		}
		if (m->ax[i].used < m->ax[slot].used) {
			slot = i;
		}
	}
	ax_entry_clear(&m->ax[slot]);
	m->ax[slot] = *built; // takes over the element references
	m->ax[slot].used = t;
	pthread_mutex_unlock(&m->lock);
	free(built);
}

/*!
 * The AXUIElement of window @p wid owned by @p pid (retained, caller releases).
 * Uses the per-pid cache; on a miss rebuilds once (and scans sheets once); a
 * repeated miss is cached too unless @p force (actions: the window may be new).
 * Never call for our own pid from the main thread.
 */
static bool
ax_window_for(struct macos_backend *m,
              pid_t pid,
              CGWindowID wid,
              float timeout,
              bool force,
              AXUIElementRef *out_el,
              CGWindowID *out_parent)
{
	if (pid == getpid() && is_main_thread()) {
		return false;
	}
	uint64_t t = now_ns();
	bool fresh_hit = false;
	bool negative = false;
	pthread_mutex_lock(&m->lock);
	for (int i = 0; i < AX_CACHE_APPS; i++) {
		struct ax_app_entry *e = &m->ax[i];
		if (e->pid == pid && t - e->built < AX_CACHE_TTL_NS) {
			e->used = t;
			fresh_hit = ax_entry_find(e, wid, out_el, out_parent);
			// A fresh table that already scanned sheets and still misses: the
			// window has no AX element (another Space, no AX support). Do not
			// rebuild on every call — unless the caller is about to act on it.
			negative = !fresh_hit && e->sheets_scanned && !force;
			break;
		}
	}
	pthread_mutex_unlock(&m->lock);
	if (fresh_hit) {
		return true;
	}
	if (negative) {
		return false;
	}

	struct ax_app_entry *built = calloc(1, sizeof(*built));
	if (built == NULL) {
		return false;
	}
	ax_build_table(m, pid, timeout, built);
	if (!ax_entry_find(built, wid, NULL, NULL)) {
		ax_scan_sheets(m, built);
	}
	bool found = ax_entry_find(built, wid, out_el, out_parent);

	ax_publish(m, built);
	return found;
}

/*!
 * Observation-side lookup: the cached AX data of @p wid, no AX round trip.
 * Filled by the refresher thread (and by actions); may be up to ~1 s old.
 */
static bool
ax_cache_peek(struct macos_backend *m, pid_t pid, CGWindowID wid, CGWindowID *parent, char *title, size_t cap)
{
	bool found = false;
	pthread_mutex_lock(&m->lock);
	for (int i = 0; i < AX_CACHE_APPS && !found; i++) {
		struct ax_app_entry *e = &m->ax[i];
		if (e->pid != pid) {
			continue;
		}
		for (uint32_t k = 0; k < e->n; k++) {
			if (e->w[k].wid == wid) {
				if (parent) {
					*parent = e->w[k].parent;
				}
				if (title) {
					copy_utf8(title, cap, e->w[k].title);
				}
				found = true;
				break;
			}
		}
		break;
	}
	pthread_mutex_unlock(&m->lock);
	return found;
}

//! CGWindowID of an AX window element (private symbol, else pid + frame match). 0 if unknown.
static CGWindowID
ax_element_window_id(struct macos_backend *m, AXUIElementRef win)
{
	CGWindowID wid = 0;
	if (m->ax_get_window != NULL) {
		if (m->ax_get_window(win, &wid) == kAXErrorSuccess) {
			return wid;
		}
		return 0;
	}
	pid_t pid = 0;
	CGRect fr;
	if (AXUIElementGetPid(win, &pid) != kAXErrorSuccess || !ax_get_frame(win, &fr)) {
		return 0;
	}
	CFArrayRef all = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
	if (all == NULL) {
		return 0;
	}
	for (CFIndex i = 0; i < CFArrayGetCount(all); i++) {
		CFDictionaryRef d = CFArrayGetValueAtIndex(all, i);
		if (dict_int(d, kCGWindowOwnerPID, -1) == pid && rect_near(dict_bounds(d), fr, 1.0)) {
			wid = (CGWindowID)dict_int(d, kCGWindowNumber, 0);
			break;
		}
	}
	CFRelease(all);
	return wid;
}

/*
 *
 * Screens (for MAXIMIZE and the MAXIMIZED / FULLSCREEN state bits).
 *
 */

//! Off-main: the Dock's frame from its AX list element (top-left space).
static bool
dock_frame(CGRect *out)
{
	bool ok = false;
	@autoreleasepool {
		NSArray *docks = [NSRunningApplication runningApplicationsWithBundleIdentifier:@"com.apple.dock"];
		if (docks.count == 0 || !trusted()) {
			return false;
		}
		pid_t pid = ((NSRunningApplication *)docks[0]).processIdentifier;
		AXUIElementRef app = ax_app_create(pid, AX_OBSERVE_TIMEOUT_S);
		CFTypeRef kids = ax_copy(app, kAXChildrenAttribute);
		if (kids != NULL && CFGetTypeID(kids) == CFArrayGetTypeID()) {
			for (CFIndex i = 0; i < CFArrayGetCount(kids) && !ok; i++) {
				AXUIElementRef c = (AXUIElementRef)CFArrayGetValueAtIndex(kids, i);
				if (ax_role_is(c, kAXListRole)) {
					ok = ax_get_frame(c, out);
				}
			}
		}
		if (kids) {
			CFRelease(kids);
		}
		CFRelease(app);
	}
	return ok;
}

static uint32_t
screens_compute(struct screen_info *out)
{
	uint32_t n = 0;
	if (is_main_thread()) {
		@autoreleasepool {
			NSArray *ss = [NSScreen screens];
			if (ss.count > 0) {
				CGFloat primary_h = ((NSScreen *)ss[0]).frame.size.height;
				for (NSScreen *s in ss) {
					if (n >= SCREENS_MAX) {
						break;
					}
					NSRect f = s.frame, v = s.visibleFrame;
					out[n].bounds = CGRectMake(f.origin.x, primary_h - (f.origin.y + f.size.height),
					                           f.size.width, f.size.height);
					out[n].visible = CGRectMake(v.origin.x, primary_h - (v.origin.y + v.size.height),
					                            v.size.width, v.size.height);
					n++;
				}
				return n;
			}
		}
	}

	// Off the main thread: CGDisplay bounds, minus the menu bar window on each
	// display (Window Server, kCGMainMenuWindowLevel) and the Dock's AX frame.
	CGDirectDisplayID ids[SCREENS_MAX];
	uint32_t dn = 0;
	if (CGGetActiveDisplayList(SCREENS_MAX, ids, &dn) != kCGErrorSuccess) {
		return 0;
	}
	CFArrayRef all = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
	CGRect dock;
	bool have_dock = dock_frame(&dock);
	for (uint32_t i = 0; i < dn; i++) {
		CGRect b = CGDisplayBounds(ids[i]);
		CGRect v = b;
		for (CFIndex k = 0; all != NULL && k < CFArrayGetCount(all); k++) {
			CFDictionaryRef d = CFArrayGetValueAtIndex(all, k);
			if (dict_int(d, kCGWindowLayer, 0) != kCGMainMenuWindowLevel) {
				continue;
			}
			CGRect mbar = dict_bounds(d);
			if (fabs(mbar.origin.y - b.origin.y) < 1 && CGRectContainsPoint(b, mbar.origin) &&
			    mbar.size.width >= b.size.width - 1) {
				v.origin.y += mbar.size.height;
				v.size.height -= mbar.size.height;
				break;
			}
		}
		if (have_dock) {
			CGRect in = CGRectIntersection(dock, v);
			if (!CGRectIsNull(in) && in.size.width > 0 && in.size.height > 0) {
				if (in.size.width >= in.size.height) { // bottom (or top) dock
					if (CGRectGetMaxY(in) >= CGRectGetMaxY(v) - 1) {
						v.size.height = in.origin.y - v.origin.y;
					}
				} else if (in.origin.x <= v.origin.x + 1) { // left dock
					v.size.width -= CGRectGetMaxX(in) - v.origin.x;
					v.origin.x = CGRectGetMaxX(in);
				} else { // right dock
					v.size.width = in.origin.x - v.origin.x;
				}
			}
		}
		out[n].bounds = b;
		out[n].visible = v;
		n++;
	}
	if (all) {
		CFRelease(all);
	}
	return n;
}

static uint32_t
screens_get(struct macos_backend *m, struct screen_info *out)
{
	uint64_t t = now_ns();
	pthread_mutex_lock(&m->lock);
	bool fresh = m->screens_n > 0 && t - m->screens_at < SCREENS_TTL_NS;
	uint32_t n = m->screens_n;
	if (fresh) {
		memcpy(out, m->screens, sizeof(struct screen_info) * n);
	}
	pthread_mutex_unlock(&m->lock);
	if (fresh) {
		return n;
	}
	n = screens_compute(out);
	pthread_mutex_lock(&m->lock);
	memcpy(m->screens, out, sizeof(struct screen_info) * n);
	m->screens_n = n;
	m->screens_at = t;
	pthread_mutex_unlock(&m->lock);
	return n;
}

static int
screen_for_rect(const struct screen_info *s, uint32_t n, CGRect r)
{
	int best = -1;
	double best_area = 0;
	for (uint32_t i = 0; i < n; i++) {
		CGRect in = CGRectIntersection(s[i].bounds, r);
		double a = CGRectIsNull(in) ? 0 : in.size.width * in.size.height;
		if (a > best_area) {
			best_area = a;
			best = (int)i;
		}
	}
	return best;
}

static bool
point_on_any_display(int32_t x, int32_t y)
{
	uint32_t count = 0;
	CGGetDisplaysWithPoint(CGPointMake(x, y), 0, NULL, &count);
	return count > 0;
}

/*
 *
 * Focus.
 *
 */

//! Focused window via AX (frontmost app's AXFocusedWindow). 0 when unknown.
static CGWindowID
focused_wid_ax(struct macos_backend *m)
{
	CGWindowID wid = 0;
	AXUIElementRef sys = AXUIElementCreateSystemWide();
	AXUIElementSetMessagingTimeout(sys, AX_OBSERVE_TIMEOUT_S);
	AXUIElementRef app = (AXUIElementRef)ax_copy(sys, kAXFocusedApplicationAttribute);
	CFRelease(sys);
	if (app == NULL) {
		// The system-wide element answers by asking the focused app, and some
		// apps (Electron, verified with Cursor) make it fail with
		// kAXErrorCannotComplete. Fall back to NSWorkspace's frontmost app — its
		// value is refreshed by notifications on the main run loop, so it can be
		// stale in a process that never runs one (fine in an app).
		pid_t front = 0;
		@autoreleasepool {
			front = [[NSWorkspace sharedWorkspace] frontmostApplication].processIdentifier;
		}
		if (front <= 0) {
			return 0;
		}
		app = AXUIElementCreateApplication(front);
		if (app == NULL) {
			return 0;
		}
	}
	pid_t pid = 0;
	AXUIElementGetPid(app, &pid);
	if (pid == getpid() && is_main_thread()) {
		// Our own app is frontmost; an AX request to ourselves from the main
		// thread would stall until the timeout. Ask AppKit directly.
		@autoreleasepool {
			NSWindow *k = [NSApp keyWindow];
			wid = k != nil ? (CGWindowID)k.windowNumber : 0;
		}
		CFRelease(app);
		return wid;
	}
	AXUIElementSetMessagingTimeout(app, AX_OBSERVE_TIMEOUT_S);
	AXUIElementRef win = (AXUIElementRef)ax_copy(app, kAXFocusedWindowAttribute);
	CFRelease(app);
	if (win != NULL) {
		wid = ax_element_window_id(m, win);
		CFRelease(win);
	}
	return wid;
}

//! Without AX: the top-most normal-layer window (belongs to the frontmost app in practice).
static CGWindowID
focused_wid_cg(void)
{
	CGWindowID wid = 0;
	CFArrayRef all = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
	                                            kCGNullWindowID);
	for (CFIndex i = 0; all != NULL && i < CFArrayGetCount(all); i++) {
		CFDictionaryRef d = CFArrayGetValueAtIndex(all, i);
		if (dict_int(d, kCGWindowLayer, -1) == 0 && dict_double(d, kCGWindowAlpha, 1) > 0) {
			wid = (CGWindowID)dict_int(d, kCGWindowNumber, 0);
			break;
		}
	}
	if (all) {
		CFRelease(all);
	}
	return wid;
}

static CGWindowID
focused_wid(struct macos_backend *m)
{
	return trusted() ? focused_wid_ax(m) : focused_wid_cg();
}

/*
 *
 * Window description.
 *
 */

static void
fill_window(struct macos_backend *m,
            CFDictionaryRef d,
            CGWindowID focused,
            const struct screen_info *scr,
            uint32_t scr_n,
            bool ax_ok,
            struct u_desktop_window *w)
{
	memset(w, 0, sizeof(*w));
	CGWindowID wid = (CGWindowID)dict_int(d, kCGWindowNumber, 0);
	pid_t pid = (pid_t)dict_int(d, kCGWindowOwnerPID, 0);
	CGRect fr = dict_bounds(d);
	w->id = wid;
	w->pid = (uint32_t)pid;
	w->frame = rect_from_cg(fr);

	char owner[U_DESKTOP_APP_ID_SIZE] = {0};
	cfstr_to_utf8(CFDictionaryGetValue(d, kCGWindowOwnerName), owner, sizeof(owner));
	app_identity(m, pid, owner, w->app_id, sizeof(w->app_id), NULL);

	cfstr_to_utf8(CFDictionaryGetValue(d, kCGWindowName), w->title, sizeof(w->title));

	// AX (cached by the refresher): title when Screen Recording hides it, and
	// the parent of a sheet.
	if (ax_ok) {
		char ax_title[U_DESKTOP_TITLE_SIZE] = {0};
		CGWindowID parent = 0;
		if (ax_cache_peek(m, pid, wid, &parent, ax_title, sizeof(ax_title))) {
			w->parent_id = parent;
			if (w->title[0] == '\0') {
				copy_utf8(w->title, sizeof(w->title), ax_title);
			}
		}
	}

	if (wid == focused) {
		w->state |= U_DESKTOP_WINDOW_FOCUSED;
	}
	if (pid == getpid()) {
		w->state |= U_DESKTOP_WINDOW_OWN;
	}
	int s = screen_for_rect(scr, scr_n, fr);
	if (s >= 0) {
		if (rect_near(fr, scr[s].bounds, 1.0)) {
			w->state |= U_DESKTOP_WINDOW_FULLSCREEN;
		} else if (rect_near(fr, scr[s].visible, 2.0)) {
			w->state |= U_DESKTOP_WINDOW_MAXIMIZED;
		}
	}
}

static bool
cg_window_listable(CFDictionaryRef d)
{
	CGRect r = dict_bounds(d);
	return dict_int(d, kCGWindowLayer, -1) == 0 && dict_double(d, kCGWindowAlpha, 1.0) > 0.0 &&
	       r.size.width > 0 && r.size.height > 0;
}

static bool
slow_skip(struct macos_backend *m, pid_t pid, uint64_t t)
{
	for (int i = 0; i < 16; i++) {
		if (m->slow[i].pid == pid && t < m->slow[i].until) {
			return true;
		}
	}
	return false;
}

static void
slow_mark(struct macos_backend *m, pid_t pid, uint64_t until)
{
	int slot = 0;
	for (int i = 0; i < 16; i++) {
		if (m->slow[i].pid == pid || m->slow[i].until < m->slow[slot].until) {
			slot = i;
			if (m->slow[i].pid == pid) {
				break;
			}
		}
	}
	m->slow[slot].pid = pid;
	m->slow[slot].until = until;
}

/*!
 * One refresh (refresher thread): rebuild the AX table of every app that owns a
 * normal-layer window, then derive the minimized list. Minimized windows are not
 * on screen, so candidates are off-screen layer-0 CG windows of regular apps; AX
 * says which are AXMinimized (the rest are on other Spaces or in hidden apps).
 */
static void
refresh_once(struct macos_backend *m)
{
	if (!trusted()) {
		return;
	}
	CFArrayRef all = CGWindowListCopyWindowInfo(kCGWindowListOptionAll | kCGWindowListExcludeDesktopElements,
	                                            kCGNullWindowID);
	if (all == NULL) {
		return;
	}
	pid_t pids[128];
	uint32_t np = 0;
	CFIndex cn = CFArrayGetCount(all);
	for (CFIndex i = 0; i < cn; i++) {
		CFDictionaryRef d = CFArrayGetValueAtIndex(all, i);
		CGRect r = dict_bounds(d);
		if (dict_int(d, kCGWindowLayer, -1) != 0 || r.size.width < 1 || r.size.height < 1) {
			continue;
		}
		CFBooleanRef on = CFDictionaryGetValue(d, kCGWindowIsOnscreen);
		pid_t pid = (pid_t)dict_int(d, kCGWindowOwnerPID, 0);
		if (on == NULL || !CFBooleanGetValue(on)) {
			bool regular = false;
			char id[U_DESKTOP_APP_ID_SIZE];
			app_identity(m, pid, NULL, id, sizeof(id), &regular);
			if (!regular || r.size.width < 40 || r.size.height < 40) {
				continue;
			}
		}
		bool seen = false;
		for (uint32_t k = 0; k < np; k++) {
			seen = seen || pids[k] == pid;
		}
		if (!seen && np < 128) {
			pids[np++] = pid;
		}
	}

	for (uint32_t k = 0; k < np && !m->stop; k++) {
		uint64_t t = now_ns();
		pthread_mutex_lock(&m->lock);
		bool skip = slow_skip(m, pids[k], t);
		pthread_mutex_unlock(&m->lock);
		if (skip) {
			continue;
		}
		struct ax_app_entry *e = calloc(1, sizeof(*e));
		if (e == NULL) {
			break;
		}
		ax_build_table(m, pids[k], AX_OBSERVE_TIMEOUT_S, e);
		// Sheets: only if one of this app's on-screen windows has no AX match.
		for (CFIndex i = 0; i < cn && !e->sheets_scanned; i++) {
			CFDictionaryRef d = CFArrayGetValueAtIndex(all, i);
			CFBooleanRef on = CFDictionaryGetValue(d, kCGWindowIsOnscreen);
			if ((pid_t)dict_int(d, kCGWindowOwnerPID, 0) == pids[k] && dict_int(d, kCGWindowLayer, -1) == 0 &&
			    on != NULL && CFBooleanGetValue(on) &&
			    !ax_entry_find(e, (CGWindowID)dict_int(d, kCGWindowNumber, 0), NULL, NULL)) {
				ax_scan_sheets(m, e);
			}
		}
		uint64_t took = now_ns() - t;
		if (took > 100ull * 1000 * 1000) { // timed out somewhere: back off 10 s
			pthread_mutex_lock(&m->lock);
			slow_mark(m, pids[k], now_ns() + 10ull * 1000 * 1000 * 1000);
			pthread_mutex_unlock(&m->lock);
		}
		ax_publish(m, e);
	}

	// Minimized list from the fresh tables.
	struct u_desktop_window *res = calloc(MINIMIZED_MAX, sizeof(*res));
	uint32_t n = 0;
	for (CFIndex i = 0; res != NULL && i < cn && n < MINIMIZED_MAX; i++) {
		CFDictionaryRef d = CFArrayGetValueAtIndex(all, i);
		CFBooleanRef on = CFDictionaryGetValue(d, kCGWindowIsOnscreen);
		CGRect r = dict_bounds(d);
		if (dict_int(d, kCGWindowLayer, -1) != 0 || (on != NULL && CFBooleanGetValue(on)) ||
		    r.size.width < 40 || r.size.height < 40) {
			continue;
		}
		pid_t pid = (pid_t)dict_int(d, kCGWindowOwnerPID, 0);
		CGWindowID wid = (CGWindowID)dict_int(d, kCGWindowNumber, 0);
		bool mini = false;
		char ax_title[U_DESKTOP_TITLE_SIZE] = {0};
		pthread_mutex_lock(&m->lock);
		for (int a = 0; a < AX_CACHE_APPS; a++) {
			if (m->ax[a].pid != pid) {
				continue;
			}
			for (uint32_t k = 0; k < m->ax[a].n; k++) {
				if (m->ax[a].w[k].wid == wid) {
					mini = m->ax[a].w[k].minimized;
					copy_utf8(ax_title, sizeof(ax_title), m->ax[a].w[k].title);
				}
			}
			break;
		}
		pthread_mutex_unlock(&m->lock);
		if (!mini) {
			continue;
		}
		struct u_desktop_window *w = &res[n++];
		memset(w, 0, sizeof(*w));
		w->id = wid;
		w->pid = (uint32_t)pid;
		char owner[U_DESKTOP_APP_ID_SIZE] = {0};
		cfstr_to_utf8(CFDictionaryGetValue(d, kCGWindowOwnerName), owner, sizeof(owner));
		app_identity(m, pid, owner, w->app_id, sizeof(w->app_id), NULL);
		if (!cfstr_to_utf8(CFDictionaryGetValue(d, kCGWindowName), w->title, sizeof(w->title))) {
			copy_utf8(w->title, sizeof(w->title), ax_title);
		}
		w->frame = rect_from_cg(r); // the frame it will come back to
		w->state = U_DESKTOP_WINDOW_MINIMIZED | (pid == getpid() ? U_DESKTOP_WINDOW_OWN : 0);
	}
	CFRelease(all);

	pthread_mutex_lock(&m->lock);
	if (res != NULL) {
		memcpy(m->minimized, res, sizeof(*res) * n);
		m->minimized_n = n;
	}
	m->minimized_at = now_ns();
	m->snapshot_gen++;
	pthread_cond_broadcast(&m->cond);
	pthread_mutex_unlock(&m->lock);
	free(res);
}

static void *
refresher_main(void *p)
{
	struct macos_backend *m = p;
	pthread_setname_np("dxr-desktop-ax");
	pthread_mutex_lock(&m->lock);
	while (!m->stop) {
		uint64_t t = now_ns();
		bool wanted = t - m->demand_at < 5000ull * 1000 * 1000;
		bool due = m->minimized_at == 0 || t - m->minimized_at >= AX_CACHE_TTL_NS;
		if (wanted && due) {
			pthread_mutex_unlock(&m->lock);
			@autoreleasepool {
				refresh_once(m);
			}
			pthread_mutex_lock(&m->lock);
			continue;
		}
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 250 * 1000 * 1000;
		if (ts.tv_nsec >= 1000000000) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000;
		}
		pthread_cond_timedwait(&m->cond, &m->lock, &ts);
	}
	pthread_mutex_unlock(&m->lock);
	return NULL;
}

/*!
 * Note demand for AX data and start the refresher on first use. The very first
 * call waits (at most 1.5 s) for the first snapshot so a one-shot caller sees
 * minimized windows and AX titles; later calls never wait.
 */
static void
refresher_touch(struct macos_backend *m)
{
	if (!trusted()) {
		return;
	}
	pthread_mutex_lock(&m->lock);
	m->demand_at = now_ns();
	if (!m->thread_started) {
		m->thread_started = pthread_create(&m->thread, NULL, refresher_main, m) == 0;
	}
	pthread_cond_broadcast(&m->cond);
	if (m->thread_started && m->snapshot_gen == 0) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += 1;
		ts.tv_nsec += 500 * 1000 * 1000;
		if (ts.tv_nsec >= 1000000000) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000;
		}
		while (m->snapshot_gen == 0 && !m->stop) {
			if (pthread_cond_timedwait(&m->cond, &m->lock, &ts) != 0) {
				break;
			}
		}
	}
	pthread_mutex_unlock(&m->lock);
}

static uint32_t
minimized_get(struct macos_backend *m, struct u_desktop_window *out, uint32_t cap)
{
	pthread_mutex_lock(&m->lock);
	uint32_t n = m->minimized_n < cap ? m->minimized_n : cap;
	memcpy(out, m->minimized, sizeof(*out) * n);
	pthread_mutex_unlock(&m->lock);
	return n;
}

static void
minimized_invalidate(struct macos_backend *m)
{
	pthread_mutex_lock(&m->lock);
	m->minimized_at = 0; // the refresher rebuilds on its next tick
	pthread_cond_broadcast(&m->cond);
	pthread_mutex_unlock(&m->lock);
}

/*
 *
 * Consent, lock, idle.
 *
 */

static enum u_desktop_access
mac_access_status(struct u_desktop_backend *b)
{
	(void)b;
	// Accessibility is the gate. Screen Recording only affects window titles.
	return trusted() ? U_DESKTOP_ACCESS_GRANTED : U_DESKTOP_ACCESS_DENIED;
}

struct access_wait
{
	u_desktop_access_done_fn done;
	void *ctx;
};

static void *
access_wait_thread(void *p)
{
	struct access_wait *w = p;
	enum u_desktop_access res = U_DESKTOP_ACCESS_DENIED;
	// TCC has no completion callback: poll. AXIsProcessTrusted reflects a grant
	// made in System Settings without restarting the process.
	for (int i = 0; i < 120 * 4; i++) {
		if (AXIsProcessTrusted()) {
			res = U_DESKTOP_ACCESS_GRANTED;
			break;
		}
		sleep_ms(250);
	}
	w->done(w->ctx, res);
	free(w);
	return NULL;
}

static void
mac_request_access(struct u_desktop_backend *b,
                   const struct u_desktop_app_info *app,
                   u_desktop_access_done_fn done,
                   void *ctx)
{
	(void)b;
	(void)app; // TCC names the responsible process itself; we cannot (and must not) relabel it.

	if (!CGPreflightScreenCaptureAccess()) {
		// Optional (titles only). Shows the system prompt once per responsible
		// process, then only System Settings can change the answer.
		CGRequestScreenCaptureAccess();
	}

	const void *keys[1] = {kAXTrustedCheckOptionPrompt};
	const void *vals[1] = {kCFBooleanTrue};
	CFDictionaryRef opts =
	    CFDictionaryCreate(NULL, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	bool now = AXIsProcessTrustedWithOptions(opts);
	CFRelease(opts);
	if (now) {
		done(ctx, U_DESKTOP_ACCESS_GRANTED);
		return;
	}

	struct access_wait *w = calloc(1, sizeof(*w));
	pthread_t th;
	if (w == NULL) {
		done(ctx, U_DESKTOP_ACCESS_DENIED);
		return;
	}
	w->done = done;
	w->ctx = ctx;
	if (pthread_create(&th, NULL, access_wait_thread, w) != 0) {
		free(w);
		done(ctx, U_DESKTOP_ACCESS_DENIED);
		return;
	}
	pthread_detach(th);
}

static bool
mac_is_locked(struct u_desktop_backend *b)
{
	(void)b;
	bool locked = false;
	CFDictionaryRef d = CGSessionCopyCurrentDictionary();
	if (d != NULL) {
		CFTypeRef v = CFDictionaryGetValue(d, CFSTR("CGSSessionScreenIsLocked"));
		if (v != NULL && CFGetTypeID(v) == CFBooleanGetTypeID()) {
			locked = CFBooleanGetValue(v);
		} else if (v != NULL && CFGetTypeID(v) == CFNumberGetTypeID()) {
			int iv = 0;
			CFNumberGetValue(v, kCFNumberIntType, &iv);
			locked = iv != 0;
		}
		// Fast user switching: the session is not the console one.
		CFTypeRef oc = CFDictionaryGetValue(d, kCGSessionOnConsoleKey);
		if (oc != NULL && CFGetTypeID(oc) == CFBooleanGetTypeID() && !CFBooleanGetValue(oc)) {
			locked = true;
		}
		CFRelease(d);
	}
	return locked;
}

static bool
mac_user_idle_ms(struct u_desktop_backend *b, uint64_t *out_ms)
{
	(void)b;
	double s = CGEventSourceSecondsSinceLastEventType(kCGEventSourceStateCombinedSessionState, kCGAnyInputEventType);
	if (!(s >= 0)) {
		return false;
	}
	*out_ms = (uint64_t)(s * 1000.0);
	return true;
}

/*
 *
 * Observation.
 *
 */

static enum u_desktop_result
mac_list_windows(struct u_desktop_backend *b, struct u_desktop_window *out, uint32_t cap, uint32_t *count)
{
	struct macos_backend *m = mb(b);
	*count = 0;
	refresher_touch(m);
	bool ax_ok = trusted();
	CGWindowID focused = focused_wid(m);
	struct screen_info scr[SCREENS_MAX];
	uint32_t scr_n = screens_get(m, scr);

	CFArrayRef all = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
	                                            kCGNullWindowID);
	if (all == NULL) {
		return U_DESKTOP_ERR_FAILED;
	}
	uint32_t total = 0;
	for (CFIndex i = 0; i < CFArrayGetCount(all); i++) {
		CFDictionaryRef d = CFArrayGetValueAtIndex(all, i);
		if (!cg_window_listable(d)) {
			continue;
		}
		if (total < cap) {
			fill_window(m, d, focused, scr, scr_n, ax_ok, &out[total]);
			out[total].z = total;
		}
		total++;
	}
	CFRelease(all);

	if (ax_ok) {
		struct u_desktop_window mins[MINIMIZED_MAX];
		uint32_t mn = minimized_get(m, mins, MINIMIZED_MAX);
		for (uint32_t i = 0; i < mn; i++) {
			if (total < cap) {
				out[total] = mins[i];
				out[total].z = total;
			}
			total++;
		}
	}
	*count = total;
	return U_DESKTOP_OK;
}

static enum u_desktop_result
mac_get_pointer(struct u_desktop_backend *b, int32_t *x, int32_t *y)
{
	(void)b;
	CGEventRef e = CGEventCreate(NULL);
	if (e == NULL) {
		return U_DESKTOP_ERR_FAILED;
	}
	CGPoint p = CGEventGetLocation(e);
	CFRelease(e);
	*x = (int32_t)floor(p.x);
	*y = (int32_t)floor(p.y);
	return U_DESKTOP_OK;
}

//! On the main thread, is our own window @p wid click-through (ignoresMouseEvents)?
static bool
own_window_ignores_mouse(CGWindowID wid)
{
	if (!is_main_thread()) {
		return false;
	}
	bool ignores = false;
	@autoreleasepool {
		if (NSApp != nil) {
			NSWindow *w = [NSApp windowWithWindowNumber:(NSInteger)wid];
			ignores = w != nil && w.ignoresMouseEvents;
		}
	}
	return ignores;
}

static bool
is_shell_bundle(const char *id)
{
	static const char *shell[] = {
	    "com.apple.dock",          "com.apple.controlcenter", "com.apple.systemuiserver",
	    "com.apple.notificationcenterui", "com.apple.WindowManager", "com.apple.Spotlight",
	    "com.apple.loginwindow",
	};
	for (size_t i = 0; i < sizeof(shell) / sizeof(shell[0]); i++) {
		if (strcmp(id, shell[i]) == 0) {
			return true;
		}
	}
	return false;
}

enum ax_pick
{
	AXP_UNKNOWN,
	AXP_WINDOW,
	AXP_SHELL,
	AXP_DESKTOP,
};

/*!
 * AX hit test: unlike CGWindowList it honours click-through windows (the window
 * server routes the hit to whatever accepts mouse events there).
 */
static enum ax_pick
ax_pick(struct macos_backend *m, int32_t x, int32_t y, CGWindowID *out_wid)
{
	*out_wid = 0;
	AXUIElementRef sys = AXUIElementCreateSystemWide();
	AXUIElementSetMessagingTimeout(sys, AX_OBSERVE_TIMEOUT_S);
	AXUIElementRef el = NULL;
	AXError e = AXUIElementCopyElementAtPosition(sys, (float)x, (float)y, &el);
	CFRelease(sys);
	if (e != kAXErrorSuccess || el == NULL) {
		return AXP_UNKNOWN;
	}
	enum ax_pick res = AXP_UNKNOWN;
	pid_t pid = 0;
	AXUIElementGetPid(el, &pid);
	char app_id[U_DESKTOP_APP_ID_SIZE] = {0};
	app_identity(m, pid, NULL, app_id, sizeof(app_id), NULL);
	CFTypeRef role = ax_copy(el, kAXRoleAttribute);
	bool menu = role != NULL && CFGetTypeID(role) == CFStringGetTypeID() &&
	            (CFEqual(role, kAXMenuBarRole) || CFEqual(role, kAXMenuBarItemRole) || CFEqual(role, kAXMenuRole) ||
	             CFEqual(role, kAXMenuItemRole));
	bool is_window = role != NULL && CFGetTypeID(role) == CFStringGetTypeID() && CFEqual(role, kAXWindowRole);
	if (role) {
		CFRelease(role);
	}

	if (menu || is_shell_bundle(app_id)) {
		res = AXP_SHELL;
	} else {
		AXUIElementRef win = is_window ? (AXUIElementRef)CFRetain(el) : (AXUIElementRef)ax_copy(el, kAXWindowAttribute);
		if (win != NULL) {
			*out_wid = ax_element_window_id(m, win);
			CFRelease(win);
		}
		if (*out_wid != 0) {
			res = AXP_WINDOW;
		} else if (strcmp(app_id, "com.apple.finder") == 0) {
			// Finder element with no real window = the desktop (its AXWindow is
			// the desktop AXScrollArea, which has no CGWindowID; verified 26.3).
			res = AXP_DESKTOP;
		}
	}
	CFRelease(el);
	return res;
}

static enum u_desktop_result
mac_pick(struct u_desktop_backend *b, int32_t x, int32_t y, struct u_desktop_pick *out)
{
	struct macos_backend *m = mb(b);
	memset(out, 0, sizeof(*out));
	out->x = x;
	out->y = y;
	if (!point_on_any_display(x, y)) {
		out->kind = U_DESKTOP_PICK_OUTSIDE;
		return U_DESKTOP_OK;
	}
	CGPoint pt = CGPointMake(x + 0.5, y + 0.5);
	refresher_touch(m);
	struct screen_info scr[SCREENS_MAX];
	uint32_t scr_n = screens_get(m, scr);

	CFArrayRef all = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
	if (all == NULL) {
		return U_DESKTOP_ERR_FAILED;
	}
	CFIndex hit = -1;
	bool own_hit = false;
	out->kind = U_DESKTOP_PICK_DESKTOP;
	for (CFIndex i = 0; i < CFArrayGetCount(all); i++) {
		CFDictionaryRef d = CFArrayGetValueAtIndex(all, i);
		CGRect r = dict_bounds(d);
		if (dict_double(d, kCGWindowAlpha, 1.0) <= 0.0 || !CGRectContainsPoint(r, pt)) {
			continue;
		}
		int64_t layer = dict_int(d, kCGWindowLayer, 0);
		if (layer < 0) {
			break; // desktop elements: wallpaper, Finder desktop icons
		}
		if (layer > 0) {
			// A shell surface that spans a whole display (the Dock's overlay) is
			// click-through except where it draws; resolved below.
			bool full = false;
			for (uint32_t s = 0; s < scr_n; s++) {
				full = full || rect_near(r, scr[s].bounds, 1.0);
			}
			if (full) {
				continue;
			}
			out->kind = U_DESKTOP_PICK_SHELL;
			break;
		}
		CGWindowID wid = (CGWindowID)dict_int(d, kCGWindowNumber, 0);
		if ((pid_t)dict_int(d, kCGWindowOwnerPID, 0) == getpid()) {
			if (own_window_ignores_mouse(wid)) {
				continue;
			}
			own_hit = true;
		}
		hit = i;
		out->kind = U_DESKTOP_PICK_WINDOW;
		break;
	}
	if (out->kind == U_DESKTOP_PICK_DESKTOP) {
		// Not under any window: outside every visible frame = menu bar / Dock strip.
		bool in_visible = false;
		for (uint32_t s = 0; s < scr_n; s++) {
			in_visible = in_visible || CGRectContainsPoint(scr[s].visible, pt);
		}
		if (scr_n > 0 && !in_visible) {
			out->kind = U_DESKTOP_PICK_SHELL;
		}
	}

	// Cross-check with AX (click-through overlays of other processes, the Dock's
	// icons over a window). Skipped when the CG hit is our own window on the main
	// thread: there AppKit already answered and AX to ourselves would stall.
	CGWindowID ax_wid = 0;
	enum ax_pick axp = AXP_UNKNOWN;
	if (trusted() && !(own_hit && is_main_thread())) {
		axp = ax_pick(m, x, y, &ax_wid);
	}
	CGWindowID focused = 0;
	if (axp == AXP_WINDOW) {
		CFIndex ax_idx = -1;
		bool ax_desktop = false;
		for (CFIndex i = 0; i < CFArrayGetCount(all); i++) {
			CFDictionaryRef d = CFArrayGetValueAtIndex(all, i);
			if ((CGWindowID)dict_int(d, kCGWindowNumber, 0) == ax_wid) {
				ax_idx = cg_window_listable(d) && CGRectContainsPoint(dict_bounds(d), pt) ? i : -1;
				ax_desktop = dict_int(d, kCGWindowLayer, 0) < 0; // Finder's desktop window
				break;
			}
		}
		if (ax_desktop) {
			out->kind = U_DESKTOP_PICK_DESKTOP;
			hit = -1;
		} else if (ax_idx >= 0) {
			hit = ax_idx;
			out->kind = U_DESKTOP_PICK_WINDOW;
		}
	} else if (axp == AXP_SHELL) {
		out->kind = U_DESKTOP_PICK_SHELL;
	} else if (axp == AXP_DESKTOP) {
		out->kind = U_DESKTOP_PICK_DESKTOP;
	}

	if (out->kind == U_DESKTOP_PICK_WINDOW && hit >= 0) {
		focused = focused_wid(m);
		fill_window(m, CFArrayGetValueAtIndex(all, hit), focused, scr, scr_n, trusted(), &out->window);
	}
	CFRelease(all);
	return U_DESKTOP_OK;
}

static enum u_desktop_result
mac_focused_window(struct u_desktop_backend *b, uint64_t *out_id)
{
	*out_id = focused_wid(mb(b));
	return U_DESKTOP_OK;
}

static enum u_desktop_result
mac_text_at(struct u_desktop_backend *b, int32_t x, int32_t y, char *buf, size_t cap, char *msg)
{
	(void)b;
	if (cap > 0) {
		buf[0] = '\0';
	}
	if (!trusted()) {
		set_msg(msg, "Accessibility permission not granted");
		return U_DESKTOP_ERR_NOT_GRANTED;
	}
	if (is_main_thread()) {
		// An element of our own process would be answered by this very thread.
		CFArrayRef all = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
		CGPoint pt = CGPointMake(x + 0.5, y + 0.5);
		bool own = false;
		for (CFIndex i = 0; all != NULL && i < CFArrayGetCount(all); i++) {
			CFDictionaryRef d = CFArrayGetValueAtIndex(all, i);
			if (dict_double(d, kCGWindowAlpha, 1) <= 0 || !CGRectContainsPoint(dict_bounds(d), pt)) {
				continue;
			}
			if (dict_int(d, kCGWindowLayer, 0) >= kCGDockWindowLevel) {
				continue; // shell overlays
			}
			CGWindowID wid = (CGWindowID)dict_int(d, kCGWindowNumber, 0);
			if ((pid_t)dict_int(d, kCGWindowOwnerPID, 0) == getpid() && own_window_ignores_mouse(wid)) {
				continue;
			}
			own = (pid_t)dict_int(d, kCGWindowOwnerPID, 0) == getpid();
			break;
		}
		if (all) {
			CFRelease(all);
		}
		if (own) {
			set_msg(msg, "own window: read it from the app, not through AX on the main thread");
			return U_DESKTOP_OK;
		}
	}

	AXUIElementRef sys = AXUIElementCreateSystemWide();
	AXUIElementSetMessagingTimeout(sys, AX_OBSERVE_TIMEOUT_S);
	AXUIElementRef el = NULL;
	AXError e = AXUIElementCopyElementAtPosition(sys, (float)x, (float)y, &el);
	CFRelease(sys);
	if (e == kAXErrorNoValue || el == NULL) {
		return U_DESKTOP_OK; // nothing there
	}
	if (e == kAXErrorAPIDisabled) {
		set_msg(msg, "Accessibility permission not granted");
		return U_DESKTOP_ERR_NOT_GRANTED;
	}
	if (e == kAXErrorCannotComplete) {
		set_msg(msg, "the app under the point did not answer");
		if (el) {
			CFRelease(el);
		}
		return U_DESKTOP_ERR_TIMEOUT;
	}
	if (e != kAXErrorSuccess) {
		set_msg(msg, "AXUIElementCopyElementAtPosition failed (%d)", (int)e);
		if (el) {
			CFRelease(el);
		}
		return U_DESKTOP_ERR_FAILED;
	}
	AXUIElementSetMessagingTimeout(el, AX_OBSERVE_TIMEOUT_S);
	static CFStringRef const attrs[] = {kAXValueAttribute, kAXTitleAttribute, kAXDescriptionAttribute};
	for (size_t i = 0; i < 3; i++) {
		CFTypeRef v = ax_copy(el, attrs[i]);
		bool got = cfstr_to_utf8(v, buf, cap);
		if (v) {
			CFRelease(v);
		}
		if (got) {
			break;
		}
	}
	CFRelease(el);
	return U_DESKTOP_OK;
}

/*
 *
 * Window actions (worker thread).
 *
 */

static void
saved_put(struct macos_backend *m, CGWindowID wid, CGRect fr)
{
	pthread_mutex_lock(&m->lock);
	uint32_t slot = m->saved_next;
	for (uint32_t i = 0; i < SAVED_FRAMES; i++) {
		if (m->saved[i].wid == wid) {
			slot = i;
			break;
		}
	}
	if (slot == m->saved_next) {
		m->saved_next = (m->saved_next + 1) % SAVED_FRAMES;
	}
	m->saved[slot].wid = wid;
	m->saved[slot].frame = fr;
	pthread_mutex_unlock(&m->lock);
}

static bool
saved_take(struct macos_backend *m, CGWindowID wid, CGRect *out)
{
	bool found = false;
	pthread_mutex_lock(&m->lock);
	for (uint32_t i = 0; i < SAVED_FRAMES; i++) {
		if (m->saved[i].wid == wid && wid != 0) {
			*out = m->saved[i].frame;
			m->saved[i].wid = 0;
			found = true;
			break;
		}
	}
	pthread_mutex_unlock(&m->lock);
	return found;
}

static bool
wait_bool(AXUIElementRef el, CFStringRef attr, bool want, unsigned timeout_ms)
{
	for (unsigned t = 0; t <= timeout_ms; t += 25) {
		bool v = !want;
		if (ax_get_bool(el, attr, &v) && v == want) {
			return true;
		}
		sleep_ms(25);
	}
	return false;
}

/*!
 * AX answers as soon as the app applied the frame; CGWindowList (what pick and
 * list read) follows a few frames later. Wait for it (best effort, <= 300 ms) so
 * the common layer's next verification sees the new geometry.
 */
static void
wait_cg_frame(CGWindowID wid, CGRect want, bool keep_size)
{
	for (int i = 0; i < 30; i++) {
		CFDictionaryRef d = cg_window_info(wid);
		if (d == NULL) {
			return;
		}
		CGRect r = dict_bounds(d);
		CFRelease(d);
		if (fabs(r.origin.x - want.origin.x) <= 2 && fabs(r.origin.y - want.origin.y) <= 2 &&
		    (keep_size || (fabs(r.size.width - want.size.width) <= 2 && fabs(r.size.height - want.size.height) <= 2))) {
			return;
		}
		sleep_ms(10);
	}
}

//! Position, then size, then position again (a resize across a screen edge can shift the origin).
static enum u_desktop_result
set_frame(AXUIElementRef el, CGWindowID wid, CGRect want, bool keep_size, char *msg)
{
	if (!ax_set_point(el, kAXPositionAttribute, want.origin)) {
		set_msg(msg, "the app refused AXPosition");
		return U_DESKTOP_ERR_REFUSED;
	}
	if (!keep_size) {
		if (!ax_set_size(el, want.size)) {
			set_msg(msg, "the app refused AXSize");
			return U_DESKTOP_ERR_REFUSED;
		}
		ax_set_point(el, kAXPositionAttribute, want.origin);
	}
	CGRect got;
	for (int i = 0; i < 20; i++) { // read back, up to ~500 ms for apps that animate
		if (ax_get_frame(el, &got) && fabs(got.origin.x - want.origin.x) <= 2 &&
		    fabs(got.origin.y - want.origin.y) <= 2 &&
		    (keep_size ||
		     (fabs(got.size.width - want.size.width) <= 2 && fabs(got.size.height - want.size.height) <= 2))) {
			wait_cg_frame(wid, got, keep_size);
			return U_DESKTOP_OK;
		}
		sleep_ms(25);
	}
	if (ax_get_frame(el, &got)) {
		set_msg(msg, "the window settled at (%d,%d %dx%d)", iround(got.origin.x), iround(got.origin.y),
		        iround(got.size.width), iround(got.size.height));
	} else {
		set_msg(msg, "could not read the frame back");
	}
	return U_DESKTOP_ERR_REFUSED;
}

static enum u_desktop_result
ensure_unminimized(AXUIElementRef el, char *msg)
{
	bool mini = false;
	if (ax_get_bool(el, kAXMinimizedAttribute, &mini) && mini) {
		if (!ax_set_bool(el, kAXMinimizedAttribute, false) || !wait_bool(el, kAXMinimizedAttribute, false, 1000)) {
			set_msg(msg, "could not unminimize the window");
			return U_DESKTOP_ERR_REFUSED;
		}
	}
	return U_DESKTOP_OK;
}

static enum u_desktop_result
ensure_not_fullscreen(AXUIElementRef el, char *msg)
{
	bool fs = false;
	if (ax_get_bool(el, CFSTR("AXFullScreen"), &fs) && fs) {
		if (!ax_set_bool(el, CFSTR("AXFullScreen"), false) || !wait_bool(el, CFSTR("AXFullScreen"), false, 2000)) {
			set_msg(msg, "could not leave full screen");
			return U_DESKTOP_ERR_REFUSED;
		}
		sleep_ms(300); // the Space transition outlives the attribute flip
	}
	return U_DESKTOP_OK;
}

static enum u_desktop_result
mac_window_op(struct u_desktop_backend *b,
              enum u_desktop_window_op op,
              uint64_t window_id,
              const struct u_desktop_rect *rect,
              char *msg)
{
	struct macos_backend *m = mb(b);
	if (!trusted()) {
		set_msg(msg, "Accessibility permission not granted");
		return U_DESKTOP_ERR_NOT_GRANTED;
	}
	CGWindowID wid = (CGWindowID)window_id;
	CFDictionaryRef info = window_id != 0 && window_id <= UINT32_MAX ? cg_window_info(wid) : NULL;
	if (info == NULL) {
		set_msg(msg, "window %llu no longer exists", (unsigned long long)window_id);
		return U_DESKTOP_ERR_WINDOW_LOST;
	}
	pid_t pid = (pid_t)dict_int(info, kCGWindowOwnerPID, 0);
	CFRelease(info);

	AXUIElementRef el = NULL;
	if (!ax_window_for(m, pid, wid, AX_ACTION_TIMEOUT_S, true, &el, NULL)) {
		set_msg(msg, "window not reachable through Accessibility (another Space, or the app has no AX)");
		return U_DESKTOP_ERR_FAILED;
	}
	AXUIElementSetMessagingTimeout(el, AX_ACTION_TIMEOUT_S);
	minimized_invalidate(m);

	enum u_desktop_result r = U_DESKTOP_OK;
	switch (op) {
	case U_DESKTOP_OP_ACTIVATE: {
		r = ensure_unminimized(el, msg);
		if (r != U_DESKTOP_OK) {
			break;
		}
		@autoreleasepool {
			NSRunningApplication *app = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
			[app activateWithOptions:NSApplicationActivateIgnoringOtherApps];
#pragma clang diagnostic pop
		}
		// macOS 14+ cooperative activation may ignore the request above from a
		// background app; AXFrontmost (Accessibility-granted) is not subject to it.
		AXUIElementRef ax_app = ax_app_create(pid, AX_ACTION_TIMEOUT_S);
		ax_set_bool(ax_app, kAXFrontmostAttribute, true);
		CFRelease(ax_app);
		AXUIElementPerformAction(el, kAXRaiseAction);
		ax_set_bool(el, kAXMainAttribute, true);
		r = U_DESKTOP_ERR_REFUSED;
		for (int i = 0; i < 20; i++) {
			if (focused_wid_ax(m) == wid) {
				r = U_DESKTOP_OK;
				break;
			}
			sleep_ms(25);
		}
		if (r != U_DESKTOP_OK) {
			set_msg(msg, "the window did not take focus within 500 ms");
		}
		break;
	}
	case U_DESKTOP_OP_MOVE_RESIZE: {
		bool keep = rect == NULL || rect->w <= 0 || rect->h <= 0;
		CGRect want = CGRectMake(rect ? rect->x : 0, rect ? rect->y : 0, keep ? 0 : rect->w, keep ? 0 : rect->h);
		if (rect == NULL) {
			set_msg(msg, "no rect");
			r = U_DESKTOP_ERR_FAILED;
			break;
		}
		r = set_frame(el, wid, want, keep, msg);
		break;
	}
	case U_DESKTOP_OP_MINIMIZE:
		if (!ax_set_bool(el, kAXMinimizedAttribute, true)) {
			set_msg(msg, "the app refused AXMinimized");
			r = U_DESKTOP_ERR_REFUSED;
		} else if (!wait_bool(el, kAXMinimizedAttribute, true, 1000)) {
			set_msg(msg, "the window did not minimize");
			r = U_DESKTOP_ERR_REFUSED;
		}
		break;
	case U_DESKTOP_OP_RESTORE: {
		r = ensure_unminimized(el, msg);
		if (r == U_DESKTOP_OK) {
			r = ensure_not_fullscreen(el, msg);
		}
		CGRect prev;
		if (r == U_DESKTOP_OK && saved_take(m, wid, &prev)) {
			r = set_frame(el, wid, prev, false, msg);
		}
		break;
	}
	case U_DESKTOP_OP_MAXIMIZE: {
		r = ensure_unminimized(el, msg);
		if (r == U_DESKTOP_OK) {
			r = ensure_not_fullscreen(el, msg);
		}
		if (r != U_DESKTOP_OK) {
			break;
		}
		CGRect cur;
		if (!ax_get_frame(el, &cur)) {
			set_msg(msg, "could not read the window frame");
			r = U_DESKTOP_ERR_FAILED;
			break;
		}
		struct screen_info scr[SCREENS_MAX];
		uint32_t n = screens_compute(scr); // fresh: the Dock may have moved
		int s = screen_for_rect(scr, n, cur);
		if (s < 0) {
			set_msg(msg, "the window is on no display");
			r = U_DESKTOP_ERR_FAILED;
			break;
		}
		if (!rect_near(cur, scr[s].visible, 2.0)) {
			saved_put(m, wid, cur);
		}
		r = set_frame(el, wid, scr[s].visible, false, msg);
		break;
	}
	case U_DESKTOP_OP_CLOSE: {
		AXUIElementRef btn = (AXUIElementRef)ax_copy(el, kAXCloseButtonAttribute);
		if (btn == NULL) {
			set_msg(msg, "the window has no close button");
			r = U_DESKTOP_ERR_REFUSED;
			break;
		}
		AXError e = AXUIElementPerformAction(btn, kAXPressAction);
		CFRelease(btn);
		if (e != kAXErrorSuccess) {
			set_msg(msg, "pressing the close button failed (%d)", (int)e);
			r = U_DESKTOP_ERR_REFUSED;
		}
		break;
	}
	default: set_msg(msg, "unknown window op %d", (int)op); r = U_DESKTOP_ERR_UNSUPPORTED;
	}
	CFRelease(el);
	return r;
}

/*
 *
 * Input (worker thread).
 *
 */

#define KVK_RETURN 0x24
#define KVK_TAB 0x30
#define KVK_COMMAND 0x37
#define KVK_SHIFT 0x38
#define KVK_OPTION 0x3A
#define KVK_CONTROL 0x3B

struct key_map
{
	const char *name;
	uint16_t code;
	bool fn; //!< hardware sets kCGEventFlagMaskSecondaryFn on these
	bool np; //!< ... and kCGEventFlagMaskNumericPad (arrows)
};

// Named keys: layout-independent kVK_* codes (HIToolbox/Events.h).
static const struct key_map g_keys[] = {
    {"enter", 0x24, 0, 0},    {"tab", 0x30, 0, 0},      {"space", 0x31, 0, 0},     {"backspace", 0x33, 0, 0},
    {"escape", 0x35, 0, 0},   {"capslock", 0x39, 0, 0}, {"menu", 0x6E, 0, 0},      {"insert", 0x72, 1, 0}, // kVK_Help
    {"home", 0x73, 1, 0},     {"pageup", 0x74, 1, 0},   {"delete", 0x75, 1, 0},    {"end", 0x77, 1, 0},
    {"pagedown", 0x79, 1, 0}, {"left", 0x7B, 1, 1},     {"right", 0x7C, 1, 1},     {"down", 0x7D, 1, 1},
    {"up", 0x7E, 1, 1},       {"f1", 0x7A, 1, 0},       {"f2", 0x78, 1, 0},        {"f3", 0x63, 1, 0},
    {"f4", 0x76, 1, 0},       {"f5", 0x60, 1, 0},       {"f6", 0x61, 1, 0},        {"f7", 0x62, 1, 0},
    {"f8", 0x64, 1, 0},       {"f9", 0x65, 1, 0},       {"f10", 0x6D, 1, 0},       {"f11", 0x67, 1, 0},
    {"f12", 0x6F, 1, 0},      {"f13", 0x69, 1, 0},      {"f14", 0x6B, 1, 0},       {"f15", 0x71, 1, 0},
    {"f16", 0x6A, 1, 0},      {"f17", 0x40, 1, 0},      {"f18", 0x4F, 1, 0},       {"f19", 0x50, 1, 0},
    {"f20", 0x5A, 1, 0},
};

//! Character keys: the chord names a CHARACTER on the active layout (u_desktop.h).
static uint16_t
chord_char(const char *key)
{
	if (key[0] != '\0' && key[1] == '\0' &&
	    ((key[0] >= 'a' && key[0] <= 'z') || (key[0] >= '0' && key[0] <= '9'))) {
		return (uint16_t)key[0];
	}
	static const struct
	{
		const char *name;
		char c;
	} punct[] = {
	    {"minus", '-'}, {"equal", '='},     {"comma", ','},       {"period", '.'},        {"slash", '/'},
	    {"backslash", '\\'}, {"semicolon", ';'}, {"quote", '\''}, {"bracketleft", '['}, {"bracketright", ']'},
	    {"grave", '`'},
	};
	for (size_t i = 0; i < sizeof(punct) / sizeof(punct[0]); i++) {
		if (strcmp(key, punct[i].name) == 0) {
			return (uint16_t)punct[i].c;
		}
	}
	return 0;
}

/*
 * Character -> kVK through the ACTIVE keyboard layout (UCKeyTranslate over the
 * current input source's 'uchr' data), looked up per chord (256 UCKeyTranslate
 * calls, microseconds; no cache to go stale when the user switches layout). Text Input
 * Sources is main-thread API in Apple's book: the lookup is dispatched to the
 * main queue when we are not on it, with a bounded wait; a process whose main
 * queue is not serviced falls back to calling it on this thread (works in
 * practice on macOS 14-26, verified on 26.3).
 */
struct layout_lookup
{
	UniChar ch;
	bool found;
	uint16_t code;
	bool shift;
	char source_id[128];
	char err[96];
	dispatch_semaphore_t done;
	int refs; // atomically decremented: whoever reaches 0 frees
};

static void
layout_lookup_run(struct layout_lookup *l)
{
	TISInputSourceRef src = TISCopyCurrentKeyboardLayoutInputSource();
	if (src == NULL) {
		snprintf(l->err, sizeof(l->err), "no current keyboard layout");
		return;
	}
	CFStringRef sid = TISGetInputSourceProperty(src, kTISPropertyInputSourceID);
	if (sid != NULL) {
		CFStringGetCString(sid, l->source_id, sizeof(l->source_id), kCFStringEncodingUTF8);
	}
	CFDataRef data = TISGetInputSourceProperty(src, kTISPropertyUnicodeKeyLayoutData);
	if (data == NULL) {
		snprintf(l->err, sizeof(l->err), "layout %s has no Unicode key layout data", l->source_id);
		CFRelease(src);
		return;
	}
	const UCKeyboardLayout *kl = (const UCKeyboardLayout *)CFDataGetBytePtr(data);
	// Unshifted first (so "1" on AZERTY only matches with Shift if nothing else gives it).
	for (int pass = 0; pass < 2 && !l->found; pass++) {
		UInt32 mods = pass == 0 ? 0 : ((shiftKey >> 8) & 0xFF);
		for (uint16_t code = 0; code < 128 && !l->found; code++) {
			UInt32 dead = 0;
			UniChar out[4];
			UniCharCount n = 0;
			if (UCKeyTranslate(kl, code, kUCKeyActionDown, mods, LMGetKbdType(), kUCKeyTranslateNoDeadKeysBit,
			                   &dead, 4, &n, out) == noErr &&
			    n == 1 && out[0] == l->ch) {
				l->found = true;
				l->code = code;
				l->shift = pass == 1;
			}
		}
	}
	CFRelease(src);
}

static void
layout_lookup_unref(struct layout_lookup *l)
{
	if (__atomic_sub_fetch(&l->refs, 1, __ATOMIC_ACQ_REL) == 0) {
		if (l->done != NULL) {
			dispatch_release(l->done);
		}
		free(l);
	}
}

static bool
layout_char_to_key(struct macos_backend *m, UniChar ch, uint16_t *code, bool *shift, char *msg)
{
	struct layout_lookup *l = calloc(1, sizeof(*l));
	if (l == NULL) {
		return false;
	}
	l->ch = ch;
	l->refs = 1;
	if (is_main_thread()) {
		layout_lookup_run(l);
	} else {
		l->done = dispatch_semaphore_create(0);
		l->refs = 2;
		dispatch_async(dispatch_get_main_queue(), ^{
		  layout_lookup_run(l);
		  dispatch_semaphore_signal(l->done);
		  layout_lookup_unref(l);
		});
		if (dispatch_semaphore_wait(l->done, dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC)) != 0) {
			// Main queue not serviced: do it here on a private copy.
			layout_lookup_unref(l);
			l = calloc(1, sizeof(*l));
			if (l == NULL) {
				return false;
			}
			l->ch = ch;
			l->refs = 1;
			layout_lookup_run(l);
		}
	}
	(void)m;
	bool ok = l->found;
	if (ok) {
		*code = l->code;
		*shift = l->shift;
	} else if (l->err[0]) {
		set_msg(msg, "%s", l->err);
	} else {
		set_msg(msg, "no key on the active layout (%s) types '%c'", l->source_id, (char)ch);
	}
	layout_lookup_unref(l);
	return ok;
}

static bool
post_key(struct macos_backend *m, uint16_t code, bool down, CGEventFlags flags)
{
	CGEventRef e = CGEventCreateKeyboardEvent(m->src, code, down);
	if (e == NULL) {
		return false;
	}
	CGEventSetFlags(e, flags);
	CGEventPost(kCGHIDEventTap, e);
	CFRelease(e);
	return true;
}

static bool
post_unicode(struct macos_backend *m, const UniChar *s, CFIndex n)
{
	for (int down = 1; down >= 0; down--) {
		CGEventRef e = CGEventCreateKeyboardEvent(m->src, 0, down != 0);
		if (e == NULL) {
			return false;
		}
		CGEventSetFlags(e, 0); // a held modifier must not turn text into a shortcut
		CGEventKeyboardSetUnicodeString(e, (UniCharCount)n, s);
		CGEventPost(kCGHIDEventTap, e);
		CFRelease(e);
	}
	return true;
}

static enum u_desktop_result
mac_type_text(struct u_desktop_backend *b, const char *utf8, char *msg)
{
	struct macos_backend *m = mb(b);
	if (!trusted()) {
		set_msg(msg, "Accessibility permission not granted");
		return U_DESKTOP_ERR_NOT_GRANTED;
	}
	CFStringRef s = CFStringCreateWithBytes(NULL, (const UInt8 *)utf8, (CFIndex)strlen(utf8),
	                                        kCFStringEncodingUTF8, false);
	if (s == NULL) {
		set_msg(msg, "text is not valid UTF-8");
		return U_DESKTOP_ERR_FAILED;
	}
	CFIndex len = CFStringGetLength(s);
	UniChar *u = malloc(sizeof(UniChar) * (size_t)(len > 0 ? len : 1));
	if (u == NULL) {
		CFRelease(s);
		return U_DESKTOP_ERR_FAILED;
	}
	CFStringGetCharacters(s, CFRangeMake(0, len), u);
	CFRelease(s);

	bool ok = true;
	CFIndex i = 0;
	while (ok && i < len) {
		// Line breaks and tabs as their keys: a unicode "\n" is not Return to most apps.
		if (u[i] == '\n' || u[i] == '\r' || u[i] == '\t') {
			uint16_t code = u[i] == '\t' ? KVK_TAB : KVK_RETURN;
			if (u[i] == '\r' && i + 1 < len && u[i + 1] == '\n') {
				i++;
			}
			ok = post_key(m, code, true, 0) && post_key(m, code, false, 0);
			i++;
			sleep_ms(4);
			continue;
		}
		CFIndex n = 0;
		while (i + n < len && n < 20 && u[i + n] != '\n' && u[i + n] != '\r' && u[i + n] != '\t') {
			n++;
		}
		// Never split a surrogate pair across two events.
		if (i + n < len && n > 1 && CFStringIsSurrogateHighCharacter(u[i + n - 1])) {
			n--;
		}
		ok = post_unicode(m, &u[i], n);
		i += n;
		sleep_ms(4);
	}
	free(u);
	if (!ok) {
		set_msg(msg, "CGEvent creation failed");
		return U_DESKTOP_ERR_FAILED;
	}
	return U_DESKTOP_OK;
}

static enum u_desktop_result
mac_key_chord(struct u_desktop_backend *b, const struct u_desktop_chord *chord, char *msg)
{
	struct macos_backend *m = mb(b);
	if (!trusted()) {
		set_msg(msg, "Accessibility permission not granted");
		return U_DESKTOP_ERR_NOT_GRANTED;
	}
	const struct key_map *key = NULL;
	struct key_map char_key = {NULL, 0, 0, 0};
	uint32_t mods_eff = chord->mods;
	if (chord->key[0] != '\0') {
		for (size_t i = 0; i < sizeof(g_keys) / sizeof(g_keys[0]); i++) {
			if (strcmp(g_keys[i].name, chord->key) == 0) {
				key = &g_keys[i];
				break;
			}
		}
		UniChar ch = key == NULL ? chord_char(chord->key) : 0;
		if (key == NULL && ch != 0) {
			bool shift = false;
			if (!layout_char_to_key(m, ch, &char_key.code, &shift, msg)) {
				return U_DESKTOP_ERR_UNSUPPORTED;
			}
			char_key.name = chord->key;
			key = &char_key;
			if (shift) {
				mods_eff |= U_DESKTOP_MOD_SHIFT;
			}
		}
		if (key == NULL) {
			set_msg(msg, "key \"%s\" has no macOS key code", chord->key);
			return U_DESKTOP_ERR_UNSUPPORTED;
		}
	}
	static const struct
	{
		uint32_t mod;
		uint16_t code;
		CGEventFlags flag;
	} mods[] = {
	    {U_DESKTOP_MOD_CTRL, KVK_CONTROL, kCGEventFlagMaskControl},
	    {U_DESKTOP_MOD_ALT, KVK_OPTION, kCGEventFlagMaskAlternate},
	    {U_DESKTOP_MOD_SHIFT, KVK_SHIFT, kCGEventFlagMaskShift},
	    {U_DESKTOP_MOD_SUPER, KVK_COMMAND, kCGEventFlagMaskCommand},
	};
	CGEventFlags flags = 0;
	bool ok = true;
	for (size_t i = 0; i < 4; i++) {
		if (mods_eff & mods[i].mod) {
			flags |= mods[i].flag;
			ok = ok && post_key(m, mods[i].code, true, flags);
			sleep_ms(2);
		}
	}
	if (key != NULL) {
		CGEventFlags kf = flags | (key->fn ? kCGEventFlagMaskSecondaryFn : 0) |
		                  (key->np ? kCGEventFlagMaskNumericPad : 0);
		ok = ok && post_key(m, key->code, true, kf);
		sleep_ms(4);
		ok = ok && post_key(m, key->code, false, kf);
		sleep_ms(2);
	}
	for (int i = 3; i >= 0; i--) {
		if (mods_eff & mods[i].mod) {
			flags &= ~mods[i].flag;
			ok = post_key(m, mods[i].code, false, flags) && ok; // always release
			sleep_ms(2);
		}
	}
	if (!ok) {
		set_msg(msg, "CGEvent creation failed");
		return U_DESKTOP_ERR_FAILED;
	}
	return U_DESKTOP_OK;
}

static CGPoint
cursor_now(void)
{
	CGEventRef e = CGEventCreate(NULL);
	CGPoint p = e ? CGEventGetLocation(e) : CGPointZero;
	if (e) {
		CFRelease(e);
	}
	return p;
}

static enum u_desktop_result
mac_pointer_move(struct u_desktop_backend *b, int32_t x, int32_t y, char *msg)
{
	struct macos_backend *m = mb(b);
	if (!trusted()) {
		set_msg(msg, "Accessibility permission not granted");
		return U_DESKTOP_ERR_NOT_GRANTED;
	}
	if (!point_on_any_display(x, y)) {
		set_msg(msg, "(%d,%d) is on no display", x, y);
		return U_DESKTOP_ERR_REFUSED;
	}
	CGPoint pt = CGPointMake(x, y);
	CGEventType type = kCGEventMouseMoved;
	CGMouseButton btn = kCGMouseButtonLeft;
	if (m->buttons_down & (1u << U_DESKTOP_BUTTON_LEFT)) {
		type = kCGEventLeftMouseDragged;
	} else if (m->buttons_down & (1u << U_DESKTOP_BUTTON_RIGHT)) {
		type = kCGEventRightMouseDragged;
		btn = kCGMouseButtonRight;
	} else if (m->buttons_down & (1u << U_DESKTOP_BUTTON_MIDDLE)) {
		type = kCGEventOtherMouseDragged;
		btn = kCGMouseButtonCenter;
	}
	CGWarpMouseCursorPosition(pt);
	CGEventRef e = CGEventCreateMouseEvent(m->src, type, pt, btn);
	if (e == NULL) {
		set_msg(msg, "CGEvent creation failed");
		return U_DESKTOP_ERR_FAILED;
	}
	CGEventPost(kCGHIDEventTap, e);
	CFRelease(e);
	for (int i = 0; i < 20; i++) {
		CGPoint p = cursor_now();
		if (fabs(p.x - x) <= 2 && fabs(p.y - y) <= 2) {
			return U_DESKTOP_OK;
		}
		sleep_ms(5);
	}
	CGPoint p = cursor_now();
	set_msg(msg, "pointer is at (%d,%d), not (%d,%d)", iround(p.x), iround(p.y), x, y);
	return U_DESKTOP_ERR_FAILED;
}

static enum u_desktop_result
mac_pointer_button(struct u_desktop_backend *b, enum u_desktop_button button, bool down, char *msg)
{
	struct macos_backend *m = mb(b);
	if (!trusted()) {
		set_msg(msg, "Accessibility permission not granted");
		return U_DESKTOP_ERR_NOT_GRANTED;
	}
	CGEventType type;
	CGMouseButton cgb;
	switch (button) {
	case U_DESKTOP_BUTTON_LEFT:
		type = down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;
		cgb = kCGMouseButtonLeft;
		break;
	case U_DESKTOP_BUTTON_RIGHT:
		type = down ? kCGEventRightMouseDown : kCGEventRightMouseUp;
		cgb = kCGMouseButtonRight;
		break;
	case U_DESKTOP_BUTTON_MIDDLE:
		type = down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp;
		cgb = kCGMouseButtonCenter;
		break;
	default: set_msg(msg, "unknown button %d", (int)button); return U_DESKTOP_ERR_UNSUPPORTED;
	}
	CGPoint pt = cursor_now();

	// Click count: the system double-click interval, same button, within 4 pt.
	if (down) {
		double interval = 0.5;
		@autoreleasepool {
			interval = [NSEvent doubleClickInterval];
		}
		uint64_t t = now_ns();
		if (m->last_click_button == (int)button && t - m->last_click_ns <= (uint64_t)(interval * 1e9) &&
		    fabs(pt.x - m->last_click_pt.x) <= 4 && fabs(pt.y - m->last_click_pt.y) <= 4) {
			m->click_count++;
		} else {
			m->click_count = 1;
		}
		m->last_click_button = (int)button;
		m->last_click_ns = t;
		m->last_click_pt = pt;
	}

	CGEventRef e = CGEventCreateMouseEvent(m->src, type, pt, cgb);
	if (e == NULL) {
		set_msg(msg, "CGEvent creation failed");
		return U_DESKTOP_ERR_FAILED;
	}
	CGEventSetIntegerValueField(e, kCGMouseEventClickState, m->click_count > 0 ? m->click_count : 1);
	if (button == U_DESKTOP_BUTTON_MIDDLE) {
		CGEventSetIntegerValueField(e, kCGMouseEventButtonNumber, 2);
	}
	CGEventPost(kCGHIDEventTap, e);
	CFRelease(e);
	if (down) {
		m->buttons_down |= 1u << button;
	} else {
		m->buttons_down &= ~(1u << button);
	}
	return U_DESKTOP_OK;
}

static enum u_desktop_result
mac_scroll(struct u_desktop_backend *b, int32_t dx, int32_t dy, char *msg)
{
	struct macos_backend *m = mb(b);
	if (!trusted()) {
		set_msg(msg, "Accessibility permission not granted");
		return U_DESKTOP_ERR_NOT_GRANTED;
	}
	// wheel1 = vertical (+ = up), wheel2 = horizontal (+ = left); ours is +y down, +x right.
	CGEventRef e = CGEventCreateScrollWheelEvent2(m->src, kCGScrollEventUnitLine, 2, -dy, -dx, 0);
	if (e == NULL) {
		set_msg(msg, "CGEvent creation failed");
		return U_DESKTOP_ERR_FAILED;
	}
	CGEventSetLocation(e, cursor_now());
	CGEventPost(kCGHIDEventTap, e);
	CFRelease(e);
	return U_DESKTOP_OK;
}

/*
 *
 * Lifetime.
 *
 */

static void
mac_destroy(struct u_desktop_backend *b)
{
	struct macos_backend *m = mb(b);
	if (m == NULL) {
		return;
	}
	pthread_mutex_lock(&m->lock);
	m->stop = true;
	pthread_cond_broadcast(&m->cond);
	bool started = m->thread_started;
	pthread_mutex_unlock(&m->lock);
	if (started) {
		pthread_join(m->thread, NULL); // bounded: one AX timeout per remaining app at most
	}
	for (int i = 0; i < AX_CACHE_APPS; i++) {
		ax_entry_clear(&m->ax[i]);
	}
	if (m->src) {
		CFRelease(m->src);
	}
	pthread_cond_destroy(&m->cond);
	pthread_mutex_destroy(&m->lock);
	free(m);
}

struct u_desktop_backend *
u_desktop_backend_create_platform(void)
{
	struct macos_backend *m = calloc(1, sizeof(*m));
	if (m == NULL) {
		return NULL;
	}
	pthread_mutex_init(&m->lock, NULL);
	pthread_cond_init(&m->cond, NULL);
	m->ax_get_window = (ax_get_window_fn)dlsym(RTLD_DEFAULT, "_AXUIElementGetWindow");
	m->src = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
	if (m->src != NULL) {
		CGEventSourceSetLocalEventsSuppressionInterval(m->src, 0.0);
	}
	m->last_click_button = -1;

	struct u_desktop_backend *b = &m->base;
	b->name = "macos";
	b->caps = U_DESKTOP_CAP_LIST_WINDOWS | U_DESKTOP_CAP_POINTER_POSITION | U_DESKTOP_CAP_PICK |
	          U_DESKTOP_CAP_READ_TEXT | U_DESKTOP_CAP_WINDOW_ACTIONS | U_DESKTOP_CAP_INPUT;
	b->remembers_in_store = false;
	b->access_status = mac_access_status;
	b->request_access = mac_request_access;
	b->is_locked = mac_is_locked;
	b->user_idle_ms = mac_user_idle_ms;
	b->list_windows = mac_list_windows;
	b->get_pointer = mac_get_pointer;
	b->pick = mac_pick;
	b->focused_window = mac_focused_window;
	b->text_at = mac_text_at;
	b->window_op = mac_window_op;
	b->type_text = mac_type_text;
	b->key_chord = mac_key_chord;
	b->pointer_move = mac_pointer_move;
	b->pointer_button = mac_pointer_button;
	b->scroll = mac_scroll;
	b->destroy = mac_destroy;
	return b;
}
