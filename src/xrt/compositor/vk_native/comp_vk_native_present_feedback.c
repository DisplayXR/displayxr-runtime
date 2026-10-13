// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Presentation feedback from the window system (#902, desktop Linux).
 * @ingroup comp_vk_native
 */

#include "comp_vk_native_present_feedback.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

#include <xcb/xcb.h>
#include <xcb/xcbext.h>

#ifdef DXR_HAVE_WP_PRESENTATION
#include <wayland-client.h>
#include "presentation-time-client-protocol.h"
#endif

#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum pf_kind
{
	PF_KIND_WAYLAND,
	PF_KIND_XCB,
};

//! Samples that arrived but have not been polled yet. A frame drains it every
//! present, and a compositor answers within a few refreshes, so 32 is slack.
#define PF_RING 32

//! Outstanding Wayland feedback objects. Each is answered (presented or
//! discarded) within a few refreshes; hitting the cap means the compositor
//! stopped answering, and then requesting more would only leak.
#define PF_MAX_PENDING 16

#ifdef DXR_HAVE_WP_PRESENTATION
struct pf_pending
{
	struct comp_vk_native_present_feedback *pf;
	struct wp_presentation_feedback *fb;
	uint64_t tag;
	bool in_use;
};
#endif

struct comp_vk_native_present_feedback
{
	enum pf_kind kind;
	pthread_mutex_t mutex;

	struct comp_vk_native_present_feedback_event ring[PF_RING];
	uint32_t ring_head; //!< next to read
	uint32_t ring_count;
	uint64_t ring_overflow;

	//! The connection broke (protocol error / X I/O error): stop touching it.
	bool dead;

#ifdef DXR_HAVE_WP_PRESENTATION
	struct wl_display *display;
	struct wl_surface *surface;
	struct wl_event_queue *queue;
	struct wl_display *wrapper; //!< display proxy wrapper bound to @ref queue
	struct wl_registry *registry;
	struct wp_presentation *presentation;
	uint32_t presentation_version;
	//! wp_presentation.clock_id; -1 until it arrives.
	int64_t clock_id;
	struct pf_pending pending[PF_MAX_PENDING];
	uint64_t pending_full;
#endif

	xcb_connection_t *xcb;
	xcb_window_t window;
	uint32_t eid;
	xcb_special_event_t *special;
	uint32_t special_stamp;
};

static void
pf_push(struct comp_vk_native_present_feedback *pf, const struct comp_vk_native_present_feedback_event *ev)
{
	if (pf->ring_count == PF_RING) {
		// Oldest out: the grid only moves forward, so the newest matters most.
		pf->ring_head = (pf->ring_head + 1) % PF_RING;
		pf->ring_count--;
		pf->ring_overflow++;
	}
	pf->ring[(pf->ring_head + pf->ring_count) % PF_RING] = *ev;
	pf->ring_count++;
}

static uint64_t
pf_clock_ns(clockid_t clk)
{
	struct timespec ts;
	if (clock_gettime(clk, &ts) != 0) {
		return 0;
	}
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}


/*
 *
 * Wayland: wp_presentation.
 *
 */

#ifdef DXR_HAVE_WP_PRESENTATION

static void
pf_wl_presentation_clock_id(void *data, struct wp_presentation *p, uint32_t clk_id)
{
	(void)p;
	struct comp_vk_native_present_feedback *pf = data;
	pf->clock_id = (int64_t)clk_id;
}

static const struct wp_presentation_listener pf_wl_presentation_listener = {
    .clock_id = pf_wl_presentation_clock_id,
};

static void
pf_wl_pending_release(struct pf_pending *pp)
{
	if (pp->fb != NULL) {
		wp_presentation_feedback_destroy(pp->fb);
	}
	pp->fb = NULL;
	pp->in_use = false;
}

static void
pf_wl_fb_sync_output(void *data, struct wp_presentation_feedback *fb, struct wl_output *output)
{
	(void)data;
	(void)fb;
	(void)output;
}

