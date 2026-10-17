// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `displayxr-cli conv run | consent | camera-block` (ADR-057 S1 / S2).
 *
 *   conv run <task> <in> [<in-right>] <out> [--provider <id>] [--format sog|ply]
 *            [--baseline-m <m>] [--rectified | --raw] [--sbs] [--focal-px <px>]
 *            [--max-long-side <px>] [--facts <facts.json>] [--camera-block auto]
 *            [--allow-research-only] [--json]
 *       One job, file to file. The CLI decodes the input itself (the service
 *       never decodes media), hosts the provider HEADLESS in this process —
 *       negotiate, probe, describe, warm, job_execute, the same job lane the
 *       conversion core drives — and writes the blob to <out>. Until the
 *       service carries spec-5 jobs over IPC this is the only file-to-file
 *       path; for a local provider it loads a second model instance, which the
 *       command says. Prints the result's FACTS (build stamp, refusal, metric
 *       fit, layers, focus suggestion); --facts saves them (X-DXR-Result shape).
 *       The blob is written untouched: the runtime never writes a camera block.
 *       --camera-block auto is the separate convenience writer (ADR-057 §3):
 *       facts + suggestions into the SOG meta.json as `camera`, `"source":
 *       "auto"`, never over an authored block.
 *   conv consent <id> grant | revoke | status [--json]
 *       Off-box consent: may provider <id> send pictures to a remote endpoint?
 *       Per user, revocable; stored beside the camera consent (ADR-043 store).
 *       `conv consent status` with no id lists every grant.
 *   conv camera-block <file.sog | meta.json | dir> --facts <facts.json>
 *       The convenience writer on its own (e.g. after a `conv run --facts`).
 *
 * @ingroup cli
 */

#include "cli_common.h"

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_conversion_plugin.h"
#include "os/os_time.h"
#include "util/u_camera_consent.h"
#include "util/u_credential.h"

#include "target_conv_status.h"
#include "target_conversion_plugin_loader.h"
#include "conversion/conv_camera_block.h"

// A private (static) decoder: cli_cmd_lift.cpp keeps its own, Windows-only.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <cjson/cJSON.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#define P(...) fprintf(stdout, __VA_ARGS__)
#define E(...) fprintf(stderr, __VA_ARGS__)

