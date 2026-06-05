// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Shared capture-intent polling for in-process compositors.
 *
 * Each in-process compositor (d3d11, d3d12, gl, metal, vk_native) wants
 * to expose two capture surfaces — a trigger-file path for devs and
 * the MCP @c capture_frame tool — and two modes per surface:
 *  - @c POST_COMPOSE: full atlas (projection + window-space + quads),
 *    captured at end of frame.
 *  - @c PROJECTION_ONLY: atlas state before window-space layers are
 *    composed in, captured mid-frame at the projection-done boundary.
 *
 * This header bundles the per-frame intent struct + the polling helper
 * so each compositor can ship the same behavior with one struct field
 * and two function calls.
 *
 * Usage:
 *   1. Add @ref u_capture_intent as a field on the compositor struct.
 *   2. Call @ref u_capture_intent_poll once at the top of layer_commit.
 *   3. At the projection-done boundary call
 *      @ref u_capture_intent_should_capture with mode=PROJECTION_ONLY;
 *      if true, run the compositor's capture_atlas_to_png and then
 *      @ref u_capture_intent_complete.
 *   4. At end of frame, same with mode=POST_COMPOSE.
 *
 * Caller owns the @c mcp_capture_request used for the MCP path; this
 * helper just forwards to @ref mcp_capture_poll / _complete.
 *
 * @ingroup aux_util
 */

#pragma once

#include "displayxr_mcp/mcp_capture.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef XRT_OS_WINDOWS
#include <windows.h>
#else
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Per-frame "the user asked for a capture" record. Set by
 * @ref u_capture_intent_poll at the top of a frame; consumed by
 * @ref u_capture_intent_should_capture at the right boundary.
 */
struct u_capture_intent
{
	bool pending;
	uint32_t mode; // enum mcp_capture_mode
	char path[MCP_CAPTURE_PATH_MAX];
	bool from_mcp; // true → call mcp_capture_complete on done
};

/*!
 * Poll for a pending capture this frame. Checks the MCP request first
 * (highest priority — there's a blocked client thread waiting), then
 * the two trigger files. Returns silently if nothing is requested.
 *
 * Trigger filenames (relative to %TEMP% / $TMPDIR):
 *   - "displayxr_atlas_trigger"            → POST_COMPOSE
 *     output: "displayxr_atlas.png"
 *   - "displayxr_atlas_trigger.projection" → PROJECTION_ONLY
 *     output: "displayxr_atlas.projection.png"
 *
 * Per-exe variants (#433 follow-up) — when several DisplayXR apps run at
 * once they all race the global trigger files above (first poller wins);
 * suffix the trigger with the process basename (no ".exe") to target one:
 *   - "displayxr_atlas_trigger.<exe>"            → POST_COMPOSE
 *     output: "displayxr_atlas.<exe>.png"
 *   - "displayxr_atlas_trigger.<exe>.projection" → PROJECTION_ONLY
 *     output: "displayxr_atlas.<exe>.projection.png"
 * e.g. "displayxr_atlas_trigger.cube_texture_d3d12_win.projection".
 * A per-exe trigger always beats the global one; outputs are per-exe so
 * concurrent apps can't clobber each other's PNGs.
 */
static inline void
u_capture_intent_poll(struct u_capture_intent *intent,
                       struct mcp_capture_request *mcp_req)
{
	intent->pending = false;
	intent->from_mcp = false;
	intent->mode = MCP_CAPTURE_MODE_POST_COMPOSE;
	intent->path[0] = '\0';

	if (mcp_req != NULL) {
		char mcp_path[MCP_CAPTURE_PATH_MAX];
		uint32_t mcp_mode = MCP_CAPTURE_MODE_POST_COMPOSE;
		if (mcp_capture_poll(mcp_req, mcp_path, &mcp_mode)) {
			intent->pending = true;
			intent->from_mcp = true;
			intent->mode = mcp_mode;
			memcpy(intent->path, mcp_path, sizeof(mcp_path));
			return;
		}
	}

