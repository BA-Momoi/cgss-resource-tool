// Full browser-path test: .skel binary -> CGSSSkelParser -> spine runtime -> render.
const fs = require('fs');
const path = require('path');
eval(fs.readFileSync(path.join(__dirname, '..', 'spine-core.js'), 'utf8'));
eval(fs.readFileSync(path.join(__dirname, '..', 'spine-canvas.js'), 'utf8'));
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

const atlasPath = fs.existsSync(path.join(skelDir, 'SP3S301290_tex.atlas'))
  ? path.join(skelDir, 'SP3S301290_tex.atlas')
  : path.join(skelDir, 'SP3S301290_tex.atlas.asset');
const atlasText = fs.readFileSync(atlasPath, 'utf8');
const fakeImg = { width: 2001, height: 882 };
const atlas = new spine.TextureAtlas(atlasText, () => new spine.canvas.CanvasTexture(fakeImg));
const json = new spine.SkeletonJson(new spine.AtlasAttachmentLoader(atlas));

const ctxStub = new Proxy({}, {
  get(t, p) {
    if (['save','restore','beginPath','closePath','clip','moveTo','lineTo','transform',
         'translate','rotate','scale','setTransform','clearRect','stroke'].includes(p)) return () => {};
    return t[p];
  },
  set(t, p, v) { t[p] = v; return true; }
});
ctxStub.drawImage = () => {};
const renderer = new spine.canvas.SkeletonRenderer(ctxStub);
renderer.triangleRendering = true;

const names = ['chara', 'bg', 'eff1', 'eff2', 'fg'];
for (const name of names) {
  const skelFile = fs.readdirSync(skelDir).find(f => f.indexOf(`SP3S301290_${name}.skel`) === 0);
  if (!skelFile) { console.log(name + ': SKIP'); continue; }
  const buf = fs.readFileSync(path.join(skelDir, skelFile));
  const ab = buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
  const jsonObj = CGSSSkelParser.parse(ab);
  const data = json.readSkeletonData(JSON.stringify(jsonObj));
  const skeleton = new spine.Skeleton(data);
  const state = new spine.AnimationState(new spine.AnimationStateData(data));
  state.setAnimation(0, data.animations[0].name, true);
  skeleton.setToSetupPose();
  skeleton.updateWorldTransform();
  const off = new spine.Vector2(), size = new spine.Vector2();
  skeleton.getBounds(off, size, []);
  for (let i = 0; i < 90; i++) {
    state.update(0.016);
    state.apply(skeleton);
    skeleton.updateWorldTransform();
    renderer.draw(skeleton);
  }
  console.log(`${name}: parse+load+animate+render OK (bounds ${off.x.toFixed(0)},${off.y.toFixed(0)} ${size.x.toFixed(0)}x${size.y.toFixed(0)})`);
}
console.log('E2E-SKEL PASS');
