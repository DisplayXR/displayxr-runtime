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

| Workflow | Runs on | Purpose |
|---|---|---|
| `publish-demo-<name>.yml` | `v*` tag on pvt | Sync source → demo repo + attach Release zip |
| `build-windows.yml` | `v*` tag, PRs, `*-ci` branches | Produces `GaussianSplattingVK` artifact that the publish workflow attaches |
| `build-macos.yml` | currently disabled | Will produce the macOS binary artifact when re-enabled |

The `sync-source` job runs in parallel with `build-windows.yml`; `publish-binaries` waits on the Windows build and attaches the artifact to the release the tag already created.

## Adding a new demo

See `docs/guides/add-new-demo-repo.md` for the step-by-step.

## What is *not* published

- `demos/spatial_os_handle_d3d11_win/` — superseded by the Shell feature. Stays in `displayxr-runtime-pvt/demos/` as a reference implementation, but no public repo and no publish workflow.
- Anything under `test_apps/` — these are CI-only builds, not user-facing demos.

## What happened to `DisplayXR/displayxr-demos`

That repo is retired once the per-demo replacement is producing green releases. It's **deleted** outright — no redirect README, no umbrella index. Any links in old blog posts / tweets that pointed to it break; users following them should find the per-demo repo via the `DisplayXR` organisation page or a search.
