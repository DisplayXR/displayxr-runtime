// Copyright 2024-2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D12 output composite implementation (masked 2D-over-3D pass).
 * @author David Fattal
 * @ingroup comp_d3d12
 *
 * #918 D12-1. The D3D12 twin of comp_d3d11_outcomp, written against the same
 * contract so the D12-3 Stage A that runs the output tail on a native D3D12
 * device on the SCANOUT adapter can be written symmetric to the D3D11 one. The
 * shaders are not a port: both units compile the same strings out of
 * d3d_shared/comp_masked_composite_shaders.h.
 *
 * Everything here is device-scoped and owned by the unit; the device itself is
 * BORROWED (no AddRef/Release), like comp_d3d11_outcomp's. The unit records
 * into the caller's command list and never submits — see the header's file
 * comment for the four places D3D12 forces a decision D3D11 did not.
 */

#include "comp_d3d12_outcomp.h"
#include "d3d_shared/comp_masked_composite_shaders.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>

#include <climits>
#include <cstdlib>
#include <cstring>

/*!
 * Masked 2D-over-3D composite constant buffer (unified-2d-3d-compositing #439,
 * Phase 0). Matches `cbuffer CompositeParams : register(b0)` in
 * d3d_shared/comp_masked_composite_shaders.h — the same 64-byte struct the
 * D3D11 unit maps into a dynamic constant buffer, kept byte-identical here so
 * the two units are diffable field by field.
 *
 * D3D12 passes it as ROOT CONSTANTS rather than a CB: it is 16 DWORDs of
 * per-draw data, which is exactly what root constants are for, and it removes a
 * per-frame Map/Unmap (and with it the D3D11 unit's "a stale CB could write the
 * 2D layer over the canvas" failure mode — there is no buffer to go stale).
 * Root constants are laid out onto the cbuffer's packed DWORDs, so the CPU-side
 * order here IS the mapping — including the trailing pad1[2], which is not
 * CPU-side slack but the HLSL's own tail padding: a cbuffer is rounded up to a
 * 16-byte boundary, so `float2 weave_uv_scale` ending at offset 56 makes the
 * declared buffer 64 bytes. The root signature must cover the whole 64, so this
 * struct's size IS the constant count (tests_comp_masked_composite_shaders
 * pins that against the shader's reflected cbuffer — the relationship is
 * runtime-only otherwise, and a root signature that under-covers the cbuffer
 * fails PSO creation).
 */
struct CompositeParams
{
	float dst_dims[2];       // destination width,height in px
	float canvas_origin[2];  // 3D canvas sub-rect top-left (px)
	float canvas_size[2];    // 3D canvas sub-rect size (px)
	uint32_t use_rect_mask;  // 1 = Phase 0 analytic rect mask
	uint32_t composite_mode; // COMP_D3D12_COMPOSITE_MODE_*: 0 hard M-lerp, 1 #491 over, 2 zones
	uint32_t opaque_present; // #833/#116: 1 = flatten (DWM completes no blends)
	uint32_t pad0;
	/*!
	 * #918 Phase 2a: region / source extent, for the two inputs addressed BY
	 * PIXEL. 1.0 whenever the input is exactly region-sized, which is every
	 * input on the non-split path; the Local2D bridge plane is panel-sized and
	 * the weave scratch is grow-only, so both need the sub-rect scale.
	 *
	 * There is deliberately no MASK scale. An authored mask maps
	 * STRETCH-TO-REGION — the whole mask texture over the whole region — in the
	 * composite and in the DP publish alike, split and non-split alike. That is
	 * the pre-#918 behavior and the mapping the vendor DP applies, so the mask
	 * is sampled at plain uv and a mask whose dims differ from the region is
	 * never cropped.
	 */
	float twod_uv_scale[2];
	float weave_uv_scale[2];
	uint32_t pad1[2];
};
static_assert(sizeof(CompositeParams) == 64, "CompositeParams must match the HLSL cbuffer packing");

//! DWORDs of @ref CompositeParams — the root-signature constant count and the
//! push count, which are the whole struct because the HLSL cbuffer rounds up to
//! exactly the same 64 bytes.
static const UINT kCompositeRootConstants = sizeof(CompositeParams) / 4;

