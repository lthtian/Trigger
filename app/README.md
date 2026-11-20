# Trigger

Windows 场景启动器：保存网页、EXE、显示器、窗口布局和 Windows 虚拟桌面分配，再通过按钮、桌面快捷方式或全局快捷键恢复。C++20 / Win32 负责系统能力，WebView2 加载本地 HTML/CSS/JavaScript 管理界面。

## 试用

完整解压 `dist/Trigger-0.1.0-win-x64.zip`，运行其中的 `Trigger.exe`。预置“求职”场景：牛客在主屏左半边，BOSS 直聘在右半边。默认使用检测到的 Edge，也可以逐项改为 Chrome。

管理界面的“从现有窗口记录”可以记录应用路径、窗口大小、显示器和虚拟桌面。浏览器需要补充网址；应用的文件、项目等启动参数需要手工填写。编辑窗口后点击“应用到场景”，再点击“保存配置”。

场景启动后默认收起并释放 WebView2。已启用的验证码提醒和全局快捷键由原生后台处理。两项都关闭时，完成启动后退出整个进程。托盘菜单可以重新打开管理界面或退出后台。

## 范围

- 试用包为 Windows x64；当前实测系统为 Windows 10 22H2，19045.6466。
- 虚拟桌面使用 Windows Shell 私有接口，当前仅开放 build 19045。其他版本保留当前桌面的普通窗口操作，不宣称支持跨桌面恢复。
- 显示器按设备标识及工作区比例记录；原显示器缺失时回到主屏并提示。混合 DPI、不同硬件组合尚需进一步实机验证。
- 指定尚不存在的虚拟桌面时自动创建；不会主动切换当前桌面。桌面位置按当前顺序编号保存。
- Chrome/Edge 使用普通用户配置，按明确保存的网址创建窗口，不要求扩展或独立浏览器配置。
- 已跟踪窗口通过句柄、进程创建时间和启动配置识别。没有跟踪信息时，仅复用可唯一确定的路径/标题匹配项。修改网址或参数后会重新启动相应内容。
- 应用自己的最小尺寸、管理员权限、启动器/宿主进程和非标准窗口可能限制恢复。超时或尺寸不符会显示具体结果，不保证适配所有应用。
- 本版本不自动抓取浏览器全部标签、恢复应用内部未保存状态、解锁账号或完成网站登录。

## 验证码

“设置 → 验证码连接”填写 HTTPS 服务根地址与读取令牌。客户端每 3 秒读取 `/events`；失败后退避，最多 30 秒。启动时已存在的短信进入最近列表，不弹出历史提醒。新验证码在主屏左下角显示 15 秒，点击复制后约 1.2 秒收起。

服务返回：`{"events":[{"id":"unique-id","sender":"sender","code":"123456","receivedAt":1234567890}]}`。仅接收 4 至 8 位数字、最近 5 分钟的条目，最多保留 32 条。TLS 证书必须有效，不接受重定向。连接凭据通过当前 Windows 账号的 DPAPI 加密保存；验证码正文仅存于内存。

本次自动审批拒绝导入已有服务器读取凭据，试用包没有预置令牌。已有手机到服务器的实际收码验证和桌面测试弹窗验证分别完成；完整的“现有服务器授权读取 → 此客户端弹窗”尚未验证。

## 本地文件

默认目录：`%LOCALAPPDATA%\Trigger`。

- `scenes.json`：场景与运行设置。
- `windows.json`：已识别窗口的指纹。
- `connection.dat`：当前账号加密的连接凭据。
- `last-restore.json`：最近一次启动结果，不含验证码。
- `WebView/`：管理界面的 WebView2 用户数据。

命令行支持 `--scene <id>`、`--background`、`--data-dir <目录>`。快捷方式包含正确的数据目录。不要单独移动 EXE，必须保留相邻的 `web` 文件夹。

## 构建和验证

依赖位于 `vendor/`：WebView2 SDK 1.0.4258.31、nlohmann/json 3.12.0。虚拟桌面接口参考 MScholtes/VirtualDesktop 的 Windows 10 实现；许可证保存在 `vendor/`。运行需要 Microsoft Edge WebView2 Runtime；C++ 运行库和 WebView2 Loader 静态链接。

使用 CMake 和已安装的 Visual Studio x64 MSVC。先通过 `vswhere` 发现工具路径；已有 `build/CMakeCache.txt` 时使用其中记录的 CMake、生成器和平台。通过 CMD 启动构建，在 CMake 所在目录执行：

```bat
cmd.exe /d /c cmake.exe --build "D:\Trigger\app\build" --config Release --target Trigger --parallel -- /p:TrackFileAccess=false
```

缺少 build 目录时才配置：`cmake.exe -S <源码目录> -B <构建目录> -G <已安装的 Visual Studio 生成器> -A x64`。编译本身不自动启动程序或测试。

测试目标：`Trigger.CoreTests`、`Trigger.DesktopTests`、`Trigger.PlatformTests`、`Trigger.TestControl`。平台测试需要正常桌面会话，使用专用 `Trigger.WindowProbe` 窗口并正常关闭。虚拟桌面测试创建临时桌面，验证后移除。UI 测试通过仅测试实例启用的 WebView2 本机 CDP 执行，正式包不启用调试端口、不携带 Node/Playwright。

所有 C++ 源文件为 UTF-8 BOM。clang-format 配置保留头文件顺序，因为 Windows SDK 与 WebView2 头文件存在先后依赖。
