// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Desktop Linux pad reader: evdev (`/dev/input/event*`).
 *
 * Plain file I/O and ioctls, no libudev or SDL, so the plug-in gains no
 * dependency. A pad is any event device reporting BTN_SOUTH, ABS_X and ABS_Y;
 * the logged-in user can open those through systemd's uaccess rule for
 * joysticks (ID_INPUT_JOYSTICK), which every desktop distro and SteamOS ship.
 *
 * Hot-plug is a rescan, no faster than once a second while no pad is open
 * (the hub polls a disconnected backend slowly anyway). A raw Valve device is
 * held only until something better appears: while one is open the reader
 * rescans every few seconds, so Steam Input's virtual pad (created when Steam
 * starts) takes over from the Deck's hid-steam controls. Which device wins,
 * and what each code means, is @ref gamepad_evdev_rank and
 * gamepad_evdev_map.c.
 *
 * Rumble is EV_FF FF_RUMBLE when the device offers it and could be opened
 * read-write; otherwise it is silently ignored, like a pad with no motors.
 *
 * @ingroup drv_gamepad
 */

#include "xrt/xrt_config_os.h"

#ifdef XRT_OS_LINUX_DESKTOP

#include "gamepad_backend.h"
#include "gamepad_evdev_map.h"

#include "os/os_time.h"
#include "util/u_logging.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define EVDEV_DIR "/dev/input"
#define RESCAN_IDLE_NS (1000LL * 1000 * 1000)    //!< no pad open
#define RESCAN_UPGRADE_NS (3000LL * 1000 * 1000) //!< a rank-1 (raw Valve) pad open
#define BITS_PER_LONG_ (sizeof(unsigned long) * 8)
#define NLONGS(n) (((n) + BITS_PER_LONG_ - 1) / BITS_PER_LONG_)

static struct
{
	int fd;
	int rank;
	bool can_rumble;
	int16_t ff_id; //!< uploaded rumble effect, -1 = none
	uint16_t vendor, product;
	char path[64];
	char name[128];
	char label[160]; //!< for gamepad_backend_name
	struct gamepad_evdev_pad pad;
	int64_t next_scan_ns;
} g = {.fd = -1, .ff_id = -1};

static bool
test_bit(const unsigned long *bits, unsigned bit)
{
	return (bits[bit / BITS_PER_LONG_] >> (bit % BITS_PER_LONG_)) & 1UL;
}

//! Is @p fd a pad? Fills vendor/product.
static bool
is_pad(int fd, struct input_id *id)
{
	unsigned long ev[NLONGS(EV_MAX + 1)] = {0};
	unsigned long keys[NLONGS(KEY_MAX + 1)] = {0};
	unsigned long abs[NLONGS(ABS_MAX + 1)] = {0};
	if (ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev) < 0 || !test_bit(ev, EV_KEY) || !test_bit(ev, EV_ABS)) {
		return false;
	}
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0 || !test_bit(keys, BTN_SOUTH)) {
		return false;
	}
	if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs) < 0 || !test_bit(abs, ABS_X) || !test_bit(abs, ABS_Y)) {
		return false;
	}
	return ioctl(fd, EVIOCGID, id) >= 0;
}

//! The kernel driver bound to /dev/input/<event>, e.g. "xpad"; "" when unknown.
static void
driver_of(const char *event_name, char *out, size_t size)
{
	out[0] = '\0';
	char link[256], target[256];
	snprintf(link, sizeof(link), "/sys/class/input/%s/device/device/driver", event_name);
	const ssize_t n = readlink(link, target, sizeof(target) - 1);
	if (n <= 0) {
		return;
	}
	target[n] = '\0';
	const char *base = strrchr(target, '/');
	snprintf(out, size, "%s", base != NULL ? base + 1 : target);
}

