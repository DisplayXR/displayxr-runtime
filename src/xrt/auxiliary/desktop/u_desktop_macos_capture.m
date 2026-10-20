// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS desktop control: ScreenCaptureKit stills (XR_DXR_desktop_control
 *         spec §10, ADR-056 Amendment 1). See u_desktop_macos_capture.h.
 *
 * ScreenCaptureKit is WEAK-linked (CMake: -weak_framework): on a macOS without it
 * (< 12.3) the backend simply reports no CAPTURE capability. One still per display
 * the region touches, each with a display SCContentFilter that excludes the calling
 * process's own applications, the cursor hidden, BGRA in sRGB, at the display's
 * backing scale:
 *   - macOS 14+: SCScreenshotManager captureSampleBufferWithFilter (one call);
 *   - macOS 12.3–13: a one-shot SCStream (start, the first complete frame, stop).
 *
 * Every completion handler of ScreenCaptureKit may run after we stopped waiting
 * for it (the 2 s bound), so each async step talks to us through a small
 * reference-counted `struct sck_wait`, never through the caller's stack.
 *
 * Privacy (spec §10.4): pixels go only into the caller's image. Nothing here
 * logs or writes them; messages name failures, never content.
 *
 * Built without ARC: every alloc/retain is released explicitly.
 */

#include "u_desktop_macos_capture.h"
#include "u_desktop_capture.h"
#include "os/os_time.h"

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static bool g_force_stream = false;

static void
cap_msg(char *msg, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void
cap_msg(char *msg, const char *fmt, ...)
{
	if (msg == NULL) {
		return;
	}
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, U_DESKTOP_MSG_SIZE, fmt, ap);
	va_end(ap);
}

/*
 *
 * One async step: a reference-counted rendezvous with a completion handler.
 *
 */

struct sck_wait
{
	pthread_mutex_t mu;
	pthread_cond_t cv;
	int refs;
	bool done;
	long err;      //!< 0 = success, else the NSError code (or -1)
	bool declined; //!< SCStreamErrorUserDeclined: the TCC grant is gone
	id obj;        //!< retained result object (SCShareableContent)
	CVPixelBufferRef pb; //!< retained frame
};

static struct sck_wait *
wait_new(void)
{
	struct sck_wait *w = calloc(1, sizeof(*w));
	if (w == NULL) {
		return NULL;
	}
	pthread_mutex_init(&w->mu, NULL);
	pthread_cond_init(&w->cv, NULL);
	w->refs = 1;
	return w;
}

static void
wait_ref(struct sck_wait *w)
{
	pthread_mutex_lock(&w->mu);
	w->refs++;
	pthread_mutex_unlock(&w->mu);
}

static void
wait_unref(struct sck_wait *w)
{
	pthread_mutex_lock(&w->mu);
	int r = --w->refs;
	pthread_mutex_unlock(&w->mu);
	if (r > 0) {
		return;
	}
	if (w->obj != nil) {
		[w->obj release];
	}
	if (w->pb != NULL) {
		CVPixelBufferRelease(w->pb);
	}
	pthread_cond_destroy(&w->cv);
	pthread_mutex_destroy(&w->mu);
	free(w);
}

//! First answer wins; later ones (a stream's next frames) are ignored.
static void
wait_finish(struct sck_wait *w, id obj, CVPixelBufferRef pb, NSError *err)
{
	pthread_mutex_lock(&w->mu);
	if (!w->done) {
		w->done = true;
		if (obj != nil) {
			w->obj = [obj retain];
		}
		if (pb != NULL) {
			w->pb = CVPixelBufferRetain(pb);
		}
		if (err != nil) {
			w->err = err.code != 0 ? (long)err.code : -1;
			if (@available(macOS 12.3, *)) {
				w->declined = [err.domain isEqualToString:SCStreamErrorDomain] &&
				              err.code == SCStreamErrorUserDeclined;
			}
		} else if (obj == nil && pb == NULL) {
			w->err = -1;
		}
		pthread_cond_broadcast(&w->cv);
	}
	pthread_mutex_unlock(&w->mu);
}

