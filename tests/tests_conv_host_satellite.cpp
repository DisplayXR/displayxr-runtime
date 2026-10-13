// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief displayxr-conversion-host (ADR-053 D5, #2033): the satellite binary
 *        driven over a loopback channel by a test acting as the service.
 *
 * Headless, no display. The test spawns the BUILT host with the BUILT sim
 * provider, plays the service's half of the protocol (conv_host_wire.h):
 * hello / ack, DESCRIBE every capability, WARM, STREAM_OPEN, ALLOC_OUTPUTS,
 * a CPU input image in the lane's shm, LANE_OPEN, one submit record through
 * the command ring, the completion through the completion ring, and checks
 * the provider's bytes in the host-allocated output. Then SHUTDOWN, exit 0.
 * The exit-code table is checked for usage (2), elevated (14, only when this
 * process is elevated), ABI reject (10), negotiate failed (11), no device
 * (13, Windows), and the idle exit (0).
 *
 * Windows: the channel is a named pipe the test serves; handles go INTO the
 * host with DuplicateHandle and come back the same way (the service-side
 * rule of §3.5). POSIX: a socketpair end the host inherits, fds as
 * SCM_RIGHTS.
 */

#include "conv_host_args.h"
#include "conv_host_chan.h"

#include "conversion/conv_host_wire.h"
#include "util/u_shm_ring.h"
#include "xrt/xrt_conversion_plugin.h"

#include "catch_amalgamated.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace {

void
set_env(const char *name, const char *value)
{
#ifdef _WIN32
	_putenv_s(name, value != nullptr ? value : "");
	SetEnvironmentVariableA(name, value);
#else
	if (value != nullptr) {
		setenv(name, value, 1);
	} else {
		unsetenv(name);
	}
#endif
}

bool
self_is_elevated()
{
#ifdef _WIN32
	HANDLE tok = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
		return false;
	}
	TOKEN_ELEVATION e = {};
	DWORD len = 0;
	const BOOL ok = GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &len);
	CloseHandle(tok);
	return ok && e.TokenIsElevated != 0;
#else
	return geteuid() == 0 || geteuid() != getuid();
#endif
}

void
common_env()
{
	// The sim provider and the stubs live in the build tree (#952 guard).
	set_env("DXR_ALLOW_DEV_PLUGIN_PATHS", "1");
	// CI runners and this dev box's harness are elevated; the refusal itself
	// is covered by its own case below.
	set_env("DXR_CONV_HOST_ALLOW_ELEVATED", "1");
	// The provider's settings, as the service's environment would carry them.
	set_env("DXR_CONV_SIM_CONVERSION_ENABLE", "1");
	set_env("DXR_CONV_SIM_CONVERSION_LATENCY_MS", "0");
}

/*!
 * The test's "service" end of one host: the spawned process, the channel and
 * the handle plumbing.
 */
struct Host
{
	conv_host_chan *chan = nullptr;
#ifdef _WIN32
	PROCESS_INFORMATION pi = {};
	HANDLE job = nullptr; //!< KILL_ON_JOB_CLOSE: a host never outlives the test, whatever happens
#else
	pid_t pid = -1;
#endif
	bool spawned = false;

	~Host()
	{
		conv_host_chan_destroy(&chan);
		kill_now();
	}

