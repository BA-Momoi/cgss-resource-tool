// Verify generated JSON loads in the official spine-ts 3.6 runtime.
const fs = require('fs');
const path = require('path');

const dir = process.argv[2] || 'D:\\Curl_test\\CGSS\\build\\CGSS_DOWN';
const runtimeDir = path.join(__dirname, '..');
eval(fs.readFileSync(path.join(runtimeDir, 'spine-core.js'), 'utf8'));
eval(fs.readFileSync(path.join(runtimeDir, 'spine-canvas.js'), 'utf8'));

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

const skelDir = findSkelDir(dir);
if (!skelDir) { console.error('spine folder not found under ' + dir); process.exit(1); }

const atlasPath = fs.existsSync(path.join(skelDir, 'SP3S301290_tex.atlas'))
  ? path.join(skelDir, 'SP3S301290_tex.atlas')
  : path.join(skelDir, 'SP3S301290_tex.atlas.asset');
const atlasText = fs.readFileSync(atlasPath, 'utf8');
const fakeTex = new spine.canvas.CanvasTexture({ width: 1, height: 1, __canvasFake: true });
const atlas = new spine.TextureAtlas(atlasText, () => fakeTex);
const attLoader = new spine.AtlasAttachmentLoader(atlas);
const json = new spine.SkeletonJson(attLoader);

const files = ['chara', 'bg', 'eff1', 'eff2', 'fg'];
let ok = 0;
for (const name of files) {
  const jsonFile = fs.readdirSync(skelDir).find(f => f.indexOf(`SP3S301290_${name}.json`) === 0 && f.indexOf('_v38') < 0);
  if (!jsonFile) { console.log(`${name}: SKIP (no json)`); continue; }
  try {
    const text = fs.readFileSync(path.join(skelDir, jsonFile), 'utf8');
    const data = json.readSkeletonData(text);
    const skeleton = new spine.Skeleton(data);
    const stateData = new spine.AnimationStateData(data);
    const state = new spine.AnimationState(stateData);
    const anim = data.animations[0];
    if (!anim) throw new Error('no animations');
    state.setAnimation(0, anim.name, true);
    for (let i = 0; i < 3; i++) {
      state.update(0.016);
      state.apply(skeleton);
      skeleton.updateWorldTransform();
    }
    console.log(`${name}: OK  bones=${data.bones.length} slots=${data.slots.length} anims=${data.animations.map(a => a.name).join(',')}`);
    ok++;
  } catch (e) {
    console.log(`${name}: FAIL ${e.message}`);
  }
}
console.log(ok === files.filter(n => fs.readdirSync(skelDir).some(f => f.indexOf(`SP3S301290_${n}.json`) === 0 && f.indexOf('_v38') < 0)).length ? 'ALL PASS' : 'SOME FAILED');
