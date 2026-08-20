// xbridge12_bench.cpp - D3D12 CROSS-ADAPTER heap transfer + fence benchmark.
// Producer: D3D12 device on the dGPU.  Consumer: D3D12 device on the iGPU.
// displayxr-runtime issue #918 hybrid-GPU investigation tooling.
//
// This is the counterpart to xbridge_bench.exe (D3D11), which proved that D3D11
// cross-adapter texture sharing is simply unavailable (E_INVALIDARG on every
// open, both directions).  D3D12 cross-adapter heaps are the documented
// mechanism that IS supposed to work; this tool measures it.
//
// Usage:
//   xbridge12_bench.exe --w=3840 --h=1080 --mode=full|copyonly|sample
//                       --paced=0|1 --seconds=<n> [--reverse=1] [--debug=1]
//
//   mode=copyonly : consumer CopyTextureRegion(cross-adapter -> local)   [bandwidth]
//   mode=full     : the above, plus a full-screen pass sampling the local copy
//   mode=sample   : consumer samples the CROSS-ADAPTER resource DIRECTLY in a
//                   full-screen pass (no pull copy) - what a real weave wants
//   --reverse=1   : swap roles (producer=iGPU, consumer=dGPU)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
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
	case S_OK: s += " (S_OK)"; break;
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

static std::string
LuidStr(const LUID &l)
{
	char b[64];
	snprintf(b, sizeof(b), "%08lX:%08lX", (unsigned long)l.HighPart, (unsigned long)l.LowPart);
	return b;
}

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

// Producer pattern. Deterministic in uv so a CPU-side pixel check can verify
// the bytes really crossed the adapter boundary intact.
static const char *kPatternPS = R"(
cbuffer CB : register(b0) { float gFrame; float3 gPad; };
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    float t = frac(gFrame * 0.017);
    return float4(uv.x, uv.y, t, 1.0);
}
)";

static const char *kSamplePS = R"(
Texture2D    gTex : register(t0);
SamplerState gSmp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
    return gTex.Sample(gSmp, uv);
}
)";

static bool
CompileShader(const char *src, const char *target, ID3DBlob **blob)
{
	ID3DBlob *err = nullptr;
	HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "main", target,
	                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob, &err);
	if (FAILED(hr)) {
		printf("[xb12] D3DCompile(%s) failed %s: %s\n", target, HrString(hr).c_str(),
		       err ? (const char *)err->GetBufferPointer() : "");
		SafeRelease(err);
		return false;
	}
	SafeRelease(err);
	return true;
}

// ----------------------------------------------------------- d3d12 defaults

static D3D12_RASTERIZER_DESC
DefaultRaster()
{
	D3D12_RASTERIZER_DESC r{};
	r.FillMode = D3D12_FILL_MODE_SOLID;
	r.CullMode = D3D12_CULL_MODE_NONE;
	r.DepthClipEnable = TRUE;
	r.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
	return r;
}

static D3D12_BLEND_DESC
DefaultBlend()
{
	D3D12_BLEND_DESC b{};
	b.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
	b.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
	b.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	b.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	b.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
	b.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	b.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
	b.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	return b;
}

static D3D12_RESOURCE_BARRIER
Transition(ID3D12Resource *res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
	D3D12_RESOURCE_BARRIER b{};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = res;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = before;
	b.Transition.StateAfter = after;
	return b;
}

// ------------------------------------------------------------------ gpu ctx

struct Gpu12
{
	std::string label;
	std::string name;
	LUID luid{};
	IDXGIAdapter1 *adapter = nullptr;
	ID3D12Device *dev = nullptr;
	ID3D12CommandQueue *queue = nullptr;
	ID3D12CommandAllocator *alloc = nullptr;
	ID3D12GraphicsCommandList *list = nullptr;
	ID3D12Fence *fence = nullptr; // local timing fence
	HANDLE evt = nullptr;
	bool crossAdapterRowMajor = false;
};

static bool
InitGpu12(Gpu12 &g, IDXGIAdapter1 *ad, const std::string &name, LUID luid, const char *label)
{
	g.adapter = ad;
	g.name = name;
	g.luid = luid;
	g.label = label;
	HRESULT hr = D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
	                               (void **)&g.dev);
	printf("[xb12] D3D12CreateDevice(%s \"%s\") -> %s\n", label, name.c_str(),
	       HrString(hr).c_str());
	if (FAILED(hr))
		return false;

	D3D12_FEATURE_DATA_D3D12_OPTIONS opt{};
	if (SUCCEEDED(g.dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opt, sizeof(opt))))
		g.crossAdapterRowMajor = opt.CrossAdapterRowMajorTextureSupported ? true : false;
	printf("[xb12]   %s CrossAdapterRowMajorTextureSupported = %s\n", label,
	       g.crossAdapterRowMajor ? "TRUE" : "FALSE");

	D3D12_COMMAND_QUEUE_DESC qd{};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	hr = g.dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void **)&g.queue);
	if (FAILED(hr)) {
		printf("[xb12] CreateCommandQueue(%s) failed %s\n", label, HrString(hr).c_str());
		return false;
	}
	hr = g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
	                                   __uuidof(ID3D12CommandAllocator), (void **)&g.alloc);
	if (FAILED(hr)) {
		printf("[xb12] CreateCommandAllocator(%s) failed %s\n", label, HrString(hr).c_str());
		return false;
	}
	hr = g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc, nullptr,
	                              __uuidof(ID3D12GraphicsCommandList), (void **)&g.list);
	if (FAILED(hr)) {
		printf("[xb12] CreateCommandList(%s) failed %s\n", label, HrString(hr).c_str());
		return false;
	}
	g.list->Close();
	hr = g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), (void **)&g.fence);
	if (FAILED(hr)) {
		printf("[xb12] CreateFence(%s) failed %s\n", label, HrString(hr).c_str());
		return false;
	}
	g.evt = CreateEventA(nullptr, FALSE, FALSE, nullptr);
	return true;
}

