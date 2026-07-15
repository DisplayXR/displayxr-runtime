# Copyright 2019-2023, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0

add_library(
	aux_vk STATIC
	vk_bundle_init.c
	vk_cmd.c
	vk_cmd.h
	vk_cmd_pool.c
	vk_cmd_pool.h
	vk_compositor_flags.c
	vk_debug.c
	vk_documentation.h
	vk_enumerate.c
	vk_function_loaders.c
	vk_helpers.c
	vk_helpers.h
	vk_hud_blend.c
	vk_hud_blend.h
	vk_local2d_composite.c
	vk_local2d_composite.h
	vk_image_allocator.c
	vk_image_allocator.h
	vk_image_readback_to_xf_pool.c
	vk_image_readback_to_xf_pool.h
	vk_mini_helpers.h
	vk_print.c
	vk_state_creators.c
	vk_surface_info.c
	vk_surface_info.h
	vk_sync_objects.c
	vk_time.c
	)
target_link_libraries(aux_vk PUBLIC aux_os aux_util)
target_link_libraries(aux_vk PUBLIC Vulkan::Vulkan)
target_include_directories(aux_vk PUBLIC ${Vulkan_INCLUDE_DIR})

if(ANDROID)
	target_link_libraries(aux_vk PUBLIC aux_android)
	target_link_libraries(aux_vk PUBLIC ${ANDROID_LIBRARY})
endif()

if(XRT_HAVE_WAYLAND)
	target_include_directories(aux_vk PRIVATE ${WAYLAND_INCLUDE_DIR})
endif()
if(XRT_HAVE_XCB)
	target_include_directories(aux_vk PRIVATE ${XCB_INCLUDE_DIR})
endif()
if(XRT_HAVE_XLIB_XRANDR)
	# With VK_USE_PLATFORM_XLIB_XRANDR_EXT defined, vulkan.h pulls in
	# vulkan_xlib_xrandr.h (-> X11/Xlib.h, X11/extensions/Xrandr.h), so the aux
	# VK display helpers need the X11/Xrandr headers on the include path. These
	# are usually system-global; the dirs are belt-and-suspenders. ST-5539.
	target_include_directories(aux_vk PRIVATE ${XLIB_XRANDR_INCLUDE_DIRS})
endif()
