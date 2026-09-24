---
status: Active (runtime-pin-bump.yml + pin-rot-canary.yml)
owner: David Fattal
---
# Downstream pin bump + pin-rot canary

## Problem

Two pin classes exist downstream of this repo, and **both only move by hand**:

1. **Runtime-tag pins.** A vendor plug-in builds against a pinned runtime tag
   (`DXR_RUNTIME_GIT_TAG` in `CMakeLists.txt`, `RUNTIME_REF` in its
   `build-windows.yml`). When the runtime ships a new ABI slot, the plug-in must
   repin or the slot compiles out and the runtime silently degrades — exactly what
   the append-only D3D11 slot 20 `set_window` needed (runtime v2.7.0 +
   leia-plugin v2.3.0, both repinned manually).
2. **Third-party SDK pins.** `humbletim/install-vulkan-sdk` with a hardcoded
   version, in every demo repo, the Leia plug-in, and the vendor template.

Today the loop runs one way only: a sibling releases, dispatches
`versions-bump`, and this repo records the new pin in `versions.json`. Nothing
pushes the other way. `build-windows.yml` even documents the manual step in prose
("rebuild the leia plug-in against runtime `<tag>` … tag a new release") — that
comment is the gap this spec closes.

Both classes failed in the same week: the Vulkan pin rotted org-wide (LunarG
deleted the 1.3.283.0 installer; six repos' Windows CI broke mid-release-train),
and the runtime-tag pin needed a hand-written script to move.

## Why CI, not the `/release` skill

The skill only runs when a human types `/release` from a checkout. CI fires on the
tag regardless of who cut it, and the `versions.json` bump already lives in CI —
putting this in the skill would duplicate a working mechanism and skip any tag
pushed another way. The skill's role is to **watch and report** the dispatches, as
it already does for `BumpVersionsJsonOnTag`.

## Mechanism

**`downstream-pins.json`** at the repo root — a reviewable manifest declaring,
per repo, every location a pin lives. Centralized on purpose (mirroring how
`versions.json` is pushed into `displayxr-installer`): one implementation to fix,
and onboarding a new vendor repo is a manifest entry, not a new workflow in their
repo.

Each location carries the file, the key, and a `track`, and each track carries
its **policy** as data — `bump_when: "abi" | "features" | "manual"` (see *Bump
policy* below). A `manual` track is human-owned and the bumper never touches it. Where a repo's CI asserts two locations are *equal* (leia's
self-check compares workflow `RUNTIME_REF` against CMake `DXR_RUNTIME_GIT_TAG`),
the manifest lists both and the bumper writes them in **one commit** — updating
one alone breaks that repo's CI.

**`runtime-pin-bump.yml`** — on a canonical `v*` tag, for each manifest entry:
rewrite the auto-tracked locations, push `chore/runtime-pin-<tag>`, open a PR.

- **PR, never a direct commit.** `versions.json` may direct-commit because it
  *records* a pin; a source pin change alters what compiles, so downstream CI must
  gate it and a human merges. If the bump genuinely breaks a plug-in, the PR sits
  red and harms nothing.
- **Branch name carries the tag** — a fixed branch name strands a stale PR and
  silently attaches the next run to it (the website editorial sync hit exactly
  this).
- **Branch name carries the track too** (`chore/runtime-pin-<tag>-<track>`) — two
  tracks of one repo can qualify on the same tag, and a shared branch would let the
  second overwrite the first.
- **Idempotent**: already at the tag → exit 0, no PR, no noise.
- **Never backwards**: a pinned tag newer than the released one (a patch on an older
  line) is skipped.
- Auth: the `create-github-app-token` publish-bot App, as `versions-bump.yml` uses.

### Bump policy: gated, never tag-chasing

**Do not bump a runtime-tag pin just because a newer tag exists.** In a vendor
plug-in the pin is load-bearing twice over: `installer/CMakeLists.txt` *derives*
`MIN_RUNTIME_VERSION` from `DXR_RUNTIME_GIT_TAG` by regex, so raising the pin
raises the installer's minimum-runtime floor. Chasing every patch release would
make a plug-in installer refuse a runtime it works perfectly well against
(`exit 5` = "runtime below the ABI floor"), for no gain. Even where no installer
derives a floor, a newer pin compiles the plug-in against newer headers and so
raises the runtime it effectively needs.

