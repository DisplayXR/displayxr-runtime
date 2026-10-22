// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-desktop-capture — the macOS live-desktop producer for
 *         transparent sessions on a weaving display processor (#2264).
 *
 * A weaving DP (Leia SR) cannot present per-pixel alpha: the weave flattens
 * it. Windows and Linux therefore compose the app OVER the live desktop behind
 * its window before weaving, then alpha-gate the result. On those platforms the
 * capture is free — WGC with IsBorderRequired(false), mutter ScreenCast through
 * our GNOME extension. On macOS every capture API needs the Screen Recording
 * grant, so this helper exists to hold it ONCE, for DisplayXR, instead of
 * every app that weaves in-process asking for it separately.
 *
 * Lifecycle — invisible unless used:
 *  - launchd starts it on demand when a compositor connects to the Mach service
 *    (`MachServices` in its LaunchAgent; no RunAtLoad). It has no UI
 *    (LSUIElement).
 *  - A display's SCStream runs only while some client on that display wants a
 *    background; clients ask only while their content is actually transparent
 *    (the compositor's alpha probe), so the menu-bar recording indicator shows
 *    only then.
 *  - With no clients it exits after a short idle period.
 *
 * Permission — asked at most once, ever:
 *  - CGPreflightScreenCaptureAccess() gates every ScreenCaptureKit call (it
 *    never prompts; touching SCK without a grant would).
 *  - CGRequestScreenCaptureAccess() runs only for a START that asks for it,
 *    and only if the marker file says we never asked before. After that, the
 *    user decides in System Settings.
 *  - A grant does not reach a running process, so a NOT_GRANTED helper exits
 *    as soon as its last client leaves; the client's retry starts a fresh one.
 *
 * Wire protocol: u_desktop_bg_protocol_macos.h.
 */

#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#include <xpc/xpc.h>

#include "util/u_desktop_bg_protocol_macos.h"

#define DC_MAX_CLIENTS 32
#define DC_RING 4
#define DC_IDLE_EXIT_S 10.0
#define DC_RESTART_BACKOFF_S 1.0

static void
dc_log(NSString *fmt, ...) NS_FORMAT_FUNCTION(1, 2);

static void
dc_log(NSString *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	NSString *s = [[NSString alloc] initWithFormat:fmt arguments:ap];
	va_end(ap);
	NSLog(@"displayxr-desktop-capture: %@", s);
}

struct dc_client
{
	xpc_connection_t conn;
	pid_t pid;
	CGDirectDisplayID display;
	//! A FRAME was sent and not yet ACKed: send nothing else (latest-wins pacing).
	bool in_flight;
	//! Surface the in-flight FRAME refers to (retained). The helper holds one
	//! use count on it until the ACK, so the slot is not rewritten before the
	//! client takes its own hold.
	IOSurfaceRef in_flight_surf;
};

@class DCStream;

static dispatch_queue_t g_q;
static struct dc_client g_clients[DC_MAX_CLIENTS];
static uint32_t g_client_count;
static NSMutableDictionary<NSNumber *, DCStream *> *g_streams;
static id<MTLDevice> g_device;
static id<MTLCommandQueue> g_queue;
static bool g_not_granted;
static uint64_t g_idle_gen;

static void
dc_refresh_display(CGDirectDisplayID display);
static void
dc_schedule_idle_exit(void);

/*
 *
 * Permission.
 *
 */

static NSString *
dc_marker_path(void)
{
	NSString *dir = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/DisplayXR"];
	[[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
	return [dir stringByAppendingPathComponent:@"desktop-capture-prompted"];
}

//! True when granted. Prompts only if @p may_prompt and we never prompted before.
static bool
dc_check_access(bool may_prompt)
{
	if (CGPreflightScreenCaptureAccess()) {
		return true;
	}
	NSString *marker = dc_marker_path();
	if (may_prompt && ![[NSFileManager defaultManager] fileExistsAtPath:marker]) {
		[@"" writeToFile:marker atomically:YES encoding:NSUTF8StringEncoding error:nil];
		dc_log(@"no Screen Recording grant — asking once (System Settings → Privacy & Security → Screen Recording)");
		(void)CGRequestScreenCaptureAccess();
	}
	if (!g_not_granted) {
		dc_log(@"no Screen Recording grant; transparent sessions on a weaving display stay opaque until it is given");
	}
	g_not_granted = true;
	return false;
}

/*
 *
 * Per-display stream.
 *
 */

API_AVAILABLE(macos(12.3))
@interface DCStream : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic) CGDirectDisplayID display;
@property(nonatomic, strong) SCStream *stream;
@property(nonatomic) bool starting;
@property(nonatomic) bool restart_pending;
@property(nonatomic) uint64_t seq;
@property(nonatomic) uint32_t width;
@property(nonatomic) uint32_t height;
//! PIDs the current filter excludes, to know when it must be rebuilt.
@property(nonatomic, strong) NSSet<NSNumber *> *excluded;
@end

//! This display's output ring: what clients sample (never SCK's own pool).
struct dc_ring
{
	IOSurfaceRef surf[DC_RING];
	id<MTLTexture> __strong tex[DC_RING];
	uint32_t w, h;
};

static NSMutableDictionary<NSNumber *, NSValue *> *g_rings;

static struct dc_ring *
dc_ring_for(CGDirectDisplayID display)
{
	NSValue *v = g_rings[@(display)];
	if (v == nil) {
		struct dc_ring *r = calloc(1, sizeof(*r));
		v = [NSValue valueWithPointer:r];
		g_rings[@(display)] = v;
	}
	return (struct dc_ring *)v.pointerValue;
}

static bool
dc_ensure_ring(struct dc_ring *r, uint32_t w, uint32_t h)
{
	if (r->w == w && r->h == h && r->surf[0] != NULL) {
		return true;
	}
	for (int i = 0; i < DC_RING; i++) {
		r->tex[i] = nil;
		if (r->surf[i] != NULL) {
			CFRelease(r->surf[i]); // a client's in-flight hold keeps its own reference
			r->surf[i] = NULL;
		}
	}
	NSDictionary *props = @{
		(id)kIOSurfaceWidth : @(w),
		(id)kIOSurfaceHeight : @(h),
		(id)kIOSurfaceBytesPerElement : @4,
		(id)kIOSurfacePixelFormat : @(kCVPixelFormatType_32BGRA),
	};
	MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
	                                                                                width:w
	                                                                               height:h
	                                                                            mipmapped:NO];
	desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
	desc.storageMode = MTLStorageModeShared;
	for (int i = 0; i < DC_RING; i++) {
		r->surf[i] = IOSurfaceCreate((__bridge CFDictionaryRef)props);
		if (r->surf[i] == NULL) {
			dc_log(@"IOSurfaceCreate %ux%u failed", w, h);
			r->w = r->h = 0;
			return false;
		}
		r->tex[i] = [g_device newTextureWithDescriptor:desc iosurface:r->surf[i] plane:0];
	}
	r->w = w;
	r->h = h;
	return true;
}

static int
dc_free_slot(struct dc_ring *r)
{
	for (int i = 0; i < DC_RING; i++) {
		if (!IOSurfaceIsInUse(r->surf[i])) {
			return i;
		}
	}
	return -1;
}

static void
dc_send_frame(struct dc_client *cl, struct dc_ring *r, int slot, uint64_t seq, CGDirectDisplayID display)
{
	xpc_object_t msg = xpc_dictionary_create(NULL, NULL, 0);
	xpc_dictionary_set_int64(msg, U_DESKTOP_BG_KEY_OP, U_DESKTOP_BG_OP_FRAME);
	xpc_dictionary_set_uint64(msg, U_DESKTOP_BG_KEY_SEQ, seq);
	xpc_dictionary_set_uint64(msg, U_DESKTOP_BG_KEY_DISPLAY, display);
	xpc_dictionary_set_uint64(msg, U_DESKTOP_BG_KEY_WIDTH, r->w);
	xpc_dictionary_set_uint64(msg, U_DESKTOP_BG_KEY_HEIGHT, r->h);
	xpc_object_t surf = IOSurfaceCreateXPCObject(r->surf[slot]);
	xpc_dictionary_set_value(msg, U_DESKTOP_BG_KEY_SURFACE, surf);
	IOSurfaceIncrementUseCount(r->surf[slot]);
	CFRetain(r->surf[slot]);
	cl->in_flight = true;
	cl->in_flight_surf = r->surf[slot];
	xpc_connection_send_message(cl->conn, msg);
}

static void
dc_release_in_flight(struct dc_client *cl)
{
	if (cl->in_flight_surf != NULL) {
		IOSurfaceDecrementUseCount(cl->in_flight_surf);
		CFRelease(cl->in_flight_surf);
		cl->in_flight_surf = NULL;
	}
	cl->in_flight = false;
}

//! "name" or "ICC <n> bytes" for a colour space, for the first-frame log.
static NSString *
dc_colour_desc(CGColorSpaceRef cs)
{
	if (cs == NULL) {
		return @"(untagged)";
	}
	CFStringRef nm = CGColorSpaceCopyName(cs);
	if (nm != NULL) {
		return (__bridge_transfer NSString *)nm;
	}
	CFDataRef icc = CGColorSpaceCopyICCData(cs);
	NSString *d = [NSString stringWithFormat:@"ICC %ld bytes", icc != NULL ? (long)CFDataGetLength(icc) : 0L];
	if (icc != NULL) {
		CFRelease(icc);
	}
	return d;
}

static NSString *
dc_buffer_colour_desc(CVPixelBufferRef pb)
{
	CFDictionaryRef att = CVBufferCopyAttachments(pb, kCVAttachmentMode_ShouldPropagate);
	CGColorSpaceRef cs = att != NULL ? CVImageBufferCreateColorSpaceFromAttachments(att) : NULL;
	NSString *d = dc_colour_desc(cs);
	if (cs != NULL) {
		CGColorSpaceRelease(cs);
	}
	if (att != NULL) {
		CFRelease(att);
	}
	return d;
}

static NSString *
dc_display_colour_desc(CGDirectDisplayID display)
{
	CGColorSpaceRef cs = CGDisplayCopyColorSpace(display);
	NSString *d = dc_colour_desc(cs);
	if (cs != NULL) {
		CGColorSpaceRelease(cs);
	}
	return d;
}

@implementation DCStream

- (void)stream:(SCStream *)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type API_AVAILABLE(macos(12.3))
{
	if (type != SCStreamOutputTypeScreen) {
		return;
	}
	// Only complete frames carry new pixels (idle / blank frames do not).
	CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
	if (attachments != NULL && CFArrayGetCount(attachments) > 0) {
		NSDictionary *a = (__bridge NSDictionary *)CFArrayGetValueAtIndex(attachments, 0);
		NSNumber *st = a[SCStreamFrameInfoStatus];
		if (st != nil && st.integerValue != SCFrameStatusComplete) {
			return;
		}
	}
	CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sampleBuffer);
	if (pb == NULL || CVPixelBufferGetPixelFormatType(pb) != kCVPixelFormatType_32BGRA) {
		return;
	}
	IOSurfaceRef src = CVPixelBufferGetIOSurface(pb);
	if (src == NULL) {
		return;
	}
	uint32_t w = (uint32_t)IOSurfaceGetWidth(src);
	uint32_t h = (uint32_t)IOSurfaceGetHeight(src);
	struct dc_ring *r = dc_ring_for(self.display);
	if (!dc_ensure_ring(r, w, h)) {
		return;
	}
	int slot = dc_free_slot(r);
	if (slot < 0) {
		return; // every slot held by a consumer: drop this frame (latest-wins)
	}

	// Copy out of SCK's pool so its buffers recycle on their own schedule and
	// a slow consumer can never starve the stream.
	MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
	                                                                              width:w
	                                                                             height:h
	                                                                          mipmapped:NO];
	sd.usage = MTLTextureUsageShaderRead;
	sd.storageMode = MTLStorageModeShared;
	id<MTLTexture> src_tex = [g_device newTextureWithDescriptor:sd iosurface:src plane:0];
	if (src_tex == nil) {
		return;
	}
	id<MTLCommandBuffer> cb = [g_queue commandBuffer];
	id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
	[blit copyFromTexture:src_tex toTexture:r->tex[slot]];
	[blit endEncoding];
	[cb commit];
	[cb waitUntilCompleted];

	self.seq = self.seq + 1;
	self.width = w;
	self.height = h;
	if (self.seq == 1) {
		dc_log(@"display %u: first frame %ux%u, colour %@ (display's own: %@)", self.display, w, h,
		       dc_buffer_colour_desc(pb), dc_display_colour_desc(self.display));
	}
	for (uint32_t i = 0; i < g_client_count; i++) {
		struct dc_client *cl = &g_clients[i];
		if (cl->display == self.display && !cl->in_flight) {
			dc_send_frame(cl, r, slot, self.seq, self.display);
		}
	}
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error API_AVAILABLE(macos(12.3))
{
	// Lock / sleep / display reconfiguration / grant revoked.
	dispatch_async(g_q, ^{
	  if (self.stream != stream) {
		  return;
	  }
	  dc_log(@"display %u stream stopped (%@) — restarting in %.0f s", self.display,
	         error.localizedDescription, DC_RESTART_BACKOFF_S);
	  self.stream = nil;
	  self.excluded = nil;
	  if (!self.restart_pending) {
		  self.restart_pending = true;
		  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(DC_RESTART_BACKOFF_S * NSEC_PER_SEC)), g_q, ^{
			self.restart_pending = false;
			dc_refresh_display(self.display);
		  });
	  }
	});
}

