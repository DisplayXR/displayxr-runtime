// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-conversion-host command line (ADR-053 roadmap §3.4).
 *
 *     displayxr-conversion-host --provider <id> --binary <path> --channel <name>
 *                               --parent <pid> [--adapter-luid <hex>]
 *                               [--idle-exit-ms <ms>]
 *
 * `--adapter-luid` is the service's ingest (render) adapter as a 64-bit hex
 * LUID (`0x` optional, or `<high>:<low>`); 0 / absent means "CPU lane only"
 * (POSIX, or a service without a D3D11 device). `--idle-exit-ms` overrides the
 * 10 min idle exit (tests only). Pure: no I/O, tested by
 * tests_conv_host_satellite.
 *
 * @ingroup conv_host
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! The roadmap's idle exit: no bound task and no prewarm for this long.
#define CONV_HOST_IDLE_EXIT_MS_DEFAULT (10u * 60u * 1000u)

struct conv_host_args
{
	char provider_id[64];
	char binary_path[512];
	char channel[128];
	uint64_t parent_pid;
	uint64_t adapter_luid; //!< 0 = none (CPU lane only)
	bool has_adapter_luid;
	uint32_t idle_exit_ms;
	int trust_enforce; //!< -1 = not given (platform policy), 0 = audit, 1 = enforce (ADR-054 N4b)
	char error[160]; //!< why parsing failed
};

/*!
 * Parse @p argv into @p out (zeroed first). Returns false with out->error set
 * on an unknown flag, a missing value, a malformed number or a missing
 * required flag (--provider, --binary, --channel, --parent).
 */
bool
conv_host_args_parse(int argc, const char *const *argv, struct conv_host_args *out);

/*!
 * Parse a LUID written as hex ("0x0000000000012a4f", "12a4f"), or as the
 * "<high>:<low>" pair (each hex) adapter dumps print. False on garbage.
 */
bool
conv_host_parse_luid(const char *s, uint64_t *out);

#ifdef __cplusplus
}
#endif
