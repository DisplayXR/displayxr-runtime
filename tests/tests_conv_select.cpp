// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-053 conversion selection (conv_select): deterministic, and
 *         readiness never reorders.
 *
 * What must hold (spec conversion-provider-discovery.md §3.4, roadmap §2.4):
 *  - the per-task preference wins, then ProbeOrder, then direct before
 *    composed, then provider id (a cross-provider tie raises tie_warn), then
 *    cap_id; FALLBACK providers sort last inside the ProbeOrder step, but an
 *    explicit per-task preference wins over that;
 *  - RESEARCH_ONLY and UNKNOWN licence classes need AllowResearchOnly; the
 *    grandfathered legacy adapter is exempt;
 *  - input kind, lane, view range and EXPLICIT_VIEWPOINTS filter;
 *  - a waited-for first choice (INSTALLED / DOWNLOADING / WARMING / EVICTED)
 *    is chosen over a READY lower-ranked one; ABSENT (incl. policy),
 *    INCOMPATIBLE, UNLICENSED, DISABLED and ANY FAILED fall through;
 *  - Supersedes hides only the named row;
 *  - Policy\Lock makes the machine (HKLM) preference win.
 */

#include "catch_amalgamated.hpp"

#include "conversion/conv_readiness.h"
#include "conversion/conv_select.h"

#include <string>
#include <vector>

namespace {

conv_select_row
row(const char *id, uint32_t probe, uint32_t task, uint32_t state = XRT_CONV_STATE_READY, uint32_t cap_id = 1)
{
	conv_select_row r{};
	r.provider_id = id;
	r.probe_order = probe;
	r.provider_enabled = true;
	r.provider_usable = true;
	r.cap_id = cap_id;
	r.task = task;
	r.lanes = XRT_CONV_LANE_STREAM;
	r.input_kinds = XRT_CONV_INPUT_MONO;
	r.licence_class = XRT_CONV_LICENCE_PERMISSIVE;
	r.state = state;
	if (task == XRT_CONV_TASK_VIEWS_MONO || task == XRT_CONV_TASK_VIEWS_FROM_DEPTH) {
		r.min_views = 2;
		r.max_views = 8;
		r.flags |= XRT_CONV_CAP_EXPLICIT_VIEWPOINTS;
	}
	return r;
}

conv_select_request
req(uint32_t task, uint32_t views = 0)
{
	conv_select_request q{};
	q.task = task;
	q.input_kind = XRT_CONV_INPUT_MONO;
	q.lane = XRT_CONV_LANE_STREAM;
	q.view_count = views;
	return q;
}

conv_select_result
pick(const std::vector<conv_select_row> &rows,
     const conv_select_request &q,
     const char *preferred = nullptr,
     bool research = false,
     std::vector<uint32_t> *verdicts = nullptr)
{
	conv_select_policy pol{preferred, research};
	if (verdicts != nullptr) {
		verdicts->assign(rows.size(), 0xFFFFFFFFu);
	}
	return conv_select(rows.data(), (uint32_t)rows.size(), &q, &pol,
	                   verdicts != nullptr ? verdicts->data() : nullptr, nullptr);
}

constexpr uint32_t DEPTH = XRT_CONV_TASK_DEPTH_VIDEO;
constexpr uint32_t VIEWS = XRT_CONV_TASK_VIEWS_MONO;

} // namespace

TEST_CASE("conv_select: ProbeOrder ascending when there is no preference")
{
	std::vector<conv_select_row> rows = {row("open", 200, DEPTH), row("vendor", 50, DEPTH),
	                                     row("dp-lift", 150, DEPTH)};
	auto r = pick(rows, req(DEPTH));
	CHECK(r.row == 1);
	CHECK(r.reason == CONV_SEL_OK_ORDER);
	CHECK_FALSE(r.wait);
	CHECK(r.candidate_count == 3);
	CHECK_FALSE(r.tie_warn);
}

