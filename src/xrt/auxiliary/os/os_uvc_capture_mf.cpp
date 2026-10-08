// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Media Foundation capture backend for the UVC stereo camera source —
 *         see os_uvc_capture.h.
 *
 * Privacy-relevant facts, in order of the calls:
 *  - enumerate: MFEnumDeviceSources only. IMFActivate objects are read for
 *    their friendly name + symbolic link and released; nothing is activated.
 *  - list_modes: MFCreateDeviceSource + the presentation descriptor's media
 *    types, then IMFMediaSource::Shutdown. The device object exists for the
 *    duration of the call; no stream is selected or started (no frames, no
 *    camera light). Only ever called for a device the user's config matched.
 *  - open: the device source + an ASYNC source reader; streaming starts with
 *    the first ReadSample. close flushes, releases the reader and shuts the
 *    source down. The service calls these only while a stream is started.
 *  - the decoder report (`displayxr-cli camera uvc-devices --decoder`) touches
 *    no camera at all: DXGI adapters + the registered MJPEG decoder MFTs.
 *
 * Decode paths (the reason this file is more than a reader):
 *  - HARDWARE (default when possible): a 4K60 MJPEG webcam decoded in software
 *    delivers ~11 Hz with ~90 ms latency on a laptop CPU. So open() first looks
 *    for an adapter that has a hardware MJPEG decoder MFT (MFTEnumEx HARDWARE,
 *    matched to DXGI adapters by vendor id), creates a D3D11 device on it
 *    (VIDEO_SUPPORT, multithread-protected), hands the reader an
 *    IMFDXGIDeviceManager (MF_SOURCE_READER_D3D_MANAGER +
 *    MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS + advanced video processing), and
 *    asks for NV12 AT THE OUTPUT SIZE the source wants (2 x eye width x eye
 *    height) — so the GPU decodes AND downscales (half-SBS 3840x2160 ->
 *    2560x720) and only that small frame crosses to the CPU: one
 *    CopySubresourceRegion into a staging NV12 texture + one Map per frame.
 *    After the topology is built the reader's decoder is inspected; if MF put
 *    a SOFTWARE decoder behind the device manager anyway (nothing gained, a
 *    GPU round trip added) the device is reopened on the software path.
 *  - SOFTWARE (fallback, one WARN saying why): the plain reader with advanced
 *    video processing at the native size; the source splits / downscales on
 *    the CPU.
 *  Knobs (service environment): DXR_STEREO_CAMERA_UVC_DECODER=auto|hardware|
 *  software (hardware = never fall back: the open fails instead),
 *  DXR_STEREO_CAMERA_UVC_ADAPTER=<substring of the adapter description> to
 *  pick among several capable adapters (default: the one with the most
 *  dedicated video memory, i.e. the discrete GPU).
 *
 * Threading: open / read / close run on the service's camera thread (COM MTA,
 * initialised here and balanced in close); the reader's callback runs on a
 * Media Foundation work-queue thread and only swaps the newest sample in under
 * a lock + signals an auto-reset event. read() waits on that event, takes the
 * newest sample (older ones are simply dropped: latest-wins, like every other
 * camera source) and keeps its buffer LOCKED / its staging texture MAPPED until
 * the next read / close so the planes it hands out stay valid. The D3D11 device
 * is shared with MF's own threads, hence ID3D10Multithread protection.
 *
 * @ingroup aux_os
 */

#include "os/os_uvc_capture.h"
#include "os/os_time.h"
#include "util/u_logging.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d10.h> // ID3D10Multithread
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {

template <typename T>
void
safe_release(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

//! COM (MTA) + Media Foundation for the duration of one call or one open device.
struct mf_scope
{
	bool com = false;
	bool mf = false;

	bool
	begin()
	{
		HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		com = SUCCEEDED(hr); // RPC_E_CHANGED_MODE: an STA thread — MF works there too, nothing to balance
		mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET));
		return mf;
	}

	void
	end()
	{
		if (mf) {
			MFShutdown();
			mf = false;
		}
		if (com) {
			CoUninitialize();
			com = false;
		}
	}
};

void
wide_to_utf8(const wchar_t *w, char *out, size_t cap)
{
	out[0] = '\0';
	if (w == nullptr) {
		return;
	}
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, nullptr, nullptr);
	if (n <= 0) {
		out[0] = '\0';
	}
	out[cap - 1] = '\0';
}

bool
utf8_to_wide(const char *s, wchar_t *out, size_t cap)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, (int)cap);
	return n > 0;
}

//! Append printf-style to a bounded buffer.
void
appendf(char *buf, size_t cap, const char *fmt, ...)
{
	size_t n = std::strlen(buf);
	if (n + 1 >= cap) {
		return;
	}
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf + n, cap - n, fmt, ap);
	va_end(ap);
}

//! The device source for symbolic link @p id (not started).
HRESULT
create_device_source(const char *id, IMFMediaSource **out)
{
	*out = nullptr;
	// "file:<path>": a media FILE through the identical reader / decoder path —
	// `displayxr-cli camera uvc-decode-bench` measures the decoders with it, no
	// camera involved. Never an enumerated id, so the service cannot reach it.
	if (std::strncmp(id, "file:", 5) == 0) {
		wchar_t path[1024];
		if (!utf8_to_wide(id + 5, path, sizeof(path) / sizeof(path[0]))) {
			return E_INVALIDARG;
		}
		IMFSourceResolver *res = nullptr;
		IUnknown *obj = nullptr;
		MF_OBJECT_TYPE type = MF_OBJECT_INVALID;
		HRESULT hr = MFCreateSourceResolver(&res);
		if (SUCCEEDED(hr)) {
			hr = res->CreateObjectFromURL(path, MF_RESOLUTION_MEDIASOURCE, nullptr, &type, &obj);
		}
		if (SUCCEEDED(hr)) {
			hr = obj->QueryInterface(IID_PPV_ARGS(out));
		}
		safe_release(obj);
		safe_release(res);
		return hr;
	}
	wchar_t link[512];
	if (!utf8_to_wide(id, link, sizeof(link) / sizeof(link[0]))) {
		return E_INVALIDARG;
	}
	IMFAttributes *attr = nullptr;
	HRESULT hr = MFCreateAttributes(&attr, 2);
	if (SUCCEEDED(hr)) {
		hr = attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
	}
	if (SUCCEEDED(hr)) {
		hr = attr->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, link);
	}
	if (SUCCEEDED(hr)) {
		hr = MFCreateDeviceSource(attr, out);
	}
	safe_release(attr);
	return hr;
}

//! The subtypes the reader can turn into NV12 for us.
int
subtype_rank(const GUID &st)
{
	if (st == MFVideoFormat_NV12) {
		return 3; // no conversion
	}
	if (st == MFVideoFormat_YUY2) {
		return 2;
	}
	if (st == MFVideoFormat_MJPG) {
		return 1; // decoded by an MJPEG decoder MFT
	}
	return 0;
}

bool
type_mode(IMFMediaType *t, u_stereo_uvc_mode *m, GUID *subtype)
{
	UINT32 w = 0, h = 0, num = 0, den = 0;
	if (FAILED(MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h)) || w == 0 || h == 0) {
		return false;
	}
	if (FAILED(MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &num, &den)) || den == 0) {
		num = 0;
		den = 1;
	}
	GUID st = GUID_NULL;
	t->GetGUID(MF_MT_SUBTYPE, &st);
	if (subtype != nullptr) {
		*subtype = st;
	}
	m->width = w;
	m->height = h;
	m->fps = (float)((double)num / (double)den);
	return true;
}