/*!
 * Descriptor sets in the SRV ring. D3D12 reads descriptor TABLES at GPU-execute
 * time, not at record time, so a single 3-slot heap rewritten per call would
 * hand every composite already recorded into an unexecuted list the LAST call's
 * sources. One set per composite in flight avoids that; 4 is a frame of
 * headroom over the one-composite-per-target the output tail actually records,
 * and costs 12 descriptors.
 *
 * The RTV heap rings in lockstep even though RTV handles passed to
 * OMSetRenderTargets are consumed at RECORD time (so one slot would do) —
 * 3 spare descriptors is cheaper than having to re-derive that rule at every
 * later edit.
 */
static const uint32_t kSrvRingSets = 4;
static const uint32_t kSrvPerSet = 3;

/*!
 * #918 Phase 2a — the uv scale for one PIXEL-ADDRESSED input: how much of it the
 * composite region occupies. Reading the extent off the resource rather than
 * taking it as another parameter is deliberate — a caller that hands in a
 * bigger texture gets the right sub-rect automatically, instead of silently
 * stretching it across the pass.
 *
 * ONLY for inputs whose content is a top-left sub-rect of an over-allocated
 * texture: the panel-sized Local2D bridge plane and the grow-only weave
 * scratch. The MASK is NOT such an input — it maps stretch-to-region and must
 * never be run through this.
 */
static void
outcomp_uv_scale(void *res_ptr, uint32_t region_w, uint32_t region_h, float out_scale[2])
{
	out_scale[0] = 1.0f;
	out_scale[1] = 1.0f;
	auto *res = static_cast<ID3D12Resource *>(res_ptr);
	if (res == nullptr || region_w == 0 || region_h == 0) {
		return;
	}
	/*
	 * #918 F2/D3: BIDIRECTIONAL. The earlier one-sided `if (Width > region_w)`
	 * silently stretched an input that was SMALLER than the region across the
	 * whole pass instead of addressing it by pixel — the exact failure the
	 * scale exists to prevent, just in the other direction. Both remaining
	 * callers are >= the region by construction (the Local2D plane is
	 * panel-sized and the region is clamped to the panel; the weave scratch is
	 * grow-only), so a scale > 1 should be unreachable — and if it ever
	 * happens the CLAMP sampler edge-repeats rather than sampling garbage.
	 *
	 * No QueryInterface here, unlike the D3D11 twin: a D3D12 resource carries
	 * its own descriptor, so the extent is one GetDesc away.
	 */
	D3D12_RESOURCE_DESC rd = res->GetDesc();
	if (rd.Width != 0) {
		out_scale[0] = (float)region_w / (float)rd.Width;
	}
	if (rd.Height != 0) {
		out_scale[1] = (float)region_h / (float)rd.Height;
	}
}

/*!
 * The output composite unit.
 */
struct comp_d3d12_outcomp
{
	//! BORROWED device — the caller outlives this unit and owns the reference
	//! (no AddRef/Release here), same contract as comp_d3d11_outcomp. There is
	//! no context/queue/allocator/fence: the unit only ever RECORDS, into the
	//! command list each call is handed.
	ID3D12Device *dev;

	//! SRV table (t0..t2) + the CompositeParams root constants (b0) + a static
	//! POINT sampler (s0). D3D12's answer to the D3D11 unit's separate sampler
	//! object: a static sampler is part of the signature, so it cannot drift.
	ID3D12RootSignature *root_signature;

	/*!
	 * Masked-composite PSOs — D3D12's answer to the D3D11 unit's blend /
	 * rasterizer / depth-stencil state objects, which it bound per draw. All
	 * of that state (no blend: the shader emits the finished pixel; no cull;
	 * no depth) is baked in here, and so is the render-target FORMAT, which
	 * D3D11's format-agnostic RTV let it ignore. Hence one PSO per weave-target
	 * format: DXGI targets are RGBA8, app shared textures in the wild are BGRA8
	 * (cube_texture_d3d12_win). The shader is channel-agnostic, so the two
	 * share bytecode; an _SRGB-typed target matches neither, deliberately (see
	 * the header's sRGB note).
	 */
	ID3D12PipelineState *pso_rgba; // DXGI_FORMAT_R8G8B8A8_UNORM
	ID3D12PipelineState *pso_bgra; // DXGI_FORMAT_B8G8R8A8_UNORM

	//! Shader-visible SRV ring: kSrvRingSets sets of (t0 = 2D layer, t1 =
	//! authored mask, t2 = weave snapshot), written fresh per composite call.
	ID3D12DescriptorHeap *srv_heap;
	//! Non-shader-visible RTV ring for the destination, created per call from
	//! the weave target's own format (D3D11 created a temporary RTV inline).
	ID3D12DescriptorHeap *rtv_heap;
	uint32_t srv_descriptor_size;
	uint32_t rtv_descriptor_size;
	//! Next ring set for both heaps.
	uint32_t ring_next;

