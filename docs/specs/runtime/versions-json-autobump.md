# `versions.json` auto-bump on component releases

## What `versions.json` is

`versions.json` (at the runtime repo root) is the **tested-together
bundle manifest** consumed by `scripts/setup-displayxr.{sh,bat}` to
install a known-compatible matrix on a developer's box. It pins one
tag per DisplayXR component:

```json
{
  "runtime":     "v1.6.1",
  "shell":       "v1.3.1",
  "leia_plugin": "v1.0.7",
  "mcp_tools":   "v0.3.2",
  "demos": {
    "gaussiansplat": "v1.4.3"
  }
}
```

Historically this file was manually curated and drifted constantly —
the runtime field was stuck at `v1.5.0` for three releases (v1.5.1,
v1.5.2, v1.6.0, v1.6.1 all came and went). The auto-bump flow below
keeps it in lockstep with reality at no human cost, while still
catching the one structural compat hazard that component CI can't
check (ABI majors).

## Contract: "released = tested"

The working assumption is that **tagging a release in any DisplayXR
component repo implies that component's standalone CI is green and
that whoever cut the release has validated it against the current
runtime**. The auto-bump flow takes that claim at face value and
promotes the tag straight into `versions.json` — direct commit to
`main`, no PR, no review.

The **one exception** is the plug-in ABI contract, which is the only
cross-component invariant that component-internal CI provably cannot
validate. See "ABI gate" below.

## Topology

```
displayxr-shell-pvt ─── tag v* ──┐                ┌──────────────────────────┐
displayxr-leia-plugin ─ tag v* ──┼─ dispatch ──►  │ displayxr-runtime        │
displayxr-mcp ──────── tag v* ──┤                │                          │
displayxr-demo-* ───── tag v* ──┘                │ versions-bump.yml        │
                                                  │  ├─ ABI gate (leia only) │
displayxr-runtime ──── tag v* ───────────────►   │  └─ commit versions.json │
                       build-windows.yml          └──────────────────────────┘
                       BumpVersionsJsonOnTag
                       (same ABI gate, opposite direction)
```

## Runtime self-bump (lives in this repo)

The `BumpVersionsJsonOnTag` job in
`.github/workflows/build-windows.yml` runs on every `v*` tag push,
gated on `Runtime` + `BundleTestApps` succeeding. It:

1. Reads the currently-pinned `leia_plugin` field from
   `versions.json`.
2. Runs `scripts/check_plugin_abi.py --leia-tag <pin>` against the
   runtime tree being released.
3. **ABI OK** → bumps `versions.json[runtime]` to `GITHUB_REF_NAME`,
   commits via the `displayxr-publish-bot` GitHub App.
4. **ABI mismatch** → skips the bump and opens (or comments on) an
   issue in `displayxr-leia-plugin` titled *"ABI mismatch: leia
   v1.0.X cannot load runtime v1.Y.Z"* asking for a rebuild. The
   orchestrator stack stays on the last known-good bundle.

## Sibling auto-bump (lives in this repo's `versions-bump.yml`)

`.github/workflows/versions-bump.yml` listens for
`repository_dispatch` events of type `versions-bump` from sibling
repos. Each sibling repo's release workflow ends with a dispatch
step (snippets below). The handler reads the dispatched `{field,
tag, source_repo}`, runs the same ABI gate when `field ==
leia_plugin`, and commits the bump.

The handler also supports `workflow_dispatch` so a human can bump
any field by hand from the Actions tab — useful if a sibling repo's
auto-dispatch step is missing, or to manually re-sync after a
known-good runtime+leia coordinated release.

## Sibling-side snippets

Each sibling repo adds **one step** to its existing release workflow
(typically right after the `gh release create` / artifact upload
step). The snippets use the org-level `DISPLAYXR_APP_ID` /
`DISPLAYXR_APP_PRIVATE_KEY` secrets that already exist for the
publish-bot.

### `displayxr-shell-pvt`

In the shell's release workflow, after the binary release lands in
`displayxr-shell-releases`:

```yaml
      - name: Mint dispatch token
        id: dispatch-token
        uses: actions/create-github-app-token@v1
        with:
          app-id: ${{ secrets.DISPLAYXR_APP_ID }}
          private-key: ${{ secrets.DISPLAYXR_APP_PRIVATE_KEY }}
          owner: DisplayXR
          repositories: displayxr-runtime

      - name: Dispatch versions.json bump
        if: startsWith(github.ref, 'refs/tags/v')
        uses: peter-evans/repository-dispatch@v3
        with:
          token: ${{ steps.dispatch-token.outputs.token }}
          repository: DisplayXR/displayxr-runtime
          event-type: versions-bump
          client-payload: |
            {
              "field":       "shell",
              "tag":         "${{ github.ref_name }}",
              "source_repo": "${{ github.repository }}"
            }
```

