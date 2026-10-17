# Copyright 2019-2022, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0

# Common includes
add_library(aux-includes INTERFACE)
target_include_directories(
	aux-includes INTERFACE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}
	)
target_link_libraries(aux-includes INTERFACE xrt-interfaces)

# Bindings library.
add_subdirectory(bindings)

# OpenGL library.
if(XRT_HAVE_OPENGL OR XRT_HAVE_OPENGLES)
	add_subdirectory(ogl)
endif()

# Direct3D library
if(XRT_HAVE_D3D11 OR XRT_HAVE_D3D12)
	add_subdirectory(d3d)
endif()

# Math library.
add_subdirectory(math)

# OS library.
add_subdirectory(os)

# Tracking library (IMU fusion only).
add_subdirectory(tracking)

# Util library.
add_subdirectory(util)

# Desktop control library (XR_DXR_desktop_control, ADR-056).
if(NOT ANDROID)
	add_subdirectory(desktop)
endif()

# Android library.
if(ANDROID)
	add_subdirectory(android)
endif()

# Vulkan library.
if(XRT_HAVE_VULKAN)
	add_subdirectory(vk)
endif()
