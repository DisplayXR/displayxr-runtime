# OpenXR Runtime Comparison: SRHydra vs Monado

This report compares the SRHydra and Monado OpenXR runtime approaches for achieving Windows/Android OpenXR support.

## High-Level Feature Comparison

| Feature | SRHydra | Monado | Notes |
|---------|---------|--------|-------|
| **Windows** | YES | YES | |
| **WebXR** | YES | NO (1-2 weeks) | |
| **D3D11** | YES | YES | |
| **D3D12** | YES | YES | Does not work on AMD GPU |
| **OpenGL (Blender)** | YES | YES | Does not work on AMD GPU |
| **Vulkan** | YES | YES | Does not work on AMD GPU |
| **Android** | NO | YES | |
| **Linux** | NO | YES | |
| **macOS** | NO | YES | Can be built and launched via MoltenVK, not tested |
| **SR GUI** | YES | NO (can be ported) | |
| **Keyboard Controls** | YES | YES | Monado has qwerty driver for keyboard/mouse input |
| **Various controllers (Hydra, Daydream)** | NO | YES | |
| **Documentation** | Lacking | Extensive | |
| **Stable release cadence** | NO | YES | |
| **Multi-app support** | NO | YES | |
| **External maintainers** | NO | YES | A controversial point but definitely worth noting |
| **Hand tracking** | NO | YES | |

## Keyboard Controls Correction

The original assessment indicated Monado lacked keyboard controls. This is incorrect. Monado includes a full `qwerty` driver (`src/xrt/drivers/qwerty/`) that provides:

- Simulated HMD via keyboard/mouse input
- Simulated left and right controllers
- SDL2-based input handling
- Useful for development and testing without physical hardware

## Notable Examples

- **PortalVR** - Featured at CES, based on Monado, closed-source

## Summary

Both SRHydra and Monado codebases can be used to achieve the goal. However, Monado requires less effort to establish and maintain a Windows/Android OpenXR runtime in the long term (due to its open-source nature). There are a few trade-offs (e.g. the lack of WebXR support) but they are low-risk and low-effort.

### Monado Advantages
- Cross-platform (Windows, Linux, Android, experimental macOS)
- Active open-source community with external maintainers
- Extensive driver support for various HMD and controller hardware
- Built-in hand tracking (Mercury)
- Multi-app support via IPC service mode
- Keyboard/mouse input for development via qwerty driver

### SRHydra Advantages
- WebXR support
- SR-specific GUI integration