/*
 *
 * Decoder policy: which adapter has a hardware MJPEG decoder.
 *
 */

enum class decoder_pref
{
	automatic,
	hardware,
	software,
};

decoder_pref
read_decoder_pref()
{
	const char *e = getenv("DXR_STEREO_CAMERA_UVC_DECODER");
	if (e == nullptr || e[0] == '\0' || std::strcmp(e, "auto") == 0) {
		return decoder_pref::automatic;
	}
	if (std::strcmp(e, "hardware") == 0 || std::strcmp(e, "hw") == 0) {
		return decoder_pref::hardware;
	}
	if (std::strcmp(e, "software") == 0 || std::strcmp(e, "sw") == 0) {
		return decoder_pref::software;
	}
	U_LOG_W("uvc capture: DXR_STEREO_CAMERA_UVC_DECODER=\"%s\" is not auto|hardware|software — using auto", e);
	return decoder_pref::automatic;
}

struct hw_decoder
{
	char name[128];
	uint32_t vendor; //!< PCI vendor id from MFT_ENUM_HARDWARE_VENDOR_ID_Attribute ("VEN_xxxx"), 0 = unknown
};

//! Registered decoder MFTs taking MJPEG (@p hardware: the HARDWARE ones).
uint32_t
list_mjpeg_decoders(bool hardware, hw_decoder *out, uint32_t cap)
{
	MFT_REGISTER_TYPE_INFO in = {MFMediaType_Video, MFVideoFormat_MJPG};
	IMFActivate **acts = nullptr;
	UINT32 count = 0;
	UINT32 flags = (hardware ? MFT_ENUM_FLAG_HARDWARE : (MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT)) |
	               MFT_ENUM_FLAG_SORTANDFILTER;
	if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, &in, nullptr, &acts, &count))) {
		return 0;
	}
	uint32_t n = 0;
	for (UINT32 i = 0; i < count; i++) {
		if (n < cap) {
			hw_decoder *d = &out[n++];
			std::memset(d, 0, sizeof(*d));
			WCHAR *s = nullptr;
			UINT32 len = 0;
			if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &s, &len))) {
				wide_to_utf8(s, d->name, sizeof(d->name));
				CoTaskMemFree(s);
			} else {
				std::snprintf(d->name, sizeof(d->name), "(unnamed decoder)");
			}
			s = nullptr;
			if (SUCCEEDED(acts[i]->GetAllocatedString(MFT_ENUM_HARDWARE_VENDOR_ID_Attribute, &s, &len))) {
				char v[64];
				wide_to_utf8(s, v, sizeof(v));
				CoTaskMemFree(s);
				const char *p = std::strstr(v, "VEN_");
				d->vendor = p != nullptr ? (uint32_t)std::strtoul(p + 4, nullptr, 16) : 0;
			}
		}
		acts[i]->Release();
	}
	CoTaskMemFree(acts);
	return n;
}

struct adapter_choice
{
	bool found = false;
	IDXGIAdapter1 *adapter = nullptr; //!< owned
	char desc[128] = {0};
	char decoder[128] = {0};
	char why[160] = {0};
};

/*!
 * Pick the adapter to decode on: one whose vendor has a hardware MJPEG decoder
 * MFT; DXR_STEREO_CAMERA_UVC_ADAPTER (description substring) wins, else the
 * most dedicated video memory. @p report (may be NULL) gets the whole picture.
 */
