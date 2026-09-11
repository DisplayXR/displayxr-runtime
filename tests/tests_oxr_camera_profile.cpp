// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
// Explicit Windows/GPU acceptance probe. Opens a runtime-owned native window;
// excluded from automatic CTest. Loads the built runtime directly, without
// changing the system OpenXR manifest or plug-in registrations.
#define WIN32_LEAN_AND_MEAN
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/XR_DXR_view_rig.h>
#include <openxr/XR_DXR_display_info.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

static void
require(bool condition, const char *message)
{
	if (!condition)
		throw std::runtime_error(message);
}
static void
check(XrResult result, const char *message)
{
	if (XR_FAILED(result)) {
		std::fprintf(stderr, "%s: XrResult %d\n", message, int(result));
		throw std::runtime_error(message);
	}
}

struct Runtime
{
	PFN_xrGetInstanceProcAddr gipa = nullptr;
	Runtime()
	{
		std::string path = DXR_RUNTIME_LIB_PATH;
		for (char &c : path)
			if (c == '/')
				c = '\\';
		HMODULE library = LoadLibraryExA(path.c_str(), nullptr,
		                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
		require(library != nullptr, "Could not load the candidate runtime");
		auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
		    GetProcAddress(library, "xrNegotiateLoaderRuntimeInterface"));
		require(negotiate != nullptr, "Runtime negotiation export missing");
		XrNegotiateLoaderInfo loader{};
		loader.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
		loader.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
		loader.structSize = sizeof(loader);
		loader.minInterfaceVersion = 1;
		loader.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
		loader.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
		loader.maxApiVersion = XR_CURRENT_API_VERSION;
		XrNegotiateRuntimeRequest request{};
		request.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
		request.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
		request.structSize = sizeof(request);
		check(negotiate(&loader, &request), "negotiate");
		gipa = request.getInstanceProcAddr;
		require(gipa != nullptr, "Runtime did not supply xrGetInstanceProcAddr");
		// As in tests_oxr_view_space, keep the runtime loaded for process lifetime.
	}
	template <typename T>
	T
	fn(XrInstance instance, const char *name)
	{
		PFN_xrVoidFunction value = nullptr;
		check(gipa(instance, name, &value), name);
		require(value != nullptr, name);
		return reinterpret_cast<T>(value);
	}
};

