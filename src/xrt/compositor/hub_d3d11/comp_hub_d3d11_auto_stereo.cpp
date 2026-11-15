// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Legacy automatic 2D + automatic re-convergence on the D3D11 hub.
 * @ingroup comp_hub_d3d11
 *
 * See comp_hub_d3d11_auto_stereo.h for the shape; the policy is
 * util/u_stereo_auto.h.
 */

#include "xrt/xrt_config_have.h"
#include "xrt/xrt_config_os.h"

#if defined(XRT_OS_WINDOWS) && defined(XRT_HAVE_D3D11)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include "comp_hub_d3d11_auto_stereo.h"

#include "os/os_time.h"
#include "util/u_logging.h"
#include "util/u_stereo_auto.h"

#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

constexpr uint32_t STAGING_RING = 2;

DXGI_FORMAT
typeless_of(DXGI_FORMAT f)
{
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
	case DXGI_FORMAT_B8G8R8X8_TYPELESS:
	case DXGI_FORMAT_B8G8R8X8_UNORM:
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_TYPELESS;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
	case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
	case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
	default: return DXGI_FORMAT_UNKNOWN;
	}
}

//! The non-decoding view of a family (ADR-044: the hub never decodes on read).
DXGI_FORMAT
raw_view_of(DXGI_FORMAT typeless)
{
	switch (typeless) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	default: return DXGI_FORMAT_UNKNOWN;
	}
}

uint32_t
bytes_per_pixel(DXGI_FORMAT typeless)
{
	return typeless == DXGI_FORMAT_R16G16B16A16_TYPELESS ? 8u : 4u;
}

float
half_to_float(uint16_t h)
{
	const uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 0x1fu, m = h & 0x3ffu;
	float v;
	if (e == 0) {
		v = std::ldexp((float)m, -24);
	} else if (e == 31) {
		v = m != 0 ? 0.0f : 65504.0f;
	} else {
		v = std::ldexp((float)(m | 0x400u), (int)e - 25);
	}
	return s ? -v : v;
}

//! One pixel to display-encoded r, g, b in 0..1 (a linear FP16 atlas is encoded ~2.2).
void
decode(DXGI_FORMAT typeless, const uint8_t *p, float *r, float *g, float *b)
{
	switch (typeless) {
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8X8_TYPELESS:
		*b = p[0] / 255.0f;
		*g = p[1] / 255.0f;
		*r = p[2] / 255.0f;
		return;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS: {
		uint32_t v;
		memcpy(&v, p, 4);
		*r = (v & 0x3ffu) / 1023.0f;
		*g = ((v >> 10) & 0x3ffu) / 1023.0f;
		*b = ((v >> 20) & 0x3ffu) / 1023.0f;
		return;
	}
	case DXGI_FORMAT_R16G16B16A16_TYPELESS: {
		uint16_t c[3];
		memcpy(c, p, 6);
		float *o[3] = {r, g, b};
		for (int i = 0; i < 3; i++) {
			const float lin = half_to_float(c[i]);
			*o[i] = lin <= 0.0f ? 0.0f : (lin >= 1.0f ? 1.0f : std::pow(lin, 1.0f / 2.2f));
		}
		return;
	}
	default:
		*r = p[0] / 255.0f;
		*g = p[1] / 255.0f;
		*b = p[2] / 255.0f;
		return;
	}
}

template <typename T>
void
release(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

} // namespace

struct comp_hub_auto_stereo
{
	struct u_legacy_stereo_auto cfg;

	/*
	 * GPU, weave thread only.
	 */
	DXGI_FORMAT family = DXGI_FORMAT_UNKNOWN; //!< typeless family of the probed atlas
	bool broken = false;                      //!< an unsupported format or a failed create: idle for good
	ID3D11Texture2D *mip_tex = nullptr;
	ID3D11ShaderResourceView *mip_srv = nullptr;
	uint32_t mip_w = 0, mip_h = 0, mip_level = 0; //!< level-0 extent, the level read back
	uint32_t st_w = 0, st_h = 0;                  //!< extent of that level
	ID3D11Texture2D *staging[STAGING_RING] = {};
	bool pending[STAGING_RING] = {};
	uint32_t st_next = 0;
	uint64_t next_probe_ns = 0;

