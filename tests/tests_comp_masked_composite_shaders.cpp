// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #918 D12-1 — the shared masked-composite HLSL, checked without a GPU.
 * @ingroup tests
 *
 * comp_d3d11_outcomp and comp_d3d12_outcomp compile the SAME strings out of
 * d3d_shared/comp_masked_composite_shaders.h. That reuse is what makes "the two
 * APIs composite identically" checkable by inspection — but it also means the
 * two units bind the shader's interface in different currencies, and a change
 * to the HLSL can break one while leaving the other compiling:
 *
 *  - D3D11 uploads CompositeParams as a constant buffer, so a new cbuffer field
 *    is caught by its 64-byte static_assert.
 *  - D3D12 pushes it as ROOT CONSTANTS with a count baked into the root
 *    signature (kCompositeRootConstants), and the SRVs through a descriptor
 *    table of a fixed width. Neither is a compile-time relationship with the
 *    HLSL: a field added to the cbuffer, or a fourth texture, would just be
 *    silently under-bound at runtime.
 *
 * So this pins the shader's interface — the part no compiler checks. D3DCompile
 * and D3DReflect need no device, which is why this can run in CI on the same
 * hardware-free footing as the rest of tests/.
 */

#include "d3d_shared/comp_masked_composite_shaders.h"

#include "catch_amalgamated.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>

#include <string>

namespace {

//! Compile one entry point, failing the test with the compiler's own message.
ID3DBlob *
compile(const char *source, const char *entry, const char *target)
{
	ID3DBlob *blob = nullptr;
	ID3DBlob *errors = nullptr;
	HRESULT hr = D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, entry, target, 0, 0, &blob, &errors);
	if (FAILED(hr)) {
		std::string msg =
		    errors != nullptr ? static_cast<const char *>(errors->GetBufferPointer()) : "no error blob";
		if (errors != nullptr) {
			errors->Release();
		}
		FAIL(entry << " (" << target << ") failed to compile: " << msg);
		return nullptr;
	}
	if (errors != nullptr) {
		errors->Release();
	}
	return blob;
}

} // namespace

TEST_CASE("the shared masked-composite shaders compile at the model both units ask for")
{
	// vs_5_0 / ps_5_0 is what comp_d3d11_outcomp and comp_d3d12_outcomp both
	// pass; nothing in here may reach for a later model, or one of the two
	// units stops building its own composite.
	ID3DBlob *vs = compile(masked_composite_vs_source, "VSMain", "vs_5_0");
	REQUIRE(vs != nullptr);
	CHECK(vs->GetBufferSize() > 0);
	vs->Release();

	ID3DBlob *ps = compile(masked_composite_ps_source, "PSMain", "ps_5_0");
	REQUIRE(ps != nullptr);
	CHECK(ps->GetBufferSize() > 0);
	ps->Release();
}

TEST_CASE("the composite pixel shader's binding interface is what both units bind")
{
	ID3DBlob *ps = compile(masked_composite_ps_source, "PSMain", "ps_5_0");
	REQUIRE(ps != nullptr);

	ID3D11ShaderReflection *refl = nullptr;
	REQUIRE(SUCCEEDED(D3DReflect(ps->GetBufferPointer(), ps->GetBufferSize(), __uuidof(ID3D11ShaderReflection),
	                             reinterpret_cast<void **>(&refl))));
	REQUIRE(refl != nullptr);

	SECTION("CompositeParams is 64 bytes / 16 DWORDs at b0")
	{
		// 64 is the size of the CompositeParams struct both units declare —
		// D3D11 static_asserts it and uploads that many bytes; D3D12 pushes
		// sizeof/4 root constants (kCompositeRootConstants) and declares the
		// same count in its root signature, where a count that UNDER-covers the
		// shader's cbuffer fails PSO creation at runtime and nowhere earlier.
		// The struct's trailing pad1[2] is not slack: a cbuffer rounds up to a
		// 16-byte boundary, so weave_uv_scale ending at 56 declares 64.
		ID3D11ShaderReflectionConstantBuffer *cb = refl->GetConstantBufferByName("CompositeParams");
		REQUIRE(cb != nullptr);
		D3D11_SHADER_BUFFER_DESC cbd = {};
		REQUIRE(SUCCEEDED(cb->GetDesc(&cbd)));
		CHECK(cbd.Size == 64);
		CHECK(cbd.Size % 4 == 0);
		CHECK(cbd.Size / 4 == 16);

		D3D11_SHADER_INPUT_BIND_DESC bd = {};
		REQUIRE(SUCCEEDED(refl->GetResourceBindingDescByName("CompositeParams", &bd)));
		CHECK(bd.Type == D3D_SIT_CBUFFER);
		CHECK(bd.BindPoint == 0);
	}

	SECTION("the sources are t0..t2 and the sampler is s0")
	{
		// D3D12 binds these through a descriptor table of a FIXED width
		// (kSrvPerSet = 3, base register 0) plus one static sampler; D3D11
		// binds three SRVs at slot 0 and one sampler at slot 0. A fourth
		// texture, or a register moved, is silently under-bound on D3D12.
		const char *tex[3] = {"twod_tex", "mask_tex", "weave_tex"};
		for (uint32_t i = 0; i < 3; i++) {
			D3D11_SHADER_INPUT_BIND_DESC bd = {};
			INFO("texture " << tex[i]);
			REQUIRE(SUCCEEDED(refl->GetResourceBindingDescByName(tex[i], &bd)));
			CHECK(bd.Type == D3D_SIT_TEXTURE);
			CHECK(bd.BindPoint == i);
			CHECK(bd.BindCount == 1);
		}

		D3D11_SHADER_INPUT_BIND_DESC sd = {};
		REQUIRE(SUCCEEDED(refl->GetResourceBindingDescByName("samp", &sd)));
		CHECK(sd.Type == D3D_SIT_SAMPLER);
		CHECK(sd.BindPoint == 0);

		// Nothing else may be bound: an unnoticed fourth resource is exactly
		// the failure the fixed table width cannot express.
		D3D11_SHADER_DESC desc = {};
		REQUIRE(SUCCEEDED(refl->GetDesc(&desc)));
		CHECK(desc.BoundResources == 5); // 3 textures + 1 sampler + 1 cbuffer
	}

	refl->Release();
	ps->Release();
}
