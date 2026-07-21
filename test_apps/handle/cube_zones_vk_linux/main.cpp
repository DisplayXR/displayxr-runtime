// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Cube Zones VK — XR_DXR_display_zones exerciser (ADR-027), Linux VK leg
 *
 * Port of cube_zones_vk_macos onto the cube_handle_vk_linux XCB/Vulkan handle
 * scaffold (#778). Handle class: the app creates and owns its X11 window and
 * passes it to the runtime via XR_DXR_xlib_window_binding (the Phase 3 vehicle
 * for app-provided windows on desktop Linux, docs/roadmap/linux-support.md,
 * #660). On top of that base it drives two 3D cube zones + an always-on Local2D
 * amber strip (the 2D-in-3D region the runtime's software-composite path
 * `vk_composite_local_2d` handles). Each zone renders the full cube + grid into
 * its own swapchain (per-zone rig framing, spin phase, premultiplied background
 * clear), keeping the flat background regions pixel-predictable:
 *
 *  - Zone A (zoneId=1, left)  : identity rig, spin phase 0, OPAQUE
 *    dark-red clear (0.15, 0.03, 0.03, 1).
 *  - Zone B (zoneId=2, right) : ipdFactor 0.6 + perspectiveFactor 0.5 rig,
 *    spin phase +1.5 rad, SEMI-TRANSPARENT dark-blue clear (premultiplied
 *    0.0165, 0.0165, 0.0825, 0.55) — overlap oracle in background regions:
 *    B + A*(1-0.55) per channel.
 *  - Local2D strip (top 25%)  : solid amber clear, always on.
 *
 * Keys / env: M = wish mode (AUTO / explicit Tier-2 rects), O = zone B overlap
 * toggle; DXR_ZONES_WISH_MODE=1 / DXR_ZONES_OVERLAP=1 preselect those states
 * headlessly; DXR_ZONES_VALIDATE=1 chains the validate bit. Without
 * XR_DXR_display_zones the app runs as the plain 2-view SBS cube (the base-app
 * handle path below is intact).
 *
 * The app owns the X event loop (pumped once per frame) and the window
 * lifecycle; the runtime derives its XCB connection from the app's Display
 * (XGetXCBConnection) and presents into the app's window.
 */

#include <X11/Xlib.h> // before the extension header so it sees real Xlib types
#include <X11/Xutil.h> // XSizeHints for INV-1.3 window placement; XLookupKeysym
#include <X11/keysym.h> // XK_m / XK_o for the M/O zone keys

#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_DXR_xlib_window_binding.h>
#include <openxr/XR_DXR_display_info.h>
#include <openxr/XR_DXR_view_rig.h>       // XR_DXR_display_zones prereq (XrDisplayRigDXR)
#include <openxr/XR_DXR_local_3d_zone.h>  // XrLocal3DZoneMaskDXR (wish mask)
#include <openxr/XR_DXR_display_zones.h>

#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <chrono>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

// stb_image implementation TU lives in displayxr::common (stb_image_impl_macos.cpp) — declarations only here (#396 W4).
#include "stb_image.h"

// ============================================================================
// Logging
// ============================================================================

#define LOG_INFO(fmt, ...)  fprintf(stdout, "[INFO]  " fmt "\n", ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  fprintf(stderr, "[WARN]  " fmt "\n", ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) fprintf(stderr, "[ERROR] " fmt "\n", ##__VA_ARGS__)

#define XR_CHECK(call) \
    do { \
        XrResult _r = (call); \
        if (XR_FAILED(_r)) { \
            LOG_ERROR("%s failed: %d", #call, (int)_r); \
            return false; \
        } \
    } while (0)

#define VK_CHECK(call) \
    do { \
        VkResult _r = (call); \
        if (_r != VK_SUCCESS) { \
            LOG_ERROR("%s failed: %d", #call, (int)_r); \
            return false; \
        } \
    } while (0)

// ============================================================================
// XR_DXR_display_zones state (ADR-027) — clear-based zones, see file header
// ============================================================================

static const uint32_t kNumZones = 2;
static const float kZoneVirtualDisplayHeight = 0.30f;

struct DisplayZone {
    uint32_t zoneId = 0;
    XrRect2Di rect = {};            //!< client-window (backing) pixels; locate AND submit use this one variable
    float ipdFactor = 1.0f;
    float perspectiveFactor = 1.0f;
    float spinPhase = 0.0f;         //!< added to the shared cube rotation for this zone
    float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f}; //!< premultiplied RGBA
    XrSwapchain swapchain = XR_NULL_HANDLE;
    uint32_t tileW = 0;
    uint32_t tileH = 0;
    uint32_t tileCount = 0;
    std::vector<VkImage> images;
};
static DisplayZone g_zonesArr[kNumZones];
// Per-zone framebuffer sets are declared after the renderer structs they need
// (see g_zoneFbs below the VkRenderer definition).

static XrRect2Di g_zoneARect, g_zoneBRect, g_zoneBOverlapRect, g_stripRect;
static bool g_zoneBOverlap = false;

// Local2D strip: solid amber clear, filled once.
struct StripLayer {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    uint32_t w = 0, h = 0;
};
static StripLayer g_strip;

static bool g_zonesActive = false;
static bool g_zonesAttempted = false;
static long g_zonesFrameCounter = 0;
static const long kZonesActivationFrame = 10;

// Window backing size (X11 has no Retina scaling, so backing == window size).
// The zone rects are proportions of this. Set once when the window is created.
static uint32_t g_windowW = 0, g_windowH = 0;

// Wish modes (M key): 0 AUTO, 1 explicit Tier-2 rects.
static int g_wishMode = 0;
static bool g_wishModeCycleRequested = false;
static bool g_overlapToggleRequested = false;

static bool g_hasViewRigExt = false;
static bool g_hasLocal3DZoneExt = false;
static bool g_hasDisplayZonesExt = false;
static PFN_xrGetDisplayZoneCapabilitiesDXR g_pfnGetZoneCaps = nullptr;
static PFN_xrGetDisplayZoneRecommendedViewSizeDXR g_pfnGetZoneViewSize = nullptr;
static PFN_xrCreateLocal3DZoneMaskDXR g_pfnCreateZoneMask = nullptr;
static PFN_xrSetLocal3DZoneFromRectsDXR g_pfnSetZoneRects = nullptr;
static PFN_xrDestroyLocal3DZoneMaskDXR g_pfnDestroyZoneMask = nullptr;
static XrLocal3DZoneMaskDXR g_zoneMask = XR_NULL_HANDLE;

static bool ZonesValidateEnabled() {
    static const bool e = []() {
        const char* v = getenv("DXR_ZONES_VALIDATE");
        return v != nullptr && *v == '1';
    }();
    return e;
}

// ============================================================================
// App-owned X11 window (handle class — XR_DXR_xlib_window_binding)
// ============================================================================

static volatile bool g_running = true;

static Display* g_xDisplay = nullptr;
static ::Window g_xWindow = 0;
static Atom g_wmDeleteWindow = 0;

static bool CreateAppWindow(uint32_t width, uint32_t height, int32_t screenLeft, int32_t screenTop) {
    g_xDisplay = XOpenDisplay(nullptr);
    if (g_xDisplay == nullptr) {
        LOG_ERROR("XOpenDisplay failed — is DISPLAY set?");
        return false;
    }

    // INV-1.3: open on the 3D panel (#715). (screenLeft, screenTop) is the
    // panel top-left in virtual-desktop pixels (top-down, origin = primary
    // top-left, XrDisplayDesktopPositionDXR); (0,0) = primary/unknown is a
    // safe create position either way.
    int screen = DefaultScreen(g_xDisplay);
    g_xWindow = XCreateSimpleWindow(
        g_xDisplay, RootWindow(g_xDisplay, screen),
        screenLeft, screenTop, width, height, 0,
        BlackPixel(g_xDisplay, screen), BlackPixel(g_xDisplay, screen));
    if (g_xWindow == 0) {
        LOG_ERROR("XCreateSimpleWindow failed");
        return false;
    }

    // WM_NORMAL_HINTS with USPosition|PPosition, so the window manager treats
    // the create-time position as intentional instead of auto-placing the
    // window (ICCCM §4.1.2.3; GNOME/Mutter auto-places without this). Mirrors
    // the runtime's own hosted-window placement (comp_vk_native_window_xcb.c).
    {
        XSizeHints hints = {};
        hints.flags = USPosition | PPosition;
        hints.x = screenLeft;
        hints.y = screenTop;
        XSetWMNormalHints(g_xDisplay, g_xWindow, &hints);
    }

    XStoreName(g_xDisplay, g_xWindow, "Cube Handle VK (DisplayXR)");
    XSelectInput(g_xDisplay, g_xWindow, StructureNotifyMask | KeyPressMask);

    // Clean close on the window manager's close button.
    g_wmDeleteWindow = XInternAtom(g_xDisplay, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g_xDisplay, g_xWindow, &g_wmDeleteWindow, 1);

    XMapWindow(g_xDisplay, g_xWindow);
    XFlush(g_xDisplay);

    // Re-assert the position after mapping — many WMs (Mutter included)
    // ignore the create-time x/y of a freshly mapped toplevel, but honor a
    // post-map ConfigureRequest (this is what `xdotool windowmove` sends).
    XMoveWindow(g_xDisplay, g_xWindow, screenLeft, screenTop);
    XFlush(g_xDisplay);

    LOG_INFO("Created app-owned X11 window 0x%lx (%ux%u) at (%d, %d)",
             g_xWindow, width, height, screenLeft, screenTop);
    return true;
}

// Pump pending X events. The runtime borrows this Display's connection for
// its XCB surface but never reads events from it, so the app owns the queue.
static void PumpAppWindowEvents() {
    if (g_xDisplay == nullptr) {
        return;
    }
    while (XPending(g_xDisplay) > 0) {
        XEvent ev;
        XNextEvent(g_xDisplay, &ev);
        if (ev.type == ClientMessage &&
            (Atom)ev.xclient.data.l[0] == g_wmDeleteWindow) {
            LOG_INFO("Window closed by user — exiting");
            g_running = false;
        } else if (ev.type == KeyPress) {
            // Zone controls (edge-triggered requests, applied in HandleZoneKeys):
            //   M = cycle wish mode (AUTO / explicit Tier-2 rects)
            //   O = toggle zone B onto/off the overlap rect
            KeySym ks = XLookupKeysym(&ev.xkey, 0);
            if (ks == XK_m || ks == XK_M) {
                g_wishModeCycleRequested = true;
            } else if (ks == XK_o || ks == XK_O) {
                g_overlapToggleRequested = true;
            }
        }
    }
}

// Destroy AFTER the session is torn down — the runtime's VkSurface borrows
// this Display's XCB connection.
static void DestroyAppWindow() {
    if (g_xDisplay != nullptr) {
        if (g_xWindow != 0) {
            XDestroyWindow(g_xDisplay, g_xWindow);
            g_xWindow = 0;
        }
        XCloseDisplay(g_xDisplay);
        g_xDisplay = nullptr;
    }
}

// ============================================================================
// Inline math — column-major float[16] matrices
// ============================================================================
// Column-major layout: m[col*4 + row]
// SPIR-V shaders declare push constants with ColMajor decoration,
// so column-major data is read directly as the matrix M.
// Shader computes gl_Position = M * vec4(pos, 1.0).

static void mat4_identity(float* m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_multiply(float* out, const float* a, const float* b) {
    float tmp[16];
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            tmp[col * 4 + row] = sum;
        }
    }
    memcpy(out, tmp, sizeof(tmp));
}

static void mat4_translation(float* m, float tx, float ty, float tz) {
    mat4_identity(m);
    m[12] = tx;
    m[13] = ty;
    m[14] = tz;
}

static void mat4_scaling(float* m, float sx, float sy, float sz) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = sx;
    m[5] = sy;
    m[10] = sz;
    m[15] = 1.0f;
}

static void mat4_rotation_y(float* m, float angle) {
    float c = cosf(angle), s = sinf(angle);
    mat4_identity(m);
    m[0] = c;
    m[8] = s;
    m[2] = -s;
    m[10] = c;
}

// Asymmetric projection from XrFovf (OpenGL-style infinite-far optional)
static void mat4_from_xr_fov(float* m, const XrFovf& fov, float nearZ, float farZ) {
    float left = nearZ * tanf(fov.angleLeft);
    float right = nearZ * tanf(fov.angleRight);
    float top = nearZ * tanf(fov.angleUp);
    float bottom = nearZ * tanf(fov.angleDown);

    float w = right - left;
    float h = top - bottom;

    memset(m, 0, 16 * sizeof(float));
    // Column-major: m[col*4 + row]
    m[0]  = 2.0f * nearZ / w;                        // col 0, row 0
    m[5]  = 2.0f * nearZ / h;                        // col 1, row 1
    m[8]  = (right + left) / w;                       // col 2, row 0
    m[9]  = (top + bottom) / h;                       // col 2, row 1
    m[10] = -(farZ + nearZ) / (farZ - nearZ);         // col 2, row 2
    m[11] = -1.0f;                                     // col 2, row 3
    m[14] = -2.0f * farZ * nearZ / (farZ - nearZ);    // col 3, row 2
}

// View matrix from XR pose: V = R^T * T(-pos)
static void mat4_view_from_xr_pose(float* m, const XrPosef& pose) {
    float qx = pose.orientation.x;
    float qy = pose.orientation.y;
    float qz = pose.orientation.z;
    float qw = pose.orientation.w;

    // Rotation matrix from quaternion (R)
    float r00 = 1.0f - 2.0f * (qy * qy + qz * qz);
    float r01 = 2.0f * (qx * qy - qz * qw);
    float r02 = 2.0f * (qx * qz + qy * qw);

    float r10 = 2.0f * (qx * qy + qz * qw);
    float r11 = 1.0f - 2.0f * (qx * qx + qz * qz);
    float r12 = 2.0f * (qy * qz - qx * qw);

    float r20 = 2.0f * (qx * qz - qy * qw);
    float r21 = 2.0f * (qy * qz + qx * qw);
    float r22 = 1.0f - 2.0f * (qx * qx + qy * qy);

    // R^T (inverse of rotation) — swap indices
    // R^T[i][j] = R[j][i]
    float px = pose.position.x;
    float py = pose.position.y;
    float pz = pose.position.z;

    // Translation: -R^T * pos
    float tx = -(r00 * px + r10 * py + r20 * pz);
    float ty = -(r01 * px + r11 * py + r21 * pz);
    float tz = -(r02 * px + r12 * py + r22 * pz);

    // Column-major: m[col*4 + row]
    // Row 0: R^T[0][0], R^T[0][1], R^T[0][2], tx
    // Row 1: R^T[1][0], R^T[1][1], R^T[1][2], ty
    // Row 2: R^T[2][0], R^T[2][1], R^T[2][2], tz
    // Row 3: 0,          0,          0,          1
    m[0]  = r00;  m[4]  = r10;  m[8]  = r20;  m[12] = tx;
    m[1]  = r01;  m[5]  = r11;  m[9]  = r21;  m[13] = ty;
    m[2]  = r02;  m[6]  = r12;  m[10] = r22;  m[14] = tz;
    m[3]  = 0.0f; m[7]  = 0.0f; m[11] = 0.0f; m[15] = 1.0f;
}

// Rotate vec3 by quaternion: v' = q * v * q^-1 (ported from cube_zones_vk_macos).
static void quat_rotate_vec3(XrQuaternionf q, float vx, float vy, float vz,
    float* ox, float* oy, float* oz) {
    float tx = 2.0f * (q.y * vz - q.z * vy);
    float ty = 2.0f * (q.z * vx - q.x * vz);
    float tz = 2.0f * (q.x * vy - q.y * vx);
    *ox = vx + q.w * tx + (q.y * tz - q.z * ty);
    *oy = vy + q.w * ty + (q.z * tx - q.x * tz);
    *oz = vz + q.w * tz + (q.x * ty - q.y * tx);
}

// Display-local eye distance for the ZDP-anchored clip (#396 W7 consume path):
// z of (rigPose^-1 * eyeWorld). Degenerates to pose.position.z at identity rig.
static float RigLocalEyeZ(const XrPosef &rig, const XrVector3f &eyeWorld) {
    XrQuaternionf inv = {-rig.orientation.x, -rig.orientation.y,
                         -rig.orientation.z, rig.orientation.w};
    float ox, oy, oz;
    quat_rotate_vec3(inv,
                     eyeWorld.x - rig.position.x,
                     eyeWorld.y - rig.position.y,
                     eyeWorld.z - rig.position.z,
                     &ox, &oy, &oz);
    return oz;
}

// Remap a GL-convention [-1,1] clip-Z projection to Vulkan's [0,1] range
// (newZrow = (Zrow + Wrow)/2). Column-major m[col*4+row]: Z output is row 2,
// W output is row 3. Equivalent to displayxr::common's
// convert_projection_gl_to_zero_to_one (inlined here to keep this app
// self-contained, mirroring the base Linux handle app).
static void convert_projection_gl_to_zero_to_one(float* m) {
    m[2]  = 0.5f * (m[2]  + m[3]);
    m[6]  = 0.5f * (m[6]  + m[7]);
    m[10] = 0.5f * (m[10] + m[11]);
    m[14] = 0.5f * (m[14] + m[15]);
}

// ============================================================================
// Texture path helper — cross-platform executable-relative path
// ============================================================================

static std::string GetTextureDir() {
    char pathBuf[4096];
#ifdef __APPLE__
    uint32_t pathSize = sizeof(pathBuf);
    if (_NSGetExecutablePath(pathBuf, &pathSize) == 0) {
        char* lastSlash = strrchr(pathBuf, '/');
        if (lastSlash) *lastSlash = '\0';
        return std::string(pathBuf) + "/textures/";
    }
#elif defined(_WIN32)
    DWORD len = GetModuleFileNameA(NULL, pathBuf, sizeof(pathBuf));
    if (len > 0 && len < sizeof(pathBuf)) {
        char* lastSlash = strrchr(pathBuf, '\\');
        if (lastSlash) *lastSlash = '\0';
        return std::string(pathBuf) + "\\textures\\";
    }
#else
    ssize_t len = readlink("/proc/self/exe", pathBuf, sizeof(pathBuf) - 1);
    if (len > 0) {
        pathBuf[len] = '\0';
        char* lastSlash = strrchr(pathBuf, '/');
        if (lastSlash) *lastSlash = '\0';
        return std::string(pathBuf) + "/textures/";
    }
#endif
    return "textures/";
}

// ============================================================================
// Embedded SPIR-V shaders (copied from vk_renderer.cpp)
// ============================================================================

// Textured cube vertex shader: push constants (MVP + model), outputs UV/normal/tangent
//   layout(push_constant) uniform PushConstants { mat4 mvp; mat4 model; };
//   in: position(0), color(1), uv(2), normal(3), tangent(4)
//   out: fragUV(0), fragWorldNormal(1), fragWorldTangent(2)
static const uint32_t g_cubeVertSpv[] = {
    0x07230203,0x00010000,0x0008000b,0x00000043,0x00000000,0x00020011,
    0x00000001,0x0006000b,0x00000001,0x4c534c47,0x6474732e,0x3035342e,
    0x00000000,0x0003000e,0x00000000,0x00000001,0x000e000f,0x00000000,
    0x00000004,0x6e69616d,0x00000000,0x0000000d,0x00000019,0x00000025,
    0x00000027,0x00000037,0x00000039,0x0000003c,0x0000003e,0x00000042,
    0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,0x6e69616d,
    0x00000000,0x00060005,0x0000000b,0x505f6c67,0x65567265,0x78657472,
    0x00000000,0x00060006,0x0000000b,0x00000000,0x505f6c67,0x7469736f,
    0x006e6f69,0x00070006,0x0000000b,0x00000001,0x505f6c67,0x746e696f,
    0x657a6953,0x00000000,0x00070006,0x0000000b,0x00000002,0x435f6c67,
    0x4470696c,0x61747369,0x0065636e,0x00070006,0x0000000b,0x00000003,
    0x435f6c67,0x446c6c75,0x61747369,0x0065636e,0x00030005,0x0000000d,
    0x00000000,0x00060005,0x00000011,0x68737550,0x736e6f43,0x746e6174,
    0x00000073,0x00040006,0x00000011,0x00000000,0x0070766d,0x00050006,
    0x00000011,0x00000001,0x65646f6d,0x0000006c,0x00030005,0x00000013,
    0x00000000,0x00050005,0x00000019,0x6f506e69,0x69746973,0x00006e6f,
    0x00040005,0x00000025,0x67617266,0x00005655,0x00040005,0x00000027,
    0x56556e69,0x00000000,0x00050005,0x0000002b,0x6d726f6e,0x614d6c61,
    0x00000074,0x00060005,0x00000037,0x67617266,0x6c726f57,0x726f4e64,
    0x006c616d,0x00050005,0x00000039,0x6f4e6e69,0x6c616d72,0x00000000,
    0x00070005,0x0000003c,0x67617266,0x6c726f57,0x6e615464,0x746e6567,
    0x00000000,0x00050005,0x0000003e,0x61546e69,0x6e65676e,0x00000074,
    0x00040005,0x00000042,0x6f436e69,0x00726f6c,0x00030047,0x0000000b,
    0x00000002,0x00050048,0x0000000b,0x00000000,0x0000000b,0x00000000,
    0x00050048,0x0000000b,0x00000001,0x0000000b,0x00000001,0x00050048,
    0x0000000b,0x00000002,0x0000000b,0x00000003,0x00050048,0x0000000b,
    0x00000003,0x0000000b,0x00000004,0x00030047,0x00000011,0x00000002,
    0x00040048,0x00000011,0x00000000,0x00000005,0x00050048,0x00000011,
    0x00000000,0x00000007,0x00000010,0x00050048,0x00000011,0x00000000,
    0x00000023,0x00000000,0x00040048,0x00000011,0x00000001,0x00000005,
    0x00050048,0x00000011,0x00000001,0x00000007,0x00000010,0x00050048,
    0x00000011,0x00000001,0x00000023,0x00000040,0x00040047,0x00000019,
    0x0000001e,0x00000000,0x00040047,0x00000025,0x0000001e,0x00000000,
    0x00040047,0x00000027,0x0000001e,0x00000002,0x00040047,0x00000037,
    0x0000001e,0x00000001,0x00040047,0x00000039,0x0000001e,0x00000003,
    0x00040047,0x0000003c,0x0000001e,0x00000002,0x00040047,0x0000003e,
    0x0000001e,0x00000004,0x00040047,0x00000042,0x0000001e,0x00000001,
    0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00030016,
    0x00000006,0x00000020,0x00040017,0x00000007,0x00000006,0x00000004,
    0x00040015,0x00000008,0x00000020,0x00000000,0x0004002b,0x00000008,
    0x00000009,0x00000001,0x0004001c,0x0000000a,0x00000006,0x00000009,
    0x0006001e,0x0000000b,0x00000007,0x00000006,0x0000000a,0x0000000a,
    0x00040020,0x0000000c,0x00000003,0x0000000b,0x0004003b,0x0000000c,
    0x0000000d,0x00000003,0x00040015,0x0000000e,0x00000020,0x00000001,
    0x0004002b,0x0000000e,0x0000000f,0x00000000,0x00040018,0x00000010,
    0x00000007,0x00000004,0x0004001e,0x00000011,0x00000010,0x00000010,
    0x00040020,0x00000012,0x00000009,0x00000011,0x0004003b,0x00000012,
    0x00000013,0x00000009,0x00040020,0x00000014,0x00000009,0x00000010,
    0x00040017,0x00000017,0x00000006,0x00000003,0x00040020,0x00000018,
    0x00000001,0x00000017,0x0004003b,0x00000018,0x00000019,0x00000001,
    0x0004002b,0x00000006,0x0000001b,0x3f800000,0x00040020,0x00000021,
    0x00000003,0x00000007,0x00040017,0x00000023,0x00000006,0x00000002,
    0x00040020,0x00000024,0x00000003,0x00000023,0x0004003b,0x00000024,
    0x00000025,0x00000003,0x00040020,0x00000026,0x00000001,0x00000023,
    0x0004003b,0x00000026,0x00000027,0x00000001,0x00040018,0x00000029,
    0x00000017,0x00000003,0x00040020,0x0000002a,0x00000007,0x00000029,
    0x0004002b,0x0000000e,0x0000002c,0x00000001,0x00040020,0x00000036,
    0x00000003,0x00000017,0x0004003b,0x00000036,0x00000037,0x00000003,
    0x0004003b,0x00000018,0x00000039,0x00000001,0x0004003b,0x00000036,
    0x0000003c,0x00000003,0x0004003b,0x00000018,0x0000003e,0x00000001,
    0x00040020,0x00000041,0x00000001,0x00000007,0x0004003b,0x00000041,
    0x00000042,0x00000001,0x00050036,0x00000002,0x00000004,0x00000000,
    0x00000003,0x000200f8,0x00000005,0x0004003b,0x0000002a,0x0000002b,
    0x00000007,0x00050041,0x00000014,0x00000015,0x00000013,0x0000000f,
    0x0004003d,0x00000010,0x00000016,0x00000015,0x0004003d,0x00000017,
    0x0000001a,0x00000019,0x00050051,0x00000006,0x0000001c,0x0000001a,
    0x00000000,0x00050051,0x00000006,0x0000001d,0x0000001a,0x00000001,
    0x00050051,0x00000006,0x0000001e,0x0000001a,0x00000002,0x00070050,
    0x00000007,0x0000001f,0x0000001c,0x0000001d,0x0000001e,0x0000001b,
    0x00050091,0x00000007,0x00000020,0x00000016,0x0000001f,0x00050041,
    0x00000021,0x00000022,0x0000000d,0x0000000f,0x0003003e,0x00000022,
    0x00000020,0x0004003d,0x00000023,0x00000028,0x00000027,0x0003003e,
    0x00000025,0x00000028,0x00050041,0x00000014,0x0000002d,0x00000013,
    0x0000002c,0x0004003d,0x00000010,0x0000002e,0x0000002d,0x00050051,
    0x00000007,0x0000002f,0x0000002e,0x00000000,0x0008004f,0x00000017,
    0x00000030,0x0000002f,0x0000002f,0x00000000,0x00000001,0x00000002,
    0x00050051,0x00000007,0x00000031,0x0000002e,0x00000001,0x0008004f,
    0x00000017,0x00000032,0x00000031,0x00000031,0x00000000,0x00000001,
    0x00000002,0x00050051,0x00000007,0x00000033,0x0000002e,0x00000002,
    0x0008004f,0x00000017,0x00000034,0x00000033,0x00000033,0x00000000,
    0x00000001,0x00000002,0x00060050,0x00000029,0x00000035,0x00000030,
    0x00000032,0x00000034,0x0003003e,0x0000002b,0x00000035,0x0004003d,
    0x00000029,0x00000038,0x0000002b,0x0004003d,0x00000017,0x0000003a,
    0x00000039,0x00050091,0x00000017,0x0000003b,0x00000038,0x0000003a,
    0x0003003e,0x00000037,0x0000003b,0x0004003d,0x00000029,0x0000003d,
    0x0000002b,0x0004003d,0x00000017,0x0000003f,0x0000003e,0x00050091,
    0x00000017,0x00000040,0x0000003d,0x0000003f,0x0003003e,0x0000003c,
    0x00000040,0x000100fd,0x00010038,
};

// Textured cube fragment shader: samples basecolor, normal, AO textures with directional lighting
//   binding 0: basecolor sampler, binding 1: normal sampler, binding 2: AO sampler
//   in: fragUV(0), fragWorldNormal(1), fragWorldTangent(2)
//   Constructs TBN matrix, applies normal mapping, directional light with AO
static const uint32_t g_cubeFragSpv[] = {
    0x07230203,0x00010000,0x0008000b,0x00000071,0x00000000,0x00020011,
    0x00000001,0x0006000b,0x00000001,0x4c534c47,0x6474732e,0x3035342e,
    0x00000000,0x0003000e,0x00000000,0x00000001,0x0009000f,0x00000004,
    0x00000004,0x6e69616d,0x00000000,0x00000011,0x00000027,0x0000002b,
    0x00000067,0x00030010,0x00000004,0x00000007,0x00030003,0x00000002,
    0x000001c2,0x00040005,0x00000004,0x6e69616d,0x00000000,0x00050005,
    0x00000009,0x65736162,0x6f6c6f63,0x00000072,0x00060005,0x0000000d,
    0x65736162,0x6f6c6f63,0x78655472,0x00000000,0x00040005,0x00000011,
    0x67617266,0x00005655,0x00050005,0x00000016,0x6d726f6e,0x614d6c61,
    0x00000070,0x00050005,0x00000017,0x6d726f6e,0x65546c61,0x00000078,
    0x00030005,0x0000001d,0x00006f61,0x00040005,0x0000001e,0x65546f61,
    0x00000078,0x00030005,0x00000025,0x0000004e,0x00060005,0x00000027,
    0x67617266,0x6c726f57,0x726f4e64,0x006c616d,0x00030005,0x0000002a,
    0x00000054,0x00070005,0x0000002b,0x67617266,0x6c726f57,0x6e615464,
    0x746e6567,0x00000000,0x00030005,0x00000036,0x00000042,0x00030005,
    0x0000003c,0x004e4254,0x00060005,0x0000004f,0x7070616d,0x6f4e6465,
    0x6c616d72,0x00000000,0x00050005,0x00000058,0x6867696c,0x72694474,
    0x00000000,0x00040005,0x0000005d,0x66666964,0x00657375,0x00050005,
    0x00000067,0x4374756f,0x726f6c6f,0x00000000,0x00040047,0x0000000d,
    0x00000021,0x00000000,0x00040047,0x0000000d,0x00000022,0x00000000,
    0x00040047,0x00000011,0x0000001e,0x00000000,0x00040047,0x00000017,
    0x00000021,0x00000001,0x00040047,0x00000017,0x00000022,0x00000000,
    0x00040047,0x0000001e,0x00000021,0x00000002,0x00040047,0x0000001e,
    0x00000022,0x00000000,0x00040047,0x00000027,0x0000001e,0x00000001,
    0x00040047,0x0000002b,0x0000001e,0x00000002,0x00040047,0x00000067,
    0x0000001e,0x00000000,0x00020013,0x00000002,0x00030021,0x00000003,
    0x00000002,0x00030016,0x00000006,0x00000020,0x00040017,0x00000007,
    0x00000006,0x00000003,0x00040020,0x00000008,0x00000007,0x00000007,
    0x00090019,0x0000000a,0x00000006,0x00000001,0x00000000,0x00000000,
    0x00000000,0x00000001,0x00000000,0x0003001b,0x0000000b,0x0000000a,
    0x00040020,0x0000000c,0x00000000,0x0000000b,0x0004003b,0x0000000c,
    0x0000000d,0x00000000,0x00040017,0x0000000f,0x00000006,0x00000002,
    0x00040020,0x00000010,0x00000001,0x0000000f,0x0004003b,0x00000010,
    0x00000011,0x00000001,0x00040017,0x00000013,0x00000006,0x00000004,
    0x0004003b,0x0000000c,0x00000017,0x00000000,0x00040020,0x0000001c,
    0x00000007,0x00000006,0x0004003b,0x0000000c,0x0000001e,0x00000000,
    0x00040015,0x00000022,0x00000020,0x00000000,0x0004002b,0x00000022,
    0x00000023,0x00000000,0x00040020,0x00000026,0x00000001,0x00000007,
    0x0004003b,0x00000026,0x00000027,0x00000001,0x0004003b,0x00000026,
    0x0000002b,0x00000001,0x00040018,0x0000003a,0x00000007,0x00000003,
    0x00040020,0x0000003b,0x00000007,0x0000003a,0x0004002b,0x00000006,
    0x00000040,0x3f800000,0x0004002b,0x00000006,0x00000041,0x00000000,
    0x0004002b,0x00000006,0x00000052,0x40000000,0x0004002b,0x00000006,
    0x00000059,0x3e9b28d0,0x0004002b,0x00000006,0x0000005a,0x3f4ee116,
    0x0004002b,0x00000006,0x0000005b,0x3f014cae,0x0006002c,0x00000007,
    0x0000005c,0x00000059,0x0000005a,0x0000005b,0x0004002b,0x00000006,
    0x00000062,0x3f333333,0x0004002b,0x00000006,0x00000064,0x3e99999a,
    0x00040020,0x00000066,0x00000003,0x00000013,0x0004003b,0x00000066,
    0x00000067,0x00000003,0x00050036,0x00000002,0x00000004,0x00000000,
    0x00000003,0x000200f8,0x00000005,0x0004003b,0x00000008,0x00000009,
    0x00000007,0x0004003b,0x00000008,0x00000016,0x00000007,0x0004003b,
    0x0000001c,0x0000001d,0x00000007,0x0004003b,0x00000008,0x00000025,
    0x00000007,0x0004003b,0x00000008,0x0000002a,0x00000007,0x0004003b,
    0x00000008,0x00000036,0x00000007,0x0004003b,0x0000003b,0x0000003c,
    0x00000007,0x0004003b,0x00000008,0x0000004f,0x00000007,0x0004003b,
    0x00000008,0x00000058,0x00000007,0x0004003b,0x0000001c,0x0000005d,
    0x00000007,0x0004003d,0x0000000b,0x0000000e,0x0000000d,0x0004003d,
    0x0000000f,0x00000012,0x00000011,0x00050057,0x00000013,0x00000014,
    0x0000000e,0x00000012,0x0008004f,0x00000007,0x00000015,0x00000014,
    0x00000014,0x00000000,0x00000001,0x00000002,0x0003003e,0x00000009,
    0x00000015,0x0004003d,0x0000000b,0x00000018,0x00000017,0x0004003d,
    0x0000000f,0x00000019,0x00000011,0x00050057,0x00000013,0x0000001a,
    0x00000018,0x00000019,0x0008004f,0x00000007,0x0000001b,0x0000001a,
    0x0000001a,0x00000000,0x00000001,0x00000002,0x0003003e,0x00000016,
    0x0000001b,0x0004003d,0x0000000b,0x0000001f,0x0000001e,0x0004003d,
    0x0000000f,0x00000020,0x00000011,0x00050057,0x00000013,0x00000021,
    0x0000001f,0x00000020,0x00050051,0x00000006,0x00000024,0x00000021,
    0x00000000,0x0003003e,0x0000001d,0x00000024,0x0004003d,0x00000007,
    0x00000028,0x00000027,0x0006000c,0x00000007,0x00000029,0x00000001,
    0x00000045,0x00000028,0x0003003e,0x00000025,0x00000029,0x0004003d,
    0x00000007,0x0000002c,0x0000002b,0x0006000c,0x00000007,0x0000002d,
    0x00000001,0x00000045,0x0000002c,0x0003003e,0x0000002a,0x0000002d,
    0x0004003d,0x00000007,0x0000002e,0x0000002a,0x0004003d,0x00000007,
    0x0000002f,0x0000002a,0x0004003d,0x00000007,0x00000030,0x00000025,
    0x00050094,0x00000006,0x00000031,0x0000002f,0x00000030,0x0004003d,
    0x00000007,0x00000032,0x00000025,0x0005008e,0x00000007,0x00000033,
    0x00000032,0x00000031,0x00050083,0x00000007,0x00000034,0x0000002e,
    0x00000033,0x0006000c,0x00000007,0x00000035,0x00000001,0x00000045,
    0x00000034,0x0003003e,0x0000002a,0x00000035,0x0004003d,0x00000007,
    0x00000037,0x00000025,0x0004003d,0x00000007,0x00000038,0x0000002a,
    0x0007000c,0x00000007,0x00000039,0x00000001,0x00000044,0x00000037,
    0x00000038,0x0003003e,0x00000036,0x00000039,0x0004003d,0x00000007,
    0x0000003d,0x0000002a,0x0004003d,0x00000007,0x0000003e,0x00000036,
    0x0004003d,0x00000007,0x0000003f,0x00000025,0x00050051,0x00000006,
    0x00000042,0x0000003d,0x00000000,0x00050051,0x00000006,0x00000043,
    0x0000003d,0x00000001,0x00050051,0x00000006,0x00000044,0x0000003d,
    0x00000002,0x00050051,0x00000006,0x00000045,0x0000003e,0x00000000,
    0x00050051,0x00000006,0x00000046,0x0000003e,0x00000001,0x00050051,
    0x00000006,0x00000047,0x0000003e,0x00000002,0x00050051,0x00000006,
    0x00000048,0x0000003f,0x00000000,0x00050051,0x00000006,0x00000049,
    0x0000003f,0x00000001,0x00050051,0x00000006,0x0000004a,0x0000003f,
    0x00000002,0x00060050,0x00000007,0x0000004b,0x00000042,0x00000043,
    0x00000044,0x00060050,0x00000007,0x0000004c,0x00000045,0x00000046,
    0x00000047,0x00060050,0x00000007,0x0000004d,0x00000048,0x00000049,
    0x0000004a,0x00060050,0x0000003a,0x0000004e,0x0000004b,0x0000004c,
    0x0000004d,0x0003003e,0x0000003c,0x0000004e,0x0004003d,0x0000003a,
    0x00000050,0x0000003c,0x0004003d,0x00000007,0x00000051,0x00000016,
    0x0005008e,0x00000007,0x00000053,0x00000051,0x00000052,0x00060050,
    0x00000007,0x00000054,0x00000040,0x00000040,0x00000040,0x00050083,
    0x00000007,0x00000055,0x00000053,0x00000054,0x00050091,0x00000007,
    0x00000056,0x00000050,0x00000055,0x0006000c,0x00000007,0x00000057,
    0x00000001,0x00000045,0x00000056,0x0003003e,0x0000004f,0x00000057,
    0x0003003e,0x00000058,0x0000005c,0x0004003d,0x00000007,0x0000005e,
    0x0000004f,0x0004003d,0x00000007,0x0000005f,0x00000058,0x00050094,
    0x00000006,0x00000060,0x0000005e,0x0000005f,0x0007000c,0x00000006,
    0x00000061,0x00000001,0x00000028,0x00000060,0x00000041,0x00050085,
    0x00000006,0x00000063,0x00000061,0x00000062,0x00050081,0x00000006,
    0x00000065,0x00000063,0x00000064,0x0003003e,0x0000005d,0x00000065,
    0x0004003d,0x00000007,0x00000068,0x00000009,0x0004003d,0x00000006,
    0x00000069,0x0000001d,0x0005008e,0x00000007,0x0000006a,0x00000068,
    0x00000069,0x0004003d,0x00000006,0x0000006b,0x0000005d,0x0005008e,
    0x00000007,0x0000006c,0x0000006a,0x0000006b,0x00050051,0x00000006,
    0x0000006d,0x0000006c,0x00000000,0x00050051,0x00000006,0x0000006e,
    0x0000006c,0x00000001,0x00050051,0x00000006,0x0000006f,0x0000006c,
    0x00000002,0x00070050,0x00000013,0x00000070,0x0000006d,0x0000006e,
    0x0000006f,0x00000040,0x0003003e,0x00000067,0x00000070,0x000100fd,
    0x00010038,
};

// Grid vertex shader:
//   layout(push_constant) uniform PC { mat4 transform; vec4 color; } pc;
//   layout(location=0) in vec3 aPos;
//   void main() { gl_Position = pc.transform * vec4(aPos, 1.0); }
static const uint32_t g_gridVertSpv[] = {
    0x07230203, 0x00010000, 0x000d000a, 0x00000023,
    0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000000, 0x00000004, 0x6e69616d,
    0x00000000, 0x0000000d, 0x00000017, 0x00030003,
    0x00000002, 0x000001c2, 0x00040005, 0x00000004,
    0x6e69616d, 0x00000000, 0x00060005, 0x0000000b,
    0x505f6c67, 0x65567265, 0x78657472, 0x00000000,
    0x00060006, 0x0000000b, 0x00000000, 0x505f6c67,
    0x7469736f, 0x006e6f69, 0x00030005, 0x0000000d,
    0x00000000, 0x00040005, 0x0000000f, 0x00006350,
    0x00000000, 0x00060006, 0x0000000f, 0x00000000,
    0x6e617274, 0x726f6673, 0x0000006d, 0x00050006,
    0x0000000f, 0x00000001, 0x6f6c6f63, 0x00000072,
    0x00030005, 0x00000011, 0x00006370, 0x00040005,
    0x00000017, 0x736f5061, 0x00000000, 0x00050048,
    0x0000000b, 0x00000000, 0x0000000b, 0x00000000,
    0x00030047, 0x0000000b, 0x00000002, 0x00040048,
    0x0000000f, 0x00000000, 0x00000005, 0x00050048,
    0x0000000f, 0x00000000, 0x00000023, 0x00000000,
    0x00050048, 0x0000000f, 0x00000000, 0x00000007,
    0x00000010, 0x00050048, 0x0000000f, 0x00000001,
    0x00000023, 0x00000040, 0x00030047, 0x0000000f,
    0x00000002, 0x00040047, 0x00000017, 0x0000001e,
    0x00000000, 0x00020013, 0x00000002, 0x00030021,
    0x00000003, 0x00000002, 0x00030016, 0x00000006,
    0x00000020, 0x00040017, 0x00000007, 0x00000006,
    0x00000004, 0x0003001e, 0x0000000b, 0x00000007,
    0x00040020, 0x0000000c, 0x00000003, 0x0000000b,
    0x0004003b, 0x0000000c, 0x0000000d, 0x00000003,
    0x00040015, 0x0000000e, 0x00000020, 0x00000001,
    0x00040018, 0x00000010, 0x00000007, 0x00000004,
    0x0004001e, 0x0000000f, 0x00000010, 0x00000007,
    0x00040020, 0x00000012, 0x00000009, 0x0000000f,
    0x0004003b, 0x00000012, 0x00000011, 0x00000009,
    0x0004002b, 0x0000000e, 0x00000013, 0x00000000,
    0x00040020, 0x00000014, 0x00000009, 0x00000010,
    0x00040017, 0x00000016, 0x00000006, 0x00000003,
    0x00040020, 0x00000018, 0x00000001, 0x00000016,
    0x0004003b, 0x00000018, 0x00000017, 0x00000001,
    0x0004002b, 0x00000006, 0x0000001a, 0x3f800000,
    0x00040020, 0x0000001e, 0x00000003, 0x00000007,
    0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200f8, 0x00000005, 0x00050041,
    0x00000014, 0x00000015, 0x00000011, 0x00000013,
    0x0004003d, 0x00000010, 0x00000019, 0x00000015,
    0x0004003d, 0x00000016, 0x0000001b, 0x00000017,
    0x00050051, 0x00000006, 0x0000001c, 0x0000001b,
    0x00000000, 0x00050051, 0x00000006, 0x0000001d,
    0x0000001b, 0x00000001, 0x00050051, 0x00000006,
    0x0000001f, 0x0000001b, 0x00000002, 0x00070050,
    0x00000007, 0x00000022, 0x0000001c, 0x0000001d,
    0x0000001f, 0x0000001a, 0x00050091, 0x00000007,
    0x00000020, 0x00000019, 0x00000022, 0x00050041,
    0x0000001e, 0x00000021, 0x0000000d, 0x00000013,
    0x0003003e, 0x00000021, 0x00000020, 0x000100fd,
    0x00010038,
};

// Grid fragment shader:
//   layout(push_constant) uniform PC { mat4 transform; vec4 color; } pc;
//   layout(location=0) out vec4 FragColor;
//   void main() { FragColor = pc.color; }
static const uint32_t g_gridFragSpv[] = {
    0x07230203, 0x00010000, 0x000d000a, 0x00000013,
    0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0006000f, 0x00000004, 0x00000004, 0x6e69616d,
    0x00000000, 0x00000009, 0x00030003, 0x00000002,
    0x000001c2, 0x00040005, 0x00000004, 0x6e69616d,
    0x00000000, 0x00050005, 0x00000009, 0x67617246,
    0x6f6c6f43, 0x00000072, 0x00040005, 0x0000000b,
    0x00006350, 0x00000000, 0x00060006, 0x0000000b,
    0x00000000, 0x6e617274, 0x726f6673, 0x0000006d,
    0x00050006, 0x0000000b, 0x00000001, 0x6f6c6f63,
    0x00000072, 0x00030005, 0x0000000d, 0x00006370,
    0x00040047, 0x00000009, 0x0000001e, 0x00000000,
    0x00040048, 0x0000000b, 0x00000000, 0x00000005,
    0x00050048, 0x0000000b, 0x00000000, 0x00000023,
    0x00000000, 0x00050048, 0x0000000b, 0x00000000,
    0x00000007, 0x00000010, 0x00050048, 0x0000000b,
    0x00000001, 0x00000023, 0x00000040, 0x00030047,
    0x0000000b, 0x00000002, 0x00020013, 0x00000002,
    0x00030021, 0x00000003, 0x00000002, 0x00030016,
    0x00000006, 0x00000020, 0x00040017, 0x00000007,
    0x00000006, 0x00000004, 0x00040020, 0x00000008,
    0x00000003, 0x00000007, 0x0004003b, 0x00000008,
    0x00000009, 0x00000003, 0x00040018, 0x0000000a,
    0x00000007, 0x00000004, 0x0004001e, 0x0000000b,
    0x0000000a, 0x00000007, 0x00040020, 0x0000000c,
    0x00000009, 0x0000000b, 0x0004003b, 0x0000000c,
    0x0000000d, 0x00000009, 0x00040015, 0x0000000e,
    0x00000020, 0x00000001, 0x0004002b, 0x0000000e,
    0x0000000f, 0x00000001, 0x00040020, 0x00000010,
    0x00000009, 0x00000007, 0x00050036, 0x00000002,
    0x00000004, 0x00000000, 0x00000003, 0x000200f8,
    0x00000005, 0x00050041, 0x00000010, 0x00000011,
    0x0000000d, 0x0000000f, 0x0004003d, 0x00000007,
    0x00000012, 0x00000011,
    0x0003003e, 0x00000009, 0x00000012, 0x000100fd,
    0x00010038,
};

// ============================================================================
// Vulkan helpers (from vk_renderer.cpp)
// ============================================================================

static uint32_t FindMemoryType(VkPhysicalDevice physDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

static bool CreateBuffer(VkDevice device, VkPhysicalDevice physDevice,
    VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memProps,
    VkBuffer& buffer, VkDeviceMemory& memory)
{
    VkBufferCreateInfo bufferInfo = {};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, buffer, &memReqs);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(physDevice, memReqs.memoryTypeBits, memProps);

    if (allocInfo.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }

    vkBindBufferMemory(device, buffer, memory, 0);
    return true;
}

static VkShaderModule CreateShaderModule(VkDevice device, const uint32_t* code, size_t codeSize) {
    VkShaderModuleCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = codeSize;
    createInfo.pCode = code;

    VkShaderModule module;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &module) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return module;
}

