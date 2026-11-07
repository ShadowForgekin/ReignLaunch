// ============================================================================
//  ReignLaunch — Minecraft 启动器（FLTK 版，单文件源码）
//
//  从 Win32 GDI 版迁移而来，逻辑层与界面层合并在本文件内：
//    · 逻辑层：下载 / 安装 / 启动 / 账户 / Java / 配置
//    · 界面层：FLTK 1.4.5，6 个页面 + 沉浸式启动浮层
//    · 解压：miniz 3.1.2（替代 tar.exe 子进程）
//    · 中文：字体映射 + UTF-8 安全截断，修复乱码和半个汉字
//
//  编译：
//    gcc -O2 -c miniz.c -o miniz.o
//    windres res.rc -O coff -o res.o
//    g++ -std=c++17 -O2 -mwindows -static -static-libgcc -static-libstdc++ \
//        -finput-charset=UTF-8 -fexec-charset=UTF-8 -fwide-exec-charset=UTF-16LE \
//        -I. -IFL -o ReignLaunch.exe ReignLaunch.cpp miniz.o res.o \
//        -Wl,--start-group libfltk.a -Wl,--end-group \
//        -lwinhttp -lshell32 -ladvapi32 -lole32 -luuid -lgdi32 -lcomctl32 \
//        -loleaut32 -lshlwapi -lcomdlg32 -lgdiplus -lwinspool -lws2_32
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <regex>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "json.hpp"
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Group.H>
#include <FL/fl_ask.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/fl_draw.H>
#include <FL/x.H>   // fl_xid()

#include "miniz.h"

#include <memory>
#include <cmath>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ============================================================================
//  第一部分：类型与全局状态
// ============================================================================
#define IDR_AUTHLIB    101
#define IDI_APP        100

// ============================ 页面 ============================
enum Page {
    PAGE_LAUNCH = 0, PAGE_USER = 1, PAGE_SETTINGS = 2,
    PAGE_ABOUT = 3, PAGE_INSTANCES = 4, PAGE_DOWNLOAD = 5, PAGE_MODS = 6
};

// ============================ 配置 ============================
struct Account {
    std::string type;    // offline / thirdparty / microsoft
    std::string name;
    std::string server;
    std::string token;
    std::string uuid;
    std::string skin;    // 该账户的皮肤 PNG
    std::string refresh; // 微软账号的 refresh_token
};

struct Config {
    std::vector<Account> accounts;
    int activeAcc = -1;
    std::string gameDir;
    std::string skinPath;
    int  isoMode = 0;
    int  dlThreads = 16;
    std::string extraJvm;
    std::string extraGame;
    int  dlSource = 0;               // 0=自动 1=仅官方 2=仅镜像
    int  winMode = 0;                // 0=窗口 1=最大化 2=全屏
    std::string javaPath;
    int         maxMemMB = 4096;
    std::string playerName = "Steve";

    std::string authType = "offline";   // offline | thirdparty | microsoft
    std::string authServer;
    std::string authToken;
    std::string authClientToken;
    std::string authName;
    std::string authUuid;
    std::string authlibPath;

    std::string cfProxy  = "https://api.curse.tools/v1/cf";
    std::string cfApiKey;
    std::string msClientId = "00000000402b5328";
};

// ============================ 路径安全包装 ============================
// g_cfg.gameDir 为空时 fs::u8path("") 在部分 MinGW 版本会抛异常导致闪退。
// 统一用这两个函数代替直接调用 u8path。
static fs::path SafePath(const std::string& u8) {
    if (u8.empty()) return fs::path();
    return fs::u8path(u8);
}


// ============================ 全局状态（逻辑侧）============================

extern Config      g_cfg;
extern std::string g_dataDir;
extern std::string g_cfgPath;

// g_cfg.gameDir 的安全版本（空字符串时返回空 path，不抛异常）
static fs::path GameDirPath() { return SafePath(g_cfg.gameDir); }

extern std::vector<std::string> g_versions;
extern int         g_selVersion;
extern Page        g_page;
extern std::atomic<bool> g_launching;
// 工作线程下载完成后不直接改 g_versions（会和 UI 线程遍历冲突），
// 而是置这个标志，由 ui_tick 在 UI 线程里调 ScanVersions()。
extern std::atomic<bool> g_needScanVersions;

extern std::atomic<int>  g_progState;   // 0空闲 1补全 2启动
extern std::atomic<int>  g_progCur;
extern std::atomic<int>  g_progTotal;
extern std::atomic<int>  g_stepCur;     // 已完成步骤 0..4
extern std::atomic<bool> g_cancelLaunch;

extern DWORD        g_overlayDoneTick;

extern std::wstring g_status;           // 界面提示文字（UI 层负责显示）

// 窗口句柄：核心逻辑用它 PostMessage 通知 UI。FLTK 版可设为 nullptr。
extern HWND g_hwnd;

// ============================ 消息 ============================
#define WM_APP_DONE      (WM_APP + 1)
#define WM_APP_ERROR     (WM_APP + 2)
#define WM_APP_NEEDJAVA  (WM_APP + 9)
#define WM_APP_UPD       (WM_APP + 11)
#define WM_APP_SYNCUI    (WM_APP + 12)


// 对外 API 见 core_api.h（需在 core_state.h 之后包含）


// ============================================================================
//  第二部分：共享数据结构
// ============================================================================
// ============================================================================
//  core_state.h — ReignLaunch 逻辑层共享状态与数据结构
//  被 core.h（对外）与 core.cpp（实现）共同包含
// ============================================================================

// ---------------- 版本清单 ----------------
struct McVer {
    std::string id, type, url, date;
};

// ---------------- 模组 ----------------
struct ModHit { std::string slug, title, desc; long long downloads = 0; };

// ---------------- 认证 ----------------
struct AuthResult {
    bool ok = false;
    std::string err;
    std::string accessToken, clientToken, uuid, name;
};

// ---------------- 启动 ----------------
struct LaunchPlan {
    std::wstring javaPath, commandLine, workDir;
    int maxMem = 4096;
};

// ---------------- 下载 ----------------
struct DownloadStats { int existed = 0, downloaded = 0, failed = 0; };
struct DLTask { std::string url; fs::path dest; std::string label; std::string sha1; };

// ---------------- Java 候选 ----------------
struct Cand { std::string path; std::string label; };

// ============================ 全局状态 ============================
extern std::vector<McVer> g_mcVers;
extern std::atomic<int>   g_mcVersState;
extern int                g_dlFilter;
extern std::vector<int>   g_dlView;
extern int                g_dlSel;
extern bool               g_dlDetail;
extern std::string        g_dlDetailMc;

extern std::atomic<int>   g_ldState[4];
extern std::wstring       g_ldVer[4];
extern int                g_dlLoader;
extern std::vector<std::string> g_ldVers;
extern std::vector<std::string> g_ldFiles;
extern int                g_ldSel;
extern std::atomic<int>   g_ldVersState;
extern std::atomic<int>   g_ldGen;

extern std::atomic<bool>  g_dlBusy;
extern std::atomic<bool>  g_dlCancel;

extern int                g_userMode;

extern std::vector<ModHit>      g_modHits;
extern std::mutex          g_modHitsMutex;   // 保护 g_modHits（后台线程写）
extern std::vector<std::string> g_modFiles;
extern bool               g_modDetail;
extern std::string        g_modDetailSlug, g_modDetailTitle, g_modDetailDesc;
extern std::vector<std::string> g_modVerNames, g_modVerFiles;
extern std::mutex                g_modVerMutex;
extern std::atomic<int>   g_modVerState;
extern std::atomic<int>   g_modVerGen;   // 防止旧请求覆盖新结果
extern int                g_modVerSel;
extern std::atomic<int>   g_modState;
extern std::atomic<int>   g_modGen;
extern int                g_modRunGen;
extern std::atomic<bool>  g_cfFallback;
extern int                g_modSel;
extern int                g_modFileSel;
extern int                g_modTab;
extern int                g_modSource;
extern int                g_modCat;      // 0=Mod 1=整合包 2=数据包 3=资源包 4=光影包
extern int                g_dlSection;
extern int                g_instTab;

extern std::vector<Cand>  g_dirCands, g_javaCands;

// 下载进度（UI 读取）
extern std::atomic<long long> g_dlBytes;
extern std::wstring           g_dlTarget;
extern long long              g_dlLastBytes;
extern DWORD                  g_dlLastTick;
extern double                 g_dlSpeed;

// 游戏进程
extern HANDLE             g_gameProc;
extern std::atomic<DWORD> g_gamePid;

// 列表滚动位置（UI 用，但放在这里共享）
extern int g_listScroll;

// 登录状态
extern std::atomic<int> g_loginState;   // 0空闲 1进行中 2成功 3失败


// ============================================================================
//  第三部分：对外接口声明
// ============================================================================
// ============================================================================
//  core_api.h — ReignLaunch 逻辑层对外接口
//  使用前需先包含 core_state.h（提供 DLTask / DownloadStats / AuthResult 等类型）
// ============================================================================

// ============================ 对外 API ============================
// 核心逻辑全部以普通函数导出，UI 层直接调用（无需 static）

std::wstring U8ToW(const std::string& s);
std::string  WToU8(const std::wstring& w);
bool         FileExists(const fs::path& p);
bool         DirExists(const fs::path& p);
std::string  TrimSlash(std::string s);
std::string  ExtractEmbeddedAuthlib();
std::string  DetectAuthlib();
std::string  DetectJava();
std::string  DetectGameDir();

void         Log(const std::string& s);
std::string  LogTail(int n);

void         SaveConfig();
void         SaveConfig();
void         LoadConfig();


void         ScanVersions();
json         ReadJsonFile(const fs::path& p);
json         ResolveVersion(const std::string& id, int depth = 0);
bool         RulesAllow(const json& rules);
bool         ShouldIsolate(const std::string& id, const fs::path& mcDir);
bool         IsModdedInstance(const std::string& id, const fs::path& mcDir);

bool         ExtractZip(const fs::path& zip, const fs::path& outDir);
std::string  Sha1Hex(const std::vector<unsigned char>& data);
std::string  Sha1OfFile(const fs::path& p);
std::string  ToMirror(std::string url);
bool         HttpRequest(const std::string& urlU8, const std::string& method,
                         const std::string& body, const std::string& contentType,
                         std::vector<unsigned char>& out, std::string& err);
bool         DownloadFile(const std::string& url, const fs::path& dest, std::string& err);
void         RunDownloads(std::vector<DLTask>& tasks, DownloadStats& st, int threads = 0);
void         DownloadMissing(const std::string& versionId, DownloadStats& st);
int          CountMissing(const std::string& versionId);

AuthResult   YggdrasilLogin(const std::string& serverRaw, const std::string& user, const std::string& pass);
std::string  OfflineUUID(const std::string& name);
int          UpsertAccount(const std::string& type, const std::string& name,
                           const std::string& server = "", const std::string& token = "",
                           const std::string& uuid = "");
std::string  CurrentPlayerName();
void         CaptureActiveAccount();

std::string  JavaVersionOf(const fs::path& javaw);
int          RequiredJavaMajor(const std::string& id);
std::string  PickJavaFor(int need);
bool         InstallJava(int need);
fs::path     FindJavaIn(const fs::path& root);
void         RefreshJavaCands();
void         RefreshDirCands();

bool         BuildLaunchPlan(const std::string& versionId, LaunchPlan& plan, std::string& error);

// 启动（在工作线程里调用）
void         InstallVersionThread(std::string id, std::string url);  // 安装版本
void         FetchManifestThread();                                  // 拉取版本清单
void         FetchLoaderVersThread(int kind, int gen);               // 拉取加载器版本
void         InstallMetaLoaderThread(std::string mc, bool quilt, std::string wantVer);   // Fabric/Quilt
void         InstallInstallerThread(std::string mc, bool neo, std::string wantVer);      // Forge/NeoForge
void         LaunchThread(std::string versionId);      // 实际启动线程
void         MicrosoftLoginThread();                   // 微软登录线程
void         LaunchGame(std::string versionId);
void         MicrosoftLoginSimple();      // 微软设备码登录

// 账户操作（由 core_extra.cpp 提供）
void         CreateOfflineAccount(const std::string& name);
bool         ThirdPartyLogin(const std::string& server, const std::string& user,
                             const std::string& pass, std::string& err);
void         SelectAccount(int idx);
void         RemoveAccount(int idx);

// 杂项
void         ReloadVersions();
void         SetGameDir(const std::string& dir);


// 模组
fs::path     InstanceModsDir();
std::string  CurrentMcVersion();
std::string  CurrentLoaderTag();
void         RefreshInstalledMods();
fs::path     ModsDirOf(const std::string& versionId);
std::vector<std::string> ListModsOf(const std::string& versionId);
void         ToggleModIn(const std::string& versionId, const std::string& fileName, std::string& err);
bool         DeleteModIn(const std::string& versionId, const std::string& fileName, std::string& err);

void         ToggleMod(const std::string& fileName, std::string& err);
bool         DeleteMod(const std::string& fileName, std::string& err);

// ============================================================================
//  第四部分：界面主题（配色 / 中文修复 / 绘制辅助）
// ============================================================================
// ============================================================================
//  ui_theme.h — FLTK 主题（配色与原 Win32 版一致）+ 中文字体修复
// ============================================================================

// 配色（与 Win32 版 C_* 一一对应）
inline Fl_Color clr_sidebar() { return fl_rgb_color(30, 31, 35); }   // 侧边栏底色
inline Fl_Color clr_bg()       { return fl_rgb_color(24, 25, 28); }   // C_BG
inline Fl_Color clr_inputbg()  { return fl_rgb_color(36, 38, 42); }   // C_INPUTBG
inline Fl_Color clr_header()   { return fl_rgb_color(31, 33, 37); }   // C_HEADER
inline Fl_Color clr_card()     { return fl_rgb_color(45, 48, 53); }   // C_CARD
inline Fl_Color clr_card_sel() { return fl_rgb_color(58, 62, 70); }   // C_CARD_SEL
inline Fl_Color clr_accent()   { return fl_rgb_color(37, 99, 235); }  // C_ACCENT
inline Fl_Color clr_accent2()  { return fl_rgb_color(59, 130, 246); } // C_ACCENT2
inline Fl_Color clr_text()     { return fl_rgb_color(232, 234, 237); }// C_TEXT
inline Fl_Color clr_dim()      { return fl_rgb_color(150, 155, 162); }// C_DIM
inline Fl_Color clr_ok()       { return fl_rgb_color(74, 222, 128); } // C_OK
inline Fl_Color clr_err()      { return fl_rgb_color(248, 113, 113); }// C_ERR

// ============================================================================
//  中文字体修复
//
//  问题根因：FLTK 默认用 FL_HELVETICA（Arial）绘制，Arial 不含汉字，
//  Windows GDI 会回退到别的字体，但 fl_width() 仍按半角算宽度。
//  结果：中文被裁成半个字，显示成 "?"（这就是"乱码没显示完全"）。
//
//  修法：把 FL_HELVETICA 槽位重映射到系统自带的中文字体。
// ============================================================================
inline void setup_cjk_fonts() {
    // 按优先级尝试（Win10/11 前两个一定存在）
    const char* candidates[] = {
        "Microsoft YaHei UI",
        "Microsoft YaHei",
        "SimHei",
        "SimSun",
        "Noto Sans CJK SC",
    };

    auto try_font = [](Fl_Font slot, const char* name) -> bool {
        Fl::set_font(slot, name);
        int attrs = 0;
        const char* got = Fl::get_font_name(slot, &attrs);
        if (!got) return false;
        // 名字能对上才算成功（FLTK 对不存在的字体静默保留旧名）
        return std::string(got).find(name) != std::string::npos ||
               std::string(name).find(got) != std::string::npos;
    };

    Fl_Font slots[4] = { FL_HELVETICA, FL_HELVETICA_BOLD,
                         FL_HELVETICA_ITALIC, FL_HELVETICA_BOLD_ITALIC };
    for (Fl_Font slot : slots) {
        for (const char* name : candidates) {
            if (try_font(slot, name)) break;
        }
    }
    // 等宽字体槽（日志之类）
    Fl::set_font(FL_COURIER, "Consolas");
    Fl::set_font(FL_SCREEN,  "Consolas");
}

// ============================================================================
//  绘制辅助
// ============================================================================
inline void fill_round_rect(int x, int y, int w, int h, int r, Fl_Color c) {
    if (w <= 0 || h <= 0) return;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    fl_color(c);
    if (r <= 0) { fl_rectf(x, y, w, h); return; }

    // 主体十字（不留缝）
    fl_rectf(x + r, y, w - r * 2, h);
    fl_rectf(x, y + r, r, h - r * 2);
    fl_rectf(x + w - r, y + r, r, h - r * 2);

    // 四角用 fl_pie 填充（已验证可靠）
    int d = r * 2;
    fl_pie(x,           y,           d, d,  90, 180);   // 左上
    fl_pie(x + w - d,   y,           d, d,   0,  90);   // 右上
    fl_pie(x + w - d,   y + h - d,   d, d, 270, 360);   // 右下
    fl_pie(x,           y + h - d,   d, d, 180, 270);   // 左下
}

inline void stroke_round_rect(int x, int y, int w, int h, int r, Fl_Color c) {
    fl_color(c);
    fl_line_style(FL_SOLID, 1);
    fl_arc(x, y, r * 2, r * 2, 90, 180);
    fl_arc(x + w - r * 2, y, r * 2, r * 2, 270, 360);
    fl_arc(x + w - r * 2, y + h - r * 2, r * 2, r * 2, 0, 90);
    fl_arc(x, y + h - r * 2, r * 2, r * 2, 180, 270);
    fl_line(x + r, y, x + w - r, y);
    fl_line(x + w, y + r, x + w, y + h - r);
    fl_line(x + r, y + h, x + w - r, y + h);
    fl_line(x, y + r, x, y + h - r);
    fl_line_style(0, 0);
}

// ---- UTF-8 字符宽度工具 ----
inline size_t utf8_char_len(unsigned char c) {
    if ((c & 0x80) == 0x00) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;   // 非法字节按 1 处理
}

// 按「完整字符」截断到指定像素宽度，超出补省略号
inline std::string utf8_ellipsize(const std::string& s, int pt, int maxW) {
    fl_font(FL_HELVETICA, pt);
    if (maxW <= 0) return "";
    if ((int)fl_width(s.c_str()) <= maxW) return s;

    int ellW = (int)fl_width("...");
    int budget = maxW - ellW;
    if (budget <= 0) return "";

    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        size_t len = utf8_char_len((unsigned char)s[i]);
        if (i + len > s.size()) break;
        std::string next = out + s.substr(i, len);
        if ((int)fl_width(next.c_str()) > budget) break;
        out = next;
        i += len;
    }
    return out.empty() ? "" : (out + "...");
}

// 单行文字：垂直居中；宽度够就整行画，不够就按完整字符截断
inline void draw_text(int x, int y, int w, int h, const char* s, Fl_Color c, int pt = 15) {
    if (!s || !*s || w <= 0 || h <= 0) return;
    fl_font(FL_HELVETICA, pt);
    std::string src(s);
    std::string out = ((int)fl_width(s) <= w) ? src : utf8_ellipsize(src, pt, w);
    if (out.empty()) return;
    fl_color(c);
    // 基线 = 区域顶部 + (高度 + 行高)/2 - descent，保证视觉垂直居中
    int by = y + (h + fl_height()) / 2 - fl_descent();
    fl_draw(out.c_str(), x, by);
}

inline void draw_text(int x, int y, int w, int h, const std::string& s, Fl_Color c, int pt = 15) {
    draw_text(x, y, w, h, s.c_str(), c, pt);
}

// 右对齐版本（列表里的日期列用）
inline void draw_text_right(int x, int y, int w, int h, const std::string& s, Fl_Color c, int pt = 15) {
    if (s.empty() || w <= 0 || h <= 0) return;
    fl_font(FL_HELVETICA, pt);
    std::string cut = utf8_ellipsize(s, pt, w);
    if (cut.empty()) return;
    fl_color(c);
    int tw = (int)fl_width(cut.c_str());
    int by = y + (h + fl_height()) / 2 - fl_descent();
    fl_draw(cut.c_str(), x + w - tw, by);
}

// 居中版本（按钮文字都走这里）
inline void draw_text_center(int x, int y, int w, int h, const std::string& s, Fl_Color c, int pt = 15) {
    if (s.empty() || w <= 0 || h <= 0) return;
    fl_font(FL_HELVETICA, pt);
    std::string cut = utf8_ellipsize(s, pt, w);
    if (cut.empty()) return;
    fl_color(c);
    int tw = (int)fl_width(cut.c_str());
    int bx = x + (w - tw) / 2;
    int by = y + (h + fl_height()) / 2 - fl_descent();
    fl_draw(cut.c_str(), bx, by);
}

// wstring → UTF-8
inline std::string w2u(const std::wstring& w) {
    if (w.empty()) return {};
    int n = (int)w.size() * 4 + 4;
    std::string out(n, '\0');
    int r = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    out.resize(r > 0 ? (size_t)r : 0);
    return out;
}

inline void draw_text(int x, int y, int w, int h, const std::wstring& s, Fl_Color c, int pt = 15) {
    draw_text(x, y, w, h, w2u(s), c, pt);
}

// ============================================================================
//  应用全局深色主题 + 中文字体
// ============================================================================
inline void apply_theme() {
    Fl::scheme(nullptr);
    Fl::background(24, 25, 28);
    Fl::background2(36, 38, 42);
    Fl::foreground(232, 234, 237);
    Fl::set_color(FL_SELECTION_COLOR, 37, 99, 235);
    setup_cjk_fonts();
}


// ============================================================================
//  第五部分：可复用滚动列表
// ============================================================================
class RowList : public Fl_Widget {
public:
    using Drawer = std::function<void(RowList*, int, int, int, int, int, bool)>;
    bool ignored = false;   // true 时完全不绘制、不响应

    RowList(int X, int Y, int W, int H, const char* L = nullptr)
        : Fl_Widget(X, Y, W, H, L) {
        box(FL_NO_BOX);
        color(clr_bg());
    }

    void setRowHeight(int h) { rowH_ = h; redraw(); }
    int  rowHeight() const { return rowH_; }
    void setDrawer(Drawer d) { drawer_ = std::move(d); redraw(); }
    void setMergeBottom(int px) { mergeBottom_ = px; }

    int count() const { return (int)rows_.size(); }
    int value() const { return sel_; }
    void value(int i) { sel_ = i; redraw(); }
    void clear() { rows_.clear(); sel_ = -1; scroll_ = 0; redraw(); }

    void add(const std::string& text, const std::string& sub = {}) {
        rows_.push_back({ text, sub });
    }
    const std::string& label(int i) const {
        static const std::string empty;
        return (i >= 0 && i < (int)rows_.size()) ? rows_[i].first : empty;
    }
    const std::string& sub(int i) const {
        static const std::string empty;
        return (i >= 0 && i < (int)rows_.size()) ? rows_[i].second : empty;
    }

    int scroll() const { return scroll_; }
    void scroll(int v) { scroll_ = v; clampScroll(); redraw(); }

    void draw() override {
        if (ignored) return;
        fl_push_clip(x(), y(), w(), h());
        fill_round_rect(x(), y(), w(), h(), 0, color());

        int top = y();
        int bot = y() + h();
        int yy = top - scroll_;

        for (int i = 0; i < (int)rows_.size(); ++i) {
            if (yy + rowH_ > top && yy < bot) {
                bool sel = (i == sel_);
                if (drawer_) {
                    drawer_(this, i, x(), yy, w(), rowH_, sel);
                } else {
                    // 默认样式
                    fill_round_rect(x(), yy + 4, w(), rowH_ - 8, 8,
                                    sel ? clr_card_sel() : clr_card());
                    if (sel) fill_round_rect(x() + 2, yy + 12, 3, rowH_ - 24, 0, clr_accent2());
                    draw_text(x() + 18, yy + 4, w() - 36, rowH_ - 8,
                              rows_[i].first.c_str(),
                              sel ? clr_text() : clr_dim(), 15);
                }
            }
            yy += rowH_;
        }

        fl_pop_clip();
    }

    int handle(int e) override {
        if (ignored) return 0;
        switch (e) {
        case FL_PUSH: {
            // 按 FLTK 官方文档：PUSH 只记录状态，RELEASE 才触发回调
            // （回调里可能切页/清空本控件，PUSH 里触发会导致后续 RELEASE 访问野指针）
            pressed_ = -1;
            if (Fl::event_button() == FL_LEFT_MOUSE) {
                int idx = hit(Fl::event_y());
                if (idx >= 0) {
                    pressed_ = idx;
                    if (sel_ != idx) { sel_ = idx; redraw(); }
                }
            }
            return 1;
        }
        case FL_RELEASE: {
            if (pressed_ >= 0) {
                int idx = pressed_;
                pressed_ = -1;
                // 确认鼠标还在同一行内
                if (hit(Fl::event_y()) == idx) {
                    do_callback();   // ★ 回调之后不能再访问任何成员
                    return 1;
                }
            }
            return 1;
        }
        case FL_DRAG:
            return 1;
        case FL_MOUSEWHEEL: {
            int maxS = maxScroll();
            if (maxS <= 0) return 1;
            scroll_ += Fl::event_dy() * rowH_;
            clampScroll();
            redraw();
            return 1;
        }
        case FL_ENTER:
            return 1;
        }
        return Fl_Widget::handle(e);
    }

private:
    struct Row { std::string first, second; };
    std::vector<Row> rows_;
    Drawer drawer_;
    int sel_ = -1;
    int pressed_ = -1;   // 按下时的行号（FL_RELEASE 时触发回调）
    int scroll_ = 0;
    int rowH_ = 52;
    int mergeBottom_ = 0;

    int visibleH() const { return h() - mergeBottom_; }

    int maxScroll() const {
        int contentH = (int)rows_.size() * rowH_;
        int v = visibleH();
        return contentH > v ? contentH - v : 0;
    }

    void clampScroll() {
        int m = maxScroll();
        if (scroll_ < 0) scroll_ = 0;
        if (scroll_ > m) scroll_ = m;
    }

    int hit(int py) const {
        int rel = py - y() + scroll_;
        if (rel < 0) return -1;
        int idx = rel / rowH_;
        return (idx >= 0 && idx < (int)rows_.size()) ? idx : -1;
    }
};


// ============================================================================
//  第六部分：沉浸式启动浮层
// ============================================================================
// clr_* / fill_round_rect / stroke_round_rect / draw_text / w2u

// ============================================================================
//  OverlayRoot — 浮层底下的「真透明」占位
//
//  被放在父窗口的第一个子控件、位置 (0,0)、尺寸等于父区域。FLTK 的叠加层会先画它
//  （半透明遮罩色），再画浮层，这就是真半透明。其余子控件仍由父窗口正常裁剪，
//  不受影响。
// ============================================================================
class OverlayRoot : public Fl_Widget {
public:
    explicit OverlayRoot(int X = 0, int Y = 0, const char* L = nullptr)
        : Fl_Widget(X, Y, 1, 1, L) {
        box(FL_NO_BOX);
        // 叠加层里 alpha 会被门限到 [0.05, 0.95]，所以这里画实色，由 gui_alpha 控制浓淡。
        color(fl_rgb_color(18, 19, 22));
    }

    void draw() override {
        fl_color(color());
        fl_rectf(x(), y(), w(), h());
    }

    int handle(int) override { return 0; }
};

// ============================================================================
//  Overlay
// ============================================================================
class Overlay : public Fl_Widget {
public:
    // ---- 几何常量 ----
    static const int CARD_W = 560;    // 卡片宽
    static const int CARD_H = 200;    // 卡片高
    static const int CARD_R = 12;     // 圆角
    static const int BTN_W  = 88;     // 按钮宽
    static const int BTN_H  = 28;     // 按钮高

    // ---- 回调 ----
    std::function<void()> onCancel;   // 点「取消」
    std::function<void()> onFinish;   // 点「关闭」
    std::function<void()> onClose;    // 浮层被关闭（按钮或 hide()）时调用

    // ================================================================ 构造
    explicit Overlay(int X = 0, int Y = 0, int W = 0, int H = 0,
                     const char* L = nullptr)
        : Fl_Widget(X, Y, W, H, L) {
        box(FL_NO_BOX);
        color(clr_bg());
        align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
        vis_ = 0;                    // 自己的标志，不碰 Fl_Widget::visible()
        done_.store(0);
        for (int i = 0; i < 4; ++i) stepDone_[i].store(0);
        if (W > 0 && H > 0) set_bounds(X, Y, W, H);
    }

    ~Overlay() override {
        leave_all();
        detach_root();
        if (parent()) parent()->remove(*this);
        clear_transparent_host();
    }

    // ================================================================ 显示
    // ================================ 下载模式 ================================
    // 用于「下载并安装」时显示：文件数进度 + 下载速度 + 已下载量
    bool dlMode_ = false;
    long long dlLastBytes_ = 0;
    long long dlSpeed_ = 0;          // 字节/秒
    int       dlTick_ = 0;

    void showDownload(const std::string& name) {
        show(name);
        dlMode_ = true;
        dlLastBytes_ = 0;
        dlSpeed_ = 0;
        dlTick_ = 0;
        redrawHost();
    }

    void setDownloadDone() {
        dlMode_ = false;
        setDone();
    }

    // 每帧更新速度（由 tick_cb 调用）
    void updateSpeed() {
        if (!dlMode_) return;
        ++dlTick_;
        if (dlTick_ % 6 == 0) {   // 60fps 下约每 100ms 采样一次
            long long now = g_dlBytes.load();
            long long delta = now - dlLastBytes_;
            if (delta < 0) delta = 0;
            dlLastBytes_ = now;
            // 平滑：新值占 60%
            dlSpeed_ = (dlSpeed_ * 4 + delta * 6) / 10;
        }
    }

    static std::string fmtSize(long long b) {
        char buf[48];
        if (b >= 1073741824LL)      std::snprintf(buf, sizeof(buf), "%.2f GB", b / 1073741824.0);
        else if (b >= 1048576LL)    std::snprintf(buf, sizeof(buf), "%.1f MB", b / 1048576.0);
        else if (b >= 1024LL)       std::snprintf(buf, sizeof(buf), "%.0f KB", b / 1024.0);
        else                        std::snprintf(buf, sizeof(buf), "%lld B", b);
        return buf;
    }

    void show(const std::string& versionName) {
        Fl_Group* pg = parent();
        if (!pg) {                          // 没挂父窗口：挂到第一个窗口上
            Fl_Window* w = Fl::first_window();
            if (!w) return;
            w->add(this);
            pg = w;
        }
        suppress_transparent_host();

        version_   = versionName;
        done_.store(0);
        press_     = 0;
        baseStep_  = clampStep(g_stepCur.load());
        for (int i = 0; i < 4; ++i) stepDone_[i].store(baseStep_ > i ? 1 : 0);

        vis_ = 1;
        Fl_Widget::show();
        fit_to(pg);
        start_anim();
        redrawHost();
    }

    // ================================================================ 隐藏
    void hide() {
        if (!vis_) return;
        vis_   = 0;
        press_ = 0;
        leave_all();
        Fl_Widget::hide();
        detach_root();
        clear_transparent_host();
        redrawHost();
        auto cb = onClose;
        if (cb) cb();
    }

    bool visible() const { return vis_ != 0; }

    // ================================================================ 完成
    // 标记完成：四步全部画绿、按钮从「取消」变「关闭」
    void setDone() {
        done_.store(1);
        baseStep_ = 4;
        for (int i = 0; i < 4; ++i) stepDone_[i].store(1);
        redrawHost();
    }

    bool done() const { return done_.load() != 0; }

    // ---- 额外的小工具 ----
    void setVersion(const std::string& v) { version_ = v; redrawHost(); }
    const std::string& version() const { return version_; }

    // 手动把已完成的步骤标为 n（0..4），仍保持「进行中」
    void setStep(int n) {
        int s = clampStep(n);
        baseStep_ = s;
        for (int i = 0; i < 4; ++i)
            if (s > i) stepDone_[i].store(1);
        redrawHost();
    }

    // ================================================================ 绘制
    void draw() override {
        if (!vis_) return;
        if (!cardValid_) set_bounds(x(), y(), w(), h());

        int bx = 0, by = 0, bw = 0, bh = 0;
        button_rect(bx, by, bw, bh);

        // ---------- 1. 遮罩 ----------
        // 有 OverlayRoot 时由叠加层负责；退化模式自己画实色遮罩
        if (!root_) {
            fl_color(fl_rgb_color(18, 19, 22));
            fl_rectf(x(), y(), w(), h());
        }

        // ---------- 2. 卡片 ----------
        draw_shadow();
        fill_round_rect(card_.x, card_.y, card_.w, card_.h, CARD_R, clr_card());
        stroke_round_rect(card_.x, card_.y, card_.w, card_.h, CARD_R,
                          fl_rgb_color(62, 66, 74));

        // ---------- 3. 标题 ----------
        const int L  = card_.x + 24;
        const int IW = card_.w - 48;
        std::string title = dlMode_ ? ("正在下载 " + version_ + " …")
                                       : (version_.empty() ? std::string("正在启动 …")
                                                          : ("正在启动 " + version_ + " …"));

        // ---------- 4. 底部：进度条 / 百分比 / 状态 ----------
        const long long cur = g_progCur.load();
        const long long tot = g_progTotal.load();
        double ratio = 0.0;
        const bool showBar = (tot > 0);
        if (showBar) ratio = (double)cur / (double)tot;
        if (ratio < 0.0) ratio = 0.0;
        if (ratio > 1.0) ratio = 1.0;

        const int barX = L, barW = IW, barY = card_.y + card_.h - 40;
        fill_round_rect(barX, barY, barW, 5, 2, fl_rgb_color(66, 71, 78));
        if (showBar && ratio > 0.0) {
            int fw = (int)std::lround(barW * ratio);
            if (fw < 5) fw = 5;
            fill_round_rect(barX, barY, fw, 5, 2, clr_accent2());
        } else if (!showBar && !done_.load()) {
            // 没有总量时画一段流动的虚光，表示「还在动」
            int phase = (frame_ / 3) % (barW + 120) - 60;
            int sx = barX + std::max(0, phase);
            int ex = std::min(barX + barW, sx + 60);
            if (ex > sx) fill_round_rect(sx, barY, ex - sx, 5, 2, fl_rgb_color(84, 92, 104));
        }

        if (showBar) {
            char pc[32];
            std::snprintf(pc, sizeof(pc), "%d%%", (int)std::lround(ratio * 100.0));
            draw_text(barX + barW - 52, barY - 6, 52, 18, pc, clr_text(), 12);

            // 下载模式：显示文件数 + 速度 + 已下载字节
            if (dlMode_) {
                long long bytes = g_dlBytes.load();
                std::string info = std::to_string(cur) + " / " + std::to_string(tot) + " 个文件";
                if (bytes > 0) info += "    ·    " + fmtSize(bytes);
                // 速度用 B/s 转成 KB/s 或 MB/s
                long long sp = dlSpeed_ * 6;   // 100ms 采样 → 每秒
                if (sp > 0) info += "  (" + fmtSize(sp) + "/s)";
                draw_text(barX, barY - 6, barW - 60, 18, info, clr_dim(), 11);
            }
        }

        // 状态文字：g_status 是 std::wstring，交给 ui_theme.h 的 w2u() 转 UTF-8
        std::string st8;
        {
            std::lock_guard<std::mutex> lk(status_mtx());
            st8 = w2u(g_status);
        }
        if (st8.empty()) st8 = done_.load() ? "启动完成" : "正在准备启动…";
        draw_text(L, card_.y + card_.h - 26, IW - 104, 18, st8,
                  done_.load() ? clr_ok() : clr_dim(), 12);

        // ---------- 5. 四步 ----------
        static const char* kNames[4] = {
            "检测 Java 版本", "处理游戏依赖", "登录", "等待游戏启动"
        };
        const int stepTop = card_.y + 42;
        const int rowH    = (card_.y + card_.h - 34 - stepTop) / 4;
        const bool finished = done_.load() != 0;

        for (int i = 0; i < 4; ++i) {
            const int ry = stepTop + i * rowH;
            const int iy = ry + (rowH - 16) / 2;
            const bool fin    = stepDone_[i].load() != 0;
            const bool active = !fin && !finished && (baseStep_ <= i);

            // 图标
            if (fin) {
                fill_round_rect(L, iy, 16, 16, 8, clr_ok());
                fl_color(fl_rgb_color(20, 22, 25));
                fl_font(FL_HELVETICA_BOLD, 11);
                fl_draw("\xE2\x9C\x93", L, iy, 16, 16, FL_ALIGN_CENTER | FL_ALIGN_INSIDE);   // UTF-8 "✓"
            } else if (active) {
                draw_spinner(L + 8.0, iy + 8.0, 7.0);
            } else {
                // 未开始：空心灰圈，文字为灰色
                stroke_round_rect(L, iy, 16, 16, 8, fl_rgb_color(92, 98, 106));
            }

            // 文字
            Fl_Color tc = fin ? clr_dim()
                              : (active ? clr_text() : fl_rgb_color(118, 124, 132));
            draw_text(L + 28, ry, IW - 160, rowH, kNames[i], tc, 14);

            // 行尾状态
            if (fin)         draw_text(card_.x + card_.w - 152, ry, 128, rowH, "完成",   clr_ok(), 12);
            else if (active) draw_text(card_.x + card_.w - 152, ry, 128, rowH, "进行中…", clr_dim(), 12);
        }

        // ---------- 6. 按钮 ----------
        draw_button(bx, by, bw, bh);
    }

    // ================================================================ 事件
    int handle(int e) override {
        if (!vis_) return 0;

        int bx = 0, by = 0, bw = 0, bh = 0;
        button_rect(bx, by, bw, bh);
        const bool hot = hit(bx, by, bw, bh);

        switch (e) {
        case FL_ENTER:
        case FL_MOVE:
            if (hot && press_ == 0)       { press_ = 1; redrawHost(); }
            else if (!hot && press_ == 1) { press_ = 0; redrawHost(); }
            return 1;
        case FL_LEAVE:
            if (press_ == 1) { press_ = 0; redrawHost(); }
            return 1;
        case FL_PUSH:
            press_ = hot ? 2 : 0;
            redrawHost();
            return 1;
        case FL_RELEASE: {
            int was = press_;
            press_ = hot ? 1 : 0;
            redrawHost();
            if (was == 2 && hot) fire();
            return 1;
        }
        case FL_DRAG:
            return 1;                    // 进行中不响应窗口拖动
        case FL_SHORTCUT:
            if (Fl::event_key() == FL_Escape) {
                if (done_.load()) fire();
                return 1;
            }
            return 1;
        default:
            return 1;                    // 吞掉全部事件，避免穿透到底下的界面
        }
    }

    void resize(int X, int Y, int W, int H) override {
        Fl_Widget::resize(X, Y, W, H);
        if (vis_) fit_to(parent());
        else     set_bounds(X, Y, W, H);
    }

private:
    // ---------------------------------------------------------------- 数据
    struct Rect { int x = 0, y = 0, w = 0, h = 0; };

