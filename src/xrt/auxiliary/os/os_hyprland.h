// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Hyprland (Omarchy) IPC: the compositor's request and event sockets,
 *         and the parts of their replies the runtime reads.
 *
 * Hyprland serves two UNIX sockets per instance, in
 * `$XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/` (`/tmp/hypr/<sig>/`
 * before Hyprland 0.40):
 *
 * - `.socket.sock`: one request per connection (`j/clients`, `monitors`, ...),
 *   the reply is everything read until the compositor closes it;
 * - `.socket2.sock`: an event stream, one `EVENT>>DATA` line per event.
 *
 * Used by the panel lookup (os_display_connector_linux.c: connector modes,
 * scale and transform without Mutter) and by displayxr-hypr-geometry-bridge
 * (org.displayxr.WindowGeometry1 on Hyprland). See
 * docs/roadmap/omarchy-support.md.
 *
 * The parsers here are pure and build on every platform (unit tests); the
 * socket functions exist on desktop Linux only.
 *
 * @ingroup aux_os
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! One output, as `hyprctl monitors` (plain text) describes it.
struct os_hyprland_monitor
{
	char name[64];        //!< connector, e.g. "DP-1"
	uint32_t mode_w;      //!< current mode, scanout orientation (not turned)
	uint32_t mode_h;      //!< current mode, scanout orientation (not turned)
	uint32_t refresh_mhz; //!< 0 = unknown
	int32_t x, y;         //!< layout position, logical px
	double scale;         //!< fractional scale; 0 = unknown
	uint32_t transform;   //!< wl_output transform 0-7; UINT32_MAX = unknown
	bool disabled;
};

/*!
 * Parse the plain-text reply to the `monitors` request. Lines look like:
 *
 *     Monitor DP-1 (ID 0):
 *     	2560x1600@60.00000 at 0x0
 *     	...
 *     	scale: 1.00
 *     	transform: 0
 *     	disabled: false
 *
 * Plain text, not `j/monitors`, because aux_os has no JSON parser; the format
 * has been stable since 2022. Fields it cannot read stay unknown.
 *
 * @return how many monitors were written (at most @p max).
 */
uint32_t
os_hyprland_parse_monitors_text(const char *text, struct os_hyprland_monitor *out, uint32_t max);

/*!
 * Can this socket2 event change any window's or output's geometry or
 * visibility? Takes one `EVENT>>DATA` line (or just the event name).
 * Unknown events count as yes: a spare re-query costs one request, and a
 * missed one leaves a stale rect.
 */
bool
os_hyprland_event_moves_windows(const char *line);

/*!
 * Is this a Hyprland session? An entry "Hyprland" in XDG_CURRENT_DESKTOP, or a
 * HYPRLAND_INSTANCE_SIGNATURE. Either may be NULL.
 */
bool
os_hyprland_session_detected(const char *xdg_current_desktop, const char *instance_signature);


/*
 *
 * Sockets (desktop Linux only).
 *
 */

/*!
 * The live instance's socket directory. Uses HYPRLAND_INSTANCE_SIGNATURE;
 * without it (a systemd user unit that was started before the compositor
 * exported it), the only directory under `$XDG_RUNTIME_DIR/hypr/` whose
 * request socket accepts a connection. Returns false when none is live.
 */
bool
os_hyprland_socket_dir(char *out, size_t out_size);

/*!
 * Send one request on `.socket.sock` and read the whole reply. Returns a
 * NUL-terminated string to release with free(), or NULL (no Hyprland,
 * timeout, or a reply over 8 MiB). Never blocks longer than @p timeout_ms per
 * read.
 */
char *
os_hyprland_request(const char *dir, const char *request, int timeout_ms);

//! Connect to `.socket2.sock`. Returns the fd (non-blocking, close-on-exec) or -1.
int
os_hyprland_event_connect(const char *dir);

#ifdef __cplusplus
}
#endif
