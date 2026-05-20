set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

# Force pthreads to build as a static library so DisplayXRClient.dll has no
# runtime dependency on pthreadVCE3.dll. The PThreads4WConfig.cmake provided
# by the port auto-detects the absence of bin/*.dll and creates an UNKNOWN
# IMPORTED CMake target — same target name (PThreads4W::PThreads4W_CXXEXC),
# so xrt-pthreads requires no source changes.
#
# Static pthreads is the only piece statically linked from vcpkg; everything
# else (Vulkan loader, glslang, eigen) keeps its default linkage. This lets
# the installer drop its system-PATH modification (issue: PATH was previously
# required to resolve pthreadVCE3.dll alongside DisplayXRClient.dll).
if(PORT STREQUAL "pthreads")
    set(VCPKG_LIBRARY_LINKAGE static)
endif()