    Rect         card_;
    std::string  version_;
    Fl_Group*    host_    = nullptr;   // 浮层所在的组（父区域）
    OverlayRoot* root_    = nullptr;   // 半透明占位
    Fl_Window*   alphaHost_ = nullptr; // 被临时改回不透明的窗口
    int          savedAlpha_ = 0;
    bool         cardValid_  = false;
    int          press_      = 0;      // 0 普通 / 1 悬停 / 2 按下
    int          baseStep_   = 0;
    int          animOn_     = 0;
    int          frame_      = 0;      // 静态帧计数（UI 线程写）

    std::atomic<int> vis_{ 0 };
    std::atomic<int> done_{ 0 };
    std::atomic<int> stepDone_[4] = {};// 每步是否已完成

    static std::mutex& status_mtx() {
        static std::mutex m;
        return m;
    }

    // ---------------------------------------------------------------- 父区域
    void fit_to(Fl_Group* pg) {
        if (!pg || (Fl_Widget*)pg == (Fl_Widget*)this) return;
        OverlayRoot* r = find_root(pg);
        if (!r) {
            r = new OverlayRoot(pg->x(), pg->y());
            pg->insert(*r, 0);           // 第一个子控件 → 叠底层
        }
        r->position(pg->x(), pg->y());
        r->size(pg->w(), pg->h());
        r->show();

        root_ = r;
        host_ = pg;
        Fl_Widget::resize(pg->x(), pg->y(), pg->w(), pg->h());
        set_bounds(pg->x(), pg->y(), pg->w(), pg->h());
        pg->init_sizes();
    }

    static OverlayRoot* find_root(Fl_Group* g) {
        if (!g) return nullptr;
        for (int i = 0; i < g->children(); ++i) {
            OverlayRoot* r = dynamic_cast<OverlayRoot*>(g->child(i));
            if (r) return r;
        }
        return nullptr;
    }

    void detach_root() {
        if (root_) {
            root_->hide();
            if (root_->parent()) root_->parent()->remove(*root_);
            root_ = nullptr;
        }
        host_ = nullptr;
    }

    // 中央卡片：560x200，父区域放不下时收缩
    void set_bounds(int X, int Y, int W, int H) {
        card_.w = (W >= CARD_W + 32) ? CARD_W : (W > 64 ? W - 32 : W);
        card_.h = (H >= CARD_H + 32) ? CARD_H : (H > 64 ? H - 32 : H);
        card_.x = X + (W - card_.w) / 2;
        card_.y = Y + (H - card_.h) / 2;
        cardValid_ = true;
    }

    void button_rect(int& bx, int& by, int& bw, int& bh) const {
        bw = BTN_W;
        bh = BTN_H;
        bx = card_.x + card_.w - 24 - bw;
        by = card_.y + card_.h - 35;
    }

    static bool hit(int bx, int by, int bw, int bh) {
        int ex = Fl::event_x(), ey = Fl::event_y();
        return ex >= bx && ex < bx + bw && ey >= by && ey < by + bh;
    }

    // ---------------------------------------------------------------- 透明窗口兜底
    // 浮层不能挂在一个半透明窗口里（整个窗口都会跟着淡下去），临时改回不透明。
    void suppress_transparent_host() {
        Fl_Group* pg = parent();
        Fl_Window* w = pg ? pg->window() : nullptr;
        if (!w || w == pg) return;
        // 没有 gui_alpha() 公接口时这里保持 0，窗口本来就该是不透明的。
        (void)w;
    }

    void clear_transparent_host() {
        (void)alphaHost_;
        (void)savedAlpha_;
        alphaHost_  = nullptr;
        savedAlpha_ = 0;
    }

    // ---------------------------------------------------------------- 绘制细节
    void draw_shadow() {
        for (int i = 6; i >= 1; --i)
            stroke_round_rect(card_.x - i, card_.y - i, card_.w + i * 2, card_.h + i * 2,
                              CARD_R + i, fl_rgb_color(16, 17, 20));
    }

    // 当前步骤前面的小圆圈：12 段圆环 + 中心点，靠 frame_ 旋转
    void draw_spinner(double cx, double cy, double r) {
        const int    SEG = 12;
        const double PI  = 3.14159265358979323846;
        int off = (frame_ / 3) % SEG;
        fl_line_style(FL_SOLID, 2);
        for (int i = 0; i < SEG; ++i) {
            int k = (i + off) % SEG;
            int v = 70 + k * 15;                 // 亮度随段递增 → 旋转感
            if (v > 232) v = 232;
            fl_color(fl_rgb_color((uchar)v, (uchar)v, (uchar)v));
            double a1 = -PI / 2 + i * (2 * PI / SEG);
            double a2 = a1 + (2 * PI / SEG) * 0.62;
            fl_line(cx + r * std::cos(a1), cy + r * std::sin(a1),
                    cx + r * std::cos(a2), cy + r * std::sin(a2));
        }
        fl_line_style(0, 0);
        fl_color(clr_text());
        fl_pie(cx - 2.0, cy - 2.0, 4.0, 4.0, 0.0, 360.0);
    }

    void draw_button(int bx, int by, int bw, int bh) {
        const bool fin = done_.load() != 0;
        Fl_Color bg;
        if (fin) {
            bg = (press_ == 1) ? clr_accent() : clr_accent2();
            if (press_ == 2) bg = clr_card_sel();
        } else {
            bg = (press_ == 1) ? fl_rgb_color(72, 77, 86) : fl_rgb_color(58, 62, 70);
            if (press_ == 2) bg = fl_rgb_color(48, 51, 57);
        }
        fill_round_rect(bx, by, bw, bh, 8, bg);
        fl_font(FL_HELVETICA, 14);
        fl_color(FL_WHITE);
        fl_draw(fin ? "关闭" : "取消", bx, by, bw, bh, FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
    }

    // ---------------------------------------------------------------- 动画
    static int& frame_ref() { static int f = 0; return f; }

    void start_anim() {
        if (animOn_) return;
        animOn_ = 1;
        Fl::add_timeout(1.0 / 60.0, &Overlay::tick_cb, this);
    }

    void leave_all() {
        animOn_ = 0;
        Fl::remove_timeout(&Overlay::tick_cb, this);
    }

    static void tick_cb(void* u) {
        Overlay* o = static_cast<Overlay*>(u);
        if (!o || !o->vis_) { if (o) o->leave_all(); return; }
        o->frame_ = ++frame_ref();
        if (o->host_) o->host_->redraw();
        o->redraw();
        Fl::repeat_timeout(1.0 / 60.0, &Overlay::tick_cb, o);
    }

    // ---------------------------------------------------------------- 其它
    void redrawHost() {
        if (host_) host_->redraw();
        Fl_Widget::redraw();
    }

    static int clampStep(int s) { return s < 0 ? 0 : (s > 4 ? 4 : s); }

    // 点按钮：先 hide()（触发 onClose），再按状态调用 onCancel / onFinish
    void fire() {
        const bool fin = done_.load() != 0;
        if (!fin && g_launching) g_cancelLaunch = true;   // 通知逻辑层取消
        press_ = 0;
        auto cancel = onCancel;
        auto finish = onFinish;
        hide();
        if (fin) { if (finish) onFinish(); }
        else     { if (cancel) onCancel(); }
    }
};


// ============================================================================
//  第七部分：逻辑层实现
// ============================================================================
// ============================================================================
//  core.cpp — ReignLaunch 逻辑层（从 Win32 版 ReignLaunch.cpp 抽取）
//  不含任何界面代码：下载 / 安装 / 启动 / 账户 / Java / 配置
// ============================================================================

// ============================ 全局定义 ============================
Config      g_cfg;
std::string g_dataDir;
std::string g_cfgPath;

std::vector<std::string> g_versions;
int         g_selVersion = -1;
Page        g_page = PAGE_LAUNCH;
std::atomic<bool> g_launching{ false };
std::atomic<bool> g_needScanVersions{ false };

std::atomic<int>  g_progState{ 0 };
std::atomic<int>  g_progCur{ 0 };
std::atomic<int>  g_progTotal{ 0 };
std::atomic<int>  g_stepCur{ 0 };
std::atomic<bool> g_cancelLaunch{ false };

DWORD        g_overlayDoneTick = 0;
std::wstring g_status;
HWND         g_hwnd = nullptr;

static std::mutex              g_logMutex;
static std::vector<std::string> g_logs;

// ---------------- 前置声明（定义在文件后面）----------------
bool FileExists(const fs::path& p);
bool DirExists(const fs::path& p);
std::string JavaVersionOf(const fs::path& javaw);
int RequiredJavaMajor(const std::string& id);
std::string PickJavaFor(int need);
json ResolveVersion(const std::string& id, int depth);
bool RulesAllow(const json& rules);
bool HttpRequest(const std::string& urlU8, const std::string& method,
                        const std::string& body, const std::string& contentType,
                        std::vector<unsigned char>& out, std::string& err);
std::string ToMirror(std::string url);
bool DownloadFile(const std::string& url, const fs::path& dest, std::string& err);
static bool Sha1OfFileExists(const char* p);
std::string Sha1Hex(const std::vector<unsigned char>& data);
bool BuildLaunchPlan(const std::string& versionId, LaunchPlan& plan, std::string& error);
void DownloadMissing(const std::string& versionId, DownloadStats& st);
int CountMissing(const std::string& versionId);
bool InstallJava(int need);
static void PumpOutput(HANDLE hRead);
static bool WaitForGameWindow(DWORD pid, int timeoutMs);


bool ShouldIsolate(const std::string& id, const fs::path& mcDir);
fs::path InstanceModsDir();
std::string CurrentMcVersion();
std::string CurrentLoaderTag();
void RefreshInstalledMods();
void ScanVersions();
void SaveConfig();
void LoadConfig();
std::wstring U8ToW(const std::string& s);
std::string WToU8(const std::wstring& w);
std::string ExtractEmbeddedAuthlib();
std::string DetectAuthlib();
std::string OfflineUUID(const std::string& name);
void Log(const std::string& s);
void InstallModrinthModThread(std::string slug, std::string mcVer, std::string loaderTag, std::string targetVer);

// 等游戏窗口
struct WinWaitCtx { DWORD pid; bool found; };

// 微软登录状态（UI 读取并显示）
static std::wstring     g_msCode;
static std::wstring     g_msUrl;
static std::atomic<int> g_msState{ 0 };   // 0空闲 1进行中 2等待用户 3成功 4失败
static std::wstring     g_msMsg;

// GDI+ 状态
static ULONG_PTR g_gpToken = 0;
static bool      g_gpOk = false;

// 自动更新状态
#define APP_TAG     L"v1.3.0-rc1"      // 用于更新检查比对（GitHub tag）
#define APP_VER_STR "1.3.0 RC 1"       // 界面显示用
static std::wstring        g_newTag;
static std::string         g_newUrl;
static std::atomic<int>    g_updateState{ 0 };
static std::atomic<bool>   g_updating{ false };





// 前置声明
void RefreshJavaCands();
static void RebuildDlView();

// 模组搜索框由 UI 层提供，core 里用弱引用读取
static HWND g_hEdSearch = nullptr;

#define WM_APP_LOGIN (WM_APP + 13)

// 皮肤位图（UI 使用）
static HBITMAP g_skinBmp = nullptr;
static void LoadSkinBitmap() { /* FLTK 版不用 HBITMAP，空实现 */ }



std::vector<McVer> g_mcVers;
std::mutex g_mcVersMutex;   // 保护 g_mcVers（工作线程写、UI 线程读）
std::atomic<int>   g_mcVersState{ 0 };
int                g_dlFilter = 0;
std::vector<int>   g_dlView;
int                g_dlSel = -1;
bool               g_dlDetail = false;
std::string        g_dlDetailMc;

std::atomic<int>   g_ldState[4] = { {0}, {0}, {0}, {0} };
std::wstring       g_ldVer[4];
int                g_dlLoader = 0;
std::vector<std::string> g_ldVers;
std::vector<std::string> g_ldFiles;
int                g_ldSel = -1;
std::atomic<int>   g_ldVersState{ 0 };
std::atomic<int>   g_ldGen{ 0 };

std::atomic<bool>  g_dlBusy{ false };
std::atomic<bool>  g_dlCancel{ false };
int                g_userMode = 0;

std::vector<ModHit>      g_modHits;
std::mutex                 g_modHitsMutex;
std::vector<std::string> g_modFiles;
bool               g_modDetail = false;
std::string        g_modDetailSlug, g_modDetailTitle, g_modDetailDesc;
std::vector<std::string> g_modVerNames, g_modVerFiles;
std::mutex                g_modVerMutex;   // 保护上面两个 vector（后台线程写）
std::atomic<int>   g_modVerState{ 0 };
std::atomic<int>   g_modVerGen{ 0 };
int                g_modVerSel = -1;
std::atomic<int>   g_modState{ 0 };
std::atomic<int>   g_modGen{ 0 };
int                g_modRunGen = 0;
std::atomic<bool>  g_cfFallback{ false };
int                g_modSel = -1;
int                g_modFileSel = -1;
int                g_modTab = 0;
int                g_modSource = 0;
int                g_modCat = 0;
int                g_dlSection = 0;
int                g_instTab = 0;

std::vector<Cand>  g_dirCands, g_javaCands;

std::atomic<long long> g_dlBytes{ 0 };
std::wstring           g_dlTarget;
long long              g_dlLastBytes = 0;
DWORD                  g_dlLastTick = 0;
double                 g_dlSpeed = 0;

HANDLE             g_gameProc = nullptr;
std::atomic<DWORD> g_gamePid{ 0 };

int g_listScroll = 0;
std::atomic<int> g_loginState{ 0 };




// ---------------- U8ToW ----------------
std::wstring U8ToW(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// ---------------- WToU8 ----------------
std::string WToU8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// ---------------- EndsWithNoCase ----------------
static bool EndsWithNoCase(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return _stricmp(s.c_str() + s.size() - suffix.size(), suffix.c_str()) == 0;
}

// ---------------- EffectiveJavaExe ----------------
static std::string EffectiveJavaExe() {
    // 统一使用 java.exe（不是 javaw.exe）
    // 配合 CreateProcess 的 CREATE_NO_WINDOW，控制台不会出现黑窗口
    std::string p = g_cfg.javaPath;
    if (EndsWithNoCase(p, "javaw.exe")) {
        std::string cand = p.substr(0, p.size() - 9) + "java.exe";
        if (FileExists(fs::u8path(cand))) return cand;
    }
    if (EndsWithNoCase(p, "java.exe")) return p;
    return p;
}

// ---------------- TrimSlash ----------------
std::string TrimSlash(std::string s) {
    while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}

// ---------------- FileExists ----------------
bool FileExists(const fs::path& p) {
    std::error_code ec; return fs::exists(p, ec) && !fs::is_directory(p, ec);
}

// ---------------- DirExists ----------------
bool DirExists(const fs::path& p) {
    std::error_code ec; return fs::is_directory(p, ec);
}

// ---------------- ExeDir ----------------
static fs::path ExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

// ---------------- ExtractEmbeddedAuthlib ----------------
std::string ExtractEmbeddedAuthlib() {
    HRSRC hr = FindResourceA(nullptr, MAKEINTRESOURCEA(IDR_AUTHLIB), MAKEINTRESOURCEA(10));
    if (!hr) return "";
    DWORD sz = SizeofResource(nullptr, hr);
    HGLOBAL hg = LoadResource(nullptr, hr);
    if (!hg || sz == 0) return "";
    const void* src = LockResource(hg);
    if (!src) return "";

    std::vector<fs::path> targets;
    if (!g_dataDir.empty()) targets.push_back(fs::u8path(g_dataDir) / L"authlib-injector.jar");
    targets.push_back(ExeDir() / L"authlib-injector.jar");

    for (auto& dst : targets) {
        std::error_code ec;
        if (FileExists(dst) && fs::file_size(dst, ec) == sz) return WToU8(dst.wstring());

        fs::create_directories(dst.parent_path(), ec);
        {
            std::ofstream f{ dst, std::ios::binary | std::ios::trunc };
            if (!f) continue;
            f.write((const char*)src, (std::streamsize)sz);
            f.close();
        }
        if (FileExists(dst) && fs::file_size(dst, ec) == sz) return WToU8(dst.wstring());
    }
    return "";
}

// ---------------- Log ----------------
void Log(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    g_logs.push_back(s);
    if (!g_dataDir.empty()) {
        std::ofstream lf{ fs::u8path(g_dataDir) / L"launcher.log", std::ios::app };
        if (lf) {
            SYSTEMTIME st; GetLocalTime(&st);
            char ts[24] = {0};
            sprintf_s(ts, sizeof(ts), "%02d:%02d:%02d ", st.wHour, st.wMinute, st.wSecond);
            lf << ts << s << "\r\n";
        }
    }
}

// ---------------- LogTail ----------------
std::string LogTail(int n = 400) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    int start = (int)g_logs.size() > n ? (int)g_logs.size() - n : 0;
    std::string out;
    for (int i = start; i < (int)g_logs.size(); ++i) { out += g_logs[i]; out += "\r\n"; }
    return out;
}

// ---------------- SaveConfig ----------------
void SaveConfig() {
    json j;
    j["gameDir"]       = g_cfg.gameDir;
    j["isoMode"] = g_cfg.isoMode;
    j["dlThreads"] = g_cfg.dlThreads;
    j["extraJvm"] = g_cfg.extraJvm;
    j["extraGame"] = g_cfg.extraGame;
    j["dlSource"] = g_cfg.dlSource;
    j["cfProxy"]  = g_cfg.cfProxy;
    j["cfApiKey"] = g_cfg.cfApiKey;
    j["msClientId"] = g_cfg.msClientId;
    j["winMode"] = g_cfg.winMode;
    j["skinPath"] = g_cfg.skinPath;
    {
        json arr = json::array();
        for (auto& a : g_cfg.accounts) {
            json o;
            o["type"]   = a.type;
            o["name"]   = a.name;
            o["server"] = a.server;
            o["token"]  = a.token;
            o["uuid"]   = a.uuid;
            o["skin"]   = a.skin;
            o["refresh"] = a.refresh;
            arr.push_back(o);
        }
        j["accounts"] = arr;
        j["activeAcc"] = g_cfg.activeAcc;
    }
    j["javaPath"]      = g_cfg.javaPath;
    j["maxMemMB"]      = g_cfg.maxMemMB;
    j["playerName"]    = g_cfg.playerName;
    j["authType"]      = g_cfg.authType;
    j["authServer"]    = g_cfg.authServer;
    j["authToken"]     = g_cfg.authToken;
    j["authClientToken"]= g_cfg.authClientToken;
    j["authName"]      = g_cfg.authName;
    j["authUuid"]      = g_cfg.authUuid;
    j["authlibPath"]   = g_cfg.authlibPath;
    std::ofstream f{ fs::u8path(g_cfgPath) };
    if (f) f << j.dump(2);
}

// ---------------- LoadConfig ----------------
void LoadConfig() {
    std::ifstream f{ fs::u8path(g_cfgPath) };
    if (!f) return;
    try {
        json j; f >> j;
        g_cfg.gameDir        = j.value("gameDir", std::string());
    g_cfg.isoMode = j.value("isoMode", 0);
    g_cfg.dlThreads = j.value("dlThreads", 16);
    g_cfg.extraJvm = j.value("extraJvm", std::string());
    g_cfg.extraGame = j.value("extraGame", std::string());
    g_cfg.dlSource = j.value("dlSource", 0);
    g_cfg.cfProxy  = j.value("cfProxy",  std::string("https://api.curse.tools/v1/cf"));
    g_cfg.cfApiKey = j.value("cfApiKey", std::string());
    g_cfg.msClientId = j.value("msClientId", std::string("00000000402b5328"));
    g_cfg.winMode = j.value("winMode", 0);
    if (!j.contains("winMode") && j.contains("fullscreen"))   // 兼容旧的布尔配置
        g_cfg.winMode = j.value("fullscreen", false) ? 2 : 0;
    if (g_cfg.winMode < 0 || g_cfg.winMode > 2) g_cfg.winMode = 0;
    g_cfg.skinPath = j.value("skinPath", std::string());

    g_cfg.activeAcc = j.value("activeAcc", -1);
    if (j.contains("accounts") && j["accounts"].is_array()) {
        for (auto& o : j["accounts"]) {
            Account a;
            a.type   = o.value("type", std::string("offline"));
            a.name   = o.value("name", std::string());
            a.server = o.value("server", std::string());
            a.token  = o.value("token", std::string());
            a.uuid   = o.value("uuid", std::string());
            a.skin   = o.value("skin", std::string());
            a.refresh = o.value("refresh", std::string());
            if (!a.name.empty()) g_cfg.accounts.push_back(a);
        }
    }

    if (j.contains("versionIsolation"))            // 兼容上一版布尔配置
        g_cfg.isoMode = j.value("versionIsolation", true) ? 0 : 2;
        g_cfg.javaPath       = j.value("javaPath", std::string());
        g_cfg.maxMemMB       = j.value("maxMemMB", 4096);
        g_cfg.playerName     = j.value("playerName", std::string("Steve"));
    // 账户列表为空则用当前单账号迁移（此时 playerName 已读入）
    if (g_cfg.accounts.empty()) {
        std::string nm = (g_cfg.authType == "thirdparty") ? g_cfg.authName : g_cfg.playerName;
        if (!nm.empty()) {
            Account a;
            a.type = g_cfg.authType.empty() ? "offline" : g_cfg.authType;
            a.name = nm;
            a.server = g_cfg.authServer;
            a.token = g_cfg.authToken;
            a.uuid = g_cfg.authUuid;
            a.skin = g_cfg.skinPath;
            g_cfg.accounts.push_back(a);
            g_cfg.activeAcc = 0;
        }
    }
        g_cfg.authType       = j.value("authType", std::string("offline"));
        g_cfg.authServer     = j.value("authServer", std::string());
        g_cfg.authToken      = j.value("authToken", std::string());
        g_cfg.authClientToken= j.value("authClientToken", std::string());
        g_cfg.authName       = j.value("authName", std::string());
        g_cfg.authUuid       = j.value("authUuid", std::string());
        g_cfg.authlibPath    = j.value("authlibPath", std::string());
    } catch (...) {}
}

// ---------------- DetectJava ----------------
std::string DetectJava() {
    std::vector<fs::path> cands;
    wchar_t buf[32767];
    if (GetEnvironmentVariableW(L"JAVA_HOME", buf, 32767))
        cands.push_back(fs::path(buf) / L"bin" / L"javaw.exe");

    wchar_t appdata[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
        fs::path hmcl = fs::path(appdata) / L".hmcl" / L"java" / L"windows-x86_64";
        std::error_code ec;
        if (DirExists(hmcl)) {
            for (auto& e : fs::directory_iterator(hmcl, ec)) {
                if (ec) break;
                cands.push_back(e.path() / L"bin" / L"javaw.exe");
            }
        }
    }
    cands.push_back(L"C:/Program Files/Java/jdk-17/bin/javaw.exe");
    cands.push_back(L"C:/Program Files/Java/jdk-21/bin/javaw.exe");

    for (auto& c : cands) if (FileExists(c)) return WToU8(c.wstring());
    return "";
}

// ---------------- DetectGameDir ----------------
std::string DetectGameDir() {
    std::vector<fs::path> cands;
    wchar_t appdata[MAX_PATH], profile[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata)))
        cands.push_back(fs::path(appdata) / L".minecraft");
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, profile)))
        cands.push_back(fs::path(profile) / L".minecraft");
    cands.push_back(L"D:/MC/.minecraft");
    cands.push_back(L"C:/MC/.minecraft");
    cands.push_back(L"D:/.minecraft");

    for (auto& c : cands) {
        std::error_code ec;
        if (fs::is_directory(c / L"versions", ec)) return WToU8(c.wstring());
    }
    return "";
}

// ---------------- DetectAuthlib ----------------
std::string DetectAuthlib() {
    std::vector<fs::path> cands;
    cands.push_back(ExeDir() / L"authlib-injector.jar");
    if (!g_cfg.gameDir.empty()) {
        fs::path g = GameDirPath();
        cands.push_back(g / L"authlib-injector.jar");
        cands.push_back(g.parent_path() / L"authlib-injector.jar");
    }
    // 任意 authlib-injector-*.jar
    for (auto& c : cands) if (FileExists(c)) return WToU8(c.wstring());

    std::error_code ec;
    for (auto& d : { ExeDir(), g_cfg.gameDir.empty() ? fs::path() : GameDirPath() }) {
        if (d.empty()) continue;
        for (auto& e : fs::directory_iterator(d, ec)) {
            if (ec) break;
            std::string n = WToU8(e.path().filename().wstring());
            if (n.rfind("authlib-injector", 0) == 0 && e.path().extension() == L".jar")
                return WToU8(e.path().wstring());
        }
    }
    return "";
}

// ---------------- ScanVersions ----------------
void ScanVersions() {
    g_versions.clear();
    if (g_cfg.gameDir.empty()) return;

    fs::path verDir = GameDirPath() / L"versions";
    std::error_code ec;
    if (!DirExists(verDir)) return;

    for (auto& e : fs::directory_iterator(verDir, ec)) {
        if (ec) break;
        if (!e.is_directory()) continue;
        std::string id = WToU8(e.path().filename().wstring());
        if (FileExists(e.path() / fs::u8path(id + ".json")))
            g_versions.push_back(id);
    }
    std::sort(g_versions.begin(), g_versions.end(), std::greater<std::string>());
    g_selVersion = g_versions.empty() ? -1 : 0;
    g_listScroll = 0;
}

// ---------------- ReadJsonFile ----------------
json ReadJsonFile(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return json();
    try { json j; f >> j; return j; } catch (...) { return json(); }
}

// ---------------- ResolveVersion ----------------
json ResolveVersion(const std::string& id, int depth) {
    if (depth > 6) return json();
    fs::path base = GameDirPath() / L"versions" / fs::u8path(id);
    json child = ReadJsonFile(base / fs::u8path(id + ".json"));
    if (!child.is_object()) return json();

    std::string parentId = child.value("inheritsFrom", std::string());
    if (parentId.empty()) return child;

    json parent = ResolveVersion(parentId, depth + 1);
    if (!parent.is_object()) return child;

    json merged = parent;
    for (auto it = child.begin(); it != child.end(); ++it) {
        const std::string& k = it.key();
        if (k == "libraries" && merged.contains("libraries") && merged["libraries"].is_array()) {
            for (auto& lib : it.value()) merged["libraries"].push_back(lib);
        } else if (k == "arguments" && merged.contains("arguments")) {
            for (auto& sub : it.value().items()) {
                const std::string& sk = sub.key();
                if (merged["arguments"].contains(sk) && merged["arguments"][sk].is_array())
                    for (auto& v : sub.value()) merged["arguments"][sk].push_back(v);
                else
                    merged["arguments"][sk] = sub.value();
            }
        } else {
            merged[k] = it.value();
        }
    }
    return merged;
}

// ---------------- RulesAllow ----------------
bool RulesAllow(const json& rules) {
    if (!rules.is_array() || rules.empty()) return true;

    bool hasAllow = false;
    for (auto& r : rules)
        if (r.is_object() && r.value("action", std::string()) == "allow") { hasAllow = true; break; }

    bool matched = false;
    for (auto& r : rules) {
        if (!r.is_object()) continue;
        std::string action = r.value("action", "");
        bool ok = true;
        if (r.contains("os") && r["os"].is_object()) {
            const json& os = r["os"];
            if (os.contains("name") && os["name"].get<std::string>() != "windows") ok = false;
            if (ok && os.contains("arch")) {
                std::string a = os["arch"].get<std::string>();
                if (a != "x86_64" && a != "amd64") ok = false;
            }
        }
        if (ok && r.contains("features") && r["features"].is_object()) {
            for (auto& f : r["features"].items())
                if (f.value().is_boolean() && f.value().get<bool>()) ok = false;
        }
        if (!ok) continue;
        if (action == "allow") matched = true;
        else if (action == "deny") return false;
    }
    return hasAllow ? matched : true;
}

// ---------------- MavenPath ----------------
static fs::path MavenPath(const fs::path& libRoot, const std::string& name, const std::string& classifier = "") {
    std::vector<std::string> parts;
    std::stringstream ss(name);
    std::string seg;
    while (std::getline(ss, seg, ':')) parts.push_back(seg);
    if (parts.size() < 3) return fs::path();

    std::string group = parts[0], artifact = parts[1], version = parts[2];
    std::string cls = classifier;
    if (parts.size() >= 4 && cls.empty()) cls = parts[3];

    std::replace(group.begin(), group.end(), '.', '/');
    std::string file = artifact + "-" + version + (cls.empty() ? "" : "-" + cls) + ".jar";
    return libRoot / fs::u8path(group) / fs::u8path(artifact) / fs::u8path(version) / fs::u8path(file);
}

// ---------------- ExtractZip ----------------
bool ExtractZip(const fs::path& zip, const fs::path& outDir) {
    std::error_code ec;
    fs::create_directories(outDir, ec);
    if (ec) { Log("[警告] 无法创建解压目录: " + ec.message()); return false; }

    // 用 miniz 直接解压（不依赖系统 tar.exe）
    mz_zip_archive za{};
    if (!mz_zip_reader_init_file(&za, zip.u8string().c_str(), 0)) {
        Log("[解压] 无法打开 zip: " + zip.u8string());
        return false;
    }

    const mz_uint total = mz_zip_reader_get_num_files(&za);
    int okCount = 0, failCount = 0;
    for (mz_uint i = 0; i < total; ++i) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&za, i, &st)) { failCount++; continue; }
        if (st.m_is_directory) continue;

        fs::path dest = outDir / fs::u8path(st.m_filename);
        std::error_code e2;
        fs::create_directories(dest.parent_path(), e2);

        std::ofstream of(dest, std::ios::binary);
        if (!of) { failCount++; continue; }

        size_t outSize = 0;
        void* buf = mz_zip_reader_extract_to_heap(&za, i, &outSize, 0);
        if (!buf) { failCount++; continue; }
        of.write((const char*)buf, (std::streamsize)outSize);
        mz_free(buf);
        okCount++;
    }
    mz_zip_reader_end(&za);

    Log("[解压] " + std::to_string(okCount) + " 个文件成功" +
        (failCount ? "，" + std::to_string(failCount) + " 个失败" : ""));
    return failCount == 0;
}

// ---------------- Sha1Hex ----------------
std::string Sha1Hex(const std::vector<unsigned char>& data) {
    HCRYPTPROV prov = 0; HCRYPTHASH h = 0;
    std::string hex;
    if (!CryptAcquireContextA(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) return "";
    if (CryptCreateHash(prov, CALG_SHA1, 0, 0, &h)) {
        if (CryptHashData(h, data.data(), (DWORD)data.size(), 0)) {
            BYTE d[20] = {0}; DWORD len = 20;
            if (CryptGetHashParam(h, HP_HASHVAL, d, &len, 0)) {
                const char* hx = "0123456789abcdef";
                hex.resize(40);
                for (int i = 0; i < 20; ++i) { hex[i*2] = hx[d[i] >> 4]; hex[i*2+1] = hx[d[i] & 15]; }
            }
        }
        CryptDestroyHash(h);
    }
    CryptReleaseContext(prov, 0);
    return hex;
}

// ---------------- ToMirror ----------------
std::string ToMirror(std::string url) {
    const std::pair<const char*, const char*> map[] = {
        { "https://libraries.minecraft.net/",          "https://bmclapi2.bangbang93.com/maven/" },
        { "https://piston-data.mojang.com/",           "https://bmclapi2.bangbang93.com/" },
        { "https://piston-meta.mojang.com/",           "https://bmclapi2.bangbang93.com/" },
        { "https://launchermeta.mojang.com/",          "https://bmclapi2.bangbang93.com/" },
        { "https://resources.download.minecraft.net/", "https://bmclapi2.bangbang93.com/assets/" },
    };
    for (auto& m : map) {
        size_t p = url.find(m.first);
        if (p == 0) return std::string(m.second) + url.substr(strlen(m.first));
    }
    return url;
}

// ---------------- HttpRequest ----------------
bool HttpRequest(const std::string& urlU8, const std::string& method,
                        const std::string& body, const std::string& contentType,
                        std::vector<unsigned char>& out, std::string& err) {
    out.clear();
    std::wstring url = U8ToW(urlU8);

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[512] = {0}, path[4096] = {0};
    uc.lpszHostName = host; uc.dwHostNameLength = 511;
    uc.lpszUrlPath  = path; uc.dwUrlPathLength  = 4095;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { err = "URL 解析失败"; return false; }

    bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    static thread_local HINTERNET tlsSes = nullptr;
    if (!tlsSes) {
        tlsSes = WinHttpOpen(L"ReignLaunch/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (tlsSes) WinHttpSetTimeouts(tlsSes, 15000, 15000, 30000, 60000);
    }
    HINTERNET ses = tlsSes;
    WinHttpSetTimeouts(ses, 15000, 15000, 30000, 60000);

    HINTERNET con = WinHttpConnect(ses, host, uc.nPort ? uc.nPort : (https ? 443 : 80), 0);
    if (!con) { err = "连接失败"; return false; }

    std::wstring wmethod = U8ToW(method);
    HINTERNET req = WinHttpOpenRequest(con, wmethod.c_str(), path, nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
    if (!req) { WinHttpCloseHandle(con); err = "请求创建失败"; return false; }   // 不要关 ses！tlsSes 会变野指针

    std::wstring hdr = L"Content-Type: " + U8ToW(contentType) + L"\r\n";
    DWORD bodyLen = (DWORD)body.size();
    BOOL sent = WinHttpSendRequest(req, hdr.c_str(), (DWORD)-1,
                                   bodyLen ? (LPVOID)body.data() : WINHTTP_NO_REQUEST_DATA,
                                   bodyLen, bodyLen, 0);

    bool ok = false;
    if (sent && WinHttpReceiveResponse(req, nullptr)) {
        DWORD code = 0, sz = sizeof(code);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
        DWORD avail = 0;
        do {
            avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail)) break;
            if (avail == 0) break;
            std::vector<unsigned char> chunk(avail);
            DWORD got = 0;
            if (!WinHttpReadData(req, chunk.data(), avail, &got) || got == 0) break;
            out.insert(out.end(), chunk.begin(), chunk.begin() + got);
        } while (avail > 0);

        ok = (code >= 200 && code < 300);
        if (!ok) err = "HTTP " + std::to_string(code);
    } else {
        err = "请求失败";
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(con);
    return ok;
}

// ---------------- DownloadFile ----------------
bool DownloadFile(const std::string& url, const fs::path& dest, std::string& err) {
    std::vector<unsigned char> data;
    std::string mirror = ToMirror(url);
    // dlSource: 0=自动(先官方，失败切镜像) 1=仅官方 2=仅镜像
    if (g_cfg.dlSource == 2 && mirror != url) {
        std::string e2;
        if (!HttpRequest(mirror, "GET", "", "", data, e2)) { err = "镜像: " + e2; return false; }
    }
    else if (!HttpRequest(url, "GET", "", "", data, err)) {
        if (g_cfg.dlSource == 1) return false;   // 仅官方：不切镜像
        if (mirror != url) {
            std::string e2;
            if (HttpRequest(mirror, "GET", "", "", data, e2)) err.clear();
            else { err += " / 镜像: " + e2; return false; }
        } else return false;
    }

    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    if (ec) { err = "mkdir failed: " + ec.message() + " | dest=" + WToU8(dest.wstring()); return false; }

    fs::path part = dest;
    part += L".part";
    {
        std::ofstream f{ part, std::ios::binary | std::ios::trunc };
        if (!f) { err = "写入失败"; return false; }
        f.write((const char*)data.data(), (std::streamsize)data.size());
    }
    g_dlBytes += (long long)data.size();
    fs::rename(part, dest, ec);
    if (ec) { fs::remove(part, ec); err = "重命名失败"; return false; }
    return true;
}

// ---------------- YggdrasilLogin ----------------
AuthResult YggdrasilLogin(const std::string& serverRaw, const std::string& user, const std::string& pass) {
    AuthResult r;
    std::string server = TrimSlash(serverRaw);
    if (server.empty()) { r.err = "认证服务器地址为空"; return r; }

    json reqBody;
    reqBody["agent"] = { { "name", "Minecraft" }, { "version", 1 } };
    reqBody["username"] = user;
    reqBody["password"] = pass;
    reqBody["requestUser"] = false;

    std::vector<unsigned char> resp;
    std::string err;
    bool ok = HttpRequest(server + "/authserver/authenticate", "POST",
                          reqBody.dump(), "application/json", resp, err);
    if (!ok) { r.err = "登录请求失败：" + err; return r; }

    std::string text(resp.begin(), resp.end());
    json j;
    try { j = json::parse(text); } catch (...) { r.err = "服务器返回的不是合法 JSON"; return r; }

    if (j.contains("error")) {
        std::string msg = j.value("errorMessage", std::string("未知错误"));
        r.err = msg;
        return r;
    }
    r.accessToken  = j.value("accessToken", std::string());
    r.clientToken  = j.value("clientToken", std::string());
    if (j.contains("selectedProfile") && j["selectedProfile"].is_object()) {
        r.uuid = j["selectedProfile"].value("id", std::string());
        r.name = j["selectedProfile"].value("name", std::string());
    }
    if (r.accessToken.empty() || r.uuid.empty()) { r.err = "服务器没有返回有效的 accessToken/角色"; return r; }
    r.ok = true;
    return r;
}

// ---------------- ReplaceAll ----------------
static std::string ReplaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) { s.replace(pos, from.size(), to); pos += to.size(); }
    return s;
}

