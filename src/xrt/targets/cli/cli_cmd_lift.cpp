// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  `displayxr-cli lift` — XR_DXR_lift (ADR-042) caps + the N0 probe.
 *
 *   displayxr-cli lift caps [--json] [--wait S]
 *   displayxr-cli lift probe <image|frames_dir> [--mode depth|sbs|nview|gaussians]
 *                            [--n N] [--views N] [--strength F] [--convergence F]
 *                            [--focal PX] [--priority paused|low|normal|high]
 *                            [--pipelined] [--fps F] [--out DIR] [--wait S]
 *
 * Connects to the running service over IPC as a DIAG client (lift streams are
 * owned by the connection, no session needed) — run it from a NON-elevated
 * prompt, like `displayxr-cli clients`. `probe` is Windows-only: it hands the
 * service D3D11 shared textures, exactly as the browser does, and reads the
 * results back through the exported texture + fence.
 *
 * Per frame it prints the submit→acquire latency measured here (the number a
 * consumer sees), the service's own submit→converted latency, and the output
 * layout; results are written as lift_out_<i>.png (depth normalised to 8-bit
 * grey) or lift_out_<i>.ply (gaussians). The vendor module reads its knobs
 * (e.g. a DirectML/CUDA backend switch) from the SERVICE's environment, not
 * this process's — see docs/specs/extensions/XR_DXR_lift.md §9.
 */

#include "cli_common.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef CLI_HAVE_IPC

#include "xrt/xrt_instance.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_config_os.h"
#include "xrt/xrt_lift.h"
#include "util/u_logging.h"
#include "os/os_time.h"

#include "client/ipc_client.h"
#include "client/ipc_client_connection.h"
#include "client/ipc_client_lift.h"

#include <algorithm>
#include <string>
#include <vector>

#ifdef XRT_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#endif