//! Read every axis range + value and the held keys: at open and after SYN_DROPPED.
static void
resync(void)
{
	const enum gamepad_evdev_layout layout = g.pad.layout;
	gamepad_evdev_pad_init(&g.pad, layout);

	unsigned long abs[NLONGS(ABS_MAX + 1)] = {0};
	if (ioctl(g.fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs) >= 0) {
		for (unsigned code = 0; code < GAMEPAD_EVDEV_ABS_COUNT; code++) {
			struct input_absinfo ai;
			if (test_bit(abs, code) && ioctl(g.fd, EVIOCGABS(code), &ai) >= 0) {
				gamepad_evdev_set_abs(&g.pad, (uint16_t)code, ai.minimum, ai.maximum, ai.value);
			}
		}
	}
	unsigned long keys[NLONGS(KEY_MAX + 1)] = {0};
	if (ioctl(g.fd, EVIOCGKEY(sizeof(keys)), keys) >= 0) {
		for (unsigned code = BTN_SOUTH; code <= BTN_DPAD_RIGHT; code++) {
			if (test_bit(keys, code)) {
				gamepad_evdev_apply(&g.pad, GP_EV_KEY, (uint16_t)code, 1);
			}
		}
	}
}

static void
close_pad(const char *why)
{
	if (g.fd < 0) {
		return;
	}
	if (g.ff_id >= 0) {
		ioctl(g.fd, EVIOCRMFF, (long)g.ff_id);
	}
	close(g.fd);
	U_LOG_W("gamepad: evdev %s (%04X:%04X '%s') %s.", g.path, g.vendor, g.product, g.name, why);
	g.fd = -1;
	g.ff_id = -1;
	g.rank = 0;
	g.can_rumble = false;
	snprintf(g.label, sizeof(g.label), "evdev (no pad)");
}

//! Open the best pad, if it beats the one held. Throttled by the caller.
static void
scan(void)
{
	DIR *d = opendir(EVDEV_DIR);
	if (d == NULL) {
		return;
	}
	char best[64] = "";
	char best_event[32] = "";
	int best_rank = g.rank;
	int best_num = -1;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		int num = -1;
		if (sscanf(e->d_name, "event%d", &num) != 1) {
			continue;
		}
		char path[64];
		snprintf(path, sizeof(path), EVDEV_DIR "/%s", e->d_name);
		if (strcmp(path, g.path) == 0 && g.fd >= 0) {
			continue; // the one held
		}
		const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) {
			continue; // not ours to read (no uaccess): not a pad for this user
		}
		struct input_id id = {0};
		const bool pad = is_pad(fd, &id);
		close(fd);
		if (!pad) {
			continue;
		}
		const int rank = gamepad_evdev_rank(id.vendor, id.product);
		if (rank > best_rank || (rank == best_rank && best[0] != '\0' && num < best_num)) {
			best_rank = rank;
			best_num = num;
			snprintf(best, sizeof(best), "%s", path);
			snprintf(best_event, sizeof(best_event), "%s", e->d_name);
		}
	}
	closedir(d);
	if (best[0] == '\0') {
		return;
	}

	// Read-write for rumble; read-only is enough for input.
	bool rw = true;
	int fd = open(best, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		rw = false;
		fd = open(best, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	}
	struct input_id id = {0};
	if (fd < 0 || !is_pad(fd, &id)) {
		if (fd >= 0) {
			close(fd);
		}
		return;
	}
	close_pad(g.rank > 0 ? "replaced by a preferred pad" : "closed");

	g.fd = fd;
	g.rank = best_rank;
	g.vendor = id.vendor;
	g.product = id.product;
	snprintf(g.path, sizeof(g.path), "%s", best);
	if (ioctl(fd, EVIOCGNAME(sizeof(g.name)), g.name) < 0) {
		snprintf(g.name, sizeof(g.name), "?");
	}
	char driver[64];
	driver_of(best_event, driver, sizeof(driver));
	gamepad_evdev_pad_init(&g.pad, gamepad_evdev_layout_for(id.vendor, driver));

	unsigned long ff[NLONGS(FF_MAX + 1)] = {0};
	g.can_rumble = rw && ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ff)), ff) >= 0 && test_bit(ff, FF_RUMBLE);
	g.ff_id = -1;
	resync();

	snprintf(g.label, sizeof(g.label), "evdev:%s", g.name);
	U_LOG_W("gamepad: evdev %s opened: %04X:%04X '%s' (driver %s, %s layout, rank %d%s%s).", g.path, id.vendor,
	        id.product, g.name, driver[0] != '\0' ? driver : "?",
	        g.pad.layout == GAMEPAD_EVDEV_LAYOUT_XPAD ? "xpad" : "spec", g.rank,
	        g.rank == 3 ? ": Steam Input virtual pad" : "", g.can_rumble ? ", rumble" : "");
}

