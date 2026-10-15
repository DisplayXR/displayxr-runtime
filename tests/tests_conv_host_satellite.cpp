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
#include "conv_host_shm.h"

#include "conversion/conv_host_wire.h"
#include "os/os_process.h"
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
#include <d3d11_4.h>
#include <d3d11sdklayers.h>
#include <dxgi1_2.h>
#else
#include <unistd.h>
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
	set_env("DXR_CONV_SIM_CONVERSION_JOB_MS", "50");
}

/*!
 * The test's "service" end of one host, built from the same os_process.h
 * layer the supervisor uses: os_channel_create_for_child, os_process_spawn
 * with kill_on_parent_exit (a job object on Windows, PDEATHSIG on Linux), so
 * a host never outlives the test, and os_process_dup_* for handles.
 */
struct Host
{
	conv_host_chan *chan = nullptr;
	os_process *proc = nullptr;
	bool exited = false;

	~Host()
	{
		conv_host_chan_destroy(&chan);
		kill_now();
	}

	/*!
	 * @p bare_token (Windows): pass the pipe's bare name instead of the
	 * `\\.\pipe\...` path, the second accepted --channel form.
	 */
	bool
	spawn(const std::vector<std::string> &extra_args, bool with_channel = true, bool bare_token = false)
	{
		std::vector<std::string> args;
		os_channel *parent = nullptr;
		uint64_t child_handle = OS_NATIVE_HANDLE_INVALID;
		if (with_channel) {
			char token[256] = {0};
			if (os_channel_create_for_child("dxr-convhost-test", &parent, token, sizeof(token),
			                                &child_handle) != 0) {
				return false;
			}
			std::string t = token;
			const std::string prefix = "\\\\.\\pipe\\";
			if (bare_token && t.compare(0, prefix.size(), prefix) == 0) {
				t = t.substr(prefix.size());
			}
			args.push_back("--channel");
			args.push_back(t);
		}
		// Unsigned build-tree providers under an elevated harness (ADR-054 N4b PR B).
		args.push_back("--trust-enforce");
		args.push_back("0");
		for (const auto &a : extra_args) {
			args.push_back(a);
		}
		std::vector<const char *> argv;
		for (const auto &a : args) {
			argv.push_back(a.c_str());
		}
		os_process_spawn_info info = {};
		info.exe_path = CONV_HOST_EXE;
		info.args = argv.data();
		info.arg_count = (uint32_t)argv.size();
		info.kill_on_parent_exit = true;
		info.channel_child_handle = child_handle;
		if (os_process_spawn(&info, &proc) != 0) {
			os_channel_destroy(&parent);
			os_native_handle_close(child_handle);
			return false;
		}
		if (with_channel) {
			// Windows: waits for the host to connect and checks its pid. A
			// host that exits before connecting leaves chan NULL.
			if (os_channel_accept_child(parent, proc, child_handle, 20000) == 0) {
				chan = conv_host_chan_wrap(parent);
			} else {
				os_channel_destroy(&parent);
			}
		}
		return true;
	}

	std::string
	parent_arg() const
	{
		return std::to_string(os_process_current_pid());
	}

	//! Wait for the process to exit; returns its code, or -1000 on timeout.
	int
	wait_exit(uint32_t timeout_ms)
	{
		if (proc == nullptr) {
			return -1001;
		}
		int32_t code = 0;
		if (os_process_wait(proc, timeout_ms, &code) != 1) {
			return -1000;
		}
		exited = true;
		return (int)code;
	}

	void
	kill_now()
	{
		if (proc == nullptr) {
			return;
		}
		if (!exited) {
			os_process_terminate(proc, 99);
			int32_t code = 0;
			(void)os_process_wait(proc, 5000, &code);
		}
		os_process_destroy(&proc);
	}

	//! A local handle as the host will see it (Windows: duplicated INTO the host).
	uint64_t
	to_host(uint64_t local)
	{
#ifdef _WIN32
		uint64_t remote = 0;
		return os_process_dup_to_child(proc, local, &remote) == 0 ? remote : 0;
#else
		return local; // SCM_RIGHTS duplicates on send
#endif
	}

