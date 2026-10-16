// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  desktop_control_probe — headless CLI over XR_DXR_desktop_control (ADR-056).
 *
 * A plain OpenXR app (XR_MND_headless: no graphics, no window) that enables
 * XR_DXR_desktop_control and exposes every call, for bring-up and end-to-end
 * runs on Windows, macOS and Linux:
 *
 *   desktop_control_probe state
 *   desktop_control_probe request [reason...]        waits for the outcome event
 *   desktop_control_probe list
 *   desktop_control_probe pointer
 *   desktop_control_probe pick X Y
 *   desktop_control_probe text X Y
 *   desktop_control_probe act TYPE key=value...      waits for the completion event
 *   desktop_control_probe script FILE                one command per line, one session
 *
 * act TYPE: activate | move | minimize | restore | maximize | close | type | key |
 *           move-pointer | click | drag | scroll
 * act keys: target=ID  x=  y=  x2=  y2=  rect=X,Y,W,H  button=left|right|middle
 *           count=N  sx=  sy=  text="..."  keys=ctrl+s
 *
 * Script-only commands (E2E runs):
 *   run <shell command>         e.g. `run displayxr-cli desktop stop`
 *   sleep MS
 *   expect-state STATE          fail unless xrGetDesktopControlStateDXR says STATE
 *   expect RESULT               fail unless the last act ended with RESULT
 *                               (XR_SUCCESS, XR_ERROR_DESKTOP_CONTROL_REFUSED_DXR, ...)
 *   expect-text X Y "TEXT"      fail unless the text at X Y is TEXT
 *   # comment
 *
 * Exit code: 0, or the number of failed commands — in a script, failed expectations
 * (an `act` there is judged by the `expect` after it); standalone, a failed `act`.
 *
 * WARNING: against a real backend this DOES operate the desktop (after the
 * OS / DisplayXR consent). Use DXR_DESKTOP_BACKEND=fake for a dry run.
 */

#include <openxr/openxr.h>
#include <openxr/XR_DXR_desktop_control.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#define XR_MND_HEADLESS_NAME "XR_MND_headless"

namespace {

XrInstance g_instance = XR_NULL_HANDLE;
XrSession g_session = XR_NULL_HANDLE;
XrResult g_last_act = XR_SUCCESS;
int g_failures = 0;
//! In a script an `act` outcome is judged by the following `expect`, not by its result.
bool g_in_script = false;

PFN_xrGetDesktopControlStateDXR pGetState;
PFN_xrRequestDesktopControlDXR pRequest;
PFN_xrEnumerateDesktopWindowsDXR pEnumWindows;
PFN_xrGetDesktopPointerDXR pGetPointer;
PFN_xrPickDesktopDXR pPick;
PFN_xrGetDesktopTextAtDXR pTextAt;
PFN_xrSubmitDesktopActionDXR pSubmit;

const char *
state_name(XrDesktopControlStateDXR s)
{
	switch (s) {
	case XR_DESKTOP_CONTROL_STATE_UNSUPPORTED_DXR: return "UNSUPPORTED";
	case XR_DESKTOP_CONTROL_STATE_NOT_REQUESTED_DXR: return "NOT_REQUESTED";
	case XR_DESKTOP_CONTROL_STATE_PENDING_DXR: return "PENDING";
	case XR_DESKTOP_CONTROL_STATE_GRANTED_DXR: return "GRANTED";
	case XR_DESKTOP_CONTROL_STATE_DENIED_DXR: return "DENIED";
	case XR_DESKTOP_CONTROL_STATE_DISABLED_BY_USER_DXR: return "DISABLED_BY_USER";
	case XR_DESKTOP_CONTROL_STATE_SUSPENDED_DXR: return "SUSPENDED";
	default: return "?";
	}
}

std::string
result_name(XrResult r)
{
	char buf[XR_MAX_RESULT_STRING_SIZE] = {0};
	if (g_instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(g_instance, r, buf))) {
		return buf;
	}
	return std::to_string((int)r);
}

const char *
pick_kind(XrDesktopPickKindDXR k)
{
	switch (k) {
	case XR_DESKTOP_PICK_KIND_WINDOW_DXR: return "window";
	case XR_DESKTOP_PICK_KIND_DESKTOP_DXR: return "desktop";
	case XR_DESKTOP_PICK_KIND_SHELL_DXR: return "shell";
	case XR_DESKTOP_PICK_KIND_OUTSIDE_DXR: return "outside";
	default: return "?";
	}
}

