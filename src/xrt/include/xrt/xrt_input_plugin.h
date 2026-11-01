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

#include "xrt/xrt_camera_pose.h"
#include "xrt/xrt_component_settings.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_stereo_camera.h"

#include <stdbool.h>
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
 * What happened to one pointer in a @ref xrt_input_pointer_event. The values
 * are the platform-neutral subset every pointer source has (Android
 * `MotionEvent` actions, Win32 / X11 / Cocoa mouse, pen); a source maps its
 * own vocabulary onto these before publishing.
 *
 * @ingroup xrt_iface
 */
enum xrt_input_pointer_action
{
	//! A pointer went down. The first down of a gesture AND every later finger.
	XRT_INPUT_POINTER_ACTION_DOWN = 1,
	//! A pointer moved. One event per pointer that moved (never a batch).
	XRT_INPUT_POINTER_ACTION_MOVE = 2,
	//! A pointer went up (lifted / button released).
	XRT_INPUT_POINTER_ACTION_UP = 3,
	//! The gesture was taken away (Android `ACTION_CANCEL`, capture lost): every pointer is up.
	XRT_INPUT_POINTER_ACTION_CANCEL = 4,
};

/*!
 * One pointer event on a surface the runtime renders into, as drained by
 * @ref xrt_input_plugin_host_iface::poll_pointer_events.
 *
 * Coordinates are SURFACE pixels, origin top-left, y down — exactly what
 * the window system reports — together with the surface's extent at the
 * time of the event, so a provider can normalise without a second query
 * (and without ever learning where the surface sits on the panel). A
 * provider that maps the surface onto the display plane assumes the
 * surface fills the panel, which is exact for a fullscreen `_hosted`
 * surface and an approximation for a sub-panel window.
 *
 * `struct_size` first, like every struct on this ABI (ADR-020): the queue
 * element may grow at the end under its cover.
 *
 * @ingroup xrt_iface
 */
struct xrt_input_pointer_event
{
	uint32_t struct_size;    //!< `sizeof(struct xrt_input_pointer_event)` at the publisher's compile time
	uint32_t action;         //!< @ref xrt_input_pointer_action
	int64_t timestamp_ns;    //!< Monotonic clock (`os_monotonic_get_ns` domain) of the event
	uint32_t pointer_id;     //!< Stable id of this pointer across its DOWN..UP life (Android pointer id)
	uint32_t pointer_count;  //!< Pointers down AFTER this event (0 after the last UP / a CANCEL)
	float x;                 //!< Surface pixels, origin top-left
	float y;                 //!< Surface pixels, y down
	uint32_t surface_width;  //!< Surface extent in pixels at the time of the event; 0 = unknown
	uint32_t surface_height; //!< Surface extent in pixels at the time of the event; 0 = unknown
};

/*!
 * Opaque handle of one shared-camera stream opened through
 * @ref xrt_input_plugin_host_iface::open_shared_camera. Host-owned; valid
 * until @ref xrt_input_plugin_host_iface::close_shared_camera.
 */
struct xrt_input_host_camera;

/*!
 * What a provider asks for in @ref xrt_input_plugin_host_iface::open_shared_camera
 * (the "in" fields) and what the host hands back (the "out" fields).
 *
 * The out fields are the values a `CAMERA_CONSUMER` client of the service
 * gets over IPC (XR_DXR_stereo_camera, ADR-043): the camera's properties, its
 * RECTIFIED calibration, the stream's shared-memory ring layout and the
 * stream's wake handle — so a provider written against this slot is one
 * `#ifdef` away from the IPC form.
 *
 * `struct_size` first (ADR-020): the caller sets it to its own `sizeof`; the
 * host reads and writes only the fields that size covers.
 *
 * @ingroup xrt_iface
 */
