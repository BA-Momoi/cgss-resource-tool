// Uses local extracted resources to exercise the real file inputs and canvas.
const assert = require('assert/strict');
const fs = require('fs');
const path = require('path');
const { pathToFileURL } = require('url');
const { chromium } = require('playwright');

const repo = path.resolve(__dirname, '../..');
const root = path.resolve(process.argv[2] || path.join(repo, 'release/CGSS_ResourceTool/CGSS_DOWN'));
const html = path.resolve(process.argv[3] || path.join(repo, 'spine_preview/preview.html'));
const output = process.argv[4] && path.resolve(process.argv[4]);
const petitDirs = fs.readdirSync(root).map(name => path.join(root, name, 'Spine'))
  .filter(dir => fs.existsSync(dir) && fs.readdirSync(dir).some(name => /^SPC\d+\.atlas$/.test(name)));
assert(petitDirs.length > 0, 'No extracted petit resources found');

async function ready(page) {
  await page.waitForFunction(() => previewReady && document.getElementById('msg').textContent.startsWith('加载成功'));
  await page.waitForTimeout(250);
  const pixels = await page.evaluate(() => {
    const cv = document.getElementById('cv');
    const data = cv.getContext('2d').getImageData(0, 0, cv.width, cv.height).data;
    let visible = 0;
    for (let i = 0; i < data.length; i += 4)
      if (data[i + 3] && Math.max(data[i], data[i + 1], data[i + 2]) - Math.min(data[i], data[i + 1], data[i + 2]) > 20) visible++;
    return visible;
  });
  assert(pixels > 1000, `Canvas has only ${pixels} colored pixels`);
  assert.equal(await page.evaluate(() => window.testFrames.size), 1, 'Multiple animation loops');
  return pixels;
}

async function selectPetit(page, dir, skeleton) {
  const atlas = fs.readdirSync(dir).find(name => /^SPC\d+\.atlas$/.test(name));
  await page.locator('#f_skel').setInputFiles(path.join(dir, skeleton));
  await page.locator('#f_atlas').setInputFiles(path.join(dir, atlas));
  await page.locator('#f_png').setInputFiles(path.join(dir, atlas.replace('.atlas', '.png')));
  return ready(page);
}

