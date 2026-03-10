// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  macOS Vulkan OpenXR 3D Gaussian Splatting with external window binding
 *
 * Renders 3DGS scenes on tracked 3D displays via OpenXR.
 * Based on cube_ext_vk_macos with the cube/grid renderer replaced by
 * a 3DGS.cpp compute pipeline.  Features a "Load Scene" button overlay.
 *
 * Features:
 * - App creates and owns the NSWindow (XR_EXT_cocoa_window_binding)
 * - Mouse drag camera, WASD/QE movement, scroll zoom
 * - XR_EXT_display_info: Kooima projection, display metrics
 * - V key cycles rendering modes via xrRequestDisplayRenderingModeEXT
 * - 0-3 keys select rendering mode directly
 * - L key or button click: NSOpenPanel to load .ply/.spz scenes
 * - Tab: toggle HUD overlay, Space: reset camera, ESC: quit
 */

#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/XR_EXT_cocoa_window_binding.h>
#include <openxr/XR_EXT_display_info.h>

#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <chrono>
#include <vector>

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <unistd.h>

#include "stereo_params.h"
#include "display3d_view.h"
#include "camera3d_view.h"
#include "gs_renderer.h"
#include "gs_scene_loader.h"

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
// Input state
// ============================================================================

struct InputState {
    float yaw = 0.0f;
    float pitch = 0.0f;
    bool keyW = false, keyA = false, keyS = false, keyD = false;
    bool keyE = false, keyQ = false;
    float cameraPosX = 0.0f, cameraPosY = 0.0f, cameraPosZ = 0.0f;
    bool resetViewRequested = false;
    StereoParams stereo;
    bool hudVisible = true;
    bool cameraMode = false;
    float nominalViewerZ = 0.5f;
    bool eyeTrackingModeToggleRequested = false;
    bool loadRequested = false;
    bool teleportRequested = false;
    float teleportMouseX = 0.0f, teleportMouseY = 0.0f; // logical points

    // Teleport animation
    bool teleportAnimating = false;
    float teleportTargetX = 0, teleportTargetY = 0, teleportTargetZ = 0;

    // Unified rendering mode (V key cycles, 0-8 keys select directly)
    uint32_t currentRenderingMode = 1;   // Default: mode 1 (first 3D mode)
    uint32_t renderingModeCount = 0;     // Set from xrEnumerateDisplayRenderingModesEXT
    bool renderingModeChangeRequested = false;
};

// ============================================================================
// Globals
// ============================================================================

static volatile bool g_running = true;
static NSWindow *g_window = nil;
static NSView *g_metalView = nil;
static InputState g_input;
static const float CAMERA_HALF_TAN_VFOV = 0.32491969623f;

typedef void (*PFN_sim_display_set_output_mode)(int mode);
static PFN_sim_display_set_output_mode g_pfnSetOutputMode = nullptr;

static bool g_fullscreen = false;
static NSRect g_savedWindowFrame = {};
static NSUInteger g_savedWindowStyle = 0;

// 3DGS state
static GsRenderer g_gsRenderer;
static std::string g_loadedFileName;

static double g_avgFrameTime = 0.0;
static uint64_t g_frameCount = 0;
static float g_hudUpdateTimer = 0.0f;
static uint32_t g_renderW = 0, g_renderH = 0;
static uint32_t g_windowW = 0, g_windowH = 0;

// ============================================================================
// Inline math — column-major float[16] matrices
// ============================================================================

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
    m[12] = tx; m[13] = ty; m[14] = tz;
}

