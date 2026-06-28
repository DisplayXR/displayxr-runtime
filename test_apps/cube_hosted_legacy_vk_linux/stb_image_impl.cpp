// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// stb_image implementation TU for the Linux app. The shared main.cpp (verbatim
// from cube_hosted_legacy_vk_macos) only *declares* stb_image: on macOS the
// definition comes from displayxr::common's stb_image_impl_macos.cpp, which is
// macOS-gated, so the Linux build must supply its own (else stbi_load /
// stbi_image_free are undefined at link). Mirrors how the Windows peer defines
// STB_IMAGE_IMPLEMENTATION in its own main.cpp. (#660)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
