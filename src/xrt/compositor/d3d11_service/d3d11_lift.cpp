// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_lift (ADR-042) on the D3D11 service — see d3d11_lift.h. The
 *         service-device glue is in d3d11_conv_gpu.cpp (ADR-053 P1b, #2028).
 *
 * Deliberately raw COM (no WIL) so the file is portable to the MinGW
 * compile-check (scripts/build-mingw-check.sh cannot build WIL).
 *
 * @ingroup comp_d3d11_service
 */

#include "d3d11_lift_internal.h"


static void
caps_set_state(d3d11_lift *l, uint32_t state, const char *why)
{
	// Caller holds mtx. WARN once per state CHANGE, never per poll.
	l->caps.state = state;
	l->change_count++;
	if (state == XRT_DP_LIFT_STATE_UNAVAILABLE && why != nullptr) {
		snprintf(l->unavail_detail, sizeof(l->unavail_detail), "%s", why);
	}
	if (l->logged_state != state) {
		l->logged_state = state;
		U_LOG_W("[lift] module state -> %s (modes=0x%x backend='%s' max_streams=%u max_views=%u)%s%s",
		        state_str(state), l->caps.modes, l->caps.backend, l->caps.max_streams, l->caps.max_views,
		        why != nullptr ? " — " : "", why != nullptr ? why : "");
	}
	if (state != XRT_DP_LIFT_STATE_ACTIVATING) {
		l->activating_since_ns = 0;
		l->activating_warned = false;
	} else if (l->activating_since_ns == 0 && l->activate_requested) {
		l->activating_since_ns = os_monotonic_get_ns();
	}
}

//! How long the module may stay ACTIVATING after first being asked for before
//! the one-shot stall WARN (#1812). A healthy module warms up in seconds.
static const uint64_t k_activating_warn_ns = 30ull * 1000 * 1000 * 1000;

/*!
 * One-shot WARN when the module has been ACTIVATING for longer than
 * k_activating_warn_ns since someone first asked for it (#1812: a vendor
 * conversion runtime whose files were half-removed under the running service).
 * Callers keep getting the ACTIVATING snapshot without waiting — this only
 * makes the stall visible. Caller holds mtx; called from every backend
 * snapshot (the adapter's warm polls it), so it fires even while the lift
 * thread itself is blocked inside a vendor call.
 */
static void
lift_check_activating_stall(d3d11_lift *l, uint64_t now)
{
	if (l->caps.state != XRT_DP_LIFT_STATE_ACTIVATING || !l->activate_requested) {
		return;
	}
	if (l->activating_since_ns == 0) {
		l->activating_since_ns = now;
		return;
	}
	if (l->activating_warned || now - l->activating_since_ns < k_activating_warn_ns) {
		return;
	}
	l->activating_warned = true;
	if (l->vendor_call != nullptr) {
		U_LOG_W(
		    "[lift] module still ACTIVATING after %.0f s; the lift thread is blocked in the plug-in's %s "
		    "(for %.0f s). The dp-lift row stays WARMING, streams wait. Usual cause: a broken or "
		    "half-removed vendor conversion runtime install. Repair it, then restart the service.",
		    (double)(now - l->activating_since_ns) / 1e9, l->vendor_call,
		    (double)(now - l->vendor_call_since_ns) / 1e9);
	} else {
		U_LOG_W(
		    "[lift] module still ACTIVATING after %.0f s; the plug-in keeps reporting ACTIVATING (its module "
		    "load has not finished). The dp-lift row stays WARMING, streams wait. Usual cause: a "
		    "broken or half-removed vendor conversion runtime install.",
		    (double)(now - l->activating_since_ns) / 1e9);
	}
}

//! Mark the lift thread as inside / out of a (possibly blocking) vendor call.
static void
lift_vendor_call(d3d11_lift *l, const char *what)
{
	std::lock_guard<std::mutex> g(l->mtx);
	l->vendor_call = what;
	l->vendor_call_since_ns = what != nullptr ? os_monotonic_get_ns() : 0;
}


/*
 *
 * Lift thread.
 *
 */

//! The lift device on the service's adapter (shared textures cross between them).
static HRESULT
lift_create_device(d3d11_lift *l)
{
	IDXGIDevice *dxgi_dev = nullptr;
	IDXGIAdapter *adapter = nullptr;
	HRESULT hr = S_OK;
	hr = l->svc_device->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi_dev);
	if (SUCCEEDED(hr)) {
		hr = dxgi_dev->GetAdapter(&adapter);
	}
	rel(dxgi_dev);
	D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
	if (SUCCEEDED(hr)) {
		hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
		                       levels, 2, D3D11_SDK_VERSION, &l->lift_device, &got, &l->lift_context);
	}
	rel(adapter);
	if (SUCCEEDED(hr)) {
		hr = l->lift_device->QueryInterface(__uuidof(ID3D11Device1), (void **)&l->lift_device1);
	}
	if (SUCCEEDED(hr)) {
		// A conversion module may flush / signal this context from its own
		// worker thread — the same protection the vendor weaver relies on.
		ID3D11Multithread *mt = nullptr;
		if (SUCCEEDED(l->lift_context->QueryInterface(__uuidof(ID3D11Multithread), (void **)&mt))) {
			mt->SetMultithreadProtected(TRUE);
			rel(mt);
		}
	}
	if (FAILED(hr)) {
		rel(l->lift_device1);
		rel(l->lift_context);
		rel(l->lift_device);
	}
	return hr;
}

