# CGSS 资源工具

CGSS（偶像大师 灰姑娘女孩 星光舞台）资源整合工具：查询、下载、解包一体的命令行程序
纯 C 编写（MinGW + CMake），可独立运行
下载及解包文件所属均为BANDAI NAMCO Entertainment Inc. 
仅用作学习交流，请于下载或解包后24小时内删除BANDAI NAMCO Entertainment Inc. 

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
   - ACB 音乐提取和 HCA 解码（acb2wavs）

## 目录结构

```
CGSS/
├── main.c                 主菜单入口（1.查询 2.下载 3.解包）
├── lookup_common.c        查询公共函数（hash 打印）
├── lookup_3d.c            3D 模型查询
├── lookup_spine.c         2D Spine 小人查询
├── lookup_song.c          歌曲查询
├── lookup_action.c        歌曲动作查询
├── lookup_chart.c         谱面查询
├── lookup_card.c          卡面查询
├── lookup_voice.c         角色语言查询
├── lookup_stage.c         3D 舞台查询
├── lookup_main.c          查询主菜单
├── lookup_table.h         查询模块公共声明
├── download.c             数据下载（卡片/歌曲/按角色批量）
├── net.c                  网络下载 + LZ4 解压
├── acb.c                  ACB 音乐提取和 HCA 解码
├── unpack.c               解包菜单 + 公共解包工具
├── unpack_fbx.c           模型解包为 FBX
├── unpack_res.c           角色资源解包（卡面/背景/卡面Spina动画/3d照片/spine）
├── util.c                 公共工具（宽字符转换/目录/多选解析）
├── GBKswapUTF8.c          编码转换
├── sqlite3.c / sqlite3.h  SQLite 库
├── cgss_apply_textures.py     Blender 贴图脚本（自动贴图 + 糙度=1）
├── cgss_anim_to_shapekeys.py  Blender 形态键脚本（骨骼表情 -> 形态键）
├── CMakeLists.txt         构建配置
└── README.md              本文档
```

## 编译

需要 MinGW-w64 和 CMake：

```bash
cmake -S . -B build
cmake --build build --config Debug --target all -j 16
```

产物：`build/CGSS_Script.exe`

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
