// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #1601 — the layered (Texture2DArray) layer shaders, checked without a GPU.
 * @ingroup tests
 *
 * #1601 was that every D3D11 layer draw that takes an `xrt_sub_image` — quad,
 * window-space, cylinder, equirect2 — bound the swapchain's SRV to a shader
 * declaring `Texture2D`. comp_d3d11_swapchain and the D3D11 service both create
 * a WHOLE-ARRAY `Texture2DArray` SRV whenever `arraySize > 1`, so that was a
 * view-dimension mismatch AND it discarded `subImage.imageArrayIndex`: the
 * conformance `Subimage` case drew its bottom row from slice 0 (1-6 again)
 * instead of slice 1 (7-12).
 *
 * The fix is a Texture2DArray pixel-shader variant per layer type, selected on
 * the SWAPCHAIN's array size. This pins the half of that a compiler cannot see:
 *
 *  - that the array variants exist and compile at the model the units ask for;
 *  - that each array variant really declares `Texture2DArray` at t0, and each
 *    plain variant really still declares `Texture2D` — one assertion is
 *    worthless without the other, since a check that passes for both dimensions
 *    could not have failed before the fix;
 *  - that the `array_params` slot the draw sites memcpy into sits at the SAME
 *    byte offset in the HLSL cbuffer as in the C++ struct. A mismatch there is
 *    silent: the Map succeeds, the shader reads a neighbouring field as its
 *    slice index, and the picture is subtly wrong rather than absent.
 *
 * What this canNOT check is whether the draw sites actually WRITE array_params
 * and bind the array variant — reflection sees the shader, not its caller. The
 * pixel oracle for that is the conformance `Subimage` case, which is
 * [interactive]. Noted so nobody reads a green here as end-to-end coverage.
 *
 * D3DCompile and D3DReflect need no device, so this runs on the same
 * hardware-free footing as tests_comp_masked_composite_shaders.cpp, whose shape
 * it follows.
 */

#include "d3d11/d3d11_layer_shaders.h"
#include "d3d11_service/d3d11_service_shaders.h"

#include "catch_amalgamated.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>

#include <cstddef>
#include <string>

namespace {

//! Compile one entry point, failing the test with the compiler's own message.
//! @p defines mirrors the optional argument the service's own compile_shader
//! grew for the equirect2 variant.
ID3DBlob *
compile(const char *source, const char *entry, const char *target, const D3D_SHADER_MACRO *defines = nullptr)
{
	ID3DBlob *blob = nullptr;
	ID3DBlob *errors = nullptr;
	HRESULT hr = D3DCompile(source, strlen(source), nullptr, defines, nullptr, entry, target, 0, 0, &blob, &errors);
	if (FAILED(hr)) {
		std::string msg =
		    errors != nullptr ? static_cast<const char *>(errors->GetBufferPointer()) : "no error blob";
		if (errors != nullptr) {
			errors->Release();
		}
		FAIL(entry << " failed to compile: " << msg);
		return nullptr;
	}
	if (errors != nullptr) {
		errors->Release();
	}
	return blob;
}

//! Reflect @p ps and return the SRV dimension its t0 texture declares.
D3D_SRV_DIMENSION
t0_dimension(ID3DBlob *ps)
{
	ID3D11ShaderReflection *refl = nullptr;
	REQUIRE(SUCCEEDED(D3DReflect(ps->GetBufferPointer(), ps->GetBufferSize(), __uuidof(ID3D11ShaderReflection),
	                             reinterpret_cast<void **>(&refl))));
	REQUIRE(refl != nullptr);

	D3D11_SHADER_INPUT_BIND_DESC bd = {};
	REQUIRE(SUCCEEDED(refl->GetResourceBindingDescByName("layer_tex", &bd)));
	CHECK(bd.Type == D3D_SIT_TEXTURE);
	CHECK(bd.BindPoint == 0);
	D3D_SRV_DIMENSION dim = bd.Dimension;
	refl->Release();
	return dim;
}

//! Byte offset of @p field inside the cbuffer @p cb_name of @p ps.
//!
//! NOTE on the null checks below: D3D11 reflection's GetConstantBufferByName /
//! GetVariableByName never return nullptr for a name that does not exist — they
//! return an "invalid" singleton whose GetDesc() then fails. So the REQUIREs on
//! the pointers are not the guard; the REQUIREs on GetDesc are. Written out
//! because a reader who assumes the pointer check is doing the work would
//! "simplify" this into a test that cannot fail.
uint32_t
cbuffer_field_offset(ID3DBlob *ps, const char *cb_name, const char *field)
{
	ID3D11ShaderReflection *refl = nullptr;
	REQUIRE(SUCCEEDED(D3DReflect(ps->GetBufferPointer(), ps->GetBufferSize(), __uuidof(ID3D11ShaderReflection),
	                             reinterpret_cast<void **>(&refl))));
	REQUIRE(refl != nullptr);

	ID3D11ShaderReflectionConstantBuffer *cb = refl->GetConstantBufferByName(cb_name);
	REQUIRE(cb != nullptr);
	ID3D11ShaderReflectionVariable *var = cb->GetVariableByName(field);
	REQUIRE(var != nullptr);
	D3D11_SHADER_VARIABLE_DESC vd = {};
	REQUIRE(SUCCEEDED(var->GetDesc(&vd)));
	uint32_t off = vd.StartOffset;
	refl->Release();
	return off;
}

} // namespace

