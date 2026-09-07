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

struct Bridge
{
	bool tried = false;     //!< resolved (or failed) once per process
	jclass clazz = nullptr; //!< global ref
	jmethodID compute = nullptr;
	jmethodID reset = nullptr;
	jmethodID scalable = nullptr;
};

Bridge &
bridge()
{
	static Bridge b;
	return b;
}

/*!
 * The runtime .so is dlopen'ed by the OpenXR loader, so ART's `FindClass` on a
 * native thread resolves against the SYSTEM classloader and would never see
 * `MiniWindowLayout`. Same dance as android_custom_surface: go through the
 * runtime APK's own DexClassLoader.
 */
bool
resolve(JNIEnv *env)
{
	Bridge &b = bridge();
	if (b.tried) {
		return b.clazz != nullptr;
	}
	b.tried = true;

	void *context = android_globals_get_context();
	if (context == nullptr) {
		U_LOG_W("android_mini_window: no Android Context — mini-window layout hints disabled (#1396)");
		return false;
	}

	try {
		jni::init((JavaVM *)android_globals_get_vm());
		auto loaded =
		    loadClassFromRuntimeApk((jobject)context, "org.freedesktop.monado.auxiliary.MiniWindowLayout");
		if (loaded.isNull()) {
			U_LOG_W(
			    "android_mini_window: could not load MiniWindowLayout from '%s' — "
			    "mini-window layout hints disabled (#1396)",
			    XRT_ANDROID_PACKAGE);
			return false;
		}
		jclass local = (jclass)loaded.object().getHandle();
		b.clazz = (jclass)env->NewGlobalRef(local);
	} catch (std::exception const &e) {
		U_LOG_W("android_mini_window: MiniWindowLayout lookup threw (%s) — hints disabled (#1396)", e.what());
		return false;
	}
	if (b.clazz == nullptr) {
		return false;
	}

	b.compute = env->GetStaticMethodID(b.clazz, "computeHintForActivity", "(Ljava/lang/Object;IIIIII)[I");
	if (b.compute == nullptr) {
		env->ExceptionClear();
	}
	b.reset = env->GetStaticMethodID(b.clazz, "resetForActivity", "()V");
	if (b.reset == nullptr) {
		env->ExceptionClear();
	}
	b.scalable = env->GetStaticMethodID(b.clazz, "isInScalableContainer", "(Ljava/lang/Object;)Z");
	if (b.scalable == nullptr) {
		env->ExceptionClear();
	}
	if (b.compute == nullptr) {
		// An older aux AAR without the method (mismatched runtime install).
		U_LOG_W(
		    "android_mini_window: MiniWindowLayout.computeHintForActivity missing — "
		    "mini-window layout hints disabled (#1396)");
		env->DeleteGlobalRef(b.clazz);
		b.clazz = nullptr;
		return false;
	}
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

	jobject activity = (jobject)android_globals_get_activity();
	jobject result = env->CallStaticObjectMethod(b.clazz, b.compute, activity, (jint)x, (jint)y, (jint)w, (jint)h,
	                                             (jint)disp_w, (jint)disp_h);
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
	jobject activity = (jobject)android_globals_get_activity();
	jboolean ret = env->CallStaticBooleanMethod(b.clazz, b.scalable, activity);
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
