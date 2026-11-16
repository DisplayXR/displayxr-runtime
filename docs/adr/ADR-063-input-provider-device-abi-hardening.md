---
status: Proposed
date: 2026-11-15
---
# ADR-063: Harden the provider-device ABI — the devices an input provider returns get a versioned contract

**Status:** Proposed (2026-11-15). Not implemented. Extends
[ADR-034](ADR-034-input-provider-plugins.md) (input providers) and applies
[ADR-020](ADR-020-plugin-abi-compatibility-policy.md) (plug-in ABI policy) to the one part of the
input-provider contract it does not reach today. Related: [ADR-019](ADR-019-vendor-plugin-aux-boundary.md)
(the aux boundary), [ADR-054](ADR-054-component-model.md) (the component model),
[`xrt_input_plugin.h`](../../src/xrt/include/xrt/xrt_input_plugin.h),
[input-provider discovery spec](../specs/runtime/input-provider-discovery.md),
[input-provider onboarding](../guides/input-provider-onboarding.md).

## In one paragraph

An input provider's negotiation surface — `xrt_input_plugin_iface` and `xrt_input_plugin_host_iface`
— follows ADR-020: every struct starts with `struct_size`, slots are append-only, and the loader
rejects a mismatched major. But the **devices** the provider hands back from `create_devices` are
raw `struct xrt_device`s: the runtime's internal device struct, inherited from Monado, with no
`struct_size`, no version, a function-pointer table the runtime calls directly, input/output arrays
whose layout is the runtime's, and enums (`xrt_device_name`, `xrt_device_type`,
`xrt_tracking_type`) numbered by declaration order. None of it is covered by input ABI 1. A
provider binary built against one runtime release and loaded by another that added a field to
`xrt_device`, a slot to its vtable, or an enumerator anywhere but the end, loads cleanly, passes
the major check, and then misbehaves — wrong field read, wrong callback called, wrong profile
bound. This ADR proposes a versioned **provider-device descriptor and vtable** that the runtime
wraps in its own internal `xrt_device`, so the struct the provider fills is owned by the plug-in
ABI rather than by the runtime's internals. Recommendation: Option B, landed additively under
input ABI 1, with the raw-`xrt_device` path kept and fingerprint-guarded for a deprecation window.

## Context

### What is covered today and what is not

| Surface | Versioned? | Who allocates | Who reads |
|---|---|---|---|
| `xrt_input_plugin_iface` | yes — `struct_size`, append-only, `XRT_INPUT_PLUGIN_IFACE_HAS` | provider | runtime |
| `xrt_input_plugin_host_iface` | yes — `struct_size`, carved `reserved[]` slots, `XRT_INPUT_PLUGIN_HOST_HAS_*` | runtime | provider |
| `xrt_input_host_*` structs (geometry, pointer events, camera) | yes — `struct_size` first | caller | callee |
| `struct xrt_device` (fields + ~25 function pointers) | **no** | provider | runtime |
| `struct xrt_input` / `xrt_output` arrays | **no** | provider | runtime |
| `struct xrt_tracking_origin` | **no** | provider | runtime |
| `struct xrt_binding_profile` | **no** | provider | runtime |
| `enum xrt_device_name`, `xrt_device_type`, `xrt_tracking_type` | **no** — implicit, positional values | header | both |
| `enum xrt_input_name`, `xrt_output_name` | partly — explicit ids (`XRT_INPUT_NAME(0x1101, …)`), but not frozen by any rule | header | both |

The display-processor contract does not have this gap: a display plug-in returns a device too,
but the runtime builds its head from `xrt_plugin_display_info` (a versioned struct) and the DP
vtables (versioned). The input contract is the only one where the runtime consumes a
provider-allocated internal struct.

### Why it has not bitten yet, and why it will

- **Every in-tree provider is rebuilt with the runtime.** `sim_input`, `net_input`, `ultraleap`,
  `gamepad`, `touch_input` and the stylus reference ship in the runtime's own build, so their
  `xrt_device` always matches.
- **External providers are starting.** The stylus work (ADR-034 Amendment 8) exists so a hardware
  vendor can ship a provider from its own repo, on its own release cadence, against a pinned
  runtime tag — exactly the arrangement in which a skewed struct is undetected.
- **The struct moves.** `xrt_device` is shared with every driver and the IPC layer; it grows when
  Monado-derived features land (body tracking, plane detection and face tracking slots arrived
  this way), and Amendment 8 itself appended enumerators to `xrt_device_name` and
  `xrt_device_type`. Appending at the end of an enum is safe; appending a field in the middle of
  `xrt_device`, or a slot before `destroy`, is not — and the struct's own convention is the unsafe
  one ("Add new functions above destroy", so every new slot moves `destroy`). Nothing enforces
  which kind a change is.
- **The failure is silent.** A shifted `xrt_device` does not fail negotiation. It fails as a crash
  in `xrSyncActions`, a pose read from the wrong slot, or a device bound to the wrong profile.
  Release builds of a vendor DLL have no symbols, and the crash lands in the runtime.

