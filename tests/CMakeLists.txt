# Copyright 2018-2024, Collabora, Ltd.
# Copyright 2024-2025, NVIDIA CORPORATION.
#
# SPDX-License-Identifier: BSL-1.0

set(tests
    tests_camera_profile
    tests_oxr_session_window_binding
    tests_cxx_wrappers
    tests_deque
    tests_generic_callbacks
    tests_history_buf
    tests_id_ringbuffer
    tests_json
    tests_lowpass_float
    tests_lowpass_integer
    tests_pacing
    tests_quatexpmap
    tests_quat_change_of_basis
    tests_quat_swing_twist
    tests_rational
    tests_relation_chain
    tests_vector
    tests_worker
    tests_pose
    tests_view_rig_point
    tests_vec3_angle
    tests_u_multi_display
    tests_dp_weave_scope
    tests_dp_vk_snap_window_rect
    tests_weave_snap_grid
    tests_lift_mailbox
    tests_sim_conversion
    tests_remote_conversion
    tests_conversion_abi
    tests_conv_wire
    tests_conv_select
    tests_conv_readiness
    tests_conv_registry
    tests_conv_store
    tests_conv_lanes
    tests_conv_supervisor
    tests_conv_shm_ring
    tests_lift_abi
    tests_conv_viewsyn_math
    tests_aux_lift_viewpoint
    tests_aux_lift_rig_depth
    tests_space_overseer_rig
    tests_rig_composer
    tests_aux_x11_scale
    tests_aux_display_desktop_select
    tests_cli_dims_check
    tests_cli_metres_check
    tests_cli_lever_catalog
    tests_cli_input_predict
    tests_cli_dp_predict
    tests_comp_vblank_grid
    tests_comp_present_feedback
    tests_comp_late_weave_adapt
    tests_comp_residual_window
    tests_comp_feedback_pace
    tests_comp_late_weave_depth
    tests_comp_zone_tier1
    tests_comp_swapchain_ring
    tests_aux_bg_neutrality
    tests_aux_color_encoding
    tests_aux_grip_surface
    tests_aux_cursor_depth
    tests_aux_lift_cursor
    tests_aux_lift_autoconv
    tests_aux_rear_budget
    tests_aux_route_policy
    tests_aux_vk_queue_lock_route
    tests_aux_present_owner_route
    tests_aux_overlay_lease
    tests_aux_mini_window_tell
    tests_aux_zone_canvas
    tests_aux_tiling_zero_copy
    tests_aux_screen_info
    tests_ipc_app_group
    tests_status_snapshot
    tests_status_ipc
    tests_aux_display_edid_parse
    tests_aux_tiling_row_origin
    tests_aux_window_replug
    tests_aux_wayland_geom
    tests_comp_wl_move_sync_tag
    tests_aux_weave_span2d
    tests_aux_weave_present_origin
    tests_aux_weave_flat
    tests_aux_display_mode_hold
    tests_aux_tracking_any
    tests_comp_rear_budget
    tests_comp_segments
    tests_comp_lazy_transparency
    tests_comp_overlay_2d_vk
    tests_dp_vk_weave_sync
    tests_oxr_weave_latch
    tests_oxr_mcp_app_result
    tests_oxr_layer_space
    tests_oxr_view_config_rule
    tests_oxr_segment_views
    tests_segments_ipc
    tests_weave_segments
    tests_comp_workspace_screens
    tests_target_screen_pin
    tests_u_setting_per_screen
    tests_u_file_socket_path
    tests_u_eye_history
    tests_sim_eyes
    tests_dp_eye_positions_at
    tests_oxr_legacy_mode_rule
    tests_oxr_mode_fillable_rule
    tests_oxr_tracking_properties_rule
    tests_oxr_input_providers_rule
    tests_xrt_active_mode_scale
    tests_input_arbiter_rig
    tests_input_arbiter_rerank
    tests_input_arbiter_floor
    tests_input_arbiter_per_client
    tests_sim_input_nav
    tests_sim_input_hands
    tests_gamepad_map
    tests_touch_gesture
    tests_comp_layer_view_camera
    tests_comp_color_policy
    tests_m_matrix_projection
    tests_stereo_camera
    tests_camera_consent
    tests_stereo_rectify
    tests_stereo_vrefine
    tests_stereo_uvc
    tests_sim_webcam_tracking
	)
if(XRT_BUILD_DRIVER_QWERTY)
	list(APPEND tests tests_qwerty_camera_profile tests_qwerty_velocity)
endif()
# ADR-052 -- XR_DXR_spatial_workspace v25 over IPC: the screen list and the
# placement through their wire forms. Needs the generated protocol.
if(TARGET ipc_shared)
	list(APPEND tests tests_workspace_screens_ipc)
endif()
# #1727 -- the X11 qwerty front-end's edge logic (one click = one select edge).
if(XRT_BUILD_DRIVER_QWERTY AND XRT_HAVE_XCB AND NOT ANDROID)
	list(APPEND tests tests_qwerty_xcb)
endif()

# #1380 -- the input-provider host iface's display-geometry cache. Needs the
# loader itself (target_lists), so it is only registered where that target
# exists; everything it drives is the exact function pointer providers get.
if(TARGET target_lists)
	list(APPEND tests tests_input_host_geometry)
	# ADR-053 / #1999 -- the conversion-provider loader: manifest + registry row
	# parsing, ordering, the plug-in-kind router and the ABI gate.
	list(APPEND tests tests_conversion_loader)
	# ADR-054 N2 -- the shared component loader: the class-agnostic manifest /
	# registry row reader, ordering + shadowing, the load path and the ABI gate,
	# driven by test-only class rows.
	list(APPEND tests tests_component_loader)
	# ADR-054 N3c -- the one options contract's store: resolution order
	# (environment > user > machine / manifest), the DXR_<CLASS>_<ID>_<KEY>
	# mapping, the per-user JSON lookup, and a live POSIX round trip.
	list(APPEND tests tests_component_settings)
	# ADR-054 N7 (#2143) -- the EmulatedControllers gate: the root-policy
	# resolve rule (user over machine, under Policy\Lock) and the input
	# loader's live POSIX per-user file read with HOME redirected.
	list(APPEND tests tests_input_emulated_controllers)
	# ADR-034 Amendment 5 (#2181) -- per-client input selection policy: the
	# per-family precedence (Lock > per-app pin > request > PreferredPlugin),
	# the exe-basename per-app key, and the live POSIX per-app pin file.
	list(APPEND tests tests_input_selection_policy)
	# ADR-053 / #2006 -- conversion diagnostics: the snapshot's conversion rows
	# (DIAG reply size, generation, CONV_* warnings, `conv list --json`), and the
	# headless pass + selftest exit-13 path on stub providers.
	list(APPEND tests tests_conv_snapshot)
	# Multi-screen M1 -- target_screens_build: registry -> per-screen list.
	list(APPEND tests tests_target_screens)
	# ADR-047 Amendment 8 -- display hotplug: the claim sweep's scheduler, the
	# per-monitor winner across a hotplug, the published-list comparison.
	list(APPEND tests tests_target_display_hotplug)
endif()
# Multi-screen M0 (#69) -- where a plug-in without probe_displays gets its
# synthesized display claim (its panel's monitor, not the primary). Pure
# policy in the loader, so it needs target_lists like the test above.
if(TARGET target_lists)
	list(APPEND tests tests_target_backcompat_claim)
endif()
# Display dashboard phase 8 -- workspace-controller launch settings: the hotkey
# grammar, service.json's `controllers` map, the active-controller selection
# rule and the `workspace list --json` document (target_workspace_config).
if(TARGET target_workspace_config)
	list(APPEND tests tests_service_workspace_config)
endif()
# #960 CONTROLLER-class verification on POSIX: realpath-compare of the peer's
# (kernel-resolved) executable against a manifest `binary` that may be a symlink.
if(TARGET target_workspace_config AND NOT WIN32)
	list(APPEND tests tests_service_client_class)
endif()
# MCP capability marker paths per platform (desktop Linux: XDG config, /etc).
list(APPEND tests tests_u_mcp_capability)
# Desktop-Linux workspace orchestrator (spawn / respawn / hotkey arming, with a
# stubbed hotkey backend and a shell-script controller) and the Linux hotkey
# worker built without its D-Bus / X11 backends -- neither touches the desktop.
if(TARGET target_workspace_config AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
	list(APPEND tests tests_service_orchestrator_linux tests_service_hotkey_linux)
endif()
# ADR-051 D6: the service's DRM hotplug feeder -- the kernel-uevent parser and
# the polling fallback's status hash, against recorded uevent payloads.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
	list(APPEND tests tests_service_drm_hotplug_linux)
endif()
if(XRT_HAVE_D3D11)
	list(APPEND tests tests_aux_d3d_d3d11 tests_aux_d3d_render_adapter tests_comp_client_d3d11
	     tests_comp_xbridge_plane_policy tests_comp_split_gate tests_comp_eye_dp_policy
		)
endif()
if(XRT_HAVE_D3D12)
	list(APPEND tests tests_comp_client_d3d12)
	# #1503 -- the D3D12 swapchain image format policy (typed request ->
	# TYPELESS resource). Pure header functions plus one optional
	# device-backed creation check that self-skips without a D3D12 device.
	list(APPEND tests tests_comp_d3d12_swapchain_format)
endif()
if(WIN32 AND (XRT_HAVE_D3D11 OR XRT_HAVE_D3D12))
	# #918 D12-1 — the masked-composite HLSL that comp_d3d11_outcomp and
	# comp_d3d12_outcomp both compile. D3DCompile/D3DReflect need no device.
	list(APPEND tests tests_comp_masked_composite_shaders)
endif()
if(WIN32 AND XRT_HAVE_D3D11)
	# #1601 — the layered (Texture2DArray) layer-shader variants that carry
	# subImage.imageArrayIndex, in the in-process D3D11 renderer and in the
	# D3D11 service. Reflection only; D3DCompile/D3DReflect need no device.
	list(APPEND tests tests_comp_layer_array_shaders)
	# #1601 — the pixel oracle the reflection suite cannot be: does the slice
	# index actually reach the sampler. WARP, so no GPU; SKIPs if no D3D11
	# device exists at all.
	list(APPEND tests tests_comp_layer_array_warp)
	# #1602 — the equirect2 shader pair: does a screen ray land on the right
	# part of the sphere, and does a layer stay inside its angular extent.
	# WARP, same as above; SKIPs if no D3D11 device exists at all.
	list(APPEND tests tests_comp_layer_equirect2_warp)
	# #1956 — the D3D11 service's swapchain format policy (VkFormat wire map,
	# typeless depth, bind flags, advertised depth set) plus a WARP check that a
	# depth image created the production way shares across devices (NT handle,
	# keyed mutex, D3D11 + D3D12 open). SKIPs if no WARP device exists.
	list(APPEND tests tests_comp_d3d11_service_formats)
endif()
# #1956 — depth swapchains for GL clients over IPC (WGL_NV_DX_interop2 path):
# the GL depth recipe / format filter / transfer layout, cross-checked against
# the D3D11 service's format policy, plus a WARP run of the D3D11 half of the
# release copy (colour-family staging -> TYPELESS shared depth under the keyed
# mutex). SKIPs if no WARP device exists.
if(WIN32 AND XRT_HAVE_D3D11 AND XRT_HAVE_OPENGL AND TARGET aux_ogl)
	list(APPEND tests tests_comp_gl_d3d11_depth)
endif()
if(TARGET comp_d3d11_lift)
	# ADR-053 P1a (#2003) -- the legacy lift path's golden replay on WARP: the
	# sim_display fake behind d3d11_lift, its output bytes pinned before the
	# conversion core took over the lift thread. SKIPs without a D3D11 device.
	list(APPEND tests tests_lift_dp_golden)
	if(TARGET ipc_server_conv)
		# #2054 -- XR_DXR_lift spec-4 transports on WARP (D3D11 NT + region,
		# NT + D3D fence, a D3D12 resource + ID3D12Fence, CPU bytes, CPU
		# export), each reproducing tests_lift_dp_golden's pinned SBS bytes.
		list(APPEND tests tests_ipc_conv_v4_transports)
	endif()
endif()
if(TARGET comp_d3d11_conv_host_lane)
	# ADR-053 P2 (#2034) -- the satellite's D3D11 lane on WARP (debug layer when
	# installed): the host fence is observed on the CPU before any copy, and a
	# host that never signals costs a bounded CPU wait, never a GPU one.
	list(APPEND tests tests_conv_host_lane_d3d11)
endif()
if(WIN32)
	# ADR-053 P2 (#2035) -- roadmap §11.2: shared NT-handle textures + shared
	# fences across a process boundary on WARP, a queue wait on a fence whose
	# signaller died, and the wrong-adapter open (hardware: DXR_TEST_D3D11_HW=1).
	list(APPEND tests tests_conv_d3d11_xproc)
endif()
if(TARGET comp_d3d11_conv_viewsyn)
	# ADR-053 S5 -- the reference view synthesis on WARP: a synthetic step edge
	# rendered into 2 and 4 views, parallax checked against u_conv_viewsyn's
	# CPU march. SKIPs if no D3D11 device exists at all.
	list(APPEND tests tests_conv_viewsyn_d3d11)
endif()
if(TARGET comp_vk_conv_viewsyn)
	# ADR-053 P5 -- the Vulkan reference view synthesis against a CPU golden of
	# the same passes, plus the D3D11 test's step-edge oracles. SKIPs with no
	# Vulkan device.
	list(APPEND tests tests_conv_viewsyn_vk)
endif()
if(TARGET comp_vk_conv)
	# ADR-053 P5 -- the Vulkan GPU glue: the allocation rule, exportable memory
	# (opaque fd / dma-buf on Linux, IOSurface export + import on macOS), CPU <->
	# image round trips, the mirror upload, S6 readback, S5 on CPU inputs, and a
	# composed conversion through the core. SKIPs with no Vulkan device.
	list(APPEND tests tests_conv_vk_gpu)
endif()
if(TARGET comp_d3d11_conv_content)
	# #2030 -- still-image dedupe of GPU PHOTO inputs on WARP: the content
	# check's key drives the core (two identical inputs -> one convert, a
	# changed block -> another). SKIPs without a D3D11 device.
	list(APPEND tests tests_conv_gpu_dedupe)
endif()
if(XRT_HAVE_VULKAN)
	list(APPEND tests tests_comp_client_vulkan tests_uv_to_tangent)
endif()
if(XRT_HAVE_VULKAN
   AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
   AND NOT ANDROID
	)
	# #1699 R5 -- vk/vk_dmabuf.h: dma-buf export/import round trip, SYNC_FD
	# semaphores, fd hygiene, failure paths closing the fd. Headless; runs on
	# every Vulkan device present and SKIPs when none offers dma-buf + DRM
	# format modifiers (a CI runner's lavapipe has no DRM device).
	list(APPEND tests tests_vk_dmabuf)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID)
	# Multi-screen M0 (#69) -- the desktop-Linux EDID enumeration: EDID
	# parser, DRM sysfs reader (against a temp-dir fixture) and the
	# RandR <-> DRM join. Hardware-free.
	list(APPEND tests tests_aux_display_edid_linux)
	# sim_display adopts a real monitor's device mode + EDID size on desktop
	# Linux (the #369 fix's twin): the pick, pinned on the DS1 box's records
	# and end to end from a temp-dir DRM sysfs fixture.
	list(APPEND tests tests_sim_display_monitor_geometry)
