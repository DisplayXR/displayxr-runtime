// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Presence-gated hand- and rig-role arbitration across N input
 *         providers and the qwerty fallback.
 *
 * ADR-034 shipped a one-shot rule: whatever the active input-provider
 * plug-in created at system build claimed the left/right hand roles for
 * the process lifetime. That is wrong for hardware that comes and goes —
 * a registered Ultraleap provider with no Leap Motion plugged in still
 * created devices, displaced qwerty, and left the user with no
 * controllers at all (#823 follow-up).
 *
 * This module makes the *roles* dynamic while leaving the *devices*
 * alone. Every candidate pair — one per claiming provider, plus
 * qwerty's — is created once at system build and all of them live in
 * `xrt_system_devices::xdevs` exactly as before. The arbiter installs
 * its own `xrt_system_devices::get_roles`, which on each call walks the
 * candidates in priority order asking
 * @ref xrt_input_plugin_iface::get_presence, and points
 * `xrt_system_roles::left` / `::right` at the first present candidate
 * that supplies each hand, bumping `generation_id` on every flip.
 *
 * ADR-034 Amendment 3 generalised this from a single provider-vs-qwerty
 * pair to a ranked list, mirroring how the DP loader ranks display
 * plug-ins (Leia beats sim when present). Priority is ProbeOrder
 * ascending; qwerty is the floor and always present. The walk is
 * per-hand, so a provider that supplies only one controller leaves the
 * other hand to the next-ranked candidate rather than dragging both
 * down to qwerty.
 *
 * ADR-034 Amendment 4 adds the **rig (navigation) role** to the
 * same walk: a provider may supply one @ref XRT_DEVICE_TYPE_NAVIGATION
 * device, and `xrt_system_roles::rig` points at the highest-priority
 * present one. Its floor is not a candidate but an absence — `rig == -1`
 * means the runtime's own fly camera (qwerty WASD/mouse-look) drives the
 * rig, which is what every box without a navigating provider reports.
 *
 * The **gamepad role** (`xrt_system_roles::gamepad`, `/user/gamepad`) joins
 * the same walk: a provider may supply one @ref XRT_DEVICE_TYPE_GAMEPAD
 * device, and the role points at the highest-priority present one. Like the
 * rig, its floor is an absence (-1 — qwerty has no pad); unlike the rig, the
 * runtime seeds a static assignment from the builder, which the arbiter then
 * keeps presence-gated so an unplugged pad really is gone.
 *
 * ADR-054 N7 (#2143) makes the HAND floor optional too: with the
 * `EmulatedControllers` gate at 0 the builder publishes no qwerty pair, so
 * there is no floor candidate, and the hands fall to "no role" (-1, which
 * the OpenXR layer reports as XR_NULL_PATH) when every provider is absent —
 * exactly the rig's shape. A live gate flip withdraws / re-admits a pair
 * that exists (@ref t_input_arbiter_set_floor_withdrawn), the way `Enabled`
 * withdraws a provider; it never adds or removes devices.
 *
 * That reuses the role-change path Monado already has end to end: the
 * OpenXR state tracker re-reads the roles in `xrSyncActions`, rebinds
 * actions and queues `XrEventDataInteractionProfileChanged` when the
 * generation moves, and the IPC layer forwards `get_roles` to the
 * service. So re-arbitration happens per app start AND mid-session, with
 * no new plumbing and nothing to poll.
 *
 * Scope: a runtime process hosts exactly one system, so the state here
 * is file-scope, mirroring `target_input_plugin_loader.c` and the claim
 * records in `target_builder_input_provider.c`. `get_roles` calls on any
 * other `xrt_system_devices` are forwarded to the implementation the
 * arbiter displaced.
 *
 * @ingroup target_common
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_device;
struct xrt_system_devices;
struct xrt_input_plugin_iface;
struct xrt_input_plugin_instance;

/*!
 * Asks whether a floor (qwerty) device should report its OWN interaction
 * profile instead of masquerading as the top idle provider's.
 */
typedef bool (*t_input_arbiter_floor_self_fn)(struct xrt_device *xdev);

/*!
 * Install @p fn (NULL = none, the default). The builder that adds the qwerty
 * floor registers `qwerty_is_mouse_hand`, so the mouse hand keeps its MX Ink
 * / own profiles while a controller provider is installed but idle.
 */
void
t_input_arbiter_set_floor_self_report(t_input_arbiter_floor_self_fn fn);

/*!
 * Forget everything from a previous system build. Called at the top of
 * the input-provider builder pass.
 */
void
t_input_arbiter_reset(void);

/*!
 * Record one provider's left/right motion controllers, at the priority
 * @p probe_order (ascending = wins). Either device may be NULL. Called
 * once per claiming provider by
 * @ref t_builder_add_input_provider_devices after the devices are in
 * `xsysd->xdevs`.
 *
 * @p iface / @p inst identify the provider that owns the pair, so the
 * arbiter can ask THAT provider for presence rather than assuming a
 * single active one.
 */
void
t_input_arbiter_note_provider_pair(const struct xrt_input_plugin_iface *iface,
                                   struct xrt_input_plugin_instance *inst,
                                   uint32_t probe_order,
                                   struct xrt_device *left,
                                   struct xrt_device *right);

/*!
 * Record the hand-tracking devices the SAME provider supplied — must
 * follow the matching @ref t_input_arbiter_note_provider_pair call
 * (attaches to the most recently noted provider candidate). Any device
 * may be NULL; one device commonly serves several slots (ultraleap's
 * controllers are also its unobstructed trackers).
 *
 * The dynamic hand-tracking roles in @ref xrt_system_roles then follow
 * the same presence walk as the controller roles: per source and hand,
 * the highest-priority present provider carrying that input wins. There
 * is no qwerty floor here — the keyboard has no joints — so with every
 * carrier absent the role parks at -1 and consumers fall back to the
 * static assignment's inactive joints.
 */
void
t_input_arbiter_note_provider_hand_tracking(struct xrt_device *unobstructed_left,
                                            struct xrt_device *unobstructed_right,
                                            struct xrt_device *conforming_left,
                                            struct xrt_device *conforming_right);

/*!
 * Record the navigation (rig) device the SAME provider supplied — must
 * follow the matching @ref t_input_arbiter_note_provider_pair call
 * (attaches to the most recently noted provider candidate).
 *
 * @p devs / @p count is the provider's whole device array as returned by
 * @ref xrt_input_plugin_iface::create_devices; the arbiter picks the one
 * device of type @ref XRT_DEVICE_TYPE_NAVIGATION out of it. A provider
 * may supply at most one — more than one is a provider bug, so the
 * arbiter warns and takes the first. A provider without one has no rig
 * candidate and is simply skipped by the rig walk.
 *
 * The rig role then follows the same presence walk as the hands: the
 * highest-priority present candidate that supplies a navigation device
 * wins. There is no qwerty candidate here — the runtime's own fly camera
 * IS the floor, and it is expressed as `xrt_system_roles::rig == -1`
 * (ADR-034 Amendment 4).
 */
void
t_input_arbiter_note_provider_navigation(struct xrt_device *const *devs, uint32_t count);

/*!
 * Record the gamepad device the SAME provider supplied — must follow the
 * matching @ref t_input_arbiter_note_provider_pair call (attaches to the most
 * recently noted provider candidate). Same shape as
 * @ref t_input_arbiter_note_provider_navigation: the arbiter picks the one
 * device of type @ref XRT_DEVICE_TYPE_GAMEPAD out of @p devs; a second is a
 * provider bug (warned, first kept); none means no gamepad candidate.
 *
 * `xrt_system_roles::gamepad` then follows the presence walk: the
 * highest-priority present candidate with a gamepad wins, -1 when none does
 * (there is no qwerty gamepad). `gamepad_profile` is that device's `name`.
 */
void
t_input_arbiter_note_provider_gamepad(struct xrt_device *const *devs, uint32_t count);

/*!
 * Record the qwerty fallback's left/right emulated controllers. Either
 * may be NULL. Called by @ref t_builder_add_qwerty_input. Both NULL (the
 * `EmulatedControllers` gate at 0, N7) records no candidate: the hand floor
 * is then "no role".
 */
void
t_input_arbiter_note_qwerty_pair(struct xrt_device *left, struct xrt_device *right);

/*!
 * N7: was a qwerty pair noted for this build — i.e. does a hand-role floor
 * candidate exist at all? False when the `EmulatedControllers` gate was 0
 * at system build (or no qwerty driver). Cheap; any thread.
 */
bool
t_input_arbiter_has_floor(void);

/*!
 * N7 (#2143), the live half of the `EmulatedControllers` gate: withdraw
 * (@p withdrawn = true) or re-admit the qwerty floor candidate, exactly as
 * `Enabled` = 0 withdraws a provider through @ref t_input_arbiter_rerank —
 * the pair's devices stay in `xsysd->xdevs`, the walks skip it, and the
 * roles are recomputed at once (so a withdrawn floor with every provider
 * absent lands both hands on -1 and bumps `generation_id`). Returns the
 * number of candidates whose standing changed: 1 on a flip, 0 when the
 * floor already stood that way, when no pair was noted, or when the
 * arbiter is not installed. Any thread.
 */
int
t_input_arbiter_set_floor_withdrawn(bool withdrawn);

/*!
 * Should ANY input provider hold the hand roles at this instant?
 *
 * True when at least one claiming provider supplied a hand device and
 * either reports @ref XRT_INPUT_PROVIDER_PRESENCE_PRESENT or predates
 * the presence slot entirely (no `get_presence` — assume present, the
 * pre-#823-follow-up behaviour). False means qwerty keeps both hands.
 *
 * Cheap and side-effect free; the arbiter throttles the underlying
 * provider queries internally.
 */
bool
t_input_arbiter_provider_holds_roles(void);

/*!
 * Is the candidate that owns the navigation device at system-device index
 * @p nav_index present at this instant?
 *
 * The rig counterpart of @ref t_input_arbiter_provider_holds_roles, and it
 * exists for the same reason: a navigation device sitting in `xsysd->xdevs`
 * while `xrt_system_roles::rig` reads -1 is the CORRECT state when the
 * provider's hardware is unplugged (the runtime's own fly camera holds the
 * rig), and only a diagnostic that can tell that apart from a present
 * provider that never took the role can say which of the two it is looking
 * at (#1465).
 *
 * False when no candidate owns @p nav_index, when @p nav_index is negative,
 * or when the owning candidate's cached presence verdict says absent. Cheap
 * and side-effect free — a pure read of the poll thread's cache.
 */
bool
t_input_arbiter_nav_candidate_present(int32_t nav_index);

/*!
 * Install the dynamic `get_roles` on @p xsysd. Call once, after BOTH the
 * provider pass and the qwerty pass have added their devices, from the
 * builder's `open_system_impl` (the device list is final by then and the
 * indices the roles carry can be resolved).
 *
 * A no-op when neither pair was recorded, or when neither pair is a
 * complete alternative to the other — the pre-existing static roles are
 * left in place rather than replaced with a degenerate arbitration.
 */
void
t_input_arbiter_install(struct xrt_system_devices *xsysd);

/*!
 * Diagnostics for `displayxr-cli` (`selftest` / `info`): whether the
 * arbiter is installed, and whether the provider currently holds the
 * roles. Both out-params are optional.
 */
void
t_input_arbiter_get_status(bool *out_installed, bool *out_provider_holds_roles);

/*!
 * One provider's new standing for @ref t_input_arbiter_rerank: matched to
 * the candidate whose `iface->id` is @p id.
 */
struct t_input_arbiter_rank
{
	const char *id;    //!< the provider's `xrt_input_plugin_iface::id`
	uint32_t priority; //!< ascending = wins; 0 for the PreferredPlugin
	bool enabled;      //!< false = withdrawn as a candidate (devices stay)
};

/*!
 * ADR-054 N3d (#2051): live re-select on an `Enabled` / `PreferredPlugin`
 * change. The provider set is fixed per process (devices are per
 * `xrt_system_devices`, spec §3 "Lifetime"), so a policy change cannot add
 * or remove a candidate — it re-RANKS them. For every provider candidate
 * named in @p ranks: its priority becomes @p priority and it is withdrawn
 * while `enabled` is false, exactly as presence loss withdraws it (the walks
 * skip it; its devices stay resident in `xsysd->xdevs`; it is polled for
 * presence throughout so re-enabling is instant). A candidate not named
 * keeps its standing; qwerty is never named. The roles are then recomputed
 * at once — not at the next `get_roles` — so `generation_id` moves (and the
 * arbiter logs the new assignment) the moment the policy does, and every
 * `xrSyncActions` client sees the flip on its next call.
 *
 * Returns the number of candidates whose priority or withdrawal changed;
 * 0 when nothing moved or the arbiter is not installed. Callable from any
 * thread (the service's policy watch thread calls it).
 */
int
t_input_arbiter_rerank(const struct t_input_arbiter_rank *ranks, int count);


/*
 *
 * Per-client resolution (ADR-034 Amendment 5, #2181).
 *
 * The controller, hand-tracking and gamepad roles are resolved PER CLIENT: an
 * IPC connection, or the implicit in-process client (key 0) whose view is what
 * `xsysd->get_roles` returns. A client contributes at most one preferred
 * provider id per role family — from the user's per-app pin or its own
 * non-persisted request (the precedence in target_input_pref.h) — which
 * hoists that candidate to the front of THAT family's walk for THAT client
 * only, and only where the candidate supplies the family. Presence still
 * gates: an absent preferred candidate falls through to the default walk. The
 * rig role stays runtime-wide; the static hand-tracking roles are untouched.
 *
 * Each client's resolved roles are cached and recomputed only when the
 * arbiter's state epoch (a presence edge, a re-rank, a floor withdrawal) or the
 * client's own overlay moved; each client carries ONE monotonic generation,
 * seeded from the system's and bumped only when its resolved roles change
 * (the OpenXR layer refreshes on strict `<`). Nothing here runs on the
 * presence poll thread (#958).
 *
 * The installed arbiter publishes all of this as
 * `xsysd->input_selection` (`xrt/xrt_input_selection.h`).
 *
 */

/*!
 * Policy reads the arbiter needs but must not own (the plug-in loader's store;
 * the arbiter's tests supply fakes). Every slot is optional. The arbiter calls
 * them OFF its lock, never on the presence poll thread.
 */
struct t_input_arbiter_policy
{
	//! The user's per-app pin for @p exe_basename ("" + false when none, or hidden by Policy\Lock).
	bool (*get_app_pin)(const char *exe_basename, char *out, size_t cap);
	//! Write / clear (@p provider_id NULL or "") that pin. 0 = written; anything else = refused.
	int (*set_app_pin)(const char *exe_basename, const char *provider_id);
	//! HKLM `InputProviders\Policy\Lock` = 1: per-user pins and app requests are ignored.
	bool (*get_policy_lock)(void);
	//! The user default (`PreferredPlugin`, with the lock applied).
	bool (*get_user_default)(char *out, size_t cap);
	//! The REGISTRATION id of the provider that probed into @p inst ("" / NULL = use `iface->id`).
	const char *(*row_id_for)(const struct xrt_input_plugin_iface *iface, struct xrt_input_plugin_instance *inst);
	//! This process's executable basename (the implicit client's per-app key).
	bool (*self_exe_basename)(char *out, size_t cap);
};

/*!
 * Install the policy reads (copied; NULL clears them). Survives
 * @ref t_input_arbiter_reset. The plug-in loader installs the real store at
 * discovery.
 */
void
t_input_arbiter_set_policy(const struct t_input_arbiter_policy *policy);

/*!
 * ADR-054 N3d hook for Amendment 5: the class policy changed — re-read the
 * lock, the user default and every attached client's per-app pin (store I/O
 * off the lock), then mark the clients for re-resolution. Called by
 * @ref target_input_plugin_reread_policy after the re-rank. Returns the number
 * of clients whose pin changed. Any thread except the presence poll thread.
 */
int
t_input_arbiter_policy_changed(void);

#ifdef __cplusplus
}
#endif
