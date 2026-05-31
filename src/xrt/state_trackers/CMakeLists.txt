# Copyright 2019, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0

if(XRT_HAVE_OPENGL)
	add_subdirectory(gui)
endif()

if(XRT_MODULE_OPENXR_STATE_TRACKER)
	add_subdirectory(oxr)
endif()

add_subdirectory(prober)