bool
gamepad_backend_available(void)
{
	return true;
}

bool
gamepad_backend_init(void)
{
	g.fd = -1;
	g.ff_id = -1;
	g.rank = 0;
	g.path[0] = '\0';
	g.next_scan_ns = 0;
	snprintf(g.label, sizeof(g.label), "evdev (no pad)");
	return true;
}

void
gamepad_backend_fini(void)
{
	close_pad("closed");
}

static void
disconnected(struct gamepad_state *out)
{
	out->connected = false;
	out->buttons = 0;
	out->left_x = out->left_y = out->right_x = out->right_y = 0.0f;
	out->left_trigger = out->right_trigger = 0.0f;
}

void
gamepad_backend_poll(struct gamepad_state *out)
{
	const int64_t now = os_monotonic_get_ns();
	if (g.fd < 0 || g.rank == 1) {
		if (now >= g.next_scan_ns) {
			g.next_scan_ns = now + (g.fd < 0 ? RESCAN_IDLE_NS : RESCAN_UPGRADE_NS);
			scan();
		}
	}
	if (g.fd < 0) {
		disconnected(out);
		return;
	}

	struct input_event ev[64];
	for (;;) {
		const ssize_t n = read(g.fd, ev, sizeof(ev));
		if (n < 0) {
			if (errno == EAGAIN || errno == EINTR) {
				break;
			}
			close_pad(errno == ENODEV ? "unplugged" : "failed to read");
			g.next_scan_ns = 0;
			disconnected(out);
			return;
		}
		const size_t count = (size_t)n / sizeof(ev[0]);
		for (size_t i = 0; i < count; i++) {
			if (ev[i].type == EV_SYN && ev[i].code == SYN_DROPPED) {
				resync(); // the kernel's queue overflowed: re-read the whole state
				continue;
			}
			gamepad_evdev_apply(&g.pad, ev[i].type, ev[i].code, ev[i].value);
		}
		if (count < sizeof(ev) / sizeof(ev[0])) {
			break;
		}
	}
	gamepad_evdev_fill(&g.pad, out);
}

void
gamepad_backend_rumble(float low_frequency, float high_frequency)
{
	if (g.fd < 0 || !g.can_rumble) {
		return;
	}
	const float lo = low_frequency < 0.0f ? 0.0f : (low_frequency > 1.0f ? 1.0f : low_frequency);
	const float hi = high_frequency < 0.0f ? 0.0f : (high_frequency > 1.0f ? 1.0f : high_frequency);

	struct input_event play = {0};
	play.type = EV_FF;
	if (lo == 0.0f && hi == 0.0f) {
		if (g.ff_id >= 0) {
			play.code = (uint16_t)g.ff_id;
			play.value = 0; // stop
			(void)!write(g.fd, &play, sizeof(play));
		}
		return;
	}

	// Level-set: (re)upload the effect with the new magnitudes and play it. The
	// hub expires rumble itself, so the effect runs until the next call.
	struct ff_effect fx = {0};
	fx.type = FF_RUMBLE;
	fx.id = g.ff_id; // -1 uploads a new effect; an id updates it in place
	fx.u.rumble.strong_magnitude = (uint16_t)(lo * 65535.0f);
	fx.u.rumble.weak_magnitude = (uint16_t)(hi * 65535.0f);
	fx.replay.length = 0xFFFF;
	if (ioctl(g.fd, EVIOCSFF, &fx) < 0) {
		g.can_rumble = false; // the driver refused: behave like a pad with no motors
		return;
	}
	g.ff_id = fx.id;
	play.code = (uint16_t)g.ff_id;
	play.value = 1;
	(void)!write(g.fd, &play, sizeof(play));
}

const char *
gamepad_backend_name(void)
{
	return g.label[0] != '\0' ? g.label : "evdev";
}

#else
// Keep the translation unit non-empty where this backend is compiled out (-Wempty-translation-unit).
typedef int gamepad_backend_evdev_not_built_here_t;
#endif // XRT_OS_LINUX_DESKTOP
