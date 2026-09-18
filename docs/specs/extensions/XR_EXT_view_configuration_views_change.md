# XR_EXT_view_configuration_views_change — DisplayXR adoption note

| Property | Value |
|----------|-------|
| Extension Name | `XR_EXT_view_configuration_views_change` |
| Owner | **Khronos**, not DisplayXR. Extension 840, `SPEC_VERSION 1`, first in the OpenXR SDK **1.1.58**; this repo vendors it from 1.1.63. |
| Type Value | `XR_TYPE_EVENT_DATA_VIEW_CONFIGURATION_VIEWS_CHANGED_EXT` = `1000839000` (Khronos-assigned) |
| Scope | `type="instance"` — the event carries **no `XrSession`**. |
| Status | The registry `xr.xml` carries **no `ratified="openxr"`** on extension 840, so the generated man page reports *"Status: Not ratified"* despite the 1.1.58 changelog billing. |
| Tracking | [#1488](https://github.com/DisplayXR/displayxr-runtime/issues/1488) |

> This is an **adoption note**, not a specification. The normative text is
> Khronos's (`specification/sources/chapters/extensions/ext/ext_view_configuration_views_change.adoc`).
> This page records only what *DisplayXR* does with it — which is the part an app
> author cannot read out of the Khronos spec.
>
> It is deliberately **not** in `docs/specs/extensions/index.json`: that catalog is
> the `XR_DXR_*` set, and `scripts/gen_extensions_index.py` globs `XR_DXR_*.h` only.
> A Khronos extension is outside it by construction.

---

## 1. Why the runtime needs it at all

`xrEnumerateViewConfigurationViews` is frozen by the core spec:

> *"Runtimes **must:** always return identical buffer contents from this
> enumeration … for the lifetime of the instance."*

DisplayXR fills `sys->views` once, at `xrCreateInstance` time, and never writes it
again — so every window resize, display-mode switch and mask activation silently
invalidates the answer the app was given. `XR_EXT_view_configuration_views_change`
is the **only sanctioned carve-out** from that rule. Enabling it is therefore the
mechanism by which the runtime stops lying, and the enablement gate is *mandatory*,
not a nicety: an app that has **not** enabled the extension keeps the frozen
snapshot bit-for-bit.

**Be honest about the payoff.** As of this writing **no engine handles this event**
— not Godot, not Unity, not (on the best available evidence) Unreal. LÖVR is the
only complete app-side implementation found in the wild. What adoption buys is
(a) spec compliance, (b) a standard channel for our own providers, and (c) the
retirement of two bespoke events. It does **not** mean "engines now handle resizes
for free."

## 2. What DisplayXR emits, and when

The runtime queues `XrEventDataViewConfigurationViewsChangedEXT` from the frame-end
path — the same point that has always pushed the bespoke
`XrEventDataLocal3DZoneViewSizeChangedDXR` — and it queues it **only when
`xrEnumerateViewConfigurationViews` would actually return something different**.

The event is emitted *after* the new values are already visible to the enumerate
call, so an app that wakes on the event and immediately re-enumerates reads the new
size, never the old one.

`systemId` and `viewConfigurationType` are populated from the system's real
`systemId` and `view_config_type`. A conformant consumer **filters on both** (LÖVR
does), so both are load-bearing; an app that ignores the filter and re-enumerates a
different view configuration gets whatever that configuration reports, which is its
own problem.

Because the event is instance-scoped it is **not** removed by session teardown —
correct, and deliberate.

## 3. Only `recommended*` moves. `max*` is immutable.

The Khronos text is explicit:

> *"The runtime **must:** only change the content of the **recommended** values
> within `XrViewConfigurationView`."*

DisplayXR narrows that further. Of the four fields, exactly **two** ever change:

| Field | Changes? |
|---|---|
| `recommendedImageRectWidth` | **yes** |
| `recommendedImageRectHeight` | **yes** |
| `maxImageRectWidth` | never |
| `maxImageRectHeight` | never |
| `recommendedSwapchainSampleCount` | never — the spec would allow it; we hold it fixed so "only two fields ever change" stays trivially auditable |
| `maxSwapchainSampleCount` | never |

`maxImageRect*` immutability is not a promise we invented for this extension — it
is [ADR-010](../../adr/ADR-010-shared-app-iosurface-worst-case-sized.md)'s worst-case
swapchain invariant, which predates the extension by years. The Khronos `must:` and
our ADR happen to say the same thing. That agreement is the whole reason adoption
is cheap.

## 4. The rule for apps: move the rect, never reallocate

Straight from the Khronos chapter, and the single most important sentence on this
page:

> **An app that sized at `maxImageRect*` never needs to reallocate.**

Every DisplayXR app sizes at `maxImageRect*`, because ADR-010 requires it. So the
correct handler is:

```c
case XR_TYPE_EVENT_DATA_VIEW_CONFIGURATION_VIEWS_CHANGED_EXT: {
    const XrEventDataViewConfigurationViewsChangedEXT *e = (const void *)&ev;
    if (e->systemId != my_system_id ||
        e->viewConfigurationType != my_view_config_type) {
            break;                       // not mine — see §2
    }
    xrEnumerateViewConfigurationViews(instance, e->systemId,
                                      e->viewConfigurationType,
                                      cap, &count, views);
    // Re-tile: recompute subImage.imageRect from the new recommended size.
    // Do NOT call xrCreateSwapchain here.
    retile_from(views, count);
    break;
}
```

No VU ties the event to swapchain lifetime. `xrCreateSwapchain` is a `should:` in
the chapter; ignoring the event entirely is an explicit `may:`. Reallocating is
therefore never *required*, and in DisplayXR it is actively wrong — see §6.

This is what the app linter's **INV-4.9** checks
(`scripts/check_displayxr_app.py`, prose in
[`docs/guides/displayxr-app-rules.md`](../../guides/displayxr-app-rules.md)).

## 5. The 1 Hz throttle is mandatory

> *"The runtime **must:** not use this event for frequent (at a rate faster than
> 1 Hz per view configuration) adjustments of the resolution."*

