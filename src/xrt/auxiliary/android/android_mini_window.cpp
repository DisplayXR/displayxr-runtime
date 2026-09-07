// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Native bridge to org.freedesktop.monado.auxiliary.MiniWindowLayout (#1396).
 * @author David Fattal
 * @ingroup aux_android
 */

#include "android_mini_window.h"
#include "android_globals.h"
#include "android_load_class.hpp"

#include "xrt/xrt_config_android.h"
#include "util/u_logging.h"
#include "os/os_time.h"

#include "wrap/android.app.h"

#include <jni.h>

using xrt::auxiliary::android::loadClassFromRuntimeApk;

namespace {

//! Mirrors MiniWindowLayout.HINT_* — keep the two in step.
enum
{
	HINT_LAYOUT_W = 0,
	HINT_LAYOUT_H = 1,
	HINT_BUFFER_W = 2,
	HINT_BUFFER_H = 3,
	HINT_RAT_P = 4,
	HINT_RAT_Q = 5,
	HINT_LEN = 6,
};

/*!
 * How long to wait before re-attempting a class lookup that failed (#1401).
 *
 * Same shape and the same reasoning as `MiniWindowLayout.VENDOR_RETRY_MS`: don't
 * re-reflect on every published rect while it keeps failing, but never let ONE
 * failure be final.
 */
constexpr uint64_t RESOLVE_RETRY_NS = 500 * 1000 * 1000ULL;

struct Bridge
{
	//! Resolved. THE SUCCESS is cached, not the attempt (#1401).
	jclass clazz = nullptr; //!< global ref
	jmethodID compute = nullptr;
	jmethodID reset = nullptr;
	jmethodID scalable = nullptr;

	//! Monotonic ns of the last FAILED attempt; 0 = never tried.
	uint64_t last_fail_ns = 0;
	//! One WARN per distinct failure episode, not one per retry.
	bool fail_logged = false;
	//! Hard stop: the aux AAR is too old to have the method. Retrying that
	//! cannot change the answer within the life of the process.
	bool unsupported = false;
};

Bridge &
bridge()
{
	static Bridge b;
	return b;
}

//! Note a failed attempt so the next one backs off, and WARN at most once per episode.
bool
resolve_failed(const char *why)
{
	Bridge &b = bridge();
	b.last_fail_ns = os_monotonic_get_ns();
	if (b.last_fail_ns == 0) {
		b.last_fail_ns = 1; // 0 means "never tried"
	}
	if (!b.fail_logged) {
		b.fail_logged = true;
		U_LOG_W(
		    "android_mini_window: %s — mini-window layout hints unavailable for now, retrying "
		    "every %u ms (#1396/#1401)",
		    why, (unsigned)(RESOLVE_RETRY_NS / (1000 * 1000)));
	}
	return false;
}

/*!
 * The runtime .so is dlopen'ed by the OpenXR loader, so ART's `FindClass` on a
 * native thread resolves against the SYSTEM classloader and would never see
 * `MiniWindowLayout`. Same dance as android_custom_surface: go through the
 * runtime APK's own DexClassLoader.
 *
 * CACHES THE SUCCESS, NOT THE ATTEMPT (#1401). Every input this depends on can
 * be transiently absent — the Context is stored during instance creation, the
 * runtime APK's DexClassLoader is built lazily, JNI can throw — and caching the
 * attempt turned any one of those into a permanent, silent loss of the feature
 * for the life of the process, with a single WARN at the moment it happened. The
 * one thing that IS cached as final is an aux AAR without the method: that is a
 * mismatched install, and no amount of retrying changes it.
 */
bool
resolve(JNIEnv *env)
{
	Bridge &b = bridge();
	if (b.clazz != nullptr) {
		return true; // resolved; nothing below can un-resolve it
	}
	if (b.unsupported) {
		return false;
	}
	if (b.last_fail_ns != 0 && os_monotonic_get_ns() - b.last_fail_ns < RESOLVE_RETRY_NS) {
		return false; // backing off
	}

	void *context = android_globals_get_context();
	if (context == nullptr) {
		return resolve_failed("no Android Context yet");
	}

	jclass global = nullptr;
	try {
		jni::init((JavaVM *)android_globals_get_vm());
		auto loaded =
		    loadClassFromRuntimeApk((jobject)context, "org.freedesktop.monado.auxiliary.MiniWindowLayout");
		if (loaded.isNull()) {
			return resolve_failed("could not load MiniWindowLayout from " XRT_ANDROID_PACKAGE);
		}
		jclass local = (jclass)loaded.object().getHandle();
		global = (jclass)env->NewGlobalRef(local);
	} catch (std::exception const &e) {
		U_LOG_W("android_mini_window: MiniWindowLayout lookup threw (%s) (#1396)", e.what());
		return resolve_failed("MiniWindowLayout lookup threw");
	}
	if (global == nullptr) {
		return resolve_failed("NewGlobalRef on MiniWindowLayout failed");
	}

	jmethodID compute = env->GetStaticMethodID(global, "computeHintForActivity", "(Ljava/lang/Object;IIIIII)[I");
	if (compute == nullptr) {
		env->ExceptionClear();
	}
	jmethodID reset = env->GetStaticMethodID(global, "resetForActivity", "()V");
	if (reset == nullptr) {
		env->ExceptionClear();
	}
	jmethodID scalable = env->GetStaticMethodID(global, "isInScalableContainer", "(Ljava/lang/Object;)Z");
	if (scalable == nullptr) {
		env->ExceptionClear();
	}
	if (compute == nullptr) {
		// An older aux AAR without the method (mismatched runtime install).
		// Final, unlike everything else above.
		U_LOG_W(
		    "android_mini_window: MiniWindowLayout.computeHintForActivity missing — "
		    "mini-window layout hints disabled (#1396)");
		env->DeleteGlobalRef(global);
		b.unsupported = true;
		return false;
	}

	// Publish only once everything is in hand, so a partially-resolved bridge
	// is never observable.
	b.compute = compute;
	b.reset = reset;
	b.scalable = scalable;
	b.clazz = global;
	b.last_fail_ns = 0;
	b.fail_logged = false;
	return true;
}

JNIEnv *
attached_env()
{
	try {
		jni::init((JavaVM *)android_globals_get_vm());
		return jni::env(); // jnipp's ScopedEnv attaches this thread if needed
	} catch (std::exception const &) {
		return nullptr;
	}
}

} // namespace