@end

/*
 *
 * Stream management (all on g_q).
 *
 */

static NSSet<NSNumber *> *
dc_pids_for(CGDirectDisplayID display)
{
	NSMutableSet *s = [NSMutableSet set];
	for (uint32_t i = 0; i < g_client_count; i++) {
		if (g_clients[i].display == display) {
			[s addObject:@(g_clients[i].pid)];
		}
	}
	return s;
}

static void
dc_stop_stream(DCStream *ds) API_AVAILABLE(macos(12.3))
{
	SCStream *st = ds.stream;
	ds.stream = nil;
	ds.excluded = nil;
	if (st != nil) {
		[st stopCaptureWithCompletionHandler:^(NSError *e) { (void)e; }];
		dc_log(@"display %u: capture stopped (no client wants a background)", ds.display);
	}
}

static void
dc_refresh_display(CGDirectDisplayID display)
{
	if (@available(macOS 12.3, *)) {
		DCStream *ds = g_streams[@(display)];
		NSSet *pids = dc_pids_for(display);
		if (pids.count == 0) {
			if (ds != nil) {
				dc_stop_stream(ds);
				[g_streams removeObjectForKey:@(display)];
			}
			return;
		}
		if (!dc_check_access(false)) {
			return;
		}
		if (ds == nil) {
			ds = [DCStream new];
			ds.display = display;
			g_streams[@(display)] = ds;
		}
		if (ds.starting || (ds.stream != nil && [ds.excluded isEqualToSet:pids])) {
			return;
		}
		ds.starting = true;

		[SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent *content, NSError *err) {
		  dispatch_async(g_q, ^{
			ds.starting = false;
			if (err != nil || content == nil) {
				dc_log(@"SCShareableContent failed: %@", err.localizedDescription);
				return;
			}
			NSSet *want = dc_pids_for(display);
			if (want.count == 0) {
				return;
			}
			SCDisplay *scd = nil;
			for (SCDisplay *d in content.displays) {
				if (d.displayID == display) {
					scd = d;
				}
			}
			if (scd == nil) {
				dc_log(@"display %u not in SCShareableContent", display);
				return;
			}
			NSMutableArray *apps = [NSMutableArray array];
			for (SCRunningApplication *app in content.applications) {
				if ([want containsObject:@(app.processID)]) {
					[apps addObject:app];
				}
			}
			SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:scd
			                                             excludingApplications:apps
			                                                  exceptingWindows:@[]];
			if (ds.stream != nil) {
				[ds.stream updateContentFilter:filter
				             completionHandler:^(NSError *e) {
				               if (e != nil) {
					               dc_log(@"updateContentFilter failed: %@", e.localizedDescription);
				               }
				             }];
				ds.excluded = want;
				return;
			}

			CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
			size_t pw = mode != NULL ? CGDisplayModeGetPixelWidth(mode) : (size_t)scd.width;
			size_t ph = mode != NULL ? CGDisplayModeGetPixelHeight(mode) : (size_t)scd.height;
			double hz = mode != NULL ? CGDisplayModeGetRefreshRate(mode) : 0.0;
			if (mode != NULL) {
				CGDisplayModeRelease(mode);
			}
			if (hz <= 0.0) {
				hz = 60.0;
			}

			SCStreamConfiguration *cfg = [SCStreamConfiguration new];
			cfg.width = pw;
			cfg.height = ph;
			cfg.pixelFormat = kCVPixelFormatType_32BGRA;
			/*
			 * Colour: the DISPLAY's own space, not sRGB. The weaving DP tags
			 * its woven frame with the panel's colour space so WindowServer
			 * leaves the weave alone (no colour matching). The backdrop is
			 * woven into that frame, so its bytes must already be panel-
			 * native, i.e. what the desktop looks like on that panel. An sRGB
			 * capture shown as panel-native came out visibly washed out on
			 * the Odyssey G90XF, whose EDID profile has no named space.
			 * Unset = SCK's default; the first-frame log names what arrived.
			 * DXR_DESKTOP_CAPTURE_SRGB=1 restores the sRGB request (A/B).
			 */
			const char *srgb_env = getenv("DXR_DESKTOP_CAPTURE_SRGB");
			if (srgb_env != NULL && srgb_env[0] == '1') {
				cfg.colorSpaceName = kCGColorSpaceSRGB;
			}
			cfg.showsCursor = NO;
			cfg.minimumFrameInterval = CMTimeMake(1, (int32_t)(hz + 0.5));
			cfg.queueDepth = 4;

			SCStream *st = [[SCStream alloc] initWithFilter:filter configuration:cfg delegate:ds];
			NSError *addErr = nil;
			if (![st addStreamOutput:ds type:SCStreamOutputTypeScreen sampleHandlerQueue:g_q error:&addErr]) {
				dc_log(@"addStreamOutput failed: %@", addErr.localizedDescription);
				return;
			}
			ds.stream = st;
			ds.excluded = want;
			[st startCaptureWithCompletionHandler:^(NSError *e) {
			  if (e != nil) {
				  dc_log(@"display %u: startCapture failed: %@", display, e.localizedDescription);
				  dispatch_async(g_q, ^{
					if (ds.stream == st) {
						ds.stream = nil;
						ds.excluded = nil;
					}
				  });
				  return;
			  }
			  dc_log(@"display %u: capturing %zux%zu @ %.0f Hz, excluding %lu client app(s)", display, pw, ph,
			         hz, (unsigned long)apps.count);
			}];
		  });
		}];
	}
}

