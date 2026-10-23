// Copyright 2019-2023, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Common interface to create the system through builders.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_device.h"

#ifdef __cplusplus
extern "C" {
#endif


/*
 *
 * Prober.
 *
 */

struct cJSON;
typedef struct cJSON cJSON;

struct xrt_prober;
struct xrt_prober_entry_lists;
struct xrt_builder;
struct xrt_system_devices;
struct xrt_space_overseer;
struct xrt_session_event_sink;

/*!
 * Owns the list of @ref xrt_builder and picks the one that creates the
 * system.
 *
 * DisplayXR has no USB/HID/video device probing: the display comes from a
 * display-processor plug-in and input from input-provider plug-ins, both
 * reached through the builders. What is left of the Monado prober is the
 * builder selection plus its lifetime (builders, and what they own, live
 * until the prober is destroyed).
 *
 * @ingroup xrt_iface
 */
struct xrt_prober
{
	/*!
	 * Refresh the prober's view of the system. Kept for callers that prime
	 * the prober before creating the system; there are no hardware lists to
	 * enumerate, so it currently does nothing.
	 */
	xrt_result_t (*probe)(struct xrt_prober *xp);

	/*!
	 * Create system devices.
	 *
	 * @param[in]  xp        Prober self parameter.
	 * @param[in]  broadcast Event sink that broadcasts events to all sessions.
	 * @param[out] out_xsysd Return of system devices, the pointed pointer must be NULL.
	 * @param[out] out_xso   Return of the @ref xrt_space_overseer, the pointed pointer must be NULL.
	 *
	 * @note Code consuming this interface should use xrt_prober_create_system()
	 */
	xrt_result_t (*create_system)(struct xrt_prober *xp,
	                              struct xrt_session_event_sink *broadcast,
	                              struct xrt_system_devices **out_xsysd,
	                              struct xrt_space_overseer **out_xso);

	/*!
	 * Retrieve the created @ref xrt_builder array. The prober keeps ownership.
	 *
	 * @param xp Pointer to self
	 * @param[out] out_builder_count The size of @p out_builders
	 * @param[out] out_builders An array of builders.
	 *
	 * @return 0 on success, <0 on error.
	 */
	int (*get_builders)(struct xrt_prober *xp, size_t *out_builder_count, struct xrt_builder ***out_builders);

	/*!
	 * Destroy the prober and set the pointer to null.
	 *
	 * Code consuming this interface should use xrt_prober_destroy().
	 *
	 * @param xp_ptr pointer to self-pointer
	 */
	void (*destroy)(struct xrt_prober **xp_ptr);
};

/*!
 * @copydoc xrt_prober::probe
 *
 * Helper function for @ref xrt_prober::probe.
 *
 * @public @memberof xrt_prober
 */
static inline xrt_result_t
xrt_prober_probe(struct xrt_prober *xp)
{
	return xp->probe(xp);
}

/*!
 * @copydoc xrt_prober::create_system
 *
 * Helper function for @ref xrt_prober::create_system.
 *
 * @public @memberof xrt_prober
 */
static inline xrt_result_t
xrt_prober_create_system(struct xrt_prober *xp,
                         struct xrt_session_event_sink *broadcast,
                         struct xrt_system_devices **out_xsysd,
                         struct xrt_space_overseer **out_xso)
{
	return xp->create_system(xp, broadcast, out_xsysd, out_xso);
}

/*!
 * @copydoc xrt_prober::get_builders
 *
 * Helper function for @ref xrt_prober::get_builders.
 *
 * @public @memberof xrt_prober
 */
static inline int
xrt_prober_get_builders(struct xrt_prober *xp, size_t *out_builder_count, struct xrt_builder ***out_builders)
{
	return xp->get_builders(xp, out_builder_count, out_builders);
}

/*!
 * @copydoc xrt_prober::destroy
 *
 * Helper for calling through the function pointer: does a null check and sets
 * xp_ptr to null if freed.
 *
 * @public @memberof xrt_prober
 */
static inline void
xrt_prober_destroy(struct xrt_prober **xp_ptr)
{
	struct xrt_prober *xp = *xp_ptr;
	if (xp == NULL) {
		return;
	}

	xp->destroy(xp_ptr);
	*xp_ptr = NULL;
}


/*
 *
 * Builder interface.
 *
 */

/*!
 * A estimate from a setter upper about how many devices they can open.
 *
 * @ingroup xrt_iface
 */
struct xrt_builder_estimate
{
	struct
	{
		bool head;
		bool left;
		bool right;
		bool dof6;
		uint32_t extra_device_count;
	} certain, maybe;

	/*!
	 * A setter upper defined priority, mostly for vive vs survive.
	 *
	 * 0 normal priority, positive value higher, negative lower.
	 */
	int32_t priority;
};

/*!
 * Function pointer type for creating a @ref xrt_builder.
 *
 * @ingroup xrt_iface
 */
typedef struct xrt_builder *(*xrt_builder_create_func_t)(void);

/*!
 * Sets up a collection of devices and builds a system, a setter upper.
 *
 * @ingroup xrt_iface
 */
struct xrt_builder
{
	//! Short identifier, like "vive", "north_star", "rgb_tracking".
	const char *identifier;

	//! "Localized" pretty name.
	const char *name;

	//! List of identifiers for drivers this setter-upper uses/supports.
	const char **driver_identifiers;

	//! Number of driver identifiers.
	size_t driver_identifier_count;

	//! Should this builder be excluded from automatic discovery.
	bool exclude_from_automatic_discovery;

	/*!
	 * From the devices found, estimate without opening the devices how
	 * good the system will be.
	 *
	 * @param[in]  xb           Builder self parameter.
	 * @param[in]  xp           Prober
	 * @param[in]  config       JSON config object if found for this setter upper.
	 * @param[out] out_estimate Estimate to be filled out.
	 *
	 * @note Code consuming this interface should use xrt_builder_estimate_system()
	 */
	xrt_result_t (*estimate_system)(struct xrt_builder *xb,
	                                cJSON *config,
	                                struct xrt_prober *xp,
	                                struct xrt_builder_estimate *out_estimate);

	/*!
	 * We are now committed to opening these devices.
	 *
	 * @param[in]  xb        Builder self parameter.
	 * @param[in]  xp        Prober
	 * @param[in]  config    JSON config object if found for this setter upper.
	 * @param[in]  broadcast Event sink that broadcasts events to all sessions.
	 * @param[out] out_xsysd Return of system devices, the pointed pointer must be NULL.
	 * @param[out] out_xso   Return of the @ref xrt_space_overseer, the pointed pointer must be NULL.
	 *
	 * @note Code consuming this interface should use xrt_builder_open_system()
	 */
	xrt_result_t (*open_system)(struct xrt_builder *xb,
	                            cJSON *config,
	                            struct xrt_prober *xp,
	                            struct xrt_session_event_sink *broadcast,
	                            struct xrt_system_devices **out_xsysd,
	                            struct xrt_space_overseer **out_xso);

	/*!
	 * Destroy this setter upper.
	 *
	 * @note Code consuming this interface should use xrt_builder_destroy()
	 */
	void (*destroy)(struct xrt_builder *xb);
};

/*!
 * @copydoc xrt_builder::estimate_system
 *
 * Helper function for @ref xrt_builder::estimate_system.
 *
 * @public @memberof xrt_builder
 */
static inline xrt_result_t
xrt_builder_estimate_system(struct xrt_builder *xb,
                            cJSON *config,
                            struct xrt_prober *xp,
                            struct xrt_builder_estimate *out_estimate)
{
	return xb->estimate_system(xb, config, xp, out_estimate);
}

/*!
 * @copydoc xrt_builder::open_system
 *
 * Helper function for @ref xrt_builder::open_system.
 *
 * @public @memberof xrt_builder
 */
static inline xrt_result_t
xrt_builder_open_system(struct xrt_builder *xb,
                        cJSON *config,
                        struct xrt_prober *xp,
                        struct xrt_session_event_sink *broadcast,
                        struct xrt_system_devices **out_xsysd,
                        struct xrt_space_overseer **out_xso)
{
	return xb->open_system(xb, config, xp, broadcast, out_xsysd, out_xso);
}

/*!
 * @copydoc xrt_builder::destroy
 *
 * Helper for calling through the function pointer: does a null check and sets
 * xb_ptr to null if freed.
 *
 * @public @memberof xrt_builder
 */
static inline void
xrt_builder_destroy(struct xrt_builder **xb_ptr)
{
	struct xrt_builder *xb = *xb_ptr;
	if (xb == NULL) {
		return;
	}

	xb->destroy(xb);
	*xb_ptr = NULL;
}



/*
 *
 * Prober creation.
 *
 */

/*!
 * Main root of all of the probing device.
 *
 * @ingroup xrt_iface
 */
struct xrt_prober_entry_lists
{
	/*!
	 * A null terminated list of @ref xrt_builder creation functions.
	 */
	xrt_builder_create_func_t *builders;

	/*!
	 * Lets you chain multiple prober entry lists.
	 */
	struct xrt_prober_entry_lists *next;
};

/*!
 * Create a prober with a list of builders.
 *
 * Typically used by xrt_instance_create implementations to create the prober,
 * often with a shared list called `target_list`.
 *
 * @param[out] out_xp Pointer to xrt_prober pointer, will be populated with
 * created xrt_prober instance.
 * @param[in] list Prober entry list
 *
 * @public @memberof xrt_prober
 */
int
xrt_prober_create_with_lists(struct xrt_prober **out_xp, struct xrt_prober_entry_lists *list);


#ifdef __cplusplus
}
#endif
