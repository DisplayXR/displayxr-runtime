# Copyright 2018-2023, Collabora, Ltd.
# SPDX-License-Identifier: BSL-1.0

if(TARGET xrt-optimized-math)
	return()
endif()

# Target used for applying more aggressive optimizations to math-heavy code
add_library(xrt-optimized-math INTERFACE)

if(MSVC)
	target_compile_options(xrt-optimized-math INTERFACE $<IF:$<CONFIG:Debug>,/O2 /Ob2,/O2 /Ob3>)
else()
	set(CMAKE_C_FLAGS
	    "${CMAKE_C_FLAGS} -pedantic -Wall -Wextra -Wno-unused-parameter -Wno-missing-braces"
		)
	set(CMAKE_C_FLAGS
	    "${CMAKE_C_FLAGS} -Werror-implicit-function-declaration -Werror=incompatible-pointer-types"
		)
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror=int-conversion")
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pedantic -Wall -Wextra -Wno-unused-parameter")

	# Use effectively ubiquitous SSE2 instead of x87 floating point
	# for increased reliability/consistency
	if(CMAKE_SYSTEM_PROCESSOR MATCHES "[xX]86" AND XRT_FEATURE_SSE2)
		set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -msse2 -mfpmath=sse")
		set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -msse2 -mfpmath=sse")
	endif()

	target_compile_options(xrt-optimized-math INTERFACE $<IF:$<CONFIG:Debug>,-O2,-O3>)
endif()

if(NOT WIN32 AND NOT APPLE)
	# GNU ld flag; not accepted by MSVC linker or macOS Apple ld.

	set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--no-undefined")
	set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,--no-undefined")
endif()

if(ANDROID)
	# 16 KB page-size support (DisplayXR/displayxr-unity#418). Android 15+
	# devices may run a 16 KB-page kernel, whose dynamic linker refuses any .so
	# with PT_LOAD segments aligned below 16 KB -- and NDK r26 (our pin) still
	# links at 4 KB. A 16 KB-aligned library loads fine on 4 KB kernels too.
	# This is the NDK-documented mechanism for r26 and older (r27 adds the
	# ANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES switch, r28 makes it the default);
	# the explicit flag stays correct on every NDK. Both the root build and the
	# client AAR's standalone layer build include this file, so their
	# queue-lock layer copies stay byte-identical. CI asserts the result on
	# the artifacts: scripts/check_android_page_align.py.
	set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,-z,max-page-size=16384")
	set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} -Wl,-z,max-page-size=16384")
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-z,max-page-size=16384")
endif()

# Must call before adding targets that will use xrt-optimized-math
macro(xrt_optimized_math_flags)
	if(MSVC)
		foreach(
			FLAGSVAR
			CMAKE_CXX_FLAGS_DEBUG
			CMAKE_CXX_FLAGS_RELEASE
			CMAKE_CXX_FLAGS_RELWITHDEBINFO
			CMAKE_CXX_FLAGS_MINSIZEREL
			CMAKE_C_FLAGS_DEBUG
			CMAKE_C_FLAGS_RELEASE
			CMAKE_C_FLAGS_RELWITHDEBINFO
			CMAKE_C_FLAGS_MINSIZEREL
			)

			string(REPLACE "/Od" "" ${FLAGSVAR} "${${FLAGSVAR}}")
			string(REPLACE "/O1" "" ${FLAGSVAR} "${${FLAGSVAR}}")
			string(REPLACE "/O2" "" ${FLAGSVAR} "${${FLAGSVAR}}")
			string(REPLACE "/Ob1" "" ${FLAGSVAR} "${${FLAGSVAR}}")
			string(REPLACE "/Ob0" "" ${FLAGSVAR} "${${FLAGSVAR}}")
			string(REPLACE "/RTC1" "" ${FLAGSVAR} "${${FLAGSVAR}}")
		endforeach()
	endif()
endmacro()
