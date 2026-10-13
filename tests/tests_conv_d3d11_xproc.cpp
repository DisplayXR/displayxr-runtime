// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-053 P2 (#2035), roadmap conversion-providers.md §11.2: what the
 *         satellite's D3D11 lane relies on, checked ACROSS A PROCESS BOUNDARY
 *         (a child, tests_conv_d3d11_xproc_helper, plays the host).
 *
 *  1. A shared NT-handle texture and a shared fence created in one process,
 *     pulled into another with DuplicateHandle (the §3.5 rule: the service
 *     pulls), opened there, signalled there and observed here: on the CPU
 *     (SetEventOnCompletion) and, on this test's own throwaway device, with a
 *     GPU queue wait. Runs on WARP, so in CI.
 *  2. The fence rule's reason: a queue wait on a fence whose signaller died.
 *     The waiter is another child (a wait that never returns must not wedge
 *     the test); the measured behaviour is pinned below. WARP always; the
 *     machine's hardware adapters too with DXR_TEST_D3D11_HW=1 (headless:
 *     devices, textures and fences only, nothing is presented).
 *  3. A shared texture / fence opened on the WRONG adapter (hybrid boxes):
 *     every pair of distinct adapters; SKIPs with fewer than two.
 *
 * The parent's devices run under the D3D11 debug layer when it is installed;
 * an ERROR / CORRUPTION message fails the case.
 *
 * Measured on the win box (hybrid Intel iGPU + NVIDIA dGPU + WARP,
 * 2026-10-13), recorded in the CHECKs and in roadmap §11.2:
 *  - case 1 on WARP: signal -> CPU observe ~15 ms, a queued GPU wait releases
 *    at once;
 *  - case 2: the dead signaller's fence jumps to UINT64_MAX within ~2 ms of
 *    the kill on every adapter (CPU waits return); the GPU queue behind a
 *    wait is released at once on WARP and the iGPU, but stays stalled for
 *    ~3.8 s on the dGPU (4 runs: 3.78-3.82 s);
 *  - case 3: every cross-adapter open of a shared texture or fence fails
 *    with E_INVALIDARG (all six pairs): loud, not a silent drop.
 */

#include "catch_amalgamated.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <d3d11sdklayers.h>
#include <dxgi1_2.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef CONV_D3D11_XPROC_HELPER
#error "CONV_D3D11_XPROC_HELPER (path of tests_conv_d3d11_xproc_helper) must be defined"
#endif

namespace {

template <typename T>
void
rel(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

uint64_t
luid_of(const DXGI_ADAPTER_DESC1 &d)
{
	return (uint64_t)(uint32_t)d.AdapterLuid.HighPart << 32 | d.AdapterLuid.LowPart;
}

struct AdapterInfo
{
	uint64_t luid;
	bool software;
	std::string name;
};

std::vector<AdapterInfo>
adapters()
{
	std::vector<AdapterInfo> out;
	IDXGIFactory1 *f = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f))) {
		return out;
	}
	IDXGIAdapter1 *a = nullptr;
	for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
		DXGI_ADAPTER_DESC1 d;
		a->GetDesc1(&d);
		char name[128] = {0};
		WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof(name), nullptr, nullptr);
		// The display-only "Basic Display Adapter" has no D3D device; it is
		// filtered by Dev::create failing.
		out.push_back({luid_of(d), (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0, name});
		a->Release();
	}
	f->Release();
	return out;
}

bool
find_warp(AdapterInfo &out)
{
	for (const auto &a : adapters()) {
		if (a.software) {
			out = a;
			return true;
		}
	}
	return false;
}

bool
hw_enabled()
{
	const char *v = std::getenv("DXR_TEST_D3D11_HW");
	return v != nullptr && v[0] == '1';
}

struct Dev
{
	ID3D11Device5 *dev = nullptr;
	ID3D11DeviceContext4 *ctx = nullptr;
	ID3D11InfoQueue *iq = nullptr;
	bool debug = false;

