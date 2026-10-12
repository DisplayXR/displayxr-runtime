// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ADR-053 conversion registry (conv_registry): the capability table
 *         built from describe_cap(), descriptor validation, Supersedes, and
 *         the persisted describe cache.
 *
 * What must hold (spec conversion-provider-discovery.md §3.1, §4.1, §4.5,
 * §7.2; roadmap §2.3):
 *  - describe_cap is walked one index at a time, between two generation reads
 *    (re-walked when the generation moved);
 *  - every malformed descriptor kind is rejected and counted (unknown task,
 *    lanes == 0, views out of order or above 8, VIEWS_* without explicit
 *    viewpoints, unknown licence class, duplicate or zero cap_id, too many
 *    capabilities), while the provider's good rows are kept;
 *  - a re-describe keeps the readiness of a cap_id that survives;
 *  - Supersedes hides only the named provider id;
 *  - the describe cache round-trips byte-for-byte, is keyed on (id, binary
 *    path, size, mtime), rejects corruption, and lets a cold boot select and
 *    answer ACTIVATING before any provider is loaded.
 */

#include "catch_amalgamated.hpp"

#include "conversion/conv_registry.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr uint64_t S = CONV_NS_PER_S;

/*
 *
 * A fake provider: the instance pointer IS the fake.
 *
 */

struct fake
{
	std::vector<xrt_conv_capability> caps;
	std::set<uint32_t> failing_indices;
	uint32_t reported_count_override = 0; //!< non-zero: lie about the count
	uint64_t generation = 1;
	int bump_generation_walks = 0; //!< bump during this many walks
	int walks = 0;
	uint32_t status_state = 1; // READY
	uint32_t status_flags = 0;
};

fake *g_status_fake = nullptr; // get_provider_status takes no instance

fake *
as_fake(xrt_conversion_plugin_instance *inst)
{
	return reinterpret_cast<fake *>(inst);
}

bool
fake_status(xrt_conv_provider_status *out)
{
	if (g_status_fake == nullptr) {
		return false;
	}
	out->state = g_status_fake->status_state;
	out->flags = g_status_fake->status_flags;
	return true;
}

xrt_result_t
fake_describe(xrt_conversion_plugin_instance *inst, uint32_t index, xrt_conv_capability *out, uint32_t *out_count)
{
	fake *f = as_fake(inst);
	uint32_t count = f->reported_count_override != 0 ? f->reported_count_override : (uint32_t)f->caps.size();
	if (out == nullptr) {
		f->walks++;
		if (f->bump_generation_walks > 0) {
			f->bump_generation_walks--;
			f->generation++;
		}
		*out_count = count;
		return XRT_SUCCESS;
	}
	if (out_count != nullptr) {
		*out_count = count;
	}
	if (index >= count || f->failing_indices.count(index) != 0) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	const xrt_conv_capability &src = f->caps[index % f->caps.size()];
	uint32_t keep = out->struct_size;
	std::memcpy(out, &src, std::min<size_t>(keep, sizeof(src)));
	out->struct_size = keep;
	return XRT_SUCCESS;
}

uint64_t
fake_generation(xrt_conversion_plugin_instance *inst)
{
	return as_fake(inst)->generation;
}

xrt_conversion_plugin_iface
make_iface()
{
	xrt_conversion_plugin_iface i{};
	i.struct_size = sizeof(i);
	i.id = "fake";
	i.display_name = "Fake";
	i.get_provider_status = fake_status;
	i.describe_cap = fake_describe;
	i.get_generation = fake_generation;
	return i;
}

const xrt_conversion_plugin_iface k_iface = make_iface();