	bool
	spawn(const std::vector<std::string> &extra_args, bool with_channel = true)
	{
		std::vector<std::string> args = {CONV_HOST_EXE};
#ifdef _WIN32
		static int counter = 0;
		char name[128];
		snprintf(name, sizeof(name), "dxr-convhost-test-%lu-%d", (unsigned long)GetCurrentProcessId(),
		         ++counter);
		HANDLE pipe = INVALID_HANDLE_VALUE;
		if (with_channel) {
			char path[192];
			snprintf(path, sizeof(path), "\\\\.\\pipe\\%s", name);
			pipe = CreateNamedPipeA(path, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
			                        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, nullptr);
			if (pipe == INVALID_HANDLE_VALUE) {
				return false;
			}
			args.push_back("--channel");
			args.push_back(name);
		}
		for (const auto &a : extra_args) {
			args.push_back(a);
		}
		std::string cmd;
		for (const auto &a : args) {
			cmd += "\"" + a + "\" ";
		}
		STARTUPINFOA si = {};
		si.cb = sizeof(si);
		std::vector<char> buf(cmd.begin(), cmd.end());
		buf.push_back('\0');
		if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
		                    nullptr, nullptr, &si, &pi)) {
			if (pipe != INVALID_HANDLE_VALUE) {
				CloseHandle(pipe);
			}
			return false;
		}
		// As the supervisor does: the host dies with this process (and with
		// this Host object), so a failed assertion never leaves one behind.
		job = CreateJobObjectA(nullptr, nullptr);
		if (job != nullptr) {
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
			li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
			AssignProcessToJobObject(job, pi.hProcess);
		}
		ResumeThread(pi.hThread);
		spawned = true;
		if (with_channel) {
			OVERLAPPED ov = {};
			ov.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
			BOOL ok = ConnectNamedPipe(pipe, &ov);
			if (!ok && GetLastError() == ERROR_IO_PENDING) {
				HANDLE hs[2] = {ov.hEvent, pi.hProcess};
				const DWORD w = WaitForMultipleObjects(2, hs, FALSE, 20000);
				DWORD dummy = 0;
				ok = w == WAIT_OBJECT_0 && GetOverlappedResult(pipe, &ov, &dummy, FALSE);
				if (!ok) {
					CancelIoEx(pipe, &ov);
				}
			} else if (!ok && GetLastError() == ERROR_PIPE_CONNECTED) {
				ok = TRUE;
			}
			CloseHandle(ov.hEvent);
			if (!ok) {
				CloseHandle(pipe);
				return true; // spawned, but the host never connected (it exited early)
			}
			char token[32];
			snprintf(token, sizeof(token), "%llu", (unsigned long long)(uintptr_t)pipe);
			chan = conv_host_chan_open(token, nullptr, 0);
		}
		return true;
#else
		int sv[2] = {-1, -1};
		if (with_channel) {
			if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
				return false;
			}
			fcntl(sv[0], F_SETFD, FD_CLOEXEC); // ours; sv[1] is inherited
			args.push_back("--channel");
			args.push_back(std::to_string(sv[1]));
		}
		for (const auto &a : extra_args) {
			args.push_back(a);
		}
		std::vector<char *> argv;
		for (auto &a : args) {
			argv.push_back(const_cast<char *>(a.c_str()));
		}
		argv.push_back(nullptr);
		const int r = posix_spawn(&pid, CONV_HOST_EXE, nullptr, nullptr, argv.data(), environ);
		if (with_channel) {
			close(sv[1]);
		}
		if (r != 0) {
			if (with_channel) {
				close(sv[0]);
			}
			return false;
		}
		spawned = true;
		if (with_channel) {
			char token[32];
			snprintf(token, sizeof(token), "%d", sv[0]);
			chan = conv_host_chan_open(token, nullptr, 0);
		}
		return true;
