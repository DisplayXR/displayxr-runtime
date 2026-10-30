// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-053 P2 (#2032): the OS process layer of the conversion-host
 *         satellite (os_process.h), against a real helper child
 *         (tests_conv_os_process_helper).
 *
 * What must hold (roadmap conversion-providers.md §3.4, §3.5):
 *  - exit codes arrive verbatim (0, 20, 30, 139), a terminate reports the
 *    code it was given, a wait times out;
 *  - the control channel carries conv_host_wire frames both ways, with a
 *    handle: Windows pushes it into the child (DuplicateHandle), POSIX sends
 *    it as SCM_RIGHTS;
 *  - the parent PULLS a child's handle (Windows) / receives it (POSIX), the
 *    child never gets a handle to the parent;
 *  - the wake signal latches (signal before wait still wakes) and coalesces;
 *  - a grandchild started with kill_on_parent_exit dies with its parent
 *    (job object / PR_SET_PDEATHSIG / kqueue NOTE_EXIT).
 */

#include "catch_amalgamated.hpp"

#include "os/os_process.h"
#include "conversion/conv_host_wire.h"

#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <signal.h>
#include <stdio.h>
#endif

#ifndef CONV_OS_PROCESS_HELPER
#error "CONV_OS_PROCESS_HELPER must name the helper executable"
#endif

namespace {

const char *const kHelper = CONV_OS_PROCESS_HELPER;

struct proc_guard
{
	os_process *p = nullptr;
	~proc_guard()
	{
		if (p != nullptr) {
			os_process_terminate(p, 1);
			int32_t code = 0;
			os_process_wait(p, 5000, &code);
			os_process_destroy(&p);
		}
	}
};

int
spawn(const std::vector<std::string> &args,
      proc_guard &g,
      uint64_t channel_child = OS_NATIVE_HANDLE_INVALID,
      bool disclaim = false)
{
	std::vector<const char *> argv;
	for (const auto &a : args) {
		argv.push_back(a.c_str());
	}
	os_process_spawn_info si{};
	si.exe_path = kHelper;
	si.args = argv.data();
	si.arg_count = (uint32_t)argv.size();
	si.kill_on_parent_exit = true;
	si.channel_child_handle = channel_child;
	si.disclaim_responsibility = disclaim; // macOS: the posix_spawn path; elsewhere ignored
	return os_process_spawn(&si, &g.p);
}

int32_t
wait_exit(proc_guard &g, uint32_t ms = 10000)
{
	int32_t code = -12345;
	REQUIRE(os_process_wait(g.p, ms, &code) == 1);
	return code;
}

bool
pid_alive(long pid)
{
#ifdef _WIN32
	HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
	if (h == NULL) {
		return false;
	}
	bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
	CloseHandle(h);
	return alive;
#else
	if (kill((pid_t)pid, 0) != 0) {
		return false;
	}
#ifdef __linux__
	// A reparented zombie still answers kill(0): read its state.
	char path[64];
	snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
	FILE *f = fopen(path, "r");
	if (f == nullptr) {
		return false;
	}
	char buf[512] = {};
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	const char *rp = strrchr(buf, ')');
	if (n == 0 || rp == nullptr || rp[1] == '\0' || rp[2] == '\0') {
		return false;
	}
	return rp[2] != 'Z' && rp[2] != 'X';
#else
	return true;
#endif
#endif
}

} // namespace

TEST_CASE("os_process: exit codes arrive verbatim", "[conv][os_process]")
{
	const bool disclaim = GENERATE(false, true);
	CAPTURE(disclaim);
	for (int code : {0, 7, 20, 30, 139}) {
		proc_guard g;
		REQUIRE(spawn({"exit", std::to_string(code)}, g, OS_NATIVE_HANDLE_INVALID, disclaim) == 0);
		CHECK(os_process_pid(g.p) > 0);
		CHECK(wait_exit(g) == code);
		// A second wait returns the same answer.
		int32_t again = -1;
		CHECK(os_process_wait(g.p, 0, &again) == 1);
		CHECK(again == code);
	}
}

TEST_CASE("os_process: wait times out, terminate reports its code", "[conv][os_process]")
{
	proc_guard g;
	REQUIRE(spawn({"sleep", "60000"}, g) == 0);
	int32_t code = -1;
	CHECK(os_process_wait(g.p, 50, &code) == 0);
	REQUIRE(os_process_terminate(g.p, CONV_HOST_EXIT_WATCHDOG) == 0);
	CHECK(wait_exit(g) == CONV_HOST_EXIT_WATCHDOG);
	// Terminating an exited process is harmless.
	CHECK(os_process_terminate(g.p, 5) == 0);
}