xrt_conv_capability
cap(uint32_t id, uint32_t task, uint32_t licence = XRT_CONV_LICENCE_PERMISSIVE)
{
	xrt_conv_capability c{};
	c.struct_size = sizeof(c);
	c.cap_id = id;
	c.task = task;
	c.lanes = XRT_CONV_LANE_STREAM;
	c.input_kinds = XRT_CONV_INPUT_MONO;
	c.outputs = XRT_CONV_OUT_DEPTH;
	c.licence_class = licence;
	c.depth_kind = XRT_CONV_DEPTH_AFFINE_INVERSE;
	std::snprintf(c.model_id, sizeof(c.model_id), "model-%u", id);
	std::snprintf(c.backend, sizeof(c.backend), "cpu");
	if (task == XRT_CONV_TASK_VIEWS_MONO || task == XRT_CONV_TASK_VIEWS_FROM_DEPTH) {
		c.min_views = 2;
		c.max_views = 8;
		c.flags = XRT_CONV_CAP_EXPLICIT_VIEWPOINTS;
		c.outputs = XRT_CONV_OUT_VIEWS;
	}
	return c;
}

conv_provider_desc
desc(const char *id, uint32_t probe, fake *f, const char *supersedes = nullptr, const char *path = nullptr)
{
	conv_provider_desc d{};
	d.id = id;
	d.probe_order = probe;
	d.enabled = true;
	d.supersedes = supersedes;
	d.binary_path = path;
	d.binary_size = 1234;
	d.binary_mtime = 5678;
	d.iface = f != nullptr ? &k_iface : nullptr;
	d.inst = reinterpret_cast<xrt_conversion_plugin_instance *>(f);
	return d;
}

std::unique_ptr<conv_registry>
new_registry()
{
	auto reg = std::make_unique<conv_registry>();
	conv_registry_init(reg.get());
	return reg;
}

conv_select_result
select_task(const conv_registry &reg,
            uint32_t task,
            uint32_t views = 0,
            bool composed = false,
            std::vector<conv_select_row> *rows_out = nullptr)
{
	std::vector<conv_select_row> rows(CONV_REGISTRY_MAX_ROWS * 2);
	uint32_t n = conv_registry_build_candidates(&reg, task, composed, rows.data(), nullptr, (uint32_t)rows.size());
	rows.resize(n);
	conv_select_request q{task, XRT_CONV_INPUT_MONO, XRT_CONV_LANE_STREAM, views};
	conv_select_policy pol{nullptr, false};
	auto r = conv_select(rows.data(), n, &q, &pol, nullptr, nullptr);
	if (rows_out != nullptr) {
		*rows_out = rows;
	}
	return r;
}

} // namespace

TEST_CASE("conv_registry: describe builds the capability table, rows start INSTALLED")
{
	auto reg = new_registry();
	fake f;
	f.caps = {cap(1, XRT_CONV_TASK_DEPTH_MONO), cap(2, XRT_CONV_TASK_DEPTH_VIDEO),
	          cap(3, XRT_CONV_TASK_VIEWS_MONO)};
	auto d = desc("vendor", 50, &f);
	int32_t pi = conv_registry_add(reg.get(), &d);
	REQUIRE(pi == 0);
	CHECK_FALSE(conv_registry_discovery_settled(reg.get()));

	CHECK(conv_registry_describe(reg.get(), (uint32_t)pi, 10 * S) == 3);
	CHECK(reg->row_count == 3);
	CHECK(reg->providers[0].described);
	CHECK_FALSE(reg->providers[0].from_cache);
	CHECK(reg->providers[0].bad_descriptors == 0);
	for (uint32_t i = 0; i < reg->row_count; i++) {
		CHECK(reg->rows[i].rd.state == XRT_CONV_STATE_INSTALLED);
		CHECK(reg->rows[i].cap.struct_size == sizeof(xrt_conv_capability));
	}
	CHECK(conv_registry_find_row(reg.get(), 0, 3) == 2);
	CHECK(conv_registry_find_row(reg.get(), 0, 9) == -1);
	CHECK(conv_registry_discovery_settled(reg.get()));

	SECTION("no live iface: describe refuses")
	{
		auto d2 = desc("cold", 100, nullptr);
		int32_t p2 = conv_registry_add(reg.get(), &d2);
		CHECK(conv_registry_describe(reg.get(), (uint32_t)p2, 10 * S) == -1);
		CHECK_FALSE(conv_registry_discovery_settled(reg.get()));
		conv_registry_set_usable(reg.get(), (uint32_t)p2, false); // e.g. ABI rejected
		CHECK(conv_registry_discovery_settled(reg.get()));
	}
	SECTION("re-adding the same id updates it in place")
	{
		auto d3 = desc("vendor", 60, &f);
		CHECK(conv_registry_add(reg.get(), &d3) == 0);
		CHECK(reg->provider_count == 1);
		CHECK(reg->providers[0].probe_order == 60);
	}
	SECTION("bad ids are refused")
	{
		auto bad = desc("", 1, &f);
		CHECK(conv_registry_add(reg.get(), &bad) == -1);
		std::string longid(CONV_ID_MAX, 'x');
		auto bad2 = desc(longid.c_str(), 1, &f);
		CHECK(conv_registry_add(reg.get(), &bad2) == -1);
	}
}

