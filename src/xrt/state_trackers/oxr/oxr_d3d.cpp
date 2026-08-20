// Copyright 2021-2022, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  D3D 11 and 12 shared routines
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @ingroup oxr_main
 */

#include "util/u_misc.h"
#include "util/u_debug.h"
#include "util/u_logging.h"
#include "d3d/d3d_dxgi_helpers.hpp"
#include "d3d/d3d_scanout_helpers.hpp"

#include "oxr_objects.h"
#include "oxr_logger.h"

#include <dxgi1_6.h>
#include <wil/com.h>
#include <wil/result.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define DEFAULT_CATCH(MSG)                                                                                             \
	catch (wil::ResultException const &e)                                                                          \
	{                                                                                                              \
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, MSG ": %s", e.what());                                 \
	}                                                                                                              \
	catch (std::exception const &e)                                                                                \
	{                                                                                                              \
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, MSG ": %s", e.what());                                 \
	}                                                                                                              \
	catch (...)                                                                                                    \
	{                                                                                                              \
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, MSG);                                                  \
	}

using namespace xrt::auxiliary::d3d;

/*
 * DXR_D3D_FORCE_GPU: external override for the adapter the runtime suggests
 * via xrGetD3D11/12GraphicsRequirementsKHR (which well-behaved clients — and
 * the Unity provider's own-device path — then create their device on). The
 * default suggestion is EnumAdapterByGpuPreference(0, HIGH_PERFORMANCE),
 * i.e. always the discrete GPU on an Optimus box, with no external way to
 * redirect it; a client rendering on the iGPU then diverges from the
 * session device and the cross-adapter eye bridge presents black (#240).
 *
 * Accepted: "scanout", "igpu"/"integrated", "dgpu"/"discrete", or an adapter
 * index. Returns an adapter or nullptr when unset/invalid (normal selection).
 * D3D sibling of the Vulkan-side DXR_VK_FORCE_GPU.
 *
 * "scanout" (#918 Phase 0, closes the #846 gap) resolves dynamically to the
 * adapter that owns the output the 3D panel is scanned out on — the iGPU on a
 * box whose panel hangs off the integrated display controller, the dGPU on a
 * MUX'd laptop or a panel wired to the discrete card. It is NOT an alias for
 * "igpu": the rule is "weave next to scanout", and which silicon that is is a
 * property of the machine, not of the keyword. @p info supplies the panel rect
 * and may be NULL (then it is unresolvable → WARN + normal selection).
 *
 * SUPPORTED CONTRACT (#845): clients (e.g. the Unity plugin's Target GPU
 * setting) build on this name and these semantics — do not rename, drop, or
 * change classification without the deprecation path in
 * docs/reference/adapter-selection.md. The getenv() read means in-process
 * clients must set it via the CRT (_putenv_s), not SetEnvironmentVariableW.
 */
static wil::com_ptr<IDXGIAdapter>
env_forced_d3d_adapter(const struct xrt_system_compositor_info *info)
{
	const char *val = getenv("DXR_D3D_FORCE_GPU");
	if (val == NULL || val[0] == '\0') {
		return nullptr;
	}

	if (strcmp(val, "scanout") == 0) {
		if (info == NULL) {
			U_LOG_W("DXR_D3D_FORCE_GPU=scanout: no display info available — ignoring");
			return nullptr;
		}
		auto adapter = getScanoutAdapter(info->display_screen_left, info->display_screen_top,
		                                 info->display_pixel_width, info->display_pixel_height, U_LOGGING_INFO);
		if (adapter == nullptr) {
			U_LOG_W(
			    "DXR_D3D_FORCE_GPU=scanout: could not resolve the panel's scanout adapter "
			    "(panel %ux%u at %d,%d) — ignoring",
			    info->display_pixel_width, info->display_pixel_height, (int)info->display_screen_left,
			    (int)info->display_screen_top);
			return nullptr;
		}
		DXGI_ADAPTER_DESC sdesc{};
		if (SUCCEEDED(adapter->GetDesc(&sdesc))) {
			U_LOG_W(
			    "DXR_D3D_FORCE_GPU=scanout: suggesting adapter %ls (scans out the panel %ux%u at "
			    "%d,%d)",
			    sdesc.Description, info->display_pixel_width, info->display_pixel_height,
			    (int)info->display_screen_left, (int)info->display_screen_top);
		}
		return adapter;
	}

	bool want_igpu;
	if (strcmp(val, "igpu") == 0 || strcmp(val, "integrated") == 0) {
		want_igpu = true;
	} else if (strcmp(val, "dgpu") == 0 || strcmp(val, "discrete") == 0) {
		want_igpu = false;
	} else if (val[0] >= '0' && val[0] <= '9') {
		auto adapter = getAdapterByIndex((uint16_t)atoi(val), U_LOGGING_INFO);
		if (adapter == nullptr) {
			U_LOG_W("DXR_D3D_FORCE_GPU=%s: no adapter at that index — ignoring", val);
		}
		return adapter;
	} else {
		U_LOG_W("DXR_D3D_FORCE_GPU=%s: unrecognized value — ignoring", val);
		return nullptr;
	}

	// NOT EnumAdapterByGpuPreference: a per-app UserGpuPreferences registry
	// entry overrides the preference ARGUMENT, so with GpuPreference=2 set
	// MINIMUM_POWER still returns the discrete GPU first (observed on HW).
	// Classify by dedicated VRAM instead — the integrated GPU is the
	// hardware adapter with the least of it, the discrete the one with the
	// most — which no registry state can reorder.
	wil::com_ptr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void())) || factory == nullptr) {
		U_LOG_W("DXR_D3D_FORCE_GPU=%s: DXGI factory unavailable — ignoring", val);
		return nullptr;
	}
	wil::com_ptr<IDXGIAdapter1> best;
	DXGI_ADAPTER_DESC1 best_desc{};
	for (UINT i = 0;; i++) {
		wil::com_ptr<IDXGIAdapter1> adapter;
		if (FAILED(factory->EnumAdapters1(i, adapter.put())) || adapter == nullptr) {
			break;
		}
		DXGI_ADAPTER_DESC1 desc{};
		if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
			continue; // skip WARP / Basic Render Driver
		}
		bool better = (best == nullptr) ||
		              (want_igpu ? desc.DedicatedVideoMemory < best_desc.DedicatedVideoMemory
		                         : desc.DedicatedVideoMemory > best_desc.DedicatedVideoMemory);
		if (better) {
			best = adapter;
			best_desc = desc;
		}
	}
	if (best == nullptr) {
		U_LOG_W("DXR_D3D_FORCE_GPU=%s: no hardware adapter found — ignoring", val);
		return nullptr;
	}
	U_LOG_W("DXR_D3D_FORCE_GPU=%s: suggesting adapter %ls (dedicated VRAM %llu MB)", val, best_desc.Description,
	        (unsigned long long)(best_desc.DedicatedVideoMemory / (1024 * 1024)));
	return best.query<IDXGIAdapter>();
}