TEST_CASE("conv_select: per-task preference beats ProbeOrder")
{
	std::vector<conv_select_row> rows = {row("vendor", 50, DEPTH), row("open", 200, DEPTH)};
	auto r = pick(rows, req(DEPTH), "open");
	CHECK(r.row == 1);
	CHECK(r.reason == CONV_SEL_OK_PREFERRED);

	SECTION("a preference for another provider id changes nothing")
	{
		auto r2 = pick(rows, req(DEPTH), "nobody");
		CHECK(r2.row == 0);
		CHECK(r2.reason == CONV_SEL_OK_ORDER);
	}
	SECTION("a preference naming a cap_id picks that capability")
	{
		std::vector<conv_select_row> caps = {row("open", 200, DEPTH, XRT_CONV_STATE_READY, 1),
		                                     row("open", 200, DEPTH, XRT_CONV_STATE_READY, 7),
		                                     row("vendor", 50, DEPTH)};
		auto r3 = pick(caps, req(DEPTH), "open/7");
		CHECK(r3.row == 1);
		CHECK(r3.reason == CONV_SEL_OK_PREFERRED);
	}
	SECTION("a preference naming a model_id picks that capability")
	{
		std::vector<conv_select_row> caps = {row("open", 200, DEPTH, XRT_CONV_STATE_READY, 1),
		                                     row("open", 200, DEPTH, XRT_CONV_STATE_READY, 2)};
		caps[0].model_id = "small";
		caps[1].model_id = "large";
		auto r4 = pick(caps, req(DEPTH), "open/large");
		CHECK(r4.row == 1);
	}
	SECTION("a preferred provider that is FAILED still falls through")
	{
		rows[1].state = XRT_CONV_STATE_FAILED;
		auto r5 = pick(rows, req(DEPTH), "open");
		CHECK(r5.row == 0);
		CHECK(r5.reason == CONV_SEL_OK_FALLTHROUGH);
		CHECK(r5.fallthrough == 1);
	}
}

TEST_CASE("conv_select: ties on ProbeOrder break by provider id, then cap_id, with tie_warn")
{
	std::vector<conv_select_row> rows = {row("zeta", 100, DEPTH), row("alpha", 100, DEPTH, XRT_CONV_STATE_READY, 9),
	                                     row("alpha", 100, DEPTH, XRT_CONV_STATE_READY, 3)};
	std::vector<uint32_t> order(rows.size());
	conv_select_policy pol{nullptr, false};
	auto q = req(DEPTH);
	auto r = conv_select(rows.data(), (uint32_t)rows.size(), &q, &pol, nullptr, order.data());
	CHECK(r.row == 2); // alpha, cap 3
	CHECK(r.tie_warn);
	CHECK(order == std::vector<uint32_t>{2, 1, 0});

	SECTION("two caps of ONE provider at the same rank is not a provider tie")
	{
		std::vector<conv_select_row> one = {row("alpha", 100, DEPTH, XRT_CONV_STATE_READY, 2),
		                                    row("alpha", 100, DEPTH, XRT_CONV_STATE_READY, 1)};
		auto r2 = pick(one, req(DEPTH));
		CHECK(r2.row == 1);
		CHECK_FALSE(r2.tie_warn);
	}
	SECTION("input order does not matter")
	{
		std::vector<conv_select_row> rev = {rows[2], rows[1], rows[0]};
		auto r3 = pick(rev, req(DEPTH));
		CHECK(std::string(rev[(size_t)r3.row].provider_id) == "alpha");
		CHECK(rev[(size_t)r3.row].cap_id == 3);
	}
}

TEST_CASE("conv_select: direct before composed at equal ProbeOrder")
{
	std::vector<conv_select_row> rows = {row("open", 200, VIEWS), row("open", 200, VIEWS, XRT_CONV_STATE_READY, 5)};
	rows[0].composed = true;
	auto r = pick(rows, req(VIEWS, 2));
	CHECK(r.row == 1);
	CHECK_FALSE(r.tie_warn);
}

