// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Capability ranking over DXGI adapters, the one place that answers
 *         "which adapter should render?" (ADR-037 §2).
 * @ingroup aux_d3d
 *
 * Render-side sibling of d3d_scanout_helpers. ADR-037 §1 splits placement in
 * two — *render and composite go on the most capable available render adapter;
 * weave and present go on the scanout adapter* — and until this file existed
 * only the second half had a single implementation. The first half was
 * `EnumAdapterByGpuPreference(0, HIGH_PERFORMANCE)`, open-coded at three sites.
 *
 * DXGI's "high performance" is not our policy and cannot express our ranking:
 * it is a driver/OS-defined ordering that a per-app `UserGpuPreferences`
 * registry entry silently reorders (observed on hardware, see the note on
 * `DXR_D3D_FORCE_GPU` classification below). It happens to agree with §2 on the
 * reference box; that agreement is asserted in the log, not assumed.
 */

#include "d3d_render_adapter.hpp"
#include "d3d_render_adapter.h"

#include "d3d_scanout_helpers.hpp"

#include "xrt/xrt_config_have.h"

#include "util/u_logging.h"
#include "util/u_setting.h"

#include <dxgi1_6.h>
#include <wil/com.h>

#ifdef XRT_HAVE_D3D11
#include <d3d11.h>
#endif

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <vector>

namespace xrt::auxiliary::d3d {

namespace {

	/*
	 * Provenance vocabulary. Short, stable, and static — call sites log these
	 * verbatim and #1023 established that a placement decision without its
	 * reason is a log that cannot be triaged.
	 */
	constexpr const char *kProvOnlyCandidate = "only candidate";
	constexpr const char *kProvMostVram = "most VRAM";
	constexpr const char *kProvAdapterType = "adapter type";
	constexpr const char *kProvIndexTiebreak = "lowest index (tie)";
	constexpr const char *kProvUnresolved = "unresolved";
	/*
	 * #1252: the override can now come from the per-user store the Control
	 * Panel writes or from the machine default, not only from the environment.
	 * This string lands in `displayxr-cli info`'s `service ingest:` line — the
	 * one people paste into bug reports — so it names the ACTUAL source. A
	 * hardcoded "env-forced" would send a reader hunting for an environment
	 * variable that does not exist.
	 *
	 * A static table rather than a formatted buffer: RenderAdapterChoice
	 * carries a `const char *` that outlives the call and is read on other
	 * threads, so these need process lifetime. The source labels match the ones
	 * `displayxr-cli perf list` prints, so the two surfaces agree.
	 */
	enum ForcedKeyword
	{
		kFwScanout = 0,
		kFwIgpu,
		kFwDgpu,
		kFwIndex,
	};

	const char *
	prov_forced(u_setting_source src, ForcedKeyword kw)
	{
		static const char *const table[3][4] = {
		    {"env-forced: scanout", "env-forced: igpu", "env-forced: dgpu", "env-forced: index"},
		    {"user-forced: scanout", "user-forced: igpu", "user-forced: dgpu", "user-forced: index"},
		    {"machine-forced: scanout", "machine-forced: igpu", "machine-forced: dgpu",
		     "machine-forced: index"},
		};
		const int row = src == U_SETTING_SOURCE_USER ? 1 : (src == U_SETTING_SOURCE_MACHINE ? 2 : 0);
		return table[row][(int)kw];
	}

	/*!
	 * Dedicated-VRAM watermark separating "discrete" from "integrated" for the
	 * *kind* tiebreak. DXGI reports no adapter kind, and integrated parts report
	 * either 0 or a small carve-out (128 MB is common on Intel). This never
	 * excludes anything — it only orders adapters whose dedicated VRAM ties.
	 */
	constexpr uint64_t kDiscreteVramThresholdBytes = 512ull * 1024ull * 1024ull;

	//! Microsoft Basic Render Driver, which does not always set the SOFTWARE flag.
	constexpr UINT kMicrosoftVendorId = 0x1414;
	constexpr UINT kBasicRenderDriverDeviceId = 0x008c;

	enum class AdapterKind
	{
		Software = 0,
		Integrated = 1,
		Discrete = 2,
	};

	AdapterKind
	classify(const DXGI_ADAPTER_DESC1 &desc)
	{
		if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
			return AdapterKind::Software;
		}
		if (desc.VendorId == kMicrosoftVendorId && desc.DeviceId == kBasicRenderDriverDeviceId) {
			return AdapterKind::Software;
		}
		return desc.DedicatedVideoMemory >= kDiscreteVramThresholdBytes ? AdapterKind::Discrete
		                                                                : AdapterKind::Integrated;
	}