// Load a texture from disk, create VkImage + VkImageView, upload via staging buffer.
// If the file can't be loaded, creates a 1x1 fallback (white for basecolor/AO, flat blue for normal).
static bool CreateTextureFromFile(VkDevice device, VkPhysicalDevice physDevice,
    VkCommandPool cmdPool, VkQueue queue,
    const char* path, bool isNormalMap,
    VkImage& outImage, VkDeviceMemory& outMemory, VkImageView& outView)
{
    int w, h, channels;
    unsigned char* pixels = stbi_load(path, &w, &h, &channels, 4); // force RGBA
    bool fallback = false;
    if (!pixels) {
        LOG_WARN("Failed to load texture: %s — using fallback", path);
        w = h = 1;
        static unsigned char whitePixel[] = {255, 255, 255, 255};
        static unsigned char bluePixel[] = {128, 128, 255, 255}; // flat normal
        pixels = isNormalMap ? bluePixel : whitePixel;
        fallback = true;
    }

    VkDeviceSize imageSize = (VkDeviceSize)w * h * 4;

    // Staging buffer
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;
    VkBufferCreateInfo bufInfo = {};
    bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufInfo.size = imageSize;
    bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device, &bufInfo, nullptr, &stagingBuffer);

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, stagingBuffer, &memReqs);
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(physDevice, memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(device, &allocInfo, nullptr, &stagingMemory);
    vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0);

    void* data;
    vkMapMemory(device, stagingMemory, 0, imageSize, 0, &data);
    memcpy(data, pixels, (size_t)imageSize);
    vkUnmapMemory(device, stagingMemory);
    if (!fallback) stbi_image_free(pixels);

    // Create image
    VkImageCreateInfo imgInfo = {};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imgInfo.extent = {(uint32_t)w, (uint32_t)h, 1};
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(device, &imgInfo, nullptr, &outImage);

    vkGetImageMemoryRequirements(device, outImage, &memReqs);
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(physDevice, memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device, &allocInfo, nullptr, &outMemory);
    vkBindImageMemory(device, outImage, outMemory, 0);

    // Copy staging -> image via one-shot command buffer
    VkCommandBufferAllocateInfo cmdAlloc = {};
    cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAlloc.commandPool = cmdPool;
    cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &cmdAlloc, &cmd);

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // Transition UNDEFINED -> TRANSFER_DST
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = outImage;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyBufferToImage(cmd, stagingBuffer, outImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition TRANSFER_DST -> SHADER_READ_ONLY
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(device, cmdPool, 1, &cmd);

    // Cleanup staging
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vkFreeMemory(device, stagingMemory, nullptr);

    // Create image view
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = outImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(device, &viewInfo, nullptr, &outView);

    return !fallback;
}

