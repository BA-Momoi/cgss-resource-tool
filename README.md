# CGSS 资源工具（CGSS Resource Tool）

CGSS（偶像大师 灰姑娘女孩 星光舞台）资源查询、下载、解包一体化工具。

纯 C 编写（MinGW + CMake），静态链接，Windows 10/11 上解压即用，不需要安装任何运行库。

> 游戏相关资源及内容的著作权归 BANDAI NAMCO Entertainment Inc. 所有。
> 本工具仅用于学习交流，请勿用于商业用途；下载、解包的内容请在 24 小时内删除。

#### 解包和部分下载功能均为DS_V4编写(磕了)
##### 其实傻不傻瓜式我也不知道，我尽量做的简单了

---

## 功能一览

主菜单：

```
1. 数据表查找数据
2. 数据下载并解析
3. 解包
4. 打开 Spine 预览(beta)
```

### 1. 数据表查找

在 `master.mdb` 中按需查询资源名与 hash，支持：

- 3D 模型（自动优先 HQ 版本）
- 2D Spine 小人（新卡 s 骨架 / 老卡 n 骨架）
- 歌曲信息（含已下架歌曲的全部条目）
- 歌曲动作、谱面、3D 舞台
- 卡面（circle / sm / s / m / l / xl 六种尺寸、背景、Live2D、3D 照片）
- 角色语言（文本 / 音频）

### 2. 数据下载并解析

- 卡片资源：卡面、背景、Live2D、3D 照片、语音、Spine 小人、3D 模型、台词、贴纸动作
- 歌曲资源：音频（acb）、封面、动作、谱面、舞台、导演包
- 按角色批量下载：列出该角色全部卡片，支持多选 / 全选
- 下载时自动做 LZ4 解压，已下载的文件可跳过（断点续跑）

### 3. 解包

- 模型解包为 FBX（调用 AssetStudio.CLI；CLI 导出身体贴图会无法自动引用）
- 卡面 / 背景 / Live2D / 3D 照片 / Spine 解包为 PNG
- Spine `.skel` 自动转 JSON，同时生成两份：
  - `*.json`：3.6 格式（浏览器预览用）
  - `*_v38.json`：3.8.75 格式（Spine 编辑器用）
- RGB 主贴图 + A8 透明通道自动合成 `*_merged.png`，并生成配套 atlas
- ACB 音乐提取与 HCA 解码（acb2wavs）

### 4. Spine 预览（beta）

扫描 `CGSS_DOWN` 里带 Spine（Live2D）资源的角色，自动补转缺失的 JSON，
用默认浏览器打开 `spine_preview/preview.html` 即可播放动画。

- 支持直接选择 `.skel` 文件自动转 JSON（需同时选 atlas 和贴图）
- 图层顺序自动排好：bg → eff2 → chara → eff1 → fg
- 混合模式按 Spine 规则模拟（normal / additive / multiply / screen）
- 默认 WebGL 渲染，无三角形接缝；不可用时自动回退 canvas 2D + 2 倍超采样
- 支持左右镜像（flip），新卡 s / 老卡 n 骨架加载前自动提示
- 页面提供「导出 MP4」按钮：WebCodecs 硬编码 H.264，30fps，
  以背景包围盒为输出尺寸（需新版 Chrome / Edge）

---

## 快速开始

1. 下载发布包：
   - `CGSS_ResourceTool.zip`：完整版，已含数据库，解压即用
   - `CGSS_ResourceTool_nodb.zip`：精简版，不含数据库（见下文）
2. 解压后把 `CGSS_Script.exe`、`master.mdb`、`manifest_*.db` 放在同一目录
3. 双击 `CGSS_Script.exe` 即可使用

> 游戏已停止更新新内容，数据库即为最终版本（资源版本 10133800），
> 资源查询与下载不受影响。

### check_update.exe（可选）

附带的小工具，负责检查 / 获取资源清单库：

- 自动扫描同目录的 `manifest_*.db` 并选择最新版本
- 联网查询最新资源版本（starlight.kirara.ca 数据源）
- 不是最新版时自动下载、MD5 校验、LZ4 解压并写入新库
- 全新环境没有清单库时，运行一次即可自动下载最终版清单库

用法：把 `check_update.exe` 放到与 `CGSS_Script.exe` 相同目录后双击，
或在命令行运行 `check_update.exe [目录]`。

`master.mdb`（游戏主库）不通过它下载，请从完整发布包获取。

---

## Spine 编辑器（3.8.75）使用

- Spine 3.8.75 **不能直接打开** 3.6 版本的 `.skel` / `.json`（数据版本必须一致）
- 本工具在解包 / 预览时会额外生成 `*_v38.json`（数据版本 3.8.75）
  这批卡面动画没有 IK / Transform / Path 约束，3.6 → 3.8 仅差版本号，
  已在 Spine 3.8 运行时实测可正常加载播放
