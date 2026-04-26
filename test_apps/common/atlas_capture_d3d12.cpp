// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D12 host-readback path for the 'I' key atlas capture.
 *
 * One-shot allocator/list/fence on the caller's command queue: transition
 * srcTex → COPY_SOURCE, CopyTextureRegion to a readback buffer, transition
 * back, signal fence, wait. Tightly-pack the readback rows (D3D12 requires
 * a 256-byte aligned row pitch) into RGBA, swap BGRA→RGBA if needed, and
 * write a PNG via stb_image_write.
 *
 * Caller passes `entryState` so we can return the resource to whatever
 * state the runtime expects after the capture (typically COMMON or
 * RENDER_TARGET — passed as `int` to keep the header DXGI-free).
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgiformat.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "stb_image_write.h"
#include "atlas_capture.h"

namespace dxr_capture {

namespace {

bool IsBgraFormat(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_B8G8R8A8_UNORM ||
           f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           f == DXGI_FORMAT_B8G8R8A8_TYPELESS;
}

constexpr uint32_t kRowAlign = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;  // 256

uint32_t AlignedRowPitch(uint32_t bytes) {
    return (bytes + kRowAlign - 1) & ~(kRowAlign - 1);
}

}  // namespace

bool CaptureAtlasRegionD3D12(ID3D12Device* device,
                             ID3D12CommandQueue* queue,
                             ID3D12Resource* srcTex,
                             uint32_t srcImageWidth,
                             uint32_t srcImageHeight,
                             int entryState,
                             uint32_t rectX,
                             uint32_t rectY,
                             uint32_t rectW,
                             uint32_t rectH,
                             const std::string& outPath) {
    if (device == nullptr || queue == nullptr || srcTex == nullptr) return false;
    if (rectW == 0 || rectH == 0) return false;
    if ((uint64_t)rectX + rectW > srcImageWidth) return false;
    if ((uint64_t)rectY + rectH > srcImageHeight) return false;

    D3D12_RESOURCE_DESC desc = srcTex->GetDesc();
    DXGI_FORMAT format = desc.Format;

    const uint32_t rowBytes = rectW * 4u;
    const uint32_t rowPitch = AlignedRowPitch(rowBytes);
    const uint64_t bufBytes = (uint64_t)rowPitch * rectH;

    // Readback heap + buffer for the GPU→CPU copy.
    D3D12_HEAP_PROPERTIES rbHeap{};
    rbHeap.Type = D3D12_HEAP_TYPE_READBACK;
    rbHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    rbHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    rbHeap.CreationNodeMask = 1;
    rbHeap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC rbDesc{};
    rbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbDesc.Alignment = 0;
    rbDesc.Width = bufBytes;
    rbDesc.Height = 1;
    rbDesc.DepthOrArraySize = 1;
    rbDesc.MipLevels = 1;
    rbDesc.Format = DXGI_FORMAT_UNKNOWN;
    rbDesc.SampleDesc = {1, 0};
    rbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rbDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ID3D12Resource* readback = nullptr;
    if (FAILED(device->CreateCommittedResource(
            &rbHeap, D3D12_HEAP_FLAG_NONE, &rbDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback)))) {
        return false;
    }

    // One-shot allocator / list on the same queue type (DIRECT — copies are
    // legal on direct queues; matches the caller's render queue).
    ID3D12CommandAllocator* alloc = nullptr;
    if (FAILED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) || !alloc) {
        readback->Release();
        return false;
    }
    ID3D12GraphicsCommandList* list = nullptr;
    if (FAILED(device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
            IID_PPV_ARGS(&list))) || !list) {
        alloc->Release();
        readback->Release();
        return false;
    }

    auto barrier = [](ID3D12Resource* r, D3D12_RESOURCE_STATES before,
                      D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter = after;
        return b;
    };

    const auto entry = static_cast<D3D12_RESOURCE_STATES>(entryState);

    if (entry != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER toCopy = barrier(srcTex, entry, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &toCopy);
    }

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = format;
    dst.PlacedFootprint.Footprint.Width = rectW;
    dst.PlacedFootprint.Footprint.Height = rectH;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = rowPitch;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = srcTex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    D3D12_BOX box;
    box.left = rectX;
    box.top = rectY;
    box.front = 0;
    box.right = rectX + rectW;
    box.bottom = rectY + rectH;
    box.back = 1;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

    if (entry != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        D3D12_RESOURCE_BARRIER toEntry = barrier(srcTex, D3D12_RESOURCE_STATE_COPY_SOURCE, entry);
        list->ResourceBarrier(1, &toEntry);
    }
    list->Close();

    ID3D12CommandList* lists[] = {list};
    queue->ExecuteCommandLists(1, lists);

    // Fence + wait for completion.
    ID3D12Fence* fence = nullptr;
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) || !fence) {
        list->Release();
        alloc->Release();
        readback->Release();
        return false;
    }
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    queue->Signal(fence, 1);
    fence->SetEventOnCompletion(1, evt);
    WaitForSingleObject(evt, INFINITE);
    CloseHandle(evt);
    fence->Release();

    // Map readback buffer and pack rows into a tight RGBA buffer.
    D3D12_RANGE readRange{0, (SIZE_T)bufBytes};
    void* mapped = nullptr;
    if (FAILED(readback->Map(0, &readRange, &mapped)) || !mapped) {
        list->Release();
        alloc->Release();
        readback->Release();
        return false;
    }
    std::vector<uint8_t> rgba((size_t)rowBytes * rectH);
    const uint8_t* mp = static_cast<const uint8_t*>(mapped);
    for (uint32_t y = 0; y < rectH; y++) {
        std::memcpy(rgba.data() + (size_t)y * rowBytes,
                    mp + (size_t)y * rowPitch,
                    rowBytes);
    }
    D3D12_RANGE wroteNothing{0, 0};
    readback->Unmap(0, &wroteNothing);

    list->Release();
    alloc->Release();
    readback->Release();

    if (IsBgraFormat(format)) {
        for (size_t i = 0; i < rgba.size(); i += 4) {
            std::swap(rgba[i + 0], rgba[i + 2]);
        }
    }

    int ok = stbi_write_png(outPath.c_str(), (int)rectW, (int)rectH, 4,
                            rgba.data(), (int)rowBytes);
    return ok != 0;
}

}  // namespace dxr_capture