struct xrt_input_host_camera_open
{
	uint32_t struct_size; //!< in: `sizeof(struct xrt_input_host_camera_open)` at the provider's compile time
	//! in: enum xrt_stereo_camera_output. RECTIFIED only — RAW is refused
	//! (`XRT_ERROR_NOT_AUTHORIZED`), as it is for a present-owner client.
	uint32_t output;
	//! in: xrt_stereo_camera_properties::camera_id, or 0 = the active display's own camera.
	uint64_t camera_id;
	//! in: delivery cap in Hz; 0 = the source's rate.
	float max_frame_rate;
	uint32_t reserved; //!< in: must be 0

	struct xrt_stereo_camera_properties props;     //!< out: the opened camera
	struct xrt_stereo_camera_calibration calib;    //!< out: RECTIFIED calibration (zeroed when none)
	struct xrt_stereo_camera_stream_layout layout; //!< out: the stream's ring (GRAY8 side-by-side)
	//! out: signalled after every published frame. HOST-owned: valid until
	//! close, never closed or signalled by the provider (Windows: an
	//! auto-reset event; POSIX: the read end of a non-blocking pipe).
	xrt_graphics_sync_handle_t wake;
};

/*!
 * One frame acquired through @ref xrt_input_plugin_host_iface::camera_acquire.
 * The planes point into the host's ring and stay byte-stable until
 * @ref xrt_input_plugin_host_iface::camera_release of this frame or the next
 * acquire, whichever comes first. Side by side: plane 0 is the full SBS image
 * (left eye = left half); plane 1 is the second plane of a two-plane format
 * (NULL for GRAY8).
 *
 * @ingroup xrt_iface
 */
struct xrt_input_host_camera_frame
{
	uint32_t struct_size;  //!< `sizeof(struct xrt_input_host_camera_frame)` at the provider's compile time
	bool time_is_exposure; //!< @ref timestamp_ns is the exposure time (else the delivery time)
	uint64_t frame_index;  //!< the source's sequence number (> 0)
	int64_t timestamp_ns;  //!< os_monotonic clock
	uint32_t width;        //!< full side-by-side width
	uint32_t height;
	uint32_t format; //!< enum xrt_stereo_camera_format (GRAY8)
	//! Moves when the camera's calibration changes: re-read it (close + reopen).
	uint32_t calibration_generation;
	const uint8_t *plane[2];
	uint32_t row_pitch[2];
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
	 * The one options contract (ADR-054 decision 7, N3c): the provider's
	 * per-component settings, read ONLY through this slot — never
	 * `getenv` — with `id` = the provider's own `iface->id`. The runtime
	 * resolves `DXR_INPUT_<ID>_<KEY>` in the hosting process's environment,
	 * then the per-user store, then the registration row's `Settings\<KEY>`
	 * / manifest `settings` (`xrt/xrt_component_settings.h`;
	 * `displayxr-cli input options|set|unset`). Valid for the life of the
	 * process (copy it at negotiate time); read at init, not per frame.
	 *
	 * Carved out of the former reserved[1], like @ref get_display_geometry:
	 * size and every other offset unchanged, the API major stays 1. An older
	 * runtime leaves it NULL — providers MUST NULL-check (and cover-check
	 * `struct_size`). Announced by @ref XRT_INPUT_PLUGIN_HOST_HAS_GET_SETTING.
	 */
	xrt_component_get_setting_fn_t get_setting;

