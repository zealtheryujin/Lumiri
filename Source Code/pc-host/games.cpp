#include "common.h"
#include <shellapi.h>
#include <shlobj.h>
#include <cctype>
#include <string>
#include <vector>
#include <algorithm>

static std::wstring utf8ToW(const std::string& s);
static std::string wToUtf8(const std::wstring& w);

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

struct Game {
    uint8_t type;
    std::string name, launch, args, dir;
    std::string iconExe;
    std::vector<uint8_t> icon;
};
static std::vector<Game> g_games;

static std::string readFile(const std::string& path) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return {};
    std::string s;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

static std::string extractQuoted(const std::string& text, const char* key, size_t from = 0) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = text.find(pat, from);
    if (p == std::string::npos) return {};
    p = text.find('"', p + pat.size());
    if (p == std::string::npos) return {};
    size_t e = p + 1;
    std::string out;
    while (e < text.size() && text[e] != '"') {
        if (text[e] == '\\' && e + 1 < text.size()) { out += text[e + 1]; e += 2; }
        else out += text[e++];
    }
    return out;
}

static bool junkExeName(const char* n) {
    static const char* junk[] = { "unins", "crash", "redist", "vcredist", "dxsetup",
        "setup", "eac", "easyanticheat", "prereq", "installer" };
    char low[MAX_PATH];
    size_t i = 0;
    for (; n[i] && i < MAX_PATH - 1; i++) low[i] = (char)tolower((unsigned char)n[i]);
    low[i] = 0;
    for (auto j : junk) if (strstr(low, j)) return true;
    return false;
}

static void findLargestExe(const std::string& dir, int depth,
                           std::string& best, uint64_t& bestSize) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0) findLargestExe(dir + "\\" + fd.cFileName, depth - 1, best, bestSize);
        } else {
            size_t l = strlen(fd.cFileName);
            if (l > 4 && _stricmp(fd.cFileName + l - 4, ".exe") == 0 && !junkExeName(fd.cFileName)) {
                uint64_t sz = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                if (sz > bestSize) { bestSize = sz; best = dir + "\\" + fd.cFileName; }
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static bool junkSteamApp(const std::string& name) {
    static const char* junk[] = { "Redistributable", "Steamworks", "Proton",
        "Steam Linux", "SteamVR", "Runtime" };
    for (auto j : junk) if (name.find(j) != std::string::npos) return true;
    return false;
}

static void scanSteam() {
    char steamPath[MAX_PATH] = "";
    DWORD len = sizeof(steamPath);
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath",
        RRF_RT_REG_SZ, nullptr, steamPath, &len) != ERROR_SUCCESS) return;

    std::string vdf = readFile(std::string(steamPath) + "/steamapps/libraryfolders.vdf");
    std::vector<std::string> libs;
    size_t pos = 0;
    while (true) {
        size_t p = vdf.find("\"path\"", pos);
        if (p == std::string::npos) break;
        std::string lib = extractQuoted(vdf, "path", p ? p - 1 : 0);
        if (!lib.empty()) libs.push_back(lib);
        pos = p + 6;
    }
    if (libs.empty()) libs.push_back(steamPath);

    for (auto& lib : libs) {
        WIN32_FIND_DATAA fd;
        std::string dir = lib + "\\steamapps\\";
        HANDLE h = FindFirstFileA((dir + "appmanifest_*.acf").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::string acf = readFile(dir + fd.cFileName);
            std::string appid = extractQuoted(acf, "appid");
            std::string name = extractQuoted(acf, "name");
            if (appid.empty() || name.empty() || junkSteamApp(name)) continue;
            Game g{ RP_GAME_STEAM, name, "steam://rungameid/" + appid, "", "", "", {} };
            std::string inst = extractQuoted(acf, "installdir");
            if (!inst.empty()) {
                uint64_t sz = 0;
                findLargestExe(dir + "common\\" + inst, 2, g.iconExe, sz);
            }
            g_games.push_back(std::move(g));
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
}

static void scanEpic() {
    char pd[MAX_PATH] = "C:\\ProgramData";
    GetEnvironmentVariableA("ProgramData", pd, sizeof(pd));
    std::string dir = std::string(pd) + "\\Epic\\EpicGamesLauncher\\Data\\Manifests\\";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "*.item").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string js = readFile(dir + fd.cFileName);
        std::string name = extractQuoted(js, "DisplayName");
        std::string app = extractQuoted(js, "AppName");
        if (name.empty() || app.empty()) continue;
        if (js.find("\"bIsIncompleteInstall\": true") != std::string::npos) continue;
        Game g{ RP_GAME_EPIC, name,
            "com.epicgames.launcher://apps/" + app + "?action=launch&silent=true", "", "", "", {} };
        std::string loc = extractQuoted(js, "InstallLocation");
        std::string exe = extractQuoted(js, "LaunchExecutable");
        if (!loc.empty() && !exe.empty()) g.iconExe = loc + "\\" + exe;
        g_games.push_back(std::move(g));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static std::string exeDir() {
    char p[MAX_PATH];
    GetModuleFileNameA(nullptr, p, sizeof(p));
    std::string s = p;
    size_t sl = s.find_last_of('\\');
    return sl == std::string::npos ? "." : s.substr(0, sl);
}

static void scanManual() {
    std::string path = exeDir() + "\\games.txt";
    std::string txt = readFile(path);
    if (txt.empty()) {
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "wb") == 0 && f) {
            fputs("# Lumiri ozel oyun listesi (UTF-8 kaydet)\n"
                  "# Format: Oyun Adi|C:\\Tam\\Yol\\oyun.exe|opsiyonel argumanlar\n"
                  "# Ornek:\n"
                  "# GTA V|D:\\Games\\GTAV\\PlayGTAV.exe|\n", f);
            fclose(f);
        }
        return;
    }
    size_t pos = 0;
    while (pos < txt.size()) {
        size_t e = txt.find('\n', pos);
        if (e == std::string::npos) e = txt.size();
        std::string line = txt.substr(pos, e - pos);
        pos = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t p1 = line.find('|');
        if (p1 == std::string::npos) continue;
        size_t p2 = line.find('|', p1 + 1);
        std::string name = line.substr(0, p1);
        std::string exe = line.substr(p1 + 1, p2 == std::string::npos ? std::string::npos : p2 - p1 - 1);
        std::string args = p2 == std::string::npos ? "" : line.substr(p2 + 1);
        if (name.empty() || exe.empty()) continue;
        size_t sl = exe.find_last_of('\\');
        g_games.push_back({ RP_GAME_CUSTOM, name, exe, args,
                            sl == std::string::npos ? "" : exe.substr(0, sl), exe, {} });
    }
}