	/*!
	 * Sampleable scratch snapshot of the weave target's window region, for the
	 * authored-mask lerp (the mask path reads the weave; the target is the
	 * pass's render target). Lazily allocated window-sized (#464) and then
	 * GROW-ONLY (#918 F10).
	 *
	 * Grow-only, because two callers ask for two sizes in the same frame under
	 * the output-device split: the deposit half runs at the LIVE window region
	 * and the consume half at the region stamped on the egress slot, which
	 * during a resize drag differ by a frame. Reallocating on every mismatch
	 * made them ping-pong — two full texture rebuilds per frame for the whole
	 * drag. An oversized scratch costs nothing instead: the composite takes the
	 * region explicitly and `weave_uv_scale` addresses the sub-rect, so the
	 * pixels are identical either way.
	 *
	 * Its steady state is PIXEL_SHADER_RESOURCE — the state the composite needs
	 * it in. The snapshot round-trips it through COPY_DEST and puts it back, so
	 * no caller ever has to know or track it. (The compositor's own scratches
	 * steady-state at COMMON because their callers do track them; a unit that
	 * hands its scratch out as an opaque `void *` cannot ask that.)
	 */
	ID3D12Resource *weave_scratch;
	//! Allocated extent of @ref weave_scratch (0 when unallocated).
	uint32_t weave_scratch_w, weave_scratch_h;
	//! Grow events, reported once at destroy — the number #918 F10 is about.
	uint64_t weave_scratch_reallocs;
};

#define SAFE_RELEASE(p)                                                                                                \
	do {                                                                                                           \
		if ((p) != nullptr) {                                                                                  \
			(p)->Release();                                                                                \
			(p) = nullptr;                                                                                 \
		}                                                                                                      \
	} while (0)

static HRESULT
outcomp_compile_shader(const char *source, const char *entry, const char *target, ID3DBlob **out_blob)
{
	ID3DBlob *errors = nullptr;
	HRESULT hr =
	    D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, entry, target, 0, 0, out_blob, &errors);
	if (FAILED(hr)) {
		if (errors != nullptr) {
			U_LOG_E("D3D12 outcomp: shader compile error: %s",
			        static_cast<const char *>(errors->GetBufferPointer()));
			errors->Release();
		}
		return hr;
	}
	if (errors != nullptr) {
		errors->Release();
	}
	return hr;
}

//! One transition barrier, elided when the resource is already in the state the
//! step needs — which is what makes the round-trip contract free for a caller
//! that already keeps the target in RENDER_TARGET.
static void
outcomp_transition(ID3D12GraphicsCommandList *cmd_list,
                   ID3D12Resource *res,
                   D3D12_RESOURCE_STATES before,
                   D3D12_RESOURCE_STATES after)
{
	if (res == nullptr || before == after) {
		return;
	}
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = res;
	barrier.Transition.StateBefore = before;
	barrier.Transition.StateAfter = after;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	cmd_list->ResourceBarrier(1, &barrier);
}

// (Re)allocate a DEFAULT-heap committed scratch texture to the given
// dims/format (no-op when it already matches). D3D12 textures are SRV-able
// without bind flags; created directly in PIXEL_SHADER_RESOURCE, this unit's
// steady state for it. Returns false on allocation failure (with *res released
// and nulled).
//
// #918: takes the device explicitly — it moved here with weave_scratch, its
// only consumer, so the snapshot is always allocated on the composite's device.
static bool
d3d12_ensure_srv_scratch(
    ID3D12Device *device, ID3D12Resource **res, uint32_t w, uint32_t h, DXGI_FORMAT fmt, const char *what)
{
	bool need_alloc = *res == nullptr;
	if (!need_alloc) {
		D3D12_RESOURCE_DESC cur = (*res)->GetDesc();
		need_alloc = (cur.Width != w || cur.Height != h || cur.Format != fmt);
	}
	if (!need_alloc) {
		return true;
	}
	if (*res != nullptr) {
		(*res)->Release();
		*res = nullptr;
	}

	D3D12_RESOURCE_DESC desc = {};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = w;
	desc.Height = h;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = fmt;
	desc.SampleDesc.Count = 1;
	desc.Flags = D3D12_RESOURCE_FLAG_NONE;

	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;

	HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
	                                             D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
	                                             __uuidof(ID3D12Resource), reinterpret_cast<void **>(res));
	if (FAILED(hr) || *res == nullptr) {
		U_LOG_W("%s: scratch alloc (%ux%u fmt=%u) failed: 0x%08x", what, w, h, fmt, hr);
		*res = nullptr;
		return false;
	}
	return true;
}

