// ============================================================================
//  MCLite - 自制 Minecraft 启动器（Win32 + C++17）
//  单窗口多页面：启动 / 用户 / 设置
//  - 第三方登录（Yggdrasil / authlib-injector，如 LittleSkin）
//  - 设置全部在主窗口内，不弹新窗口
//  - 日志不显示，只有游戏出错才弹错误窗口
// ---------------------------------------------------------------------------
//  编译：
//    g++ -std=c++17 -O2 -mwindows -static ^
//        -finput-charset=UTF-8 -fexec-charset=UTF-8 -fwide-exec-charset=UTF-16LE ^
//        -o MCLite.exe MCLite.cpp ^
//        -lcomctl32 -lole32 -lshell32 -lgdi32 -luuid -ladvapi32 -lwinhttp
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

namespace fs = std::filesystem;
using json = nlohmann::json;

#define IDR_AUTHLIB     101   // 内嵌的 authlib-injector.jar 资源 ID
#define IDI_APP         100   // 应用图标

// ============================ 页面 ============================
enum Page { PAGE_LAUNCH = 0, PAGE_USER = 1, PAGE_SETTINGS = 2, PAGE_ABOUT = 3, PAGE_INSTANCES = 4, PAGE_DOWNLOAD = 5 };

// ============================ 配置 ============================
struct Account {
    std::string type;    // offline / thirdparty
    std::string name;
    std::string server;
    std::string token;
    std::string uuid;
    std::string skin;    // 该账户的皮肤 PNG
};

struct Config {
    std::vector<Account> accounts;
    int activeAcc = -1;
    std::string gameDir;
    std::string skinPath;   // 当前皮肤 PNG
    int  isoMode = 0;
    int  dlThreads = 4;
    std::string extraJvm;            // 追加的 JVM 参数
    std::string extraGame;           // 追加的游戏参数
    int  dlSource = 0;               // 0=自动 1=仅官方 2=仅镜像
    int  winMode = 0;                // 0=窗口 1=最大化 2=全屏             // 下载线程数 1..128               // 版本隔离：0=隔离所有实例 1=仅模组实例 2=不隔离
    std::string javaPath;
    int         maxMemMB = 4096;
    std::string playerName = "Steve";

    std::string authType = "offline";   // offline | thirdparty
    std::string authServer;             // 如 https://littleskin.cn/api/yggdrasil
    std::string authToken;
    std::string authClientToken;
    std::string authName;
    std::string authUuid;
    std::string authlibPath;
};

static Config      g_cfg;
static std::string g_dataDir;   // 配置/缓存目录
static std::string g_cfgPath;

static std::vector<std::string> g_versions;
static int         g_selVersion = -1;
static int         g_listScroll = 0;
static Page        g_page = PAGE_LAUNCH;
static bool        g_launching = false;
static std::atomic<int> g_progState{ 0 };   // 0 空闲 1 补全 2 启动
static std::atomic<int> g_progCur{ 0 };
static std::atomic<int> g_progTotal{ 0 };
static bool g_overlay = false;                       // 启动浮层
static std::atomic<int>  g_stepCur{ 0 };             // 已完成步骤 0..4
static std::atomic<bool> g_cancelLaunch{ false };
static ULONG_PTR g_gpToken = 0;
static bool      g_gpOk = false;
static HBITMAP   g_skinBmp = nullptr;
static DWORD g_overlayDoneTick = 0;   // 全部步骤完成的时刻，用于自动收起浮层
static int         g_dpi = 96;

static std::mutex               g_logMutex;
static std::vector<std::string> g_logs;

static HWND g_hwnd = nullptr;
static HFONT g_fTitle = nullptr, g_fUi = nullptr, g_fSmall = nullptr;

// ============================ 控件 ============================
static HWND hBtnTabLaunch, hBtnTabUser, hBtnTabSettings, hBtnTabAbout, hBtnTabInst, hBtnTabDl;
static HWND hBtnLaunch, hBtnFill, hBtnRefresh, hBtnInstArrow, hBtnOverlayCancel;
static std::string JavaVersionOf(const fs::path& javaw);
static int RequiredJavaMajor(const std::string& id);
static std::string PickJavaFor(int need);
static RECT OverlayCardRect();
static RECT OverlayCancelRect();
static void ApplyOverlayVisibility();

static HWND hLblServer, hEdServer, hLblUser, hEdUser, hLblPass, hEdPass;
static HWND hBtnLogin, hBtnLogout, hBtnOffline, hBtnSkin, hBtnDelAcc;

static HWND hLblDir, hEdDir, hBtnAutoDir, hBtnBrowseDir, hBtnDirDrop;
static HWND hLblJava, hEdJava, hBtnAutoJava, hBtnBrowseJava, hBtnJavaDrop;
static HWND hLblMem, hEdMem, hEdName, hLblName, hBtnSave, hBtnIso, hEdThreads, hEdJvm, hEdGame, hBtnSource, hBtnFull;
static HWND hBtnUpdate;
static HWND hBtnDlVanilla, hBtnDlFabric, hBtnDlQuilt, hBtnDlForge, hBtnDlNeo;
static HWND hBtnDlF0, hBtnDlF1, hBtnDlF2, hBtnDlF3, hEdSearch, hBtnDlBack, hBtnDlInstall, hBtnDlFapi, hBtnDlQsl, hBtnDlFilterSel;

struct McVer { std::string id, type, url, date; };
static std::vector<McVer> g_mcVers;
static std::atomic<int>  g_mcVersState{ 0 };
static int g_dlFilter = 0;                 // 0正式 1快照 2远古 3愚人节
static std::vector<int> g_dlView;
static int g_dlSel = -1;          // 过滤后的下标
static bool g_dlDetail = false;   // 是否在详情页
// 加载器可用性：0=查询中 1=有 2=没有
static std::atomic<int> g_ldState[4] = { {0}, {0}, {0}, {0} };
static std::wstring g_ldVer[4];
static void FetchLoadersThread();
static std::string g_dlDetailMc;
static int g_dlLoader = 0;              // 0=原版 1=Fabric 2=Quilt 3=Forge 4=NeoForge
static std::vector<std::string> g_ldVers;   // 当前加载器的可选版本
static std::vector<std::string> g_ldFiles;  // 对应 mod 的下载地址（0/1/2/3/4 为空）
static int g_ldSel = -1;
static std::atomic<int> g_ldVersState{ 0 }; // 0未取 1取中 2成功 3失败
static void FetchLoaderVersThread();
static void RebuildDlView();
static void FetchManifestThread();
static bool IsAprilFools(const McVer& v);
// ============================ 颜色 ============================
static const COLORREF C_BG       = RGB(24, 25, 28);
static const COLORREF C_INPUTBG  = RGB(36, 38, 42);
static const COLORREF C_HEADER   = RGB(31, 33, 37);
static const COLORREF C_CARD     = RGB(45, 48, 53);
static const COLORREF C_CARD_SEL = RGB(58, 62, 70);
static const COLORREF C_ACCENT   = RGB(37, 99, 235);
static const COLORREF C_ACCENT2  = RGB(59, 130, 246);
static const COLORREF C_TEXT     = RGB(232, 234, 237);
static const COLORREF C_DIM      = RGB(150, 155, 162);
static const COLORREF C_OK       = RGB(74, 222, 128);
static const COLORREF C_ERR      = RGB(248, 113, 113);

static HBRUSH g_brInput = nullptr;

// ============================ 消息 ============================
#define WM_APP_DONE  (WM_APP + 1)
#define WM_APP_ERROR (WM_APP + 2)
#define WM_APP_NEEDJAVA (WM_APP + 9)
#define WM_APP_UPD       (WM_APP + 11)

// ============================ 工具 ============================
static std::wstring U8ToW(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}
static std::string WToU8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static bool EndsWithNoCase(const std::string& s, const std::string& suffix) {
    if (s.size() < suffix.size()) return false;
    return _stricmp(s.c_str() + s.size() - suffix.size(), suffix.c_str()) == 0;
}

static bool FileExists(const fs::path& p);

static std::string EffectiveJavaExe() {
    std::string p = g_cfg.javaPath;
    if (EndsWithNoCase(p, "javaw.exe")) {
        std::string cand = p.substr(0, p.size() - 9) + "java.exe";
        if (FileExists(fs::u8path(cand))) return cand;
    }
    return p;
}

static std::string TrimSlash(std::string s) {
    while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return s;
}
static bool FileExists(const fs::path& p) {
    std::error_code ec; return fs::exists(p, ec) && !fs::is_directory(p, ec);
}
static bool DirExists(const fs::path& p) {
    std::error_code ec; return fs::is_directory(p, ec);
}
static fs::path ExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

// 把编译进 exe 的 authlib-injector.jar 释放到磁盘
static std::string ExtractEmbeddedAuthlib() {
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

static void Log(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    g_logs.push_back(s);
    if (g_logs.size() > 5000) g_logs.erase(g_logs.begin(), g_logs.begin() + 1000);
}
static std::string LogTail(int n = 400) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    int start = (int)g_logs.size() > n ? (int)g_logs.size() - n : 0;
    std::string out;
    for (int i = start; i < (int)g_logs.size(); ++i) { out += g_logs[i]; out += "\r\n"; }
    return out;
}