TEST_CASE("conv_select: FALLBACK sorts last inside ProbeOrder; an explicit preference wins over it")
{
	std::vector<conv_select_row> rows = {row("sim-conversion", 0, DEPTH),
	                                     row("open", 200, DEPTH, XRT_CONV_STATE_WARMING)};
	rows[0].provider_flags = CONV_PROV_FALLBACK;

	SECTION("unpreferred FALLBACK sorts last despite the lowest ProbeOrder")
	{
		auto r = pick(rows, req(DEPTH));
		CHECK(r.row == 1);
		CHECK(r.wait);
		CHECK(r.reason == CONV_SEL_OK_ORDER);
		auto r2 = pick(rows, req(DEPTH), "open");
		CHECK(r2.row == 1);
	}
	SECTION("a preference for another provider still leaves FALLBACK last")
	{
		rows.push_back(row("vendor", 50, DEPTH));
		auto r = pick(rows, req(DEPTH), "open");
		CHECK(r.row == 1); // preferred open first, then vendor, then the FALLBACK
		std::vector<uint32_t> order(rows.size());
		conv_select_policy pol{"open", false};
		auto q = req(DEPTH);
		conv_select(rows.data(), (uint32_t)rows.size(), &q, &pol, nullptr, order.data());
		CHECK(order == std::vector<uint32_t>{1, 2, 0});
	}
	SECTION("an explicitly preferred FALLBACK provider wins")
	{
		auto r = pick(rows, req(DEPTH), "sim-conversion");
		CHECK(r.row == 0);
		CHECK_FALSE(r.wait);
		CHECK(r.reason == CONV_SEL_OK_PREFERRED);
	}
	SECTION("a preferred FALLBACK that is terminal still falls through")
	{
		rows[0].state = XRT_CONV_STATE_FAILED;
		auto r = pick(rows, req(DEPTH), "sim-conversion");
		CHECK(r.row == 1);
		CHECK(r.reason == CONV_SEL_OK_FALLTHROUGH);
	}
	SECTION("FALLBACK is chosen when it is the only candidate left")
	{
		rows[1].state = XRT_CONV_STATE_ABSENT;
		auto r = pick(rows, req(DEPTH));
		CHECK(r.row == 0);
		CHECK(r.reason == CONV_SEL_OK_FALLTHROUGH);
	}
}

TEST_CASE("conv_select: licence gate")
{
	std::vector<conv_select_row> rows = {row("research", 50, DEPTH), row("unknown", 60, DEPTH),
	                                     row("commercial", 100, DEPTH)};
	rows[0].licence_class = XRT_CONV_LICENCE_RESEARCH_ONLY;
	rows[1].licence_class = XRT_CONV_LICENCE_UNKNOWN;
	rows[2].licence_class = XRT_CONV_LICENCE_COMMERCIAL;

	std::vector<uint32_t> v;
	auto r = pick(rows, req(DEPTH), nullptr, false, &v);
	CHECK(r.row == 2);
	CHECK(r.reason == CONV_SEL_OK_ORDER); // dropped by the filter, not a fall-through
	CHECK(v[0] == CONV_ROW_DROP_LICENCE);
	CHECK(v[1] == CONV_ROW_DROP_LICENCE);
	CHECK(v[2] == CONV_ROW_SELECTED);

	SECTION("AllowResearchOnly admits RESEARCH_ONLY and UNKNOWN")
	{
		auto r2 = pick(rows, req(DEPTH), nullptr, true);
		CHECK(r2.row == 0);
	}
	SECTION("the grandfathered legacy adapter is exempt")
	{
		rows[1].provider_flags = CONV_PROV_LICENCE_EXEMPT;
		auto r3 = pick(rows, req(DEPTH));
		CHECK(r3.row == 1);
	}
	SECTION("a preference cannot bypass the gate")
	{
		auto r4 = pick(rows, req(DEPTH), "research");
		CHECK(r4.row == 2);
	}
	SECTION("an out-of-range class is treated as research-only")
	{
		rows[2].licence_class = 42;
		auto r5 = pick(rows, req(DEPTH));
		CHECK(r5.row == -1);
		CHECK(r5.reason == CONV_SEL_NONE_NO_CANDIDATE);
	}
}