endif()
if(APPLE)
	# macOS monitor enumeration -- CG vendor byte order, the EDID <-> display
	# match, the shared parser on an IOKit EDID, and a live smoke check that
	# SKIPs on a headless runner.
	list(APPEND tests tests_aux_display_edid_macos)
endif()
if(TARGET comp_vk_native
   AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
   AND NOT ANDROID
	)
	# #1602 -- the vk_native equirect2 draw, rendered: the Vulkan twin of
	# tests_comp_layer_equirect2_warp. Prefers a CPU (lavapipe) device and
	# SKIPs when there is no Vulkan device at all.
	list(APPEND tests tests_comp_vk_equirect2_draw)
endif()
if(XRT_FEATURE_OPENXR)
	list(APPEND tests tests_input_transform)
endif()
# #2005 -- the XR_DXR_lift v3 startup sequence (properties -> stream ->
# submit -> first result, IPC session and ADR-049 lift-only link), pinned
# before ADR-053 re-points the handlers. Needs the generated IPC client header
# and the lift entry points (not built on Android).
if(XRT_FEATURE_OPENXR
   AND TARGET ipc_client
   AND NOT ANDROID
	)
	list(APPEND tests tests_oxr_lift_startup)
endif()
# #2005 -- the v3 lift handlers routed to the conversion core (the adapter
# the handlers call, server/ipc_server_conv.c), driven with the CPU fake
# provider. Needs the ipc_server_conv target (built with the IPC subtree).
if(TARGET ipc_server_conv)
	list(APPEND tests tests_ipc_lift_core)
	# #2003 -- the legacy DP-lift adapter + the legacy golden replay through the
	# core and the v3 route, with a scripted lift-thread backend (no GPU).
	list(APPEND tests tests_conv_dp_lift)
	# #2054 -- XR_DXR_lift spec 4 at the conv_* adapter: the create rules, the
	# opt-in / ordered / deduplicated task events, ProvidersChanged on an
	# install, the depth-kind contract; and the four v3-observable shifts as
	# golden tests (spec XR_DXR_lift.md section 14.2).
	list(APPEND tests tests_ipc_conv_v4 tests_lift_v3_shifts)
	if(NOT WIN32)
		# The POSIX CPU I/O glue (spec-4 CPU kinds without a D3D11 device).
		list(APPEND tests tests_ipc_conv_cpu)
	endif()
endif()
if(XRT_FEATURE_OPENXR)
	# #1488 -- XR_EXT_view_configuration_views_change. Compiles
	# oxr_views_change.c straight into the unit and injects now_ns, so the
	# 1 Hz throttle is deterministic (no sleeping, no wall clock, no
	# compositor). Its integration half additionally dlopens the built
	# runtime when there is one, and SKIPs without a registered DP.
	list(APPEND tests tests_oxr_view_config_views_change)
endif()
if(XRT_FEATURE_OPENXR AND XRT_FEATURE_OPENXR_HEADLESS AND RUNTIME_TARGET)
	# #1370 -- drives the BUILT runtime library through the public OpenXR API
	# on a headless session (see the file header). Needs a registered display
	# processor at run time and SKIPs without one, so the unregistered ctest
	# pass stays green; CI re-runs it after registering sim-display.
	list(APPEND tests tests_oxr_view_space)
endif()
if(XRT_HAVE_OPENGL
   AND XRT_HAVE_OPENGL_GLX
   AND XRT_HAVE_SDL2
   AND (WIN32 OR VK_USE_PLATFORM_XLIB_XRANDR_EXT)
	)
	set(_have_opengl_test ON)
	list(APPEND tests tests_comp_client_opengl)
endif()
# ADR-053 D5 (#2033) -- the conversion-host satellite binary, spawned with the
# sim provider over a loopback channel; describe / warm / one submit through
# the shm rings, and the exit-code table. Headless (CPU lane).
if(TARGET displayxr-conversion-host
   AND TARGET drv_sim_conversion_plugin
   AND TARGET target_lists
	)
	list(APPEND tests tests_conv_host_satellite)
endif()

# ADR-053 P2 (#2032): the OS process layer of the conversion-host satellite
# (os_process.h) against a real helper child. Windows, desktop Linux, macOS.
if(WIN32 OR APPLE OR (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ANDROID))
	list(APPEND tests tests_conv_os_process)
	# #2034: the satellite host proxy against a scripted host process.
	list(APPEND tests tests_conv_proxy)
endif()

# ADR-056 -- XR_DXR_desktop_control's common layer (consent state machine,
# store + audit, queue, safety layer) end to end over the in-memory fake
# backend. Desktop OSes only, like aux_desktop itself.
if(TARGET aux_desktop)
	list(APPEND tests tests_desktop_control)
endif()

foreach(testname ${tests})
	add_executable(${testname} ${testname}.cpp)
	target_link_libraries(${testname} PRIVATE xrt-external-catch2)
	target_link_libraries(${testname} PRIVATE aux_util)
	add_test(NAME ${testname} COMMAND ${testname} --success --allow-running-no-tests)
endforeach()

# For tests that require more than just aux_util, link those other libs down here.

target_include_directories(tests_oxr_session_window_binding PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/state_trackers/oxr)

if(TARGET tests_desktop_control)
	target_link_libraries(tests_desktop_control PRIVATE aux_desktop)
endif()

# #1594 -- compiles oxr_layer_space.c straight into the unit and injects the
# relations oxr_space_locate_device() would have returned, so the VIEW/LOCAL
# coincidence is pinned with no session, no device and no GPU.
target_sources(
	tests_oxr_layer_space
	PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/state_trackers/oxr/oxr_layer_space.c
	)
target_include_directories(
	tests_oxr_layer_space
	PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/state_trackers/oxr
		${PROJECT_SOURCE_DIR}/src/xrt/auxiliary
	)
target_link_libraries(tests_oxr_layer_space PRIVATE aux_math)

if(TARGET tests_oxr_view_config_views_change)
	# PROJECT_SOURCE_DIR, not CMAKE_SOURCE_DIR -- the vendor plug-in's signing
	# recipe builds this runtime as a sub-project (see tests_rig_composer).
	target_sources(
		tests_oxr_view_config_views_change
		PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/state_trackers/oxr/oxr_views_change.c
		)
	target_include_directories(
		tests_oxr_view_config_views_change
		PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/state_trackers/oxr
			${PROJECT_SOURCE_DIR}/src/xrt/auxiliary
			${PROJECT_SOURCE_DIR}/src/external/openxr_includes
		)
	target_link_libraries(tests_oxr_view_config_views_change PRIVATE aux_os xrt-interfaces)
	if(RUNTIME_TARGET)
		# Enables the integration half; without a runtime target the unit
		# half still builds and runs.
		target_compile_definitions(
			tests_oxr_view_config_views_change
			PRIVATE "DXR_RUNTIME_LIB_PATH=\"$<TARGET_FILE:${RUNTIME_TARGET}>\""
			)
		add_dependencies(tests_oxr_view_config_views_change ${RUNTIME_TARGET})
		if(NOT WIN32)
			target_link_libraries(tests_oxr_view_config_views_change PRIVATE ${CMAKE_DL_LIBS})
		endif()
	endif()
endif()

if(TARGET tests_qwerty_camera_profile)
	target_link_libraries(tests_qwerty_camera_profile PRIVATE drv_qwerty drv_qwerty_includes)
endif()

if(TARGET tests_qwerty_velocity)
	target_link_libraries(tests_qwerty_velocity PRIVATE drv_qwerty drv_qwerty_includes aux_math)
endif()

if(TARGET tests_qwerty_xcb)
	target_link_libraries(tests_qwerty_xcb PRIVATE drv_qwerty drv_qwerty_includes)
endif()

if(WIN32 AND XRT_HAVE_D3D11 AND XRT_FEATURE_OPENXR AND XRT_FEATURE_OPENXR_HEADLESS AND RUNTIME_TARGET)
	# Explicit acceptance probe: opens native windows, so never an automatic CTest.
	add_executable(tests_oxr_camera_profile EXCLUDE_FROM_ALL tests_oxr_camera_profile.cpp)
	target_include_directories(tests_oxr_camera_profile PRIVATE ${PROJECT_SOURCE_DIR}/src/external/openxr_includes)
	target_compile_definitions(tests_oxr_camera_profile PRIVATE "DXR_RUNTIME_LIB_PATH=\"$<TARGET_FILE:${RUNTIME_TARGET}>\"")
	target_link_libraries(tests_oxr_camera_profile PRIVATE d3d11 dxgi)
	add_dependencies(tests_oxr_camera_profile ${RUNTIME_TARGET})
endif()

# ADR-030/ADR-041 -- u_tiling_can_zero_copy(), the sole zero-copy gate. Header
# arithmetic only; it reads xrt_rendering_mode, hence xrt-interfaces.
target_link_libraries(tests_aux_tiling_zero_copy PRIVATE xrt-interfaces)

# Multi-screen M1 -- per-screen display-info derivation (u_screen_info.h):
# system-default / plug-in per-monitor slot / EDID-derived rules. Header only;
# it reads the plug-in ABI structs, hence xrt-interfaces.
target_link_libraries(tests_aux_screen_info PRIVATE xrt-interfaces)

# Input focus follows the application (#962 follow-up): the pure sibling rule
# in ipc/shared/ipc_app_group.h — name, process tree, executable tie-break.
target_link_libraries(tests_ipc_app_group PRIVATE xrt-interfaces)
target_include_directories(tests_ipc_app_group PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/ipc)

# ADR-051 -- the display status snapshot: §4 warning rules, §3 JSON shape, §7
# text formatter (util/u_status_snapshot.h). Pure; synthetic snapshots only.
target_link_libraries(tests_status_snapshot PRIVATE xrt-interfaces)