#endif
	}

	std::string
	parent_arg() const
	{
#ifdef _WIN32
		return std::to_string((unsigned long)GetCurrentProcessId());
#else
		return std::to_string((long)getpid());
#endif
	}

	//! Wait for the process to exit; returns its code, or -1000 on timeout.
	int
	wait_exit(uint32_t timeout_ms)
	{
#ifdef _WIN32
		if (!spawned) {
			return -1001;
		}
		if (WaitForSingleObject(pi.hProcess, timeout_ms) != WAIT_OBJECT_0) {
			return -1000;
		}
		DWORD code = 0;
		GetExitCodeProcess(pi.hProcess, &code);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		if (job != nullptr) {
			CloseHandle(job);
			job = nullptr;
		}
		spawned = false;
		return (int)code;
#else
		if (!spawned) {
			return -1001;
		}
		const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
		int status = 0;
		while (std::chrono::steady_clock::now() < end) {
			const pid_t w = waitpid(pid, &status, WNOHANG);
			if (w == pid) {
				spawned = false;
				return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return -1000;
#endif
	}

	void
	kill_now()
	{
		if (!spawned) {
			return;
		}
#ifdef _WIN32
		TerminateProcess(pi.hProcess, 99);
		WaitForSingleObject(pi.hProcess, 5000);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		if (job != nullptr) {
			CloseHandle(job);
			job = nullptr;
		}
#else
		::kill(pid, SIGKILL);
		int status = 0;
		waitpid(pid, &status, 0);
#endif
		spawned = false;
	}

	//! A local handle as the host will see it (Windows: duplicated INTO the host).
	uint64_t
	to_host(uint64_t local)
	{
#ifdef _WIN32
		HANDLE out = nullptr;
		if (!DuplicateHandle(GetCurrentProcess(), (HANDLE)(uintptr_t)local, pi.hProcess, &out, 0, FALSE,
		                     DUPLICATE_SAME_ACCESS)) {
			return 0;
		}
		return (uint64_t)(uintptr_t)out;
#else
		return local; // SCM_RIGHTS duplicates on send
#endif
	}

	//! A received handle as this process can use it (Windows: PULLED from the host).
	uint64_t
	from_host(uint64_t received)
	{
#ifdef _WIN32
		HANDLE out = nullptr;
		if (!DuplicateHandle(pi.hProcess, (HANDLE)(uintptr_t)received, GetCurrentProcess(), &out, 0, FALSE,
		                     DUPLICATE_SAME_ACCESS)) {
			return 0;
		}
		return (uint64_t)(uintptr_t)out;
#else
		return received;
#endif
	}

	bool
	send(uint32_t type, uint32_t rid, const void *p, uint32_t n, std::vector<uint64_t> local_handles = {})
	{
		std::vector<uint64_t> h;
		for (uint64_t l : local_handles) {
			h.push_back(to_host(l));
		}
		return conv_host_chan_send(chan, type, rid, p, n, h.data(), (uint32_t)h.size());
	}

	//! Receive the next frame that is not LOG / HEARTBEAT / STATE_CHANGED.
	bool
	recv(conv_host_wire_frame &f, std::vector<uint8_t> &payload)
	{
		payload.assign(CONV_HOST_WIRE_MAX_PAYLOAD, 0);
		for (;;) {
			if (!conv_host_chan_recv(chan, &f, payload.data())) {
				return false;
			}
			if (f.type == CONV_HOST_MSG_LOG || f.type == CONV_HOST_MSG_HEARTBEAT ||
			    f.type == CONV_HOST_MSG_STATE_CHANGED) {
				continue;
			}
			return true;
		}
	}

	//! Send a request and return its conv_host_wire_status result (other frames with the id are collected).
	int32_t
	request_status(uint32_t type,
	               uint32_t rid,
	               const void *p,
	               uint32_t n,
	               std::vector<uint64_t> local_handles = {},
	               std::vector<std::pair<conv_host_wire_frame, std::vector<uint8_t>>> *side = nullptr)
	{
		if (!send(type, rid, p, n, local_handles)) {
			return -10000;
		}
		conv_host_wire_frame f;
		std::vector<uint8_t> pl;
		while (recv(f, pl)) {
			if (f.type == (type | CONV_HOST_MSG_REPLY_BIT) && f.request_id == rid) {
				conv_host_wire_status st;
				if (!conv_host_wire_receive(&st, sizeof(st), pl.data(), f.payload_bytes)) {
					return -10001;
				}
				return st.result;
			}
			if (side != nullptr) {
				side->push_back({f, pl});
			}
		}
		return -10002;
	}
};

void
make_wake(uint64_t &host_end, uint64_t &my_end, bool host_waits)
{
#ifdef _WIN32
	(void)host_waits;
	HANDLE e = CreateEventA(nullptr, FALSE, FALSE, nullptr); // auto-reset: a wait consumes it
	host_end = my_end = (uint64_t)(uintptr_t)e;
#else
	int p[2];
	REQUIRE(pipe(p) == 0);
	fcntl(p[0], F_SETFD, FD_CLOEXEC);
	fcntl(p[1], F_SETFD, FD_CLOEXEC);
	// The host waits on a read end and signals a write end.
	host_end = (uint64_t)(host_waits ? p[0] : p[1]);
	my_end = (uint64_t)(host_waits ? p[1] : p[0]);
#endif
}

bool
handshake(Host &h, uint32_t *out_caps = nullptr)
{
	conv_host_wire_frame f;
	std::vector<uint8_t> pl;
	if (!h.recv(f, pl) || f.type != CONV_HOST_MSG_HELLO) {
		return false;
	}
	conv_host_wire_hello hello;
	if (!conv_host_wire_receive(&hello, sizeof(hello), pl.data(), f.payload_bytes)) {
		return false;
	}
	CHECK(std::string(hello.provider_id) == "sim-conversion");
	CHECK(hello.provider_api_version == XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT);
	if (out_caps != nullptr) {
		*out_caps = hello.capability_count;
	}
	conv_host_wire_hello_ack ack;
	conv_host_wire_init(&ack, sizeof(ack));
	ack.accepted = 1;
	conv_host_wire_set_str(ack.git_tag, sizeof(ack.git_tag), hello.git_tag);
	return h.send(CONV_HOST_MSG_HELLO_ACK, 0, &ack, sizeof(ack));
}

std::vector<std::string>
sim_args(Host &h)
{
	return {"--provider", "sim-conversion", "--binary", SIM_CONVERSION_PATH, "--parent", h.parent_arg()};
}

} // namespace


