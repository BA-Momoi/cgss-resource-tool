# -*- coding: utf-8 -*-
"""
CGSS Spine 3.6 binary (.skel) -> Spine 3.6 JSON converter.

CGSS skel format differs from stock Spine 3.6 binary in 4 ways:
  1. A 44-byte custom header: 0x1C + 27-byte hash + version string + 9-byte blob.
  2. Floats are BIG-endian (stock runtime uses little-endian).
  3. uint32 colors and int16 shorts are BIG-endian.
  4. The "nonessential" section is absent (no bone colors, no mesh edges/width/height,
     no boundingbox/path/point/clipping colors).

Usage:
    python skel2json.py <in.skel> [out.json]
"""

import struct, sys, json, os

class ParseError(Exception):
    pass

class R:
    def __init__(self, b, pos):
        self.b = b
        self.pos = pos

    def read_byte(self):
        if self.pos >= len(self.b):
            raise ParseError("EOF")
        v = self.b[self.pos]
        self.pos += 1
        return v

    def read_bool(self):
        return self.read_byte() != 0

    def read_varint(self):
        b = self.read_byte()
        result = b & 0x7f
        if b & 0x80:
            b = self.read_byte()
            result |= (b & 0x7f) << 7
            if b & 0x80:
                b = self.read_byte()
                result |= (b & 0x7f) << 14
                if b & 0x80:
                    b = self.read_byte()
                    result |= (b & 0x7f) << 21
                    if b & 0x80:
                        b = self.read_byte()
                        result |= (b & 0x7f) << 28
        return result

    def read_int(self, opt=True):
        result = self.read_varint()
        return result if opt else (result >> 1) ^ -(result & 1)

    def read_uint32(self):
        if self.pos + 4 > len(self.b):
            raise ParseError("EOF in uint32")
        v = struct.unpack_from('>I', self.b, self.pos)[0]
        self.pos += 4
        return v

    def read_float(self):
        if self.pos + 4 > len(self.b):
            raise ParseError("EOF in float")
        v = struct.unpack_from('>f', self.b, self.pos)[0]
        self.pos += 4
        return v

    def read_short(self):
        if self.pos + 2 > len(self.b):
            raise ParseError("EOF in short")
        v = struct.unpack_from('>h', self.b, self.pos)[0]
        self.pos += 2
        return v

    def read_string(self):
        n = self.read_varint()
        if n == 0:
            return None
        if n == 1:
            return ""
        n -= 1
        if self.pos + n > len(self.b):
            raise ParseError("EOF in string")
        s = self.b[self.pos:self.pos + n].decode('utf-8', 'replace')
        self.pos += n
        return s

    def remaining(self):
        return len(self.b) - self.pos


