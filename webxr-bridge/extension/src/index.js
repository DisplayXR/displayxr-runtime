// Monado WebXR Bridge — Entry point
// Uses Meta's IWER (Immersive Web Emulation Runtime) to provide a complete
// WebXR implementation. Overrides Chrome's native navigator.xr (which doesn't
// support immersive-vr on macOS). Camera controls match EXT test apps:
// mouse drag = look, WASD = move, Q/E = up/down, scroll = zoom, space = reset.

import { XRDevice } from 'iwer';
import { metaQuestTouchPlus } from 'iwer/lib/device/configs/controller/meta.js';

// --- Device Configuration ---

const MONADO_CONFIG = {
  name: 'Monado 3D Display',
  controllerConfig: metaQuestTouchPlus,
  supportedSessionModes: ['inline', 'immersive-vr'],
  supportedFeatures: ['viewer', 'local', 'local-floor'],
  supportedFrameRates: [60],
  isSystemKeyboardSupported: false,
  internalNominalFrameRate: 60,
  environmentBlendModes: {
    'immersive-vr': 'opaque',
  },
  interactionMode: 'world-space',
  userAgent: navigator.userAgent,
};

const EYE_HEIGHT = 1.6;   // meters

// --- Setup ---

window.addEventListener('unhandledrejection', (e) => {
  console.error('MonadoXR: UNHANDLED REJECTION:', e.reason);
});

// Create device with stereo enabled
const xrDevice = new XRDevice(MONADO_CONFIG, {
  ipd: 0.063,
  fovy: Math.PI / 3,    // 60 degree vertical FOV
  stereoEnabled: true,
});

xrDevice.position.set(0, EYE_HEIGHT, 0);

// Install the WebXR runtime (sets navigator.xr, all WebXR globals)
xrDevice.installRuntime();

// Signal to other polyfills that a custom polyfill is active
window.CustomWebXRPolyfill = true;

// --- Protect against page polyfill interference ---
// Sites like xrdinosaurs.com bundle their own webxr-polyfill which tries to
// wrap navigator.xr methods via _injectCompatibilityShims. We protect our
// methods with getter/setter pairs that silently absorb writes.

const xr = navigator.xr;
const methodsToProtect = ['requestSession', 'isSessionSupported', 'supportsSession'];

for (const method of methodsToProtect) {
  if (typeof xr[method] === 'function') {
    const bound = xr[method].bind(xr);
    Object.defineProperty(xr, method, {
      get() { return bound; },
      set() {},
      configurable: true,
    });
  }
}

// Protect XRWebGLLayer global
const iwerXRWebGLLayer = window.XRWebGLLayer;
Object.defineProperty(window, 'XRWebGLLayer', {
  get() { return iwerXRWebGLLayer; },
  set() {},
  configurable: true,
  enumerable: true,
});

// --- VR Session Lifecycle ---
// Track whether an immersive VR session is active so IO interception
// (keyboard, mouse, scroll) only fires during a session.
let vrSessionActive = false;

const origRequestSession = navigator.xr.requestSession.bind(navigator.xr);
const wrappedRequestSession = async function(mode, ...rest) {
  const session = await origRequestSession(mode, ...rest);
  if (mode === 'immersive-vr') {
    vrSessionActive = true;
    console.log('MonadoXR: VR session started — IO controls enabled');
    session.addEventListener('end', () => {
      vrSessionActive = false;
      mouseDragging = false;
      console.log('MonadoXR: VR session ended — IO controls disabled');
    });
  }
  return session;
};

// Re-protect requestSession after our override (same pattern as above)
Object.defineProperty(navigator.xr, 'requestSession', {
  get() { return wrappedRequestSession; },
  set() {},
  configurable: true,
});

// --- Camera & Controller Controls ---
// Matches EXT app (test_apps/sim_cube_openxr_ext_macos) + qwerty_macos.m pattern.
// Default: WASD = move camera, LMB drag = look, Q/E = up/down, Scroll = zoom
// CTRL held: focus LEFT controller — mouse = move, LMB = trigger, MMB = squeeze
// ALT held: focus RIGHT controller — same
// CTRL+ALT: focus both controllers

const keys = {};
let lastTime = 0;

// Camera state (ported from InputState in EXT apps)
let camYaw = 0;
let camPitch = 0;
let camPosX = 0, camPosY = 0, camPosZ = 0;
let zoomScale = 1.0;

// Mouse drag state
let mouseDragging = false;

// Controller state — offsets from head position in head-local space.
// Updated by CTRL/ALT + mouse/WASD. Controllers follow the head each frame.
let ctrlPressed = false;
let altPressed = false;
const ctrlOffsetLeft  = { x: -0.2, y: -0.15, z: -0.3 };
const ctrlOffsetRight = { x:  0.2, y: -0.15, z: -0.3 };
const CTRL_OFFSET_DEFAULTS = {
  left:  { x: -0.2, y: -0.15, z: -0.3 },
  right: { x:  0.2, y: -0.15, z: -0.3 },
};

