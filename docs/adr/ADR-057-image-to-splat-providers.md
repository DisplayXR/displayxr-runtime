---
status: Proposed
date: 2026-10-16
deciders: David Fattal
issues: []
related: [ADR-053, ADR-054, ADR-042, ADR-048, ADR-046]
---

# ADR-057: Image → splat conversion — one provider per class, models are packs, local or remote hosting; the provider reports facts, the author owns the camera block

**Status:** Proposed (2026-10-16, draft for owner review) · builds on
[ADR-053](ADR-053-conversion-providers.md) (conversion providers, the `SPLATS_MONO` /
`SPLATS_STEREO` job-lane tasks, the schema-2 model store) and
[ADR-054](ADR-054-component-model.md) (component classes, options contract, provenance gate) ·
contract changes land in
[specs/runtime/conversion-provider-discovery.md](../specs/runtime/conversion-provider-discovery.md).

## In one paragraph

Apple's **ML-Sharp** turns one photo into a Gaussian splat, and Leia's gallery already ships it
as a cloud worker that writes `.sog` files with a camera block. This ADR makes that capability
generic. A plug-in is a **provider of a conversion class** — "image (or rectified stereo pair)
→ splats" — never a plug-in per model. Models such as ML-Sharp are **packs** a provider
serves. A provider runs its model **locally** in the conversion host or **remotely** behind an
HTTPS endpoint, and both look identical to the runtime because both use the existing job lane.
The result separates **facts** the model measured (intrinsics, baseline, metric scale or a
refusal, near/far, a build stamp) from **intent**: the rig, the rest view and the focus point
an author chooses. Facts are the provider's to report and never invented. The **camera block
in the content file is authored**: the app or the person who made the picture writes it, the
runtime never does. A provider may *suggest* a focus, labelled as a suggestion. Research-only
weights are never bundled, never mirrored and never pre-fetched; the user downloads them, or
uses a remote endpoint, with the licence shown.

## Context

**What exists (2026-10-16).**

- **The conversion contract already has the tasks** (ADR-053): `SPLATS_MONO` (6) and
  `SPLATS_STEREO` (7) on the job lane (one-shot, pixels in, blob out, progress and cancel).
  `XRT_CONV_BLOB_SOG` is a declared output, and `struct xrt_conv_splat_info` already carries
  `fx, fy, cx, cy`, the grid size, `coords`, `metric`, `baseline_m` (0 = unknown, never
  defaulted), `focus_z_m`, `near_m`, `far_m` and `splat_count`. **No shipping provider
  implements a splat task.**
