# CGSS GUI 开发说明

GUI 已经作为主工程的可选目标加入，源码都在 `gui/` 内。CLI 默认构建行为不变。

## 直接构建

双击 `gui/build_gui.bat`，生成文件位于：

```text
build_gui/bin/CGSS_GUI.exe
```

也可以在工程根目录执行：

```bat
cmake -S . -B build_gui -DCMAKE_BUILD_TYPE=Release -DCGSS_BUILD_GUI=ON -DCGSS_STATIC=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build_gui --target CGSS_GUI -j 8
```

## 本机依赖

默认按当前目录结构寻找：

- cimgui：`../SDL2/cimgui`
- SDL3 源码：`../sdl3_src/SDL3-3.4.14`
- SDL3 静态库：`../sdl3_build/libSDL3.a`
- Spine 3.6 C runtime：`build/_spine36b/spine-c/spine-c`

换电脑后可以通过 CMake 参数覆盖对应的 `CGSS_GUI_*` 路径，配置失败时会直接显示缺少哪个依赖。

## VS Code 头文件报红

请在 VS Code 中打开包含顶层 `CMakeLists.txt` 的工程根目录，不要只打开 `gui`。
工程已提供 `.vscode/c_cpp_properties.json`，优先读取 CMake 生成的
`build_gui/compile_commands.json`，并包含 SDL3、cimgui、ImGui backend 和 Spine
的备用头文件路径。CMake 能编译但 VS Code 报
`无法打开源文件`，通常只是 C/C++ 扩展没有读取 CMake 的 include 参数，并不代表代码编译失败。

首次打开或更换依赖目录后执行：

1. `Ctrl+Shift+P`
2. 运行 `C/C++: Reset IntelliSense Database`
3. 再运行 `Developer: Reload Window`

默认相对目录结构如下：

```text
<开发目录>\CGSS                         工程根目录
<开发目录>\SDL2\cimgui                cimgui
<开发目录>\sdl3_src\SDL3-3.4.14      SDL3
```

如果你移动了这些目录，同时修改：

- `gui/CMakeLists.txt` 中的 `CGSS_GUI_CIMGUI_DIR`、`CGSS_GUI_SDL3_DIR`
- `.vscode/c_cpp_properties.json` 中的 `includePath`

## 先理解 ImGui 的写法

ImGui 是“每帧重画”的界面。`draw_main_panel()` 会在每一帧执行一次，控件函数
返回 `true` 表示这一帧用户触发了它：

```c
if (igButton("刷新", v2(100.0f, 36.0f)))
    rescan_scene(app);
```

需要跨帧保留的数据必须放进 `GuiApp`，不要写成绘制函数里的普通局部变量：

```c
typedef struct GuiApp {
    /* ... */
    bool my_option;
    char my_query[256];
} GuiApp;
```

然后直接把状态地址交给控件：

```c
igCheckbox("启用功能", &app->my_option);
igInputText("关键词", app->my_query, sizeof(app->my_query),
            ImGuiInputTextFlags_None, NULL, NULL);
```

## 修改现有页面

主要入口都在 `gui/main.c`：

- `GuiPage`：页面编号
- `GuiApp`：整个 GUI 的持久状态
- `draw_footer()`：底部五个游戏入口及点击区域
- `draw_main_panel()`：根据当前页面绘制内容
- `set_style()`：全局间距、圆角和基础颜色
- `begin_game_panel()` / `end_game_panel()`：游戏面板及浅色/深色控件主题

在 `draw_main_panel()` 中找到对应的判断即可修改页面：

```c
if (app->page == GUI_PAGE_UNPACK) {
    /* 解包页控件 */
    return;
}
```

内容可能换行时，不要用固定屏幕坐标放下一个控件，否则长路径会压住按钮。让控件
跟随当前光标位置：

```c
igTextWrapped("%s", long_path);
igSpacing();
if (game_button_at(app, "##reload", "重新载入",
                   igGetCursorScreenPos(), v2(170.0f, 42.0f), 0, false)) {
    rescan_scene(app);
}
```

只有固定在边角的工具按钮才适合直接使用 `v2(x, y)`。

## 新增一个页面

例如新增“日志”页，按下面顺序修改：

1. 在 `GuiPage` 增加 `GUI_PAGE_LOG`。
2. 在 `GuiApp` 增加日志页需要保存的状态。
3. 在 `draw_main_panel()` 增加 `GUI_PAGE_LOG` 分支。
4. 在 `draw_footer()` 的 `pages`、`ids`、`tips` 和图标数组中增加对应项。
5. 如果底栏素材仍只有五格，需要先准备游戏内的新入口素材和点击区域，不能直接把现有格子横向拉长。