static void mat4_from_xr_fov(float* m, XrFovf fov, float nearZ, float farZ) {
    float tanL = tanf(fov.angleLeft);
    float tanR = tanf(fov.angleRight);
    float tanU = tanf(fov.angleUp);
    float tanD = tanf(fov.angleDown);
    float w = tanR - tanL;
    float h = tanU - tanD;
    memset(m, 0, 16 * sizeof(float));
    m[0]  = 2.0f / w;
    m[5]  = 2.0f / h;
    m[8]  = (tanR + tanL) / w;
    m[9]  = (tanU + tanD) / h;
    m[10] = -(farZ + nearZ) / (farZ - nearZ);
    m[11] = -1.0f;
    m[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
}

static void mat4_view_from_xr_pose(float* viewMat, XrPosef pose) {
    float qx = pose.orientation.x, qy = pose.orientation.y;
    float qz = pose.orientation.z, qw = pose.orientation.w;
    float rot[16];
    mat4_identity(rot);
    rot[0]  = 1 - 2*(qy*qy + qz*qz);
    rot[1]  = 2*(qx*qy + qz*qw);
    rot[2]  = 2*(qx*qz - qy*qw);
    rot[4]  = 2*(qx*qy - qz*qw);
    rot[5]  = 1 - 2*(qx*qx + qz*qz);
    rot[6]  = 2*(qy*qz + qx*qw);
    rot[8]  = 2*(qx*qz + qy*qw);
    rot[9]  = 2*(qy*qz - qx*qw);
    rot[10] = 1 - 2*(qx*qx + qy*qy);
    float invRot[16];
    mat4_identity(invRot);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            invRot[j*4+i] = rot[i*4+j];
    float invTrans[16];
    mat4_translation(invTrans, -pose.position.x, -pose.position.y, -pose.position.z);
    mat4_multiply(viewMat, invRot, invTrans);
}

static void quat_from_yaw_pitch(float yaw, float pitch, XrQuaternionf* out) {
    float cy = cosf(yaw / 2.0f), sy = sinf(yaw / 2.0f);
    float cp = cosf(pitch / 2.0f), sp = sinf(pitch / 2.0f);
    out->w = cy * cp;
    out->x = cy * sp;
    out->y = sy * cp;
    out->z = -sy * sp;
}

static void quat_rotate_vec3(XrQuaternionf q, float vx, float vy, float vz,
    float* ox, float* oy, float* oz) {
    float tx = 2.0f * (q.y * vz - q.z * vy);
    float ty = 2.0f * (q.z * vx - q.x * vz);
    float tz = 2.0f * (q.x * vy - q.y * vx);
    *ox = vx + q.w * tx + (q.y * tz - q.z * ty);
    *oy = vy + q.w * ty + (q.z * tx - q.x * tz);
    *oz = vz + q.w * tz + (q.x * ty - q.y * tx);
}

// ============================================================================
// Camera movement (ported from common/input_handler)
// ============================================================================

static void UpdateCameraMovement(InputState& input, float dt, float displayHeightM) {
    if (input.resetViewRequested) {
        input.yaw = 0; input.pitch = 0;
        input.cameraPosX = input.cameraPosY = input.cameraPosZ = 0;
        input.stereo = StereoParams();
        input.stereo.virtualDisplayHeight = 0.24f;
        input.resetViewRequested = false;
        input.teleportAnimating = false;
        return;
    }

    float speed = 0.15f;
    if (displayHeightM > 0.0f) speed *= displayHeightM / 0.1f;

    // Build orientation quaternion and derive basis vectors
    XrQuaternionf ori;
    quat_from_yaw_pitch(input.yaw, input.pitch, &ori);

    float fwdX, fwdY, fwdZ, rtX, rtY, rtZ, upX, upY, upZ;
    quat_rotate_vec3(ori, 0, 0, -1, &fwdX, &fwdY, &fwdZ);
    quat_rotate_vec3(ori, 1, 0, 0, &rtX, &rtY, &rtZ);
    quat_rotate_vec3(ori, 0, 1, 0, &upX, &upY, &upZ);

    float d = speed * dt;
    if (input.keyW) { input.cameraPosX += fwdX*d; input.cameraPosY += fwdY*d; input.cameraPosZ += fwdZ*d; }
    if (input.keyS) { input.cameraPosX -= fwdX*d; input.cameraPosY -= fwdY*d; input.cameraPosZ -= fwdZ*d; }
    if (input.keyD) { input.cameraPosX += rtX*d;  input.cameraPosY += rtY*d;  input.cameraPosZ += rtZ*d; }
    if (input.keyA) { input.cameraPosX -= rtX*d;  input.cameraPosY -= rtY*d;  input.cameraPosZ -= rtZ*d; }
    if (input.keyE) { input.cameraPosX += upX*d;  input.cameraPosY += upY*d;  input.cameraPosZ += upZ*d; }
    if (input.keyQ) { input.cameraPosX -= upX*d;  input.cameraPosY -= upY*d;  input.cameraPosZ -= upZ*d; }

    // Teleport animation: exponential ease-out
    if (input.teleportAnimating) {
        float t = 1.0f - expf(-10.0f * dt); // ~90% in 0.23s
        input.cameraPosX += (input.teleportTargetX - input.cameraPosX) * t;
        input.cameraPosY += (input.teleportTargetY - input.cameraPosY) * t;
        input.cameraPosZ += (input.teleportTargetZ - input.cameraPosZ) * t;
        float dx = input.teleportTargetX - input.cameraPosX;
        float dy = input.teleportTargetY - input.cameraPosY;
        float dz = input.teleportTargetZ - input.cameraPosZ;
        if (dx*dx + dy*dy + dz*dz < 1e-8f)
            input.teleportAnimating = false;
    }
}

// ============================================================================
// HUD overlay (simple NSView with CoreText)
// ============================================================================

@interface HudOverlayView : NSView
@property (nonatomic, strong) NSString *hudText;
@end

@implementation HudOverlayView

- (BOOL)isFlipped { return YES; }

- (void)drawRect:(NSRect)dirtyRect {
    [[NSColor colorWithCalibratedRed:0 green:0 blue:0 alpha:0.65] set];
    NSRectFill(self.bounds);

    if (!_hudText) return;

    NSDictionary *attrs = @{
        NSFontAttributeName: [NSFont monospacedSystemFontOfSize:10 weight:NSFontWeightRegular],
        NSForegroundColorAttributeName: [NSColor colorWithCalibratedRed:0.9 green:0.95 blue:1.0 alpha:1.0]
    };
    NSRect textRect = NSInsetRect(self.bounds, 8, 6);
    [_hudText drawInRect:textRect withAttributes:attrs];
}

@end

static HudOverlayView *g_hudView = nil;

// ============================================================================
// Load button overlay (NSButton)
// ============================================================================

static NSButton *g_loadButton = nil;

static void OpenLoadDialog() {
    @autoreleasepool {
        NSOpenPanel *panel = [NSOpenPanel openPanel];
        [panel setCanChooseFiles:YES];
        [panel setCanChooseDirectories:NO];
        [panel setAllowsMultipleSelection:NO];
        [panel setTitle:@"Load Gaussian Splatting Scene"];
        [panel setMessage:@"Select a .ply or .spz file containing 3D Gaussian splats"];

        if (@available(macOS 11.0, *)) {
            UTType *plyType = [UTType typeWithFilenameExtension:@"ply"];
            UTType *spzType = [UTType typeWithFilenameExtension:@"spz"];
            NSMutableArray<UTType *> *types = [NSMutableArray array];
            if (plyType) [types addObject:plyType];
            if (spzType) [types addObject:spzType];
            if (types.count > 0) {
                [panel setAllowedContentTypes:types];
            }
        }

        if ([panel runModal] == NSModalResponseOK) {
            NSURL *url = [[panel URLs] firstObject];
            if (url) {
                const char *path = [[url path] UTF8String];
                std::string pathStr(path);
                if (ValidateSceneFile(pathStr)) {
                    LOG_INFO("Loading 3DGS scene: %s", path);
                    if (g_gsRenderer.loadScene(path)) {
                        g_loadedFileName = GetPlyFilename(pathStr);
                        LOG_INFO("Scene loaded: %s (%s)", g_loadedFileName.c_str(),
                            GetPlyFileSize(pathStr).c_str());
                    } else {
                        LOG_ERROR("Failed to load scene: %s", path);
                        NSAlert *alert = [[NSAlert alloc] init];
                        [alert setMessageText:@"Failed to load scene file"];
                        [alert setInformativeText:@"The file may be corrupt or unsupported."];
                        [alert runModal];
                    }
                }
            }
        }
    }
}

// ============================================================================
// macOS Window + Metal Layer
// ============================================================================

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation AppDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)n { (void)n; }
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)a { (void)a; return YES; }
- (void)windowWillClose:(NSNotification *)n { (void)n; g_running = false; }
@end

@interface MetalView : NSView
@end

@implementation MetalView
- (CALayer*)makeBackingLayer {
    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    return layer;
}
- (BOOL)wantsLayer { return YES; }
- (BOOL)wantsUpdateLayer { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }

- (void)mouseDown:(NSEvent *)event {
    if ([event clickCount] >= 2) {
        NSPoint loc = [event locationInWindow];
        g_input.teleportRequested = true;
        g_input.teleportMouseX = (float)loc.x;
        g_input.teleportMouseY = (float)loc.y;
    }
    [super mouseDown:event];
}

- (void)mouseDragged:(NSEvent *)event {
    g_input.yaw += (float)[event deltaX] * 0.005f;
    g_input.pitch += (float)[event deltaY] * 0.005f;
    float maxPitch = 1.5f;
    if (g_input.pitch > maxPitch) g_input.pitch = maxPitch;
    if (g_input.pitch < -maxPitch) g_input.pitch = -maxPitch;
}

- (void)scrollWheel:(NSEvent *)event {
    float delta = (float)[event scrollingDeltaY] * 0.02f;
    NSUInteger flags = [event modifierFlags];
    if (flags & NSEventModifierFlagShift) {
        g_input.stereo.ipdFactor += delta * 0.5f;
        if (g_input.stereo.ipdFactor < 0.0f) g_input.stereo.ipdFactor = 0.0f;
        if (g_input.stereo.ipdFactor > 1.0f) g_input.stereo.ipdFactor = 1.0f;
        g_input.stereo.parallaxFactor += delta * 0.5f;
        if (g_input.stereo.parallaxFactor < 0.0f) g_input.stereo.parallaxFactor = 0.0f;
        if (g_input.stereo.parallaxFactor > 1.0f) g_input.stereo.parallaxFactor = 1.0f;
    } else {
        g_input.stereo.scaleFactor += delta * 0.5f;
        if (g_input.stereo.scaleFactor < 0.1f) g_input.stereo.scaleFactor = 0.1f;
    }
}

- (void)keyDown:(NSEvent *)event {
    NSString *chars = [event charactersIgnoringModifiers];
    if ([chars length] == 0) return;
    unichar ch = [chars characterAtIndex:0];
    switch (ch) {
        case 'w': case 'W': g_input.keyW = true; break;
        case 'a': case 'A': g_input.keyA = true; break;
        case 's': case 'S': g_input.keyS = true; break;
        case 'd': case 'D': g_input.keyD = true; break;
        case 'e': case 'E': g_input.keyE = true; break;
        case 'q': case 'Q': g_input.keyQ = true; break;
        case 'v': case 'V':
            // Cycle through all rendering modes
            if (g_input.renderingModeCount > 0) {
                g_input.currentRenderingMode = (g_input.currentRenderingMode + 1) % g_input.renderingModeCount;
            }
            g_input.renderingModeChangeRequested = true;
            break;
        case 'c': case 'C':
            g_input.cameraMode = !g_input.cameraMode;
            break;
        case 't': case 'T':
            g_input.eyeTrackingModeToggleRequested = true;
            break;
        case 'l': case 'L':
            g_input.loadRequested = true;
            break;
        case ' ':
            g_input.resetViewRequested = true;
            break;
        case '0':
            g_input.currentRenderingMode = 0;
            g_input.renderingModeChangeRequested = true;
            break;
        case '1':
            if (g_input.renderingModeCount > 1) g_input.currentRenderingMode = 1;
            g_input.renderingModeChangeRequested = true;
            break;
        case '2':
            if (g_input.renderingModeCount > 2) g_input.currentRenderingMode = 2;
            g_input.renderingModeChangeRequested = true;
            break;
        case '3':
            if (g_input.renderingModeCount > 3) g_input.currentRenderingMode = 3;
            g_input.renderingModeChangeRequested = true;
            break;
        case '\t':
            g_input.hudVisible = !g_input.hudVisible;
            break;
        case 27: // ESC
            g_running = false;
            break;
    }
}

