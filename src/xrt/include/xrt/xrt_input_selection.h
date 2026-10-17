// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Per-client input-provider selection: the runtime-internal interface
 *         behind `XR_DXR_input_providers` (ADR-034 Amendment 5, #2181).
 *
 * Every claiming input provider stays resident and the arbiter ranks them
 * (ADR-034 Amendments 1-3). Amendment 5 resolves the controller, hand-tracking
 * and gamepad roles **per client** with an optional preferred provider id per
 * role family layered on top of the default walk. This interface is how the
 * OpenXR layer (and the IPC server, on a client's behalf) reads that state and
 * expresses a preference, without knowing whether the arbiter is in this
 * process or behind a service connection:
 *
 *  - in-process: the arbiter's implementation, one implicit client (the
 *    process) whose per-app key is this process's own executable basename;
 *  - IPC client: forwards to the service over the connection, which IS the
 *    client — the service resolves the per-app key from the peer process,
 *    never from anything the client sends;
 *  - IPC server: the handler calls the service's in-process implementation
 *    with the connection's @ref xrt_input_selection_client.
 *
 * Hung off @ref xrt_system_devices::input_selection; NULL = this system has no
 * selection support (null / headless builds, a system with no arbiter), which
 * the OpenXR layer reports as an empty provider list.
 *
 * Precedence, top wins (ADR-034 Amendment 5 decision 3): `Policy\Lock`
 * (machine) -> the user's per-app pin -> the client's session request -> the
 * user default (`PreferredPlugin`) -> `ProbeOrder`. `DXR_INPUT_PROVIDERS=0` and
 * `ForceQwerty` beat every pin. A preference applies only to the families the
 * provider claims and is still presence-gated. The rig role and the head are
 * runtime-wide and never selected here.
 *
 * Threading: every entry point may be called from any thread except the
 * arbiter's presence poll thread (#958); implementations must not block on
 * provider I/O.
 *
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_results.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/*!
 * Size of a provider id, NUL included — the registration id (Windows subkey /
 * manifest `id`), which is what `PreferredPlugin` and every pin name. Mirrors
 * `XR_MAX_INPUT_PROVIDER_ID_SIZE_DXR`.
 */
#define XRT_INPUT_PROVIDER_ID_SIZE 64

//! Size of a provider display name, NUL included. Mirrors `XR_MAX_INPUT_PROVIDER_NAME_SIZE_DXR`.
#define XRT_INPUT_PROVIDER_NAME_SIZE 128

/*!
 * Size of a per-app key — an executable basename such as `SampleGame.exe`,
 * NUL included. Registry key names are capped at 255 characters and POSIX file
 * names at 255 bytes; a longer basename is not pinnable (the resolver refuses
 * it rather than truncating into another app's key).
 */
#define XRT_INPUT_EXE_BASENAME_SIZE 128

/*!
 * The reserved provider id naming the runtime's own emulated controller pair
 * (the qwerty floor, ADR-034 / ADR-054 N7). It is not a plug-in, but it can be
 * pinned like one — pinning an app to it keeps that app on keyboard/mouse
 * controllers while a hand tracker is resident. Claims CONTROLLERS only.
 * Mirrors `XR_INPUT_PROVIDER_ID_EMULATED_CONTROLLERS_DXR`.
 */
#define XRT_INPUT_PROVIDER_ID_EMULATED "qwerty"

/*!
 * The role families a client can express a preference for. Values are bit
 * positions (@ref XRT_INPUT_ROLE_FAMILY_BIT) and stay in step with the
 * extension's `XrInputRoleFamilyDXR` minus one.
 */
enum xrt_input_role_family
{
	//! `xrt_system_roles::left` / `::right` (motion controllers).
	XRT_INPUT_ROLE_FAMILY_CONTROLLERS = 0,
	//! The dynamic hand-tracking roles (unobstructed / conforming x L/R).
	XRT_INPUT_ROLE_FAMILY_HAND_TRACKING = 1,
	//! `xrt_system_roles::gamepad`.
	XRT_INPUT_ROLE_FAMILY_GAMEPAD = 2,

	XRT_INPUT_ROLE_FAMILY_COUNT = 3,
};

//! The flag for one @ref xrt_input_role_family.
#define XRT_INPUT_ROLE_FAMILY_BIT(family) (1u << (uint32_t)(family))

//! Every family.
#define XRT_INPUT_ROLE_FAMILY_ALL_BITS ((1u << (uint32_t)XRT_INPUT_ROLE_FAMILY_COUNT) - 1u)

/*!
 * Which client a call is about. Over IPC the server fills this from the
 * connection (the per-app key from the PEER process, resolved at accept);
 * in-process there is one implicit client, @ref key 0, keyed by the process's
 * own executable.
 */
struct xrt_input_selection_client
{
	/*!
	 * Stable for the client's lifetime and never reused by a later client of
	 * the same process (the IPC server's monotonic client id); 0 = the
	 * in-process implicit client.
	 */
	uint64_t key;

	//! The per-app pin key (UTF-8 executable basename); "" when unknown, which disables the pin.
	char exe_basename[XRT_INPUT_EXE_BASENAME_SIZE];
};

/*!
 * One provider as THIS client sees it. Fixed-size, so it travels over IPC
 * unchanged.
 */
struct xrt_input_provider_info
{
	//! Registration id; @ref XRT_INPUT_PROVIDER_ID_EMULATED for the built-in floor.
	char id[XRT_INPUT_PROVIDER_ID_SIZE];

	//! The provider's own display name, verbatim (UTF-8); a neutral runtime string for the floor.
	char display_name[XRT_INPUT_PROVIDER_NAME_SIZE];

	//! Families this provider supplies devices for (@ref XRT_INPUT_ROLE_FAMILY_BIT).
	uint32_t claimable_families;

	//! Families whose role THIS client currently resolves to this provider.
	uint32_t current_families;

	//! Families THIS client's (non-persisted) session request names this provider for.
	uint32_t requested_families;

	//! Its hardware / transport is there right now (the arbiter's cached verdict).
	bool present;

	//! It is the user's default (`PreferredPlugin`, `input use <id>`).
	bool user_default;

	//! The user pinned THIS client's app to it (`input use <id> --app <exe>`, or a persisted request).
	bool app_pinned;

	//! The runtime's own emulated pair, not a plug-in.
	bool builtin;
};

/*!
 * Providers per @ref xrt_input_provider_page — sized so one page fits the IPC
 * message buffer (asserted in `ipc_protocol.h`).
 */
#define XRT_INPUT_PROVIDER_PAGE_MAX 4

/*!
 * A page of the client's provider list: entries [@ref start, @ref start +
 * @ref count) of @ref total. @ref generation is this client's selection
 * generation; a caller assembling several pages restarts when it moves.
 */
struct xrt_input_provider_page
{
	XRT_ALIGNAS(8) uint64_t generation;
	uint32_t total;
	uint32_t start;
	uint32_t count;
	uint32_t _pad;
	struct xrt_input_provider_info items[XRT_INPUT_PROVIDER_PAGE_MAX];
};

/*!
 * One client's resolved holder per family, for diagnostics (`displayxr-cli
 * input list --json`, the ADR-051 status snapshot). "" = no provider holds
 * the family for that client (the qwerty floor holds controllers as
 * @ref XRT_INPUT_PROVIDER_ID_EMULATED; nothing at all = "").
 */
struct xrt_input_client_holders
{
	uint64_t key;
	char exe_basename[XRT_INPUT_EXE_BASENAME_SIZE];
	char holder[XRT_INPUT_ROLE_FAMILY_COUNT][XRT_INPUT_PROVIDER_ID_SIZE];
	//! The id this client's effective preference names per family ("" = default walk).
	char preferred[XRT_INPUT_ROLE_FAMILY_COUNT][XRT_INPUT_PROVIDER_ID_SIZE];
	uint64_t generation;
};

/*!
 * The selection interface. Any slot may be NULL (= not supported); use the
 * inline helpers below, which map a NULL slot to XRT_ERROR_FEATURE_NOT_SUPPORTED.
 */
struct xrt_input_selection
{
	/*!
	 * Page @p start of the provider list as @p client sees it. Writes
	 * `total` even when @p start is past the end (then `count` = 0).
	 */
	xrt_result_t (*enumerate)(struct xrt_input_selection *xis,
	                          const struct xrt_input_selection_client *client,
	                          uint32_t start,
	                          struct xrt_input_provider_page *out_page);

	/*!
	 * Set (@p provider_id non-empty) or clear ("" or NULL = back to the default
	 * walk) @p client's preference for @p family. @p persist = also write the
	 * user's per-app pin record for the client's executable — the SAME record
	 * `displayxr-cli input use <id> --app <exe>` and the Dashboard write, so an
	 * in-app selector and the Dashboard never disagree. A non-persisted request
	 * lasts for the client's lifetime and ranks below a pin.
	 *
	 * An unregistered provider id is accepted (it may be registered later, as
	 * with `input use`) and simply never resolves. Errors:
	 * XRT_ERROR_FEATURE_NOT_SUPPORTED (no selection on this system, or
	 * @p persist with no per-app key / no per-user store), XRT_ERROR_IPC_FAILURE
	 * (IPC client: the service went away).
	 */
	xrt_result_t (*set_preference)(struct xrt_input_selection *xis,
	                               const struct xrt_input_selection_client *client,
	                               enum xrt_input_role_family family,
	                               const char *provider_id,
	                               bool persist);

	/*!
	 * Holders of client @p index (0-based over the currently attached
	 * clients), for diagnostics. XRT_ERROR_FEATURE_NOT_SUPPORTED past the end.
	 */
	xrt_result_t (*query_holders)(struct xrt_input_selection *xis,
	                              uint32_t index,
	                              struct xrt_input_client_holders *out_holders);

	/*!
	 * @p client is gone (IPC disconnect): drop its session requests and
	 * cached roles. Its per-app pin is the user's and stays.
	 */
	void (*client_detach)(struct xrt_input_selection *xis, const struct xrt_input_selection_client *client);
};

//! @copydoc xrt_input_selection::enumerate
static inline xrt_result_t
xrt_input_selection_enumerate(struct xrt_input_selection *xis,
                              const struct xrt_input_selection_client *client,
                              uint32_t start,
                              struct xrt_input_provider_page *out_page)
{
	if (xis == NULL || xis->enumerate == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	return xis->enumerate(xis, client, start, out_page);
}

//! @copydoc xrt_input_selection::set_preference
static inline xrt_result_t
xrt_input_selection_set_preference(struct xrt_input_selection *xis,
                                   const struct xrt_input_selection_client *client,
                                   enum xrt_input_role_family family,
                                   const char *provider_id,
                                   bool persist)
{
	if (xis == NULL || xis->set_preference == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	return xis->set_preference(xis, client, family, provider_id, persist);
}

//! @copydoc xrt_input_selection::query_holders
static inline xrt_result_t
xrt_input_selection_query_holders(struct xrt_input_selection *xis,
                                  uint32_t index,
                                  struct xrt_input_client_holders *out_holders)
{
	if (xis == NULL || xis->query_holders == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	return xis->query_holders(xis, index, out_holders);
}

//! @copydoc xrt_input_selection::client_detach
static inline void
xrt_input_selection_client_detach(struct xrt_input_selection *xis, const struct xrt_input_selection_client *client)
{
	if (xis != NULL && xis->client_detach != NULL) {
		xis->client_detach(xis, client);
	}
}

#ifdef __cplusplus
}
#endif
