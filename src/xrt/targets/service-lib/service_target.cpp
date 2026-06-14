// Copyright 2020, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Library exposing IPC server.
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @ingroup ipc_android
 */

#include "jnipp.h"
#include "jni.h"

#include "wrap/android.view.h"

#include "server/ipc_server.h"
#include "server/ipc_server_interface.h"
#include "server/ipc_server_mainloop_android.h"
#include "util/u_logging.h"

#include <android/native_window.h>
#include <android/native_window_jni.h>

#include "android/android_globals.h"
#include "android/android_custom_surface.h"
#include "android/android_main_thread.h"

#include <chrono>
#include <memory>
#include <thread>

using wrap::android::view::Surface;
using namespace std::chrono_literals;

namespace {
struct IpcServerHelper
{
public:
	static IpcServerHelper &
	instance()
	{
		static IpcServerHelper instance;
		return instance;
	}

	void
	signalStartupComplete()
	{
		std::unique_lock<std::mutex> lock{server_mutex};
		startup_complete = true;
		startup_cond.notify_all();
	}

	void
	startServer()
	{
		std::unique_lock lock(server_mutex);
		if (!server && !server_thread) {
			server_thread = std::make_unique<std::thread>(
			    [&]() { ipc_server_main_android(&server, signalStartupCompleteTrampoline, this); });
		}
	}

	static void
	signalStartupCompleteTrampoline(void *data)
	{
		static_cast<IpcServerHelper *>(data)->signalStartupComplete();
	}

	int32_t
	addClient(int fd)
	{
		if (!waitForStartupComplete()) {
			return -1;
		}
		return ipc_server_mainloop_add_fd(server, &server->ml, fd);
	}

	int32_t
	shutdownServer()
	{
		if (!server || !server_thread) {
			// Should not happen.
			U_LOG_E("service: shutdownServer called before server started up!");
			return -1;
		}

		{
			// Wait until IPC server stop
			std::unique_lock lock(server_mutex);
			ipc_server_handle_shutdown_signal(server);
			server_thread->join();
			server_thread.reset(nullptr);
			server = NULL;
			startup_complete = false;
		}

		return 0;
	}

private:
	IpcServerHelper() {}

	bool
	waitForStartupComplete()
	{
		std::unique_lock<std::mutex> lock{server_mutex};
		bool completed = startup_cond.wait_for(lock, START_TIMEOUT_SECONDS, [&]() { return startup_complete; });

		if (!server) {
			U_LOG_E("Failed to create ipc server");
		}

		if (!completed) {
			U_LOG_E("Server startup timeout!");
		}
		return server && completed;
	}

	//! Reference to the ipc_server, managed by ipc_server_process
	struct ipc_server *server = NULL;

	//! Mutex for starting thread
	std::mutex server_mutex;

	//! Server thread
	std::unique_ptr<std::thread> server_thread{};

	//! Condition variable for starting thread
	std::condition_variable startup_cond;

	//! Server startup state
	bool startup_complete = false;

	//! Timeout duration in seconds
	static constexpr std::chrono::seconds START_TIMEOUT_SECONDS = 40s;
};
} // namespace

extern "C" JNIEXPORT void JNICALL
Java_org_freedesktop_monado_ipc_MonadoImpl_nativeStartServer(JNIEnv *env, jobject thiz, jobject context)
{
	JavaVM *jvm = nullptr;
	jint result = env->GetJavaVM(&jvm);
	assert(result == JNI_OK);
	assert(jvm);

	jni::init(env);
	jni::Object monadoImpl(thiz);
	U_LOG_D("service: Called nativeStartServer");

	android_globals_store_vm_and_context(jvm, context);

	// nativeStartServer runs on the service main thread (Service.onCreate → MonadoImpl
	// ctor). Capture the main Looper here so vendor display-processor init (e.g. the
	// Leia CNSDK) can be marshaled onto it from the comp_multi render worker — that
	// init must be kicked off from a Looper-bearing thread or it hangs (#510 M2).
	android_main_thread_dispatch_init();

	IpcServerHelper::instance().startServer();
}