namespace {

std::atomic<bool> g_cancel{false};

void
on_sigint(int)
{
	g_cancel.store(true);
}

bool
is_cancelled(void *)
{
	return g_cancel.load();
}

const char *const k_value_flags[] = {"--provider", "--format",       "--baseline-m", "--focal-px", "--max-long-side",
                                     "--facts",    "--camera-block", "--timeout",    nullptr};

bool
takes_value(const char *a)
{
	for (int i = 0; k_value_flags[i] != nullptr; i++) {
		if (strcmp(a, k_value_flags[i]) == 0) {
			return true;
		}
	}
	return false;
}

const char *
value_of(int argc, const char **argv, const char *name)
{
	for (int i = 3; i + 1 < argc; i++) {
		if (strcmp(argv[i], name) == 0) {
			return argv[i + 1];
		}
	}
	return nullptr;
}

//! The positional arguments after `conv <verb>`.
std::vector<const char *>
positionals(int argc, const char **argv)
{
	std::vector<const char *> out;
	for (int i = 3; i < argc; i++) {
		if (strncmp(argv[i], "--", 2) == 0) {
			if (takes_value(argv[i])) {
				i++;
			}
			continue;
		}
		out.push_back(argv[i]);
	}
	return out;
}

const char *
state_word(uint32_t s)
{
	static const char *const w[] = {"unknown", "ready",    "warming",   "absent",      "incompatible", "unlicensed",
	                                "failed",  "disabled", "installed", "downloading", "evicted"};
	return s < sizeof(w) / sizeof(w[0]) ? w[s] : "?";
}

const char *
licence_word(uint32_t c)
{
	switch (c) {
	case XRT_CONV_LICENCE_PERMISSIVE: return "permissive";
	case XRT_CONV_LICENCE_COMMERCIAL: return "commercial";
	case XRT_CONV_LICENCE_RESEARCH_ONLY: return "research_only";
	default: return "unknown";
	}
}

//! RGBA8 image, decoded; optionally box-downscaled so the long side is <= max.
struct pixels
{
	int w = 0, h = 0;
	std::vector<uint8_t> rgba;
};

bool
load_image(const char *path, pixels &out)
{
	int w = 0, h = 0, c = 0;
	stbi_uc *p = stbi_load(path, &w, &h, &c, 4);
	if (p == nullptr) {
		E("conv run: cannot decode '%s': %s\n", path, stbi_failure_reason());
		return false;
	}
	out.w = w;
	out.h = h;
	out.rgba.assign(p, p + (size_t)w * h * 4);
	stbi_image_free(p);
	return true;
}

//! The runtime's rule (cap.max_input_edge): downscale, never thin the output.
void
downscale(pixels &img, uint32_t max_edge, int split_columns)
{
	const int long_side = std::max(img.w / split_columns, img.h);
	if (max_edge == 0 || long_side <= (int)max_edge) {
		return;
	}
	const double s = (double)max_edge / (double)long_side;
	const int nw = std::max(split_columns, (int)((double)img.w * s) / split_columns * split_columns);
	const int nh = std::max(1, (int)((double)img.h * s));
	std::vector<uint8_t> dst((size_t)nw * nh * 4);
	for (int y = 0; y < nh; y++) {
		const int y0 = (int)((double)y / nh * img.h),
		          y1 = std::max(y0 + 1, (int)((double)(y + 1) / nh * img.h));
		for (int x = 0; x < nw; x++) {
			const int x0 = (int)((double)x / nw * img.w),
			          x1 = std::max(x0 + 1, (int)((double)(x + 1) / nw * img.w));
			uint32_t acc[4] = {0, 0, 0, 0}, n = 0;
			for (int yy = y0; yy < y1 && yy < img.h; yy++) {
				for (int xx = x0; xx < x1 && xx < img.w; xx++) {
					const uint8_t *p = &img.rgba[((size_t)yy * img.w + xx) * 4];
					for (int k = 0; k < 4; k++) {
						acc[k] += p[k];
					}
					n++;
				}
			}
			for (int k = 0; k < 4; k++) {
				dst[((size_t)y * nw + x) * 4 + k] = (uint8_t)(n != 0 ? acc[k] / n : 0);
			}
		}
	}
	E("conv run: input downscaled %dx%d -> %dx%d (the provider's max input edge is %u px)\n", img.w, img.h, nw, nh,
	  max_edge);
	img.w = nw;
	img.h = nh;
	img.rgba.swap(dst);
}

struct file_sink
{
	std::string tmp;
	FILE *f = nullptr;
	uint32_t format = 0;
	uint64_t written = 0;
	bool failed = false;
};

bool
sink_begin(void *ctx, uint32_t blob_format, uint64_t)
{
	auto *s = (file_sink *)ctx;
	s->format = blob_format;
	if (s->f != nullptr) {
		fclose(s->f);
	}
	s->f = fopen(s->tmp.c_str(), "wb");
	s->written = 0;
	s->failed = s->f == nullptr;
	return !s->failed;
}

bool
sink_write(void *ctx, const void *bytes, uint64_t count)
{
	auto *s = (file_sink *)ctx;
	if (s->f == nullptr || fwrite(bytes, 1, (size_t)count, s->f) != count) {
		s->failed = true;
		return false;
	}
	s->written += count;
	return true;
}

int g_last_pct = -1;

void
on_progress(void *, float fraction, const char *stage)
{
	const int pct = (int)(fraction * 100.0f + 0.5f);
	if (pct / 10 != g_last_pct / 10) {
		E("conv run: %3d%%  %s\n", pct, stage != nullptr ? stage : "");
		g_last_pct = pct;
	}
}

void
print_facts_human(const cJSON *facts)
{
	char *text = cJSON_Print(facts);
	P("facts (measured / suggested by the provider; never the author's intent):\n%s\n",
	  text != nullptr ? text : "{}");
	free(text);
}

bool
write_text(const char *path, const char *text)
{
	FILE *f = fopen(path, "wb");
	if (f == nullptr) {
		return false;
	}
	const bool ok = fputs(text, f) >= 0;
	return fclose(f) == 0 && ok;
}

struct hosted
{
	xrt_conversion_plugin_entry *e = nullptr;
	const xrt_conversion_plugin_iface *iface = nullptr;
	xrt_conversion_plugin_instance *inst = nullptr;
	xrt_conv_capability cap{};
};

void
release(hosted &h)
{
	if (h.inst != nullptr && h.iface != nullptr && h.iface->destroy != nullptr) {
		h.iface->destroy(h.inst);
	}
	h.inst = nullptr;
	if (h.e != nullptr) {
		target_conversion_plugin_unload(h.e);
	}
	target_conv_status_set_headless_entry(nullptr);
	h.iface = nullptr;
}

bool
read_state(const hosted &h, xrt_conv_state &st)
{
	memset(&st, 0, sizeof(st));
	st.struct_size = sizeof(st);
	return h.iface->get_state != nullptr && h.iface->get_state(h.inst, h.cap.cap_id, &st) == XRT_SUCCESS;
}

/*!
 * Negotiate + probe @p e headless and find its job capability for @p task.
 * False (and unloaded) when it does not serve the task.
 */
bool
host_one(xrt_conversion_plugin_entry *e, uint32_t task, hosted &h)
{
	h = hosted{};
	h.e = e;
	target_conv_status_set_headless_entry(e);
	if (!target_conversion_plugin_negotiate_one(e, target_conv_status_headless_host())) {
		E("conv run: %s: not loaded (%s)\n", e->id, e->reason);
		release(h);
		return false;
	}
	h.iface = e->iface;
	if (h.iface->probe == nullptr || h.iface->probe(&h.inst) != XRT_SUCCESS || h.inst == nullptr) {
		E("conv run: %s: probe declined (is it enabled? conv options %s)\n", e->id, e->id);
		release(h);
		return false;
	}
	uint32_t count = 0;
	(void)h.iface->describe_cap(h.inst, 0, nullptr, &count);
	for (uint32_t i = 0; i < count && i < XRT_CONV_MAX_CAPABILITIES; i++) {
		xrt_conv_capability c{};
		c.struct_size = sizeof(c);
		if (h.iface->describe_cap(h.inst, i, &c, nullptr) == XRT_SUCCESS && c.task == task &&
		    (c.lanes & XRT_CONV_LANE_JOB) != 0) {
			h.cap = c;
			return true;
		}
	}
	release(h);
	return false;
}

bool
recheck_cap(hosted &h)
{
	uint32_t count = 0;
	(void)h.iface->describe_cap(h.inst, 0, nullptr, &count);
	for (uint32_t i = 0; i < count && i < XRT_CONV_MAX_CAPABILITIES; i++) {
		xrt_conv_capability c{};
		c.struct_size = sizeof(c);
		if (h.iface->describe_cap(h.inst, i, &c, nullptr) == XRT_SUCCESS && c.cap_id == h.cap.cap_id) {
			h.cap = c;
			return true;
		}
	}
	return false;
}

int
usage_run(void)
{
	E("usage: displayxr-cli conv run splats-mono <in> <out.sog|.ply> [options]\n"
	  "       displayxr-cli conv run splats-stereo <left> <right> <out> [options]   (or one SBS image with --sbs)\n"
	  "  --provider <id>        prefer this provider (a hint: the runtime's selection still arbitrates;\n"
	  "                         an unknown or terminal one falls through to the default)\n"
	  "  --format sog|ply       blob format (default from the output extension)\n"
	  "  --baseline-m <m>       stereo baseline in metres; omitted = unknown (no metric result, never defaulted)\n"
	  "  --rectified | --raw    declare the pair (stereo needs one; it is never guessed)\n"
	  "  --focal-px <px>        a GIVEN focal length (wins over EXIF / estimates)\n"
	  "  --max-long-side <px>   bound the splat count by the input size\n"
	  "  --facts <file.json>    save the result's facts (X-DXR-Result shape)\n"
	  "  --camera-block auto    also write facts + suggestions as the SOG camera block, \"source\":\"auto\"\n"
	  "                         (never over an authored block)\n"
	  "  --allow-research-only  run a research-only / unknown-licence capability (this run only)\n"
	  "  --timeout <s>          warm + job deadline in seconds (default 600)\n");
	return 2;
}

} // namespace