	ID3D11Texture2D *out_tex = nullptr;
	ID3D11ShaderResourceView *out_srv = nullptr;
	ID3D11RenderTargetView *out_rtv = nullptr;
	uint32_t out_w = 0, out_h = 0;
	DXGI_FORMAT out_family = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT out_view = DXGI_FORMAT_UNKNOWN;
	//! What out_tex holds (a repaint of the same frame reuses it).
	ID3D11ShaderResourceView *out_src = nullptr;
	uint64_t out_seq = 0;
	int32_t out_px = 0;
	bool out_flat = false;

	bool flat_committed = false;
	int32_t applied_px = 0;
	uint32_t cuts_seen = 0;
	bool eligible_logged = false;

	/*
	 * Worker.
	 */
	std::thread worker;
	std::mutex mtx;
	std::condition_variable cv;
	bool quit = false;
	bool job_ready = false;
	std::vector<uint8_t> job;
	uint32_t job_w = 0, job_h = 0;
	uint64_t job_ns = 0;
	DXGI_FORMAT job_family = DXGI_FORMAT_UNKNOWN;

	// Results (under mtx).
	bool res_want_2d = false;
	float res_shift = 0.0f;
	bool res_have_shift = false;
	uint32_t res_cuts = 0;
	struct comp_hub_auto_stereo_stats stats = {};

	// The controller's state, worker only (big: two grids per probe and the previous left grid).
	struct u_stereo_auto ctl;
};

static void
as_worker(struct comp_hub_auto_stereo *as)
{
	std::vector<uint8_t> bytes;
	std::vector<float> luma;
	std::vector<float> left(U_STEREO_AUTO_CELLS), right(U_STEREO_AUTO_CELLS);
	for (;;) {
		uint32_t w, h;
		uint64_t ns;
		DXGI_FORMAT fam;
		{
			std::unique_lock<std::mutex> lk(as->mtx);
			as->cv.wait(lk, [as] { return as->quit || as->job_ready; });
			if (as->quit) {
				return;
			}
			bytes.swap(as->job);
			w = as->job_w;
			h = as->job_h;
			ns = as->job_ns;
			fam = as->job_family;
			as->job_ready = false;
		}
		const uint32_t bpp = bytes_per_pixel(fam);
		if (w < 2 || h == 0 || bytes.size() < (size_t)w * h * bpp) {
			continue;
		}
		luma.resize((size_t)w * h);
		for (uint32_t y = 0; y < h; y++) {
			const uint8_t *row = bytes.data() + (size_t)y * w * bpp;
			for (uint32_t x = 0; x < w; x++) {
				float r, g, b;
				decode(fam, row + (size_t)x * bpp, &r, &g, &b);
				luma[(size_t)y * w + x] = u_stereo_auto_luma(r, g, b);
			}
		}
		const uint32_t eye_w = w / 2;
		u_stereo_auto_grid_from_luma(luma.data(), w, h, 0, eye_w, left.data());
		u_stereo_auto_grid_from_luma(luma.data(), w, h, eye_w, eye_w, right.data());
		struct u_stereo_auto_result res;
		u_stereo_auto_probe(&as->ctl, left.data(), right.data(), ns, &res);
		{
			std::lock_guard<std::mutex> lk(as->mtx);
			as->res_want_2d = res.want_2d;
			as->res_shift = res.shift;
			as->res_have_shift = as->ctl.have_shift;
			as->res_cuts = as->ctl.cuts;
			as->stats.unshifted = res.flat.unshifted;
			as->stats.best = res.flat.best;
			as->stats.best_shift_frac = res.flat.best_shift / (double)U_STEREO_AUTO_GRID_X;
			as->stats.shift = res.shift;
			as->stats.blocks = res.est.blocks;
			as->stats.probes = as->ctl.probes;
			as->stats.cuts = as->ctl.cuts;
		}
	}
}

