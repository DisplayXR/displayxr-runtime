// Copyright 2026, DisplayXR
// SPDX-License-Identifier: Apache-2.0
//
// PROVISIONAL — DXR is DisplayXR's Khronos-registered OpenXR author ID, but
// the XR_DXR_* extensions in this header are NOT yet registered in the
// Khronos OpenXR registry: extension numbers and XrStructureType values sit
// in a provisional experimental block (1004999xxx) pending official
// assignment. Extension names are expected to be stable; numeric values are
// not. See GOVERNANCE.md.
//
/*!
 * @file
 * @brief  Header for XR_DXR_input_providers (which input provider drives this
 *         app's controllers, hands and gamepad — and asking for another).
 * @author DisplayXR
 * @ingroup external_openxr
 *
 * A box may have several input providers installed at once — an optical hand
 * tracker, a pair of tracked controllers, a gamepad, a network feeder — plus
 * the runtime's own emulated (keyboard/mouse) controllers. The runtime keeps
 * every one of them resident and resolves each ROLE FAMILY per client
 * (ADR-034 Amendment 5): an app may learn which provider currently holds its
 * controllers / hands / gamepad, show the user the choices, and ask for one.
 *
 * WHAT AN APP MAY DO WITH THIS:
 *   - list the resident providers: id, the provider's own display name, the
 *     families it can supply, whether its hardware is there, and which
 *     families THIS app currently resolves to it;
 *   - ask for a provider per family (or go back to the default);
 *   - persist that choice as the user's per-app pin — the same record the
 *     DisplayXR Dashboard and `displayxr-cli input use <id> --app <exe>` write.
 *
 * WHAT AN APP MUST NOT DO: hardcode, match on, or recognise a provider id or
 * name. Show the strings the runtime returns; let the user pick. An app is a
 * consumer of whatever the user installed, never a policy for it.
 *
 * Precedence (top wins): a machine policy lock -> the user's per-app pin ->
 * this app's non-persisted request -> the user's default -> the providers'
 * registered order. A request only ever reorders: it applies to the families
 * the requested provider actually supplies, and while that provider's hardware
 * is absent the role falls through to the next present one exactly as it
 * would without a request. Disabling input providers (a CTS run, a debug
 * override) beats every request.
 *
 * Scope: the CLIENT, i.e. this XrInstance's connection to the runtime — not
 * the session. The setter takes a session only as proof of a running app;
 * the request survives that session ending and dies with the instance. The
 * head pose and the navigation (rig) role are shared by every app on the
 * display and are never selected here.
 *
 * Rebinding needs nothing new: when a request moves a role, the next
 * xrSyncActions sees a new assignment and the runtime queues
 * XrEventDataInteractionProfileChanged as it always does, plus
 * XrEventDataInputProvidersChangedDXR so a provider selector can refresh.
 *
 * Spec: displayxr-runtime docs/specs/extensions/XR_DXR_input_providers.md
 * Decision: docs/adr/ADR-034-input-provider-plugins.md, Amendment 5
 */
#ifndef XR_DXR_INPUT_PROVIDERS_H
#define XR_DXR_INPUT_PROVIDERS_H 1

#include <openxr/openxr.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XR_DXR_input_providers 1
#define XR_DXR_input_providers_SPEC_VERSION 1
#define XR_DXR_INPUT_PROVIDERS_EXTENSION_NAME "XR_DXR_input_providers"

#define XR_TYPE_INPUT_PROVIDER_PROPERTIES_DXR             ((XrStructureType)1004999410)
#define XR_TYPE_INPUT_PROVIDER_PREFERENCE_INFO_DXR        ((XrStructureType)1004999411)
#define XR_TYPE_EVENT_DATA_INPUT_PROVIDERS_CHANGED_DXR    ((XrStructureType)1004999412)

//! Size of XrInputProviderPropertiesDXR::providerId (and of a request's id), NUL included.
#define XR_MAX_INPUT_PROVIDER_ID_SIZE_DXR 64
//! Size of XrInputProviderPropertiesDXR::displayName, NUL included.
#define XR_MAX_INPUT_PROVIDER_NAME_SIZE_DXR 128

/*!
 * The id of the runtime's own emulated controller pair (keyboard / mouse).
 * Listed (when the pair exists) and requestable like any provider; it
 * supplies CONTROLLERS only. The ONE id an app may name in code — it is the
 * runtime's, not a vendor's, and it is stable: it names the runtime's emulated
 * floor whatever sources feed it (keyboard / mouse today), never a driver.
 */
#define XR_INPUT_PROVIDER_ID_EMULATED_CONTROLLERS_DXR "emulated"

/*!
 * A role family: the unit a preference applies to.
 */
typedef enum XrInputRoleFamilyDXR {
    //! The left / right controller roles (/user/hand/left, /user/hand/right action bindings).
    XR_INPUT_ROLE_FAMILY_CONTROLLERS_DXR   = 1,
    //! The hand-tracking roles (XR_EXT_hand_tracking joint data).
    XR_INPUT_ROLE_FAMILY_HAND_TRACKING_DXR = 2,
    //! The gamepad role (/user/gamepad).
    XR_INPUT_ROLE_FAMILY_GAMEPAD_DXR       = 3,
    XR_INPUT_ROLE_FAMILY_MAX_ENUM_DXR      = 0x7FFFFFFF
} XrInputRoleFamilyDXR;

