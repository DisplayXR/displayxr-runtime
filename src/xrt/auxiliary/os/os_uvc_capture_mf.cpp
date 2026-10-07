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
 *
 * Threading: open / read / close run on the service's camera thread (COM MTA,
 * initialised here and balanced in close); the reader's callback runs on a
 * Media Foundation work-queue thread and only swaps the newest sample in under
 * a lock + signals an auto-reset event. read() waits on that event, takes the
 * newest sample (older ones are simply dropped: latest-wins, like every other
 * camera source) and keeps its buffer LOCKED until the next read / close so
 * the planes it hands out stay valid.
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
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <cstdio>
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

//! The device source for symbolic link @p id (not started).
HRESULT
create_device_source(const char *id, IMFMediaSource **out)
{
	*out = nullptr;
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
		return 1; // decoded by the MJPEG decoder MFT
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
			}
		} else if (sample != nullptr) {
			if (pending_ != nullptr) {
				dropped_++;
				pending_->Release();
			}
			pending_ = sample;
			pending_->AddRef();
			pending_ns_ = now;
		}
		// Keep the pipe full until close. The reader pointer is cleared under
		// this lock before the reader is released, so it is never dangling here.
		if (!failed_ && !stopping_ && reader_ != nullptr) {
			if (FAILED(reader_->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr,
			                               nullptr, nullptr))) {
				failed_ = true;
				fail_hr_ = E_FAIL;
			}
		}
		LeaveCriticalSection(&lock_);
		SetEvent(event_);
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
	uint64_t dropped_ = 0;

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
	// The sample whose buffer read() handed out, locked until the next read / close.
	IMFSample *held = nullptr;
	IMFMediaBuffer *held_buf = nullptr;
	IMF2DBuffer2 *held_2d = nullptr;
};

void
unlock_held(mf_handle *h)
{
	if (h->held_2d != nullptr) {
		h->held_2d->Unlock2D();
		safe_release(h->held_2d);
	} else if (h->held_buf != nullptr) {
		h->held_buf->Unlock();
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

//! Ask the reader for @p subtype output at the native size.
HRESULT
set_output(IMFSourceReader *reader, IMFMediaType *native, const GUID &subtype)
{
	IMFMediaType *out = nullptr;
	HRESULT hr = MFCreateMediaType(&out);
	if (SUCCEEDED(hr)) {
		hr = out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	}
	if (SUCCEEDED(hr)) {
		hr = out->SetGUID(MF_MT_SUBTYPE, subtype);
	}
	UINT32 w = 0, h = 0, num = 0, den = 0;
	if (SUCCEEDED(hr) && SUCCEEDED(MFGetAttributeSize(native, MF_MT_FRAME_SIZE, &w, &h))) {
		hr = MFSetAttributeSize(out, MF_MT_FRAME_SIZE, w, h);
	}
	if (SUCCEEDED(hr) && SUCCEEDED(MFGetAttributeRatio(native, MF_MT_FRAME_RATE, &num, &den))) {
		hr = MFSetAttributeRatio(out, MF_MT_FRAME_RATE, num, den);
	}
	if (SUCCEEDED(hr)) {
		hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, out);
	}
	safe_release(out);
	return hr;
}

void
mf_close(void *handle);

bool
mf_open(void *ctx, const char *id, const u_stereo_uvc_mode *mode, void **out_handle)
{
	(void)ctx;
	*out_handle = nullptr;
	mf_handle *h = new (std::nothrow) mf_handle();
	if (h == nullptr) {
		return false;
	}
	h->thread_id = GetCurrentThreadId();
	if (!h->scope.begin()) {
		mf_close(h);
		return false;
	}
	h->cb = new (std::nothrow) reader_cb();
	IMFAttributes *attr = nullptr;
	IMFMediaType *native = nullptr;
	IMFMediaType *cur = nullptr;
	HRESULT hr = h->cb != nullptr && h->cb->event_ != nullptr ? S_OK : E_OUTOFMEMORY;
	if (SUCCEEDED(hr)) {
		hr = create_device_source(id, &h->source);
	}
	if (SUCCEEDED(hr)) {
		hr = MFCreateAttributes(&attr, 3);
	}
	if (SUCCEEDED(hr)) {
		hr = attr->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, static_cast<IMFSourceReaderCallback *>(h->cb));
	}
	if (SUCCEEDED(hr)) {
		// The reader inserts the MJPEG decoder MFT + colour conversion to NV12.
		hr = attr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
	}
	if (SUCCEEDED(hr)) {
		hr = attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
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
		if (SUCCEEDED(set_output(h->reader, native, MFVideoFormat_NV12))) {
			h->pixel = U_STEREO_UVC_PIXEL_NV12;
		} else if (SUCCEEDED(set_output(h->reader, native, MFVideoFormat_YUY2))) {
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
		EnterCriticalSection(&h->cb->lock_);
		h->cb->reader_ = h->reader;
		LeaveCriticalSection(&h->cb->lock_);
		hr = h->reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr,
		                           nullptr);
	}
	u_stereo_uvc_mode nm = {0, 0, 0.0f};
	if (native != nullptr) {
		type_mode(native, &nm, nullptr);
	}
	safe_release(cur);
	safe_release(native);
	safe_release(attr);
	if (FAILED(hr)) {
		U_LOG_W("uvc capture (Media Foundation): open %ux%u@%.1f failed, hr 0x%08lx", mode->width, mode->height,
		        mode->fps, (unsigned long)hr);
		mf_close(h);
		return false;
	}
	U_LOG_W("uvc capture (Media Foundation): started %ux%u@%.2f, reader output %s %ux%u", nm.width, nm.height,
	        nm.fps, h->pixel == U_STEREO_UVC_PIXEL_NV12 ? "NV12" : "YUY2", h->width, h->height);
	*out_handle = h;
	return true;
}

