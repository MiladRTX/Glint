// Runs the app's probe (app/tools/probe.cpp) against the host payload server and checks the artifacts.
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const root = path.resolve(__dirname, '..');
const exe = path.join(root, 'payload', 'build', process.platform === 'win32' ? 'glint-payload.exe' : 'glint-payload');
const probe = path.join(root, 'app', 'build', process.platform === 'win32' ? 'probe.exe' : 'probe');
const port = process.argv[2] || '19100';
const dataDir = path.join(os.tmpdir(), 'glint-probe-data');
const videoFile = path.join(dataDir, 'probe.h264');
const audioFile = path.join(dataDir, 'probe.pcm');

fs.mkdirSync(dataDir, { recursive: true });
try { fs.unlinkSync(videoFile); } catch {}
try { fs.unlinkSync(audioFile); } catch {}

const extraPath = process.platform === 'win32' ? 'C:/msys64/mingw64/bin;' : '';
const server = spawn(exe, [port], {
  cwd: root,
  env: { ...process.env, GLINT_DATA_DIR: dataDir, PATH: extraPath + (process.env.PATH || '') },
  stdio: ['ignore', 'pipe', 'pipe'],
  windowsHide: true,
});
server.stdout.on('data', d => process.stdout.write('[server] ' + d));
server.stderr.on('data', d => process.stderr.write('[server] ' + d));

const wait = ms => new Promise(r => setTimeout(r, ms));

(async () => {
  await wait(1000);
  const child = spawn(probe, [
    'http://127.0.0.1:' + port,
    '--record', videoFile,
    '--audio', audioFile,
    '--seconds', '2',
  ], { stdio: 'inherit', env: { ...process.env, PATH: extraPath + (process.env.PATH || '') } });

  child.on('exit', code => {
    let ok = code === 0;
    try {
      const video = fs.readFileSync(videoFile);
      const audio = fs.readFileSync(audioFile);
      console.log('video bytes', video.length, 'audio bytes', audio.length);
      const starts = video.length > 4 && video[0] === 0 && video[1] === 0 && video[2] === 0 && video[3] === 1;
      if (!starts) { console.error('video does not start with an Annex-B start code'); ok = false; }
      if (video.length < 1000) { console.error('video too small'); ok = false; }
      if (audio.length < 1000) { console.error('audio too small'); ok = false; }
    } catch (e) {
      console.error('artifact check failed:', e.message);
      ok = false;
    }
    if (server.exitCode === null) server.kill();
    console.log(ok ? 'PASS probe' : 'FAIL probe');
    process.exit(ok ? 0 : 1);
  });
})();

