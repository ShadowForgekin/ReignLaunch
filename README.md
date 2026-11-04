# ReignLaunch 官网（EdgeOne Pages 部署）

纯静态站 + 可选边缘函数。**下载不经过 GitHub**：用户从你自己的域名直接下 exe。

---

## ⚠️ 先修这个：不要只上传 index.html

如果你在 EdgeOne Pages 只上传了 `index.html`，那么：

- `/assets/style.css`、`/shots/*.jpg` 全部会**回退成首页 HTML**（表现为页面没样式、截图裂）
- `/api/*` 也不存在

判断方法：浏览器打开 `https://你的域名/assets/app.js`，如果返回的是一堆 HTML（首页源码）而不是 JS，就是没有上传完整。

**必须上传整个目录**（见下文「部署」）。

---

## 目录结构

```
reignlaunch-site/
├─ index.html
├─ 404.html
├─ assets/            style.css / app.js / favicon.svg
├─ shots/             11 张界面截图
├─ download/
│  └─ ReignLaunch.exe  ← 静态直下（方案 A）
└─ functions/api/
   ├─ latest.js        GET /api/latest    最新版本号 + 下载直链
   └─ download.js      GET /api/download  边缘代理，流式下载（方案 B）
```

---

## 下载的三种方式

站点前端按顺序自动探测，**第一个真能下的就用**：

| 顺序 | 地址 | 说明 | 需要什么 |
|---|---|---|---|
| 1 | `/download/ReignLaunch.exe` | 静态文件，EdgeOne CDN 直出，最快 | 把 exe 一起上传（7.2MB） |
| 2 | `/api/download` | 边缘函数代理 GitHub，同域名下流式下载，带 1 小时边缘缓存、支持断点续传 | Git 部署（函数才生效） |
| 3 | GitHub Release | 兜底 | 无 |

前端会先 `HEAD` 探测：如果返回 `text/html` 或长度 < 1MB，就判定为「不是真文件」（静态站回退陷阱），自动跳到下一档。

### 方案 A：静态托管（最推荐，最简单）

1. 把最新的 `ReignLaunch.exe` 放进 `download/` 目录
2. 整个站点一起上传 / push
3. 用户点下载 → 直接从 `https://你的域名/download/ReignLaunch.exe` 拉，走 EdgeOne CDN

> 每次发新版：替换 `download/ReignLaunch.exe` 这一个文件即可。
> EdgeOne Pages 对单文件大小有限制（免费版通常 25MB），7.2MB 没问题。

### 方案 B：边缘函数代理（不想每次传 exe）

用 Git 方式部署后，`/api/download` 会自动：

1. 查 GitHub 最新 Release
2. 边缘节点回源拉 exe
3. **缓存在边缘**，后续用户命中缓存
4. 透传 `Range`，支持下载器多线程 / 断点续传

用户看到的地址永远是 `https://你的域名/api/download`，不跳 GitHub。

---

## 部署

### 方式一：Git 导入（推荐，支持函数 + 自动更新）

```bash
cd reignlaunch-site
git init
git add -A
git commit -m "site: ReignLaunch official website"
git branch -M main
git remote add origin https://github.com/<你的账号>/reignlaunch-site.git
git push -u origin main
```

[EdgeOne Pages 控制台](https://console.cloud.tencent.com/edgeone/pages) → 创建项目 → 导入 Git 仓库 → 选择该仓库：

| 配置项 | 值 |
|---|---|
| 框架预设 | 无 / 静态站点 |
| 构建命令 | **留空** |
| 输出目录 | `/` |
| 根目录 | `/` |

部署完成后，`https://xxx.edgeone.app/` 能直接访问。

### 方式二：直接上传 zip（不支持函数）

把整个目录打成 zip（**内容要放在 zip 根**，不要多套一层文件夹），在控制台选择「直接上传」。此时只有方案 A 有效。

---

## 绑定自己的域名

1. 项目 → 域名管理 → 添加域名
2. 到域名商 / DNSPod 添加 CNAME，指向 EdgeOne 提供的接入地址
3. 等免费证书签发（1–3 分钟）→ 开启强制 HTTPS

---

## 改内容的位置

| 想改什么 | 改哪里 |
|---|---|
| 版本号兜底 | `index.html` 的 `#verTag` |
| 下载兜底地址 | `index.html` 的 `#dlBtn` / `#dlBtn2` |
| 下载候选顺序 | `assets/app.js` 的 `pickDownloadUrl()` |
| GitHub 仓库名 | `assets/app.js` 与 `functions/api/*.js` 顶部 `REPO` |
| 文案 / FAQ | `index.html` |
| 配色 | `assets/style.css` 顶部 `:root` |
| 截图 | 替换 `shots/*.jpg`（文件名不变即可） |

配色与启动器一致：背景 `#18191c`、主色 `#3b82f6`、卡片 `#24262b`。

---

## 本地预览

```bash
python -m http.server 8080
# 或
npx serve .
```

打开 http://localhost:8080 。
本地没有函数，`/api/latest` 和 `/api/download` 会 404，前端自动回退 —— 属正常。

---

## 上线检查清单

- [ ] `/assets/style.css` 返回 `text/css`（不是 HTML）→ 说明整站上传成功
- [ ] `/shots/ui_launch.jpg` 返回 `image/jpeg`
- [ ] `/download/ReignLaunch.exe` 返回 `application/octet-stream` 或 `x-msdownload`，大小 ~7.2MB
- [ ] 首页版本号显示为最新 Release 的 tag
- [ ] 点「下载 Windows 版」浏览器直接开始下载 exe，地址栏是**你自己的域名**
- [ ] 手机宽度下无横向滚动
- [ ] `/api/latest` 返回 JSON（Git 部署才有）
