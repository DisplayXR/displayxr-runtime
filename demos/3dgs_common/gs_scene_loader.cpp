// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  PLY scene loading utilities + binary PLY parser implementation
 */

#include "gs_scene_loader.h"

#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

// On-disk PLY vertex: 62 floats = 248 bytes
struct PlyVertexStorage {
    float x, y, z;           // position (3)
    float nx, ny, nz;        // normals (3) — always 0, discarded
    float shs[48];           // SH coefficients in PLY order (48)
    float opacity;           // raw logit opacity (1)
    float sx, sy, sz;        // raw log scale (3)
    float rot_w, rot_x, rot_y, rot_z;  // quaternion (4)
};
static_assert(sizeof(PlyVertexStorage) == 248, "PlyVertexStorage must be 248 bytes");

static float sigmoid(float x) {
    return 1.0f / (1.0f + std::exp(-x));
}

bool ParsePlyFile(const std::string& path, std::vector<GsVertex>& vertices)
{
    vertices.clear();

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        fprintf(stderr, "ParsePlyFile: cannot open %s\n", path.c_str());
        return false;
    }

    // Parse PLY header
    std::string line;
    uint32_t numVertices = 0;
    bool foundHeader = false;

    while (std::getline(file, line)) {
        // Strip carriage return if present
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (line.find("element vertex") != std::string::npos) {
            if (sscanf(line.c_str(), "element vertex %u", &numVertices) != 1) {
                fprintf(stderr, "ParsePlyFile: failed to parse vertex count\n");
                return false;
            }
        }
        if (line == "end_header") {
            foundHeader = true;
            break;
        }
    }

    if (!foundHeader || numVertices == 0) {
        fprintf(stderr, "ParsePlyFile: invalid PLY header (vertices=%u)\n", numVertices);
        return false;
    }

    printf("ParsePlyFile: loading %u gaussians from %s\n", numVertices, path.c_str());

    // Read binary vertex data
    std::vector<PlyVertexStorage> storage(numVertices);
    file.read(reinterpret_cast<char*>(storage.data()),
              (std::streamsize)(numVertices * sizeof(PlyVertexStorage)));

    if (!file) {
        fprintf(stderr, "ParsePlyFile: failed to read vertex data (expected %zu bytes)\n",
                (size_t)numVertices * sizeof(PlyVertexStorage));
        return false;
    }

    // Transform to GPU format
    vertices.resize(numVertices);

    for (uint32_t i = 0; i < numVertices; i++) {
        const PlyVertexStorage& s = storage[i];
        GsVertex& v = vertices[i];

        // gsplat-trainer PLY: RDF (Y-down, Z-forward) plus an X-mirror vs
        // SuperSplat reference. Convert to canonical RUB (+X right, +Y up,
        // +Z back) at load so the renderer / app code can stay in one
        // consistent +Y-up convention end-to-end. Without this, runtime
        // view-stage flips would have to compensate (and that pattern caused
        // the eye-Y / click-NDC / teleport-target Y inconsistencies).
        v.position[0] = -s.x;   // undo X-mirror
        v.position[1] = -s.y;   // RDF Y-down → canonical Y-up
        v.position[2] = -s.z;   // RDF Z-forward → canonical Z-back
        v.position[3] = 1.0f;

        // Scale: exp(raw), Opacity: sigmoid(raw)
        v.scale_opacity[0] = std::exp(s.sx);
        v.scale_opacity[1] = std::exp(s.sy);
        v.scale_opacity[2] = std::exp(s.sz);
        v.scale_opacity[3] = sigmoid(s.opacity);

        // Rotation: normalize quaternion
        float qw = s.rot_w, qx = s.rot_x, qy = s.rot_y, qz = s.rot_z;
        float qlen = std::sqrt(qw*qw + qx*qx + qy*qy + qz*qz);
        if (qlen > 1e-8f) {
            float inv = 1.0f / qlen;
            qw *= inv; qx *= inv; qy *= inv; qz *= inv;
        }
        // Quaternion under the (-X, -Y, -Z) coord-flip composition:
        // for q = (w, x, y, z), the rotation conjugated by the -I sign flip
        // on the imaginary axes leaves the imaginary part unchanged in a
        // pure 180-about-anything sense; but here we're flipping all three
        // axes, which is equivalent to negating w (or equivalently the
        // double-cover identity gives same rotation). Empirically this
        // pattern matches the position transform for the X+Y+Z mirror:
        v.rotation[0] = qw;  // w first (matches shader convention: q.x = w)
        v.rotation[1] = -qx;
        v.rotation[2] =  qy;
        v.rotation[3] = -qz;

        // SH coefficients: de-interleave from PLY layout to GPU layout.
        // PLY:  [R_dc, G_dc, B_dc, R_1..R_15, G_1..G_15, B_1..B_15]
        // GPU:  [R_dc, G_dc, B_dc, R_1, G_1, B_1, R_2, G_2, B_2, ...]
        v.sh[0] = s.shs[0];  // R DC
        v.sh[1] = s.shs[1];  // G DC
        v.sh[2] = s.shs[2];  // B DC

        for (int j = 1; j < 16; j++) {
            v.sh[j * 3 + 0] = s.shs[(j - 1) + 3];       // R channel
            v.sh[j * 3 + 1] = s.shs[(j - 1) + 18];      // G channel (3 + 15)
            v.sh[j * 3 + 2] = s.shs[(j - 1) + 33];      // B channel (3 + 30)
        }
    }

    printf("ParsePlyFile: loaded %u gaussians (%.1f MB GPU data)\n",
           numVertices, (double)(numVertices * sizeof(GsVertex)) / (1024.0 * 1024.0));
    return true;
}

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

bool ValidateSceneFile(const std::string& path)
{
    if (path.empty()) return false;

    std::filesystem::path p(path);
    auto ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext != ".ply" && ext != ".spz") return false;

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
