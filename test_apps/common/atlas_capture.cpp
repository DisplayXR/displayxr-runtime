// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Windows-side cross-platform helpers for atlas capture: filename /
 *         output-path resolution, white-flash overlay, and the (single)
 *         stb_image_write implementation TU for Windows targets.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string>

// Single STB implementation for Windows-linked targets. macOS has its own in
// atlas_capture_macos.mm — apps link exactly one of the two.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "atlas_capture.h"
#include "xr_session_common.h"  // XrSessionManager + XR_EXT_atlas_capture
#include "logging.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace dxr_capture {

// ---------------------------------------------------------------------------
// Output path / filename helpers
// ---------------------------------------------------------------------------

std::string PicturesDirectory() {
    PWSTR picsW = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_CREATE,
                                      nullptr, &picsW);
    if (FAILED(hr) || picsW == nullptr) return "";
    char picsA[MAX_PATH] = {};
    WideCharToMultiByte(CP_UTF8, 0, picsW, -1, picsA, MAX_PATH, nullptr, nullptr);
    CoTaskMemFree(picsW);
    std::string out = std::string(picsA) + "\\DisplayXR";
    CreateDirectoryA(out.c_str(), nullptr);  // ignore "already exists"
    return out;
}

// Scan `dir` for files named "<stem>-<N><suffix>" with an all-digit <N> and
// return max(N)+1 (1 if none). Shared by the legacy and the xrCaptureAtlasEXT
// naming, which differ only in the trailing suffix.
static int NextCaptureNumSuffix(const std::string& dir,
                                const std::string& stem,
                                const std::string& suffix) {
    if (dir.empty()) return 1;
    std::string prefix = stem + "-";
    int maxNum = 0;
    std::string pattern = dir + "\\" + prefix + "*" + suffix;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 1;
    do {
        std::string fname = fd.cFileName;
        if (fname.size() <= prefix.size() + suffix.size()) continue;
        if (fname.compare(0, prefix.size(), prefix) != 0) continue;
        size_t suffixStart = fname.size() - suffix.size();
        if (fname.compare(suffixStart, suffix.size(), suffix) != 0) continue;
        std::string numStr = fname.substr(prefix.size(),
                                          suffixStart - prefix.size());
        if (numStr.empty()) continue;
        bool allDigits = true;
        for (char c : numStr) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                allDigits = false; break;
            }
        }
        if (!allDigits) continue;
        int n = std::atoi(numStr.c_str());
        if (n > maxNum) maxNum = n;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return maxNum + 1;
}

int NextCaptureNum(const std::string& dir,
                   const std::string& stem,
                   uint32_t cols, uint32_t rows) {
    char suffixBuf[64];
    snprintf(suffixBuf, sizeof(suffixBuf), "_%ux%u.png", cols, rows);
    return NextCaptureNumSuffix(dir, stem, suffixBuf);
}

std::string MakeCapturePath(const std::string& stem,
                            uint32_t cols, uint32_t rows) {
    std::string dir = PicturesDirectory();
    int n = NextCaptureNum(dir, stem, cols, rows);
    char tail[256];
    snprintf(tail, sizeof(tail), "%s-%d_%ux%u.png",
             stem.c_str(), n, cols, rows);
    return dir.empty() ? std::string(tail) : (dir + "\\" + tail);
}

std::string MakeCaptureAtlasPrefix(const std::string& stem,
                                   uint32_t cols, uint32_t rows) {
    std::string dir = PicturesDirectory();
    // xrCaptureAtlasEXT appends "_atlas.png"; number against that final name so
    // repeats accumulate rather than overwrite (the legacy "_CxR.png" name space
    // is scanned separately, so the two never clobber each other's count).
    char atlasSuffix[80];
    snprintf(atlasSuffix, sizeof(atlasSuffix), "_%ux%u_atlas.png", cols, rows);
    int n = NextCaptureNumSuffix(dir, stem, atlasSuffix);
    char tail[256];
    snprintf(tail, sizeof(tail), "%s-%d_%ux%u", stem.c_str(), n, cols, rows);
    return dir.empty() ? std::string(tail) : (dir + "\\" + tail);
}

