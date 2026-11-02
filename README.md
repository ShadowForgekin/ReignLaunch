# ReignLaunch

用 **C++17 + Win32 GDI** 从零手写的 Minecraft 启动器，无第三方 UI 框架，单窗口多页面。

**v1.2.0 Beta 4** · 制作人：加纳Brightchaserove（抖音）

![启动页](screenshots/ui_launch.jpg?v=2)

---

## 功能

### 页面结构

顶部五个标签：**启动 / 下载 / 实例 / 用户 / 设置**；「关于」在设置页左侧子菜单里。

| 页面 | 内容 |
|---|---|
| 启动 | 账号 / 内存 / 当前版本，启动按钮 + ▲ 实例选择浮层 |
| 下载 | 原版与模组加载器下载（两级页面，见下） |
| 实例 | 整页实例列表，点一行即切换 |
| 用户 | 左侧账户列表，右侧三种登录方式 |
| 设置 | 游戏目录、Java 路径、内存、下载线程、JVM/游戏参数、下载源、窗口模式、版本隔离 |

### 下载页

**第一级（列表）**

- 版本类型下拉：正式版 / 快照版 / 远古版 / 愚人节 / 所有版本
- 搜索框实时过滤（如输入 `1.20.1`）
- 列表显示 **版本号 · 发布日期 · 类型**，点一行进详情

**第二级（详情）**

七个可选项：`原版` `Fabric` `Quilt` `Forge` `NeoForge` `Fabric API` `QSL`

- 选「原版」= 不装加载器，直接点「安装」下载游戏本体
- 选加载器后列出**该 MC 版本可用的全部加载器版本**，选中再装
- `Fabric API` / `QSL` 从 **Modrinth** 取版本，下载到 `mods/`（跟随版本隔离）

| 下载列表 | 版本详情 |
|---|---|
| ![下载](screenshots/ui_download.jpg?v=2) | ![详情](screenshots/ui_download_detail.jpg?v=2) |

**下载任务面板**

下载时页面中央出现面板，显示：

- 正在下载中：`26.3 Fabric`
- 进度条 + 百分比
- 已完成 / 剩余文件数
- 实时速度（KB/s 或 MB/s）
- 右下角「取消下载」

![下载中](screenshots/ui_download_task.jpg?v=2)

**下载加速**：每个线程复用一条 WinHTTP 会话（避免每文件重新 TLS 握手），
默认 **16 线程**并发，设置页可调 1–128。

### 账户

用户页顶部三种登录方式，点哪个显示哪个表单：

| 模式 | 内容 |
|---|---|
| 第三方登录 | 认证服务器 + 用户名 + 密码（Yggdrasil / authlib-injector） |
| 离线登录 | 离线玩家名 + 使用离线 |
| 微软登录 | **正在开发中**，暂不可用 |

- **多账户**：创建任意多个账户，点左侧卡片切换，每个账户独立保存皮肤
- **离线皮肤**：64×64 或 64×32 的 PNG
  - 存到 `%APPDATA%\ReignLaunch\skins\<玩家名>.png`（界面头像）
  - 同时写入 `.minecraft\CustomSkinLoader\LocalSkin\textures\<玩家名>.png`
    （装了 CustomSkinLoader 模组游戏内可见）
- **离线 UUID**：`MD5("OfflinePlayer:名字")` + version 3 / variant 位，与官方一致

![用户页](screenshots/ui_user.jpg?v=2)

### Java 自动匹配

按版本 JSON 的 `javaVersion.majorVersion` 选 Java，没有就顺着 `inheritsFrom` 找父版本，
再按 MC 版本推断（1.20.5+ → 21，1.18–1.20.4 → 17，更早 → 8）。

**优先选最贴近的版本**（1.20.1 用 17 而不是 25）。找不到时提示并打开 Adoptium 下载页。

### 启动流程

点「启动游戏」后主窗口内叠一层沉浸式画面（不新开窗口、不覆盖顶部标签）：

