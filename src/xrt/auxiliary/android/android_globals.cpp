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
#include <mutex>
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