struct comp_hub_auto_stereo *
comp_hub_auto_stereo_create(const struct u_legacy_stereo_auto *cfg)
{
	if (cfg == nullptr || (!cfg->auto_2d && !cfg->auto_convergence)) {
		return nullptr;
	}
	auto *as = new (std::nothrow) comp_hub_auto_stereo();
	if (as == nullptr) {
		return nullptr;
	}
	as->cfg = *cfg;
	u_stereo_auto_init(&as->ctl, cfg);
	try {
		as->worker = std::thread(as_worker, as);
	} catch (...) {
		delete as;
		return nullptr;
	}
	return as;
}

static void
as_release_gpu(struct comp_hub_auto_stereo *as)
{
	release(as->mip_srv);
	release(as->mip_tex);
	for (uint32_t i = 0; i < STAGING_RING; i++) {
		release(as->staging[i]);
		as->pending[i] = false;
	}
	release(as->out_rtv);
	release(as->out_srv);
	release(as->out_tex);
	as->out_src = nullptr;
	as->out_seq = 0;
}

void
comp_hub_auto_stereo_destroy(struct comp_hub_auto_stereo **as_ptr)
{
	if (as_ptr == nullptr || *as_ptr == nullptr) {
		return;
	}
	struct comp_hub_auto_stereo *as = *as_ptr;
	{
		std::lock_guard<std::mutex> lk(as->mtx);
		as->quit = true;
	}
	as->cv.notify_all();
	if (as->worker.joinable()) {
		as->worker.join();
	}
	as_release_gpu(as);
	delete as;
	*as_ptr = nullptr;
}

bool
comp_hub_auto_stereo_lens_edge(
    struct comp_hub_auto_stereo *as, bool session_3d, uint32_t cols, uint32_t rows, bool *out_flat)
{
	if (as == nullptr || out_flat == nullptr) {
		return false;
	}
	bool want = false;
	if (as->cfg.auto_2d && session_3d && cols == 2 && rows == 1 && !as->broken) {
		std::lock_guard<std::mutex> lk(as->mtx);
		want = as->res_want_2d;
	}
	if (want == as->flat_committed) {
		return false;
	}
	*out_flat = want;
	return true;
}

void
comp_hub_auto_stereo_lens_commit(struct comp_hub_auto_stereo *as, bool flat)
{
	if (as != nullptr) {
		as->flat_committed = flat;
	}
}

void
comp_hub_auto_stereo_get_stats(struct comp_hub_auto_stereo *as, struct comp_hub_auto_stereo_stats *out)
{
	if (as == nullptr || out == nullptr) {
		return;
	}
	std::lock_guard<std::mutex> lk(as->mtx);
	*out = as->stats;
}

//! Read back what is ready; never waits.
static void
as_poll(struct comp_hub_auto_stereo *as, ID3D11DeviceContext *ctx)
{
	for (uint32_t i = 0; i < STAGING_RING; i++) {
		if (!as->pending[i] || as->staging[i] == nullptr) {
			continue;
		}
		D3D11_MAPPED_SUBRESOURCE m = {};
		const HRESULT hr = ctx->Map(as->staging[i], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
		if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
			continue;
		}
		as->pending[i] = false;
		if (FAILED(hr)) {
			continue;
		}
		{
			std::lock_guard<std::mutex> lk(as->mtx);
			if (!as->job_ready) { // the worker still busy with the last one: drop this probe
				const uint32_t bpp = bytes_per_pixel(as->family);
				as->job.resize((size_t)as->st_w * as->st_h * bpp);
				for (uint32_t y = 0; y < as->st_h; y++) {
					memcpy(as->job.data() + (size_t)y * as->st_w * bpp,
					       static_cast<const uint8_t *>(m.pData) + (size_t)y * m.RowPitch,
					       (size_t)as->st_w * bpp);
				}
				as->job_w = as->st_w;
				as->job_h = as->st_h;
				as->job_ns = os_monotonic_get_ns();
				as->job_family = as->family;
				as->job_ready = true;
			}
		}
		ctx->Unmap(as->staging[i], 0);
		as->cv.notify_one();
	}
}

