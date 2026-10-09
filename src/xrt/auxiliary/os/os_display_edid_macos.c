// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS EDID enumeration: CoreGraphics displays joined to the EDID
 *         blobs IOKit publishes (multi-screen on macOS).
 * @ingroup aux_os
 *
 * ## Records
 *
 * One record per active CoreGraphics display (mirrors folded), main display
 * first, from @ref os_display_macos_fill_desktop_info:
 *
 * - `screen_left/top`, `pixel_width/height`: `CGDisplayBounds`, top-down
 *   global display space, in POINTS — the space windows are placed in and the
 *   space `os_display_desktop_*` and `XR_DXR_display_info` already publish on
 *   macOS, so a registry entry's rect contains the resolved panel origin the
 *   same way it does on Windows and X11.
 * - `native_width/height`: the current mode's BACKING pixels — the space a
 *   plug-in reports panel dims in (`xrt_display_physical::native_pixel_width`).
 * - `output_name`: the display UUID, i.e. `os_display_desktop_info::device_name`,
 *   and the platform key of the monitor id, so the id survives reboots and
 *   rearrangement (a `CGDirectDisplayID` survives neither).
 *
 * ## Identity
 *
 * CoreGraphics already knows each display's EDID vendor, product and serial
 * (`CGDisplayVendorNumber` / `ModelNumber` / `SerialNumber`), so identity never
 * depends on IOKit. IOKit adds what CG does not expose — the EDID's own mm and
 * monitor name — when a blob matching those numbers can be found. Where the
 * blob lives depends on the machine:
 *
 * - Apple Silicon: an `EDID` property on the DCP transport service
 *   (`IOPortTransportStateDisplayPort` for DP / USB-C / Thunderbolt), and
 *   `DisplayAttributes.ProductAttributes.ProductName` on the `AppleCLCD2`
 *   framebuffer. The built-in panel has neither an EDID nor a matching
 *   ProductID there.
 * - Intel: an `IODisplayEDID` property on the `IODisplay` service.
 *
 * Both are found with a property-exists match, not by class name, so a new
 * transport class carrying `EDID` is picked up unchanged.
 *
 * No logging (aux_os sits below u_logging); the runtime logs the records
 * where it turns them into descriptors.
 */

#include "os_display_edid.h"
#include "os_display_edid_parse.h"
#include "os_display_macos.h"

#include <stdio.h>
#include <string.h>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/IOKitLib.h>

//! EDID blobs kept from IOKit.
#define MACOS_EDID_MAX_BLOBS 32

//! `DisplayAttributes` product names kept from IOKit.
#define MACOS_MAX_PRODUCT_NAMES 16


/*
 *
 * Pure match.
 *
 */

int32_t
os_display_macos_match_edid(uint32_t cg_vendor,
                            uint32_t cg_model,
                            uint32_t cg_serial,
                            const struct os_display_edid_parsed *cands,
                            const bool *used,
                            uint32_t cand_count)
{
	if (cands == NULL) {
		return -1;
	}
	const uint16_t mfr = os_display_macos_cg_vendor_to_edid_raw(cg_vendor);
	const uint16_t product = (uint16_t)(cg_model & 0xFFFFu);

	// Pass 0: the serial agrees exactly (both sides have one).
	// Pass 1: either side lacks a serial — manufacturer + product decide.
	for (uint32_t pass = 0; pass < 2; pass++) {
		for (uint32_t i = 0; i < cand_count; i++) {
			const struct os_display_edid_parsed *c = &cands[i];
			if ((used != NULL && used[i]) || c->manufacturer_id != mfr || c->product_id != product) {
				continue;
			}
			const bool both_serial = cg_serial != 0 && c->serial_number != 0;
			if (pass == 0 && both_serial && c->serial_number == cg_serial) {
				return (int32_t)i;
			}
			if (pass == 1 && !both_serial) {
				return (int32_t)i;
			}
		}
	}
	return -1;
}


/*
 *
 * IOKit.
 *
 */

static bool
cf_number_u64(CFTypeRef v, uint64_t *out)
{
	if (v == NULL || CFGetTypeID(v) != CFNumberGetTypeID()) {
		return false;
	}
	int64_t x = 0;
	if (!CFNumberGetValue((CFNumberRef)v, kCFNumberSInt64Type, &x)) {
		return false;
	}
	*out = (uint64_t)x;
	return true;
}