/*
 *
 * conv run.
 *
 */

extern "C" int
cli_cmd_conv_run(int argc, const char **argv)
{
	const std::vector<const char *> pos = positionals(argc, argv);
	if (pos.empty()) {
		return usage_run();
	}
	const char *task_name = pos[0];
	uint32_t task = 0;
	if (strcmp(task_name, "splats-mono") == 0) {
		task = XRT_CONV_TASK_SPLATS_MONO;
	} else if (strcmp(task_name, "splats-stereo") == 0) {
		task = XRT_CONV_TASK_SPLATS_STEREO;
	} else {
		E("conv run: '%s': this release runs splats-mono and splats-stereo jobs\n", task_name);
		return usage_run();
	}
	const bool sbs = cli_has_flag(argc, argv, "--sbs");
	const bool stereo = task == XRT_CONV_TASK_SPLATS_STEREO;
	const size_t need = stereo && !sbs ? 4u : 3u;
	if (pos.size() != need) {
		return usage_run();
	}
	const char *out_path = pos[need - 1];
	const bool json = cli_has_flag(argc, argv, "--json");
	const bool rectified = cli_has_flag(argc, argv, "--rectified");
	const bool raw = cli_has_flag(argc, argv, "--raw");
	if (rectified && raw) {
		E("conv run: --rectified and --raw contradict each other\n");
		return 2;
	}
	const char *camera_block = value_of(argc, argv, "--camera-block");
	if (camera_block != nullptr && strcmp(camera_block, "auto") != 0) {
		E("conv run: --camera-block takes 'auto' (the only mode a tool may write)\n");
		return 2;
	}
	float baseline = 0.0f, focal = 0.0f;
	if (const char *v = value_of(argc, argv, "--baseline-m")) {
		baseline = strtof(v, nullptr);
		if (!(baseline > 0.0f && baseline <= XRT_CONV_MAX_BASELINE_M)) {
			E("conv run: --baseline-m must be in (0, %.0f] metres; omit it when unknown\n",
			  (double)XRT_CONV_MAX_BASELINE_M);
			return 2;
		}
	}
	if (const char *v = value_of(argc, argv, "--focal-px")) {
		focal = strtof(v, nullptr);
		if (!(focal > 0.0f)) {
			E("conv run: --focal-px must be > 0\n");
			return 2;
		}
	}
	const uint32_t max_long = value_of(argc, argv, "--max-long-side") != nullptr
	                              ? (uint32_t)strtoul(value_of(argc, argv, "--max-long-side"), nullptr, 10)
	                              : 0u;
	const double timeout_s =
	    value_of(argc, argv, "--timeout") != nullptr ? strtod(value_of(argc, argv, "--timeout"), nullptr) : 600.0;
	uint32_t format = 0;
	const char *fmt = value_of(argc, argv, "--format");
	const size_t olen = strlen(out_path);
	if (fmt != nullptr) {
		format = strcmp(fmt, "sog") == 0   ? XRT_CONV_BLOB_SOG
		         : strcmp(fmt, "ply") == 0 ? XRT_CONV_BLOB_PLY_3DGS
		                                   : 0u;
		if (format == 0) {
			E("conv run: --format sog | ply\n");
			return 2;
		}
	} else if (olen > 4 && strcmp(out_path + olen - 4, ".ply") == 0) {
		format = XRT_CONV_BLOB_PLY_3DGS;
	} else if (olen > 4 && strcmp(out_path + olen - 4, ".sog") == 0) {
		format = XRT_CONV_BLOB_SOG;
	}
	if (camera_block != nullptr && format == XRT_CONV_BLOB_PLY_3DGS) {
		E("conv run: --camera-block needs a SOG output (a PLY has no meta.json)\n");
		return 2;
	}

	pixels img[2];
	if (!load_image(pos[1], img[0]) || (stereo && !sbs && !load_image(pos[2], img[1]))) {
		return 1;
	}
	if (stereo && !sbs && (img[0].w != img[1].w || img[0].h != img[1].h)) {
		E("conv run: the two eyes differ in size\n");
		return 1;
	}

	// Providers: registered, ranked by the runtime's own selection (ProbeOrder,
	// PreferredProvider, licence gate, readiness fall-through), per-user Enabled
	// applied. --provider is the per-create preference HINT of lift spec 5: the
	// same rule an app's XrLiftProviderPreferenceDXR goes through.
	auto *entries =
	    (xrt_conversion_plugin_entry *)calloc(XRT_CONV_LOADER_MAX_ENTRIES, sizeof(xrt_conversion_plugin_entry));
	if (entries == nullptr) {
		return 1;
	}
	target_conv_policy policy;
	target_conv_policy_read(&policy);
	const bool allow_research = policy.allow_research_only || cli_has_flag(argc, argv, "--allow-research-only");
	const int n = target_conversion_plugin_enumerate(entries, XRT_CONV_LOADER_MAX_ENTRIES);
	const char *want = value_of(argc, argv, "--provider");
	target_conv_policy sel_policy = policy;
	sel_policy.allow_research_only = allow_research;
	auto status = std::make_unique<target_conv_status>();
	auto select = [&](const target_conv_policy &pol) -> const char * {
		target_conv_status_build_from_entries_hinted(entries, n < 0 ? 0 : n, nullptr, &pol, task, want,
		                                             status.get());
		for (uint32_t i = 0; i < status->task_count && i < XRT_STATUS_MAX_CONV_TASKS; i++) {
			const xrt_status_conv_task &t = status->tasks[i];
			if (t.task == task && t.selected && t.provider < status->provider_count &&
			    (t.lanes & XRT_CONV_LANE_JOB) != 0) {
				return status->providers[t.provider].id;
			}
		}
		return nullptr;
	};
	const char *chosen = select(sel_policy);
	if (chosen == nullptr && !allow_research) {
		// Nothing eligible without the opt-in: rank once more with it, so a
		// research-only provider is named (and refused below with its licence)
		// rather than reported as missing.
		target_conv_policy research = sel_policy;
		research.allow_research_only = true;
		chosen = select(research);
	}
	if (want != nullptr && !status->hint_applied) {
		// Never an error (lift spec 5): an unknown, filtered or terminal hint falls through.
		E("conv run: --provider %s is not a usable candidate for %s (unknown, disabled, licence-gated or "
		  "terminal: `displayxr-cli conv candidates %s`); using the runtime's selection\n",
		  want, task_name, task_name);
	}
	hosted h;
	bool found = false;
	for (int i = 0; i < n && !found && chosen != nullptr; i++) {
		if (strcmp(entries[i].id, chosen) == 0) {
			found = host_one(&entries[i], task, h);
		}
	}
	if (!found) {
		if (chosen != nullptr) {
			E("conv run: the selected provider %s could not be hosted for %s on the job lane\n", chosen,
			  task_name);
		} else {
			E("conv run: no registered provider serves %s on the job lane\n", task_name);
		}
		free(entries);
		return 1;
	}
	const bool remote = (h.cap.flags & XRT_CONV_CAP_REMOTE) != 0;
	E("conv run: %s via %s (cap %u, %s)%s\n", task_name, h.e->id, h.cap.cap_id, h.cap.display_name,
	  remote ? " - REMOTE: the picture leaves this machine" : " - local: loads its own model instance here");

	std::signal(SIGINT, on_sigint);
	const uint64_t deadline = os_monotonic_get_ns() + (uint64_t)(timeout_s * 1e9);

	// Warm (blocking) and require READY: a non-terminal state is waited out.
	xrt_conv_state st{};
	if (h.iface->warm != nullptr) {
		(void)h.iface->warm(h.inst, h.cap.cap_id, is_cancelled, nullptr);
	}
	while (read_state(h, st) && st.state != XRT_CONV_STATE_READY && !g_cancel.load() &&
	       (st.state == XRT_CONV_STATE_WARMING || st.state == XRT_CONV_STATE_DOWNLOADING) &&
	       (uint64_t)os_monotonic_get_ns() < deadline) {
		os_nanosleep(100ll * 1000 * 1000);
	}
	if (st.state != XRT_CONV_STATE_READY) {
		E("conv run: %s is %s: %s\n", h.e->id, state_word(st.state), st.detail);
		release(h);
		free(entries);
		return st.state == XRT_CONV_STATE_DISABLED ? 3 : 1;
	}
	(void)recheck_cap(h); // licence, limits and formats are known once warm
	if ((h.cap.licence_class == XRT_CONV_LICENCE_RESEARCH_ONLY ||
	     h.cap.licence_class == XRT_CONV_LICENCE_UNKNOWN) &&
	    !allow_research) {
		E("conv run: %s's capability is %s (%s): it is selectable only with AllowResearchOnly "
		  "(Windows: HKCU\\Software\\DisplayXR\\ConversionProviders AllowResearchOnly = 1; "
		  "or --allow-research-only for this run)\n",
		  h.e->id, licence_word(h.cap.licence_class), h.cap.licence_spdx);
		release(h);
		free(entries);
		return 4;
	}
	E("conv run: licence %s (%s), model %s\n", licence_word(h.cap.licence_class), h.cap.licence_spdx,
	  h.cap.model_id);

	// Inputs (CPU RGBA8), downscaled to the capability's max input edge (the runtime's rule).
	downscale(img[0], h.cap.max_input_edge, sbs ? 2 : 1);
	if (stereo && !sbs) {
		downscale(img[1], h.cap.max_input_edge, 1);
	}
	xrt_conv_job_in in{};
	in.struct_size = sizeof(in);
	in.input_kind = stereo ? XRT_CONV_INPUT_STEREO_PAIR : XRT_CONV_INPUT_MONO;
	in.image_count = stereo && !sbs ? 2u : 1u;
	for (uint32_t i = 0; i < in.image_count; i++) {
		xrt_conv_image &im = in.images[i];
		im.memory_kind = XRT_CONV_MEM_CPU;
		im.cpu_ptr = img[i].rgba.data();
		im.byte_size = img[i].rgba.size();
		im.width = (uint32_t)img[i].w;
		im.height = (uint32_t)img[i].h;
		im.format = XRT_CONV_FMT_RGBA8_SRGB;
		im.color_space = XRT_CONV_CS_SRGB;
		im.row_stride = (uint32_t)img[i].w * 4u;
		in.image_role[i] = i == 0 ? XRT_CONV_IMG_COLOR : XRT_CONV_IMG_COLOR_RIGHT;
	}
	in.requested_blob_format = format;
	in.job_flags = (rectified ? XRT_CONV_JOB_PAIR_RECTIFIED : 0u) | (raw ? XRT_CONV_JOB_PAIR_RAW : 0u) |
	               (focal > 0.0f ? XRT_CONV_JOB_FOCAL_GIVEN : 0u);
	in.max_input_long_side = max_long;
	xrt_conv_params params{};
	params.struct_size = sizeof(params);
	params.content_hint = XRT_CONV_CONTENT_PHOTO;
	params.focal_px = focal;
	params.baseline_m = baseline;
	params.stereo.baseline_m = baseline; // 0 = unknown, never defaulted
	params.stereo.sbs_packed = sbs ? 1u : 0u;
	params.stereo.fx = params.stereo.fy = focal;

	file_sink fs;
	fs.tmp = std::string(out_path) + ".part";
	xrt_conv_blob_sink sink{};
	sink.struct_size = sizeof(sink);
	sink.role = XRT_CONV_OUT_SPLAT_BLOB;
	sink.ctx = &fs;
	sink.begin = sink_begin;
	sink.write = sink_write;
	xrt_conv_job_control ctl{};
	ctl.struct_size = sizeof(ctl);
	ctl.sink_count = 1;
	ctl.sinks = &sink;
	ctl.progress = on_progress;
	ctl.is_cancelled = is_cancelled;
	xrt_conv_job_out out{};
	out.struct_size = sizeof(out);
	xrt_conv_splat_result sr{};
	sr.struct_size = sizeof(sr);
	out.splat_result = &sr;

	const uint64_t t0 = os_monotonic_get_ns();
	const xrt_result_t r = h.iface->job_execute(h.inst, h.cap.cap_id, &in, &params, &ctl, &out);
	const double secs = (double)(os_monotonic_get_ns() - t0) / 1e9;
	if (fs.f != nullptr) {
		fclose(fs.f);
		fs.f = nullptr;
	}
	cJSON *facts = conv_splat_facts_to_json(&out.splat_info, &sr);
	int rc = 0;
	if (g_cancel.load()) {
		E("conv run: cancelled\n");
		rc = 130;
	} else if (r != XRT_SUCCESS || fs.failed || fs.written == 0) {
		E("conv run: the job failed (%d) after %.2f s%s%s\n", (int)r, secs,
		  sr.refusal_code != XRT_CONV_REFUSAL_NONE ? ": " : "", sr.refusal_reason);
		rc = 1;
	}
	if (rc != 0) {
		(void)remove(fs.tmp.c_str());
	} else {
		(void)remove(out_path);
		if (rename(fs.tmp.c_str(), out_path) != 0) {
			E("conv run: cannot write %s\n", out_path);
			rc = 1;
		} else {
			E("conv run: wrote %s (%llu bytes, %s) in %.2f s\n", out_path, (unsigned long long)fs.written,
			  fs.format == XRT_CONV_BLOB_PLY_3DGS ? "ply" : "sog", secs);
		}
	}
	if (json) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "provider", h.e->id);
		cJSON_AddNumberToObject(o, "cap_id", h.cap.cap_id);
		cJSON_AddBoolToObject(o, "remote", remote);
		cJSON_AddBoolToObject(o, "succeeded", rc == 0);
		cJSON_AddNumberToObject(o, "seconds", secs);
		cJSON_AddItemToObject(o, "facts", cJSON_Duplicate(facts, true));
		char *t = cJSON_Print(o);
		P("%s\n", t != nullptr ? t : "{}");
		free(t);
		cJSON_Delete(o);
	} else {
		print_facts_human(facts);
	}
	if (const char *fp = value_of(argc, argv, "--facts")) {
		char *t = cJSON_Print(facts);
		if (t == nullptr || !write_text(fp, t)) {
			E("conv run: cannot write %s\n", fp);
			rc = rc == 0 ? 1 : rc;
		}
		free(t);
	}
	if (rc == 0 && camera_block != nullptr) {
		char err[256] = {0};
		cJSON *block = conv_camera_block_from_facts(facts, err, sizeof(err));
		if (block == nullptr) {
			E("conv run: camera block not written: %s\n", err);
		} else {
			const conv_camera_block_result br = conv_camera_block_write(out_path, block, err, sizeof(err));
			E("conv run: camera block: %s%s%s\n", conv_camera_block_result_str(br),
			  err[0] != 0 ? " - " : "", err);
			if (br == CONV_CAMERA_BLOCK_ERROR) {
				rc = 1;
			}
			cJSON_Delete(block);
		}
	}
	cJSON_Delete(facts);
	release(h);
	free(entries);
	return rc;
}


