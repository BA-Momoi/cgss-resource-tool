# -*- coding: utf-8 -*-
# cgss_apply_textures.py
# 在 Blender 中给已导入的 CGSS FBX 自动贴图，并把所有材质 BSDF 糙度设为 1。
# 用法：导入 FBX 后，Scripting 面板打开本脚本，点 Run Script。
# 结果会弹窗显示；找不到贴图目录也会弹窗提示改 TEXDIR。
import bpy
import os
import glob
import traceback

# 手动指定贴图文件夹（留空则自动搜索）
TEXDIR = r''
# 搜索过的目录（诊断用）
SEARCHED_PATHS = []


def show_popup(title, msg):
    if bpy.app.background:
        print('[%s] %s' % (title, msg))
        return
    lines = msg.split('\n')
    def draw(self, context):
        for line in lines:
            self.layout.label(text=line)
    bpy.context.window_manager.popup_menu(draw, title=title, icon='INFO')


def script_dir():
    # 从 Text Editor 打开的脚本文件拿到它的所在目录（不写死路径，换电脑也能用）
    for t in bpy.data.texts:
        if 'cgss_apply_textures' in t.name and t.filepath:
            d = os.path.dirname(bpy.path.abspath(t.filepath))
            if os.path.isdir(d):
                return d
    for t in bpy.data.texts:
        if t.filepath:
            d = os.path.dirname(bpy.path.abspath(t.filepath))
            if os.path.isdir(d):
                return d
    return None


def find_texdir():
    if TEXDIR and os.path.isdir(TEXDIR):
        return TEXDIR
    cands = []
    sd = script_dir()
    if sd:
        cands += [sd, os.path.join(sd, 'CGSS_DOWN'), os.path.join(sd, 'fbx\u5bfc\u51fa\u5bf9\u6bd4')]
    if bpy.data.filepath:
        cands.append(os.path.dirname(bpy.data.filepath))
    cands.append(os.getcwd())
    SEARCHED_PATHS[:] = [c for c in cands if c]
    for c in cands:
        if not c or not os.path.isdir(c):
            continue
        try:
            has_png = any(f.lower().endswith('.png') for f in os.listdir(c))
        except OSError:
            has_png = False
        if has_png:
            return c
        pats = [os.path.join(c, 'CGSS_DOWN', '*', '3d\u6a21\u578b', '\u8d34\u56fe'),
                os.path.join(c, 'fbx\u5bfc\u51fa\u5bf9\u6bd4'),
                os.path.join(c, '**', '\u8d34\u56fe')]
        for pat in pats:
            hits = glob.glob(pat, recursive=True) if '**' in pat else glob.glob(pat)
            for h in hits:
                if os.path.isdir(h):
                    try:
                        ok = any(f.lower().endswith('.png') for f in os.listdir(h))
                    except OSError:
                        ok = False
                    if ok:
                        return h
    return None


def pick(texdir, keys):
    cands = []
    for fn in sorted(os.listdir(texdir)):
        n = fn.lower()
        if n.endswith('.png') and all(k in n for k in keys):
            cands.append(fn)
    for fn in cands:
        if '_obj_' not in fn.lower():
            return os.path.join(texdir, fn)
    if cands:
        return os.path.join(texdir, cands[0])
    return None


def set_all_roughness():
    # 强制糙度=1：先断开 Roughness 输入上的所有连接（否则 default_value 不生效）
    n = 0
    for mat in bpy.data.materials:
        mat.use_nodes = True
        tree = mat.node_tree
        bsdf = tree.nodes.get('Principled BSDF')
        if not bsdf:
            continue
        if 'Roughness' in bsdf.inputs:
            inp = bsdf.inputs['Roughness']
            for link in list(inp.links):
                tree.links.remove(link)
            inp.default_value = 1.0
            n += 1
    return n


def clear_tex_nodes(mat):
    # 删掉材质里已有的贴图节点，保证重复运行是重贴而不是跳过
    tree = mat.node_tree
    for node in list(tree.nodes):
        if node.type == 'TEX_IMAGE':
            tree.nodes.remove(node)


def pick_image(keys):
    # 优先用场景里已加载的图片（GUI 导出的 FBX 带贴图引用，导入时已加载）
    cands = [img for img in bpy.data.images
             if all(k in img.name.lower() for k in keys)]
    png = [img for img in cands if img.name.lower().endswith('.png')]
    cands = png or cands
    for img in cands:
        if '_obj_' not in img.name.lower():
            return img
    if cands:
        return cands[0]
    return None