	//! A received handle as this process can use it (Windows: PULLED from the host).
	uint64_t
	from_host(uint64_t received)
	{
#ifdef _WIN32
		uint64_t local = 0;
		return os_process_dup_from_child(proc, received, false, &local) == 0 ? local : 0;
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

//! One wake direction: *mine is this process's end, *for_host the native to send.
void
make_wake(os_wake &mine, uint64_t &for_host)
{
	REQUIRE(os_wake_create_pair(&mine, &for_host) == 0);
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
	// The bare pipe name (Windows), the second accepted --channel form.
	REQUIRE(h.spawn(a, true, true));
	REQUIRE(h.chan != nullptr);
	REQUIRE(handshake(h));
	// Heartbeats keep arriving; with nothing bound the host leaves on its own.
	const auto t0 = std::chrono::steady_clock::now();
	CHECK(h.wait_exit(15000) == CONV_HOST_EXIT_ORDERLY);
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
	CHECK(ms.count() >= 1000);
}

TEST_CASE("conv_host: a warmed capability counts as bound; no idle exit until it is evicted", "[conv_host]")
{
	// #2056 / roadmap §3.4 "no bound task AND no prewarm": a host holding a warm
	// (prewarmed) model never idle-exits by itself; evicted, it does (exit 0).
	common_env();
	Host h;
	auto a = sim_args(h);
	a.push_back("--idle-exit-ms");
	a.push_back("1500");
	REQUIRE(h.spawn(a));
	REQUIRE(h.chan != nullptr);
	REQUIRE(handshake(h));
	uint32_t rid = 1;
	conv_host_wire_cap_request q;
	conv_host_wire_init(&q, sizeof(q));
	q.cap_id = XRT_CONV_TASK_DEPTH_MONO;
	REQUIRE(h.request_status(CONV_HOST_MSG_WARM, rid++, &q, sizeof(q)) == XRT_SUCCESS);

	// Well past the idle window: still alive.
	CHECK(h.wait_exit(4000) == -1000);

	REQUIRE(h.request_status(CONV_HOST_MSG_EVICT, rid++, &q, sizeof(q)) == XRT_SUCCESS);
	const auto t0 = std::chrono::steady_clock::now();
	CHECK(h.wait_exit(15000) == CONV_HOST_EXIT_ORDERLY);
	const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
	CHECK(ms.count() >= 1000); // the idle window counts from the evict
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
	os_wake wake_in_mine = {};  // we signal: the command ring is non-empty
	os_wake wake_out_mine = {}; // the host signals: the completion ring is non-empty
	uint64_t wake_in_host = 0, wake_out_host = 0;
	make_wake(wake_in_mine, wake_in_host);
	make_wake(wake_out_mine, wake_out_host);
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
		os_wake_signal(&wake_in_mine);
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
			(void)os_wake_wait(&wake_out_mine, 100);
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

	// A job on the same capability, input from the lowest open lane's shm;
	// the blob comes back as a host shm, then JOB_RELEASE frees the host's copy.
	{
		conv_host_wire_job_submit q;
		conv_host_wire_init(&q, sizeof(q));
		q.job_id = 9001;
		q.cap_id = cap_id;
		q.input_kind = XRT_CONV_INPUT_MONO;
		q.image_count = 1;
		q.image_idx[0] = 3;
		q.image_role[0] = XRT_CONV_IMG_COLOR;
		for (uint32_t i = 1; i < XRT_CONV_MAX_FRAME_IMAGES; i++) {
			q.image_idx[i] = CONV_HOST_WIRE_NO_INDEX;
		}
		xrt_conv_wire_pack_params(&params, &q.params);
		const uint32_t jrid = rid++;
		REQUIRE(h.send(CONV_HOST_MSG_JOB_SUBMIT, jrid, &q, sizeof(q)));
		conv_host_wire_frame f;
		std::vector<uint8_t> pl;
		bool done = false;
		while (!done && h.recv(f, pl)) {
			if (f.type != (CONV_HOST_MSG_JOB_SUBMIT | CONV_HOST_MSG_REPLY_BIT)) {
				continue; // JOB_PROGRESS
			}
			done = true;
			CHECK(f.request_id == jrid);
			conv_host_wire_job_done jd;
			REQUIRE(conv_host_wire_receive(&jd, sizeof(jd), pl.data(), f.payload_bytes));
			CHECK(jd.result == XRT_SUCCESS);
			CHECK(jd.job_id == 9001u);
			REQUIRE(jd.blob_bytes >= (uint64_t)W * H * 4);
			REQUIRE(f.handle_count == 1);
			const uint64_t blob = h.from_host(f.handles[0]);
			REQUIRE(blob != 0);
			void *bm = conv_host_shm_map(blob, jd.blob_bytes);
			REQUIRE(bm != nullptr);
			conv_host_shm_unmap(bm, jd.blob_bytes);
			conv_host_handle_close(blob);
		}
		REQUIRE(done);
		conv_host_wire_job_release rel;
		conv_host_wire_init(&rel, sizeof(rel));
		rel.job_id = 9001;
		CHECK(h.send(CONV_HOST_MSG_JOB_RELEASE, 0, &rel, sizeof(rel)));
	}

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
	os_wake_destroy(&wake_in_mine);
	os_wake_destroy(&wake_out_mine);
#ifndef _WIN32
	// POSIX: the host's ends were sent with SCM_RIGHTS; close our copies.
	// (Windows: the same event as ours, closed by os_wake_destroy.)
	conv_host_handle_close(wake_in_host);
	conv_host_handle_close(wake_out_host);
#endif
}


#ifdef _WIN32

namespace {

//! WARP's LUID (0 = none).
uint64_t
warp_luid()
{
	IDXGIFactory1 *f = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f))) {
		return 0;
	}
	uint64_t out = 0;
	IDXGIAdapter1 *a = nullptr;
	for (UINT i = 0; out == 0 && f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
		DXGI_ADAPTER_DESC1 d;
		a->GetDesc1(&d);
		if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
			out = (uint64_t)(uint32_t)d.AdapterLuid.HighPart << 32 | d.AdapterLuid.LowPart;
		}
		a->Release();
	}
	f->Release();
	return out;
}

