# CGSS 资源工具

CGSS（偶像大师 灰姑娘女孩 星光舞台）资源整合工具：傻瓜式 查询、下载、解包一体的命令行程序
纯 C 编写（MinGW + CMake），可独立运行
下载及解包文件所属均为BANDAI NAMCO Entertainment Inc. 
仅用作学习交流，请于下载或解包后24小时内删除BANDAI NAMCO Entertainment Inc. 
#### 解包和部分下载功能均为DS_V4编写(磕了)
##### 其实傻不傻瓜式我也不知道，我尽量做的简单了
## 功能
### 待完善
1. **数据表查找数据**：从 master.mdb 查 3D 模型 / 2D Spine / 歌曲 / 歌曲动作 / 谱面 / 卡面 / 角色语言 / 3D 舞台的资源文件名和 hash
2. **数据下载并解析**：
- 按卡片 id 下载（卡面 6 尺寸 / 背景 / 卡面Spina动画 / 3D 照片 / 语音 / Spine / 3D 模型 / 台词）
   - 按歌曲 id 下载（音频 acb / 封面 / 动作 / 谱面 / 舞台 / 导演包）
   - 按角色 chara_id 批量下载（列出该角色全部卡，多选或全选，资源类型一次选择）
3. **解包**：
   - 模型解包为 FBX（调用 AssetStudio.CLI）
   - 角色资源解包（卡面 / 背景 / 卡面Spina动画 / 3d照片 / spine → png / 数据文件）
     - 卡面Spina动画（live2d）解出的文件单独放在 角色目录\卡面Spina动画\spine\ 子文件夹
     - 自动把 .skel 转成两份 json：*.json（3.6，浏览器预览用）+ *_v38.json（3.8.75，编辑器用）
   - ACB 音乐提取和 HCA 解码（acb2wavs）
4. **打开Spine预览(beta)**：扫描 CGSS_DOWN 里带 Spine(live2d) 资源的角色，
   缺 json 自动补转，然后用默认浏览器打开 spine_preview/preview.html
   （页面支持直接选 .skel 自动转 JSON，需同时选 atlas 和 tex.png + tex_A8.png，
   A8 是透明通道，合成后特效无黑边；画面已按 Spine 坐标自动翻转）
   - 图层顺序已自动排好：bg → eff2 → chara → eff1 → fg（页面按文件名排序）
   - 默认用 WebGL 渲染（无三角形接缝，大三角形/雾气等半透明效果不会再被 canvas 丢弃）；
     WebGL 不可用时自动回退 canvas 2D + 2 倍超采样
   - 混合模式已模拟：additive/multiply/screen 均按 Spine 规则混合（bg/eff 特效层实测正常）
   - **导出MP4**：页面有“导出MP4”按钮，用 WebCodecs 硬编码 H.264（Main 高码率），
     30fps、尺寸以背景(bg)包围盒为界（无 bg 时用全部图层包围盒，上限 4096），
     导出完成自动下载 .mp4（需新版 Chrome/Edge）

## Spine 编辑器（3.8.75）使用

- 3.8.75 **不能直接打开** 3.6 版本的 .skel/.json（Spine 官方要求数据版本一致）
- 本工具在解包/预览时会额外生成 `*_v38.json`（数据版本标为 3.8.75）
  这批卡面动画数据没有 IK/Transform/Path 约束，3.6→3.8 只差版本号
  （如果哪些卡带了我目前也没辙）
  已在 Spine 3.8 运行时实测可正常加载播放。
- 在 3.8.75 编辑器里：打开 `*_v38.json` + `*_v38.atlas` + `*_merged.png`
  （merged 是 RGB 主贴图 + A8 透明通道合成后的单张贴图，编辑器不支持双贴图）
## 目录结构