- (void)keyUp:(NSEvent *)event {
    NSString *chars = [event charactersIgnoringModifiers];
    if ([chars length] == 0) return;
    unichar ch = [chars characterAtIndex:0];
    switch (ch) {
        case 'w': case 'W': g_input.keyW = false; break;
        case 'a': case 'A': g_input.keyA = false; break;
        case 's': case 'S': g_input.keyS = false; break;
        case 'd': case 'D': g_input.keyD = false; break;
        case 'e': case 'E': g_input.keyE = false; break;
        case 'q': case 'Q': g_input.keyQ = false; break;
    }
}

- (void)flagsChanged:(NSEvent *)event {
    // Cmd+Ctrl+F = fullscreen toggle
    NSUInteger flags = [event modifierFlags];
    (void)flags;
}
@end

static void SignalHandler(int sig) {
    (void)sig;
    g_running = false;
}

static bool CreateMacOSWindow(uint32_t width, uint32_t height) {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    AppDelegate *delegate = [[AppDelegate alloc] init];
    [NSApp setDelegate:delegate];

    NSRect frame = NSMakeRect(100, 100, width, height);
    NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                       NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable;
    g_window = [[NSWindow alloc] initWithContentRect:frame
        styleMask:style backing:NSBackingStoreBuffered defer:NO];
    [g_window setTitle:@"SR 3DGS OpenXR Ext macOS (Press ESC to exit)"];
    [g_window setDelegate:delegate];
    [g_window center];

    g_metalView = [[MetalView alloc] initWithFrame:frame];
    [g_window setContentView:g_metalView];
    [g_window makeKeyAndOrderFront:nil];
    [g_window makeFirstResponder:g_metalView];

    // Add HUD overlay
    NSRect hudFrame = NSMakeRect(8, 8, 320, 520);
    g_hudView = [[HudOverlayView alloc] initWithFrame:hudFrame];
    [g_metalView addSubview:g_hudView];

    // Add Load button overlay (top-right)
    g_loadButton = [[NSButton alloc] initWithFrame:NSMakeRect(width - 120, height - 40, 110, 30)];
    [g_loadButton setTitle:@"Load Scene"];
    [g_loadButton setBezelStyle:NSBezelStyleRounded];
    [g_loadButton setTarget:nil];
    [g_loadButton setAction:@selector(loadButtonClicked:)];
    [g_metalView addSubview:g_loadButton];

    [NSApp activateIgnoringOtherApps:YES];
    LOG_INFO("macOS window created (%ux%u)", width, height);
    return true;
}

// Button action handler (added as category on NSApplication)
@interface NSApplication (LoadAction)
- (void)loadButtonClicked:(id)sender;
@end

@implementation NSApplication (LoadAction)
- (void)loadButtonClicked:(id)sender {
    (void)sender;
    g_input.loadRequested = true;
}
@end

static void PumpMacOSEvents() {
    @autoreleasepool {
        NSEvent *event;
        while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
            untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES])) {
            [NSApp sendEvent:event];
        }
    }
}

static void ToggleBorderlessFullscreen() {
    if (g_fullscreen) {
        [g_window setStyleMask:g_savedWindowStyle];
        [g_window setFrame:g_savedWindowFrame display:YES animate:NO];
        [g_window setLevel:NSNormalWindowLevel];
        g_fullscreen = false;
        LOG_INFO("Exited fullscreen");
    } else {
        g_savedWindowStyle = [g_window styleMask];
        g_savedWindowFrame = [g_window frame];
        NSScreen *screen = [g_window screen] ?: [NSScreen mainScreen];
        [g_window setStyleMask:NSWindowStyleMaskBorderless];
        [g_window setFrame:[screen frame] display:YES animate:NO];
        [g_window setLevel:NSStatusWindowLevel];
        g_fullscreen = true;
        LOG_INFO("Entered fullscreen");
    }
}

// ============================================================================
// OpenXR Session (ported from cube_ext_vk_macos)
// ============================================================================

struct AppXrSession {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrViewConfigurationType viewConfigType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    bool sessionRunning = false;
    bool exitRequested = false;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    char systemName[256] = {};

    // Swapchain
    struct { XrSwapchain swapchain; uint32_t width, height, imageCount; int64_t format; } swapchain = {};

    // Display info from XR_EXT_display_info
    bool hasDisplayInfoExt = false;
    bool hasCocoaWindowBinding = false;
    float displayWidthM = 0, displayHeightM = 0;
    float nominalViewerX = 0, nominalViewerY = 0, nominalViewerZ = 0.5f;
    float recommendedViewScaleX = 0.5f, recommendedViewScaleY = 1.0f;
    bool supportsDisplayModeSwitch = false;
    uint32_t displayPixelWidth = 0, displayPixelHeight = 0;

    // Eye tracking
    float leftEyeX = 0, leftEyeY = 0, leftEyeZ = 0;
    float rightEyeX = 0, rightEyeY = 0, rightEyeZ = 0;
    bool eyeTrackingActive = false;
    bool isEyeTracking = false;
    uint32_t activeEyeTrackingMode = 0;
    uint32_t supportedEyeTrackingModes = 0;

    // Function pointers
    PFN_xrRequestDisplayModeEXT pfnRequestDisplayModeEXT = nullptr;
    PFN_xrRequestEyeTrackingModeEXT pfnRequestEyeTrackingModeEXT = nullptr;
    PFN_xrRequestDisplayRenderingModeEXT pfnRequestDisplayRenderingModeEXT = nullptr;
    PFN_xrEnumerateDisplayRenderingModesEXT pfnEnumerateDisplayRenderingModesEXT = nullptr;

    // Enumerated rendering mode info
    uint32_t renderingModeCount = 0;
    char renderingModeNames[8][XR_MAX_SYSTEM_NAME_SIZE] = {};
    uint32_t renderingModeViewCounts[8] = {};
    float renderingModeScaleX[8] = {};
    float renderingModeScaleY[8] = {};
    bool renderingModeDisplay3D[8] = {};

    void* windowHandle = nullptr;  // unused on macOS, kept for compatibility
};

// Forward declarations for OpenXR functions (same as cube_ext_vk_macos)
static bool InitializeOpenXR(AppXrSession& xr);
static bool GetVulkanGraphicsRequirements(AppXrSession& xr);
static bool CreateVulkanInstance(AppXrSession& xr, VkInstance& vkInstance);
static bool GetVulkanPhysicalDevice(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice& physDevice);
static bool GetVulkanDeviceExtensions(AppXrSession& xr, std::vector<const char*>& exts,
    std::vector<std::string>& storage, VkPhysicalDevice physDevice);
static bool FindGraphicsQueueFamily(VkPhysicalDevice physDevice, uint32_t& queueFamilyIndex);
static bool CreateVulkanDevice(VkPhysicalDevice physDevice, uint32_t queueFamilyIndex,
    const std::vector<const char*>& exts, VkDevice& device, VkQueue& queue);
static bool CreateSession(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice physDevice,
    VkDevice device, uint32_t queueFamilyIndex);
static bool CreateSpaces(AppXrSession& xr);
static bool CreateSwapchains(AppXrSession& xr);
static void PollEvents(AppXrSession& xr);
static bool BeginFrame(AppXrSession& xr, XrFrameState& frameState);
static bool AcquireSwapchainImage(AppXrSession& xr, uint32_t& imageIndex);
static void ReleaseSwapchainImage(AppXrSession& xr);
static void EndFrame(AppXrSession& xr, XrTime displayTime,
    XrCompositionLayerProjectionView* projViews, uint32_t viewCount);
