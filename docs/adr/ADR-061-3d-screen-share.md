---
status: Accepted
date: 2026-11-03
---
# ADR-061: 3D screen share — a running app's pre-weave views are a stereo-camera source of kind app

**Status:** Accepted (2026-11-03; approved by David as recommended — the six open questions of the
draft are resolved as decisions 16–21). Not implemented. Introduces
[`XR_DXR_app_share`](../specs/extensions/XR_DXR_app_share.md) (spec v1, a thin extension that
depends on [`XR_DXR_stereo_camera`](../specs/extensions/XR_DXR_stereo_camera.md)). Extends
[ADR-043](ADR-043-stereo-camera-source.md) with a third source kind; amends the
[ADR-035](ADR-035-service-owned-arbitration-single-pipeline-isolated-satellites.md) D2 lease table
(decision 11) and [ADR-049](ADR-049-in-process-lift-rides-the-service.md) (Amendment 1 there: one
service link, decision 16). Related: [ADR-007](ADR-007-compositor-never-weaves.md),
[ADR-019](ADR-019-vendor-plugin-aux-boundary.md), [ADR-023](ADR-023-unified-atlas-capture.md),
[ADR-030](ADR-030-crop-before-dp-zero-copy-only-when-swapchain-equals-atlas.md),
[ADR-041](ADR-041-fixed-view-count-with-per-frame-activity.md),
[ADR-044](ADR-044-colour-contract-per-backend.md),
[ADR-049](ADR-049-in-process-lift-rides-the-service.md),
[ADR-059](ADR-059-d3d11-hub-compositor-windows.md). Roadmap context:
[3d-capture.md](../roadmap/3d-capture.md) (this is its first streaming case).

## In one paragraph

In a web call in the DisplayXR Browser, the user clicks "Share 3D", picks a DisplayXR app running
on the same machine, and the call sends that app's **pre-weave left/right views** as a
side-by-side video track that every receiver weaves on its own display. It must work offline and,
in v1, for **any** running app on Windows: service-composited and in-process. The runtime already
has everything a stereo video source needs — a service-owned source thread with fan-out, a
pinned NV12 shared-memory ring with per-stream wake handles, consent, an in-use indicator, a kill
switch, live re-enumeration, a CLI probe — and the browser already turns every service camera into
an ordinary `getUserMedia` device. So an app becomes **one more source kind of the stereo camera
stack** (`XRT_STEREO_CAMERA_SOURCE_APP`, next to `PLUGIN` and `UVC`), visible only to consumers that
enable `XR_DXR_app_share`, gated by a per-start runtime prompt, and fed by a non-blocking GPU→NV12
tap in the D3D11 service (service-composited apps) or by a share link from the app process
(in-process apps).

## Context

- **What is useful to a receiver is the views, not the panel.** The woven output is specific to
  the sender's lens and eye position (ADR-007); the atlas before the display processor is not.
  It is already cropped per mode (ADR-030), already has a fixed view count with per-frame activity
  (ADR-041), and has a known colour state (ADR-044). It is reachable today only through one-shot
  PNG captures (`xrCaptureAtlasDXR` / ADR-023, `u_capture_intent`, the `%TEMP%` trigger files).
- **The call encodes on the CPU anyway.** The DisplayXR Browser is built with
  `media_use_openh264=false`; its calls encode VP8 / VP9 / AV1 in software from CPU frames. A GPU
  texture handed to Chromium would be read back by Chromium. So a GPU-only transport buys nothing
  until hardware encode is verified on the endpoints.
- **The stereo-camera stack is the right shape.** ADR-043's camera manager owns sources behind a
  four-operation vtable (`src_open / src_wait_frame / src_release_frame / src_close`,
  `ipc_server_stereo_camera.c`), with one thread per open source, fan-out to every authorised
  stream, a 3-slot latest-wins ring (no section header; `slot_stride × 3`), per-stream wake
  events, consent (`consent_evaluate`), RAW refusal for third-party content (`output_allowed`),
  the foreground rule, session-lock suspend, the tray indicator and kill switch,
  `XrEventDataStereoCamerasChangedDXR`, and `displayxr-cli camera probe`. Amendment 4 already added
  a second, service-owned source (UVC) behind the same vtable without changing the API.