What *is* worth a repin is something the plug-in can compile in. Each track names
which signal counts, as `bump_when` in `downstream-pins.json`, so policy is data
rather than workflow logic:

| `bump_when` | Repins when, between the pinned tag and the new tag… | Tracks |
|---|---|---|
| `abi` | `XRT_PLUGIN_API_VERSION_CURRENT` changed | leia `windows`, leia `android`, vendor-template `windows` |
| `features` | the **feature surface** of the track's headers changed (below) | leia `linux` |
| `manual` | never — human-owned; `reason` required | — |

**`abi`** compares `XRT_PLUGIN_API_VERSION_CURRENT` at both tags — the same
comparison `scripts/check_plugin_abi.py` performs in the opposite direction, so
the gate reuses its resolver rather than reimplementing ABI compatibility.

**`features`** exists because the ABI gate is blind to the commonest real change.
ADR-020 extends a display-processor vtable by **appending** a slot and publishing a
`XRT_DP_<API>_HAS_<FEATURE>` macro — at an **unchanged** ABI (it has been 5 since
v2.16.0). A plug-in guards the new code on the macro, so at an old pin the code
compiles out **silently**. That is exactly what happened to leia-plugin#264: it
guards the Linux lazy-capture path on `XRT_DP_VK_HAS_TRANSPARENCY_ACTIVE`, first
shipped in runtime v2.21.1; the Linux track sat at v2.18.0 and had to be repinned
by hand (leia-plugin#265). The feature surface of a header set is:

- every object-like `#define XRT_*_HAS_*` feature macro (added, removed, or redefined);
- every function-pointer member of every `struct` — a vtable slot — keyed
  `struct.member`, compared by whitespace-normalised declaration (added, removed,
  or re-signed);
- `XRT_PLUGIN_API_VERSION_CURRENT`, when `xrt_plugin.h` is in the set — so
  `features` is a strict superset of `abi`.

Comments are stripped first, so doc edits, reflowed declarations and new
`static inline` helpers are **not** triggers (a header that changed only that way is
reported as a note in the skip reason). The header set is `feature_surface.headers`
(`xrt_plugin.h` + every `xrt_display_processor*.h`), or the track's own
`feature_headers` — narrowed to what that track's build compiles, so a slot in an
API it never builds cannot raise its floor. Leia's Linux build compiles only the
Vulkan display processor, so its set is `xrt_plugin.h`, `xrt_display_processor.h`,
`xrt_display_processor_vk.h`. `scripts/tests/test_downstream_pin_bump.py` fails if a
new `xrt_display_processor*.h` is not in the default set.

The PR a `features` bump opens writes **both** of the track's locations (CMake tag
and CI `RUNTIME_REF`) in one commit and lists each trigger, e.g. *new feature macro
`XRT_DP_VK_HAS_TRANSPARENCY_ACTIVE` (`xrt_display_processor_vk.h`)*.

**Dry run.** `verdict` computes the decision between any two runtime tags without
reading a downstream repo or writing anything (local tags when present, else
raw.githubusercontent.com):

```
python3 scripts/downstream_pin_bump.py verdict --from v2.21.0 --to v2.21.1 \
    --repo displayxr-leia-plugin --track linux      # BUMP: XRT_DP_VK_HAS_TRANSPARENCY_ACTIVE + slot
python3 scripts/downstream_pin_bump.py verdict --from v2.20.1 --to v2.21.0 \
    --repo displayxr-leia-plugin --track linux      # SKIP: no macro/slot/ABI change
python3 scripts/downstream_pin_bump.py verdict --from vA --to vB --policy abi   # compare policies
```

Replaying v2.16.0 → v2.21.1 (46 release pairs): `abi` fires **zero** times;
`features` over all DP headers fires four times (v2.16.9 background preview on
every API; v2.16.19 `XRT_DP_VK_HAS_FRAME_DROPPED`; v2.18.0
`XRT_DP_VK_HAS_SNAP_WINDOW_RECT`; v2.21.1 `XRT_DP_VK_HAS_TRANSPARENCY_ACTIVE`). The
hand-set Windows (v2.16.9) and Android (v2.16.19) pins sit exactly on two of those —
humans have been applying the feature rule by hand. Moving those tracks to
`features` (with per-API `feature_headers`) is the natural follow-up; it is not
done here because it changes when the Windows installer's floor moves.