static void CleanupOpenXR(AppXrSession& xr);

// ============================================================================
// OpenXR implementation (abbreviated — same logic as cube_ext_vk_macos)
// ============================================================================

static bool InitializeOpenXR(AppXrSession& xr) {
    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());

    bool hasVulkan = false;
    for (const auto& ext : exts) {
        if (strcmp(ext.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0) hasVulkan = true;
        if (strcmp(ext.extensionName, XR_EXT_COCOA_WINDOW_BINDING_EXTENSION_NAME) == 0) xr.hasCocoaWindowBinding = true;
        if (strcmp(ext.extensionName, XR_EXT_DISPLAY_INFO_EXTENSION_NAME) == 0) xr.hasDisplayInfoExt = true;
    }

    if (!hasVulkan) { LOG_ERROR("XR_KHR_vulkan_enable not available"); return false; }

    std::vector<const char*> enabled;
    enabled.push_back(XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    if (xr.hasCocoaWindowBinding) enabled.push_back(XR_EXT_COCOA_WINDOW_BINDING_EXTENSION_NAME);
    if (xr.hasDisplayInfoExt) enabled.push_back(XR_EXT_DISPLAY_INFO_EXTENSION_NAME);

    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy(ci.applicationInfo.applicationName, "SR3DGSOpenXRExtMacOS", sizeof(ci.applicationInfo.applicationName));
    ci.applicationInfo.applicationVersion = 1;
    strncpy(ci.applicationInfo.engineName, "None", sizeof(ci.applicationInfo.engineName));
    ci.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    ci.enabledExtensionCount = (uint32_t)enabled.size();
    ci.enabledExtensionNames = enabled.data();
    XR_CHECK(xrCreateInstance(&ci, &xr.instance));

    XrSystemGetInfo si = {XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(xr.instance, &si, &xr.systemId));

    { XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
      xrGetSystemProperties(xr.instance, xr.systemId, &sp);
      memcpy(xr.systemName, sp.systemName, sizeof(xr.systemName)); }

    if (xr.hasDisplayInfoExt) {
        XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
        XrDisplayInfoEXT di = {(XrStructureType)XR_TYPE_DISPLAY_INFO_EXT};
        XrEyeTrackingModeCapabilitiesEXT ec = {(XrStructureType)XR_TYPE_EYE_TRACKING_MODE_CAPABILITIES_EXT};
        di.next = &ec; sp.next = &di;
        if (XR_SUCCEEDED(xrGetSystemProperties(xr.instance, xr.systemId, &sp))) {
            xr.recommendedViewScaleX = di.recommendedViewScaleX;
            xr.recommendedViewScaleY = di.recommendedViewScaleY;
            xr.displayWidthM = di.displaySizeMeters.width;
            xr.displayHeightM = di.displaySizeMeters.height;
            xr.nominalViewerX = di.nominalViewerPositionInDisplaySpace.x;
            xr.nominalViewerY = di.nominalViewerPositionInDisplaySpace.y;
            xr.nominalViewerZ = di.nominalViewerPositionInDisplaySpace.z;
            xr.supportsDisplayModeSwitch = (di.supportsDisplayModeSwitch == XR_TRUE);
            xr.displayPixelWidth = di.displayPixelWidth;
            xr.displayPixelHeight = di.displayPixelHeight;
            xr.supportedEyeTrackingModes = (uint32_t)ec.supportedModes;
        }
        if (xr.supportsDisplayModeSwitch)
            xrGetInstanceProcAddr(xr.instance, "xrRequestDisplayModeEXT", (PFN_xrVoidFunction*)&xr.pfnRequestDisplayModeEXT);
        if (xr.supportedEyeTrackingModes != 0)
            xrGetInstanceProcAddr(xr.instance, "xrRequestEyeTrackingModeEXT", (PFN_xrVoidFunction*)&xr.pfnRequestEyeTrackingModeEXT);

        // Load unified rendering mode function pointers (v7)
        xrGetInstanceProcAddr(xr.instance, "xrRequestDisplayRenderingModeEXT", (PFN_xrVoidFunction*)&xr.pfnRequestDisplayRenderingModeEXT);
        xrGetInstanceProcAddr(xr.instance, "xrEnumerateDisplayRenderingModesEXT", (PFN_xrVoidFunction*)&xr.pfnEnumerateDisplayRenderingModesEXT);
    }

    LOG_INFO("OpenXR initialized: %s", xr.systemName);
    return true;
}

static bool GetVulkanGraphicsRequirements(AppXrSession& xr) {
    PFN_xrGetVulkanGraphicsRequirementsKHR fn = nullptr;
    xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&fn);
    if (!fn) return false;
    XrGraphicsRequirementsVulkanKHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    return XR_SUCCEEDED(fn(xr.instance, xr.systemId, &req));
}

static bool CreateVulkanInstance(AppXrSession& xr, VkInstance& vkInstance) {
    PFN_xrGetVulkanInstanceExtensionsKHR fn = nullptr;
    xrGetInstanceProcAddr(xr.instance, "xrGetVulkanInstanceExtensionsKHR", (PFN_xrVoidFunction*)&fn);
    if (!fn) return false;
    uint32_t bufSize = 0;
    fn(xr.instance, xr.systemId, 0, &bufSize, nullptr);
    std::string extStr(bufSize, '\0');
    fn(xr.instance, xr.systemId, bufSize, &bufSize, extStr.data());
    std::vector<std::string> extNames;
    std::vector<const char*> extPtrs;
    { size_t s = 0; while (s < extStr.size()) {
        size_t e = extStr.find(' ', s); if (e == std::string::npos) e = extStr.size();
        std::string n = extStr.substr(s, e - s);
        if (!n.empty() && n[0] != '\0') extNames.push_back(n);
        s = e + 1;
    }}
    // Add portability enumeration for MoltenVK
    extNames.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    for (auto& n : extNames) extPtrs.push_back(n.c_str());

    VkApplicationInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "SR3DGSOpenXRExtMacOS";
    ai.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    ci.pApplicationInfo = &ai;
    ci.enabledExtensionCount = (uint32_t)extPtrs.size();
    ci.ppEnabledExtensionNames = extPtrs.data();
    VK_CHECK(vkCreateInstance(&ci, nullptr, &vkInstance));
    return true;
}

static bool GetVulkanPhysicalDevice(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice& pd) {
    PFN_xrGetVulkanGraphicsDeviceKHR fn = nullptr;
    xrGetInstanceProcAddr(xr.instance, "xrGetVulkanGraphicsDeviceKHR", (PFN_xrVoidFunction*)&fn);
    if (!fn) return false;
    XR_CHECK(fn(xr.instance, xr.systemId, vkInstance, &pd));
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
    LOG_INFO("GPU: %s", props.deviceName);
    return true;
}

static bool GetVulkanDeviceExtensions(AppXrSession& xr, std::vector<const char*>& exts,
    std::vector<std::string>& storage, VkPhysicalDevice physDevice) {
    PFN_xrGetVulkanDeviceExtensionsKHR fn = nullptr;
    xrGetInstanceProcAddr(xr.instance, "xrGetVulkanDeviceExtensionsKHR", (PFN_xrVoidFunction*)&fn);
    if (!fn) return false;
    uint32_t bufSize = 0;
    fn(xr.instance, xr.systemId, 0, &bufSize, nullptr);
    std::string extStr(bufSize, '\0');
    fn(xr.instance, xr.systemId, bufSize, &bufSize, extStr.data());
    { size_t s = 0; while (s < extStr.size()) {
        size_t e = extStr.find(' ', s); if (e == std::string::npos) e = extStr.size();
        std::string n = extStr.substr(s, e - s);
        if (!n.empty() && n[0] != '\0') storage.push_back(n);
        s = e + 1;
    }}
    // Add portability subset for MoltenVK
    storage.push_back("VK_KHR_portability_subset");
    for (auto& n : storage) exts.push_back(n.c_str());
    return true;
}