	/*!
	 * Drain the host's pointer-event queue (touch, mouse, pen — whatever the
	 * surface the runtime renders into receives). The runtime OWNS the
	 * surfaces and so is the only side that ever sees their input events:
	 * its own hosted `SurfaceView` on Android, an app-owned surface fed
	 * through `xrSubmitAndroidInputEventDXR`, and — later — a workspace
	 * controller forwarding the events it hit-tests (#1996). A provider
	 * that turns those events into devices (the touch provider's
	 * navigation device + emulated controller) reads them ONLY here and
	 * keeps every bit of gesture policy on its own side (ADR-034
	 * Amendment 4: no gesture policy enters the runtime).
	 *
	 * Copies up to @p max events, oldest first, into @p out and returns how
	 * many. Non-blocking, allocation-free, safe from any thread; call it
	 * from `update_inputs`, which `xrSyncActions` sweeps at frame rate.
	 * The queue is process-wide and single-consumer by convention (the
	 * runtime loads at most one provider that consumes it), and it drops
	 * the OLDEST event when it overflows — a stale MOVE is worth less than
	 * the UP that follows it.
	 *
	 * Carved out of the former reserved[2], like the two slots above: size
	 * and every other offset unchanged, the API major stays 1. An older
	 * runtime leaves it NULL — providers MUST NULL-check (and cover-check
	 * `struct_size`). Announced by
	 * @ref XRT_INPUT_PLUGIN_HOST_HAS_POLL_POINTER_EVENTS.
	 */
	uint32_t (*poll_pointer_events)(struct xrt_input_pointer_event *out, uint32_t max);

	/*!
	 * Where camera `camera_key` sits relative to screen `screen_key` — for a
	 * camera-based input provider (hand tracking off the same webcam a
	 * viewer-tracking provider reads). The runtime's camera calibration
	 * store, resolved display-declared > user-calibration > placement-hint >
	 * default-prior, so the answer is never "not found"
	 * (`xrt/xrt_camera_pose.h`; `tracking-provider-discovery.md` §6). Pass
	 * this host iface as `host`. It is the SAME answer the viewer-tracking
	 * host iface's slot of the same name gives for the same camera and
	 * screen: a calibration done once for head tracking serves the hands.
	 *
	 * Carved out of the former reserved[3], like the slots above: size and
	 * every other offset unchanged, the API major stays 1. An older runtime
	 * leaves it NULL — providers MUST NULL-check (and cover-check
	 * `struct_size`). Announced by @ref XRT_INPUT_PLUGIN_HOST_HAS_GET_CAMERA_POSE.
	 */
	xrt_camera_get_pose_fn_t get_camera_pose;

	/*!
	 * Open the display's SHARED stereo camera as a hands source (ADR-043;
	 * tracking-provider-discovery.md §7 / §10 item 2): an in-process consumer
	 * of the service's camera manager, which stays the camera's single owner
	 * — the same RECTIFIED GRAY8 shared-memory stream a `CAMERA_CONSUMER`
	 * client gets over IPC, without a connection.
	 *
	 * @p provider_id is the provider's own `iface->id` (the id it passes to
	 * @ref get_setting): the host shares ONE iface between all providers, so
	 * the id is how it knows who is asking — the name the camera indicator
	 * shows and the binary provenance (ADR-054 N4b) the consent policy reads.
	 * A signed / allowlisted / platform-trusted provider is allowed without a
	 * prompt; any other is prompted like an app. The kill switch, the sharing
	 * toggle, the session lock and the in-use indicator apply exactly as they
	 * do to an app.
	 *
	 * Call it ON DEMAND (from `create_devices` or later, never `probe`), not
	 * per frame, and from a thread that may block: an unverified provider's
	 * open may wait for the user's answer. Returns
	 * `XRT_ERROR_FEATURE_NOT_SUPPORTED` when this process has no camera
	 * manager (the in-process runtime, or the service still starting — retry
	 * later) or no rectifiable camera, `XRT_ERROR_STEREO_CAMERA_DISABLED`
	 * when sharing is off, `XRT_ERROR_STEREO_CAMERA_CONSENT_REFUSED`,
	 * `XRT_ERROR_NOT_AUTHORIZED` (RAW asked, an unknown @p provider_id),
	 * `XRT_ERROR_CLIENT_LIMIT_REACHED` (the camera-consumer quota),
	 * `XRT_ERROR_STEREO_CAMERA_BUSY`.
	 *
	 * Carved out of the former reserved[4], like the slots above: size and
	 * every other offset unchanged, the API major stays 1. An older runtime
	 * leaves it (and the three slots below) NULL — providers MUST NULL-check
	 * (and cover-check `struct_size`). Announced by
	 * @ref XRT_INPUT_PLUGIN_HOST_HAS_SHARED_CAMERA.
	 */
	xrt_result_t (*open_shared_camera)(const char *provider_id,
	                                   struct xrt_input_host_camera_open *inout,
	                                   struct xrt_input_host_camera **out_cam);

