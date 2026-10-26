// Culling FOV widening - see camfov.h and claude/culling-fov.md.
//
// Data only, never code. What the search found (2026-10-21): the game camera keeps its vertical field of view as a
// float in radians (0.6545 = 37.5 deg in the field) in a heap object, and recomputes it twice per frame; the
// projection (and the culling frustum) are derived from it. Writing it once has no effect, so:
//  1. A background thread scans the game's writable heap for floats equal to the current vertical FOV in radians
//     (derived from the projection the game sends to its shaders).
//  2. Each candidate gets a CPU data breakpoint (debug registers, "break on write") for 20 frames: right after each
//     game write the handler replaces the value with a 30% wider one. The candidate is accepted when both P00 and
//     P11 of the game's projection shrink by that factor.
//  3. The found value is then kept widened by `cull_fov` the same way. If it stops being written (the camera object
//     was replaced, e.g. a battle), the search runs again.
#include "camfov.h"
#include "stereo.h"
#include "globals.h"
#include <psapi.h>
#include <tlhelp32.h>
#include <vector>
#include <cmath>

namespace fvr {

static const float kTestScale = 1.3f;
static const long kTestFrames = 20, kSettleFrames = 5, kRefreshFrames = 300, kLostFrames = 120;

// Wider vertical FOV (radians, full angle) for a frustum `scale` times as wide (tangent space).
static float Widen(float fov, double scale) { return (float)(2 * atan(tan(fov / 2.0) * scale)); }
// The same for a value in degrees (Lightning Returns keeps its FOV in degrees; FF XIII and XIII-2 in radians).
static float WidenDeg(float fov, double scale) { return (float)(Widen((float)(fov * 3.14159265358979 / 180), scale) * 180 / 3.14159265358979); }

// ------------------------------------------------------------------ write trap (one CPU data breakpoint, DR0)
static float* volatile g_bpAddr = nullptr;
static volatile double g_bpScale = 1;
static float g_bpWritten = 0;
static volatile bool g_bpDeg = false;   // the trapped value is in degrees
static volatile LONG g_bpHits = 0;
static PVOID g_veh = nullptr;

static LONG CALLBACK BpHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(ep->ContextRecord->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    ep->ContextRecord->Dr6 = 0;
    float* a = g_bpAddr;
    if (!a) return EXCEPTION_CONTINUE_EXECUTION;
    InterlockedIncrement(&g_bpHits);
    float v = *a;
    double s = g_bpScale;
    // our own write traps too; and only plausible FOV values are touched (5..120 degrees)
    bool deg = g_bpDeg;
    if (v != g_bpWritten && s != 1 && (deg ? (v > 5.f && v < 120.f) : (v > 0.087f && v < 2.1f))) {
        float w = deg ? WidenDeg(v, s) : Widen(v, s);
        g_bpWritten = w;
        *a = w;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

// DR0/DR7 on every thread of the process (from a helper thread, so the calling thread is included).
static DWORD WINAPI BpSetter(LPVOID) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    float* a = g_bpAddr;
    DWORD dr7 = a ? (1u | (1u << 16) | (3u << 18)) : 0;   // DR0: local enable, break on write, 4 bytes
    THREADENTRY32 te{sizeof(te)};
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!th) continue;
        if (SuspendThread(th) != (DWORD)-1) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &ctx)) {
                ctx.Dr0 = (DWORD)(uintptr_t)a;
                ctx.Dr7 = (ctx.Dr7 & ~0x000F0003u) | dr7;   // leave DR1-3 alone
                ctx.Dr6 = 0;
                SetThreadContext(th, &ctx);
            }
            ResumeThread(th);
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
    return 0;
}

static void SetTrap(float* addr, double scale, bool deg) {
    if (!g_veh) g_veh = AddVectoredExceptionHandler(1, BpHandler);
    g_bpDeg = deg;
    g_bpScale = scale;
    g_bpWritten = 0;
    g_bpAddr = addr;
    HANDLE t = CreateThread(nullptr, 0, BpSetter, nullptr, 0, nullptr);
    if (t) { WaitForSingleObject(t, 2000); CloseHandle(t); }
}
static void ClearTrap() {
    if (!g_bpAddr) return;
    g_bpAddr = nullptr;
    HANDLE t = CreateThread(nullptr, 0, BpSetter, nullptr, 0, nullptr);
    if (t) { WaitForSingleObject(t, 2000); CloseHandle(t); }
}