	~Dev()
	{
		rel(iq);
		rel(ctx);
		rel(dev);
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
			if (luid_of(d) == luid) {
				found = a;
			} else {
				a->Release();
			}
		}
		f->Release();
		if (found == nullptr) {
			return false;
		}
		const D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
		ID3D11Device *d = nullptr;
		ID3D11DeviceContext *c = nullptr;
		HRESULT hr = D3D11CreateDevice(found, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_DEBUG, fl, 2,
		                               D3D11_SDK_VERSION, &d, nullptr, &c);
		debug = SUCCEEDED(hr);
		if (FAILED(hr)) {
			hr = D3D11CreateDevice(found, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, fl, 2, D3D11_SDK_VERSION, &d,
			                       nullptr, &c);
		}
		found->Release();
		if (FAILED(hr)) {
			return false;
		}
		const bool ok = SUCCEEDED(d->QueryInterface(__uuidof(ID3D11Device5), (void **)&dev)) &&
		                SUCCEEDED(c->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&ctx));
		if (debug) {
			(void)d->QueryInterface(__uuidof(ID3D11InfoQueue), (void **)&iq);
		}
		d->Release();
		c->Release();
		return ok;
	}

	//! ERROR / CORRUPTION messages since the last call (each one also printed).
	uint32_t
	debug_errors()
	{
		if (iq == nullptr) {
			return 0;
		}
		uint32_t errors = 0;
		const UINT64 n = iq->GetNumStoredMessages();
		for (UINT64 i = 0; i < n; i++) {
			SIZE_T len = 0;
			iq->GetMessage(i, nullptr, &len);
			std::vector<uint8_t> buf(len);
			auto *m = (D3D11_MESSAGE *)buf.data();
			if (SUCCEEDED(iq->GetMessage(i, m, &len)) &&
			    (m->Severity == D3D11_MESSAGE_SEVERITY_ERROR || m->Severity == D3D11_MESSAGE_SEVERITY_CORRUPTION)) {
				errors++;
				std::printf("d3d11 debug layer: %.*s\n", (int)m->DescriptionByteLength, m->pDescription);
			}
		}
		iq->ClearStoredMessages();
		return errors;
	}
};

/*!
 * One child with stdin / stdout pipes, inside a kill-on-close job so it
 * never outlives the test.
 */
struct Child
{
	PROCESS_INFORMATION pi = {};
	HANDLE job = nullptr;
	HANDLE in_w = nullptr;  //!< our end of its stdin
	HANDLE out_r = nullptr; //!< our end of its stdout
	std::string pending;

	~Child()
	{
		kill(99);
		if (in_w != nullptr) {
			CloseHandle(in_w);
		}
		if (out_r != nullptr) {
			CloseHandle(out_r);
		}
		if (job != nullptr) {
			CloseHandle(job);
		}
	}

	bool
	start(const std::string &args)
	{
		SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
		HANDLE in_r = nullptr, out_w = nullptr;
		if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 0)) {
			return false;
		}
		SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
		SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
		STARTUPINFOA si = {};
		si.cb = sizeof(si);
		si.dwFlags = STARTF_USESTDHANDLES;
		si.hStdInput = in_r;
		si.hStdOutput = out_w;
		si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
		std::string cmd = std::string("\"") + CONV_D3D11_XPROC_HELPER + "\" " + args;
		std::vector<char> buf(cmd.begin(), cmd.end());
		buf.push_back(0);
		job = CreateJobObjectA(nullptr, nullptr);
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
		li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
		const BOOL ok = CreateProcessA(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr,
		                               nullptr, &si, &pi);
		CloseHandle(in_r);
		CloseHandle(out_w);
		if (!ok) {
			return false;
		}
		AssignProcessToJobObject(job, pi.hProcess);
		ResumeThread(pi.hThread);
		return true;
	}

	bool
	send(const std::string &line)
	{
		const std::string l = line + "\n";
		DWORD n = 0;
		return WriteFile(in_w, l.data(), (DWORD)l.size(), &n, nullptr) && n == l.size();
	}

	//! The next stdout line, or "" after @p timeout_ms / at EOF.
	std::string
	line(uint32_t timeout_ms)
	{
		const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
		for (;;) {
			const size_t nl = pending.find('\n');
			if (nl != std::string::npos) {
				std::string l = pending.substr(0, nl);
				pending.erase(0, nl + 1);
				if (!l.empty() && l.back() == '\r') {
					l.pop_back();
				}
				return l;
			}
			DWORD avail = 0;
			if (!PeekNamedPipe(out_r, nullptr, 0, nullptr, &avail, nullptr)) {
				return ""; // EOF: the child is gone
			}
			if (avail > 0) {
				std::vector<char> b(avail);
				DWORD n = 0;
				if (ReadFile(out_r, b.data(), avail, &n, nullptr) && n > 0) {
					pending.append(b.data(), n);
				}
				continue;
			}
			if (std::chrono::steady_clock::now() >= end) {
				return "";
			}
			Sleep(2);
		}
	}

	//! A handle value of the child's, duplicated into this process.
	HANDLE
	pull(uint64_t child_value)
	{
		HANDLE local = nullptr;
		if (!DuplicateHandle(pi.hProcess, (HANDLE)(uintptr_t)child_value, GetCurrentProcess(), &local, 0, FALSE,
		                     DUPLICATE_SAME_ACCESS)) {
			return nullptr;
		}
		return local;
	}

	//! A local handle duplicated into the child; its value there.
	uint64_t
	push(HANDLE local)
	{
		HANDLE remote = nullptr;
		if (!DuplicateHandle(GetCurrentProcess(), local, pi.hProcess, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
			return 0;
		}
		return (uint64_t)(uintptr_t)remote;
	}

	void
	kill(UINT code)
	{
		if (pi.hProcess == nullptr) {
			return;
		}
		TerminateProcess(pi.hProcess, code);
		WaitForSingleObject(pi.hProcess, 5000);
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		pi = {};
	}
};