	/*!
	 * The newest frame of @p cam, waiting up to @p timeout_ms for one newer
	 * than the last acquire (0 = poll). Returns `XRT_SUCCESS` with @p out
	 * filled, `XRT_TIMEOUT` when no new frame came, or
	 * `XRT_ERROR_STEREO_CAMERA_STREAM_ENDED` when the service ended the
	 * stream (the user stopped sharing, the source went away, shutdown):
	 * close it, and open again later if still wanted.
	 *
	 * @p out_state (may be NULL) always receives the camera's state as an app
	 * would be told it (enum xrt_stereo_camera_state): AVAILABLE = frames
	 * flow; WAITING = the display delivers frames only while a 3D session
	 * holds its tracker and none does — a provider reports
	 * XRT_INPUT_PROVIDER_PRESENCE_ABSENT then, PRESENT once frames flow
	 * again; SUSPENDED = session locked / sharing off / frames stopped;
	 * UNAVAILABLE = gone. Carved out of the former reserved[5].
	 */
	xrt_result_t (*camera_acquire)(struct xrt_input_host_camera *cam,
	                               uint32_t timeout_ms,
	                               struct xrt_input_host_camera_frame *out,
	                               uint32_t *out_state);

	/*!
	 * Done with frame @p frame_index: its slot may be rewritten. Optional (the
	 * next acquire releases it too), but a provider that holds a frame for
	 * long should release it. Carved out of the former reserved[6].
	 */
	void (*camera_release)(struct xrt_input_host_camera *cam, uint64_t frame_index);

	/*!
	 * Stop and free the stream; @p cam and every pointer it handed out are
	 * invalid afterwards. NULL is ignored. Carved out of the former
	 * reserved[7].
	 */
	void (*close_shared_camera)(struct xrt_input_host_camera *cam);

	/*!
	 * Reserved space for forward-compatible host-supplied callbacks.
	 * Providers MUST NOT dereference any reserved slot.
	 */
	void *reserved[6];
};

/*!
 * Defined when this header carries the
 * @ref xrt_input_plugin_host_iface::get_setting slot (N3c), so a provider
 * that must also compile against an older runtime header can `#ifdef`-guard
 * its use.
 */
#define XRT_INPUT_PLUGIN_HOST_HAS_GET_SETTING 1

/*!
 * Defined when this header carries the
 * @ref xrt_input_plugin_host_iface::poll_pointer_events slot, so a provider
 * that must also compile against an older runtime header can `#ifdef`-guard
 * its use.
 */
#define XRT_INPUT_PLUGIN_HOST_HAS_POLL_POINTER_EVENTS 1

/*!
 * Defined when this header carries the
 * @ref xrt_input_plugin_host_iface::get_camera_pose slot, so a provider that
 * must also compile against an older runtime header can `#ifdef`-guard its use.
 */
#define XRT_INPUT_PLUGIN_HOST_HAS_GET_CAMERA_POSE 1

/*!
 * Defined when this header carries the shared-camera slots
 * (@ref xrt_input_plugin_host_iface::open_shared_camera, `camera_acquire`,
 * `camera_release`, `close_shared_camera`), so a provider that must also
 * compile against an older runtime header can `#ifdef`-guard their use.
 */
#define XRT_INPUT_PLUGIN_HOST_HAS_SHARED_CAMERA 1