TEST_CASE("#1601 the array layer shaders compile at ps_5_0")
{
	// ps_5_0 is what every one of these units passes to its own
	// compile_shader; reaching for a later model here would pass a test the
	// runtime cannot reproduce.
	struct
	{
		const char *name;
		const char *source;
	} sources[] = {
	    {"in-process quad_ps_array_source", quad_ps_array_source},
	    {"service quad_ps_array_hlsl", quad_ps_array_hlsl},
	    {"service cylinder_ps_array_hlsl", cylinder_ps_array_hlsl},
	};

	for (const auto &s : sources) {
		INFO(s.name);
		ID3DBlob *ps = compile(s.source, "PSMain", "ps_5_0");
		REQUIRE(ps != nullptr);
		CHECK(ps->GetBufferSize() > 0);
		ps->Release();
	}
}

TEST_CASE("#1601 each array variant samples an array, and each plain variant still does not")
{
	// BOTH halves matter. Asserting only that the array variants are arrayed
	// would also have passed if the plain ones had been converted wholesale,
	// which would break every arraySize==1 quad on the box. Asserting only the
	// plain ones would have passed before the fix. Together they say the two
	// shapes exist and are distinct, which is exactly what the swapchain-shape
	// gate at the draw sites chooses between.
	struct
	{
		const char *name;
		const char *plain;
		const char *arrayed;
	} pairs[] = {
	    {"in-process quad", quad_ps_source, quad_ps_array_source},
	    {"service quad", quad_ps_hlsl, quad_ps_array_hlsl},
	    {"service cylinder", cylinder_ps_hlsl, cylinder_ps_array_hlsl},
	};

	for (const auto &p : pairs) {
		INFO(p.name);

		ID3DBlob *plain = compile(p.plain, "PSMain", "ps_5_0");
		REQUIRE(plain != nullptr);
		CHECK(t0_dimension(plain) == D3D_SRV_DIMENSION_TEXTURE2D);
		plain->Release();

		ID3DBlob *arrayed = compile(p.arrayed, "PSMain", "ps_5_0");
		REQUIRE(arrayed != nullptr);
		CHECK(t0_dimension(arrayed) == D3D_SRV_DIMENSION_TEXTURE2DARRAY);
		arrayed->Release();
	}
}