	// Trigger-file paths cached once per process — getenv is cheap but
	// the formatted strings are stable for the lifetime of the runtime.
	static char tg_post[MCP_CAPTURE_PATH_MAX] = {0};
	static char tg_proj[MCP_CAPTURE_PATH_MAX] = {0};
	static char out_post[MCP_CAPTURE_PATH_MAX] = {0};
	static char out_proj[MCP_CAPTURE_PATH_MAX] = {0};
	// Per-exe variants (#433 follow-up) — empty (never polled) when the
	// process basename can't be resolved.
	static char tg_post_exe[MCP_CAPTURE_PATH_MAX] = {0};
	static char tg_proj_exe[MCP_CAPTURE_PATH_MAX] = {0};
	static char out_post_exe[MCP_CAPTURE_PATH_MAX] = {0};
	static char out_proj_exe[MCP_CAPTURE_PATH_MAX] = {0};
	if (tg_post[0] == '\0') {
		// Process basename without a trailing ".exe" — matches the
		// per-app log filename convention so the suffix is guessable
		// from %LOCALAPPDATA%\DisplayXR\DisplayXR_<exe>.*.log.
		char exe[128] = {0};
#ifdef XRT_OS_WINDOWS
		{
			char exe_path[MAX_PATH];
			if (GetModuleFileNameA(NULL, exe_path, MAX_PATH) > 0) {
				const char *base = strrchr(exe_path, '\\');
				base = base != NULL ? base + 1 : exe_path;
				snprintf(exe, sizeof(exe), "%s", base);
				size_t elen = strlen(exe);
				if (elen > 4 && _stricmp(exe + elen - 4, ".exe") == 0) {
					exe[elen - 4] = '\0';
				}
			}
		}
		const char *tmp = getenv("TEMP");
		if (tmp == NULL || tmp[0] == '\0') {
			tmp = "C:\\Temp";
		}
		snprintf(tg_post, sizeof(tg_post), "%s\\displayxr_atlas_trigger", tmp);
		snprintf(tg_proj, sizeof(tg_proj), "%s\\displayxr_atlas_trigger.projection", tmp);
		snprintf(out_post, sizeof(out_post), "%s\\displayxr_atlas.png", tmp);
		snprintf(out_proj, sizeof(out_proj), "%s\\displayxr_atlas.projection.png", tmp);
		if (exe[0] != '\0') {
			snprintf(tg_post_exe, sizeof(tg_post_exe), "%s\\displayxr_atlas_trigger.%s", tmp, exe);
			snprintf(tg_proj_exe, sizeof(tg_proj_exe), "%s\\displayxr_atlas_trigger.%s.projection", tmp,
			         exe);
			snprintf(out_post_exe, sizeof(out_post_exe), "%s\\displayxr_atlas.%s.png", tmp, exe);
			snprintf(out_proj_exe, sizeof(out_proj_exe), "%s\\displayxr_atlas.%s.projection.png", tmp,
			         exe);
		}
#else
#if defined(__APPLE__)
		snprintf(exe, sizeof(exe), "%s", getprogname());
#elif defined(__linux__)
		{
			char lnk[256];
			ssize_t n = readlink("/proc/self/exe", lnk, sizeof(lnk) - 1);
			if (n > 0) {
				lnk[n] = '\0';
				const char *base = strrchr(lnk, '/');
				snprintf(exe, sizeof(exe), "%s", base != NULL ? base + 1 : lnk);
			}
		}
#endif
		const char *tmp = getenv("TMPDIR");
		if (tmp == NULL || tmp[0] == '\0') {
			tmp = "/tmp";
		}
		// Trim a trailing slash so we don't end up with double slashes.
		size_t tlen = strlen(tmp);
		char tmp_clean[MCP_CAPTURE_PATH_MAX];
		size_t copy = tlen < sizeof(tmp_clean) - 1 ? tlen : sizeof(tmp_clean) - 1;
		memcpy(tmp_clean, tmp, copy);
		tmp_clean[copy] = '\0';
		while (copy > 0 && tmp_clean[copy - 1] == '/') {
			tmp_clean[--copy] = '\0';
		}
		snprintf(tg_post, sizeof(tg_post), "%s/displayxr_atlas_trigger", tmp_clean);
		snprintf(tg_proj, sizeof(tg_proj), "%s/displayxr_atlas_trigger.projection", tmp_clean);
		snprintf(out_post, sizeof(out_post), "%s/displayxr_atlas.png", tmp_clean);
		snprintf(out_proj, sizeof(out_proj), "%s/displayxr_atlas.projection.png", tmp_clean);
		if (exe[0] != '\0') {
			snprintf(tg_post_exe, sizeof(tg_post_exe), "%s/displayxr_atlas_trigger.%s", tmp_clean, exe);
			snprintf(tg_proj_exe, sizeof(tg_proj_exe), "%s/displayxr_atlas_trigger.%s.projection",
			         tmp_clean, exe);
			snprintf(out_post_exe, sizeof(out_post_exe), "%s/displayxr_atlas.%s.png", tmp_clean, exe);
			snprintf(out_proj_exe, sizeof(out_proj_exe), "%s/displayxr_atlas.%s.projection.png",
			         tmp_clean, exe);
		}
#endif
	}