// ============================================================================
// Vertex types + renderer struct
// ============================================================================

struct CubeVertex { float pos[3]; float color[4]; float uv[2]; float normal[3]; float tangent[3]; };
struct GridVertex { float pos[3]; };

struct PushConstants {
    float transform[16];
    float color[4];
};

struct EyeRenderParams {
    uint32_t viewportX, viewportY, width, height;
    float viewMat[16];
    float projMat[16];
};

// A framebuffer set over one swapchain (the main SBS swapchain OR a zone
// swapchain). Each set owns its own depth image so zones can be rendered into
// independently (ported from cube_zones_vk_macos's multi-target refactor).
struct SwapchainFramebuffers {
    std::vector<VkImageView> colorViews;
    std::vector<VkImageView> depthViews;
    std::vector<VkFramebuffer> framebuffers;
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct VkRenderer {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex = 0;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;        // grid: push constants only (80 bytes)
    VkPipelineLayout cubePipelineLayout = VK_NULL_HANDLE;    // textured cube: descriptor set + push constants (128 bytes)
    VkPipeline cubePipeline = VK_NULL_HANDLE;
    VkPipeline gridPipeline = VK_NULL_HANDLE;

    VkBuffer cubeVertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory cubeVertexMemory = VK_NULL_HANDLE;
    VkBuffer cubeIndexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory cubeIndexMemory = VK_NULL_HANDLE;

    VkBuffer gridVertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory gridVertexMemory = VK_NULL_HANDLE;
    int gridVertexCount = 0;

    // Framebuffer set over the main SBS swapchain (zones use g_zoneFbs).
    SwapchainFramebuffers swapchainFBs;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkFence frameFence = VK_NULL_HANDLE;

    float cubeRotation = 0.0f;

    // Texture resources
    VkImage texImages[3] = {};          // basecolor, normal, AO
    VkDeviceMemory texMemory[3] = {};
    VkImageView texViews[3] = {};
    VkSampler texSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    bool texturesLoaded = false;
};

//! XR_DXR_display_zones: per-zone framebuffer sets over the zone swapchains
//! (the zones render the same cube + grid scene as the main path).
static SwapchainFramebuffers g_zoneFbs[kNumZones];

// ============================================================================
// Renderer init / framebuffers / cleanup (ported from vk_renderer.cpp)
// ============================================================================

static bool InitializeVkRenderer(VkRenderer& renderer, VkDevice device, VkPhysicalDevice physDevice,
    VkQueue queue, uint32_t queueFamilyIndex, VkFormat colorFormat)
{
    renderer.device = device;
    renderer.physicalDevice = physDevice;
    renderer.graphicsQueue = queue;
    renderer.queueFamilyIndex = queueFamilyIndex;

    // Render pass
    {
        VkAttachmentDescription colorAttach = {};
        colorAttach.format = colorFormat;
        colorAttach.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttach.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentDescription depthAttach = {};
        depthAttach.format = VK_FORMAT_D32_SFLOAT;
        depthAttach.samples = VK_SAMPLE_COUNT_1_BIT;
        depthAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttach.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttach.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentDescription attachments[] = {colorAttach, depthAttach};

        VkAttachmentReference colorRef = {};
        colorRef.attachment = 0;
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depthRef = {};
        depthRef.attachment = 1;
        depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;
        subpass.pDepthStencilAttachment = &depthRef;

        VkRenderPassCreateInfo rpInfo = {};
        rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = 2;
        rpInfo.pAttachments = attachments;
        rpInfo.subpassCount = 1;
        rpInfo.pSubpasses = &subpass;

        if (vkCreateRenderPass(device, &rpInfo, nullptr, &renderer.renderPass) != VK_SUCCESS) {
            LOG_ERROR("Failed to create render pass");
            return false;
        }
    }

    // Grid pipeline layout (push constants only, 80 bytes: mat4 + vec4)
    {
        VkPushConstantRange pushRange = {};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(PushConstants);

        VkPipelineLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;

        if (vkCreatePipelineLayout(device, &layoutInfo, nullptr, &renderer.pipelineLayout) != VK_SUCCESS) {
            LOG_ERROR("Failed to create grid pipeline layout");
            return false;
        }
    }

    // Descriptor set layout for textured cube (3 combined image samplers)
    {
        VkDescriptorSetLayoutBinding bindings[3] = {};
        for (int i = 0; i < 3; i++) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dslInfo = {};
        dslInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslInfo.bindingCount = 3;
        dslInfo.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &dslInfo, nullptr, &renderer.descriptorSetLayout) != VK_SUCCESS) {
            LOG_ERROR("Failed to create descriptor set layout");
            return false;
        }
    }

    // Cube pipeline layout (descriptor set + 128 bytes push constants: MVP + model)
    {
        VkPushConstantRange cubePushRange = {};
        cubePushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        cubePushRange.offset = 0;
        cubePushRange.size = 128; // MVP + model matrices

        VkPipelineLayoutCreateInfo cubeLayoutInfo = {};
        cubeLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        cubeLayoutInfo.setLayoutCount = 1;
        cubeLayoutInfo.pSetLayouts = &renderer.descriptorSetLayout;
        cubeLayoutInfo.pushConstantRangeCount = 1;
        cubeLayoutInfo.pPushConstantRanges = &cubePushRange;

        if (vkCreatePipelineLayout(device, &cubeLayoutInfo, nullptr, &renderer.cubePipelineLayout) != VK_SUCCESS) {
            LOG_ERROR("Failed to create cube pipeline layout");
            return false;
        }
    }

    // Shader modules
    VkShaderModule cubeVert = CreateShaderModule(device, g_cubeVertSpv, sizeof(g_cubeVertSpv));
    VkShaderModule cubeFrag = CreateShaderModule(device, g_cubeFragSpv, sizeof(g_cubeFragSpv));
    VkShaderModule gridVert = CreateShaderModule(device, g_gridVertSpv, sizeof(g_gridVertSpv));
    VkShaderModule gridFrag = CreateShaderModule(device, g_gridFragSpv, sizeof(g_gridFragSpv));

    if (!cubeVert || !cubeFrag || !gridVert || !gridFrag) {
        LOG_ERROR("Failed to create shader modules");
        return false;
    }

    // Cube pipeline
    {
        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = cubeVert;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = cubeFrag;
        stages[1].pName = "main";

        VkVertexInputBindingDescription binding = {};
        binding.binding = 0;
        binding.stride = sizeof(CubeVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription attrs[5] = {};
        attrs[0].location = 0; attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[0].offset = offsetof(CubeVertex, pos);
        attrs[1].location = 1; attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT; attrs[1].offset = offsetof(CubeVertex, color);
        attrs[2].location = 2; attrs[2].format = VK_FORMAT_R32G32_SFLOAT; attrs[2].offset = offsetof(CubeVertex, uv);
        attrs[3].location = 3; attrs[3].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[3].offset = offsetof(CubeVertex, normal);
        attrs[4].location = 4; attrs[4].format = VK_FORMAT_R32G32B32_SFLOAT; attrs[4].offset = offsetof(CubeVertex, tangent);

        VkPipelineVertexInputStateCreateInfo vertexInput = {};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 5;
        vertexInput.pVertexAttributeDescriptions = attrs;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState = {};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer = {};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_NONE;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

        VkPipelineMultisampleStateCreateInfo multisampling = {};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil = {};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

        VkPipelineColorBlendAttachmentState colorBlendAttach = {};
        colorBlendAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo colorBlend = {};
        colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = &colorBlendAttach;

        VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamicState = {};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = 2;
        dynamicState.pDynamicStates = dynamicStates;

        VkGraphicsPipelineCreateInfo pipelineInfo = {};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = renderer.cubePipelineLayout;
        pipelineInfo.renderPass = renderer.renderPass;
        pipelineInfo.subpass = 0;

        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &renderer.cubePipeline) != VK_SUCCESS) {
            LOG_ERROR("Failed to create cube pipeline");
            return false;
        }
    }

    // Grid pipeline (line list, no cull)
    {
        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = gridVert;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = gridFrag;
        stages[1].pName = "main";

        VkVertexInputBindingDescription binding = {};
        binding.binding = 0;
        binding.stride = sizeof(GridVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription attr = {};
        attr.location = 0;
        attr.binding = 0;
        attr.format = VK_FORMAT_R32G32B32_SFLOAT;
        attr.offset = 0;

        VkPipelineVertexInputStateCreateInfo vertexInput = {};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = 1;
        vertexInput.pVertexAttributeDescriptions = &attr;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

        VkPipelineViewportStateCreateInfo viewportState = {};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer = {};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_NONE;

        VkPipelineMultisampleStateCreateInfo multisampling = {};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil = {};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

        VkPipelineColorBlendAttachmentState colorBlendAttach = {};
        colorBlendAttach.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo colorBlend = {};
        colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = &colorBlendAttach;

        VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamicState = {};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = 2;
        dynamicState.pDynamicStates = dynamicStates;

        VkGraphicsPipelineCreateInfo pipelineInfo = {};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = stages;
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = renderer.pipelineLayout;
        pipelineInfo.renderPass = renderer.renderPass;
        pipelineInfo.subpass = 0;

        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &renderer.gridPipeline) != VK_SUCCESS) {
            LOG_ERROR("Failed to create grid pipeline");
            return false;
        }
    }

    vkDestroyShaderModule(device, cubeVert, nullptr);
    vkDestroyShaderModule(device, cubeFrag, nullptr);
    vkDestroyShaderModule(device, gridVert, nullptr);
    vkDestroyShaderModule(device, gridFrag, nullptr);

    // Vertex/index buffers — textured cube with UV, normal, tangent per vertex
    //   pos[3], color[4], uv[2], normal[3], tangent[3] = 15 floats per vertex
    CubeVertex cubeVerts[] = {
        // Front face (-Z): normal (0,0,-1), tangent (1,0,0)
        {{-0.5f,-0.5f,-0.5f},{1,1,1,1},{0,1},{0,0,-1},{1,0,0}},
        {{-0.5f, 0.5f,-0.5f},{1,1,1,1},{0,0},{0,0,-1},{1,0,0}},
        {{ 0.5f, 0.5f,-0.5f},{1,1,1,1},{1,0},{0,0,-1},{1,0,0}},
        {{ 0.5f,-0.5f,-0.5f},{1,1,1,1},{1,1},{0,0,-1},{1,0,0}},
        // Back face (+Z): normal (0,0,1), tangent (-1,0,0)
        {{-0.5f,-0.5f, 0.5f},{1,1,1,1},{1,1},{0,0,1},{-1,0,0}},
        {{ 0.5f,-0.5f, 0.5f},{1,1,1,1},{0,1},{0,0,1},{-1,0,0}},
        {{ 0.5f, 0.5f, 0.5f},{1,1,1,1},{0,0},{0,0,1},{-1,0,0}},
        {{-0.5f, 0.5f, 0.5f},{1,1,1,1},{1,0},{0,0,1},{-1,0,0}},
        // Top face (+Y): normal (0,1,0), tangent (1,0,0)
        {{-0.5f, 0.5f,-0.5f},{1,1,1,1},{0,1},{0,1,0},{1,0,0}},
        {{-0.5f, 0.5f, 0.5f},{1,1,1,1},{0,0},{0,1,0},{1,0,0}},
        {{ 0.5f, 0.5f, 0.5f},{1,1,1,1},{1,0},{0,1,0},{1,0,0}},
        {{ 0.5f, 0.5f,-0.5f},{1,1,1,1},{1,1},{0,1,0},{1,0,0}},
        // Bottom face (-Y): normal (0,-1,0), tangent (1,0,0)
        {{-0.5f,-0.5f,-0.5f},{1,1,1,1},{0,0},{0,-1,0},{1,0,0}},
        {{ 0.5f,-0.5f,-0.5f},{1,1,1,1},{1,0},{0,-1,0},{1,0,0}},
        {{ 0.5f,-0.5f, 0.5f},{1,1,1,1},{1,1},{0,-1,0},{1,0,0}},
        {{-0.5f,-0.5f, 0.5f},{1,1,1,1},{0,1},{0,-1,0},{1,0,0}},
        // Left face (-X): normal (-1,0,0), tangent (0,0,-1)
        {{-0.5f,-0.5f, 0.5f},{1,1,1,1},{0,1},{-1,0,0},{0,0,-1}},
        {{-0.5f, 0.5f, 0.5f},{1,1,1,1},{0,0},{-1,0,0},{0,0,-1}},
        {{-0.5f, 0.5f,-0.5f},{1,1,1,1},{1,0},{-1,0,0},{0,0,-1}},
        {{-0.5f,-0.5f,-0.5f},{1,1,1,1},{1,1},{-1,0,0},{0,0,-1}},
        // Right face (+X): normal (1,0,0), tangent (0,0,1)
        {{ 0.5f,-0.5f,-0.5f},{1,1,1,1},{0,1},{1,0,0},{0,0,1}},
        {{ 0.5f, 0.5f,-0.5f},{1,1,1,1},{0,0},{1,0,0},{0,0,1}},
        {{ 0.5f, 0.5f, 0.5f},{1,1,1,1},{1,0},{1,0,0},{0,0,1}},
        {{ 0.5f,-0.5f, 0.5f},{1,1,1,1},{1,1},{1,0,0},{0,0,1}},
    };

    uint16_t cubeIndices[] = {
        0,1,2, 0,2,3,  4,5,6, 4,6,7,  8,9,10, 8,10,11,
        12,13,14, 12,14,15,  16,17,18, 16,18,19,  20,21,22, 20,22,23,
    };

    if (!CreateBuffer(device, physDevice, sizeof(cubeVerts),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        renderer.cubeVertexBuffer, renderer.cubeVertexMemory)) {
        LOG_ERROR("Failed to create cube vertex buffer");
        return false;
    }
    void* data;
    vkMapMemory(device, renderer.cubeVertexMemory, 0, sizeof(cubeVerts), 0, &data);
    memcpy(data, cubeVerts, sizeof(cubeVerts));
    vkUnmapMemory(device, renderer.cubeVertexMemory);

    if (!CreateBuffer(device, physDevice, sizeof(cubeIndices),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        renderer.cubeIndexBuffer, renderer.cubeIndexMemory)) {
        LOG_ERROR("Failed to create cube index buffer");
        return false;
    }
    vkMapMemory(device, renderer.cubeIndexMemory, 0, sizeof(cubeIndices), 0, &data);
    memcpy(data, cubeIndices, sizeof(cubeIndices));
    vkUnmapMemory(device, renderer.cubeIndexMemory);

    // Grid vertex buffer
    const int gridSize = 10;
    const float gridSpacing = 1.0f;
    std::vector<GridVertex> gridVerts;
    for (int i = -gridSize; i <= gridSize; i++) {
        gridVerts.push_back({{(float)i * gridSpacing, -1.0f, -gridSize * gridSpacing}});
        gridVerts.push_back({{(float)i * gridSpacing, -1.0f,  gridSize * gridSpacing}});
        gridVerts.push_back({{-gridSize * gridSpacing, -1.0f, (float)i * gridSpacing}});
        gridVerts.push_back({{ gridSize * gridSpacing, -1.0f, (float)i * gridSpacing}});
    }
    renderer.gridVertexCount = (int)gridVerts.size();

    VkDeviceSize gridBufSize = gridVerts.size() * sizeof(GridVertex);
    if (!CreateBuffer(device, physDevice, gridBufSize,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        renderer.gridVertexBuffer, renderer.gridVertexMemory)) {
        LOG_ERROR("Failed to create grid vertex buffer");
        return false;
    }
    vkMapMemory(device, renderer.gridVertexMemory, 0, gridBufSize, 0, &data);
    memcpy(data, gridVerts.data(), (size_t)gridBufSize);
    vkUnmapMemory(device, renderer.gridVertexMemory);

    // Command pool + command buffer
    {
        VkCommandPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamilyIndex;

        if (vkCreateCommandPool(device, &poolInfo, nullptr, &renderer.commandPool) != VK_SUCCESS) {
            LOG_ERROR("Failed to create command pool");
            return false;
        }

        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = renderer.commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        if (vkAllocateCommandBuffers(device, &allocInfo, &renderer.commandBuffer) != VK_SUCCESS) {
            LOG_ERROR("Failed to allocate command buffer");
            return false;
        }
    }

    // Frame fence
    {
        VkFenceCreateInfo fenceInfo = {};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        if (vkCreateFence(device, &fenceInfo, nullptr, &renderer.frameFence) != VK_SUCCESS) {
            LOG_ERROR("Failed to create fence");
            return false;
        }
    }

    // Load textures and create descriptor set
    {
        std::string texDir = GetTextureDir();
        const char* texFiles[3] = {
            "Wood_Crate_001_basecolor.jpg",
            "Wood_Crate_001_normal.jpg",
            "Wood_Crate_001_ambientOcclusion.jpg",
        };
        bool isNormal[3] = {false, true, false};
        renderer.texturesLoaded = true;
        for (int i = 0; i < 3; i++) {
            std::string path = texDir + texFiles[i];
            if (!CreateTextureFromFile(device, physDevice, renderer.commandPool,
                    renderer.graphicsQueue, path.c_str(), isNormal[i],
                    renderer.texImages[i], renderer.texMemory[i], renderer.texViews[i])) {
                renderer.texturesLoaded = false;
            }
        }

        // Sampler (linear filtering, repeat wrap)
        VkSamplerCreateInfo samplerInfo = {};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.maxAnisotropy = 1.0f;
        samplerInfo.maxLod = 1.0f;
        if (vkCreateSampler(device, &samplerInfo, nullptr, &renderer.texSampler) != VK_SUCCESS) {
            LOG_ERROR("Failed to create texture sampler");
            return false;
        }

        // Descriptor pool and set
        VkDescriptorPoolSize poolSize = {};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = 3;
        VkDescriptorPoolCreateInfo dpInfo = {};
        dpInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpInfo.maxSets = 1;
        dpInfo.poolSizeCount = 1;
        dpInfo.pPoolSizes = &poolSize;
        if (vkCreateDescriptorPool(device, &dpInfo, nullptr, &renderer.descriptorPool) != VK_SUCCESS) {
            LOG_ERROR("Failed to create descriptor pool");
            return false;
        }

        VkDescriptorSetAllocateInfo dsAllocInfo = {};
        dsAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsAllocInfo.descriptorPool = renderer.descriptorPool;
        dsAllocInfo.descriptorSetCount = 1;
        dsAllocInfo.pSetLayouts = &renderer.descriptorSetLayout;
        if (vkAllocateDescriptorSets(device, &dsAllocInfo, &renderer.descriptorSet) != VK_SUCCESS) {
            LOG_ERROR("Failed to allocate descriptor set");
            return false;
        }

        // Update descriptor set with texture views
        VkDescriptorImageInfo imageInfos[3] = {};
        VkWriteDescriptorSet writes[3] = {};
        for (int i = 0; i < 3; i++) {
            imageInfos[i].sampler = renderer.texSampler;
            imageInfos[i].imageView = renderer.texViews[i];
            imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = renderer.descriptorSet;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &imageInfos[i];
        }
        vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

        if (renderer.texturesLoaded) {
            LOG_INFO("All crate textures loaded successfully");
        } else {
            LOG_WARN("Some textures missing — using fallback colors");
        }
    }

    LOG_INFO("Vulkan renderer initialized");
    return true;
}

