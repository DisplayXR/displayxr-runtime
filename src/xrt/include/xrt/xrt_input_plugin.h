// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Input-provider plug-in negotiation interface.
 *
 * Defines the C ABI the DisplayXR runtime uses to discover and negotiate
 * with input-provider plug-in DLLs at `xrCreateInstance` time — the second
 * plug-in type next to the display-processor contract in `xrt_plugin.h`
 * (ADR-034). An input provider exposes tracked motion-controller
 * @ref xrt_device implementations (6DOF pose + buttons/axes + haptics) to
 * the runtime; the devices flow through the system builder into the intact
 * OpenXR action system.
 *
 * Discovery mirrors the display-processor loader
 * (`docs/specs/runtime/input-provider-discovery.md`):
 * `HKLM\Software\DisplayXR\InputProviders\*` on Windows,
 * `NNN-<name>-input-provider.json` manifests in the same directories the
 * DP loader scans on POSIX. The runtime loads each entry, resolves the
 * single exported symbol `xrtInputPluginNegotiate`, version-gates it, and
 * probes in ProbeOrder — first successful probe wins (single active
 * provider in v1).
 *
 * This contract is deliberately independent of the display-processor
 * ABI: a tracking vendor is not a display vendor. The two ifaces version
 * and ship separately (ADR-034; ABI policy per ADR-020 — struct_size on
 * every struct, append-only vtable, loader-side major reject).
 *
 * Terminology: "motion controller" = tracked hand-held input device, not
 * the workspace controller (shell) of ADR-014.
 *
 * @author David Fattal
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_results.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Forward declaration of the host's device interface (full def in
 * `xrt/xrt_device.h`). This iface only handles `xrt_device *` by pointer.
 */
struct xrt_device;


/*
 *
 * API versioning.
 *
 */

/*!
 * Input-provider plug-in ABI version (major). Independent of
 * @ref XRT_PLUGIN_API_VERSION_CURRENT — the two plug-in types version
 * separately (ADR-034). ADR-020 rules apply unchanged: same major ==
 * compatible; a mismatch is REJECTED by the loader before any vtable
 * dispatch; additive growth appends at the end under `struct_size`
 * cover; reordering / removing / retyping a slot is a major bump.
 */
#define XRT_INPUT_PLUGIN_API_VERSION_1 1

/*!
 * The version the runtime / provider is built against at compile time.
 * Providers returning a different value from `xrtInputPluginNegotiate`'s
 * `*out_plugin_api_version` are rejected by the runtime with a logged
 * error (ADR-020 rule 3).
 */
#define XRT_INPUT_PLUGIN_API_VERSION_CURRENT XRT_INPUT_PLUGIN_API_VERSION_1

/*!
 * The single exported symbol every input-provider DLL must provide.
 * C linkage, no name mangling. Spelled here as a literal so the
 * runtime's `GetProcAddress` / `dlsym` call doesn't drift from the
 * provider side.
 */
#define XRT_INPUT_PLUGIN_ENTRYPOINT_NAME "xrtInputPluginNegotiate"

/*!
 * Upper bound on the number of devices a single provider may return from
 * @ref xrt_input_plugin_iface::create_devices. Generous — a provider
 * typically supplies two motion controllers; the headroom admits future
 * generic trackers without an ABI change.
 */
#define XRT_INPUT_PLUGIN_MAX_DEVICES 8

/*!
 * Linkage decoration providers should put on their
 * `xrtInputPluginNegotiate` definition. Identical semantics to
 * `XRT_PLUGIN_EXPORT` in `xrt_plugin.h`; duplicated so this header
 * stays self-contained for providers that never touch the DP contract.
 */
#if defined(_WIN32) || defined(__CYGWIN__)
#define XRT_INPUT_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define XRT_INPUT_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define XRT_INPUT_PLUGIN_EXPORT
#endif

/*!
 * Guard for a vtable slot that was APPENDED after a provider may have
 * been compiled (ADR-020): true only when the provider's reported
 * `struct_size` covers ALL of @p FIELD's bytes *and* the provider filled
 * it in. Every runtime-side dispatch through an appended slot must go
 * through this — reading past `struct_size` is undefined by contract.
 *
 * Whole-field coverage, exactly like @ref XRT_DP_HAS_SLOT: a `struct_size`
 * that lands anywhere inside the slot (`> offsetof` alone) still leaves
 * part of the pointer outside the provider's allocation.
 */
