// DisplayXR WebXR Bridge v2 — sample (Phase 2).
//
// Bridge-aware WebXR page that:
// 1. Reads session.displayXR for display info and rendering mode.
// 2. Renders per-tile into Chrome's framebuffer using bridge-delivered
//    mode info (tile layout, view scale) instead of Chrome's getViewport().
// 3. Updates tile layout on 'renderingmodechange' events.
// 4. Uses bridge-delivered RAW eye poses.
//
// Uses raw GL calls (no three.js) to prove the rendering pipeline works
// without interference from three.js's internal state management.

const statusEl = document.getElementById('status');
const enterBtn = document.getElementById('enter-xr');
const exitBtn = document.getElementById('exit-xr');

function log(msg) {
  statusEl.textContent += msg + '\n';
  statusEl.scrollTop = statusEl.scrollHeight;
  console.log('[sample]', msg);
}

// --- XR state ---

let xrSession = null;
let gl = null;
let xrLayer = null;
let displayXR = null;
let frameCount = 0;

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

// --- XR frame loop (raw GL, no three.js) ---

function onXRFrame(time, frame) {
  if (!xrSession) return;
  xrSession.requestAnimationFrame(onXRFrame);
  frameCount++;

  // Re-read displayXR each frame for latest eye poses.
  displayXR = xrSession.displayXR;

  const glLayer = xrSession.renderState.baseLayer;
  if (!glLayer) return;
  gl.bindFramebuffer(gl.FRAMEBUFFER, glLayer.framebuffer);

  // Clear entire FB to dark.
  gl.disable(gl.SCISSOR_TEST);
  gl.viewport(0, 0, glLayer.framebufferWidth, glLayer.framebufferHeight);
  gl.clearColor(0.02, 0.02, 0.04, 1.0);
  gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

  // Per-tile render dims from bridge mode info.
  const tileW = Math.round(tileLayout.displayW * tileLayout.viewScaleX);
  const tileH = Math.round(tileLayout.displayH * tileLayout.viewScaleY);

  gl.enable(gl.SCISSOR_TEST);

  for (let eye = 0; eye < tileLayout.eyeCount; eye++) {
    const tileX = tileLayout.monoMode ? 0 : (eye % tileLayout.tileColumns);
    const tileY = tileLayout.monoMode ? 0 : Math.floor(eye / tileLayout.tileColumns);
    const vpX = tileX * tileW;
    const vpY = tileY * tileH;

    // Per-eye background: left=red, right=green, mono=blue.
    let r = 0, g = 0, b = 0;
    if (tileLayout.monoMode) { r = 0.15; g = 0.15; b = 0.35; }
    else if (eye === 0) { r = 0.4; g = 0.05; b = 0.05; }
    else { r = 0.05; g = 0.4; b = 0.05; }

    gl.scissor(vpX, vpY, tileW, tileH);
    gl.viewport(vpX, vpY, tileW, tileH);
    gl.clearColor(r, g, b, 1.0);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);

    // Animated white square with per-eye horizontal disparity.
    const t = time * 0.001;
    const squareSize = Math.min(tileW, tileH) * 0.2;
    const cx = vpX + tileW * 0.5;
    const cy = vpY + tileH * 0.5;
    const disparity = tileW * 0.03 * (eye === 0 ? -1 : 1);
    const bounce = Math.sin(t * 1.5) * tileH * 0.1;
    const sx = cx - squareSize * 0.5 + disparity;
    const sy = cy - squareSize * 0.5 + bounce;

    gl.scissor(sx, sy, squareSize, squareSize);
    gl.clearColor(1.0, 1.0, 1.0, 1.0);
    gl.clear(gl.COLOR_BUFFER_BIT);

    // Cyan crosshair at tile center.
    const cw = Math.max(2, Math.floor(tileW * 0.003));
    const ch = Math.max(2, Math.floor(tileH * 0.003));
    gl.scissor(cx - tileW * 0.06, cy - ch / 2, tileW * 0.12, ch);
    gl.clearColor(0.0, 1.0, 1.0, 1.0);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.scissor(cx - cw / 2, cy - tileH * 0.06, cw, tileH * 0.12);
    gl.clear(gl.COLOR_BUFFER_BIT);

    // Yellow mode indicator — small square in top-left of each tile.
    // Size changes with mode so you can see mode transitions.
    const indicatorSize = tileLayout.monoMode ? 30 : 15;
    gl.scissor(vpX + 4, vpY + tileH - indicatorSize - 4, indicatorSize, indicatorSize);
    gl.clearColor(1.0, 1.0, 0.0, 1.0);
    gl.clear(gl.COLOR_BUFFER_BIT);
  }

  gl.disable(gl.SCISSOR_TEST);
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

  // WebGL context.
  const canvas = document.createElement('canvas');
  gl = canvas.getContext('webgl2', { xrCompatible: true }) ||
       canvas.getContext('webgl', { xrCompatible: true });
  if (!gl) { log('failed to get WebGL context'); xrSession.end(); return; }

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