namespace {

const char *
state_str(uint32_t s)
{
	switch (s) {
	case XRT_DP_LIFT_STATE_READY: return "READY";
	case XRT_DP_LIFT_STATE_ACTIVATING: return "ACTIVATING";
	default: return "UNAVAILABLE";
	}
}

std::string
modes_str(uint32_t m)
{
	std::string s;
	if (m & XRT_DP_LIFT_MODE_DEPTH) {
		s += "DEPTH ";
	}
	if (m & XRT_DP_LIFT_MODE_SBS) {
		s += "SBS ";
	}
	if (m & XRT_DP_LIFT_MODE_NVIEW) {
		s += "NVIEW ";
	}
	if (m & XRT_DP_LIFT_MODE_GAUSSIANS) {
		s += "GAUSSIANS ";
	}
	if (s.empty()) {
		s = "(none)";
	} else {
		s.pop_back();
	}
	return s;
}

const char *
opt_value(int argc, const char **argv, const char *name, const char *def)
{
	for (int i = 3; i + 1 < argc; i++) {
		if (strcmp(argv[i], name) == 0) {
			return argv[i + 1];
		}
	}
	return def;
}

bool
connect(struct ipc_connection *ipc_c)
{
	struct xrt_instance_info ii = {};
	snprintf(ii.app_info.application_name, sizeof(ii.app_info.application_name), "%s", "displayxr-cli lift");
	ii.app_info.declared_client_class = XRT_CLIENT_CLASS_DIAG;
	xrt_result_t xret = ipc_client_connection_init(ipc_c, U_LOGGING_ERROR, &ii);
	if (xret != XRT_SUCCESS) {
		printf("displayxr-cli lift: not connected to the service (xrt_result=%d).\n", (int)xret);
		printf("  Is displayxr-service running? On Windows, run from a NON-elevated prompt.\n");
		return false;
	}
	return true;
}

//! Poll caps until the module leaves ACTIVATING or @p wait_s passes.
bool
wait_caps(struct ipc_connection *ipc_c, double wait_s, struct xrt_dp_lift_caps *caps, bool verbose)
{
	uint64_t t0 = os_monotonic_get_ns();
	uint32_t last = 0xffffffffu;
	for (;;) {
		xrt_result_t xret = ipc_client_lift_get_properties(ipc_c, caps);
		if (xret != XRT_SUCCESS) {
			printf("lift_get_properties failed: %d\n", (int)xret);
			return false;
		}
		if (verbose && caps->state != last) {
			printf("  state: %s\n", state_str(caps->state));
			last = caps->state;
		}
		if (caps->state != XRT_DP_LIFT_STATE_ACTIVATING) {
			return true;
		}
		if ((double)(os_monotonic_get_ns() - t0) / 1e9 > wait_s) {
			return true;
		}
		os_nanosleep(250 * 1000 * 1000);
	}
}

void
print_caps(const struct xrt_dp_lift_caps &c, bool json)
{
	if (json) {
		printf(
		    "{\"connected\": true, \"state\": \"%s\", \"modes\": %u, \"modes_str\": \"%s\", "
		    "\"max_streams\": %u, \"max_views\": %u, \"depth_semantics\": \"%s\", "
		    "\"typical_latency_ms\": %.2f, \"backend\": \"%s\"}\n",
		    state_str(c.state), c.modes, modes_str(c.modes).c_str(), c.max_streams, c.max_views,
		    c.depth_semantics == XRT_DP_LIFT_DEPTH_METRIC ? "metric" : "relative",
		    (double)c.typical_latency_ns / 1e6, c.backend);
		return;
	}
	printf("XR_DXR_lift conversion module:\n");
	printf("  state:            %s\n", state_str(c.state));
	printf("  backend:          %s\n", c.backend[0] != '\0' ? c.backend : "(none)");
	printf("  modes:            %s (0x%x)\n", modes_str(c.modes).c_str(), c.modes);
	printf("  max streams:      %u\n", c.max_streams);
	printf("  max views:        %u\n", c.max_views);
	printf("  depth semantics:  %s\n", c.depth_semantics == XRT_DP_LIFT_DEPTH_METRIC ? "metric" : "relative");
	printf("  typical latency:  %.2f ms\n", (double)c.typical_latency_ns / 1e6);
}

int
cmd_caps(int argc, const char **argv)
{
	bool json = cli_has_flag(argc, argv, "--json");
	double wait_s = atof(opt_value(argc, argv, "--wait", "10"));
	struct ipc_connection ipc_c = {};
	if (!connect(&ipc_c)) {
		return 2;
	}
	struct xrt_dp_lift_caps caps;
	bool ok = wait_caps(&ipc_c, wait_s, &caps, !json);
	if (ok) {
		print_caps(caps, json);
	}
	ipc_client_connection_fini(&ipc_c);
	return ok ? 0 : 2;
}

#ifdef XRT_OS_WINDOWS

template <typename T>
void
rel(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

struct probe_gpu
{
	ID3D11Device *dev = nullptr;
	ID3D11Device1 *dev1 = nullptr;
	ID3D11Device5 *dev5 = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	ID3D11DeviceContext4 *ctx4 = nullptr;

	// Input: a shared keyed-mutex texture, re-created on size change.
	ID3D11Texture2D *in_tex = nullptr;
	IDXGIKeyedMutex *in_km = nullptr;
	HANDLE in_handle = nullptr;
	uint32_t in_w = 0, in_h = 0;

	// Output: the service's export texture + fence, re-opened on realloc.
	ID3D11Texture2D *out_tex = nullptr;
	ID3D11Fence *out_fence = nullptr;
	ID3D11Texture2D *staging = nullptr;
	uint32_t st_w = 0, st_h = 0, st_fmt = 0;

	~probe_gpu()
	{
		rel(staging);
		rel(out_fence);
		rel(out_tex);
		if (in_handle != nullptr) {
			CloseHandle(in_handle);
		}
		rel(in_km);
		rel(in_tex);
		rel(ctx4);
		rel(ctx);
		rel(dev5);
		rel(dev1);
		rel(dev);
	}
};

bool
gpu_init(probe_gpu &g)
{
	D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
	HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
	                               &fl, 1, D3D11_SDK_VERSION, &g.dev, nullptr, &g.ctx);
	if (FAILED(hr)) {
		printf("D3D11CreateDevice failed: 0x%08lx\n", (unsigned long)hr);
		return false;
	}
	g.dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&g.dev1);
	g.dev->QueryInterface(__uuidof(ID3D11Device5), (void **)&g.dev5);
	g.ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&g.ctx4);
	return g.dev1 != nullptr && g.dev5 != nullptr && g.ctx4 != nullptr;
}