TEST_CASE("conv_host_args: the supervisor's command line", "[conv_host]")
{
	conv_host_args a;
	const char *ok[] = {"x", "--provider", "sim-conversion", "--binary", "/p/sim.so", "--channel", "7",
	                    "--parent", "1234", "--adapter-luid", "0x00000000000012af"};
	REQUIRE(conv_host_args_parse(11, ok, &a));
	CHECK(std::string(a.provider_id) == "sim-conversion");
	CHECK(a.parent_pid == 1234u);
	CHECK(a.adapter_luid == 0x12afu);
	CHECK(a.has_adapter_luid);
	CHECK(a.idle_exit_ms == CONV_HOST_IDLE_EXIT_MS_DEFAULT);

	const char *no_luid[] = {"x", "--provider", "p", "--binary", "b", "--channel", "c", "--parent", "1"};
	REQUIRE(conv_host_args_parse(9, no_luid, &a));
	CHECK_FALSE(a.has_adapter_luid);

	const char *missing[] = {"x", "--provider", "p", "--binary", "b", "--channel", "c"};
	CHECK_FALSE(conv_host_args_parse(7, missing, &a));
	CHECK(std::string(a.error).find("--parent") != std::string::npos);

	const char *bad_pid[] = {"x", "--provider", "p", "--binary", "b", "--channel", "c", "--parent", "-5"};
	CHECK_FALSE(conv_host_args_parse(9, bad_pid, &a));
	const char *unknown[] = {"x", "--frobnicate", "1"};
	CHECK_FALSE(conv_host_args_parse(3, unknown, &a));
	const char *dangling[] = {"x", "--provider"};
	CHECK_FALSE(conv_host_args_parse(2, dangling, &a));

	uint64_t l = 0;
	CHECK(conv_host_parse_luid("1:2a", &l));
	CHECK(l == ((1ull << 32) | 0x2a));
	CHECK(conv_host_parse_luid("ABCDEF", &l));
	CHECK(l == 0xabcdefu);
	CHECK_FALSE(conv_host_parse_luid("0x", &l));
	CHECK_FALSE(conv_host_parse_luid("12g", &l));
	CHECK_FALSE(conv_host_parse_luid("11112222333344445", &l));
}