TEST_CASE("conv_select: input kind, lane, view range, explicit viewpoints")
{
	std::vector<conv_select_row> rows = {row("a", 10, VIEWS), row("b", 20, VIEWS), row("c", 30, VIEWS),
	                                     row("d", 40, VIEWS), row("e", 50, VIEWS)};
	rows[0].input_kinds = XRT_CONV_INPUT_STEREO_PAIR;
	rows[1].lanes = XRT_CONV_LANE_JOB;
	rows[2].max_views = 2;
	rows[3].flags &= ~XRT_CONV_CAP_EXPLICIT_VIEWPOINTS;

	std::vector<uint32_t> v;
	auto r = pick(rows, req(VIEWS, 4), nullptr, false, &v);
	CHECK(r.row == 4);
	CHECK(v[0] == CONV_ROW_DROP_INPUT);
	CHECK(v[1] == CONV_ROW_DROP_LANE);
	CHECK(v[2] == CONV_ROW_DROP_VIEWS);
	CHECK(v[3] == CONV_ROW_DROP_VIEWPOINTS);

	SECTION("view_count below min_views drops too")
	{
		auto r2 = pick(rows, req(VIEWS, 1));
		CHECK(r2.row == -1);
		CHECK(r2.reason == CONV_SEL_NONE_NO_CANDIDATE);
	}
	SECTION("2 views fits the max_views = 2 row")
	{
		auto r3 = pick(rows, req(VIEWS, 2));
		CHECK(r3.row == 2);
	}
	SECTION("view range is not applied to depth tasks")
	{
		std::vector<conv_select_row> d = {row("x", 10, DEPTH)};
		CHECK(pick(d, req(DEPTH, 0)).row == 0);
	}
}

TEST_CASE("conv_select: readiness never reorders")
{
	const uint32_t waited[] = {XRT_CONV_STATE_INSTALLED, XRT_CONV_STATE_DOWNLOADING, XRT_CONV_STATE_WARMING,
	                           XRT_CONV_STATE_EVICTED};
	for (uint32_t s : waited) {
		CAPTURE(conv_state_name(s));
		std::vector<conv_select_row> rows = {row("vendor", 50, DEPTH, s),
		                                     row("open", 200, DEPTH, XRT_CONV_STATE_READY)};
		auto r = pick(rows, req(DEPTH));
		CHECK(r.row == 0); // a READY lower-ranked row never pre-empts it
		CHECK(r.wait);
		CHECK(r.fallthrough == 0);
		CHECK(r.reason == CONV_SEL_OK_ORDER);
	}
}