//! Iterator over every IOService carrying property @p key (caller releases).
static io_iterator_t
services_with_property(const char *key)
{
	CFMutableDictionaryRef match =
	    CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (match == NULL) {
		return 0;
	}
	CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
	if (k == NULL) {
		CFRelease(match);
		return 0;
	}
	CFDictionarySetValue(match, CFSTR("IOPropertyExistsMatch"), k);
	CFRelease(k);

	io_iterator_t it = 0;
	// Consumes one reference to the matching dictionary. MACH_PORT_NULL is the
	// default main port, spelled so it builds on every SDK (kIOMainPortDefault
	// is macOS 12+, kIOMasterPortDefault deprecated since).
	if (IOServiceGetMatchingServices(MACH_PORT_NULL, match, &it) != KERN_SUCCESS) {
		return 0;
	}
	return it;
}

//! Append every parseable, not-yet-seen EDID found under property @p key.
static void
collect_edids(const char *key, struct os_display_edid_parsed *out, uint32_t *count, uint32_t max)
{
	io_iterator_t it = services_with_property(key);
	if (it == 0) {
		return;
	}
	CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
	io_object_t svc;
	while ((svc = IOIteratorNext(it)) != 0) {
		CFTypeRef v = k != NULL ? IORegistryEntryCreateCFProperty(svc, k, kCFAllocatorDefault, 0) : NULL;
		if (v != NULL && CFGetTypeID(v) == CFDataGetTypeID() && *count < max) {
			struct os_display_edid_parsed p;
			if (os_display_edid_parse(CFDataGetBytePtr((CFDataRef)v), (size_t)CFDataGetLength((CFDataRef)v),
			                          &p)) {
				bool dup = false;
				for (uint32_t i = 0; i < *count && !dup; i++) {
					dup = memcmp(&out[i], &p, sizeof(p)) == 0;
				}
				if (!dup) {
					out[(*count)++] = p;
				}
			}
		}
		if (v != NULL) {
			CFRelease(v);
		}
		IOObjectRelease(svc);
	}
	if (k != NULL) {
		CFRelease(k);
	}
	IOObjectRelease(it);
}

struct product_name
{
	uint32_t vendor; //!< LegacyManufacturerID — CoreGraphics' vendor number.
	uint32_t model;  //!< ProductID.
	uint32_t serial; //!< SerialNumber; 0 = absent.
	char name[64];
};

//! Apple Silicon: `DisplayAttributes.ProductAttributes` on each framebuffer.
static uint32_t
collect_product_names(struct product_name *out, uint32_t max)
{
	io_iterator_t it = services_with_property("DisplayAttributes");
	if (it == 0) {
		return 0;
	}
	uint32_t n = 0;
	io_object_t svc;
	while ((svc = IOIteratorNext(it)) != 0) {
		CFTypeRef attrs =
		    IORegistryEntryCreateCFProperty(svc, CFSTR("DisplayAttributes"), kCFAllocatorDefault, 0);
		if (attrs != NULL && CFGetTypeID(attrs) == CFDictionaryGetTypeID() && n < max) {
			CFTypeRef pa = CFDictionaryGetValue((CFDictionaryRef)attrs, CFSTR("ProductAttributes"));
			if (pa != NULL && CFGetTypeID(pa) == CFDictionaryGetTypeID()) {
				CFDictionaryRef d = (CFDictionaryRef)pa;
				uint64_t vendor = 0, model = 0, serial = 0;
				CFTypeRef name = CFDictionaryGetValue(d, CFSTR("ProductName"));
				if (cf_number_u64(CFDictionaryGetValue(d, CFSTR("LegacyManufacturerID")), &vendor) &&
				    cf_number_u64(CFDictionaryGetValue(d, CFSTR("ProductID")), &model) &&
				    name != NULL && CFGetTypeID(name) == CFStringGetTypeID()) {
					(void)cf_number_u64(CFDictionaryGetValue(d, CFSTR("SerialNumber")), &serial);
					struct product_name *pn = &out[n];
					memset(pn, 0, sizeof(*pn));
					pn->vendor = (uint32_t)vendor;
					pn->model = (uint32_t)model;
					pn->serial = (uint32_t)serial;
					if (CFStringGetCString((CFStringRef)name, pn->name, (CFIndex)sizeof(pn->name),
					                       kCFStringEncodingUTF8) &&
					    pn->name[0] != '\0') {
						n++;
					}
				}
			}
		}
		if (attrs != NULL) {
			CFRelease(attrs);
		}
		IOObjectRelease(svc);
	}
	IOObjectRelease(it);
	return n;
}


/*
 *
 * Public entry points.
 *
 */