### What a provider actually needs from a device

Looking at every in-tree provider, the device surface a provider uses is small:

- identity: device name (profile), device type (role), a display string, a serial;
- a tracking origin: a frame type and a name;
- an input list: names, current values, active flags, timestamps;
- an output list: names;
- capability flags: orientation/position tracking, hand tracking;
- callbacks: `update_inputs`, `get_tracked_pose`, `get_hand_tracking` (hands only), `set_output`,
  `destroy`;
- optionally alternate binding profiles.

Everything else on `xrt_device` (HMD parts, rendering modes, view poses, distortion, visibility
masks, plane detection, face/body tracking, battery, brightness, form factor, reference-space
usage) is either head-only or unused by any provider, and a provider today must still fill each of
those function pointers with a "not implemented" stub, because the runtime's inline helpers call
them without a NULL check.

## Options

### Option A — Version `struct xrt_device` itself

Put `uint32_t struct_size` (and a layout version) at the front of `xrt_device`, make its tail
append-only, and have the runtime guard every provider-device field and slot read with a
coverage check.

- **For:** no new types; providers keep writing the struct they write today.
- **Against:** `xrt_device` is the runtime's universal internal device type — every driver, the
  builders, the IPC client devices and the state tracker use it. Freezing its layout freezes the
  runtime's internals for the plug-in's sake, and every future internal change must be
  append-only forever. Every call site that can see a provider device (dozens, through inline
  helpers) would need a coverage guard, and missing one is the same silent bug. The input/output
  arrays, the tracking origin and the binding profiles would each need the same treatment.
  Inserting `struct_size` at offset 0 is itself a one-time break for every existing provider
  binary.

### Option B — A thin, versioned provider-device descriptor + vtable (recommended)

The provider no longer returns `xrt_device`. It returns a **provider device**: a small struct the
plug-in ABI owns, which the runtime reads once and wraps in its own internal `xrt_device` adapter.

```c
struct xrt_input_provider_device_desc {
	uint32_t struct_size;
	uint32_t abi_version;          // XRT_INPUT_PROVIDER_DEVICE_VERSION_1
	uint32_t profile;              // xrt_input_profile_id: fixed-value ABI enum, not xrt_device_name
	uint32_t role;                 // xrt_input_device_role: LEFT, RIGHT, ANY, DOMINANT, NAVIGATION, GAMEPAD, NONE
	uint32_t frame;                // xrt_input_frame: RIG_LOCAL, RIG_LOCAL_PHYSICAL, STAGE
	uint32_t caps;                 // ORIENTATION | POSITION | HAND_TRACKING bits
	const char *display_name;
	const char *serial;
	const char *origin_name;       // devices naming the same origin share one
	uint32_t input_count;
	const uint32_t *inputs;        // XRT_INPUT_* ids (explicit, already fixed-valued)
	uint32_t output_count;
	const uint32_t *outputs;       // XRT_OUTPUT_NAME_* ids
	uint32_t alt_profile_count;
	const struct xrt_input_provider_alt_profile *alt_profiles; // struct_size-prefixed
};

struct xrt_input_provider_device_iface {
	uint32_t struct_size;
	uint32_t reserved_0;
	// Fill the CURRENT values of every declared input (by index into desc.inputs).
	xrt_result_t (*update_inputs)(void *dev, struct xrt_input_provider_input_state *states, uint32_t count);
	xrt_result_t (*get_tracked_pose)(void *dev, uint32_t input, int64_t at_ns, struct xrt_space_relation *out);
	xrt_result_t (*set_output)(void *dev, uint32_t output, const struct xrt_input_provider_output_value *v);
	void (*destroy)(void *dev);
	/* Append-only below this line (ADR-020): get_hand_tracking, get_battery, … */
};
```

`create_devices` grows an appended sibling, `create_provider_devices(inst, out_descs, out_ifaces,
out_handles, max, &n)`. The runtime builds one internal `xrt_device` per descriptor, fills every
head-only slot with its own stubs, keeps the input array itself, and forwards the five calls. The
plain-old-data that crosses the boundary — `xrt_space_relation`, `xrt_pose`, the input value
union — is already layout-stable math and is pinned with static asserts.

- **For:** the plug-in ABI covers exactly what a provider uses and nothing else; the runtime's
  `xrt_device` is free to change; the existing ADR-020 machinery (`struct_size`, append-only,
  `HAS` macros) applies unchanged; a vendor writes ~5 callbacks instead of ~25 plus stubs; role,
  profile and frame become **fixed-value ABI enums**, so a reordering in an internal enum cannot
  move a provider's profile. It keeps the pull model, so timestamp-correct prediction stays where
  the knowledge is (the provider), and `m_relation_history` remains an optional helper.
- **Against:** one adapter layer in the runtime (one indirection per call — negligible beside IPC
  and pose prediction); a mapping table from the ABI profile ids to `xrt_device_name`; every
  in-tree provider migrates (mechanical); a new provider-facing input-value struct for
  `update_inputs`.