### `displayxr-leia-plugin`

Same snippet, with `field: "leia_plugin"`. The runtime side runs
the ABI assertion on this field before committing — if the new
plug-in tag was built against an older runtime ABI, the bump is
skipped and an issue is opened back on this repo asking for a
rebuild.

### `displayxr-mcp`

Same snippet, with `field: "mcp_tools"`.

### `displayxr-demo-*`

Same snippet, with `field: "demos.<name>"`. Nested fields are
supported by the handler. Example for the gaussiansplat demo:

```yaml
          client-payload: |
            {
              "field":       "demos.gaussiansplat",
              "tag":         "${{ github.ref_name }}",
              "source_repo": "${{ github.repository }}"
            }
```

## ABI gate (the one carve-out)

The leia plug-in reports `XRT_PLUGIN_API_VERSION_CURRENT` from the
runtime headers it was built against — its `CMakeLists.txt` pins
`DXR_RUNTIME_GIT_TAG` via FetchContent. So a tagged leia release's
ABI version is fully derivable from its source tree at that tag.

`scripts/check_plugin_abi.py` does this derivation:

1. Reads `XRT_PLUGIN_API_VERSION_CURRENT` from this runtime tree's
   `src/xrt/include/xrt/xrt_plugin.h`.
2. Fetches the leia plug-in's `CMakeLists.txt` at the proposed tag,
   extracts `DXR_RUNTIME_GIT_TAG`.
3. Fetches `xrt_plugin.h` at that runtime tag, extracts the same
   macro — that is what the plug-in's `xrtPluginNegotiate()` will
   report.
4. Exit 0 on match, non-zero on mismatch with a clear diagnostic.

The script runs in two places:

- **`versions-bump.yml`** when a leia release dispatches a bump.
- **`build-windows.yml`'s `BumpVersionsJsonOnTag`** when the runtime
  itself ships a new tag — checks whether the *currently-pinned*
  leia still loads against the new runtime.

If either check fails, the bump is dropped and an issue lands on
`displayxr-leia-plugin` describing the mismatch and what to rebuild.

## Why direct commit (no PR review)

Because "released = tested" is the working contract. A merged PR
adds zero validation that the file's `setup-displayxr.sh` consumers
couldn't already get from the GitHub Release notes. The one place
where extra validation *would* help — the ABI gate — happens in CI
before the commit. Anything else is review theatre on a single-line
JSON edit.

The handler uses `concurrency: versions-bump` to serialize event
handlers so two siblings releasing in the same minute don't clobber
each other on `versions.json`, and retries the push once with a
rebase if `main` moves underneath the run.

## Meta-installer bundle

`DisplayXR/displayxr-installer` (the meta-installer bundle, per
project `#284`) stays `workflow_dispatch`-only — it does **not**
auto-rebuild on every component tag. A meta-installer release is a
deliberate "users should upgrade now" event, separate from the
continuous-pin signal that `versions.json` provides to dev-box
users. When you cut a meta-installer, run `workflow_dispatch` on its
build pipeline with the `versions.json` ref as input; it'll pull the
known-good matrix that's been continuously curated by this flow.

## Failure recovery

| Situation | What happens | Recovery |
|---|---|---|
| Sibling dispatches a bump but `versions.json` already pins that tag | Handler logs "nothing to do" and exits 0 | None needed |
| Two siblings race the same `versions.json` write | `concurrency: versions-bump` serializes them | None needed |
| ABI mismatch on leia bump | Bump skipped, issue opened on leia repo | Rebuild leia against current runtime, tag a new release |
| ABI mismatch on runtime self-bump | Bump skipped, issue opened on leia repo | Rebuild leia + tag, *then* re-run `workflow_dispatch` on `versions-bump.yml` with `field=runtime, tag=<this runtime tag>` |
| Sibling repo forgets the dispatch step | `versions.json` doesn't update | Manual `workflow_dispatch` on `versions-bump.yml` with the field + tag, then fix the sibling repo's release workflow |
| `displayxr-publish-bot` token expires | All bumps fail with auth error | Rotate the GitHub App's private key per `.secrets/NOTE.md` |
