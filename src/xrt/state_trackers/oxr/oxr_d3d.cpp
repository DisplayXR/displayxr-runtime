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
#include "d3d/d3d_render_adapter.hpp"

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
 * ADR-037 §2: the adapter the runtime suggests via
 * xrGetD3D11/12GraphicsRequirementsKHR (which well-behaved clients — and the
 * Unity provider's own-device path — then create their device on) is the
 * *render* adapter, resolved by capability ranking in aux_d3d rather than by
 * DXGI's DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE. The DXR_D3D_FORCE_GPU override
 * channel (igpu | dgpu | scanout | <index>) flows THROUGH that resolver, so
 * this site no longer carries a copy of it. See d3d_render_adapter.hpp for the
 * ranking, the tiebreak and the provenance vocabulary.
 */

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

		// ADR-037 §2, one call: the override channel and the capability
		// ranking both live in the resolver, and it reports which of the two
		// answered. `from_env` is what preserves the historical precedence —
		// an explicit DXR_D3D_FORCE_GPU still outranks the compositor's own
		// device, while the *ranked* answer stays below it.
		const struct xrt_system_compositor_info *info = &sys->xsysc->info;
		RenderAdapterChoice choice =
		    getRenderAdapter(info->display_screen_left, info->display_screen_top, info->display_pixel_width,
		                     info->display_pixel_height, D3D_FEATURE_LEVEL_11_0, U_LOGGING_INFO);

		if (choice.from_env && choice.adapter != nullptr) {
			THROW_IF_FAILED(choice.adapter->GetDesc(&desc));
			have_desc = true;
			sys->suggested_d3d_luid = desc.AdapterLuid;
			provenance = choice.provenance;
			/*
			 * ADR-037 §7 / #1153: `client_d3d_deviceLUID` is the compositor's
			 * INGEST adapter, and ingest is the one device that must share an
			 * adapter with its clients. An override that points this session
			 * elsewhere is therefore a deliberate cross-adapter configuration
			 * (the all-on-scanout crossover arm). It is logged LOUDLY and it
			 * PROCEEDS — the branch order above already makes `from_env`
			 * outrank the compositor's LUID; this only makes the divergence
			 * visible instead of silent, so a black session is diagnosable
			 * from the log rather than from a dump.
			 */
			if (sys->xsysc->info.client_d3d_deviceLUID_valid) {
				const LUID &comp_luid =
				    reinterpret_cast<const LUID &>(sys->xsysc->info.client_d3d_deviceLUID);
				if (comp_luid.HighPart != desc.AdapterLuid.HighPart ||
				    comp_luid.LowPart != desc.AdapterLuid.LowPart) {
					U_LOG_W(
					    "DXR_D3D_FORCE_GPU (%s) puts this session on LUID=%08lx:%08lx while the "
					    "compositor ingests on LUID=%08lx:%08lx — CROSS-ADAPTER by explicit "
					    "override; proceeding (ADR-037 §4/§7, #1153)",
					    provenance, (unsigned long)desc.AdapterLuid.HighPart,
					    (unsigned long)desc.AdapterLuid.LowPart, (unsigned long)comp_luid.HighPart,
					    (unsigned long)comp_luid.LowPart);
				}
			}
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
		} else if (choice.adapter != nullptr) {
			THROW_IF_FAILED(choice.adapter->GetDesc(&desc));
			have_desc = true;
			sys->suggested_d3d_luid = desc.AdapterLuid;
			provenance = choice.provenance;
		} else {
			// The resolver found nothing usable — DXGI is broken or every
			// adapter was excluded. Keep the old last resort rather than
			// failing the app outright.
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
