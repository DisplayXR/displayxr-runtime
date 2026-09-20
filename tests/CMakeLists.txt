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
    tests_vec3_angle
    tests_u_multi_display
    tests_dp_weave_scope
    tests_dp_vk_snap_window_rect
    tests_space_overseer_rig
    tests_rig_composer
    tests_cli_dims_check
    tests_comp_vblank_grid
    tests_comp_zone_tier1
    tests_comp_swapchain_ring
    tests_aux_bg_neutrality
    tests_aux_rear_budget
    tests_aux_route_policy
    tests_aux_overlay_lease
    tests_aux_mini_window_tell
    tests_aux_zone_canvas
    tests_aux_tiling_zero_copy
    tests_comp_rear_budget
    tests_oxr_weave_latch
    tests_oxr_view_config_rule
    tests_oxr_legacy_mode_rule
    tests_oxr_mode_fillable_rule
    tests_xrt_active_mode_scale
    tests_input_arbiter_rig
    tests_sim_input_nav
    tests_comp_layer_view_camera
	)
if(XRT_BUILD_DRIVER_QWERTY)
	list(APPEND tests tests_qwerty_camera_profile)
endif()

# #1380 -- the input-provider host iface's display-geometry cache. Needs the
# loader itself (target_lists), so it is only registered where that target
# exists; everything it drives is the exact function pointer providers get.
if(TARGET target_lists)
	list(APPEND tests tests_input_host_geometry)
endif()
if(XRT_HAVE_D3D11)
	list(APPEND tests tests_aux_d3d_d3d11 tests_aux_d3d_render_adapter tests_comp_client_d3d11
	     tests_comp_xbridge_plane_policy tests_comp_split_gate
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
if(XRT_HAVE_VULKAN)
	list(APPEND tests tests_comp_client_vulkan tests_uv_to_tangent)
endif()
if(XRT_FEATURE_OPENXR)
	list(APPEND tests tests_input_transform)
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
if(XRT_BUILD_DRIVER_HANDTRACKING)
	list(APPEND tests tests_levenbergmarquardt)
endif()

foreach(testname ${tests})
	add_executable(${testname} ${testname}.cpp)
	target_link_libraries(${testname} PRIVATE xrt-external-catch2)
	target_link_libraries(${testname} PRIVATE aux_util)
	add_test(NAME ${testname} COMMAND ${testname} --success --allow-running-no-tests)
endforeach()

# For tests that require more than just aux_util, link those other libs down here.

target_include_directories(tests_oxr_session_window_binding PRIVATE ${CMAKE_SOURCE_DIR}/src/xrt/state_trackers/oxr)

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

if(WIN32 AND XRT_HAVE_D3D11 AND XRT_FEATURE_OPENXR AND XRT_FEATURE_OPENXR_HEADLESS AND RUNTIME_TARGET)
	# Explicit acceptance probe: opens native windows, so never an automatic CTest.
	add_executable(tests_oxr_camera_profile EXCLUDE_FROM_ALL tests_oxr_camera_profile.cpp)
	target_include_directories(tests_oxr_camera_profile PRIVATE ${CMAKE_SOURCE_DIR}/src/external/openxr_includes)
	target_compile_definitions(tests_oxr_camera_profile PRIVATE "DXR_RUNTIME_LIB_PATH=\"$<TARGET_FILE:${RUNTIME_TARGET}>\"")
	target_link_libraries(tests_oxr_camera_profile PRIVATE d3d11 dxgi)
	add_dependencies(tests_oxr_camera_profile ${RUNTIME_TARGET})
endif()

# ADR-030/ADR-041 -- u_tiling_can_zero_copy(), the sole zero-copy gate. Header
# arithmetic only; it reads xrt_rendering_mode, hence xrt-interfaces.
target_link_libraries(tests_aux_tiling_zero_copy PRIVATE xrt-interfaces)

target_link_libraries(tests_cxx_wrappers PRIVATE xrt-interfaces)
target_link_libraries(tests_history_buf PRIVATE aux_math)
target_link_libraries(tests_lowpass_float PRIVATE aux_math)
target_link_libraries(tests_lowpass_integer PRIVATE aux_math)
target_link_libraries(tests_quatexpmap PRIVATE aux_math)
target_link_libraries(tests_rational PRIVATE aux_math)
target_link_libraries(tests_relation_chain PRIVATE aux_math)
target_link_libraries(tests_pose PRIVATE aux_math)
target_link_libraries(tests_quat_change_of_basis PRIVATE aux_math)
target_link_libraries(tests_quat_swing_twist PRIVATE aux_math)
target_link_libraries(tests_vec3_angle PRIVATE aux_math)
target_link_libraries(tests_space_overseer_rig PRIVATE aux_math)

# #1580 -- the shared per-view camera resolver every backend's quad/cylinder/
# equirect/cube path consumes. Compiled straight into the unit (like
# tests_rig_composer) rather than linked from comp_util, which is Vulkan-gated
# and would drag a whole compositor into a test that needs no device at all.
# aux_math carries displayxr::math_xrt, i.e. dxr_view_math.h.
target_sources(
	tests_comp_layer_view_camera
	PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor/util/comp_layer_view_camera.c
	)
target_include_directories(
	tests_comp_layer_view_camera PRIVATE ${PROJECT_SOURCE_DIR}/src/xrt/compositor
	)
target_link_libraries(tests_comp_layer_view_camera PRIVATE xrt-interfaces aux_math)

if(TARGET tests_input_host_geometry)
	target_link_libraries(tests_input_host_geometry PRIVATE target_lists xrt-interfaces)
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
		tests_oxr_view_space PRIVATE ${CMAKE_SOURCE_DIR}/src/external/openxr_includes
		)
	target_compile_definitions(
		tests_oxr_view_space PRIVATE "DXR_RUNTIME_LIB_PATH=\"$<TARGET_FILE:${RUNTIME_TARGET}>\""
		)
	add_dependencies(tests_oxr_view_space ${RUNTIME_TARGET})
	if(NOT WIN32)
		target_link_libraries(tests_oxr_view_space PRIVATE ${CMAKE_DL_LIBS})
	endif()

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

