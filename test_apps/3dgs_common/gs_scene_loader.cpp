// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  PLY scene loading utilities implementation
 */

#include "gs_scene_loader.h"

#include <filesystem>
#include <cstdio>

bool ValidatePlyFile(const std::string& path)
{
    if (path.empty()) return false;

    // Check extension
    std::filesystem::path p(path);
    auto ext = p.extension().string();
    if (ext != ".ply" && ext != ".PLY") return false;

    // Check file exists
    return std::filesystem::exists(p);
}

std::string GetPlyFilename(const std::string& path)
{
    return std::filesystem::path(path).filename().string();
}

std::string GetPlyFileSize(const std::string& path)
{
    try {
        auto size = std::filesystem::file_size(path);
        if (size >= 1024 * 1024 * 1024) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.1f GB", (double)size / (1024.0 * 1024.0 * 1024.0));
            return buf;
        } else if (size >= 1024 * 1024) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.1f MB", (double)size / (1024.0 * 1024.0));
            return buf;
        } else if (size >= 1024) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.1f KB", (double)size / 1024.0);
            return buf;
        } else {
            return std::to_string(size) + " B";
        }
    } catch (...) {
        return "unknown";
    }
}
