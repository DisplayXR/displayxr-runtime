// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-061 arm 2 / ADR-049 Amendment 1: the in-process SERVICE LINK's
 *         wire types — registration {lift, share}, the share poll, the
 *         published ring and the per-frame ready notice.
 *
 * One sessionless `APP` connection per in-process session
 * (`ipc_client_service_link`, named "<exe> (in-process)") carries the
 * session's lift calls (ADR-049, unchanged) and, when it declares
 * @ref XRT_SHARE_LINK_CAP_SHARE, the share capability:
 *
 * @verbatim
 *   app (in-process)                         service (stereo-camera manager)
 *   ----------------                         -------------------------------
 *   service_link_register {caps, ...}  --->  listed as SOURCE_APP_LINK
 *                                            (AVAILABLE or UNAVAILABLE+reason)
 *   service_link_share_poll            <---  NONE | SHARE_REQUESTED | SHARE_STOPPED
 *   service_link_share_publish_ring    --->  3 textures (NT handles) + desc
 *   service_link_share_publish_fence   --->  the ring's shared fence
 *   service_link_share_frame_ready     --->  {generation, slot, fence value, ...}
 *        (reply: the poll state, so a sharing client needs no extra poll)
 * @endverbatim
 *
 * Plain POD, every field explicitly sized: these ride the IPC wire as is.
 * Graphics-API free on purpose — the compositor side (compositor/xbridge/
 * comp_xbridge_share_tap.h) and the IPC side both include it.
 *
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_defines.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * What a service link declares at registration (ADR-061 decision 16). A link
 * without SHARE is exactly the ADR-049 lift link: it never registers.
 */
enum xrt_share_link_caps
{
	XRT_SHARE_LINK_CAP_LIFT = 1u << 0,  //!< the session enabled XR_DXR_lift
	XRT_SHARE_LINK_CAP_SHARE = 1u << 1, //!< the session offers its pre-weave views (DXR_APP_SHARE != 0)
};

//! The session's graphics tier, as the service reports it (`displayxr-cli share list`).
enum xrt_share_link_tier
{
	XRT_SHARE_LINK_TIER_UNKNOWN = 0,
	XRT_SHARE_LINK_TIER_D3D11 = 1,
	XRT_SHARE_LINK_TIER_D3D12 = 2,
	XRT_SHARE_LINK_TIER_VULKAN = 3,
	XRT_SHARE_LINK_TIER_GL = 4,
	XRT_SHARE_LINK_TIER_METAL = 5,
};

//! What the service asks of a sharing link (`service_link_share_poll`).
enum xrt_share_link_state
{
	//! Nobody is sharing this session: publish nothing, hold no ring.
	XRT_SHARE_LINK_STATE_NONE = 0,
	//! A consented stream runs: publish a ring for @ref xrt_share_link_poll::stream_generation, then frames.
	XRT_SHARE_LINK_STATE_SHARE_REQUESTED = 1,
	//! The stream stopped: drop the ring. Reported once, then NONE.
	XRT_SHARE_LINK_STATE_SHARE_STOPPED = 2,
};

//! Size of @ref xrt_share_link_registration::exe (= XRT_APP_SHARE_EXECUTABLE_NAME_SIZE).
#define XRT_SHARE_LINK_EXE_SIZE 260
//! Size of @ref xrt_share_link_registration::app_name (= XR_MAX_APPLICATION_NAME_SIZE).
#define XRT_SHARE_LINK_APP_NAME_SIZE 128
//! Textures in a published ring.
#define XRT_SHARE_LINK_RING_SLOTS 3

/*!
 * `service_link_register`. Sent once the link connects (only when the link
 * declares SHARE) and again whenever a field changes (session begin / end, the
 * compositor's tap flag): re-registration is an idempotent update.
 *
 * The service never trusts @ref pid or @ref exe for identity — the listed pid
 * and executable are OS-derived from the connection (ADR-061 spec §2); these
 * two are diagnostics (a mismatch is logged once).
 */
