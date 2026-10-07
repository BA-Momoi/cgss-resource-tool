/*
 * CGSS Spine 2.1/3.6 binary (.skel) -> Spine 3.6 JSON object.
 * JS port of skel2json.py. Handles the CGSS custom header (44 bytes),
 * big-endian floats/uint32/int16, and the missing nonessential section.
 *
 * Usage in browser:
 *   <script src="spine-core.js"></script>
 *   <script src="cgss_skel_parser.js"></script>
 *   var json = CGSSSkelParser.parse(arrayBuffer);   // -> plain JSON object
 */
(function (global) {
  'use strict';

  var TRANSFORM_NAMES = ["normal", "onlyTranslation", "noRotationOrReflection", "noScale", "noScaleOrReflection"];
  var BLEND_NAMES = ["normal", "additive", "multiply", "screen"];
  var POSITION_MODES = ["fixed", "percent"];
  var SPACING_MODES = ["length", "fixed", "percent"];
  var ROTATE_MODES = ["tangent", "chain", "chainScale"];

  function Reader(b) {
    this.b = new Uint8Array(b);
    this.pos = 44; // skip 0x1C + 27-byte hash + version string + 9-byte blob
  }
  Reader.prototype.readByte = function () {
    if (this.pos >= this.b.length) throw new Error('EOF');
    return this.b[this.pos++];
  };
  Reader.prototype.readBool = function () { return this.readByte() !== 0; };
  Reader.prototype.readVarint = function () {
    var b = this.readByte(), result = b & 0x7f;
    if (b & 0x80) {
      b = this.readByte(); result |= (b & 0x7f) << 7;
      if (b & 0x80) {
        b = this.readByte(); result |= (b & 0x7f) << 14;
        if (b & 0x80) {
          b = this.readByte(); result |= (b & 0x7f) << 21;
          if (b & 0x80) {
            b = this.readByte(); result |= (b & 0x7f) << 28;
          }
        }
      }
    }
    return result;
  };
  Reader.prototype.readInt = function (opt) {
    var result = this.readVarint();
    return opt ? result : (result >> 1) ^ -(result & 1);
  };
  Reader.prototype.readUint32 = function () {
    var p = this.pos;
    if (p + 4 > this.b.length) throw new Error('EOF in uint32');
    this.pos += 4;
    return ((this.b[p] << 24) | (this.b[p + 1] << 16) | (this.b[p + 2] << 8) | this.b[p + 3]) >>> 0;
  };
  Reader.prototype.readFloat = function () {
    var p = this.pos;
    if (p + 4 > this.b.length) throw new Error('EOF in float');
    this.pos += 4;
    var v = new DataView(this.b.buffer, this.b.byteOffset + p, 4).getFloat32(0, false); // big-endian
    return v;
  };
  Reader.prototype.readShort = function () {
    var p = this.pos;
    if (p + 2 > this.b.length) throw new Error('EOF in short');
    this.pos += 2;
    return new DataView(this.b.buffer, this.b.byteOffset + p, 2).getInt16(0, false); // big-endian
  };
  Reader.prototype.readString = function () {
    var n = this.readVarint();
    if (n === 0) return null;
    if (n === 1) return '';
    n -= 1;
    if (this.pos + n > this.b.length) throw new Error('EOF in string');
    var s = '';
    for (var i = 0; i < n; i++) s += String.fromCharCode(this.b[this.pos + i]);
    this.pos += n;
    return s;
  };

  function f(x) {
    if (x === 0) return 0;
    if (x !== x) return 0;
    return Math.round(x * 1e6) / 1e6;
  }

  function rgbaHex(v) {
    return ('00000000' + v.toString(16)).slice(-8);
  }

  function rgbHex(v) {
    return ('000000' + ((v >>> 8) & 0xffffff).toString(16)).slice(-6);
  }

  function Parser(bytes) {
    this.r = new Reader(bytes);
    this.bones = [];
    this.slots = [];
    this.ik = [];
    this.transform = [];
    this.path = [];
    this.skins = [];
    this.events = [];
    this.animations = [];
  }

  Parser.prototype.parse = function () {
    var r = this.r, i, n;
    n = r.readVarint();
    for (i = 0; i < n; i++) {
      var boneName = r.readString();
      var parent = i === 0 ? null : r.readVarint();
      this.bones.push({
        name: boneName,
        parent: parent === null ? null : this.bones[parent].name,
        rotation: r.readFloat(), x: r.readFloat(), y: r.readFloat(),
        scaleX: r.readFloat(), scaleY: r.readFloat(),
        shearX: r.readFloat(), shearY: r.readFloat(),
        length: r.readFloat(),
        transform: TRANSFORM_NAMES[r.readVarint()]
      });
    }
    n = r.readVarint();
    for (i = 0; i < n; i++) {
      var slot = { name: r.readString(), bone: this.bones[r.readVarint()].name };
      slot.color = rgbaHex(r.readUint32());
      var dark = r.readUint32();
      if (dark !== 0xffffffff) slot.dark = rgbHex(dark);
      var att = r.readString();
      if (att !== null) slot.attachment = att;
      slot.blend = BLEND_NAMES[r.readVarint()];
      this.slots.push(slot);
    }
    n = r.readVarint();
    for (i = 0; i < n; i++) {
      var d = { name: r.readString(), order: r.readVarint(), bones: [], target: null };
      for (var j = 0, m = r.readVarint(); j < m; j++) d.bones.push(this.bones[r.readVarint()].name);
      d.target = this.bones[r.readVarint()].name;
      d.mix = r.readFloat();
      d.bendPositive = (r.readByte() === 1);
      this.ik.push(d);
    }
    n = r.readVarint();
    for (i = 0; i < n; i++) {
      var t = { name: r.readString(), order: r.readVarint(), bones: [], target: null };
      for (var j2 = 0, m2 = r.readVarint(); j2 < m2; j2++) t.bones.push(this.bones[r.readVarint()].name);
      t.target = this.bones[r.readVarint()].name;
      t.local = r.readBool(); t.relative = r.readBool();
      t.rotation = r.readFloat(); t.x = r.readFloat(); t.y = r.readFloat();
      t.scaleX = r.readFloat(); t.scaleY = r.readFloat(); t.shearY = r.readFloat();
      t.rotateMix = r.readFloat(); t.translateMix = r.readFloat();
      t.scaleMix = r.readFloat(); t.shearMix = r.readFloat();
      this.transform.push(t);
    }
    n = r.readVarint();
    for (i = 0; i < n; i++) {
      var p = { name: r.readString(), order: r.readVarint(), bones: [], target: null };
      for (var j3 = 0, m3 = r.readVarint(); j3 < m3; j3++) p.bones.push(this.bones[r.readVarint()].name);
      p.target = this.slots[r.readVarint()].name;
      p.positionMode = POSITION_MODES[r.readVarint()];
      p.spacingMode = SPACING_MODES[r.readVarint()];
      p.rotateMode = ROTATE_MODES[r.readVarint()];
      p.rotation = r.readFloat(); p.position = r.readFloat(); p.spacing = r.readFloat();
      p.rotateMix = r.readFloat(); p.translateMix = r.readFloat();
      this.path.push(p);
    }
    var skin = this.readSkin('default');
    if (skin) this.skins.push(skin);
    n = r.readVarint();
    for (i = 0; i < n; i++) {
      skin = this.readSkin(r.readString());
      if (skin) this.skins.push(skin);
    }
    n = r.readVarint();
    for (i = 0; i < n; i++) {
      var ev = { name: r.readString() };
      ev.int = r.readInt(false);
      ev.float = r.readFloat();
      ev.string = r.readString() || '';
      this.events.push(ev);
    }
    n = r.readVarint();
    for (i = 0; i < n; i++) this.readAnimation(r.readString());
    return this.toJson();
  };

  Parser.prototype.readSkin = function (skinName) {
    var r = this.r;
    var slotCount = r.readVarint();
    if (slotCount === 0) return null;
    var skin = { name: skinName, attachments: {} };
    for (var i = 0; i < slotCount; i++) {
      var slotIdx = r.readVarint();
      var slotName = this.slots[slotIdx].name;
      var atts = {};
      for (var j = 0, n = r.readVarint(); j < n; j++) {
        var name = r.readString();
        var a = this.readAttachment(slotIdx, name);
        if (a) atts[name] = a;
      }
      skin.attachments[slotName] = atts;
    }
    return skin;
  };

  Parser.prototype.readAttachment = function (slotIdx, attachmentName) {
    var r = this.r;
    var name = r.readString();
    if (name === null) name = attachmentName;
    var type = r.readByte();
    if (type === 0) { // region
      var path = r.readString();
      if (path === null) path = name;
      var att = { type: 'region', name: name, path: path,
        rotation: r.readFloat(), x: r.readFloat(), y: r.readFloat(),
        scaleX: r.readFloat(), scaleY: r.readFloat(),
        width: r.readFloat(), height: r.readFloat() };
      att.color = rgbaHex(r.readUint32());
      return att;
    }
    if (type === 1) { // bounding box
      var vc = r.readVarint();
      var v = this.readVertices(vc);
      return { type: 'boundingbox', name: name, vertexCount: vc, vertices: v.vertices };
    }
    if (type === 2) { // mesh
      var mp = r.readString();
      if (mp === null) mp = name;
      var mc = r.readUint32();
      var mvc = r.readVarint();
      var uvs = [];
      for (var i = 0; i < mvc * 2; i++) uvs.push(f(r.readFloat()));
      var tris = [];
      for (var j = 0, n = r.readVarint(); j < n; j++) tris.push(r.readShort());
      var mv = this.readVertices(mvc);
      var hull = r.readVarint();
      return { type: 'mesh', name: name, path: mp, uvs: uvs, triangles: tris,
        vertices: mv.vertices, hull: hull, color: rgbaHex(mc) };
    }
    if (type === 3) { // linked mesh
      var lp = r.readString();
      if (lp === null) lp = name;
      var lc = r.readUint32();
      var skinName = r.readString();
      var parent = r.readString();
      var inherit = r.readBool();
      var la = { type: 'linkedmesh', name: name, path: lp, parent: parent, deform: inherit, color: rgbaHex(lc) };
      if (skinName !== null) la.skin = skinName;
      return la;
    }
    if (type === 4) { // path
      var closed = r.readBool(), cs = r.readBool();
      var pc = r.readVarint();
      var pv = this.readVertices(pc);
      var lengths = [];
      for (var k = 0; k < pc / 3; k++) lengths.push(f(r.readFloat()));
      return { type: 'path', name: name, closed: closed, constantSpeed: cs,
        vertexCount: pc, vertices: pv.vertices, lengths: lengths };
    }
    if (type === 5) { // point
      return { type: 'point', name: name, rotation: r.readFloat(), x: r.readFloat(), y: r.readFloat() };
    }
    if (type === 6) { // clipping
      var endSlot = r.readVarint();
      var cc = r.readVarint();
      var cv = this.readVertices(cc);
      return { type: 'clipping', name: name, end: this.slots[endSlot].name,
        vertexCount: cc, vertices: cv.vertices };
    }
    throw new Error('unknown attachment type ' + type);
  };

  Parser.prototype.readVertices = function (vc) {
    var r = this.r;
    if (!r.readBool()) {
      var flat = [];
      for (var i = 0; i < vc * 2; i++) flat.push(f(r.readFloat()));
      return { vertices: flat, bones: [] };
    }
    var out = [];
    for (var v = 0; v < vc; v++) {
      var bc = r.readVarint();
      out.push(bc);
      for (var j = 0; j < bc; j++) {
        out.push(r.readVarint());
        out.push(f(r.readFloat()));
        out.push(f(r.readFloat()));
        out.push(f(r.readFloat()));
      }
    }
    return { vertices: out, bones: [] };
  };

  Parser.prototype.readCurve = function () {
    var r = this.r;
    var t = r.readByte();
    if (t === 1) return 'stepped';
    if (t === 2) return [f(r.readFloat()), f(r.readFloat()), f(r.readFloat()), f(r.readFloat())];
    return null;
  };

  Parser.prototype.readAnimation = function (name) {
    var r = this.r, i, j, n, timelines = [];
    n = r.readVarint();
    for (i = 0; i < n; i++) {
      var slotIdx = r.readVarint();
      for (var j = 0, n2 = r.readVarint(); j < n2; j++) {
        var ttype = r.readByte(), fc = r.readVarint(), frames = [], fi;
        if (ttype === 0) {
          for (fi = 0; fi < fc; fi++) frames.push({ time: f(r.readFloat()), name: r.readString() });
          timelines.push({ kind: 'slot', slot: slotIdx, type: 'attachment', frames: frames });
        } else if (ttype === 1) {
          for (fi = 0; fi < fc; fi++) {
            var fr = { time: f(r.readFloat()), color: rgbaHex(r.readUint32()) };
            if (fi < fc - 1) fr.curve = this.readCurve();
            frames.push(fr);
          }
          timelines.push({ kind: 'slot', slot: slotIdx, type: 'color', frames: frames });
        } else if (ttype === 2) {
          for (fi = 0; fi < fc; fi++) {
            var fr2 = { time: f(r.readFloat()), light: rgbaHex(r.readUint32()), dark: rgbHex(r.readUint32()) };
            if (fi < fc - 1) fr2.curve = this.readCurve();
            frames.push(fr2);
          }
          timelines.push({ kind: 'slot', slot: slotIdx, type: 'twoColor', frames: frames });
        } else throw new Error('unknown slot timeline ' + ttype);
      }
    }
    n = r.readVarint();
    for (i = 0; i < n; i++) {
      var boneIdx = r.readVarint();
      for (var j = 0, n2 = r.readVarint(); j < n2; j++) {
        var bt = r.readByte(), bfc = r.readVarint(), bframes = [];
        for (var bi = 0; bi < bfc; bi++) {
          var bf;
          if (bt === 0) bf = { time: f(r.readFloat()), angle: f(r.readFloat()) };
          else bf = { time: f(r.readFloat()), x: f(r.readFloat()), y: f(r.readFloat()) };
          if (bi < bfc - 1) bf.curve = this.readCurve();
          bframes.push(bf);
        }
        var tname = ['rotate', 'translate', 'scale', 'shear'][bt];
        timelines.push({ kind: 'bone', bone: boneIdx, type: tname, frames: bframes });
      }
    }
    n = r.readVarint();
    for (i = 0; i < n; i++) {
      var ikIdx = r.readVarint(), ikfc = r.readVarint(), ikf = [];
      for (j = 0; j < ikfc; j++) {
        var ikfr = { time: f(r.readFloat()), mix: f(r.readFloat()), bendPositive: (r.readByte() === 1) };
        if (j < ikfc - 1) ikfr.curve = this.readCurve();
        ikf.push(ikfr);
      }
      timelines.push({ kind: 'ik', index: ikIdx, frames: ikf });
    }
    n = r.readVarint();
    for (i = 0; i < n; i++) {
      var tfIdx = r.readVarint(), tffc = r.readVarint(), tff = [];
      for (j = 0; j < tffc; j++) {
        var tffr = { time: f(r.readFloat()), rotateMix: f(r.readFloat()),
          translateMix: f(r.readFloat()), scaleMix: f(r.readFloat()), shearMix: f(r.readFloat()) };
        if (j < tffc - 1) tffr.curve = this.readCurve();
        tff.push(tffr);
      }
      timelines.push({ kind: 'transform', index: tfIdx, frames: tff });
    }
    n = r.readVarint();
    for (i = 0; i < n; i++) {
      var pcIdx = r.readVarint();
      for (var j = 0, n2 = r.readVarint(); j < n2; j++) {
        var pt = r.readByte(), pfc = r.readVarint(), pf = [];
        for (var pi = 0; pi < pfc; pi++) {
          var pfr;
          if (pt === 0 || pt === 1) {
            pfr = { time: f(r.readFloat()) };
            pfr[pt === 0 ? 'position' : 'spacing'] = f(r.readFloat());
          } else {
            pfr = { time: f(r.readFloat()), rotateMix: f(r.readFloat()), translateMix: f(r.readFloat()) };
          }
          if (pi < pfc - 1) pfr.curve = this.readCurve();
          pf.push(pfr);
        }
        timelines.push({ kind: 'path', index: pcIdx, type: ['position', 'spacing', 'mix'][pt], frames: pf });
      }
    }
    n = r.readVarint();
    for (i = 0; i < n; i++) {
      var skinIdx = r.readVarint();
      var skinName = this.skins[skinIdx].name;
      for (var j = 0, n2 = r.readVarint(); j < n2; j++) {
        var dSlot = r.readVarint();
        for (var k = 0, n3 = r.readVarint(); k < n3; k++) {
          var attName = r.readString(), dfc = r.readVarint(), df = [];
          for (var di = 0; di < dfc; di++) {
            var t = r.readFloat(), end = r.readVarint(), dfr;
            if (end === 0) {
              dfr = { time: f(t) };
            } else {
              var start = r.readVarint();
              end += start;
              var verts = [];
              for (var v2 = start; v2 < end; v2++) verts.push(f(r.readFloat()));
              dfr = { time: f(t), offset: start, vertices: verts };
            }
            if (di < dfc - 1) dfr.curve = this.readCurve();
            df.push(dfr);
          }
          timelines.push({ kind: 'deform', skin: skinName, slot: dSlot, attachment: attName, frames: df });
        }
      }
    }
    n = r.readVarint();
    if (n > 0) {
      var dof = [];
      for (i = 0; i < n; i++) {
        var dt = r.readFloat(), oc = r.readVarint(), offs = [];
        for (j = 0; j < oc; j++) {
          var si = r.readVarint(), so = r.readVarint();
          offs.push({ slot: this.slots[si].name, offset: so });
        }
        dof.push({ time: f(dt), offsets: offs });
      }
      timelines.push({ kind: 'drawOrder', frames: dof });
    }
    n = r.readVarint();
    if (n > 0) {
      var evf = [];
      for (i = 0; i < n; i++) {
        var et = r.readFloat(), ev = this.events[r.readVarint()];
        var iv = r.readInt(false), fv = r.readFloat();
        var sv = r.readBool() ? r.readString() : null;
        var efr = { time: f(et), name: ev.name };
        if (iv !== 0) efr.int = iv;
        if (fv !== 0) efr.float = f(fv);
        if (sv !== null) efr.string = sv;
        evf.push(efr);
      }
      timelines.push({ kind: 'event', frames: evf });
    }
    this.animations.push({ name: name, timelines: timelines });
  };

  Parser.prototype.toJson = function () {
    var p = this, root = {
      skeleton: { hash: 'cgss', spine: '3.6.47', width: 0, height: 0, fps: 30, images: '', audio: '' },
      bones: [], slots: [], skins: {}, animations: {}
    };
    this.bones.forEach(function (b) {
      var d = { name: b.name };
      if (b.parent !== null) d.parent = b.parent;
      ['length', 'x', 'y', 'rotation', 'scaleX', 'scaleY', 'shearX', 'shearY'].forEach(function (k) {
        var isScale = k === 'scaleX' || k === 'scaleY';
        if (isScale ? b[k] !== 1 : b[k] !== 0) d[k] = f(b[k]);
      });
      if (b.transform !== 'normal') d.transform = b.transform;
      root.bones.push(d);
    });
    root.slots = this.slots.slice();
    if (this.ik.length) root.ik = this.ik;
    if (this.transform.length) root.transform = this.transform;
    if (this.path.length) root.path = this.path;
    var skinsObj = {};
    this.skins.forEach(function (s) { skinsObj[s.name] = s.attachments; });
    root.skins = skinsObj;
    if (this.events.length) {
      var evObj = {};
      this.events.forEach(function (e) {
        var m = {};
        if (e.int !== 0) m.int = e.int;
        if (e.float !== 0) m.float = f(e.float);
        if (e.string) m.string = e.string;
        evObj[e.name] = m;
      });
      root.events = evObj;
    }
    this.animations.forEach(function (anim) {
      var a = {};
      anim.timelines.forEach(function (tl) {
        if (tl.kind === 'slot') {
          var sn = p.slots[tl.slot].name;
          if (!a.slots) a.slots = {};
          if (!a.slots[sn]) a.slots[sn] = {};
          a.slots[sn][tl.type] = tl.frames;
        } else if (tl.kind === 'bone') {
          var bn = p.bones[tl.bone].name;
          if (!a.bones) a.bones = {};
          if (!a.bones[bn]) a.bones[bn] = {};
          a.bones[bn][tl.type] = tl.frames;
        } else if (tl.kind === 'ik') {
          if (!a.ik) a.ik = {};
          a.ik[p.ik[tl.index].name] = tl.frames;
        } else if (tl.kind === 'transform') {
          if (!a.transform) a.transform = {};
          a.transform[p.transform[tl.index].name] = tl.frames;
        } else if (tl.kind === 'path') {
          if (!a.paths) a.paths = {};
          var pn = p.path[tl.index].name;
          if (!a.paths[pn]) a.paths[pn] = {};
          a.paths[pn][tl.type] = tl.frames;
        } else if (tl.kind === 'deform') {
          if (!a.deform) a.deform = {};
          if (!a.deform[tl.skin]) a.deform[tl.skin] = {};
          var dn = p.slots[tl.slot].name;
          if (!a.deform[tl.skin][dn]) a.deform[tl.skin][dn] = {};
          a.deform[tl.skin][dn][tl.attachment] = tl.frames;
        } else if (tl.kind === 'drawOrder') {
          a.drawOrder = tl.frames;
        } else if (tl.kind === 'event') {
          a.events = tl.frames;
        }
      });
      root.animations[anim.name] = a;
    });
    return root;
  };

  // The petit layout matches spine_convert.c; its card atlases use half-size coordinates.
  function PetitParser(bytes) { Parser.call(this, bytes); }
  PetitParser.prototype = Object.create(Parser.prototype);
  PetitParser.prototype.parse = function () {
    var r = this.r, i, n, scale = 0.5;
    n = r.readVarint();
    for (i = 0; i < n; i++) {
      var name = r.readString(), parent = r.readVarint() - 1;
      var bone = { name: name, parent: parent < 0 ? null : this.bones[parent].name,
        x: r.readFloat() * scale, y: r.readFloat() * scale,
        scaleX: r.readFloat(), scaleY: r.readFloat(),
        rotation: r.readFloat(), length: r.readFloat() * scale,
        shearX: 0, shearY: 0, transform: 'normal' };
      if (r.readBool()) bone.scaleX = -bone.scaleX;
      if (r.readBool()) bone.scaleY = -bone.scaleY;
      var inheritScale = r.readBool(), inheritRotation = r.readBool();
      if (!inheritScale && !inheritRotation) bone.transform = 'onlyTranslation';
      else if (!inheritScale) bone.transform = 'noScale';
      else if (!inheritRotation) bone.transform = 'noRotationOrReflection';
      this.bones.push(bone);
    }
    n = r.readVarint();
    for (i = 0; i < n; i++) {
      var ik = { name: r.readString(), bones: [] };
      for (var b = 0, count = r.readVarint(); b < count; b++) ik.bones.push(this.bones[r.readVarint()].name);
      ik.target = this.bones[r.readVarint()].name;
      ik.mix = r.readFloat(); ik.bendPositive = r.readByte() === 1;
      this.ik.push(ik);
    }
    n = r.readVarint();
    for (i = 0; i < n; i++) {
      var slot = { name: r.readString(), bone: this.bones[r.readVarint()].name, color: rgbaHex(r.readUint32()) };
      var attachment = r.readString();
      if (attachment !== null) slot.attachment = attachment;
      slot.blend = r.readBool() ? 'additive' : 'normal';
      this.slots.push(slot);
    }
    var skin = this.readSkin('default');
    if (skin) this.skins.push(skin);
    n = r.readVarint();
    for (i = 0; i < n; i++) {
      skin = this.readSkin(r.readString());
      if (skin) this.skins.push(skin);
    }
    n = r.readVarint();
    for (i = 0; i < n; i++) this.events.push({ name: r.readString(), int: r.readInt(false), float: r.readFloat(), string: r.readString() });
    n = r.readVarint();
    for (i = 0; i < n; i++) this.readAnimation(r.readString());
    return this.toJson();
  };

  PetitParser.prototype.readAttachment = function (slotIdx, attachmentName) {
    var r = this.r, name = r.readString() || attachmentName, type = r.readByte(), scale = 0.5;
    var att = { type: type === 0 ? 'region' : (type === 1 ? 'boundingbox' : 'mesh'), name: name };
    if (type === 0) {
      att.path = r.readString() || name;
      att.x = f(r.readFloat() * scale); att.y = f(r.readFloat() * scale);
      att.scaleX = f(r.readFloat()); att.scaleY = f(r.readFloat());
      att.rotation = f(r.readFloat());
      att.width = f(r.readFloat() * scale); att.height = f(r.readFloat() * scale);
      att.color = rgbaHex(r.readUint32());
    } else if (type === 1) {
      var count = r.readVarint();
      att.vertexCount = count / 2; att.vertices = [];
      for (var i = 0; i < count; i++) att.vertices.push(f(r.readFloat() * scale));
    } else if (type === 2 || type === 3) {
      att.path = r.readString() || name; att.color = rgbaHex(r.readUint32());
      att.uvs = []; att.triangles = []; att.vertices = [];
      for (var u = 0, nu = r.readVarint(); u < nu; u++) att.uvs.push(f(r.readFloat()));
      for (var t = 0, nt = r.readVarint(); t < nt; t++) att.triangles.push(r.readShort());
      var nv = r.readVarint();
      for (var v = 0; v < nv; v++) {
        if (type === 2) att.vertices.push(f(r.readFloat() * scale));
        else {
          var bones = r.readFloat();
          if (!Number.isInteger(bones) || bones < 0) throw new Error('Invalid weighted vertex');
          att.vertices.push(bones);
          for (var b = 0; b < bones; b++) att.vertices.push(r.readFloat(), f(r.readFloat() * scale), f(r.readFloat() * scale), f(r.readFloat()));
        }
      }
      att.hull = r.readVarint();
    } else throw new Error('Unknown Spine 2.1 attachment type ' + type);
    return att;
  };

  PetitParser.prototype.readAnimation = function (name) {
    var r = this.r, timelines = [], i, j, n, fi, count;
    n = r.readVarint();
    for (i = 0; i < n; i++) {
      var slot = r.readVarint();
      for (j = 0, count = r.readVarint(); j < count; j++) {
        var type = r.readByte(), fc = r.readVarint(), frames = [];
        if (type !== 3 && type !== 4) throw new Error('Unknown Spine 2.1 slot timeline ' + type);
        for (fi = 0; fi < fc; fi++) {
          var frame = { time: f(r.readFloat()) };
          if (type === 3) frame.name = r.readString();
          else { frame.color = rgbaHex(r.readUint32()); if (fi < fc - 1) frame.curve = this.readCurve(); }
          frames.push(frame);
        }
        timelines.push({ kind: 'slot', slot: slot, type: type === 3 ? 'attachment' : 'color', frames: frames });
      }
    }
    n = r.readVarint();
    for (i = 0; i < n; i++) {
      var bone = r.readVarint();
      for (j = 0, count = r.readVarint(); j < count; j++) {
        type = r.readByte(); fc = r.readVarint(); frames = [];
        if ([0, 1, 2, 5, 6].indexOf(type) < 0) throw new Error('Unknown Spine 2.1 bone timeline ' + type);
        for (fi = 0; fi < fc; fi++) {
          frame = { time: f(r.readFloat()) };
          if (type === 5 || type === 6) { r.readBool(); continue; }
          if (type === 1) frame.angle = f(r.readFloat());
          else {
            var scale = type === 2 ? 0.5 : 1;
            frame.x = f(r.readFloat() * scale); frame.y = f(r.readFloat() * scale);
          }
          if (fi < fc - 1) frame.curve = this.readCurve();
          frames.push(frame);
        }
        // Flip timelines have no equivalent in the existing 3.6 JSON converter.
        if (type !== 5 && type !== 6) timelines.push({ kind: 'bone', bone: bone, type: ['scale', 'rotate', 'translate'][type], frames: frames });
      }
    }
    n = r.readVarint();
    for (i = 0; i < n; i++) {
      var ik = r.readVarint(); fc = r.readVarint(); frames = [];
      for (fi = 0; fi < fc; fi++) {
        frame = { time: f(r.readFloat()), mix: f(r.readFloat()), bendPositive: r.readByte() === 1 };
        if (fi < fc - 1) frame.curve = this.readCurve();
        frames.push(frame);
      }
      timelines.push({ kind: 'ik', index: ik, frames: frames });
    }
    n = r.readVarint();
    for (i = 0; i < n; i++) {
      var skin = this.skins[r.readVarint()].name;
      for (j = 0, count = r.readVarint(); j < count; j++) {
        slot = r.readVarint();
        for (var a = 0, ac = r.readVarint(); a < ac; a++) {
          var attachment = r.readString(); fc = r.readVarint(); frames = [];
          for (fi = 0; fi < fc; fi++) {
            frame = { time: f(r.readFloat()) };
            var end = r.readVarint();
            if (end) {
              frame.offset = r.readVarint(); frame.vertices = [];
              for (var v = 0; v < end; v++) frame.vertices.push(f(r.readFloat() * 0.5));
            }
            if (fi < fc - 1) frame.curve = this.readCurve();
            frames.push(frame);
          }
          timelines.push({ kind: 'deform', skin: skin, slot: slot, attachment: attachment, frames: frames });
        }
      }
    }
    n = r.readVarint(); frames = [];
    for (i = 0; i < n; i++) {
      var offsets = [];
      for (j = 0, count = r.readVarint(); j < count; j++) offsets.push({ slot: this.slots[r.readVarint()].name, offset: r.readVarint() });
      frames.push({ offsets: offsets, time: f(r.readFloat()) });
    }
    if (n) timelines.push({ kind: 'drawOrder', frames: frames });
    n = r.readVarint(); frames = [];
    for (i = 0; i < n; i++) {
      var time = r.readFloat(), event = this.events[r.readVarint()], iv = r.readInt(false), fv = r.readFloat();
      var sv = r.readBool() ? r.readString() : null;
      frame = { time: f(time), name: event.name };
      if (iv) frame.int = iv;
      if (fv) frame.float = f(fv);
      if (sv !== null) frame.string = sv;
      frames.push(frame);
    }
    if (n) timelines.push({ kind: 'event', frames: frames });
    this.animations.push({ name: name, timelines: timelines });
  };

  global.CGSSSkelParser = {
    parse: function (arrayBuffer) {
      var bytes = new Uint8Array(arrayBuffer);
      if (bytes.length < 44 || bytes[0] !== 0x1c) throw new Error('Invalid or incomplete CGSS skeleton header');
      var header = new Reader(arrayBuffer);
      header.pos = 0;
      header.readString();
      var version = header.readString();
      if (header.pos + 9 !== 44) throw new Error('Unsupported CGSS skeleton header');
      if (/^2\.1\./.test(version)) return new PetitParser(arrayBuffer).parse();
      if (/^3\.6\./.test(version)) return new Parser(arrayBuffer).parse();
      throw new Error('Unsupported Spine binary version: ' + version);
    }
  };
})(typeof window !== 'undefined' ? window : globalThis);
