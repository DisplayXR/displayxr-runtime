// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief browser#205: a Windows IPC read with a bound gives up when the peer
 *        accepted the pipe but never answers, instead of blocking in ReadFile
 *        forever — the wedged-service case the client's connect handshake now
 *        survives. The fake service here is a plain message-mode named pipe
 *        that is connected and then stays silent.
 */

#include "shared/ipc_message_channel.h"

#include "os/os_time.h"
#include "util/u_time.h"

#include "catch_amalgamated.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace {

//! A connected message-mode pipe pair: `server` is the fake service end,
//! `imc` the client channel, opened exactly as the client opens the real one.
struct PipePair
{
	HANDLE server = INVALID_HANDLE_VALUE;
	struct ipc_message_channel imc = {};

	PipePair()
	{
		char name[128];
		snprintf(name, sizeof(name), "\\\\.\\pipe\\displayxr\\tests_ipc_pipe_read_timeout_%lu_%llu",
		         GetCurrentProcessId(), (unsigned long long)os_monotonic_get_ns());
		server =
		    CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX,
		                     PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
		                     1, 1024, 1024, 0, nullptr);
		REQUIRE(server != INVALID_HANDLE_VALUE);

		imc.ipc_handle = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		REQUIRE(imc.ipc_handle != INVALID_HANDLE_VALUE);
		imc.log_level = U_LOGGING_WARN;

		DWORD mode = PIPE_READMODE_MESSAGE | PIPE_WAIT;
		REQUIRE(SetNamedPipeHandleState(imc.ipc_handle, &mode, nullptr, nullptr));

		// The client already connected, so this returns at once (or reports
		// ERROR_PIPE_CONNECTED, which means the same).
		if (!ConnectNamedPipe(server, nullptr)) {
			REQUIRE(GetLastError() == ERROR_PIPE_CONNECTED);
		}
	}

	~PipePair()
	{
		ipc_message_channel_close(&imc);
		if (server != INVALID_HANDLE_VALUE) {
			CloseHandle(server);
		}
	}
};

uint64_t
elapsed_ms(uint64_t start_ns)
{
	return (os_monotonic_get_ns() - start_ns) / U_TIME_1MS_IN_NS;
}

} // namespace

TEST_CASE("a bounded read on a silent peer gives up near the bound", "[ipc][browser205]")
{
	PipePair p;
	p.imc.read_timeout_ms = 200;

	uint32_t reply = 0;
	const uint64_t start_ns = os_monotonic_get_ns();
	const xrt_result_t xret = ipc_receive(&p.imc, &reply, sizeof(reply));
	const uint64_t ms = elapsed_ms(start_ns);

	CHECK(xret == XRT_ERROR_IPC_FAILURE);
	CHECK(p.imc.read_timed_out);
	CHECK(ms >= 190);
	CHECK(ms < 5000); // Generous: a loaded CI runner, never "forever".
}

TEST_CASE("a bounded read returns a message the peer did send", "[ipc][browser205]")
{
	PipePair p;
	p.imc.read_timeout_ms = 2000;

	const uint32_t sent = 0xD15A7u;
	DWORD written = 0;
	REQUIRE(WriteFile(p.server, &sent, sizeof(sent), &written, nullptr));

	uint32_t got = 0;
	CHECK(ipc_receive(&p.imc, &got, sizeof(got)) == XRT_SUCCESS);
	CHECK(got == sent);
	CHECK_FALSE(p.imc.read_timed_out);
}

TEST_CASE("a bounded read waits for a reply that arrives inside the bound", "[ipc][browser205]")
{
	PipePair p;
	p.imc.read_timeout_ms = 3000;

	struct Late
	{
		HANDLE server;
		static DWORD WINAPI
		run(LPVOID arg)
		{
			Sleep(150);
			const uint32_t v = 42;
			DWORD w = 0;
			WriteFile(static_cast<Late *>(arg)->server, &v, sizeof(v), &w, nullptr);
			return 0;
		}
	} late{p.server};
	HANDLE t = CreateThread(nullptr, 0, &Late::run, &late, 0, nullptr);
	REQUIRE(t != nullptr);

	uint32_t got = 0;
	CHECK(ipc_receive(&p.imc, &got, sizeof(got)) == XRT_SUCCESS);
	CHECK(got == 42u);
	CHECK_FALSE(p.imc.read_timed_out);

	WaitForSingleObject(t, INFINITE);
	CloseHandle(t);
}

TEST_CASE("a bounded read on a closed peer fails at once, not as a timeout", "[ipc][browser205]")
{
	PipePair p;
	p.imc.read_timeout_ms = 3000;

	DisconnectNamedPipe(p.server);
	CloseHandle(p.server);
	p.server = INVALID_HANDLE_VALUE;

	uint32_t got = 0;
	const uint64_t start_ns = os_monotonic_get_ns();
	CHECK(ipc_receive(&p.imc, &got, sizeof(got)) == XRT_ERROR_IPC_FAILURE);
	CHECK_FALSE(p.imc.read_timed_out);
	CHECK(elapsed_ms(start_ns) < 1000);
}

TEST_CASE("an unbounded channel is the default", "[ipc][browser205]")
{
	struct ipc_message_channel imc = {};
	CHECK(imc.read_timeout_ms == 0u);
	CHECK_FALSE(imc.read_timed_out);
}
