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
#include "server/ipc_server_stereo_camera.h"
#include "util/u_camera_consent.h"
#include "util/u_camera_consent_android.h"
#include "util/u_logging.h"

#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <sys/system_properties.h>

#include "android/android_globals.h"
#include "android/android_custom_surface.h"
#include "android/android_main_thread.h"

#include <chrono>
#include <string>
#include <memory>
#include <thread>

using wrap::android::view::Surface;
using namespace std::chrono_literals;

namespace {

/*!
 * #1006 (ADR-036 D4): the Java half of the Android workspace overlay mode,
 * org.freedesktop.monado.ipc.WorkspaceControllerPolicy. Resolved ONCE on the
 * service main thread (nativeStartServer): FindClass on an IPC pthread would
 * use the system class loader and miss the runtime's own classes.
 */
struct WorkspacePolicyJni
{
	JavaVM *vm = nullptr;
	jclass cls = nullptr;            //!< global ref
	jmethodID is_verified = nullptr; //!< static boolean isVerifiedController(Context, int)
	jmethodID on_overlay = nullptr;  //!< static void onOverlayChanged(boolean, int)
	jobject context = nullptr;       //!< global ref to the service context
} g_ws_policy;

//! Attach the calling thread if needed; *attached says whether to detach after.
JNIEnv *
ws_policy_env(bool *attached)
{
	*attached = false;
	if (g_ws_policy.vm == nullptr) {
		return nullptr;
	}
	JNIEnv *env = nullptr;
	if (g_ws_policy.vm->GetEnv((void **)&env, JNI_VERSION_1_6) == JNI_OK) {
		return env;
	}
	if (g_ws_policy.vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
		return nullptr;
	}
	*attached = true;
	return env;
}

bool
ws_policy_verify_controller(long peer_uid)
{
	if (g_ws_policy.cls == nullptr || g_ws_policy.is_verified == nullptr) {
		return false;
	}
	bool attached = false;
	JNIEnv *env = ws_policy_env(&attached);
	if (env == nullptr) {
		return false;
	}
	jboolean ok = env->CallStaticBooleanMethod(g_ws_policy.cls, g_ws_policy.is_verified, g_ws_policy.context,
	                                           (jint)peer_uid);
	if (env->ExceptionCheck()) {
		env->ExceptionDescribe();
		env->ExceptionClear();
		ok = JNI_FALSE;
	}
	if (attached) {
		g_ws_policy.vm->DetachCurrentThread();
	}
	return ok == JNI_TRUE;
}

void
ws_policy_overlay_changed(bool active, long controller_uid)
{
	if (g_ws_policy.cls == nullptr || g_ws_policy.on_overlay == nullptr) {
		return;
	}
	bool attached = false;
	JNIEnv *env = ws_policy_env(&attached);
	if (env == nullptr) {
		return;
	}
	env->CallStaticVoidMethod(g_ws_policy.cls, g_ws_policy.on_overlay, (jboolean)(active ? JNI_TRUE : JNI_FALSE),
	                          (jint)controller_uid);
	if (env->ExceptionCheck()) {
		env->ExceptionDescribe();
		env->ExceptionClear();
	}
	if (attached) {
		g_ws_policy.vm->DetachCurrentThread();
	}
}

void
ws_policy_init(JNIEnv *env, JavaVM *vm, jobject context)
{
	if (g_ws_policy.cls != nullptr) {
		return;
	}
	jclass local = env->FindClass("org/freedesktop/monado/ipc/WorkspaceControllerPolicy");
	if (local == nullptr || env->ExceptionCheck()) {
		env->ExceptionClear();
		U_LOG_E("service: WorkspaceControllerPolicy not found - Android workspace controllers cannot verify");
		return;
	}
	g_ws_policy.vm = vm;
	g_ws_policy.cls = (jclass)env->NewGlobalRef(local);
	env->DeleteLocalRef(local);
	g_ws_policy.is_verified =
	    env->GetStaticMethodID(g_ws_policy.cls, "isVerifiedController", "(Landroid/content/Context;I)Z");
	g_ws_policy.on_overlay = env->GetStaticMethodID(g_ws_policy.cls, "onOverlayChanged", "(ZI)V");
	if (env->ExceptionCheck()) {
		env->ExceptionClear();
		U_LOG_E("service: WorkspaceControllerPolicy methods not found");
		g_ws_policy.is_verified = nullptr;
		g_ws_policy.on_overlay = nullptr;
		return;
	}
	g_ws_policy.context = env->NewGlobalRef(context);
	ipc_server_set_android_controller_verify_provider(ws_policy_verify_controller);
	ipc_server_set_android_overlay_hook(ws_policy_overlay_changed);
	U_LOG_I("service: Android workspace-controller policy registered (#1006)");
}

/*
 *
 * XR_DXR_stereo_camera A2 (stereo-camera-android §4): the Android consent
 * platform — identity (package + signing certificate of the peer uid), the
 * peer's and the runtime's CAMERA permission, the consent dialog, the in-use
 * notification with Stop, and the screen-lock watch. The Java half is
 * org.freedesktop.monado.ipc.StereoCameraConsent; resolved once on the main
 * thread like the workspace policy above.
 *
 */
struct CameraConsentJni
{
	JavaVM *vm = nullptr;
	jclass cls = nullptr;
	jobject context = nullptr;
	jmethodID peer_identity = nullptr;  //!< String peerIdentity(Context, int)
	jmethodID peer_camera = nullptr;    //!< boolean peerCameraAllowed(Context, String)
	jmethodID ensure_runtime = nullptr; //!< boolean ensureRuntimeCamera(Context, int)
	jmethodID prompt = nullptr;         //!< int prompt(Context, String, String, int)
	jmethodID in_use = nullptr;         //!< void inUseChanged(Context, int, String)
	jmethodID has_cert = nullptr;       //!< boolean hasSigningCertificate(Context, String, String)
} g_cam;

JNIEnv *
cam_env(bool *attached)
{
	*attached = false;
	if (g_cam.vm == nullptr) {
		return nullptr;
	}
	JNIEnv *env = nullptr;
	if (g_cam.vm->GetEnv((void **)&env, JNI_VERSION_1_6) == JNI_OK) {
		return env;
	}
	if (g_cam.vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
		return nullptr;
	}
	*attached = true;
	return env;
}

void
cam_env_done(JNIEnv *env, bool attached)
{
	if (env->ExceptionCheck()) {
		env->ExceptionDescribe();
		env->ExceptionClear();
	}
	if (attached) {
		g_cam.vm->DetachCurrentThread();
	}
}

bool
cam_peer_subject(void * /*ctx*/, long pid, long uid, char *out, size_t cap)
{
	out[0] = '\0';
	bool attached = false;
	JNIEnv *env = cam_env(&attached);
	if (env == nullptr || g_cam.peer_identity == nullptr) {
		return false;
	}
	jstring js = (jstring)env->CallStaticObjectMethod(g_cam.cls, g_cam.peer_identity, g_cam.context, (jint)uid);
	std::string id;
	if (!env->ExceptionCheck() && js != nullptr) {
		const char *c = env->GetStringUTFChars(js, nullptr);
		if (c != nullptr) {
			id = c;
			env->ReleaseStringUTFChars(js, c);
		}
		env->DeleteLocalRef(js);
	}
	cam_env_done(env, attached);

	std::string pkg, digest;
	size_t tab = id.find('\t');
	if (tab != std::string::npos) {
		pkg = id.substr(0, tab);
		digest = id.substr(tab + 1);
	}
	uint32_t n = 0;
	const struct u_camera_consent_android_trusted *list = u_camera_consent_android_builtin_trusted(&n);
	// A trusted package whose CURRENT certificate is not the listed one may have
	// rotated its key (APK v3 lineage): it keeps its trust iff the listed
	// certificate is in its signing history; the subject then names the listed one.
	if (!pkg.empty() && !digest.empty() && g_cam.has_cert != nullptr &&
	    u_camera_consent_android_trust_lookup(list, n, pkg.c_str(), digest.c_str(), nullptr) ==
	        U_CAMERA_CONSENT_ANDROID_PACKAGE_ONLY) {
		bool att = false;
		JNIEnv *e2 = cam_env(&att);
		for (uint32_t i = 0; e2 != nullptr && i < n; i++) {
			if (pkg != list[i].package) {
				continue;
			}
			char want[U_CAMERA_CONSENT_ANDROID_DIGEST_SIZE];
			if (!u_camera_consent_android_digest_normalize(list[i].cert_sha256, want)) {
				continue;
			}
			jstring jp = e2->NewStringUTF(pkg.c_str());
			jstring jw = e2->NewStringUTF(want);
			jboolean has = e2->CallStaticBooleanMethod(g_cam.cls, g_cam.has_cert, g_cam.context, jp, jw);
			if (e2->ExceptionCheck()) {
				e2->ExceptionClear();
				has = JNI_FALSE;
			}
			e2->DeleteLocalRef(jp);
			e2->DeleteLocalRef(jw);
			if (has == JNI_TRUE) {
				U_LOG_W(
				    "stereo camera consent: %s rotated its signing key; the listed certificate is in "
				    "its "
				    "lineage — trusted",
				    pkg.c_str());
				digest = want;
				break;
			}
		}
		if (e2 != nullptr) {
			cam_env_done(e2, att);
		}
	}
	enum u_camera_consent_android_identity_result r =
	    u_camera_consent_android_identity(list, n, pkg.c_str(), digest.c_str(), out, cap);
	if (r != U_CAMERA_CONSENT_ANDROID_ID_OK) {
		U_LOG_W("stereo camera consent: pid %ld uid %ld (%s) has no verifiable identity — %s", pid, uid,
		        pkg.empty() ? "?" : pkg.c_str(), u_camera_consent_android_identity_str(r));
		return false;
	}
	return true;
}

bool
cam_os_camera_allowed(void * /*ctx*/, const char *subject)
{
	bool attached = false;
	JNIEnv *env = cam_env(&attached);
	if (env == nullptr || g_cam.peer_camera == nullptr) {
		return false;
	}
	jstring js = env->NewStringUTF(subject);
	jboolean ok = env->CallStaticBooleanMethod(g_cam.cls, g_cam.peer_camera, g_cam.context, js);
	if (env->ExceptionCheck()) {
		ok = JNI_FALSE;
	}
	env->DeleteLocalRef(js);
	cam_env_done(env, attached);
	return ok == JNI_TRUE;
}

bool
cam_ensure_service_camera(void * /*ctx*/, uint32_t timeout_ms)
{
	bool attached = false;
	JNIEnv *env = cam_env(&attached);
	if (env == nullptr || g_cam.ensure_runtime == nullptr) {
		return false;
	}
	jboolean ok = env->CallStaticBooleanMethod(g_cam.cls, g_cam.ensure_runtime, g_cam.context, (jint)timeout_ms);
	if (env->ExceptionCheck()) {
		ok = JNI_FALSE;
	}
	cam_env_done(env, attached);
	return ok == JNI_TRUE;
}

enum u_camera_consent_prompt_answer
cam_prompt(void * /*ctx*/, const char *subject, const char *app_name, long /*pid*/, uint32_t timeout_ms)
{
	bool attached = false;
	JNIEnv *env = cam_env(&attached);
	if (env == nullptr || g_cam.prompt == nullptr) {
		return U_CAMERA_CONSENT_PROMPT_UNAVAILABLE;
	}
	jstring js = env->NewStringUTF(subject);
	jstring jn = env->NewStringUTF(app_name != nullptr ? app_name : "");
	jint r = env->CallStaticIntMethod(g_cam.cls, g_cam.prompt, g_cam.context, js, jn, (jint)timeout_ms);
	if (env->ExceptionCheck()) {
		r = U_CAMERA_CONSENT_PROMPT_UNAVAILABLE;
	}
	env->DeleteLocalRef(js);
	env->DeleteLocalRef(jn);
	cam_env_done(env, attached);
	switch (r) {
	case U_CAMERA_CONSENT_PROMPT_ALLOW: return U_CAMERA_CONSENT_PROMPT_ALLOW;
	case U_CAMERA_CONSENT_PROMPT_ALLOW_ONCE: return U_CAMERA_CONSENT_PROMPT_ALLOW_ONCE;
	case U_CAMERA_CONSENT_PROMPT_DENY: return U_CAMERA_CONSENT_PROMPT_DENY;
	case U_CAMERA_CONSENT_PROMPT_TIMEOUT: return U_CAMERA_CONSENT_PROMPT_TIMEOUT;
	default: return U_CAMERA_CONSENT_PROMPT_UNAVAILABLE;
	}
}

//! The in-use state may have changed: refresh the notification (no manager lock held).
void
cam_indicator(void * /*ctx*/)
{
	struct ipc_server_stereo_camera_status st;
	if (!ipc_server_stereo_camera_get_status(&st)) {
		return;
	}
	bool attached = false;
	JNIEnv *env = cam_env(&attached);
	if (env == nullptr || g_cam.in_use == nullptr) {
		return;
	}
	jstring jc = env->NewStringUTF(st.consumers);
	env->CallStaticVoidMethod(g_cam.cls, g_cam.in_use, g_cam.context, (jint)st.started_streams, jc);
	env->DeleteLocalRef(jc);
	cam_env_done(env, attached);
}

void
cam_consent_init(JNIEnv *env, JavaVM *vm, jobject context)
{
	if (g_cam.cls != nullptr) {
		return;
	}
	jclass local = env->FindClass("org/freedesktop/monado/ipc/StereoCameraConsent");
	if (local == nullptr || env->ExceptionCheck()) {
		env->ExceptionClear();
		U_LOG_E("service: StereoCameraConsent not found — every stereo camera consumer will be refused");
		return;
	}
	g_cam.vm = vm;
	g_cam.cls = (jclass)env->NewGlobalRef(local);
	env->DeleteLocalRef(local);
	jmethodID store_dir =
	    env->GetStaticMethodID(g_cam.cls, "storeDir", "(Landroid/content/Context;)Ljava/lang/String;");
	g_cam.peer_identity =
	    env->GetStaticMethodID(g_cam.cls, "peerIdentity", "(Landroid/content/Context;I)Ljava/lang/String;");
	g_cam.peer_camera =
	    env->GetStaticMethodID(g_cam.cls, "peerCameraAllowed", "(Landroid/content/Context;Ljava/lang/String;)Z");
	g_cam.ensure_runtime =
	    env->GetStaticMethodID(g_cam.cls, "ensureRuntimeCamera", "(Landroid/content/Context;I)Z");
	g_cam.prompt = env->GetStaticMethodID(g_cam.cls, "prompt",
	                                      "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;I)I");
	g_cam.in_use =
	    env->GetStaticMethodID(g_cam.cls, "inUseChanged", "(Landroid/content/Context;ILjava/lang/String;)V");
	g_cam.has_cert = env->GetStaticMethodID(g_cam.cls, "hasSigningCertificate",
	                                        "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)Z");
	jmethodID start_watching = env->GetStaticMethodID(g_cam.cls, "startWatching", "(Landroid/content/Context;)V");
	if (env->ExceptionCheck() || store_dir == nullptr || start_watching == nullptr) {
		env->ExceptionClear();
		U_LOG_E(
		    "service: StereoCameraConsent methods not found — every stereo camera consumer will be refused");
		g_cam.peer_identity = g_cam.peer_camera = g_cam.ensure_runtime = g_cam.prompt = g_cam.in_use = nullptr;
		return;
	}
	g_cam.context = env->NewGlobalRef(context);

	// The JSON store lives in the runtime's private files dir (no $HOME here).
	jstring jd = (jstring)env->CallStaticObjectMethod(g_cam.cls, store_dir, g_cam.context);
	if (!env->ExceptionCheck() && jd != nullptr) {
		const char *d = env->GetStringUTFChars(jd, nullptr);
		if (d != nullptr) {
			u_camera_consent_store_set_user_dir(d);
			env->ReleaseStringUTFChars(jd, d);
		}
		env->DeleteLocalRef(jd);
	}
	env->ExceptionClear();

	struct ipc_server_stereo_camera_platform p = {};
	p.peer_subject = cam_peer_subject;
	p.os_camera_allowed = cam_os_camera_allowed;
	p.prompt = cam_prompt;
	p.ensure_service_camera = cam_ensure_service_camera;
	p.user_facing_requires_presenting = true; // option R (stereo-camera-android §1.2)
	p.gave_up_is_suspended = true;            // the OS takes the camera in the background
	ipc_server_stereo_camera_set_platform(&p);
	ipc_server_stereo_camera_set_indicator_provider(cam_indicator, nullptr);

	env->CallStaticVoidMethod(g_cam.cls, start_watching, g_cam.context);
	env->ExceptionClear();
	U_LOG_I("service: Android stereo camera consent platform registered (A2)");
}

} // namespace

