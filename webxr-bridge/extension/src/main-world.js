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
  var activeSessions = []; // Proxy-wrapped sessions that receive events.

  // --- Helper: send a message to the bridge via ISOLATED world ---

  function sendToBridge(payload) {
    window.postMessage({
      source: MSG_SOURCE_TO_BRIDGE,
      payload: payload
    }, window.location.origin);
  }

  // --- Build the session.displayXR surface ---

  function buildDisplayXR() {
    if (!latestDisplayInfo) return null;

    var di = latestDisplayInfo;
    var rm = latestRenderingMode || null;

    return {
      displayInfo: {
        displayPixelSize: di.displayPixelSize,
        displaySizeMeters: di.displaySizeMeters,
        nominalViewerPosition: di.nominalViewerPosition
      },

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
    } else if (msg.type === 'eye-poses') {
      latestEyePoses = msg.eyes;
    }
  });

  // --- Wrap navigator.xr.requestSession ---

  if (!navigator.xr) return;

  var originalRequestSession = navigator.xr.requestSession.bind(navigator.xr);

  navigator.xr.requestSession = function () {
    var args = arguments;
    return originalRequestSession.apply(navigator.xr, args).then(function (realSession) {
      // Add displayXR as a getter directly on the session object.
      // No Proxy needed — avoids brand-check failures with XRWebGLLayer,
      // updateRenderState, and other WebXR APIs that reject Proxy wrappers.
      Object.defineProperty(realSession, 'displayXR', {
        get: function () { return buildDisplayXR(); },
        configurable: true
      });

      // Track this session for event dispatch.
      activeSessions.push({ session: realSession });

      realSession.addEventListener('end', function () {
        activeSessions = activeSessions.filter(function (e) { return e.session !== realSession; });
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
