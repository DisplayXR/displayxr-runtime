# Demo distribution — per-demo public repos

## Motivation

Previously every DisplayXR demo lived in a single public umbrella repo, `DisplayXR/displayxr-demos`, auto-synced on each release tag. That worked when there was one demo; with several planned, a single repo makes it awkward to:

- version individual demos independently
- let users grab a demo without cloning unrelated code
- attach per-demo binary releases
- track issues / PRs per demo

So we split: **one repo per demo**, named `DisplayXR/displayxr-demo-<name>`. The first is `displayxr-demo-gaussiansplat`.

## Contract per demo repo

Each per-demo public repo:

1. **Is auto-synced.** Source comes from `displayxr-runtime-pvt` via a dedicated workflow `.github/workflows/publish-demo-<name>.yml`, triggered by `v*` tags. Users should open issues on `displayxr-runtime-pvt` (or on the demo repo — they get forwarded), not PR the synced source directly.
2. **Builds offline, cross-platform where applicable.** Each repo is self-contained: vendored OpenXR headers (no submodule), pinned FetchContent dependencies, no path references back into the runtime repo.
3. **Ships binaries as GitHub Releases.** On each tag, a zipped Windows binary (and, once macOS CI is re-enabled, a macOS binary) is attached to the Release matching the tag.
4. **Has a standalone `CMakeLists.txt`.** Same sources as in `displayxr-runtime-pvt/demos/<demo>/`, but the top-level and per-subdir CMakeLists use sibling paths instead of `../../` references.

## How the dual layout works

Each demo source tree in `displayxr-runtime-pvt/demos/<demo>/` ships two CMakeLists:

- `CMakeLists.txt` — the in-tree one, used by local dev and `build-windows.yml`. Reaches up into `test_apps/common/` and `src/external/openxr_includes/`.
- `CMakeLists.standalone.txt` — renamed to `CMakeLists.txt` by the publish workflow when copied into the demo repo. Uses sibling paths (`../common`, `../openxr_includes`, `../3dgs_common`).

The in-tree layout is **never** touched by the standalone files and vice versa, so you can iterate on the demo locally without any ceremony.

The shared libraries (`3dgs_common/`, curated subset of `test_apps/common/`) get the same treatment. `scripts/publish-demo.sh` packages one demo into a target directory and runs all the renames — useful for dry-runs and as the single source of truth that both CI and local use.

## Repo layout (example: displayxr-demo-gaussiansplat)

```
displayxr-demo-gaussiansplat/
├── CMakeLists.txt            # top-level (was CMakeLists.top.txt in pvt)
├── README.md                 # (was README.standalone.md in pvt)
├── LICENSE                   # copied from pvt root
├── macos/                    # (← demos/gaussian_splatting_handle_vk_macos/)
├── windows/                  # (← demos/gaussian_splatting_handle_vk_win/)
├── 3dgs_common/              # (← demos/3dgs_common/)
├── common/                   # (← curated subset of test_apps/common/)
├── openxr_includes/          # (← src/external/openxr_includes/)
└── scripts/
    ├── build_macos.sh
    └── build_windows.bat
```

## CI workflows involved

| Workflow | Trigger tags | Purpose |
|---|---|---|
| `publish-public.yml` | `v*`, `runtime/v*` | Release runtime + shell to their public repos |
| `publish-demo-<name>.yml` | `v*`, `demo-<name>/v*` | Sync demo source + attach binary Release zip |
| `build-windows.yml` | `v*`, `runtime/v*`, `demo-<name>/v*`, PRs, `*-ci` branches | Produces `DisplayXR`, `DisplayXR-Installer`, and per-demo artifacts |
| `build-macos.yml` | currently disabled | Will produce macOS binary artifacts when re-enabled |

The `sync-source` job runs in parallel with `build-windows.yml`; `publish-binaries` waits on the Windows build and attaches the artifact to the release the tag already created.

## Tag scheme — per-component release cadences

Each public repo versions independently. Tag pvt with one of:

| Tag pattern | Fires | Use when |
|---|---|---|
| `vX.Y.Z` | **every** `publish-*` workflow | Synchronised full-stack release. Back-compat with pre-split tags; fine as a default. |
| `runtime/vX.Y.Z` | `publish-public.yml` only | Runtime or shell source changed; demos unaffected. |
| `demo-<name>/vX.Y.Z` | `publish-demo-<name>.yml` only | Only that demo changed; runtime + shell + other demos unaffected. |

**The public repo tag is derived by stripping any prefix**: `runtime/v1.2.0` → `v1.2.0` on the runtime + shell public repos; `demo-gaussiansplat/v1.0.0` → `v1.0.0` on the demo repo. Each public repo sees a clean semver lineage regardless of how many fix-forward cycles the pvt side went through.

### Why not one `vX.Y.Z` for everything?

Before the per-component tag scheme existed, every CI-only fix-forward (e.g. a workflow YAML typo) dragged all public repos through a meaningless release bump — users saw a v1.1.0 → v1.1.1 → v1.1.2 chain where v1.1.1/v1.1.2 had source-identical runtime+shell to v1.1.0. [#170](https://github.com/DisplayXR/displayxr-runtime-pvt/issues/170) tracked the cleanup. With per-component tags, a runtime-only fix becomes `runtime/v1.1.1` → v1.1.1 on the runtime repo only; the demo repo stays at whatever version it was.

### Runtime-compatibility covenant

Each demo repo's `README.md` states the minimum compatible runtime version, for example:

> **Requires the DisplayXR runtime v1.1.0 or newer.** Install from [`displayxr-shell-releases`](https://github.com/DisplayXR/displayxr-shell-releases/releases).

Update that line in `demos/<your_demo>/README.standalone.md` whenever the demo starts relying on new runtime behaviour. Users coordinate versions by reading the README, not by matching tags — same pattern Unity packages and npm libraries use. Keeps releases decoupled without fragmenting the UX.

## Adding a new demo

See `docs/guides/add-new-demo-repo.md` for the step-by-step.

## What is *not* published

- `demos/spatial_os_handle_d3d11_win/` — superseded by the Shell feature. Stays in `displayxr-runtime-pvt/demos/` as a reference implementation, but no public repo and no publish workflow.
- Anything under `test_apps/` — these are CI-only builds, not user-facing demos.

## What happened to `DisplayXR/displayxr-demos`

That repo is retired once the per-demo replacement is producing green releases. It's **deleted** outright — no redirect README, no umbrella index. Any links in old blog posts / tweets that pointed to it break; users following them should find the per-demo repo via the `DisplayXR` organisation page or a search.
