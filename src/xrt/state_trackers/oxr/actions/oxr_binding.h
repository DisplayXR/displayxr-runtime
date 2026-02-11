// Copyright 2018-2024, Collabora, Ltd.
// Copyright 2023-2026, NVIDIA CORPORATION.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Holds binding related functions.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup oxr_main
 */

#pragma once

#include "xrt/xrt_device.h"

#include "oxr_extension_support.h"
#include "oxr_forward_declarations.h"


#ifdef __cplusplus
extern "C" {
#endif


/*
 *
 * Binding functions
 *
 */

/*!
 * Destroy an interaction profile.
 *
 * @param profile Interaction profile to destroy
 * @public @memberof oxr_interaction_profile
 */
void
oxr_interaction_profile_destroy(struct oxr_interaction_profile *profile);

/*!
 * Clone an interaction profile.
 *
 * @param src_profile Source interaction profile to clone
 * @return Cloned interaction profile, or NULL if src_profile is NULL
 * @public @memberof oxr_interaction_profile
 */
struct oxr_interaction_profile *
oxr_interaction_profile_clone(const struct oxr_interaction_profile *src_profile);

/*!
 * Find bindings from action key in a profile.
 *
 * @param log Logger
 * @param profile Interaction profile
 * @param key Action key
 * @param max_binding_count Maximum number of bindings to return
 * @param out_bindings Output bindings array
 * @param out_binding_count Output binding count
 * @public @memberof oxr_interaction_profile
 */
void
oxr_binding_find_bindings_from_act_key(struct oxr_logger *log,
                                       struct oxr_interaction_profile *profile,
                                       uint32_t key,
                                       size_t max_binding_count,
                                       struct oxr_binding **out_bindings,
                                       size_t *out_binding_count);

/*!
 * @public @memberof oxr_instance
 */
XrResult
oxr_action_suggest_interaction_profile_bindings(struct oxr_logger *log,
                                                struct oxr_instance *inst,
                                                const XrInteractionProfileSuggestedBinding *suggestedBindings,
                                                struct oxr_dpad_state *state);

/*!
 * @public @memberof oxr_instance
 */
XrResult
oxr_action_get_current_interaction_profile(struct oxr_logger *log,
                                           struct oxr_session *sess,
                                           XrPath topLevelUserPath,
                                           XrInteractionProfileState *interactionProfile);

/*!
 * @public @memberof oxr_session
 */
XrResult
oxr_action_get_input_source_localized_name(struct oxr_logger *log,
                                           struct oxr_session *sess,
                                           const XrInputSourceLocalizedNameGetInfo *getInfo,
                                           uint32_t bufferCapacityInput,
                                           uint32_t *bufferCountOutput,
                                           char *buffer);


#ifdef __cplusplus
}
#endif