adapter_choice
choose_adapter(char *report, size_t report_cap)
{
	adapter_choice c;
	hw_decoder hw[16];
	uint32_t nhw = list_mjpeg_decoders(true, hw, 16);
	if (report != nullptr) {
		appendf(report, report_cap, "hardware MJPEG decoder MFTs: %u\n", nhw);
		for (uint32_t i = 0; i < nhw; i++) {
			appendf(report, report_cap, "  - \"%s\" (vendor 0x%04x)\n", hw[i].name, hw[i].vendor);
		}
		hw_decoder sw[16];
		uint32_t nsw = list_mjpeg_decoders(false, sw, 16);
		appendf(report, report_cap, "software MJPEG decoder MFTs: %u\n", nsw);
		for (uint32_t i = 0; i < nsw; i++) {
			appendf(report, report_cap, "  - \"%s\"\n", sw[i].name);
		}
	}
	const char *want = getenv("DXR_STEREO_CAMERA_UVC_ADAPTER");
	IDXGIFactory1 *f = nullptr;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) {
		std::snprintf(c.why, sizeof(c.why), "no DXGI factory");
		return c;
	}
	SIZE_T best_mem = 0;
	bool best_named = false;
	if (report != nullptr) {
		appendf(report, report_cap, "adapters:\n");
	}
	for (UINT i = 0;; i++) {
		IDXGIAdapter1 *a = nullptr;
		if (f->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) {
			break;
		}
		DXGI_ADAPTER_DESC1 d;
		if (FAILED(a->GetDesc1(&d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
			safe_release(a);
			continue;
		}
		char name[128];
		wide_to_utf8(d.Description, name, sizeof(name));
		int dec = -1;
		for (uint32_t k = 0; k < nhw && dec < 0; k++) {
			if (hw[k].vendor == d.VendorId) {
				dec = (int)k;
			}
		}
		if (report != nullptr) {
			appendf(report, report_cap,
			        "  [%u] \"%s\" vendor 0x%04x, %llu MB dedicated, hardware MJPEG decoder: %s\n", i, name,
			        d.VendorId, (unsigned long long)(d.DedicatedVideoMemory >> 20),
			        dec >= 0 ? hw[dec].name : "none");
		}
		bool named = want != nullptr && want[0] != '\0' && std::strstr(name, want) != nullptr;
		bool better = dec >= 0 && (!c.found || (named && !best_named) ||
		                           (named == best_named && d.DedicatedVideoMemory > best_mem));
		if (better) {
			safe_release(c.adapter);
			c.adapter = a;
			c.adapter->AddRef();
			c.found = true;
			best_mem = d.DedicatedVideoMemory;
			best_named = named;
			std::snprintf(c.desc, sizeof(c.desc), "%s", name);
			std::snprintf(c.decoder, sizeof(c.decoder), "%s", hw[dec].name);
		}
		safe_release(a);
	}
	safe_release(f);
	if (!c.found) {
		std::snprintf(c.why, sizeof(c.why), "%s",
		              nhw == 0 ? "no hardware MJPEG decoder MFT is registered on this machine"
		                       : "no adapter matches a hardware MJPEG decoder MFT's vendor");
	} else if (want != nullptr && want[0] != '\0' && !best_named) {
		std::snprintf(c.why, sizeof(c.why), "DXR_STEREO_CAMERA_UVC_ADAPTER=\"%s\" matches no capable adapter",
		              want);
	}
	return c;
}


/*
 *
 * The async reader: newest sample wins.
 *
 */

class reader_cb final : public IMFSourceReaderCallback
{
public:
	reader_cb()
	{
		InitializeCriticalSection(&lock_);
		event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		flushed_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	}

	// IUnknown
	STDMETHODIMP
	QueryInterface(REFIID iid, void **ppv) override
	{
		if (ppv == nullptr) {
			return E_POINTER;
		}
		if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFSourceReaderCallback)) {
			*ppv = static_cast<IMFSourceReaderCallback *>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}

	STDMETHODIMP_(ULONG)
	AddRef() override
	{
		return (ULONG)InterlockedIncrement(&refs_);
	}

	STDMETHODIMP_(ULONG)
	Release() override
	{
		LONG r = InterlockedDecrement(&refs_);
		if (r == 0) {
			delete this;
		}
		return (ULONG)r;
	}

	// IMFSourceReaderCallback
	STDMETHODIMP
	OnReadSample(HRESULT hr, DWORD, DWORD flags, LONGLONG, IMFSample *sample) override
	{
		const int64_t now = os_monotonic_get_ns();
		EnterCriticalSection(&lock_);
		if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)) != 0) {
			if (!failed_) {
				failed_ = true;
				fail_hr_ = FAILED(hr) ? hr : E_FAIL;
				fail_flags_ = flags;
			}
		} else if (sample != nullptr) {
			if (pending_ != nullptr) {
				dropped_++;
				pending_->Release();
			}
			pending_ = sample;
			pending_->AddRef();
			pending_ns_ = now;
			delivered_++;
		}
		// Keep the pipe full until close — but issue the next ReadSample OUTSIDE
		// the lock: the reader may decode the next sample inline and call us
		// back re-entrantly from inside ReadSample, and a held (recursive) lock
		// would then starve read() for as long as the chain runs (a whole file;
		// a decode's worth of latency per frame on a camera). The reader is
		// AddRef'd under the lock, so teardown releasing it meanwhile is safe.
		IMFSourceReader *next = nullptr;
		if (!failed_ && !stopping_ && reader_ != nullptr) {
			next = reader_;
			next->AddRef();
		}
		LeaveCriticalSection(&lock_);
		SetEvent(event_);
		if (next != nullptr && pace_ns_ > 0) {
			// file: ids only (the decode bench) — a file is not paced by a
			// sensor: the pacer thread requests the next frame at the file's
			// own rate, like a camera delivers it. Never sleep here: blocking
			// this MF work-queue thread stalls the reader.
			next->Release();
			SetEvent(pace_event_);
			return S_OK;
		}
		if (next != nullptr) {
			HRESULT rr = next->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr,
			                              nullptr, nullptr);
			if (FAILED(rr)) {
				EnterCriticalSection(&lock_);
				if (!failed_ && !stopping_) {
					failed_ = true;
					fail_hr_ = rr;
					fail_flags_ = 0xffffffffu; // the re-issued ReadSample itself failed
				}
				LeaveCriticalSection(&lock_);
				SetEvent(event_);
			}
			next->Release();
		}
		return S_OK;
	}

	STDMETHODIMP
	OnFlush(DWORD) override
	{
		SetEvent(flushed_);
		return S_OK;
	}

	STDMETHODIMP
	OnEvent(DWORD, IMFMediaEvent *) override
	{
		return S_OK;
	}

	// Ours.
	CRITICAL_SECTION lock_;
	HANDLE event_ = nullptr;
	HANDLE flushed_ = nullptr;
	IMFSourceReader *reader_ = nullptr; //!< raw: the reader owns us, not the reverse
	IMFSample *pending_ = nullptr;
	int64_t pending_ns_ = 0;
	bool failed_ = false;
	bool stopping_ = false;
	HRESULT fail_hr_ = S_OK;
	DWORD fail_flags_ = 0; //!< MF_SOURCE_READERF_* of the failing callback (all ones: ReadSample failed)
	uint64_t dropped_ = 0;
	uint64_t delivered_ = 0; //!< samples the reader handed us
	int64_t pace_ns_ = 0;    //!< file: ids only — request frames at this period
	HANDLE pace_event_ = nullptr;
	HANDLE pace_thread_ = nullptr;
	volatile LONG pace_quit_ = 0;

	//! file: ids only: a thread that issues ReadSample every pace_ns_.
	bool
	start_pacer(int64_t period_ns)
	{
		pace_ns_ = period_ns;
		pace_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		AddRef(); // the thread's reference
		pace_thread_ = CreateThread(nullptr, 0, pacer_main, this, 0, nullptr);
		if (pace_thread_ == nullptr) {
			Release();
			pace_ns_ = 0;
			return false;
		}
		return true;
	}

	void
	stop_pacer()
	{
		if (pace_thread_ != nullptr) {
			InterlockedExchange(&pace_quit_, 1);
			SetEvent(pace_event_);
			WaitForSingleObject(pace_thread_, 2000);
			CloseHandle(pace_thread_);
			pace_thread_ = nullptr;
		}
	}

	static DWORD WINAPI
	pacer_main(LPVOID p)
	{
		reader_cb *cb = (reader_cb *)p;
		const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
		int64_t t0 = 0;
		uint64_t n = 0;
		bool warned = false;
		while (cb->pace_quit_ == 0) {
			if (WaitForSingleObject(cb->pace_event_, 500) != WAIT_OBJECT_0 || cb->pace_quit_ != 0) {
				continue;
			}
			int64_t now = os_monotonic_get_ns();
			if (t0 == 0) {
				t0 = now;
			}
			int64_t wait = t0 + (int64_t)(++n) * cb->pace_ns_ - now;
			if (wait > 0) {
				os_nanosleep(wait);
			}
			EnterCriticalSection(&cb->lock_);
			IMFSourceReader *r = (!cb->failed_ && !cb->stopping_) ? cb->reader_ : nullptr;
			if (r != nullptr) {
				r->AddRef();
			}
			LeaveCriticalSection(&cb->lock_);
			if (r != nullptr) {
				HRESULT hr = r->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr,
				                           nullptr, nullptr, nullptr);
				if (FAILED(hr) && !warned) {
					warned = true;
					U_LOG_W("uvc capture (decode bench pacer): ReadSample failed, hr 0x%08lx",
					        (unsigned long)hr);
				}
				r->Release();
			}
		}
		if (com) {
			CoUninitialize();
		}
		cb->Release();
		return 0;
	}

private:
	~reader_cb()
	{
		if (pending_ != nullptr) {
			pending_->Release();
		}
		if (event_ != nullptr) {
			CloseHandle(event_);
		}
		if (flushed_ != nullptr) {
			CloseHandle(flushed_);
		}
		if (pace_event_ != nullptr) {
			CloseHandle(pace_event_);
		}
		DeleteCriticalSection(&lock_);
	}

	volatile LONG refs_ = 1;
};

//! One open device.
struct mf_handle
{
	mf_scope scope;
	DWORD thread_id = 0;
	IMFMediaSource *source = nullptr;
	IMFSourceReader *reader = nullptr;
	reader_cb *cb = nullptr;
	uint32_t pixel = 0; //!< enum u_stereo_uvc_pixel of the reader's OUTPUT
	uint32_t width = 0, height = 0;
	LONG default_stride = 0;

	// Hardware path.
	bool hw = false;
	ID3D11Device *dev = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	IMFDXGIDeviceManager *dxgi_mgr = nullptr;
	//! The pipelined readback ring (read_back_texture).
	static const int RING = 3;
	ID3D11Texture2D *ring[RING] = {};
	ID3D11Query *ring_q[RING] = {};
	int64_t ring_t[RING] = {};
	int pend[RING] = {}; //!< slots whose copy is queued, not yet returned (oldest first)
	int npend = 0;
	uint8_t *cpu = nullptr; //!< the read-back frame (tight NV12), valid until the next read
	size_t cpu_size = 0;

