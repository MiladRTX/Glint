// Spawns the host server, runs payload_smoke.js against it, then cleans up.
const { spawn } = require('child_process');
const fs = require('fs');
const path = require('path');
const os = require('os');

const root = path.resolve(__dirname, '..');
const exe = path.join(root, 'payload', 'build', process.platform === 'win32' ? 'glint-payload.exe' : 'glint-payload');
const port = process.argv[2] || '19099';
const dataDir = process.argv[3] || path.join(os.tmpdir(), 'glint-host-data');

if (!fs.existsSync(dataDir)) fs.mkdirSync(dataDir, { recursive: true });

const server = spawn(exe, [port], {
  cwd: root,
  env: { ...process.env, GLINT_DATA_DIR: dataDir },
  stdio: ['ignore', 'pipe', 'pipe'],
  windowsHide: true,
});

let serverLog = '';
server.stdout.on('data', d => { serverLog += d.toString(); process.stdout.write('[server] ' + d.toString()); });
server.stderr.on('data', d => { serverLog += d.toString(); process.stderr.write('[server] ' + d.toString()); });
server.on('exit', code => { if (code !== null && code !== 0) console.error('[server] exited ' + code); });

function wait(ms) { return new Promise(r => setTimeout(r, ms)); }

(async () => {
  await wait(1200);
  if (server.exitCode !== null) {
    console.error('server exited before test, code ' + server.exitCode);
    process.exit(1);
  }

  const test = spawn(process.execPath, [path.join(__dirname, 'payload_smoke.js'), 'http://127.0.0.1:' + port], {
    stdio: 'inherit',
  });

  test.on('exit', code => {
    if (server.exitCode === null) {
      server.kill();
      setTimeout(() => { if (server.exitCode === null) server.kill('SIGKILL'); }, 1000);
    }
    process.exit(code === null ? 1 : code);
  });
})();