static bool FindGraphicsQueueFamily(VkPhysicalDevice pd, uint32_t& idx) {
    uint32_t count = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
    std::vector<VkQueueFamilyProperties> fams(count);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, fams.data());
    for (uint32_t i = 0; i < count; i++) {
        if (fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { idx = i; return true; }
    }
    return false;
}

static bool CreateVulkanDevice(VkPhysicalDevice pd, uint32_t qfi,
    const std::vector<const char*>& exts, VkDevice& dev, VkQueue& queue) {
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {};
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = qfi; qi.queueCount = 1; qi.pQueuePriorities = &prio;

    VkPhysicalDeviceFeatures features = {};
    features.shaderInt64 = VK_TRUE;
    features.shaderStorageImageWriteWithoutFormat = VK_TRUE;

    VkDeviceCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = 1; ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = (uint32_t)exts.size(); ci.ppEnabledExtensionNames = exts.data();
    ci.pEnabledFeatures = &features;
    VK_CHECK(vkCreateDevice(pd, &ci, nullptr, &dev));
    vkGetDeviceQueue(dev, qfi, 0, &queue);
    return true;
}

static bool CreateSession(AppXrSession& xr, VkInstance vkInstance, VkPhysicalDevice pd,
    VkDevice dev, uint32_t qfi) {
    XrGraphicsBindingVulkanKHR vkBinding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    vkBinding.instance = vkInstance;
    vkBinding.physicalDevice = pd;
    vkBinding.device = dev;
    vkBinding.queueFamilyIndex = qfi;
    vkBinding.queueIndex = 0;

    XrCocoaWindowBindingCreateInfoEXT macBinding = {(XrStructureType)XR_TYPE_COCOA_WINDOW_BINDING_CREATE_INFO_EXT};
    macBinding.viewHandle = (__bridge void*)g_metalView;
    if (xr.hasCocoaWindowBinding && g_metalView) {
        vkBinding.next = &macBinding;
        LOG_INFO("Using XR_EXT_cocoa_window_binding");
    }

    XrSessionCreateInfo si = {XR_TYPE_SESSION_CREATE_INFO};
    si.next = &vkBinding; si.systemId = xr.systemId;
    XR_CHECK(xrCreateSession(xr.instance, &si, &xr.session));

    // Enumerate available rendering modes and store names
    if (xr.pfnEnumerateDisplayRenderingModesEXT && xr.session != XR_NULL_HANDLE) {
        uint32_t modeCount = 0;
        XrResult enumRes = xr.pfnEnumerateDisplayRenderingModesEXT(xr.session, 0, &modeCount, nullptr);
        if (XR_SUCCEEDED(enumRes) && modeCount > 0) {
            std::vector<XrDisplayRenderingModeInfoEXT> modes(modeCount);
            for (uint32_t i = 0; i < modeCount; i++) {
                modes[i].type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_EXT;
                modes[i].next = nullptr;
            }
            enumRes = xr.pfnEnumerateDisplayRenderingModesEXT(xr.session, modeCount, &modeCount, modes.data());
            if (XR_SUCCEEDED(enumRes)) {
                xr.renderingModeCount = modeCount > 8 ? 8 : modeCount;
                LOG_INFO("Display rendering modes (%u):", modeCount);
                for (uint32_t i = 0; i < xr.renderingModeCount; i++) {
                    strncpy(xr.renderingModeNames[i], modes[i].modeName, XR_MAX_SYSTEM_NAME_SIZE - 1);
                    xr.renderingModeNames[i][XR_MAX_SYSTEM_NAME_SIZE - 1] = '\0';
                    xr.renderingModeViewCounts[i] = modes[i].viewCount;
                    xr.renderingModeScaleX[i] = modes[i].viewScaleX;
                    xr.renderingModeScaleY[i] = modes[i].viewScaleY;
                    xr.renderingModeDisplay3D[i] = (modes[i].display3D == XR_TRUE);
                    LOG_INFO("  [%u] %s (views=%u, scale=%.2fx%.2f, 3D=%d)",
                        modes[i].modeIndex, modes[i].modeName, modes[i].viewCount,
                        modes[i].viewScaleX, modes[i].viewScaleY, modes[i].display3D);
                }
            }
        }
    }

    return true;
}

static bool CreateSpaces(AppXrSession& xr) {
    XrReferenceSpaceCreateInfo ci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    ci.poseInReferenceSpace = {{0,0,0,1},{0,0,0}};
    XR_CHECK(xrCreateReferenceSpace(xr.session, &ci, &xr.localSpace));

    return true;
}

static bool CreateSwapchains(AppXrSession& xr) {
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, 0, &viewCount, nullptr);
    std::vector<XrViewConfigurationView> views(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(xr.instance, xr.systemId, xr.viewConfigType, viewCount, &viewCount, views.data());

    uint32_t fmtCount = 0;
    xrEnumerateSwapchainFormats(xr.session, 0, &fmtCount, nullptr);
    std::vector<int64_t> fmts(fmtCount);
    xrEnumerateSwapchainFormats(xr.session, fmtCount, &fmtCount, fmts.data());

    int64_t selectedFmt = fmts.empty() ? VK_FORMAT_B8G8R8A8_UNORM : fmts[0];
    for (auto f : fmts) {
        if (f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB) { selectedFmt = f; break; }
        if (f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_R8G8B8A8_UNORM) selectedFmt = f;
    }

    uint32_t w = views[0].recommendedImageRectWidth * 2;
    uint32_t h = views[0].recommendedImageRectHeight;

    XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sci.format = selectedFmt;
    sci.sampleCount = 1;
    sci.width = w; sci.height = h;
    sci.faceCount = 1; sci.arraySize = 1; sci.mipCount = 1;

    XR_CHECK(xrCreateSwapchain(xr.session, &sci, &xr.swapchain.swapchain));
    xr.swapchain.width = w; xr.swapchain.height = h; xr.swapchain.format = selectedFmt;

    uint32_t imgCount = 0;
    xrEnumerateSwapchainImages(xr.swapchain.swapchain, 0, &imgCount, nullptr);
    xr.swapchain.imageCount = imgCount;

    LOG_INFO("Swapchain: %ux%u, %u images, format=%lld", w, h, imgCount, selectedFmt);
    return true;
}

static void PollEvents(AppXrSession& xr) {
    XrEventDataBuffer event = {};
    event.type = XR_TYPE_EVENT_DATA_BUFFER;
    while (xrPollEvent(xr.instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* ssc = (XrEventDataSessionStateChanged*)&event;
            xr.sessionState = ssc->state;
            if (ssc->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = xr.viewConfigType;
                xrBeginSession(xr.session, &bi);
                xr.sessionRunning = true;
            } else if (ssc->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(xr.session);
                xr.sessionRunning = false;
            } else if (ssc->state == XR_SESSION_STATE_EXITING) {
                xr.exitRequested = true;
            }
        }
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
}

static bool BeginFrame(AppXrSession& xr, XrFrameState& fs) {
    fs = {XR_TYPE_FRAME_STATE};
    XrResult r = xrWaitFrame(xr.session, nullptr, &fs);
    if (XR_FAILED(r)) return false;
    return XR_SUCCEEDED(xrBeginFrame(xr.session, nullptr));
}

static bool AcquireSwapchainImage(AppXrSession& xr, uint32_t& imageIndex) {
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(xr.swapchain.swapchain, &ai, &imageIndex))) return false;
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = 1000000000;
    return XR_SUCCEEDED(xrWaitSwapchainImage(xr.swapchain.swapchain, &wi));
}

