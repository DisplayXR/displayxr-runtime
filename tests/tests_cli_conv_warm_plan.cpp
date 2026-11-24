// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `displayxr-cli conv warm <id>[/<cap>] [<task>]`: the per-task plan
 *         (cli_conv_warm_plan.h). One verdict per task, never one per row; the
 *         "not selected" line names the provider and the capability that is.
 */

#include "catch_amalgamated.hpp"

#include "cli_conv_warm_plan.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t DEPTH_MONO = 1;
constexpr uint32_t VIEWS_MONO = 3;

struct table
{
	std::vector<xrt_status_conv_provider> providers;
	std::vector<xrt_status_conv_task> rows;

	uint32_t
	provider(const char *id)
	{
		xrt_status_conv_provider p;
		std::memset(&p, 0, sizeof(p));
		std::snprintf(p.id, sizeof(p.id), "%s", id);
		providers.push_back(p);
		return (uint32_t)providers.size() - 1;
	}

	void
	row(uint32_t provider_index, uint32_t task, const char *task_name, uint32_t cap_id, bool selected)
	{
		xrt_status_conv_task t;
		std::memset(&t, 0, sizeof(t));
		t.provider = provider_index;
		t.task = task;
		std::snprintf(t.task_name, sizeof(t.task_name), "%s", task_name);
		t.cap_id = cap_id;
		t.selected = selected;
		rows.push_back(t);
	}

	cli_conv_warm_plan
	plan(const char *id, uint32_t cap = 0, uint32_t only_task = 0) const
	{
		return cli_conv_warm_plan_for_provider(rows.data(), (uint32_t)rows.size(), providers.data(),
		                                       (uint32_t)providers.size(), id, cap, only_task);
	}
};

//! The reported shape: the provider's cap 3 serves views-mono directly and is
//! selected; its depth capabilities reach views-mono composed (cap 1, cap 2).
table
leia_and_open()
{
	table t;
	const uint32_t leia = t.provider("leia-conversion");
	const uint32_t open = t.provider("open-conversion");
	t.row(leia, DEPTH_MONO, "depth-mono", 1, true);
	t.row(open, DEPTH_MONO, "depth-mono", 1, false);
	t.row(leia, VIEWS_MONO, "views-mono", 1, false); // composed: depth cap 1 + view synthesis
	t.row(leia, VIEWS_MONO, "views-mono", 2, false); // composed: depth cap 2 + view synthesis
	t.row(leia, VIEWS_MONO, "views-mono", 3, true);
	t.row(open, VIEWS_MONO, "views-mono", 1, false);
	return t;
}

} // namespace

TEST_CASE("conv warm <id> <task>: the provider's own sibling rows are not reported", "[cli][conv_warm]")
{
	const table t = leia_and_open();
	const cli_conv_warm_plan p = t.plan("leia-conversion", 0, VIEWS_MONO);
	CHECK(p.matched);
	REQUIRE(p.tasks.size() == 1u);
	CHECK(p.tasks[0] == VIEWS_MONO);
	// Was: two "cap N is not the selected capability (selected: leia-conversion)" lines.
	CHECK(p.notes.empty());
}

TEST_CASE("conv warm <id>: every task the provider is selected for, each once, no notes", "[cli][conv_warm]")
{
	const table t = leia_and_open();
	const cli_conv_warm_plan p = t.plan("leia-conversion");
	CHECK(p.matched);
	REQUIRE(p.tasks.size() == 2u);
	CHECK(p.tasks[0] == DEPTH_MONO);
	CHECK(p.tasks[1] == VIEWS_MONO);
	CHECK(p.notes.empty());
}

TEST_CASE("conv warm <id>: a provider that is not the selection gets one line per task, naming who is",
          "[cli][conv_warm]")
{
	const table t = leia_and_open();
	const cli_conv_warm_plan p = t.plan("open-conversion");
	CHECK(p.matched);
	CHECK(p.tasks.empty());
	REQUIRE(p.notes.size() == 2u);
	CHECK(p.notes[0] ==
	      "depth-mono: open-conversion is not the selected provider (selected: leia-conversion cap 1) "
	      "-- `conv warm` warms the selection; `conv use depth-mono open-conversion` first to change "
	      "it.");
	CHECK(p.notes[1].rfind("views-mono: open-conversion is not the selected provider (selected: leia-conversion "
	                       "cap 3)",
	                       0) == 0);
}

TEST_CASE("conv warm <id>/<cap>: another capability of the same provider is selected: said once, with the cap",
          "[cli][conv_warm]")
{
	const table t = leia_and_open();
	const cli_conv_warm_plan p = t.plan("leia-conversion", 2, VIEWS_MONO);
	CHECK(p.matched);
	CHECK(p.tasks.empty());
	REQUIRE(p.notes.size() == 1u);
	CHECK(p.notes[0] ==
	      "views-mono: leia-conversion cap 2 is not the selected capability (selected: leia-conversion cap 3) -- "
	      "`conv warm` warms the selection; `conv warm leia-conversion views-mono` warms that one.");

	// The selected capability asked for by number: warmed, nothing to say.
	const cli_conv_warm_plan sel = t.plan("leia-conversion", 3);
	REQUIRE(sel.tasks.size() == 1u);
	CHECK(sel.tasks[0] == VIEWS_MONO);
	CHECK(sel.notes.empty());

	// Cap 1 is selected for depth-mono and a sibling for views-mono: one warm, one line.
	const cli_conv_warm_plan one = t.plan("leia-conversion", 1);
	REQUIRE(one.tasks.size() == 1u);
	CHECK(one.tasks[0] == DEPTH_MONO);
	REQUIRE(one.notes.size() == 1u);
	CHECK(one.notes[0].rfind("views-mono: leia-conversion cap 1 is not the selected capability", 0) == 0);
}

TEST_CASE("conv warm <id>: row order does not matter; nothing selected; no row at all", "[cli][conv_warm]")
{
	// The selected row listed before its siblings.
	table t;
	const uint32_t leia = t.provider("leia-conversion");
	t.row(leia, VIEWS_MONO, "views-mono", 3, true);
	t.row(leia, VIEWS_MONO, "views-mono", 1, false);
	cli_conv_warm_plan p = t.plan("leia-conversion");
	REQUIRE(p.tasks.size() == 1u);
	CHECK(p.notes.empty());

	// Nothing is selected for the task (every candidate dropped).
	table none;
	const uint32_t a = none.provider("acme-conversion");
	none.row(a, VIEWS_MONO, "views-mono", 1, false);
	none.row(a, VIEWS_MONO, "views-mono", 2, false);
	p = none.plan("acme-conversion");
	CHECK(p.matched);
	CHECK(p.tasks.empty());
	REQUIRE(p.notes.size() == 1u);
	CHECK(p.notes[0] ==
	      "views-mono: acme-conversion is not selected (nothing is selected for this task) -- `conv "
	      "warm` warms the selection.");

	// A provider with no row for the request.
	p = leia_and_open().plan("acme-conversion");
	CHECK_FALSE(p.matched);
	CHECK(p.tasks.empty());
	CHECK(p.notes.empty());
	p = t.plan("leia-conversion", 0, DEPTH_MONO);
	CHECK_FALSE(p.matched);
}
