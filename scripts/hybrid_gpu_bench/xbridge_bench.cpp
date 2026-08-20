// xbridge_bench.cpp - cross-adapter NT-shared texture transfer + fence benchmark.
// Prototype of an "atlas bridge": producer dGPU -> consumer iGPU.
// displayxr-runtime issue #918 hybrid-GPU investigation tooling.
//
// Usage:
//   xbridge_bench.exe --w=3840 --h=1080 --owner=igpu|dgpu --mode=full|copyonly
//                     --paced=0|1 --seconds=<n> [--fallback=dgpu|igpu|none]
//
// Producer runs on the dGPU (most dedicated VRAM), consumer on the iGPU (least).
// The shared texture is created on the --owner device and opened on the other.
// If a cross-adapter open fails, the exact HRESULT is reported and the opposite
// direction is tried automatically; if BOTH fail, the tool falls back to a
// same-adapter pair (clearly labelled) so a baseline is still produced.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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
	case E_ACCESSDENIED: s += " (E_ACCESSDENIED)"; break;
	case E_FAIL: s += " (E_FAIL)"; break;
	case E_HANDLE: s += " (E_HANDLE)"; break;
	case DXGI_ERROR_UNSUPPORTED: s += " (DXGI_ERROR_UNSUPPORTED)"; break;
	case DXGI_ERROR_INVALID_CALL: s += " (DXGI_ERROR_INVALID_CALL)"; break;
	case DXGI_ERROR_DEVICE_REMOVED: s += " (DXGI_ERROR_DEVICE_REMOVED)"; break;
	case DXGI_ERROR_NOT_FOUND: s += " (DXGI_ERROR_NOT_FOUND)"; break;
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
QpcMs()
{
	static LARGE_INTEGER freq = [] {
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		return f;
	}();
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static const DWORD kWatchdogMs = 2000;

static void
WatchdogFail(const char *stage)
{
	printf("[watchdog] wait exceeded 2s at stage %s\n", stage);
	fflush(stdout);
	exit(2);
}

// ------------------------------------------------------------------- stats

struct Stat
{
	std::vector<double> v;
	void add(double x) { v.push_back(x); }
	void clear() { v.clear(); }
	double mean() const
	{
		if (v.empty())
			return 0.0;
		double s = 0;
		for (double x : v)
			s += x;
		return s / v.size();
	}
	double p95() const
	{
		if (v.empty())
			return 0.0;
		std::vector<double> t = v;
		std::sort(t.begin(), t.end());
		size_t i = (size_t)std::floor(0.95 * (t.size() - 1) + 0.5);
		return t[std::min(i, t.size() - 1)];
	}
	double maxv() const
	{
		double m = 0;
		for (double x : v)
			m = std::max(m, x);
		return m;
	}
};

// ------------------------------------------------------------- adapters/dev

struct AdapterInfo
{
	IDXGIAdapter1 *adapter = nullptr;
	std::string name;
	LUID luid{};
	SIZE_T dedicated = 0;
	UINT index = 0;
};

struct GpuDev
{
	std::string label;   // "dgpu" / "igpu"
	std::string name;
	LUID luid{};
	ID3D11Device *dev = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	ID3D11Device1 *dev1 = nullptr;
	ID3D11Device5 *dev5 = nullptr;
	ID3D11DeviceContext4 *ctx4 = nullptr;
};

static std::string
LuidStr(const LUID &l)
{
	char b[64];
	snprintf(b, sizeof(b), "%08lX:%08lX", (unsigned long)l.HighPart, (unsigned long)l.LowPart);
	return b;
}

static bool
CreateGpuDev(IDXGIAdapter1 *ad, const AdapterInfo &ai, const char *label, GpuDev &out)
{
	const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	D3D_FEATURE_LEVEL got{};
	HRESULT hr = D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, want, _countof(want),
	                               D3D11_SDK_VERSION, &out.dev, &got, &out.ctx);
	if (FAILED(hr)) {
		printf("[xbridge] D3D11CreateDevice(%s) failed %s\n", label, HrString(hr).c_str());
		return false;
	}
	out.label = label;
	out.name = ai.name;
	out.luid = ai.luid;
	out.dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&out.dev1);
	out.dev->QueryInterface(__uuidof(ID3D11Device5), (void **)&out.dev5);
	out.ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&out.ctx4);
	if (!out.dev1) {
		printf("[xbridge] %s: ID3D11Device1 unavailable (need D3D11.1)\n", label);
		return false;
	}
	if (!out.dev5 || !out.ctx4)
		printf("[xbridge] %s: ID3D11Device5/Context4 unavailable -> no fence support\n", label);
	return true;
}

// ------------------------------------------------------------------ shaders