extern "C" xrt_result_t
comp_d3d12_outcomp_create(void *device, struct comp_d3d12_outcomp **out_outcomp)
{
	if (device == nullptr || out_outcomp == nullptr) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	struct comp_d3d12_outcomp *oc = U_TYPED_CALLOC(struct comp_d3d12_outcomp);
	if (oc == nullptr) {
		return XRT_ERROR_ALLOCATION;
	}
	oc->dev = static_cast<ID3D12Device *>(device);

	// --- Root signature: SRV table of 3 (t0 2D / t1 mask / t2 weave snapshot)
	// + the CompositeParams root constants (b0) + a static POINT sampler.
	// Point, because the composite samples region-sized inputs 1:1, so any
	// filtering would be a resampling error, not a smoothing choice; CLAMP so
	// an out-of-range uv edge-repeats rather than sampling garbage. ---
	D3D12_DESCRIPTOR_RANGE srv_range = {};
	srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	srv_range.NumDescriptors = kSrvPerSet;
	srv_range.BaseShaderRegister = 0;
	srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	D3D12_ROOT_PARAMETER root_params[2] = {};
	root_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	root_params[0].DescriptorTable.NumDescriptorRanges = 1;
	root_params[0].DescriptorTable.pDescriptorRanges = &srv_range;
	root_params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
	root_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	root_params[1].Constants.ShaderRegister = 0;
	root_params[1].Constants.RegisterSpace = 0;
	root_params[1].Constants.Num32BitValues = kCompositeRootConstants;
	root_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_STATIC_SAMPLER_DESC sampler = {};
	sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
	sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
	sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
	sampler.MaxLOD = D3D12_FLOAT32_MAX;
	sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

	D3D12_ROOT_SIGNATURE_DESC rs_desc = {};
	rs_desc.NumParameters = 2;
	rs_desc.pParameters = root_params;
	rs_desc.NumStaticSamplers = 1;
	rs_desc.pStaticSamplers = &sampler;
	rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

	ID3DBlob *sig_blob = nullptr;
	ID3DBlob *error_blob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &error_blob);
	if (FAILED(hr)) {
		if (error_blob != nullptr) {
			U_LOG_E("D3D12 outcomp: root signature serialize error: %s",
			        static_cast<const char *>(error_blob->GetBufferPointer()));
			error_blob->Release();
		}
		comp_d3d12_outcomp_destroy(&oc);
		return XRT_ERROR_D3D;
	}
	hr =
	    oc->dev->CreateRootSignature(0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
	                                 __uuidof(ID3D12RootSignature), reinterpret_cast<void **>(&oc->root_signature));
	sig_blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("D3D12 outcomp: failed to create root signature: 0x%08x", hr);
		comp_d3d12_outcomp_destroy(&oc);
		return XRT_ERROR_D3D;
	}

	// --- Masked 2D-over-3D composite shaders (#439 Phase 0), compiled from the
	// same strings the D3D11 unit compiles. ---
	ID3DBlob *vs_blob = nullptr;
	ID3DBlob *ps_blob = nullptr;
	hr = outcomp_compile_shader(masked_composite_vs_source, "VSMain", "vs_5_0", &vs_blob);
	if (FAILED(hr)) {
		U_LOG_E("D3D12 outcomp: failed to compile composite vertex shader");
		comp_d3d12_outcomp_destroy(&oc);
		return XRT_ERROR_D3D;
	}
	hr = outcomp_compile_shader(masked_composite_ps_source, "PSMain", "ps_5_0", &ps_blob);
	if (FAILED(hr)) {
		U_LOG_E("D3D12 outcomp: failed to compile composite pixel shader");
		vs_blob->Release();
		comp_d3d12_outcomp_destroy(&oc);
		return XRT_ERROR_D3D;
	}

	// The D3D11 unit's fixed-function state for this pass, baked in: opaque
	// (BlendEnable FALSE — the shader emits the finished pixel), no cull, no
	// depth. The blend factors are carried over from the D3D11 desc with
	// blending switched off, so the write mask and alpha ops stay what they
	// always were.
	D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
	pso_desc.pRootSignature = oc->root_signature;
	pso_desc.VS.pShaderBytecode = vs_blob->GetBufferPointer();
	pso_desc.VS.BytecodeLength = vs_blob->GetBufferSize();
	pso_desc.PS.pShaderBytecode = ps_blob->GetBufferPointer();
	pso_desc.PS.BytecodeLength = ps_blob->GetBufferSize();
	pso_desc.BlendState.RenderTarget[0].BlendEnable = FALSE;
	pso_desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
	pso_desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	pso_desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
	pso_desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
	pso_desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	pso_desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
	pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	pso_desc.RasterizerState.FrontCounterClockwise = FALSE;
	pso_desc.RasterizerState.DepthClipEnable = TRUE;
	pso_desc.DepthStencilState.DepthEnable = FALSE;
	pso_desc.DepthStencilState.StencilEnable = FALSE;
	pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	pso_desc.NumRenderTargets = 1;
	pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	pso_desc.SampleDesc.Count = 1;
	pso_desc.SampleMask = UINT_MAX;

	hr = oc->dev->CreateGraphicsPipelineState(&pso_desc, __uuidof(ID3D12PipelineState),
	                                          reinterpret_cast<void **>(&oc->pso_rgba));
	if (SUCCEEDED(hr)) {
		// BGRA8 variant — same shader/state, only the baked RTV format differs.
		pso_desc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
		hr = oc->dev->CreateGraphicsPipelineState(&pso_desc, __uuidof(ID3D12PipelineState),
		                                          reinterpret_cast<void **>(&oc->pso_bgra));
	}
	vs_blob->Release();
	ps_blob->Release();
	if (FAILED(hr)) {
		U_LOG_E("D3D12 outcomp: failed to create composite PSO: 0x%08x", hr);
		comp_d3d12_outcomp_destroy(&oc);
		return XRT_ERROR_D3D;
	}

	// --- Descriptor rings (see kSrvRingSets). ---
	D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc = {};
	srv_heap_desc.NumDescriptors = kSrvRingSets * kSrvPerSet;
	srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	hr = oc->dev->CreateDescriptorHeap(&srv_heap_desc, __uuidof(ID3D12DescriptorHeap),
	                                   reinterpret_cast<void **>(&oc->srv_heap));
	if (FAILED(hr)) {
		U_LOG_E("D3D12 outcomp: failed to create SRV heap: 0x%08x", hr);
		comp_d3d12_outcomp_destroy(&oc);
		return XRT_ERROR_D3D;
	}

	D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
	rtv_heap_desc.NumDescriptors = kSrvRingSets;
	rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	hr = oc->dev->CreateDescriptorHeap(&rtv_heap_desc, __uuidof(ID3D12DescriptorHeap),
	                                   reinterpret_cast<void **>(&oc->rtv_heap));
	if (FAILED(hr)) {
		U_LOG_E("D3D12 outcomp: failed to create RTV heap: 0x%08x", hr);
		comp_d3d12_outcomp_destroy(&oc);
		return XRT_ERROR_D3D;
	}

	oc->srv_descriptor_size = oc->dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	oc->rtv_descriptor_size = oc->dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

	*out_outcomp = oc;
	return XRT_SUCCESS;
}