# ADR-051 D3 (phase 2) -- the snapshot crosses the DIAG IPC path as head + one
# screen row per system_get_status_snapshot reply + one client row per
# system_get_client_segments reply; this round-trips a synthetic snapshot
# through the generated message structs where the protocol is generated.
target_link_libraries(tests_status_ipc PRIVATE xrt-interfaces)
if(TARGET ipc_shared)
	target_link_libraries(tests_status_ipc PRIVATE ipc_shared)
	target_compile_definitions(tests_status_ipc PRIVATE DXR_TESTS_HAVE_IPC_PROTOCOL)
endif()

# Portable EDID parser (physical mm) shared by the Windows, Linux + macOS readers.
target_link_libraries(tests_aux_display_edid_parse PRIVATE aux_os xrt-interfaces)

# ADR-054 §4.1 / N6 Part 1 (#2144): the trunk's eye-pair ring sampled at a
# target time (exact / interpolate / bounded extrapolate / stale).
target_link_libraries(tests_u_eye_history PRIVATE aux_os xrt-interfaces)
# ... and the target-time slot on all five DP vtables: appended last, gated,
# falling back to the vendor horizon.
target_link_libraries(tests_dp_eye_positions_at PRIVATE xrt-interfaces)

if(TARGET tests_sim_display_monitor_geometry)
	# The pure pick is compiled straight in; the fixture path uses aux_os's
	# DRM sysfs reader + RandR <-> DRM join, as the plug-in does.
	target_sources(
		tests_sim_display_monitor_geometry
		PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_display/sim_display_monitor_geometry.c
		)
	target_include_directories(
		tests_sim_display_monitor_geometry PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_display
		)
	target_link_libraries(tests_sim_display_monitor_geometry PRIVATE aux_os xrt-interfaces)
endif()

# #1625 -- the atlas tile ROW ORIGIN: view 0 is the top-left tile as displayed,
# on every backend. The GL-vs-everyone-else disagreement was two lines of
# integer arithmetic, so it is pinned on the host rather than on five GPUs.
target_link_libraries(tests_aux_tiling_row_origin PRIVATE xrt-interfaces)

target_link_libraries(tests_cxx_wrappers PRIVATE xrt-interfaces)
# part of #1633 -- u_grip_surface(), the one grip -> grip_surface transform.
# Pure geometry, so it needs the math library and nothing else.
target_link_libraries(tests_aux_grip_surface PRIVATE aux_math)

target_link_libraries(tests_history_buf PRIVATE aux_math)
target_link_libraries(tests_lowpass_float PRIVATE aux_math)
target_link_libraries(tests_lowpass_integer PRIVATE aux_math)
target_link_libraries(tests_quatexpmap PRIVATE aux_math)
target_link_libraries(tests_rational PRIVATE aux_math)
target_link_libraries(tests_relation_chain PRIVATE aux_math)
target_link_libraries(tests_pose PRIVATE aux_math)
# XR_DXR_view_rig spec 4 (#2189) -- the rig point mapping hand joints use IS the
# shared core's eye mapping: bit-for-bit against displayxr-common across a sweep,
# plus the issue's joint cases.
target_link_libraries(tests_view_rig_point PRIVATE aux_math)
target_link_libraries(tests_quat_change_of_basis PRIVATE aux_math)
target_link_libraries(tests_quat_swing_twist PRIVATE aux_math)
target_link_libraries(tests_vec3_angle PRIVATE aux_math)
target_link_libraries(tests_space_overseer_rig PRIVATE aux_math)

# #1580 -- the two infinite-far/reversed-depth projection builders. Pure
# aux_math; no device, no compositor.
target_link_libraries(tests_m_matrix_projection PRIVATE aux_math)

# #1580 -- the shared per-view camera resolver every backend's quad/cylinder/
# equirect/cube path consumes. Compiled straight into the unit (like
# tests_rig_composer) rather than linked from comp_util, which is Vulkan-gated
# and would drag a whole compositor into a test that needs no device at all.
# aux_math carries displayxr::math_xrt, i.e. dxr_view_math.h; the auxiliary dir
# is on the include path for math/m_api.h (the #1594 head -> root lift).
target_sources(
	tests_comp_layer_view_camera
	PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor/util/comp_layer_view_camera.c
	)
target_include_directories(
	tests_comp_layer_view_camera PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor
					     ${PROJECT_SOURCE_DIR}/src/xrt/auxiliary
	)
target_link_libraries(tests_comp_layer_view_camera PRIVATE xrt-interfaces aux_math)
# #1581 -- the "no backend gates its draw on the return value" guard reads the
# backend sources as text (no device to render with), so it needs the compositor
# root as a path. PROJECT_SOURCE_DIR, not CMAKE_SOURCE_DIR: the vendor plug-in's
# signing recipe builds this runtime as a sub-project.
target_compile_definitions(
	tests_comp_layer_view_camera
	PRIVATE "DXR_COMP_SRC_DIR=\"${PROJECT_SOURCE_DIR}/src/xrt/compositor\""
	)

# #1589/#1610 -- the colour-policy guard reads the same backend sources as text
# (the encode is done by a hardware _SRGB render target, and there is no device
# here), so it needs the same compositor root. Registered unconditionally: it
# compiles no D3D header and links nothing, and a non-Windows CI run is exactly
# where a D3D backend quietly growing a local colour rule should still be
# caught. PROJECT_SOURCE_DIR, not CMAKE_SOURCE_DIR -- see above.
target_compile_definitions(
	tests_comp_color_policy PRIVATE "DXR_COMP_SRC_DIR=\"${PROJECT_SOURCE_DIR}/src/xrt/compositor\""
	)

if(TARGET tests_target_screens)
	target_link_libraries(tests_target_screens PRIVATE target_lists xrt-interfaces aux_os)
endif()
if(TARGET tests_target_display_hotplug)
	target_link_libraries(tests_target_display_hotplug PRIVATE target_lists xrt-interfaces aux_os)
endif()

if(TARGET tests_input_host_geometry)
	target_link_libraries(tests_input_host_geometry PRIVATE target_lists xrt-interfaces)
endif()
if(TARGET tests_conversion_loader)
	target_link_libraries(tests_conversion_loader PRIVATE target_lists xrt-interfaces aux_util)
	# Three stub binaries the loader is pointed at by path (never registered):
	# a well-formed provider, the same provider built against the next ABI
	# major, and a display-processor-shaped DLL with no conversion entry point.
	# ADR-054 N4 adds the display and input shapes and their ABI-mismatch
	# twins (`*_dp_abi`, `*_input_abi`): CI's per-class conformance step
	# registers those under HKLM for one self-test run to prove every class
	# loader rejects a foreign ABI major and falls through (ADR-020 rule 3).
	foreach(_stub ok abi dp dp_abi input input_abi)
		add_library(tests_conversion_loader_stub_${_stub} MODULE tests_conversion_loader_stub.c)
		target_link_libraries(tests_conversion_loader_stub_${_stub} PRIVATE xrt-interfaces)
		set_target_properties(tests_conversion_loader_stub_${_stub} PROPERTIES C_VISIBILITY_PRESET hidden PREFIX "")
		add_dependencies(tests_conversion_loader tests_conversion_loader_stub_${_stub})
	endforeach()
	target_compile_definitions(tests_conversion_loader_stub_ok PRIVATE STUB_KIND_CONVERSION)
	target_compile_definitions(
		tests_conversion_loader_stub_abi PRIVATE STUB_KIND_CONVERSION
							 "STUB_PLUGIN_API=(XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT + 1)"
		)
	target_compile_definitions(tests_conversion_loader_stub_dp PRIVATE STUB_KIND_DISPLAY)
	target_compile_definitions(
		tests_conversion_loader_stub_dp_abi PRIVATE STUB_KIND_DISPLAY
							    "STUB_PLUGIN_API=(XRT_PLUGIN_API_VERSION_CURRENT + 1)"
		)
	target_compile_definitions(tests_conversion_loader_stub_input PRIVATE STUB_KIND_INPUT)
	target_compile_definitions(
		tests_conversion_loader_stub_input_abi PRIVATE STUB_KIND_INPUT
							       "STUB_PLUGIN_API=(XRT_INPUT_PLUGIN_API_VERSION_CURRENT + 1)"
		)
	target_compile_definitions(
		tests_conversion_loader
		PRIVATE "CONV_STUB_OK_PATH=\"$<TARGET_FILE:tests_conversion_loader_stub_ok>\""
			"CONV_STUB_ABI_PATH=\"$<TARGET_FILE:tests_conversion_loader_stub_abi>\""
			"CONV_STUB_DP_PATH=\"$<TARGET_FILE:tests_conversion_loader_stub_dp>\""
		)
endif()
if(TARGET tests_component_settings)
	target_link_libraries(tests_component_settings PRIVATE target_lists xrt-interfaces aux_util)
endif()
if(TARGET tests_input_emulated_controllers)
	target_link_libraries(tests_input_emulated_controllers PRIVATE target_lists xrt-interfaces aux_util)
endif()
if(TARGET tests_input_selection_policy)
	target_link_libraries(tests_input_selection_policy PRIVATE target_lists xrt-interfaces aux_util)
endif()
if(TARGET tests_component_loader)
	target_link_libraries(tests_component_loader PRIVATE target_lists xrt-interfaces aux_util)
	# Reuses the conversion loader's stub binaries: a conversion-shaped module
	# and a display-processor-shaped one, so the shared load path can be shown
	# to resolve each class's entry point and refuse the other's.
	if(TARGET tests_conversion_loader_stub_ok)
		add_dependencies(tests_component_loader tests_conversion_loader_stub_ok tests_conversion_loader_stub_dp)
		target_compile_definitions(
			tests_component_loader
			PRIVATE "CONV_STUB_OK_PATH=\"$<TARGET_FILE:tests_conversion_loader_stub_ok>\""
				"CONV_STUB_DP_PATH=\"$<TARGET_FILE:tests_conversion_loader_stub_dp>\""
			)
	endif()
endif()
if(TARGET tests_conv_host_satellite)
	target_link_libraries(tests_conv_host_satellite PRIVATE conv_host_core xrt-interfaces aux_util)
	add_dependencies(tests_conv_host_satellite displayxr-conversion-host drv_sim_conversion_plugin
	                 tests_conversion_loader_stub_abi)
	target_compile_definitions(
		tests_conv_host_satellite
		PRIVATE "CONV_HOST_EXE=\"$<TARGET_FILE:displayxr-conversion-host>\""
			"SIM_CONVERSION_PATH=\"$<TARGET_FILE:drv_sim_conversion_plugin>\""
			"CONV_STUB_ABI_PATH=\"$<TARGET_FILE:tests_conversion_loader_stub_abi>\""
		)
	set_target_properties(tests_conv_host_satellite PROPERTIES CXX_STANDARD 17)
	if(WIN32)
		# The D3D11 NT-handle lane case (WARP, debug layer when installed).
		target_link_libraries(tests_conv_host_satellite PRIVATE d3d11 dxgi)
	endif()
endif()
if(TARGET tests_conv_snapshot)
	target_link_libraries(
		tests_conv_snapshot PRIVATE target_lists displayxr::conversion_core xrt-interfaces aux_util
					    xrt-external-cjson
		)
	# ADR-053 P4 (#2108): the model-store pack rows (std::filesystem temp store).
	target_link_libraries(tests_conv_snapshot PRIVATE displayxr::conversion_store)
	set_target_properties(tests_conv_snapshot PROPERTIES CXX_STANDARD 17)
	# The selftest verdict (cli_query_apply_conversion, exit 13) where the CLI's
	# query core is built.
	if(TARGET cli_query)
		# cli_query.c references xrt_instance_create: link the no-compositor instance too.
		target_link_libraries(tests_conv_snapshot PRIVATE cli_query target_instance_no_comp)
		target_compile_definitions(tests_conv_snapshot PRIVATE TESTS_HAVE_CLI_QUERY)
	endif()
	# Three stub providers handed to the headless pass by path (never registered).
	foreach(_stub good malformed bad_state)
		add_library(tests_conv_snapshot_stub_${_stub} MODULE tests_conv_snapshot_stub.c)
		target_link_libraries(tests_conv_snapshot_stub_${_stub} PRIVATE xrt-interfaces)
		set_target_properties(tests_conv_snapshot_stub_${_stub} PROPERTIES C_VISIBILITY_PRESET hidden PREFIX "")
		add_dependencies(tests_conv_snapshot tests_conv_snapshot_stub_${_stub})
	endforeach()
	target_compile_definitions(tests_conv_snapshot_stub_good PRIVATE STUB_VARIANT_GOOD)
	target_compile_definitions(tests_conv_snapshot_stub_malformed PRIVATE STUB_VARIANT_MALFORMED)
	target_compile_definitions(tests_conv_snapshot_stub_bad_state PRIVATE STUB_VARIANT_BAD_STATE)
	# The shipped sim provider through the same pass: what CI's selftest gate sees
	# (#2002). Linked in: its DLL needs the runtime DLLs next to it.
	if(TARGET sim_conversion_static)
		target_link_libraries(tests_conv_snapshot PRIVATE sim_conversion_static)
		target_compile_definitions(tests_conv_snapshot PRIVATE CONV_SNAP_SIM_STATIC)
	endif()
	target_compile_definitions(
		tests_conv_snapshot
		PRIVATE "CONV_SNAP_STUB_GOOD=\"$<TARGET_FILE:tests_conv_snapshot_stub_good>\""
			"CONV_SNAP_STUB_MALFORMED=\"$<TARGET_FILE:tests_conv_snapshot_stub_malformed>\""
			"CONV_SNAP_STUB_BAD_STATE=\"$<TARGET_FILE:tests_conv_snapshot_stub_bad_state>\""
		)
