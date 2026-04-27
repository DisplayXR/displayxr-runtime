// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// Optimus / PowerXpress hints for NVIDIA / AMD hybrid-GPU laptops.
// Exporting these symbols from the executable tells the driver to put
// the process on the discrete GPU. The DisplayXR service compositor is
// pinned to the same dGPU via DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE; if
// an app lands on the iGPU instead, IPC shared textures fail.
//
// Lives in sr_common_base. The CMake target uses /INCLUDE:NvOptimusEnablement
// (INTERFACE linker option) to force inclusion of this TU even though no
// other code references the symbols.

#ifdef _WIN32
#include <windows.h>

__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
#endif