1. 检测 Java 版本
2. 处理游戏依赖
3. 登录
4. 等待游戏启动

底部 200px 渐变压暗、左下角状态文字、**贴底进度条**；第四步要等游戏窗口真正出现才算完成
（属于游戏 PID + 窗口可见 + 标题非空，最多 180 秒），完成后 1.5 秒自动收起。
中途点「取消」会立即终止 java 进程。

![启动画面](screenshots/ui_launching.jpg?v=2)

### 版本隔离

设置页三档，解决 Forge 找不到 mods：

| 选项 | 行为 |
|---|---|
| 隔离所有实例 | `--gameDir` 指向 `versions/<版本>` |
| 隔离模组实例 | 目录有 `.jar`，或版本 id / JSON 含 Forge / Fabric / NeoForge / Quilt |
| 不隔离实例 | 全部用 `.minecraft` 根目录 |

### 其他设置

- **JVM 参数 / 游戏参数**：空格分隔追加到对应命令行
- **下载源**：自动 / 仅官方 / 仅 BMCLAPI
- **游戏窗口模式**：窗口（854×480）/ 最大化 / 全屏
- **自动更新**：启动时检查 GitHub Releases，有新版提示；下载失败自动切 `gh-proxy.com` 加速

### 版本与文件

- **JSON 解析**：`inheritsFrom` 递归合并，`rules` 规则判定（os.name / os.arch / features）
- **classpath**：优先 `downloads.artifact.path`，回退 Maven 坐标推算
- **natives**：老版本 classifier 解压（系统自带 `tar.exe`）
- **下载器**：WinHTTP，SHA1 校验，`.part` 原子写入，官方源失败切 BMCLAPI
- 用 `java.exe` + `CREATE_NO_WINDOW` 启动：不弹黑窗口但能抓日志

## 构建

需要 **MinGW-w64 GCC 8+**（在 GCC 16.2.0 上验证）与 `windres`。

1. 下载 [authlib-injector](https://github.com/yushijinhun/authlib-injector/releases) 的 jar，
   放到 `src/` 命名 `authlib-injector.jar`（编译进 exe，不随仓库分发）
2. 编译：

```bat
cd src
windres res.rc -O coff -o res.o
g++ -std=c++17 -O2 -mwindows -static ^
    -finput-charset=UTF-8 -fexec-charset=UTF-8 -fwide-exec-charset=UTF-16LE ^
    -o ReignLaunch.exe ReignLaunch.cpp res.o ^
    -lcomctl32 -lole32 -lshell32 -lgdi32 -luuid -ladvapi32 -lwinhttp -lshlwapi -lcomdlg32 -lgdiplus
```

> `-fwide-exec-charset=UTF-16LE` 不能省，否则中文界面全是乱码。
>
> GDI+（`-lgdiplus`）用于加载皮肤 PNG —— 原生 `LoadImage` 只认 BMP/ICO。

也可直接运行仓库根目录的 `build.bat`。

## 使用

1. 设置页选 `.minecraft` 目录（▾ 自动检测或浏览）
2. 用户页选登录方式建账号（离线登录直接填名字点「使用离线」）
3. 下载页选版本 → 进详情 → 选原版或加载器 → 安装
4. 实例页选实例，回启动页点启动

配置在 `%APPDATA%\ReignLaunch\config.json`（首次运行从旧 `%APPDATA%\MCLite` 迁移）。

### 命令行

```bat
ReignLaunch.exe --dump 1.20.1-Forge
```

只生成启动命令行到 `dump.txt`，不启动游戏，用于排查参数。

## 已知限制

- 微软正版登录未实现
- Forge / NeoForge 依赖官方 installer，installer 下载受网络影响
- 第三方登录不做 token 续期，过期需重新登录
- 皮肤需 CustomSkinLoader 模组才能在游戏内显示
- 窗口尺寸固定，除 DPI 缩放外不支持调整

## 许可

MIT。第三方组件：`json.hpp`（MIT, nlohmann）、`authlib-injector`（构建时自行提供）。
