// DisplayXR WebXR Bridge v2 — three.js Kooima 3D sample.
//
// Replicates cube_handle_d3d11_win's display-centric rendering:
//  - Single Wood Crate cube at (0, 0.03, 0), size 0.06m, rotating 0.5 rad/s around Y
//  - 10x10 gray grid floor at y = -0.05m, 0.05m spacing
//  - Dark blue background (0.05, 0.05, 0.25)
//  - Directional light at (0.3, 0.8, 0.5) with 0.3 ambient + 0.7 diffuse
//  - Display-centric rig at camera origin (0, 0, 0), identity orientation
//  - Default tunables: ipdFactor=1, parallaxFactor=1, perspectiveFactor=1,
//    scaleFactor=1, virtualDisplayHeight=0.24m
//
// Kooima projection is built from bridge-delivered asymmetric FOV angles
// (already Kooima-computed in the runtime). Eye positions are scaled by
// m2v = virtualDisplayHeight / physicalDisplayHeightMeters to match the
// cube app's perspective/scale pipeline.

import * as THREE from 'three';

// --- DOM refs & logging ---

const statusEl = document.getElementById('status');
const enterBtn = document.getElementById('enter-xr');
const exitBtn = document.getElementById('exit-xr');

function log(msg) {
  statusEl.textContent += msg + '\n';
  // Keep the panel from growing unbounded — trim old lines.
  if (statusEl.textContent.length > 8000) {
    statusEl.textContent = statusEl.textContent.slice(-6000);
  }
  statusEl.scrollTop = statusEl.scrollHeight;
  console.log('[sample]', msg);
}

// --- Constants (match cube_handle_d3d11_win) ---

const NEAR = 0.01;
const FAR = 100.0;
const CUBE_SIZE = 0.06;
const CUBE_HEIGHT = 0.03;   // Y position
const CUBE_Z = 0.0;          // Z position (on display plane)
const CUBE_ROT_RATE = 0.5;   // rad/s around Y
const GRID_Y = -0.05;
const GRID_SIZE = 0.5;       // 10 * 0.05m
const GRID_DIVS = 10;
const GRID_COLOR = 0x4d4d59; // (0.3, 0.3, 0.35) gamma-corrected approx
const BG_COLOR = 0x0d0d40;   // (0.05, 0.05, 0.25)
const VIRTUAL_DISPLAY_HEIGHT = 0.24; // meters (4x cube height)
const CAMERA_HALF_TAN_VFOV = 0.32491969623; // tan(18°) → 36° vFOV (matches camera3d_view.c)

// --- XR state ---

let xrSession = null;
let gl = null;
let xrLayer = null;
let displayXR = null;
let frameCount = 0;

// --- Input state (driven by displayxrinput events from the bridge) ---
//
// Mirrors cube_handle_d3d11_win's input_handler: WASD/QE move the rig in
// view-relative directions, arrows rotate (yaw/pitch), right-mouse-drag looks
// around, mouse-wheel + modifiers tweak tunables. All transforms are applied
// to the rig pose; per-eye Kooima projection is unchanged.
const VK = {
  W: 87, A: 65, S: 83, D: 68, Q: 81, E: 69,
  C: 67, V: 86, T: 84, TAB: 9,
  SPACE: 32, R: 82,
  LEFT: 37, UP: 38, RIGHT: 39, DOWN: 40,
  SHIFT: 16,
};
const keyDown = new Set();           // VK codes currently down
let lastRequestedMode = -1;          // V key: track locally (async mode-changed is stale)
let eyeTrackingMode = 0;            // 0=MANAGED, 1=MANUAL (T key toggle)
let hudVisible = false;              // TAB key toggle
let hudJustToggled = false;          // Send one frame after toggle-off to clear
let mouseLookDown = false;           // Left-mouse-drag for look (matches cube_handle)
let mouseLastX = 0, mouseLastY = 0;
const RIG_DEFAULTS = {
  pos: [0, 0, 0],     // additive offset to nominalViewerPosition
  yaw: 0,             // around +Y, radians
  pitch: 0,           // around +X, radians
  ipdFactor: 1.0,
  parallaxFactor: 1.0,
  perspectiveFactor: 1.0,
  vHeight: VIRTUAL_DISPLAY_HEIGHT,
  cameraMode: false,          // false=display-centric, true=camera-centric (C key)
  invConvergenceDistance: 1.0, // camera-centric: 1/convergence_dist (1/meters)
  zoomFactor: 1.0,            // camera-centric: divides half_tan_vfov
};
const rig = JSON.parse(JSON.stringify(RIG_DEFAULTS));
function rigReset() {
  rig.pos = [0, 0, 0];
  rig.yaw = 0;
  rig.pitch = 0;
  rig.ipdFactor = 1.0;
  rig.parallaxFactor = 1.0;
  rig.perspectiveFactor = 1.0;
  rig.vHeight = VIRTUAL_DISPLAY_HEIGHT;
  rig.invConvergenceDistance = 1.0;
  rig.zoomFactor = 1.0;
  // Also drop any phantom held keys — bridge's hook can miss keyup events
  // during focus changes (alt-tab, menus), leaving stuck keys that drive
  // continuous motion/rotation.
  keyDown.clear();
  mouseLookDown = false;
  // Force the origin offset to re-calibrate on the next eye-pose frame, so
  // the current head position becomes the new display-centric baseline.
  originOffset = null;
  recalibKey = '';
  // Also reset prevTimeMs so the next frame doesn't see a huge dt.
  prevTimeMs = 0;
  // Force three.js to re-read all GL state from the context. Our monkey-
  // patched setRenderTarget + per-eye viewport/scissor juggling can leave
  // three.js's cached state out of sync with the real WebGL state — which
  // only resyncs on focus-regain via Chrome's context-restore path. Calling
  // resetState() does the same thing on demand.
  if (renderer) renderer.resetState();
}

