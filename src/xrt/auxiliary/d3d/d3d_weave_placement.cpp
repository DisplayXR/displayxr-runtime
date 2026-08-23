// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The ONE canonical `weave placement:` line.
 * @ingroup aux_d3d
 */

#include "d3d_weave_placement.h"
#include "d3d_scanout_helpers.hpp"

#include "util/u_logging.h"

#include <dxgi1_6.h>
#include <wil/com.h>

#include <string.h>

namespace {

//! Unpack the Vulkan-style packed LUID back into the Win32 halves.
void
unpack_luid(uint64_t packed, unsigned long *out_high, unsigned long *out_low)
{
	LUID luid = {};
	memcpy(&luid, &packed, sizeof(luid));
	*out_high = (unsigned long)luid.HighPart;
	*out_low = (unsigned long)luid.LowPart;
}

/*!
 * The adapter's marketing name, by LUID.
 *
 * A LUID alone is unreadable in a bug report ("is 00000000:0001b3c2 the iGPU?"),
 * and every caller but D3D11 has only the LUID — D3D12 hands back
 * `GetAdapterLuid()`, Vulkan hands back `deviceLUID`. So the lookup happens here
 * once rather than three times, badly.
 */
bool
adapter_name_by_luid(uint64_t packed, wchar_t *out_name, size_t out_name_chars)
{
	if (packed == 0 || out_name == nullptr || out_name_chars == 0) {
		return false;
	}
	LUID want = {};
	memcpy(&want, &packed, sizeof(want));

	wil::com_ptr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void())) || !factory) {
		return false;
	}
	for (UINT i = 0;; i++) {
		wil::com_ptr<IDXGIAdapter1> adapter;
		if (factory->EnumAdapters1(i, adapter.put()) == DXGI_ERROR_NOT_FOUND || !adapter) {
			break;
		}
		DXGI_ADAPTER_DESC1 d = {};
		if (FAILED(adapter->GetDesc1(&d))) {
			continue;
		}
		if (d.AdapterLuid.LowPart == want.LowPart && d.AdapterLuid.HighPart == want.HighPart) {
			wcsncpy_s(out_name, out_name_chars, d.Description, _TRUNCATE);
			return true;
		}
	}
	return false;
}

} // namespace

extern "C" void
d3d_log_weave_placement(uint64_t render_packed_luid,
                        int32_t panel_screen_left,
                        int32_t panel_screen_top,
                        uint32_t panel_pixel_width,
                        uint32_t panel_pixel_height,
                        bool split_active,
                        const char *short_reason)
{
	if (short_reason == nullptr || short_reason[0] == '\0') {
		short_reason = "unknown";
	}

	wchar_t rname[128] = L"<unknown>";
	const bool render_known = adapter_name_by_luid(render_packed_luid, rname, ARRAYSIZE(rname));
	unsigned long rhi = 0, rlo = 0;
	unpack_luid(render_packed_luid, &rhi, &rlo);

	DXGI_ADAPTER_DESC pdesc = {};
	bool scanout_ok = false;
	{
		wil::com_ptr<IDXGIAdapter> panel = xrt::auxiliary::d3d::getScanoutAdapter(
		    panel_screen_left, panel_screen_top, panel_pixel_width, panel_pixel_height, U_LOGGING_INFO);
		scanout_ok = panel != nullptr && SUCCEEDED(panel->GetDesc(&pdesc));
	}

	/*
	 * The render half of the line is one of two shapes, and only one: either the
	 * adapter is named with its LUID, or it says UNKNOWN. A LUID the caller could
	 * not resolve to a name is still printed — a LUID with no name is a real
	 * diagnostic (a remote / software adapter), an invented name is not.
	 */
	if (!scanout_ok) {
		// Say so — never guess. Without the scanout adapter there is no way to
		// know whether this session crosses adapters at all.
		if (render_packed_luid == 0) {
			U_LOG_W(
			    "weave placement: render=UNKNOWN, panel scanout=UNRESOLVED — cannot tell whether the "
			    "weave crosses adapters (split=0 reason=scanout_unresolvable) (#918)");
		} else {
			U_LOG_W(
			    "weave placement: render='%ls' LUID=%08lx:%08lx, panel scanout=UNRESOLVED — cannot "
			    "tell whether the weave crosses adapters (split=0 reason=scanout_unresolvable) (#918)",
			    rname, rhi, rlo);
		}
		return;
	}

	const unsigned long phi = (unsigned long)pdesc.AdapterLuid.HighPart;
	const unsigned long plo = (unsigned long)pdesc.AdapterLuid.LowPart;

	if (render_packed_luid == 0) {
		// OpenGL: no adapter-identity API exists (ADR-037 §5), so the honest
		// answer is that we do not know which adapter rendered — only that the
		// split is not implemented for this path.
		U_LOG_W(
		    "weave placement: render=UNKNOWN, panel scanout='%ls' LUID=%08lx:%08lx — weave on the RENDER "
		    "adapter; every present crosses adapters to reach scanout if they differ (split=0 reason=%s) "
		    "(#918)",
		    pdesc.Description, phi, plo, short_reason);
		return;
	}

	(void)render_known;

	if (pdesc.AdapterLuid.HighPart == (LONG)rhi && pdesc.AdapterLuid.LowPart == (DWORD)rlo) {
		U_LOG_W(
		    "weave placement: render='%ls' LUID=%08lx:%08lx, panel scanout='%ls' LUID=%08lx:%08lx — render "
		    "and scanout share one adapter; weave is local (split=0 reason=same_adapter) (#918)",
		    rname, rhi, rlo, pdesc.Description, phi, plo);
	} else if (split_active) {
		U_LOG_W(
		    "weave placement: render='%ls' LUID=%08lx:%08lx, panel scanout='%ls' LUID=%08lx:%08lx — "
		    "weave/present on the SCANOUT adapter (split=1) (#918)",
		    rname, rhi, rlo, pdesc.Description, phi, plo);
	} else {
		// ADR-037 §3 rung 2, and the whole reason the reason string exists.
		U_LOG_W(
		    "weave placement: render='%ls' LUID=%08lx:%08lx, panel scanout='%ls' LUID=%08lx:%08lx — weave "
		    "on the RENDER adapter; every present crosses adapters to reach scanout (split=0 reason=%s) "
		    "(#918)",
		    rname, rhi, rlo, pdesc.Description, phi, plo, short_reason);
	}
}