endif()
if(TARGET tests_target_backcompat_claim)
	target_link_libraries(tests_target_backcompat_claim PRIVATE target_lists xrt-interfaces aux_os)
endif()

# #1380 rig composer -- the head device's pose source. Compiled straight into
# the unit (like tests_comp_rear_budget) rather than linked from target_lists,
# which would drag the whole prober/plug-in/builder stack into a test that needs
# no plug-in, no display and no provider. The one driver symbol it calls,
# qwerty_set_hmd_pose, is stubbed in the test file: that stub IS the fake qwerty.
# PROJECT_SOURCE_DIR, not CMAKE_SOURCE_DIR -- the vendor plug-in's signing recipe
# builds this runtime as a sub-project and a target_sources() path resolved
# against the wrong root is a hard configure error.
target_sources(
	tests_rig_composer PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/targets/common/target_rig_composer.c
	)
target_include_directories(
	tests_rig_composer PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/targets/common
			   ${PROJECT_SOURCE_DIR}/src/xrt/drivers
	)
target_link_libraries(tests_rig_composer PRIVATE xrt-interfaces aux_math aux_os)

if(TARGET tests_oxr_view_space)
	target_include_directories(
		tests_oxr_view_space PRIVATE ${PROJECT_SOURCE_DIR}/src/external/openxr_includes
		)
	target_compile_definitions(
		tests_oxr_view_space PRIVATE "DXR_RUNTIME_LIB_PATH=\"$<TARGET_FILE:${RUNTIME_TARGET}>\""
		)
	add_dependencies(tests_oxr_view_space ${RUNTIME_TARGET})
	if(NOT WIN32)
		target_link_libraries(tests_oxr_view_space PRIVATE ${CMAKE_DL_LIBS})
	endif()

	# #1631 — whether this build HAS a tracked role device to find. The
	# sim-display builder always adds the qwerty controllers, and those are the
	# only role devices a hardware-free headless session gets; without that
	# driver the system genuinely does not track and XR_FALSE is correct, so the
	# arm must know which build it is in rather than guess from the answer.
	target_compile_definitions(
		tests_oxr_view_space PRIVATE "DXR_TEST_HAVE_QWERTY=$<BOOL:${XRT_BUILD_DRIVER_QWERTY}>"
		)

	# #1486 -- the DXR_VIEW_CONFIG_LEGACY kill switch, in a SECOND ctest running
	# the SAME binary. It has to be its own process: the runtime reads the
	# variable through DEBUG_GET_ONCE_BOOL_OPTION, which caches the first read
	# for the life of the process, so one process cannot host both the new and
	# the legacy view-configuration mapping. The tag filter keeps this run to the
	# legacy case, and the case itself returns early (a SUCCEED, deliberately
	# not a Catch2 SKIP - build-windows.yml reads any "SKIPPED:" from this
	# binary as "the headless runtime did not come up") unless the variable is
	# set, so the plain registration above, which runs every case, pins nothing
	# there.
	add_test(NAME tests_oxr_view_space_legacy
		 COMMAND tests_oxr_view_space "[view_config_legacy]" --success --allow-running-no-tests
		)
	set_tests_properties(
		tests_oxr_view_space_legacy PROPERTIES ENVIRONMENT "DXR_VIEW_CONFIG_LEGACY=1"
		)

	# #1499 -- the mode floor, in two MORE processes of the same binary, for the
	# same reason as the legacy run above: which mode sim-display comes up in is
	# decided by SIM_DISPLAY_OUTPUT when the plug-in is loaded, and
	# SIM_DISPLAY_FORCE_MODE is read through DEBUG_GET_ONCE_NUM_OPTION, so one
	# process cannot host both the floored and the pinned case. Both arms
	# SUCCEED with a WARN (never a Catch2 SKIP -- build-windows.yml reads any
	# "SKIPPED:" from this binary as "the headless runtime did not come up")
	# when their variable is absent, so the plain registration above pins
	# nothing here and stays green on a box with no >2-view mode.
	add_test(NAME tests_oxr_view_space_mode_floor
		 COMMAND tests_oxr_view_space "[mode_floor]" --success --allow-running-no-tests
		)
	set_tests_properties(
		tests_oxr_view_space_mode_floor PROPERTIES ENVIRONMENT "SIM_DISPLAY_OUTPUT=quad"
		)
	add_test(NAME tests_oxr_view_space_mode_pinned
		 COMMAND tests_oxr_view_space "[mode_pinned]" --success --allow-running-no-tests
		)
	set_tests_properties(
		tests_oxr_view_space_mode_pinned PROPERTIES ENVIRONMENT "SIM_DISPLAY_FORCE_MODE=4"
		)

	# #1499 kill switch, in a process of its OWN for the third time: the runtime
	# reads DXR_MODE_FLOOR through DEBUG_GET_ONCE_BOOL_OPTION, so a process that
	# has already answered "on" cannot also answer "off". This is what makes the
	# switch mean something -- the other arms only prove that the TEST reads the
	# variable, not that the runtime does.
	# #1631 — the CTS's own input configuration, in a process of its OWN for the
	# same one-shot reason as the arms above: DXR_INPUT_PROVIDERS is read through
	# CRT getenv at the FIRST instance create and the provider scan is latched
	# (`g_input_load_attempted`), so one process cannot host both "a provider is
	# loaded" and "none is". scripts/run_cts.ps1 always sets it to 0 so the hands
	# stay on qwerty, and that is precisely the configuration in which the
	# dynamic-role CACHE (empty until xrSyncActions) used to make
	# xrGetSystemProperties answer XR_FALSE.
	# #2189 — rig-scoped hand joints need a hand-tracking provider resident: the
	# sim-input plug-in through a generated manifest, in its own process (the
	# provider scan is one-shot per process).
	if(TARGET drv_sim_input_plugin)
		# Per-config directory: the manifest names the plug-in binary through
		# $<TARGET_FILE>, which differs per configuration on a multi-config
		# generator (Visual Studio), and file(GENERATE) must not write one path
		# with two contents. The environment rides in the COMMAND (cmake -E env),
		# where generator expressions are evaluated per configuration too.
		set(_rig_hands_dir "${CMAKE_CURRENT_BINARY_DIR}/rig_hands_plugins/$<CONFIG>")
		file(GENERATE
		     OUTPUT "${_rig_hands_dir}/200-sim-input-input-provider.json"
		     CONTENT "{\n  \"file_format_version\": \"1.0\",\n  \"plugin\": {\n    \"id\": \"sim-input\",\n    \"display_name\": \"DisplayXR Sim Input\",\n    \"binary_path\": \"$<TARGET_FILE:drv_sim_input_plugin>\",\n    \"probe_order\": 200\n  }\n}\n"
		     )
		add_dependencies(tests_oxr_view_space drv_sim_input_plugin)
		# POSIX discovery reads XRT_PLUGIN_SEARCH_PATH; Windows discovery is
		# registry-only, so there the arm finds no provider and SUCCEEDs with a WARN.
		add_test(NAME tests_oxr_view_space_rig_hands
		         COMMAND ${CMAKE_COMMAND} -E env "XRT_PLUGIN_SEARCH_PATH=${_rig_hands_dir}"
		                 DXR_INPUT_SIM_INPUT_ENABLE=1 $<TARGET_FILE:tests_oxr_view_space> "[rig_hands]" --success
		                 --allow-running-no-tests
		         )
		# #2191 — HANDS=right PINCH=hold: one tracked hand holding one pinch.
		add_test(NAME tests_oxr_view_space_rig_hands_hold
		         COMMAND ${CMAKE_COMMAND} -E env "XRT_PLUGIN_SEARCH_PATH=${_rig_hands_dir}"
		                 DXR_INPUT_SIM_INPUT_ENABLE=1 DXR_INPUT_SIM_INPUT_HANDS=right
		                 DXR_INPUT_SIM_INPUT_PINCH=hold $<TARGET_FILE:tests_oxr_view_space> "[rig_hands_hold]"
		                 --success --allow-running-no-tests
		         )
	endif()
	add_test(NAME tests_oxr_view_space_no_input_providers
		 COMMAND tests_oxr_view_space "[tracking_props_noproviders]" --success --allow-running-no-tests
		)
	set_tests_properties(
		tests_oxr_view_space_no_input_providers PROPERTIES ENVIRONMENT "DXR_INPUT_PROVIDERS=0"
		)

	add_test(NAME tests_oxr_view_space_mode_floor_off
		 COMMAND tests_oxr_view_space "[mode_floor_off]" --success --allow-running-no-tests
		)
	set_tests_properties(
		tests_oxr_view_space_mode_floor_off
		PROPERTIES ENVIRONMENT "SIM_DISPLAY_OUTPUT=quad;DXR_MODE_FLOOR=0"
		)
endif()

# #1380 — the sim_input scripted NAVIGATION device: the hardware-free reference
# rig driver. Compiled straight into the unit (like tests_rig_composer above)
# rather than loaded as the plug-in DLL, because what is under test is the
# SCRIPT — pure functions of the requested timestamp — and not discovery: a
# loader-driven test could not pin determinism without a registry write.
# PROJECT_SOURCE_DIR, not CMAKE_SOURCE_DIR: the vendor plug-in's signing recipe
# builds this runtime as a SUB-PROJECT and a target_sources() path resolved
# against the wrong root is a hard configure error.
target_sources(
	tests_sim_input_nav PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_input/sim_input_nav.c
	)
target_include_directories(
	tests_sim_input_nav PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_input
			    ${PROJECT_SOURCE_DIR}/src/xrt/auxiliary
	)
target_link_libraries(tests_sim_input_nav PRIVATE xrt-interfaces aux_os)

# #2191 — the sim_input HAND script: placement relative to the display plane,
# HANDS / PINCH / HAND_DISTANCE_M, and the pinch-hold tip gap through the real
# u_hand_simulation. Pure functions, compiled straight in like the nav unit.
# The end-to-end placement check is tests_oxr_view_space_rig_hands.
target_sources(
	tests_sim_input_hands PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_input/sim_input_hand_script.c
	)
target_include_directories(
	tests_sim_input_hands PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_input
			      ${PROJECT_SOURCE_DIR}/src/xrt/auxiliary
	)
target_link_libraries(tests_sim_input_hands PRIVATE xrt-interfaces aux_math)

# The gamepad input provider's mapping table (drivers/gamepad/gamepad_map.c):
# stick -> rig velocity with deadzone, the integrator, the button actions.
# Pure functions with no OS or device dependency, compiled straight into the
# unit like tests_sim_input_nav — what is under test is the POLICY the
# provider's devices trust, and it needs no pad.
target_sources(
	tests_gamepad_map PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/drivers/gamepad/gamepad_map.c
	)
target_include_directories(
	tests_gamepad_map PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/drivers/gamepad
			  ${PROJECT_SOURCE_DIR}/src/xrt/auxiliary
	)
target_link_libraries(tests_gamepad_map PRIVATE xrt-interfaces)
# #132 — the touch provider's gesture recogniser + rig/aim math, and the
# pointer-event ring it is fed from. Compiled straight in for the same reason
# as tests_sim_input_nav: what is under test is the mapping table (pure
# functions of the events and timestamps fed in), not discovery.
target_sources(
	tests_touch_gesture PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/drivers/touch_input/touch_gesture.c
	)
target_include_directories(
	tests_touch_gesture PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/drivers/touch_input
			     ${PROJECT_SOURCE_DIR}/src/xrt/auxiliary
	)
target_link_libraries(tests_touch_gesture PRIVATE xrt-interfaces aux_util aux_os)

# #1380 — the rig (navigation) role in the input arbiter. The arbiter is pure
# decision logic over a candidate table, so the unit compiles it straight in
# (like tests_comp_rear_budget does) rather than linking the target_lists
# library, which would drag the whole plug-in loader and its registry probing
# into a test that needs no plug-in, no device and no display.
# PROJECT_SOURCE_DIR, not CMAKE_SOURCE_DIR: the vendor plug-in's signing recipe
# builds this runtime as a SUB-PROJECT, and a target_sources() path resolved
# against the wrong root is a hard configure error.
target_sources(
	tests_input_arbiter_rig PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/targets/common/target_input_arbiter.c
	)