// ============================ 配置 ============================
static void SaveConfig() {
    json j;
    j["gameDir"]       = g_cfg.gameDir;
    j["isoMode"] = g_cfg.isoMode;
    j["dlThreads"] = g_cfg.dlThreads;
    j["extraJvm"] = g_cfg.extraJvm;
    j["extraGame"] = g_cfg.extraGame;
    j["dlSource"] = g_cfg.dlSource;
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
static void LoadConfig() {
    std::ifstream f{ fs::u8path(g_cfgPath) };
    if (!f) return;
    try {
        json j; f >> j;
        g_cfg.gameDir        = j.value("gameDir", std::string());
    g_cfg.isoMode = j.value("isoMode", 0);
    g_cfg.dlThreads = j.value("dlThreads", 4);
    g_cfg.extraJvm = j.value("extraJvm", std::string());
    g_cfg.extraGame = j.value("extraGame", std::string());
    g_cfg.dlSource = j.value("dlSource", 0);
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

// ============================ 自动检测 ============================
static std::string DetectJava() {
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

static std::string DetectGameDir() {
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

static std::string DetectAuthlib() {
    std::vector<fs::path> cands;
    cands.push_back(ExeDir() / L"authlib-injector.jar");
    if (!g_cfg.gameDir.empty()) {
        fs::path g = fs::u8path(g_cfg.gameDir);
        cands.push_back(g / L"authlib-injector.jar");
        cands.push_back(g.parent_path() / L"authlib-injector.jar");
    }
    // 任意 authlib-injector-*.jar
    for (auto& c : cands) if (FileExists(c)) return WToU8(c.wstring());

    std::error_code ec;
    for (auto& d : { ExeDir(), g_cfg.gameDir.empty() ? fs::path() : fs::u8path(g_cfg.gameDir) }) {
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

// ============================ 版本 ============================
static void ScanVersions() {
    g_versions.clear();
    if (g_cfg.gameDir.empty()) return;

    fs::path verDir = fs::u8path(g_cfg.gameDir) / L"versions";
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

static json ReadJsonFile(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return json();
    try { json j; f >> j; return j; } catch (...) { return json(); }
}

static json ResolveVersion(const std::string& id, int depth = 0) {
    if (depth > 6) return json();
    fs::path base = fs::u8path(g_cfg.gameDir) / L"versions" / fs::u8path(id);
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

static bool RulesAllow(const json& rules) {
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

static bool ExtractZip(const fs::path& zip, const fs::path& outDir) {
    {
        std::error_code ecd;
        fs::create_directories(outDir, ecd);
        if (ecd) { Log("[警告] 无法创建解压目录: " + ecd.message()); return false; }
    }
    std::wstring cmd = L"tar.exe -xf \"" + zip.wstring() + L"\" -C \"" + outDir.wstring() + L"\"";
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 20000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return code == 0;
}

// ============================ 网络 ============================
static std::string Sha1Hex(const std::vector<unsigned char>& data) {
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

static std::string ToMirror(std::string url) {
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

// GET / POST 通用
static bool HttpRequest(const std::string& urlU8, const std::string& method,
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
    HINTERNET ses = WinHttpOpen(L"ReignLaunch/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { err = "WinHttpOpen 失败"; return false; }
    WinHttpSetTimeouts(ses, 15000, 15000, 30000, 60000);

    HINTERNET con = WinHttpConnect(ses, host, uc.nPort ? uc.nPort : (https ? 443 : 80), 0);
    if (!con) { WinHttpCloseHandle(ses); err = "连接失败"; return false; }

    std::wstring wmethod = U8ToW(method);
    HINTERNET req = WinHttpOpenRequest(con, wmethod.c_str(), path, nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
    if (!req) { WinHttpCloseHandle(con); WinHttpCloseHandle(ses); err = "请求创建失败"; return false; }

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
    WinHttpCloseHandle(ses);
    return ok;
}

static bool DownloadFile(const std::string& url, const fs::path& dest, std::string& err) {
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
    fs::rename(part, dest, ec);
    if (ec) { fs::remove(part, ec); err = "重命名失败"; return false; }
    return true;
}

// ============================ 第三方登录（Yggdrasil）============================
struct AuthResult {
    bool ok = false;
    std::string err;
    std::string accessToken, clientToken, uuid, name;
};

static AuthResult YggdrasilLogin(const std::string& serverRaw, const std::string& user, const std::string& pass) {
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

// ============================ 启动 ============================
struct LaunchPlan {
    std::wstring javaPath, commandLine, workDir;
    int maxMem = 4096;
};

static std::string ReplaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) { s.replace(pos, from.size(), to); pos += to.size(); }
    return s;
}
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
static std::string QuoteIfNeeded(const std::string& s) {
    return (s.find(' ') == std::string::npos) ? s : "\"" + s + "\"";
}
static std::string OfflineUUID(const std::string& name) {
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

// 判断实例是否算「模组实例」：有 mods 里的 jar，或版本 id / 主类带加载器名
static bool IsModdedInstance(const std::string& id, const fs::path& mcDir) {
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

static bool ShouldIsolate(const std::string& id, const fs::path& mcDir) {
    if (g_cfg.isoMode == 2) return false;
    if (!DirExists(mcDir / L"versions" / fs::u8path(id))) return false;
    if (g_cfg.isoMode == 1) return IsModdedInstance(id, mcDir);
    return true;
}
static bool BuildLaunchPlan(const std::string& versionId, LaunchPlan& plan, std::string& error) {
    fs::path mcDir = fs::u8path(g_cfg.gameDir);

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
    bool thirdParty = (g_cfg.authType == "thirdparty" && !g_cfg.authToken.empty());
    std::string userName = thirdParty ? g_cfg.authName : g_cfg.playerName;
    std::string userUuid = thirdParty ? g_cfg.authUuid : OfflineUUID(g_cfg.playerName);
    std::string userToken = thirdParty ? g_cfg.authToken : "0";
    std::string userType = thirdParty ? "mojang" : "offline";

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

// ============================ 下载 ============================
struct DownloadStats { int existed = 0, downloaded = 0, failed = 0; };

// ---------------- 多线程下载 ----------------
struct DLTask { std::string url; fs::path dest; std::string label; };

static void RunDownloads(std::vector<DLTask>& tasks, DownloadStats& st, int threads = 0) {
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
                size_t idx = next.fetch_add(1);
                if (idx >= tasks.size()) break;
                std::string err;
                if (DownloadFile(tasks[idx].url, tasks[idx].dest, err)) {
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

static void DownloadMissing(const std::string& versionId, DownloadStats& st) {
    fs::path mcDir = fs::u8path(g_cfg.gameDir);
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
            if (need) batch1.push_back({ url, dest, rel });
        }
    }

    fs::path clientJar = mcDir / L"versions" / fs::u8path(versionId) / fs::u8path(versionId + ".jar");
    if (vj.contains("downloads") && vj["downloads"].is_object() && vj["downloads"].contains("client")) {
        std::string url = vj["downloads"]["client"].value("url", std::string());
        if (!url.empty()) {
            if (FileExists(clientJar)) st.existed++;
            else batch1.push_back({ url, clientJar, versionId + ".jar" });
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
        if (!FileExists(idxPath)) {
            std::string err;
            if (DownloadFile(indexUrl, idxPath, err)) st.downloaded++;
            else st.failed++;
        } else st.existed++;

        json idx = ReadJsonFile(idxPath);
        if (idx.is_object() && idx.contains("objects") && idx["objects"].is_object()) {
            std::vector<DLTask> batch2;
            for (auto& it : idx["objects"].items()) {
                std::string hash = it.value().value("hash", std::string());
                if (hash.size() < 3) continue;
                fs::path obj = mcDir / L"assets" / L"objects" / fs::u8path(hash.substr(0, 2)) / fs::u8path(hash);
                if (FileExists(obj)) { st.existed++; continue; }
                batch2.push_back({ "https://resources.download.minecraft.net/" + hash.substr(0, 2) + "/" + hash, obj, hash });
            }
            RunDownloads(batch2, st);
        }
    }
}


// 启动前快速检查：只查文件是否存在，不做哈希校验（快）
static int CountMissing(const std::string& versionId) {
    fs::path mcDir = fs::u8path(g_cfg.gameDir);

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

// ============================ 启动线程 ============================
static HANDLE g_gameProc = nullptr;
static std::atomic<DWORD> g_gamePid{ 0 };

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

// 等游戏窗口真正出现（不是进程创建就算完成）
struct WinWaitCtx { DWORD pid; bool found; };

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
static void LaunchThread(std::string versionId) {
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
                Log("[错误] 该版本需要 Java " + std::to_string(need) + "，未检测到可用 Java");
                if (g_hwnd) PostMessageW(g_hwnd, WM_APP_NEEDJAVA, (WPARAM)need, 0);
                g_launching = false;
                g_progState = 0;
                return;
            }
        }
    }

    if (g_cfg.javaPath.empty() || !FileExists(fs::u8path(g_cfg.javaPath))) {
        Log("[错误] Java 路径无效");
        g_launching = false;
        PostMessageW(g_hwnd, WM_APP_ERROR, 0, 0);
        return;
    }

    g_stepCur = 1;   // 已通过 Java 检测
    if (g_cancelLaunch) { g_launching = false; g_progState = 0; return; }

    // 启动前自动检查缺失文件并补全
    {
        int miss = CountMissing(versionId);
        if (miss > 0) {
            g_progTotal = miss;
            g_progCur = 0;
            g_progState = 1;
            RebuildDlView();
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
        }
        if (miss < 0) {
            Log("[错误] 版本 JSON 无法解析，无法检查文件完整性");
        } else if (miss > 0) {
            Log("[补全] 检测到 " + std::to_string(miss) + " 个文件缺失，开始自动下载…");
            DownloadStats st;
            DownloadMissing(versionId, st);
            Log("[补全] 完成：新下载 " + std::to_string(st.downloaded) +
                "，失败 " + std::to_string(st.failed));
            g_progState = 0;
            if (st.failed > 0) {
                Log("[错误] 有文件下载失败，请检查网络后重试");
                g_launching = false;
                PostMessageW(g_hwnd, WM_APP_ERROR, 0, 0);
                return;
            }
        }
    }

    g_stepCur = 2;   // 依赖处理完
    if (g_cancelLaunch) { g_launching = false; g_progState = 0; return; }

    LaunchPlan plan;
    plan.maxMem = g_cfg.maxMemMB;
    std::string err;
    if (!BuildLaunchPlan(versionId, plan, err)) {
        Log("[错误] " + err);
        g_launching = false;
        PostMessageW(g_hwnd, WM_APP_ERROR, 0, 0);
        return;
    }

    g_stepCur = 3;   // 登录完成
    if (g_cancelLaunch) { g_launching = false; g_progState = 0; return; }

    SECURITY_ATTRIBUTES sa{ sizeof(sa) };
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        Log("[错误] 创建管道失败");
        g_launching = false;
        PostMessageW(g_hwnd, WM_APP_ERROR, 0, 0);
        return;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = wr; si.hStdError = wr; si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuf(plan.commandLine.begin(), plan.commandLine.end());
    cmdBuf.push_back(0);

    BOOL ok = CreateProcessW(plan.javaPath.c_str(), cmdBuf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, plan.workDir.c_str(), &si, &pi);
    CloseHandle(wr);

    if (!ok) {
        Log("[错误] 进程创建失败，GetLastError=" + std::to_string(GetLastError()));
        CloseHandle(rd);
        g_launching = false;
        PostMessageW(g_hwnd, WM_APP_ERROR, 0, 0);
        return;
    }

    g_progState = 0;
    Log("[成功] 游戏进程已创建，PID=" + std::to_string(pi.dwProcessId) + "，等待游戏窗口…");
    CloseHandle(pi.hThread);
    g_gameProc = pi.hProcess;
    g_gamePid = pi.dwProcessId;

    // 第四步「等待游戏启动」：等窗口真的出现才打勾
    {
        DWORD gamePid = pi.dwProcessId;
        std::thread waiter([gamePid]() {
            if (WaitForGameWindow(gamePid, 180000)) {
                g_stepCur = 4;
                RebuildDlView();
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
                Log("[成功] 游戏窗口已出现");
            } else {
                Log("[提示] 等待 180 秒仍未检测到游戏窗口（可能已崩溃，请看日志）");
            }
        });
        waiter.detach();
    }

    std::thread reader(PumpOutput, rd);
    reader.detach();

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    Log("=========== 游戏退出，退出码 " + std::to_string(code) + " ===========");
    CloseHandle(pi.hProcess);
    g_gameProc = nullptr;
    g_gamePid = 0;
    g_launching = false;

    if (code != 0) PostMessageW(g_hwnd, WM_APP_ERROR, 0, 0);
    else PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
}

// ============================ 错误窗口 ============================
static HWND g_hErrWnd = nullptr;
static HWND g_hErrEdit = nullptr;

static LRESULT CALLBACK ErrProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_hErrEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            12, 12, 656, 380, hwnd, (HMENU)9001, nullptr, nullptr);
        SendMessageW(g_hErrEdit, WM_SETFONT, (WPARAM)g_fUi, TRUE);
        CreateWindowW(L"BUTTON", L"复制日志", WS_CHILD | WS_VISIBLE, 470, 404, 96, 32, hwnd, (HMENU)9002, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"关闭", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 578, 404, 90, 32, hwnd, (HMENU)IDCANCEL, nullptr, nullptr);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDCANCEL) DestroyWindow(hwnd);
        else if (LOWORD(wp) == 9002) {
            int n = GetWindowTextLengthW(g_hErrEdit);
            std::wstring t(n > 0 ? n : 0, L'\0');
            if (n > 0) GetWindowTextW(g_hErrEdit, &t[0], n + 1);
            if (OpenClipboard(hwnd)) {
                EmptyClipboard();
                size_t bytes = (t.size() + 1) * sizeof(wchar_t);
                HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
                if (h) {
                    memcpy(GlobalLock(h), t.c_str(), bytes);
                    GlobalUnlock(h);
                    SetClipboardData(CF_UNICODETEXT, h);
                }
                CloseClipboard();
            }
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_hErrWnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================ 界面层 ============================
static std::atomic<bool> g_downloading{ false };
static bool g_uiTest = false;
static bool g_showDirList = false;
static int  g_settingsTab = 0;   // 设置页左侧菜单：0=设置 1=关于
static bool g_showJavaList = false;
static int Scale(int v) { return (int)(v * g_dpi / 96.0); }

struct Layout { RECT header{}, sidebar{}, main{}, content{}; };
static Layout g_ly;

static std::wstring       g_status;
static std::atomic<int>   g_loginState{ 0 };   // 0空闲 1进行中 2成功 3失败
static std::string        g_loginErr;

#define WM_APP_LOGIN (WM_APP + 3)

// 控件 ID
#define ID_TAB_LAUNCH   1001
#define ID_TAB_USER     1002
#define ID_TAB_SET      1003
#define ID_TAB_ABOUT    1004
#define ID_TAB_INST     1005
#define ID_TAB_DL       1006
#define ID_BTN_LAUNCH   1101
#define ID_BTN_FILL     1102
#define ID_BTN_REFRESH  1103
#define ID_ED_SERVER    1201
#define ID_ED_USER      1202
#define ID_ED_PASS      1203
#define ID_BTN_LOGIN    1204
#define ID_BTN_OFFLINE  1206
#define ID_BTN_LOGOUT   1205
#define ID_ED_DIR       1301
#define ID_ED_JAVA      1302
#define ID_ED_MEM       1303
#define ID_ED_NAME      1304
#define ID_BTN_AUTODIR  1311
#define ID_BTN_BROWSEDIR 1312
#define ID_BTN_AUTOJAVA 1313
#define ID_BTN_BROWSEJAVA 1314
#define ID_BTN_DIRDROP   1315
#define ID_BTN_JAVADROP  1316
#define ID_BTN_INSTARROW 1317
#define ID_BTN_OVCANCEL  1330
#define ID_BTN_SKIN      1340
#define ID_BTN_DELACC    1341
#define ID_ED_THREADS    1360
#define ID_ED_JVM        1361
#define ID_ED_GAME       1362
#define ID_BTN_SOURCE    1363
#define ID_BTN_FULL      1364
#define ID_BTN_UPDATE    1365
#define ID_DL_VANILLA    1370
#define ID_DL_FABRIC     1371
#define ID_DL_QUILT      1372
#define ID_DL_FORGE      1373
#define ID_DL_NEO        1374
#define ID_DL_FILTER0    1380
#define ID_DL_FILTER1    1381
#define ID_DL_FILTER2    1382
#define ID_DL_FILTER3    1383
#define ID_DL_INSTALL    1384
#define ID_DL_FAPI       1387
#define ID_DL_QSL        1388
#define ID_DL_FILTERSEL  1389
#define ID_ED_SEARCH     1385
#define ID_DL_BACK       1386
#define ID_BTN_SAVE     1320
#define ID_BTN_ISO      1321

static void FillRoundRect(HDC dc, RECT r, int radius, COLORREF color) {
    HRGN rgn = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1, radius, radius);
    HBRUSH br = CreateSolidBrush(color);
    FillRgn(dc, rgn, br);
    DeleteObject(br);
    DeleteObject(rgn);
}
static void DrawTextIn(HDC dc, const std::wstring& s, RECT r, COLORREF c, HFONT f,
                       UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, fmt | DT_NOPREFIX);
}
static std::wstring CtlText(HWND h) {
    int n = GetWindowTextLengthW(h);
    if (n <= 0) return L"";
    std::wstring s(n + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize(n);
    return s;
}

static HWND MakeBtn(HWND parent, const wchar_t* text, int id) {
    return CreateWindowW(L"BUTTON", text, WS_CHILD | BS_OWNERDRAW,
                         0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
}
static HWND MakeEdit(HWND parent, int id, const wchar_t* text, DWORD extra = 0) {
    HWND h = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text,
                             WS_CHILD | ES_AUTOHSCROLL | extra,
                             0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_fUi, TRUE);
    SendMessageW(h, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 6));
    return h;
}

// ---------------- 布局 ----------------
static void ComputeLayout(int w, int h) {
    int headerH = Scale(64);
    int sideW   = Scale(300);
    g_ly.header  = { 0, 0, w, headerH };
    g_ly.sidebar = { 0, headerH, sideW, h };
    g_ly.main    = { sideW, headerH, w, h };
    g_ly.content = { 0, headerH, w, h };
}

static RECT TabRect(int i) {
    int x = Scale(220) + i * Scale(96);
    int y = Scale(14);
    RECT r = { x, y, x + Scale(88), y + Scale(36) };
    return r;
}
static RECT LaunchBtnRect() {
    int pad = Scale(24), bw = Scale(260), bh = Scale(64);
    RECT r;
    r.right = g_ly.main.right - pad - Scale(52);
    r.left = r.right - bw;
    r.bottom = g_ly.main.bottom - pad;
    r.top = r.bottom - bh;
    return r;
}
static RECT SmallBtnRect(int idx) {   // 0=补全文件 1=刷新
    RECT lb = LaunchBtnRect();
    int w = Scale(120), h = Scale(36), gap = Scale(12);
    RECT r;
    r.right = lb.right - idx * (w + gap);
    r.left = r.right - w;
    r.bottom = lb.top - Scale(16);
    r.top = r.bottom - h;
    return r;
}

static void SettingsGeom(int row, RECT& ed, RECT& drop, RECT& autoB, RECT& browse);

static const wchar_t* IsoLabel() {
    switch (g_cfg.isoMode) {
    case 0: return L"隔离所有实例";
    case 1: return L"隔离模组实例";
    default: return L"不隔离实例";
    }
}
static const wchar_t* WinModeLabel(int m) {
    switch (m) {
    case 1:  return L"最大化";
    case 2:  return L"全屏";
    default: return L"窗口";
    }
}
static void LayoutControls() {
    int w = g_ly.content.right, h = g_ly.content.bottom;

    // 顶部标签
    HWND tabBtns[5] = { hBtnTabLaunch, hBtnTabDl, hBtnTabInst, hBtnTabUser, hBtnTabSettings };
    for (int i = 0; i < 5; ++i) {
        RECT r = TabRect(i);
        MoveWindow(tabBtns[i], r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
    }

    // 启动页
    {
        RECT b = LaunchBtnRect();
        MoveWindow(hBtnLaunch, b.left, b.top, b.right - b.left, b.bottom - b.top, TRUE);
        MoveWindow(hBtnInstArrow, b.right + Scale(8), b.top, Scale(44), b.bottom - b.top, TRUE);

        RECT ocr = OverlayCancelRect();
        MoveWindow(hBtnOverlayCancel, ocr.left, ocr.top, ocr.right - ocr.left, ocr.bottom - ocr.top, TRUE);
        RECT s0 = SmallBtnRect(0), s1 = SmallBtnRect(1);
        MoveWindow(hBtnRefresh, g_ly.content.right - Scale(134), g_ly.content.top + Scale(12), Scale(110), Scale(32), TRUE);
    }

    // 用户页：居中面板
    {
        int panelW = Scale(620);
        int x = g_ly.main.left + (g_ly.main.right - g_ly.main.left - panelW) / 2;
        if (x < g_ly.main.left + Scale(20)) x = g_ly.main.left + Scale(20);
        int y = g_ly.content.top + Scale(60);
        int rowH = Scale(58);
        int edW = Scale(380), edH = Scale(30);

        MoveWindow(hEdServer, x + Scale(110), y, edW, edH, TRUE);
        MoveWindow(hEdUser,   x + Scale(110), y + rowH, edW, edH, TRUE);
        MoveWindow(hEdPass,   x + Scale(110), y + rowH * 2, edW, edH, TRUE);
        MoveWindow(hBtnLogin, x + Scale(110), y + rowH * 3, Scale(120), Scale(36), TRUE);
        MoveWindow(hBtnLogout, x + Scale(246), y + rowH * 3, Scale(140), Scale(36), TRUE);
        MoveWindow(hEdName, x + Scale(110), y + rowH * 4 + Scale(16), Scale(240), edH, TRUE);
        MoveWindow(hBtnOffline, x + Scale(366), y + rowH * 4 + Scale(14), Scale(110), Scale(34), TRUE);

        RECT sb = g_ly.sidebar;
        MoveWindow(hBtnSkin, sb.left + Scale(12), sb.bottom - Scale(92), (sb.right - sb.left) - Scale(24), Scale(36), TRUE);
        MoveWindow(hBtnDelAcc, sb.left + Scale(12), sb.bottom - Scale(50), (sb.right - sb.left) - Scale(24), Scale(36), TRUE);
    }

    // 设置页
    {
        int x = g_ly.main.left + Scale(40);
        int y = g_ly.content.top + Scale(50);
        int rowH = Scale(62);
        int edW = Scale(420), edH = Scale(30);
        int bx = x + Scale(110) + edW + Scale(12);

        RECT e0, d0, a0, b0, e1, d1, a1, b1;
        SettingsGeom(0, e0, d0, a0, b0);
        SettingsGeom(1, e1, d1, a1, b1);

        MoveWindow(hEdDir,        e0.left, e0.top, e0.right - e0.left, e0.bottom - e0.top, TRUE);
        MoveWindow(hBtnDirDrop,   d0.left, d0.top, d0.right - d0.left, d0.bottom - d0.top, TRUE);
        MoveWindow(hBtnAutoDir,   a0.left, a0.top, a0.right - a0.left, a0.bottom - a0.top, TRUE);
        MoveWindow(hBtnBrowseDir, b0.left, b0.top, b0.right - b0.left, b0.bottom - b0.top, TRUE);

        MoveWindow(hEdJava,        e1.left, e1.top, e1.right - e1.left, e1.bottom - e1.top, TRUE);
        MoveWindow(hBtnJavaDrop,   d1.left, d1.top, d1.right - d1.left, d1.bottom - d1.top, TRUE);
        MoveWindow(hBtnAutoJava,   a1.left, a1.top, a1.right - a1.left, a1.bottom - a1.top, TRUE);
        MoveWindow(hBtnBrowseJava, b1.left, b1.top, b1.right - b1.left, b1.bottom - b1.top, TRUE);

        MoveWindow(hEdMem,  x + Scale(110), y + rowH * 2, Scale(110), edH, TRUE);

        MoveWindow(hBtnSave, x + Scale(110), y + rowH * 7 + Scale(10), Scale(140), Scale(40), TRUE);
        MoveWindow(hBtnIso,  x + Scale(270), y + rowH * 7 + Scale(10), Scale(240), Scale(40), TRUE);
        MoveWindow(hEdThreads, x + Scale(110), y + rowH * 3 + Scale(6), Scale(90), Scale(30), TRUE);
        MoveWindow(hEdJvm,  x + Scale(110), y + rowH * 4 + Scale(6), Scale(560), Scale(30), TRUE);
        MoveWindow(hEdGame, x + Scale(110), y + rowH * 5 + Scale(6), Scale(560), Scale(30), TRUE);
        MoveWindow(hBtnSource, x + Scale(110), y + rowH * 6 + Scale(4), Scale(200), Scale(36), TRUE);
        MoveWindow(hBtnFull,   x + Scale(322), y + rowH * 6 + Scale(4), Scale(230), Scale(36), TRUE);
        MoveWindow(hBtnUpdate, g_ly.main.left + Scale(40), g_ly.content.bottom - Scale(70), Scale(140), Scale(38), TRUE);

        {
            int tx0 = g_ly.content.left + Scale(24);
            int ty0 = g_ly.content.top + Scale(16);
            MoveWindow(hBtnDlFilterSel, tx0, ty0, Scale(150), Scale(32), TRUE);
            MoveWindow(hEdSearch, g_ly.content.right - Scale(24) - Scale(240), ty0, Scale(240), Scale(30), TRUE);

            int dby = g_ly.content.top + Scale(64);
            int dbw = Scale(102), dbg = Scale(6);
            int dbx = g_ly.content.left + Scale(24);
            MoveWindow(hBtnDlVanilla, dbx, dby, dbw, Scale(32), TRUE); dbx += dbw + dbg;
            MoveWindow(hBtnDlFabric,  dbx, dby, dbw, Scale(32), TRUE); dbx += dbw + dbg;
            MoveWindow(hBtnDlQuilt,   dbx, dby, dbw, Scale(32), TRUE); dbx += dbw + dbg;
            MoveWindow(hBtnDlForge,   dbx, dby, dbw, Scale(32), TRUE); dbx += dbw + dbg;
            MoveWindow(hBtnDlNeo,     dbx, dby, dbw, Scale(32), TRUE); dbx += dbw + dbg;
            MoveWindow(hBtnDlFapi,    dbx, dby, Scale(122), Scale(32), TRUE); dbx += Scale(128);
            MoveWindow(hBtnDlQsl,     dbx, dby, Scale(70), Scale(32), TRUE);
            MoveWindow(hBtnDlInstall, g_ly.content.right - Scale(24) - Scale(130),
                       g_ly.content.bottom - Scale(58), Scale(130), Scale(38), TRUE);
            MoveWindow(hBtnDlBack, g_ly.content.left + Scale(24), g_ly.content.bottom - Scale(58), Scale(100), Scale(32), TRUE);

        }
    }
}

static void SyncSettingsVisibility();

static void SyncDownloadControls() {
    bool onDl   = (g_page == PAGE_DOWNLOAD);
    int showDl  = (onDl && !g_dlDetail) ? SW_SHOW : SW_HIDE;
    int showDet = (onDl &&  g_dlDetail) ? SW_SHOW : SW_HIDE;
    ShowWindow(hBtnDlBack, showDet);
    ShowWindow(hBtnDlInstall, showDet);
    ShowWindow(hBtnDlFapi, showDet);
    ShowWindow(hBtnDlQsl,  showDet);
    ShowWindow(hBtnDlFilterSel, showDl);
    ShowWindow(hBtnDlF0, SW_HIDE);
    ShowWindow(hBtnDlF1, SW_HIDE);
    ShowWindow(hBtnDlF2, SW_HIDE);
    ShowWindow(hBtnDlF3, SW_HIDE);
    ShowWindow(hEdSearch, showDl);
    ShowWindow(hBtnDlVanilla, showDet);
    ShowWindow(hBtnDlFabric,  showDet);
    ShowWindow(hBtnDlQuilt,   showDet);
    ShowWindow(hBtnDlForge,   showDet);
    ShowWindow(hBtnDlNeo,     showDet);

}

static void ShowPage(Page p) {
    g_page = p;
    bool L = (p == PAGE_LAUNCH), U = (p == PAGE_USER), S = (p == PAGE_SETTINGS);
    if (p != PAGE_SETTINGS) { g_showDirList = false; g_showJavaList = false; }
    if (p == PAGE_DOWNLOAD && g_mcVersState.load() == 0)
        std::thread(FetchManifestThread).detach();

    // 顶部标签必须始终可见，否则切不到别的页
    ShowWindow(hBtnTabLaunch, SW_SHOW);
    ShowWindow(hBtnTabUser, SW_SHOW);
    ShowWindow(hBtnTabSettings, SW_SHOW);
    ShowWindow(hBtnTabInst, SW_SHOW);
    ShowWindow(hBtnTabDl, SW_SHOW);
        SyncDownloadControls();



    ApplyOverlayVisibility();

    ShowWindow(hEdServer, U); ShowWindow(hEdUser, U); ShowWindow(hEdPass, U);
    ShowWindow(hBtnLogin, U); ShowWindow(hBtnLogout, U); ShowWindow(hEdName, U); ShowWindow(hBtnOffline, U); ShowWindow(hBtnSkin, U); ShowWindow(hBtnDelAcc, U);

    ShowWindow(hEdDir, S); ShowWindow(hBtnAutoDir, S); ShowWindow(hBtnBrowseDir, S); ShowWindow(hBtnDirDrop, S);
    ShowWindow(hEdJava, S); ShowWindow(hBtnAutoJava, S); ShowWindow(hBtnBrowseJava, S); ShowWindow(hBtnJavaDrop, S);
    ShowWindow(hEdMem, S); ShowWindow(hBtnSave, S); ShowWindow(hBtnIso, S);

    LayoutControls();
    SyncSettingsVisibility();
    InvalidateRect(g_hwnd, nullptr, TRUE);
}

// ---------------- 绘制 ----------------
// ============================ 皮肤 ============================
static void LoadSkinBitmap() {
    if (g_skinBmp) { DeleteObject(g_skinBmp); g_skinBmp = nullptr; }
    if (!g_gpOk || g_cfg.skinPath.empty()) return;
    std::wstring wp = U8ToW(g_cfg.skinPath);
    Gdiplus::Bitmap bmp(wp.c_str());
    if (bmp.GetLastStatus() != Gdiplus::Ok) return;
    HBITMAP hb = nullptr;
    if (bmp.GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &hb) == Gdiplus::Ok) g_skinBmp = hb;
}

// 画头像（取 64x64 皮肤的头部正面 8x8 区域）
static void DrawAvatar(HDC dc, int x, int y, int size) {
    if (g_skinBmp) {
        HDC src = CreateCompatibleDC(dc);
        HBITMAP old = (HBITMAP)SelectObject(src, g_skinBmp);
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchBlt(dc, x, y, size, size, src, 8, 8, 8, 8, SRCCOPY);
        SelectObject(src, old);
        DeleteDC(src);
    } else {
        RECT r = { x, y, x + size, y + size };
        HBRUSH b = CreateSolidBrush(RGB(72, 76, 86));
        FillRect(dc, &r, b);
        DeleteObject(b);
    }
}

static std::string CurrentPlayerName() {
    return (g_cfg.authType == "thirdparty" && !g_cfg.authName.empty()) ? g_cfg.authName : g_cfg.playerName;
}

// 应用皮肤：校验尺寸 -> 存到配置目录 -> 写入 CustomSkinLoader 本地皮肤
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
        fs::path csl = fs::u8path(g_cfg.gameDir) / L"CustomSkinLoader" / L"LocalSkin" / L"textures";
        fs::create_directories(csl, ec);
        fs::copy_file(local, csl / fs::u8path(player + ".png"), fs::copy_options::overwrite_existing, ec);
    }

    g_cfg.skinPath = WToU8(local.wstring());
    SaveConfig();
    LoadSkinBitmap();
    return true;
}
// ---- 账户切换 ----
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

static void CaptureActiveAccount() {
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

// 找一个同名账户，找不到就新建；返回下标
static int UpsertAccount(const std::string& type, const std::string& name,
                         const std::string& server = "", const std::string& token = "",
                         const std::string& uuid = "") {
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
// ---------------- 检测候选列表 ----------------
struct Cand { std::string path; std::string label; };
static std::vector<Cand> g_dirCands, g_javaCands;


static void AddDirCand(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_directory(p / L"versions", ec)) return;
    std::string s = WToU8(p.wstring());
    for (auto& c : g_dirCands) if (c.path == s) return;
    g_dirCands.push_back({ s, s });
}

static void RefreshDirCands() {
    g_dirCands.clear();
    wchar_t appdata[MAX_PATH] = {0}, profile[MAX_PATH] = {0};
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata);
    SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, profile);

    if (!g_cfg.gameDir.empty()) AddDirCand(fs::u8path(g_cfg.gameDir));
    AddDirCand(fs::path(appdata) / L".minecraft");
    AddDirCand(fs::path(profile) / L".minecraft");
    for (const wchar_t* drv : { L"C:", L"D:", L"E:" }) {
        AddDirCand(fs::path(std::wstring(drv) + L"\\MC\\.minecraft"));
        AddDirCand(fs::path(std::wstring(drv) + L"\\Games\\.minecraft"));
        AddDirCand(fs::path(std::wstring(drv) + L"\\.minecraft"));
    }
}

// 从 JDK 的 release 文件读版本号，不用启动 java.exe
static std::string JavaVersionOf(const fs::path& javaw) {
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

static void AddJavaCand(const fs::path& javaw) {
    if (!FileExists(javaw)) return;
    std::string s = WToU8(javaw.wstring());
    for (auto& c : g_javaCands) if (c.path == s) return;
    std::string v = JavaVersionOf(javaw);
    g_javaCands.push_back({ s, v.empty() ? "版本未知" : ("Java " + v) });
}

static void RefreshJavaCands() {
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

// ============================ Java 版本匹配 ============================
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

// 该版本需要哪个大版本 Java
static int RequiredJavaMajor(const std::string& id) {
    fs::path base = fs::u8path(g_cfg.gameDir) / L"versions" / fs::u8path(id);
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

// 在候选里挑一个满足要求的 Java，返回路径；找不到返回空
static std::string PickJavaFor(int need) {
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
// ---------------- 设置页几何 ----------------
static void SettingsGeom(int row, RECT& ed, RECT& drop, RECT& autoB, RECT& browse) {
    int x  = g_ly.main.left + Scale(40);
    int y  = g_ly.content.top + Scale(50) + row * Scale(62);
    int ew = Scale(330), eh = Scale(30);
    ed = { x + Scale(110), y, x + Scale(110) + ew, y + eh };
    int bx = ed.right + Scale(10);
    drop   = { bx, y - Scale(2), bx + Scale(34), y + Scale(32) };
    autoB  = { bx + Scale(40), y - Scale(2), bx + Scale(40) + Scale(90), y + Scale(32) };
    browse = { bx + Scale(136), y - Scale(2), bx + Scale(136) + Scale(70), y + Scale(32) };
}

static RECT CandListRect(int row, int count) {
    RECT ed, drop, a, b;
    SettingsGeom(row, ed, drop, a, b);
    int h = Scale(28) * count + Scale(8);
    RECT r = { ed.left, ed.bottom + Scale(4), b.right, ed.bottom + Scale(4) + h };
    return r;
}

// 下拉打开时，把会被列表盖住的控件先藏起来（子控件永远画在父窗口绘制之上）
static void SyncSettingsVisibility() {
    bool onSettings = (g_page == PAGE_SETTINGS) && (g_settingsTab == 0);
    if (!hEdMem) return;

    ShowWindow(hEdDir,        onSettings ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnDirDrop,   onSettings ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnAutoDir,   onSettings ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnBrowseDir, onSettings ? SW_SHOW : SW_HIDE);
    ShowWindow(hEdJava,        onSettings ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnJavaDrop,   onSettings ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnAutoJava,   onSettings ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnBrowseJava, onSettings ? SW_SHOW : SW_HIDE);
    ShowWindow(hEdMem,   onSettings ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnSave, onSettings ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnIso,  onSettings ? SW_SHOW : SW_HIDE);
    ShowWindow(hEdThreads, onSettings ? SW_SHOW : SW_HIDE);
    ShowWindow(hEdJvm,  onSettings ? SW_SHOW : SW_HIDE);
    ShowWindow(hEdGame, onSettings ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnSource, onSettings ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnFull,   onSettings ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnUpdate, (g_page == PAGE_SETTINGS && g_settingsTab == 1) ? SW_SHOW : SW_HIDE);
}
static void DrawCandList(HDC dc, bool isDir) {
    const std::vector<Cand>& list = isDir ? g_dirCands : g_javaCands;
    if (list.empty()) return;

    int row = isDir ? 0 : 1;
    RECT r = CandListRect(row, (int)list.size());
    FillRoundRect(dc, r, 6, C_CARD_SEL);

    int itemH = Scale(28);
    int y = r.top + Scale(4);
    for (size_t i = 0; i < list.size(); ++i) {
        RECT it = { r.left + Scale(8), y, r.right - Scale(8), y + itemH };
        std::wstring txt = isDir
            ? U8ToW(list[i].path)
            : (U8ToW(list[i].label) + L"    " + U8ToW(list[i].path));
        DrawTextIn(dc, txt, it, C_TEXT, g_fSmall);
        y += itemH;
    }
}

// 关于信息块（关于页和设置页共用）
static void DrawAboutBlock(HDC dc) {
    int x  = g_ly.main.left + Scale(40);
    int y  = g_ly.content.top + Scale(50);
    int lw = Scale(110);

    RECT k1 = { x, y, x + lw, y + Scale(34) };
    DrawTextIn(dc, L"关于", k1, C_DIM, g_fUi);

    int iconSz = Scale(64);
    HICON ic = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, iconSz, iconSz, LR_DEFAULTCOLOR);
    if (ic) {
        DrawIconEx(dc, x + lw, y - Scale(12), ic, iconSz, iconSz, 0, nullptr, DI_NORMAL);
        DestroyIcon(ic);
    }

    int tx = x + lw + iconSz + Scale(14);
    RECT t1 = { tx, y - Scale(6), g_ly.main.right - Scale(40), y + Scale(30) };
    DrawTextIn(dc, L"ReignLaunch", t1, C_TEXT, g_fTitle);

    RECT t2 = { tx, y + Scale(30), g_ly.main.right - Scale(40), y + Scale(54) };
    DrawTextIn(dc, L"Minecraft 启动器", t2, C_DIM, g_fUi);

    struct AboutRow { const wchar_t* k; const wchar_t* v; };
    const AboutRow rows[] = {
        { L"版本",   L"v1.1.0 Beta 3.5" },
        { L"制作人", L"加纳Brightchaserove（抖音）" },
        { L"技术栈", L"C++17 / Win32 GDI / nlohmann-json / WinHTTP" },
        { L"登录",   L"离线模式、第三方 Yggdrasil（authlib-injector 已内置）" },
        { L"下载",   L"Mojang 官方源 + BMCLAPI 镜像，SHA1 校验" },
        { L"定位",   L"实例化管理、启动前自动补全、出错才弹日志窗口" },
    };

    int ry = y + Scale(84);
    for (const auto& r : rows) {
        RECT kk = { x, ry, x + lw, ry + Scale(28) };
        RECT vv = { x + lw, ry, g_ly.main.right - Scale(40), ry + Scale(28) };
        DrawTextIn(dc, r.k, kk, C_DIM, g_fUi);
        DrawTextIn(dc, r.v, vv, C_TEXT, g_fUi);
        ry += Scale(38);
    }

    RECT footer = { x + lw, ry + Scale(20), g_ly.main.right - Scale(40), g_ly.content.bottom - Scale(20) };
    DrawTextIn(dc, L"本程序仅用于学习交流，与 Mojang Studios / 微软无关。",
               footer, C_DIM, g_fSmall, DT_LEFT | DT_TOP | DT_WORDBREAK);
}

// 设置页左侧子菜单
static void DrawSettingsMenu(HDC dc) {
    RECT sb = g_ly.sidebar;
    HBRUSH sbb = CreateSolidBrush(RGB(30, 31, 35));
    FillRect(dc, &sb, sbb);
    DeleteObject(sbb);

    RECT lbl = { sb.left + Scale(20), sb.top + Scale(14), sb.right, sb.top + Scale(44) };
    DrawTextIn(dc, L"设置", lbl, C_DIM, g_fSmall);

    const wchar_t* items[] = { L"设置", L"关于" };
    int itemH = Scale(46);
    int y = sb.top + Scale(50);
    for (int i = 0; i < 2; ++i) {
        RECT it = { sb.left + Scale(12), y, sb.right - Scale(12), y + itemH - Scale(8) };
        bool sel = (g_settingsTab == i);
        FillRoundRect(dc, it, 8, sel ? C_CARD_SEL : C_CARD);
        RECT tx = it; tx.left += Scale(14);
        DrawTextIn(dc, items[i], tx, sel ? C_TEXT : C_DIM, g_fUi);
        if (sel) {
            RECT bar = { it.left, it.top + Scale(8), it.left + Scale(3), it.bottom - Scale(8) };
            HBRUSH b = CreateSolidBrush(C_ACCENT2);
            FillRect(dc, &bar, b);
            DeleteObject(b);
        }
        y += itemH;
    }
}

// ============================ 原版下载 ============================


static void FetchManifestThread() {
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
            g_mcVers = list;
            g_mcVersState = 2;
        } else g_mcVersState = 3;
    } else g_mcVersState = 3;
    RebuildDlView();
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
}

// 安装：下载版本 json → 自动补全客户端/依赖/资源
static void InstallVersionThread(std::string id, std::string url) {
    g_progState = 0;
    g_progCur = 0;
    g_progTotal = 0;
    g_status = L"正在下载版本 JSON…";

    fs::path dir = fs::u8path(g_cfg.gameDir) / L"versions" / fs::u8path(id);
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

    if (g_hwnd) {
        g_status = L"安装完成：" + U8ToW(id) + L"（下载 " + std::to_wstring(st.downloaded) + L" 个文件）";
        ScanVersions();
        PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
    }
}
// ---------------- 详情页：加载器版本列表 ----------------
static void FetchLoaderVersThread() {
    int kind = g_dlLoader;
    g_ldVersState = 1;
    g_ldVers.clear();
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
    g_ldFiles.clear();
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
                if (!v.empty()) { g_ldVers.push_back(v); g_ldFiles.push_back(fu + "\n" + fn); }
                continue;
            }
            if (kind == 1 || kind == 2) v = x["loader"].value("version", std::string());
            else v = x.value("version", std::string());
            if (!v.empty()) g_ldVers.push_back(v);
        }
    }
    g_ldVersState = 2;   // 2=成功（空列表由绘制层显示"没有可下载的版本"）
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
}
// ---------------- 详情页：加载器可用性 ----------------
static void FetchLoadersThread() {
    std::string mc = g_dlDetailMc;
    for (int i = 0; i < 4; ++i) { g_ldState[i] = 0; g_ldVer[i].clear(); }

    auto getJson = [](const std::string& url) {
        std::vector<unsigned char> d;
        std::string e;
        if (!HttpRequest(url, "GET", "", "", d, e)) return json();
        return json::parse(std::string(d.begin(), d.end()), nullptr, false);
    };

    // 0=Fabric 1=Quilt 2=Forge 3=NeoForge
    {
        json j = getJson("https://bmclapi2.bangbang93.com/fabric-meta/v2/versions/loader/" + mc);
        if (j.is_array() && !j.empty()) {
            std::string v = j[0]["loader"].value("version", std::string());
            if (!v.empty()) { g_ldVer[0] = U8ToW(v); g_ldState[0] = 1; } else g_ldState[0] = 2;
        } else g_ldState[0] = 2;
    }
    {
        json j = getJson("https://meta.quiltmc.org/v3/versions/loader/" + mc);
        if (j.is_array() && !j.empty()) {
            std::string v = j[0]["loader"].value("version", std::string());
            if (!v.empty()) { g_ldVer[1] = U8ToW(v); g_ldState[1] = 1; } else g_ldState[1] = 2;
        } else g_ldState[1] = 2;
    }
    {
        json j = getJson("https://bmclapi2.bangbang93.com/forge/minecraft/" + mc);
        if (j.is_array() && !j.empty()) {
            std::string v;
            for (auto& x : j) {
                std::string vv = x.value("version", std::string());
                if (vv.empty()) continue;
                v = vv;
                if (x.value("branch", std::string()) == "recommended") break;
            }
            if (!v.empty()) { g_ldVer[2] = U8ToW(v); g_ldState[2] = 1; } else g_ldState[2] = 2;
        } else g_ldState[2] = 2;
    }
    {
        json j = getJson("https://bmclapi2.bangbang93.com/neoforge/list/" + mc);
        if (j.is_array() && !j.empty()) {
            std::string v = j[0].value("version", std::string());
            if (!v.empty()) { g_ldVer[3] = U8ToW(v); g_ldState[3] = 1; } else g_ldState[3] = 2;
        } else g_ldState[3] = 2;
    }

    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
}
// ---------------- 下载页过滤 ----------------
static bool IsAprilFools(const McVer& v) {
    if (v.type == "release") return false;   // 正式版不算愚人节
    if (v.date.size() >= 10 && v.date.substr(5, 2) == "04" && v.date.substr(8, 2) == "01") return true;
    static const char* ids[] = { "15w14a", "1.rv-pre1", "2.0", "3D Shareware v1.34",
                                 "20w14infinite", "22w13oneblockatatime", "23w13a_or_b" };
    for (auto id : ids) if (v.id == id) return true;
    return false;
}

static void RebuildDlView() {
    g_dlView.clear();
    std::wstring kw;
    {
        wchar_t buf[256] = { 0 };
        if (hEdSearch) GetWindowTextW(hEdSearch, buf, 256);
        kw = buf;
        for (auto& c : kw) c = (wchar_t)towlower(c);
    }

    for (size_t i = 0; i < g_mcVers.size(); ++i) {
        const McVer& v = g_mcVers[i];
        bool april = IsAprilFools(v);
        bool isRelease = (v.type == "release");
        bool isSnap    = (v.type == "snapshot");
        bool isOld     = (v.type == "old_beta" || v.type == "old_alpha");

        bool pass = false;
        switch (g_dlFilter) {
        case 0: pass = isRelease; break;
        case 1: pass = (isSnap && !april); break;
        case 2: pass = isOld; break;
        default: pass = april; break;
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
// ---------------- 模组加载器安装 ----------------


static std::string HttpGetString(const std::string& url, std::string& err) {
    std::vector<unsigned char> data;
    if (!HttpRequest(url, "GET", "", "", data, err)) return std::string();
    return std::string(data.begin(), data.end());
}

// Fabric / Quilt：拿 loader 列表 -> profile json -> 落到 versions/<id>
static void InstallMetaLoaderThread(std::string mc, bool quilt, std::string wantVer = "") {
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

    fs::path dir = fs::u8path(g_cfg.gameDir) / L"versions" / fs::u8path(vid);
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::ofstream f{ dir / fs::u8path(vid + ".json"), std::ios::binary | std::ios::trunc };
    f << prof;
    f.close();

    g_status = L"正在补全加载器依赖…";
    DownloadStats st;
    DownloadMissing(vid, st);
    g_progState = 0;
    if (g_hwnd) {
        ScanVersions();
        g_status = L"安装完成：" + U8ToW(vid) + L"（下载 " + std::to_wstring(st.downloaded) + L" 个文件）";
        PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
    }
}

// Forge / NeoForge：下载 installer 并执行 --installClient
static void InstallInstallerThread(std::string mc, bool neo, std::string wantVer = "") {
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

    if (g_hwnd) {
        ScanVersions();
        g_status = (code == 0) ? (L"安装完成：" + U8ToW(ver))
                               : (L"安装器退出码 " + std::to_wstring(code) + L"，请查看日志");
        PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
    }
}
// ============================ 自动更新 ============================
#define APP_TAG L"v1.1.0-beta3.5"

static std::wstring    g_newTag;
static std::string     g_newUrl;
static std::atomic<int> g_updateState{ 0 };   // 0未查 1查中 2有新版 3已最新 4失败
static std::atomic<bool> g_updating{ false };

// a 是否比 b 新
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
                g_updateState = TagNewer(g_newTag, APP_TAG) ? 2 : 3;
                break;
            }
            if (g_updateState == 1) g_updateState = 3;
        } else g_updateState = 4;
    } else {
        g_updateState = 4;
    }
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_UPD, interactive ? 1 : 0, 0);
}

// 下载新版并写一个批处理：等本进程退出 -> 覆盖 exe -> 启动新版
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
// ============================ 启动浮层 ============================
static void ApplyOverlayVisibility() {
    bool L = (g_page == PAGE_LAUNCH) && !g_overlay;
    ShowWindow(hBtnLaunch, L ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnInstArrow, L ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnRefresh, (g_page == PAGE_INSTANCES && !g_overlay) ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnOverlayCancel, g_overlay ? SW_SHOW : SW_HIDE);
}
static RECT OverlayCancelRect() {
    RECT c = g_ly.content;
    int w = Scale(96), h = Scale(34);
    return { c.right - Scale(28) - w, c.bottom - Scale(62), c.right - Scale(28), c.bottom - Scale(28) };
}

// 颜色线性插值
static COLORREF MixColor(COLORREF a, COLORREF b, double t) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int r = (int)(GetRValue(a) * (1 - t) + GetRValue(b) * t);
    int g = (int)(GetGValue(a) * (1 - t) + GetGValue(b) * t);
    int bl = (int)(GetBValue(a) * (1 - t) + GetBValue(b) * t);
    return RGB(r, g, bl);
}

static void FillLine(HDC dc, int left, int right, int y, COLORREF col) {
    RECT line = { left, y, right, y + 1 };
    HBRUSH b = CreateSolidBrush(col);
    FillRect(dc, &line, b);
    DeleteObject(b);
}

// 沉浸式启动画面：背景图/深灰渐变 + 底部 200px 渐变压暗 + 左下角状态 + 贴底进度条
static void DrawOverlay(HDC dc) {
    RECT c = g_ly.content;
    int h = c.bottom - c.top;
    if (h <= 0) return;

    int done  = g_stepCur.load();
    int total = g_progTotal.load(), cur = g_progCur.load();
    bool downloading = (g_progState.load() == 1 && total > 0);
    double ratio = downloading ? ((double)cur / total) : (done / 4.0);
    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;

    // ---------- 1. 背景：优先用资源位图 102，没有就深灰渐变 ----------
    bool painted = false;
    {
        HINSTANCE hi = GetModuleHandleW(nullptr);
        HBITMAP bg = (HBITMAP)LoadImageA(hi, MAKEINTRESOURCEA(102), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
        if (bg) {
            BITMAP bm{};
            GetObject(bg, sizeof(bm), &bm);
            HDC src = CreateCompatibleDC(dc);
            HBITMAP oldb = (HBITMAP)SelectObject(src, bg);
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchBlt(dc, c.left, c.top, c.right - c.left, h, src, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
            SelectObject(src, oldb);
            DeleteDC(src);
            DeleteObject(bg);
            painted = true;
        }
    }
    if (!painted) {
        const COLORREF cTop = RGB(30, 32, 38), cBottom = RGB(10, 11, 14);
        for (int y = c.top; y < c.bottom; ++y) {
            double t = (double)(y - c.top) / h;
            FillLine(dc, c.left, c.right, y, MixColor(cTop, cBottom, t));
        }
    }

    // ---------- 2. 底部 200px 渐变压暗（透明 -> 近黑）----------
    int gH = Scale(200);
    if (gH > h) gH = h;
    for (int i = 0; i < gH; ++i) {
        int y = c.bottom - gH + i;
        double t = (double)(y - c.top) / h;
        double a = (double)i / gH * 0.95;
        COLORREF base = MixColor(RGB(30, 32, 38), RGB(10, 11, 14), t);
        FillLine(dc, c.left, c.right, y, MixColor(base, RGB(0, 0, 0), a));
    }

    // ---------- 3. 左下角状态 ----------
    std::wstring ver = (g_selVersion >= 0 && g_selVersion < (int)g_versions.size())
                     ? U8ToW(g_versions[g_selVersion]) : std::wstring(L"");
    std::wstring st;
    if (downloading)
        st = L"正在补全文件  " + std::to_wstring(cur) + L" / " + std::to_wstring(total);
    else if (done >= 4)
        st = ver.empty() ? std::wstring(L"游戏已启动") : (L"游戏已启动 · " + ver);
    else if (!ver.empty())
        st = L"正在启动  " + ver + L" …";
    else
        st = L"正在启动 …";

    RECT sr = { c.left + Scale(32), c.bottom - Scale(90), c.right - Scale(180), c.bottom - Scale(56) };
    DrawTextIn(dc, st, sr, RGB(228, 231, 236), g_fUi);

    const wchar_t* stage = (done == 0) ? L"检测 Java 版本"
                        : (done == 1) ? L"处理游戏依赖"
                        : (done == 2) ? L"登录"
                        : (done == 3) ? L"等待游戏窗口"
                                      : L"完成";
    RECT sr2 = { c.left + Scale(32), c.bottom - Scale(54), c.right - Scale(180), c.bottom - Scale(26) };
    DrawTextIn(dc, stage, sr2, RGB(138, 144, 156), g_fSmall);

    // ---------- 4. 贴底进度条 ----------
    int barH = Scale(4);
    RECT barBg = { c.left, c.bottom - barH, c.right, c.bottom };
    HBRUSH bgB = CreateSolidBrush(RGB(38, 40, 46));
    FillRect(dc, &barBg, bgB);
    DeleteObject(bgB);

    if (ratio > 0) {
        RECT fill = barBg;
        fill.right = barBg.left + (int)((barBg.right - barBg.left) * ratio);
        if (fill.right < fill.left + 1) fill.right = fill.left + 1;
        HBRUSH fb = CreateSolidBrush(C_ACCENT2);
        FillRect(dc, &fill, fb);
        DeleteObject(fb);
    }
}

static void PaintMain(HDC dc, int w, int h) {
    HBRUSH bg = CreateSolidBrush(C_BG);
    RECT all = { 0, 0, w, h };
    FillRect(dc, &all, bg);
    DeleteObject(bg);

    // 顶栏
    HBRUSH hb = CreateSolidBrush(C_HEADER);
    FillRect(dc, &g_ly.header, hb);
    DeleteObject(hb);

    RECT t = g_ly.header; t.left += Scale(24); t.right = t.left + Scale(190);
    DrawTextIn(dc, L"ReignLaunch", t, C_TEXT, g_fTitle);

    // 当前页标题
    RECT pt = { g_ly.main.left + Scale(40), g_ly.content.top + Scale(12), g_ly.main.right, g_ly.content.top + Scale(44) };
    const wchar_t* titles[] = { L"", L"用户", L"设置", L"关于", L"", L"" };
    if (g_page != PAGE_LAUNCH && g_page != PAGE_ABOUT) DrawTextIn(dc, titles[g_page], pt, C_DIM, g_fSmall);

    if (g_page == PAGE_LAUNCH) {
        // 主区信息
        int x = g_ly.content.left + Scale(40);
        RECT l1 = { x, g_ly.content.top + Scale(40), g_ly.main.right, g_ly.content.top + Scale(66) };
        DrawTextIn(dc, L"启动设置", l1, C_DIM, g_fSmall);

        std::wstring acct = (g_cfg.authType == "thirdparty")
            ? (L"账号: " + U8ToW(g_cfg.authName) + L"（第三方）")
            : (L"账号: " + U8ToW(g_cfg.playerName) + L"（离线）");

        RECT l3 = { x, g_ly.content.top + Scale(76), g_ly.main.right, g_ly.content.top + Scale(102) };
        RECT l4 = { x, g_ly.content.top + Scale(102), g_ly.main.right, g_ly.content.top + Scale(128) };
        RECT l5 = { x, g_ly.content.top + Scale(128), g_ly.main.right, g_ly.content.top + Scale(154) };
        DrawTextIn(dc, acct, l3, C_DIM, g_fUi);
        DrawTextIn(dc, L"内存: " + std::to_wstring(g_cfg.maxMemMB) + L" MB", l4, C_DIM, g_fUi);
        std::wstring verSel = (g_selVersion >= 0 && g_selVersion < (int)g_versions.size())
            ? U8ToW(g_versions[g_selVersion]) : std::wstring(L"（未选择）");
        DrawTextIn(dc, L"版本: " + verSel, l5, C_DIM, g_fUi);

        int pst = g_progState.load();
        if (pst != 0) {
            RECT lb2 = LaunchBtnRect();
            int total = g_progTotal.load(), cur = g_progCur.load();
            float p = (pst == 1 && total > 0) ? ((float)cur / (float)total) : 1.0f;
            if (p > 1.0f) p = 1.0f;
            if (p < 0.0f) p = 0.0f;

            RECT txt = { lb2.left - Scale(20), lb2.top - Scale(96), lb2.right, lb2.top - Scale(72) };
            std::wstring s2 = (pst == 1)
                ? (L"正在补全文件  " + std::to_wstring(cur) + L" / " + std::to_wstring(total))
                : L"正在启动游戏…";
            DrawTextIn(dc, s2, txt, C_DIM, g_fSmall);

            RECT bar = { lb2.left - Scale(20), lb2.top - Scale(64), lb2.right, lb2.top - Scale(48) };
            FillRoundRect(dc, bar, 8, C_CARD);
            RECT fill = bar;
            fill.right = bar.left + (int)((bar.right - bar.left) * p);
            if (fill.right > fill.left + 2) FillRoundRect(dc, fill, 8, C_ACCENT2);
        }

        if (!g_status.empty()) {
            RECT st = { x, g_ly.content.top + Scale(212), g_ly.main.right - Scale(20), g_ly.content.top + Scale(240) };
            DrawTextIn(dc, g_status, st, C_DIM, g_fSmall, DT_LEFT | DT_TOP | DT_WORDBREAK);
        }
    }
    else if (g_page == PAGE_USER) {
        // 左侧账户列表
        {
            RECT sb = g_ly.sidebar;
            HBRUSH bg = CreateSolidBrush(RGB(30, 31, 35));
            FillRect(dc, &sb, bg);
            DeleteObject(bg);

            RECT lb = { sb.left + Scale(20), sb.top + Scale(10), sb.right, sb.top + Scale(34) };
            DrawTextIn(dc, L"账户 (" + std::to_wstring(g_cfg.accounts.size()) + L")", lb, C_DIM, g_fSmall);

            int itemH = Scale(64);
            int y = sb.top + Scale(38);
            for (size_t i = 0; i < g_cfg.accounts.size(); ++i) {
                RECT card = { sb.left + Scale(10), y, sb.right - Scale(10), y + itemH - Scale(6) };
                bool sel = ((int)i == g_cfg.activeAcc);
                FillRoundRect(dc, card, 8, sel ? C_CARD_SEL : C_CARD);

                DrawAvatar(dc, card.left + Scale(10), card.top + Scale(8), Scale(42));

                RECT n1 = { card.left + Scale(60), card.top + Scale(10), card.right - Scale(8), card.top + Scale(34) };
                DrawTextIn(dc, U8ToW(g_cfg.accounts[i].name), n1, sel ? C_TEXT : C_DIM, g_fUi);

                std::wstring ty = (g_cfg.accounts[i].type == "thirdparty")
                    ? (L"外置登录 · " + U8ToW(g_cfg.accounts[i].server))
                    : L"离线模式";
                RECT n2 = { card.left + Scale(60), card.top + Scale(34), card.right - Scale(8), card.top + Scale(54) };
                DrawTextIn(dc, ty, n2, C_DIM, g_fSmall);

                if (sel) {
                    RECT bar = { card.left + Scale(2), card.top + Scale(10), card.left + Scale(5), card.bottom - Scale(10) };
                    HBRUSH b = CreateSolidBrush(C_ACCENT2);
                    FillRect(dc, &bar, b);
                    DeleteObject(b);
                }
                y += itemH;
            }

            if (g_cfg.accounts.empty()) {
                RECT e = { sb.left + Scale(16), sb.top + Scale(44), sb.right - Scale(16), sb.top + Scale(120) };
                DrawTextIn(dc, L"还没有账户\n在右边填名字后点「使用离线」", e, C_DIM, g_fSmall,
                           DT_LEFT | DT_TOP | DT_WORDBREAK);
            }
        }

        int panelW = Scale(620);
        int x = g_ly.main.left + (g_ly.main.right - g_ly.main.left - panelW) / 2;
        if (x < g_ly.main.left + Scale(20)) x = g_ly.main.left + Scale(20);
        int y = g_ly.content.top + Scale(60);
        int rowH = Scale(58);

        DrawTextIn(dc, L"认证服务器", { x, y, x + Scale(100), y + Scale(30) }, C_DIM, g_fUi);
        DrawTextIn(dc, L"用户名 / 邮箱", { x, y + rowH, x + Scale(100), y + rowH + Scale(30) }, C_DIM, g_fUi);
        DrawTextIn(dc, L"密码", { x, y + rowH * 2, x + Scale(100), y + rowH * 2 + Scale(30) }, C_DIM, g_fUi);
        DrawTextIn(dc, L"离线玩家名", { x, y + rowH * 4 + Scale(16), x + Scale(100), y + rowH * 4 + Scale(46) }, C_DIM, g_fUi);

        RECT cur = { x, y + rowH * 5 + Scale(12), g_ly.main.right - Scale(20), y + rowH * 5 + Scale(42) };
        std::wstring s = (g_cfg.authType == "thirdparty")
            ? (L"当前账号: " + U8ToW(g_cfg.authName) + L"  (" + U8ToW(g_cfg.authServer) + L")")
            : (L"当前账号: 离线模式 (" + U8ToW(g_cfg.playerName) + L")");
        DrawTextIn(dc, s, cur, C_TEXT, g_fUi);

        if (!g_status.empty()) {
            RECT st = { x, y + rowH * 5 + Scale(46), g_ly.main.right - Scale(20), y + rowH * 5 + Scale(82) };
            DrawTextIn(dc, g_status, st,
                       (g_loginState == 3) ? C_ERR : C_OK, g_fSmall, DT_LEFT | DT_TOP | DT_WORDBREAK);
        }

        RECT hint = { x, g_ly.content.bottom - Scale(90), g_ly.main.right - Scale(20), g_ly.content.bottom - Scale(20) };
        DrawTextIn(dc, L"支持 authlib-injector 协议的认证服务器（如 LittleSkin）。\n"
                       L"authlib-injector 已内置在程序里，无需外部 jar。",
                   hint, C_DIM, g_fSmall, DT_LEFT | DT_TOP | DT_WORDBREAK);
    }
    else if (g_page == PAGE_INSTANCES) {
        int x   = g_ly.content.left + Scale(24);
        int top = g_ly.content.top + Scale(16);

        RECT lbl = { x, top, g_ly.content.right - Scale(160), top + Scale(26) };
        DrawTextIn(dc, L"实例 (" + std::to_wstring(g_versions.size()) + L")", lbl, C_DIM, g_fSmall);

        int itemH = Scale(56);
        int y = top + Scale(34) - g_listScroll;
        for (size_t i = 0; i < g_versions.size(); ++i) {
            RECT item = { x, y, g_ly.content.right - Scale(24), y + itemH - Scale(8) };
                int listBottom = g_ly.content.bottom - Scale(72);
                if (item.bottom > listBottom) item.bottom = listBottom;
                if (item.bottom > top + Scale(34) && item.top < listBottom) {
                bool sel = ((int)i == g_selVersion);
                FillRoundRect(dc, item, 8, sel ? C_CARD_SEL : C_CARD);
                RECT tx = item; tx.left += Scale(16); tx.right -= Scale(12);
                DrawTextIn(dc, U8ToW(g_versions[i]), tx, sel ? C_TEXT : C_DIM, g_fUi);
                if (sel) {
                    RECT bar = { item.left, item.top + Scale(8), item.left + Scale(3), item.bottom - Scale(8) };
                    HBRUSH b = CreateSolidBrush(C_ACCENT2);
                    FillRect(dc, &bar, b);
                    DeleteObject(b);
                }
            }
            y += itemH;
        }
        if (g_versions.empty()) {
            RECT e = { x, top + Scale(50), g_ly.content.right - Scale(24), top + Scale(140) };
            DrawTextIn(dc, L"没有找到实例\n请到【设置】里指定 .minecraft 目录", e, C_DIM, g_fSmall,
                       DT_LEFT | DT_TOP | DT_WORDBREAK);
        }
    }
    else if (g_page == PAGE_DOWNLOAD) {
        if (g_dlDetail) {
            if (g_dlSel < 0 || g_dlSel >= (int)g_dlView.size()) {
                RECT e = { g_ly.content.left + Scale(24), g_ly.content.top + Scale(60),
                           g_ly.content.right - Scale(24), g_ly.content.top + Scale(100) };
                DrawTextIn(dc, L"未选中版本，点「返回」重新选择", e, C_DIM, g_fSmall);
                return;
            }

            const McVer& v = g_mcVers[g_dlView[g_dlSel]];

            // 标题
            int hx = g_ly.content.left + Scale(140);
            int hy = g_ly.content.top + Scale(10);
            RECT t1 = { hx, hy, g_ly.content.right - Scale(24), hy + Scale(32) };
            DrawTextIn(dc, U8ToW(v.id), t1, C_TEXT, g_fTitle);

            std::wstring sub = U8ToW(v.date);
            if (!sub.empty()) sub += L"   ";
            sub += (v.type == "release") ? L"正式版" : (v.type == "snapshot") ? L"快照" :
                   (v.type == "old_beta") ? L"远古 Beta" : L"远古 Alpha";
            RECT t2 = { hx, hy + Scale(30), g_ly.content.right - Scale(24), hy + Scale(52) };
            DrawTextIn(dc, sub, t2, C_DIM, g_fSmall);

            // 加载器选择行（按钮是控件，这里只画提示）
            const wchar_t* hint = (g_dlLoader == 0)
                ? L"原版：不安装任何加载器"
                : (g_dlLoader == 1) ? L"Fabric：请在下方选择加载器版本"
                : (g_dlLoader == 2) ? L"Quilt：请在下方选择加载器版本"
                : (g_dlLoader == 3) ? L"Forge：请在下方选择加载器版本"
                : (g_dlLoader == 4) ? L"NeoForge：请在下方选择加载器版本"
                : (g_dlLoader == 5) ? L"Fabric API：请在下方选择模组版本（下载到 mods）"
                                    : L"QSL（Quilt Standard Libraries）：请在下方选择模组版本";
            RECT hr = { g_ly.content.left + Scale(24), g_ly.content.top + Scale(104),
                        g_ly.content.right - Scale(24), g_ly.content.top + Scale(130) };
            DrawTextIn(dc, hint, hr, C_DIM, g_fSmall);

            // 版本列表
            int listTop  = g_ly.content.top + Scale(136);
            int listBot  = g_ly.content.bottom - Scale(70);

            if (g_dlLoader == 0) {
                RECT e = { g_ly.content.left + Scale(24), listTop + Scale(10),
                           g_ly.content.right - Scale(24), listTop + Scale(46) };
                DrawTextIn(dc, L"选中「原版」时不安装加载器，直接点右下角安装即可。", e, C_DIM, g_fSmall);
            } else if (g_ldVersState.load() == 0 || g_ldVersState.load() == 1) {
                RECT e = { g_ly.content.left + Scale(24), listTop + Scale(10),
                           g_ly.content.right - Scale(24), listTop + Scale(46) };
                DrawTextIn(dc, L"正在获取加载器版本…", e, C_DIM, g_fSmall);
            } else if (g_ldVersState.load() == 3) {
                RECT e = { g_ly.content.left + Scale(24), listTop + Scale(10),
                           g_ly.content.right - Scale(24), listTop + Scale(46) };
                DrawTextIn(dc, L"获取失败，检查网络后重试", e, C_ERR, g_fSmall);
            } else if (g_ldVers.empty()) {
                RECT e = { g_ly.content.left + Scale(24), listTop + Scale(10),
                           g_ly.content.right - Scale(24), listTop + Scale(46) };
                DrawTextIn(dc, L"没有可下载的版本", e, C_ERR, g_fSmall);
            } else {
                int itemH = Scale(44);
                int yy = listTop - g_listScroll;
                for (size_t i = 0; i < g_ldVers.size(); ++i) {
                    RECT item = { g_ly.content.left + Scale(24), yy,
                                  g_ly.content.right - Scale(24), yy + itemH - Scale(6) };
                    if (item.bottom > listTop && item.top < listBot) {
                        if (item.bottom > listBot) item.bottom = listBot;
                        bool sel = ((int)i == g_ldSel);
                        FillRoundRect(dc, item, 8, sel ? C_CARD_SEL : C_CARD);
                        RECT tx = item; tx.left += Scale(16);
                        DrawTextIn(dc, U8ToW(g_ldVers[i]), tx, sel ? C_TEXT : C_DIM, g_fUi);
                        if (sel) {
                            RECT bar = { item.left, item.top + Scale(6), item.left + Scale(3), item.bottom - Scale(6) };
                            HBRUSH b = CreateSolidBrush(C_ACCENT2);
                            FillRect(dc, &bar, b);
                            DeleteObject(b);
                        }
                    }
                    yy += itemH;
                }
            }
            return;
        }

        int x    = g_ly.content.left + Scale(24);
        int top  = g_ly.content.top + Scale(16);
        int listTop = top + Scale(46);
        int listBottom = g_ly.content.bottom - Scale(72);

        if (g_mcVersState.load() == 0 || g_mcVersState.load() == 1) {
            RECT e = { x, listTop + Scale(20), g_ly.content.right - Scale(24), listTop + Scale(60) };
            DrawTextIn(dc, L"正在获取版本清单…", e, C_DIM, g_fSmall);
        } else if (g_mcVersState.load() == 3) {
            RECT e = { x, listTop + Scale(20), g_ly.content.right - Scale(24), listTop + Scale(60) };
            DrawTextIn(dc, L"获取版本清单失败，检查网络后重进本页", e, C_ERR, g_fSmall);
        } else if (g_dlView.empty()) {
            RECT e = { x, listTop + Scale(20), g_ly.content.right - Scale(24), listTop + Scale(60) };
            DrawTextIn(dc, L"没有符合条件的版本", e, C_DIM, g_fSmall);
        } else {
            int itemH = Scale(52);
            int y = listTop - g_listScroll;
            for (size_t i = 0; i < g_dlView.size(); ++i) {
                const McVer& v = g_mcVers[g_dlView[i]];
                RECT item = { x, y, g_ly.content.right - Scale(24), y + itemH - Scale(8) };
                if (item.bottom > listTop && item.top < listBottom) {
                    if (item.bottom > listBottom) item.bottom = listBottom;
                    bool sel = ((int)i == g_dlSel);
                    FillRoundRect(dc, item, 8, sel ? C_CARD_SEL : C_CARD);

                    RECT tx = item; tx.left += Scale(16);
                    DrawTextIn(dc, U8ToW(v.id), tx, sel ? C_TEXT : C_DIM, g_fUi);

                    if (!v.date.empty()) {
                        RECT td = { item.right - Scale(300), item.top, item.right - Scale(180), item.bottom };
                        DrawTextIn(dc, U8ToW(v.date), td, C_DIM, g_fSmall);
                    }

                    bool april = IsAprilFools(v);
                    std::wstring ty = april ? L"愚人节"
                                    : (v.type == "release") ? L"正式版"
                                    : (v.type == "snapshot") ? L"快照"
                                    : (v.type == "old_beta") ? L"远古 Beta"
                                                             : L"远古 Alpha";
                    RECT tr2 = { item.right - Scale(160), item.top, item.right - Scale(16), item.bottom };
                    DrawTextIn(dc, ty, tr2, april ? C_ERR : C_DIM, g_fSmall);

                    if (sel) {
                        RECT bar = { item.left, item.top + Scale(8), item.left + Scale(3), item.bottom - Scale(8) };
                        HBRUSH b = CreateSolidBrush(C_ACCENT2);
                        FillRect(dc, &bar, b);
                        DeleteObject(b);
                    }
                }
                y += itemH;
            }
        }
    }
    else if (g_page == PAGE_ABOUT) {
        DrawAboutBlock(dc);
    }
    else {
        DrawSettingsMenu(dc);
        if (g_settingsTab == 1) {
            DrawAboutBlock(dc);
        } else {
        int x = g_ly.main.left + Scale(40);
        int y = g_ly.content.top + Scale(50);
        int rowH = Scale(62);

        DrawTextIn(dc, L"游戏目录", { x, y, x + Scale(100), y + Scale(30) }, C_DIM, g_fUi);
        DrawTextIn(dc, L"Java 路径", { x, y + rowH, x + Scale(100), y + rowH + Scale(30) }, C_DIM, g_fUi);
        DrawTextIn(dc, L"最大内存", { x, y + rowH * 2, x + Scale(100), y + rowH * 2 + Scale(30) }, C_DIM, g_fUi);
        DrawTextIn(dc, L"", { x, y + rowH * 3, x + Scale(100), y + rowH * 3 + Scale(30) }, C_DIM, g_fUi);

        DrawTextIn(dc, L"MB", { x + Scale(230), y + rowH * 2, x + Scale(300), y + rowH * 2 + Scale(30) }, C_DIM, g_fUi);
        DrawTextIn(dc, L"", { x + Scale(360), y + rowH * 3, x + Scale(600), y + rowH * 3 + Scale(30) },
                   C_DIM, g_fSmall);

        {
            RECT tl = { x, y + rowH * 3 + Scale(6), x + Scale(100), y + rowH * 3 + Scale(36) };
            DrawTextIn(dc, L"下载线程", tl, C_DIM, g_fUi);
            RECT th = { x + Scale(210), y + rowH * 3 + Scale(12), g_ly.main.right - Scale(40), y + rowH * 3 + Scale(34) };
            DrawTextIn(dc, L"1 - 128（默认 4）", th, C_DIM, g_fSmall);
        }

        {
            RECT a = { x, y + rowH * 4 + Scale(6), x + Scale(100), y + rowH * 4 + Scale(36) };
            DrawTextIn(dc, L"JVM 参数", a, C_DIM, g_fUi);
            RECT b2 = { x, y + rowH * 5 + Scale(6), x + Scale(100), y + rowH * 5 + Scale(36) };
            DrawTextIn(dc, L"游戏参数", b2, C_DIM, g_fUi);
            RECT c2 = { x + Scale(560), y + rowH * 6 + Scale(16), g_ly.main.right - Scale(40), y + rowH * 6 + Scale(36) };
            DrawTextIn(dc, L"空格分隔，可留空", c2, C_DIM, g_fSmall);
        }


        if (!g_status.empty()) {
            RECT st = { x, y + rowH * 8 + Scale(6), g_ly.main.right - Scale(40), g_ly.content.bottom - Scale(4) };
            DrawTextIn(dc, g_status, st, C_OK, g_fSmall, DT_LEFT | DT_TOP | DT_WORDBREAK);
        }
        }
    }


}

// ---------------- 错误窗口 ----------------
static void ShowErrorWindow() {
    if (g_hErrWnd) { SetForegroundWindow(g_hErrWnd); return; }

    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = ErrProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ReignLaunchError";
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        reg = true;
    }
    int w = Scale(700), h = Scale(500);
    g_hErrWnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ReignLaunchError", L"游戏出错",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, w, h, g_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_hErrWnd) return;

    std::string tail = LogTail(400);
    SetWindowTextW(g_hErrEdit, U8ToW(tail).c_str());
    ShowWindow(g_hErrWnd, SW_SHOW);
    SetForegroundWindow(g_hErrWnd);
}

// ---------------- 登录线程 ----------------
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

// ---------------- 文件对话框 ----------------
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

// ============================ 下拉浮层 ============================
#define WM_DROP_PICK (WM_APP + 10)

static HWND  g_dropWnd = nullptr;
static int   g_dropMode = 0;   // 0=dir 1=java 2=instance
static int   g_dropHot = -1;
static DWORD g_dropBorn = 0;
static std::vector<Cand> g_dropItems;

static LRESULT CALLBACK DropProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(C_CARD);
        FillRect(dc, &rc, bg);
        DeleteObject(bg);

        int itemH = Scale(28);
        int y = Scale(4);
        for (size_t i = 0; i < g_dropItems.size(); ++i) {
            RECT it = { Scale(4), y, rc.right - Scale(4), y + itemH };
            if ((int)i == g_dropHot) {
                HBRUSH hb = CreateSolidBrush(C_CARD_SEL);
                FillRect(dc, &it, hb);
                DeleteObject(hb);
            }
            std::wstring txt = U8ToW(g_dropItems[i].path);
            if (g_dropMode == 1) txt = U8ToW(g_dropItems[i].label) + L"    " + U8ToW(g_dropItems[i].path);
            RECT tt = it; tt.left += Scale(8); tt.right -= Scale(4);
            DrawTextIn(dc, txt, tt, C_TEXT, g_fSmall);
            y += itemH;
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int idx = (GET_Y_LPARAM(lp) - Scale(4)) / Scale(28);
        if (idx < 0 || idx >= (int)g_dropItems.size()) idx = -1;
        if (idx != g_dropHot) { g_dropHot = idx; InvalidateRect(hwnd, nullptr, FALSE); }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int idx = (GET_Y_LPARAM(lp) - Scale(4)) / Scale(28);
        if (idx >= 0 && idx < (int)g_dropItems.size())
            PostMessageW(g_hwnd, WM_DROP_PICK, (WPARAM)idx, (LPARAM)g_dropMode);
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_KILLFOCUS:
        // 刚弹出时焦点可能立刻被按钮抢回，忽略这一次
        if (GetTickCount() - g_dropBorn > 250) DestroyWindow(hwnd);
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_dropWnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 浮层只是一个小方块，无标题栏、不进任务栏，看起来仍是一个窗口
static void ShowDropPopup(int mode) {
    if (g_dropWnd) { DestroyWindow(g_dropWnd); return; }

    g_dropItems.clear();
    if (mode == 0)      g_dropItems = g_dirCands;
    else if (mode == 1) g_dropItems = g_javaCands;
    else if (mode == 2) { for (auto& v : g_versions) g_dropItems.push_back({ v, v }); }
    else if (mode == 7) {
        g_dropItems.push_back({ "正式版", "正式版" });
        g_dropItems.push_back({ "快照版", "快照版" });
        g_dropItems.push_back({ "远古版", "远古版" });
        g_dropItems.push_back({ "愚人节", "愚人节" });
    }
    else {
        g_dropItems.push_back({ "隔离所有实例", "隔离所有实例" });
        g_dropItems.push_back({ "隔离模组实例", "隔离模组实例" });
        g_dropItems.push_back({ "不隔离实例",   "不隔离实例" });
    }
    if (g_dropItems.empty()) return;

    RECT er{};
    if (mode == 0)      GetWindowRect(hEdDir, &er);
    else if (mode == 1) GetWindowRect(hEdJava, &er);
    else if (mode == 2) GetWindowRect(hBtnInstArrow, &er);
    else if (mode == 3) GetWindowRect(hBtnIso, &er);
    else                GetWindowRect(hBtnDlFilterSel, &er);

    int itemH = Scale(28);
    int w = Scale(mode == 2 ? 300 : (mode == 7 ? 180 : (mode == 3 ? 300 : 420)));
    int h = itemH * (int)g_dropItems.size() + Scale(8);

    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DropProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ReignLaunchDrop";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        reg = true;
    }

    g_dropMode = mode;
    g_dropHot = -1;
    int px2 = er.left;
    int py2 = (mode == 2) ? (er.top - h - Scale(2)) : (er.bottom + Scale(2));
    g_dropWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"ReignLaunchDrop", L"",
        WS_POPUP | WS_BORDER,
        px2, py2, w, h,
        g_hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_dropWnd) return;
    g_dropBorn = GetTickCount();
    ShowWindow(g_dropWnd, SW_SHOW);
    SetFocus(g_dropWnd);
}

// ---------------- 主窗口 ----------------
static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;

        hBtnTabLaunch   = MakeBtn(hwnd, L"启动", ID_TAB_LAUNCH);
        hBtnTabUser     = MakeBtn(hwnd, L"用户", ID_TAB_USER);
        hBtnTabSettings = MakeBtn(hwnd, L"设置", ID_TAB_SET);
        hBtnTabAbout    = MakeBtn(hwnd, L"关于", ID_TAB_ABOUT);
        hBtnTabInst     = MakeBtn(hwnd, L"实例", ID_TAB_INST);
        hBtnTabDl       = MakeBtn(hwnd, L"下载", ID_TAB_DL);
        hBtnDlVanilla = MakeBtn(hwnd, L"原版", ID_DL_VANILLA);
        hBtnDlFabric  = MakeBtn(hwnd, L"Fabric",   ID_DL_FABRIC);
        hBtnDlQuilt   = MakeBtn(hwnd, L"Quilt",    ID_DL_QUILT);
        hBtnDlForge   = MakeBtn(hwnd, L"Forge",    ID_DL_FORGE);
        hBtnDlNeo     = MakeBtn(hwnd, L"NeoForge", ID_DL_NEO);
        hBtnDlF0      = MakeBtn(hwnd, L"正式版",   ID_DL_FILTER0);
        hBtnDlF1      = MakeBtn(hwnd, L"快照版",   ID_DL_FILTER1);
        hBtnDlF2      = MakeBtn(hwnd, L"远古版",   ID_DL_FILTER2);
        hBtnDlF3      = MakeBtn(hwnd, L"愚人节",   ID_DL_FILTER3);
        hEdSearch = CreateWindowExW(0, L"EDIT", L"",
                        WS_CHILD | WS_BORDER | ES_AUTOHSCROLL,
                        0, 0, 10, 10, hwnd, (HMENU)ID_ED_SEARCH, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hEdSearch, WM_SETFONT, (WPARAM)g_fUi, TRUE);
        SendMessageW(hEdSearch, EM_SETCUEBANNER, 0, (LPARAM)L"搜索版本，如 1.20.1");
        hBtnDlBack    = MakeBtn(hwnd, L"← 返回", ID_DL_BACK);
        hBtnDlInstall = MakeBtn(hwnd, L"安装", ID_DL_INSTALL);
        hBtnDlFapi    = MakeBtn(hwnd, L"Fabric API", ID_DL_FAPI);
        hBtnDlQsl     = MakeBtn(hwnd, L"QSL", ID_DL_QSL);
        hBtnDlFilterSel = MakeBtn(hwnd, L"正式版  ▾", ID_DL_FILTERSEL);

        hBtnLaunch  = MakeBtn(hwnd, L"启动游戏", ID_BTN_LAUNCH);
        hBtnRefresh = MakeBtn(hwnd, L"刷新版本", ID_BTN_REFRESH);
        hBtnInstArrow = MakeBtn(hwnd, L"", ID_BTN_INSTARROW);
        hBtnOverlayCancel = MakeBtn(hwnd, L"取消", ID_BTN_OVCANCEL);

        hEdServer = MakeEdit(hwnd, ID_ED_SERVER, U8ToW(g_cfg.authServer).c_str());
        hEdUser   = MakeEdit(hwnd, ID_ED_USER, L"");
        hEdPass   = MakeEdit(hwnd, ID_ED_PASS, L"", ES_PASSWORD);
        hBtnLogin = MakeBtn(hwnd, L"登录", ID_BTN_LOGIN);
        hBtnLogout= MakeBtn(hwnd, L"退出登录", ID_BTN_LOGOUT);
        hBtnOffline = MakeBtn(hwnd, L"使用离线", ID_BTN_OFFLINE);
        hBtnSkin    = MakeBtn(hwnd, L"更换皮肤", ID_BTN_SKIN);
        hBtnDelAcc  = MakeBtn(hwnd, L"删除账户", ID_BTN_DELACC);
        hEdThreads = CreateWindowExW(0, L"EDIT", std::to_wstring(g_cfg.dlThreads).c_str(),
                        WS_CHILD | WS_BORDER | ES_NUMBER | ES_CENTER,
                        0, 0, 10, 10, hwnd, (HMENU)ID_ED_THREADS, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hEdThreads, WM_SETFONT, (WPARAM)g_fUi, TRUE);

        if (hEdJvm) {} else {
        hEdJvm = CreateWindowExW(0, L"EDIT", U8ToW(g_cfg.extraJvm).c_str(),
                    WS_CHILD | WS_BORDER, 0, 0, 10, 10, hwnd, (HMENU)ID_ED_JVM, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hEdJvm, WM_SETFONT, (WPARAM)g_fUi, TRUE);

        hEdGame = CreateWindowExW(0, L"EDIT", U8ToW(g_cfg.extraGame).c_str(),
                    WS_CHILD | WS_BORDER, 0, 0, 10, 10, hwnd, (HMENU)ID_ED_GAME, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(hEdGame, WM_SETFONT, (WPARAM)g_fUi, TRUE);

        hBtnSource = MakeBtn(hwnd, g_cfg.dlSource == 1 ? L"下载源：仅官方"
                                  : g_cfg.dlSource == 2 ? L"下载源：仅 BMCLAPI" : L"下载源：自动", ID_BTN_SOURCE);
        hBtnFull   = MakeBtn(hwnd, (std::wstring(L"游戏窗口模式：") + WinModeLabel(g_cfg.winMode)).c_str(), ID_BTN_FULL);
        hBtnUpdate = MakeBtn(hwnd, L"检查更新", ID_BTN_UPDATE);
        }

        hEdDir  = MakeEdit(hwnd, ID_ED_DIR, U8ToW(g_cfg.gameDir).c_str());
        hEdJava = MakeEdit(hwnd, ID_ED_JAVA, U8ToW(g_cfg.javaPath).c_str());
        hEdMem  = MakeEdit(hwnd, ID_ED_MEM, std::to_wstring(g_cfg.maxMemMB).c_str(), ES_NUMBER);
        hEdName = MakeEdit(hwnd, ID_ED_NAME, U8ToW(g_cfg.playerName).c_str());
        hBtnAutoDir    = MakeBtn(hwnd, L"自动检测", ID_BTN_AUTODIR);
        hBtnBrowseDir  = MakeBtn(hwnd, L"浏览", ID_BTN_BROWSEDIR);
        hBtnDirDrop    = MakeBtn(hwnd, L"▾", ID_BTN_DIRDROP);
        hBtnAutoJava   = MakeBtn(hwnd, L"自动检测", ID_BTN_AUTOJAVA);
        hBtnBrowseJava = MakeBtn(hwnd, L"浏览", ID_BTN_BROWSEJAVA);
        hBtnJavaDrop   = MakeBtn(hwnd, L"▾", ID_BTN_JAVADROP);
        hBtnSave       = MakeBtn(hwnd, L"保存设置", ID_BTN_SAVE);
        hBtnIso        = MakeBtn(hwnd, (std::wstring(L"版本隔离：") + IsoLabel() + L"  ▾").c_str(), ID_BTN_ISO);

        SetTimer(hwnd, 1, 300, nullptr);
        return 0;
    }

    case WM_SIZE:
        ComputeLayout(LOWORD(lp), HIWORD(lp));
        ShowPage(g_page);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
        PaintMain(mem, rc.right, rc.bottom);
        if (g_overlay) DrawOverlay(mem);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, C_TEXT);
        SetBkColor(dc, C_INPUTBG);
        return (LRESULT)g_brInput;
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT d = (LPDRAWITEMSTRUCT)lp;
        RECT r = d->rcItem;
        bool sel = (d->itemState & ODS_SELECTED) != 0;

        int id = (int)d->CtlID;
        bool accent = (id == ID_BTN_LAUNCH || id == ID_BTN_LOGIN || id == ID_BTN_SAVE);
        bool tab = (id == ID_TAB_LAUNCH || id == ID_TAB_USER || id == ID_TAB_SET || id == ID_TAB_ABOUT || id == ID_TAB_INST || id == ID_TAB_DL);
        bool activeTab = tab && ((id == ID_TAB_LAUNCH && g_page == PAGE_LAUNCH) ||
                                 (id == ID_TAB_USER && g_page == PAGE_USER) ||
                                 (id == ID_TAB_SET && g_page == PAGE_SETTINGS) ||
                                 (id == ID_TAB_ABOUT && g_page == PAGE_ABOUT) ||
                                 (id == ID_TAB_INST && g_page == PAGE_INSTANCES) ||
                                 (id == ID_TAB_DL && g_page == PAGE_DOWNLOAD));

        COLORREF bg;
        if (accent) bg = (g_launching && id == ID_BTN_LAUNCH) ? C_CARD : (sel ? RGB(29, 78, 216) : C_ACCENT);
        else if (activeTab) bg = C_CARD_SEL;
        else bg = sel ? C_CARD_SEL : C_CARD;

        FillRoundRect(d->hDC, r, 8, bg);

        if (id == ID_BTN_DIRDROP || id == ID_BTN_JAVADROP || id == ID_BTN_INSTARROW) {
            int cx = (r.left + r.right) / 2;
            int cy = (r.top + r.bottom) / 2;
            POINT tri[3];
            if (id == ID_BTN_INSTARROW) {
                tri[0] = { cx - Scale(8), cy + Scale(4) };
                tri[1] = { cx + Scale(8), cy + Scale(4) };
                tri[2] = { cx,            cy - Scale(6) };
            } else {
                tri[0] = { cx - Scale(6), cy - Scale(3) };
                tri[1] = { cx + Scale(6), cy - Scale(3) };
                tri[2] = { cx,            cy + Scale(4) };
            }
            HBRUSH tb = CreateSolidBrush(C_TEXT);
            HPEN   tp = CreatePen(PS_SOLID, 1, C_TEXT);
            HBRUSH ob = (HBRUSH)SelectObject(d->hDC, tb);
            HPEN   op = (HPEN)SelectObject(d->hDC, tp);
            Polygon(d->hDC, tri, 3);
            SelectObject(d->hDC, ob);
            SelectObject(d->hDC, op);
            DeleteObject(tb);
            DeleteObject(tp);
            return TRUE;
        }

        wchar_t text[128] = {0};
        GetWindowTextW(d->hwndItem, text, 127);
        SetBkMode(d->hDC, TRANSPARENT);
        SetTextColor(d->hDC, (accent || activeTab) ? RGB(255, 255, 255) : C_TEXT);
        SelectObject(d->hDC, g_fUi);
        DrawTextW(d->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return TRUE;
    }

    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (g_dropWnd) { DestroyWindow(g_dropWnd); }

        if (g_page == PAGE_SETTINGS) {
            RECT sb = g_ly.sidebar;
            int itemH = Scale(46);
            int top = sb.top + Scale(50);
            if (pt.x >= sb.left && pt.x <= sb.right && pt.y >= top && pt.y < top + itemH * 2) {
                int idx = (pt.y - top) / itemH;
                if (idx >= 0 && idx < 2 && idx != g_settingsTab) {
                    g_settingsTab = idx;
                    SyncSettingsVisibility();
                    InvalidateRect(hwnd, nullptr, TRUE);
                }
                return 0;
            }
        }

        if (g_page == PAGE_DOWNLOAD) {
            if (g_dlDetail) {
                if (g_dlLoader > 0 && !g_ldVers.empty()) {
                    int itemH0 = Scale(44);
                    int top0 = g_ly.content.top + Scale(136);
                    if (pt.y >= top0) {
                        int idx0 = (pt.y - top0 + g_listScroll) / itemH0;
                        if (idx0 >= 0 && idx0 < (int)g_ldVers.size()) {
                            g_ldSel = idx0;
                            g_status = L"已选择加载器版本：" + U8ToW(g_ldVers[idx0]);
                            InvalidateRect(hwnd, nullptr, TRUE);
                        }
                    }
                }
                return 0;
            }
            if (g_mcVersState.load() == 0) {
                std::thread(FetchManifestThread).detach();
            } else if (g_mcVersState.load() == 2) {
                int itemH = Scale(52);
                int top = g_ly.content.top + Scale(16) + Scale(46);
                if (pt.y >= top) {
                                    int idx = (pt.y - top + g_listScroll) / itemH;
                    if (idx >= 0 && idx < (int)g_dlView.size()) {
                        std::string id = g_mcVers[g_dlView[idx]].id;
                        std::string url = g_mcVers[g_dlView[idx]].url;
                        g_dlSel = idx;
                        g_dlDetail = true;
                        g_dlDetailMc = id;
                        for (int k = 0; k < 4; ++k) { g_ldState[k] = 0; g_ldVer[k].clear(); }
                        std::thread(FetchLoadersThread).detach();
                        LayoutControls();
                        SyncDownloadControls();
                        SetFocus(hwnd);
                        g_status = L"已选择：" + U8ToW(id) + L"  → 点下方的按钮安装";
                        InvalidateRect(hwnd, nullptr, TRUE);
                    }
                }
            }
            return 0;
        }

        if (g_page == PAGE_INSTANCES) {
            int itemH = Scale(56);
            int top = g_ly.content.top + Scale(16) + Scale(34);
            if (pt.y >= top) {
                int idx = (pt.y - top + g_listScroll) / itemH;
                if (idx >= 0 && idx < (int)g_versions.size()) {
                    g_selVersion = idx;
                    InvalidateRect(hwnd, nullptr, TRUE);
                }
            }
            return 0;
        }

        if (g_page == PAGE_USER) {
            RECT sb = g_ly.sidebar;
            int itemH = Scale(64);
            int top = sb.top + Scale(38);
            if (pt.x >= sb.left && pt.x <= sb.right && pt.y >= top) {
                int idx = (pt.y - top) / itemH;
                if (idx >= 0 && idx < (int)g_cfg.accounts.size() && idx != g_cfg.activeAcc) {
                    CaptureActiveAccount();
                    g_cfg.activeAcc = idx;
                    ApplyActiveAccount();
    std::thread(UpdateThread, false).detach();
                    LoadSkinBitmap();
                    SaveConfig();
                    g_status = L"已切换到账户：" + U8ToW(g_cfg.accounts[idx].name);
                    InvalidateRect(hwnd, nullptr, TRUE);
                }
                return 0;
            }
        }

        if (g_page == PAGE_INSTANCES || g_page == PAGE_DOWNLOAD) {
            int delta0 = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
            bool dl = (g_page == PAGE_DOWNLOAD);
            int itemH0 = dl ? Scale(48) : Scale(56);
            int cnt = dl ? (int)g_mcVers.size() : (int)g_versions.size();
            int listH0 = (int)(g_ly.content.bottom - g_ly.content.top) - Scale(70);
            int maxS = std::max(0, cnt * itemH0 - listH0);
            g_listScroll -= delta0 * itemH0;
            g_listScroll = std::max(0, std::min(maxS, g_listScroll));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g_page != PAGE_LAUNCH) return 0;

        // 版本列表点击
        if (PtInRect(&g_ly.sidebar, pt) && pt.y > g_ly.sidebar.top + Scale(86)) {
            int itemH = Scale(46);
            int idx = (pt.y - (g_ly.sidebar.top + Scale(86)) + g_listScroll) / itemH;
            if (idx >= 0 && idx < (int)g_versions.size()) {
                g_selVersion = idx;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        if (g_page == PAGE_INSTANCES || g_page == PAGE_DOWNLOAD) {
            int delta0 = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
            bool dl = (g_page == PAGE_DOWNLOAD);
            int itemH0 = dl ? Scale(48) : Scale(56);
            int cnt = dl ? (int)g_mcVers.size() : (int)g_versions.size();
            int listH0 = (int)(g_ly.content.bottom - g_ly.content.top) - Scale(70);
            int maxS = std::max(0, cnt * itemH0 - listH0);
            g_listScroll -= delta0 * itemH0;
            g_listScroll = std::max(0, std::min(maxS, g_listScroll));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g_page != PAGE_LAUNCH) return 0;
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &pt);
        if (!PtInRect(&g_ly.sidebar, pt)) return 0;

        int delta = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        int itemH = Scale(46);
        int listH = (int)(g_ly.sidebar.bottom - g_ly.sidebar.top) - Scale(96);
        int maxScroll = std::max(0, (int)g_versions.size() * itemH - listH);
        g_listScroll -= delta * itemH;
        g_listScroll = std::max(0, std::min(maxScroll, g_listScroll));
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wp);
        switch (id) {
        case ID_TAB_LAUNCH: ShowPage(PAGE_LAUNCH); break;
        case ID_TAB_USER:   ShowPage(PAGE_USER);   break;
        case ID_TAB_SET:    ShowPage(PAGE_SETTINGS); break;
        case ID_TAB_ABOUT:  ShowPage(PAGE_ABOUT);    break;
        case ID_TAB_INST:   ShowPage(PAGE_INSTANCES); break;
        case ID_TAB_DL:     ShowPage(PAGE_DOWNLOAD); break;

        case ID_BTN_REFRESH:
            ScanVersions();
            g_status = L"已刷新，共 " + std::to_wstring(g_versions.size()) + L" 个版本";
            InvalidateRect(hwnd, nullptr, FALSE);
            break;

        case ID_BTN_FILL:
            if (g_selVersion < 0 || g_selVersion >= (int)g_versions.size()) {
                g_status = L"请先选择版本";
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (g_downloading) { g_status = L"正在补全中…"; InvalidateRect(hwnd, nullptr, FALSE); return 0; }
            {
                std::string ver = g_versions[g_selVersion];
                g_downloading = true;
                g_status = L"正在补全文件…";
                InvalidateRect(hwnd, nullptr, FALSE);
                std::thread([ver]() {
                    DownloadStats st;
                    DownloadMissing(ver, st);
                    g_downloading = false;
                    {
                        std::lock_guard<std::mutex> lk(g_logMutex);
                        g_status = L"补全完成：已有 " + std::to_wstring(st.existed) +
                                   L"，新下载 " + std::to_wstring(st.downloaded) +
                                   L"，失败 " + std::to_wstring(st.failed);
                    }
                    RebuildDlView();
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
                }).detach();
            }
            break;

        case ID_DL_BACK: {
            g_dlDetail = false;
            InvalidateRect(hwnd, nullptr, TRUE);
            break;
        }

        case ID_DL_FILTERSEL:
        ShowDropPopup(7);
        break;

    case ID_DL_FILTER0: case ID_DL_FILTER1: case ID_DL_FILTER2: case ID_DL_FILTER3: {
            g_dlFilter = (int)(wp & 0xFFFF) - ID_DL_FILTER0;
            g_dlSel = -1;
            RebuildDlView();
            InvalidateRect(hwnd, nullptr, TRUE);
            break;
        }

        case ID_DL_VANILLA: case ID_DL_FABRIC: case ID_DL_QUILT:
        case ID_DL_FORGE:   case ID_DL_NEO:
        case ID_DL_FAPI:    case ID_DL_QSL: {
            int id = (int)(wp & 0xFFFF);
            int kind = (id == ID_DL_VANILLA) ? 0 : (id == ID_DL_FABRIC) ? 1
                     : (id == ID_DL_QUILT) ? 2 : (id == ID_DL_FORGE) ? 3
                     : (id == ID_DL_NEO) ? 4 : (id == ID_DL_FAPI) ? 5 : 6;
            if (kind >= 5) { g_ldFiles.clear(); }
            g_dlLoader = kind;
            g_ldSel = -1;
            g_ldVers.clear();
            g_ldVersState = 0;
            g_listScroll = 0;
            if (kind > 0) std::thread(FetchLoaderVersThread).detach();
            InvalidateRect(hwnd, nullptr, TRUE);
            break;
        }

        case ID_DL_INSTALL: {
            if (g_dlSel < 0 || g_dlSel >= (int)g_dlView.size()) {
                MessageBoxW(hwnd, L"请先返回列表选择一个游戏版本。", L"下载", MB_OK | MB_ICONINFORMATION);
                break;
            }
            int realIdx = g_dlView[g_dlSel];
            std::string mc  = g_mcVers[realIdx].id;
            std::string url = g_mcVers[realIdx].url;

            if (g_dlLoader == 0) {
                std::wstring cmsg = L"安装原版 " + U8ToW(mc) + L"（不安装加载器）？";
                if (MessageBoxW(hwnd, cmsg.c_str(), L"下载原版", MB_YESNO | MB_ICONQUESTION) != IDYES) break;
                g_status = L"开始安装：" + U8ToW(mc);
                std::thread(InstallVersionThread, mc, url).detach();
            } else if (g_dlLoader >= 5) {
                if (g_ldSel < 0 || g_ldSel >= (int)g_ldFiles.size()) {
                    MessageBoxW(hwnd, L"请先在列表里选择一个模组版本。", L"提示", MB_OK | MB_ICONINFORMATION);
                    break;
                }
                {
                    std::string info = g_ldFiles[g_ldSel];
                    size_t nl = info.find((char)10);
                    std::string surl = (nl == std::string::npos) ? info : info.substr(0, nl);
                    std::string sfn  = (nl == std::string::npos) ? std::string("mod.jar") : info.substr(nl + 1);
                    if (surl.empty()) {
                        MessageBoxW(hwnd, L"该版本没有可下载的文件。", L"提示", MB_OK | MB_ICONINFORMATION);
                        break;
                    }
                    fs::path mcDir2 = fs::u8path(g_cfg.gameDir);
                    fs::path modDir = ShouldIsolate(mc, mcDir2)
                                      ? (mcDir2 / L"versions" / fs::u8path(mc) / L"mods")
                                      : (mcDir2 / L"mods");
                    std::error_code ecm;
                    fs::create_directories(modDir, ecm);
                    fs::path dst = modDir / fs::u8path(sfn);
                    std::string derr;
                    if (DownloadFile(surl, dst, derr))
                        g_status = L"已下载到 mods：" + U8ToW(sfn);
                    else
                        g_status = L"下载失败：" + U8ToW(derr);
                }
                InvalidateRect(hwnd, nullptr, TRUE);
                break;
            } else {
                if (g_ldSel < 0 || g_ldSel >= (int)g_ldVers.size()) {
                    MessageBoxW(hwnd, L"请先在列表里选择一个加载器版本。", L"提示", MB_OK | MB_ICONINFORMATION);
                    break;
                }
                std::string lv = g_ldVers[g_ldSel];
                std::wstring cmsg = L"为 " + U8ToW(mc) + L" 安装 " +
                                    (g_dlLoader == 1 ? L"Fabric " : g_dlLoader == 2 ? L"Quilt " :
                                     g_dlLoader == 3 ? L"Forge " : L"NeoForge ") +
                                    U8ToW(lv) + L"？";
                if (MessageBoxW(hwnd, cmsg.c_str(), L"安装加载器", MB_YESNO | MB_ICONQUESTION) != IDYES) break;
                if (g_dlLoader == 1)      std::thread(InstallMetaLoaderThread, mc, false, lv).detach();
                else if (g_dlLoader == 2) std::thread(InstallMetaLoaderThread, mc, true, lv).detach();
                else if (g_dlLoader == 3) std::thread(InstallInstallerThread, mc, false, lv).detach();
                else                      std::thread(InstallInstallerThread, mc, true, lv).detach();
            }
            InvalidateRect(hwnd, nullptr, TRUE);
            break;
        }

    case ID_BTN_UPDATE:
        g_status = L"正在检查更新…";
        std::thread(UpdateThread, true).detach();
        InvalidateRect(hwnd, nullptr, TRUE);
        break;

    case ID_BTN_SOURCE: {
        g_cfg.dlSource = (g_cfg.dlSource + 1) % 3;
        SetWindowTextW(hBtnSource,
            g_cfg.dlSource == 1 ? L"下载源：仅官方"
          : g_cfg.dlSource == 2 ? L"下载源：仅 BMCLAPI" : L"下载源：自动");
        SaveConfig();
        g_status = L"下载源已切换";
        InvalidateRect(hwnd, nullptr, TRUE);
        break;
    }

    case ID_BTN_FULL: {
        g_cfg.winMode = (g_cfg.winMode + 1) % 3;
        SetWindowTextW(hBtnFull, (std::wstring(L"游戏窗口模式：") + WinModeLabel(g_cfg.winMode)).c_str());
        SaveConfig();
        g_status = (g_cfg.winMode == 0) ? L"窗口模式：854×480（不可调分辨率）"
                 : (g_cfg.winMode == 1) ? L"最大化：按桌面工作区大小开窗"
                                        : L"全屏模式";
        InvalidateRect(hwnd, nullptr, TRUE);
        break;
    }

    case ID_BTN_OVCANCEL:
        g_cancelLaunch = true;
        // 立刻终止已经起来的游戏进程
        {
            DWORD pid = g_gamePid.load();
            HANDLE hp = g_gameProc;
            if (hp) {
                TerminateProcess(hp, 0);
                Log("[取消] 已强制结束游戏进程（句柄）");
            } else if (pid != 0) {
                HANDLE h2 = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
                if (h2) {
                    TerminateProcess(h2, 0);
                    CloseHandle(h2);
                    Log("[取消] 已强制结束游戏进程（PID " + std::to_string(pid) + "）");
                }
            }
        }
        g_overlay = false;
        ApplyOverlayVisibility();
        InvalidateRect(hwnd, nullptr, TRUE);
        break;

    case ID_BTN_LAUNCH:
            if (g_launching) return 0;
            if (g_selVersion < 0 || g_selVersion >= (int)g_versions.size()) {
                g_status = L"请先选择版本";
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            {
                std::string ver = g_versions[g_selVersion];
                g_overlay = true;
                g_overlayDoneTick = 0;
                g_stepCur = 0;
                g_cancelLaunch = false;
                g_launching = true;
                SetWindowTextW(hBtnOverlayCancel, L"取消");
                ApplyOverlayVisibility();
                std::thread(LaunchThread, ver).detach();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;

        case ID_BTN_DELACC: {
        if (g_cfg.activeAcc < 0 || g_cfg.activeAcc >= (int)g_cfg.accounts.size()) {
            g_status = L"没有可删除的账户";
            InvalidateRect(hwnd, nullptr, TRUE);
            break;
        }
        g_cfg.accounts.erase(g_cfg.accounts.begin() + g_cfg.activeAcc);
        if (g_cfg.accounts.empty()) {
            g_cfg.activeAcc = -1;
            g_cfg.authType = "offline";
            g_cfg.authToken.clear();
            g_cfg.authUuid.clear();
            g_cfg.skinPath.clear();
            LoadSkinBitmap();
        } else {
            if (g_cfg.activeAcc >= (int)g_cfg.accounts.size()) g_cfg.activeAcc = (int)g_cfg.accounts.size() - 1;
            ApplyActiveAccount();
    std::thread(UpdateThread, false).detach();
            LoadSkinBitmap();
        }
        SaveConfig();
        g_status = L"账户已删除";
        InvalidateRect(hwnd, nullptr, TRUE);
        break;
    }

    case ID_BTN_SKIN: {
        OPENFILENAMEW ofn{};
        wchar_t file[MAX_PATH] = L"";
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = L"皮肤 PNG (*.png)\0*.png\0所有文件\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (!GetOpenFileNameW(&ofn)) break;

        std::wstring err;
        if (ApplySkin(WToU8(file), err)) {
            g_status = L"皮肤已应用：" + fs::u8path(g_cfg.skinPath).filename().wstring();
        } else {
            g_status = L"更换皮肤失败：" + err;
            MessageBoxW(hwnd, err.c_str(), L"更换皮肤", MB_OK | MB_ICONWARNING);
        }
        InvalidateRect(hwnd, nullptr, TRUE);
        break;
    }

    case ID_BTN_LOGIN: {
            if (g_loginState == 1) return 0;
            std::string srv  = WToU8(CtlText(hEdServer));
            std::string user = WToU8(CtlText(hEdUser));
            std::string pass = WToU8(CtlText(hEdPass));
            if (srv.empty() || user.empty()) {
                g_status = L"请填写认证服务器和用户名";
                g_loginState = 3;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            g_loginState = 1;
            g_status = L"正在登录…";
            InvalidateRect(hwnd, nullptr, FALSE);
            std::thread(LoginThread, srv, user, pass).detach();
            break;
        }

        case ID_BTN_OFFLINE: {
            std::string nm = WToU8(CtlText(hEdName));
            if (nm.empty()) nm = "玩家" + std::to_string(g_cfg.accounts.size() + 1);
            CaptureActiveAccount();
            int idx = UpsertAccount("offline", nm);
            g_cfg.activeAcc = idx;
            ApplyActiveAccount();
    std::thread(UpdateThread, false).detach();
            SaveConfig();
            g_loginState = 0;
            g_status = L"已切换到离线账户：" + U8ToW(nm);
            InvalidateRect(hwnd, nullptr, TRUE);
            break;
        }

        case ID_BTN_LOGOUT:
            g_cfg.authType = "offline";
            g_cfg.authToken.clear();
            g_cfg.authName.clear();
            g_cfg.authUuid.clear();
            SaveConfig();
            g_loginState = 0;
            g_status = L"已退出第三方账号，回到离线模式";
            InvalidateRect(hwnd, nullptr, FALSE);
            break;

        case ID_BTN_INSTARROW:
        if (g_versions.empty()) ScanVersions();
        ShowDropPopup(2);
        break;

    case ID_BTN_DIRDROP:
            if (g_dirCands.empty()) RefreshDirCands();
            ShowDropPopup(0);
            break;

        case ID_BTN_JAVADROP:
            if (g_javaCands.empty()) RefreshJavaCands();
            ShowDropPopup(1);
            break;

        case ID_BTN_AUTODIR: {
            std::string d = DetectGameDir();
            if (d.empty()) { g_status = L"没找到 .minecraft，请手动浏览指定"; }
            else { g_cfg.gameDir = d; SetWindowTextW(hEdDir, U8ToW(d).c_str()); SaveConfig(); ScanVersions(); g_status = L"已自动检测游戏目录"; }
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        }
        case ID_BTN_BROWSEDIR: {
            std::string d;
            if (PickFolder(hwnd, d)) {
                g_cfg.gameDir = d;
                SetWindowTextW(hEdDir, U8ToW(d).c_str());
                SaveConfig(); ScanVersions();
                g_status = L"游戏目录已设置";
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        }
        case ID_BTN_AUTOJAVA: {
            std::string j = DetectJava();
            if (j.empty()) { g_status = L"没找到 Java，请手动浏览指定"; }
            else { g_cfg.javaPath = j; SetWindowTextW(hEdJava, U8ToW(j).c_str()); SaveConfig(); g_status = L"已自动检测 Java"; }
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        }
        case ID_BTN_BROWSEJAVA: {
            std::string j;
            if (PickJava(hwnd, j)) {
                g_cfg.javaPath = j;
                SetWindowTextW(hEdJava, U8ToW(j).c_str());
                SaveConfig();
                g_status = L"Java 路径已设置";
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        }
        case ID_BTN_ISO:
        ShowDropPopup(3);
        break;

        case ID_ED_SEARCH:
            if (HIWORD(wp) == EN_CHANGE) {
                RebuildDlView();
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            break;

    case ID_BTN_SAVE: {
            g_cfg.gameDir = WToU8(CtlText(hEdDir));
            g_cfg.javaPath = WToU8(CtlText(hEdJava));
            g_cfg.playerName = WToU8(CtlText(hEdName));
            int mem = _wtoi(CtlText(hEdMem).c_str());
            if (mem < 512) mem = 512;
            if (mem > 16384) mem = 16384;
            g_cfg.maxMemMB = mem;
    {
        std::string ts = WToU8(CtlText(hEdThreads));
        int n = ts.empty() ? 4 : atoi(ts.c_str());
        if (n < 1) n = 1;
        if (n > 128) n = 128;
        g_cfg.dlThreads = n;
        SetWindowTextW(hEdThreads, std::to_wstring(n).c_str());
    }
    g_cfg.extraJvm  = WToU8(CtlText(hEdJvm));
    g_cfg.extraGame = WToU8(CtlText(hEdGame));
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
            ScanVersions();
            g_status = L"设置已保存";
            InvalidateRect(hwnd, nullptr, FALSE);
            break;
        }
        }
        return 0;
    }

    case WM_DROP_PICK: {
        int idx = (int)wp;
        int mode = (int)lp;
        if (mode == 7) {
            if (idx >= 0 && idx < 4) {
                g_dlFilter = idx;
                const wchar_t* names[] = { L"正式版", L"快照版", L"远古版", L"愚人节" };
                SetWindowTextW(hBtnDlFilterSel, (std::wstring(names[idx]) + L"  ▾").c_str());
                g_dlSel = -1;
                RebuildDlView();
            }
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        if (mode == 3) {
            if (idx >= 0 && idx < 3) {
                g_cfg.isoMode = idx;
                SetWindowTextW(hBtnIso, (std::wstring(L"版本隔离：") + IsoLabel() + L"  ▾").c_str());
                SaveConfig();
                const wchar_t* tips[] = {
                    L"已设为：隔离所有实例",
                    L"已设为：隔离模组实例（带 mods 或 Forge/Fabric 的实例才隔离）",
                    L"已设为：不隔离实例（模组读 .minecraft\\mods）"
                };
                g_status = tips[idx];
            }
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        if (mode == 2) {
            if (idx >= 0 && idx < (int)g_versions.size()) {
                g_selVersion = idx;
                g_status = L"已切换到实例：" + U8ToW(g_versions[idx]);
            }
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        bool isDir = (mode == 0);
        if (isDir && idx >= 0 && idx < (int)g_dirCands.size()) {
            g_cfg.gameDir = g_dirCands[idx].path;
            SetWindowTextW(hEdDir, U8ToW(g_cfg.gameDir).c_str());
            SaveConfig();
            ScanVersions();
            g_status = L"已选择游戏目录";
        } else if (!isDir && idx >= 0 && idx < (int)g_javaCands.size()) {
            g_cfg.javaPath = g_javaCands[idx].path;
            SetWindowTextW(hEdJava, U8ToW(g_cfg.javaPath).c_str());
            SaveConfig();
            g_status = L"已选择 Java：" + U8ToW(g_javaCands[idx].label);
        }
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }

    case WM_APP_LOGIN:
        g_loginState = (g_loginState == 2) ? 2 : 3;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_APP_UPD: {
        bool interactive = (wp != 0);
        int st = g_updateState.load();
        if (st == 2) {
            std::wstring msg = std::wstring(L"发现新版本 ") + g_newTag + L"\n当前版本 " APP_TAG L"\n\n是否现在更新？";
            if (MessageBoxW(hwnd, msg.c_str(), L"检查更新", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                StartUpdate();
            }
        } else if (st == 3 && interactive) {
            MessageBoxW(hwnd, (std::wstring(L"已是最新版本（") + APP_TAG + L"）").c_str(), L"检查更新", MB_OK | MB_ICONINFORMATION);
        } else if (st == 4 && interactive) {
            MessageBoxW(hwnd, L"检查更新失败，请检查网络后重试。", L"检查更新", MB_OK | MB_ICONWARNING);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_APP_NEEDJAVA: {
        int need = (int)wp;
        if (g_overlay) { g_overlay = false; ApplyOverlayVisibility(); }
        std::wstring msg = L"当前版本需要 Java " + std::to_wstring(need) +
                           L"，但本机没有检测到合适的 Java。\n\n是否打开 Adoptium 下载页面？";
        if (MessageBoxW(hwnd, msg.c_str(), L"缺少 Java", MB_YESNO | MB_ICONWARNING) == IDYES) {
            std::wstring url = L"https://adoptium.net/temurin/releases/?version=" + std::to_wstring(need);
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }

    case WM_APP_ERROR:
        if (g_overlay) { g_overlay = false; ApplyOverlayVisibility(); }
        ShowErrorWindow();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_APP_DONE:
        if (g_overlay && g_stepCur.load() >= 4 && g_overlayDoneTick == 0)
            g_overlayDoneTick = GetTickCount();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_TIMER:
        if (g_overlay) {
            if (g_stepCur.load() >= 4) {
                SetWindowTextW(hBtnOverlayCancel, L"关闭");
                // 窗口已出现：1.5 秒后自动收起，让主界面可用
                if (g_overlayDoneTick && GetTickCount() - g_overlayDoneTick > 1500) {
                    g_overlay = false;
                    g_overlayDoneTick = 0;
                    ApplyOverlayVisibility();
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        else if (g_launching || g_downloading || g_progState != 0) InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================ 入口 ============================
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR lpCmdLine, int) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    typedef BOOL(WINAPI* SetCtx)(void*);
    auto pSetCtx = (SetCtx)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (pSetCtx) pSetCtx((void*)(intptr_t)-4);

    HDC screen = GetDC(nullptr);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    if (g_dpi <= 0) g_dpi = 96;

    g_fTitle = CreateFontW(-Scale(22), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    g_fUi    = CreateFontW(-Scale(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    g_fSmall = CreateFontW(-Scale(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");

    g_brInput = CreateSolidBrush(C_INPUTBG);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    wchar_t appdata[MAX_PATH];
    SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata);
    std::error_code ec;
    // 便携模式：exe 同目录有 config.json 就用它
    fs::path portable = ExeDir() / L"config.json";
    if (FileExists(portable)) {
        g_cfgPath = WToU8(portable.wstring());
    } else {
        fs::path cfgDir = fs::path(appdata) / L"ReignLaunch";
        fs::create_directories(cfgDir, ec);
        g_cfgPath = WToU8((cfgDir / L"config.json").wstring());

        // 从旧的 MCLite 目录迁移一次配置
        if (!FileExists(fs::u8path(g_cfgPath))) {
            fs::path oldCfg = fs::path(appdata) / L"MCLite" / L"config.json";
            if (FileExists(oldCfg)) {
                std::error_code e2;
                fs::copy_file(oldCfg, fs::u8path(g_cfgPath), fs::copy_options::overwrite_existing, e2);
            }
        }
    }
    g_dataDir = WToU8(fs::path(g_cfgPath).parent_path().wstring());
    LoadConfig();
    ApplyActiveAccount();
    std::thread(UpdateThread, false).detach();


    Gdiplus::GdiplusStartupInput gpsi;
    g_gpOk = (Gdiplus::GdiplusStartup(&g_gpToken, &gpsi, nullptr) == Gdiplus::Ok);
    LoadSkinBitmap();

    if (g_cfg.gameDir.empty()) g_cfg.gameDir = DetectGameDir();
    if (g_cfg.javaPath.empty()) g_cfg.javaPath = DetectJava();
    {
        std::string emb = ExtractEmbeddedAuthlib();
        if (!emb.empty()) {
            g_cfg.authlibPath = emb;
            Log("[内置] authlib-injector 已释放: " + emb);
        } else if (g_cfg.authlibPath.empty()) {
            g_cfg.authlibPath = DetectAuthlib();
        }
    }

    ScanVersions();

    // --dump 自检
    std::string cmdline = lpCmdLine ? lpCmdLine : "";
    auto trimCmd = [](std::string s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '"' || s.front() == '\t')) s.erase(s.begin());
        while (!s.empty() && (s.back() == ' ' || s.back() == '"' || s.back() == '\t')) s.pop_back();
        return s;
    };
    cmdline = trimCmd(cmdline);
    if (cmdline.rfind("--dump", 0) == 0) {
        std::string ver = trimCmd(cmdline.substr(6));
        if (ver.empty() && !g_versions.empty()) ver = g_versions[0];

        LaunchPlan plan;
        plan.maxMem = g_cfg.maxMemMB;
        std::string err;
        bool ok = BuildLaunchPlan(ver, plan, err);

        std::ofstream f{ fs::path(ExeDir().wstring() + L"\\dump.txt") };
        if (f) f << (ok ? WToU8(plan.commandLine) : ("FAILED: " + err));
        return ok ? 0 : 1;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc   = MainProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"ReignLaunchMain";
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.hIcon         = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, L"ReignLaunchMain", L"ReignLaunch - Minecraft 启动器",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, Scale(1060), Scale(680),
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 0;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    ShowPage(PAGE_LAUNCH);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
