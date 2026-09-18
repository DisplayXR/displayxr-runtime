// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #1503 -- the D3D12 swapchain image FORMAT policy.
 *
 * The OpenXR D3D12 binding has the runtime allocate the TYPELESS member of the
 * requested format's family, so the application can build whatever typed view
 * it wants over it. The CTS `Swapchains` test asserts exactly that on the
 * created `ID3D12Resource` (`desc.Format == expectedCreatedImageFormat`), and
 * the table below is transcribed from the CTS's own
 * `GetDxgiSwapchainTestMap()` (`src/conformance/utilities/d3d_common.cpp`) for
 * every format `comp_d3d12_compositor` enumerates.
 *
 * These are pure functions -- no device, no window, no plug-in. The optional
 * final section additionally asks a real (WARP is fine) D3D12 device to create
 * the resource the way `comp_d3d12_swapchain_create` does, because the risk of
 * the promotion is not the mapping but the combination: TYPELESS resource +
 * TYPED clear value + ALLOW_RENDER_TARGET. It self-skips with no device.
 *
 * @ingroup tests
 */

#include "catch_amalgamated.hpp"

#include "d3d/d3d_dxgi_formats.h"

#include <d3d12.h>

namespace {

//! What comp_d3d12_compositor lists in xrt_system_compositor_info::formats.
struct EnumeratedFormat
{
	const char *name;
	DXGI_FORMAT requested; //!< What the app asks for, and what we enumerate.
	DXGI_FORMAT created;   //!< What the CTS requires the resource to BE.
	bool depth;
};

// clang-format off
constexpr EnumeratedFormat kEnumerated[] = {
    {"R8G8B8A8_UNORM",       DXGI_FORMAT_R8G8B8A8_UNORM,       DXGI_FORMAT_R8G8B8A8_TYPELESS,     false},
    {"R8G8B8A8_UNORM_SRGB",  DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,  DXGI_FORMAT_R8G8B8A8_TYPELESS,     false},
    {"B8G8R8A8_UNORM",       DXGI_FORMAT_B8G8R8A8_UNORM,       DXGI_FORMAT_B8G8R8A8_TYPELESS,     false},
    {"B8G8R8A8_UNORM_SRGB",  DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,  DXGI_FORMAT_B8G8R8A8_TYPELESS,     false},
    {"R16G16B16A16_FLOAT",   DXGI_FORMAT_R16G16B16A16_FLOAT,   DXGI_FORMAT_R16G16B16A16_TYPELESS, false},
    {"R16G16B16A16_UNORM",   DXGI_FORMAT_R16G16B16A16_UNORM,   DXGI_FORMAT_R16G16B16A16_TYPELESS, false},
    {"D24_UNORM_S8_UINT",    DXGI_FORMAT_D24_UNORM_S8_UINT,    DXGI_FORMAT_R24G8_TYPELESS,        true },
    {"D32_FLOAT",            DXGI_FORMAT_D32_FLOAT,            DXGI_FORMAT_R32_TYPELESS,          true },
    {"D16_UNORM",            DXGI_FORMAT_D16_UNORM,            DXGI_FORMAT_R16_TYPELESS,          true },
};
// clang-format on

bool
is_typeless(DXGI_FORMAT f)
{
	switch (f) {
	case DXGI_FORMAT_R32G32B32A32_TYPELESS:
	case DXGI_FORMAT_R32G32B32_TYPELESS:
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
	case DXGI_FORMAT_R32G32_TYPELESS:
	case DXGI_FORMAT_R32G8X24_TYPELESS:
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R16G16_TYPELESS:
	case DXGI_FORMAT_R32_TYPELESS:
	case DXGI_FORMAT_R24G8_TYPELESS:
	case DXGI_FORMAT_R8G8_TYPELESS:
	case DXGI_FORMAT_R16_TYPELESS:
	case DXGI_FORMAT_R8_TYPELESS:
	case DXGI_FORMAT_BC1_TYPELESS:
	case DXGI_FORMAT_BC2_TYPELESS:
	case DXGI_FORMAT_BC3_TYPELESS:
	case DXGI_FORMAT_BC4_TYPELESS:
	case DXGI_FORMAT_BC5_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8X8_TYPELESS:
	case DXGI_FORMAT_BC6H_TYPELESS:
	case DXGI_FORMAT_BC7_TYPELESS: return true;
	default: return false;
	}
}

} // namespace


TEST_CASE("d3d12_swapchain_created_format_is_the_typeless_sibling")
{
	for (const auto &f : kEnumerated) {
		INFO("enumerated format " << f.name);
		// This is the single call comp_d3d12_swapchain_create makes to pick
		// the resource format, so this IS the CTS assertion, one step earlier.
		CHECK(d3d_dxgi_format_to_typeless_dxgi(f.requested) == f.created);
		CHECK(is_typeless(d3d_dxgi_format_to_typeless_dxgi(f.requested)));
	}
}