async function run() {
  const browser = await chromium.launch({ channel: 'chrome', headless: true });
  try {
    const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    await page.addInitScript(() => {
      const request = window.requestAnimationFrame.bind(window);
      const cancel = window.cancelAnimationFrame.bind(window);
      window.testFrames = new Set();
      window.requestAnimationFrame = callback => {
        const id = request(time => { window.testFrames.delete(id); callback(time); });
        window.testFrames.add(id);
        return id;
      };
      window.cancelAnimationFrame = id => { window.testFrames.delete(id); cancel(id); };
    });
    await page.goto(pathToFileURL(html).href);
    const dir = petitDirs[0];
    const atlas = fs.readdirSync(dir).find(name => /^SPC\d+\.atlas$/.test(name));

    // Reproduce the supplied error, then select the PNG to check the error survives.
    await page.locator('#f_skel').setInputFiles(path.join(dir, 'SPSprachen_s.json'));
    await page.locator('#f_atlas').setInputFiles(path.join(dir, 'SPSprachen_s.skel.asset'));
    await page.waitForFunction(() => document.getElementById('msg').textContent.includes('二进制骨架'));
    await page.locator('#f_png').setInputFiles(path.join(dir, atlas.replace('.atlas', '.png')));
    await page.waitForFunction(() => !loading.png);
    assert.match(await page.locator('#msg').textContent(), /二进制骨架/);
    assert.equal(await page.evaluate(() => atlasText), null);
    assert.equal(await page.evaluate(() => previewReady), false);

    await page.locator('#f_atlas').setInputFiles(path.join(dir, atlas));
    await ready(page);
    for (const sample of petitDirs) {
      for (const skeleton of ['SPSprachen_N.json', 'SPSprachen_s.json', 'SPSprachen_N.skel.asset', 'SPSprachen_s.skel.asset']) {
        const pixels = await selectPetit(page, sample, skeleton);
        console.log(`${path.basename(path.dirname(sample))}: ${skeleton}, ${pixels} colored pixels`);
      }
    }
    for (const name of await page.locator('#anim_rows select option').evaluateAll(options => options.map(o => o.value))) {
      await page.locator('#anim_rows select').selectOption(name);
      await page.waitForTimeout(80);
    }
    const times = await page.evaluate(() => [skelList[0].state.tracks[0].trackTime]);
    await page.waitForTimeout(150);
    assert(await page.evaluate(() => skelList[0].state.tracks[0].trackTime) > times[0], 'Animation is not moving');

    await page.locator('#f_atlas').setInputFiles(path.join(dir, 'SPSprachen_s.json'));
    await page.waitForFunction(() => !!loadErrors.atlas);
    assert.match(await page.locator('#msg').textContent(), /不是 Spine 图集/);
    await selectPetit(page, dir, 'SPSprachen_s.json');

    await page.locator('#f_skel').setInputFiles([
      { name: 'SPSprachen_s.json', mimeType: 'application/json', buffer: fs.readFileSync(path.join(dir, 'SPSprachen_s.json')) },
      { name: 'bad.json', mimeType: 'application/json', buffer: Buffer.from('{') }
    ]);
    await page.waitForFunction(() => !!loadErrors.skel);
    assert.equal(await page.evaluate(() => skelList.length), 0, 'A broken skeleton group loaded partially');
    await selectPetit(page, dir, 'SPSprachen_N.json');

    await page.locator('#f_png').setInputFiles({ name: 'broken.png', mimeType: 'image/png', buffer: Buffer.from('broken') });
    await page.waitForFunction(() => !!loadErrors.png);
    assert.match(await page.locator('#msg').textContent(), /贴图无法读取/);
    await selectPetit(page, dir, 'SPSprachen_N.skel.asset');
    await page.locator('#f_png').setInputFiles(path.join(dir, '模板示例(卯月杏)/SPSprachen_s.png'));
    await page.waitForFunction(() => document.getElementById('msg').textContent.startsWith('加载失败'));
    assert.match(await page.locator('#msg').textContent(), /不在已选贴图里/);
    await selectPetit(page, dir, 'SPSprachen_N.skel.asset');
    if (output) {
      fs.mkdirSync(output, { recursive: true });
      await page.screenshot({ path: path.join(output, 'spine-desktop.png'), fullPage: true });
      await page.setViewportSize({ width: 390, height: 844 });
      await ready(page);
      await page.screenshot({ path: path.join(output, 'spine-mobile.png'), fullPage: true });
      assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'Mobile horizontal overflow');
      await page.setViewportSize({ width: 1280, height: 900 });
    }

    const cardRoot = path.join(repo, 'build/CGSS_DOWN');
    const cardDir = fs.existsSync(cardRoot) && fs.readdirSync(cardRoot)
      .map(name => path.join(cardRoot, name, 'Live2D/spine'))
      .find(candidate => fs.existsSync(path.join(candidate, 'SP3S301290_tex.atlas')));
    assert(cardDir, 'No card animation fixture found');
    await page.locator('#f_skel').setInputFiles(fs.readdirSync(cardDir)
      .filter(name => /^SP3S301290_(bg|chara|eff1|eff2|fg)\.skel\.asset$/.test(name))
      .map(name => path.join(cardDir, name)));
    await page.locator('#f_atlas').setInputFiles(path.join(cardDir, 'SP3S301290_tex.atlas'));
    await page.locator('#f_png').setInputFiles(['SP3S301290_tex.png', 'SP3S301290_tex_A8.png'].map(name => path.join(cardDir, name)));
    await ready(page);
    assert.equal(await page.locator('#anim_rows select').count(), 5);
    assert(await page.locator('#anim_rows').evaluate(rows => Array.from(rows.querySelectorAll('label')).every(label => {
      const next = label.nextElementSibling;
      const a = label.getBoundingClientRect(), b = next.getBoundingClientRect();
      return a.right <= b.left || a.bottom <= b.top;
    })), 'Skeleton labels overlap animation menus');
    if (output) await page.screenshot({ path: path.join(output, 'spine-card.png'), fullPage: true });
    assert.deepEqual(errors, [], 'Browser runtime errors');
    console.log('PREVIEW-FILES PASS');
  } finally {
    await browser.close();
  }
}

run().catch(error => { console.error(error); process.exitCode = 1; });
