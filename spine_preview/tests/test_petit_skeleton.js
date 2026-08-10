// 组合测试: SPSprachen 共享小人骨架 + card_spine SPC atlas -> 加载/动画/渲染。
const fs = require('fs');
const path = require('path');

eval(fs.readFileSync(path.join(__dirname, '..', 'spine-core.js'), 'utf8'));
eval(fs.readFileSync(path.join(__dirname, '..', 'spine-canvas.js'), 'utf8'));

const base = 'D:\\Curl_test\\CGSS\\build\\_spinetest\\petit_sim';
const skelJson = JSON.parse(fs.readFileSync(path.join(base, 'SPSprachen_s.json'), 'utf8'));
const atlasText = fs.readFileSync(path.join(base, 'SPC100001.atlas.asset'), 'utf8');

// 真实尺寸取自 atlas
const m = /size:\s*(\d+),(\d+)/.exec(atlasText);
const fakeImg = { width: m ? parseInt(m[1]) : 512, height: m ? parseInt(m[2]) : 512 };
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

const data = json.readSkeletonData(JSON.stringify(skelJson));
console.log('bones=' + data.bones.length, 'slots=' + data.slots.length,
            'skins=' + data.skins.length, 'anims=' + data.animations.map(a => a.name).join(','));
const skeleton = new spine.Skeleton(data);
const state = new spine.AnimationState(new spine.AnimationStateData(data));

// 用默认皮肤的每个 slot 检查附件是否都能在 atlas 找到
const skin = data.defaultSkin;
let found = 0, missing = 0;
for (const slot of data.slots) {
  const att = skin.getAttachment(slot.index, slot.attachmentName || slot.name);
  if (att) found++; else missing++;
}
console.log('setup attachments: found=' + found + ' missing=' + missing);

let total = 0;
for (const anim of data.animations) {
  state.setAnimation(0, anim.name, true);
  skeleton.setToSetupPose();
  for (let i = 0; i < 30; i++) {
    state.update(0.016);
    state.apply(skeleton);
    skeleton.updateWorldTransform();
    renderer.draw(skeleton);
  }
  total++;
}
console.log('animated+rendered ' + total + ' animations OK');

// v38 JSON 也加载一遍（3.8 数据格式）
const v38 = JSON.parse(fs.readFileSync(path.join(base, 'SPSprachen_s_v38.json'), 'utf8'));
const data38 = json.readSkeletonData(JSON.stringify(v38));
const skel38 = new spine.Skeleton(data38);
const st38 = new spine.AnimationState(new spine.AnimationStateData(data38));
st38.setAnimation(0, data38.animations[0].name, true);
for (let i = 0; i < 30; i++) {
  st38.update(0.016);
  st38.apply(skel38);
  skel38.updateWorldTransform();
  renderer.draw(skel38);
}
console.log('v38 load+animate+render OK (bones=' + data38.bones.length + ')');
console.log('PETIT-SKELETON PASS');