TEST_CASE("#1601 equirect2's two variants come from ONE source via DXR_LAYERED")
{
	// equirect2 is the one layer whose array variant is NOT a second copy:
	// its body is ~100 lines of sphere intersection and angular-extent math,
	// and a hand-maintained twin would drift. So the same string compiles
	// twice. This pins that the define actually switches the declaration --
	// a typo in the guard would silently yield two identical Texture2D
	// shaders and no error anywhere.
	ID3DBlob *plain = compile(equirect2_ps_hlsl, "PSMain", "ps_5_0");
	REQUIRE(plain != nullptr);
	CHECK(t0_dimension(plain) == D3D_SRV_DIMENSION_TEXTURE2D);
	plain->Release();

	const D3D_SHADER_MACRO layered[] = {{"DXR_LAYERED", "1"}, {nullptr, nullptr}};
	ID3DBlob *arrayed = compile(equirect2_ps_hlsl, "PSMain", "ps_5_0", layered);
	REQUIRE(arrayed != nullptr);
	CHECK(t0_dimension(arrayed) == D3D_SRV_DIMENSION_TEXTURE2DARRAY);
	arrayed->Release();
}

TEST_CASE("#1601 array_params sits where the C++ struct memcpy'd into the cbuffer puts it")
{
	// Every draw site fills a C++ struct and memcpy's the whole thing into a
	// WRITE_DISCARD-mapped cbuffer. Nothing checks that the two layouts agree.
	// If the HLSL declared array_params one float4 earlier or later than the
	// struct does, the Map still succeeds and the shader reads a NEIGHBOURING
	// field as its slice index -- a wrong picture, not a missing one, and no
	// diagnostic at any layer.
	SECTION("in-process LayerConstants")
	{
		ID3DBlob *ps = compile(quad_ps_array_source, "PSMain", "ps_5_0");
		REQUIRE(ps != nullptr);
		CHECK(cbuffer_field_offset(ps, "LayerCB", "array_params") ==
		      static_cast<uint32_t>(offsetof(LayerConstants, array_params)));
		ps->Release();
	}

	SECTION("service QuadLayerConstants")
	{
		ID3DBlob *ps = compile(quad_ps_array_hlsl, "PSMain", "ps_5_0");
		REQUIRE(ps != nullptr);
		CHECK(cbuffer_field_offset(ps, "LayerCB", "array_params") ==
		      static_cast<uint32_t>(offsetof(QuadLayerConstants, array_params)));
		ps->Release();
	}

	SECTION("service CylinderLayerConstants")
	{
		ID3DBlob *ps = compile(cylinder_ps_array_hlsl, "PSMain", "ps_5_0");
		REQUIRE(ps != nullptr);
		CHECK(cbuffer_field_offset(ps, "LayerCB", "array_params") ==
		      static_cast<uint32_t>(offsetof(CylinderLayerConstants, array_params)));
		ps->Release();
	}

	SECTION("service Equirect2LayerConstants")
	{
		const D3D_SHADER_MACRO layered[] = {{"DXR_LAYERED", "1"}, {nullptr, nullptr}};
		ID3DBlob *ps = compile(equirect2_ps_hlsl, "PSMain", "ps_5_0", layered);
		REQUIRE(ps != nullptr);
		CHECK(cbuffer_field_offset(ps, "LayerCB", "array_params") ==
		      static_cast<uint32_t>(offsetof(Equirect2LayerConstants, array_params)));
		ps->Release();
	}
}

TEST_CASE("#1601 the layer constant buffer is sized for the LARGEST layer struct")
{
	// create_layer_resources sizes one shared cbuffer for all three service
	// layer types. It used to hardcode sizeof(Equirect2LayerConstants) with
	// the comment "Largest" -- true when written, enforced by nothing, and
	// growing any other struct past it would have produced a buffer too small
	// for the memcpy that follows. It now takes a max; this is the assertion
	// that max is actually needed, i.e. that the property is about all three
	// and not about equirect2 in particular.
	const size_t largest = sizeof(Equirect2LayerConstants);
	CHECK(sizeof(QuadLayerConstants) <= largest);
	CHECK(sizeof(CylinderLayerConstants) <= largest);

	// And that each one is a whole number of 16-byte cbuffer registers, which
	// is what makes the memcpy of the C++ struct a valid cbuffer image.
	CHECK(sizeof(QuadLayerConstants) % 16 == 0);
	CHECK(sizeof(CylinderLayerConstants) % 16 == 0);
	CHECK(sizeof(Equirect2LayerConstants) % 16 == 0);
}
