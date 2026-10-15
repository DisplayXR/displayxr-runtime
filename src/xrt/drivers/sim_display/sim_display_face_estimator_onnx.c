// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  sim_display webcam eye tracking (#1855): the opt-in face estimator,
 *         YuNet run through ONNX Runtime on the CPU.
 *
 * Built only with -DXRT_SIM_WEBCAM_ESTIMATOR=onnx (the default build compiles
 * sim_display_face_estimator_none.c instead). That configure step fetches the
 * ONNX Runtime prebuilt package and the YuNet model, both pinned by SHA256,
 * and copies the runtime library and the model next to the plug-in. See
 * src/xrt/drivers/sim_display/sim_webcam_onnx.cmake and
 * docs/vendors/sim_display/README.md § Webcam eye tracking.
 *
 * ONNX Runtime is loaded at run time, by full path from the plug-in's own
 * directory, never linked: the plug-in keeps loading when the library is
 * missing, and Windows' own System32\onnxruntime.dll (an older Windows ML
 * copy) can never be picked up instead. The model path can be overridden
 * with SIM_DISPLAY_WEBCAM_MODEL=<path>.
 *
 * Logging: one WARN when the library or model is missing, one when either
 * fails to load, one if inference ever fails. Nothing per frame.
 *
 * @ingroup drv_sim_display
 */

#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE // dladdr() + Dl_info on glibc
#endif

#include "sim_display_face_estimator.h"
#include "sim_display_settings.h"
#include "sim_display_face_yunet.h"

#include "util/u_logging.h"
#include "util/u_stereo_uvc.h"

#include <onnxruntime_c_api.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#ifndef SIM_ORT_LIB_NAME
#error "SIM_ORT_LIB_NAME must be defined by sim_webcam_onnx.cmake"
#endif
#ifndef SIM_WEBCAM_MODEL_NAME
#error "SIM_WEBCAM_MODEL_NAME must be defined by sim_webcam_onnx.cmake"
#endif

#define PATH_CAP 4096

#ifdef _WIN32
typedef wchar_t path_char;
#else
typedef char path_char;
#endif

static const char *const k_input_names[1] = {"input"};
// bbox_* is not fetched: the eyes need only the scores and the keypoints.
#define NUM_OUTPUTS 9
static const char *const k_output_names[NUM_OUTPUTS] = {
    "cls_8", "cls_16", "cls_32", "obj_8", "obj_16", "obj_32", "kps_8", "kps_16", "kps_32",
};


/*
 *
 * Paths.
 *
 */

static void
path_to_utf8(const path_char *p, char *out, size_t cap)
{
#ifdef _WIN32
	if (WideCharToMultiByte(CP_UTF8, 0, p, -1, out, (int)cap, NULL, NULL) <= 0) {
		out[0] = '\0';
	}
#else
	snprintf(out, cap, "%s", p);
#endif
}

//! Any address inside this module, for asking the OS which file it came from.
static const char k_module_anchor = 0;

static bool
is_sep(path_char c)
{
#ifdef _WIN32
	return c == L'\\' || c == L'/';
#else
	return c == '/';
#endif
}

//! The directory holding this plug-in, with a trailing separator.
static bool
own_dir(path_char *out, size_t cap)
{
#ifdef _WIN32
	HMODULE mod = NULL;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                        (LPCWSTR)&k_module_anchor, &mod)) {
		return false;
	}
	const DWORD n = GetModuleFileNameW(mod, out, (DWORD)cap);
	if (n == 0 || n >= cap) {
		return false;
	}
#else
	Dl_info info;
	if (dladdr(&k_module_anchor, &info) == 0 || info.dli_fname == NULL) {
		return false;
	}
	const int n = snprintf(out, cap, "%s", info.dli_fname);
	if (n <= 0 || (size_t)n >= cap) {
		return false;
	}
#endif
	path_char *last = NULL;
	for (path_char *c = out; *c != 0; c++) {
		if (is_sep(*c)) {
			last = c;
		}
	}
	if (last == NULL) {
		return false;
	}
	last[1] = 0;
	return true;
}