// --- Three.js objects ---

let renderer = null;
let scene = null;
let camera = null;
let cubeMesh = null;
let activeXRFramebuffer = null;

// Current tile layout derived from bridge mode info.
let tileLayout = {
  tileColumns: 2,
  tileRows: 1,
  viewScaleX: 0.5,
  viewScaleY: 0.5,
  displayW: 1920,
  displayH: 1080,
  monoMode: false,
  eyeCount: 2,
};

function updateTileLayout() {
  displayXR = xrSession ? xrSession.displayXR : null;
  if (!displayXR || !displayXR.renderingMode) return;

  const rm = displayXR.renderingMode;
  const di = displayXR.displayInfo;
  tileLayout.tileColumns = rm.tileColumns || 1;
  tileLayout.tileRows = rm.tileRows || 1;
  tileLayout.viewScaleX = rm.viewScale ? rm.viewScale[0] : 1;
  tileLayout.viewScaleY = rm.viewScale ? rm.viewScale[1] : 1;
  tileLayout.displayW = di.displayPixelSize ? di.displayPixelSize[0] : 1920;
  tileLayout.displayH = di.displayPixelSize ? di.displayPixelSize[1] : 1080;
  tileLayout.monoMode = !rm.hardware3D;
  tileLayout.eyeCount = tileLayout.monoMode ? 1 : (rm.viewCount || 2);

  log('tile: ' + tileLayout.tileColumns + 'x' + tileLayout.tileRows +
      ' scale=' + tileLayout.viewScaleX.toFixed(2) + 'x' + tileLayout.viewScaleY.toFixed(2) +
      ' mono=' + tileLayout.monoMode + ' eyes=' + tileLayout.eyeCount);
}

// --- Scene construction ---

function createScene() {
  scene = new THREE.Scene();
  scene.background = new THREE.Color(BG_COLOR);

  // Lighting: directional (0.3, 0.8, 0.5) with 0.7 diffuse + 0.3 ambient.
  scene.add(new THREE.AmbientLight(0xffffff, 0.3));
  const dirLight = new THREE.DirectionalLight(0xffffff, 0.7);
  dirLight.position.set(0.3, 0.8, 0.5);
  scene.add(dirLight);

  // Wood Crate cube.
  const loader = new THREE.TextureLoader();
  const basecolor = loader.load('textures/Wood_Crate_001_basecolor.jpg');
  const normalMap = loader.load('textures/Wood_Crate_001_normal.jpg');
  const aoMap = loader.load('textures/Wood_Crate_001_ambientOcclusion.jpg');
  basecolor.colorSpace = THREE.SRGBColorSpace;

  const geo = new THREE.BoxGeometry(CUBE_SIZE, CUBE_SIZE, CUBE_SIZE);
  // Duplicate UVs to uv2 channel for AO map.
  geo.setAttribute('uv2', geo.getAttribute('uv'));

  const mat = new THREE.MeshStandardMaterial({
    map: basecolor,
    normalMap: normalMap,
    aoMap: aoMap,
    aoMapIntensity: 1.0,
    roughness: 0.8,
    metalness: 0.0,
  });

  cubeMesh = new THREE.Mesh(geo, mat);
  cubeMesh.position.set(0, CUBE_HEIGHT, CUBE_Z);
  scene.add(cubeMesh);

  // Grid floor at y = -0.05m, 10x10 lines of 0.05m spacing = 0.5m square.
  const grid = new THREE.GridHelper(GRID_SIZE, GRID_DIVS, GRID_COLOR, GRID_COLOR);
  grid.position.y = GRID_Y;
  scene.add(grid);
}

// --- Projection helpers ---

// Local Kooima asymmetric frustum — EXACT port of test_apps/common/display3d_view.c
// (display3d_compute_view + display3d_compute_projection).
//   m2v        = vHeight / physicalScreenH
//   es         = perspectiveFactor * m2v          (eye scale)
//   eye_scaled = eye * es                         (passed to projection)
//   kScreenW   = physicalScreenW * m2v            (virtual screen size)
//   kScreenH   = physicalScreenH * m2v
//   l = near * (-kScreenW/2 - eye_scaled.x) / eye_scaled.z     (similar triangles)
//   (same shape for r, t, b)
// The camera world position MUST be built from the same eye_scaled so the
// view matrix and projection matrix agree.
function buildKooimaProjection(eye, displayW, displayH, m2v, perspFac) {
  const vh = (m2v && m2v > 0) ? m2v : 1.0;
  const pf = (perspFac && perspFac > 0) ? perspFac : 1.0;
  const es = pf * vh;
  const ex = eye[0] * es, ey = eye[1] * es;
  let ez = eye[2] * es;
  if (ez <= 0.001) ez = 0.65;    // safety fallback (matches display3d_view.c:186)
  const halfW = displayW * 0.5 * vh;
  const halfH = displayH * 0.5 * vh;
  const l = NEAR * (-halfW - ex) / ez;
  const r = NEAR * (halfW - ex) / ez;
  const t = NEAR * (halfH - ey) / ez;
  const b = NEAR * (-halfH - ey) / ez;
  return new THREE.Matrix4().makePerspective(l, r, t, b, NEAR, FAR);
}

function buildFallbackProjection(tileW, tileH) {
  const aspect = tileW / tileH;
  const halfV = Math.tan((60 * Math.PI / 180) / 2) * NEAR;
  const halfH = halfV * aspect;
  return new THREE.Matrix4().makePerspective(-halfH, halfH, halfV, -halfV, NEAR, FAR);
}

