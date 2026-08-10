// End-to-end: parse JSON -> apply animation -> getBounds -> renderer.draw with a stub 2D context.
const fs = require('fs');
const path = require('path');

eval(fs.readFileSync(path.join(__dirname, '..', 'spine-core.js'), 'utf8'));
eval(fs.readFileSync(path.join(__dirname, '..', 'spine-canvas.js'), 'utf8'));

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
  const jsonFile = fs.readdirSync(skelDir).find(f => f.indexOf(`SP3S301290_${name}.json`) === 0 && f.indexOf('_v38') < 0);
  if (!jsonFile) { console.log(name + ': SKIP'); continue; }
  const p = path.join(skelDir, jsonFile);
  const data = json.readSkeletonData(fs.readFileSync(p, 'utf8'));
  const skeleton = new spine.Skeleton(data);
  const state = new spine.AnimationState(new spine.AnimationStateData(data));
  if (data.animations.length) state.setAnimation(0, data.animations[0].name, true);
  skeleton.setToSetupPose();
  skeleton.updateWorldTransform();
  const off = new spine.Vector2(), size = new spine.Vector2();
  skeleton.getBounds(off, size, []);
  console.log(`${name}: bounds x=${off.x.toFixed(1)} y=${off.y.toFixed(1)} w=${size.x.toFixed(1)} h=${size.y.toFixed(1)}`);
  for (let i = 0; i < 60; i++) {
    state.update(0.016);
    state.apply(skeleton);
    skeleton.updateWorldTransform();
    renderer.draw(skeleton);
  }
  console.log(`${name}: render OK`);
}
console.log('E2E PASS');