/*!
 * LIFT_CMD_ACTIVATE: bring up the lift device + the vendor module's DP (one
 * attempt; the dp-lift adapter's readiness machine decides when to retry).
 * Lift thread, no lock held.
 */
static void
cmd_activate(d3d11_lift *l)
{
	const char *kill = getenv("DXR_LIFT");
	if (kill != nullptr && strcmp(kill, "0") == 0) {
		std::lock_guard<std::mutex> g(l->mtx);
		l->activated = true;
		caps_set_state(l, XRT_DP_LIFT_STATE_UNAVAILABLE, "DXR_LIFT=0 (kill switch)");
		return;
	}
	// G5: re-resolved at EVERY attempt, from the plug-in that registered the
	// lift factory (a plug-in swap or a late install is picked up, never latched).
	void *lift_factory_p = nullptr, *fallback_factory_p = nullptr;
	if (l->resolve != nullptr) {
		(void)l->resolve(l->resolve_ud, &lift_factory_p, &fallback_factory_p);
	}
	const xrt_dp_factory_d3d11_fn_t lift_factory = (xrt_dp_factory_d3d11_fn_t)lift_factory_p;
	const xrt_dp_factory_d3d11_fn_t fallback_factory = (xrt_dp_factory_d3d11_fn_t)fallback_factory_p;
	if (lift_factory == nullptr && fallback_factory == nullptr) {
		std::lock_guard<std::mutex> g(l->mtx);
		l->activated = true;
		caps_set_state(l, XRT_DP_LIFT_STATE_UNAVAILABLE, "no D3D11 display-processor factory");
		return;
	}

	// Same adapter as the service device: shared textures cross between them.
	// A retry keeps the device it already made.
	if (l->lift_device == nullptr && FAILED(lift_create_device(l))) {
		std::lock_guard<std::mutex> g(l->mtx);
		l->activated = true;
		caps_set_state(l, XRT_DP_LIFT_STATE_UNAVAILABLE, "lift device creation failed");
		return;
	}

	// The lift DP — never weaves, carries only the module. The plug-in's
	// explicit lift-only factory when it has one (no weaver, no tracker
	// session); else its ordinary factory with a NULL window.
	const bool lift_only = lift_factory != nullptr;
	lift_vendor_call(l, "display-processor factory");
	xrt_result_t xret =
	    (lift_only ? lift_factory : fallback_factory)(l->lift_device, l->lift_context, nullptr, &l->dp);
	lift_vendor_call(l, nullptr);
	U_LOG_W("[lift] lift DP %s via the plug-in's %s factory (dedicated device, NULL window)",
	        xret == XRT_SUCCESS && l->dp != nullptr ? "created" : "REFUSED",
	        lift_only ? "lift-only" : "ordinary (no lift-only factory)");
	xrt_dp_lift_caps caps;
	bool have = false;
	if (xret == XRT_SUCCESS && l->dp != nullptr && xrt_display_processor_d3d11_has_lift(l->dp)) {
		lift_vendor_call(l, "lift_get_caps");
		have = xrt_display_processor_d3d11_lift_get_caps(l->dp, &caps);
		lift_vendor_call(l, nullptr);
	} else {
		xrt_dp_lift_caps_init(&caps);
	}
	if (!have && l->dp != nullptr) {
		// No module on this plug-in: don't keep a vendor DP instance alive for nothing.
		xrt_display_processor_d3d11_destroy(&l->dp);
	}

	std::lock_guard<std::mutex> g(l->mtx);
	l->activated = true;
	l->module_alive = have;
	l->has_depth_slot = have && XRT_DP_HAS_SLOT(l->dp, lift_get_depth) && l->dp->lift_get_depth != nullptr;
	if (!have) {
		l->caps = caps;
		caps_set_state(l, XRT_DP_LIFT_STATE_UNAVAILABLE,
		               xret != XRT_SUCCESS ? "lift DP factory refused (NULL window)"
		                                   : "display processor ships no conversion module");
		return;
	}
	l->caps = caps;
	if (l->caps.state > XRT_DP_LIFT_STATE_READY) {
		l->caps.state = XRT_DP_LIFT_STATE_UNAVAILABLE;
	}
	if (l->caps.modes == 0) {
		l->caps.state = XRT_DP_LIFT_STATE_UNAVAILABLE;
	}
	l->logged_state = 0xffffffffu;
	caps_set_state(l, l->caps.state, nullptr);
}