TEST_CASE("conv_registry: validation rejects every malformed descriptor kind")
{
	struct bad_case
	{
		const char *name;
		xrt_conv_capability c;
		uint32_t err;
	};
	std::vector<bad_case> cases;
	{
		auto c = cap(0, XRT_CONV_TASK_DEPTH_MONO);
		cases.push_back({"cap_id zero", c, CONV_CAP_ERR_CAP_ID_ZERO});
	}
	{
		auto c = cap(5, XRT_CONV_TASK_NONE);
		cases.push_back({"task NONE", c, CONV_CAP_ERR_TASK});
	}
	{
		auto c = cap(5, XRT_CONV_TASK_RESERVED_FIRST);
		cases.push_back({"task reserved range", c, CONV_CAP_ERR_TASK});
	}
	{
		auto c = cap(5, 64);
		cases.push_back({"task >= 64", c, CONV_CAP_ERR_TASK});
	}
	{
		auto c = cap(5, XRT_CONV_TASK_DEPTH_MONO);
		c.lanes = 0;
		cases.push_back({"lanes zero", c, CONV_CAP_ERR_LANES});
	}
	{
		auto c = cap(5, XRT_CONV_TASK_DEPTH_MONO);
		c.lanes = XRT_CONV_LANE_STREAM | (1u << 5);
		cases.push_back({"unknown lane bit", c, CONV_CAP_ERR_LANES});
	}
	{
		auto c = cap(5, XRT_CONV_TASK_VIEWS_MONO);
		c.min_views = 4;
		c.max_views = 2;
		cases.push_back({"min_views > max_views", c, CONV_CAP_ERR_VIEWS});
	}
	{
		auto c = cap(5, XRT_CONV_TASK_VIEWS_MONO);
		c.max_views = XRT_CONV_MAX_VIEWS + 1;
		cases.push_back({"max_views > 8", c, CONV_CAP_ERR_VIEWS});
	}
	{
		auto c = cap(5, XRT_CONV_TASK_VIEWS_MONO);
		c.flags = 0;
		cases.push_back({"VIEWS_MONO without EXPLICIT_VIEWPOINTS", c, CONV_CAP_ERR_VIEWPOINTS});
	}
	{
		auto c = cap(5, XRT_CONV_TASK_VIEWS_FROM_DEPTH);
		c.flags = XRT_CONV_CAP_TEMPORAL;
		cases.push_back({"VIEWS_FROM_DEPTH without EXPLICIT_VIEWPOINTS", c, CONV_CAP_ERR_VIEWPOINTS});
	}
	{
		auto c = cap(5, XRT_CONV_TASK_DEPTH_MONO, XRT_CONV_LICENCE_RESEARCH_ONLY + 1);
		cases.push_back({"unknown licence class", c, CONV_CAP_ERR_LICENCE});
	}

	for (const bad_case &b : cases) {
		CAPTURE(b.name);
		CHECK(conv_cap_validate(&b.c) == b.err);

		// Through a describe: the bad row is dropped, the good one kept.
		auto reg = new_registry();
		fake f;
		f.caps = {cap(1, XRT_CONV_TASK_DEPTH_VIDEO), b.c};
		auto d = desc("p", 100, &f);
		conv_registry_add(reg.get(), &d);
		CHECK(conv_registry_describe(reg.get(), 0, S) == 1);
		CHECK(reg->row_count == 1);
		CHECK(reg->rows[0].cap.cap_id == 1);
		CHECK(reg->providers[0].bad_descriptors == 1);
		CHECK(reg->providers[0].first_error == b.err);
	}

	SECTION("a well-formed descriptor of every served task passes")
	{
		for (uint32_t t = XRT_CONV_TASK_DEPTH_MONO; t < XRT_CONV_TASK_RESERVED_FIRST; t++) {
			CAPTURE(t);
			auto c = cap(1, t);
			CHECK(conv_cap_validate(&c) == CONV_CAP_OK);
		}
	}
	SECTION("duplicate cap_id")
	{
		auto reg = new_registry();
		fake f;
		f.caps = {cap(7, XRT_CONV_TASK_DEPTH_MONO), cap(7, XRT_CONV_TASK_DEPTH_VIDEO)};
		auto d = desc("p", 100, &f);
		conv_registry_add(reg.get(), &d);
		CHECK(conv_registry_describe(reg.get(), 0, S) == 1);
		CHECK(reg->providers[0].first_error == CONV_CAP_ERR_DUPLICATE_CAP_ID);
	}
	SECTION("the same cap_id on two providers is fine")
	{
		auto reg = new_registry();
		fake f1, f2;
		f1.caps = {cap(1, XRT_CONV_TASK_DEPTH_MONO)};
		f2.caps = {cap(1, XRT_CONV_TASK_DEPTH_MONO)};
		auto d1 = desc("a", 100, &f1);
		auto d2 = desc("b", 100, &f2);
		conv_registry_add(reg.get(), &d1);
		conv_registry_add(reg.get(), &d2);
		CHECK(conv_registry_describe(reg.get(), 0, S) == 1);
		CHECK(conv_registry_describe(reg.get(), 1, S) == 1);
		CHECK(reg->row_count == 2);
	}
	SECTION("more than XRT_CONV_MAX_CAPABILITIES")
	{
		auto reg = new_registry();
		fake f;
		for (uint32_t i = 1; i <= XRT_CONV_MAX_CAPABILITIES + 3; i++) {
			f.caps.push_back(cap(i, XRT_CONV_TASK_DEPTH_MONO));
		}
		auto d = desc("p", 100, &f);
		conv_registry_add(reg.get(), &d);
		CHECK(conv_registry_describe(reg.get(), 0, S) == (int32_t)XRT_CONV_MAX_CAPABILITIES);
		CHECK(reg->providers[0].first_error == CONV_CAP_ERR_COUNT);
	}
	SECTION("describe_cap failing for an index below the count")
	{
		auto reg = new_registry();
		fake f;
		f.caps = {cap(1, XRT_CONV_TASK_DEPTH_MONO), cap(2, XRT_CONV_TASK_DEPTH_VIDEO)};
		f.failing_indices = {0};
		auto d = desc("p", 100, &f);
		conv_registry_add(reg.get(), &d);
		CHECK(conv_registry_describe(reg.get(), 0, S) == 1);
		CHECK(reg->providers[0].first_error == CONV_CAP_ERR_DESCRIBE_FAILED);
	}
	SECTION("get_state answers are range-checked")
	{
		xrt_conv_state st{};
		st.struct_size = sizeof(st);
		st.state = XRT_CONV_STATE_EVICTED;
		CHECK(conv_cap_validate_state(&st) == CONV_CAP_OK);
		st.state = XRT_CONV_STATE_EVICTED + 1;
		CHECK(conv_cap_validate_state(&st) == CONV_CAP_ERR_STATE);
	}
}