struct SignallerHandles
{
	uint64_t tex = 0, fence = 0;
	std::string error;
};

SignallerHandles
start_signaller(Child &c, uint64_t luid)
{
	SignallerHandles h;
	char a[64];
	std::snprintf(a, sizeof(a), "signaller %llx", (unsigned long long)luid);
	if (!c.start(a)) {
		h.error = "spawn failed";
		return h;
	}
	const std::string l = c.line(20000);
	unsigned long long t = 0, f = 0;
	if (std::sscanf(l.c_str(), "READY tex=%llx fence=%llx", &t, &f) != 2) {
		h.error = l.empty() ? "no READY" : l;
		return h;
	}
	h.tex = t;
	h.fence = f;
	return h;
}

std::vector<uint32_t>
read_back(Dev &d, ID3D11Texture2D *tex)
{
	D3D11_TEXTURE2D_DESC td;
	tex->GetDesc(&td);
	td.Usage = D3D11_USAGE_STAGING;
	td.BindFlags = 0;
	td.MiscFlags = 0;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	std::vector<uint32_t> out(td.Width * td.Height, 0);
	ID3D11Texture2D *st = nullptr;
	if (FAILED(d.dev->CreateTexture2D(&td, nullptr, &st))) {
		return out;
	}
	d.ctx->CopyResource(st, tex);
	D3D11_MAPPED_SUBRESOURCE m;
	if (SUCCEEDED(d.ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
		for (UINT y = 0; y < td.Height; y++) {
			std::memcpy(&out[y * td.Width], (const uint8_t *)m.pData + y * m.RowPitch, td.Width * 4u);
		}
		d.ctx->Unmap(st, 0);
	}
	st->Release();
	return out;
}

double
ms_since(std::chrono::steady_clock::time_point t0)
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

//! The fields of one waiter STATE / DONE line.
struct WaiterState
{
	unsigned long long t = 0, completed = 0;
	int query = -1, cpu_event = -1;
	unsigned long removed = 0;
	bool parsed = false;
};

WaiterState
parse_state(const std::string &l, const char *tag)
{
	WaiterState s;
	std::string fmt = std::string(tag) + " t=%llu query=%d cpu_event=%d completed=%llu removed=0x%lx";
	s.parsed = std::sscanf(l.c_str(), fmt.c_str(), &s.t, &s.query, &s.cpu_event, &s.completed, &s.removed) == 5;
	return s;
}

} // namespace

