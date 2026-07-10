# DisplayXR Browser — packaging the inline-3D fork as a developer preview

> Status: **plan** (no build yet). Companion to [`webxr-support.md`](webxr-support.md) §2.4–2.6
> (Step B) and [`webxr-step-b-design.md`](webxr-step-b-design.md). Tracks how we turn the working
> `displayxr-inline-3d` Chromium patch into a downloadable browser product **without** signing up
> for a full browser-vendor treadmill.

## 1. Goal & framing

Ship inline-3D to our own users **now**, independent of Google or the standards process
(`webxr-support.md` §2.5 "ship inline 3D to our own users now → a patched Chromium / CEF
distribution" — the one adoption goal that is fully in our control). The deliverable is a
**Developer Preview**, explicitly *not* a maintained daily-driver browser:

- **In scope:** a signed, branded, installable Chromium build that renders the whole web normally
  and weaves glasses-free 3D for `inline-3d` WebXR pages on DisplayXR hardware; a first-run that
  provisions/detects the runtime + display plug-in; a GitHub Release + a site download.
- **Commitment (bounded):** rebase ~monthly onto each Chrome **stable milestone** (§6) — mechanical
  because the patch is small and touches a known file set.
- **Explicitly out of scope (the treadmill):** Chrome's *mid-cycle security* cadence (the ~1–2-week
  dot-releases between milestones — we track milestones monthly, not those), Widevine DRM, Google
  Sync/Safe-Browsing, and silent auto-update. These are what make "run a browser" a dedicated-team
  commitment (`webxr-support.md` §2.5); the monthly-milestone + preview framing captures the upside —
  dev adoption, demos, the evidence that drives the hardware + standards narrative — without it.

**Why a preview, not a product fork:** `webxr-support.md` §2.4/§2.5 already concluded the fork should
be *"deliberately not a maintained product fork"* — it is simultaneously (a) how we deliver inline-3D
to users and (b) the reference implementation a standards proposal needs, and the realistic *native*
adopters are the other Chromium browsers (Edge/Brave/Arc) cherry-picking a clean patch, not us running
a browser forever. This doc keeps that discipline: the preview is a **demo/dev-adoption artifact with a
bounded maintenance policy** (§6), not a security-critical daily driver.

## 2. What we have vs. what a distributable needs

| Piece | Today | Needed for a preview |
|---|---|---|
| Chromium patch | Working on fork branch `displayxr-inline-3d` @ `C:\src\chromium\src` (local only) | Patch captured in a repo as a rebaseable series + documented integration points (§5) |
| Build type | `is_component_build=true`, dev (`out/Default`) | `is_official_build=true`, static, optimized, `chrome_pgo_phase=0` |
| Branding | Stock Chromium | Product name / icons / about page / user-agent tag (a Chromium-based browser under our name — like Brave/Edge; **not** "Chrome") |
| Weave prerequisites | Assumes runtime + DP already registered | Installer provisions/detects the DisplayXR runtime + a display plug-in; graceful "no 3D display" fallback |
| Signing | EV-cert signing infra exists (`DXR_SIGN_REPO`, `docs/specs/runtime/release-signing.md`) | Sign the browser exe + installer with the same provider |
| Installer | Meta-bundle exists (`displayxr-installer`) | A DisplayXR Browser installer (standalone, or a new bundle target) |
| Update | none | "Check for updates" nag → download link (NOT silent auto-update for a preview) |
| Platform | Windows / D3D11 + DirectComposition only | Windows only for the preview; macOS/Linux need more compositor work (§7) |

The single most important gap is **not** engineering — it is the **maintenance policy** (§6). The build
mechanics are a few days; the open-ended commitment is the thing to bound *before* shipping.

## 3. Does it behave like a real browser? Yes.

It **is** Chromium: every site works normally. The only delta is the inline-3D surface (`inline-3d`
session mode + `XRDisplayLayer` + the GPU-resident weave path, `webxr-step-b-design.md` §13). On a
DisplayXR panel with the runtime installed, inline-3D pages weave glasses-free; on any other machine or
a 2D monitor, the weave silently no-ops and it is an ordinary browser. So it degrades gracefully and can
be a genuine daily driver *functionally* — the preview label is about the **maintenance/security
commitment**, not missing browser capability.