	// The sample whose buffer read() handed out, locked until the next read / close.
	IMFSample *held = nullptr;
	IMFMediaBuffer *held_buf = nullptr;
	IMF2DBuffer2 *held_2d = nullptr;
	bool held_locked = false; //!< held_buf->Lock() (not 2D) is outstanding

	// Diagnostics.
	char decoder[160];
	uint64_t frames = 0;
	int64_t readback_ns = 0;
};

void
unlock_held(mf_handle *h)
{
	if (h->held_2d != nullptr) {
		h->held_2d->Unlock2D();
		safe_release(h->held_2d);
	}
	if (h->held_locked) {
		h->held_buf->Unlock();
		h->held_locked = false;
	}
	safe_release(h->held_buf);
	safe_release(h->held);
}


/*
 *
 * Backend ops.
 *
 */

uint32_t
mf_enumerate(void *ctx, u_stereo_uvc_device *out, uint32_t cap)
{
	(void)ctx;
	mf_scope scope;
	if (!scope.begin()) {
		scope.end();
		return 0;
	}
	IMFAttributes *attr = nullptr;
	IMFActivate **devs = nullptr;
	UINT32 count = 0;
	HRESULT hr = MFCreateAttributes(&attr, 1);
	if (SUCCEEDED(hr)) {
		hr = attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
	}
	if (SUCCEEDED(hr)) {
		hr = MFEnumDeviceSources(attr, &devs, &count);
	}
	uint32_t n = 0;
	for (UINT32 i = 0; SUCCEEDED(hr) && i < count; i++) {
		if (n < cap && out != nullptr) {
			u_stereo_uvc_device *d = &out[n];
			std::memset(d, 0, sizeof(*d));
			WCHAR *s = nullptr;
			UINT32 len = 0;
			if (SUCCEEDED(devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &s, &len))) {
				wide_to_utf8(s, d->name, sizeof(d->name));
				CoTaskMemFree(s);
			}
			s = nullptr;
			if (SUCCEEDED(devs[i]->GetAllocatedString(
			        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &s, &len))) {
				wide_to_utf8(s, d->id, sizeof(d->id));
				CoTaskMemFree(s);
			}
			d->has_vid_pid = u_stereo_uvc_parse_vid_pid(d->id, &d->vid, &d->pid);
			n++;
		}
		devs[i]->Release();
	}
	CoTaskMemFree(devs);
	safe_release(attr);
	scope.end();
	return n;
}

uint32_t
mf_list_modes(void *ctx, const char *id, u_stereo_uvc_mode *out, uint32_t cap)
{
	(void)ctx;
	mf_scope scope;
	if (!scope.begin()) {
		scope.end();
		return 0;
	}
	IMFMediaSource *src = nullptr;
	IMFPresentationDescriptor *pd = nullptr;
	IMFStreamDescriptor *sd = nullptr;
	IMFMediaTypeHandler *th = nullptr;
	uint32_t n = 0;
	BOOL selected = FALSE;
	DWORD types = 0;
	HRESULT hr = create_device_source(id, &src);
	if (SUCCEEDED(hr)) {
		hr = src->CreatePresentationDescriptor(&pd);
	}
	if (SUCCEEDED(hr)) {
		hr = pd->GetStreamDescriptorByIndex(0, &selected, &sd);
	}
	if (SUCCEEDED(hr)) {
		hr = sd->GetMediaTypeHandler(&th);
	}
	if (SUCCEEDED(hr)) {
		hr = th->GetMediaTypeCount(&types);
	}
	for (DWORD i = 0; SUCCEEDED(hr) && i < types && n < cap; i++) {
		IMFMediaType *t = nullptr;
		if (FAILED(th->GetMediaTypeByIndex(i, &t))) {
			continue;
		}
		u_stereo_uvc_mode m;
		GUID st;
		if (type_mode(t, &m, &st) && subtype_rank(st) > 0) {
			bool dup = false;
			for (uint32_t k = 0; k < n; k++) {
				dup |= out[k].width == m.width && out[k].height == m.height && out[k].fps == m.fps;
			}
			if (!dup && out != nullptr) {
				out[n++] = m;
			}
		}
		safe_release(t);
	}
	safe_release(th);
	safe_release(sd);
	safe_release(pd);
	if (src != nullptr) {
		src->Shutdown();
		safe_release(src);
	}
	scope.end();
	return n;
}

//! Pick the native type: the exact size, the rate closest to @p want (0 = fastest), NV12 > YUY2 > MJPG.
IMFMediaType *
pick_native(IMFSourceReader *reader, const u_stereo_uvc_mode *want)
{
	IMFMediaType *best = nullptr;
	float best_fps = 0.0f;
	int best_rank = 0;
	for (DWORD i = 0;; i++) {
		IMFMediaType *t = nullptr;
		HRESULT hr = reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &t);
		if (hr == MF_E_NO_MORE_TYPES || FAILED(hr)) {
			break;
		}
		u_stereo_uvc_mode m;
		GUID st = GUID_NULL;
		const bool sized = type_mode(t, &m, &st);
		const int rank = sized ? subtype_rank(st) : 0;
		if (rank > 0 && m.width == want->width && m.height == want->height) {
			bool better;
			if (best == nullptr) {
				better = true;
			} else if (want->fps > 0.0f) {
				float d = m.fps > want->fps ? m.fps - want->fps : want->fps - m.fps;
				float bd = best_fps > want->fps ? best_fps - want->fps : want->fps - best_fps;
				better = d < bd - 0.01f || (d <= bd + 0.01f && rank > best_rank);
			} else {
				better = m.fps > best_fps + 0.01f || (m.fps >= best_fps - 0.01f && rank > best_rank);
			}
			if (better) {
				safe_release(best);
				best = t;
				best->AddRef();
				best_fps = m.fps;
				best_rank = rank;
			}
		}
		safe_release(t);
	}
	return best;
}

