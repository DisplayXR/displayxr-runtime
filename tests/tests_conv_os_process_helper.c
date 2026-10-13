// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Child process driven by tests_conv_os_process (#2032).
 *
 *   exit <code>                 exit with <code>
 *   sleep <ms> [watch <ppid>]   sleep, exit 0; with watch: os_process_watch_parent(ppid) first
 *   echo <token>                read one conv_host_wire frame + payload carrying a wake handle,
 *                               signal the wake, reply, exit 7
 *   handback <token>            send a wake handle of OUR process to the parent (Windows: the
 *                               value, pulled with DuplicateHandle; POSIX: SCM_RIGHTS), exit 9
 *                               once the parent signals it (3 on timeout)
 *   grandchild <self> <token>   spawn `sleep 60000 watch <ourpid>` with kill_on_parent_exit,
 *                               send its pid (u64) on the channel, then exit 0
 */

#include "os/os_process.h"
#include "conversion/conv_host_wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
static void
sleep_ms(unsigned ms)
{
	Sleep(ms);
}
#else
#include <time.h>
static void
sleep_ms(unsigned ms)
{
	struct timespec ts = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
	nanosleep(&ts, NULL);
}
#endif

static int
do_echo(const char *token)
{
	struct os_channel *ch = NULL;
	if (os_channel_open_child(token, &ch) != 0) {
		return 2;
	}
	struct conv_host_wire_frame f;
	uint64_t fds[CONV_HOST_WIRE_MAX_HANDLES];
	uint32_t nfd = CONV_HOST_WIRE_MAX_HANDLES;
	if (os_channel_recv(ch, &f, sizeof(f), fds, &nfd, 10000) != 1 || !conv_host_wire_frame_valid(&f)) {
		return 3;
	}
	uint8_t payload[CONV_HOST_WIRE_MAX_PAYLOAD];
	uint32_t nfd2 = 0;
	if (os_channel_recv(ch, payload, f.payload_bytes, NULL, &nfd2, 10000) != 1) {
		return 4;
	}
	struct conv_host_wire_shutdown sd;
	if (f.type != CONV_HOST_MSG_SHUTDOWN || !conv_host_wire_receive(&sd, sizeof(sd), payload, f.payload_bytes) ||
	    sd.orderly != 1 || f.handle_count != 1) {
		return 5;
	}

	// Windows: frame.handles[0] is a value the parent pushed into us.
	// POSIX: the descriptor rode the frame.
	struct os_wake w;
#ifdef _WIN32
	os_wake_from_native(&w, f.handles[0]);
#else
	if (nfd != 1) {
		return 6;
	}
	os_wake_from_native(&w, fds[0]);
#endif
	if (os_wake_signal(&w) != 0) {
		return 8;
	}

	struct conv_host_wire_frame r;
	memset(&r, 0, sizeof(r));
	r.magic = CONV_HOST_WIRE_MAGIC;
	r.type = f.type | CONV_HOST_MSG_REPLY_BIT;
	r.request_id = f.request_id;
	struct conv_host_wire_status st;
	conv_host_wire_init(&st, sizeof(st));
	st.result = 0;
	r.payload_bytes = sizeof(st);
	if (os_channel_send(ch, &r, sizeof(r), NULL, 0) != 0 || os_channel_send(ch, &st, sizeof(st), NULL, 0) != 0) {
		return 10;
	}
	os_wake_destroy(&w);
	os_channel_destroy(&ch);
	return 7;
}

static int
do_handback(const char *token)
{
	struct os_channel *ch = NULL;
	if (os_channel_open_child(token, &ch) != 0) {
		return 2;
	}
	struct os_wake local;
	uint64_t remote = OS_NATIVE_HANDLE_INVALID;
	if (os_wake_create_pair(&local, &remote) != 0) {
		return 4;
	}

	struct conv_host_wire_frame f;
	memset(&f, 0, sizeof(f));
	f.magic = CONV_HOST_WIRE_MAGIC;
	f.type = CONV_HOST_MSG_SYNC;
	f.handle_count = 1;
#ifdef _WIN32
	// A value valid in OUR process; the parent pulls it.
	f.handles[0] = remote;
	if (os_channel_send(ch, &f, sizeof(f), NULL, 0) != 0) {
		return 5;
	}
#else
	if (os_channel_send(ch, &f, sizeof(f), &remote, 1) != 0) {
		return 5;
	}
	os_native_handle_close(remote);
#endif
	int r = os_wake_wait(&local, 10000);
	os_wake_destroy(&local);
	os_channel_destroy(&ch);
	return r == 1 ? 9 : 3;
}

static int
do_grandchild(const char *self, const char *token)
{
	struct os_channel *ch = NULL;
	if (os_channel_open_child(token, &ch) != 0) {
		return 2;
	}
	char ppid[32];
	snprintf(ppid, sizeof(ppid), "%ld", os_process_current_pid());
	const char *args[] = {"sleep", "60000", "watch", ppid};
	struct os_process_spawn_info si;
	memset(&si, 0, sizeof(si));
	si.exe_path = self;
	si.args = args;
	si.arg_count = 4;
	si.kill_on_parent_exit = true;
	si.channel_child_handle = OS_NATIVE_HANDLE_INVALID;
	struct os_process *gc = NULL;
	if (os_process_spawn(&si, &gc) != 0) {
		return 4;
	}
	uint64_t pid = (uint64_t)os_process_pid(gc);
	if (os_channel_send(ch, &pid, sizeof(pid), NULL, 0) != 0) {
		return 5;
	}
	// Give the grandchild time to arm its parent watch (macOS), then leave
	// WITHOUT killing it: the containment must.
	sleep_ms(500);
	os_channel_destroy(&ch);
	return 0; // the job handle closes with us (Windows); PDEATHSIG / kqueue fire on POSIX
}

int
main(int argc, char **argv)
{
	if (argc >= 3 && strcmp(argv[1], "exit") == 0) {
		return atoi(argv[2]);
	}
	if (argc >= 3 && strcmp(argv[1], "sleep") == 0) {
		if (argc >= 5 && strcmp(argv[3], "watch") == 0) {
			if (os_process_watch_parent(atol(argv[4]), 99) != 0) {
				return 98;
			}
		}
		sleep_ms((unsigned)atoi(argv[2]));
		return 0;
	}
	if (argc >= 3 && strcmp(argv[1], "echo") == 0) {
		return do_echo(argv[2]);
	}
	if (argc >= 3 && strcmp(argv[1], "handback") == 0) {
		return do_handback(argv[2]);
	}
	if (argc >= 4 && strcmp(argv[1], "grandchild") == 0) {
		return do_grandchild(argv[2], argv[3]);
	}
	return 1;
}
