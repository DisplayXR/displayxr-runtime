// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  RAII save/restore of the app's D3D11 immediate-context pipeline state.
 * @ingroup comp_d3d11
 *
 * The in-process D3D11 compositor renders on the APP's ID3D11Device and its
 * immediate context (there is no interop device, that is the whole point of the
 * native compositor). Every draw the compositor issues — atlas composition, the
 * vendor DP weave, the HUD, the swapchain present — leaves pipeline state behind
 * on a context the app is about to keep using.
 *
 * Engines cache that state. Unity's D3D11 backend in particular tracks what it
 * believes is bound and only re-sets on change (GL.InvalidateState() exists
 * precisely because nothing tells it otherwise). A stock-OpenXR Unity title
 * therefore renders its next frame against a stale cache: depth-stencil state
 * it never re-bound (see-through geometry), a vertex buffer / input layout it
 * thinks is still its own (degenerate stretched triangles), shaders and
 * constant buffers it assumes unchanged (shredded text). Every mainstream D3D11
 * OpenXR runtime composites out-of-process or on a separate device, so those
 * engines have never had to defend against a runtime touching their context —
 * this compositor is the one that has to be the good citizen.
 *
 * Usage: construct on the app's context at the top of any compositor entry point
 * that issues GPU work on it (layer_commit, the repaint thread's replay, the
 * window-resize path). Destruction restores what was saved. Restore order puts
 * shader resources back BEFORE render targets so no SRV/RTV hazard fires while
 * unwinding.
 *
 * Deliberately restores only the slot ranges this compositor and a sane
 * middleware weaver could plausibly have touched (16 SRVs / 16 samplers /
 * 14 CBs per stage, 8 vertex buffers, 8 RTVs). Anything the app has bound
 * beyond those ranges was never touched by us and is left alone.
 */

#pragma once

#include <d3d11_1.h>
#include <cstdint>
#include <cstring>

namespace xrt::compositor::d3d11 {

/*!
 * Saves the app-visible pipeline state of a D3D11 immediate context on
 * construction and restores it on destruction.
 */
class app_state_guard
{
public:
	static constexpr UINT kVBs = 8;
	static constexpr UINT kCBs = 14;                              // D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT
	static constexpr UINT kSRVs = 16;                             // covers everything the compositor/weaver bind
	static constexpr UINT kSamplers = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; // 16
	static constexpr UINT kRTVs = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;   // 8
	static constexpr UINT kUAVs = D3D11_PS_CS_UAV_REGISTER_COUNT;           // 8
	static constexpr UINT kSO = D3D11_SO_BUFFER_SLOT_COUNT;                 // 4
	static constexpr UINT kClassInstances = 256;
	static constexpr UINT kViewports = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; // 16

	explicit app_state_guard(ID3D11DeviceContext *ctx) : m_ctx(ctx)
	{
		if (m_ctx == nullptr) {
			return;
		}
		m_ctx->AddRef();

		// IA
		m_ctx->IAGetInputLayout(&ia.layout);
		m_ctx->IAGetPrimitiveTopology(&ia.topology);
		m_ctx->IAGetIndexBuffer(&ia.ib, &ia.ib_format, &ia.ib_offset);
		m_ctx->IAGetVertexBuffers(0, kVBs, ia.vbs, ia.vb_strides, ia.vb_offsets);

		// Shader stages
		vs.get(m_ctx, &ID3D11DeviceContext::VSGetShader, &ID3D11DeviceContext::VSGetConstantBuffers,
		       &ID3D11DeviceContext::VSGetShaderResources, &ID3D11DeviceContext::VSGetSamplers);
		hs.get(m_ctx, &ID3D11DeviceContext::HSGetShader, &ID3D11DeviceContext::HSGetConstantBuffers,
		       &ID3D11DeviceContext::HSGetShaderResources, &ID3D11DeviceContext::HSGetSamplers);
		ds.get(m_ctx, &ID3D11DeviceContext::DSGetShader, &ID3D11DeviceContext::DSGetConstantBuffers,
		       &ID3D11DeviceContext::DSGetShaderResources, &ID3D11DeviceContext::DSGetSamplers);
		gs.get(m_ctx, &ID3D11DeviceContext::GSGetShader, &ID3D11DeviceContext::GSGetConstantBuffers,
		       &ID3D11DeviceContext::GSGetShaderResources, &ID3D11DeviceContext::GSGetSamplers);
		ps.get(m_ctx, &ID3D11DeviceContext::PSGetShader, &ID3D11DeviceContext::PSGetConstantBuffers,
		       &ID3D11DeviceContext::PSGetShaderResources, &ID3D11DeviceContext::PSGetSamplers);
		cs.get(m_ctx, &ID3D11DeviceContext::CSGetShader, &ID3D11DeviceContext::CSGetConstantBuffers,
		       &ID3D11DeviceContext::CSGetShaderResources, &ID3D11DeviceContext::CSGetSamplers);
		m_ctx->CSGetUnorderedAccessViews(0, kUAVs, cs_uavs);

		// RS
		m_ctx->RSGetState(&rs.state);
		rs.num_viewports = kViewports;
		m_ctx->RSGetViewports(&rs.num_viewports, rs.viewports);
		rs.num_scissors = kViewports;
		m_ctx->RSGetScissorRects(&rs.num_scissors, rs.scissors);

		// OM
		m_ctx->OMGetRenderTargets(kRTVs, om.rtvs, &om.dsv);
		m_ctx->OMGetBlendState(&om.blend, om.blend_factor, &om.sample_mask);
		m_ctx->OMGetDepthStencilState(&om.ds_state, &om.stencil_ref);

		// SO + predication
		m_ctx->SOGetTargets(kSO, so_targets);
		m_ctx->GetPredication(&predicate, &predicate_value);

		m_saved = true;
	}

