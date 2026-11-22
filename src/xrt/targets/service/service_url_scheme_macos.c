// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS: register the `displayxr://` handler bundle with Launch
 *         Services for the user this service runs as (see
 *         service_url_scheme_macos.h).
 * @ingroup targets
 */

#include "service_url_scheme_macos.h"

#include "util/u_logging.h"

#include <CoreServices/CoreServices.h>
#include <mach-o/dyld.h>

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

//! Only an installed runtime registers: a dev build run out of a build or
//! package tree must not take the scheme away from the installed handler.
static const char k_install_bin[] = "/Library/Application Support/DisplayXR/bin/";

void
service_url_scheme_register_macos(void)
{
	char raw[PATH_MAX];
	char exe[PATH_MAX];
	uint32_t cap = (uint32_t)sizeof(raw);
	if (_NSGetExecutablePath(raw, &cap) != 0 || realpath(raw, exe) == NULL) {
		return;
	}
	if (strncmp(exe, k_install_bin, sizeof(k_install_bin) - 1) != 0) {
		return;
	}
	char app[PATH_MAX];
	struct stat st;
	if (snprintf(app, sizeof(app), "%sDisplayXRActivate.app", k_install_bin) >= (int)sizeof(app) ||
	    stat(app, &st) != 0 || !S_ISDIR(st.st_mode)) {
		return; // this install ships no handler (activation by pasted key still works)
	}
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault, (const UInt8 *)app,
	                                                       (CFIndex)strlen(app), true);
	if (url == NULL) {
		return;
	}
	// The Launch Services database is per user, and nothing scans this
	// directory. update=false: a bundle already registered and unchanged costs
	// nothing; a rebuilt database or a user who never logged in before gets it.
	const OSStatus os = LSRegisterURL(url, false);
	CFRelease(url);
	if (os != noErr) {
		U_LOG_W("displayxr:// URL scheme: Launch Services did not register the handler bundle (OSStatus %d).",
		        (int)os);
	}
}