def main():
    # 糙度=1 永远执行，不依赖贴图目录是否找到
    rough_n = set_all_roughness()

    texdir = find_texdir()
    lines = []
    lines.append('糙度=1 已设置到 %d 个材质' % rough_n)
    if texdir:
        lines.append('贴图目录：%s' % texdir)
    else:
        lines.append('贴图目录：未找到')
    mats = list(bpy.data.materials)
    lines.append('场景材质数量：%d' % len(mats))
    if not texdir:
        lines.append('')
        lines.append('搜索过的目录：')
        for p in SEARCHED_PATHS:
            lines.append('  ' + p)
        lines.append('请把贴图文件夹放到这些目录之一，')
        lines.append('或在脚本开头修改 TEXDIR 手动指定。')
        msg = '\n'.join(lines)
        print(msg)
        show_popup('没有贴图目录', msg)
        return

    mat_count = 0
    slot_count = 0
    for mat in bpy.data.materials:
        match_info = []
        mat.use_nodes = True
        clear_tex_nodes(mat)
        bsdf = mat.node_tree.nodes.get('Principled BSDF')
        if not bsdf:
            lines.append('  [%s] 无 Principled BSDF，跳过' % mat.name)
            continue
        # 镜面输入名随 Blender 版本变化：4.x 叫 Specular IOR Level，旧版叫 Specular
        spec_key = None
        for cand in ('Specular IOR Level', 'Specular'):
            if cand in bsdf.inputs:
                spec_key = cand
                break
        m = mat.name.lower()
        # 腮红：贴图带透明通道，同时连 Base Color 和 Alpha，并启用透明混合
        if 'cheek' in m:
            img = pick_image(['cheek'])
            if not img and texdir:
                png = pick(texdir, ['cheek'])
                if png:
                    img = bpy.data.images.load(png, check_existing=True)
            if img:
                tex = mat.node_tree.nodes.new('ShaderNodeTexImage')
                tex.image = img
                mat.node_tree.links.new(tex.outputs['Color'], bsdf.inputs['Base Color'])
                if 'Alpha' in bsdf.inputs:
                    mat.node_tree.links.new(tex.outputs['Alpha'], bsdf.inputs['Alpha'])
                mat.blend_method = 'BLEND'
                mat.show_transparent_back = True
                mat_count += 1
                slot_count += 2
                match_info.append('cheek -> %s(+Alpha)' % img.name)
            else:
                match_info.append('cheek 未找到贴图')
            lines.append('  [%s] %s' % (mat.name, '; '.join(match_info) or '无'))
            continue
        slots = []
        if m.startswith('m_body') or 'mt_body' in m:
            img = pick_image(['body', '_hq']) or pick_image(['body'])
            if not img and texdir:
                png = pick(texdir, ['body', '_hq']) or pick(texdir, ['body'])
                if png:
                    img = bpy.data.images.load(png, check_existing=True)
            if img:
                slots.append(('Base Color', img))
                match_info.append('body -> %s' % img.name)
            img = pick_image(['body', '_spec'])
            if not img and texdir:
                png = pick(texdir, ['body', '_spec'])
                if png:
                    img = bpy.data.images.load(png, check_existing=True)
            if img:
                slots.append(('Specular', img))
                match_info.append('spec -> %s' % img.name)
        elif m.startswith('m_head') or m.startswith('m_cheek') or 'mt_chr' in m:
            img = pick_image(['chr', '_hq']) or pick_image(['chr'])
            if not img and texdir:
                png = pick(texdir, ['chr', '_hq']) or pick(texdir, ['chr'])
                if png:
                    img = bpy.data.images.load(png, check_existing=True)
            if img:
                slots.append(('Base Color', img))
                match_info.append('head -> %s' % img.name)
            img = pick_image(['chr', '_spec'])
            if not img and texdir:
                png = pick(texdir, ['chr', '_spec'])
                if png:
                    img = bpy.data.images.load(png, check_existing=True)
            if img:
                slots.append(('Specular', img))
                match_info.append('spec -> %s' % img.name)
        if not slots:
            lines.append('  [%s] 材质名没匹配到规则，跳过' % mat.name)
            continue
        for slot, img in slots:
            tex = mat.node_tree.nodes.new('ShaderNodeTexImage')
            tex.image = img
            if slot == 'Base Color':
                mat.node_tree.links.new(tex.outputs['Color'], bsdf.inputs['Base Color'])
            elif slot == 'Specular' and spec_key:
                mat.node_tree.links.new(tex.outputs['Color'], bsdf.inputs[spec_key])
            slot_count += 1
        mat_count += 1
        lines.append('  [%s] %s' % (mat.name, '; '.join(match_info) or '无'))

    msg = ('贴图完成：%d 个材质，%d 张贴图\n设置糙度=1 的材质：%d 个\n\n详细：\n%s' % (
        mat_count, slot_count, rough_n, '\n'.join(lines)))
    print(msg)
    show_popup('贴图完成', msg)


try:
    main()
except Exception:
    err = traceback.format_exc()
    print(err)
    show_popup('脚本出错', err[-500:])
