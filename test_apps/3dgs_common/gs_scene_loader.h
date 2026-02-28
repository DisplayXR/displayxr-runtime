// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  PLY scene loading utilities for 3DGS
 */

#pragma once

#include <string>

// Validate that a file path points to a valid .ply Gaussian splatting scene.
// Returns true if the file exists and has a .ply extension.
bool ValidatePlyFile(const std::string& path);

// Extract the filename (without directory) from a full path.
std::string GetPlyFilename(const std::string& path);

// Get a human-readable size string (e.g., "12.3 MB") for the file.
std::string GetPlyFileSize(const std::string& path);