	/*!
	 * ADR-037 §2 excludes "adapters that ... cannot present". Read literally
	 * against DXGI that would mean "enumerates no outputs", which is **wrong**
	 * here: on an Optimus box the render-only discrete GPU enumerates zero
	 * outputs and presents fine through the OS's hybrid present — it is exactly
	 * the adapter the rule wants to win. So the honest reading is "has no local
	 * presentation capability at all": software rasterizers and remote adapters.
	 */
	bool
	can_present(const DXGI_ADAPTER_DESC1 &desc, AdapterKind kind)
	{
		if (kind == AdapterKind::Software) {
			return false;
		}
		return (desc.Flags & DXGI_ADAPTER_FLAG_REMOTE) == 0;
	}

	/*!
	 * Does this adapter reach @p min_feature_level? Passing a null `ppDevice` to
	 * `D3D11CreateDevice` is the documented capability query — it probes the
	 * driver without handing back a device (and returns `S_FALSE`, hence
	 * `SUCCEEDED`, not `== S_OK`).
	 *
	 * **Memoized by (LUID, level).** This is the only expensive step in the whole
	 * resolver: it loads the adapter's D3D11 user-mode driver, which on a hybrid
	 * box also wakes the discrete GPU. Both wired call sites can fire in the same
	 * process and each may run per session, so the probe is answered once per
	 * adapter per process and everything else stays a microsecond-scale DXGI
	 * walk. Adapter capability does not change under a live process; if the
	 * adapter disappears entirely, enumeration drops it before we get here.
	 *
	 * When D3D11 is not compiled in there is nothing cheap to probe with, so the
	 * check is skipped rather than guessed at; the ranking then rests on the
	 * exclusions above, which is the pre-ADR-037 status quo.
	 */
	bool
	meets_feature_level(IDXGIAdapter *adapter,
	                    const DXGI_ADAPTER_DESC1 &desc,
	                    D3D_FEATURE_LEVEL min_feature_level,
	                    u_logging_level log_level)
	{
#ifdef XRT_HAVE_D3D11
		struct Probe
		{
			LUID luid;
			D3D_FEATURE_LEVEL level;
			bool ok;
		};
		static std::vector<Probe> probed;

		for (const Probe &p : probed) {
			if (p.luid.HighPart == desc.AdapterLuid.HighPart &&
			    p.luid.LowPart == desc.AdapterLuid.LowPart && p.level == min_feature_level) {
				return p.ok;
			}
		}

		D3D_FEATURE_LEVEL wanted[] = {min_feature_level};
		D3D_FEATURE_LEVEL obtained = static_cast<D3D_FEATURE_LEVEL>(0);
		HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, wanted, 1,
		                               D3D11_SDK_VERSION, nullptr, &obtained, nullptr);
		bool ok = SUCCEEDED(hr);
		if (!ok) {
			U_LOG_IFL_I(log_level, "render adapter: feature-level probe failed (hr 0x%08lx) — excluded.",
			            (unsigned long)hr);
		}
		probed.push_back(Probe{desc.AdapterLuid, min_feature_level, ok});
		return ok;
#else
		(void)adapter;
		(void)desc;
		(void)min_feature_level;
		(void)log_level;
		return true;
#endif
	}

	struct Candidate
	{
		wil::com_ptr<IDXGIAdapter> adapter;
		DXGI_ADAPTER_DESC1 desc;
		UINT index;
		AdapterKind kind;
	};

	/*!
	 * Strict "is @p a a better render adapter than @p b", i.e. the ADR-037 §2
	 * ranking. Keys in order: dedicated VRAM (descending), adapter kind
	 * (discrete > integrated > software), enumeration index (ascending).
	 *
	 * The index key is what makes the ranking *deterministic* rather than
	 * merely correct: two adapters that tie on both capability keys must not
	 * resolve differently between runs.
	 */
	bool
	is_better(const Candidate &a, const Candidate &b)
	{
		if (a.desc.DedicatedVideoMemory != b.desc.DedicatedVideoMemory) {
			return a.desc.DedicatedVideoMemory > b.desc.DedicatedVideoMemory;
		}
		if (a.kind != b.kind) {
			return static_cast<int>(a.kind) > static_cast<int>(b.kind);
		}
		return a.index < b.index;
	}

	//! Which key separated the winner from the runner-up? That is the provenance.
	const char *
	provenance_for(const Candidate &winner, const Candidate &runner_up)
	{
		if (winner.desc.DedicatedVideoMemory != runner_up.desc.DedicatedVideoMemory) {
			return kProvMostVram;
		}
		if (winner.kind != runner_up.kind) {
			return kProvAdapterType;
		}
		return kProvIndexTiebreak;
	}

	std::vector<Candidate>
	gather_candidates(IDXGIFactory1 *factory, D3D_FEATURE_LEVEL min_feature_level, u_logging_level log_level)
	{
		std::vector<Candidate> candidates;

		for (UINT i = 0;; i++) {
			wil::com_ptr<IDXGIAdapter1> adapter1;
			if (FAILED(factory->EnumAdapters1(i, adapter1.put())) || adapter1 == nullptr) {
				break;
			}
			DXGI_ADAPTER_DESC1 desc{};
			if (FAILED(adapter1->GetDesc1(&desc))) {
				continue;
			}

			AdapterKind kind = classify(desc);
			if (!can_present(desc, kind)) {
				U_LOG_IFL_I(log_level, "render adapter: skipping '%ls' (software or remote).",
				            desc.Description);
				continue;
			}

			wil::com_ptr<IDXGIAdapter> adapter = adapter1.query<IDXGIAdapter>();
			if (!meets_feature_level(adapter.get(), desc, min_feature_level, log_level)) {
				U_LOG_IFL_I(log_level,
				            "render adapter: skipping '%ls' (below the required feature level).",
				            desc.Description);
				continue;
			}

			U_LOG_IFL_I(log_level, "render adapter: candidate[%u] '%ls', dedicated VRAM %llu MB, kind %d.",
			            i, desc.Description,
			            (unsigned long long)(desc.DedicatedVideoMemory / (1024 * 1024)),
			            static_cast<int>(kind));

			candidates.push_back(Candidate{adapter, desc, i, kind});
		}

		return candidates;
	}

	//! `DXR_D3D_FORCE_GPU=igpu|dgpu`: the extreme of the dedicated-VRAM ordering.
	RenderAdapterChoice
	pick_by_vram_extreme(const std::vector<Candidate> &candidates, bool want_igpu, const char *provenance)
	{
		const Candidate *best = nullptr;
		for (const Candidate &c : candidates) {
			if (best == nullptr) {
				best = &c;
				continue;
			}
			bool better = want_igpu ? c.desc.DedicatedVideoMemory < best->desc.DedicatedVideoMemory
			                        : c.desc.DedicatedVideoMemory > best->desc.DedicatedVideoMemory;
			if (better) {
				best = &c;
			}
		}
		if (best == nullptr) {
			return RenderAdapterChoice{nullptr, LUID{}, kProvUnresolved, true};
		}
		U_LOG_W("%s: adapter '%ls' (dedicated VRAM %llu MB)", provenance, best->desc.Description,
		        (unsigned long long)(best->desc.DedicatedVideoMemory / (1024 * 1024)));
		return RenderAdapterChoice{best->adapter, best->desc.AdapterLuid, provenance, true};
	}

	/*!
	 * The `DXR_D3D_FORCE_GPU` override channel, resolved *inside* the policy unit
	 * rather than alongside it (ADR-037 §4: overrides remain, and they remain
	 * overrides). Returns a choice with a null adapter when the variable is
	 * unset, unrecognised, or names something not present — in every one of those
	 * cases the caller falls through to the ranking, so a copied-around env var
	 * can never brick an app.
	 *
	 * SUPPORTED CONTRACT (#845): clients (e.g. the Unity plugin's Target GPU
	 * setting) build on this name and these semantics — do not rename, drop, or
	 * change classification without the deprecation path in
	 * docs/reference/adapter-selection.md. The getenv() read means in-process
	 * clients must set it via the CRT (_putenv_s), not SetEnvironmentVariableW.
	 */
	RenderAdapterChoice
	env_forced_adapter(const std::vector<Candidate> &candidates,
	                   int32_t panel_screen_left,
	                   int32_t panel_screen_top,
	                   uint32_t panel_pixel_width,
	                   uint32_t panel_pixel_height,
	                   u_logging_level log_level)
	{
		RenderAdapterChoice none{nullptr, LUID{}, kProvUnresolved, false};

		// #1252: resolved through the settings chain (env > per-user file >
		// machine) so the Control Panel's Target GPU control reaches apps it
		// never launched. The environment still wins, so the documented
		// in-process `_putenv_s` route (#845, displayxr-unity#243) and every
		// launcher script keep working exactly as before.
		char gpu_buf[64];
		u_setting_source forced_src = U_SETTING_SOURCE_DEFAULT;
		const char *val = u_setting_get_raw("DXR_D3D_FORCE_GPU", gpu_buf, sizeof(gpu_buf), &forced_src);
		if (val == nullptr || val[0] == '\0') {
			return none;
		}

		/*
		 * "scanout" (#918 Phase 0, closes the #846 gap) resolves dynamically to
		 * the adapter that owns the output the 3D panel is scanned out on — the
		 * iGPU on a box whose panel hangs off the integrated display
		 * controller, the dGPU on a MUX'd laptop or a panel wired to the
		 * discrete card. It is NOT an alias for "igpu": the rule is "weave next
		 * to scanout", and which silicon that is is a property of the machine,
		 * not of the keyword. Delegated to the scanout helper so there is still
		 * exactly one QueryDisplayConfig implementation.
		 */
		if (strcmp(val, "scanout") == 0) {
			if (panel_pixel_width == 0 || panel_pixel_height == 0) {
				U_LOG_W("DXR_D3D_FORCE_GPU=scanout: no display info available — ignoring");
				return none;
			}
			auto adapter = getScanoutAdapter(panel_screen_left, panel_screen_top, panel_pixel_width,
			                                 panel_pixel_height, log_level);
			if (adapter == nullptr) {
				U_LOG_W(
				    "DXR_D3D_FORCE_GPU=scanout: could not resolve the panel's scanout adapter "
				    "(panel %ux%u at %d,%d) — ignoring",
				    panel_pixel_width, panel_pixel_height, (int)panel_screen_left,
				    (int)panel_screen_top);
				return none;
			}
			DXGI_ADAPTER_DESC sdesc{};
			if (FAILED(adapter->GetDesc(&sdesc))) {
				U_LOG_W("DXR_D3D_FORCE_GPU=scanout: the scanout adapter has no description — ignoring");
				return none;
			}
			U_LOG_W("%s: adapter '%ls' (scans out the panel %ux%u at %d,%d)",
			        prov_forced(forced_src, kFwScanout), sdesc.Description, panel_pixel_width,
			        panel_pixel_height, (int)panel_screen_left, (int)panel_screen_top);
			return RenderAdapterChoice{adapter, sdesc.AdapterLuid, prov_forced(forced_src, kFwScanout),
			                           true};
		}

		if (candidates.empty()) {
			U_LOG_W("DXR_D3D_FORCE_GPU=%s: no usable adapter found — ignoring", val);
			return none;
		}

		if (strcmp(val, "igpu") == 0 || strcmp(val, "integrated") == 0) {
			// NOT EnumAdapterByGpuPreference: a per-app UserGpuPreferences
			// registry entry overrides the preference ARGUMENT, so with
			// GpuPreference=2 set MINIMUM_POWER still returns the discrete
			// GPU first (observed on HW). Classify by dedicated VRAM
			// instead — the integrated GPU is the hardware adapter with the
			// least of it — which no registry state can reorder.
			return pick_by_vram_extreme(candidates, /* want_igpu */ true, prov_forced(forced_src, kFwIgpu));
		}
		if (strcmp(val, "dgpu") == 0 || strcmp(val, "discrete") == 0) {
			return pick_by_vram_extreme(candidates, /* want_igpu */ false,
			                            prov_forced(forced_src, kFwDgpu));
		}
		if (val[0] >= '0' && val[0] <= '9') {
			UINT want = (UINT)atoi(val);
			for (const Candidate &c : candidates) {
				if (c.index == want) {
					U_LOG_W("%s: adapter[%u] '%ls'", prov_forced(forced_src, kFwIndex), c.index,
					        c.desc.Description);
					return RenderAdapterChoice{c.adapter, c.desc.AdapterLuid,
					                           prov_forced(forced_src, kFwIndex), true};
				}
			}
			U_LOG_W("DXR_D3D_FORCE_GPU=%s: no usable adapter at that index — ignoring", val);
			return none;
		}

		U_LOG_W("DXR_D3D_FORCE_GPU=%s: unrecognized value — ignoring", val);
		return none;
	}

	/*!
	 * One-shot assertion that §2's ranking still agrees with the DXGI preference
	 * the three call sites used to hardcode. The task of this unit is to replace
	 * that preference *without* changing what the reference box picks, so the
	 * equivalence is written to the log rather than assumed — a future box where
	 * the two diverge then says so instead of silently changing placement.
	 */
	void
	log_choice_once(const Candidate &winner, const char *provenance)
	{
		static bool logged = false;
		if (logged) {
			return;
		}
		logged = true;

		wil::com_ptr<IDXGIFactory6> factory6;
		DXGI_ADAPTER_DESC hp_desc{};
		bool have_hp = false;
		if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory6), factory6.put_void())) &&
		    factory6 != nullptr) {
			wil::com_ptr<IDXGIAdapter> hp;
			if (SUCCEEDED(factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
			                                                   __uuidof(IDXGIAdapter), hp.put_void())) &&
			    hp != nullptr) {
				have_hp = SUCCEEDED(hp->GetDesc(&hp_desc));
			}
		}

		bool same = have_hp && hp_desc.AdapterLuid.HighPart == winner.desc.AdapterLuid.HighPart &&
		            hp_desc.AdapterLuid.LowPart == winner.desc.AdapterLuid.LowPart;

		U_LOG_W(
		    "ADR-037 render adapter: '%ls' LUID=%08lx:%08lx (%s); DXGI HIGH_PERFORMANCE picks '%ls' — %s "
		    "(#918)",
		    winner.desc.Description, (unsigned long)winner.desc.AdapterLuid.HighPart,
		    (unsigned long)winner.desc.AdapterLuid.LowPart, provenance,
		    have_hp ? hp_desc.Description : L"<unavailable>",
		    same ? "MATCH" : (have_hp ? "DIVERGES" : "unverified"));
	}

} // namespace