// Camera-centric asymmetric frustum — port of camera3d_view.c:camera3d_compute_view.
// eyeLocal = processed eye displacement from screen plane (eye - {0,0,nominal_z}).
// Tangent-space shifts produce per-eye asymmetric frustum around the camera.
function buildCameraProjection(eyeLocal, screenWm, screenHm, invd, halfTanVfov) {
  const aspect = (screenHm > 0) ? screenWm / screenHm : 1.0;
  const ro = halfTanVfov * aspect;
  const uo = halfTanVfov;
  const dx = eyeLocal[0] * invd;
  const dy = eyeLocal[1] * invd;
  const dz = eyeLocal[2] * invd;
  const denom = 1.0 + dz;
  const tanR = (ro - dx) / denom;
  const tanL = (ro + dx) / denom;
  const tanU = (uo - dy) / denom;
  const tanD = (uo + dy) / denom;
  const l = -tanL * NEAR, r = tanR * NEAR;
  const b = -tanD * NEAR, t = tanU * NEAR;
  return new THREE.Matrix4().makePerspective(l, r, t, b, NEAR, FAR);
}

// --- XR frame loop ---

let prevTimeMs = 0;
let cubeRotation = 0;
// Origin offset — computed on first bridge eye-pose frame. Shifts bridge's
// LOCAL reference space (often not display-anchored — e.g. LOCAL origin may
// be at XR session start pose, not the display) so that the initial head
// position maps to nominalViewerPosition. After that, head motion produces
// parallax relative to this anchored display-centric frame.
let originOffset = null;
let recalibKey = '';