def rgba8888(v):
    return [(v >> 24) & 0xff, (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff]


def rgb888(v):
    return [(v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff]


def rgba_hex(v):
    r, g, b, a = rgba8888(v)
    return "%02x%02x%02x%02x" % (r, g, b, a)


def rgb_hex(v):
    r, g, b = rgb888(v)
    return "%02x%02x%02x" % (r, g, b)


def f(x):
    """Shortest round-trippable float representation."""
    if x == 0:
        return 0
    if x != x:
        return 0
    return round(x, 6)


TRANSFORM_NAMES = ["normal", "onlyTranslation", "noRotationOrReflection", "noScale", "noScaleOrReflection"]
BLEND_NAMES = ["normal", "additive", "multiply", "screen"]
POSITION_MODES = ["fixed", "percent"]
SPACING_MODES = ["length", "fixed", "percent"]
ROTATE_MODES = ["tangent", "chain", "chainScale"]


class Parser:
    def __init__(self, b, version="3.6.47"):
        self.r = R(b, 44)
        self.scale = 1.0
        self.bones = []
        self.slots = []
        self.ik = []
        self.transform = []
        self.path = []
        self.skins = []
        self.events = []
        self.animations = []
        self.version = version

    def parse(self):
        r = self.r
        # header: version string already skipped by offset 44
        n = r.read_varint()
        for i in range(n):
            name = r.read_string()
            parent = None if i == 0 else r.read_varint()
            rot = r.read_float()
            x = r.read_float()
            y = r.read_float()
            sx = r.read_float()
            sy = r.read_float()
            shx = r.read_float()
            shy = r.read_float()
            length = r.read_float()
            tm = r.read_varint()
            self.bones.append({
                "name": name,
                "parent": None if parent is None else self.bones[parent]["name"],
                "rotation": rot, "x": x, "y": y,
                "scaleX": sx, "scaleY": sy, "shearX": shx, "shearY": shy,
                "length": length,
                "transform": TRANSFORM_NAMES[tm],
            })

        n = r.read_varint()
        for i in range(n):
            slot = {"name": r.read_string(), "bone": self.bones[r.read_varint()]["name"]}
            c = r.read_uint32()
            slot["color"] = rgba_hex(c)
            dark = r.read_uint32()
            if dark != 0xffffffff:
                slot["dark"] = rgb_hex(dark)
            att = r.read_string()
            if att is not None:
                slot["attachment"] = att
            slot["blend"] = BLEND_NAMES[r.read_varint()]
            self.slots.append(slot)

        n = r.read_varint()
        for i in range(n):
            d = {"name": r.read_string(), "order": r.read_varint(), "bones": [], "target": None}
            for _ in range(r.read_varint()):
                d["bones"].append(self.bones[r.read_varint()]["name"])
            d["target"] = self.bones[r.read_varint()]["name"]
            d["mix"] = r.read_float()
            d["bendPositive"] = (r.read_byte() == 1)
            self.ik.append(d)

        n = r.read_varint()
        for i in range(n):
            d = {"name": r.read_string(), "order": r.read_varint(), "bones": [], "target": None}
            for _ in range(r.read_varint()):
                d["bones"].append(self.bones[r.read_varint()]["name"])
            d["target"] = self.bones[r.read_varint()]["name"]
            d["local"] = r.read_bool()
            d["relative"] = r.read_bool()
            d["rotation"] = r.read_float()
            d["x"] = r.read_float()
            d["y"] = r.read_float()
            d["scaleX"] = r.read_float()
            d["scaleY"] = r.read_float()
            d["shearY"] = r.read_float()
            d["rotateMix"] = r.read_float()
            d["translateMix"] = r.read_float()
            d["scaleMix"] = r.read_float()
            d["shearMix"] = r.read_float()
            self.transform.append(d)

        n = r.read_varint()
        for i in range(n):
            d = {"name": r.read_string(), "order": r.read_varint(), "bones": [], "target": None}
            for _ in range(r.read_varint()):
                d["bones"].append(self.bones[r.read_varint()]["name"])
            d["target"] = self.slots[r.read_varint()]["name"]
            d["positionMode"] = POSITION_MODES[r.read_varint()]
            d["spacingMode"] = SPACING_MODES[r.read_varint()]
            d["rotateMode"] = ROTATE_MODES[r.read_varint()]
            d["rotation"] = r.read_float()
            d["position"] = r.read_float()
            d["spacing"] = r.read_float()
            d["rotateMix"] = r.read_float()
            d["translateMix"] = r.read_float()
            self.path.append(d)

        # default skin
        skin = self.read_skin("default")
        if skin is not None:
            self.skins.append(skin)
        n = r.read_varint()
        for i in range(n):
            skin = self.read_skin(r.read_string())
            if skin is not None:
                self.skins.append(skin)

        n = r.read_varint()
        for i in range(n):
            d = {"name": r.read_string()}
            d["int"] = r.read_int(False)
            d["float"] = r.read_float()
            d["string"] = r.read_string() or ""
            self.events.append(d)

        n = r.read_varint()
        for i in range(n):
            self.read_animation(r.read_string())

        if r.remaining() != 0:
            raise ParseError("trailing bytes: %d" % r.remaining())
        return self

    def read_skin(self, skin_name):
        r = self.r
        slot_count = r.read_varint()
        if slot_count == 0:
            return None
        skin = {"name": skin_name, "attachments": {}}
        for i in range(slot_count):
            slot_idx = r.read_varint()
            slot_name = self.slots[slot_idx]["name"]
            atts = {}
            for _ in range(r.read_varint()):
                name = r.read_string()
                att = self.read_attachment(slot_idx, name)
                if att is not None:
                    atts[name] = att
            skin["attachments"][slot_name] = atts
        return skin

    def read_attachment(self, slot_idx, attachment_name):
        r = self.r
        name = r.read_string()
        if name is None:
            name = attachment_name
        atype = r.read_byte()
        if atype == 0:  # region
            path = r.read_string()
            if path is None:
                path = name
            att = {"type": "region", "name": name, "path": path,
                   "rotation": r.read_float(), "x": r.read_float(), "y": r.read_float(),
                   "scaleX": r.read_float(), "scaleY": r.read_float(),
                   "width": r.read_float(), "height": r.read_float()}
            c = r.read_uint32()
            att["color"] = rgba_hex(c)
            return att
        if atype == 1:  # bounding box
            vc = r.read_varint()
            vertices, bones = self.read_vertices(vc)
            return {"type": "boundingbox", "name": name, "vertexCount": vc,
                    "vertices": vertices}
        if atype == 2:  # mesh
            path = r.read_string()
            if path is None:
                path = name
            c = r.read_uint32()
            vc = r.read_varint()
            uvs = [f(r.read_float()) for _ in range(vc * 2)]
            tris = [r.read_short() for _ in range(r.read_varint())]
            vertices, bones = self.read_vertices(vc)
            hull = r.read_varint()
            return {"type": "mesh", "name": name, "path": path,
                    "uvs": uvs, "triangles": tris, "vertices": vertices,
                    "hull": hull,
                    "color": rgba_hex(c)}
        if atype == 3:  # linked mesh
            path = r.read_string()
            if path is None:
                path = name
            c = r.read_uint32()
            skin_name = r.read_string()
            parent = r.read_string()
            inherit = r.read_bool()
            att = {"type": "linkedmesh", "name": name, "path": path,
                   "parent": parent, "deform": inherit, "color": rgba_hex(c)}
            if skin_name is not None:
                att["skin"] = skin_name
            return att
        if atype == 4:  # path
            closed = r.read_bool()
            cs = r.read_bool()
            vc = r.read_varint()
            vertices, bones = self.read_vertices(vc)
            lengths = [f(r.read_float()) for _ in range(vc // 3)]
            return {"type": "path", "name": name, "closed": closed, "constantSpeed": cs,
                    "vertexCount": vc, "vertices": vertices, "lengths": lengths}
        if atype == 5:  # point
            return {"type": "point", "name": name, "rotation": r.read_float(),
                    "x": r.read_float(), "y": r.read_float()}
        if atype == 6:  # clipping
            end_slot = r.read_varint()
            vc = r.read_varint()
            vertices, bones = self.read_vertices(vc)
            return {"type": "clipping", "name": name,
                    "end": self.slots[end_slot]["name"],
                    "vertexCount": vc, "vertices": vertices}
        raise ParseError("unknown attachment type %d" % atype)

    def read_vertices(self, vc):
        """Returns (json_vertices, is_weighted)."""
        r = self.r
        if not r.read_bool():
            return [f(r.read_float()) for _ in range(vc * 2)], False
        out = []
        for i in range(vc):
            bc = r.read_varint()
            out.append(bc)
            for _ in range(bc):
                out.append(r.read_varint())
                out.append(f(r.read_float()))
                out.append(f(r.read_float()))
                out.append(f(r.read_float()))
        return out, True

    def read_animation(self, name):
        r = self.r
        timelines = []
        # slot timelines
        n = r.read_varint()
        for i in range(n):
            slot_idx = r.read_varint()
            for _ in range(r.read_varint()):
                ttype = r.read_byte()
                fc = r.read_varint()
                if ttype == 0:  # attachment
                    frames = [{"time": f(r.read_float()), "name": r.read_string()} for _ in range(fc)]
                    timelines.append({"kind": "slot", "slot": slot_idx, "type": "attachment", "frames": frames})
                elif ttype == 1:  # color
                    frames = []
                    for fi in range(fc):
                        t = r.read_float()
                        col = rgba_hex(r.read_uint32())
                        frame = {"time": f(t), "color": col}
                        if fi < fc - 1:
                            frame["curve"] = self.read_curve()
                        frames.append(frame)
                    timelines.append({"kind": "slot", "slot": slot_idx, "type": "color", "frames": frames})
                elif ttype == 2:  # two color
                    frames = []
                    for fi in range(fc):
                        t = r.read_float()
                        light = rgba_hex(r.read_uint32())
                        dark = rgb_hex(r.read_uint32())
                        frame = {"time": f(t), "light": light, "dark": dark}
                        if fi < fc - 1:
                            frame["curve"] = self.read_curve()
                        frames.append(frame)
                    timelines.append({"kind": "slot", "slot": slot_idx, "type": "twoColor", "frames": frames})
                else:
                    raise ParseError("unknown slot timeline type %d" % ttype)

        # bone timelines
        n = r.read_varint()
        for i in range(n):
            bone_idx = r.read_varint()
            for _ in range(r.read_varint()):
                ttype = r.read_byte()
                fc = r.read_varint()
                frames = []
                for fi in range(fc):
                    if ttype == 0:  # rotate
                        frame = {"time": f(r.read_float()), "angle": f(r.read_float())}
                    else:
                        t = r.read_float()
                        x = r.read_float()
                        y = r.read_float()
                        frame = {"time": f(t), "x": f(x), "y": f(y)}
                    if fi < fc - 1:
                        frame["curve"] = self.read_curve()
                    frames.append(frame)
                tname = {0: "rotate", 1: "translate", 2: "scale", 3: "shear"}[ttype]
                timelines.append({"kind": "bone", "bone": bone_idx, "type": tname, "frames": frames})

        # ik timelines
        n = r.read_varint()
        for i in range(n):
            idx = r.read_varint()
            fc = r.read_varint()
            frames = []
            for fi in range(fc):
                frame = {"time": f(r.read_float()), "mix": f(r.read_float()),
                         "bendPositive": (r.read_byte() == 1)}
                if fi < fc - 1:
                    frame["curve"] = self.read_curve()
                frames.append(frame)
            timelines.append({"kind": "ik", "index": idx, "frames": frames})

        # transform timelines
        n = r.read_varint()
        for i in range(n):
            idx = r.read_varint()
            fc = r.read_varint()
            frames = []
            for fi in range(fc):
                frame = {"time": f(r.read_float()),
                         "rotateMix": f(r.read_float()),
                         "translateMix": f(r.read_float()),
                         "scaleMix": f(r.read_float()),
                         "shearMix": f(r.read_float())}
                if fi < fc - 1:
                    frame["curve"] = self.read_curve()
                frames.append(frame)
            timelines.append({"kind": "transform", "index": idx, "frames": frames})

        # path timelines
        n = r.read_varint()
        for i in range(n):
            idx = r.read_varint()
            for _ in range(r.read_varint()):
                ttype = r.read_byte()
                fc = r.read_varint()
                frames = []
                for fi in range(fc):
                    if ttype in (0, 1):
                        frame = {"time": f(r.read_float()),
                                 ("position" if ttype == 0 else "spacing"): f(r.read_float())}
                    else:
                        frame = {"time": f(r.read_float()),
                                 "rotateMix": f(r.read_float()),
                                 "translateMix": f(r.read_float())}
                    if fi < fc - 1:
                        frame["curve"] = self.read_curve()
                    frames.append(frame)
                timelines.append({"kind": "path", "index": idx,
                                  "type": ["position", "spacing", "mix"][ttype], "frames": frames})

        # deform timelines
        n = r.read_varint()
        for i in range(n):
            skin_idx = r.read_varint()
            skin_name = self.skins[skin_idx]["name"]
            for _ in range(r.read_varint()):
                slot_idx = r.read_varint()
                for _ in range(r.read_varint()):
                    att_name = r.read_string()
                    fc = r.read_varint()
                    frames = []
                    for fi in range(fc):
                        t = r.read_float()
                        end = r.read_varint()
                        if end == 0:
                            frame = {"time": f(t)}
                        else:
                            start = r.read_varint()
                            end += start
                            verts = [f(r.read_float()) for _ in range(end - start)]
                            frame = {"time": f(t), "offset": start, "vertices": verts}
                        if fi < fc - 1:
                            frame["curve"] = self.read_curve()
                        frames.append(frame)
                    timelines.append({"kind": "deform", "skin": skin_name, "slot": slot_idx,
                                      "attachment": att_name, "frames": frames})

        # draw order
        n = r.read_varint()
        if n > 0:
            frames = []
            for i in range(n):
                t = r.read_float()
                oc = r.read_varint()
                offsets = []
                for _ in range(oc):
                    si = r.read_varint()
                    so = r.read_varint()
                    if so >= 0x80000000:
                        so -= 0x100000000      # drawOrder offset 是有符号 int32
                    offsets.append({"slot": self.slots[si]["name"], "offset": so})
                frames.append({"time": f(t), "offsets": offsets})
            timelines.append({"kind": "drawOrder", "frames": frames})

        # events
        n = r.read_varint()
        if n > 0:
            frames = []
            for i in range(n):
                t = r.read_float()
                ev = self.events[r.read_varint()]
                iv = r.read_int(False)
                fv = r.read_float()
                sv = r.read_string() if r.read_bool() else None
                frame = {"time": f(t), "name": ev["name"]}
                if iv != 0:
                    frame["int"] = iv
                if fv != 0:
                    frame["float"] = fv
                if sv is not None:
                    frame["string"] = sv
                frames.append(frame)
            timelines.append({"kind": "event", "frames": frames})

        self.animations.append({"name": name, "timelines": timelines})

    def read_curve(self):
        r = self.r
        t = r.read_byte()
        if t == 1:
            return "stepped"
        if t == 2:
            return [f(r.read_float()), f(r.read_float()), f(r.read_float()), f(r.read_float())]
        return None


def to_json(p):
    root = {
        "skeleton": {
            "hash": "cgss",
            "spine": p.version,
            "width": 0,
            "height": 0,
            "fps": 30,
            "images": "",
            "audio": "",
        },
        "bones": [],
        "slots": [],
        "skins": [],
        "animations": {},
    }
    for b in p.bones:
        d = {"name": b["name"]}
        if b["parent"] is not None:
            d["parent"] = b["parent"]
        for k in ("length", "x", "y", "rotation", "scaleX", "scaleY", "shearX", "shearY"):
            v = b[k]
            if v not in (0, 1) or (k in ("scaleX", "scaleY") and v != 1):
                d[k] = f(v)
        if b["transform"] != "normal":
            d["transform"] = b["transform"]
        root["bones"].append(d)
    root["slots"] = [dict(s) for s in p.slots]
    if p.ik:
        root["ik"] = p.ik
    if p.transform:
        root["transform"] = p.transform
    if p.path:
        root["path"] = p.path
    skins_obj = {}
    for s in p.skins:
        skins_obj[s["name"]] = s["attachments"]
    root["skins"] = skins_obj
    if p.events:
        ev_obj = {}
        for e in p.events:
            m = {}
            if e["int"] != 0:
                m["int"] = e["int"]
            if e["float"] != 0:
                m["float"] = f(e["float"])
            if e["string"]:
                m["string"] = e["string"]
            ev_obj[e["name"]] = m
        root["events"] = ev_obj

    for anim in p.animations:
        a = {}
        for tl in anim["timelines"]:
            if tl["kind"] == "slot":
                slot_name = p.slots[tl["slot"]]["name"]
                a.setdefault("slots", {}).setdefault(slot_name, {})[tl["type"]] = tl["frames"]
            elif tl["kind"] == "bone":
                bone_name = p.bones[tl["bone"]]["name"]
                a.setdefault("bones", {}).setdefault(bone_name, {})[tl["type"]] = tl["frames"]
            elif tl["kind"] == "ik":
                a.setdefault("ik", {})[p.ik[tl["index"]]["name"]] = tl["frames"]
            elif tl["kind"] == "transform":
                a.setdefault("transform", {})[p.transform[tl["index"]]["name"]] = tl["frames"]
            elif tl["kind"] == "path":
                a.setdefault("paths", {}).setdefault(p.path[tl["index"]]["name"], {})[tl["type"]] = tl["frames"]
            elif tl["kind"] == "deform":
                a.setdefault("deform", {}).setdefault(tl["skin"], {}).setdefault(
                    p.slots[tl["slot"]]["name"], {})[tl["attachment"]] = tl["frames"]
            elif tl["kind"] == "drawOrder":
                a["drawOrder"] = tl["frames"]
            elif tl["kind"] == "event":
                a["events"] = tl["frames"]
        root["animations"][anim["name"]] = a
    return root


def convert(src, dst):
    convert_ver(src, dst, "3.6.47")

def convert_ver(src, dst, version):
    b = open(src, 'rb').read()
    p = Parser(b, version)
    p.parse()
    root = to_json(p)
    with open(dst, 'w', encoding='utf-8') as fp:
        json.dump(root, fp, ensure_ascii=False, separators=(",", ":"))
    print("%s -> %s  (bones=%d slots=%d skins=%d anims=%s)"
          % (os.path.basename(src), os.path.basename(dst),
             len(p.bones), len(p.slots), len(p.skins),
             [a["name"] for a in p.animations]))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    src = sys.argv[1]
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    v38 = "--v38" in sys.argv[1:]
    src = args[0]
    dst = args[1] if len(args) > 1 else os.path.splitext(src)[0] + (".json" if not v38 else "_v38.json")
    convert_ver(src, dst, "3.8.75" if v38 else "3.6.47")