static bool extractIcon64(const std::string& exeUtf8, std::vector<uint8_t>& out) {
    if (exeUtf8.empty()) return false;
    std::wstring exe = utf8ToW(exeUtf8);
    HICON hi = nullptr;
    SHDefExtractIconW(exe.c_str(), 0, 0, &hi, nullptr, RP_ICON_SIZE);
    if (!hi) {
        UINT r = PrivateExtractIconsW(exe.c_str(), 0, RP_ICON_SIZE, RP_ICON_SIZE, &hi, nullptr, 1, 0);
        if (r == 0 || r == (UINT)-1 || !hi) return false;
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = RP_ICON_SIZE;
    bi.bmiHeader.biHeight = -RP_ICON_SIZE;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(dc, bmp);
        memset(bits, 0, RP_ICON_SIZE * RP_ICON_SIZE * 4);
        DrawIconEx(dc, 0, 0, hi, RP_ICON_SIZE, RP_ICON_SIZE, 0, nullptr, DI_NORMAL);
        GdiFlush();
        out.assign((uint8_t*)bits, (uint8_t*)bits + RP_ICON_SIZE * RP_ICON_SIZE * 4);

        bool anyAlpha = false;
        for (size_t i = 3; i < out.size(); i += 4) if (out[i]) { anyAlpha = true; break; }
        if (!anyAlpha) for (size_t i = 3; i < out.size(); i += 4) out[i] = 255;
        SelectObject(dc, old);
        ok = true;
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(dc);
    DestroyIcon(hi);
    return ok;
}

void scanGames() {
    g_games.clear();
    scanSteam();
    scanEpic();
    scanManual();
    int icons = 0;
    for (auto& g : g_games)
        if (extractIcon64(g.iconExe, g.icon)) icons++;
    logmsg("Oyun taraması: %zu oyun bulundu (%d ikonlu)", g_games.size(), icons);
}

void sendGamesList(SOCKET c) {
    if (g_games.empty()) scanGames();
    uint16_t count = (uint16_t)(g_games.size() > 512 ? 512 : g_games.size());
    std::vector<char> buf(sizeof(RpGamesRespHdr) + (size_t)count * sizeof(RpGameEntry));
    RpGamesRespHdr* hdr = (RpGamesRespHdr*)buf.data();
    hdr->count = count; hdr->_pad = 0;
    RpGameEntry* e = (RpGameEntry*)(buf.data() + sizeof(RpGamesRespHdr));
    for (uint16_t i = 0; i < count; i++) {
        memset(&e[i], 0, sizeof(RpGameEntry));
        e[i].type = g_games[i].type;
        e[i].hasIcon = g_games[i].icon.empty() ? 0 : 1;
        strncpy_s(e[i].name, g_games[i].name.c_str(), _TRUNCATE);
    }
    send(c, buf.data(), (int)buf.size(), 0);
    for (uint16_t i = 0; i < count; i++)
        if (!g_games[i].icon.empty())
            send(c, (char*)g_games[i].icon.data(), (int)g_games[i].icon.size(), 0);
}

static std::wstring utf8ToW(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, 0);
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
static std::string wToUtf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, 0);
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