static bool
join(path_char *out, size_t cap, const path_char *dir, const char *name)
{
#ifdef _WIN32
	wchar_t wname[260];
	if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 260) <= 0) {
		return false;
	}
	return _snwprintf_s(out, cap, _TRUNCATE, L"%ls%ls", dir, wname) > 0;
#else
	const int n = snprintf(out, cap, "%s%s", dir, name);
	return n > 0 && (size_t)n < cap;
#endif
}

static bool
file_exists(const path_char *p)
{
#ifdef _WIN32
	const DWORD a = GetFileAttributesW(p);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
	return access(p, R_OK) == 0;
#endif
}

//! The ORT library and model paths. Resolved once; never loads anything.
static bool
resolve_paths(path_char *lib, path_char *model, size_t cap)
{
	path_char dir[PATH_CAP];
	if (!own_dir(dir, PATH_CAP) || !join(lib, cap, dir, SIM_ORT_LIB_NAME)) {
		return false;
	}
	// WEBCAM_MODEL through the host iface (N3c; UTF-8, converted for the wide path on Windows).
	char override[512];
	if (sim_display_setting("WEBCAM_MODEL", override, sizeof(override))) {
#ifdef _WIN32
		return MultiByteToWideChar(CP_UTF8, 0, override, -1, model, (int)cap) > 0;
#else
		const int n = snprintf(model, cap, "%s", override);
		return n > 0 && (size_t)n < cap;
#endif
	}
	return join(model, cap, dir, SIM_WEBCAM_MODEL_NAME);
}


/*
 *
 * Availability (cheap: two file checks, once).
 *
 */

bool
sim_face_estimator_default_available(void)
{
	// Idempotent, so a racing second caller only repeats the checks.
	static int cached = -1;
	if (cached >= 0) {
		return cached == 1;
	}
	path_char lib[PATH_CAP], model[PATH_CAP];
	char u8[PATH_CAP];
	bool ok = resolve_paths(lib, model, PATH_CAP);
	if (!ok) {
		U_LOG_W("sim_display webcam tracking (#1855): could not resolve the ONNX estimator's paths");
	} else if (!file_exists(lib)) {
		path_to_utf8(lib, u8, sizeof(u8));
		U_LOG_W("sim_display webcam tracking (#1855): ONNX Runtime not found at '%s' - no face estimator", u8);
		ok = false;
	} else if (!file_exists(model)) {
		path_to_utf8(model, u8, sizeof(u8));
		U_LOG_W(
		    "sim_display webcam tracking (#1855): face model not found at '%s' (SIM_DISPLAY_WEBCAM_MODEL "
		    "overrides) - no face estimator",
		    u8);
		ok = false;
	}
	cached = ok ? 1 : 0;
	return ok;
}


/*
 *
 * The estimator.
 *
 */

struct onnx_est
{
	struct sim_face_estimator base;

	bool tried;      //!< load attempted (worker thread only)
	bool ok;         //!< session usable
	bool warned_run; //!< inference failure already logged

	void *lib;
	const OrtApi *api;
	OrtEnv *env;
	OrtSession *session;
	OrtMemoryInfo *mem;

	struct sim_yunet_plan plan;
	float *tensor;
	size_t tensor_cap;
};

typedef const OrtApiBase *(ORT_API_CALL *pfn_get_api_base)(void);

//! Log and release a failed status. @return true when @p st was an error.
static bool
failed(struct onnx_est *e, OrtStatus *st, const char *what)
{
	if (st == NULL) {
		return false;
	}
	U_LOG_W("sim_display webcam tracking (#1855): ONNX Runtime %s failed: %s", what, e->api->GetErrorMessage(st));
	e->api->ReleaseStatus(st);
	return true;
}