TEST_CASE("os_process: a missing binary never runs", "[conv][os_process]")
{
	os_process_spawn_info si{};
	si.exe_path = "this-binary-does-not-exist-2032";
	si.channel_child_handle = OS_NATIVE_HANDLE_INVALID;
	os_process *p = nullptr;
	int r = os_process_spawn(&si, &p);
	if (r == 0) {
		// POSIX: fork succeeded, exec failed.
		int32_t code = 0;
		REQUIRE(os_process_wait(p, 10000, &code) == 1);
		CHECK(code == 127);
		os_process_destroy(&p);
	} else {
		CHECK(r < 0);
		CHECK(p == nullptr);
	}
}

TEST_CASE("os_process: wake latches and coalesces", "[conv][os_process]")
{
	os_wake a{};
	uint64_t remote = OS_NATIVE_HANDLE_INVALID;
	REQUIRE(os_wake_create_pair(&a, &remote) == 0);
	os_wake b{};
#ifdef _WIN32
	// One event: both ends are the same handle.
	b.native = remote;
#else
	os_wake_from_native(&b, remote);
#endif
	CHECK(os_wake_wait(&b, 0) == 0);
	// Signalled BEFORE the wait: still wakes.
	REQUIRE(os_wake_signal(&a) == 0);
	REQUIRE(os_wake_signal(&a) == 0);
	REQUIRE(os_wake_signal(&a) == 0);
	CHECK(os_wake_wait(&b, 1000) == 1);
	// Coalesced: one wait consumed all three.
	CHECK(os_wake_wait(&b, 20) == 0);

	// Cross-thread.
	std::thread t([&] {
		std::this_thread::sleep_for(std::chrono::milliseconds(30));
		os_wake_signal(&a);
	});
	CHECK(os_wake_wait(&b, 5000) == 1);
	t.join();

	os_wake_destroy(&a);
#ifndef _WIN32
	os_wake_destroy(&b);
#endif
}

TEST_CASE("os_process: channel frame + pushed handle round trip", "[conv][os_process]")
{
	// Both spawn paths: fork + exec, and (macOS) posix_spawn with the
	// responsibility disclaimed, which must lay out the child identically.
	const bool disclaim = GENERATE(false, true);
	CAPTURE(disclaim);
	os_channel *ch = nullptr;
	char token[256] = {};
	uint64_t child_end = OS_NATIVE_HANDLE_INVALID;
	REQUIRE(os_channel_create_for_child("displayxr-conv-test", &ch, token, sizeof(token), &child_end) == 0);

	proc_guard g;
	REQUIRE(spawn({"echo", token}, g, child_end, disclaim) == 0);
	REQUIRE(os_channel_accept_child(ch, g.p, child_end, 10000) == 0);

	// The wake the child signals back.
	os_wake w{};
	uint64_t w_remote = OS_NATIVE_HANDLE_INVALID;
	REQUIRE(os_wake_create_pair(&w, &w_remote) == 0);

	conv_host_wire_frame f{};
	f.magic = CONV_HOST_WIRE_MAGIC;
	f.type = CONV_HOST_MSG_SHUTDOWN;
	f.request_id = 42;
	f.handle_count = 1;
	conv_host_wire_shutdown sd;
	conv_host_wire_init(&sd, sizeof(sd));
	sd.orderly = 1;
	f.payload_bytes = sizeof(sd);
#ifdef _WIN32
	// Push: the value in frame.handles[] is valid in the CHILD.
	uint64_t in_child = 0;
	REQUIRE(os_process_dup_to_child(g.p, w_remote, &in_child) == 0);
	f.handles[0] = in_child;
	REQUIRE(os_channel_send(ch, &f, sizeof(f), nullptr, 0) == 0);
	// Handles cannot ride a Windows channel.
	CHECK(os_channel_send(ch, &f, 0, &w_remote, 1) == OS_PROCESS_ERR_UNSUPPORTED);
#else
	REQUIRE(os_channel_send(ch, &f, sizeof(f), &w_remote, 1) == 0);
	os_native_handle_close(w_remote);
	uint64_t dummy = 0;
	CHECK(os_process_dup_to_child(g.p, 0, &dummy) == OS_PROCESS_ERR_UNSUPPORTED);
#endif
	REQUIRE(os_channel_send(ch, &sd, sizeof(sd), nullptr, 0) == 0);

	CHECK(os_wake_wait(&w, 10000) == 1);

	conv_host_wire_frame r{};
	uint32_t nfd = 0;
	REQUIRE(os_channel_recv(ch, &r, sizeof(r), nullptr, &nfd, 10000) == 1);
	REQUIRE(conv_host_wire_frame_valid(&r));
	CHECK(r.type == (CONV_HOST_MSG_SHUTDOWN | CONV_HOST_MSG_REPLY_BIT));
	CHECK(r.request_id == 42);
	REQUIRE(r.payload_bytes == sizeof(conv_host_wire_status));
	uint8_t payload[sizeof(conv_host_wire_status)];
	REQUIRE(os_channel_recv(ch, payload, sizeof(payload), nullptr, nullptr, 10000) == 1);
	conv_host_wire_status st;
	REQUIRE(conv_host_wire_receive(&st, sizeof(st), payload, sizeof(payload)));
	CHECK(st.result == 0);

	CHECK(wait_exit(g) == 7);
	// The child is gone: the channel reports it closed.
	uint8_t byte = 0;
	CHECK(os_channel_recv(ch, &byte, 1, nullptr, nullptr, 2000) == OS_PROCESS_ERR_CLOSED);

	os_wake_destroy(&w);
	os_channel_destroy(&ch);
}

