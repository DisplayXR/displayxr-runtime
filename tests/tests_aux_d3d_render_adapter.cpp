// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief ADR-037 §2 render-adapter resolver tests.
 *
 * These run against whatever adapters the box actually has, so they assert the
 * *policy invariants* rather than a particular winner. The one machine-specific
 * question — "does §2's ranking still agree with the DXGI
 * HIGH_PERFORMANCE preference the call sites used to hardcode?" — is REPORTED,
 * not asserted: the whole point of ADR-037 §2 is that DXGI's preference is not
 * our policy, so a box where the two diverge is a finding, not a test failure.
 */
#include "catch_amalgamated.hpp"

#include <xrt/xrt_config_have.h>
#include <d3d/d3d_render_adapter.h>
#include <d3d/d3d_render_adapter.hpp>
#include <util/u_win32_com_guard.hpp>

#include <dxgi1_6.h>
#include <wil/com.h>

#include <iostream>
#include <string.h>

using namespace xrt::auxiliary::d3d;
using namespace xrt::auxiliary::util;

namespace {

uint64_t
pack(LUID luid)
{
	uint64_t packed = 0;
	memcpy(&packed, &luid, sizeof(packed));
	return packed;
}

} // namespace

TEST_CASE("render_adapter_resolves", "[.][needgpu]")
{
	ComGuard comGuard;

	RenderAdapterChoice choice = getRenderAdapter(0, 0, 0, 0, D3D_FEATURE_LEVEL_11_0, U_LOGGING_TRACE);

	// Any Windows box that can run the runtime at all has one usable adapter.
	REQUIRE(choice.adapter != nullptr);

	// Provenance is load-bearing, not decoration (ADR-037 §4 / PR #1023): a
	// placement decision that cannot say which rule produced it cannot be
	// triaged from a bug report.
	REQUIRE(choice.provenance != nullptr);
	CHECK(choice.provenance[0] != '\0');

	DXGI_ADAPTER_DESC desc{};
	REQUIRE(SUCCEEDED(choice.adapter->GetDesc(&desc)));
	CHECK(pack(desc.AdapterLuid) == pack(choice.luid));

	std::wcout << L"[ADR-037] render adapter: '" << desc.Description << L"'" << std::endl;
	std::cout << "[ADR-037] provenance: " << choice.provenance << ", from_env=" << (choice.from_env ? "yes" : "no")
	          << std::endl;
}

TEST_CASE("render_adapter_is_deterministic", "[.][needgpu]")
{
	ComGuard comGuard;

	// The index tiebreak exists so two capability-identical adapters cannot
	// resolve differently between runs. Re-resolving must be a pure function of
	// the machine.
	RenderAdapterChoice a = getRenderAdapter(0, 0, 0, 0, D3D_FEATURE_LEVEL_11_0, U_LOGGING_TRACE);
	RenderAdapterChoice b = getRenderAdapter(0, 0, 0, 0, D3D_FEATURE_LEVEL_11_0, U_LOGGING_TRACE);

	REQUIRE(a.adapter != nullptr);
	REQUIRE(b.adapter != nullptr);
	CHECK(pack(a.luid) == pack(b.luid));
	CHECK(std::string(a.provenance) == std::string(b.provenance));
}

TEST_CASE("render_adapter_is_hardware", "[.][needgpu]")
{
	ComGuard comGuard;

	RenderAdapterChoice choice = getRenderAdapter(0, 0, 0, 0, D3D_FEATURE_LEVEL_11_0, U_LOGGING_TRACE);
	REQUIRE(choice.adapter != nullptr);

	// ADR-037 §2: "a software adapter never does [win render]". Confirm the
	// winner is a real hardware adapter as DXGI itself reports it.
	wil::com_ptr<IDXGIAdapter1> adapter1 = choice.adapter.query<IDXGIAdapter1>();
	REQUIRE(adapter1 != nullptr);
	DXGI_ADAPTER_DESC1 desc1{};
	REQUIRE(SUCCEEDED(adapter1->GetDesc1(&desc1)));
	CHECK((desc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0);
	CHECK((desc1.Flags & DXGI_ADAPTER_FLAG_REMOTE) == 0);
}

TEST_CASE("render_adapter_c_and_cxx_agree", "[.][needgpu]")
{
	ComGuard comGuard;

	RenderAdapterChoice cxx = getRenderAdapter(0, 0, 0, 0, D3D_FEATURE_LEVEL_11_0, U_LOGGING_TRACE);
	REQUIRE(cxx.adapter != nullptr);

	// The C entry point is what the Vulkan side consumes (packed the way
	// VkPhysicalDeviceIDProperties::deviceLUID is). It must be the same answer.
	uint64_t packed = 0;
	const char *provenance = nullptr;
	REQUIRE(d3d_render_adapter_luid(0, 0, 0, 0, &packed, &provenance));
	CHECK(packed == pack(cxx.luid));
	REQUIRE(provenance != nullptr);
	CHECK(std::string(provenance) == std::string(cxx.provenance));
}

TEST_CASE("render_adapter_vs_dxgi_high_performance", "[.][needgpu]")
{
	ComGuard comGuard;

	RenderAdapterChoice choice = getRenderAdapter(0, 0, 0, 0, D3D_FEATURE_LEVEL_11_0, U_LOGGING_TRACE);
	REQUIRE(choice.adapter != nullptr);

	wil::com_ptr<IDXGIFactory6> factory6;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory6), factory6.put_void())) || factory6 == nullptr) {
		WARN("IDXGIFactory6 unavailable — cannot compare against HIGH_PERFORMANCE.");
		return;
	}
	wil::com_ptr<IDXGIAdapter> hp;
	REQUIRE(SUCCEEDED(factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
	                                                       __uuidof(IDXGIAdapter), hp.put_void())));
	DXGI_ADAPTER_DESC hp_desc{};
	REQUIRE(SUCCEEDED(hp->GetDesc(&hp_desc)));

	bool same = pack(hp_desc.AdapterLuid) == pack(choice.luid);
	std::wcout << L"[ADR-037] DXGI HIGH_PERFORMANCE picks: '" << hp_desc.Description << L"'" << std::endl;
	std::cout << "[ADR-037] agreement with the capability ranking: " << (same ? "MATCH" : "DIVERGES") << std::endl;

	// Reported, never asserted — see the file header.
	if (!same) {
		WARN("This box's DXGI HIGH_PERFORMANCE preference disagrees with the ADR-037 ranking.");
	}
}