**Not visible to either gate:** a layout-only coupling with no macro. Leia's Linux
floor v2.14.6 came from `vk_bundle` ABI-fingerprint fields (#1243) in
`auxiliary/vk`, outside the DP headers, where an older ref simply fails to compile.
That class still needs a human, or a feature macro added alongside it.

**Derived values are not pins.** `installer/CMakeLists.txt` computes its floor from
the pin; a bumper that edits it too would be writing to a value CMake overwrites.
The manifest lists only real pins, and every derived consumer stays out of it.

**`pin-rot-canary.yml`** — scheduled, reads the same manifest's SDK pins and, for
each, HEAD-checks the download URL **and asserts the extracted archive still
contains `Include/` and `Lib/`**. Both halves are load-bearing: LunarG repackaged
the installer at 1.4.313.0 so newer versions extract `Bin/` only, and the action's
own `glslangValidator --version` check still reports green while the build dies
much later at `find_package(Vulkan)`. A URL check alone would therefore pass a
pin that cannot build. Failure updates ONE tracking issue rather than opening a
new one per run.

## Consumer floors — the axis the pins miss

A build pin answers *"what did this repo compile against?"*. It says nothing
about a consumer that never compiles against the runtime at all — the browser
and the engine plug-ins reach it purely over the OpenXR wire, and their real
coupling is the set of extension `SPEC_VERSION`s they were built for.

That gap was not theoretical. `displayxr-browser` hand-typed
`MIN_RUNTIME_VERSION "2.2.3"` in its NSIS while actually requiring
`XR_DXR_weave` spec 8, which first shipped in runtime **v2.8.0**. Every user
between those two versions was told the prerequisite was satisfied and got a
browser whose weave path could not work — the precise failure the check was
added (browser#68) to prevent, reintroduced by the check's own literal going
stale.

`downstream-pins.json`'s `consumer_floors` block closes it, and
`scripts/drift_audit.py::check_consumer_floors` enforces it weekly:

1. **Resolve what each consumer requires.** Scan its `spec_sources` for
   `#define XR_DXR_<ext>_SPEC_VERSION <n>`, live, on every run.
2. **Can the runtime still serve it?** Compare against the runtime's current
   header. A consumer needing a spec the runtime has dropped, or has not
   reached, is a finding.
3. **Derive the true floor.** Binary-search the runtime's release tags for the
   earliest one shipping each required spec; the newest of those is the floor.
4. **Compare against what the consumer advertises.** Under-declaring is a
   finding (users get a broken install told it is fine); over-declaring is only
   a note (needless upgrades, nobody breaks).

### The rule that keeps it honest

**Record where to look, never the version numbers.** The manifest holds file
paths; the numbers are re-read from the consumer's own headers every run, so
the manifest cannot drift from them the way the NSIS literal did. The one
exception is the browser, which vendors no header *file* — it is a
patch series, so `XR_DXR_weave.h` exists only after the patches apply. Its
number is written down, and the audit cross-checks it against prose in
`patches/README.md` (`requires_anchor`), emitting a note when it cannot
confirm it. Since the 2026-09 repo split its key is **`displayxr-browser-pvt`**:
the audit reads the patch series and the NSIS floor, and both live in the private
source repo (the public `displayxr-browser` keeps only releases and assets). That
read needs the publish-bot App token `drift-audit.yml` already mints for
`displayxr-shell-pvt`; on the default `GITHUB_TOKEN` the consumer reads as
unfetchable and is reported, not silently skipped.

### A consumer that vendors nothing points at its pin

The rule above assumes the consumer has a header *file* to scan. The shell has
none: `external/openxr_extensions/` is `.gitkeep` only, because its build clones
`DisplayXR/displayxr-extensions`. Writing its requirements out by hand (the
browser's `requires` escape hatch) would have broken the rule for the largest
`XR_DXR_spatial_workspace` consumer we have.

So a `spec_sources` item may be **either** a string — a path in the consumer's
own repo at its default branch, the original and still the common case — **or**
an object naming another repo at a pinned ref:

```json
"spec_sources": [
  { "repo": "displayxr-extensions", "ref": "v2.16.37",
    "path": "include/openxr/XR_DXR_spatial_workspace.h" }
],
"pin_sync": { "file": ".github/workflows/build-shell.yml",
              "regex": "DXR_EXTENSIONS_REF:\\s*(\\S+)" }
```

The rule survives intact, because a **tag is a pin, not a floor**: the version
numbers are still re-read from headers on every run, just from the mirror the
consumer builds against rather than from its own tree.

That pin is also a new drift vector, and `pin_sync` is its detector. The ref in
the manifest is only the truth if the consumer's build really clones *that* ref;
if the two drift apart, the audit would derive a confident floor from headers the
consumer never compiled with — worse than not auditing it. So `pin_sync` names
where the consumer writes the ref, and the audit compares the two every run: a
mismatch is a `consumer-floor-pin-desync` **finding**, and a file or regex that
will not resolve is a **note** naming the unchecked ref, never a silent pass.
**Use the object form only with a `pin_sync`.** The shell's ref and the
`DXR_EXTENSIONS_REF` in `build-shell.yml` + `scripts/build-shell.bat` are an
atomic group: bump them together (shell-pvt#113).

Two consequences worth knowing:

- **The binary search assumes `SPEC_VERSION`s never decrease** across
  releases. That is the intended contract and has held for every `XR_DXR_*`
  extension; walking one back would make the search report a wrong floor.
- **`gate` decides how loud a violation is.** `hard` (the browser: its
  installer refuses or chains an upgrade) is worth acting on. `soft` (the
  engine plug-ins: name-based detection, degrade with a warning) is worth
  reporting. Vendor plug-ins are deliberately **excluded** — they have a build
  pin their installer derives its minimum from, so `runtime_tag_pins` already
  covers them and listing them here would double-report.

### Behavioural floors — the one thing spec derivation cannot see

Derivation reads extension `SPEC_VERSION`s, so it is blind by construction to a
runtime change that bumps no spec but that a shipped consumer still requires.
That is not a bug in the audit; it is a limit of what specs express. Real case
(runtime#1347): runtime#1336 routes an opaque present-owner to `CLIENT_TEXTURE`
in the service, touching no extension header at all — and the opaque browser
(browser-pvt#2) does not present correctly without it. Its true floor is
**v2.16.3**, the first release carrying #1336, and nothing in any header says so.

A consumer entry may therefore carry a `behavioural_floor`:

```json
"behavioural_floor": {
  "min_runtime": "v2.16.3",
  "commit": "9399479b4...",
  "why":  "the opaque present-owner path ...",
  "ref":  "runtime#1336 = commit above, first released in v2.16.3"
}
```

The audit folds it into the same `max()` as the derived floors, so the advertised
minimum is checked against whichever is stricter. This **deliberately breaks the
rule above** — it hardcodes a version — and the reason it is safe to is the reason
the rule exists: a number copied from a header drifts from the header, but a
behavioural floor has no header to drift from. It is a fact about which release
first carried a behaviour, and unchanging once the release is cut — **so the audit
checks it rather than trusting it.** `commit` is required, and on every run the
audit asks the GitHub compare API whether that commit is identical to or behind
the `min_runtime` tag (the checkout-free equivalent of `git merge-base
--is-ancestor`). A missing `commit`, a tag or commit that does not exist, or a
commit that is not an ancestor of the tag is a `consumer-floor-unverifiable`
finding and the floor is **not** applied — loud, never silently trusted. That is
what stops the hand-typed number from becoming the very "true when written" claim
the rest of this file is designed to avoid. Keep the entry to the behaviours a
consumer genuinely cannot run without.

## Invariant

A pin that no job verifies is a pin that rots silently until a release train
trips over it. Every pin in the manifest is either auto-bumped
under a declared policy, human-owned with a recorded reason, or canary-checked — and every consumer floor is re-derived rather than trusted.