function onXRFrame(time, frame) {
  if (!xrSession) return;
  xrSession.requestAnimationFrame(onXRFrame);
  frameCount++;

  // Re-read displayXR each frame for latest eye poses.
  displayXR = xrSession.displayXR;

  const glLayer = xrSession.renderState.baseLayer;
  if (!glLayer) return;

  // Animate cube: 0.5 rad/s around Y (match cube_handle_d3d11_win).
  const dt = prevTimeMs > 0 ? (time - prevTimeMs) * 0.001 : 0;
  prevTimeMs = time;
  cubeRotation = (cubeRotation + dt * CUBE_ROT_RATE) % (Math.PI * 2);
  if (cubeMesh) cubeMesh.rotation.y = cubeRotation;

  // Apply held-key movement to rig. cube_handle uses 0.1 * m2v / scaleFactor;
  // since our rig.vHeight = cube_handle's (virtualDisplayHeight / scaleFactor)
  // already, this simplifies to 0.1 * m2v. Compute m2v locally (same formula
  // as used lower down for the projection).
  const _di0 = displayXR ? displayXR.displayInfo : null;
  const _wi0 = displayXR ? displayXR.windowInfo : null;
  const _useWin0 = !!(_wi0 && _wi0.valid && _wi0.windowSizeMeters);
  const _screenH0 = _useWin0 ? _wi0.windowSizeMeters[1]
                             : (_di0 && _di0.displaySizeMeters ? _di0.displaySizeMeters[1] : 0.194);
  const _m2v0 = (_screenH0 > 0) ? (rig.vHeight / _screenH0) : 1.0;
  const moveSpeed = 0.1 * _m2v0;                         // m/s
  const rotSpeed = 1.2;                                  // rad/s
  if (dt > 0) {
    let mx = 0, my = 0, mz = 0;
    if (keyDown.has(VK.W)) mz -= 1;   // forward = local -Z
    if (keyDown.has(VK.S)) mz += 1;
    if (keyDown.has(VK.A)) mx -= 1;   // left = local -X
    if (keyDown.has(VK.D)) mx += 1;
    if (keyDown.has(VK.Q)) my -= 1;   // down = local -Y
    if (keyDown.has(VK.E)) my += 1;
    if (mx || my || mz) {
      // Rotate the full move vector by the rig's yaw+pitch quaternion so all
      // six directions are rig-local (matches cube_handle's 6DOF flying cam).
      const moveQ = new THREE.Quaternion();
      moveQ.setFromEuler(new THREE.Euler(rig.pitch, rig.yaw, 0, 'YXZ'));
      const mv = new THREE.Vector3(mx, my, mz);
      mv.applyQuaternion(moveQ);
      rig.pos[0] += mv.x * moveSpeed * dt;
      rig.pos[1] += mv.y * moveSpeed * dt;
      rig.pos[2] += mv.z * moveSpeed * dt;
    }
    if (keyDown.has(VK.LEFT))  rig.yaw   += rotSpeed * dt;
    if (keyDown.has(VK.RIGHT)) rig.yaw   -= rotSpeed * dt;
    if (keyDown.has(VK.UP))    rig.pitch += rotSpeed * dt;
    if (keyDown.has(VK.DOWN))  rig.pitch -= rotSpeed * dt;
    if (rig.pitch > 1.4) rig.pitch = 1.4;
    if (rig.pitch < -1.4) rig.pitch = -1.4;
  }

  // Direct XR framebuffer to three.js via monkey-patch.
  activeXRFramebuffer = glLayer.framebuffer;
  gl.bindFramebuffer(gl.FRAMEBUFFER, glLayer.framebuffer);

  // Clear entire FB once.
  renderer.setViewport(0, 0, glLayer.framebufferWidth, glLayer.framebufferHeight);
  renderer.setScissor(0, 0, glLayer.framebufferWidth, glLayer.framebufferHeight);
  renderer.setScissorTest(true);
  renderer.setClearColor(BG_COLOR, 1.0);
  renderer.clear(true, true, false);

  const eyePoses = displayXR ? displayXR.eyePoses : null;
  const di = displayXR ? displayXR.displayInfo : null;
  const wi = displayXR ? displayXR.windowInfo : null;

  // Per-tile render dims: prefer the bridge-authoritative viewWidth/Height
  // (bridge computed = windowPixelSize × viewScale and pushed to the
  // compositor via DXR_BridgeViewW/H — same number we must render at).
  // Fallbacks exist for the first few frames before windowInfo arrives:
  // windowPixelSize × viewScale → displayPixelSize × viewScale → fb/grid.
  const haveBridgeView = !!(wi && wi.valid && wi.viewWidth > 0 && wi.viewHeight > 0);
  const pixW = (wi && wi.valid && wi.windowPixelSize) ? wi.windowPixelSize[0]
             : (di && di.displayPixelSize) ? di.displayPixelSize[0] : 0;
  const pixH = (wi && wi.valid && wi.windowPixelSize) ? wi.windowPixelSize[1]
             : (di && di.displayPixelSize) ? di.displayPixelSize[1] : 0;
  const tileW = haveBridgeView
    ? wi.viewWidth
    : (pixW > 0
      ? Math.round(pixW * tileLayout.viewScaleX)
      : Math.floor(glLayer.framebufferWidth / tileLayout.tileColumns));
  const tileH = haveBridgeView
    ? wi.viewHeight
    : (pixH > 0
      ? Math.round(pixH * tileLayout.viewScaleY)
      : Math.floor(glLayer.framebufferHeight / tileLayout.tileRows));
  // Window-relative Kooima: when the bridge has located the compositor window,
  // use its physical size as the screen and subtract the window-center offset
  // from each eye position. This matches cube_handle_d3d11_win's display-centric
  // path (main.cpp:342-433). Falls back to display dimensions when no window info.
  const useWindow = !!(wi && wi.valid && wi.windowSizeMeters);
  const screenWm = useWindow ? wi.windowSizeMeters[0]
                             : (di && di.displaySizeMeters ? di.displaySizeMeters[0] : 0.344);
  const screenHm = useWindow ? wi.windowSizeMeters[1]
                             : (di && di.displaySizeMeters ? di.displaySizeMeters[1] : 0.194);
  const winOffX = useWindow ? wi.windowCenterOffsetMeters[0] : 0;
  const winOffY = useWindow ? wi.windowCenterOffsetMeters[1] : 0;
  // m2v = virtualDisplayHeight / physicalScreenHeight (window or display).
  // m2v uses the LIVE rig vHeight (wheel-adjustable), not the const.
  const m2v = (screenHm > 0) ? (rig.vHeight / screenHm) : 1.0;
  const nominalPos = (di && di.nominalViewerPosition) ? di.nominalViewerPosition : [0, 0.1, 0.6];

  // Auto-calibrate origin offset on first valid eye-pose frame so that the
  // initial head midpoint maps to nominalViewerPosition (display-centric).
  if (eyePoses && eyePoses.length > 0) {
    // Re-calibrate if mode changed (viewCount swap) or first time.
    const key = tileLayout.monoMode + ':' + eyePoses.length;
    if (originOffset === null || key !== recalibKey) {
      let hx = 0, hy = 0, hz = 0;
      for (const e of eyePoses) {
        hx += e.position[0]; hy += e.position[1]; hz += e.position[2];
      }
      hx /= eyePoses.length; hy /= eyePoses.length; hz /= eyePoses.length;
      originOffset = [nominalPos[0] - hx, nominalPos[1] - hy, nominalPos[2] - hz];
      recalibKey = key;
      log('calibrated origin offset: [' +
          originOffset[0].toFixed(3) + ',' +
          originOffset[1].toFixed(3) + ',' +
          originOffset[2].toFixed(3) + ']');
    }
  }

  // Compute pair midpoint (head position) for IPD/parallax tunables.
  let headMid = nominalPos;
  if (eyePoses && eyePoses.length > 0 && originOffset) {
    let mx = 0, my = 0, mz = 0;
    for (const e of eyePoses) {
      mx += e.position[0] + originOffset[0];
      my += e.position[1] + originOffset[1];
      mz += e.position[2] + originOffset[2];
    }
    headMid = [mx / eyePoses.length, my / eyePoses.length, mz / eyePoses.length];
  }
  // parallaxFactor=0 → head locked to nominal; =1 → full head tracking.
  const trackedMid = [
    nominalPos[0] + (headMid[0] - nominalPos[0]) * rig.parallaxFactor,
    nominalPos[1] + (headMid[1] - nominalPos[1]) * rig.parallaxFactor,
    nominalPos[2] + (headMid[2] - nominalPos[2]) * rig.parallaxFactor,
  ];

  // Rig orientation as a three.js quaternion (yaw around Y, pitch around X).
  const rigQuat = new THREE.Quaternion();
  rigQuat.setFromEuler(new THREE.Euler(rig.pitch, rig.yaw, 0, 'YXZ'));

  // Diagnostic log (once every 600 frames ~ 10s).
  if (frameCount % 600 === 1) {
    const tileSrc = haveBridgeView ? ' (bridge)'
                 : (pixW > 0 ? ((wi && wi.valid && wi.windowPixelSize) ? ' (win*scale)' : ' (disp*scale)') : ' (fb/grid)');
    log('diag: tile=' + tileW + 'x' + tileH + tileSrc +
        ' ' + (rig.cameraMode ? 'CAM' : 'DISP') +
        ' rig=[' + rig.pos.map(v => v.toFixed(2)).join(',') + ']' +
        ' yaw=' + rig.yaw.toFixed(2) + ' pitch=' + rig.pitch.toFixed(2) +
        ' ipd=' + rig.ipdFactor.toFixed(2) +
        ' par=' + rig.parallaxFactor.toFixed(2) +
        (rig.cameraMode
          ? ' invD=' + rig.invConvergenceDistance.toFixed(2) + ' zoom=' + rig.zoomFactor.toFixed(2)
          : ' per=' + rig.perspectiveFactor.toFixed(2) + ' vH=' + rig.vHeight.toFixed(3)));
  }

  for (let eye = 0; eye < tileLayout.eyeCount; eye++) {
    const tileX = tileLayout.monoMode ? 0 : (eye % tileLayout.tileColumns);
    const tileY = tileLayout.monoMode ? 0 : Math.floor(eye / tileLayout.tileColumns);
    const vpX = tileX * tileW;
    // GL viewport Y=0 is at the BOTTOM. ANGLE maps GL coords to D3D11
    // (Y=0 at top). To place content at the D3D11 top (where the compositor
    // reads), set GL Y so that ANGLE's flip lands at D3D11 row 0.
    // GL vpY = fbHeight - tileH - (tileY * tileH)
    const vpY = glLayer.framebufferHeight - tileH - (tileY * tileH);

    renderer.setViewport(vpX, vpY, tileW, tileH);
    renderer.setScissor(vpX, vpY, tileW, tileH);
    renderer.setScissorTest(true);

    // Eye position in display-centric frame, with IPD/parallax tunables.
    // Then subtract window center offset so eye XY is window-relative.
    let eyePos;
    if (eyePoses && eyePoses.length > 0 && originOffset) {
      const idx = tileLayout.monoMode ? 0 : Math.min(eye, eyePoses.length - 1);
      const p = eyePoses[idx].position;
      const calibrated = [
        p[0] + originOffset[0],
        p[1] + originOffset[1],
        p[2] + originOffset[2],
      ];
      // Per-eye IPD offset from headMid, scaled by ipdFactor (0=mono).
      const offX = (calibrated[0] - headMid[0]) * rig.ipdFactor;
      const offY = (calibrated[1] - headMid[1]) * rig.ipdFactor;
      const offZ = (calibrated[2] - headMid[2]) * rig.ipdFactor;
      eyePos = [
        trackedMid[0] + offX - winOffX,
        trackedMid[1] + offY - winOffY,
        trackedMid[2] + offZ,
      ];
    } else {
      const ipdHalf = tileLayout.monoMode ? 0
        : (eye === 0 ? -0.0315 : 0.0315) * rig.ipdFactor;
      eyePos = [nominalPos[0] + ipdHalf - winOffX, nominalPos[1] - winOffY, nominalPos[2]];
    }

    if (rig.cameraMode) {
      // Camera-centric: app-owned camera at rig pose, eye tracking produces
      // per-eye asymmetric frustum shifts (matches camera3d_view.c).
      const nomZ = nominalPos[2];
      const eyeLocal = [eyePos[0], eyePos[1], eyePos[2] - nomZ];
      const halfTan = CAMERA_HALF_TAN_VFOV / rig.zoomFactor;

      if (screenHm > 0) {
        camera.projectionMatrix.copy(
          buildCameraProjection(eyeLocal, screenWm, screenHm, rig.invConvergenceDistance, halfTan));
      } else {
        camera.projectionMatrix.copy(buildFallbackProjection(tileW, tileH));
      }
      camera.projectionMatrixInverse.copy(camera.projectionMatrix).invert();

      // Camera position = rig + eye_local rotated into rig frame
      const eyeInRig = new THREE.Vector3(eyeLocal[0], eyeLocal[1], eyeLocal[2]);
      eyeInRig.applyQuaternion(rigQuat);
      camera.position.set(
        rig.pos[0] + eyeInRig.x,
        rig.pos[1] + eyeInRig.y,
        rig.pos[2] + eyeInRig.z
      );
      camera.quaternion.copy(rigQuat);
    } else {
      // Display-centric Kooima: screen scaled by m2v (vHeight zoom, FOV invariant),
      // eye scaled by perspectiveFactor (FOV change). Camera world position
      // uses the same eye*perspFac so view and projection stay consistent.
      if (screenHm > 0) {
        camera.projectionMatrix.copy(
          buildKooimaProjection(eyePos, screenWm, screenHm, m2v, rig.perspectiveFactor));
      } else {
        camera.projectionMatrix.copy(buildFallbackProjection(tileW, tileH));
      }
      camera.projectionMatrixInverse.copy(camera.projectionMatrix).invert();

      // eye_scaled = eye * perspectiveFactor * m2v (matches display3d_view.c)
      const es = rig.perspectiveFactor * m2v;
      const eyeInRig = new THREE.Vector3(eyePos[0] * es, eyePos[1] * es, eyePos[2] * es);
      eyeInRig.applyQuaternion(rigQuat);
      camera.position.set(
        rig.pos[0] + eyeInRig.x,
        rig.pos[1] + eyeInRig.y,
        rig.pos[2] + eyeInRig.z
      );
      camera.quaternion.copy(rigQuat);
    }

    renderer.render(scene, camera);
  }

  // Send HUD data to compositor via bridge shared memory.
  // Send every frame when visible (eye positions are live).
  // Send once when hidden to clear the overlay.
  if (hudVisible || hudJustToggled) {
    sendHUD(di, eyePoses, tileW, tileH);
    hudJustToggled = false;
  }

  activeXRFramebuffer = null;
}

