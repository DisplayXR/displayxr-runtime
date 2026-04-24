# Adding a new demo (with its own public repo)

This guide walks you through turning a demo in `displayxr-runtime-pvt/demos/` into a standalone `DisplayXR/displayxr-demo-<name>` public repo with CI-driven source sync + binary releases.

See `docs/roadmap/demo-distribution.md` for the rationale behind the per-repo split.

## 1. Build the demo in-tree first

Before splitting, get the demo working under `demos/<your_demo>/` with its in-tree `CMakeLists.txt` the normal way. Make sure:

- The demo only depends on `test_apps/common/` (or a subset), `src/external/openxr_includes/`, and external packages (Vulkan, OpenXR loader). No `src/xrt/` runtime-internal code.
- Any bundled assets live under `demos/<your_demo>/assets/` and are committed (LFS is fine for large assets, but keep them small enough to ship per-release).
- `build-windows.yml` has a job that produces a named artifact containing the binary + required DLLs. Windows-only demos already follow this pattern — model after `TestAppGaussianSplattingVK`.

## 2. Write the `*.standalone.txt` CMake variants

For every `CMakeLists.txt` in the demo and any shared lib it vendors (`3dgs_common/`, curated `common/`), add a `CMakeLists.standalone.txt` sibling that uses the sibling-dir layout:

- `${CMAKE_SOURCE_DIR}/../../src/external/openxr_includes` → `${CMAKE_SOURCE_DIR}/openxr_includes`
- `${CMAKE_SOURCE_DIR}/../../test_apps/common` → `${CMAKE_SOURCE_DIR}/common`
- `${CMAKE_SOURCE_DIR}/../3dgs_common` → `${CMAKE_SOURCE_DIR}/3dgs_common`
- Drop `displayxr-runtime-pvt`-specific fallbacks (e.g., the `PARENT_BUILD_DIR` OpenXR fallback).

Also create:

- `demos/<your_demo>/CMakeLists.top.txt` — the top-level orchestrator. Uses `add_subdirectory(common)` + `add_subdirectory(3dgs_common)` then platform-specific `add_subdirectory(macos|windows)`.
- `demos/<your_demo>/README.standalone.md` — becomes the public repo's `README.md`. Must cover download, controls, build, and layout. **Include a "Requires the DisplayXR runtime ≥ vX.Y.Z" callout near the top** — this is the contract users rely on to pair versions across repos. Update it whenever the demo starts needing newer runtime behaviour.

See `demos/gaussian_splatting_handle_vk_macos/` for reference files.

## 3. Extend `scripts/publish-demo.sh`

Add a new `case "$DEMO_NAME"` branch in `scripts/publish-demo.sh` that:

1. `rsync`s the demo source dir(s) + any shared libs into the target.
2. Cherry-picks files out of `test_apps/common/` (not a full rsync — textures, DLLs, and Windows-only sources should be selective).
3. Vendors `src/external/openxr_includes/openxr/` into `$TARGET/openxr_includes/openxr/`.
4. Renames every `*.standalone.txt` → `CMakeLists.txt`.
5. Copies the top-level `CMakeLists.top.txt` and `README.standalone.md` to repo root.
6. Copies the pvt `LICENSE` into the target.

Test locally with `./scripts/publish-demo.sh <your_demo> /tmp/demo-scaffold` → `cmake -S /tmp/demo-scaffold -B /tmp/demo-build` → `cmake --build /tmp/demo-build`. The binary should build and run offline.

## 4. Create the public repo

```bash
gh repo create DisplayXR/displayxr-demo-<name> --public \
    --description "DisplayXR demo — <what it does>"
```

Clone it, add a placeholder `README.md`, push to `main`. This gives the publish workflow a branch to write to on first run.

## 5. Add the publish workflow

Copy `.github/workflows/publish-demo-gaussiansplat.yml` to `.github/workflows/publish-demo-<name>.yml` and adjust:

- Trigger tag pattern: `'demo-<name>/v*'` (alongside the back-compat `'v*'`)
- `PROJECT_NAME` references → your demo's exe name
- Downloaded CI artifact name → whatever `build-windows.yml` calls your demo's artifact
- Zip filename → your demo's conventional naming
- Release title

Also extend `.github/workflows/build-windows.yml`'s `on: push: tags:` list to include `'demo-<name>/v*'` so build-windows runs on demo-only tags and produces the demo's artifact.

Trigger a manual dry-run:

```bash
gh workflow run publish-demo-<name>.yml -f ref_name=v0.0.0-demo-test
```

Watch:

```bash
gh run watch
```

Confirm the public repo receives a commit and a tag, and that a Release appears with your binary zip attached.

## 6. Wire up `/release`

The `/release` skill auto-discovers per-demo workflows from `.github/workflows/publish-demo-*.yml` — so once your new workflow file is committed, `/release demo-<name> patch` (and friends) should work. Smoke-test it with an explicit version first: `/release demo-<name> v0.1.0` to avoid surprises from the auto-bump path's "no previous tag" fallback. No edits to `.claude/skills/release/SKILL.md` are needed for the happy path; if the skill's component validator complains, it's likely because the `publish-demo-<name>.yml` file didn't land on main before dispatch.

## 7. Update `CLAUDE.md`

Add a row to the repo table and note the existence of the new demo.

## 8. Test a real release

Tag a real release (`/release patch` or similar). Confirm end-to-end:

- Runtime + shell binary releases still publish normally.
- Your demo repo gets a source sync and a binary release.
- `cube_handle_*` test apps still build locally (no regression from shared changes, if any).

## Patterns to reuse

- **Shared libs (`common/`, `3dgs_common/`) vendor-in, don't submodule.** Publish copies them on every tag. A shared lib contract lives in `test_apps/common/CMakeLists.standalone.txt` — Windows-only sources are guarded with `if(WIN32)` so macOS demos can link `sr_common_base` without hitting `windows.h`.
- **OpenXR headers vendor-in too.** Don't depend on `displayxr-extensions` as a submodule; it's just a flat copy of `src/external/openxr_includes/openxr/`. Simpler, offline-buildable.
- **Don't break the in-tree build.** The standalone files are additive siblings. Local dev on `demos/<your_demo>/` keeps working exactly as before.