# #1427 — the XR_DXR_weave handle-export latch rule. A pure boolean header with
# no runtime dependency (see tests_oxr_weave_latch.cpp for why the entry point
# itself is not host-drivable), so it needs the st_oxr include path and nothing
# else.
target_include_directories(
	tests_oxr_weave_latch PRIVATE ${CMAKE_SOURCE_DIR}/src/xrt/state_trackers/oxr
	)

# #1486 — the two xrEndFrame projection-layer viewCount rules (tight
# PRIMARY_STEREO vs the permissive body PRIMARY_MULTIVIEW_DXR and the
# DXR_VIEW_CONFIG_LEGACY rollback share). Pure integer decisions in a header, for
# the same reason as the weave latch above: the entry point needs a submitted
# frame, so only the rule is host-drivable.
target_include_directories(
	tests_oxr_view_config_rule PRIVATE ${CMAKE_SOURCE_DIR}/src/xrt/state_trackers/oxr
	)

# #1510 — the legacy mode floor and the compromise view scale it implies. Same
# reason as the two above: the real decision needs an instance, a system, a head
# device and a plug-in, so only the rule is host-drivable. It reads
# xrt_rendering_mode, hence xrt-interfaces (headers only).
target_include_directories(
	tests_oxr_legacy_mode_rule PRIVATE ${CMAKE_SOURCE_DIR}/src/xrt/state_trackers/oxr
	)
target_link_libraries(tests_oxr_legacy_mode_rule PRIVATE xrt-interfaces)

# #1499 — the GENERAL mode floor the legacy one above is now a wrapper over:
# "a session never runs in a mode it cannot fill", parameterised on the view
# count of the view configuration it began. Same header, same reason it is only
# the RULE that is host-drivable (the wiring needs an instance, a system, a head
# device and a plug-in), so the same include + xrt-interfaces and nothing else.
target_include_directories(
	tests_oxr_mode_fillable_rule PRIVATE ${CMAKE_SOURCE_DIR}/src/xrt/state_trackers/oxr
	)
target_link_libraries(tests_oxr_mode_fillable_rule PRIVATE xrt-interfaces)

# One source of truth for the ACTIVE mode's per-view scale. The accessor is a
# static inline over xrt_device / xrt_rendering_mode, so headers are all it
# needs -- but it is the thing three hand-maintained overwrite sites used to do
# by hand, which is exactly why it is worth pinning.
target_link_libraries(tests_xrt_active_mode_scale PRIVATE xrt-interfaces)

# #1201 — the self-test rule that makes `display_dims` prove its number instead
# of printing it. A header of pure integer decisions (the probe that feeds it is
# Windows-only, the rule is not), so there is nothing to link but the include.
target_include_directories(tests_cli_dims_check PRIVATE ${CMAKE_SOURCE_DIR}/src/xrt/targets/cli)

# Android late-weave / repaint pacing (#868/#902). The vblank grid is
# header-only and deliberately free of Vulkan and DXGI types — that is what
# lets its drift / staleness / mode-switch logic be tested on the host, where
# those bugs are actually findable.
target_include_directories(tests_comp_vblank_grid PRIVATE ${CMAKE_SOURCE_DIR}/src/xrt/compositor)
# ADR-027 tier-1 dispatch (#1331). Header-only pure logic -- no compositor
# library, no graphics API, no device -- so it needs the include path and
# nothing else.
target_include_directories(tests_comp_zone_tier1 PRIVATE ${CMAKE_SOURCE_DIR}/src/xrt/compositor)

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

if(XRT_BUILD_DRIVER_HANDTRACKING)
	target_link_libraries(
		tests_levenbergmarquardt
		PRIVATE
			aux_math
			t_ht_mercury_includes
			t_ht_mercury_kine_lm_includes
			t_ht_mercury
			t_ht_mercury_kine_lm
		)
endif()

if(XRT_HAVE_D3D11)
	target_link_libraries(tests_aux_d3d_d3d11 PRIVATE aux_d3d)
	# ADR-037 §2 render-adapter resolver — DXGI enumeration plus a driverless
	# feature-level probe; no compositor, no window, no panel.
	target_link_libraries(tests_aux_d3d_render_adapter PRIVATE aux_d3d)
	# #918 bridge-plane transport policy — a header of pure decisions, no D3D
	# link needed (the point of extracting it was that none of these need a GPU).
	target_include_directories(
		tests_comp_xbridge_plane_policy PRIVATE ${CMAKE_SOURCE_DIR}/src/xrt/compositor/xbridge
		)
	# #918 Phase 3 — the split's activation decision. Pure C, no device, no
	# graphics types at all (the reason it was extracted), so the unit links the
	# gate's own static lib and nothing else.
	target_include_directories(
		tests_comp_split_gate PRIVATE ${CMAKE_SOURCE_DIR}/src/xrt/compositor/xbridge
		)
	if(TARGET comp_xbridge)
		target_link_libraries(tests_comp_split_gate PRIVATE comp_xbridge)
	endif()
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
		tests_comp_masked_composite_shaders PRIVATE ${CMAKE_SOURCE_DIR}/src/xrt/compositor
		)
	target_link_libraries(tests_comp_masked_composite_shaders PRIVATE d3dcompiler)
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