uint32_t
mf_read(void *handle, int64_t timeout_ns, u_stereo_uvc_raw_frame *out)
{
	mf_handle *h = (mf_handle *)handle;
	unlock_held(h); // the previous frame's planes are released by contract

	DWORD ms = timeout_ns <= 0 ? 0 : (DWORD)((timeout_ns + 999999) / 1000000);
	IMFSample *sample = nullptr;
	int64_t t_ns = 0;
	bool failed = false;
	EnterCriticalSection(&h->cb->lock_);
	bool have = h->cb->pending_ != nullptr || h->cb->failed_;
	LeaveCriticalSection(&h->cb->lock_);
	if (!have) {
		WaitForSingleObject(h->cb->event_, ms);
	}
	EnterCriticalSection(&h->cb->lock_);
	sample = h->cb->pending_;
	h->cb->pending_ = nullptr;
	t_ns = h->cb->pending_ns_;
	failed = h->cb->failed_;
	HRESULT fail_hr = h->cb->fail_hr_;
	LeaveCriticalSection(&h->cb->lock_);
	if (sample == nullptr) {
		if (failed) {
			U_LOG_W("uvc capture (Media Foundation): the device stopped delivering (hr 0x%08lx)",
			        (unsigned long)fail_hr);
			return U_STEREO_UVC_READ_ERROR;
		}
		return U_STEREO_UVC_READ_TIMEOUT;
	}

	h->held = sample;
	if (FAILED(sample->GetBufferByIndex(0, &h->held_buf))) {
		unlock_held(h);
		return U_STEREO_UVC_READ_TIMEOUT;
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
		pitch = h->default_stride;
	}
	if (pitch <= 0) { // bottom-up never happens for YUV; refuse rather than mis-read
		unlock_held(h);
		return U_STEREO_UVC_READ_TIMEOUT;
	}
	std::memset(out, 0, sizeof(*out));
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
	return U_STEREO_UVC_READ_OK;
}

void
mf_close(void *handle)
{
	mf_handle *h = (mf_handle *)handle;
	if (h == nullptr) {
		return;
	}
	unlock_held(h);
	if (h->cb != nullptr) {
		EnterCriticalSection(&h->cb->lock_);
		h->cb->stopping_ = true;
		LeaveCriticalSection(&h->cb->lock_);
	}
	if (h->reader != nullptr) {
		// Drain the outstanding ReadSample before the reader goes away.
		if (h->cb != nullptr && SUCCEEDED(h->reader->Flush((DWORD)MF_SOURCE_READER_ALL_STREAMS))) {
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