// ------------------------------------------------------------------ background memory scan
static const size_t kMaxCands = 256;
static float* g_scanOut[kMaxCands];
static bool g_scanDeg[kMaxCands];        // candidate holds degrees (else radians)
static volatile LONG g_scanCount = 0, g_scanDone = 0;
static float g_scanTarget = 0;           // radians; the degree form is searched too
static size_t g_scanBytes = 0;

static size_t ScanRegion(const char* base, size_t size, float t, float tDeg, size_t count) {
    __try {
        const float* p = (const float*)base;
        for (size_t i = 0; i < size / 4 && count < kMaxCands; ++i)
            if (fabsf(p[i] - t) <= t * 2e-5f) { g_scanDeg[count] = false; g_scanOut[count++] = (float*)&p[i]; }
            else if (fabsf(p[i] - tDeg) <= tDeg * 2e-5f) { g_scanDeg[count] = true; g_scanOut[count++] = (float*)&p[i]; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return count;
}

// Thread stacks: locals there change meaning all the time, never trap them (a private allocation with a guard page).
static bool IsStack(const MEMORY_BASIC_INFORMATION& m) {
    MEMORY_BASIC_INFORMATION r;
    for (const char* q = (const char*)m.AllocationBase; VirtualQuery(q, &r, sizeof(r)) && r.AllocationBase == m.AllocationBase;
         q = (const char*)r.BaseAddress + r.RegionSize)
        if (r.Protect & PAGE_GUARD) return true;
    return false;
}

static DWORD WINAPI ScanThread(LPVOID) {
    t_memProbe = true;   // memory can be freed while we read it: not a crash (ScanRegion catches it)
    HMODULE exe = GetModuleHandleW(nullptr);
    size_t count = 0, bytes = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t a = 0x10000; a < 0x7FFF0000 && VirtualQuery((void*)a, &mbi, sizeof(mbi)); a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) continue;
        if (!(mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY))) continue;          // writable data only
        if (mbi.Type == MEM_IMAGE && mbi.AllocationBase != (void*)exe) continue;   // DLL data (incl. ours)
        if (mbi.Type == MEM_PRIVATE && IsStack(mbi)) continue;
        bytes += mbi.RegionSize;
        count = ScanRegion((const char*)mbi.BaseAddress, mbi.RegionSize, g_scanTarget, g_scanTarget * 57.2957795f, count);
    }
    g_scanBytes = bytes;
    g_scanCount = (LONG)count;
    InterlockedExchange(&g_scanDone, 1);
    return 0;
}

// ------------------------------------------------------------------ state machine (render thread, once per frame)
enum State { S_IDLE, S_SCANNING, S_COUNT, S_COUNT_WAIT, S_TRY, S_TRY_WAIT, S_SETTLE, S_ACTIVE };
static const long kCountFrames = 8;
static State g_state = S_IDLE;
static long g_until = 0, g_nextSearch = 0;
static std::vector<float*> g_cands;
static std::vector<bool> g_candDeg;
static std::vector<float*> g_keep;   // candidates the game writes every frame (counting pass)
static std::vector<bool> g_keepDeg;
static bool g_foundDeg = false;
static size_t g_next = 0;
static float g_baseP00 = 0, g_baseP11 = 0;
static float* g_found = nullptr;
static double g_scale = 0;           // 0 = from config (cull_fov)
static LONG g_lastHits = 0;
static long g_lastHitFrame = 0, g_refreshAt = 0;
static unsigned g_searches = 0, g_failed = 0;   // searches stop after kMaxFailed misses in a row (each one costs frame rate)
static const unsigned kMaxFailed = 3;
static float g_failTarget = 0;   // the FOV the misses were for: another camera FOV (new scene) gets new attempts

static long Frame() { return G().frame; }
static double Scale() { return g_scale > 0 ? g_scale : G().s.cullFov; }