target_include_directories(
	tests_input_arbiter_rig PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/targets/common
	)
target_link_libraries(tests_input_arbiter_rig PRIVATE xrt-interfaces aux_os)

# ADR-054 N3d (#2051) — the arbiter's live re-rank: a PreferredPlugin flip and
# an Enabled = 0 withdrawal move the roles at once and bump generation_id.
# Same shape as tests_input_arbiter_rig (the arbiter compiled straight in).
target_sources(
	tests_input_arbiter_rerank PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/targets/common/target_input_arbiter.c
	)
target_include_directories(
	tests_input_arbiter_rerank PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/targets/common
	)
target_link_libraries(tests_input_arbiter_rerank PRIVATE xrt-interfaces aux_os)

# ADR-054 N7 (#2143) -- the optional hand-role floor: no qwerty pair means the
# hands fall to -1 (a lone provider is still arbitrated), and a live
# EmulatedControllers flip withdraws / re-admits an existing pair. Same shape.
target_sources(
	tests_input_arbiter_floor PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/targets/common/target_input_arbiter.c
	)
target_include_directories(
	tests_input_arbiter_floor PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/targets/common
	)
target_link_libraries(tests_input_arbiter_floor PRIVATE xrt-interfaces aux_os)

# ADR-034 Amendment 5 (#2181) — per-client role resolution: the overlay walk
# (pins only where the provider claims the family, presence-gated), per-client
# monotonic generations across a global re-rank, persist, Lock, enumerate,
# holders. Same shape as tests_input_arbiter_floor (the arbiter compiled in;
# the policy store faked through t_input_arbiter_set_policy).
target_sources(
	tests_input_arbiter_per_client PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/targets/common/target_input_arbiter.c
	)
target_include_directories(
	tests_input_arbiter_per_client PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/targets/common
	)
target_link_libraries(tests_input_arbiter_per_client PRIVATE xrt-interfaces aux_os)

# #1427 — the XR_DXR_weave handle-export latch rule. A pure boolean header with
# no runtime dependency (see tests_oxr_weave_latch.cpp for why the entry point
# itself is not host-drivable), so it needs the st_oxr include path and nothing
# else.
target_include_directories(
	tests_oxr_weave_latch PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/state_trackers/oxr
	)

# XR_DXR_mcp_tools result shaping (an XR_FALSE submit keeps the app's message as
# an isError result). Pure cJSON -- compiled straight in; aux_util brings cJSON.
target_sources(
	tests_oxr_mcp_app_result PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/state_trackers/oxr/oxr_mcp_app_result.c
	)
target_include_directories(
	tests_oxr_mcp_app_result PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/state_trackers/oxr
	)

# #1486 — the two xrEndFrame projection-layer viewCount rules (tight
# PRIMARY_STEREO vs the permissive body PRIMARY_MULTIVIEW_DXR and the
# DXR_VIEW_CONFIG_LEGACY rollback share). Pure integer decisions in a header, for
# the same reason as the weave latch above: the entry point needs a submitted
# frame, so only the rule is host-drivable.
target_include_directories(
	tests_oxr_view_config_rule PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/state_trackers/oxr
	)

# #1510 — the legacy mode floor and the compromise view scale it implies. Same
# reason as the two above: the real decision needs an instance, a system, a head
# device and a plug-in, so only the rule is host-drivable. It reads
# xrt_rendering_mode, hence xrt-interfaces (headers only).
target_include_directories(
	tests_oxr_legacy_mode_rule PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/state_trackers/oxr
	)
target_link_libraries(tests_oxr_legacy_mode_rule PRIVATE xrt-interfaces)

# #1499 — the GENERAL mode floor the legacy one above is now a wrapper over:
# "a session never runs in a mode it cannot fill", parameterised on the view
# count of the view configuration it began. Same header, same reason it is only
# the RULE that is host-drivable (the wiring needs an instance, a system, a head
# device and a plug-in), so the same include + xrt-interfaces and nothing else.
target_include_directories(
	tests_oxr_mode_fillable_rule PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/state_trackers/oxr
	)
target_link_libraries(tests_oxr_mode_fillable_rule PRIVATE xrt-interfaces)

# #1631 — what XrSystemTrackingProperties advertises on a 3D display: the head's
# own flags OR the display processor's eye-tracking capability OR any tracked
# role device. Same reason as the three rules above that it is only the RULE
# which is host-drivable (the real answer needs an instance, a system, a head, a
# plug-in and role devices). Pure bools/uint32 over <stdbool.h>, so unlike those
# it needs no xrt-interfaces either -- the include is the whole dependency.
target_include_directories(
	tests_oxr_tracking_properties_rule PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/state_trackers/oxr
	)

# ADR-034 Amendment 5 (#2181) — XR_DXR_input_providers event discipline: the
# setter's "should I queue the event" rule, header-only like the rules above.
target_include_directories(
	tests_oxr_input_providers_rule PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/state_trackers/oxr
	)
target_link_libraries(tests_oxr_input_providers_rule PRIVATE xrt-interfaces)

# One source of truth for the ACTIVE mode's per-view scale. The accessor is a
# static inline over xrt_device / xrt_rendering_mode, so headers are all it
# needs -- but it is the thing three hand-maintained overwrite sites used to do
# by hand, which is exactly why it is worth pinning.
target_link_libraries(tests_xrt_active_mode_scale PRIVATE xrt-interfaces)

# #1201 — the self-test rule that makes `display_dims` prove its number instead
# of printing it. A header of pure integer decisions (the probe that feeds it is
# Windows-only, the rule is not), so there is nothing to link but the include.
target_include_directories(tests_cli_dims_check PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/targets/cli)

# Display dashboard phase 0 (ADR-051): the non-failing selftest rule "every
# VERIFIED claim has physical metres" (the 0 m screen trap). Header-only.
target_include_directories(tests_cli_metres_check PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/targets/cli)
target_link_libraries(tests_cli_metres_check PRIVATE xrt-interfaces)

# perf-settings Phase 2A: the generated Tier 1/2 lever catalog behind
# `perf list --all` (scripts/gen_lever_catalog.py). Header-only data; aux_util
# (linked by the loop) supplies u_setting_is_managed for the managed-lever check.
target_include_directories(tests_cli_lever_catalog PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/targets/cli)
target_link_libraries(tests_cli_lever_catalog PRIVATE xrt-interfaces)

# N3a (ADR-054): the `input list` prediction rule — PreferredPlugin over
# ProbeOrder, Enabled=0 never predicted — and the provider-id check behind
# `input enable|disable|use`. Header-only; the loader header it includes is
# only used for the descriptor struct.
target_include_directories(
	tests_cli_input_predict PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/targets/cli ${PROJECT_SOURCE_DIR}/src/xrt/targets/common
	)
target_link_libraries(tests_cli_input_predict PRIVATE xrt-interfaces)

# N3b (ADR-054): the `dp list` twin — PreferredPlugin over ProbeOrder among
# ENABLED plug-ins, a disabled one never predicted (the fallback takes over),
# and the registered gate behind `dp enable|disable`. Header-only.
target_include_directories(
	tests_cli_dp_predict PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/targets/cli ${PROJECT_SOURCE_DIR}/src/xrt/targets/common
	)
target_link_libraries(tests_cli_dp_predict PRIVATE xrt-interfaces)

# Android late-weave / repaint pacing (#868/#902). The vblank grid is
# header-only and deliberately free of Vulkan and DXGI types — that is what
# lets its drift / staleness / mode-switch logic be tested on the host, where
# those bugs are actually findable.
target_include_directories(tests_comp_vblank_grid PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor)
# #902 desktop Linux: window-system presentation feedback -> vblank grid.
# Header-only decisions on top of the grid, same include path, nothing to link.
target_include_directories(tests_comp_present_feedback PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor)
# #2068: self-correcting late-weave deadlines on a composited desktop. Header-only.
target_include_directories(tests_comp_late_weave_adapt PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor)
# #2068: windowed #206 residual statistics. Header-only.
target_include_directories(tests_comp_residual_window PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor)
# #2068: feedback pacing (wait / slip / backoff). Header-only.
target_include_directories(tests_comp_feedback_pace PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor)
# #2119: present_wait depth governor (lag floor + slip machine). Header-only.
target_include_directories(tests_comp_late_weave_depth PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor)
# #1748 -- the move-sync tag encode is a header-only function in vk_native.
target_include_directories(tests_comp_wl_move_sync_tag PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor)
# ADR-027 tier-1 dispatch (#1331). Header-only pure logic -- no compositor
# library, no graphics API, no device -- so it needs the include path and
# nothing else.
target_include_directories(tests_comp_zone_tier1 PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor)
target_include_directories(tests_comp_lazy_transparency PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor)

# #1504 / #1513 -- the acquire/wait/release ring the vk_native, OpenGL and Metal
# swapchains all share. Same shape as the two above: a header of pure index
# bookkeeping, so the CTS acquire sequence is reproducible with no graphics
# device and no window. Unconditional now that three backends depend on it.
target_include_directories(
	tests_comp_swapchain_ring PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor
	)
target_link_libraries(tests_comp_swapchain_ring PRIVATE xrt-interfaces)

# #1401 -- the OEM container-scaled tell. The C copy is a header of pure integer
# arithmetic (deliberately NOT gated on XRT_OS_ANDROID, so it is testable here);
# the fourth copy lives in the aux AAR and cannot be linked, so the test READS
# the Java source and evaluates its expression. PROJECT_SOURCE_DIR, not
# CMAKE_SOURCE_DIR: the vendor plug-in's signing recipe builds this runtime as a
# sub-project, and a path resolved against the wrong root is a hard error there.
target_include_directories(
	tests_aux_mini_window_tell PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/auxiliary
	)
target_compile_definitions(
	tests_aux_mini_window_tell
	PRIVATE
		"DXR_MINI_WINDOW_LAYOUT_JAVA=\"${PROJECT_SOURCE_DIR}/src/xrt/auxiliary/android/src/main/java/org/freedesktop/monado/auxiliary/MiniWindowLayout.java\""
	)

# XR_DXR_depth_budget (#1364) -- the per-session runner every native compositor
# drives. Compiled straight into the unit rather than linked from comp_util,
# which is Vulkan-gated and would drag the whole render library into a test that
# needs no GPU, no window and no plug-in: a synthetic BGRA preview stands in for
# the display processor.
# PROJECT_SOURCE_DIR, not CMAKE_SOURCE_DIR: the vendor plug-in's signing recipe builds
# this runtime as a SUB-PROJECT (the plug-in repo is the top-level source dir), and a
# target_sources() path that resolves against the wrong root is a hard configure
# error (leia-plugin v2.6.16 shipped unsigned because of it). Include dirs elsewhere
# in this file tolerate the wrong root; a source file does not.
target_sources(
	tests_comp_rear_budget PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor/util/comp_rear_budget.c
	)
target_include_directories(tests_comp_rear_budget PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor)
target_link_libraries(tests_comp_rear_budget PRIVATE xrt-interfaces aux_os xrt-external-stb)

# Multi-screen M2: the segment table, the per-segment 1:1 policy and the DP
# lifecycle hysteresis. Pure math -- compiled into the unit like
# tests_comp_rear_budget instead of linking the Vulkan-gated comp_util.
target_sources(tests_comp_segments PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor/util/comp_segments.c)
target_include_directories(tests_comp_segments PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor)
target_link_libraries(tests_comp_segments PRIVATE xrt-interfaces aux_util)

# Multi-screen M3 — per-segment views: the pure geometry oxr_session_locate_views
# composes (oxr_segment_views.c) plus the shared Kooima core it feeds (aux_math
# carries displayxr-common), so two screens' frusta are asserted end to end.
target_sources(tests_oxr_segment_views PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/state_trackers/oxr/oxr_segment_views.c)
target_include_directories(tests_oxr_segment_views PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/state_trackers/oxr)
target_link_libraries(tests_oxr_segment_views PRIVATE xrt-interfaces aux_math)

# Multi-screen on the service / IPC path (ADR-047 Amendment 3): the routed-view
# mosaic placement and the change-only table publish (compositor/util), the
# client's change-only routing send (oxr), and -- where the IPC protocol is
# generated -- that the two new wire messages fit one IPC buffer.
target_sources(tests_segments_ipc PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor/util/comp_segments.c)
target_include_directories(
	tests_segments_ipc PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor
	${PROJECT_SOURCE_DIR}/src/xrt/state_trackers/oxr
	)
target_link_libraries(tests_segments_ipc PRIVATE xrt-interfaces aux_util)
if(TARGET ipc_shared)
	target_link_libraries(tests_segments_ipc PRIVATE ipc_shared)
	target_compile_definitions(tests_segments_ipc PRIVATE DXR_TESTS_HAVE_IPC_PROTOCOL)
