// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The minimal native message of the `displayxr://activate` handler
 *         (see activation_deliver.h): the user always sees which component
 *         was activated, or that nothing was.
 *
 * The text comes from @ref activation_outcome_message: fixed sentences, the
 * component id from the PENDING RECORD, and a component's own line when it is
 * well-formed text. Nothing from the link reaches a window or a command line.
 * On Linux the helper is run by absolute path with an argument vector
 * (posix_spawn), never through a shell, and "--" ends option parsing before
 * any text argument.
 *
 * @ingroup targets
 */

#include "activation_deliver.h"

#include "xrt/xrt_config_os.h"

#include <string.h>

#if defined(XRT_OS_WINDOWS)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

bool
activation_notify_available(void)
{
	return true;
}

bool
activation_notify(const struct activation_outcome *o)
{
	char title[96];
	char body[1024];
	wchar_t wtitle[96];
	wchar_t wbody[1024];
	activation_outcome_message(o, title, sizeof(title), body, sizeof(body));
	if (MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, (int)(sizeof(wtitle) / sizeof(wtitle[0]))) <= 0 ||
	    MultiByteToWideChar(CP_UTF8, 0, body, -1, wbody, (int)(sizeof(wbody) / sizeof(wbody[0]))) <= 0) {
		return false;
	}
	const UINT icon = o->kind == ACTIVATION_OUTCOME_ACTIVATED ? MB_ICONINFORMATION : MB_ICONWARNING;
	(void)MessageBoxW(NULL, wbody, wtitle, MB_OK | icon | MB_SETFOREGROUND | MB_TOPMOST);
	return true;
}

#elif defined(XRT_OS_LINUX_DESKTOP)

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

//! The first of the fixed, root-owned locations that holds an executable @p name. No PATH search.
static bool
find_helper(const char *name, char *out, size_t cap)
{
	static const char *const dirs[] = {"/usr/bin", "/bin", "/usr/local/bin"};
	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		const size_t need = strlen(dirs[i]) + 1 + strlen(name) + 1;
		if (need > cap) {
			continue;
		}
		strcpy(out, dirs[i]);
		strcat(out, "/");
		strcat(out, name);
		if (access(out, X_OK) == 0) {
			return true;
		}
	}
	return false;
}

bool
activation_notify_available(void)
{
	char path[64];
	return find_helper("notify-send", path, sizeof(path)) || find_helper("zenity", path, sizeof(path)) ||
	       find_helper("kdialog", path, sizeof(path));
}

bool
activation_notify(const struct activation_outcome *o)
{
	char title[96];
	char body[1024];
	char path[64];
	activation_outcome_message(o, title, sizeof(title), body, sizeof(body));
	char *argv[8];
	bool wait = true;
	if (find_helper("notify-send", path, sizeof(path))) {
		argv[0] = path;
		argv[1] = "--app-name=DisplayXR";
		argv[2] = "--";
		argv[3] = title;
		argv[4] = body;
		argv[5] = NULL;
	} else if (find_helper("zenity", path, sizeof(path))) {
		argv[0] = path;
		argv[1] = "--info";
		argv[2] = "--no-markup";
		argv[3] = "--title";
		argv[4] = title;
		argv[5] = "--text";
		argv[6] = body;
		argv[7] = NULL;
		wait = false; // a modal box: do not hold the handler on it
	} else if (find_helper("kdialog", path, sizeof(path))) {
		argv[0] = path;
		argv[1] = "--title";
		argv[2] = title;
		argv[3] = "--msgbox";
		argv[4] = body;
		argv[5] = NULL;
		wait = false;
	} else {
		return false;
	}
	pid_t pid = 0;
	if (posix_spawn(&pid, path, NULL, NULL, argv, environ) != 0) {
		return false;
	}
	if (wait) {
		int status = 0;
		(void)waitpid(pid, &status, 0);
	}
	return true;
}

#else

bool
activation_notify_available(void)
{
	return false;
}

bool
activation_notify(const struct activation_outcome *o)
{
	(void)o;
	return false;
}

#endif