// Quaternion from yaw/pitch (matches EXT app quat_from_yaw_pitch)
function quatFromYawPitch(yaw, pitch) {
  const cy = Math.cos(yaw / 2), sy = Math.sin(yaw / 2);
  const cp = Math.cos(pitch / 2), sp = Math.sin(pitch / 2);
  return { x: cy * sp, y: sy * cp, z: -sy * sp, w: cy * cp };
}

// Hamilton product: a * b (matches EXT app quat_multiply)
function quatMultiply(a, b) {
  return {
    x: a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
    y: a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
    z: a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
    w: a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
  };
}

// Rotate vec3 by quaternion: v' = q * v * q^-1
function quatRotateVec3(q, vx, vy, vz) {
  const tx = 2 * (q.y * vz - q.z * vy);
  const ty = 2 * (q.z * vx - q.x * vz);
  const tz = 2 * (q.x * vy - q.y * vx);
  return {
    x: vx + q.w * tx + (q.y * tz - q.z * ty),
    y: vy + q.w * ty + (q.z * tx - q.x * tz),
    z: vz + q.w * tz + (q.x * ty - q.y * tx),
  };
}

// iwer device stays at fixed neutral position (set once at init, line 42).
// All view transforms are computed in getViewerPose from:
//   1. rawEyePositions (from xrLocateViews via bridge host)
//   2. virtual display pose (camPos + camYaw/camPitch from WASD/mouse)
// iwer is just the WebXR API shim — we bypass its eye positioning entirely.
// IPD will be controlled by modifying rawEyePositions directly (not iwer's IPD setter).

window.addEventListener('keydown', (e) => {
  if (!vrSessionActive) return;
  keys[e.code] = true;
  if (e.code === 'Space') e.preventDefault();
  if (e.key === 'Control') ctrlPressed = true;
  if (e.key === 'Alt') { altPressed = true; e.preventDefault(); }
});
window.addEventListener('keyup', (e) => {
  if (!vrSessionActive) return;
  keys[e.code] = false;
  if (e.key === 'Control') ctrlPressed = false;
  if (e.key === 'Alt') altPressed = false;
});

// Mouse events — routes to controllers when CTRL/ALT held, camera otherwise.
// Matches qwerty_macos.m: modifier+mousemove = controller XY translation,
// LMB = trigger, MMB = squeeze (when controller focused).
window.addEventListener('mousedown', (e) => {
  if (!vrSessionActive) return;
  if (ctrlPressed || altPressed) {
    const ctrls = xrDevice.controllers;
    if (e.button === 0) { // LMB = trigger
      if (ctrlPressed && ctrls.left)  ctrls.left.updateButtonValue('trigger', 1);
      if (altPressed  && ctrls.right) ctrls.right.updateButtonValue('trigger', 1);
    } else if (e.button === 1) { // MMB = squeeze
      if (ctrlPressed && ctrls.left)  ctrls.left.updateButtonValue('squeeze', 1);
      if (altPressed  && ctrls.right) ctrls.right.updateButtonValue('squeeze', 1);
    }
    return;
  }
  if (e.button === 0) mouseDragging = true;
});
window.addEventListener('mouseup', (e) => {
  if (!vrSessionActive) return;
  if (ctrlPressed || altPressed) {
    const ctrls = xrDevice.controllers;
    if (e.button === 0) {
      if (ctrlPressed && ctrls.left)  ctrls.left.updateButtonValue('trigger', 0);
      if (altPressed  && ctrls.right) ctrls.right.updateButtonValue('trigger', 0);
    } else if (e.button === 1) {
      if (ctrlPressed && ctrls.left)  ctrls.left.updateButtonValue('squeeze', 0);
      if (altPressed  && ctrls.right) ctrls.right.updateButtonValue('squeeze', 0);
    }
    return;
  }
  if (e.button === 0) mouseDragging = false;
});
window.addEventListener('mousemove', (e) => {
  if (!vrSessionActive) return;
  if (ctrlPressed || altPressed) {
    // Controller focused: mouse move = XY translation in head-local space
    if (wsConnected && !browserDisplay) return;
    const dx = e.movementX * 0.001;
    const dy = -e.movementY * 0.001; // Screen Y is inverted
    if (ctrlPressed) { ctrlOffsetLeft.x  += dx; ctrlOffsetLeft.y  += dy; }
    if (altPressed)  { ctrlOffsetRight.x += dx; ctrlOffsetRight.y += dy; }
    return;
  }
  if (!mouseDragging) return;
  if (wsConnected && !browserDisplay) return; // Monado handles input
  camYaw -= e.movementX * 0.005;
  camPitch -= e.movementY * 0.005;
  if (camPitch > 1.4) camPitch = 1.4;
  if (camPitch < -1.4) camPitch = -1.4;
});

