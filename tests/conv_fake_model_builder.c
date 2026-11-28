// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A fake `dxr-model-builder` implementing the §5.6.2 command line
 *         (conversion-provider-discovery.md), for tests_conv_model_build and
 *         the CLI end-to-end test. Its behaviour is chosen by the --variant
 *         names it is asked to build (the store passes only argv and a
 *         minimal environment, so nothing else can steer it):
 *
 *   a, b, c     write out/<variant>.bin (fake_builder_output) and exit 0
 *   noise       print a non-JSON line on stdout first, then like "a"
 *   envcheck    exit 1 unless HOME / TMP / TEMP / TMPDIR point at --work and
 *               the working directory is --work; else like "a"
 *   wrong       write bytes that are not the pack's, exit 0
 *   exit-<n>    print an `error` event and exit <n>
 *   silent      print `start`, then nothing, forever (the watchdog)
 *   cancel      print progress until the cancel file appears, then exit 17
 *   stubborn    print progress forever, ignoring the cancel file
 *
 * It re-checks the source (size + sha256 against its own pin) and
 * --accept-license like a real builder: exit 12 / exit 10.
 */

#include "conv_fake_model_builder_data.h"

#include "util/u_sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define SLEEP_MS(ms) Sleep(ms)
#else
#include <unistd.h>
#define SLEEP_MS(ms) usleep((ms)*1000)
#endif

static char g_out[1024], g_work[1024], g_cancel[1024], g_source[1024];
static const char *g_variants[16];
static int g_nvar = 0;
static int g_licence_ok = 0;

static void
hex_of(const char *data, size_t n, char hex[65])
{
	struct u_sha256 h;
	u_sha256_init(&h);
	u_sha256_update(&h, data, n);
	unsigned char d[32];
	u_sha256_final(&h, d);
	for (int i = 0; i < 32; i++) {
		snprintf(hex + i * 2, 3, "%02x", d[i]);
	}
}

static void
ev(const char *line)
{
	fprintf(stdout, "%s\n", line);
	fflush(stdout);
}

static int
exists(const char *p)
{
	FILE *f = fopen(p, "rb");
	if (f != NULL) {
		fclose(f);
		return 1;
	}
	return 0;
}

static void
join(char *out, size_t n, const char *a, const char *b)
{
#ifdef _WIN32
	snprintf(out, n, "%s\\%s", a, b);
#else
	snprintf(out, n, "%s/%s", a, b);
#endif
}

static int
describe(void)
{
	char src[8192], hex[65];
	const size_t n = fake_builder_source(src, sizeof(src));
	hex_of(src, n, hex);
	printf(
	    "{\"v\":1,\"tool\":\"%s\",\"version\":\"%s\",\"recipes\":[{\"recipe\":\"%s\",\"variants\":[\"a\",\"b\","
	    "\"c\",\"noise\",\"envcheck\",\"wrong\",\"exit-1\",\"exit-10\",\"exit-11\",\"exit-12\",\"exit-13\","
	    "\"exit-14\",\"exit-15\",\"exit-16\",\"exit-17\",\"silent\",\"cancel\",\"stubborn\"],\"sources\":[{"
	    "\"role\":\"checkpoint\",\"sha256\":\"%s\",\"size\":%zu,\"license_id\":\"%s\"}],\"needs\":{\"ram_mb\":"
	    "64,\"disk_mb\":1}}]}\n",
	    FAKE_BUILDER_TOOL_ID, FAKE_BUILDER_VERSION, FAKE_BUILDER_RECIPE, hex, n, FAKE_BUILDER_LICENCE);
	return 0;
}

static int
write_output(const char *variant, int wrong)
{
	char buf[8192];
	size_t n = fake_builder_output(variant, buf, sizeof(buf));
	if (wrong) {
		n = (size_t)snprintf(buf, sizeof(buf), "not the bytes the pack names\n");
	}
	char name[128], path[1200];
	snprintf(name, sizeof(name), "%s.bin", variant);
	join(path, sizeof(path), g_out, name);
	FILE *f = fopen(path, "wb");
	if (f == NULL || fwrite(buf, 1, n, f) != n) {
		return 15;
	}
	fclose(f);
	char hex[65], line[512];
	hex_of(buf, n, hex);
	snprintf(
	    line, sizeof(line),
	    "{\"v\":1,\"event\":\"output\",\"variant\":\"%s\",\"role\":\"model\",\"file\":\"%s\",\"sha256\":\"%s\","
	    "\"size\":%zu}",
	    variant, name, hex, n);
	ev(line);
	return 0;
}

static int
env_is_work(const char *name)
{
	const char *v = getenv(name);
	return v != NULL && strcmp(v, g_work) == 0;
}

static int
fail(int code, const char *name, const char *msg)
{
	char line[512];
	snprintf(line, sizeof(line), "{\"v\":1,\"event\":\"error\",\"code\":\"%s\",\"message\":\"%s\"}", name, msg);
	ev(line);
	ev("{\"v\":1,\"event\":\"done\",\"ok\":false}");
	return code;
}

