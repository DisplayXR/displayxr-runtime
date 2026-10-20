// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Client of the macOS desktop-capture helper (#2264). Compiled with ARC.
 * @ingroup comp_metal
 */

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#include <os/lock.h>
#include <xpc/xpc.h>

#include "comp_metal_desktop_bg.h"

#include "util/u_debug.h"
#include "util/u_desktop_bg_protocol_macos.h"
#include "util/u_logging.h"

//! Retry period while the helper answers NOT_GRANTED / ERROR, or is missing.
#define DBG_RETRY_S 10.0

/*
 * DXR_MACOS_DESKTOP_BG_PROMPT=0: never let the helper show the Screen
 * Recording prompt from this process (the grant must already exist).
 */
DEBUG_GET_ONCE_BOOL_OPTION(desktop_bg_prompt, "DXR_MACOS_DESKTOP_BG_PROMPT", true)

struct comp_metal_desktop_bg
{
	id<MTLDevice> device;
	dispatch_queue_t q;

	// --- q only ---
	xpc_connection_t conn;
	bool want;
	uint32_t display;
	bool retry_scheduled;
	bool unsupported;
	bool asked_access;
	enum u_desktop_bg_status last_status;
	bool status_logged;

	// --- render-thread mirror of `want`/`display`, to detect edges cheaply ---
	bool rt_want;
	uint32_t rt_display;

	// --- latest frame, guarded by lock (written on q, read on render thread) ---
	os_unfair_lock lock;
	IOSurfaceRef latest;      //!< retained + one use count held
	uint32_t latest_display;
	uint32_t latest_w, latest_h;

	// --- render thread only ---
	IOSurfaceRef cached_surf; //!< retained; @ref cached_tex wraps it
	id<MTLTexture> cached_tex;
};

static void
dbg_clear_latest(struct comp_metal_desktop_bg *bg)
{
	os_unfair_lock_lock(&bg->lock);
	IOSurfaceRef old = bg->latest;
	bg->latest = NULL;
	os_unfair_lock_unlock(&bg->lock);
	if (old != NULL) {
		IOSurfaceDecrementUseCount(old);
		CFRelease(old);
	}
}

static void
dbg_connect(struct comp_metal_desktop_bg *bg);

static void
dbg_schedule_retry(struct comp_metal_desktop_bg *bg)
{
	if (bg->retry_scheduled || bg->unsupported) {
		return;
	}
	bg->retry_scheduled = true;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(DBG_RETRY_S * NSEC_PER_SEC)), bg->q, ^{
	  bg->retry_scheduled = false;
	  if (bg->want && bg->conn == nil) {
		  dbg_connect(bg);
	  }
	});
}

static void
dbg_disconnect(struct comp_metal_desktop_bg *bg)
{
	if (bg->conn != nil) {
		xpc_connection_cancel(bg->conn);
		bg->conn = nil;
	}
	dbg_clear_latest(bg);
}

static void
dbg_send_start(struct comp_metal_desktop_bg *bg)
{
	xpc_object_t msg = xpc_dictionary_create(NULL, NULL, 0);
	xpc_dictionary_set_int64(msg, U_DESKTOP_BG_KEY_OP, U_DESKTOP_BG_OP_START);
	xpc_dictionary_set_uint64(msg, U_DESKTOP_BG_KEY_DISPLAY, bg->display);
	// Ask for the prompt only on the first START of this session; the helper
	// itself shows it at most once ever.
	bool prompt = !bg->asked_access && debug_get_bool_option_desktop_bg_prompt();
	bg->asked_access = true;
	xpc_dictionary_set_bool(msg, U_DESKTOP_BG_KEY_REQUEST_ACCESS, prompt);
	xpc_connection_send_message(bg->conn, msg);
}

