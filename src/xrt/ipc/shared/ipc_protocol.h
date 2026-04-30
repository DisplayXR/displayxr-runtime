// Copyright 2020-2024 Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Common protocol definition.
 * @author Pete Black <pblack@collabora.com>
 * @author Jakob Bornecrantz <jakob@collabora.com>
 * @author Korcan Hussein <korcan.hussein@collabora.com>
 * @ingroup ipc_shared
 */

#pragma once

#include "xrt/xrt_limits.h"
#include "xrt/xrt_compiler.h"
#include "xrt/xrt_compositor.h"
#include "xrt/xrt_results.h"
#include "xrt/xrt_defines.h"
#include "xrt/xrt_system.h"
#include "xrt/xrt_session.h"
#include "xrt/xrt_instance.h"
#include "xrt/xrt_compositor.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_space.h"
#include "xrt/xrt_tracking.h"
#include "xrt/xrt_display_metrics.h"
#include "xrt/xrt_config_build.h"

#include <assert.h>
#include <sys/types.h>


#define IPC_CRED_SIZE 1    // auth not implemented
#define IPC_BUF_SIZE 1024  // must be >= largest message length in bytes
#define IPC_MAX_VIEWS 8    // max views we will return configs for
#define IPC_MAX_FORMATS 32 // max formats our server-side compositor supports
#define IPC_MAX_DEVICES 8  // max number of devices we will map using shared mem
#define IPC_MAX_LAYERS XRT_MAX_LAYERS
#define IPC_MAX_SLOTS 128
#define IPC_MAX_CLIENTS 8
#define IPC_MAX_RAW_VIEWS 32 // Max views that we can get, artificial limit.
#define IPC_EVENT_QUEUE_SIZE 32

#define IPC_SHARED_MAX_INPUTS 1024
#define IPC_SHARED_MAX_OUTPUTS 128
#define IPC_SHARED_MAX_BINDINGS 64

// example: v21.0.0-560-g586d33b5
#define IPC_VERSION_NAME_LEN 64

#if defined(XRT_OS_WINDOWS) && !defined(XRT_ENV_MINGW)
typedef int pid_t;
#endif

/*
 *
 * Shared memory structs.
 *
 */

/*!
 * A tracking in the shared memory area.
 *
 * @ingroup ipc
 */
struct ipc_shared_tracking_origin
{
	//! For debugging.
	char name[XRT_TRACKING_NAME_LEN];

	//! What can the state tracker expect from this tracking system.
	enum xrt_tracking_type type;

	//! Initial offset of the tracking origin.
	struct xrt_pose offset;
};

/*!
 * A binding in the shared memory area.
 *
 * @ingroup ipc
 */
struct ipc_shared_binding_profile
{
	enum xrt_device_name name;

	//! Number of inputs.
	uint32_t input_count;
	//! Offset into the array of pairs where this input bindings starts.
	uint32_t first_input_index;

	//! Number of outputs.
	uint32_t output_count;
	//! Offset into the array of pairs where this output bindings starts.
	uint32_t first_output_index;
};

/*!
 * A device in the shared memory area.
 *
 * @ingroup ipc
 */
struct ipc_shared_device
{
	//! Enum identifier of the device.
	enum xrt_device_name name;
	enum xrt_device_type device_type;

	//! Which tracking system origin is this device attached to.
	uint32_t tracking_origin_index;

	//! A string describing the device.
	char str[XRT_DEVICE_NAME_LEN];

	//! A unique identifier. Persistent across configurations, if possible.
	char serial[XRT_DEVICE_NAME_LEN];

	//! Number of bindings.
	uint32_t binding_profile_count;
	//! 'Offset' into the array of bindings where the bindings starts.
	uint32_t first_binding_profile_index;

	//! Number of inputs.
	uint32_t input_count;
	//! 'Offset' into the array of inputs where the inputs starts.
	uint32_t first_input_index;

	//! Number of outputs.
	uint32_t output_count;
	//! 'Offset' into the array of outputs where the outputs starts.
	uint32_t first_output_index;

	//! The supported fields.
	struct xrt_device_supported supported;
};

/*!
 * Data for a single composition layer.
 *
 * Similar in function to @ref comp_layer
 *
 * @ingroup ipc
 */
