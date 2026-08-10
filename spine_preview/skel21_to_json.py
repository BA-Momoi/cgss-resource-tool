# -*- coding: utf-8 -*-
"""
CGSS Spine 2.1 binary (.skel) -> Spine 3.6 JSON converter.

Spine 2.1 skeleton data (e.g. the shared petit skeleton
spine_sprachen_petit_chara_common.unity3d -> SPSprachen_s.skel) uses the
same CGSS custom 44-byte header, but the binary layout after the header is
the Spine 2.1 format, which differs from 3.6:
  - bones: parent is stored for every bone (index+1); fields are
    x, y, scaleX, scaleY, rotation, length, flipX, flipY,
    inheritScale, inheritRotation (no shear / transform mode);
  - slots: no dark color, "additiveBlending" boolean instead of blend enum;
  - attachments: region/boundingbox/mesh/skinnedmesh only
    (skinnedmesh stores boneCount as float);
  - animations: color=4, attachment=3, rotate=1, translate=2, scale=0,
    flipX=5, flipY=6; no two-color / shear / transform / path timelines.

Output JSON matches the 3.6 converter (skel2json.py) so the browser preview
and the Spine 3.8.75 editor can load it directly.
"""

import struct, sys, json, os
from skel2json import R, rgba_hex, rgb_hex, f, to_json, ParseError