#define XRT_INPUT_PLUGIN_IFACE_HAS(IFACE, FIELD)                                                                       \
	((IFACE) != NULL &&                                                                                            \
	 (IFACE)->struct_size >= offsetof(struct xrt_input_plugin_iface, FIELD) + sizeof((IFACE)->FIELD) &&            \
	 (IFACE)->FIELD != NULL)


/*!
 * Static-assert helper for this header's ABI invariants, mirroring
 * `XRT_DP_ABI_ASSERT` in `xrt_plugin.h`'s display-processor contract.
 */
#ifndef XRT_INPUT_PLUGIN_ABI_ASSERT
#if defined(__cplusplus)
#define XRT_INPUT_PLUGIN_ABI_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define XRT_INPUT_PLUGIN_ABI_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif
#endif


/*
 *
 * Opaque types.
 *
 */

/*!
 * Opaque per-provider instance handle. The provider defines the concrete
 * layout; the runtime treats it as a `void *` keyed off `probe()`'s
 * out-param and passes it back to every subsequent vtable call.
 */
struct xrt_input_plugin_instance;


/*
 *
 * Liveness.
 *
 */

/*!
 * Is the provider's hardware/transport actually there RIGHT NOW?
 *
 * `probe()` answers "should I be loaded at all" once, at
 * `xrCreateInstance`. This answers the different, *continuous* question
 * the role arbiter asks every time it runs: is there real hardware
 * behind these devices at this instant. A Leap Motion can be unplugged
 * five minutes into a session and plugged back in later; the runtime
 * re-reads this and moves the hand roles between the provider and the
 * qwerty fallback accordingly (ADR-034 "Presence-gated role
 * arbitration").
 *
 * Distinct from "is a hand currently visible": a device whose hardware
 * is PRESENT but which sees nothing simply reports inactive inputs. Do
 * NOT report ABSENT for an empty tracking volume — that would bounce the
 * roles back to qwerty every time the user's hands leave the frame.
 *
 * @ingroup xrt_iface
 */
enum xrt_input_provider_presence
{
	/*!
	 * The provider cannot determine presence right now — typically
	 * still starting up. The arbiter treats this as NOT present, so a
	 * provider that can never tell should leave
	 * @ref xrt_input_plugin_iface::get_presence NULL instead of
	 * returning this; that keeps the pre-presence behaviour (the
	 * provider holds the roles unconditionally).
	 */
	XRT_INPUT_PROVIDER_PRESENCE_UNKNOWN = 0,

	//! Hardware/transport is not there — the runtime falls back to qwerty.
	XRT_INPUT_PROVIDER_PRESENCE_ABSENT = 1,

	//! Hardware/transport is there — the provider holds the hand roles.
	XRT_INPUT_PROVIDER_PRESENCE_PRESENT = 2,
};


/*
 *
 * Iface definitions.
 *
 */

/*!
 * Nominal panel + viewer geometry of the active display processor, filled in
 * by @ref xrt_input_plugin_host_iface::get_display_geometry.
 *
 * @ingroup xrt_iface
 */
struct xrt_input_host_display_geometry
{
	uint32_t struct_size; /* caller sets to sizeof before the call */
	float display_width_m;
	float display_height_m;
	float nominal_viewer_x_m; /* nominal viewer, display-centre origin, +Z toward viewer */
	float nominal_viewer_y_m;
	float nominal_viewer_z_m; /* never the tracked eyes */
};

/*!
 * Host-supplied callbacks the provider may call. Intentionally minimal in
 * v1 — logging, debug-var tracking, and metrics reach providers through
 * the runtime's aux export surface, same boundary discipline as
 * display-processor plug-ins (ADR-019). The `reserved[]` array provides
 * room for future host callbacks without an API version bump.
 *
 * Forward-compat rules:
 *   - Providers MUST NOT dereference any field whose offset is at or
 *     past `host->struct_size`.
 *   - The runtime MAY introduce new callbacks by repurposing reserved
 *     slots. Because a reserved slot is one providers are forbidden to
 *     dereference, and the repurposing keeps the struct's size and every
 *     other offset identical, the API major does NOT move: a provider
 *     detects the callback with the usual `struct_size` coverage check
 *     plus a NULL test (an older runtime leaves the slot NULL).
 *
 * Lifetime: valid for the duration of the `xrtInputPluginNegotiate` call
 * and for the lifetime of the negotiated provider (until
 * `xrt_input_plugin_iface::destroy`).
 */
struct xrt_input_plugin_host_iface
{
	/*!
	 * `sizeof(struct xrt_input_plugin_host_iface)` at the runtime's
	 * compile time. Providers built against an older header MUST NOT
	 * read past this offset.
	 */
	uint32_t struct_size;