static bool
load(struct onnx_est *e)
{
	path_char lib[PATH_CAP], model[PATH_CAP];
	char u8[PATH_CAP];
	if (!resolve_paths(lib, model, PATH_CAP)) {
		return false;
	}
	path_to_utf8(lib, u8, sizeof(u8));

	pfn_get_api_base get_base = NULL;
#ifdef _WIN32
	// Full path + altered search path: our copy, never System32's.
	HMODULE h = LoadLibraryExW(lib, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (h != NULL) {
		e->lib = (void *)h;
		get_base = (pfn_get_api_base)(void *)GetProcAddress(h, "OrtGetApiBase");
	}
#else
	e->lib = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
	if (e->lib != NULL) {
		get_base = (pfn_get_api_base)dlsym(e->lib, "OrtGetApiBase");
	}
#endif
	if (get_base == NULL) {
		U_LOG_W(
		    "sim_display webcam tracking (#1855): could not load ONNX Runtime from '%s' - no face estimates",
		    u8);
		return false;
	}
	const OrtApiBase *base = get_base();
	e->api = base != NULL ? base->GetApi(ORT_API_VERSION) : NULL;
	if (e->api == NULL) {
		U_LOG_W("sim_display webcam tracking (#1855): '%s' does not offer ONNX Runtime C API %d", u8,
		        ORT_API_VERSION);
		return false;
	}

	const OrtApi *a = e->api;
	if (failed(e, a->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "sim_display_webcam", &e->env), "CreateEnv")) {
		return false;
	}
	OrtStatus *tel = a->DisableTelemetryEvents(e->env); // best effort
	if (tel != NULL) {
		a->ReleaseStatus(tel);
	}

	OrtSessionOptions *opts = NULL;
	if (failed(e, a->CreateSessionOptions(&opts), "CreateSessionOptions")) {
		return false;
	}
	// A background helper: two threads, never the whole machine.
	OrtStatus *st = a->SetIntraOpNumThreads(opts, 2);
	if (st == NULL) {
		st = a->SetInterOpNumThreads(opts, 1);
	}
	if (st == NULL) {
		st = a->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL);
	}
	if (st == NULL) {
		st = a->CreateSession(e->env, model, opts, &e->session);
	}
	a->ReleaseSessionOptions(opts);
	if (st != NULL) {
		path_to_utf8(model, u8, sizeof(u8));
		U_LOG_W("sim_display webcam tracking (#1855): could not load the face model '%s': %s", u8,
		        a->GetErrorMessage(st));
		a->ReleaseStatus(st);
		return false;
	}
	if (failed(e, a->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &e->mem), "CreateCpuMemoryInfo")) {
		return false;
	}
	path_to_utf8(model, u8, sizeof(u8));
	U_LOG_W("sim_display webcam tracking (#1855): face estimator ready (YuNet via ONNX Runtime %s, model '%s')",
	        base->GetVersionString(), u8);
	return true;
}