void launchGameIdx(uint16_t idx) {
    if (idx >= g_games.size()) scanGames();
    if (idx >= g_games.size()) { logmsg("Oyun indeksi geçersiz: %u", idx); return; }
    Game& g = g_games[idx];
    logmsg("Oyun başlatılıyor: %s", g.name.c_str());
    std::wstring args = utf8ToW(g.args), dir = utf8ToW(g.dir);
    ShellExecuteW(nullptr, L"open", utf8ToW(g.launch).c_str(),
                  args.empty() ? nullptr : args.c_str(),
                  dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL);
}

void handleBrowse(SOCKET c, const RpBrowseReq* req) {
    std::vector<RpBrowseEntry> dirs, exes;
    std::string path(req->path, strnlen(req->path, sizeof(req->path)));

    if (path.empty()) {
        wchar_t drv[256] = {};
        GetLogicalDriveStringsW(255, drv);
        for (wchar_t* d = drv; *d; d += wcslen(d) + 1) {
            RpBrowseEntry e{};
            e.type = RP_FS_DIR;
            std::wstring w(d);
            if (w.size() > 2) w.resize(2);
            strncpy_s(e.name, wToUtf8(w).c_str(), _TRUNCATE);
            dirs.push_back(e);
        }
    } else {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((utf8ToW(path) + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
                std::wstring n = fd.cFileName;
                if (n == L"." || n == L"..") continue;
                RpBrowseEntry e{};
                strncpy_s(e.name, wToUtf8(n).c_str(), _TRUNCATE);
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    e.type = RP_FS_DIR;
                    if (dirs.size() < 400) dirs.push_back(e);
                } else if (n.size() > 4 && _wcsicmp(n.c_str() + n.size() - 4, L".exe") == 0) {
                    e.type = RP_FS_EXE;
                    if (exes.size() < 100) exes.push_back(e);
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    auto byName = [](const RpBrowseEntry& a, const RpBrowseEntry& b) {
        return _stricmp(a.name, b.name) < 0; };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(exes.begin(), exes.end(), byName);
    dirs.insert(dirs.end(), exes.begin(), exes.end());

    RpBrowseRespHdr hdr{ (uint16_t)dirs.size(), 0 };
    send(c, (char*)&hdr, sizeof(hdr), 0);
    if (!dirs.empty())
        send(c, (char*)dirs.data(), (int)(dirs.size() * sizeof(RpBrowseEntry)), 0);
}

void handleAddGame(SOCKET c, const RpAddGameReq* req) {
    std::string name(req->name, strnlen(req->name, sizeof(req->name)));
    std::string path(req->path, strnlen(req->path, sizeof(req->path)));

    for (auto& ch : name) if (ch == '|' || ch == '\n' || ch == '\r') ch = ' ';
    uint8_t ok = 0;
    if (!name.empty() && !path.empty()) {
        FILE* f = nullptr;
        if (fopen_s(&f, (exeDir() + "\\games.txt").c_str(), "ab") == 0 && f) {
            fprintf(f, "%s|%s|\n", name.c_str(), path.c_str());
            fclose(f);
            ok = 1;
            logmsg("Oyun eklendi: %s -> %s", name.c_str(), path.c_str());
        }
    }
    send(c, (char*)&ok, 1, 0);
}