// Scroll wheel for display-centric zoom (matches EXT app)
// zoomScale divides both eye positions and screen dimensions in Kooima math.
// Currently cancels (same ratio), but kept explicit for upcoming
// baseline/parallax/perspective modifiers.
window.addEventListener('wheel', (e) => {
  if (!vrSessionActive) return; // Don't intercept scroll outside VR sessions
  if (wsConnected && !browserDisplay) return; // Monado handles input
  e.preventDefault();
  const factor = e.deltaY < 0 ? 1.1 : (1.0 / 1.1);
  zoomScale *= factor;
  if (zoomScale < 0.1) zoomScale = 0.1;
  if (zoomScale > 10.0) zoomScale = 10.0;
}, { passive: false });

function updateCamera(time) {
  requestAnimationFrame(updateCamera);
  if (!vrSessionActive) return;
  if (wsConnected && !browserDisplay) return;

  const now = time / 1000;
  const dt = lastTime > 0 ? Math.min(now - lastTime, 0.1) : 1 / 60;
  lastTime = now;

  // Space = reset view + controller offsets
  if (keys['Space']) {
    camPosX = camPosY = camPosZ = 0;
    camYaw = camPitch = 0;
    zoomScale = 1.0;
    Object.assign(ctrlOffsetLeft, CTRL_OFFSET_DEFAULTS.left);
    Object.assign(ctrlOffsetRight, CTRL_OFFSET_DEFAULTS.right);
    keys['Space'] = false;
  }

  // WASD/QE movement
  const hasMovement = keys['KeyW'] || keys['KeyS'] || keys['KeyA'] ||
    keys['KeyD'] || keys['KeyE'] || keys['KeyQ'];

  if (hasMovement) {
    const d = 0.1 * dt * 4 / zoomScale; // m/frame-tick, scaled by 4/zoomScale

    if (ctrlPressed || altPressed) {
      // Controller focused: move controller offset in head-local space
      const targets = [];
      if (ctrlPressed) targets.push(ctrlOffsetLeft);
      if (altPressed)  targets.push(ctrlOffsetRight);
      for (const off of targets) {
        if (keys['KeyW']) off.z -= d; // Forward = -Z
        if (keys['KeyS']) off.z += d;
        if (keys['KeyA']) off.x -= d;
        if (keys['KeyD']) off.x += d;
        if (keys['KeyE']) off.y += d;
        if (keys['KeyQ']) off.y -= d;
      }
    } else {
      // Camera movement (existing)
      const q = quatFromYawPitch(camYaw, camPitch);
      const fwd = quatRotateVec3(q, 0, 0, -1);
      const rt = quatRotateVec3(q, 1, 0, 0);
      const up = quatRotateVec3(q, 0, 1, 0);
      if (keys['KeyW']) { camPosX += fwd.x*d; camPosY += fwd.y*d; camPosZ += fwd.z*d; }
      if (keys['KeyS']) { camPosX -= fwd.x*d; camPosY -= fwd.y*d; camPosZ -= fwd.z*d; }
      if (keys['KeyD']) { camPosX += rt.x*d; camPosY += rt.y*d; camPosZ += rt.z*d; }
      if (keys['KeyA']) { camPosX -= rt.x*d; camPosY -= rt.y*d; camPosZ -= rt.z*d; }
      if (keys['KeyE']) { camPosX += up.x*d; camPosY += up.y*d; camPosZ += up.z*d; }
      if (keys['KeyQ']) { camPosX -= up.x*d; camPosY -= up.y*d; camPosZ -= up.z*d; }
    }
  }

  // Update controller positions using same transform as getViewerPose:
  //   displayPos = nominalViewPos + offset  (controller pos in display space)
  //   worldPos = rotate(displayPos / zoomScale, playerOri) + cameraPos
  const headOri = quatFromYawPitch(camYaw, camPitch);
  const zs = zoomScale;
  const [nvX, nvY, nvZ] = nominalViewPos;
  const ctrls = xrDevice.controllers;
  for (const [hand, offset] of [['left', ctrlOffsetLeft], ['right', ctrlOffsetRight]]) {
    const ctrl = ctrls[hand];
    if (!ctrl) continue;
    // Display-space position: nominal viewer + head-local offset
    const dx = (nvX + offset.x) / zs;
    const dy = (nvY + offset.y) / zs;
    const dz = (nvZ + offset.z) / zs;
    // Rotate into world space + camera translation (matches eye transform)
    const r = quatRotateVec3(headOri, dx, dy, dz);
    ctrl.position.set(
      r.x + camPosX,
      r.y + EYE_HEIGHT + camPosY,
      r.z + camPosZ
    );
    ctrl.quaternion.set(headOri.x, headOri.y, headOri.z, headOri.w);
  }
}

requestAnimationFrame(updateCamera);

