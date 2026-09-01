# DPI awareness: the DLL rule

**One rule, and it is the one that keeps getting missed:**

> A DLL does not control its process's DPI awareness. Every Win32 geometry call
> made inside one answers in the **host process's** DPI space — not the
> runtime's, and not necessarily physical pixels.

The runtime ships as a DLL, and for every in-process app class it is loaded into
the **application's** process. Vendor plug-ins are DLLs inside that same process.
So the per-monitor-v2 manifest the runtime embeds in its own executables governs
`displayxr-service.exe`, `displayxr-cli.exe` and friends — and governs **nothing**
about geometry read while running inside a game engine's player.

## What goes wrong

A DPI-unaware process is handed **virtualised** coordinates: a 3840×2160 panel at
250% scaling reports as 1536×864, and a monitor whose true origin is x=2560 can
compute as x=5120. Geometry read in that space and then published — through an
OpenXR extension, an IPC field, a log line used as evidence — is silently wrong,
and wrong by *the ratio of two monitors' scale factors*. That means:

- It looks **correct on every single-monitor development box**, where there is
  only one scale factor and everything is consistently wrong together.
- It fails on **mixed-DPI multi-monitor rigs** — exactly the configuration the
  geometry was needed for in the first place.

This has now bitten in three separate places (displayxr-unity#263, and both the
runtime and plug-in halves of #1301), always the same way: two components in one
process disagreeing about which space they are in, with nothing converting
between them.

## The primitive

`SetProcessDpiAwarenessContext` is **not** the answer inside a DLL — awareness is
a process property and a library must not change its host's.

`SetThreadDpiAwarenessContext` is per-thread and reversible, so a DLL may
legitimately pin it around a query:

```c
// Resolve dynamically: Windows 10 1607+, and MinGW headers may not declare it.
// Absence is not fatal — fall back to the host's context.
prev = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
/* ... MonitorFromPoint / GetMonitorInfoW / GetWindowRect ... */
SetThreadDpiAwarenessContext(prev);
```

Reference implementation: `src/xrt/auxiliary/os/os_display_desktop_win32.c`.

**Applies to** `MonitorFromPoint` / `MonitorFromWindow`, `GetMonitorInfo`,
`EnumDisplayMonitors`, `GetWindowRect` / `GetClientRect`, `ClientToScreen`, and
anything else returning screen or window geometry. `EnumDisplaySettingsW(…,
ENUM_CURRENT_SETTINGS)` is the exception — it reports true pixels regardless,
which is what makes it usable as an oracle (`displayxr-cli selftest`'s
`display_dims` check).

## Publishing geometry across a boundary

State the space, and make the value match the claim. `XR_DXR_display_info`'s
`XrDisplayDesktopInfoDXR.desktopRect` is specified as physical virtual-desktop
pixels and is resolved under a pinned context, so it holds whatever the host
declared. A consumer still has to *act* in that space — placing a window with
physical coordinates from a DPI-unaware process re-introduces the error at the
other end.

**Never compare two values that came from different spaces.** The panel
dimensions a plug-in reports (`displayPixelWidth`/`Height`) are read in the
host's space and virtualise; a rect resolved under a pinned context does not.
Comparing them fails on every box at non-100% scaling while saying nothing about
whether the monitors match. When such a comparison is genuinely needed, take both
sides in the *same* space — `os_display_desktop_info` returns
`width_in_caller_dpi`/`height_in_caller_dpi` for exactly this, and for nothing
else.

## Reproducing it

Force any process DPI-unaware without touching the box's display settings:

```bat
set __COMPAT_LAYER=~ DPIUNAWARE
displayxr-cli.exe info
```

Run it with and without, and diff. On a 250%-scaled box the unaware run collapses
every virtualised value to 40% of the physical one, which makes an otherwise
invisible class of bug obvious in a single A/B. `displayxr-cli info` prints
`DPI aware:`, the plug-in's `pixels:` and the resolved `desktop rect:` side by
side for this purpose — under `__COMPAT_LAYER` the first two change and the third
must not.

`displayxr-cli selftest`'s `dpi_awareness` check asserts the **process** is
per-monitor aware (#1201); it cannot tell you whether a given call site pinned
its context, so the A/B above is the real test.