// The game's projection of the latest frame, if it is recent (we're in the 3D scene).
static bool Projection(float* p00, float* p11) {
    long f;
    return StereoGameProjection(p00, p11, &f) && Frame() - f <= 2;
}

static std::string Where(const void* p) {
    HMODULE exe = GetModuleHandleW(nullptr);
    MODULEINFO mi{};
    GetModuleInformation(GetCurrentProcess(), exe, &mi, sizeof(mi));
    char b[64];
    uintptr_t a = (uintptr_t)p, base = (uintptr_t)mi.lpBaseOfDll;
    if (a >= base && a < base + mi.SizeOfImage) sprintf_s(b, "exe+%Ix", a - base);
    else sprintf_s(b, "heap %p", p);
    return b;
}

static void StartSearch(float p11) {
    g_scanTarget = (float)(2 * atan(1.0 / p11));
    g_scanDone = 0;
    g_scanCount = 0;
    HANDLE t = CreateThread(nullptr, 0, ScanThread, nullptr, 0, nullptr);
    if (!t) { g_nextSearch = Frame() + 600; return; }
    SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(t);
    g_searches++;
    g_state = S_SCANNING;
}

void CamFovTick() {
    double scale = Scale();
    float p00, p11;
    switch (g_state) {
    case S_IDLE: {
        // Lightning Returns: never found there, and each test widens a candidate value for a frame (a visible flash
        // if it is a camera value), so no automatic search; the script command "fovscan" still runs one
        if (G().game == GAME_LR && g_scale <= 0) {
            static bool logged = false;
            if (!logged && scale > 1.001) { logged = true; LOGI("fov: culling FOV search is off in Lightning Returns (cull_fov has no effect there)"); }
            return;
        }
        if (scale <= 1.001 || !Projection(&p00, &p11)) return;
        float target = (float)(2 * atan(1.0 / p11));
        if (g_failed && fabsf(target / g_failTarget - 1) > 0.01f) { g_failed = 0; g_nextSearch = 0; }   // a different camera FOV
        if (Frame() < g_nextSearch) return;
        StartSearch(p11);
    }
        return;
    case S_SCANNING:
        if (!g_scanDone) return;
        g_cands.assign(g_scanOut, g_scanOut + g_scanCount);
        g_candDeg.assign(g_scanDeg, g_scanDeg + g_scanCount);
        g_next = 0;
        LOGI("fov: search %u: %u candidates for %.2f deg (vertical FOV, radians or degrees) in %.0f MB", g_searches, (unsigned)g_cands.size(),
             g_scanTarget * 57.2958f, g_scanBytes / 1048576.0);
        g_keep.clear(); g_keepDeg.clear();
        g_state = S_COUNT;
        return;
    // Counting pass: a breakpoint that only counts writes (scale 1 = never modifies). The camera writes its FOV a few
    // times per frame; constants and unrelated data with the same value (42 degrees is a common number) are not
    // rewritten and are never touched by the widening test below (an earlier version corrupted d3d9 data that way).
    case S_COUNT:
        if (g_next >= g_cands.size()) {
            LOGI("fov: %u of %u candidates are written every frame", (unsigned)g_keep.size(), (unsigned)g_cands.size());
            g_cands = g_keep; g_candDeg = g_keepDeg;
            g_next = 0;
            g_state = S_TRY;
            return;
        }
        if (!Projection(&p00, &p11)) return;
        g_bpHits = 0;
        SetTrap(g_cands[g_next], 1.0, g_candDeg[g_next]);
        g_until = Frame() + kCountFrames;
        g_state = S_COUNT_WAIT;
        return;
    case S_COUNT_WAIT: {
        if (Frame() < g_until) return;
        double perFrame = g_bpHits / (double)kCountFrames;
        ClearTrap();
        if (perFrame >= 0.5 && perFrame <= 12) { g_keep.push_back(g_cands[g_next]); g_keepDeg.push_back(g_candDeg[g_next]); }
        g_next++;
        g_state = S_COUNT;
        return;
    }
    case S_TRY:
        if (g_next >= g_cands.size()) {
            g_failTarget = g_scanTarget;
            if (++g_failed >= kMaxFailed) {
                LOGW("fov: the camera FOV value was not found (%u searches); culling FOV stays off until the camera FOV changes", g_failed);
                g_nextSearch = 0x7FFFFFFF;
            } else {
                LOGW("fov: the camera FOV value was not found; trying again in 10 s");
                g_nextSearch = Frame() + 600;
            }
            g_state = S_IDLE;
            return;
        }
        if (!Projection(&p00, &p11)) return;   // menu/movie: wait
        g_baseP00 = p00; g_baseP11 = p11;
        g_bpHits = 0;
        SetTrap(g_cands[g_next], kTestScale, g_candDeg[g_next]);
        g_until = Frame() + kTestFrames;
        g_state = S_TRY_WAIT;
        return;
    case S_TRY_WAIT: {
        if (Frame() < g_until) return;
        bool have = Projection(&p00, &p11);
        LONG hits = g_bpHits;
        ClearTrap();
        bool cdeg = g_candDeg[g_next];
        float* c = g_cands[g_next++];
        // accepted when both axes shrink by the test factor (in tangent space: P = 1 / tan(half angle))
        bool ok = have && hits > 0 && fabsf(g_baseP11 / p11 / kTestScale - 1) < 0.03f && fabsf(g_baseP00 / p00 / kTestScale - 1) < 0.03f;
        if (ok) {
            g_found = c;
            g_foundDeg = cdeg;
            g_failed = 0;
            LOGI("fov: camera FOV found at %s (%.1f writes/frame): projection P00 %.4f -> %.4f, P11 %.4f -> %.4f with a %.0f%% wider value",
                 Where(c).c_str(), hits / (double)kTestFrames, g_baseP00, p00, g_baseP11, p11, (kTestScale - 1) * 100);
            SetTrap(g_found, scale, g_foundDeg);
            LOGI("fov: culling FOV x%.2f", scale);
            g_lastHits = g_bpHits;
            g_lastHitFrame = Frame();
            g_refreshAt = Frame() + kRefreshFrames;
            g_state = S_ACTIVE;
            return;
        }
        g_until = Frame() + kSettleFrames;
        g_state = S_SETTLE;
        return;
    }
    case S_SETTLE:
        if (Frame() >= g_until) g_state = S_TRY;
        return;
    case S_ACTIVE: {
        if (scale <= 1.001) { ClearTrap(); g_found = nullptr; g_state = S_IDLE; LOGI("fov: culling FOV back to the game's"); return; }
        if (g_bpScale != scale) { g_bpScale = scale; LOGI("fov: culling FOV x%.2f", scale); }
        LONG h = g_bpHits;
        if (h != g_lastHits) { g_lastHits = h; g_lastHitFrame = Frame(); }
        else if (Frame() - g_lastHitFrame > kLostFrames && Projection(&p00, &p11)) {
            // 3D scene, but our value isn't written any more: the camera object was replaced. Search again.
            LOGI("fov: camera FOV value at %s no longer used; searching again", Where(g_found).c_str());
            ClearTrap();
            g_found = nullptr;
            g_state = S_IDLE;
            g_nextSearch = Frame() + 30;
            return;
        }
        if (Frame() >= g_refreshAt) {   // threads created since then get the breakpoint too
            g_refreshAt = Frame() + kRefreshFrames;
            SetTrap(g_found, scale, g_foundDeg);
        }
        return;
    }
    }
}

void CamFovScan() {
    if (g_state == S_ACTIVE) ClearTrap();
    g_found = nullptr;
    if (g_scale <= 0 && G().s.cullFov <= 1.001) g_scale = 1.5;   // a search needs a scale to apply afterwards
    g_state = S_IDLE;
    g_nextSearch = 0;
}

void CamFovSet(float scale) {
    g_scale = scale < 1 ? 1 : scale;
    LOGI("fov: culling FOV set to x%.2f by script", g_scale);
}

void CamFovLogStatus() {
    if (g_state == S_ACTIVE)
        LOGI("fov: culling FOV x%.2f active (%s)", Scale(), Where(g_found).c_str());
}

} // namespace fvr
