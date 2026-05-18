# Hosting 2D Apps in the Workspace — Beyond Screen Capture

**Status**: brainstorm (May 2026). Pre-design. Not a commitment.
**Scope**: Windows workspace shell. Replaces / supplements the current M6 Phase 5 "2D app capture" path. Macos workspace is out of scope (not shipping today anyway).
**Audience**: DisplayXR runtime + shell contributors thinking about 2D-app embedding latency and input feel.
**Related**: M6 Phase 5 (2D app capture, shipped); ADR-013 (universal app launch model); ADR-017 (modal dialogs tiered strategy); [#228](https://github.com/DisplayXR/displayxr-runtime/issues/228) (Tier 1 spatial file picker — superficially similar, structurally different, see "What this is NOT").

## Problem

The shell embeds existing 2D Win32 apps as workspace tiles via Windows Graphics Capture (WGC) — the standard `GraphicsCaptureItem` / `Direct3D11CaptureFramePool` API. Captured frames are sampled into a workspace tile texture, input events (mouse / keyboard) are forwarded back to the captured HWND via `PostMessage`. Users describe the result as **clunky**:

- Visible end-to-end latency between cursor motion in the workspace and pointer motion in the captured app (probably 2–4 frames).
- Keystroke echo delay in text fields (synthesized `WM_KEYDOWN` arrives after several frames).
- Pointer hover affordances (tooltips, hover-lighten in browsers) feel off by a frame.
- GPU-accelerated apps with custom presentation paths (some game launchers, hardware-decoded video) sometimes capture as a black rectangle.

This is acceptable as a "look, your 2D apps work in the workspace" demo, but it's the worst-feeling part of the shell. We want a path to "feels native" for at least the 2D apps that matter most.

## Diagnosis: capture vs. input are independent problems

The clunkiness is **two unrelated latency budgets** stacked on top of each other. Improving one without the other halves the perceived improvement at best.

| Subsystem | Approx latency | Fix lever |
|---|---|---|
| WGC pixel pipeline | 2–4 frames (~30–60ms at 60Hz) | Replace WGC with a lower-latency pixel path |
| Shell → captured-window input forwarding | 1–2 frames (`PostMessage` round trip + app message-pump pickup) | Optimize input path independently |
| Workspace compositor → display | 1–2 frames (already optimized in current pipeline) | Out of scope here |

Many of the things users perceive as "capture is slow" are actually the input-forwarding tail. Worth profiling in isolation before assuming the pixel path is the bottleneck — quick win possible if input forwarding has avoidable buffering.

## Bifurcation: cooperating vs. non-cooperating apps

The real architectural fork isn't sidecar-vs-direct. It's **does the 2D app cooperate**.

### Cooperating apps (DComp visual hosting)

If a 2D app exposes its DirectComposition visual tree to other processes, the workspace compositor can sample it directly — microseconds of latency, no copy. The Microsoft spatial-apps preview, WinUI 3 islands, and Windows 11's spatial shell experiments all use this. The catch is that most legacy Win32 apps don't expose their DComp surface.

Apps that *do* expose it today, with no source changes needed:

- **UWP apps** (XAML / WinUI) — `Windows.UI.Composition` makes the visual tree consumable cross-process by design.
- **Edge WebView2** — exposes its compositor swap-chain visual via DComp; this is how it's hosted by other shells.
- **WinUI 3 islands** — same DComp underneath as UWP.
- **Anything we ship ourselves** — if a new DisplayXR 2D app is built on WebView2, it gets the cooperating path for free.

Apps that *could* be made to cooperate with a small shim:

- **Win32 apps built with skiasharp / cef / electron** — anything that already renders into a D3D surface internally. A small initialization shim could publish that surface to the runtime via shared handle.
- **Native Win32 apps the vendor controls** — add 10 lines to opt in via a tiny `displayxr-2d-host.dll`.

The mechanism in either case: the cooperating app publishes a `DCompositionCreateSurfaceFromHandle`-style surface handle (or a shared D3D11 texture handle, or a UWP `ICoreApplicationView` visual tree pointer) via IPC. The workspace compositor binds it as a tile texture and samples it directly each frame. Latency drops to compositor-frame-aligned (sub-frame).

### Non-cooperating apps (legacy long tail)

Photoshop, Excel, random `.exe` from 2008 — these will never expose a cross-process visual. For these the only options are pixel-extraction APIs:

- **WGC** (current) — works on almost everything; 2–4 frame latency floor.
- **PrintWindow / BitBlt** — lower latency for non-accelerated UIs; broken on GPU-accelerated apps; CPU-expensive.
- **DXGI Output Duplication** — desktop-level, not per-window; not useful for tile hosting.

The capture-path floor for non-cooperating apps is just what it is — we don't get to fix Photoshop. The realistic goal is **acceptable-feeling capture** for the long tail, **excellent-feeling DComp** for the apps that matter most.

## What this is NOT

- **Not the Tier 1 spatial file picker** ([#228](https://github.com/DisplayXR/displayxr-runtime/issues/228)). That's a *new* OpenXR app drawing its own 3D UI — there is no 2D app to host. The sidecar-app architecture there doesn't generalize to 2D-app hosting because the picker's leverage comes from being natively 3D, not from being a wrapper.
- **Not a replacement for the current capture path**. Cooperating-app DComp is *additive*. WGC stays as the fallback for everything that can't or won't cooperate, indefinitely.
- **Not a UWP-only story**. The DComp visual-sharing API is OS-level; Win32 apps that opt in (via the proposed shim) get the same path.
- **Not a spatial-XR-specific contract**. The "DisplayXR 2D host" pattern would just be a thin Win32 lib that exposes a DComp visual — usable by any compositor that wants to consume it. We'd inherit the API design from Windows' existing visual-sharing primitives.

## Three candidate workstreams (none committed)

### W1: Input-forwarding latency audit (cheap, high-confidence win)

Independent of any of the below. Instrument the current shell input-forwarding path: WndProc → workspace raycast → IPC → captured-window `PostMessage` → captured-window message pump → app reaction. Find where the 1–2 frames live. Likely candidates: shell's per-frame batching of events, `PostMessage` queueing behavior, captured-app's own message pump tick rate. **Quick win regardless of which capture path we use.** Engineering: ~1 week.

### W2: DComp path for cooperating apps (medium, big perceived win)

Add a parallel "DComp tile" path to the d3d11_service compositor. New IPC: client publishes a shared DComp visual handle / shared-texture handle; compositor binds it as a tile texture instead of using WGC. Hook the first cooperating apps:

- Any WebView2-based DisplayXR app gets it free (just need the host glue).
- A small `displayxr-2d-host.dll` for Win32 apps that want to opt in (10 lines on the app side).
- A demo: a WebView2-based "DisplayXR notes" or "DisplayXR browser" tile, side-by-side with a WGC-captured Notepad, to show the latency delta viscerally.

Engineering: ~3–4 weeks for the runtime / shell plumbing + first cooperating demo. Larger if we want to ship the `displayxr-2d-host.dll` as a contractually-stable public lib.

### W3: Curated cooperating-app ecosystem (long, strategic)

If the verdict on W2 is "this feels great," the followup is encouraging an ecosystem of cooperating apps. Mostly product/partnership work, not engineering:

- Document the contract (probably just a header + a tiny lib).
- Ship a reference WebView2-based "DisplayXR Web App" launcher template.
- Engage with one or two strategic 2D apps (e.g., a CAD vendor, a media player vendor) to add cooperation.
- Long tail: a CLI tool that wraps an arbitrary `.exe` with the shim if the app is built on a known framework (Electron, CEF, Qt).

This is the part with the biggest payoff but the longest timeline. Not something to start until W2 has proved the latency win on a real demo.

## Open questions

1. **How much of the perceived clunkiness is W1 vs. W2?** Worth profiling before sizing W2. If input forwarding is 80% of the felt latency, W1 alone might be enough.
2. **Does WGC's "Auto HDR / SDR conversion" issue (the black-rectangle case on some apps) have a non-DComp workaround?** PrintWindow fallback was attempted in M6 Phase 5 but had its own issues. Worth revisiting given two more years of WGC fixes.
3. **What's the bar for "cooperating"?** Is it a `.dll` you LoadLibrary, a header-only `.lib`, a Windows runtime API, a contract via env-var? Different bars produce different ecosystems.
4. **Cross-platform story.** macOS workspace is dormant today, but if it revives, this whole strategy is Windows-specific. The cooperating-app contract would need a macOS equivalent (CALayer hosting via NSWindow contentView surface sharing) — doable but separate design.
5. **Trust boundary.** If a cooperating app publishes its DComp visual to the shell, can a malicious app publish a deceptive visual (e.g., draw fake credential prompts that look like the shell chrome)? Probably no worse than today (any app can `CreateWindow` and draw anything in 2D) but worth thinking through before opening the contract publicly.

## Decision points

We don't have to commit to anything from this doc to make progress. Reasonable next steps in order:

1. **Do W1** — input audit is cheap and pays off regardless. Defer everything else pending its findings.
2. **Pilot W2 on a single WebView2 demo app**, compare side-by-side with WGC-captured equivalent, get a felt-quality verdict before scoping the broader plumbing.
3. **Only then** decide whether W3 is worth the partnership / ecosystem effort.

If the W1 audit finds input forwarding is the dominant felt latency and the WGC pixel pipeline is acceptable, W2/W3 may be unnecessary — at least for the apps we have today. Don't pre-commit to a multi-quarter ecosystem play before measuring.

## Status / next step

Brainstorm only. No issue filed yet. Next actionable step is the W1 input-forwarding audit; that's a small enough scope to file as a single issue when it's prioritized.
