// DisplayXR WebXR Bridge v2 — ISOLATED world content script.
//
// Owns the WebSocket connection to displayxr-webxr-bridge.exe (127.0.0.1:9014).
// Relays messages between the bridge and the MAIN world script via
// window.postMessage. Reconnects with exponential backoff on disconnect.
//
// Connect policy: the bridge is only needed when a page actually uses
// session.displayXR. MAIN world only sends messages in response to
// session.displayXR usage, so we defer WebSocket.connect() until the first
// such message arrives. On pages where no app touches session.displayXR
// (legacy WebXR sites, non-XR sites), the WebSocket is never opened — so
// in Auto mode, the service's trampoline listener never sees a connection
// and does not spawn the bridge.
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
  // Messages from MAIN world that arrived before the WebSocket was open.
  // Flushed in ws.onopen. Once an app has triggered a connect the queue
  // is the only thing that guarantees bridge-attach reaches the bridge.
  const pendingQueue = [];

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
      postBridgeStatus();
      // Send hello with this extension's origin.
      ws.send(JSON.stringify({
        type: 'hello',
        version: 1,
        origin: window.location.origin || 'chrome-extension://'
      }));
      // Do NOT flush the pending queue here. The bridge's WS loop serves
      // one incoming message per iteration; hello + queued messages sent
      // back-to-back can race with the bridge's outgoing-queue drain (of
      // display-info, window-info, etc.) and occasionally lose frames.
      // Flush after the bridge's first response instead (see onmessage).
    };

    var hadFirstResponse = false;

    function flushQueueOnce() {
      if (hadFirstResponse) return;
      hadFirstResponse = true;
      while (pendingQueue.length > 0) {
        var payload = pendingQueue.shift();
        try {
          ws.send(JSON.stringify(payload));
        } catch (e) {
          // Socket died mid-flush; remaining items are lost, scheduleReconnect
          // will kick in on ws.onclose.
          break;
        }
      }
    }

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
      // First inbound message from bridge (usually display-info) proves the
      // bridge has finished processing hello. Safe to flush queued sends now.
      flushQueueOnce();
    };

    ws.onclose = function () {
      ws = null;
      postBridgeStatus();
      // Only retry once an app has shown intent. If the queue is empty
      // and we've never successfully opened a bridge session, stay idle —
      // this is the common case for legacy / non-XR pages.
      if (pendingQueue.length > 0) {
        scheduleReconnect();
      }
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

  function postBridgeStatus() {
    window.postMessage({
      source: MSG_SOURCE_TO_MAIN,
      payload: { type: 'bridge-status', connected: (ws && ws.readyState === WebSocket.OPEN) }
    }, window.location.origin);
  }

  // Listen for messages from MAIN world to forward to the bridge.
  window.addEventListener('message', function (event) {
    // Strict origin check.
    if (event.origin !== window.location.origin) return;
    if (!event.data || event.data.source !== MSG_SOURCE_FROM_MAIN) return;

    // Status query from sample — reply with current bridge connection state.
    // Never triggers a connect.
    if (event.data.payload && event.data.payload.type === 'status-request') {
      postBridgeStatus();
      return;
    }

    if (!event.data.payload) return;

    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify(event.data.payload));
      return;
    }

    // Any other message from MAIN world implies the app is using
    // session.displayXR. Queue it and kick off a connect if needed.
    pendingQueue.push(event.data.payload);
    connect();
  });

  // Note: no top-level connect() call. The WS only opens when MAIN world
  // sends its first displayXR-related message.
})();
