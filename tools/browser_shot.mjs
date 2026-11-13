// Captures a screenshot of the running playback page through the Chrome DevTools
// Protocol and prints the HUD values the browser actually measured.
//
//   1) start chrome:  chrome.exe --headless=new --remote-debugging-port=9222 \
//                       --user-data-dir=<temp> about:blank
//   2) node tools/browser_shot.mjs [port] [url] [out.png] [waitMs]
//
// Uses only built-in Node.js APIs (fetch + WebSocket + fs).

import { writeFileSync } from 'node:fs';

const port = process.argv[2] || '9222';
const pageUrl = process.argv[3] || 'http://127.0.0.1:8203/';
const outPath = process.argv[4] || 'evidence/browser.png';
const waitMs = Number(process.argv[5] || 6000);

const targets = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
const page = targets.find((target) => target.type === 'page');
if (!page) {
    console.error('no page target found on Chrome debugging port');
    process.exit(1);
}

const socket = new WebSocket(page.webSocketDebuggerUrl);
const pending = new Map();
let nextId = 1;

function send(method, params = {}) {
    const id = nextId++;
    socket.send(JSON.stringify({ id, method, params }));
    return new Promise((resolve, reject) => {
        pending.set(id, { resolve, reject });
    });
}

socket.onmessage = (event) => {
    const message = JSON.parse(event.data);
    if (message.id && pending.has(message.id)) {
        const { resolve } = pending.get(message.id);
        pending.delete(message.id);
        resolve(message.result);
    }
};

socket.onerror = (event) => {
    console.error('CDP socket error', event.message || event.type);
    process.exit(1);
};

socket.onopen = async () => {
    try {
        await send('Page.enable');
        await send('Runtime.enable');
        await send('Emulation.setDeviceMetricsOverride', {
            width: 1300, height: 820, deviceScaleFactor: 1, mobile: false,
        });
        await send('Page.navigate', { url: pageUrl });
        await new Promise((resolve) => setTimeout(resolve, waitMs));

        const hudExpression = `(() => {
            const read = (id) => (document.getElementById(id) || {}).textContent || '';
            return JSON.stringify({
                fps: read('fps'),
                bandwidth_mbps: read('bw'),
                latency_ms: read('lat'),
                frame_size: read('size'),
                resolution: read('res'),
                capture_mode: read('mode'),
                server_encode_fps: read('sfps'),
                frames_displayed: read('rx'),
                connection: read('status'),
                canvas_pixels: (() => { const c = document.getElementById('screen'); return c ? c.width + 'x' + c.height : null; })()
            });
        })()`;
        const hud = await send('Runtime.evaluate', { expression: hudExpression, returnByValue: true });
        console.log('HUD:', hud && hud.result ? hud.result.value : '(unavailable)');

        const screenshot = await send('Page.captureScreenshot', { format: 'png' });
        writeFileSync(outPath, Buffer.from(screenshot.data, 'base64'));
        console.log(`screenshot saved to ${outPath}`);
        socket.close();
        process.exit(0);
    } catch (error) {
        console.error('browser_shot failed:', error);
        process.exit(1);
    }
};
