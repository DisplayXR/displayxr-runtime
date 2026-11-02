---
status: Accepted
date: 2026-01-15
source: "#23"
---
# ADR-001: Native Compositors Per Graphics API

## Context
The original Monado architecture used a Vulkan server compositor as the single rendering backend, with interop layers for D3D11/D3D12/OpenGL. For tracked 3D displays, a Vulkan-only compositor puts the composited atlas in a VK texture, which forces one of two weave paths: either **the vendor must supply a Vulkan weaver**, or the runtime must **interop the texture back to the app's native API** to run the vendor's native (e.g. D3D11) weaver.

**Interop-weaving works — this is not "interop is impossible."** The vendor's native weaver runs correctly through VK↔D3D interop on nVidia dGPU, and the generic `comp_multi` + `null` per-session path is architecturally viable. The decision to go native was driven by *broad compatibility* plus the fidelity/dependency wins below, not by an absolute wall.

**The empirical breaker: interop is a driver-quality lottery, and Windows is not VK-first.** VK↔D3D image sharing rides on `VK_KHR_external_memory_win32` + keyed-mutex / timeline-semaphore handshakes whose correctness is ICD-dependent. Windows devices are frequently not Vulkan-first, so their VK drivers are the weaker path — and we hit concrete interop failures on **Intel Iris iGPUs**. The alternative interop-free path — requiring every vendor to ship a **Vulkan weaver** — pushes a real implementation burden onto vendors. Native-per-API compositors avoid both: the atlas is already in the app's graphics API, so the vendor's weaver for that API runs directly, with no interop and no forced-VK-weaver requirement. A related interop failure surface is hybrid-graphics machines (integrated + discrete GPU), where a VK device and the app's D3D device can resolve to *different physical adapters*, so the interop copy crosses the bus or fails; a native compositor uses the app's own device and is same-adapter by construction.

## Decision
Each graphics API gets its own native compositor implementation (D3D11, D3D12, Metal, OpenGL, Vulkan). No Vulkan intermediary. Each compositor directly manages swapchains and rendering in its native API.

## Consequences
More compositor code to maintain (5 implementations vs 1), but each is simpler. Vendor display processors integrate natively. No interop overhead or texture copies (on Windows this no longer holds for sessions on the D3D11 hub; see Amendment 1). Each compositor implements the same `xrt_compositor` vtable; the native compositors fill it by hand (as of 2026-10-23 `comp_base` is used only by `compositor/main/` and `compositor/null/`, not by the five native compositors).

### Why native wins beyond weaving compatibility
Native was **not strictly gating**: the Leia SDK weaver already supports DX, OpenGL, and VK, and interop-weaving worked on nVidia dGPU — so no API *had* to be native just to weave. The five-way surface is justified instead by a stack of independent robustness, fidelity, **performance**, and **platform-reach** benefits:

1. **Native D3D11 unlocks weaver performance features the other paths lack.** The vendor's VK and DX12 weavers do **not** have late latching (and other subtle weave-timing optimizations) today; the native D3D11 compositor gets direct access to those, buying real latency/perf that an interop-to-D3D11 or VK-weave path cannot match. (See `[[project_vk_adaptive_weave_latency]]` for the VK-side workaround this avoids.)
2. **The Vulkan machinery is load-bearing for platform reach, not dead weight.** Monado's VK compositor infrastructure is what powers the **Android out-of-process** path and the upcoming **Linux** support. The VK native compositor earns its place through platform coverage independent of any weaving argument.
3. **Interop sync is a driver lottery; native deletes the class.** See Context — the Intel Iris iGPU failure is a concrete instance; cross-API fence/keyed-mutex sharing is a fragile ICD-quality surface that native side-steps entirely.
4. **No forced Vulkan dependency on the machine.** A single-VK-compositor runtime requires a working VK ICD + loader present for *every* app, even a pure-D3D11 one. Windows devices are frequently not VK-first, so that dependency is a real compatibility risk. Native means a D3D11 app needs only D3D11 — smaller install-prereq surface, fewer first-run failures.
5. **Hybrid-graphics adapter matching.** Native uses the app's own device → guaranteed same adapter (see Context); interop can land the VK device on a different adapter than the app's D3D device.
6. **Format / color fidelity end-to-end.** No cross-API image copy that could re-encode or coerce a format. The app's exact DXGI format + sRGB-view aliasing ([ADR-011](ADR-011-d3d11-typeless-swapchain-textures.md)) reach the weaver untouched, making the color-encoding-state invariant ([ADR-021](ADR-021-color-management-encoding-state-invariant.md)) structural rather than actively defended across an interop boundary.
7. **Native present + transparency semantics.** DXGI flip-model, DirectComposition transparent present ([ADR-029](ADR-029-client-owned-transparent-ipc-present.md)), waitable swapchains, and present timing are D3D/DXGI-native features the weaver and transparent-overlay path depend on; a VK intermediary cannot express them cleanly.
8. **Single-API GPU captures.** PIX/RenderDoc capture the weave path in one API; an interop path splits the capture across two APIs plus shared handles the tools often won't follow — real friction when debugging on partner OEM hardware.

## Amendment 1 — on Windows the in-process tiers can present through one D3D11 hub (ADR-059)

[ADR-059](ADR-059-d3d11-hub-compositor-windows.md) (accepted 2026-10-24) amends this decision on
Windows. It does not supersede it.

**What still holds.** Every graphics API keeps its own compositor and fills the `xrt_compositor`
vtable itself. The app's swapchains, layer accumulation and the atlas are rendered in the app's
API, on the app's device, with no Vulkan intermediary. macOS (Metal, Vulkan, OpenGL), Linux
(Vulkan) and Android (Vulkan) are unchanged.

**What changed on Windows.** The *weave and present* of an in-process session can move to
`compositor/hub_d3d11/` (`comp_hub_d3d11`), one D3D11 compositor that owns its own device, runs the
vendor's D3D11 display processor and presents. For such a session the per-API compositor is an
import shim: it renders its atlas into a D3D11-shareable deposit on its own adapter and hands it
to the hub, read in place on one adapter or carried by the `compositor/xbridge/` cross-adapter
transport otherwise. A D3D12, Vulkan or OpenGL session on the hub is therefore woven by the
D3D11 weaver.

**When it applies.** See ADR-059's status: the in-process D3D11 and D3D12 tiers on a hybrid
(cross-adapter) box by default, the Vulkan tier wherever its output-device split engages, OpenGL
only with `DXR_HUB=1`. A D3D11 or GL session, and a D3D12 session on a hybrid box, logs one
`hub placement: hub=1 … | hub=0 reason=<token>` line. A session the hub refuses (for example
`hosted_window`, `transparent_background`, `shared_texture_session`, `no_shared_fence`), and
every D3D11/D3D12 session under the `DXR_HUB=0` kill switch, keeps the native path this ADR
describes.

**How this squares with the reasoning above.** Interop returns at one boundary, the deposit: for
Vulkan an NT-shared D3D11 texture with an `ID3D11Fence` imported as a timeline semaphore, for
OpenGL `GL_EXT_memory_object_win32` or `WGL_NV_DX_interop2`. ADR-059 accepted that cost on
measurement, per API. Points 1 (D3D11 weaver features) and 5 (adapter matching) are why the hub
is D3D11 and sits on the scanout adapter. Every refusal falls back, by name, to the native leg
rather than failing, and the native legs stay as the kill-switch targets until ADR-059 Phase 5
retires them.
