// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  "Which adapter scans out the 3D panel?" resolver (C++ flavour).
 * @ingroup aux_d3d
 */

#pragma once

#include "util/u_logging.h"

#include <dxgi.h>
#include <wil/com.h>

#include <stdint.h>


namespace xrt::auxiliary::d3d {

/*!
 * @brief The DXGI adapter that owns the output the 3D panel is scanned out on.
 *
 * The panel is located by its rect's **centre point** through
 * `MonitorFromPoint(..., MONITOR_DEFAULTTONEAREST)`. Deliberately **not**
 * edge-exact rect matching: `display_pixel_width/height` are physical panel
 * pixels while `display_screen_left/top` are virtual-screen coordinates, which
 * the OS DPI-virtualizes for a non-DPI-aware process. The centre point plus
 * `DEFAULTTONEAREST` tolerates that scaling error; comparing edges would not.
 *
 * That monitor is then mapped to an adapter by asking the OS display-config
 * database (`QueryDisplayConfig`) which kernel adapter owns the active display
 * path driving the monitor's GDI device — `sourceInfo.adapterId` is the same
 * LUID space as `DXGI_ADAPTER_DESC::AdapterLuid`. Walking DXGI outputs and
 * matching `DXGI_OUTPUT_DESC::Monitor` is only the *fallback*: on a hybrid
 * (Optimus) box a render-only discrete adapter also enumerates the internal
 * panel's output, so the output walk can name a GPU that scans out nothing.
 *
 * @param panel_screen_left Panel left edge in OS virtual-screen coordinates.
 * @param panel_screen_top Panel top edge in OS virtual-screen coordinates.
 * @param panel_pixel_width Panel width in physical pixels (0 = unknown).
 * @param panel_pixel_height Panel height in physical pixels (0 = unknown).
 * @param log_level The level to compare against for internal log messages.
 *
 * @return the adapter, or nullptr when unresolvable (caller WARNs once and
 * falls back to normal selection).
 */
wil::com_ptr<IDXGIAdapter>
getScanoutAdapter(int32_t panel_screen_left,
                  int32_t panel_screen_top,
                  uint32_t panel_pixel_width,
                  uint32_t panel_pixel_height,
                  u_logging_level log_level = U_LOGGING_INFO);

} // namespace xrt::auxiliary::d3d
