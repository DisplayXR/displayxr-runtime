// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-conversion-host: one binary for every conversion provider
 *         (ADR-053 D5; roadmap conversion-providers.md §3.4, §3.5).
 *
 *     displayxr-conversion-host --provider <id> --binary <path> --channel <token>
 *                               --parent <pid> [--adapter-luid <hex>]
 *
 * Spawned lazily by the service's supervisor, one per provider DLL, at medium
 * integrity in a KILL_ON_JOB_CLOSE job (POSIX: parent-death signal). Order:
 *
 *  1. log file `DisplayXR_conversion-host_<provider>.<pid>_<ts>.log`, crash
 *     writer (minidump / core);
 *  2. refuse to run elevated (exit 14);
 *  3. Windows: the hidden session-end window (Restart Manager -> exit 20);
 *  4. the control channel (the service's inherited token or name);
 *  5. Windows: a D3D11 device on --adapter-luid, never the default adapter
 *     (exit 13 when none is usable); POSIX: CPU lane only;
 *  6. load the provider through the conversion loader: negotiate + ABI gate
 *     (exit 10 / 11), probe, describe;
 *  7. hello -> serve -> exit code (exit-code table: conv_host_wire.h).
 *
 * Host-only knob (never read for a provider): DXR_CONV_HOST_ALLOW_ELEVATED=1
 * turns the elevation refusal into a WARN, for CI runners and dev harnesses
 * that are elevated themselves.
 *
 * @ingroup conv_host
 */

#include "xrt/xrt_config_os.h"

#include "conv_host_args.h"
#include "conv_host_chan.h"
#include "conv_host_exit.h"
#include "conv_host_log.h"
#include "conv_host_os.h"
#include "conv_host_provider.h"
#include "conv_host_serve.h"

#ifdef XRT_OS_WINDOWS
#include "conv_host_d3d11.h"
#endif

#include "xrt/xrt_config_os.h"
#include "util/u_git_tag.h"
#include "util/u_logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//! How long the host may take to exit after a Restart Manager close request.
#define CONV_HOST_SESSION_END_BACKSTOP_MS 3000u

static void
on_session_end(void *ctx)
{
	(void)ctx;
	conv_host_serve_request_exit(CONV_HOST_EXIT_UPDATE);
}

static int
finish(int code)
{
	U_LOG_W("conversion host: exiting %d (%s)", code, conv_host_exit_str(code));
	conv_host_log_shutdown();
	return code;
}

int
main(int argc, char *argv[])
{
	conv_host_serve_prepare();

	struct conv_host_args args;
	if (!conv_host_args_parse(argc, (const char *const *)argv, &args)) {
		conv_host_log_init("invalid-args");
		U_LOG_E("conversion host: %s", args.error);
		fprintf(stderr, "displayxr-conversion-host: %s\n", args.error);
		return finish(CONV_HOST_EXIT_USAGE);
	}

	// 1. Log + crash forensics, before anything can fail interestingly.
	conv_host_log_init(args.provider_id);
	conv_host_os_install_crash_handler(args.provider_id);
	U_LOG_W("conversion host: provider '%s' binary '%s' parent %llu adapter 0x%016llx tag %s", args.provider_id,
	        args.binary_path, (unsigned long long)args.parent_pid, (unsigned long long)args.adapter_luid,
	        u_git_tag);

	// 2. Integrity: inherit the service's medium token, never elevated.
	char detail[96] = {0};
	if (conv_host_os_is_elevated(detail, sizeof(detail))) {
		const char *allow = getenv("DXR_CONV_HOST_ALLOW_ELEVATED");
		if (allow == NULL || strcmp(allow, "1") != 0) {
			U_LOG_E("conversion host: refusing to run elevated (%s)", detail);
			return finish(CONV_HOST_EXIT_ELEVATED);
		}
		U_LOG_W("conversion host: running ELEVATED (%s) because DXR_CONV_HOST_ALLOW_ELEVATED=1 (CI / dev only)",
		        detail);
	}

	// Parent death (POSIX): the service went away before we even started.
	if (!conv_host_os_watch_parent(args.parent_pid)) {
		U_LOG_W("conversion host: parent %llu is gone; nothing to serve", (unsigned long long)args.parent_pid);
		return finish(CONV_HOST_EXIT_ORDERLY);
	}

	// 3. Restart Manager / logoff close requests -> exit 20.
	if (!conv_host_os_start_session_end_watch(on_session_end, NULL, CONV_HOST_SESSION_END_BACKSTOP_MS)) {
		U_LOG_W("conversion host: no session-end window; Restart Manager can only force this host closed");
	}

	// 4. The control channel.
	char reason[192] = {0};
	struct conv_host_chan *chan = conv_host_chan_open(args.channel, reason, sizeof(reason));
	if (chan == NULL) {
		U_LOG_E("conversion host: control channel '%s': %s", args.channel, reason);
		return finish(CONV_HOST_EXIT_CHANNEL);
	}

	// 5. The device, on the service's adapter only.
	struct conv_host_d3d11 *d3d11 = NULL;
	uint64_t opened_luid = 0;
#ifdef XRT_OS_WINDOWS
	if (args.has_adapter_luid) {
		d3d11 = conv_host_d3d11_create(args.adapter_luid, reason, sizeof(reason));
		if (d3d11 == NULL) {
			U_LOG_E("conversion host: no usable D3D11 device on adapter 0x%016llx: %s",
			        (unsigned long long)args.adapter_luid, reason);
			conv_host_chan_destroy(&chan);
			return finish(CONV_HOST_EXIT_NO_DEVICE);
		}
		opened_luid = args.adapter_luid;
		U_LOG_W("conversion host: D3D11 device on '%s' (0x%016llx)", conv_host_d3d11_adapter_name(d3d11),
		        (unsigned long long)opened_luid);
	}
#else
	if (args.has_adapter_luid) {
		U_LOG_I("conversion host: adapter LUID ignored on this platform (CPU lane only in P2)");
	}
#endif

	// 6. The provider: the settings the service would hand it, then load.
	conv_host_provider_config_from_registration(args.provider_id, opened_luid);
	struct conv_host_provider provider;
	int code = conv_host_provider_load(&provider, args.provider_id, args.binary_path, reason, sizeof(reason));
	if (code != CONV_HOST_EXIT_ORDERLY) {
		U_LOG_E("conversion host: provider '%s' not loaded: %s", args.provider_id, reason);
		conv_host_provider_unload(&provider);
	} else {
		// 7. Serve.
		const struct conv_host_serve_config cfg = {
		    .provider_id = args.provider_id,
		    .provider = &provider,
		    .d3d11 = d3d11,
		    .adapter_luid = opened_luid,
		    .idle_exit_ms = args.idle_exit_ms,
		};
		code = conv_host_serve_run(chan, &cfg);
	}

	// The provider is gone (or leaked on exit 20); the device and channel go
	// with the process. Closing the channel last tells the service we are done.
	conv_host_chan_destroy(&chan);
#ifdef XRT_OS_WINDOWS
	if (code != CONV_HOST_EXIT_UPDATE) {
		conv_host_d3d11_destroy(&d3d11);
	}
#else
	(void)d3d11;
#endif
	return finish(code);
}