typedef XrFlags64 XrInputRoleFamilyFlagsDXR;
static const XrInputRoleFamilyFlagsDXR XR_INPUT_ROLE_FAMILY_CONTROLLERS_BIT_DXR   = 0x00000001;
static const XrInputRoleFamilyFlagsDXR XR_INPUT_ROLE_FAMILY_HAND_TRACKING_BIT_DXR = 0x00000002;
static const XrInputRoleFamilyFlagsDXR XR_INPUT_ROLE_FAMILY_GAMEPAD_BIT_DXR       = 0x00000004;

/*!
 * One resident input provider, as THIS app sees it.
 *
 * A family absent from every entry's currentFamilies has no holder for this
 * app right now (no present provider supplies it).
 */
typedef struct XrInputProviderPropertiesDXR {
    XrStructureType             type;   //!< XR_TYPE_INPUT_PROVIDER_PROPERTIES_DXR
    void* XR_MAY_ALIAS          next;
    //! Stable id (UTF-8) — what a request names. Opaque: never parse or match it.
    char                        providerId[XR_MAX_INPUT_PROVIDER_ID_SIZE_DXR];
    //! The provider's own name for itself (UTF-8), for display only.
    char                        displayName[XR_MAX_INPUT_PROVIDER_NAME_SIZE_DXR];
    //! Families this provider can supply.
    XrInputRoleFamilyFlagsDXR   supportedFamilies;
    //! Families this app's roles currently resolve to this provider.
    XrInputRoleFamilyFlagsDXR   currentFamilies;
    //! Families this app's non-persisted request names this provider for.
    XrInputRoleFamilyFlagsDXR   requestedFamilies;
    //! Its hardware / transport is there right now.
    XrBool32                    isPresent;
    //! The user's default provider (Dashboard / `displayxr-cli input use <id>`).
    XrBool32                    isUserDefault;
    //! The user pinned this app to it; the pin outranks this app's own requests.
    XrBool32                    isAppPinned;
} XrInputProviderPropertiesDXR;

/*!
 * A preference for one family. An empty providerId clears this app's
 * preference for the family (back to the default); with persist, it also
 * clears the user's per-app pin.
 */
typedef struct XrInputProviderPreferenceInfoDXR {
    XrStructureType             type;   //!< XR_TYPE_INPUT_PROVIDER_PREFERENCE_INFO_DXR
    const void* XR_MAY_ALIAS    next;
    XrInputRoleFamilyDXR        family;
    char                        providerId[XR_MAX_INPUT_PROVIDER_ID_SIZE_DXR];
    /*!
     * XR_TRUE: also write the user's per-app pin for this app's executable —
     * the record the Dashboard edits, which survives the app and applies to
     * every family the provider supplies. Only on an explicit user action in
     * the app's own UI; never on the app's own initiative.
     */
    XrBool32                    persist;
} XrInputProviderPreferenceInfoDXR;

/*!
 * This app's view of the providers changed: a role moved to another provider
 * (a request took effect, hardware came or went, the user changed a pin or
 * the default), or a preference was set. Re-enumerate. Instance-scoped — it
 * names no session. Presence of a provider that holds nothing for this app
 * is current at enumeration time and does not raise this event on its own.
 */
typedef struct XrEventDataInputProvidersChangedDXR {
    XrStructureType             type;   //!< XR_TYPE_EVENT_DATA_INPUT_PROVIDERS_CHANGED_DXR
    const void* XR_MAY_ALIAS    next;
    //! Families whose holder may have changed (all bits when unknown).
    XrInputRoleFamilyFlagsDXR   changedFamilies;
} XrEventDataInputProvidersChangedDXR;

/*!
 * The resident input providers (two-call idiom). Callable before any session
 * exists. Entries are in the runtime's current rank order for this app. A
 * runtime with no provider support for @p systemId returns a count of 0.
 */
typedef XrResult (XRAPI_PTR *PFN_xrEnumerateInputProvidersDXR)(
    XrInstance                     instance,
    XrSystemId                     systemId,
    uint32_t                       providerCapacityInput,
    uint32_t*                      providerCountOutput,
    XrInputProviderPropertiesDXR*  providers);

/*!
 * Ask for a provider for one family, for this app (the instance's client,
 * not only @p session). Returns once the request is recorded; the roles move
 * on the next xrSyncActions. An id that is not resident is accepted and
 * never resolves (the user may install it later).
 *
 * XR_ERROR_FEATURE_UNSUPPORTED: the runtime cannot select providers for this
 * system, or persist was requested and there is no per-user store (or no
 * executable identity) to write.
 */
typedef XrResult (XRAPI_PTR *PFN_xrSetInputProviderPreferenceDXR)(
    XrSession                                 session,
    const XrInputProviderPreferenceInfoDXR*   info);

#ifndef XR_NO_PROTOTYPES
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateInputProvidersDXR(XrInstance instance, XrSystemId systemId, uint32_t providerCapacityInput, uint32_t* providerCountOutput, XrInputProviderPropertiesDXR* providers);
XRAPI_ATTR XrResult XRAPI_CALL xrSetInputProviderPreferenceDXR(XrSession session, const XrInputProviderPreferenceInfoDXR* info);
#endif

#ifdef __cplusplus
}
#endif

#endif // XR_DXR_INPUT_PROVIDERS_H