//! LIFT_CMD_POLL: one lift_get_caps (the adapter polls ≤ 1 Hz while not READY). Lift thread.
static void
cmd_poll(d3d11_lift *l)
{
	if (l->dp == nullptr) {
		return;
	}
	xrt_dp_lift_caps caps;
	lift_vendor_call(l, "lift_get_caps");
	bool have = xrt_display_processor_d3d11_lift_get_caps(l->dp, &caps);
	std::lock_guard<std::mutex> g(l->mtx);
	l->vendor_call = nullptr;
	if (!have) {
		caps_set_state(l, XRT_DP_LIFT_STATE_UNAVAILABLE, "module stopped answering");
		return;
	}
	uint32_t st = caps.state > XRT_DP_LIFT_STATE_READY ? XRT_DP_LIFT_STATE_UNAVAILABLE : caps.state;
	l->caps = caps;
	l->caps.state = l->logged_state; // keep the logged value until caps_set_state compares
	caps_set_state(l, st, nullptr);
}

/*
 *
 * The dp-lift backend on the lift thread (#2003). The provider's exec thread
 * queues one command per [S] call (lift_cmd_run) and waits; the lift thread
 * runs them in order, one at a time. The runtime's images arrive as
 * shared NT handles (glue-allocated on the service device, keyed mutex) and
 * are opened on the lift device once per allocation (lift_open).
 *
 */

static const char *k_blit_ps = R"(
Texture2D src : register(t0);
SamplerState samp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
	return src.SampleLevel(samp, uv, 0);
}
)";


//! Drop lift-device views of images the glue freed. Lift thread.
static void
lift_drop_evicted(d3d11_lift *l)
{
	std::vector<uint64_t> keys;
	{
		std::lock_guard<std::mutex> g(l->mtx);
		keys.swap(l->lift_evict);
	}
	for (uint64_t k : keys) {
		auto it = l->lift_open.find(k);
		if (it != l->lift_open.end()) {
			rel(it->second.rtv);
			rel(it->second.km);
			rel(it->second.tex);
			l->lift_open.erase(it);
		}
	}
}

