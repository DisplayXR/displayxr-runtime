// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-061 arm 2: the Windows body of the in-process share tap, the
 *         NT-shared ring the fill arm copies its pre-weave views into (see the
 *         contract in comp_xbridge_share_tap.h).
 * @ingroup comp_xbridge
 */

#include "comp_xbridge_share_tap.h"

#include "xrt/xrt_stereo_camera.h"

#include "util/u_logging.h"
#include "util/u_share_link.h"

#include <d3d11_4.h>
#include <dxgi1_2.h>

#include <wil/com.h>

#include <cstring>
#include <new>

//! Half a commit of slack on the schedule, so commit jitter does not cost a frame.
#define SHARE_TAP_SLACK_NS (8 * 1000 * 1000)

struct comp_xbridge_share_tap
{
	wil::com_ptr<ID3D11Device> device;
	wil::com_ptr<ID3D11DeviceContext> context;
	wil::com_ptr<ID3D11DeviceContext4> context4;
	wil::com_ptr<ID3D11Device5> device5;

	wil::com_ptr<ID3D11Texture2D> slot[XRT_SHARE_LINK_RING_SLOTS];
	wil::com_ptr<ID3D11Fence> fence;
	HANDLE slot_handle[XRT_SHARE_LINK_RING_SLOTS];
	HANDLE fence_handle;
	uint64_t fence_value;

	bool has_ring;
	uint32_t ring_generation;
	uint32_t ring_stream_generation;
	uint32_t ring_format;
	uint32_t content_w, content_h;
	bool ring_flip_y, ring_linear;
	uint32_t next_slot;

	uint32_t content_generation;
	bool last_mono;
	int64_t next_due_ns;

	bool warned_create;
	bool warned_format;
	uint64_t n_frames;
};

//! The UNORM format of a family: slots hold the atlas's bytes untouched, the
//! service reads them raw and is told the encoding. 0 when it has none here.
static DXGI_FORMAT
ring_format_for(DXGI_FORMAT f)
{
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
	case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
	case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	default: return DXGI_FORMAT_UNKNOWN;
	}
}

static void
ring_release(struct comp_xbridge_share_tap *t)
{
	for (uint32_t i = 0; i < XRT_SHARE_LINK_RING_SLOTS; i++) {
		t->slot[i].reset();
		if (t->slot_handle[i] != nullptr) {
			CloseHandle(t->slot_handle[i]);
			t->slot_handle[i] = nullptr;
		}
	}
	t->fence.reset();
	if (t->fence_handle != nullptr) {
		CloseHandle(t->fence_handle);
		t->fence_handle = nullptr;
	}
	t->fence_value = 0;
	t->has_ring = false;
}

static bool
ring_create(struct comp_xbridge_share_tap *t,
            const struct xrt_share_link_poll *poll,
            const struct comp_xbridge_share_tap_frame *frame,
            DXGI_FORMAT format)
{
	ring_release(t);

	D3D11_TEXTURE2D_DESC d = {};
	d.Width = frame->content_width * 2;
	d.Height = frame->content_height;
	d.MipLevels = 1;
	d.ArraySize = 1;
	d.Format = format;
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

	HRESULT hr = S_OK;
	for (uint32_t i = 0; i < XRT_SHARE_LINK_RING_SLOTS && SUCCEEDED(hr); i++) {
		hr = t->device->CreateTexture2D(&d, nullptr, t->slot[i].put());
		if (SUCCEEDED(hr)) {
			wil::com_ptr<IDXGIResource1> res;
			hr = t->slot[i]->QueryInterface(IID_PPV_ARGS(res.put()));
			if (SUCCEEDED(hr)) {
				hr = res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
				                             nullptr, &t->slot_handle[i]);
			}
		}
	}
	if (SUCCEEDED(hr)) {
		hr = t->device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(t->fence.put()));
	}
	if (SUCCEEDED(hr)) {
		hr = t->fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &t->fence_handle);
	}
	if (FAILED(hr)) {
		if (!t->warned_create) {
			t->warned_create = true;
			U_LOG_W("ADR-061 in-process share tap: ring %ux%u format %d failed (0x%08lx)", d.Width, d.Height,
			        (int)format, (unsigned long)hr);
		}
		ring_release(t);
		return false;
	}

	struct xrt_share_link_ring_desc desc = {};
	desc.generation = ++t->ring_generation;
	desc.stream_generation = poll->stream_generation;
	desc.format = (uint32_t)format;
	desc.slot_width = d.Width;
	desc.slot_height = d.Height;
	desc.content_width = frame->content_width;
	desc.content_height = frame->content_height;
	desc.flip_y = frame->flip_y ? 1 : 0;
	desc.encoding_is_linear = frame->encoding_is_linear ? 1 : 0;
	desc.slot_count = XRT_SHARE_LINK_RING_SLOTS;

	xrt_graphics_buffer_handle_t handles[XRT_SHARE_LINK_RING_SLOTS];
	for (uint32_t i = 0; i < XRT_SHARE_LINK_RING_SLOTS; i++) {
		handles[i] = t->slot_handle[i];
	}
	xrt_result_t xret = u_share_link_publish_ring(&desc, handles, XRT_SHARE_LINK_RING_SLOTS, t->fence_handle);
	if (xret != XRT_SUCCESS) {
		U_LOG_W("ADR-061 in-process share tap: publishing ring %u failed (%d)", desc.generation, (int)xret);
		ring_release(t);
		return false;
	}

	t->has_ring = true;
	t->ring_stream_generation = poll->stream_generation;
	t->ring_format = (uint32_t)format;
	t->content_w = frame->content_width;
	t->content_h = frame->content_height;
	t->ring_flip_y = frame->flip_y;
	t->ring_linear = frame->encoding_is_linear;
	t->next_slot = 0;
	U_LOG_W("ADR-061 in-process share tap: ring %u: 3 x %ux%u format %d, stream generation %u, flip %d, linear %d",
	        desc.generation, d.Width, d.Height, (int)format, poll->stream_generation, (int)frame->flip_y,
	        (int)frame->encoding_is_linear);
	return true;
}