struct ipc_layer_entry
{
	//! @todo what is this used for?
	uint32_t xdev_id;

	/*!
	 * Up to two indices of swapchains to use.
	 *
	 * How many are actually used depends on the value of @p data.type
	 */
	uint32_t swapchain_ids[XRT_MAX_VIEWS * 2];

	/*!
	 * All basic (trivially-serializable) data associated with a layer,
	 * aside from which swapchain(s) are used.
	 */
	struct xrt_layer_data data;
};

/*!
 * Render state for a single client, including all layers.
 *
 * @ingroup ipc
 */
struct ipc_layer_slot
{
	struct xrt_layer_frame_data data;
	uint32_t layer_count;
	struct ipc_layer_entry layers[IPC_MAX_LAYERS];
};

/*!
 * A big struct that contains all data that is shared to a client, no pointers
 * allowed in this. To get the inputs of a device you go:
 *
 * ```C++
 * struct xrt_input *
 * helper(struct ipc_shared_memory *ism, uint32_t device_id, uint32_t input)
 * {
 * 	uint32_t index = ism->isdevs[device_id]->first_input_index + input;
 * 	return &ism->inputs[index];
 * }
 * ```
 *
 * @ingroup ipc
 */
struct ipc_shared_memory
{
	/*!
	 * The git revision of the service, used by clients to detect version mismatches.
	 */
	char u_git_tag[IPC_VERSION_NAME_LEN];

	/*!
	 * Number of elements in @ref itracks that are populated/valid.
	 */
	uint32_t itrack_count;

	/*!
	 * @brief Array of shared tracking origin data.
	 *
	 * Only @ref itrack_count elements are populated/valid.
	 */
	struct ipc_shared_tracking_origin itracks[XRT_SYSTEM_MAX_DEVICES];

	/*!
	 * Number of elements in @ref isdevs that are populated/valid.
	 */
	uint32_t isdev_count;

	/*!
	 * @brief Array of shared data per device.
	 *
	 * Only @ref isdev_count elements are populated/valid.
	 */
	struct ipc_shared_device isdevs[XRT_SYSTEM_MAX_DEVICES];

	/*!
	 * Various roles for the devices.
	 */
	struct
	{
		int32_t head;
		int32_t eyes;
		int32_t face;
		int32_t body;

		struct
		{
			struct
			{
				int32_t left;
				int32_t right;
			} unobstructed;

			struct
			{
				int32_t left;
				int32_t right;
			} conforming;
		} hand_tracking;
	} roles;

	struct
	{
		struct
		{
			/*!
			 * Pixel properties of this display, not in absolute
			 * screen coordinates that the compositor sees. So
			 * before any rotation is applied by xrt_view::rot.
			 *
			 * The xrt_view::display::w_pixels &
			 * xrt_view::display::h_pixels become the recommended
			 * image size for this view.
			 *
			 * @todo doesn't account for overfill for timewarp or
			 * distortion?
			 */
			struct
			{
				uint32_t w_pixels;
				uint32_t h_pixels;
			} display;
		} views[XRT_MAX_VIEWS];
		//! Number of valid views
		uint32_t view_count;
		enum xrt_blend_mode blend_modes[XRT_MAX_DEVICE_BLEND_MODES];
		uint32_t blend_mode_count;
		uint32_t rendering_mode_count;
		struct xrt_rendering_mode rendering_modes[XRT_MAX_RENDERING_MODES];
		uint32_t active_rendering_mode_index;
	} hmd;

	struct xrt_input inputs[IPC_SHARED_MAX_INPUTS];

	struct xrt_output outputs[IPC_SHARED_MAX_OUTPUTS];

	struct ipc_shared_binding_profile binding_profiles[IPC_SHARED_MAX_BINDINGS];
	struct xrt_binding_input_pair input_pairs[IPC_SHARED_MAX_INPUTS];
	struct xrt_binding_output_pair output_pairs[IPC_SHARED_MAX_OUTPUTS];

	struct ipc_layer_slot slots[IPC_MAX_SLOTS];

	uint64_t startup_timestamp;
	struct xrt_plane_detector_begin_info_ext plane_begin_info_ext;
};

/*!
 * Initial info from a client when it connects.
 */