extern "C" void
comp_d3d12_outcomp_destroy(struct comp_d3d12_outcomp **outcomp_ptr)
{
	if (outcomp_ptr == nullptr || *outcomp_ptr == nullptr) {
		return;
	}
	struct comp_d3d12_outcomp *oc = *outcomp_ptr;

	/*
	 * #918 F10: grow events for the whole session. A resize drag should cost a
	 * handful (one per new high-water mark), never two a frame.
	 *
	 * Behind DXR_XBRIDGE_DIAG with the rest of the #918 evidence, deliberately:
	 * this unit is on the NON-split path too, and the split-off WARN class set is
	 * diffed against the merge base as an acceptance check. A diagnostic for a
	 * split concern must not add a class to a session that has no split.
	 */
	if (oc->weave_scratch_reallocs > 0) {
		const char *diag = getenv("DXR_XBRIDGE_DIAG");
		if (diag != nullptr && *diag == '1') {
			U_LOG_W("D3D12 outcomp: weave scratch grew %llu time(s), final %ux%u (#918 F10)",
			        (unsigned long long)oc->weave_scratch_reallocs, oc->weave_scratch_w,
			        oc->weave_scratch_h);
		}
	}

	SAFE_RELEASE(oc->weave_scratch);
	SAFE_RELEASE(oc->rtv_heap);
	SAFE_RELEASE(oc->srv_heap);
	SAFE_RELEASE(oc->pso_bgra);
	SAFE_RELEASE(oc->pso_rgba);
	SAFE_RELEASE(oc->root_signature);
	// dev is borrowed — never released here.

	free(oc);
	*outcomp_ptr = nullptr;
}

