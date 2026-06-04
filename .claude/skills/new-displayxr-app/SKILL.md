---
name: new-displayxr-app
description: |
  Scaffold a new DisplayXR OpenXR app from a known-good test app, wired to the authoring rules so it starts correct instead of tripping the common pitfalls. Clones the nearest cube_* reference app, parameterizes it, drops a workspace manifest + icon placeholders + a per-app CLAUDE.md, wires CMake, and runs the app linter until clean.
  Usage: /new-displayxr-app <name> [class=handle|texture|hosted] [api=d3d11|d3d12|gl|vk|metal] [platform=win|macos]
  Examples:
    /new-displayxr-app photo_viewer class=handle api=d3d11 platform=win
    /new-displayxr-app stereo_player class=handle api=vk platform=macos
    /new-displayxr-app editor class=texture api=d3d11 platform=win
---

# new-displayxr-app — scaffold a correct DisplayXR app

This skill consumes [`docs/guides/displayxr-app-rules.md`](../../../docs/guides/displayxr-app-rules.md)
(the authoring invariants) and `scripts/check_displayxr_app.py` (the linter). It exists because
agents writing a DisplayXR app from scratch reliably get the extension subtleties wrong
(view-count, swapchain tiling, color space, capture, manifest). The fastest way to be correct is
to **start from a known-good test app and adapt it**, not synthesize from first principles.

## When to use
The user wants to start a new DisplayXR OpenXR app (a test app under `test_apps/`, or a new
project). For a standalone *shipping* demo in its own repo, the canonical template is the
`displayxr-demo-gaussiansplat` layout (installer + README runtime-compat covenant + CI) — see
[INV-8 / §8 of the rules doc](../../../docs/guides/displayxr-app-rules.md); this skill scaffolds the
app itself, which is the same regardless of where it lives.

## Inputs
Resolve these; if any is missing or ambiguous, **ask the user** (AskUserQuestion) before scaffolding:
- **name** — snake_case app stem (e.g. `photo_viewer`). The exe/target/manifest derive from it.
- **class** — `handle` (owns its window; the default and simplest) | `texture` (weaved 3D in a
  canvas sub-rect + optional 2D surround) | `hosted` (runtime creates the window).
- **api** — `d3d11` | `d3d12` | `gl` | `vk` | `metal`.
- **platform** — `win` | `macos`.
- **location** — default `test_apps/<name>_<class>_<api>_<platform>/` (in-tree). Ask if the user
  wants it elsewhere / standalone.

Target name follows the convention `cube_*`-style: `<name>_<class>_<api>_<platform>`.

## Reference-app map (clone the nearest, then adapt)
Pick the exact match; if none exists, pick the same **class+platform** (swap API) or same
**class+api** (swap platform) and adapt, and tell the user which you used.

| class \ target | win | macos |
|---|---|---|
| **handle** | d3d11 → `cube_handle_d3d11_win` · d3d12 → `cube_handle_d3d12_win` · gl → `cube_handle_gl_win` · vk → `cube_handle_vk_win` | gl → `cube_handle_gl_macos` · metal → `cube_handle_metal_macos` · vk → `cube_handle_vk_macos` |
| **texture** | d3d11 → `cube_texture_d3d11_win` · d3d12 → `cube_texture_d3d12_win` | metal → `cube_texture_metal_macos` |
| **hosted** | d3d11 → `cube_hosted_d3d11_win` | metal → `cube_hosted_metal_macos` |

## Procedure

1. **Read the rules doc first** — `docs/guides/displayxr-app-rules.md`. Treat every `INV-*` and the
   `F-*` foundations as binding for the code you produce. Also read `§8` (folder layout) and `§9`
   (manifest) closely; you implement them here.

2. **Copy the reference app** into the target dir and rename. From the repo root:
   `cp -R test_apps/<reference> test_apps/<target>` (or into the chosen location). Then rename
   files and in-file identifiers from the reference stem to `<target>` (the `APP_NAME`/`#define`,
   window title, CMake `project()`/target name, `resource.rc` strings, `*.manifest` name, the
   `displayxr/<stem>.displayxr.json` filename). Do NOT rewrite the OpenXR/session/render plumbing —
   that's the point of cloning; it already satisfies the `F-*` foundations and the INVs.

3. **Manifest + icons** (per `§9`). Replace the copied `displayxr/<old>.displayxr.json` with the
   template in `templates/manifest.displayxr.json`, renamed to `<target>.displayxr.json`, and fill
   `name` / `description`. Keep the reference app's `icon.png` (512×512) and `icon_sbs.png`
   (1024×512) as **placeholders** (correct sizes already) and leave a `TODO: replace icon art`
   note — do not invent art. (`hosted`/`texture` test variants in the repo deliberately ship no
   manifest, but a real app should have one, so include it unless the user says otherwise.)

4. **Drop the per-app guidance file.** Copy `templates/app-CLAUDE.md` to `<target>/CLAUDE.md`
   (replace `<APP>` placeholders). This keeps the invariants + the linter command in front of any
   future agent that opens the app, even outside a runtime checkout.

5. **Wire CMake** (per `§8`/INV-8.1–8.4). The cloned `CMakeLists.txt` already finds the OpenXR
   loader, links `sr_common[_base]`, copies the loader DLL/assets, and calls
   `displayxr_install_manifest(<target> "${CMAKE_CURRENT_SOURCE_DIR}/displayxr")` — just retarget
   the names. If in-tree, add `add_subdirectory(<target>)` to `test_apps/CMakeLists.txt`.

6. **Lint until clean.** Run `python3 scripts/check_displayxr_app.py <target-dir>` and fix every
   ERROR (and address WARNs or explain them). Re-run until it passes. This is the gate — do not
   report done with outstanding errors.

7. **Report.** Summarize: which reference was cloned, what was renamed, manifest/icons status
   (placeholders to replace), how to build (`scripts/build_windows.bat test-apps` /
   `./scripts/build_macos.sh`) and run (the generated `run_*` script), and the concrete next steps
   (replace icon art; implement the app's actual scene/content in the render callback — the cube is
   a placeholder). Point the user at the rules doc for anything they extend.

## Guardrails
- **Reuse, don't reinvent** the session/frame loop — it lives in `test_apps/common/` (Windows) or
  the reference `main.mm` (macOS). Re-implementing it from scratch is how the `F-*` mistakes creep in.
- **Don't change the compositor or runtime** to make an app work — isolate app-side issues (this is
  a standing project rule). The app talks to the runtime only through the OpenXR + `XR_EXT_*` wire.
- **Color (INV-4.6):** the scaffold inherits the reference app's swapchain choice; if you change it,
  request an sRGB swapchain and store a correctly-encoded image — never double-encode.
- The linter is advisory on WARNs but **blocking on ERRORs**; treat a clean linter run as the
  definition of "scaffold complete."