/*
 *
 * Clients.
 *
 */

static struct dc_client *
dc_find(xpc_connection_t conn)
{
	for (uint32_t i = 0; i < g_client_count; i++) {
		if (g_clients[i].conn == conn) {
			return &g_clients[i];
		}
	}
	return NULL;
}

static void
dc_remove(xpc_connection_t conn)
{
	for (uint32_t i = 0; i < g_client_count; i++) {
		if (g_clients[i].conn == conn) {
			CGDirectDisplayID d = g_clients[i].display;
			dc_release_in_flight(&g_clients[i]);
			g_clients[i] = g_clients[g_client_count - 1];
			g_clients[g_client_count - 1] = (struct dc_client){0};
			g_client_count--;
			dc_refresh_display(d);
			break;
		}
	}
	if (g_client_count == 0) {
		dc_schedule_idle_exit();
	}
}

static void
dc_reply_status(xpc_connection_t conn, enum u_desktop_bg_status status)
{
	xpc_object_t msg = xpc_dictionary_create(NULL, NULL, 0);
	xpc_dictionary_set_int64(msg, U_DESKTOP_BG_KEY_OP, U_DESKTOP_BG_OP_STATUS);
	xpc_dictionary_set_int64(msg, U_DESKTOP_BG_KEY_STATUS, status);
	xpc_connection_send_message(conn, msg);
}

