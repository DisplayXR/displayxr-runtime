# OpenXR CTS — the interactive categories, operator procedure

The OpenXR CTS splits into an **automated** half (`exclude:[interactive]`, which
`cts.yml` runs on every scheduled lane and every `v*` tag) and three
**human-evaluated** categories that no machine can score:

| Category | Catch2 spec | What the human does |
|---|---|---|
| composition | `[composition][interactive]` | Looks at the screen, compares it against a reference image the test itself can show, presses PASS or FAIL |
| scenario | `[scenario][interactive]` | Same, for whole-session scenarios rather than single layers |
| actions | `[actions][interactive]` | Presses/moves controller inputs on cue; the test asserts what the runtime reported |

A conformance submission needs, per the
[CTS usage guide](https://registry.khronos.org/OpenXR/conformance/cts_usage.html),
one **composition** result file per supported graphics API, at least one
**scenario** run, and one **actions** run *per supported interaction profile* —
on top of the automated runs. Tracking issue: **#1523** § 4.

**This page is the procedure, not the result.** It exists so an operator can
produce those result files without guessing. Nothing here has been executed
end to end yet; when it is, record the outcome on #1523.

---

## 1. Prerequisites

### 1.1 The box

- **Windows**, a real display, and a **non-elevated** shell. Elevation is not
  merely unnecessary — the bundled Khronos loader ignores `XR_RUNTIME_JSON` in
  an elevated process (CLAUDE.md § *Running without installing*), and these runs
  need you to be sure which runtime answered.
- `run_cts.ps1` sidesteps that by pointing `HKLM\Software\Khronos\OpenXR\1\
  ActiveRuntime` at the dev build and restoring it in a `finally` block, so the
  shell **does** need write access to HKLM. That is the one thing elevation
  buys; the loader caveat above is why you should still confirm what loaded
  (§1.4).

### 1.2 The runtime build

Build from the worktree per CLAUDE.md:

```bat
scripts\build_windows.bat all
```

`run_cts.ps1` hard-requires `build\Release\openxr_displayxr-dev.json` and throws
if it is missing. Use a **RelWithDebInfo/Release** build — a Debug build's frame
timing is different enough to change what a flicker test looks like.

### 1.3 The CTS itself

```bat
scripts\fetch_build_cts.bat
```

Pinned at **`openxr-cts-1.1.63.0`** (`CTS_TAG` in that script, #1487). The
script also embeds and asserts a per-monitor-DPI manifest on
`conformance_cli.exe` — the Khronos binary ships without one, and a DPI-unaware
CTS reads every window rect, Kooima projection and view pose scaled wrong
(#1506, [DPI awareness](dpi-awareness.md) § *The CTS runner is third-party*).
**If you build the CTS by hand, you lose that and every geometric judgement in
this document is void.**

**Verify the manifest is actually there before every interactive run — this is
not optional, and a green-looking build does not prove it.** A
`conformance_cli.exe` left on the box from *before* `fetch_build_cts.bat` grew
the embed step is DPI-unaware and will be reused silently:

```bat
mt.exe -inputresource:conformance_cli.exe;#1 -out:m.xml
findstr /i PerMonitorV2 m.xml
```

`PerMonitorV2` must be present. (`mt.exe` ships with the Windows SDK; it is on
the path in a *Developer Command Prompt for VS 2022*.)

**What it looks like when it is missing**, so you recognise it instead of
filing it: the entire CTS scene renders at **1536×864 in the corner of the
window**, with all text far too small to read. That is the DPI scale factor
applied twice, not a compositor defect. It happened on the win box on
2026-09-19 and cost a full composition pass. If you see it, stop, rebuild the
CTS with `fetch_build_cts.bat`, re-check with `mt.exe`, and start the run over.

### 1.4 A clean, verified loader chain

Before *and* after every run:

```bat
displayxr-cli runtime status
```

This resolves the full loader precedence chain (`XR_RUNTIME_JSON` → HKCU → HKLM,
× 64-bit/WOW6432Node) and flags each conflict. It matters twice over here:

- **Before** — a stale `ActiveRuntime` from an aborted earlier run means you may
  be testing the *installed* runtime, not the dev build, and the result file
  would be attributed to the wrong binary.
- **After** — an interactive run is long and gets aborted. `run_cts.ps1`
  restores the registry in a `finally` block, but Ctrl+C (and a harness
  `TaskStop`) can tear the PowerShell pipeline down without running it. If
  `status` is not clean afterwards, re-point with
  `displayxr-cli runtime activate <manifest>`.

Then confirm which DLL actually answered. Every `xrCreateInstance` logs a WARN
line near the top of
`%LOCALAPPDATA%\DisplayXR\DisplayXR_conformance_cli.<pid>_<ts>.log`; search for
`loaded from:`. Put that path in the run record.

### 1.5 The conformance layer

Pass `-ConformanceLayer`. It registers the Khronos
`XR_APILAYER_KHRONOS_runtime_conformance` validation layer (and the
`conformance_test_layer`) in HKLM and requests it with `-L`. A submission is not
valid without it.

> **Known open layer warning — expect it, do not re-diagnose it.** On the
> composition category the layer currently warns on **every sub-case**:
>
> ```
> XrEventDataSessionStateChanged: Suspicious session state transition to
> XR_SESSION_STATE_SYNCHRONIZED when no frame(s) have been submitted and
> session has not requested an exit.
> ```
>
> This is **submission-relevant**, not cosmetic: the usage guide requires every
> warning to be acceptably explained, and an unexplained one invalidates the
> package. It is recorded in **#1580** and must be resolved or explained before
> anything is submitted. Seeing it is not a reason to abort a practice run —
> it is a reason not to call that run a submission.

---

## 2. Which display processor — and why the two passes are different evidence

Run this **twice**, and submit only one of them.

### 2.1 The sim-display pass — this is the submission

`sim_display` is the runtime's own vendor-neutral reference display processor.
In its `2d` (passthrough) output mode it puts one view flat and full-screen on
an ordinary monitor. **Every CTS reference image is a flat 2D screenshot**, so
this is the only configuration in which "does the screen match the reference?"
is a question with a defined answer. It is also hardware-free and therefore
reproducible by anyone reviewing the submission.

There is direct precedent for a simulation-device submission being a conformant
product in its own right: Collabora's *"Monado: Simulation Device"* has been on
the Khronos conformant-products list since 2021-02-27 (Ubuntu 20.10 / x86-64 /
OpenGL + Vulkan). A hardware-free matrix is a legitimate target, not a
rehearsal — see #1523 § *Why*.

### 2.2 The vendor-plug-in pass on a lenticular panel — product evidence, not conformance evidence

Running the same categories with the vendor display processor against a real
lenticular panel is worth doing, and it is **not** what you submit. The panel
interlaces the views, so what reaches your eye is neither view: a judgement like
"the gradient banded" cannot be separated from "the weave resampled it". Running
it proves the frames reach a real panel and catches plug-in regressions; file
any delta as a plug-in issue against the vendor's own repo.

The conformance claim is about the **runtime's** OpenXR behaviour. The CTS
contains no `XR_DXR_*` tests and never sees the display processor's pixels
through the API. Judging it through a weave adds a variable the claim does not
cover.

### 2.3 Set the sim-display output mode deliberately — the default will ruin the run

`SIM_DISPLAY_OUTPUT` is read at device create
(`src/xrt/drivers/sim_display/sim_display_device.c:539-556`):

| Value | Mode | Use for |
|---|---|---|
| `2d` / `passthrough` | first tile fills the screen, flat, 1 view | **everything except eye-visibility** |
| `sbs` | side-by-side, centre-cropped, 2 views | the `EyeVisibility`-shaped tests only |
| `squeezed` / `squeezed_sbs` | SBS, no crop, tiles as-is | rarely — cross-check a tiling doubt |
| `anaglyph` | red/cyan, 2 views | **never for these runs** |
| `blend` | 50/50 alpha blend of the views | never |
| `quad` | 2×2, 4 views | **never** — see below |

> **The default is `anaglyph`.** If `SIM_DISPLAY_OUTPUT` is unset the DP falls
> back to red/cyan (the driver says so in its own warning text: *"keeping the
> anaglyph default"*). Anaglyph destroys every colour judgement in the
> composition set — `GradientFormatsLinearVsNonLinear` and the alpha-blending
> tests become meaningless, and you would not necessarily notice, because a
> red/cyan gradient still looks like *a* gradient. **Always set it explicitly.**

```powershell
$env:SIM_DISPLAY_OUTPUT = "2d"     # PowerShell sets the process env block;
                                   # Start-Process inherits it.
```

**Never `quad`.** A CTS session is a *legacy* session (§3), so it can submit at
most 2 views; the #1499 mode floor would demote a 4-view mode anyway, and
pinning it past the floor with `SIM_DISPLAY_FORCE_MODE` makes the lane fail —
see [View-Configuration Model](view-configuration-model.md). `2d` and `sbs` both
**stick**: `oxr_pick_fillable_mode_index()` returns the active index untouched
whenever `view_count <= max_views`
(`src/xrt/state_trackers/oxr/oxr_legacy_mode_rule.h`), and 1 and 2 both are.

---

## 3. What kind of session the CTS actually gets

The CTS enables no `XR_DXR_*` extension, so it is a **legacy** session:

- `max_views` is a fixed **2** (`OXR_LEGACY_MAX_SUBMITTED_VIEWS`).
- It enumerates only `PRIMARY_STEREO`, which reports exactly 2 views (#1486).
- The display is moved to a fillable (≤ 2-view) mode at `xrBeginSession`.
- It gets the V (2D/3D) toggle but never the 1/2/3 direct-mode keys.

Full contract: [View-Configuration Model](view-configuration-model.md). Nothing
in this document should ever see a 4-view atlas; if you do, the mode is pinned
and the run is invalid.

---

## 4. Input — every button you press is qwerty

### 4.1 There is exactly one input device

`sim_display` claims only the **head** role. The only thing supplying
`/user/hand/left` and `/user/hand/right` in a CTS session is the **qwerty**
keyboard/mouse driver, added unconditionally by
`src/xrt/targets/common/target_builder_sim_display.c:164`. There is no real
controller anywhere in this picture, and no input-provider plug-in is installed
on a conformance box.

### 4.2 Which interaction profiles the runtime can bind

Qwerty's primary device name is `XRT_DEVICE_WMR_CONTROLLER`, plus five
`binding_profiles` remap tables (`src/xrt/drivers/qwerty/qwerty_device.c:189-226`)
that `oxr_find_profile_for_device()`
(`src/xrt/state_trackers/oxr/oxr_binding.c:486-530`) walks when the app's
suggested profile is not the primary name. That makes these five, and only
these five, bindable:

| Interaction profile | Backed by | Submit? |
|---|---|---|
| `/interaction_profiles/khr/simple_controller` | `simple_inputs[4]` → WMR trigger/menu/grip/aim | **Yes — this is the defensible one** |
| `/interaction_profiles/microsoft/motion_controller` | `wmr_inputs[11]` (identity — this *is* the device) | Optional |
| `/interaction_profiles/oculus/touch_controller` | `touch_inputs[…]` remap | Optional |
| `/interaction_profiles/valve/index_controller` | `index_inputs[…]` remap | Optional |
| `/interaction_profiles/htc/vive_controller` | `vive_inputs[…]` remap | Optional, and **incomplete** (§4.5) |

`/interaction_profiles/khr/generic_controller` is **not** in `bindings.json` and
is not bindable.

> **Submission scope: `khr/simple_controller` only.**
> The usage guide asks for one actions run per *supported* interaction profile.
> There is no real controller on this box, and the other four profiles are
> **masquerades** — one WMR-shaped qwerty device reporting someone else's
> profile path through a remap table (the mechanism is documented as exactly
> that, "profile stability (masquerade)", in
> [`docs/specs/runtime/input-provider-discovery.md`](../specs/runtime/input-provider-discovery.md)).
> Claiming conformance for `oculus/touch_controller` on that basis would be
> claiming support for hardware the product does not have. Run the other four if
> you want the coverage — keep the XML, note it as informational — but submit
> `khr/simple_controller`.

### 4.3 The window, and getting keyboard focus into it

`run_cts.ps1` sets `XRT_COMPOSITOR_START_WINDOWED=true`. The CTS app has no
window of its own, so the native compositor self-creates one titled
**"DisplayXR — D3D11 Native Compositor"**.

That title is correct on **all five** Windows graphics plugins, not just D3D11:
`d3d12`, `gl` and `vk_native` all call `comp_d3d11_window_create()` for the
hosted window. It is also why keyboard input works on all of them —
`qwerty_process_win32()` is pumped straight from that WndProc
(`src/xrt/compositor/d3d11/comp_d3d11_window.cpp:852, 997, 1047, 1400`), and
qwerty is enabled and active by default on a runtime-owned window
(`:1760-1761`). The only thing that ever deactivates it is the **D3D11 service**
compositor arbitrating focus between multiple clients
(`comp_d3d11_service.cpp:12467, 12474`) — the CTS runs **in-process**, with no
`XRT_FORCE_MODE=ipc` and no shell, so that path is never taken.

**Click the compositor window once before the first prompt.** Keystrokes go to
the foreground window; if focus is still on your terminal, nothing you press
reaches the runtime and a composition test will sit on its prompt forever while
an actions test silently burns its timeout.

### 4.4 The key map (source of truth: `qwerty_win32.c`)

Focus is modal, but **the modifier does not gate the controller buttons — it
only redirects them.** `qwerty_process_win32()` resolves *two* independent
targets each event, and this is the distinction that matters:

| Modifier held | Movement / pose keys go to (`targets[]`) | Controller **buttons** go to (`ctrl_targets[]`) |
|---|---|---|
| *(none)* | the qwerty **HMD** (`default_qdev`) | the **right controller** (`default_qctrl`) |
| **CTRL** | left controller | **left** controller |
| **ALT** | right controller | **right** controller |
| **CTRL + ALT** | both controllers | **both** controllers |

With no modifier, `default_qdev` is the HMD but `default_qctrl` is resolved
*separately* and is never the HMD — it is the right controller, by role or by
fallback (`qwerty_win32.c`, `default_qwerty_controller()`). So **a bare
left-click still fires a controller trigger and a bare `N` still fires a
controller menu.** Only WASD/arrows/RMB-look change target with the modifier.

Every modifier change calls `qwerty_release_all()` on all devices, so a button
never "sticks" on the hand you just left.

| Input | Action on the targeted controller | OpenXR source path (simple profile) |
|---|---|---|
| **Left mouse button** | Trigger | `…/input/select/click` |
| **N** | Menu | `…/input/menu/click` |
| Middle mouse button | Squeeze / grip | *(not in the simple profile)* |
| **B** | System / home | *(not in the simple profile)* |
| T / F / G / H | Thumbstick **and** trackpad up / left / down / right | *(not in the simple profile)* |
| V | Thumbstick click *(when a controller is focused)* | — |
| RMB + drag | Rotate the focused device | grip + aim pose |
| Mouse move (with CTRL/ALT held) | Translate the focused controller in XY | grip + aim pose |
| R | Reset controller pose — both, or the focused one with a modifier | — |
| C | Toggle controller-follows-HMD parenting | — |
| W A S D / Q E | Move the focused device | head or grip/aim pose |
| Arrow keys | Rotate the focused device — pitch (up/down), yaw (left/right) | — |
| **Z / X** | **Roll** the focused device left / right — the third rotation axis (#1692) | grip + aim pose |
| SHIFT | Sprint — 3× on **both** movement and look speed (#1692) | — |
| Numpad + / − | Movement speed up / down | — |
| TAB | Toggle the runtime HUD | — |

**For the composition and scenario categories, use no modifier at all:**

| You want | Press |
|---|---|
| **Select** (= PASS) | **LMB** |
| **Menu** (= show description + reference image) | **N**, held |
| **FAIL** | hold **N**, click **LMB** while still holding, then release |

That is the whole control set for judging a composition test. The CTS binds
select and menu on **both** hands, so the bare (right-hand) press satisfies
either binding — there is nothing to aim.

**When the modifier does matter: the `[actions]` category.** Those tests name a
specific top-level user path and wait for input *on that hand*, so there you
must target explicitly — hold **CTRL** for `/user/hand/left`, **ALT** for
`/user/hand/right` (§8.5). Elsewhere the modifier is noise, and worse than
noise on a laptop (see the trackpad note below).

Full table, including the HMD-focused stereo controls:
[Qwerty Device Driver](qwerty-device.md) § 6.

> **Laptop trackpads.** Windows Precision Touchpad drivers suppress
> `WM_LBUTTONDOWN`/`UP` while CTRL or ALT is held (palm rejection) — exactly the
> combination this procedure needs. The Win32 backend works around it by reading
> `MK_LBUTTON` out of `WM_MOUSEMOVE`, which means **you may have to jiggle the
> pointer for a trackpad "click" to register**. Use a real mouse.

### 4.5 What qwerty cannot do, and what that costs

| Gap | Consequence |
|---|---|
| **No haptics you can feel.** `qwerty_set_output()` (`qwerty_device.c:330`) only emits a `QWERTY_INFO` log line. | Any test that says "you should feel a vibration" is judged from the **log**, not the hand. Watch for `Haptic output: frequency=… amplitude=… duration=…` in the console log. Its presence is the pass; the absence of a buzz is not a failure. |
| **No trackpad click.** `qwerty_press_trackpad_click()` exists in the driver API but no backend binds a key to it. | `/interaction_profiles/htc/vive_controller`'s `…/input/trackpad/click` is unreachable. That profile cannot be completely exercised — another reason it is not in the submission scope. |
| **Thumbstick and trackpad share T/F/G/H.** | You cannot move one without the other. Harmless for `khr/simple_controller`, which has neither. |
| **Poses are keyboard-driven, not tracked.** | "Hold the controller still" prompts are satisfied by *not pressing anything* (§7.3). |

---

## 5. Running a category

One command shape for all three. `-Interactive` sets the test spec, disables the
timeout, and names the outputs the way a submission package wants them:

```powershell
# composition — repeat per graphics plugin
$env:SIM_DISPLAY_OUTPUT = "2d"
.\scripts\run_cts.ps1 -Interactive composition -Graphics d3d11 -ConformanceLayer

# scenario — once per platform is enough
.\scripts\run_cts.ps1 -Interactive scenario    -Graphics d3d11 -ConformanceLayer

# actions — once per interaction profile
.\scripts\run_cts.ps1 -Interactive actions     -Graphics d3d11 -ConformanceLayer `
    -InteractionProfile khr/simple_controller `
    -ExtraCliArgs --nonDisconnectableDevices
```

Substitute `-Graphics d3d12 | opengl | vulkan | vulkan2` for the other Windows
plugins. Nothing about this runs in CI: `cts.yml` never sets `-Interactive`,
because every one of these needs a human at a display.

> **`-TimeoutSec` is forced to 0 (no timeout) by `-Interactive`.** The 1800 s
> default would kill a hand-paced run partway and leave a **truncated XML that
> still looks like a result file**. If you override `-TimeoutSec`, you own that
> risk.

> **`-I` takes the SHORT form — `khr/simple_controller`, with no
> `/interaction_profiles/` prefix — and a full path silently enables nothing.**
> The CTS compares `-I` case-insensitively against a shortname it builds by
> stripping exactly that prefix, and it never normalises the other way, so
> `/interaction_profiles/khr/simple_controller` matches no profile, enables no
> profile, and **skips every profile-gated `[actions]` test without erroring**.
> `conformance_cli`'s own `-I` help text is misleading on this point; the CTS
> usage guide (`usage/configuration.adoc`) is the authority. `run_cts.ps1`
> accepts either spelling and strips the prefix for you — but if you ever call
> `conformance_cli` directly, use the short form.
>
> **Always pass `-I` explicitly, even for the default profile.** The CTS injects
> its `khr/simple_controller` default *after* it snapshots the options it writes
> into the report, so on a default run the result XML's
> `<cts:enabledInteractionProfiles>` element comes out **empty** and the file
> does not record which profile was tested. `run_cts.ps1` only emits `-I` when
> you give it one.
>
> The long spelling is `--interactionProfiles` (plural) and is repeatable, but a
> submission wants one result file per profile, so one profile per run is right.

### 5.1 The Select / Menu contract every composition test uses

Composition tests are driven by the CTS's `InteractiveLayerManager`
(`src/conformance/conformance_test/composition_utils.h`). It binds **select**
and **menu** on `/interaction_profiles/khr/simple_controller`, **on both
hands**, so either hand works:

- The test renders its content plus a prompt: *"Press Select to PASS. Press Menu
  for description"*.
- **Hold Menu** → the overlay switches to the test's description and its
  **reference image**, and the prompt flips to *"Press Select to FAIL"*.
- Releasing Menu returns to the content and the PASS prompt.

So: **Select while looking at the content = PASS. Hold Menu, then Select =
FAIL.** In this runtime's keys, **with no modifier held**:

| | Keys |
|---|---|
| PASS | click **LMB** |
| See the description + reference image | hold **N** |
| FAIL | hold **N**, click **LMB**, release **N** |

Do **not** hold CTRL or ALT for this. A bare press already reaches a controller
(§4.4), the CTS binds select/menu on both hands so it does not matter which,
and holding a modifier while clicking is what triggers the Precision-Touchpad
palm-rejection path — i.e. adding the modifier can only make the click *less*
likely to register.

There is no way to skip a test from inside the CTS. Judge it.

---

## 6. `[composition][interactive]` — what to look at, per test

> Counts and names below are from the pinned tag **`openxr-cts-1.1.63.0`**.
> Re-derive them if the pin moves.

> ### ⚠ This category is runnable on `d3d11`, `d3d12` and `opengl` (2026-09-23)
>
> **#1581 — quad layers are accepted but never rendered on Vulkan and
> Vulkan2** on `main`. Those renderers filter to projection / projection-depth
> / zone layers and drop quads on the floor. Every CTS composition test puts
> its **prompt, its labels and its reference image in quad layers**, so on
> those plug-ins the operator sees no prompt, no labels and no quad content —
> the category is not merely failing there, it is **unjudgeable**. The
> `vk_native` leg is #1623 (quads drawn through the shared camera / cull /
> painter's rules, plus the #1589/#1610 colour model on a private `_SRGB`-view
> target); a Windows `-Graphics vulkan` hardware run of that branch passed
> QuadOcclusion, the gradients (13/13), SourceAlphaBlending and the
> environment-blend pair. Equirect2 on `vk_native` is a separate change, and
> `Subimage` / `MinLayers` have not yet been judged on Vulkan. A run that
> produces no visible prompt is a harness gap, not a result.
>
> **`d3d11`** is the reference lane: 0 runtime-attributable failures on `main`
> since #1606 (gradients 11/11, SourceAlphaBlending, the environment-blend
> pair; `QuadHands` needs the qwerty hands raised first — §10.6).
> **`d3d12`** joined it with #1689 (quad drawing, per-view cameras, painter's
> order, the SRV heap sized for `XRT_MAX_LAYERS × XRT_MAX_VIEWS`), #1694 (the
> same private `_SRGB`-view compose model as D3D11) and #1695 (equirect2) —
> every judgeable case passes, matching d3d11 case for case.
> **`opengl`** joined with #1704 (quads drawn through the shared rules; the GL
> texture-origin flip; the #1700 phantom-select fix), #1705 (the compose model
> on a private `GL_SRGB8_ALPHA8` target) and #1706 (equirect2): every judgeable
> case passes there too, **provided the CTS carries the GL-plugin patch
> below** for the gradients. On the GL and Vulkan lanes the hosted view fills
> the window's client area where D3D's letterboxes ~40 px top and bottom, so
> a GL/VK frame is never pixel-identical to a D3D one — compare content, not
> pixels. #1580 (projection content displaced relative to quads) is resolved
> on all three lanes.
>
> **`opengl` needs a patched CTS for `GradientFormatsLinearVsNonLinear`.** The
> CTS's own GL plugin (`framework/graphics_plugin_opengl.cpp`, `RenderView` and
> `ClearImageSlice`) renders into the swapchain through an FBO without ever
> enabling `GL_FRAMEBUFFER_SRGB`, so its "sRGB" swapchain holds the shader's
> *linear* bytes — bit-identical to its UNORM swapchain (measured through the
> `DXR_COLOR_LEGACY_UNORM_ENCODED=1` passthrough: 7/29/57/97/170 at five x in
> both). D3D/Vulkan `_SRGB` views encode on write unconditionally, which is why
> the same CTS code is right there. No format-honest runtime can render two
> swapchains with the same bytes alike, so on stock CTS every UNORM-vs-sRGB
> pair fails on `opengl` (0/7; the sRGB/sRGB pair reads "identical but NOT
> linear") and always did. Upstream half-knows (`SelectColorSwapchainFormat`:
> *"sRGB formats skipped due to CTS bug"*). The local fix is two brackets in
> those two functions, gated on the colour attachment being `GL_SRGB8_ALPHA8` /
> `GL_SRGB8`:
>
> ```cpp
> const int64_t fmt = swapchainData->GetCreateInfo().format;
> const bool srgbTarget = (fmt == GL_SRGB8_ALPHA8 || fmt == GL_SRGB8);
> if (srgbTarget) { XRC_CHECK_THROW_GLCMD(glEnable(GL_FRAMEBUFFER_SRGB)); }
> … existing viewport/scissor/clear or draw …
> if (srgbTarget) { XRC_CHECK_THROW_GLCMD(glDisable(GL_FRAMEBUFFER_SRGB)); }
> ```
>
> With it: 7/7 (PR #1705). It changes nothing for the D3D/Vulkan plugins.
> `fetch_build_cts.bat` re-fetches the pinned tag and drops the patch; check
> `git -C build-cts/OpenXR-CTS diff`. **Rebuild trap:** `conformance_cli` loads
> the `conformance_test.dll` *next to the exe*
> (`build-cts/build/src/conformance/conformance_cli/RelWithDebInfo/`), which
> `cmake --build` does not refresh — copy it from
> `…/conformance_test/RelWithDebInfo/` after the build, and build with the real
> ninja first on `PATH` (depot_tools' `ninja.bat` exits 0 doing nothing outside a
> Chromium checkout).
>
> **Scenario cases are not composition cases:** their Menu key is not "Help" —
> `SpaceOffsets` treats Menu as FAIL and `GripAndAimPose` as "swap hands", so
> capture them without the Help step. `GripAndAimPose` / `SpaceOffsets` /
> `InteractiveThrow` un-skip since #1693 (`trackingProperties` now reflects the
> live role devices); the latter two cannot auto-pass on the qwerty rig until
> the controllers report `XrSpaceVelocity` (#1692).

**28 tests carry `[composition][interactive]`** at the pin. Ten of them skip on
this runtime because the extension or view configuration they need is not
advertised — a skip is an *allowed* result code, not a failure, but **record
each one** so a reviewer can see why your XML has fewer results than someone
else's.

Several tests expand into Catch2 sub-cases, so 28 test cases is **not** 28
prompts. Budget accordingly.

### 6.1 The 18 that run

| Test | What you should see | Pass rule |
|---|---|---|
| `GradientFormatsLinearVsNonLinear` | Two gradient rectangles — the upper one drawn through a **projection** layer, the lower through a **quad** layer — labelled with the swapchain format name. | Both gradients match each other *and* the reference, and read as perceptually linear. Banding is allowed and expected; a **gamma-shaped** difference is not. See §10.4. |
| `QuadOcclusion` | Blue and green quads at Z = −2 rotated oppositely about Y, forming an X. A red quad faces away. | Green fully visible (painter's algorithm), **red never visible**. |
| `QuadProjectionQuad` | Three squares — blue, green, yellow — each continuing hidden under the next. Green is a transparent **projection** layer. | Three squares in that stacking order. Green may **jitter** relative to the others; that is called out in the test's own description and is not a failure. |
| `ProjectionQuadProjection` | Same sandwich, but blue and yellow are projection layers and green is the quad. | As above; blue/yellow may jitter. |
| `QuadPoses` | Two quad pairs. Blue/green rotate about Z on the `XrSpace` then translate via the layer pose; orange/yellow do it the other way round. | Order of operations matches the reference — the two pairs land in different places, and that difference is the point. |
| `MultipleMutableProjections` | Four coloured squares, one per display quadrant, via mutable FoV. | Same colour-to-quadrant mapping as the reference. |
| `SourceAlphaBlending` | Three squares, each a blue-green gradient: one pre-combined "source of truth", one premultiplied pair, one unpremultiplied pair. | **All three identical.** Judge in `2d` only — §10.4. |
| `SourceAlphaBlendingWithEnvironment` | Left column opaque black + white squares; right column semi-transparent black + white. One sub-case per supported environment blend mode. | On a black background the black square is invisible — expected. **Red must never be visible.** |
| `EyeVisibility` | A **green** quad at view-space `{-1, 0, -2}` marked `XR_EYE_VISIBILITY_LEFT` and a **blue** quad at `{+1, 0, -2}` marked `…_RIGHT`. | See §10.2 — this is the one test that needs `SIM_DISPLAY_OUTPUT=sbs`. |
| `Subimage` | A 6×2 grid of quad layers exercising `subImage` array index and `imageRect`. | Red not visible except minor bleed at the edges. |
| `ProjectionArraySwapchain` | One texture **array** for the projection layer; each view is a differently-coloured slice. Sub-cases: with / without depth submission. | Matches the reference; each eye gets its own slice colour. |
| `ProjectionWideSwapchain` | One **wide** texture for the projection layer. Sub-cases: with / without depth. | As above. |
| `ProjectionSeparateSwapchains` | A **separate** texture per projection view. Sub-cases: with / without depth. | As above. |
| **`MinLayers`** | Three sections, each preceded by an **instruction screen**, then a scene with `XR_MIN_COMPOSITION_LAYERS_SUPPORTED` layers: N projection layers (one cube each), N quad layers, then a mixed split. | **Inverted controls — read §6.3 before running this one.** Count the cubes/quads; the count must equal the number the instruction screen named. |
| `QuadHands` | 10 cm cubes at each controller grip pose, with 10×10 cm quads labelled **L** and **R** 10 cm along each grip's +Z. | Quads face you and are upright when the controllers are thumbs-up pointing into the screen; correctly **backface-culled**; **R always drawn atop L**, both atop the cubes. Needs **both hands** — §10.6. |
| `ProjectionMutableFieldOfView` | Mutable FoV per projection view. Sub-cases: with / without depth. | Matches the reference. Skips itself if the view configuration reports `fovMutable == false`. |
| **`StaleSwapchain`** | Two 2 cm squares in view space. The **left** is written green once a second, every second. The **right** alternates green ↔ blue at 1 Hz. | **The left square must stay constantly green.** Any flicker on it — especially synchronised with the right square changing colour — is a failure. §10.3 first. |
| `XR_KHR_composition_layer_equirect2-interactive` | A 360° view of the inside of a test cube, six sub-cases (§10.7). Titles read `Equirect2 layer: subtest N of 6`. | Each sub-case matches its reference. |

### 6.2 The 10 that skip here, and why

| Test | Why it skips |
|---|---|
| `ProjectionDepth` | Tagged `[XR_KHR_composition_layer_depth][XR_FB_composition_layer_depth_test]`; `XRT_FEATURE_OPENXR_LAYER_DEPTH` is **OFF**. |
| `XR_EXT_composition_layer_inverted_alpha` | Extension not advertised. |
| `XR_FB_composition_layer_image_layout` | Extension not advertised. |
| `XR_KHR_android_surface_swapchain-interactive` | Android-only extension. |
| `XR_KHR_composition_layer_equirect-interactive` | `XRT_FEATURE_OPENXR_LAYER_EQUIRECT1` is **OFF** — §10.7. |
| `XR_KHR_visibility_mask-interactive` | Advertised, but the runtime returns an empty mask, and the test skips itself: `"No vertices returned, so no visibility mask available in this system."` — §10.5. |
| `XR_VARJO_quad_views-interactive` | `XR_VARJO_quad_views` not advertised. |
| `StereoWithFoveatedInset-interactive` | Forces `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET`, which this runtime does not enumerate (it offers `PRIMARY_STEREO` and `PRIMARY_MULTIVIEW_DXR`). |
| `XR_VARJO_quad_views-mutableFoV-interactive` | As above, plus `[no_auto]`. |
| `StereoWithFoveatedInset-mutableFoV-interactive` | As above. |

> `MaxLayers-noninteractive` (`test_LayerComposition.cpp:1184`) is registered
> with an **empty** tag string. It is not selected by `[composition][interactive]`
> and belongs to the automated lane. Don't go looking for it here.

### 6.3 Two tests that break the normal Select/Menu contract

**`MinLayers` inverts the buttons, and it is the easiest test in the set to fail
by accident.** It does not use `InteractiveLayerManager`; it drives
`InteractionManager` directly and renders its own instruction quads. There:

- **Select advances** — it dismisses the instruction screen, and later passes the
  scene.
- **Menu FAILS** the test outright (`"User failed the test by pressing MENU"`).

So the habit you build on the other 17 tests — *hold Menu to see the reference* —
**fails `MinLayers` instantly**. Do not press Menu during it.

**`GradientFormatsLinearVsNonLinear` has a runtime-dependent number of
sub-cases.** It generates one section per enumerated swapchain colour format
that supports rendering and is not an integer format, each with a "Custom
projection layer format" and a "Custom quad layer format" variant. On a
D3D11/Vulkan runtime that is easily dozens of prompts. It is not stuck; it is
iterating formats. The format under test is printed on screen.

### 6.4 Two behaviours of the help overlay worth knowing

- **The help quads stop following your head.** On the first frame you hold Menu,
  the description and reference-image quads are re-anchored from VIEW space into
  LOCAL space, deliberately, so you can read them without them tracking. They
  will appear to "stick" in the world. That is correct.
- **`Example Not Available`** in red on the right-hand quad means the reference
  screenshot file is missing from the CTS build, not that the runtime did
  anything wrong. Fix the CTS build (re-run `fetch_build_cts.bat`) rather than
  judging the test blind.

---

## 7. `[scenario][interactive]` — the core set

**49 tests carry `[scenario][interactive]`** at the pin, and **41 of them are
gated on extensions this runtime does not advertise** — eye gaze, hand tracking,
plane detection, spatial anchors/markers/persistence, render models,
`XR_FB_hand_tracking_mesh`, `XR_MSFT_controller_model`,
`XR_EXT_view_configuration_views_change`, `XR_EXT_haptic_parametric`,
`XR_EXT_dpad_binding`. They skip.

The usage guide asks for **one** scenario run, on **one** graphics API, not one
per API: *"a conformance submission only requires a report showing an overall
pass on a single API."* Do it on `d3d11`.

### 7.1 The core set — what actually runs

| Test | Gate | What the operator does |
|---|---|---|
| `GripAndAimPose` | Core. Skips if the system reports no orientation/position tracking. | Move a controller (hold CTRL or ALT, drag with RMB / move the mouse) and confirm the grip and aim poses behave as the instructions describe. |
| `HapticInterrupt` | **Core, no guard at all.** | Haptics you cannot feel — see §7.3. |
| `InteractiveThrow` | Core + tracking. | Move a controller and release; judge the reported motion. Needs the reported velocity — **§10.8**. |
| `SpaceOffsets` | Core + tracking. | **Auto-passes on velocity, not on looks** — drive all six axes per **§10.8**. Menu is FAIL. |
| `local_floor-local` | `FeatureSet{XR_VERSION_1_1}` — satisfied because `run_cts.ps1` passes `--minApiVersion 1.1`. | Confirm the local-floor space behaves as described. |
| `XR_EXT_local_floor-local` | `FeatureSet{XR_VERSION_1_0, XR_EXT_local_floor}` — `XRT_FEATURE_OPENXR_SPACE_LOCAL_FLOOR` is **ON**. | As above, through the extension rather than 1.1 core. |

That is the **six** to plan for.

### 7.2 Four more that may or may not run — check, don't assume

| Test | The open question |
|---|---|
| `local_floor-stage`, `XR_EXT_local_floor-stage` | Both additionally need `XR_REFERENCE_SPACE_TYPE_STAGE`, which this runtime enumerates only when the device supplies a **stage** semantic space (`oxr_system.c:461-463`). The sim display almost certainly does not, in which case the tests skip themselves with `"XR_REFERENCE_SPACE_TYPE_STAGE not supported"`. That skip is fine; record it. |
| `GripSurface`, `GripSurface-XR_KHR_maintenance1` | These want `…/input/grip_surface/pose`. `grip_surface` **is** present in `src/xrt/auxiliary/bindings/bindings.json` as a virtual profile extending all five controller profiles, and since #1633 the qwerty controllers also **supply** `XRT_INPUT_GENERIC_PALM_POSE` (derived from grip by `u_grip_surface_from_grip()`), so the action space is locatable. Before that they did not, and the binding resolved to nothing — which is what `GripSurface-objective` caught. **If one of these fails, that is a defect to file against the runtime, not a judgement call.** Do not mark it FAIL-by-inspection; capture the output and open an issue. |

Note that `XR_EXT_palm_pose` itself is **OFF**
(`XRT_FEATURE_OPENXR_INTERACTION_EXT_PALM_POSE`), so the `XR_EXT_palm_pose`
scenario test and its `-objective` action siblings skip.

### 7.3 `HapticInterrupt` — the one test with no extension gate and no way to feel it

`HapticInterrupt` has **no** extension check, **no** tracking check and **no**
graphics check. It will run on any configuration, and it asks you to confirm
haptic feedback.

Qwerty's `qwerty_set_output()` only writes a `QWERTY_INFO` log line
(`qwerty_device.c:330`) — there is no motor. **Judge it from the console log.**
Watch `%TEMP%\interactive_scenario_<graphics>_console.log` (and the runtime log)
for lines of the form:

```
[…] Haptic output:
	frequency=… amplitude=… duration=…
```

The runtime having *received and acted on* the haptic call is what the test is
actually about at the API level. Its presence, with the right
timing/interruption pattern the test describes, is the pass. **The absence of a
physical buzz is not a failure and must not be recorded as one.**

If the log lines are missing, that *is* a failure — the runtime did not route
the haptic output to the device at all.

### 7.4 A CTS guard defect to be aware of

`XR_EXT_interaction_render_model-objective` checks only for
`XR_EXT_render_model` but its instance request also asks for
`XR_EXT_interaction_render_model`. On a runtime that has the first and not the
second, it **fails with `XR_ERROR_EXTENSION_NOT_PRESENT` instead of skipping.**
We advertise neither, so it skips cleanly here and this cannot bite us — but if
a future runtime picks up `XR_EXT_render_model` alone, this test will go red for
a CTS bug, not a runtime one. Recorded so nobody re-derives it under time
pressure.

---

## 8. `[actions][interactive]` — the loop, and the trap

**25 tests carry `[actions][interactive]`** at the pin. This category behaves
unlike the other two and has the single worst failure mode in the whole
procedure.

### 8.1 There is no pass button

The `[actions]` tests do **not** use `InteractiveLayerManager`. They use
`ActionLayerManager`, which only *displays instruction text* on a view-locked
quad. Pass and fail are **objective** — `REQUIRE`/`CHECK` assertions on what the
runtime reported. Your job is not to judge; your job is to **perform the
requested input** so the assertion has something to assert on.

(The one exception is `XR_EXT_haptic_parametric-scenario`, which is tagged both
`[scenario]` and `[actions]` and *does* get the Select/Menu buttons — and it
skips here anyway, since we do not advertise `XR_EXT_haptic_parametric`.)

### 8.2 The timeout trap — it is not one 600-second clock

Miss a prompt and the test does not wait politely; it fails on a timer. There
are four separate clocks:

| Clock | Value | Where it applies | Message on expiry |
|---|---|---|---|
| Generic prompted wait | **20 s** | Every ordinary `WaitWithMessage` prompt | `Time out: <the prompt text>` |
| Device activity / tracking validity | **30 s** | Waiting for a device to become active/inactive or gain/lose orientation. Requires the state to be **stably** correct for 250 ms. | `Input device activity not detected` |
| "Use all controller inputs" | **600 s (10 min)** | `StateQueryFunctionsInteractive` only — the one test that walks every input on a top-level user path | followed by `REQUIRE(seenActions.size() == actionCount)` |
| Poll interval | 5 ms (0 ms on Android) | inside all of the above | — |

**The 20-second one is what will actually bite you.** Most prompts give you 20
seconds, not ten minutes. Read the screen, keep your hand on the mouse, and do
not alt-tab away to check a log — the log check belongs in §8.5, which the 600 s
test gives you room for.

> **Never pass `--autoSkipTimeout`.** It auto-advances interactive tests and
> emits `WARN("User-specified timeout reached, skipping/continuing
> automatically.")`. A CTS warning must be explained in the submission package,
> and "I let it skip" is not an explanation. It is deliberately a WARN rather
> than a SKIP so that an auto-advanced run cannot look clean.

### 8.3 `--nonDisconnectableDevices` is mandatory here, and must be declared

Several `[actions]` tests prompt **`Turn off /user/hand/left`** and
**`Turn on /user/hand/left`** (and the right equivalents). There is no way to
satisfy those: qwerty's controllers are synthesised by the driver and exist for
as long as the session does. You cannot unplug a keyboard.

`XR_EXT_conformance_automation` could force the device inactive — and it is
explicitly **forbidden in a conformance submission**, so that is not a way out.

The supported way out is the CTS's own flag, which skips exactly those blocks:

```powershell
.\scripts\run_cts.ps1 -Interactive actions -Graphics d3d11 -ConformanceLayer `
    -InteractionProfile khr/simple_controller `
    -ExtraCliArgs --nonDisconnectableDevices
```

The usage guide is explicit that this flag **"must be called out and justified if
used in a submission"**. Write the justification into the run record at the time,
while you remember it: *the only input device is a software-synthesised
keyboard/mouse controller that has no disconnect state.*

`run_cts.ps1` does **not** turn this on for you — an argument with submission
consequences should be visible on the command line, not hidden inside
`-Interactive`.

### 8.4 `--hands` — leave it at `both`

The CTS defaults to `--hands both` and tells runtimes whose *single* controller
can be held in either hand to run the category twice, `--hands left` then
`--hands right`. That does not apply here: qwerty creates **two distinct
controller devices**, both always present, so one `both` run covers it. State
that in the run record so a reviewer does not ask for the second run.

These tests lose coverage under a single-hand run, i.e. they have branches that
only execute with both hands under test — another reason to leave the default
alone:

`xrSyncActions` · `xrSyncActions_priority_rules` ·
`xrSyncActions_priority_rules_EXT_active_action_set_priority` · `ActionSpaces` ·
`XR_EXT_dpad_binding-interactive_*` (and, in the composition category,
`QuadHands`).

### 8.5 The loop — what you will be asked to do, and the key for it

Prompts appear on a quad in front of you. In order of how often you will see
them:

| Prompt (verbatim from the CTS) | What to do |
|---|---|
| `Use all controller inputs on\n/user/hand/left` — then a running counter `Used N/M inputs on: …` | Exercise **every** input on that hand. **This is the one category where the modifier is required** — the prompt names a hand, so hold **CTRL** for `/user/hand/left` (**ALT** for right) throughout: **LMB** (trigger/select), **N** (menu), **MMB** (squeeze), **B** (system), **T/F/G/H** (thumbstick + trackpad), **V** (thumbstick click), RMB-drag and mouse-move (poses). Watch the counter climb; it tells you what is still missing. 600 s. |
| `Release all inputs` | Let go of everything. Release CTRL/ALT too — a modifier change calls `qwerty_release_all()`. Followed by a 2 s settle. |
| `Activate any boolean .../click action when you feel the 3 second haptic vibration, e.g.: on <actions>` — and a short-pulse variant | **You will feel nothing.** See §8.6. |
| `Place left controller somewhere static but trackable` / `Place right controller somewhere static but trackable. Keep left controller on and trackable.` / `Place controller somewhere static but trackable` (then a 5 s sleep) | **Do nothing at all.** See §8.7. |
| `Keep left controller trackable.` / `Keep right controller trackable.` | Likewise — don't touch it. |
| `Waiting for session focus...` / `Waiting for <hand> controller to gain\|lose tracking...` | Click the compositor window if focus wandered. |
| `Turn on /user/hand/left` / `Turn off …` | Cannot be satisfied — §8.3. |
| `(1) With your LEFT controller, push fully UP on your thumbstick and release.` … through `(8)`, plus `(9)`/`(10)` centre-push for trackpad | `XR_EXT_dpad_binding` only, which skips here. |

**Both hands must be exercised.** The tests iterate the interaction profile's
top-level user paths; each hand gets its own "use all inputs" pass. Hold **CTRL**
for the left-hand pass and **ALT** for the right-hand pass — the prompt names
which path it is waiting on.

### 8.6 The haptic confirmation — the one prompt you answer from a log

Inside `StateQueryFunctionsInteractive` the CTS asks you to *press a button when
you feel a vibration*. The usage guide makes this a formal requirement: *"in at
least one interactive test, you have to wait for haptic feedback and confirm it."*

There is no motor. `qwerty_set_output()` writes a log line and returns
`XRT_SUCCESS`. So:

1. Before starting the actions run, open a second window tailing the runtime log
   in `%LOCALAPPDATA%\DisplayXR\` (the newest `DisplayXR_conformance_cli.*.log`).
2. When the prompt appears, watch for `Haptic output: frequency=… amplitude=…
   duration=…`. The 3-second variant and the short-pulse variant are
   distinguishable by `duration`.
3. Press **LMB** (with the prompted hand's modifier held) as soon as the line
   appears.

You are inside the 600 s clock here, so there is time to look. **Record in the
run notes that haptic confirmation was made against the runtime's haptic-output
log rather than a physical motor, and why** — a reviewer is entitled to know how
that requirement was met.

### 8.7 "Place the controller somewhere static" — qwerty satisfies it by default

Three prompts ask you to put a controller down somewhere static but trackable,
then sleep 5 seconds while the CTS samples the pose.

A qwerty controller moves **only** when you drive it. It has no jitter, no drift
and no tracking loss. So the correct response is to **take your hands off the
keyboard and mouse** — the pose is already perfectly static and perfectly
trackable, which is exactly what the prompt asks for.

Two things will ruin it:

- **Leaving CTRL or ALT held.** A modifier *change* fires `qwerty_release_all()`
  and resets the mouse baseline; a modifier *held* while you nudge the mouse
  translates that controller. Let go.
- **Mouse drift.** With CTRL/ALT held, raw `WM_MOUSEMOVE` translates the focused
  controller at 0.001 m/px. Move your hand off the mouse entirely.

Pressing **R** first (reset both controller poses) is harmless and makes the
two-handed variants start from the symmetric default.

### 8.8 Which profile each test actually uses

Nine of the 25 loop over every interaction profile and run only for the ones
named with `-I` — including `StateQueryFunctionsInteractive`, the
`ParentComponents*` pair, and the dpad tests. This is why `-I` must be correct
and explicit (§5).

A handful ignore `-I` entirely and hard-bind `khr/simple_controller`:
`xrSuggestInteractionProfileBindings_interactive`, the palm-pose `-objective`
family, and `XR_EXT_interaction_profile_battery_state_display-interactive`. They
run the same way regardless.

`XR_KHR_generic_controller-select` is gated on `XR_KHR_generic_controller`,
which this runtime does not advertise and which is not in `bindings.json`. It
skips.

---

## 9. Where the XML lands, and how to package it

`run_cts.ps1` writes **three** files to `%TEMP%`, all on the same stem, and
prints their paths as its last three lines:

```
XML:     C:\Users\<you>\AppData\Local\Temp\interactive_composition_d3d11.xml
CONSOLE: C:\Users\<you>\AppData\Local\Temp\interactive_composition_d3d11_console.log
STDOUT:  C:\Users\<you>\AppData\Local\Temp\interactive_composition_d3d11_stdout.log
```

The two logs are **not** duplicates. `_console.log` is the Catch2 console
reporter's output — the per-test results. `_stdout.log` is `conformance_cli`'s
own standard output, which is where it prints its frame-timing block (average
`xrWaitFrame` wait, overhead score) that never goes through the reporter at all.
Keep both.

> **An interactive run shows no live terminal output.** `-RedirectStandardOutput`
> can only target a file, so `conformance_cli`'s stdout is buffered to
> `_stdout.log` and replayed to the terminal in one write **after the process
> exits**. On an hour-long hand-paced run that means a silent console
> throughout. It is not a hang. The prompts you act on are rendered composition
> layers, not stdout — and when a test needs a log to answer it (§8.6, haptic
> confirmation), the log to tail is the **runtime's**, in
> `%LOCALAPPDATA%\DisplayXR\`, not this one.

The `-Interactive` names are chosen so a submission package can be assembled
from them unmodified:

| Category | XML | Reporter log | Stdout log |
|---|---|---|---|
| composition | `interactive_composition_<graphics>.xml` | `…_console.log` | `…_stdout.log` |
| scenario | `interactive_scenario_<graphics>.xml` | `…_console.log` | `…_stdout.log` |
| actions | `interactive_actions_<graphics>_<profile-slug>.xml` | `…_console.log` | `…_stdout.log` |

(Automated runs keep the historical `cts_<tag>` stem, so the two families never
collide in one directory.)

**Rename on packaging.** The CTS usage guide's own example filenames are
slightly different — they do not carry the graphics API for the last two
categories, because the guide assumes one run each. We run five graphics
plugins, so the harness keeps the API in every name and you rename the two that
differ when assembling the package:

| Harness writes | Guide's submission name |
|---|---|
| `interactive_composition_<graphics>.xml` | `interactive_composition_<api>.xml` — same shape, no change |
| `interactive_scenario_<graphics>.xml` | `interactive_scenarios.xml` |
| `interactive_actions_<graphics>_<profile-slug>.xml` | `interactive_action_<profile>.xml` |
| (automated) `cts_<graphics>_<api>.xml` | `automated_<api>.xml` |

**Copy all three out of `%TEMP%` immediately.** `%TEMP%` is swept, and the screenshot
tooling in CLAUDE.md § *Autonomous capture* writes there too.

For each run, record alongside the XML:

1. the `loaded from:` path from the runtime log (§1.4),
2. `displayxr-cli info` output (runtime version + git tag, plug-in ABI, active
   plug-in identity and display info),
3. the CTS tag (`openxr-cts-1.1.63.0`) and the `SIM_DISPLAY_OUTPUT` value used,
4. every test that **skipped**, and why (§6.2, §7.1–7.2, §10.7),
5. for every test you marked FAIL, *why* — a screenshot and one sentence.

And, for the actions category, three written justifications the guide requires
or a reviewer will ask for:

- **`--nonDisconnectableDevices`** — *"must be called out and justified if used
  in a submission"*. The justification is §8.3: the only input device is a
  software-synthesised keyboard/mouse controller with no disconnect state.
- **How haptic confirmation was made** — §8.6: against the runtime's
  haptic-output log line, because the simulated device has no motor.
- **Why `--hands both` was run once and not twice** — §8.4: two distinct
  controller devices exist simultaneously, so the left/right double run the
  guide asks of single-controller runtimes does not apply.

> **`XR_EXT_conformance_automation` must not be used for a submission**, full
> stop. It is the mechanism that could inject the device-inactive state §8.3
> cannot otherwise reach, and reaching for it there is exactly the trap.
> Similarly, `--pollGetSystem` must be called out and justified if used — don't
> use it.

A conformance run passes only if **all** tests required by the testing steps
finish with allowed result codes (pass or skip as appropriate) **and all
warnings are acceptably explained**. One unreasoned FAIL — or one unexplained
WARN — sinks the submission, so a FAIL you cannot explain is a defect to file,
not a result to ship.

---

## 10. 3D-display ambiguities — stated verdict rules

The CTS was written for HMDs. Several of its interactive judgements do not
transfer cleanly to a flat panel that may be weaving. Each of these has a
**stated rule** so two operators reach the same verdict; if you disagree with a
rule, change it here rather than deciding case by case.

### 10.1 A woven image versus a flat reference screenshot

**Rule: judge on a non-weaving configuration. A weave artefact is never a CTS
failure.**

The reference images the CTS shows under Menu are flat 2D screenshots. With the
vendor plug-in on a lenticular panel, the panel is interlacing two views, so the
screen *cannot* match a flat reference no matter how correct the runtime is.
Do the judged pass with sim-display in `SIM_DISPLAY_OUTPUT=2d` (§2.3), where one
view is presented flat and the comparison is well-posed. If the panel pass shows
something the 2D pass did not, that is a display-processor issue — file it
against the plug-in repo, do not mark the CTS test FAIL.

### 10.2 `EyeVisibility` — both views land on one panel

**Rule: switch to `SIM_DISPLAY_OUTPUT=sbs` for this one test, and read the panel
as two side-by-side images: LEFT half = left view, RIGHT half = right view. Pass
only if each half contains exactly one quad, of the right colour.**

`XrEyeVisibility` asks the runtime to show a layer in one eye only — on an HMD
you check by closing one eye. There is no "one eye" on a flat panel, and on a
lenticular panel both views are interlaced into pixels you cannot separate by
inspection.

The test puts a **green** quad at view-space `{-1, 0, -2}` with
`XR_EYE_VISIBILITY_LEFT` and a **blue** quad at `{+1, 0, -2}` with
`XR_EYE_VISIBILITY_RIGHT`. (`XR_EYE_VISIBILITY_BOTH` is *not* exercised, so
there is no "should appear in both halves" case to look for.) In `sbs` the sim
DP lays the two views out horizontally, so:

| Panel half | Must contain | Must **not** contain |
|---|---|---|
| Left (= left view) | the green quad, toward its own left | the blue quad |
| Right (= right view) | the blue quad, toward its own right | the green quad |

*How to look:* do not try to fuse the image. Cover half the screen with your
hand and check each half on its own. **If you see both a green and a blue quad
in the same half, the runtime ignored `eyeVisibility` — that is a FAIL**, and it
is a real conformance defect, not a display artefact.

Note `sbs` is **centre-cropped**, and the quads sit at x = ±1 m, i.e. toward the
outer edges. A quad clipped by the crop is not the same as a quad suppressed by
eye visibility. If a quad's presence is genuinely ambiguous, re-run this one
test in `squeezed_sbs`, which places the tiles as-is with no crop, and judge
there.

### 10.3 Stale-swapchain flicker versus weave-phase flicker

**Rule: only the LEFT square's flicker counts, and only if it survives
`SIM_DISPLAY_OUTPUT=2d` with your head still.**

`StaleSwapchain` renders two 2 cm squares in view space from 1×1-pixel
swapchains. Once a second it re-acquires **both**: the **left** square is written
green every time, the **right** alternates green ↔ blue. The pass criterion is
the test's own words — *"Square on left should be constantly green… If there is
any flicker on the green square, likely at the same time as the other square
changes color, that is a failure."*

So the right square **is supposed to change colour at 1 Hz**; do not report that.
The thing under test is whether the left square holds its content across the
re-acquire. Two unrelated things on this platform also produce flicker:

- the **weave phase** shifting as the viewer moves (a lenticular panel's
  sub-pixel assignment changes with tracked eye position), and
- **eye-tracking warmup**, during which the DP flips between its 2D fallback and
  a tracked 3D weave.

Both are display-processor behaviour, both are invisible in `2d` mode, and both
are movement-correlated. So: judge in `2d`, keep your head still, and if the
flicker on the **left** square is still there it is the runtime's. Corroborate
before filing — CLAUDE.md § *Windows compositor screenshot* describes the
post-weave / pre-weave capture pair, and the **pre-weave atlas** capture tells
you directly whether the content the DP consumed was already flickering, which
settles compositor-versus-DP without an argument.

> The CTS author notes in-source that a failure here *creates a flashing image*,
> and chose 1 Hz and a deliberately small square to stay well outside the
> photosensitive-epilepsy range (rarely as low as 3 Hz). Worth knowing before
> you hand the box to someone else.

### 10.4 `GradientFormatsLinearVsNonLinear` and the alpha-blending tests

**Rule: judge these on Windows only, never on macOS, and never in `anaglyph` or
`blend` output mode.**

These tests ask whether a gradient was transferred with the right
(linear vs sRGB) transfer function, and whether source-alpha blending produced
the right composite. Both are pure colour judgements, and both are destroyed by
the sim DP's colour-mixing output modes — `anaglyph` (the default!) recolours
everything red/cyan, `blend` averages the two views. Use `2d`.

macOS is separately disqualified: its window-server colour pipeline *understates*
transfer-function errors, so a gradient that is visibly wrong on Windows can look
fine there. (This is a standing rule for all colour work in this project, not a
CTS-specific one.) macOS/`metal` is out of scope for #1523 anyway.

If a gradient looks banded but monotonic and correctly oriented, that is
quantisation, not a transfer-function error — pass it. A gradient that is
visibly *gamma-shaped* the wrong way (crushed in the shadows or washed in the
highlights relative to the reference) is a fail.

### 10.5 `XR_KHR_visibility_mask` — there is no lens to mask

**Rule: expect a SKIP, record it, and do not try to make it run.**

We advertise `XR_KHR_visibility_mask` (`XRT_FEATURE_OPENXR_VISIBILITY_MASK` is
`ON`), and `oxr_xrGetVisibilityMaskKHR()`
(`src/xrt/state_trackers/oxr/oxr_api_session.c:355`) returns
`vertexCountOutput = 0`, `indexCountOutput = 0` — an **empty** mask. That is
spec-legal and it is the physically correct answer: a flat panel has no lens
occlusion and no hidden-area mesh, so there is no region the app should skip.

`XR_KHR_visibility_mask-interactive` is tagged **`[composition][interactive]`**
(not scenario) and handles this itself: it skips with
`"No vertices returned, so no visibility mask available in this system."` So you
will not be asked to judge anything. Had it run, it would have generated three
sub-cases — `HIDDEN_TRIANGLE_MESH`, `VISIBLE_TRIANGLE_MESH`, `LINE_LOOP` — and
asked you to confirm you see *no red geometry* except a trace at the edges.

Two things not to do: do not treat the skip as a gap to be closed by
synthesising a mask (that would be claiming an occlusion that does not exist),
and do not turn the extension off to make the skip go away — advertising it and
returning an empty mask is the correct behaviour, and the skip is an allowed
result code.

### 10.6 `QuadHands` and other pose-driven tests

**Rule: reset the poses first (`R`), then place them with the mouse; judge
relative geometry, not absolute realism.**

Qwerty's controllers start at a fixed offset from the HMD — left
`(-0.2, -0.3, -0.5)`, right `(0.2, -0.3, -0.5)` — and are parented to it until
you press `C`. They move only when you drive them, in straight lines, with no
jitter and no tracking loss. A test that asks for a quad "at each hand" is
therefore asking whether the quad follows the pose the runtime reported, which
is a question qwerty answers perfectly well.

**Raise the hands first — they start below the camera's field of view.** The
controller offset `(±0.2, −0.3, −0.5)` is head-*local*, and the controllers are
parented to the head, so moving or pitching the head never changes where they
sit in the view (that is why an unmodified `E` "moves the camera": with no
CTRL/ALT registered the HMD is the target and the hands ride along). Under the
100° legacy camera profile (`DXR_LEGACY_CAMERA_RIG={"horizontalFovDeg":100,
"convergenceDiopters":0}`, half-vFOV 33.8°) the quad centres sit 3° *below* the
frustum and only their top ~18 % shows. The fix is a controller-local move:
hold **CTRL+ALT** (both hands focused), press **E** for ~500 ms (qwerty moves
0.6 m/s, time-integrated → +0.30 m, i.e. eye level, mid-frustum; anything in
220–860 ms lands inside the view), release **E**, then ALT, then CTRL. Park the
cursor *before* the modifiers, never move the mouse while one is held, and never
release ALT without a key in between (system-menu mode). Oracle: run the CTS
with `DXR_QTRACE=1` — `[QTRACE] QD … pos=(x,y,z)` is printed per moved device; a
controller's `y` walks −0.300 → 0.000, whereas 1.6 → 1.9 means the head moved
and the modifier never reached the window. (The service window's log line
"F/G controller focus" is stale: F/G/T/H are the thumbstick; focus is CTRL/ALT.)

Press **R** to reset both controllers to their defaults before judging, so both
hands start symmetric and you can see at a glance that **L** and **R** are not
swapped. Then hold CTRL (or ALT) and move the mouse to translate one hand, and
confirm only that hand's quad moved. Do not fail a pose test because the motion
looks unnaturally smooth — that is the simulator, and the test is not measuring
tracking quality.

`QuadHands` specifically asks for four things, and all four are answerable here:
the quads **face you and are upright** when the controllers are in a thumbs-up,
pointing-into-screen pose (press **R**, then RMB-drag to orient); the quads are
correctly **backface-culled** (rotate a controller 180° and its quad must
disappear, not show mirrored); **R is always drawn atop L**; and both are atop
the cubes. It needs **both** controllers — leave `--hands` at `both` (§8.4), and
note that a controller "not being tested" renders at the origin rather than
vanishing, which is expected and not a pose failure.

### 10.7 The two equirect tests — only one of them runs here

**Rule: `XR_KHR_composition_layer_equirect2` is in scope; plain
`XR_KHR_composition_layer_equirect` is not, and its test will skip.**

The layer extensions this runtime advertises are set in the top-level
`CMakeLists.txt`:

| Extension | Option | Default | Interactive test runs? |
|---|---|---|---|
| `XR_KHR_composition_layer_equirect2` | `XRT_FEATURE_OPENXR_LAYER_EQUIRECT2` | **ON** | **Yes** |
| `XR_KHR_composition_layer_equirect` | `XRT_FEATURE_OPENXR_LAYER_EQUIRECT1` | OFF | No — extension not advertised |
| `XR_KHR_composition_layer_cylinder` | `XRT_FEATURE_OPENXR_LAYER_CYLINDER` | **ON** | **Yes** |
| `XR_KHR_composition_layer_cube` | `XRT_FEATURE_OPENXR_LAYER_CUBE` | OFF | No |
| `XR_KHR_composition_layer_depth` | `XRT_FEATURE_OPENXR_LAYER_DEPTH` | OFF | No |
| `XR_KHR_composition_layer_color_scale_bias` | `XRT_FEATURE_OPENXR_LAYER_COLOR_SCALE_BIAS` | OFF | No |

A test whose extension is not advertised is skipped by the CTS, not failed, and
a skip is an allowed result code. **Record the skips in the run notes** — a
reviewer comparing your XML against another runtime's will see fewer tests and
should be able to tell why without reading CMake.

**Both equirect tests are `[composition][interactive]`** (not scenario), and each
generates **six** sub-cases from a static table, wrapped in a
`DYNAMIC_SECTION`. On screen the title reads
`Equirect2 layer: subtest N of 6`. The six are identical in name, description,
pose and image between the two extensions — only the way the sub-sphere is
described differs (`scale`/`bias` for equirect1, angles for equirect2):

| # | Sub-case name | What you should see | radius | equirect2 angles (central horizontal / upper / lower vertical) |
|---|---|---|---|---|
| 1 | `Full sphere at infinity` | A 360° view of the inside of a test cube at infinity, in **LOCAL** space | `0.0` (∞) | `2π` / `π/2` / `−π/2` |
| 2 | `Full sphere at infinity (view space)` | The same, but rendered in **VIEW** space — it should follow your head | `0.0` (∞) | `2π` / `π/2` / `−π/2` |
| 3 | `Full sphere at 2m` | The same cube image on a **2 m** sphere. The reference is shot from above and left of the origin so the perspective effect is obvious | `2.0` | `2π` / `π/2` / `−π/2` |
| 4 | `Full sphere at 2m with pose` | The 2 m sphere moved forward 1.5 m and rotated 45° downward about X | `2.0` | `2π` / `π/2` / `−π/2` |
| 5 | `90 degree section at infinity (cropped file)` | A 90° section in both latitude and longitude, at infinity, from a **pre-cropped image file** (`equirect_central_90.png`) | `0.0` (∞) | `π/2` / `π/4` / `−π/4` |
| 6 | `90 degree section at infinity (cropped image extents)` | The same 90° section, but cropped by **`imageRect`** out of the full 8K image | `0.0` (∞) | `π/2` / `π/4` / `−π/4` |

Sub-cases 5 and 6 must look **the same as each other** — that is the whole point
of having both: one crops in the file, one crops via the layer's image extents,
and a runtime that mishandles `imageRect` shows a difference. Judge them as a
pair.

Both layers use `XR_EYE_VISIBILITY_BOTH` and `imageArrayIndex = 0`, so neither
interacts with §10.2.

The equirect1 table is the same six rows with `scale`/`bias` in place of the
angles (`{1.0, 1.0}` / `{0, 0}` for 1–4, `{0.25, 0.5}` / `{0, 0}` for 5–6). You
will not see them — the extension is off — but if `XRT_FEATURE_OPENXR_LAYER_
EQUIRECT1` is ever turned on, the judgement rules above transfer unchanged.

### 10.8 `SpaceOffsets` and `InteractiveThrow` — the six velocity criteria, driven from the keyboard

**Rule: these two auto-pass on velocity, not on looks. Drive all six axes and
let the test end itself; Menu is FAIL, never "done".**

`SpaceOffsets` renders its gnomons and then waits for the *runtime-reported*
`XrSpaceVelocity` of the controller's base space to have reached **0.5 m/s**
along each of X, Y and Z and **6 rad/s** about each of X, Y and Z
(`test_SpaceOffsets.cpp:105-112`). Until #1692 the qwerty controllers reported
no velocity at all, so every criterion read zero, nothing could ever fail and
nothing could ever pass, and the only exit was Menu = "user has failed the
test". They now report both velocities analytically (`docs/reference/qwerty-device.md` §3.7),
and the sprint-boosted look rate plus the new Z/X roll axis (§4) put all six
criteria inside keyboard reach. `InteractiveThrow` reads the same linear
velocity at the moment of release.

The recipe. **Hold CTRL+ALT throughout** (both hands focused — `SpaceOffsets`
locates both `/user/hand/left` and `/user/hand/right`, and driving both at once
halves the work). Park the mouse cursor *before* taking the modifiers and do not
move it while one is held (§10.6). Each row: press and hold for **~300 ms**,
then release the key. The velocity is correct from the first poll after the
press, so the hold only has to outlive a few frames; 300 ms is slack, and short
holds keep the controller near its reset pose.

| Criterion | Hold | Reported |
|---|---|---|
| X linear ≥ 0.5 m/s | `D` (or `A`) | ±0.60 m/s on base X |
| Y linear ≥ 0.5 m/s | `E` (or `Q`) | ±0.60 m/s on base Y |
| Z linear ≥ 0.5 m/s | `S` (or `W`) | ±0.60 m/s on base Z |
| X angular ≥ 6 rad/s | **SHIFT** + `↑` (or `↓`) | ±9.16 rad/s about base X |
| Y angular ≥ 6 rad/s | **SHIFT** + `←` (or `→`) | ±9.16 rad/s about base Y |
| Z angular ≥ 6 rad/s | **SHIFT** + `Z` (or `X`) | ±9.16 rad/s about base Z |

Two things make or break it:

- **SHIFT is mandatory on the three angular rows.** Unboosted the controller
  turns at `0.05 * 60 = 3 rad/s`, exactly half the criterion — it will look
  like it is rotating perfectly well and still never satisfy anything.
- **Tap `R` between the angular rows.** `R` resets both controllers to the
  identity orientation, which is what makes "the device's own axis" and "the
  base axis" the same axis. Pitch and roll are device-local: roll after a 60°
  pitch is reported about a tilted axis, and its base-Z component drops below
  6 rad/s while the controller is visibly spinning. The linear rows do not
  need this (Q/E is base-space, and W/A/S/D is rotated by an orientation that
  `R` has just made identity).

Order does not matter — each criterion latches the first time it is met — but
the CTS only *counts* a frame whose offset-space velocities agree with its own
prediction from the base-space velocity, so drive one axis at a time rather
than mashing several keys together.

---

## 11. Run evidence

One row per recorded interactive run. A run is only submission evidence if it is
listed here as **valid**; anything else is practice.

| Date | Category | Graphics | DP / mode | Result | Status |
|---|---|---|---|---|---|
| 2026-09-19 | composition | `d3d11` | sim-display, `SIM_DISPLAY_OUTPUT=2d` | `interactive_composition_d3d11.xml` — 16 pass / 11 skip / 0 fail | **VOID — do not submit, must be redone** |

**Why the 2026-09-19 `d3d11` composition run is void.** It was judged before the
operator understood the controls (§4.4/§5.1: the doc then said a modifier was
required for Select/Menu, which is wrong), so the pass/fail decisions in it do
not reflect what was on screen. Its own headline result contradicts what a
correct pass finds: `GradientFormatsLinearVsNonLinear` is now known to FAIL on
every format sub-case (#1580), and it is recorded there as 0 fail. The XML,
console log and stdout log are kept as an artefact of the defect hunt only.

Redo it once **#1580** and **#1581** land, on a `conformance_cli.exe` whose DPI
manifest has been verified with `mt.exe` (§1.3), and add a new row.

---

## See also

- [View-Configuration Model](view-configuration-model.md) — why a CTS session is
  a legacy 2-view session, and the automated-lane CTS status
- [Qwerty Device Driver](qwerty-device.md) — the full key map and pose mechanics
- [DPI awareness: the DLL rule](dpi-awareness.md) — why `conformance_cli.exe`
  needs a manifest
- [CTS bring-up — Windows handoff runbook](../roadmap/cts-windows-handoff.md) —
  how the harness got here
- `scripts/run_cts.ps1` — the harness; `scripts/fetch_build_cts.bat` — the pin
- #1523 — the full-matrix coverage epic this procedure is § 4 of
