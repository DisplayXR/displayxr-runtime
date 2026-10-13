// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The cheap GPU content check behind still-image dedupe (#2030).
 * @ingroup comp_d3d11_service
 */

#include "d3d11_conv_content_key.h"

#include "util/u_logging.h"

#include <d3d11_1.h>

#include <cstring>
#include <map>
#include <mutex>

namespace {

template <typename T>
void
rel(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

//! Largest edge of the sampled block.
constexpr uint32_t kBlockEdge = 16;

struct opened_img
{
	ID3D11Texture2D *tex = nullptr;
	IDXGIKeyedMutex *km = nullptr;
};

struct stream_state
{
	uint32_t w = 0, h = 0;
	DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
	ID3D11Texture2D *mip = nullptr;
	ID3D11ShaderResourceView *mip_srv = nullptr;
	uint32_t level = 0, lw = 0, lh = 0;
	ID3D11Texture2D *staging[2] = {nullptr, nullptr};
	uint64_t seq_in[2] = {0, 0}; //!< sample number each staging copy holds (0 = none)
	bool hashed[2] = {false, false};
	uint64_t key[2] = {0, 0};
	uint32_t cur = 0;
	uint64_t next_seq = 0;

	void
	release()
	{
		rel(mip_srv);
		rel(mip);
		rel(staging[0]);
		rel(staging[1]);
	}
};

//! The typed format the mip chain uses (copy-compatible with the slot).
DXGI_FORMAT
mip_format(DXGI_FORMAT f)
{
	switch (f) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
	default: return DXGI_FORMAT_UNKNOWN;
	}
}

DXGI_FORMAT
typeless_of(DXGI_FORMAT f)
{
	return f == DXGI_FORMAT_R8G8B8A8_UNORM ? DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_B8G8R8A8_TYPELESS;
}

uint64_t
mix64(uint64_t h, uint64_t v)
{
	h ^= v;
	h *= 0x9E3779B97F4A7C15ull;
	return h ^ (h >> 31);
}

} // namespace

struct d3d11_conv_content_check
{
	std::mutex mtx;
	ID3D11Device *dev = nullptr;
	ID3D11Device1 *dev1 = nullptr;
	ID3D11DeviceContext *ctx = nullptr; //!< private: no other code draws on it
	std::map<uint64_t, opened_img> imgs;
	std::map<uint64_t, stream_state> streams;
};

struct d3d11_conv_content_check *
d3d11_conv_content_check_create(IDXGIAdapter *adapter)
{
	d3d11_conv_content_check *c = new d3d11_conv_content_check();
	HRESULT hr = D3D11CreateDevice(adapter, adapter != nullptr ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
	                               nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &c->dev, nullptr, &c->ctx);
	if (SUCCEEDED(hr)) {
		hr = c->dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&c->dev1);
	}
	if (FAILED(hr)) {
		U_LOG_W("[conv] still-image content check: no private D3D11 device (0x%08lx): GPU PHOTO inputs always "
		        "convert",
		        (unsigned long)hr);
		d3d11_conv_content_check_destroy(&c);
		return nullptr;
	}
	return c;
}

void
d3d11_conv_content_check_destroy(struct d3d11_conv_content_check **chk_ptr)
{
	if (chk_ptr == nullptr || *chk_ptr == nullptr) {
		return;
	}
	d3d11_conv_content_check *c = *chk_ptr;
	for (auto &kv : c->imgs) {
		rel(kv.second.km);
		rel(kv.second.tex);
	}
	for (auto &kv : c->streams) {
		kv.second.release();
	}
	rel(c->ctx);
	rel(c->dev1);
	rel(c->dev);
	delete c;
	*chk_ptr = nullptr;
}