static void
dc_handle(xpc_connection_t conn, xpc_object_t msg)
{
	int64_t op = xpc_dictionary_get_int64(msg, U_DESKTOP_BG_KEY_OP);
	struct dc_client *cl = dc_find(conn);

	switch (op) {
	case U_DESKTOP_BG_OP_START: {
		CGDirectDisplayID display = (CGDirectDisplayID)xpc_dictionary_get_uint64(msg, U_DESKTOP_BG_KEY_DISPLAY);
		bool may_prompt = xpc_dictionary_get_bool(msg, U_DESKTOP_BG_KEY_REQUEST_ACCESS);
		if (@available(macOS 12.3, *)) {
		} else {
			dc_reply_status(conn, U_DESKTOP_BG_STATUS_UNSUPPORTED);
			return;
		}
		if (!dc_check_access(may_prompt)) {
			dc_reply_status(conn, U_DESKTOP_BG_STATUS_NOT_GRANTED);
			return;
		}
		if (cl == NULL) {
			if (g_client_count >= DC_MAX_CLIENTS) {
				dc_reply_status(conn, U_DESKTOP_BG_STATUS_ERROR);
				return;
			}
			cl = &g_clients[g_client_count++];
			*cl = (struct dc_client){0};
			cl->conn = conn;
			cl->pid = xpc_connection_get_pid(conn);
		}
		CGDirectDisplayID old = cl->display;
		cl->display = display;
		dc_reply_status(conn, U_DESKTOP_BG_STATUS_STREAMING);
		if (old != 0 && old != display) {
			dc_release_in_flight(cl);
			dc_refresh_display(old);
		}
		dc_refresh_display(display);
		break;
	}
	case U_DESKTOP_BG_OP_ACK:
		if (cl != NULL) {
			dc_release_in_flight(cl);
		}
		break;
	case U_DESKTOP_BG_OP_STOP: dc_remove(conn); break;
	default: break;
	}
}