static void
dbg_on_status(struct comp_metal_desktop_bg *bg, xpc_connection_t conn, enum u_desktop_bg_status status)
{
	if (status != bg->last_status || !bg->status_logged) {
		bg->status_logged = true;
		bg->last_status = status;
		switch (status) {
		case U_DESKTOP_BG_STATUS_STREAMING:
			U_LOG_W("desktop background: live capture of display %u (displayxr-desktop-capture)", bg->display);
			break;
		case U_DESKTOP_BG_STATUS_NOT_GRANTED:
			U_LOG_W("desktop background: no Screen Recording grant for DisplayXR Desktop Capture — the transparent "
			        "background weaves without the desktop behind it. Enable it in System Settings → Privacy & "
			        "Security → Screen Recording.");
			break;
		case U_DESKTOP_BG_STATUS_UNSUPPORTED:
			U_LOG_W("desktop background: unsupported here (ScreenCaptureKit needs macOS 12.3+)");
			break;
		default: U_LOG_W("desktop background: capture helper reported an error; retrying"); break;
		}
	}
	if (status == U_DESKTOP_BG_STATUS_STREAMING) {
		return;
	}
	if (status == U_DESKTOP_BG_STATUS_UNSUPPORTED) {
		bg->unsupported = true;
	}
	// Drop the connection so a NOT_GRANTED helper can exit and the retry
	// reaches a fresh process that sees a grant given meanwhile.
	if (bg->conn == conn) {
		dbg_disconnect(bg);
	}
	dbg_schedule_retry(bg);
}

static void
dbg_on_frame(struct comp_metal_desktop_bg *bg, xpc_connection_t conn, xpc_object_t msg)
{
	uint64_t seq = xpc_dictionary_get_uint64(msg, U_DESKTOP_BG_KEY_SEQ);
	xpc_object_t xs = xpc_dictionary_get_value(msg, U_DESKTOP_BG_KEY_SURFACE);
	IOSurfaceRef surf = xs != NULL ? IOSurfaceLookupFromXPCObject(xs) : NULL; // +1
	static bool first_logged = false;
	if (!first_logged) {
		first_logged = true;
		U_LOG_W("desktop background: first frame received (seq %llu, surface %s)", (unsigned long long)seq,
		        surf != NULL ? "ok" : "MISSING");
	}
	if (surf != NULL) {
		// Our own hold before the ACK lets the helper drop its in-flight one.
		IOSurfaceIncrementUseCount(surf);
		os_unfair_lock_lock(&bg->lock);
		IOSurfaceRef old = bg->latest;
		bg->latest = surf;
		bg->latest_display = (uint32_t)xpc_dictionary_get_uint64(msg, U_DESKTOP_BG_KEY_DISPLAY);
		bg->latest_w = (uint32_t)xpc_dictionary_get_uint64(msg, U_DESKTOP_BG_KEY_WIDTH);
		bg->latest_h = (uint32_t)xpc_dictionary_get_uint64(msg, U_DESKTOP_BG_KEY_HEIGHT);
		os_unfair_lock_unlock(&bg->lock);
		if (old != NULL) {
			IOSurfaceDecrementUseCount(old);
			CFRelease(old);
		}
	}
	xpc_object_t ack = xpc_dictionary_create(NULL, NULL, 0);
	xpc_dictionary_set_int64(ack, U_DESKTOP_BG_KEY_OP, U_DESKTOP_BG_OP_ACK);
	xpc_dictionary_set_uint64(ack, U_DESKTOP_BG_KEY_SEQ, seq);
	xpc_connection_send_message(conn, ack);
}

static void
dbg_connect(struct comp_metal_desktop_bg *bg)
{
	if (bg->unsupported) {
		return;
	}
	xpc_connection_t conn = xpc_connection_create_mach_service(U_DESKTOP_BG_SERVICE, bg->q, 0);
	if (conn == nil) {
		dbg_schedule_retry(bg);
		return;
	}
	bg->conn = conn;
	xpc_connection_set_event_handler(conn, ^(xpc_object_t ev) {
	  if (xpc_get_type(ev) == XPC_TYPE_DICTIONARY) {
		  int64_t op = xpc_dictionary_get_int64(ev, U_DESKTOP_BG_KEY_OP);
		  if (op == U_DESKTOP_BG_OP_STATUS) {
			  dbg_on_status(bg, conn, (enum u_desktop_bg_status)xpc_dictionary_get_int64(
			                              ev, U_DESKTOP_BG_KEY_STATUS));
		  } else if (op == U_DESKTOP_BG_OP_FRAME && bg->conn == conn) {
			  dbg_on_frame(bg, conn, ev);
		  }
		  return;
	  }
	  if (ev == XPC_ERROR_CONNECTION_INTERRUPTED) {
		  // The helper exited (idle, crash, restart); launchd relaunches it on
		  // the next message, so just say START again.
		  if (bg->conn == conn && bg->want) {
			  dbg_clear_latest(bg);
			  dbg_send_start(bg);
		  }
		  return;
	  }
	  // XPC_ERROR_CONNECTION_INVALID: no such service (helper not installed)
	  // or we cancelled.
	  if (bg->conn == conn) {
		  static bool logged = false;
		  if (!logged) {
			  logged = true;
			  U_LOG_W("desktop background: capture helper (%s) not reachable — not installed? The transparent "
			          "background weaves without the desktop behind it.",
			          U_DESKTOP_BG_SERVICE);
		  }
		  bg->conn = nil;
		  dbg_clear_latest(bg);
		  dbg_schedule_retry(bg);
	  }
	});
	xpc_connection_resume(conn);
	dbg_send_start(bg);
}