TEST_CASE("conv_select: any FAILED and every terminal state falls through for a new create")
{
	const uint32_t terminal[] = {XRT_CONV_STATE_ABSENT, XRT_CONV_STATE_INCOMPATIBLE, XRT_CONV_STATE_UNLICENSED,
	                             XRT_CONV_STATE_FAILED, XRT_CONV_STATE_DISABLED};
	for (uint32_t s : terminal) {
		CAPTURE(conv_state_name(s));
		std::vector<conv_select_row> rows = {row("vendor", 50, DEPTH, s),
		                                     row("open", 200, DEPTH, XRT_CONV_STATE_WARMING)};
		std::vector<uint32_t> v;
		auto r = pick(rows, req(DEPTH), nullptr, false, &v);
		CHECK(r.row == 1);
		CHECK(r.wait);
		CHECK(r.fallthrough == 1);
		CHECK(r.reason == CONV_SEL_OK_FALLTHROUGH);
		CHECK(v[0] == CONV_ROW_FELL_THROUGH);
		CHECK(v[1] == CONV_ROW_SELECTED);
	}

	SECTION("every FAILED reason, retryable or not")
	{
		const uint32_t reasons[] = {XRT_CONV_FAIL_LICENCE, XRT_CONV_FAIL_LOAD,        XRT_CONV_FAIL_PLATFORM,
		                            XRT_CONV_FAIL_NETWORK, XRT_CONV_FAIL_VERSION,     XRT_CONV_FAIL_RESOURCES,
		                            XRT_CONV_FAIL_MODEL,   XRT_CONV_FAIL_DEVICE_LOST, XRT_CONV_FAIL_CRASH_LOOP};
		for (uint32_t why : reasons) {
			CAPTURE(conv_reason_name(why));
			std::vector<conv_select_row> rows = {row("vendor", 50, DEPTH, XRT_CONV_STATE_FAILED),
			                                     row("open", 200, DEPTH, XRT_CONV_STATE_INSTALLED)};
			rows[0].reason = why;
			CHECK(pick(rows, req(DEPTH)).row == 1);
		}
	}
	SECTION("ABSENT(policy): a forbidden download falls through")
	{
		std::vector<conv_select_row> rows = {row("open", 200, DEPTH, XRT_CONV_STATE_ABSENT),
		                                     row("sim-conversion", 250, DEPTH, XRT_CONV_STATE_READY)};
		rows[0].reason = CONV_REASON_POLICY;
		rows[1].provider_flags = CONV_PROV_FALLBACK;
		auto r = pick(rows, req(DEPTH));
		CHECK(r.row == 1);
		CHECK(r.reason == CONV_SEL_OK_FALLTHROUGH);
	}
	SECTION("FAILED(RESOURCES): a model that cannot fit falls through")
	{
		std::vector<conv_select_row> rows = {row("vendor", 50, DEPTH, XRT_CONV_STATE_FAILED),
		                                     row("open", 200, DEPTH, XRT_CONV_STATE_READY)};
		rows[0].reason = XRT_CONV_FAIL_RESOURCES;
		auto r = pick(rows, req(DEPTH));
		CHECK(r.row == 1);
		CHECK_FALSE(r.wait);
	}
	SECTION("all terminal: NONE_ALL_TERMINAL tells the caller to send the create retry")
	{
		std::vector<conv_select_row> rows = {row("vendor", 50, DEPTH, XRT_CONV_STATE_FAILED),
		                                     row("open", 200, DEPTH, XRT_CONV_STATE_ABSENT)};
		auto r = pick(rows, req(DEPTH));
		CHECK(r.row == -1);
		CHECK(r.reason == CONV_SEL_NONE_ALL_TERMINAL);
		CHECK(r.fallthrough == 2);
	}
	SECTION("the preferred provider is chosen again once it recovers")
	{
		std::vector<conv_select_row> rows = {row("vendor", 50, DEPTH, XRT_CONV_STATE_FAILED),
		                                     row("open", 200, DEPTH, XRT_CONV_STATE_READY)};
		CHECK(pick(rows, req(DEPTH)).row == 1);
		rows[0].state = XRT_CONV_STATE_INSTALLED; // a retry trigger fired
		CHECK(pick(rows, req(DEPTH)).row == 0);
	}
}