static void
WaitFence(Gpu12 &g, UINT64 value, const char *stage)
{
	if (g.fence->GetCompletedValue() >= value)
		return;
	if (FAILED(g.fence->SetEventOnCompletion(value, g.evt))) {
		printf("[xb12] SetEventOnCompletion failed at %s\n", stage);
		exit(2);
	}
	if (WaitForSingleObject(g.evt, kWatchdogMs) == WAIT_TIMEOUT)
		WatchdogFail(stage);
}

// --------------------------------------------------------------------- main

int
main(int argc, char **argv)
{
	UINT W = 3840, H = 1080;
	std::string mode = "full";
	int paced = 1;
	double seconds = 10.0;
	int reverse = 0;
	int debugLayer = 0;

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
		else if (const char *v3 = val("--mode="))
			mode = v3;
		else if (const char *v4 = val("--paced="))
			paced = atoi(v4);
		else if (const char *v5 = val("--seconds="))
			seconds = atof(v5);
		else if (const char *v6 = val("--reverse="))
			reverse = atoi(v6);
		else if (const char *v7 = val("--debug="))
			debugLayer = atoi(v7);
		else if (a == "--help" || a == "-h") {
			printf("xbridge12_bench --w=W --h=H --mode=full|copyonly|sample --paced=0|1 "
			       "--seconds=<n> [--reverse=1] [--debug=1]\n");
			return 0;
		} else {
			printf("[xb12] unknown arg '%s'\n", a.c_str());
			return 1;
		}
	}
	const bool modeCopyOnly = (mode == "copyonly");
	const bool modeSample = (mode == "sample");
	const bool modeFull = (mode == "full");
	if (!modeCopyOnly && !modeSample && !modeFull) {
		printf("[xb12] bad --mode=%s\n", mode.c_str());
		return 1;
	}

	if (debugLayer) {
		ID3D12Debug *dbg = nullptr;
		if (SUCCEEDED(D3D12GetDebugInterface(__uuidof(ID3D12Debug), (void **)&dbg))) {
			dbg->EnableDebugLayer();
			printf("[xb12] D3D12 debug layer ENABLED\n");
			SafeRelease(dbg);
		} else {
			printf("[xb12] D3D12 debug layer unavailable\n");
		}
	}

	// --- adapters ---------------------------------------------------------
	IDXGIFactory1 *factory = nullptr;
	HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory);
	if (FAILED(hr)) {
		printf("[xb12] CreateDXGIFactory1 failed %s\n", HrString(hr).c_str());
		return 1;
	}
	struct AInfo
	{
		IDXGIAdapter1 *ad;
		std::string name;
		LUID luid;
		SIZE_T vram;
		UINT index;
	};
	std::vector<AInfo> ads;
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
			ads.push_back({a, nm, d.AdapterLuid, d.DedicatedVideoMemory, i});
			a = nullptr;
		}
	}
	if (ads.size() < 2) {
		printf("[xb12] need two hardware adapters, found %zu\n", ads.size());
		return 1;
	}
	int iIdx = 0, dIdx = 0;
	for (size_t i = 0; i < ads.size(); ++i) {
		if (ads[i].vram < ads[iIdx].vram)
			iIdx = (int)i;
		if (ads[i].vram > ads[dIdx].vram)
			dIdx = (int)i;
	}
	printf("[xb12] adapters:\n");
	for (auto &a : ads)
		printf("[xb12]   [%u] \"%s\" LUID=%s dedicatedVRAM=%llu MB\n", a.index, a.name.c_str(),
		       LuidStr(a.luid).c_str(), (unsigned long long)(a.vram / (1024ull * 1024ull)));

	int prodIdx = reverse ? iIdx : dIdx;
	int consIdx = reverse ? dIdx : iIdx;

	Gpu12 prod, cons;
	if (!InitGpu12(prod, ads[prodIdx].ad, ads[prodIdx].name, ads[prodIdx].luid,
	               reverse ? "igpu(producer)" : "dgpu(producer)"))
		return 1;
	if (!InitGpu12(cons, ads[consIdx].ad, ads[consIdx].name, ads[consIdx].luid,
	               reverse ? "dgpu(consumer)" : "igpu(consumer)"))
		return 1;
	fflush(stdout);

	// --- cross-adapter resource description -------------------------------
	D3D12_RESOURCE_DESC texDesc{};
	texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	texDesc.Alignment = 0;
	texDesc.Width = W;
	texDesc.Height = H;
	texDesc.DepthOrArraySize = 1;
	texDesc.MipLevels = 1;
	texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	texDesc.SampleDesc.Count = 1;
	texDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; // required for cross-adapter
	texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
	UINT numRows = 0;
	UINT64 rowBytes = 0, totalBytes = 0;
	{
		D3D12_RESOURCE_DESC probe = texDesc;
		probe.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		prod.dev->GetCopyableFootprints(&probe, 0, 1, 0, &fp, &numRows, &rowBytes, &totalBytes);
	}
	const UINT64 kHeapAlign = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT; // 64 KB
	UINT64 heapSize = (totalBytes + kHeapAlign - 1) & ~(kHeapAlign - 1);
	printf("[xb12] cross-adapter footprint: rowPitch=%u rows=%u rowBytes=%llu total=%llu "
	       "heapSize=%llu\n",
	       fp.Footprint.RowPitch, numRows, (unsigned long long)rowBytes,
	       (unsigned long long)totalBytes, (unsigned long long)heapSize);

	// --- shared cross-adapter heap ----------------------------------------
	D3D12_HEAP_DESC hd{};
	hd.SizeInBytes = heapSize;
	hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
	hd.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
	hd.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
	hd.Alignment = kHeapAlign;
	hd.Flags = (D3D12_HEAP_FLAGS)(D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER |
	                              D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES);

	ID3D12Heap *heapProd = nullptr, *heapCons = nullptr;
	hr = prod.dev->CreateHeap(&hd, __uuidof(ID3D12Heap), (void **)&heapProd);
	printf("[xb12] CAP CreateHeap(SHARED|SHARED_CROSS_ADAPTER) on producer -> %s\n",
	       HrString(hr).c_str());
	if (FAILED(hr))
		return 3;

	HANDLE heapHandle = nullptr;
	hr = prod.dev->CreateSharedHandle(heapProd, nullptr, GENERIC_ALL, nullptr, &heapHandle);
	printf("[xb12] CAP CreateSharedHandle(heap) -> %s\n", HrString(hr).c_str());
	if (FAILED(hr))
		return 3;

	hr = cons.dev->OpenSharedHandle(heapHandle, __uuidof(ID3D12Heap), (void **)&heapCons);
	printf("[xb12] CAP OpenSharedHandle(heap) on consumer -> %s\n", HrString(hr).c_str());
	if (FAILED(hr)) {
		printf("[xb12] FINDING: the cross-adapter HEAP could not be opened on the consumer.\n");
		return 3;
	}

	// --- placed resource: texture first, buffer fallback -------------------
	ID3D12Resource *xaProd = nullptr, *xaCons = nullptr;
	bool usingBuffer = false;
	std::string placedNote;

	hr = prod.dev->CreatePlacedResource(heapProd, 0, &texDesc, D3D12_RESOURCE_STATE_COMMON,
	                                    nullptr, __uuidof(ID3D12Resource), (void **)&xaProd);
	printf("[xb12] CAP CreatePlacedResource(TEXTURE2D ROW_MAJOR|ALLOW_CROSS_ADAPTER) producer "
	       "-> %s\n",
	       HrString(hr).c_str());
	if (SUCCEEDED(hr)) {
		hr = cons.dev->CreatePlacedResource(heapCons, 0, &texDesc, D3D12_RESOURCE_STATE_COMMON,
		                                    nullptr, __uuidof(ID3D12Resource), (void **)&xaCons);
		printf("[xb12] CAP CreatePlacedResource(TEXTURE2D) consumer -> %s\n",
		       HrString(hr).c_str());
	}
	if (FAILED(hr)) {
		SafeRelease(xaProd);
		SafeRelease(xaCons);
		usingBuffer = true;
		placedNote = "placed TEXTURE2D failed " + HrString(hr) + " -> fell back to BUFFER";
		printf("[xb12] falling back to a cross-adapter BUFFER of %llu bytes\n",
		       (unsigned long long)totalBytes);
		D3D12_RESOURCE_DESC bufDesc{};
		bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		bufDesc.Width = totalBytes;
		bufDesc.Height = 1;
		bufDesc.DepthOrArraySize = 1;
		bufDesc.MipLevels = 1;
		bufDesc.Format = DXGI_FORMAT_UNKNOWN;
		bufDesc.SampleDesc.Count = 1;
		bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		bufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
		hr = prod.dev->CreatePlacedResource(heapProd, 0, &bufDesc, D3D12_RESOURCE_STATE_COMMON,
		                                    nullptr, __uuidof(ID3D12Resource), (void **)&xaProd);
		printf("[xb12] CAP CreatePlacedResource(BUFFER) producer -> %s\n", HrString(hr).c_str());
		if (SUCCEEDED(hr))
			hr = cons.dev->CreatePlacedResource(heapCons, 0, &bufDesc,
			                                    D3D12_RESOURCE_STATE_COMMON, nullptr,
			                                    __uuidof(ID3D12Resource), (void **)&xaCons);
		printf("[xb12] CAP CreatePlacedResource(BUFFER) consumer -> %s\n", HrString(hr).c_str());
		if (FAILED(hr)) {
			printf("[xb12] FINDING: neither a placed texture nor a placed buffer could be "
			       "created in the cross-adapter heap.\n");
			return 3;
		}
	} else {
		placedNote = "placed TEXTURE2D ROW_MAJOR|ALLOW_CROSS_ADAPTER OK on both sides";
	}

	const bool directSamplePossible = !usingBuffer;
	if (modeSample && !directSamplePossible)
		printf("[xb12] --mode=sample requires the placed-texture route (buffer fallback in "
		       "effect) -> degrading to a pull-copy run\n");
	const bool doDirectSample = modeSample && directSamplePossible;
	const bool doPullCopy = !doDirectSample; // copyonly + full pull; sample does not
	const bool doLocalSamplePass = modeFull || doDirectSample;

	// --- shared cross-adapter fence ---------------------------------------
	ID3D12Fence *fenceShared = nullptr, *fenceSharedOnCons = nullptr;
	hr = prod.dev->CreateFence(0,
	                           (D3D12_FENCE_FLAGS)(D3D12_FENCE_FLAG_SHARED |
	                                               D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER),
	                           __uuidof(ID3D12Fence), (void **)&fenceShared);
	printf("[xb12] CAP CreateFence(SHARED|SHARED_CROSS_ADAPTER) producer -> %s\n",
	       HrString(hr).c_str());
	if (FAILED(hr))
		return 3;
	HANDLE fenceHandle = nullptr;
	hr = prod.dev->CreateSharedHandle(fenceShared, nullptr, GENERIC_ALL, nullptr, &fenceHandle);
	printf("[xb12] CAP CreateSharedHandle(fence) -> %s\n", HrString(hr).c_str());
	if (SUCCEEDED(hr)) {
		hr = cons.dev->OpenSharedHandle(fenceHandle, __uuidof(ID3D12Fence),
		                                (void **)&fenceSharedOnCons);
		printf("[xb12] CAP OpenSharedHandle(fence) consumer -> %s\n", HrString(hr).c_str());
	}
	if (FAILED(hr))
		return 3;
	fflush(stdout);

	// --- producer local RT + pattern PSO ----------------------------------
	ID3DBlob *vsb = nullptr, *patb = nullptr, *smpb = nullptr;
	if (!CompileShader(kVS, "vs_5_0", &vsb) || !CompileShader(kPatternPS, "ps_5_0", &patb))
		return 1;
	if (doLocalSamplePass && !CompileShader(kSamplePS, "ps_5_0", &smpb))
		return 1;

	D3D12_HEAP_PROPERTIES defHeap{};
	defHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

	D3D12_RESOURCE_DESC localDesc{};
	localDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	localDesc.Width = W;
	localDesc.Height = H;
	localDesc.DepthOrArraySize = 1;
	localDesc.MipLevels = 1;
	localDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	localDesc.SampleDesc.Count = 1;
	localDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	localDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

	ID3D12Resource *prodLocal = nullptr;
	hr = prod.dev->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &localDesc,
	                                       D3D12_RESOURCE_STATE_COMMON, nullptr,
	                                       __uuidof(ID3D12Resource), (void **)&prodLocal);
	if (FAILED(hr)) {
		printf("[xb12] producer local texture failed %s\n", HrString(hr).c_str());
		return 1;
	}

	// Consumer local: pull target (copy dest) and/or RT for the sample pass.
	D3D12_RESOURCE_DESC consPullDesc = localDesc;
	consPullDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
	ID3D12Resource *consPull = nullptr; // cross-adapter -> here
	ID3D12Resource *consRt = nullptr;   // sample pass output
	if (doPullCopy) {
		hr = cons.dev->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &consPullDesc,
		                                       D3D12_RESOURCE_STATE_COMMON, nullptr,
		                                       __uuidof(ID3D12Resource), (void **)&consPull);
		if (FAILED(hr)) {
			printf("[xb12] consumer pull texture failed %s\n", HrString(hr).c_str());
			return 1;
		}
	}
	if (doLocalSamplePass) {
		hr = cons.dev->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &localDesc,
		                                       D3D12_RESOURCE_STATE_COMMON, nullptr,
		                                       __uuidof(ID3D12Resource), (void **)&consRt);
		if (FAILED(hr)) {
			printf("[xb12] consumer RT failed %s\n", HrString(hr).c_str());
			return 1;
		}
	}

	// Descriptor heaps.
	auto makeDescHeap = [](ID3D12Device *d, D3D12_DESCRIPTOR_HEAP_TYPE t, UINT n, bool shaderVis,
	                       ID3D12DescriptorHeap **out) -> HRESULT {
		D3D12_DESCRIPTOR_HEAP_DESC dh{};
		dh.Type = t;
		dh.NumDescriptors = n;
		dh.Flags = shaderVis ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
		                     : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
		return d->CreateDescriptorHeap(&dh, __uuidof(ID3D12DescriptorHeap), (void **)out);
	};

	ID3D12DescriptorHeap *prodRtvHeap = nullptr, *consRtvHeap = nullptr, *consSrvHeap = nullptr;
	makeDescHeap(prod.dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false, &prodRtvHeap);
	D3D12_CPU_DESCRIPTOR_HANDLE prodRtv = prodRtvHeap->GetCPUDescriptorHandleForHeapStart();
	prod.dev->CreateRenderTargetView(prodLocal, nullptr, prodRtv);

	D3D12_CPU_DESCRIPTOR_HANDLE consRtv{};
	D3D12_GPU_DESCRIPTOR_HANDLE consSrvGpu{};
	if (doLocalSamplePass) {
		makeDescHeap(cons.dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, false, &consRtvHeap);
		consRtv = consRtvHeap->GetCPUDescriptorHandleForHeapStart();
		cons.dev->CreateRenderTargetView(consRt, nullptr, consRtv);

		makeDescHeap(cons.dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true, &consSrvHeap);
		D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
		sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		sv.Texture2D.MipLevels = 1;
		// Direct-sample: view the CROSS-ADAPTER resource itself.
		// mode=full: view the local pulled copy.
		ID3D12Resource *srvTarget = doDirectSample ? xaCons : consPull;
		cons.dev->CreateShaderResourceView(srvTarget, &sv,
		                                   consSrvHeap->GetCPUDescriptorHandleForHeapStart());
		consSrvGpu = consSrvHeap->GetGPUDescriptorHandleForHeapStart();
	}

	// Root signatures + PSOs.
	auto serializeRs = [](const D3D12_ROOT_SIGNATURE_DESC &d, ID3D12Device *dev,
	                      ID3D12RootSignature **out) -> HRESULT {
		ID3DBlob *sig = nullptr, *err = nullptr;
		HRESULT h = D3D12SerializeRootSignature(&d, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
		if (FAILED(h)) {
			printf("[xb12] D3D12SerializeRootSignature failed %s: %s\n", HrString(h).c_str(),
			       err ? (const char *)err->GetBufferPointer() : "");
			SafeRelease(err);
			return h;
		}
		h = dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
		                             __uuidof(ID3D12RootSignature), (void **)out);
		SafeRelease(sig);
		SafeRelease(err);
		return h;
	};

	ID3D12RootSignature *prodRs = nullptr;
	{
		D3D12_ROOT_PARAMETER rp{};
		rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		rp.Constants.ShaderRegister = 0;
		rp.Constants.RegisterSpace = 0;
		rp.Constants.Num32BitValues = 4;
		rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		D3D12_ROOT_SIGNATURE_DESC rsd{};
		rsd.NumParameters = 1;
		rsd.pParameters = &rp;
		rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
		if (FAILED(serializeRs(rsd, prod.dev, &prodRs)))
			return 1;
	}
	ID3D12RootSignature *consRs = nullptr;
	if (doLocalSamplePass) {
		D3D12_DESCRIPTOR_RANGE range{};
		range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		range.NumDescriptors = 1;
		range.BaseShaderRegister = 0;
		range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
		D3D12_ROOT_PARAMETER rp{};
		rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		rp.DescriptorTable.NumDescriptorRanges = 1;
		rp.DescriptorTable.pDescriptorRanges = &range;
		rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		D3D12_STATIC_SAMPLER_DESC ss{};
		ss.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
		ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
		ss.MaxLOD = D3D12_FLOAT32_MAX;
		ss.ShaderRegister = 0;
		ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		D3D12_ROOT_SIGNATURE_DESC rsd{};
		rsd.NumParameters = 1;
		rsd.pParameters = &rp;
		rsd.NumStaticSamplers = 1;
		rsd.pStaticSamplers = &ss;
		rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
		if (FAILED(serializeRs(rsd, cons.dev, &consRs)))
			return 1;
	}

	auto makePso = [&](ID3D12Device *d, ID3D12RootSignature *rs, ID3DBlob *vs, ID3DBlob *ps,
	                   ID3D12PipelineState **out) -> HRESULT {
		D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
		pd.pRootSignature = rs;
		pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
		pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
		pd.BlendState = DefaultBlend();
		pd.SampleMask = UINT_MAX;
		pd.RasterizerState = DefaultRaster();
		pd.DepthStencilState.DepthEnable = FALSE;
		pd.DepthStencilState.StencilEnable = FALSE;
		pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pd.NumRenderTargets = 1;
		pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
		pd.SampleDesc.Count = 1;
		return d->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), (void **)out);
	};

	ID3D12PipelineState *prodPso = nullptr, *consPso = nullptr;
	hr = makePso(prod.dev, prodRs, vsb, patb, &prodPso);
	if (FAILED(hr)) {
		printf("[xb12] producer PSO failed %s\n", HrString(hr).c_str());
		return 1;
	}
	if (doLocalSamplePass) {
		hr = makePso(cons.dev, consRs, vsb, smpb, &consPso);
		if (FAILED(hr)) {
			printf("[xb12] consumer PSO failed %s\n", HrString(hr).c_str());
			return 1;
		}
	}

	D3D12_VIEWPORT vp{0, 0, (float)W, (float)H, 0, 1};
	D3D12_RECT sc{0, 0, (LONG)W, (LONG)H};

	// Copy locations.
	D3D12_TEXTURE_COPY_LOCATION xaLocProd{}, xaLocCons{};
	if (usingBuffer) {
		xaLocProd.pResource = xaProd;
		xaLocProd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		xaLocProd.PlacedFootprint = fp;
		xaLocCons = xaLocProd;
		xaLocCons.pResource = xaCons;
	} else {
		xaLocProd.pResource = xaProd;
		xaLocProd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		xaLocProd.SubresourceIndex = 0;
		xaLocCons = xaLocProd;
		xaLocCons.pResource = xaCons;
	}
	D3D12_TEXTURE_COPY_LOCATION prodLocalLoc{};
	prodLocalLoc.pResource = prodLocal;
	prodLocalLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	prodLocalLoc.SubresourceIndex = 0;
	D3D12_TEXTURE_COPY_LOCATION consPullLoc{};
	if (doPullCopy) {
		consPullLoc.pResource = consPull;
		consPullLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		consPullLoc.SubresourceIndex = 0;
	}

	printf("[xb12] config: %ux%u RGBA8 mode=%s paced=%d seconds=%.1f transport=%s "
	       "direct_sample=%s\n",
	       W, H, mode.c_str(), paced, seconds, usingBuffer ? "BUFFER" : "PLACED_TEXTURE",
	       doDirectSample ? "YES" : "no");
	fflush(stdout);

	// --- frame loop -------------------------------------------------------
	Stat wCopy, wCons, wRt, aCopy, aCons, aRt;
	unsigned long long frames = 0, windowFrames = 0;
	const double kTickMs = 1000.0 / 60.0;
	double t0 = QpcMs();
	double nextTick = t0;
	double lastReport = t0;

	while (true) {
		double now = QpcMs();
		if ((now - t0) >= seconds * 1000.0)
			break;

		UINT64 fv = frames + 1;
		double tSubmit = QpcMs();

		// ---- producer ----
		prod.alloc->Reset();
		prod.list->Reset(prod.alloc, prodPso);
		{
			D3D12_RESOURCE_BARRIER b =
			    Transition(prodLocal, D3D12_RESOURCE_STATE_COMMON,
			               D3D12_RESOURCE_STATE_RENDER_TARGET);
			prod.list->ResourceBarrier(1, &b);
		}
		prod.list->SetGraphicsRootSignature(prodRs);
		float rc[4] = {(float)frames, 0, 0, 0};
		prod.list->SetGraphicsRoot32BitConstants(0, 4, rc, 0);
		prod.list->RSSetViewports(1, &vp);
		prod.list->RSSetScissorRects(1, &sc);
		prod.list->OMSetRenderTargets(1, &prodRtv, FALSE, nullptr);
		prod.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		prod.list->DrawInstanced(3, 1, 0, 0);
		{
			D3D12_RESOURCE_BARRIER b = Transition(prodLocal, D3D12_RESOURCE_STATE_RENDER_TARGET,
			                                      D3D12_RESOURCE_STATE_COPY_SOURCE);
			prod.list->ResourceBarrier(1, &b);
		}
		// The cross-adapter resource stays in COMMON: implicit promotion to
		// COPY_DEST/COPY_SOURCE/PIXEL_SHADER_RESOURCE covers every use here.
		prod.list->CopyTextureRegion(&xaLocProd, 0, 0, 0, &prodLocalLoc, nullptr);
		{
			D3D12_RESOURCE_BARRIER b = Transition(prodLocal, D3D12_RESOURCE_STATE_COPY_SOURCE,
			                                      D3D12_RESOURCE_STATE_COMMON);
			prod.list->ResourceBarrier(1, &b);
		}
		prod.list->Close();
		ID3D12CommandList *pl[] = {prod.list};
		prod.queue->ExecuteCommandLists(1, pl);
		prod.queue->Signal(fenceShared, fv);
		prod.queue->Signal(prod.fence, fv);
		WaitFence(prod, fv, "producer.fence");
		double tProdDone = QpcMs();

		// ---- consumer ----
		double tConsStart = QpcMs();
		cons.queue->Wait(fenceSharedOnCons, fv); // cross-adapter GPU-side sync
		cons.alloc->Reset();
		cons.list->Reset(cons.alloc, doLocalSamplePass ? consPso : nullptr);
		if (doPullCopy) {
			D3D12_RESOURCE_BARRIER b = Transition(consPull, D3D12_RESOURCE_STATE_COMMON,
			                                      D3D12_RESOURCE_STATE_COPY_DEST);
			cons.list->ResourceBarrier(1, &b);
			cons.list->CopyTextureRegion(&consPullLoc, 0, 0, 0, &xaLocCons, nullptr);
			D3D12_RESOURCE_BARRIER b2 =
			    Transition(consPull, D3D12_RESOURCE_STATE_COPY_DEST,
			               doLocalSamplePass ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
			                                 : D3D12_RESOURCE_STATE_COMMON);
			cons.list->ResourceBarrier(1, &b2);
		}
		if (doLocalSamplePass) {
			D3D12_RESOURCE_BARRIER b = Transition(consRt, D3D12_RESOURCE_STATE_COMMON,
			                                      D3D12_RESOURCE_STATE_RENDER_TARGET);
			cons.list->ResourceBarrier(1, &b);
			ID3D12DescriptorHeap *heaps[] = {consSrvHeap};
			cons.list->SetDescriptorHeaps(1, heaps);
			cons.list->SetGraphicsRootSignature(consRs);
			cons.list->SetGraphicsRootDescriptorTable(0, consSrvGpu);
			cons.list->RSSetViewports(1, &vp);
			cons.list->RSSetScissorRects(1, &sc);
			cons.list->OMSetRenderTargets(1, &consRtv, FALSE, nullptr);
			cons.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			cons.list->DrawInstanced(3, 1, 0, 0);
			D3D12_RESOURCE_BARRIER b2 = Transition(consRt, D3D12_RESOURCE_STATE_RENDER_TARGET,
			                                       D3D12_RESOURCE_STATE_COMMON);
			cons.list->ResourceBarrier(1, &b2);
			if (doPullCopy) {
				D3D12_RESOURCE_BARRIER b3 =
				    Transition(consPull, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
				               D3D12_RESOURCE_STATE_COMMON);
				cons.list->ResourceBarrier(1, &b3);
			}
		}
		cons.list->Close();
		ID3D12CommandList *cl[] = {cons.list};
		cons.queue->ExecuteCommandLists(1, cl);
		cons.queue->Signal(cons.fence, fv);
		WaitFence(cons, fv, "consumer.fence");
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
			printf("[xb12] fps=%.1f copy_signal_ms=%.3f/%.3f consumer_done_ms=%.3f/%.3f "
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

	// --- pixel verification: prove the bytes really crossed --------------
	std::string pixelVerdict = "not run";
	{
		ID3D12Resource *verifySrc = doLocalSamplePass ? consRt : consPull;
		if (verifySrc) {
			D3D12_HEAP_PROPERTIES rb{};
			rb.Type = D3D12_HEAP_TYPE_READBACK;
			D3D12_RESOURCE_DESC bd{};
			bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			bd.Width = totalBytes;
			bd.Height = 1;
			bd.DepthOrArraySize = 1;
			bd.MipLevels = 1;
			bd.Format = DXGI_FORMAT_UNKNOWN;
			bd.SampleDesc.Count = 1;
			bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ID3D12Resource *readback = nullptr;
			HRESULT rh = cons.dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd,
			                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
			                                              __uuidof(ID3D12Resource),
			                                              (void **)&readback);
			if (SUCCEEDED(rh)) {
				cons.alloc->Reset();
				cons.list->Reset(cons.alloc, nullptr);
				D3D12_RESOURCE_BARRIER b = Transition(verifySrc, D3D12_RESOURCE_STATE_COMMON,
				                                      D3D12_RESOURCE_STATE_COPY_SOURCE);
				cons.list->ResourceBarrier(1, &b);
				D3D12_TEXTURE_COPY_LOCATION dst{};
				dst.pResource = readback;
				dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
				dst.PlacedFootprint = fp;
				D3D12_TEXTURE_COPY_LOCATION src{};
				src.pResource = verifySrc;
				src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
				src.SubresourceIndex = 0;
				cons.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
				D3D12_RESOURCE_BARRIER b2 = Transition(verifySrc,
				                                       D3D12_RESOURCE_STATE_COPY_SOURCE,
				                                       D3D12_RESOURCE_STATE_COMMON);
				cons.list->ResourceBarrier(1, &b2);
				cons.list->Close();
				ID3D12CommandList *vl[] = {cons.list};
				cons.queue->ExecuteCommandLists(1, vl);
				UINT64 vfen = frames + 1000;
				cons.queue->Signal(cons.fence, vfen);
				WaitFence(cons, vfen, "verify.fence");

				void *mapped = nullptr;
				D3D12_RANGE rr{0, (SIZE_T)totalBytes};
				if (SUCCEEDED(readback->Map(0, &rr, &mapped)) && mapped) {
					const UINT8 *base = (const UINT8 *)mapped;
					// The pattern is float4(uv.x, uv.y, t, 1): R must ramp along
					// x, G must ramp along y, A must be 255.
					UINT rp2 = fp.Footprint.RowPitch;
					auto px = [&](UINT x, UINT y, int c) -> int {
						return base[(size_t)y * rp2 + (size_t)x * 4 + c];
					};
					int nonZero = 0;
					for (UINT y = 0; y < H; y += H / 16 ? H / 16 : 1)
						for (UINT x = 0; x < W; x += W / 16 ? W / 16 : 1)
							if (px(x, y, 0) || px(x, y, 1) || px(x, y, 2))
								nonZero++;
					int rL = px(1, H / 2, 0), rR = px(W - 2, H / 2, 0);
					int gT = px(W / 2, 1, 1), gB = px(W / 2, H - 2, 1);
					int aMid = px(W / 2, H / 2, 3);
					char buf[256];
					bool ramps = (rR > rL + 100) && (gB > gT + 100) && aMid == 255;
					snprintf(buf, sizeof(buf),
					         "%s (nonzero_samples=%d R[left=%d right=%d] G[top=%d bot=%d] A=%d)",
					         ramps ? "PASS - pattern crossed intact"
					               : (nonZero ? "SUSPECT - data present but pattern wrong"
					                          : "FAIL - all zero"),
					         nonZero, rL, rR, gT, gB, aMid);
					pixelVerdict = buf;
					readback->Unmap(0, nullptr);
				} else {
					pixelVerdict = "readback Map failed";
				}
				SafeRelease(readback);
			} else {
				pixelVerdict = "readback buffer create failed " + HrString(rh);
			}
		}
	}

	printf("\n========== xbridge12_bench (D3D12 cross-adapter) summary ==========\n");
	printf("  config          : %ux%u RGBA8 (%.2f MB/frame) mode=%s paced=%d seconds=%.1f\n", W, H,
	       bytes / (1024.0 * 1024.0), mode.c_str(), paced, seconds);
	printf("  producer        : %s \"%s\" LUID=%s\n", prod.label.c_str(), prod.name.c_str(),
	       LuidStr(prod.luid).c_str());
	printf("  consumer        : %s \"%s\" LUID=%s\n", cons.label.c_str(), cons.name.c_str(),
	       LuidStr(cons.luid).c_str());
	printf("  CrossAdapterRowMajorTextureSupported: producer=%s consumer=%s\n",
	       prod.crossAdapterRowMajor ? "TRUE" : "FALSE",
	       cons.crossAdapterRowMajor ? "TRUE" : "FALSE");
	printf("  transport       : %s (%s)\n", usingBuffer ? "cross-adapter BUFFER" : "PLACED TEXTURE",
	       placedNote.c_str());
	printf("  direct sample   : %s\n",
	       doDirectSample ? "YES - consumer sampled the cross-adapter texture directly"
	                      : (directSamplePossible ? "possible, not exercised in this mode"
	                                              : "NOT possible (buffer transport)"));
	printf("  shared fence    : SHARED|SHARED_CROSS_ADAPTER OK (GPU-side queue Wait in use)\n");
	printf("  row pitch       : %u bytes (heap %llu bytes)\n", fp.Footprint.RowPitch,
	       (unsigned long long)heapSize);
	printf("  pixel check     : %s\n", pixelVerdict.c_str());
	printf("  frames          : %llu in %.3f s -> fps=%.2f\n", frames, elapsed, fps);
	printf("  copy_signal_ms  : mean=%.3f p95=%.3f max=%.3f\n", aCopy.mean(), aCopy.p95(),
	       aCopy.maxv());
	printf("  consumer_done_ms: mean=%.3f p95=%.3f max=%.3f\n", aCons.mean(), aCons.p95(),
	       aCons.maxv());
	printf("  roundtrip_ms    : mean=%.3f p95=%.3f max=%.3f\n", aRt.mean(), aRt.p95(), aRt.maxv());
	printf("  bandwidth_GBps  : %.3f   (meaningful in --mode=copyonly --paced=0)\n", gbps);
	printf("===================================================================\n");
	fflush(stdout);

	// cleanup
	SafeRelease(consPso);
	SafeRelease(prodPso);
	SafeRelease(consRs);
	SafeRelease(prodRs);
	SafeRelease(consSrvHeap);
	SafeRelease(consRtvHeap);
	SafeRelease(prodRtvHeap);
	SafeRelease(consRt);
	SafeRelease(consPull);
	SafeRelease(prodLocal);
	SafeRelease(vsb);
	SafeRelease(patb);
	SafeRelease(smpb);
	SafeRelease(fenceSharedOnCons);
	SafeRelease(fenceShared);
	if (fenceHandle)
		CloseHandle(fenceHandle);
	SafeRelease(xaCons);
	SafeRelease(xaProd);
	SafeRelease(heapCons);
	SafeRelease(heapProd);
	if (heapHandle)
		CloseHandle(heapHandle);
	for (Gpu12 *g : {&prod, &cons}) {
		if (g->evt)
			CloseHandle(g->evt);
		SafeRelease(g->fence);
		SafeRelease(g->list);
		SafeRelease(g->alloc);
		SafeRelease(g->queue);
		SafeRelease(g->dev);
	}
	for (auto &a : ads)
		SafeRelease(a.ad);
	SafeRelease(factory);
	return 0;
}