//! Ask the reader for @p subtype output at @p w x @p h (0 = the native size).
HRESULT
set_output(IMFSourceReader *reader, IMFMediaType *native, const GUID &subtype, uint32_t w, uint32_t h)
{
	IMFMediaType *out = nullptr;
	HRESULT hr = MFCreateMediaType(&out);
	if (SUCCEEDED(hr)) {
		hr = out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	}
	if (SUCCEEDED(hr)) {
		hr = out->SetGUID(MF_MT_SUBTYPE, subtype);
	}
	UINT32 nw = 0, nh = 0, num = 0, den = 0;
	if (SUCCEEDED(hr) && SUCCEEDED(MFGetAttributeSize(native, MF_MT_FRAME_SIZE, &nw, &nh))) {
		hr = MFSetAttributeSize(out, MF_MT_FRAME_SIZE, w > 0 ? w : nw, h > 0 ? h : nh);
	}
	if (SUCCEEDED(hr) && SUCCEEDED(MFGetAttributeRatio(native, MF_MT_FRAME_RATE, &num, &den))) {
		hr = MFSetAttributeRatio(out, MF_MT_FRAME_RATE, num, den);
	}
	if (SUCCEEDED(hr)) {
		// The video processor preserves the DISPLAY aspect ratio: asked for
		// 2560x720 from a 16:9 3840x2160 frame with square pixels it LETTERBOXES
		// (measured on the real webcam: a centred ~1280 px picture between
		// black bars). Declare the output's pixels anamorphic so its display
		// aspect equals the source's — the scale is then a plain per-axis
		// resize of the whole SBS frame, each half staying its own eye:
		//   PAR_out = PAR_src * (src_w * out_h) / (src_h * out_w)   (1:2 here)
		UINT32 pn = 1, pd = 1;
		if (FAILED(MFGetAttributeRatio(native, MF_MT_PIXEL_ASPECT_RATIO, &pn, &pd)) || pn == 0 || pd == 0) {
			pn = pd = 1;
		}
		const uint32_t ow = w > 0 ? w : nw, oh = h > 0 ? h : nh;
		uint64_t num = (uint64_t)pn * nw * oh, den = (uint64_t)pd * nh * ow;
		for (uint64_t a = num, b = den; b != 0;) { // reduce by the gcd
			uint64_t r = a % b;
			a = b;
			b = r;
			if (b == 0) {
				num /= a;
				den /= a;
			}
		}
		while (num > 0xffffffffull || den > 0xffffffffull) {
			num >>= 1;
			den >>= 1;
		}
		hr = MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, (UINT32)(num > 0 ? num : 1),
		                         (UINT32)(den > 0 ? den : 1));
	}
	if (SUCCEEDED(hr)) {
		hr = out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	}
	if (SUCCEEDED(hr)) {
		hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, out);
	}
	safe_release(out);
	return hr;
}

/*!
 * The decoder the reader put in front of our output, after the topology is
 * built: "<name> (hardware)" / "(software)" into @p out. Returns whether it is
 * a hardware MFT. No decoder (an NV12 / YUY2 native type) counts as hardware-
 * neutral: true, "none (uncompressed native type)".
 */
bool
describe_decoder(IMFSourceReader *reader, char *out, size_t cap)
{
	std::snprintf(out, cap, "none (uncompressed native type)");
	IMFSourceReaderEx *ex = nullptr;
	if (FAILED(reader->QueryInterface(IID_PPV_ARGS(&ex)))) {
		std::snprintf(out, cap, "unknown (no IMFSourceReaderEx)");
		return false;
	}
	bool hw = true;
	for (DWORD i = 0;; i++) {
		GUID cat = GUID_NULL;
		IMFTransform *t = nullptr;
		if (FAILED(ex->GetTransformForStream((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &cat, &t))) {
			break;
		}
		if (cat == MFT_CATEGORY_VIDEO_DECODER) {
			IMFAttributes *a = nullptr;
			char name[128] = "(unnamed decoder)";
			bool is_hw = false;
			if (SUCCEEDED(t->GetAttributes(&a)) && a != nullptr) {
				WCHAR *s = nullptr;
				UINT32 len = 0;
				if (SUCCEEDED(a->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &s, &len))) {
					wide_to_utf8(s, name, sizeof(name));
					CoTaskMemFree(s);
				}
				// A hardware MFT carries its device URL and is asynchronous.
				UINT32 url_len = 0;
				is_hw = SUCCEEDED(a->GetStringLength(MFT_ENUM_HARDWARE_URL_Attribute, &url_len)) ||
				        MFGetAttributeUINT32(a, MF_TRANSFORM_ASYNC, FALSE) != FALSE;
				safe_release(a);
			}
			std::snprintf(out, cap, "%s (%s)", name, is_hw ? "hardware" : "software");
			hw = is_hw;
		}
		safe_release(t);
	}
	safe_release(ex);
	return hw;
}

void
mf_close(void *handle);

/*!
 * Create the D3D11 device + DXGI device manager on @p adapter for the
 * hardware path. Returns false (handle untouched beyond its d3d members) on
 * any failure.
 */
bool
create_hw_device(mf_handle *h, IDXGIAdapter1 *adapter)
{
	static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	HRESULT hr =
	    D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
	                      (UINT)(sizeof(levels) / sizeof(levels[0])), D3D11_SDK_VERSION, &h->dev, nullptr, &h->ctx);
	if (FAILED(hr)) {
		return false;
	}
	ID3D10Multithread *mt = nullptr;
	if (SUCCEEDED(h->dev->QueryInterface(IID_PPV_ARGS(&mt)))) {
		mt->SetMultithreadProtected(TRUE); // MF's decoder threads share this device
		safe_release(mt);
	}
	UINT token = 0;
	hr = MFCreateDXGIDeviceManager(&token, &h->dxgi_mgr);
	if (SUCCEEDED(hr)) {
		hr = h->dxgi_mgr->ResetDevice(h->dev, token);
	}
	return SUCCEEDED(hr);
}

/*!
 * One attempt at opening the reader. @p adapter non-NULL = the hardware path
 * (D3D manager, GPU scaling to @p out_w x @p out_h).
 */
HRESULT
open_reader(
    mf_handle *h, const char *id, const u_stereo_uvc_mode *mode, IDXGIAdapter1 *adapter, uint32_t out_w, uint32_t out_h)
{
	h->hw = adapter != nullptr;
	h->cb = new (std::nothrow) reader_cb();
	IMFAttributes *attr = nullptr;
	IMFMediaType *native = nullptr;
	IMFMediaType *cur = nullptr;
	HRESULT hr = h->cb != nullptr && h->cb->event_ != nullptr ? S_OK : E_OUTOFMEMORY;
	if (SUCCEEDED(hr) && h->hw && !create_hw_device(h, adapter)) {
		hr = E_FAIL;
	}
	if (SUCCEEDED(hr)) {
		hr = create_device_source(id, &h->source);
	}
	if (SUCCEEDED(hr)) {
		hr = MFCreateAttributes(&attr, 4);
	}
	if (SUCCEEDED(hr)) {
		hr = attr->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, static_cast<IMFSourceReaderCallback *>(h->cb));
	}
	if (SUCCEEDED(hr)) {
		// The reader inserts the MJPEG decoder MFT + colour conversion (+ scaling) to NV12.
		hr = attr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
	}
	if (SUCCEEDED(hr) && h->hw) {
		hr = attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
		if (SUCCEEDED(hr)) {
			hr = attr->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, h->dxgi_mgr);
		}
	}
	if (SUCCEEDED(hr)) {
		hr = MFCreateSourceReaderFromMediaSource(h->source, attr, &h->reader);
	}
	if (SUCCEEDED(hr)) {
		native = pick_native(h->reader, mode);
		hr = native != nullptr ? S_OK : MF_E_INVALIDMEDIATYPE;
	}
	if (SUCCEEDED(hr)) {
		hr = h->reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, native);
	}
	if (SUCCEEDED(hr)) {
		// Ask for the OUTPUT size on both paths: hardware decodes AND scales on
		// the GPU; software lets MF's (multithreaded) video processor scale,
		// which beats decoding 4K and resampling it on the camera thread. If
		// the scaler refuses, native size and the source downscales.
		if (SUCCEEDED(set_output(h->reader, native, MFVideoFormat_NV12, out_w, out_h))) {
			h->pixel = U_STEREO_UVC_PIXEL_NV12;
		} else if (out_w > 0 && SUCCEEDED(set_output(h->reader, native, MFVideoFormat_NV12, 0, 0))) {
			h->pixel = U_STEREO_UVC_PIXEL_NV12; // no scaler: full size, the source downscales
		} else if (!h->hw && SUCCEEDED(set_output(h->reader, native, MFVideoFormat_YUY2, 0, 0))) {
			h->pixel = U_STEREO_UVC_PIXEL_YUY2;
		} else {
			hr = MF_E_INVALIDMEDIATYPE;
		}
	}
	if (SUCCEEDED(hr)) {
		hr = h->reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);
	}
	if (SUCCEEDED(hr)) {
		UINT32 w = 0, hh = 0;
		MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &hh);
		h->width = w;
		h->height = hh;
		UINT32 stride = 0;
		if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) {
			h->default_stride = (LONG)stride;
		} else {
			h->default_stride = (LONG)(h->pixel == U_STEREO_UVC_PIXEL_YUY2 ? 2 * w : w);
		}
	}
	if (SUCCEEDED(hr)) {
		bool dec_hw = describe_decoder(h->reader, h->decoder, sizeof(h->decoder));
		if (h->hw && !dec_hw) {
			hr = MF_E_TOPO_CODEC_NOT_FOUND; // a software decoder behind a D3D manager: nothing gained
		}
	}
	u_stereo_uvc_mode nm = {0, 0, 0.0f};
	if (native != nullptr) {
		type_mode(native, &nm, nullptr);
	}
	if (SUCCEEDED(hr) && std::strncmp(id, "file:", 5) == 0 && nm.fps > 0.0f) {
		h->cb->start_pacer((int64_t)(1e9 / nm.fps)); // the decode bench: camera-like pacing
	}
	if (SUCCEEDED(hr)) {
		EnterCriticalSection(&h->cb->lock_);
		h->cb->reader_ = h->reader;
		LeaveCriticalSection(&h->cb->lock_);
		hr = h->reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr,
		                           nullptr);
	}
	if (SUCCEEDED(hr)) {
		U_LOG_W(
		    "uvc capture (Media Foundation): started %ux%u@%.2f — decoder %s, %s path, "
		    "reader output NV12 %ux%u",
		    nm.width, nm.height, nm.fps, h->decoder, h->hw ? "HARDWARE (GPU decode + scale)" : "SOFTWARE",
		    h->width, h->height);
	}
	safe_release(cur);
	safe_release(native);
	safe_release(attr);
	return hr;
}

