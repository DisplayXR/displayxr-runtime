// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  An app-published release fence on the D3D12 queue the app hands the
 *         runtime (#2362; the publishing side is displayxr-unreal#62 / #65).
 * @ingroup comp_d3d12
 *
 * ## The contract (EXPERIMENTAL, same-device only)
 *
 * An engine app that renders on its OWN main queue and hands the runtime a
 * dedicated queue (`XrGraphicsBindingD3D12KHR::queue`, the Unreal plug-in's
 * `RuntimeQueue`) has nothing that orders its render before the runtime's
 * reads: `xrReleaseSwapchainImage` is a CPU call, typically made before the
 * app's GPU work is even submitted. Such an app may publish, as D3D12 private
 * data on THAT queue:
 *
 * | GUID | payload | set by the app |
 * |---|---|---|
 * | @ref COMP_D3D12_APP_RELEASE_FENCE_GUID | `IUnknown *` of an `ID3D12Fence` on the app's device (`SetPrivateDataInterface`; read back with `GetPrivateData`, size `sizeof(IUnknown *)`, AddRef'd) | once, before `xrCreateSession` |
 * | @ref COMP_D3D12_APP_RELEASE_FENCE_VALUE_GUID | `uint64_t`: the value the app's queue signals after its last write to the image being released; 0 = nothing signalled yet | before every `xrReleaseSwapchainImage`, on the releasing thread |
 *
 * The two GUIDs are BYTE-IDENTICAL to the publisher's (displayxr-unreal,
 * `DisplayXRCompositor.cpp`, `DXR_UE_RELEASE_FENCE_GUID` /
 * `DXR_UE_RELEASE_FENCE_VALUE_GUID`; its `Docs/DisplayXR/AtlasHandoff.md`,
 * *Release fence (publish only)*). Change neither side alone.
 *
 * The runtime consumes it on the hub route only (the #1264 reroute, ADR-059),
 * as a GPU-side `ID3D12CommandQueue::Wait` on the queue the fence is
 * published on, in front of the work that reads the app's image (ADR-055).
 * A fence on another device is ignored.
 */

#pragma once

#ifndef __cplusplus
#error "comp_d3d12_app_release_fence.h is C++ only"
#endif

#include <windows.h>
#include <d3d12.h>

#include <stdint.h>

// {0caebe5f-b789-4338-b291-859f0f09a631}
static const GUID COMP_D3D12_APP_RELEASE_FENCE_GUID = {
    0x0caebe5f, 0xb789, 0x4338, {0xb2, 0x91, 0x85, 0x9f, 0x0f, 0x09, 0xa6, 0x31}};

// {8fa02ae8-9d02-431e-ab93-bccd79b601f7}
static const GUID COMP_D3D12_APP_RELEASE_FENCE_VALUE_GUID = {
    0x8fa02ae8, 0x9d02, 0x431e, {0xab, 0x93, 0xbc, 0xcd, 0x79, 0xb6, 0x01, 0xf7}};

/*!
 * Look for a published fence on @p queue. On success @p out_fence holds a
 * reference the caller releases. @p out_reason names why there is none
 * ("none" = not published, "foreign_device" = the fence is not on @p device,
 * "not_a_fence" = the interface is not an `ID3D12Fence`).
 */
static inline bool
comp_d3d12_app_release_fence_query(ID3D12CommandQueue *queue,
                                   ID3D12Device *device,
                                   ID3D12Fence **out_fence,
                                   const char **out_reason)
{
	*out_fence = nullptr;
	*out_reason = "none";
	if (queue == nullptr || device == nullptr) {
		return false;
	}
	IUnknown *unk = nullptr;
	UINT size = sizeof(unk);
	if (FAILED(queue->GetPrivateData(COMP_D3D12_APP_RELEASE_FENCE_GUID, &size, &unk)) || unk == nullptr ||
	    size != sizeof(unk)) {
		if (unk != nullptr) {
			unk->Release();
		}
		return false;
	}
	ID3D12Fence *fence = nullptr;
	HRESULT hr = unk->QueryInterface(__uuidof(ID3D12Fence), reinterpret_cast<void **>(&fence));
	unk->Release();
	if (FAILED(hr) || fence == nullptr) {
		*out_reason = "not_a_fence";
		return false;
	}
	// COM identity: compare the IUnknown of both devices (a fence of another
	// ID3D12Device, even on the same adapter, cannot be waited on by this
	// queue without a shared-handle open).
	ID3D12Device *fence_dev = nullptr;
	IUnknown *a = nullptr;
	IUnknown *b = nullptr;
	bool same = false;
	if (SUCCEEDED(fence->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void **>(&fence_dev))) &&
	    fence_dev != nullptr &&
	    SUCCEEDED(fence_dev->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void **>(&a))) &&
	    SUCCEEDED(device->QueryInterface(__uuidof(IUnknown), reinterpret_cast<void **>(&b)))) {
		same = (a == b);
	}
	if (a != nullptr) {
		a->Release();
	}
	if (b != nullptr) {
		b->Release();
	}
	if (fence_dev != nullptr) {
		fence_dev->Release();
	}
	if (!same) {
		fence->Release();
		*out_reason = "foreign_device";
		return false;
	}
	*out_fence = fence;
	*out_reason = "app";
	return true;
}

//! The value published for the image released most recently (0 when unset).
static inline uint64_t
comp_d3d12_app_release_fence_value(ID3D12CommandQueue *queue)
{
	uint64_t v = 0;
	UINT size = sizeof(v);
	if (queue == nullptr || FAILED(queue->GetPrivateData(COMP_D3D12_APP_RELEASE_FENCE_VALUE_GUID, &size, &v)) ||
	    size != sizeof(v)) {
		return 0;
	}
	return v;
}
