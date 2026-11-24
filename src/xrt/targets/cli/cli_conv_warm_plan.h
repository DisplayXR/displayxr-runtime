// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `conv warm <id>[/<cap>] [<task>]`: which tasks to warm, and what to
 *         say about the ones that cannot be.
 *
 * `conv warm` warms a TASK's selected capability; a provider id only names
 * which tasks (those where one of that provider's rows is the selection). A
 * provider usually has several rows for one task: its direct capability, and
 * others that reach the task composed with a runtime stage (a depth
 * capability plus view synthesis serves views-mono, listed under the depth
 * capability's cap id). So the verdict is per TASK, not per row:
 *
 *  - one of the matching rows is the selection: warm the task, say nothing
 *    about its siblings;
 *  - none is: one line for the task, naming the provider AND the capability
 *    that is selected.
 *
 * The old per-row line printed "leia-conversion cap 1 is not the selected
 * capability (selected: leia-conversion)" once per sibling row before warming
 * the same provider's cap 3: it named the provider where it meant the
 * capability, and it was said about rows that were never the question.
 *
 * Pure, header-only (C++), unit-tested in `tests/tests_cli_conv_warm_plan.cpp`.
 */

#pragma once

#include "xrt/xrt_display_status.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct cli_conv_warm_plan
{
	bool matched = false;           //!< the provider has at least one row matching the request
	std::vector<uint32_t> tasks;    //!< XRT_CONV_TASK_* to warm, in row order, each once
	std::vector<std::string> notes; //!< one line per task that cannot be warmed for this provider
};

//! The id of the provider row @p t belongs to ("" = none).
static inline const char *
cli_conv_warm_provider_id(const struct xrt_status_conv_provider *providers,
                          uint32_t provider_count,
                          const struct xrt_status_conv_task &t)
{
	return t.provider < provider_count ? providers[t.provider].id : "";
}

/*!
 * Plan `conv warm @p id[/@p cap] [@p only_task]` against the service's rows.
 * @p cap 0 = any capability of the provider; @p only_task 0 = every task.
 */
static inline cli_conv_warm_plan
cli_conv_warm_plan_for_provider(const struct xrt_status_conv_task *rows,
                                uint32_t row_count,
                                const struct xrt_status_conv_provider *providers,
                                uint32_t provider_count,
                                const char *id,
                                uint32_t cap,
                                uint32_t only_task)
{
	cli_conv_warm_plan plan;
	std::vector<const struct xrt_status_conv_task *>
	    unselected; // first matching row of each task with none selected
	for (uint32_t i = 0; i < row_count; i++) {
		const struct xrt_status_conv_task &t = rows[i];
		if (strcmp(cli_conv_warm_provider_id(providers, provider_count, t), id) != 0 ||
		    (cap != 0 && t.cap_id != cap) || (only_task != 0 && t.task != only_task)) {
			continue;
		}
		plan.matched = true;
		const bool warming = std::find(plan.tasks.begin(), plan.tasks.end(), t.task) != plan.tasks.end();
		const auto un = std::find_if(unselected.begin(), unselected.end(),
		                             [&](const struct xrt_status_conv_task *r) { return r->task == t.task; });
		if (t.selected) {
			if (!warming) {
				plan.tasks.push_back(t.task);
			}
			if (un != unselected.end()) {
				unselected.erase(un);
			}
		} else if (!warming && un == unselected.end()) {
			unselected.push_back(&t);
		}
	}
	for (const struct xrt_status_conv_task *t : unselected) {
		const struct xrt_status_conv_task *sel = nullptr;
		for (uint32_t i = 0; i < row_count && sel == nullptr; i++) {
			if (rows[i].task == t->task && rows[i].selected) {
				sel = &rows[i];
			}
		}
		char what[96];
		if (cap != 0) {
			(void)snprintf(what, sizeof(what), "%s cap %u", id, t->cap_id);
		} else {
			(void)snprintf(what, sizeof(what), "%s", id);
		}
		char line[512];
		if (sel == nullptr) {
			(void)snprintf(
			    line, sizeof(line),
			    "%s: %s is not selected (nothing is selected for this task) -- `conv warm` warms "
			    "the selection.",
			    t->task_name, what);
		} else {
			const char *sel_id = cli_conv_warm_provider_id(providers, provider_count, *sel);
			if (strcmp(sel_id, id) == 0) {
				// Only with <id>/<cap>: the provider is selected, on another capability.
				(void)snprintf(
				    line, sizeof(line),
				    "%s: %s is not the selected capability (selected: %s cap %u) -- `conv warm` "
				    "warms the selection; `conv warm %s %s` warms that one.",
				    t->task_name, what, sel_id, sel->cap_id, id, t->task_name);
			} else {
				(void)snprintf(
				    line, sizeof(line),
				    "%s: %s is not the selected provider (selected: %s cap %u) -- `conv warm` "
				    "warms the selection; `conv use %s %s` first to change it.",
				    t->task_name, what, sel_id, sel->cap_id, t->task_name, id);
			}
		}
		plan.notes.push_back(line);
	}
	return plan;
}