TEST_CASE("d3d11 xproc: a shared NT-handle texture + shared fence cross a process boundary on WARP", "[conv_d3d11_xproc]")
{
	AdapterInfo warp;
	if (!find_warp(warp)) {
		SKIP("no WARP adapter");
	}
	Dev d;
	if (!d.create(warp.luid)) {
		SKIP("no D3D11.4 device on WARP");
	}
	INFO("debug layer: " << (d.debug ? "on" : "not installed"));

	Child host;
	const SignallerHandles sh = start_signaller(host, warp.luid);
	if (sh.error.rfind("ERROR fence", 0) == 0) {
		SKIP("WARP has no shared fences in the child (" << sh.error << "): fence cases run on hardware only");
	}
	if (sh.error.rfind("ERROR texture", 0) == 0) {
		SKIP("WARP has no shared NT-handle textures (" << sh.error << "): hardware only");
	}
	REQUIRE(sh.error.empty());

	// The §3.5 rule: this process (the "service") PULLS the host's handles.
	HANDLE tex_h = host.pull(sh.tex);
	HANDLE fence_h = host.pull(sh.fence);
	REQUIRE(tex_h != nullptr);
	REQUIRE(fence_h != nullptr);

	ID3D11Texture2D *tex = nullptr;
	REQUIRE(SUCCEEDED(d.dev->OpenSharedResource1(tex_h, __uuidof(ID3D11Texture2D), (void **)&tex)));
	ID3D11Fence *fence = nullptr;
	REQUIRE(SUCCEEDED(d.dev->OpenSharedFence(fence_h, __uuidof(ID3D11Fence), (void **)&fence)));
	CHECK(fence->GetCompletedValue() == 0u);

	// CPU observe (the fence rule's path).
	HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	REQUIRE(SUCCEEDED(fence->SetEventOnCompletion(1, ev)));
	const auto t0 = std::chrono::steady_clock::now();
	REQUIRE(host.send("signal 1"));
	CHECK(host.line(5000) == "OK");
	REQUIRE(WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0);
	const double observe_ms = ms_since(t0);
	std::printf("xproc WARP: signal -> CPU observe %.3f ms\n", observe_ms);
	CHECK(fence->GetCompletedValue() >= 1u);
	const std::vector<uint32_t> px = read_back(d, tex);
	CHECK(px == std::vector<uint32_t>(px.size(), 0xFF4080C0u));

	// A GPU queue wait on this test's own device (never a compositor's): it
	// is released by the other process's signal.
	D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
	ID3D11Query *q = nullptr;
	REQUIRE(SUCCEEDED(d.dev->CreateQuery(&qd, &q)));
	REQUIRE(SUCCEEDED(d.ctx->Wait(fence, 2)));
	d.ctx->End(q);
	d.ctx->Flush();
	BOOL done = FALSE;
	Sleep(50);
	CHECK(d.ctx->GetData(q, &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_FALSE); // still waiting
	REQUIRE(host.send("signal 2"));
	CHECK(host.line(5000) == "OK");
	const auto t1 = std::chrono::steady_clock::now();
	while (!(d.ctx->GetData(q, &done, sizeof(done), 0) == S_OK && done) && ms_since(t1) < 5000) {
		Sleep(1);
	}
	CHECK(done);
	std::printf("xproc WARP: signal -> GPU queue wait released %.3f ms\n", ms_since(t1));

	CHECK(d.debug_errors() == 0u);
	host.send("exit");
	rel(q);
	rel(fence);
	rel(tex);
	CloseHandle(ev);
	CloseHandle(fence_h);
	CloseHandle(tex_h);
}

namespace {

struct DeadSignallerResult
{
	bool ran = false;
	std::string skip;
	std::vector<std::string> lines; //!< the waiter's STATE / DONE / RELEASE lines
	WaiterState final_state;
	bool release_ok = false;
	bool parent_cpu_event = false; //!< this process's SetEventOnCompletion after the kill
	double parent_event_ms = -1.0; //!< kill -> this process's CPU event (-1 = never, within 5 s)
	unsigned long long parent_completed = 0;
	DWORD signaller_exit = 0;
};

/*!
 * The signaller creates the fence, the waiter (a second child) enqueues a
 * GPU wait for value 1 on its own device; the signaller is killed without
 * signalling. Everything the waiter sees for @p window_ms is recorded.
 */
DeadSignallerResult
dead_signaller(const AdapterInfo &a, uint32_t window_ms)
{
	DeadSignallerResult r;
	Child host;
	const SignallerHandles sh = start_signaller(host, a.luid);
	if (!sh.error.empty()) {
		r.skip = "signaller on '" + a.name + "': " + sh.error;
		return r;
	}
	HANDLE fence_h = host.pull(sh.fence);
	if (fence_h == nullptr) {
		r.skip = "pull failed";
		return r;
	}
	Dev d;
	ID3D11Fence *mine = nullptr;
	HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (d.create(a.luid) && SUCCEEDED(d.dev->OpenSharedFence(fence_h, __uuidof(ID3D11Fence), (void **)&mine))) {
		(void)mine->SetEventOnCompletion(1, ev);
	}

	Child waiter;
	char args[96];
	std::snprintf(args, sizeof(args), "waiter %llx %u", (unsigned long long)a.luid, window_ms);
	if (!waiter.start(args)) {
		r.skip = "waiter spawn failed";
		CloseHandle(fence_h);
		return r;
	}
	const uint64_t remote = waiter.push(fence_h);
	char cmd[64];
	std::snprintf(cmd, sizeof(cmd), "fence %llx 1", (unsigned long long)remote);
	waiter.send(cmd);
	const std::string w = waiter.line(20000);
	if (w.rfind("WAITING", 0) != 0) {
		r.skip = "waiter: " + (w.empty() ? std::string("no WAITING") : w);
		CloseHandle(fence_h);
		return r;
	}
	r.lines.push_back(w);
	Sleep(200); // the wait is queued and the queue is stalled behind it

	// The host dies before signalling.
	const auto t_kill = std::chrono::steady_clock::now();
	TerminateProcess(host.pi.hProcess, 3);
	if (mine != nullptr && WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0) {
		r.parent_event_ms = ms_since(t_kill);
	}
	WaitForSingleObject(host.pi.hProcess, 5000);
	GetExitCodeProcess(host.pi.hProcess, &r.signaller_exit);
	host.kill(3);

	for (;;) {
		const std::string l = waiter.line(window_ms + 10000);
		if (l.empty()) {
			break;
		}
		r.lines.push_back(l);
		if (l.rfind("DONE", 0) == 0) {
			r.final_state = parse_state(l, "DONE");
		}
		if (l.rfind("RELEASE", 0) == 0) {
			r.release_ok = l.rfind("RELEASE ok", 0) == 0;
			break;
		}
	}
	r.parent_cpu_event = WaitForSingleObject(ev, 0) == WAIT_OBJECT_0;
	if (mine != nullptr) {
		r.parent_completed = mine->GetCompletedValue();
	}
	rel(mine);
	CloseHandle(ev);
	CloseHandle(fence_h);
	r.ran = true;
	return r;
}

void
print(const char *who, const DeadSignallerResult &r)
{
	std::printf("dead signaller on %s: signaller exit %lu\n", who, (unsigned long)r.signaller_exit);
	for (const auto &l : r.lines) {
		std::printf("  waiter: %s\n", l.c_str());
	}
	std::printf("  this process: cpu_event=%d (%.1f ms after the kill) completed=0x%llx\n", r.parent_cpu_event ? 1 : 0,
	            r.parent_event_ms, r.parent_completed);
}

} // namespace

TEST_CASE("d3d11 xproc: a queue wait on a fence whose signaller died (the fence rule's reason)", "[conv_d3d11_xproc]")
{
	std::vector<AdapterInfo> targets;
	AdapterInfo warp;
	if (find_warp(warp)) {
		targets.push_back(warp);
	}
	if (hw_enabled()) {
		for (const auto &a : adapters()) {
			if (!a.software) {
				targets.push_back(a);
			}
		}
	}
	if (targets.empty()) {
		SKIP("no WARP adapter");
	}
	// DXR_TEST_DEAD_SIGNALLER_MS lengthens the window for a manual run.
	const char *wenv = std::getenv("DXR_TEST_DEAD_SIGNALLER_MS");
	const uint32_t WINDOW_MS = wenv != nullptr && std::atoi(wenv) > 0 ? (uint32_t)std::atoi(wenv) : 3000u;
	for (const auto &a : targets) {
		DYNAMIC_SECTION("adapter " << a.name)
		{
			const DeadSignallerResult r = dead_signaller(a, WINDOW_MS);
			if (!r.ran) {
				// A hardware adapter without D3D (the display-only basic
				// adapter) or WARP without shared fences.
				SKIP(r.skip);
			}
			print(a.name.c_str(), r);
			REQUIRE(r.final_state.parsed);
			// THE MEASURED BEHAVIOUR (win box 2026-10-13: WARP, Intel UHD,
			// NVIDIA RTX 3080 Laptop; recorded in roadmap §11.2):
			//  - the OS does NOT leave the fence where the dead signaller
			//    left it: within ~2 ms of the kill it reads UINT64_MAX in
			//    every process that holds it, and every SetEventOnCompletion
			//    fires. A CPU observe therefore RETURNS — and returns
			//    "reached" for any value: a fence at UINT64_MAX means "the
			//    signaller is gone", never "the frame is done" (the lane must
			//    treat it as a failure, d3d11_conv_host_lane observe_fence);
			//  - no device removal is reported, and the waiting device
			//    releases in milliseconds;
			//  - the GPU queue wait is vendor-dependent: WARP and the Intel
			//    iGPU release the queued wait at once; on the NVIDIA dGPU
			//    the queue stayed stalled ~3.8 s after the kill (4 runs,
			//    3.78-3.82 s) although the fence read UINT64_MAX after ~1 ms.
			//    A compositor context waiting on a host fence would freeze
			//    for ~230 frames at 60 Hz: the fence rule.
			CHECK(r.final_state.completed == UINT64_MAX);
			CHECK(r.final_state.cpu_event == 1);
			CHECK(r.final_state.removed == 0u);
			CHECK(r.final_state.t >= WINDOW_MS);
			CHECK(r.parent_cpu_event);
			CHECK(r.parent_completed == UINT64_MAX);
			CHECK(r.parent_event_ms >= 0.0);
			CHECK(r.parent_event_ms < 2000.0);
			if (a.software) {
				CHECK(r.final_state.query == 1); // WARP releases the queue
			}
			std::printf("  GPU queue behind the wait: %s after %u ms\n",
			            r.final_state.query == 1 ? "released" : "STILL STALLED", WINDOW_MS);
			CHECK(r.release_ok);
		}
	}
}

TEST_CASE("d3d11 xproc: a shared texture or fence opened on the wrong adapter fails loudly", "[conv_d3d11_xproc]")
{
	std::vector<AdapterInfo> usable;
	for (const auto &a : adapters()) {
		if (a.software || hw_enabled()) {
			Dev probe;
			if (probe.create(a.luid)) {
				usable.push_back(a);
			}
		}
	}
	if (usable.size() < 2) {
		SKIP("needs two D3D11 adapters (hybrid box, DXR_TEST_D3D11_HW=1); found " << usable.size());
	}
	for (const auto &src : usable) {
		for (const auto &dst : usable) {
			if (src.luid == dst.luid) {
				continue;
			}
			DYNAMIC_SECTION(src.name << " -> " << dst.name)
			{
				Child host;
				const SignallerHandles sh = start_signaller(host, src.luid);
				REQUIRE(sh.error.empty());
				HANDLE tex_h = host.pull(sh.tex);
				HANDLE fence_h = host.pull(sh.fence);
				REQUIRE(tex_h != nullptr);
				REQUIRE(fence_h != nullptr);
				Dev d;
				REQUIRE(d.create(dst.luid));
				ID3D11Texture2D *tex = nullptr;
				const HRESULT th =
				    d.dev->OpenSharedResource1(tex_h, __uuidof(ID3D11Texture2D), (void **)&tex);
				ID3D11Fence *fence = nullptr;
				const HRESULT fh = d.dev->OpenSharedFence(fence_h, __uuidof(ID3D11Fence), (void **)&fence);
				std::printf("wrong adapter %s -> %s: texture 0x%08lx, fence 0x%08lx\n", src.name.c_str(),
				            dst.name.c_str(), (unsigned long)th, (unsigned long)fh);
				if (SUCCEEDED(th)) {
					// Not loud: say whether the bytes made it.
					const std::vector<uint32_t> px = read_back(d, tex);
					std::printf("  opened; first pixel 0x%08x (expected 0xff4080c0)\n", px.empty() ? 0 : px[0]);
				}
				CHECK(FAILED(th));
				CHECK(FAILED(fh));
				(void)d.debug_errors(); // an open failure is reported by the debug layer too: expected
				rel(fence);
				rel(tex);
				CloseHandle(fence_h);
				CloseHandle(tex_h);
				host.send("exit");
			}
		}
	}
}