static uint64_t
hash_staging(d3d11_conv_content_check *c, stream_state &s, uint32_t i, bool wait, bool *out_ready)
{
	*out_ready = false;
	D3D11_MAPPED_SUBRESOURCE m = {};
	const HRESULT hr = c->ctx->Map(s.staging[i], 0, D3D11_MAP_READ, wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
	if (hr == DXGI_ERROR_WAS_STILL_DRAWING || FAILED(hr)) {
		return 0;
	}
	uint64_t h = mix64(0xCBF29CE484222325ull, ((uint64_t)s.w << 32) | s.h);
	h = mix64(h, ((uint64_t)s.lw << 32) | s.lh);
	for (uint32_t y = 0; y < s.lh; y++) {
		const uint8_t *row = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
		for (uint32_t x = 0; x + 8 <= s.lw * 4u; x += 8) {
			uint64_t v;
			memcpy(&v, row + x, 8);
			h = mix64(h, v);
		}
		if ((s.lw * 4u) % 8u != 0) {
			uint32_t v; // lw is odd: one 4-byte texel left
			memcpy(&v, row + s.lw * 4u - 4u, 4);
			h = mix64(h, v);
		}
	}
	c->ctx->Unmap(s.staging[i], 0);
	*out_ready = true;
	return h != 0 ? h : 1;
}

static bool
ensure_stream(d3d11_conv_content_check *c, stream_state &s, uint32_t w, uint32_t h, DXGI_FORMAT fmt)
{
	if (s.mip != nullptr && s.w == w && s.h == h && s.fmt == fmt) {
		return true;
	}
	s.release();
	s.seq_in[0] = s.seq_in[1] = 0;
	s.next_seq = 0;
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 0; // the full chain
	td.ArraySize = 1;
	td.Format = typeless_of(fmt);
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
	if (FAILED(c->dev->CreateTexture2D(&td, nullptr, &s.mip))) {
		return false;
	}
	s.mip->GetDesc(&td);
	D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
	sd.Format = fmt;
	sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	sd.Texture2D.MipLevels = (UINT)-1;
	if (FAILED(c->dev->CreateShaderResourceView(s.mip, &sd, &s.mip_srv))) {
		s.release();
		return false;
	}
	s.level = 0;
	while (s.level + 1 < td.MipLevels && ((w >> s.level) > kBlockEdge || (h >> s.level) > kBlockEdge)) {
		s.level++;
	}
	s.lw = (w >> s.level) > 0 ? (w >> s.level) : 1;
	s.lh = (h >> s.level) > 0 ? (h >> s.level) : 1;
	D3D11_TEXTURE2D_DESC st = {};
	st.Width = s.lw;
	st.Height = s.lh;
	st.MipLevels = 1;
	st.ArraySize = 1;
	st.Format = td.Format;
	st.SampleDesc.Count = 1;
	st.Usage = D3D11_USAGE_STAGING;
	st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	for (ID3D11Texture2D *&t : s.staging) {
		if (FAILED(c->dev->CreateTexture2D(&st, nullptr, &t))) {
			s.release();
			return false;
		}
	}
	s.w = w;
	s.h = h;
	s.fmt = fmt;
	return true;
}

uint64_t
d3d11_conv_content_check_sample(struct d3d11_conv_content_check *c, uint64_t stream, HANDLE share, uint64_t import_key)
{
	if (c == nullptr || share == nullptr) {
		return 0;
	}
	std::lock_guard<std::mutex> g(c->mtx);
	opened_img &oi = c->imgs[import_key];
	if (oi.tex == nullptr) {
		if (FAILED(c->dev1->OpenSharedResource1(share, __uuidof(ID3D11Texture2D), (void **)&oi.tex))) {
			c->imgs.erase(import_key);
			return 0;
		}
		(void)oi.tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&oi.km);
	}
	D3D11_TEXTURE2D_DESC d = {};
	oi.tex->GetDesc(&d);
	const DXGI_FORMAT fmt = mip_format(d.Format);
	stream_state &s = c->streams[stream];
	if (fmt == DXGI_FORMAT_UNKNOWN || d.ArraySize != 1 || !ensure_stream(c, s, d.Width, d.Height, fmt)) {
		return 0;
	}
	if (oi.km != nullptr) {
		const HRESULT hr = oi.km->AcquireSync(0, 4);
		if (FAILED(hr) || hr == (HRESULT)WAIT_TIMEOUT) {
			return 0;
		}
	}
	c->ctx->CopySubresourceRegion(s.mip, 0, 0, 0, 0, oi.tex, 0, nullptr);
	if (oi.km != nullptr) {
		oi.km->ReleaseSync(0);
	}
	c->ctx->GenerateMips(s.mip_srv);
	const uint32_t i = s.cur;
	c->ctx->CopySubresourceRegion(s.staging[i], 0, 0, 0, 0, s.mip, s.level, nullptr);
	c->ctx->Flush();
	const uint64_t seq = ++s.next_seq;
	s.seq_in[i] = seq;
	s.hashed[i] = false;
	s.cur ^= 1u;

	bool ready = false;
	if (seq == 1) {
		// The stream's first sample: wait for it, once, so the next
		// identical submit already dedupes.
		s.key[i] = hash_staging(c, s, i, true, &ready);
		s.hashed[i] = ready;
		return ready ? s.key[i] : 0;
	}
	const uint32_t p = i ^ 1u;
	if (s.seq_in[p] != seq - 1) {
		return 0;
	}
	if (!s.hashed[p]) {
		s.key[p] = hash_staging(c, s, p, false, &ready);
		s.hashed[p] = ready;
	}
	return s.hashed[p] ? s.key[p] : 0;
}

void
d3d11_conv_content_check_forget_image(struct d3d11_conv_content_check *c, uint64_t import_key)
{
	if (c == nullptr) {
		return;
	}
	std::lock_guard<std::mutex> g(c->mtx);
	auto it = c->imgs.find(import_key);
	if (it != c->imgs.end()) {
		rel(it->second.km);
		rel(it->second.tex);
		c->imgs.erase(it);
	}
}

void
d3d11_conv_content_check_forget_stream(struct d3d11_conv_content_check *c, uint64_t stream)
{
	if (c == nullptr) {
		return;
	}
	std::lock_guard<std::mutex> g(c->mtx);
	auto it = c->streams.find(stream);
	if (it != c->streams.end()) {
		it->second.release();
		c->streams.erase(it);
	}
}
