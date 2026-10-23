# Copyright 2019-2021, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0

add_library(
	st_prober STATIC
	p_documentation.h
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
