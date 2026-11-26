---
status: Accepted
date: 2026-10-16
deciders: David Fattal
issues: []
related: [ADR-053, ADR-054, ADR-042, ADR-048, ADR-046]
---

# ADR-057: Image → splat conversion — one provider per class, models are packs, local or remote hosting; the provider reports facts, the author owns the camera block

**Status:** Accepted (2026-10-16, owner go) · builds on
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
runtime never does. A provider may *suggest* a focus, labelled as a suggestion. *(Amended
2026-11-25: a provider may also write the block into a splat container it produces itself, under
the same schema and focus rule; see "Amendment: a provider may author the block in its own
container".)* Research-only
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
  - **Runtime:** PyTorch CUDA on a cloud GPU (two hosts, one fallback), about 8.5 s
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
  licence, and all of them remain derivatives under `apple-amlr`. One of them,
  `sm079/sharp-onnx-webgpu` (int8, 0.66 GB), runs in a browser tab in a public personal
  experiment page (ONNX Runtime Web 1.30 on WebGPU, about 3.6 s warm in one observation).
  That page runs Sharp on the left eye, gets metric scale from its own CPU stereo matcher
  (census + SGM) by a median depth ratio, and writes a `.sog` with a camera block. It has no
  FoundationStereo and no `local_align`, gives no checkpoint hash, and has no quality
  measurement against PyTorch. It shows the in-page path is feasible, not that it is good.
- **Distilled alternatives.** A smaller distilled model trained against Sharp is being
  evaluated internally. Its licence depends on how it was trained, and that is undetermined.

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

**In-page inference is a web-SDK option, not the runtime's path.** An int8 export has run
in a browser tab, so in-page mono Sharp is possible. It is still 0.4–0.8 GB per page load,
and it cannot run the stereo arm: the stereo model's 3D convolutions fail in ONNX Runtime
Web. On the DisplayXR Browser, splat requests go through `XR_DXR_lift`'s job lane like any
other app, so one provider serves native apps and the browser and the runtime's store holds
the weights once. A web-SDK in-page provider for browsers without DisplayXR is the SDK's
decision. If it ships, it must pin a revision and verify the file hash, carry the research-only
licence, and be A/B'd against the server first.

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

*Amended 2026-11-25: a provider that produces the container itself (a SOG) may write the block
into it; the runtime passes that block through like any authored block. See "Amendment: a
provider may author the block in its own container" below.*

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
  *Amended 2026-11-22, see "Amendment: verified on entry" below.*

### 6. Remote endpoint contract, v1

What a remote endpoint implements, so the provider (S2) and the first endpoint (S3) are built
against one text. HTTPS, `Authorization: Bearer <token>`, JSON in UTF-8.

- **`GET /v1/describe`** returns:
  `{ "contract": 1, "endpoint_id", "tasks": [ { "task": "splats-mono" | "splats-stereo",
  "formats": ["sog", "ply"], "max_long_side", "max_bytes", "rectified_required" } ],
  "licence": { "class": "commercial" | "research_only" | "unknown", "name", "link" },
  "build": { "provider_commit", "model_id", "model_sha256", "runtime", "runtime_version",
  "host_kind", "host_id" } }`.
  The provider calls it on warm-up and on each endpoint switch. A `build` that differs between
  hosts of one endpoint list is logged as a WARN.
- **`POST /v1/jobs`**, `multipart/form-data`:
  - **Images:** `image`, or `left` + `right`.
  - **`params`** (JSON): `task`, `format`, `baseline_m` (omitted = unknown), `rectified`,
    `focal_px` (given; wins over EXIF), `max_long_side`, `options` (the provider's settings,
    passed through).
  - **Synchronous:** the response body is the blob (`application/octet-stream`).
  - **Header `X-DXR-Result`** carries one JSON object with every fact of §4: `build`,
    `refusal { code, reason }`, `coords`, `intrinsics { fx, fy, cx, cy, width, height }`,
    `metric`, `baseline_m`, `metric_fit`, `near_m`, `far_m`, `layers`, `splat_count`,
    `focus_suggestion`, `timings_ms`.
  - **Cancel** is the client closing the connection.
- **Errors:** `422 { code, message }` means bad input; it is never retried and maps to a
  failed job with that reason. `5xx`, `429`, `408`, `401`, `403`, `404` and timeouts fall
  through to the next host in the list.
- **Not in v1:** signed-URL uploads and webhooks for long jobs (video) — contract 2.

### Amendment (2026-11-22): verified on entry, size-checked on load

The tamper rule above ("the host re-hashes a blob on first load", per process) cost about 19 s
for the 2.5 GB fp32 Sharp export and about 4 s for int4 on every `conv run` and every
conversion-host start, and it outlasted a provider's request grace, so the fp32 model failed to
warm at all. The rule is now: **verified on entry to the store; size-checked on every load; full
re-check on demand.**

- A blob is hashed once, when it enters the store: a download while it streams, a `local_build`
  script before it stamps, an unstamped blob by the first request (then stamped). A mismatch
  there refuses it, as before.
- On a load, in any process, the check is stamp + size + a trusted writer (this user, an
  administrator / SYSTEM, root). No hash. A wrong size reads `ABSENT` and is never handed out.
- A blob another local user wrote (the Windows store root lets any user create files) is hashed
  on its first load per process, as before.
- `DXR_CONV_STORE_FULL_VERIFY=1` brings back the per-process hash for every blob;
  `displayxr-cli conv models verify` always hashes. No new file format: the existing `.ok` stamp
  is the entry marker.

Threat model: the load check catches partial writes, truncation and any size change; it does not
catch a same-size in-place edit or bit rot after entry, which the on-demand check is for. A
same-user attacker able to rewrite a blob can rewrite its stamp and the manifest it is checked
against too, so they were out of scope before and still are. Spec:
`conversion-provider-discovery.md` §5.1, §5.3, §5.4.

### Amendment (2026-11-25): a provider may author the block in its own container

§3 said only the app, the SDK or a convenience writer writes the camera block. The open
conversion provider (`displayxr-conversion-open` v0.2.2) now encodes the SOG itself when its
output is `.sog`, and the moment it writes the container is the one moment it knows, exactly,
the focal it unprojected through and the clump it measured. A block added after the fact by
another tool can only repeat those numbers. The gallery's online generator has always done the
same. So the rule is widened, not reversed: **facts stay the provider's, intent stays the
author's, and a provider that builds the container is the author of that file's first block.**

- **A provider MAY write the camera block into a splat container it produces itself** (today a
  SOG's `meta.json` `camera` key). It is never required to: a provider that returns PLY, or a SOG
  without a block, is as conformant as before.
- **When it does, the block follows the one schema and the one focus rule.**
  - The schema is the camera block v2 the runtime already documents
    ([`XR_DXR_lift.md` §5.2](../specs/extensions/XR_DXR_lift.md), discovery spec §2.6):
    `convention`, `rig`, `rest`, `intrinsics` (the focal the Gaussians were actually unprojected
    through, in the pixels of the image the provider was handed), `stereo.baseline_m` only for a
    metric result, `metric` only for a fit, `focus`, `dxr`.
  - The focus is the nearest-substantial-clump rule the gallery uses (the provider's
    `focus_suggestion` with source `NEAREST_CLUMP`); no clump means no `focus` key.
  - The provider-side text is the provider's spec,
    [`docs/sog-camera-block.md`](https://github.com/DisplayXR/displayxr-conversion-open/blob/main/docs/sog-camera-block.md)
    in `displayxr-conversion-open`: the mono block (`rig: "camera"`, identity `rest`, no `stereo`
    key, `dxr` factors 1 / 1) and the rule's constants.
- **Facts and block agree.** A provider that writes a block still returns every fact in
  `xrt_conv_splat_info` / `xrt_conv_splat_result` (or `X-DXR-Result`), and the two carry the same
  numbers: intrinsics, focus suggestion, near / far, metric and baseline. The block never replaces
  the facts.
- **It is an authored block.** It carries no top-level `"source": "auto"`, so every runtime path
  treats it as authored, exactly as it treats one a person wrote.
- **What the runtime does, by case.**
  - **PLY + facts** (today's GAUSSIANS lift path, which asks for PLY): the runtime builds the
    block from the facts, as before.
  - **An authored block** (any `camera` without a top-level `"source": "auto"`, a provider's or a
    person's): passed through unchanged. `conv camera-block` and `conv run --camera-block auto`
    leave the file byte for byte (`kept-authored`). The lift path returns it as authored, with one
    re-expression: when it downscaled the photo, the block's `intrinsics` come back in the
    caller's pixels, scaled exactly as the `facts` are (#2836); the file is never rewritten.
  - **An `auto` block**: the only kind the runtime rewrites. The convenience writer replaces it;
    the lift path builds its own block from the facts and keeps the auto block's focus.
- **Authority when both exist.** A provider-authored block in the file wins over anything the
  runtime would build: the runtime passes it through and never merges its own estimate into it.
  For an `auto` block the runtime keeps authority over the suggestion and may replace it. An app
  or a person may still replace a provider's block (they own the content file); the runtime never
  does.

What does not change: the runtime still never writes or rewrites an authored block, display-time
behaviour stays runtime policy (§3), and no ABI, struct or enum moves. Resolved
([#2836](https://github.com/DisplayXR/displayxr-runtime/issues/2836)): when the lift path
downscaled the photo, a provider-authored SOG block used to come back with intrinsics in the
downscaled pixels while `facts` were scaled to the caller's. The lift result's camera block is now
a view of the file: only its `intrinsics` are re-expressed in the caller's pixels, with the scale
the `facts` get, and the file is never rewritten. (The lift path asks for PLY, so today only the
open provider's `SPLAT_FORMAT=sog` test switch reaches it.)

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
- **In-page ONNX Runtime Web as the runtime's path.** Mono Sharp can run in a tab, but each
  page downloads 0.4–0.8 GB and the stereo arm cannot run there. The runtime path serves the
  DisplayXR Browser instead; an in-page provider stays a web-SDK option (§2).
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

- **A distilled model**: is it a derivative under `apple-amlr`, how good is it against Apple's
  checkpoint, and could it become the commercial or local model?
- **Int8 quality.** No one has measured an int8 Sharp against the PyTorch checkpoint. The
  export spike (S4) must, on the gallery corpus, before any quantized pack ships.

- **Product use of Sharp.** Every path here is research-only until Apple licenses it
  otherwise. Is internal and demo use the only goal, or should S2 also prove a commercial
  model (an open splat model) on the same contract?
- **Who runs the remote endpoint for users outside Leia**, and under which terms?
- **Splat size versus display.** 1.18M Gaussians per photo is fixed by the model's grid. Does
  `max_input_long_side` give a useful size-quality trade on the panels we ship?