struct comp_metal_desktop_bg *
comp_metal_desktop_bg_create(void *mtl_device)
{
	struct comp_metal_desktop_bg *bg = calloc(1, sizeof(*bg));
	bg->device = (__bridge id<MTLDevice>)mtl_device;
	bg->q = dispatch_queue_create("com.displayxr.desktop-bg.client", DISPATCH_QUEUE_SERIAL);
	bg->lock = OS_UNFAIR_LOCK_INIT;
	return bg;
}

void
comp_metal_desktop_bg_destroy(struct comp_metal_desktop_bg **bg_ptr)
{
	struct comp_metal_desktop_bg *bg = *bg_ptr;
	if (bg == NULL) {
		return;
	}
	*bg_ptr = NULL;
	dispatch_sync(bg->q, ^{
	  bg->want = false;
	  dbg_disconnect(bg);
	});
	bg->cached_tex = nil;
	if (bg->cached_surf != NULL) {
		CFRelease(bg->cached_surf);
	}
	bg->device = nil;
	bg->q = nil;
	bg->conn = nil;
	free(bg);
}

void
comp_metal_desktop_bg_want(struct comp_metal_desktop_bg *bg, bool want, uint32_t display_id)
{
	if (bg == NULL || (want == bg->rt_want && (!want || display_id == bg->rt_display))) {
		return;
	}
	bg->rt_want = want;
	bg->rt_display = display_id;
	dispatch_async(bg->q, ^{
	  bool display_changed = bg->display != display_id;
	  bg->want = want;
	  bg->display = display_id;
	  if (!want) {
		  if (bg->conn != nil) {
			  U_LOG_I("desktop background: no longer wanted — releasing the capture");
		  }
		  dbg_disconnect(bg);
	  } else if (bg->conn == nil) {
		  if (!bg->retry_scheduled) {
			  dbg_connect(bg);
		  }
	  } else if (display_changed) {
		  dbg_clear_latest(bg);
		  dbg_send_start(bg);
	  }
	});
}

void *
comp_metal_desktop_bg_acquire(struct comp_metal_desktop_bg *bg,
                              void *mtl_command_buffer,
                              uint32_t display_id,
                              uint32_t *out_width,
                              uint32_t *out_height)
{
	*out_width = 0;
	*out_height = 0;
	if (bg == NULL || !bg->rt_want) {
		return NULL;
	}

	os_unfair_lock_lock(&bg->lock);
	IOSurfaceRef surf = bg->latest;
	uint32_t w = bg->latest_w, h = bg->latest_h;
	bool match = surf != NULL && bg->latest_display == display_id;
	if (match) {
		// The GPU hold: released when the caller's command buffer completes.
		CFRetain(surf);
		IOSurfaceIncrementUseCount(surf);
	}
	os_unfair_lock_unlock(&bg->lock);
	if (!match) {
		return NULL;
	}

	id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)mtl_command_buffer;
	[cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
	  (void)done;
	  IOSurfaceDecrementUseCount(surf);
	  CFRelease(surf);
	}];

	if (bg->cached_surf != surf) {
		MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
		                                                                             width:w
		                                                                            height:h
		                                                                         mipmapped:NO];
		d.usage = MTLTextureUsageShaderRead;
		d.storageMode = MTLStorageModeShared;
		bg->cached_tex = [bg->device newTextureWithDescriptor:d iosurface:surf plane:0];
		if (bg->cached_surf != NULL) {
			CFRelease(bg->cached_surf);
		}
		CFRetain(surf);
		bg->cached_surf = surf;
	}
	if (bg->cached_tex == nil) {
		return NULL;
	}
	*out_width = w;
	*out_height = h;
	return (__bridge void *)bg->cached_tex;
}
