// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Drive the platform desktop-control backend directly — no OpenXR, no
 *         policy layer. A developer / bring-up tool for u_desktop_*.{c,cpp,m}.
 *
 *   desktop_backend_smoke info                     name, caps, access, locked, idle
 *   desktop_backend_smoke request                  ask for access (prompts!)
 *   desktop_backend_smoke list                     windows, top-most first
 *   desktop_backend_smoke pointer                  pointer + pick under it
 *   desktop_backend_smoke pick X Y
 *   desktop_backend_smoke focused
 *   desktop_backend_smoke text X Y
 *   desktop_backend_smoke op activate|minimize|restore|maximize|close WINDOW_ID
 *   desktop_backend_smoke op move WINDOW_ID X Y [W H]
 *   desktop_backend_smoke type "text"
 *   desktop_backend_smoke key MODS KEY            MODS: bits ctrl=1 shift=2 alt=4 super=8
 *   desktop_backend_smoke move X Y
 *   desktop_backend_smoke click X Y [left|right|middle] [count]
 *   desktop_backend_smoke scroll DX DY            at the current pointer
 * Spec v3 (held input; the key stays down until keyup — in a script, one process):
 *   desktop_backend_smoke keydown NAME            one key or modifier: shift | alt | tab | a ...
 *   desktop_backend_smoke keyup NAME
 *   desktop_backend_smoke buttondown X Y [left|right|middle]   move there, press, keep it down
 *   desktop_backend_smoke buttonup X Y [left|right|middle]     move there (a drag), release
 *   desktop_backend_smoke modclick MODS X Y [left|right|middle]  MODS bits held around one click
 *   desktop_backend_smoke target ID               (script) pin the guard's target; 0 = auto;
 *                                                 `shell` = the spec v3 shell target
 *   desktop_backend_smoke parent [HANDLE]         (script) the consent dialog's parent window for
 *                                                 request / request-scopes: wayland:H | x11:HEX
 *                                                 (unvalidated here); none = clear. A
 *                                                 handle with spaces: "double-quote" it
 *                                                 (in a script too); any bytes at all:
 *   desktop_backend_smoke parent-hex HEX          (script) the same, hex of the string
 *   desktop_backend_smoke request-scopes control|capture|all[+again]   spec v2 consent (prompts!)
 *   desktop_backend_smoke monitors                every monitor, desktop coordinates
 *   desktop_backend_smoke capture X Y W H [OUT.ppm]  one still (native density); stats only,
 *                                                 the pixels go to OUT.ppm only when given
 *   desktop_backend_smoke sck-stream on|off       (macOS, script) take later captures through a
 *                                                 one-shot SCStream, the macOS 12.3–13 path
 *   desktop_backend_smoke script < FILE           one command per line, ONE process (so an
 *                                                 OS grant from `request` lasts for the rest;
 *                                                 needed on GNOME, where the portal grant
 *                                                 outlives the process only for a registered
 *                                                 app id — DXR_DESKTOP_APP_ID, spec §6.4).
 *                                                 Lines: the same commands, quotes for
 *                                                 spaces, `sleep MS`, `#` comments.
 *
 * It bypasses EVERY safety check of the common layer (consent, kill switch, idle
 * wait, target verification, audit): it is for the person sitting at the machine.
 */

#include "u_desktop.h"
#include "u_desktop_chord.h"
#ifdef __APPLE__
#include "u_desktop_macos_capture.h"
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
static void
sleep_ms(unsigned ms)
{
	Sleep(ms);
}
#else
#include <unistd.h>
static void
sleep_ms(unsigned ms)
{
	usleep(ms * 1000u);
}
#endif

static const char *
res_str(enum u_desktop_result r)
{
	switch (r) {
	case U_DESKTOP_OK: return "ok";
	case U_DESKTOP_ERR_UNSUPPORTED: return "unsupported";
	case U_DESKTOP_ERR_NOT_GRANTED: return "not-granted";
	case U_DESKTOP_ERR_WINDOW_LOST: return "window-lost";
	case U_DESKTOP_ERR_REFUSED: return "refused";
	case U_DESKTOP_ERR_TIMEOUT: return "timeout";
	default: return "failed";
	}
}

static const char *
access_str(enum u_desktop_access a)
{
	switch (a) {
	case U_DESKTOP_ACCESS_GRANTED: return "granted";
	case U_DESKTOP_ACCESS_DENIED: return "denied";
	case U_DESKTOP_ACCESS_PENDING: return "pending";
	default: return "unknown";
	}
}