//! (Re)create the probe chain for a @p cw x @p ch content box of family @p fam.
static bool
as_ensure_probe(
    struct comp_hub_auto_stereo *as, ID3D11Device *dev, DXGI_FORMAT fam, uint32_t cw, uint32_t ch, uint32_t vw)
{
	if (as->mip_tex != nullptr && as->family == fam && as->mip_w == cw && as->mip_h == ch) {
		return true;
	}
	release(as->mip_srv);
	release(as->mip_tex);
	for (uint32_t i = 0; i < STAGING_RING; i++) {
		release(as->staging[i]);
		as->pending[i] = false;
	}
	const DXGI_FORMAT view = raw_view_of(fam);
	UINT support = 0;
	if (view == DXGI_FORMAT_UNKNOWN || FAILED(dev->CheckFormatSupport(view, &support)) ||
	    (support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN) == 0) {
		U_LOG_W("legacy auto-stereo: OFF for this session — the atlas format %d has no GPU mip support (#2639)",
		        (int)fam);
		as->broken = true;
		return false;
	}
	// The read-back level: each eye at least 256 px wide (the grid's columns)
	// and below 512, so the readback stays under ~2 MB at 10 Hz.
	uint32_t level = 0;
	while (level < 8 && (vw >> (level + 1)) >= U_STEREO_AUTO_GRID_X) {
		level++;
	}
	D3D11_TEXTURE2D_DESC d = {};
	d.Width = cw;
	d.Height = ch;
	d.MipLevels = level + 1;
	d.ArraySize = 1;
	d.Format = fam;
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
	D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
	sd.Format = view;
	sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	sd.Texture2D.MipLevels = level + 1;
	bool ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &as->mip_tex)) &&
	          SUCCEEDED(dev->CreateShaderResourceView(as->mip_tex, &sd, &as->mip_srv));
	const uint32_t sw = cw >> level > 0 ? cw >> level : 1;
	const uint32_t sh = ch >> level > 0 ? ch >> level : 1;
	for (uint32_t i = 0; ok && i < STAGING_RING; i++) {
		D3D11_TEXTURE2D_DESC st = {};
		st.Width = sw;
		st.Height = sh;
		st.MipLevels = 1;
		st.ArraySize = 1;
		st.Format = fam;
		st.SampleDesc.Count = 1;
		st.Usage = D3D11_USAGE_STAGING;
		st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ok = SUCCEEDED(dev->CreateTexture2D(&st, nullptr, &as->staging[i]));
	}
	if (!ok) {
		U_LOG_W("legacy auto-stereo: OFF for this session — the probe textures could not be created (#2639)");
		as_release_gpu(as);
		as->broken = true;
		return false;
	}
	as->family = fam;
	as->mip_w = cw;
	as->mip_h = ch;
	as->mip_level = level;
	as->st_w = sw;
	as->st_h = sh;
	return true;
}

static ID3D11Texture2D *
as_src_tex(ID3D11ShaderResourceView *srv, DXGI_FORMAT *out_fam)
{
	ID3D11Resource *res = nullptr;
	srv->GetResource(&res);
	if (res == nullptr) {
		return nullptr;
	}
	ID3D11Texture2D *tex = nullptr;
	if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&tex))) {
		tex = nullptr;
	}
	res->Release();
	if (tex != nullptr) {
		D3D11_TEXTURE2D_DESC d = {};
		tex->GetDesc(&d);
		*out_fam = d.SampleDesc.Count == 1 ? typeless_of(d.Format) : DXGI_FORMAT_UNKNOWN;
	}
	return tex;
}