## 4. Distribution architecture

```
DisplayXR-Browser-Preview-Setup.exe   (signed)
  ├─ DisplayXR Browser (branded static Chromium, inline-3d patch)
  ├─ DisplayXR runtime            (prereq for weave; reuse the runtime installer)
  └─ display plug-in (e.g. Leia)  (prereq for weave; vendor installer)
        │  first run: detect 3D display + registered DP; if absent, run as a normal
        │  browser and surface a one-time "no DisplayXR 3D display detected" notice.
        ▼
  GitHub Release (displayxr-browser repo)  +  download button on displayxr-website
        label: "Developer Preview — Windows, requires a DisplayXR 3D display"
```

The runtime + DP are the *same* components the meta-bundle already ships, so the browser installer can
either **chain** them (like `displayxr-installer` chains component installers) or hard-require an
existing DisplayXR install.

## 5. Repo strategy (answers "where does this live?")

Three distinct artifacts, three homes — do **not** overload the CEF repo (that is the Step-A native
OSR stand-in, a different artifact from the browser product):

- **`displayxr-browser` (NEW)** — the fork productization: the inline-3D patch captured as a
  rebaseable `.patch` series (or a documented git overlay) over a pinned Chromium milestone, plus
  `fetch/build/brand/package/sign` scripts and release automation. This is the home of the "how to
  rebuild the browser" story. The patch is small and touches a known, enumerated set of files
  (`webxr-step-b-design.md` §13 lists them: Blink `inline-3d`/`XRDisplayLayer`, `cc`/`viz` metadata
  plumbing, `skia_output_surface_impl_on_gpu.*` weave, the `components/displayxr` component, and the
  two additive `ProduceOverlayForWeave` gpu-layer methods), which is what makes rebasing mechanical.
- **`displayxr-web` (NEW)** — the **web-developer surface**: a tiny optional JS helper/SDK over
  `XRDisplayLayer`, the three.js inline-3D sample(s) (§8), and dev docs ("build a 3D page for the
  DisplayXR Browser"). Static, served via **GitHub Pages** (`displayxr.github.io/displayxr-web` or a
  custom subdomain). This is the canonical repo web devs clone — the DisplayXR analogue of
  `immersive-web/webxr-samples`. The browser navigates to it for the live demos.
- **`displayxr-website` (existing)** — marketing/narrative. Links to the browser download and **embeds
  or links a hero demo** from `displayxr-web`; it does not host the canonical samples.

Naming note: `displayxr-web` = the *web content/SDK* surface (distinct from `displayxr-browser` = the
*browser build*). If the ambiguity grates, `displayxr-web-samples` is the fallback.

## 6. Maintenance policy — the load-bearing decision

The preview lives or dies on a *bounded* commitment. **Decision (locked):**