// Standalone per-set depth image (one per SwapchainFramebuffers, so zones can
// be rendered into independently). Ported from cube_zones_vk_macos.
static bool CreateDepthImage(VkDevice device, VkPhysicalDevice physDevice,
    uint32_t width, uint32_t height,
    VkImage& image, VkDeviceMemory& memory)
{
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_D32_SFLOAT;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) return false;

    VkMemoryRequirements memReqs;
    vkGetImageMemoryRequirements(device, image, &memReqs);

    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryType(physDevice, memReqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device, &allocInfo, nullptr, &memory) != VK_SUCCESS) return false;
    vkBindImageMemory(device, image, memory, 0);
    return true;
}

// Build a framebuffer set (color + own depth) over `images` — used for both
// the main SBS swapchain and each zone swapchain (multi-target refactor from
// cube_zones_vk_macos).
static bool CreateFramebuffersInto(VkRenderer& renderer, SwapchainFramebuffers& fb,
    const VkImage* images, uint32_t imageCount,
    uint32_t width, uint32_t height, VkFormat colorFormat)
{
    VkDevice device = renderer.device;
    fb.width = width;
    fb.height = height;
    fb.colorViews.resize(imageCount);
    fb.depthViews.resize(imageCount);
    fb.framebuffers.resize(imageCount);

    if (!CreateDepthImage(device, renderer.physicalDevice, width, height,
        fb.depthImage, fb.depthMemory)) {
        LOG_ERROR("Failed to create depth image (%ux%u)", width, height);
        return false;
    }

    for (uint32_t i = 0; i < imageCount; i++) {
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = images[i];
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = colorFormat;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device, &viewInfo, nullptr, &fb.colorViews[i]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create image view for image %u", i);
            return false;
        }

        VkImageViewCreateInfo depthViewInfo = {};
        depthViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        depthViewInfo.image = fb.depthImage;
        depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        depthViewInfo.format = VK_FORMAT_D32_SFLOAT;
        depthViewInfo.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device, &depthViewInfo, nullptr, &fb.depthViews[i]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create depth view for image %u", i);
            return false;
        }

        VkImageView attachments[] = {fb.colorViews[i], fb.depthViews[i]};
        VkFramebufferCreateInfo fbInfo = {};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = renderer.renderPass;
        fbInfo.attachmentCount = 2;
        fbInfo.pAttachments = attachments;
        fbInfo.width = width;
        fbInfo.height = height;
        fbInfo.layers = 1;
        if (vkCreateFramebuffer(device, &fbInfo, nullptr, &fb.framebuffers[i]) != VK_SUCCESS) {
            LOG_ERROR("Failed to create framebuffer for image %u", i);
            return false;
        }
    }

    LOG_INFO("Created %u framebuffers (%ux%u)", imageCount, width, height);
    return true;
}