static void ReleaseSwapchainImage(AppXrSession& xr) {
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(xr.swapchain.swapchain, &ri);
}

static void EndFrame(AppXrSession& xr, XrTime displayTime,
    XrCompositionLayerProjectionView* projViews, uint32_t viewCount) {
    XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = xr.localSpace;
    layer.viewCount = viewCount;
    layer.views = projViews;
    const XrCompositionLayerBaseHeader* layers[] = {(const XrCompositionLayerBaseHeader*)&layer};
    XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
    ei.displayTime = displayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = 1; ei.layers = layers;
    xrEndFrame(xr.session, &ei);
}

static void CleanupOpenXR(AppXrSession& xr) {
    if (xr.swapchain.swapchain) xrDestroySwapchain(xr.swapchain.swapchain);
    if (xr.localSpace) xrDestroySpace(xr.localSpace);
    if (xr.session) xrDestroySession(xr.session);
    if (xr.instance) xrDestroyInstance(xr.instance);
}

// ============================================================================
// Placeholder rendering (clear to dark gray when no scene loaded)
// ============================================================================

static void RenderPlaceholder(VkDevice dev, VkQueue queue, VkCommandPool pool,
                               VkImage image, uint32_t w, uint32_t h,
                               float yaw, float pitch) {
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(dev, &ai, &cmd);
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = 0; barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Tint color based on camera direction so drag-rotation gives visual feedback
    float ny = (yaw / 3.14159f) * 0.5f + 0.5f;   // 0..1 over ±π
    float np = (pitch / 1.5f) * 0.5f + 0.5f;       // 0..1 over ±1.5 rad
    VkClearColorValue cc = {{0.05f + ny * 0.15f, 0.08f + np * 0.12f, 0.15f, 1.0f}};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &range);

    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(dev, pool, 1, &cmd);
}

