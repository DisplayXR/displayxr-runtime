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
 *   desktop_backend_smoke scroll DX DY
 *
 * It bypasses EVERY safety check of the common layer (consent, kill switch, idle
 * wait, target verification, audit): it is for the person sitting at the machine.
 */

#include "u_desktop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static volatile int g_done = 0;
static enum u_desktop_access g_access = U_DESKTOP_ACCESS_UNKNOWN;

static void
on_access(void *ctx, enum u_desktop_access a)
{
	(void)ctx;
	g_access = a;
	g_done = 1;
}

static int
done(enum u_desktop_result r, const char *msg)
{
	printf("result: %s%s%s\n", res_str(r), msg[0] ? " — " : "", msg);
	return r == U_DESKTOP_OK ? 0 : 1;
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
		struct u_desktop_app_info app = {argv[0], "desktop_backend_smoke", "bring-up test"};
		b->request_access(b, &app, on_access, NULL);
		for (int i = 0; i < 1200 && !g_done; i++) {
			sleep_ms(100);
		}
		printf("access: %s\n", g_done ? access_str(g_access) : "no answer in 120 s");
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
		r = b->window_op(b, (enum u_desktop_window_op)op, id, &rect, msg);
		rc = done(r, msg);
	} else if (strcmp(cmd, "type") == 0 && argc >= 3) {
		r = b->type_text(b, argv[2], msg);
		rc = done(r, msg);
	} else if (strcmp(cmd, "key") == 0 && argc >= 4) {
		struct u_desktop_chord c = {0};
		c.mods = (uint32_t)strtoul(argv[2], NULL, 0);
		snprintf(c.key, sizeof(c.key), "%s", argv[3]);
		r = b->key_chord(b, &c, msg);
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
		r = b->pointer_move(b, atoi(argv[2]), atoi(argv[3]), msg);
		for (int i = 0; r == U_DESKTOP_OK && i < count; i++) {
			r = b->pointer_button(b, btn, true, msg);
			if (r == U_DESKTOP_OK) {
				r = b->pointer_button(b, btn, false, msg);
			}
		}
		rc = done(r, msg);
	} else if (strcmp(cmd, "scroll") == 0 && argc >= 4) {
		r = b->scroll(b, atoi(argv[2]), atoi(argv[3]), msg);
		rc = done(r, msg);
	} else {
		fprintf(stderr, "unknown command / missing arguments: %s\n", cmd);
		rc = 2;
	}

	b->destroy(b);
	return rc;
}