static bool CreateSwapchainFramebuffers(VkRenderer& renderer,
    const VkImage* images, uint32_t count,
    uint32_t width, uint32_t height, VkFormat colorFormat)
{
    return CreateFramebuffersInto(renderer, renderer.swapchainFBs, images, count,
                                  width, height, colorFormat);
}

static void DestroyFramebuffers(VkDevice device, SwapchainFramebuffers& fb)
{
    for (auto f : fb.framebuffers) {
        if (f != VK_NULL_HANDLE) vkDestroyFramebuffer(device, f, nullptr);
    }
    for (auto v : fb.colorViews) {
        if (v != VK_NULL_HANDLE) vkDestroyImageView(device, v, nullptr);
    }
    for (auto v : fb.depthViews) {
        if (v != VK_NULL_HANDLE) vkDestroyImageView(device, v, nullptr);
    }
    if (fb.depthImage != VK_NULL_HANDLE) vkDestroyImage(device, fb.depthImage, nullptr);
    if (fb.depthMemory != VK_NULL_HANDLE) vkFreeMemory(device, fb.depthMemory, nullptr);
    fb = SwapchainFramebuffers{};
}

// Generalized scene render: cube + grid into ANY framebuffer set with an
// explicit clear color and a per-call rotation offset (XR_DXR_display_zones:
// each zone renders into its own swapchain with its own spin phase and
// premultiplied background). The main SBS path calls it through the RenderScene
// wrapper below. Cube geometry is the small display-centered zones cube
// (0.06 m at origin), so the zone rig framing shows it.
static void RenderSceneToFramebuffer(VkRenderer& renderer, SwapchainFramebuffers& fb,
    uint32_t imageIndex, const EyeRenderParams* eyes, int eyeCount,
    const float clearColor[4], float rotationOffset)
{
    VkDevice device = renderer.device;

    // Wait for previous frame
    vkWaitForFences(device, 1, &renderer.frameFence, VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &renderer.frameFence);

    // Begin command buffer
    VkCommandBuffer cmd = renderer.commandBuffer;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    VkClearValue clearValues[2] = {};
    clearValues[0].color = VkClearColorValue{{clearColor[0], clearColor[1], clearColor[2], clearColor[3]}};
    clearValues[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rpBegin = {};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = renderer.renderPass;
    rpBegin.framebuffer = fb.framebuffers[imageIndex];
    rpBegin.renderArea = {{0, 0}, {fb.width, fb.height}};
    rpBegin.clearValueCount = 2;
    rpBegin.pClearValues = clearValues;

    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    // Render all eyes in a single render pass — just change viewport/scissor
    for (int eye = 0; eye < eyeCount; eye++) {
        const auto& e = eyes[eye];

        // Viewport with Y-flip for correct NDC convention
        VkViewport viewport = {(float)e.viewportX, (float)(e.viewportY + e.height),
            (float)e.width, -(float)e.height, 0.0f, 1.0f};
        VkRect2D scissor = {{(int32_t)e.viewportX, (int32_t)e.viewportY}, {e.width, e.height}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        float vp[16];
        mat4_multiply(vp, e.projMat, e.viewMat);

        // Draw textured cube — small, display-centered on the Y=0 grid floor.
        {
            const float cubeSize = 0.06f;
            const float cubeHeight = cubeSize / 2.0f;

            float rot[16], scl[16], trans[16], model[16], tmp[16];
            mat4_rotation_y(rot, renderer.cubeRotation + rotationOffset);
            mat4_scaling(scl, cubeSize, cubeSize, cubeSize);
            mat4_translation(trans, 0.0f, cubeHeight, 0.0f);
            mat4_multiply(tmp, scl, rot);
            mat4_multiply(model, trans, tmp);
            float mvp[16];
            mat4_multiply(mvp, vp, model);

            // Push constants: MVP (64 bytes) + model (64 bytes) = 128 bytes
            float pushData[32];
            memcpy(pushData, mvp, 64);
            memcpy(pushData + 16, model, 64);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.cubePipeline);
            vkCmdPushConstants(cmd, renderer.cubePipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT, 0, 128, pushData);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.cubePipelineLayout,
                0, 1, &renderer.descriptorSet, 0, nullptr);

            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &renderer.cubeVertexBuffer, &offset);
            vkCmdBindIndexBuffer(cmd, renderer.cubeIndexBuffer, 0, VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexed(cmd, 36, 1, 0, 0, 0);
        }

        // Draw grid floor (keeps the Linux grid pipeline: PushConstants{mat4,vec4})
        {
            const float gridScale = 0.05f;
            float scale[16], trans[16];
            mat4_scaling(scale, gridScale, gridScale, gridScale);
            mat4_translation(trans, 0.0f, gridScale, 0.0f);

            float tmp1[16], mvp[16];
            mat4_multiply(tmp1, trans, scale);
            mat4_multiply(mvp, vp, tmp1);

            PushConstants pc = {};
            memcpy(pc.transform, mvp, sizeof(mvp));
            pc.color[0] = 0.3f; pc.color[1] = 0.3f; pc.color[2] = 0.35f; pc.color[3] = 1.0f;

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, renderer.gridPipeline);
            vkCmdPushConstants(cmd, renderer.pipelineLayout,
                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);

            VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &renderer.gridVertexBuffer, &offset);
            vkCmdDraw(cmd, renderer.gridVertexCount, 1, 0, 0);
        }
    }

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    // Submit
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;

    vkQueueSubmit(renderer.graphicsQueue, 1, &submitInfo, renderer.frameFence);

    // Wait for completion before returning (runtime needs the image ready).
    vkWaitForFences(device, 1, &renderer.frameFence, VK_TRUE, UINT64_MAX);
}

// Main SBS path (zones unavailable) — renders into the main swapchain FB set.
// TODO(#778): the macOS zones app defaults to a transparent-bg clear
// (DISPLAYXR_TRANSPARENT_BG); the Linux handle scaffold has no transparent-bg
// path (it clears opaque), so the fallback stays opaque. The zone path itself
// still alpha-composites against the desktop via ALPHA_BLEND + premultiplied
// zone clears, independent of this fallback.
static void RenderScene(
    VkRenderer& renderer, uint32_t imageIndex,
    const EyeRenderParams* eyes, int eyeCount)
{
    const float clearOpaque[4] = {0.05f, 0.05f, 0.25f, 1.0f};
    RenderSceneToFramebuffer(renderer, renderer.swapchainFBs, imageIndex, eyes, eyeCount,
                             clearOpaque, 0.0f);
}

static void CleanupVkRenderer(VkRenderer& renderer) {
    if (!renderer.device) return;

    vkDeviceWaitIdle(renderer.device);

    // Main SBS framebuffer set (zone sets are torn down in main()).
    DestroyFramebuffers(renderer.device, renderer.swapchainFBs);

    if (renderer.frameFence) {
        vkDestroyFence(renderer.device, renderer.frameFence, nullptr);
        renderer.frameFence = VK_NULL_HANDLE;
    }
    if (renderer.commandPool) {
        vkDestroyCommandPool(renderer.device, renderer.commandPool, nullptr);
        renderer.commandPool = VK_NULL_HANDLE;
    }
    if (renderer.gridVertexBuffer) {
        vkDestroyBuffer(renderer.device, renderer.gridVertexBuffer, nullptr);
        renderer.gridVertexBuffer = VK_NULL_HANDLE;
    }
    if (renderer.gridVertexMemory) {
        vkFreeMemory(renderer.device, renderer.gridVertexMemory, nullptr);
        renderer.gridVertexMemory = VK_NULL_HANDLE;
    }
    if (renderer.cubeIndexBuffer) {
        vkDestroyBuffer(renderer.device, renderer.cubeIndexBuffer, nullptr);
        renderer.cubeIndexBuffer = VK_NULL_HANDLE;
    }
    if (renderer.cubeIndexMemory) {
        vkFreeMemory(renderer.device, renderer.cubeIndexMemory, nullptr);
        renderer.cubeIndexMemory = VK_NULL_HANDLE;
    }
    if (renderer.cubeVertexBuffer) {
        vkDestroyBuffer(renderer.device, renderer.cubeVertexBuffer, nullptr);
        renderer.cubeVertexBuffer = VK_NULL_HANDLE;
    }
    if (renderer.cubeVertexMemory) {
        vkFreeMemory(renderer.device, renderer.cubeVertexMemory, nullptr);
        renderer.cubeVertexMemory = VK_NULL_HANDLE;
    }
    if (renderer.gridPipeline) {
        vkDestroyPipeline(renderer.device, renderer.gridPipeline, nullptr);
        renderer.gridPipeline = VK_NULL_HANDLE;
    }
    if (renderer.cubePipeline) {
        vkDestroyPipeline(renderer.device, renderer.cubePipeline, nullptr);
        renderer.cubePipeline = VK_NULL_HANDLE;
    }
    // Texture cleanup
    if (renderer.descriptorPool) {
        vkDestroyDescriptorPool(renderer.device, renderer.descriptorPool, nullptr);
        renderer.descriptorPool = VK_NULL_HANDLE;
    }
    if (renderer.descriptorSetLayout) {
        vkDestroyDescriptorSetLayout(renderer.device, renderer.descriptorSetLayout, nullptr);
        renderer.descriptorSetLayout = VK_NULL_HANDLE;
    }
    if (renderer.texSampler) {
        vkDestroySampler(renderer.device, renderer.texSampler, nullptr);
        renderer.texSampler = VK_NULL_HANDLE;
    }
    for (int i = 0; i < 3; i++) {
        if (renderer.texViews[i]) {
            vkDestroyImageView(renderer.device, renderer.texViews[i], nullptr);
            renderer.texViews[i] = VK_NULL_HANDLE;
        }
        if (renderer.texImages[i]) {
            vkDestroyImage(renderer.device, renderer.texImages[i], nullptr);
            renderer.texImages[i] = VK_NULL_HANDLE;
        }
        if (renderer.texMemory[i]) {
            vkFreeMemory(renderer.device, renderer.texMemory[i], nullptr);
            renderer.texMemory[i] = VK_NULL_HANDLE;
        }
    }
    if (renderer.cubePipelineLayout) {
        vkDestroyPipelineLayout(renderer.device, renderer.cubePipelineLayout, nullptr);
        renderer.cubePipelineLayout = VK_NULL_HANDLE;
    }
    if (renderer.pipelineLayout) {
        vkDestroyPipelineLayout(renderer.device, renderer.pipelineLayout, nullptr);
        renderer.pipelineLayout = VK_NULL_HANDLE;
    }
    if (renderer.renderPass) {
        vkDestroyRenderPass(renderer.device, renderer.renderPass, nullptr);
        renderer.renderPass = VK_NULL_HANDLE;
    }
}

// ============================================================================
// OpenXR session management
// ============================================================================

struct SwapchainInfo {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    int64_t format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t imageCount = 0;
};

struct AppXrSession {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;

    SwapchainInfo swapchain;

    XrViewConfigurationType viewConfigType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    std::vector<XrViewConfigurationView> configViews;

    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning = false;
    bool exitRequested = false;

    // Per-view dimensions from recommendedImageRectWidth/Height (set once at init)
    uint32_t viewWidth = 0;
    uint32_t viewHeight = 0;

    // v16 XrDisplayDesktopPositionDXR — 3D panel top-left in virtual-desktop
    // pixels (top-down, origin = primary top-left); (0,0) = primary/unknown.
    int32_t displayScreenLeft = 0;
    int32_t displayScreenTop = 0;
};

