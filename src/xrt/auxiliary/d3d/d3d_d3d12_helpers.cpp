// Copyright 2022, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Misc D3D12 helper routines.
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @ingroup aux_d3d
 */

#include "d3d_d3d12_helpers.hpp"
#include "d3d_d3d12_bits.h"

#include "util/u_logging.h"

#include <d3d12.h>
#include <wil/com.h>
#include <wil/result.h>

#include <vector>
#include <stdexcept>

namespace xrt::auxiliary::d3d::d3d12 {

wil::com_ptr<ID3D12Device>
createDevice(const wil::com_ptr<IDXGIAdapter> &adapter, u_logging_level log_level)
{
	if (adapter) {
		U_LOG_IFL_D(log_level, "Adapter provided.");
	}

	wil::com_ptr<ID3D12Device> device;
	THROW_IF_FAILED(
	    D3D12CreateDevice(wil::com_raw_ptr(adapter), D3D_FEATURE_LEVEL_11_1, IID_PPV_ARGS(device.put())));

	return device;
}


HRESULT
createCommandListImageCopy(ID3D12Device &device,
                           ID3D12CommandAllocator &command_allocator,
                           ID3D12Resource &resource_src,
                           ID3D12Resource &resource_dst,
                           D3D12_RESOURCE_STATES src_resource_state,
                           D3D12_RESOURCE_STATES dst_resource_state,
                           wil::com_ptr<ID3D12CommandList> &out_copy_command_list)
{
	wil::com_ptr<ID3D12GraphicsCommandList> copyCommandList;
	RETURN_IF_FAILED(device.CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, &command_allocator, nullptr,
	                                          IID_PPV_ARGS(copyCommandList.put())));

	// Transition images into copy state
	D3D12_RESOURCE_BARRIER preCopyBarriers[2]{};
	preCopyBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	preCopyBarriers[0].Transition.pResource = &resource_src;
	preCopyBarriers[0].Transition.StateBefore = src_resource_state;
	preCopyBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
	preCopyBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	preCopyBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	preCopyBarriers[1].Transition.pResource = &resource_dst;
	preCopyBarriers[1].Transition.StateBefore = dst_resource_state;
	preCopyBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
	preCopyBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	copyCommandList->ResourceBarrier(2, preCopyBarriers);

	// Insert texture copy command
	D3D12_TEXTURE_COPY_LOCATION srcCopyLocation;
	srcCopyLocation.pResource = &resource_src;
	srcCopyLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	srcCopyLocation.SubresourceIndex = 0;

	D3D12_TEXTURE_COPY_LOCATION dstCopyLocation;
	dstCopyLocation.pResource = &resource_dst;
	dstCopyLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dstCopyLocation.SubresourceIndex = 0;

	copyCommandList->CopyTextureRegion(&dstCopyLocation, 0, 0, 0, &srcCopyLocation, nullptr);

	// Transition images back from copy state
	D3D12_RESOURCE_BARRIER postCopyBarriers[2]{};
	postCopyBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	postCopyBarriers[0].Transition.pResource = &resource_src;
	postCopyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
	postCopyBarriers[0].Transition.StateAfter = src_resource_state;
	postCopyBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	postCopyBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	postCopyBarriers[1].Transition.pResource = &resource_dst;
	postCopyBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	postCopyBarriers[1].Transition.StateAfter = dst_resource_state;
	postCopyBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

	copyCommandList->ResourceBarrier(2, postCopyBarriers);

	RETURN_IF_FAILED(copyCommandList->Close());

	out_copy_command_list = std::move(copyCommandList);
	return S_OK;
}


wil::com_ptr<ID3D12Resource>
importImage(ID3D12Device &device, HANDLE h)
{
	wil::com_ptr<ID3D12Resource> tex;

	if (h == nullptr) {
		throw std::logic_error("Cannot import empty handle");
	}
	THROW_IF_FAILED(device.OpenSharedHandle(h, IID_PPV_ARGS(tex.put())));
	return tex;
}


wil::com_ptr<ID3D12Fence1>
importFence(ID3D12Device &device, HANDLE h)
{
	wil::com_ptr<ID3D12Fence1> fence;

	if (h == nullptr) {
		throw std::logic_error("Cannot import empty handle");
	}
	THROW_IF_FAILED(device.OpenSharedHandle(h, IID_PPV_ARGS(fence.put())));
	return fence;
}

} // namespace xrt::auxiliary::d3d::d3d12