endif()

# ADR-052 -- the workspace on N screens: which screens present, the anchor-screen
# placement mapping, the window -> presenter split and the workspace-pixel
# cursor space. Pure math, compiled into the unit (comp_util is Vulkan-gated).
# + the state tracker's per-segment geometry, so a per-screen-views table
# (ADR-052 D2) is checked against what the client frames from it.
target_sources(
	tests_comp_workspace_screens PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor/util/comp_workspace_screens.c
	${PROJECT_SOURCE_DIR}/src/xrt/compositor/util/comp_segments.c
	${PROJECT_SOURCE_DIR}/src/xrt/state_trackers/oxr/oxr_segment_views.c
	)
target_include_directories(
	tests_comp_workspace_screens PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor
	${PROJECT_SOURCE_DIR}/src/xrt/state_trackers/oxr
	)
target_link_libraries(tests_comp_workspace_screens PRIVATE xrt-interfaces aux_util)

if(TARGET ipc_shared)
	target_sources(
		tests_workspace_screens_ipc
		PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor/util/comp_workspace_screens.c
			${PROJECT_SOURCE_DIR}/src/xrt/compositor/util/comp_segments.c
		)
	target_include_directories(
		tests_workspace_screens_ipc PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor
		${PROJECT_SOURCE_DIR}/src/xrt/state_trackers/oxr
		)
	target_link_libraries(tests_workspace_screens_ipc PRIVATE xrt-interfaces aux_util ipc_shared xrt-external-openxr)
endif()

# XR_DXR_weave v19 (#1884, ADR-047 Amendment 4): a present-owner's weave rects
# split per screen -- the segment table (compositor/util), the per-rect parts
# the client cuts from it (aux util, header-only) and, where the IPC protocol
# is generated, the weave_get_segments reply carrying every screen's eyes.
# Amendment 4 addendum: which screen frames an element's views (aux util), its
# window metrics cross-checked against the per-segment views' own
# (oxr_segment_views.c), and the submit reply's cheap split signal.
target_sources(
	tests_weave_segments PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor/util/comp_segments.c
	${PROJECT_SOURCE_DIR}/src/xrt/state_trackers/oxr/oxr_segment_views.c
	)
target_include_directories(
	tests_weave_segments PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor
	${PROJECT_SOURCE_DIR}/src/xrt/state_trackers/oxr
	)
target_link_libraries(tests_weave_segments PRIVATE xrt-interfaces aux_util)
if(TARGET ipc_shared)
	target_link_libraries(tests_weave_segments PRIVATE ipc_shared)
	target_compile_definitions(tests_weave_segments PRIVATE DXR_TESTS_HAVE_IPC_PROTOCOL)
endif()

# Multi-screen M4 — DXR_SCREEN_PLUGIN (per-screen plug-in pin) and the per-monitor
# winner rule of the DP registry, pure functions of target_screen_pin.c.
target_sources(tests_target_screen_pin PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/targets/common/target_screen_pin.c)
target_include_directories(tests_target_screen_pin PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/targets/common)

if(TARGET target_workspace_config)
	target_link_libraries(tests_service_workspace_config PRIVATE target_workspace_config xrt-external-cjson)
endif()
if(TARGET tests_service_client_class)
	target_sources(
		tests_service_client_class PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/targets/service/service_client_class.c
		)
	# Compiled without the orchestrator (a service-target-only unit): the
	# manifest registry is the source of truth here.
	target_compile_definitions(tests_service_client_class PRIVATE SERVICE_CLIENT_CLASS_NO_ORCHESTRATOR)
	target_link_libraries(tests_service_client_class PRIVATE target_workspace_config xrt-interfaces)
endif()
if(TARGET tests_service_orchestrator_linux)
	target_sources(
		tests_service_orchestrator_linux
		PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/targets/service/service_orchestrator.c
		)
	target_link_libraries(
		tests_service_orchestrator_linux PRIVATE target_workspace_config xrt-interfaces aux_os xrt-pthreads
		)
	# The chord forward to a running controller, against a recording stand-in
	# of the controller-key queue header (keeps the IPC server out of the test).
	target_include_directories(
		tests_service_orchestrator_linux PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/fake_controller_key
		)
	target_sources(
		tests_service_hotkey_linux PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/targets/service/service_hotkey_linux.c
		)
	target_link_libraries(
		tests_service_hotkey_linux PRIVATE target_workspace_config xrt-interfaces aux_os xrt-pthreads
		)
endif()

if(TARGET tests_service_drm_hotplug_linux)
	target_sources(
		tests_service_drm_hotplug_linux
		PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/targets/service/service_drm_hotplug_linux.c
		)
	target_include_directories(
		tests_service_drm_hotplug_linux PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/targets/service
		)
	target_link_libraries(tests_service_drm_hotplug_linux PRIVATE xrt-interfaces aux_util xrt-pthreads)
endif()

# The hotkey worker's GNOME-extension backend against a fake WorkspaceHotkey1
# on a PRIVATE session bus (dbus-run-session; python3-gi plays the extension).
# Registered only where all three exist, so a bare build host just skips it.
if(TARGET tests_service_hotkey_linux AND XRT_HAVE_DBUS)
	find_program(DXR_DBUS_RUN_SESSION dbus-run-session)
	find_package(Python3 COMPONENTS Interpreter QUIET)
	if(DXR_DBUS_RUN_SESSION AND Python3_Interpreter_FOUND)
		execute_process(
			COMMAND ${Python3_EXECUTABLE} -c "from gi.repository import Gio"
			RESULT_VARIABLE _dxr_py_gi
			OUTPUT_QUIET ERROR_QUIET
			)
		if(_dxr_py_gi EQUAL 0)
			add_executable(
				tests_service_hotkey_linux_dbus tests_service_hotkey_linux_dbus.cpp
				${PROJECT_SOURCE_DIR}/src/xrt/targets/service/service_hotkey_linux.c
				)
			target_compile_definitions(tests_service_hotkey_linux_dbus PRIVATE SERVICE_HOTKEY_HAVE_DBUS)
			target_include_directories(tests_service_hotkey_linux_dbus SYSTEM PRIVATE ${DBUS_INCLUDE_DIRS})
			target_link_libraries(
				tests_service_hotkey_linux_dbus
				PRIVATE xrt-external-catch2
					aux_util
					target_workspace_config
					xrt-interfaces
					aux_os
					xrt-pthreads
					xrt-external-cjson
					${DBUS_LIBRARIES}
				)
			add_test(
				NAME tests_service_hotkey_linux_dbus
				COMMAND
					${DXR_DBUS_RUN_SESSION} -- ${Python3_EXECUTABLE}
					${CMAKE_CURRENT_SOURCE_DIR}/fake_workspace_hotkey_ext.py
					$<TARGET_FILE:tests_service_hotkey_linux_dbus> --success
				)
		endif()
	endif()
endif()

if(XRT_HAVE_D3D11)
	target_link_libraries(tests_aux_d3d_d3d11 PRIVATE aux_d3d)
	# ADR-037 §2 render-adapter resolver — DXGI enumeration plus a driverless
	# feature-level probe; no compositor, no window, no panel.
	target_link_libraries(tests_aux_d3d_render_adapter PRIVATE aux_d3d)
	# #918 bridge-plane transport policy — a header of pure decisions, no D3D
	# link needed (the point of extracting it was that none of these need a GPU).
	target_include_directories(
		tests_comp_xbridge_plane_policy PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor/xbridge
		)
	# #918 Phase 3 — the split's activation decision. Pure C, no device, no
	# graphics types at all (the reason it was extracted), so the unit links the
	# gate's own static lib and nothing else.
	target_include_directories(
		tests_comp_split_gate PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor/xbridge
		)
	if(TARGET comp_xbridge)
		target_link_libraries(tests_comp_split_gate PRIVATE comp_xbridge)
	endif()
	# #1414 — which DP answers a client's eye query (and therefore whether an
	# IPC rig locate reports rig_applied != 0). Pure decisions, extracted out of
	# the service unit for exactly this reason: no device, no window, no panel,
	# so the submit-less-client regression is assertable in ctest. The ipc
	# include is for IPC_VIEW_RIG_NONE alone.
	target_include_directories(
		tests_comp_eye_dp_policy
		PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor/d3d11_service
			${PROJECT_SOURCE_DIR}/src/xrt/ipc
		)
	target_link_libraries(tests_comp_eye_dp_policy PRIVATE xrt-interfaces)
	# Phase 2 — comp_d3d11_client.cpp now references the
	# comp_ipc_client_compositor_{get,set}_workspace_sync_fence{,_value}
	# bridge symbols defined in ipc_client_compositor.c. Add ipc_client to
	# the link line so the unit test binary resolves them.
	target_link_libraries(tests_comp_client_d3d11 PRIVATE comp_client comp_mock ipc_client)
endif()

if(WIN32 AND (XRT_HAVE_D3D11 OR XRT_HAVE_D3D12))
	# The shader strings are a header of embedded HLSL — no compositor library
	# to link, only the compiler/reflection the units themselves use.
	target_include_directories(
		tests_comp_masked_composite_shaders PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor
		)
	target_link_libraries(tests_comp_masked_composite_shaders PRIVATE d3dcompiler)
endif()

if(WIN32 AND XRT_HAVE_D3D11)
	# #1601 — same setup: both shader sets are headers of embedded HLSL, so
	# there is no compositor library to link, only the compiler/reflection.
	target_include_directories(
		tests_comp_layer_array_shaders PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor
		)
	target_link_libraries(tests_comp_layer_array_shaders PRIVATE d3dcompiler)

	target_include_directories(
		tests_comp_layer_array_warp PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor
		)
	target_link_libraries(tests_comp_layer_array_warp PRIVATE d3dcompiler d3d11)

	# #1602 — draws through d3d_shared/comp_equirect2_shaders.h, reached via
	# the in-process d3d11/d3d11_layer_shaders.h. Still header-only HLSL, so
	# again no compositor library to link.
	target_include_directories(
		tests_comp_layer_equirect2_warp PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor
		)
	target_link_libraries(tests_comp_layer_equirect2_warp PRIVATE d3dcompiler d3d11)

	# #1956 — header-only policy from the service directory; aux_d3d for
	# d3d_dxgi_formats.h (+ its Vulkan format constants), xrt-interfaces for the
	# swapchain usage bits.
	target_include_directories(
		tests_comp_d3d11_service_formats PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor/d3d11_service
		)
	target_link_libraries(tests_comp_d3d11_service_formats PRIVATE aux_d3d xrt-interfaces d3d11 dxgi)
	if(TARGET tests_comp_gl_d3d11_depth)
		target_include_directories(
			tests_comp_gl_d3d11_depth PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor/d3d11_service
							  ${PROJECT_SOURCE_DIR}/src/xrt/compositor
			)
		target_link_libraries(tests_comp_gl_d3d11_depth PRIVATE aux_d3d aux_ogl xrt-interfaces d3d11 dxgi)
	endif()
	if(XRT_HAVE_D3D12)
		target_compile_definitions(tests_comp_d3d11_service_formats PRIVATE DXR_TEST_HAVE_D3D12)
		target_link_libraries(tests_comp_d3d11_service_formats PRIVATE d3d12)
	endif()
endif()

if(XRT_HAVE_D3D12)
	# #573 — comp_d3d12_client.cpp now references the
	# comp_ipc_client_compositor_get_transparent_output{,_fence} bridge symbols
	# defined in ipc_client_compositor.c. Add ipc_client (same as d3d11 above).
	target_link_libraries(tests_comp_client_d3d12 PRIVATE comp_client comp_mock ipc_client)
	target_link_libraries(tests_comp_d3d12_swapchain_format PRIVATE aux_d3d d3d12)
endif()

if(XRT_HAVE_VULKAN)
	# Phase 2 — comp_vk_client.c references the
	# comp_ipc_client_compositor_{get,set}_workspace_sync_fence{,_value}
	# bridge symbols defined in ipc_client_compositor.c (same setup as
	# tests_comp_client_d3d11 above).
	target_link_libraries(
		tests_comp_client_vulkan PRIVATE comp_client comp_mock comp_util aux_vk ipc_client
		)
	target_link_libraries(tests_uv_to_tangent PRIVATE comp_render)
endif()
if(TARGET tests_vk_dmabuf)
	target_link_libraries(tests_vk_dmabuf PRIVATE comp_util aux_vk)