static bool InitializeOpenXR(AppXrSession& xr) {
    LOG_INFO("Initializing OpenXR...");

    uint32_t extensionCount = 0;
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr));

    std::vector<XrExtensionProperties> extensions(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data()));

    bool hasVulkan = false;
    bool hasXlibBinding = false;
    bool hasDisplayInfo = false;
    for (const auto& ext : extensions) {
        if (strcmp(ext.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0) {
            hasVulkan = true;
        }
        if (strcmp(ext.extensionName, XR_DXR_XLIB_WINDOW_BINDING_EXTENSION_NAME) == 0) {
            hasXlibBinding = true;
        }
        if (strcmp(ext.extensionName, XR_DXR_DISPLAY_INFO_EXTENSION_NAME) == 0) {
            hasDisplayInfo = true;
        }
        if (strcmp(ext.extensionName, XR_DXR_VIEW_RIG_EXTENSION_NAME) == 0) {
            g_hasViewRigExt = true;
        }
        if (strcmp(ext.extensionName, XR_DXR_LOCAL_3D_ZONE_EXTENSION_NAME) == 0) {
            g_hasLocal3DZoneExt = true;
        }
        if (strcmp(ext.extensionName, XR_DXR_DISPLAY_ZONES_EXTENSION_NAME) == 0) {
            g_hasDisplayZonesExt = true;
        }
    }

    // XR_DXR_display_zones composes XR_DXR_local_3d_zone + XR_DXR_view_rig.
    if (g_hasDisplayZonesExt && (!g_hasLocal3DZoneExt || !g_hasViewRigExt)) {
        LOG_ERROR("XR_DXR_display_zones advertised without its prerequisites — zones path disabled");
        g_hasDisplayZonesExt = false;
    }
    if (!g_hasDisplayZonesExt) {
        LOG_ERROR("XR_DXR_display_zones NOT available — running as plain 2-view SBS cube");
    }
    LOG_INFO("XR_DXR_view_rig: %s", g_hasViewRigExt ? "AVAILABLE" : "NOT FOUND");

    LOG_INFO("XR_KHR_vulkan_enable: %s", hasVulkan ? "AVAILABLE" : "NOT FOUND");
    if (!hasVulkan) {
        LOG_ERROR("XR_KHR_vulkan_enable extension not available");
        return false;
    }

    LOG_INFO("XR_DXR_xlib_window_binding: %s", hasXlibBinding ? "AVAILABLE" : "NOT FOUND");
    if (!hasXlibBinding) {
        // Handle class needs the binding — without it the runtime would
        // self-create a second window (hosted fallback), defeating the test.
        LOG_ERROR("XR_DXR_xlib_window_binding not available — this is the "
                  "handle-class vehicle; use cube_hosted_legacy_vk_linux against "
                  "runtimes without the extension");
        return false;
    }

    std::vector<const char*> enabledExtensions;
    enabledExtensions.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    enabledExtensions.push_back(XR_DXR_XLIB_WINDOW_BINDING_EXTENSION_NAME);
    if (hasDisplayInfo) {
        // Enabled for the INV-1.3 panel desktop-position query below (#715).
        // NOTE: enabling XR_DXR_display_info also switches the runtime's view
        // sizing off the legacy-app compromise path — the app still renders a
        // fixed 2-view SBS at whatever dimensions xrEnumerateViewConfigurationViews
        // reports at init, and does not adapt to later mode changes.
        enabledExtensions.push_back(XR_DXR_DISPLAY_INFO_EXTENSION_NAME);
        LOG_INFO("XR_DXR_display_info: AVAILABLE (enabled for panel position)");
    }
    // Zones + its prerequisites (view_rig, local_3d_zone) — enabled together so
    // the zones path can activate; if any is missing, g_hasDisplayZonesExt was
    // already cleared above and only view_rig/local_3d_zone (if present) enable.
    if (g_hasViewRigExt) {
        enabledExtensions.push_back(XR_DXR_VIEW_RIG_EXTENSION_NAME);
    }
    if (g_hasLocal3DZoneExt) {
        enabledExtensions.push_back(XR_DXR_LOCAL_3D_ZONE_EXTENSION_NAME);
    }
    if (g_hasDisplayZonesExt) {
        enabledExtensions.push_back(XR_DXR_DISPLAY_ZONES_EXTENSION_NAME);
        LOG_INFO("XR_DXR_display_zones: AVAILABLE (zones path enabled)");
    }

    XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy(createInfo.applicationInfo.applicationName, "VkCubeZonesLinux",
            sizeof(createInfo.applicationInfo.applicationName) - 1);
    createInfo.applicationInfo.applicationVersion = 1;
    strncpy(createInfo.applicationInfo.engineName, "None",
            sizeof(createInfo.applicationInfo.engineName) - 1);
    createInfo.applicationInfo.engineVersion = 0;
    createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    createInfo.enabledExtensionCount = (uint32_t)enabledExtensions.size();
    createInfo.enabledExtensionNames = enabledExtensions.data();

    XR_CHECK(xrCreateInstance(&createInfo, &xr.instance));
    LOG_INFO("OpenXR instance created");

    XrSystemGetInfo systemInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(xr.instance, &systemInfo, &xr.systemId));

    // INV-1.3: panel desktop position, so the window opens on the 3D panel
    // instead of the primary monitor (spec v16, #715). The runtime fills the
    // chained struct only when XR_DXR_display_info is enabled; the zero-init
    // (0,0) = primary is the safe fallback either way.
    if (hasDisplayInfo) {
        XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES};
        XrDisplayDesktopPositionDXR desktopPos = {};
        desktopPos.type = XR_TYPE_DISPLAY_DESKTOP_POSITION_DXR;
        sysProps.next = &desktopPos;
        if (XR_SUCCEEDED(xrGetSystemProperties(xr.instance, xr.systemId, &sysProps))) {
            xr.displayScreenLeft = desktopPos.left;
            xr.displayScreenTop = desktopPos.top;
            LOG_INFO("Display desktop position: (%d, %d)",
                     xr.displayScreenLeft, xr.displayScreenTop);
        }
    }

    uint32_t viewCount = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, 0, &viewCount, nullptr));
    xr.configViews.resize(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    XR_CHECK(xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, viewCount, &viewCount, xr.configViews.data()));

    for (uint32_t i = 0; i < viewCount; i++) {
        LOG_INFO("  View %u: %ux%u", i,
            xr.configViews[i].recommendedImageRectWidth,
            xr.configViews[i].recommendedImageRectHeight);
    }

    // Store per-view dimensions from recommended (fixed for session lifetime)
    xr.viewWidth = xr.configViews[0].recommendedImageRectWidth;
    xr.viewHeight = xr.configViews[0].recommendedImageRectHeight;

    // XR_DXR_display_zones + XR_DXR_local_3d_zone entry points.
    if (g_hasDisplayZonesExt) {
        xrGetInstanceProcAddr(xr.instance, "xrGetDisplayZoneCapabilitiesDXR",
            (PFN_xrVoidFunction*)&g_pfnGetZoneCaps);
        xrGetInstanceProcAddr(xr.instance, "xrGetDisplayZoneRecommendedViewSizeDXR",
            (PFN_xrVoidFunction*)&g_pfnGetZoneViewSize);
        xrGetInstanceProcAddr(xr.instance, "xrCreateLocal3DZoneMaskDXR",
            (PFN_xrVoidFunction*)&g_pfnCreateZoneMask);
        xrGetInstanceProcAddr(xr.instance, "xrSetLocal3DZoneFromRectsDXR",
            (PFN_xrVoidFunction*)&g_pfnSetZoneRects);
        xrGetInstanceProcAddr(xr.instance, "xrDestroyLocal3DZoneMaskDXR",
            (PFN_xrVoidFunction*)&g_pfnDestroyZoneMask);
        if (g_pfnGetZoneCaps == nullptr || g_pfnGetZoneViewSize == nullptr) {
            LOG_ERROR("XR_DXR_display_zones entry points unresolved — zones path disabled");
            g_hasDisplayZonesExt = false;
        }
    }

    return true;
}

static bool GetVulkanGraphicsRequirements(AppXrSession& xr) {
    PFN_xrGetVulkanGraphicsRequirementsKHR pfn = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsRequirementsKHR",
        (PFN_xrVoidFunction*)&pfn));

    XrGraphicsRequirementsVulkanKHR graphicsReq = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    XR_CHECK(pfn(xr.instance, xr.systemId, &graphicsReq));

    LOG_INFO("Vulkan graphics requirements: %d.%d.%d - %d.%d.%d",
        VK_VERSION_MAJOR(graphicsReq.minApiVersionSupported),
        VK_VERSION_MINOR(graphicsReq.minApiVersionSupported),
        VK_VERSION_PATCH(graphicsReq.minApiVersionSupported),
        VK_VERSION_MAJOR(graphicsReq.maxApiVersionSupported),
        VK_VERSION_MINOR(graphicsReq.maxApiVersionSupported),
        VK_VERSION_PATCH(graphicsReq.maxApiVersionSupported));

    return true;
}

static bool CreateVulkanInstance(AppXrSession& xr, VkInstance& vkInstance) {
    LOG_INFO("Creating Vulkan instance...");

    // Get required instance extensions from the runtime
    PFN_xrGetVulkanInstanceExtensionsKHR pfn = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(xr.instance, "xrGetVulkanInstanceExtensionsKHR",
        (PFN_xrVoidFunction*)&pfn));

    uint32_t bufferSize = 0;
    pfn(xr.instance, xr.systemId, 0, &bufferSize, nullptr);
    std::string extensionsStr(bufferSize, '\0');
    pfn(xr.instance, xr.systemId, bufferSize, &bufferSize, extensionsStr.data());

    // Parse space-separated extension names
    std::vector<std::string> extensionNames;
    {
        size_t start = 0;
        while (start < extensionsStr.size()) {
            size_t end = extensionsStr.find(' ', start);
            if (end == std::string::npos) end = extensionsStr.size();
            std::string name = extensionsStr.substr(start, end - start);
            if (!name.empty() && name[0] != '\0') {
                extensionNames.push_back(name);
            }
            start = end + 1;
        }
    }

    // MoltenVK portability: enumerate available instance extensions and add
    // VK_KHR_portability_enumeration if present
    uint32_t availExtCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &availExtCount, nullptr);
    std::vector<VkExtensionProperties> availExts(availExtCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &availExtCount, availExts.data());

    bool hasPortabilityEnum = false;
    // CANDIDATE PATCH (#706 Linux validation): MoltenVK-only portability
    // enumeration; guard on the macro so it compiles out where Vulkan headers
    // don't define it (e.g. Ubuntu Vulkan-Headers v204). Not needed on Linux.
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
    for (const auto& ext : availExts) {
        if (strcmp(ext.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0) {
            hasPortabilityEnum = true;
            break;
        }
    }
    if (hasPortabilityEnum) {
        extensionNames.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        LOG_INFO("  Adding VK_KHR_portability_enumeration for MoltenVK");
    }
#endif

    std::vector<const char*> extensionPtrs;
    for (auto& name : extensionNames) {
        extensionPtrs.push_back(name.c_str());
        LOG_INFO("  VkInstance extension: %s", name.c_str());
    }

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "SimCubeOpenXR";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "None";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = (uint32_t)extensionPtrs.size();
    createInfo.ppEnabledExtensionNames = extensionPtrs.data();
#ifdef VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR
    if (hasPortabilityEnum) {
        createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
#endif

    VK_CHECK(vkCreateInstance(&createInfo, nullptr, &vkInstance));
    LOG_INFO("Vulkan instance created");
    return true;
}

static bool GetVulkanPhysicalDevice(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice& physDevice) {
    PFN_xrGetVulkanGraphicsDeviceKHR pfn = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsDeviceKHR",
        (PFN_xrVoidFunction*)&pfn));

    XR_CHECK(pfn(xr.instance, xr.systemId, vkInstance, &physDevice));

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physDevice, &props);
    LOG_INFO("Vulkan physical device: %s", props.deviceName);

    return true;
}

static bool GetVulkanDeviceExtensions(AppXrSession& xr,
    std::vector<const char*>& deviceExtensions, std::vector<std::string>& extensionStorage,
    VkPhysicalDevice physDevice)
{
    PFN_xrGetVulkanDeviceExtensionsKHR pfn = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(xr.instance, "xrGetVulkanDeviceExtensionsKHR",
        (PFN_xrVoidFunction*)&pfn));

    uint32_t bufferSize = 0;
    pfn(xr.instance, xr.systemId, 0, &bufferSize, nullptr);

    std::string extensionsStr(bufferSize, '\0');
    pfn(xr.instance, xr.systemId, bufferSize, &bufferSize, extensionsStr.data());

    extensionStorage.clear();
    deviceExtensions.clear();
    {
        size_t start = 0;
        while (start < extensionsStr.size()) {
            size_t end = extensionsStr.find(' ', start);
            if (end == std::string::npos) end = extensionsStr.size();
            std::string name = extensionsStr.substr(start, end - start);
            if (!name.empty() && name[0] != '\0') {
                extensionStorage.push_back(name);
            }
            start = end + 1;
        }
    }

    // MoltenVK portability: add VK_KHR_portability_subset if available on device
    uint32_t devExtCount = 0;
    vkEnumerateDeviceExtensionProperties(physDevice, nullptr, &devExtCount, nullptr);
    std::vector<VkExtensionProperties> devExts(devExtCount);
    vkEnumerateDeviceExtensionProperties(physDevice, nullptr, &devExtCount, devExts.data());

    for (const auto& ext : devExts) {
        if (strcmp(ext.extensionName, "VK_KHR_portability_subset") == 0) {
            extensionStorage.push_back("VK_KHR_portability_subset");
            LOG_INFO("  Adding VK_KHR_portability_subset for MoltenVK");
            break;
        }
    }

    for (auto& name : extensionStorage) {
        deviceExtensions.push_back(name.c_str());
        LOG_INFO("  VkDevice extension: %s", name.c_str());
    }

    return true;
}

static bool FindGraphicsQueueFamily(VkPhysicalDevice physDevice, uint32_t& queueFamilyIndex) {
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &queueFamilyCount, nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physDevice, &queueFamilyCount, queueFamilies.data());

    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            queueFamilyIndex = i;
            LOG_INFO("Graphics queue family: %u", i);
            return true;
        }
    }

    LOG_ERROR("No graphics queue family found");
    return false;
}

static bool CreateVulkanDevice(VkPhysicalDevice physDevice, uint32_t queueFamilyIndex,
    const std::vector<const char*>& deviceExtensions,
    VkDevice& device, VkQueue& graphicsQueue)
{
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo = {};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamilyIndex;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    VkDeviceCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos = &queueInfo;
    createInfo.enabledExtensionCount = (uint32_t)deviceExtensions.size();
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();

    VK_CHECK(vkCreateDevice(physDevice, &createInfo, nullptr, &device));

    vkGetDeviceQueue(device, queueFamilyIndex, 0, &graphicsQueue);
    LOG_INFO("Vulkan device and graphics queue created");
    return true;
}

static bool CreateSession(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice physDevice,
    VkDevice device, uint32_t queueFamilyIndex)
{
    LOG_INFO("Creating OpenXR session with Vulkan binding...");

    XrGraphicsBindingVulkanKHR vkBinding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    vkBinding.instance = vkInstance;
    vkBinding.physicalDevice = physDevice;
    vkBinding.device = device;
    vkBinding.queueFamilyIndex = queueFamilyIndex;
    vkBinding.queueIndex = 0;

    // Handle class: hand the app-owned X11 window to the runtime.
    XrXlibWindowBindingCreateInfoDXR xlibBinding = {XR_TYPE_XLIB_WINDOW_BINDING_CREATE_INFO_DXR};
    xlibBinding.next = &vkBinding;
    xlibBinding.xDisplay = g_xDisplay;
    xlibBinding.window = g_xWindow;

    XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &xlibBinding;
    sessionInfo.systemId = xr.systemId;

    XR_CHECK(xrCreateSession(xr.instance, &sessionInfo, &xr.session));
    LOG_INFO("Session created (window binding: Display %p, Window 0x%lx)",
             (void*)g_xDisplay, g_xWindow);

    return true;
}

static bool CreateSpaces(AppXrSession& xr) {
    LOG_INFO("Creating reference spaces...");

    XrReferenceSpaceCreateInfo localSpaceInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    localSpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    localSpaceInfo.poseInReferenceSpace.orientation = {0, 0, 0, 1};
    localSpaceInfo.poseInReferenceSpace.position = {0, 0, 0};
    XR_CHECK(xrCreateReferenceSpace(xr.session, &localSpaceInfo, &xr.localSpace));

    XrReferenceSpaceCreateInfo viewSpaceInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    viewSpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    viewSpaceInfo.poseInReferenceSpace.orientation = {0, 0, 0, 1};
    viewSpaceInfo.poseInReferenceSpace.position = {0, 0, 0};
    XR_CHECK(xrCreateReferenceSpace(xr.session, &viewSpaceInfo, &xr.viewSpace));

    LOG_INFO("Reference spaces created (LOCAL + VIEW)");
    return true;
}

static bool CreateSwapchain(AppXrSession& xr) {
    LOG_INFO("Creating atlas swapchain...");

    uint32_t formatCount = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(xr.session, 0, &formatCount, nullptr));

    std::vector<int64_t> formats(formatCount);
    XR_CHECK(xrEnumerateSwapchainFormats(xr.session, formatCount, &formatCount, formats.data()));

    int64_t selectedFormat = formats[0];
    LOG_INFO("Selected swapchain format: %lld (0x%llX)", (long long)selectedFormat, (long long)selectedFormat);

    const auto& view = xr.configViews[0];

    // Legacy: use recommendedImageRectWidth * 2 (stereo SBS) since no modes are enumerated
    uint32_t scWidth = view.recommendedImageRectWidth * 2;
    uint32_t scHeight = view.recommendedImageRectHeight;

    XrSwapchainCreateInfo swapchainInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    swapchainInfo.format = selectedFormat;
    swapchainInfo.sampleCount = view.recommendedSwapchainSampleCount;
    swapchainInfo.width = scWidth;
    swapchainInfo.height = scHeight;
    swapchainInfo.faceCount = 1;
    swapchainInfo.arraySize = 1;
    swapchainInfo.mipCount = 1;

    LOG_INFO("  Atlas swapchain: %ux%u", swapchainInfo.width, swapchainInfo.height);

    XR_CHECK(xrCreateSwapchain(xr.session, &swapchainInfo, &xr.swapchain.swapchain));

    xr.swapchain.format = selectedFormat;
    xr.swapchain.width = swapchainInfo.width;
    xr.swapchain.height = swapchainInfo.height;

    uint32_t imageCount = 0;
    XR_CHECK(xrEnumerateSwapchainImages(xr.swapchain.swapchain, 0, &imageCount, nullptr));
    xr.swapchain.imageCount = imageCount;

    LOG_INFO("  %u swapchain images", imageCount);
    LOG_INFO("Atlas swapchain created");
    return true;
}

static bool PollEvents(AppXrSession& xr) {
    XrEventDataBuffer event = {XR_TYPE_EVENT_DATA_BUFFER};

    while (xrPollEvent(xr.instance, &event) == XR_SUCCESS) {
        switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            auto* stateEvent = (XrEventDataSessionStateChanged*)&event;
            xr.sessionState = stateEvent->state;

            switch (xr.sessionState) {
            case XR_SESSION_STATE_READY: {
                XrSessionBeginInfo beginInfo = {XR_TYPE_SESSION_BEGIN_INFO};
                beginInfo.primaryViewConfigurationType = xr.viewConfigType;
                if (XR_SUCCEEDED(xrBeginSession(xr.session, &beginInfo))) {
                    xr.sessionRunning = true;
                    LOG_INFO("Session running");
                }
                break;
            }
            case XR_SESSION_STATE_STOPPING:
                xrEndSession(xr.session);
                xr.sessionRunning = false;
                LOG_INFO("Session stopped");
                break;
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
                xr.exitRequested = true;
                break;
            default:
                break;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            xr.exitRequested = true;
            break;
        // Legacy app: no rendering mode events (XR_DXR_display_info not enabled)
        default:
            break;
        }

        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }

    return true;
}

static bool BeginFrame(AppXrSession& xr, XrFrameState& frameState) {
    frameState = {XR_TYPE_FRAME_STATE};

    XrFrameWaitInfo waitInfo = {XR_TYPE_FRAME_WAIT_INFO};
    XrResult result = xrWaitFrame(xr.session, &waitInfo, &frameState);
    if (XR_FAILED(result)) {
        xr.exitRequested = true;
        return false;
    }

    XrFrameBeginInfo beginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
    result = xrBeginFrame(xr.session, &beginInfo);
    if (XR_FAILED(result)) return false;

    return true;
}

