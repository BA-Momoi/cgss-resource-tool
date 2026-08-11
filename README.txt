==================================================
  CGSS 资源工具 v1.41
  查询 / 下载 / 解包 / Spine 预览 一体化
==================================================

【这是什么】

面向 CGSS（偶像大师 灰姑娘女孩 星光舞台）的资源工具：
查询资源文件名与 hash、从官方资源服务器下载、解包成
PNG / FBX / WAV / Spine 工程文件，并在浏览器里预览卡面动画。

纯 C 编写、静态链接，本目录解压后双击即可运行。

游戏相关资源的著作权归 BANDAI NAMCO Entertainment Inc. 所有。
本工具仅用于学习交流，请勿用于商业用途；下载、解包的内容请于
24 小时内删除。


【目录说明】

  CGSS_Script.exe            主程序（双击运行）
  check_update.exe           资源清单检查 / 下载工具（可选）
  master.mdb                 游戏主库（卡片/角色/歌曲数据）
  manifest_10133800.db       资源清单库（资源名 -> hash）
  spine_preview\             Spine 浏览器预览网页（主菜单 4 使用）
  AssetStudio\               模型解包引擎（.NET 7，可选）
  acb2wavs.exe + *.dll       语音解码（可选）
  cgss_apply_textures.py     Blender 贴图脚本（可选）
  cgss_anim_to_shapekeys.py  Blender 形态键脚本（可选）

注：CGSS_ResourceTool_nodb.zip 不含 master.mdb 和
manifest_10133800.db，需要自行准备数据库。


【快速开始】

1. 解压，保持所有文件在同一目录
2. 双击 CGSS_Script.exe
3. 主菜单选择：
     1.数据表查找数据    2.数据下载并解析
     3.解包              4.打开Spine预览(beta)

下载的资源保存在本目录 CGSS_DOWN\ 下，按角色/类型分目录。


【check_update.exe 是什么】

游戏已停止更新新内容，数据库为最终版本。check_update 用于：

  - 确认清单库是最新版本（运行后显示"已是最新"即可）
  - 全新环境没有清单库时，运行一次自动从服务器下载最终版清单库

运行方式：放在与 CGSS_Script.exe 相同目录后双击，
或命令行执行：check_update.exe [目录]

注意：master.mdb（游戏主库）不通过它下载，请从完整发布包获取。


【依赖】

  - 模型解包为 FBX：需要安装 .NET 7 Desktop Runtime
  - 语音解码：acb2wavs.exe 及同目录 DLL 请不要删除或隔离
  - Blender 脚本：配合 Blender 使用（可选）


【常见问题】

Q: 解包报"启动 AssetStudio.CLI 失败"？
A: 安装 .NET 7 Desktop Runtime。

Q: 语音解码无输出？
A: 确认 acb2wavs.exe 和同目录 DLL 未被杀毒软件删除。

Q: 提示缺少数据库？
A: 完整包解压后数据库应与 exe 同目录；
   精简版请自行准备，或运行 check_update.exe 获取清单库。

Q: CLI 导出的 FBX 身体没有贴图？
A: 带贴图的 body_FBX 请用 AssetStudio GUI 导出
   （解包菜单内有详细步骤）。


【版本】

v1.41：新增 check_update.exe；主程序自动选用最新 manifest_*.db；
        发布包整理，README 重写。
v1.4 ：新增贴纸动作下载与解包（310 个）；预览小人镜像可切换。
更早版本见仓库 README.md。