// ---------------- SplitArgs ----------------
static std::vector<std::string> SplitArgs(const std::string& s) {
    std::vector<std::string> out; std::string cur; bool inQuote = false;
    for (char c : s) {
        if (c == '"') { inQuote = !inQuote; continue; }
        if (!inQuote && (c == ' ' || c == '\t')) { if (!cur.empty()) { out.push_back(cur); cur.clear(); } }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// ---------------- QuoteIfNeeded ----------------
static std::string QuoteIfNeeded(const std::string& s) {
    return (s.find(' ') == std::string::npos) ? s : "\"" + s + "\"";
}

// ---------------- OfflineUUID ----------------
std::string OfflineUUID(const std::string& name) {
    HCRYPTPROV prov = 0; HCRYPTHASH hash = 0;
    std::string hex, src = "OfflinePlayer:" + name;
    if (CryptAcquireContextA(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(prov, CALG_MD5, 0, 0, &hash)) {
            if (CryptHashData(hash, (const BYTE*)src.data(), (DWORD)src.size(), 0)) {
                BYTE d[16] = {0}; DWORD len = 16;
                if (CryptGetHashParam(hash, HP_HASHVAL, d, &len, 0)) {
                    const char* hx = "0123456789abcdef";
                    hex.resize(32);
                    for (int i = 0; i < 16; ++i) { hex[i*2] = hx[d[i] >> 4]; hex[i*2+1] = hx[d[i] & 15]; }
                }
            }
            CryptDestroyHash(hash);
        }
        CryptReleaseContext(prov, 0);
    }
    if (hex.size() != 32) return "00000000-0000-3000-8000-000000000000";
    hex[12] = '3';
    int hv = (hex[16] <= '9') ? (hex[16] - '0') : (hex[16] - 'a' + 10);
    hex[16] = "89ab"[hv & 3];
    return hex.substr(0,8) + "-" + hex.substr(8,4) + "-" + hex.substr(12,4) + "-" + hex.substr(16,4) + "-" + hex.substr(20,12);
}

// ---------------- IsModdedInstance ----------------
bool IsModdedInstance(const std::string& id, const fs::path& mcDir) {
    fs::path vd = mcDir / L"versions" / fs::u8path(id);
    std::error_code ec;
    fs::path mods = vd / L"mods";
    if (DirExists(mods)) {
        for (auto& e : fs::directory_iterator(mods, ec)) {
            std::wstring n = e.path().filename().wstring();
            if (n.size() > 4 && n.substr(n.size() - 4) == L".jar") return true;
        }
    }
    std::string low = id;
    for (auto& c : low) c = (char)tolower((unsigned char)c);
    if (low.find("forge") != std::string::npos || low.find("fabric") != std::string::npos ||
        low.find("quilt") != std::string::npos || low.find("neoforge") != std::string::npos)
        return true;

    std::string vjson = WToU8((vd / L"json").wstring());
    std::ifstream vf{ fs::u8path(vjson) };
    if (vf) {
        std::stringstream ss; ss << vf.rdbuf();
        std::string body = ss.str();
        for (auto& c : body) c = (char)tolower((unsigned char)c);
        if (body.find("net.minecraftforge") != std::string::npos ||
            body.find("net.fabricmc") != std::string::npos ||
            body.find("net.neoforged") != std::string::npos) return true;
    }
    return false;
}

// ---------------- ShouldIsolate ----------------
bool ShouldIsolate(const std::string& id, const fs::path& mcDir) {
    if (g_cfg.isoMode == 2) return false;
    if (!DirExists(mcDir / L"versions" / fs::u8path(id))) return false;
    if (g_cfg.isoMode == 1) return IsModdedInstance(id, mcDir);
    return true;
}

// ---------------- Sha1OfFile ----------------
std::string Sha1OfFile(const fs::path& p) {
    std::ifstream f{ p, std::ios::binary };
    if (!f) return "";
    std::vector<unsigned char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return Sha1Hex(buf);
}

// ---------------- RunDownloads ----------------
void RunDownloads(std::vector<DLTask>& tasks, DownloadStats& st, int threads) {
    if (tasks.empty()) return;
    if (threads <= 0) threads = g_cfg.dlThreads;
    if (threads < 1) threads = 1;
    if (threads > 128) threads = 128;
    if (threads > (int)tasks.size()) threads = (int)tasks.size();

    std::atomic<size_t> next{ 0 };
    std::atomic<int> okCount{ 0 }, failCount{ 0 };
    std::mutex logMu;

    g_progTotal = (int)tasks.size();
    g_progCur = 0;

    std::vector<std::thread> pool;
    for (int i = 0; i < threads; ++i) {
        pool.emplace_back([&]() {
            for (;;) {
                if (g_dlCancel) break;
                size_t idx = next.fetch_add(1);
                if (idx >= tasks.size()) break;
                std::string err;
                bool okDl = DownloadFile(tasks[idx].url, tasks[idx].dest, err);
                if (okDl && !tasks[idx].sha1.empty()) {
                    std::string got = Sha1OfFile(tasks[idx].dest);
                    if (got != tasks[idx].sha1) {
                        okDl = false;
                        err = "SHA1 不匹配（期望 " + tasks[idx].sha1.substr(0, 8) +
                              "…，实际 " + got.substr(0, 8) + "…）";
                        std::error_code re; fs::remove(tasks[idx].dest, re);
                    }
                }
                if (okDl) {
                    okCount.fetch_add(1);
                } else {
                    failCount.fetch_add(1);
                    std::lock_guard<std::mutex> lk(logMu);
                    Log("[失败] " + tasks[idx].label + " : " + err);
                }
                g_progCur = okCount.load() + failCount.load();
            }
        });
    }
    for (auto& th : pool) th.join();

    st.downloaded += okCount.load();
    st.failed += failCount.load();
}

// ---------------- DownloadMissing ----------------
void DownloadMissing(const std::string& versionId, DownloadStats& st) {
    fs::path mcDir = GameDirPath();
    fs::path libRoot = mcDir / L"libraries";
    json vj = ResolveVersion(versionId);
    if (!vj.is_object()) { st.failed++; return; }

    std::vector<DLTask> batch1;

    if (vj.contains("libraries") && vj["libraries"].is_array()) {
        for (auto& lib : vj["libraries"]) {
            if (!lib.is_object()) continue;
            if (lib.contains("rules") && !RulesAllow(lib["rules"])) continue;
            if (!lib.contains("downloads") || !lib["downloads"].is_object()) continue;
            const json& dl = lib["downloads"];
            if (!dl.contains("artifact") || !dl["artifact"].is_object()) continue;

            const json& art = dl["artifact"];
            std::string rel  = art.value("path", std::string());
            std::string url  = art.value("url", std::string());
            std::string sha1 = art.value("sha1", std::string());
            if (rel.empty() || url.empty()) continue;

            fs::path dest = libRoot / fs::u8path(rel);
            bool need = true;
            if (FileExists(dest)) {
                need = false;
                if (!sha1.empty()) {
                    std::ifstream f{ dest, std::ios::binary };
                    std::vector<unsigned char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                    if (Sha1Hex(buf) != sha1) need = true;
                }
                if (!need) st.existed++;
            }
            if (need) batch1.push_back({ url, dest, rel, sha1 });
        }
    }

    fs::path clientJar = mcDir / L"versions" / fs::u8path(versionId) / fs::u8path(versionId + ".jar");
    if (vj.contains("downloads") && vj["downloads"].is_object() && vj["downloads"].contains("client")) {
        std::string url  = vj["downloads"]["client"].value("url", std::string());
        std::string csha = vj["downloads"]["client"].value("sha1", std::string());
        if (!url.empty()) {
            bool needJar = true;
            if (FileExists(clientJar)) {
                needJar = false;
                if (!csha.empty() && Sha1OfFile(clientJar) != csha) needJar = true;   // 客户端 jar 也要校验
                if (!needJar) st.existed++;
            }
            if (needJar) batch1.push_back({ url, clientJar, versionId + ".jar", csha });
        }
    }

    RunDownloads(batch1, st);

    std::string indexId, indexUrl;
    if (vj.contains("assetIndex") && vj["assetIndex"].is_object()) {
        indexId  = vj["assetIndex"].value("id", std::string());
        indexUrl = vj["assetIndex"].value("url", std::string());
    }
    if (!indexId.empty() && !indexUrl.empty()) {
        fs::path idxPath = mcDir / L"assets" / L"indexes" / fs::u8path(indexId + ".json");
        std::string idxSha = vj["assetIndex"].value("sha1", std::string());
        bool needIdx = !FileExists(idxPath);
        if (!needIdx && !idxSha.empty() && Sha1OfFile(idxPath) != idxSha) needIdx = true;
        if (needIdx) {
            std::string err;
            if (DownloadFile(indexUrl, idxPath, err) &&
                (idxSha.empty() || Sha1OfFile(idxPath) == idxSha)) st.downloaded++;
            else { st.failed++; std::error_code e3; fs::remove(idxPath, e3); }
        } else st.existed++;

        json idx = ReadJsonFile(idxPath);
        if (idx.is_object() && idx.contains("objects") && idx["objects"].is_object()) {
            std::vector<DLTask> batch2;
            for (auto& it : idx["objects"].items()) {
                std::string hash = it.value().value("hash", std::string());
                if (hash.size() < 3) continue;
                fs::path obj = mcDir / L"assets" / L"objects" / fs::u8path(hash.substr(0, 2)) / fs::u8path(hash);
                bool needObj = !FileExists(obj);
                if (!needObj && Sha1OfFile(obj) != hash) needObj = true;   // 资源文件按内容哈希校验
                if (!needObj) { st.existed++; continue; }
                batch2.push_back({ "https://resources.download.minecraft.net/" + hash.substr(0, 2) + "/" + hash, obj, hash, hash });
            }
            RunDownloads(batch2, st);
        }
    }
}

// ---------------- CountMissing ----------------
int CountMissing(const std::string& versionId) {
    fs::path mcDir = GameDirPath();

    // 版本隔离：把 gameDir 指向 versions/<id>，让 Forge 用该版本自己的 mods
    std::string gameDirEff = g_cfg.gameDir;
    if (ShouldIsolate(versionId, mcDir)) {
        gameDirEff = WToU8((mcDir / L"versions" / fs::u8path(versionId)).wstring());
    }
    fs::path libRoot = mcDir / L"libraries";
    json vj = ResolveVersion(versionId);
    if (!vj.is_object()) return -1;

    int missing = 0;

    if (vj.contains("libraries") && vj["libraries"].is_array()) {
        for (auto& lib : vj["libraries"]) {
            if (!lib.is_object()) continue;
            if (lib.contains("rules") && !RulesAllow(lib["rules"])) continue;
            if (!lib.contains("downloads") || !lib["downloads"].is_object()) continue;
            const json& dl = lib["downloads"];
            if (!dl.contains("artifact") || !dl["artifact"].is_object()) continue;
            std::string rel = dl["artifact"].value("path", std::string());
            if (rel.empty()) continue;
            if (!FileExists(libRoot / fs::u8path(rel))) missing++;
        }
    }

    fs::path clientJar = mcDir / L"versions" / fs::u8path(versionId) / fs::u8path(versionId + ".jar");
    if (vj.contains("downloads") && vj["downloads"].is_object() && vj["downloads"].contains("client"))
        if (!FileExists(clientJar)) missing++;

    std::string indexId;
    if (vj.contains("assetIndex") && vj["assetIndex"].is_object())
        indexId = vj["assetIndex"].value("id", std::string());

    if (!indexId.empty()) {
        fs::path idxPath = mcDir / L"assets" / L"indexes" / fs::u8path(indexId + ".json");
        if (!FileExists(idxPath)) {
            missing++;
        } else {
            json idx = ReadJsonFile(idxPath);
            if (idx.is_object() && idx.contains("objects") && idx["objects"].is_object()) {
                for (auto& it : idx["objects"].items()) {
                    std::string hash = it.value().value("hash", std::string());
                    if (hash.size() < 3) continue;
                    fs::path obj = mcDir / L"assets" / L"objects" / fs::u8path(hash.substr(0, 2)) / fs::u8path(hash);
                    if (!FileExists(obj)) missing++;
                }
            }
        }
    }
    return missing;
}

// ---------------- JavaRoot ----------------
static fs::path JavaRoot() {
    return fs::u8path(g_dataDir) / L"java";
}

// ---------------- FindJavaIn ----------------
fs::path FindJavaIn(const fs::path& root) {
    std::error_code ec;
    if (!fs::exists(root, ec)) return {};
    for (auto it = fs::recursive_directory_iterator(root, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file()) continue;
        std::wstring n = it->path().filename().wstring();
        if (_wcsicmp(n.c_str(), L"javaw.exe") == 0 &&
            it->path().parent_path().filename() == L"bin")
            return it->path();
    }
    return {};
}

// ---------------- PickTemurinZip ----------------
static std::string PickTemurinZip(const std::string& jsonText) {
    json j = json::parse(jsonText, nullptr, false);
    if (!j.is_array()) return "";
    for (auto& rel : j) {
        if (!rel.contains("binary")) continue;
        const json& b = rel["binary"];
        if (b.value("os", std::string()) != "windows") continue;
        if (b.value("architecture", std::string()) != "x64") continue;
        if (b.value("image_type", std::string()) != "jdk") continue;
        std::string link = b["package"].value("link", std::string());
        if (!link.empty()) return link;
    }
    return "";
}

// ---------------- InstallJava ----------------
bool InstallJava(int need) {
    fs::path dir = JavaRoot() / fs::u8path("temurin" + std::to_string(need));

    // 已经装过就直接用
    if (fs::path ex = FindJavaIn(dir); !ex.empty()) {
        g_cfg.javaPath = WToU8(ex.wstring());
        SaveConfig();
        return true;
    }

    if (g_hwnd) { g_status = L"缺少 Java " + std::to_wstring(need) + L"，正在自动下载…"; PostMessageW(g_hwnd, WM_APP_DONE, 0, 0); }
    Log("[Java] 开始下载 Temurin " + std::to_string(need));

    // 1) 查 Adoptium API
    std::string api = "https://api.adoptium.net/v3/assets/latest/" + std::to_string(need) +
                      "/hotspot?architecture=x64&image_type=jdk&os=windows&vendor=eclipse";
    std::vector<unsigned char> resp; std::string err;
    std::string zipUrl;
    if (HttpRequest(api, "GET", "", "", resp, err) && !resp.empty())
        zipUrl = PickTemurinZip(std::string(resp.begin(), resp.end()));
    if (zipUrl.empty()) {
        if (g_hwnd) { g_status = L"查询 Java 下载地址失败"; PostMessageW(g_hwnd, WM_APP_DONE, 0, 0); }
        Log("[Java] API 失败：" + err);
        return false;
    }

    // 2) 下载 zip
    std::error_code ec;
    fs::create_directories(JavaRoot(), ec);
    fs::path zip = JavaRoot() / fs::u8path("temurin" + std::to_string(need) + ".zip");
    g_dlBytes = 0;
    g_progTotal = 1; g_progCur = 0;
    if (!DownloadFile(zipUrl, zip, err)) {
        // 失败再用代理试一次
        std::string e2;
        if (!DownloadFile("https://gh-proxy.com/" + zipUrl, zip, e2)) {
            if (g_hwnd) { g_status = L"Java 下载失败：" + U8ToW(err); PostMessageW(g_hwnd, WM_APP_DONE, 0, 0); }
            Log("[Java] 下载失败：" + err + " / " + e2);
            return false;
        }
    }
    g_progCur = 1;

    // 3) 用 miniz 解压（不依赖系统 tar.exe）
    if (g_hwnd) { g_status = L"正在解压 Java…"; PostMessageW(g_hwnd, WM_APP_DONE, 0, 0); }
    if (!ExtractZip(zip, dir)) {
        if (g_hwnd) { g_status = L"解压 Java 失败"; PostMessageW(g_hwnd, WM_APP_DONE, 0, 0); }
        return false;
    }
    fs::remove(zip, ec);

    // 4) 找 javaw.exe
    fs::path ex = FindJavaIn(dir);
    if (ex.empty()) {
        if (g_hwnd) { g_status = L"解压后没找到 javaw.exe"; PostMessageW(g_hwnd, WM_APP_DONE, 0, 0); }
        return false;
    }
    g_cfg.javaPath = WToU8(ex.wstring());
    SaveConfig();
    Log("[Java] 安装完成：" + g_cfg.javaPath);
    if (g_hwnd) { g_status = L"Java " + std::to_wstring(need) + L" 已就绪"; PostMessageW(g_hwnd, WM_APP_DONE, 0, 0); }
    return true;
}

// ---------------- PumpOutput ----------------
static void PumpOutput(HANDLE hRead) {
    char buf[4096];
    DWORD got = 0;
    std::string pending;
    while (ReadFile(hRead, buf, sizeof(buf) - 1, &got, nullptr) && got > 0) {
        buf[got] = 0;
        pending += buf;
        size_t pos;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            Log("[游戏] " + line);
            pending.erase(0, pos + 1);
        }
    }
    if (!pending.empty()) Log("[游戏] " + pending);
    CloseHandle(hRead);
}

// ---------------- FindGameWindowProc ----------------
static BOOL CALLBACK FindGameWindowProc(HWND h, LPARAM lp) {
    WinWaitCtx* c = (WinWaitCtx*)lp;
    DWORD wp = 0;
    GetWindowThreadProcessId(h, &wp);
    if (wp != c->pid) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    wchar_t title[256] = {0};
    GetWindowTextW(h, title, 256);
    if (wcslen(title) == 0) return TRUE;
    c->found = true;
    return FALSE;
}

// ---------------- WaitForGameWindow ----------------
static bool WaitForGameWindow(DWORD pid, int timeoutMs) {
    DWORD start = GetTickCount();
    while (GetTickCount() - start < (DWORD)timeoutMs) {
        WinWaitCtx c{ pid, false };
        EnumWindows(FindGameWindowProc, (LPARAM)&c);
        if (c.found) return true;
        if (g_cancelLaunch) return false;
        Sleep(250);
    }
    return false;
}

// ---------------- LaunchThread ----------------
void LaunchThread(std::string versionId) {
    g_launching = true;
    Log("=========== 启动 " + versionId + " ===========");
    Log("账号: " + (g_cfg.authType == "thirdparty" ? (g_cfg.authName + " (第三方)") : (g_cfg.playerName + " (离线)")));

    g_stepCur = 0;

    // 按 MC 版本自动匹配 Java
    {
        int need = RequiredJavaMajor(versionId);
        if (need > 0) {
            std::string cur = g_cfg.javaPath.empty() ? "" : JavaVersionOf(fs::u8path(g_cfg.javaPath));
            int curVer = 0;
            try { if (!cur.empty()) curVer = std::stoi(cur); } catch (...) {}

            std::string pick = PickJavaFor(need);
            int pickVer = 0;
            if (!pick.empty()) {
                try { pickVer = std::stoi(JavaVersionOf(fs::u8path(pick))); } catch (...) {}
            }

            if (pickVer > 0 && pickVer != curVer) {
                // 优先用最贴近的版本：1.20.1 用 17 而不是 25
                g_cfg.javaPath = pick;
                SaveConfig();
                Log("[Java] 自动切换到 Java " + std::to_string(pickVer) +
                    "（该版本需要 Java " + std::to_string(need) + "）");
            } else if (curVer < need) {
                Log("[Java] 该版本需要 Java " + std::to_string(need) + "，开始自动下载");
                if (g_hwnd) { g_status = L"缺少 Java " + std::to_wstring(need) + L"，正在自动下载…"; PostMessageW(g_hwnd, WM_APP_DONE, 0, 0); }

                if (!InstallJava(need)) {
                    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_NEEDJAVA, (WPARAM)need, 0);
                    g_launching = false;
                    g_progState = 0;
                    return;
                }
                int nowVer = 0;
                try { nowVer = std::stoi(JavaVersionOf(fs::u8path(g_cfg.javaPath))); } catch (...) {}
                if (nowVer < need) {
                    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_NEEDJAVA, (WPARAM)need, 0);
                    g_status = L"启动失败：需要 Java " + std::to_wstring(need) + L"，自动安装未完成";
                    g_launching = false;
                    g_progState = 0;
                    return;
                }
                Log("[Java] 自动下载完成，继续启动");
            }
        }
    }

    // ---------- 补全缺失文件 ----------
    {
        int miss = CountMissing(versionId);
        if (miss > 0) {
            Log("[补全] 检测到 " + std::to_string(miss) + " 个文件缺失，开始自动下载…");
            if (g_hwnd) { g_status = L"正在补全文件…"; PostMessageW(g_hwnd, WM_APP_DONE, 0, 0); }
            g_progState = 1;
            DownloadStats st;
            DownloadMissing(versionId, st);
            Log("[补全] 完成：新下载 " + std::to_string(st.downloaded) +
                "，已存在 " + std::to_string(st.existed) +
                "，失败 " + std::to_string(st.failed));
            g_progState = 0;
            if (g_dlCancel) { g_launching = false; if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0); return; }
        }
    }

    // ---------- 生成启动方案 ----------
    g_stepCur = 1;
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);

    LaunchPlan plan;
    std::string planErr;
    if (!BuildLaunchPlan(versionId, plan, planErr)) {
        Log("[错误] 生成启动命令失败：" + planErr);
        g_status = L"启动失败：" + U8ToW(planErr);
        g_launching = false;
        g_progState = 0;
        g_stepCur = 0;
        if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
        return;
    }

    // ---------- 启动进程 ----------
    g_stepCur = 2;
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE hRead = nullptr, hWrite = nullptr;
    CreatePipe(&hRead, &hWrite, &sa, 0);
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite;
    si.hStdError  = hWrite;
    si.hStdInput  = nullptr;

    PROCESS_INFORMATION pi{};
    std::wstring cmdline = plan.commandLine;
    std::wstring workDir = plan.workDir;

    BOOL started = CreateProcessW(
        plan.javaPath.empty() ? nullptr : plan.javaPath.c_str(),
        cmdline.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
        nullptr,
        workDir.empty() ? nullptr : workDir.c_str(),
        &si, &pi);

    if (hWrite) CloseHandle(hWrite);

    if (!started) {
        if (hRead) CloseHandle(hRead);
        Log("[错误] 创建游戏进程失败，错误码 " + std::to_string(GetLastError()));
        g_status = L"启动失败：无法创建游戏进程（检查 Java 路径）";
        g_launching = false;
        g_progState = 0;
        g_stepCur = 0;
        if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
        return;
    }

    g_stepCur = 4;                 // 四步全部完成，等游戏窗口
    g_gamePid = pi.dwProcessId;
    if (g_gameProc) CloseHandle(g_gameProc);
    g_gameProc = pi.hProcess;
    CloseHandle(pi.hThread);
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);

    Log("[启动] 游戏进程已创建，PID " + std::to_string(pi.dwProcessId));

    // 读输出（后台）
    if (hRead) std::thread(PumpOutput, hRead).detach();

    // 等游戏窗口真正出现（期间轮询取消标志，取消就杀掉进程）
    bool shown = false;
    for (int i = 0; i < 1800; ++i) {          // 最多 180 秒，每 100ms 一轮
        if (g_cancelLaunch) {
            Log("[启动] 用户取消，正在结束游戏进程");
            TerminateProcess(pi.hProcess, 0);
            // 等它真的退出，避免窗口残留
            WaitForSingleObject(pi.hProcess, 5000);
            g_status = L"已取消启动，游戏进程已关闭";
            g_launching = false;
            g_progState = 0;
            g_stepCur = 0;
            if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
            return;
        }
        if (WaitForGameWindow(pi.dwProcessId, 100)) { shown = true; break; }

        // 进程自己退出了就不再等
        DWORD ec2 = 0;
        if (GetExitCodeProcess(pi.hProcess, &ec2) && ec2 != STILL_ACTIVE) {
            Log("[启动] 游戏进程已退出，退出码 " + std::to_string(ec2));
            break;
        }
    }
    Log(shown ? "[启动] 游戏窗口已出现" : "[启动] 等待游戏窗口结束");

    g_launching = false;
    g_progState = 0;
    g_stepCur = 0;
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
}

// ---------------- CurrentPlayerName ----------------
std::string CurrentPlayerName() {
    return (g_cfg.authType == "thirdparty" && !g_cfg.authName.empty()) ? g_cfg.authName : g_cfg.playerName;
}

// ---------------- ApplySkin ----------------
static bool ApplySkin(const std::string& srcPath, std::wstring& err) {
    std::wstring wp = U8ToW(srcPath);
    if (g_gpOk) {
        Gdiplus::Bitmap bmp(wp.c_str());
        if (bmp.GetLastStatus() != Gdiplus::Ok) { err = L"无法读取该图片"; return false; }
        int w = (int)bmp.GetWidth(), h = (int)bmp.GetHeight();
        if (!((w == 64 && h == 64) || (w == 64 && h == 32))) {
            err = L"皮肤必须是 64x64 或 64x32 的 PNG";
            return false;
        }
    }

    wchar_t appdata[MAX_PATH] = { 0 };
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata);
    fs::path skins = fs::path(appdata) / L"ReignLaunch" / L"skins";
    std::error_code ec;
    fs::create_directories(skins, ec);

    std::string player = CurrentPlayerName();
    if (player.empty()) player = "player";

    fs::path local = skins / fs::u8path(player + ".png");
    fs::copy_file(fs::u8path(srcPath), local, fs::copy_options::overwrite_existing, ec);
    if (ec) { err = L"保存皮肤失败"; return false; }

    // CustomSkinLoader 本地皮肤（装了 CSL 模组就能在游戏里看到）
    if (!g_cfg.gameDir.empty()) {
        fs::path csl = GameDirPath() / L"CustomSkinLoader" / L"LocalSkin" / L"textures";
        fs::create_directories(csl, ec);
        fs::copy_file(local, csl / fs::u8path(player + ".png"), fs::copy_options::overwrite_existing, ec);
    }

    g_cfg.skinPath = WToU8(local.wstring());
    SaveConfig();
    LoadSkinBitmap();
    return true;
}

// ---------------- ApplyActiveAccount ----------------
static void ApplyActiveAccount() {
    if (g_cfg.activeAcc < 0 || g_cfg.activeAcc >= (int)g_cfg.accounts.size()) return;
    Account& a = g_cfg.accounts[g_cfg.activeAcc];
    g_cfg.authType   = a.type;
    g_cfg.authServer = a.server;
    g_cfg.authToken  = a.token;
    g_cfg.authUuid   = a.uuid;
    if (a.type == "thirdparty") g_cfg.authName = a.name;
    else                        g_cfg.playerName = a.name;
    g_cfg.skinPath = a.skin;
}

// ---------------- CaptureActiveAccount ----------------
void CaptureActiveAccount() {
    if (g_cfg.activeAcc < 0 || g_cfg.activeAcc >= (int)g_cfg.accounts.size()) return;
    Account& a = g_cfg.accounts[g_cfg.activeAcc];
    a.type   = g_cfg.authType;
    a.server = g_cfg.authServer;
    a.token  = g_cfg.authToken;
    a.uuid   = g_cfg.authUuid;
    a.skin   = g_cfg.skinPath;
    if (a.type == "thirdparty") a.name = g_cfg.authName;
    else                        a.name = g_cfg.playerName;
}

// ---------------- UpsertAccount ----------------
int UpsertAccount(const std::string& type, const std::string& name,
                 const std::string& server, const std::string& token,
                 const std::string& uuid) {
    for (size_t i = 0; i < g_cfg.accounts.size(); ++i) {
        if (g_cfg.accounts[i].name == name && g_cfg.accounts[i].type == type) {
            g_cfg.accounts[i].server = server;
            g_cfg.accounts[i].token  = token;
            g_cfg.accounts[i].uuid   = uuid;
            return (int)i;
        }
    }
    Account a;
    a.type = type; a.name = name; a.server = server; a.token = token; a.uuid = uuid;
    g_cfg.accounts.push_back(a);
    return (int)g_cfg.accounts.size() - 1;
}

// ---------------- PostJson ----------------
static bool PostJson(const std::string& url, const std::string& body,
                     const std::string& ctype, json& out, std::string& err) {
    std::vector<unsigned char> d;
    if (!HttpRequest(url, "POST", body, ctype, d, err)) return false;
    if (d.empty()) { err = "空响应"; return false; }
    out = json::parse(std::string(d.begin(), d.end()), nullptr, false);
    if (out.is_discarded()) { err = "响应不是 JSON"; return false; }
    return true;
}

// ---------------- MicrosoftLoginThread ----------------
void MicrosoftLoginThread() {
    if (g_cfg.msClientId.empty()) {
        g_msState = 4;
        g_msMsg = L"未配置 Azure 客户端 ID：请在 config.json 里填 msClientId";
        if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
        return;
    }

    g_msState = 1;
    g_msCode.clear();
    g_msMsg = L"正在向微软申请登录代码…";
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);

    // 两套端点：
    //   v2  —— 自己注册的 Azure 应用（consumers 租户）
    //   live —— 旧版 MSA 应用（如 Mojang 官方的 00000000402b5328）
    const std::string v2base   = "https://login.microsoftonline.com/consumers/oauth2/v2.0";
    const std::string liveDev  = "https://login.live.com/oauth20_connect.srf";
    const std::string liveTok  = "https://login.live.com/oauth20_token.srf";
    const std::string scope    = "XboxLive.signin%20offline_access";

    // 1) 申请设备代码（先 v2，失败再 live）
    json dc; std::string err;
    bool liveMode = false;

    {
        std::string body = "client_id=" + g_cfg.msClientId + "&scope=" + scope;
        if (!PostJson(v2base + "/devicecode", body, "application/x-www-form-urlencoded", dc, err)
            || !dc.contains("device_code")) {
            // 走 live.com
            std::string lbody = "client_id=" + g_cfg.msClientId + "&scope=" + scope +
                                "&response_type=device_code";
            json dc2; std::string err2;
            if (PostJson(liveDev, lbody, "application/x-www-form-urlencoded", dc2, err2)
                && dc2.contains("device_code")) {
                dc = dc2;
                liveMode = true;
            } else {
                g_msState = 4;
                g_msMsg = L"申请登录代码失败：" + U8ToW(err2.empty() ? err : err2);
                if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
                return;
            }
        }
    }

    std::string deviceCode = dc.value("device_code", std::string());
    std::string userCode   = dc.value("user_code", std::string());
    std::string verifyUri  = dc.value("verification_uri", std::string("https://www.microsoft.com/link"));
    int interval = dc.value("interval", 5);
    int expires  = dc.value("expires_in", 900);
    if (deviceCode.empty() || userCode.empty()) {
        g_msState = 4;
        g_msMsg = L"微软返回的数据不完整";
        if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
        return;
    }

    g_msCode = U8ToW(userCode);
    g_msUrl  = U8ToW(verifyUri);
    g_msState = 2;
    g_msMsg = L"请在浏览器打开 " + g_msUrl + L" 并输入代码 " + g_msCode;
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);

    // 打开浏览器（失败也无所谓，用户自己复制）
    ShellExecuteW(nullptr, L"open", g_msUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    // 2) 轮询令牌
    json tk;
    bool got = false;
    int waited = 0;
    while (waited < expires) {
        Sleep(interval * 1000);
        waited += interval;
        std::string tb = "grant_type=urn:ietf:params:oauth:grant-type:device_code"
                         "&client_id=" + g_cfg.msClientId +
                         "&device_code=" + deviceCode;
        json r; std::string e2;
        if (!PostJson((liveMode ? liveTok : (v2base + "/token")), tb, "application/x-www-form-urlencoded", r, e2)) continue;
        if (r.contains("access_token")) { tk = r; got = true; break; }
        std::string er = r.value("error", std::string());
        if (er.empty() || er == "authorization_pending" || er == "slow_down") continue;
        g_msState = 4;
        g_msMsg = L"登录被拒绝：" + U8ToW(r.value("error_description", er));
        if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
        return;
    }
    if (!got) {
        g_msState = 4;
        g_msMsg = L"登录超时，请重新开始";
        if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
        return;
    }

    std::string msToken = tk.value("access_token", std::string());
    std::string refresh = tk.value("refresh_token", std::string());

    // 3) Xbox Live
    g_msMsg = L"正在登录 Xbox Live…";
    g_msState = 1;
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);

    json xblBody = {
        { "Properties", { { "AuthMethod", "RPS" }, { "SiteName", "user.auth.xboxlive.com" },
                          { "RpsTicket", "d=" + msToken } } },
        { "RelyingParty", "http://auth.xboxlive.com" },
        { "TokenType", "JWT" }
    };
    json xbl; std::string e3;
    if (!PostJson("https://user.auth.xboxlive.com/user/authenticate", xblBody.dump(),
                  "application/json", xbl, e3) || !xbl.contains("Token")) {
        g_msState = 4; g_msMsg = L"Xbox Live 登录失败：" + U8ToW(e3);
        if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
        return;
    }
    std::string xblToken = xbl["Token"].get<std::string>();
    std::string uhs;
    try { uhs = xbl["DisplayClaims"]["xui"][0]["uhs"].get<std::string>(); } catch (...) {}

    // 4) XSTS
    json xstsBody = {
        { "Properties", { { "SandboxId", "RETAIL" }, { "UserTokens", json::array({ xblToken }) } } },
        { "RelyingParty", "rp://api.minecraftservices.com/" },
        { "TokenType", "JWT" }
    };
    json xsts; std::string e4;
    if (!PostJson("https://xsts.auth.xboxlive.com/xsts/authorize", xstsBody.dump(),
                  "application/json", xsts, e4) || !xsts.contains("Token")) {
        g_msState = 4;
        g_msMsg = L"XSTS 授权失败（账号可能未购买 Minecraft）";
        if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
        return;
    }
    std::string xstsToken = xsts["Token"].get<std::string>();
    try { uhs = xsts["DisplayClaims"]["xui"][0]["uhs"].get<std::string>(); } catch (...) {}

    // 5) 换 Minecraft 令牌
    g_msMsg = L"正在获取 Minecraft 令牌…";
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);

    json mcBody = { { "identityToken", "XBL3.0 x=" + uhs + ";" + xstsToken } };
    json mc; std::string e5;
    if (!PostJson("https://api.minecraftservices.com/authentication/login_with_xbox",
                  mcBody.dump(), "application/json", mc, e5) || !mc.contains("access_token")) {
        g_msState = 4; g_msMsg = L"获取 Minecraft 令牌失败：" + U8ToW(e5);
        if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
        return;
    }
    std::string mcToken = mc["access_token"].get<std::string>();

    // 6) 拉档案
    std::vector<unsigned char> d6; std::string e6;
    json prof;
    {
        std::string hdr = "application/json\r\nAuthorization: Bearer " + mcToken;
        if (HttpRequest("https://api.minecraftservices.com/minecraft/profile", "GET", "", hdr, d6, e6)
            && !d6.empty()) {
            prof = json::parse(std::string(d6.begin(), d6.end()), nullptr, false);
        }
    }
    if (!prof.is_object() || !prof.contains("id")) {
        g_msState = 4; g_msMsg = L"该账号没有 Minecraft: Java 版";
        if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
        return;
    }
    std::string uuid = prof.value("id", std::string());
    std::string name = prof.value("name", std::string());

    // 7) 存账户
    int idx = UpsertAccount("microsoft", name, "", mcToken, uuid);
    if (idx >= 0 && idx < (int)g_cfg.accounts.size()) g_cfg.accounts[idx].refresh = refresh;
    g_cfg.activeAcc = idx;
    g_cfg.authType  = "microsoft";
    g_cfg.authName  = name;
    g_cfg.authUuid  = uuid;
    g_cfg.authToken = mcToken;
    SaveConfig();

    g_msState = 3;
    g_msMsg = L"登录成功：" + U8ToW(name);
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
}

// ---------------- AddDirCand ----------------
static void AddDirCand(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_directory(p / L"versions", ec)) return;
    std::string s = WToU8(p.wstring());
    for (auto& c : g_dirCands) if (c.path == s) return;
    g_dirCands.push_back({ s, s });
}

// ---------------- RefreshDirCands ----------------
void RefreshDirCands() {
    g_dirCands.clear();
    wchar_t appdata[MAX_PATH] = {0}, profile[MAX_PATH] = {0};
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata);
    SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, profile);

    if (!g_cfg.gameDir.empty()) AddDirCand(GameDirPath());
    AddDirCand(fs::path(appdata) / L".minecraft");
    AddDirCand(fs::path(profile) / L".minecraft");
    for (const wchar_t* drv : { L"C:", L"D:", L"E:" }) {
        AddDirCand(fs::path(std::wstring(drv) + L"\\MC\\.minecraft"));
        AddDirCand(fs::path(std::wstring(drv) + L"\\Games\\.minecraft"));
        AddDirCand(fs::path(std::wstring(drv) + L"\\.minecraft"));
    }
}

// ---------------- JavaVersionOf ----------------
std::string JavaVersionOf(const fs::path& javaw) {
    fs::path home = javaw.parent_path().parent_path();
    std::ifstream f{ home / L"release" };
    if (!f) return "";
    std::string line;
    while (std::getline(f, line)) {
        size_t p = line.find("JAVA_VERSION=");
        if (p == std::string::npos) continue;
        std::string v = line.substr(p + 13);
        if (!v.empty() && v.front() == '"') v.erase(v.begin());
        if (!v.empty() && v.back() == '"') v.pop_back();
        if (v.rfind("1.", 0) == 0 && v.size() > 2) return std::string(1, v[2]); // 1.8.0 -> 8
        size_t dot = v.find('.');
        return (dot == std::string::npos) ? v : v.substr(0, dot);
    }
    return "";
}

// ---------------- AddJavaCand ----------------
static void AddJavaCand(const fs::path& javaw) {
    if (!FileExists(javaw)) return;
    std::string s = WToU8(javaw.wstring());
    for (auto& c : g_javaCands) if (c.path == s) return;
    std::string v = JavaVersionOf(javaw);
    g_javaCands.push_back({ s, v.empty() ? "版本未知" : ("Java " + v) });
}

// ---------------- RefreshJavaCands ----------------
void RefreshJavaCands() {
    g_javaCands.clear();

    wchar_t buf[32767];
    if (GetEnvironmentVariableW(L"JAVA_HOME", buf, 32767))
        AddJavaCand(fs::path(buf) / L"bin" / L"javaw.exe");

    wchar_t appdata[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
        fs::path hm = fs::path(appdata) / L".hmcl" / L"java" / L"windows-x86_64";
        std::error_code ec;
        if (DirExists(hm))
            for (auto& e : fs::directory_iterator(hm, ec))
                AddJavaCand(e.path() / L"bin" / L"javaw.exe");
    }

    for (const wchar_t* root : { L"C:\\Program Files\\Java", L"C:\\Program Files (x86)\\Java",
                                 L"D:\\Program Files\\Java", L"D:\\Java" }) {
        std::error_code ec;
        if (!DirExists(root)) continue;
        for (auto& e : fs::directory_iterator(root, ec))
            AddJavaCand(e.path() / L"bin" / L"javaw.exe");
    }

    {
        std::error_code ec;
        fs::path ms = L"D:\\MC\\Microsoft";
        if (DirExists(ms))
            for (auto& e : fs::directory_iterator(ms, ec))
                AddJavaCand(e.path() / L"bin" / L"javaw.exe");
    }
}

// ---------------- ReadJavaMajorFromJson ----------------
static int ReadJavaMajorFromJson(const fs::path& p) {
    std::ifstream f{ p };
    if (!f) return 0;
    std::stringstream ss; ss << f.rdbuf();
    std::string s = ss.str();
    std::regex re("\"majorVersion\"\\s*:\\s*(\\d+)");
    std::smatch m;
    if (std::regex_search(s, m, re)) {
        try { return std::stoi(m[1].str()); } catch (...) { return 0; }
    }
    return 0;
}