static void
print_window(const struct u_desktop_window *w)
{
	printf("  id=%llu parent=%llu pid=%u z=%u state=0x%x frame=(%d,%d %dx%d) app=\"%s\" title=\"%s\"\n",
	       (unsigned long long)w->id, (unsigned long long)w->parent_id, w->pid, w->z, w->state, w->frame.x,
	       w->frame.y, w->frame.w, w->frame.h, w->app_id, w->title);
}

static void
print_pick(const struct u_desktop_pick *p)
{
	static const char *kinds[] = {"window", "desktop", "shell", "outside"};
	printf("pick (%d,%d): %s\n", p->x, p->y, (unsigned)p->kind < 4 ? kinds[p->kind] : "?");
	if (p->kind == U_DESKTOP_PICK_WINDOW) {
		print_window(&p->window);
	}
}

//! The smoke tool has no policy layer: the guard always allows, and targets the
//! focused window (text / keys) or the window under the point (buttons / scroll),
//! read at the moment the command starts. Set `target ID` in a script to pin one.
static uint64_t g_target = 0;
//! `parent HANDLE`: the consent dialog's parent window for request / request-scopes.
static char g_parent[U_DESKTOP_PARENT_WINDOW_SIZE] = "";
static bool
smoke_still_allowed(void *ctx)
{
	(void)ctx;
	return true;
}
static struct u_desktop_input_guard
guard_for_focus(struct u_desktop_backend *b)
{
	uint64_t id = g_target;
	if (id == 0 && b->focused_window) {
		b->focused_window(b, &id);
	}
	struct u_desktop_input_guard g = {id, smoke_still_allowed, NULL, NULL};
	return g;
}
static struct u_desktop_input_guard
guard_for_point(struct u_desktop_backend *b, int32_t x, int32_t y)
{
	uint64_t id = g_target;
	struct u_desktop_pick p;
	if (id == 0 && b->pick && b->pick(b, x, y, &p) == U_DESKTOP_OK && p.kind == U_DESKTOP_PICK_WINDOW) {
		id = p.window.id;
	}
	struct u_desktop_input_guard g = {id, smoke_still_allowed, NULL, NULL};
	return g;
}

static volatile int g_done = 0;
static enum u_desktop_access g_access = U_DESKTOP_ACCESS_UNKNOWN;

static void
on_access(void *ctx, enum u_desktop_access a)
{
	(void)ctx;
	g_access = a;
	g_done = 1;
}

static volatile int g_scopes_done = 0;
static enum u_desktop_access g_ctl = U_DESKTOP_ACCESS_UNKNOWN, g_cap = U_DESKTOP_ACCESS_UNKNOWN;

static void
on_scopes(void *ctx, enum u_desktop_access control, enum u_desktop_access capture)
{
	(void)ctx;
	g_ctl = control;
	g_cap = capture;
	g_scopes_done = 1;
}