extern "C" bool
comp_d3d12_outcomp_ensure_weave_scratch(struct comp_d3d12_outcomp *outcomp,
                                        uint32_t w,
                                        uint32_t h,
                                        uint32_t dxgi_format)
{
	if (outcomp == nullptr || w == 0 || h == 0) {
		return false;
	}
	/*
	 * #918 F10 — GROW-ONLY within a session. A scratch that already covers the
	 * request is kept: the composite takes the region explicitly and scales the
	 * weave uv into the sub-rect, so an oversized scratch produces identical
	 * pixels. What it buys is that the deposit and consume halves — which under
	 * the split legitimately ask for two different regions in one frame during a
	 * resize — can no longer ping-pong the allocation.
	 *
	 * A FORMAT change is not a grow: the snapshot has to match the target, so
	 * that one still reallocates (it happens on a target rebuild, not per frame).
	 */
	if (outcomp->weave_scratch != nullptr) {
		D3D12_RESOURCE_DESC cur = outcomp->weave_scratch->GetDesc();
		if (cur.Format == static_cast<DXGI_FORMAT>(dxgi_format) && cur.Width >= w && cur.Height >= h) {
			return true;
		}
		// Grow to the high-water mark in BOTH axes so a taller-then-wider
		// sequence costs two allocations, not an alternating series.
		if (cur.Format == static_cast<DXGI_FORMAT>(dxgi_format)) {
			w = (cur.Width > w) ? static_cast<uint32_t>(cur.Width) : w;
			h = (cur.Height > h) ? cur.Height : h;
		}
	}
	if (!d3d12_ensure_srv_scratch(outcomp->dev, &outcomp->weave_scratch, w, h,
	                              static_cast<DXGI_FORMAT>(dxgi_format), "local2d weave")) {
		outcomp->weave_scratch_w = 0;
		outcomp->weave_scratch_h = 0;
		return false;
	}
	outcomp->weave_scratch_w = w;
	outcomp->weave_scratch_h = h;
	outcomp->weave_scratch_reallocs++;
	return true;
}

