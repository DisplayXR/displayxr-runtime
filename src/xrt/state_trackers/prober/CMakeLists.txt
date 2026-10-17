# Copyright 2019-2021, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0

add_library(
	st_prober STATIC
	p_documentation.h
	p_dump.c
	p_prober.c
	p_prober.h
	)

target_link_libraries(st_prober PUBLIC xrt-interfaces)
target_link_libraries(
	st_prober
	PRIVATE
		drv_includes
		aux_util
		aux_os
	)

# Add libudev
if(XRT_HAVE_LIBUDEV)
	target_sources(st_prober PRIVATE p_udev.c)
	target_include_directories(st_prober PRIVATE ${UDEV_INCLUDE_DIRS})
	target_link_libraries(st_prober PRIVATE ${UDEV_LIBRARIES})
endif()
