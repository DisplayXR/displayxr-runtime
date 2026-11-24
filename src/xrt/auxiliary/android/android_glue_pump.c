// Copyright 2026, DisplayXR
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Keep an app's native_app_glue command queue moving while the runtime
 *         blocks the glue's own thread (#2087, #1358 bug 1).
 * @ingroup aux_android
 */

#include "android_glue_pump.h"

#ifdef XRT_OS_ANDROID

#include "util/u_logging.h"

#include <android/looper.h>
#include <sys/system_properties.h>

#include <stddef.h>

/*
 * The glue is compiled into the APP, not into the runtime, so its structs are
 * not ours to include. Only the prefix below is used, and it is identical in
 * the NDK's android_native_app_glue.h and in AGDK's GameActivity glue:
 *
 *   struct android_poll_source { int32_t id; struct android_app *app;
 *                                void (*process)(app, source); };
 *   struct android_app         { void *userData;
 *                                void (*onAppCmd)(app, int32_t cmd); … };
 *
 * The command source is registered on android_main's looper with ident
 * LOOPER_ID_MAIN (1) and is a member of `struct android_app` itself, which is
 * what glue_source_plausible() checks before anything is called through it.
 */
struct glue_app;

struct glue_poll_source
{
	int32_t id;
	struct glue_app *app;
	void (*process)(struct glue_app *app, struct glue_poll_source *source);
};

struct glue_app
{
	void *userData;
	void (*onAppCmd)(struct glue_app *app, int32_t cmd);
};

#define GLUE_LOOPER_ID_MAIN 1

// Same value in both glue flavours.
#define GLUE_CMD_INIT_WINDOW 1
#define GLUE_CMD_TERM_WINDOW 2
// NDK glue: 15 = DESTROY. GameActivity glue: 15 = STOP, 16 = DESTROY.
#define GLUE_CMD_TEARDOWN_A 15
#define GLUE_CMD_TEARDOWN_B 16

// Upper bound on sizeof(struct android_app) in either flavour, generously.
#define GLUE_APP_MAX_SIZE 8192

// How many looper events one pass looks at. An unprocessed input source is
// level-triggered and comes back on every poll, so the pass must be bounded.
#define GLUE_MAX_EVENTS_PER_PASS 8

struct pump_state
{
	void (*app_handler)(struct glue_app *app, int32_t cmd);
	struct android_glue_pump_result *result;
};

static __thread struct pump_state t_pump;

static bool
glue_source_plausible(const struct glue_poll_source *src)
{
	if (src == NULL || src->id != GLUE_LOOPER_ID_MAIN || src->app == NULL || src->process == NULL) {
		return false;
	}
	const uintptr_t s = (uintptr_t)src;
	const uintptr_t a = (uintptr_t)src->app;
	return s >= a && s - a < GLUE_APP_MAX_SIZE;
}

/*
 * Stands in for the app's onAppCmd while the glue processes one command, so
 * the command number is known before the app's handler runs.
 */
static void
pump_on_app_cmd(struct glue_app *app, int32_t cmd)
{
	struct android_glue_pump_result *r = t_pump.result;

	switch (cmd) {
	case GLUE_CMD_INIT_WINDOW:
		r->withheld++;
		U_LOG_W("glue pump (#2087): INIT_WINDOW arrived inside a runtime call — not forwarded to the app");
		return;
	case GLUE_CMD_TEARDOWN_A:
	case GLUE_CMD_TEARDOWN_B:
		r->withheld++;
		r->teardown = true;
		U_LOG_W(
		    "glue pump (#2087): command %d (stop/destroy) arrived inside a runtime call — not "
		    "forwarded to the app; the runtime call fails",
		    cmd);
		return;
	case GLUE_CMD_TERM_WINDOW: r->teardown = true; break;
	default: break;
	}

	if (t_pump.app_handler != NULL) {
		t_pump.app_handler(app, cmd);
	}
}

bool
android_glue_pump_enabled(void)
{
	static int cached = -1;
	if (cached < 0) {
		char value[PROP_VALUE_MAX] = {0};
		const bool off = __system_property_get("debug.dxr.glue_pump", value) > 0 && value[0] == '0';
		cached = off ? 0 : 1;
		if (off) {
			U_LOG_W("glue pump (#2087): DISABLED (debug.dxr.glue_pump=%s)", value);
		}
	}
	return cached == 1;
}

struct android_glue_pump_result
android_glue_pump_once(void)
{
	struct android_glue_pump_result result = {0};

	if (!android_glue_pump_enabled() || ALooper_forThread() == NULL) {
		return result;
	}
	// A nested pass (a handler we forwarded to called back into the runtime).
	if (t_pump.result != NULL) {
		return result;
	}

	for (int i = 0; i < GLUE_MAX_EVENTS_PER_PASS && !result.teardown; i++) {
		int events = 0;
		void *data = NULL;
		const int ident = ALooper_pollOnce(0, NULL, &events, &data);
		if (ident < 0) {
			// Nothing pending (TIMEOUT / WAKE), or callbacks ran (CALLBACK), or ERROR.
			if (ident == ALOOPER_POLL_CALLBACK) {
				continue;
			}
			break;
		}
		if (ident != GLUE_LOOPER_ID_MAIN) {
			// Input (or an fd of the app's own): left for the app's loop.
			continue;
		}
		struct glue_poll_source *src = (struct glue_poll_source *)data;
		if (!glue_source_plausible(src)) {
			break;
		}

		struct glue_app *app = src->app;
		t_pump.app_handler = app->onAppCmd;
		t_pump.result = &result;
		app->onAppCmd = pump_on_app_cmd;

		src->process(app, src);

		app->onAppCmd = t_pump.app_handler;
		t_pump.app_handler = NULL;
		t_pump.result = NULL;

		if (result.processed++ == 0) {
			static bool logged = false;
			if (!logged) {
				logged = true;
				U_LOG_W(
				    "glue pump (#2087): servicing the app glue's command queue during a "
				    "blocking runtime call (first time this process)");
			}
		}
	}

	return result;
}

#endif // XRT_OS_ANDROID