//! Wait until answered or @p deadline_ns (monotonic). True when answered.
static bool
wait_until(struct sck_wait *w, uint64_t deadline_ns)
{
	pthread_mutex_lock(&w->mu);
	while (!w->done) {
		uint64_t now = os_monotonic_get_ns();
		if (now >= deadline_ns) {
			break;
		}
		uint64_t left = deadline_ns - now;
		if (left > 50ull * 1000 * 1000) {
			left = 50ull * 1000 * 1000; // re-read the monotonic clock regularly
		}
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		uint64_t ns = (uint64_t)ts.tv_nsec + left;
		ts.tv_sec += (time_t)(ns / 1000000000ull);
		ts.tv_nsec = (long)(ns % 1000000000ull);
		pthread_cond_timedwait(&w->cv, &w->mu, &ts);
	}
	bool done = w->done;
	pthread_mutex_unlock(&w->mu);
	return done;
}

/*
 *
 * The macOS 12.3–13 path: a one-shot SCStream.
 *
 */

API_AVAILABLE(macos(12.3))
@interface DXRDesktopCaptureStreamOutput : NSObject <SCStreamOutput, SCStreamDelegate>
{
@public
	struct sck_wait *wait; //!< one reference, dropped in dealloc
}
@end

@implementation DXRDesktopCaptureStreamOutput

- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sb ofType:(SCStreamOutputType)type
{
	(void)stream;
	if (type != SCStreamOutputTypeScreen || sb == NULL || !CMSampleBufferIsValid(sb)) {
		return;
	}
	CFArrayRef atts = CMSampleBufferGetSampleAttachmentsArray(sb, false);
	if (atts == NULL || CFArrayGetCount(atts) == 0) {
		return;
	}
	NSDictionary *a = (NSDictionary *)CFArrayGetValueAtIndex(atts, 0);
	NSNumber *st = a[SCStreamFrameInfoStatus];
	if (st == nil || st.integerValue != SCFrameStatusComplete) {
		return; // idle / blank / suspended frames carry no picture
	}
	CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sb);
	if (pb != NULL) {
		wait_finish(wait, nil, pb, nil);
	}
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
	(void)stream;
	wait_finish(wait, nil, NULL, error);
}

- (void)dealloc
{
	if (wait != NULL) {
		wait_unref(wait);
	}
	[super dealloc];
}

@end

/*
 *
 * Steps.
 *
 */

static SCShareableContent *
fetch_content(uint64_t deadline_ns, enum u_desktop_result *r, char *msg) API_AVAILABLE(macos(12.3))
{
	struct sck_wait *w = wait_new();
	if (w == NULL) {
		*r = U_DESKTOP_ERR_FAILED;
		cap_msg(msg, "out of memory");
		return nil;
	}
	wait_ref(w); // the completion handler's
	[SCShareableContent getShareableContentExcludingDesktopWindows:NO
	                                           onScreenWindowsOnly:YES
	                                             completionHandler:^(SCShareableContent *c, NSError *e) {
		                                             wait_finish(w, c, NULL, e);
		                                             wait_unref(w);
	                                             }];
	SCShareableContent *content = nil;
	if (!wait_until(w, deadline_ns)) {
		*r = U_DESKTOP_ERR_TIMEOUT;
		cap_msg(msg, "ScreenCaptureKit did not list the displays in time");
	} else if (w->declined) {
		*r = U_DESKTOP_ERR_NOT_GRANTED;
		cap_msg(msg, "Screen Recording permission is not granted");
	} else if (w->err != 0 || w->obj == nil) {
		*r = U_DESKTOP_ERR_FAILED;
		cap_msg(msg, "ScreenCaptureKit could not list the displays (error %ld)", w->err);
	} else {
		content = [w->obj retain];
		*r = U_DESKTOP_OK;
	}
	wait_unref(w);
	return content;
}