static void
pf_wl_fb_presented(void *data,
                   struct wp_presentation_feedback *fb,
                   uint32_t tv_sec_hi,
                   uint32_t tv_sec_lo,
                   uint32_t tv_nsec,
                   uint32_t refresh,
                   uint32_t seq_hi,
                   uint32_t seq_lo,
                   uint32_t flags)
{
	(void)fb;
	struct pf_pending *pp = data;
	struct comp_vk_native_present_feedback *pf = pp->pf;

	struct comp_vk_native_present_feedback_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.tag = pp->tag;

	const uint64_t sec = ((uint64_t)tv_sec_hi << 32) | tv_sec_lo;
	uint64_t t = sec * 1000000000ULL + tv_nsec;
	if (pf->clock_id < 0) {
		t = 0; // no clock domain yet: unusable, counted as a discard
	} else if ((clockid_t)pf->clock_id != CLOCK_MONOTONIC && t != 0) {
		// Another domain (MONOTONIC_RAW, BOOTTIME, ...): translate through
		// "now" in both. The runtime's clock is os_monotonic_get_ns().
		const uint64_t mono = pf_clock_ns(CLOCK_MONOTONIC);
		const uint64_t other = pf_clock_ns((clockid_t)pf->clock_id);
		if (mono == 0 || other == 0) {
			t = 0;
		} else {
			t = t + mono - other;
		}
	}
	ev.sample.present_ns = t;
	ev.sample.refresh_ns = refresh;
	ev.sample.seq = ((uint64_t)seq_hi << 32) | seq_lo;
	// The protocol's "no counter" is 0.
	ev.sample.seq_valid = ev.sample.seq != 0;
	ev.sample.vsync = (flags & WP_PRESENTATION_FEEDBACK_KIND_VSYNC) != 0;
	ev.sample.hw_clock = (flags & WP_PRESENTATION_FEEDBACK_KIND_HW_CLOCK) != 0;
	ev.sample.discarded = (t == 0);
	pf_push(pf, &ev);

	// presented is a destructor event: the server already dropped it.
	pf_wl_pending_release(pp);
}

static void
pf_wl_fb_discarded(void *data, struct wp_presentation_feedback *fb)
{
	(void)fb;
	struct pf_pending *pp = data;
	struct comp_vk_native_present_feedback_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.tag = pp->tag;
	ev.sample.discarded = true;
	pf_push(pp->pf, &ev);
	pf_wl_pending_release(pp);
}

static const struct wp_presentation_feedback_listener pf_wl_fb_listener = {
    .sync_output = pf_wl_fb_sync_output,
    .presented = pf_wl_fb_presented,
    .discarded = pf_wl_fb_discarded,
};

static void
pf_wl_registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *iface, uint32_t version)
{
	struct comp_vk_native_present_feedback *pf = data;
	if (strcmp(iface, wp_presentation_interface.name) == 0 && pf->presentation == NULL) {
		pf->presentation_version = version < 2 ? version : 2;
		pf->presentation =
		    wl_registry_bind(registry, name, &wp_presentation_interface, pf->presentation_version);
		if (pf->presentation != NULL) {
			wp_presentation_add_listener(pf->presentation, &pf_wl_presentation_listener, pf);
		}
	}
}

static void
pf_wl_registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener pf_wl_registry_listener = {
    .global = pf_wl_registry_global,
    .global_remove = pf_wl_registry_global_remove,
};

/*!
 * Read whatever the socket already holds into the queues and dispatch ours.
 * Never waits for the compositor: poll() with a zero timeout decides whether
 * there is anything to read at all.
 *
 * wl_display_read_events() can wait for OTHER threads that have prepared a
 * read (the app's event thread) to finish theirs — but it is only called when
 * the fd is already readable, so those threads' own poll() returns at once
 * too. This is the pattern libwayland documents for multi-threaded readers,
 * and the one the Vulkan WSI itself uses on the same connection.
 */
static void
pf_wl_pump(struct comp_vk_native_present_feedback *pf)
{
	struct wl_display *d = pf->display;
	if (pf->dead) {
		return;
	}
	while (wl_display_prepare_read_queue(d, pf->queue) != 0) {
		if (wl_display_dispatch_queue_pending(d, pf->queue) < 0) {
			pf->dead = true;
			return;
		}
	}
	(void)wl_display_flush(d); // EAGAIN is fine: the WSI flushes every commit
	struct pollfd p = {.fd = wl_display_get_fd(d), .events = POLLIN, .revents = 0};
	if (poll(&p, 1, 0) > 0 && (p.revents & POLLIN) != 0) {
		if (wl_display_read_events(d) < 0) {
			pf->dead = true;
			return;
		}
	} else {
		wl_display_cancel_read(d);
	}
	if (wl_display_dispatch_queue_pending(d, pf->queue) < 0) {
		pf->dead = true;
	}
}

#endif // DXR_HAVE_WP_PRESENTATION

