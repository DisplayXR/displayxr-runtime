// gpu_loadgen.cpp - synthetic controllable GPU load on a chosen adapter.
// displayxr-runtime issue #918 hybrid-GPU investigation tooling.
//
// Build: see build.bat  (cl /std:c++17 ... d3d11.lib dxgi.lib d3dcompiler.lib)
//
// Usage:
//   gpu_loadgen.exe --adapter=igpu|dgpu|<index> --duty=<pct> --seconds=<n> --res=WxH
//
// igpu = hardware adapter with the LEAST dedicated VRAM, dgpu = the MOST.
// Microsoft Basic Render Driver / WARP / software adapters are skipped.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

// ---------------------------------------------------------------- utilities

template <class T>
static void SafeRelease(T *&p)
{
	if (p) {
		p->Release();
		p = nullptr;
	}
}

static std::string
HrString(HRESULT hr)
{
	char buf[64];
	snprintf(buf, sizeof(buf), "0x%08lX", (unsigned long)hr);
	std::string s = buf;
	switch (hr) {
	case E_INVALIDARG: s += " (E_INVALIDARG)"; break;
	case E_OUTOFMEMORY: s += " (E_OUTOFMEMORY)"; break;
	case E_NOTIMPL: s += " (E_NOTIMPL)"; break;
	case E_FAIL: s += " (E_FAIL)"; break;
	case DXGI_ERROR_UNSUPPORTED: s += " (DXGI_ERROR_UNSUPPORTED)"; break;
	case DXGI_ERROR_INVALID_CALL: s += " (DXGI_ERROR_INVALID_CALL)"; break;
	case DXGI_ERROR_DEVICE_REMOVED: s += " (DXGI_ERROR_DEVICE_REMOVED)"; break;
	default: break;
	}
	return s;
}

static std::string
WideToUtf8(const wchar_t *w)
{
	if (!w)
		return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s(n > 0 ? n - 1 : 0, '\0');
	if (n > 1)
		WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
	return s;
}

static double
QpcSeconds()
{
	static LARGE_INTEGER freq = [] {
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		return f;
	}();
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart / (double)freq.QuadPart;
}

// ------------------------------------------------------------ adapter picks

struct AdapterInfo
{
	IDXGIAdapter1 *adapter = nullptr;
	std::string name;
	LUID luid{};
	SIZE_T dedicated = 0;
	UINT index = 0;
};

static bool
EnumHardwareAdapters(std::vector<AdapterInfo> &out, IDXGIFactory1 **factoryOut)
{
	IDXGIFactory1 *factory = nullptr;
	HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory);
	if (FAILED(hr)) {
		fprintf(stderr, "CreateDXGIFactory1 failed %s\n", HrString(hr).c_str());
		return false;
	}
	*factoryOut = factory;

	IDXGIAdapter1 *a = nullptr;
	for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
		DXGI_ADAPTER_DESC1 d{};
		a->GetDesc1(&d);
		std::string nm = WideToUtf8(d.Description);
		bool software = (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 ||
		                nm.find("Basic Render") != std::string::npos ||
		                nm.find("WARP") != std::string::npos;
		if (software) {
			a->Release();
			a = nullptr;
			continue;
		}
		AdapterInfo info;
		info.adapter = a;
		info.name = nm;
		info.luid = d.AdapterLuid;
		info.dedicated = d.DedicatedVideoMemory;
		info.index = i;
		out.push_back(info);
		a = nullptr;
	}
	return !out.empty();
}

static int
PickAdapter(const std::vector<AdapterInfo> &ads, const std::string &sel)
{
	if (ads.empty())
		return -1;
	if (sel == "igpu") {
		int best = 0;
		for (size_t i = 1; i < ads.size(); ++i)
			if (ads[i].dedicated < ads[best].dedicated)
				best = (int)i;
		return best;
	}
	if (sel == "dgpu") {
		int best = 0;
		for (size_t i = 1; i < ads.size(); ++i)
			if (ads[i].dedicated > ads[best].dedicated)
				best = (int)i;
		return best;
	}
	// numeric: matches the DXGI enumeration index
	int want = atoi(sel.c_str());
	for (size_t i = 0; i < ads.size(); ++i)
		if ((int)ads[i].index == want)
			return (int)i;
	return -1;
}

// ------------------------------------------------------------------ shaders

static const char *kVS = R"(
struct VSOut { float4 pos : SV_Position; };
VSOut main(uint vid : SV_VertexID)
{
    VSOut o;
    float2 p = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
)";