// ---------------- RequiredJavaMajor ----------------
int RequiredJavaMajor(const std::string& id) {
    fs::path base = GameDirPath() / L"versions" / fs::u8path(id);
    fs::path j1 = base / fs::u8path(id + ".json");

    int m = ReadJavaMajorFromJson(j1);
    if (m > 0) return m;

    // 顺着 inheritsFrom 找一层
    std::ifstream f{ j1 };
    if (f) {
        std::stringstream ss; ss << f.rdbuf();
        std::string s = ss.str();
        std::regex re("\"inheritsFrom\"\\s*:\\s*\"([^\"]+)\"");
        std::smatch mm;
        if (std::regex_search(s, mm, re)) {
            std::string parent = mm[1].str();
            int pm = ReadJavaMajorFromJson(base / fs::u8path(parent + ".json"));
            if (pm > 0) return pm;
        }
    }

    // 兜底：按版本号猜
    std::regex vre("1\\.(\\d+)(?:\\.(\\d+))?");
    std::smatch vm;
    if (std::regex_search(id, vm, vre)) {
        int minor = std::stoi(vm[1].str());
        int patch = vm[2].matched ? std::stoi(vm[2].str()) : 0;
        if (minor > 20 || (minor == 20 && patch >= 5)) return 21;
        if (minor >= 18) return 17;
        if (minor == 17) return 17;
        return 8;
    }
    return 0;
}

// ---------------- PickJavaFor ----------------
std::string PickJavaFor(int need) {
    if (g_javaCands.empty()) RefreshJavaCands();
    std::string best;
    int bestVer = 9999;
    for (auto& c : g_javaCands) {
        std::string v = JavaVersionOf(fs::u8path(c.path));
        if (v.empty()) continue;
        int iv = 0;
        try { iv = std::stoi(v); } catch (...) { continue; }
        if (iv >= need && iv < bestVer) { bestVer = iv; best = c.path; }
    }
    return best;
}

// ---------------- FetchManifestThread ----------------
void FetchManifestThread() {
    g_mcVersState = 1;
    std::vector<unsigned char> data;
    std::string err;
    bool ok = HttpRequest("https://piston-meta.mojang.com/mc/game/version_manifest_v2.json",
                          "GET", "", "", data, err);
    if (ok && !data.empty()) {
        std::string body(data.begin(), data.end());
        json j = json::parse(body, nullptr, false);
        if (j.is_object() && j.contains("versions") && j["versions"].is_array()) {
            std::vector<McVer> list;
            for (auto& v : j["versions"]) {
                McVer m;
                m.id   = v.value("id", std::string());
                m.type = v.value("type", std::string());
                m.url  = v.value("url", std::string());
                std::string rt = v.value("releaseTime", std::string());
                if (rt.size() >= 10) m.date = rt.substr(0, 10);
                if (!m.id.empty() && !m.url.empty()) list.push_back(m);
            }
            {
                std::lock_guard<std::mutex> lk(g_mcVersMutex);
                g_mcVers = std::move(list);      // 锁内写，move 避免拷贝
            }
            g_mcVersState = 2;
        } else g_mcVersState = 3;
    } else g_mcVersState = 3;
    // RebuildDlView();   // 删掉！让 UI 线程独占 g_dlView
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
}

// ---------------- BuildLaunchPlan ----------------
bool BuildLaunchPlan(const std::string& versionId, LaunchPlan& plan, std::string& error) {
    fs::path mcDir = GameDirPath();

    // 版本隔离：把 gameDir 指向 versions/<id>，让 Forge 用该版本自己的 mods
    std::string gameDirEff = g_cfg.gameDir;
    if (ShouldIsolate(versionId, mcDir)) {
        gameDirEff = WToU8((mcDir / L"versions" / fs::u8path(versionId)).wstring());
    }
    json vj = ResolveVersion(versionId);
    if (!vj.is_object()) { error = "版本 JSON 读取或解析失败"; return false; }

    std::string mainClass = vj.value("mainClass", std::string());
    if (mainClass.empty()) { error = "版本 JSON 里没有 mainClass"; return false; }

    std::string assetIndex = "legacy";
    if (vj.contains("assetIndex") && vj["assetIndex"].is_object())
        assetIndex = vj["assetIndex"].value("id", std::string("legacy"));
    else if (vj.contains("assets") && vj["assets"].is_string())
        assetIndex = vj["assets"].get<std::string>();

    fs::path nativesDir = mcDir / L"versions" / fs::u8path(versionId) / L"natives";
    {
        std::error_code ecd;
        fs::create_directories(nativesDir, ecd);
        if (ecd) {
            error = "无法创建 natives 目录（权限不足？）：" + ecd.message();
            return false;
        }
    }

    fs::path libRoot = mcDir / L"libraries";
    std::vector<std::string> classpath;
    std::vector<fs::path> nativeJars;

    if (vj.contains("libraries") && vj["libraries"].is_array()) {
        for (auto& lib : vj["libraries"]) {
            if (!lib.is_object()) continue;
            if (lib.contains("rules") && !RulesAllow(lib["rules"])) continue;
            std::string name = lib.value("name", std::string());
            if (name.empty()) continue;

            if (lib.contains("natives") && lib["natives"].is_object()) {
                std::string cls;
                if (lib["natives"].contains("windows") && lib["natives"]["windows"].is_string())
                    cls = ReplaceAll(lib["natives"]["windows"].get<std::string>(), "${arch}", "64");
                if (!cls.empty()) {
                    fs::path p = MavenPath(libRoot, name, cls);
                    if (!p.empty() && FileExists(p)) nativeJars.push_back(p);
                }
                continue;
            }

            fs::path full;
            if (lib.contains("downloads") && lib["downloads"].is_object() &&
                lib["downloads"].contains("artifact") && lib["downloads"]["artifact"].is_object()) {
                std::string rel = lib["downloads"]["artifact"].value("path", std::string());
                if (!rel.empty()) full = libRoot / fs::u8path(rel);
            }
            if (full.empty()) full = MavenPath(libRoot, name);
            if (!full.empty() && FileExists(full)) classpath.push_back(WToU8(full.wstring()));
        }
    }

    fs::path clientJar = mcDir / L"versions" / fs::u8path(versionId) / fs::u8path(versionId + ".jar");
    if (FileExists(clientJar)) classpath.push_back(WToU8(clientJar.wstring()));
    if (classpath.empty()) { error = "classpath 为空，可能缺少 libraries"; return false; }

    if (!nativeJars.empty()) {
        std::error_code ec;
        fs::remove_all(nativesDir, ec);
        {
        std::error_code ecd;
        fs::create_directories(nativesDir, ecd);
        if (ecd) {
            error = "无法创建 natives 目录（权限不足？）：" + ecd.message();
            return false;
        }
    }
        for (auto& nj : nativeJars) ExtractZip(nj, nativesDir);
    }

    std::string cp;
    for (size_t i = 0; i < classpath.size(); ++i) { if (i) cp += ";"; cp += classpath[i]; }

    // 账号信息：第三方优先
    bool msAcc = (g_cfg.authType == "microsoft" && !g_cfg.authToken.empty() && !g_cfg.authUuid.empty());
    bool thirdParty = (!msAcc && g_cfg.authType == "thirdparty" && !g_cfg.authToken.empty());
    std::string userName = msAcc ? g_cfg.authName : (thirdParty ? g_cfg.authName : g_cfg.playerName);
    std::string userUuid = msAcc ? g_cfg.authUuid : (thirdParty ? g_cfg.authUuid : OfflineUUID(g_cfg.playerName));
    std::string userToken = msAcc ? g_cfg.authToken : (thirdParty ? g_cfg.authToken : "0");
    std::string userType = msAcc ? "msa" : (thirdParty ? "mojang" : "offline");
    std::map<std::string, std::string> ph;
    ph["auth_player_name"]   = userName;
    ph["version_name"]       = versionId;
    ph["game_directory"]     = gameDirEff;
    ph["assets_root"]        = WToU8((mcDir / L"assets").wstring());
    ph["assets_index_name"]  = assetIndex;
    ph["auth_uuid"]          = userUuid;
    ph["auth_access_token"]  = userToken;
    ph["auth_session"]       = thirdParty ? ("token:" + userToken + ":" + userUuid) : "0";
    ph["user_type"]          = userType;
    ph["version_type"]       = vj.value("type", std::string("release"));
    ph["user_properties"]    = "{}";
    ph["natives_directory"]  = WToU8(nativesDir.wstring());
    ph["launcher_name"]      = "ReignLaunch";
    ph["launcher_version"]   = "1.1";
    ph["classpath"]          = cp;
    ph["classpath_separator"]= ";";
    ph["library_directory"]  = WToU8(libRoot.wstring());
    ph["clientid"]           = "0";
    ph["auth_xuid"]          = "0";
    ph["primary_jar"]        = versionId + ".jar";
    ph["primary_jar_name"]   = versionId + ".jar";

    auto sub = [&](const std::string& in) {
        std::string out = in;
        for (auto& kv : ph) out = ReplaceAll(out, "${" + kv.first + "}", kv.second);
        return out;
    };

    std::vector<std::string> jvmArgs, gameArgs;

    // 用户自定义参数（按空格拆分，支持带引号）
    auto splitArgs = [](const std::string& s) {
        std::vector<std::string> out;
        std::string cur; bool q = false;
        for (char c : s) {
            if (c == '"') { q = !q; continue; }
            if (c == ' ' && !q) { if (!cur.empty()) { out.push_back(cur); cur.clear(); } }
            else cur.push_back(c);
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    };
    if (vj.contains("arguments") && vj["arguments"].is_object()) {
        auto pushArg = [&](std::vector<std::string>& out, const std::string& raw) {
            std::string s = sub(raw);
            if (s.find("${") != std::string::npos) return;
            out.push_back(s);
        };
        auto collect = [&](const char* key, std::vector<std::string>& out) {
            if (!vj["arguments"].contains(key)) return;
            const json& arr = vj["arguments"][key];
            if (!arr.is_array()) return;
            for (auto& a : arr) {
                if (a.is_string()) pushArg(out, a.get<std::string>());
                else if (a.is_object()) {
                    if (a.contains("rules") && !RulesAllow(a["rules"])) continue;
                    if (!a.contains("value")) continue;
                    const json& val = a["value"];
                    if (val.is_string()) pushArg(out, val.get<std::string>());
                    else if (val.is_array())
                        for (auto& v : val) if (v.is_string()) pushArg(out, v.get<std::string>());
                }
            }
        };
        collect("jvm", jvmArgs);
        collect("game", gameArgs);
    } else if (vj.contains("minecraftArguments")) {
        gameArgs = SplitArgs(vj["minecraftArguments"].get<std::string>());
        for (auto& a : gameArgs) a = sub(a);
    }

    std::string javaExe = EffectiveJavaExe();

    std::wstring cmd = L"\"" + U8ToW(javaExe) + L"\"";
    cmd += L" -Xmx" + std::to_wstring(plan.maxMem) + L"M";

    // 第三方登录：authlib-injector
    if (thirdParty) {
        if (g_cfg.authlibPath.empty() || !FileExists(fs::u8path(g_cfg.authlibPath))) {
            error = "第三方登录需要 authlib-injector.jar，但没找到";
            return false;
        }
        cmd += L" -javaagent:\"" + U8ToW(g_cfg.authlibPath) + L"\"=" + U8ToW(g_cfg.authServer);
    }

    for (auto& a : splitArgs(g_cfg.extraJvm)) cmd += L" " + U8ToW(QuoteIfNeeded(a));
    for (auto& a : jvmArgs) cmd += L" " + U8ToW(QuoteIfNeeded(a));
    cmd += L" -cp \"" + U8ToW(cp) + L"\"";
    cmd += L" " + U8ToW(mainClass);
    for (auto& a : splitArgs(g_cfg.extraGame)) cmd += L" " + U8ToW(QuoteIfNeeded(a));
    if (g_cfg.winMode == 2) {
        cmd += L" --fullscreen";
    } else if (g_cfg.winMode == 1) {
        // 最大化：按桌面工作区尺寸开窗（减去任务栏）
        RECT wa{};
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
            int w = wa.right - wa.left, hh = wa.bottom - wa.top;
            cmd += L" --width " + std::to_wstring(w) + L" --height " + std::to_wstring(hh);
        }
    }
    for (auto& a : gameArgs) cmd += L" " + U8ToW(QuoteIfNeeded(a));

    plan.commandLine = cmd;
    plan.workDir = U8ToW(gameDirEff);
    plan.javaPath = U8ToW(javaExe);
    return true;
}

// ---------------- InstallVersionThread ----------------
void InstallVersionThread(std::string id, std::string url) {
    g_dlBusy = true;
    g_progState = 1;
    g_progCur = 0;
    g_progTotal = 0;
    g_status = L"正在下载版本 JSON…";

    g_progTotal = 1;
    g_progCur = 0;

    fs::path dir = GameDirPath() / L"versions" / fs::u8path(id);
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path jf = dir / fs::u8path(id + ".json");

    std::string err;
    if (!DownloadFile(url, jf, err)) {
        g_status = L"下载版本 JSON 失败：" + U8ToW(err);
        if (g_hwnd) PostMessageW(g_hwnd, WM_APP_ERROR, 0, 0);
        return;
    }

    g_status = L"正在补全文件…";
    DownloadStats st;
    DownloadMissing(id, st);
    g_progState = 0;
    g_dlBusy = false;                      // ★ 移出 if (g_hwnd)
    g_status = L"安装完成：" + U8ToW(id) + L"（下载 " + std::to_wstring(st.downloaded) + L" 个文件）";
    g_needScanVersions = true;             // ★ 交给 UI 线程扫描
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
}

// ---------------- FetchLoaderVersThread ----------------
void FetchLoaderVersThread(int kind, int gen) {
    if (gen != g_ldGen.load()) return;
    g_ldVersState = 1;
    g_ldSel = -1;

    std::string mc = g_dlDetailMc;
    std::string url;
    if (kind == 5) {
        url = "https://api.modrinth.com/v2/project/fabric-api/version?game_versions=[%22" + mc + "%22]&loaders=[%22fabric%22]";
    } else if (kind == 6) {
        url = "https://api.modrinth.com/v2/project/qsl/version?game_versions=[%22" + mc + "%22]&loaders=[%22quilt%22]";
    } else if (kind == 1) url = "https://bmclapi2.bangbang93.com/fabric-meta/v2/versions/loader/" + mc;
    else if (kind == 2) url = "https://meta.quiltmc.org/v3/versions/loader/" + mc;
    else if (kind == 3) url = "https://bmclapi2.bangbang93.com/forge/minecraft/" + mc;
    else if (kind == 4) url = "https://bmclapi2.bangbang93.com/neoforge/list/" + mc;
    else { g_ldVersState = 2; if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0); return; }

    std::vector<unsigned char> d;
    std::string e;
    if (!HttpRequest(url, "GET", "", "", d, e)) {
        g_ldVersState = 3;
        if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
        return;
    }
    std::vector<std::string> vers, files;
    json j = json::parse(std::string(d.begin(), d.end()), nullptr, false);
    if (j.is_array()) {
        for (auto& x : j) {
            std::string v;
            if (kind == 5 || kind == 6) {
                std::string v = x.value("version_number", x.value("name", std::string()));
                std::string fu, fn;
                if (x.contains("files") && x["files"].is_array() && !x["files"].empty()) {
                    fu = x["files"][0].value("url", std::string());
                    fn = x["files"][0].value("filename", std::string());
                }
                if (!v.empty()) { vers.push_back(v); files.push_back(fu + "\n" + fn); }
                continue;
            }
            if (kind == 1 || kind == 2) v = x["loader"].value("version", std::string());
            else v = x.value("version", std::string());
            if (!v.empty()) vers.push_back(v);
        }
    }
    if (gen != g_ldGen.load()) return;   // 期间切换过加载器，丢弃本次结果
    g_ldVers  = vers;
    g_ldFiles = files;
    g_ldVersState = 2;
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
}

// ---------------- IsAprilFools ----------------
static bool IsAprilFools(const McVer& v) {
    if (v.type == "release") return false;   // 正式版不算愚人节
    if (v.date.size() >= 10 && v.date.substr(5, 2) == "04" && v.date.substr(8, 2) == "01") return true;
    static const char* ids[] = { "15w14a", "1.rv-pre1", "2.0", "3D Shareware v1.34",
                                 "20w14infinite", "22w13oneblockatatime", "23w13a_or_b" };
    for (auto id : ids) if (v.id == id) return true;
    return false;
}

// ---------------- RebuildDlView ----------------
static void RebuildDlView() {
    // 锁内拷贝快照，锁外遍历
    std::vector<McVer> snapshot;
    {
        std::lock_guard<std::mutex> lk(g_mcVersMutex);
        snapshot = g_mcVers;
    }

    g_dlView.clear();
    std::wstring kw;
    {
        wchar_t buf[256] = { 0 };
        if (g_hEdSearch) GetWindowTextW(g_hEdSearch, buf, 256);
        kw = buf;
        for (auto& c : kw) c = (wchar_t)towlower(c);
    }

    for (size_t i = 0; i < snapshot.size(); ++i) {
        const McVer& v = snapshot[i];
        bool april = IsAprilFools(v);
        bool isRelease = (v.type == "release");
        bool isSnap    = (v.type == "snapshot");
        bool isOld     = (v.type == "old_beta" || v.type == "old_alpha");

        bool pass = false;
        switch (g_dlFilter) {
        case 0: pass = isRelease; break;
        case 1: pass = (isSnap && !april); break;
        case 2: pass = isOld; break;
        case 3: pass = april; break;
        default: pass = true; break;      // 4 = 所有版本
        }
        if (!pass) continue;

        if (!kw.empty()) {
            std::wstring idw = U8ToW(v.id);
            for (auto& c : idw) c = (wchar_t)towlower(c);
            if (idw.find(kw) == std::wstring::npos) continue;
        }
        g_dlView.push_back((int)i);
    }
    g_listScroll = 0;
    if (g_dlSel >= (int)g_dlView.size()) g_dlSel = -1;
}

// ---------------- HttpGetString ----------------
static std::string HttpGetString(const std::string& url, std::string& err) {
    std::vector<unsigned char> data;
    if (!HttpRequest(url, "GET", "", "", data, err)) return std::string();
    return std::string(data.begin(), data.end());
}

// ---------------- InstallMetaLoaderThread ----------------
void InstallMetaLoaderThread(std::string mc, bool quilt, std::string wantVer) {
    g_status = L"正在获取 " + std::wstring(quilt ? L"Quilt" : L"Fabric") + L" 加载器列表…";

    std::string url = quilt
        ? "https://meta.quiltmc.org/v3/versions/loader/" + mc
        : "https://bmclapi2.bangbang93.com/fabric-meta/v2/versions/loader/" + mc;

    std::string err;
    std::string body = HttpGetString(url, err);
    if (body.empty()) { g_status = L"加载器列表获取失败：" + U8ToW(err); if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0); return; }

    json j = json::parse(body, nullptr, false);
    if (!j.is_array() || j.empty()) { g_status = L"该版本没有可用的加载器"; if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0); return; }

    std::string lv = wantVer;
    if (lv.empty()) lv = j[0]["loader"].value("version", std::string());
    if (lv.empty()) { g_status = L"解析加载器版本失败"; if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0); return; }

    g_status = L"正在获取 profile（" + U8ToW(lv) + L"）…";
    std::string profUrl = quilt
        ? "https://meta.quiltmc.org/v3/versions/loader/" + mc + "/" + lv + "/profile/json"
        : "https://bmclapi2.bangbang93.com/fabric-meta/v2/versions/loader/" + mc + "/" + lv + "/profile/json";
    std::string prof = HttpGetString(profUrl, err);
    if (prof.empty()) { g_status = L"profile 获取失败：" + U8ToW(err); if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0); return; }

    json pj = json::parse(prof, nullptr, false);
    std::string vid = pj.value("id", std::string());
    if (vid.empty()) { g_status = L"profile 内容异常"; if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0); return; }

    fs::path dir = GameDirPath() / L"versions" / fs::u8path(vid);
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream f{ dir / fs::u8path(vid + ".json"), std::ios::binary | std::ios::trunc };
    f << prof;
    f.close();

    g_status = L"正在补全加载器依赖…";
    DownloadStats st;
    DownloadMissing(vid, st);
    g_progState = 0;
    g_dlBusy = false;                      // ★ 移出 if (g_hwnd)
    g_needScanVersions = true;             // ★ 交给 UI 线程扫描
    g_status = L"安装完成：" + U8ToW(vid) + L"（下载 " + std::to_wstring(st.downloaded) + L" 个文件）";
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
}
// ---------------- InstallInstallerThread ----------------
void InstallInstallerThread(std::string mc, bool neo, std::string wantVer) {
    std::string listUrl = neo
        ? "https://bmclapi2.bangbang93.com/neoforge/list/" + mc
        : "https://bmclapi2.bangbang93.com/forge/minecraft/" + mc;
    g_status = L"正在获取 " + std::wstring(neo ? L"NeoForge" : L"Forge") + L" 版本列表…";

    std::string err;
    std::string body = HttpGetString(listUrl, err);
    if (body.empty()) { g_status = L"版本列表获取失败：" + U8ToW(err); if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0); return; }

    json j = json::parse(body, nullptr, false);
    if (!j.is_array() || j.empty()) { g_status = L"该版本没有可用的加载器"; if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0); return; }

    std::string ver, dlUrl;
    if (!neo) {
        // [{"version":"47.4.23","mcversion":"1.20.1",...}]
        for (auto& v : j) {
            std::string vv = v.value("version", std::string());
            std::string branch = v.value("branch", std::string());
            if (vv.empty()) continue;
            if (ver.empty() || branch == "recommended") ver = vv;
        }
        if (ver.empty()) { g_status = L"未找到 Forge 版本"; if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0); return; }
        std::string full = mc + "-" + ver;
        dlUrl = "https://bmclapi2.bangbang93.com/maven/net/minecraftforge/forge/" + full +
                "/forge-" + full + "-installer.jar";
    } else {
        for (auto& v : j) {
            std::string vv = v.value("version", std::string());
            if (!vv.empty()) { ver = vv; break; }
        }
        if (ver.empty()) { g_status = L"未找到 NeoForge 版本"; if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0); return; }
        dlUrl = "https://bmclapi2.bangbang93.com/maven/net/neoforged/neoforge/" + ver +
                "/neoforge-" + ver + "-installer.jar";
    }

    wchar_t tmp[MAX_PATH] = { 0 };
    GetTempPathW(MAX_PATH, tmp);
    fs::path jar = fs::path(tmp) / L"reignlaunch_installer.jar";

    g_status = L"正在下载安装器 " + U8ToW(ver) + L"…";
    std::string e2;
    if (!DownloadFile(dlUrl, jar, e2)) {
        g_status = L"安装器下载失败：" + U8ToW(e2);
        RebuildDlView();
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
        return;
    }

    g_status = L"正在运行安装器（可能需要几分钟）…";
    std::string javaExe = EffectiveJavaExe();
    std::wstring cmd = L"\"" + U8ToW(javaExe) + L"\" -jar \"" + jar.wstring() + L"\" --installClient \"" +
                       U8ToW(g_cfg.gameDir) + L"\"";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);

    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, U8ToW(g_cfg.gameDir).c_str(), &si, &pi)) {
        g_status = L"安装器启动失败（错误码 " + std::to_wstring(GetLastError()) + L"）";
        RebuildDlView();
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
        return;
    }
    WaitForSingleObject(pi.hProcess, 600000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    { std::error_code ecr; fs::remove(jar, ecr); }

    g_dlBusy = false;                      // ★ 移出 if (g_hwnd)
    g_progState = 0;
    g_needScanVersions = true;             // ★ 交给 UI 线程扫描
    g_status = (code == 0) ? (L"安装完成：" + U8ToW(ver))
                           : (L"安装器退出码 " + std::to_wstring(code) + L"，请查看日志");
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
}

// ---------------- InstallModrinthModThread ----------------
// 从 Modrinth 下载一个前置模组（Fabric API / QSL）到指定实例的 mods 目录。
//   slug        Modrinth 项目 slug（"fabric-api" / "qsl"）
//   mcVer       Minecraft 版本（用于筛选适配版本）
//   loaderTag   "fabric" / "quilt"（用于筛选加载器）
//   targetVer   目标实例版本名（决定 mods 目录）
void InstallModrinthModThread(std::string slug, std::string mcVer,
                              std::string loaderTag, std::string targetVer) {
    std::string dispName = (slug == "qsl") ? "QSL" : "Fabric API";
    g_dlBusy = true;
    g_dlCancel = false;
    g_status = L"正在查找 " + U8ToW(dispName) + L" 的适配版本…";

    std::string err;
    std::string url = "https://api.modrinth.com/v2/project/" + slug +
                      "/version?game_versions=[%22" + mcVer + "%22]&loaders=[%22" +
                      loaderTag + "%22]";

    std::vector<unsigned char> d;
    if (!HttpRequest(url, "GET", "", "", d, err) || d.empty()) {
        g_status = L"没有适配此版本（" + U8ToW(mcVer) + L"）的 " + U8ToW(dispName);
        g_dlBusy = false;
        if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
        return;
    }

    json j = json::parse(std::string(d.begin(), d.end()), nullptr, false);
    if (!j.is_array() || j.empty()) {
        g_status = L"没有适配此版本（" + U8ToW(mcVer) + L"）的 " + U8ToW(dispName);
        g_dlBusy = false;
        if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
        return;
    }

    // 取第一个（Modrinth 按发布时间倒序）
    const json& v0 = j[0];
    std::string fileUrl, fileName;
    if (v0.contains("files") && v0["files"].is_array() && !v0["files"].empty()) {
        fileUrl  = v0["files"][0].value("url", std::string());
        fileName = v0["files"][0].value("filename", std::string());
    }
    std::string verNum = v0.value("version_number", v0.value("name", std::string()));

    if (fileUrl.empty() || fileName.empty()) {
        g_status = L"没有适配此版本（" + U8ToW(mcVer) + L"）的 " + U8ToW(dispName);
        g_dlBusy = false;
        if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
        return;
    }

    // 确保 mods 目录存在
    fs::path modsDir = ModsDirOf(targetVer);
    std::error_code ec;
    fs::create_directories(modsDir, ec);

    fs::path dest = modsDir / fs::u8path(fileName);
    if (FileExists(dest)) {
        g_status = U8ToW(dispName) + L" 已存在：" + U8ToW(fileName);
        g_dlBusy = false;
        if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
        return;
    }

    g_status = L"正在下载 " + U8ToW(dispName) + L" " + U8ToW(verNum) + L"…";
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);

    std::string dlErr;
    if (DownloadFile(fileUrl, dest, dlErr)) {
        g_status = U8ToW(dispName) + L" 安装完成：" + U8ToW(fileName);
        g_needScanVersions = true;
    } else {
        g_status = U8ToW(dispName) + L" 下载失败：" + U8ToW(dlErr);
    }
    g_dlBusy = false;
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
}

// ---------------- CheckModrinthModAvailable ----------------
// 只查询是否有适配版本，不下载（用于详情页显示状态）
bool CheckModrinthModAvailable(const std::string& slug, const std::string& mcVer,
                               const std::string& loaderTag, std::string& outVer) {
    outVer.clear();
    std::string url = "https://api.modrinth.com/v2/project/" + slug +
                      "/version?game_versions=[%22" + mcVer + "%22]&loaders=[%22" +
                      loaderTag + "%22]";
    std::vector<unsigned char> d;
    std::string err;
    if (!HttpRequest(url, "GET", "", "", d, err) || d.empty()) return false;
    json j = json::parse(std::string(d.begin(), d.end()), nullptr, false);
    if (!j.is_array() || j.empty()) return false;
    outVer = j[0].value("version_number", j[0].value("name", std::string()));
    return true;
}

// ---------------- UtcEpoch ----------------
static long long UtcEpoch(int y, int mo, int d, int h, int mi, int se) {
    y -= (mo <= 2);
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + (long long)doe - 719468;
    return days * 86400LL + h * 3600LL + mi * 60LL + se;
}

// ---------------- BuildEpoch ----------------
static long long BuildEpoch() {
    static long long v = -2;
    if (v != -2) return v;
    std::tm tm{};
    std::istringstream ss(std::string(__DATE__) + " " + __TIME__);   // "Nov  3 2026 20:06:05"
    ss >> std::get_time(&tm, "%b %d %Y %H:%M:%S");
    // __DATE__/__TIME__ 是本地时间，用 mktime 转成 UTC 秒
    v = ss.fail() ? 0 : (long long)mktime(&tm);
    return v;
}

// ---------------- ParseIsoTime ----------------
static long long ParseIsoTime(const std::string& s) {
    std::tm tm{};
    std::istringstream ss(s);
    ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (ss.fail()) return 0;
    return UtcEpoch(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                    tm.tm_hour, tm.tm_min, tm.tm_sec);
}

// ---------------- TagNewer ----------------
static bool TagNewer(const std::wstring& a, const std::wstring& b) {
    auto parse = [](std::string s) {
        if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(s.begin());
        std::vector<int> nums;
        double beta = 0;
        size_t dash = s.find('-');
        std::string mainPart = (dash == std::string::npos) ? s : s.substr(0, dash);
        std::string sub = (dash == std::string::npos) ? "" : s.substr(dash + 1);
        std::string cur;
        for (char c : mainPart) {
            if (c == '.') { if (!cur.empty()) { nums.push_back(atoi(cur.c_str())); cur.clear(); } }
            else cur.push_back(c);
        }
        if (!cur.empty()) nums.push_back(atoi(cur.c_str()));
        size_t bp = sub.find("beta");
        if (bp != std::string::npos) beta = atof(sub.c_str() + bp + 4);
        return std::make_pair(nums, beta);
    };
    auto A = parse(WToU8(a)), B = parse(WToU8(b));
    size_t n = std::max(A.first.size(), B.first.size());
    for (size_t i = 0; i < n; ++i) {
        int x = i < A.first.size() ? A.first[i] : 0;
        int y = i < B.first.size() ? B.first[i] : 0;
        if (x != y) return x > y;
    }
    return A.second > B.second;
}

// ---------------- UpdateThread ----------------
static void UpdateThread(bool interactive) {
    g_updateState = 1;
    std::vector<unsigned char> data;
    std::string err;
    bool ok = HttpRequest("https://api.github.com/repos/ShadowForgekin/ReignLaunch/releases",
                          "GET", "", "", data, err);
    if (ok && !data.empty()) {
        std::string body(data.begin(), data.end());
        json j = json::parse(body, nullptr, false);
        if (j.is_array()) {
            for (auto& r : j) {
                if (!r.is_object() || r.value("draft", false)) continue;
                std::string tag = r.value("tag_name", std::string());
                std::string url;
                if (r.contains("assets") && r["assets"].is_array()) {
                    for (auto& a : r["assets"]) {
                        if (a.value("name", std::string()) == "ReignLaunch.exe") {
                            url = a.value("browser_download_url", std::string());
                            break;
                        }
                    }
                }
                if (tag.empty() || url.empty()) continue;
                g_newTag = U8ToW(tag);
                g_newUrl = url;
                // 按发布时间判断：比本机编译时间晚即为新版本
                long long pub = ParseIsoTime(r.value("published_at", std::string()));
                bool newer;
                if (tag == WToU8(APP_TAG)) {
                    newer = false;                      // 同一个版本
                } else if (pub > 0) {
                    newer = (pub > BuildEpoch() + 120); // 发布时间晚于本机构建
                } else {
                    newer = TagNewer(g_newTag, APP_TAG); // 兜底：没有时间就比版本号
                }
                g_updateState = newer ? 2 : 3;
                break;
            }
            if (g_updateState == 1) g_updateState = 3;
        } else g_updateState = 4;
    } else {
        g_updateState = 4;
    }
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_UPD, interactive ? 1 : 0, 0);
}

// ---------------- StartUpdate ----------------
static void StartUpdate() {
    if (g_updating.load()) return;
    g_updating = true;

    wchar_t exePath[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    fs::path cur = exePath;
    fs::path nw = cur;
    nw += L".new";

    // 依次尝试：直连 -> gh-proxy 加速
    std::vector<std::string> urls;
    urls.push_back(g_newUrl);
    urls.push_back("https://gh-proxy.com/" + g_newUrl);
    urls.push_back("https://gh-proxy.com/https://gh-proxy.com/" + g_newUrl);

    bool ok = false;
    std::string lastErr;
    for (size_t i = 0; i < urls.size() && !ok; ++i) {
        std::string err;
        Log("[更新] 尝试下载: " + urls[i]);
        ok = DownloadFile(urls[i], nw, err);
        if (!ok) lastErr = err;
    }
    if (!ok) {
        g_updating = false;
        MessageBoxW(g_hwnd, (L"下载新版本失败：\n" + U8ToW(lastErr) +
                             L"\n\n可尝试开启代理后重试。").c_str(), L"更新失败", MB_OK | MB_ICONERROR);
        return;
    }

    fs::path bat = cur;
    bat += L".update.bat";
    {
        std::ofstream f{ bat, std::ios::binary | std::ios::trunc };
        std::string s;
        s += "@echo off\r\n";
        s += "timeout /t 2 /nobreak >nul\r\n";
        s += ":wait\r\n";
        s += "tasklist /fi \"imagename eq " + WToU8(cur.filename().wstring()) + "\" 2>nul | find /i \"" +
             WToU8(cur.filename().wstring()) + "\" >nul\r\n";
        s += "if not errorlevel 1 (timeout /t 1 /nobreak >nul & goto wait)\r\n";
        s += "move /y \"" + WToU8(nw.wstring()) + "\" \"" + WToU8(cur.wstring()) + "\" >nul\r\n";
        s += "start \"\" \"" + WToU8(cur.wstring()) + "\"\r\n";
        s += "del \"%~f0\"\r\n";
        f << s;
    }

    ShellExecuteW(nullptr, L"open", bat.c_str(), nullptr, nullptr, SW_HIDE);
    MessageBoxW(g_hwnd, L"新版本已下载，启动器会关闭并自动替换为最新版。", L"更新", MB_OK | MB_ICONINFORMATION);
    DestroyWindow(g_hwnd);
}

// ---------------- MixColor ----------------
static COLORREF MixColor(COLORREF a, COLORREF b, double t) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int r = (int)(GetRValue(a) * (1 - t) + GetRValue(b) * t);
    int g = (int)(GetGValue(a) * (1 - t) + GetGValue(b) * t);
    int bl = (int)(GetBValue(a) * (1 - t) + GetBValue(b) * t);
    return RGB(r, g, bl);
}

// ---------------- CurrentLoaderTag ----------------
std::string CurrentLoaderTag() {
    if (g_selVersion < 0 || g_selVersion >= (int)g_versions.size()) return "fabric";
    std::string id = g_versions[g_selVersion];
    for (auto& c : id) c = (char)tolower((unsigned char)c);
    if (id.find("neoforge") != std::string::npos) return "neoforge";
    if (id.find("forge") != std::string::npos)    return "forge";
    if (id.find("quilt") != std::string::npos)    return "quilt";
    return "fabric";
}

// ---------------- CurrentMcVersion ----------------
std::string CurrentMcVersion() {
    if (g_selVersion < 0 || g_selVersion >= (int)g_versions.size()) return "";
    std::string id = g_versions[g_selVersion];

    // 1) 版本 JSON 里的 inheritsFrom 最准
    {
        fs::path jp = GameDirPath() / L"versions" / fs::u8path(id) / fs::u8path(id + ".json");
        std::ifstream f{ jp };
        if (f) {
            std::stringstream ss; ss << f.rdbuf();
            std::string s = ss.str();
            std::regex re("\"inheritsFrom\"\\s*:\\s*\"([^\"]+)\"");
            std::smatch m;
            if (std::regex_search(s, m, re)) return m[1].str();
        }
    }

    // 2) 用官方版本清单做最长前缀匹配（"26.3-Fabric" -> "26.3"）
    {
        std::lock_guard<std::mutex> lk(g_mcVersMutex);
        if (!g_mcVers.empty()) {
            std::string best;
            for (auto& v : g_mcVers) {
                const std::string& cand = v.id;
                if (cand.size() > id.size()) continue;
                if (id.compare(0, cand.size(), cand) != 0) continue;
                if (cand.size() > best.size()) best = cand;
            }
            if (!best.empty()) return best;
        }
    }

    // 3) 兜底：砍掉 -fabric / -forge / -neoforge / -quilt 及其后内容
    std::string low = id;
    for (auto& c : low) c = (char)tolower((unsigned char)c);
    const char* seps[] = { "-fabric", "-forge", "-neoforge", "-quilt", " fabric", " forge" };
    for (auto* s : seps) {
        size_t pos = low.find(s);
        if (pos != std::string::npos && pos > 0) return id.substr(0, pos);
    }
    return id;
}

// ---------------- InstanceModsDir ----------------
fs::path InstanceModsDir() {
    fs::path mcDir = GameDirPath();
    if (g_selVersion >= 0 && g_selVersion < (int)g_versions.size() &&
        ShouldIsolate(g_versions[g_selVersion], mcDir))
        return mcDir / L"versions" / fs::u8path(g_versions[g_selVersion]) / L"mods";
    return mcDir / L"mods";
}

// ---------------- RefreshInstalledMods ----------------
void RefreshInstalledMods() {
    g_modFiles.clear();
    std::error_code ec;
    fs::path d = InstanceModsDir();
    if (fs::exists(d, ec)) {
        for (auto& e : fs::directory_iterator(d, ec)) {
            if (!e.is_regular_file()) continue;
            std::string n = WToU8(e.path().filename().wstring());
            if (n.size() > 4 && (n.substr(n.size() - 4) == ".jar" ||
                                 n.substr(n.size() - 13) == ".jar.disabled"))
                g_modFiles.push_back(n);
        }
        std::sort(g_modFiles.begin(), g_modFiles.end());
    }
}

// ---------------- 按指定实例枚举模组 ----------------
fs::path ModsDirOf(const std::string& versionId) {
    fs::path mcDir = GameDirPath();
    if (ShouldIsolate(versionId, mcDir))
        return mcDir / L"versions" / fs::u8path(versionId) / L"mods";
    return mcDir / L"mods";
}

std::vector<std::string> ListModsOf(const std::string& versionId) {
    std::vector<std::string> out;
    std::error_code ec;
    fs::path d = ModsDirOf(versionId);
    if (!fs::exists(d, ec)) return out;
    for (auto& e : fs::directory_iterator(d, ec)) {
        if (!e.is_regular_file()) continue;
        std::string n = WToU8(e.path().filename().wstring());
        if (n.size() > 4 && (n.substr(n.size() - 4) == ".jar" ||
                             (n.size() > 13 && n.substr(n.size() - 13) == ".jar.disabled")))
            out.push_back(n);
    }
    std::sort(out.begin(), out.end());
    return out;
}

void ToggleModIn(const std::string& versionId, const std::string& fileName, std::string& err) {
    fs::path dir = ModsDirOf(versionId);
    std::error_code ec;
    bool disabled = (fileName.size() > 13 && fileName.substr(fileName.size() - 13) == ".jar.disabled");
    std::string target = disabled ? fileName.substr(0, fileName.size() - 9) : (fileName + ".disabled");
    fs::rename(dir / fs::u8path(fileName), dir / fs::u8path(target), ec);
    if (ec) { err = "重命名失败：" + ec.message(); return; }
    err.clear();
}