extern "C" xrt_result_t
xbridge_share_tap_begin(const struct comp_xbridge_share_tap_info *info, struct comp_xbridge_share_tap **out_tap)
{
	if (out_tap != nullptr) {
		*out_tap = nullptr;
	}
	uint8_t luid[XRT_LUID_SIZE] = {};
	bool luid_valid = false;
	if (info != nullptr && info->luid_valid) {
		std::memcpy(luid, info->luid, XRT_LUID_SIZE);
		luid_valid = true;
	}
	ID3D11Device *device = info != nullptr ? static_cast<ID3D11Device *>(info->device) : nullptr;
	ID3D11DeviceContext *context = info != nullptr ? static_cast<ID3D11DeviceContext *>(info->context) : nullptr;

	if (device != nullptr && !luid_valid) {
		wil::com_ptr<IDXGIDevice> dxgi;
		wil::com_ptr<IDXGIAdapter> adapter;
		DXGI_ADAPTER_DESC ad = {};
		if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(dxgi.put()))) && SUCCEEDED(dxgi->GetAdapter(adapter.put())) &&
		    SUCCEEDED(adapter->GetDesc(&ad))) {
			std::memcpy(luid, &ad.AdapterLuid, XRT_LUID_SIZE);
			luid_valid = true;
		}
	}

	uint32_t reason = info != nullptr ? info->unavailable_reason : (uint32_t)XRT_APP_SHARE_UNAVAILABLE_NO_TAP;
	wil::com_ptr<ID3D11Device5> device5;
	wil::com_ptr<ID3D11DeviceContext4> context4;
	if (reason == 0 && (device == nullptr || context == nullptr ||
	                    FAILED(device->QueryInterface(IID_PPV_ARGS(device5.put()))) ||
	                    FAILED(context->QueryInterface(IID_PPV_ARGS(context4.put()))))) {
		reason = XRT_APP_SHARE_UNAVAILABLE_NO_TAP; // no shared fences on this device
	}
	if (reason != 0) {
		u_share_link_set_tap(false, reason, luid, luid_valid);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}

	auto *t = new (std::nothrow) comp_xbridge_share_tap();
	if (t == nullptr) {
		u_share_link_set_tap(false, XRT_APP_SHARE_UNAVAILABLE_NO_TAP, luid, luid_valid);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	t->device = device;
	t->context = context;
	t->context4 = std::move(context4);
	t->device5 = std::move(device5);
	u_share_link_set_tap(true, 0, luid, luid_valid);
	U_LOG_W("ADR-061 in-process share tap: offered");
	if (out_tap != nullptr) {
		*out_tap = t;
	}
	return XRT_SUCCESS;
}

