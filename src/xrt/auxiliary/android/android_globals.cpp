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
#include <stdint.h>
#include <atomic>
#include <mutex>
#include <jni.h>
#include <wrap/android.app.h>
#include <android/native_window.h>

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

/*
 * Reference-counting contract (#1040)
 * ------------------------------------
 * `android_globals` OWNS exactly one reference on the window it publishes:
 * every caller of set/store_window TRANSFERS one reference in (the one
 * `ANativeWindow_fromSurface` just acquired), and the globals release the
 * previous one. Consumers that want to keep the window take their OWN
 * reference via android_globals_acquire_window().
 *
 * The old model ("the consumer adopts the single published reference") made
 * the count depend on how many consumers happened to see one publish: an
 * xrEndSession→xrBeginSession cycle builds a second compositor target from the
 * same still-valid publication, so the second teardown released a reference
 * nobody had taken. That dropped the native Surface's strong count to zero
 * while the Java Surface still held its own, and the eventual
 * Surface.finalize() → nativeRelease() ran RefBase::incStrong() on freed
 * memory → SIGSEGV in FinalizerDaemon.
 */
void
android_globals_set_window(struct _ANativeWindow *window)
{
	struct _ANativeWindow *drop = nullptr;
	{
		std::lock_guard<std::mutex> lock(android_surface.mutex);
		// Either the window we are replacing, or — when the same window is
		// re-published (ANativeWindow_fromSurface returns the same pointer for
		// an unchanged Surface, referencing it again) — the caller's duplicate.
		// Both cases drop exactly one reference and leave the globals owning one.
		drop = android_surface.window;
		android_surface.window = window;
		android_surface.valid = (window != nullptr);
		android_surface.generation++;
	}
	if (drop != nullptr) {
		ANativeWindow_release((ANativeWindow *)drop);
	}
}

void
android_globals_clear_window(void)
{
	std::lock_guard<std::mutex> lock(android_surface.mutex);
	// Keep the pointer AND the globals' reference on it — that is what makes
	// the stale pointer safe to compare against until the next publish (#1040).
	// Mark it invalid + bump the generation so the next re-sync tears the
	// surface down.
	android_surface.valid = false;
	android_surface.generation++;
}

struct _ANativeWindow *
android_globals_acquire_window(uint64_t *out_generation, bool *out_valid)
{
	std::lock_guard<std::mutex> lock(android_surface.mutex);
	if (out_generation != nullptr) {
		*out_generation = android_surface.generation;
	}
	if (out_valid != nullptr) {
		*out_valid = android_surface.valid;
	}
	if (android_surface.window == nullptr) {
		return nullptr;
	}
	// Acquired under the same lock that replaces/frees it, so this can never
	// race a concurrent set_window into a use-after-free.
	ANativeWindow_acquire((ANativeWindow *)android_surface.window);
	return android_surface.window;
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
/*!
 * Client window on-screen rect (ADR-036 D6, #1033) — published from the Java
 * `IMonado.updateWindowRect` binder call (UI-thread Choreographer sample in the
 * app process) and read from the compositor render thread, hence its own lock.
 * @ref generation is bumped on every publish so the consumer can skip the
 * vendor call when nothing moved.
 */
static struct
{
	std::mutex mutex;
	int32_t x = 0;
	int32_t y = 0;
	uint32_t w = 0;
	uint32_t h = 0;
	int32_t display_id = -1;
	uint32_t disp_w = 0; //!< panel extent in the CURRENT rotation (#1034)
	uint32_t disp_h = 0;
	uint64_t generation = 0;
	bool have = false;
} android_window_rect;

void
android_globals_set_window_screen_rect(
    int32_t x, int32_t y, uint32_t w, uint32_t h, int32_t display_id, uint32_t disp_w, uint32_t disp_h)
{
	std::lock_guard<std::mutex> lock(android_window_rect.mutex);
	if (android_window_rect.have && android_window_rect.x == x && android_window_rect.y == y &&
	    android_window_rect.w == w && android_window_rect.h == h &&
	    android_window_rect.display_id == display_id && android_window_rect.disp_w == disp_w &&
	    android_window_rect.disp_h == disp_h) {
		return; // unchanged — don't churn the generation
	}
	android_window_rect.x = x;
	android_window_rect.y = y;
	android_window_rect.w = w;
	android_window_rect.h = h;
	android_window_rect.display_id = display_id;
	android_window_rect.disp_w = disp_w;
	android_window_rect.disp_h = disp_h;
	android_window_rect.generation++;
	android_window_rect.have = true;
}

bool
android_globals_get_window_screen_rect(int32_t *out_x,
                                       int32_t *out_y,
                                       uint32_t *out_w,
                                       uint32_t *out_h,
                                       int32_t *out_display_id,
                                       uint32_t *out_disp_w,
                                       uint32_t *out_disp_h,
                                       uint64_t *out_generation)
{
	std::lock_guard<std::mutex> lock(android_window_rect.mutex);
	if (!android_window_rect.have) {
		return false;
	}
	if (out_x != nullptr) {
		*out_x = android_window_rect.x;
	}
	if (out_y != nullptr) {
		*out_y = android_window_rect.y;
	}
	if (out_w != nullptr) {
		*out_w = android_window_rect.w;
	}
	if (out_h != nullptr) {
		*out_h = android_window_rect.h;
	}
	if (out_display_id != nullptr) {
		*out_display_id = android_window_rect.display_id;
	}
	if (out_disp_w != nullptr) {
		*out_disp_w = android_window_rect.disp_w;
	}
	if (out_disp_h != nullptr) {
		*out_disp_h = android_window_rect.disp_h;
	}
	if (out_generation != nullptr) {
		*out_generation = android_window_rect.generation;
	}
	return true;
}

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

/*!
 * Read one boolean `<meta-data>` entry off THIS process's own ApplicationInfo.
 * Returns @p def on any JNI failure, a missing key, or a missing Context — the
 * callers are all "opt-in flag" shaped, so a failure must read as "not set".
 *
 * Factored out of android_globals_self_declares_overlay() for the #1031
 * hybrid-mode `com.displayxr.force_ipc` read; the JNI shape is unchanged.
 * Not cached here — each caller caches its own key.
 */
static bool
read_self_bool_meta_data(const char *name, bool def, void *context)
{
	int result = def ? 1 : 0;
	jobject ctx = (jobject)context;
	if (ctx != nullptr) {
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
						jstring key = env->NewStringUTF(name);
						result = env->CallBooleanMethod(bundle, mGetBool, key,
						                                def ? JNI_TRUE : JNI_FALSE)
						             ? 1
						             : 0;
					}
				}
			}
			if (env->ExceptionCheck()) {
				env->ExceptionClear();
			}
		}
	}
	return result != 0;
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
	int result = read_self_bool_meta_data("com.displayxr.overlay_mode", false, android_globals_get_context()) ? 1 : 0;
	cached.store(result, std::memory_order_release);
	return result != 0;
}

bool
android_globals_self_declares_force_ipc(struct _JavaVM *vm, void *context)
{
	// #1031 hybrid mode. Called once, from xrt_instance_create — before any of
	// the usual per-session setup — so the VM and Context come in from
	// xrt_instance_info.platform_info, with the globals as a fallback for
	// clients that stored them some other way (a JNI_OnLoad host, the CLI).
	if (vm == nullptr) {
		vm = android_globals_get_vm();
	}
	if (context == nullptr) {
		context = android_globals_get_context();
	}
	if (vm == nullptr || context == nullptr) {
		return false;
	}
	jni::init(vm);
	return read_self_bool_meta_data("com.displayxr.force_ipc", false, context);
}