struct ipc_client_description
{
	pid_t pid;
	struct xrt_application_info info;
};

struct ipc_client_list
{
	uint32_t ids[IPC_MAX_CLIENTS];
	uint32_t id_count;
};

/*!
 * Phase 5.8: registered-app record shipped one-at-a-time from the workspace
 * controller to the service for the spatial launcher panel. Sized to fit a
 * single ipc message under IPC_BUF_SIZE — the workspace controller calls
 * ipc_call_launcher_clear_apps then loops over its registry calling
 * ipc_call_launcher_add_app per entry whenever the registry changes.
 *
 * @ingroup ipc
 */

#define IPC_LAUNCHER_MAX_APPS 32
#define IPC_LAUNCHER_NAME_MAX 96
#define IPC_LAUNCHER_PATH_MAX 256
#define IPC_LAUNCHER_TYPE_MAX 8

// Phase 5.14: sentinel returned via ipc_call_launcher_poll_click to mean
// "the user clicked the Browse-for-app virtual tile" rather than a real tile.
// Real tile indices are >= 0; -1 means no pending action.
#define IPC_LAUNCHER_ACTION_BROWSE (-100)

// Phase 6.6: sentinel base for permanent remove. Returned as
// -(IPC_LAUNCHER_ACTION_REMOVE_BASE + full_index). Workspace controller decodes via
// full_index = -(value) - IPC_LAUNCHER_ACTION_REMOVE_BASE.
#define IPC_LAUNCHER_ACTION_REMOVE_BASE 200

// Phase 6.6: sentinel for "refresh app list" (re-scan sidecars).
#define IPC_LAUNCHER_ACTION_REFRESH (-300)

struct ipc_launcher_app
{
	char name[IPC_LAUNCHER_NAME_MAX];
	char exe_path[IPC_LAUNCHER_PATH_MAX];
	char type[IPC_LAUNCHER_TYPE_MAX]; // "3d" or "2d"
	char icon_path[IPC_LAUNCHER_PATH_MAX];
	char icon_3d_path[IPC_LAUNCHER_PATH_MAX];
	char icon_3d_layout[IPC_LAUNCHER_TYPE_MAX]; // "sbs-lr" etc
};

/*!
 * Phase 2.D: workspace input event wire format. Tagged union with
 * event_type as the discriminator. Mirrors the public XrWorkspaceInputEventEXT
 * but uses plain C types (no XR enum dependency in IPC headers). The state
 * tracker translates between this wire form and the public form so the
 * extension surface stays decoupled from IPC.
 *
 * Cursor coordinates are int64_t over the wire because the proto generator
 * does not support int32_t (see ipcproto/common.py); the public surface
 * uses int32_t and the state tracker truncates at the boundary.
 *
 * Sized to fit IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX events in IPC_BUF_SIZE.
 *
 * @ingroup ipc
 */
#define IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX 16

enum ipc_workspace_input_event_type
{
	IPC_WORKSPACE_INPUT_EVENT_POINTER        = 0,
	IPC_WORKSPACE_INPUT_EVENT_POINTER_HOVER  = 1,
	IPC_WORKSPACE_INPUT_EVENT_KEY            = 2,
	IPC_WORKSPACE_INPUT_EVENT_SCROLL         = 3,
	IPC_WORKSPACE_INPUT_EVENT_POINTER_MOTION = 4, //!< spec_version 6
	IPC_WORKSPACE_INPUT_EVENT_FRAME_TICK     = 5, //!< spec_version 6
	IPC_WORKSPACE_INPUT_EVENT_FOCUS_CHANGED  = 6, //!< spec_version 6
};

