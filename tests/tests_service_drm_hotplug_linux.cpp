// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The service's DRM hotplug feeder (ADR-051 D6): the kernel-uevent
 *         parser against `NETLINK_KOBJECT_UEVENT` payloads in the kernel's
 *         wire format, and the
 *         polling fallback's connector-status hash. Pure; no socket, no sysfs.
 */

#include "catch_amalgamated.hpp"

#include "service_drm_hotplug_linux.h"

#include <string>

namespace {

//! A uevent datagram from its NUL-separated parts (the wire format).
std::string
uevent(std::initializer_list<const char *> parts)
{
	std::string s;
	for (const char *p : parts) {
		s.append(p);
		s.push_back('\0');
	}
	return s;
}

bool
is_hotplug(const std::string &s)
{
	return service_drm_uevent_is_hotplug(s.data(), s.size());
}

} // namespace

TEST_CASE("DRM connector change is a hotplug", "[service][drm][hotplug]")
{
	// The shape drm_sysfs_connector_hotplug_event() sends (drivers/gpu/drm/
	// drm_sysfs.c): KOBJ_CHANGE on the card minor with HOTPLUG=1 and the
	// connector id, as `udevadm monitor --kernel --property` prints it.
	CHECK(is_hotplug(
	    uevent({"change@/devices/pci0000:00/0000:00:02.0/drm/card1", "ACTION=change",
	            "DEVPATH=/devices/pci0000:00/0000:00:02.0/drm/card1", "SUBSYSTEM=drm", "HOTPLUG=1", "CONNECTOR=107",
	            "DEVNAME=dri/card1", "DEVTYPE=drm_minor", "SEQNUM=5521", "MAJOR=226", "MINOR=1"})));
	// drm_sysfs_hotplug_event() (the whole-device variant) has no CONNECTOR.
	CHECK(is_hotplug(
	    uevent({"change@/devices/pci0000:00/0000:00:08.1/0000:c4:00.0/drm/card0", "ACTION=change",
	            "DEVPATH=/devices/pci0000:00/0000:00:08.1/0000:c4:00.0/drm/card0", "SUBSYSTEM=drm", "HOTPLUG=1",
	            "DEVNAME=dri/card0", "DEVTYPE=drm_minor", "SEQNUM=9001", "MAJOR=226", "MINOR=0"})));
	// No trailing NUL on the last pair (the length bounds the parse).
	std::string tail = uevent({"change@/x", "SUBSYSTEM=drm", "ACTION=change"});
	tail.pop_back();
	CHECK(is_hotplug(tail));
}

TEST_CASE("Other uevents are not a hotplug", "[service][drm][hotplug]")
{
	// A connector being added at driver load (add, not change).
	CHECK_FALSE(is_hotplug(uevent({"add@/devices/pci0000:00/0000:00:02.0/drm/card1/card1-HDMI-A-1", "ACTION=add",
	                               "DEVPATH=/devices/pci0000:00/0000:00:02.0/drm/card1/card1-HDMI-A-1",
	                               "SUBSYSTEM=drm", "SEQNUM=4410"})));
	// A change on another subsystem (a USB hub, a backlight).
	CHECK_FALSE(is_hotplug(
	    uevent({"change@/devices/platform/x/backlight/intel_backlight", "ACTION=change",
	            "DEVPATH=/devices/platform/x/backlight/intel_backlight", "SUBSYSTEM=backlight", "SEQNUM=77"})));
	// A prefix is not a match: "SUBSYSTEM=drm_dp_aux_dev".
	CHECK_FALSE(is_hotplug(uevent({"change@/x", "ACTION=change", "SUBSYSTEM=drm_dp_aux_dev"})));
	CHECK_FALSE(is_hotplug(uevent({"change@/x", "ACTION=changed", "SUBSYSTEM=drm"})));
	// The header alone matches nothing.
	CHECK_FALSE(is_hotplug(uevent({"change@/devices/drm/card1"})));
	// udev's re-broadcast (binary "libudev" header) is not a kernel uevent.
	std::string udev("libudev\0\xfe\xed\xca\xfe", 12);
	udev += uevent({"ACTION=change", "SUBSYSTEM=drm"});
	CHECK_FALSE(is_hotplug(udev));
	// Empty / NULL.
	CHECK_FALSE(service_drm_uevent_is_hotplug(nullptr, 10));
	CHECK_FALSE(service_drm_uevent_is_hotplug("", 0));
}

TEST_CASE("Polling fallback: the status hash moves with any connector", "[service][drm][hotplug]")
{
	const char *names[] = {"card1-DP-1", "card1-HDMI-A-1", "card1-eDP-1"};
	const char *before[] = {"disconnected\n", "disconnected\n", "connected\n"};
	const char *after[] = {"disconnected\n", "connected\n", "connected\n"};
	const uint64_t a = service_drm_status_hash(names, before, 3);
	CHECK(a == service_drm_status_hash(names, before, 3));
	CHECK(a != service_drm_status_hash(names, after, 3));
	// A connector appearing (an MST hub) moves it too.
	CHECK(a != service_drm_status_hash(names, before, 2));
	// Field boundaries count: ("ab","c") != ("a","bc").
	const char *n1[] = {"ab"};
	const char *s1[] = {"c"};
	const char *n2[] = {"a"};
	const char *s2[] = {"bc"};
	CHECK(service_drm_status_hash(n1, s1, 1) != service_drm_status_hash(n2, s2, 1));
}