struct App
{
	Runtime &runtime;
	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId system = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	XrSpace space = XR_NULL_HANDLE;
	XrSession sibling_session = XR_NULL_HANDLE;
	XrSpace sibling_space = XR_NULL_HANDLE;
	ComPtr<ID3D11Device> device;
	bool native;
	App(Runtime &rt, bool display_aware, bool native_session, bool prepare_native = false)
	    : runtime(rt), native(native_session)
	{
		std::vector<const char *> extensions{XR_DXR_VIEW_RIG_EXTENSION_NAME,
		                                     XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME};
		extensions.push_back(native ? XR_KHR_D3D11_ENABLE_EXTENSION_NAME : XR_MND_HEADLESS_EXTENSION_NAME);
		if (!native && prepare_native)
			extensions.push_back(XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
		if (display_aware)
			extensions.push_back(XR_DXR_DISPLAY_INFO_EXTENSION_NAME);
		XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
		std::snprintf(info.applicationInfo.applicationName, sizeof(info.applicationInfo.applicationName),
		              "camera-profile-acceptance");
		info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
		info.enabledExtensionCount = uint32_t(extensions.size());
		info.enabledExtensionNames = extensions.data();
		check(runtime.fn<PFN_xrCreateInstance>(XR_NULL_HANDLE, "xrCreateInstance")(&info, &instance),
		      "xrCreateInstance");
		XrSystemGetInfo get{XR_TYPE_SYSTEM_GET_INFO};
		get.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
		check(fn<PFN_xrGetSystem>("xrGetSystem")(instance, &get, &system), "xrGetSystem");
		if (native || prepare_native) {
			XrGraphicsRequirementsD3D11KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
			check(fn<PFN_xrGetD3D11GraphicsRequirementsKHR>("xrGetD3D11GraphicsRequirementsKHR")(
			          instance, system, &requirements),
			      "graphics requirements");
			ComPtr<IDXGIFactory1> factory;
			require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "DXGI factory failed");
			ComPtr<IDXGIAdapter1> adapter;
			for (UINT i = 0;; i++) {
				adapter.Reset();
				require(SUCCEEDED(factory->EnumAdapters1(i, &adapter)), "Runtime adapter not found");
				DXGI_ADAPTER_DESC1 desc{};
				require(SUCCEEDED(adapter->GetDesc1(&desc)), "Adapter description failed");
				if (desc.AdapterLuid.HighPart == requirements.adapterLuid.HighPart &&
				    desc.AdapterLuid.LowPart == requirements.adapterLuid.LowPart)
					break;
			}
			D3D_FEATURE_LEVEL level;
			require(SUCCEEDED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr,
			                                    0, D3D11_SDK_VERSION, &device, &level, nullptr)),
			        "D3D11 device failed");
			require(level >= requirements.minFeatureLevel, "Insufficient D3D11 feature level");
		}
		create_session();
	}
	template <typename T>
	T
	fn(const char *name)
	{
		return runtime.fn<T>(instance, name);
	}
	void
	destroy_session()
	{
		if (space) {
			fn<PFN_xrDestroySpace>("xrDestroySpace")(space);
			space = XR_NULL_HANDLE;
		}
		if (session) {
			fn<PFN_xrDestroySession>("xrDestroySession")(session);
			session = XR_NULL_HANDLE;
		}
	}
	~App()
	{
		destroy_session();
		if (sibling_space)
			fn<PFN_xrDestroySpace>("xrDestroySpace")(sibling_space);
		if (sibling_session)
			fn<PFN_xrDestroySession>("xrDestroySession")(sibling_session);
		if (instance)
			fn<PFN_xrDestroyInstance>("xrDestroyInstance")(instance);
	}
	void
	create_session()
	{
		XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
		binding.device = device.Get();
		XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
		info.systemId = system;
		if (native)
			info.next = &binding;
		check(fn<PFN_xrCreateSession>("xrCreateSession")(instance, &info, &session), "xrCreateSession");
		XrReferenceSpaceCreateInfo reference{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
		reference.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		reference.poseInReferenceSpace.orientation.w = 1;
		check(fn<PFN_xrCreateReferenceSpace>("xrCreateReferenceSpace")(session, &reference, &space),
		      "xrCreateReferenceSpace");
		XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
		begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		check(fn<PFN_xrBeginSession>("xrBeginSession")(session, &begin), "xrBeginSession");
	}
	XrTime
	now()
	{
		LARGE_INTEGER counter;
		QueryPerformanceCounter(&counter);
		XrTime time = 0;
		check(fn<PFN_xrConvertWin32PerformanceCounterToTimeKHR>("xrConvertWin32PerformanceCounterToTimeKHR")(
		          instance, &counter, &time),
		      "convert time");
		return time;
	}
	std::vector<XrView>
	views(XrTime time, const XrCameraRigDXR *rig = nullptr, bool sibling = false)
	{
		XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO, rig};
		locate.space = sibling ? sibling_space : space;
		locate.displayTime = time;
		locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		XrViewState state{XR_TYPE_VIEW_STATE};
		uint32_t count = 0;
		std::vector<XrView> result(8, {XR_TYPE_VIEW});
		check(fn<PFN_xrLocateViews>("xrLocateViews")(sibling ? sibling_session : session, &locate, &state,
		                                             uint32_t(result.size()), &count, result.data()),
		      "xrLocateViews");
		require(count > 0 && count <= result.size(), "No views supplied");
		result.resize(count);
		const auto valid = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
		require((state.viewStateFlags & valid) == valid, "Views are invalid");
		return result;
	}
};