bool DeleteModIn(const std::string& versionId, const std::string& fileName, std::string& err) {
    fs::path dir = ModsDirOf(versionId);
    std::error_code ec;
    fs::remove(dir / fs::u8path(fileName), ec);
    if (ec) { err = "删除失败：" + ec.message(); return false; }
    err.clear();
    return true;
}

// ---------------- UrlEncode ----------------
static std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out; out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') out.push_back((char)c);
        else { out.push_back('%'); out.push_back(hex[c >> 4]); out.push_back(hex[c & 0x0F]); }
    }
    return out;
}

// ---------------- FetchCurseForgeViaProxy ----------------
static bool FetchCurseForgeViaProxy(std::string query, std::string mcVersion) {
    // 依次尝试：配置的中转站 → 内置多个中转站（自动检测可用性） → 官方 API（需 Key）
    // 说明：CurseForge 官方 API 需要申请 Key；这里用公开中转站兜底
    static const char* kCfMirrors[] = {
        "https://api.curse.tools/v1/cf",     // curse.tools 公共中转（无需 Key）
        "https://cf.mcim.moe/v1/cf",         // MCIM 中转站
        "https://mcim.bangbang93.com/v1/cf", // BMCLAPI 生态的中转
    };

    std::vector<std::pair<std::string, bool>> mirrors;   // (base, 是否需要 key)
    if (!g_cfg.cfProxy.empty()) mirrors.push_back({ g_cfg.cfProxy, false });
    for (const char* m : kCfMirrors) {
        bool dup = false;
        for (auto& e2 : mirrors) if (e2.first == m) { dup = true; break; }
        if (!dup) mirrors.push_back({ m, false });
    }
    if (!g_cfg.cfApiKey.empty())
        mirrors.push_back({ "https://api.curseforge.com/v1", true });

    std::string qs = "?gameId=432&classId=6&pageSize=20&sortField=2&sortOrder=desc";
    if (!query.empty())     qs += "&searchFilter=" + UrlEncode(query);
    if (!mcVersion.empty()) qs += "&gameVersion=" + UrlEncode(mcVersion);

    for (auto& m : mirrors) {
        std::string url = m.first + "/mods/search" + qs;
        std::string ctype = "application/json";
        if (m.second && !g_cfg.cfApiKey.empty())
            ctype += "\r\nx-api-key: " + g_cfg.cfApiKey;   // 官方 API 需要的头

        std::vector<unsigned char> d; std::string e;
        if (!HttpRequest(url, "GET", "", ctype, d, e)) continue;
        if (d.empty()) continue;

        json j = json::parse(std::string(d.begin(), d.end()), nullptr, false);
        if (!j.is_object() || !j.contains("data") || !j["data"].is_array()) continue;

        std::vector<ModHit> hits;
        for (auto& x : j["data"]) {
            ModHit h;
            long long id = x.value("id", 0LL);
            h.title     = x.value("name", std::string());
            h.desc      = x.value("summary", std::string());
            h.downloads = x.value("downloadCount", 0LL);
            if (id <= 0 || h.title.empty()) continue;
            h.slug = "cf:" + std::to_string(id);
            hits.push_back(h);
        }
        if (hits.empty()) continue;

        g_modHits = hits;
        return true;
    }
    return false;
}

// ---------------- FetchModSearchThread ----------------
static void FetchModSearchThread(std::string q, int gen) {
    if (gen != g_modGen.load()) return;
    { std::lock_guard<std::mutex> lk(g_modHitsMutex); g_modHits.clear(); }
    g_modSel = -1;
    std::vector<ModHit> hits;   // 先收本地，最后一次性写入
    std::string mc = CurrentMcVersion();
    std::string loader = CurrentLoaderTag();

    // CurseForge 源：走公共镜像，免 API Key
    if (g_modSource == 1) {
        if (FetchCurseForgeViaProxy(q, mc)) {
            g_modState = 2;
            if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
            return;
        }
        g_cfFallback = true;   // 镜像不可用：提示并切回 Modrinth
        g_modSource = 0;
    }

    // 分类：Modrinth 用 project_type 过滤
    // 0=mod 1=modpack 2=datapack 3=resourcepack 4=shader
    static const char* kModTypes[5] = { "mod", "modpack", "datapack", "resourcepack", "shader" };

    // ---------- 拼 URL ----------
    // 注意：facets 只能出现一次，多个条件要合并在一个 facets 数组里
    // 结构：[[project_type:X],[versions:Y],[categories:Z]]  这是「与」关系
    std::string base = "https://api.modrinth.com/v2/search?limit=20";

    std::string queryPart;
    if (!q.empty()) queryPart = "&query=" + UrlEncode(q);   // ★ 必须编码，中文/空格才能搜
    else            queryPart = "&index=downloads";          // 空搜索按下载量排

    bool isMod = (g_modCat == 0);
    std::vector<std::string> urls;

    // 1) 最精确：类型 + MC 版本 + 加载器（仅 mod 类有加载器概念）
    if (isMod && !mc.empty()) {
        urls.push_back(base + queryPart +
            "&facets=%5B%5B%22project_type%3A" + std::string(kModTypes[g_modCat]) + "%22%5D%2C" +
            "%5B%22versions%3A" + mc + "%22%5D%2C" +
            "%5B%22categories%3A" + loader + "%22%5D%5D");
    }
    // 2) 退一步：类型 + 加载器
    if (isMod) {
        urls.push_back(base + queryPart +
            "&facets=%5B%5B%22project_type%3A" + std::string(kModTypes[g_modCat]) + "%22%5D%2C" +
            "%5B%22categories%3A" + loader + "%22%5D%5D");
    }
    // 3) 再退一步：只要类型
    urls.push_back(base + queryPart +
        "&facets=%5B%5B%22project_type%3A" + std::string(kModTypes[g_modCat]) + "%22%5D%5D");
    // 4) 兜底：完全不过滤（有搜索词时才有意义）
    if (!q.empty()) urls.push_back(base + queryPart);
    bool anyOk = false;
    for (auto& url : urls) {
        std::vector<unsigned char> d; std::string e;
        if (!HttpRequest(url, "GET", "", "", d, e)) continue;
        json j = json::parse(std::string(d.begin(), d.end()), nullptr, false);
        if (!(j.is_object() && j.contains("hits") && j["hits"].is_array())) continue;
        anyOk = true;
        for (auto& h : j["hits"]) {
            ModHit m;
            m.slug      = h.value("slug", std::string());
            m.title     = h.value("title", std::string());
            m.desc      = h.value("description", std::string());
            m.downloads = h.value("downloads", 0LL);
            if (!m.slug.empty()) hits.push_back(m);
        }
        if (!hits.empty()) break;   // 有结果就停
    }
    if (gen != g_modGen.load()) return;      // 已有更新的搜索，丢弃本次
    { std::lock_guard<std::mutex> lk(g_modHitsMutex); g_modHits = std::move(hits); }
    g_modState = anyOk ? 2 : 3;
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
}
// ---------------- FetchModVersThread ----------------
static void FetchModVersThread(std::string slug, int gen) {
    std::vector<std::string> vNames, vFiles;   // 先收在本地，最后一次性加锁写入
    g_modVerState = 1;
    g_modVerSel = -1;
    std::string mc = CurrentMcVersion();
    std::string loader = CurrentLoaderTag();
    // CurseForge 来源（slug = cf:<id>）：走镜像取文件列表
    if (slug.rfind("cf:", 0) == 0) {
        std::string id = slug.substr(3);
        // 同样自动检测中转站
        std::string curl;
        {
            static const char* kMirrors[] = {
                "https://api.curse.tools/v1/cf",
                "https://cf.mcim.moe/v1/cf",
                "https://mcim.bangbang93.com/v1/cf",
            };
            for (const char* m : kMirrors) {
                std::string probe = std::string(m) + "/mods/" + id + "/files?pageSize=1";
                std::vector<unsigned char> pd; std::string pe;
                if (HttpRequest(probe, "GET", "", "", pd, pe) && !pd.empty()) {
                    curl = std::string(m) + "/mods/" + id + "/files?pageSize=50";
                    break;
                }
            }
            if (curl.empty()) curl = std::string("https://api.curse.tools/v1/cf/mods/") + id + "/files?pageSize=50";
        }
        if (!mc.empty()) curl += "&gameVersion=" + UrlEncode(mc);
        std::vector<unsigned char> cd; std::string ce;
        if (HttpRequest(curl, "GET", "", "", cd, ce)) {
            json cj = json::parse(std::string(cd.begin(), cd.end()), nullptr, false);
            if (cj.is_object() && cj.contains("data") && cj["data"].is_array()) {
                for (auto& f : cj["data"]) {
                    std::string fn = f.value("fileName", std::string());
                    std::string dl = f.value("downloadUrl", std::string());
                    std::string dn = f.value("displayName", fn);
                    if (!dl.empty()) { vNames.push_back(dn); vFiles.push_back(dl + "\n" + fn); }
                }
                { std::lock_guard<std::mutex> lk(g_modVerMutex); g_modVerNames = vNames; g_modVerFiles = vFiles; }
                g_modVerState = vNames.empty() ? 3 : 2;
                if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
                return;
            }
        }
        g_modVerState = 3;
        if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
        return;
    }

    std::string base = "https://api.modrinth.com/v2/project/" + slug + "/version";

    // 依次放宽：版本+加载器 -> 仅加载器 -> 全部
    std::vector<std::string> urls;
    if (!mc.empty())
        urls.push_back(base + "?game_versions=%5B%22" + mc + "%22%5D&loaders=%5B%22" + loader + "%22%5D");
    urls.push_back(base + "?loaders=%5B%22" + loader + "%22%5D");
    urls.push_back(base);

    bool anyOk = false;
    for (auto& url : urls) {
        std::vector<unsigned char> d; std::string e;
        if (!HttpRequest(url, "GET", "", "", d, e)) continue;
        json j = json::parse(std::string(d.begin(), d.end()), nullptr, false);
        if (!j.is_array()) continue;
        anyOk = true;
        for (auto& v : j) {
            std::string name = v.value("version_number", v.value("name", std::string()));
            std::string fu, fnn;
            if (v.contains("files") && v["files"].is_array() && !v["files"].empty()) {
                fu  = v["files"][0].value("url", std::string());
                fnn = v["files"][0].value("filename", std::string());
            }
            if (!fu.empty()) { vNames.push_back(name); vFiles.push_back(fu + "\n" + fnn); }
        }
        if (!vNames.empty()) break;   // 有结果就停
    }
    if (gen != g_modVerGen.load()) return;   // 期间又点了别的模组，丢弃
    { std::lock_guard<std::mutex> lk(g_modVerMutex); g_modVerNames = vNames; g_modVerFiles = vFiles; }
    g_modVerState = (anyOk && !vNames.empty()) ? 2 : 3;
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
}

// ---------------- InstallModVersionThread ----------------
static void InstallModVersionThread(std::string url, std::string fname) {
    g_dlBusy = true; g_dlCancel = false; g_dlBytes = 0;
    g_dlTarget = U8ToW(g_modDetailTitle);
    g_progState = 1; g_progTotal = 1; g_progCur = 0;
    g_status = L"正在下载模组…";

    std::error_code ec;
    fs::path dir = InstanceModsDir();
    fs::create_directories(dir, ec);
    std::string err;
    if (DownloadFile(url, dir / fs::u8path(fname), err)) {
        g_progCur = 1;
        g_status = L"已安装模组：" + U8ToW(fname);
        RefreshInstalledMods();
    } else {
        g_status = L"下载失败：" + U8ToW(err);
    }
    g_dlBusy = false; g_progState = 0;
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
}

// ---------------- InstallModThread ----------------
static void InstallModThread(std::string slug) {
    g_dlBusy = true; g_dlCancel = false; g_dlBytes = 0;
    g_dlTarget = U8ToW(slug);
    g_progState = 1; g_progCur = 0; g_progTotal = 0;
    g_status = L"正在获取模组版本…";

    std::string mc = CurrentMcVersion();
    std::string loader = CurrentLoaderTag();
    std::string url = "https://api.modrinth.com/v2/project/" + slug + "/version";
    if (!mc.empty()) url += "?game_versions=%5B%22" + mc + "%22%5D&loaders=%5B%22" + loader + "%22%5D";

    std::vector<unsigned char> d; std::string e;
    if (!HttpRequest(url, "GET", "", "", d, e)) {
        g_status = L"获取模组版本失败"; g_dlBusy = false; g_progState = 0;
        if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
        return;
    }
    json j = json::parse(std::string(d.begin(), d.end()), nullptr, false);
    if (!j.is_array() || j.empty()) {
        g_status = L"没有适配当前版本的模组文件"; g_dlBusy = false; g_progState = 0;
        if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
        return;
    }
    std::string fileUrl, fileName;
    if (j[0].contains("files") && j[0]["files"].is_array() && !j[0]["files"].empty()) {
        fileUrl  = j[0]["files"][0].value("url", std::string());
        fileName = j[0]["files"][0].value("filename", std::string());
    }
    if (fileUrl.empty()) {
        g_status = L"该版本没有可下载文件"; g_dlBusy = false; g_progState = 0;
        if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
        return;
    }

    std::error_code ec;
    fs::path dir = InstanceModsDir();
    fs::create_directories(dir, ec);
    g_progTotal = 1; g_progCur = 0;
    std::string err;
    if (DownloadFile(fileUrl, dir / fs::u8path(fileName), err)) {
        g_progCur = 1;
        g_status = L"已安装模组：" + U8ToW(fileName);
        RefreshInstalledMods();
    } else {
        g_status = L"下载失败：" + U8ToW(err);
    }
    g_dlBusy = false; g_progState = 0;
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
}

// ---------------- LoginThread ----------------
static void LoginThread(std::string srv, std::string user, std::string pass) {
    AuthResult r = YggdrasilLogin(srv, user, pass);
    if (r.ok) {
        CaptureActiveAccount();
        int accIdx = UpsertAccount("thirdparty", r.name, TrimSlash(srv), r.accessToken, r.uuid);
        g_cfg.activeAcc = accIdx;
        g_cfg.authType = "thirdparty";
        g_cfg.authServer = TrimSlash(srv);
        g_cfg.authToken = r.accessToken;
        g_cfg.authClientToken = r.clientToken;
        g_cfg.authUuid = r.uuid;
        g_cfg.authName = r.name;
        {
        std::string emb = ExtractEmbeddedAuthlib();
        if (!emb.empty()) {
            g_cfg.authlibPath = emb;
            Log("[内置] authlib-injector 已释放: " + emb);
        } else if (g_cfg.authlibPath.empty()) {
            g_cfg.authlibPath = DetectAuthlib();
        }
    }
        SaveConfig();
        g_status = L"登录成功：" + U8ToW(r.name);
        g_loginState = 2;
    } else {
        g_status = L"登录失败：" + U8ToW(r.err);
        g_loginState = 3;
    }
    PostMessageW(g_hwnd, WM_APP_LOGIN, 0, 0);
}

// ---------------- PickFolder ----------------
static bool PickFolder(HWND owner, std::string& out) {
    BROWSEINFOW bi{};
    bi.hwndOwner = owner;
    bi.lpszTitle = L"选择 .minecraft 目录";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;
    wchar_t path[MAX_PATH] = {0};
    bool ok = SHGetPathFromIDListW(pidl, path) != 0;
    CoTaskMemFree(pidl);
    if (ok) out = WToU8(path);
    return ok;
}

// ---------------- PickJava ----------------
static bool PickJava(HWND owner, std::string& out) {
    wchar_t file[MAX_PATH] = {0};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Java (javaw.exe)\0javaw.exe;java.exe\0所有文件\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return false;
    out = WToU8(file);
    return true;
}


// ============================================================================
//  第八部分：账户与杂项封装
// ============================================================================
// ============================================================================
//  core_extra.cpp — 为 UI 层补充的封装函数
//  原 Win32 版这些逻辑散在 MainProc 里，这里收敛成可调用函数
// ============================================================================

// ---------------- 启动游戏（在工作线程里调用）----------------
void LaunchGame(std::string versionId) {
    // 原 LaunchThread 直接可用
    LaunchThread(versionId);
}

// ---------------- 微软登录（无参版本，给 UI 用）----------------
void MicrosoftLoginSimple() {
    MicrosoftLoginThread();
}

// ---------------- 模组启用 / 禁用 ----------------
void ToggleMod(const std::string& fileName, std::string& err) {
    fs::path dir = InstanceModsDir();
    std::error_code ec;

    const bool disabled = (fileName.size() > 13 &&
                           fileName.substr(fileName.size() - 13) == ".jar.disabled");
    const std::string target = disabled
        ? fileName.substr(0, fileName.size() - 9)      // 去掉 ".disabled"
        : (fileName + ".disabled");

    fs::rename(dir / fs::u8path(fileName), dir / fs::u8path(target), ec);
    if (ec) { err = "重命名失败：" + ec.message(); return; }

    RefreshInstalledMods();
    err.clear();
}

// ---------------- 删除模组 ----------------
bool DeleteMod(const std::string& fileName, std::string& err) {
    fs::path dir = InstanceModsDir();
    std::error_code ec;
    fs::remove(dir / fs::u8path(fileName), ec);
    if (ec) { err = "删除失败：" + ec.message(); return false; }
    RefreshInstalledMods();
    err.clear();
    return true;
}

// ---------------- 离线账户：新建或切换 ----------------
void CreateOfflineAccount(const std::string& name) {
    if (name.empty()) return;
    CaptureActiveAccount();

    int idx = UpsertAccount("offline", name, "", "", OfflineUUID(name));
    g_cfg.activeAcc = idx;
    g_cfg.authType  = "offline";
    g_cfg.playerName = name;
    g_cfg.authName  = name;
    g_cfg.authUuid  = OfflineUUID(name);
    g_cfg.authToken.clear();
    SaveConfig();
    Log("[账户] 已创建离线账户：" + name);
}

// ---------------- 第三方登录 ----------------
bool ThirdPartyLogin(const std::string& server, const std::string& user,
                     const std::string& pass, std::string& err) {
    AuthResult r = YggdrasilLogin(server, user, pass);
    if (!r.ok) { err = r.err.empty() ? "登录失败" : r.err; return false; }

    CaptureActiveAccount();
    int idx = UpsertAccount("thirdparty", r.name, TrimSlash(server), r.accessToken, r.uuid);
    g_cfg.activeAcc = idx;
    g_cfg.authType  = "thirdparty";
    g_cfg.authServer = TrimSlash(server);
    g_cfg.authToken = r.accessToken;
    g_cfg.authClientToken = r.clientToken;
    g_cfg.authUuid  = r.uuid;
    g_cfg.authName  = r.name;
    SaveConfig();
    err.clear();
    Log("[账户] 第三方登录成功：" + r.name);
    return true;
}

// ---------------- 切换激活账户 ----------------
void SelectAccount(int idx) {
    if (idx < 0 || idx >= (int)g_cfg.accounts.size()) return;
    CaptureActiveAccount();

    const Account& a = g_cfg.accounts[idx];
    g_cfg.activeAcc = idx;
    g_cfg.authType  = a.type;
    g_cfg.authName  = a.name;
    g_cfg.authUuid  = a.uuid;
    g_cfg.authToken = a.token;
    g_cfg.authServer = a.server;
    if (a.type == "offline") g_cfg.playerName = a.name;
    SaveConfig();
    Log("[账户] 切换到：" + a.name);
}

// ---------------- 删除账户 ----------------
void RemoveAccount(int idx) {
    if (idx < 0 || idx >= (int)g_cfg.accounts.size()) return;
    g_cfg.accounts.erase(g_cfg.accounts.begin() + idx);
    if (g_cfg.activeAcc >= (int)g_cfg.accounts.size()) g_cfg.activeAcc = (int)g_cfg.accounts.size() - 1;
    SaveConfig();
}

// ---------------- 刷新版本 + 选中默认 ----------------
void ReloadVersions() {
    g_versions.clear();
    ScanVersions();
    if (g_selVersion < 0 || g_selVersion >= (int)g_versions.size())
        g_selVersion = g_versions.empty() ? -1 : 0;
}

// ---------------- 设置 .minecraft 目录 ----------------
void SetGameDir(const std::string& dir) {
    if (dir.empty()) return;
    g_cfg.gameDir = dir;
    SaveConfig();
    ReloadVersions();
}


// ============================================================================
//  第九部分：界面实现与入口
// ============================================================================
// 布局要点：
//   子控件坐标必须转成绝对坐标（页面 x/y + 相对偏移），否则会错位到顶栏。
//   统一用 addTo(page, widget, relX, relY) 添加控件。
// ============================================================================

// ============================ 页面 ============================
enum UiPage { P_LAUNCH = 0, P_DOWNLOAD, P_USER, P_SETTINGS, P_COUNT };

static const char* kTabNames[P_COUNT] = { "启动", "下载", "用户", "设置" };

static UiPage g_curPage = P_LAUNCH;
static Fl_Double_Window* g_win = nullptr;
static Overlay* g_overlay = nullptr;

class PageBase;
class PageUser;
static PageBase* doSwitch(UiPage p);
static void showAbout();
static void backFromAbout();
static void launchSelected();

// ============================ 通用辅助 ============================
static void addTo(Fl_Group* page, Fl_Widget* w, int relX, int relY) {
    if (!page || !w) return;
    // 页面创建时位于 (0,0)，子控件坐标即相对坐标
    w->position(relX, relY);
    page->add(w);
}
// 自绘圆角按钮
class RoundButton : public Fl_Button {
public:
    bool accent_;
    bool hidden_ = false;   // true 时不绘制、不响应
    RoundButton(int X, int Y, int W, int H, const char* L, bool accent = false)
        : Fl_Button(X, Y, W, H, L), accent_(accent) {
        box(FL_NO_BOX);
    }
    void draw() override {
        if (hidden_) return;
        bool hot = (Fl::belowmouse() == this);
        bool on  = active();
        Fl_Color bg;
        if (accent_) bg = hot ? clr_accent() : clr_accent2();
        else         bg = hot ? clr_card_sel() : clr_card();
        fill_round_rect(x(), y(), w(), h(), 8, bg);
        draw_text_center(x(), y(), w(), h(), label() ? label() : "",
                         on ? clr_text() : clr_dim(), 15);
    }
    int handle(int e) override {
        if (hidden_) return 0;
        if (e == FL_ENTER || e == FL_LEAVE) { redraw(); return 1; }
        return Fl_Button::handle(e);
    }
};

// 圆角输入框
class RoundInput : public Fl_Input {
public:
    bool hidden_ = false;   // true 时不绘制、不响应（FLTK 的 hide() 会被 add()/show() 强行覆盖）

    RoundInput(int X, int Y, int W, int H) : Fl_Input(X, Y, W, H, nullptr) {
        box(FL_NO_BOX);
        color(clr_inputbg());
        textcolor(clr_text());
        textfont(FL_HELVETICA);
        textsize(14);
        selection_color(clr_accent());
    }
    void draw() override {
        if (hidden_) return;
        fill_round_rect(x(), y(), w(), h(), 6, clr_inputbg());
        Fl_Input::draw();
    }
    int handle(int e) override {
        if (hidden_) return 0;
        return Fl_Input::handle(e);
    }
};

// 圆角下拉框
class RoundChoice : public Fl_Choice {
public:
    RoundChoice(int X, int Y, int W, int H) : Fl_Choice(X, Y, W, H, nullptr) {
        box(FL_NO_BOX);
        color(clr_inputbg());
        textcolor(clr_text());
        textsize(14);
    }
    void draw() override {
        fill_round_rect(x(), y(), w(), h(), 6, clr_inputbg());
        // 画下拉箭头
        int ax = x() + w() - 18, ay = y() + h() / 2 - 2;
        fl_color(clr_dim());
        fl_begin_polygon();
        fl_vertex(ax - 5, ay);
        fl_vertex(ax + 5, ay);
        fl_vertex(ax, ay + 6);
        fl_end_polygon();
        // 文字
        const char* t = (value() >= 0 && text()) ? text(value()) : "";
        draw_text(x() + 10, y(), w() - 28, h(), t, clr_text(), 14);
    }
};

// ============================ 启动打点 ============================
static void TR(const char* s) {
    FILE* f = nullptr;
    fopen_s(&f, "C:\\tmp\\trace.txt", "a");
    if (f) { std::fprintf(f, "%s\n", s); std::fflush(f); std::fclose(f); }
}
// ============================ 标签按钮 ============================
class TabButton : public Fl_Button {
public:
    TabButton(int X, int Y, int W, int H, const char* L, int idx)
        : Fl_Button(X, Y, W, H, L), idx_(idx) {
        box(FL_NO_BOX);
    }
    void draw() override {
        bool active = ((int)g_curPage == idx_);
        bool hot = (Fl::belowmouse() == this);
        fill_round_rect(x(), y(), w(), h(), 8,
                        (active || hot) ? clr_card_sel() : clr_card());
        draw_text_center(x(), y(), w(), h(), label() ? label() : "",
                         active ? clr_text() : clr_dim(), 15);
    }
    int handle(int e) override {
        if (e == FL_PUSH) { doSwitch((UiPage)idx_); return 1; }
        if (e == FL_ENTER || e == FL_LEAVE) { redraw(); return 1; }
        return Fl_Button::handle(e);
    }
private:
    int idx_;
};

// ============================ 页面基类 ============================
class PageBase : public Fl_Group {
public:
    // ---------- 统一滚动支持 ----------
    // 每个页面声明自己的「内容总高度」；窗口比它矮时自动出现滚动条，
    // 滚轮/拖动都走这里，子类只要重写 contentHeight() 和 drawContent()。
    int pageScroll = 0;

    explicit PageBase(int W, int H) : Fl_Group(0, 0, W, H) {
        box(FL_NO_BOX);
        color(clr_bg());
    }
    virtual void refresh() {}
    virtual void onShown() {}
    virtual void onHidden() {}

    // 子类返回「内容需要多高」；返回 0 表示不需要滚动（自适应即可）
    virtual int contentHeight() { return 0; }

    int maxPageScroll() {
        int ch = contentHeight();
        if (ch <= 0) return 0;
        int v = h() - contentHeight();
        return v < 0 ? -v : 0;
    }

    void clampPageScroll() {
        int m = maxPageScroll();
        if (pageScroll < 0) pageScroll = 0;
        if (pageScroll > m) pageScroll = m;
    }

    // 画右侧滚动条（各页面在 draw() 末尾调用）
    void drawPageScrollbar() {
        int m = maxPageScroll();
        if (m <= 0) return;
        int vis = h();
        int ch = contentHeight();
        int barH = vis * vis / ch;
        if (barH < 30) barH = 30;
        int barY = y() + (vis - barH) * pageScroll / m;
        fill_round_rect(x() + w() - 10, barY, 6, barH, 3, clr_card_sel());
    }

    // 统一的滚轮处理（子类在 handle 里先调用它）
    bool handlePageWheel(int e, int dy) {
        if (e != FL_MOUSEWHEEL) return false;
        if (maxPageScroll() <= 0) return false;
        pageScroll += dy * 48;   // FLTK: 向下滚 dy>0
        clampPageScroll();
        redraw();
        return true;
    }
};

static PageBase* g_pages[P_COUNT] = { nullptr };
static PageBase* g_aboutPage = nullptr;

// 显示/隐藏页面时连子控件一起处理，否则隐藏页的输入框会浮在上面挡住标签
static void setPageVisible(Fl_Group* pg, bool on) {
    if (!pg) return;
    if (on) {
        for (int i = 0; i < pg->children(); ++i) pg->child(i)->show();
        pg->show();
        if (auto* pb = dynamic_cast<PageBase*>(pg)) pb->onShown();
    } else {
        for (int i = 0; i < pg->children(); ++i) pg->child(i)->hide();
        pg->hide();
        if (auto* pb = dynamic_cast<PageBase*>(pg)) pb->onHidden();
    }
    if (!pg) return;
    if (on) {
        for (int i = 0; i < pg->children(); ++i) pg->child(i)->show();
        pg->show();
    } else {
        for (int i = 0; i < pg->children(); ++i) pg->child(i)->hide();
        pg->hide();
    }
}

class PageSettings;   // 前置声明
static void (*g_resetSettings)() = nullptr;   // 切页后复位设置页状态

static PageBase* doSwitch(UiPage p) {
    if ((int)p >= P_COUNT) return nullptr;
    g_curPage = p;
    g_page = (Page)p;
    for (int i = 0; i < P_COUNT; ++i) {
        if (!g_pages[i]) continue;
        if (i == (int)p) { setPageVisible(g_pages[i], true); g_pages[i]->refresh(); }
        else             setPageVisible(g_pages[i], false);
    }
    if (g_aboutPage) setPageVisible(g_aboutPage, false);
    if (g_win) { g_win->redraw(); }

    return g_pages[p];
}

static void showAbout() {
    for (int i = 0; i < P_COUNT; ++i) if (g_pages[i]) setPageVisible(g_pages[i], false);
    if (g_aboutPage) { setPageVisible(g_aboutPage, true); g_aboutPage->refresh(); g_aboutPage->redraw(); }
    if (g_win) g_win->redraw();
}

static void backFromAbout() {
    if (g_aboutPage) setPageVisible(g_aboutPage, false);
    doSwitch(P_SETTINGS);
}
static void launchSelected() {
    if (g_launching) return;
    if (g_selVersion < 0 || g_selVersion >= (int)g_versions.size()) {
        g_status = L"请先选择一个实例";   // 不弹系统框，底部提示即可
        if (g_win) g_win->redraw();
        return;
    }
    const std::string ver = g_versions[g_selVersion];
    g_launching = true;
    g_overlayDoneTick = 0;
    g_cancelLaunch = false;
    if (g_overlay) g_overlay->show(ver);
    std::thread([ver]() { LaunchThread(ver); }).detach();
}

static const int LP_LEFT = 300;

//  布局仿原 Win32 版：
//    左侧侧边栏（实例卡片列表）  中间信息区  右下角启动按钮 + 右侧向上箭头


// ============================ 启动页 ============================
//  左侧导航栏（仿设置页）：启动游戏 / 实例
//  实例页里可以对每个实例管理模组（启用/禁用/删除）
static const int LP_SIDEBAR = 300;

// ============================ 启动页 ============================
//  左侧导航栏（仿设置页）：启动游戏 / 实例
//  实例页：每个实例是一张卡片，右侧有「设置」按钮
//          点设置 → 该实例的设置面板，里面可以管理模组

// ============================ 启动页 ============================
//  左侧导航栏（仿设置页）：启动游戏 / 实例
//  实例页：卡片直接由 PageLaunch::draw() 绘制，点击由 handle() 处理
//         （不用 RowList 子控件，避免两层事件拦截导致点击失灵）

class PageLaunch : public PageBase {
public:
    int subTab = 0;            // 0=启动游戏 1=实例
    int detailIdx = -1;        // >=0 表示正在看某个实例的设置
    int cardScroll = 0;        // 卡片列表滚动
    std::vector<std::string> modsCache_;
    std::vector<int> modCounts_;
    RoundButton* btnRefresh = nullptr;

    static const int CARD_H = 96;
    static const int CARD_GAP = 6;

    PageLaunch(int W, int H) : PageBase(W, H) {
        // 纯自绘页面，不加任何子控件（除了刷新按钮）
        btnRefresh = new RoundButton(0, 0, 90, 30, "刷新");
        btnRefresh->callback([](Fl_Widget*, void* d) {
            auto* self = (PageLaunch*)d;
            ReloadVersions();
            self->detailIdx = -1;
            self->refresh();
            g_status = L"已刷新";
        }, this);
        addTo(this, btnRefresh, LP_SIDEBAR + 20, 14);

        end();
        refresh();
        subTab = 0;
        syncVisibility();
    }

    void resize(int X, int Y, int W, int H) override {
        PageBase::resize(X, Y, W, H);
        if (btnRefresh) btnRefresh->resize(X + LP_SIDEBAR + 20, Y + 14, 90, 30);
    }

    void syncVisibility() {
        if (btnRefresh) btnRefresh->hidden_ = (subTab != 1);
        redraw();
    }

    void setTab(int i) {
        if (subTab == i) return;
        subTab = i;
        if (subTab != 1) { detailIdx = -1; cardScroll = 0; }
        syncVisibility();
    }

    std::string curVersionId() const {
        if (g_selVersion >= 0 && g_selVersion < (int)g_versions.size())
            return g_versions[g_selVersion];
        return "";
    }

    void refresh() override {
        modCounts_.clear();
        for (auto& v : g_versions) modCounts_.push_back((int)ListModsOf(v).size());

        if (detailIdx >= 0 && detailIdx < (int)g_versions.size())
            modsCache_ = ListModsOf(g_versions[detailIdx]);
        else
            modsCache_.clear();

        redraw();
    }

    // ---------- 布局 ----------
    int cardX() const { return x() + LP_SIDEBAR + 20; }
    int cardW() const { return w() - LP_SIDEBAR - 40; }
    int cardY(int i) const { return y() + 56 + i * (CARD_H + CARD_GAP) - cardScroll; }

    // 计算某张卡片的「设置」按钮矩形
    void settingsBtnRect(int i, int& bx, int& by, int& bw, int& bh) const {
        bw = 84; bh = 34;
        int ry = cardY(i);
        bx = cardX() + cardW() - bw - 16;
        by = ry + (CARD_H - bh) / 2;
    }

    static bool inRect(int rx, int ry, int rw, int rh, int mx, int my) {
        return mx >= rx && mx < rx + rw && my >= ry && my < ry + rh;
    }

    // ---------- 绘制 ----------
    void draw() override {
        fill_round_rect(x(), y(), w(), h(), 0, clr_bg());

        // 左侧导航栏
        fill_round_rect(x(), y(), LP_SIDEBAR, h(), 0, clr_sidebar());
        draw_text(x() + 20, y() + 14, LP_SIDEBAR - 40, 30, "启动", clr_dim(), 12);

        int iy = y() + 50;
        for (int i = 0; i < 2; ++i) {
            int ix = x() + 12, iw = LP_SIDEBAR - 24, ih = 38;
            bool sel = (subTab == i);
            bool hot = inRect(ix, iy, iw, ih, Fl::event_x(), Fl::event_y());
            fill_round_rect(ix, iy, iw, ih, 8,
                            sel ? clr_card_sel() : (hot ? clr_card_sel() : clr_card()));
            draw_text(ix + 14, iy, iw - 28, ih, i == 0 ? "启动游戏" : "实例",
                      sel ? clr_text() : clr_dim(), 15);
            if (sel) fill_round_rect(ix, iy + 8, 3, ih - 16, 0, clr_accent2());
            iy += 46;
        }

        if (subTab == 0) drawLaunchTab();
        else              drawInstanceTab();

        PageBase::draw();
    }

    void drawLaunchTab() {
        int cx = x() + LP_SIDEBAR + 40;
        int cw = w() - LP_SIDEBAR - 80;
        if (cw < 120) cw = 120;

        draw_text(cx, y() + 20, cw, 26, "启动设置", clr_dim(), 12);

        std::string acct;
        if (g_cfg.authType == "microsoft")        acct = "账号: " + g_cfg.authName + "（微软）";
        else if (g_cfg.authType == "thirdparty")  acct = "账号: " + g_cfg.authName + "（第三方）";
        else                                      acct = "账号: " + g_cfg.playerName + "（离线）";

        draw_text(cx, y() + 56,  cw, 26, acct, clr_dim(), 15);
        draw_text(cx, y() + 82,  cw, 26, "内存: " + std::to_string(g_cfg.maxMemMB) + " MB", clr_dim(), 15);

        std::string ver = curVersionId().empty() ? "（未选择）" : curVersionId();
        draw_text(cx, y() + 108, cw, 26, "版本: " + ver, clr_dim(), 15);

        if (!g_cfg.javaPath.empty()) {
            std::string jv = JavaVersionOf(fs::u8path(g_cfg.javaPath));
            draw_text(cx, y() + 134, cw, 22,
                      std::string("Java: ") + (jv.empty() ? std::string("未知版本") : jv),
                      jv.empty() ? clr_err() : clr_ok(), 12);
        }

        // 右下角启动按钮
        int bw = 220, bh = 64;
        int bx = x() + w() - 24 - bw;
        int by = y() + h() - 24 - bh;

        int pst = g_progState.load();
        if (pst != 0) {
            int total = g_progTotal.load(), cur = g_progCur.load();
            float p = (pst == 1 && total > 0) ? ((float)cur / (float)total) : 1.0f;
            if (p > 1.0f) p = 1.0f;
            if (p < 0.0f) p = 0.0f;
            std::string s2 = (pst == 1)
                ? ("正在补全文件  " + std::to_string(cur) + " / " + std::to_string(total))
                : "正在启动游戏…";
            draw_text(bx - 20, by - 96, bw + 20, 24, s2, clr_dim(), 12);
            int barY = by - 64;
            fill_round_rect(bx - 20, barY, bw + 20, 16, 8, clr_card());
            int fw = (int)((bw + 20) * p);
            if (fw > 4) fill_round_rect(bx - 20, barY, fw, 16, 8, clr_accent2());
        }

        bool hot = inRect(bx, by, bw, bh, Fl::event_x(), Fl::event_y());
        fill_round_rect(bx, by, bw, bh, 10,
                        g_launching ? clr_card() : (hot ? clr_accent() : clr_accent2()));
        draw_text_center(bx, by, bw, bh, g_launching ? "启动中…" : "启动游戏",
                         g_launching ? clr_dim() : FL_WHITE, 18);

        if (!g_status.empty())
            draw_text(cx, y() + 166, cw, 80, g_status, clr_dim(), 12);
    }

    void drawInstanceTab() {
        int cx = cardX();
        int cw = cardW();
        if (cw < 120) cw = 120;

        if (detailIdx >= 0 && detailIdx < (int)g_versions.size()) {
            drawDetail(cx, cw);
            return;
        }

        // ---------- 卡片列表 ----------
        draw_text(cx, y() + 18, 240, 30,
                  "实例 (" + std::to_string(g_versions.size()) + ")", clr_text(), 15);

        if (g_versions.empty()) {
            draw_text(cx, y() + 90, cw, 60,
                      "没有找到实例，请检查游戏目录设置", clr_err(), 13);
            return;
        }

        fl_push_clip(cx, y() + 52, cw, h() - 60);
        for (int i = 0; i < (int)g_versions.size(); ++i) {
            int ry = cardY(i);
            if (ry + CARD_H < y() + 52 || ry > y() + h()) continue;
            bool cur = (i == g_selVersion);

            fill_round_rect(cx, ry + 4, cw, CARD_H - 10, 10,
                            cur ? clr_card_sel() : clr_card());
            if (cur) fill_round_rect(cx + 3, ry + 14, 3, CARD_H - 30, 0, clr_accent2());

            std::string n = g_versions[i];
            draw_text(cx + 20, ry + 12, cw - 160, 30, n, clr_text(), 17);

            std::string tag = "原版";
            if (n.find("Fabric") != std::string::npos) tag = "Fabric";
            else if (n.find("Quilt") != std::string::npos) tag = "Quilt";
            else if (n.find("NeoForge") != std::string::npos) tag = "NeoForge";
            else if (n.find("Forge") != std::string::npos) tag = "Forge";
            draw_text(cx + 20, ry + 42, cw - 160, 20, tag, clr_dim(), 12);

            int nm = (i < (int)modCounts_.size()) ? modCounts_[i] : 0;
            draw_text(cx + 20, ry + 62, cw - 160, 20,
                      std::to_string(nm) + " 个模组", clr_dim(), 11);

            if (cur) draw_text_right(cx + 20, ry + CARD_H - 26, cw - 130, 18, "当前", clr_ok(), 11);

            // 设置按钮
            int bx, by, bw, bh;
            settingsBtnRect(i, bx, by, bw, bh);
            bool hot = inRect(bx, by, bw, bh, Fl::event_x(), Fl::event_y());
            fill_round_rect(bx, by, bw, bh, 7, hot ? clr_accent() : clr_accent2());
            draw_text_center(bx, by, bw, bh, "设置", FL_WHITE, 14);
        }
        fl_pop_clip();

        if (!g_status.empty())
            draw_text(cx, y() + h() - 30, cw, 24, g_status, clr_dim(), 12);
    }

