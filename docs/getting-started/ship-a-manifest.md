# Ship a Manifest — Make Your App Discoverable

> **Audience:** developers shipping a DisplayXR app. **Reading time:** ~5 minutes.

The DisplayXR runtime will load and run your app whether or not you ship a manifest. So why bother authoring one — and especially a 3D logo — when a user running just the bare runtime never sees it?

Because the manifest is **portable spatial-app metadata**, not a DisplayXR-Shell config. The same `.displayxr.json` is what gets your app onto the launcher, into the public showcase, into any third-party workspace controller (vertical, kiosk, OEM-branded), and into the discovery surfaces of every shell that ships across the ecosystem. **The bare runtime ignoring the file is a feature, not a bug** — it proves the manifest is portable metadata, not lock-in.

This is the OpenXR equivalent of an `Info.plist`. Author it once. Cash in on it everywhere.

---

## What you ship

A `.displayxr.json` next to your `.exe`, plus a 2D icon, plus (optionally) a 3D icon — the 3D icon is the killer differentiator and what makes the platform feel like a 3D-native OS instead of "Windows but with a parallax effect."

```
my_app/
├── my_app.exe
├── my_app.displayxr.json     ← 30 seconds of work
├── icon.png                  ← 512×512, your existing app icon
└── icon_sbs.png              ← optional but high-leverage: stereo pair
```

Minimum manifest:

```json
{ "schema_version": 1, "name": "My App", "type": "3d" }
```

Recommended manifest (this is the version that gets you featured):

```json
{
  "schema_version": 1,
  "name": "My App",
  "type": "3d",
  "icon": "icon.png",
  "icon_3d": "icon_sbs.png",
  "icon_3d_layout": "sbs-lr",
  "category": "demo",
  "description": "One-line pitch shown on hover."
}
```

That's the whole authoring surface. Full spec: [`docs/specs/displayxr-app-manifest.md`](../specs/displayxr-app-manifest.md).

---

## Where the manifest shows up

| Surface | Consumes the manifest? | Why this matters to you |
|---|---|---|
| **DisplayXR Shell launcher** | Yes — primary consumer | Your tile, with your 3D logo, on every Shell user's home screen |
| **Public app showcase** (displayxr.com) | Yes — curated from manifested apps only | Free distribution / social proof |
| **OEM-branded shells** (Lenovo, Samsung, ZTE, …) | Yes — same `.displayxr.json` schema | One manifest, every OEM ships your app discoverable |
| **Vertical / kiosk controllers** (CAD, medical, retail) | Yes — third-party workspace controllers all read this format | Your app is reachable from any compliant controller |
| **AI-agent drivers** (MCP-driven launchers, voice assistants) | Yes — they introspect manifests to decide what to launch | Future-proofing for agentic UIs |
| **Browse-for-app** in any shell | Writes a manifest on the user's behalf | If you don't ship one, the user gets a generic, ugly auto-extracted icon |
| **Bare DisplayXR runtime** | No — by design | The bare runtime is the OpenXR layer. Discovery is the workspace layer. |

The bare runtime not consuming the manifest is exactly what makes this contract worth participating in. A consumer-facing surface that *did* read your manifest would mean the runtime had a UI, which would mean a brand was baked into the platform, which would mean lock-in. Instead, the runtime is the platform, and *every* discovery surface — first-party, third-party, OEM, AI — reads the same file.

If you ever want to be discoverable on a 3D-display surface that isn't your own, this is the contract.

---

## Why ship a manifest now, even if you don't use the Shell

Three concrete reasons:

**1. Discoverability compounds.** The Shell is the reference workspace controller today. OEM-branded shells, vertical controllers, and the public showcase all use the same manifest contract — they're being built or are already shipping. Apps that ship a manifest now will appear in those surfaces the day they go live, with no work from you. Apps that don't will need a separate authoring pass per surface.

**2. The cost is ~zero.** Minimum manifest is two lines of JSON. A 2D icon you almost certainly already have. The 3D icon is the only real ask — and even that is a single Unity / Unreal menu click for the most common case (see "The 3D logo" below).

**3. There's no version to be stuck on.** The manifest is additive — schema versions only ever grow new optional fields. The manifest you ship today is the manifest that works in 2030. There is no "v2 migration" anyone is going to ask you to do.

The only investment you avoid by not shipping a manifest is the 30 seconds it would take to author one. The only thing you trade away is being visible on every spatial surface that ships in this ecosystem.

---

## The 3D logo: your tile is your storefront

The DisplayXR Shell renders launcher tiles in stereoscopic 3D. A tile with `icon_3d` set is the difference between a flat thumbnail and a small floating diorama of your app — the platform's signature visual moment. Users notice. Reviewers notice. Press shots feature 3D-iconed apps disproportionately.

Authoring a 3D icon takes the same effort as the existing 2D icon, in any of these flows:

- **Unity** — the [DisplayXR Unity plugin](https://github.com/DisplayXR/displayxr-unity) menu item ships a "Capture stereo icon" command that renders a side-by-side PNG from the runtime's stereo camera pair at 0.5–1.0 m convergence. One click.
- **Unreal** — same flow, exposed via the [DisplayXR Unreal plugin](https://github.com/DisplayXR/displayxr-unreal) menu.
- **Native apps** — render two offset views of your scene with asymmetric frustums, save side-by-side. ~20 lines of code, runs once.

Convergence guidance and parallax budget: §4.2 of the [manifest spec](../specs/displayxr-app-manifest.md). Aim for a comfortable 40 cm convergence with ±2% of image width as the parallax range. Most authoring engines render this without issue — there is no "doing the stereo math wrong" failure mode that lands you in a malformed file.

If you skip `icon_3d`, your tile falls back to the 2D `icon` rendered as a flat quad. You're not punished — but you're also not differentiated. The 3D logo is what makes the platform feel like a platform.

---

## Author once, distribute everywhere

If you publish to one of the standard discovery directories, every workspace controller on the user's machine sees your app:

```
%LOCALAPPDATA%\DisplayXR\apps\          ← per-user (no elevation needed)
%ProgramData%\DisplayXR\apps\           ← system-wide (installer-elevated)
```

The directory paths are **DisplayXR-runtime-branded, not Shell-branded** — every compliant workspace controller scans these. An app installed once is found by every controller a user might run, now or later. This is the "one app, every shell" deal.

For in-tree development, you can also use sidecar mode — drop the manifest next to your `.exe` and the Shell's dev-tree scanner picks it up. See §2 of the spec for the full discovery rules.

---

## Get started

| You want to… | Do this |
|---|---|
| Ship a minimum manifest in 30 seconds | Copy the two-line JSON above next to your `.exe`, name it `<exe_basename>.displayxr.json` |
| Add a 3D icon | Use the Unity / Unreal plugin menu item, or render two offset views in your engine |
| See a working example | The reference cube apps under `test_apps/cube_handle_*_win/` ship full manifests with 3D icons |
| Read the full spec | [`docs/specs/displayxr-app-manifest.md`](../specs/displayxr-app-manifest.md) |
| Submit your app to the public showcase | (Coming soon — manifested apps will be auto-eligible) |

---

## TL;DR

You're shipping an OpenXR app for a 3D display. The runtime runs it either way. **A 30-second JSON file plus an optional stereo PNG gets your app onto every discovery surface in the ecosystem — first-party, OEM, vertical, AI-driven — for free.** The bare runtime ignoring the file is what makes the contract portable. Ship a manifest. Ship a 3D logo. Show up where users look.