建议先把页面内容写成单独函数：

```c
static void draw_log_page(GuiApp *app, float width, float height)
{
    begin_game_panel(app, "##log_page", v2(28.0f, 28.0f),
                     v2(width - 56.0f, height - 56.0f), false);
    igText("日志");
    igSeparator();
    igTextWrapped("%s", app->my_log);
    end_game_panel();
}
```

## 新增实际功能

按钮只负责发出动作，不要在绘制函数内直接执行长时间下载、解包或等待子进程，否则
窗口会卡住。现有资源下载可作为模板：

1. 在 `resource_backend.h` 声明后端函数和任务状态。
2. 在 `resource_backend.c` 创建工作线程处理下载或解包。
3. 工作线程通过 `CRITICAL_SECTION` 更新快照。
4. GUI 每帧调用 `resource_backend_snapshot()` 读取进度。
5. 按钮只调用 `resource_backend_start_download()` 启动任务。

短操作可以直接放在按钮判断内，长操作必须走后台任务。

## 使用游戏 UI 素材

素材放在 `gui/assets/game_ui`。游戏 PNG 和 OTF 字体不属于本仓库的 MIT
许可；当前功能分支仅按仓库所有者要求用于临时学习演示，具体来源和清理要求见
`gui/assets/game_ui/SOURCE.txt`。缺少素材时程序会使用基础控件回退样式。
增加素材时需要同时处理：

1. 在 `UiAssets` 增加一个 `UiTexture` 字段。
2. 在 `load_ui_assets()` 调用 `load_ui_png()`。
3. 在 `free_ui_assets()` 调用 `SDL_DestroyTexture()`。
4. 绘制时使用 `draw_slice_at()` 或九宫格 `draw_nine_slice()`。

普通按钮优先使用 `game_button_at()`。这个函数会保持原图宽高比；不要直接把一张按钮
图片强制铺满任意宽高，否则边框、星形和圆角都会被拉伸。可伸缩面板使用九宫格，
只拉伸中间区域，不拉伸四角。

## 中文和日文输入

搜索框使用 UTF-8 `char` 缓冲区。`SDL_HINT_IME_IMPLEMENTED_UI` 必须在
`SDL_Init()` 前设置为 `none`，由 Windows 显示原生输入法组合和候选窗口；SDL3 的
ImGui backend 会负责 `SDL_StartTextInput()` 和 `SDL_EVENT_TEXT_INPUT`。

不要在业务控件中再次反复调用 `SDL_StartTextInput()` / `SDL_StopTextInput()`，否则会
与 ImGui backend 争用输入法状态。字体字形范围由 `gui_cjk_ranges()` 提供。

SDL/ImGui 只接收系统输入法提交的文字，不负责把罗马字转换成假名。Windows 必须先在
“设置 -> 时间和语言 -> 语言和区域 -> 添加语言 -> 日语”中安装“基本输入”。可以用
下面的 PowerShell 命令检查，`State` 为 `NotPresent` 就表示本机没有日文 IME：

```powershell
Get-WindowsCapability -Online -Name 'Language.Basic~~~ja-JP~0.0.1.0'
```

未安装日文 IME 时，仍可把 `島村卯月` 或 `しまむらうづき` 粘贴到搜索框，验证
UTF-8 输入和日文名称映射；但不能直接键入罗马字并在程序内完成日文组合。

中文输入法输入与日文名称相同的汉字片段也可以直接搜索，例如只输入 `卯月`。
`岛村卯月` 和 `島村卯月` 会自动生成简体、繁体变体，不需要分别输入两次。

资源文件名本身大多是英文，因此日文角色名、卡名和歌曲名搜索还会通过
`master.mdb` 映射为资源 ID，再与 manifest 资源名匹配。

## 运行时文件

构建后会自动复制游戏 UI 素材、`master.mdb`、所有 `manifest_*.db`，并在找到 AssetStudio 时复制完整目录。数据库、AssetStudio、下载内容和构建目录仍由 `.gitignore` 排除，不会提交到仓库。

主页会自动下载 `b/bgm_studio_night.acb` 到 `CGSS_DOWN/BGM`，在后台调用
`acb2wavs.exe` 解码，并通过 SDL3 循环播放。离开主页时暂停，回到主页后
从原位置继续。构建脚本会从现有 CLI Release 或 `build` 目录复制
`acb2wavs.exe` 及其小型运行库；系统需要 .NET Framework 4.x。
