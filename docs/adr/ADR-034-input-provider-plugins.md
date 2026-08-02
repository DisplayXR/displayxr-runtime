# ADR-034: Input Providers Are a Second Plug-in Type, Not a Display-Processor Extension

**Status:** Proposed (design accepted; implementation pending)
**Date:** 2026-08-02

## Context

DisplayXR inherited Monado's complete VR input stack and kept it: the OpenXR
action system (`oxr_api_action.c`, `oxr_input.c`), the full interaction-profile
table (`auxiliary/bindings/bindings.json`), the `xrt_device` input/haptics
vtable, all five reference spaces, and real 6DOF action-space location over
IPC. What the fork removed was one layer down — the 34 headset hardware
drivers and the OpenVR state trackers. The one in-tree input source today is
the qwerty driver, which the sim-display builder deliberately wires up as an
emulated left/right motion-controller pair
(`target_builder_qwerty_input.c`).

This leaves a gap: **an externally-shipped tracking system has no supported
way to surface real motion controllers to the runtime.** The vendor plug-in
interface (`xrt_plugin.h`, ADR-019) covers exactly the display side — probe,
one head device, display info, eye tracking, a pose-source hook — and
nothing else. Concrete demand exists: camera+IMU tracked consumer
controllers (Joy-Con-class hardware tracked by a display's on-board camera),
phone-as-controller systems, and future hand-tracking sources all need to
feed 6DOF pose + buttons + haptics into the action system so unmodified
OpenXR titles can consume them.

Terminology note: throughout this ADR, "motion controller" means a tracked
hand-held input device. It is unrelated to the *workspace controller*
(shell) concept of ADR-014/016/018/024.

### Options considered

1. **Extend the display-processor vtable** (append `create_input_devices` to
   `xrt_plugin_iface` under ADR-020's append-only rule).
2. **A second, independent plug-in type** with its own header, negotiation
   entry point, ABI version, and discovery root.
3. **A wire protocol only** (runtime listens on loopback; external process
   feeds poses) with no plug-in ABI at all.

## Decision

**Input providers are a second plug-in type** — a separate contract in
`xrt/xrt_input_plugin.h` with its own `xrtInputPluginNegotiate` entry point,
its own `XRT_INPUT_PLUGIN_API_VERSION_CURRENT` (starting at 1), and its own
discovery root (`HKLM\Software\DisplayXR\InputProviders` on Windows; JSON
manifests on POSIX). Discovery, ProbeOrder semantics, and ABI gating mirror
the display-processor loader (`docs/specs/runtime/plugin-discovery.md`,
ADR-020) and share its loader plumbing.

A provider exposes **N `xrt_device`s** (left/right motion controllers
first-class; the device self-describes via `device_type` and its claimed
interaction profile, so generic trackers and hand-tracking sources fit later
without vtable changes). Devices are ordinary `xrt_device`s: pose via
`get_tracked_pose` (providers are expected to push timestamped samples into
`m_relation_history` and predict on demand), input via `update_inputs`,
haptics via `set_output`. The provider owns its own threads and transport.

**Role arbitration:** the system builder loads input providers before the
qwerty fallback. If a provider supplies a left/right pair, it claims
`xrt_system_roles`; qwerty still registers (debug value) but does not claim
the hand roles. A registry override (`HKLM\Software\DisplayXR\Input\
ForceQwerty`) forces the fallback for debugging — a registry gate, not an
env var, per project convention. v1 activates a **single provider**
(first successful probe in ProbeOrder wins, exactly like the DP loader);
multi-provider composition is deferred.

Option 3 is not rejected — it is demoted to *inside* a provider: the
reference `net_input` provider wraps a documented loopback wire protocol
(derived from Monado's removed `remote` driver), so processes that cannot
ship a DLL still have a path. But the runtime-facing contract is the plug-in
ABI, not the socket.

## Consequences

- **A tracking vendor is not a display vendor.** The two contracts version,
  ship, and get certified independently. A display OEM's plug-in never grows
  input obligations; an input vendor never touches weaving. This is the main
  argument against option 1, which would have coupled the two vendor
  populations to one vtable's evolution and forced every DP vendor to
  understand (and ABI-track) input semantics they don't implement.
- **The runtime core stays driver-free** (ADR-019 discipline extends to
  input): in-tree providers (`sim_input`, `net_input`) build as plug-in
  DLLs like sim-display; restored Monado driver code is vendored into
  providers, never linked into the runtime.
- **The intact Monado input stack becomes load-bearing.** No in-repo app
  exercises the action system today; the first real provider will be its
  first real consumer, and latent bugs surface then — this is accepted and
  is exactly why the sim provider + an actions-mode test app ship in the
  same phase.
- **DisplayXR becomes a two-sided interface**: display vendors on one side,
  input vendors on the other, with the same discovery/ABI story on both.
  The vendor-onboarding narrative extends naturally.
- Costs: a second loader path to maintain (mitigated by sharing plumbing),
  a second ABI to police under ADR-020, and an arbitration policy in the
  builder that must stay predictable as providers multiply.
