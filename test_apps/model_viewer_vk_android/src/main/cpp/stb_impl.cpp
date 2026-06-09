// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// stb_image implementation TU. The glTF loader builds tinygltf with
// TINYGLTF_NO_STB_IMAGE and supplies a custom image loader that calls stbi_*;
// on desktop the stb implementation came from common/d3d11_renderer.cpp, which
// the Android port doesn't include — so define it here once.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
