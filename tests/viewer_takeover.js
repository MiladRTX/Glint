#!/usr/bin/env node
// One viewer at a time: a new viewer ends the old one's streams, the old one is
// refused (409) while the new one watches, and take=1 gets it back.
// Usage: node viewer_takeover.js http://127.0.0.1:19099
const http = require('http');
const base = new URL(process.argv[2] || 'http://127.0.0.1:19099');

function open(path) {
  return new Promise((resolve, reject) => {
    const req = http.get({ host: base.hostname, port: base.port, path }, res => {
      const s = { status: res.statusCode, bytes: 0, ended: false, res };
      res.on('data', d => { s.bytes += d.length; });
      res.on('end', () => { s.ended = true; });
      res.on('close', () => { s.ended = true; });
      resolve(s);
    });
    req.on('error', reject);
  });
}
const wait = ms => new Promise(r => setTimeout(r, ms));
function assert(ok, what) { if (!ok) { console.error('FAIL ' + what); process.exit(1); } console.log('ok ' + what); }

(async () => {
  const a = await open('/frames?viewer=aaa'), aAudio = await open('/audio?viewer=aaa');
  await wait(800);
  assert(a.status === 200 && a.bytes > 0 && aAudio.status === 200, 'viewer A streams video and audio');

  const b = await open('/frames?viewer=bbb');
  await wait(1500);
  assert(b.status === 200 && b.bytes > 0, 'viewer B streams');
  assert(a.ended && aAudio.ended, 'viewer A was kicked (video and audio)');

  const a2 = await open('/frames?viewer=aaa');
  assert(a2.status === 409, 'A reconnecting on its own is refused while B watches');

  const app = await open('/frames');
  await wait(1500);
  assert(app.status === 200 && app.bytes > 0 && b.ended, 'a client without an id (the app) takes over from B');
  app.res.destroy();

  const a3 = await open('/frames?viewer=aaa&take=1');
  await wait(800);
  assert(a3.status === 200 && a3.bytes > 0, 'A takes it back with take=1');
  a3.res.destroy();
  process.exit(0);
})().catch(e => { console.error('FAIL ' + e.message); process.exit(1); });