// --- Canvas Resize Handling ---
// Send viewport dimensions to bridge host when canvas size changes,
// so it can adjust swapchain/FOV like EXT apps do on window resize.

let lastCanvasWidth = 0, lastCanvasHeight = 0;

function checkCanvasResize() {
  requestAnimationFrame(checkCanvasResize);
  if (!wsConnected) return;

  // Find the active WebGL canvas
  const canvas = document.querySelector('canvas');
  if (!canvas) return;
  const w = canvas.width, h = canvas.height;
  if (w === lastCanvasWidth && h === lastCanvasHeight) return;
  lastCanvasWidth = w;
  lastCanvasHeight = h;

  window.postMessage({ type: 'monado-ws-out', json: JSON.stringify({ resize: { w, h } }) }, '*');
  console.log(`MonadoXR: Canvas resized to ${w}x${h}, notified bridge host`);
}

requestAnimationFrame(checkCanvasResize);

// --- Display Mode State ---
// 'sbs' = side-by-side (no post-process), 'anaglyph' = red-cyan, 'blend' = 50% mix
let displayMode = 'sbs';
// When true, browser handles display — skip frame capture to Monado
let browserDisplay = false;

// --- Post-Process Shader Pipeline ---
// Lazy-initialized WebGL2 resources for SBS→anaglyph/blend display processing.
// Reusable across Option D (copyTexImage2D), Option A (texImage2D from readback),
// and future browser-native (importExternalTexture).

let ppProgram = null;
let ppTexture = null;
let ppVAO = null;
let ppInitialized = false;

const PP_VERT_SRC = `#version 300 es
in vec2 aPos;
out vec2 vUV;
void main() {
    vUV = aPos * 0.5 + 0.5;
    gl_Position = vec4(aPos, 0.0, 1.0);
}`;

const PP_FRAG_SRC = `#version 300 es
precision mediump float;
uniform sampler2D uSBS;
uniform int uMode; // 1=anaglyph, 2=blend
in vec2 vUV;
out vec4 fragColor;
void main() {
    vec4 left = texture(uSBS, vec2(vUV.x * 0.5, vUV.y));
    vec4 right = texture(uSBS, vec2(vUV.x * 0.5 + 0.5, vUV.y));
    if (uMode == 1) {
        fragColor = vec4(left.r, right.g, right.b, 1.0);
    } else {
        fragColor = mix(left, right, 0.5);
    }
}`;

function initPostProcess(gl) {
    // Compile shaders
    const vs = gl.createShader(gl.VERTEX_SHADER);
    gl.shaderSource(vs, PP_VERT_SRC);
    gl.compileShader(vs);
    if (!gl.getShaderParameter(vs, gl.COMPILE_STATUS)) {
        console.error('MonadoXR: PP vertex shader error:', gl.getShaderInfoLog(vs));
        return false;
    }

    const fs = gl.createShader(gl.FRAGMENT_SHADER);
    gl.shaderSource(fs, PP_FRAG_SRC);
    gl.compileShader(fs);
    if (!gl.getShaderParameter(fs, gl.COMPILE_STATUS)) {
        console.error('MonadoXR: PP fragment shader error:', gl.getShaderInfoLog(fs));
        return false;
    }

    ppProgram = gl.createProgram();
    gl.attachShader(ppProgram, vs);
    gl.attachShader(ppProgram, fs);
    gl.bindAttribLocation(ppProgram, 0, 'aPos');
    gl.linkProgram(ppProgram);
    if (!gl.getProgramParameter(ppProgram, gl.LINK_STATUS)) {
        console.error('MonadoXR: PP program link error:', gl.getProgramInfoLog(ppProgram));
        return false;
    }
    gl.deleteShader(vs);
    gl.deleteShader(fs);

    // Fullscreen quad VAO (triangle strip: [-1,-1], [1,-1], [-1,1], [1,1])
    ppVAO = gl.createVertexArray();
    gl.bindVertexArray(ppVAO);
    const quadBuf = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, quadBuf);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1,-1, 1,-1, -1,1, 1,1]), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    gl.bindVertexArray(null);

    // Texture for capturing current canvas content
    ppTexture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, ppTexture);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.bindTexture(gl.TEXTURE_2D, null);

    ppInitialized = true;
    console.log('MonadoXR: Post-process shader pipeline initialized');
    return true;
}