static XrCameraRigDXR
rig(float vertical, float ipd = 0, float parallax = 0)
{
	XrCameraRigDXR result{XR_TYPE_CAMERA_RIG_DXR};
	result.pose.orientation.w = 1;
	result.ipdFactor = ipd;
	result.parallaxFactor = parallax;
	result.convergenceDiopters = .5f;
	result.verticalFov = vertical;
	result.metersToVirtual = 1;
	return result;
}
static void
same_fov(const std::vector<XrView> &a, const std::vector<XrView> &b)
{
	require(a.size() == b.size(), "View counts differ");
	for (size_t i = 0; i < a.size(); i++) {
		const auto &x = a[i].fov, &y = b[i].fov;
		require(std::fabs(x.angleUp - y.angleUp) < .01f && std::fabs(x.angleDown - y.angleDown) < .01f &&
		            std::fabs(x.angleLeft - y.angleLeft) < .01f &&
		            std::fabs(x.angleRight - y.angleRight) < .01f,
		        "Profile unexpectedly changed the view FOV");
	}
}

int
main(int argc, char **argv)
{
	if (argc != 2 || std::strcmp(argv[1], "--native") != 0) {
		std::fprintf(
		    stderr,
		    "Usage: tests_oxr_camera_profile --native (opens native GPU windows; needs a display processor)\n");
		return 2;
	}
	try {
		require(_putenv_s("XRT_FORCE_MODE", "native") == 0, "Could not select native runtime mode");
		SetEnvironmentVariableW(L"DXR_LEGACY_CAMERA_RIG",
		                        L"{\"ipdFactor\":0,\"parallaxFactor\":0,\"verticalFov\":1.0}");
		Runtime runtime;
		{
			App app(runtime, false, false, true);
			const auto defaults = rig(2.f * std::atan(.3249f), 1, 1);
			const auto before = app.now();
			same_fov(app.views(before), app.views(before, &defaults));
			app.sibling_session = app.session;
			app.sibling_space = app.space;
			app.session = XR_NULL_HANDLE;
			app.space = XR_NULL_HANDLE;
			app.native = true;
			app.create_session();
			const auto expected = rig(1.0f), explicit_rig = rig(.8f);
			const auto time = app.now();
			same_fov(app.views(time), app.views(time, &expected));
			same_fov(app.views(time, nullptr, true), app.views(time, &expected, true));
			for (const auto &v : app.views(time))
				require(std::fabs((v.fov.angleUp - v.fov.angleDown) - 1.0f) < .01f,
				        "Legacy camera profile was not applied");
			for (const auto &v : app.views(time, &explicit_rig))
				require(std::fabs((v.fov.angleUp - v.fov.angleDown) - .8f) < .01f,
				        "Explicit rig did not win");
			same_fov(app.views(time), app.views(time, &expected));
			app.destroy_session();
			const auto held = app.now();
			same_fov(app.views(held, nullptr, true), app.views(held, &expected, true));
			SetEnvironmentVariableW(L"DXR_LEGACY_CAMERA_RIG", L"{}");
			app.create_session();
			const auto later = app.now();
			same_fov(app.views(later), app.views(later, &expected));
			std::puts(
			    "PASS: instance seed, mixed native/headless sharing, explicit-rig precedence and no "
			    "session reseed");
		}
		{
			App recreated(runtime, false, true);
			const auto defaults = rig(2.f * std::atan(.3249f), 1, 1);
			const auto now = recreated.now();
			same_fov(recreated.views(now), recreated.views(now, &defaults));
			std::puts("PASS: destroying/recreating the instance allows a new profile lookup");
		}
		SetEnvironmentVariableW(L"DXR_LEGACY_CAMERA_RIG",
		                        L"{\"ipdFactor\":0,\"parallaxFactor\":0,\"verticalFov\":1.0}");
		for (bool native : {false, true}) {
			App app(runtime, native, native); // Headless legacy, then display-aware native.
			const auto defaults = rig(2.f * std::atan(.3249f), 1, 1);
			const auto time = app.now();
			same_fov(app.views(time), app.views(time, &defaults));
			std::puts(native ? "PASS: display-aware instance excludes profile"
			                 : "PASS: headless-only instance excludes profile");
		}
		return 0;
	} catch (const std::exception &e) {
		std::fprintf(stderr, "FAIL: %s\n", e.what());
		return 1;
	}
}