static bool
run(struct onnx_est *e, const struct u_stereo_uvc_raw_frame *frame, struct sim_face_landmarks *out)
{
	if (e->plan.src_w != frame->width || e->plan.src_h != frame->height) {
		if (!sim_yunet_make_plan(frame->width, frame->height, SIM_YUNET_NET_WIDTH, &e->plan)) {
			return false;
		}
		const size_t need = sim_yunet_tensor_floats(&e->plan);
		if (need > e->tensor_cap) {
			float *t = (float *)realloc(e->tensor, need * sizeof(float));
			if (t == NULL) {
				e->plan.src_w = 0;
				return false;
			}
			e->tensor = t;
			e->tensor_cap = need;
		}
	}
	if (!sim_yunet_fill_tensor(frame, &e->plan, e->tensor)) {
		return false;
	}

	const OrtApi *a = e->api;
	const int64_t shape[4] = {1, 3, (int64_t)e->plan.pad_h, (int64_t)e->plan.pad_w};
	OrtValue *input = NULL;
	OrtValue *outputs[NUM_OUTPUTS] = {0};
	bool ok = false;
	OrtStatus *st =
	    a->CreateTensorWithDataAsOrtValue(e->mem, e->tensor, sim_yunet_tensor_floats(&e->plan) * sizeof(float),
	                                      shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input);
	if (st == NULL) {
		st = a->Run(e->session, NULL, k_input_names, (const OrtValue *const *)&input, 1, k_output_names,
		            NUM_OUTPUTS, outputs);
	}

	float *data[NUM_OUTPUTS] = {0};
	size_t elems[NUM_OUTPUTS] = {0};
	for (int i = 0; st == NULL && i < NUM_OUTPUTS; i++) {
		OrtTensorTypeAndShapeInfo *info = NULL;
		st = a->GetTensorTypeAndShape(outputs[i], &info);
		if (st == NULL) {
			st = a->GetTensorShapeElementCount(info, &elems[i]);
			a->ReleaseTensorTypeAndShapeInfo(info);
		}
		if (st == NULL) {
			st = a->GetTensorMutableData(outputs[i], (void **)&data[i]);
		}
	}
	if (st == NULL) {
		// cls/obj carry 1 value per row, kps 10.
		struct sim_yunet_head heads[SIM_YUNET_STRIDES];
		bool shapes_ok = true;
		for (int h = 0; h < SIM_YUNET_STRIDES; h++) {
			heads[h].cls = data[h];
			heads[h].obj = data[3 + h];
			heads[h].kps = data[6 + h];
			heads[h].rows = elems[h];
			shapes_ok = shapes_ok && elems[3 + h] == elems[h] && elems[6 + h] == elems[h] * 10u;
		}
		ok = shapes_ok && sim_yunet_decode(&e->plan, heads, SIM_YUNET_SCORE_MIN, out);
		if (!ok && !e->warned_run) {
			e->warned_run = true;
			U_LOG_W(
			    "sim_display webcam tracking (#1855): the face model's outputs do not have YuNet's shape");
		}
	} else {
		if (!e->warned_run) {
			e->warned_run = true;
			U_LOG_W("sim_display webcam tracking (#1855): face inference failed: %s",
			        a->GetErrorMessage(st));
		}
		a->ReleaseStatus(st);
	}

	for (int i = 0; i < NUM_OUTPUTS; i++) {
		if (outputs[i] != NULL) {
			a->ReleaseValue(outputs[i]);
		}
	}
	if (input != NULL) {
		a->ReleaseValue(input);
	}
	return ok;
}

static bool
onnx_estimate(struct sim_face_estimator *est,
              const struct u_stereo_uvc_raw_frame *frame,
              struct sim_face_landmarks *out)
{
	struct onnx_est *e = (struct onnx_est *)est;
	memset(out, 0, sizeof(*out));
	if (!e->tried) {
		e->tried = true; // one attempt, one WARN: a broken install never retries per frame
		e->ok = load(e);
	}
	if (!e->ok || frame == NULL) {
		return false;
	}
	return run(e, frame, out);
}

static void
onnx_destroy(struct sim_face_estimator *est)
{
	struct onnx_est *e = (struct onnx_est *)est;
	if (e->api != NULL) {
		if (e->mem != NULL) {
			e->api->ReleaseMemoryInfo(e->mem);
		}
		if (e->session != NULL) {
			e->api->ReleaseSession(e->session);
		}
		if (e->env != NULL) {
			e->api->ReleaseEnv(e->env);
		}
	}
	// The library stays mapped: ORT keeps process-wide state (its default
	// logger, thread pools) that does not survive an unload cleanly, and a
	// second start would only map it again.
	free(e->tensor);
	free(e);
}

struct sim_face_estimator *
sim_face_estimator_create_default(void)
{
	if (!sim_face_estimator_default_available()) {
		return NULL;
	}
	// Cheap: the library and model load on the worker's first estimate().
	struct onnx_est *e = (struct onnx_est *)calloc(1, sizeof(*e));
	if (e == NULL) {
		return NULL;
	}
	e->base.name = "yunet-onnx";
	e->base.estimate = onnx_estimate;
	e->base.destroy = onnx_destroy;
	return &e->base;
}