The runtime coalesces: at most one event per view configuration per second, and the
coalesced value the next enumerate returns is the **latest** one, not the first. A
drag-resize therefore produces a trickle, not a 60 Hz flood — which matters beyond
politeness, because every queued event is heap-allocated until the app polls it.

This is a spec `must:`, not a tuning knob. Do not "helpfully" raise the rate.

## 6. The LÖVR hazard — why a doorbell alone would be harmful

LÖVR's handler (`src/modules/headset/headset.c`) calls `updateViewSize()` **and
`createSwapchains()` unconditionally** on the event. That shape is the reason the
runtime must never ring a doorbell for a value that did not move: a consumer built
that way would tear down and recreate its swapchains for an unchanged size,
reintroducing exactly the reallocation stutter ADR-010's worst-case swapchain
exists to prevent.

Hence the structural rule in §2 — the emit is downstream of the live-value write,
which is downstream of the edge detection, so a no-change frame produces no write
and no event. **Advertising the extension without making the values live is
rejected**: it is not merely useless, it is a regression.

## 7. Kill switches

Two independent next-launch gates on the `DEBUG_GET_ONCE_BOOL_OPTION` seam (both
also reachable on Android as `debug.xrt.<name>`). Registered in Appendix A of
`docs/roadmap/control-panel-performance-settings.md`.

| Env var | Default | `0` means |
|---|---|---|
| `DXR_VIEWS_CHANGE_EVENT` | on | Never emit `XrEventDataViewConfigurationViewsChangedEXT`. The extension stays advertised and enableable; an app that enables it simply never gets a doorbell — spec-legal (`may:`). |
| `DXR_VIEWS_CHANGE_LIVE` | on | `xrEnumerateViewConfigurationViews` always returns the frozen `xrCreateInstance`-time snapshot, regardless of enablement — i.e. exactly the pre-#1488 behaviour. |

Both `0` ⇒ behaviourally identical to the pre-change runtime, with **no rebuild and
no reinstall**. These are dev/support escape hatches; the *"registry gates, not env
vars"* convention governs user-facing settings, which these are not.

## 8. Relationship to the two bespoke DXR events

| Event | Status after #1488 |
|---|---|
| `XrEventDataLocal3DZoneViewSizeChangedDXR` (`XR_DXR_local_3d_zone`, spec **5**) | **Soft-deprecated, still emitted forever**, unchanged layout. Every known consumer logs it and does nothing else. Removing it would buy nothing and could break an unknown external consumer. |
| `XrEventDataDisplayZoneMetricsChangedDXR` (`XR_DXR_display_zones`) | **Deprecated, never emitted.** It never had a push site. The type is **kept** — deleting it is a source break for anything that `case`s on it, including `displayxr-unreal`'s `abi-guard`. |

Neither header changes layout, so **no consumer must rebuild and no wire contract
breaks**.

**What replaces the dead one:** for **per-zone** sizes, poll
`xrGetDisplayZoneRecommendedViewSizeDXR` for each zone — that is what shipping code
already does. For the **single-size** case, this EXT. The EXT is instance-level and
carries one size, so it structurally cannot describe N zones; that is why the two
answers differ rather than one superseding the other.

## 9. Non-goals

- **View *count* changes.** The EXT forbids them and DisplayXR's count is fixed per
  system anyway. `XrEventDataRenderingModeChangedDXR` remains the count/mode
  doorbell. (Separate issue: [#1486](https://github.com/DisplayXR/displayxr-runtime/issues/1486),
  [`docs/reference/view-configuration-model.md`](../../reference/view-configuration-model.md).)
- **Moving `maxImageRect*`, the worst-case swapchain, or `u_tiling_can_zero_copy()`** — ADR-010 stands.
- **Bumping sibling repos' vendored headers.** None of them can even see the symbol
  today, and none enables the extension.

## See also

- [`XR_DXR_display_zones`](XR_DXR_display_zones.md) · [ADR-027](../../adr/ADR-027-display-zones.md) · [display-zones roadmap](../../roadmap/display-zones.md)
- [`docs/reference/view-configuration-model.md`](../../reference/view-configuration-model.md) — what `PRIMARY_STEREO` means here
- [`docs/guides/displayxr-app-rules.md`](../../guides/displayxr-app-rules.md) — INV-4.9
- [`docs/specs/runtime/swapchain-model.md`](../runtime/swapchain-model.md) — the two swapchains and the canvas