// --- HUD overlay (compositor-side via shared memory) ---

let lastFpsTime = 0;
let fpsFrameCount = 0;
let currentFps = 0;

let hudSendCount = 0;
function sendHUD(di, eyePoses, tileW, tileH) {
  if (!displayXR) { if (hudSendCount === 0) log('sendHUD: displayXR null'); return; }

  // FPS
  const now = performance.now();
  fpsFrameCount++;
  if (now - lastFpsTime >= 1000) {
    currentFps = fpsFrameCount;
    fpsFrameCount = 0;
    lastFpsTime = now;
  }

  const mm = (v) => (v * 1000).toFixed(0);
  const f2 = (v) => v.toFixed(2);

  const lines = [];

  // Mode + tile
  const rm = displayXR.renderingMode;
  if (rm) {
    const sx = rm.viewScale ? f2(rm.viewScale[0]) : '1';
    const sy = rm.viewScale ? f2(rm.viewScale[1]) : '1';
    lines.push({ label: 'Mode', text: rm.name + ' (' + rm.tileColumns + 'x' + rm.tileRows + ') scale=' + sx + 'x' + sy });
  }
  lines.push({ label: 'Tile', text: tileW + 'x' + tileH + '  ' + currentFps + ' fps' });

  // Eyes
  if (eyePoses && eyePoses.length > 0) {
    let eyeStr = '';
    for (let i = 0; i < eyePoses.length; i++) {
      const p = eyePoses[i].position;
      eyeStr += (i > 0 ? '  ' : '') + (i === 0 ? 'L' : 'R') + '=(' + mm(p[0]) + ',' + mm(p[1]) + ',' + mm(p[2]) + ')';
    }
    eyeStr += ' mm [' + (eyeTrackingMode === 1 ? 'MANUAL' : 'MANAGED') + ']';
    lines.push({ label: 'Eyes', text: eyeStr });
  }

  // Kooima + params
  if (rig.cameraMode) {
    lines.push({ label: 'Kooima', text: 'Camera  invD=' + f2(rig.invConvergenceDistance) + ' zoom=' + f2(rig.zoomFactor) });
  } else {
    lines.push({ label: 'Kooima', text: 'Display  per=' + f2(rig.perspectiveFactor) + ' vH=' + rig.vHeight.toFixed(3) + 'm' });
  }
  lines.push({ label: 'Stereo', text: 'IPD=' + f2(rig.ipdFactor) + ' PAR=' + f2(rig.parallaxFactor) +
    ' rig=[' + rig.pos.map(v => f2(v)).join(',') + ']' });

  // Send via window.postMessage to the extension's isolated world,
  // which forwards to the bridge WS.
  const payload = { type: 'hud-update', version: 1, visible: hudVisible, lines: lines };
  window.postMessage({ source: 'displayxr-bridge-req', payload: payload }, window.location.origin);
  if (hudSendCount++ < 3) log('sendHUD: posted ' + lines.length + ' lines, visible=' + hudVisible);
}

