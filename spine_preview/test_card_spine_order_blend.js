// 验证: 5 个 skel 按 bg->eff2->chara->eff1->fg 排序, 以及 additive/multiply 混合模式切换。
const fs = require('fs');
const path = require('path');

eval(fs.readFileSync(path.join(__dirname, '..', 'spine-core.js'), 'utf8'));
eval(fs.readFileSync(path.join(__dirname, '..', 'spine-canvas.js'), 'utf8'));
eval(fs.readFileSync(path.join(__dirname, '..', 'cgss_skel_parser.js'), 'utf8'));

const r009 = 'D:\\Curl_test\\CGSS\\build\\AssetStudio_out\\r009';
const texDir = path.join(r009, 'TextAsset');
const imgDir = path.join(r009, 'Texture2D');

// --- 排序逻辑(与 preview.html finishSkelLoad 一致) ---
function priority(name) {
  const s = name.toLowerCase();
  if (s.indexOf('bg') >= 0) return 0;
  if (s.indexOf('eff2') >= 0) return 1;
  if (s.indexOf('chara') >= 0) return 2;
  if (s.indexOf('eff1') >= 0) return 3;
  if (s.indexOf('fg') >= 0) return 4;
  return 5;
}
const names = ['SP3S301290_chara.skel.asset', 'SP3S301290_eff2.skel.asset',
               'SP3S301290_bg.skel.asset', 'SP3S301290_fg.skel.asset',
               'SP3S301290_eff1.skel.asset'];
const sorted = names.slice().sort((a, b) => {
  const pa = priority(a), pb = priority(b);
  return pa !== pb ? pa - pb : (a < b ? -1 : 1);
});
const expected = ['SP3S301290_bg.skel.asset', 'SP3S301290_eff2.skel.asset',
                  'SP3S301290_chara.skel.asset', 'SP3S301290_eff1.skel.asset',
                  'SP3S301290_fg.skel.asset'];
if (JSON.stringify(sorted) !== JSON.stringify(expected)) {
  console.error('ORDER FAIL', sorted);
  process.exit(1);
}
console.log('ORDER OK:', sorted.join(' '));

// --- atlas: .atlas.asset 即纯文本 atlas ---
const atlasText = fs.readFileSync(path.join(texDir, 'SP3S301290_tex.atlas.asset'), 'utf8');
const fakeImg = { width: 2001, height: 882 };
const atlas = new spine.TextureAtlas(atlasText, () => new spine.canvas.CanvasTexture(fakeImg));
const json = new spine.SkeletonJson(new spine.AtlasAttachmentLoader(atlas));

// --- 记录混合模式切换的 ctx stub ---
const ops = [];
const ctxStub = new Proxy({}, {
  get(t, p) {
    if (['save','restore','beginPath','closePath','clip','moveTo','lineTo','transform',
         'translate','rotate','scale','setTransform','clearRect','stroke'].includes(p)) return () => {};
    if (p === 'drawImage') return () => {};
    if (p === 'globalCompositeOperation') return t[p] || 'source-over';
    return t[p];
  },
  set(t, p, v) {
    if (p === 'globalCompositeOperation' && t[p] !== v) {
      ops.push(v);
      t[p] = v;
    }
    return true;
  }
});
const renderer = new spine.canvas.SkeletonRenderer(ctxStub);
renderer.triangleRendering = true;

for (const name of sorted) {
  const buf = fs.readFileSync(path.join(texDir, name));
  const ab = buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
  const skelJson = CGSSSkelParser.parse(ab);
  const data = json.readSkeletonData(JSON.stringify(skelJson));
  const skeleton = new spine.Skeleton(data);
  const state = new spine.AnimationState(new spine.AnimationStateData(data));
  if (data.animations.length) state.setAnimation(0, data.animations[0].name, true);
  skeleton.setToSetupPose();
  skeleton.updateWorldTransform();
  for (let i = 0; i < 30; i++) {
    state.update(0.016);
    state.apply(skeleton);
    skeleton.updateWorldTransform();
    renderer.draw(skeleton);
  }
  console.log(`${name}: load+animate+draw OK (anims=${data.animations.length})`);
}

// --- 验证混合模式确实发生过切换 ---
if (ops.length === 0) {
  console.error('BLEND FAIL: no blend mode changes observed');
  process.exit(1);
}
console.log('BLEND OK, transitions seen:', ops.join(' -> '));
console.log('E2E-ORDER-BLEND PASS');
