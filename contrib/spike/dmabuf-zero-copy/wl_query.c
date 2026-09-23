// Copyright 2026, DisplayXR contributors.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief Print the dma-buf format/modifier pairs the Wayland compositor advertises
 *        (zwp_linux_dmabuf_v1 default feedback on v4+, modifier events on v3) for
 *        ARGB8888 / XRGB8888 / ABGR8888. Binds globals only: no surface, no window.
 */
#include "common.h"

#include "linux-dmabuf-v1-client.h"

#include <drm_fourcc.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>
#include <wayland-client.h>
#include <xf86drm.h>

static struct zwp_linux_dmabuf_v1 *g_dmabuf;
static uint32_t g_version;
/* --modlist=FOURCC: print ONLY the comma-separated modifier list of the main-device
 * tranche(s) for that format (exactly what a Wayland client would feed to gbm). */
static uint32_t g_modlist_fourcc;
static int g_modlist_n;
static dev_t g_main_dev, g_tranche_dev;
static const uint32_t want[] = {DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888, DRM_FORMAT_ABGR8888};

static int
wanted(uint32_t f)
{
	for (unsigned i = 0; i < 3; i++)
		if (want[i] == f) return 1;
	return 0;
}

static void
pr(uint32_t fmt, uint64_t mod, const char *extra)
{
	if (g_modlist_fourcc) {
		if (fmt == g_modlist_fourcc)
			printf("%s0x%016llx", g_modlist_n++ ? "," : "", (unsigned long long)mod);
		return;
	}
	char *n = drmGetFormatModifierName(mod), *v = drmGetFormatModifierVendor(mod);
	printf("  %c%c%c%c  0x%016llx  %s_%s%s\n", fmt & 0xff, (fmt >> 8) & 0xff, (fmt >> 16) & 0xff, fmt >> 24,
	       (unsigned long long)mod, v ? v : "?", n ? n : "?", extra);
	free(n), free(v);
}

/* v3 */
static void
on_format(void *d, struct zwp_linux_dmabuf_v1 *z, uint32_t f)
{}
static void
on_modifier(void *d, struct zwp_linux_dmabuf_v1 *z, uint32_t f, uint32_t hi, uint32_t lo)
{
	if (wanted(f)) pr(f, ((uint64_t)hi << 32) | lo, "");
}
static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {on_format, on_modifier};

/* v4 feedback */
struct fb
{
	void *table;
	uint32_t table_size;
	int tranche;
	uint32_t flags;
	dev_t target;
};
struct entry
{
	uint32_t fmt, pad;
	uint64_t mod;
};

static void
fb_done(void *d, struct zwp_linux_dmabuf_feedback_v1 *f)
{
	if (!g_modlist_fourcc)
		printf("feedback done\n");
}
static void
fb_table(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, int32_t fd, uint32_t size)
{
	struct fb *s = d;
	s->table = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	s->table_size = size;
	close(fd);
	if (!g_modlist_fourcc)
		printf("format table: %u entries\n", size / 16);
}
static void
print_dev(const char *what, struct wl_array *a)
{
	if (g_modlist_fourcc)
		return;
	dev_t dv;
	memcpy(&dv, a->data, sizeof(dv));
	drmDevicePtr dd = NULL;
	char node[64] = "?";
	if (drmGetDeviceFromDevId(dv, 0, &dd) == 0) {
		if (dd->available_nodes & (1 << DRM_NODE_RENDER))
			snprintf(node, sizeof(node), "%s", dd->nodes[DRM_NODE_RENDER]);
		drmFreeDevice(&dd);
	}
	if (!g_modlist_fourcc)
		printf("%s: %u:%u (%s)\n", what, major(dv), minor(dv), node);
}
static void
fb_main(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, struct wl_array *a)
{
	memcpy(&g_main_dev, a->data, sizeof(dev_t));
	print_dev("main_device", a);
}
static void
fb_tdone(void *d, struct zwp_linux_dmabuf_feedback_v1 *f)
{
	((struct fb *)d)->tranche++;
}
static void
fb_ttarget(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, struct wl_array *a)
{
	struct fb *s = d;
	memcpy(&g_tranche_dev, a->data, sizeof(dev_t));
	if (!g_modlist_fourcc)
		printf("-- tranche %d --\n", s->tranche);
	print_dev("  target_device", a);
}
static void
fb_tformats(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, struct wl_array *a)
{
	struct fb *s = d;
	uint16_t *idx;
	struct entry *e = s->table;
	int shown = 0;
	wl_array_for_each(idx, a)
	{
		if (!e || *idx >= s->table_size / 16) continue;
		if (g_modlist_fourcc && g_tranche_dev != g_main_dev)
			continue; /* main-device tranches only */
		if (wanted(e[*idx].fmt)) pr(e[*idx].fmt, e[*idx].mod, ""), shown++;
	}
	if (!g_modlist_fourcc)
		printf("  (%zu formats in tranche, %d shown for ARGB/XRGB/ABGR8888)\n", a->size / 2, shown);
}
static void
fb_tflags(void *d, struct zwp_linux_dmabuf_feedback_v1 *f, uint32_t flags)
{
	if (!g_modlist_fourcc)
		printf("  tranche flags: 0x%x%s\n", flags, (flags & 1) ? " (SCANOUT)" : "");
}
static const struct zwp_linux_dmabuf_feedback_v1_listener fb_listener = {fb_done, fb_table, fb_main, fb_tdone,
                                                                         fb_ttarget, fb_tformats, fb_tflags};

static void
reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
	if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name)) {
		g_version = ver < 5 ? ver : 5;
		if (getenv("WLQ_FORCE_V3") && g_version > 3) g_version = 3;
		g_dmabuf = wl_registry_bind(r, name, &zwp_linux_dmabuf_v1_interface, g_version);
	}
}
static void
reg_remove(void *d, struct wl_registry *r, uint32_t name)
{}
static const struct wl_registry_listener reg_listener = {reg_global, reg_remove};

int
main(int argc, char **argv)
{
	if (argc > 1 && !strncmp(argv[1], "--modlist=", 10)) {
		const char *f = argv[1] + 10;
		g_modlist_fourcc = !strcasecmp(f, "ARGB8888") ? DRM_FORMAT_ARGB8888
		                   : !strcasecmp(f, "XRGB8888") ? DRM_FORMAT_XRGB8888
		                   : !strcasecmp(f, "ABGR8888") ? DRM_FORMAT_ABGR8888
		                                                : 0;
	}
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "wl_query: cannot connect to $WAYLAND_DISPLAY\n");
		return 1;
	}
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(dpy);
	if (!g_dmabuf) {
		fprintf(stderr, "wl_query: no zwp_linux_dmabuf_v1\n");
		return 1;
	}
	if (!g_modlist_fourcc)
		printf("zwp_linux_dmabuf_v1 bound at version %u\n", g_version);
	if (g_version >= 4) {
		struct fb s = {0};
		struct zwp_linux_dmabuf_feedback_v1 *f = zwp_linux_dmabuf_v1_get_default_feedback(g_dmabuf);
		zwp_linux_dmabuf_feedback_v1_add_listener(f, &fb_listener, &s);
		wl_display_roundtrip(dpy);
		wl_display_roundtrip(dpy);
	} else {
		zwp_linux_dmabuf_v1_add_listener(g_dmabuf, &dmabuf_listener, NULL);
		wl_display_roundtrip(dpy);
		wl_display_roundtrip(dpy);
	}
	wl_display_disconnect(dpy);
	if (g_modlist_fourcc)
		printf("\n");
	return 0;
}