// ---------------------------------------------------------------------------
// White-flash overlay (layered top-most popup, fades out via WM_TIMER)
// ---------------------------------------------------------------------------

namespace {
HWND g_flashHwnd = nullptr;
int  g_flashAlpha = 0;
}  // namespace

void TriggerCaptureFlash(HWND parent) {
    if (parent == nullptr) return;
    if (g_flashHwnd == nullptr) {
        g_flashHwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT |
                WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            L"STATIC", L"",
            WS_POPUP,
            0, 0, 100, 100,
            parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (g_flashHwnd == nullptr) return;
        // Class-level white background brush; leak is OK for the lifetime
        // of the process (one-shot overlay).
        HBRUSH brush = CreateSolidBrush(RGB(255, 255, 255));
        SetClassLongPtrW(g_flashHwnd, GCLP_HBRBACKGROUND, (LONG_PTR)brush);
    }
    RECT cr; GetClientRect(parent, &cr);
    POINT pt = {0, 0}; ClientToScreen(parent, &pt);
    SetWindowPos(g_flashHwnd, HWND_TOPMOST, pt.x, pt.y,
                 cr.right - cr.left, cr.bottom - cr.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    g_flashAlpha = 255;
    SetLayeredWindowAttributes(g_flashHwnd, 0, (BYTE)g_flashAlpha, LWA_ALPHA);
    InvalidateRect(g_flashHwnd, nullptr, TRUE);
    SetTimer(parent, kFlashTimerId, 16, nullptr);  // ~60 Hz fade
}

void TickCaptureFlash(HWND parent) {
    g_flashAlpha -= 18;  // ~14 ticks × 16 ms ≈ 225 ms total
    if (g_flashAlpha <= 0 || g_flashHwnd == nullptr) {
        KillTimer(parent, kFlashTimerId);
        if (g_flashHwnd) ShowWindow(g_flashHwnd, SW_HIDE);
        return;
    }
    SetLayeredWindowAttributes(g_flashHwnd, 0, (BYTE)g_flashAlpha, LWA_ALPHA);
}

// ---------------------------------------------------------------------------
// Runtime-owned atlas capture (XR_EXT_atlas_capture) — the unified path.
// ---------------------------------------------------------------------------

bool RequestRuntimeAtlasCapture(const ::XrSessionManager& xr,
                                const char* appName,
                                uint32_t tileColumns,
                                uint32_t tileRows,
                                HWND flashHwnd) {
    if (xr.pfnCaptureAtlasEXT == nullptr || xr.session == XR_NULL_HANDLE) {
        LOG_WARN("Atlas capture unavailable: XR_EXT_atlas_capture not active");
        return false;
    }
    // Captures the app's projection atlas; meaningless for a mono (1×1) layout.
    if (tileColumns <= 1 && tileRows <= 1) {
        LOG_INFO("Capture skipped: need 3D mode with cols/rows > 1");
        return false;
    }

    std::string prefix = MakeCaptureAtlasPrefix(appName ? appName : "capture", tileColumns, tileRows);
    XrAtlasCaptureInfoEXT info = {XR_TYPE_ATLAS_CAPTURE_INFO_EXT};
    info.next = nullptr;
    info.stage = XR_ATLAS_CAPTURE_STAGE_PROJECTION_ONLY_EXT;
    strncpy_s(info.pathPrefix, prefix.c_str(), _TRUNCATE);

    XrResult cr = xr.pfnCaptureAtlasEXT(xr.session, &info, nullptr);
    if (XR_SUCCEEDED(cr)) {
        LOG_INFO("Atlas capture requested -> %s_atlas.png", prefix.c_str());
        PostFlashRequest(flashHwnd);
        return true;
    }
    LOG_WARN("xrCaptureAtlasEXT failed: 0x%x", (unsigned)cr);
    return false;
}

}  // namespace dxr_capture
