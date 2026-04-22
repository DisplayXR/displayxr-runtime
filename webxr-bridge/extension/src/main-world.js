// DisplayXR WebXR Bridge v2 — MAIN world content script.
//
// Wraps navigator.xr.requestSession to return a Proxy over the real
// XRSession that exposes session.displayXR — the DisplayXR metadata
// surface fed by the bridge WebSocket (relayed via isolated-world.js).
//
// Also dispatches 'renderingmodechange' and 'hardwarestatechange' events
// on the session object so the page can react to runtime-initiated mode
// switches.

(function () {
  'use strict';

  const MSG_SOURCE_FROM_BRIDGE = 'displayxr-bridge';
  const MSG_SOURCE_TO_BRIDGE = 'displayxr-bridge-req';

  // --- State updated by bridge messages ---

  var latestDisplayInfo = null;
  var latestRenderingMode = null;
  var latestEyePoses = null;
  var latestWindowInfo = null;
  var activeSessions = []; // Proxy-wrapped sessions that receive events.

  // --- Ready promise infrastructure ---
  //
  // The first read of session.displayXR triggers the bridge attach (which
  // spawns the bridge in Auto mode). display-info arrives asynchronously —
  // apps await session.displayXR.ready before reading displayInfo fields.
  //
  // Cold-start budget: ~500 ms (service spawn + OpenXR init) + ~10 ms
  // (bridge handshake). 3 s gives healthy margin.

  var READY_TIMEOUT_MS = 3000;
  var readyPromise = null;
  var readyResolve = null;
  // ready resolves only when BOTH of these are true — otherwise the app
  // would see `ready` fire on display-info arrival while the compositor
  // gate (DXR_BridgeClientActive, set by the bridge on bridge-attach
  // handling) hasn't engaged yet, causing a tile-dim mismatch for the
  // first few frames of the new-session render.
  var readyHasDisplayInfo = false;
  var readyHasBridgeAck = false;

  function ensureReadyPromise() {
    if (readyPromise) return readyPromise;
    readyPromise = new Promise(function (resolve, reject) {
      readyResolve = resolve;
      setTimeout(function () {
        if (readyResolve) {
          readyResolve = null;
          readyPromise = null; // allow next access to retry
          reject(new Error('displayXR ready timeout'));
        }
      }, READY_TIMEOUT_MS);
    });
    return readyPromise;
  }

  function maybeResolveReady() {
    if (!readyHasDisplayInfo || !readyHasBridgeAck) return;
    if (readyResolve) {
      var fn = readyResolve;
      readyResolve = null;
      fn();
    }
  }

  function invalidateReady() {
    // Called on bridge disconnect / session end so the next getter access
    // builds a fresh promise and re-waits for the compositor gate to engage.
    readyResolve = null;
    readyPromise = null;
    // bridge-attached ACK is per-attach; require a new one for the next
    // session. display-info is a stable bridge-side cache — leave its flag
    // alone unless the bridge itself dropped (caller sets hasDisplayInfo = false
    // explicitly when appropriate).
    readyHasBridgeAck = false;
  }

  // --- Helper: send a message to the bridge via ISOLATED world ---

  function sendToBridge(payload) {
    window.postMessage({
      source: MSG_SOURCE_TO_BRIDGE,
      payload: payload
    }, window.location.origin);
  }

  // --- Build the session.displayXR surface ---

  function buildDisplayXR() {
    var ready = ensureReadyPromise();

    if (!latestDisplayInfo) {
      // Pending surface — apps await `ready` before reading fields.
      // Methods that queue bridge messages work now: isolated-world's queue
      // holds them until the WS opens, then flushes on connect.
      return {
        ready: ready,
        displayInfo: null,
        eyeTracking: null,
        windowInfo: null,
        renderingMode: null,
        eyePoses: null,
        renderingModes: null,
        computeFramebufferSize: function () { return null; },
        requestRenderingMode: function (modeIndex) {
          sendToBridge({ type: 'request-mode', version: 1, modeIndex: modeIndex });
        },
        requestEyeTrackingMode: function (mode) {
          sendToBridge({ type: 'request-eye-tracking-mode', version: 1, mode: mode });
        },
        sendHudUpdate: function (visible, lines) {
          sendToBridge({ type: 'hud-update', version: 1, visible: visible, lines: lines || [] });
        },
        configureEyePoses: function (format) {
          sendToBridge({ type: 'configure', version: 1, eyePoseFormat: format || 'raw' });
        }
      };
    }

    var di = latestDisplayInfo;
    var rm = latestRenderingMode || null;

    return {
      ready: ready,

      displayInfo: {
        displayPixelSize: di.displayPixelSize,
        displaySizeMeters: di.displaySizeMeters,
        nominalViewerPosition: di.nominalViewerPosition
      },

      // Eye-tracking capabilities advertised by the DP.
      //   supportedModes: string[] of 'MANAGED' / 'MANUAL'
      //   defaultMode:    'MANAGED' | 'MANUAL'
      // Apps MUST check supportedModes before calling requestEyeTrackingMode
      // — some devices only support one mode (e.g. Leia is MANAGED-only).
      eyeTracking: di.eyeTracking || { supportedModes: [], defaultMode: 'MANAGED' },

      // Compositor window info — present when the bridge could locate the
      // service compositor HWND. Updates live on resize/move via 'windowinfochange'
      // event on the session. Pages doing window-relative Kooima should use
      // windowSizeMeters as the screen and subtract windowCenterOffsetMeters from
      // each eye's XY position before computing the asymmetric frustum (matches
      // test_apps/cube_handle_d3d11_win/main.cpp:342-433).
      windowInfo: latestWindowInfo,

      renderingMode: rm ? {
        index: rm.currentModeIndex !== undefined ? rm.currentModeIndex : (di.currentModeIndex || 0),
        name: getModeName(rm.currentModeIndex !== undefined ? rm.currentModeIndex : (di.currentModeIndex || 0)),
        viewCount: getModeField('viewCount', rm.currentModeIndex),
        tileColumns: getModeField('tileColumns', rm.currentModeIndex),
        tileRows: getModeField('tileRows', rm.currentModeIndex),
        viewScale: getModeField('viewScale', rm.currentModeIndex),
        hardware3D: rm.hardware3D !== undefined ? rm.hardware3D : false,
        views: rm.views || di.views || []
      } : buildRenderingModeFromDisplayInfo(di),

      eyePoses: latestEyePoses,

      renderingModes: di.renderingModes || [],

      computeFramebufferSize: function () {
        var mode = this.renderingMode;
        if (!mode || !di.displayPixelSize) return { width: 1920, height: 1080 };
        var w = di.displayPixelSize[0] * (mode.viewScale ? mode.viewScale[0] : 1) * (mode.tileColumns || 1);
        var h = di.displayPixelSize[1] * (mode.viewScale ? mode.viewScale[1] : 1) * (mode.tileRows || 1);
        return { width: Math.round(w), height: Math.round(h) };
      },

      requestRenderingMode: function (modeIndex) {
        sendToBridge({ type: 'request-mode', version: 1, modeIndex: modeIndex });
      },

      requestEyeTrackingMode: function (mode) {
        sendToBridge({ type: 'request-eye-tracking-mode', version: 1, mode: mode });
      },

      sendHudUpdate: function (visible, lines) {
        sendToBridge({ type: 'hud-update', version: 1, visible: visible, lines: lines || [] });
      },

      configureEyePoses: function (format) {
        sendToBridge({ type: 'configure', version: 1, eyePoseFormat: format || 'raw' });
      }
    };
  }

  function buildRenderingModeFromDisplayInfo(di) {
    var idx = di.currentModeIndex || 0;
    var modes = di.renderingModes || [];
    var mode = modes[idx] || {};
    return {
      index: idx,
      name: mode.name || 'unknown',
      viewCount: mode.viewCount || 1,
      tileColumns: mode.tileColumns || 1,
      tileRows: mode.tileRows || 1,
      viewScale: mode.viewScale || [1, 1],
      hardware3D: mode.hardware3D || false,
      views: di.views || []
    };
  }

  function getModeName(idx) {
    if (!latestDisplayInfo || !latestDisplayInfo.renderingModes) return 'unknown';
    var m = latestDisplayInfo.renderingModes[idx];
    return m ? m.name : 'unknown';
  }

  function getModeField(field, idx) {
    if (idx === undefined && latestDisplayInfo) idx = latestDisplayInfo.currentModeIndex || 0;
    if (!latestDisplayInfo || !latestDisplayInfo.renderingModes) return undefined;
    var m = latestDisplayInfo.renderingModes[idx];
    return m ? m[field] : undefined;
  }

  // --- Listen for bridge messages from ISOLATED world ---

  window.addEventListener('message', function (event) {
    if (event.origin !== window.location.origin) return;
    if (!event.data || event.data.source !== MSG_SOURCE_FROM_BRIDGE) return;

    var msg = event.data.payload;
    if (!msg || !msg.type) return;

    if (msg.type === 'display-info') {
      latestDisplayInfo = msg;
      latestRenderingMode = buildRenderingModeFromDisplayInfo(msg);
      // display-info embeds the current window-info snapshot.
      if (msg.windowInfo) latestWindowInfo = msg.windowInfo;
      // First half of the ready gate. Full resolve only when bridge-attached
      // ACK also arrives (see below).
      readyHasDisplayInfo = true;
      maybeResolveReady();
    } else if (msg.type === 'bridge-attached') {
      // Bridge acknowledged bridge-attach by setting DXR_BridgeClientActive
      // on the compositor HWND. Safe to resolve ready — compositor gate is
      // live from this point onward.
      readyHasBridgeAck = true;
      maybeResolveReady();
    } else if (msg.type === 'window-info') {
      latestWindowInfo = msg.windowInfo || null;
      var wDetail = { windowInfo: latestWindowInfo };
      activeSessions.forEach(function (entry) {
        try {
          entry.session.dispatchEvent(new CustomEvent('windowinfochange', { detail: wDetail }));
        } catch (e) {}
      });
    } else if (msg.type === 'mode-changed') {
      // Update current mode index in display info cache.
      if (latestDisplayInfo) {
        latestDisplayInfo.currentModeIndex = msg.currentModeIndex;
        if (msg.views) latestDisplayInfo.views = msg.views;
      }
      latestRenderingMode = msg;

      // Dispatch renderingmodechange on all active sessions.
      var detail = {
        previousModeIndex: msg.previousModeIndex,
        currentModeIndex: msg.currentModeIndex,
        renderingMode: buildRenderingModeFromDisplayInfo(latestDisplayInfo || {}),
        hardware3D: msg.hardware3D
      };
      activeSessions.forEach(function (entry) {
        try {
          entry.session.dispatchEvent(new CustomEvent('renderingmodechange', { detail: detail }));
        } catch (e) { /* session may have ended */ }
      });
    } else if (msg.type === 'hardware-state-changed') {
      var hwDetail = { hardware3D: msg.hardware3D };
      activeSessions.forEach(function (entry) {
        try {
          entry.session.dispatchEvent(new CustomEvent('hardwarestatechange', { detail: hwDetail }));
        } catch (e) {}
      });
    } else if (msg.type === 'bridge-status') {
      if (!msg.connected) {
        // Bridge dropped — clear cached data and reset the per-connection
        // ready flags. Do NOT invalidateReady() here: during cold-spawn,
        // the trampoline's TCP accept-then-close causes a spurious WS
        // close/open cycle before the bridge is even up, and destroying
        // the pending readyResolve would leave the sample's existing
        // `await displayXR.ready` with no way to unblock. Instead keep
        // the promise pending; the flags will re-flip true on reconnect +
        // new display-info + new bridge-attached ACK, and the original
        // resolver fires then. If the disconnect is permanent (bridge
        // actually crashed), the 3 s timeout rejects.
        latestDisplayInfo = null;
        latestRenderingMode = null;
        latestWindowInfo = null;
        latestEyePoses = null;
        readyHasDisplayInfo = false;
        readyHasBridgeAck = false;
      }
      activeSessions.forEach(function (entry) {
        try {
          entry.session.dispatchEvent(new CustomEvent('bridgestatus', {
            detail: { connected: msg.connected }
          }));
        } catch (e) {}
      });
    } else if (msg.type === 'eye-poses') {
      latestEyePoses = msg.eyes;
    } else if (msg.type === 'input') {
      // Raw Win32 input event captured by the bridge from the compositor
      // window. Page implements its own semantics (WASD, mouse look, etc.)
      // — runtime's qwerty/HUD/V-toggle are gated off when bridge is active.
      activeSessions.forEach(function (entry) {
        try {
          entry.session.dispatchEvent(new CustomEvent('displayxrinput', { detail: msg }));
        } catch (e) {}
      });
    }
  });

  // --- Wrap navigator.xr.requestSession ---

  if (!navigator.xr) return;

  var originalRequestSession = navigator.xr.requestSession.bind(navigator.xr);

  navigator.xr.requestSession = function () {
    var args = arguments;
    return originalRequestSession.apply(navigator.xr, args).then(function (realSession) {
      // Per-session "this page is using displayXR" flag. First read of
      // session.displayXR sends a 'bridge-attach' to the bridge, which
      // sets the compositor-facing DXR_BridgeClientActive HWND prop.
      // Legacy WebXR pages never read the getter, so the compositor
      // stays on its normal non-bridge path even with the extension
      // loaded. On session 'end' we send 'bridge-detach' to release
      // the gate.
      var hasAttached = false;
      function markAttached() {
        if (hasAttached) return;
        hasAttached = true;
        sendToBridge({ type: 'bridge-attach', version: 1 });
      }

      // Add displayXR as a getter directly on the session object.
      // No Proxy needed — avoids brand-check failures with XRWebGLLayer,
      // updateRenderState, and other WebXR APIs that reject Proxy wrappers.
      //
      // The getter always fires markAttached (sends bridge-attach). In Auto
      // mode this is the signal that opens isolated-world's WebSocket and,
      // on first connect, spawns the bridge via the service trampoline. The
      // returned surface is always non-null; its `ready` Promise resolves
      // when display-info arrives. Legacy WebXR pages never touch this
      // getter, so they never spawn the bridge.
      Object.defineProperty(realSession, 'displayXR', {
        get: function () {
          markAttached();
          return buildDisplayXR();
        },
        configurable: true
      });

      // Track this session for event dispatch.
      activeSessions.push({ session: realSession });

      realSession.addEventListener('end', function () {
        activeSessions = activeSessions.filter(function (e) { return e.session !== realSession; });
        if (hasAttached) {
          sendToBridge({ type: 'bridge-detach', version: 1 });
          hasAttached = false;
        }
        // Next session's displayXR access must re-wait for a fresh
        // bridge-attach ACK before `ready` resolves — otherwise the app
        // would start rendering in bridge dims before the compositor gate
        // has re-engaged on the new session's HWND. display-info is a
        // stable bridge-side cache across sessions, so we keep that flag.
        invalidateReady();
      });

      return realSession;
    });
  };

  // Request eye pose streaming by default once display-info arrives.
  // The page can reconfigure via session.displayXR.configureEyePoses().
  var eyePoseRequested = false;
  window.addEventListener('message', function (event) {
    if (eyePoseRequested) return;
    if (!event.data || event.data.source !== MSG_SOURCE_FROM_BRIDGE) return;
    if (event.data.payload && event.data.payload.type === 'display-info') {
      eyePoseRequested = true;
      sendToBridge({ type: 'configure', version: 1, eyePoseFormat: 'raw' });
    }
  });
})();