- 在 3.8.75 编辑器里打开：
  `*_v38.json` + `*_v38.atlas` + `*_merged.png`
  （merged 是 RGB 主贴图与 A8 透明通道合成后的单张贴图，编辑器不支持双贴图）

---

## 目录结构

```
CGSS/
├── main.c                     主菜单入口（1.查询 2.下载 3.解包 4.Spine预览）
├── check_update.c             资源清单检查 / 下载工具（check_update.exe）
├── lookup_common.c / .h       查询公共函数（hash 打印）
├── lookup_3d.c                3D 模型查询
├── lookup_spine.c             2D Spine 小人查询
├── lookup_song.c              歌曲查询
├── lookup_action.c            歌曲动作查询
├── lookup_chart.c             谱面查询
├── lookup_card.c              卡面查询
├── lookup_voice.c             角色语言查询
├── lookup_stage.c             3D 舞台查询
├── lookup_main.c              查询主菜单
├── lookup_table.h             查询模块公共声明
├── data.h                     数据结构定义（卡面/角色等）
├── download.c / .h            数据下载（卡片/歌曲/按角色批量）
├── net.c / .h                 网络下载 + LZ4 解压
├── acb.c / .h                 ACB 音乐提取和 HCA 解码
├── unpack.c / .h              解包菜单 + 公共解包工具
├── unpack_fbx.c               模型解包为 FBX
├── unpack_res.c               角色资源解包
├── spine_convert.c / .h       Spine .skel -> JSON 转换（CGSS 大端格式）
├── texture_merge.cpp / .h     RGB + A8 贴图合成
├── preview.c / .h             Spine 浏览器预览
├── util.c / .h                公共工具
├── GBKswapUTF8.c / .h         编码转换
├── sqlite3.c / sqlite3.h / sqlite3ext.h   SQLite 库
├── spine_preview/             Spine 预览网页资源
├── cgss_apply_textures.py       Blender 贴图脚本
├── cgss_anim_to_shapekeys.py    Blender 形态键脚本
├── CMakeLists.txt              构建配置
├── master.mdb / manifest_10133800.db   游戏数据库（不在仓库内，自行准备）
└── README.md                   本文档
```

---

## 编译

需要 MinGW-w64 和 CMake：

```bash
cmake -S . -B build_static -DCMAKE_BUILD_TYPE=Release -DCGSS_STATIC=ON
cmake --build build_static --target all -j 8
```

产物：

- `build_static/CGSS_Script.exe`（主程序）
- `build_static/check_update.exe`（清单检查工具）

默认静态链接：libgcc / libstdc++ 已内嵌进 exe，产物只依赖 Windows 系统 DLL，
不需要安装 MinGW，也不需要附带任何 MinGW DLL。

如需动态链接（体积略小，但要带 libgcc / libstdc++ 两个 DLL）：

```bash
cmake -S . -B build -DCGSS_STATIC=OFF
```

也可以直接运行 `pack_release.bat`：一键构建静态版并重新打包 release 下的两个 zip。

---

## 依赖说明

- 数据库：从游戏客户端提取的 `master.mdb`（主库）和 `manifest_10133800.db`（资源清单）。
  完整发布包已包含
- 模型解包：AssetStudio（.NET 7 程序，需要安装 .NET 7 Desktop Runtime）
- 语音解码：deretore-toolkit 的 `acb2wavs.exe` 及同目录 DLL
- Blender 脚本（可选）：
  - `cgss_apply_textures.py`：FBX 在 Blender 里自动贴图 + 糙度 = 1
  - `cgss_anim_to_shapekeys.py`：把骨骼表情动作烘焙成形态键

---

## 常见问题

- 解包报「启动 AssetStudio.CLI 失败」：安装 .NET 7 Desktop Runtime
- 语音解码无输出：确认 `acb2wavs.exe` 和同目录 DLL 未被杀毒软件删除
- 找不到数据库：完整包解压后 `master.mdb`、`manifest_*.db` 与 exe 同目录即可；
  `_nodb` 版请自行准备数据库，或运行一次 `check_update.exe` 获取清单库
- CLI 导出的 FBX 身体没有贴图：带贴图的 body_FBX 需用 GUI 导出
  （解包菜单内有详细步骤）

---

## 版本历史

### 版本号规则

- 小更新（修 bug / 小功能）：版本号 +0.01，如 1.3 → 1.31 → 1.32
- 大更新（新功能模块 / 较大改动）：版本号 +0.1，如 1.3 → 1.4 → 1.5
- 超大更新（如新增 GUI 等大改版）：主版本 +1，如 1.x → 2.0

### v1.41（2026-08-12）

