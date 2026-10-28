// End-to-end protocol smoke test against glint-payload (host build).
// Usage: node payload_smoke.js http://127.0.0.1:19099
const fs = require('fs');
const http = require('http');
const path = require('path');

// The payload serves these byte for byte (embedded by tools/gen_blobs.js).
const assets = path.join(__dirname, '..', 'payload', 'assets');
const dashboard = fs.readFileSync(path.join(assets, 'dashboard.html'));
const bannerPng = fs.readFileSync(path.join(assets, 'banner.png'));

const base = process.argv[2] || 'http://127.0.0.1:19099';
const url = new URL(base);

function request(path, options = {}) {
  return new Promise((resolve, reject) => {
    const req = http.request({
      host: url.hostname,
      port: url.port,
      path,
      method: options.method || 'GET',
      headers: options.headers || {},
      timeout: options.timeout || 5000,
    }, res => {
      const chunks = [];
      res.on('data', c => chunks.push(c));
      res.on('end', () => resolve({
        status: res.statusCode,
        headers: res.headers,
        body: Buffer.concat(chunks),
      }));
    });
    req.on('timeout', () => req.destroy(new Error('timeout ' + path)));
    req.on('error', reject);
    req.end(options.body);
  });
}

function assert(cond, message) {
  if (!cond) throw new Error('ASSERT: ' + message);
}

function be32(buf, off) {
  return buf.readUInt32BE(off);
}

function readStream(path, bytes, timeoutMs) {
  return new Promise((resolve, reject) => {
    const req = http.get({ host: url.hostname, port: url.port, path, timeout: timeoutMs }, res => {
      const chunks = [];
      let total = 0;
      res.on('data', c => {
        chunks.push(c);
        total += c.length;
        if (total >= bytes) {
          req.destroy();
          resolve({ status: res.statusCode, headers: res.headers, body: Buffer.concat(chunks) });
        }
      });
      res.on('end', () => resolve({ status: res.statusCode, headers: res.headers, body: Buffer.concat(chunks) }));
      res.on('error', e => { if (total >= bytes) resolve({ status: res.statusCode, headers: res.headers, body: Buffer.concat(chunks) }); else reject(e); });
    });
    req.on('timeout', () => { req.destroy(); resolve({ status: 0, body: Buffer.concat([]) }); });
    req.on('error', e => { if (e.code === 'ECONNRESET') resolve({ status: 0, body: Buffer.alloc(0) }); else reject(e); });
  });
}

const sleep = ms => new Promise(r => setTimeout(r, ms));

(async () => {
  const index = await request('/');
  assert(index.status === 200, 'index status');
  assert(index.body.equals(dashboard), 'index differs from payload/assets/payload-dashboard.html');
  assert(index.body.subarray(0, 15).toString() === '<!doctype html>', 'index doctype');
  console.log(`ok index ${index.body.length} bytes`);

  const banner = await request('/banner.png');
  assert(banner.status === 200, 'banner status');
  assert(banner.body.equals(bannerPng), 'banner differs from payload/assets/payload-banner.png');
  assert(banner.body[0] === 0x89 && banner.body[1] === 0x50, 'banner png');
  console.log(`ok banner ${banner.body.length} bytes`);

  await sleep(500);
  const config = JSON.parse((await request('/config.json')).body.toString());
  assert(config.platform === 'PS5', 'platform');
  assert(config.width === 1920 && config.height === 1080, 'default resolution 1080p');
  assert(config.fps === 60, 'fps');
  assert(config.codec.startsWith('avc1.'), 'codec');
  assert(config.sps.length > 8 && config.pps.length > 4, 'sps/pps hex');
  assert(JSON.stringify(config.resolutions) === '[1080]', 'resolutions 1080p only');
  assert(config.audio && config.audio.codec === 'pcm_f32le' && config.audio.rate === 48000, 'audio config');
  console.log('ok config', config.width + 'x' + config.height, config.codec);

  const status = JSON.parse((await request('/status.json')).body.toString());
  assert(status.backend === 'h264', 'backend');
  assert(status.frames > 0, 'frames advancing');
  assert(status.audio_chunks > 0, 'audio chunks');
  console.log('ok status frames=' + status.frames + ' audio=' + status.audio_chunks);

  const frames = await readStream('/frames', 20000, 5000);
  assert(frames.status === 200, 'frames status');
  assert(frames.body.length >= 20, 'frames body');
  const size = be32(frames.body, 0);
  const pts = be32(frames.body, 4);
  assert(size > 4 && size <= 2097152, 'frame size ' + size);
  assert(frames.body[8] === 0 && frames.body[9] === 0 && frames.body[10] === 0 && frames.body[11] === 1,
    'annex-b start code');
  console.log('ok frames size=' + size + ' pts=' + pts);

  const audio = await readStream('/audio', 8000, 5000);
  assert(audio.status === 200, 'audio status');
  const asize = be32(audio.body, 0);
  assert(asize > 0 && asize <= 65536, 'audio packet size');
  console.log('ok audio packet=' + asize);

  const key = await request('/video/keyframe', { method: 'POST' });
  assert(key.status === 200 && key.body.toString() === '{}', 'keyframe');

  const start = await request('/record/start', { method: 'POST' });
  assert(start.status === 200, 'record start');
  await sleep(1200);
  const recStatus = JSON.parse((await request('/status.json')).body.toString());
  assert(recStatus.recording === true, 'recording active');
  assert(recStatus.recording_name.endsWith('.h264'), 'recording name');

  const stop = await request('/record/stop', { method: 'POST' });
  assert(stop.status === 200, 'record stop');
  const list = JSON.parse((await request('/recordings')).body.toString());
  const entry = list.find(x => x.name === recStatus.recording_name);
  assert(entry && entry.bytes > 0, 'recording listed');
  const download = await request('/recordings/' + encodeURIComponent(entry.name));
  assert(download.status === 200, 'download status');
  assert(download.body.length === entry.bytes, 'download length ' + download.body.length + '/' + entry.bytes);
  assert(download.headers['content-type'] === 'video/h264', 'download content type');
  assert(download.body.readUInt32BE(0) > 0, 'recording starts with SPS length');
  console.log('ok recording', entry.name, entry.bytes + ' bytes');

  const profile = await request('/capture/profile?height=1080', { method: 'POST' });
  assert(profile.status === 200, 'profile status');
  await sleep(800);
  const config2 = JSON.parse((await request('/config.json')).body.toString());
  assert(config2.height === 1080 && config2.width === 1920, 'profile applied ' + config2.width + 'x' + config2.height);
  assert(config2.profile_changing === false, 'profile settled');
  console.log('ok profile 1920x1080');

  const noProfile = await request('/capture/profile?height=1440', { method: 'POST' });
  assert(noProfile.status === 400, '1440p rejected (1080p only)');

  const badProfile = await request('/capture/profile?height=1234', { method: 'POST' });
  assert(badProfile.status === 400, 'bad profile rejected');

  const restart = await request('/capture/restart', { method: 'POST' });
  assert(restart.status === 200, 'restart');

  const unavailable = await request('/nope');
  assert(unavailable.status === 200 && unavailable.body.toString() === 'Unavailable route',
    'unknown route body');

  const stopStream = await request('/stop', { method: 'POST' });
  assert(stopStream.status === 200, 'stop');
  console.log('ok stop');

  console.log('PASS payload protocol smoke test');
})().catch(err => {
  console.error('FAIL', err.message);
  process.exit(1);
});