//! The lift device's view of a runtime image (opened once per allocation). Lift thread.
static lift_open_img *
lift_open(d3d11_lift *l, const xrt_conv_image *img, bool want_rtv)
{
	if (img == nullptr || img->memory_kind != XRT_CONV_MEM_WIN32_NT_HANDLE || img->handle == 0 ||
	    l->lift_device1 == nullptr) {
		return nullptr;
	}
	auto it = l->lift_open.find(img->import_key);
	if (it == l->lift_open.end()) {
		lift_open_img o;
		HRESULT hr = l->lift_device1->OpenSharedResource1((HANDLE)(uintptr_t)img->handle,
		                                                  __uuidof(ID3D11Texture2D), (void **)&o.tex);
		if (FAILED(hr)) {
			U_LOG_E("[lift] dp-lift: runtime image open on the lift device failed: 0x%08lx",
			        (unsigned long)hr);
			return nullptr;
		}
		(void)o.tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&o.km);
		it = l->lift_open.emplace(img->import_key, o).first;
	}
	lift_open_img &o = it->second;
	if (want_rtv && o.rtv == nullptr) {
		D3D11_TEXTURE2D_DESC td = {};
		o.tex->GetDesc(&td);
		D3D11_RENDER_TARGET_VIEW_DESC rd = {};
		rd.Format = typed_srv_format(td.Format);
		rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		(void)l->lift_device->CreateRenderTargetView(o.tex, &rd, &o.rtv);
	}
	return &o;
}

static bool
lift_blit_init(d3d11_lift *l)
{
	if (l->blit_tried) {
		return l->blit_ps != nullptr && l->blit_vs != nullptr && l->blit_samp != nullptr;
	}
	l->blit_tried = true;
	ID3DBlob *b = nullptr, *err = nullptr;
	bool ok = SUCCEEDED(D3DCompile(k_snap_vs, strlen(k_snap_vs), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0,
	                               &b, &err)) &&
	          SUCCEEDED(l->lift_device->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr,
	                                                       &l->blit_vs));
	rel(b);
	rel(err);
	ok = ok &&
	     SUCCEEDED(D3DCompile(k_blit_ps, strlen(k_blit_ps), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &b,
	                          &err)) &&
	     SUCCEEDED(
	         l->lift_device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &l->blit_ps));
	rel(b);
	rel(err);
	if (ok) {
		D3D11_SAMPLER_DESC sd = {};
		sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		ok = SUCCEEDED(l->lift_device->CreateSamplerState(&sd, &l->blit_samp));
	}
	if (!ok) {
		U_LOG_E(
		    "[lift] dp-lift: output blit init failed — module outputs of another size / format are dropped");
	}
	return ok;
}

/*!
 * Write the module's output @p src (@p sw x @p sh, DXGI @p sfmt) into the
 * runtime target @p img: a copy when the shapes match (the usual case: the
 * provider's output desc predicted them), else a stretch / format-converting
 * draw. Lift thread.
 */