endif()
if(TARGET tests_comp_vk_equirect2_draw)
	# The generated SPIR-V headers (shaders/equirect2.*.h) live in
	# comp_vk_native's binary dir; the packer header in its source dir.
	get_target_property(_vk_native_bin comp_vk_native BINARY_DIR)
	target_include_directories(
		tests_comp_vk_equirect2_draw PRIVATE ${_vk_native_bin} ${PROJECT_SOURCE_DIR}/src/xrt/compositor
		)
	add_dependencies(tests_comp_vk_equirect2_draw comp_vk_native)
	target_link_libraries(tests_comp_vk_equirect2_draw PRIVATE xrt-interfaces Vulkan::Vulkan)
endif()

if(XRT_FEATURE_OPENXR)
	target_link_libraries(
		tests_input_transform PRIVATE st_oxr xrt-interfaces xrt-external-openxr
		)
endif()
if(_have_opengl_test)
	# #573 — on Windows comp_gl_win32_client.c references the
	# comp_ipc_client_compositor_get_transparent_output{,_fence} bridge symbols
	# defined in ipc_client_compositor.c. Add ipc_client (same as d3d11 above).
	target_link_libraries(
		tests_comp_client_opengl PRIVATE comp_client comp_mock aux_ogl SDL2::SDL2
		)
	if(WIN32)
		target_link_libraries(tests_comp_client_opengl PRIVATE ipc_client)
	endif()
	target_include_directories(tests_comp_client_opengl PRIVATE SDL2::SDL2)
endif()

if(XRT_HAVE_VULKAN AND XRT_HAVE_D3D11)
	target_link_libraries(tests_aux_d3d_d3d11 PRIVATE comp_util aux_vk)
endif()

# #1699 — the IPC protocol generator. weave_submit_dmabuf is the first call with
# BOTH in_handles and out_handles; this pins the optional per-handle "name" key
# that keeps the two parameter sets of the client proxy from colliding, and the
# shape of the generated client/server code for that call. Pure Python, no build
# dependency.
add_test(NAME tests_ipc_proto COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests_ipc_proto.py
					 ${PROJECT_SOURCE_DIR}/src/xrt/ipc/shared
	)

# ADR-053 S5: the reference view synthesis' pure math, compiled straight into
# the unit (no graphics API), like tests_comp_rear_budget.
target_sources(tests_conv_viewsyn_math PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor/util/u_conv_viewsyn.c)
target_include_directories(tests_conv_viewsyn_math PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor)
if(TARGET tests_conv_host_lane_d3d11)
	target_link_libraries(tests_conv_host_lane_d3d11 PRIVATE comp_d3d11_conv_host_lane d3d11 dxgi)
endif()
if(TARGET tests_conv_d3d11_xproc)
	add_executable(tests_conv_d3d11_xproc_helper tests_conv_d3d11_xproc_helper.cpp)
	target_link_libraries(tests_conv_d3d11_xproc_helper PRIVATE d3d11 dxgi)
	target_link_libraries(tests_conv_d3d11_xproc PRIVATE d3d11 dxgi)
	target_compile_definitions(
		tests_conv_d3d11_xproc
		PRIVATE "CONV_D3D11_XPROC_HELPER=\"$<TARGET_FILE:tests_conv_d3d11_xproc_helper>\""
		)
	add_dependencies(tests_conv_d3d11_xproc tests_conv_d3d11_xproc_helper)
endif()
if(TARGET comp_d3d11_conv_viewsyn)
	target_link_libraries(tests_conv_viewsyn_d3d11 PRIVATE comp_d3d11_conv_viewsyn d3d11)
endif()
if(TARGET tests_conv_viewsyn_vk)
	target_link_libraries(tests_conv_viewsyn_vk PRIVATE comp_vk_conv_viewsyn comp_util aux_vk)