bool
upload(probe_gpu &g, const uint8_t *rgba, uint32_t w, uint32_t h)
{
	if (g.in_tex == nullptr || g.in_w != w || g.in_h != h) {
		if (g.in_handle != nullptr) {
			CloseHandle(g.in_handle);
			g.in_handle = nullptr;
		}
		rel(g.in_km);
		rel(g.in_tex);
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
		HRESULT hr = g.dev->CreateTexture2D(&td, nullptr, &g.in_tex);
		IDXGIResource1 *r1 = nullptr;
		if (SUCCEEDED(hr)) {
			hr = g.in_tex->QueryInterface(__uuidof(IDXGIResource1), (void **)&r1);
		}
		if (SUCCEEDED(hr)) {
			hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
			                            nullptr, &g.in_handle);
		}
		rel(r1);
		if (SUCCEEDED(hr)) {
			hr = g.in_tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&g.in_km);
		}
		if (FAILED(hr)) {
			printf("input texture %ux%u create/share failed: 0x%08lx\n", w, h, (unsigned long)hr);
			return false;
		}
		g.in_w = w;
		g.in_h = h;
	}
	// Key 0 = "caller done writing" — the service's AcquireSync(0) waits on it.
	if (FAILED(g.in_km->AcquireSync(0, INFINITE))) {
		return false;
	}
	g.ctx->UpdateSubresource(g.in_tex, 0, nullptr, rgba, w * 4, 0);
	g.in_km->ReleaseSync(0);
	g.ctx->Flush();
	return true;
}