static bool AcquireSwapchainImage(AppXrSession& xr, uint32_t& imageIndex) {
    XrSwapchainImageAcquireInfo acquireInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrResult result = xrAcquireSwapchainImage(xr.swapchain.swapchain, &acquireInfo, &imageIndex);
    if (XR_FAILED(result)) return false;

    XrSwapchainImageWaitInfo waitInfo = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    result = xrWaitSwapchainImage(xr.swapchain.swapchain, &waitInfo);
    if (XR_FAILED(result)) {
        XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(xr.swapchain.swapchain, &releaseInfo);
        return false;
    }

    return true;
}

static bool ReleaseSwapchainImage(AppXrSession& xr) {
    XrSwapchainImageReleaseInfo releaseInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    return XR_SUCCEEDED(xrReleaseSwapchainImage(xr.swapchain.swapchain, &releaseInfo));
}

static bool EndFrame(AppXrSession& xr, XrTime displayTime, const XrCompositionLayerProjectionView* views, uint32_t viewCount) {
    XrCompositionLayerProjection projectionLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projectionLayer.space = xr.localSpace;
    projectionLayer.viewCount = viewCount;
    projectionLayer.views = views;

    const XrCompositionLayerBaseHeader* layers[] = {
        (XrCompositionLayerBaseHeader*)&projectionLayer
    };

    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = displayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = 1;
    endInfo.layers = layers;

    return XR_SUCCEEDED(xrEndFrame(xr.session, &endInfo));
}

static void CleanupOpenXR(AppXrSession& xr) {
    if (xr.swapchain.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(xr.swapchain.swapchain);
        xr.swapchain.swapchain = XR_NULL_HANDLE;
    }
    if (xr.viewSpace != XR_NULL_HANDLE) {
        xrDestroySpace(xr.viewSpace);
        xr.viewSpace = XR_NULL_HANDLE;
    }
    if (xr.localSpace != XR_NULL_HANDLE) {
        xrDestroySpace(xr.localSpace);
        xr.localSpace = XR_NULL_HANDLE;
    }
    if (xr.session != XR_NULL_HANDLE) {
        xrDestroySession(xr.session);
        xr.session = XR_NULL_HANDLE;
    }
    if (xr.instance != XR_NULL_HANDLE) {
        xrDestroyInstance(xr.instance);
        xr.instance = XR_NULL_HANDLE;
    }
    LOG_INFO("OpenXR cleanup complete");
}

// ============================================================================
// XR_DXR_display_zones helpers (ADR-027) — clear-based zones
// (ported from cube_zones_vk_macos)
// ============================================================================

// One-shot graphics-queue submit clearing a whole VK image to `color` and
// leaving it in COLOR_ATTACHMENT_OPTIMAL (the steady-state layout the
// compositor's zone pass transitions from). oldLayout UNDEFINED is always
// valid here — the clear overwrites every pixel.
static void ClearImageToColor(VkDevice device, VkCommandPool pool, VkQueue queue,
                              VkImage image, const float color[4])
{
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device, &ai, &cmd) != VK_SUCCESS) {
        return;
    }

    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    VkClearColorValue cv;
    for (int i = 0; i < 4; i++) {
        cv.float32[i] = color[i];
    }
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &range);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(device, pool, 1, &cmd);
}

// Create one zone's swapchain, sized per xrGetDisplayZoneRecommendedViewSizeDXR,
// horizontally tiled per view; enumerate its VK images and build the zone's
// framebuffer set (the zone renders the cube + grid scene).
static bool CreateZoneResources(AppXrSession& xr, VkRenderer& renderer, uint32_t zoneIndex,
                                DisplayZone& z, uint32_t viewCount)
{
    XrExtent2Di rec = {};
    XrResult r = g_pfnGetZoneViewSize(xr.session, &z.rect, &rec);
    if (XR_FAILED(r) || rec.width <= 0 || rec.height <= 0) {
        LOG_ERROR("[zones] zone %u: xrGetDisplayZoneRecommendedViewSizeDXR failed (0x%x, %dx%d)",
                  z.zoneId, (unsigned)r, rec.width, rec.height);
        return false;
    }
    z.tileW = (uint32_t)rec.width;
    z.tileH = (uint32_t)rec.height;
    z.tileCount = viewCount;

    XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                     XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sci.format = xr.swapchain.format;
    sci.sampleCount = 1;
    sci.width = z.tileW * z.tileCount;
    sci.height = z.tileH;
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(xr.session, &sci, &z.swapchain))) {
        LOG_ERROR("[zones] zone %u: xrCreateSwapchain failed (%ux%u)", z.zoneId, sci.width, sci.height);
        return false;
    }

    uint32_t n = 0;
    xrEnumerateSwapchainImages(z.swapchain, 0, &n, nullptr);
    std::vector<XrSwapchainImageVulkanKHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    if (n == 0 || XR_FAILED(xrEnumerateSwapchainImages(z.swapchain, n, &n,
                                                       (XrSwapchainImageBaseHeader*)imgs.data()))) {
        LOG_ERROR("[zones] zone %u: xrEnumerateSwapchainImages failed", z.zoneId);
        return false;
    }
    z.images.resize(n);
    for (uint32_t i = 0; i < n; i++) {
        z.images[i] = imgs[i].image;
    }

    if (!CreateFramebuffersInto(renderer, g_zoneFbs[zoneIndex], z.images.data(), n,
                                z.tileW * z.tileCount, z.tileH, (VkFormat)xr.swapchain.format)) {
        LOG_ERROR("[zones] zone %u: framebuffer creation failed", z.zoneId);
        return false;
    }

    LOG_INFO("[zones] zone %u: rect %d,%d %dx%d -> swapchain %ux%u (%u tiles of %ux%u)",
             z.zoneId, z.rect.offset.x, z.rect.offset.y, z.rect.extent.width, z.rect.extent.height,
             z.tileW * z.tileCount, z.tileH, z.tileCount, z.tileW, z.tileH);
    return true;
}

// Create the always-on Local2D strip and fill it once with solid amber.
static bool CreateAndFillStrip(AppXrSession& xr, VkDevice device, VkCommandPool pool, VkQueue queue)
{
    const uint32_t w = (uint32_t)g_stripRect.extent.width;
    const uint32_t h = (uint32_t)g_stripRect.extent.height;
    if (w == 0 || h == 0) {
        return false;
    }

    XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                     XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sci.format = xr.swapchain.format;
    sci.sampleCount = 1;
    sci.width = w;
    sci.height = h;
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(xr.session, &sci, &g_strip.swapchain))) {
        LOG_ERROR("[zones] strip: xrCreateSwapchain failed");
        return false;
    }
    g_strip.w = w;
    g_strip.h = h;

    uint32_t n = 0;
    xrEnumerateSwapchainImages(g_strip.swapchain, 0, &n, nullptr);
    std::vector<XrSwapchainImageVulkanKHR> imgs(n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    if (n == 0 || XR_FAILED(xrEnumerateSwapchainImages(g_strip.swapchain, n, &n,
                                                       (XrSwapchainImageBaseHeader*)imgs.data()))) {
        LOG_ERROR("[zones] strip: xrEnumerateSwapchainImages failed");
        return false;
    }

    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    uint32_t idx = 0;
    if (XR_FAILED(xrAcquireSwapchainImage(g_strip.swapchain, &ai, &idx))) {
        return false;
    }
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(g_strip.swapchain, &wi);

    const float amber[4] = {1.0f, 0.667f, 0.0f, 1.0f};
    ClearImageToColor(device, pool, queue, imgs[idx].image, amber);

    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g_strip.swapchain, &ri);
    return true;
}

static bool EnsureWishMask(AppXrSession& xr)
{
    if (g_zoneMask != XR_NULL_HANDLE) return true;
    if (g_pfnCreateZoneMask == nullptr) return false;
    XrLocal3DZoneMaskCreateInfoDXR mci = {(XrStructureType)XR_TYPE_LOCAL_3D_ZONE_MASK_CREATE_INFO_DXR};
    mci.maskWidth = 0; // runtime picks the window backing size
    mci.maskHeight = 0;
    XrResult r = g_pfnCreateZoneMask(xr.session, &mci, &g_zoneMask);
    if (XR_FAILED(r)) {
        LOG_ERROR("[zones] xrCreateLocal3DZoneMaskDXR failed (0x%x)", (unsigned)r);
        g_zoneMask = XR_NULL_HANDLE;
        return false;
    }
    LOG_INFO("[zones] wish mask created (window backing size)");
    return true;
}

static void ApplyWishAuthoring(AppXrSession& xr)
{
    if (g_wishMode == 1) {
        if (!EnsureWishMask(xr) || g_pfnSetZoneRects == nullptr) return;
        XrRect2Di rects[kNumZones];
        for (uint32_t zi = 0; zi < kNumZones; zi++) rects[zi] = g_zonesArr[zi].rect;
        XrResult r = g_pfnSetZoneRects(g_zoneMask, kNumZones, rects);
        if (XR_FAILED(r)) {
            LOG_ERROR("[zones] xrSetLocal3DZoneFromRectsDXR failed (0x%x)", (unsigned)r);
        }
    }
}

static const char* WishModeName(int mode)
{
    return mode == 1 ? "explicit Tier-2 rects" : "AUTO";
}

// Edge-triggered M (wish mode cycle) + O (zone B overlap toggle).
static void HandleZoneKeys(AppXrSession& xr)
{
    if (g_wishModeCycleRequested) {
        g_wishModeCycleRequested = false;
        g_wishMode = (g_wishMode + 1) % 2;
        LOG_INFO("[zones] wish mode %d (%s)", g_wishMode, WishModeName(g_wishMode));
        ApplyWishAuthoring(xr);
    }
    if (g_overlapToggleRequested) {
        g_overlapToggleRequested = false;
        g_zoneBOverlap = !g_zoneBOverlap;
        g_zonesArr[1].rect = g_zoneBOverlap ? g_zoneBOverlapRect : g_zoneBRect;
        LOG_INFO("[zones] zone B rect -> %d,%d %dx%d (%s zone A)",
                 g_zonesArr[1].rect.offset.x, g_zonesArr[1].rect.offset.y,
                 g_zonesArr[1].rect.extent.width, g_zonesArr[1].rect.extent.height,
                 g_zoneBOverlap ? "OVERLAPPING" : "beside");
        ApplyWishAuthoring(xr);
    }
}

// One-time zones activation: capabilities check + per-zone swapchains + strip.
static void TryActivateZones(AppXrSession& xr, VkRenderer& renderer,
                             VkDevice device, VkCommandPool pool, VkQueue queue)
{
    const int32_t W = (int32_t)g_windowW;
    const int32_t H = (int32_t)g_windowH;
    if (W <= 0 || H <= 0) {
        return; // window metrics not settled — retry next frame
    }
    g_zonesAttempted = true;

    XrDisplayZoneCapabilitiesDXR caps = {XR_TYPE_DISPLAY_ZONE_CAPABILITIES_DXR};
    XrResult r = g_pfnGetZoneCaps(xr.session, &caps);
    if (XR_FAILED(r) || !caps.supported) {
        LOG_ERROR("[zones] xrGetDisplayZoneCapabilitiesDXR: rc=0x%x supported=%d — zones path disabled",
                  (unsigned)r, (int)caps.supported);
        g_hasDisplayZonesExt = false;
        return;
    }
    if (caps.maxZones3D < kNumZones) {
        LOG_ERROR("[zones] maxZones3D=%u < %u — zones path disabled", caps.maxZones3D, kNumZones);
        g_hasDisplayZonesExt = false;
        return;
    }
    LOG_INFO("[zones] capabilities: supported=1 maxZones3D=%u", caps.maxZones3D);

    // D3D11/Metal zones-app proportions of the window backing size.
    g_stripRect        = {{0, 0}, {W, H / 4}};
    g_zoneARect        = {{0, H / 4}, {W / 2, H * 3 / 4}};
    g_zoneBRect        = {{W * 700 / 1280, H / 4}, {W * 520 / 1280, H / 2}};
    g_zoneBOverlapRect = {{W * 400 / 1280, H * 300 / 720}, {W * 520 / 1280, H / 2}};

    // The Linux handle app renders a fixed 2-view SBS (no rendering-mode
    // enumeration), so each zone tiles 2 views.
    const uint32_t viewCount = 2;

    // Zone A: opaque dark red. Zone B: SEMI-TRANSPARENT dark blue
    // (premultiplied, alpha 0.55) — overlap oracle (background regions):
    // B + A*(1-0.55).
    g_zonesArr[0].zoneId = 1;
    g_zonesArr[0].rect = g_zoneARect;
    g_zonesArr[0].ipdFactor = 1.0f;
    g_zonesArr[0].perspectiveFactor = 1.0f;
    g_zonesArr[0].spinPhase = 0.0f;
    g_zonesArr[0].clearColor[0] = 0.15f;
    g_zonesArr[0].clearColor[1] = 0.03f;
    g_zonesArr[0].clearColor[2] = 0.03f;
    g_zonesArr[0].clearColor[3] = 1.0f;

    g_zonesArr[1].zoneId = 2;
    g_zonesArr[1].rect = g_zoneBOverlap ? g_zoneBOverlapRect : g_zoneBRect;
    g_zonesArr[1].ipdFactor = 0.6f;
    g_zonesArr[1].perspectiveFactor = 0.5f;
    g_zonesArr[1].spinPhase = 1.5f;
    g_zonesArr[1].clearColor[0] = 0.03f * 0.55f;
    g_zonesArr[1].clearColor[1] = 0.03f * 0.55f;
    g_zonesArr[1].clearColor[2] = 0.15f * 0.55f;
    g_zonesArr[1].clearColor[3] = 0.55f;

    for (uint32_t zi = 0; zi < kNumZones; zi++) {
        if (!CreateZoneResources(xr, renderer, zi, g_zonesArr[zi], viewCount)) {
            g_hasDisplayZonesExt = false;
            return;
        }
    }
    if (!CreateAndFillStrip(xr, device, pool, queue)) {
        g_hasDisplayZonesExt = false;
        return;
    }

    g_zonesActive = true;
    if (g_wishMode == 1) {
        ApplyWishAuthoring(xr);
    }
    LOG_INFO("[zones] ACTIVE: zone A %d,%d %dx%d + zone B %d,%d %dx%d + strip %d,%d %dx%d "
             "(views=%u, wish mode %d %s, validate=%d) — M=wish mode, O=overlap toggle",
             g_zoneARect.offset.x, g_zoneARect.offset.y, g_zoneARect.extent.width, g_zoneARect.extent.height,
             g_zonesArr[1].rect.offset.x, g_zonesArr[1].rect.offset.y,
             g_zonesArr[1].rect.extent.width, g_zonesArr[1].rect.extent.height,
             g_stripRect.offset.x, g_stripRect.offset.y, g_stripRect.extent.width, g_stripRect.extent.height,
             viewCount, g_wishMode, WishModeName(g_wishMode), ZonesValidateEnabled() ? 1 : 0);
}