struct xrt_share_link_registration
{
	uint32_t caps; //!< enum xrt_share_link_caps
	uint32_t pid;  //!< the app's own idea of its pid (diagnostic)
	char exe[XRT_SHARE_LINK_EXE_SIZE];
	//! XrApplicationInfo::applicationName (the entry's displayName).
	char app_name[XRT_SHARE_LINK_APP_NAME_SIZE];
	//! The session chained XrSessionShareabilityDXR shareable = XR_FALSE (spec §5): never listed.
	uint32_t opt_out;
	uint32_t tier; //!< enum xrt_share_link_tier
	/*!
	 * The adapter the session's tap would publish its ring on (ADR-037 §7,
	 * the LUID the in-process compositor's tap device lives on). All zero with
	 * @ref luid_valid 0 off Windows: the service then does no adapter check.
	 */
	uint8_t luid[XRT_LUID_SIZE];
	uint32_t luid_valid;
	/*!
	 * The in-process compositor can tap its pre-weave content (the xbridge
	 * share tap, comp_xbridge_share_tap.h, set it). 0 everywhere but a
	 * Windows session whose fill arm engaged.
	 */
	uint32_t tap_available;
	/*!
	 * When @ref tap_available is 0: why (enum xrt_app_share_availability:
	 * UNSUPPORTED_API, SHARED_TEXTURE, SPLIT_DISABLED, LEGACY_STANDALONE,
	 * OTHER_ADAPTER, NO_TAP). 0 = unknown, reported as NO_TAP.
	 */
	uint32_t tap_unavailable_reason;
	//! Between xrBeginSession and xrEndSession: only a running session is listed.
	uint32_t session_running;
	uint32_t reserved;
};

//! `service_link_share_poll` / the `service_link_share_frame_ready` reply.
struct xrt_share_link_poll
{
	uint32_t state; //!< enum xrt_share_link_state
	//! The stream extent the service delivers (per eye); the ring may be any size, the service scales.
	uint32_t eye_w, eye_h;
	float fps; //!< the stream rate; publishing faster is wasted work
	/*!
	 * Bumps on every SHARE_REQUESTED. A ring is published FOR one stream
	 * generation; a ring of an older one is never imported.
	 */
	uint32_t stream_generation;
	uint32_t reserved;
};

//! @ref xrt_share_link_ring_desc::format values that are not a DXGI_FORMAT.
#define XRT_SHARE_LINK_FORMAT_CPU_NV12 0x80000001u //!< service-internal fake (DXR_APP_SHARE_FAKE=link): CPU NV12

/*!
 * `service_link_share_publish_ring`: the app's ring, sent with its
 * @ref XRT_SHARE_LINK_RING_SLOTS NT-shared textures (Windows; no handles for
 * the fake / test rings). The ring's shared fence follows in
 * `service_link_share_publish_fence` with the same @ref generation when
 * @ref has_fence says so; the ring is imported once both arrived.
 *
 * Each slot texture is @ref slot_width x @ref slot_height: the left view in
 * the left half, the right view in the right half, each at
 * @ref content_x / @ref content_y / @ref content_width / @ref content_height
 * inside its half (the app copies its views unscaled; the SERVICE scales to
 * the stream extent and letterboxes, ADR-061 decision 6).
 */
struct xrt_share_link_ring_desc
{
	//! Client-owned, bumps on every new ring (resize, re-create). Frames name it.
	uint32_t generation;
	//! The xrt_share_link_poll::stream_generation this ring answers.
	uint32_t stream_generation;
	//! DXGI_FORMAT of the slot textures (e.g. 28 = R8G8B8A8_UNORM), or XRT_SHARE_LINK_FORMAT_*.
	uint32_t format;
	uint32_t slot_width, slot_height;
	int32_t content_x, content_y;
	uint32_t content_width, content_height;
	uint32_t flip_y;             //!< the views are bottom-up (atlas_flip_y)
	uint32_t encoding_is_linear; //!< the texels are LINEAR, encode them (ADR-044)
	uint32_t slot_count;         //!< XRT_SHARE_LINK_RING_SLOTS (0 = withdraw the ring)
	/*!
	 * A `service_link_share_publish_fence` of this generation follows; the
	 * service imports the ring once it arrived (time-boxed — a fence that
	 * never comes ends the share). 0 = no fence: imported at once (frames are
	 * then read as complete when frame_ready arrives). The client link sets
	 * it from the fence it is given.
	 */
	uint32_t has_fence;
	uint32_t reserved;
};

/*!
 * `service_link_share_frame_ready`: slot @ref slot of ring @ref generation
 * holds a finished frame once the ring's fence reaches @ref fence_value.
 * Latest-wins: an unconsumed earlier notice is replaced.
 */
struct xrt_share_link_frame
{
	uint32_t generation; //!< the ring (xrt_share_link_ring_desc::generation)
	uint32_t slot;       //!< < slot_count
	uint64_t fence_value;
	int64_t commit_ns; //!< the app's commit, os_monotonic (both ends share the clock)
	float zero_disparity_shift_frac;
	uint32_t mono; //!< one active view: the left half is duplicated
	//! The frame's content rect inside one half when it moved without a new ring; width 0 = the ring's.
	int32_t content_x, content_y;
	uint32_t content_width, content_height;
	uint32_t content_generation; //!< bumps when content rect / mono / layout changed
	uint32_t reserved;
};

#ifdef __cplusplus
}
#endif