	/*!
	 * The API version the runtime advertises. Identical to the
	 * `runtime_api_version` parameter passed to
	 * `xrtInputPluginNegotiate`, duplicated as a structural cross-check.
	 */
	uint32_t host_api_version;

	/*! Nominal panel + viewer geometry of the active display processor. Optional (NULL on
	 *  an older runtime). The CALLBACK is valid for the life of the process from
	 *  xrtInputPluginNegotiate on (persistent host storage); the GEOMETRY is ready from the
	 *  provider's create_devices call on (cached from the display plug-in before any provider
	 *  creates devices) and is static for the life of the system. Earlier calls return a
	 *  distinct error and leave the struct untouched. */
	xrt_result_t (*get_display_geometry)(struct xrt_input_host_display_geometry *inout);

	/*!
	 * Reserved space for forward-compatible host-supplied callbacks.
	 * Providers MUST NOT dereference any reserved slot.
	 */
	void *reserved[13];
};

// The get_display_geometry slot was carved out of the former reserved[0]: the
// host iface's size and every other offset are unchanged, so providers built
// against the previous header keep working and the API major stays 1.
// clang-format off
XRT_INPUT_PLUGIN_ABI_ASSERT(offsetof(struct xrt_input_plugin_host_iface, reserved) ==
                                offsetof(struct xrt_input_plugin_host_iface, get_display_geometry) +
                                    sizeof(void *),
                            "xrt_input_plugin_host_iface: get_display_geometry must occupy the former reserved[0]");
XRT_INPUT_PLUGIN_ABI_ASSERT(sizeof(struct xrt_input_plugin_host_iface) ==
                                2 * sizeof(uint32_t) + 14 * sizeof(void *),
                            "xrt_input_plugin_host_iface size changed - see ADR-020, this needs an API major bump");
// clang-format on

/*!
 * The provider's vtable. Filled in by the provider inside its
 * `xrtInputPluginNegotiate` implementation and handed back via the
 * `out_iface` out-param. Storage is owned by the provider; the runtime
 * treats `*out_iface` as a read-only borrow.
 *
 * Forward-compat rules (ADR-020):
 *   - The runtime MUST NOT dereference any field whose offset is at or
 *     past the provider's reported `struct_size`.
 *   - New fields are only ever APPENDED at the end, forever. Reordering
 *     or redefining an existing field bumps
 *     @ref XRT_INPUT_PLUGIN_API_VERSION_CURRENT.
 *
 * Lifetime: must remain valid until `destroy()` is called.
 */
struct xrt_input_plugin_iface
{
	/*!
	 * `sizeof(struct xrt_input_plugin_iface)` at the provider's compile
	 * time. Lets the runtime detect providers built against a newer
	 * header and clamp its reads.
	 */
	uint32_t struct_size;

	/*!
	 * Reserved for alignment. Must be 0.
	 */
	uint32_t reserved_0;

	/*!
	 * Short identifier. UTF-8. Matches the registry / manifest `<id>`
	 * used at discovery (e.g. `"sim-input"`). Storage owned by the
	 * provider; must remain valid for the provider's lifetime.
	 */
	const char *id;

	/*!
	 * Human-readable display name. UTF-8. Logged at probe.
	 */
	const char *display_name;

	/*!
	 * Optional publisher name. UTF-8. May be NULL.
	 */
	const char *vendor;

	/*!
	 * Optional version string. UTF-8. Logged at probe. May be NULL.
	 */
	const char *version;

	/*!
	 * Does this provider want to claim the current system?
	 *
	 * Cheap "is my hardware/transport present" check — sub-millisecond
	 * budget, called on the `xrCreateInstance` hot path for every
	 * registered provider until one succeeds.
	 *
	 * On success: returns `XRT_SUCCESS` and sets `*out_inst` to a
	 * provider-defined handle (NULL is legal for providers keeping
	 * their state in file-scope statics). The runtime frees the
	 * instance via `destroy()`.
	 *
	 * On clean decline: returns `XRT_ERROR_PROBER_NOT_SUPPORTED` —
	 * "no hardware of this type on this system." The runtime logs an
	 * info-level line and moves to the next registered provider.
	 *
	 * Other `XRT_ERROR_*` codes are hard probe failures: logged at
	 * warning level, the provider is skipped.
	 */
	xrt_result_t (*probe)(struct xrt_input_plugin_instance **out_inst);

