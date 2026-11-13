#include "page.h"

namespace ss {

const char* PlaybackHtml() {
    static const char* kHtml = R"HTML(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ScreenStream 播放页</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  html, body { margin: 0; height: 100%; background: #0b0f14; color: #e6edf3;
    font: 13px/1.45 Consolas, ui-monospace, SFMono-Regular, Menlo, monospace; overflow: hidden; }
  header { display: flex; align-items: center; gap: 16px; flex-wrap: wrap;
    padding: 7px 14px; background: #11161d; border-bottom: 1px solid #1f2933; min-height: 42px; }
  header h1 { margin: 0 6px 0 0; font-size: 13px; letter-spacing: 2px; color: #7ee787; font-weight: 700; }
  .hud { display: flex; align-items: center; gap: 16px; flex-wrap: wrap; }
  .item { display: flex; align-items: baseline; gap: 5px; white-space: nowrap; }
  .k { color: #8b949e; }
  .v { color: #58a6ff; font-weight: 700; }
  .v.good { color: #3fb950; }
  .v.warn { color: #d29922; }
  .v.bad { color: #f85149; }
  .spacer { flex: 1 1 auto; }
  label.chk { display: flex; align-items: center; gap: 6px; color: #8b949e; cursor: pointer;
    user-select: none; white-space: nowrap; }
  #stage { position: relative; width: 100vw; height: calc(100vh - 42px); background: #000;
    display: flex; align-items: center; justify-content: center; overflow: hidden; }
  canvas#screen { max-width: 100%; max-height: 100%; width: auto; height: auto; background: #000;
    display: block; cursor: crosshair; }
  #placeholder { position: absolute; inset: 0; display: flex; align-items: center; justify-content: center;
    flex-direction: column; gap: 10px; color: #8b949e; pointer-events: none; text-align: center; }
  #placeholder .big { font-size: 18px; color: #58a6ff; letter-spacing: 1px; }
  #placeholder .sub { font-size: 12px; }
  #placeholder.hidden { display: none; }
  .warnbox { background: #2d1b00; border: 1px solid #9e6a03; color: #d29922; padding: 6px 10px;
    border-radius: 4px; font-size: 12px; }
  a { color: #58a6ff; }
</style>
</head>
<body>
<header>
  <h1>SCREENSTREAM</h1>
  <div class="hud">
    <div class="item"><span class="k">FPS</span><span class="v" id="fps">0.0</span></div>
    <div class="item"><span class="k">带宽</span><span class="v" id="bw">0.00</span><span class="k">Mbps</span></div>
    <div class="item"><span class="k">端到端延迟</span><span class="v" id="lat">-</span><span class="k">ms</span></div>
    <div class="item"><span class="k">帧大小</span><span class="v" id="size">-</span></div>
    <div class="item"><span class="k">分辨率</span><span class="v" id="res">1280x720</span></div>
    <div class="item"><span class="k">采集</span><span class="v" id="mode">-</span></div>
    <div class="item"><span class="k">服务端编码</span><span class="v" id="sfps">0.0</span><span class="k">fps</span></div>
    <div class="item"><span class="k">已显示</span><span class="v" id="rx">0</span><span class="k">帧</span></div>
    <div class="item"><span class="k">连接</span><span class="v" id="status">connecting</span></div>
  </div>
  <div class="spacer"></div>
  <label class="chk" title="勾选后鼠标移动/点击会通过 WebSocket 回传到本机并注入 SendInput">
    <input type="checkbox" id="enableInput"> 远程输入注入
  </label>
</header>

<div id="stage">
  <canvas id="screen" width="1280" height="720"></canvas>
  <div id="placeholder">
    <div class="big">等待视频帧 ...</div>
    <div class="sub">如果长时间没有画面，请确认 exe 已启动且桌面未被锁屏</div>
  </div>
</div>

<script>
(() => {
  'use strict';

  const MAGIC = 0x31465353;            // "SSF1" little endian
  const CHUNK_HEADER = 33;
  const MAX_CHUNKS = 1024;
  const $ = (id) => document.getElementById(id);

  const canvas = $('screen');
  const ctx = canvas.getContext('2d', { alpha: false, desynchronized: true });
  const placeholder = $('placeholder');
  const statusEl = $('status');
  const enableInput = $('enableInput');

  let socket = null;
  let reconnectTimer = 0;
  let lastBitmap = null;
  let framesReceived = 0;
  let framesDropped = 0;
  let lastHudUpdate = 0;
  let connectedAt = 0;

  const assembler = new Map();   // seq -> partial frame
  const fpsTimes = [];
  const bwSamples = [];          // { t, bytes }
  const latencySamples = [];     // ms

  function setStatus(text, cssClass) {
    statusEl.textContent = text;
    statusEl.className = 'v' + (cssClass ? ' ' + cssClass : '');
  }

  function connect() {
    clearTimeout(reconnectTimer);
    const scheme = location.protocol === 'https:' ? 'wss' : 'ws';
    setStatus('connecting');
    socket = new WebSocket(scheme + '://' + location.host + '/ws');
    socket.binaryType = 'arraybuffer';

    socket.onopen = () => {
      connectedAt = performance.now();
      setStatus('connected', 'good');
      $('rx').textContent = '0';
    };
    socket.onclose = () => {
      setStatus('disconnected', 'bad');
      socket = null;
      placeholder.classList.remove('hidden');
      reconnectTimer = setTimeout(connect, 1000);
    };
    socket.onerror = () => { if (socket) socket.close(); };
    socket.onmessage = (event) => {
      if (typeof event.data === 'string') return;
      try {
        handleChunk(new DataView(event.data));
      } catch (err) {
        console.error('chunk parse error', err);
      }
    };
  }

  function handleChunk(view) {
    if (view.byteLength < CHUNK_HEADER) return;
    if (view.getUint32(0, true) !== MAGIC) return;

    const seq = view.getUint32(4, true);
    const tsUs = view.getBigUint64(8, true);
    const width = view.getUint16(16, true);
    const height = view.getUint16(18, true);
    const format = view.getUint8(20);
    const totalLen = view.getUint32(21, true);
    const chunkIndex = view.getUint16(25, true);
    const chunkCount = view.getUint16(27, true);
    const chunkLen = view.getUint32(29, true);

    if (format !== 1 || chunkCount === 0 || chunkCount > MAX_CHUNKS) return;
    if (chunkIndex >= chunkCount) return;
    if (chunkLen > view.byteLength - CHUNK_HEADER) return;
    if (width === 0 || height === 0 || totalLen === 0 || totalLen > 32 * 1024 * 1024) return;

    let entry = assembler.get(seq);
    if (!entry || entry.totalLen !== totalLen || entry.chunkCount !== chunkCount) {
      entry = {
        totalLen, chunkCount, width, height, tsUs,
        chunks: new Array(chunkCount),
        bytes: 0,
        createdAt: performance.now(),
      };
      assembler.set(seq, entry);
    }

    if (!entry.chunks[chunkIndex]) {
      const offset = view.byteOffset + CHUNK_HEADER;
      entry.chunks[chunkIndex] = new Uint8Array(view.buffer, offset, chunkLen).slice();
      entry.bytes += chunkLen;
    }

    if (entry.bytes >= entry.totalLen) {
      assembler.delete(seq);
      renderFrame(entry);
    }

    // Drop stale partial frames (a lost chunk must not leak memory).
    if (assembler.size > 24) {
      const now = performance.now();
      for (const [key, value] of assembler) {
        if (now - value.createdAt > 2000) assembler.delete(key);
      }
    }
  }

  function renderFrame(entry) {
    const jpeg = new Uint8Array(entry.bytes);
    let offset = 0;
    for (let i = 0; i < entry.chunkCount; i++) {
      const chunk = entry.chunks[i];
      jpeg.set(chunk, offset);
      offset += chunk.length;
    }

    const blob = new Blob([jpeg], { type: 'image/jpeg' });
    createImageBitmap(blob).then((bitmap) => {
      if (lastBitmap) lastBitmap.close();
      lastBitmap = bitmap;
      if (canvas.width !== entry.width || canvas.height !== entry.height) {
        canvas.width = entry.width;
        canvas.height = entry.height;
        $('res').textContent = entry.width + 'x' + entry.height;
      }
      ctx.drawImage(bitmap, 0, 0, canvas.width, canvas.height);

      const now = performance.now();
      placeholder.classList.add('hidden');
      framesReceived++;
      $('rx').textContent = String(framesReceived);
      $('size').textContent = (entry.totalLen / 1024).toFixed(1) + ' KB';

      fpsTimes.push(now);
      bwSamples.push({ t: now, bytes: jpeg.length });
      // Same machine loopback: server epoch microseconds vs browser epoch clock.
      latencySamples.push((performance.timeOrigin + now) - Number(entry.tsUs) / 1000);
      if (latencySamples.length > 40) latencySamples.shift();

      updateHud(now);
    }).catch(() => { framesDropped++; });
  }

  function updateHud(now) {
    if (now - lastHudUpdate < 200) return;
    lastHudUpdate = now;

    while (fpsTimes.length && now - fpsTimes[0] > 2000) fpsTimes.shift();
    const fps = fpsTimes.filter((t) => now - t <= 1000).length;
    $('fps').textContent = fps.toFixed(1);
    $('fps').className = 'v ' + (fps >= 12 ? 'good' : fps > 0 ? 'warn' : 'bad');

    while (bwSamples.length && now - bwSamples[0].t > 2000) bwSamples.shift();
    const bytes = bwSamples.filter((s) => now - s.t <= 1000).reduce((acc, s) => acc + s.bytes, 0);
    const mbps = bytes * 8 / 1e6;
    $('bw').textContent = mbps.toFixed(2);

    const latency = latencySamples.length
      ? latencySamples.reduce((a, b) => a + b, 0) / latencySamples.length : 0;
    $('lat').textContent = latency.toFixed(1);
    $('lat').className = 'v ' + (latency < 80 ? 'good' : latency < 200 ? 'warn' : 'bad');

    if (socket && socket.readyState === WebSocket.OPEN) {
      const seconds = Math.max(0.001, (now - connectedAt) / 1000);
      $('sfps').textContent = (framesReceived / seconds).toFixed(1);
    }
  }

  async function pollServerStats() {
    try {
      const response = await fetch('/api/stats', { cache: 'no-store' });
      const data = await response.json();
      $('mode').textContent = (data.capture_mode || '?').toUpperCase();
      $('mode').className = 'v ' + (data.capture_mode === 'dxgi' ? 'good' : 'warn');
      $('sfps').textContent = Number(data.producer_fps || 0).toFixed(1);
    } catch (err) {
      // Server may be restarting; the WebSocket reconnect loop handles the UI state.
    }
    setTimeout(pollServerStats, 2000);
  }

  function toFrameCoords(event) {
    const rect = canvas.getBoundingClientRect();
    if (rect.width <= 0 || rect.height <= 0) return null;
    return {
      x: Math.round((event.clientX - rect.left) / rect.width * canvas.width),
      y: Math.round((event.clientY - rect.top) / rect.height * canvas.height),
    };
  }

  let lastMoveSent = 0;
  canvas.addEventListener('mousemove', (event) => {
    if (!enableInput.checked || !socket || socket.readyState !== WebSocket.OPEN) return;
    const now = performance.now();
    if (now - lastMoveSent < 33) return;
    lastMoveSent = now;
    const point = toFrameCoords(event);
    if (!point) return;
    socket.send(JSON.stringify({ type: 'mouse', x: point.x, y: point.y, click: 0 }));
  });

  canvas.addEventListener('mousedown', (event) => {
    if (!enableInput.checked || !socket || socket.readyState !== WebSocket.OPEN) return;
    const point = toFrameCoords(event);
    if (!point) return;
    socket.send(JSON.stringify({ type: 'mouse', x: point.x, y: point.y, click: 1 }));
  });

  enableInput.addEventListener('change', () => {
    if (!socket || socket.readyState !== WebSocket.OPEN) return;
    socket.send(JSON.stringify({ type: 'input', enable: enableInput.checked }));
  });

  connect();
  pollServerStats();
  console.log('ScreenStream playback page ready, protocol=SSF1 chunkHeader=' + CHUNK_HEADER);
})();
</script>
</body>
</html>
)HTML";
    return kHtml;
}

} // namespace ss
