/*
 * CGSS Spine 预览 WebGL 渲染器（自绘，不依赖 spine-ts WebGL 包）。
 * 直接用 spine-core 的 Skeleton/Attachment 数据：
 *   - MeshAttachment.computeWorldVertices(slot, 0, len, world, 0, 2)
 *   - RegionAttachment.computeWorldVertices(bone, world, 0, 2)
 * 逐三角形共享顶点光栅化 -> 不会像 canvas 2D clip 那样在三角形边上留抗锯齿接缝。
 *
 * 用法：
 *   var glRenderer = new CGSSWebGLRenderer(canvas);       // 内部创建 webgl 上下文
 *   glRenderer.triangleRendering = true;                   // 兼容预览页习惯，保留
 *   glRenderer.setTransform(scale, tx, ty);                // 世界->画布像素
 *   glRenderer.clear(0.2, 0.2, 0.2, 1);                    // 清背景
 *   glRenderer.draw(skeleton);
 */
(function (global) {
  'use strict';

  var VERT_SRC =
    'attribute vec2 aPos;' +
    'attribute vec2 aUV;' +
    'attribute vec4 aColor;' +
    'varying vec2 vUV;' +
    'varying vec4 vColor;' +
    'void main(){ gl_Position = vec4(aPos, 0.0, 1.0); vUV = aUV; vColor = aColor; }';

  var FRAG_SRC =
    'precision mediump float;' +
    'varying vec2 vUV;' +
    'varying vec4 vColor;' +
    'uniform sampler2D uTex;' +
    // 输出预乘颜色，与 canvas 'lighter'/'source-over' 的合成规则一致
    'void main(){ vec4 t = texture2D(uTex, vUV);' +
    'float a = t.a * vColor.a;' +
    'gl_FragColor = vec4(t.rgb * vColor.rgb * a, a); }';

  function compileShader(gl, type, src) {
    var sh = gl.createShader(type);
    gl.shaderSource(sh, src);
    gl.compileShader(sh);
    if (!gl.getShaderParameter(sh, gl.COMPILE_STATUS)) {
      throw new Error('shader compile: ' + gl.getShaderInfoLog(sh));
    }
    return sh;
  }

  function CGSSWebGLRenderer(canvas, opts) {
    opts = opts || {};
    this.canvas = canvas;
    this.gl = canvas.getContext('webgl', {
      alpha: true,
      premultipliedAlpha: true,
      preserveDrawingBuffer: !!opts.preserveDrawingBuffer,
      antialias: false
    }) || canvas.getContext('experimental-webgl', {
      alpha: true,
      premultipliedAlpha: true,
      preserveDrawingBuffer: !!opts.preserveDrawingBuffer,
      antialias: false
    });
    if (!this.gl) throw new Error('WebGL 不可用');
    var gl = this.gl;
    this.program = gl.createProgram();
    gl.attachShader(this.program, compileShader(gl, gl.VERTEX_SHADER, VERT_SRC));
    gl.attachShader(this.program, compileShader(gl, gl.FRAGMENT_SHADER, FRAG_SRC));
    gl.linkProgram(this.program);
    if (!gl.getProgramParameter(this.program, gl.LINK_STATUS)) {
      throw new Error('program link: ' + gl.getProgramInfoLog(this.program));
    }
    this.aPos = gl.getAttribLocation(this.program, 'aPos');
    this.aUV = gl.getAttribLocation(this.program, 'aUV');
    this.aColor = gl.getAttribLocation(this.program, 'aColor');
    this.uTex = gl.getUniformLocation(this.program, 'uTex');

    this.vbo = gl.createBuffer();
    this.texCache = {};
    this.width = canvas.width || 1;
    this.height = canvas.height || 1;
    this.triangleRendering = true; // 兼容旧调用
    this._scale = 1; this._tx = 0; this._ty = 0;
    this._batch = [];
  }

  CGSSWebGLRenderer.prototype.resize = function (w, h) {
    this.width = w; this.height = h;
    this.canvas.width = w; this.canvas.height = h;
    this.gl.viewport(0, 0, w, h);
  };

  CGSSWebGLRenderer.prototype.setTransform = function (scale, tx, ty) {
    this._scale = scale; this._tx = tx; this._ty = ty;
  };

  CGSSWebGLRenderer.prototype.clear = function (r, g, b, a) {
    var gl = this.gl;
    gl.disable(gl.SCISSOR_TEST);
    gl.clearColor(r, g, b, a);
    gl.clear(gl.COLOR_BUFFER_BIT);
  };

  CGSSWebGLRenderer.prototype._texture = function (img) {
    if (!img) return null;
    if (this.texCache[img.__cgssTexId]) return this.texCache[img.__cgssTexId];
    var gl = this.gl;
    var tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, img);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    if (!img.__cgssTexId) img.__cgssTexId = Math.random().toString(36).slice(2);
    this.texCache[img.__cgssTexId] = tex;
    return tex;
  };

  CGSSWebGLRenderer.prototype._push = function (tex, x, y, u, v, cr, cg, cb, ca) {
    // 世界->画布像素
    var sx = x * this._scale + this._tx;
    var sy = y * this._scale + this._ty;
    // 像素->NDC（y 向下）
    var nx = sx / this.width * 2 - 1;
    var ny = 1 - sy / this.height * 2;
    this._batch.push(nx, ny, u, v, cr, cg, cb, ca);
  };

  CGSSWebGLRenderer.prototype._flush = function () {
    if (!this._batch.length) return;
    var gl = this.gl;
    gl.useProgram(this.program);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(this._batch), gl.DYNAMIC_DRAW);
    var stride = 8 * 4;
    gl.enableVertexAttribArray(this.aPos);
    gl.vertexAttribPointer(this.aPos, 2, gl.FLOAT, false, stride, 0);
    gl.enableVertexAttribArray(this.aUV);
    gl.vertexAttribPointer(this.aUV, 2, gl.FLOAT, false, stride, 8);
    gl.enableVertexAttribArray(this.aColor);
    gl.vertexAttribPointer(this.aColor, 4, gl.FLOAT, false, stride, 16);
    gl.uniform1i(this.uTex, 0);
    gl.drawArrays(gl.TRIANGLES, 0, this._batch.length / 8);
    this._batch.length = 0;
  };

  CGSSWebGLRenderer.prototype._setBlend = function (mode) {
    var gl = this.gl;
    if (mode === 1) { // additive
      gl.blendFunc(gl.ONE, gl.ONE);
    } else if (mode === 2) { // multiply
      gl.blendFunc(gl.DST_COLOR, gl.ONE_MINUS_SRC_ALPHA);
    } else if (mode === 3) { // screen
      gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_COLOR);
    } else {
      gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    }
  };

  CGSSWebGLRenderer.prototype.draw = function (skeleton) {
    var gl = this.gl;
    gl.enable(gl.BLEND);
    gl.disable(gl.DEPTH_TEST);
    gl.useProgram(this.program);

    var skeletonColor = skeleton.color;
    var drawOrder = skeleton.drawOrder;
    var currentTex = null, currentBlend = -1;
    var self = this;

    function beginTex(tex, blend) {
      if (currentTex === tex && currentBlend === blend) return;
      self._flush();
      currentTex = tex;
      currentBlend = blend;
      if (tex) gl.bindTexture(gl.TEXTURE_2D, tex);
      self._setBlend(blend);
    }

    for (var i = 0, n = drawOrder.length; i < n; i++) {
      var slot = drawOrder[i];
      var attachment = slot.attachment;
      if (!attachment) continue;
      var slotColor = slot.color;
      var attachmentColor = attachment.color;
      if (!attachmentColor) attachmentColor = { r: 1, g: 1, b: 1, a: 1 };
      var cr = skeletonColor.r * slotColor.r * attachmentColor.r;
      var cg = skeletonColor.g * slotColor.g * attachmentColor.g;
      var cb = skeletonColor.b * slotColor.b * attachmentColor.b;
      var ca = skeletonColor.a * slotColor.a * attachmentColor.a;
      var blend = slot.data.blendMode || 0;

      if (attachment instanceof spine.MeshAttachment) {
        var mesh = attachment;
        if (!mesh.region || !mesh.region.renderObject) continue;
        var tex = this._texture(mesh.region.renderObject.texture.getImage());
        if (!tex) continue;
        var wl = mesh.worldVerticesLength || mesh.vertices.length;
        var world = this._world || new Float32Array(0);
        var need = wl;
        if (world.length < need) world = new Float32Array(need);
        this._world = world;
        mesh.computeWorldVertices(slot, 0, wl, world, 0, 2);
        var uvs = mesh.uvs, tris = mesh.triangles;
        beginTex(tex, blend);
        for (var t = 0; t < tris.length; t++) {
          var vi = tris[t] * 2;
          this._push(tex, world[vi], world[vi + 1], uvs[tris[t] * 2], uvs[tris[t] * 2 + 1], cr, cg, cb, ca);
        }
      } else if (attachment instanceof spine.RegionAttachment) {
        var region = attachment;
        var tex2 = this._texture(region.region.renderObject.texture.getImage());
        if (!tex2) continue;
        var world4 = this._world4 || new Float32Array(8);
        this._world4 = world4;
        region.computeWorldVertices(slot.bone, world4, 0, 2);
        var uvs2 = region.uvs;
        beginTex(tex2, blend);
        var quad = [0, 1, 2, 0, 2, 3];
        for (var q = 0; q < quad.length; q++) {
          var qi = quad[q] * 2;
          this._push(tex2, world4[qi], world4[qi + 1], uvs2[qi], uvs2[qi + 1], cr, cg, cb, ca);
        }
      }
    }
    this._flush();
  };

  global.CGSSWebGLRenderer = CGSSWebGLRenderer;
})(typeof window !== 'undefined' ? window : globalThis);
