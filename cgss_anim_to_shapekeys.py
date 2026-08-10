# -*- coding: utf-8 -*-
# cgss_anim_to_shapekeys.py
# 把 CGSS head FBX 里的骨骼表情动作（AnimationStack）烘焙成形态键（Shape Keys / Blend Shapes）。
# 用法：Blender 里 Scripting 面板打开本脚本，点 Run Script 即可。
#  - 场景里已有 head FBX（带动作）就直接处理；
#  - 没有的话会自动搜索并导入（见 FBX_PATH / 自动搜索路径）。
# 结果会弹窗显示，无需看系统控制台。
import bpy
import os
import glob
import traceback

# ===== 配置 =====
# 手动指定带动作的 FBX（留空则自动搜索）
FBX_PATH = r''
# 只处理名字含此串的动作（留空=全部动作）。默认 an_chr = 角色的 face 动作
ACTION_FILTER = 'an_chr'
# ================


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
        if 'cgss_anim_to_shapekeys' in t.name and t.filepath:
            d = os.path.dirname(bpy.path.abspath(t.filepath))
            if os.path.isdir(d):
                return d
    for t in bpy.data.texts:
        if t.filepath:
            d = os.path.dirname(bpy.path.abspath(t.filepath))
            if os.path.isdir(d):
                return d
    return None


def search_dirs():
    dirs = []
    sd = script_dir()
    if sd:
        dirs += [sd, os.path.join(sd, 'CGSS_DOWN'), os.path.join(sd, 'fbx\u5bfc\u51fa\u5bf9\u6bd4')]
    if bpy.data.filepath:
        dirs.append(os.path.dirname(bpy.data.filepath))
    dirs.append(os.getcwd())
    return dirs


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


def find_fbx():
    if FBX_PATH and os.path.isfile(FBX_PATH):
        return FBX_PATH
    for d in search_dirs():
        if not os.path.isdir(d):
            continue
        hits = [p for p in glob.glob(os.path.join(d, '**', '*.fbx'), recursive=True)
                if 'chr' in os.path.basename(p).lower()]
        if hits:
            hits.sort(key=os.path.getmtime, reverse=True)
            return hits[0]
    return None


def ensure_imported(fbx_path):
    # 场景里已有骨架+蒙皮网格就直接用
    for obj in bpy.data.objects:
        if obj.type == 'MESH':
            for mod in obj.modifiers:
                if mod.type == 'ARMATURE' and mod.object:
                    return True
    if not fbx_path:
        return False
    # 先清空场景再导入，避免旧对象干扰
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)
    bpy.ops.import_scene.fbx(filepath=fbx_path)
    return True


def main():
    fbx_path = find_fbx()
    ok = ensure_imported(fbx_path)
    if not ok:
        show_popup('未导入模型', '场景里没有骨架蒙皮网格，也找不到 FBX。\n'
                   '请先在 Blender 导入带动作的 head FBX，\n'
                   '或在脚本开头修改 FBX_PATH。')
        return

    scene = bpy.context.scene
    depsgraph = bpy.context.evaluated_depsgraph_get()

    meshes, arm = [], None
    for obj in scene.objects:
        if obj.type != 'MESH':
            continue
        for mod in obj.modifiers:
            if mod.type == 'ARMATURE' and mod.object:
                meshes.append(obj)
                if arm is None:
                    arm = mod.object
                break

    if not meshes or arm is None:
        show_popup('未找到网格', '导入的 FBX 里没有带骨架修改器的网格。')
        return

    actions = [a for a in bpy.data.actions if not ACTION_FILTER or ACTION_FILTER in a.name]
    if not actions:
        show_popup('没有匹配动作', '动作过滤器 "%s" 没匹配到任何动作（共 %d 个动作）。\n'
                   '\n'
                   '常见原因：这个 FBX 是 CLI 解包的，只有骨架+网格，\n'
                   '没有动画数据。要用 AssetStudio GUI 全选\n'
                   'Animator + AnimationClips 导出带动作的 FBX：\n'
                   '  右键 Animator -> Export selected objects (merge)\n'
                   '  + Selected AnimationClips\n'
                   '然后再运行本脚本。' % (ACTION_FILTER, len(bpy.data.actions)))
        return

    arm.animation_data_create()
    arm.animation_data.action = None
    scene.frame_set(scene.frame_start)
    depsgraph.update()

    base = {}
    for obj in meshes:
        ev = obj.evaluated_get(depsgraph)
        base[obj.name] = [ev.data.vertices[i].co.copy() for i in range(len(obj.data.vertices))]
        if not obj.data.shape_keys:
            obj.shape_key_add(name='Basis')

    def sample_verts():
        res = {}
        for obj in meshes:
            ev = obj.evaluated_get(depsgraph)
            res[obj.name] = [ev.data.vertices[i].co.copy() for i in range(len(obj.data.vertices))]
        return res

    def action_frames(action):
        times = set()
        for fc in action.fcurves:
            for kp in fc.keyframe_points:
                times.add(int(round(kp.co[0])))
        fr = action.frame_range
        if times:
            times.add(int(round(fr[0])))
            times.add(int(round(fr[1])))
            return sorted(times)
        f0, f1 = int(round(fr[0])), int(round(fr[1]))
        return [f0] if f1 <= f0 else list(range(f0, f1 + 1))

    count = 0
    for action in actions:
        arm.animation_data.action = action
        frames = action_frames(action)
        main = meshes[0].name
        best_f, best_dev, best_verts = frames[0], -1.0, None
        for f in frames:
            scene.frame_set(f)
            depsgraph.update()
            vs = sample_verts()
            dev = sum((vs[main][i] - base[main][i]).length for i in range(len(base[main])))
            if dev > best_dev:
                best_dev, best_f, best_verts = dev, f, vs
        if best_verts is None:
            continue
        for obj in meshes:
            name = action.name
            # Blender 导入 FBX 的动作名可能是 "对象|动作|Base Layer" 三段式，取动作段
            if '|' in name:
                parts = name.split('|')
                name = parts[1] if len(parts) >= 2 else parts[-1]
            if name in obj.data.shape_keys.key_blocks:
                name += '_%d' % best_f
            sk = obj.shape_key_add(name=name, from_mix=False)
            for i, v in enumerate(best_verts[obj.name]):
                sk.data[i].co = v
        count += 1

    arm.animation_data.action = None
    rough_n = set_all_roughness()
    msg = '完成：%d 个动作已转为形态键\n网格：%s\n动作数量：%d\n设置糙度=1 的材质：%d' % (
        count, ', '.join(o.name for o in meshes), len(actions), rough_n)
    print(msg)
    show_popup('转换完成', msg)


try:
    main()
except Exception:
    err = traceback.format_exc()
    print(err)
    show_popup('脚本出错', err[-500:])