static const char *kPS = R"(
cbuffer CB : register(b0)
{
    uint  gIters;
    uint  gFrame;
    uint2 gPad;
};

float4 main(float4 pos : SV_Position) : SV_Target
{
    float2 uv = pos.xy * 0.0005;
    float acc = uv.x + uv.y + (float)(gFrame & 255) * 0.001;
    [loop]
    for (uint i = 0; i < gIters; ++i) {
        acc = acc * 1.00001 + sin(acc) * 0.5 + cos(acc * 1.3) * 0.25;
        acc = frac(acc * 1.618033) + 0.001;
    }
    return float4(acc, acc * 0.5, acc * 0.25, 1.0);
}
)";

struct CB
{
	UINT iters;
	UINT frame;
	UINT pad0;
	UINT pad1;
};

// -------------------------------------------------------------------- state

static std::atomic<bool> g_quit{false};

static BOOL WINAPI
CtrlHandler(DWORD)
{
	g_quit.store(true);
	return TRUE;
}

static void
StdinQuitThread()
{
	int c;
	while ((c = getchar()) != EOF) {
		if (c == 'q' || c == 'Q') {
			g_quit.store(true);
			return;
		}
	}
}

// --------------------------------------------------------------------- main

int
main(int argc, char **argv)
{
	std::string selAdapter = "igpu";
	double duty = 25.0;
	double seconds = -1.0;
	UINT resW = 3840, resH = 2160;

	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		auto val = [&](const char *k) -> const char * {
			size_t n = strlen(k);
			return (a.compare(0, n, k) == 0) ? a.c_str() + n : nullptr;
		};
		if (const char *v = val("--adapter="))
			selAdapter = v;
		else if (const char *v2 = val("--duty="))
			duty = atof(v2);
		else if (const char *v3 = val("--seconds="))
			seconds = atof(v3);
		else if (const char *v4 = val("--res=")) {
			int w = 0, h = 0;
			if (sscanf(v4, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
				resW = (UINT)w;
				resH = (UINT)h;
			}
		} else if (a == "--help" || a == "-h") {
			printf("gpu_loadgen --adapter=igpu|dgpu|<index> --duty=<pct> "
			       "--seconds=<n> --res=WxH\n");
			return 0;
		} else {
			fprintf(stderr, "[loadgen] unknown arg '%s'\n", a.c_str());
			return 1;
		}
	}
	duty = std::max(0.5, std::min(95.0, duty));

	SetConsoleCtrlHandler(CtrlHandler, TRUE);

	IDXGIFactory1 *factory = nullptr;
	std::vector<AdapterInfo> ads;
	if (!EnumHardwareAdapters(ads, &factory)) {
		fprintf(stderr, "[loadgen] no hardware adapters found\n");
		return 1;
	}
	int idx = PickAdapter(ads, selAdapter);
	if (idx < 0) {
		fprintf(stderr, "[loadgen] adapter selector '%s' matched nothing\n", selAdapter.c_str());
		return 1;
	}
	AdapterInfo &ai = ads[idx];

	printf("[loadgen] adapters seen:\n");
	for (auto &a : ads)
		printf("[loadgen]   [%u] \"%s\" LUID=%08lX:%08lX dedicatedVRAM=%llu MB\n", a.index,
		       a.name.c_str(), (unsigned long)a.luid.HighPart, (unsigned long)a.luid.LowPart,
		       (unsigned long long)(a.dedicated / (1024ull * 1024ull)));
	printf("[loadgen] chosen adapter=\"%s\" LUID=%08lX:%08lX dedicatedVRAM=%llu MB res=%ux%u "
	       "duty_target=%.1f%%\n",
	       ai.name.c_str(), (unsigned long)ai.luid.HighPart, (unsigned long)ai.luid.LowPart,
	       (unsigned long long)(ai.dedicated / (1024ull * 1024ull)), resW, resH, duty);
	fflush(stdout);

	ID3D11Device *dev = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	D3D_FEATURE_LEVEL fl{};
	const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	HRESULT hr = D3D11CreateDevice(ai.adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, want,
	                               _countof(want), D3D11_SDK_VERSION, &dev, &fl, &ctx);
	if (FAILED(hr)) {
		fprintf(stderr, "[loadgen] D3D11CreateDevice failed %s\n", HrString(hr).c_str());
		return 1;
	}

	// Offscreen RGBA8 render target.
	D3D11_TEXTURE2D_DESC td{};
	td.Width = resW;
	td.Height = resH;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	ID3D11Texture2D *rt = nullptr;
	hr = dev->CreateTexture2D(&td, nullptr, &rt);
	if (FAILED(hr)) {
		fprintf(stderr, "[loadgen] CreateTexture2D failed %s\n", HrString(hr).c_str());
		return 1;
	}
	ID3D11RenderTargetView *rtv = nullptr;
	hr = dev->CreateRenderTargetView(rt, nullptr, &rtv);
	if (FAILED(hr)) {
		fprintf(stderr, "[loadgen] CreateRenderTargetView failed %s\n", HrString(hr).c_str());
		return 1;
	}

	// Runtime shader compile.
	auto compile = [&](const char *src, const char *target, ID3DBlob **blob) -> bool {
		ID3DBlob *err = nullptr;
		HRESULT c = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "main", target,
		                       D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &err);
		if (FAILED(c)) {
			fprintf(stderr, "[loadgen] D3DCompile(%s) failed %s: %s\n", target,
			        HrString(c).c_str(), err ? (const char *)err->GetBufferPointer() : "");
			SafeRelease(err);
			return false;
		}
		SafeRelease(err);
		return true;
	};

	ID3DBlob *vsb = nullptr, *psb = nullptr;
	if (!compile(kVS, "vs_5_0", &vsb) || !compile(kPS, "ps_5_0", &psb))
		return 1;

	ID3D11VertexShader *vs = nullptr;
	ID3D11PixelShader *ps = nullptr;
	dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
	dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
	if (!vs || !ps) {
		fprintf(stderr, "[loadgen] shader object creation failed\n");
		return 1;
	}

	D3D11_BUFFER_DESC bd{};
	bd.ByteWidth = sizeof(CB);
	bd.Usage = D3D11_USAGE_DEFAULT;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	ID3D11Buffer *cb = nullptr;
	hr = dev->CreateBuffer(&bd, nullptr, &cb);
	if (FAILED(hr)) {
		fprintf(stderr, "[loadgen] CreateBuffer failed %s\n", HrString(hr).c_str());
		return 1;
	}

	// Timestamp query ring (read N-3 frames later; never stall).
	const int kRing = 4;
	struct QSlot
	{
		ID3D11Query *disjoint = nullptr;
		ID3D11Query *tsBegin = nullptr;
		ID3D11Query *tsEnd = nullptr;
		bool pending = false;
	} slots[kRing];
	D3D11_QUERY_DESC qd{};
	for (int i = 0; i < kRing; ++i) {
		qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
		dev->CreateQuery(&qd, &slots[i].disjoint);
		qd.Query = D3D11_QUERY_TIMESTAMP;
		dev->CreateQuery(&qd, &slots[i].tsBegin);
		dev->CreateQuery(&qd, &slots[i].tsEnd);
		if (!slots[i].disjoint || !slots[i].tsBegin || !slots[i].tsEnd) {
			fprintf(stderr, "[loadgen] CreateQuery failed\n");
			return 1;
		}
	}

	D3D11_VIEWPORT vp{};
	vp.Width = (float)resW;
	vp.Height = (float)resH;
	vp.MaxDepth = 1.0f;

	std::thread(StdinQuitThread).detach();

	const double kTick = 1.0 / 60.0; // 16.667 ms
	const double targetMs = duty / 100.0 * kTick * 1000.0;

	UINT iters = 8;
	const UINT kMinIters = 1, kMaxIters = 400000;
	const unsigned long long kWarmupFrames = 12;

	double t0 = QpcSeconds();
	double nextTick = t0;
	double lastReport = t0;

	unsigned long long frame = 0;
	double sumMs = 0.0;
	int sumN = 0;
	double lastMs = 0.0;
	double emaMs = 0.0;
	int ctlCount = 0;

	while (!g_quit.load()) {
		double now = QpcSeconds();
		if (seconds > 0.0 && (now - t0) >= seconds)
			break;

		int s = (int)(frame % kRing);

		// Retire the oldest in-flight slot (N-3) before reusing it.
		if (slots[s].pending) {
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
			UINT64 tb = 0, te = 0;
			HRESULT r1, r2, r3;
			// The slot is 3 frames old; these should be ready without stalling,
			// but loop defensively with DONOTFLUSH.
			int guard = 0;
			do {
				r1 = ctx->GetData(slots[s].disjoint, &dj, sizeof(dj),
				                  D3D11_ASYNC_GETDATA_DONOTFLUSH);
				r2 = ctx->GetData(slots[s].tsBegin, &tb, sizeof(tb),
				                  D3D11_ASYNC_GETDATA_DONOTFLUSH);
				r3 = ctx->GetData(slots[s].tsEnd, &te, sizeof(te),
				                  D3D11_ASYNC_GETDATA_DONOTFLUSH);
				if (r1 == S_OK && r2 == S_OK && r3 == S_OK)
					break;
				if (++guard > 2000)
					break;
				Sleep(0);
			} while (true);

			if (r1 == S_OK && r2 == S_OK && r3 == S_OK && !dj.Disjoint && dj.Frequency) {
				double ms = (double)(te - tb) * 1000.0 / (double)dj.Frequency;
				lastMs = ms;
				// Skip the first frames: shader/PSO warmup outliers otherwise
				// dominate the reported mean.
				if (frame >= kWarmupFrames) {
					sumMs += ms;
					sumN++;
				}
				// Smooth the measurement: on a shared iGPU that is also
				// scanning out the panel, per-frame GPU time is bimodal and a
				// raw sample would make the controller oscillate.
				emaMs = (emaMs <= 0.0) ? ms : (0.85 * emaMs + 0.15 * ms);

				// Proportional controller on the shader iteration count.
				if (++ctlCount >= 4) {
					ctlCount = 0;
					if (emaMs > 0.02) {
						double ratio = targetMs / emaMs;
						ratio = std::max(0.4, std::min(4.0, ratio));
						double ni = (double)iters * ratio;
						// Damp: move 60% of the way.
						ni = (double)iters + 0.6 * (ni - (double)iters);
						iters = (UINT)std::max((double)kMinIters,
						                       std::min((double)kMaxIters, ni + 0.5));
					} else {
						iters = std::min(kMaxIters, iters * 2);
					}
				}
			}
			slots[s].pending = false;
		}

		// Render one load pass under timestamps.
		CB c{iters, (UINT)frame, 0, 0};
		ctx->UpdateSubresource(cb, 0, nullptr, &c, 0, 0);

		ctx->Begin(slots[s].disjoint);
		ctx->End(slots[s].tsBegin);

		ctx->OMSetRenderTargets(1, &rtv, nullptr);
		ctx->RSSetViewports(1, &vp);
		ctx->IASetInputLayout(nullptr);
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ctx->VSSetShader(vs, nullptr, 0);
		ctx->PSSetShader(ps, nullptr, 0);
		ctx->PSSetConstantBuffers(0, 1, &cb);
		ctx->Draw(3, 0);

		ctx->End(slots[s].tsEnd);
		ctx->End(slots[s].disjoint);
		slots[s].pending = true;
		ctx->Flush();

		frame++;

		// Report once a second.
		now = QpcSeconds();
		if (now - lastReport >= 1.0) {
			double mean = sumN ? sumMs / sumN : lastMs;
			printf("[loadgen] adapter=\"%s\" gpu_ms=%.3f duty=%.1f%% iters=%u\n",
			       ai.name.c_str(), mean, mean / (kTick * 1000.0) * 100.0, iters);
			fflush(stdout);
			sumMs = 0.0;
			sumN = 0;
			lastReport = now;
		}

		// Sleep to the next 60 Hz tick.
		nextTick += kTick;
		double sleepS = nextTick - QpcSeconds();
		if (sleepS > 0.0) {
			if (sleepS > 0.5)
				nextTick = QpcSeconds() + kTick; // resync after a hiccup
			else
				Sleep((DWORD)(sleepS * 1000.0));
		} else if (sleepS < -0.1) {
			nextTick = QpcSeconds();
		}
	}

	// Final line so a short run still prints something useful.
	{
		double mean = sumN ? sumMs / sumN : lastMs;
		printf("[loadgen] adapter=\"%s\" gpu_ms=%.3f duty=%.1f%% iters=%u (final, frames=%llu)\n",
		       ai.name.c_str(), mean, mean / (kTick * 1000.0) * 100.0, iters, frame);
		fflush(stdout);
	}

	ctx->ClearState();
	ctx->Flush();
	for (int i = 0; i < kRing; ++i) {
		SafeRelease(slots[i].disjoint);
		SafeRelease(slots[i].tsBegin);
		SafeRelease(slots[i].tsEnd);
	}
	SafeRelease(cb);
	SafeRelease(vs);
	SafeRelease(ps);
	SafeRelease(vsb);
	SafeRelease(psb);
	SafeRelease(rtv);
	SafeRelease(rt);
	SafeRelease(ctx);
	SafeRelease(dev);
	for (auto &a : ads)
		SafeRelease(a.adapter);
	SafeRelease(factory);
	return 0;
}