static uint64_t
smoke_now_ms(void)
{
	struct timespec ts;
	timespec_get(&ts, TIME_UTC);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int
done(enum u_desktop_result r, const char *msg)
{
	printf("result: %s%s%s\n", res_str(r), msg[0] ? " — " : "", msg);
	return r == U_DESKTOP_OK ? 0 : 1;
}

static enum u_desktop_button
parse_button(int argc, char **argv, int i)
{
	if (argc > i && strcmp(argv[i], "right") == 0) {
		return U_DESKTOP_BUTTON_RIGHT;
	}
	if (argc > i && strcmp(argv[i], "middle") == 0) {
		return U_DESKTOP_BUTTON_MIDDLE;
	}
	return U_DESKTOP_BUTTON_LEFT;
}

static int
run_cmd(struct u_desktop_backend *b, int argc, char **argv)
{
	const char *cmd = argv[1];
	char msg[U_DESKTOP_MSG_SIZE] = {0};
	enum u_desktop_result r = U_DESKTOP_ERR_UNSUPPORTED;
	int rc = 0;

	if (strcmp(cmd, "info") == 0) {
		uint64_t idle = 0;
		bool have_idle = b->user_idle_ms && b->user_idle_ms(b, &idle);
		printf("backend=%s caps=0x%x remembers_in_store=%d access=%s locked=%d idle=%s%llu ms\n", b->name,
		       b->caps, b->remembers_in_store, b->access_status ? access_str(b->access_status(b)) : "?",
		       b->is_locked ? b->is_locked(b) : -1, have_idle ? "" : "unknown/",
		       (unsigned long long)idle);
	} else if (strcmp(cmd, "request") == 0) {
		struct u_desktop_app_info app = {argv[0], "desktop_backend_smoke", "bring-up test", g_parent};
		// Each request waits for ITS answer (the REPL issues several).
		g_done = 0;
		g_access = U_DESKTOP_ACCESS_UNKNOWN;
		b->request_access(b, &app, on_access, NULL);
		for (int i = 0; i < 1200 && !g_done; i++) {
			sleep_ms(100);
		}
		printf("access: %s\n", g_done ? access_str(g_access) : "no answer in 120 s");
		// As the common layer does once GRANTED (GNOME: window titles, extension v18).
		if (g_done && g_access == U_DESKTOP_ACCESS_GRANTED && b->grant_changed != NULL) {
			b->grant_changed(b, true);
		}
	} else if (strcmp(cmd, "list") == 0) {
		struct u_desktop_window ws[256];
		uint32_t n = 0;
		r = b->list_windows(b, ws, 256, &n);
		printf("%u window(s)\n", n);
		for (uint32_t i = 0; i < n && i < 256; i++) {
			print_window(&ws[i]);
		}
		rc = done(r, msg);
	} else if (strcmp(cmd, "pointer") == 0) {
		int32_t x = 0, y = 0;
		r = b->get_pointer(b, &x, &y);
		printf("pointer: (%d,%d)\n", x, y);
		if (r == U_DESKTOP_OK) {
			struct u_desktop_pick p;
			r = b->pick(b, x, y, &p);
			if (r == U_DESKTOP_OK) {
				print_pick(&p);
			}
		}
		rc = done(r, msg);
	} else if (strcmp(cmd, "pick") == 0 && argc >= 4) {
		struct u_desktop_pick p;
		r = b->pick(b, atoi(argv[2]), atoi(argv[3]), &p);
		if (r == U_DESKTOP_OK) {
			print_pick(&p);
		}
		rc = done(r, msg);
	} else if (strcmp(cmd, "focused") == 0) {
		uint64_t id = 0;
		r = b->focused_window(b, &id);
		printf("focused: %llu\n", (unsigned long long)id);
		rc = done(r, msg);
	} else if (strcmp(cmd, "text") == 0 && argc >= 4) {
		char buf[4096] = {0};
		r = b->text_at(b, atoi(argv[2]), atoi(argv[3]), buf, sizeof(buf), msg);
		printf("text: \"%s\"\n", buf);
		rc = done(r, msg);
	} else if (strcmp(cmd, "op") == 0 && argc >= 4) {
		static const char *ops[] = {"activate", "move", "minimize", "restore", "maximize", "close"};
		int op = -1;
		for (int i = 0; i < 6; i++) {
			if (strcmp(argv[2], ops[i]) == 0) {
				op = i;
			}
		}
		if (op < 0) {
			fprintf(stderr, "unknown op %s\n", argv[2]);
			return 2;
		}
		uint64_t id = strtoull(argv[3], NULL, 10);
		struct u_desktop_rect rect = {0, 0, 0, 0};
		if (op == U_DESKTOP_OP_MOVE_RESIZE) {
			if (argc < 6) {
				return 2;
			}
			rect.x = atoi(argv[4]);
			rect.y = atoi(argv[5]);
			rect.w = argc > 6 ? atoi(argv[6]) : 0;
			rect.h = argc > 7 ? atoi(argv[7]) : 0;
		}
		struct u_desktop_input_guard g = {id, smoke_still_allowed, NULL, NULL};
		if (b->prepare_input) {
			b->prepare_input(b, msg);
		}
		r = b->window_op(b, &g, (enum u_desktop_window_op)op, id, &rect, msg);
		rc = done(r, msg);
	} else if (strcmp(cmd, "type") == 0 && argc >= 3) {
		if (b->prepare_input) {
			b->prepare_input(b, msg);
		}
		struct u_desktop_input_guard g = guard_for_focus(b);
		r = b->type_text(b, &g, argv[2], msg);
		rc = done(r, msg);
	} else if (strcmp(cmd, "key") == 0 && argc >= 4) {
		struct u_desktop_chord c = {0};
		c.mods = (uint32_t)strtoul(argv[2], NULL, 0);
		snprintf(c.key, sizeof(c.key), "%s", argv[3]);
		for (char *k = c.key; *k; k++) {
			if (*k >= 'A' && *k <= 'Z') {
				*k = (char)(*k - 'A' + 'a'); // canonical, as u_desktop_chord_parse makes it
			}
		}
		if (b->prepare_input) {
			b->prepare_input(b, msg);
		}
		struct u_desktop_input_guard g = guard_for_focus(b);
		r = b->key_chord(b, &g, &c, msg);
		rc = done(r, msg);
	} else if (strcmp(cmd, "move") == 0 && argc >= 4) {
		r = b->pointer_move(b, atoi(argv[2]), atoi(argv[3]), msg);
		rc = done(r, msg);
	} else if (strcmp(cmd, "click") == 0 && argc >= 4) {
		enum u_desktop_button btn = U_DESKTOP_BUTTON_LEFT;
		if (argc > 4 && strcmp(argv[4], "right") == 0) {
			btn = U_DESKTOP_BUTTON_RIGHT;
		} else if (argc > 4 && strcmp(argv[4], "middle") == 0) {
			btn = U_DESKTOP_BUTTON_MIDDLE;
		}
		int count = argc > 5 ? atoi(argv[5]) : 1;
		int32_t x = atoi(argv[2]), y = atoi(argv[3]);
		if (b->prepare_input) {
			b->prepare_input(b, msg);
		}
		struct u_desktop_input_guard g = guard_for_point(b, x, y);
		r = b->pointer_move(b, x, y, msg);
		for (int i = 0; r == U_DESKTOP_OK && i < count; i++) {
			r = b->pointer_button(b, &g, btn, true, x, y, msg);
			if (r == U_DESKTOP_OK) {
				r = b->pointer_button(b, &g, btn, false, x, y, msg);
			}
		}
		rc = done(r, msg);
	} else if (strcmp(cmd, "request-scopes") == 0 && argc >= 3) {
		uint32_t scopes = strncmp(argv[2], "capture", 7) == 0 ? U_DESKTOP_SCOPE_CAPTURE
		                  : strncmp(argv[2], "all", 3) == 0 ? U_DESKTOP_SCOPE_CONTROL | U_DESKTOP_SCOPE_CAPTURE
		                                                    : U_DESKTOP_SCOPE_CONTROL;
		if (strstr(argv[2], "+again") != NULL) {
			scopes |= U_DESKTOP_SCOPE_REASK;
		}
		if (b->request_scopes == NULL) {
			printf("this backend has no request_scopes (no capture)\n");
			return 1;
		}
		struct u_desktop_app_info app = {argv[0], "desktop_backend_smoke", "bring-up test", g_parent};
		g_scopes_done = 0;
		b->request_scopes(b, &app, scopes, on_scopes, NULL);
		for (int i = 0; i < 1200 && !g_scopes_done; i++) {
			sleep_ms(100);
		}
		printf("access: control %s, capture %s\n", g_scopes_done ? access_str(g_ctl) : "no answer",
		       g_scopes_done ? access_str(g_cap) : "no answer");
		if (g_scopes_done && g_ctl == U_DESKTOP_ACCESS_GRANTED && b->grant_changed != NULL) {
			b->grant_changed(b, true); // as after `request`
		}
		printf("now: control %s, capture %s\n", b->access_status ? access_str(b->access_status(b)) : "?",
		       b->capture_access_status ? access_str(b->capture_access_status(b)) : "?");
	} else if (strcmp(cmd, "monitors") == 0) {
		struct u_desktop_rect m[16];
		uint32_t n = 0;
		r = b->monitors != NULL ? b->monitors(b, m, 16, &n, msg) : U_DESKTOP_ERR_UNSUPPORTED;
		for (uint32_t i = 0; i < n && i < 16 && r == U_DESKTOP_OK; i++) {
			printf("  monitor %u: (%d,%d %dx%d)\n", i, m[i].x, m[i].y, m[i].w, m[i].h);
		}
		rc = done(r, msg);
	} else if (strcmp(cmd, "capture") == 0 && argc >= 6) {
		struct u_desktop_rect src = {atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5])};
		struct u_desktop_image img;
		memset(&img, 0, sizeof(img));
		uint64_t t0 = smoke_now_ms();
		r = b->capture != NULL ? b->capture(b, &src, &img, msg) : U_DESKTOP_ERR_UNSUPPORTED;
		uint64_t t1 = smoke_now_ms();
		if (r == U_DESKTOP_OK) {
			size_t withheld = 0;
			for (size_t i = 0; i < (size_t)img.w * img.h; i++) {
				withheld += img.rgba[i * 4 + 3] == 0 ? 1 : 0;
			}
			printf("capture: %ux%u in %llu ms, %zu withheld px\n", img.w, img.h,
			       (unsigned long long)(t1 - t0), withheld);
			if (argc >= 7) {
				// Only on request: the person running the tool asked for the pixels.
				FILE *f = fopen(argv[6], "wb");
				if (f != NULL) {
					fprintf(f, "P6\n%u %u\n255\n", img.w, img.h);
					for (size_t i = 0; i < (size_t)img.w * img.h; i++) {
						fwrite(img.rgba + i * 4, 1, 3, f);
					}
					fclose(f);
					printf("saved %s\n", argv[6]);
				}
			}
			u_desktop_image_free(&img);
		}
		rc = done(r, msg);
	} else if (strcmp(cmd, "parent") == 0) {
		// Unvalidated on purpose: the backend is driven directly, so a stale or
		// malformed handle can be fed to the portal to see it fall back.
		snprintf(g_parent, sizeof(g_parent), "%s", argc >= 3 ? argv[2] : "");
		printf("parent window: [%s] (%zu bytes)\n", g_parent, strlen(g_parent));
	} else if (strcmp(cmd, "parent-hex") == 0 && argc >= 3) {
		// Any handle round-trips this way, whatever it contains (mutter's handles are
		// random printable ASCII: spaces, quotes, '#').
		const char *h = argv[2];
		size_t n = strlen(h), o = 0;
		bool ok = n % 2 == 0 && n / 2 < sizeof(g_parent);
		char tmp[U_DESKTOP_PARENT_WINDOW_SIZE];
		for (size_t i = 0; ok && i < n; i += 2, o++) {
			unsigned v = 0;
			ok = sscanf(h + i, "%2x", &v) == 1 && isxdigit((unsigned char)h[i]) &&
			     isxdigit((unsigned char)h[i + 1]);
			tmp[o] = (char)v;
		}
		if (!ok) {
			printf("parent-hex: not an even-length hex string (< %d bytes decoded)\n",
			       U_DESKTOP_PARENT_WINDOW_SIZE);
			rc = 1;
		} else {
			tmp[o] = '\0';
			memcpy(g_parent, tmp, o + 1);
			printf("parent window: [%s] (%zu bytes)\n", g_parent, o);
		}
#ifdef __APPLE__
	} else if (strcmp(cmd, "sck-stream") == 0 && argc >= 3) {
		u_desktop_macos_sck_force_stream(strcmp(argv[2], "on") == 0);
		printf("ScreenCaptureKit path: %s\n", strcmp(argv[2], "on") == 0 ? "one-shot SCStream" : "default");
#endif
	} else if ((strcmp(cmd, "keydown") == 0 || strcmp(cmd, "keyup") == 0) && argc >= 3) {
		bool down = strcmp(cmd, "keydown") == 0;
		struct u_desktop_chord c;
		if (!u_desktop_chord_parse(argv[2], &c)) {
			printf("not a key name\n");
			return 2;
		}
		if (b->key_event == NULL) {
			return done(U_DESKTOP_ERR_UNSUPPORTED, "no key_event on this backend");
		}
		if (down && b->prepare_input) {
			b->prepare_input(b, msg);
			msg[0] = '\0';
		}
		struct u_desktop_input_guard g = guard_for_focus(b);
		r = b->key_event(b, &g, &c, down, down, msg);
		rc = done(r, msg);
	} else if ((strcmp(cmd, "buttondown") == 0 || strcmp(cmd, "buttonup") == 0) && argc >= 4) {
		bool down = strcmp(cmd, "buttondown") == 0;
		int32_t x = atoi(argv[2]), y = atoi(argv[3]);
		enum u_desktop_button btn = parse_button(argc, argv, 4);
		if (down && b->prepare_input) {
			b->prepare_input(b, msg);
			msg[0] = '\0';
		}
		struct u_desktop_input_guard g = guard_for_point(b, x, y);
		r = b->pointer_move(b, x, y, msg); // with the button held: a drag motion
		if (r == U_DESKTOP_OK || !down) {
			enum u_desktop_result rb = b->pointer_button(b, &g, btn, down, x, y, msg);
			r = r == U_DESKTOP_OK ? rb : r;
		}
		rc = done(r, msg);
	} else if (strcmp(cmd, "modclick") == 0 && argc >= 5) {
		uint32_t mods = (uint32_t)strtoul(argv[2], NULL, 0) & 0xFu;
		int32_t x = atoi(argv[3]), y = atoi(argv[4]);
		enum u_desktop_button btn = parse_button(argc, argv, 5);
		if (b->key_event == NULL) {
			return done(U_DESKTOP_ERR_UNSUPPORTED, "no key_event on this backend");
		}
		if (b->prepare_input) {
			b->prepare_input(b, msg);
			msg[0] = '\0';
		}
		struct u_desktop_input_guard g = guard_for_point(b, x, y);
		r = b->pointer_move(b, x, y, msg);
		uint32_t pressed = 0;
		for (uint32_t bit = 1; r == U_DESKTOP_OK && bit <= 8; bit <<= 1) {
			if (mods & bit) {
				struct u_desktop_chord k = {bit, {0}};
				r = b->key_event(b, &g, &k, true, false, msg);
				pressed |= r == U_DESKTOP_OK ? bit : 0u;
			}
		}
		if (r == U_DESKTOP_OK) {
			r = b->pointer_button(b, &g, btn, true, x, y, msg);
			if (r == U_DESKTOP_OK) {
				r = b->pointer_button(b, &g, btn, false, x, y, msg);
			}
		}
		for (uint32_t bit = 8; bit != 0; bit >>= 1) {
			if (pressed & bit) {
				struct u_desktop_chord k = {bit, {0}};
				char m2[U_DESKTOP_MSG_SIZE] = {0};
				b->key_event(b, &g, &k, false, false, m2);
			}
		}
		rc = done(r, msg);
	} else if (strcmp(cmd, "target") == 0 && argc >= 3) {
		g_target = strcmp(argv[2], "shell") == 0 ? U_DESKTOP_TARGET_SHELL : strtoull(argv[2], NULL, 10);
		printf("target pinned: %llu (0 = auto)\n", (unsigned long long)g_target);
	} else if (strcmp(cmd, "scroll") == 0 && argc >= 4) {
		int32_t x = 0, y = 0;
		b->get_pointer(b, &x, &y);
		if (b->prepare_input) {
			b->prepare_input(b, msg);
		}
		struct u_desktop_input_guard g = guard_for_point(b, x, y);
		r = b->scroll(b, &g, x, y, atoi(argv[2]), atoi(argv[3]), msg);
		rc = done(r, msg);
	} else {
		fprintf(stderr, "unknown command / missing arguments: %s\n", cmd);
		rc = 2;
	}
	return rc;
}