    void drawDetail(int cx, int cw) {
        const std::string vid = g_versions[detailIdx];

        // 标题
        draw_text(cx, y() + 14, cw - 110, 34, vid, clr_text(), 20);

        // 返回按钮
        int bbx = cx + cw - 90, bby = y() + 16, bbw = 90, bbh = 30;
        bool hotBack = inRect(bbx, bby, bbw, bbh, Fl::event_x(), Fl::event_y());
        fill_round_rect(bbx, bby, bbw, bbh, 7, hotBack ? clr_card_sel() : clr_card());
        draw_text_center(bbx, bby, bbw, bbh, "返回", clr_text(), 14);

        // 模组数
        draw_text(cx, y() + 56, cw, 22,
                  "模组 (" + std::to_string(modsCache_.size()) + ")", clr_dim(), 12);

        if (modsCache_.empty()) {
            draw_text(cx, y() + 92, cw, 70,
                      "这个实例没有安装模组\n把 .jar 放进该实例的 mods 文件夹后会显示在这里",
                      clr_dim(), 12);
        } else {
            int my = y() + 84;
            const int rowH = 36;
            int maxRows = (h() - 160) / rowH;
            if (maxRows < 1) maxRows = 1;

            for (int i = 0; i < (int)modsCache_.size() && i < maxRows; ++i) {
                const std::string& f = modsCache_[i];
                bool dis = (f.size() > 13 && f.substr(f.size() - 13) == ".jar.disabled");

                fill_round_rect(cx, my, cw, rowH - 4, 6, clr_card());

                // 状态点
                fl_color(dis ? clr_dim() : clr_ok());
                fl_pie(cx + 8, my + rowH / 2 - 4, 8, 8, 0, 360);

                draw_text(cx + 26, my, cw - 240, rowH - 4, f,   // 给右侧按钮留位
                          dis ? clr_dim() : clr_text(), 12);

                // 启用/禁用
                int tb = cx + cw - 190, tw = 88;
                bool hotT = inRect(tb, my + 3, tw, rowH - 10, Fl::event_x(), Fl::event_y());
                fill_round_rect(tb, my + 3, tw, rowH - 10, 5,
                                hotT ? clr_accent() : clr_accent2());
                draw_text_center(tb, my + 3, tw, rowH - 10,
                                 dis ? "启用" : "禁用", FL_WHITE, 11);

                // 删除
                int db = cx + cw - 96, dw = 88;
                bool hotD = inRect(db, my + 3, dw, rowH - 10, Fl::event_x(), Fl::event_y());
                fill_round_rect(db, my + 3, dw, rowH - 10, 5,
                                hotD ? clr_card() : clr_card_sel());
                draw_text_center(db, my + 3, dw, rowH - 10, "删除", clr_err(), 11);

                my += rowH;
            }
        }

        // 元信息
        int my = y() + h() - 90;
        std::string acct;
        if (g_cfg.authType == "microsoft")        acct = g_cfg.authName + "（微软）";
        else if (g_cfg.authType == "thirdparty")  acct = g_cfg.authName + "（外置）";
        else                                      acct = g_cfg.playerName + "（离线）";

        draw_text(cx, my, 80, 24, "账号", clr_dim(), 12);
        draw_text(cx + 80, my, cw - 80, 24, acct, clr_text(), 12);
        draw_text(cx, my + 26, 80, 24, "mods", clr_dim(), 12);
        draw_text(cx + 80, my + 26, cw - 80, 24,
                  WToU8(ModsDirOf(vid).wstring()), clr_dim(), 11);

        if (!g_status.empty())
            draw_text(cx, y() + h() - 32, cw - 200, 24, g_status, clr_dim(), 12);
    }

    // ---------- 事件 ----------
    int handle(int e) override {
        int mx = Fl::event_x(), my = Fl::event_y();

        if (e == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) {
            // 左侧导航
            if (mx >= x() && mx < x() + LP_SIDEBAR) {
                int iy = y() + 50;
                for (int i = 0; i < 2; ++i) {
                    if (my >= iy && my < iy + 38) { setTab(i); return 1; }
                    iy += 46;
                }
                return 1;
            }

            if (subTab == 0) {
                // 启动按钮
                int bw = 220, bh = 64;
                int bx = x() + w() - 24 - bw;
                int by = y() + h() - 24 - bh;
                if (inRect(bx, by, bw, bh, mx, my)) { launchSelected(); return 1; }
                return 1;
            }

            // ---------- 实例页 ----------
            int cx = cardX(), cw = cardW();

            if (detailIdx >= 0 && detailIdx < (int)g_versions.size()) {
                // 返回
                if (inRect(cx + cw - 90, y() + 16, 90, 30, mx, my)) {
                    detailIdx = -1;
                    modsCache_.clear();
                    redraw();
                    return 1;
                }
                // 模组按钮
                const std::string vid = g_versions[detailIdx];
                int rowH = 36;
                int maxRows = (h() - 160) / rowH;
                if (maxRows < 1) maxRows = 1;
                for (int i = 0; i < (int)modsCache_.size() && i < maxRows; ++i) {
                    int ry = y() + 84 + i * rowH;
                    int tb = cx + cw - 190, tw = 88;
                    int db = cx + cw - 96,  dw = 88;
                    if (inRect(tb, ry + 3, tw, rowH - 10, mx, my)) {
                        std::string fn = modsCache_[i];
                        std::string err;
                        ToggleModIn(vid, fn, err);
                        g_status = err.empty() ? (L"已切换：" + U8ToW(fn))
                                               : (L"失败：" + U8ToW(err));
                        refresh();
                        return 1;
                    }
                    if (inRect(db, ry + 3, dw, rowH - 10, mx, my)) {
                        std::string fn = modsCache_[i];
                        std::string err;
                        if (DeleteModIn(vid, fn, err)) g_status = L"已删除：" + U8ToW(fn);
                        else                           g_status = L"删除失败：" + U8ToW(err);
                        refresh();
                        return 1;
                    }
                }
                return 1;
            }

            // 卡片点击：先判「设置」按钮，再判整卡
            if (my >= y() + 52) {
                for (int i = 0; i < (int)g_versions.size(); ++i) {
                    int ry = cardY(i);
                    if (ry + CARD_H < y() + 52 || ry > y() + h()) continue;
                    if (my < ry || my >= ry + CARD_H) continue;

                    int bx, by, bw, bh;
                    settingsBtnRect(i, bx, by, bw, bh);
                    if (inRect(bx, by, bw, bh, mx, my)) {
                        detailIdx = i;
                        g_selVersion = i;
                        modsCache_ = ListModsOf(g_versions[i]);
                        g_status = L"打开设置：" + U8ToW(g_versions[i]);
                        redraw();
                        return 1;
                    }
                    if (mx >= cx && mx < cx + cw) {
                        g_selVersion = i;
                        g_status = L"已选择实例：" + U8ToW(g_versions[i]);
                        redraw();
                        return 1;
                    }
                }
            }
            return 1;
        }

        if (e == FL_MOUSEWHEEL && subTab == 1 && detailIdx < 0) {
            int maxS = (int)g_versions.size() * (CARD_H + CARD_GAP) - (h() - 60);
            if (maxS < 0) maxS = 0;
            cardScroll += Fl::event_dy() * 40;
            if (cardScroll < 0) cardScroll = 0;
            if (cardScroll > maxS) cardScroll = maxS;
            redraw();
            return 1;
        }

        if (e == FL_ENTER || e == FL_LEAVE || e == FL_MOVE) { redraw(); return 1; }
        return PageBase::handle(e);
    }
};

// ============================ 下载页 ============================
//  纯自绘：版本列表 + 版本详情（点卡片进入）
//  加载器下拉已移除（"原版"那栏），位置留给后面的 Mod 下载
class PageDownload : public PageBase {
public:
    int filterType = 0;        // 0=正式版 1=快照版 2=远古版 3=所有
    bool filterDropOpen = false;   // 版本类型下拉是否展开
    int  syncTabCache = -1;   // 上次同步时的 dlTab/detailIdx（避免每帧 resize）
    int  syncDetailCache = -2;
    int selIdx = -1;           // 列表里选中的行（g_dlView 下标）
    int detailIdx = -1;        // >=0 表示在版本详情（g_dlView 下标）
    int scroll = 0;
    std::string filter;        // 搜索词
    std::string detailErr;     // 详情加载错误

    // 详情缓存
    std::string dId, dType, dDate, dUrl;
    std::string dMcVer, dJavaVer, dLoader;
    long long dSizeAssets = 0;
    int dLibCount = 0, dFileCount = 0;
    bool detailLoaded = false;
    int loaderSel = 0;                       // 选中的加载器（0=原版）
    bool loaderDropOpen = false;             // 加载器下拉是否展开
    static const char* kLoaderNames[7];   // 原版/Fabric/Quilt/Forge/NeoForge/QSL/FabricAPI

    // ---------- 加载器版本自选 ----------
    std::vector<std::string> ldrVers;        // 该加载器的可用版本
    int  ldrSel = -1;                        // 选中的版本下标（-1=自动最新）
    bool ldrVerOpen = false;                 // 版本下拉是否展开
    int  ldrVerScroll = 0;
    bool ldrVersLoaded = false;              // 是否已拉取过
    int  ldrVersFor = -1;                    // 已拉取的加载器种类
    std::string ldrVersMc;                   // 已拉取对应的 MC 版本
    std::string ldrVersErr;                  // 拉取失败原因

    RoundInput* search = nullptr;
    RoundInput* modSearch = nullptr;   // 模组页搜索框
    RoundButton* btnInstall = nullptr;

    static const int ITEM_H = 52;
    static const int ITEM_GAP = 5;
    static const int TYPE_W = 130;
    static const int TOOLBAR_Y = 18;
    static const int TAB_W = 72;
    // ==================== 标签页：0=版本 1=模组 ====================
    int dlTab = 0;

    // ---------- 模组页状态 ----------
    std::string modQuery;          // 搜索词
    int  modScroll = 0;
    int  modSel = -1;              // 选中的搜索结果
    int  modDetail = -1;           // >=0 进入模组详情
    std::string modDetailSlug, modDetailTitle, modDetailDesc;
    int  modVerSel = -1;           // 选中的模组版本
    int  modVerScroll = 0;
    std::string modErr;
    bool modAutoLoaded = false;    // 是否已自动拉过热门列表
    int  modCat = 0;    // 0=Mod 1=整合包 2=数据包 3=资源包 4=光影包


    PageDownload(int W, int H) : PageBase(W, H) {
        // 版本页的搜索框
        search = new RoundInput(0, 0, 220, 32);
        search->callback([](Fl_Widget* w, void* d) {
            auto* self = (PageDownload*)d;
            self->filter = ((Fl_Input*)w)->value();
            self->rebuild();
        }, this);
        addTo(this, search, W - 240, TOOLBAR_Y);

        // 模组页的搜索框（真控件，可输入）
        modSearch = new RoundInput(0, 0, 300, 32);
        modSearch->callback([](Fl_Widget* w, void* d) {
            auto* self = (PageDownload*)d;
            self->modQuery = ((Fl_Input*)w)->value();
        }, this);
        addTo(this, modSearch, CAT_W + 16, TOOLBAR_Y);   // 与 modSearchRect 一致

        btnInstall = new RoundButton(0, 0, 150, 42, "下载并安装", true);
        btnInstall->callback([](Fl_Widget*, void* d) { ((PageDownload*)d)->install(); }, this);
        addTo(this, btnInstall, W - 170, H - 60);

        end();
        rebuild();
        syncSearchVisible();
    }

    // 切换标签时显示/隐藏对应的搜索框
    void syncSearchVisible() {
        if (!search || !modSearch || !btnInstall) return;
        bool verTab = (dlTab == 0);
        bool showInstall = (verTab && detailIdx >= 0);

        // 版本详情页也要隐藏搜索框（否则盖住「返回」按钮）
        bool verList = (verTab && detailIdx < 0 && modDetail < 0);
        search->hidden_ = !verList;
        modSearch->hidden_ = !(verTab == false && modDetail < 0);
        btnInstall->hidden_ = !showInstall;

        // 版本页搜索框：右上角（筛选框左边留空，不重叠）
        if (verList) {
            search->resize(x() + w() - 250, y() + TOOLBAR_Y, 230, 32);
            modSearch->resize(-9999, -9999, 240, 32);
        } else {
            // 模组页搜索框：和工具栏同一坐标
            int tBtnX, tBoxX, tSrcX, tBoxW;
            modToolbar(tBtnX, tBoxX, tSrcX, tBoxW);
            modSearch->resize(tBoxX, y() + TOOLBAR_Y, tBoxW, 32);
            search->resize(-9999, -9999, 230, 32);
        }
        if (showInstall) btnInstall->resize(x() + w() - 170, y() + h() - 60, 150, 42);
        else             btnInstall->resize(-9999, -9999, 150, 42);

        redraw();
    }

    void resize(int X, int Y, int W, int H) override {
        PageBase::resize(X, Y, W, H);
        syncSearchVisible();   // 拉伸时也按当前标签重新定位
    }


    // ---------- 数据 ----------
    void snapshot(std::vector<McVer>& out) {
        std::lock_guard<std::mutex> lk(g_mcVersMutex);
        out = g_mcVers;
    }

    void rebuild() {
        std::vector<McVer> all;
        snapshot(all);

        g_dlView.clear();
        for (int i = 0; i < (int)all.size(); ++i) {
            const McVer& v = all[i];
            bool rel  = (v.type == "release");
            bool snap = (v.type == "snapshot");
            bool old  = (v.type == "old_beta" || v.type == "old_alpha");
            bool pass = false;
            switch (filterType) {
                case 0: pass = rel;  break;
                case 1: pass = snap; break;
                case 2: pass = old;  break;
                default: pass = true; break;
            }
            if (!pass) continue;
            if (!filter.empty() && v.id.find(filter) == std::string::npos) continue;
            g_dlView.push_back(i);
        }
        if (selIdx >= (int)g_dlView.size()) selIdx = -1;
        if (detailIdx >= (int)g_dlView.size()) detailIdx = -1;
        clampScroll();
        redraw();
    }

    int maxScroll() const {
        int contentH = (int)g_dlView.size() * (ITEM_H + ITEM_GAP);
        int vis = h() - ITEM_TOP() - 70;
        return contentH > vis ? contentH - vis : 0;
    }
    int ITEM_TOP() const { return 62; }

    void clampScroll() {
        int m = maxScroll();
        if (scroll < 0) scroll = 0;
        if (scroll > m) scroll = m;
    }

    void openDetail(int idx) {
        detailIdx = idx;
        selIdx = idx;
        detailLoaded = false;
        detailErr.clear();
        dId = dType = dDate = dUrl = dMcVer = dJavaVer = dLoader = "";
        dSizeAssets = 0; dLibCount = 0; dFileCount = 0;

        if (idx >= 0 && idx < (int)g_dlView.size()) {
            std::vector<McVer> all;
            snapshot(all);
            int gi = g_dlView[idx];
            if (gi >= 0 && gi < (int)all.size()) {
                dId = all[gi].id;
                dType = all[gi].type;
                dDate = all[gi].date;
                dUrl = all[gi].url;
                loadDetailMeta(all[gi]);
                detailLoaded = true;
            }
        }
        redraw();
    }

    // 从版本 JSON 里读大小 / Java 版本 / 依赖数
    void loadDetailMeta(const McVer& v) {
        dJavaVer.clear(); dLibCount = 0; dFileCount = 0; dSizeAssets = 0;

        // 先用本地已安装的 version.json（如果有），省一次网络请求
        fs::path local = GameDirPath() / L"versions"
                       / fs::u8path(v.id) / fs::u8path(v.id + ".json");
        std::string body;
        if (FileExists(local)) {
            std::ifstream f{ local, std::ios::binary };
            if (f) { std::stringstream ss; ss << f.rdbuf(); body = ss.str(); }
        } else {
            std::vector<unsigned char> d; std::string e;
            if (HttpRequest(v.url, "GET", "", "", d, e) && !d.empty())
                body.assign(d.begin(), d.end());
            else
                detailErr = e.empty() ? "无法获取版本信息" : e;
        }
        if (body.empty()) return;

        json j = json::parse(body, nullptr, false);
        if (!j.is_object()) return;

        if (j.contains("javaVersion") && j["javaVersion"].is_object()) {
            if (j["javaVersion"].contains("majorVersion"))
                dJavaVer = std::to_string(j["javaVersion"]["majorVersion"].get<int>());
        }
        if (j.contains("libraries") && j["libraries"].is_array())
            dLibCount = (int)j["libraries"].size();
        if (j.contains("downloads") && j["downloads"].is_object()
            && j["downloads"].contains("client")
            && j["downloads"]["client"].is_object()) {
            const json& c = j["downloads"]["client"];
            if (c.contains("size") && c["size"].is_number())
                dSizeAssets = c["size"].get<long long>();
            dFileCount++;
        }
        if (j.contains("assetIndex") && j["assetIndex"].is_object()
            && j["assetIndex"].contains("size"))
            dSizeAssets += j["assetIndex"]["size"].get<long long>();

        if (j.contains("inheritsFrom") && j["inheritsFrom"].is_string())
            dLoader = "基于 " + j["inheritsFrom"].get<std::string>();

        // 加载器信息
        std::string n = v.id;
        if (n.find("NeoForge") != std::string::npos) dLoader = "NeoForge";
        else if (n.find("Fabric") != std::string::npos) dLoader = "Fabric";
        else if (n.find("Quilt") != std::string::npos)  dLoader = "Quilt";
        else if (n.find("Forge") != std::string::npos)  dLoader = "Forge";
        else dLoader = "原版";
    }

    // ---------- 拉取加载器可用版本 ----------
    // 在 UI 线程同步调用（阻塞 1~2 秒），只在用户切换加载器时触发一次
    void loadLoaderVersions() {
        if (loaderSel <= 0) return;   // 原版没有版本可选
        // 同一加载器 + 同一 MC 版本只用拉一次
        if (ldrVersLoaded && ldrVersFor == loaderSel && ldrVersMc == dId) return;

        ldrVers.clear();
        ldrSel = -1;
        ldrVerScroll = 0;
        ldrVersErr.clear();
        ldrVersLoaded = true;
        ldrVersFor = loaderSel;
        ldrVersMc = dId;

        std::string err, body, url;
        switch (loaderSel) {
            case 1:   // Fabric
                url = "https://bmclapi2.bangbang93.com/fabric-meta/v2/versions/loader/" + dId;
                break;
            case 2:   // Quilt
                url = "https://meta.quiltmc.org/v3/versions/loader/" + dId;
                break;
            case 3:   // Forge
                url = "https://bmclapi2.bangbang93.com/forge/minecraft/" + dId;
                break;
            case 4:   // NeoForge
                url = "https://bmclapi2.bangbang93.com/neoforge/list/" + dId;
                break;
            case 5:   // QSL（Modrinth）
                url = "https://api.modrinth.com/v2/project/qsl/version"
                      "?game_versions=[%22" + dId + "%22]&loaders=[%22quilt%22]";
                break;
            case 6:   // Fabric API（Modrinth）
                url = "https://api.modrinth.com/v2/project/fabric-api/version"
                      "?game_versions=[%22" + dId + "%22]&loaders=[%22fabric%22]";
                break;
            default:
                return;
        }

        body = HttpGetString(url, err);
        if (body.empty()) {
            ldrVersErr = err.empty() ? "无法获取版本列表" : err;
            redraw();
            return;
        }

        json j = json::parse(body, nullptr, false);
        if (!j.is_array()) {
            ldrVersErr = "版本列表格式异常";
            redraw();
            return;
        }
        if (j.empty()) {
            ldrVersErr = "没有适配此版本的加载器";
            redraw();
            return;
        }

        for (auto& x : j) {
            std::string v;
            if (loaderSel == 5 || loaderSel == 6) {
                // Modrinth 结构
                v = x.value("version_number", x.value("name", std::string()));
            } else if (loaderSel == 1 || loaderSel == 2) {
                if (x.contains("loader") && x["loader"].is_object())
                    v = x["loader"].value("version", std::string());
            } else {
                v = x.value("version", std::string());
            }
            if (!v.empty()) ldrVers.push_back(v);
        }

        if (ldrVers.empty()) ldrVersErr = "没有适配此版本的加载器";
        // 默认选第一个（最新）
        if (!ldrVers.empty()) ldrSel = 0;
        redraw();
    }

    // 当前选中的加载器版本（空 = 让后端自动选最新）
    std::string selectedLoaderVersion() const {
        if (ldrSel >= 0 && ldrSel < (int)ldrVers.size()) return ldrVers[ldrSel];
        return "";
    }

    void install() {
        int use = (detailIdx >= 0) ? detailIdx : selIdx;
        if (use < 0 || use >= (int)g_dlView.size()) {
            g_status = L"请先选择一个版本";
            redraw();
            return;
        }
        std::string mcId, mcUrl;
        {
            std::lock_guard<std::mutex> lk(g_mcVersMutex);
            int idx = g_dlView[use];
            if (idx < 0 || idx >= (int)g_mcVers.size()) return;
            mcId  = g_mcVers[idx].id;
            mcUrl = g_mcVers[idx].url;
        }

        // 加载器（0=原版 1=Fabric 2=Quilt 3=Forge 4=NeoForge 5=QSL 6=FabricAPI）
        const int loader = loaderSel;
        const std::string lv = selectedLoaderVersion();   // 用户选的版本，空=自动最新
        std::string label = mcId;
        if (loader > 0) {
            label += " + " + std::string(kLoaderNames[loader]);
            if (!lv.empty()) label += " " + lv;
        }

        // 重置进度，让进度条从 0 开始
        g_progCur = 0;
        g_progTotal = 0;
        g_dlBytes = 0;
        g_dlTarget = U8ToW(label);
        if (g_overlay) g_overlay->showDownload(label);   // 下载模式：显示速度/字节
        g_launching = true;
        g_launching = true;
        g_cancelLaunch = false;

        std::thread([mcId, mcUrl, loader, lv]() {
            switch (loader) {
            case 0: InstallVersionThread(mcId, mcUrl); break;              // 原版
            case 1: InstallMetaLoaderThread(mcId, false, lv); break;       // Fabric
            case 2: InstallMetaLoaderThread(mcId, true,  lv); break;       // Quilt
            case 3: InstallInstallerThread(mcId, false, lv);  break;       // Forge
            case 4: InstallInstallerThread(mcId, true,  lv);  break;       // NeoForge
            case 5: InstallModrinthModThread("qsl", mcId, "quilt", mcId + "-Quilt"); break;
            case 6: InstallModrinthModThread("fabric-api", mcId, "fabric", mcId + "-Fabric"); break;
            }
        }).detach();
    }

    void refresh() override {
        if (g_mcVers.empty() && g_mcVersState.load() == 0) {
            g_status = L"正在获取版本清单…";
            std::thread([]() { FetchManifestThread(); }).detach();
        }
        rebuild();
        redraw();
    }

    // ---------- 布局辅助 ----------
    int listX() const { return x() + 20; }
    int listW() const { return w() - 40; }

    int itemY(int i) const {
        return y() + ITEM_TOP() + i * (ITEM_H + ITEM_GAP) - scroll;
    }

    static bool inRect(int rx, int ry, int rw, int rh, int mx, int my) {
        return mx >= rx && mx < rx + rw && my >= ry && my < ry + rh;
    }

    void toolbarRect(int which, int& bx, int& by, int& bw, int& bh) const {
        if (which == 0) {
            // 版本类型下拉：标签右侧，搜索框左边
            bx = x() + 20 + 2 * (TAB_W + 6) + 8;
            by = y() + TOOLBAR_Y;
            bw = 130; bh = 32;
        } else {
            // 返回按钮：右上角
            bx = x() + w() - 110;
            by = y() + TOOLBAR_Y;
            bw = 90; bh = 32;
        }
    }

    // ---------- 绘制 ----------
    // 标签页矩形：0=版本 1=模组
    void dlTabRect(int which, int& tx, int& ty, int& tw, int& th) const {
        tw = 72; th = 30;
        tx = x() + 20 + which * (tw + 6);
        ty = y() + TOOLBAR_Y + 1;
    }

    void draw() override {
        // 每帧强制同步输入框可见性（防止 FLTK 的 show() 覆盖导致筛选框被挡）
        // 每帧强制同步控件可见性 + 位置（防止 FLTK 的 show() 覆盖）
        syncSearchVisible();
        // 首次显示：清单已到但还没 rebuild 时补一次（修「要刷新才显示」）
        if (g_dlView.empty() && g_mcVersState.load() == 2 && !g_mcVers.empty()) {
            rebuild();
        }
        fill_round_rect(x(), y(), w(), h(), 0, clr_bg());

        // ---------- 顶部标签：版本 / 模组 ----------
        for (int i = 0; i < 2; ++i) {
            int tx, ty, tw, th;
            dlTabRect(i, tx, ty, tw, th);
            bool act = (dlTab == i);
            bool hot = inRect(tx, ty, tw, th, Fl::event_x(), Fl::event_y());
            fill_round_rect(tx, ty, tw, th, 6,
                            act ? clr_accent2() : (hot ? clr_card_sel() : clr_card()));
            draw_text_center(tx, ty, tw, th, i == 0 ? "版本" : "模组",
                             act ? FL_WHITE : clr_dim(), 13);
        }

        if (dlTab == 0) {
            if (detailIdx >= 0) drawDetail();
            else                drawList();
        } else {
            drawModTab();
        }

        PageBase::draw();
    }

    void drawList() {
        int bx, by, bw, bh;
        toolbarRect(0, bx, by, bw, bh);
        bool hot = inRect(bx, by, bw, bh, Fl::event_x(), Fl::event_y());
        fill_round_rect(bx, by, bw, bh, 6, hot ? clr_card_sel() : clr_card());
        static const char* tn[] = { "正式版", "快照版", "远古版", "所有版本" };
        draw_text(bx + 10, by, bw - 34, bh, tn[filterType], clr_text(), 13);
        arrowDown(bx + bw - 16, by + bh / 2, clr_dim());

        int st = g_mcVersState.load();
        if (st == 1) {
            draw_text(bx + bw + 20, by, 400, bh, "正在获取版本清单…", clr_dim(), 12);
        }

        if (g_dlView.empty()) {
            std::string msg = (st == 1) ? "正在加载…"
                            : (st == 3) ? "没有获取到版本数据（检查网络）"
                                        : "没有匹配的版本";
            draw_text(x() + 20, y() + 120, w() - 40, 60, msg, clr_dim(), 13);
            return;
        }

        std::vector<McVer> all;
        snapshot(all);

        int top = y() + ITEM_TOP();
        int bot = y() + h() - 70;
        fl_push_clip(listX(), top, listW(), bot - top);

        for (int i = 0; i < (int)g_dlView.size(); ++i) {
            int iy = itemY(i);
            if (iy + ITEM_H < top || iy > bot) continue;

            int gi = g_dlView[i];
            if (gi < 0 || gi >= (int)all.size()) continue;
            const McVer& v = all[gi];

            bool sel = (i == selIdx);
            fill_round_rect(listX(), iy + 3, listW(), ITEM_H - 6, 8,
                            sel ? clr_card_sel() : clr_card());
            if (sel) fill_round_rect(listX() + 2, iy + 11, 3, ITEM_H - 22, 0, clr_accent2());

            draw_text(listX() + 18, iy + 3, listW() - 260, ITEM_H - 6,
                      v.id, sel ? clr_text() : clr_dim(), 15);
            draw_text_right(listX() + listW() - 200, iy + 3, 180, ITEM_H - 6,
                            v.date, clr_dim(), 12);
        }
        fl_pop_clip();

        // 滚动条
        if (maxScroll() > 0) {
            int vis = h() - ITEM_TOP() - 70;
            int contentH = (int)g_dlView.size() * (ITEM_H + ITEM_GAP);
            int barH = vis * vis / contentH;
            if (barH < 30) barH = 30;
            int barY = top + (vis - barH) * scroll / maxScroll();
            fill_round_rect(x() + w() - 12, barY, 6, barH, 3, clr_card_sel());
        }

        // ---------- 展开的筛选下拉（最后画，盖在列表上面） ----------
        if (filterDropOpen) {
            int bx2, by2, bw2, bh2;
            toolbarRect(0, bx2, by2, bw2, bh2);
            int ih = 30;
            int ly = by2 + bh2;
            static const char* tn2[] = { "正式版", "快照版", "远古版", "所有版本" };
            fill_round_rect(bx2, ly, bw2, 4 * ih + 8, 8, clr_card_sel());
            stroke_round_rect(bx2, ly, bw2, 4 * ih + 8, 8, clr_accent());
            for (int k = 0; k < 4; ++k) {
                int ky = ly + 4 + k * ih;
                bool hotK = inRect(bx2 + 2, ky, bw2 - 4, ih - 2, Fl::event_x(), Fl::event_y());
                if (hotK)                 fill_round_rect(bx2 + 2, ky, bw2 - 4, ih - 2, 5, clr_accent2());
                else if (k == filterType) fill_round_rect(bx2 + 2, ky, bw2 - 4, ih - 2, 5, clr_card());
                Fl_Color tc = hotK ? FL_WHITE : (k == filterType ? clr_ok() : clr_text());
                draw_text(bx2 + 12, ky, bw2 - 24, ih - 2, tn2[k], tc, 12);
            }
        }

        if (!g_status.empty())
            draw_text(x() + 20, y() + h() - 40, w() - 200, 24, g_status, clr_dim(), 12);
    }

    void drawDetail() {
        int bx, by, bw, bh;
        toolbarRect(1, bx, by, bw, bh);
        bool hotBack = inRect(bx, by, bw, bh, Fl::event_x(), Fl::event_y());
        fill_round_rect(bx, by, bw, bh, 6, hotBack ? clr_card_sel() : clr_card());
        draw_text_center(bx, by, bw, bh, "返回", clr_text(), 13);

        int cx = x() + 20, cw = w() - 40;

        // 标题
        draw_text(cx, y() + 66, cw - 120, 40, dId, clr_text(), 24);

        // 类型标签
        std::string tn = (dType == "release") ? "正式版"
                       : (dType == "snapshot") ? "快照版"
                       : (dType == "old_beta") ? "旧 Beta"
                       : (dType == "old_alpha") ? "旧 Alpha" : dType;
        int tagW = 62;
        fill_round_rect(cx, y() + 112, tagW, 22, 5,
                        (dType == "release") ? clr_ok() : clr_card_sel());
        draw_text_center(cx, y() + 112, tagW, 22, tn, FL_WHITE, 11);

        // 信息卡片
        int iy = y() + 150;
        int rowH = 34;
        struct KV { const char* k; std::string v; Fl_Color c; };
        std::vector<KV> rows;
        rows.push_back({ "发布日期", dDate, clr_text() });
        rows.push_back({ "加载器",   dLoader, clr_text() });

        if (!dJavaVer.empty())
            rows.push_back({ "需要 Java", dJavaVer + " 及以上", clr_text() });
        if (dLibCount > 0)
            rows.push_back({ "依赖库", std::to_string(dLibCount) + " 个", clr_text() });
        if (dSizeAssets > 0) {
            double mb = dSizeAssets / 1048576.0;
            char buf[64];
            snprintf(buf, sizeof(buf), "%.1f MB", mb);
            rows.push_back({ "客户端大小", buf, clr_text() });
        }

        for (size_t k = 0; k < rows.size(); ++k) {
            draw_text(cx, iy, 110, rowH, rows[k].k, clr_dim(), 13);
            draw_text(cx + 110, iy, cw - 110, rowH, rows[k].v, rows[k].c, 13);
            iy += rowH;
        }

        if (!detailErr.empty()) {
            draw_text(cx, iy + 10, cw, 30, detailErr, clr_err(), 12);
            iy += 40;
        }

        if (!detailLoaded && detailErr.empty())
            draw_text(cx, iy + 10, cw, 30, "正在读取版本信息…", clr_dim(), 12);

        // ---------- 加载器选择 ----------
        {
            int lx = cx, ly = y() + h() - 168;
            draw_text(lx, ly, 90, 30, "加载器", clr_dim(), 13);

            int bx2 = lx + 76, by2 = ly + 2, bw2 = 200, bh2 = 28;
            bool hotL = inRect(bx2, by2, bw2, bh2, Fl::event_x(), Fl::event_y());
            fill_round_rect(bx2, by2, bw2, bh2, 6, hotL ? clr_card_sel() : clr_card());
            draw_text(bx2 + 10, by2, bw2 - 34, bh2, kLoaderNames[loaderSel], clr_text(), 13);
            arrowSmall(bx2 + bw2 - 17, by2 + 14, clr_dim());

            // ---------- 加载器版本下拉 ----------
            int vx = bx2 + bw2 + 10, vw = 180;
            bool hasVersion = (loaderSel >= 1 && loaderSel <= 4);   // 只有真加载器有版本可选
            bool hasPre = (loaderSel == 5 || loaderSel == 6);        // QSL / FabricAPI 也有版本

            if (hasVersion || hasPre) {
                if (!ldrVersLoaded || ldrVersFor != loaderSel || ldrVersMc != dId) {
                    // 还没拉过：显示「加载中」并在下面立即触发
                    fill_round_rect(vx, by2, vw, bh2, 6, clr_card());
                    draw_text(vx + 10, by2, vw - 34, bh2, "点此加载版本…", clr_dim(), 12);
                    loadLoaderVersions();
                }

                std::string vtext;
                if (ldrSel >= 0 && ldrSel < (int)ldrVers.size()) vtext = ldrVers[ldrSel];
                else if (!ldrVersErr.empty())                   vtext = ldrVersErr;
                else                                            vtext = "（无可用版本）";

                bool hotV = inRect(vx, by2, vw, bh2, Fl::event_x(), Fl::event_y());
                fill_round_rect(vx, by2, vw, bh2, 6, hotV ? clr_card_sel() : clr_card());
                draw_text(vx + 10, by2, vw - 34, bh2, vtext,
                          ldrVers.empty() ? clr_err() : clr_text(), 12);
                arrowSmall(vx + vw - 17, by2 + 14, clr_dim());

                // 版本下拉展开
                if (ldrVerOpen && !ldrVers.empty()) {
                    int showN = (int)ldrVers.size() > 8 ? 8 : (int)ldrVers.size();
                    int lh = showN * 26 + 8;
                    int ly3 = by2 + bh2 + 2;
                    if (ly3 + lh > y() + h() - 10) ly3 = by2 - lh - 2;
                    fill_round_rect(vx, ly3, vw, lh, 8, clr_card_sel());
                    stroke_round_rect(vx, ly3, vw, lh, 8, clr_accent());
                    for (int k = 0; k < showN; ++k) {
                        int i2 = k + ldrVerScroll;
                        if (i2 >= (int)ldrVers.size()) break;
                        int ky2 = ly3 + 4 + k * 26;
                        bool hotK2 = inRect(vx + 2, ky2, vw - 4, 24, Fl::event_x(), Fl::event_y());
                        if (hotK2)               fill_round_rect(vx + 2, ky2, vw - 4, 24, 5, clr_accent2());
                        else if (i2 == ldrSel)   fill_round_rect(vx + 2, ky2, vw - 4, 24, 5, clr_card());
                        Fl_Color tc2 = hotK2 ? FL_WHITE : (i2 == ldrSel ? clr_ok() : clr_text());
                        draw_text(vx + 12, ky2, vw - 24, 24, ldrVers[i2], tc2, 12);
                    }
                }
            }

            // 当前选择的说明
            std::string hint;
            if (loaderSel == 0)      hint = "只装原版，不加任何加载器";
            else if (loaderSel == 1) hint = "Fabric 加载器";
            else if (loaderSel == 2) hint = "Quilt 加载器";
            else if (loaderSel == 3) hint = "Forge 加载器";
            else if (loaderSel == 4) hint = "NeoForge 加载器";
            else if (loaderSel == 5) hint = "QSL 前置库（装到 Quilt 实例）";
            else                     hint = "Fabric API 前置库（装到 Fabric 实例）";
            draw_text(vx + vw + 14, by2, cw - bw2 - vw - 108, bh2, hint, clr_dim(), 11);

            // 展开的下拉列表
            if (loaderDropOpen) {
                int listH = 7 * 28 + 8;
                int ly2 = by2 + bh2 + 2;
                if (ly2 + listH > y() + h() - 10) ly2 = by2 - listH - 2;
                fill_round_rect(bx2, ly2, bw2, listH, 8, clr_card_sel());
                stroke_round_rect(bx2, ly2, bw2, listH, 8, clr_accent());
                for (int k = 0; k < 7; ++k) {
                    int ky = ly2 + 4 + k * 28;
                    bool hotK = inRect(bx2 + 2, ky, bw2 - 4, 26, Fl::event_x(), Fl::event_y());
                    if (hotK)          fill_round_rect(bx2 + 2, ky, bw2 - 4, 26, 5, clr_accent2());
                    else if (k == loaderSel) fill_round_rect(bx2 + 2, ky, bw2 - 4, 26, 5, clr_card());
                    Fl_Color tc = hotK ? FL_WHITE : (k == loaderSel ? clr_ok() : clr_text());
                    draw_text(bx2 + 12, ky, bw2 - 24, 26, kLoaderNames[k], tc, 12);
                }
            }
        }

        draw_text(cx, y() + h() - 108, cw, 24,
                  "点右下角「下载并安装」把这个版本安装到本地", clr_dim(), 12);

        if (!g_status.empty())
            draw_text(cx, y() + h() - 40, cw - 200, 24, g_status, clr_dim(), 12);
    }
    // ==================== 模组页 ====================
    // 进入页面就按「当前实例的 MC 版本 + 加载器」自动拉热门模组

    void enterModTab() {
        if (!modAutoLoaded) {
            modAutoLoaded = true;
            doModSearch("");
        }
    }

    // 左侧分类栏
    static const int CAT_W = 148;