	/*!
	 * Create ALL the input devices this provider supplies — at most
	 * @p max_count, writing the actual count to @p out_count. Called
	 * once, only after a successful `probe()`.
	 *
	 * Each returned device is an ordinary @ref xrt_device that
	 * self-describes: `device_type`
	 * (`XRT_DEVICE_TYPE_{LEFT,RIGHT,ANY}_HAND_CONTROLLER`, …) plus the
	 * interaction profile it binds via `name` / `binding_profiles`
	 * (an existing profile from `bindings.json`). The device must
	 * implement `update_inputs`, `get_tracked_pose` (timestamp-correct
	 * prediction — push samples into `m_relation_history` and predict
	 * on demand for asynchronously-fed hardware), `set_output`
	 * (haptics; may be a no-op), and `destroy`.
	 *
	 * Providers never supply a head device — the display processor /
	 * builder owns the head (ADR-034).
	 *
	 * Ownership of the devices transfers to the runtime, which destroys
	 * them via `xrt_device::destroy` at system teardown.
	 */
	xrt_result_t (*create_devices)(struct xrt_input_plugin_instance *inst,
	                               struct xrt_device **out_devices,
	                               uint32_t max_count,
	                               uint32_t *out_count);

	/*!
	 * Free `inst` and all provider-owned resources hanging off it
	 * (threads, transport). Devices already handed to the runtime are
	 * destroyed separately via their own `xrt_device::destroy`. After
	 * this returns, the runtime stops dereferencing the vtable.
	 */
	void (*destroy)(struct xrt_input_plugin_instance *inst);

	/* Append-only below this line, forever (ADR-020). */

	/*!
	 * Snapshot of whether the provider's hardware is actually present —
	 * see @ref xrt_input_provider_presence. Appended under `struct_size`
	 * cover, so the API major stays 1 and providers built before it
	 * still load; the runtime guards every call with
	 * @ref XRT_INPUT_PLUGIN_IFACE_HAS.
	 *
	 * Called from the runtime's role arbiter, which runs on the
	 * `xrSyncActions` path (and over IPC, per client, per frame). It
	 * MUST therefore be non-blocking and allocation-free: return a
	 * value your own transport thread maintains, never perform
	 * discovery or I/O here.
	 *
	 * May be called before, after, and between `create_devices()`
	 * calls, and with a NULL @p inst if that is what `probe()` produced.
	 *
	 * Leaving this NULL is legal and means "assume present" — the
	 * pre-presence ADR-034 behaviour where the provider holds the hand
	 * roles for as long as it is loaded.
	 */
	enum xrt_input_provider_presence (*get_presence)(struct xrt_input_plugin_instance *inst);
};


/*
 *
 * Entry point.
 *
 */

/*!
 * Signature of the single C-ABI symbol each input-provider DLL must
 * export as @ref XRT_INPUT_PLUGIN_ENTRYPOINT_NAME
 * (`"xrtInputPluginNegotiate"`).
 *
 * @param      runtime_api_version    The @ref XRT_INPUT_PLUGIN_API_VERSION_CURRENT
 *                                    the runtime speaks. Providers may
 *                                    return `XRT_ERROR_PROBER_NOT_SUPPORTED`
 *                                    to bail cleanly on a mismatch; the
 *                                    runtime also enforces the major match
 *                                    by rejecting any provider whose
 *                                    @p out_plugin_api_version differs.
 * @param      host                   Pointer to the host's iface;
 *                                    `host->struct_size` bounds reads.
 * @param[out] out_iface              The provider's vtable. The provider
 *                                    MUST set `(*out_iface)->struct_size`
 *                                    to its own compile-time
 *                                    `sizeof(struct xrt_input_plugin_iface)`.
 * @param[out] out_plugin_api_version The major the provider implements.
 *                                    Must equal the runtime's major or the
 *                                    loader rejects the provider.
 *
 * @return `XRT_SUCCESS` on negotiation success — the runtime proceeds to
 *         call `(*out_iface)->probe()`. `XRT_ERROR_PROBER_NOT_SUPPORTED`
 *         to decline cleanly. Any other `XRT_ERROR_*` is a hard failure:
 *         logged and skipped.
 */
typedef xrt_result_t (*xrt_input_plugin_negotiate_fn_t)(uint32_t runtime_api_version,
                                                        const struct xrt_input_plugin_host_iface *host,
                                                        struct xrt_input_plugin_iface **out_iface,
                                                        uint32_t *out_plugin_api_version);


#ifdef __cplusplus
}
#endif