extern "C" JNIEXPORT void JNICALL
Java_org_freedesktop_monado_ipc_StereoCameraConsent_nativeSetLocked(JNIEnv *env, jclass, jboolean locked, jstring why)
{
	const char *w = why != nullptr ? env->GetStringUTFChars(why, nullptr) : nullptr;
	ipc_server_stereo_camera_set_session_locked(locked == JNI_TRUE, w != nullptr ? w : "android");
	if (w != nullptr) {
		env->ReleaseStringUTFChars(why, w);
	}
}

extern "C" JNIEXPORT void JNICALL
Java_org_freedesktop_monado_ipc_StereoCameraConsent_nativeStopAll(JNIEnv *env, jclass, jstring why)
{
	const char *w = why != nullptr ? env->GetStringUTFChars(why, nullptr) : nullptr;
	(void)ipc_server_stereo_camera_stop_all(w != nullptr ? w : "android");
	if (w != nullptr) {
		env->ReleaseStringUTFChars(why, w);
	}
}

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

	// #1006: workspace-controller verification + overlay hook, resolved here on
	// the main thread (the runtime's class loader is the current one).
	ws_policy_init(env, jvm, context);
	// A2: stereo camera consent platform (before the server creates its camera manager).
	cam_consent_init(env, jvm, context);

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

	// The overlay-mode gate now lives in MonadoImpl.canDrawOverOtherApps (per-app,
	// from the connecting client's manifest); connect() only calls this in overlay
	// mode, so no sysprop gate here — that would wrongly block a per-app overlay
	// app whenever debug.dxr.overlay is unset (#558 per-app).

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
		// #558 stale-overlay-stall fix: the service overlay persists across client
		// (avatar) restarts — its MonadoView + surface stay attached to the service
		// window — but the PRIOR client's compositor released its ANativeWindow ref
		// and the published window was left stale/invalid, so a NEW client's
		// comp_window_android_init_swapchain stalls polling for a valid window
		// (previously only a runtime reinstall, which kills the service, cleared it).
		// Re-publish a fresh ANativeWindow ref from the live overlay surface for the
		// new client instead of skipping.
		struct android_custom_surface *cs =
		    (struct android_custom_surface *)android_globals_get_custom_surface();
		android_custom_surface_refresh_window(cs);
		U_LOG_I("service: overlay reused — refreshed window for new client (#558)");
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
	                                       /*preferred_display_mode_id*/ 0, false);
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