XrResult
oxr_d3d_get_requirements(struct oxr_logger *log,
                         struct oxr_system *sys,
                         LUID *adapter_luid,
                         D3D_FEATURE_LEVEL *min_feature_level)
{
	if (sys->xsysc == NULL) {
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, " sys->xsysc == NULL");
	}

	try {

		// #1000: track WHICH adapter we suggest and WHY. A hung run on a hybrid
		// iGPU/dGPU box used to produce a log byte-identical to a healthy one
		// because placement was never recorded — it took a process dump to learn
		// it. This fires once per app (xrGetD3D11/12GraphicsRequirementsKHR).
		const char *provenance = "unknown";
		DXGI_ADAPTER_DESC desc{};
		bool have_desc = false;

		wil::com_ptr<IDXGIAdapter> forced = env_forced_d3d_adapter(&sys->xsysc->info);
		if (forced != nullptr) {
			THROW_IF_FAILED(forced->GetDesc(&desc));
			have_desc = true;
			sys->suggested_d3d_luid = desc.AdapterLuid;
			provenance = "env-forced";
		} else if (sys->xsysc->info.client_d3d_deviceLUID_valid) {
			sys->suggested_d3d_luid =
			    reinterpret_cast<const LUID &>(sys->xsysc->info.client_d3d_deviceLUID);
			// Keep the enumerated adapter (rather than just null-checking it) so
			// the log below can name it.
			wil::com_ptr<IDXGIAdapter> adapter =
			    getAdapterByLUID(sys->xsysc->info.client_d3d_deviceLUID, U_LOGGING_INFO);
			if (adapter == nullptr) {
				return oxr_error(log, XR_ERROR_RUNTIME_FAILURE,
				                 " failure enumerating adapter for LUID specified for use.");
			}
			have_desc = SUCCEEDED(adapter->GetDesc(&desc));
			provenance = "compositor-device";
		} else {
			auto adapter = getAdapterByIndex(0, U_LOGGING_INFO);
			if (adapter == nullptr) {
				return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, " failure enumerating adapter LUIDs.");
			}
			THROW_IF_FAILED(adapter->GetDesc(&desc));
			have_desc = true;
			sys->suggested_d3d_luid = desc.AdapterLuid;
			provenance = "adapter[0] fallback";
		}
		const WCHAR *desc_name = have_desc ? desc.Description : L"<unknown>";
		U_LOG_W("D3D graphics requirements: adapter '%ls' LUID=%08lx:%08lx (%s) (#1000)", desc_name,
		        (unsigned long)sys->suggested_d3d_luid.HighPart,
		        (unsigned long)sys->suggested_d3d_luid.LowPart, provenance);

		sys->suggested_d3d_luid_valid = true;
		*adapter_luid = sys->suggested_d3d_luid;
		//! @todo implement better?
		*min_feature_level = D3D_FEATURE_LEVEL_11_0;

		return XR_SUCCESS;
	}
	DEFAULT_CATCH(" failure determining adapter LUID")
}

XrResult
oxr_d3d_check_luid(struct oxr_logger *log, struct oxr_system *sys, LUID *adapter_luid)
{
	if (sys->xsysc == NULL) {
		return oxr_error(log, XR_ERROR_RUNTIME_FAILURE, " sys->xsysc == NULL");
	}

	if (adapter_luid->HighPart != sys->suggested_d3d_luid.HighPart ||
	    adapter_luid->LowPart != sys->suggested_d3d_luid.LowPart) {

		return oxr_error(log, XR_ERROR_GRAPHICS_DEVICE_INVALID,
		                 " supplied device does not match required LUID.");
	}

	return XR_SUCCESS;
}