//! Tear down everything open_reader built (keeps the COM/MF scope).
void
teardown_reader(mf_handle *h)
{
	unlock_held(h);
	if (h->cb != nullptr) {
		EnterCriticalSection(&h->cb->lock_);
		h->cb->stopping_ = true;
		LeaveCriticalSection(&h->cb->lock_);
		h->cb->stop_pacer();
	}
	if (h->reader != nullptr) {
		// Drain the outstanding ReadSample before the reader goes away.
		if (h->cb != nullptr && h->cb->reader_ != nullptr &&
		    SUCCEEDED(h->reader->Flush((DWORD)MF_SOURCE_READER_ALL_STREAMS))) {
			WaitForSingleObject(h->cb->flushed_, 1000);
		}
		if (h->cb != nullptr) {
			EnterCriticalSection(&h->cb->lock_);
			h->cb->reader_ = nullptr;
			LeaveCriticalSection(&h->cb->lock_);
		}
		safe_release(h->reader);
	}
	if (h->source != nullptr) {
		h->source->Shutdown();
		safe_release(h->source);
	}
	if (h->cb != nullptr) {
		if (h->cb->dropped_ > 0) {
			U_LOG_I("uvc capture (Media Foundation): closed; %llu sample(s) superseded before a read",
			        (unsigned long long)h->cb->dropped_);
		}
		h->cb->Release();
		h->cb = nullptr;
	}
	for (int i = 0; i < mf_handle::RING; i++) {
		safe_release(h->ring_q[i]);
		safe_release(h->ring[i]);
	}
	h->npend = 0;
	safe_release(h->dxgi_mgr);
	safe_release(h->ctx);
	safe_release(h->dev);
	h->hw = false;
}

bool
mf_open(void *ctx, const char *id, const u_stereo_uvc_mode *mode, uint32_t out_w, uint32_t out_h, void **out_handle)
{
	(void)ctx;
	*out_handle = nullptr;
	mf_handle *h = new (std::nothrow) mf_handle();
	if (h == nullptr) {
		return false;
	}
	std::snprintf(h->decoder, sizeof(h->decoder), "?");
	h->thread_id = GetCurrentThreadId();
	if (!h->scope.begin()) {
		mf_close(h);
		return false;
	}
	const decoder_pref pref = read_decoder_pref();
	HRESULT hr = E_FAIL;
	char why[256] = {0};
	if (pref != decoder_pref::software) {
		adapter_choice c = choose_adapter(nullptr, 0);
		if (c.found) {
			hr = open_reader(h, id, mode, c.adapter, out_w, out_h);
			if (FAILED(hr)) {
				std::snprintf(why, sizeof(why),
				              "hardware path on \"%s\" (decoder MFT \"%s\") failed, hr 0x%08lx%s",
				              c.desc, c.decoder, (unsigned long)hr,
				              hr == MF_E_TOPO_CODEC_NOT_FOUND ? " — MF chose a software decoder anyway"
				                                              : "");
				teardown_reader(h);
			} else if (c.why[0] != '\0') {
				U_LOG_W("uvc capture: %s — decoding on \"%s\"", c.why, c.desc);
			}
		} else {
			std::snprintf(why, sizeof(why), "%s", c.why);
		}
		safe_release(c.adapter);
		if (FAILED(hr) && pref == decoder_pref::hardware) {
			U_LOG_W(
			    "uvc capture (Media Foundation): DXR_STEREO_CAMERA_UVC_DECODER=hardware and no "
			    "hardware path (%s) — not opening",
			    why);
			mf_close(h);
			return false;
		}
	} else {
		std::snprintf(why, sizeof(why), "DXR_STEREO_CAMERA_UVC_DECODER=software");
	}
	if (FAILED(hr)) {
		U_LOG_W(
		    "uvc capture (Media Foundation): SOFTWARE decode (%s) — slower and more CPU than the hardware path",
		    why);
		hr = open_reader(h, id, mode, nullptr, out_w, out_h);
	}
	if (FAILED(hr)) {
		U_LOG_W("uvc capture (Media Foundation): open %ux%u@%.1f failed, hr 0x%08lx", mode->width, mode->height,
		        mode->fps, (unsigned long)hr);
		mf_close(h);
		return false;
	}
	*out_handle = h;
	return true;
}

/*!
 * Hardware path readback, PIPELINED over a ring of @ref RING staging textures:
 * this read queues the GPU copy of the frame just decoded into one slot and
 * returns the frame queued by the PREVIOUS read, whose copy has long finished
 * — so the readback overlaps the decode of the next frame instead of waiting
 * for it (a synchronous copy + Map measured 22-25 ms per 4K MJPEG frame on
 * the real webcam: the copy waits behind the decode). Cost: one frame of
 * latency. The very first read only primes the ring.
 * @return 1 = @p out holds a frame (stamped @p out_t), 0 = primed (no frame
 *         yet), -1 = not a DXGI texture / failure (caller locks the buffer).
 */
