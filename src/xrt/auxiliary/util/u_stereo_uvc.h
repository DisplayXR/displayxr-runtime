// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  XR_DXR_stereo_camera (ADR-043 Amendment 4): a vendor-neutral stereo
 *         camera source for plain UVC side-by-side stereo webcams.
 *
 * Every other stereo camera comes from a display plug-in (the tracker camera
 * the plug-in reads without taking it from the eye tracker). This one is the
 * SERVICE's own: an external stereo webcam that delivers ONE side-by-side
 * image per frame over plain UVC, opted in by the user, works with any display
 * and needs no vendor plug-in. Nothing here is vendor-specific.
 *
 * Layers, so everything but the OS capture call is testable on every OS:
 *
 *  1. **Config** (@ref u_stereo_uvc_config_parse / _load): which devices are
 *     claimed and how to read them. OPT-IN only — with no config file the
 *     source enumerates nothing and never touches a camera. A JSON file in
 *     the per-user DisplayXR config dir (the u_setting.h directory):
 *       Windows  %LOCALAPPDATA%\DisplayXR\stereo-cameras.json
 *       POSIX    $XDG_CONFIG_HOME/displayxr/stereo-cameras.json
 *                (else ~/.config/displayxr/stereo-cameras.json)
 *     or the file named by DXR_STEREO_CAMERA_UVC_CONFIG in the SERVICE's
 *     environment; DXR_STEREO_CAMERA_UVC=0 turns the source off.
 *     @code
 *     {"uvc": [{
 *        "match": "eyes",           // friendly-name substring, case-insensitive, and/or
 *        "vid_pid": "1234:abcd",    // USB VID:PID (hex); both given = both must match
 *        "name": "Stereo webcam",   // display name ("" = the device's own)
 *        "layout": "sbs-half",      // "sbs-full": each half is a full-res eye;
 *                                   // "sbs-half": each half is an eye squeezed 2:1 horizontally
 *        "eyes": "lr",              // "lr" (left lens in the left half) | "rl"
 *        "mode": "3840x2160@60",    // capture mode (optional: else the largest, fastest)
 *        "eye_size": "1280x720",    // per-eye output (optional: else the information-honest size,
 *                                   // capped at 1280 px wide)
 *        "baseline_mm": 60,         // nominal lens separation (optional)
 *        "hfov_deg": 90,            // nominal per-eye horizontal field of view (optional)
 *        "calibration": "C:/cal/eyes.yaml"  // OpenCV-style K1 D1 K2 D2 R T (optional)
 *     }]}
 *     @endcode
 *     A built-in device allowlist is deliberately empty: none is shipped
 *     without a public spec for the device's SBS layout.
 *  2. **Matching** (@ref u_stereo_uvc_entry_matches): name substring and/or
 *     USB VID:PID. A device a display plug-in already reports as its camera
 *     (its platform_device_hint) is never claimed — the tracker camera belongs
 *     to the plug-in and its eye tracker (@ref u_stereo_uvc_device_claimed_by_hint).
 *  3. **Geometry**: the SBS split (@ref u_stereo_uvc_split: either eye order,
 *     full or half SBS, resampled to square-pixel eyes, NV12 out) and, for an
 *     uncalibrated device, a NOMINAL pinhole (@ref
 *     u_stereo_uvc_nominal_calibration: from HFOV + eye size + baseline, no
 *     distortion, a parallel pair). The service reports such a camera WITHOUT
 *     CALIBRATED; its RECTIFIED output is the frames as delivered with the
 *     service's online vertical refinement doing the row alignment.
 *     A calibration file (@ref u_stereo_uvc_calibration_parse) makes it
 *     CALIBRATED and the service's ordinary rectifier runs.
 *  4. **The source** (@ref u_stereo_uvc_create ... _close): the same six
 *     operations as the plug-in camera slots (enumerate, calibration, open,
 *     wait_frame, release_frame, close), over a capture BACKEND
 *     (@ref u_stereo_uvc_backend). The backend is the only OS-specific part:
 *     Media Foundation on Windows (os_uvc_capture_mf.cpp), V4L2 on Linux (a
 *     stub for now), and @ref u_stereo_uvc_fake_backend_init — a synthetic SBS
 *     camera with a known disparity and vertical misalignment, for tests and
 *     hardware-free development (`"fake": {...}` in the config).
 *
 * The device is opened only while the service holds the source open, i.e.
 * while a stream is started (plus the manager's 2 s linger).
 *
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_compiler.h"
#include "xrt/xrt_plugin.h"
#include "xrt/xrt_results.h"

#include "util/u_stereo_camera.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//! Config entries (= cameras) at most.
#define U_STEREO_UVC_MAX_ENTRIES 4
//! OS capture devices considered at most.
#define U_STEREO_UVC_MAX_DEVICES 16
//! Modes a backend reports per device at most.
#define U_STEREO_UVC_MAX_MODES 64
//! The default per-eye output width cap (a 3D call does not need more).
#define U_STEREO_UVC_DEFAULT_MAX_EYE_WIDTH 1280u
//! Used for the nominal model when the config gives none (reported as 0 = unknown).
#define U_STEREO_UVC_DEFAULT_HFOV_DEG 70.0
#define U_STEREO_UVC_DEFAULT_BASELINE_MM 60.0
//! File name in the per-user DisplayXR config dir.
#define U_STEREO_UVC_CONFIG_FILENAME "stereo-cameras.json"


/*
 *
 * Config.
 *
 */

enum u_stereo_uvc_layout
{
	//! Each half of the frame is one eye at full horizontal resolution (2 sensors side by side).
	U_STEREO_UVC_LAYOUT_SBS_FULL = 1,
	//! Each half is one eye squeezed 2:1 horizontally (a W x H frame carries two W x H views).
	U_STEREO_UVC_LAYOUT_SBS_HALF = 2,
};

struct u_stereo_uvc_entry
{
	char name_contains[128]; //!< "" = not matched by name
	bool match_vid_pid;
	uint16_t vid, pid;
	char display_name[128]; //!< "" = the device's friendly name
	uint32_t layout;        //!< enum u_stereo_uvc_layout
	//! "eyes": "rl" — the device puts its RIGHT lens in the LEFT half.
	bool swap_eyes;
	uint32_t mode_width, mode_height; //!< 0 = auto
	float mode_fps;                   //!< 0 = auto
	uint32_t eye_width, eye_height;   //!< 0 = auto
	float baseline_mm;                //!< 0 = not configured
	float hfov_deg;                   //!< 0 = not configured
	char calibration[512];            //!< "" = none (nominal model)
};

//! The synthetic camera's scene (fake backend).
struct u_stereo_uvc_fake_params
{
	//! Horizontal disparity of the scene, left x - right x, fraction of the eye width.
	double disparity;
	//! Vertical misalignment: features appear this much LOWER in the right eye, fraction of the eye height.
	double dy;
	//! Raw pixel format the fake delivers: 1 = NV12, 2 = YUY2 (enum u_stereo_uvc_pixel).
	uint32_t pixel;
	float fps;
};

struct u_stereo_uvc_config
{
	uint32_t count;
	struct u_stereo_uvc_entry entries[U_STEREO_UVC_MAX_ENTRIES];
	//! "fake": a synthetic device the entries can match ("DisplayXR synthetic SBS camera").
	bool fake;
	struct u_stereo_uvc_fake_params fake_params;
};

//! One entry with every default filled (sbs-full, lr, auto everything).
void
u_stereo_uvc_entry_defaults(struct u_stereo_uvc_entry *e);

/*!
 * Parse a config document (see the file comment). Unknown keys are ignored;
 * an entry with neither "match" nor "vid_pid" or a bad value fails the whole
 * document (a typo must not silently claim a different camera).
 * @return false + a message in @p err on an invalid document.
 */
bool
u_stereo_uvc_config_parse(const char *json, struct u_stereo_uvc_config *out, char *err, size_t err_cap);

//! The config file path: DXR_STEREO_CAMERA_UVC_CONFIG, else the per-user default.
bool
u_stereo_uvc_config_path(char *out, size_t cap);

enum u_stereo_uvc_config_load_result
{
	U_STEREO_UVC_CONFIG_NONE = 0,    //!< no file (or DXR_STEREO_CAMERA_UVC=0): the source is off
	U_STEREO_UVC_CONFIG_OK = 1,      //!< parsed
	U_STEREO_UVC_CONFIG_INVALID = 2, //!< present but unusable (@p err says why): the source is off
};

//! Read and parse the config file. @p path gets the path consulted.
enum u_stereo_uvc_config_load_result
u_stereo_uvc_config_load(struct u_stereo_uvc_config *out, char *path, size_t path_cap, char *err, size_t err_cap);


/*
 *
 * Devices + matching.
 *
 */

struct u_stereo_uvc_device
{
	char name[128]; //!< friendly name, UTF-8
	//! The OS device id: Windows the Media Foundation symbolic link
	//! (\\?\usb#vid_xxxx&pid_yyyy&mi_00#...), Linux /dev/videoN.
	char id[256];
	bool has_vid_pid;
	uint16_t vid, pid;
};

/*!
 * Find a USB VID:PID in @p s: "vid_XXXX&pid_YYYY" anywhere (any case, as in
 * Windows device ids and symbolic links) or a plain "XXXX:YYYY".
 */
bool
u_stereo_uvc_parse_vid_pid(const char *s, uint16_t *out_vid, uint16_t *out_pid);

//! Does config entry @p e claim device @p d?
bool
u_stereo_uvc_entry_matches(const struct u_stereo_uvc_entry *e, const struct u_stereo_uvc_device *d);

//! @ref u_stereo_uvc_assign: the entry matched no available device.
#define U_STEREO_UVC_ASSIGN_NONE (-1)
//! @ref u_stereo_uvc_assign: a NAME-ONLY entry matched several devices — claims none.
#define U_STEREO_UVC_ASSIGN_AMBIGUOUS (-2)

/*!
 * Which device each config entry claims, in entry order: the first matching
 * device not @p unavailable (a plug-in's camera) and not claimed by an earlier
 * entry. A name-substring entry WITHOUT vid_pid that matches more than one
 * such device claims nothing (@ref U_STEREO_UVC_ASSIGN_AMBIGUOUS): a loose
 * name ("3D", a vendor prefix) must never pick, say, a display's own tracking
 * camera over the intended webcam by enumeration order. An entry with a
 * VID:PID takes the first of several identical devices.
 * @param unavailable may be NULL.
 */
void
u_stereo_uvc_assign(const struct u_stereo_uvc_config *cfg,
                    const struct u_stereo_uvc_device *devs,
                    uint32_t device_count,
                    const bool *unavailable,
                    int32_t out_device_for_entry[U_STEREO_UVC_MAX_ENTRIES]);

/*!
 * Is @p d the physical device behind a plug-in camera whose
 * platform_device_hint is @p hint? Same OS id (case-insensitive), or the same
 * USB VID:PID when the hint carries one. Such a device is never claimed.
 */
bool
u_stereo_uvc_device_claimed_by_hint(const struct u_stereo_uvc_device *d, const char *hint);


/*
 *
 * Geometry.
 *
 */

/*!
 * The per-eye output size for a @p frame_w x @p frame_h capture. Requested
 * @p want_w x @p want_h when non-zero (rounded down to even), else the
 * information-honest square-pixel size — SBS_FULL: (W/2, H); SBS_HALF:
 * (W/2, H/2) — scaled down to at most @ref U_STEREO_UVC_DEFAULT_MAX_EYE_WIDTH
 * wide, aspect kept, even extents.
 * @return false on a frame that cannot be split (odd/zero width, too small).
 */
bool
u_stereo_uvc_eye_size(uint32_t layout,
                      uint32_t frame_w,
                      uint32_t frame_h,
                      uint32_t want_w,
                      uint32_t want_h,
                      uint32_t *out_w,
                      uint32_t *out_h);

/*!
 * The nominal RAW calibration of an uncalibrated pair: per eye a pinhole with
 * fx = fy = (eye_w / 2) / tan(hfov / 2), principal point at the image centre
 * ((w - 1) / 2, (h - 1) / 2, OpenCV pixel centres), no distortion; the right
 * camera a pure +x translation (R = I, T = (-baseline, 0, 0) mm).
 */
void
u_stereo_uvc_nominal_calibration(uint32_t eye_w,
                                 uint32_t eye_h,
                                 double hfov_deg,
                                 double baseline_mm,
                                 struct xrt_plugin_stereo_camera_calibration *out);

/*!
 * Parse an OpenCV-style stereo calibration — FileStorage YAML (`K1: !!opencv-matrix
 * ... data: [...]`) or JSON (`"K1": [[...]]` or flat) — keys K1/M1, D1, K2/M2,
 * D2, R, T, optional image_width / image_height (default @p default_w x @p
 * default_h: the per-eye size the camera delivers before any resampling).
 * D with 4 or 5 values is RADTAN5, 8 is RADTAN8; "distortion_model": "fisheye"
 * makes 4 values KB4. T in mm; a |T| below 1 is taken as metres and scaled.
 */
bool
u_stereo_uvc_calibration_parse(const char *text,
                               uint32_t default_w,
                               uint32_t default_h,
                               struct xrt_plugin_stereo_camera_calibration *out,
                               char *err,
                               size_t err_cap);


/*
 *
 * Frames.
 *
 */

//! Raw pixel formats a backend hands over (decoded: MJPEG is the backend's job).
enum u_stereo_uvc_pixel
{
	U_STEREO_UVC_PIXEL_NV12 = 1,
	U_STEREO_UVC_PIXEL_YUY2 = 2,
};

struct u_stereo_uvc_raw_frame
{
	uint32_t pixel; //!< enum u_stereo_uvc_pixel
	uint32_t width, height;
	const uint8_t *planes[2]; //!< NV12: Y, UV. YUY2: one packed plane.
	uint32_t pitches[2];
	int64_t time_ns; //!< os_monotonic_get_ns() domain
	bool time_is_exposure;
};

/*!
 * Split + resample one raw SBS frame into an NV12 SBS image of two
 * @p eye_w x @p eye_h eyes, LEFT lens left (swapping the halves when
 * @p swap_eyes). Each eye samples only its own half. Downscale is an integer
 * box pre-reduction then bilinear (pixel-centre mapping), so a 2:1 vertical
 * reduction of a half-SBS frame is an exact 2-row average.
 * @p dst_layout must be the NV12 layout of (2 * eye_w) x eye_h.
 */
bool
u_stereo_uvc_split(const struct u_stereo_uvc_raw_frame *in,
                   bool swap_eyes,
                   uint32_t eye_w,
                   uint32_t eye_h,
                   uint8_t *dst,
                   const struct u_stereo_camera_planes *dst_layout);


/*
 *
 * Capture backend (the OS part).
 *
 */

struct u_stereo_uvc_mode
{
	uint32_t width, height;
	float fps;
};

//! Result of @ref u_stereo_uvc_backend::read.
enum u_stereo_uvc_read
{
	U_STEREO_UVC_READ_OK = 0,
	U_STEREO_UVC_READ_TIMEOUT = 1,
	U_STEREO_UVC_READ_ERROR = 2, //!< the device failed / went away
};

struct u_stereo_uvc_backend
{
	const char *name; //!< "media-foundation", "v4l2", "fake"
	void *ctx;
	//! Capture devices present now. Must NOT open or start any of them.
	uint32_t (*enumerate)(void *ctx, struct u_stereo_uvc_device *out, uint32_t cap);
	/*!
	 * The capture modes of device @p id. May instantiate the OS device object
	 * to read its media types, but never starts streaming. NULL / 0 = unknown.
	 */
	uint32_t (*list_modes)(void *ctx, const char *id, struct u_stereo_uvc_mode *out, uint32_t cap);
	/*!
	 * Open + start @p id at @p mode (exact size; fps best effort). @p out_w x
	 * @p out_h is the frame size the source will resample to (2 x eye width x
	 * eye height): a backend that can decode + scale on the GPU delivers that
	 * instead of the full capture size (half-SBS 3840x2160 -> 2560x720). 0 =
	 * no preference. The source resamples whatever arrives, so honouring it is
	 * optional.
	 */
	bool (*open)(void *ctx,
	             const char *id,
	             const struct u_stereo_uvc_mode *mode,
	             uint32_t out_w,
	             uint32_t out_h,
	             void **out_handle);
	//! Block up to @p timeout_ns for the next decoded frame; valid until the next read / close.
	uint32_t (*read)(void *handle, int64_t timeout_ns, struct u_stereo_uvc_raw_frame *out);
	void (*close)(void *handle);
};

/*!
 * The synthetic backend: one device ("DisplayXR synthetic SBS camera", id
 * "dxr-fake-uvc", no VID:PID), listing 1280x480 / 2560x720 / 1280x960 and
 * opening any even mode, rendering a smooth random texture with the
 * configured disparity + right-eye vertical offset. The scene is defined in
 * EYE-NORMALISED coordinates, so a full-SBS and a half-SBS entry read back
 * the same scene after the split. @p params must outlive the backend (it is
 * the backend's ctx). A config with `"fake"` uses it INSTEAD of OS capture.
 */
void
u_stereo_uvc_fake_backend_init(struct u_stereo_uvc_backend *out, const struct u_stereo_uvc_fake_params *params);


/*
 *
 * The source: the plug-in camera slots' shape, over a backend.
 *
 */

struct u_stereo_uvc;
struct u_stereo_uvc_stream;

/*!
 * Resolve the config against the devices present. @p backend NULL = no OS
 * backend (only a fake device, if configured, can match). Devices whose id /
 * VID:PID equals one of @p exclude_hints (the plug-ins' platform_device_hint)
 * are skipped. One camera per config entry: the first matching, unclaimed
 * device. Always returns an object (zero cameras when nothing matched).
 */
struct u_stereo_uvc *
u_stereo_uvc_create(const struct u_stereo_uvc_config *cfg,
                    const struct u_stereo_uvc_backend *backend,
                    const char *const *exclude_hints,
                    uint32_t exclude_count);

void
u_stereo_uvc_destroy(struct u_stereo_uvc **uvc_ptr);

//! Like the plug-in slot: fill up to @p capacity infos (struct_size preset), return the count.
uint32_t
u_stereo_uvc_enumerate(struct u_stereo_uvc *uvc, uint32_t capacity, struct xrt_plugin_stereo_camera_info *out);

/*!
 * Calibration of camera @p index: the file's (@p out_nominal false; the
 * camera then reports CALIBRATED) or the nominal pinhole (@p out_nominal
 * true). @p out_baseline_known / @p out_hfov_known: whether the config gave
 * them (an unknown one is a default the service must not report).
 */
xrt_result_t
u_stereo_uvc_get_calibration(struct u_stereo_uvc *uvc,
                             uint32_t index,
                             struct xrt_plugin_stereo_camera_calibration *out,
                             bool *out_nominal,
                             bool *out_baseline_known,
                             bool *out_hfov_known);

//! The capture mode camera @p index opens at (diagnostics).
bool
u_stereo_uvc_get_mode(struct u_stereo_uvc *uvc, uint32_t index, struct u_stereo_uvc_mode *out, uint32_t *out_layout);

//! Open (start capturing) camera @p index.
xrt_result_t
u_stereo_uvc_open(struct u_stereo_uvc *uvc, uint32_t index, struct u_stereo_uvc_stream **out_stream);

//! An enum xrt_plugin_stereo_camera_wait. On OK @p out stays valid until release.
uint32_t
u_stereo_uvc_wait_frame(struct u_stereo_uvc_stream *s, int64_t timeout_ns, struct xrt_plugin_stereo_camera_frame *out);

void
u_stereo_uvc_release_frame(struct u_stereo_uvc_stream *s);

void
u_stereo_uvc_close(struct u_stereo_uvc_stream *s);

#ifdef __cplusplus
}
#endif