static bool
as_ensure_out(
    struct comp_hub_auto_stereo *as, ID3D11Device *dev, DXGI_FORMAT fam, DXGI_FORMAT view, uint32_t w, uint32_t h)
{
	if (as->out_tex != nullptr && as->out_family == fam && as->out_view == view && as->out_w == w &&
	    as->out_h == h) {
		return true;
	}
	release(as->out_rtv);
	release(as->out_srv);
	release(as->out_tex);
	as->out_src = nullptr;
	as->out_seq = 0;
	D3D11_TEXTURE2D_DESC d = {};
	d.Width = w;
	d.Height = h;
	d.MipLevels = 1;
	d.ArraySize = 1;
	d.Format = fam;
	d.SampleDesc.Count = 1;
	d.Usage = D3D11_USAGE_DEFAULT;
	d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
	sd.Format = view;
	sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	sd.Texture2D.MipLevels = 1;
	D3D11_RENDER_TARGET_VIEW_DESC rd = {};
	rd.Format = raw_view_of(fam);
	rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	if (FAILED(dev->CreateTexture2D(&d, nullptr, &as->out_tex)) ||
	    FAILED(dev->CreateShaderResourceView(as->out_tex, &sd, &as->out_srv)) ||
	    FAILED(dev->CreateRenderTargetView(as->out_tex, &rd, &as->out_rtv))) {
		release(as->out_rtv);
		release(as->out_srv);
		release(as->out_tex);
		return false;
	}
	as->out_family = fam;
	as->out_view = view;
	as->out_w = w;
	as->out_h = h;
	return true;
}

bool
comp_hub_auto_stereo_frame(struct comp_hub_auto_stereo *as,
                           ID3D11Device *dev,
                           ID3D11DeviceContext *ctx,
                           bool is_repaint,
                           uint64_t seq,
                           bool session_3d,
                           struct comp_hub_auto_stereo_view *io)
{
	if (as == nullptr || dev == nullptr || ctx == nullptr || io == nullptr || io->srv == nullptr || as->broken) {
		return false;
	}
	const bool stereo = io->cols == 2 && io->rows == 1 && io->view_w >= 16 && io->view_h >= 16;
	if (!session_3d || !stereo) {
		as->applied_px = 0;
		return false;
	}
	const uint32_t vw = io->view_w, vh = io->view_h;
	const uint32_t cw = 2 * vw;

	as_poll(as, ctx);

	DXGI_FORMAT fam = DXGI_FORMAT_UNKNOWN;
	ID3D11Texture2D *src = as_src_tex(io->srv, &fam);
	if (src == nullptr) {
		return false;
	}
	if (fam == DXGI_FORMAT_UNKNOWN) {
		U_LOG_W("legacy auto-stereo: OFF for this session — unsupported atlas format (#2639)");
		as->broken = true;
		src->Release();
		return false;
	}
	if (!as->eligible_logged) {
		as->eligible_logged = true;
		U_LOG_W(
		    "legacy auto-stereo: probing the %ux%u stereo atlas (%ux%u per eye) at 10 Hz on a GPU mip with an "
		    "asynchronous readback (#2639 / #2640)",
		    cw, vh, vw, vh);
	}