endif()
if(TARGET tests_conv_vk_gpu)
	target_sources(tests_conv_vk_gpu PRIVATE conv_fake_provider.c)
	target_include_directories(tests_conv_vk_gpu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
	target_link_libraries(tests_conv_vk_gpu PRIVATE comp_vk_conv comp_util aux_vk ipc_server_conv ipc_shared displayxr::conversion_runtime aux_os)
	set_target_properties(tests_conv_vk_gpu PROPERTIES CXX_STANDARD 17)
	if(APPLE)
		target_link_libraries(tests_conv_vk_gpu PRIVATE "-framework CoreFoundation" "-framework IOSurface")
		if(XRT_HAVE_METAL)
			# The macOS Metal weave backend's lifted-rect recorder, on a
			# headless Metal device (SKIPs without one).
			target_sources(
				tests_conv_vk_gpu
				PRIVATE conv_metal_weave_rig.m
					${PROJECT_SOURCE_DIR}/src/xrt/compositor/multi/comp_multi_weave_metal_lift.m
				)
			target_include_directories(tests_conv_vk_gpu PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor)
			target_compile_definitions(tests_conv_vk_gpu PRIVATE CONV_TEST_HAVE_METAL_WEAVE)
			target_link_libraries(tests_conv_vk_gpu PRIVATE "-framework Metal" "-framework Foundation")
		endif()
	endif()
endif()
if(TARGET tests_conv_gpu_dedupe)
	target_sources(tests_conv_gpu_dedupe PRIVATE conv_fake_provider.c)
	target_link_libraries(tests_conv_gpu_dedupe PRIVATE comp_d3d11_conv_content displayxr::conversion_runtime d3d11 dxgi)
endif()
if(TARGET tests_lift_dp_golden)
	# The sim_display fake lift + the synthesis it shares with the sim provider,
	# compiled straight in (the plug-in DLL is not linkable). The fake lift
	# reads its knobs through the settings shim (N3c), so that comes along;
	# unbound (no host) it answers from the env aliases, as before.
	target_sources(
		tests_lift_dp_golden
		PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_display/sim_display_lift_d3d11.cpp
			${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_display/sim_display_settings.c
			${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_conversion/sim_conv_synth.c
			${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_conversion/sim_conv_synth.cpp
			${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_conversion/sim_conv_fake_ply.c
		)
	target_include_directories(
		tests_lift_dp_golden PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_display
					     ${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_conversion
		)
	target_link_libraries(tests_lift_dp_golden PRIVATE comp_d3d11_lift aux_os d3d11 dxgi d3dcompiler)
	# #2003: the same conversions core-routed (the v3 adapter over the core).
	if(TARGET ipc_server_conv)
		target_link_libraries(tests_lift_dp_golden PRIVATE ipc_server_conv displayxr::conversion_runtime)
	endif()
	set_target_properties(tests_lift_dp_golden PROPERTIES CXX_STANDARD 17)
endif()
if(TARGET tests_ipc_conv_v4_transports)
	# As tests_lift_dp_golden: the sim_display fake lift compiled straight in, with
	# the settings shim it reads its knobs through (N3c).
	target_sources(
		tests_ipc_conv_v4_transports
		PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_display/sim_display_lift_d3d11.cpp
			${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_display/sim_display_settings.c
			${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_conversion/sim_conv_synth.c
			${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_conversion/sim_conv_synth.cpp
			${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_conversion/sim_conv_fake_ply.c
		)
	target_include_directories(
		tests_ipc_conv_v4_transports PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_display
						     ${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_conversion
		)
	target_link_libraries(
		tests_ipc_conv_v4_transports PRIVATE comp_d3d11_lift ipc_server_conv displayxr::conversion_runtime aux_os
						     d3d11 d3d12 dxgi d3dcompiler
		)
	set_target_properties(tests_ipc_conv_v4_transports PROPERTIES CXX_STANDARD 17)
endif()

if(TARGET tests_oxr_lift_startup)
	# The REAL entry points (oxr_conv.c) and client (ipc_client_lift.c), plus
	# the handle and logger code they verify with, compiled straight in; the
	# wire calls, the lift link and the session bridge under them are faked in
	# tests_oxr_lift_startup_fake.c. Deliberately NOT linked: st_oxr, ipc_client
	# and ipc_shared, whose real definitions of those seams would collide with
	# the fakes. ipc_client is a build dependency only, for its generated
	# headers.
	target_sources(
		tests_oxr_lift_startup
		PRIVATE tests_oxr_lift_startup_fake.c
			tests_oxr_lift_startup_conv_fake.c
			${PROJECT_SOURCE_DIR}/src/xrt/state_trackers/oxr/oxr_conv.c
			${PROJECT_SOURCE_DIR}/src/xrt/state_trackers/oxr/oxr_conv_gpu_d3d.c
			${PROJECT_SOURCE_DIR}/src/xrt/state_trackers/oxr/oxr_handle_base.c
			${PROJECT_SOURCE_DIR}/src/xrt/state_trackers/oxr/oxr_logger.c
			${PROJECT_SOURCE_DIR}/src/xrt/ipc/client/ipc_client_lift.c
		)
	target_include_directories(
		tests_oxr_lift_startup
		PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
			${PROJECT_SOURCE_DIR}/src/xrt/state_trackers/oxr
			$<TARGET_PROPERTY:ipc_client,INTERFACE_INCLUDE_DIRECTORIES>
		)
	target_link_libraries(tests_oxr_lift_startup PRIVATE xrt-interfaces xrt-external-openxr aux_os)
	if(NOT WIN32)
		# The POSIX CPU kind's shared sections (oxr_conv_gpu_d3d.c): no fake,
		# the real ones (they touch no IPC seam the fakes stand in for).
		target_sources(
			tests_oxr_lift_startup PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/ipc/shared/ipc_cpu_buffer.c
			)
		if(APPLE)
			target_link_libraries(tests_oxr_lift_startup PRIVATE "-framework IOSurface" "-framework CoreFoundation")
		endif()
	endif()
	if(TARGET Vulkan::Vulkan)
		# oxr_objects.h pulls in xrt_vulkan_includes.h; st_oxr gets those headers
		# through aux_vk, which this test deliberately does not link (macOS CI has
		# no system vulkan/vulkan.h, Linux does).
		target_link_libraries(tests_oxr_lift_startup PRIVATE Vulkan::Vulkan)
	endif()
	if(XRT_FEATURE_RENDERDOC)
		target_link_libraries(tests_oxr_lift_startup PRIVATE xrt-external-renderdoc)
	endif()
	add_dependencies(tests_oxr_lift_startup ipc_client ipc_shared)
endif()

if(TARGET tests_conv_dp_lift)
	target_sources(tests_conv_dp_lift PRIVATE conv_fake_provider.c)
	target_include_directories(tests_conv_dp_lift PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
	target_link_libraries(tests_conv_dp_lift PRIVATE ipc_server_conv displayxr::conversion_runtime aux_os)
	set_target_properties(tests_conv_dp_lift PROPERTIES CXX_STANDARD 17)
endif()
foreach(_t tests_ipc_conv_v4 tests_lift_v3_shifts)
	if(TARGET ${_t})
		target_sources(${_t} PRIVATE conv_fake_provider.c)
		target_include_directories(${_t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
		                                          ${PROJECT_SOURCE_DIR}/src/xrt/state_trackers/oxr)
		target_link_libraries(${_t} PRIVATE ipc_server_conv displayxr::conversion_runtime aux_os xrt-external-openxr)
		set_target_properties(${_t} PROPERTIES CXX_STANDARD 17)
	endif()
endforeach()
if(TARGET tests_ipc_conv_cpu)
	target_link_libraries(tests_ipc_conv_cpu PRIVATE ipc_server_conv ipc_shared displayxr::conversion_runtime aux_os)
	set_target_properties(tests_ipc_conv_cpu PROPERTIES CXX_STANDARD 17)
endif()
if(TARGET tests_ipc_lift_core)
	target_sources(tests_ipc_lift_core PRIVATE conv_fake_provider.c)
	target_include_directories(tests_ipc_lift_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
	target_link_libraries(tests_ipc_lift_core PRIVATE ipc_server_conv displayxr::conversion_runtime aux_os)
	if(NOT WIN32)
		target_link_libraries(tests_ipc_lift_core PRIVATE ipc_shared) # the CPU I/O glue's buffers
	endif()
	set_target_properties(tests_ipc_lift_core PROPERTIES CXX_STANDARD 17)
endif()

# ADR-042: the sim_display FAKE lift's splat PLY is pinned host-side next to the
# lift mailbox tests (the module that emits it is Windows/D3D11-only).
# ADR-042: the FAKE lift's splat PLY is pinned host-side next to the lift
# mailbox tests. Shared by sim_display's deprecated fake and the
# DisplayXR-SimConversion provider (src/xrt/drivers/sim_conversion/).
target_sources(tests_lift_mailbox PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_conversion/sim_conv_fake_ply.c)
target_include_directories(tests_lift_mailbox PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_conversion)

# ADR-053 P1a: the DisplayXR-SimConversion provider, linked statically through
# its own entry point (sim_conversion_static): no dlopen, no registry, no GPU.
target_link_libraries(tests_sim_conversion PRIVATE sim_conversion_static displayxr::conversion_store)
set_target_properties(tests_sim_conversion PROPERTIES CXX_STANDARD 17)

# ADR-057 S1 + S2: the remote conversion provider (remote_conversion_static)
# against tests/conv_remote_test_server.py, a stdlib-only contract-v1 endpoint
# on 127.0.0.1 started by the test; pack url / auth, the camera-block writer and
# the off-box consent store. Without a Python 3 interpreter the endpoint cases
# SKIP.
target_link_libraries(tests_remote_conversion PRIVATE remote_conversion_static displayxr::conversion_store)
set_target_properties(tests_remote_conversion PROPERTIES CXX_STANDARD 17)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
	target_compile_definitions(
		tests_remote_conversion PRIVATE "DXR_TEST_PYTHON=\"${Python3_EXECUTABLE}\""
		                                "DXR_TEST_SERVER_PY=\"${CMAKE_CURRENT_SOURCE_DIR}/conv_remote_test_server.py\""
		)
endif()

# ADR-053 P1a: the conversion-provider ABI (xrt_conversion_plugin.h) and the
# conv_* wire family (xrt_conv_wire.h). The ABI test builds as C++17 on purpose
# (the header promises C11 + C++17); the wire test carries a C11 TU so both
# headers also compile as C with every _Static_assert.
target_link_libraries(tests_conversion_abi PRIVATE xrt-interfaces)
set_target_properties(tests_conversion_abi PROPERTIES CXX_STANDARD 17)
target_sources(tests_conv_wire PRIVATE tests_conv_wire_c.c)
target_link_libraries(tests_conv_wire PRIVATE xrt-interfaces)

# ADR-053 P1a (#2000): the conversion core's pure modules - selection,
# readiness state machine + spec-3 mapping, registry + describe cache.
foreach(_conv_test tests_conv_select tests_conv_readiness tests_conv_registry)
	target_link_libraries(${_conv_test} PRIVATE displayxr::conversion_core)
endforeach()

# ADR-053 P4 (#2108): the model store - root order, write protocol over a
# file:// pack, tamper rejection, research-only gating, the store ACL (Windows).
target_link_libraries(tests_conv_store PRIVATE displayxr::conversion_store)
set_target_properties(tests_conv_store PROPERTIES CXX_STANDARD 17)

# ADR-053 P1a (#2001): the conversion core's runtime half (hosts, lanes,
# pipeline, policy, jobs) against a scriptable CPU fake provider.
target_sources(tests_conv_lanes PRIVATE conv_fake_provider.c)
target_link_libraries(tests_conv_lanes PRIVATE displayxr::conversion_runtime)
# #2030: the still-image dedupe measured against the shipped sim provider.
if(TARGET sim_conversion_static)
	target_link_libraries(tests_conv_lanes PRIVATE sim_conversion_static)
	target_compile_definitions(tests_conv_lanes PRIVATE CONV_LANES_SIM_STATIC)
endif()

# ADR-053 P2 (#2032): the satellite supervisor FSM (pure, scripted clock) and
# the shm command / completion rings with the host wire records.
target_link_libraries(tests_conv_supervisor PRIVATE displayxr::conversion_core)
target_link_libraries(tests_conv_shm_ring PRIVATE displayxr::conversion_core)
if(TARGET tests_conv_os_process)
	add_executable(tests_conv_os_process_helper tests_conv_os_process_helper.c)
	target_link_libraries(tests_conv_os_process_helper PRIVATE aux_os displayxr::conversion_core)
	target_link_libraries(tests_conv_os_process PRIVATE displayxr::conversion_core)
	target_compile_definitions(
		tests_conv_os_process
		PRIVATE "CONV_OS_PROCESS_HELPER=\"$<TARGET_FILE:tests_conv_os_process_helper>\""
		)
	add_dependencies(tests_conv_os_process tests_conv_os_process_helper)
endif()
if(TARGET tests_conv_proxy)
	add_executable(tests_conv_proxy_host tests_conv_proxy_host.c)
	target_link_libraries(tests_conv_proxy_host PRIVATE aux_os aux_util displayxr::conversion_core)
	if(NOT WIN32)
		target_link_libraries(tests_conv_proxy_host PRIVATE xrt-pthreads)
	else()
		target_link_libraries(tests_conv_proxy_host PRIVATE user32)
	endif()
	target_link_libraries(tests_conv_proxy PRIVATE displayxr::conversion_runtime)
	target_compile_definitions(
		tests_conv_proxy PRIVATE "CONV_PROXY_HOST=\"$<TARGET_FILE:tests_conv_proxy_host>\""
		)
	add_dependencies(tests_conv_proxy tests_conv_proxy_host)
	if(WIN32)
		# #2056: the update-flow test drives a real Restart Manager session.
		target_link_libraries(tests_conv_proxy PRIVATE rstrtmgr)
	endif()
	# The real host + the sim provider, compared against the same sources in-service.
	if(TARGET displayxr-conversion-host AND TARGET drv_sim_conversion_plugin AND TARGET sim_conversion_static)
		target_link_libraries(tests_conv_proxy PRIVATE sim_conversion_static)
		if(WIN32 AND TARGET comp_d3d11_lift AND TARGET comp_d3d11_conv_host_lane)
			# The real host over the D3D11 lane on WARP (service glue allocator + lane ops).
			target_link_libraries(
				tests_conv_proxy PRIVATE comp_d3d11_lift comp_d3d11_conv_host_lane d3d11 dxgi
				)
		endif()
		add_dependencies(tests_conv_proxy displayxr-conversion-host drv_sim_conversion_plugin)
		target_compile_definitions(
			tests_conv_proxy
			PRIVATE "CONV_REAL_HOST_EXE=\"$<TARGET_FILE:displayxr-conversion-host>\""
				"SIM_CONVERSION_PATH=\"$<TARGET_FILE:drv_sim_conversion_plugin>\""
			)
	endif()
endif()

# ADR-053 P1a: every XR_DXR_lift v3 sizeof / offsetof / enum pinned, so spec 4
# can only append.
target_link_libraries(tests_lift_abi PRIVATE xrt-external-openxr)

# XR_DXR_lift v3 (ADR-048 Addendum A): the app-rig mapping is checked against the
# shared displayxr-common rig core xrLocateViews runs.
target_link_libraries(tests_aux_lift_rig_depth PRIVATE displayxr::math_core)

# #1855: sim_display webcam eye tracking -- geometry, one-euro, timeout/collapse,
# device/mode selection, and the worker end to end on the synthetic UVC backend
# with a fake face estimator (no camera is ever opened).
target_sources(
	tests_sim_webcam_tracking
	PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_display/sim_display_webcam_tracking.c
		${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_display/sim_display_webcam_tracker.c
		${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_display/sim_display_face_estimator_none.c
		${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_display/sim_display_face_yunet.c
	)
target_include_directories(tests_sim_webcam_tracking PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_display)

# N6 Part 1 (#2144): sim_display's synthetic viewer at a target time — the pure
# half of its get_eye_positions_at slot (sway + square-wave tracking), pinned
# host-side (it compiles into the plug-in).
target_sources(tests_sim_eyes PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_display/sim_display_eyes.c)
target_include_directories(tests_sim_eyes PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_display)
target_link_libraries(tests_sim_eyes PRIVATE xrt-interfaces)

# ADR-043: the sim_display FAKE stereo camera's scene is pinned host-side next to
# the ring / conversion / disparity-probe tests (it compiles into the plug-in).
target_sources(tests_stereo_camera PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_display/sim_display_stereo_camera_pattern.c)
target_include_directories(tests_stereo_camera PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_display)

# ADR-043 R2: the rectifier (u_stereo_rectify) against OpenCV-generated golden
# fixtures, and end to end on the sim_display DISTORTED fake (its scene +
# synthetic lens live in the pattern file).
target_sources(tests_stereo_rectify PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_display/sim_display_stereo_camera_pattern.c)
target_include_directories(tests_stereo_rectify PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_display ${CMAKE_CURRENT_SOURCE_DIR}/fixtures)

# ADR-043 R2: the online vertical-alignment refinement — on the distorted fake
# with an injected, uncalibrated offset + slope, and on one REAL raw frame from
# a Leia SR laptop (fixtures/stereo_vrefine_sr_raw.jpg, decoded with stb_image).
target_sources(tests_stereo_vrefine PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_display/sim_display_stereo_camera_pattern.c)
target_include_directories(tests_stereo_vrefine PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/drivers/sim_display ${CMAKE_CURRENT_SOURCE_DIR}/fixtures ${PROJECT_SOURCE_DIR}/src/external/stb)
target_compile_definitions(tests_stereo_vrefine PRIVATE STEREO_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

# ADR-043 Amendment 4: the service's REAL camera manager end to end, driven
# through its IPC handlers with the UVC source on its fake backend (no device,
# no Media Foundation, no service process). Compiles the manager straight in
# rather than linking ipc_server, whose handler file pulls in every compositor;
# the glue stubs the two symbols it would have taken from there.
if(TARGET ipc_server)
	add_executable(
		tests_stereo_camera_manager
		tests_stereo_camera_manager.cpp tests_stereo_camera_manager_glue.c tests_stereo_camera_manager_glue.h
		${PROJECT_SOURCE_DIR}/src/xrt/ipc/server/ipc_server_stereo_camera.c
		${PROJECT_SOURCE_DIR}/src/xrt/ipc/server/ipc_server_peer_creds.c
		)
	target_link_libraries(
		tests_stereo_camera_manager PRIVATE xrt-external-catch2 xrt-interfaces aux_util aux_os aux_os_uvc ipc_shared
		)
	# ipc_server_generated.h (handler prototypes) is generated with ipc_server.
	add_dependencies(tests_stereo_camera_manager ipc_server)
	add_test(NAME tests_stereo_camera_manager COMMAND tests_stereo_camera_manager --success
							 --allow-running-no-tests
		)
endif()

# ADR-043 Amendment 4, Windows: the Media Foundation UVC backend's scaled output
# (GPU and CPU video processor) is a plain resize, never letterboxed — through
# the real backend on a generated YUY2 AVI. SKIPs without Media Foundation.
if(WIN32 AND TARGET aux_os_uvc)
	add_executable(tests_uvc_capture_mf tests_uvc_capture_mf.cpp)
	target_link_libraries(tests_uvc_capture_mf PRIVATE xrt-external-catch2 aux_util aux_os aux_os_uvc)
	add_test(NAME tests_uvc_capture_mf COMMAND tests_uvc_capture_mf --success --allow-running-no-tests)
endif()

# Shared-surface FRAME_TICK coalescing into the controller's input queue
# (macOS + desktop Linux; the queue is pthread-based, so POSIX only). Compiles
# the queue and the comp_multi workspace registry straight into the unit.
if(NOT WIN32 AND NOT ANDROID)
	add_executable(
		tests_ipc_input_queue_frame_tick
		tests_ipc_input_queue_frame_tick.cpp
		${PROJECT_SOURCE_DIR}/src/xrt/ipc/server/ipc_server_input_queue.c
		${PROJECT_SOURCE_DIR}/src/xrt/compositor/multi/comp_multi_workspace.c
		)
	target_include_directories(
		tests_ipc_input_queue_frame_tick PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/ipc
							 ${PROJECT_SOURCE_DIR}/src/xrt/compositor
		)
	target_link_libraries(tests_ipc_input_queue_frame_tick PRIVATE xrt-external-catch2 aux_util aux_os)
	add_test(NAME tests_ipc_input_queue_frame_tick COMMAND tests_ipc_input_queue_frame_tick --success)
endif()

# #886 -- VK_LAYER_DXR_queue_lock's activation routes through the REAL Vulkan
# loader: the explicit + implicit manifests side by side (as the .deb installs
# them), the enable2 injection unaffected by the implicit twin, the
# DXR_VK_QUEUE_LOCK=1 opt-in for vulkan_enable1 apps, and the route the layer
# reports. Desktop Linux only (the implicit manifest ships there); SKIPs (77)
# without a Vulkan driver.
if(NOT WIN32
   AND NOT ANDROID
   AND NOT APPLE
   AND TARGET VkLayer_DXR_queue_lock
   AND TARGET Vulkan::Vulkan
	)
	add_executable(tests_vk_queue_lock_activation tests_vk_queue_lock_activation.c)
	target_link_libraries(tests_vk_queue_lock_activation PRIVATE Vulkan::Vulkan)
	add_dependencies(tests_vk_queue_lock_activation VkLayer_DXR_queue_lock)
	add_test(NAME tests_vk_queue_lock_activation
		 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests_vk_queue_lock_activation.sh
			 $<TARGET_FILE:tests_vk_queue_lock_activation>
			 $<TARGET_FILE_DIR:VkLayer_DXR_queue_lock>
		)
	set_tests_properties(tests_vk_queue_lock_activation PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 240)
endif()
