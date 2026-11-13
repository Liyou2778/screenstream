// Minimal ScreenStream WebSocket probe using Node.js >= 22 built-in WebSocket.
//
//   node tools/ws_probe.mjs [ws://127.0.0.1:8203/ws] [frames=30]
//
// Speaks the SSF1 chunk protocol by hand (33 byte header per chunk), reassembles
// whole JPEG frames and reports average frame size, byte rate and frame interval.
// No npm dependencies are required.

const url = process.argv[2] || 'ws://127.0.0.1:8203/ws';
const targetFrames = Number(process.argv[3] || 30);

const MAGIC = 0x31465353; // "SSF1" little endian
const CHUNK_HEADER = 33;

const socket = new WebSocket(url);
socket.binaryType = 'arraybuffer';

const assembler = new Map();
const frames = [];
const intervals = [];
const startedAt = performance.now();
let firstFrameAt = 0;
let lastFrameAt = 0;
let totalPayload = 0;
let done = false;

const timeout = setTimeout(() => {
    console.error(`timeout: only ${frames.length}/${targetFrames} frames received`);
    finish(1);
}, 20000);

socket.onopen = () => console.log(`connected: ${url}`);

socket.onerror = (event) => {
    console.error('websocket error:', event.message || event.type || 'unknown');
    finish(1);
};

socket.onclose = () => {
    if (!done) {
        console.log('websocket closed by server');
        finish(1);
    }
};

socket.onmessage = (event) => {
    if (typeof event.data === 'string') return; // control text messages are not used here
    const view = new DataView(event.data);
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
    if (format !== 1 || chunkCount === 0 || chunkIndex >= chunkCount) return;
    if (chunkLen > view.byteLength - CHUNK_HEADER) return;

    let entry = assembler.get(seq);
    if (!entry || entry.totalLen !== totalLen || entry.chunkCount !== chunkCount) {
        entry = { totalLen, chunkCount, chunks: new Array(chunkCount), bytes: 0, tsUs, width, height };
        assembler.set(seq, entry);
    }
    if (!entry.chunks[chunkIndex]) {
        entry.chunks[chunkIndex] = new Uint8Array(view.buffer, view.byteOffset + CHUNK_HEADER, chunkLen).slice();
        entry.bytes += chunkLen;
    }
    if (entry.bytes < entry.totalLen) return;

    assembler.delete(seq);
    const jpeg = new Uint8Array(entry.bytes);
    let offset = 0;
    for (const chunk of entry.chunks) {
        jpeg.set(chunk, offset);
        offset += chunk.length;
    }

    const hasJpegSoi = jpeg[0] === 0xff && jpeg[1] === 0xd8;
    const hasJpegEoi = jpeg[jpeg.length - 2] === 0xff && jpeg[jpeg.length - 1] === 0xd9;

    const now = performance.now();
    if (firstFrameAt === 0) {
        firstFrameAt = now;
    } else {
        intervals.push(now - lastFrameAt);
    }
    lastFrameAt = now;
    totalPayload += jpeg.length;
    frames.push({
        seq,
        bytes: jpeg.length,
        width,
        height,
        chunks: chunkCount,
        hasJpegSoi,
        hasJpegEoi,
        serverTsUs: Number(tsUs),
        clientNowUs: Date.now() * 1000,
    });

    if (frames.length >= targetFrames) {
        finish(0);
    }
};

function finish(code) {
    if (done) return;
    done = true;
    clearTimeout(timeout);
    try {
        socket.close();
    } catch {
        // ignore
    }

    const elapsedMs = lastFrameAt - firstFrameAt;
    const elapsedSeconds = elapsedMs / 1000;
    const bytes = frames.reduce((sum, f) => sum + f.bytes, 0);
    const avgBytes = frames.length ? bytes / frames.length : 0;
    const mbps = elapsedSeconds > 0 ? (bytes * 8) / 1e6 / elapsedSeconds : 0;
    const avgIntervalMs = intervals.length
        ? intervals.reduce((a, b) => a + b, 0) / intervals.length
        : 0;
    const jitterMs = intervals.length
        ? Math.sqrt(intervals.reduce((a, b) => a + (b - avgIntervalMs) ** 2, 0) / intervals.length)
        : 0;
    const fps = elapsedSeconds > 0 ? (frames.length - 1) / elapsedSeconds : 0;
    const first = frames[0];
    const last = frames[frames.length - 1];
    const latencyMs = frames.length
        ? frames.reduce((sum, f) => sum + (f.clientNowUs - f.serverTsUs) / 1000, 0) / frames.length
        : 0;
    const allJpegValid = frames.every((f) => f.hasJpegSoi && f.hasJpegEoi);

    const report = {
        url,
        frames: frames.length,
        expected_frames: targetFrames,
        resolution: first ? `${first.width}x${first.height}` : null,
        chunks_per_frame: first ? first.chunks : null,
        avg_frame_bytes: Math.round(avgBytes),
        avg_frame_kb: Number((avgBytes / 1024).toFixed(2)),
        total_bytes: bytes,
        elapsed_seconds: Number(elapsedSeconds.toFixed(3)),
        avg_frame_interval_ms: Number(avgIntervalMs.toFixed(2)),
        frame_interval_jitter_ms: Number(jitterMs.toFixed(2)),
        achieved_fps: Number(fps.toFixed(2)),
        bitrate_mbps: Number(mbps.toFixed(2)),
        jpeg_frames_valid: allJpegValid,
        first_seq: first ? first.seq : null,
        last_seq: last ? last.seq : null,
        avg_server_to_client_latency_ms: Number(latencyMs.toFixed(2)),
    };

    console.log('\n--- ScreenStream WS probe report ---');
    console.log(JSON.stringify(report, null, 2));
    console.log('---');
    console.log(
        `frames=${report.frames} avg=${report.avg_frame_kb} KB interval=${report.avg_frame_interval_ms} ms ` +
            `fps=${report.achieved_fps} bitrate=${report.bitrate_mbps} Mbps valid_jpeg=${allJpegValid}`
    );
    process.exit(code);
}