### Option C — Opaque device handle + host callbacks (push model)

The provider never sees a device struct. It calls host callbacks: `host->create_device(desc,
&handle)`, then pushes state — `host->publish_pose(handle, input, sample)`,
`host->set_input(handle, input, value, ts)`, `host->set_presence(handle, …)` — and registers one
output callback for haptics. The runtime owns the device, the history and the prediction.

- **For:** the smallest provider: no device struct, no prediction code, no threading contract for
  callbacks; the runtime can validate every sample at the boundary.
- **Against:** moves prediction into the runtime, which knows less than the provider about the
  sensor (latency, filter state, IMU rates) — the provider loses the place where a good tracker
  earns its quality; a push model needs a runtime-side history per input with its own sizing and
  locking, on every host (app process and service); hand joints (26 × pose per sample) become a
  heavy push path; it is the largest departure from how every in-tree provider and the Monado
  driver lineage work, so migration is a rewrite, not a mechanical change.

### Option D — Do nothing beyond documentation

Document that providers must be rebuilt against the runtime release they target, and rely on
release discipline.

- **For:** zero cost.
- **Against:** it is the current state, and it is exactly the state in which an external provider
  fails silently. It also blocks a stated goal of the stylus work: a vendor shipping on its own
  cadence.

## Decision (proposed)

**Option B**, delivered in four steps, none of which needs an input-ABI major bump:

1. **Freeze what providers already touch (immediately, no new API).**
   - Give `xrt_device_name`, `xrt_device_type` and `xrt_tracking_type` explicit values for every
     enumerator a provider may use, plus static asserts pinning them; document "append only, never
     renumber" on each.
   - Add a static-asserted **layout fingerprint** of the provider-visible part of `xrt_device`
     (`sizeof`, the offsets of `name`, `device_type`, `tracking_origin`, `input_count`, `inputs`,
     `output_count`, `outputs`, `supported`, `update_inputs`, `get_tracked_pose`,
     `get_hand_tracking`, `set_output`, `destroy`) to `xrt_input_plugin.h` as
     `XRT_INPUT_PLUGIN_DEVICE_LAYOUT`, and an appended iface field `device_layout` the provider
     fills from it. The loader compares the provider's fingerprint with its own and **refuses** a
     provider whose devices it cannot read — turning today's silent corruption into a logged,
     fall-through rejection, like an ABI-major mismatch.
   - A CI check in this repo fails any change that moves the fingerprint without a release note,
     so "the device layout changed" is a reviewed event, not an accident.
2. **Add the descriptor path** (`xrt_input_provider_device_desc`,
   `xrt_input_provider_device_iface`, the appended `create_provider_devices` slot, the runtime
   adapter), announced by `XRT_INPUT_PLUGIN_IFACE_HAS_PROVIDER_DEVICES`. The loader prefers
   `create_provider_devices` when the slot is present; otherwise it uses `create_devices` under the
   fingerprint gate of step 1.
3. **Migrate the in-tree providers and the input template** to the descriptor path; the reference
   docs and the onboarding guide teach only it.
4. **Deprecate the raw path.** After at least two runtime releases with both paths, the raw
   `create_devices` path is removed in the next input-ABI major (2), which is then the only
   breaking step and is announced in advance.

## Consequences

- An external provider built against runtime N keeps working on runtime N+k for as long as input
  ABI 1 lives, without rebuilding, once it uses the descriptor path — the property the display
  side already has.
- Until a provider migrates, the fingerprint gate makes skew a **refusal with a log line** instead
  of a crash. That is a behaviour change for a provider that "happened to work" across a layout
  change; it is intended.
- The runtime gains one adapter (`target_input_provider_device.c` or similar) and a profile-id
  mapping table. The device vtable for providers stops tracking `xrt_device`'s growth.
- Role, profile and frame are spelled twice for a while (the ABI enums and the internal ones); the
  mapping is table-driven and unit-tested.
- Testing: a fixture provider built against a deliberately different device layout (the same
  approach as the ABI-mismatch fixtures `tests_conversion_loader_stub_*_abi`) must be rejected by
  the fingerprint gate and accepted through the descriptor path; the headless selftest covers both.

## Open questions

1. Should `update_inputs` hand the provider the runtime's input array (as today) or a
   provider-facing state array the runtime copies from? The latter is cleaner for the ABI and
   costs one small copy per sync.
2. Hand joints: keep `xrt_hand_joint_set` (already plain data, but large and Monado-shaped) as the
   provider-facing type, or define a provider-facing joint struct with `struct_size`?
3. Should the fingerprint gate also guard `xrt_binding_profile`, or should alternate profiles be
   accepted only through the descriptor path?
4. Is there value in offering Option C's push helpers as an *optional* host convenience on top of
   Option B (a host-side relation history a simple provider can publish into), without making it
   the contract?
