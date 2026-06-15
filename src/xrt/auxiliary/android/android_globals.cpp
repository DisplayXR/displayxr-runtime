// Copyright 2020, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Functions for Android-specific global state.
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @ingroup aux_android
 */

#include "android_globals.h"

#include <stddef.h>
#include <atomic>
#include <mutex>
#include <jni.h>
#include <wrap/android.app.h>

/*!
 * @todo Do we need locking here?
 */
static struct
{
	struct _JavaVM *vm = nullptr;
	jni::Object activity = {};
	jni::Object context = {};
	struct _ANativeWindow *window = nullptr;
} android_globals;

/*!
 * Versioned surface state, mutated from the Java SurfaceView callbacks
 * (surfaceChanged / surfaceDestroyed on the UI thread) and read from the
 * compositor render thread — hence its own lock, separate from the (unlocked)
 * legacy globals above. @ref generation is bumped on every publish/clear so the
 * consumer can tell a brand-new surface (resume) from the one it already built.
 */
static struct
{
	std::mutex mutex;
	struct _ANativeWindow *window = nullptr;
	uint64_t generation = 0;
	bool valid = false;
	//! Active android_custom_surface (opaque), polled by oxr_session_poll. #507
	void *custom_surface = nullptr;
} android_surface;

void
android_globals_store_vm_and_activity(struct _JavaVM *vm, void *activity)
{
	jni::init(vm);
	android_globals.vm = vm;
	android_globals.activity = jni::Object((jobject)activity);
}

void
android_globals_store_vm_and_context(struct _JavaVM *vm, void *context)
{
	jni::init(vm);
	android_globals.vm = vm;
	android_globals.context = jni::Object((jobject)context);
	if (android_globals_is_instance_of_activity(vm, context)) {
		android_globals.activity = jni::Object((jobject)context);
	}
}

bool
android_globals_is_instance_of_activity(struct _JavaVM *vm, void *obj)
{
	jni::init(vm);

	auto activity_cls = jni::Class(wrap::android::app::Activity::getTypeName());
	return JNI_TRUE == jni::env()->IsInstanceOf((jobject)obj, activity_cls.getHandle());
}

void
android_globals_store_window(struct _ANativeWindow *window)
{
	android_globals.window = window;
	// Keep the versioned view in sync for legacy callers (IPC service path
	// seeds the window via this entry point) so the compositor's surface
	// re-sync sees a valid initial surface too.
	android_globals_set_window(window);
}

struct _ANativeWindow *
android_globals_get_window()
{
	return android_globals.window;
}

void
android_globals_set_window(struct _ANativeWindow *window)
{
	std::lock_guard<std::mutex> lock(android_surface.mutex);
	android_surface.window = window;
	android_surface.valid = (window != nullptr);
	android_surface.generation++;
}

void
android_globals_clear_window(void)
{
	std::lock_guard<std::mutex> lock(android_surface.mutex);
	// Keep the pointer (consumer releases it after idling the GPU) but mark it
	// invalid + bump the generation so the next re-sync tears the surface down.
	android_surface.valid = false;
	android_surface.generation++;
}

void
android_globals_get_window_state(struct _ANativeWindow **out_window, uint64_t *out_generation, bool *out_valid)
{
	std::lock_guard<std::mutex> lock(android_surface.mutex);
	if (out_window != nullptr) {
		*out_window = android_surface.window;
	}
	if (out_generation != nullptr) {
		*out_generation = android_surface.generation;
	}
	if (out_valid != nullptr) {
		*out_valid = android_surface.valid;
	}
}

void
android_globals_set_custom_surface(void *custom_surface)
{
	std::lock_guard<std::mutex> lock(android_surface.mutex);
	android_surface.custom_surface = custom_surface;
}

void *
android_globals_get_custom_surface(void)
{
	std::lock_guard<std::mutex> lock(android_surface.mutex);
	return android_surface.custom_surface;
}

struct _JavaVM *
android_globals_get_vm()
{
	return android_globals.vm;
}

void *
android_globals_get_activity()
{
	return android_globals.activity.getHandle();
}

void *
android_globals_get_context()
{
	return android_globals.context.isNull() ? android_globals.activity.getHandle()
	                                        : android_globals.context.getHandle();
}

// #558 per-app overlay mode: service-process flag set by MonadoImpl from the
// connecting client's manifest, read by the vendor DP plug-in.
static std::atomic<bool> android_overlay_mode{false};

void
android_globals_set_overlay_mode(bool enabled)
{
	android_overlay_mode.store(enabled, std::memory_order_release);
}

bool
android_globals_get_overlay_mode(void)
{
	return android_overlay_mode.load(std::memory_order_acquire);
}

bool
android_globals_self_declares_overlay(void)
{
	// Query this process's own package manifest metadata once; cache the result
	// (-1 uncomputed, 0 no, 1 yes). Used in the APP process where the connecting
	// app is this process (e.g. oxr_session keep-alive).
	static std::atomic<int> cached{-1};
	int c = cached.load(std::memory_order_acquire);
	if (c >= 0) {
		return c != 0;
	}
	int result = 0; // default false, also on any JNI failure
	jobject ctx = (jobject)android_globals_get_context();
	if (android_globals.vm != nullptr && ctx != nullptr) {
		JNIEnv *env = jni::env();
		if (env != nullptr) {
			jclass ctxCls = env->GetObjectClass(ctx);
			jmethodID mGetPkgName = env->GetMethodID(ctxCls, "getPackageName", "()Ljava/lang/String;");
			jmethodID mGetPM =
			    env->GetMethodID(ctxCls, "getPackageManager", "()Landroid/content/pm/PackageManager;");
			jstring pkg = (jstring)env->CallObjectMethod(ctx, mGetPkgName);
			jobject pm = env->CallObjectMethod(ctx, mGetPM);
			if (pkg != nullptr && pm != nullptr) {
				jclass pmCls = env->GetObjectClass(pm);
				jmethodID mGetAppInfo = env->GetMethodID(
				    pmCls, "getApplicationInfo",
				    "(Ljava/lang/String;I)Landroid/content/pm/ApplicationInfo;");
				const jint GET_META_DATA = 0x00000080;
				jobject ai = env->CallObjectMethod(pm, mGetAppInfo, pkg, GET_META_DATA);
				if (env->ExceptionCheck()) {
					env->ExceptionClear();
				} else if (ai != nullptr) {
					jclass aiCls = env->GetObjectClass(ai);
					jfieldID fMeta = env->GetFieldID(aiCls, "metaData", "Landroid/os/Bundle;");
					jobject bundle = env->GetObjectField(ai, fMeta);
					if (bundle != nullptr) {
						jclass bCls = env->GetObjectClass(bundle);
						jmethodID mGetBool =
						    env->GetMethodID(bCls, "getBoolean", "(Ljava/lang/String;Z)Z");
						jstring key = env->NewStringUTF("com.displayxr.overlay_mode");
						result = env->CallBooleanMethod(bundle, mGetBool, key, JNI_FALSE) ? 1 : 0;
					}
				}
			}
			if (env->ExceptionCheck()) {
				env->ExceptionClear();
			}
		}
	}
	cached.store(result, std::memory_order_release);
	return result != 0;
}
