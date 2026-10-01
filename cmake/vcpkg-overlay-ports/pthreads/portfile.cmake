if(NOT VCPKG_TARGET_IS_WINDOWS OR VCPKG_TARGET_IS_MINGW)
  set(VCPKG_POLICY_EMPTY_PACKAGE enabled)
  return()
endif()

if(VCPKG_TARGET_IS_UWP)
  list(APPEND PATCH_FILES fix-uwp-linkage.patch)
  # Inject linker option using the `LINK` environment variable
  # https://docs.microsoft.com/en-us/cpp/build/reference/linker-options
  # https://docs.microsoft.com/en-us/cpp/build/reference/linking#link-environment-variables
  set(ENV{LINK} "/APPCONTAINER")
endif()

if (VCPKG_CRT_LINKAGE STREQUAL dynamic)
  list(APPEND PATCH_FILES use-md.patch)
else()
  list(APPEND PATCH_FILES use-mt.patch)
endif()

# DisplayXR overlay: identical to microsoft/vcpkg ports/pthreads at the
# builtin-baseline (c82f74667287d3dc386bce81e44964370c91a289, port-version 14)
# except for the source fetch below. Upstream does vcpkg_from_sourceforge for
# pthreads4w-code-v3.0.0.zip; SourceForge now answers CI runners with an HTML
# bot-check page, so every mirror fails the hash check.
#
# The same source comes from GitHub instead. Commit 07053a52 is the
# Version-3-0-0-release tag of the upstream SourceForge git repository
# (git.code.sf.net/p/pthreads4w/code), mirrored with full history in
# GerHobbelt/pthread-win32. Commit ids are content-addressed, so the tree is
# upstream's byte for byte. The patches below were authored against the
# SourceForge zip; their "index <blob>.." preimages (Makefile a703b9c,
# context.h 33294c1, version.rc aa0596c, pthread_getname_np.c 8fc32b1,
# implement.h 1579376) all equal the blobs at this commit.
#
# Drop this overlay once upstream vcpkg moves the port off SourceForge.
vcpkg_from_github(
  OUT_SOURCE_PATH SOURCE_PATH
  REPO GerHobbelt/pthread-win32
  REF 07053a521b0a9deb6db2a649cde1f828f2eb1f4f # Version-3-0-0-release
  SHA512 4700c0c8bf584aca761c368ab08e098eeb63a3f2119dfb9f2dd667ef3f0e6c31e7c93b9e375266a2f494b661a5aeea89990dc31d306c1548f5ef86ac76e7e79b
  PATCHES
    fix-arm-macro.patch
    fix-arm64-version_rc.patch # https://sourceforge.net/p/pthreads4w/code/merge-requests/6/
    fix-pthread_getname_np.patch
    fix-install.patch
    whitespace_in_path.patch
    ${PATCH_FILES}
)

file(TO_NATIVE_PATH "${CURRENT_PACKAGES_DIR}/debug" DESTROOT_DEBUG)
file(TO_NATIVE_PATH "${CURRENT_PACKAGES_DIR}" DESTROOT_RELEASE)

vcpkg_list(SET OPTIONS_DEBUG "DESTROOT=${DESTROOT_DEBUG}")
vcpkg_list(SET OPTIONS_RELEASE "DESTROOT=${DESTROOT_RELEASE}" "BUILD_RELEASE=1")

if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
  vcpkg_list(APPEND OPTIONS_DEBUG "BUILD_STATIC=1")
  vcpkg_list(APPEND OPTIONS_RELEASE "BUILD_STATIC=1")
endif()

vcpkg_install_nmake(
  CL_LANGUAGE C
  SOURCE_PATH "${SOURCE_PATH}"
  PROJECT_NAME Makefile
  OPTIONS_DEBUG ${OPTIONS_DEBUG}
  OPTIONS_RELEASE ${OPTIONS_RELEASE}
)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
  file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/bin" "${CURRENT_PACKAGES_DIR}/debug/bin")
endif()

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/PThreads4WConfig.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/PThreads4W")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/vcpkg-cmake-wrapper-pthread.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/pthread" RENAME vcpkg-cmake-wrapper.cmake)
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/vcpkg-cmake-wrapper-pthreads.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/pthreads" RENAME vcpkg-cmake-wrapper.cmake)
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/vcpkg-cmake-wrapper-pthreads-windows.cmake" DESTINATION "${CURRENT_PACKAGES_DIR}/share/PThreads_windows" RENAME vcpkg-cmake-wrapper.cmake)

file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

set(VCPKG_POLICY_ALLOW_RESTRICTED_HEADERS enabled)