static SCStreamConfiguration *
make_config(const struct u_desktop_macos_capture_part *p) API_AVAILABLE(macos(12.3))
{
	SCStreamConfiguration *cfg = [[SCStreamConfiguration alloc] init];
	cfg.sourceRect = CGRectMake(p->local.x, p->local.y, p->local.w, p->local.h);
	cfg.width = p->px_w;
	cfg.height = p->px_h;
	cfg.scalesToFit = YES;
	cfg.showsCursor = NO;
	cfg.pixelFormat = kCVPixelFormatType_32BGRA;
	cfg.colorSpaceName = kCGColorSpaceSRGB;
	if (@available(macOS 14.0, *)) {
		cfg.captureResolution = SCCaptureResolutionBest;
		cfg.shouldBeOpaque = YES;
		cfg.preservesAspectRatio = NO; // the size is the source rect's already
	}
	return cfg;
}

//! One frame of @p filter as configured, through the screenshot API (14+) or a
//! one-shot stream. The returned wait is answered (or the caller timed out).
static struct sck_wait *
grab_frame(SCContentFilter *filter, SCStreamConfiguration *cfg, uint64_t deadline_ns) API_AVAILABLE(macos(12.3))
{
	struct sck_wait *w = wait_new();
	if (w == NULL) {
		return NULL;
	}
	if (@available(macOS 14.0, *)) {
		if (!g_force_stream) {
			wait_ref(w); // the completion handler's
			[SCScreenshotManager captureSampleBufferWithFilter:filter
			                                     configuration:cfg
			                                 completionHandler:^(CMSampleBufferRef sb, NSError *e) {
				                                 CVPixelBufferRef pb =
				                                     sb != NULL ? CMSampleBufferGetImageBuffer(sb) : NULL;
				                                 wait_finish(w, nil, pb, e);
				                                 wait_unref(w);
			                                 }];
			wait_until(w, deadline_ns);
			return w;
		}
	}

	// One-shot SCStream (macOS 12.3–13, or forced).
	cfg.minimumFrameInterval = CMTimeMake(1, 60);
	cfg.queueDepth = 3;
	DXRDesktopCaptureStreamOutput *out = [[DXRDesktopCaptureStreamOutput alloc] init];
	wait_ref(w);
	out->wait = w;
	SCStream *s = [[SCStream alloc] initWithFilter:filter configuration:cfg delegate:out];
	dispatch_queue_t q = dispatch_queue_create("org.displayxr.desktop.capture", DISPATCH_QUEUE_SERIAL);
	NSError *err = nil;
	if (![s addStreamOutput:out type:SCStreamOutputTypeScreen sampleHandlerQueue:q error:&err]) {
		wait_finish(w, nil, NULL, err);
	} else {
		wait_ref(w); // the start handler's
		[s startCaptureWithCompletionHandler:^(NSError *e) {
			if (e != nil) {
				wait_finish(w, nil, NULL, e);
			}
			wait_unref(w);
		}];
		wait_until(w, deadline_ns);

		// Stop it whatever happened (bounded: a little past the deadline at most).
		struct sck_wait *sw = wait_new();
		if (sw != NULL) {
			wait_ref(sw);
			[s stopCaptureWithCompletionHandler:^(NSError *e) {
				(void)e;
				wait_finish(sw, nil, NULL, nil);
				wait_unref(sw);
			}];
			uint64_t now = os_monotonic_get_ns();
			wait_until(sw, (deadline_ns > now ? deadline_ns : now) + 500ull * 1000 * 1000);
			wait_unref(sw);
		}
		[s removeStreamOutput:out type:SCStreamOutputTypeScreen error:nil];
	}
	[s release];
	[out release];
	dispatch_release(q);
	return w;
}

/*
 *
 * Entry points.
 *
 */

bool
u_desktop_macos_sck_available(void)
{
	if (@available(macOS 12.3, *)) {
		// Weak-linked: the class is nil when the framework is absent.
		return NSClassFromString(@"SCShareableContent") != nil && NSClassFromString(@"SCStream") != nil;
	}
	return false;
}

void
u_desktop_macos_sck_force_stream(bool on)
{
	g_force_stream = on;
}