TEST_CASE("conv_registry: generation snapshot, re-describe keeps readiness")
{
	auto reg = new_registry();
	fake f;
	f.caps = {cap(1, XRT_CONV_TASK_DEPTH_MONO), cap(2, XRT_CONV_TASK_DEPTH_VIDEO)};
	f.bump_generation_walks = 1; // moved during the first walk
	auto d = desc("p", 100, &f);
	conv_registry_add(reg.get(), &d);
	CHECK(conv_registry_describe(reg.get(), 0, S) == 2);
	CHECK(f.walks == 2);
	CHECK(reg->providers[0].generation == f.generation);

	// Cap 2 becomes READY; then the provider drops cap 1 and adds cap 3.
	int32_t r2 = conv_registry_find_row(reg.get(), 0, 2);
	conv_readiness_event e{};
	e.type = CONV_EV_WARM_REQUESTED;
	e.now_ns = 2 * S;
	conv_readiness_apply(&reg->rows[r2].rd, &e);
	e.type = CONV_EV_WARM_OK;
	conv_readiness_apply(&reg->rows[r2].rd, &e);

	f.caps = {cap(2, XRT_CONV_TASK_DEPTH_VIDEO), cap(3, XRT_CONV_TASK_VIEWS_MONO)};
	f.caps[0].max_input_edge = 1024; // refined
	f.generation++;
	CHECK(conv_registry_describe(reg.get(), 0, 3 * S) == 2);
	CHECK(conv_registry_find_row(reg.get(), 0, 1) == -1);
	r2 = conv_registry_find_row(reg.get(), 0, 2);
	REQUIRE(r2 >= 0);
	CHECK(reg->rows[r2].rd.state == XRT_CONV_STATE_READY);
	CHECK(reg->rows[r2].cap.max_input_edge == 1024);
	int32_t r3 = conv_registry_find_row(reg.get(), 0, 3);
	REQUIRE(r3 >= 0);
	CHECK(reg->rows[r3].rd.state == XRT_CONV_STATE_INSTALLED);
}