TEST_CASE("conv_host: exit codes before and at load", "[conv_host]")
{
	common_env();

	SECTION("usage error -> 2")
	{
		Host h;
		REQUIRE(h.spawn({"--provider", "x"}, false));
		CHECK(h.wait_exit(20000) == 2);
	}

	SECTION("ABI-major mismatch -> 10")
	{
		Host h;
		REQUIRE(h.spawn({"--provider", "stub-abi", "--binary", CONV_STUB_ABI_PATH, "--parent", h.parent_arg()}));
		CHECK(h.wait_exit(20000) == CONV_HOST_EXIT_ABI_REJECT);
	}

	SECTION("no binary -> 11")
	{
		Host h;
		REQUIRE(h.spawn({"--provider", "nope", "--binary", SIM_CONVERSION_PATH ".missing", "--parent",
		                 h.parent_arg()}));
		CHECK(h.wait_exit(20000) == CONV_HOST_EXIT_NEGOTIATE_FAILED);
	}

	SECTION("probe declines (ENABLE unset) -> 11")
	{
		set_env("DXR_CONV_SIM_CONVERSION_ENABLE", "0");
		Host h;
		REQUIRE(h.spawn(sim_args(h)));
		CHECK(h.wait_exit(20000) == CONV_HOST_EXIT_NEGOTIATE_FAILED);
		set_env("DXR_CONV_SIM_CONVERSION_ENABLE", "1");
	}

#ifdef _WIN32
	SECTION("adapter LUID with no adapter -> 13")
	{
		Host h;
		auto a = sim_args(h);
		a.push_back("--adapter-luid");
		a.push_back("0x7fffffff0badf00d");
		REQUIRE(h.spawn(a));
		CHECK(h.wait_exit(20000) == CONV_HOST_EXIT_NO_DEVICE);
	}
#endif

	SECTION("elevated -> 14")
	{
		if (!self_is_elevated()) {
			SKIP("this process is not elevated; the host would inherit a medium token");
		}
		set_env("DXR_CONV_HOST_ALLOW_ELEVATED", nullptr);
		Host h;
		REQUIRE(h.spawn(sim_args(h)));
		CHECK(h.wait_exit(20000) == CONV_HOST_EXIT_ELEVATED);
		set_env("DXR_CONV_HOST_ALLOW_ELEVATED", "1");
	}
}

TEST_CASE("conv_host: idle exit with no bound task", "[conv_host]")
{
	common_env();
	Host h;
	auto a = sim_args(h);
	a.push_back("--idle-exit-ms");
	a.push_back("1500");
	REQUIRE(h.spawn(a));
	REQUIRE(h.chan != nullptr);
	REQUIRE(handshake(h));
	// Heartbeats keep arriving; with nothing bound the host leaves on its own.
	const auto t0 = std::chrono::steady_clock::now();
	CHECK(h.wait_exit(15000) == CONV_HOST_EXIT_ORDERLY);
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
	CHECK(ms.count() >= 1000);
}