struct ipc_workspace_input_event
{
	uint32_t event_type;       //!< enum ipc_workspace_input_event_type
	uint32_t timestamp_ms;     //!< Host monotonic ms, low 32 bits.
	union
	{
		struct
		{
			uint32_t hit_client_id;
			uint32_t hit_region;     //!< XrWorkspaceHitRegionEXT cast to uint32_t
			float    local_u;
			float    local_v;
			int64_t  cursor_x;
			int64_t  cursor_y;
			uint32_t button;         //!< 1=L, 2=R, 3=M
			uint32_t is_down;        //!< XrBool32 semantics
			uint32_t modifiers;      //!< bit0=SHIFT, bit1=CTRL, bit2=ALT
			uint32_t _pad;
		} pointer;
		struct
		{
			uint32_t prev_client_id;
			uint32_t prev_region;
			uint32_t curr_client_id;
			uint32_t curr_region;
		} pointer_hover;
		struct
		{
			uint32_t vk_code;
			uint32_t is_down;
			uint32_t modifiers;
		} key;
		struct
		{
			float    delta_y;        //!< Wheel ticks; positive = scroll up
			int64_t  cursor_x;
			int64_t  cursor_y;
			uint32_t modifiers;
		} scroll;
		struct                      //!< spec_version 6: per-frame motion (capture-gated)
		{
			uint32_t hit_client_id;
			uint32_t hit_region;
			float    local_u;
			float    local_v;
			int64_t  cursor_x;
			int64_t  cursor_y;
			uint32_t button_mask;    //!< bit0=L, bit1=R, bit2=M (currently held)
			uint32_t modifiers;
		} pointer_motion;
		struct                      //!< spec_version 6: vsync-aligned frame tick
		{
			uint64_t timestamp_ns;
		} frame_tick;
		struct                      //!< spec_version 6: focused-client transition
		{
			uint32_t prev_client_id;
			uint32_t curr_client_id;
		} focus_changed;
	} u;
};

struct ipc_workspace_input_event_batch
{
	uint32_t count;
	struct ipc_workspace_input_event events[IPC_WORKSPACE_INPUT_EVENT_BATCH_MAX];
};

/*!
 * Phase 8: 3D capture MVP. Bitmask of which views (sub-images) the workspace controller
 * is requesting from the service compositor's combined atlas.
 *
 * @ingroup ipc
 */
#define IPC_CAPTURE_FLAG_ATLAS (1u << 0)
#define IPC_CAPTURE_FLAG_ALL (IPC_CAPTURE_FLAG_ATLAS)

#define IPC_CAPTURE_PATH_MAX 256

/*!
 * Phase 8: request struct for workspace_capture_frame. Wraps the path prefix
 * (without extension — runtime appends "_atlas.png") because the IPC
 * schema only supports struct/scalar parameter types.
 *
 * @ingroup ipc
 */
struct ipc_capture_request
{
	char path_prefix[IPC_CAPTURE_PATH_MAX];
	uint32_t flags; // IPC_CAPTURE_FLAG_* bitmask
};

/*!
 * Phase 8: result returned by workspace_capture_frame. The runtime fills this
 * with the metadata needed for a sidecar JSON file (timestamp, atlas/eye
 * dimensions, stereo layout, display physical size, eye poses at capture).
 *
 * @ingroup ipc
 */
struct ipc_capture_result
{
	uint64_t timestamp_ns;
	uint32_t atlas_width;
	uint32_t atlas_height;
	uint32_t eye_width;
	uint32_t eye_height;
	uint32_t views_written; // bitmask of IPC_CAPTURE_FLAG_* actually written
	uint32_t tile_columns;
	uint32_t tile_rows;
	float display_width_m;
	float display_height_m;
	float eye_left_m[3];
	float eye_right_m[3];
	char _pad[16];
};

/*!
 * State for a connected application.
 *
 * @ingroup ipc
 */
struct ipc_app_state
{
	// Stable and unique ID of the client, only unique within this instance.
	uint32_t id;

	bool primary_application;
	bool session_active;
	bool session_visible;
	bool session_focused;
	bool session_overlay;
	bool io_active;
	uint32_t z_order;
	pid_t pid;
	struct xrt_application_info info;
};


/*!
 * Arguments for creating swapchains from native images.
 */
struct ipc_arg_swapchain_from_native
{
	uint32_t sizes[XRT_MAX_SWAPCHAIN_IMAGES];
};

/*!
 * Arguments for xrt_device::get_view_poses with two views.
 */
struct ipc_info_get_view_poses_2
{
	struct xrt_fov fovs[XRT_MAX_VIEWS];
	struct xrt_pose poses[XRT_MAX_VIEWS];
	struct xrt_space_relation head_relation;
};

struct ipc_pcm_haptic_buffer
{
	uint32_t num_samples;
	float sample_rate;
	bool append;
};
