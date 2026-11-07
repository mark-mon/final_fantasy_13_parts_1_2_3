// Start-up diagnostics (startup_diag = 1, default on).
//
// Written for a FF XIII start-up failure on one user's PC: with the mod, Steam's DRM wrapper shows "Application load
// error 3:0000065432" and the game exits before its first Direct3D call; without the mod it starts. Logs what can
// differ between PCs: Windows build, elevation, parent process and command line, Steam's process and elevation, the
// modules in the process (overlays, antivirus, other mods) and the files in the game folder. A thread then watches
// for new modules and for windows of this process (the error box and its text) until the game's first D3D call, so
// the log shows what happened during Steam's start-up check.
#include "globals.h"
#include <tlhelp32.h>
#include <string>
#include <set>

using namespace fvr;

static volatile LONG g_graphicsStarted = 0;
static HANDLE g_diagThread = nullptr;

static std::string Narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::string FileTimeStr(const FILETIME& ft) {
    FILETIME lft; SYSTEMTIME st;
    FileTimeToLocalFileTime(&ft, &lft);
    FileTimeToSystemTime(&lft, &st);
    char b[32];
    sprintf_s(b, "%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return b;
}

static void LogWindowsVersion() {
    typedef LONG(WINAPI * RtlGetVersion_t)(OSVERSIONINFOW*);
    RtlGetVersion_t f = (RtlGetVersion_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW v = {sizeof(v)};
    if (f && f(&v) == 0) LOGI("diag: Windows %lu.%lu build %lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    BOOL wow = FALSE;
    IsWow64Process(GetCurrentProcess(), &wow);
    LOGI("diag: 32-bit process on %s Windows", wow ? "64-bit" : "32-bit");
}

static bool ProcessElevated(HANDLE proc, bool* known) {
    *known = false;
    HANDLE tok = nullptr;
    if (!OpenProcessToken(proc, TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION e = {};
    DWORD len = 0;
    bool ok = GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &len) != 0;
    CloseHandle(tok);
    *known = ok;
    return ok && e.TokenIsElevated;
}

static void LogProcesses() {
    DWORD me = GetCurrentProcessId(), parent = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) { LOGW("diag: no process snapshot (%lu)", GetLastError()); return; }
    PROCESSENTRY32W pe = {sizeof(pe)};
    std::wstring parentName = L"(not running any more)";
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (pe.th32ProcessID == me) parent = pe.th32ParentProcessID;
    int steams = 0;
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == parent) parentName = pe.szExeFile;
        std::wstring n = pe.szExeFile;
        for (auto& c : n) c = (wchar_t)towlower(c);
        if (n == L"steam.exe" || n == L"steamservice.exe" || n == L"gameoverlayui.exe") {
            steams++;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!h) {
                LOGI("diag: %S pid %lu: cannot open it (error %lu; 5 = access denied, e.g. it runs elevated and the game does not)",
                     pe.szExeFile, pe.th32ProcessID, GetLastError());
                continue;
            }
            bool known = false, elev = ProcessElevated(h, &known);
            wchar_t path[MAX_PATH] = L"";
            DWORD len = MAX_PATH;
            QueryFullProcessImageNameW(h, 0, path, &len);
            CloseHandle(h);
            LOGI("diag: %S pid %lu, %s, %S", pe.szExeFile, pe.th32ProcessID,
                 known ? (elev ? "elevated (admin)" : "not elevated") : "elevation unknown", path);
        }
    }
    CloseHandle(snap);
    if (!steams) LOGI("diag: steam.exe is not running");
    bool known = false, elev = ProcessElevated(GetCurrentProcess(), &known);
    LOGI("diag: this game process pid %lu: %s; parent pid %lu %S", me,
         known ? (elev ? "elevated (admin)" : "not elevated") : "elevation unknown", parent, parentName.c_str());
    LOGI("diag: command line: %S", GetCommandLineW());
    const wchar_t* vars[] = {L"SteamAppId", L"SteamGameId", L"SteamClientLaunch", L"SteamEnv", L"SteamOverlayGameId"};
    for (auto v : vars) {
        wchar_t buf[256];
        DWORD n = GetEnvironmentVariableW(v, buf, 256);
        LOGI("diag: env %S = %S", v, n && n < 256 ? buf : L"(not set)");
    }
    const wchar_t* compat[] = {L"__COMPAT_LAYER"};
    for (auto v : compat) {
        wchar_t buf[512];
        DWORD n = GetEnvironmentVariableW(v, buf, 512);
        if (n && n < 512) LOGI("diag: env %S = %S (compatibility mode / run as admin set on the exe?)", v, buf);
    }
}