TEST_CASE("conv_host: sim provider describe / warm / one submit through the rings", "[conv_host]")
{
	common_env();
	Host h;
	REQUIRE(h.spawn(sim_args(h)));
	REQUIRE(h.chan != nullptr);
	uint32_t caps = 0;
	REQUIRE(handshake(h, &caps));
	REQUIRE(caps >= 1);

	uint32_t rid = 1;

	// DESCRIBE every capability; find DEPTH_MONO.
	bool have_depth = false;
	for (uint32_t i = 0; i < caps; i++) {
		conv_host_wire_index q;
		conv_host_wire_init(&q, sizeof(q));
		q.index = i;
		REQUIRE(h.send(CONV_HOST_MSG_DESCRIBE, rid, &q, sizeof(q)));
		conv_host_wire_frame f;
		std::vector<uint8_t> pl;
		REQUIRE(h.recv(f, pl));
		REQUIRE(f.type == (CONV_HOST_MSG_DESCRIBE | CONV_HOST_MSG_REPLY_BIT));
		REQUIRE(f.request_id == rid);
		conv_host_wire_describe_reply rep;
		REQUIRE(conv_host_wire_receive(&rep, sizeof(rep), pl.data(), f.payload_bytes));
		CHECK(rep.result == XRT_SUCCESS);
		CHECK(rep.count == caps);
		xrt_conv_capability cap;
		conv_host_wire_unpack_capability(&rep.capability, &cap);
		CHECK(cap.cap_id != 0);
		CHECK((cap.import_memory_kinds & XRT_CONV_MEM_CPU) != 0);
		have_depth = have_depth || (cap.cap_id == XRT_CONV_TASK_DEPTH_MONO && cap.task == XRT_CONV_TASK_DEPTH_MONO);
		rid++;
	}
	REQUIRE(have_depth);
	const uint32_t cap_id = XRT_CONV_TASK_DEPTH_MONO;

	// WARM.
	{
		conv_host_wire_cap_request q;
		conv_host_wire_init(&q, sizeof(q));
		q.cap_id = cap_id;
		CHECK(h.request_status(CONV_HOST_MSG_WARM, rid++, &q, sizeof(q)) == XRT_SUCCESS);

		conv_host_wire_cap_request s;
		conv_host_wire_init(&s, sizeof(s));
		s.cap_id = cap_id;
		const uint32_t srid = rid++;
		REQUIRE(h.send(CONV_HOST_MSG_GET_STATE, srid, &s, sizeof(s)));
		conv_host_wire_frame f;
		std::vector<uint8_t> pl;
		REQUIRE(h.recv(f, pl));
		REQUIRE(f.type == (CONV_HOST_MSG_GET_STATE | CONV_HOST_MSG_REPLY_BIT));
		conv_host_wire_state_reply sr;
		REQUIRE(conv_host_wire_receive(&sr, sizeof(sr), pl.data(), f.payload_bytes));
		CHECK(sr.result == XRT_SUCCESS);
		CHECK(sr.state.state == XRT_CONV_STATE_READY);
	}

	// STREAM_OPEN (service id 77).
	const uint64_t stream = 77;
	{
		conv_host_wire_stream_open q;
		conv_host_wire_init(&q, sizeof(q));
		q.cap_id = cap_id;
		q.stream = stream;
		q.input_kind = XRT_CONV_INPUT_MONO;
		q.outputs = XRT_CONV_OUT_DEPTH;
		q.content_hint = XRT_CONV_CONTENT_PHOTO;
		q.max_input_width = 8;
		q.max_input_height = 4;
		CHECK(h.request_status(CONV_HOST_MSG_STREAM_OPEN, rid++, &q, sizeof(q)) == XRT_SUCCESS);
	}

	// ALLOC_OUTPUTS: 2 sets at 8x4; collect the OUTPUT_IMAGE frames.
	const uint32_t W = 8, H = 4;
	struct Out
	{
		uint32_t index;
		uint32_t role;
		uint64_t handle; //!< local
		uint64_t bytes;
	};
	std::vector<Out> outs;
	{
		conv_host_wire_alloc_outputs q;
		conv_host_wire_init(&q, sizeof(q));
		q.stream = stream;
		q.width = W;
		q.height = H;
		q.set_count = 2;
		std::vector<std::pair<conv_host_wire_frame, std::vector<uint8_t>>> side;
		CHECK(h.request_status(CONV_HOST_MSG_ALLOC_OUTPUTS, rid, &q, sizeof(q), {}, &side) == XRT_SUCCESS);
		for (auto &fr : side) {
			if (fr.first.type != CONV_HOST_MSG_OUTPUT_IMAGE) {
				continue;
			}
			CHECK(fr.first.request_id == rid);
			REQUIRE(fr.first.handle_count == 1);
			conv_host_wire_output_image oi;
			REQUIRE(conv_host_wire_receive(&oi, sizeof(oi), fr.second.data(), fr.first.payload_bytes));
			CHECK(oi.stream == stream);
			CHECK(oi.image.memory_kind == XRT_CONV_MEM_CPU); // no adapter LUID: the CPU lane
			CHECK(oi.image.format == XRT_CONV_FMT_R32_FLOAT);
			CHECK(oi.image.width == W);
			CHECK(oi.image.height == H);
			outs.push_back({oi.index, oi.role, h.from_host(fr.first.handles[0]), oi.image.byte_size});
		}
		rid++;
	}
	REQUIRE(outs.size() == 2); // one depth image per set
	for (const auto &o : outs) {
		CHECK(o.role == CONV_HOST_WIRE_OUT_DEPTH);
		CHECK(o.handle != 0);
	}

	// The lane shm: [submit ring][complete ring][input image].
	const size_t sb = u_shm_ring_bytes(sizeof(conv_host_wire_submit_record), CONV_HOST_WIRE_RING_CAPACITY);
	const size_t cb = u_shm_ring_bytes(sizeof(conv_host_wire_complete_record), CONV_HOST_WIRE_RING_CAPACITY);
	REQUIRE(sb > 0);
	REQUIRE(cb > 0);
	const uint64_t img_off = sb + cb;
	const uint64_t img_bytes = (uint64_t)W * H * 4;
	const uint64_t shm_bytes = img_off + img_bytes;
	void *map = nullptr;
	const uint64_t shm = conv_host_shm_create(shm_bytes, &map);
	REQUIRE(shm != 0);
	u_shm_ring submit = {};
	u_shm_ring complete = {};
	REQUIRE(u_shm_ring_init(&submit, map, sb, sizeof(conv_host_wire_submit_record), CONV_HOST_WIRE_RING_CAPACITY));
	REQUIRE(u_shm_ring_init(&complete, (uint8_t *)map + sb, cb, sizeof(conv_host_wire_complete_record),
	                        CONV_HOST_WIRE_RING_CAPACITY));
	std::memset((uint8_t *)map + img_off, 0x80, (size_t)img_bytes); // grey RGBA8

	// REGISTER_IMAGE: input slot 3, CPU, bytes in the lane shm.
	{
		conv_host_wire_register_image q;
		conv_host_wire_init(&q, sizeof(q));
		q.index = 3;
		q.image.memory_kind = XRT_CONV_MEM_CPU;
		q.image.role = XRT_CONV_IMG_COLOR;
		q.image.width = W;
		q.image.height = H;
		q.image.format = XRT_CONV_FMT_RGBA8_UNORM;
		q.image.color_space = XRT_CONV_CS_SRGB;
		q.image.row_stride = W * 4;
		q.image.byte_size = img_bytes;
		q.image.shm_offset = img_off;
		CHECK(h.request_status(CONV_HOST_MSG_REGISTER_IMAGE, rid++, &q, sizeof(q)) == XRT_SUCCESS);
	}

	// LANE_OPEN: shm + wake in (host waits) + wake out (host signals).
	uint64_t wake_in_host = 0, wake_in_mine = 0, wake_out_host = 0, wake_out_mine = 0;
	make_wake(wake_in_host, wake_in_mine, true);
	make_wake(wake_out_host, wake_out_mine, false);
	{
		conv_host_wire_lane_open q;
		conv_host_wire_init(&q, sizeof(q));
		q.lane = 0;
		q.lane_kind = CONV_HOST_LANE_STREAM;
		q.ring_capacity = CONV_HOST_WIRE_RING_CAPACITY;
		q.submit_record_bytes = sizeof(conv_host_wire_submit_record);
		q.complete_record_bytes = sizeof(conv_host_wire_complete_record);
		q.shm_bytes = shm_bytes;
		q.submit_ring_offset = 0;
		q.complete_ring_offset = sb;
		CHECK(h.request_status(CONV_HOST_MSG_LANE_OPEN, rid++, &q, sizeof(q),
		                       {shm, wake_in_host, wake_out_host}) == XRT_SUCCESS);
	}

	// One submit record.
	conv_host_wire_submit_record rec;
	std::memset(&rec, 0, sizeof(rec));
	rec.seq = 1;
	rec.stream = stream;
	rec.op = CONV_HOST_RING_OP_CONVERT;
	rec.image_count = 1;
	rec.in_image_idx[0] = 3;
	rec.in_image_role[0] = XRT_CONV_IMG_COLOR;
	for (uint32_t i = 1; i < XRT_CONV_MAX_FRAME_IMAGES; i++) {
		rec.in_image_idx[i] = CONV_HOST_WIRE_NO_INDEX;
	}
	rec.in_sync_idx = CONV_HOST_WIRE_NO_INDEX;
	xrt_conv_params params = {};
	params.struct_size = sizeof(params);
	params.frame_id = 1;
	params.strength = 1.0f;
	xrt_conv_wire_pack_params(&params, &rec.params);
	bool wake = false;
	REQUIRE(u_shm_ring_push(&submit, &rec, &wake) == U_SHM_RING_OK);
	if (wake) {
		conv_host_wake_signal(wake_in_mine);
	}

	// The completion.
	conv_host_wire_complete_record comp;
	bool got = false;
	const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!got && std::chrono::steady_clock::now() < end) {
		const u_shm_ring_result r = u_shm_ring_pop(&complete, &comp);
		if (r == U_SHM_RING_OK) {
			got = true;
			break;
		}
		REQUIRE(r == U_SHM_RING_EMPTY);
		if (u_shm_ring_prepare_wait(&complete)) {
			(void)conv_host_wake_wait(wake_out_mine, 100);
			u_shm_ring_finish_wait(&complete);
		}
	}
	REQUIRE(got);
	CHECK(comp.seq == 1);
	CHECK(comp.status == XRT_SUCCESS);
	CHECK((comp.written & XRT_CONV_OUT_DEPTH) != 0);
	CHECK(comp.out_sync_idx == CONV_HOST_WIRE_NO_INDEX); // CPU: complete at hand-over
	CHECK(comp.end_ns >= comp.start_ns);
	xrt_conv_depth_info di;
	xrt_conv_wire_unpack_depth_info(&comp.depth_info, &di);
	CHECK(di.kind == XRT_CONV_DEPTH_AFFINE);

	// The provider's bytes, read through the output we pulled.
	const uint32_t out_idx = comp.out_image_idx[CONV_HOST_WIRE_OUT_DEPTH];
	const Out *o = nullptr;
	for (const auto &x : outs) {
		if (x.index == out_idx) {
			o = &x;
		}
	}
	REQUIRE(o != nullptr);
	void *omap = conv_host_shm_map(o->handle, o->bytes);
	REQUIRE(omap != nullptr);
	const float *px = (const float *)omap;
	for (uint32_t y = 0; y < H; y++) {
		const float expect = ((float)y + 0.5f) / (float)H; // DEPTH_MONO: vertical ramp, 0 at the top
		CHECK(px[y * W + 0] == Catch::Approx(expect));
		CHECK(px[y * W + W - 1] == Catch::Approx(expect));
	}
	conv_host_shm_unmap(omap, o->bytes);

	// Orderly SHUTDOWN: reply, then exit 0.
	{
		conv_host_wire_shutdown q;
		conv_host_wire_init(&q, sizeof(q));
		q.orderly = 1;
		CHECK(h.request_status(CONV_HOST_MSG_SHUTDOWN, rid++, &q, sizeof(q)) == XRT_SUCCESS);
	}
	CHECK(h.wait_exit(5000) == CONV_HOST_EXIT_ORDERLY);

	conv_host_shm_unmap(map, shm_bytes);
	conv_host_handle_close(shm);
	for (const auto &x : outs) {
		conv_host_handle_close(x.handle);
	}
#ifdef _WIN32
	conv_host_handle_close(wake_in_mine);
	conv_host_handle_close(wake_out_mine);
#else
	conv_host_handle_close(wake_in_host);
	conv_host_handle_close(wake_in_mine);
	conv_host_handle_close(wake_out_host);
	conv_host_handle_close(wake_out_mine);
#endif
}
