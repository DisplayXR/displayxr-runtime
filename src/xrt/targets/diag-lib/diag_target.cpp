// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  JNI bridge for the Android diagnostics dashboard (#558).
 *
 * One export: run the displayxr-cli query core (headless plug-in discovery,
 * display info, self-test) plus — when the service is up — a session-less
 * IPC "monitor" pass (connected clients, workspace state, live display
 * status), and return everything as one JSON string.
 *
 * Lifetime contract: this runs in the APK's dedicated short-lived `:diag`
 * process (DiagQueryService). The process is killed by the Java side right
 * after the reply, which is why nothing here is torn down — see the note at
 * the bottom of the export.
 *
 * @author David Fattal
 */

#include "xrt/xrt_instance.h"
#include "xrt/xrt_config_build.h"

#include "util/u_logging.h"

#include "android/android_globals.h"
#include "android/android_main_thread.h"

#include "cli_query.h"

#include <cjson/cJSON.h>

#include <jni.h>
#include <stdio.h>

#ifdef DIAG_HAVE_IPC
#include "shared/ipc_protocol.h"
#include "client/ipc_client_connection.h"
#include "ipc_client_generated.h"
#endif


#ifdef DIAG_HAVE_IPC
/*!
 * Session-less monitor pass over a live service. Returns NULL when the
 * service isn't running / refuses the connection — the dashboard renders
 * that as "service not running". The connection itself briefly appears in
 * the client list under the name below; the UI filters it out.
 */
static cJSON *
query_live_state(struct _JavaVM *vm, void *context)
{
	struct xrt_instance_info ii = {};
	snprintf(ii.app_info.application_name, sizeof(ii.app_info.application_name), "%s", "displayxr-diag");
	ii.platform_info.vm = vm;
	ii.platform_info.context = context;

	struct ipc_connection ipc_c = {};
	if (ipc_client_connection_init(&ipc_c, U_LOGGING_INFO, &ii) != XRT_SUCCESS) {
		return NULL;
	}

	cJSON *live = cJSON_CreateObject();

	struct ipc_display_status st = {};
	if (ipc_call_system_get_display_status(&ipc_c, &st) == XRT_SUCCESS) {
		cJSON *ds = cJSON_AddObjectToObject(live, "display_status");
		cJSON_AddBoolToObject(ds, "mode_valid", st.mode_valid);
		cJSON_AddNumberToObject(ds, "active_rendering_mode", (double)st.active_rendering_mode);
		cJSON_AddBoolToObject(ds, "tracking_valid", st.tracking_valid);
		cJSON_AddBoolToObject(ds, "is_tracking", st.is_tracking);
	}

	bool ws_active = false;
	if (ipc_call_workspace_get_state(&ipc_c, &ws_active) == XRT_SUCCESS) {
		cJSON_AddBoolToObject(live, "workspace_active", ws_active);
	}

	struct ipc_client_list list = {};
	if (ipc_call_system_get_clients(&ipc_c, &list) == XRT_SUCCESS) {
		cJSON *arr = cJSON_AddArrayToObject(live, "clients");
		for (uint32_t i = 0; i < list.id_count; i++) {
			struct ipc_app_state ias = {};
			if (ipc_call_system_get_client_info(&ipc_c, list.ids[i], &ias) != XRT_SUCCESS) {
				continue;
			}
			cJSON *c = cJSON_CreateObject();
			cJSON_AddNumberToObject(c, "id", (double)ias.id);
			cJSON_AddNumberToObject(c, "pid", (double)ias.pid);
			cJSON_AddStringToObject(c, "name", ias.info.application_name);
			cJSON_AddBoolToObject(c, "primary", ias.primary_application);
			cJSON_AddBoolToObject(c, "active", ias.session_active);
			cJSON_AddBoolToObject(c, "visible", ias.session_visible);
			cJSON_AddBoolToObject(c, "focused", ias.session_focused);
			cJSON_AddItemToArray(arr, c);
		}
	}

	ipc_client_connection_fini(&ipc_c);

	return live;
}
#endif // DIAG_HAVE_IPC

extern "C" JNIEXPORT jstring JNICALL
Java_org_freedesktop_monado_openxr_1runtime_DiagNative_nativeRunQueryJson(JNIEnv *env, jobject thiz, jobject context)
{
	(void)thiz;

	JavaVM *jvm = NULL;
	if (env->GetJavaVM(&jvm) != JNI_OK || jvm == NULL) {
		return env->NewStringUTF("{}");
	}

	android_globals_store_vm_and_context((struct _JavaVM *)jvm, (void *)context);

	// Called from the DiagQueryService HandlerThread (Looper-bearing), so
	// vendor display-processor init can be marshaled onto it (#510 M2).
	android_main_thread_dispatch_init();

	cJSON *root = cJSON_CreateObject();

	// Live pass FIRST, while no in-process vendor plug-in is loaded yet —
	// the headless probe below may grab the same camera/CNSDK resources
	// the service's DP holds.
#ifdef DIAG_HAVE_IPC
	cJSON *live = query_live_state((struct _JavaVM *)jvm, (void *)context);
	if (live != NULL) {
		cJSON_AddItemToObject(root, "live", live);
	} else {
		cJSON_AddNullToObject(root, "live");
	}
#else
	cJSON_AddNullToObject(root, "live");
#endif

	// Headless in-process pass: the exact displayxr-cli info/selftest path
	// (real plug-in discovery, no compositor). NULL instance info on
	// purpose — our context is a Service, which the Android instance
	// base rejects (it wants an Activity for lifecycle callbacks); the
	// runtime falls back to the android_globals vm/context stored above,
	// exactly like displayxr-service does.
	struct cli_query_result r;
	struct cli_query_handles h;
	cli_query_fill(&r, &h, NULL);

	cJSON_AddItemToObject(root, "info", cli_query_info_to_cjson(&r));
	cJSON_AddItemToObject(root, "selftest", cli_query_selftest_to_cjson(&r));

	char *out = cJSON_PrintUnformatted(root);
	jstring jstr = env->NewStringUTF(out != NULL ? out : "{}");
	if (out != NULL) {
		cJSON_free(out);
	}
	cJSON_Delete(root);

	// DELIBERATELY no cli_query_teardown(&h): vendor plug-in destroy can
	// hang (displayxr-leia-plugin#39). DiagQueryService kills this :diag
	// process right after the reply — process death is the teardown.
	(void)h;

	return jstr;
}