```
CGSS/
├── main.c                     主菜单入口（1.查询 2.下载 3.解包 4.Spine预览）
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
├── unpack_res.c               角色资源解包（卡面/背景/卡面Spina动画/3d照片/spine）
├── spine_convert.c / .h       Spine 二进制 .skel -> JSON 转换（CGSS 大端格式，2.1/3.6）
├── texture_merge.cpp / .h     RGB + A8 贴图合成（_merged.png）
├── preview.c / .h             打开 Spine 浏览器预览（主菜单 4）
├── util.c / .h                公共工具（宽字符转换/目录/多选解析）
├── GBKswapUTF8.c / .h         编码转换
├── sqlite3.c / sqlite3.h / sqlite3ext.h   SQLite 库
├── spine_preview/             Spine 浏览器预览网页资源
│   ├── preview.html           预览页（WebGL 渲染 + canvas 回退）
│   ├── spine-core.js          Spine 运行时核心（解析/动画/骨骼）
│   ├── spine-canvas.js        canvas 2D 渲染器
│   ├── spine-webgl.js         WebGL 渲染器（无三角形接缝）
│   ├── cgss_skel_parser.js    CGSS 二进制 .skel 浏览器端解析
│   ├── skel2json.py / skel21_to_json.py    .skel -> JSON 转换脚本
│   └── mp4-muxer.min.js       MP4 导出（WebCodecs H.264）
├── cgss_apply_textures.py        Blender 贴图脚本（自动贴图 + 糙度=1）
├── cgss_anim_to_shapekeys.py     Blender 形态键脚本（骨骼表情 -> 形态键）
├── CMakeLists.txt               构建配置
├── master.mdb / manifest_10133800.db   游戏数据库（不在仓库内，自行准备）
└── README.md                     本文档
```

## 编译

需要 MinGW-w64 和 CMake：

```bash
cmake -S . -B build
cmake --build build --config Release --target all -j 16
```

产物：`build/CGSS_Script.exe`（Release，静态链接）

默认 **静态链接**（CMake 选项 `CGSS_STATIC=ON`）：libgcc/libstdc++ 内嵌进 exe，
产物只依赖 Windows 系统 DLL（gdiplus/winhttp/kernel32/msvcrt/shell32），
**不需要安装 MinGW、不需要带任何 MinGW DLL**，拷到 Windows 10/11 上即可运行。

体积说明：静态版比同配置（Release）动态版大约 20 万字节（内嵌运行库的代价），
但换来了"单 exe 免 DLL"；发布包看起来变小，是因为此前发的是 Debug 版
（未优化 + 带调试信息），Release 优化后明显缩小，与静态无关。

如需动态链接（体积略小，但要带 libgcc/libstdc++ 两个 DLL）：
`cmake -S . -B build -DCGSS_STATIC=OFF`

## 运行

把 `CGSS_Script.exe` 和以下文件放在同一目录：

- `master.mdb`：游戏主库（卡片/角色/歌曲数据）
- `manifest_10133800.db`：资源清单库（资源名 → hash）
- `AssetStudio/`（可选，解包需要）：AssetStudio CLI + GUI
- `acb2wavs.exe` 及依赖 DLL（可选，语音解码需要）

下载的资源保存在 `CGSS_DOWN/{卡id}{卡名}/`，按类型分子目录。

## 依赖说明

- 数据库：从游戏客户端提取的 `master.mdb` 和 `manifest_10133800.db`
- 解包引擎：AssetStudio（.NET 7 程序，需要安装 .NET 7 Desktop Runtime）
- 语音解码：deretore-toolkit 的 acb2wavs
- Blender 脚本（可选，配套提供）：
  - `cgss_apply_textures.py`：FBX 在 Blender 里自动贴图 + 糙度=1
  - `cgss_anim_to_shapekeys.py`：把骨骼表情动作烘焙成形态键

## 常见问题

- 解包报"启动 AssetStudio.CLI 失败"：安装 .NET 7 Desktop Runtime
- 语音解码无输出：确认 acb2wavs.exe 和同目录 DLL 未被杀毒软件删除
- 找不到数据库：保持 master.mdb / manifest_10133800.db 与 exe 同目录
- CLI 导出的 FBX 没有动作：AssetStudio CLI 的硬限制，带动作的 FBX 需用 GUI 全选导出
  （解包菜单内有详细步骤）

## 从 GitHub 获取

源码仓库包含：全部 C 源码、CMakeLists.txt、Blender 脚本、README.md、.gitignore。
**不包含**：游戏数据库、第三方二进制（AssetStudio / acb2wavs）、编译产物。

### 数据库准备

`master.mdb`（游戏主库）和 `manifest_10133800.db`（资源清单）是游戏数据文件，
不在仓库内，需要自行准备：

- 从 CGSS 客户端提取（配合 mishiro 客户端 / 抓包工具）
- 或从 Release 附件获取（看情况发）

编译后把 `CGSS_Script.exe` 和两个数据库放同一目录即可运行。

### Release 附件

GitHub Releases 里提供编译好的完整工具包（zip）：

- `CGSS_Script.exe` + 依赖（AssetStudio / acb2wavs / Blender 脚本）
- 数据库请按上文说明准备

下载后解压，把数据库放进同一目录，双击 `CGSS_Script.exe` 即可使用。

## Spine 小人（SPC，card_spine）说明

