---
status: Accepted
date: 2026-05-27
---
# ADR-020: Plug-in ABI Compatibility Policy (versioning, `struct_size` negotiation, drift guards)

## Context

ADR-019 split the vendor display drivers into separately-built plug-in DLLs
(`DisplayXR-LeiaSR.dll`, `DisplayXR-SimDisplay.dll`) that the runtime
(`DisplayXRClient.dll`) discovers and negotiates with at `xrCreateInstance`.
The negotiation handshake (`xrt_plugin.h`) was designed with forward-compat in
mind: `xrt_plugin_iface`, `xrt_plugin_host_iface`, and
`xrt_plugin_display_info` each carry a `struct_size` field, are documented as
**append-only**, and the consuming side "MUST NOT read past `struct_size`."

But the **display-processor vtable** the plug-in factory returns —
`struct xrt_display_processor` (and the per-API factory typedefs) — was **left
out of that discipline**. It is an inline C vtable that the runtime calls at
**fixed compile-time offsets**. It has no `struct_size`. It is not mentioned in
the API-version contract. And the plug-in builds it against whatever runtime
headers it pinned via `FetchContent` (`DXR_RUNTIME_GIT_TAG`).

This bit us hard (the multi-day standalone-VK-no-weave / `xrCreateSession`-crash
saga, fixed immediately by leia PR #16):

- Runtime commit `d665014ab` (after `v1.4.1`) **inserted** `is_self_submitting`,
  `on_pause`, `on_resume` into `xrt_display_processor` *before* `destroy`,
  shifting `set_chroma_key` `0x48 → 0x50` and `destroy` `0x50 → 0x68`.
- `XRT_PLUGIN_API_VERSION_CURRENT` was **not** bumped (the version contract only
  ever covered the structs in `xrt_plugin.h`, not the DP vtable).
- The leia plug-in pinned runtime headers at `v1.4.1` while shipping alongside
  runtime `v1.5.2`. So the runtime's `set_chroma_key` call (slot `0x50`) landed
  on the plug-in's `leia_dp_destroy` (which sits at `0x50` in the `v1.4.1`
  layout): **the runtime destroyed the DP immediately after creating it** —
  freeing `ldp`, tearing down the SR weaver. Downstream that manifested as
  "freed/corrupt vtable", a UAF crash, and no weave.
- It reproduced only in CI/shipping because only the CI build uses the pinned
  tag; local/ASan builds point `DXR_RUNTIME_SOURCE_DIR` at the current runtime
  checkout (matching ABI), which masked it for days.

Two structural facts made this possible and will recur unless addressed:

1. **The DP vtable is an unversioned, fixed-offset ABI** with no `struct_size`
   negotiation. *Any* layout change (even a pure append, today) silently breaks
   a plug-in built against older headers.
2. **Nothing enforces that a runtime ABI change bumps the version, nor that a
   plug-in's pinned runtime matches the runtime it is deployed with.** The
   `KEEP IN SYNC` note in the plug-in CI is a comment, and the runtime loader
   *logs* the negotiated `plugin_api` version but never compares it.

We also want plug-ins to be **updatable independently of the runtime** (ship a
new `DisplayXR-LeiaSR.dll` without re-cutting the runtime, and vice-versa)
within a compatible window — lock-step pairing (e.g. a bundle-only pairing
assert) is explicitly *not* the goal.

## Decision

Treat the **entire plug-in-facing ABI**, including the DP vtables, as a single
versioned contract governed by these rules:

### 1. `struct_size` + append-only on the DP vtable

