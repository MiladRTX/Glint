// Embeds the payload's web assets (payload/assets) as C arrays in payload/src.
// Run after changing payload-dashboard.html or payload-banner.png:
//   node tools/gen_blobs.js
const fs = require('fs');
const path = require('path');

function emit(srcPath, outC, outH, symbol) {
  const data = fs.readFileSync(srcPath);
  let body = '';
  for (let i = 0; i < data.length; i++) {
    if (i % 16 === 0) body += '\n    ';
    body += '0x' + data[i].toString(16).padStart(2, '0') + ',';
  }
  fs.writeFileSync(outC,
`#include "${path.basename(outH)}"\n\nconst unsigned char ${symbol}[] = {${body}\n};\nconst size_t ${symbol}_len = ${data.length};\n`);
  fs.writeFileSync(outH,
`#ifndef ${symbol.toUpperCase()}_H\n#define ${symbol.toUpperCase()}_H\n#include <stddef.h>\nextern const unsigned char ${symbol}[];\nextern const size_t ${symbol}_len;\n#endif\n`);
  console.log(path.relative(root, outC), data.length);
}

const root = path.resolve(__dirname, '..');
const payload = path.join(root, 'payload');
emit(path.join(payload, 'assets/dashboard.html'), path.join(payload, 'src/dashboard_blob.c'),
     path.join(payload, 'src/dashboard_blob.h'), 'SL_DASHBOARD_HTML');
emit(path.join(payload, 'assets/banner.png'), path.join(payload, 'src/banner_blob.c'),
     path.join(payload, 'src/banner_blob.h'), 'SL_BANNER_PNG');
