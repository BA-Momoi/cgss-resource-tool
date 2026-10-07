const assert = require('assert/strict');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

for (const file of ['spine-core.js', 'spine-canvas.js', 'cgss_skel_parser.js'])
  vm.runInThisContext(fs.readFileSync(path.join(__dirname, '..', file), 'utf8'));
const root = path.resolve(process.argv[2] || path.join(__dirname, '../../release/CGSS_ResourceTool/CGSS_DOWN'));
function parse(buffer) {
  return CGSSSkelParser.parse(buffer.buffer.slice(buffer.byteOffset, buffer.byteOffset + buffer.byteLength));
}
function close(actual, expected, location) {
  if (location.endsWith('.curve') && actual == null && expected == null) return;
  if (typeof actual === 'number' && typeof expected === 'number') {
    assert(Math.abs(actual - expected) < 0.00003, `${location}: ${actual} != ${expected}`);
  } else if (actual && expected && typeof actual === 'object' && typeof expected === 'object') {
    for (const key of new Set([...Object.keys(actual), ...Object.keys(expected)])) close(actual[key], expected[key], `${location}.${key}`);
  } else assert.deepEqual(actual, expected, location);
}

let count = 0, header;
for (const directory of fs.readdirSync(root)) {
  const dir = path.join(root, directory, 'Spine');
  if (!fs.existsSync(dir)) continue;
  const atlasName = fs.readdirSync(dir).find(name => /^SPC\d+\.atlas$/.test(name));
  if (!atlasName) continue;
  const atlasText = fs.readFileSync(path.join(dir, atlasName), 'utf8');
  const size = /size:\s*(\d+)\s*,\s*(\d+)/.exec(atlasText);
  const atlas = new spine.TextureAtlas(atlasText, () => new spine.FakeTexture({ width: +size[1], height: +size[2] }));
  const reader = new spine.SkeletonJson(new spine.AtlasAttachmentLoader(atlas));
  for (const name of ['N', 's']) {
    const binary = fs.readFileSync(path.join(dir, `SPSprachen_${name}.skel.asset`));
    header = binary.subarray(0, 44);
    const json = parse(binary);
    close(json, JSON.parse(fs.readFileSync(path.join(dir, `SPSprachen_${name}.json`), 'utf8')), `${directory}/${name}`);
    const data = reader.readSkeletonData(JSON.stringify(json));
    const skeleton = new spine.Skeleton(data);
    const state = new spine.AnimationState(new spine.AnimationStateData(data));
    for (const animation of data.animations) {
      state.setAnimation(0, animation.name, true);
      for (let step = 0; step < 30; step++) {
        state.update(animation.duration / 30);
        state.apply(skeleton);
        skeleton.updateWorldTransform();
        for (const bone of skeleton.bones) assert(Number.isFinite(bone.worldX) && Number.isFinite(bone.worldY));
      }
    }
    assert.throws(() => parse(binary.subarray(0, binary.length - 20)));
    count++;
  }
}
assert(count > 0, 'No extracted Spine 2.1 fixtures');

// Values that become 1 after petit scaling must survive default-value elision.
const floats = Buffer.alloc(24);
[2, 2, 1, 1, 1, 2].forEach((value, index) => floats.writeFloatBE(value, index * 4));
const minimal = Buffer.concat([header, Buffer.from([1, 5, 114, 111, 111, 116, 0]), floats,
  Buffer.from([0, 0, 1, 1, 0, 0, 0, 0, 0, 0])]);
assert.deepEqual(parse(minimal).bones, [{ name: 'root', x: 1, y: 1, rotation: 1, length: 1 }]);
assert.throws(() => parse(Buffer.alloc(0)), /header/);
assert.throws(() => parse(Buffer.from([0x1c])), /header/);
assert.throws(() => parse(minimal.subarray(0, 45)), /EOF/);
const unknown = Buffer.from(minimal);
unknown.write('9.9.99', 29, 'ascii');
assert.throws(() => parse(unknown), /Unsupported Spine binary version/);
console.log(`BINARY-COMPAT PASS: ${count} real skeletons, all animations, scaled unit values and invalid inputs`);