	~app_state_guard()
	{
		if (m_ctx == nullptr) {
			return;
		}
		if (m_saved) {
			restore();
		}
		release_all();
		m_ctx->Release();
	}

	app_state_guard(const app_state_guard &) = delete;
	app_state_guard &operator=(const app_state_guard &) = delete;

private:
	struct stage
	{
		ID3D11DeviceChild *shader = nullptr; // exact type varies per stage
		ID3D11ClassInstance *instances[kClassInstances] = {};
		UINT num_instances = kClassInstances;
		ID3D11Buffer *cbs[kCBs] = {};
		ID3D11ShaderResourceView *srvs[kSRVs] = {};
		ID3D11SamplerState *samplers[kSamplers] = {};

		template <typename ShaderT>
		void
		get(ID3D11DeviceContext *ctx,
		    void (STDMETHODCALLTYPE ID3D11DeviceContext::*get_shader)(ShaderT **, ID3D11ClassInstance **, UINT *),
		    void (STDMETHODCALLTYPE ID3D11DeviceContext::*get_cbs)(UINT, UINT, ID3D11Buffer **),
		    void (STDMETHODCALLTYPE ID3D11DeviceContext::*get_srvs)(UINT, UINT, ID3D11ShaderResourceView **),
		    void (STDMETHODCALLTYPE ID3D11DeviceContext::*get_samplers)(UINT, UINT, ID3D11SamplerState **))
		{
			ShaderT *s = nullptr;
			num_instances = kClassInstances;
			(ctx->*get_shader)(&s, instances, &num_instances);
			shader = s;
			(ctx->*get_cbs)(0, kCBs, cbs);
			(ctx->*get_srvs)(0, kSRVs, srvs);
			(ctx->*get_samplers)(0, kSamplers, samplers);
		}

		template <typename ShaderT>
		void
		set(ID3D11DeviceContext *ctx,
		    void (STDMETHODCALLTYPE ID3D11DeviceContext::*set_shader)(ShaderT *, ID3D11ClassInstance *const *, UINT),
		    void (STDMETHODCALLTYPE ID3D11DeviceContext::*set_cbs)(UINT, UINT, ID3D11Buffer *const *),
		    void (STDMETHODCALLTYPE ID3D11DeviceContext::*set_srvs)(UINT, UINT, ID3D11ShaderResourceView *const *),
		    void (STDMETHODCALLTYPE ID3D11DeviceContext::*set_samplers)(UINT, UINT, ID3D11SamplerState *const *))
		{
			(ctx->*set_srvs)(0, kSRVs, srvs);
			(ctx->*set_samplers)(0, kSamplers, samplers);
			(ctx->*set_cbs)(0, kCBs, cbs);
			(ctx->*set_shader)(static_cast<ShaderT *>(shader), instances, num_instances);
		}

		void
		release()
		{
			if (shader != nullptr) {
				shader->Release();
				shader = nullptr;
			}
			for (UINT i = 0; i < num_instances && i < kClassInstances; i++) {
				if (instances[i] != nullptr) {
					instances[i]->Release();
					instances[i] = nullptr;
				}
			}
			release_array(cbs, kCBs);
			release_array(srvs, kSRVs);
			release_array(samplers, kSamplers);
		}
	};

	template <typename T>
	static void
	release_array(T **arr, UINT n)
	{
		for (UINT i = 0; i < n; i++) {
			if (arr[i] != nullptr) {
				arr[i]->Release();
				arr[i] = nullptr;
			}
		}
	}