// Modules: logs every module not seen before; returns how many were new.
static std::set<std::wstring> g_seenModules;
static int LogModules(const char* when) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) { LOGW("diag: no module snapshot (%lu)", GetLastError()); return 0; }
    MODULEENTRY32W me = {sizeof(me)};
    int n = 0;
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        std::wstring p = me.szExePath;
        if (!g_seenModules.insert(p).second) continue;
        LOGI("diag: module %s: %S (%lu KB at %p)", when, me.szExePath, me.modBaseSize / 1024, me.modBaseAddr);
        n++;
    }
    CloseHandle(snap);
    return n;
}

// Game folder: from the exe's folder up to the folder below steamapps\common (or 3 levels up), recursively.
static int g_listed = 0;
static const int kMaxListed = 4000;
static void ListDir(const std::wstring& root, const std::wstring& rel, int depth) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + rel + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (++g_listed > kMaxListed) break;
        bool dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        ULONGLONG size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        if (dir) LOGI("diag: file  %S%S\\  (folder, %s)", rel.c_str(), n.c_str(), FileTimeStr(fd.ftLastWriteTime).c_str());
        else LOGI("diag: file  %S%S  %llu  %s%s", rel.c_str(), n.c_str(), size, FileTimeStr(fd.ftLastWriteTime).c_str(),
                  (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? "  read-only" : "");
        if (dir && depth < 12 && n != L"FF13VR") ListDir(root, rel + n + L"\\", depth + 1);
    } while (g_listed <= kMaxListed && FindNextFileW(h, &fd));
    FindClose(h);
}

static void LogGameFolder() {
    std::wstring dir = G().dllDir;   // exe folder, ends with '\'
    std::wstring root = dir, cur = dir;
    for (int up = 0; up < 8; ++up) {   // FF XIII: bin <- win <- prog <- white_data <- FINAL FANTASY XIII <- common
        std::wstring t = cur.substr(0, cur.size() - 1);
        size_t s = t.find_last_of(L'\\');
        if (s == std::wstring::npos) break;
        std::wstring parent = t.substr(0, s + 1), pl = parent;
        for (auto& c : pl) c = (wchar_t)towlower(c);
        if (pl.size() >= 8 && pl.compare(pl.size() - 8, 8, L"\\common\\") == 0) { root = cur; break; }   // cur is the game folder
        cur = parent;
    }
    LOGI("diag: game folder listing of %S (FF13VR output folder skipped):", root.c_str());
    g_listed = 0;
    ListDir(root, L"", 0);
    if (g_listed > kMaxListed) LOGI("diag: listing stopped after %d entries", kMaxListed);
}

static BOOL CALLBACK ChildText(HWND h, LPARAM) {
    wchar_t cls[64] = L"", text[512] = L"";
    GetClassNameW(h, cls, 64);
    GetWindowTextW(h, text, 512);
    if (text[0]) LOGI("diag:     child %S: \"%S\"", cls, text);
    return TRUE;
}
static std::set<HWND> g_seenWindows;
static BOOL CALLBACK OwnWindow(HWND h, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || !g_seenWindows.insert(h).second) return TRUE;
    wchar_t cls[64] = L"", title[256] = L"";
    GetClassNameW(h, cls, 64);
    GetWindowTextW(h, title, 256);
    LOGI("diag: window of this process appeared: class %S, title \"%S\"", cls, title);
    EnumChildWindows(h, ChildText, 0);
    return TRUE;
}

static DWORD WINAPI DiagThread(LPVOID) {
    LogWindowsVersion();
    LogProcesses();
    LogModules("at load");
    LogGameFolder();
    xrb::LogFlush();
    // Watch Steam's start-up check: new modules and windows (the error box) until the game's first D3D call.
    DWORD t0 = GetTickCount();
    while (!g_graphicsStarted && GetTickCount() - t0 < 60000) {
        Sleep(250);
        int n = LogModules("loaded later");
        size_t w = g_seenWindows.size();
        EnumWindows(OwnWindow, 0);
        if (n || g_seenWindows.size() != w) xrb::LogFlush();
    }
    if (g_graphicsStarted) LOGI("diag: the game started its graphics after %.1f s; start-up diagnostics done", (GetTickCount() - t0) / 1000.0);
    else LOGI("diag: no graphics after 60 s; start-up watch stopped");
    xrb::LogFlush();
    return 0;
}

void DiagStartup() {
    if (!G().s.startupDiag) return;
    LOGI("diag: startup diagnostics on (startup_diag = 0 turns them off)");
    // The thread starts when the loader lock is released, i.e. while the game's start-up (and Steam's check) runs.
    g_diagThread = CreateThread(nullptr, 0, DiagThread, nullptr, 0, nullptr);
}

void DiagGraphicsStarted() { InterlockedExchange(&g_graphicsStarted, 1); }

void DiagExit() {
    if (!G().s.startupDiag) return;
    LOGI("diag: process exit; the game %s its graphics", g_graphicsStarted ? "had started" : "had NOT started");
}