static bool
lift_write_target(
    d3d11_lift *l, const xrt_conv_image *img, ID3D11Resource *src, uint32_t sw, uint32_t sh, uint32_t sfmt)
{
	if (img == nullptr || img->memory_kind != XRT_CONV_MEM_WIN32_NT_HANDLE || src == nullptr) {
		return false; // the glue never hands dp-lift a CPU target
	}
	const DXGI_FORMAT tf = conv_fmt_to_dxgi(img->format);
	const bool same =
	    img->width == sw && img->height == sh && typed_srv_format((DXGI_FORMAT)sfmt) == typed_srv_format(tf);
	lift_open_img *o = lift_open(l, img, !same);
	if (o == nullptr || !img_acquire(o->km, img, 100)) {
		return false;
	}
	ID3D11DeviceContext *ctx = l->lift_context;
	bool ok = true;
	if (same) {
		D3D11_BOX box = {0, 0, 0, sw, sh, 1};
		ctx->CopySubresourceRegion(o->tex, 0, 0, 0, 0, src, 0, &box);
	} else {
		ID3D11ShaderResourceView *srv = nullptr;
		ok = o->rtv != nullptr && lift_blit_init(l);
		if (ok) {
			D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
			sd.Format = typed_srv_format((DXGI_FORMAT)sfmt);
			sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			sd.Texture2D.MipLevels = 1;
			ok = SUCCEEDED(l->lift_device->CreateShaderResourceView(src, &sd, &srv));
		}
		if (ok) {
			D3D11_VIEWPORT vp = {};
			vp.Width = (float)img->width;
			vp.Height = (float)img->height;
			vp.MaxDepth = 1.0f;
			ctx->RSSetViewports(1, &vp);
			ctx->RSSetState(nullptr);
			ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
			ctx->OMSetDepthStencilState(nullptr, 0);
			ctx->OMSetRenderTargets(1, &o->rtv, nullptr);
			ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
			ctx->IASetInputLayout(nullptr);
			ctx->VSSetShader(l->blit_vs, nullptr, 0);
			ctx->GSSetShader(nullptr, nullptr, 0);
			ctx->PSSetShader(l->blit_ps, nullptr, 0);
			ctx->PSSetSamplers(0, 1, &l->blit_samp);
			ctx->PSSetShaderResources(0, 1, &srv);
			ctx->Draw(4, 0);
			ID3D11ShaderResourceView *null_srv = nullptr;
			ctx->PSSetShaderResources(0, 1, &null_srv);
			ID3D11RenderTargetView *null_rtv = nullptr;
			ctx->OMSetRenderTargets(1, &null_rtv, nullptr);
		}
		rel(srv);
	}
	img_release(o->km, img);
	return ok;
}

//! One core-routed conversion (LIFT_CMD_CONVERT). Lift thread, no lock held.
static xrt_result_t
lift_core_convert(d3d11_lift *l, uint64_t dp_id, conv_dp_lift_convert *io)
{
	if (l->dp == nullptr || io == nullptr || io->input == nullptr) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	const xrt_conv_image *in = io->input;
	const uint32_t w = in->width, h = in->height;
	lift_core_stream &cs = l->core_streams[dp_id];

	// The module contract: an RGBA8 input of exactly w x h on the lift device,
	// valid for the call. Copied out of the runtime's slot so its keyed mutex
	// is held for one copy only.
	bool ok = w > 0 && h > 0;
	if (ok && (cs.exact_tex == nullptr || cs.exact_w != w || cs.exact_h != h)) {
		rel(cs.exact_tex);
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		ok = SUCCEEDED(l->lift_device->CreateTexture2D(&td, nullptr, &cs.exact_tex));
		cs.exact_w = ok ? w : 0;
		cs.exact_h = ok ? h : 0;
	}
	if (ok && in->memory_kind == XRT_CONV_MEM_WIN32_NT_HANDLE) {
		lift_open_img *o = lift_open(l, in, false);
		ok = o != nullptr && img_acquire(o->km, in, 100);
		if (ok) {
			D3D11_BOX box = {0, 0, 0, w, h, 1};
			l->lift_context->CopySubresourceRegion(cs.exact_tex, 0, 0, 0, 0, o->tex, 0, &box);
			img_release(o->km, in);
		}
	} else if (ok && in->memory_kind == XRT_CONV_MEM_CPU && in->cpu_ptr != nullptr) {
		l->lift_context->UpdateSubresource(cs.exact_tex, 0, nullptr, in->cpu_ptr,
		                                   in->row_stride != 0 ? in->row_stride : w * 4u, 0);
	} else {
		ok = false;
	}

	void *out_res = nullptr;
	uint32_t ow = 0, oh = 0, of = 0;
	if (ok) {
		lift_vendor_call(l, "lift_convert");
		ok = l->dp->lift_convert(l->dp, dp_id, l->lift_context, cs.exact_tex, w, h, io->params, io->viewpoints,
		                         io->viewpoint_floats, &out_res, &ow, &oh, &of) &&
		     out_res != nullptr && ow > 0 && oh > 0;
		lift_vendor_call(l, nullptr);
	}
	if (!ok) {
		return XRT_ERROR_OUTPUT_REQUEST_FAILURE; // no output this frame: the core keeps the previous one
	}
	io->out_w = ow;
	io->out_h = oh;
	io->out_format = dxgi_to_conv_fmt(of);

	const xrt_conv_image *primary = io->primary_is_depth ? io->depth_target : io->views_target;
	const bool pw = primary != nullptr && lift_write_target(l, primary, (ID3D11Resource *)out_res, ow, oh, of);
	if (io->primary_is_depth) {
		io->depth_written = pw;
	} else {
		io->views_written = pw;
	}
	// The auxiliary depth of THIS conversion (the DP's texture is valid only
	// until its next lift_convert on this stream).
	if (pw && io->want_depth && io->depth_target != nullptr) {
		xrt_dp_lift_depth dd;
		lift_vendor_call(l, "lift_get_depth");
		const bool got = xrt_display_processor_d3d11_lift_get_depth(l->dp, dp_id, l->lift_context, &dd);
		lift_vendor_call(l, nullptr);
		if (got && dd.resource != nullptr &&
		    (dd.format == DXGI_FORMAT_R32_FLOAT || dd.format == DXGI_FORMAT_R16_FLOAT)) {
			io->depth_written = lift_write_target(l, io->depth_target, (ID3D11Resource *)dd.resource,
			                                      dd.width, dd.height, dd.format);
			io->have_depth_meta = io->depth_written;
			io->depth = dd;
			io->depth.resource = nullptr;
		} else if (!cs.depth_logged) {
			cs.depth_logged = true;
			U_LOG_W("[lift] dp-lift stream %llu: depth asked for, the module returned none%s",
			        (unsigned long long)dp_id, got ? " in R32_FLOAT / R16_FLOAT" : "");
		}
	}
	l->lift_context->Flush();
	return pw ? XRT_SUCCESS : XRT_ERROR_OUTPUT_REQUEST_FAILURE;
}