- **The model store** (P4, #2116) downloads schema-2 packs: hash, size, licence AND over model
  + dependencies + runtime, `RESEARCH_ONLY` selectable only with `AllowResearchOnly`, never
  pre-fetched when non-commercial, progress as `DOWNLOADING`.
- **ML-Sharp in production (gallery and Show).**
  - **Weights:** Apple's stock checkpoint `sharp_2572gikvuh.pt`, 2,809,738,232 bytes, from
    `ml-site.cdn-apple.com`. The official Hugging Face repo `apple/Sharp` holds the same file,
    same size. Licence: Apple ML Research Model Licence (`apple-amlr`), research only, and it
    follows derivatives.
  - **Runtime:** PyTorch CUDA on a cloud GPU (RunPod primary, Modal L4 fallback), about 8.5 s
    per image warm, 40–60 s cold. Output is always 2 layers × 768² = 1,179,648 Gaussians, a
    roughly 10 MB `.sog`.
  - **Stereo:** the winning arm is full FoundationStereo plus `local_align`. `local_align` is
    Apple's own alignment UNet inside the checkpoint, so SHARP re-decodes from stereo-aligned
    depth, with a guarded residual correction on top. Replacing SHARP's depth with stereo depth
    lost: it erases thin structure. FoundationStereo is NVIDIA research-only, so the licences
    stack.
  - **Camera block:** written into the SOG `meta.json` after the fact, because SOG has no
    camera field. It is the `camera` v2 block: `convention`, `rig`, `rest`, `intrinsics`,
    `stereo.baseline_m` (present only when metric), `metric` fit statistics, `focus`, `dxr`.
- **Lessons from 224 real photos**, each of which became a rule:
  - A wrong baseline silently rescales everything.
  - A refused metric fit must say "refused, and why".
  - Rectified and raw pairs need to be declared, not guessed.
  - Cap the input resolution rather than thinning the Gaussians: thinning to half left holes at
    4K.
  - Geometry is deterministic across GPUs; bytes are not.
  - Production ran a weaker stereo model for days because no response said which build
    answered.
- **Third-party exports exist.** Hugging Face hosts ONNX conversions of the same checkpoint:
  fp16 at about 1.3 GB, int8 at about 660–800 MB, int4 at about 430–580 MB, some aimed at
  WebGPU, a Qualcomm QNN build, and an MLX fp16 build. They show that an ONNX export is
  feasible. Their quality is unmeasured, their provenance is unverified, several mislabel the
  licence, and all of them remain derivatives under `apple-amlr`.

## Decision

### 1. A provider serves a class; a model is a pack

A conversion **provider** is an inference backend — the open ONNX provider, a vendor provider,
the remote provider below — and declares which **tasks** it serves. ML-Sharp is a **model**:
a schema-2 pack entry with its weights, licence and the task it implements (`splats-mono`,
`splats-stereo`). A second splat model is a new pack in an existing provider, not a new
plug-in. This is ADR-053's contract applied, not a change to it.

Pre- and post-processing that is specific to one model — Sharp's `local_align`, its residual
correction and its stereo-depth model — lives **inside the provider** behind the task.
`SPLATS_STEREO` is therefore provider-internal: the runtime does not build it by combining
`SPLATS_MONO` with `DISPARITY_STEREO`, because the arm that won depends on a network inside
Sharp's own checkpoint.

### 2. Two hosting modes, one contract

Both modes sit behind the job lane, so an app cannot tell them apart.

- **Local.** The provider runs in its satellite conversion host (ADR-053 D5) with ONNX Runtime
  (DirectML, CUDA, Core ML) on an ONNX export of the model. Gated on the export spike in
  Next steps.
- **Remote.** A generic **remote conversion provider** forwards each job to an HTTPS endpoint.
  It is generic: an endpoint is configuration, not code.
  - **Endpoint list.** Several hosts with one identical contract and a declared fallback order.
    It falls back on 5xx, 429, 408, 401, 403, 404 and timeout. The provider never retries a
    4xx that means "bad input".
  - **Credential** in the OS keychain (Windows Credential Manager, macOS Keychain, libsecret),
    referenced by name from the provider's settings (ADR-054 options contract). It is never
    stored in plain registry or file settings.
  - **Consent.** Sending a photo off the box is a per-provider consent the user grants in the
    Dashboard, revocable, shown while active — the same shape as ADR-043's camera consumer.
    Without consent the remote provider reports `DISABLED`.
  - **Describe.** The endpoint's `/describe` returns its tasks, licence class, limits (max
    input resolution, max file size) and **build stamp**. The provider reports the licence
    class as-is: remote hosting does not make a research-only model commercial.
  - **Long jobs.** Uploads and results go through signed URLs, so the GPU host never holds a
    storage key.

**No in-page inference for this class.** The weights are 1.3–2.8 GB (0.4–0.8 GB quantized)
and the stereo model's 3D convolutions already failed in ONNX Runtime Web. On the DisplayXR
Browser, splat requests go through `XR_DXR_lift`'s job lane like any other app, so one
provider serves native apps and the browser. What the web SDK does on a browser without
DisplayXR is outside this ADR.

### 3. Facts, suggestions, intent — and who writes the camera block

A splat result carries three kinds of information, and they have three owners.