extern "C" bool
android_mini_window_compute_hint(int32_t x,
                                 int32_t y,
                                 uint32_t w,
                                 uint32_t h,
                                 uint32_t disp_w,
                                 uint32_t disp_h,
                                 struct android_mini_window_hint *out)
{
	if (out == nullptr || w == 0 || h == 0 || disp_w == 0 || disp_h == 0) {
		return false;
	}
	JNIEnv *env = attached_env();
	if (env == nullptr || !resolve(env)) {
		return false;
	}
	Bridge &b = bridge();

	// PINNED, not borrowed: this runs on the app's geometry thread while the UI
	// thread may be retiring the Activity through a #1392 relaunch (#1401 review).
	jobject activity = (jobject)android_globals_acquire_activity(env);
	jobject result = env->CallStaticObjectMethod(b.clazz, b.compute, activity, (jint)x, (jint)y, (jint)w, (jint)h,
	                                             (jint)disp_w, (jint)disp_h);
	android_globals_release_activity(env, activity);
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
		return false;
	}
	if (result == nullptr) {
		return false;
	}

	jintArray arr = (jintArray)result;
	jint vals[HINT_LEN] = {0};
	if (env->GetArrayLength(arr) < (jsize)HINT_LEN) {
		env->DeleteLocalRef(result);
		return false;
	}
	env->GetIntArrayRegion(arr, 0, HINT_LEN, vals);
	env->DeleteLocalRef(result);

	out->layout_w = vals[HINT_LAYOUT_W];
	out->layout_h = vals[HINT_LAYOUT_H];
	out->buffer_w = vals[HINT_BUFFER_W];
	out->buffer_h = vals[HINT_BUFFER_H];
	out->rat_p = vals[HINT_RAT_P];
	out->rat_q = vals[HINT_RAT_Q];
	out->scale = vals[HINT_RAT_Q] > 0 ? (float)vals[HINT_RAT_P] / (float)vals[HINT_RAT_Q]
	                                  : (float)vals[HINT_BUFFER_W] / (float)vals[HINT_LAYOUT_W];

	return out->layout_w > 0 && out->layout_h > 0 && out->buffer_w > 0 && out->buffer_h > 0;
}

extern "C" bool
android_mini_window_still_scalable(void)
{
	Bridge &b = bridge();
	JNIEnv *env = attached_env();
	if (env == nullptr || !resolve(env) || b.scalable == nullptr) {
		return true; // never end a hint on ignorance
	}
	jobject activity = (jobject)android_globals_acquire_activity(env);
	jboolean ret = env->CallStaticBooleanMethod(b.clazz, b.scalable, activity);
	android_globals_release_activity(env, activity);
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
		return true;
	}
	return ret == JNI_TRUE;
}

extern "C" void
android_mini_window_reset(void)
{
	Bridge &b = bridge();
	/*
	 * A container transition is the one moment worth spending a lookup on
	 * regardless of the backoff — it is exactly when the feature is about to be
	 * needed again, and by then whatever was transiently missing (the Context,
	 * the DexClassLoader) has had a whole episode to appear. Re-arm the WARN
	 * with it so a failure that comes back is not silent (#1401).
	 */
	b.last_fail_ns = 0;
	b.fail_logged = false;
	if (b.clazz == nullptr || b.reset == nullptr) {
		return;
	}
	JNIEnv *env = attached_env();
	if (env == nullptr) {
		return;
	}
	env->CallStaticVoidMethod(b.clazz, b.reset);
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
	}
}