TEST_CASE("conv_registry: provider status (FALLBACK, platform absent)")
{
	auto reg = new_registry();
	fake f;
	f.caps = {cap(1, XRT_CONV_TASK_DEPTH_MONO)};
	f.status_flags = XRT_CONV_PROVIDER_FLAG_FALLBACK;
	g_status_fake = &f;
	auto d = desc("sim-conversion", 250, &f);
	conv_registry_add(reg.get(), &d);
	conv_registry_describe(reg.get(), 0, S);
	CHECK((reg->providers[0].flags & CONV_PROV_FALLBACK) != 0);
	CHECK(reg->providers[0].usable);

	f.status_state = 2; // PLATFORM_ABSENT
	conv_registry_describe(reg.get(), 0, S);
	CHECK_FALSE(reg->providers[0].usable);
	CHECK(select_task(*reg, XRT_CONV_TASK_DEPTH_MONO).row == -1);
	g_status_fake = nullptr;
}

TEST_CASE("conv_registry: Supersedes hides only the named provider id")
{
	auto reg = new_registry();
	fake fv, fl, fo;
	fv.caps = {cap(1, XRT_CONV_TASK_DEPTH_VIDEO)};
	fl.caps = {cap(2, XRT_CONV_TASK_DEPTH_VIDEO, XRT_CONV_LICENCE_UNKNOWN)};
	fo.caps = {cap(1, XRT_CONV_TASK_DEPTH_VIDEO)};
	auto dv = desc("vendor", 50, &fv, "dp-lift");
	auto dl = desc("dp-lift", 150, &fl);
	dl.flags = CONV_PROV_LICENCE_EXEMPT;
	auto dopen = desc("open", 200, &fo);
	conv_registry_add(reg.get(), &dv);
	conv_registry_add(reg.get(), &dl);
	conv_registry_add(reg.get(), &dopen);
	for (uint32_t i = 0; i < 3; i++) {
		conv_registry_describe(reg.get(), i, S);
	}

	CHECK_FALSE(conv_registry_is_hidden(reg.get(), 0));
	CHECK(conv_registry_is_hidden(reg.get(), 1));
	CHECK_FALSE(conv_registry_is_hidden(reg.get(), 2));

	std::vector<conv_select_row> rows;
	auto r = select_task(*reg, XRT_CONV_TASK_DEPTH_VIDEO, 0, false, &rows);
	REQUIRE(r.row >= 0);
	CHECK(std::string(rows[(size_t)r.row].provider_id) == "vendor");

	SECTION("the superseding provider's module is absent: fall through to open, not to dp-lift")
	{
		int32_t rv = conv_registry_find_row(reg.get(), 0, 1);
		conv_readiness_event e{};
		e.type = CONV_EV_DESCRIBE_FAILED;
		e.reason = CONV_REASON_NOT_DECLARED;
		e.now_ns = 2 * S;
		conv_readiness_apply(&reg->rows[rv].rd, &e);
		auto r2 = select_task(*reg, XRT_CONV_TASK_DEPTH_VIDEO, 0, false, &rows);
		REQUIRE(r2.row >= 0);
		CHECK(std::string(rows[(size_t)r2.row].provider_id) == "open");
		CHECK(r2.reason == CONV_SEL_OK_FALLTHROUGH);
	}
	SECTION("without the superseding provider the adapter is a candidate again")
	{
		auto dv2 = desc("vendor", 50, &fv, nullptr);
		conv_registry_add(reg.get(), &dv2);
		CHECK_FALSE(conv_registry_is_hidden(reg.get(), 1));
	}
	SECTION("Supersedes naming an unknown id, or itself, hides nothing")
	{
		auto dv3 = desc("vendor", 50, &fv, "vendor");
		conv_registry_add(reg.get(), &dv3);
		auto do3 = desc("open", 200, &fo, "nobody");
		conv_registry_add(reg.get(), &do3);
		for (uint32_t i = 0; i < 3; i++) {
			CHECK_FALSE(conv_registry_is_hidden(reg.get(), i));
		}
	}
}