- **The browser is already wired.** Patches 0370–0375 of `displayxr-browser-pvt` wrap
  Chromium's `VideoCaptureDeviceFactory` in the unsandboxed video-capture utility process, which
  opens one `CAMERA_CONSUMER` instance, lists the service's cameras as `videoinput` devices
  (`dxr-stereo:` + persistentId), requests `RECTIFIED` + `NV12` over shared memory, and attaches
  `track.getSettings().displayxrStereo`. A page's existing camera picker therefore lists whatever
  the service enumerates.
- **Two kinds of apps hold the atlas in two places.** A service-composited app's per-client atlas
  lives in the D3D11 service (`render.atlas_texture`, final after `compositor_layer_commit`). An
  in-process app's atlas lives in the app process; on Windows its D3D11, D3D12 and Vulkan tiers
  all feed the D3D11 fill arm (`compositor/xbridge/`, ADR-039 same-adapter engage, ADR-059 hub).
  ADR-049 already gives an in-process session a lazy, sessionless `APP` link to the service for
  lift.
- **Synthetic content has a known zero-disparity plane.** The runtime's off-axis projection puts
  the display plane at zero disparity; `XR_DXR_view_rig` camera rigs map their convergence plane
  onto the display. A receiver does not need to measure it.

## Decision

### Source and API

1. **An app is a stereo-camera source of kind `APP`.** `XRT_STEREO_CAMERA_SOURCE_APP`
   (service-composited) and `XRT_STEREO_CAMERA_SOURCE_APP_LINK` (in-process, decision 9) join
   `PLUGIN` and `UVC` in `enum xrt_stereo_camera_source` and are served behind the existing
   `src_*` dispatch. Everything above that dispatch — thread, fan-out, ring, wake, stats, events,
   indicator, kill switch, session-lock suspend, CLI — is reused as is. App sources live in their
   **own table** in the manager (not `cams[XRT_STEREO_CAMERA_MAX_CAMERAS]`, which is four slots
   that are never reused), and are listed after every camera.
2. **`XR_DXR_app_share` is the public surface: a thin dependent extension.** It adds no entry
   point, only: the flag `XR_STEREO_CAMERA_APP_CONTENT_BIT_DXR` (`0x40`, reserved in the stereo
   camera header); `XrAppShareSourcePropertiesDXR` chained on the properties (pid, executable,
   application name, window rect, content size, view count / active view count,
   `zeroDisparityShiftFrac`, encoding state); `XrAppShareFrameInfoDXR` chained on each acquired
   frame (content rect, mono, content generation, `zeroDisparityShiftFrac`, commit time); and
   `XrSessionShareabilityDXR` on `XrSessionCreateInfo`, the app's own opt-out. App entries are
   flagged `APP_CONTENT | NATIVELY_RECTIFIED | RECTIFIABLE`, with `baselineMm = 0`,
   `horizontalFovDeg = 0`, `displayName` = the app's `XrApplicationInfo` name, and `persistentId`
   = keyed hash of (app executable, instance ordinal, consumer executable).
3. **Only instances that enable the extension see apps.** The state tracker records the
   enablement as an internal client flag on the connection; existing camera consumers never
   suddenly list apps. *(The design pass had a public `XR_STEREO_CAMERA_CLIENT_APP_SHARE_BIT_DXR`
   too; enabling the extension already says it, so v1 keeps one declaration, not two.)*