	// The probe: app frames only, at most every U_STEREO_AUTO_PROBE_PERIOD_NS,
	// into a staging slot nobody is reading.
	const uint64_t now = os_monotonic_get_ns();
	if (!is_repaint && now >= as->next_probe_ns && as_ensure_probe(as, dev, fam, cw, vh, vw)) {
		const uint32_t i = as->st_next;
		if (!as->pending[i]) {
			D3D11_BOX box = {0, 0, 0, cw, vh, 1};
			ctx->CopySubresourceRegion(as->mip_tex, 0, 0, 0, 0, src, 0, &box);
			ctx->GenerateMips(as->mip_srv);
			ctx->CopySubresourceRegion(as->staging[i], 0, 0, 0, 0, as->mip_tex, as->mip_level, nullptr);
			as->pending[i] = true;
			as->st_next = (i + 1) % STAGING_RING;
			as->next_probe_ns = now + U_STEREO_AUTO_PROBE_PERIOD_NS;
		}
	}
	if (as->broken) {
		src->Release();
		return false;
	}

	// The re-convergence shift in view pixels: snap on a cut, otherwise a few
	// px per app frame at most (the worker's follower already smooths at 10 Hz).
	float shift = 0.0f;
	bool have = false;
	uint32_t cuts = 0;
	{
		std::lock_guard<std::mutex> lk(as->mtx);
		shift = as->res_shift;
		have = as->res_have_shift;
		cuts = as->res_cuts;
	}
	if (as->cfg.auto_convergence && have) {
		const int32_t target = (int32_t)std::lround((double)shift * (double)vw);
		if (cuts != as->cuts_seen) {
			as->cuts_seen = cuts;
			as->applied_px = target;
		} else if (!is_repaint && target != as->applied_px) {
			const int32_t diff = target - as->applied_px;
			int32_t step = std::abs(diff) / 4;
			step = step < 1 ? 1 : (step > 8 ? 8 : step);
			as->applied_px += diff > 0 ? (diff < step ? diff : step) : (-diff < step ? diff : -step);
		}
	} else {
		as->applied_px = 0;
	}

	const bool flat = as->flat_committed;
	const int32_t px = flat ? 0 : as->applied_px;
	if (!flat && px == 0) {
		src->Release();
		return false;
	}
	const uint32_t ow = flat ? vw : cw;
	D3D11_SHADER_RESOURCE_VIEW_DESC in_sd = {};
	io->srv->GetDesc(&in_sd);
	if (!as_ensure_out(as, dev, fam, in_sd.Format, ow, vh)) {
		src->Release();
		return false;
	}
	const bool cached =
	    seq != 0 && as->out_src == io->srv && as->out_seq == seq && as->out_px == px && as->out_flat == flat;
	if (!cached) {
		if (flat) {
			D3D11_BOX box = {0, 0, 0, vw, vh, 1};
			ctx->CopySubresourceRegion(as->out_tex, 0, 0, 0, 0, src, 0, &box);
		} else {
			const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
			ctx->ClearRenderTargetView(as->out_rtv, black);
			// d -> d + px: the right eye moves right by a, the left eye left by px - a.
			const int32_t a = px >= 0 ? (px + 1) / 2 : -((-px + 1) / 2);
			const int32_t off[2] = {-(px - a), a};
			for (uint32_t e = 0; e < 2; e++) {
				const int32_t o = off[e];
				const uint32_t mag = (uint32_t)(o < 0 ? -o : o);
				if (mag >= vw) {
					continue;
				}
				const uint32_t tx = e * vw;
				D3D11_BOX box = {o >= 0 ? tx : tx + mag, 0, 0, o >= 0 ? tx + vw - mag : tx + vw, vh, 1};
				ctx->CopySubresourceRegion(as->out_tex, 0, o >= 0 ? tx + mag : tx, 0, 0, src, 0, &box);
			}
		}
		as->out_src = io->srv;
		as->out_seq = seq;
		as->out_px = px;
		as->out_flat = flat;
	}
	src->Release();
	io->srv = as->out_srv;
	if (flat) {
		io->cols = 1;
		io->rows = 1;
	}
	return true;
}

#else // !(XRT_OS_WINDOWS && XRT_HAVE_D3D11)

typedef int comp_hub_auto_stereo_unused;

#endif
