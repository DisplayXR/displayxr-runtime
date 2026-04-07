---
status: Proposal
owner: David Fattal
updated: 2026-03-21
issues: [43, 44]
code-paths: [src/xrt/compositor/multi/]
---

> **Status: Proposal** — not yet implemented. Tracking issue: [#43](https://github.com/DisplayXR/displayxr-runtime-pvt/issues/43), [#44](https://github.com/DisplayXR/displayxr-runtime-pvt/issues/44)

# 3D Capture Pipeline

## Scope and Related Docs

This doc covers the **3D capture pipeline** — how the runtime captures spatial content before display-specific weaving. It is a runtime feature with shell-provided UX.

| Doc | Relationship |
|-----|-------------|
| [spatial-os.md](spatial-os.md) (#43) | **Compositing mechanism.** Capture taps into the multi-compositor pipeline defined here. |
| [3d-shell.md](3d-shell.md) (#44) | **Shell layer.** Owns capture UX (hotkeys, browse, share). |
| [shell-runtime-contract.md](shell-runtime-contract.md) | **IPC contract.** Defines capture command/completion messages between shell and runtime. |

## Vision

Capture the composed spatial scene **after** multi-compositor compositing but **before** weaving/interlacing. This preserves reusable stereo content rather than display-specific interlaced output.

3D capture is one of the clearest platform differentiators because it makes the spatial desktop:
- recordable
- shareable
- inspectable
- reusable for ML / world-model pipelines later

## Capture Point in Pipeline

Capture must occur after cross-app spatial composition and before weave / display-specific transformation.

```text
App outputs
   → per-app compositor / window capture
   → multi compositor
   → [CAPTURE POINT]
   → display processor / weaver
   → display
```

At the capture point, the runtime has access to final L/R images in physical display pixel space — the richest representation before it gets destroyed by display-specific interlacing.

## Capture Layers

### A. Frame Capture

Single-frame capture of the composed spatial scene.

**Outputs:**
- Left image
- Right image
- Optional SBS (side-by-side) image
- Optional metadata sidecar:
  - Timestamp
  - Display pose
  - Eye pose
  - Window poses
  - Camera / FOV parameters

### B. Recording

Temporal capture of the composed spatial scene.

**Outputs:**
- Stereo video
- Optional metadata stream
- Optional per-frame window transforms

### C. Session Capture

Higher-level capture of a replayable session.

**Outputs:**
- Frame sequence or compressed stream
- Time series of window poses
- User inputs if available
- Shell state if relevant
- Future support for viewpoint-scrubbable replay

### D. Dataset Capture Mode

Opt-in capture mode designed for ML / analytics pipelines.

**Outputs:**
- Stereo pairs over time
- Calibrated camera / eye metadata
- Window transforms
- Interaction traces
- Optional depth if available from app / pipeline

## Ownership Split

### Runtime owns
- Actual capture point in the pipeline
- Serialization of frame / recording / dataset outputs
- Timestamp integrity
- Metadata collection from compositor state
- Export formats

### Shell owns
- Hotkeys / buttons / menus
- Browse, preview, and share actions
- Policy UI around privacy / consent / opt-in

## MVP Requirements

- 3D screenshot (frame capture)
- Save L/R images
- Optional SBS export
- Basic metadata sidecar (timestamp, eye pose, display pose)

## Later Phases

- **Phase 2:** 3D recording (stereo video + metadata stream)
- **Phase 3:** Full session capture (replayable with pose scrubbing)
- **Phase 4:** Training-oriented dataset mode (opt-in, calibrated metadata)

## Open Questions

- **Image format:** PNG for lossless screenshots? EXR for HDR? WebP for compressed?
- **Video format:** Stereo MKV? Custom container with metadata track?
- **Compression:** Per-frame vs temporal compression trade-offs for dataset mode
- **Privacy policy hooks:** Per-app opt-out? Enterprise policy to disable capture for specific apps/workspaces?
- **Depth buffer capture:** Should the capture point also grab depth if available?
- **Capture notification:** How should apps be informed that capture is active? (consent indicator)