TEST_CASE("conv_registry: composed VIEWS_MONO candidates")
{
	auto reg = new_registry();
	fake fa, fb;
	fa.caps = {cap(1, XRT_CONV_TASK_DEPTH_VIDEO)};
	fb.caps = {cap(4, XRT_CONV_TASK_VIEWS_MONO)};
	auto da = desc("depth-only", 50, &fa);
	auto db = desc("views", 50, &fb);
	conv_registry_add(reg.get(), &da);
	conv_registry_add(reg.get(), &db);
	conv_registry_describe(reg.get(), 0, S);
	conv_registry_describe(reg.get(), 1, S);

	std::vector<conv_select_row> rows;
	auto r = select_task(*reg, XRT_CONV_TASK_VIEWS_MONO, 4, true, &rows);
	REQUIRE(rows.size() == 2);
	CHECK(rows[1].composed);
	CHECK(rows[1].task == XRT_CONV_TASK_VIEWS_MONO);
	CHECK(rows[1].max_views == XRT_CONV_MAX_VIEWS);
	// Same ProbeOrder: the direct row beats the composed pipeline.
	CHECK(std::string(rows[(size_t)r.row].provider_id) == "views");

	CHECK(select_task(*reg, XRT_CONV_TASK_VIEWS_MONO, 4, false, &rows).row == 0);
	CHECK(rows.size() == 1);
}