//! Read the export texture back (after waiting its fence) and write a PNG.
bool
read_back_png(probe_gpu &g, uint64_t fence_value, uint32_t w, uint32_t h, uint32_t fmt, const std::string &path)
{
	if (g.out_tex == nullptr || g.out_fence == nullptr) {
		return false;
	}
	if (g.staging == nullptr || g.st_w != w || g.st_h != h || g.st_fmt != fmt) {
		rel(g.staging);
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = (DXGI_FORMAT)fmt;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_STAGING;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &g.staging))) {
			printf("  staging %ux%u fmt=%u create failed\n", w, h, fmt);
			return false;
		}
		g.st_w = w;
		g.st_h = h;
		g.st_fmt = fmt;
	}
	g.ctx4->Wait(g.out_fence, fence_value);
	g.ctx->CopyResource(g.staging, g.out_tex);
	D3D11_MAPPED_SUBRESOURCE m = {};
	if (FAILED(g.ctx->Map(g.staging, 0, D3D11_MAP_READ, 0, &m))) {
		return false;
	}
	std::vector<uint8_t> rgba((size_t)w * h * 4);
	const uint8_t *src = (const uint8_t *)m.pData;
	if (fmt == DXGI_FORMAT_R8G8B8A8_UNORM || fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
	    fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
		const bool bgra = fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
		for (uint32_t y = 0; y < h; y++) {
			const uint8_t *row = src + (size_t)y * m.RowPitch;
			for (uint32_t x = 0; x < w; x++) {
				uint8_t *o = &rgba[((size_t)y * w + x) * 4];
				o[0] = row[x * 4 + (bgra ? 2 : 0)];
				o[1] = row[x * 4 + 1];
				o[2] = row[x * 4 + (bgra ? 0 : 2)];
				o[3] = 255;
			}
		}
	} else if (fmt == DXGI_FORMAT_R32_FLOAT || fmt == DXGI_FORMAT_R8_UNORM || fmt == DXGI_FORMAT_R16_UNORM) {
		// Depth: normalise min..max to 8-bit grey.
		std::vector<float> v((size_t)w * h);
		for (uint32_t y = 0; y < h; y++) {
			const uint8_t *row = src + (size_t)y * m.RowPitch;
			for (uint32_t x = 0; x < w; x++) {
				float f = 0.0f;
				if (fmt == DXGI_FORMAT_R32_FLOAT) {
					memcpy(&f, row + x * 4, 4);
				} else if (fmt == DXGI_FORMAT_R8_UNORM) {
					f = row[x] / 255.0f;
				} else {
					uint16_t u;
					memcpy(&u, row + x * 2, 2);
					f = u / 65535.0f;
				}
				v[(size_t)y * w + x] = f;
			}
		}
		auto mm = std::minmax_element(v.begin(), v.end());
		float lo = *mm.first, span = *mm.second - *mm.first;
		for (size_t i = 0; i < v.size(); i++) {
			uint8_t gy = span > 0 ? (uint8_t)std::min(255.0f, 255.0f * (v[i] - lo) / span) : 0;
			rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = gy;
			rgba[i * 4 + 3] = 255;
		}
	} else {
		g.ctx->Unmap(g.staging, 0);
		printf("  (format %u not written as PNG)\n", fmt);
		return false;
	}
	g.ctx->Unmap(g.staging, 0);
	return stbi_write_png(path.c_str(), (int)w, (int)h, 4, rgba.data(), (int)w * 4) != 0;
}

std::vector<std::string>
list_frames(const std::string &path)
{
	std::vector<std::string> out;
	DWORD attr = GetFileAttributesA(path.c_str());
	if (attr == INVALID_FILE_ATTRIBUTES) {
		return out;
	}
	if ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
		out.push_back(path);
		return out;
	}
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((path + "\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) {
		return out;
	}
	do {
		std::string n = fd.cFileName;
		std::string lower = n;
		std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
		auto ends = [&](const char *e) {
			size_t l = strlen(e);
			return lower.size() >= l && lower.compare(lower.size() - l, l, e) == 0;
		};
		if (ends(".png") || ends(".jpg") || ends(".jpeg") || ends(".bmp")) {
			out.push_back(path + "\\" + n);
		}
	} while (FindNextFileA(h, &fd));
	FindClose(h);
	std::sort(out.begin(), out.end());
	return out;
}