    // ===== 模组页工具栏统一坐标（绘制 / 点击 / 控件三处共用） =====
    void modToolbar(int& btnX, int& boxX, int& srcX, int& boxW) const {
        // 工具栏要避开顶部「版本/模组」标签（x+20 .. x+176）
        int cx = x() + 20 + 2 * (TAB_W + 6) + 8;   // = x + 184
        btnX = cx;                 // 搜索按钮
        boxX = cx + 84;            // 搜索框
        boxW = 240;
        srcX = boxX + boxW + 10;   // 来源切换
    }
    static const char* kCatNames[5];

    void catRect(int i, int& rx, int& ry, int& rw, int& rh) const {
        rw = CAT_W - 4;
        rh = 44;
        rx = x() + 16;
        ry = y() + 76 + i * (rh + 6);
    }

    void drawModTab() {
        bool inDetail = (modDetail >= 0 && modDetail < (int)g_modHits.size());

        // ---------- 左侧分类栏（详情页隐藏） ----------
        if (!inDetail) {
            draw_text(x() + 16, y() + 52, CAT_W, 20, "分类", clr_dim(), 11);
            for (int i = 0; i < 5; ++i) {
                int rx, ry, rw, rh;
                catRect(i, rx, ry, rw, rh);
                bool act = (g_modCat == i);
                bool hot = inRect(rx, ry, rw, rh, Fl::event_x(), Fl::event_y());
                if (act) {
                    fill_round_rect(rx, ry, rw, rh, 8, clr_accent2());
                    fill_round_rect(rx, ry + 8, 4, rh - 16, 0, FL_WHITE);
                } else {
                    fill_round_rect(rx, ry, rw, rh, 8, hot ? clr_card_sel() : clr_card());
                }
                draw_text(rx + 18, ry, rw - 26, rh, kCatNames[i],
                          act ? FL_WHITE : clr_text(), act ? 15 : 14);
            }
        }

        // 内容区：详情页占满，列表页在分类栏右侧
        int cx = inDetail ? (x() + 24) : (x() + CAT_W + 16);
        int cw = inDetail ? (w() - 48) : (w() - CAT_W - 36);


        // ---------- 搜索按钮 / 搜索框 / 来源切换（统一坐标）----------
        int sy = y() + TOOLBAR_Y;
        int tBtnX, tBoxX, tSrcX, tBoxW;
        modToolbar(tBtnX, tBoxX, tSrcX, tBoxW);

        // 搜索按钮
        {
            int bbw = 76;
            bool hotB = inRect(tBtnX, sy, bbw, 32, Fl::event_x(), Fl::event_y());
            fill_round_rect(tBtnX, sy, bbw, 32, 6, hotB ? clr_accent() : clr_accent2());
            draw_text_center(tBtnX, sy, bbw, 32, "搜索", FL_WHITE, 13);
        }

        // 搜索框由 modSearch 控件绘制，这里只画底色（控件在其上）
        fill_round_rect(tBoxX, sy, tBoxW, 32, 6, clr_inputbg());

        // 来源切换
        {
            const char* srcName = (g_modSource == 1) ? "CurseForge" : "Modrinth";
            bool hotS = inRect(tSrcX, sy, 104, 32, Fl::event_x(), Fl::event_y());
            fill_round_rect(tSrcX, sy, 104, 32, 6, hotS ? clr_card_sel() : clr_card());
            draw_text(tSrcX + 8, sy, 90, 32, srcName, clr_dim(), 12);
        }
        // ---------- 模组详情 ----------
        if (modDetail >= 0 && modDetail < (int)g_modHits.size()) {
            drawModDetail(cx, cw);
            return;
        }

        // ---------- 当前实例信息 ----------
        std::string mc = CurrentMcVersion();
        std::string ldr = CurrentLoaderTag();
        std::string inst = (g_selVersion >= 0 && g_selVersion < (int)g_versions.size())
                         ? g_versions[g_selVersion] : "（未选择实例）";
        int top = y() + 62, bot = y() + h() - 30;

        // ---------- 顶部：当前分类 + 实例信息 + 返回 ----------
        draw_text(cx, top, 200, 22, kCatNames[g_modCat], clr_text(), 14);
        draw_text(cx + 90, top, cw - 260, 22,
                  "·  " + inst + "  ·  MC " + mc + " / " + ldr, clr_dim(), 11);

        int listTop = top + 26;
        int st = g_modState.load();

        if (g_modHits.empty()) {
            std::string msg = (st == 1) ? "正在获取模组列表…"
                            : (st == 3) ? "没有找到模组（检查网络，或在搜索框输入关键词）"
                                        : "点「搜索」获取模组";
            draw_text(cx, listTop + 30, cw, 50, msg, clr_dim(), 13);
            return;
        }

        // ---------- 模组列表（可滚动） ----------
        const int ROW = 66;
        int contentH = (int)g_modHits.size() * ROW;

        fl_push_clip(cx, listTop, cw, bot - listTop);
        for (int i = 0; i < (int)g_modHits.size(); ++i) {
            int iy = listTop + i * ROW - modScroll;
            if (iy + ROW < listTop || iy > bot) continue;

            bool sel = (i == modSel);
            fill_round_rect(cx, iy + 3, cw, ROW - 8, 8,
                            sel ? clr_card_sel() : clr_card());
            if (sel) fill_round_rect(cx + 2, iy + 11, 3, ROW - 26, 0, clr_accent2());

            const ModHit& m = g_modHits[i];

            // 标题（限制宽度，超出由 draw_text 自动加 …）
            int textW = cw - 220;
            if (textW < 80) textW = 80;
            draw_text(cx + 16, iy + 6, textW, 24, m.title, clr_text(), 15);

            // 简介（截断，不越界）
            std::string desc = m.desc;
            if (desc.size() > 84) desc = desc.substr(0, 82) + "…";
            draw_text(cx + 16, iy + 30, textW, 22, desc, clr_dim(), 11);

            // 下载量
            draw_text_right(cx, iy + 6, cw - 24, 24,
                            formatDownloads(m.downloads), clr_dim(), 11);

            // 选择按钮
            int dbx = cx + cw - 90, dby = iy + 14, dbw = 74, dbh = 30;
            bool hotD = inRect(dbx, dby, dbw, dbh, Fl::event_x(), Fl::event_y());
            fill_round_rect(dbx, dby, dbw, dbh, 6,
                            hotD ? clr_accent() : clr_accent2());
            draw_text_center(dbx, dby, dbw, dbh, "选择", FL_WHITE, 12);
        }
        fl_pop_clip();

        drawScrollbar(cx, cw, listTop, bot, contentH, modScroll);

        if (!g_status.empty())
            draw_text(cx, y() + h() - 26, cw - 160, 22, g_status, clr_dim(), 11);
    }

    void drawModDetail(int cx, int cw) {
        const ModHit& m = g_modHits[modDetail];

        // 返回
        int bbx = x() + w() - 110, bby = y() + TOOLBAR_Y, bbw = 90, bbh = 32;
        bool hotBack = inRect(bbx, bby, bbw, bbh, Fl::event_x(), Fl::event_y());
        fill_round_rect(bbx, bby, bbw, bbh, 6, hotBack ? clr_card_sel() : clr_card());
        draw_text_center(bbx, bby, bbw, bbh, "返回", clr_text(), 13);

        // 标题
        draw_text(cx, y() + 62, cw - 130, 34, m.title, clr_text(), 20);

        // 简介（限制两行高度）
        std::string desc = m.desc;
        if (desc.size() > 120) desc = desc.substr(0, 118) + "…";
        draw_text(cx, y() + 98, cw - 20, 44, desc, clr_dim(), 12);

        // 元信息
        std::string mc = CurrentMcVersion();
        std::string ldr = CurrentLoaderTag();
        draw_text(cx, y() + 146, cw, 22,
                  "下载量 " + formatDownloads(m.downloads) +
                  "    ·    当前实例 MC " + mc + " / " + ldr,
                  clr_dim(), 12);

        // ---------- 版本列表 ----------
        int vtop = y() + 180;
        draw_text(cx, vtop, 240, 24,
                  "可选版本 (" + std::to_string(g_modVerNames.size()) + ")",
                  clr_dim(), 13);

        int vst = g_modVerState.load();
        int listTop = vtop + 28;
        int listBot = y() + h() - 80;

        if (vst == 1) {
            draw_text(cx, listTop + 10, cw, 30, "正在获取版本列表…", clr_dim(), 12);
        } else if (g_modVerNames.empty()) {
            draw_text(cx, listTop + 10, cw, 50,
                      "没有适配当前版本（" + mc + " / " + ldr + "）的模组文件",
                      clr_err(), 12);
        } else {
            const int rowH = 36;
            int contentH = (int)g_modVerNames.size() * rowH;

            fl_push_clip(cx, listTop, cw, listBot - listTop);
            for (int i = 0; i < (int)g_modVerNames.size(); ++i) {
                int ry = listTop + i * rowH - modVerScroll;
                if (ry + rowH < listTop || ry > listBot) continue;

                bool sel = (i == modVerSel);
                bool hot = inRect(cx, ry, cw, rowH - 4, Fl::event_x(), Fl::event_y());

                fill_round_rect(cx, ry, cw, rowH - 4, 6,
                                sel ? clr_card_sel() : (hot ? clr_card_sel() : clr_card()));
                if (sel) fill_round_rect(cx + 2, ry + 6, 3, rowH - 16, 0, clr_accent2());

                // 版本号（限制宽度）
                std::string vn = g_modVerNames[i];
                if (vn.size() > 90) vn = vn.substr(0, 88) + "…";
                draw_text(cx + 16, ry, cw - 32, rowH - 4, vn,
                          sel ? clr_text() : clr_dim(), 12);
            }
            fl_pop_clip();

            drawScrollbar(cx, cw, listTop, listBot, contentH, modVerScroll);
        }

        // ---------- 下载按钮 ----------
        // 版本列表后台加载完成后，默认选中第一个（否则按钮一直是灰的）
        if (modVerSel < 0 && !g_modVerNames.empty()) modVerSel = 0;
        int dbx = x() + w() - 170, dby = y() + h() - 60, dbw = 150, dbh = 42;
        bool canDl = (modVerSel >= 0 && modVerSel < (int)g_modVerFiles.size());
        bool hotD = canDl && inRect(dbx, dby, dbw, dbh, Fl::event_x(), Fl::event_y());
        fill_round_rect(dbx, dby, dbw, dbh, 8,
                        canDl ? (hotD ? clr_accent() : clr_accent2()) : clr_card());
        draw_text_center(dbx, dby, dbw, dbh, "下载并安装",
                         canDl ? FL_WHITE : clr_dim(), 14);

        if (!g_status.empty())
            draw_text(cx, y() + h() - 40, cw - 200, 24, g_status, clr_dim(), 12);
    }

    // 通用滚动条
    void drawScrollbar(int cx, int cw, int top, int bot, int contentH, int scroll) {
        int vis = bot - top;
        if (contentH <= vis || vis < 10) return;
        int barH = vis * vis / contentH;
        if (barH < 30) barH = 30;
        if (barH > vis) barH = vis;
        int maxS = contentH - vis;
        if (maxS < 1) return;
        int barY = top + (vis - barH) * scroll / maxS;
        if (barY < top) barY = top;
        if (barY + barH > bot) barY = bot - barH;
        fill_round_rect(x() + w() - 10, barY, 6, barH, 3, clr_card_sel());
    }

    // 触发模组搜索（UI 线程同步，1~2 秒）
    void doModSearch(const std::string& q) {
        std::string query = q;
        if (query.empty() && modSearch) query = modSearch->value();
        modQuery = query;

        g_modState = 1;
        { std::lock_guard<std::mutex> lk(g_modHitsMutex); g_modHits.clear(); }
        modSel = -1;
        modDetail = -1;
        modScroll = 0;
        modErr.clear();
        modVerSel = -1;
        redraw();

        // ★ 后台搜索（不阻塞 UI，切到模组页不卡）
        int gen = ++g_modGen;
        std::thread([query, gen]() { FetchModSearchThread(query, gen); }).detach();
    }

    // 打开模组详情并拉取版本
    void openModDetail(int idx) {
        if (idx < 0 || idx >= (int)g_modHits.size()) return;
        modDetail = idx;
        modVerSel = -1;
        modVerScroll = 0;
        modDetailSlug = g_modHits[idx].slug;
        { std::lock_guard<std::mutex> lk(g_modVerMutex); g_modVerNames.clear(); g_modVerFiles.clear(); }

        // ★ 后台拉取版本列表（不阻塞 UI，避免点开就卡）
        int gen = ++g_modVerGen;
        std::string slug = modDetailSlug;
        std::thread([slug, gen]() { FetchModVersThread(slug, gen); }).detach();
        redraw();
    }

    // 下载选中的模组版本
    void installModVersion() {
        if (modVerSel < 0 || modVerSel >= (int)g_modVerFiles.size()) return;
        const std::string& packed = g_modVerFiles[modVerSel];
        size_t nl = packed.find('\n');
        if (nl == std::string::npos) return;
        std::string url  = packed.substr(0, nl);
        std::string fnam = packed.substr(nl + 1);
        if (url.empty() || fnam.empty()) return;

        g_modDetailTitle = g_modHits[modDetail].title;
        std::thread([url, fnam]() { InstallModVersionThread(url, fnam); }).detach();
        g_status = L"正在下载：" + U8ToW(fnam);
        redraw();
    }

    static std::string formatDownloads(long long n) {
        if (n >= 100000000) return std::to_string(n / 100000000) + "亿+";
        if (n >= 10000) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.1f万", n / 10000.0);
            return buf;
        }
        return std::to_string(n);
    }



    static void arrowSmall(int cx, int cy, Fl_Color c) {
        fl_color(c);
        fl_begin_polygon();
        fl_vertex(cx - 4, cy - 2);
        fl_vertex(cx + 4, cy - 2);
        fl_vertex(cx,     cy + 3);
        fl_end_polygon();
    }

    static void arrowDown(int cx, int cy, Fl_Color c) {
        fl_color(c);
        fl_begin_polygon();
        fl_vertex(cx - 5, cy - 3);
        fl_vertex(cx + 5, cy - 3);
        fl_vertex(cx,     cy + 4);
        fl_end_polygon();
    }

    // ---------- 事件 ----------
    int handle(int e) override {
        // ★ 先给子控件（输入框）机会，否则它们永远收不到 PUSH，无法获得焦点
        if (e == FL_PUSH || e == FL_DRAG || e == FL_RELEASE ||
            e == FL_KEYBOARD || e == FL_KEYUP || e == FL_FOCUS || e == FL_UNFOCUS) {
            if (PageBase::handle(e)) return 1;
        }

        int mx = Fl::event_x(), my = Fl::event_y();

        // 移动类事件不改状态（只在 PUSH 里处理点击）
        if (e != FL_PUSH || Fl::event_button() != FL_LEFT_MOUSE) {
            if (e == FL_MOVE || e == FL_ENTER || e == FL_LEAVE) { redraw(); return 1; }
            if (e == FL_MOUSEWHEEL) {
                if (dlTab == 1 && modDetail < 0) {
                    int maxS = (int)g_modHits.size() * 66 - (h() - 92);
                    if (maxS < 0) maxS = 0;
                    modScroll += Fl::event_dy() * 40;
                    if (modScroll < 0) modScroll = 0;
                    if (modScroll > maxS) modScroll = maxS;
                    redraw();
                    return 1;
                }
            }
            return PageBase::handle(e);
        }

        // ========== 顶部标签：版本 / 模组 ==========
        for (int i = 0; i < 2; ++i) {
            int tx, ty, tw, th;
            dlTabRect(i, tx, ty, tw, th);
            if (inRect(tx, ty, tw, th, mx, my)) {
                dlTab = i;
                syncSearchVisible();
                if (dlTab == 1) enterModTab();   // 首次进模组页自动拉热门列表
                redraw();
                return 1;
            }
        }

        // ========== 模组页点击 ==========
        if (dlTab == 1) {

            // ---------- 左侧分类栏 ----------
            for (int k = 0; k < 5; ++k) {
                int rx, ry, rw, rh;
                catRect(k, rx, ry, rw, rh);
                if (inRect(rx, ry, rw, rh, mx, my)) {
                    if (g_modCat != k) {
                        g_modCat = k;
                        doModSearch(modSearch ? modSearch->value() : "");
                    }
                    return 1;
                }
            }

            int cx = x() + CAT_W + 16, cw = w() - CAT_W - 36;

            // 模组详情里的返回
            if (modDetail >= 0) {
                int bbx = x() + w() - 110, bby = y() + TOOLBAR_Y, bbw = 90, bbh = 32;
                if (inRect(bbx, bby, bbw, bbh, mx, my)) {
                    modDetail = -1;
                    modVerSel = -1;
                    redraw();
                    return 1;
                }
                // 版本选择
                int vtop = y() + 180 + 30;
                int rowH = 34;
                int maxRows = (y() + h() - vtop - 90) / rowH;
                if (maxRows < 1) maxRows = 1;
                for (int i = 0; i < (int)g_modVerNames.size() && i < maxRows; ++i) {
                    int ry = vtop + i * rowH;
                    if (inRect(cx, ry, cw, rowH - 4, mx, my)) {
                        modVerSel = i;
                        redraw();
                        return 1;
                    }
                }
                // 下载按钮
                int dbx = x() + w() - 170, dby = y() + h() - 60, dbw = 150, dbh = 42;
                if (inRect(dbx, dby, dbw, dbh, mx, my)) {
                    installModVersion();
                    return 1;
                }
                return 1;
            }

            // 搜索按钮 / 来源切换（统一坐标）
            int sy = y() + TOOLBAR_Y;
            int tBtnX, tBoxX, tSrcX, tBoxW;
            modToolbar(tBtnX, tBoxX, tSrcX, tBoxW);
            if (inRect(tBtnX, sy, 76, 32, mx, my)) {
                doModSearch("");
                return 1;
            }
            if (inRect(tSrcX, sy, 104, 32, mx, my)) {
                g_modSource = (g_modSource == 0) ? 1 : 0;
                doModSearch(modSearch ? modSearch->value() : "");
                return 1;
            }

            // ---------- 模组卡片 / 选择按钮 ----------
            int top = y() + 62;
            int listTop = top + 26;   // 与 drawModTab 保持一致
            for (int i = 0; i < (int)g_modHits.size(); ++i) {
                int iy = listTop + i * 66 - modScroll;
                if (iy + 60 < listTop) continue;
                if (iy > y() + h() - 30) break;

                int dbx = cx + cw - 90, dby = iy + 14, dbw = 74, dbh = 30;
                if (inRect(dbx, dby, dbw, dbh, mx, my)) {
                    modSel = i;
                    openModDetail(i);
                    return 1;
                }
                if (inRect(cx, iy + 3, cw, 58, mx, my)) {
                    modSel = i;
                    openModDetail(i);
                    return 1;
                }
            }
            return 1;
        }
        // ========== 版本页点击 ==========
        if (dlTab == 0) {
            // MC 版本详情页：返回按钮
            if (detailIdx >= 0) {
                int bx, by, bw, bh;
                toolbarRect(1, bx, by, bw, bh);
                if (inRect(bx, by, bw, bh, mx, my)) {
                    detailIdx = -1;
                    syncSearchVisible();
                    redraw();
                    return 1;
                }
            }

            // ========== 详情页的加载器 / 版本下拉（优先处理）==========
            if (detailIdx >= 0) {
                int cx = x() + 20, cw = w() - 40;
                int lx = cx, ly = y() + h() - 168;
                int bx2 = lx + 76, by2 = ly + 2, bw2 = 200, bh2 = 28;
                int vx = bx2 + bw2 + 10, vw = 180;

                // ---------- 版本下拉已展开 ----------
                if (ldrVerOpen) {
                    int showN = (int)ldrVers.size() > 8 ? 8 : (int)ldrVers.size();
                    int lh = showN * 26 + 8;
                    int ly3 = by2 + bh2 + 2;
                    if (ly3 + lh > y() + h() - 10) ly3 = by2 - lh - 2;
                    if (mx >= vx && mx < vx + vw && my >= ly3 && my < ly3 + lh) {
                        int k = (my - ly3 - 4) / 26;
                        int i2 = k + ldrVerScroll;
                        if (i2 >= 0 && i2 < (int)ldrVers.size()) {
                            ldrSel = i2;
                            ldrVerOpen = false;
                            redraw();
                        }
                        return 1;
                    }
                    ldrVerOpen = false;
                    redraw();
                    return 1;
                }

                // ---------- 加载器下拉已展开 ----------
                if (loaderDropOpen) {
                    int listH = 7 * 28 + 8;
                    int ly2 = by2 + bh2 + 2;
                    if (ly2 + listH > y() + h() - 10) ly2 = by2 - listH - 2;
                    if (mx >= bx2 && mx < bx2 + bw2 && my >= ly2 && my < ly2 + listH) {
                        int k = (my - ly2 - 4) / 28;
                        if (k >= 0 && k < 7) {
                            if (loaderSel != k) {
                                loaderSel = k;
                                ldrVersLoaded = false;   // 换了加载器要重新拉版本
                                ldrVers.clear();
                                ldrSel = -1;
                                ldrVersErr.clear();
                            }
                            loaderDropOpen = false;
                            redraw();
                        }
                        return 1;
                    }
                    loaderDropOpen = false;
                    redraw();
                    return 1;
                }

                // ---------- 点加载器框 → 展开 ----------
                if (inRect(bx2, by2, bw2, bh2, mx, my)) {
                    loaderDropOpen = true;
                    redraw();
                    return 1;
                }

                // ---------- 点版本框 → 展开 ----------
                if (loaderSel >= 1) {
                    if (inRect(vx, by2, vw, bh2, mx, my)) {
                        if (ldrVers.empty()) loadLoaderVersions();   // 首次点先拉取
                        ldrVerOpen = !ldrVerOpen;
                        ldrVerScroll = 0;
                        redraw();
                        return 1;
                    }
                }
            }

            // 返回按钮（详情页）
            if (detailIdx >= 0) {
                int bx, by, bw, bh;
                toolbarRect(1, bx, by, bw, bh);
                if (inRect(bx, by, bw, bh, mx, my)) {
                    detailIdx = -1;
                    redraw();
                    return 1;
                }
            }

            // ---------- 版本类型下拉 ----------
            int bx, by, bw, bh;
            toolbarRect(0, bx, by, bw, bh);

            // 展开时：先判断点在哪一项
            if (filterDropOpen) {
                int ih = 30;
                if (mx >= bx && mx < bx + bw && my >= by + bh && my < by + bh + 4 * ih) {
                    int k = (my - by - bh) / ih;
                    if (k >= 0 && k < 4) {
                        filterType = k;
                        filterDropOpen = false;
                        rebuild();
                        return 1;
                    }
                }
                filterDropOpen = false;
                redraw();
                return 1;
            }

            if (inRect(bx, by, bw, bh, mx, my)) {
                filterDropOpen = true;
                redraw();
                return 1;
            }

            // 列表项
            if (detailIdx < 0 && my >= y() + ITEM_TOP() && my < y() + h() - 70) {
                int i = (my - y() - ITEM_TOP() + scroll) / (ITEM_H + ITEM_GAP);
                if (i >= 0 && i < (int)g_dlView.size()) {
                    openDetail(i);
                    return 1;
                }
            }
            return 1;
        }

    }
};


const char* PageDownload::kCatNames[5] = {
    "Mod", "整合包", "数据包", "资源包", "光影包"
};

const char* PageDownload::kLoaderNames[7] = {
    "原版", "Fabric", "Quilt", "Forge", "NeoForge", "QSL", "Fabric API"
};

// ============================ 实例页 ============================
class PageInstance : public PageBase {
public:
    RowList* list = nullptr;
    RoundButton* btnRefresh = nullptr;
    RoundButton* btnLaunch = nullptr;

    PageInstance(int W, int H) : PageBase(W, H) {
        list = new RowList(0, 0, W - 40, H - 150);
        list->setRowHeight(48);
        list->setDrawer([this](RowList* self, int idx, int rx, int ry, int rw, int rh, bool sel) {
            fill_round_rect(rx, ry + 4, rw, rh - 8, 8, sel ? clr_card_sel() : clr_card());
            if (sel) fill_round_rect(rx + 2, ry + 10, 3, rh - 20, 0, clr_accent2());
            draw_text(rx + 18, ry + 4, rw - 36, rh - 8, self->label(idx),
                      sel ? clr_text() : clr_dim(), 15);
        });
        list->callback([](Fl_Widget* w, void*) {
            int i = ((RowList*)w)->value();
            if (i >= 0 && i < (int)g_versions.size()) {
                g_selVersion = i;
                g_status = L"已切换到实例：" + U8ToW(g_versions[i]);
            }
        });
        addTo(this, list, 20, 56);

        btnRefresh = new RoundButton(0, 0, 100, 32, "刷新");
        btnRefresh->callback([](Fl_Widget*, void* d) {
    ReloadVersions();
            ((PageInstance*)d)->refresh();
            g_status = L"实例列表已刷新";
        }, this);
        addTo(this, btnRefresh, W - 120, 14);

        btnLaunch = new RoundButton(0, 0, 150, 42, "启动此实例", true);
        btnLaunch->callback([](Fl_Widget*, void*) { launchSelected(); });
        addTo(this, btnLaunch, W - 170, H - 60);

        end();
        refresh();
    }

    void resize(int X, int Y, int W, int H) override {
        PageBase::resize(X, Y, W, H);
        if (list)       list->resize(X + 20, Y + 56, W - 40, H - 150);
        if (btnRefresh) btnRefresh->resize(X + W - 120, Y + 14, 100, 32);
        if (btnLaunch)  btnLaunch->resize(X + W - 170, Y + H - 60, 150, 42);
    }

    void refresh() override {
        if (!list) return;
        list->clear();
        for (auto& v : g_versions) list->add(v);
        list->value(g_selVersion);
        redraw();
    }

    void draw() override {
        fill_round_rect(x(), y(), w(), h(), 0, clr_bg());
        draw_text(x() + 20, y() + 16, 300, 28,
                  "实例 (" + std::to_string(g_versions.size()) + ")", clr_text(), 15);
        if (g_versions.empty())
            draw_text(x() + 20, y() + 90, 600, 24,
                      "没有找到实例，请在「设置」里检查游戏目录", clr_err(), 13);
        if (!g_status.empty())
            draw_text(x() + 20, y() + h() - 40, w() - 200, 24, g_status, clr_dim(), 12);
        PageBase::draw();
    }
};

// ============================ 用户页 ============================
class ModeBtn : public Fl_Button {
public:
    int idx_;
    ModeBtn(int X, int Y, int W, int H, const char* L, int i)
        : Fl_Button(X, Y, W, H, L), idx_(i) { box(FL_NO_BOX); }
    void draw() override {
        bool act = (idx_ == g_userMode);
        bool hot = (Fl::belowmouse() == this);
        fill_round_rect(x(), y(), w(), h(), 8, (act || hot) ? clr_card_sel() : clr_card());
        draw_text_center(x(), y(), w(), h(), label() ? label() : "",
                         act ? clr_text() : clr_dim(), 15);
    }
};

class PageUser : public PageBase {
public:
    ModeBtn* modeBtn[3] = { nullptr, nullptr, nullptr };
    RoundInput* edServer = nullptr;
    RoundInput* edUser   = nullptr;
    RoundInput* edPass   = nullptr;
    RoundButton* btnGo   = nullptr;
    RowList* accList     = nullptr;
    int lastMode_        = -1;

    PageUser(int W, int H) : PageBase(W, H) {
        const char* names[3] = { "第三方登录", "离线登录", "微软登录" };
        for (int i = 0; i < 3; ++i) {
            modeBtn[i] = new ModeBtn(0, 0, 140, 36, names[i], i);
            modeBtn[i]->callback([](Fl_Widget* w, void* d) {
                g_userMode = ((ModeBtn*)w)->idx_;
                ((PageUser*)d)->syncMode();
            }, this);
            addTo(this, modeBtn[i], 30 + i * 150, 20);
        }

        edServer = new RoundInput(0, 0, 340, 34);
        edServer->value("https://littleskin.cn/api/yggdrasil");
        addTo(this, edServer, 110, 84);

        edUser = new RoundInput(0, 0, 260, 34);
        edUser->value(g_cfg.playerName.c_str());
        addTo(this, edUser, 110, 130);

        edPass = new RoundInput(0, 0, 260, 34);
        edPass->type(FL_SECRET_INPUT);
        addTo(this, edPass, 110, 176);

        btnGo = new RoundButton(0, 0, 150, 40, "登录", true);
        btnGo->callback([](Fl_Widget*, void* d) { ((PageUser*)d)->doLogin(); }, this);
        addTo(this, btnGo, 110, 226);

        accList = new RowList(0, 0, 320, H - 120);
        accList->setRowHeight(40);
        accList->setDrawer([](RowList* self, int idx, int rx, int ry, int rw, int rh, bool sel) {
            fill_round_rect(rx, ry + 3, rw, rh - 6, 6, sel ? clr_card_sel() : clr_card());
            if (sel) fill_round_rect(rx + 2, ry + 8, 3, rh - 16, 0, clr_accent2());
            draw_text(rx + 16, ry + 3, rw - 130, rh - 6, self->label(idx),
                      sel ? clr_text() : clr_dim(), 14);
            draw_text_right(rx + rw - 110, ry + 3, 90, rh - 6, self->sub(idx), clr_dim(), 11);
        });
        accList->callback([](Fl_Widget* w, void*) {
            int i = ((RowList*)w)->value();
            if (i >= 0 && i < (int)g_cfg.accounts.size()) {
                SelectAccount(i);
                g_status = L"已切换账户：" + U8ToW(g_cfg.accounts[i].name);
            }
        });
        addTo(this, accList, W - 350, 84);

        end();
        syncMode();
        refresh();
    }

    void resize(int X, int Y, int W, int H) override {
        PageBase::resize(X, Y, W, H);
        for (int i = 0; i < 3; ++i)
            if (modeBtn[i]) modeBtn[i]->resize(X + 30 + i * 150, Y + 20, 140, 36);
        if (edServer) edServer->resize(X + 110, Y + 84, 340, 34);
        if (edUser)   edUser->resize(X + 110, Y + 130, 260, 34);
        if (edPass)   edPass->resize(X + 110, Y + 176, 260, 34);
        if (btnGo)    btnGo->resize(X + 110, Y + 226, 150, 40);
        if (accList)  accList->resize(X + W - 350, Y + 84, 320, H - 120);
    }

    void syncMode() {
        lastMode_ = g_userMode;
        bool third = (g_userMode == 0);
        bool ms    = (g_userMode == 2);

        if (edServer) { if (third) edServer->show(); else edServer->hide(); }
        if (edPass)   { if (third) edPass->show();   else edPass->hide(); }
        if (edUser)   { if (!ms)   edUser->show();   else edUser->hide(); }
        if (btnGo) {
            btnGo->label(third ? "登录" : (ms ? "开始微软登录" : "创建账号"));
            btnGo->redraw();
        }
        for (int i = 0; i < 3; ++i) if (modeBtn[i]) modeBtn[i]->redraw();
        redraw();
    }

    void doLogin() {
        if (g_userMode == 0) {
            std::string srv = edServer ? edServer->value() : "";
            std::string usr = edUser ? edUser->value() : "";
            std::string pwd = edPass ? edPass->value() : "";
            if (srv.empty() || usr.empty()) { g_status = L"服务器地址和用户名不能为空"; redraw(); return; }
            g_status = L"正在登录…";
            redraw();
            std::thread([srv, usr, pwd]() {
                std::string err;
                if (ThirdPartyLogin(srv, usr, pwd, err)) g_status = L"登录成功：" + U8ToW(g_cfg.authName);
                else                                     g_status = L"登录失败：" + U8ToW(err);
            }).detach();
        } else if (g_userMode == 1) {
            std::string usr = edUser ? edUser->value() : "";
            if (usr.empty()) { g_status = L"玩家名不能为空"; redraw(); return; }
            CreateOfflineAccount(usr);
            g_status = L"已创建离线账户：" + U8ToW(usr);
        } else {
            g_status = L"正在启动微软登录…";
            redraw();
            std::thread([]() { MicrosoftLoginThread(); }).detach();
        }
        redraw();
    }

    void refresh() override {
        if (lastMode_ != g_userMode) syncMode();
        if (accList) {
            accList->clear();
            for (auto& a : g_cfg.accounts) {
                const char* ty = a.type == "microsoft" ? "微软"
                               : a.type == "thirdparty" ? "外置" : "离线";
                accList->add(a.name, ty);
            }
            accList->value(g_cfg.activeAcc);
        }
        redraw();
    }

    void draw() override {
        fill_round_rect(x(), y(), w(), h(), 0, clr_bg());

        bool third = (g_userMode == 0);
        int rx = x() + 110;
        int lx = x() + 30;
        if (third) {
            draw_text(lx, y() + 84, 80, 34, "服务器", clr_dim(), 13);
            draw_text(lx, y() + 130, 80, 34, "用户名", clr_dim(), 13);
            draw_text(lx, y() + 176, 80, 34, "密码", clr_dim(), 13);
        } else if (g_userMode == 1) {
            draw_text(lx, y() + 130, 80, 34, "玩家名", clr_dim(), 13);
        } else {
            draw_text(lx, y() + 84, w() - 60, 80,
                      "微软登录会打开浏览器，按提示输入验证码完成授权。", clr_dim(), 13);
        }

        draw_text(x() + 30, y() + h() - 200, 200, 22,
                  "账户 (" + std::to_string(g_cfg.accounts.size()) + ")", clr_dim(), 12);

        if (!g_status.empty())
            draw_text(x() + 30, y() + h() - 40, w() - 60, 24, g_status, clr_dim(), 12);

        PageBase::draw();
    }
};

// ============================ 设置页 ============================
//  纯自绘：点整行任意位置都能展开下拉；内存/线程可直接手动输入数字
class PageSettings : public PageBase {
public:
    int openDrop = -1;      // -1=无  0=游戏目录 1=Java 2=下载源 3=窗口 4=隔离
    bool draggingSlider = false;   // 滑条是否正在拖动
    int dropScroll = 0;
    std::vector<Cand> dirCands, javaCands;
    std::string jvCache_, jvFor_;
    // 当前活动实例（供全局鼠标钩子使用）
    static PageSettings* active;

    // 全局鼠标钩子：滑条拖动时绕过 FLTK 的控件内派发
    static int globalHook(int e) {
        if (!active) return 0;

        // ===== 数字输入中：全局接管键盘 =====
        if (active->editing >= 0) {
            if (e == FL_KEYBOARD) {
                int k = Fl::event_key();
                if (k == FL_Enter || k == FL_KP_Enter) { active->commitEdit(); return 1; }
                if (k == FL_Escape) { active->editing = -1; active->editBuf.clear(); active->redraw(); return 1; }
                if (k == FL_BackSpace) {
                    if (!active->editBuf.empty()) active->editBuf.pop_back();
                    active->redraw(); return 1;
                }
                if (k == FL_Delete) { active->editBuf.clear(); active->redraw(); return 1; }
                const char* tx = Fl::event_text();
                if (tx && *tx) {
                    for (const char* q = tx; *q; ++q)
                        if (active->editing >= 2) {
                            if (*q >= 0x20 && *q < 0x7F && active->editBuf.size() < 200) active->editBuf += *q;
                        } else {
                            if (*q >= 0x30 && *q <= 0x39 && active->editBuf.size() < 6) active->editBuf += *q;
                        }
                    active->redraw();
                }
                return 1;
            }
            if (e == FL_PUSH) {
                int fx, fw;
                active->fieldRect(fx, fw);
                int ry0 = active->y() + active->rowY(PageSettings::ROW_MEM);
                bool onMem = PageSettings::inRect(fx, ry0, fw, 34, Fl::event_x(), Fl::event_y());
                if (!onMem) { active->commitEdit(); }
            }
            return 0;
        }

        // ===== 滑条拖动中：接管鼠标移动 =====
        if (!active->draggingSlider) return 0;
        if (e == FL_DRAG || e == FL_MOVE) {
            int fx, fw;
            active->fieldRect(fx, fw);
            active->sliderFromX(Fl::event_x(), fx, fw);
            return 1;
        }
        if (e == FL_PUSH) return 1;   // 拖动中吞掉按下，避免误触别的行
        if (e == FL_RELEASE) {
            active->draggingSlider = false;
            SaveConfig();
            active->redraw();
            return 1;
        }
        return 0;
    }


    // 数字输入状态：-1=无  0=内存  1=线程
    int editing = -1;
    std::string editBuf;

    static const int LABEL_W = 110;
    static const int FIELD_X = 130;
    static const int FIELD_W = 380;
    static const int ROW_H   = 46;
    static const int ROW_TOP = 76;
    static const int ARROW_W = 30;

    // 0=游戏目录 1=Java 2=内存 3=线程 4=下载源 5=窗口 6=隔离
    // 7=额外JVM参数 8=额外游戏参数 9=CF中转站 10=authlib
    static const int ROW_DIR = 0, ROW_JAVA = 1, ROW_MEM = 2,
                     ROW_THREAD = 3, ROW_SRC = 4, ROW_WIN = 5, ROW_ISO = 6,
                     ROW_JVM = 7, ROW_GAMEARG = 8, ROW_CF = 9, ROW_AUTHLIB = 10;
    static const int ROW_COUNT = 11;

    static const char* kSourceOpts[3];
    static const char* kWinOpts[3];
    static const char* kIsoOpts[3];

    PageSettings(int W, int H) : PageBase(W, H) { end(); }

    // 行 y 坐标（含滚动偏移）
    int rowY(int i) const { return ROW_TOP + i * ROW_H - pageScroll; }