bool
os_display_edid_enumerate(struct os_display_edid_list *out_list)
{
	if (out_list == NULL) {
		return false;
	}
	memset(out_list, 0, sizeof(*out_list));

	uint32_t ids[OS_DISPLAY_EDID_MAX_MONITORS];
	const uint32_t id_count = os_display_macos_list_displays(ids, OS_DISPLAY_EDID_MAX_MONITORS);
	// diag_gdi_count is "monitors the placement source saw" — CoreGraphics here.
	out_list->diag_gdi_count = id_count;
	if (id_count == 0) {
		out_list->diag_error = OS_EDID_DIAG_NO_GDI_MONITORS;
		return false;
	}

	struct os_display_edid_parsed edids[MACOS_EDID_MAX_BLOBS];
	uint32_t edid_count = 0;
	collect_edids("EDID", edids, &edid_count, MACOS_EDID_MAX_BLOBS);          // Apple Silicon (DCP)
	collect_edids("IODisplayEDID", edids, &edid_count, MACOS_EDID_MAX_BLOBS); // Intel (IODisplay)
	out_list->diag_edid_read_count = edid_count;
	bool used[MACOS_EDID_MAX_BLOBS] = {0};

	struct product_name names[MACOS_MAX_PRODUCT_NAMES];
	const uint32_t name_count = collect_product_names(names, MACOS_MAX_PRODUCT_NAMES);

	uint32_t n = 0;
	for (uint32_t i = 0; i < id_count && n < OS_DISPLAY_EDID_MAX_MONITORS; i++) {
		const CGDirectDisplayID did = (CGDirectDisplayID)ids[i];
		struct os_display_desktop_info desk;
		if (!os_display_macos_fill_desktop_info(ids[i], &desk)) {
			continue;
		}

		struct os_display_edid_monitor *m = &out_list->monitors[n++];
		memset(m, 0, sizeof(*m));
		m->screen_left = desk.left;
		m->screen_top = desk.top;
		m->pixel_width = desk.width;
		m->pixel_height = desk.height;
		m->native_width = desk.native_width;
		m->native_height = desk.native_height;
		m->refresh_hz = (desk.native_refresh_mhz + 500u) / 1000u;
		m->is_primary = desk.is_primary;
		m->physical_width_mm = desk.physical_width_mm;
		m->physical_height_mm = desk.physical_height_mm;
		(void)snprintf(m->output_name, sizeof(m->output_name), "%s", desk.device_name);

		const uint32_t vendor = CGDisplayVendorNumber(did);
		const uint32_t model = CGDisplayModelNumber(did);
		const uint32_t serial = CGDisplaySerialNumber(did);
		m->manufacturer_id = os_display_macos_cg_vendor_to_edid_raw(vendor);
		m->product_id = (uint16_t)(model & 0xFFFFu);
		m->serial_number = serial;
		m->join = OS_EDID_JOIN_CG_IDS;

		const int32_t e = os_display_macos_match_edid(vendor, model, serial, edids, used, edid_count);
		if (e >= 0) {
			used[e] = true;
			m->join = OS_EDID_JOIN_IOKIT_EDID;
			if (edids[e].serial_number != 0) {
				m->serial_number = edids[e].serial_number;
			}
			// The EDID's own mm beat CoreGraphics' (which is derived from
			// the same EDID, but through the cm fields on some panels).
			uint32_t w_mm = 0, h_mm = 0;
			os_display_edid_parsed_mm(&edids[e], &w_mm, &h_mm);
			if (w_mm > 0 && h_mm > 0) {
				m->physical_width_mm = w_mm;
				m->physical_height_mm = h_mm;
			}
			(void)snprintf(m->display_name, sizeof(m->display_name), "%s", edids[e].monitor_name);
		}

		// No EDID name: the framebuffer's ProductName (Apple Silicon).
		for (uint32_t k = 0; k < name_count && m->display_name[0] == '\0'; k++) {
			if (names[k].vendor == vendor && names[k].model == model &&
			    (names[k].serial == 0 || serial == 0 || names[k].serial == serial)) {
				(void)snprintf(m->display_name, sizeof(m->display_name), "%s", names[k].name);
			}
		}
		if (m->display_name[0] == '\0' && CGDisplayIsBuiltin(did)) {
			(void)snprintf(m->display_name, sizeof(m->display_name), "Built-in Display");
		}
	}

	out_list->count = n;
	if (n > 0 && edid_count == 0) {
		out_list->diag_error = OS_EDID_DIAG_NO_EDID_DATA;
	}
	return n > 0;
}

const struct os_display_edid_monitor *
os_display_edid_find_in_table(const struct os_display_edid_list *list, const uint16_t table[][2], uint32_t table_len)
{
	if (list == NULL || table == NULL || table_len == 0) {
		return NULL;
	}
	for (uint32_t m = 0; m < list->count; m++) {
		for (uint32_t t = 0; t < table_len; t++) {
			if (list->monitors[m].manufacturer_id == table[t][0] &&
			    list->monitors[m].product_id == table[t][1]) {
				return &list->monitors[m];
			}
		}
	}
	return NULL;
}