TEST_CASE("conv_registry: describe cache round-trip and cold boot")
{
	auto live = new_registry();
	fake fv, fo;
	fv.caps = {cap(1, XRT_CONV_TASK_DEPTH_MONO), cap(2, XRT_CONV_TASK_DEPTH_VIDEO),
	           cap(3, XRT_CONV_TASK_VIEWS_MONO)};
	fo.caps = {cap(9, XRT_CONV_TASK_DEPTH_VIDEO)};
	fo.generation = 77;
	auto dv = desc("vendor", 50, &fv, "dp-lift", "C:\\providers\\vendor.dll");
	auto dopen = desc("open", 200, &fo, nullptr, "C:\\providers\\open.dll");
	conv_registry_add(live.get(), &dv);
	conv_registry_add(live.get(), &dopen);
	auto dnever = desc("never-described", 100, nullptr);
	conv_registry_add(live.get(), &dnever);
	conv_registry_describe(live.get(), 0, S);
	conv_registry_describe(live.get(), 1, S);

	size_t need = conv_registry_cache_save(live.get(), nullptr, 0);
	CHECK(need > 32);
	std::vector<uint8_t> small(need - 1);
	CHECK(conv_registry_cache_save(live.get(), small.data(), small.size()) == need); // nothing written
	std::vector<uint8_t> buf(need);
	REQUIRE(conv_registry_cache_save(live.get(), buf.data(), buf.size()) == need);

	// Cold boot: same registration, nothing loaded.
	auto cold = new_registry();
	auto cv = desc("vendor", 50, nullptr, "dp-lift", "C:\\providers\\vendor.dll");
	auto co = desc("open", 200, nullptr, nullptr, "C:\\providers\\open.dll");
	conv_registry_add(cold.get(), &cv);
	conv_registry_add(cold.get(), &co);
	CHECK_FALSE(conv_registry_discovery_settled(cold.get()));

	REQUIRE(conv_registry_cache_load(cold.get(), buf.data(), buf.size(), 5 * S) == 2);
	CHECK(conv_registry_discovery_settled(cold.get()));
	REQUIRE(cold->row_count == live->row_count);
	for (uint32_t i = 0; i < live->row_count; i++) {
		CAPTURE(i);
		CHECK(cold->rows[i].provider == live->rows[i].provider);
		CHECK(std::memcmp(&cold->rows[i].cap, &live->rows[i].cap, sizeof(xrt_conv_capability)) == 0);
		CHECK(cold->rows[i].rd.state == XRT_CONV_STATE_INSTALLED);
	}
	CHECK(cold->providers[0].from_cache);
	CHECK(cold->providers[1].generation == 77);
	CHECK(std::string(cold->rows[0].cap.model_id) == "model-1");

	// Answers from memory before any provider is loaded: selection works and a
	// v3 caller reads ACTIVATING.
	std::vector<conv_select_row> rows;
	auto r = select_task(*cold, XRT_CONV_TASK_VIEWS_MONO, 2, false, &rows);
	REQUIRE(r.row >= 0);
	CHECK(std::string(rows[(size_t)r.row].provider_id) == "vendor");
	CHECK(r.wait);
	uint32_t st = rows[(size_t)r.row].state;
	CHECK(conv_legacy_v3_state(&st, 1, conv_registry_discovery_settled(cold.get())) == CONV_V3_STATE_ACTIVATING);

	// Saving the restored registry reproduces the same bytes.
	std::vector<uint8_t> again(conv_registry_cache_save(cold.get(), nullptr, 0));
	conv_registry_cache_save(cold.get(), again.data(), again.size());
	CHECK(again == buf);

	SECTION("a live describe replaces the cached rows, keeping their readiness")
	{
		auto lv = desc("vendor", 50, &fv, "dp-lift", "C:\\providers\\vendor.dll");
		conv_registry_add(cold.get(), &lv);
		conv_registry_describe(cold.get(), 0, 6 * S);
		CHECK_FALSE(cold->providers[0].from_cache);
		CHECK(cold->row_count == live->row_count);
	}
	SECTION("a different binary (size / mtime / path) is not restored")
	{
		auto other = new_registry();
		auto ov = desc("vendor", 50, nullptr, "dp-lift", "C:\\providers\\vendor.dll");
		ov.binary_mtime = 9999;
		auto oo = desc("open", 200, nullptr, nullptr, "C:\\elsewhere\\open.dll");
		conv_registry_add(other.get(), &ov);
		conv_registry_add(other.get(), &oo);
		CHECK(conv_registry_cache_load(other.get(), buf.data(), buf.size(), S) == 0);
		CHECK(other->row_count == 0);
		CHECK_FALSE(conv_registry_discovery_settled(other.get()));
	}
	SECTION("a provider no longer registered is skipped; an already-live one is not overwritten")
	{
		auto other = new_registry();
		auto oo = desc("open", 200, &fo, nullptr, "C:\\providers\\open.dll");
		conv_registry_add(other.get(), &oo);
		conv_registry_describe(other.get(), 0, S);
		CHECK(conv_registry_cache_load(other.get(), buf.data(), buf.size(), S) == 0);
		CHECK(other->row_count == 1);
		CHECK_FALSE(other->providers[0].from_cache);
	}
	SECTION("corruption is rejected")
	{
		auto fresh = [&]() {
			auto c = new_registry();
			conv_registry_add(c.get(), &cv);
			conv_registry_add(c.get(), &co);
			return c;
		};
		std::vector<uint8_t> b = buf;
		b[b.size() / 2] ^= 0x5A;
		CHECK(conv_registry_cache_load(fresh().get(), b.data(), b.size(), S) == CONV_CACHE_ERR_CHECKSUM);

		CHECK(conv_registry_cache_load(fresh().get(), buf.data(), buf.size() - 1, S) == CONV_CACHE_ERR_SHORT);
		CHECK(conv_registry_cache_load(fresh().get(), buf.data(), 10, S) == CONV_CACHE_ERR_SHORT);
		CHECK(conv_registry_cache_load(fresh().get(), nullptr, 0, S) == CONV_CACHE_ERR_SHORT);

		b = buf;
		b[0] = 'X';
		CHECK(conv_registry_cache_load(fresh().get(), b.data(), b.size(), S) == CONV_CACHE_ERR_MAGIC);

		b = buf;
		b[8] = 99; // version
		CHECK(conv_registry_cache_load(fresh().get(), b.data(), b.size(), S) == CONV_CACHE_ERR_VERSION);

		b = buf;
		std::swap(b[12], b[15]); // endian tag
		CHECK(conv_registry_cache_load(fresh().get(), b.data(), b.size(), S) == CONV_CACHE_ERR_ENDIAN);
	}
	SECTION("a malformed row inside a valid cache is dropped on load")
	{
		// Find cap 2's task field in the buffer and break it, then fix the checksum.
		auto bad = new_registry();
		fake fb;
		fb.caps = {cap(1, XRT_CONV_TASK_DEPTH_MONO)};
		auto db = desc("vendor", 50, &fb, nullptr, "C:\\providers\\vendor.dll");
		conv_registry_add(bad.get(), &db);
		conv_registry_describe(bad.get(), 0, S);
		bad->rows[0].cap.task = XRT_CONV_TASK_NONE; // corrupt in memory, then save
		std::vector<uint8_t> b(conv_registry_cache_save(bad.get(), nullptr, 0));
		conv_registry_cache_save(bad.get(), b.data(), b.size());

		auto c = new_registry();
		conv_registry_add(c.get(), &cv);
		CHECK(conv_registry_cache_load(c.get(), b.data(), b.size(), S) == 1);
		CHECK(c->row_count == 0);
		CHECK(c->providers[0].first_error == CONV_CAP_ERR_TASK);
	}
}