class Parser21:
    def __init__(self, b, version="2.1.27", scale=1.0):
        self.r = R(b, 44)
        self.scale = scale
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
        n = r.read_varint()
        for i in range(n):
            name = r.read_string()
            pi = r.read_varint() - 1          # 2.1: parent stored for every bone
            parent = None if pi < 0 else pi
            x = r.read_float() * self.scale
            y = r.read_float() * self.scale
            sx = r.read_float()
            sy = r.read_float()
            rot = r.read_float()
            length = r.read_float() * self.scale
            flipX = r.read_bool()
            flipY = r.read_bool()
            inheritScale = r.read_bool()
            inheritRotation = r.read_bool()
            if flipX:
                sx = -sx
            if flipY:
                sy = -sy
            self.bones.append({
                "name": name,
                "parent": None if parent is None else self.bones[parent]["name"],
                "rotation": rot, "x": x, "y": y,
                "scaleX": sx, "scaleY": sy,
                "shearX": 0, "shearY": 0,
                "length": length,
                "transform": "normal",
            })

        # 2.1 order: bones -> IK constraints -> slots
        n = r.read_varint()
        for i in range(n):
            d = {"name": r.read_string(), "bones": [], "target": None}
            for _ in range(r.read_varint()):
                d["bones"].append(self.bones[r.read_varint()]["name"])
            d["target"] = self.bones[r.read_varint()]["name"]
            d["mix"] = f(r.read_float())
            d["bendPositive"] = (r.read_byte() == 1)
            self.ik.append(d)

        n = r.read_varint()
        for i in range(n):
            slot = {"name": r.read_string(), "bone": self.bones[r.read_varint()]["name"]}
            slot["color"] = rgba_hex(r.read_uint32())
            att = r.read_string()
            if att is not None:
                slot["attachment"] = att
            slot["blend"] = "additive" if r.read_bool() else "normal"
            self.slots.append(slot)

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
            d["float"] = f(r.read_float())
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
            # 2.1 order: x, y, scaleX, scaleY, rotation, width, height
            att = {"type": "region", "name": name, "path": path,
                   "x": f(r.read_float() * self.scale),
                   "y": f(r.read_float() * self.scale),
                   "scaleX": f(r.read_float()), "scaleY": f(r.read_float()),
                   "rotation": f(r.read_float()),
                   "width": f(r.read_float() * self.scale),
                   "height": f(r.read_float() * self.scale)}
            att["color"] = rgba_hex(r.read_uint32())
            return att
        if atype == 1:  # bounding box (unweighted vertices)
            vc = r.read_varint()
            verts = [f(r.read_float() * self.scale) for _ in range(vc)]
            return {"type": "boundingbox", "name": name, "vertexCount": vc,
                    "vertices": verts}
        if atype == 2:  # mesh (unweighted)
            path = r.read_string()
            if path is None:
                path = name
            c = r.read_uint32()
            uvs = [f(r.read_float()) for _ in range(r.read_varint())]
            tris = [r.read_short() for _ in range(r.read_varint())]
            verts = [f(r.read_float() * self.scale) for _ in range(r.read_varint())]
            hull = r.read_varint()
            return {"type": "mesh", "name": name, "path": path,
                    "uvs": uvs, "triangles": tris, "vertices": verts,
                    "hull": hull, "color": rgba_hex(c)}
        if atype == 3:  # skinned mesh -> weighted
            path = r.read_string()
            if path is None:
                path = name
            c = r.read_uint32()
            uvs = [f(r.read_float()) for _ in range(r.read_varint())]
            tris = [r.read_short() for _ in range(r.read_varint())]
            vc = r.read_varint()
            verts = []
            for i in range(vc):
                bc = int(r.read_float())       # 2.1 stores boneCount as float
                verts.append(bc)
                for _ in range(bc):
                    verts.append(int(r.read_float()))
                    verts.append(f(r.read_float() * self.scale))
                    verts.append(f(r.read_float() * self.scale))
                    verts.append(f(r.read_float()))
            hull = r.read_varint()
            return {"type": "weightedmesh", "name": name, "path": path,
                    "uvs": uvs, "triangles": tris, "vertices": verts,
                    "hull": hull, "color": rgba_hex(c)}
        raise ParseError("unknown 2.1 attachment type %d" % atype)

    def read_animation(self, name):
        r = self.r
        timelines = []
        # slot timelines: type 4 = color, 3 = attachment
        n = r.read_varint()
        for i in range(n):
            slot_idx = r.read_varint()
            for _ in range(r.read_varint()):
                ttype = r.read_byte()
                fc = r.read_varint()
                if ttype == 4:  # color
                    frames = []
                    for fi in range(fc):
                        t = r.read_float()
                        col = rgba_hex(r.read_uint32())
                        frame = {"time": f(t), "color": col}
                        if fi < fc - 1:
                            frame["curve"] = self.read_curve()
                        frames.append(frame)
                    timelines.append({"kind": "slot", "slot": slot_idx,
                                      "type": "color", "frames": frames})
                elif ttype == 3:  # attachment
                    frames = [{"time": f(r.read_float()), "name": r.read_string()}
                              for _ in range(fc)]
                    timelines.append({"kind": "slot", "slot": slot_idx,
                                      "type": "attachment", "frames": frames})
                else:
                    raise ParseError("unknown 2.1 slot timeline type %d" % ttype)

        # bone timelines: 1 = rotate, 2 = translate, 0 = scale, 5/6 = flipX/flipY
        n = r.read_varint()
        for i in range(n):
            bone_idx = r.read_varint()
            for _ in range(r.read_varint()):
                ttype = r.read_byte()
                fc = r.read_varint()
                if ttype in (5, 6):  # flip timelines: not representable in 3.6 JSON
                    for _ in range(fc):
                        r.read_float()
                        r.read_bool()
                    continue
                frames = []
                for fi in range(fc):
                    if ttype == 1:  # rotate
                        frame = {"time": f(r.read_float()), "angle": f(r.read_float())}
                    elif ttype in (0, 2):  # scale / translate
                        t = r.read_float()
                        x = r.read_float() * (self.scale if ttype == 2 else 1.0)
                        y = r.read_float() * (self.scale if ttype == 2 else 1.0)
                        frame = {"time": f(t), "x": f(x), "y": f(y)}
                    else:
                        raise ParseError("unknown 2.1 bone timeline type %d" % ttype)
                    if fi < fc - 1:
                        frame["curve"] = self.read_curve()
                    frames.append(frame)
                tname = {1: "rotate", 2: "translate", 0: "scale"}[ttype]
                if frames:
                    timelines.append({"kind": "bone", "bone": bone_idx,
                                      "type": tname, "frames": frames})

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

        # deform (ffd) timelines
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
                            verts = [f(r.read_float() * self.scale) for _ in range(end - start)]
                            frame = {"time": f(t), "offset": start, "vertices": verts}
                        if fi < fc - 1:
                            frame["curve"] = self.read_curve()
                        frames.append(frame)
                    timelines.append({"kind": "deform", "skin": skin_name,
                                      "slot": slot_idx, "attachment": att_name,
                                      "frames": frames})

        # draw order
        n = r.read_varint()
        if n > 0:
            frames = []
            for i in range(n):
                # 2.1: offsetCount + offsets 在前，time 在最后（3.6 相反）
                oc = r.read_varint()
                offsets = []
                for _ in range(oc):
                    si = r.read_varint()
                    so = r.read_varint()
                    if so >= 0x80000000:
                        so -= 0x100000000      # drawOrder offset 是有符号 int32
                    offsets.append({"slot": self.slots[si]["name"], "offset": so})
                t = r.read_float()
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
                    frame["float"] = f(fv)
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


def convert_ver(src, dst, version="2.1.27", scale=1.0):
    b = open(src, 'rb').read()
    p = Parser21(b, version, scale)
    p.parse()
    root = to_json(p)
    with open(dst, 'w', encoding='utf-8') as fp:
        json.dump(root, fp, ensure_ascii=False, separators=(",", ":"))
    print("%s -> %s  (bones=%d slots=%d skins=%d anims=%s)"
          % (os.path.basename(src), os.path.basename(dst),
             len(p.bones), len(p.slots), len(p.skins),
             [a["name"] for a in p.animations]))


def main():
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else src.replace(".skel", ".json")
    convert_ver(src, dst)


if __name__ == '__main__':
    main()