static const char *kVS = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint vid : SV_VertexID)
{
    VSOut o;
    float2 p = float2((vid << 1) & 2, vid & 2);
    o.uv = p;
    o.pos = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
)";

// Producer: frame-varying pattern, cheap.
static const char *kPatternPS = R"(
cbuffer CB : register(b0) { float gFrame; float3 gPad; };
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float t = gFrame * 0.017;
    float3 c = float3(frac(uv.x + t), frac(uv.y - t), frac(uv.x * uv.y + t));
    return float4(c, 1.0);
}
)";

// Consumer: sample the shared texture into a local RT.
static const char *kSamplePS = R"(
Texture2D    gTex : register(t0);
SamplerState gSmp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return gTex.Sample(gSmp, uv);
}
)";

struct PatCB
{
	float frame;
	float pad[3];
};

static bool
CompileShader(const char *src, const char *target, ID3DBlob **blob)
{
	ID3DBlob *err = nullptr;
	HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "main", target,
	                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &err);
	if (FAILED(hr)) {
		printf("[xbridge] D3DCompile(%s) failed %s: %s\n", target, HrString(hr).c_str(),
		       err ? (const char *)err->GetBufferPointer() : "");
		SafeRelease(err);
		return false;
	}
	SafeRelease(err);
	return true;
}

// ------------------------------------------------------- shared texture pair

struct SharedPair
{
	ID3D11Texture2D *ownerTex = nullptr;
	ID3D11Texture2D *otherTex = nullptr;
	IDXGIKeyedMutex *kmOwner = nullptr;
	IDXGIKeyedMutex *kmOther = nullptr;
	HANDLE handle = nullptr;
};

static bool
TryShare(GpuDev &owner, GpuDev &other, UINT w, UINT h, SharedPair &out, std::string &errOut)
{
	D3D11_TEXTURE2D_DESC td{};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	// In D3D11, SHARED_NTHANDLE requires SHARED_KEYEDMUTEX.
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

	HRESULT hr = owner.dev->CreateTexture2D(&td, nullptr, &out.ownerTex);
	if (FAILED(hr)) {
		errOut = "CreateTexture2D(shared) on " + owner.label + " failed " + HrString(hr);
		return false;
	}

	IDXGIResource1 *res1 = nullptr;
	hr = out.ownerTex->QueryInterface(__uuidof(IDXGIResource1), (void **)&res1);
	if (FAILED(hr)) {
		errOut = "QI IDXGIResource1 failed " + HrString(hr);
		return false;
	}
	hr = res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
	                              nullptr, &out.handle);
	SafeRelease(res1);
	if (FAILED(hr)) {
		errOut = "IDXGIResource1::CreateSharedHandle failed " + HrString(hr);
		return false;
	}

	hr = other.dev1->OpenSharedResource1(out.handle, __uuidof(ID3D11Texture2D),
	                                     (void **)&out.otherTex);
	if (FAILED(hr)) {
		errOut = "OpenSharedResource1 on " + other.label + " failed " + HrString(hr);
		return false;
	}

	hr = out.ownerTex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&out.kmOwner);
	if (SUCCEEDED(hr))
		hr = out.otherTex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&out.kmOther);
	if (FAILED(hr)) {
		errOut = "QI IDXGIKeyedMutex failed " + HrString(hr);
		return false;
	}
	return true;
}

// -------------------------------------------------- cross-adapter cap probe
//
// The headline question for #918 is simply "can a D3D11 texture cross the
// iGPU/dGPU boundary at all, and by which mechanism?".  Probe every flavour on
// the REAL adapter pair and print the exact HRESULT for each - before any
// same-adapter fallback muddies the picture.

