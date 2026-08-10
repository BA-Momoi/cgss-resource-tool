// Compare the JS binary parser output with the Python-generated JSON.
const fs = require('fs');
const path = require('path');
eval(fs.readFileSync(path.join(__dirname, '..', 'cgss_skel_parser.js'), 'utf8'));

function findSkelDir(base) {
  for (const d of fs.readdirSync(base, { withFileTypes: true })) {
    if (!d.isDirectory()) continue;
    const cands = [
      path.join(base, d.name, 'live2d'),
      path.join(base, d.name, '卡面Spina动画', 'spine'),
    ];
    for (const c of cands) {
      if (fs.existsSync(path.join(c, 'SP3S301290_tex.atlas'))) return c;
      if (fs.existsSync(path.join(c, 'SP3S301290_tex.atlas.asset'))) return c;
    }
  }
  return null;
}

const base = process.argv[2] || 'D:\\Curl_test\\CGSS\\build\\CGSS_DOWN';
const skelDir = findSkelDir(base);
if (!skelDir) { console.error('spine folder not found under ' + base); process.exit(1); }

let fail = 0;
for (const name of ['chara', 'bg', 'eff1', 'eff2', 'fg']) {
  const skelFile = fs.readdirSync(skelDir).find(f => f.indexOf(`SP3S301290_${name}.skel`) === 0);
  const jsonFile = fs.readdirSync(skelDir).find(f => f.indexOf(`SP3S301290_${name}.json`) === 0 && f.indexOf('_v38') < 0);
  if (!skelFile || !jsonFile) { console.log(`${name}: SKIP`); continue; }
  const buf = fs.readFileSync(path.join(skelDir, skelFile));
  const ab = buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
  const jsOut = CGSSSkelParser.parse(ab);
  const pyOut = JSON.parse(fs.readFileSync(path.join(skelDir, jsonFile), 'utf8'));
  const errs = [];
  compare(jsOut, pyOut, '', errs);
  if (errs.length) {
    fail++;
    console.log(`${name}: ${errs.length} diffs`);
    errs.slice(0, 8).forEach(e => console.log('   ' + e));
  } else {
    console.log(`${name}: identical to python output`);
  }
}
console.log(fail ? 'MISMATCH' : 'ALL IDENTICAL');

function compare(a, b, pathStr, errs) {
  // Linear curves: Python/JS output "curve":null, C converter omits it; runtime-equivalent.
  if (/\.curve$/.test(pathStr) && (a == null || b == null) && a !== b) return;
  if (typeof a === 'number' && typeof b === 'number') {
    if (Math.abs(a - b) > 1e-5) errs.push(`${pathStr}: ${a} != ${b}`);
    return;
  }
  if (Array.isArray(a) || Array.isArray(b)) {
    if (!Array.isArray(a) || !Array.isArray(b) || a.length !== b.length) {
      errs.push(`${pathStr}: array mismatch`); return;
    }
    for (let i = 0; i < a.length; i++) compare(a[i], b[i], `${pathStr}[${i}]`, errs);
    return;
  }
  if (typeof a === 'object' && a !== null && typeof b === 'object' && b !== null) {
    const keys = new Set([...Object.keys(a), ...Object.keys(b)]);
    for (const k of keys) {
      if (k === 'curve' && (!(k in a) || !(k in b))) continue;
      if (!(k in a)) { errs.push(`${pathStr}.${k}: missing in js`); continue; }
      if (!(k in b)) { errs.push(`${pathStr}.${k}: missing in python`); continue; }
      compare(a[k], b[k], `${pathStr}.${k}`, errs);
    }
    return;
  }
  if (a !== b) errs.push(`${pathStr}: ${JSON.stringify(a)} != ${JSON.stringify(b)}`);
}
