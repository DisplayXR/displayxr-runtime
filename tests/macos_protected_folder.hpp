// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Test helper (#2198): is a helper binary inside one of the macOS
 *         privacy-protected user folders (~/Documents, ~/Desktop, ~/Downloads)?
 *
 * A child started with `disclaim_responsibility` (os_process.h) is its own
 * "responsible process", so it does not inherit the terminal's grant for those
 * folders. When the build tree lives in one, the child stalls in dyld opening
 * its own executable (waiting on a consent that a test run cannot answer) and
 * the test times out. That is a property of where the tree was built, not of
 * the code under test: the same sources built elsewhere pass. Tests that spawn
 * a disclaimed helper SKIP in that one situation instead of failing.
 *
 * Always false off macOS, where nothing is disclaimed.
 */

#pragma once

#include <string>

#if defined(__APPLE__)
#include <climits>
#include <cstdlib>
#include <pwd.h>
#include <unistd.h>
#endif

/*!
 * True when the real path of @p exe_path is under ~/Documents, ~/Desktop or
 * ~/Downloads of the current user; @p out_folder (optional) names which.
 */
static inline bool
macos_exe_in_protected_folder(const char *exe_path, std::string *out_folder = nullptr)
{
#if defined(__APPLE__)
	char real_exe[PATH_MAX];
	if (exe_path == nullptr || realpath(exe_path, real_exe) == nullptr) {
		return false;
	}
	// The account's home, not $HOME: a test may have redirected the latter.
	const struct passwd *pw = getpwuid(getuid());
	if (pw == nullptr || pw->pw_dir == nullptr) {
		return false;
	}
	const std::string exe = real_exe;
	for (const char *leaf : {"Documents", "Desktop", "Downloads"}) {
		char real_dir[PATH_MAX];
		const std::string dir = std::string(pw->pw_dir) + "/" + leaf;
		if (realpath(dir.c_str(), real_dir) == nullptr) {
			continue;
		}
		const std::string prefix = std::string(real_dir) + "/";
		if (exe.compare(0, prefix.size(), prefix) == 0) {
			if (out_folder != nullptr) {
				*out_folder = std::string("~/") + leaf;
			}
			return true;
		}
	}
	return false;
#else
	(void)exe_path;
	(void)out_folder;
	return false;
#endif
}