| Kind | Examples | Owner | Rule |
|---|---|---|---|
| **Fact** | intrinsics of the grid actually unprojected through, coordinate convention, `metric` + `baseline_m` + fit statistics, or a refusal reason; near/far; splat and layer counts; build stamp | the **provider** (it measured them) | never invented; absent means unknown |
| **Suggestion** | a focus point (nearest-clump), a subject distance | the **provider**, labelled `source: "auto"` | advisory; an author may discard it |
| **Intent** | `rig` (camera or display), rest pose, `focus.point` = pivot = convergence, `dxr.*` factors | the **author**: the app, the gallery, the person editing the picture | only the author writes it |

**The camera block in the content file is authored.** It records the author's point of view,
a decision about the picture. The runtime returns the blob and the facts and suggestions as
typed fields. The **app or SDK** that saves the content writes the SOG `camera` block,
combining facts with its own intent. Convenience writers — `displayxr-cli conv run`, an SDK
helper — may write a block from facts plus suggestions, marked `source: "auto"`, which an
author overrides. The runtime never rewrites a block it reads.

**What stays runtime policy is display-time behaviour**, as ADR-054 §4.6 says: how a splat is
converged and budgeted *on this display*, for this viewer. That policy *reads* the author's
intent as input. It may adapt to the panel, for example clamping to the depth budget
(ADR-040), but it does not change the file.

### 4. Contract additions

All additive under ADR-020 / ADR-053 rules, in a new top-level `xrt_conv_splat_result` chained
to the job result (the frozen `xrt_conv_splat_info` is not grown):

- **`build_stamp`**: provider build, model id and its SHA-256, inference runtime and version,
  host kind (local or remote endpoint id). Returned with every result and every describe.
- **`refusal`**: an enum (`NONE`, `METRIC_FIT_REFUSED`, `BASELINE_MISSING`,
  `INPUT_TOO_LARGE`, `UNRECTIFIED_PAIR`, …) and a reason string. A refused metric fit still
  returns the non-metric splat with `metric = 0`.
- **`metric_fit`**: scale, source, inlier fraction, MAD(log), principal-point shift
  (deconvergence expressed as a `cx` offset).
- **`layer_count`**, and per layer whether it is visible or inpainted.
- **`focus_suggestion`**: point xyz, subject distance, near/far, source.

**Job inputs** (on `xrt_conv_job_info`, additive):

- `baseline_m` — validated (0 means unknown, so no metric result).
- A **rectified-or-raw** flag for pairs.
- Focal length, as given or from EXIF; given always wins.
- `max_input_long_side`, the knob that bounds splat count.

Model-specific switches (`local_align`, the residual correction) are **settings** under the
ADR-054 options contract, declared by the provider in `describe_settings`. They are not
contract fields.

**Determinism.** Providers promise identical **geometry**, not identical bytes. Consumers key
caches on input hash + parameters + build stamp. A changed build stamp is the "superseded by
build X" signal a downstream copy uses to re-pull.

### 5. Weights: packs, provenance, licence

- **Pack schema additions:**
  - **A per-model `url`.** Today's `<blobBaseUrl>/<sha256>.<format>` naming cannot point at
    Apple's CDN or a Hugging Face `resolve` URL, so a model may give its own URL; the SHA-256
    and size still identify the bytes.
  - **An optional `auth` block** (`kind: bearer | huggingface`, `credential: <keychain
    name>`) for gated repos or Leia's auth server.
- **We do not mirror or bundle research-only weights.** Sharp is `RESEARCH_ONLY`. It is
  selectable only with `AllowResearchOnly`, never pre-fetched by an installer, and downloaded
  only when the user presses Download in the Dashboard, which shows the model, its licence and
  a link to it, its source URL and its size, with progress. `displayxr-cli conv models fetch`
  is the same action from the CLI.
