// DisplayXR WebXR Bridge v2 — ISOLATED world content script.
//
// Owns the WebSocket connection to displayxr-webxr-bridge.exe (127.0.0.1:9014).
// Relays messages between the bridge and the MAIN world script via
// window.postMessage. Reconnects with exponential backoff on disconnect.
//
// No host permissions required — MV3 allows WebSocket to localhost from
// content scripts in ISOLATED world without explicit host_permissions.

(function () {
  'use strict';

  const WS_URL = 'ws://127.0.0.1:9014';
  const MSG_SOURCE_TO_MAIN = 'displayxr-bridge';
  const MSG_SOURCE_FROM_MAIN = 'displayxr-bridge-req';
  const BACKOFF_INITIAL_MS = 500;
  const BACKOFF_MAX_MS = 8000;

  let ws = null;
  let backoffMs = BACKOFF_INITIAL_MS;
  let reconnectTimer = null;

  function connect() {
    if (ws && (ws.readyState === WebSocket.CONNECTING || ws.readyState === WebSocket.OPEN)) {
      return;
    }

    try {
      ws = new WebSocket(WS_URL);
    } catch (e) {
      scheduleReconnect();
      return;
    }

    ws.onopen = function () {
      backoffMs = BACKOFF_INITIAL_MS;
      // Send hello with this extension's origin.
      ws.send(JSON.stringify({
        type: 'hello',
        version: 1,
        origin: window.location.origin || 'chrome-extension://'
      }));
    };

    ws.onmessage = function (event) {
      try {
        var payload = JSON.parse(event.data);
      } catch (e) {
        return;
      }
      // Relay to MAIN world via postMessage.
      window.postMessage({
        source: MSG_SOURCE_TO_MAIN,
        payload: payload
      }, window.location.origin);
    };

    ws.onclose = function () {
      ws = null;
      scheduleReconnect();
    };

    ws.onerror = function () {
      // onclose will fire after onerror.
    };
  }

  function scheduleReconnect() {
    if (reconnectTimer) return;
    reconnectTimer = setTimeout(function () {
      reconnectTimer = null;
      connect();
      backoffMs = Math.min(backoffMs * 2, BACKOFF_MAX_MS);
    }, backoffMs);
  }

  // Listen for messages from MAIN world to forward to the bridge.
  window.addEventListener('message', function (event) {
    // Strict origin check.
    if (event.origin !== window.location.origin) return;
    if (!event.data || event.data.source !== MSG_SOURCE_FROM_MAIN) return;

    if (ws && ws.readyState === WebSocket.OPEN && event.data.payload) {
      ws.send(JSON.stringify(event.data.payload));
    }
  });

  // Start connection immediately.
  connect();
})();