// ============================================================================
// Main
// ============================================================================

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    LOG_INFO("=== SR 3DGS OpenXR + External macOS Window ===");

    // Initialize rendering mode from env var (legacy fallback)
    {
        const char *mode_str = getenv("SIM_DISPLAY_OUTPUT");
        if (mode_str) {
            if (strcmp(mode_str, "anaglyph") == 0) g_input.currentRenderingMode = 1;
            else if (strcmp(mode_str, "sbs") == 0) g_input.currentRenderingMode = 2;
            else if (strcmp(mode_str, "blend") == 0) g_input.currentRenderingMode = 3;
            else g_input.currentRenderingMode = 1; // default to anaglyph
        }
    }

    // Step 1: Create macOS window
    g_windowW = 1280; g_windowH = 720;
    if (!CreateMacOSWindow(g_windowW, g_windowH)) {
        LOG_ERROR("Failed to create macOS window");
        return 1;
    }

    { NSSize cs = [[g_window contentView] bounds].size;
      CGFloat bs = [g_window backingScaleFactor];
      g_windowW = (uint32_t)(cs.width * bs);
      g_windowH = (uint32_t)(cs.height * bs);
      LOG_INFO("Window drawable: %ux%u", g_windowW, g_windowH); }

    // Step 2: Initialize OpenXR
    AppXrSession xr = {};
    if (!InitializeOpenXR(xr)) { LOG_ERROR("OpenXR init failed"); return 1; }

    // Try to find sim_display_set_output_mode
    { void *rtHandle = NULL;
      uint32_t ic = _dyld_image_count();
      for (uint32_t i = 0; i < ic; i++) {
          const char *name = _dyld_get_image_name(i);
          if (name && strstr(name, "openxr_displayxr")) {
              rtHandle = dlopen(name, RTLD_NOLOAD); break;
          }
      }
      if (rtHandle) g_pfnSetOutputMode = (PFN_sim_display_set_output_mode)dlsym(rtHandle, "sim_display_set_output_mode");
      LOG_INFO("sim_display hot-reload: %s", g_pfnSetOutputMode ? "available" : "not available"); }

    if (!GetVulkanGraphicsRequirements(xr)) { CleanupOpenXR(xr); return 1; }

    VkInstance vkInstance = VK_NULL_HANDLE;
    if (!CreateVulkanInstance(xr, vkInstance)) { CleanupOpenXR(xr); return 1; }

    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    if (!GetVulkanPhysicalDevice(xr, vkInstance, physDevice)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    std::vector<const char*> devExts;
    std::vector<std::string> extStorage;
    if (!GetVulkanDeviceExtensions(xr, devExts, extStorage, physDevice)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    uint32_t queueFamilyIndex = 0;
    if (!FindGraphicsQueueFamily(physDevice, queueFamilyIndex)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    VkDevice vkDevice = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    if (!CreateVulkanDevice(physDevice, queueFamilyIndex, devExts, vkDevice, graphicsQueue)) {
        vkDestroyInstance(vkInstance, nullptr); CleanupOpenXR(xr); return 1; }

    if (!CreateSession(xr, vkInstance, physDevice, vkDevice, queueFamilyIndex)) {
        vkDestroyDevice(vkDevice, nullptr); vkDestroyInstance(vkInstance, nullptr);
        CleanupOpenXR(xr); return 1; }

    if (!CreateSpaces(xr) || !CreateSwapchains(xr)) {
        CleanupOpenXR(xr); vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr); return 1; }

    // Enumerate swapchain images
    std::vector<XrSwapchainImageVulkanKHR> swapchainImages;
    { uint32_t count = xr.swapchain.imageCount;
      swapchainImages.resize(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
      xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
          (XrSwapchainImageBaseHeader*)swapchainImages.data()); }

    // Initialize 3DGS renderer
    { uint32_t rw = xr.swapchain.width;   // Full width — mono uses entire swapchain
      uint32_t rh = xr.swapchain.height;
      if (!g_gsRenderer.init(vkInstance, physDevice, vkDevice, graphicsQueue, queueFamilyIndex, rw, rh))
          LOG_WARN("3DGS renderer init failed");
    }

    // Command pool for placeholder rendering
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    { VkCommandPoolCreateInfo ci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
      ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      ci.queueFamilyIndex = queueFamilyIndex;
      vkCreateCommandPool(vkDevice, &ci, nullptr, &cmdPool); }

    g_input.stereo.virtualDisplayHeight = 0.24f;
    g_input.nominalViewerZ = xr.nominalViewerZ;
    g_input.renderingModeCount = xr.renderingModeCount;

    LOG_INFO("=== Entering main loop ===");
    LOG_INFO("Controls: L=Load Scene, WASD=Move, Drag=Look, Scroll=Scale");
    LOG_INFO("          V=Cycle Modes, Tab=HUD, 0-3=Select Mode, ESC=Quit");

    auto lastTime = std::chrono::high_resolution_clock::now();

    while (g_running && !xr.exitRequested) {
        PumpMacOSEvents();

        auto now = std::chrono::high_resolution_clock::now();
        float deltaTime = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;
        g_frameCount++;
        g_avgFrameTime = g_avgFrameTime * 0.95 + deltaTime * 0.05;

        // Handle load request (from L key or button click)
        if (g_input.loadRequested) {
            g_input.loadRequested = false;
            OpenLoadDialog();
        }

        UpdateCameraMovement(g_input, deltaTime, xr.displayHeightM);

        // Handle rendering mode change (V=cycle, 0-3=direct)
        if (g_input.renderingModeChangeRequested) {
            g_input.renderingModeChangeRequested = false;
            if (xr.pfnRequestDisplayRenderingModeEXT && xr.session != XR_NULL_HANDLE) {
                xr.pfnRequestDisplayRenderingModeEXT(xr.session, g_input.currentRenderingMode);
            }
        }

        // Handle eye tracking mode toggle
        if (g_input.eyeTrackingModeToggleRequested) {
            g_input.eyeTrackingModeToggleRequested = false;
            if (xr.pfnRequestEyeTrackingModeEXT && xr.session != XR_NULL_HANDLE) {
                XrEyeTrackingModeEXT newMode = (xr.activeEyeTrackingMode == XR_EYE_TRACKING_MODE_SMOOTH_EXT)
                    ? XR_EYE_TRACKING_MODE_RAW_EXT : XR_EYE_TRACKING_MODE_SMOOTH_EXT;
                xr.pfnRequestEyeTrackingModeEXT(xr.session, newMode);
            }
        }

        PollEvents(xr);

        if (xr.sessionRunning) {
            XrFrameState frameState;
            if (BeginFrame(xr, frameState)) {
                XrCompositionLayerProjectionView projectionViews[2] = {};
                bool rendered = false;

                if (frameState.shouldRender) {
                    XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
                    locateInfo.viewConfigurationType = xr.viewConfigType;
                    locateInfo.displayTime = frameState.predictedDisplayTime;
                    locateInfo.space = xr.localSpace;

                    XrViewState viewState = {XR_TYPE_VIEW_STATE};
                    XrViewEyeTrackingStateEXT eyeTrackingState = {};
                    eyeTrackingState.type = (XrStructureType)XR_TYPE_VIEW_EYE_TRACKING_STATE_EXT;
                    viewState.next = &eyeTrackingState;

                    uint32_t viewCount = 2;
                    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};

                    XrResult locResult = xrLocateViews(xr.session, &locateInfo, &viewState, 2, &viewCount, views);
                    if (XR_SUCCEEDED(locResult) &&
                        (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                        (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {

                        XrVector3f rawEyePos[2] = {views[0].pose.position, views[1].pose.position};
                        xr.leftEyeX = rawEyePos[0].x; xr.leftEyeY = rawEyePos[0].y; xr.leftEyeZ = rawEyePos[0].z;
                        xr.rightEyeX = rawEyePos[1].x; xr.rightEyeY = rawEyePos[1].y; xr.rightEyeZ = rawEyePos[1].z;
                        xr.isEyeTracking = (eyeTrackingState.isTracking == XR_TRUE);
                        xr.activeEyeTrackingMode = (uint32_t)eyeTrackingState.activeMode;

                        bool monoMode = (xr.renderingModeCount > 0 && !xr.renderingModeDisplay3D[g_input.currentRenderingMode]);
                        int eyeCount = monoMode ? 1 : 2;
                        if (monoMode) {
                            rawEyePos[0] = {
                                (rawEyePos[0].x + rawEyePos[1].x) / 2.0f,
                                (rawEyePos[0].y + rawEyePos[1].y) / 2.0f,
                                (rawEyePos[0].z + rawEyePos[1].z) / 2.0f};
                            rawEyePos[1] = rawEyePos[0];
                        }

                        XrPosef cameraPose;
                        quat_from_yaw_pitch(g_input.yaw, g_input.pitch, &cameraPose.orientation);
                        cameraPose.position = {g_input.cameraPosX, g_input.cameraPosY, g_input.cameraPosZ};

                        XrVector3f nominalViewer = {xr.nominalViewerX, xr.nominalViewerY, xr.nominalViewerZ};

                        float scaleX = (xr.renderingModeCount > 0 && g_input.currentRenderingMode < xr.renderingModeCount) ? xr.renderingModeScaleX[g_input.currentRenderingMode] : 0.5f;
                        float scaleY = (xr.renderingModeCount > 0 && g_input.currentRenderingMode < xr.renderingModeCount) ? xr.renderingModeScaleY[g_input.currentRenderingMode] : 1.0f;
                        uint32_t eyeRenderW = xr.swapchain.width / 2;
                        uint32_t eyeRenderH = xr.swapchain.height;
                        uint32_t renderW, renderH;
                        if (monoMode) {
                            renderW = g_windowW; renderH = g_windowH;
                            if (renderW > xr.swapchain.width) renderW = xr.swapchain.width;
                            if (renderH > xr.swapchain.height) renderH = xr.swapchain.height;
                        } else {
                            renderW = (uint32_t)(g_windowW * scaleX);
                            renderH = (uint32_t)(g_windowH * scaleY);
                            if (renderW > eyeRenderW) renderW = eyeRenderW;
                            if (renderH > eyeRenderH) renderH = eyeRenderH;
                        }
                        g_renderW = renderW; g_renderH = renderH;

                        // Stereo views (Kooima)
                        Display3DStereoView stereoViews[2];
                        bool hasKooima = (xr.displayWidthM > 0 && xr.displayHeightM > 0);
                        if (hasKooima) {
                            float dispPxW = xr.displayPixelWidth > 0 ? (float)xr.displayPixelWidth : (float)xr.swapchain.width;
                            float dispPxH = xr.displayPixelHeight > 0 ? (float)xr.displayPixelHeight : (float)xr.swapchain.height;
                            float pxSizeX = xr.displayWidthM / dispPxW;
                            float pxSizeY = xr.displayHeightM / dispPxH;
                            float winW_m = (float)g_windowW * pxSizeX;
                            float winH_m = (float)g_windowH * pxSizeY;
                            float minDisp = fminf(xr.displayWidthM, xr.displayHeightM);
                            float minWin = fminf(winW_m, winH_m);
                            float vs = minDisp / minWin;
                            float screenWidthM = winW_m * vs;
                            float screenHeightM = winH_m * vs;
                            // Kooima always uses full physical display dimensions.
                            // The display processor (weaver) handles cropping for SBS layout.
                            Display3DScreen screen = {screenWidthM, screenHeightM};
                            Display3DTunables tunables;
                            tunables.ipd_factor = g_input.stereo.ipdFactor;
                            tunables.parallax_factor = g_input.stereo.parallaxFactor;
                            tunables.perspective_factor = g_input.stereo.perspectiveFactor;
                            tunables.virtual_display_height = g_input.stereo.virtualDisplayHeight / g_input.stereo.scaleFactor;

                            display3d_compute_stereo_views(
                                &rawEyePos[0], &rawEyePos[1], &nominalViewer,
                                &screen, &tunables, &cameraPose,
                                0.01f, 100.0f, &stereoViews[0], &stereoViews[1]);
                        }

                        // Double-click teleport: unproject through left eye matrices
                        if (g_input.teleportRequested && hasKooima) {
                            g_input.teleportRequested = false;
                            NSSize viewSize = [[g_window contentView] bounds].size;

                            // Mouse to NDC. NSView Y=0 at bottom, but shader ndc2Pix maps
                            // ndcY=-1 to pixel 0 (top of image), so negate Y.
                            float ndcX = 2.0f * g_input.teleportMouseX / (float)viewSize.width - 1.0f;
                            float ndcY = -(2.0f * g_input.teleportMouseY / (float)viewSize.height - 1.0f);

                            // Unproject NDC through left eye projection (column-major)
                            const float *P = stereoViews[0].projection_matrix;
                            float vx = (ndcX + P[8]) / P[0];
                            float vy = (ndcY + P[9]) / P[5];
                            float vz = -1.0f;

                            // View-space direction to world-space via inverse view rotation (transpose of 3x3)
                            const float *V = stereoViews[0].view_matrix;
                            float wx = V[0]*vx + V[1]*vy + V[2]*vz;
                            float wy = V[4]*vx + V[5]*vy + V[6]*vz;
                            float wz = V[8]*vx + V[9]*vy + V[10]*vz;

                            // Normalize world ray direction
                            float len = sqrtf(wx*wx + wy*wy + wz*wz);
                            float rayDir[3] = {wx/len, wy/len, wz/len};

                            // Ray origin = left eye world position
                            float rayOrigin[3] = {
                                stereoViews[0].eye_world.x,
                                stereoViews[0].eye_world.y,
                                stereoViews[0].eye_world.z
                            };

                            float hitPos[3];
                            if (g_gsRenderer.pickGaussian(rayOrigin, rayDir, hitPos)) {
                                g_input.teleportAnimating = true;
                                g_input.teleportTargetX = hitPos[0];
                                g_input.teleportTargetY = hitPos[1];
                                g_input.teleportTargetZ = hitPos[2];
                                LOG_INFO("Teleporting to (%.3f, %.3f, %.3f)", hitPos[0], hitPos[1], hitPos[2]);
                            }
                        } else if (g_input.teleportRequested) {
                            g_input.teleportRequested = false; // consume without Kooima
                        }

                        rendered = true;
                        uint32_t imageIndex;
                        if (AcquireSwapchainImage(xr, imageIndex)) {
                            // Build per-eye view/proj matrices
                            float viewMat[2][16], projMat[2][16];
                            for (int eye = 0; eye < eyeCount; eye++) {
                                if (hasKooima) {
                                    memcpy(viewMat[eye], stereoViews[eye].view_matrix, sizeof(float) * 16);
                                    memcpy(projMat[eye], stereoViews[eye].projection_matrix, sizeof(float) * 16);
                                    views[eye].pose.position = stereoViews[eye].eye_world;
                                    views[eye].pose.orientation = cameraPose.orientation;
                                } else {
                                    mat4_view_from_xr_pose(viewMat[eye], views[eye].pose);
                                    mat4_from_xr_fov(projMat[eye], views[eye].fov, 0.01f, 100.0f);
                                }

                                uint32_t vpX = monoMode ? 0 : (eye * renderW);
                                projectionViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                                projectionViews[eye].subImage.swapchain = xr.swapchain.swapchain;
                                projectionViews[eye].subImage.imageRect.offset = {(int32_t)vpX, 0};
                                projectionViews[eye].subImage.imageRect.extent = {(int32_t)renderW, (int32_t)renderH};
                                projectionViews[eye].subImage.imageArrayIndex = 0;
                                projectionViews[eye].pose = views[eye].pose;
                                projectionViews[eye].fov = hasKooima ? stereoViews[eye].fov : views[eye].fov;
                            }

                            // Render 3DGS or placeholder
                            VkImage targetImage = swapchainImages[imageIndex].image;
                            VkFormat swapFormat = (VkFormat)xr.swapchain.format;

                            if (g_gsRenderer.hasScene()) {
                                for (int eye = 0; eye < eyeCount; eye++) {
                                    uint32_t vpX = monoMode ? 0 : (eye * renderW);
                                    g_gsRenderer.renderEye(
                                        targetImage, swapFormat,
                                        xr.swapchain.width, xr.swapchain.height,
                                        vpX, 0, renderW, renderH,
                                        viewMat[eye], projMat[eye]);
                                }
                            } else {
                                RenderPlaceholder(vkDevice, graphicsQueue, cmdPool,
                                    targetImage, xr.swapchain.width, xr.swapchain.height,
                                    g_input.yaw, g_input.pitch);
                            }

                            ReleaseSwapchainImage(xr);
                        } else {
                            rendered = false;
                        }
                    }
                }

                if (rendered) {
                    uint32_t submitCount = (xr.renderingModeCount > 0 && g_input.currentRenderingMode < xr.renderingModeCount) ? xr.renderingModeViewCounts[g_input.currentRenderingMode] : 2u;
                    EndFrame(xr, frameState.predictedDisplayTime, projectionViews, submitCount);
                } else {
                    XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
                    ei.displayTime = frameState.predictedDisplayTime;
                    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                    ei.layerCount = 0; ei.layers = nullptr;
                    xrEndFrame(xr.session, &ei);
                }
            }
        } else {
            usleep(100000);
        }

        // Update HUD
        g_hudUpdateTimer += deltaTime;
        if (g_hudUpdateTimer >= 0.5f) {
            g_hudUpdateTimer = 0.0f;
            @autoreleasepool {
                if (g_input.hudVisible && g_hudView != nil) {
                    double fps = (g_avgFrameTime > 0) ? 1.0 / g_avgFrameTime : 0;
                    NSString *sceneInfo = g_gsRenderer.hasScene()
                        ? [NSString stringWithFormat:@"Scene: %s", g_loadedFileName.c_str()]
                        : @"No scene loaded (press L)";

                    NSString *text = [NSString stringWithFormat:
                        @"%s\nSession: %d\n"
                        "Mode: %s  Output: %d\n"
                        "%@\n"
                        "FPS: %.0f (%.1f ms)\n"
                        "Render: %ux%u  Window: %ux%u\n"
                        "Display: %.3f x %.3f m\n"
                        "Eye L: (%.3f, %.3f, %.3f)\n"
                        "Eye R: (%.3f, %.3f, %.3f)\n"
                        "Camera: (%.2f, %.2f, %.2f)\n"
                        "Fwd: (%.3f, %.3f, %.3f)\n"
                        "IPD: %.2f  Parallax: %.2f  Scale: %.2f\n"
                        "\nL=Load  WASD=Move  DblClick=Teleport\n"
                        "Scroll=Scale  Shift+Scroll=IPD+Parallax\n"
                        "V=Cycle  Tab=HUD  Space=Reset  ESC=Quit",
                        xr.systemName, (int)xr.sessionState,
                        (xr.renderingModeCount > 0 ? (xr.renderingModeDisplay3D[g_input.currentRenderingMode] ? "3D" : "2D") : "3D"), g_input.currentRenderingMode,
                        sceneInfo,
                        fps, g_avgFrameTime * 1000.0,
                        g_renderW, g_renderH, g_windowW, g_windowH,
                        xr.displayWidthM, xr.displayHeightM,
                        xr.leftEyeX, xr.leftEyeY, xr.leftEyeZ,
                        xr.rightEyeX, xr.rightEyeY, xr.rightEyeZ,
                        g_input.cameraPosX, g_input.cameraPosY, g_input.cameraPosZ,
                        cosf(g_input.pitch) * sinf(g_input.yaw),
                        -sinf(g_input.pitch),
                        -cosf(g_input.pitch) * cosf(g_input.yaw),
                        g_input.stereo.ipdFactor, g_input.stereo.parallaxFactor,
                        g_input.stereo.scaleFactor];
                    g_hudView.hudText = text;
                    // Auto-size HUD to fit text
                    NSDictionary *attrs = @{
                        NSFontAttributeName: [NSFont monospacedSystemFontOfSize:10 weight:NSFontWeightRegular]
                    };
                    NSRect textBounds = [text boundingRectWithSize:NSMakeSize(400, CGFLOAT_MAX)
                                         options:NSStringDrawingUsesLineFragmentOrigin
                                         attributes:attrs];
                    CGFloat pad = 16.0;
                    NSRect hudFrame = NSMakeRect(8, 8,
                        ceilf(textBounds.size.width + pad),
                        ceilf(textBounds.size.height + pad));
                    [g_hudView setFrame:hudFrame];
                    [g_hudView setNeedsDisplay:YES];
                    [g_hudView setHidden:NO];
                } else if (g_hudView != nil) {
                    [g_hudView setHidden:YES];
                }
            }
        }
    }

    // Clean exit
    if (!xr.exitRequested && xr.session != XR_NULL_HANDLE && xr.sessionRunning) {
        xrRequestExitSession(xr.session);
        for (int i = 0; i < 100 && !xr.exitRequested; i++) {
            PollEvents(xr); usleep(10000);
        }
    }

    LOG_INFO("=== Shutting down ===");
    g_gsRenderer.cleanup();
    if (cmdPool != VK_NULL_HANDLE) vkDestroyCommandPool(vkDevice, cmdPool, nullptr);
    CleanupOpenXR(xr);
    // MoltenVK may throw std::system_error ("mutex lock failed") during device/instance
    // destruction due to internal threading cleanup.  Catch and ignore since we're exiting.
    try {
        vkDestroyDevice(vkDevice, nullptr);
        vkDestroyInstance(vkInstance, nullptr);
    } catch (const std::exception& e) {
        LOG_WARN("Vulkan cleanup exception (ignored): %s", e.what());
    }
    LOG_INFO("Application shutdown complete");
    return 0;
}