// --- Enter/Exit XR ---

async function enterXR() {
  if (!navigator.xr) { log('navigator.xr not available'); return; }
  const supported = await navigator.xr.isSessionSupported('immersive-vr');
  if (!supported) { log('immersive-vr not supported'); return; }

  log('requesting immersive-vr session...');
  try {
    xrSession = await navigator.xr.requestSession('immersive-vr', {
      optionalFeatures: ['local', 'local-floor'],
    });
  } catch (e) {
    log('requestSession failed: ' + e.message);
    return;
  }

  displayXR = xrSession.displayXR;
  if (displayXR) {
    setDot(dotExtension, 'ok');
    setDot(dotBridge, 'ok');
    log('session.displayXR available:');
    log('  displayPixelSize: ' + JSON.stringify(displayXR.displayInfo.displayPixelSize));
    log('  displaySizeMeters: ' + JSON.stringify(displayXR.displayInfo.displaySizeMeters));
    log('  renderingMode: ' + displayXR.renderingMode.name +
        ' (' + displayXR.renderingMode.tileColumns + 'x' + displayXR.renderingMode.tileRows + ')');
    displayXR.configureEyePoses('raw');
  } else {
    setDot(dotExtension, 'err');
    setDot(dotBridge, 'err');
    log('session.displayXR NOT available — is the bridge running and extension loaded?');
    log('  Start displayxr-webxr-bridge.exe, then reload this page.');
  }

  xrSession.addEventListener('end', () => {
    log('session ended');
    xrSession = null;
    displayXR = null;
    if (renderer) { renderer.dispose(); renderer = null; }
    scene = null;
    camera = null;
    cubeMesh = null;
    prevTimeMs = 0;
    cubeRotation = 0;
    originOffset = null;
    recalibKey = '';
    enterBtn.disabled = false;
    exitBtn.disabled = true;
  });

  xrSession.addEventListener('renderingmodechange', (event) => {
    log('MODE CHANGE: prev=' + event.detail.previousModeIndex +
        ' curr=' + event.detail.currentModeIndex +
        ' hw3D=' + event.detail.hardware3D);
    lastRequestedMode = event.detail.currentModeIndex;
    updateTileLayout();
  });

  xrSession.addEventListener('bridgestatus', (event) => {
    setDot(dotBridge, event.detail.connected ? 'ok' : 'err');
    log('BRIDGE: ' + (event.detail.connected ? 'connected' : 'disconnected — is displayxr-webxr-bridge running?'));
  });

  // Debounce hardwarestatechange: the Leia driver briefly flaps 3D→2D→3D
  // during fullscreen⇄windowed transitions. Acting on each event forces a
  // real rendering-mode transition which drops eye tracking for seconds.
  // Only auto-request after the state has been stable for ~600 ms. If the
  // state flaps BACK to match current mode before the timer fires, cancel —
  // otherwise we'd force a transition to a state that's no longer wanted.
  let hwStatePendingTimer = null;
  let hwStatePending = null;
  xrSession.addEventListener('hardwarestatechange', (event) => {
    const hw3D = event.detail.hardware3D;
    log('HW STATE: hw3D=' + hw3D);
    if (!displayXR) return;
    const cur = displayXR.renderingMode;
    if (cur && cur.hardware3D === hw3D) {
      // Already matches — cancel any pending transition (driver flapped back).
      if (hwStatePendingTimer !== null) {
        clearTimeout(hwStatePendingTimer);
        hwStatePendingTimer = null;
        hwStatePending = null;
      }
      return;
    }
    hwStatePending = hw3D;
    if (hwStatePendingTimer !== null) clearTimeout(hwStatePendingTimer);
    hwStatePendingTimer = setTimeout(() => {
      hwStatePendingTimer = null;
      const pending = hwStatePending;
      hwStatePending = null;
      if (!displayXR) return;
      const curNow = displayXR.renderingMode;
      if (curNow && curNow.hardware3D === pending) return;  // settled
      const modes = displayXR.renderingModes || [];
      for (let i = 0; i < modes.length; i++) {
        if (modes[i].hardware3D === pending) {
          lastRequestedMode = i;
          displayXR.requestRenderingMode(i);
          log('auto-requesting mode ' + i + ' (' + modes[i].name +
              ') to match stable hw3D=' + pending);
          break;
        }
      }
    }, 600);
  });

  xrSession.addEventListener('displayxrinput', (event) => {
    const m = event.detail;
    if (m.kind === 'key') {
      const firstDown = m.down && !keyDown.has(m.code); // ignore repeats
      if (m.down) keyDown.add(m.code); else keyDown.delete(m.code);
      if (firstDown) {
        if (m.code === VK.SPACE || m.code === VK.R) {
          rigReset();
          log('rig reset');
        } else if (m.code === VK.C) {
          rig.cameraMode = !rig.cameraMode;
          // Reset rig on mode switch (matches input_handler.cpp:215-232)
          rig.yaw = 0;
          rig.pitch = 0;
          if (rig.cameraMode) {
            // Camera mode: place camera at nominalViewerZ, looking at display plane
            const dxr = displayXR ? displayXR.displayInfo : null;
            const nz = (dxr && dxr.nominalViewerPosition) ? dxr.nominalViewerPosition[2] : 0.6;
            rig.pos = [0, 0, nz];
            log('camera pos=[' + rig.pos.map(v => v.toFixed(2)).join(',') + '] nz=' + nz.toFixed(3));
            if (nz > 0) rig.invConvergenceDistance = 1.0 / nz;
            rig.zoomFactor = 1.0;
          } else {
            // Display-centric: rig at origin (display-anchored)
            rig.pos = [0, 0, 0];
          }
          originOffset = null; recalibKey = '';
          log('Kooima: ' + (rig.cameraMode ? 'camera-centric' : 'display-centric'));
        } else if (m.code === VK.V && displayXR) {
          // Cycle rendering mode (V key, matches cube_handle_d3d11_win).
          // Use lastRequestedMode (updated immediately) instead of
          // renderingMode.index (async — stale until mode-changed event).
          const modes = displayXR.renderingModes || [];
          if (modes.length > 0) {
            if (lastRequestedMode < 0) {
              lastRequestedMode = displayXR.renderingMode ? displayXR.renderingMode.index : 0;
            }
            const next = (lastRequestedMode + 1) % modes.length;
            lastRequestedMode = next;
            displayXR.requestRenderingMode(next);
            log('requesting mode ' + next + ' (' + (modes[next] ? modes[next].name : '?') + ')');
          }
        } else if (m.code >= 48 && m.code <= 56 && displayXR) {
          // 0-8 number keys: direct mode selection (matches cube_handle_d3d11_win)
          const idx = m.code - 48;
          const modes = displayXR.renderingModes || [];
          if (idx < modes.length) {
            displayXR.requestRenderingMode(idx);
            log('requesting mode ' + idx + ' (' + modes[idx].name + ')');
          }
        } else if (m.code === VK.T && displayXR) {
          // Toggle eye tracking mode: MANAGED(0) ↔ MANUAL(1)
          eyeTrackingMode = eyeTrackingMode === 1 ? 0 : 1;
          displayXR.requestEyeTrackingMode(eyeTrackingMode);
          log('eye tracking: ' + (eyeTrackingMode === 1 ? 'MANUAL' : 'MANAGED'));
        } else if (m.code === VK.TAB) {
          hudVisible = !hudVisible;
          hudJustToggled = true;
          log('HUD: ' + (hudVisible ? 'visible' : 'hidden'));
        }
      }
    } else if (m.kind === 'mouse') {
      if (m.event === 'down' && m.button === 0) {
        // Left-mouse-drag = look (matches cube_handle_d3d11_win).
        mouseLookDown = true;
        mouseLastX = m.x; mouseLastY = m.y;
      } else if (m.event === 'up' && m.button === 0) {
        mouseLookDown = false;
      } else if (m.event === 'move' && mouseLookDown) {
        const dx = m.x - mouseLastX;
        const dy = m.y - mouseLastY;
        mouseLastX = m.x; mouseLastY = m.y;
        rig.yaw -= dx * 0.005;
        rig.pitch -= dy * 0.005;
        if (rig.pitch > 1.4) rig.pitch = 1.4;
        if (rig.pitch < -1.4) rig.pitch = -1.4;
      }
    } else if (m.kind === 'wheel') {
      const notches = m.deltaY / 120;
      const mods = m.modifiers || {};
      const factor = notches > 0 ? 1.1 : 1 / 1.1;
      if (mods.shift) {
        rig.ipdFactor = Math.max(0, Math.min(1, rig.ipdFactor + notches * 0.05));
      } else if (mods.ctrl) {
        rig.parallaxFactor = Math.max(0, Math.min(1, rig.parallaxFactor + notches * 0.05));
      } else if (mods.alt) {
        if (rig.cameraMode) {
          rig.invConvergenceDistance = Math.max(0.1, Math.min(10, rig.invConvergenceDistance * factor));
        } else {
          rig.perspectiveFactor = Math.max(0.1, Math.min(10, rig.perspectiveFactor * factor));
        }
      } else {
        if (rig.cameraMode) {
          rig.zoomFactor = Math.max(0.1, Math.min(10, rig.zoomFactor * factor));
        } else {
          rig.vHeight = Math.max(0.05, Math.min(2.0, rig.vHeight * (notches > 0 ? 1 / 1.1 : 1.1)));
        }
      }
    }
  });

  xrSession.addEventListener('windowinfochange', (event) => {
    const w = event.detail.windowInfo;
    if (w && w.valid) {
      log('WINDOW: ' + w.windowPixelSize[0] + 'x' + w.windowPixelSize[1] + 'px ' +
          w.windowSizeMeters[0].toFixed(3) + 'x' + w.windowSizeMeters[1].toFixed(3) + 'm ' +
          'off=[' + w.windowCenterOffsetMeters[0].toFixed(3) + ',' +
          w.windowCenterOffsetMeters[1].toFixed(3) + ']' +
          (w.viewWidth ? ' view=' + w.viewWidth + 'x' + w.viewHeight : ' view=N/A'));
    } else {
      log('WINDOW: lost');
    }
  });

  // WebGL context + three.js renderer.
  const canvas = document.createElement('canvas');
  gl = canvas.getContext('webgl2', { xrCompatible: true }) ||
       canvas.getContext('webgl', { xrCompatible: true });
  if (!gl) { log('failed to get WebGL context'); xrSession.end(); return; }

  renderer = new THREE.WebGLRenderer({ canvas, context: gl });
  renderer.autoClear = false;
  renderer.setPixelRatio(1);
  renderer.outputColorSpace = THREE.SRGBColorSpace;

  // Monkey-patch setRenderTarget so three.js renders into the XR framebuffer
  // instead of framebuffer 0 (canvas) when we're inside the eye loop.
  const origSetRT = renderer.setRenderTarget.bind(renderer);
  renderer.setRenderTarget = function(target, ...args) {
    origSetRT(target, ...args);
    if (target === null && activeXRFramebuffer) {
      gl.bindFramebuffer(gl.FRAMEBUFFER, activeXRFramebuffer);
    }
  };

  // Create scene.
  createScene();

  // Camera: three.js default (at origin, looking -Z). We override
  // projectionMatrix per eye and set position/quaternion from bridge pose.
  camera = new THREE.PerspectiveCamera();

  // XR layer.
  xrLayer = new XRWebGLLayer(xrSession, gl);
  await xrSession.updateRenderState({ baseLayer: xrLayer });
  log('fb=' + xrLayer.framebufferWidth + 'x' + xrLayer.framebufferHeight);

  updateTileLayout();

  xrSession.requestAnimationFrame(onXRFrame);
  enterBtn.disabled = true;
  exitBtn.disabled = false;
  log('XR session started — press V in compositor to switch modes');
}

