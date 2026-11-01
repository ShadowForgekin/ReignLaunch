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
enum Page { PAGE_LAUNCH = 0, PAGE_USER = 1, PAGE_SETTINGS = 2, PAGE_ABOUT = 3, PAGE_INSTANCES = 4 };

// ============================ 配置 ============================
struct Config {
    std::string gameDir;
    int  isoMode = 0;               // 版本隔离：0=隔离所有实例 1=仅模组实例 2=不隔离
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
static int         g_dpi = 96;

static std::mutex               g_logMutex;
static std::vector<std::string> g_logs;

static HWND g_hwnd = nullptr;
static HFONT g_fTitle = nullptr, g_fUi = nullptr, g_fSmall = nullptr;

// ============================ 控件 ============================
static HWND hBtnTabLaunch, hBtnTabUser, hBtnTabSettings, hBtnTabAbout, hBtnTabInst;
static HWND hBtnLaunch, hBtnFill, hBtnRefresh, hBtnInstArrow, hBtnOverlayCancel;
static std::string JavaVersionOf(const fs::path& javaw);
static int RequiredJavaMajor(const std::string& id);
static std::string PickJavaFor(int need);
static RECT OverlayCardRect();
static void ApplyOverlayVisibility();

static HWND hLblServer, hEdServer, hLblUser, hEdUser, hLblPass, hEdPass;
static HWND hBtnLogin, hBtnLogout, hBtnOffline;

static HWND hLblDir, hEdDir, hBtnAutoDir, hBtnBrowseDir, hBtnDirDrop;
static HWND hLblJava, hEdJava, hBtnAutoJava, hBtnBrowseJava, hBtnJavaDrop;
static HWND hLblMem, hEdMem, hEdName, hLblName, hBtnSave, hBtnIso;

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
    if (j.contains("versionIsolation"))            // 兼容上一版布尔配置
        g_cfg.isoMode = j.value("versionIsolation", true) ? 0 : 2;
        g_cfg.javaPath       = j.value("javaPath", std::string());
        g_cfg.maxMemMB       = j.value("maxMemMB", 4096);
        g_cfg.playerName     = j.value("playerName", std::string("Steve"));
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
    if (!HttpRequest(url, "GET", "", "", data, err)) {
        std::string mirror = ToMirror(url);
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

    for (auto& a : jvmArgs) cmd += L" " + U8ToW(QuoteIfNeeded(a));
    cmd += L" -cp \"" + U8ToW(cp) + L"\"";
    cmd += L" " + U8ToW(mainClass);
    for (auto& a : gameArgs) cmd += L" " + U8ToW(QuoteIfNeeded(a));

    plan.commandLine = cmd;
    plan.workDir = U8ToW(gameDirEff);
    plan.javaPath = U8ToW(javaExe);
    return true;
}

// ============================ 下载 ============================
struct DownloadStats { int existed = 0, downloaded = 0, failed = 0; };

static void DownloadMissing(const std::string& versionId, DownloadStats& st) {
    fs::path mcDir = fs::u8path(g_cfg.gameDir);

    // 版本隔离：把 gameDir 指向 versions/<id>，让 Forge 用该版本自己的 mods
    std::string gameDirEff = g_cfg.gameDir;
    if (ShouldIsolate(versionId, mcDir)) {
        gameDirEff = WToU8((mcDir / L"versions" / fs::u8path(versionId)).wstring());
    }
    fs::path libRoot = mcDir / L"libraries";
    json vj = ResolveVersion(versionId);
    if (!vj.is_object()) { st.failed++; return; }

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
            if (FileExists(dest)) {
                if (!sha1.empty()) {
                    std::ifstream f{ dest, std::ios::binary };
                    std::vector<unsigned char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                    if (Sha1Hex(buf) != sha1) {
                        std::string err;
                        if (DownloadFile(url, dest, err)) st.downloaded++;
                        else st.failed++;
                        continue;
                    }
                }
                st.existed++;
                continue;
            }
            std::string err;
            if (DownloadFile(url, dest, err)) { st.downloaded++; }
            else { st.failed++; Log("[失败] " + rel + " : " + err); }
        }
    }

    fs::path clientJar = mcDir / L"versions" / fs::u8path(versionId) / fs::u8path(versionId + ".jar");
    if (vj.contains("downloads") && vj["downloads"].is_object() && vj["downloads"].contains("client")) {
        std::string url = vj["downloads"]["client"].value("url", std::string());
        if (!url.empty()) {
            if (FileExists(clientJar)) st.existed++;
            else {
                std::string err;
                if (DownloadFile(url, clientJar, err)) st.downloaded++;
                else st.failed++;
            }
        }
    }

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
            int present = 0, fetched = 0, failedA = 0;
            for (auto& it : idx["objects"].items()) {
                std::string hash = it.value().value("hash", std::string());
                if (hash.size() < 3) continue;
                fs::path obj = mcDir / L"assets" / L"objects" / fs::u8path(hash.substr(0, 2)) / fs::u8path(hash);
                if (FileExists(obj)) { present++; continue; }
                std::string err;
                if (DownloadFile("https://resources.download.minecraft.net/" + hash.substr(0, 2) + "/" + hash, obj, err))
                    fetched++;
                else failedA++;
            }
            st.existed += present;
            st.downloaded += fetched;
            st.failed += failedA;
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
    g_stepCur = 4;   // 全部完成
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
    Log("[成功] 游戏已启动，PID=" + std::to_string(pi.dwProcessId));
    CloseHandle(pi.hThread);
    g_gameProc = pi.hProcess;

    std::thread reader(PumpOutput, rd);
    reader.detach();

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    Log("=========== 游戏退出，退出码 " + std::to_string(code) + " ===========");
    CloseHandle(pi.hProcess);
    g_gameProc = nullptr;
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
static void LayoutControls() {
    int w = g_ly.content.right, h = g_ly.content.bottom;

    // 顶部标签
    HWND tabBtns[4] = { hBtnTabLaunch, hBtnTabInst, hBtnTabUser, hBtnTabSettings };
    for (int i = 0; i < 4; ++i) {
        RECT r = TabRect(i);
        MoveWindow(tabBtns[i], r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
    }

    // 启动页
    {
        RECT b = LaunchBtnRect();
        MoveWindow(hBtnLaunch, b.left, b.top, b.right - b.left, b.bottom - b.top, TRUE);
        MoveWindow(hBtnInstArrow, b.right + Scale(8), b.top, Scale(44), b.bottom - b.top, TRUE);

        RECT card = OverlayCardRect();
        MoveWindow(hBtnOverlayCancel, card.right - Scale(140), card.bottom - Scale(58), Scale(104), Scale(38), TRUE);
        RECT s0 = SmallBtnRect(0), s1 = SmallBtnRect(1);
        MoveWindow(hBtnFill, s0.left, s0.top, s0.right - s0.left, s0.bottom - s0.top, TRUE);
        MoveWindow(hBtnRefresh, s1.left, s1.top, s1.right - s1.left, s1.bottom - s1.top, TRUE);
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

        MoveWindow(hBtnSave, x + Scale(110), y + rowH * 3 + Scale(10), Scale(140), Scale(40), TRUE);
        MoveWindow(hBtnIso,  x + Scale(270), y + rowH * 3 + Scale(10), Scale(240), Scale(40), TRUE);
    }
}

static void SyncSettingsVisibility();

static void ShowPage(Page p) {
    g_page = p;
    bool L = (p == PAGE_LAUNCH), U = (p == PAGE_USER), S = (p == PAGE_SETTINGS);
    if (p != PAGE_SETTINGS) { g_showDirList = false; g_showJavaList = false; }

    // 顶部标签必须始终可见，否则切不到别的页
    ShowWindow(hBtnTabLaunch, SW_SHOW);
    ShowWindow(hBtnTabUser, SW_SHOW);
    ShowWindow(hBtnTabSettings, SW_SHOW);
    ShowWindow(hBtnTabInst, SW_SHOW);



    ApplyOverlayVisibility();

    ShowWindow(hEdServer, U); ShowWindow(hEdUser, U); ShowWindow(hEdPass, U);
    ShowWindow(hBtnLogin, U); ShowWindow(hBtnLogout, U); ShowWindow(hEdName, U); ShowWindow(hBtnOffline, U);

    ShowWindow(hEdDir, S); ShowWindow(hBtnAutoDir, S); ShowWindow(hBtnBrowseDir, S); ShowWindow(hBtnDirDrop, S);
    ShowWindow(hEdJava, S); ShowWindow(hBtnAutoJava, S); ShowWindow(hBtnBrowseJava, S); ShowWindow(hBtnJavaDrop, S);
    ShowWindow(hEdMem, S); ShowWindow(hBtnSave, S); ShowWindow(hBtnIso, S);

    LayoutControls();
    SyncSettingsVisibility();
    InvalidateRect(g_hwnd, nullptr, TRUE);
}

// ---------------- 绘制 ----------------
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
        { L"版本",   L"v1.0.0 Beta 2" },
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

// ============================ 启动浮层 ============================
static RECT OverlayCardRect() {
    RECT c = g_ly.content;
    int w = Scale(560), h = Scale(310);
    int cx = (c.left + c.right) / 2;
    int cy = (c.top + c.bottom) / 2;
    return { cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2 };
}

static void ApplyOverlayVisibility() {
    bool L = (g_page == PAGE_LAUNCH) && !g_overlay;
    ShowWindow(hBtnLaunch, L ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnFill,   L ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnRefresh, L ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnInstArrow, L ? SW_SHOW : SW_HIDE);
    ShowWindow(hBtnOverlayCancel, g_overlay ? SW_SHOW : SW_HIDE);
}

static void DrawOverlay(HDC dc) {
    RECT c = g_ly.content;
    HBRUSH bg = CreateSolidBrush(RGB(16, 17, 20));
    FillRect(dc, &c, bg);
    DeleteObject(bg);

    RECT card = OverlayCardRect();
    FillRoundRect(dc, card, 12, C_CARD);

    int x = card.left + Scale(30);
    RECT ttl = { x, card.top + Scale(18), card.right - Scale(30), card.top + Scale(48) };
    DrawTextIn(dc, L"启动游戏", ttl, C_TEXT, g_fUi);

    const wchar_t* steps[4] = { L"检测 Java 版本", L"处理游戏依赖", L"登录", L"等待游戏启动" };
    int done = g_stepCur.load();
    int yy = card.top + Scale(62);
    for (int i = 0; i < 4; ++i) {
        std::wstring line = (i < done) ? (std::wstring(L"✓  ") + steps[i])
                          : (i == done) ? (std::wstring(L"→  ") + steps[i])
                                        : (std::wstring(L"…  ") + steps[i]);
        COLORREF col = (i < done) ? C_OK : (i == done ? C_TEXT : C_DIM);
        RECT r = { x, yy, card.right - Scale(30), yy + Scale(28) };
        DrawTextIn(dc, line, r, col, g_fUi);
        yy += Scale(32);
    }

    int total = g_progTotal.load(), cur = g_progCur.load();
    bool downloading = (g_progState.load() == 1 && total > 0);
    double ratio = downloading ? ((double)cur / total) : (done / 4.0);
    if (ratio > 1.0) ratio = 1.0;

    RECT bar = { x, yy + Scale(10), card.right - Scale(30), yy + Scale(20) };
    FillRoundRect(dc, bar, 5, RGB(42, 44, 50));
    if (ratio > 0) {
        RECT f = bar;
        f.right = bar.left + (int)((bar.right - bar.left) * ratio);
        FillRoundRect(dc, f, 5, C_ACCENT2);
    }

    std::wstring st;
    if (downloading)
        st = L"正在补全文件 " + std::to_wstring(cur) + L" / " + std::to_wstring(total);
    else if (done >= 4)
        st = L"游戏已启动，出错的日志会单独弹出";
    else if (done == 3)
        st = L"正在处理启动参数…";
    else
        st = L"请耐心等待…";
    RECT sr = { x, yy + Scale(28), card.right - Scale(160), yy + Scale(54) };
    DrawTextIn(dc, st, sr, C_DIM, g_fSmall);
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
    const wchar_t* titles[] = { L"", L"用户", L"设置", L"关于", L"" };
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

        RECT lbl = { x, top, g_ly.content.right - Scale(24), top + Scale(26) };
        DrawTextIn(dc, L"实例 (" + std::to_wstring(g_versions.size()) + L")", lbl, C_DIM, g_fSmall);

        int itemH = Scale(56);
        int y = top + Scale(34) - g_listScroll;
        for (size_t i = 0; i < g_versions.size(); ++i) {
            RECT item = { x, y, g_ly.content.right - Scale(24), y + itemH - Scale(8) };
            if (item.bottom > top + Scale(34) && item.top < g_ly.content.bottom) {
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

        if (!g_status.empty()) {
            RECT st = { x, y + rowH * 4 + Scale(56), g_ly.main.right - Scale(40), g_ly.content.bottom - Scale(20) };
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
    else                GetWindowRect(hBtnIso, &er);

    int itemH = Scale(28);
    int w = Scale(mode == 2 ? 300 : (mode == 3 ? 300 : 420));
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

        hBtnLaunch  = MakeBtn(hwnd, L"启动游戏", ID_BTN_LAUNCH);
        hBtnFill    = MakeBtn(hwnd, L"补全文件", ID_BTN_FILL);
        hBtnRefresh = MakeBtn(hwnd, L"刷新版本", ID_BTN_REFRESH);
        hBtnInstArrow = MakeBtn(hwnd, L"", ID_BTN_INSTARROW);
        hBtnOverlayCancel = MakeBtn(hwnd, L"取消", ID_BTN_OVCANCEL);

        hEdServer = MakeEdit(hwnd, ID_ED_SERVER, U8ToW(g_cfg.authServer).c_str());
        hEdUser   = MakeEdit(hwnd, ID_ED_USER, L"");
        hEdPass   = MakeEdit(hwnd, ID_ED_PASS, L"", ES_PASSWORD);
        hBtnLogin = MakeBtn(hwnd, L"登录", ID_BTN_LOGIN);
        hBtnLogout= MakeBtn(hwnd, L"退出登录", ID_BTN_LOGOUT);
        hBtnOffline = MakeBtn(hwnd, L"使用离线", ID_BTN_OFFLINE);

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
        bool tab = (id == ID_TAB_LAUNCH || id == ID_TAB_USER || id == ID_TAB_SET || id == ID_TAB_ABOUT || id == ID_TAB_INST);
        bool activeTab = tab && ((id == ID_TAB_LAUNCH && g_page == PAGE_LAUNCH) ||
                                 (id == ID_TAB_USER && g_page == PAGE_USER) ||
                                 (id == ID_TAB_SET && g_page == PAGE_SETTINGS) ||
                                 (id == ID_TAB_ABOUT && g_page == PAGE_ABOUT) ||
                                 (id == ID_TAB_INST && g_page == PAGE_INSTANCES));

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
                    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
                }).detach();
            }
            break;

        case ID_BTN_OVCANCEL:
        if (g_launching && g_stepCur.load() < 4) g_cancelLaunch = true;
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
                g_stepCur = 0;
                g_cancelLaunch = false;
                g_launching = true;
                SetWindowTextW(hBtnOverlayCancel, L"取消");
                ApplyOverlayVisibility();
                std::thread(LaunchThread, ver).detach();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;

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

        case ID_BTN_OFFLINE:
            g_cfg.authType = "offline";
            g_cfg.playerName = WToU8(CtlText(hEdName));
            SaveConfig();
            g_loginState = 0;
            g_status = L"已切换到离线账号：" + U8ToW(g_cfg.playerName);
            InvalidateRect(hwnd, nullptr, FALSE);
            break;

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

    case ID_BTN_SAVE: {
            g_cfg.gameDir = WToU8(CtlText(hEdDir));
            g_cfg.javaPath = WToU8(CtlText(hEdJava));
            g_cfg.playerName = WToU8(CtlText(hEdName));
            int mem = _wtoi(CtlText(hEdMem).c_str());
            if (mem < 512) mem = 512;
            if (mem > 16384) mem = 16384;
            g_cfg.maxMemMB = mem;
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
        if (mode == 3) {
            if (idx >= 0 && idx < 3) {
                g_cfg.isoMode = idx;
                SetWindowTextW(hBtnIso, (std::wstring(L"版本隔离：") + IsoLabel() + L"  ▾").c_str());
                SaveConfig();
                const wchar_t* tips[] = {
                    L"已设为：隔离所有实例",
                    L"已设为：隔离模组实例（带 mods 或 Forge/Fabric 的实例才隔离）",
                    L"已设为：不隔离实例（模组读 .minecraft\mods）"
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
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_TIMER:
        if (g_overlay) {
            if (g_stepCur.load() >= 4) {
                SetWindowTextW(hBtnOverlayCancel, L"关闭");
                if (!g_launching) { g_overlay = false; ApplyOverlayVisibility(); }
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