function applyDisplayProcessing(gl, width, height) {
    if (!ppInitialized && !initPostProcess(gl)) return;

    const modeInt = displayMode === 'anaglyph' ? 1 : 2; // blend

    // Save WebGL state
    const prevProgram = gl.getParameter(gl.CURRENT_PROGRAM);
    const prevFB = gl.getParameter(gl.FRAMEBUFFER_BINDING);
    const prevVAO = gl.getParameter(gl.VERTEX_ARRAY_BINDING);
    const prevActiveTexture = gl.getParameter(gl.ACTIVE_TEXTURE);
    const prevTex2D = gl.getParameter(gl.TEXTURE_BINDING_2D);
    const prevViewport = gl.getParameter(gl.VIEWPORT);

    // Capture current canvas content into texture
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, ppTexture);
    gl.copyTexImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 0, 0, width, height, 0);

    // Disable depth/stencil/blend so fullscreen quad overwrites all pixels
    const prevDepthTest = gl.isEnabled(gl.DEPTH_TEST);
    const prevStencilTest = gl.isEnabled(gl.STENCIL_TEST);
    const prevBlend = gl.isEnabled(gl.BLEND);
    gl.disable(gl.DEPTH_TEST);
    gl.disable(gl.STENCIL_TEST);
    gl.disable(gl.BLEND);

    // Draw fullscreen quad with post-process shader
    gl.useProgram(ppProgram);
    gl.uniform1i(gl.getUniformLocation(ppProgram, 'uSBS'), 0);
    gl.uniform1i(gl.getUniformLocation(ppProgram, 'uMode'), modeInt);
    gl.viewport(0, 0, width, height);
    gl.bindVertexArray(ppVAO);
    gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);

    // Restore WebGL state
    if (prevDepthTest) gl.enable(gl.DEPTH_TEST);
    if (prevStencilTest) gl.enable(gl.STENCIL_TEST);
    if (prevBlend) gl.enable(gl.BLEND);
    gl.useProgram(prevProgram);
    gl.bindFramebuffer(gl.FRAMEBUFFER, prevFB);
    gl.bindVertexArray(prevVAO);
    gl.activeTexture(prevActiveTexture);
    gl.bindTexture(gl.TEXTURE_2D, prevTex2D);
    gl.viewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
}

// --- WebSocket Frame Capture ---
// Sends stereo SBS frames to the native OpenXR bridge host via WebSocket.
// Protocol: [uint32 width][uint32 height][RGBA pixels]

let wsConnected = false;
let captureCanvas = null;
let captureCtx = null;
// Target capture dimensions — set by native host config message, or canvas native size
let captureWidth = 0;
let captureHeight = 0;
// Per-eye Kooima FOV from OpenXR runtime — [[angleL,angleR,angleU,angleD], [same for right eye]]
let fovAngles = null;
// Raw per-eye positions from xrLocateViews — [[x,y,z], [x,y,z]]
// These are in display-local space (relative to display center).
let rawEyePositions = null;
// Nominal viewer position in display space (from bridge host config)
let nominalViewPos = [0, 0.1, 0.65];
// Physical display dimensions from XR_EXT_display_info (for Kooima projection)
let displayWidthM = 0;
let displayHeightM = 0;
let displayPixelW = 0;
let displayPixelH = 0;
let viewScaleX = 1.0;
let viewScaleY = 1.0;
let loggedBridgeInit = false;

// --- WebSocket relay via isolated-world content script ---
// The content script (ISOLATED world) owns the WebSocket and is exempt from
// page CSP. We communicate via window.postMessage.

function handleWsMessage(jsonStr) {
  try {
    const msg = JSON.parse(jsonStr);
    // Config message: swapchain dimensions + display mode
    if (msg.w && msg.h) {
      captureWidth = msg.w;
      captureHeight = msg.h;
      captureCanvas = null;
      captureCtx = null;
      console.log(`MonadoXR: Native host requested capture at ${msg.w}x${msg.h}`);
    }
    if (msg.displayMode) {
      displayMode = msg.displayMode;
      console.log(`MonadoXR: Display mode set to ${displayMode}`);
    }
    if (msg.browserDisplay !== undefined) {
      browserDisplay = msg.browserDisplay;
      console.log(`MonadoXR: Browser display mode ${browserDisplay ? 'enabled' : 'disabled'}`);
    }
    // Physical display dimensions for Kooima projection
    if (msg.displayWidthM) {
      displayWidthM = msg.displayWidthM;
      displayHeightM = msg.displayHeightM;
      displayPixelW = msg.displayPixelW || 0;
      displayPixelH = msg.displayPixelH || 0;
      viewScaleX = msg.viewScaleX || 1.0;
      viewScaleY = msg.viewScaleY || 1.0;
      if (msg.nominalViewer) {
        nominalViewPos = msg.nominalViewer;
        console.log(`MonadoXR: Nominal viewer position: (${msg.nominalViewer.map(v => v.toFixed(3)).join(', ')})`);
      }
    }
    // Pose message: [px, py, pz, qx, qy, qz, qw]
    // Skip when browser owns display — WASD controls pose locally
    if (msg.pose && !browserDisplay) {
      const [px, py, pz, qx, qy, qz, qw] = msg.pose;
      xrDevice.position.set(px, py, pz);
      xrDevice.quaternion.set(qx, qy, qz, qw);
    }
    // Per-eye positions from xrLocateViews (display-space): [[x,y,z],[x,y,z]]
    if (msg.eyes) {
      rawEyePositions = msg.eyes;
    }
    // Per-eye FOV from Kooima projection: [[angleL,angleR,angleU,angleD],[...]]
    if (msg.fov) {
      fovAngles = msg.fov;
    }
    // Controller state: [{pose, tr, sq, mn, ts, tc}, {same}] for [left, right]
    // Skip when browser owns display — IWER handles controllers locally
    if (msg.ctrl && !browserDisplay) {
      const controllers = xrDevice.controllers;
      const hands = ['left', 'right'];
      for (let i = 0; i < 2; i++) {
        const c = msg.ctrl[i];
        const controller = controllers[hands[i]];
        if (!c || !controller) continue;

        // Grip pose
        if (c.pose) {
          controller.position.set(c.pose[0], c.pose[1], c.pose[2]);
          controller.quaternion.set(c.pose[3], c.pose[4], c.pose[5], c.pose[6]);
        }
        // Buttons
        controller.updateButtonValue('trigger', c.tr);
        controller.updateButtonValue('squeeze', c.sq ? 1 : 0);
        controller.updateButtonValue(i === 0 ? 'x-button' : 'a-button', c.mn ? 1 : 0);
        controller.updateButtonValue('thumbstick', c.tc ? 1 : 0);
        // Thumbstick axes
        controller.updateAxes('thumbstick', c.ts[0], c.ts[1]);
      }
    }
  } catch (e) { /* ignore non-JSON */ }
}

