#!/usr/bin/env node
// Mock Glint payload for testing the desktop app without a console.
//
// Serves the payload HTTP API (docs/API.md) from a recording, in real time:
//   GET  /config.json, /status.json
//   GET  /frames   [BE u32 len][BE u32 pts 90 kHz][Annex-B access unit] ...
//   GET  /audio    [BE u32 len][BE u32 pts][pcm_f32le 2ch 48 kHz] ...
//   POST /video/keyframe, /record/start|stop, /capture/profile, /stop -> 200
//
// Make the inputs from any core recording (H.264 + AAC MKV):
//   ffmpeg -i rec.mkv -map 0:v -c copy -bsf:v h264_mp4toannexb -f h264 video.h264
//   ffmpeg -i rec.mkv -map 0:a -f f32le -ar 48000 -ac 2 audio.f32
// Run:  node tests/mock_payload.js <video.h264> <audio.f32> [port=19999] [fps=60]
// The streams loop. New /frames viewers start at a keyframe, like the payload.
'use strict';
const fs = require('fs');
const http = require('http');

const [videoPath, audioPath] = process.argv.slice(2, 4);
const port = +(process.argv[4] || 19999);
const fps = +(process.argv[5] || 60);
if (!videoPath || !audioPath) {
  console.error('usage: node tests/mock_payload.js <video.h264> <audio.f32> [port] [fps]');
  process.exit(2);
}

// --- split Annex-B into access units -------------------------------------------
function nalUnits(buf) {
  const out = [];
  let i = 0, start = -1;
  while (i + 3 <= buf.length) {
    if (buf[i] === 0 && buf[i + 1] === 0 && buf[i + 2] === 1) {
      if (start >= 0) {
        let end = i;
        if (end > start && buf[end - 1] === 0) end--;   // part of a 4-byte start code
        out.push(buf.subarray(start, end));
      }
      start = i + 3;
      i += 3;
    } else {
      i++;
    }
  }
  if (start >= 0) out.push(buf.subarray(start));
  return out;
}

const startCode = Buffer.from([0, 0, 0, 1]);
const units = [];      // { data: Buffer, idr: bool }
let current = [], hasSlice = false, isIdr = false;
let sps = null, pps = null;
for (const nal of nalUnits(fs.readFileSync(videoPath))) {
  const type = nal[0] & 0x1f;
  const firstMb = type >= 1 && type <= 5 && (nal[1] & 0x80) !== 0;   // first_mb_in_slice == 0
  const opensAu = (type === 7 || type === 8 || type === 6 || type === 9) || firstMb;
  if (opensAu && hasSlice) {
    units.push({ data: Buffer.concat(current), idr: isIdr });
    current = []; hasSlice = false; isIdr = false;
  }
  if (type === 7 && !sps) sps = Buffer.from(nal);
  if (type === 8 && !pps) pps = Buffer.from(nal);
  if (type === 7 || type === 8) continue;   // clients inject SPS/PPS from config.json, as the payload does
  current.push(startCode, nal);
  if (type >= 1 && type <= 5) hasSlice = true;
  if (type === 5) isIdr = true;
}
if (hasSlice) units.push({ data: Buffer.concat(current), idr: isIdr });
if (!sps || !pps || !units.length) {
  console.error('no SPS/PPS or frames found in', videoPath);
  process.exit(2);
}
const audio = fs.readFileSync(audioPath);
const chunkBytes = 1024 * 2 * 4;   // 1024 stereo float frames, as the console sends
console.log(`mock payload: ${units.length} frames (${units.filter(u => u.idr).length} IDR), ` +
            `${(audio.length / 8 / 48000).toFixed(1)} s audio, port ${port}`);

// --- shared clock: every viewer sees the same "live" position ----------------------
const t0 = process.hrtime.bigint();
const nowMs = () => Number(process.hrtime.bigint() - t0) / 1e6;
let frameCount = 0, byteCount = 0, audioChunks = 0, audioBytes = 0, clients = 0;