//! Split @p line in place into argv[1..] (argv[0] = @p prog); "double quotes" group.
static int
tokenize(char *line, const char *prog, char **argv, int max)
{
	int n = 0;
	argv[n++] = (char *)prog;
	char *p = line;
	while (*p != '\0' && n < max) {
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
			p++;
		}
		if (*p == '\0' || *p == '#') {
			break;
		}
		if (*p == '"') {
			argv[n++] = ++p;
			while (*p != '\0' && *p != '"') {
				p++;
			}
		} else {
			argv[n++] = p;
			while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
				p++;
			}
		}
		if (*p != '\0') {
			*p++ = '\0';
		}
	}
	return n;
}

int
main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: see the header of desktop_backend_smoke.c\n");
		return 2;
	}
	struct u_desktop_backend *b = u_desktop_backend_create_platform();
	if (b == NULL) {
		printf("no desktop-control backend on this OS / desktop\n");
		return 3;
	}
	int rc = 0;
	if (strcmp(argv[1], "script") == 0) {
		char line[4096];
		while (fgets(line, sizeof(line), stdin) != NULL) {
			char *args[32];
			int n = tokenize(line, argv[0], args, 32);
			if (n < 2) {
				continue;
			}
			printf("> %s\n", args[1]);
			if (strcmp(args[1], "sleep") == 0 && n >= 3) {
				sleep_ms((unsigned)atoi(args[2]));
				continue;
			}
			int r = run_cmd(b, n, args);
			if (r != 0) {
				rc = r;
			}
			fflush(stdout);
		}
	} else {
		rc = run_cmd(b, argc, argv);
	}
	b->destroy(b);
	return rc;
}