window.addEventListener('message', (evt) => {
  if (evt.source !== window) return;
  if (!evt.data) return;
  if (evt.data.type === 'monado-ws-in') {
    handleWsMessage(evt.data.json);
  } else if (evt.data.type === 'monado-ws-status') {
    wsConnected = evt.data.connected;
    console.log(`MonadoXR: WebSocket ${wsConnected ? 'connected' : 'disconnected'}`);
  }
});

// Build a column-major 4x4 perspective matrix from OpenXR FOV angles (radians)
function fovToProjectionMatrix(angleLeft, angleRight, angleUp, angleDown, near, far) {
  const l = near * Math.tan(angleLeft);
  const r = near * Math.tan(angleRight);
  const t = near * Math.tan(angleUp);
  const b = near * Math.tan(angleDown);
  const m = new Float32Array(16);
  m[0]  = 2 * near / (r - l);
  m[5]  = 2 * near / (t - b);
  m[8]  = (r + l) / (r - l);
  m[9]  = (t + b) / (t - b);
  m[10] = -(far + near) / (far - near);
  m[11] = -1;
  m[14] = -2 * far * near / (far - near);
  return m;
}

// Kooima off-axis perspective projection (matches ext app mat4_kooima_projection).
// eyePos: display-relative eye position [x,y,z] from DISPLAY-space xrLocateViews.
// screenW, screenH: virtual display rect in meters (after viewport scaling).
function kooimaProjectionMatrix(eyePos, screenW, screenH, near, far) {
  let ez = eyePos[2];
  if (ez <= 0.001) ez = 0.65;
  const halfW = screenW / 2;
  const halfH = screenH / 2;
  const ex = eyePos[0], ey = eyePos[1];

  // Asymmetric frustum bounds from eye offset relative to screen center
  const left   = near * (-halfW - ex) / ez;
  const right  = near * ( halfW - ex) / ez;
  const bottom = near * (-halfH - ey) / ez;
  const top    = near * ( halfH - ey) / ez;
  const w = right - left, h = top - bottom;

  const m = new Float32Array(16);
  m[0]  = 2 * near / w;
  m[5]  = 2 * near / h;
  m[8]  = (right + left) / w;
  m[9]  = (top + bottom) / h;
  m[10] = -(far + near) / (far - near);
  m[11] = -1;
  m[14] = -2 * far * near / (far - near);
  return m;
}

// Compute Kooima screen rect from display dimensions and canvas size.
// Matches ext app viewport-scaling logic: convert window pixels to meters,
// then apply isotropic scale so FOV stays consistent across window sizes.
function computeKooimaScreenRect(canvasW, canvasH) {
  const dispPxW = displayPixelW > 0 ? displayPixelW : captureWidth;
  const dispPxH = displayPixelH > 0 ? displayPixelH : captureHeight;
  const pxSizeX = displayWidthM / dispPxW;
  const pxSizeY = displayHeightM / dispPxH;
  const winW_m = canvasW * pxSizeX;
  const winH_m = canvasH * pxSizeY;
  const minDisp = Math.min(displayWidthM, displayHeightM);
  const minWin  = Math.min(winW_m, winH_m);
  const vs = minDisp / minWin;
  return { screenW: winW_m * vs, screenH: winH_m * vs };
}