extern "C" xrt_result_t
xbridge_share_tap_commit(struct comp_xbridge_share_tap *t, const struct comp_xbridge_share_tap_frame *frame)
{
	if (t == nullptr) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	struct xrt_share_link_poll poll = {};
	if (!u_share_link_poll(&poll) || poll.state != XRT_SHARE_LINK_STATE_SHARE_REQUESTED) {
		if (t->has_ring) {
			ring_release(t); // no wait: the service holds its own references
		}
		return XRT_SUCCESS;
	}

	const int64_t now = frame->commit_ns;
	if (now < t->next_due_ns - SHARE_TAP_SLACK_NS) {
		return XRT_SUCCESS;
	}
	if (frame->atlas == nullptr || frame->content_width == 0 || frame->content_height == 0 || frame->tile_columns == 0 ||
	    frame->tile_rows == 0 || frame->view_count == 0) {
		return XRT_SUCCESS;
	}

	ID3D11Texture2D *atlas = static_cast<ID3D11Texture2D *>(frame->atlas);
	D3D11_TEXTURE2D_DESC ad = {};
	atlas->GetDesc(&ad);
	const DXGI_FORMAT format = ring_format_for(ad.Format);
	if (format == DXGI_FORMAT_UNKNOWN) {
		if (!t->warned_format) {
			t->warned_format = true;
			U_LOG_W("ADR-061 in-process share tap: atlas format %d cannot be shared", (int)ad.Format);
		}
		return XRT_SUCCESS;
	}

	if (!t->has_ring || t->ring_stream_generation != poll.stream_generation || t->ring_format != (uint32_t)format ||
	    t->content_w != frame->content_width || t->content_h != frame->content_height ||
	    t->ring_flip_y != frame->flip_y || t->ring_linear != frame->encoding_is_linear) {
		if (!ring_create(t, &poll, frame, format)) {
			return XRT_ERROR_ALLOCATION;
		}
	}

	const bool mono = frame->active_view_count == 1 || frame->view_count == 1;
	uint32_t vl = 0, vr = 0;
	if (!mono) {
		if (frame->view_count > 2) {
			vl = frame->view_count / 2 - 1;
			vr = frame->view_count / 2;
		} else {
			vr = 1;
		}
	}

	const uint32_t sx = frame->tile_stride_x != 0 ? frame->tile_stride_x : frame->atlas_width / frame->tile_columns;
	const uint32_t sy = frame->tile_stride_y != 0 ? frame->tile_stride_y : frame->atlas_height / frame->tile_rows;
	const uint32_t views[2] = {vl, vr};
	D3D11_BOX box[2];
	for (uint32_t e = 0; e < 2; e++) {
		const uint32_t col = views[e] % frame->tile_columns;
		const uint32_t row = views[e] / frame->tile_columns;
		box[e].left = col * sx;
		box[e].top = row * sy;
		box[e].front = 0;
		box[e].right = box[e].left + frame->content_width;
		box[e].bottom = box[e].top + frame->content_height;
		box[e].back = 1;
		if (box[e].right > ad.Width || box[e].bottom > ad.Height) {
			return XRT_SUCCESS; // a layout the atlas does not hold this frame
		}
	}

	// The producer's write has to land first. GPU-side, this thread does not block.
	if (frame->wait_fence != nullptr && frame->wait_fence_value != 0) {
		t->context4->Wait(static_cast<ID3D11Fence *>(frame->wait_fence), frame->wait_fence_value);
	}
	IDXGIKeyedMutex *km = static_cast<IDXGIKeyedMutex *>(frame->keyed_mutex);
	if (km != nullptr && km->AcquireSync(0, 0) != S_OK) {
		return XRT_SUCCESS; // the weave side holds it: skip, the next frame is due at once
	}

	ID3D11Texture2D *dst = t->slot[t->next_slot].get();
	t->context->CopySubresourceRegion(dst, 0, 0, 0, 0, atlas, 0, &box[0]);
	t->context->CopySubresourceRegion(dst, 0, frame->content_width, 0, 0, atlas, 0, &box[1]);
	if (km != nullptr) {
		km->ReleaseSync(0);
	}

	t->fence_value++;
	t->context4->Signal(t->fence.get(), t->fence_value);
	t->context->Flush(); // what lets the service's queue ever see the signal

	if (mono != t->last_mono) {
		t->last_mono = mono;
		t->content_generation++;
	}
	struct xrt_share_link_frame out = {};
	out.generation = t->ring_generation;
	out.slot = t->next_slot;
	out.fence_value = t->fence_value;
	out.commit_ns = frame->commit_ns;
	out.zero_disparity_shift_frac = frame->zero_disparity_shift_frac;
	out.mono = mono ? 1 : 0;
	out.content_generation = t->content_generation;

	struct xrt_share_link_poll reply = {};
	xrt_result_t xret = u_share_link_frame_ready(&out, &reply);
	if (xret != XRT_SUCCESS) {
		ring_release(t); // the link went: the next SHARE_REQUESTED makes a new ring
		return xret;
	}

	t->next_slot = (t->next_slot + 1) % XRT_SHARE_LINK_RING_SLOTS;
	const int64_t period = poll.fps > 0.0f ? (int64_t)(1e9f / poll.fps) : 0;
	// Fell behind (the app stalled): restart the schedule, no catching up.
	t->next_due_ns = t->next_due_ns + period < now ? now + period : t->next_due_ns + period;
	if (t->n_frames++ == 0) {
		U_LOG_W("ADR-061 in-process share tap: first frame: atlas %ux%u format %d, grid %ux%u, views %u (pair %u,%u), "
		        "content %ux%u, stride %ux%u",
		        ad.Width, ad.Height, (int)ad.Format, frame->tile_columns, frame->tile_rows, frame->view_count, vl, vr,
		        frame->content_width, frame->content_height, sx, sy);
	}
	return XRT_SUCCESS;
}

extern "C" void
xbridge_share_tap_end(struct comp_xbridge_share_tap **tap_ptr)
{
	if (tap_ptr == nullptr || *tap_ptr == nullptr) {
		return;
	}
	struct comp_xbridge_share_tap *t = *tap_ptr;
	*tap_ptr = nullptr;
	U_LOG_W("ADR-061 in-process share tap: ended after %llu frames", (unsigned long long)t->n_frames);
	ring_release(t);
	u_share_link_set_tap(false, XRT_APP_SHARE_UNAVAILABLE_NO_TAP, nullptr, false);
	delete t;
}