struct comp_vk_native_present_feedback *
comp_vk_native_present_feedback_create_wayland(void *wl_display, void *wl_surface)
{
#ifdef DXR_HAVE_WP_PRESENTATION
	if (wl_display == NULL || wl_surface == NULL) {
		return NULL;
	}
	struct comp_vk_native_present_feedback *pf = U_TYPED_CALLOC(struct comp_vk_native_present_feedback);
	if (pf == NULL) {
		return NULL;
	}
	pf->kind = PF_KIND_WAYLAND;
	pthread_mutex_init(&pf->mutex, NULL);
	pf->display = (struct wl_display *)wl_display;
	pf->surface = (struct wl_surface *)wl_surface;
	pf->clock_id = -1;

	// A private queue on the APP's connection (the one the surface lives on):
	// the app dispatches the default queue on its own thread, and none of our
	// objects' events may ever reach its listeners.
	pf->queue = wl_display_create_queue(pf->display);
	pf->wrapper = wl_proxy_create_wrapper(pf->display);
	if (pf->queue == NULL || pf->wrapper == NULL) {
		goto fail;
	}
	wl_proxy_set_queue((struct wl_proxy *)pf->wrapper, pf->queue);
	pf->registry = wl_display_get_registry(pf->wrapper);
	wl_registry_add_listener(pf->registry, &pf_wl_registry_listener, pf);
	// Creation-time only: one round trip for the globals, a second for the
	// clock_id event the compositor sends on bind.
	if (wl_display_roundtrip_queue(pf->display, pf->queue) < 0) {
		goto fail;
	}
	if (pf->presentation == NULL) {
		U_LOG_W(
		    "#902: present-feedback feeder — the Wayland compositor has no wp_presentation; "
		    "the vblank grid stays unfed and late weave dormant");
		goto fail;
	}
	if (wl_display_roundtrip_queue(pf->display, pf->queue) < 0) {
		goto fail;
	}
	U_LOG_W(
	    "#902: present-feedback feeder — wp_presentation v%u on the app's wl_display, "
	    "clock %s (%lld)",
	    pf->presentation_version,
	    pf->clock_id == CLOCK_MONOTONIC ? "MONOTONIC"
	    : pf->clock_id < 0              ? "UNKNOWN"
	                                    : "other, translated",
	    (long long)pf->clock_id);
	return pf;

fail:
	comp_vk_native_present_feedback_destroy(&pf);
	return NULL;
#else
	(void)wl_display;
	(void)wl_surface;
	U_LOG_W(
	    "#902: present-feedback feeder — built without wp_presentation (wayland-scanner "
	    "missing at build time); the vblank grid stays unfed on Wayland");
	return NULL;
#endif
}


/*
 *
 * X11: the Present extension, encoded directly.
 *
 * Two fixed-size protocol structs are all this needs, so they are written out
 * here rather than linking libxcb-present: a new system library is a new
 * soname on the .deb's cross-release list (scripts/package_deb_linux.sh).
 * Layouts are the protocol's (presentproto, "PresentSelectInput" and
 * "PresentCompleteNotify"), as libxcb-present generates them.
 *
 */

//! Process-wide: libxcb assigns global_id on first use and caches the
//! extension's opcode per connection under it.
static xcb_extension_t pf_xcb_present_ext = {"Present", 0};

#define PF_XCB_PRESENT_SELECT_INPUT 3
#define PF_XCB_PRESENT_EVENT_COMPLETE_NOTIFY 1
#define PF_XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY 2
#define PF_XCB_PRESENT_COMPLETE_KIND_PIXMAP 0
#define PF_XCB_PRESENT_COMPLETE_MODE_COPY 0
#define PF_XCB_PRESENT_COMPLETE_MODE_FLIP 1
#define PF_XCB_PRESENT_COMPLETE_MODE_SKIP 2
#define PF_XCB_PRESENT_COMPLETE_MODE_SUBOPTIMAL_COPY 3

struct pf_xcb_select_input_request
{
	uint8_t major_opcode;
	uint8_t minor_opcode;
	uint16_t length;
	uint32_t eid;
	uint32_t window;
	uint32_t event_mask;
};

//! As libxcb delivers it: a generic event with full_sequence spliced in after
//! the first 32 bytes.
struct __attribute__((packed)) pf_xcb_complete_notify_event
{
	uint8_t response_type;
	uint8_t extension;
	uint16_t sequence;
	uint32_t length;
	uint16_t event_type;
	uint8_t kind;
	uint8_t mode;
	uint32_t event;
	uint32_t window;
	uint32_t serial;
	uint64_t ust;
	uint32_t full_sequence;
	uint64_t msc;
};

