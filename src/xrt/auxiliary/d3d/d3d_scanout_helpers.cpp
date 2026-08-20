// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Panel rect -> HMONITOR -> DXGI adapter, the one place that answers
 *         "which adapter scans out the 3D panel?".
 * @ingroup aux_d3d
 *
 * Single source of truth for the `scanout` keyword accepted by both
 * DXR_D3D_FORCE_GPU and DXR_VK_FORCE_GPU (#918 Phase 0, closes the #846 gap).
 * D3D consumes the adapter directly; Vulkan consumes the LUID and matches it
 * against `VkPhysicalDeviceIDProperties::deviceLUID`.
 */

#include "d3d_scanout_helpers.hpp"
#include "d3d_scanout_helpers.h"

#include "util/u_logging.h"

#include <dxgi1_6.h>
#include <wil/com.h>
#include <wil/result.h>

#include <string.h>
#include <wchar.h>

#include <vector>

namespace xrt::auxiliary::d3d {

namespace {

	//! The GDI device name (`\\.\DISPLAY1`) of the monitor the panel sits on.
	bool
	monitor_gdi_name(HMONITOR mon, MONITORINFOEXW *out_info)
	{
		MONITORINFOEXW mi{};
		mi.cbSize = sizeof(mi);
		if (!GetMonitorInfoW(mon, &mi)) {
			return false;
		}
		*out_info = mi;
		return true;
	}

	/*!
	 * Authoritative source: ask the OS display-config database which kernel
	 * adapter owns the active display path that drives this GDI device.
	 *
	 * `DISPLAYCONFIG_PATH_SOURCE_INFO::adapterId` is the same LUID space as
	 * `DXGI_ADAPTER_DESC::AdapterLuid`, so the answer maps straight onto a DXGI
	 * adapter (and onto `VkPhysicalDeviceIDProperties::deviceLUID`).
	 */
	bool
	scanout_luid_from_display_config(const wchar_t *gdi_device_name, LUID *out_luid)
	{
		UINT32 path_count = 0;
		UINT32 mode_count = 0;
		if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
			return false;
		}

		std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
		std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
		if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(),
		                       nullptr) != ERROR_SUCCESS) {
			return false;
		}
		paths.resize(path_count);

		for (const DISPLAYCONFIG_PATH_INFO &path : paths) {
			DISPLAYCONFIG_SOURCE_DEVICE_NAME sdn{};
			sdn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
			sdn.header.size = sizeof(sdn);
			sdn.header.adapterId = path.sourceInfo.adapterId;
			sdn.header.id = path.sourceInfo.id;
			if (DisplayConfigGetDeviceInfo(&sdn.header) != ERROR_SUCCESS) {
				continue;
			}
			if (wcscmp(sdn.viewGdiDeviceName, gdi_device_name) == 0) {
				*out_luid = path.sourceInfo.adapterId;
				return true;
			}
		}

		return false;
	}

	wil::com_ptr<IDXGIAdapter>
	adapter_by_luid(IDXGIFactory1 *factory, LUID luid, DXGI_ADAPTER_DESC1 *out_desc)
	{
		for (UINT i = 0;; i++) {
			wil::com_ptr<IDXGIAdapter1> adapter;
			if (FAILED(factory->EnumAdapters1(i, adapter.put())) || adapter == nullptr) {
				break;
			}
			DXGI_ADAPTER_DESC1 desc{};
			if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
				continue; // skip WARP / Basic Render Driver
			}
			if (desc.AdapterLuid.HighPart == luid.HighPart && desc.AdapterLuid.LowPart == luid.LowPart) {
				*out_desc = desc;
				return adapter.query<IDXGIAdapter>();
			}
		}
		return nullptr;
	}

	/*!
	 * Fallback for when the display-config database is unavailable: the adapter
	 * whose DXGI output claims this HMONITOR.
	 *
	 * Weaker than the display-config answer — on a hybrid (Optimus) box a
	 * render-only discrete adapter can also enumerate the internal panel's
	 * output, which is exactly the case the primary path exists to get right.
	 */
	wil::com_ptr<IDXGIAdapter>
	adapter_owning_monitor(IDXGIFactory1 *factory, HMONITOR mon, DXGI_ADAPTER_DESC1 *out_desc)
	{
		for (UINT i = 0;; i++) {
			wil::com_ptr<IDXGIAdapter1> adapter;
			if (FAILED(factory->EnumAdapters1(i, adapter.put())) || adapter == nullptr) {
				break;
			}
			DXGI_ADAPTER_DESC1 adesc{};
			if (FAILED(adapter->GetDesc1(&adesc)) || (adesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
				continue;
			}
			for (UINT j = 0;; j++) {
				wil::com_ptr<IDXGIOutput> output;
				if (FAILED(adapter->EnumOutputs(j, output.put())) || output == nullptr) {
					break;
				}
				DXGI_OUTPUT_DESC odesc{};
				if (FAILED(output->GetDesc(&odesc))) {
					continue;
				}
				// HMONITOR equality keeps the comparison inside this
				// process's GDI coordinate space.
				if (odesc.Monitor == mon) {
					*out_desc = adesc;
					return adapter.query<IDXGIAdapter>();
				}
			}
		}
		return nullptr;
	}

} // namespace

