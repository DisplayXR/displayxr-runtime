// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Child process for tests_conv_d3d11_xproc (ADR-053 P2, #2035): one
 *         side of a cross-process D3D11 share, driven line by line over
 *         stdin / stdout.
 *
 * Modes (argv[1]), the adapter given by LUID (argv[2], hex):
 *
 *  signaller <luid>
 *      Creates a D3D11.4 device, a shared NT-handle texture (16x8 RGBA8,
 *      every pixel 0xFF4080C0) and a shared fence (D3D11_FENCE_FLAG_SHARED),
 *      prints `READY tex=<hex> fence=<hex>` (handle values in THIS process;
 *      the parent pulls them with DuplicateHandle, the §3.5 rule), then obeys
 *      `signal <n>` (Signal + Flush on its immediate context, prints `OK`)
 *      and `exit`. The parent kills it with TerminateProcess to play a host
 *      that died before signalling.
 *
 *  waiter <luid> <ms>
 *      Reads `fence <hex> <value>` (a handle the parent duplicated INTO this
 *      process), opens the fence, enqueues ID3D11DeviceContext4::Wait(fence,
 *      value) followed by an event query on its immediate context, Flushes,
 *      arms SetEventOnCompletion(value) on a CPU event and prints `WAITING`.
 *      Then for <ms> it polls, without ever blocking: the query (GetData
 *      with DONOTFLUSH), the CPU event, the fence's completed value and the
 *      device-removed reason, printing one `STATE ...` line per change. Then
 *      `DONE ...` with the final state, and the device is released on a
 *      worker thread bounded to 5 s (`RELEASE ok <ms>` / `RELEASE hung`).
 *      This is the queue wait the fence rule forbids on a compositor
 *      context: measured here, in a throwaway process, so a wait that never
 *      returns cannot wedge the test.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

uint64_t
luid_of(const DXGI_ADAPTER_DESC1 &d)
{
	return (uint64_t)(uint32_t)d.AdapterLuid.HighPart << 32 | d.AdapterLuid.LowPart;
}

bool
make_device(uint64_t luid, ID3D11Device5 **dev, ID3D11DeviceContext4 **ctx)
{
	IDXGIFactory1 *f = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f))) {
		return false;
	}
	IDXGIAdapter1 *a = nullptr;
	IDXGIAdapter1 *found = nullptr;
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
	HRESULT hr = D3D11CreateDevice(found, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, fl, 2, D3D11_SDK_VERSION, &d, nullptr,
	                               &c);
	found->Release();
	if (FAILED(hr)) {
		return false;
	}
	const bool ok = SUCCEEDED(d->QueryInterface(__uuidof(ID3D11Device5), (void **)dev)) &&
	                SUCCEEDED(c->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)ctx));
	d->Release();
	c->Release();
	return ok;
}

void
say(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);
	fputc('\n', stdout);
	fflush(stdout);
}

int
signaller(uint64_t luid)
{
	ID3D11Device5 *dev = nullptr;
	ID3D11DeviceContext4 *ctx = nullptr;
	if (!make_device(luid, &dev, &ctx)) {
		say("ERROR device");
		return 2;
	}
	constexpr UINT W = 16, H = 8;
	uint32_t px[W * H];
	for (uint32_t &p : px) {
		p = 0xFF4080C0u;
	}
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = W;
	td.Height = H;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	D3D11_SUBRESOURCE_DATA sd = {px, W * 4, 0};
	ID3D11Texture2D *tex = nullptr;
	HANDLE tex_h = nullptr;
	HRESULT hr = dev->CreateTexture2D(&td, &sd, &tex);
	if (SUCCEEDED(hr)) {
		IDXGIResource1 *r = nullptr;
		hr = tex->QueryInterface(__uuidof(IDXGIResource1), (void **)&r);
		if (SUCCEEDED(hr)) {
			hr = r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
			                           &tex_h);
			r->Release();
		}
	}
	if (FAILED(hr)) {
		say("ERROR texture 0x%08lx", (unsigned long)hr);
		return 3;
	}
	ID3D11Fence *fence = nullptr;
	HANDLE fence_h = nullptr;
	hr = dev->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void **)&fence);
	if (SUCCEEDED(hr)) {
		hr = fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence_h);
	}
	if (FAILED(hr)) {
		say("ERROR fence 0x%08lx", (unsigned long)hr);
		return 4;
	}
	ctx->Flush(); // the initial data is on the GPU before anyone opens it
	say("READY tex=%llx fence=%llx", (unsigned long long)(uintptr_t)tex_h, (unsigned long long)(uintptr_t)fence_h);

	char line[256];
	while (fgets(line, sizeof(line), stdin) != nullptr) {
		unsigned long long v = 0;
		if (sscanf(line, "signal %llu", &v) == 1) {
			hr = ctx->Signal(fence, v);
			ctx->Flush();
			say(SUCCEEDED(hr) ? "OK" : "ERROR signal");
		} else if (strncmp(line, "exit", 4) == 0) {
			break;
		}
	}
	CloseHandle(fence_h);
	CloseHandle(tex_h);
	fence->Release();
	tex->Release();
	ctx->Release();
	dev->Release();
	return 0;
}

