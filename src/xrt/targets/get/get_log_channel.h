// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The log channel between `dxr-get` and a front end that started it
 *         elevated (`--log-pipe <token>`), writer side.
 *
 * A front end that starts `dxr-get` elevated cannot read its stderr. It gets
 * the messages through a named pipe that THE FRONT END creates and owns:
 * `\\.\pipe\dxr-get-log-<token>`, where the token is 32 lowercase hex digits
 * the front end chose at random. `dxr-get` is told the token, never a path.
 *
 * Why a pipe and not a file: this process may be elevated while its command
 * line came from a process that is not. A file path handed in by the caller
 * would make an elevated process write caller-chosen text to a caller-chosen
 * place. With this channel the elevated process writes to no file at all:
 *  - the name is built HERE, from a token that can only be hex digits, so it
 *    always lies in the named-pipe namespace and never names a file;
 *  - what is opened is checked to be a pipe;
 *  - the pipe is opened with SECURITY_ANONYMOUS, so the (unelevated) process
 *    that owns the other end cannot impersonate this one;
 *  - a write waits a bounded time. A reader that does not read costs one
 *    timeout, after which the caller drops the channel.
 *
 * Header only, with no dependencies, so `dxr-get`, the unit tests and the
 * test stand-in for the engine (tests_cli_catalog_fake_get.c) all use the
 * same code. The POSIX half exists for that stand-in alone: there is no
 * engine, and no elevated start, on macOS and Linux yet. There the channel
 * is a FIFO named by the same token in the caller's temporary directory.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define DXR_LOG_TOKEN_LEN 32
#define DXR_LOG_PIPE_PREFIX "dxr-get-log-"
//! How long one write to the channel may wait for the reader, in milliseconds.
#define DXR_LOG_WRITE_TIMEOUT_MS 2000

//! Exactly 32 lowercase hex digits: nothing that could be a path, a device or a separator.
static inline bool
dxr_log_token_ok(const char *token)
{
	if (token == NULL || strlen(token) != DXR_LOG_TOKEN_LEN) {
		return false;
	}
	for (int i = 0; i < DXR_LOG_TOKEN_LEN; i++) {
		const char c = token[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
			return false;
		}
	}
	return true;
}

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

typedef HANDLE dxr_log_channel_t;
#define DXR_LOG_CHANNEL_NONE INVALID_HANDLE_VALUE

//! `\\.\pipe\dxr-get-log-<token>`, for a token that passed @ref dxr_log_token_ok.
static inline void
dxr_log_channel_name(const char *token, wchar_t out[80])
{
	static const char head[] = "\\\\.\\pipe\\" DXR_LOG_PIPE_PREFIX;
	size_t n = 0;
	for (const char *p = head; *p != '\0'; p++) {
		out[n++] = (wchar_t)*p;
	}
	for (int i = 0; i < DXR_LOG_TOKEN_LEN; i++) {
		out[n++] = (wchar_t)token[i];
	}
	out[n] = L'\0';
}

//! Connect to the front end's pipe. DXR_LOG_CHANNEL_NONE when the token is malformed or nothing listens.
static inline dxr_log_channel_t
dxr_log_channel_open(const char *token)
{
	if (!dxr_log_token_ok(token)) {
		return DXR_LOG_CHANNEL_NONE;
	}
	wchar_t name[80];
	dxr_log_channel_name(token, name);
	HANDLE h = CreateFileW(name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
	                       FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, NULL);
	if (h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_PIPE) {
		CloseHandle(h);
		h = INVALID_HANDLE_VALUE;
	}
	return h;
}

//! Write @p n bytes, waiting at most DXR_LOG_WRITE_TIMEOUT_MS. False: close the channel and stop using it.
static inline bool
dxr_log_channel_write(dxr_log_channel_t h, const char *data, size_t n)
{
	OVERLAPPED ov;
	memset(&ov, 0, sizeof(ov));
	ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (ov.hEvent == NULL) {
		return false;
	}
	DWORD wrote = 0;
	bool ok = true;
	if (!WriteFile(h, data, (DWORD)n, NULL, &ov)) {
		if (GetLastError() != ERROR_IO_PENDING) {
			ok = false;
		} else if (WaitForSingleObject(ov.hEvent, DXR_LOG_WRITE_TIMEOUT_MS) != WAIT_OBJECT_0) {
			// The reader is not reading. Take the write back and give up on the channel.
			(void)CancelIo(h);
			(void)GetOverlappedResult(h, &ov, &wrote, TRUE);
			ok = false;
		} else {
			ok = GetOverlappedResult(h, &ov, &wrote, FALSE) != FALSE;
		}
	}
	CloseHandle(ov.hEvent);
	return ok;
}

static inline void
dxr_log_channel_close(dxr_log_channel_t h)
{
	if (h != INVALID_HANDLE_VALUE) {
		CloseHandle(h);
	}
}

#else // !_WIN32: the test stand-in's half

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

typedef int dxr_log_channel_t;
#define DXR_LOG_CHANNEL_NONE (-1)

//! `$TMPDIR/dxr-get-log-<token>`, for a token that passed @ref dxr_log_token_ok.
static inline void
dxr_log_channel_name(const char *token, char *out, size_t cap)
{
	const char *dir = getenv("TMPDIR");
	if (dir == NULL || dir[0] == '\0') {
		dir = "/tmp";
	}
	const size_t len = strlen(dir);
	(void)snprintf(out, cap, "%s%s" DXR_LOG_PIPE_PREFIX "%s", dir, dir[len - 1] == '/' ? "" : "/", token);
}

static inline dxr_log_channel_t
dxr_log_channel_open(const char *token)
{
	if (!dxr_log_token_ok(token)) {
		return DXR_LOG_CHANNEL_NONE;
	}
	char name[1024];
	dxr_log_channel_name(token, name, sizeof(name));
	// Non-blocking: without a reader this fails (ENXIO) instead of waiting for one.
	return open(name, O_WRONLY | O_NONBLOCK | O_NOFOLLOW);
}

static inline bool
dxr_log_channel_write(dxr_log_channel_t fd, const char *data, size_t n)
{
	int waited_ms = 0;
	while (n > 0) {
		const ssize_t w = write(fd, data, n);
		if (w > 0) {
			data += w;
			n -= (size_t)w;
			continue;
		}
		if (w < 0 && errno == EINTR) {
			continue;
		}
		if (w < 0 && errno != EAGAIN) {
			return false;
		}
		if (waited_ms >= DXR_LOG_WRITE_TIMEOUT_MS) {
			return false;
		}
		// A plain sleep: poll() reports a full FIFO as writable on macOS.
		struct timespec nap = {0, 2 * 1000 * 1000};
		(void)nanosleep(&nap, NULL);
		waited_ms += 2;
	}
	return true;
}

static inline void
dxr_log_channel_close(dxr_log_channel_t fd)
{
	if (fd >= 0) {
		close(fd);
	}
}

#endif
