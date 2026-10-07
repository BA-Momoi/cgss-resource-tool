==================================================
  CGSS 资源工具 v1.41
  查询 / 下载 / 解包 / Spine 预览 / USM 解包
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
  usm.exe                    USM 视频解包工具
  master.mdb                 游戏主库（卡片/角色/歌曲数据）
  manifest_*.db              资源清单库（资源名 -> hash）
  ffmpeg.exe                 视频转换工具
  spine_preview\             Spine 浏览器预览网页（主菜单 3 使用）
  AssetStudio\               模型解包引擎
  dotnet\                     随包提供的 .NET 7 运行时
  acb2wavs.exe + *.dll       语音解码（可选）
  cgss_apply_textures.py     Blender 贴图脚本（可选）
  cgss_anim_to_shapekeys.py  Blender 形态键脚本（可选）

【快速开始】

1. 解压，保持所有文件在同一目录
2. 联网启动时会检查资源清单并自动同步 master.mdb；离线时继续使用本地数据库
3. 双击 CGSS_Script.exe
4. 主菜单选择：
     1.资源查找与下载    2.解包
     3.打开Spine预览     4.USM/CG解包
     5.退出

下载的资源保存在本目录 CGSS_DOWN\ 下，按角色/类型分目录。


【依赖】

  - 完整发布包附带 .NET 7 和 Windows Desktop Runtime，无需另行安装
  - 语音解码：acb2wavs.exe 及同目录 DLL 请不要删除或隔离
  - Spine 预览需要系统浏览器；Blender 脚本需配合 Blender 使用（可选）


【常见问题】

Q: 解包报"启动 AssetStudio.CLI 失败"？
A: 确认解压目录中的 dotnet 和 AssetStudio 文件夹完整；也可检查安全软件是否隔离了文件。

Q: 语音解码无输出？
A: 确认 acb2wavs.exe 和同目录 DLL 未被杀毒软件删除。

Q: 提示缺少数据库？
A: 完整发布包中的 master.mdb 和 manifest_*.db 应与 exe 同目录。
   若文件缺失，请重新解压完整包；下载资源还需要网络连接。

Q: CLI 导出的 FBX 身体没有贴图？
A: 带贴图的 body_FBX 请用 AssetStudio GUI 导出
   （解包菜单内有详细步骤）。


【版本】

v1.42：check_update 新增 master.mdb 自动补齐，_nodb 精简版自给自足。
v1.41：新增 check_update.exe；主程序自动选用最新 manifest_*.db；
        发布包整理，README 重写。
v1.4 ：新增贴纸动作下载与解包（310 个）；预览小人镜像可切换。
更早版本见仓库 README.md。