	void
	restore()
	{
		// Shader resources first: unbinding OUR SRVs of app textures before we
		// hand the app's render targets back avoids an SRV/RTV hazard on the
		// way out (D3D11 would otherwise force-null the SRV with a warning).
		vs.set(m_ctx, &ID3D11DeviceContext::VSSetShader, &ID3D11DeviceContext::VSSetConstantBuffers,
		       &ID3D11DeviceContext::VSSetShaderResources, &ID3D11DeviceContext::VSSetSamplers);
		hs.set(m_ctx, &ID3D11DeviceContext::HSSetShader, &ID3D11DeviceContext::HSSetConstantBuffers,
		       &ID3D11DeviceContext::HSSetShaderResources, &ID3D11DeviceContext::HSSetSamplers);
		ds.set(m_ctx, &ID3D11DeviceContext::DSSetShader, &ID3D11DeviceContext::DSSetConstantBuffers,
		       &ID3D11DeviceContext::DSSetShaderResources, &ID3D11DeviceContext::DSSetSamplers);
		gs.set(m_ctx, &ID3D11DeviceContext::GSSetShader, &ID3D11DeviceContext::GSSetConstantBuffers,
		       &ID3D11DeviceContext::GSSetShaderResources, &ID3D11DeviceContext::GSSetSamplers);
		ps.set(m_ctx, &ID3D11DeviceContext::PSSetShader, &ID3D11DeviceContext::PSSetConstantBuffers,
		       &ID3D11DeviceContext::PSSetShaderResources, &ID3D11DeviceContext::PSSetSamplers);
		cs.set(m_ctx, &ID3D11DeviceContext::CSSetShader, &ID3D11DeviceContext::CSSetConstantBuffers,
		       &ID3D11DeviceContext::CSSetShaderResources, &ID3D11DeviceContext::CSSetSamplers);
		{
			UINT keep[kUAVs];
			for (UINT i = 0; i < kUAVs; i++) {
				keep[i] = (UINT)-1; // -1 == keep current initial count
			}
			m_ctx->CSSetUnorderedAccessViews(0, kUAVs, cs_uavs, keep);
		}

		// IA
		m_ctx->IASetInputLayout(ia.layout);
		m_ctx->IASetPrimitiveTopology(ia.topology);
		m_ctx->IASetIndexBuffer(ia.ib, ia.ib_format, ia.ib_offset);
		m_ctx->IASetVertexBuffers(0, kVBs, ia.vbs, ia.vb_strides, ia.vb_offsets);

		// RS
		m_ctx->RSSetState(rs.state);
		m_ctx->RSSetViewports(rs.num_viewports, rs.viewports);
		m_ctx->RSSetScissorRects(rs.num_scissors, rs.scissors);

		// OM
		m_ctx->OMSetRenderTargets(kRTVs, om.rtvs, om.dsv);
		m_ctx->OMSetBlendState(om.blend, om.blend_factor, om.sample_mask);
		m_ctx->OMSetDepthStencilState(om.ds_state, om.stencil_ref);

		// SO + predication
		{
			UINT offsets[kSO] = {};
			for (UINT i = 0; i < kSO; i++) {
				offsets[i] = (UINT)-1; // -1 == append; preserves the app's SO write position
			}
			m_ctx->SOSetTargets(kSO, so_targets, offsets);
		}
		m_ctx->SetPredication(predicate, predicate_value);
	}

	void
	release_all()
	{
		if (ia.layout != nullptr) {
			ia.layout->Release();
			ia.layout = nullptr;
		}
		if (ia.ib != nullptr) {
			ia.ib->Release();
			ia.ib = nullptr;
		}
		release_array(ia.vbs, kVBs);
		vs.release();
		hs.release();
		ds.release();
		gs.release();
		ps.release();
		cs.release();
		release_array(cs_uavs, kUAVs);
		if (rs.state != nullptr) {
			rs.state->Release();
			rs.state = nullptr;
		}
		release_array(om.rtvs, kRTVs);
		if (om.dsv != nullptr) {
			om.dsv->Release();
			om.dsv = nullptr;
		}
		if (om.blend != nullptr) {
			om.blend->Release();
			om.blend = nullptr;
		}
		if (om.ds_state != nullptr) {
			om.ds_state->Release();
			om.ds_state = nullptr;
		}
		release_array(so_targets, kSO);
		if (predicate != nullptr) {
			predicate->Release();
			predicate = nullptr;
		}
	}

	ID3D11DeviceContext *m_ctx = nullptr;
	bool m_saved = false;

	struct
	{
		ID3D11InputLayout *layout = nullptr;
		D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		ID3D11Buffer *ib = nullptr;
		DXGI_FORMAT ib_format = DXGI_FORMAT_UNKNOWN;
		UINT ib_offset = 0;
		ID3D11Buffer *vbs[kVBs] = {};
		UINT vb_strides[kVBs] = {};
		UINT vb_offsets[kVBs] = {};
	} ia;

	stage vs, hs, ds, gs, ps, cs;
	ID3D11UnorderedAccessView *cs_uavs[kUAVs] = {};

	struct
	{
		ID3D11RasterizerState *state = nullptr;
		UINT num_viewports = 0;
		D3D11_VIEWPORT viewports[kViewports] = {};
		UINT num_scissors = 0;
		D3D11_RECT scissors[kViewports] = {};
	} rs;

	struct
	{
		ID3D11RenderTargetView *rtvs[kRTVs] = {};
		ID3D11DepthStencilView *dsv = nullptr;
		ID3D11BlendState *blend = nullptr;
		FLOAT blend_factor[4] = {};
		UINT sample_mask = 0xFFFFFFFF;
		ID3D11DepthStencilState *ds_state = nullptr;
		UINT stencil_ref = 0;
	} om;

	ID3D11Buffer *so_targets[kSO] = {};
	ID3D11Predicate *predicate = nullptr;
	BOOL predicate_value = FALSE;
};

} // namespace xrt::compositor::d3d11