//! Run one backend command. Lift thread, no lock held.
static void
lift_run_cmd(d3d11_lift *l, lift_cmd *c)
{
	lift_drop_evicted(l);
	switch (c->op) {
	case LIFT_CMD_ACTIVATE: {
		bool run;
		{
			std::lock_guard<std::mutex> g(l->mtx);
			l->activate_requested = true;
			// G5: no latch — a retry after "no module" runs the whole attempt
			// again, re-resolving the factory pair.
			run = !l->activated || (c->retry && !l->module_alive);
			if (run) {
				l->activated = false;
			}
		}
		if (run) {
			cmd_activate(l);
		}
		c->result = XRT_SUCCESS;
		break;
	}
	case LIFT_CMD_POLL:
		if (l->dp != nullptr) {
			cmd_poll(l);
		}
		c->result = XRT_SUCCESS;
		break;
	case LIFT_CMD_STREAM_CREATE: {
		c->result = XRT_ERROR_FEATURE_NOT_SUPPORTED;
		if (l->dp == nullptr || c->info == nullptr) {
			break;
		}
		lift_vendor_call(l, "lift_stream_create");
		const bool ok = l->dp->lift_stream_create(l->dp, c->info, &c->dp_id);
		lift_vendor_call(l, nullptr);
		if (ok) {
			l->core_streams[c->dp_id] = lift_core_stream{};
			c->result = XRT_SUCCESS;
			U_LOG_W("[lift] dp-lift: module stream %llu created (mode=%u hint=%u aux=%u)",
			        (unsigned long long)c->dp_id, c->info->mode, c->info->content_hint,
			        c->info->aux_outputs);
		} else {
			U_LOG_W("[lift] dp-lift: the module refused a stream (mode=%u hint=%u)", c->info->mode,
			        c->info->content_hint);
		}
		break;
	}
	case LIFT_CMD_STREAM_DESTROY: {
		if (l->dp != nullptr) {
			lift_vendor_call(l, "lift_stream_destroy");
			l->dp->lift_stream_destroy(l->dp, c->dp_id);
			lift_vendor_call(l, nullptr);
		}
		auto it = l->core_streams.find(c->dp_id);
		if (it != l->core_streams.end()) {
			rel(it->second.exact_tex);
			l->core_streams.erase(it);
		}
		c->result = XRT_SUCCESS;
		break;
	}
	case LIFT_CMD_CONVERT: c->result = lift_core_convert(l, c->dp_id, c->io); break;
	}
}

