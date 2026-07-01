// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// stb_image implementation TU for the Linux GL app. main.cpp only *declares*
// stb_image (via displayxr::common's header); this TU provides the definition
// so stbi_load / stbi_image_free resolve at link. Mirrors the VK Linux peer. (#660)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