static unsigned int
pf_xcb_select_input(xcb_connection_t *c, uint32_t eid, xcb_window_t window, uint32_t mask)
{
	static const xcb_protocol_request_t req = {
	    .count = 2,
	    .ext = &pf_xcb_present_ext,
	    .opcode = PF_XCB_PRESENT_SELECT_INPUT,
	    .isvoid = 1,
	};
	struct pf_xcb_select_input_request out;
	memset(&out, 0, sizeof(out));
	out.eid = eid;
	out.window = window;
	out.event_mask = mask;
	// xcb_send_request needs two writable iovecs BEFORE the ones it is given.
	struct iovec parts[4];
	parts[2].iov_base = (char *)&out;
	parts[2].iov_len = sizeof(out);
	parts[3].iov_base = 0;
	parts[3].iov_len = -parts[2].iov_len & 3;
	// CHECKED: an error (BadWindow on a window that went away first) lands on
	// this cookie instead of the connection's event queue — on an Xlib app's
	// connection the default error handler would otherwise exit the app.
	return xcb_send_request(c, XCB_REQUEST_CHECKED, parts + 2, &req);
}

struct comp_vk_native_present_feedback *
comp_vk_native_present_feedback_create_xcb(void *xcb_connection, uint32_t window)
{
	xcb_connection_t *c = (xcb_connection_t *)xcb_connection;
	if (c == NULL || window == 0 || xcb_connection_has_error(c)) {
		return NULL;
	}
	const xcb_query_extension_reply_t *ext = xcb_get_extension_data(c, &pf_xcb_present_ext);
	if (ext == NULL || !ext->present) {
		U_LOG_W(
		    "#902: present-feedback feeder — the X server has no Present extension; "
		    "the vblank grid stays unfed and late weave dormant");
		return NULL;
	}

	struct comp_vk_native_present_feedback *pf = U_TYPED_CALLOC(struct comp_vk_native_present_feedback);
	if (pf == NULL) {
		return NULL;
	}
	pf->kind = PF_KIND_XCB;
	pthread_mutex_init(&pf->mutex, NULL);
	pf->xcb = c;
	pf->window = window;
	pf->eid = xcb_generate_id(c);

	// Register BEFORE selecting, so not one CompleteNotify can reach the
	// connection's ordinary event queue (the app's, for an Xlib window).
	pf->special = xcb_register_for_special_xge(c, &pf_xcb_present_ext, pf->eid, &pf->special_stamp);
	if (pf->special == NULL) {
		U_LOG_W("#902: present-feedback feeder — could not register for Present events");
		goto fail;
	}
	xcb_void_cookie_t ck = {pf_xcb_select_input(c, pf->eid, window, PF_XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY)};
	// Creation-time only: one round trip to learn whether the selection held.
	xcb_generic_error_t *err = xcb_request_check(c, ck);
	if (err != NULL) {
		U_LOG_W(
		    "#902: present-feedback feeder — PresentSelectInput on window 0x%x failed "
		    "(X error %u)",
		    window, err->error_code);
		free(err);
		goto fail;
	}
	U_LOG_W(
	    "#902: present-feedback feeder — X11 Present CompleteNotify on window 0x%x "
	    "(ust/msc, CLOCK_MONOTONIC)",
	    window);
	return pf;

fail:
	comp_vk_native_present_feedback_destroy(&pf);
	return NULL;
}

static void
pf_xcb_pump(struct comp_vk_native_present_feedback *pf)
{
	if (pf->dead) {
		return;
	}
	if (xcb_connection_has_error(pf->xcb)) {
		pf->dead = true;
		return;
	}
	// xcb_poll_for_special_event reads what the socket holds and never waits.
	xcb_generic_event_t *ev;
	while ((ev = xcb_poll_for_special_event(pf->xcb, pf->special)) != NULL) {
		const struct pf_xcb_complete_notify_event *cn = (const struct pf_xcb_complete_notify_event *)ev;
		if (cn->event_type == PF_XCB_PRESENT_EVENT_COMPLETE_NOTIFY &&
		    cn->kind == PF_XCB_PRESENT_COMPLETE_KIND_PIXMAP) {
			struct comp_vk_native_present_feedback_event out;
			memset(&out, 0, sizeof(out));
			// ust is CLOCK_MONOTONIC microseconds (the DRM vblank clock).
			out.sample.present_ns = cn->ust * 1000ULL;
			out.sample.seq = cn->msc;
			out.sample.seq_valid = true;
			out.sample.hw_clock = true;
			// FLIP and COPY both execute at a vblank (the copy is scheduled
			// on the target msc, and ust/msc are that vblank's). SKIP never
			// reached the window.
			out.sample.vsync = cn->mode == PF_XCB_PRESENT_COMPLETE_MODE_FLIP ||
			                   cn->mode == PF_XCB_PRESENT_COMPLETE_MODE_COPY ||
			                   cn->mode == PF_XCB_PRESENT_COMPLETE_MODE_SUBOPTIMAL_COPY;
			out.sample.discarded = cn->mode == PF_XCB_PRESENT_COMPLETE_MODE_SKIP || cn->ust == 0;
			pf_push(pf, &out);
		}
		free(ev);
	}
}