- 程序里“Spine小人”下载的是 card_spine_{卡id}.unity3d（贴图+图集），
  同时会自动下载**共享小人骨架** spine_sprachen_petit_chara_common.unity3d。
- 共享骨架（SPSprachen_s.skel）是 **Spine 2.1 格式**（CGSS 头 + 大端浮点），
  工具内置 2.1 → JSON 转换（spine_convert.c），解包时自动识别转换，
  并把坐标按 0.5 缩放对齐 SPC 卡面图集（SPC 图集是半分辨率）。
- 解包后 角色目录\spine\ 里就是完整可用的 Spine 工程：
  - SPSprachen_s.json / SPSprachen_s_v38.json（3.6 / 3.8.75 骨架）
  - SPC{卡id}.atlas（图集，自动从 .atlas.asset 复制一份）
  - SPC{卡id}.png（自带 alpha 的贴图）
- Spine 3.8.75 编辑器：打开 SPSprachen_s_v38.json + SPC{卡id}.atlas + SPC{卡id}.png，
  内置 10 个动画（anime_0000_000 ~ anime_0001_004），主菜单 4 可浏览器预览。
- 注意：共享骨架只含正面皮肤（front_to_left/front_to_right），
  背面皮肤（back_*）是游戏客户端另一套机制，不在下载资源里。

## 版本历史

### 版本号规则
- 小更新（修 bug / 小功能）：版本号 +0.01，如 1.3 -> 1.3.1 -> 1.3.2
- 大更新（新功能模块 / 较大改动）：版本号 +0.1，如 1.3 -> 1.4 -> 1.5
- 超大更新（如新增 GUI 等大改版）：主版本 +1，如 1.x -> 2.0

### v1.3（2026-08-10）
- **小人（SPC）朝向修复**：小人骨架用 flip 骨骼左右镜像（scaleX=-1），
  角色朝右走、朝右看，方向一致（之前动画向右走但角色左视，看起来像倒退）
- 导出 MP4 同步应用该镜像
- **预览加载防呆**：
  - 骨架栏误选图集/贴图等非骨架文件时直接报错并拒绝加载，不再乱解析
  - 图集引用的贴图不在已选贴图里时，明确提示"图集和贴图不配套"，
    不再静默拿第一张贴图渲染成"随机扒块"
- 小人正确文件组合：骨架 SPSprachen_s.json + 图集 SPC{id}.atlas(.asset) + 贴图 SPC{id}.png

### v1.2（2026-08-10）
- **Spine 预览 / 导出改用 WebGL 渲染**：
  - 逐三角形共享顶点光栅化，消除三角形接缝网格线（脸部/影子不再有线框感）
  - 修复背景大三角形被 canvas 2D 丢弃导致的"缺块"：雾气/半透明水面特效正常显示
  - 混合模式按 Spine 规则（normal / additive / multiply / screen），特效不再发黑发暗
  - WebGL 不可用时自动回退 canvas 2D + 2 倍超采样
- **未成熟功能统一标注 beta**：Spine预览、卡面Spina动画、Spine小人、
  导出MP4、表情/镜头、模型解包为FBX

### v1.1（2026-08-10）
- 新增 **Spine 预览（beta）**（主菜单 4）：扫描已解包角色，浏览器直接看卡面 Spine 动画
  - 图层顺序自动排好（bg → eff2 → chara → eff1 → fg）
  - 模拟 additive / multiply 混合模式，特效不再发黑发暗
- 新增 **Spine 2.1 共享小人骨架（beta）** 支持：
  - 自动下载共享骨架 spine_sprachen_petit_chara_common.unity3d
  - 逆向 CGSS Spine 2.1 二进制格式，解包时自动转 JSON（3.6 + 3.8.75）
  - 坐标按 0.5 对齐 SPC 卡面图集，10 个内置动画可播可导入 3.8.75 编辑器
- 卡面Spina动画（card_cartoon）解包增强：
  - .skel 自动转两份 JSON：*.json（3.6 预览）+ *_v38.json（3.8.75 编辑器）
  - RGB + A8 透明通道自动合成 *_merged.png，并生成 *_v38.atlas
  - 二次解包不再误伤已生成的 JSON
- 其他：查询菜单显示共享骨架资源名；atlas 自动复制一份 .atlas 方便编辑器打开

### v1.0
- 首个发布版：数据表查找（3D模型/Spine/歌曲/动作/谱面/舞台/卡面/语音）
- 数据下载（按卡/按角色/按歌曲）+ LZ4 自动解压
- 解包（模型转 FBX / 卡面 / 背景 / 语音 ACB→WAV）
- 配套 Blender 脚本（自动贴图、表情转形态键）