// #558: the debug.dxr.overlay sysprop is now a DEV FORCE-ALL override (forces
// every client into overlay mode). The normal per-app decision is in
// MonadoImpl.canDrawOverOtherApps (the connecting app's manifest flag).
extern "C" JNIEXPORT jboolean JNICALL
Java_org_freedesktop_monado_ipc_MonadoImpl_nativeOverlayModeEnabled(JNIEnv * /*env*/,
                                                                    jobject /*thiz*/)
{
	char prop[PROP_VALUE_MAX] = {};
	return (__system_property_get("debug.dxr.overlay", prop) > 0 && prop[0] == '1') ? JNI_TRUE
	                                                                                 : JNI_FALSE;
}

// #558 per-app: publish the per-session overlay decision to the service-process
// globals so the compositor + vendor DP plug-in see it (keep-3D-while-bg) without
// re-reading a global sysprop. Set by MonadoImpl.connect from the client's manifest.
extern "C" JNIEXPORT void JNICALL
Java_org_freedesktop_monado_ipc_MonadoImpl_nativeSetOverlayMode(JNIEnv * /*env*/, jobject /*thiz*/,
                                                                jboolean enabled)
{
	android_globals_set_overlay_mode(enabled == JNI_TRUE);
}

// #558: authoritative teardown of the service-owned overlay, called from
// MonadoImpl.shutdown() (← MonadoService.onDestroy) when the runtime service is
// going away. Runs on the service main thread (JNI-attached), so it reliably
// removes the MonadoView even when the IPC last-client-disconnect teardown races
// the Android service-shutdown lifecycle (onUnbind/onPrepareShutdown), which on
// some ROMs orphaned the overlay and left a frozen frame on the launcher.
// Idempotent: a no-op once the surface is gone (e.g. the IPC path got there first).
extern "C" JNIEXPORT void JNICALL
Java_org_freedesktop_monado_ipc_MonadoImpl_nativeDestroyServiceOverlay(JNIEnv *env, jobject thiz)
{
	jni::init(env);
	struct android_custom_surface *cs =
	    (struct android_custom_surface *)android_globals_get_custom_surface();
	if (cs == nullptr) {
		return;
	}
	android_globals_set_custom_surface(nullptr);
	android_globals_clear_window();
	android_custom_surface_destroy(&cs);
	U_LOG_I("service: TYPE_APPLICATION_OVERLAY destroyed on shutdown (#558)");
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

/*!
 * Publish the client window's on-screen rect (ADR-036 D6, #1033).
 *
 * The app process samples `View.getLocationOnScreen()` + the SurfaceView extent
 * from a Choreographer callback and pushes changes over the oneway
 * `IMonado.updateWindowRect`; this lands them in the service-process globals,
 * where the per-session compositor picks them up and forwards them to the
 * display processor (`set_window_screen_rect`) before each weave. Without it a
 * window that is not at the panel's top-left weaves at the wrong interlace
 * phase — and a pure MOVE never even raises a resize on Android.
 *
 * Deduped Java-side; the globals dedupe again, so this is cheap.
 */
extern "C" JNIEXPORT void JNICALL
Java_org_freedesktop_monado_ipc_MonadoImpl_nativeWindowScreenRect(
    JNIEnv *env, jobject thiz, jint x, jint y, jint w, jint h, jint display_id, jint disp_w, jint disp_h)
{
	jni::init(env);
	jni::Object monadoImpl(thiz);

	if (w <= 0 || h <= 0) {
		return;
	}
	android_globals_set_window_screen_rect((int32_t)x, (int32_t)y, (uint32_t)w, (uint32_t)h,
	                                       (int32_t)display_id, (uint32_t)(disp_w > 0 ? disp_w : 0),
	                                       (uint32_t)(disp_h > 0 ? disp_h : 0));
	U_LOG_I("service: window screen rect %d,%d %dx%d display %d panel %dx%d (#1033/#1034)", (int)x, (int)y,
	        (int)w, (int)h, (int)display_id, (int)disp_w, (int)disp_h);
}

extern "C" JNIEXPORT jint JNICALL
Java_org_freedesktop_monado_ipc_MonadoImpl_nativeShutdownServer(JNIEnv *env, jobject thiz)
{
	jni::init(env);
	jni::Object monadoImpl(thiz);

	return IpcServerHelper::instance().shutdownServer();
}