struct ReleaseJob
{
	ID3D11Fence *fence;
	ID3D11Query *query;
	ID3D11DeviceContext4 *ctx;
	ID3D11Device5 *dev;
};

DWORD WINAPI
release_thread(void *p)
{
	ReleaseJob *j = (ReleaseJob *)p;
	j->query->Release();
	j->fence->Release();
	j->ctx->ClearState();
	j->ctx->Flush();
	j->ctx->Release();
	j->dev->Release();
	return 0;
}

int
waiter(uint64_t luid, uint32_t ms)
{
	ID3D11Device5 *dev = nullptr;
	ID3D11DeviceContext4 *ctx = nullptr;
	if (!make_device(luid, &dev, &ctx)) {
		say("ERROR device");
		return 2;
	}
	char line[256];
	unsigned long long h = 0, value = 0;
	if (fgets(line, sizeof(line), stdin) == nullptr || sscanf(line, "fence %llx %llu", &h, &value) != 2) {
		say("ERROR protocol");
		return 5;
	}
	ID3D11Fence *fence = nullptr;
	HRESULT hr = dev->OpenSharedFence((HANDLE)(uintptr_t)h, __uuidof(ID3D11Fence), (void **)&fence);
	CloseHandle((HANDLE)(uintptr_t)h);
	if (FAILED(hr)) {
		say("ERROR open_fence 0x%08lx", (unsigned long)hr);
		return 6;
	}
	D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
	ID3D11Query *q = nullptr;
	if (FAILED(dev->CreateQuery(&qd, &q))) {
		say("ERROR query");
		return 7;
	}
	HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	hr = ctx->Wait(fence, value);
	ctx->End(q);
	ctx->Flush();
	const HRESULT arm = fence->SetEventOnCompletion(value, ev);
	say("WAITING wait_hr=0x%08lx arm_hr=0x%08lx", (unsigned long)hr, (unsigned long)arm);

	const ULONGLONG t0 = GetTickCount64();
	int last_q = -1, last_ev = -1;
	unsigned long long last_c = ~0ull;
	long last_rm = 1;
	for (;;) {
		BOOL done = FALSE;
		const HRESULT gd = ctx->GetData(q, &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);
		const int qs = gd == S_OK && done ? 1 : 0;
		const int es = WaitForSingleObject(ev, 0) == WAIT_OBJECT_0 ? 1 : 0;
		const unsigned long long c = fence->GetCompletedValue();
		const long rm = (long)dev->GetDeviceRemovedReason();
		const unsigned long long t = GetTickCount64() - t0;
		if (qs != last_q || es != last_ev || c != last_c || rm != last_rm) {
			say("STATE t=%llu query=%d cpu_event=%d completed=%llu removed=0x%08lx", t, qs, es, c,
			    (unsigned long)rm);
			last_q = qs, last_ev = es, last_c = c, last_rm = rm;
		}
		if (t >= ms) {
			say("DONE t=%llu query=%d cpu_event=%d completed=%llu removed=0x%08lx", t, qs, es, c,
			    (unsigned long)rm);
			break;
		}
		Sleep(10);
	}
	ReleaseJob job = {fence, q, ctx, dev};
	const ULONGLONG r0 = GetTickCount64();
	HANDLE th = CreateThread(nullptr, 0, release_thread, &job, 0, nullptr);
	if (th != nullptr && WaitForSingleObject(th, 5000) == WAIT_OBJECT_0) {
		say("RELEASE ok %llu", (unsigned long long)(GetTickCount64() - r0));
	} else {
		say("RELEASE hung");
	}
	CloseHandle(ev);
	// Never return through the CRT if the release is stuck: exit now.
	TerminateProcess(GetCurrentProcess(), 0);
	return 0;
}

} // namespace

int
main(int argc, char **argv)
{
	if (argc < 3) {
		return 1;
	}
	const uint64_t luid = strtoull(argv[2], nullptr, 16);
	if (strcmp(argv[1], "signaller") == 0) {
		return signaller(luid);
	}
	if (strcmp(argv[1], "waiter") == 0 && argc >= 4) {
		return waiter(luid, (uint32_t)strtoul(argv[3], nullptr, 10));
	}
	return 1;
}
