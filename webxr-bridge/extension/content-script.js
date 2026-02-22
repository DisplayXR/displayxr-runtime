// Monado WebXR Bridge — Isolated-world content script
// Runs in Chrome's ISOLATED world (exempt from page CSP).
// Manages the WebSocket connection to the native bridge host and relays
// messages to/from the MAIN world polyfill (index.js) via window.postMessage.

const WS_URL = 'ws://localhost:9013';
const BACKPRESSURE_LIMIT = 4 * 1024 * 1024; // 4MB

let ws = null;

function connectWebSocket() {
  ws = new WebSocket(WS_URL);
  ws.binaryType = 'arraybuffer';

  ws.onopen = () => {
    console.log('MonadoXR: WebSocket connected to native host (content script)');
    window.postMessage({ type: 'monado-ws-status', connected: true }, '*');
  };

  ws.onmessage = (evt) => {
    if (typeof evt.data === 'string') {
      // JSON text (pose, config, etc.) — forward to main world
      window.postMessage({ type: 'monado-ws-in', json: evt.data }, '*');
    } else if (evt.data instanceof ArrayBuffer) {
      // Binary readback frame from Monado (Option A) — relay to main world
      window.postMessage({ type: 'monado-ws-binary', buffer: evt.data }, '*', [evt.data]);
    }
  };

  ws.onclose = () => {
    ws = null;
    console.log('MonadoXR: WebSocket disconnected, reconnecting in 2s...');
    window.postMessage({ type: 'monado-ws-status', connected: false }, '*');
    setTimeout(connectWebSocket, 2000);
  };

  ws.onerror = () => {
    // onclose will fire after this, triggering reconnect
  };
}

connectWebSocket();

// Listen for outgoing messages from the MAIN world polyfill
window.addEventListener('message', (evt) => {
  if (evt.source !== window) return;
  if (!evt.data || evt.data.type !== 'monado-ws-out') return;
  if (!ws || ws.readyState !== WebSocket.OPEN) return;

  if (evt.data.binary) {
    // Binary frame packet — check backpressure before sending
    if (ws.bufferedAmount > BACKPRESSURE_LIMIT) return;
    ws.send(evt.data.binary);
  } else if (evt.data.json) {
    // JSON string (resize, etc.)
    ws.send(evt.data.json);
  }
});