    // 物理内存（MB）
    static long long totalRamMB() {
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) return (long long)(ms.ullTotalPhys / 1048576ULL);
        return 8192;
    }

    // 推荐内存：物理内存 × 50%，夹在 1024 ~ 该机器上限之间
    static int recommendedMem() {
        long long ram = totalRamMB();
        long long rec = ram / 2;
        if (rec > 32768) rec = 32768;
        long long lo = 1024;
        if (rec < lo) rec = lo;
        // 向上取到 512 的倍数，更好看
        rec = ((rec + 511) / 512) * 512;
        return (int)rec;
    }

    static bool inRect(int rx, int ry, int rw, int rh, int mx, int my) {
        return mx >= rx && mx < rx + rw && my >= ry && my < ry + rh;
    }

    void reload() {
        RefreshDirCands();
        dirCands = g_dirCands;
        RefreshJavaCands();
        javaCands = g_javaCands;
        jvFor_.clear(); jvCache_.clear();
    }

    // 页面切换时复位下拉/编辑状态（防止切走后残留导致「没点自己展开」）
    void onShown() override {
        openDrop = -1;
        dropScroll = 0;
        editing = -1;
        editBuf.clear();
        draggingSlider = false;
        redraw();
    }

    void resetState() {
        openDrop = -1;
        dropScroll = 0;
        editing = -1;
        editBuf.clear();
        draggingSlider = false;
        redraw();
    }

    void onHidden() override { resetState(); }

    void refresh() override {
        if (dirCands.empty() && javaCands.empty()) reload();
        redraw();
    }

    std::string javaVerText() {
        if (g_cfg.javaPath.empty()) return "";
        if (jvFor_ != g_cfg.javaPath) {
            jvFor_ = g_cfg.javaPath;
            jvCache_ = JavaVersionOf(fs::u8path(g_cfg.javaPath));
        }
        return jvCache_;
    }

    int dropCount() const {
        switch (openDrop) {
            case ROW_DIR:  return (int)dirCands.size();
            case ROW_JAVA: return (int)javaCands.size();
            case ROW_SRC:  return 3;
            case ROW_WIN:  return 3;
            case ROW_ISO:  return 3;
            default: return 0;
        }
    }

    void fieldRect(int& fx, int& fw) const {
        fx = x() + FIELD_X;
        fw = FIELD_W;
        if (fx + fw > x() + w() - 24) fw = x() + w() - 24 - fx;
        if (fw < 160) fw = 160;
    }

    // 内容总高度：7 行 + 标题 + 底部按钮 + 间距
    int contentHeight() override {
        // 末尾留出底部按钮的高度，保证最后一行能滚到按钮上方
        return ROW_TOP + ROW_COUNT * ROW_H + 96;
    }

    // 统一的滚轮
    bool pageWheel(int e, int dy) { return handlePageWheel(e, dy); }

    // ============ 绘制 ============
    void draw() override {
        fill_round_rect(x(), y(), w(), h(), 0, clr_bg());

        int fx, fw;
        fieldRect(fx, fw);

        draw_text(x() + 30, y() + 26, 300, 30, "设置", clr_text(), 20);

        const char* labels[ROW_COUNT] = {
            "游戏目录", "Java 路径", "最大内存", "下载线程", "下载源", "窗口模式", "版本隔离",
            "额外 JVM 参数", "额外游戏参数", "CF 中转站", "authlib-injector"
        };

        // 行区域裁剪：内容超出时只在可视区内绘制（底部按钮固定）
        int rowsTop = y() + 66;
        int rowsBot = y() + h() - 76;
        fl_push_clip(x(), rowsTop, w(), rowsBot - rowsTop);

        for (int i = 0; i < ROW_COUNT; ++i) {
            int ry = y() + rowY(i);
            draw_text(x() + 30, ry, LABEL_W, 34, labels[i], clr_dim(), 14);
            int editRow = (editing == 0) ? ROW_MEM
                        : (editing == 1) ? ROW_THREAD
                        : (editing == 2) ? ROW_JVM
                        : (editing == 3) ? ROW_GAMEARG
                        : (editing == 4) ? ROW_CF : -1;
            bool isEdit = (editing >= 0 && i == editRow);
            fill_round_rect(fx, ry, fw, 34, 6,
                            isEdit ? clr_card_sel() : clr_inputbg());

            std::string val;
            Fl_Color vc = clr_text();
            std::string jvArg;
            int jvW = 0;

            if (isEdit) {
                val = editBuf;
                val += "_";   // 光标
            } else {
                switch (i) {
                    case ROW_DIR:    val = g_cfg.gameDir; break;
                    case ROW_JAVA:
                        val = g_cfg.javaPath;
                        jvArg = javaVerText();
                        break;
                    case ROW_MEM:    val = std::to_string(g_cfg.maxMemMB) + " MB"; break;
                    case ROW_THREAD: val = ""; break;   // 滑条行，不画文字值
                    case ROW_SRC:    val = kSourceOpts[g_cfg.dlSource]; break;
                    case ROW_WIN:    val = kWinOpts[g_cfg.winMode]; break;
                    case ROW_ISO:    val = kIsoOpts[g_cfg.isoMode]; break;
                    case ROW_JVM:     val = g_cfg.extraJvm.empty() ? "（未设置）" : g_cfg.extraJvm; break;
                    case ROW_GAMEARG: val = g_cfg.extraGame.empty() ? "（未设置）" : g_cfg.extraGame; break;
                    case ROW_CF:      val = g_cfg.cfProxy; break;
                    case ROW_AUTHLIB: val = g_cfg.authlibPath.empty() ? "（未设置，点下方自动检测）" : g_cfg.authlibPath; break;
                }
            }

            int valW = fw - ARROW_W - 16;
            if (valW < 60) valW = 60;

            std::string shown = val;
            if (i == ROW_JAVA && !isEdit && !val.empty()) {
                size_t sp = val.find_last_of("\\/");
                if (sp != std::string::npos) {
                    std::string dir = val.substr(0, sp);
                    size_t sp2 = dir.find_last_of("\\/");
                    if (sp2 != std::string::npos)
                        shown = "…\\" + dir.substr(sp2 + 1) + "\\" + val.substr(sp + 1);
                }
            }
            draw_text(fx + 10, ry, valW, 34, shown, vc, 13);

            if (!jvArg.empty()) {
                // Java 版本号画在字段框外右侧（不压箭头）
                draw_text(fx + fw + 12, ry, 120, 34, "Java " + jvArg, clr_ok(), 12);
            }

            // 内存 / 线程行不画箭头（这两行是手动输入，没有下拉）
            // 只有下拉行画箭头（内存/线程/文本参数/authlib 行不画）
            bool hasArrow = (i == ROW_DIR || i == ROW_JAVA || i == ROW_SRC ||
                             i == ROW_WIN || i == ROW_ISO);
            if (hasArrow) {
                int ax = fx + fw - ARROW_W - 4;
                bool hotA = inRect(ax, ry + 2, ARROW_W, 30, Fl::event_x(), Fl::event_y());
                fill_round_rect(ax, ry + 2, ARROW_W, 30, 5, hotA ? clr_card_sel() : clr_card());
                arrowDown(ax + ARROW_W / 2, ry + 17, hotA ? clr_text() : clr_dim());
            }
            // ---------- 下载线程：滑动条 ----------
            if (i == ROW_THREAD) drawSlider(fx, ry, fw);

            // ---------- authlib 行：操作按钮 ----------
            if (i == ROW_AUTHLIB) {
                int bx2 = fx + fw + 12, by2 = ry + 5, bw2 = 92, bh2 = 24;
                bool hot = inRect(bx2, by2, bw2, bh2, Fl::event_x(), Fl::event_y());
                fill_round_rect(bx2, by2, bw2, bh2, 6, hot ? clr_accent() : clr_card_sel());
                draw_text_center(bx2, by2, bw2, bh2, "自动检测", hot ? FL_WHITE : clr_ok(), 11);
            }
        }   // for ROW_COUNT
        fl_pop_clip();   // 结束行区域裁剪
        {
            int ry = y() + rowY(ROW_MEM);
            int rec = recommendedMem();
            long long ram = totalRamMB();
            // 推荐值提示
            std::string tip = "推荐 " + std::to_string(rec) + " MB（本机 " +
                              std::to_string(ram / 1024) + " GB）";
            // 提示文字放在按钮右侧，避免和按钮重叠
            draw_text(fx + fw + 102, ry + 5, 200, 24, tip, clr_dim(), 10);
            // 一键设为推荐
            int rb = fx + fw + 12;
            int rby = ry + 5;
            bool hotR = inRect(rb, rby, 84, 24, Fl::event_x(), Fl::event_y());
            fill_round_rect(rb, rby, 84, 24, 6, hotR ? clr_accent() : clr_card_sel());
            draw_text_center(rb, rby, 84, 24, "使用推荐", hotR ? FL_WHITE : clr_ok(), 11);
        }
        // 线程行提示：点一下可手动输入
        {
            int ry = y() + rowY(ROW_THREAD);
            draw_text(fx + fw + 12, ry, 200, 34, "点一下可手动输入", clr_dim(), 10);
        }

        // 底部按钮
        int by = y() + h() - 64;
        drawBtn(x() + 30,  by, 130, 40, "自动检测", false);
        drawBtn(x() + 176, by, 130, 40, "保存",     true);
        drawBtn(x() + 322, by, 110, 40, "关于",     false);

        if (!g_status.empty())
            draw_text(x() + 456, by, w() - 480, 40, g_status, clr_dim(), 12);
        drawDropdown();
        drawPageScrollbar();
        PageBase::draw();
    }

    void drawBtn(int bx, int by, int bw, int bh, const char* label, bool accent) {
        bool hot = inRect(bx, by, bw, bh, Fl::event_x(), Fl::event_y());
        fill_round_rect(bx, by, bw, bh, 8,
                        accent ? (hot ? clr_accent() : clr_accent2())
                               : (hot ? clr_card_sel() : clr_card()));
        draw_text_center(bx, by, bw, bh, label, accent ? FL_WHITE : clr_text(), 14);
    }

    static void arrowDown(int cx, int cy, Fl_Color c) {
        fl_color(c);
        fl_begin_polygon();
        fl_vertex(cx - 5, cy - 3);
        fl_vertex(cx + 5, cy - 3);
        fl_vertex(cx,     cy + 4);
        fl_end_polygon();
    }

    /// 下载线程滑条
    static const int SL_MIN = 1;
    static const int SL_MAX = 256;

    void sliderTrack(int fx, int fw, int& x0, int& x1) {
        x0 = fx + 10;
        x1 = fx + fw - 100;      // 右侧留出数字
        if (x1 < x0 + 40) x1 = x0 + 40;
    }

    static void drawCircleC(int cx, int cy, int r, Fl_Color c) {
        fl_color(c);
        fl_pie(cx - r, cy - r, r * 2, r * 2, 0, 360);
    }

    void drawSlider(int fx, int ry, int fw) {
        int x0, x1;
        sliderTrack(fx, fw, x0, x1);
        int cy = ry + 17;

        fill_round_rect(x0, cy - 3, x1 - x0, 6, 3, clr_card());

        int v = g_cfg.dlThreads;
        if (v < SL_MIN) v = SL_MIN;
        if (v > SL_MAX) v = SL_MAX;
        float t = (float)(v - SL_MIN) / (float)(SL_MAX - SL_MIN);
        int px = x0 + (int)((x1 - x0) * t + 0.5f);
        if (px > x0) fill_round_rect(x0, cy - 3, px - x0, 6, 3, clr_accent2());

        bool hot = inRect(px - 9, cy - 9, 18, 18, Fl::event_x(), Fl::event_y());
        drawCircleC(px, cy, 9, hot ? clr_accent() : clr_accent2());
        drawCircleC(px, cy, 5, clr_bg());

        draw_text_right(x1 + 6, ry, 90, 34,
                        std::to_string(g_cfg.dlThreads) + " 线程", clr_text(), 12);
    }

    void sliderFromX(int mx, int fx, int fw) {
        int x0, x1;
        sliderTrack(fx, fw, x0, x1);
        if (mx < x0) mx = x0;
        if (mx > x1) mx = x1;
        float t = (float)(mx - x0) / (float)(x1 - x0);
        int v = SL_MIN + (int)(t * (SL_MAX - SL_MIN) + 0.5f);
        if (v < SL_MIN) v = SL_MIN;
        if (v > SL_MAX) v = SL_MAX;
        g_cfg.dlThreads = v;
        SaveConfig();
        redraw();
    }

    // 「使用推荐」按钮统一坐标（draw 和 handle 共用，窗口拉伸也不会点不到）
    void recBtnRect(int& bx, int& by, int& bw, int& bh) const {
        int fx, fw;
        fieldRect(fx, fw);
        bx = fx + fw + 12;
        by = y() + rowY(ROW_MEM) + 2;
        bw = 150;
        bh = 30;
    }

    void dropGeom(int& fx, int& fw, int& ry, int& mh, int& visible) {
        fieldRect(fx, fw);
        int n = dropCount();
        ry = y() + rowY(openDrop) + 37;
        const int ih = 28;
        visible = n > 8 ? 8 : n;
        if (visible < 1) visible = 1;
        mh = visible * ih + 8;
        if (ry + mh > y() + h() - 70) ry = y() + h() - 70 - mh;
        if (ry < y() + 60) ry = y() + 60;
    }

    void drawDropdown() {
        if (openDrop < 0) return;

        int fx, fw, ry, mh, visible;
        dropGeom(fx, fw, ry, mh, visible);
        int n = dropCount();
        const int ih = 28;

        fill_round_rect(fx, ry, fw, mh, 8, clr_card_sel());
        stroke_round_rect(fx, ry, fw, mh, 8, clr_accent());

        if (n == 0) {
            draw_text(fx + 12, ry + 4, fw - 24, ih, "（没有检测到候选）", clr_dim(), 12);
            return;
        }

        for (int k = 0; k < visible; ++k) {
            int i = k + dropScroll;
            if (i >= n) break;
            int iy = ry + 4 + k * ih;

            std::string txt, sub;
            bool cur = false;
            switch (openDrop) {
                case ROW_DIR:  txt = dirCands[i].path; sub = dirCands[i].label;
                               cur = (dirCands[i].path == g_cfg.gameDir); break;
                case ROW_JAVA: txt = javaCands[i].path; sub = javaCands[i].label;
                               cur = (javaCands[i].path == g_cfg.javaPath); break;
                case ROW_SRC:  txt = kSourceOpts[i]; cur = (g_cfg.dlSource == i); break;
                case ROW_WIN:  txt = kWinOpts[i];    cur = (g_cfg.winMode == i); break;
                case ROW_ISO:  txt = kIsoOpts[i];    cur = (g_cfg.isoMode == i); break;
            }

            bool hot = inRect(fx + 2, iy, fw - 4, ih - 2, Fl::event_x(), Fl::event_y());
            if (hot)        fill_round_rect(fx + 2, iy, fw - 4, ih - 2, 5, clr_accent2());
            else if (cur)   fill_round_rect(fx + 2, iy, fw - 4, ih - 2, 5, clr_card());

            Fl_Color tc = hot ? FL_WHITE : (cur ? clr_ok() : clr_text());

            if (!sub.empty()) {
                draw_text(fx + 12, iy, 96, ih - 2, sub, hot ? FL_WHITE : clr_ok(), 11);
                draw_text(fx + 112, iy, fw - 124, ih - 2, txt, tc, 12);
            } else {
                draw_text(fx + 12, iy, fw - 24, ih - 2, txt, tc, 12);
            }
        }

        if (n > visible) {
            draw_text_right(fx, ry + mh - 14, fw - 10, 14,
                            std::to_string(dropScroll + 1) + "-" +
                            std::to_string(std::min(n, dropScroll + visible)) +
                            "/" + std::to_string(n), clr_dim(), 10);
        }
    }

    // ============ 事件 ============
    int handle(int e) override {
        // 统一滚动（内容超出窗口时）
        if (handlePageWheel(e, Fl::event_dy())) return 1;
        int mx = Fl::event_x(), my = Fl::event_y();
        int fx, fw;
        fieldRect(fx, fw);

        if (draggingSlider) {
            if (e == FL_DRAG || e == FL_MOVE) {
                sliderFromX(mx, fx, fw);
                return 1;
            }
            if (e == FL_RELEASE) {
                draggingSlider = false;
                SaveConfig();
                redraw();
                return 1;
            }
            // 新的 FL_PUSH 说明上一轮拖动已结束（可能丢了 RELEASE），清掉残留状态
            if (e == FL_PUSH) { draggingSlider = false; }
        }

        // ---------- 数字输入中 ----------
        if (editing >= 0) {
            if (e == FL_KEYBOARD || e == FL_KEYUP) {
                int k = Fl::event_key();
                if (k == FL_Enter || k == FL_KP_Enter) { commitEdit(); return 1; }
                if (k == FL_Escape) { editing = -1; editBuf.clear(); redraw(); return 1; }
                if (k == FL_BackSpace) {
                    if (!editBuf.empty()) editBuf.pop_back();
                    redraw(); return 1;
                }
                if (k == FL_Delete) { editBuf.clear(); redraw(); return 1; }
                const char* t = Fl::event_text();
                if (t && *t) {
                    for (const char* p = t; *p; ++p) {
                        // 数字字段只收数字；文本参数收任意可打印字符
                        if (editing >= 2) {
                            if (*p >= 0x20 && *p < 0x7F && editBuf.size() < 200) editBuf += *p;
                        } else {
                            if (*p >= '0' && *p <= '9' && editBuf.size() < 6) editBuf += *p;
                        }
                    }
                    redraw();
                }
                return 1;
            }
            if (e == FL_UNFOCUS) { commitEdit(); return 1; }
            if (e == FL_PUSH) {
                int ry = y() + rowY(ROW_MEM);
                // 只认整行（不只是字段框），点到行外就提交
                if (!inRect(x() + 30, ry, w() - 54, 34, mx, my)) { commitEdit(); }
                else return 1;
            }
            if (e == FL_MOVE || e == FL_ENTER || e == FL_LEAVE) return 1;
            return 1;
        }

        // ---------- 下拉展开中 ----------
        if (openDrop >= 0) {
            int fx2, fw2, ry, mh, visible;
            dropGeom(fx2, fw2, ry, mh, visible);
            int n = dropCount();
            const int ih = 28;

            if (e == FL_MOUSEWHEEL) {
                dropScroll += Fl::event_dy();   // FLTK: 向下滚 dy>0
                int maxS = n - visible;
                if (maxS < 0) maxS = 0;
                if (dropScroll < 0) dropScroll = 0;
                if (dropScroll > maxS) dropScroll = maxS;
                redraw();
                return 1;
            }
            if (e == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) {
                if (mx >= fx2 && mx < fx2 + fw2 && my >= ry && my < ry + mh) {
                    int k = (my - ry - 4) / ih;
                    int i = k + dropScroll;
                    if (i >= 0 && i < n) pickDrop(i);
                    redraw();
                    return 1;
                }
                // 点到下拉以外 → 关闭
                openDrop = -1;
                dropScroll = 0;
                redraw();
                return 1;
            }
            // 展开中其它事件一律吞掉，绝不让 FL_MOVE 掉到下面的行点击逻辑
            return 1;
        }

        // ============================================================
        //  只有 FL_PUSH 才允许改 openDrop / editing / draggingSlider
        //  鼠标移动类事件只负责触发重绘（hover 高亮），不改任何状态
        // ============================================================
        if (e != FL_PUSH || Fl::event_button() != FL_LEFT_MOUSE) {
            if (e == FL_MOVE || e == FL_ENTER || e == FL_LEAVE || e == FL_DRAG) {
                redraw();     // 让 inRect(..Fl::event_x()) 重新算 hover
                return 1;
            }
            return PageBase::handle(e);
        }

        // ---------- 整行可点（内存/线程行除外，它们是字段内）----------
        {
            int rowX = x() + 30;
            int rowW = x() + w() - 24 - rowX;
            if (rowW < 100) rowW = 100;

            for (int i = 0; i < ROW_COUNT; ++i) {
                int ry = y() + rowY(i);
                // 行在可视区外就跳过（滚动后）
                if (ry + 34 < y() + 66 || ry > y() + h() - 76) continue;

                if (i == ROW_THREAD) {
                    // 线程行：只认字段框（滑条区域），避免误触
                    if (!inRect(fx, ry, fw, 34, mx, my)) continue;
                    draggingSlider = true;
                    sliderFromX(mx, fx, fw);
                    return 1;
                }
                if (i == ROW_MEM) {
                    if (!inRect(fx, ry, fw, 34, mx, my)) continue;
                    editing = 0;
                    editBuf = std::to_string(g_cfg.maxMemMB);
                    take_focus();
                    if (Fl::focus() != this) Fl::focus(this);
                    redraw();
                    return 1;
                }

                // 额外参数行：点字段框进入文本编辑
                if (i == ROW_JVM || i == ROW_GAMEARG || i == ROW_CF) {
                    if (!inRect(fx, ry, fw, 34, mx, my)) continue;
                    editing = (i == ROW_JVM) ? 2 : (i == ROW_GAMEARG) ? 3 : 4;
                    editBuf = (i == ROW_JVM)      ? g_cfg.extraJvm
                            : (i == ROW_GAMEARG)  ? g_cfg.extraGame
                                                  : g_cfg.cfProxy;
                    take_focus();
                    if (Fl::focus() != this) Fl::focus(this);
                    redraw();
                    return 1;
                }

                // authlib 行：只有右侧按钮生效
                if (i == ROW_AUTHLIB) {
                    int bx2 = fx + fw + 12, by2 = ry + 5, bw2 = 92, bh2 = 24;
                    if (inRect(bx2, by2, bw2, bh2, mx, my)) {
                        g_cfg.authlibPath = DetectAuthlib();
                        if (g_cfg.authlibPath.empty()) g_cfg.authlibPath = ExtractEmbeddedAuthlib();
                        if (!g_cfg.authlibPath.empty()) {
                            SaveConfig();
                            g_status = L"已找到 authlib-injector";
                        } else {
                            g_status = L"没有找到 authlib-injector.jar";
                        }
                        redraw();
                        return 1;
                    }
                    continue;
                }

                // 其余行：整行可点（下拉）
                if (!inRect(rowX, ry, rowW, 34, mx, my)) continue;

                if (i == ROW_DIR && dirCands.empty()) { RefreshDirCands(); dirCands = g_dirCands; }
                if (i == ROW_JAVA && javaCands.empty()) { RefreshJavaCands(); javaCands = g_javaCands; }
                openDrop = i;
                dropScroll = 0;
                redraw();
                return 1;
            }
        }

        // ---------- 底部按钮（同样只能 FL_PUSH 触发）----------
        int by = y() + h() - 64;
        if (inRect(x() + 30, by, 130, 40, mx, my)) {
            std::string d = DetectGameDir();
            if (d.empty()) {
                g_status = L"没有找到 .minecraft 目录";
            } else {
                SetGameDir(d);
                reload();
                jvFor_.clear();
                g_status = L"已检测到游戏目录";
            }
            redraw();
            return 1;
        }
        if (inRect(x() + 176, by, 130, 40, mx, my)) {
            SaveConfig();
            g_status = L"设置已保存";
            redraw();
            return 1;
        }
        {
            int rb, rby, rbw, rbh;
            recBtnRect(rb, rby, rbw, rbh);
            if (inRect(rb, rby, rbw, rbh, mx, my)) {
                int rec = recommendedMem();
                g_cfg.maxMemMB = rec;
                SaveConfig();
                g_status = L"已设为推荐内存 " + std::to_wstring(rec) + L" MB";
                redraw();
                return 1;
            }
        }
        if (inRect(x() + 322, by, 110, 40, mx, my)) {
            showAbout();
            return 1;
        }
        return 1;
    }
    void commitEdit() {
        if (editing < 0) return;

        if (editing == 2) {              // 额外 JVM 参数（字符串）
            g_cfg.extraJvm = editBuf;
            g_status = L"额外 JVM 参数已保存";
        } else if (editing == 3) {       // 额外游戏参数
            g_cfg.extraGame = editBuf;
            g_status = L"额外游戏参数已保存";
        } else if (editing == 4) {       // CF 中转站
            if (!editBuf.empty()) g_cfg.cfProxy = editBuf;
            g_status = L"CurseForge 中转站已保存";
        } else {
            int v = editBuf.empty() ? 0 : std::atoi(editBuf.c_str());
            if (editing == 0) {
                if (v > 0) g_cfg.maxMemMB = v;
            } else {
                if (v >= 1 && v <= 256) g_cfg.dlThreads = v;
                else if (v < 1) g_cfg.dlThreads = 1;
                else g_cfg.dlThreads = 256;
            }
        }
        SaveConfig();
        editing = -1;
        editBuf.clear();
        redraw();
    }

    void pickDrop(int i) {
        switch (openDrop) {
            case ROW_DIR:
                if (i < (int)dirCands.size()) {
                    SetGameDir(dirCands[i].path);
                    jvFor_.clear();
                    g_status = L"游戏目录已切换";
                }
                break;
            case ROW_JAVA:
                if (i < (int)javaCands.size()) {
                    g_cfg.javaPath = javaCands[i].path;
                    jvFor_.clear();
                    SaveConfig();
                    g_status = L"Java 已切换";
                }
                break;
            case ROW_SRC:
                g_cfg.dlSource = i;
                SaveConfig();
                g_status = std::wstring(L"下载源：") + U8ToW(kSourceOpts[i]);
                break;
            case ROW_WIN:
                g_cfg.winMode = i;
                SaveConfig();
                g_status = std::wstring(L"窗口模式：") + U8ToW(kWinOpts[i]);
                break;
            case ROW_ISO:
                g_cfg.isoMode = i;
                SaveConfig();
                g_status = std::wstring(L"版本隔离：") + U8ToW(kIsoOpts[i]);
                break;
        }
        openDrop = -1;
        dropScroll = 0;
        redraw();
    }
};

const char* PageSettings::kSourceOpts[3] = { "自动", "仅官方", "仅 BMCLAPI" };
const char* PageSettings::kWinOpts[3]    = { "窗口", "最大化", "全屏" };
const char* PageSettings::kIsoOpts[3]    = { "隔离所有实例", "仅模组实例", "不隔离" };
PageSettings* PageSettings::active = nullptr;

// ============================ 关于页 ============================
class PageAbout : public PageBase {
public:
    RoundButton* btnBack  = nullptr;
    RoundButton* btnCheck = nullptr;

    PageAbout(int W, int H) : PageBase(W, H) {
        btnBack = new RoundButton(0, 0, 120, 40, "返回设置");
        btnBack->callback([](Fl_Widget*, void*) { backFromAbout(); });
        addTo(this, btnBack, 30, H - 60);

        btnCheck = new RoundButton(0, 0, 130, 40, "检查更新");
        btnCheck->callback([](Fl_Widget*, void* d) {
            auto* self = (PageAbout*)d;
            // 已有新版本 → 直接下载；否则先检查
            if (g_updateState.load() == 2 && !g_newUrl.empty()) {
                g_status = L"正在下载新版本…";
                self->redraw();
                std::thread([]() { StartUpdate(); }).detach();
                return;
            }
            if (g_updateState.load() == 1) return;      // 正在检查
            g_updateState = 1;
            g_status = L"正在检查更新…";
            self->redraw();
            std::thread([]() { UpdateThread(true); }).detach();
        }, this);
        addTo(this, btnCheck, W - 170, H - 60);
        end();
    }

    void resize(int X, int Y, int W, int H) override {
        PageBase::resize(X, Y, W, H);
        if (btnBack)  btnBack->resize(X + 30, Y + H - 60, 120, 40);
        if (btnCheck) btnCheck->resize(X + W - 170, Y + H - 60, 130, 40);
    }

    // 链接区域（用于点击判定）
    void linkRect(int& lx, int& ly, int& lw, int& lh) const {
        lx = x() + 40;
        ly = y() + 396;
        lw = 420;
        lh = 26;
    }

    void draw() override {
        fill_round_rect(x(), y(), w(), h(), 0, clr_bg());

        int cx = x() + 40, cy = y() + 30;
        fill_round_rect(cx, cy, 72, 72, 14, clr_accent2());
        draw_text_center(cx, cy, 72, 72, "RL", FL_WHITE, 24);

        draw_text(cx + 90, cy + 2, 480, 34, "ReignLaunch", clr_text(), 22);
        draw_text(cx + 90, cy + 40, 480, 24, "Minecraft 启动器", clr_dim(), 14);

        // ---------- 基本信息 ----------
        int ly = y() + 130;
        struct KV { const char* k; std::string v; };
        KV rows[] = {
            { "版本",     APP_VER_STR },
            { "制作人",   "加纳Brightchaserove（抖音）" },
            { "游戏目录", g_cfg.gameDir },
        };
        for (auto& r : rows) {
            draw_text(cx, ly, 100, 26, r.k, clr_dim(), 13);
            draw_text(cx + 110, ly, w() - 200, 26, r.v, clr_text(), 13);
            ly += 34;
        }

        draw_text(cx, ly + 14, w() - 120, 22, "从零自制的 Minecraft 启动器。", clr_dim(), 12);
        draw_text(cx, ly + 38, w() - 120, 22, "支持多账户、加载器安装、Java 自动匹配。", clr_dim(), 12);

        // ---------- 开源地址（可点击） ----------
        int lx, lkY, lw, lh;
        linkRect(lx, lkY, lw, lh);
        draw_text(cx, lkY - 30, 200, 22, "开源地址", clr_dim(), 12);

        bool hotLink = inRect(lx, lkY, lw, lh, Fl::event_x(), Fl::event_y());
        draw_text(lx, lkY, lw, lh,
                  "https://github.com/ShadowForgekin/ReignLaunch",
                  hotLink ? clr_accent() : clr_accent2(), 13);
        if (hotLink) {
            // 悬停时画下划线
            fl_color(clr_accent());
            fl_line(lx, lkY + 22, lx + 340, lkY + 22);
        }

        // ---------- 特别鸣谢 ----------
        int ty = lkY + 42;
        draw_text(cx, ty, 200, 22, "特别鸣谢", clr_dim(), 12);
        draw_text(cx, ty + 24, w() - 80, 20,
                  "Powered by FLTK 1.4.5 · miniz 3.1.2 · nlohmann/json 3.12.0",
                  clr_dim(), 11);
        draw_text(cx, ty + 48, w() - 80, 20,
                  "数据源 MCIM · BMCLAPI · Modrinth · CurseForge",
                  clr_dim(), 11);

        // ---------- 更新状态 ----------
        {
            int st = g_updateState.load();
            std::string msg;
            Fl_Color mc = clr_dim();
            switch (st) {
                case 1: msg = "正在检查更新…"; break;
                case 2:
                    msg = "发现新版本 " + WToU8(g_newTag) + "，点「检查更新」下载";
                    mc = clr_ok();
                    break;
                case 3: msg = std::string("已是最新版本 (") + APP_VER_STR + ")"; mc = clr_ok(); break;
                case 4: msg = "检查失败（网络问题或未发布 Release）"; mc = clr_err(); break;
                default: break;
            }
            if (!msg.empty())
                draw_text(cx, y() + h() - 96, w() - 240, 24, msg, mc, 12);
        }

        if (!g_status.empty())
            draw_text(cx, y() + h() - 36, w() - 200, 22, g_status, clr_dim(), 12);

        PageBase::draw();
    }

    int handle(int e) override {
        // 先给子控件（按钮）
        if (e == FL_PUSH || e == FL_RELEASE || e == FL_MOVE || e == FL_ENTER || e == FL_LEAVE) {
            if (PageBase::handle(e)) return 1;
        }

        int mx = Fl::event_x(), my = Fl::event_y();

        if (e == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) {
            int lx, ly, lw, lh;
            linkRect(lx, ly, lw, lh);
            if (inRect(lx, ly, lw, lh, mx, my)) {
                // 打开 GitHub 页面
                ShellExecuteW(nullptr, L"open",
                              L"https://github.com/ShadowForgekin/ReignLaunch",
                              nullptr, nullptr, SW_SHOWNORMAL);
                g_status = L"已在浏览器中打开项目主页";
                redraw();
                return 1;
            }
            return 1;
        }

        if (e == FL_MOVE || e == FL_ENTER || e == FL_LEAVE) { redraw(); return 1; }
        return PageBase::handle(e);
    }

    static bool inRect(int rx, int ry, int rw, int rh, int mx, int my) {
        return mx >= rx && mx < rx + rw && my >= ry && my < ry + rh;
    }
};

// ============================ 主窗口 ============================
static PageLaunch*   pageLaunch   = nullptr;
static PageDownload* pageDownload = nullptr;
static PageInstance* pageInstance = nullptr;
static PageUser*     pageUser     = nullptr;
static PageSettings* pageSettings = nullptr;

// ============================ 自适应布局 ============================
// 窗口最大化/拉伸时，把所有页面、浮层、标题栏都重新铺满
static Fl_Box*    g_headerBg = nullptr;
static const int  HDR_H = 66;

static void layoutAll(int W, int H) {
    if (W < 800) W = 800;
    if (H < 500) H = 500;
    if (g_headerBg) g_headerBg->resize(0, 0, W, HDR_H);

    int CH = H - HDR_H;
    if (CH < 100) CH = 100;

    for (int i = 0; i < P_COUNT; ++i) {
        if (!g_pages[i]) continue;
        g_pages[i]->resize(0, HDR_H, W, CH);   // 虚函数，触发各页自己的重排
    }
    if (g_aboutPage) g_aboutPage->resize(0, HDR_H, W, CH);
    if (g_overlay)   g_overlay->resize(0, HDR_H, W, CH);
    if (g_win)       g_win->redraw();
}

// 自定义窗口：resize 时重新布局
class MainWindow : public Fl_Double_Window {
public:
    MainWindow(int W, int H, const char* t) : Fl_Double_Window(W, H, t) {}
    void resize(int X, int Y, int W, int H) override {
        Fl_Double_Window::resize(X, Y, W, H);
        layoutAll(W, H);
    }
};

// ============================ 全局事件过滤器 ============================
// FLTK 的 Fl_Group 会先把事件分派给子控件，
// 启动页左侧导航栏没有对应子控件，所以要在这里拦截。
static PageLaunch* g_pageLaunchPtr = nullptr;

static int global_handler(int e) {
    if (g_pageLaunchPtr && (e == FL_PUSH || e == FL_RELEASE || e == FL_MOVE)) {
        int mx = Fl::event_x(), my = Fl::event_y();
        Fl_Widget* pw = Fl::belowmouse();
        // 只有当鼠标在启动页区域、且没被其它控件处理时才转给启动页
        if (g_curPage == P_LAUNCH && pw == (Fl_Widget*)g_pageLaunchPtr) {
            if (g_pageLaunchPtr->handle(e)) return 1;
        }
    }
    if (g_pageLaunchPtr && e == FL_PUSH) {
        int mx = Fl::event_x(), my = Fl::event_y();
        if (g_curPage == P_LAUNCH &&
            mx >= g_pageLaunchPtr->x() && mx < g_pageLaunchPtr->x() + LP_SIDEBAR) {
            if (g_pageLaunchPtr->handle(e)) return 1;
        }
    }
    return 0;
}

static bool g_rebuildPending = false;
static void ui_tick(void*) {
    // ★ 后台搜索/拉版本完成后，通知 UI 刷新（不用手动点）
    static int lastModState = -1, lastModVerState = -1;
    int ms = g_modState.load(), mvs = g_modVerState.load();
    if (ms != lastModState || mvs != lastModVerState) {
        lastModState = ms;
        lastModVerState = mvs;
        if (g_curPage == P_DOWNLOAD && pageDownload) pageDownload->redraw();
    }

    // ★ 更新检查完成后刷新关于页
    static int lastUpdState = -1;
    int us = g_updateState.load();
    if (us != lastUpdState) {
        lastUpdState = us;
        if (g_curPage == P_SETTINGS && g_aboutPage) g_aboutPage->redraw();
        if (us == 2) g_status = L"发现新版本 " + g_newTag + L"，点「检查更新」下载";
        else if (us == 3) g_status = L"已是最新版本";
        else if (us == 4) g_status = L"更新检查失败（网络问题）";
    }

    Fl::repeat_timeout(0.15, ui_tick, nullptr);
}

int main(int argc, char** argv) {
    // 预初始化 C++ locale（MinGW 下工作线程首次构造 locale 会崩溃）
    try { std::locale::classic(); (void)std::locale(); } catch (...) {}
    // ---- 初始化 core ----
    wchar_t appdata[MAX_PATH] = { 0 };
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata);
    g_dataDir = WToU8((fs::path(appdata) / L"ReignLaunch").wstring());
    g_cfgPath = WToU8((fs::path(appdata) / L"ReignLaunch" / L"config.json").wstring());
    LoadConfig();
    ReloadVersions();
    g_hwnd = nullptr;

    // ---- 界面 ----
    apply_theme();

    const int W = 1060, H = 680, HEADER_H = 66;

    g_win = new MainWindow(W, H, "ReignLaunch - Minecraft 启动器");
    g_win->color(clr_bg());
    g_win->size_range(900, 600);
    g_win->begin();

    g_headerBg = new Fl_Box(0, 0, W, HEADER_H);
    g_headerBg->box(FL_FLAT_BOX);
    g_headerBg->color(clr_header());

    Fl_Box* logo = new Fl_Box(24, 0, 200, HEADER_H, "ReignLaunch");
    logo->labelfont(FL_HELVETICA_BOLD);
    logo->labelsize(20);
    logo->labelcolor(clr_text());
    logo->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    int tx = 220;
    for (int i = 0; i < P_COUNT; ++i) {
        new TabButton(tx, 17, 86, 32, kTabNames[i], i);
        tx += 92;
    }

    const int CH = H - HEADER_H;

    pageLaunch   = new PageLaunch(W, CH);
    pageDownload = new PageDownload(W, CH);
    (void)0;   // 实例页已并入启动页左侧列表
    pageUser     = new PageUser(W, CH);
    pageSettings = new PageSettings(W, CH);
    PageSettings::active = pageSettings;
    g_resetSettings = []() { PageSettings::active->resetState(); };
    PageAbout* pageAbout = new PageAbout(W, CH);

    g_pages[P_LAUNCH]   = pageLaunch;
    g_pages[P_DOWNLOAD] = pageDownload;

    g_pages[P_USER]     = pageUser;
    g_pages[P_SETTINGS] = pageSettings;
    g_aboutPage         = pageAbout;

    for (int i = 0; i < P_COUNT; ++i) {
        g_pages[i]->position(0, HEADER_H);
        g_pages[i]->size(W, CH);
        setPageVisible(g_pages[i], false);
    }
    pageAbout->position(0, HEADER_H);
    pageAbout->size(W, CH);
    setPageVisible(pageAbout, false);

    // 浮层（最后创建，保证在最上层）
    // 浮层（最后创建，保证在最上层）
    g_overlay = new Overlay(0, HEADER_H, W, CH);
    g_overlay->onCancel = []() {
        g_cancelLaunch = true;
        g_status = L"已取消";
        if (g_overlay) g_overlay->hide();
        g_launching = false;
    };
    g_overlay->onClose = []() {
        if (g_overlay) g_overlay->hide();
    };
    g_overlay->hide();
    g_win->show(argc, argv);
    // 设置窗口图标（任务栏 + 标题栏），图标资源 ID 100 来自 res.rc
    if (HWND hwndMain = (HWND)fl_xid(g_win)) {
        HICON hi = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(100));
        if (!hi) hi = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
        if (hi) {
            SendMessageW(hwndMain, WM_SETICON, ICON_BIG,   (LPARAM)hi);
            SendMessageW(hwndMain, WM_SETICON, ICON_SMALL, (LPARAM)hi);
        }
    }

    // 启动时就异步拉版本清单（修复「下载页一打开是空的」）
    // 清单拉取放在 UI 线程（工作线程里 JSON 解析会导致 locale 崩溃）
    // 用定时器延迟，先让窗口画出来
    Fl::add_timeout(0.5, [](void*) { FetchManifestThread(); });

    doSwitch(P_LAUNCH);
    g_win->redraw();

    g_pageLaunchPtr = pageLaunch;

    Fl::add_handler(PageSettings::globalHook);   // 滑条拖动（绕过控件内派发）
    Fl::add_timeout(0.15, ui_tick);
    int rc = Fl::run();
    return rc;
}
