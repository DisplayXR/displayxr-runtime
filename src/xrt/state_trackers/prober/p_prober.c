// Copyright 2019-2024, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Main prober code.
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup st_prober
 *
 * What survives of the Monado prober: it creates the builders from the
 * target's lists, picks the one that is certain it can create a head, opens
 * the system through it and keeps the builders alive until teardown. There is
 * no USB/HID/video enumeration — displays come from display-processor
 * plug-ins and input from input-provider plug-ins, both reached through the
 * builders (#703).
 */

#include "xrt/xrt_system.h"

#include "util/u_var.h"
#include "util/u_misc.h"
#include "util/u_config_json.h"
#include "util/u_debug.h"
#include "util/u_pretty_print.h"
#include "util/u_trace_marker.h"

#include "p_prober.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>


/*
 *
 * Env variable options.
 *
 */

DEBUG_GET_ONCE_LOG_OPTION(prober_log, "PROBER_LOG", U_LOGGING_INFO)


/*
 *
 * Internal functions.
 *
 */

static void
add_builder(struct prober *p, struct xrt_builder *xb)
{
	U_ARRAY_REALLOC_OR_FREE(p->builders, struct xrt_builder *, (p->builder_count + 1));
	p->builders[p->builder_count++] = xb;

	P_TRACE(p, "%s: %s", xb->identifier, xb->name);
}

static void
collect_builders(struct prober *p)
{
	for (struct xrt_prober_entry_lists *lists = p->lists; lists != NULL; lists = lists->next) {
		for (size_t i = 0; lists->builders[i] != NULL; i++) {
			struct xrt_builder *xb = lists->builders[i]();
			if (xb == NULL) {
				continue;
			}

			add_builder(p, xb);
		}
	}
}

static void
teardown(struct prober *p)
{
	XRT_TRACE_MARKER();

	// First remove the variable tracking.
	u_var_remove_root((void *)p);

	// Clean up all setter uppers.
	for (size_t i = 0; i < p->builder_count; i++) {
		xrt_builder_destroy(&p->builders[i]);
	}
	p->builder_count = 0;
	free(p->builders);
	p->builders = NULL;

	u_config_json_close(&p->json);
}

static void
print_system_devices(u_pp_delegate_t dg, struct xrt_system_devices *xsysd)
{
	struct xrt_system_roles roles = XRT_SYSTEM_ROLES_INIT;
	xrt_system_devices_get_roles(xsysd, &roles);

	u_pp(dg, "\n\tGot devices:");

	for (uint32_t i = 0; i < xsysd->xdev_count; i++) {
		u_pp(dg, "\n\t\t%u: %s", i, xsysd->xdevs[i]->str);
	}

	u_pp(dg, "\n\tIn roles:");

#define GET(IDENT, FIELD, DEFAULT) xsysd->static_roles.IDENT ? xsysd->static_roles.IDENT->FIELD : DEFAULT
#define GET_XDEV(IDENT, FIELD, DEFAULT) roles.IDENT >= 0 ? xsysd->xdevs[roles.IDENT]->FIELD : DEFAULT

#define PH(IDENT)                                                                                                      \
	u_pp(dg, "\n\t\t%s: %s (%s), view count: %zu", #IDENT, GET(IDENT, str, "<none>"),                              \
	     GET(IDENT, serial, "<none>"), GET(IDENT, hmd->view_count, 0u));
#define P(IDENT) u_pp(dg, "\n\t\t%s: %s (%s)", #IDENT, GET(IDENT, str, "<none>"), GET(IDENT, serial, "<none>"))
#define PD(IDENT)                                                                                                      \
	u_pp(dg, "\n\t\t%s: %s (%s)", #IDENT, GET_XDEV(IDENT, str, "<none>"), GET_XDEV(IDENT, serial, "<none>"))

	PH(head);
	P(eyes);
	P(face);
	PD(left);
	PD(right);
	PD(gamepad);
	P(hand_tracking.unobstructed.left);
	P(hand_tracking.unobstructed.right);
	P(hand_tracking.conforming.left);
	P(hand_tracking.conforming.right);

#undef P
#undef PD
#undef PH
#undef GET
#undef GET_XDEV
}


/*
 *
 * Member functions.
 *
 */

static xrt_result_t
p_probe(struct xrt_prober *xp)
{
	XRT_TRACE_MARKER();

	// Nothing to enumerate: no hardware lists, see the file comment.
	(void)xp;

	return XRT_SUCCESS;
}

static xrt_result_t
p_create_system(struct xrt_prober *xp,
                struct xrt_session_event_sink *broadcast,
                struct xrt_system_devices **out_xsysd,
                struct xrt_space_overseer **out_xso)
{
	XRT_TRACE_MARKER();

	struct prober *p = (struct prober *)xp;
	struct xrt_builder *select = NULL;
	xrt_result_t xret = XRT_SUCCESS;
	struct u_pp_sink_stack_only sink; // Not inited, very large.
	u_pp_delegate_t dg = u_pp_sink_stack_only_init(&sink);


	/*
	 * Logging.
	 */

	u_pp(dg, "Creating system:");
	u_pp(dg, "\n\tBuilders:");
	for (size_t i = 0; i < p->builder_count; i++) {
		u_pp(dg, "\n\t\t%s: %s", p->builders[i]->identifier, p->builders[i]->name);
	}


	/*
	 * Estimate.
	 */

	// Select the highest-priority builder that is certain it can create a head.
	int32_t best_priority = INT32_MIN;
	for (size_t i = 0; i < p->builder_count; i++) {
		struct xrt_builder *xb = p->builders[i];

		if (xb->exclude_from_automatic_discovery) {
			continue;
		}

		struct xrt_builder_estimate estimate = {0};
		xrt_builder_estimate_system(xb, p->json.root, xp, &estimate);

		if (estimate.certain.head && estimate.priority > best_priority) {
			select = xb;
			best_priority = estimate.priority;
		}
	}

	if (select != NULL) {
		u_pp(dg, "\n\tSelected %s (priority %d) because it was certain it could create a head",
		     select->identifier, best_priority);
		u_pp(dg, "\n\tUsing builder %s: %s", select->identifier, select->name);
		xret = xrt_builder_open_system( //
		    select,                     //
		    p->json.root,               //
		    xp,                         //
		    broadcast,                  //
		    out_xsysd,                  //
		    out_xso);                   //

		if (xret == XRT_SUCCESS) {
			print_system_devices(dg, *out_xsysd);
		}
	} else {
		u_pp(dg, "\n\tNo builder can be used to create a head device");
		xret = XRT_ERROR_DEVICE_CREATION_FAILED;
	}

	u_pp(dg, "\n\tResult: ");
	u_pp_xrt_result(dg, xret);

	P_INFO(p, "%s", sink.buffer);

	return xret;
}

static int
p_get_builders(struct xrt_prober *xp, size_t *out_builder_count, struct xrt_builder ***out_builders)
{
	struct prober *p = (struct prober *)xp;

	*out_builder_count = p->builder_count;
	*out_builders = p->builders;

	return 0;
}

static void
p_destroy(struct xrt_prober **xp)
{
	XRT_TRACE_MARKER();

	struct prober *p = (struct prober *)*xp;
	if (p == NULL) {
		return;
	}

	teardown(p);
	free(p);

	*xp = NULL;
}


/*
 *
 * "Exported" functions.
 *
 */

int
xrt_prober_create_with_lists(struct xrt_prober **out_xp, struct xrt_prober_entry_lists *lists)
{
	XRT_TRACE_MARKER();

	struct prober *p = U_TYPED_CALLOC(struct prober);

	p->base.probe = p_probe;
	p->base.create_system = p_create_system;
	p->base.get_builders = p_get_builders;
	p->base.destroy = p_destroy;
	p->lists = lists;
	p->log_level = debug_get_log_option_prober_log();

	p->json.file_loaded = false;
	p->json.root = NULL;

	u_var_add_root((void *)p, "Prober", true);
	u_var_add_log_level(p, &p->log_level, "Log level");

	u_config_json_open_or_create_main_file(&p->json);

	collect_builders(p);

	*out_xp = &p->base;

	return 0;
}
