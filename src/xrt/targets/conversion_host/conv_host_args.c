// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  displayxr-conversion-host command line parsing.
 * @ingroup conv_host
 */

#include "conv_host_args.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool
parse_hex64(const char *s, uint64_t *out)
{
	if (s == NULL || *s == '\0') {
		return false;
	}
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
	}
	if (*s == '\0' || strlen(s) > 16) {
		return false;
	}
	uint64_t v = 0;
	for (const char *c = s; *c != '\0'; c++) {
		uint32_t d;
		if (*c >= '0' && *c <= '9') {
			d = (uint32_t)(*c - '0');
		} else if (*c >= 'a' && *c <= 'f') {
			d = (uint32_t)(*c - 'a' + 10);
		} else if (*c >= 'A' && *c <= 'F') {
			d = (uint32_t)(*c - 'A' + 10);
		} else {
			return false;
		}
		v = (v << 4) | d;
	}
	*out = v;
	return true;
}

bool
conv_host_parse_luid(const char *s, uint64_t *out)
{
	if (s == NULL || out == NULL) {
		return false;
	}
	const char *colon = strchr(s, ':');
	if (colon == NULL) {
		return parse_hex64(s, out);
	}
	// "<high>:<low>": HighPart (LONG) then LowPart (DWORD), each hex.
	char hi_s[24];
	const size_t hl = (size_t)(colon - s);
	if (hl == 0 || hl >= sizeof(hi_s)) {
		return false;
	}
	memcpy(hi_s, s, hl);
	hi_s[hl] = '\0';
	uint64_t hi = 0;
	uint64_t lo = 0;
	if (!parse_hex64(hi_s, &hi) || !parse_hex64(colon + 1, &lo) || hi > 0xffffffffull || lo > 0xffffffffull) {
		return false;
	}
	*out = (hi << 32) | lo;
	return true;
}

static bool
parse_u64_dec(const char *s, uint64_t *out)
{
	if (s == NULL || *s < '0' || *s > '9') {
		return false;
	}
	char *end = NULL;
	errno = 0;
	unsigned long long v = strtoull(s, &end, 10);
	if (errno != 0 || end == s || *end != '\0') {
		return false;
	}
	*out = (uint64_t)v;
	return true;
}

static bool
copy_str(char *dst, size_t dst_size, const char *src)
{
	const size_t n = strlen(src);
	if (n == 0 || n >= dst_size) {
		return false;
	}
	memcpy(dst, src, n + 1);
	return true;
}

bool
conv_host_args_parse(int argc, const char *const *argv, struct conv_host_args *out)
{
	memset(out, 0, sizeof(*out));
	out->idle_exit_ms = CONV_HOST_IDLE_EXIT_MS_DEFAULT;
	out->trust_enforce = -1;
	bool have_parent = false;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;
		if (strncmp(a, "--", 2) != 0) {
			(void)snprintf(out->error, sizeof(out->error), "unexpected argument '%s'", a);
			return false;
		}
		if (v == NULL) {
			(void)snprintf(out->error, sizeof(out->error), "%s: missing value", a);
			return false;
		}
		bool ok;
		if (strcmp(a, "--provider") == 0) {
			ok = copy_str(out->provider_id, sizeof(out->provider_id), v);
		} else if (strcmp(a, "--binary") == 0) {
			ok = copy_str(out->binary_path, sizeof(out->binary_path), v);
		} else if (strcmp(a, "--channel") == 0) {
			ok = copy_str(out->channel, sizeof(out->channel), v);
		} else if (strcmp(a, "--parent") == 0) {
			ok = parse_u64_dec(v, &out->parent_pid) && out->parent_pid != 0;
			have_parent = ok;
		} else if (strcmp(a, "--adapter-luid") == 0) {
			ok = conv_host_parse_luid(v, &out->adapter_luid);
			out->has_adapter_luid = ok && out->adapter_luid != 0;
		} else if (strcmp(a, "--trust-enforce") == 0) {
			ok = strcmp(v, "0") == 0 || strcmp(v, "1") == 0;
			out->trust_enforce = ok ? (v[0] == '1' ? 1 : 0) : -1;
		} else if (strcmp(a, "--idle-exit-ms") == 0) {
			uint64_t ms = 0;
			ok = parse_u64_dec(v, &ms) && ms > 0 && ms <= 0xffffffffull;
			out->idle_exit_ms = (uint32_t)ms;
		} else {
			(void)snprintf(out->error, sizeof(out->error), "unknown argument '%s'", a);
			return false;
		}
		if (!ok) {
			(void)snprintf(out->error, sizeof(out->error), "%s: bad value '%s'", a, v);
			return false;
		}
		i++;
	}

	const char *missing = out->provider_id[0] == '\0'   ? "--provider"
	                      : out->binary_path[0] == '\0' ? "--binary"
	                      : out->channel[0] == '\0'     ? "--channel"
	                      : !have_parent                ? "--parent"
	                                                    : NULL;
	if (missing != NULL) {
		(void)snprintf(out->error, sizeof(out->error), "%s is required", missing);
		return false;
	}
	return true;
}
