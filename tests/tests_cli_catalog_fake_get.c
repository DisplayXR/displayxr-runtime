// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  A stand-in for `dxr-get`, driven by tests_cli_catalog.
 *
 * It speaks the engine's command line and exit codes (dxr-get.md) with canned
 * answers, so `displayxr-cli catalog`'s argument mapping, exit-code mapping,
 * JSON envelope and elevation step are tested on every platform without a
 * catalog, a network or an installer.
 *
 *   FAKE_DXR_GET_LOG=<file>   every invocation appends its arguments, joined by '|'
 *   FAKE_DXR_GET_ADMIN=1      answer as an elevated process would
 *
 * What it does is chosen by the item id and by the `--catalog` value:
 *   --catalog offline   exit 10 with the offline document
 *   --catalog garbage   `list` prints text that is not JSON, exit 0
 *   --catalog slow      sleeps 5 s before answering (the bounded-wait test)
 *   --catalog current   `outdated` reports nothing
 *   install open-conv | leia-sr | --default   a plan with work: 50 when not elevated, else 0
 *   install browser     nothing to do, 0
 *   install nofetch     gate 40: 50 when not elevated (the engine's order), else 40
 *   install incompat    41;   install runtime --no-runtime   41
 *   install badsig | dlfail | verify | child   11 | 20 | 21 | 30
 *   uninstall leia-sr   50 when not elevated, else 0
 *   uninstall browser   43;   uninstall <other>   42
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define nap_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define nap_ms(ms) usleep((ms)*1000)
#endif

#define ROW_RUNTIME                                                                                                    \
	"{\"id\":\"runtime\",\"name\":\"DisplayXR Runtime\",\"class\":\"runtime\",\"publisher\":\"displayxr\","        \
	"\"tier\":\"verified\",\"installed\":\"2.60.0\",\"available\":\"2.70.0\",\"state\":\"outdated\","              \
	"\"installable\":true,\"receipt\":false,\"aliases\":[\"runtime\"],\"recommended\":false,"                      \
	"\"detected\":\"none\",\"size\":50000000,\"download\":50000000,\"compatible\":true,\"prerequisites\":[]}"
#define ROW_LEIA                                                                                                       \
	"{\"id\":\"leia-sr\",\"name\":\"Leia SR display processor\",\"class\":\"display_processor\","                  \
	"\"registered_id\":\"leia-sr\",\"publisher\":\"leia\",\"tier\":\"verified\",\"installed\":\"2.22.0\","         \
	"\"available\":\"2.23.0\",\"state\":\"outdated\",\"installable\":true,\"receipt\":true,"                       \
	"\"summary\":\"Weaving for Leia panels\",\"aliases\":[\"leia_plugin\"],\"recommended\":false,"                 \
	"\"detected\":\"high\",\"evidence\":\"display EDID 44048/16711\",\"size\":31457280,\"download\":31457280,"     \
	"\"compatible\":true,\"prerequisites\":[{\"name\":\"SR Platform\",\"met\":true,\"installable\":false}]}"
#define ROW_CONV                                                                                                       \
	"{\"id\":\"open-conv\",\"name\":\"Open conversion provider\",\"class\":\"conversion_provider\","               \
	"\"registered_id\":\"open-conv\",\"publisher\":\"displayxr\",\"tier\":\"verified\",\"installed\":null,"        \
	"\"available\":\"0.5.0\",\"state\":\"not_installed\",\"installable\":true,\"receipt\":false,"                  \
	"\"aliases\":[],\"recommended\":true,\"detected\":\"none\",\"size\":1000,\"download\":1000,"                   \
	"\"compatible\":true,\"prerequisites\":[]}"
#define ROW_BROWSER                                                                                                    \
	"{\"id\":\"browser\",\"name\":\"DisplayXR Browser\",\"class\":\"app\",\"publisher\":\"displayxr\","            \
	"\"tier\":\"verified\",\"installed\":\"1.10.5\",\"available\":\"1.10.5\",\"state\":\"current\","               \
	"\"installable\":true,\"receipt\":true,\"aliases\":[\"browser\"],\"recommended\":false,"                       \
	"\"detected\":\"none\",\"size\":200,\"download\":0,\"compatible\":true,\"prerequisites\":[]}"
#define ROW_LINKOUT                                                                                                    \
	"{\"id\":\"vendor.hands\",\"name\":\"Vendor hands\",\"class\":\"input_provider\","                             \
	"\"registered_id\":\"vendor-hands\",\"publisher\":\"vendor\",\"tier\":\"community\","                          \
	"\"installed\":\"1.0.0\",\"available\":\"1.1.0\",\"state\":\"outdated\",\"installable\":false,"                \
	"\"receipt\":false,\"listing\":\"https://example.invalid/hands\",\"aliases\":[],\"recommended\":false,"        \
	"\"detected\":\"none\",\"size\":0,\"download\":0,\"compatible\":true,\"prerequisites\":[]}"

static bool
is_admin(void)
{
	const char *v = getenv("FAKE_DXR_GET_ADMIN");
	return v != NULL && strcmp(v, "1") == 0;
}

static void
plan(const char *id, const char *action, int gate, const char *gate_reason)
{
	printf("{\"for\":\"%s\",\"schema\":2,\"gate\":%d,", id, gate);
	if (gate_reason != NULL) {
		printf("\"gate_reason\":\"%s\",", gate_reason);
	}
	printf("\"total_size\":1000,\"total_download\":1000,\"actions\":[");
	if (strcmp(id, "--default") == 0) {
		printf(
		    "{\"component\":\"open-conv\",\"installed\":null,\"state\":\"missing\",\"target\":\"0.5.0\","
		    "\"size\":1000,\"id\":\"open-conv\",\"kind\":\"item\",\"action\":\"install\",\"download\":1000},"
		    "{\"component\":\"leia_plugin\",\"installed\":\"2.22.0\",\"state\":\"too_old\","
		    "\"target\":\"2.23.0\",\"size\":31457280,\"id\":\"leia-sr\",\"kind\":\"item\","
		    "\"action\":\"upgrade\",\"download\":31457280}");
	} else {
		printf(
		    "{\"component\":\"%s\",\"installed\":%s,\"state\":\"%s\",\"target\":\"9.9.9\",\"size\":1000,"
		    "\"id\":\"%s\",\"kind\":\"item\",\"action\":\"%s\",\"download\":1000}",
		    id, strcmp(action, "install") == 0 ? "null" : "\"1.0.0\"",
		    strcmp(action, "none") == 0 ? "ok" : "missing", id, action);
	}
	printf("],\"prerequisites\":[]}\n");
}

int
main(int argc, char **argv)
{
	const char *log = getenv("FAKE_DXR_GET_LOG");
	if (log != NULL && log[0] != '\0') {
		FILE *f = fopen(log, "ab");
		if (f != NULL) {
			for (int i = 1; i < argc; i++) {
				fprintf(f, "%s%s", i > 1 ? "|" : "", argv[i]);
			}
			fprintf(f, "\n");
			fclose(f);
		}
	}

	const char *catalog = "", *cmd = NULL, *arg = NULL;
	bool json = false, no_runtime = false, installed = false, use_default = false;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (strcmp(a, "--catalog") == 0 && i + 1 < argc) {
			catalog = argv[++i];
		} else if (strcmp(a, "--json") == 0) {
			json = true;
		} else if (strcmp(a, "--no-runtime") == 0) {
			no_runtime = true;
		} else if (strcmp(a, "--installed") == 0) {
			installed = true;
		} else if (strcmp(a, "--default") == 0) {
			use_default = true;
		} else if (a[0] == '-') {
			return 2;
		} else if (cmd == NULL) {
			cmd = a;
		} else if (arg == NULL) {
			arg = a;
		} else {
			return 2;
		}
	}
	if (cmd == NULL) {
		return 2;
	}

	if (strcmp(cmd, "list") == 0 && installed) {
		printf(
		    "{\"installed\":["
		    "{\"id\":\"leia-sr\",\"name\":\"Leia SR display processor\",\"class\":\"display_processor\","
		    "\"version\":\"2.22.0\",\"install_time\":\"2026-11-20T10:00:00Z\",\"can_uninstall\":true},"
		    "{\"id\":\"open-conv\",\"name\":\"Open conversion provider\",\"class\":\"conversion_provider\","
		    "\"version\":\"0.5.0\",\"install_time\":\"2026-11-21T10:00:00Z\",\"can_uninstall\":false},"
		    "{\"id\":\"browser\",\"name\":\"DisplayXR Browser\",\"class\":\"app\",\"version\":\"1.10.5\","
		    "\"install_time\":\"2026-11-19T10:00:00Z\",\"can_uninstall\":false}]}\n");
		return 0;
	}
	if (strcmp(catalog, "slow") == 0) {
		nap_ms(5000);
	}
	if (strcmp(catalog, "offline") == 0) {
		if (json) {
			printf(
			    "{\"offline\":true,\"reason\":\"catalog unreachable or expired\",\"skipped\":[\"%s\"],"
			    "\"retry\":\"dxr-get %s\"}\n",
			    arg != NULL ? arg : cmd, cmd);
		}
		return 10;
	}
	if (strcmp(cmd, "list") == 0) {
		if (strcmp(catalog, "garbage") == 0) {
			printf("this is not JSON\n");
			return 0;
		}
		if (!json) {
			printf("ID CLASS INSTALLED AVAILABLE STATE\n");
			return 0;
		}
		printf("{\"schema\":2,\"items\":[" ROW_RUNTIME "," ROW_LEIA "," ROW_CONV "," ROW_BROWSER "," ROW_LINKOUT
		       "]}\n");
		return 0;
	}
	if (strcmp(cmd, "outdated") == 0) {
		if (strcmp(catalog, "current") == 0) {
			printf("{\"schema\":2,\"outdated\":[]}\n");
			return 0;
		}
		printf("{\"schema\":2,\"outdated\":[" ROW_RUNTIME "," ROW_LEIA "," ROW_LINKOUT "]}\n");
		return 0;
	}
	if (strcmp(cmd, "install") == 0) {
		const char *id = use_default ? "--default" : arg;
		if (id == NULL) {
			return 2;
		}
		static const struct
		{
			const char *id;
			int code;
		} plain[] = {{"badsig", 11}, {"dlfail", 20}, {"verify", 21}, {"child", 30}};
		for (size_t i = 0; i < sizeof(plain) / sizeof(plain[0]); i++) {
			if (strcmp(id, plain[i].id) == 0) {
				return plain[i].code;
			}
		}
		if (strcmp(id, "incompat") == 0 || (strcmp(id, "runtime") == 0 && no_runtime)) {
			if (json) {
				plan(id, "skip", 41, "needs runtime 3.0.0, 2.60.0 is installed");
			}
			return 41;
		}
		if (strcmp(id, "browser") == 0) {
			if (json) {
				plan(id, "none", 0, NULL);
			}
			return 0;
		}
		if (strcmp(id, "nofetch") == 0) {
			if (json) {
				plan(id, "skip", 40, "no signed Windows build");
			}
			return is_admin() ? 40 : 50;
		}
		if (json) {
			plan(id, strcmp(id, "open-conv") == 0 ? "install" : "upgrade", 0, NULL);
		} else {
			printf("PLAN TEXT for %s\n", id);
		}
		return is_admin() ? 0 : 50;
	}
	if (strcmp(cmd, "uninstall") == 0 && arg != NULL) {
		int code = 42;
		const char *result = "not_installed";
		const char *detail = "dxr-get has no receipt for it";
		if (strcmp(arg, "browser") == 0) {
			code = 43;
			result = "no_uninstall_command";
			detail = "remove it from Windows Settings";
		} else if (strcmp(arg, "leia-sr") == 0) {
			code = is_admin() ? 0 : 50;
			result = is_admin() ? "uninstalled" : "needs_elevation";
			detail = is_admin() ? "Leia SR display processor 2.22.0 removed" : "run it elevated";
		}
		if (json) {
			printf("{\"id\":\"%s\",\"result\":\"%s\",\"exit\":%d,\"detail\":\"%s\"}\n", arg, result, code,
			       detail);
		}
		return code;
	}
	return 2;
}