int
map_slot(mf_handle *h, int take, int64_t t0, u_stereo_uvc_raw_frame *out, int64_t *out_t);

int
read_back_texture(mf_handle *h, IMFMediaBuffer *buf, int64_t t_ns, u_stereo_uvc_raw_frame *out, int64_t *out_t)
{
	IMFDXGIBuffer *db = nullptr;
	if (FAILED(buf->QueryInterface(IID_PPV_ARGS(&db)))) {
		return -1;
	}
	ID3D11Texture2D *tex = nullptr;
	UINT sub = 0;
	bool ok = SUCCEEDED(db->GetResource(IID_PPV_ARGS(&tex))) && SUCCEEDED(db->GetSubresourceIndex(&sub));
	safe_release(db);
	if (!ok) {
		safe_release(tex);
		return -1;
	}
	D3D11_TEXTURE2D_DESC td;
	tex->GetDesc(&td);
	if (td.Format != DXGI_FORMAT_NV12 || td.Width < h->width || td.Height < h->height) {
		safe_release(tex);
		return -1;
	}
	for (int i = 0; i < mf_handle::RING; i++) {
		if (h->ring[i] == nullptr) {
			D3D11_TEXTURE2D_DESC sd = {};
			sd.Width = h->width;
			sd.Height = h->height;
			sd.MipLevels = 1;
			sd.ArraySize = 1;
			sd.Format = DXGI_FORMAT_NV12;
			sd.SampleDesc.Count = 1;
			sd.Usage = D3D11_USAGE_STAGING;
			sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
			if (FAILED(h->dev->CreateTexture2D(&sd, nullptr, &h->ring[i])) ||
			    FAILED(h->dev->CreateQuery(&qd, &h->ring_q[i]))) {
				safe_release(tex);
				return -1;
			}
		}
	}
	const int64_t t0 = os_monotonic_get_ns();
	// Queue this frame's copy into a free slot (the GPU orders it after the
	// decode that wrote the texture; the decoder may reuse the texture after).
	int s = -1;
	for (int c = 0; c < mf_handle::RING && s < 0; c++) {
		bool used = false;
		for (int k = 0; k < h->npend; k++) {
			used |= h->pend[k] == c;
		}
		s = used ? -1 : c;
	}
	if (s < 0) { // cannot happen (at most RING - 1 pending): reset the ring
		h->npend = 0;
		s = 0;
	}
	D3D11_BOX box = {0, 0, 0, h->width, h->height, 1};
	h->ctx->CopySubresourceRegion(h->ring[s], 0, 0, 0, 0, tex, sub, &box);
	safe_release(tex);
	h->ctx->End(h->ring_q[s]);
	h->ctx->Flush();
	h->ring_t[s] = t_ns;
	h->pend[h->npend++] = s;
	// Deliver the NEWEST copy that has already finished — possibly this very
	// frame when the GPU is quick — and drop the older ones (latest wins).
	for (int k = h->npend - 1; k >= 0; k--) {
		if (h->ctx->GetData(h->ring_q[h->pend[k]], nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
			const int take = h->pend[k];
			const int remain = h->npend - 1 - k;
			for (int r = 0; r < remain; r++) {
				h->pend[r] = h->pend[k + 1 + r];
			}
			h->npend = remain;
			return map_slot(h, take, t0, out, out_t);
		}
	}
	// None finished yet. With two copies in flight, wait for the older one (it
	// was queued a frame ago); with one, the frame comes out on a later read.
	if (h->npend >= 2) {
		const int take = h->pend[0];
		for (int r = 0; r + 1 < h->npend; r++) {
			h->pend[r] = h->pend[r + 1];
		}
		h->npend--;
		return map_slot(h, take, t0, out, out_t);
	}
	return 0;
}

/*!
 * Map ring slot @p take (its copy was queued earlier), copy the frame out to
 * h->cpu, Unmap. 1 = frame in @p out, -1 = failure.
 */
int
map_slot(mf_handle *h, int take, int64_t t0, u_stereo_uvc_raw_frame *out, int64_t *out_t)
{
	// The previous copy: normally done long ago. Poll (never a blocking Map:
	// the device is multithread-protected and shared with MF's decoder).
	const int64_t give_up = os_monotonic_get_ns() + 100ll * 1000 * 1000;
	while (h->ctx->GetData(h->ring_q[take], nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_FALSE &&
	       os_monotonic_get_ns() < give_up) {
		Sleep(0);
	}
	D3D11_MAPPED_SUBRESOURCE m = {};
	if (FAILED(h->ctx->Map(h->ring[take], 0, D3D11_MAP_READ, 0, &m))) {
		return -1;
	}
	// Copy out and Unmap at once: no staging texture or decoder sample is held
	// while the frame is consumed.
	const size_t pitch = h->width;
	const size_t need = pitch * h->height * 3 / 2;
	if (h->cpu_size < need) {
		uint8_t *nb = (uint8_t *)std::realloc(h->cpu, need);
		if (nb == nullptr) {
			h->ctx->Unmap(h->ring[take], 0);
			return -1;
		}
		h->cpu = nb;
		h->cpu_size = need;
	}
	const uint8_t *src_uv = (const uint8_t *)m.pData + (size_t)m.RowPitch * h->height; // UV follows Y
	for (uint32_t y = 0; y < h->height; y++) {
		std::memcpy(h->cpu + y * pitch, (const uint8_t *)m.pData + (size_t)y * m.RowPitch, pitch);
	}
	for (uint32_t y = 0; y < h->height / 2; y++) {
		std::memcpy(h->cpu + (h->height + y) * pitch, src_uv + (size_t)y * m.RowPitch, pitch);
	}
	h->ctx->Unmap(h->ring[take], 0);
	h->readback_ns += os_monotonic_get_ns() - t0;
	out->pixel = U_STEREO_UVC_PIXEL_NV12;
	out->width = h->width;
	out->height = h->height;
	out->planes[0] = h->cpu;
	out->pitches[0] = (uint32_t)pitch;
	out->planes[1] = h->cpu + pitch * h->height;
	out->pitches[1] = (uint32_t)pitch;
	*out_t = h->ring_t[take];
	return 1;
}

uint32_t
mf_read(void *handle, int64_t timeout_ns, u_stereo_uvc_raw_frame *out)
{
	mf_handle *h = (mf_handle *)handle;
	unlock_held(h); // the previous frame's planes are released by contract

	IMFSample *sample = nullptr;
	int64_t t_ns = 0;
	bool failed = false;
	// Wait against a DEADLINE: the auto-reset event can be left signalled by an
	// earlier callback whose sample was already taken, so one wait may return
	// with nothing pending — that is not a timeout yet.
	const int64_t deadline = os_monotonic_get_ns() + (timeout_ns > 0 ? timeout_ns : 0);
	for (;;) {
		EnterCriticalSection(&h->cb->lock_);
		bool have = h->cb->pending_ != nullptr || h->cb->failed_;
		LeaveCriticalSection(&h->cb->lock_);
		const int64_t left = deadline - os_monotonic_get_ns();
		if (have || left <= 0) {
			break;
		}
		WaitForSingleObject(h->cb->event_, (DWORD)((left + 999999) / 1000000));
	}
	EnterCriticalSection(&h->cb->lock_);
	sample = h->cb->pending_;
	h->cb->pending_ = nullptr;
	t_ns = h->cb->pending_ns_;
	failed = h->cb->failed_;
	HRESULT fail_hr = h->cb->fail_hr_;
	DWORD fail_flags = h->cb->fail_flags_;
	const unsigned long long delivered = h->cb->delivered_, superseded = h->cb->dropped_;
	LeaveCriticalSection(&h->cb->lock_);
	if (sample == nullptr && h->hw && h->npend > 0) {
		// No newer frame within the timeout (end of a file, a paused camera):
		// deliver the newest one still in the readback ring rather than lose it.
		const int take = h->pend[h->npend - 1];
		h->npend = 0;
		std::memset(out, 0, sizeof(*out));
		int64_t t_take = 0;
		if (map_slot(h, take, os_monotonic_get_ns(), out, &t_take) > 0) {
			out->time_ns = t_take;
			out->time_is_exposure = false;
			h->frames++;
			return U_STEREO_UVC_READ_OK;
		}
	}
	if (sample == nullptr) {
		if (failed) {
			U_LOG_W(
			    "uvc capture (Media Foundation): the device stopped delivering (hr 0x%08lx, reader flags "
			    "0x%lx%s) after %llu frame(s) read; %llu delivered by the reader, %llu superseded",
			    (unsigned long)fail_hr, (unsigned long)fail_flags,
			    (fail_flags != 0xffffffffu && (fail_flags & MF_SOURCE_READERF_ENDOFSTREAM))
			        ? " = end of stream"
				: "",
			    (unsigned long long)h->frames, delivered, superseded);
			return U_STEREO_UVC_READ_ERROR;
		}
		return U_STEREO_UVC_READ_TIMEOUT;
	}

	h->held = sample;
	if (FAILED(sample->GetBufferByIndex(0, &h->held_buf))) {
		unlock_held(h);
		return U_STEREO_UVC_READ_TIMEOUT;
	}
	std::memset(out, 0, sizeof(*out));
	const int rb = h->hw ? read_back_texture(h, h->held_buf, t_ns, out, &t_ns) : -1;
	if (rb == 0) {
		unlock_held(h); // primed: the copy is queued, the sample can go back to the decoder
		return U_STEREO_UVC_READ_TIMEOUT;
	}
	if (rb > 0) {
		unlock_held(h); // the frame is in h->cpu now: give the sample back to the decoder
		out->time_ns = t_ns;
		out->time_is_exposure = false;
		if (++h->frames == 300) {
			U_LOG_W(
			    "uvc capture (Media Foundation): hardware path GPU->CPU readback %.3f ms/frame over the "
			    "first 300 frames (%ux%u NV12)",
			    (double)h->readback_ns / 300.0 * 1e-6, h->width, h->height);
		}
		return U_STEREO_UVC_READ_OK;
	}
	BYTE *base = nullptr;
	LONG pitch = 0;
	DWORD len = 0;
	IMF2DBuffer2 *b2 = nullptr;
	if (SUCCEEDED(h->held_buf->QueryInterface(IID_PPV_ARGS(&b2)))) {
		BYTE *start = nullptr;
		if (SUCCEEDED(b2->Lock2DSize(MF2DBuffer_LockFlags_Read, &base, &pitch, &start, &len))) {
			h->held_2d = b2; // keeps the ref; Unlock2D on release
			b2 = nullptr;
		} else {
			base = nullptr;
		}
		safe_release(b2);
	}
	if (base == nullptr) {
		DWORD max = 0;
		if (FAILED(h->held_buf->Lock(&base, &max, &len))) {
			unlock_held(h);
			return U_STEREO_UVC_READ_TIMEOUT;
		}
		h->held_locked = true;
		pitch = h->default_stride;
	}
	if (pitch <= 0) { // bottom-up never happens for YUV; refuse rather than mis-read
		unlock_held(h);
		return U_STEREO_UVC_READ_TIMEOUT;
	}
	out->pixel = h->pixel;
	out->width = h->width;
	out->height = h->height;
	out->planes[0] = base;
	out->pitches[0] = (uint32_t)pitch;
	if (h->pixel == U_STEREO_UVC_PIXEL_NV12) {
		// The UV plane follows the (possibly row-padded, e.g. 1088 for 1080) Y plane.
		uint32_t rows = (uint32_t)(len / (DWORD)pitch) * 2 / 3;
		if (rows < h->height) {
			unlock_held(h);
			return U_STEREO_UVC_READ_TIMEOUT;
		}
		out->planes[1] = base + (size_t)rows * (size_t)pitch;
		out->pitches[1] = (uint32_t)pitch;
	} else if (len < (DWORD)pitch * h->height) {
		unlock_held(h);
		return U_STEREO_UVC_READ_TIMEOUT;
	}
	out->time_ns = t_ns;
	out->time_is_exposure = false;
	h->frames++;
	return U_STEREO_UVC_READ_OK;
}

void
mf_close(void *handle)
{
	mf_handle *h = (mf_handle *)handle;
	if (h == nullptr) {
		return;
	}
	if (h->frames > 0) {
		U_LOG_W("uvc capture (Media Foundation): closed after %llu frame(s) — decoder %s%s",
		        (unsigned long long)h->frames, h->decoder, h->hw ? "" : " (software path)");
		if (h->hw) {
			U_LOG_W("uvc capture (Media Foundation): mean GPU->CPU readback %.3f ms/frame (%ux%u NV12)",
			        (double)h->readback_ns / (double)h->frames * 1e-6, h->width, h->height);
		}
	}
	teardown_reader(h);
	std::free(h->cpu);
	if (GetCurrentThreadId() == h->thread_id) {
		h->scope.end();
	}
	delete h;
}

} // namespace

extern "C" bool
os_uvc_capture_backend(struct u_stereo_uvc_backend *out)
{
	std::memset(out, 0, sizeof(*out));
	out->name = "media-foundation";
	out->enumerate = mf_enumerate;
	out->list_modes = mf_list_modes;
	out->open = mf_open;
	out->read = mf_read;
	out->close = mf_close;
	return true;
}

extern "C" void
os_uvc_capture_decoder_report(char *out, size_t cap)
{
	if (out == nullptr || cap == 0) {
		return;
	}
	out[0] = '\0';
	mf_scope scope;
	if (!scope.begin()) {
		scope.end();
		appendf(out, cap, "Media Foundation unavailable\n");
		return;
	}
	const decoder_pref pref = read_decoder_pref();
	appendf(out, cap, "decoder policy: %s (DXR_STEREO_CAMERA_UVC_DECODER)\n",
	        pref == decoder_pref::software   ? "software"
	        : pref == decoder_pref::hardware ? "hardware only"
	                                         : "auto (hardware, else software)");
	adapter_choice c = choose_adapter(out, cap);
	if (pref == decoder_pref::software) {
		appendf(out, cap, "=> SOFTWARE decode (forced)\n");
	} else if (c.found) {
		appendf(out, cap,
		        "=> HARDWARE decode on \"%s\" with \"%s\"%s%s (confirmed per open: the service logs the "
		        "decoder MF actually inserted)\n",
		        c.desc, c.decoder, c.why[0] ? "; " : "", c.why);
	} else {
		appendf(out, cap, "=> %s decode: %s\n",
		        pref == decoder_pref::hardware ? "NO (hardware only)" : "SOFTWARE", c.why);
	}
	safe_release(c.adapter);
	scope.end();
}
