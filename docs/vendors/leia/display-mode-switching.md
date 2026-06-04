# Leia SR — Display Mode Switching (2D/3D)

How the Leia plug-in implements the neutral `xrRequestDisplayRenderingModeEXT` /
`xrRequestDisplayModeEXT` contract from
[`XR_EXT_display_info`](../../specs/extensions/XR_EXT_display_info.md). The extension spec stays
vendor-neutral: the runtime translates a mode request into the display processor's `set_property`
call, and the vendor SDK implements it as either a preference-based request (aggregated across
applications) or direct hardware control. This page documents the concrete Leia mechanism.

## Per-platform translation

| Platform | Runtime Implementation |
|---|---|
| Windows (SR SDK) | `SwitchableLensHint::enable()` / `SwitchableLensHint::disable()` — preference-based, aggregated across applications. |
| Android (CNSDK) | `leia_core_set_backlight(core, true)` / `leia_core_set_backlight(core, false)` — direct backlight control. |

On Windows the SR SDK's `SwitchableLensHint` is a **preference**, not a hard set: the platform
may aggregate hints from multiple applications or defer the switch, which is why the OpenXR API
is shaped as a *request* (returning `XR_SUCCESS` on acceptance, not on physical completion).

On Android, `leia_core_set_backlight` is direct: enabling the backlight engages the lightfield
optics, disabling it returns the panel to conventional 2D.