extern "C" void *
comp_d3d12_outcomp_snapshot_weave(struct comp_d3d12_outcomp *outcomp,
                                  void *cmd_list_ptr,
                                  void *dst_resource,
                                  uint32_t dst_state,
                                  uint32_t region_w,
                                  uint32_t region_h)
{
	if (outcomp == nullptr || outcomp->weave_scratch == nullptr || cmd_list_ptr == nullptr ||
	    dst_resource == nullptr) {
		return nullptr;
	}

	auto *cmd_list = static_cast<ID3D12GraphicsCommandList *>(cmd_list_ptr);
	auto *dst = static_cast<ID3D12Resource *>(dst_resource);

	/*
	 * Snapshot the window region of the weave (the DP wrote dst; a resource
	 * cannot be the pass's render target and one of its SRVs, so the lerp reads
	 * this copy — impl doc §3 step 2).
	 *
	 * #918 F6: clamp the box to min(src, dst). An oversized copy box is not an
	 * error the runtime sees — D3D11 DROPS the call silently, and D3D12 makes it
	 * a debug-layer message on a list that still executes — so an unclamped box
	 * hands the composite whatever the scratch happened to hold.
	 */
	D3D12_RESOURCE_DESC sd = dst->GetDesc();
	uint32_t w = region_w, h = region_h;
	if (w > sd.Width) {
		w = static_cast<uint32_t>(sd.Width);
	}
	if (h > sd.Height) {
		h = sd.Height;
	}
	if (w > outcomp->weave_scratch_w) {
		w = outcomp->weave_scratch_w;
	}
	if (h > outcomp->weave_scratch_h) {
		h = outcomp->weave_scratch_h;
	}
	if (w == 0 || h == 0) {
		return nullptr;
	}

	// Round-trip both ends (header point 2): the caller's dst comes back in
	// dst_state, the unit's scratch comes back sampleable.
	const auto caller_state = static_cast<D3D12_RESOURCE_STATES>(dst_state);
	outcomp_transition(cmd_list, dst, caller_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
	outcomp_transition(cmd_list, outcomp->weave_scratch, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
	                   D3D12_RESOURCE_STATE_COPY_DEST);

	D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
	dst_loc.pResource = outcomp->weave_scratch;
	dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst_loc.SubresourceIndex = 0;
	D3D12_TEXTURE_COPY_LOCATION src_loc = {};
	src_loc.pResource = dst;
	src_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src_loc.SubresourceIndex = 0;
	D3D12_BOX sbox = {0, 0, 0, w, h, 1};
	cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, &sbox);

	outcomp_transition(cmd_list, outcomp->weave_scratch, D3D12_RESOURCE_STATE_COPY_DEST,
	                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	outcomp_transition(cmd_list, dst, D3D12_RESOURCE_STATE_COPY_SOURCE, caller_state);

	return outcomp->weave_scratch;
}

extern "C" xrt_result_t
comp_d3d12_outcomp_composite_2d_masked(struct comp_d3d12_outcomp *outcomp,
                                       void *cmd_list_ptr,
                                       void *dst_resource,
                                       uint32_t dst_state,
                                       void *twod_resource,
                                       void *mask_resource,
                                       void *weave_resource,
                                       uint32_t region_w,
                                       uint32_t region_h,
                                       int32_t cx,
                                       int32_t cy,
                                       uint32_t cw,
                                       uint32_t ch,
                                       uint32_t composite_mode,
                                       bool opaque_present)
{
	if (outcomp == nullptr || cmd_list_ptr == nullptr || dst_resource == nullptr || twod_resource == nullptr) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}
	// The authored-mask path lerps against the weave, so it must be readable.
	if (mask_resource != nullptr && weave_resource == nullptr) {
		return XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	auto *cmd_list = static_cast<ID3D12GraphicsCommandList *>(cmd_list_ptr);
	auto *dst = static_cast<ID3D12Resource *>(dst_resource);
	D3D12_RESOURCE_DESC dd = dst->GetDesc();

	// PSO by target format — D3D12 bakes the RTV format in, where D3D11's RTV
	// was format-agnostic. An _SRGB-typed target lands in `default` on purpose:
	// giving it an _SRGB RTV would encode on write, which the D3D11 twin never
	// does (the DP wants the bytes it was handed).
	ID3D12PipelineState *pso = nullptr;
	switch (dd.Format) {
	case DXGI_FORMAT_R8G8B8A8_UNORM: pso = outcomp->pso_rgba; break;
	case DXGI_FORMAT_B8G8R8A8_UNORM: pso = outcomp->pso_bgra; break;
	default: break;
	}
	if (pso == nullptr) {
		U_LOG_E("composite_2d_masked: no PSO for weave target format %u", (unsigned)dd.Format);
		return XRT_ERROR_D3D;
	}

	const uint32_t set = outcomp->ring_next;
	outcomp->ring_next = (outcomp->ring_next + 1) % kSrvRingSets;

	// RTV on the weave target (which already holds the weave), from its own
	// format — the D3D12 spelling of D3D11's CreateRenderTargetView(dst,
	// nullptr, ...). Created into this call's ring slot.
	D3D12_CPU_DESCRIPTOR_HANDLE rtv = outcomp->rtv_heap->GetCPUDescriptorHandleForHeapStart();
	rtv.ptr += (SIZE_T)outcomp->rtv_descriptor_size * set;
	outcomp->dev->CreateRenderTargetView(dst, nullptr, rtv);

	// t0 = 2D layer, t1 = authored mask (Phase 1), t2 = weave snapshot
	// (Phase 1). On the Phase 0 rect path the shader never samples t1/t2 —
	// but a D3D12 descriptor table may not contain uninitialized slots even
	// when the shader does not read them, so the empty ones get NULL
	// descriptors (D3D11 could simply bind a null SRV pointer). Each source's
	// OWN format is used: no UNORM coercion, no sRGB decode inserted here.
	ID3D12Resource *srcs[kSrvPerSet] = {
	    static_cast<ID3D12Resource *>(twod_resource),
	    static_cast<ID3D12Resource *>(mask_resource),
	    static_cast<ID3D12Resource *>(weave_resource),
	};
	D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu = outcomp->srv_heap->GetCPUDescriptorHandleForHeapStart();
	srv_cpu.ptr += (SIZE_T)outcomp->srv_descriptor_size * kSrvPerSet * set;
	for (uint32_t i = 0; i < kSrvPerSet; i++) {
		D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
		srv_desc.Format = srcs[i] != nullptr ? srcs[i]->GetDesc().Format : DXGI_FORMAT_R8G8B8A8_UNORM;
		srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srv_desc.Texture2D.MipLevels = 1;
		outcomp->dev->CreateShaderResourceView(srcs[i], &srv_desc, srv_cpu);
		srv_cpu.ptr += outcomp->srv_descriptor_size;
	}

	// #464: the composite region is window-sized at the top-left anchor of
	// the (worst-case-allocated) surface; pixels beyond it are never written.
	// The full-screen triangle's uv [0,1] spans the viewport, so region-sized
	// sources sample 1:1. Phase 0 passes region == dst dims (full surface).
	D3D12_VIEWPORT vp = {};
	vp.Width = static_cast<float>(region_w);
	vp.Height = static_cast<float>(region_h);
	vp.MaxDepth = 1.0f;
	D3D12_RECT scissor = {};
	scissor.right = static_cast<LONG>(region_w);
	scissor.bottom = static_cast<LONG>(region_h);

	CompositeParams params = {};
	params.dst_dims[0] = static_cast<float>(region_w);
	params.dst_dims[1] = static_cast<float>(region_h);
	params.canvas_origin[0] = static_cast<float>(cx);
	params.canvas_origin[1] = static_cast<float>(cy);
	params.canvas_size[0] = static_cast<float>(cw);
	params.canvas_size[1] = static_cast<float>(ch);
	// Phase 0: hard rect mask derived from the canvas. Phase 1: sample the
	// authored mask and run the lerp.
	params.use_rect_mask = (mask_resource == nullptr) ? 1 : 0;
	params.composite_mode = composite_mode; // LERP / ALPHA_OVER (#491) / ZONES (ADR-027)
	// #833/#116 — the flatten needs the weave, so it stays off on the rect path.
	params.opaque_present = (mask_resource != nullptr && opaque_present) ? 1 : 0;
	/*
	 * #918 Phase 2a: the Local2D bridge plane is panel-sized and the weave
	 * scratch is grow-only (#918 F10), so both carry the region in a top-left
	 * sub-rect and their uv scales into it. The MASK deliberately gets no
	 * scale — it maps stretch-to-region, so it is sampled at plain uv whatever
	 * its dims are.
	 */
	outcomp_uv_scale(twod_resource, region_w, region_h, params.twod_uv_scale);
	outcomp_uv_scale(weave_resource, region_w, region_h, params.weave_uv_scale);

	// Round-trip dst into RENDER_TARGET for the draw (header point 2). The
	// caller-owned sources are NOT transitioned — the unit cannot know their
	// steady state — and must already be sampleable.
	const auto caller_state = static_cast<D3D12_RESOURCE_STATES>(dst_state);
	outcomp_transition(cmd_list, dst, caller_state, D3D12_RESOURCE_STATE_RENDER_TARGET);

	// Full-screen triangle (3 verts pulled from SV_VertexID), opaque output
	// + point sampling → byte-identical to the strip copies.
	//
	// NOTE: this leaves the unit's heap / root signature / PSO bound —
	// downstream recording re-binds what it needs. There is no D3D11-style
	// unbind of the SRVs and RTV afterwards because there is no equivalent
	// read/write hazard to silence: what orders this draw against the
	// downstream reads of dst (shared-texture readback, capture) is the
	// transition back to the caller's state below, not a bind.
	cmd_list->SetDescriptorHeaps(1, &outcomp->srv_heap);
	cmd_list->SetGraphicsRootSignature(outcomp->root_signature);
	cmd_list->SetPipelineState(pso);
	cmd_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
	cmd_list->RSSetViewports(1, &vp);
	cmd_list->RSSetScissorRects(1, &scissor);
	cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu = outcomp->srv_heap->GetGPUDescriptorHandleForHeapStart();
	srv_gpu.ptr += (UINT64)outcomp->srv_descriptor_size * kSrvPerSet * set;
	cmd_list->SetGraphicsRootDescriptorTable(0, srv_gpu);
	cmd_list->SetGraphicsRoot32BitConstants(1, kCompositeRootConstants, &params, 0);
	cmd_list->DrawInstanced(3, 1, 0, 0);

	outcomp_transition(cmd_list, dst, D3D12_RESOURCE_STATE_RENDER_TARGET, caller_state);

	return XRT_SUCCESS;
}