/*!
 * The one lift thread: the only thread that ever calls the lift display
 * processor (the vendor module's contract). It runs the dp-lift adapter's
 * commands in order; scheduling across streams, readiness and the activation
 * policy live in the conversion core and the adapter (ADR-053 P1b).
 */
static void
lift_thread_main(d3d11_lift *l)
{
	std::unique_lock<std::mutex> lk(l->mtx);
	for (;;) {
		l->cv.wait(lk, [&] { return l->stop || !l->cmds.empty(); });
		if (l->stop) {
			break;
		}
		lift_cmd *c = l->cmds.front();
		l->cmds.pop_front();
		lk.unlock();
		lift_run_cmd(l, c);
		lk.lock();
		c->done = true;
		l->cmd_cv.notify_all();
	}

	// Shutdown: queued backend commands fail, every module stream goes, then
	// the DP, on this thread.
	while (!l->cmds.empty()) {
		lift_cmd *c = l->cmds.front();
		l->cmds.pop_front();
		c->result = XRT_ERROR_FEATURE_NOT_SUPPORTED;
		c->done = true;
	}
	l->module_alive = false;
	l->cmd_cv.notify_all();
	lk.unlock();
	for (auto &kv : l->core_streams) {
		if (l->dp != nullptr) {
			l->dp->lift_stream_destroy(l->dp, kv.first);
		}
		rel(kv.second.exact_tex);
	}
	l->core_streams.clear();
	{
		std::lock_guard<std::mutex> g(l->mtx);
		l->lift_evict.clear();
	}
	for (auto &kv : l->lift_open) {
		rel(kv.second.rtv);
		rel(kv.second.km);
		rel(kv.second.tex);
	}
	l->lift_open.clear();
	rel(l->blit_samp);
	rel(l->blit_ps);
	rel(l->blit_vs);
	if (l->dp != nullptr) {
		xrt_display_processor_d3d11_destroy(&l->dp);
	}
}


/*
 *
 * Public API.
 *
 */

struct d3d11_lift *
d3d11_lift_create(ID3D11Device *svc_device,
                  ID3D11DeviceContext *svc_context,
                  std::mutex *svc_ctx_mutex,
                  d3d11_lift_resolve_factories_fn resolve,
                  void *resolve_ud)
{
	if (svc_device == nullptr || svc_context == nullptr || svc_ctx_mutex == nullptr) {
		return nullptr;
	}
	auto *l = new d3d11_lift();
	l->svc_device = svc_device;
	l->svc_device->AddRef();
	l->svc_context = svc_context;
	l->svc_context->AddRef();
	l->svc_ctx_mutex = svc_ctx_mutex;
	l->resolve = resolve;
	l->resolve_ud = resolve_ud;
	const bool snap_ok = conv_gpu_init(l);

	xrt_dp_lift_caps_init(&l->caps);
	l->caps.state = XRT_DP_LIFT_STATE_ACTIVATING;
	if (!snap_ok) {
		// No snapshot pipeline: nothing can feed a module. An attempt counts
		// as made, so the adapter reports ABSENT (retryable) instead of WARMING.
		l->activated = true;
		caps_set_state(l, XRT_DP_LIFT_STATE_UNAVAILABLE, "snapshot pipeline init failed");
	}
	l->thread = std::thread(lift_thread_main, l);
	return l;
}