wil::com_ptr<IDXGIAdapter>
getScanoutAdapter(int32_t panel_screen_left,
                  int32_t panel_screen_top,
                  uint32_t panel_pixel_width,
                  uint32_t panel_pixel_height,
                  u_logging_level log_level)
{
	wil::com_ptr<IDXGIAdapter> ret;

	// A zero-sized panel means the display processor never reported real
	// dimensions — there is nothing to locate. (A 0,0 ORIGIN is fine: that's
	// the primary monitor.)
	if (panel_pixel_width == 0 || panel_pixel_height == 0) {
		U_LOG_IFL_I(log_level, "scanout: no panel dimensions reported — cannot resolve.");
		return ret;
	}

	// Centre point, not edges: see the DPI note on the header declaration.
	POINT centre{};
	centre.x = (LONG)panel_screen_left + (LONG)(panel_pixel_width / 2);
	centre.y = (LONG)panel_screen_top + (LONG)(panel_pixel_height / 2);

	HMONITOR panel_monitor = MonitorFromPoint(centre, MONITOR_DEFAULTTONEAREST);
	if (panel_monitor == nullptr) {
		U_LOG_IFL_I(log_level, "scanout: MonitorFromPoint(%ld, %ld) found no monitor.", centre.x, centre.y);
		return ret;
	}

	MONITORINFOEXW mi{};
	if (!monitor_gdi_name(panel_monitor, &mi)) {
		U_LOG_IFL_I(log_level, "scanout: GetMonitorInfoW failed for the panel's monitor.");
		return ret;
	}

	wil::com_ptr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void())) || factory == nullptr) {
		U_LOG_IFL_I(log_level, "scanout: DXGI factory unavailable.");
		return ret;
	}

	DXGI_ADAPTER_DESC1 desc{};

	LUID path_luid{};
	if (scanout_luid_from_display_config(mi.szDevice, &path_luid)) {
		ret = adapter_by_luid(factory.get(), path_luid, &desc);
		if (ret != nullptr) {
			U_LOG_IFL_I(log_level,
			            "scanout: display path '%ls' is driven by adapter '%ls' (LUID %08lx:%08lx).",
			            mi.szDevice, desc.Description, (unsigned long)desc.AdapterLuid.HighPart,
			            (unsigned long)desc.AdapterLuid.LowPart);
			return ret;
		}
		U_LOG_IFL_I(log_level,
		            "scanout: display path '%ls' names LUID %08lx:%08lx, which no DXGI hardware adapter "
		            "reports — falling back to the output walk.",
		            mi.szDevice, (unsigned long)path_luid.HighPart, (unsigned long)path_luid.LowPart);
	} else {
		U_LOG_IFL_I(log_level, "scanout: no active display path for '%ls' — falling back to the output walk.",
		            mi.szDevice);
	}

	ret = adapter_owning_monitor(factory.get(), panel_monitor, &desc);
	if (ret != nullptr) {
		U_LOG_IFL_I(log_level, "scanout: monitor '%ls' is enumerated as an output of adapter '%ls'.",
		            mi.szDevice, desc.Description);
		return ret;
	}

	U_LOG_IFL_I(log_level, "scanout: no hardware adapter claims the panel's monitor.");
	return ret;
}

} // namespace xrt::auxiliary::d3d


extern "C" bool
d3d_scanout_adapter_luid(int32_t panel_screen_left,
                         int32_t panel_screen_top,
                         uint32_t panel_pixel_width,
                         uint32_t panel_pixel_height,
                         uint64_t *out_packed_luid)
{
	if (out_packed_luid == nullptr) {
		return false;
	}

	wil::com_ptr<IDXGIAdapter> adapter = xrt::auxiliary::d3d::getScanoutAdapter(
	    panel_screen_left, panel_screen_top, panel_pixel_width, panel_pixel_height, U_LOGGING_INFO);
	if (adapter == nullptr) {
		return false;
	}

	DXGI_ADAPTER_DESC desc{};
	if (FAILED(adapter->GetDesc(&desc))) {
		return false;
	}

	// Pack exactly as Vulkan hands back VkPhysicalDeviceIDProperties::deviceLUID:
	// the raw bytes of the Windows LUID.
	uint64_t packed = 0;
	static_assert(sizeof(desc.AdapterLuid) == sizeof(packed), "LUID size mismatch");
	memcpy(&packed, &desc.AdapterLuid, sizeof(packed));
	*out_packed_luid = packed;

	return true;
}