static void
dc_schedule_idle_exit(void)
{
	uint64_t gen = ++g_idle_gen;
	// NOT_GRANTED: leave at once, so the next START sees a fresh grant.
	double delay = g_not_granted ? 0.5 : DC_IDLE_EXIT_S;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(delay * NSEC_PER_SEC)), g_q, ^{
	  if (gen == g_idle_gen && g_client_count == 0) {
		  dc_log(@"idle — exiting (launchd restarts on demand)");
		  exit(0);
	  }
	});
}

int
main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	@autoreleasepool {
		g_q = dispatch_queue_create("com.displayxr.desktop-capture", DISPATCH_QUEUE_SERIAL);
		g_streams = [NSMutableDictionary dictionary];
		g_rings = [NSMutableDictionary dictionary];
		g_device = MTLCreateSystemDefaultDevice();
		g_queue = [g_device newCommandQueue];
		if (g_device == nil || g_queue == nil) {
			dc_log(@"no Metal device");
			return 1;
		}

		xpc_connection_t listener =
		    xpc_connection_create_mach_service(U_DESKTOP_BG_SERVICE, g_q, XPC_CONNECTION_MACH_SERVICE_LISTENER);
		xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
		  if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) {
			  return;
		  }
		  xpc_connection_t conn = (xpc_connection_t)peer;
		  ++g_idle_gen; // a client arrived: cancel a pending idle exit
		  xpc_connection_set_target_queue(conn, g_q);
		  xpc_connection_set_event_handler(conn, ^(xpc_object_t ev) {
			if (xpc_get_type(ev) == XPC_TYPE_DICTIONARY) {
				dc_handle(conn, ev);
			} else if (xpc_get_type(ev) == XPC_TYPE_ERROR) {
				dc_remove(conn);
				if (g_client_count == 0) {
					dc_schedule_idle_exit();
				}
			}
		  });
		  xpc_connection_resume(conn);
		});
		xpc_connection_resume(listener);
		dispatch_async(g_q, ^{ dc_schedule_idle_exit(); }); // launched but nobody connected
		dispatch_main();
	}
	return 0;
}