TEST_CASE("os_process: the parent pulls a child's handle", "[conv][os_process]")
{
	os_channel *ch = nullptr;
	char token[256] = {};
	uint64_t child_end = OS_NATIVE_HANDLE_INVALID;
	REQUIRE(os_channel_create_for_child("displayxr-conv-test", &ch, token, sizeof(token), &child_end) == 0);

	proc_guard g;
	REQUIRE(spawn({"handback", token}, g, child_end) == 0);
	REQUIRE(os_channel_accept_child(ch, g.p, child_end, 10000) == 0);

	conv_host_wire_frame f{};
	uint64_t fds[CONV_HOST_WIRE_MAX_HANDLES] = {};
	uint32_t nfd = CONV_HOST_WIRE_MAX_HANDLES;
	REQUIRE(os_channel_recv(ch, &f, sizeof(f), fds, &nfd, 10000) == 1);
	REQUIRE(conv_host_wire_frame_valid(&f));
	REQUIRE(f.handle_count == 1);

	uint64_t mine = OS_NATIVE_HANDLE_INVALID;
#ifdef _WIN32
	CHECK(nfd == 0);
	REQUIRE(os_process_dup_from_child(g.p, f.handles[0], false, &mine) == 0);
#else
	REQUIRE(nfd == 1);
	mine = fds[0];
	CHECK(os_process_dup_from_child(g.p, 1, false, &mine) == OS_PROCESS_ERR_UNSUPPORTED);
#endif
	os_wake w{};
	os_wake_from_native(&w, mine);
	REQUIRE(os_wake_signal(&w) == 0);
	CHECK(wait_exit(g) == 9);
	os_wake_destroy(&w);
	os_channel_destroy(&ch);
}

TEST_CASE("os_process: a grandchild dies with its parent", "[conv][os_process]")
{
	os_channel *ch = nullptr;
	char token[256] = {};
	uint64_t child_end = OS_NATIVE_HANDLE_INVALID;
	REQUIRE(os_channel_create_for_child("displayxr-conv-test", &ch, token, sizeof(token), &child_end) == 0);

	proc_guard g;
	REQUIRE(spawn({"grandchild", kHelper, token}, g, child_end) == 0);
	REQUIRE(os_channel_accept_child(ch, g.p, child_end, 10000) == 0);

	uint64_t gc_pid = 0;
	REQUIRE(os_channel_recv(ch, &gc_pid, sizeof(gc_pid), nullptr, nullptr, 10000) == 1);
	REQUIRE(gc_pid > 0);
	CHECK(wait_exit(g) == 0);

	bool gone = false;
	for (int i = 0; i < 100 && !gone; i++) {
		gone = !pid_alive((long)gc_pid);
		if (!gone) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}
	CHECK(gone);
	os_channel_destroy(&ch);
}

TEST_CASE("os_process: elevation, pid, maintenance mutex", "[conv][os_process]")
{
	CHECK(os_process_current_pid() > 0);
	// A mutex nobody created is never held.
	CHECK_FALSE(os_named_mutex_held("Local\\DisplayXR-ConvMaintenance-test-2032-absent"));
#ifdef _WIN32
	// Held by another process: create it in a child-free way (a second
	// handle on another thread owns it).
	HANDLE m = CreateMutexW(NULL, FALSE, L"Local\\DisplayXR-ConvMaintenance-test-2032");
	REQUIRE(m != NULL);
	CHECK_FALSE(os_named_mutex_held("Local\\DisplayXR-ConvMaintenance-test-2032"));
	std::thread owner([&] {
		WaitForSingleObject(m, INFINITE);
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		ReleaseMutex(m);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	CHECK(os_named_mutex_held("Local\\DisplayXR-ConvMaintenance-test-2032"));
	owner.join();
	CHECK_FALSE(os_named_mutex_held("Local\\DisplayXR-ConvMaintenance-test-2032"));
	CloseHandle(m);
#endif
	(void)os_process_is_elevated(); // must not crash; the value depends on the runner
}