static void
ProbeTexShare(GpuDev &a, GpuDev &b, UINT miscFlags, const char *label)
{
	const UINT w = 256, h = 256;
	D3D11_TEXTURE2D_DESC td{};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	td.MiscFlags = miscFlags;

	ID3D11Texture2D *tex = nullptr;
	HRESULT hr = a.dev->CreateTexture2D(&td, nullptr, &tex);
	if (FAILED(hr)) {
		printf("[probe] %-34s %s -> %s : CreateTexture2D failed %s\n", label, a.label.c_str(),
		       b.label.c_str(), HrString(hr).c_str());
		return;
	}

	HANDLE handle = nullptr;
	ID3D11Texture2D *opened = nullptr;
	const char *stage = "";
	if (miscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) {
		IDXGIResource1 *r1 = nullptr;
		hr = tex->QueryInterface(__uuidof(IDXGIResource1), (void **)&r1);
		stage = "QI IDXGIResource1";
		if (SUCCEEDED(hr)) {
			hr = r1->CreateSharedHandle(
			    nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
			    &handle);
			stage = "CreateSharedHandle";
			SafeRelease(r1);
		}
		if (SUCCEEDED(hr)) {
			hr = b.dev1->OpenSharedResource1(handle, __uuidof(ID3D11Texture2D),
			                                 (void **)&opened);
			stage = "OpenSharedResource1";
		}
	} else {
		IDXGIResource *r0 = nullptr;
		hr = tex->QueryInterface(__uuidof(IDXGIResource), (void **)&r0);
		stage = "QI IDXGIResource";
		if (SUCCEEDED(hr)) {
			hr = r0->GetSharedHandle(&handle);
			stage = "GetSharedHandle";
			SafeRelease(r0);
		}
		if (SUCCEEDED(hr)) {
			hr = b.dev->OpenSharedResource(handle, __uuidof(ID3D11Texture2D), (void **)&opened);
			stage = "OpenSharedResource";
		}
	}

	printf("[probe] %-34s %s -> %s : %s%s%s\n", label, a.label.c_str(), b.label.c_str(),
	       SUCCEEDED(hr) ? "OK" : "FAIL at ", SUCCEEDED(hr) ? "" : stage,
	       SUCCEEDED(hr) ? "" : (" " + HrString(hr)).c_str());

	SafeRelease(opened);
	if (handle && (miscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE))
		CloseHandle(handle); // legacy handles are not owned by the caller
	SafeRelease(tex);
}

static void
ProbeFenceShare(GpuDev &a, GpuDev &b)
{
	if (!a.dev5 || !b.dev5) {
		printf("[probe] %-34s %s -> %s : ID3D11Device5 unavailable\n", "fence SHARED|CROSS_ADAPTER",
		       a.label.c_str(), b.label.c_str());
		return;
	}
	ID3D11Fence *f = nullptr;
	HRESULT hr = a.dev5->CreateFence(
	    0, (D3D11_FENCE_FLAG)(D3D11_FENCE_FLAG_SHARED | D3D11_FENCE_FLAG_SHARED_CROSS_ADAPTER),
	    __uuidof(ID3D11Fence), (void **)&f);
	const char *stage = "CreateFence";
	HANDLE h = nullptr;
	ID3D11Fence *of = nullptr;
	if (SUCCEEDED(hr)) {
		hr = f->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h);
		stage = "Fence::CreateSharedHandle";
	}
	if (SUCCEEDED(hr)) {
		hr = b.dev5->OpenSharedFence(h, __uuidof(ID3D11Fence), (void **)&of);
		stage = "OpenSharedFence";
	}
	printf("[probe] %-34s %s -> %s : %s%s%s\n", "fence SHARED|CROSS_ADAPTER", a.label.c_str(),
	       b.label.c_str(), SUCCEEDED(hr) ? "OK" : "FAIL at ", SUCCEEDED(hr) ? "" : stage,
	       SUCCEEDED(hr) ? "" : (" " + HrString(hr)).c_str());
	SafeRelease(of);
	if (h)
		CloseHandle(h);
	SafeRelease(f);
}

static void
RunCapabilityProbe(GpuDev &dgpu, GpuDev &igpu)
{
	printf("[probe] --- cross-adapter capability matrix (real dGPU/iGPU pair) ---\n");
	const UINT kNtKm =
	    D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
	ProbeTexShare(dgpu, igpu, kNtKm, "tex NTHANDLE|KEYEDMUTEX");
	ProbeTexShare(igpu, dgpu, kNtKm, "tex NTHANDLE|KEYEDMUTEX");
	ProbeTexShare(dgpu, igpu, D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX, "tex KEYEDMUTEX (legacy hnd)");
	ProbeTexShare(igpu, dgpu, D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX, "tex KEYEDMUTEX (legacy hnd)");
	ProbeTexShare(dgpu, igpu, D3D11_RESOURCE_MISC_SHARED, "tex SHARED (legacy hnd)");
	ProbeTexShare(igpu, dgpu, D3D11_RESOURCE_MISC_SHARED, "tex SHARED (legacy hnd)");
	ProbeFenceShare(dgpu, igpu);
	ProbeFenceShare(igpu, dgpu);
	printf("[probe] --- end capability matrix ---\n");
	fflush(stdout);
}

static void
FreeShared(SharedPair &p)
{
	SafeRelease(p.kmOwner);
	SafeRelease(p.kmOther);
	SafeRelease(p.otherTex);
	SafeRelease(p.ownerTex);
	if (p.handle) {
		CloseHandle(p.handle);
		p.handle = nullptr;
	}
	p = SharedPair{};
}

// --------------------------------------------------------------------- main