// Per-frame zones path: zone-scoped locate, per-zone cube + grid render
// (per-zone clear color, rig framing, and spin phase), submit
// [projA, projB, strip] with the zone structs chained on the projections.
static void RenderZonesFrame(AppXrSession& xr, VkRenderer& renderer, const XrFrameState& frameState)
{
    XrDisplayZoneDXR zoneStructs[kNumZones];
    XrDisplayRigDXR rigStructs[kNumZones];
    std::vector<XrCompositionLayerProjectionView> projViews[kNumZones];
    uint32_t submitViewCounts[kNumZones] = {};

    for (uint32_t zi = 0; zi < kNumZones; zi++) {
        DisplayZone& z = g_zonesArr[zi];

        rigStructs[zi] = {XR_TYPE_DISPLAY_RIG_DXR};
        rigStructs[zi].pose = {{0, 0, 0, 1}, {0, 0, 0}};
        rigStructs[zi].virtualDisplayHeight = kZoneVirtualDisplayHeight;
        rigStructs[zi].ipdFactor = z.ipdFactor;
        rigStructs[zi].parallaxFactor = 1.0f;
        rigStructs[zi].perspectiveFactor = z.perspectiveFactor;

        zoneStructs[zi] = {XR_TYPE_DISPLAY_ZONE_DXR};
        zoneStructs[zi].next = &rigStructs[zi];
        zoneStructs[zi].zoneId = z.zoneId;
        zoneStructs[zi].rect = z.rect;

        XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
        locateInfo.next = &zoneStructs[zi];
        locateInfo.viewConfigurationType = xr.viewConfigType;
        locateInfo.displayTime = frameState.predictedDisplayTime;
        locateInfo.space = xr.localSpace;

        XrViewState viewState = {XR_TYPE_VIEW_STATE};
        uint32_t viewCountOutput = 0;
        XrView zoneViews[8];
        for (uint32_t vi = 0; vi < 8; vi++) zoneViews[vi] = {XR_TYPE_VIEW};
        XrResult lr = xrLocateViews(xr.session, &locateInfo, &viewState, 8, &viewCountOutput, zoneViews);
        if (XR_FAILED(lr) || viewCountOutput == 0) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                LOG_WARN("[zones] zone %u zone-scoped xrLocateViews failed (0x%x)", z.zoneId, (unsigned)lr);
            }
            submitViewCounts[zi] = 0;
            continue;
        }

        const uint32_t n = viewCountOutput < z.tileCount ? viewCountOutput : z.tileCount;
        submitViewCounts[zi] = n;
        projViews[zi].assign(n, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});

        // Render-ready views -> matrices. ZDP-anchored clip: near = ez - vH,
        // far = ez + 1000*vH (identity rig here, so ez = pose z). GL-convention
        // fov matrix remapped to Vulkan's [0,1] clip.
        std::vector<EyeRenderParams> eyes(n);
        for (uint32_t vi = 0; vi < n; vi++) {
            const XrView& v = zoneViews[vi];
            const float ez = RigLocalEyeZ(rigStructs[zi].pose, v.pose.position);
            const float vH = kZoneVirtualDisplayHeight;
            const float nearZ = (ez - vH > 0.001f) ? (ez - vH) : 0.001f;
            const float farZ = ez + 1000.0f * vH;
            mat4_view_from_xr_pose(eyes[vi].viewMat, v.pose);
            mat4_from_xr_fov(eyes[vi].projMat, v.fov, nearZ, farZ);
            convert_projection_gl_to_zero_to_one(eyes[vi].projMat);
            eyes[vi].viewportX = vi * z.tileW;
            eyes[vi].viewportY = 0;
            eyes[vi].width = z.tileW;
            eyes[vi].height = z.tileH;

            projViews[zi][vi].subImage.swapchain = z.swapchain;
            projViews[zi][vi].subImage.imageRect.offset = {(int32_t)(vi * z.tileW), 0};
            projViews[zi][vi].subImage.imageRect.extent = {(int32_t)z.tileW, (int32_t)z.tileH};
            projViews[zi][vi].subImage.imageArrayIndex = 0;
            projViews[zi][vi].pose = v.pose;
            projViews[zi][vi].fov = v.fov;
        }

        XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        uint32_t imageIndex = 0;
        if (XR_FAILED(xrAcquireSwapchainImage(z.swapchain, &ai, &imageIndex))) {
            submitViewCounts[zi] = 0;
            continue;
        }
        XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(z.swapchain, &wi);

        RenderSceneToFramebuffer(renderer, g_zoneFbs[zi], imageIndex, eyes.data(), (int)n,
                                 z.clearColor, z.spinPhase);

        XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(z.swapchain, &ri);
    }

    // Layer list: [projA (zone A chained), projB (zone B chained), strip].
    XrCompositionLayerProjection projLayers[kNumZones];
    XrCompositionLayerLocal2DDXR stripLayer = {(XrStructureType)XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_DXR};
    const XrCompositionLayerBaseHeader* layers[kNumZones + 1] = {};
    uint32_t layerCount = 0;

    for (uint32_t zi = 0; zi < kNumZones; zi++) {
        if (submitViewCounts[zi] == 0) continue;
        projLayers[zi] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projLayers[zi].next = &zoneStructs[zi]; // SAME instance as the locate chain
        projLayers[zi].layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        projLayers[zi].space = xr.localSpace;
        projLayers[zi].viewCount = submitViewCounts[zi];
        projLayers[zi].views = projViews[zi].data();
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&projLayers[zi];
    }

    if (g_strip.swapchain != XR_NULL_HANDLE) {
        stripLayer.layerFlags = 0; // opaque content
        stripLayer.subImage.swapchain = g_strip.swapchain;
        stripLayer.subImage.imageRect.offset = {0, 0};
        stripLayer.subImage.imageRect.extent = {(int32_t)g_strip.w, (int32_t)g_strip.h};
        stripLayer.subImage.imageArrayIndex = 0;
        stripLayer.rect = g_stripRect;
        layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&stripLayer;
    }

    // Zones alpha-composite against the desktop by design — submit
    // ALPHA_BLEND whenever the runtime advertises it.
    static XrEnvironmentBlendMode zonesBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    static bool blendModeResolved = false;
    if (!blendModeResolved) {
        blendModeResolved = true;
        XrEnvironmentBlendMode modes[8];
        uint32_t count = 0;
        if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(xr.instance, xr.systemId,
                                                          xr.viewConfigType, 8, &count, modes))) {
            for (uint32_t i = 0; i < count; i++) {
                if (modes[i] == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) {
                    zonesBlendMode = XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
                    break;
                }
            }
        }
        LOG_INFO("[zones] environment blend mode: %s",
                 zonesBlendMode == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND ? "ALPHA_BLEND" : "OPAQUE");
    }

    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = zonesBlendMode;
    endInfo.layerCount = layerCount;
    endInfo.layers = layers;

    XrDisplayZonesFrameEndInfoDXR zonesEnd = {(XrStructureType)XR_TYPE_DISPLAY_ZONES_FRAME_END_INFO_DXR};
    zonesEnd.flags = 0;
    zonesEnd.wishMask = XR_NULL_HANDLE;
    bool chainZonesEnd = false;
    if (g_wishMode >= 1 && g_zoneMask != XR_NULL_HANDLE) {
        zonesEnd.wishMask = g_zoneMask;
        chainZonesEnd = true;
    }
    if (ZonesValidateEnabled()) {
        zonesEnd.flags |= XR_DISPLAY_ZONES_FRAME_END_VALIDATE_BIT_DXR;
        chainZonesEnd = true;
    }
    if (chainZonesEnd) {
        endInfo.next = &zonesEnd;
    }

    xrEndFrame(xr.session, &endInfo);
}

// ============================================================================
// Main
// ============================================================================

static void SignalHandler(int sig) {
    (void)sig;
    g_running = false;
}

int main() {
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    LOG_INFO("=== VK Cube Zones Linux (XR_DXR_display_zones, ADR-027) ===");

    // Headless validation knobs: preselect the M / O key states.
    {
        const char *e = getenv("DXR_ZONES_WISH_MODE");
        if (e != nullptr && *e == '1') {
            g_wishMode = 1;
            LOG_INFO("DXR_ZONES_WISH_MODE=1 — starting in explicit Tier-2 rects wish mode");
        }
        e = getenv("DXR_ZONES_OVERLAP");
        if (e != nullptr && *e == '1') {
            g_zoneBOverlap = true;
            LOG_INFO("DXR_ZONES_OVERLAP=1 — zone B starts on the overlap rect");
        }
    }

    // Initialize OpenXR FIRST — xrGetSystemProperties needs only instance +
    // system id, and returns the panel desktop position the window below is
    // created at (INV-1.3 ordering: instance → system → properties → window
    // → session).
    AppXrSession xr = {};
    if (!InitializeOpenXR(xr)) {
        LOG_ERROR("OpenXR initialization failed");
        return 1;
    }

    // Create the app-owned X11 window on the 3D panel — it is passed to the
    // runtime at xrCreateSession via XR_DXR_xlib_window_binding. X11 has no
    // Retina scaling, so the window size IS the backing size the zone rects are
    // proportions of.
    g_windowW = 1600;
    g_windowH = 900;
    if (!CreateAppWindow(g_windowW, g_windowH, xr.displayScreenLeft, xr.displayScreenTop)) {
        LOG_ERROR("X11 window creation failed");
        CleanupOpenXR(xr);
        return 1;
    }

    if (!GetVulkanGraphicsRequirements(xr)) {
        LOG_ERROR("Failed to get Vulkan graphics requirements");
        CleanupOpenXR(xr);
        return 1;
    }

    // Create Vulkan instance
    VkInstance vkInstance = VK_NULL_HANDLE;
    if (!CreateVulkanInstance(xr, vkInstance)) {
        LOG_ERROR("Vulkan instance creation failed");
        CleanupOpenXR(xr);
        return 1;
    }

    // Get physical device
    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    if (!GetVulkanPhysicalDevice(xr, vkInstance, physDevice)) {
        LOG_ERROR("Failed to get Vulkan physical device");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        return 1;
    }

    // Get device extensions
    std::vector<const char*> deviceExtensions;
    std::vector<std::string> extensionStorage;
    if (!GetVulkanDeviceExtensions(xr, deviceExtensions, extensionStorage, physDevice)) {
        LOG_ERROR("Failed to get Vulkan device extensions");
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        return 1;
    }

    // Enable VK_KHR_maintenance1 for negative viewport height (Y-flip).
    // Without this extension, negative viewport height is undefined in Vulkan 1.0.
    deviceExtensions.push_back(VK_KHR_MAINTENANCE1_EXTENSION_NAME);

    // Find graphics queue
    uint32_t queueFamilyIndex = 0;
    if (!FindGraphicsQueueFamily(physDevice, queueFamilyIndex)) {
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        return 1;
    }

    // Create logical device
    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    if (!CreateVulkanDevice(physDevice, queueFamilyIndex, deviceExtensions, vkDevice, graphicsQueue)) {
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        return 1;
    }

    // Create session
    if (!CreateSession(xr, vkInstance, physDevice, vkDevice, queueFamilyIndex)) {
        LOG_ERROR("Session creation failed");
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr);
        return 1;
    }

    if (!CreateSpaces(xr)) {
        LOG_ERROR("Space creation failed");
        CleanupOpenXR(xr);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        return 1;
    }

    if (!CreateSwapchain(xr)) {
        LOG_ERROR("Swapchain creation failed");
        CleanupOpenXR(xr);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        return 1;
    }

    // Enumerate Vulkan swapchain images
    uint32_t scImageCount = xr.swapchain.imageCount;
    std::vector<XrSwapchainImageVulkanKHR> swapchainImages(scImageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    xrEnumerateSwapchainImages(xr.swapchain.swapchain, scImageCount, &scImageCount,
        (XrSwapchainImageBaseHeader*)swapchainImages.data());
    LOG_INFO("Enumerated %u Vulkan swapchain images", scImageCount);

    // Initialize Vulkan renderer
    VkFormat colorFormat = (VkFormat)xr.swapchain.format;
    VkRenderer vkRenderer = {};
    if (!InitializeVkRenderer(vkRenderer, vkDevice, physDevice, graphicsQueue, queueFamilyIndex, colorFormat)) {
        LOG_ERROR("Vulkan renderer initialization failed");
        CleanupOpenXR(xr);
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
        return 1;
    }

    // Create framebuffers for the single SBS swapchain
    {
        std::vector<VkImage> images(scImageCount);
        for (uint32_t i = 0; i < scImageCount; i++) {
            images[i] = swapchainImages[i].image;
        }

        if (!CreateSwapchainFramebuffers(vkRenderer, images.data(), scImageCount,
            xr.swapchain.width, xr.swapchain.height, colorFormat)) {
            LOG_ERROR("Failed to create framebuffers");
            CleanupVkRenderer(vkRenderer);
            CleanupOpenXR(xr);
            vkDestroyDevice(vkDevice, nullptr);
            vkDestroyInstance(vkInstance, nullptr);
            return 1;
        }
    }

    LOG_INFO("=== Entering main loop (Ctrl+C to exit) ===");

    // Frame timing
    auto lastTime = std::chrono::high_resolution_clock::now();

    while (g_running && !xr.exitRequested) {
        // Delta time
        auto now = std::chrono::high_resolution_clock::now();
        float deltaTime = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        // Update scene
        vkRenderer.cubeRotation += deltaTime * 0.5f;
        if (vkRenderer.cubeRotation > 2.0f * 3.14159265f) {
            vkRenderer.cubeRotation -= 2.0f * 3.14159265f;
        }

        PumpAppWindowEvents();
        PollEvents(xr);

        if (xr.sessionRunning) {
            XrFrameState frameState;
            if (BeginFrame(xr, frameState)) {
                // ---- zones path (XR_DXR_display_zones, ADR-027) -------------
                g_zonesFrameCounter++;
                if (g_hasDisplayZonesExt && !g_zonesActive && !g_zonesAttempted &&
                    g_zonesFrameCounter >= kZonesActivationFrame) {
                    TryActivateZones(xr, vkRenderer, vkDevice, vkRenderer.commandPool, graphicsQueue);
                }
                if (g_zonesActive && g_hasDisplayZonesExt) {
                    HandleZoneKeys(xr);
                    if (frameState.shouldRender) {
                        RenderZonesFrame(xr, vkRenderer, frameState);
                    } else {
                        XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
                        endInfo.displayTime = frameState.predictedDisplayTime;
                        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                        endInfo.layerCount = 0;
                        endInfo.layers = nullptr;
                        xrEndFrame(xr.session, &endInfo);
                    }
                    continue;
                }

                // ---- fallback: plain 2-view SBS cube ------------------------
                bool rendered = false;
                uint32_t viewCount = 0;
                std::vector<XrCompositionLayerProjectionView> projectionViews;

                if (frameState.shouldRender) {
                    // Locate views — first query count, then fetch
                    XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                    locateInfo.viewConfigurationType = xr.viewConfigType;
                    locateInfo.displayTime = frameState.predictedDisplayTime;
                    locateInfo.space = xr.localSpace;

                    XrViewState viewState = {XR_TYPE_VIEW_STATE};
                    // Query view count
                    xrLocateViews(xr.session, &locateInfo, &viewState, 0, &viewCount, nullptr);
                    if (viewCount == 0) viewCount = 2; // fallback

                    std::vector<XrView> views(viewCount, {XR_TYPE_VIEW});
                    XrResult locResult = xrLocateViews(xr.session, &locateInfo, &viewState, viewCount, &viewCount, views.data());
                    if (XR_SUCCEEDED(locResult) &&
                        (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                        (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
                    {
                        uint32_t imageIndex;
                        if (AcquireSwapchainImage(xr, imageIndex)) {
                            rendered = true;

                            // Legacy app: always render 2 stereo views in SBS layout.
                            // View dimensions come from recommendedImageRectWidth/Height (set at init).
                            // The runtime handles 2D/3D mode switching — we just render the same SBS atlas.
                            uint32_t eyeCount = 2;
                            uint32_t eyeW = xr.viewWidth;
                            uint32_t eyeH = xr.viewHeight;

                            EyeRenderParams eyeParams[2];
                            projectionViews.resize(eyeCount, {});
                            for (uint32_t i = 0; i < eyeCount; i++) {
                                eyeParams[i].viewportX = i * eyeW;  // SBS: left eye at 0, right at eyeW
                                eyeParams[i].viewportY = 0;
                                eyeParams[i].width = eyeW;
                                eyeParams[i].height = eyeH;
                                mat4_view_from_xr_pose(eyeParams[i].viewMat, views[i].pose);
                                mat4_from_xr_fov(eyeParams[i].projMat, views[i].fov, 0.01f, 100.0f);

                                projectionViews[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[i].subImage.swapchain = xr.swapchain.swapchain;
                                projectionViews[i].subImage.imageRect.offset = {(int32_t)(i * eyeW), 0};
                                projectionViews[i].subImage.imageRect.extent = {(int32_t)eyeW, (int32_t)eyeH};
                                projectionViews[i].subImage.imageArrayIndex = 0;
                                projectionViews[i].pose = views[i].pose;
                                projectionViews[i].fov = views[i].fov;
                            }

                            RenderScene(vkRenderer, imageIndex, eyeParams, (int)eyeCount);
                            ReleaseSwapchainImage(xr);
                        }
                    }
                }

                if (rendered) {
                    EndFrame(xr, frameState.predictedDisplayTime, projectionViews.data(), (uint32_t)projectionViews.size());
                } else {
                    // Submit empty frame
                    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
                    endInfo.displayTime = frameState.predictedDisplayTime;
                    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                    endInfo.layerCount = 0;
                    endInfo.layers = nullptr;
                    xrEndFrame(xr.session, &endInfo);
                }
            }
        } else {
            // Session not running — sleep to avoid busy-waiting
#ifdef _WIN32
            Sleep(100);
#else
            usleep(100000);
#endif
        }
    }

    // Request exit if we got SIGINT but session is still running
    if (!xr.exitRequested && xr.session != XR_NULL_HANDLE && xr.sessionRunning) {
        LOG_INFO("Requesting session exit...");
        xrRequestExitSession(xr.session);
        // Drain events until exit completes
        for (int i = 0; i < 100 && !xr.exitRequested; i++) {
            PollEvents(xr);
#ifdef _WIN32
            Sleep(10);
#else
            usleep(10000);
#endif
        }
    }

    LOG_INFO("=== Shutting down ===");

    // Zones teardown (before the renderer/session): destroy per-zone framebuffer
    // sets + swapchains, the Local2D strip, and the wish mask.
    vkDeviceWaitIdle(vkDevice);
    for (uint32_t zi = 0; zi < kNumZones; zi++) {
        DestroyFramebuffers(vkDevice, g_zoneFbs[zi]);
        if (g_zonesArr[zi].swapchain != XR_NULL_HANDLE) {
            xrDestroySwapchain(g_zonesArr[zi].swapchain);
            g_zonesArr[zi].swapchain = XR_NULL_HANDLE;
        }
    }
    if (g_strip.swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(g_strip.swapchain);
        g_strip.swapchain = XR_NULL_HANDLE;
    }
    if (g_zoneMask != XR_NULL_HANDLE && g_pfnDestroyZoneMask != nullptr) {
        g_pfnDestroyZoneMask(g_zoneMask);
        g_zoneMask = XR_NULL_HANDLE;
    }

    CleanupVkRenderer(vkRenderer);
    CleanupOpenXR(xr);
    vkDestroyDevice(vkDevice, nullptr);
    vkDestroyInstance(vkInstance, nullptr);

    // Last: the session's VkSurface borrowed this Display's XCB connection.
    DestroyAppWindow();

    LOG_INFO("Application shutdown complete");
    return 0;
}