/*
 *
 * conv consent.
 *
 */

namespace {

void
list_cb(void *ctx, const char *id)
{
	auto *v = (std::vector<std::string> *)ctx;
	v->push_back(id);
}

} // namespace

extern "C" int
cli_cmd_conv_consent(int argc, const char **argv)
{
	const std::vector<const char *> pos = positionals(argc, argv);
	const bool json = cli_has_flag(argc, argv, "--json");
	if (pos.size() == 1 && strcmp(pos[0], "status") == 0) {
		std::vector<std::string> ids;
		u_offbox_consent_list(list_cb, &ids);
		if (json) {
			cJSON *o = cJSON_CreateObject();
			cJSON *a = cJSON_AddArrayToObject(o, "offbox_granted");
			for (const std::string &s : ids) {
				cJSON_AddItemToArray(a, cJSON_CreateString(s.c_str()));
			}
			char *t = cJSON_Print(o);
			P("%s\n", t);
			free(t);
			cJSON_Delete(o);
		} else if (ids.empty()) {
			P("No conversion provider may send pictures off this machine.\n");
		} else {
			for (const std::string &s : ids) {
				P("%s: off-box consent GRANTED\n", s.c_str());
			}
		}
		return 0;
	}
	if (pos.size() != 2) {
		E("usage: displayxr-cli conv consent <provider-id> grant | revoke | status [--json]\n"
		  "       displayxr-cli conv consent status        (every grant)\n"
		  "Off-box consent: may the provider send your pictures to a remote endpoint? Per user, revocable.\n");
		return 2;
	}
	const char *id = pos[0];
	const char *verb = pos[1];
	if (strcmp(verb, "grant") == 0 || strcmp(verb, "revoke") == 0) {
		const bool grant = strcmp(verb, "grant") == 0;
		if (!u_offbox_consent_set(id, grant)) {
			E("conv consent: cannot %s consent for '%s' (an id is [A-Za-z0-9._-], up to 63 chars)\n", verb,
			  id);
			return 1;
		}
		P("%s: off-box consent %s. %s\n", id, grant ? "GRANTED" : "revoked",
		  grant ? "Pictures it converts are sent to its configured endpoints; revoke with "
		          "`conv consent <id> revoke`."
		        : "Its remote capabilities report DISABLED from the next state query.");
		return 0;
	}
	if (strcmp(verb, "status") == 0) {
		const bool g = u_offbox_consent_granted(id);
		if (json) {
			P("{\"provider\": \"%s\", \"offbox_consent\": %s}\n", id, g ? "true" : "false");
		} else {
			P("%s: off-box consent %s\n", id, g ? "GRANTED" : "not granted");
		}
		return 0;
	}
	E("conv consent: '%s' is not grant | revoke | status\n", verb);
	return 2;
}