void
print_window(const XrDesktopWindowDXR &w)
{
	printf("  id=%llu parent=%llu pid=%u z=%u state=0x%llx frame=(%d,%d %dx%d) app=\"%s\" title=\"%s\"\n",
	       (unsigned long long)w.windowId, (unsigned long long)w.parentWindowId, w.pid, w.zOrder,
	       (unsigned long long)w.state, w.frame.offset.x, w.frame.offset.y, w.frame.extent.width,
	       w.frame.extent.height, w.appId, w.title);
}

/*!
 * Drain events. Returns true when @p want_action matched a completion (and sets
 * @p out_result) or @p want_state_resolved saw a non-PENDING state event.
 */
bool
pump(uint64_t want_action, XrResult *out_result, bool want_state_resolved)
{
	bool hit = false;
	for (;;) {
		XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
		XrResult r = xrPollEvent(g_instance, &ev);
		if (r != XR_SUCCESS) {
			return hit;
		}
		if (ev.type == XR_TYPE_EVENT_DATA_DESKTOP_CONTROL_STATE_CHANGED_DXR) {
			auto *e = reinterpret_cast<XrEventDataDesktopControlStateChangedDXR *>(&ev);
			printf("event: state -> %s\n", state_name(e->state));
			if (want_state_resolved && e->state != XR_DESKTOP_CONTROL_STATE_PENDING_DXR) {
				hit = true;
			}
		} else if (ev.type == XR_TYPE_EVENT_DATA_DESKTOP_ACTION_COMPLETED_DXR) {
			auto *e = reinterpret_cast<XrEventDataDesktopActionCompletedDXR *>(&ev);
			printf("event: action %llu -> %s \"%s\"\n", (unsigned long long)e->actionId,
			       result_name(e->result).c_str(), e->message);
			if (e->actionId == want_action) {
				*out_result = e->result;
				hit = true;
			}
		}
	}
}