	// Per-exe triggers beat the global ones (they were aimed at THIS
	// process); within each scope projection-only takes priority when
	// both files exist (rare race).
#ifdef XRT_OS_WINDOWS
	if (tg_proj_exe[0] != '\0' && GetFileAttributesA(tg_proj_exe) != INVALID_FILE_ATTRIBUTES) {
		DeleteFileA(tg_proj_exe);
		intent->pending = true;
		intent->mode = MCP_CAPTURE_MODE_PROJECTION_ONLY;
		memcpy(intent->path, out_proj_exe, sizeof(out_proj_exe));
		return;
	}
	if (tg_post_exe[0] != '\0' && GetFileAttributesA(tg_post_exe) != INVALID_FILE_ATTRIBUTES) {
		DeleteFileA(tg_post_exe);
		intent->pending = true;
		intent->mode = MCP_CAPTURE_MODE_POST_COMPOSE;
		memcpy(intent->path, out_post_exe, sizeof(out_post_exe));
		return;
	}
	if (GetFileAttributesA(tg_proj) != INVALID_FILE_ATTRIBUTES) {
		DeleteFileA(tg_proj);
		intent->pending = true;
		intent->mode = MCP_CAPTURE_MODE_PROJECTION_ONLY;
		memcpy(intent->path, out_proj, sizeof(out_proj));
		return;
	}
	if (GetFileAttributesA(tg_post) != INVALID_FILE_ATTRIBUTES) {
		DeleteFileA(tg_post);
		intent->pending = true;
		intent->mode = MCP_CAPTURE_MODE_POST_COMPOSE;
		memcpy(intent->path, out_post, sizeof(out_post));
		return;
	}
#else
	struct stat st;
	if (tg_proj_exe[0] != '\0' && stat(tg_proj_exe, &st) == 0) {
		unlink(tg_proj_exe);
		intent->pending = true;
		intent->mode = MCP_CAPTURE_MODE_PROJECTION_ONLY;
		memcpy(intent->path, out_proj_exe, sizeof(out_proj_exe));
		return;
	}
	if (tg_post_exe[0] != '\0' && stat(tg_post_exe, &st) == 0) {
		unlink(tg_post_exe);
		intent->pending = true;
		intent->mode = MCP_CAPTURE_MODE_POST_COMPOSE;
		memcpy(intent->path, out_post_exe, sizeof(out_post_exe));
		return;
	}
	if (stat(tg_proj, &st) == 0) {
		unlink(tg_proj);
		intent->pending = true;
		intent->mode = MCP_CAPTURE_MODE_PROJECTION_ONLY;
		memcpy(intent->path, out_proj, sizeof(out_proj));
		return;
	}
	if (stat(tg_post, &st) == 0) {
		unlink(tg_post);
		intent->pending = true;
		intent->mode = MCP_CAPTURE_MODE_POST_COMPOSE;
		memcpy(intent->path, out_post, sizeof(out_post));
		return;
	}
#endif
}

/*!
 * Returns true iff there is a pending capture matching @p mode_filter.
 * Caller should run its capture_atlas_to_png with @c intent->path on
 * true and then call @ref u_capture_intent_complete.
 */
static inline bool
u_capture_intent_should_capture(const struct u_capture_intent *intent, uint32_t mode_filter)
{
	return intent->pending && intent->mode == mode_filter;
}

/*!
 * Mark the intent as consumed and signal the MCP waiter (if any).
 * Pass the success/failure of the PNG write so the MCP client gets
 * an accurate status.
 */
static inline void
u_capture_intent_complete(struct u_capture_intent *intent,
                            struct mcp_capture_request *mcp_req,
                            bool ok)
{
	if (intent->from_mcp && mcp_req != NULL) {
		mcp_capture_complete(mcp_req, ok);
	}
	intent->pending = false;
	intent->from_mcp = false;
}

#ifdef __cplusplus
}
#endif