enum u_desktop_result
u_desktop_macos_sck_grab(const struct u_desktop_rect *src,
                         const struct u_desktop_macos_display *displays,
                         const struct u_desktop_macos_capture_part *parts,
                         uint32_t nparts,
                         struct u_desktop_image *img,
                         uint64_t deadline_ns,
                         char *msg)
{
	if (!u_desktop_macos_sck_available()) {
		cap_msg(msg, "ScreenCaptureKit is not available on this macOS");
		return U_DESKTOP_ERR_UNSUPPORTED;
	}
	if (@available(macOS 12.3, *)) {
		enum u_desktop_result r = U_DESKTOP_OK;
		uint32_t drawn = 0;
		@autoreleasepool {
			SCShareableContent *content = fetch_content(deadline_ns, &r, msg);
			if (content == nil) {
				return r;
			}
			// The calling process's applications (an app may own more than one
			// SCRunningApplication entry); its windows are withheld by the caller
			// anyway, this keeps them out of the picture underneath too.
			NSMutableArray *own = [NSMutableArray array];
			pid_t me = getpid();
			for (SCRunningApplication *a in content.applications) {
				if (a.processID == me) {
					[own addObject:a];
				}
			}

			for (uint32_t i = 0; i < nparts && r == U_DESKTOP_OK; i++) {
				const struct u_desktop_macos_capture_part *p = &parts[i];
				SCDisplay *disp = nil;
				for (SCDisplay *d in content.displays) {
					if (d.displayID == displays[p->display].id) {
						disp = d;
						break;
					}
				}
				if (disp == nil) {
					continue; // a display ScreenCaptureKit does not offer: stays withheld
				}
				SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:disp
				                                             excludingApplications:own
				                                                  exceptingWindows:@[]];
				SCStreamConfiguration *cfg = make_config(p);
				struct sck_wait *w = grab_frame(filter, cfg, deadline_ns);
				[cfg release];
				[filter release];
				if (w == NULL) {
					r = U_DESKTOP_ERR_FAILED;
					cap_msg(msg, "out of memory");
					break;
				}
				pthread_mutex_lock(&w->mu);
				bool done = w->done;
				long err = w->err;
				bool declined = w->declined;
				CVPixelBufferRef pb = w->pb != NULL ? CVPixelBufferRetain(w->pb) : NULL;
				pthread_mutex_unlock(&w->mu);
				wait_unref(w);

				if (!done) {
					r = U_DESKTOP_ERR_TIMEOUT;
					cap_msg(msg, "ScreenCaptureKit returned no frame in time");
				} else if (declined) {
					r = U_DESKTOP_ERR_NOT_GRANTED;
					cap_msg(msg, "Screen Recording permission is not granted");
				} else if (err != 0 || pb == NULL) {
					r = U_DESKTOP_ERR_FAILED;
					cap_msg(msg, "ScreenCaptureKit could not capture the display (error %ld)", err);
				} else if (CVPixelBufferGetPixelFormatType(pb) != kCVPixelFormatType_32BGRA ||
				           CVPixelBufferIsPlanar(pb)) {
					r = U_DESKTOP_ERR_FAILED;
					cap_msg(msg, "ScreenCaptureKit returned an unexpected pixel format");
				} else if (CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly) ==
				           kCVReturnSuccess) {
					const uint8_t *base = CVPixelBufferGetBaseAddress(pb);
					size_t fw = CVPixelBufferGetWidth(pb), fh = CVPixelBufferGetHeight(pb);
					size_t bpr = CVPixelBufferGetBytesPerRow(pb);
					if (base != NULL && fw > 0 && fh > 0 && bpr >= fw * 4) {
						u_desktop_image_draw_frame(img, src, &p->rect, base, (uint32_t)fw, (uint32_t)fh,
						                           (uint32_t)bpr, U_DESKTOP_PIXFMT_BGRA);
						drawn++;
					}
					CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
				} else {
					r = U_DESKTOP_ERR_FAILED;
					cap_msg(msg, "could not read the captured frame");
				}
				if (pb != NULL) {
					CVPixelBufferRelease(pb);
				}
			}
			[content release];
		}
		if (r == U_DESKTOP_OK && drawn == 0) {
			r = U_DESKTOP_ERR_FAILED;
			cap_msg(msg, "ScreenCaptureKit offers none of the displays in the region");
		}
		return r;
	}
	cap_msg(msg, "ScreenCaptureKit is not available on this macOS");
	return U_DESKTOP_ERR_UNSUPPORTED;
}