int
main(int argc, char **argv)
{
	UINT W = 3840, H = 1080;
	std::string ownerSel = "igpu";
	std::string mode = "full";
	int paced = 1;
	double seconds = 10.0;
	std::string fallbackSel = "dgpu";

	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		auto val = [&](const char *k) -> const char * {
			size_t n = strlen(k);
			return (a.compare(0, n, k) == 0) ? a.c_str() + n : nullptr;
		};
		if (const char *v = val("--w="))
			W = (UINT)atoi(v);
		else if (const char *v2 = val("--h="))
			H = (UINT)atoi(v2);
		else if (const char *v3 = val("--owner="))
			ownerSel = v3;
		else if (const char *v4 = val("--mode="))
			mode = v4;
		else if (const char *v5 = val("--paced="))
			paced = atoi(v5);
		else if (const char *v6 = val("--seconds="))
			seconds = atof(v6);
		else if (const char *v7 = val("--fallback="))
			fallbackSel = v7;
		else if (a == "--help" || a == "-h") {
			printf("xbridge_bench --w=W --h=H --owner=igpu|dgpu --mode=full|copyonly "
			       "--paced=0|1 --seconds=<n> [--fallback=dgpu|igpu|none]\n");
			return 0;
		} else {
			printf("[xbridge] unknown arg '%s'\n", a.c_str());
			return 1;
		}
	}
	if (W == 0 || H == 0) {
		printf("[xbridge] bad dimensions\n");
		return 1;
	}
	bool fullMode = (mode == "full");

	// --- adapters ---------------------------------------------------------
	IDXGIFactory1 *factory = nullptr;
	HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory);
	if (FAILED(hr)) {
		printf("[xbridge] CreateDXGIFactory1 failed %s\n", HrString(hr).c_str());
		return 1;
	}
	std::vector<AdapterInfo> ads;
	{
		IDXGIAdapter1 *a = nullptr;
		for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
			DXGI_ADAPTER_DESC1 d{};
			a->GetDesc1(&d);
			std::string nm = WideToUtf8(d.Description);
			if ((d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ||
			    nm.find("Basic Render") != std::string::npos ||
			    nm.find("WARP") != std::string::npos) {
				a->Release();
				a = nullptr;
				continue;
			}
			AdapterInfo ai;
			ai.adapter = a;
			ai.name = nm;
			ai.luid = d.AdapterLuid;
			ai.dedicated = d.DedicatedVideoMemory;
			ai.index = i;
			ads.push_back(ai);
			a = nullptr;
		}
	}
	if (ads.size() < 1) {
		printf("[xbridge] no hardware adapters\n");
		return 1;
	}
	int iIdx = 0, dIdx = 0;
	for (size_t i = 0; i < ads.size(); ++i) {
		if (ads[i].dedicated < ads[iIdx].dedicated)
			iIdx = (int)i;
		if (ads[i].dedicated > ads[dIdx].dedicated)
			dIdx = (int)i;
	}
	bool singleAdapter = (iIdx == dIdx);

	printf("[xbridge] adapters:\n");
	for (auto &a : ads)
		printf("[xbridge]   [%u] \"%s\" LUID=%s dedicatedVRAM=%llu MB%s%s\n", a.index,
		       a.name.c_str(), LuidStr(a.luid).c_str(),
		       (unsigned long long)(a.dedicated / (1024ull * 1024ull)),
		       ((int)(&a - &ads[0]) == dIdx) ? "  <- dgpu(producer)" : "",
		       ((int)(&a - &ads[0]) == iIdx && iIdx != dIdx) ? "  <- igpu(consumer)" : "");
	if (singleAdapter)
		printf("[xbridge] NOTE: only one hardware adapter present - cross-adapter cannot be "
		       "tested here.\n");
	fflush(stdout);

	GpuDev dgpu, igpu;
	if (!CreateGpuDev(ads[dIdx].adapter, ads[dIdx], "dgpu", dgpu))
		return 1;
	if (!singleAdapter) {
		if (!CreateGpuDev(ads[iIdx].adapter, ads[iIdx], "igpu", igpu))
			return 1;
	}

	if (!singleAdapter)
		RunCapabilityProbe(dgpu, igpu);

	GpuDev *producer = &dgpu;
	GpuDev *consumer = singleAdapter ? nullptr : &igpu;

	// --- shared texture: try requested owner, then the other direction -----
	SharedPair sp;
	std::string err1, err2, directionDesc;
	bool ownerIsProducer = false;
	bool crossAdapterWorked = false;
	GpuDev fallbackDev; // used only if cross-adapter fails entirely

	auto attempt = [&](GpuDev *own, GpuDev *oth, std::string &errOut) -> bool {
		FreeShared(sp);
		bool ok = TryShare(*own, *oth, W, H, sp, errOut);
		if (!ok)
			FreeShared(sp);
		return ok;
	};

	if (consumer) {
		GpuDev *firstOwner = (ownerSel == "dgpu") ? producer : consumer;
		GpuDev *firstOther = (firstOwner == producer) ? consumer : producer;
		if (attempt(firstOwner, firstOther, err1)) {
			crossAdapterWorked = true;
			ownerIsProducer = (firstOwner == producer);
			directionDesc = "cross-adapter owner=" + firstOwner->label + " (" +
			                firstOwner->name + ") -> opened on " + firstOther->label;
		} else {
			printf("[xbridge] cross-adapter attempt owner=%s FAILED: %s\n",
			       firstOwner->label.c_str(), err1.c_str());
			fflush(stdout);
			if (attempt(firstOther, firstOwner, err2)) {
				crossAdapterWorked = true;
				ownerIsProducer = (firstOther == producer);
				directionDesc = "cross-adapter owner=" + firstOther->label + " (" +
				                firstOther->name + ") -> opened on " + firstOwner->label +
				                " [AUTO-FLIPPED from requested owner=" + ownerSel + "]";
			} else {
				printf("[xbridge] cross-adapter attempt owner=%s FAILED: %s\n",
				       firstOther->label.c_str(), err2.c_str());
				fflush(stdout);
			}
		}
	}

	if (!crossAdapterWorked) {
		printf("[xbridge] FINDING: NT-shared keyed-mutex texture could NOT be opened across "
		       "adapters in either direction.\n");
		if (fallbackSel == "none") {
			printf("[xbridge] --fallback=none -> exiting.\n");
			return 3;
		}
		int fi = (fallbackSel == "igpu") ? iIdx : dIdx;
		if (!CreateGpuDev(ads[fi].adapter, ads[fi], "fallback", fallbackDev))
			return 1;
		producer = (fallbackSel == "igpu" && !singleAdapter) ? &igpu : &dgpu;
		if (singleAdapter)
			producer = &dgpu;
		consumer = &fallbackDev;
		std::string e3;
		if (!attempt(producer, consumer, e3)) {
			printf("[xbridge] same-adapter fallback ALSO failed: %s\n", e3.c_str());
			return 3;
		}
		ownerIsProducer = true;
		directionDesc = "SAME-ADAPTER FALLBACK on " + ads[fi].name +
		                " (two devices, one adapter) - cross-adapter unsupported here";
	}

	ID3D11Texture2D *prodShared = ownerIsProducer ? sp.ownerTex : sp.otherTex;
	ID3D11Texture2D *consShared = ownerIsProducer ? sp.otherTex : sp.ownerTex;
	IDXGIKeyedMutex *kmProd = ownerIsProducer ? sp.kmOwner : sp.kmOther;
	IDXGIKeyedMutex *kmCons = ownerIsProducer ? sp.kmOther : sp.kmOwner;

	printf("[xbridge] producer=%s \"%s\" LUID=%s\n", producer->label.c_str(),
	       producer->name.c_str(), LuidStr(producer->luid).c_str());
	printf("[xbridge] consumer=%s \"%s\" LUID=%s\n", consumer->label.c_str(),
	       consumer->name.c_str(), LuidStr(consumer->luid).c_str());
	printf("[xbridge] share direction: %s\n", directionDesc.c_str());
	fflush(stdout);

	// --- fences -----------------------------------------------------------
	ID3D11Fence *fenceProd = nullptr, *fenceCons = nullptr, *fenceProdOnCons = nullptr;
	bool sharedFenceOk = false;
	std::string fenceNote;
	if (producer->dev5 && consumer->dev5 && producer->ctx4 && consumer->ctx4) {
		hr = producer->dev5->CreateFence(0,
		                                 (D3D11_FENCE_FLAG)(D3D11_FENCE_FLAG_SHARED |
		                                                    D3D11_FENCE_FLAG_SHARED_CROSS_ADAPTER),
		                                 __uuidof(ID3D11Fence), (void **)&fenceProd);
		if (SUCCEEDED(hr)) {
			HANDLE fh = nullptr;
			hr = fenceProd->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fh);
			if (SUCCEEDED(hr)) {
				hr = consumer->dev5->OpenSharedFence(fh, __uuidof(ID3D11Fence),
				                                     (void **)&fenceProdOnCons);
				if (SUCCEEDED(hr)) {
					sharedFenceOk = true;
					fenceNote = "cross-adapter SHARED fence OK";
				} else {
					fenceNote = "OpenSharedFence on consumer failed " + HrString(hr);
				}
				CloseHandle(fh);
			} else {
				fenceNote = "ID3D11Fence::CreateSharedHandle failed " + HrString(hr);
			}
		} else {
			fenceNote = "CreateFence(SHARED|SHARED_CROSS_ADAPTER) failed " + HrString(hr);
		}
		if (!fenceProd) {
			hr = producer->dev5->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence),
			                                 (void **)&fenceProd);
			if (FAILED(hr))
				fenceNote += "; local producer CreateFence failed " + HrString(hr);
		}
		if (consumer->dev5) {
			hr = consumer->dev5->CreateFence(0, D3D11_FENCE_FLAG_NONE, __uuidof(ID3D11Fence),
			                                 (void **)&fenceCons);
			if (FAILED(hr))
				fenceNote += "; local consumer CreateFence failed " + HrString(hr);
		}
	} else {
		fenceNote = "ID3D11Device5/Context4 unavailable";
	}
	bool useFences = (fenceProd && fenceCons && producer->ctx4 && consumer->ctx4);
	printf("[xbridge] fences (%s -> %s): %s -> timing via %s\n", producer->label.c_str(),
	       consumer->label.c_str(), fenceNote.c_str(),
	       useFences ? "fence SetEventOnCompletion" : "DEGRADED keyed-mutex-only + QPC (no fences)");
	fflush(stdout);

	// --- per-device resources --------------------------------------------
	ID3DBlob *vsb = nullptr, *patb = nullptr, *smpb = nullptr;
	if (!CompileShader(kVS, "vs_5_0", &vsb) || !CompileShader(kPatternPS, "ps_5_0", &patb))
		return 1;
	if (fullMode && !CompileShader(kSamplePS, "ps_5_0", &smpb))
		return 1;

	// producer: local texture + pattern pass
	ID3D11Texture2D *prodLocal = nullptr;
	ID3D11RenderTargetView *prodRtv = nullptr;
	ID3D11VertexShader *prodVs = nullptr;
	ID3D11PixelShader *prodPs = nullptr;
	ID3D11Buffer *prodCb = nullptr;
	{
		D3D11_TEXTURE2D_DESC td{};
		td.Width = W;
		td.Height = H;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		hr = producer->dev->CreateTexture2D(&td, nullptr, &prodLocal);
		if (FAILED(hr)) {
			printf("[xbridge] producer local texture failed %s\n", HrString(hr).c_str());
			return 1;
		}
		producer->dev->CreateRenderTargetView(prodLocal, nullptr, &prodRtv);
		producer->dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr,
		                                  &prodVs);
		producer->dev->CreatePixelShader(patb->GetBufferPointer(), patb->GetBufferSize(), nullptr,
		                                 &prodPs);
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = sizeof(PatCB);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		producer->dev->CreateBuffer(&bd, nullptr, &prodCb);
		if (!prodRtv || !prodVs || !prodPs || !prodCb) {
			printf("[xbridge] producer resource creation failed\n");
			return 1;
		}
	}

	// consumer: SRV over shared + local RT + sampling pass
	ID3D11ShaderResourceView *consSrv = nullptr;
	ID3D11Texture2D *consLocal = nullptr;
	ID3D11RenderTargetView *consRtv = nullptr;
	ID3D11VertexShader *consVs = nullptr;
	ID3D11PixelShader *consPs = nullptr;
	ID3D11SamplerState *consSmp = nullptr;
	if (fullMode) {
		hr = consumer->dev->CreateShaderResourceView(consShared, nullptr, &consSrv);
		if (FAILED(hr)) {
			printf("[xbridge] consumer SRV over shared texture failed %s\n",
			       HrString(hr).c_str());
			return 1;
		}
		D3D11_TEXTURE2D_DESC td{};
		td.Width = W;
		td.Height = H;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET;
		hr = consumer->dev->CreateTexture2D(&td, nullptr, &consLocal);
		if (FAILED(hr)) {
			printf("[xbridge] consumer local RT failed %s\n", HrString(hr).c_str());
			return 1;
		}
		consumer->dev->CreateRenderTargetView(consLocal, nullptr, &consRtv);
		consumer->dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr,
		                                  &consVs);
		consumer->dev->CreatePixelShader(smpb->GetBufferPointer(), smpb->GetBufferSize(), nullptr,
		                                 &consPs);
		D3D11_SAMPLER_DESC sd{};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		consumer->dev->CreateSamplerState(&sd, &consSmp);
		if (!consRtv || !consVs || !consPs || !consSmp) {
			printf("[xbridge] consumer resource creation failed\n");
			return 1;
		}
	}

	D3D11_VIEWPORT vp{};
	vp.Width = (float)W;
	vp.Height = (float)H;
	vp.MaxDepth = 1.0f;

	HANDLE evProd = CreateEventA(nullptr, FALSE, FALSE, nullptr);
	HANDLE evCons = CreateEventA(nullptr, FALSE, FALSE, nullptr);

	const UINT64 kKeyProd = 0; // producer acquires 0, releases 1
	const UINT64 kKeyCons = 1; // consumer acquires 1, releases 0

	Stat wCopy, wCons, wRt;  // per-second window
	Stat aCopy, aCons, aRt;  // all-run
	unsigned long long frames = 0, windowFrames = 0;

	const double kTickMs = 1000.0 / 60.0;
	double t0 = QpcMs();
	double nextTick = t0;
	double lastReport = t0;

	printf("[xbridge] config: %ux%u RGBA8 mode=%s paced=%d seconds=%.1f\n", W, H, mode.c_str(),
	       paced, seconds);
	fflush(stdout);

	while (true) {
		double now = QpcMs();
		if ((now - t0) >= seconds * 1000.0)
			break;

		double tSubmit = QpcMs();

		// ---- producer -----------------------------------------------------
		PatCB pcb{(float)frames, {0, 0, 0}};
		producer->ctx->UpdateSubresource(prodCb, 0, nullptr, &pcb, 0, 0);
		producer->ctx->OMSetRenderTargets(1, &prodRtv, nullptr);
		producer->ctx->RSSetViewports(1, &vp);
		producer->ctx->IASetInputLayout(nullptr);
		producer->ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		producer->ctx->VSSetShader(prodVs, nullptr, 0);
		producer->ctx->PSSetShader(prodPs, nullptr, 0);
		producer->ctx->PSSetConstantBuffers(0, 1, &prodCb);
		producer->ctx->Draw(3, 0);

		hr = kmProd->AcquireSync(kKeyProd, kWatchdogMs);
		if (hr == (HRESULT)WAIT_TIMEOUT)
			WatchdogFail("producer.AcquireSync");
		if (FAILED(hr)) {
			printf("[xbridge] producer AcquireSync failed %s\n", HrString(hr).c_str());
			return 2;
		}
		producer->ctx->CopyResource(prodShared, prodLocal);
		hr = kmProd->ReleaseSync(kKeyCons);
		if (FAILED(hr)) {
			printf("[xbridge] producer ReleaseSync failed %s\n", HrString(hr).c_str());
			return 2;
		}

		UINT64 fv = frames + 1;
		if (useFences) {
			producer->ctx4->Signal(fenceProd, fv);
			producer->ctx->Flush();
			if (fenceProd->GetCompletedValue() < fv) {
				if (FAILED(fenceProd->SetEventOnCompletion(fv, evProd))) {
					printf("[xbridge] SetEventOnCompletion(prod) failed\n");
					return 2;
				}
				DWORD wr = WaitForSingleObject(evProd, kWatchdogMs);
				if (wr == WAIT_TIMEOUT)
					WatchdogFail("producer.fence");
			}
		} else {
			producer->ctx->Flush();
		}
		double tProdDone = QpcMs();

		// ---- consumer -----------------------------------------------------
		double tConsStart = QpcMs();
		hr = kmCons->AcquireSync(kKeyCons, kWatchdogMs);
		if (hr == (HRESULT)WAIT_TIMEOUT)
			WatchdogFail("consumer.AcquireSync");
		if (FAILED(hr)) {
			printf("[xbridge] consumer AcquireSync failed %s\n", HrString(hr).c_str());
			return 2;
		}
		if (fullMode) {
			consumer->ctx->OMSetRenderTargets(1, &consRtv, nullptr);
			consumer->ctx->RSSetViewports(1, &vp);
			consumer->ctx->IASetInputLayout(nullptr);
			consumer->ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			consumer->ctx->VSSetShader(consVs, nullptr, 0);
			consumer->ctx->PSSetShader(consPs, nullptr, 0);
			consumer->ctx->PSSetShaderResources(0, 1, &consSrv);
			consumer->ctx->PSSetSamplers(0, 1, &consSmp);
			consumer->ctx->Draw(3, 0);
			ID3D11ShaderResourceView *nullSrv = nullptr;
			consumer->ctx->PSSetShaderResources(0, 1, &nullSrv);
		}
		hr = kmCons->ReleaseSync(kKeyProd);
		if (FAILED(hr)) {
			printf("[xbridge] consumer ReleaseSync failed %s\n", HrString(hr).c_str());
			return 2;
		}
		if (useFences) {
			consumer->ctx4->Signal(fenceCons, fv);
			consumer->ctx->Flush();
			if (fenceCons->GetCompletedValue() < fv) {
				if (FAILED(fenceCons->SetEventOnCompletion(fv, evCons))) {
					printf("[xbridge] SetEventOnCompletion(cons) failed\n");
					return 2;
				}
				DWORD wr = WaitForSingleObject(evCons, kWatchdogMs);
				if (wr == WAIT_TIMEOUT)
					WatchdogFail("consumer.fence");
			}
		} else {
			consumer->ctx->Flush();
		}
		double tConsDone = QpcMs();

		double copySignal = tProdDone - tSubmit;
		double consDone = tConsDone - tConsStart;
		double roundtrip = tConsDone - tSubmit;
		wCopy.add(copySignal);
		wCons.add(consDone);
		wRt.add(roundtrip);
		aCopy.add(copySignal);
		aCons.add(consDone);
		aRt.add(roundtrip);
		frames++;
		windowFrames++;

		now = QpcMs();
		if (now - lastReport >= 1000.0) {
			double fps = windowFrames * 1000.0 / (now - lastReport);
			printf("[xbridge] fps=%.1f copy_signal_ms=%.3f/%.3f consumer_done_ms=%.3f/%.3f "
			       "roundtrip_ms=%.3f/%.3f\n",
			       fps, wCopy.mean(), wCopy.p95(), wCons.mean(), wCons.p95(), wRt.mean(),
			       wRt.p95());
			fflush(stdout);
			wCopy.clear();
			wCons.clear();
			wRt.clear();
			windowFrames = 0;
			lastReport = now;
		}

		if (paced) {
			nextTick += kTickMs;
			double sleepMs = nextTick - QpcMs();
			if (sleepMs > 0.0) {
				if (sleepMs > 500.0)
					nextTick = QpcMs() + kTickMs;
				else
					Sleep((DWORD)sleepMs);
			} else if (sleepMs < -100.0) {
				nextTick = QpcMs();
			}
		}
	}

	double elapsed = (QpcMs() - t0) / 1000.0;
	double fps = frames / std::max(1e-6, elapsed);
	double bytes = (double)W * (double)H * 4.0;
	double gbps = fps * bytes / 1e9;

	printf("\n========== xbridge_bench summary ==========\n");
	printf("  config          : %ux%u RGBA8 (%.2f MB/frame) mode=%s paced=%d seconds=%.1f\n", W, H,
	       bytes / (1024.0 * 1024.0), mode.c_str(), paced, seconds);
	printf("  producer        : %s \"%s\" LUID=%s\n", producer->label.c_str(),
	       producer->name.c_str(), LuidStr(producer->luid).c_str());
	printf("  consumer        : %s \"%s\" LUID=%s\n", consumer->label.c_str(),
	       consumer->name.c_str(), LuidStr(consumer->luid).c_str());
	printf("  share direction : %s\n", directionDesc.c_str());
	printf("  cross-adapter   : %s\n", crossAdapterWorked ? "WORKED" : "FAILED (see HRESULTs above)");
	if (!err1.empty())
		printf("  attempt#1 err   : %s\n", err1.c_str());
	if (!err2.empty())
		printf("  attempt#2 err   : %s\n", err2.c_str());
	printf("  fences          : %s pair=%s->%s (%s)\n",
	       sharedFenceOk ? "shared fence OK" : "DEGRADED", producer->label.c_str(),
	       consumer->label.c_str(), fenceNote.c_str());
	printf("  NOTE            : see the [probe] capability matrix above for the real\n");
	printf("                    dGPU<->iGPU share/fence results, independent of any fallback.\n");
	printf("  frames          : %llu in %.3f s -> fps=%.2f\n", frames, elapsed, fps);
	printf("  copy_signal_ms  : mean=%.3f p95=%.3f max=%.3f\n", aCopy.mean(), aCopy.p95(),
	       aCopy.maxv());
	printf("  consumer_done_ms: mean=%.3f p95=%.3f max=%.3f\n", aCons.mean(), aCons.p95(),
	       aCons.maxv());
	printf("  roundtrip_ms    : mean=%.3f p95=%.3f max=%.3f\n", aRt.mean(), aRt.p95(), aRt.maxv());
	printf("  bandwidth_GBps  : %.3f   (meaningful in --mode=copyonly --paced=0)\n", gbps);
	printf("===========================================\n");
	fflush(stdout);

	// cleanup
	if (evProd)
		CloseHandle(evProd);
	if (evCons)
		CloseHandle(evCons);
	SafeRelease(fenceProdOnCons);
	SafeRelease(fenceCons);
	SafeRelease(fenceProd);
	SafeRelease(consSmp);
	SafeRelease(consPs);
	SafeRelease(consVs);
	SafeRelease(consRtv);
	SafeRelease(consLocal);
	SafeRelease(consSrv);
	SafeRelease(prodCb);
	SafeRelease(prodPs);
	SafeRelease(prodVs);
	SafeRelease(prodRtv);
	SafeRelease(prodLocal);
	SafeRelease(vsb);
	SafeRelease(patb);
	SafeRelease(smpb);
	FreeShared(sp);
	for (GpuDev *g : {&dgpu, &igpu, &fallbackDev}) {
		if (g->ctx)
			g->ctx->ClearState();
		SafeRelease(g->ctx4);
		SafeRelease(g->dev5);
		SafeRelease(g->dev1);
		SafeRelease(g->ctx);
		SafeRelease(g->dev);
	}
	for (auto &a : ads)
		SafeRelease(a.adapter);
	SafeRelease(factory);
	return 0;
}