int
cmd_probe(int argc, const char **argv)
{
	if (argc < 4 || argv[3][0] == '-') {
		printf(
		    "usage: displayxr-cli lift probe <image|frames_dir> [--mode depth|sbs|nview|gaussians] [--n N]\n"
		    "       [--views N] [--strength F] [--convergence F] [--focal PX]\n"
		    "       [--priority paused|low|normal|high] [--pipelined] [--fps F] [--out DIR] [--wait S]\n");
		return 1;
	}
	const std::string src = argv[3];
	const std::string mode_s = opt_value(argc, argv, "--mode", "sbs");
	const int n_frames = atoi(opt_value(argc, argv, "--n", "100"));
	const std::string out_dir = opt_value(argc, argv, "--out", ".");
	const bool pipelined = cli_has_flag(argc, argv, "--pipelined");
	const double fps = atof(opt_value(argc, argv, "--fps", "30"));
	const double wait_s = atof(opt_value(argc, argv, "--wait", "30"));
	const std::string prio_s = opt_value(argc, argv, "--priority", "normal");

	uint32_t mode = XRT_DP_LIFT_MODE_SBS;
	if (mode_s == "depth") {
		mode = XRT_DP_LIFT_MODE_DEPTH;
	} else if (mode_s == "nview") {
		mode = XRT_DP_LIFT_MODE_NVIEW;
	} else if (mode_s == "gaussians") {
		mode = XRT_DP_LIFT_MODE_GAUSSIANS;
	} else if (mode_s != "sbs") {
		printf("unknown --mode '%s'\n", mode_s.c_str());
		return 1;
	}
	uint32_t prio = prio_s == "paused" ? 0 : prio_s == "low" ? 1 : prio_s == "high" ? 3 : 2;

	xrt_dp_lift_params params = {};
	params.struct_size = (uint32_t)sizeof(params);
	params.convergence = (float)atof(opt_value(argc, argv, "--convergence", "-1"));
	params.strength = (float)atof(opt_value(argc, argv, "--strength", "1"));
	params.inpaint = 1;
	params.view_count =
	    (uint32_t)atoi(opt_value(argc, argv, "--views", mode == XRT_DP_LIFT_MODE_NVIEW ? "4" : "2"));
	params.focal_px = (float)atof(opt_value(argc, argv, "--focal", "0"));

	std::vector<std::string> frames = list_frames(src);
	if (frames.empty()) {
		printf("no input image(s) at '%s'\n", src.c_str());
		return 1;
	}

	struct ipc_connection ipc_c = {};
	if (!connect(&ipc_c)) {
		return 2;
	}
	struct xrt_dp_lift_caps caps;
	printf("waiting for the conversion module (up to %.0f s)...\n", wait_s);
	if (!wait_caps(&ipc_c, wait_s, &caps, true)) {
		ipc_client_connection_fini(&ipc_c);
		return 2;
	}
	print_caps(caps, false);
	if (caps.state != XRT_DP_LIFT_STATE_READY || (caps.modes & mode) == 0) {
		printf("module not READY for mode %s — nothing to probe.\n", mode_s.c_str());
		ipc_client_connection_fini(&ipc_c);
		return 3;
	}

	probe_gpu g;
	if (!gpu_init(g)) {
		ipc_client_connection_fini(&ipc_c);
		return 2;
	}

	uint64_t sid = 0;
	xrt_result_t xret = ipc_client_lift_stream_create(
	    &ipc_c, mode, mode == XRT_DP_LIFT_MODE_GAUSSIANS ? XRT_DP_LIFT_CONTENT_PHOTO : XRT_DP_LIFT_CONTENT_VIDEO,
	    1.0f, &sid);
	if (xret != XRT_SUCCESS) {
		printf("lift_stream_create failed: %d\n", (int)xret);
		ipc_client_connection_fini(&ipc_c);
		return 2;
	}
	(void)ipc_client_lift_set_priority(&ipc_c, sid, prio);
	printf("stream %llu (%s, priority %s), %d frame(s) from %zu image(s), %s\n", (unsigned long long)sid,
	       mode_s.c_str(), prio_s.c_str(), n_frames, frames.size(),
	       pipelined ? "pipelined (latest wins)" : "sequential (wait for each result)");
	printf("%6s %10s %12s %12s %12s  %s\n", "frame", "frame_id", "submit->acq", "svc_latency", "out", "file");

	std::vector<double> lat_ms;
	int written = 0;
	int cached_idx = -1;
	int iw = 0, ih = 0;
	stbi_uc *pixels = nullptr;
	uint64_t next_submit_ns = os_monotonic_get_ns();
	for (int i = 0; i < n_frames; i++) {
		int idx = (int)(i % frames.size());
		if (idx != cached_idx) {
			if (pixels != nullptr) {
				stbi_image_free(pixels);
			}
			int comp = 0;
			pixels = stbi_load(frames[idx].c_str(), &iw, &ih, &comp, 4);
			cached_idx = idx;
			if (pixels == nullptr) {
				printf("cannot load '%s'\n", frames[idx].c_str());
				break;
			}
		}
		if (!upload(g, pixels, (uint32_t)iw, (uint32_t)ih)) {
			printf("upload failed\n");
			break;
		}
		if (pipelined) {
			uint64_t now = os_monotonic_get_ns();
			if (next_submit_ns > now) {
				os_nanosleep((int64_t)(next_submit_ns - now));
			}
			next_submit_ns += (uint64_t)(1e9 / (fps > 0 ? fps : 30));
		}
		uint64_t t_submit = os_monotonic_get_ns();
		uint64_t frame_id = 0;
		xret = ipc_client_lift_submit(&ipc_c, sid, (xrt_graphics_buffer_handle_t)g.in_handle, false,
		                              (uint32_t)iw, (uint32_t)ih, (int64_t)i, &params, nullptr, 0, &frame_id);
		if (xret != XRT_SUCCESS) {
			printf("%6d submit refused (xrt_result=%d) — retrying next frame\n", i, (int)xret);
			continue;
		}

		// Sequential: wait (bounded) for THIS frame's result. Pipelined: take
		// whatever is newest right now.
		const uint64_t deadline = t_submit + (uint64_t)(mode == XRT_DP_LIFT_MODE_GAUSSIANS ? 30e9 : 5e9);
		for (;;) {
			bool got = false;
			std::string file;
			std::string outdesc;
			uint64_t got_id = 0;
			uint64_t svc_lat = 0;
			if (mode == XRT_DP_LIFT_MODE_GAUSSIANS) {
				bool ready = false, delivered = false;
				struct xrt_lift_blob_info bi = {};
				xret = ipc_client_lift_acquire_blob(&ipc_c, sid, 0, nullptr, &ready, &delivered, &bi);
				if (xret == XRT_SUCCESS && ready) {
					std::vector<uint8_t> bytes((size_t)bi.byte_count);
					xret = ipc_client_lift_acquire_blob(&ipc_c, sid, bi.byte_count, bytes.data(),
					                                    &ready, &delivered, &bi);
					if (xret == XRT_SUCCESS && delivered) {
						got = true;
						got_id = bi.frame_id;
						char name[64];
						snprintf(name, sizeof(name), "lift_out_%d.%s", i,
						         bi.format == XRT_DP_LIFT_BLOB_SOG ? "sog" : "ply");
						file = out_dir + "\\" + name;
						FILE *f = fopen(file.c_str(), "wb");
						if (f != nullptr) {
							fwrite(bytes.data(), 1, bytes.size(), f);
							fclose(f);
							written++;
						}
						char d[64];
						snprintf(d, sizeof(d), "%llu B", (unsigned long long)bi.byte_count);
						outdesc = d;
					}
				}
			} else {
				bool ready = false;
				struct xrt_lift_result r = {};
				xret = ipc_client_lift_acquire(&ipc_c, sid, &ready, &r);
				if (xret == XRT_SUCCESS && ready) {
					got = true;
					got_id = r.frame_id;
					svc_lat = r.latency_ns;
					if (r.output_realloc || g.out_tex == nullptr) {
						rel(g.out_tex);
						rel(g.out_fence);
						bool have = false;
						xrt_graphics_buffer_handle_t th = XRT_GRAPHICS_BUFFER_HANDLE_INVALID;
						uint32_t ow = 0, oh = 0, of = 0;
						if (ipc_client_lift_get_output(&ipc_c, sid, &have, &ow, &oh, &of,
						                               &th) == XRT_SUCCESS &&
						    have) {
							g.dev1->OpenSharedResource1(
							    (HANDLE)th, __uuidof(ID3D11Texture2D), (void **)&g.out_tex);
							CloseHandle((HANDLE)th);
						}
						xrt_graphics_sync_handle_t fh = XRT_GRAPHICS_SYNC_HANDLE_INVALID;
						if (ipc_client_lift_get_fence(&ipc_c, sid, &have, &fh) == XRT_SUCCESS &&
						    have) {
							g.dev5->OpenSharedFence((HANDLE)fh, __uuidof(ID3D11Fence),
							                        (void **)&g.out_fence);
							CloseHandle((HANDLE)fh);
						}
					}
					char name[64];
					snprintf(name, sizeof(name), "lift_out_%d.png", i);
					file = out_dir + "\\" + name;
					if (read_back_png(g, r.fence_value, r.width, r.height, r.format, file)) {
						written++;
					} else {
						file = "(not written)";
					}
					char d[64];
					snprintf(d, sizeof(d), "%ux%u/%uv", r.width, r.height, r.view_count);
					outdesc = d;
				}
			}
			if (xret != XRT_SUCCESS) {
				printf("%6d acquire failed (xrt_result=%d)\n", i, (int)xret);
				break;
			}
			if (got && (pipelined || got_id >= frame_id)) {
				double ms = (double)(os_monotonic_get_ns() - t_submit) / 1e6;
				lat_ms.push_back(ms);
				printf("%6d %10llu %10.2fms %10.2fms %12s  %s\n", i, (unsigned long long)got_id, ms,
				       (double)svc_lat / 1e6, outdesc.c_str(), file.c_str());
				break;
			}
			if (pipelined || (uint64_t)os_monotonic_get_ns() > deadline) {
				if (!pipelined) {
					printf("%6d no result within the deadline\n", i);
				}
				break;
			}
			os_nanosleep(1000 * 1000);
		}
	}
	if (pixels != nullptr) {
		stbi_image_free(pixels);
	}

	struct xrt_lift_stream_stats st = {};
	if (ipc_client_lift_stats(&ipc_c, sid, &st) == XRT_SUCCESS) {
		printf(
		    "stream stats: submitted=%llu converted=%llu dropped=%llu failed=%llu rate=%.1f Hz "
		    "latency last=%.2f avg=%.2f min=%.2f max=%.2f ms\n",
		    (unsigned long long)st.submitted, (unsigned long long)st.converted, (unsigned long long)st.dropped,
		    (unsigned long long)st.failed, st.rate_hz, st.latency_last_ns / 1e6, st.latency_avg_ns / 1e6,
		    st.latency_min_ns / 1e6, st.latency_max_ns / 1e6);
	}
	if (!lat_ms.empty()) {
		std::vector<double> s = lat_ms;
		std::sort(s.begin(), s.end());
		double sum = 0;
		for (double v : s) {
			sum += v;
		}
		printf("submit->acquire: n=%zu avg=%.2f p50=%.2f p95=%.2f min=%.2f max=%.2f ms; %d file(s) written\n",
		       s.size(), sum / s.size(), s[s.size() / 2], s[(size_t)(s.size() * 0.95)], s.front(), s.back(),
		       written);
	}
	(void)ipc_client_lift_stream_destroy(&ipc_c, sid);
	ipc_client_connection_fini(&ipc_c);
	return lat_ms.empty() ? 4 : 0;
}

#else // !XRT_OS_WINDOWS

int
cmd_probe(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	printf("displayxr-cli lift probe: Windows-only (D3D11 shared textures). `lift caps` works everywhere.\n");
	return 2;
}

#endif // XRT_OS_WINDOWS

} // namespace

int
cli_cmd_lift(int argc, const char **argv)
{
	if (argc >= 3 && strcmp(argv[2], "caps") == 0) {
		return cmd_caps(argc, argv);
	}
	if (argc >= 3 && strcmp(argv[2], "probe") == 0) {
		return cmd_probe(argc, argv);
	}
	printf(
	    "usage: displayxr-cli lift caps [--json] [--wait S]\n"
	    "       displayxr-cli lift probe <image|frames_dir> [--mode depth|sbs|nview|gaussians] [--n N] ...\n");
	return 1;
}

#else // !CLI_HAVE_IPC

int
cli_cmd_lift(int argc, const char **argv)
{
	(void)argc;
	(void)argv;
	printf("displayxr-cli lift: this build has no IPC client (XRT_FEATURE_SERVICE off).\n");
	return 2;
}

#endif