//! The test's "service" device on WARP, under the debug layer when installed.
struct SvcDev
{
	ID3D11Device5 *dev = nullptr;
	ID3D11DeviceContext4 *ctx = nullptr;
	ID3D11InfoQueue *iq = nullptr;

	~SvcDev()
	{
		if (iq != nullptr) {
			iq->Release();
		}
		if (ctx != nullptr) {
			ctx->Release();
		}
		if (dev != nullptr) {
			dev->Release();
		}
	}

	bool
	create(uint64_t luid)
	{
		IDXGIFactory1 *f = nullptr;
		if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f))) {
			return false;
		}
		IDXGIAdapter1 *a = nullptr, *found = nullptr;
		for (UINT i = 0; found == nullptr && f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
			DXGI_ADAPTER_DESC1 d;
			a->GetDesc1(&d);
			if (((uint64_t)(uint32_t)d.AdapterLuid.HighPart << 32 | d.AdapterLuid.LowPart) == luid) {
				found = a;
			} else {
				a->Release();
			}
		}
		f->Release();
		if (found == nullptr) {
			return false;
		}
		const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
		ID3D11Device *d = nullptr;
		ID3D11DeviceContext *c = nullptr;
		HRESULT hr = D3D11CreateDevice(found, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_DEBUG, &fl, 1,
		                               D3D11_SDK_VERSION, &d, nullptr, &c);
		if (FAILED(hr)) {
			hr = D3D11CreateDevice(found, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &d,
			                       nullptr, &c);
		} else {
			(void)d->QueryInterface(__uuidof(ID3D11InfoQueue), (void **)&iq);
		}
		found->Release();
		if (FAILED(hr)) {
			return false;
		}
		const bool ok = SUCCEEDED(d->QueryInterface(__uuidof(ID3D11Device5), (void **)&dev)) &&
		                SUCCEEDED(c->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&ctx));
		d->Release();
		c->Release();
		return ok;
	}

	uint32_t
	debug_errors()
	{
		if (iq == nullptr) {
			return 0;
		}
		uint32_t n_err = 0;
		const UINT64 n = iq->GetNumStoredMessages();
		for (UINT64 i = 0; i < n; i++) {
			SIZE_T len = 0;
			iq->GetMessage(i, nullptr, &len);
			std::vector<uint8_t> buf(len);
			auto *m = (D3D11_MESSAGE *)buf.data();
			if (SUCCEEDED(iq->GetMessage(i, m, &len)) &&
			    (m->Severity == D3D11_MESSAGE_SEVERITY_ERROR || m->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION)) {
				n_err++;
				std::printf("service-side d3d11 debug layer: %.*s\n", (int)m->DescriptionByteLength,
				            m->pDescription);
			}
		}
		iq->ClearStoredMessages();
		return n_err;
	}

	//! R32_FLOAT texels of @p tex.
	std::vector<float>
	read_r32f(ID3D11Texture2D *tex)
	{
		D3D11_TEXTURE2D_DESC td;
		tex->GetDesc(&td);
		td.Usage = D3D11_USAGE_STAGING;
		td.BindFlags = 0;
		td.MiscFlags = 0;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		std::vector<float> out((size_t)td.Width * td.Height, -1.0f);
		ID3D11Texture2D *st = nullptr;
		if (FAILED(dev->CreateTexture2D(&td, nullptr, &st))) {
			return out;
		}
		ctx->CopyResource(st, tex);
		D3D11_MAPPED_SUBRESOURCE m;
		if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
			for (UINT y = 0; y < td.Height; y++) {
				std::memcpy(&out[(size_t)y * td.Width], (const uint8_t *)m.pData + (size_t)y * m.RowPitch,
				            td.Width * 4u);
			}
			ctx->Unmap(st, 0);
		}
		st->Release();
		return out;
	}
};

} // namespace