function frame(index, pts) {
  const u = units[index % units.length];
  const head = Buffer.alloc(8);
  head.writeUInt32BE(u.data.length, 0);
  head.writeUInt32BE(pts >>> 0, 4);
  return Buffer.concat([head, u.data]);
}

function serveFrames(res) {
  clients++;
  res.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Cache-Control': 'no-cache' });
  // Start at the latest keyframe at or before the live position, on this
  // viewer's own real-time clock (recordings may hold a single IDR).
  let index = Math.floor(nowMs() * fps / 1000);
  while (index > 0 && !units[index % units.length].idr) index--;
  const first = index, viewerStart = nowMs();
  const timer = setInterval(() => {
    const due = first + Math.floor((nowMs() - viewerStart) * fps / 1000);
    while (index <= due) {
      const pts = Math.round(index * 90000 / fps);
      const buf = frame(index, pts);
      frameCount++; byteCount += buf.length;
      index++;                       // advance first: a full socket buffer must not resend this frame
      if (!res.write(buf)) break;
    }
  }, 4);
  res.on('close', () => { clearInterval(timer); clients--; });
}

function serveAudio(res) {
  clients++;
  res.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Cache-Control': 'no-cache' });
  const per = 1024 / 48000 * 1000;
  let chunk = Math.floor(nowMs() / per);
  const total = Math.floor(audio.length / chunkBytes);
  const timer = setInterval(() => {
    const due = Math.floor(nowMs() / per);
    while (chunk <= due) {
      const at = (chunk % total) * chunkBytes;
      const head = Buffer.alloc(8);
      head.writeUInt32BE(chunkBytes, 0);
      head.writeUInt32BE(Math.round(chunk * 1024 * 90000 / 48000) >>> 0, 4);
      audioChunks++; audioBytes += chunkBytes;
      chunk++;
      if (!res.write(Buffer.concat([head, audio.subarray(at, at + chunkBytes)]))) break;
    }
  }, 5);
  res.on('close', () => { clearInterval(timer); clients--; });
}

const hex = b => b.toString('hex');
const config = () => JSON.stringify({
  platform: 'PS5', width: 1920, height: 1080, fps,
  codec: 'avc1.' + hex(sps.subarray(1, 4)), sps: hex(sps), pps: hex(pps),
  audio: { codec: 'pcm_f32le', channels: 2, rate: 48000, bits: 32 },
  resolutions: [720, 1080, 1440, 2160], requested_height: 1080, profile_changing: false, profile_error: false,
});
const startedAt = Date.now();
const status = () => {
  const s = (Date.now() - startedAt) / 1000;
  return JSON.stringify({
    backend: 'mock', width: 1920, height: 1080, frames: frameCount, bytes: byteCount,
    fps: fps, kbps: s > 0 ? byteCount * 8 / 1000 / s : 0, clients,
    recording: false, recording_name: '', recording_frames: 0, recording_bytes: 0, recording_seconds: 0.0,
    recording_error: false, audio_chunks: audioChunks, audio_bytes: audioBytes,
    audio_kbps: s > 0 ? audioBytes * 8 / 1000 / s : 0, audio_codec: 'pcm_f32le',
  });
};

http.createServer((req, res) => {
  const path = req.url.split('?')[0];
  if (req.method === 'GET' && path === '/config.json') { res.writeHead(200, { 'Content-Type': 'application/json' }); return res.end(config()); }
  if (req.method === 'GET' && path === '/status.json') { res.writeHead(200, { 'Content-Type': 'application/json' }); return res.end(status()); }
  if (req.method === 'GET' && path === '/frames') return serveFrames(res);
  if (req.method === 'GET' && path === '/audio') return serveAudio(res);
  if (req.method === 'POST') { res.writeHead(200, { 'Content-Type': 'application/json' }); return res.end('{"message":"ok (mock)"}'); }
  res.writeHead(404); res.end();
}).listen(port, '127.0.0.1', () => console.log(`listening on http://127.0.0.1:${port}`));
