// Monado WebXR Bridge — Entry point
// Uses Meta's IWER (Immersive Web Emulation Runtime) to provide a complete
// WebXR implementation. Overrides Chrome's native navigator.xr (which doesn't
// support immersive-vr on macOS). WASD keyboard controls for camera movement.

import { XRDevice } from 'iwer';

// --- Device Configuration ---

const MONADO_CONFIG = {
  name: 'Monado 3D Display',
  controllerConfig: undefined,
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

const MOVE_SPEED = 2.0;   // m/s
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

// --- WASD Camera Controls ---

const keys = {};
let yaw = 0;
let lastTime = 0;

window.addEventListener('keydown', (e) => { keys[e.code] = true; });
window.addEventListener('keyup', (e) => { keys[e.code] = false; });

function updateCamera(time) {
  requestAnimationFrame(updateCamera);

  const now = time / 1000;
  const dt = lastTime > 0 ? Math.min(now - lastTime, 0.1) : 1 / 60;
  lastTime = now;

  const hasMovement = keys['KeyW'] || keys['KeyS'] || keys['KeyA'] ||
    keys['KeyD'] || keys['Space'] || keys['ShiftLeft'] ||
    keys['ArrowLeft'] || keys['ArrowRight'];
  if (!hasMovement) return;

  const moveSpeed = MOVE_SPEED * dt;

  if (keys['ArrowLeft']) yaw += 1.5 * dt;
  if (keys['ArrowRight']) yaw -= 1.5 * dt;

  const fwdX = -Math.sin(yaw);
  const fwdZ = -Math.cos(yaw);
  const rightX = Math.cos(yaw);
  const rightZ = -Math.sin(yaw);

  let dx = 0, dy = 0, dz = 0;
  if (keys['KeyW']) { dx += fwdX * moveSpeed; dz += fwdZ * moveSpeed; }
  if (keys['KeyS']) { dx -= fwdX * moveSpeed; dz -= fwdZ * moveSpeed; }
  if (keys['KeyD']) { dx += rightX * moveSpeed; dz += rightZ * moveSpeed; }
  if (keys['KeyA']) { dx -= rightX * moveSpeed; dz -= rightZ * moveSpeed; }
  if (keys['Space']) dy += moveSpeed;
  if (keys['ShiftLeft']) dy -= moveSpeed;

  const pos = xrDevice.position;
  pos.set(pos.x + dx, pos.y + dy, pos.z + dz);

  const halfYaw = yaw / 2;
  xrDevice.quaternion.set(0, Math.sin(halfYaw), 0, Math.cos(halfYaw));
}

requestAnimationFrame(updateCamera);

// --- WebSocket Frame Capture ---
// Sends stereo SBS frames to the native OpenXR bridge host via WebSocket.
// Protocol: [uint32 width][uint32 height][RGBA pixels]

const WS_URL = 'ws://localhost:9013';
const BACKPRESSURE_LIMIT = 4 * 1024 * 1024; // 4MB

let ws = null;
let wsConnected = false;
let captureCanvas = null;
let captureCtx = null;
// Target capture dimensions — set by native host config message, or canvas native size
let captureWidth = 0;
let captureHeight = 0;
// Per-eye Kooima FOV from OpenXR runtime — [[angleL,angleR,angleU,angleD], [same for right eye]]
let fovAngles = null;

function connectWebSocket() {
  ws = new WebSocket(WS_URL);
  ws.binaryType = 'arraybuffer';

  ws.onopen = () => {
    wsConnected = true;
    console.log('MonadoXR: WebSocket connected to native host');
  };

  ws.onmessage = (evt) => {
    // Text messages are JSON from native host
    if (typeof evt.data === 'string') {
      try {
        const msg = JSON.parse(evt.data);
        // Config message: swapchain dimensions
        if (msg.w && msg.h) {
          captureWidth = msg.w;
          captureHeight = msg.h;
          captureCanvas = null;
          captureCtx = null;
          console.log(`MonadoXR: Native host requested capture at ${msg.w}x${msg.h}`);
        }
        // Pose message: [px, py, pz, qx, qy, qz, qw]
        if (msg.pose) {
          const [px, py, pz, qx, qy, qz, qw] = msg.pose;
          xrDevice.position.set(px, py, pz);
          xrDevice.quaternion.set(qx, qy, qz, qw);
        }
        // Per-eye FOV from Kooima projection: [[angleL,angleR,angleU,angleD],[...]]
        if (msg.fov) {
          fovAngles = msg.fov;
        }
      } catch (e) { /* ignore non-JSON */ }
    }
  };

  ws.onclose = () => {
    wsConnected = false;
    console.log('MonadoXR: WebSocket disconnected, reconnecting in 2s...');
    setTimeout(connectWebSocket, 2000);
  };

  ws.onerror = () => {
    // onclose will fire after this, triggering reconnect
  };
}

connectWebSocket();

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

// Override projection matrices with Kooima FOV from the OpenXR runtime
const origGetViewerPose = XRFrame.prototype.getViewerPose;
XRFrame.prototype.getViewerPose = function(refSpace) {
  const pose = origGetViewerPose.call(this, refSpace);
  if (pose && fovAngles) {
    const near = this.session?.renderState?.depthNear ?? 0.1;
    const far = this.session?.renderState?.depthFar ?? 1000;
    for (let i = 0; i < pose.views.length && i < fovAngles.length; i++) {
      const [aL, aR, aU, aD] = fovAngles[i];
      const newMatrix = fovToProjectionMatrix(aL, aR, aU, aD, near, far);
      try {
        Object.defineProperty(pose.views[i], 'projectionMatrix', {
          value: newMatrix,
          configurable: true,
        });
      } catch (e) {
        pose.views[i].projectionMatrix = newMatrix;
      }
    }
  }
  return pose;
};

// Monkey-patch XRSession.requestAnimationFrame to capture frames after render
const origRAF = XRSession.prototype.requestAnimationFrame;
XRSession.prototype.requestAnimationFrame = function(callback) {
  return origRAF.call(this, (time, frame) => {
    // Let the app render first
    callback(time, frame);

    // Then capture the result
    if (!wsConnected || !ws || ws.readyState !== WebSocket.OPEN) return;
    if (ws.bufferedAmount > BACKPRESSURE_LIMIT) return;

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

    ws.send(packet.buffer);
  });
};

console.log('Monado WebXR Bridge: iwer runtime installed, stereo SBS active, frame capture enabled');