TEST_CASE("conv_select: hidden (Supersedes), disabled and unusable providers are not candidates")
{
	std::vector<conv_select_row> rows = {row("vendor", 50, DEPTH, XRT_CONV_STATE_ABSENT),
	                                     row("dp-lift", 150, DEPTH), row("open", 200, DEPTH)};
	rows[1].hidden = true; // vendor declared Supersedes = dp-lift

	std::vector<uint32_t> v;
	auto r = pick(rows, req(DEPTH), nullptr, false, &v);
	// Supersedes hides only the named row: the absent vendor still falls
	// through, to open, not to the hidden adapter.
	CHECK(r.row == 2);
	CHECK(r.reason == CONV_SEL_OK_FALLTHROUGH);
	CHECK(v[1] == CONV_ROW_DROP_HIDDEN);

	SECTION("disabled / unusable")
	{
		rows[1].hidden = false;
		rows[1].provider_enabled = false;
		rows[2].provider_usable = false;
		std::vector<uint32_t> v2;
		auto r2 = pick(rows, req(DEPTH), nullptr, false, &v2);
		CHECK(r2.row == -1);
		CHECK(r2.reason == CONV_SEL_NONE_ALL_TERMINAL);
		CHECK(v2[1] == CONV_ROW_DROP_DISABLED);
		CHECK(v2[2] == CONV_ROW_DROP_UNUSABLE);
	}
}

TEST_CASE("conv_select: reserved tasks and task names")
{
	std::vector<conv_select_row> rows = {row("x", 10, XRT_CONV_TASK_SPLATS_VIDEO)};
	CHECK(pick(rows, req(XRT_CONV_TASK_SPLATS_VIDEO)).reason == CONV_SEL_NONE_RESERVED_TASK);
	CHECK(pick(rows, req(XRT_CONV_TASK_NONE)).reason == CONV_SEL_NONE_RESERVED_TASK);
	CHECK(pick(rows, req(XRT_CONV_TASK_INPAINT)).reason == CONV_SEL_NONE_RESERVED_TASK);

	CHECK(conv_task_from_name("views-mono") == XRT_CONV_TASK_VIEWS_MONO);
	CHECK(conv_task_from_name("depth-video") == XRT_CONV_TASK_DEPTH_VIDEO);
	CHECK(conv_task_from_name("bogus") == XRT_CONV_TASK_NONE);
	for (uint32_t t = XRT_CONV_TASK_DEPTH_MONO; t <= XRT_CONV_TASK_INPAINT; t++) {
		CHECK(conv_task_from_name(conv_task_name(t)) == t);
	}
}

TEST_CASE("conv_select: policy resolution (HKCU over HKLM; Policy\\Lock pins HKLM)")
{
	CHECK(std::string(conv_select_resolve_preference("user", "machine", false)) == "user");
	CHECK(std::string(conv_select_resolve_preference(nullptr, "machine", false)) == "machine");
	CHECK(std::string(conv_select_resolve_preference("", "machine", false)) == "machine");
	CHECK(conv_select_resolve_preference(nullptr, nullptr, false) == nullptr);
	CHECK(std::string(conv_select_resolve_preference("user", "machine", true)) == "machine");
	CHECK(conv_select_resolve_preference("user", nullptr, true) == nullptr); // lock: HKCU ignored

	CHECK_FALSE(conv_select_resolve_research_only(-1, -1, false));
	CHECK(conv_select_resolve_research_only(1, -1, false));
	CHECK(conv_select_resolve_research_only(-1, 1, false));
	CHECK_FALSE(conv_select_resolve_research_only(0, 1, false)); // HKCU overrides
	CHECK_FALSE(conv_select_resolve_research_only(1, -1, true)); // lock: HKLM only
	CHECK(conv_select_resolve_research_only(0, 1, true));

	SECTION("a locked machine preference pins the provider over the user's")
	{
		std::vector<conv_select_row> rows = {row("vendor", 50, DEPTH), row("open", 200, DEPTH)};
		const char *pref = conv_select_resolve_preference("vendor", "open", true);
		CHECK(pick(rows, req(DEPTH), pref).row == 1);
		pref = conv_select_resolve_preference("vendor", "open", false);
		CHECK(pick(rows, req(DEPTH), pref).row == 0);
	}
}