static int
build(void)
{
	if (!g_licence_ok) {
		return fail(10, "license", "--accept-license test-licence is required");
	}
	// Re-check the source against our own pin before reading it.
	char want[8192], got[16384];
	const size_t wn = fake_builder_source(want, sizeof(want));
	FILE *f = fopen(g_source, "rb");
	size_t gn = f != NULL ? fread(got, 1, sizeof(got), f) : 0;
	if (f != NULL) {
		fclose(f);
	}
	char wh[65], gh[65];
	hex_of(want, wn, wh);
	hex_of(got, gn, gh);
	if (gn != wn || strcmp(wh, gh) != 0) {
		return fail(12, "source-mismatch", "the checkpoint is not the pinned one");
	}
	char line[512];
	snprintf(line, sizeof(line),
	         "{\"v\":1,\"event\":\"start\",\"tool\":\"%s\",\"version\":\"%s\",\"recipe\":\"%s\","
	         "\"variants\":[]}",
	         FAKE_BUILDER_TOOL_ID, FAKE_BUILDER_VERSION, FAKE_BUILDER_RECIPE);
	ev(line);
	for (int i = 0; i < g_nvar; i++) {
		const char *v = g_variants[i];
		snprintf(
		    line, sizeof(line),
		    "{\"v\":1,\"event\":\"phase\",\"phase\":\"convert\",\"variant\":\"%s\",\"step\":%d,\"steps\":%d}",
		    v, i + 1, g_nvar);
		ev(line);
		if (strncmp(v, "exit-", 5) == 0) {
			return fail(atoi(v + 5), "scripted", "the test asked for this exit code");
		}
		if (strcmp(v, "silent") == 0) {
			for (;;) {
				SLEEP_MS(1000);
			}
		}
		if (strcmp(v, "cancel") == 0 || strcmp(v, "stubborn") == 0) {
			for (int t = 0;; t++) {
				snprintf(
				    line, sizeof(line),
				    "{\"v\":1,\"event\":\"progress\",\"phase\":\"convert\",\"done\":%d,\"total\":0}",
				    t);
				ev(line);
				if (strcmp(v, "cancel") == 0 && exists(g_cancel)) {
					return fail(17, "cancelled", "the cancel file appeared");
				}
				SLEEP_MS(100);
			}
		}
		if (strcmp(v, "noise") == 0) {
			fprintf(stdout, "this line is not an event\n");
			fflush(stdout);
		}
		if (strcmp(v, "envcheck") == 0) {
			char probe[1200];
			join(probe, sizeof(probe), g_work, "cwd-probe");
			FILE *p = fopen("cwd-probe", "wb");
			if (p != NULL) {
				fclose(p);
			}
			const int cwd_ok = exists(probe);
			remove(probe);
#ifdef _WIN32
			const int env_ok = env_is_work("TMP") && env_is_work("TEMP") && env_is_work("USERPROFILE") &&
			                   env_is_work("LOCALAPPDATA") && getenv("SystemRoot") != NULL;
#else
			const int env_ok = env_is_work("HOME") && env_is_work("TMPDIR") && env_is_work("TMP") &&
			                   env_is_work("TEMP") && getenv("DXR_ALLOW_DEV_PLUGIN_PATHS") == NULL;
#endif
			if (!cwd_ok || !env_ok) {
				return fail(1, "internal",
				            "the environment or working directory is not the minimal one");
			}
		}
		const int r = write_output(v, strcmp(v, "wrong") == 0);
		if (r != 0) {
			return fail(r, "disk", "cannot write --out");
		}
	}
	ev("{\"v\":1,\"event\":\"done\",\"ok\":true}");
	return 0;
}

int
main(int argc, char **argv)
{
	if (argc >= 3 && strcmp(argv[1], "describe") == 0 && strcmp(argv[2], "--json") == 0) {
		return describe();
	}
	if (argc < 2 || strcmp(argv[1], "build") != 0) {
		fprintf(stderr, "usage: fake builder describe --json | build ...\n");
		return 2;
	}
	for (int i = 2; i + 1 < argc; i++) {
		const char *k = argv[i], *v = argv[i + 1];
		if (strcmp(k, "--recipe") == 0) {
			if (strcmp(v, FAKE_BUILDER_RECIPE) != 0) {
				return fail(2, "usage", "unknown recipe");
			}
		} else if (strcmp(k, "--variant") == 0 && g_nvar < 16) {
			g_variants[g_nvar++] = v;
		} else if (strcmp(k, "--source") == 0 && strncmp(v, "checkpoint=", 11) == 0) {
			snprintf(g_source, sizeof(g_source), "%s", v + 11);
		} else if (strcmp(k, "--out") == 0) {
			snprintf(g_out, sizeof(g_out), "%s", v);
		} else if (strcmp(k, "--work") == 0) {
			snprintf(g_work, sizeof(g_work), "%s", v);
		} else if (strcmp(k, "--cancel-file") == 0) {
			snprintf(g_cancel, sizeof(g_cancel), "%s", v);
		} else if (strcmp(k, "--accept-license") == 0) {
			g_licence_ok |= strcmp(v, FAKE_BUILDER_LICENCE) == 0;
		} else {
			continue;
		}
		i++;
	}
	fprintf(stderr, "fake builder: %d variant(s)\n", g_nvar);
	if (g_nvar == 0 || g_out[0] == '\0' || g_work[0] == '\0' || g_cancel[0] == '\0' || g_source[0] == '\0') {
		return fail(2, "usage", "missing --variant / --out / --work / --cancel-file / --source");
	}
	return build();
}
