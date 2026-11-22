// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  DisplayXRActivate.app: the macOS handler of the `displayxr://` URL
 *         scheme (ADR-064 §5, `docs/specs/runtime/activation-deep-link.md`).
 *
 * macOS routes a URL scheme to an APP BUNDLE whose Info.plist declares it
 * (CFBundleURLTypes) and delivers the link as an Apple Event, not as an
 * argument: a bare executable cannot be a scheme handler. This bundle is that
 * handler and nothing else: LSUIElement (no Dock icon, no menu bar), system
 * frameworks only, no entitlements, no window but one alert.
 *
 * It does not handle the link itself. It runs the `displayxr-cli` that sits
 * beside the bundle, `displayxr-cli activate-url --json -` with the link on
 * its standard input, and shows
 * the title and message that command reports. So there is ONE handler
 * implementation on every platform (activation_handle_url), and the process
 * that loads third-party plug-ins is the CLI, which already carries the
 * signing entitlements for that; this bundle loads nothing.
 *
 * Rules:
 *  - one link per launch; several at once is treated as an invalid link;
 *  - the CLI is found ONLY beside the bundle (no PATH, no environment), and
 *    is started with an argument vector (NSTask): there is no shell;
 *  - the link reaches the CLI on its STANDARD INPUT, never as an argument:
 *    the link arrived here privately (an Apple Event), and an argument would
 *    show it, token included, to every user of the machine in `ps` for as long
 *    as the CLI runs;
 *  - nothing of the link is shown, opened or logged here;
 *  - the alert always appears unless the CLI reports `quiet` (a failed link
 *    among a flood of failed links): no silent activation;
 *  - launched without a link (a double click in Finder), it quits.
 *
 * @ingroup targets
 */

#import <AppKit/AppKit.h>

#include <signal.h>

//! Longest link handed on, in UTF-8 bytes (U_ACTIVATION_URL_MAX of util/u_activation_url.h).
static const NSUInteger kMaxLinkBytes = 6400;
//! The CLI answers within its own bounds (a live delivery is given 45 s); this is the backstop.
static const NSTimeInterval kCliTimeoutS = 75.0;

@interface DXRActivateDelegate : NSObject <NSApplicationDelegate>
@property (nonatomic) BOOL handled;
@end

@implementation DXRActivateDelegate

- (void)applicationDidFinishLaunching:(NSNotification *)note
{
	(void)note;
	// No link arrived with the launch: there is nothing for this app to do.
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(3 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
	  if (!self.handled) {
		  [NSApp terminate:nil];
	  }
	});
}

//! Run the sibling `displayxr-cli activate-url --json -` with @p link on its stdin; its JSON answer, or nil.
- (NSDictionary *)runHandler:(NSString *)link
{
	NSString *cli = [[[[NSBundle mainBundle] bundlePath] stringByDeletingLastPathComponent]
	    stringByAppendingPathComponent:@"displayxr-cli"];
	if (![[NSFileManager defaultManager] isExecutableFileAtPath:cli]) {
		return nil;
	}
	NSTask *task = [[NSTask alloc] init];
	task.executableURL = [NSURL fileURLWithPath:cli];
	task.arguments = @[ @"activate-url", @"--json", @"-" ]; // no shell; "-" = the link is on stdin, not in `ps`
	NSPipe *out = [NSPipe pipe];
	NSPipe *in = [NSPipe pipe];
	task.standardOutput = out;
	task.standardError = [NSFileHandle fileHandleWithNullDevice];
	task.standardInput = in;
	NSError *err = nil;
	if (![task launchAndReturnError:&err]) {
		return nil;
	}
	// The link is at most kMaxLinkBytes, well under a pipe's buffer, so this
	// write cannot block on a CLI that is not reading yet. A CLI that already
	// exited makes it fail (SIGPIPE is ignored, see main), which the empty
	// answer below then reports.
	NSData *bytes = [link dataUsingEncoding:NSUTF8StringEncoding];
	if (bytes != nil) {
		(void)[[in fileHandleForWriting] writeData:bytes error:nil];
	}
	(void)[[in fileHandleForWriting] closeAndReturnError:nil];
	// Bounded: a CLI that never answers is ended, and the user is told nothing was confirmed.
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(kCliTimeoutS * NSEC_PER_SEC)),
	               dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
		             if (task.isRunning) {
			             [task terminate];
		             }
	               });
	NSData *data = [[out fileHandleForReading] readDataToEndOfFile];
	[task waitUntilExit];
	if (data.length == 0 || data.length > 65536) {
		return nil;
	}
	id json = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
	return [json isKindOfClass:[NSDictionary class]] ? json : nil;
}

- (void)application:(NSApplication *)application openURLs:(NSArray<NSURL *> *)urls
{
	(void)application;
	if (self.handled) {
		return; // one link per launch
	}
	self.handled = YES;

	// Exactly one link, of a sane size. Anything else is handed on as an
	// empty link, which the handler refuses (and counts) as invalid.
	NSString *link = @"";
	if (urls.count == 1) {
		NSString *s = urls[0].absoluteString;
		if (s != nil && [s lengthOfBytesUsingEncoding:NSUTF8StringEncoding] <= kMaxLinkBytes) {
			link = s;
		}
	}

	NSDictionary *res = [self runHandler:link];
	NSString *title = @"DisplayXR: activation not completed";
	NSString *message = @"DisplayXR could not handle this activation link. Nothing was confirmed; check the "
	                    @"component's licence state in the DisplayXR Dashboard before trying again.";
	BOOL quiet = NO;
	BOOL accepted = NO;
	if (res != nil) {
		id t = res[@"title"];
		id m = res[@"message"];
		if ([t isKindOfClass:[NSString class]] && [m isKindOfClass:[NSString class]]) {
			title = t;
			message = m;
		}
		quiet = [res[@"quiet"] isKindOfClass:[NSNumber class]] && [res[@"quiet"] boolValue];
		accepted = [res[@"accepted"] isKindOfClass:[NSNumber class]] && [res[@"accepted"] boolValue];
	}
	if (!quiet) {
		NSAlert *alert = [[NSAlert alloc] init];
		alert.messageText = title;
		alert.informativeText = message;
		alert.alertStyle = accepted ? NSAlertStyleInformational : NSAlertStyleWarning;
		[alert addButtonWithTitle:@"OK"];
		[NSApp activateIgnoringOtherApps:YES];
		[alert runModal];
	}
	[NSApp terminate:nil];
}

@end

int
main(int argc, const char *argv[])
{
	(void)argc;
	(void)argv; // a scheme handler gets its link as an Apple Event; arguments are ignored
	signal(SIGPIPE, SIG_IGN); // writing the link to a CLI that died must not kill this app
	@autoreleasepool {
		NSApplication *app = [NSApplication sharedApplication];
		DXRActivateDelegate *delegate = [[DXRActivateDelegate alloc] init];
		app.delegate = delegate;
		[app setActivationPolicy:NSApplicationActivationPolicyAccessory];
		[app run];
	}
	return 0;
}