RenderAdapterChoice
getRenderAdapter(int32_t panel_screen_left,
                 int32_t panel_screen_top,
                 uint32_t panel_pixel_width,
                 uint32_t panel_pixel_height,
                 D3D_FEATURE_LEVEL min_feature_level,
                 u_logging_level log_level)
{
	RenderAdapterChoice unresolved{nullptr, LUID{}, kProvUnresolved, false};

	wil::com_ptr<IDXGIFactory1> factory;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void())) || factory == nullptr) {
		U_LOG_IFL_I(log_level, "render adapter: DXGI factory unavailable.");
		return unresolved;
	}

	std::vector<Candidate> candidates = gather_candidates(factory.get(), min_feature_level, log_level);

	// Overrides first: ADR-037 §4 keeps them, and keeping them *inside* the
	// resolver is what stops each call site from growing its own copy.
	RenderAdapterChoice forced = env_forced_adapter(candidates, panel_screen_left, panel_screen_top,
	                                                panel_pixel_width, panel_pixel_height, log_level);
	if (forced.adapter != nullptr) {
		return forced;
	}

	if (candidates.empty()) {
		U_LOG_IFL_I(log_level, "render adapter: no adapter survived the exclusions.");
		return unresolved;
	}

	// Sorting rather than a single min-scan so the runner-up is available: the
	// provenance is *which key separated the top two*, which a scan discards.
	std::stable_sort(candidates.begin(), candidates.end(), is_better);

	const Candidate &winner = candidates.front();
	const char *provenance = candidates.size() == 1 ? kProvOnlyCandidate : provenance_for(winner, candidates[1]);

	log_choice_once(winner, provenance);

	return RenderAdapterChoice{winner.adapter, winner.desc.AdapterLuid, provenance, false};
}

} // namespace xrt::auxiliary::d3d


extern "C" bool
d3d_render_adapter_luid(int32_t panel_screen_left,
                        int32_t panel_screen_top,
                        uint32_t panel_pixel_width,
                        uint32_t panel_pixel_height,
                        uint64_t *out_packed_luid,
                        const char **out_provenance)
{
	if (out_packed_luid == nullptr) {
		return false;
	}

	xrt::auxiliary::d3d::RenderAdapterChoice choice =
	    xrt::auxiliary::d3d::getRenderAdapter(panel_screen_left, panel_screen_top, panel_pixel_width,
	                                          panel_pixel_height, D3D_FEATURE_LEVEL_11_0, U_LOGGING_INFO);
	if (choice.adapter == nullptr) {
		return false;
	}

	// Pack exactly as Vulkan hands back VkPhysicalDeviceIDProperties::deviceLUID:
	// the raw bytes of the Windows LUID.
	uint64_t packed = 0;
	static_assert(sizeof(choice.luid) == sizeof(packed), "LUID size mismatch");
	memcpy(&packed, &choice.luid, sizeof(packed));
	*out_packed_luid = packed;

	if (out_provenance != nullptr) {
		*out_provenance = choice.provenance;
	}

	return true;
}
