(function () {
  'use strict';

  const fps = 25;
  const alphaThreshold = 64;
  const button = document.getElementById('btn_export_gif');
  const mp4Button = document.getElementById('btn_export');
  const heightField = document.getElementById('gif_height');
  const status = document.getElementById('export_status');
  const yieldToPage = () => new Promise(resolve => setTimeout(resolve, 0));

  function createActors(){
    const characterLayers = skelList.filter(layer => /_chara[._]/i.test(layer.name));
    const candidates = characterLayers.length ? characterLayers : skelList.filter(layer => !/_bg[._]/i.test(layer.name));
    return candidates.filter(layer => !layer.check || layer.check.checked).map(layer => {
      const skeleton = new spine.Skeleton(layer.data);
      skeleton.flipY = true;
      return {
        name: layer.name,
        skeleton,
        animation: findAnim(layer),
        flip: skeleton.findBone('flip'),
      };
    });
  }

  function createSurface(){
    const canvas = document.createElement('canvas');
    const context = canvas.getContext('2d', { willReadFrequently: true });
    if (!context) throw new Error('无法创建 GIF 绘制画布');
    let webgl;
    try {
      webgl = new CGSSWebGLRenderer(document.createElement('canvas'), { preserveDrawingBuffer: true });
    } catch (_error) {}
    const renderer = webgl || new spine.canvas.SkeletonRenderer(context);
    renderer.triangleRendering = true;
    return {
      resize(width, height){
        canvas.width = width;
        canvas.height = height;
        if (webgl) webgl.resize(width, height);
      },
      draw(actors, scale, x, y){
        context.setTransform(1, 0, 0, 1, 0, 0);
        if (webgl){
          if (webgl.gl.isContextLost()) throw new Error('GIF 绘制时 WebGL 上下文丢失');
          webgl.setTransform(scale, x, y);
          webgl.clear(0, 0, 0, 0);
          for (const actor of actors) webgl.draw(actor.skeleton);
          context.globalCompositeOperation = 'copy';
          context.drawImage(webgl.canvas, 0, 0);
        } else {
          context.globalCompositeOperation = 'source-over';
          context.clearRect(0, 0, canvas.width, canvas.height);
          context.setTransform(scale, 0, 0, scale, x, y);
          for (const actor of actors) renderer.draw(actor.skeleton);
        }
        return context.getImageData(0, 0, canvas.width, canvas.height).data;
      },
      close(){
        webgl?.gl.getExtension('WEBGL_lose_context')?.loseContext();
      },
    };
  }

  function pose(actors, time, mirror){
    for (const { skeleton, animation, flip } of actors){
      skeleton.setToSetupPose();
      if (animation) animation.apply(skeleton, 0, time, true, null, 1, spine.MixPose.setup, spine.MixDirection.in);
      if (flip) flip.scaleX = (mirror ? -1 : 1) * Math.abs(flip.data.scaleX);
      skeleton.updateWorldTransform();
    }
  }

  function emptyBounds(){ return { left: Infinity, top: Infinity, right: -Infinity, bottom: -Infinity }; }

  async function measureGeometry(actors, frames, frameTime, mirror){
    const bounds = emptyBounds();
    const offset = new spine.Vector2(), size = new spine.Vector2(), vertices = [];
    for (let i = 0; i < frames; i++){
      pose(actors, frameTime(i), mirror);
      for (const { skeleton } of actors){
        skeleton.getBounds(offset, size, vertices);
        if (!Number.isFinite(offset.x + offset.y + size.x + size.y) || size.x <= 0 || size.y <= 0) continue;
        bounds.left = Math.min(bounds.left, offset.x);
        bounds.top = Math.min(bounds.top, offset.y);
        bounds.right = Math.max(bounds.right, offset.x + size.x);
        bounds.bottom = Math.max(bounds.bottom, offset.y + size.y);
      }
      if (i % 16 === 0) await yieldToPage();
    }
    if (!Number.isFinite(bounds.left)) throw new Error('当前动作没有可导出的角色范围');
    return bounds;
  }

  async function measureVisible(surface, actors, frames, frameTime, mirror, geometry){
    const width = geometry.right - geometry.left, height = geometry.bottom - geometry.top;
    const scale = 760 / Math.max(width, height);
    const w = Math.ceil(width * scale) + 4, h = Math.ceil(height * scale) + 4;
    surface.resize(w, h);
    const bounds = emptyBounds();
    for (let i = 0; i < frames; i++){
      pose(actors, frameTime(i), mirror);
      const rgba = surface.draw(actors, scale, 2 - geometry.left * scale, 2 - geometry.top * scale);
      for (let y = 0, pixel = 3; y < h; y++){
        for (let x = 0; x < w; x++, pixel += 4){
          if (rgba[pixel] < alphaThreshold) continue;
          if (x < bounds.left) bounds.left = x;
          if (x >= bounds.right) bounds.right = x + 1;
          if (y < bounds.top) bounds.top = y;
          if (y >= bounds.bottom) bounds.bottom = y + 1;
        }
      }
      if (i % 8 === 0) await yieldToPage();
    }
    if (!Number.isFinite(bounds.left)) throw new Error('当前动作没有可见的角色像素');
    return {
      left: geometry.left + (bounds.left - 2) / scale,
      top: geometry.top + (bounds.top - 2) / scale,
      width: (bounds.right - bounds.left) / scale,
      height: (bounds.bottom - bounds.top) / scale,
    };
  }

  function indexedFrame(rgba){
    // Keep the transparent pixels out of the opaque palette; GIF has binary alpha.
    const opaque = new Uint8Array(rgba.length);
    let count = 0;
    for (let i = 0; i < rgba.length; i += 4){
      if (rgba[i + 3] < alphaThreshold) continue;
      opaque[count++] = rgba[i];
      opaque[count++] = rgba[i + 1];
      opaque[count++] = rgba[i + 2];
      opaque[count++] = 255;
    }
    const palette = count ? Gifenc.quantize(opaque.subarray(0, count), 255, { format: 'rgb565' }) : [];
    const indices = new Uint8Array(rgba.length / 4);
    if (count){
      const colors = Gifenc.applyPalette(opaque.subarray(0, count), palette, 'rgb565');
      for (let i = 0, next = 0; i < indices.length; i++){
        if (rgba[i * 4 + 3] >= alphaThreshold) indices[i] = colors[next++] + 1;
      }
    }
    palette.unshift([0, 0, 0]);
    if (palette.length === 1) palette.push([0, 0, 0]);
    return { palette, indices };
  }

  button.addEventListener('click', async function(){
    if (!previewReady){ status.textContent = '请先加载动画'; return; }
    if (mp4Button.disabled || button.disabled) return;
    if (!window.Gifenc){ status.textContent = 'GIF 编码组件加载失败'; return; }
    let surface;
    button.disabled = mp4Button.disabled = heightField.disabled = true;
    try {
      const actors = createActors();
      if (!actors.length) throw new Error('请先启用角色图层');
      const duration = actors.reduce((max, actor) => Math.max(max, actor.animation?.duration || 0), 0);
      if (duration <= 0) throw new Error('请选择有时长的角色动作');
      const frames = Math.max(1, Math.round(duration * fps));
      const frameTime = i => i * duration / frames;
      const delay = i => (Math.round(frameTime(i + 1) * 100) - Math.round(frameTime(i) * 100)) * 10;
      const mirror = document.getElementById('chk_flip').checked;
      const label = actors[0].name.replace(/\.[^.]+$/, '');
      const motion = actors.find(actor => actor.animation)?.animation.name || 'animation';
      const h = Number(heightField.value);
      status.textContent = '正在测量整段动作的角色范围…';
      const geometry = await measureGeometry(actors, frames, frameTime, mirror);
      surface = createSurface();
      const bounds = await measureVisible(surface, actors, frames, frameTime, mirror, geometry);
      const border = Math.max(3, Math.round(h * 0.025));
      const scale = Math.min((h - 2 * border) / bounds.height, (2048 - 2 * border) / bounds.width);
      const w = Math.min(2048, Math.ceil(bounds.width * scale) + 2 * border);
      const x = (w - bounds.width * scale) / 2 - bounds.left * scale;
      const y = (h - bounds.height * scale) / 2 - bounds.top * scale;
      surface.resize(w, h);
      const encoder = Gifenc.GIFEncoder();
      for (let i = 0; i < frames; i++){
        pose(actors, frameTime(i), mirror);
        const rgba = surface.draw(actors, scale, x, y);
        const { palette, indices } = indexedFrame(rgba);
        encoder.writeFrame(indices, w, h, { palette, delay: Math.max(20, delay(i)), repeat: 0,
          transparent: true, transparentIndex: 0, dispose: 2 });
        if ((i + 1) % 4 === 0 || i + 1 === frames){
          status.textContent = '生成 GIF ' + (i + 1) + '/' + frames + ' 帧';
          await yieldToPage();
        }
      }
      encoder.finish();
      const name = ('CGSS_' + label + '_' + motion + '_' + w + 'x' + h).replace(/[^\p{L}\p{N}_.-]/gu, '_') + '.gif';
      const url = URL.createObjectURL(new Blob([encoder.bytes()], { type: 'image/gif' }));
      const link = document.createElement('a');
      link.href = url;
      link.download = name;
      document.body.appendChild(link);
      link.click();
      setTimeout(() => { URL.revokeObjectURL(url); link.remove(); }, 2000);
      status.textContent = '已导出 GIF (' + w + ' × ' + h + '，' + frames + ' 帧)';
    } catch (error) {
      console.error(error);
      status.textContent = 'GIF 导出失败：' + error.message;
    } finally {
      surface?.close();
      button.disabled = mp4Button.disabled = heightField.disabled = false;
    }
  });
})();