`struct xrt_display_processor` (and each `xrt_display_processor_<api>` —
`_d3d11`, `_d3d12`, `_gl`, `_metal` — factory's returned vtable) gains an 8-byte
header (`uint32_t struct_size; uint32_t reserved_0;`) as its **first fields**,
set by the plug-in factory to `sizeof` at the plug-in's compile time. The
8-byte header (mirroring `xrt_plugin_display_info`) makes the first vtable slot
land at offset 8 on both 64-bit and 32-bit/Android, so the compile-time asserts
anchor at a base offset rather than brittle absolute math. The runtime treats
any vtable slot whose bytes fall at/past `struct_size` as **absent** (NULL /
unsupported) via the `XRT_DP_HAS_SLOT` coverage check on every optional-method
wrapper, exactly as it already does for the optional `xrt_plugin_iface` methods.
New methods are **only ever appended at the end** — never inserted or reordered.
This makes additive evolution forward- *and* backward-compatible **within a
major version**, which is what enables independent updates:

| scenario | behavior |
|---|---|
| newer plug-in (extra appended method) + older runtime | runtime ignores slots beyond its own knowledge → OK |
| newer runtime (calls a new method) + older plug-in | new slot `>= plug-in struct_size` → treated NULL → skipped → OK |
| reorder / remove / signature change | **breaking** → major bump (below) |

### 2. Major version = the compatibility boundary

`XRT_PLUGIN_API_VERSION_CURRENT` is the **major** ABI version. It is bumped
**only** on a non-additive change (reorder, remove, signature change, or adding
a field anywhere but the end of an append-only struct). The negotiation rule is
**same major == compatible; different major == reject**. Additive minor
evolution within a major needs no version change (handled by `struct_size`), so
"plug-in requires runtime min version" reduces to "plug-in and runtime must
share the same API major." Crossing a major requires coordinated updates — and
those are caught loudly (next two rules).

### 3. The runtime loader enforces the major version

`target_plugin_loader.c` must **reject** a negotiated plug-in whose
`plugin_api_version` major differs from the runtime's (today it only logs it).
A rejected plug-in is skipped with a loud error — the runtime falls back to the
next plug-in / `sim_display` and **never** calls through a mismatched vtable.
This is the deploy-time backstop that protects against plug-ins that bypass our
CI (third parties, manual installs, stale bundles).

### 4. A compile-time ABI tripwire keeps the version honest

A block of `_Static_assert`s (committed in `xrt_display_processor.h` and a
companion check for `xrt_plugin.h`) pins the offset of every DP-vtable slot, the
struct size, and the current `XRT_PLUGIN_API_VERSION_CURRENT`. Changing the
layout fails the build (locally **and** in CI), forcing the author to
consciously update the asserts — adjacent to a loud comment that says: *this is
a breaking ABI change; bump the major, follow this ADR, re-pin every plug-in.*
Had this existed, `d665014ab` would not have compiled without a version bump.

### 5. Plug-in CI asserts its pin is self-consistent

Each plug-in repo's CI hard-asserts `DXR_RUNTIME_GIT_TAG` (CMake) == the
workflow's pinned runtime checkout `ref` (the existing `KEEP IN SYNC` comment
becomes a check). This catches "bumped one, forgot the other."

### What we explicitly do NOT do

- **No bundle-level pairing assert as the primary mechanism.** Verifying "the
  leia release in this bundle was built against the runtime this bundle pins"
  enforces *lock-step*, which is the opposite of the independent-update goal,
  only covers the bundle (not standalone/third-party installs), and catches the
  symptom rather than the cause. Rules 1–4 make compatible combinations *work*
  and incompatible ones *fail loud* regardless of how they were assembled.

## Consequences

- Implementing rules 1–3 is a **coordinated, major-version-bumped change**:
  runtime + every in-tree/first-party plug-in rebuild together, and the bump is
  validated end-to-end on hardware. It must **not** gate the immediate leia
  `v1.0.5` fix (PR #16), which is already correctly matched to runtime `v1.5.2`.
  It lands as the next runtime minor/major (e.g. `v1.6.0`) + matching plug-ins.
- Rule 4 (the `_Static_assert` tripwire) and rule 5 (plug-in pin self-check) are
  **safe to land immediately** — they change no runtime behavior, only fail the
  build on an un-versioned ABI change. They are the forward guard that prevents
  the next `d665014ab`.
- Once `struct_size` negotiation is in place, the runtime's existing
  `dp_vtable_looks_sane` guard (runtime `158dfb7c2`) remains as a belt-and-
  suspenders crash-degrader, but should rarely trigger; its "driver heap-reuse
  collision" log wording should be updated to name ABI mismatch as the likely
  cause.
- The independent-update window is bounded by the major version. Documented
  policy: a plug-in built against major *N* runs against any runtime of major
  *N*; a runtime of major *N* runs any plug-in of major *N*. Crossing *N*
  requires both sides to move and is rejected (not corrupted) in the meantime.

## Status / rollout

- **Landed in the v1.5.x line:** rule 4 (compile-time tripwire) + this ADR
  (runtime PR #348).
- **Done (runtime v1.6.0, ABI major 2):** rules 1–3 — `struct_size` + append-only
  on `xrt_display_processor` *and* the per-API vtables (`_d3d11`, `_d3d12`,
  `_gl`, `_metal`), plus loader major-version enforcement in all three
  `target_plugin_loader.c` `try_load_one` variants (Windows registry, POSIX/JSON,
  Android). `XRT_PLUGIN_API_VERSION_CURRENT` bumped `1 → 2`; the tripwire asserts
  rewritten to the `XRT_DP_BASE_OFF`-anchored, struct_size-aware scheme; all
  first-party plug-ins (in-tree sim_display, and leia v1.0.6 out-of-tree) set
  `struct_size` and re-pin to v1.6.0.
- **Plug-in repos:** rule 5 (pin self-consistency CI assert) — leia v1.0.6's CI
  asserts `DXR_RUNTIME_GIT_TAG` == the workflow's runtime checkout `ref`.