/*
 *
 * conv camera-block.
 *
 */

extern "C" int
cli_cmd_conv_camera_block(int argc, const char **argv)
{
	const std::vector<const char *> pos = positionals(argc, argv);
	const char *facts_path = value_of(argc, argv, "--facts");
	if (pos.size() != 1 || facts_path == nullptr) {
		E("usage: displayxr-cli conv camera-block <file.sog | meta.json | dir> --facts <facts.json>\n"
		  "Writes facts + suggestions as the SOG `camera` block, \"source\": \"auto\"; never over an authored "
		  "block.\n");
		return 2;
	}
	FILE *f = fopen(facts_path, "rb");
	if (f == nullptr) {
		E("conv camera-block: cannot read %s\n", facts_path);
		return 1;
	}
	std::string text;
	char buf[4096];
	size_t got;
	while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
		text.append(buf, got);
	}
	fclose(f);
	cJSON *facts = cJSON_Parse(text.c_str());
	if (facts == nullptr) {
		E("conv camera-block: %s is not JSON\n", facts_path);
		return 1;
	}
	// Accept `conv run --json` output too: its facts live under "facts".
	const cJSON *inner = cJSON_GetObjectItemCaseSensitive(facts, "facts");
	char err[256] = {0};
	cJSON *block = conv_camera_block_from_facts(cJSON_IsObject(inner) ? inner : facts, err, sizeof(err));
	cJSON_Delete(facts);
	if (block == nullptr) {
		E("conv camera-block: %s\n", err);
		return 1;
	}
	const conv_camera_block_result r = conv_camera_block_write(pos[0], block, err, sizeof(err));
	cJSON_Delete(block);
	P("%s: camera block %s%s%s\n", pos[0], conv_camera_block_result_str(r), err[0] != 0 ? " - " : "", err);
	return r == CONV_CAMERA_BLOCK_ERROR ? 1 : 0;
}