TEST_CASE("d3d12_swapchain_format_promotion_is_idempotent")
{
	// Re-mapping an already-typeless format must not walk to another family:
	// the swapchain path and any later resolve can both run over it.
	for (const auto &f : kEnumerated) {
		INFO("enumerated format " << f.name);
		const DXGI_FORMAT once = d3d_dxgi_format_to_typeless_dxgi(f.requested);
		CHECK(d3d_dxgi_format_to_typeless_dxgi(once) == once);
	}
}

TEST_CASE("d3d12_srv_format_never_stays_typeless")
{
	// A view may not carry a typeless format. Every family the swapchain can
	// now produce has to resolve to something CreateShaderResourceView will
	// accept -- that is the fallback comp_d3d12_swapchain_sample_format() lands
	// on for a resource with no stamp (a runtime scratch, an engine-supplied
	// shared texture).
	for (const auto &f : kEnumerated) {
		INFO("enumerated format " << f.name);
		const DXGI_FORMAT created = d3d_dxgi_format_to_typeless_dxgi(f.requested);
		const DXGI_FORMAT view = d3d_dxgi_format_to_unorm_sample(created);
		CHECK_FALSE(is_typeless(view));
	}

	// Including the partially-typeless depth view names, which ARE legal SRV
	// formats despite their spelling.
	CHECK(d3d_dxgi_typeless_to_typed_dxgi(DXGI_FORMAT_R24G8_TYPELESS) == DXGI_FORMAT_R24_UNORM_X8_TYPELESS);
	CHECK(d3d_dxgi_typeless_to_typed_dxgi(DXGI_FORMAT_R32G8X24_TYPELESS) == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS);

	// Identity for anything already typed -- the resolver is a fallback, never
	// a rewrite.
	for (const auto &f : kEnumerated) {
		INFO("enumerated format " << f.name);
		CHECK(d3d_dxgi_typeless_to_typed_dxgi(f.requested) == f.requested);
	}
}

TEST_CASE("d3d12_srgb_is_preserved_on_the_view_not_the_resource")
{
	// Only the RESOURCE goes typeless. The runtime's OWN sampling view drops
	// sRGB deliberately (the display processor wants display-referred bytes --
	// no implicit sRGB->linear decode); the app's view is the app's business
	// and is built from the typed format it requested, which is exactly what
	// comp_d3d12_swapchain_sample_format() stamps and reads back.
	CHECK(d3d_dxgi_format_to_unorm_sample(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) == DXGI_FORMAT_R8G8B8A8_UNORM);
	CHECK(d3d_dxgi_format_to_unorm_sample(DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) == DXGI_FORMAT_B8G8R8A8_UNORM);

	// The sRGB request and the plain request share one resource format, so the
	// resource alone cannot tell them apart -- and must not have to.
	CHECK(d3d_dxgi_format_to_typeless_dxgi(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ==
	      d3d_dxgi_format_to_typeless_dxgi(DXGI_FORMAT_R8G8B8A8_UNORM));

	// A colour format with no sRGB pair keeps its own type through the
	// resolver: sampling R16G16B16A16_FLOAT as UNORM would misread the bits,
	// which is the whole reason the requested format is stamped rather than
	// guessed from the (shared) typeless resource format.
	CHECK(d3d_dxgi_format_to_unorm_sample(DXGI_FORMAT_R16G16B16A16_FLOAT) == DXGI_FORMAT_R16G16B16A16_FLOAT);
	CHECK(d3d_dxgi_format_to_unorm_sample(DXGI_FORMAT_R16G16B16A16_UNORM) == DXGI_FORMAT_R16G16B16A16_UNORM);
}

TEST_CASE("d3d12_typeless_resource_with_a_typed_clear_value_is_creatable")
{
	// The mapping is arithmetic; the risk is the combination. Ask a real
	// device (WARP counts) for the exact resource comp_d3d12_swapchain_create
	// now asks for. Self-skips where no D3D12 device can be created.
	ID3D12Device *device = nullptr;
	if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) || device == nullptr) {
		WARN("no D3D12 device available -- skipping the device-backed creation check");
		return;
	}

	for (const auto &f : kEnumerated) {
		INFO("enumerated format " << f.name);

		D3D12_HEAP_PROPERTIES heap_props = {};
		heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

		D3D12_RESOURCE_DESC desc = {};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = 8; // the CTS's "very small texture" case
		desc.Height = 8;
		desc.DepthOrArraySize = 2; // and its arraySize == 2 case
		desc.MipLevels = 1;
		desc.Format = d3d_dxgi_format_to_typeless_dxgi(f.requested);
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags =
		    f.depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

		D3D12_CLEAR_VALUE clear = {};
		clear.Format = f.requested; // TYPED, as the runtime passes it
		if (f.depth) {
			clear.DepthStencil.Depth = 1.0f;
		}

		ID3D12Resource *res = nullptr;
		HRESULT hr = device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc,
		                                             f.depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE
		                                                     : D3D12_RESOURCE_STATE_RENDER_TARGET,
		                                             &clear, IID_PPV_ARGS(&res));
		REQUIRE(SUCCEEDED(hr));
		REQUIRE(res != nullptr);

		// What the CTS reads back.
		CHECK(res->GetDesc().Format == f.created);
		res->Release();
	}

	device->Release();
}