extern "C" JNIEXPORT jint JNICALL
Java_org_freedesktop_monado_ipc_MonadoImpl_nativeAddClient(JNIEnv *env, jobject thiz, int fd)
{
	jni::init(env);
	jni::Object monadoImpl(thiz);
	U_LOG_D("service: Called nativeAddClient with fd %d", fd);

	int native_fd = dup(fd);
	U_LOG_D("service: transfer ownership to native and native_fd %d", native_fd);

	// We try pushing the fd number to the server. If and only if we get a 0 return, has the server taken ownership.
	return IpcServerHelper::instance().addClient(native_fd);
}

extern "C" JNIEXPORT void JNICALL
Java_org_freedesktop_monado_ipc_MonadoImpl_nativeAppSurface(JNIEnv *env, jobject thiz, jobject surface)
{
	jni::init(env);
	Surface surf(surface);
	jni::Object monadoImpl(thiz);

	ANativeWindow *nativeWindow = ANativeWindow_fromSurface(env, surface);
	android_globals_store_window((struct _ANativeWindow *)nativeWindow);
	U_LOG_I("service: app surface published: ANativeWindow %p (#528)", (void *)nativeWindow);
}

extern "C" JNIEXPORT void JNICALL
Java_org_freedesktop_monado_ipc_MonadoImpl_nativeCreateServiceOverlay(JNIEnv *env, jobject thiz)
{
	jni::init(env);
	jni::Object monadoImpl(thiz);

	// Service-owned overlay (#558 revival, P1): when the runtime holds the
	// "display over other apps" permission the CLIENT skips publishing its
	// surface (Client.java gate), so the SERVICE must create the on-screen
	// surface itself. Reuse the existing C surface machinery with the SERVICE
	// context (a non-Activity Context) — android_custom_surface picks
	// TYPE_APPLICATION_OVERLAY for it, so the weave floats over the live
	// launcher (launcher stays the resumed, interactive top app) rather than a
	// full-screen client Activity that pauses it. debug.dxr.transparent makes
	// the overlay TRANSLUCENT so the desktop shows through. Idempotent: a second
	// client connect is a no-op while an overlay already exists.
	if (android_globals_get_custom_surface() != nullptr) {
		U_LOG_I("service: overlay already created — skipping");
		return;
	}

	struct _JavaVM *vm = android_globals_get_vm();
	void *context = android_globals_get_context();
	if (vm == nullptr || context == nullptr) {
		U_LOG_E("service: cannot create overlay — vm=%p context=%p", (void *)vm, context);
		return;
	}

	struct android_custom_surface *cs =
	    android_custom_surface_async_start(vm, context, /*display_id*/ 0, "DisplayXR",
	                                       /*preferred_display_mode_id*/ 0);
	if (cs == nullptr) {
		U_LOG_E("service: android_custom_surface_async_start failed (overlay mode)");
		return;
	}

	ANativeWindow *win = android_custom_surface_wait_get_surface(cs, /*timeout_ms*/ 5000);
	if (win == nullptr) {
		U_LOG_E("service: overlay surfaceCreated never fired within 5 s");
		android_custom_surface_destroy(&cs);
		return;
	}

	android_globals_store_window((struct _ANativeWindow *)win);
	// Keep the custom surface alive + discoverable (process-scoped, like the
	// in-process path) so the surface stays published for the compositor.
	android_globals_set_custom_surface(cs);
	U_LOG_I("service: TYPE_APPLICATION_OVERLAY created, ANativeWindow %p (#558 P1)", (void *)win);
}

extern "C" JNIEXPORT void JNICALL
Java_org_freedesktop_monado_ipc_MonadoImpl_nativeClearAppSurface(JNIEnv *env, jobject thiz)
{
	jni::init(env);
	jni::Object monadoImpl(thiz);

	// Mark the published window invalid + bump the generation; the compositor's
	// per-acquire surface sync tears its VkSurfaceKHR down and pauses presents
	// until the client passes the replacement surface (#528).
	android_globals_clear_window();
	U_LOG_I("service: app surface cleared (#528)");
}

extern "C" JNIEXPORT jint JNICALL
Java_org_freedesktop_monado_ipc_MonadoImpl_nativeShutdownServer(JNIEnv *env, jobject thiz)
{
	jni::init(env);
	jni::Object monadoImpl(thiz);

	return IpcServerHelper::instance().shutdownServer();
}