TEST_CASE("conv_host: the D3D11 NT-handle lane on WARP: fence values under the debug layer", "[conv_host]")
{
	const uint64_t luid = warp_luid();
	if (luid == 0) {
		SKIP("no WARP adapter");
	}
	SvcDev svc;
	if (!svc.create(luid)) {
		SKIP("no D3D11.4 device on WARP");
	}
	INFO("service-side debug layer: " << (svc.iq != nullptr ? "on" : "not installed"));

	common_env();
	set_env("DXR_CONV_HOST_D3D11_DEBUG", "1"); // the host's device under the debug layer too
	Host h;
	auto a = sim_args(h);
	char lb[32];
	std::snprintf(lb, sizeof(lb), "0x%016llx", (unsigned long long)luid);
	a.push_back("--adapter-luid");
	a.push_back(lb);
	REQUIRE(h.spawn(a));
	set_env("DXR_CONV_HOST_D3D11_DEBUG", nullptr);
	REQUIRE(h.chan != nullptr);

	// Every frame, LOG ones included: the host forwards its debug-layer errors.
	std::vector<std::string> logs;
	auto next = [&](conv_host_wire_frame &f, std::vector<uint8_t> &pl) {
		pl.assign(CONV_HOST_WIRE_MAX_PAYLOAD, 0);
		for (;;) {
			if (!conv_host_chan_recv(h.chan, &f, pl.data())) {
				return false;
			}
			if (f.type == CONV_HOST_MSG_LOG) {
				conv_host_wire_log lg;
				if (conv_host_wire_receive(&lg, sizeof(lg), pl.data(), f.payload_bytes)) {
					lg.text[sizeof(lg.text) - 1] = 0;
					logs.push_back(lg.text);
				}
				continue;
			}
			if (f.type == CONV_HOST_MSG_HEARTBEAT || f.type == CONV_HOST_MSG_STATE_CHANGED) {
				continue;
			}
			return true;
		}
	};
	auto status_of = [&](uint32_t type, uint32_t rid, const void *p, uint32_t n,
	                     std::vector<std::pair<conv_host_wire_frame, std::vector<uint8_t>>> *side,
	                     std::vector<uint64_t> handles = {}) -> int32_t {
		if (!h.send(type, rid, p, n, handles)) {
			return -10000;
		}
		conv_host_wire_frame f;
		std::vector<uint8_t> pl;
		while (next(f, pl)) {
			if (f.type == (type | CONV_HOST_MSG_REPLY_BIT) && f.request_id == rid) {
				conv_host_wire_status st;
				return conv_host_wire_receive(&st, sizeof(st), pl.data(), f.payload_bytes) ? st.result
				                                                                          : -10001;
			}
			if (side != nullptr) {
				side->push_back({f, pl});
			}
		}
		return -10002;
	};

	REQUIRE(handshake(h));
	uint32_t rid = 1;
	const uint32_t cap_id = XRT_CONV_TASK_DEPTH_MONO;
	{
		conv_host_wire_cap_request q;
		conv_host_wire_init(&q, sizeof(q));
		q.cap_id = cap_id;
		REQUIRE(status_of(CONV_HOST_MSG_WARM, rid++, &q, sizeof(q), nullptr) == XRT_SUCCESS);
	}
	const uint64_t stream = 5;
	const uint32_t W = 8, H = 4;
	{
		conv_host_wire_stream_open q;
		conv_host_wire_init(&q, sizeof(q));
		q.cap_id = cap_id;
		q.stream = stream;
		q.input_kind = XRT_CONV_INPUT_MONO;
		q.outputs = XRT_CONV_OUT_DEPTH;
		q.content_hint = XRT_CONV_CONTENT_PHOTO;
		q.max_input_width = W;
		q.max_input_height = H;
		REQUIRE(status_of(CONV_HOST_MSG_STREAM_OPEN, rid++, &q, sizeof(q), nullptr) == XRT_SUCCESS);
	}

	// ALLOC_OUTPUTS: NT-handle depth textures on WARP + the host's output fence.
	struct Out
	{
		uint32_t index;
		ID3D11Texture2D *tex;
	};
	std::vector<Out> outs;
	ID3D11Fence *fence = nullptr;
	uint64_t fence_initial = 0;
	{
		conv_host_wire_alloc_outputs q;
		conv_host_wire_init(&q, sizeof(q));
		q.stream = stream;
		q.width = W;
		q.height = H;
		q.set_count = 2;
		std::vector<std::pair<conv_host_wire_frame, std::vector<uint8_t>>> side;
		REQUIRE(status_of(CONV_HOST_MSG_ALLOC_OUTPUTS, rid++, &q, sizeof(q), &side) == XRT_SUCCESS);
		for (auto &fr : side) {
			if (fr.first.type == CONV_HOST_MSG_SYNC) {
				conv_host_wire_sync sy;
				REQUIRE(conv_host_wire_receive(&sy, sizeof(sy), fr.second.data(), fr.first.payload_bytes));
				CHECK(sy.kind == XRT_CONV_SYNC_FENCE_NT);
				CHECK(sy.direction == CONV_HOST_SYNC_HOST_SIGNALS);
				REQUIRE(fr.first.handle_count == 1);
				HANDLE fh = (HANDLE)(uintptr_t)h.from_host(fr.first.handles[0]);
				REQUIRE(fh != nullptr);
				REQUIRE(SUCCEEDED(svc.dev->OpenSharedFence(fh, __uuidof(ID3D11Fence), (void **)&fence)));
				CloseHandle(fh);
				fence_initial = sy.initial_value;
			} else if (fr.first.type == CONV_HOST_MSG_OUTPUT_IMAGE) {
				conv_host_wire_output_image oi;
				REQUIRE(conv_host_wire_receive(&oi, sizeof(oi), fr.second.data(), fr.first.payload_bytes));
				CHECK(oi.image.memory_kind == XRT_CONV_MEM_WIN32_NT_HANDLE);
				CHECK(oi.image.format == XRT_CONV_FMT_R32_FLOAT);
				REQUIRE(fr.first.handle_count == 1);
				HANDLE th = (HANDLE)(uintptr_t)h.from_host(fr.first.handles[0]);
				REQUIRE(th != nullptr);
				ID3D11Texture2D *t = nullptr;
				REQUIRE(SUCCEEDED(svc.dev->OpenSharedResource1(th, __uuidof(ID3D11Texture2D), (void **)&t)));
				CloseHandle(th);
				outs.push_back({oi.index, t});
			}
		}
	}
	REQUIRE(fence != nullptr); // the SYNC frame precedes the reply
	REQUIRE(outs.size() == 2);
	CHECK(fence->GetCompletedValue() == fence_initial);

	// The lane: CPU input in the shm, as in the CPU case above.
	const size_t sb = u_shm_ring_bytes(sizeof(conv_host_wire_submit_record), CONV_HOST_WIRE_RING_CAPACITY);
	const size_t cb = u_shm_ring_bytes(sizeof(conv_host_wire_complete_record), CONV_HOST_WIRE_RING_CAPACITY);
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
	std::memset((uint8_t *)map + img_off, 0x80, (size_t)img_bytes);
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
		REQUIRE(status_of(CONV_HOST_MSG_REGISTER_IMAGE, rid++, &q, sizeof(q), nullptr) == XRT_SUCCESS);
	}
	os_wake wake_in_mine = {}, wake_out_mine = {};
	uint64_t wake_in_host = 0, wake_out_host = 0;
	make_wake(wake_in_mine, wake_in_host);
	make_wake(wake_out_mine, wake_out_host);
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
		REQUIRE(status_of(CONV_HOST_MSG_LANE_OPEN, rid++, &q, sizeof(q), nullptr,
		                  {shm, wake_in_host, wake_out_host}) == XRT_SUCCESS);
	}

	// N frames, one at a time: each completion carries the NEXT fence value,
	// the CPU sees the fence reach it, and only then are the bytes read.
	constexpr uint32_t N = 8;
	uint64_t prev = fence_initial;
	HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	for (uint32_t i = 0; i < N; i++) {
		conv_host_wire_submit_record rec;
		std::memset(&rec, 0, sizeof(rec));
		rec.seq = 100 + i;
		rec.stream = stream;
		rec.op = CONV_HOST_RING_OP_CONVERT;
		rec.image_count = 1;
		rec.in_image_idx[0] = 3;
		rec.in_image_role[0] = XRT_CONV_IMG_COLOR;
		for (uint32_t k = 1; k < XRT_CONV_MAX_FRAME_IMAGES; k++) {
			rec.in_image_idx[k] = CONV_HOST_WIRE_NO_INDEX;
		}
		rec.in_sync_idx = CONV_HOST_WIRE_NO_INDEX;
		xrt_conv_params params = {};
		params.struct_size = sizeof(params);
		params.frame_id = 1 + i;
		params.strength = 1.0f;
		xrt_conv_wire_pack_params(&params, &rec.params);
		bool wake = false;
		REQUIRE(u_shm_ring_push(&submit, &rec, &wake) == U_SHM_RING_OK);
		if (wake) {
			os_wake_signal(&wake_in_mine);
		}
		conv_host_wire_complete_record comp;
		bool got = false;
		const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!got && std::chrono::steady_clock::now() < end) {
			if (u_shm_ring_pop(&complete, &comp) == U_SHM_RING_OK) {
				got = true;
			} else if (u_shm_ring_prepare_wait(&complete)) {
				(void)os_wake_wait(&wake_out_mine, 100);
				u_shm_ring_finish_wait(&complete);
			}
		}
		REQUIRE(got);
		INFO("frame " << i << " status " << comp.status);
		CHECK(comp.seq == rec.seq);
		REQUIRE(comp.status == XRT_SUCCESS);
		REQUIRE(comp.out_sync_idx == 0u);
		CHECK(comp.out_fence_value == prev + 1); // strictly increasing, one per frame
		prev = comp.out_fence_value;
		// THE FENCE RULE: observe on the CPU, bounded.
		REQUIRE(SUCCEEDED(fence->SetEventOnCompletion(comp.out_fence_value, ev)));
		REQUIRE(WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0);
		const uint32_t oi = comp.out_image_idx[CONV_HOST_WIRE_OUT_DEPTH];
		ID3D11Texture2D *t = nullptr;
		for (const auto &o : outs) {
			if (o.index == oi) {
				t = o.tex;
			}
		}
		REQUIRE(t != nullptr);
		const std::vector<float> px = svc.read_r32f(t);
		for (uint32_t y = 0; y < H; y++) {
			const float expect = ((float)y + 0.5f) / (float)H;
			CHECK(px[(size_t)y * W] == Catch::Approx(expect));
			CHECK(px[(size_t)y * W + W - 1] == Catch::Approx(expect));
		}
	}
	CHECK(fence->GetCompletedValue() == prev);

	{
		conv_host_wire_shutdown q;
		conv_host_wire_init(&q, sizeof(q));
		q.orderly = 1;
		CHECK(status_of(CONV_HOST_MSG_SHUTDOWN, rid++, &q, sizeof(q), nullptr) == XRT_SUCCESS);
	}
	CHECK(h.wait_exit(5000) == CONV_HOST_EXIT_ORDERLY);

	// Both sides of the share clean under the debug layer.
	CHECK(svc.debug_errors() == 0u);
	bool host_debug_on = false;
	for (const auto &l : logs) {
		INFO("host log: " << l);
		CHECK(l.find("d3d11 debug layer:") == std::string::npos);
		host_debug_on = host_debug_on || l.find("D3D11 debug layer on") != std::string::npos;
	}
	std::printf("conv_host NT lane on WARP: %u frames, fence %llu -> %llu, host debug layer %s, service %s\n", N,
	            (unsigned long long)fence_initial, (unsigned long long)prev, host_debug_on ? "on" : "off",
	            svc.iq != nullptr ? "on" : "off");

	CloseHandle(ev);
	for (auto &o : outs) {
		o.tex->Release();
	}
	fence->Release();
	conv_host_shm_unmap(map, shm_bytes);
	conv_host_handle_close(shm);
	os_wake_destroy(&wake_in_mine);
	os_wake_destroy(&wake_out_mine);
}

#endif // _WIN32
