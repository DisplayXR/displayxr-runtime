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
  SPACE: 32, R: 82,
  LEFT: 37, UP: 38, RIGHT: 39, DOWN: 40,
  SHIFT: 16,
};
const keyDown = new Set();           // VK codes currently down
let mouseRightDown = false;
let mouseLastX = 0, mouseLastY = 0;
const RIG_DEFAULTS = {
  pos: [0, 0, 0],     // additive offset to nominalViewerPosition
  yaw: 0,             // around +Y, radians
  pitch: 0,           // around +X, radians
  ipdFactor: 1.0,
  parallaxFactor: 1.0,
  perspectiveFactor: 1.0,
  vHeight: VIRTUAL_DISPLAY_HEIGHT,
};
const rig = JSON.parse(JSON.stringify(RIG_DEFAULTS));
function rigReset() { Object.assign(rig, JSON.parse(JSON.stringify(RIG_DEFAULTS))); }

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

// Local Kooima asymmetric frustum from eye position relative to display center.
// Matches display3d_view.c: frustum edges = near * (screen_edge - eye_xy) / eye_z.
function buildKooimaProjection(eye, displayW, displayH) {
  const ex = eye[0], ey = eye[1], ez = eye[2];
  const halfW = displayW * 0.5;
  const halfH = displayH * 0.5;
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

  // Apply held-key movement to rig. Speed scales with vHeight so zoom feels
  // proportional (matches cube_handle: base 0.1 units/sec * m2v / scaleFactor).
  const moveSpeed = 0.3 * rig.vHeight;  // m/s
  const rotSpeed = 1.2;                 // rad/s
  if (dt > 0) {
    let mx = 0, my = 0, mz = 0;
    if (keyDown.has(VK.W)) mz -= 1;
    if (keyDown.has(VK.S)) mz += 1;
    if (keyDown.has(VK.A)) mx -= 1;
    if (keyDown.has(VK.D)) mx += 1;
    if (keyDown.has(VK.Q)) my -= 1;
    if (keyDown.has(VK.E)) my += 1;
    if (mx || my || mz) {
      // Rotate movement vector by rig yaw (forward = -Z in scene).
      const cy = Math.cos(rig.yaw), sy = Math.sin(rig.yaw);
      const wx = cy * mx + sy * mz;
      const wz = -sy * mx + cy * mz;
      rig.pos[0] += wx * moveSpeed * dt;
      rig.pos[1] += my * moveSpeed * dt;
      rig.pos[2] += wz * moveSpeed * dt;
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

  // Per-tile render dims: divide the actual framebuffer by the tile grid
  // (matches the bridge-relay compositor's mode-native tile-rect override).
  // Note: window-aware tile sizing (renderingPx = windowPx × viewScale) was
  // attempted but broke during compositor window resize because the compositor
  // defers atlas resize during drag (in_size_move) — sample and compositor
  // briefly disagree on tile rect. Proper fix needs the bridge to mirror the
  // compositor's actual atlas dims (sys->view_width/height) via IPC.
  const tileW = Math.floor(glLayer.framebufferWidth / tileLayout.tileColumns);
  const tileH = Math.floor(glLayer.framebufferHeight / tileLayout.tileRows);
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

  // Diagnostic log (once every 300 frames ~ 5s).
  if (frameCount % 300 === 1) {
    const fb = xrLayer;
    log('diag: fb=' + fb.framebufferWidth + 'x' + fb.framebufferHeight +
        ' tile=' + tileW + 'x' + tileH +
        ' winPx=' + (wi && wi.windowPixelSize ? wi.windowPixelSize.join('x') : 'none') +
        ' rig=pos[' + rig.pos.map(v => v.toFixed(2)).join(',') + ']' +
        ' yaw=' + rig.yaw.toFixed(2) + ' pitch=' + rig.pitch.toFixed(2) +
        ' vH=' + rig.vHeight.toFixed(3) +
        ' ipd=' + rig.ipdFactor.toFixed(2) +
        ' par=' + rig.parallaxFactor.toFixed(2) +
        ' per=' + rig.perspectiveFactor.toFixed(2));
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

  for (let eye = 0; eye < tileLayout.eyeCount; eye++) {
    const tileX = tileLayout.monoMode ? 0 : (eye % tileLayout.tileColumns);
    const tileY = tileLayout.monoMode ? 0 : Math.floor(eye / tileLayout.tileColumns);
    const vpX = tileX * tileW;
    const vpY = tileY * tileH;

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

    // Kooima projection (asymmetric frustum) computed locally from eye
    // position + screen — screen-relative angles are invariant to rig pose.
    if (screenHm > 0) {
      camera.projectionMatrix.copy(buildKooimaProjection(eyePos, screenWm, screenHm));
    } else {
      camera.projectionMatrix.copy(buildFallbackProjection(tileW, tileH));
    }
    camera.projectionMatrixInverse.copy(camera.projectionMatrix).invert();

    // Camera world position = rig.pos + rigQuat * (eye_in_rig * m2v_eff).
    // perspectiveFactor scales the eye XYZ in the m2v step — zoom without
    // moving the rig (cube_handle's perspective_factor).
    const m2v_eff = m2v * rig.perspectiveFactor;
    const eyeInRig = new THREE.Vector3(
      eyePos[0] * m2v_eff,
      eyePos[1] * m2v_eff,
      eyePos[2] * m2v_eff
    );
    eyeInRig.applyQuaternion(rigQuat);
    camera.position.set(
      rig.pos[0] + eyeInRig.x,
      rig.pos[1] + eyeInRig.y,
      rig.pos[2] + eyeInRig.z
    );
    camera.quaternion.copy(rigQuat);

    renderer.render(scene, camera);
  }

  activeXRFramebuffer = null;
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
    log('session.displayXR available:');
    log('  displayPixelSize: ' + JSON.stringify(displayXR.displayInfo.displayPixelSize));
    log('  displaySizeMeters: ' + JSON.stringify(displayXR.displayInfo.displaySizeMeters));
    log('  renderingMode: ' + displayXR.renderingMode.name +
        ' (' + displayXR.renderingMode.tileColumns + 'x' + displayXR.renderingMode.tileRows + ')');
    displayXR.configureEyePoses('raw');
  } else {
    log('session.displayXR NOT available — extension not loaded?');
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
    updateTileLayout();
  });

  xrSession.addEventListener('hardwarestatechange', (event) => {
    log('HW STATE: hw3D=' + event.detail.hardware3D);
  });

  xrSession.addEventListener('displayxrinput', (event) => {
    const m = event.detail;
    if (m.kind === 'key') {
      if (m.down) keyDown.add(m.code); else keyDown.delete(m.code);
      if (m.down && !m.repeat) {
        if (m.code === VK.SPACE || m.code === VK.R) rigReset();
      }
    } else if (m.kind === 'mouse') {
      if (m.event === 'down' && m.button === 1) {
        mouseRightDown = true;
        mouseLastX = m.x; mouseLastY = m.y;
      } else if (m.event === 'up' && m.button === 1) {
        mouseRightDown = false;
      } else if (m.event === 'move' && mouseRightDown) {
        const dx = m.x - mouseLastX;
        const dy = m.y - mouseLastY;
        mouseLastX = m.x; mouseLastY = m.y;
        // Mouse look: dx → yaw, dy → pitch (matches cube_handle 0.005 rad/px).
        rig.yaw -= dx * 0.005;
        rig.pitch -= dy * 0.005;
        if (rig.pitch > 1.4) rig.pitch = 1.4;
        if (rig.pitch < -1.4) rig.pitch = -1.4;
      }
    } else if (m.kind === 'wheel') {
      const notches = m.deltaY / 120;
      const mods = m.modifiers || {};
      if (mods.shift)      rig.ipdFactor = Math.max(0, Math.min(1, rig.ipdFactor + notches * 0.05));
      else if (mods.ctrl)  rig.parallaxFactor = Math.max(0, Math.min(1, rig.parallaxFactor + notches * 0.05));
      else if (mods.alt)   rig.perspectiveFactor = Math.max(0.1, Math.min(10, rig.perspectiveFactor * (notches > 0 ? 1.1 : 1/1.1)));
      else                  rig.vHeight = Math.max(0.05, Math.min(2.0, rig.vHeight * (notches > 0 ? 1/1.1 : 1.1))); // smaller vHeight = zoom in
    }
  });

  xrSession.addEventListener('windowinfochange', (event) => {
    const w = event.detail.windowInfo;
    if (w && w.valid) {
      log('WINDOW: ' + w.windowPixelSize[0] + 'x' + w.windowPixelSize[1] + 'px ' +
          w.windowSizeMeters[0].toFixed(3) + 'x' + w.windowSizeMeters[1].toFixed(3) + 'm ' +
          'off=[' + w.windowCenterOffsetMeters[0].toFixed(3) + ',' +
          w.windowCenterOffsetMeters[1].toFixed(3) + ']');
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

if (navigator.xr) {
  navigator.xr.isSessionSupported('immersive-vr').then(ok => {
    log('WebXR immersive-vr supported: ' + ok);
    enterBtn.disabled = !ok;
  });
} else {
  log('WebXR not available.');
  enterBtn.disabled = true;
}