- 新增 `check_update.exe`：检查资源版本 / 首次使用自动下载资源清单库
  - 自动扫描同目录 `manifest_*.db` 并选最新版本
  - 联网对比最新资源版本，非最新时自动下载 + MD5 校验 + LZ4 解压
  - 游戏停更后版本号数据库若未变运行会显示「已是最新」，也可用于全新环境自动获取清单库
- 主程序不再写死 `manifest_10133800.db`：自动使用同目录版本号最大的 `manifest_*.db`
- 发布包整理：移除测试残留，README 重写为用户手册

### v1.4（2026-08-11）

- **新增贴纸动作下载与解包（310 个）**：下载类型新增「4.贴纸动作」
  - 全部 `spine_motion_sticker_*.unity3d` 下载到 `CGSS_DOWN\贴纸\原文件unity3d\`
  - 自动解出 spine 文件（skel / atlas / png / json / v38 json）到 `spine文件\SPMotionSticker_XXXXX\`
  - 按 atlas 把每张贴纸裁成两帧 PNG 到 `贴纸PNG\SPMotionSticker_XXXXX_1.png / _2.png`
  - 支持断点续跑：已下载 / 已解包的自动跳过
- **预览小人镜像可切换**：新增「左右镜像(flip)」勾选框（flip 骨骼 scaleX -1/1）
- 预览加载后自动提示检测到 N 骨架（老卡）还是 s 骨架（新卡），选错不会大头

### v1.31（2025-08-11）

- Spine 预览增加对老卡骨架选择提示

### v1.3（2026-08-10）

- **小人（SPC）朝向修复**：小人骨架用 flip 骨骼左右镜像（scaleX=-1），
  角色朝右走、朝右看，方向一致（之前动画向右走但角色左视，看起来像倒退）
- 导出 MP4 同步应用该镜像
- **预览加载防呆**：
  - 骨架栏误选图集 / 贴图等非骨架文件时直接报错并拒绝加载
  - 图集引用的贴图不在已选贴图里时，明确提示「图集和贴图不配套」
- 小人正确文件组合：骨架 SPSprachen_s.json + 图集 SPC{id}.atlas(.asset) + 贴图 SPC{id}.png

### v1.2（2026-08-10）

- **Spine 预览 / 导出改用 WebGL 渲染**：
  - 逐三角形共享顶点光栅化，消除三角形接缝网格线（脸部 / 影子不再有线框感）
  - 修复背景大三角形被 canvas 2D 丢弃导致的「缺块」：雾气 / 半透明水面特效正常显示
  - 混合模式按 Spine 规则（normal / additive / multiply / screen），特效不再发黑发暗
  - WebGL 不可用时自动回退 canvas 2D + 2 倍超采样
- 未成熟功能统一标注 beta：Spine 预览、卡面 Spina 动画、Spine 小人、导出 MP4、
  表情 / 镜头、模型解包为 FBX

### v1.1（2026-08-10）

- 新增 **Spine 预览（beta）**（主菜单 4）：扫描已解包角色，浏览器直接看卡面 Spine 动画
  - 图层顺序自动排好（bg → eff2 → chara → eff1 → fg）
  - 模拟 additive / multiply 混合模式，特效不再发黑发暗
- 新增 **Spine 2.1 共享小人骨架（beta）** 支持：
  - 自动下载共享骨架 spine_sprachen_petit_chara_common.unity3d
  - 逆向 CGSS Spine 2.1 二进制格式，解包时自动转 JSON（3.6 + 3.8.75）
  - 坐标按 0.5 对齐 SPC 卡面图集，10 个内置动画可播可导入 3.8.75 编辑器
- 卡面 Spina 动画（card_cartoon）解包增强：
  - `.skel` 自动转两份 JSON：`*.json`（3.6 预览）+ `*_v38.json`（3.8.75 编辑器）
  - RGB + A8 透明通道自动合成 `*_merged.png`，并生成 `*_v38.atlas`
  - 二次解包不再误伤已生成的 JSON
- 其他：查询菜单显示共享骨架资源名；atlas 自动复制一份 `.atlas` 方便编辑器打开

### v1.0

- 首个发布版：数据表查找（3D 模型 / Spine / 歌曲 / 动作 / 谱面 / 舞台 / 卡面 / 语音）
- 数据下载（按卡 / 按角色 / 按歌曲）+ LZ4 自动解压
- 解包（模型转 FBX / 卡面 / 背景 / 语音 ACB→WAV）
- 配套 Blender 脚本（自动贴图、表情转形态键）

---

## 致谢

- 资源清单 / 资源服务器结构参考 [mishiro](https://github.com/toyobayashi/mishiro)
- 模型解包使用 AssetStudio
- 音频解码使用 deretore-toolkit（acb2wavs）
- Spine 预览使用 Spine Runtimes（spine-core / spine-canvas / spine-webgl）