// Override view poses and projection matrices.
// Display-centric transform (matches EXT apps exactly):
//   localEyePos = raw eye position from xrLocateViews (display-space)
//   worldPos = rotate(localEyePos / zoomScale, playerOri) + cameraPos
//   worldOri = playerOri * localOri
// Rotation pivots around the display center (origin), not the eyes.
// zoomScale divides both eye positions and screen dims in Kooima (cancels
// for now, kept explicit for upcoming baseline/parallax/perspective modifiers).
const origGetViewerPose = XRFrame.prototype.getViewerPose;
XRFrame.prototype.getViewerPose = function(refSpace) {
  const pose = origGetViewerPose.call(this, refSpace);
  if (!pose) return pose;

  // Apply display-centric transform when browser handles input
  if (browserDisplay || !wsConnected) {
    const playerOri = quatFromYawPitch(camYaw, camPitch);
    const zs = zoomScale;

    for (let i = 0; i < pose.views.length; i++) {
      const view = pose.views[i];
      const t = view.transform;

      // Display-space eye position from xrLocateViews (via bridge host).
      // Includes viewer Z distance from display, IPD offsets, etc.
      // Future: IPD will be controlled by modifying rawEyePositions directly.
      let localX, localY, localZ;
      if (rawEyePositions && i < rawEyePositions.length) {
        [localX, localY, localZ] = rawEyePositions[i];
      } else {
        // Standalone fallback (no bridge host): use iwer offsets + default 0.65m viewer distance
        const headPos = xrDevice.position;
        localX = t.position.x - headPos.x;
        localY = t.position.y - headPos.y;
        localZ = (t.position.z - headPos.z) + 0.65;
      }

      // Scale by 1/zoomScale, rotate by player orientation, translate
      // (matches EXT app: worldPos = rotate(eyePos/zs, playerOri) + cameraPos)
      const scaled = quatRotateVec3(playerOri, localX / zs, localY / zs, localZ / zs);
      const worldX = scaled.x + camPosX;
      const worldY = scaled.y + EYE_HEIGHT + camPosY;
      const worldZ = scaled.z + camPosZ;

      // worldOri = playerOri * localOri (compose rotation)
      const lo = t.orientation;
      const wo = quatMultiply(playerOri, { x: lo.x, y: lo.y, z: lo.z, w: lo.w });

      try {
        const newTransform = new XRRigidTransform(
          { x: worldX, y: worldY, z: worldZ },
          { x: wo.x, y: wo.y, z: wo.z, w: wo.w });
        Object.defineProperty(view, 'transform', {
          value: newTransform, configurable: true });
      } catch (e) { /* fallback: view unchanged */ }
    }
  }

  // Override projection with Kooima asymmetric frustum.
  // When display dimensions are available, compute Kooima directly from
  // display-relative eye positions + display rect (matches ext app exactly).
  // Falls back to runtime FOV angles if display info unavailable.
  const near = this.session?.renderState?.depthNear ?? 0.1;
  const far = this.session?.renderState?.depthFar ?? 1000;

  if (displayWidthM > 0 && displayHeightM > 0 && rawEyePositions) {
    // Get canvas dimensions for viewport scaling
    const baseLayer = this.session?.renderState?.baseLayer;
    const canvasW = baseLayer ? baseLayer.framebufferWidth : (captureWidth || 1920);
    const canvasH = baseLayer ? baseLayer.framebufferHeight : (captureHeight || 1080);

    const { screenW: fullScreenW, screenH: fullScreenH } = computeKooimaScreenRect(canvasW, canvasH);
    const zs = zoomScale;
    // SBS mode: each eye sees half the display width
    const sbsMode = displayMode === 'sbs';

    for (let i = 0; i < pose.views.length && i < rawEyePositions.length; i++) {
      const [ex, ey, ez] = rawEyePositions[i];
      const kooimaEye = [ex / zs, ey / zs, ez / zs];
      const screenW = (sbsMode ? fullScreenW / 2 : fullScreenW) / zs;
      const screenH = fullScreenH / zs;
      const newMatrix = kooimaProjectionMatrix(kooimaEye, screenW, screenH, near, far);
      try {
        Object.defineProperty(pose.views[i], 'projectionMatrix', {
          value: newMatrix, configurable: true });
      } catch (e) {
        pose.views[i].projectionMatrix = newMatrix;
      }
    }
  } else if (fovAngles) {
    // Fallback: use runtime FOV angles
    for (let i = 0; i < pose.views.length && i < fovAngles.length; i++) {
      const [aL, aR, aU, aD] = fovAngles[i];
      const newMatrix = fovToProjectionMatrix(aL, aR, aU, aD, near, far);
      try {
        Object.defineProperty(pose.views[i], 'projectionMatrix', {
          value: newMatrix, configurable: true });
      } catch (e) {
        pose.views[i].projectionMatrix = newMatrix;
      }
    }
  }

  // One-time init logging: all relevant display/projection variables
  if (!loggedBridgeInit && rawEyePositions && displayWidthM > 0) {
    loggedBridgeInit = true;
    const baseLayer = this.session?.renderState?.baseLayer;
    const vpW = baseLayer ? baseLayer.framebufferWidth : (captureWidth || 0);
    const vpH = baseLayer ? baseLayer.framebufferHeight : (captureHeight || 0);
    const texW = Math.round(vpW * viewScaleX);
    const texH = Math.round(vpH * viewScaleY);
    const sbsMode = displayMode === 'sbs';
    const { screenW: fullSW, screenH: fullSH } = computeKooimaScreenRect(vpW, vpH);
    const screenW = sbsMode ? fullSW / 2 : fullSW;

    console.log('=== MonadoXR Bridge Init ===');
    console.log(`  Display pose: pos=(0, ${EYE_HEIGHT}, 0) ori=(0, 0, 0, 1)`);
    console.log(`  Display: ${displayWidthM.toFixed(4)}x${displayHeightM.toFixed(4)} m, ${displayPixelW}x${displayPixelH} px`);
    console.log(`  viewScale: ${viewScaleX.toFixed(3)}x${viewScaleY.toFixed(3)}`);
    console.log(`  Raw eyes (DISPLAY frame): L=(${rawEyePositions[0].map(v => v.toFixed(5)).join(', ')}) R=(${rawEyePositions[1].map(v => v.toFixed(5)).join(', ')})`);

    // World-space eyes for each view
    for (let i = 0; i < pose.views.length; i++) {
      const t = pose.views[i].transform;
      const side = i === 0 ? 'L' : 'R';
      console.log(`  Eye ${side} (world): pos=(${t.position.x.toFixed(5)}, ${t.position.y.toFixed(5)}, ${t.position.z.toFixed(5)})`);
    }

    console.log(`  Viewport: ${vpW}x${vpH}, texture: ${texW}x${texH} (viewport * viewScale)`);
    console.log(`  Kooima screen rect: ${screenW.toFixed(4)}x${fullSH.toFixed(4)} m (${sbsMode ? 'SBS half' : 'full'} width)`);
    console.log(`  displayMode: ${displayMode}, browserDisplay: ${browserDisplay}`);
    console.log('============================');
  }

  return pose;
};