4. **App entries churn, cameras do not.** An app entry keeps its `cameraId` while its app runs and
   gets it back when the same (executable, ordinal) restarts in the same service run; after its
   app exits it stays `UNAVAILABLE` for 10 s, then leaves the list. This is a declared exception to
   stereo camera spec v4 ("stays listed, never renumbered") for `APP_CONTENT` entries only: cameras
   come first, so no camera index moves; consumers address app entries by `cameraId`.
   `scam_rescan_apps_locked()` rebuilds the set on client connect, session begin / end and a 1 Hz
   tick, then `notify_all_cameras_locked()`.

### Transport and frames

5. **The transport is the existing CPU NV12 ring.** Default 2×1280×720 @ 30 Hz (~83 MB/s);
   2×1920×1080 opt-in (~187 MB/s at 30 Hz). The readback is a non-blocking 3-slot staging ring
   harvested with `D3D11_MAP_FLAG_DO_NOT_WAIT` (template: `svc_cursor_measure_harvest`); a slot
   still in flight is looked at again on the next commit, so the app never waits on the tap.
   `TRANSPORT_D3D11_TEXTURE` stays specified-not-implemented until hardware encode is verified on
   the endpoints; sandbox-safety (a section can carry a Low-IL label) is the second reason.
6. **One frame recipe for both taps.** Views come from the atlas the DP would weave, cropped to the
   mode's content (ADR-030: source rects = tile grid × `pipe_content_w/h`); the middle pair when
   `view_count > 2` (as `lift_pick_pair` does); one view in both halves when one is active
   (ADR-041); scaled to the fixed stream size, letterboxed; alpha dropped; mirrored per
   `atlas_flip_y`. Conversion is `ID3D11VideoContext::VideoProcessorBlt` per eye into the halves of
   an NV12 texture, input colour space from the atlas's recorded encoding state
   (`RGB_FULL_G22_NONE_P709` encoded; `G10` if linear — ADR-044 encode-if-linear), output BT.709
   limited. *(As built: a pixel shader into R8 + R8G8 targets, output BT.601 full range, the
   stereo-camera transport's contract; see the spec §8.)* Per-frame metadata (`XrAppShareFrameInfoDXR`) rides the **acquire reply**, not the ring:
   the ring section has no header to extend (see *Corrections*).
7. **Convergence is metadata, never measured.** `zeroDisparityShiftFrac = 0` in v1: the frame is
   display-referred. Consequently, for an `APP_CONTENT` stream `RECTIFIED` means "row-aligned, zero
   disparity at the sender's display plane", not the camera meaning "zero at infinity"; receivers
   branch on the source kind and apply no camera convergence crop. An app that ignores the
   runtime's FOVs puts its zero plane elsewhere — out of contract, not detectable.

### The two taps

8. **Service-composited apps** (workspace controller, `XRT_FORCE_MODE=ipc`,
   `DISPLAYXR_WORKSPACE_SESSION=1`). The tap is in `comp_d3d11_service.cpp`, at the end of
   `compositor_layer_commit` once the per-client atlas is final (beside the `pipe_content_w/h`
   publish): decimate to the stream rate, copy the cropped views out, convert, `CopyResource` into
   a staging slot, harvest on later commits, publish to the camera thread's double buffer and set
   its event. Hooks `share_tap_attach / share_tap_detach` on
   `struct ipc_server_stereo_camera_platform` keep the IPC layer graphics-free. Two traps:
   - **zero-copy bypasses the per-client atlas** outside workspace mode — while a tap is attached
     the commit forces `zc_reason = "share_tap"` (one more non-proxy guard of the ADR-030 kind);
   - **a client that does not own the panel** (`pipeline_client_texture_flat_locked`) must still
     get its per-client atlas written each commit; the P1 spike proves it.
9. **In-process apps** — the share capability of the in-process **service link**
   (`ipc_client_service_link`, decision 16), the ADR-049 lift link generalised: one sessionless
   `APP` connection per in-process session, opened lazily on a background thread at
   `xrBeginSession` while sharing is on (default **on**; `DXR_APP_SHARE=0` turns the share
   capability off), never launching the service, retried at most every 5 s; idle cost one pipe and
   one registration, no per-frame work. The service lists a link that declares `share` as
   `SOURCE_APP_LINK`. When a stream
   starts, the service queues `SHARE_REQUESTED` on the link; the app's compositor then copies its
   pre-weave content each frame into a 3-texture NT-shared ring with a shared `ID3D11Fence` it
   created, at the ADR-059 D3D11 fill arm — one tap point for D3D11, D3D12 and Vulkan sessions.
   The service imports the ring and runs decision 6 on it. **Constraints, stated:** the ring must
   be on the adapter the service publishes (`client_d3d_deviceLUID`, ADR-037 §7; the ADR-049
   cross-adapter rule) — on a hybrid box the fill arm uses whichever of its devices is on it, and
   if neither is, the entry is `UNAVAILABLE`; OpenGL, a shared-texture session the fill arm
   declines (unless `DXR_HUB=1`, ADR-059 Amendment 3), a kill-switched split
   (`DXR_SPLIT_SAME_ADAPTER=0`, `DXR_WEAVE_ON_SCANOUT=0`) and `DXR_LEGACY_STANDALONE` are listed
   `UNAVAILABLE` with a reason, not silently omitted.

### Privacy and arbitration

10. **Consent is per start, runtime-owned and not delegable.** A separate evaluation from the
    camera table: sharing off (the camera kill switches stop shares too) → no verifiable consumer
    → source opted out / gone → prompt *"Share <app> with <consumer exe>? Deny / Allow"*, every
    start (`share.prompt = always`, default; `per-app-run` remembers until the shared app exits;
    no stored "always", no stored Deny). Not lifted by `DXR_STEREO_CAMERA_DEV_ALLOW`, not covered
    by a registered browser's delegation, not gated by the OS camera switch (no camera is opened).
    Indicator text *"Sharing <app>"*. The shared app need **not** be foreground; the consumer
    visibility rule is the camera's (a `CAMERA_CONSUMER` is exempt); `SUSPENDED` after 1 s without
    commits; app exit ends the stream `SOURCE_LOST` (the page's track `ended`). The consent store
    gains a separate namespace so a camera Allow never doubles as a share Allow.
11. **Who is listed.** Every connection with a running session except `CONTROLLER`, `DIAG`,
    `CAMERA_CONSUMER` and `PRESENT_OWNER` clients (a browser's pages carry other origins'
    content), the consumer's own pid, and opted-out sessions. **ADR-035 D2 gains a holder:** its
    "Atlas capture" row reads "controller or `DIAG`; apps only their own slot" — a consented app
    share stream may read another client's pre-weave views, only through this path, only while the
    prompt's Allow stands.
12. **IPC.** All `stereo_camera_*` messages are reused. Added: `stereo_camera_get_app_source
    {camera_id} → xrt_app_share_source_info`, and the per-frame app metadata in the acquire reply
    (a wire change the client↔service version gate keeps in lockstep). The share link adds its own
    small message set (`share_link_register`, `share_link_poll` for `SHARE_REQUESTED` /
    `SHARE_STOPPED`, `share_link_publish_ring`).
13. **Browser mapping, no picker changes.** Device label `DisplayXR: <app> (3D)`, id prefix
    `dxr-app:`, hint `displayxrStereo.source = 'app'` plus `appName` and `zeroDisparityShiftFrac`.
    The page's own picker lists these devices; no `getDisplayMedia` and no Chromium picker change.
14. **Development and tests.** `DXR_APP_SHARE_DEV=1` (service env, loud WARN) lists app sources in
    the plain camera enumeration with no prompt, so the spike needs no browser rebuild.
    `DXR_APP_SHARE_FAKE=1` is a **service-side** synthetic app source (SBS with a disparity ramp
    and a zero-disparity marker) for headless CI — like the UVC source's `"fake"` backend, not a
    sim_display knob, because the source is the service's, not a plug-in's.
    `displayxr-cli share list|probe` alias `cli_cmd_camera.c`; tests extend
    `tests_stereo_camera_manager.cpp` and `tests_camera_consent.cpp`.
15. **Vendor neutrality.** The tap uses only the OS video processor; tests use the service fake and
    sim_display; no vendor code or name anywhere (ADR-019).

### Resolved at acceptance (the draft's open questions, approved by David 2026-11-03)

16. **One service link per in-process session.** The ADR-049 lift link is generalised into
    `ipc_client_service_link`: one sessionless `APP` connection per in-process session, named
    `<exe> (in-process)`, that declares its capabilities at registration — `{lift}`, `{share}` or
    both. Lift behaviour is unchanged (ADR-049 decisions 1–6: the same calls, states, waits,
    retry and loss rules); what changes is that a session that also shares opens the link at
    `xrBeginSession` instead of at its first lift call, and lift and share ride the same
    connection, so an in-process session costs **one** `APP` slot, never two. Recorded as
    ADR-049 Amendment 1 (append-only).
17. **The browser lists `dxr-app:` devices only once the page holds camera permission.** Before
    that a page sees no app device at all — not even an unlabelled one — so the device count never
    leaks how many DisplayXR apps are running. (The browser's ordinary cameras keep upstream
    behaviour.)
18. **Untappable sessions are listed `UNAVAILABLE` with a reason** (`gl`, `texture_session`,
    `split_disabled`, `legacy_standalone`, `adapter_mismatch`; an opted-out session is not
    listed at all, §5 of the spec). The reason shows in `displayxr-cli share list` and the
    service log, so a booth operator can see why an app cannot be shared instead of wondering
    where it went.
19. **`share.prompt` defaults to `always`.** `per-app-run` (one Allow covers the same consumer and
    the same app until that app exits) is the documented booth / kiosk setting. No value stores an
    "always allow" or a Deny.
20. **v1 keeps `RECTIFIED` + `APP_CONTENT`.** It works with the shipping browser, which asks only
    for `RECTIFIED`. A distinct `XR_STEREO_CAMERA_OUTPUT_DISPLAY_REFERRED_DXR` output is deferred to
    `XR_DXR_app_share` spec v2.
21. **Resolution is a user setting in v1.** `share.resolution` = `720p` (default, 2×1280×720 @ 30
    Hz) | `1080p` (2×1920×1080, the maximum, up to 60 Hz); a consumer's `maxFrameRate` can only
    lower the rate. Consumer-requested resolution (a chained create-info struct) is deferred to
    spec v2.
## Corrections to the design pass (found against `main` ed979674e)

| Design-pass assumption | Code | Resolution in this ADR |
|---|---|---|
| The per-client atlas is `R8G8B8A8_UNORM` + ENCODED | On the pipeline path (`init_client_render_resources`, the default since ADR-035 D3) it is `R8G8B8A8_TYPELESS`, with UNORM and UNORM_SRGB views over the same bytes; `UNORM` only in the legacy standalone resources | A video-processor input view needs a typed format: the tap copies the cropped views into a typed UNORM texture (one `CopySubresourceRegion` per sampled frame) or replaces `VideoProcessorBlt` with a pixel-shader RGB→NV12 pass — the spike picks. The input colour space is read from the client's encoding state, not assumed (decision 6) |
| "Append `slot_meta[3]` to the ring section header behind a layout-version bump" | The section has no header and no version: `slot_stride = align(layout.size, 4096)`, `section_size = slot_stride × 3` | Metadata rides the acquire reply into a chained `XrAppShareFrameInfoDXR` (decisions 6, 12) |
| App sources sit next to cameras in the camera table | `cams[XRT_STEREO_CAMERA_MAX_CAMERAS]` = 4 slots total, never reused (spec v4) | Own table, listed after cameras, a declared v4 exception for app entries (decisions 1, 4) |
| `SIM_DISPLAY_FAKE_APP_SHARE` | The app source is service-owned; a sim_display knob would need a new plug-in slot (and sim_display knobs are settings now, `dp set sim-display …`) | `DXR_APP_SHARE_FAKE=1`, service-side (decision 14) |
| `persistentId = hash(app exe, consumer exe)` | Two concurrent copies of one exe collide | Instance ordinal added to the key (decision 2) |
| "v1 covers ANY running app" | The fill arm declines GL, shared-texture sessions without `DXR_HUB=1`, kill-switched splits, legacy standalone | Those are listed `UNAVAILABLE` with a reason (decision 9) |
| `xrt_stereo_camera_source` is "diagnostics only; the API is source-neutral" | — | `APP_CONTENT` deliberately exposes the kind: a display-referred pair must not be treated as a camera pair (decision 7) |
| ADR-035 D2: atlas capture is controller / DIAG / own slot | — | D2 gains the consented share holder (decision 11) |

## Consequences

- **+** Any running DisplayXR app becomes a 3D call source with no app change, offline, through
  the page's ordinary camera picker; the call module's existing stereo path sends it.
- **+** One privacy surface for every frame the runtime hands out: same kill switch, indicator,
  suspend rules, and a prompt the user cannot have pre-granted.
- **+** Reuse is near total: no new thread model, ring, transport, event or browser capture code;
  `displayxr-cli camera probe` measures a share the day the source exists.
- **+** Hardware-free CI through the service-side fake.
- **−** A per-frame GPU→CPU readback in the service (or a cross-process copy plus readback for an
  in-process app): ~83 MB/s by default. Measured in P1 against a ≤0.5 ms app frame-time budget.
- **−** The shared views are the **sender's** views: they follow the sender's tracked eyes
  (look-around, IPD) and the sender's display geometry; receivers see the viewpoint move.
- **−** Every in-process session that shares now holds a sessionless service link from
  `xrBeginSession` on (one `APP` slot of `IPC_MAX_CLIENTS` = 32; a session that also lifts reuses
  it, decision 16).
- **−** The meaning of `RECTIFIED` forks by source kind; a consumer that ignores `APP_CONTENT` (and
  applies a camera convergence crop) converges wrongly. The dependent-extension gate (decision 3)
  confines that to consumers that asked for apps.
- **−** App devices exist for a page only after it holds camera permission (decision 17), so a
  page must ask for the camera before it can offer "Share 3D".
- **−** Two prompts on the sender for one share in the browser: the page's camera permission
  (once per origin) and the runtime's share prompt (each start).

## Alternatives considered

| Alternative | Verdict |
|---|---|
| **(a) A virtual camera** (Windows 11 `MFCreateVirtualCamera`; a macOS CMIO extension) fed with the app's views | **Deferred** (for reach into third-party call apps). ADR-043's reasons still apply — a media-source install, and no consent model of ours (the OS gate is per-camera, not per-app-shared) — and two new ones: the source runs inside the Frame Server service under another account and would have to reach the user's service pipe across that boundary, and frames carry no stereo metadata (no zero-disparity, no content rect). ADR-043's vendor-lifetime reason does not apply. Its one advantage, working in any browser, is moot while every endpoint runs the DisplayXR Browser |
| **(b′) A real `getDisplayMedia` source type** in the browser | **Rejected.** Upstream picker UI is rebase cost on every Chromium roll, and display-capture devices are created in the browser process (`InProcessVideoCaptureDeviceLauncher`), which already holds the weave client's `XrInstance` (one per process) — a second instance there needs a broker. The camera route lands in the utility process that already hosts a `CAMERA_CONSUMER` instance |
| **(c) Extend the one-shot captures** (`xrCaptureAtlasDXR`, `u_capture_intent`, trigger files) into a stream | **Rejected as the transport**, reused as the recipe: they are blocking one-shot PNG paths with no fan-out, consent or pacing. Their crop and colour logic is what the tap reuses |
| **(d) GPU texture transport** (`TRANSPORT_D3D11_TEXTURE`) | **Deferred.** Software encode reads frames back anyway; revisit when hardware encode is verified on the endpoints |
| **(e) Server-side composition / an SFU that weaves** | **Out of scope.** Must work offline, peer to peer |
| **(f) Share the woven output** | **Rejected** (ADR-007): useless to any other display |

## Phases and effort

Agent-days; Windows-bound work is marked **[win]**.

| Phase | Work | Effort |
|---|---|---|
| **P0** | This ADR + spec + header | done (this PR) |
| **P1 spike [win]** | Minimal service tap + `SOURCE_APP` behind `DXR_APP_SHARE_DEV=1`. `cube_handle_d3d11_win` under `XRT_FORCE_MODE=ipc` → `displayxr-cli camera list` / `camera probe --format nv12 --frames 300`; shipping browser 1.9.1 + a localhost page: `enumerateDevices` → `<video>` + a loopback `RTCPeerConnection` with `getStats`. **Exit:** ≥ 28 fps over 60 s at 2×1280×720; app frame time +≤ 0.5 ms; correct eye order and crop; colours within ±2 after NV12; frames flow while the browser has focus, under the shell AND under forced IPC; loopback ≥ 24 fps without CPU limitation; the track ends on app exit. Also settles the TYPELESS-input question and the non-owner atlas write | 4–5 |
| **P2 runtime v1** | Service tap productised **[win]** (3–4); APP source, own table, rescan, share consent + prompt + indicator, IPC (3–4); extension, state tracker, service fake, tests, CLI (3); in-process share link + fill-arm tap **[win]** (7–9; includes generalising the lift link into the service link) | 16–20 |
| **P3 browser** | Capture component: `dxr-app:` devices, label, `displayxrStereo.source/appName/zeroDisparityShiftFrac`, list-after-permission, enable `XR_DXR_app_share` (3–4) + a ~2 h build + a hardware check | 4–5 |
| **P4 SDK + chat app** | Web SDK content tile (display-referred: no camera convergence) and the `displayxr-chat` "Share 3D" UI; runs in parallel from P1 against the dev-exposed device | parallel |
| **P5 end to end [win]** | Two Windows endpoints, offline LAN, service-composited and in-process apps | 1–2 |
| **P6 macOS** | Same source kind; tap in the macOS service compositor; the browser has no macOS capture variant yet | later |

Release order: runtime → browser (with the runtime floor) → SDK → app.

## Open questions

None. The draft's six were resolved at acceptance (decisions 16–21).

## References

- Spec: [`XR_DXR_app_share.md`](../specs/extensions/XR_DXR_app_share.md); base:
  [`XR_DXR_stereo_camera.md`](../specs/extensions/XR_DXR_stereo_camera.md) §2a (client classes),
  §5.3 (transport), §7 (privacy), §9 / §9a (sources).
- Header: `src/external/openxr_includes/openxr/XR_DXR_app_share.h`; allocation row
  `1004999430–439` in that directory's `README.md`.
- Code: `src/xrt/ipc/server/ipc_server_stereo_camera.c` (`src_*` dispatch, `consent_evaluate`,
  `output_allowed`), `src/xrt/include/xrt/xrt_stereo_camera.h`,
  `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` (`compositor_layer_commit`,
  `init_client_render_resources`, `pipeline_client_texture_flat_locked`,
  `svc_cursor_measure_harvest`, `lift_pick_pair`), `src/xrt/ipc/client/ipc_client_lift_link.c`,
  `src/xrt/compositor/xbridge/`, `src/xrt/compositor/hub_d3d11/`.
- Architecture: [service-architecture.md](../architecture/service-architecture.md).
- Browser: `displayxr-browser-pvt` patches 0370–0375 (the stereo camera capture component).