void
d3d11_lift_destroy(struct d3d11_lift **lift_ptr)
{
	if (lift_ptr == nullptr || *lift_ptr == nullptr) {
		return;
	}
	d3d11_lift *l = *lift_ptr;
	glue_close(l);
	{
		std::lock_guard<std::mutex> g(l->mtx);
		l->stop = true;
	}
	l->cv.notify_all();
	if (l->thread.joinable()) {
		l->thread.join();
	}
	conv_gpu_fini(l);
	rel(l->lift_device1);
	rel(l->lift_context);
	rel(l->lift_device);
	rel(l->svc_context);
	rel(l->svc_device);
	delete l;
	*lift_ptr = nullptr;
}

//! Hand one command to the lift thread and wait for it (the provider's exec thread).
static xrt_result_t
lift_cmd_run(d3d11_lift *l, lift_cmd *c)
{
	std::unique_lock<std::mutex> lk(l->mtx);
	if (l->stop) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	l->cmds.push_back(c);
	l->cv.notify_all();
	l->cmd_cv.wait(lk, [c] { return c->done; });
	return c->result;
}

static void
be_snapshot(void *ud, conv_dp_lift_snapshot *out)
{
	d3d11_lift *l = (d3d11_lift *)ud;
	memset(out, 0, sizeof(*out));
	void *lf = nullptr, *ff = nullptr;
	if (l->resolve != nullptr) {
		(void)l->resolve(l->resolve_ud, &lf, &ff);
	}
	out->factory_present = lf != nullptr || ff != nullptr;
	std::lock_guard<std::mutex> g(l->mtx);
	// #1812: the adapter's warm polls this while the module is ACTIVATING, so
	// the stall WARN fires even while the lift thread is blocked in a vendor call.
	lift_check_activating_stall(l, os_monotonic_get_ns());
	out->attempted = l->activated;
	out->have_module = l->activated && l->module_alive;
	out->has_depth_slot = l->has_depth_slot;
	out->caps = l->caps;
	snprintf(out->detail, sizeof(out->detail), "%s", l->unavail_detail);
	out->change_count = l->change_count;
}

static xrt_result_t
be_activate(void *ud, bool retry)
{
	lift_cmd c;
	c.op = LIFT_CMD_ACTIVATE;
	c.retry = retry;
	return lift_cmd_run((d3d11_lift *)ud, &c);
}

static void
be_poll(void *ud)
{
	lift_cmd c;
	c.op = LIFT_CMD_POLL;
	(void)lift_cmd_run((d3d11_lift *)ud, &c);
}

static xrt_result_t
be_stream_create(void *ud, const xrt_dp_lift_stream_info *info, uint64_t *out_dp_id)
{
	lift_cmd c;
	c.op = LIFT_CMD_STREAM_CREATE;
	c.info = info;
	const xrt_result_t r = lift_cmd_run((d3d11_lift *)ud, &c);
	*out_dp_id = c.dp_id;
	return r;
}

static void
be_stream_destroy(void *ud, uint64_t dp_id)
{
	lift_cmd c;
	c.op = LIFT_CMD_STREAM_DESTROY;
	c.dp_id = dp_id;
	(void)lift_cmd_run((d3d11_lift *)ud, &c);
}

static xrt_result_t
be_convert(void *ud, uint64_t dp_id, conv_dp_lift_convert *io)
{
	lift_cmd c;
	c.op = LIFT_CMD_CONVERT;
	c.dp_id = dp_id;
	c.io = io;
	return lift_cmd_run((d3d11_lift *)ud, &c);
}

void
d3d11_lift_get_dp_backend(struct d3d11_lift *l, struct conv_dp_lift_backend *out)
{
	memset(out, 0, sizeof(*out));
	if (l == nullptr) {
		return;
	}
	out->snapshot = be_snapshot;
	out->activate = be_activate;
	out->poll = be_poll;
	out->stream_create = be_stream_create;
	out->stream_destroy = be_stream_destroy;
	out->convert = be_convert;
	out->ud = l;
}