/*
 *
 * Common.
 *
 */

const char *
comp_vk_native_present_feedback_kind(const struct comp_vk_native_present_feedback *pf)
{
	if (pf == NULL) {
		return "none";
	}
	return pf->kind == PF_KIND_WAYLAND ? "wp_presentation" : "x11-present";
}

void
comp_vk_native_present_feedback_request(struct comp_vk_native_present_feedback *pf, uint64_t tag)
{
	if (pf == NULL || pf->kind != PF_KIND_WAYLAND) {
		return;
	}
#ifdef DXR_HAVE_WP_PRESENTATION
	pthread_mutex_lock(&pf->mutex);
	if (!pf->dead) {
		struct pf_pending *slot = NULL;
		for (uint32_t i = 0; i < PF_MAX_PENDING; i++) {
			if (!pf->pending[i].in_use) {
				slot = &pf->pending[i];
				break;
			}
		}
		if (slot == NULL) {
			pf->pending_full++;
		} else {
			slot->pf = pf;
			slot->tag = tag;
			slot->in_use = true;
			// Associates with the surface's NEXT commit — the one the WSI
			// makes inside the vkQueuePresentKHR that follows. The request
			// itself is buffered; the WSI's own flush sends it with the commit.
			slot->fb = wp_presentation_feedback(pf->presentation, pf->surface);
			if (slot->fb == NULL) {
				slot->in_use = false;
			} else {
				wp_presentation_feedback_add_listener(slot->fb, &pf_wl_fb_listener, slot);
			}
		}
	}
	pthread_mutex_unlock(&pf->mutex);
#else
	(void)tag;
#endif
}

uint32_t
comp_vk_native_present_feedback_poll(struct comp_vk_native_present_feedback *pf,
                                     struct comp_vk_native_present_feedback_event *out,
                                     uint32_t max)
{
	if (pf == NULL || out == NULL || max == 0) {
		return 0;
	}
	pthread_mutex_lock(&pf->mutex);
	if (pf->kind == PF_KIND_XCB) {
		pf_xcb_pump(pf);
	}
#ifdef DXR_HAVE_WP_PRESENTATION
	else {
		pf_wl_pump(pf);
	}
#endif
	uint32_t n = 0;
	while (n < max && pf->ring_count > 0) {
		out[n++] = pf->ring[pf->ring_head];
		pf->ring_head = (pf->ring_head + 1) % PF_RING;
		pf->ring_count--;
	}
	pthread_mutex_unlock(&pf->mutex);
	return n;
}

void
comp_vk_native_present_feedback_destroy(struct comp_vk_native_present_feedback **pf_ptr)
{
	if (pf_ptr == NULL || *pf_ptr == NULL) {
		return;
	}
	struct comp_vk_native_present_feedback *pf = *pf_ptr;

	if (pf->kind == PF_KIND_XCB && pf->xcb != NULL) {
		if (pf->special != NULL) {
			if (!xcb_connection_has_error(pf->xcb)) {
				// Deselect first, as the Vulkan WSI does, so the server stops
				// sending to an eid nobody listens on. The error (if the
				// window is already gone) is discarded, never delivered.
				unsigned int seq = pf_xcb_select_input(pf->xcb, pf->eid, pf->window, 0);
				xcb_discard_reply(pf->xcb, seq);
				xcb_flush(pf->xcb);
			}
			xcb_unregister_for_special_event(pf->xcb, pf->special);
		}
	}

#ifdef DXR_HAVE_WP_PRESENTATION
	if (pf->kind == PF_KIND_WAYLAND) {
		for (uint32_t i = 0; i < PF_MAX_PENDING; i++) {
			if (pf->pending[i].in_use) {
				pf_wl_pending_release(&pf->pending[i]);
			}
		}
		if (pf->presentation != NULL) {
			wp_presentation_destroy(pf->presentation);
		}
		if (pf->registry != NULL) {
			wl_registry_destroy(pf->registry);
		}
		if (pf->wrapper != NULL) {
			wl_proxy_wrapper_destroy(pf->wrapper);
		}
		if (pf->display != NULL) {
			wl_display_flush(pf->display);
		}
		if (pf->queue != NULL) {
			wl_event_queue_destroy(pf->queue);
		}
	}
#endif

	pthread_mutex_destroy(&pf->mutex);
	free(pf);
	*pf_ptr = NULL;
}