async function exitXR() {
  if (xrSession) { try { await xrSession.end(); } catch (e) {} }
}

enterBtn.addEventListener('click', enterXR);
exitBtn.addEventListener('click', exitXR);

// Clear stuck-key state on focus loss / visibility change. The bridge's
// low-level hook can miss keyup events when the compositor window isn't the
// foreground window, leaving phantom keys in keyDown that drive runaway
// rotation/movement.
window.addEventListener('blur', () => { keyDown.clear(); mouseLookDown = false; });
document.addEventListener('visibilitychange', () => {
  if (document.hidden) { keyDown.clear(); mouseLookDown = false; }
});

// --- Status indicators ---
const dotWebXR = document.getElementById('dot-webxr');
const dotExtension = document.getElementById('dot-extension');
const dotBridge = document.getElementById('dot-bridge');

function setDot(dot, state) { // 'ok', 'err', or '' (gray)
  dot.classList.remove('ok', 'err');
  if (state) dot.classList.add(state);
}

// Listen for bridge-status from extension (fires before any XR session).
window.addEventListener('message', (event) => {
  if (event.data && event.data.source === 'displayxr-bridge' &&
      event.data.payload && event.data.payload.type === 'bridge-status') {
    const connected = event.data.payload.connected;
    setDot(dotExtension, 'ok'); // got a reply → extension is loaded
    setDot(dotBridge, connected ? 'ok' : 'err');
  }
});

// Check WebXR support.
if (navigator.xr) {
  navigator.xr.isSessionSupported('immersive-vr').then(ok => {
    log('WebXR immersive-vr supported: ' + ok);
    setDot(dotWebXR, ok ? 'ok' : 'err');
    enterBtn.disabled = !ok;
  });
} else {
  log('WebXR not available.');
  setDot(dotWebXR, 'err');
  enterBtn.disabled = true;
}

// Ask the extension for current bridge status. The extension's isolated-world
// handles 'status-request' and replies with 'bridge-status'. If the extension
// isn't loaded, no reply comes and dots stay gray.
window.postMessage({
  source: 'displayxr-bridge-req',
  payload: { type: 'status-request' }
}, window.location.origin);