- **Pin to a Chrome *stable* milestone; rebase ~monthly** — track each new stable milestone
  (Chrome's cadence is ~4 weeks), not tip-of-tree and not every mid-cycle dot-release. The inline-3D
  patch is small and touches a known, enumerated file set (§5), so a milestone rebase is mechanical —
  roughly a monthly `fetch → apply patches → resolve any drift → build → verify weave → sign → release`
  pass. **Honest caveat that keeps the disclaimer mandatory:** monthly milestone rebases do **not**
  pick up Chrome's *out-of-band security patches* (shipped every ~1–2 weeks between milestones), so
  the build is always some days-to-weeks behind on security fixes.
- **Security posture = preview disclaimer (non-negotiable, stays regardless of cadence).** The download
  page and first-run state plainly: *"Developer preview. Rebased ~monthly onto Chrome stable, but not
  maintained to Chrome's mid-cycle security cadence — do not use for sensitive browsing; use your
  primary browser for banking/etc."* Honest, defensible, and it retires the duty-of-care objection.
- **Update = lightweight version check, not silent auto-update.** On launch, check the GitHub Releases
  API for a newer preview and surface a "new version available → download" prompt (no silent install).
  A monthly release cadence makes this check meaningful without standing up an Omaha-style updater
  (heavy, and a stronger security promise than a preview should make).
- **EOL clause.** The preview may lag or pause between rebases; it is a showcase, not a supported
  product. If/when a Chromium-derived browser (Edge/Brave) or upstream adopts the module, its job is done.

## 7. Platform scope

Windows / D3D11 + DirectComposition only for the preview — that is where the inline-3D weave path lives
today (`webxr-step-b-design.md` §13.4/§13.5). macOS (Metal) and Linux (Vulkan) would each need a weave
hook in their respective Chromium output paths; out of scope for the first preview, notable as the
obvious follow-on if the preview lands.

## 8. The three.js demo

A small **three.js `inline-3d` sample** is the primary evidence artifact (per `webxr-support.md` §2.5,
demos + install-base outrank the reference impl and the spec text as adoption levers). Shape:

- A normal responsive page (2D chrome around it) with one bordered `<canvas>` that renders a rotating
  3D model as a **side-by-side stereo pair** into an `XRDisplayLayer` bound to that canvas, requesting
  an `inline-3d` session and re-rendering the pair off-axis from the eyes the session reports each frame
  (look-around). Around it: flat 2D text, so the "3D element inside a normal page" thesis is visible.
- **Home: `displayxr-web`** (§5), served via GitHub Pages; the browser preview's start page / demo
  gallery points at it; the marketing site embeds the hero.
- **Depends on** the Step-B look-around eyeball being green (`webxr-step-b-design.md` §13 deferred item)
  — the sample is exactly the eye-consuming page that validation needs, so building the demo and closing
  that eyeball are the *same* task. Do them together.

## 9. Relationship to standards

This preview + its demos are the **evidence packet** the standards push (`webxr-support.md` §2.6 step 4,
draft explainer `webxr-displayxr-explainer.md`) is gated on — §2.5 ranks *demos* and *hardware install
base* **above** both the spec text and the reference implementation as what actually moves browser
owners. So the preview is not a detour from standards; per the roadmap's own weighting it is the
higher-leverage near-term work. (Caveat, also from §2.5: a shipping fork can *reduce* upstream urgency —
so pair the preview with an active explainer, don't let it become the terminus.)

## 10. Implementation plan (how to best implement)

Smallest risk-retiring step first; each gate is independently checkpointable.

- **P0 — official-build + rebrand spike (retire the biggest unknown).** From the current fork, produce
  an `is_official_build=true` static, branded binary (product strings, icon, user-agent) and confirm it
  (a) launches, (b) still weaves inline-3D on hardware (the `GPU-resident scratch path` marker +
  service weave, `webxr-step-b-design.md` §13.5), (c) signs cleanly with the existing EV provider. This
  proves a *distributable* binary is real before any packaging effort. **Watch:** official builds differ
  from component builds (static link, PGO, stripped) — re-verify the weave path there specifically.
- **P1 — `displayxr-browser` repo + rebaseable patch.** Capture the patch as a `.patch` series over the
  pinned milestone tag + `fetch/build/brand/package/sign` scripts. Document the integration-point file
  list (§5) so rebases are mechanical. CI (optional) builds on a self-hosted runner.
- **P2 — installer + first-run.** A signed `DisplayXR-Browser-Preview` installer that chains (or
  requires) the runtime + DP, with the graceful "no 3D display" fallback. Reuse `displayxr-installer`
  patterns + `release-signing.md`.
- **P3 — distribution + policy.** GitHub Release in `displayxr-browser`; download button on the website
  with the explicit preview/label + the §6 maintenance policy and security disclaimer on the page.
- **P4 (parallel) — `displayxr-web` + three.js demo.** Stand up the repo + GitHub Pages, build the
  three.js `inline-3d` sample (§8), which doubles as the Step-B look-around eyeball. Link it from the
  browser start page + the marketing site.

**Sequencing call:** P0 is the gate — it is cheap and it retires the "can this even be a shippable,
signable, still-weaving binary?" risk before committing to repos, installers, and demos. Do P0, then
decide whether the preview is worth P1–P4 given the (bounded but real) maintenance policy in §6.