// The get_display_geometry, get_setting, poll_pointer_events, get_camera_pose,
// open_shared_camera, camera_acquire, camera_release and close_shared_camera
// slots were carved out of the former reserved[0] .. [7], in that order: the
// host iface's size and every other offset are unchanged, so providers built
// against the previous header keep working and the API major stays 1.
// clang-format off
XRT_INPUT_PLUGIN_ABI_ASSERT(offsetof(struct xrt_input_plugin_host_iface, get_setting) ==
                                offsetof(struct xrt_input_plugin_host_iface, get_display_geometry) +
                                    sizeof(void *),
                            "xrt_input_plugin_host_iface: get_setting must occupy the former reserved[1]");
XRT_INPUT_PLUGIN_ABI_ASSERT(offsetof(struct xrt_input_plugin_host_iface, poll_pointer_events) ==
                                offsetof(struct xrt_input_plugin_host_iface, get_display_geometry) +
                                    2 * sizeof(void *),
                            "xrt_input_plugin_host_iface: poll_pointer_events must occupy the former reserved[2]");
XRT_INPUT_PLUGIN_ABI_ASSERT(offsetof(struct xrt_input_plugin_host_iface, get_camera_pose) ==
                                offsetof(struct xrt_input_plugin_host_iface, get_display_geometry) +
                                    3 * sizeof(void *),
                            "xrt_input_plugin_host_iface: get_camera_pose must occupy the former reserved[3]");
XRT_INPUT_PLUGIN_ABI_ASSERT(offsetof(struct xrt_input_plugin_host_iface, open_shared_camera) ==
                                offsetof(struct xrt_input_plugin_host_iface, get_display_geometry) +
                                    4 * sizeof(void *),
                            "xrt_input_plugin_host_iface: open_shared_camera must occupy the former reserved[4]");
XRT_INPUT_PLUGIN_ABI_ASSERT(offsetof(struct xrt_input_plugin_host_iface, camera_acquire) ==
                                offsetof(struct xrt_input_plugin_host_iface, get_display_geometry) +
                                    5 * sizeof(void *),
                            "xrt_input_plugin_host_iface: camera_acquire must occupy the former reserved[5]");
XRT_INPUT_PLUGIN_ABI_ASSERT(offsetof(struct xrt_input_plugin_host_iface, camera_release) ==
                                offsetof(struct xrt_input_plugin_host_iface, get_display_geometry) +
                                    6 * sizeof(void *),
                            "xrt_input_plugin_host_iface: camera_release must occupy the former reserved[6]");
XRT_INPUT_PLUGIN_ABI_ASSERT(offsetof(struct xrt_input_plugin_host_iface, close_shared_camera) ==
                                offsetof(struct xrt_input_plugin_host_iface, get_display_geometry) +
                                    7 * sizeof(void *),
                            "xrt_input_plugin_host_iface: close_shared_camera must occupy the former reserved[7]");
XRT_INPUT_PLUGIN_ABI_ASSERT(offsetof(struct xrt_input_plugin_host_iface, reserved) ==
                                offsetof(struct xrt_input_plugin_host_iface, get_display_geometry) +
                                    8 * sizeof(void *),
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

	/*!
	 * The settings this provider reads through
	 * @ref xrt_input_plugin_host_iface::get_setting (ADR-054 decision 7,
	 * the one options contract), for `displayxr-cli input options <id>`.
	 * Optional: NULL (or a provider whose `struct_size` predates this slot)
	 * declares nothing — the store's keys are still listed. Appended under
	 * `struct_size` cover (API major stays 1); the runtime guards the call
	 * with @ref XRT_INPUT_PLUGIN_IFACE_HAS. Announced by
	 * @ref XRT_INPUT_PLUGIN_IFACE_HAS_DESCRIBE_SETTINGS.
	 */
	xrt_component_describe_settings_fn_t describe_settings;
};

/*!
 * Defined when @ref xrt_input_plugin_iface carries `describe_settings` (N3c),
 * so a provider built against an older runtime header can #ifdef-guard
 * filling it.
 */
#define XRT_INPUT_PLUGIN_IFACE_HAS_DESCRIBE_SETTINGS 1


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