- **Derived formats are built by us from the official checkpoint**, never taken from a
  third-party re-upload. An ONNX or quantized export of Sharp is produced by a DisplayXR
  conversion script from `apple/Sharp` at a pinned SHA-256. Its pack records `provenance`
  (`source_url`, the parent hash, the converter and its version) and carries the parent's
  licence. Where the licence allows us to publish the derivative, it goes to our model
  releases. Where it does not, as for Sharp, the user runs the conversion script locally
  after downloading the official file.
- **The model store's tamper rule is unchanged:** the host re-hashes a blob on first load.

## Consequences

- One provider implementation serves every splat model, local or remote. Adding a model is a
  pack plus, if needed, model-specific code inside the provider.
- The gallery and Show workers become **an endpoint** of the remote provider, unchanged in
  substance. They gain `/describe`, the build stamp and the refusal fields they already
  half-emit as response headers.
- Authors keep their decisions: a re-conversion never overwrites a block an author edited
  (the authored block wins; the new facts are offered alongside).
- The runtime still decides how the content is shown on this display, from the author's
  intent.
- Cost: a remote provider is a privacy surface. Consent, keychain credentials and the endpoint
  allow-list are mandatory, not optional polish.
- Local Sharp may not fit consumer GPUs at an acceptable latency. The export spike decides;
  remote remains the default either way.

## Alternatives rejected

- **A plug-in per model.** It duplicates the hosting, the job lane, the store and the
  licence plumbing for every model, and an app would have to know model names. The class
  contract already exists.
- **The runtime writes the camera block.** It confuses facts with intent. The rig, the rest
  view and the focus are the author's decisions about the picture. A runtime that rewrote them
  would undo an artistic choice on every re-conversion.
- **Runtime composes metric stereo** from `SPLATS_MONO` + `DISPARITY_STEREO`. The arm that
  won depends on a network inside Sharp's own checkpoint and on model-specific guards.
  Composition would ship the arm that lost.
- **In-page ONNX Runtime Web for this class.** The weights are 0.4–2.8 GB, and ONNX Runtime
  Web's operator coverage already failed for the stereo model. The runtime path serves the
  browser instead.
- **Consume third-party ONNX exports from Hugging Face.** Unverified provenance and quality,
  mislabelled licences. We export from the official checkpoint and record the provenance.

## Next steps

| # | Item | Owner |
|---|---|---|
| S1 | Contract: `xrt_conv_splat_result` (build stamp, refusal, metric fit, layers, focus suggestion), job inputs (rectified flag, max input size, validated baseline), pack `url` + `auth`, spec text | mac |
| S2 | Remote conversion provider: endpoint list + fallback, keychain credential, consent row in the Dashboard, `/describe`, signed-URL uploads, `SPLATS_MONO` / `SPLATS_STEREO` | mac |
| S3 | First endpoint: the gallery Sharp worker gains `/describe`, the build stamp and the refusal fields; allow-listed and research-only | gallery-gs |
| S4 | Export spike: ONNX export of `apple/Sharp` by our own script (fp16, int8); measured on DirectML (RTX laptop), CUDA and Core ML (M-series): latency, peak memory, and geometry difference vs PyTorch on the gallery corpus. Go/no-go for a local pack | mac |
| S5 | If S4 passes: local Sharp pack in the open provider; the user downloads the official checkpoint and the export runs locally | mac |
| S6 | Camera-block writer helper in the web SDK and the CLI (facts + suggestions → `source: "auto"` block; never overwrites an authored block) | web SDK |
| S7 | DisplayXR Browser: splat requests through `XR_DXR_lift` jobs | browser |

## Open questions

- **Product use of Sharp.** Every path here is research-only until Apple licenses it
  otherwise. Is internal and demo use the only goal, or should S2 also prove a commercial
  model (an open splat model) on the same contract?
- **Who runs the remote endpoint for users outside Leia**, and under which terms?
- **Splat size versus display.** 1.18M Gaussians per photo is fixed by the model's grid. Does
  `max_input_long_side` give a useful size-quality trade on the panels we ship?