// Monkey-patch XRSession.requestAnimationFrame to capture frames after render
const origRAF = XRSession.prototype.requestAnimationFrame;
XRSession.prototype.requestAnimationFrame = function(callback) {
  return origRAF.call(this, (time, frame) => {
    // Let the app render first
    callback(time, frame);

    // Apply display processing if browser handles display (anaglyph/blend)
    if (displayMode !== 'sbs' && displayMode !== 'none') {
      const baseLayer = this.renderState?.baseLayer;
      if (baseLayer) {
        const gl = baseLayer.context;
        const glCanvas = gl?.canvas;
        if (gl && glCanvas) {
          applyDisplayProcessing(gl, glCanvas.width, glCanvas.height);
        }
      }
    }

    // Skip frame capture if browser handles display
    if (browserDisplay) return;

    // Then capture the result and send to native host via content script relay
    if (!wsConnected) return;

    // Find the WebGL canvas (the one iwer renders SBS content to)
    const baseLayer = this.renderState?.baseLayer;
    if (!baseLayer) return;

    const glCanvas = baseLayer.context?.canvas;
    if (!glCanvas) return;

    // Use native host's requested dimensions, or fall back to canvas native size
    const w = captureWidth || glCanvas.width;
    const h = captureHeight || glCanvas.height;

    // Lazy-create or resize offscreen capture canvas
    if (!captureCanvas || captureCanvas.width !== w || captureCanvas.height !== h) {
      captureCanvas = document.createElement('canvas');
      captureCanvas.width = w;
      captureCanvas.height = h;
      captureCtx = captureCanvas.getContext('2d', { willReadFrequently: true });
      console.log(`MonadoXR: Capture canvas ${w}x${h} (source: ${glCanvas.width}x${glCanvas.height})`);
    }

    // Draw WebGL canvas → 2D canvas (scales to match target dimensions)
    captureCtx.drawImage(glCanvas, 0, 0, w, h);

    // Get RGBA pixel data
    const imageData = captureCtx.getImageData(0, 0, w, h);

    // Build binary message: [uint32 width][uint32 height][RGBA pixels]
    const header = new ArrayBuffer(8);
    const headerView = new DataView(header);
    headerView.setUint32(0, w, true);  // little-endian
    headerView.setUint32(4, h, true);

    const packet = new Uint8Array(8 + imageData.data.length);
    packet.set(new Uint8Array(header), 0);
    packet.set(imageData.data, 8);

    // Transfer binary to content script (zero-copy via transferable)
    window.postMessage({ type: 'monado-ws-out', binary: packet.buffer }, '*', [packet.buffer]);
  });
};

console.log('Monado WebXR Bridge: iwer runtime installed, stereo SBS active, display processing ready');