/*
 *
 * conv credential.
 *
 */

extern "C" int
cli_cmd_conv_credential(int argc, const char **argv)
{
	const std::vector<const char *> pos = positionals(argc, argv);
	if (pos.size() != 2) {
		E("usage: displayxr-cli conv credential set <name>      (reads the secret from stdin, one line)\n"
		  "       displayxr-cli conv credential delete <name>\n"
		  "       displayxr-cli conv credential status <name>   (present or not; never prints it)\n"
		  "The OS credential store: Windows Credential Manager, macOS Keychain, Linux Secret Service.\n"
		  "A provider setting names the credential (e.g. conv set remote-conversion CREDENTIAL <name>).\n");
		return 2;
	}
	const char *verb = pos[0];
	const char *name = pos[1];
	char err[256] = {0};
	if (strcmp(verb, "set") == 0) {
		char secret[U_CREDENTIAL_SECRET_MAX + 2];
		if (fgets(secret, sizeof(secret), stdin) == nullptr) {
			E("conv credential: no secret on stdin\n");
			return 2;
		}
		size_t n = strlen(secret);
		while (n > 0 && (secret[n - 1] == '\n' || secret[n - 1] == '\r')) {
			secret[--n] = 0;
		}
		const u_credential_result r = u_credential_write(name, secret, err, sizeof(err));
		u_credential_wipe(secret, sizeof(secret));
		if (r != U_CREDENTIAL_OK) {
			E("conv credential: %s (%s)\n", err, u_credential_result_str(r));
			return 1;
		}
		P("credential '%s' stored in the OS credential store\n", name);
		return 0;
	}
	if (strcmp(verb, "delete") == 0) {
		const u_credential_result r = u_credential_delete(name, err, sizeof(err));
		if (r != U_CREDENTIAL_OK) {
			E("conv credential: %s (%s)\n", err, u_credential_result_str(r));
			return 1;
		}
		P("credential '%s' deleted\n", name);
		return 0;
	}
	if (strcmp(verb, "status") == 0) {
		char secret[U_CREDENTIAL_SECRET_MAX + 1];
		const u_credential_result r = u_credential_read(name, secret, sizeof(secret), err, sizeof(err));
		u_credential_wipe(secret, sizeof(secret));
		P("credential '%s': %s%s%s\n", name,
		  r == U_CREDENTIAL_OK ? "present and readable by this program" : u_credential_result_str(r),
		  r != U_CREDENTIAL_OK ? " - " : "", r != U_CREDENTIAL_OK ? err : "");
		return r == U_CREDENTIAL_OK ? 0 : 1;
	}
	E("conv credential: '%s' is not set | delete | status\n", verb);
	return 2;
}
