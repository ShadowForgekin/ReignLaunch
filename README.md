# ReignLaunch

用 **C++17 + Win32 GDI** 从零手写的 Minecraft 启动器，无第三方 UI 框架，单窗口多页面。

**v1.0.0** · 制作人：加纳Brightchaserove（抖音）

![启动页](screenshots/ui_launch.jpg?v=20261103)

---

## 功能

### 页面结构

顶部五个标签：**启动 / 下载 / 实例 / 用户 / 设置**；「关于」在设置页左侧子菜单里。

| 页面 | 内容 |
|---|---|
| 启动 | 账号 / 内存 / 当前版本，启动按钮 + ▲ 实例选择浮层 |
| 下载 | 原版游戏 / Mod下载 两个分区 |
| 实例 | 实例列表 + 模组管理（启用/禁用/删除） |
| 用户 | 左侧账户列表，右侧三种登录方式 |
| 设置 | 游戏目录、Java、内存、线程、JVM/游戏参数、下载源、窗口模式、版本隔离 |

### 下载页

**第一级（列表）**：版本类型下拉（正式版 / 快照版 / 远古版 / 愚人节 / 所有版本）、搜索框，
列表显示 **版本号 · 发布日期 · 类型**。

**第二级（详情）**：七个选项 `原版` `Fabric` `Quilt` `Forge` `NeoForge` `Fabric API` `QSL`

- 选原版 = 不装加载器，直接下载游戏本体
- 选加载器后列出该 MC 版本可用的**全部加载器版本**，选中再装
- Fabric API / QSL 从 Modrinth 取版本，下载到 `mods/`

| 下载列表 | 版本详情 |
|---|---|
| ![下载](screenshots/ui_download.jpg?v=20261103) | ![详情](screenshots/ui_download_detail.jpg?v=20261103) |

**下载任务面板**：显示「正在下载中：26.3 Fabric」、进度条、已完成/剩余文件数、
实时速度、取消按钮。

**下载加速**：每线程复用 WinHTTP 会话，默认 16 线程（可调 1–128）。

### Mod 下载

![Mod下载](screenshots/ui_download_mod.jpg?v=20261103b)

**模组下载**

- 下载页顶部切到「Mod下载」分区，源可切 Modrinth（可用）/ CurseForge（需 API Key）
- 搜索按当前实例自动过滤：原版 MC 版本 + 加载器类型
  （Forge 实例搜 Forge 版模组，Fabric 实例搜 Fabric 版）
- 结果列出名称与简介，选中后点「下载并安装」→ 下载到该实例的 `mods/`
  （开了版本隔离就进 `versions/<实例>/mods`）

**实例页的模组管理**

- 列出当前实例 `mods/` 下的所有 `.jar`
- **启用 / 禁用**：重命名 `.jar` ↔ `.jar.disabled`，禁用的显示「已禁用」
- **删除**：二次确认后从磁盘移除

### 账户

用户页顶部三种登录方式：

| 模式 | 内容 |
|---|---|
| 第三方登录 | 认证服务器 + 用户名 + 密码（Yggdrasil / authlib-injector） |
| 离线登录 | 离线玩家名 + 使用离线 |
| 微软登录 | 正在开发中，暂不可用 |

- 多账户，点左侧卡片切换，每个账户独立保存皮肤
- 离线皮肤 64×64 或 64×32 PNG，写入 `%APPDATA%\ReignLaunch\skins\` 与
  `CustomSkinLoader/LocalSkin/textures/`（装 CSL 模组游戏内可见）
- 离线 UUID = `MD5("OfflinePlayer:名字")` + version 3 / variant 位

![用户页](screenshots/ui_user.jpg?v=20261103)

### Java 自动匹配

按版本 JSON 的 `javaVersion.majorVersion` 选 Java，没有就顺 `inheritsFrom` 找父版本，
再按 MC 版本推断（1.20.5+ → 21，1.18–1.20.4 → 17，更早 → 8）。优先最贴近的版本。

### 启动流程

点击启动后主窗口内叠一层沉浸式画面：底部 200px 渐变压暗、左下角状态、贴底进度条。
四步：检测 Java → 处理依赖 → 登录 → 等待游戏启动（等窗口真正出现才算完成）。
中途取消会立即终止 java 进程。

![启动画面](screenshots/ui_launching.jpg?v=20261103)

### 版本隔离

设置页三档：隔离所有实例 / 仅隔离模组实例 / 不隔离。

### 其他设置

JVM 参数、游戏参数、下载源（自动/仅官方/仅 BMCLAPI）、游戏窗口模式（窗口/最大化/全屏）、
自动更新（GitHub Releases，失败自动切 gh-proxy 加速）。

## 构建

需要 **MinGW-w64 GCC 8+** 与 `windres`。

1. 下载 [authlib-injector](https://github.com/yushijinhun/authlib-injector/releases) 的 jar，
   放到 `src/` 命名 `authlib-injector.jar`
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
> GDI+（`-lgdiplus`）用于加载皮肤 PNG。

也可直接运行 `build.bat`。

## 使用

1. 设置页选 `.minecraft` 目录
2. 用户页建账号（离线登录填名字点「使用离线」）
3. 下载页选版本 → 详情 → 选原版或加载器 → 安装
4. 模组页搜模组 → 下载并安装
5. 实例页选实例，回启动页启动

配置在 `%APPDATA%\ReignLaunch\config.json`（首次运行从旧 `%APPDATA%\MCLite` 迁移）。

### 命令行

```bat
ReignLaunch.exe --dump 1.20.1-Forge
```

只生成启动命令行到 `dump.txt`，不启动游戏。

## 已知限制

- 微软正版登录未实现
- CurseForge 模组源需要 API Key，目前仅 Modrinth
- Forge / NeoForge 依赖官方 installer，受网络影响
- 第三方登录不做 token 续期
- 皮肤需 CustomSkinLoader 才能在游戏内显示

## 许可

MIT。第三方组件：`json.hpp`（MIT, nlohmann）、`authlib-injector`（构建时自行提供）。