bool
wait_for(uint64_t action, XrResult *result, bool state_resolved, int timeout_ms)
{
	auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
	while (std::chrono::steady_clock::now() < end) {
		if (pump(action, result, state_resolved)) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return false;
}

XrDesktopControlStateDXR
get_state(XrDesktopControlCapabilityFlagsDXR *caps)
{
	XrDesktopControlStateDXR s = XR_DESKTOP_CONTROL_STATE_UNSUPPORTED_DXR;
	XrResult r = pGetState(g_session, &s, caps);
	if (XR_FAILED(r)) {
		printf("xrGetDesktopControlStateDXR: %s\n", result_name(r).c_str());
		g_failures++;
	}
	return s;
}

//! Split a line into tokens; "double quotes" group, key="v a l" keeps the key.
std::vector<std::string>
tokenize(const std::string &line)
{
	std::vector<std::string> out;
	std::string cur;
	bool in_q = false, have = false;
	for (char ch : line) {
		if (ch == '"') {
			in_q = !in_q;
			have = true;
		} else if ((ch == ' ' || ch == '\t') && !in_q) {
			if (have) {
				out.push_back(cur);
			}
			cur.clear();
			have = false;
		} else {
			cur += ch;
			have = true;
		}
	}
	if (have) {
		out.push_back(cur);
	}
	return out;
}

bool
parse_type(const std::string &t, XrDesktopActionTypeDXR *out)
{
	static const struct
	{
		const char *n;
		XrDesktopActionTypeDXR t;
	} table[] = {
	    {"activate", XR_DESKTOP_ACTION_TYPE_ACTIVATE_DXR},
	    {"move", XR_DESKTOP_ACTION_TYPE_MOVE_RESIZE_DXR},
	    {"minimize", XR_DESKTOP_ACTION_TYPE_MINIMIZE_DXR},
	    {"restore", XR_DESKTOP_ACTION_TYPE_RESTORE_DXR},
	    {"maximize", XR_DESKTOP_ACTION_TYPE_MAXIMIZE_DXR},
	    {"close", XR_DESKTOP_ACTION_TYPE_CLOSE_DXR},
	    {"type", XR_DESKTOP_ACTION_TYPE_TYPE_TEXT_DXR},
	    {"key", XR_DESKTOP_ACTION_TYPE_KEY_CHORD_DXR},
	    {"move-pointer", XR_DESKTOP_ACTION_TYPE_POINTER_MOVE_DXR},
	    {"click", XR_DESKTOP_ACTION_TYPE_CLICK_DXR},
	    {"drag", XR_DESKTOP_ACTION_TYPE_DRAG_DXR},
	    {"scroll", XR_DESKTOP_ACTION_TYPE_SCROLL_DXR},
	};
	for (const auto &e : table) {
		if (t == e.n) {
			*out = e.t;
			return true;
		}
	}
	return false;
}

int
cmd_act(const std::vector<std::string> &args)
{
	if (args.size() < 2) {
		printf("act: TYPE required\n");
		return 1;
	}
	XrDesktopActionDXR a = {XR_TYPE_DESKTOP_ACTION_DXR};
	if (!parse_type(args[1], &a.actionType)) {
		printf("act: unknown type '%s'\n", args[1].c_str());
		return 1;
	}
	std::string text, keys;
	bool have_text = false, have_keys = false;
	for (size_t i = 2; i < args.size(); i++) {
		size_t eq = args[i].find('=');
		if (eq == std::string::npos) {
			printf("act: expected key=value, got '%s'\n", args[i].c_str());
			return 1;
		}
		std::string k = args[i].substr(0, eq), v = args[i].substr(eq + 1);
		long n = strtol(v.c_str(), nullptr, 10);
		if (k == "target" || k == "id") {
			a.targetWindowId = strtoull(v.c_str(), nullptr, 10);
		} else if (k == "x") {
			a.point.x = (int32_t)n;
		} else if (k == "y") {
			a.point.y = (int32_t)n;
		} else if (k == "x2") {
			a.point2.x = (int32_t)n;
		} else if (k == "y2") {
			a.point2.y = (int32_t)n;
		} else if (k == "rect") {
			int x = 0, y = 0, w = 0, h = 0;
			sscanf(v.c_str(), "%d,%d,%d,%d", &x, &y, &w, &h);
			a.rect = {{x, y}, {w, h}};
		} else if (k == "button") {
			a.button = v == "right"    ? XR_DESKTOP_POINTER_BUTTON_RIGHT_DXR
			           : v == "middle" ? XR_DESKTOP_POINTER_BUTTON_MIDDLE_DXR
			                           : XR_DESKTOP_POINTER_BUTTON_LEFT_DXR;
		} else if (k == "count") {
			a.clickCount = (uint32_t)n;
		} else if (k == "sx") {
			a.scrollX = (int32_t)n;
		} else if (k == "sy") {
			a.scrollY = (int32_t)n;
		} else if (k == "text") {
			text = v;
			have_text = true;
		} else if (k == "keys") {
			keys = v;
			have_keys = true;
		} else {
			printf("act: unknown key '%s'\n", k.c_str());
			return 1;
		}
	}
	a.text = have_text ? text.c_str() : nullptr;
	a.keys = have_keys ? keys.c_str() : nullptr;

	uint64_t id = 0;
	XrResult r = pSubmit(g_session, &a, &id);
	if (XR_FAILED(r)) {
		printf("act %s: submit -> %s\n", args[1].c_str(), result_name(r).c_str());
		g_last_act = r;
		return g_in_script ? 0 : 1;
	}
	printf("act %s: queued as %llu\n", args[1].c_str(), (unsigned long long)id);
	XrResult done = XR_SUCCESS;
	if (!wait_for(id, &done, false, 30000)) {
		printf("act %s: no completion event within 30 s\n", args[1].c_str());
		g_last_act = XR_TIMEOUT_EXPIRED;
		return 1;
	}
	g_last_act = done;
	return g_in_script || done == XR_SUCCESS ? 0 : 1;
}

int
cmd_text(int32_t x, int32_t y, std::string *out)
{
	uint32_t n = 0;
	XrResult r;
	std::vector<char> buf;
	for (int attempt = 0; attempt < 4; attempt++) {
		r = pTextAt(g_session, {x, y}, 0, &n, nullptr);
		if (XR_FAILED(r)) {
			break;
		}
		buf.assign(n, '\0');
		r = pTextAt(g_session, {x, y}, n, &n, buf.data());
		if (r != XR_ERROR_SIZE_INSUFFICIENT) {
			break; // live UI: grown text between the calls -> retry
		}
	}
	if (XR_FAILED(r)) {
		printf("text: %s\n", result_name(r).c_str());
		return 1;
	}
	*out = buf.data();
	return 0;
}

int run_line(const std::string &line);

int
run_command(const std::vector<std::string> &args)
{
	const std::string &c = args[0];
	if (c == "state") {
		XrDesktopControlCapabilityFlagsDXR caps = 0;
		XrDesktopControlStateDXR s = get_state(&caps);
		printf("state: %s caps=0x%llx\n", state_name(s), (unsigned long long)caps);
		pump(0, nullptr, false);
		return 0;
	}
	if (c == "request") {
		std::string reason;
		for (size_t i = 1; i < args.size(); i++) {
			reason += (i > 1 ? " " : "") + args[i];
		}
		XrDesktopControlRequestInfoDXR info = {XR_TYPE_DESKTOP_CONTROL_REQUEST_INFO_DXR};
		info.reason = reason.c_str();
		XrResult r = pRequest(g_session, reason.empty() ? nullptr : &info);
		printf("request: %s\n", result_name(r).c_str());
		if (XR_FAILED(r)) {
			return 1;
		}
		if (!wait_for(0, nullptr, true, 120000)) {
			printf("request: no outcome within 120 s\n");
		}
		printf("state: %s\n", state_name(get_state(nullptr)));
		return 0;
	}
	if (c == "list") {
		uint32_t n = 0;
		XrResult r = pEnumWindows(g_session, 0, &n, nullptr);
		if (XR_SUCCEEDED(r)) {
			std::vector<XrDesktopWindowDXR> w(n, {XR_TYPE_DESKTOP_WINDOW_DXR});
			r = pEnumWindows(g_session, n, &n, w.data());
			if (XR_SUCCEEDED(r)) {
				printf("windows: %u\n", n);
				for (uint32_t i = 0; i < n; i++) {
					print_window(w[i]);
				}
			}
		}
		if (XR_FAILED(r)) {
			printf("list: %s\n", result_name(r).c_str());
			return 1;
		}
		return 0;
	}
	if (c == "pointer" || c == "pick") {
		XrDesktopPickDXR p = {XR_TYPE_DESKTOP_PICK_DXR};
		XrResult r;
		if (c == "pointer") {
			r = pGetPointer(g_session, &p);
		} else {
			if (args.size() < 3) {
				printf("pick X Y\n");
				return 1;
			}
			r = pPick(g_session, {atoi(args[1].c_str()), atoi(args[2].c_str())}, &p);
		}
		if (XR_FAILED(r)) {
			printf("%s: %s\n", c.c_str(), result_name(r).c_str());
			return 1;
		}
		printf("%s: (%d,%d) %s\n", c.c_str(), p.point.x, p.point.y, pick_kind(p.kind));
		if (p.kind == XR_DESKTOP_PICK_KIND_WINDOW_DXR) {
			print_window(p.window);
		}
		return 0;
	}
	if (c == "text") {
		if (args.size() < 3) {
			printf("text X Y\n");
			return 1;
		}
		std::string t;
		if (cmd_text(atoi(args[1].c_str()), atoi(args[2].c_str()), &t) != 0) {
			return 1;
		}
		printf("text: \"%s\"\n", t.c_str());
		return 0;
	}
	if (c == "act") {
		return cmd_act(args);
	}
	if (c == "sleep") {
		std::this_thread::sleep_for(std::chrono::milliseconds(args.size() > 1 ? atoi(args[1].c_str()) : 100));
		pump(0, nullptr, false);
		return 0;
	}
	if (c == "run") {
		std::string cmd;
		for (size_t i = 1; i < args.size(); i++) {
			cmd += (i > 1 ? " " : "") + args[i];
		}
		printf("run: %s\n", cmd.c_str());
		fflush(stdout);
		int rc = system(cmd.c_str());
		fflush(stdout);
		return rc == 0 ? 0 : 1;
	}
	if (c == "expect-state") {
		XrDesktopControlStateDXR s = get_state(nullptr);
		pump(0, nullptr, false);
		bool ok = args.size() > 1 && args[1] == state_name(s);
		printf("expect-state %s: %s (is %s)\n", args.size() > 1 ? args[1].c_str() : "?", ok ? "PASS" : "FAIL",
		       state_name(s));
		return ok ? 0 : 1;
	}
	if (c == "expect") {
		bool ok = args.size() > 1 && args[1] == result_name(g_last_act);
		printf("expect %s: %s (was %s)\n", args.size() > 1 ? args[1].c_str() : "?", ok ? "PASS" : "FAIL",
		       result_name(g_last_act).c_str());
		return ok ? 0 : 1;
	}
	if (c == "expect-text") {
		std::string t;
		bool ok = args.size() > 3 && cmd_text(atoi(args[1].c_str()), atoi(args[2].c_str()), &t) == 0 &&
		          t == args[3];
		printf("expect-text \"%s\": %s (is \"%s\")\n", args.size() > 3 ? args[3].c_str() : "?",
		       ok ? "PASS" : "FAIL", t.c_str());
		return ok ? 0 : 1;
	}
	if (c == "script") {
		if (args.size() < 2) {
			printf("script FILE\n");
			return 1;
		}
		std::ifstream f(args[1]);
		if (!f) {
			printf("script: cannot open %s\n", args[1].c_str());
			return 1;
		}
		int fails = 0;
		g_in_script = true;
		std::string line;
		while (std::getline(f, line)) {
			fails += run_line(line);
		}
		return fails;
	}
	printf("unknown command '%s'\n", c.c_str());
	return 1;
}

int
run_line(const std::string &line)
{
	std::string l = line;
	while (!l.empty() && (l.back() == '\r' || l.back() == '\n')) {
		l.pop_back();
	}
	std::vector<std::string> args = tokenize(l);
	if (args.empty() || args[0][0] == '#') {
		return 0;
	}
	printf("> %s\n", l.c_str());
	fflush(stdout);
	int rc = run_command(args);
	fflush(stdout);
	return rc;
}

bool
setup()
{
	uint32_t n = 0;
	xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
	std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
	xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data());
	bool have_dc = false, have_headless = false;
	for (auto &p : props) {
		have_dc |= strcmp(p.extensionName, XR_DXR_DESKTOP_CONTROL_EXTENSION_NAME) == 0;
		have_headless |= strcmp(p.extensionName, XR_MND_HEADLESS_NAME) == 0;
	}
	if (!have_dc || !have_headless) {
		fprintf(stderr, "runtime lacks %s%s\n", have_dc ? "" : XR_DXR_DESKTOP_CONTROL_EXTENSION_NAME " ",
		        have_headless ? "" : XR_MND_HEADLESS_NAME);
		return false;
	}

	const char *exts[] = {XR_MND_HEADLESS_NAME, XR_DXR_DESKTOP_CONTROL_EXTENSION_NAME};
	XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
	snprintf(ici.applicationInfo.applicationName, sizeof(ici.applicationInfo.applicationName),
	         "desktop_control_probe");
	ici.applicationInfo.applicationVersion = 1;
	snprintf(ici.applicationInfo.engineName, sizeof(ici.applicationInfo.engineName), "none");
	ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	ici.enabledExtensionCount = 2;
	ici.enabledExtensionNames = exts;
	XrResult r = xrCreateInstance(&ici, &g_instance);
	if (XR_FAILED(r)) {
		fprintf(stderr, "xrCreateInstance: %d\n", (int)r);
		return false;
	}

#define GET(name, var)                                                                                                 \
	if (XR_FAILED(xrGetInstanceProcAddr(g_instance, #name, (PFN_xrVoidFunction *)&var)) || var == nullptr) {       \
		fprintf(stderr, "missing %s\n", #name);                                                                \
		return false;                                                                                          \
	}
	GET(xrGetDesktopControlStateDXR, pGetState);
	GET(xrRequestDesktopControlDXR, pRequest);
	GET(xrEnumerateDesktopWindowsDXR, pEnumWindows);
	GET(xrGetDesktopPointerDXR, pGetPointer);
	GET(xrPickDesktopDXR, pPick);
	GET(xrGetDesktopTextAtDXR, pTextAt);
	GET(xrSubmitDesktopActionDXR, pSubmit);
#undef GET

	XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
	sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XrSystemId sys = XR_NULL_SYSTEM_ID;
	r = xrGetSystem(g_instance, &sgi, &sys);
	if (XR_FAILED(r)) {
		fprintf(stderr, "xrGetSystem: %s\n", result_name(r).c_str());
		return false;
	}
	XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
	sci.systemId = sys;
	r = xrCreateSession(g_instance, &sci, &g_session);
	if (XR_FAILED(r)) {
		fprintf(stderr, "xrCreateSession (headless): %s\n", result_name(r).c_str());
		return false;
	}
	return true;
}

} // namespace

int
main(int argc, char **argv)
{
	setvbuf(stdout, nullptr, _IOLBF, 0);
	if (argc < 2) {
		fprintf(stderr,
		        "usage: desktop_control_probe state | request [reason] | list | pointer | pick X Y |\n"
		        "       text X Y | act TYPE key=value... | script FILE\n");
		return 2;
	}
	if (!setup()) {
		return 3;
	}
	std::vector<std::string> args(argv + 1, argv + argc);
	int rc = run_command(args);
	g_failures += rc;

	xrDestroySession(g_session);
	xrDestroyInstance(g_instance);
	return g_failures > 255 ? 255 : g_failures;
}
